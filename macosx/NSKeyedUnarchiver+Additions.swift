// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import Foundation

protocol DeprecatedUnarchiveObject {
    static func deprecatedUnarchiveObject(with data: Data) -> Any?
}

extension NSKeyedUnarchiver: DeprecatedUnarchiveObject {
    @available(macOS, deprecated: 10.13)
    class func deprecatedUnarchiveObject(with data: Data) -> Any? {
        return NSUnarchiver.unarchiveObject(with: data)
    }

    @objc
    public class func deprecatedUnarchiveObject(data: Data) -> Any? {
        // ignoring deprecation warning on NSUnarchiver:
        // there are no compatible alternatives to handle the old data when migrating from Transmission 3,
        // so we'll use NSUnarchiver as long as Apple supports it
        return (self as DeprecatedUnarchiveObject.Type).deprecatedUnarchiveObject(with: data)
    }
}
