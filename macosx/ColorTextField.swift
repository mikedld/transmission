// This file Copyright © 2008-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class ColorTextField: NSTextField {
    override func awakeFromNib() {
        super.awakeFromNib()
        updateTextColor()
    }

    override var isEnabled: Bool {
        didSet {
            updateTextColor()
        }
    }

    func updateTextColor() {
        textColor = isEnabled ? .controlTextColor : .disabledControlTextColor
    }
}
