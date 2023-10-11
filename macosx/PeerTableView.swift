// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class PeerTableView: NSTableView {
    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        if row(at: point) != -1,
           column(at: point) == column(withIdentifier: NSUserInterfaceItemIdentifier("Progress")) {
            UserDefaults.standard.set(!UserDefaults.standard.bool(forKey: "DisplayPeerProgressBarNumber"),
                                      forKey: "DisplayPeerProgressBarNumber")

            let rowIndexes = IndexSet(integersIn: 0..<numberOfRows)
            let columnIndexes = IndexSet(integer: column(at: point))
            reloadData(forRowIndexes: rowIndexes, columnIndexes: columnIndexes)
        }
    }
}
