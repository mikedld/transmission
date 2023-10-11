// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class BadgeView: NSView {
    private static let kBetweenPadding: CGFloat = 2.0
    private static let kWhiteUpArrow = NSImage(named: "UpArrowTemplate")!.imageWith(color: .white)
    private static let kWhiteDownArrow = NSImage(named: "DownArrowTemplate")!.imageWith(color: .white)
    // DownloadBadge and UploadBadge should have the same size
    private static let kBadgeSize: CGSize = NSImage(named: "DownloadBadge")!.size

    private static let kAttributes: [NSAttributedString.Key: Any] = {
        let stringShadow = NSShadow()
        stringShadow.shadowOffset = NSSize(width: 2.0, height: -2.0)
        stringShadow.shadowBlurRadius = 4.0
        return [
            .foregroundColor: NSColor.white,
            .shadow: stringShadow,
            .font: NSFont.boldSystemFont(ofSize: 26),
        ]
    }()
    private static let kArrowInset = CGSize(width: kBadgeSize.height * 0.2, height: kBadgeSize.height * 0.1)
    private static let kArrowSize = {
        // DownArrowTemplate and UpArrowTemplate should have the same size
        let arrowWidthHeightRatio = kWhiteDownArrow.size.width / kWhiteDownArrow.size.height
        // arrow height equal to font capital letter height + shadow
        // swiftlint:disable:next force_cast
        let arrowHeight = (kAttributes[.font] as! NSFont).capHeight + 4
        return CGSize(width: arrowHeight * arrowWidthHeightRatio, height: arrowHeight)
    }()
    enum ArrowDirection: Int {
        case up
        case down
    }
    private var fDownloadRate: CGFloat = 0
    private var fUploadRate: CGFloat = 0

    @objc
    func setRatesWithDownload(_ downloadRate: CGFloat, upload uploadRate: CGFloat) -> Bool {
        // only needs update if the badges were displayed or are displayed now
        if fDownloadRate == downloadRate, fUploadRate == uploadRate {
            return false
        }
        fDownloadRate = downloadRate
        fUploadRate = uploadRate
        return true
    }

    override func draw(_ dirtyRect: NSRect) {
        NSApp.applicationIconImage.draw(in: dirtyRect,
                                        from: .zero,
                                        operation: .sourceOver,
                                        fraction: 1.0)

        let upload = fUploadRate >= 0.1
        let download = fDownloadRate >= 0.1
        var bottom: CGFloat = 0.0
        if download {
            badge(.down,
                  string: NSString.stringForSpeedAbbrevCompact(fDownloadRate),
                  atHeight: bottom)
            if upload {
                bottom += Self.kBadgeSize.height + Self.kBetweenPadding // upload rate above download rate
            }
        }
        if upload {
            badge(.up,
                  string: NSString.stringForSpeedAbbrevCompact(fUploadRate),
                  atHeight: bottom)
        }
    }

    private func badge(_ arrowDirection: ArrowDirection, string: String, atHeight height: CGFloat) {
        // background
        let badge = arrowDirection == .up ? NSImage(named: "UploadBadge")! : NSImage(named: "DownloadBadge")!
        let badgeRect = NSRect(origin: CGPoint(x: 0.0, y: height), size: badge.size)
        badge.draw(in: badgeRect, from: .zero, operation: .sourceOver, fraction: 1.0)

        // string is in center of image
        let stringSize = string.size(withAttributes: Self.kAttributes)
        let stringRect = NSRect(origin: CGPoint(x: badgeRect.midX - stringSize.width * 0.5 + Self.kArrowInset.width,// adjust for arrow
                                                y: badgeRect.midY - stringSize.height * 0.5 + 1.0),// adjust for shadow
                                size: stringSize)
        string.draw(in: stringRect, withAttributes: Self.kAttributes)

        // arrow is in left of image
        let arrow = arrowDirection == .up ? Self.kWhiteUpArrow : Self.kWhiteDownArrow
        let arrowRect = NSRect(origin: CGPoint(x: Self.kArrowInset.width,
                                               y: stringRect.origin.y + Self.kArrowInset.height + (arrowDirection == .up ? 0.5 : -0.5)),
                               size: Self.kArrowSize)
        arrow.draw(in: arrowRect, from: .zero, operation: .sourceOver, fraction: 1.0)
    }
}
