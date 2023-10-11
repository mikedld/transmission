// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import Foundation

class WebSeed: NSObject {
    @objc let name: String
    @objc let address: String
    @objc let isDownloading: Bool
    @objc let dlFromRate: Double
    @objc
    init(name: String, address: String, isDownloading: Bool, dlFromRate: Double) {
        self.name = name
        self.address = address
        self.isDownloading = isDownloading
        // negative value for NSSortDescriptor
        // when removing @objc, dlFromRate can be replaced with Double? and -1 with nil
        self.dlFromRate = isDownloading ? dlFromRate : -1
    }
}

class WebSeeds: NSObject {
    @objc private(set) var webSeeds: [WebSeed]
    @objc
    override init() {
        self.webSeeds = []
    }
    @objc
    func removeAllObjects() {
        webSeeds.removeAll()
    }
    @objc
    func addObjectsFromArray(_ array: [WebSeed]) {
        webSeeds.append(contentsOf: array)
    }
    @objc
    func sortUsingDescriptors(_ descriptors: [NSSortDescriptor]) {
        webSeeds.sort(using: descriptors)
    }
}
