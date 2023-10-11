// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class StatusBarView: NSView {
    override var mouseDownCanMoveWindow: Bool {
        return true
    }

    override var isOpaque: Bool {
        return true
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.windowBackgroundColor.setFill()
        dirtyRect.fill(using: .copy)
        let lineBorderRect = NSRect(x: dirtyRect.minX, y: 0.0, width: dirtyRect.width, height: 1.0)
        if lineBorderRect.intersects(dirtyRect) {
            NSColor.gridColor.setFill()
            lineBorderRect.fill(using: .copy)
        }
    }
}
