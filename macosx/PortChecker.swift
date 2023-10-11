// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import Foundation

@objc
enum PortStatus: UInt {
    case checking
    case open
    case closed
    case error
}

@objc
protocol PortCheckerDelegate: NSObjectProtocol {
    @objc
    func portCheckerDidFinishProbing(_ portChecker: PortChecker)
}

class PortChecker: NSObject {
    private static let kCheckFireInterval: TimeInterval = 3.0

    @objc var status: PortStatus {
        return fStatus
    }

    weak var fDelegate: (NSObject & PortCheckerDelegate)?

    private var fStatus: PortStatus = .checking
    private var fSession: URLSession
    private var fTask: URLSessionDataTask?
    private var fTimer: Timer?

    @objc
    init(forPort portNumber: Int, delay: Bool, withDelegate delegate: NSObject & PortCheckerDelegate) {
        fSession = URLSession(configuration: .ephemeral)
        fDelegate = delegate
        super.init()
        fTimer = Timer.scheduledTimer(timeInterval: Self.kCheckFireInterval,
                                      target: self,
                                      selector: #selector(startProbe(_:)),
                                      userInfo: portNumber,
                                      repeats: false)
        if !delay {
            fTimer?.fire()
        }
    }

    deinit {
        cancelProbe()
    }

    @objc
    func cancelProbe() {
        fTimer?.invalidate()
        fTimer = nil
        fTask?.cancel()
    }

    // MARK: - Private

    @objc
    func startProbe(_ timer: Timer) {
        fTimer = nil

        // swiftlint:disable:next force_cast
        let urlString = String(format: "https://portcheck.transmissionbt.com/%ld", timer.userInfo as! Int)
        let portProbeRequest = URLRequest(url: URL(string: urlString)!,
                                          cachePolicy: .reloadIgnoringLocalAndRemoteCacheData,
                                          timeoutInterval: 15.0)
        fTask = fSession.dataTask(with: portProbeRequest, completionHandler: { data, _/*response*/, error in
            if let error = error {
                NSLog("Unable to get port status: connection failed (%@)", error.localizedDescription)
                self.callBackWithStatus(.error)
                return
            }
            guard let probeString = String(data: data ?? Data(), encoding: .utf8) else {
                NSLog("Unable to get port status: invalid data received")
                self.callBackWithStatus(.error)
                return
            }
            if probeString == "1" {
                self.callBackWithStatus(.open)
            } else if probeString == "0" {
                self.callBackWithStatus(.closed)
            } else {
                NSLog("Unable to get port status: invalid response (%@)", probeString)
                self.callBackWithStatus(.error)
            }
        })
        fTask?.resume()
    }

    private func callBackWithStatus(_ status: PortStatus) {
        fStatus = status
        fDelegate?.performSelector(onMainThread: #selector(PortCheckerDelegate.portCheckerDidFinishProbing(_:)),
                                   with: self,
                                   waitUntilDone: false)
    }
}
