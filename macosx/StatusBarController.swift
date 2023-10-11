// This file Copyright © 2011-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

enum StatusType: Int, CustomStringConvertible {
    case totalRatio = 0
    case sessionRatio = 1
    case totalTransfer = 2
    case sessionTransfer = 3

    var description: String {
        switch self {
        case .totalRatio: return "RatioTotal"
        case .sessionRatio: return "RatioSession"
        case .totalTransfer: return "TransferTotal"
        case .sessionTransfer: return "TransferSession"
        }
    }
}

class StatusBarController: NSViewController {
    @IBOutlet private weak var fStatusButton: NSButton!
    @IBOutlet private weak var fTotalDLField: NSTextField!
    @IBOutlet private weak var fTotalULField: NSTextField!
    @IBOutlet private weak var fTotalDLImageView: NSImageView!
    @IBOutlet private weak var fTotalULImageView: NSImageView!

    private let fSession: UnsafePointer<c_tr_session> = Transmission.cSession

    private var fPreviousDownloadRate: CGFloat = -1.0
    private var fPreviousUploadRate: CGFloat = -1.0

    @objc
    init() {
        super.init(nibName: "StatusBar", bundle: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func awakeFromNib() {
        super.awakeFromNib()
        // localize menu items
        fStatusButton.menu?.item(withTag: StatusType.totalRatio.rawValue)?.title = NSLocalizedString("Total Ratio", comment: "Status Bar -> status menu")
        fStatusButton.menu?.item(withTag: StatusType.sessionRatio.rawValue)?.title = NSLocalizedString("Session Ratio", comment: "Status Bar -> status menu")
        fStatusButton.menu?.item(withTag: StatusType.totalTransfer.rawValue)?.title = NSLocalizedString("Total Transfer", comment: "Status Bar -> status menu")
        fStatusButton.menu?.item(withTag: StatusType.sessionTransfer.rawValue)?.title = NSLocalizedString("Session Transfer", comment: "Status Bar -> status menu")

        fStatusButton.cell?.backgroundStyle = .raised
        fTotalDLField.cell?.backgroundStyle = .raised
        fTotalULField.cell?.backgroundStyle = .raised
        fTotalDLImageView.cell?.backgroundStyle = .raised
        fTotalULImageView.cell?.backgroundStyle = .raised

        updateSpeedFieldsToolTips()

        // update when speed limits are changed
        NotificationCenter.default.addObserver(self,
                                               selector: #selector(updateSpeedFieldsToolTips),
                                               name: NSNotification.Name("SpeedLimitUpdate"),
                                               object: nil)
    }

    @objc
    func updateWith(download dlRate: CGFloat, upload ulRate: CGFloat) {
        // set rates
        if dlRate != fPreviousDownloadRate {
            fTotalDLField.stringValue = NSString.stringForSpeed(dlRate)
            fPreviousDownloadRate = dlRate
        }

        if ulRate != fPreviousUploadRate {
            fTotalULField.stringValue = NSString.stringForSpeed(ulRate)
            fPreviousUploadRate = ulRate
        }

        // set status button text
        let statusLabel = UserDefaults.standard.string(forKey: "StatusLabel")
        let statusString: String
        var total = statusLabel == StatusType.totalRatio.description
        if total || statusLabel == StatusType.sessionRatio.description {
            let stats = total ? c_tr_sessionGetCumulativeStats(fSession) : c_tr_sessionGetStats(fSession)

            statusString = NSLocalizedString("Ratio", comment: "status bar -> status label") + ": \(NSString.stringForRatio(CGFloat(stats.ratio)))"
        } else {
            // StatusType.totalTransfer or StatusType.sessionTransfer
            total = statusLabel == StatusType.totalTransfer.description

            let stats = total ? c_tr_sessionGetCumulativeStats(fSession) : c_tr_sessionGetStats(fSession)

            statusString = String(format: "%@: %@  %@: %@",
                                  NSLocalizedString("DL", comment: "status bar -> status label"),
                                  NSString.stringForFileSize(stats.downloadedBytes),
                                  NSLocalizedString("UL", comment: "status bar -> status label"),
                                  NSString.stringForFileSize(stats.uploadedBytes))
        }

        if fStatusButton.title != statusString {
            fStatusButton.title = statusString
        }
    }

    @IBAction private func setStatusLabel(_ sender: NSMenuItem) {
        let statusLabel = StatusType(rawValue: sender.tag)!.description

        UserDefaults.standard.set(statusLabel, forKey: "StatusLabel")

        NotificationCenter.default.post(name: NSNotification.Name("UpdateUI"), object: nil)
    }

    @objc
    func updateSpeedFieldsToolTips() {
        var uploadText: String
        var downloadText: String

        if UserDefaults.standard.bool(forKey: "SpeedLimit") {
            let speedString = String(format: "%@ (%@)",
                                     NSLocalizedString("%ld KB/s", comment: "Status Bar -> speed tooltip"),
                                     NSLocalizedString("Speed Limit", comment: "Status Bar -> speed tooltip"))
            uploadText = String(format: speedString, UserDefaults.standard.integer(forKey: "SpeedLimitUploadLimit"))
            downloadText = String(format: speedString, UserDefaults.standard.integer(forKey: "SpeedLimitDownloadLimit"))
        } else {
            if UserDefaults.standard.bool(forKey: "CheckUpload") {
                uploadText = String.localizedStringWithFormat(NSLocalizedString("%ld KB/s", comment: "Status Bar -> speed tooltip"),
                                                              UserDefaults.standard.integer(forKey: "UploadLimit"))
            } else {
                uploadText = NSLocalizedString("unlimited", comment: "Status Bar -> speed tooltip")
            }

            if UserDefaults.standard.bool(forKey: "CheckDownload") {
                downloadText = String.localizedStringWithFormat(NSLocalizedString("%ld KB/s", comment: "Status Bar -> speed tooltip"),
                                                                UserDefaults.standard.integer(forKey: "DownloadLimit"))
            } else {
                downloadText = NSLocalizedString("unlimited", comment: "Status Bar -> speed tooltip")
            }
        }

        uploadText = NSLocalizedString("Global upload limit", comment: "Status Bar -> speed tooltip") + ": \(uploadText)"
        downloadText = NSLocalizedString("Global download limit", comment: "Status Bar -> speed tooltip") + ": \(downloadText)"

        fTotalULField.toolTip = uploadText
        fTotalDLField.toolTip = downloadText
    }
}

extension StatusBarController: NSMenuItemValidation {
    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        // enable sort options
        if menuItem.action == #selector(setStatusLabel(_:)) {
            let statusLabel = StatusType(rawValue: menuItem.tag)!.description
            menuItem.state = statusLabel == UserDefaults.standard.string(forKey: "StatusLabel") ? .on : .off
            return true
        }
        return true
    }
}
