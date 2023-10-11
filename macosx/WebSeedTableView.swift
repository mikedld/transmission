// This file Copyright © 2012-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class WebSeedTableView: NSTableView {
    @objc weak var webSeeds: WebSeeds?

    override func mouseDown(with event: NSEvent) {
        window?.makeKey()
        super.mouseDown(with: event)
    }

    @objc
    func copy(_ sender: Any) {
        var addresses = [String]()
        webSeeds?.webSeeds.enumerateObjects(at: selectedRowIndexes, using: { webSeed, _/*idx*/, _/*stop*/ in
            addresses.append(webSeed.address)
        })

        let pb = NSPasteboard.general
        pb.clearContents()
        pb.writeObjects([addresses.joined(separator: "\n")] as [NSPasteboardWriting])
    }
}

extension WebSeedTableView: NSMenuItemValidation {
    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        if menuItem.action == #selector(copy(_:)) {
            return numberOfSelectedRows > 0
        }
        return true
    }
}
