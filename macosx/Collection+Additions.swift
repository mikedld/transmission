// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import Foundation

extension Collection where Self: RandomAccessCollection {
    /// Returns a copy of the receiving array sorted as specified by a given array of sort descriptors.
    @inlinable
    func sorted(using sortDescriptors: [NSSortDescriptor]) -> [Element] {
        return sorted {
            for sortDescriptor in sortDescriptors {
                switch sortDescriptor.compare($0, to: $1) {
                case .orderedAscending: return true
                case .orderedDescending: return false
                case .orderedSame: continue
                }
            }
            return false
        }
    }
}

extension MutableCollection where Self: RandomAccessCollection {
    /// Sorts the receiver using a given array of sort descriptors.
    @inlinable
    mutating func sort(using sortDescriptors: [NSSortDescriptor]) {
        sort {
            for sortDescriptor in sortDescriptors {
                switch sortDescriptor.compare($0, to: $1) {
                case .orderedAscending: return true
                case .orderedDescending: return false
                case .orderedSame: continue
                }
            }
            return false
        }
    }
}

extension RangeReplaceableCollection where Index == Int {
    mutating func insert(_ objects: [Element], at indexes: IndexSet) {
        var objectIdx = 0
        for insertionIndex in indexes {
            insert(objects[objectIdx], at: insertionIndex)
            objectIdx += 1
        }
    }
}
