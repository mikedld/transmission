// This file Copyright © 2005-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import Foundation

public extension NSString {
    @objc static let ellipsis = "…"

    @objc
    func stringByAppendingEllipsis() -> String {
        return (self as String) + Self.ellipsis
    }

    // Maximum supported localization is 9.22 EB, which is the maximum supported filesystem size by macOS, 8 EiB.
    // https://developer.apple.com/library/archive/documentation/FileManagement/Conceptual/APFS_Guide/VolumeFormatComparison/VolumeFormatComparison.html
    @objc
    static func stringForFileSize(_ size: UInt64) -> String {
        return ByteCountFormatter.string(fromByteCount: Int64(clamping: size), countStyle: .file)
    }

    // Maximum supported localization is 9.22 EB, which is the maximum supported filesystem size by macOS, 8 EiB.
    // https://developer.apple.com/library/archive/documentation/FileManagement/Conceptual/APFS_Guide/VolumeFormatComparison/VolumeFormatComparison.html
    @objc
    static func stringForFilePartialSize(_ partialSize: UInt64, fullSize: UInt64) -> String {
        let fileSizeFormatter = ByteCountFormatter()
        let fullSizeString = fileSizeFormatter.string(fromByteCount: Int64(clamping: fullSize))

        // figure out the magnitude of the two, since we can't rely on comparing the units because of localization and pluralization issues (for example, "1 byte of 2 bytes")
        let partialUnitsSame: Bool
        if partialSize == 0 {
            partialUnitsSame = true // we want to just show "0" when we have no partial data, so always set to the same units
        } else {
            partialUnitsSame = partialSize.kiloMagnitude == fullSize.kiloMagnitude
        }

        fileSizeFormatter.includesUnit = !partialUnitsSame
        let partialSizeString = fileSizeFormatter.string(fromByteCount: Int64(clamping: partialSize))

        return String(format: NSLocalizedString("%@ of %@", comment: "file size string"), partialSizeString, fullSizeString)
    }

    @objc
    static func stringForSpeed(_ speed: CGFloat) -> String {
        return stringForSpeed(speed,
                              kb: NSLocalizedString("KB/s", comment: "Transfer speed (kilobytes per second)"),
                              mb: NSLocalizedString("MB/s", comment: "Transfer speed (megabytes per second)"),
                              gb: NSLocalizedString("GB/s", comment: "Transfer speed (gigabytes per second)"))
    }

    @objc
    static func stringForSpeedAbbrev(_ speed: CGFloat) -> String {
        return stringForSpeed(speed, kb: "K", mb: "M", gb: "G")
    }

    @objc
    static func stringForSpeedAbbrevCompact(_ speed: CGFloat) -> String {
        return stringForSpeedCompact(speed, kb: "K", mb: "M", gb: "G")
    }

    @objc
    static func stringForRatio(_ ratio: CGFloat) -> String {
        // N/A is different than libtransmission's
        if ratio == CGFloat(C_TR_RATIO_NA) {
            return NSLocalizedString("N/A", comment: "No Ratio")
        }
        if ratio == CGFloat(C_TR_RATIO_INF) {
            return "∞"
        }
        if ratio < 10.0 {
            return String.localizedStringWithFormat("%.2f", ratio.truncDecimal(places: 2))
        }
        if ratio < 100.0 {
            return String.localizedStringWithFormat("%.1f", ratio.truncDecimal(places: 1))
        }
        return String.localizedStringWithFormat("%.0f", ratio.truncDecimal(places: 0))
    }

    private static let longFormatter = {
        let longFormatter = NumberFormatter()
        longFormatter.numberStyle = .percent
        longFormatter.maximumFractionDigits = 2
        return longFormatter
    }()
    private static let shortFormatter = {
        let shortFormatter = NumberFormatter()
        shortFormatter.numberStyle = .percent
        shortFormatter.maximumFractionDigits = 1
        return shortFormatter
    }()

    @objc
    static func percentString(_ progress: CGFloat, longDecimals: Bool) -> String {
        if progress >= 1.0 {
            return shortFormatter.string(from: 1)!
        }
        if longDecimals {
            return longFormatter.string(from: min(progress, 0.9999) as NSNumber)!
        }
        return shortFormatter.string(from: min(progress, 0.999) as NSNumber)!
    }

    @objc
    func compareNumeric(_ string: String) -> ComparisonResult {
        return (self as String).compare(string, options: [.numeric, .forcedOrdering], locale: .current)
    }

    @objc
    func nonEmptyComponentsSeparatedByCharactersInSet(_ separators: CharacterSet) -> [String] {
        return components(separatedBy: separators).filter { !$0.isEmpty }
    }

    @objc
    static func convertedStringWithCString(_ bytes: UnsafePointer<CChar>) -> String {
        // UTF-8 encoding
        if let result = String(validatingUTF8: bytes) {
            return result
        }
        // autodetection of the non-UTF-8 encoding (#3434)
        let data = Data(bytes: bytes, count: MemoryLayout<CChar>.stride * strlen(bytes))
        var result: NSString?
        _ = NSString.stringEncoding(for: data, encodingOptions: nil, convertedString: &result, usedLossyConversion: nil)
        if let result = result {
            return result as String
        }
        // hexa encoding as fallback
        // (alternatively to hexa representation, we could use `String(cString: bytes)` which replaces ill-formed UTF-8 code unit sequences with �)
        return data.hexString
    }
}

// MARK: private

private extension NSString {
    static func stringForSpeed(_ speedKb: CGFloat, kb: String, mb: String, gb: String) -> String {
        if speedKb < 999.95 {
            // 0.0 KB/s to 999.9 KB/s
            return String.localizedStringWithFormat("%.1f %@", speedKb, kb)
        }

        let speedMb = speedKb / 1_000.0
        if speedMb < 99.995 {
            // 1.00 MB/s to 99.99 MB/s
            return String.localizedStringWithFormat("%.2f %@", speedMb, mb)
        }
        if speedMb < 999.95 {
            // 100.0 MB/s to 999.9 MB/s
            return String.localizedStringWithFormat("%.1f %@", speedMb, mb)
        }

        let speedGb = speedMb / 1_000.0
        if speedGb < 99.995 {
            // 1.00 GB/s to 99.99 GB/s
            return String.localizedStringWithFormat("%.2f %@", speedGb, gb)
        }
        // 100.0 GB/s and above
        return String.localizedStringWithFormat("%.1f %@", speedGb, gb)
    }

    static func stringForSpeedCompact(_ speedKb: CGFloat, kb: String, mb: String, gb: String) -> String {
        if speedKb < 99.95 {
            // 0.0 KB/s to 99.9 KB/s
            return String.localizedStringWithFormat("%.1f %@", speedKb, kb)
        }
        if speedKb < 999.5 {
            // 100 KB/s to 999 KB/s
            return String.localizedStringWithFormat("%.0f %@", speedKb, kb)
        }

        let speedMb = speedKb / 1_000.0
        if speedMb < 9.995 {
            // 1.00 MB/s to 9.99 MB/s
            return String.localizedStringWithFormat("%.2f %@", speedMb, mb)
        }
        if speedMb < 99.95 {
            // 10.0 MB/s to 99.9 MB/s
            return String.localizedStringWithFormat("%.1f %@", speedMb, mb)
        }
        if speedMb < 999.5 {
            // 100 MB/s to 999 MB/s
            return String.localizedStringWithFormat("%.0f %@", speedMb, mb)
        }

        let speedGb = speedMb / 1_000.0
        if speedGb < 9.995 {
            // 1.00 GB/s to 9.99 GB/s
            return String.localizedStringWithFormat("%.2f %@", speedGb, gb)
        }
        if speedGb < 99.95 {
            // 10.0 GB/s to 99.9 GB/s
            return String.localizedStringWithFormat("%.1f %@", speedGb, gb)
        }
        // 100 GB/s and above
        return String.localizedStringWithFormat("%.0f %@", speedGb, gb)
    }
}

private extension UInt64 {
    var kiloMagnitude: Int {
        if self < 1_000 { return 0 }
        if self < 1_000_000 { return 1 }
        if self < 1_000_000_000 { return 2 }
        if self < 1_000_000_000_000 { return 3 }
        if self < 1_000_000_000_000_000 { return 4 }
        if self < 1_000_000_000_000_000_000 { return 5 }
        //       18_446_744_073_709_551_615 == UInt64.max
        return 6
    }
//    var kibiMagnitude: Int {
//        if self < 1 << 10 { return 0 }
//        if self < 1 << 20 { return 1 }
//        if self < 1 << 30 { return 2 }
//        if self < 1 << 40 { return 3 }
//        if self < 1 << 50 { return 4 }
//        if self < 1 << 60 { return 5 }
//        //        1 << 64 - 1 == UInt64.max
//        return 6
//    }
}

private extension CGFloat {
    func truncDecimal(places: Int) -> CGFloat {
        let divisor = pow(10.0, CGFloat(places))
        return (self * divisor).rounded(.towardZero) / divisor
    }
}

private extension Data {
    // hexChars from Peter, Aug 19 '14: https://stackoverflow.com/a/25378464
    // hexEncodedString from Martin R, Dec 9 '20: https://stackoverflow.com/a/40089462
    var hexString: String {
#if compiler(>=5.3.1)// macOS 11.0 support based on https://xcodereleases.com
        if #available(macOS 11.0, iOS 14.0, watchOS 7.0, tvOS 14.0, *) {
            return String(unsafeUninitializedCapacity: 2 * count) { buffer -> Int in
                let utf8Digits = Array("0123456789ABCDEF".utf8)
                var s = buffer.baseAddress!
                for byte in self {
                    s[0] = utf8Digits[Int(byte >> 4)]
                    s[1] = utf8Digits[Int(byte & 0xF)]
                    s += 2
                }
                return 2 * count
            }
        }
#endif
        return String(bytes: [UInt8](unsafeUninitializedCapacity: 2 * count) { buffer, initializedCount in
            let utf8Digits = Array("0123456789ABCDEF".utf8)
            var s = buffer.baseAddress!
            for byte in self {
                s[0] = utf8Digits[Int(byte >> 4)]
                s[1] = utf8Digits[Int(byte & 0xF)]
                s += 2
            }
            initializedCount = 2 * count
        }, encoding: .ascii)!
    }
}
