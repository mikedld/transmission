// This file Copyright © 2011-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

extension NSImage {
    private static let kIconSize: CGFloat = 16.0
    private static let kBorderWidth: CGFloat = 1.25

    @objc
    public class func discIconWith(color: NSColor, insetFactor: CGFloat) -> NSImage {
        return NSImage(size: NSSize(width: kIconSize, height: kIconSize), flipped: false) { rect in
            // shape
            let insetRect = rect.insetBy(dx: kBorderWidth / 2 + rect.size.width * insetFactor / 2, dy: kBorderWidth / 2 + rect.size.height * insetFactor / 2)
            let bp = NSBezierPath(ovalIn: insetRect)
            bp.lineWidth = kBorderWidth

            // border
            let fractionOfBlendedColor: CGFloat = NSApp.darkMode ? 0.15 : 0.3
            let borderColor = color.blended(withFraction: fractionOfBlendedColor, of: .controlTextColor)
            borderColor?.setStroke()
            bp.stroke()

            // inside
            color.setFill()
            bp.fill()

            return true
        }
    }

    @objc
    public func imageWith(color: NSColor) -> NSImage {
        // swiftlint:disable:next force_cast
        let coloredImage = copy() as! NSImage
        coloredImage.lockFocus()
        color.set()
        let size = coloredImage.size
        NSRect(x: 0.0, y: 0.0, width: size.width, height: size.height).fill(using: .sourceAtop)
        coloredImage.unlockFocus()
        return coloredImage
    }

    @objc
    public class func systemSymbol(_ symbolName: String, withFallback fallbackName: String) -> NSImage? {
#if compiler(>=5.3.1)// macOS 11.0 support based on https://xcodereleases.com
        if #available(macOS 11.0, *) {
            return NSImage(systemSymbolName: symbolName, accessibilityDescription: nil)
        }
#endif
        return NSImage(named: fallbackName)
    }

    @objc
    public class func largeSystemSymbol(_ symbolName: String, withFallback fallbackName: String) -> NSImage? {
#if compiler(>=5.3.1)// macOS 11.0 support based on https://xcodereleases.com
        if #available(macOS 11.0, *) {
            return NSImage(systemSymbolName: symbolName, accessibilityDescription: nil)?.withSymbolConfiguration(NSImage.SymbolConfiguration(scale: .large))
        }
#endif
        return NSImage(named: fallbackName)
    }
}
