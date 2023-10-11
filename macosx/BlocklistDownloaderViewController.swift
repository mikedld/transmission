// This file Copyright © 2008-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class BlocklistDownloaderViewController: NSObject {
    @IBOutlet private var fStatusWindow: NSWindow!
    @IBOutlet private var fProgressBar: NSProgressIndicator!
    @IBOutlet private var fTextField: NSTextField!
    @IBOutlet private var fButton: NSButton!

    private var fPrefsController: PrefsController

    private static var fBLViewController: BlocklistDownloaderViewController?

    @objc
    class func downloadWithPrefsController(_ prefsController: PrefsController) {
        if fBLViewController == nil {
            let controller = BlocklistDownloaderViewController(prefsController: prefsController)
            fBLViewController = controller
            controller.startDownload()
        }
    }

    override func awakeFromNib() {
        super.awakeFromNib()
        fButton.title = NSLocalizedString("Cancel", comment: "Blocklist -> cancel button")

        let oldWidth = fButton.frame.width
        fButton.sizeToFit()
        var buttonFrame = fButton.frame
        buttonFrame.size.width += 12.0 // sizeToFit sizes a bit too small
        buttonFrame.origin.x -= buttonFrame.width - oldWidth
        fButton.frame = buttonFrame

        fProgressBar.usesThreadedAnimation = true
        fProgressBar.startAnimation(self)
    }

    @IBAction private func cancelDownload(_ sender: Any) {
        BlocklistDownloader.downloader().cancelDownload()
    }

    @objc
    func setStatusStarting() {
        fTextField.stringValue = NSLocalizedString("Connecting to site", comment: "Blocklist -> message") + NSString.ellipsis
        fProgressBar.isIndeterminate = true
    }

    @objc
    func setStatusProgressForCurrentSize(_ currentSize: UInt64, expectedSize: Int64) {
        var string = NSLocalizedString("Downloading blocklist", comment: "Blocklist -> message")
        if expectedSize != swiftNSURLResponseUnknownLength, expectedSize > 0 {
            fProgressBar.isIndeterminate = false

            let substring = NSString.stringForFilePartialSize(UInt64(currentSize), fullSize: UInt64(expectedSize))
            string += " (\(substring))"
            fProgressBar.doubleValue = Double(currentSize) / Double(expectedSize)
        } else {
            string += " (\(NSString.stringForFileSize(UInt64(currentSize))))"
        }

        fTextField.stringValue = string
    }

    @objc
    func setStatusProcessing() {
        // change to indeterminate while processing
        fProgressBar.isIndeterminate = true
        fProgressBar.startAnimation(self)

        fTextField.stringValue = NSLocalizedString("Processing blocklist", comment: "Blocklist -> message") + NSString.ellipsis
        fButton.isEnabled = false
    }

    @objc
    func setFinished() {
        fPrefsController.window?.endSheet(fStatusWindow)

        Self.fBLViewController = nil
    }

    @objc
    func setFailed(_ error: String) {
        guard let window = fPrefsController.window else {
            return
        }
        window.endSheet(fStatusWindow)

        let alert = NSAlert()
        alert.addButton(withTitle: NSLocalizedString("OK", comment: "Blocklist -> button"))
        alert.messageText = NSLocalizedString("Download of the blocklist failed.", comment: "Blocklist -> message")
        alert.alertStyle = .warning

        alert.informativeText = error

        alert.beginSheetModal(for: window) { _/*returnCode*/ in
            Self.fBLViewController = nil
        }
    }

    // MARK: - Private

    init(prefsController: PrefsController) {
        fPrefsController = prefsController
        super.init()
    }

    func startDownload() {
        // load window and show as sheet
        Bundle.main.loadNibNamed("BlocklistStatusWindow", owner: self, topLevelObjects: nil)

        let downloader = BlocklistDownloader.downloader()
        downloader.viewController = self // do before showing the sheet to ensure it doesn't slide out with placeholder text

        fPrefsController.window?.beginSheet(fStatusWindow, completionHandler: nil)
    }
}
