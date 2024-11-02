#!/usr/bin/env pwsh

Param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('DepsHash', 'Build', 'Test')]
    [string] $Mode,

    [Parameter(Mandatory=$true)]
    [ValidateSet('x86', 'x64')]
    [string] $BuildArch,

    [Parameter()]
    [ValidateSet('All', 'CoreDeps', 'Deps', 'App')]
    [string] $BuildPart = 'All',

    [Parameter()]
    [ValidateSet('None', 'All', 'Deps', 'App')]
    [string] $CCachePart = 'None',

    [Parameter()]
    [ValidateSet('5', '6')]
    [string] $UseQtVersion = '6',

    [Parameter()]
    [string] $SourceDir,

    [Parameter()]
    [string] $RootDir,

    [Parameter()]
    [string] $ScriptBaseUrl,

    [Parameter()]
    [switch] $PackDebugSyms
)

Set-StrictMode -Version '6.0'

$ErrorActionPreference = 'Stop'
$PSDefaultParameterValues['*:ErrorAction'] = $ErrorActionPreference

$ScriptDir = Split-Path -Path $PSCommandPath -Parent

if (-not $RootDir) {
    $RootDir = (Get-Item $ScriptDir).Root.Name
}

$CacheDir = Join-Path $RootDir "${BuildArch}-cache"
$TempDir = Join-Path $RootDir "${BuildArch}-temp"
$PrefixDir = Join-Path $RootDir "${BuildArch}-prefix"

function Invoke-NativeCommand() {
    $Command = $Args[0]
    $CommandArgs = @()
    if ($Args.Count -gt 1) {
        $CommandArgs = $Args[1..($Args.Count - 1)]
    }

    Write-Debug "Executing native command: $Command $CommandArgs"
    & $Command $CommandArgs
    $Result = $LastExitCode

    if ($Result -ne 0) {
        throw "$Command $CommandArgs exited with code $Result."
    }
}

function Invoke-Download([string] $Url, [string] $OutFile) {
    if (-not (Test-Path $OutFile)) {
        Write-Information "Downloading ${Url} to ${OutFile}" -InformationAction Continue
        $OldProgressPreference = $ProgressPreference
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $Url -OutFile $OutFile
        $ProgressPreference = $OldProgressPreference
    }
}

function Invoke-DownloadAndUnpack([string] $Url, [string] $Filename, [string[]] $MoreFlags = @(), [string] $ArchiveBase = '') {
    New-Item -Path $CacheDir -ItemType Directory -ErrorAction Ignore | Out-Null
    $ArchivePath = Join-Path $CacheDir $Filename
    Invoke-Download $Url $ArchivePath

    if ($ArchivePath -match '^(.+)(\.(gz|bz2|xz))$') {
        $SubArchivePath = $Matches[1]
        if (-not (Test-Path $SubArchivePath)) {
            Write-Information "Unpacking archive ${ArchivePath} to ${CacheDir}" -InformationAction Continue
            Invoke-NativeCommand 7z x -y $ArchivePath "-o${CacheDir}" | Out-Host
        }
        $ArchivePath = $SubArchivePath
    }

    if ($ArchivePath -match '^(.+)(\.(tar|zip))$') {
        if ($ArchiveBase -eq '') {
            $ArchiveBase = Split-Path -Path $Matches[1] -Leaf
        }
        $FinalArchivePath = Join-Path $TempDir $ArchiveBase

        New-Item -Path $TempDir -ItemType Directory -ErrorAction Ignore | Out-Null
        Push-Location -Path $TempDir

        Write-Information "Unpacking archive ${ArchivePath} to ${TempDir}" -InformationAction Continue
        Invoke-NativeCommand 7z x -y $ArchivePath @MoreFlags | Out-Host
        $ArchivePath = $FinalArchivePath

        Pop-Location
    } else {
        throw "Archive type is not supported: ${ArchivePath}"
    }

    return $ArchivePath
}

function Invoke-DownloadAndInclude([string] $Uri, [string] $ScriptFile) {
    Invoke-Download $Uri $ScriptFile
    . $ScriptFile
}

function Edit-TextFile([string] $PatchedFile, [string] $MatchPattern, [string] $ReplacementString) {
    (Get-Content $PatchedFile) -Replace $MatchPattern, $ReplacementString | Out-File "${PatchedFile}_new" -Encoding ascii
    Move-Item -Path "${PatchedFile}_new" -Destination $PatchedFile -Force
}

function Invoke-VcEnvCommand() {
    $VcEnvScript = Join-Path $TempDir vcenv.cmd

    if (-not (Test-Path $VcEnvScript)) {
        New-Item -Path $TempDir -ItemType Directory -ErrorAction Ignore | Out-Null
        Set-Content $VcEnvScript @"
            @pushd .
            @call "${VcVarsScript}" ${BuildArch} || exit /b 1
            @popd
            @%* 2>&1
"@
    }

    Invoke-NativeCommand $VcEnvScript @args
}

function Get-StringHash([string] $String, [string] $HashName = 'sha1')
{
    $StringBuilder = New-Object Text.StringBuilder
    [System.Security.Cryptography.HashAlgorithm]::Create($HashName).ComputeHash([System.Text.Encoding]::UTF8.GetBytes($String)) | `
        ForEach-Object { [void] $StringBuilder.Append($_.ToString('x2')) }
    return $StringBuilder.ToString()
}

function Invoke-CMakeBuildAndInstall([string] $SourceDir, [string] $BuildDir, [string[]] $ConfigOptions) {
    Invoke-VcEnvCommand cmake -S $SourceDir -B $BuildDir -G Ninja @ConfigOptions
    Invoke-VcEnvCommand cmake --build $BuildDir
    Invoke-VcEnvCommand cmake --build $BuildDir --target install
}

function Publish-CTestResults([string] $ReportXmlFilesMask) {
    if ($env:APPVEYOR_URL) {
        $CTestToJUnit = New-Object System.Xml.Xsl.XslCompiledTransform
        $CTestToJUnit.Load("https://raw.githubusercontent.com/rpavlik/jenkins-ctest-plugin/master/ctest-to-junit.xsl")
        $WebClient = New-Object System.Net.WebClient
        foreach ($ReportXmlFile in (Get-ChildItem $ReportXmlFilesMask)) {
            $CTestToJUnit.Transform($ReportXmlFile.FullName, "$($ReportXmlFile.FullName).junit.xml")
            $WebClient.UploadFile("${env:APPVEYOR_URL}/api/testresults/junit/${env:APPVEYOR_JOB_ID}", "$($ReportXmlFile.FullName).junit.xml")
        }
    }
}

function Import-Script([string] $Name) {
    $ScriptFile = Join-Path $ScriptDir "$($Name.ToLower()).ps1"
    Invoke-DownloadAndInclude "${ScriptBaseUrl}/$($Name.ToLower()).ps1" $ScriptFile
    Set-Variable -Name "$($Name -replace '\W+', '')ScriptFileHash" -Value (Get-StringHash (Get-Content -Path $ScriptFile)) -Scope Global
}

function Invoke-Build([string] $Name, [switch] $NoCache = $false, [switch] $CacheArchiveNameOnly = $false, [string[]] $MoreArguments = @()) {
    Import-Script "Build-${Name}"

    if (-not $NoCache -or $CacheArchiveNameOnly) {
        $BuildScriptFileHash = Get-Variable -Name "Build${Name}ScriptFileHash" -ValueOnly
        $BuildVersion = Get-Variable -Name "${Name}Version" -ValueOnly
        $BuildDeps = Get-Variable -Name "${Name}Deps" -ValueOnly

        $CacheArchiveDeps = @(
            "$($Name.ToLower()):${BuildScriptFileHash}"
            "toolchain:${ToolchainScriptFileHash}"
        )
        $BuildDeps | `
            ForEach-Object { $CacheArchiveDeps += "$($_.ToLower()):$(Get-Variable -Name "Build$($_)ScriptFileHash" -ValueOnly)" }
        $CacheArchiveDepsString = ($CacheArchiveDeps | Sort-Object) -join ' '
        $CacheArchiveDepsHash = Get-StringHash $CacheArchiveDepsString

        $CacheArchiveName = "$($Name.ToLower())_${BuildVersion}-vs_${VsVersion}-${BuildArch}-${CacheArchiveDepsHash}.7z"
        $CacheArchive = Join-Path $CacheDir $CacheArchiveName

        Write-Information "Cache archive: ${CacheArchiveName} (${CacheArchiveDepsString})" -InformationAction Continue
    } else {
        $CacheArchiveName = $null
        $CacheArchive = $null
    }

    if ($CacheArchiveNameOnly) {
        return $CacheArchiveName
    }

    while (-not $CacheArchive -or -not (Test-Path $CacheArchive)) {
        if ($CacheArchive -and $env:AWS_S3_BUCKET_NAME) {
            try {
                Write-Information "Downloading cache archive ${CacheArchiveName} from S3" -InformationAction Continue
                Invoke-NativeCommand aws s3 cp "s3://${env:AWS_S3_BUCKET_NAME}/windows/${CacheArchiveName}" $CacheArchive
                break
            } catch {
                Write-Warning "Cache archive ${CacheArchiveName} download from S3 failed"
            }
        }

        $Builder = (Get-Command "Build-${Name}" -CommandType Function).ScriptBlock

        $TempPrefixDir = Join-Path $TempDir "${Name}-Prefix"
        $BuilderArguments = @($TempPrefixDir, $BuildArch, $PrefixDir) + $MoreArguments
        Write-Information "Running build for ${Name}" -InformationAction Continue
        Invoke-Command -ScriptBlock $Builder -ArgumentList $BuilderArguments

        if ($CacheArchive) {
            Write-Information "Packing cache archive ${CacheArchive} from ${TempPrefixDir}" -InformationAction Continue
            Invoke-NativeCommand cmake -E chdir $TempPrefixDir 7z a -t7z -m0=lzma -mx=9 -y $CacheArchive

            if ($CacheArchive -and $env:AWS_S3_BUCKET_NAME) {
                try {
                    Write-Information "Uploading cache archive ${CacheArchiveName} to S3" -InformationAction Continue
                    Invoke-NativeCommand aws s3 cp $CacheArchive "s3://${env:AWS_S3_BUCKET_NAME}/windows/${CacheArchiveName}" `
                        --metadata "build-deps=${CacheArchiveDepsString}"
                } catch {
                    Write-Warning "Cache archive ${CacheArchiveName} upload to S3 failed"
                }
            }
        }

        break
    }

    if ($CacheArchive) {
        Write-Information "Unpacking cache archive ${CacheArchive} to ${PrefixDir}" -InformationAction Continue
        Invoke-NativeCommand 7z x -y $CacheArchive "-o${PrefixDir}"
    }
}

function Invoke-Test([string] $Name, [string[]] $MoreArguments = @()) {
    Import-Script "Build-${Name}"

    $Tester = (Get-Command "Test-${Name}" -CommandType Function).ScriptBlock

    Write-Information "Running test for ${Name}" -InformationAction Continue
    Invoke-Command -ScriptBlock $Tester -ArgumentList $MoreArguments
}

[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12

if (-not $SourceDir) {
    $SourceDir = (Get-Item $ScriptDir).Parent.Parent.FullName
}

if ($Mode -eq 'DepsHash') {
    Import-Script Toolchain

    $Names = @()

    if (@('All', 'CoreDeps', 'Deps') -contains $BuildPart) {
        $Names = $Names + @(
            Invoke-Build Zlib -CacheArchiveNameOnly
            Invoke-Build OpenSsl -CacheArchiveNameOnly
            Invoke-Build Curl -CacheArchiveNameOnly
        )
    }

    if (@('All', 'Deps') -contains $BuildPart) {
        $Names = $Names + @(
            Invoke-Build Expat -CacheArchiveNameOnly
            Invoke-Build DBus -CacheArchiveNameOnly
            Invoke-Build Qt$UseQtVersion -CacheArchiveNameOnly
        )
    }

    Write-Output (Get-StringHash ($Names -join ':'))
}

if ($Mode -eq 'Build') {
    Import-Script Toolchain

    $env:CFLAGS = $CompilerFlags -join ' '
    $env:CXXFLAGS = $CompilerFlags -join ' '
    $env:LDFLAGS = $LinkerFlags -join ' '

    if (@('All', 'Deps') -contains $CCachePart) {
        $Env:CMAKE_C_COMPILER_LAUNCHER = 'ccache'
        $Env:CMAKE_CXX_COMPILER_LAUNCHER = 'ccache'
    } else {
        $Env:CMAKE_C_COMPILER_LAUNCHER = ''
        $Env:CMAKE_CXX_COMPILER_LAUNCHER = ''
    }

    if (@('All', 'CoreDeps', 'Deps') -contains $BuildPart) {
        Invoke-Build Zlib
        Invoke-Build OpenSsl
        Invoke-Build Curl
    }

    if (@('All', 'Deps') -contains $BuildPart) {
        Invoke-Build Expat
        Invoke-Build DBus
        Invoke-Build Qt$UseQtVersion
    }

    if (@('All', 'App') -contains $CCachePart) {
        $Env:CMAKE_C_COMPILER_LAUNCHER = 'ccache'
        $Env:CMAKE_CXX_COMPILER_LAUNCHER = 'ccache'
    } else {
        $Env:CMAKE_C_COMPILER_LAUNCHER = ''
        $Env:CMAKE_CXX_COMPILER_LAUNCHER = ''
    }

    if (@('All', 'App') -contains $BuildPart) {
        Invoke-Build Transmission -NoCache -MoreArguments @($SourceDir, $SourceDir, $UseQtVersion, $PackDebugSyms.IsPresent)
    }
}

if ($Mode -eq 'Test') {
    Invoke-Test Transmission -MoreArguments @($PrefixDir, $SourceDir)
}
