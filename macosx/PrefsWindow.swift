// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class PrefsWindow: NSWindow {
    override func awakeFromNib() {
        super.awakeFromNib()

#if compiler(>=5.3.1)// macOS 11.0 support based on https://xcodereleases.com
        if #available(macOS 11.0, *) {
            toolbarStyle = .preference
        }
#endif
    }

    override func keyDown(with event: NSEvent) {
        if event.keyCode == 53 {// esc key
            close()
        } else {
            super.keyDown(with: event)
        }
    }

    override func close() {
        makeFirstResponder(nil)// essentially saves pref changes on window close
        super.close()
    }
}
