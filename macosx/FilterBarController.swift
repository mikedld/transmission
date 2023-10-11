// This file Copyright © 2011-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

@objc
class FilterType: NSObject {
    @objc static let None = "None"
    @objc static let Active = "Active"
    @objc static let Download = "Download"
    @objc static let Seed = "Seed"
    @objc static let Pause = "Pause"
    @objc static let Error = "Error"
}

@objc
class FilterSearchType: NSObject {
    @objc static let Name = "Name"
    @objc static let Tracker = "Tracker"
}

@objc
enum FilterTypeTag: Int {
    case name = 401
    case tracker = 402
}

class FilterBarController: NSViewController {
    @objc static let kGroupFilterAllTag: Int = -2

    @objc var searchStrings: [String] {
        return fSearchField.stringValue.nonEmptyComponentsSeparatedByCharactersInSet(NSCharacterSet.whitespacesAndNewlines)
    }

    @IBOutlet private weak var fNoFilterButton: FilterButton!
    @IBOutlet private weak var fActiveFilterButton: FilterButton!
    @IBOutlet private weak var fDownloadFilterButton: FilterButton!
    @IBOutlet private weak var fSeedFilterButton: FilterButton!
    @IBOutlet private weak var fPauseFilterButton: FilterButton!
    @IBOutlet private weak var fErrorFilterButton: FilterButton!
    @IBOutlet private weak var fSearchField: NSSearchField!
    @IBOutlet private weak var fSearchFieldMinWidthConstraint: NSLayoutConstraint!
    @IBOutlet private weak var fGroupsButton: NSPopUpButton!

    init() {
        super.init(nibName: "FilterBar", bundle: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func awakeFromNib() {
        super.awakeFromNib()
        // localizations
        fNoFilterButton.title = NSLocalizedString("All", comment: "Filter Bar -> filter button")
        fActiveFilterButton.title = NSLocalizedString("Active", comment: "Filter Bar -> filter button")
        fDownloadFilterButton.title = NSLocalizedString("Downloading", comment: "Filter Bar -> filter button")
        fSeedFilterButton.title = NSLocalizedString("Seeding", comment: "Filter Bar -> filter button")
        fPauseFilterButton.title = NSLocalizedString("Paused", comment: "Filter Bar -> filter button")
        fErrorFilterButton.title = NSLocalizedString("Error", comment: "Filter Bar -> filter button")

        fNoFilterButton.cell?.backgroundStyle = .raised
        fActiveFilterButton.cell?.backgroundStyle = .raised
        fDownloadFilterButton.cell?.backgroundStyle = .raised
        fSeedFilterButton.cell?.backgroundStyle = .raised
        fPauseFilterButton.cell?.backgroundStyle = .raised
        fErrorFilterButton.cell?.backgroundStyle = .raised

        fSearchField.searchMenuTemplate?.item(withTag: FilterTypeTag.name.rawValue)?.title = NSLocalizedString("Name", comment: "Filter Bar -> filter menu")
        fSearchField.searchMenuTemplate?.item(withTag: FilterTypeTag.tracker.rawValue)?.title = NSLocalizedString("Tracker", comment: "Filter Bar -> filter menu")

        fGroupsButton.menu?.item(withTag: Self.kGroupFilterAllTag)?.title = NSLocalizedString("All Groups", comment: "Filter Bar -> group filter menu")

        // set current filter
        let filterType = UserDefaults.standard.string(forKey: "Filter")

        let currentFilterButton = buttonForFilterType(filterType)
        if currentFilterButton == fNoFilterButton,
           filterType != FilterType.None {
            // safety
            UserDefaults.standard.set(FilterType.None, forKey: "Filter")
        }
        currentFilterButton.state = .on

        // set filter search type
        let filterSearchType = UserDefaults.standard.string(forKey: "FilterSearchType")

        let filterSearchMenu = fSearchField.searchMenuTemplate
        let filterSearchTypeTitle: String?
        if filterSearchType == FilterSearchType.Tracker {
            filterSearchTypeTitle = filterSearchMenu?.item(withTag: FilterTypeTag.tracker.rawValue)?.title
        } else {
            // safety
            if filterType != FilterSearchType.Name {
                UserDefaults.standard.set(FilterSearchType.Name, forKey: "FilterSearchType")
            }
            filterSearchTypeTitle = filterSearchMenu?.item(withTag: FilterTypeTag.name.rawValue)?.title
        }
        fSearchField.placeholderString = filterSearchTypeTitle

        if let searchString = UserDefaults.standard.string(forKey: "FilterSearchString") {
            fSearchField.stringValue = searchString
        }

        updateGroupsButton()

        // update when groups change
        NotificationCenter.default.addObserver(self, selector: #selector(updateGroups(_:)), name: NSNotification.Name("UpdateGroups"), object: nil)

        // update when filter change
        fSearchField.delegate = self
    }

    private func buttonForFilterType(_ filterType: String?) -> FilterButton {
        if filterType == FilterType.Pause {
            return fPauseFilterButton
        } else if filterType == FilterType.Active {
            return fActiveFilterButton
        } else if filterType == FilterType.Seed {
            return fSeedFilterButton
        } else if filterType == FilterType.Download {
            return fDownloadFilterButton
        } else if filterType == FilterType.Error {
            return fErrorFilterButton
        } else {
            return fNoFilterButton
        }
    }

    @objc
    func setFilter(_ sender: FilterButton) {
        let oldFilterType = UserDefaults.standard.string(forKey: "Filter")
        let prevFilterButton = buttonForFilterType(oldFilterType)

        if sender != prevFilterButton {
            prevFilterButton.state = .off
            sender.state = .on

            let filterType: String
            if sender == fActiveFilterButton {
                filterType = FilterType.Active
            } else if sender == fDownloadFilterButton {
                filterType = FilterType.Download
            } else if sender == fPauseFilterButton {
                filterType = FilterType.Pause
            } else if sender == fSeedFilterButton {
                filterType = FilterType.Seed
            } else if sender == fErrorFilterButton {
                filterType = FilterType.Error
            } else {
                filterType = FilterType.None
            }

            UserDefaults.standard.set(filterType, forKey: "Filter")
        } else {
            sender.state = .on
        }

        NotificationCenter.default.post(name: Notification.Name("ApplyFilter"), object: nil)
    }

    @objc
    func switchFilter(_ right: Bool) {
        let filterType = UserDefaults.standard.string(forKey: "Filter")

        let button: FilterButton
        if filterType == FilterType.None {
            button = right ? fActiveFilterButton : fErrorFilterButton
        } else if filterType == FilterType.Active {
            button = right ? fDownloadFilterButton : fNoFilterButton
        } else if filterType == FilterType.Download {
            button = right ? fSeedFilterButton : fActiveFilterButton
        } else if filterType == FilterType.Seed {
            button = right ? fPauseFilterButton : fDownloadFilterButton
        } else if filterType == FilterType.Pause {
            button = right ? fErrorFilterButton : fSeedFilterButton
        } else if filterType == FilterType.Error {
            button = right ? fNoFilterButton : fPauseFilterButton
        } else {
            button = fNoFilterButton
        }

        setFilter(button)
    }

    @objc
    func setSearchText(_ sender: NSSearchField) {
        UserDefaults.standard.set(fSearchField.stringValue, forKey: "FilterSearchString")
        NotificationCenter.default.post(name: Notification.Name("ApplyFilter"), object: nil)
    }

    @objc
    func focusSearchField() {
        view.window?.makeFirstResponder(fSearchField)
    }

    @objc
    func isFocused() -> Bool {
        let textView = fSearchField.window?.firstResponder as? NSTextView
        return textView != nil &&
        fSearchField.window?.fieldEditor(false, for: nil) != nil && fSearchField == textView?.delegate as? NSSearchField
    }

    @IBAction private func setSearchType(_ sender: NSMenuItem) {
        let oldFilterType = UserDefaults.standard.string(forKey: "FilterSearchType")

        let currentTag = sender.tag
        let prevTag: Int
        if oldFilterType == FilterSearchType.Tracker {
            prevTag = FilterTypeTag.tracker.rawValue
        } else {
            prevTag = FilterTypeTag.name.rawValue
        }

        if currentTag != prevTag {
            let filterType: String
            if currentTag == FilterTypeTag.tracker.rawValue {
                filterType = FilterSearchType.Tracker
            } else {
                filterType = FilterSearchType.Name
            }

            UserDefaults.standard.set(filterType, forKey: "FilterSearchType")

            fSearchField.placeholderString = sender.title
        }

        NotificationCenter.default.post(name: Notification.Name("ApplyFilter"), object: nil)
    }

    @IBAction private func setGroupFilter(_ sender: NSMenuItem) {
        UserDefaults.standard.set(sender.tag, forKey: "FilterGroup")
        updateGroupsButton()

        NotificationCenter.default.post(name: Notification.Name("ApplyFilter"), object: nil)
    }

    @objc
    func reset(_ updateUI: Bool) {
        UserDefaults.standard.set(Self.kGroupFilterAllTag, forKey: "FilterGroup")

        if updateUI {
            updateGroupsButton()

            setFilter(fNoFilterButton)

            fSearchField.stringValue = ""
            setSearchText(fSearchField)
        } else {
            UserDefaults.standard.set(FilterType.None, forKey: "Filter")
            UserDefaults.standard.removeObject(forKey: "FilterSearchString")
        }
    }

    @objc
    func setCountAll(_ all: Int,// swiftlint:disable:this function_parameter_count
                     active: Int,
                     downloading: Int,
                     seeding: Int,
                     paused: Int,
                     error: Int) {
        fNoFilterButton.count = all
        fActiveFilterButton.count = active
        fDownloadFilterButton.count = downloading
        fSeedFilterButton.count = seeding
        fPauseFilterButton.count = paused
        fErrorFilterButton.count = error
    }

    // MARK: - Private

    private func updateGroupsButton() {
        let groupIndex = UserDefaults.standard.integer(forKey: "FilterGroup")

        let icon: NSImage?
        let toolTip: String
        if groupIndex == Self.kGroupFilterAllTag {
            icon = NSImage(named: "PinTemplate")
            toolTip = NSLocalizedString("All Groups", comment: "Groups -> Button")
        } else {
            icon = GroupsController.groups.image(for: groupIndex)
            let groupName: String = groupIndex != -1 ? GroupsController.groups.name(for: groupIndex) :
            NSLocalizedString("None", comment: "Groups -> Button")
            toolTip = NSLocalizedString("Group", comment: "Groups -> Button") + ": \(groupName)"
        }

        fGroupsButton.menu?.item(at: 0)?.image = icon
        fGroupsButton.toolTip = toolTip
    }

    @objc
    func updateGroups(_ notification: NSNotification) {
        updateGroupsButton()
    }
}

extension FilterBarController: NSMenuItemValidation {
    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        let action = menuItem.action

        // check proper filter search item
        if action == #selector(setSearchType(_:)) {
            let filterType = UserDefaults.standard.string(forKey: "FilterSearchType")

            let state: Bool
            if menuItem.tag == FilterTypeTag.tracker.rawValue {
                state = filterType == FilterSearchType.Tracker
            } else {
                state = filterType == FilterSearchType.Name
            }

            menuItem.state = state ? .on : .off
            return true
        }

        if action == #selector(setGroupFilter(_:)) {
            menuItem.state = menuItem.tag == UserDefaults.standard.integer(forKey: "FilterGroup") ? .on : .off
            return true
        }

        return true
    }
}

extension FilterBarController: NSMenuDelegate {
    func menuNeedsUpdate(_ menu: NSMenu) {
        if menu == fGroupsButton.menu {
            // remove all items except first three
            var i = menu.numberOfItems - 1
            while i >= 3 {
                menu.removeItem(at: i)
                i -= 1
            }

            guard let groupMenu = GroupsController.groups.groupMenu(withTarget: self, action: #selector(setGroupFilter(_:)), isSmall: true) else {
                return
            }

            let groupMenuCount = groupMenu.numberOfItems
            var j = 0
            while j < groupMenuCount {
                guard let item = groupMenu.item(at: 0) else {
                    return
                }
                groupMenu.removeItem(at: 0)
                menu.addItem(item)
                j += 1
            }
        }
    }
}

extension FilterBarController: NSSearchFieldDelegate {
    func searchFieldDidStartSearching(_ sender: NSSearchField) {
        fSearchFieldMinWidthConstraint.animator().constant = 95
    }

    func searchFieldDidEndSearching(_ sender: NSSearchField) {
        fSearchFieldMinWidthConstraint.animator().constant = 48
    }
}
