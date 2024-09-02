Transmission keeps working information on each torrent in a "resume" file. This file is stored in the 'resume' directory.

Filename: `<hash?>.resume`

The file contains the following **per-torrent** properties:

| Property | Description |
| :-- | :-- |
| `activity_date` | Date we last uploaded/downloaded a piece of data |
| `added_date` | Date torrent was added |
| `corrupt` | total number of corrupt bytes downloaded |
| `done_date` | Date torrent finished downloading |
| `destination` | Download directory |
| `dnd` | Do not download file integer list (one item per file in torrent) 0=download, 1=dnd |
| `downloaded` | Total non-corrupt bytes downloaded |
| `incomplete_dir` | Location of incomplete torrent files |
| `max-peers` | Maximum number of connected peers |
| `paused` | true if torrent is paused |
| `peers2` | IPv4 peers |
| `peers2-6` | IPv6 peers |
| `priority` | list of file download priorities (one item per file in torrent),<br/>each value is -1 (low), 0 (std), +1 (high) |
| `bandwidth_priority` |  |
| `progress` |  |
| `speed-limit` |  |
| `speed-limit-up` | Torrent upload speed limit |
| `speed_limit_down` | Torrent download speed limit |
| `ratio-limit` | Torrent file limit |
| `uploaded` |  |
| `speed` |  |
| `use-global-speed-limit` |  |
| `use-speed-limit` |  |
| `down-speed` |  |
| `down-mode` |  |
| `up-speed` |  |
| `up-mode` |  |
| `ratio-mode` |  |
| `mtimes` |  |
| `bitfield` |  |

The file format is bencoding, as described in [bep_0003](https://www.bittorrent.org/beps/bep_0003.html).

## Constants

| Maximum number of remembered peers | `MAX_REMEMBERED_PEERS` | 200 |
