// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class ButtonToolbarItem: NSToolbarItem {
    override func validate() {
        isEnabled = target?.validateToolbarItem(self) ?? false
    }

    override var menuFormRepresentation: NSMenuItem? {
        get {
            let menuItem = NSMenuItem(title: label, action: action, keyEquivalent: "")
            menuItem.target = target
            menuItem.isEnabled = target?.validateToolbarItem(self) ?? false

            return menuItem
        }
        set {}
    }
}
