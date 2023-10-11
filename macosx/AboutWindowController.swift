// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class AboutWindowController: NSWindowController {
    private static var fAboutWindowInstance: AboutWindowController?
    @objc static var aboutController: AboutWindowController {
        if let fAboutWindowInstance = fAboutWindowInstance {
            return fAboutWindowInstance
        }
        let aboutWindowInstance = AboutWindowController(windowNibName: "AboutWindow")
        self.fAboutWindowInstance = aboutWindowInstance
        return aboutWindowInstance
    }

    @IBOutlet private weak var fTextView: NSTextView!
    @IBOutlet private weak var fLicenseView: NSTextView!
    @IBOutlet private weak var fVersionField: NSTextField!
    @IBOutlet private weak var fCopyrightField: NSTextField!
    @IBOutlet private weak var fLicenseButton: NSButton!
    @IBOutlet private weak var fLicenseCloseButton: NSButton!
    @IBOutlet private weak var fLicenseSheet: NSPanel!

    override func awakeFromNib() {
        super.awakeFromNib()
        fVersionField.stringValue = LONG_VERSION_STRING

        fCopyrightField.stringValue = Bundle.main.localizedString(forKey: "NSHumanReadableCopyright", value: nil, table: "InfoPlist")

        // swiftlint:disable:next force_try
        let credits = try! NSAttributedString(url: Bundle.main.url(forResource: "Credits", withExtension: "rtf")!,
                                              options: [ .documentType: NSAttributedString.DocumentType.rtf ],
                                              documentAttributes: nil)
        fTextView.textStorage?.setAttributedString(credits)

        // size license button
        let oldButtonWidth = fLicenseButton.frame.width

        fLicenseButton.title = NSLocalizedString("License", comment: "About window -> license button")
        fLicenseButton.sizeToFit()

        var buttonFrame = fLicenseButton.frame
        buttonFrame.size.width += 10.0
        buttonFrame.origin.x -= buttonFrame.width - oldButtonWidth
        fLicenseButton.frame = buttonFrame
    }

    override func windowDidLoad() {
        window?.center()
    }

    @IBAction private func showLicense(_ sender: Any) {
        // swiftlint:disable:next force_try
        fLicenseView.string = try! String(contentsOfFile: Bundle.main.path(forResource: "COPYING", ofType: nil)!)
        fLicenseCloseButton.title = NSLocalizedString("OK", comment: "About window -> license close button")

        window?.beginSheet(fLicenseSheet, completionHandler: nil)
    }

    @IBAction private func hideLicense(_ sender: Any) {
        window?.endSheet(fLicenseSheet)
    }
}

extension AboutWindowController: NSWindowDelegate {
    func windowWillClose(_ notification: Notification) {
        Self.fAboutWindowInstance = nil
    }
}
