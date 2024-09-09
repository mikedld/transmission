It is not always possible to set all configurations from the GUI, especially for the daemon or the web interface. This guide will try to give an overview of how and what you can change. For the location of these files, look at the [Configuration Files](Configuration-Files.md) page.

Note: The client _should_ be closed before making changes, otherwise settings will be reverted to their previous state.

Some of Transmission's behavior can also be customized via environment variables.

# GTK / Daemon / CLI

> [!IMPORTANT]
> Since `4.1.0`, Transmission has switched to snake_case for all configurations.
> Transmission used to use kebab-case, and those are still accepted for backward compatibility,
> but they are deprecated and will be removed in `5.0.0`.
> 
> Your configuration file will be converted to snake_case automatically by running and stopping Transmission.
>
> For documentation of the old configurations, please consult documentation from previous versions.
> https://github.com/transmission/transmission/blob/4.0.6/docs/Editing-Configuration-Files.md

### Overview
GTK, CLI and daemon (both on a Mac and Linux) use a [JSON](https://www.json.org/) formatted file, mainly because of its human readability.
(Consult the [JSON website](https://www.json.org/) for detailed information)

### Reload Settings
You can make the daemon reload the settings file by sending it the `SIGHUP` signal.
Or simply run either of the following commands:
```console
$ killall -HUP transmission-daemon
```
Or:
```console
$ pkill -HUP transmission-daemon
```

### Formatting
Here is a sample of the three basic types: respectively Boolean, Number and String:

```json
{
    "rpc_enabled": true,
    "peer_port": 51413,
    "rpc-whitelist": "127.0.0.1,192.168.*.*"
}
```

### Options
#### IP Announce

 * **announce_ip:** String (default = "") Alternative IP address to announce to the tracker.
 * **announce_ip_enabled:** Boolean (default = false) When enabled `announce_ip` value is used instead of the client's address visible to the tracker for announcement requests.

#### Bandwidth

 * **alt_speed_enabled:** Boolean (default = false, aka 'Turtle Mode')
   _Note: Clicking the "Turtle" in the GUI when the [scheduler](#Scheduling) is enabled, will only temporarily remove the scheduled limit until the next cycle._
 * **alt_speed_up:** Number (KB/s, default = 50)
 * **alt_speed_down:** Number (KB/s, default = 50)
 * **speed_limit_down:** Number (KB/s, default = 100)
 * **speed_limit_down_enabled:** Boolean (default = false)
 * **speed_limit_up:** Number (KB/s, default = 100)
 * **speed_limit_up_enabled:** Boolean (default = false)
 * **upload_slots_per_torrent:** Number (default = 14)

#### [Blocklists](./Blocklists.md)

 * **blocklist_url:** String (default = https://www.example.com/blocklist)
 * **blocklist_enabled:** Boolean (default = false)

#### [Files and Locations](./Configuration-Files.md)

 * **download_dir:** String (default = [default locations](Configuration-Files.md#Locations))
 * **incomplete_dir:** String (default = [default locations](Configuration-Files.md#Locations)) Directory to keep files in until torrent is complete.
 * **incomplete_dir_enabled:** Boolean (default = false) When enabled, new torrents will download the files to `incomplete_dir`. When complete, the files will be moved to `download_dir`.
 * **preallocation:** Number (0 = Off, 1 = Fast, 2 = Full (slower but reduces disk fragmentation), default = 1)
 * **rename_partial_files:** Boolean (default = true) Postfix partially downloaded files with ".part".
 * **start_added_torrents:** Boolean (default = true) Start torrents as soon as they are added.
 * **trash_can_enabled:** Boolean (default = true) Whether to move the torrents to the system's trashcan or unlink them right away upon deletion from Transmission.
   _Note: transmission-gtk only._
 * **trash_original_torrent_files:** Boolean (default = false) Delete torrents added from the watch directory.
 * **umask:** String (default = "022") Sets Transmission's file mode creation mask. See [the umask(2) manpage](https://man7.org/linux/man-pages/man2/umask.2.html) for more information.
 * **watch_dir:** String
 * **watch_dir_enabled:** Boolean (default = false) Watch a directory for torrent files and add them to Transmission.
   _Note: When `watch_dir_enabled` is true, only the transmission-daemon, transmission-gtk, and transmission-qt applications will monitor `watch_dir` for new .torrent files and automatically load them._
 * **watch_dir_force_generic**: Boolean (default = false) Force to use a watch directory implementation that does not rely on OS-specific mechanisms. Useful when your watch directory is on a network location, such as CIFS or NFS.
   _Note: transmission-daemon only._

#### Misc
 * **cache_size_mb:** Number (default = 4), in megabytes, to allocate for Transmission's memory cache. The cache is used to help batch disk IO together, so increasing the cache size can be used to reduce the number of disk reads and writes. The value is the total available to the Transmission instance. Setting this to 0 bypasses the cache, which may be useful if your filesystem already has a cache layer that aggregates transactions.
 * **default_trackers:** String (default = "") A list of double-newline separated tracker announce URLs. These are used for all torrents in addition to the per torrent trackers specified in the torrent file. If a tracker is only meant to be a backup, it should be separated from its main tracker by a single newline character. If a tracker should be used additionally to another tracker it should be separated by two newlines. (e.g. "udp://tracker.example.invalid:1337/announce\n\nudp://tracker.another-example.invalid:6969/announce\nhttps://backup-tracker.another-example.invalid:443/announce\n\nudp://tracker.yet-another-example.invalid:1337/announce", in this case tracker.example.invalid, tracker.another-example.invalid and tracker.yet-another-example.invalid would be used as trackers and backup-tracker.another-example.invalid as backup in case tracker.another-example.invalid is unreachable.
 * **dht_enabled:** Boolean (default = true) Enable [Distributed Hash Table (DHT)](https://wiki.theory.org/BitTorrentSpecification#Distributed_Hash_Table).
 * **encryption:** Number (0 = Prefer unencrypted connections, 1 = Prefer encrypted connections, 2 = Require encrypted connections; default = 1) [Encryption](https://wiki.vuze.com/w/Message_Stream_Encryption) preference. Encryption may help get around some ISP filtering, but at the cost of slightly higher CPU use.
 * **lpd_enabled:** Boolean (default = false) Enable [Local Peer Discovery (LPD)](https://en.wikipedia.org/wiki/Local_Peer_Discovery).
 * **message_level:** Number (0 = None, 1 = Critical, 2 = Error, 3 = Warn, 4 = Info, 5 = Debug, 6 = Trace; default = 4) Set verbosity of Transmission's log messages.
 * **pex_enabled:** Boolean (default = true) Enable [Peer Exchange (PEX)](https://en.wikipedia.org/wiki/Peer_exchange).
 * **pidfile:** String Path to file in which daemon PID will be stored (_transmission-daemon only_)
 * **scrape_paused_torrents_enabled:** Boolean (default = true)
 * **script_torrent_added_enabled:** Boolean (default = false) Run a script when a torrent is added to Transmission. Environmental variables are passed in as detailed on the [Scripts](./Scripts.md) page.
 * **script_torrent_added_filename:** String (default = "") Path to script.
 * **script_torrent_done_enabled:** Boolean (default = false) Run a script when a torrent is done downloading. Environmental variables are passed in as detailed on the [Scripts](./Scripts.md) page.
 * **script_torrent_done_filename:** String (default = "") Path to script.
 * **script_torrent_done_seeding_enabled:** Boolean (default = false) Run a script when a torrent is done seeding. Environmental variables are passed in as detailed on the [Scripts](./Scripts.md) page.
 * **script_torrent_done_seeding_filename:** String (default = "") Path to script.
 * **start_paused**: Boolean (default = false) Pause the torrents when daemon starts. _Note: transmission-daemon only._
 * **tcp_enabled:** Boolean (default = true) Optionally disable TCP connection to other peers. Never disable TCP when you also disable µTP, because then your client would not be able to communicate. Disabling TCP might also break webseeds. Unless you have a good reason, you should not set this to false.
 * **torrent_added_verify_mode:** String ("fast", "full", default: "fast") Whether newly-added torrents' local data should be fully verified when added, or wait and verify them on-demand later. See [#2626](https://github.com/transmission/transmission/pull/2626) for more discussion.
 * **utp_enabled:** Boolean (default = true) Enable [Micro Transport Protocol (µTP)](https://en.wikipedia.org/wiki/Micro_Transport_Protocol)
 * **preferred_transport:** String ("utp" = Prefer µTP, "tcp" = Prefer TCP; default = "utp") Choose your preferred transport protocol (has no effect if one of them is disabled).
 * **sleep_per_seconds_during_verify:** Number (default = 100) Controls the duration in milliseconds for which the verification process will pause to reduce disk I/O pressure.

#### Peers
 * **bind_address_ipv4:** String (default = "") Where to listen for peer connections. When no valid IPv4 address is provided, Transmission will bind to "0.0.0.0".
 * **bind_address_ipv6:** String (default = "") Where to listen for peer connections. When no valid IPv6 address is provided, Transmission will try to bind to your default global IPv6 address. If that didn't work, then Transmission will bind to "::".
 * **peer_congestion_algorithm:** String. This is documented on https://www.pps.jussieu.fr/~jch/software/bittorrent/tcp-congestion-control.html.
 * **peer_limit_global:** Number (default = 200)
 * **peer_limit_per_torrent:** Number (default = 50)
 * **peer_socket_tos:** String (default = "le") Set the [DiffServ](https://en.wikipedia.org/wiki/Differentiated_services) parameter for outgoing packets. Allowed values are lowercase DSCP names. See the `tr_tos_t` class from `libtransmission/net.h` for the exact list of possible values.
 * **reqq:** Number (default = 2000) The number of outstanding block requests a peer is allowed to queue in the client. The higher this number, the higher the max possible upload speed towards each peer.

#### Peer Port
 * **peer_port:** Number (default = 51413)
 * **peer_port_random_high:** Number (default = 65535)
 * **peer_port_random_low:** Number (default = 1024)
 * **peer_port_random_on_start:** Boolean (default = false)
 * **port_forwarding_enabled:** Boolean (default = true) Enable [UPnP](https://en.wikipedia.org/wiki/Universal_Plug_and_Play) or [NAT-PMP](https://en.wikipedia.org/wiki/NAT_Port_Mapping_Protocol).

#### Queuing
 * **download_queue_enabled:** Boolean (default = true) When true, Transmission will only download `download_queue_size` non-stalled torrents at once.
 * **download_queue_size:** Number (default = 5) See `download_queue_enabled`.
 * **queue_stalled_enabled:** Boolean (default = true) When true, torrents that have not shared data for `queue_stalled_minutes` are treated as 'stalled' and are not counted against the `download_queue_size` and `seed_queue_size` limits.
 * **queue_stalled_minutes:** Number (default = 30) See `queue_stalled_enabled`.
 * **seed_queue_enabled:** Boolean (default = false) When true. Transmission will only seed `seed_queue_size` non-stalled torrents at once.
 * **seed_queue_size:** Number (default = 10) See `seed_queue_enabled`.

#### [RPC](rpc-spec.md)
 * **anti_brute_force_enabled:**: Boolean (default = false) Enable a very basic brute force protection for the RPC server. See "anti-brute-force-threshold" below.
 * **anti-brute-force-threshold:**: Number (default = 100) After this amount of failed authentication attempts is surpassed, the RPC server will deny any further authentication attempts until it is restarted. This is not tracked per IP but in total.
 * **rpc_authentication_required:** Boolean (default = false)
 * **rpc_bind_address:** String (default = "0.0.0.0") Where to listen for RPC connections
 * **rpc_enabled:** Boolean (default = true \[transmission-daemon\], false \[others\])
 * **rpc_host_whitelist:** String (Comma-delimited list of domain names. Wildcards allowed using '\*'. Example: "*.foo.org,example.com", Default: "", Always allowed: "localhost", "localhost.", all the IP addresses. Added in v2.93)
 * **rpc_host_whitelist_enabled:** Boolean (default = true. Added in v2.93)
 * **rpc_password:** String. You can enter this in as plaintext when Transmission is not running, and then Transmission will salt the value on startup and re-save the salted version as a security measure. **Note:** Transmission treats passwords starting with the character `{` as salted, so when you first create your password, the plaintext password you enter must not begin with `{`.
 * **rpc_port:** Number (default = 9091)
 * **rpc_socket_mode:** String UNIX filesystem mode for the RPC UNIX socket (default: 0750; used when `rpc_bind_address` is a UNIX socket)
 * **rpc_url:** String (default = /transmission/. Added in v2.2)
 * **rpc_username:** String
 * **rpc-whitelist:** String (Comma-delimited list of IP addresses. Wildcards allowed using '\*'. Example: "127.0.0.\*,192.168.\*.\*", Default:  "127.0.0.1")
 * **rpc-whitelist-enabled:** Boolean (default = true)

#### Scheduling
 * **alt_speed_time_enabled:** Boolean (default = false)
   _Note: When enabled, this will toggle the `alt_speed_enabled` setting._
 * **alt_speed_time_begin:** Number (default = 540, in minutes from midnight, 9am)
 * **alt_speed_time_end:** Number (default = 1020, in minutes from midnight, 5pm)
 * **alt_speed_time_day:** Number/bitfield (default = 127, all days)
   * Start with 0, then for each day you want the scheduler enabled, add:
     * **Sunday**: 1 (binary: `0000001`)
     * **Monday**: 2 (binary: `0000010`)
     * **Tuesday**: 4 (binary: `0000100`)
     * **Wednesday**: 8 (binary: `0001000`)
     * **Thursday**: 16 (binary: `0010000`)
     * **Friday**: 32 (binary: `0100000`)
     * **Saturday**: 64 (binary: `1000000`)
   * Examples:
     * **Weekdays**: 62 (binary: `0111110`)
     * **Weekends**: 65 (binary: `1000001`)
     * **All Days**: 127 (binary: `1111111`)
 * **idle_seeding_limit:** Number (default = 30) Stop seeding after being idle for _N_ minutes.
 * **idle_seeding_limit_enabled:** Boolean (default = false)
 * **ratio-limit:** Number (default = 2.0)
 * **ratio-limit-enabled:**  Boolean (default = false)

### Legacy Options
Only keys that differ from above are listed here. These options have been replaced in newer versions of Transmission.

#### 2.31 (and older)
 * **open-file-limit:** Number (default = 32)

#### 1.5x (and older)
##### Bandwidth
 * **download-limit:** Number (KB/s, default = 100)
 * **download-limit-enabled:** Boolean (default = false)
 * **upload-limit:** Number (KB/s, default = 100)
 * **upload-limit-enabled:** Boolean (default = false)

##### Peer Port
 * **peer-port-random-enabled:** Boolean (default = false)

#### 1.4x (and older)
##### Proxy
 * **proxy-authentication** String
 * **proxy-authentication-required:** Boolean (default = 0)
 * **proxy-port:** Number (default = 80)
 * **proxy-server:** String
 * **proxy-server-enabled:** Boolean (default = 0)
 * **proxy-type:** Number (0 = HTTP, 1 = SOCKS4, 2 = SOCKS5, default = 0)
 * **proxy-username:** String

##### Peers
 * **max-peers-global:** Number (default = 240)
 * **max-peers-per-torrent:** Number (default =  60)

#### 1.3x (and older)
##### [RPC](rpc-spec.md)
 * **rpc-access-control-list:** String (Comma-delimited list of IP addresses prefixed with "+" or "-". Wildcards allowed using '\*'. Example: "+127.0.0.\*,-192.168.\*.\*", Default:  "+127.0.0.1")

# macOS
### Overview
macOS has a standardized way of saving user preferences files using [XML](https://en.wikipedia.org/wiki/XML) format. These files are called [plist](https://en.wikipedia.org/wiki/Plist) (short for property list) files. Usually there is no need to modify these files directly, since Apple provided a [command-line tool](https://developer.apple.com/DOCUMENTATION/Darwin/Reference/ManPages/man1/defaults.1.html) to reliably change settings. You do need to restart Transmission before these have effect.

In short:
 * To set a key: `defaults write org.m0k.transmission <key> <value>`
 * To reset a key: `defaults delete org.m0k.transmission <key>`

### Options
 * **PeerSocketTOS:** Number (Default = 0)
 * **RPCHostWhitelist:** String, see `rpc_host_whitelist` above.
 * **RPCUseHostWhitelist:** Boolean, see `rpc_host_whitelist_enabled` above.
