// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import Foundation

extension Array {
    /// Executes a given block using the objects in the array at the specified indexes.
    func enumerateObjects(at s: IndexSet,
                          options opts: NSEnumerationOptions = [],
                          using block: (Element, Int, UnsafeMutablePointer<ObjCBool>) -> Void) {
        (self as NSArray).enumerateObjects(at: s, options: opts) { obj, idx, stop in
            // swiftlint:disable:next force_cast
            block((obj as! Element), idx, stop)
        }
    }

    /// Returns the indexes of objects in the array that pass a test in a given block for a given set of enumeration options.
    func indexesOfObjects(options opts: NSEnumerationOptions = [],
                          passingTest predicate: (Element, Int, UnsafeMutablePointer<ObjCBool>) -> Bool) -> IndexSet {
        (self as NSArray).indexesOfObjects(options: opts) { obj, idx, stop in
            // swiftlint:disable:next force_cast
            return predicate((obj as! Element), idx, stop)
        }
    }
}
