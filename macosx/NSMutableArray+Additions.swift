// This file Copyright © 2011-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import Foundation

public extension NSMutableArray {
    /*
     Note: This assumes Apple implemented this as an array under the hood.
     If the underlying data structure was a linked-list, for example, then this might be less
     efficient than simply removing the object and re-adding it.
     */
    @objc
    func moveObjectAtIndex(_ fromIndex: Int, toIndex: Int) {
        if fromIndex == toIndex {
            return
        }

        // shift objects - more efficient than simply removing the object and re-inserting the object
        let object = self[fromIndex]
        if fromIndex < toIndex {
            var i = fromIndex
            while i < toIndex {
                self[i] = self[i + 1]
                i += 1
            }
        } else {
            var i = fromIndex
            while i > toIndex {
                self[i] = self[i - 1]
                i -= 1
            }
        }
        self[toIndex] = object
    }
}

extension Array {
    /*
     Note: This assumes Swift implemented this as an array under the hood.
     If the underlying data structure was a linked-list, for example, then this might be less
     efficient than simply removing the object and re-adding it.
     */
    /*
     Benchmarking on macOS 13.1, a move fromIndex:1000 toIndex:0
     - Fastest: simply removing the object and re-adding it
     - Then: `self[i] = self[i - 1]` in a while loop [current implementation]
     - Then: `self[i] = self[i - 1]` in a `stride(from: fromIndex, to: toIndex, by: -1)` loop
     - Then: `swapAt(i, i - 1)` in a while loop
     - Slowest: SwiftUI's move(fromOffsets:toOffset:)
     On smaller distances, the performances differences are negligible, except for SwiftUI's API which is always slow.
     */
    mutating func moveObjectAtIndex(_ fromIndex: Index, toIndex: Index) {
        if fromIndex == toIndex {
            return
        }

        if abs(fromIndex - toIndex) == 1 {
            swapAt(fromIndex, toIndex)
            return
        }

        // shift objects - more efficient than simply removing the object and re-inserting the object
        let object = self[fromIndex]
        if fromIndex < toIndex {
            var i = fromIndex
            while i < toIndex {
                self[i] = self[i + 1]
                i += 1
            }
        } else {
            var i = fromIndex
            while i > toIndex {
                self[i] = self[i - 1]
                i -= 1
            }
        }
        self[toIndex] = object
    }
}
