// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class PriorityPopUpButtonCell: NSPopUpButtonCell {
    private static let kFrameInset: CGFloat = 2.0

    override func drawTitle(withFrame cellFrame: NSRect, in controlView: NSView) {
        var textFrame = cellFrame
        textFrame.origin.x += 2 * Self.kFrameInset
        super.drawTitle(withFrame: textFrame, in: controlView)
    }
}
