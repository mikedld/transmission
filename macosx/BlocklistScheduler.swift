// This file Copyright © 2008-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import Foundation

class BlocklistScheduler: NSObject {
    // thirty second delay before running after option is changed
    private static let kSmallDelay: TimeInterval = 30

    // update one week after previous update
    private static let kFullWait: TimeInterval = 60 * 60 * 24 * 7

    @objc static let scheduler = BlocklistScheduler()

    private var fTimer: Timer?

    @objc
    func updateSchedule() {
        if BlocklistDownloader.isRunning {
            return
        }

        cancelSchedule()

        if !UserDefaults.standard.bool(forKey: "BlocklistNew") ||
            UserDefaults.standard.string(forKey: "BlocklistURL")?.isEmpty != false ||
            !UserDefaults.standard.bool(forKey: "BlocklistAutoUpdate") {
            return
        }

        let lastUpdateDate = (UserDefaults.standard.object(forKey: "BlocklistNewLastUpdate") as? Date)?.addingTimeInterval(Self.kFullWait)
        let closeDate = Date(timeIntervalSinceNow: Self.kSmallDelay)

        let useDate = max(lastUpdateDate ?? closeDate, closeDate)

        let timer = Timer(fireAt: useDate,
                          interval: 0,
                          target: self,
                          selector: #selector(runUpdater),
                          userInfo: nil,
                          repeats: false)
        fTimer = timer

        // current run loop usually means a second update won't work
        let loop = RunLoop.main
        loop.add(timer, forMode: .default)
        loop.add(timer, forMode: .modalPanel)
        loop.add(timer, forMode: .eventTracking)
    }

    @objc
    func cancelSchedule() {
        fTimer?.invalidate()
        fTimer = nil
    }

    // MARK: - Private

    @objc
    private func runUpdater() {
        fTimer = nil
        _ = BlocklistDownloader.downloader()
    }
}
