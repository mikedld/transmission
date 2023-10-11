// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class DragOverlayView: NSView {
    private static let kPadding: CGFloat = 10.0
    private static let kIconWidth: CGFloat = 64.0

    private var fBadge: NSImage?

    private var fMainLineAttributes: [NSAttributedString.Key: Any]
    private var fSubLineAttributes: [NSAttributedString.Key: Any]

    override init(frame frameRect: NSRect) {
        // create attributes
        let stringShadow = NSShadow()
        stringShadow.shadowOffset = NSSize(width: 2.0, height: -2.0)
        stringShadow.shadowBlurRadius = 4.0

        let bigFont = NSFont.boldSystemFont(ofSize: 18.0)
        let smallFont = NSFont.systemFont(ofSize: 14.0)

        // swiftlint:disable:next force_cast
        let paragraphStyle = NSParagraphStyle.default.mutableCopy() as! NSMutableParagraphStyle
        paragraphStyle.lineBreakMode = .byTruncatingMiddle

        fMainLineAttributes = [
            .foregroundColor: NSColor.white,
            .font: bigFont,
            .shadow: stringShadow,
            .paragraphStyle: paragraphStyle,
        ]
        fSubLineAttributes = [
            .foregroundColor: NSColor.white,
            .font: smallFont,
            .shadow: stringShadow,
            .paragraphStyle: paragraphStyle,
        ]
        super.init(frame: frameRect)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    @objc
    func setOverlay(_ icon: NSImage, mainLine: String, subLine: String) {
        // create badge
        let badgeRect = NSRect(x: 0.0, y: 0.0, width: 325.0, height: 84.0)

        fBadge = NSImage(size: badgeRect.size)
        fBadge?.lockFocus()

        let bp = NSBezierPath(roundedRect: badgeRect, xRadius: 15.0, yRadius: 15.0)
        NSColor(calibratedWhite: 0.0, alpha: 0.75).set()
        bp.fill()

        // place icon
        icon.draw(in: NSRect(x: Self.kPadding, y: (badgeRect.height - Self.kIconWidth) * 0.5, width: Self.kIconWidth, height: Self.kIconWidth),
                  from: .zero,
                  operation: .sourceOver,
                  fraction: 1.0)

        // place main text
        let mainLineSize = mainLine.size(withAttributes: fMainLineAttributes)
        let subLineSize = subLine.size(withAttributes: fSubLineAttributes)

        var lineRect = NSRect(x: Self.kPadding + Self.kIconWidth + 5.0,
                              y: (badgeRect.height + (subLineSize.height + 2.0 - mainLineSize.height)) * 0.5,
                              width: badgeRect.width - (Self.kPadding + Self.kIconWidth + 2.0) - Self.kPadding,
                              height: mainLineSize.height)
        mainLine.draw(in: lineRect, withAttributes: fMainLineAttributes)

        // place sub text
        lineRect.origin.y -= subLineSize.height + 2.0
        lineRect.size.height = subLineSize.height
        subLine.draw(in: lineRect, withAttributes: fSubLineAttributes)

        fBadge?.unlockFocus()

        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        if let fBadge = fBadge {
            let frame = self.frame
            let imageSize = fBadge.size
            fBadge.draw(at: NSPoint(x: (frame.width - imageSize.width) * 0.5, y: (frame.height - imageSize.height) * 0.5),
                        from: .zero,
                        operation: .sourceOver,
                        fraction: 1.0)
        }
    }
}
