// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class StatsWindowController: NSWindowController {
    private static let kUpdateSeconds: TimeInterval = 1.0

    private static var fStatsWindowInstance: StatsWindowController?
    @objc static var statsWindow: StatsWindowController {
        if let fStatsWindowInstance = fStatsWindowInstance {
            return fStatsWindowInstance
        }
        let statsWindowInstance = StatsWindowController(windowNibName: "StatsWindow")
        fStatsWindowInstance = statsWindowInstance
        return statsWindowInstance
    }

    @IBOutlet private weak var fUploadedField: NSTextField!
    @IBOutlet private weak var fUploadedAllField: NSTextField!
    @IBOutlet private weak var fDownloadedField: NSTextField!
    @IBOutlet private weak var fDownloadedAllField: NSTextField!
    @IBOutlet private weak var fRatioField: NSTextField!
    @IBOutlet private weak var fRatioAllField: NSTextField!
    @IBOutlet private weak var fTimeField: NSTextField!
    @IBOutlet private weak var fTimeAllField: NSTextField!
    @IBOutlet private weak var fNumOpenedField: NSTextField!
    @IBOutlet private weak var fUploadedLabelField: NSTextField!
    @IBOutlet private weak var fDownloadedLabelField: NSTextField!
    @IBOutlet private weak var fRatioLabelField: NSTextField!
    @IBOutlet private weak var fTimeLabelField: NSTextField!
    @IBOutlet private weak var fNumOpenedLabelField: NSTextField!
    @IBOutlet private weak var fResetButton: NSButton!
    private var fTimer: Timer?
    private var fSession: UnsafePointer<c_tr_session> = Transmission.cSession

    private static var timeFormatter: DateComponentsFormatter {
        let timeFormatter = DateComponentsFormatter()
        timeFormatter.unitsStyle = .full
        timeFormatter.maximumUnitCount = 3
        timeFormatter.allowedUnits = [.year, .month, .weekOfMonth, .day, .hour, .minute]
        return timeFormatter
    }

    override func awakeFromNib() {
        super.awakeFromNib()
        updateStats()

        let timer = Timer.scheduledTimer(timeInterval: Self.kUpdateSeconds,
                                         target: self,
                                         selector: #selector(updateStats),
                                         userInfo: nil,
                                         repeats: true)
        fTimer = timer
        RunLoop.current.add(timer, forMode: .modalPanel)
        RunLoop.current.add(timer, forMode: .eventTracking)

        window?.restorationClass = type(of: self)
        window?.identifier = NSUserInterfaceItemIdentifier("StatsWindow")

        window?.title = NSLocalizedString("Statistics", comment: "Stats window -> title")

        // disable fullscreen support
        window?.collectionBehavior = .fullScreenNone

        // set label text
        fUploadedLabelField.stringValue = NSLocalizedString("Uploaded", comment: "Stats window -> label") + ":"
        fDownloadedLabelField.stringValue = NSLocalizedString("Downloaded", comment: "Stats window -> label") + ":"
        fRatioLabelField.stringValue = NSLocalizedString("Ratio", comment: "Stats window -> label") + ":"
        fTimeLabelField.stringValue = NSLocalizedString("Running Time", comment: "Stats window -> label") + ":"
        fNumOpenedLabelField.stringValue = NSLocalizedString("Program Started", comment: "Stats window -> label") + ":"

        fResetButton.title = NSLocalizedString("Reset", comment: "Stats window -> reset button")
    }

    @IBAction private func resetStats(_ sender: Any) {
        if !UserDefaults.standard.bool(forKey: "WarningResetStats") {
            performResetStats()
            return
        }
        guard let window = window else {
            return
        }

        let alert = NSAlert()
        alert.messageText = NSLocalizedString("Are you sure you want to reset usage statistics?", comment: "Stats reset -> title")
        alert.informativeText = NSLocalizedString("This will clear the global statistics displayed by Transmission. Individual transfer statistics will not be affected.", comment: "Stats reset -> message")
        alert.alertStyle = .warning
        alert.addButton(withTitle: NSLocalizedString("Reset", comment: "Stats reset -> button"))
        alert.addButton(withTitle: NSLocalizedString("Cancel", comment: "Stats reset -> button"))
        alert.showsSuppressionButton = true

        alert.beginSheetModal(for: window) { returnCode in
            if alert.suppressionButton?.state == .on {
                UserDefaults.standard.set(false, forKey: "WarningResetStats")
            }
            if returnCode == .alertFirstButtonReturn {
                self.performResetStats()
            }
        }
    }

    override var windowFrameAutosaveName: NSWindow.FrameAutosaveName {
        get {
            return "StatsWindow"
        }
        set {}
    }

    // MARK: - Private

    @objc
    private func updateStats() {
        let statsAll = c_tr_sessionGetCumulativeStats(fSession)
        let statsSession = c_tr_sessionGetStats(fSession)

        let byteFormatter = ByteCountFormatter()
        byteFormatter.allowedUnits = .useBytes

        fUploadedField.stringValue = NSString.stringForFileSize(statsSession.uploadedBytes)
        fUploadedField.toolTip = byteFormatter.string(fromByteCount: Int64(clamping: statsSession.uploadedBytes))
        fUploadedAllField.stringValue = String(format: NSLocalizedString("%@ total", comment: "stats total"), NSString.stringForFileSize(statsAll.uploadedBytes))
        fUploadedAllField.toolTip = byteFormatter.string(fromByteCount: Int64(clamping: statsAll.uploadedBytes))

        fDownloadedField.stringValue = NSString.stringForFileSize(statsSession.downloadedBytes)
        fDownloadedField.toolTip = byteFormatter.string(fromByteCount: Int64(clamping: statsSession.downloadedBytes))
        fDownloadedAllField.stringValue = String(format: NSLocalizedString("%@ total", comment: "stats total"), NSString.stringForFileSize(statsAll.downloadedBytes))
        fDownloadedAllField.toolTip = byteFormatter.string(fromByteCount: Int64(clamping: statsAll.downloadedBytes))

        fRatioField.stringValue = NSString.stringForRatio(CGFloat(statsSession.ratio))

        let totalRatioString = statsAll.ratio != Float(C_TR_RATIO_NA) ?
        String(format: NSLocalizedString("%@ total", comment: "stats total"), NSString.stringForRatio(CGFloat(statsAll.ratio))) :
        NSLocalizedString("Total N/A", comment: "stats total")
        self.fRatioAllField.stringValue = totalRatioString

        fTimeField.stringValue = Self.timeFormatter.string(from: TimeInterval(statsSession.secondsActive)) ?? ""
        fTimeAllField.stringValue = String(format: NSLocalizedString("%@ total", comment: "stats total"),
                                           Self.timeFormatter.string(from: TimeInterval(statsAll.secondsActive)) ?? "")

        if statsAll.sessionCount == 1 {
            fNumOpenedField.stringValue = NSLocalizedString("1 time", comment: "stats window -> times opened")
        } else {
            fNumOpenedField.stringValue = String.localizedStringWithFormat(NSLocalizedString("%llu times", comment: "stats window -> times opened"), statsAll.sessionCount)
        }
    }

    private func performResetStats() {
        c_tr_sessionClearStats(fSession)
        updateStats()
    }
}

extension StatsWindowController: NSWindowRestoration {
    static func restoreWindow(withIdentifier identifier: NSUserInterfaceItemIdentifier,
                              state: NSCoder,
                              completionHandler: @escaping (NSWindow?, Error?) -> Void) {
        assert(identifier.rawValue == "StatsWindow", "Trying to restore unexpected identifier \(identifier)")

        completionHandler(StatsWindowController.statsWindow.window, nil)
    }
}

extension StatsWindowController: NSWindowDelegate {
    func windowWillClose(_ notification: Notification) {
        fTimer?.invalidate()
        fTimer = nil
        Self.fStatsWindowInstance = nil
    }
}
