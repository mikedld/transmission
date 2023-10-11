// This file Copyright © 2011-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class GlobalOptionsPopoverViewController: NSViewController {
    private let fSession: UnsafePointer<c_tr_session> = Transmission.cSession
    private let fDefaults = UserDefaults.standard

    @IBOutlet private weak var fUploadLimitField: NSTextField!
    @IBOutlet private weak var fDownloadLimitField: NSTextField!
    @IBOutlet private weak var fRatioStopField: NSTextField!
    @IBOutlet private weak var fIdleStopField: NSTextField!

    var fInitialString: String?

    init() {
        super.init(nibName: "GlobalOptionsPopover", bundle: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func awakeFromNib() {
        super.awakeFromNib()
        fUploadLimitField.integerValue = fDefaults.integer(forKey: "UploadLimit")
        fDownloadLimitField.integerValue = fDefaults.integer(forKey: "DownloadLimit")
        fRatioStopField.floatValue = fDefaults.float(forKey: "RatioLimit")
        fIdleStopField.integerValue = fDefaults.integer(forKey: "IdleLimitMinutes")

        view.setFrameSize(view.fittingSize)
    }

    @IBAction private func updatedDisplayString(_ sender: Any) {
        NotificationCenter.default.post(name: NSNotification.Name("RefreshTorrentTable"), object: nil)
    }

    @IBAction private func setDownSpeedSetting(_ sender: NSButton) {
        c_tr_sessionLimitSpeed(fSession, C_TR_DOWN, fDefaults.bool(forKey: "CheckDownload"))

        NotificationCenter.default.post(name: NSNotification.Name("SpeedLimitUpdate"), object: nil)
    }

    @IBAction private func setDownSpeedLimit(_ sender: NSTextField) {
        let limit = sender.integerValue
        fDefaults.set(limit, forKey: "DownloadLimit")
        c_tr_sessionSetSpeedLimit_KBps(fSession, C_TR_DOWN, limit)

        NotificationCenter.default.post(name: NSNotification.Name("UpdateSpeedLimitValuesOutsidePrefs"), object: nil)
        NotificationCenter.default.post(name: NSNotification.Name("SpeedLimitUpdate"), object: nil)
    }

    @IBAction private func setUpSpeedSetting(_ sender: NSButton) {
        c_tr_sessionLimitSpeed(fSession, C_TR_UP, fDefaults.bool(forKey: "CheckUpload"))

        NotificationCenter.default.post(name: NSNotification.Name("UpdateSpeedLimitValuesOutsidePrefs"), object: nil)
        NotificationCenter.default.post(name: NSNotification.Name("SpeedLimitUpdate"), object: nil)
    }

    @IBAction private func setUpSpeedLimit(_ sender: NSTextField) {
        let limit = sender.integerValue
        fDefaults.set(limit, forKey: "UploadLimit")
        c_tr_sessionSetSpeedLimit_KBps(fSession, C_TR_UP, limit)

        NotificationCenter.default.post(name: NSNotification.Name("SpeedLimitUpdate"), object: nil)
    }

    @IBAction private func setRatioStopSetting(_ sender: NSButton) {
        c_tr_sessionSetRatioLimited(fSession, fDefaults.bool(forKey: "RatioCheck"))

        // reload main table for seeding progress
        NotificationCenter.default.post(name: NSNotification.Name("UpdateUI"), object: nil)

        // reload global settings in inspector
        NotificationCenter.default.post(name: NSNotification.Name("UpdateGlobalOptions"), object: nil)
    }

    @IBAction private func setRatioStopLimit(_ sender: NSTextField) {
        let value = sender.doubleValue
        fDefaults.set(value, forKey: "RatioLimit")
        c_tr_sessionSetRatioLimit(fSession, value)

        NotificationCenter.default.post(name: NSNotification.Name("UpdateRatioStopValueOutsidePrefs"), object: nil)

        // reload main table for seeding progress
        NotificationCenter.default.post(name: NSNotification.Name("UpdateUI"), object: nil)

        // reload global settings in inspector
        NotificationCenter.default.post(name: NSNotification.Name("UpdateGlobalOptions"), object: nil)
    }

    @IBAction private func setIdleStopSetting(_ sender: NSButton) {
        c_tr_sessionSetIdleLimited(fSession, fDefaults.bool(forKey: "IdleLimitCheck"))

        // reload main table for remaining seeding time
        NotificationCenter.default.post(name: NSNotification.Name("UpdateUI"), object: nil)

        // reload global settings in inspector
        NotificationCenter.default.post(name: NSNotification.Name("UpdateGlobalOptions"), object: nil)
    }

    @IBAction private func setIdleStopLimit(_ sender: NSTextField) {
        let value = sender.intValue
        fDefaults.set(value, forKey: "IdleLimitMinutes")
        c_tr_sessionSetIdleLimit(fSession, UInt16(clamping: value))

        NotificationCenter.default.post(name: NSNotification.Name("UpdateIdleStopValueOutsidePrefs"), object: nil)

        // reload main table for remaining seeding time
        NotificationCenter.default.post(name: NSNotification.Name("UpdateUI"), object: nil)

        // reload global settings in inspector
        NotificationCenter.default.post(name: NSNotification.Name("UpdateGlobalOptions"), object: nil)
    }
}

extension GlobalOptionsPopoverViewController: NSControlTextEditingDelegate {
    func control(_ control: NSControl,
                 textShouldBeginEditing fieldEditor: NSText) -> Bool {
        fInitialString = control.stringValue
        return true
    }

    func control(_ control: NSControl,
                 didFailToFormatString string: String,
                 errorDescription error: String?) -> Bool {
        NSSound.beep()
        if let fInitialString = fInitialString {
            control.stringValue = fInitialString
            self.fInitialString = nil
        }
        return false
    }
}
