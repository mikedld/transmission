This checklist helps to diagnose the most common reasons for bad speeds &mdash; or no speed at all &mdash; and tells where to find more help if the checklist does not find the problem.

Please go through the _entire checklist_ before asking for more help! Otherwise the forum helpers will likely just point you back here.

## 1. Is it your network?
If you are not sure whether the problem is a specific torrent or your network, it is easy to get a point of reference &mdash; add a fast torrent for benchmarking. Many users visit [the Ubuntu torrent page](https://torrent.ubuntu.com), scan down the page for the line with the most seeders and downloaders, and join the swarm for a few minutes to see what speeds they will reach.

More generally, you can also measure your bandwidth at [speedtest.net](https://www.speedtest.net/). However, this is not as good a BitTorrent benchmark as the previous approach.

## 2. Is your tracker responding?
Trackers are the primary source of finding other peers. When your tracker goes down, finding peers to share with is slower.

Look at the Tracker tab of the torrent dialog and see what it says about the last announce. Did the tracker respond "OK" or was there an error message?

## 3. Are there too many (or not enough) seeders?
In the inspector's tracker tab, you will see how many seeders and downloaders each tracker knows of.

If you are seeding and so are most of the other participants, your speeds will be slow because all the seeders are competing for the downloaders' limited bandwidth.

If you are downloading, things are a little better because you can download from everyone, not just seeders. But even so, it will still be slow if there are not enough seeders in the swarm. All the downloaders will get stuck at the same completion percentage as each other as they wait for more data to trickle down from the seeders. In practice this often looks like near-zero download speeds punctuated by bursts of fast downloading.

## 4. Is it your speed limits?
This falls into the are-you-sure-it-is-plugged-in category, but do not be embarrassed: lots of people have been bitten by this. Note: remember that there are both per-torrent and overall speed limits.

## 5. Is it a small swarm?
Even if you are the only downloader and there are four or five seeders ready to send you information, things can still be slow sometimes. Often what happens is the seeder's upload bandwidth is being shared between you and other peers in another torrent.

## 6. Are you uploading too fast?
If you try to upload to the limit of your connection bandwidth you may block your own downloads (which also use a little bit of upload bandwidth). It is best to limit uploads to no more than around 80% of your nominal upload bandwidth. Remember many network connections are asymmetric &mdash; which in practice means that upload speeds may only be a fraction of download speeds. If your upload bandwidth is say 25kB/s then a good value for torrent upload limits might be 20kB/s.

## 7. Is it your ISP?
If your ISP is one of those that manipulates BitTorrent packets &mdash; and even if it is not &mdash; it is often a good idea to enable the [Blocklist](./Blocklist.md) and also to tell Transmission to "Ignore Unencrypted Peers" to give your sessions slightly better privacy.

Update: Google now has [a free online tool](https://broadband.mpi-sws.org/transparency/bttest.php) to test what your ISP is doing. Follow that link and go down to the "Start testing" button.

## 8. Is it an old version of Transmission?
Work is constantly being done to improve performance and behavior. If you are using an old version, consider upgrading.

## 9. Is it your router or firewall making your "port closed"?
Connecting to a peer is like a telephone call: either you call up the peer, or the peer calls you. When Transmission says your "Port is Closed" it is like having a phone that does not allow incoming calls: you can still call peers, but they cannot call you.

Many people do not want to mess with their firewall and/or router, so they decide that dialing out is good enough and leave their port closed. Other people panic and worry too much about getting their port open even if they have a troublesome router. The truth is in the middle &mdash; you _can_ get by with a closed port, but on average you will get much faster speeds if peers can connect to you.

Opening a closed port is often the most frustrating task in BitTorrent. The good news is that the Transmission wiki has two pages dedicated to this topic: the [Port Forwarding Guide](Port-Forwarding-Guide.md) and the [Why is my port closed](Why-is-my-port-closed.md) page.

You can also test your port status at [canyouseeme.org](https://www.canyouseeme.org/).

## 10. Is it a Transmission Bug?
If you have looked at all the reasons above and none of them fit &mdash; Ubuntu downloaded quickly, and you got the latest version of Transmission, and there are plenty of seeds _and_ downloaders in your torrent, yet things are _still_ slow &mdash; then maybe you have found a Transmission bug. Go [read this post](https://forum.transmissionbt.com/viewtopic.php?f=1&t=3274) about what information the developers need to diagnose the problem, and then post a message describing your situation.

Make sure to give enough information! Vague bug reports waste everyone's time and will probably just get you referred back to this page.
