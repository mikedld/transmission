// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class ProgressGradients: NSObject {
    @objc class var progressWhiteGradient: NSGradient {
        if NSApp.darkMode {
            return progressGradientFor(red: 0.1, green: 0.1, blue: 0.1)
        } else {
            return progressGradientFor(red: 0.95, green: 0.95, blue: 0.95)
        }
    }

    @objc class var progressGrayGradient: NSGradient {
        if NSApp.darkMode {
            return progressGradientFor(red: 0.35, green: 0.35, blue: 0.35)
        } else {
            return progressGradientFor(red: 0.7, green: 0.7, blue: 0.7)
        }
    }

    @objc class var progressLightGrayGradient: NSGradient {
        if NSApp.darkMode {
            return progressGradientFor(red: 0.2, green: 0.2, blue: 0.2)
        } else {
            return progressGradientFor(red: 0.87, green: 0.87, blue: 0.87)
        }
    }

    @objc class var progressBlueGradient: NSGradient {
        if NSApp.darkMode {
            return progressGradientFor(red: 0.35 * 2.0 / 3.0, green: 0.67 * 2.0 / 3.0, blue: 0.98 * 2.0 / 3.0)
        } else {
            return progressGradientFor(red: 0.35, green: 0.67, blue: 0.98)
        }
    }

    @objc class var progressDarkBlueGradient: NSGradient {
        if NSApp.darkMode {
            return progressGradientFor(red: 0.616 * 2.0 / 3.0, green: 0.722 * 2.0 / 3.0, blue: 0.776 * 2.0 / 3.0)
        } else {
            return progressGradientFor(red: 0.616, green: 0.722, blue: 0.776)
        }
    }

    @objc class var progressGreenGradient: NSGradient {
        if NSApp.darkMode {
            return progressGradientFor(red: 0.44 * 2.0 / 3.0, green: 0.89 * 2.0 / 3.0, blue: 0.40 * 2.0 / 3.0)
        } else {
            return progressGradientFor(red: 0.44, green: 0.89, blue: 0.40)
        }
    }

    @objc class var progressLightGreenGradient: NSGradient {
        if NSApp.darkMode {
            return progressGradientFor(red: 0.62 * 3.0 / 4.0, green: 0.99 * 3.0 / 4.0, blue: 0.58 * 3.0 / 4.0)
        } else {
            return progressGradientFor(red: 0.62, green: 0.99, blue: 0.58)
        }
    }

    @objc class var progressDarkGreenGradient: NSGradient {
        if NSApp.darkMode {
            return progressGradientFor(red: 0.627 * 2.0 / 3.0, green: 0.714 * 2.0 / 3.0, blue: 0.639 * 2.0 / 3.0)
        } else {
            return progressGradientFor(red: 0.627, green: 0.714, blue: 0.639)
        }
    }

    @objc class var progressRedGradient: NSGradient {
        if NSApp.darkMode {
            return progressGradientFor(red: 0.902 * 2.0 / 3.0, green: 0.439 * 2.0 / 3.0, blue: 0.451 * 2.0 / 3.0)
        } else {
            return progressGradientFor(red: 0.902, green: 0.439, blue: 0.451)
        }
    }

    @objc class var progressYellowGradient: NSGradient {
        if NSApp.darkMode {
            return progressGradientFor(red: 0.933 * 0.8, green: 0.890 * 0.8, blue: 0.243 * 0.8)
        } else {
            return progressGradientFor(red: 0.933, green: 0.890, blue: 0.243)
        }
    }

    // MARK: - Private

    class func progressGradientFor(red redComponent: CGFloat, green greenComponent: CGFloat, blue blueComponent: CGFloat) -> NSGradient {
        let alpha: CGFloat = UserDefaults.standard.bool(forKey: "SmallView") ? 0.27 : 1.0

        let baseColor = NSColor(calibratedRed: redComponent, green: greenComponent, blue: blueComponent, alpha: alpha)
        let color2 = NSColor(calibratedRed: redComponent * 0.95, green: greenComponent * 0.95, blue: blueComponent * 0.95, alpha: alpha)
        let color3 = NSColor(calibratedRed: redComponent * 0.85, green: greenComponent * 0.85, blue: blueComponent * 0.85, alpha: alpha)

        return NSGradient(colorsAndLocations: (baseColor, 0.0), (color2, 0.5), (color3, 0.5), (baseColor, 1.0))!
    }
}
