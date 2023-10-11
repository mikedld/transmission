// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

enum LevelButtonLevel: Int {
    case error = 0
    case warn = 1
    case info = 2
    case debug = 3
    // we do not support LevelButtonLevelTrace, as it would overwhelm everything (#4233)
}

@objc
protocol MessageWindowControllerGetter {
    @objc var messageWindowController: MessageWindowController { get }
}

class LogMessage: NSObject {
    // @objc for NSSortDescriptor keys
    @objc let message: String
    let date: Date
    @objc let name: String
    let file: String
    @objc let level: c_tr_log_level
    @objc let index: UInt

    init(_ logMessage: TRLogMessage, _ index: UInt) {
        message = String(cString: logMessage.message)
        date = Date(timeIntervalSince1970: TimeInterval(logMessage.when))
        name = logMessage.name[0] != 0 ? String(cString: logMessage.name) : ProcessInfo.processInfo.processName
        file = (logMessage.file as NSString).lastPathComponent + ":\(logMessage.line)"
        level = logMessage.level
        self.index = index
    }

    override var hash: Int {
        return Int(bitPattern: index)
    }

    override func isEqual(_ object: Any?) -> Bool {
        return (object as? LogMessage)?.index == index
    }

    var levelString: String {
        let levelString: String
        switch level {
        case C_TR_LOG_ERROR:
            levelString = NSLocalizedString("Error", comment: "Message window -> level")
        case C_TR_LOG_WARN:
            levelString = NSLocalizedString("Warning", comment: "Message window -> level")
        case C_TR_LOG_INFO:
            levelString = NSLocalizedString("Info", comment: "Message window -> level")
        case C_TR_LOG_DEBUG:
            levelString = NSLocalizedString("Debug", comment: "Message window -> level")
        case C_TR_LOG_TRACE:
            levelString = NSLocalizedString("Trace", comment: "Message window -> level")
        default:
            assert(false, "Unknown message log level: \(level)")
            levelString = "?"
        }
        return levelString
    }
}

class MessageWindowController: NSWindowController {
    private static let kUpdateSeconds: TimeInterval = 0.75

    @IBOutlet private weak var fMessageTable: NSTableView!
    @IBOutlet private weak var fLevelButton: NSPopUpButton!
    @IBOutlet private weak var fSaveButton: NSButton!
    @IBOutlet private weak var fClearButton: NSButton!
    @IBOutlet private weak var fFilterField: NSSearchField!

    private var fAttributes: [NSAttributedString.Key: Any]!

    private var fMessages: [LogMessage] = []
    private var fDisplayedMessages: [LogMessage] = []
    private var fTimer: Timer?
    private var fLock = NSLock()

    convenience init() {
        self.init(windowNibName: "MessageWindow")
    }

    override func awakeFromNib() {
        super.awakeFromNib()
        window?.setFrameAutosaveName("MessageWindowFrame")
        window?.setFrameUsingName("MessageWindowFrame")
        window?.restorationClass = type(of: self)
        window?.identifier = NSUserInterfaceItemIdentifier("MessageWindow")

        window?.setContentBorderThickness(fMessageTable.enclosingScrollView!.frame.minY, for: .minY)
        window?.title = NSLocalizedString("Message Log", comment: "Message window -> title")

        // disable fullscreen support
        window?.collectionBehavior = .fullScreenNone

        // set images and text for popup button items
        fLevelButton.item(at: LevelButtonLevel.error.rawValue)?.title = NSLocalizedString("Error", comment: "Message window -> level string")
        fLevelButton.item(at: LevelButtonLevel.warn.rawValue)?.title = NSLocalizedString("Warning", comment: "Message window -> level string")
        fLevelButton.item(at: LevelButtonLevel.info.rawValue)?.title = NSLocalizedString("Info", comment: "Message window -> level string")
        fLevelButton.item(at: LevelButtonLevel.debug.rawValue)?.title = NSLocalizedString("Debug", comment: "Message window -> level string")
        fLevelButton.item(at: LevelButtonLevel.error.rawValue)?.image = Self.iconForLevel(C_TR_LOG_ERROR)
        fLevelButton.item(at: LevelButtonLevel.warn.rawValue)?.image = Self.iconForLevel(C_TR_LOG_WARN)
        fLevelButton.item(at: LevelButtonLevel.info.rawValue)?.image = Self.iconForLevel(C_TR_LOG_INFO)
        fLevelButton.item(at: LevelButtonLevel.debug.rawValue)?.image = Self.iconForLevel(C_TR_LOG_DEBUG)

        let levelButtonOldWidth = fLevelButton.frame.width
        fLevelButton.sizeToFit()

        // set table column text
        fMessageTable.tableColumn(withIdentifier: NSUserInterfaceItemIdentifier("Date"))!.headerCell.title = NSLocalizedString("Date", comment: "Message window -> table column")
        fMessageTable.tableColumn(withIdentifier: NSUserInterfaceItemIdentifier("Name"))!.headerCell.title = NSLocalizedString("Process", comment: "Message window -> table column")
        fMessageTable.tableColumn(withIdentifier: NSUserInterfaceItemIdentifier("Message"))!.headerCell.title = NSLocalizedString("Message", comment: "Message window -> table column")

        NotificationCenter.default.addObserver(self,
                                               selector: #selector(resizeColumn),
                                               name: NSTableView.columnDidResizeNotification,
                                               object: fMessageTable)

        // set and size buttons
        fSaveButton.title = NSLocalizedString("Save", comment: "Message window -> save button") + NSString.ellipsis
        fSaveButton.sizeToFit()

        var saveButtonFrame = fSaveButton.frame
        saveButtonFrame.size.width += 10.0
        saveButtonFrame.origin.x += fLevelButton.frame.width - levelButtonOldWidth
        fSaveButton.frame = saveButtonFrame

        let oldClearButtonWidth = fClearButton.frame.size.width

        fClearButton.title = NSLocalizedString("Clear", comment: "Message window -> save button")
        fClearButton.sizeToFit()

        var clearButtonFrame = fClearButton.frame
        clearButtonFrame.size.width = max(clearButtonFrame.size.width + 10.0, saveButtonFrame.size.width)
        clearButtonFrame.origin.x -= clearButtonFrame.width - oldClearButtonWidth
        fClearButton.frame = clearButtonFrame

        (fFilterField.cell as? NSTextFieldCell)?.placeholderString = NSLocalizedString("Filter", comment: "Message window -> filter field")
        var filterButtonFrame = fFilterField.frame
        filterButtonFrame.origin.x -= clearButtonFrame.width - oldClearButtonWidth
        fFilterField.frame = filterButtonFrame

        fAttributes = (fMessageTable.tableColumn(withIdentifier: NSUserInterfaceItemIdentifier("Message"))!.dataCell as? NSCell)?.attributedStringValue.attributes(at: 0, effectiveRange: nil)

        // select proper level in popup button
        let logLevel = c_tr_log_level(UInt32(clamping: UserDefaults.standard.integer(forKey: "MessageLevel")))
        switch logLevel {
        case C_TR_LOG_ERROR:
            fLevelButton.selectItem(at: LevelButtonLevel.error.rawValue)
        case C_TR_LOG_WARN:
            fLevelButton.selectItem(at: LevelButtonLevel.warn.rawValue)
        case C_TR_LOG_INFO:
            fLevelButton.selectItem(at: LevelButtonLevel.info.rawValue)
        case C_TR_LOG_DEBUG, C_TR_LOG_TRACE:
            fLevelButton.selectItem(at: LevelButtonLevel.debug.rawValue)
        default: // safety
            UserDefaults.standard.set(Int(C_TR_LOG_ERROR.rawValue), forKey: "MessageLevel")
            fLevelButton.selectItem(at: LevelButtonLevel.error.rawValue)
        }
    }

    deinit {
        fTimer?.invalidate()
    }

    // cache dictionary
    private static var icons = [NSColor: NSImage]()

    private class func iconForLevel(_ level: c_tr_log_level) -> NSImage? {
        let color: NSColor
        switch level {
        case C_TR_LOG_CRITICAL, C_TR_LOG_ERROR:
            color = .systemRed
        case C_TR_LOG_WARN:
            color = .systemOrange
        case C_TR_LOG_INFO:
            color = .systemGreen
        case C_TR_LOG_DEBUG:
            color = .systemTeal
        case C_TR_LOG_TRACE:
            color = .systemPurple
        default:
            assert(false, "Unknown message log level: \(level)")
            return nil
        }

        // from cache
        if let icon = icons[color] {
            return icon
        }
        // to cache
        let icon = NSImage.discIconWith(color: color, insetFactor: 0.5)
        icons[color] = icon
        return icon
    }

    // more accurate when sorting by date
    private static var currentIndex: UInt = 0

    @objc
    private func updateLog(_ timer: Timer?) {
        guard let messages = c_tr_logGetQueue() else {
            return
        }

        fLock.lock()

        let scroller = fMessageTable.enclosingScrollView?.verticalScroller
        let shouldScroll = Self.currentIndex == 0 || scroller?.floatValue == 1.0 || scroller?.isHidden == true || scroller?.knobProportion == 1.0

        let maxLevel = UserDefaults.standard.integer(forKey: "MessageLevel")
        let filterString = fFilterField.stringValue

        var changed = false

        var currentMessage: TRLogMessage? = messages
        while let message = currentMessage {
            let logMessage = LogMessage(message, Self.currentIndex)
            fMessages.append(logMessage)
            if message.level.rawValue <= maxLevel,
               shouldIncludeMessageForFilter(filterString, message: logMessage) {
                fDisplayedMessages.append(logMessage)
                changed = true
            }
            Self.currentIndex += 1
            currentMessage = message.next()
        }

        if fMessages.count > C_TR_LOG_MAX_QUEUE_LENGTH {
            let oldCount = fDisplayedMessages.count
            let removeIndexes = IndexSet(integersIn: 0..<(fMessages.count - Int(C_TR_LOG_MAX_QUEUE_LENGTH)))
            let itemsToRemove = Set(removeIndexes.map { fMessages[$0] })
            removeIndexes.reversed().forEach { fMessages.remove(at: $0) }
            fDisplayedMessages.removeAll {
                return itemsToRemove.contains($0)
            }

            changed = changed || oldCount > fDisplayedMessages.count
        }

        if changed {
            fDisplayedMessages.sort(using: fMessageTable.sortDescriptors)

            fMessageTable.reloadData()
            if shouldScroll {
                fMessageTable.scrollRowToVisible(fMessageTable.numberOfRows - 1)
            }
        }

        fLock.unlock()

        c_tr_logFreeQueue(messages)
    }

    @objc
    func copy(_ sender: Any) {
        let indexes = fMessageTable.selectedRowIndexes
        let messageStrings: [String] = indexes.map { index in
            return stringForMessage(fDisplayedMessages[index])
        }

        let messageString = messageStrings.joined(separator: "\n")

        let pb = NSPasteboard.general
        pb.clearContents()
        pb.writeObjects([ messageString ] as [NSPasteboardWriting])
    }

    @IBAction private func changeLevel(_ sender: Any) {
        let level: c_tr_log_level
        switch LevelButtonLevel(rawValue: fLevelButton.indexOfSelectedItem) {
        case .error:
            level = C_TR_LOG_ERROR
        case .warn:
            level = C_TR_LOG_WARN
        case .info:
            level = C_TR_LOG_INFO
        case .debug:
            level = C_TR_LOG_DEBUG
        default:
            assert(false, "Unknown message log level: \(fLevelButton.indexOfSelectedItem)")
            level = C_TR_LOG_INFO
        }

        if UserDefaults.standard.integer(forKey: "MessageLevel") == level.rawValue {
            return
        }

        UserDefaults.standard.set(level.rawValue, forKey: "MessageLevel")

        fLock.lock()

        updateListForFilter()

        fLock.unlock()
    }

    @IBAction private func changeFilter(_ sender: Any) {
        fLock.lock()

        updateListForFilter()

        fLock.unlock()
    }

    @IBAction private func clearLog(_ sender: Any) {
        fLock.lock()

        fMessages.removeAll()

        fMessageTable.beginUpdates()
        fMessageTable.removeRows(at: IndexSet(integersIn: 0..<fDisplayedMessages.count),
                                 withAnimation: .slideLeft)

        fDisplayedMessages.removeAll()

        fMessageTable.endUpdates()

        fLock.unlock()
    }

    @IBAction private func writeToFile(_ sender: Any) {
        guard let window = window else {
            return
        }
        let panel = NSSavePanel()
        panel.allowedFileTypes = [ "txt" ]
        panel.canSelectHiddenExtension = true

        panel.nameFieldStringValue = NSLocalizedString("untitled", comment: "Save log panel -> default file name")

        panel.beginSheetModal(for: window, completionHandler: { result in
            guard result == .OK else {
                return
            }
            // make the array sorted by date
            let descriptors = [ NSSortDescriptor(key: "index", ascending: true) ]
            let sortedMessages = self.fDisplayedMessages.sorted(using: descriptors)

            // create the text to output
            let fileString = sortedMessages.map { message in
                return self.stringForMessage(message)
            }.joined(separator: "\n")

            guard let path = panel.url?.path,
                  (try? fileString.write(toFile: path, atomically: true, encoding: .utf8)) != nil else {
                let alert = NSAlert()
                alert.addButton(withTitle: NSLocalizedString("OK", comment: "Save log alert panel -> button"))
                alert.messageText = NSLocalizedString("Log Could Not Be Saved", comment: "Save log alert panel -> title")
                alert.informativeText = String(format: NSLocalizedString("There was a problem creating the file \"%@\".", comment: "Save log alert panel -> message"),
                                               ((panel.url?.path ?? "") as NSString).lastPathComponent)
                alert.alertStyle = .warning
                alert.runModal()
                return
            }
        })
    }

// MARK: - Private

    @objc
    private func resizeColumn() {
        fMessageTable.noteHeightOfRows(withIndexesChanged: IndexSet(integersIn: 0..<fMessageTable.numberOfRows))
    }

    private func shouldIncludeMessageForFilter(_ filterString: String, message: LogMessage) -> Bool {
        if filterString.isEmpty {
            return true
        }

        let searchOptions: NSString.CompareOptions = [.caseInsensitive, .diacriticInsensitive]
        return message.name.range(of: filterString, options: searchOptions) != nil ||
        message.message.range(of: filterString, options: searchOptions) != nil
    }

    private func updateListForFilter() {
        let level = UserDefaults.standard.integer(forKey: "MessageLevel")
        let filterString = fFilterField.stringValue

        let indexes = fMessages.indexesOfObjects(options: .concurrent, passingTest: { message, _/*idx*/, _/*stop*/ in
            return message.level.rawValue <= level && shouldIncludeMessageForFilter(filterString, message: message)
        })

        let tempMessages = indexes.map { fMessages[$0] }.sorted(using: fMessageTable.sortDescriptors)

        fMessageTable.beginUpdates()

        // figure out which rows were added/moved
        var currentIndex: Int = 0
        var totalCount: Int = 0
        var itemsToAdd = [LogMessage]()
        var itemsToAddIndexes = IndexSet()

        for message in tempMessages {
            let previousIndex = (fDisplayedMessages as NSArray).index(of: message,
                                                                      in: NSRange(location: currentIndex, length: fDisplayedMessages.count - currentIndex))
            if previousIndex == NSNotFound {
                itemsToAdd.append(message)
                itemsToAddIndexes.insert(totalCount)
            } else {
                if previousIndex != currentIndex {
                    fDisplayedMessages.moveObjectAtIndex(previousIndex, toIndex: currentIndex)
                    fMessageTable.moveRow(at: previousIndex, to: currentIndex)
                }
                currentIndex += 1
            }
            totalCount += 1
        }

        // remove trailing items - those are the unused
        if currentIndex < fDisplayedMessages.count {
            let removeRange = currentIndex..<fDisplayedMessages.count
            fDisplayedMessages.removeSubrange(removeRange)
            fMessageTable.removeRows(at: IndexSet(integersIn: removeRange),
                                     withAnimation: .slideDown)
        }

        // add new items
        fDisplayedMessages.insert(itemsToAdd, at: itemsToAddIndexes)
        fMessageTable.insertRows(at: itemsToAddIndexes, withAnimation: .slideUp)

        fMessageTable.endUpdates()

        assert(fDisplayedMessages == tempMessages, "Inconsistency between message arrays! \(fDisplayedMessages) \(tempMessages)")
    }

    private func stringForMessage(_ message: LogMessage) -> String {
        return "\(message.date) \(message.file) [\(message.levelString)] \(message.name): \(message.message)"
    }
}

extension MessageWindowController: NSWindowDelegate {
    func windowDidBecomeKey(_ notification: Notification) {
        if fTimer == nil {
            fTimer = Timer.scheduledTimer(timeInterval: Self.kUpdateSeconds,
                                          target: self,
                                          selector: #selector(updateLog(_:)),
                                          userInfo: nil,
                                          repeats: true)
            updateLog(nil)
        }
    }

    func windowWillClose(_ notification: Notification) {
        fTimer?.invalidate()
        fTimer = nil
    }

    func window(_ window: NSWindow, didDecodeRestorableState state: NSCoder) {
        fTimer?.invalidate()
        fTimer = Timer.scheduledTimer(timeInterval: Self.kUpdateSeconds,
                                      target: self,
                                      selector: #selector(updateLog(_:)),
                                      userInfo: nil,
                                      repeats: true)
        updateLog(nil)
    }
}

extension MessageWindowController: NSWindowRestoration {
    static func restoreWindow(withIdentifier identifier: NSUserInterfaceItemIdentifier,
                              state: NSCoder,
                              completionHandler: @escaping (NSWindow?, Error?) -> Void) {
        assert(identifier.rawValue == "MessageWindow", "Trying to restore unexpected identifier \(identifier)")

        let window = (NSApp.delegate as? MessageWindowControllerGetter)?.messageWindowController.window
        completionHandler(window, nil)
    }
}

extension MessageWindowController: NSTableViewDataSource {
    func numberOfRows(in tableView: NSTableView) -> Int {
        return fDisplayedMessages.count
    }

    func tableView(_ tableView: NSTableView, objectValueFor tableColumn: NSTableColumn?, row: Int) -> Any? {
        let identifier = tableColumn?.identifier.rawValue ?? ""
        let message = fDisplayedMessages[row]

        if identifier == "Date" {
            return message.date
        } else if identifier == "Level" {
            return Self.iconForLevel(message.level)
        } else if identifier == "Name" {
            return message.name
        } else {
            return message.message
        }
    }

    func tableView(_ tableView: NSTableView, sortDescriptorsDidChange oldDescriptors: [NSSortDescriptor]) {
        fDisplayedMessages.sort(using: fMessageTable.sortDescriptors)
        fMessageTable.reloadData()
    }
}

extension MessageWindowController: NSTableViewDelegate {
#warning("don't cut off end")
    func tableView(_ tableView: NSTableView, heightOfRow row: Int) -> CGFloat {
        let message = fDisplayedMessages[row].message

        let column = tableView.tableColumn(withIdentifier: NSUserInterfaceItemIdentifier("Message"))!
        let count = floor(message.size(withAttributes: fAttributes).width / column.width)

        return tableView.rowHeight * (count + 1.0)
    }

    // swiftlint:disable:next function_parameter_count
    func tableView(_ tableView: NSTableView, toolTipFor cell: NSCell, rect: NSRectPointer, tableColumn: NSTableColumn?, row: Int, mouseLocation: NSPoint) -> String {
        return fDisplayedMessages[row].file
    }
}

extension MessageWindowController: NSMenuItemValidation {
    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        if menuItem.action == #selector(copy(_:)) {
            return fMessageTable.numberOfSelectedRows > 0
        }
        return true
    }
}
