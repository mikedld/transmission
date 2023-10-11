// This file Copyright © 2009-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class InfoTextField: NSTextField {
    override var stringValue: String {
        didSet {
            isSelectable = !stringValue.isEmpty
        }
    }

    override var objectValue: Any? {
        didSet {
            isSelectable = !stringValue.isEmpty
        }
    }
}
