// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class DragOverlayWindow: NSWindow {
    private let fFadeInAnimation = NSViewAnimation()
    private let fFadeOutAnimation = NSViewAnimation()

    @objc
    init(forWindow window: NSWindow) {
        super.init(contentRect: window.frame, styleMask: .borderless, backing: .buffered, defer: false)

        backgroundColor = NSColor(calibratedWhite: 0.0, alpha: 0.5)
        alphaValue = 0.0
        isOpaque = false
        hasShadow = false
        contentView = DragOverlayView(frame: frame)
        isReleasedWhenClosed = false
        ignoresMouseEvents = true

        fFadeInAnimation.viewAnimations = [
            [ .target: self, .effect: NSViewAnimation.EffectName.fadeIn ]
        ]
        fFadeInAnimation.duration = 0.15
        fFadeInAnimation.animationBlockingMode = .nonblockingThreaded

        fFadeOutAnimation.viewAnimations = [
            [ .target: self, .effect: NSViewAnimation.EffectName.fadeOut ]
        ]
        fFadeOutAnimation.duration = 0.5
        fFadeOutAnimation.animationBlockingMode = .nonblockingThreaded

        window.addChildWindow(self, ordered: .above)
    }

    @objc
    func setTorrents(_ files: [String]) {
        var size: UInt64 = 0
        var count: UInt = 0
        var name: String = ""
        var fileCount: UInt = 0

        for file in files {
            if (try? NSWorkspace.shared.type(ofFile: file)) == "org.bittorrent.torrent" ||
                (file as NSString).pathExtension.caseInsensitiveCompare("torrent") == .orderedSame {
                if let metainfo = TRTorrentMetainfo.parseTorrentFile(file) {
                    count += 1
                    size += metainfo.totalSize()
                    fileCount += UInt(bitPattern: metainfo.fileCount())
                    // only useful when one torrent
                    if count == 1 {
                        if fileCount == 1 {
                            name = NSString.convertedStringWithCString(metainfo.fileSubpath(0))
                        }
                        if name.isEmpty {
                            name = String(utf8String: metainfo.name()) ?? ""
                        }
                    }
                }
            }
        }

        // swiftlint:disable:next empty_count
        if count <= 0 {
            return
        }

        // set strings and icon
        var secondString = NSString.stringForFileSize(size)
        if count > 1 {
            let fileString: String
            if fileCount == 1 {
                fileString = NSLocalizedString("1 file", comment: "Drag overlay -> torrents")
            } else {
                fileString = String.localizedStringWithFormat(NSLocalizedString("%lu files", comment: "Drag overlay -> torrents"), fileCount)
            }
            secondString = String(format: "%@, %@", fileString, secondString)
        }

        let icon: NSImage
        if count == 1 {
            icon = NSWorkspace.shared.icon(forFileType: fileCount <= 1 ? (name as NSString).pathExtension : NSFileTypeForHFSTypeCode(OSType(kGenericFolderIcon)))
        } else {
            name = String.localizedStringWithFormat(NSLocalizedString("%lu Torrent Files", comment: "Drag overlay -> torrents"), count)
            secondString += " total"
            icon = NSImage(named: "TransmissionDocument.icns")!
        }

        (contentView as? DragOverlayView)?.setOverlay(icon, mainLine: name, subLine: secondString)
        fadeIn()
    }

    @objc
    func setFile(_ file: String) {
        (contentView as? DragOverlayView)?.setOverlay(NSImage(named: "CreateLarge")!,
                                                      mainLine: NSLocalizedString("Create a Torrent File", comment: "Drag overlay -> file"),
                                                      subLine: file)
        fadeIn()
    }

    @objc
    func setURL(_ url: String) {
        (contentView as? DragOverlayView)?.setOverlay(NSImage(named: "Globe")!,
                                                      mainLine: NSLocalizedString("Web Address", comment: "Drag overlay -> url"),
                                                      subLine: url)
        fadeIn()
    }

    @objc
    func fadeIn() {
        // stop other animation and set to same progress
        if fFadeOutAnimation.isAnimating {
            fFadeOutAnimation.stop()
            fFadeInAnimation.currentProgress = 1.0 - fFadeOutAnimation.currentProgress
        }
        fFadeInAnimation.start()
    }

    @objc
    func fadeOut() {
        // stop other animation and set to same progress
        if fFadeInAnimation.isAnimating {
            fFadeInAnimation.stop()
            fFadeOutAnimation.currentProgress = 1.0 - fFadeInAnimation.currentProgress
        }
        if alphaValue > 0.0 {
            fFadeOutAnimation.start()
        }
    }
}
