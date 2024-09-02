# Headless Usage

Transmission can be run in headless mode as a daemon. The daemon, `transmission-daemon`, runs in the background. It communicates with the world via the usual TCP and UDP bittorrent ports, as well as through TCP port 9091. It's on this last port that the daemon accepts commands from `transmission-remote`. These commands are called Remote Procedure Calls, or RPCs. There are RPCs for all of the interactions between a user and a bittorrent client, including adding and removing torrents, displaying global stats, checking torrent download progress, and shutting down the daemon. Each command completes in a finite amount of time and prints the result to stdout.

## Setup

First, the daemon must be started:
```
> mkdir -p ~/.local/log
> transmission-daemon --logfile ~/.local/log/transmission.log
> cat ~/.local/log/transmission.log
[2022-05-02 17:28:13.652] Transmission 3.00 (bb6b5a062e) started (session.c:769)
[2022-05-02 17:28:13.652] RPC Server Adding address to whitelist: 127.0.0.1 (rpc-server.c:956)
[2022-05-02 17:28:13.652] RPC Server Adding address to whitelist: ::1 (rpc-server.c:956)
[2022-05-02 17:28:13.652] RPC Server Serving RPC and Web requests on 127.0.0.1:9091/transmission/(rpc-server.c:1243)
[2022-05-02 17:28:13.652] RPC Server Whitelist enabled (rpc-server.c:1249)
[2022-05-02 17:28:13.652] UDP Failed to set receive buffer: requested 4194304, got 425984 (tr-udp.c:97)
[2022-05-02 17:28:13.653] UDP Please add the line "net.core.rmem_max = 4194304" to /etc/sysctl.conf (tr-udp.c:99)
[2022-05-02 17:28:13.653] UDP Failed to set send buffer: requested 1048576, got 425984 (tr-udp.c:105)
[2022-05-02 17:28:13.653] UDP Please add the line "net.core.wmem_max = 1048576" to /etc/sysctl.conf(tr-udp.c:107)
[2022-05-02 17:28:13.653] DHT Reusing old id (tr-dht.c:383)
[2022-05-02 17:28:13.653] DHT Bootstrapping from 300 IPv4 nodes (tr-dht.c:172)
[2022-05-02 17:28:13.653] Using settings from "/home/<your user>/.config/transmission-daemon" (daemon.c:646)
[2022-05-02 17:28:13.653] Saved "/home/<your user>/.config/transmission-daemon/settings.json" (variant.c:1221)
[2022-05-02 17:28:13.653] Loaded 0 torrents (session.c:2170)
[2022-05-02 17:28:13.653] Port Forwarding (NAT-PMP) initnatpmp succeeded (0) (natpmp.c:73)
[2022-05-02 17:28:13.653] Port Forwarding (NAT-PMP) sendpublicaddressrequest succeeded (2) (natpmp.c:73)
[2022-05-02 17:28:21.655] Port Forwarding State changed from "Not forwarded" to "Starting"(port-forwarding.c:106)
[2022-05-02 17:28:21.655] Port Forwarding State changed from "Starting" to "???" (port-forwarding.c:106)
[2022-05-02 17:28:21.655] web will verify tracker certs using envvar CURL_CA_BUNDLE: none (web.c:455)
[2022-05-02 17:28:21.655] web NB: this only works if you built against libcurl with openssl or gnutls, NOT nss(web.c:457)
[2022-05-02 17:28:21.655] web NB: invalid certs will show up as 'Could not connect to tracker' like many othererrors (web.c:458)
> 
```
The daemon forks into the background immediately.

The daemon automatically makes a config file at `.config/transmission-daemon/settings.json`. It can be edited, but the daemon needs to be stopped first, because the daemon automatically overwrites this file when it exits. To stop the daemon:
```
> transmission-remote --exit
localhost:9091/transmission/rpc/ responded: "success"
> 
```

Now, the config file can be edited. Some settings to draw your attention to:
```plain:
~/.config/transmission-daemon/settings.json:
-------------------------------------------------------------------------------
{
    ...
    # The default download directory.
    "download_dir": "/home/youruser/Downloads",
    ...
    # Reduce the global peer limit if your router is low on memory.
    "peer_limit_global": 32,
    ...
    # For manual port forwarding. This controls the TCP and UDP bittorrent ports.
    "peer-port": 32768,
    ...
    # Reject RPC commands from anybody except localhost. For untrusted LANs.
    "rpc-bind-address": "127.0.0.1",
    ...
    # If there are problems with other internet applications while bittorrent
    # is running, you may need to reduce the max upload speed to under 80% of
    # your ISP's max upload speed.
    "speed-limit-up": 100,  # in KB/s
    "speed-limit-up-enabled": true,
    ...
}
```

Once the daemon is configured to your liking, start it back up.
```
> transmission-daemon --logfile ~/.local/log/transmission.log
> 
```
Headless Transmission is now configured.

## Usage
To add a torrent:
```
> transmission-remote --add 'magnet:?xt=urn:btih:ff30c7c268f33f35248f05c1c4ffed6752393157&dn=archl
inux-2022.04.05-x86_64.iso'
localhost:9091/transmission/rpc/ responded: "success"
> 
```
To list all torrents:
```
> transmission-remote --list
    ID   Done       Have  ETA           Up    Down  Ratio  Status       Name
     1    41%   359.8 MB  3 min        0.0  2796.0    0.0  Downloading  archlinux-2022.04.05-x86_64.iso
Sum:            359.8 MB  
```
For session info:
```
> transmission-remote --session-info
VERSION
  Daemon version: 3.00 (bb6b5a062e)
  RPC version: 16
  RPC minimum version: 1

CONFIG
  Configuration directory: /home/<your user>/.config/transmission-daemon
  Download directory: /home/<your user>/Downloads
  Listenport: 40000
  Portforwarding enabled: Yes
  uTP enabled: Yes
  Distributed hash table enabled: Yes
  Local peer discovery enabled: No
  Peer exchange allowed: Yes
  Encryption: preferred
  Maximum memory cache size: 4.00 MiB

LIMITS
  Peer limit: 200
  Default seed ratio limit: Unlimited
  Upload speed limit: Unlimited (Disabled limit: 100 kB/s; Disabled turtle limit: 50 kB/s)
  Download speed limit: Unlimited (Disabled limit: 100 kB/s; Disabled turtle limit: 50 kB/s)

MISC
  Autostart added torrents: Yes
  Delete automatically added torrents: No
> 
```
For session stats:
```
> transmission-remote --session-stats

CURRENT SESSION
  Uploaded:   None
  Downloaded: None
  Ratio:      None
  Duration:   11 seconds (11 seconds)

TOTAL
  Started 4 times
  Uploaded:   4.52 GB
  Downloaded: 1.83 GB
  Ratio:      2.4
  Duration:   3 days, 12 hours (305174 seconds)
> 
```

### Selecting torrents
RPCs like `--remove` and `--stop` require a torrent to be specified with `--torrent <ID>`, where `<ID>` refers to a torrent ID in the output of the `--list` RPC. Here's an example of pausing, resuming, and removing a torrent.
```
> transmission-remote --list
    ID   Done       Have  ETA           Up    Down  Ratio  Status       Name
     1   100%   959.4 MB  Done         0.0     0.0    9.4  Idle         Warp Records - Artificial Intelligence (The Series)
     2   100%   864.4 MB  Done         0.0     0.0    0.7  Idle         archlinux-2022.04.05-x86_64.iso
Sum:             1.82 GB               0.0     0.0
> transmission-remote --torrent 2 --stop
localhost:9091/transmission/rpc/ responded: "success"
> transmission-remote --list
    ID   Done       Have  ETA           Up    Down  Ratio  Status       Name
     1   100%   959.4 MB  Done         0.0     0.0    9.4  Idle         Warp Records - Artificial Intelligence (The Series)
     2   100%   864.4 MB  Done         0.0     0.0    0.7  Stopped      archlinux-2022.04.05-x86_64.iso
Sum:             1.82 GB               0.0     0.0
> transmission-remote --torrent 2 --start
localhost:9091/transmission/rpc/ responded: "success"
> transmission-remote --list
    ID   Done       Have  ETA           Up    Down  Ratio  Status       Name
     1   100%   959.4 MB  Done         0.0     0.0    9.4  Idle         Warp Records - Artificial Intelligence (The Series)
     2   100%   864.4 MB  Done         0.0     0.0    0.7  Idle         archlinux-2022.04.05-x86_64.iso
Sum:             1.82 GB               0.0     0.0
> transmission-remote --torrent 2 --remove
localhost:9091/transmission/rpc/ responded: "success"
> transmission-remote --list
    ID   Done       Have  ETA           Up    Down  Ratio  Status       Name
     1   100%   959.4 MB  Done         0.0     0.0    9.4  Idle         Warp Records - Artificial Intelligence (The Series)
Sum:            959.4 MB               0.0     0.0
> 
```

### Everything else
For all other RPCs, see the [RPC Specification](rpc-spec.md) page. For more daemon options, see `transmission-daemon`'s man page. For instructions on autostart, i.e. system integration, see your distro's documentation. For most linux distributions, you'll probably want to see systemd's documentation.
