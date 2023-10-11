// This file Copyright © 2015-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

@objc
enum DownloadPopupIndex: Int {
    case folder = 0
    case torrent = 2
}

@objc
enum RPCIPTag: Int {
    case add = 0
    case remove = 1
}

@objc
protocol PrefsControllerGetter {
    @objc var prefsController: PrefsController { get }
}

typealias ToolbarTab = NSToolbarItem.Identifier

class PrefsController: NSWindowController {
    static let ToolbarTabGeneral = ToolbarTab("TOOLBAR_GENERAL")
    static let ToolbarTabTransfers = ToolbarTab("TOOLBAR_TRANSFERS")
    static let ToolbarTabGroups = ToolbarTab("TOOLBAR_GROUPS")
    static let ToolbarTabBandwidth = ToolbarTab("TOOLBAR_BANDWIDTH")
    static let ToolbarTabPeers = ToolbarTab("TOOLBAR_PEERS")
    static let ToolbarTabNetwork = ToolbarTab("TOOLBAR_NETWORK")
    static let ToolbarTabRemote = ToolbarTab("TOOLBAR_REMOTE")

    static let kRPCKeychainService = "Transmission:Remote"
    static let kRPCKeychainName = "Remote"

    static let kWebUIURLFormat = "http://localhost:%ld/"

    private let fSession: UnsafePointer<c_tr_session> = Transmission.cSession
    private let fDefaults = UserDefaults.standard
    private var fHasLoaded = false

    @IBOutlet private weak var fGeneralView: NSView!
    @IBOutlet private weak var fTransfersView: NSView!
    @IBOutlet private weak var fBandwidthView: NSView!
    @IBOutlet private weak var fPeersView: NSView!
    @IBOutlet private weak var fNetworkView: NSView!
    @IBOutlet private weak var fRemoteView: NSView!
    @IBOutlet private weak var fGroupsView: NSView!

    private var fInitialString: String?

    @IBOutlet private weak var fSystemPreferencesButton: NSButton!
    @IBOutlet private weak var fCheckForUpdatesLabel: NSTextField!
    @IBOutlet private weak var fCheckForUpdatesButton: NSButton!
    @IBOutlet private weak var fCheckForUpdatesBetaButton: NSButton!

    @IBOutlet private weak var fFolderPopUp: NSPopUpButton!
    @IBOutlet private weak var fIncompleteFolderPopUp: NSPopUpButton!
    @IBOutlet private weak var fImportFolderPopUp: NSPopUpButton!
    @IBOutlet private weak var fDoneScriptPopUp: NSPopUpButton!
    @IBOutlet private weak var fShowMagnetAddWindowCheck: NSButton!
    @IBOutlet private weak var fRatioStopField: NSTextField!
    @IBOutlet private weak var fIdleStopField: NSTextField!
    @IBOutlet private weak var fQueueDownloadField: NSTextField!
    @IBOutlet private weak var fQueueSeedField: NSTextField!
    @IBOutlet private weak var fStalledField: NSTextField!

    @IBOutlet private weak var fUploadField: NSTextField!
    @IBOutlet private weak var fDownloadField: NSTextField!
    @IBOutlet private weak var fSpeedLimitUploadField: NSTextField!
    @IBOutlet private weak var fSpeedLimitDownloadField: NSTextField!
    @IBOutlet private weak var fAutoSpeedDayTypePopUp: NSPopUpButton!

    @IBOutlet private weak var fPeersGlobalField: NSTextField!
    @IBOutlet private weak var fPeersTorrentField: NSTextField!
    @IBOutlet private weak var fBlocklistURLField: NSTextField!
    @IBOutlet private weak var fBlocklistMessageField: NSTextField!
    @IBOutlet private weak var fBlocklistDateField: NSTextField!
    @IBOutlet private weak var fBlocklistButton: NSButton!

    private var fPortChecker: PortChecker?
    @IBOutlet private weak var fPortField: NSTextField!
    @IBOutlet private weak var fPortStatusField: NSTextField!
    @IBOutlet private weak var fNatCheck: NSButton!
    @IBOutlet private weak var fPortStatusImage: NSImageView!
    @IBOutlet private weak var fPortStatusProgress: NSProgressIndicator!
    private var fPortStatusTimer: Timer?
    private var fPeerPort: Int = -1
    private var fNatStatus: Int64 = -1

    @IBOutlet private weak var fRPCPortField: NSTextField!
    @IBOutlet private weak var fRPCPasswordField: NSTextField!
    @IBOutlet private weak var fRPCWhitelistTable: NSTableView!
    private var fRPCWhitelistArray: [String] = []
    @IBOutlet private weak var fRPCAddRemoveControl: NSSegmentedControl!
    private var fRPCPassword: String?

    convenience init() {
        self.init(windowNibName: "PrefsWindow")
        // check for old version download location (before 1.1)
        if let choice = fDefaults.string(forKey: "DownloadChoice") {
            fDefaults.set(choice == "Constant", forKey: "DownloadLocationConstant")
            fDefaults.set(true, forKey: "DownloadAsk")

            fDefaults.removeObject(forKey: "DownloadChoice")
        }

        // check for old version blocklist (before 2.12)
        if let blocklistDate = fDefaults.object(forKey: "BlocklistLastUpdate") as? Date {
            fDefaults.set(blocklistDate, forKey: "BlocklistNewLastUpdateSuccess")
            fDefaults.set(blocklistDate, forKey: "BlocklistNewLastUpdate")
            fDefaults.removeObject(forKey: "BlocklistLastUpdate")

            let blocklistDir = (FileManager.default.urls(for: .applicationDirectory, in: .userDomainMask)[0] as NSURL).appendingPathComponent("Transmission/blocklists/")!
            try? FileManager.default.moveItem(at: (blocklistDir as NSURL).appendingPathComponent("level1.bin")!,
                                              to: (blocklistDir as NSURL).appendingPathComponent(String(cString: C_DEFAULT_BLOCKLIST_FILENAME))!)
        }

        // save a new random port
        if fDefaults.bool(forKey: "RandomPort") {
            fDefaults.set(Int(c_tr_sessionGetPeerPort(fSession)), forKey: "BindPort")
        }

        // set auto import
        if fDefaults.bool(forKey: "AutoImport"), let autoPath = fDefaults.string(forKey: "AutoImportDirectory") {
            (NSApp.delegate as? Controller)?.fileWatcherQueue.addPath((autoPath as NSString).expandingTildeInPath,
                                                                      notifyingAbout: VDKQueueNotify.aboutWrite.rawValue)
        }

        // set blocklist scheduler
        BlocklistScheduler.scheduler.updateSchedule()

        // set encryption
        setEncryptionMode(self)

        // update rpc password
        updateRPCPassword()

        // update rpc whitelist
        fRPCWhitelistArray = fDefaults.array(forKey: "RPCWhitelist") as? [String] ?? [ "127.0.0.1" ]
        updateRPCWhitelist()

        // reset old Sparkle settings from previous versions
        fDefaults.removeObject(forKey: "SUScheduledCheckInterval")
        if fDefaults.object(forKey: "CheckForUpdates") != nil {
            fDefaults.removeObject(forKey: "CheckForUpdates")
        }

        setAutoUpdateToBeta(self)
    }

    deinit {
        fPortStatusTimer?.invalidate()
        fPortChecker?.cancelProbe()
    }

    override func awakeFromNib() {
        super.awakeFromNib()
        fHasLoaded = true

        window?.restorationClass = type(of: self)
        window?.identifier = NSUserInterfaceItemIdentifier("Prefs")

        // disable fullscreen support
        window?.collectionBehavior = .fullScreenNone

        let toolbar = NSToolbar(identifier: "Preferences Toolbar")
        toolbar.delegate = self
        toolbar.allowsUserCustomization = false
        toolbar.displayMode = .iconAndLabel
        toolbar.sizeMode = .regular
        toolbar.selectedItemIdentifier = Self.ToolbarTabGeneral
        window?.toolbar = toolbar

        setWindowSize()
        window?.center()
        setPrefView(nil)

        // set special-handling of magnet link add window checkbox
        updateShowAddMagnetWindowField()

        // set download folder
        fFolderPopUp.selectItem(at: fDefaults.bool(forKey: "DownloadLocationConstant") ? DownloadPopupIndex.folder.rawValue :
                                        DownloadPopupIndex.torrent.rawValue)

        // set stop ratio
        fRatioStopField.floatValue = fDefaults.float(forKey: "RatioLimit")

        // set idle seeding minutes
        fIdleStopField.integerValue = fDefaults.integer(forKey: "IdleLimitMinutes")

        // set limits
        updateLimitFields()

        // set speed limit
        fSpeedLimitUploadField.integerValue = fDefaults.integer(forKey: "SpeedLimitUploadLimit")
        fSpeedLimitDownloadField.integerValue = fDefaults.integer(forKey: "SpeedLimitDownloadLimit")

        // set port
        fPortField.intValue = Int32(clamping: fDefaults.integer(forKey: "BindPort"))
        fNatStatus = -1

        updatePortStatus()
        fPortStatusTimer = Timer.scheduledTimer(timeInterval: 5.0,
                                                target: self,
                                                selector: #selector(updatePortStatus),
                                                userInfo: nil,
                                                repeats: true)

        // set peer connections
        fPeersGlobalField.integerValue = fDefaults.integer(forKey: "PeersTotal")
        fPeersTorrentField.integerValue = fDefaults.integer(forKey: "PeersTorrent")

        // set queue values
        fQueueDownloadField.integerValue = fDefaults.integer(forKey: "QueueDownloadNumber")
        fQueueSeedField.integerValue = fDefaults.integer(forKey: "QueueSeedNumber")
        fStalledField.integerValue = fDefaults.integer(forKey: "StalledMinutes")

        // set blocklist
        if let blocklistURL = fDefaults.string(forKey: "BlocklistURL") {
            fBlocklistURLField.stringValue = blocklistURL
        }

        updateBlocklistButton()
        updateBlocklistFields()

        NotificationCenter.default.addObserver(self, selector: #selector(updateLimitFields),
                                               name: NSNotification.Name("UpdateSpeedLimitValuesOutsidePrefs"),
                                               object: nil)

        NotificationCenter.default.addObserver(self, selector: #selector(updateRatioStopField),
                                               name: NSNotification.Name("UpdateRatioStopValueOutsidePrefs"),
                                               object: nil)

        NotificationCenter.default.addObserver(self, selector: #selector(updateLimitStopField),
                                               name: NSNotification.Name("UpdateIdleStopValueOutsidePrefs"),
                                               object: nil)

        NotificationCenter.default.addObserver(self, selector: #selector(updateBlocklistFields), name: NSNotification.Name("BlocklistUpdated"),
                                               object: nil)

        NotificationCenter.default.addObserver(self, selector: #selector(updateBlocklistURLField),
                                               name: NSControl.textDidChangeNotification,
                                               object: fBlocklistURLField)

        // set rpc port
        fRPCPortField.intValue = Int32(clamping: fDefaults.integer(forKey: "RPCPort"))

        // set rpc password
        fRPCPasswordField.stringValue = fRPCPassword ?? ""

        // set fRPCWhitelistTable column width to table width
        fRPCWhitelistTable.sizeToFit()
    }

    private func setWindowSize() {
        guard let window = window else {
            return
        }
        // set window width with localised value
        var windowRect = window.frame
        var sizeString = NSLocalizedString("PrefWindowSize", comment: "")
        if sizeString == "PrefWindowSize" {
            sizeString = "640"
        }
        windowRect.size.width = CGFloat((sizeString as NSString).floatValue)
        window.setFrame(windowRect, display: true, animate: false)
    }

    @IBAction private func setAutoUpdateToBeta(_ sender: Any) {
        // TODO: Support beta releases (if/when necessary)
        // for a beta release, always use the beta appcast
        // else use fDefaults.bool(forKey: "AutoUpdateBeta")
    }

    @IBAction private func setPort(_ sender: NSControl) {
        let port = UInt16(clamping: sender.integerValue)
        fDefaults.set(Int(port), forKey: "BindPort")
        c_tr_sessionSetPeerPort(fSession, port)

        fPeerPort = -1
        updatePortStatus()
    }

    @IBAction private func randomPort(_ sender: Any) {
        let port = Int(c_tr_sessionSetPeerPortRandom(fSession))
        fDefaults.set(port, forKey: "BindPort")
        fPortField.integerValue = port

        fPeerPort = -1
        updatePortStatus()
    }

    @IBAction private func setRandomPortOnStart(_ sender: NSButton) {
        c_tr_sessionSetPeerPortRandomOnStart(fSession, sender.state == .on)
    }

    @IBAction private func setNat(_ sender: Any) {
        c_tr_sessionSetPortForwardingEnabled(fSession, fDefaults.bool(forKey: "NatTraversal"))

        fNatStatus = -1
        updatePortStatus()
    }

    @objc
    private func updatePortStatus() {
        let fwd = c_tr_sessionGetPortForwarding(fSession)
        let port = c_tr_sessionGetPeerPort(fSession)
        let natStatusChanged = fNatStatus != fwd.rawValue
        let peerPortChanged = fPeerPort != port

        if natStatusChanged || peerPortChanged {
            fNatStatus = Int64(fwd.rawValue)
            fPeerPort = Int(port)

            fPortStatusField.stringValue = ""
            fPortStatusImage.image = nil
            fPortStatusProgress.startAnimation(self)

            fPortChecker?.cancelProbe()
            let delay = natStatusChanged || c_tr_sessionIsPortForwardingEnabled(fSession)
            fPortChecker = PortChecker(forPort: fPeerPort, delay: delay, withDelegate: self)
        }
    }

    @objc private var sounds: [String] {
        var sounds = [String]()

        let directories = NSSearchPathForDirectoriesInDomains(.allLibrariesDirectory, [.userDomainMask, .localDomainMask, .systemDomainMask], true)

        for parentDirectory in directories {
            let directory = (parentDirectory as NSString).appendingPathComponent("Sounds")

            var isDirectory: ObjCBool = false
            if FileManager.default.fileExists(atPath: directory, isDirectory: &isDirectory) && isDirectory.boolValue {
                let directoryContents = try? FileManager.default.contentsOfDirectory(atPath: directory)
                for soundWithExtension in directoryContents ?? [] {
                    let sound = (soundWithExtension as NSString).deletingPathExtension
                    if NSSound(named: sound) != nil {
                        sounds.append(sound)
                    }
                }
            }
        }

        return sounds
    }

    @IBAction private func setSound(_ sender: NSPopUpButton) {
        // play sound when selecting
        if let titleOfSelectedItem = sender.titleOfSelectedItem,
           let sound = NSSound(named: titleOfSelectedItem) {
            sound.play()
        }
    }

    @IBAction private func setUTP(_ sender: Any) {
        c_tr_sessionSetUTPEnabled(fSession, fDefaults.bool(forKey: "UTPGlobal"))
    }

    @IBAction private func setPeersGlobal(_ sender: NSControl) {
        let count = UInt16(clamping: sender.integerValue)
        fDefaults.set(Int(count), forKey: "PeersTotal")
        c_tr_sessionSetPeerLimit(fSession, count)
    }

    @IBAction private func setPeersTorrent(_ sender: NSControl) {
        let count = UInt16(clamping: sender.integerValue)
        fDefaults.set(Int(count), forKey: "PeersTorrent")
        c_tr_sessionSetPeerLimitPerTorrent(fSession, count)
    }

    @IBAction private func setPEX(_ sender: Any) {
        c_tr_sessionSetPexEnabled(fSession, fDefaults.bool(forKey: "PEXGlobal"))
    }

    @IBAction private func setDHT(_ sender: Any) {
        c_tr_sessionSetDHTEnabled(fSession, fDefaults.bool(forKey: "DHTGlobal"))
    }

    @IBAction private func setLPD(_ sender: Any) {
        c_tr_sessionSetLPDEnabled(fSession, fDefaults.bool(forKey: "LocalPeerDiscoveryGlobal"))
    }

    @IBAction private func setEncryptionMode(_ sender: Any) {
        let mode: c_tr_encryption_mode = fDefaults.bool(forKey: "EncryptionPrefer") ?
        (fDefaults.bool(forKey: "EncryptionRequire") ? C_TR_ENCRYPTION_REQUIRED : C_TR_ENCRYPTION_PREFERRED) :
        C_TR_CLEAR_PREFERRED
        c_tr_sessionSetEncryption(fSession, mode)
    }

    @IBAction private func setBlocklistEnabled(_ sender: Any) {
        c_tr_blocklistSetEnabled(fSession, fDefaults.bool(forKey: "BlocklistNew"))

        BlocklistScheduler.scheduler.updateSchedule()

        updateBlocklistButton()
    }

    @IBAction private func updateBlocklist(_ sender: Any) {
        BlocklistDownloaderViewController.downloadWithPrefsController(self)
    }

    @IBAction private func setBlocklistAutoUpdate(_ sender: Any) {
        BlocklistScheduler.scheduler.updateSchedule()
    }

    @objc
    private func updateBlocklistFields() {
        let exists = c_tr_blocklistExists(fSession)

        if exists {
            fBlocklistMessageField.stringValue = String.localizedStringWithFormat(NSLocalizedString("%lu IP address rules in list", comment: "Prefs -> blocklist -> message"), c_tr_blocklistGetRuleCount(fSession))
        } else {
            fBlocklistMessageField.stringValue = NSLocalizedString("A blocklist must first be downloaded", comment: "Prefs -> blocklist -> message")
        }

        let updatedDateString: String
        if exists {
            if let updatedDate = fDefaults.object(forKey: "BlocklistNewLastUpdateSuccess") as? Date {
                updatedDateString = DateFormatter.localizedString(from: updatedDate,
                                                                  dateStyle: .full,
                                                                  timeStyle: .short)
            } else {
                updatedDateString = NSLocalizedString("N/A", comment: "Prefs -> blocklist -> message")
            }
        } else {
            updatedDateString = NSLocalizedString("Never", comment: "Prefs -> blocklist -> message")
        }

        fBlocklistDateField.stringValue = NSLocalizedString("Last updated", comment: "Prefs -> blocklist -> message") + ": \(updatedDateString)"
    }

    @objc
    private func updateBlocklistURLField() {
       let blocklistString = fBlocklistURLField.stringValue

        fDefaults.set(blocklistString, forKey: "BlocklistURL")
        c_tr_blocklistSetURL(fSession, blocklistString)

        updateBlocklistButton()
    }

    private func updateBlocklistButton() {
        let blocklistString = fDefaults.string(forKey: "BlocklistURL")
        let enable = blocklistString?.isEmpty == false && fDefaults.bool(forKey: "BlocklistNew")
        fBlocklistButton.isEnabled = enable
    }

    @IBAction private func setAutoStartDownloads(_ sender: Any) {
        c_tr_sessionSetPaused(fSession, !fDefaults.bool(forKey: "AutoStartDownload"))
    }

    @IBAction private func applySpeedSettings(_ sender: Any) {
        c_tr_sessionLimitSpeed(fSession, C_TR_UP, fDefaults.bool(forKey: "CheckUpload"))
        c_tr_sessionSetSpeedLimit_KBps(fSession, C_TR_UP, fDefaults.integer(forKey: "UploadLimit"))

        c_tr_sessionLimitSpeed(fSession, C_TR_DOWN, fDefaults.bool(forKey: "CheckDownload"))
        c_tr_sessionSetSpeedLimit_KBps(fSession, C_TR_DOWN, fDefaults.integer(forKey: "DownloadLimit"))

        NotificationCenter.default.post(name: NSNotification.Name("SpeedLimitUpdate"), object: nil)
    }

    private func applyAltSpeedSettings() {
        c_tr_sessionSetAltSpeed_KBps(fSession, C_TR_UP, fDefaults.integer(forKey: "SpeedLimitUploadLimit"))
        c_tr_sessionSetAltSpeed_KBps(fSession, C_TR_DOWN, fDefaults.integer(forKey: "SpeedLimitDownloadLimit"))

        NotificationCenter.default.post(name: NSNotification.Name("SpeedLimitUpdate"), object: nil)
    }

    @IBAction private func applyRatioSetting(_ sender: Any) {
        c_tr_sessionSetRatioLimited(fSession, fDefaults.bool(forKey: "RatioCheck"))
        c_tr_sessionSetRatioLimit(fSession, fDefaults.double(forKey: "RatioLimit"))

        // reload main table for seeding progress
        NotificationCenter.default.post(name: NSNotification.Name("UpdateUI"), object: nil)

        // reload global settings in inspector
        NotificationCenter.default.post(name: NSNotification.Name("UpdateGlobalOptions"), object: nil)
    }

    @IBAction private func setRatioStop(_ sender: NSControl) {
        fDefaults.set(sender.floatValue, forKey: "RatioLimit")

        applyRatioSetting(sender)
    }

    @objc
    private func updateRatioStopField() {
        if fHasLoaded {
            fRatioStopField.floatValue = fDefaults.float(forKey: "RatioLimit")
        }
    }

    @IBAction private func applyIdleStopSetting(_ sender: Any) {
        c_tr_sessionSetIdleLimited(fSession, fDefaults.bool(forKey: "IdleLimitCheck"))
        c_tr_sessionSetIdleLimit(fSession, UInt16(clamping: fDefaults.integer(forKey: "IdleLimitMinutes")))

        // reload main table for remaining seeding time
        NotificationCenter.default.post(name: NSNotification.Name("UpdateUI"), object: nil)

        // reload global settings in inspector
        NotificationCenter.default.post(name: NSNotification.Name("UpdateGlobalOptions"), object: nil)
    }

    @IBAction private func setIdleStop(_ sender: NSControl) {
        fDefaults.set(sender.integerValue, forKey: "IdleLimitMinutes")

        applyIdleStopSetting(sender)
    }

    @objc
    func updateLimitStopField() {
        if fHasLoaded {
            fIdleStopField.integerValue = fDefaults.integer(forKey: "IdleLimitMinutes")
        }
    }

    @objc
    func updateLimitFields() {
        if !fHasLoaded {
            return
        }

        fUploadField.integerValue = fDefaults.integer(forKey: "UploadLimit")
        fDownloadField.integerValue = fDefaults.integer(forKey: "DownloadLimit")
    }

    @IBAction private func setGlobalLimit(_ sender: NSControl) {
        fDefaults.set(sender.integerValue, forKey: sender == fUploadField ? "UploadLimit" : "DownloadLimit")
        applySpeedSettings(self)
    }

    @IBAction private func setSpeedLimit(_ sender: NSControl) {
        fDefaults.set(sender.integerValue, forKey: sender == fSpeedLimitUploadField ? "SpeedLimitUploadLimit" : "SpeedLimitDownloadLimit")
        applyAltSpeedSettings()
    }

    @IBAction private func setAutoSpeedLimit(_ sender: NSButton) {
        c_tr_sessionUseAltSpeedTime(fSession, fDefaults.bool(forKey: "SpeedLimitAuto"))
    }

    @IBAction private func setAutoSpeedLimitTime(_ sender: NSDatePicker) {
        c_tr_sessionSetAltSpeedBegin(fSession, Self.dateToTimeSum(fDefaults.object(forKey: "SpeedLimitAutoOnDate") as? Date ?? Date()))
        c_tr_sessionSetAltSpeedEnd(fSession, Self.dateToTimeSum(fDefaults.object(forKey: "SpeedLimitAutoOffDate") as? Date ?? Date()))
    }

    @IBAction private func setAutoSpeedLimitDay(_ sender: NSPopUpButton) {
        c_tr_sessionSetAltSpeedDay(fSession, c_tr_sched_day(UInt32(clamping: sender.selectedItem?.tag ?? 0)))
    }

    /// - returns: number of minutes
    @objc
    class func dateToTimeSum(_ date: Date) -> Int {
        let calendar = NSCalendar.current
        let components = calendar.dateComponents([.hour, .minute], from: date)
        return Int(components.hour! * 60 + components.minute!)
    }

    class func timeSumToDate(_ sum: Int) -> Date? {
        let comps = DateComponents(hour: sum / 60, minute: sum % 60)
        return NSCalendar.current.date(from: comps)
    }

    @IBAction private func setBadge(_ sender: Any) {
        NotificationCenter.default.post(name: NSNotification.Name("UpdateUI"), object: self)
    }

    @IBAction private func openNotificationSystemPrefs(_ sender: NSButton) {
        NSWorkspace.shared.open(NSURL.fileURL(withPath: "/System/Library/PreferencePanes/Notifications.prefPane"))
    }

    @IBAction private func resetWarnings(_ sender: Any) {
        fDefaults.removeObject(forKey: "WarningDuplicate")
        fDefaults.removeObject(forKey: "WarningRemainingSpace")
        fDefaults.removeObject(forKey: "WarningFolderDataSameName")
        fDefaults.removeObject(forKey: "WarningResetStats")
        fDefaults.removeObject(forKey: "WarningCreatorBlankAddress")
        fDefaults.removeObject(forKey: "WarningCreatorPrivateBlankAddress")
        fDefaults.removeObject(forKey: "WarningRemoveTrackers")
        fDefaults.removeObject(forKey: "WarningInvalidOpen")
        fDefaults.removeObject(forKey: "WarningRemoveCompleted")
        fDefaults.removeObject(forKey: "WarningDonate")
        // fDefaults.removeObject(forKey: "WarningLegal")
    }

    @IBAction private func setDefaultForMagnets(_ sender: Any) {
        let bundleID = Bundle.main.bundleIdentifier!
        let result = LSSetDefaultHandlerForURLScheme("magnet" as CFString, bundleID as CFString)
        if result != noErr {
            NSLog("Failed setting default magnet link handler")
        }
    }

    @IBAction private func setQueue(_ sender: Any) {
        // let's just do both - easier that way
        c_tr_sessionSetQueueEnabled(fSession, C_TR_DOWN, fDefaults.bool(forKey: "Queue"))
        c_tr_sessionSetQueueEnabled(fSession, C_TR_UP, fDefaults.bool(forKey: "QueueSeed"))

        // handle if any transfers switch from queued to paused
        NotificationCenter.default.post(name: NSNotification.Name("UpdateQueue"), object: self)
    }

    @IBAction private func setQueueNumber(_ sender: NSControl) {
        let number = sender.integerValue
        let seed = sender == fQueueSeedField

        fDefaults.set(number, forKey: seed ? "QueueSeedNumber" : "QueueDownloadNumber")

        c_tr_sessionSetQueueSize(fSession, seed ? C_TR_UP : C_TR_DOWN, number)
    }

    @IBAction private func setStalled(_ sender: Any) {
        c_tr_sessionSetQueueStalledEnabled(fSession, fDefaults.bool(forKey: "CheckStalled"))

        // reload main table for stalled status
        NotificationCenter.default.post(name: NSNotification.Name("UpdateUI"), object: nil)
    }

    @IBAction private func setStalledMinutes(_ sender: NSControl) {
        let min = sender.intValue
        fDefaults.set(Int(min), forKey: "StalledMinutes")
        c_tr_sessionSetQueueStalledMinutes(fSession, min)

        // reload main table for stalled status
        NotificationCenter.default.post(name: NSNotification.Name("UpdateUI"), object: self)
    }

    @IBAction private func setDownloadLocation(_ sender: Any) {
        fDefaults.set(fFolderPopUp.indexOfSelectedItem == DownloadPopupIndex.folder.rawValue, forKey: "DownloadLocationConstant")
        updateShowAddMagnetWindowField()
    }

    @IBAction private func folderSheetShow(_ sender: Any) {
        guard let window = window else {
            return
        }
        let panel = NSOpenPanel()

        panel.prompt = NSLocalizedString("Select", comment: "Preferences -> Open panel prompt")
        panel.allowsMultipleSelection = false
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true

        panel.beginSheetModal(for: window) { result in
            if result == .OK {
                self.fFolderPopUp.selectItem(at: DownloadPopupIndex.folder.rawValue)

                let folder = panel.urls[0].path
                self.fDefaults.set(folder, forKey: "DownloadFolder")
                self.fDefaults.set(true, forKey: "DownloadLocationConstant")
                self.updateShowAddMagnetWindowField()

                assert(!folder.isEmpty)
                c_tr_sessionSetDownloadDir(self.fSession, (folder as NSString).fileSystemRepresentation)
            } else {
                // reset if cancelled
                self.fFolderPopUp.selectItem(at: self.fDefaults.bool(forKey: "DownloadLocationConstant") ? DownloadPopupIndex.folder.rawValue :
                                                DownloadPopupIndex.torrent.rawValue)
            }
        }
    }

    @IBAction private func incompleteFolderSheetShow(_ sender: Any) {
        guard let window = window else {
            return
        }
        let panel = NSOpenPanel()

        panel.prompt = NSLocalizedString("Select", comment: "Preferences -> Open panel prompt")
        panel.allowsMultipleSelection = false
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true

        panel.beginSheetModal(for: window) { result in
            if result == .OK {
                let folder = panel.urls[0].path
                self.fDefaults.set(folder, forKey: "IncompleteDownloadFolder")

                assert(!folder.isEmpty)
                c_tr_sessionSetIncompleteDir(self.fSession, (folder as NSString).fileSystemRepresentation)
            }
            self.fIncompleteFolderPopUp.selectItem(at: 0)
        }
    }

    @IBAction private func doneScriptSheetShow(_ sender: Any) {
        guard let window = window else {
            return
        }
        let panel = NSOpenPanel()

        panel.prompt = NSLocalizedString("Select", comment: "Preferences -> Open panel prompt")
        panel.allowsMultipleSelection = false
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.canCreateDirectories = false

        panel.beginSheetModal(for: window) { result in
            if result == .OK {
                let filePath = panel.urls[0].path

                assert(!filePath.isEmpty)

                self.fDefaults.set(filePath, forKey: "DoneScriptPath")
                c_tr_sessionSetScript(self.fSession, C_TR_SCRIPT_ON_TORRENT_DONE, (filePath as NSString).fileSystemRepresentation)

                self.fDefaults.set(true, forKey: "DoneScriptEnabled")
                c_tr_sessionSetScriptEnabled(self.fSession, C_TR_SCRIPT_ON_TORRENT_DONE, true)
            }
            self.fDoneScriptPopUp.selectItem(at: 0)
        }
    }

    @IBAction private func setUseIncompleteFolder(_ sender: Any) {
        c_tr_sessionSetIncompleteDirEnabled(fSession, fDefaults.bool(forKey: "UseIncompleteDownloadFolder"))
    }

    @IBAction private func setRenamePartialFiles(_ sender: Any) {
        c_tr_sessionSetIncompleteFileNamingEnabled(fSession, fDefaults.bool(forKey: "RenamePartialFiles"))
    }

    @IBAction private func setShowAddMagnetWindow(_ sender: Any) {
        fDefaults.set(fShowMagnetAddWindowCheck.state == NSControl.StateValue.on, forKey: "MagnetOpenAsk")
    }

    private func updateShowAddMagnetWindowField() {
        if !fDefaults.bool(forKey: "DownloadLocationConstant") {
            // always show the add window for magnet links when the download location is the same as the torrent file
            fShowMagnetAddWindowCheck.state = .on
            fShowMagnetAddWindowCheck.isEnabled = false
            fShowMagnetAddWindowCheck.toolTip = NSLocalizedString(
                "This option is not available if Default location is set to Same as torrent file.",
                comment: "Preferences -> Transfers -> Adding -> Magnet tooltip")
        } else {
            fShowMagnetAddWindowCheck.state = fDefaults.bool(forKey: "MagnetOpenAsk") ? .on : .off
            fShowMagnetAddWindowCheck.isEnabled = true
            fShowMagnetAddWindowCheck.toolTip = nil
        }
    }

    @IBAction private func setDoneScriptEnabled(_ sender: Any) {
        if fDefaults.bool(forKey: "DoneScriptEnabled") &&
            !FileManager.default.fileExists(atPath: fDefaults.string(forKey: "DoneScriptPath") ?? "") {
            // enabled is set but script file doesn't exist, so prompt for one and disable until they pick one
            fDefaults.set(false, forKey: "DoneScriptEnabled")
            doneScriptSheetShow(sender)
        }
        c_tr_sessionSetScriptEnabled(fSession, C_TR_SCRIPT_ON_TORRENT_DONE, fDefaults.bool(forKey: "DoneScriptEnabled"))
    }

    @IBAction private func setAutoImport(_ sender: Any) {
        if var path = fDefaults.string(forKey: "AutoImportDirectory") {
            let watcherQueue = (NSApp.delegate as? Controller)?.fileWatcherQueue
            if fDefaults.bool(forKey: "AutoImport") {
                path = (path as NSString).expandingTildeInPath
                watcherQueue?.addPath(path, notifyingAbout: VDKQueueNotify.aboutWrite.rawValue)
            } else {
                watcherQueue?.removeAllPaths()
            }

            NotificationCenter.default.post(name: NSNotification.Name("AutoImportSettingChange"), object: self)
        } else {
            importFolderSheetShow(sender)
        }
    }

    @IBAction private func importFolderSheetShow(_ sender: Any) {
        guard let window = window else {
            return
        }
        let panel = NSOpenPanel()

        panel.prompt = NSLocalizedString("Select", comment: "Preferences -> Open panel prompt")
        panel.allowsMultipleSelection = false
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true

        panel.beginSheetModal(for: window) { result in
            if result == .OK {
                let watcherQueue = (NSApp.delegate as? Controller)?.fileWatcherQueue
                watcherQueue?.removeAllPaths()

                let path = (panel.urls[0]).path
                self.fDefaults.set(path, forKey: "AutoImportDirectory")
                watcherQueue?.addPath((path as NSString).expandingTildeInPath, notifyingAbout: VDKQueueNotify.aboutWrite.rawValue)

                NotificationCenter.default.post(name: NSNotification.Name("AutoImportSettingChange"), object: self)
            } else {
                let path = self.fDefaults.string(forKey: "AutoImportDirectory")
                if path == nil {
                    self.fDefaults.set(false, forKey: "AutoImport")
                }
            }

            self.fImportFolderPopUp.selectItem(at: 0)
        }
    }

    @IBAction private func setAutoSize(_ sender: Any) {
        NotificationCenter.default.post(name: NSNotification.Name("AutoSizeSettingChange"), object: self)
    }

    @IBAction private func setRPCEnabled(_ sender: Any) {
        let enable = fDefaults.bool(forKey: "RPC")
        c_tr_sessionSetRPCEnabled(fSession, enable)

        setRPCWebUIDiscovery(sender)
    }

    @IBAction private func linkWebUI(_ sender: Any) {
        let url = URL(string: String(format: Self.kWebUIURLFormat, fDefaults.integer(forKey: "RPCPort")))!
        NSWorkspace.shared.open(url)
    }

    @IBAction private func setRPCAuthorize(_ sender: Any) {
        c_tr_sessionSetRPCPasswordEnabled(fSession, fDefaults.bool(forKey: "RPCAuthorize"))
    }

    @IBAction private func setRPCUsername(_ sender: NSControl) {
        c_tr_sessionSetRPCUsername(fSession, fDefaults.string(forKey: "RPCUsername") ?? "")
    }

    @IBAction private func setRPCPassword(_ sender: NSControl) {
        let password = sender.stringValue
        fRPCPassword = password
        setKeychainPassword(password)
        c_tr_sessionSetRPCPassword(fSession, password)
    }

    @IBAction private func setRPCPort(_ sender: NSControl) {
        let port = UInt16(clamping: sender.integerValue)
        fDefaults.set(Int(port), forKey: "RPCPort")
        c_tr_sessionSetRPCPort(fSession, port)

        setRPCWebUIDiscovery(sender)
    }

    @IBAction private func setRPCUseWhitelist(_ sender: Any) {
        c_tr_sessionSetRPCWhitelistEnabled(fSession, fDefaults.bool(forKey: "RPCUseWhitelist"))
    }

    @IBAction private func setRPCWebUIDiscovery(_ sender: Any) {
        if fDefaults.bool(forKey: "RPC") && fDefaults.bool(forKey: "RPCWebDiscovery") {
            BonjourController.defaultController.startWithPort(Int32(clamping: fDefaults.integer(forKey: "RPCPort")))
        } else {
            BonjourController.defaultController.stop()
        }
    }

    @IBAction private func addRemoveRPCIP(_ sender: NSSegmentedControl) {
        // don't allow add/remove when currently adding - it leads to weird results
        if fRPCWhitelistTable.editedRow != -1 {
            return
        }

        if (sender.cell as? NSSegmentedCell)?.tag(forSegment: sender.selectedSegment) == RPCIPTag.remove.rawValue {
            fRPCWhitelistTable.selectedRowIndexes.reversed().forEach { fRPCWhitelistArray.remove(at: $0) }
            fRPCWhitelistTable.deselectAll(self)
            fRPCWhitelistTable.reloadData()

            fDefaults.set(fRPCWhitelistArray, forKey: "RPCWhitelist")
            updateRPCWhitelist()
        } else {
            fRPCWhitelistArray.append("")
            fRPCWhitelistTable.reloadData()

            let row = fRPCWhitelistArray.count - 1
            fRPCWhitelistTable.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
            fRPCWhitelistTable.editColumn(0, row: row, with: nil, select: true)
        }
    }

    @IBAction private func helpForScript(_ sender: Any) {
        NSHelpManager.shared.openHelpAnchor("script",
                                            inBook: Bundle.main.object(forInfoDictionaryKey: "CFBundleHelpBookName") as? NSHelpManager.BookName)
    }

    @IBAction private func helpForPeers(_ sender: Any) {
        NSHelpManager.shared.openHelpAnchor("peers",
                                            inBook: Bundle.main.object(forInfoDictionaryKey: "CFBundleHelpBookName") as? NSHelpManager.BookName)
    }

    @IBAction private func helpForNetwork(_ sender: Any) {
        NSHelpManager.shared.openHelpAnchor("network",
                                            inBook: Bundle.main.object( forInfoDictionaryKey: "CFBundleHelpBookName") as? NSHelpManager.BookName)
    }

    @IBAction private func helpForRemote(_ sender: Any) {
        NSHelpManager.shared.openHelpAnchor("remote",
                                            inBook: Bundle.main.object( forInfoDictionaryKey: "CFBundleHelpBookName") as? NSHelpManager.BookName)
    }

    @objc
    func rpcUpdatePrefs() {
        // encryption
        let encryptionMode = c_tr_sessionGetEncryption(fSession)
        fDefaults.set(encryptionMode != C_TR_CLEAR_PREFERRED, forKey: "EncryptionPrefer")
        fDefaults.set(encryptionMode == C_TR_ENCRYPTION_REQUIRED, forKey: "EncryptionRequire")

        // download directory
        let downloadLocation = (String(cString: c_tr_sessionGetDownloadDir(fSession)) as NSString).standardizingPath
        fDefaults.set(downloadLocation, forKey: "DownloadFolder")

        let incompleteLocation = (String(cString: c_tr_sessionGetIncompleteDir(fSession)) as NSString).standardizingPath
        fDefaults.set(incompleteLocation, forKey: "IncompleteDownloadFolder")

        let useIncomplete = c_tr_sessionIsIncompleteDirEnabled(fSession)
        fDefaults.set(useIncomplete, forKey: "UseIncompleteDownloadFolder")

        let usePartialFileRenaming = c_tr_sessionIsIncompleteFileNamingEnabled(fSession)
        fDefaults.set(usePartialFileRenaming, forKey: "RenamePartialFiles")

        // utp
        let utp = c_tr_sessionIsUTPEnabled(fSession)
        fDefaults.set(utp, forKey: "UTPGlobal")

        // peers
        let peersTotal = c_tr_sessionGetPeerLimit(fSession)
        fDefaults.set(peersTotal, forKey: "PeersTotal")

        let peersTorrent = c_tr_sessionGetPeerLimitPerTorrent(fSession)
        fDefaults.set(peersTorrent, forKey: "PeersTorrent")

        // pex
        let pex = c_tr_sessionIsPexEnabled(fSession)
        fDefaults.set(pex, forKey: "PEXGlobal")

        // dht
        let dht = c_tr_sessionIsDHTEnabled(fSession)
        fDefaults.set(dht, forKey: "DHTGlobal")

        // lpd
        let lpd = c_tr_sessionIsLPDEnabled(fSession)
        fDefaults.set(lpd, forKey: "LocalPeerDiscoveryGlobal")

        // auto start
        let autoStart = !c_tr_sessionGetPaused(fSession)
        fDefaults.set(autoStart, forKey: "AutoStartDownload")

        // port
        let port = c_tr_sessionGetPeerPort(fSession)
        fDefaults.set(Int(port), forKey: "BindPort")

        let nat = c_tr_sessionIsPortForwardingEnabled(fSession)
        fDefaults.set(nat, forKey: "NatTraversal")

        fPeerPort = -1
        fNatStatus = -1
        updatePortStatus()

        let randomPort = c_tr_sessionGetPeerPortRandomOnStart(fSession)
        fDefaults.set(randomPort, forKey: "RandomPort")

        // speed limit - down
        let downLimitEnabled = c_tr_sessionIsSpeedLimited(fSession, C_TR_DOWN)
        fDefaults.set(downLimitEnabled, forKey: "CheckDownload")

        let downLimit = c_tr_sessionGetSpeedLimit_KBps(fSession, C_TR_DOWN)
        fDefaults.set(downLimit, forKey: "DownloadLimit")

        // speed limit - up
        let upLimitEnabled = c_tr_sessionIsSpeedLimited(fSession, C_TR_UP)
        fDefaults.set(upLimitEnabled, forKey: "CheckUpload")

        let upLimit = c_tr_sessionGetSpeedLimit_KBps(fSession, C_TR_UP)
        fDefaults.set(upLimit, forKey: "UploadLimit")

        // alt speed limit enabled
        let useAltSpeed = c_tr_sessionUsesAltSpeed(fSession)
        fDefaults.set(useAltSpeed, forKey: "SpeedLimit")

        // alt speed limit - down
        let downLimitAlt = c_tr_sessionGetAltSpeed_KBps(fSession, C_TR_DOWN)
        fDefaults.set(downLimitAlt, forKey: "SpeedLimitDownloadLimit")

        // alt speed limit - up
        let upLimitAlt = c_tr_sessionGetAltSpeed_KBps(fSession, C_TR_UP)
        fDefaults.set(upLimitAlt, forKey: "SpeedLimitUploadLimit")

        // alt speed limit schedule
        let useAltSpeedSched = c_tr_sessionUsesAltSpeedTime(fSession)
        fDefaults.set(useAltSpeedSched, forKey: "SpeedLimitAuto")

        let limitStartDate = Self.timeSumToDate(c_tr_sessionGetAltSpeedBegin(fSession))
        fDefaults.set(limitStartDate, forKey: "SpeedLimitAutoOnDate")

        let limitEndDate = Self.timeSumToDate(c_tr_sessionGetAltSpeedEnd(fSession))
        fDefaults.set(limitEndDate, forKey: "SpeedLimitAutoOffDate")

        let limitDay = c_tr_sessionGetAltSpeedDay(fSession)
        fDefaults.set(Int(clamping: limitDay.rawValue), forKey: "SpeedLimitAutoDay")

        // blocklist
        let blocklist = c_tr_blocklistIsEnabled(fSession)
        fDefaults.set(blocklist, forKey: "BlocklistNew")

        let blocklistURL = String(cString: c_tr_blocklistGetURL(fSession))
        fDefaults.set(blocklistURL, forKey: "BlocklistURL")

        // seed ratio
        let ratioLimited = c_tr_sessionIsRatioLimited(fSession)
        fDefaults.set(ratioLimited, forKey: "RatioCheck")

        let ratioLimit = c_tr_sessionGetRatioLimit(fSession)
        fDefaults.set(ratioLimit, forKey: "RatioLimit")

        // idle seed limit
        let idleLimited = c_tr_sessionIsIdleLimited(fSession)
        fDefaults.set(idleLimited, forKey: "IdleLimitCheck")

        let idleLimitMin = c_tr_sessionGetIdleLimit(fSession)
        fDefaults.set(Int(idleLimitMin), forKey: "IdleLimitMinutes")

        // queue
        let downloadQueue = c_tr_sessionGetQueueEnabled(fSession, C_TR_DOWN)
        fDefaults.set(downloadQueue, forKey: "Queue")

        let downloadQueueNum = c_tr_sessionGetQueueSize(fSession, C_TR_DOWN)
        fDefaults.set(downloadQueueNum, forKey: "QueueDownloadNumber")

        let seedQueue = c_tr_sessionGetQueueEnabled(fSession, C_TR_UP)
        fDefaults.set(seedQueue, forKey: "QueueSeed")

        let seedQueueNum = c_tr_sessionGetQueueSize(fSession, C_TR_UP)
        fDefaults.set(seedQueueNum, forKey: "QueueSeedNumber")

        let checkStalled = c_tr_sessionGetQueueStalledEnabled(fSession)
        fDefaults.set(checkStalled, forKey: "CheckStalled")

        let stalledMinutes = c_tr_sessionGetQueueStalledMinutes(fSession)
        fDefaults.set(stalledMinutes, forKey: "StalledMinutes")

        // done script
        let doneScriptEnabled = c_tr_sessionIsScriptEnabled(fSession, C_TR_SCRIPT_ON_TORRENT_DONE)
        fDefaults.set(doneScriptEnabled, forKey: "DoneScriptEnabled")

        let doneScriptPath = String(cString: c_tr_sessionGetScript(fSession, C_TR_SCRIPT_ON_TORRENT_DONE))
        fDefaults.set(doneScriptPath, forKey: "DoneScriptPath")

        // update gui if loaded
        if fHasLoaded {
            // encryption handled by bindings

            // download directory handled by bindings

            // utp handled by bindings

            fPeersGlobalField.intValue = Int32(peersTotal)
            fPeersTorrentField.intValue = Int32(peersTorrent)

            // pex handled by bindings

            // dht handled by bindings

            // lpd handled by bindings

            fPortField.intValue = Int32(port)
            // port forwarding (nat) handled by bindings
            // random port handled by bindings

            // limit check handled by bindings
            fDownloadField.integerValue = downLimit

            // limit check handled by bindings
            fUploadField.integerValue = upLimit

            fSpeedLimitDownloadField.integerValue = downLimitAlt

            fSpeedLimitUploadField.integerValue = upLimitAlt

            // speed limit schedule handled by bindings

            // speed limit schedule times and day handled by bindings

            fBlocklistURLField.stringValue = blocklistURL
            updateBlocklistButton()
            updateBlocklistFields()

            // ratio limit enabled handled by bindings
            fRatioStopField.doubleValue = ratioLimit

            // idle limit enabled handled by bindings
            fIdleStopField.integerValue = Int(idleLimitMin)

            // queues enabled handled by bindings
            fQueueDownloadField.integerValue = downloadQueueNum
            fQueueSeedField.integerValue = seedQueueNum

            // check stalled handled by bindings
            fStalledField.integerValue = stalledMinutes
        }

        NotificationCenter.default.post(name: NSNotification.Name("SpeedLimitUpdate"), object: nil)

        // reload global settings in inspector
        NotificationCenter.default.post(name: NSNotification.Name("UpdateGlobalOptions"), object: nil)
    }

// MARK: - Private

    private func viewForIdentifier(_ identifier: ToolbarTab) -> NSView {
        let view: NSView
        if identifier == Self.ToolbarTabTransfers {
            view = fTransfersView
        } else if identifier == Self.ToolbarTabGroups {
            view = fGroupsView
        } else if identifier == Self.ToolbarTabBandwidth {
            view = fBandwidthView
        } else if identifier == Self.ToolbarTabPeers {
            view = fPeersView
        } else if identifier == Self.ToolbarTabNetwork {
            view = fNetworkView
        } else if identifier == Self.ToolbarTabRemote {
            view = fRemoteView
        } else {
            view = fGeneralView
        }
        return view
    }

    @objc
    private func setPrefView(_ sender: NSToolbarItem?) {
        guard let window = window else {
            return
        }

        var identifier: ToolbarTab
        if let sender = sender {
            identifier = sender.itemIdentifier
            UserDefaults.standard.set(identifier.rawValue, forKey: "SelectedPrefView")
        } else {
            identifier = ToolbarTab(UserDefaults.standard.string(forKey: "SelectedPrefView") ?? "")
        }

        let view = viewForIdentifier(identifier)
        if view == fGeneralView {
            identifier = Self.ToolbarTabGeneral // general view is the default selected
        }

        window.toolbar?.selectedItemIdentifier = identifier

        if window.contentView == view {
            return
        }

        var windowRect = window.frame
        let difference = view.frame.height - (window.contentView?.frame.height ?? 0)
        windowRect.origin.y -= difference
        windowRect.size.height += difference

        view.isHidden = true
        window.contentView = view

        NSAnimationContext.runAnimationGroup({ context in
            context.allowsImplicitAnimation = true
            window.setFrame(windowRect, display: true)
        }, completionHandler: {
            view.isHidden = false
        })

        // set title label
        if let sender = sender {
            window.title = sender.label
        } else if let toolbar = window.toolbar {
            let itemIdentifier = toolbar.selectedItemIdentifier
            if let item = toolbar.items.first(where: { $0.itemIdentifier == itemIdentifier }) {
                window.title = item.label
            }
        }
    }

    private static func getOSStatusDescription(_ errorCode: OSStatus) -> String {
        return NSError(domain: NSOSStatusErrorDomain, code: Int(errorCode), userInfo: nil).description
    }

    private func updateRPCPassword() {
        var data: CFTypeRef?
        let result = SecItemCopyMatching(
            [
                kSecClass: kSecClassGenericPassword,
                kSecAttrAccount: Self.kRPCKeychainName,
                kSecAttrService: Self.kRPCKeychainService,
                kSecReturnData: true,
            ] as CFDictionary,
            &data)
        if result != noErr && result != errSecItemNotFound {
            NSLog("Problem accessing Keychain: %@", Self.getOSStatusDescription(result))
        }
        if let passwordData = data as? Data,
           let password = String(data: passwordData, encoding: .utf8) {
            c_tr_sessionSetRPCPassword(fSession, password)
            fRPCPassword = password
        }
    }

    private func setKeychainPassword(_ password: String) {
        var item: CFTypeRef?
        var result = SecItemCopyMatching(
            [
                kSecClass: kSecClassGenericPassword,
                kSecAttrAccount: Self.kRPCKeychainName,
                kSecAttrService: Self.kRPCKeychainService,
            ] as CFDictionary,
            &item)
        if result != noErr && result != errSecItemNotFound {
            NSLog("Problem accessing Keychain: %@", Self.getOSStatusDescription(result))
            return
        }

        let passwordLength = strlen(password)
        if item != nil {
            if passwordLength > 0 { // found and needed, so update it
                result = SecItemUpdate(
                    [
                        kSecClass: kSecClassGenericPassword,
                        kSecAttrAccount: Self.kRPCKeychainName,
                        kSecAttrService: Self.kRPCKeychainService,
                    ] as CFDictionary,
                    [
                        kSecValueData: Data(bytes: password, count: passwordLength),
                    ] as CFDictionary)
                if result != noErr {
                    NSLog("Problem updating Keychain item: %@", Self.getOSStatusDescription(result))
                }
            } else // found and not needed, so remove it
            {
                result = SecItemDelete([
                    kSecClass: kSecClassGenericPassword,
                    kSecAttrAccount: Self.kRPCKeychainName,
                    kSecAttrService: Self.kRPCKeychainService,
                ] as CFDictionary)
                if result != noErr {
                    NSLog("Problem removing Keychain item: %@", Self.getOSStatusDescription(result))
                }
            }
        } else if result == errSecItemNotFound {
            if passwordLength > 0 { // not found and needed, so add it
                result = SecItemAdd(
                    [
                        kSecClass: kSecClassGenericPassword,
                        kSecAttrAccount: Self.kRPCKeychainName,
                        kSecAttrService: Self.kRPCKeychainService,
                        kSecValueData: Data(bytes: password, count: passwordLength),
                    ] as CFDictionary,
                    nil)
                if result != noErr {
                    NSLog("Problem adding Keychain item: %@", Self.getOSStatusDescription(result))
                }
            }
        }
    }

    private func updateRPCWhitelist() {
        let string = fRPCWhitelistArray.joined(separator: ",")
        c_tr_sessionSetRPCWhitelist(fSession, string)
    }
}

extension PrefsController: NSToolbarDelegate {
    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        return [
            Self.ToolbarTabGeneral,
            Self.ToolbarTabTransfers,
            Self.ToolbarTabGroups,
            Self.ToolbarTabBandwidth,
            Self.ToolbarTabPeers,
            Self.ToolbarTabNetwork,
            Self.ToolbarTabRemote,
        ]
    }

    func toolbarSelectableItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        return toolbarAllowedItemIdentifiers(toolbar)
    }

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        return toolbarAllowedItemIdentifiers(toolbar)
    }

    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier itemIdentifier: NSToolbarItem.Identifier, willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        let item = NSToolbarItem(itemIdentifier: itemIdentifier)

        if itemIdentifier == Self.ToolbarTabGeneral {
            item.label = NSLocalizedString("General", comment: "Preferences -> toolbar item title")
            item.image = NSImage.systemSymbol("gearshape", withFallback: NSImage.preferencesGeneralName)
            item.target = self
            item.action = #selector(setPrefView(_:))
            item.autovalidates = false
        } else if itemIdentifier == Self.ToolbarTabTransfers {
            item.label = NSLocalizedString("Transfers", comment: "Preferences -> toolbar item title")
            item.image = NSImage.systemSymbol("arrow.up.arrow.down", withFallback: "Transfers")
            item.target = self
            item.action = #selector(setPrefView(_:))
            item.autovalidates = false
        } else if itemIdentifier == Self.ToolbarTabGroups {
            item.label = NSLocalizedString("Groups", comment: "Preferences -> toolbar item title")
            item.image = NSImage.systemSymbol("pin", withFallback: "Groups")
            item.target = self
            item.action = #selector(setPrefView(_:))
            item.autovalidates = false
        } else if itemIdentifier == Self.ToolbarTabBandwidth {
            item.label = NSLocalizedString("Bandwidth", comment: "Preferences -> toolbar item title")
            item.image = NSImage.systemSymbol("speedometer", withFallback: "Bandwidth")
            item.target = self
            item.action = #selector(setPrefView(_:))
            item.autovalidates = false
        } else if itemIdentifier == Self.ToolbarTabPeers {
            item.label = NSLocalizedString("Peers", comment: "Preferences -> toolbar item title")
            item.image = NSImage.systemSymbol("person.2", withFallback: NSImage.userGroupName)
            item.target = self
            item.action = #selector(setPrefView(_:))
            item.autovalidates = false
        } else if itemIdentifier == Self.ToolbarTabNetwork {
            item.label = NSLocalizedString("Network", comment: "Preferences -> toolbar item title")
            item.image = NSImage.systemSymbol("network", withFallback: NSImage.networkName)
            item.target = self
            item.action = #selector(setPrefView(_:))
            item.autovalidates = false
        } else if itemIdentifier == Self.ToolbarTabRemote {
            item.label = NSLocalizedString("Remote", comment: "Preferences -> toolbar item title")
            item.image = NSImage.systemSymbol("antenna.radiowaves.left.and.right", withFallback: "Remote")
            item.target = self
            item.action = #selector(setPrefView(_:))
            item.autovalidates = false
        } else {
            return nil
        }

        return item
    }
}

extension PrefsController: NSWindowRestoration {
    static func restoreWindow(withIdentifier identifier: NSUserInterfaceItemIdentifier,
                              state: NSCoder,
                              completionHandler: @escaping (NSWindow?, Error?) -> Void) {
        let window = (NSApp.delegate as? PrefsControllerGetter)?.prefsController.window
        completionHandler(window, nil)
    }
}

extension PrefsController: NSTableViewDataSource {
    func numberOfRows(in tableView: NSTableView) -> Int {
        return fRPCWhitelistArray.count
    }

    func tableView(_ tableView: NSTableView, objectValueFor tableColumn: NSTableColumn?, row: Int) -> Any? {
        return fRPCWhitelistArray[row]
    }

    func tableView(_ tableView: NSTableView, setObjectValue object: Any?, for tableColumn: NSTableColumn?, row: Int) {
        let components = (object as? NSString)?.components(separatedBy: ".")
        var newComponents = [String]()

        // create better-formatted ip string
        var valid = false
        if let components = components, components.count == 4 {
            valid = true
            for component in components {
                if component == "*" {
                    newComponents.append(component)
                } else {
                    let num = (component as NSString).intValue
                    if num >= 0 && num < 256 {
                        newComponents.append((num as NSNumber).stringValue)
                    } else {
                        valid = false
                        break
                    }
                }
            }
        }

        if valid {
            let newIP = newComponents.joined(separator: ".")

            // don't allow the same ip address
            if fRPCWhitelistArray.contains(newIP) && fRPCWhitelistArray[row] != newIP {
                valid = false
            } else {
                fRPCWhitelistArray[row] = newIP
                fRPCWhitelistArray.sort {
                    return ($0 as NSString).compareNumeric($1) == .orderedAscending
                }
            }
        }

        if !valid {
            NSSound.beep()
            if fRPCWhitelistArray[row].isEmpty {
                fRPCWhitelistArray.remove(at: row)
            }
        }

        fRPCWhitelistTable.deselectAll(self)
        fRPCWhitelistTable.reloadData()

        fDefaults.set(fRPCWhitelistArray, forKey: "RPCWhitelist")
        updateRPCWhitelist()
    }
}

extension PrefsController: NSTableViewDelegate {
    func tableViewSelectionDidChange(_ notification: Notification) {
        fRPCAddRemoveControl.setEnabled(fRPCWhitelistTable.numberOfSelectedRows > 0, forSegment: RPCIPTag.remove.rawValue)
    }
}

extension PrefsController: NSControlTextEditingDelegate {
    func control(_ control: NSControl, textShouldBeginEditing fieldEditor: NSText) -> Bool {
        fInitialString = control.stringValue
        return true
    }

    func control(_ control: NSControl, didFailToFormatString string: String, errorDescription error: String?) -> Bool {
        NSSound.beep()
        if let fInitialString = fInitialString {
            control.stringValue = fInitialString
            self.fInitialString = nil
        }
        return false
    }
}

extension PrefsController: PortCheckerDelegate {
    func portCheckerDidFinishProbing(_ portChecker: PortChecker) {
        fPortStatusProgress.stopAnimation(self)
        switch portChecker.status {
        case .open:
            fPortStatusField.stringValue = NSLocalizedString("Port is open", comment: "Preferences -> Network -> port status")
            fPortStatusImage.image = NSImage(named: NSImage.statusAvailableName)
        case .closed:
            fPortStatusField.stringValue = NSLocalizedString("Port is closed", comment: "Preferences -> Network -> port status")
            fPortStatusImage.image = NSImage(named: NSImage.statusUnavailableName)
        case .error:
            fPortStatusField.stringValue = NSLocalizedString("Port check site is down", comment: "Preferences -> Network -> port status")
            fPortStatusImage.image = NSImage(named: NSImage.statusPartiallyAvailableName)
        case .checking:
            break
        default:
            assert(false, "Port checker returned invalid status: \(portChecker.status)")
        }
        fPortChecker = nil
    }
}
