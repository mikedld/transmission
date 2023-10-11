// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

enum SegmentTag: Int {
    case add = 0
    case remove = 1
}

class GroupsPrefsController: NSObject {
    private static let kGroupTableViewDataType = NSPasteboard.PasteboardType("GroupTableViewDataType")

    @IBOutlet private weak var fTableView: NSTableView!
    @IBOutlet private weak var fAddRemoveControl: NSSegmentedControl!
    @IBOutlet private weak var fSelectedColorView: NSColorWell!
    @IBOutlet private weak var fSelectedColorNameField: NSTextField!
    @IBOutlet private weak var fCustomLocationEnableCheck: NSButton!
    @IBOutlet private weak var fCustomLocationPopUp: NSPopUpButton!
    @IBOutlet private weak var fAutoAssignRulesEnableCheck: NSButton!
    @IBOutlet private weak var fAutoAssignRulesEditButton: NSButton!
    @IBOutlet private weak var groupRulesSheetWindow: NSWindow!
    @IBOutlet private weak var ruleEditor: NSPredicateEditor!
    @IBOutlet private weak var ruleEditorHeightConstraint: NSLayoutConstraint!

    private var selectedColorViewObservation: NSKeyValueObservation?

    override func awakeFromNib() {
        super.awakeFromNib()
        fTableView.registerForDraggedTypes([ Self.kGroupTableViewDataType ])

        selectedColorViewObservation = observeSelectedColorView()

#if compiler(>=5.7.1)// macOS 13.0 support based on https://xcodereleases.com
        if #available(macOS 13.0, *) {
            fSelectedColorView.colorWellStyle = .minimal
        }
#endif

        updateSelectedGroup()
    }

    private func observeSelectedColorView() -> NSKeyValueObservation {
        return fSelectedColorView.observe(\NSColorWell.color) { [weak self] selectedColorView, _/*observedChange*/ in
            guard let self = self else {
                return
            }
            if self.fTableView.numberOfSelectedRows == 1 {
                let index = GroupsController.groups.index(forRow: self.fTableView.selectedRow)
                GroupsController.groups.setColor(selectedColorView.color, for: index)
                self.fTableView.needsDisplay = true
            }
        }
    }

    @IBAction private func addRemoveGroup(_ sender: NSSegmentedControl) {
        guard let cell = sender.cell as? NSSegmentedCell else {
            return
        }

        if NSColorPanel.sharedColorPanelExists {
            NSColorPanel.shared.close()
        }

        var row: Int
        switch SegmentTag(rawValue: cell.tag(forSegment: sender.selectedSegment)) {
        case .add:
            fTableView.beginUpdates()

            GroupsController.groups.addNewGroup()

            row = fTableView.numberOfRows

            fTableView.insertRows(at: IndexSet(integer: row), withAnimation: .slideUp)
            fTableView.endUpdates()

            fTableView.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
            fTableView.scrollRowToVisible(row)

            fSelectedColorNameField.window?.makeFirstResponder(fSelectedColorNameField)

        case .remove:
            row = fTableView.selectedRow

            fTableView.beginUpdates()

            GroupsController.groups.removeGroup(withRowIndex: row)

            fTableView.removeRows(at: IndexSet(integer: row), withAnimation: .slideUp)
            fTableView.endUpdates()

            if fTableView.numberOfRows > 0 {
                if row == fTableView.numberOfRows {
                    row -= 1
                }
                fTableView.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
                fTableView.scrollRowToVisible(row)
            }
        case .none:
            break
        }

        updateSelectedGroup()
    }

    @IBAction private func customDownloadLocationSheetShow(_ sender: Any?) {
        guard let window = fCustomLocationPopUp.window else {
            return
        }
        let panel = NSOpenPanel()

        panel.prompt = NSLocalizedString("Select", comment: "Preferences -> Open panel prompt")
        panel.allowsMultipleSelection = false
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true

        panel.beginSheetModal(for: window) { result in
            let index = GroupsController.groups.index(forRow: self.fTableView.selectedRow)
            if result == .OK {
                let path = panel.urls[0].path
                GroupsController.groups.setCustomDownloadLocation(path, for: index)
                GroupsController.groups.setUsesCustomDownloadLocation(true, for: index)
            } else {
                if GroupsController.groups.customDownloadLocation(for: index) == nil {
                    GroupsController.groups.setUsesCustomDownloadLocation(false, for: index)
                }
            }
            self.refreshCustomLocationWithSingleGroup()
            self.fCustomLocationPopUp.selectItem(at: 0)
        }
    }

    @IBAction private func toggleUseCustomDownloadLocation(_ sender: Any) {
        let index = GroupsController.groups.index(forRow: fTableView.selectedRow)
        if fCustomLocationEnableCheck.state == .on {
            if GroupsController.groups.customDownloadLocation(for: index) != nil {
                GroupsController.groups.setUsesCustomDownloadLocation(true, for: index)
            } else {
                customDownloadLocationSheetShow(nil)
            }
        } else {
            GroupsController.groups.setUsesCustomDownloadLocation(false, for: index)
        }
        fCustomLocationPopUp.isEnabled = fCustomLocationEnableCheck.state == .on
    }

// MARK: - Rule editor

    @IBAction private func toggleUseAutoAssignRules(_ sender: Any) {
        let index = GroupsController.groups.index(forRow: fTableView.selectedRow)
        if fAutoAssignRulesEnableCheck.state == .on {
            if GroupsController.groups.autoAssignRules(for: index) != nil {
                GroupsController.groups.setUsesAutoAssignRules(true, for: index)
            } else {
                orderFrontRulesSheet(nil)
            }
        } else {
            GroupsController.groups.setUsesAutoAssignRules(false, for: index)
        }
        fAutoAssignRulesEditButton.isEnabled = fAutoAssignRulesEnableCheck.state == .on
    }

    @IBAction private func orderFrontRulesSheet(_ sender: Any?) {
        if groupRulesSheetWindow == nil {
            Bundle.main.loadNibNamed("GroupRules", owner: self, topLevelObjects: nil)
        }

        let index = GroupsController.groups.index(forRow: fTableView.selectedRow)
        let predicate = GroupsController.groups.autoAssignRules(for: index)
        ruleEditor.objectValue = predicate

        if ruleEditor.numberOfRows == 0 {
            ruleEditor.addRow(nil)
        }

        fTableView.window?.beginSheet(groupRulesSheetWindow, completionHandler: nil)
    }

    @IBAction private func cancelRules(_ sender: Any) {
        fTableView.window?.endSheet(groupRulesSheetWindow)

        let index = GroupsController.groups.index(forRow: fTableView.selectedRow)
        if GroupsController.groups.autoAssignRules(for: index) == nil {
            GroupsController.groups.setUsesAutoAssignRules(false, for: index)
            fAutoAssignRulesEnableCheck.state = .off
            fAutoAssignRulesEditButton.isEnabled = false
        }
    }

    @IBAction private func saveRules(_ sender: Any) {
        fTableView.window?.endSheet(groupRulesSheetWindow)

        let index = GroupsController.groups.index(forRow: fTableView.selectedRow)
        GroupsController.groups.setUsesAutoAssignRules(true, for: index)

        let predicate = ruleEditor.objectValue as? NSPredicate
        GroupsController.groups.setAutoAssignRules(predicate, for: index)

        fAutoAssignRulesEnableCheck.state = GroupsController.groups.usesAutoAssignRules(for: index) ? .on : .off
        fAutoAssignRulesEditButton.isEnabled = fAutoAssignRulesEnableCheck.state == .on
    }

    @objc
    func ruleEditorRowsDidChange(_ notification: Notification) {
        guard let ruleEditorScrollView = ruleEditor.enclosingScrollView else {
            return
        }

        let rowHeight = ruleEditor.rowHeight
        let bordersHeight = ruleEditorScrollView.frame.size.height - ruleEditorScrollView.contentSize.height

        let requiredRowCount = ruleEditor.numberOfRows
        let maxVisibleRowCount = Int((ruleEditor.window?.screen?.visibleFrame.height ?? 0) * 2 / 3 / rowHeight)

        ruleEditorHeightConstraint.constant = CGFloat(min(requiredRowCount, maxVisibleRowCount)) * rowHeight + bordersHeight
        ruleEditorScrollView.hasVerticalScroller = requiredRowCount > maxVisibleRowCount
    }

// MARK: - Private

    private func updateSelectedGroup() {
        fAddRemoveControl.setEnabled(fTableView.numberOfSelectedRows > 0, forSegment: SegmentTag.remove.rawValue)
        if fTableView.numberOfSelectedRows == 1 {
            let index = GroupsController.groups.index(forRow: fTableView.selectedRow)
            fSelectedColorView.color = GroupsController.groups.color(for: index)
            fSelectedColorView.isEnabled = true
            fSelectedColorNameField.stringValue = GroupsController.groups.name(for: index)
            fSelectedColorNameField.isEnabled = true

            refreshCustomLocationWithSingleGroup()

            fAutoAssignRulesEnableCheck.state = GroupsController.groups.usesAutoAssignRules(for: index) ? .on : .off
            fAutoAssignRulesEnableCheck.isEnabled = true
            fAutoAssignRulesEditButton.isEnabled = fAutoAssignRulesEnableCheck.state == .on
        } else {
            fSelectedColorView.color = .white
            fSelectedColorView.isEnabled = false
            fSelectedColorNameField.stringValue = ""
            fSelectedColorNameField.isEnabled = false
            fCustomLocationEnableCheck.isEnabled = false
            fCustomLocationPopUp.isEnabled = false
            fAutoAssignRulesEnableCheck.isEnabled = false
            fAutoAssignRulesEditButton.isEnabled = false
        }
    }

    private func refreshCustomLocationWithSingleGroup() {
        let index = GroupsController.groups.index(forRow: fTableView.selectedRow)

        let hasCustomLocation = GroupsController.groups.usesCustomDownloadLocation(for: index)
        fCustomLocationEnableCheck.state = hasCustomLocation ? .on : .off
        fCustomLocationEnableCheck.isEnabled = true
        fCustomLocationPopUp.isEnabled = hasCustomLocation

        if let location = GroupsController.groups.customDownloadLocation(for: index) {
            let pathTransformer = ExpandedPathToPathTransformer()
            fCustomLocationPopUp.item(at: 0)?.title = pathTransformer.transformedValue(location) as? String ?? ""
            let iconTransformer = ExpandedPathToIconTransformer()
            fCustomLocationPopUp.item(at: 0)?.image = iconTransformer.transformedValue(location) as? NSImage
        } else {
            fCustomLocationPopUp.item(at: 0)?.title = ""
            fCustomLocationPopUp.item(at: 0)?.image = nil
        }
    }
}

extension GroupsPrefsController: NSTableViewDataSource {
    func numberOfRows(in tableview: NSTableView) -> Int {
        return GroupsController.groups.numberOfGroups
    }
    func tableView(_ tableView: NSTableView, objectValueFor tableColumn: NSTableColumn?, row: Int) -> Any? {
        let groupsController = GroupsController.groups
        let groupsIndex = groupsController?.index(forRow: row) ?? 0

        let identifier = tableColumn?.identifier
        if identifier == NSUserInterfaceItemIdentifier("Color") {
            return groupsController?.image(for: groupsIndex)
        } else {
            return groupsController?.name(for: groupsIndex)
        }
    }

    func tableView(_ tableView: NSTableView, writeRowsWith rowIndexes: IndexSet, to pboard: NSPasteboard) -> Bool {
        pboard.declareTypes([ Self.kGroupTableViewDataType ], owner: self)
        pboard.setData(try? NSKeyedArchiver.archivedData(withRootObject: rowIndexes, requiringSecureCoding: true),
                       forType: Self.kGroupTableViewDataType)
        return true
    }

    func tableView(_ tableView: NSTableView, validateDrop info: NSDraggingInfo, proposedRow row: Int, proposedDropOperation dropOperation: NSTableView.DropOperation) -> NSDragOperation {
        let pasteboard = info.draggingPasteboard
        if pasteboard.types?.contains(Self.kGroupTableViewDataType) == true {
            fTableView.setDropRow(row, dropOperation: .above)
            return .generic
        }
        return []
    }

    func tableView(_ tableView: NSTableView, acceptDrop info: NSDraggingInfo, row: Int, dropOperation: NSTableView.DropOperation) -> Bool {
        let pasteboard = info.draggingPasteboard
        if pasteboard.types?.contains(Self.kGroupTableViewDataType) == true {
            guard let data = pasteboard.data(forType: Self.kGroupTableViewDataType) else {
                return true
            }
            guard let indexes = try? NSKeyedUnarchiver.unarchivedObject(ofClass: NSIndexSet.self, from: data) else {
                return true
            }

            let oldRow = indexes.firstIndex
            let newRow = oldRow < row ? row - 1 : row

            fTableView.beginUpdates()

            GroupsController.groups.moveGroup(atRow: oldRow, toRow: newRow)

            fTableView.moveRow(at: oldRow, to: newRow)
            fTableView.endUpdates()
        }
        return true
    }
}

extension GroupsPrefsController: NSTableViewDelegate {
    func tableViewSelectionDidChange(_ notification: Notification) {
        updateSelectedGroup()
    }
}

extension GroupsPrefsController: NSControlTextEditingDelegate {
    func controlTextDidEndEditing(_ obj: Notification) {
        if obj.object as? NSObject == fSelectedColorNameField {
            let index = GroupsController.groups.index(forRow: fTableView.selectedRow)
            GroupsController.groups.setName(fSelectedColorNameField.stringValue, for: index)
            fTableView.needsDisplay = true
        }
    }
}
