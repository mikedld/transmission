// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import Foundation

class Transmission: NSObject {
    @objc static var cSession: UnsafePointer<c_tr_session>!
}
