// This file Copyright © 2008-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import Foundation

@objc
enum BlocklistDownloadState: UInt {
    case start
    case downloading
    case processing
}

class BlocklistDownloader: NSObject {
    private static var fBLDownloader: BlocklistDownloader?

    @objc class var isRunning: Bool {
        return fBLDownloader != nil
    }

    @objc
    class func downloader() -> BlocklistDownloader {
        guard let fBLDownloader = fBLDownloader else {
            let downloader = BlocklistDownloader()
            self.fBLDownloader = downloader
            downloader.startDownload()
            return downloader
        }
        return fBLDownloader
    }

    var viewController: BlocklistDownloaderViewController? {
        didSet {
            if let viewController = viewController {
                switch fState {
                case .start:
                    viewController.setStatusStarting()
                case .downloading:
                    viewController.setStatusProgressForCurrentSize(fCurrentSize, expectedSize: fExpectedSize)
                case .processing:
                    viewController.setStatusProcessing()
                }
            }
        }
    }

    private var fUrlSession: URLSession?
    private var fCurrentSize: UInt64 = 0
    private var fExpectedSize: Int64 = 0
    private var fState: BlocklistDownloadState = .start

    @objc
    func cancelDownload() {
        viewController?.setFinished()

        fUrlSession?.invalidateAndCancel()

        BlocklistScheduler.scheduler.updateSchedule()

        Self.fBLDownloader = nil
    }

// MARK: - Private

    func startDownload() {
        fState = .start

        let urlSession = URLSession(configuration: .ephemeral, delegate: self, delegateQueue: nil)
        fUrlSession = urlSession

        BlocklistScheduler.scheduler.cancelSchedule()

        var urlString = UserDefaults.standard.string(forKey: "BlocklistURL") ?? ""
        if !urlString.isEmpty && !urlString.contains("://") {
            urlString = "https://" + urlString
        }
        let url = URL(string: urlString) ?? URL(string: ":")!// arbitrary unsupported URL fallback to call didCompleteWithError
        let task = urlSession.downloadTask(with: url)
        task.resume()
    }

    func decompressFrom(_ file: URL, to destination: URL) -> Bool {
        if untarFrom(file, to: destination) {
            return true
        }
        if unzipFrom(file, to: destination) {
            return true
        }
        if gunzipFrom(file, to: destination) {
            return true
        }
        // If it doesn't look like archive just copy it to destination
        return (try? FileManager.default.copyItem(at: file, to: destination)) != nil
    }

    func untarFrom(_ file: URL, to destination: URL) -> Bool {
        // We need to check validity of archive before listing or unpacking.
        let tarListCheck = Process()

        tarListCheck.launchPath = "/usr/bin/tar"
        tarListCheck.arguments = [ "--list", "--file", file.path ]
        tarListCheck.standardOutput = nil
        tarListCheck.standardError = nil

        do {
            try ObjC.catchException {
                tarListCheck.launch()
                tarListCheck.waitUntilExit()
            }
            if tarListCheck.terminationStatus != 0 {
                return false
            }
        } catch {
            return false
        }

        let tarList = Process()

        tarList.launchPath = "/usr/bin/tar"
        tarList.arguments = [ "--list", "--file", file.path ]

        let pipe = Pipe()
        tarList.standardOutput = pipe
        tarList.standardError = nil

        var optionalData: Data?
        do {
            try ObjC.catchException {
                tarList.launch()
                tarList.waitUntilExit()
            }
            if tarList.terminationStatus != 0 {
                return false
            }
            try ObjC.catchException {
                optionalData = pipe.fileHandleForReading.readDataToEndOfFile()
            }
        } catch {
            return false
        }

        guard let data = optionalData else {
            return false
        }

        let output = String(data: data, encoding: .utf8)

        guard let filename = output?.components(separatedBy: .newlines).first else {
            return false
        }

        // It's a directory, skip
        if filename.hasSuffix("/") {
            return false
        }

        let destinationDir = destination.deletingLastPathComponent()

        let untar = Process()
        untar.launchPath = "/usr/bin/tar"
        untar.currentDirectoryPath = destinationDir.path
        untar.arguments = [ "--extract", "--file", file.path, filename ]

        do {
            try ObjC.catchException {
                untar.launch()
                untar.waitUntilExit()
            }
            if untar.terminationStatus != 0 {
                return false
            }
        } catch {
            return false
        }

        guard let result = (destinationDir as NSURL).appendingPathComponent(filename) else {
            return false
        }

        try? FileManager.default.moveItem(at: result, to: destination)
        return true
    }

    func gunzipFrom(_ file: URL, to destination: URL) -> Bool {
        let destinationDir = destination.deletingLastPathComponent()

        let gunzip = Process()
        gunzip.launchPath = "/usr/bin/gunzip"
        gunzip.currentDirectoryPath = destinationDir.path
        gunzip.arguments = [ "--keep", "--force", file.path ]

        do {
            try ObjC.catchException {
                gunzip.launch()
                gunzip.waitUntilExit()
            }
            if gunzip.terminationStatus != 0 {
                return false
            }
        } catch {
            return false
        }

        let result = file.deletingPathExtension()

        try? FileManager.default.moveItem(at: result, to: destination)
        return true
    }

    func unzipFrom(_ file: URL, to destination: URL) -> Bool {
        let zipinfo = Process()
        zipinfo.launchPath = "/usr/bin/zipinfo"
        zipinfo.arguments = [
            "-1", /* just the filename */
            file.path /* source zip file */
        ]
        let pipe = Pipe()
        zipinfo.standardOutput = pipe
        zipinfo.standardError = nil

        var optionalData: Data?

        do {
            try ObjC.catchException {
                zipinfo.launch()
                zipinfo.waitUntilExit()
            }
            if zipinfo.terminationStatus != 0 {
                return false
            }
            try ObjC.catchException {
                optionalData = pipe.fileHandleForReading.readDataToEndOfFile()
            }
        } catch {
            return false
        }

        guard let data = optionalData else {
            return false
        }

        let output = String(data: data, encoding: .utf8)

        guard let filename = output?.components(separatedBy: .newlines).first else {
            return false
        }

        // It's a directory, skip
        if filename.hasSuffix("/") {
            return false
        }

        let destinationDir = destination.deletingLastPathComponent()

        let unzip = Process()
        unzip.launchPath = "/usr/bin/unzip"
        unzip.currentDirectoryPath = destinationDir.path
        unzip.arguments = [ file.path, filename ]

        do {
            try ObjC.catchException {
                unzip.launch()
                unzip.waitUntilExit()
            }
            if unzip.terminationStatus != 0 {
                return false
            }
        } catch {
            return false
        }

        guard let result = (destinationDir as NSURL).appendingPathComponent(filename) else {
            return false
        }

        try? FileManager.default.moveItem(at: result, to: destination)
        return true
    }
}

extension BlocklistDownloader: URLSessionDownloadDelegate {
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        DispatchQueue.main.async {
            self.fState = .downloading

            self.fCurrentSize = UInt64(bitPattern: totalBytesWritten)
            self.fExpectedSize = totalBytesExpectedToWrite

            self.viewController?.setStatusProgressForCurrentSize(self.fCurrentSize, expectedSize: self.fExpectedSize)
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        DispatchQueue.main.async {
            if let error = error {
                self.viewController?.setFailed(error.localizedDescription)
            }

            UserDefaults.standard.set(Date(), forKey: "BlocklistNewLastUpdate")
            BlocklistScheduler.scheduler.updateSchedule()

            self.fUrlSession?.finishTasksAndInvalidate()
            Self.fBLDownloader = nil
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        fState = .processing

        DispatchQueue.main.async {
            self.viewController?.setStatusProcessing()
        }

        let filename = downloadTask.response?.suggestedFilename ?? "transmission-blocklist.tmp"

        let tempFile = (NSTemporaryDirectory() as NSString).appendingPathComponent(filename)
        let blocklistFile: String

        try? FileManager.default.moveItem(atPath: location.path, toPath: tempFile)

        if downloadTask.response?.mimeType == "text/plain" {
            blocklistFile = tempFile
        } else {
            blocklistFile = (NSTemporaryDirectory() as NSString).appendingPathComponent("transmission-blocklist")
            _ = self.decompressFrom(NSURL.fileURL(withPath: tempFile), to: NSURL.fileURL(withPath: blocklistFile))
            try? FileManager.default.removeItem(atPath: tempFile)
        }

        DispatchQueue.main.async {
            let rulesCount = c_tr_blocklistSetContent(Transmission.cSession, blocklistFile)

            // delete downloaded file
            try? FileManager.default.removeItem(atPath: blocklistFile)

            if rulesCount > 0 {
                self.viewController?.setFinished()
            } else {
                self.viewController?.setFailed(NSLocalizedString("The specified blocklist file did not contain any valid rules.", comment: "blocklist fail message"))
            }

            // update last updated date for schedule
            let date = Date()
            UserDefaults.standard.set(date, forKey: "BlocklistNewLastUpdate")
            UserDefaults.standard.set(date, forKey: "BlocklistNewLastUpdateSuccess")
            BlocklistScheduler.scheduler.updateSchedule()

            NotificationCenter.default.post(name: NSNotification.Name("BlocklistUpdated"), object: nil)

            self.fUrlSession?.finishTasksAndInvalidate()
            Self.fBLDownloader = nil
        }
    }
}
