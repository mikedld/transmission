// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class PeerProgressIndicatorCell: NSLevelIndicatorCell {
    private static let fAttributes: [NSAttributedString.Key: Any] = {
        // swiftlint:disable:next force_cast
        let paragraphStyle = NSParagraphStyle.default.mutableCopy() as! NSMutableParagraphStyle
        paragraphStyle.alignment = .right
        return [
            .font: NSFont.systemFont(ofSize: 11.0),
            .foregroundColor: NSColor.labelColor,
            .paragraphStyle: paragraphStyle,
        ]
    }()

    @objc var seed = false

    override func draw(withFrame cellFrame: NSRect, in controlView: NSView) {
        if UserDefaults.standard.bool(forKey: "DisplayPeerProgressBarNumber") {
            NSString.percentString(CGFloat(floatValue), longDecimals: false)
                .draw(in: cellFrame, withAttributes: Self.fAttributes)
        } else {
            super.draw(withFrame: cellFrame, in: controlView)
            if seed {
                let checkImage = NSImage(named: "CompleteCheck")!
                let imageSize = checkImage.size
                let rect = NSRect(x: floor(cellFrame.midX - imageSize.width * 0.5),
                                  y: floor(cellFrame.midY - imageSize.height * 0.5),
                                  width: imageSize.width,
                                  height: imageSize.height)
                checkImage.draw(in: rect,
                                from: .zero,
                                operation: .sourceOver,
                                fraction: 1.0,
                                respectFlipped: true,
                                hints: nil)
            }
        }
    }
}
