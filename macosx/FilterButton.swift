// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class FilterButton: NSButton {
    @objc var count: Int = NSNotFound {
        didSet {
            if count != oldValue {
                toolTip = count == 1 ?
                NSLocalizedString("1 transfer", comment: "Filter Button -> tool tip") :
                String.localizedStringWithFormat(NSLocalizedString("%lu transfers", comment: "Filter Bar Button -> tool tip"), count)
            }
        }
    }
}
