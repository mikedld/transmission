// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class ExpandedPathToIconTransformer: ValueTransformer {
    override class func transformedValueClass() -> AnyClass {
        return NSImage.self
    }

    override class func allowsReverseTransformation() -> Bool {
        return false
    }

    override func transformedValue(_ value: Any?) -> Any? {
        guard let value = value as? String else {
            return nil
        }

        let path = (value as NSString).expandingTildeInPath
        let icon: NSImage
        // show a folder icon if the folder doesn't exist
        if (path as NSString).pathExtension.isEmpty,
           !FileManager.default.fileExists(atPath: path) {
            icon = NSWorkspace.shared.icon(forFileType: NSFileTypeForHFSTypeCode(OSType(kGenericFolderIcon)))
        } else {
            icon = NSWorkspace.shared.icon(forFile: path)
        }
        icon.size = NSSize(width: 16.0, height: 16.0)

        return icon
    }
}
