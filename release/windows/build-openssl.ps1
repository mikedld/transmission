#!/usr/bin/env pwsh

$global:OpenSslVersion = '3.1.5'

$global:OpenSslDeps = @()

function global:Build-OpenSsl([string] $PrefixDir, [string] $Arch, [string] $DepsPrefixDir) {
    $Filename = "openssl-${OpenSslVersion}.tar.gz"
    $Url = "https://www.openssl.org/source/${Filename}"

    $SourceDir = Invoke-DownloadAndUnpack $Url $Filename
    $BuildDir = $SourceDir

    $ConfigName = if ($Arch -eq 'x86') { 'VC-WIN32' } else { 'VC-WIN64A' }
    $ConfigOptions = @(
        "--prefix=${PrefixDir}"
        '--api=1.1.0'
        $ConfigName
        'shared'
        'no-capieng'
        'no-comp'
        'no-deprecated'
        'no-dso'
        'no-dynamic-engine'
        'no-engine'
        'no-external-tests'
        'no-hw'
        'no-tests'
    )

    Push-Location -Path $BuildDir
    Invoke-VcEnvCommand perl Configure @ConfigOptions
    Invoke-VcEnvCommand jom install_dev
    Pop-Location
}
