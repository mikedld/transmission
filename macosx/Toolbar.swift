// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class Toolbar: NSToolbar {
    @objc private(set) var isRunningCustomizationPalette = false

    override var isVisible: Bool {
        didSet {
            // we need to redraw the main window after each change
            // otherwise we get strange drawing issues, leading to a potential crash
            NotificationCenter.default.post(name: NSNotification.Name("ToolbarDidChange"), object: nil)
        }
    }

    override func runCustomizationPalette(_ sender: Any?) {
        isRunningCustomizationPalette = true
        super.runCustomizationPalette(sender)
        isRunningCustomizationPalette = false
    }
}
