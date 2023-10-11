// This file Copyright © 2008-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class URLSheetWindowController: NSWindowController {
    @objc var urlString: String {
        return fTextField.stringValue
    }

    @IBOutlet private weak var fLabelField: NSTextField!
    @IBOutlet private weak var fTextField: NSTextField!
    @IBOutlet private weak var fOpenButton: NSButton!
    @IBOutlet private weak var fCancelButton: NSButton!

    convenience init() {
        self.init(windowNibName: "URLSheetWindow")
    }

    override func awakeFromNib() {
        super.awakeFromNib()
        fLabelField.stringValue = NSLocalizedString("Internet address of torrent file:", comment: "URL sheet label")
        fOpenButton.title = NSLocalizedString("Open", comment: "URL sheet button")
        fCancelButton.title = NSLocalizedString("Cancel", comment: "URL sheet button")

        fOpenButton.sizeToFit()
        fCancelButton.sizeToFit()

        // size the two buttons the same
        var openFrame = fOpenButton.frame
        openFrame.size.width += 10.0
        var cancelFrame = fCancelButton.frame
        cancelFrame.size.width += 10.0

        if openFrame.width > cancelFrame.width {
            cancelFrame.size.width = openFrame.width
        } else {
            openFrame.size.width = cancelFrame.width
        }

        openFrame.origin.x = window!.frame.width - openFrame.width - 20.0 + 6.0 // I don't know why the extra 6.0 is needed
        fOpenButton.frame = openFrame

        cancelFrame.origin.x = openFrame.minX - cancelFrame.width
        fCancelButton.frame = cancelFrame
    }

    @IBAction private func openURLEndSheet(_ sender: Any) {
        if let window = window {
            NSApp.endSheet(window, returnCode: 1)
        }
    }

    @IBAction private func openURLCancelEndSheet(_ sender: Any) {
        if let window = window {
            NSApp.endSheet(window, returnCode: 0)
        }
    }

    // MARK: - Private

    private func updateOpenButtonForURL(_ string: String) {
        var enable = true
        // disable when the tracker address is blank (ignoring anything before "://")
        if string.isEmpty {
            enable = false
        } else if let prefixRange = string.range(of: "://"),
                  string.endIndex == prefixRange.upperBound {
            enable = false
        }

        fOpenButton.isEnabled = enable
    }
}

extension URLSheetWindowController: NSControlTextEditingDelegate {
    func controlTextDidChange(_ obj: Notification) {
        updateOpenButtonForURL(fTextField.stringValue)
    }
}
