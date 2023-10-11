// This file Copyright © 2008-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import Foundation

class BonjourController: NSObject {
    private static let kBonjourServiceNameMaxLength: Int = 63
    @objc static let defaultController = BonjourController()

    var fService: NetService? {
        didSet {
            oldValue?.stop()
        }
    }

    @objc
    func startWithPort(_ port: Int32) {
        let serviceName = String(String(format: "Transmission (%@ - %@)", NSUserName(), Host.current().localizedName ?? "").prefix(Self.kBonjourServiceNameMaxLength))
        fService = NetService(domain: "", type: "_http._tcp.", name: serviceName, port: port)
        fService?.delegate = self
        fService?.publish()
    }

    @objc
    func stop() {
        fService = nil
    }
}

extension BonjourController: NetServiceDelegate {
    func netService(_ sender: NetService, didNotPublish errorDict: [String: NSNumber]) {
        NSLog("Failed to publish the web interface service on port %ld, with error: %@", sender.port, errorDict)
    }

    func netService(_ sender: NetService, didNotResolve errorDict: [String: NSNumber]) {
        NSLog("Failed to resolve the web interface service on port %ld, with error: %@", sender.port, errorDict)
    }
}
