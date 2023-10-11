// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class GroupPopUpButtonCell: NSPopUpButtonCell {
    private static let kFrameInset: CGFloat = 2.0

    override func drawImage(withFrame cellFrame: NSRect, in controlView: NSView) {
        var imageFrame = cellFrame
        imageFrame.origin.x -= Self.kFrameInset
        super.drawImage(withFrame: imageFrame, in: controlView)
    }

    override func drawTitle(withFrame cellFrame: NSRect, in controlView: NSView) {
        var textFrame = cellFrame
        textFrame.origin.y += Self.kFrameInset / 2
        super.drawTitle(withFrame: textFrame, in: controlView)
    }
}
