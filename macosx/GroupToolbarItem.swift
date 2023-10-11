// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class GroupToolbarItem: NSToolbarItemGroup {
    override func validate() {
        guard let control = view as? NSSegmentedControl else {
            return
        }
        for (i, item) in subitems.enumerated() {
            control.setEnabled(target?.validateToolbarItem(item) ?? false, forSegment: i)
        }
    }

    @objc
    func createMenu(_ labels: [String]) {
        let menuItem = NSMenuItem(title: label, action: nil, keyEquivalent: "")
        let menu = NSMenu(title: label)
        menuItem.submenu = menu
        menu.autoenablesItems = false
        for i in 0..<subitems.count {
            let addItem = NSMenuItem(title: labels[i], action: action, keyEquivalent: "")
            addItem.target = target
            addItem.tag = i
            menu.addItem(addItem)
        }
        menuFormRepresentation = menuItem
    }

    override var menuFormRepresentation: NSMenuItem? {
        get {
            guard let menuItem = super.menuFormRepresentation else {
                return nil
            }
            for (i, item) in subitems.enumerated() {
                menuItem.submenu?.item(at: i)?.isEnabled = target?.validateToolbarItem(item) ?? false
            }
            return menuItem
        }
        set {
            super.menuFormRepresentation = newValue
        }
    }
}
