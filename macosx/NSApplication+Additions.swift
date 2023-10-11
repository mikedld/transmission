// This file Copyright © 2009-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

public extension NSApplication {
    @objc var darkMode: Bool {
        return effectiveAppearance.name == .darkAqua
    }
}
