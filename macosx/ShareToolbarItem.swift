// This file Copyright © 2014-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.
// Created by Mitchell Livingston on 1/8/14.

import AppKit

class ShareToolbarItem: ButtonToolbarItem {
    override var menuFormRepresentation: NSMenuItem? {
        get {
            let menuItem = NSMenuItem(title: label, action: nil, keyEquivalent: "")
            menuItem.isEnabled = target?.validateToolbarItem(self) ?? false

            if menuItem.isEnabled {
                let servicesMenu = NSMenu(title: "")
                for item in ShareTorrentFileHelper.shared.menuItems {
                    servicesMenu.addItem(item)
                }
                menuItem.submenu = servicesMenu
            }
            return menuItem
        }
        set {}
    }
}
