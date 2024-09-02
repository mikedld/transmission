// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#define TR_NAME "Transmission"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef> // size_t
#include <cstdint> // uintX_t
#include <ctime> // time_t
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility> // for std::pair
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/socket.h> // socklen_t
#endif

#include <event2/util.h> // for evutil_socket_t

#include "libtransmission/transmission.h"

#include "libtransmission/announce-list.h"
#include "libtransmission/announcer.h"
#include "libtransmission/bandwidth.h"
#include "libtransmission/blocklist.h"
#include "libtransmission/cache.h"
#include "libtransmission/interned-string.h"
#include "libtransmission/ip-cache.h"
#include "libtransmission/log.h" // for tr_log_level
#include "libtransmission/net.h" // for tr_port, tr_tos_t
#include "libtransmission/open-files.h"
#include "libtransmission/peer-io.h" // tr_preferred_transport
#include "libtransmission/port-forwarding.h"
#include "libtransmission/quark.h"
#include "libtransmission/rpc-server.h"
#include "libtransmission/session-alt-speeds.h"
#include "libtransmission/session-id.h"
#include "libtransmission/session-thread.h"
#include "libtransmission/settings.h"
#include "libtransmission/stats.h"
#include "libtransmission/timer.h"
#include "libtransmission/torrents.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-dht.h"
#include "libtransmission/tr-lpd.h"
#include "libtransmission/tr-macros.h"
#include "libtransmission/utils-ev.h"
#include "libtransmission/verify.h"
#include "libtransmission/web.h"

tr_peer_id_t tr_peerIdInit();

class tr_peer_socket;
struct tr_pex;
struct tr_torrent;
struct struct_utp_context;
struct tr_variant;

namespace libtransmission::test
{

class SessionTest;

} // namespace libtransmission::test

/** @brief handle to an active libtransmission session */
struct tr_session
{
    using Memory = libtransmission::Values::Memory;
    using Speed = libtransmission::Values::Speed;

private:
    class BoundSocket
    {
    public:
        using IncomingCallback = void (*)(tr_socket_t, void*);
        BoundSocket(struct event_base* base, tr_address const& addr, tr_port port, IncomingCallback cb, void* cb_data);
        BoundSocket(BoundSocket&&) = delete;
        BoundSocket(BoundSocket const&) = delete;
        BoundSocket operator=(BoundSocket&&) = delete;
        BoundSocket operator=(BoundSocket const&) = delete;
        ~BoundSocket();

    private:
        static void onCanRead(evutil_socket_t fd, short /*what*/, void* vself)
        {
            auto* const self = static_cast<BoundSocket*>(vself);
            self->cb_(fd, self->cb_data_);
        }

        IncomingCallback cb_;
        void* cb_data_;
        tr_socket_t socket_ = TR_BAD_SOCKET;
        libtransmission::evhelpers::event_unique_ptr ev_;
    };

    class AltSpeedMediator final : public tr_session_alt_speeds::Mediator
    {
    public:
        explicit AltSpeedMediator(tr_session& session) noexcept
            : session_{ session }
        {
        }

        void is_active_changed(bool is_active, tr_session_alt_speeds::ChangeReason reason) override;

        [[nodiscard]] time_t time() override;

        ~AltSpeedMediator() noexcept override = default;

    private:
        tr_session& session_;
    };

    class AnnouncerUdpMediator final : public tr_announcer_udp::Mediator
    {
    public:
        explicit AnnouncerUdpMediator(tr_session& session) noexcept
            : session_{ session }
        {
        }

        ~AnnouncerUdpMediator() noexcept override = default;

        void sendto(void const* buf, size_t buflen, sockaddr const* addr, socklen_t addrlen) override
        {
            session_.udp_core_->sendto(buf, buflen, addr, addrlen);
        }

        [[nodiscard]] std::optional<tr_address> announce_ip() const override
        {
            if (!session_.useAnnounceIP())
            {
                return {};
            }

            return tr_address::from_string(session_.announceIP());
        }

    private:
        tr_session& session_;
    };

    class DhtMediator : public tr_dht::Mediator
    {
    public:
        DhtMediator(tr_session& session) noexcept
            : session_{ session }
        {
        }

        ~DhtMediator() noexcept override = default;

        [[nodiscard]] std::vector<tr_torrent_id_t> torrents_allowing_dht() const override;

        [[nodiscard]] tr_sha1_digest_t torrent_info_hash(tr_torrent_id_t id) const override;

        [[nodiscard]] std::string_view config_dir() const override
        {
            return session_.config_dir_;
        }

        [[nodiscard]] libtransmission::TimerMaker& timer_maker() override
        {
            return session_.timerMaker();
        }

        void add_pex(tr_sha1_digest_t const&, tr_pex const* pex, size_t n_pex) override;

    private:
        tr_session& session_;
    };

    class PortForwardingMediator final : public tr_port_forwarding::Mediator
    {
    public:
        explicit PortForwardingMediator(tr_session& session) noexcept
            : session_{ session }
        {
        }

        [[nodiscard]] tr_address incoming_peer_address() const override
        {
            return session_.bind_address(TR_AF_INET);
        }

        [[nodiscard]] tr_port advertised_peer_port() const override
        {
            return session_.advertisedPeerPort();
        }

        [[nodiscard]] tr_port local_peer_port() const override
        {
            return session_.localPeerPort();
        }

        [[nodiscard]] libtransmission::TimerMaker& timer_maker() override
        {
            return session_.timerMaker();
        }

        void on_port_forwarded(tr_port public_port) override
        {
            if (session_.advertised_peer_port_ != public_port)
            {
                session_.advertised_peer_port_ = public_port;
                session_.onAdvertisedPeerPortChanged();
            }
        }

    private:
        tr_session& session_;
    };

    class WebMediator final : public tr_web::Mediator
    {
    public:
        explicit WebMediator(tr_session* session) noexcept
            : session_{ session }
        {
        }

        [[nodiscard]] std::optional<std::string> cookieFile() const override;
        [[nodiscard]] std::optional<std::string> bind_address_V4() const override;
        [[nodiscard]] std::optional<std::string> bind_address_V6() const override;
        [[nodiscard]] std::optional<std::string_view> userAgent() const override;
        [[nodiscard]] size_t clamp(int torrent_id, size_t byte_count) const override;
        [[nodiscard]] time_t now() const override;
        // runs the tr_web::fetch response callback in the libtransmission thread
        void run(tr_web::FetchDoneFunc&& func, tr_web::FetchResponse&& response) const override;

    private:
        tr_session* const session_;
    };

    class LpdMediator final : public tr_lpd::Mediator
    {
    public:
        explicit LpdMediator(tr_session& session) noexcept
            : session_{ session }
        {
        }

        [[nodiscard]] tr_address bind_address(tr_address_type type) const override
        {
            return session_.bind_address(type);
        }

        [[nodiscard]] tr_port port() const override
        {
            return session_.advertisedPeerPort();
        }

        [[nodiscard]] bool allowsLPD() const override
        {
            return session_.allowsLPD();
        }

        [[nodiscard]] libtransmission::TimerMaker& timerMaker() override
        {
            return session_.timerMaker();
        }

        [[nodiscard]] std::vector<TorrentInfo> torrents() const override;

        bool onPeerFound(std::string_view info_hash_str, tr_address address, tr_port port) override;

        void setNextAnnounceTime(std::string_view info_hash_str, time_t announce_after) override;

    private:
        tr_session& session_;
    };

    class IPCacheMediator final : public tr_ip_cache::Mediator
    {
    public:
        explicit IPCacheMediator(tr_session& session) noexcept
            : session_{ session }
        {
        }

        void fetch(tr_web::FetchOptions&& options) override
        {
            session_.fetch(std::move(options));
        }

        [[nodiscard]] std::string_view settings_bind_addr(tr_address_type type) override
        {
            switch (type)
            {
            case TR_AF_INET:
                return session_.settings_.bind_address_ipv4;
            case TR_AF_INET6:
                return session_.settings_.bind_address_ipv6;
            default:
                TR_ASSERT_MSG(false, "Invalid type");
                return {};
            }
        }

        [[nodiscard]] libtransmission::TimerMaker& timer_maker() override
        {
            return session_.timerMaker();
        }

    private:
        tr_session& session_;
    };

    // UDP connectivity used for the DHT and µTP
    class tr_udp_core
    {
    public:
        tr_udp_core(tr_session& session, tr_port udp_port);
        ~tr_udp_core();

        void sendto(void const* buf, size_t buflen, struct sockaddr const* to, socklen_t tolen) const;

        [[nodiscard]] constexpr auto socket4() const noexcept
        {
            return udp4_socket_;
        }

        [[nodiscard]] constexpr auto socket6() const noexcept
        {
            return udp6_socket_;
        }

    private:
        tr_port const udp_port_;
        tr_session& session_;
        tr_socket_t udp4_socket_ = TR_BAD_SOCKET;
        tr_socket_t udp6_socket_ = TR_BAD_SOCKET;
        libtransmission::evhelpers::event_unique_ptr udp4_event_;
        libtransmission::evhelpers::event_unique_ptr udp6_event_;
    };

public:
    struct Settings final : public libtransmission::Settings
    {
    public:
        Settings() = default;

        explicit Settings(tr_variant const& src)
        {
            load(src);
        }

        // NB: When adding a field here, you must also add it to
        // fields() if you want it to be in session-settings.json
        bool announce_ip_enabled = false;
        bool blocklist_enabled = false;
        bool dht_enabled = true;
        bool download_queue_enabled = true;
        bool idle_seeding_limit_enabled = false;
        bool incomplete_dir_enabled = false;
        bool is_incomplete_file_naming_enabled = true;
        bool lpd_enabled = true;
        bool peer_port_random_on_start = false;
        bool pex_enabled = true;
        bool port_forwarding_enabled = true;
        bool queue_stalled_enabled = true;
        bool ratio_limit_enabled = false;
        bool script_torrent_added_enabled = false;
        bool script_torrent_done_enabled = false;
        bool script_torrent_done_seeding_enabled = false;
        bool seed_queue_enabled = false;
        bool should_delete_source_torrents = false;
        bool should_scrape_paused_torrents = true;
        bool should_start_added_torrents = true;
        bool speed_limit_down_enabled = false;
        bool speed_limit_up_enabled = false;
        bool tcp_enabled = true;
        bool utp_enabled = true;
        double ratio_limit = 2.0;
        size_t cache_size_mbytes = 4U;
        size_t download_queue_size = 5U;
        size_t idle_seeding_limit_minutes = 30U;
        size_t peer_limit_global = TR_DEFAULT_PEER_LIMIT_GLOBAL;
        size_t peer_limit_per_torrent = TR_DEFAULT_PEER_LIMIT_TORRENT;
        size_t queue_stalled_minutes = 30U;
        size_t reqq = 2000U;
        size_t seed_queue_size = 10U;
        size_t speed_limit_down = 100U;
        size_t speed_limit_up = 100U;
        size_t upload_slots_per_torrent = 8U;
        std::chrono::milliseconds sleep_per_seconds_during_verify = std::chrono::milliseconds{ 100 };
        std::string announce_ip;
        std::string bind_address_ipv4;
        std::string bind_address_ipv6;
        std::string blocklist_url = "http://www.example.com/blocklist";
        std::string default_trackers_str;
        std::string download_dir = tr_getDefaultDownloadDir();
        std::string incomplete_dir = tr_getDefaultDownloadDir();
        std::string peer_congestion_algorithm;
        std::string script_torrent_added_filename;
        std::string script_torrent_done_filename;
        std::string script_torrent_done_seeding_filename;
        tr_encryption_mode encryption_mode = TR_ENCRYPTION_PREFERRED;
        tr_log_level log_level = TR_LOG_INFO;
        tr_mode_t umask = 022;
        tr_open_files::Preallocation preallocation_mode = tr_open_files::Preallocation::Sparse;
        tr_port peer_port_random_high = tr_port::from_host(65535);
        tr_port peer_port_random_low = tr_port::from_host(49152);
        tr_port peer_port = tr_port::from_host(TR_DEFAULT_PEER_PORT);
        tr_preferred_transport preferred_transport = TR_PREFER_UTP;
        tr_tos_t peer_socket_tos{ 0x04 };
        tr_verify_added_mode torrent_added_verify_mode = TR_VERIFY_ADDED_FAST;

    private:
        [[nodiscard]] Fields fields() override
        {
            return {
                { TR_KEY_announce_ip, &announce_ip },
                { TR_KEY_announce_ip_enabled, &announce_ip_enabled },
                { TR_KEY_bind_address_ipv4, &bind_address_ipv4 },
                { TR_KEY_bind_address_ipv6, &bind_address_ipv6 },
                { TR_KEY_blocklist_enabled, &blocklist_enabled },
                { TR_KEY_blocklist_url, &blocklist_url },
                { TR_KEY_cache_size_mb, &cache_size_mbytes },
                { TR_KEY_default_trackers, &default_trackers_str },
                { TR_KEY_dht_enabled, &dht_enabled },
                { TR_KEY_download_dir, &download_dir },
                { TR_KEY_download_queue_enabled, &download_queue_enabled },
                { TR_KEY_download_queue_size, &download_queue_size },
                { TR_KEY_encryption, &encryption_mode },
                { TR_KEY_idle_seeding_limit, &idle_seeding_limit_minutes },
                { TR_KEY_idle_seeding_limit_enabled, &idle_seeding_limit_enabled },
                { TR_KEY_incomplete_dir, &incomplete_dir },
                { TR_KEY_incomplete_dir_enabled, &incomplete_dir_enabled },
                { TR_KEY_lpd_enabled, &lpd_enabled },
                { TR_KEY_message_level, &log_level },
                { TR_KEY_peer_congestion_algorithm, &peer_congestion_algorithm },
                { TR_KEY_peer_limit_global, &peer_limit_global },
                { TR_KEY_peer_limit_per_torrent, &peer_limit_per_torrent },
                { TR_KEY_peer_port, &peer_port },
                { TR_KEY_peer_port_random_high, &peer_port_random_high },
                { TR_KEY_peer_port_random_low, &peer_port_random_low },
                { TR_KEY_peer_port_random_on_start, &peer_port_random_on_start },
                { TR_KEY_peer_socket_tos, &peer_socket_tos },
                { TR_KEY_pex_enabled, &pex_enabled },
                { TR_KEY_port_forwarding_enabled, &port_forwarding_enabled },
                { TR_KEY_preallocation, &preallocation_mode },
                { TR_KEY_preferred_transport, &preferred_transport },
                { TR_KEY_queue_stalled_enabled, &queue_stalled_enabled },
                { TR_KEY_queue_stalled_minutes, &queue_stalled_minutes },
                { TR_KEY_ratio_limit, &ratio_limit },
                { TR_KEY_ratio_limit_enabled, &ratio_limit_enabled },
                { TR_KEY_rename_partial_files, &is_incomplete_file_naming_enabled },
                { TR_KEY_reqq, &reqq },
                { TR_KEY_scrape_paused_torrents_enabled, &should_scrape_paused_torrents },
                { TR_KEY_script_torrent_added_enabled, &script_torrent_added_enabled },
                { TR_KEY_script_torrent_added_filename, &script_torrent_added_filename },
                { TR_KEY_script_torrent_done_enabled, &script_torrent_done_enabled },
                { TR_KEY_script_torrent_done_filename, &script_torrent_done_filename },
                { TR_KEY_script_torrent_done_seeding_enabled, &script_torrent_done_seeding_enabled },
                { TR_KEY_script_torrent_done_seeding_filename, &script_torrent_done_seeding_filename },
                { TR_KEY_seed_queue_enabled, &seed_queue_enabled },
                { TR_KEY_seed_queue_size, &seed_queue_size },
                { TR_KEY_sleep_per_seconds_during_verify, &sleep_per_seconds_during_verify },
                { TR_KEY_speed_limit_down, &speed_limit_down },
                { TR_KEY_speed_limit_down_enabled, &speed_limit_down_enabled },
                { TR_KEY_speed_limit_up, &speed_limit_up },
                { TR_KEY_speed_limit_up_enabled, &speed_limit_up_enabled },
                { TR_KEY_start_added_torrents, &should_start_added_torrents },
                { TR_KEY_tcp_enabled, &tcp_enabled },
                { TR_KEY_torrent_added_verify_mode, &torrent_added_verify_mode },
                { TR_KEY_trash_original_torrent_files, &should_delete_source_torrents },
                { TR_KEY_umask, &umask },
                { TR_KEY_upload_slots_per_torrent, &upload_slots_per_torrent },
                { TR_KEY_utp_enabled, &utp_enabled },
            };
        }
    };

    explicit tr_session(std::string_view config_dir, tr_variant const& settings_dict);

    [[nodiscard]] std::string_view sessionId() const noexcept
    {
        return session_id_.sv();
    }

    [[nodiscard]] libtransmission::TimerMaker& timerMaker() noexcept
    {
        return *timer_maker_;
    }

    [[nodiscard]] auto am_in_session_thread() const noexcept
    {
        return session_thread_->am_in_session_thread();
    }

    template<typename Func, typename... Args>
    void queue_session_thread(Func&& func, Args&&... args)
    {
        session_thread_->queue(std::forward<Func>(func), std::forward<Args>(args)...);
    }

    template<typename Func, typename... Args>
    void run_in_session_thread(Func&& func, Args&&... args)
    {
        session_thread_->run(std::forward<Func>(func), std::forward<Args>(args)...);
    }

    [[nodiscard]] auto* event_base() noexcept
    {
        return session_thread_->event_base();
    }

    [[nodiscard]] constexpr auto& torrents()
    {
        return torrents_;
    }

    [[nodiscard]] constexpr auto const& torrents() const
    {
        return torrents_;
    }

    [[nodiscard]] auto unique_lock() const
    {
        return std::unique_lock(session_mutex_);
    }

    [[nodiscard]] constexpr auto const& settings() const noexcept
    {
        return settings_;
    }

    // paths

    [[nodiscard]] constexpr auto const& configDir() const noexcept
    {
        return config_dir_;
    }

    [[nodiscard]] constexpr auto const& torrentDir() const noexcept
    {
        return torrent_dir_;
    }

    [[nodiscard]] constexpr auto const& resumeDir() const noexcept
    {
        return resume_dir_;
    }

    [[nodiscard]] constexpr auto const& downloadDir() const noexcept
    {
        return settings().download_dir;
    }

    void setDownloadDir(std::string_view dir)
    {
        settings_.download_dir = dir;
    }

    // default trackers
    // (trackers to apply automatically to public torrents)

    [[nodiscard]] constexpr auto const& defaultTrackersStr() const noexcept
    {
        return settings().default_trackers_str;
    }

    [[nodiscard]] constexpr auto const& defaultTrackers() const noexcept
    {
        return default_trackers_;
    }

    void setDefaultTrackers(std::string_view trackers);

    // incomplete dir

    [[nodiscard]] constexpr auto const& incompleteDir() const noexcept
    {
        return settings().incomplete_dir;
    }

    void setIncompleteDir(std::string_view dir)
    {
        settings_.incomplete_dir = dir;
    }

    [[nodiscard]] constexpr auto useIncompleteDir() const noexcept
    {
        return settings().incomplete_dir_enabled;
    }

    constexpr void useIncompleteDir(bool enabled) noexcept
    {
        settings_.incomplete_dir_enabled = enabled;
    }

    // scripts

    constexpr void useScript(TrScript i, bool enabled)
    {
        scriptEnabledFlag(i) = enabled;
    }

    [[nodiscard]] constexpr bool useScript(TrScript i) const
    {
        return const_cast<tr_session*>(this)->scriptEnabledFlag(i);
    }

    void setScript(TrScript i, std::string_view path)
    {
        scriptFilename(i) = path;
    }

    [[nodiscard]] constexpr auto const& script(TrScript i) const
    {
        return const_cast<tr_session*>(this)->scriptFilename(i);
    }

    // blocklist

    [[nodiscard]] constexpr auto& blocklist() noexcept
    {
        return blocklists_;
    }

    void set_blocklist_enabled(bool is_enabled)
    {
        settings_.blocklist_enabled = is_enabled;
        blocklist().set_enabled(is_enabled);
    }

    [[nodiscard]] auto blocklist_enabled() const noexcept
    {
        return settings().blocklist_enabled;
    }

    [[nodiscard]] constexpr auto const& blocklistUrl() const noexcept
    {
        return settings().blocklist_url;
    }

    void setBlocklistUrl(std::string_view url)
    {
        settings_.blocklist_url = url;
    }

    // RPC

    void setRpcWhitelist(std::string_view whitelist) const;

    void useRpcWhitelist(bool enabled) const;

    [[nodiscard]] bool useRpcWhitelist() const;

    // peer networking

    [[nodiscard]] constexpr auto const& peerCongestionAlgorithm() const noexcept
    {
        return settings().peer_congestion_algorithm;
    }

    void setPeerCongestionAlgorithm(std::string_view algorithm)
    {
        settings_.peer_congestion_algorithm = algorithm;
    }

    void setSocketTOS(tr_socket_t sock, tr_address_type type) const
    {
        tr_netSetTOS(sock, settings_.peer_socket_tos, type);
    }

    [[nodiscard]] constexpr auto peerLimit() const noexcept
    {
        return settings().peer_limit_global;
    }

    [[nodiscard]] constexpr auto peerLimitPerTorrent() const noexcept
    {
        return settings().peer_limit_per_torrent;
    }

    [[nodiscard]] constexpr auto reqq() const noexcept
    {
        return settings().reqq;
    }

    constexpr void set_reqq(size_t reqq) noexcept
    {
        settings_.reqq = reqq;
    }

    // bandwidth

    [[nodiscard]] tr_bandwidth& getBandwidthGroup(std::string_view name);

    //

    [[nodiscard]] constexpr auto& openFiles() noexcept
    {
        return open_files_;
    }

    void close_torrent_files(tr_torrent_id_t tor_id) noexcept;
    void close_torrent_file(tr_torrent const& tor, tr_file_index_t file_num) noexcept;

    // announce ip

    [[nodiscard]] constexpr std::string const& announceIP() const noexcept
    {
        return settings().announce_ip;
    }

    void setAnnounceIP(std::string_view ip)
    {
        settings_.announce_ip = ip;
    }

    [[nodiscard]] constexpr bool useAnnounceIP() const noexcept
    {
        return settings().announce_ip_enabled;
    }

    constexpr void useAnnounceIP(bool enabled) noexcept
    {
        settings_.announce_ip_enabled = enabled;
    }

    // callbacks

    using queue_start_callback_t = void (*)(tr_session*, tr_torrent*, void* user_data);

    constexpr void setQueueStartCallback(queue_start_callback_t cb, void* user_data)
    {
        queue_start_callback_ = cb;
        queue_start_user_data_ = user_data;
    }

    constexpr void setIdleLimitHitCallback(tr_session_idle_limit_hit_func cb, void* user_data)
    {
        idle_limit_hit_callback_ = cb;
        idle_limit_hit_user_data_ = user_data;
    }

    void onIdleLimitHit(tr_torrent* tor)
    {
        if (idle_limit_hit_callback_ != nullptr)
        {
            idle_limit_hit_callback_(this, tor, idle_limit_hit_user_data_);
        }
    }

    constexpr void setRatioLimitHitCallback(tr_session_ratio_limit_hit_func cb, void* user_data)
    {
        ratio_limit_hit_cb_ = cb;
        ratio_limit_hit_user_data_ = user_data;
    }

    void onRatioLimitHit(tr_torrent* tor)
    {
        if (ratio_limit_hit_cb_ != nullptr)
        {
            ratio_limit_hit_cb_(this, tor, ratio_limit_hit_user_data_);
        }
    }

    constexpr void setMetadataCallback(tr_session_metadata_func cb, void* user_data)
    {
        got_metadata_cb_ = cb;
        got_metadata_user_data_ = user_data;
    }

    void onMetadataCompleted(tr_torrent* tor)
    {
        if (got_metadata_cb_ != nullptr)
        {
            got_metadata_cb_(this, tor, got_metadata_user_data_);
        }
    }

    constexpr void setTorrentCompletenessCallback(tr_torrent_completeness_func cb, void* user_data)
    {
        completeness_func_ = cb;
        completeness_func_user_data_ = user_data;
    }

    void onTorrentCompletenessChanged(tr_torrent* tor, tr_completeness completeness, bool was_running)
    {
        if (completeness_func_ != nullptr)
        {
            completeness_func_(tor, completeness, was_running, completeness_func_user_data_);
        }
    }

    /// stats

    [[nodiscard]] constexpr auto& stats() noexcept
    {
        return session_stats_;
    }

    [[nodiscard]] constexpr auto const& stats() const noexcept
    {
        return session_stats_;
    }

    constexpr void add_uploaded(uint32_t n_bytes) noexcept
    {
        stats().add_uploaded(n_bytes);
    }

    constexpr void add_downloaded(uint32_t n_bytes) noexcept
    {
        stats().add_downloaded(n_bytes);
    }

    constexpr void add_file_created() noexcept
    {
        stats().add_file_created();
    }

    // The incoming peer port that's been opened on the local machine
    // that Transmission is running on.
    [[nodiscard]] constexpr tr_port localPeerPort() const noexcept
    {
        return local_peer_port_;
    }

    [[nodiscard]] constexpr tr_port udpPort() const noexcept
    {
        // Always use the same port number that's used for incoming TCP connections.
        // This simplifies port forwarding and reduces the chance of confusion,
        // since incoming UDP and TCP connections will use the same port number
        return localPeerPort();
    }

    // The incoming peer port that's been opened on the public-facing
    // device. This is usually the same as localPeerPort() but can differ,
    // e.g. if the public device is a router that chose to use a different
    // port than the one requested by Transmission.
    [[nodiscard]] constexpr tr_port advertisedPeerPort() const noexcept
    {
        return advertised_peer_port_;
    }

    [[nodiscard]] constexpr auto queueEnabled(tr_direction dir) const noexcept
    {
        return dir == TR_DOWN ? settings_.download_queue_enabled : settings_.seed_queue_enabled;
    }

    [[nodiscard]] constexpr auto queueSize(tr_direction dir) const noexcept
    {
        return dir == TR_DOWN ? settings_.download_queue_size : settings_.seed_queue_size;
    }

    [[nodiscard]] constexpr auto queueStalledEnabled() const noexcept
    {
        return settings().queue_stalled_enabled;
    }

    [[nodiscard]] constexpr auto queueStalledMinutes() const noexcept
    {
        return settings().queue_stalled_minutes;
    }

    [[nodiscard]] constexpr auto uploadSlotsPerTorrent() const noexcept
    {
        return settings().upload_slots_per_torrent;
    }

    [[nodiscard]] constexpr auto isClosing() const noexcept
    {
        return is_closing_;
    }

    [[nodiscard]] constexpr auto encryptionMode() const noexcept
    {
        return settings().encryption_mode;
    }

    [[nodiscard]] constexpr auto preallocationMode() const noexcept
    {
        return settings().preallocation_mode;
    }

    [[nodiscard]] constexpr auto shouldScrapePausedTorrents() const noexcept
    {
        return settings().should_scrape_paused_torrents;
    }

    [[nodiscard]] constexpr auto shouldPauseAddedTorrents() const noexcept
    {
        return !settings_.should_start_added_torrents;
    }

    [[nodiscard]] constexpr auto shouldFullyVerifyAddedTorrents() const noexcept
    {
        return settings().torrent_added_verify_mode == TR_VERIFY_ADDED_FULL;
    }

    [[nodiscard]] constexpr auto shouldDeleteSource() const noexcept
    {
        return settings().should_delete_source_torrents;
    }

    [[nodiscard]] constexpr auto allowsDHT() const noexcept
    {
        return settings().dht_enabled;
    }

    [[nodiscard]] constexpr bool allowsLPD() const noexcept
    {
        return settings().lpd_enabled;
    }

    [[nodiscard]] constexpr auto allows_pex() const noexcept
    {
        return settings().pex_enabled;
    }

    [[nodiscard]] constexpr auto allowsTCP() const noexcept
    {
        return settings().tcp_enabled;
    }

    [[nodiscard]] bool allowsUTP() const noexcept;

    [[nodiscard]] constexpr auto preferred_transport() const noexcept
    {
        return settings().preferred_transport;
    }

    [[nodiscard]] constexpr auto isIdleLimited() const noexcept
    {
        return settings().idle_seeding_limit_enabled;
    }

    [[nodiscard]] constexpr auto idleLimitMinutes() const noexcept
    {
        return settings().idle_seeding_limit_minutes;
    }

    /*module_visible*/

    auto rpcNotify(tr_rpc_callback_type type, tr_torrent* tor = nullptr)
    {
        if (rpc_func_ != nullptr)
        {
            return (*rpc_func_)(this, type, tor, rpc_func_user_data_);
        }

        return TR_RPC_OK;
    }

    [[nodiscard]] size_t count_queue_free_slots(tr_direction dir) const noexcept;

    [[nodiscard]] bool has_ip_protocol(tr_address_type type) const noexcept
    {
        TR_ASSERT(tr_address::is_valid(type));
        return ip_cache_.has_ip_protocol(type);
    }

    [[nodiscard]] tr_address bind_address(tr_address_type type) const noexcept;

    [[nodiscard]] std::optional<tr_address> global_address(tr_address_type type) const noexcept
    {
        TR_ASSERT(tr_address::is_valid(type));
        return ip_cache_.global_addr(type);
    }

    bool set_global_address(tr_address const& addr) noexcept
    {
        return ip_cache_.set_global_addr(addr.type, addr);
    }

    [[nodiscard]] std::optional<tr_address> global_source_address(tr_address_type type) const noexcept
    {
        TR_ASSERT(tr_address::is_valid(type));
        return ip_cache_.global_source_addr(type);
    }

    [[nodiscard]] auto speed_limit(tr_direction const dir) const noexcept
    {
        auto const kbyps = dir == TR_DOWN ? settings_.speed_limit_down : settings_.speed_limit_up;
        return Speed{ kbyps, Speed::Units::KByps };
    }

    void set_speed_limit(tr_direction dir, Speed limit) noexcept
    {
        auto& tgt = dir == TR_DOWN ? settings_.speed_limit_down : settings_.speed_limit_up;
        tgt = limit.count(Speed::Units::KByps);
        update_bandwidth(dir);
    }

    [[nodiscard]] constexpr auto is_speed_limited(tr_direction dir) const noexcept
    {
        return dir == TR_DOWN ? settings_.speed_limit_down_enabled : settings_.speed_limit_up_enabled;
    }

    [[nodiscard]] auto piece_speed(tr_direction dir) const noexcept
    {
        return top_bandwidth_.get_piece_speed(0, dir);
    }

    [[nodiscard]] std::optional<Speed> active_speed_limit(tr_direction dir) const noexcept;

    [[nodiscard]] constexpr auto isIncompleteFileNamingEnabled() const noexcept
    {
        return settings().is_incomplete_file_naming_enabled;
    }

    [[nodiscard]] constexpr auto isPortRandom() const noexcept
    {
        return settings().peer_port_random_on_start;
    }

    [[nodiscard]] constexpr auto isRatioLimited() const noexcept
    {
        return settings().ratio_limit_enabled;
    }

    [[nodiscard]] constexpr auto desiredRatio() const noexcept
    {
        return settings().ratio_limit;
    }

    void verify_add(tr_torrent* tor);
    void verify_remove(tr_torrent const* tor);

    void fetch(tr_web::FetchOptions&& options) const
    {
        if (web_)
        {
            web_->fetch(std::move(options));
        }
    }

    [[nodiscard]] constexpr auto const& bandwidthGroups() const noexcept
    {
        return bandwidth_groups_;
    }

    void addIncoming(tr_peer_socket&& socket);

    void addTorrent(tr_torrent* tor);

    void maybe_add_dht_node(tr_address const& addr, tr_port port)
    {
        if (dht_)
        {
            dht_->maybe_add_node(addr, port);
        }
    }

private:
    constexpr bool& scriptEnabledFlag(TrScript i)
    {
        if (i == TR_SCRIPT_ON_TORRENT_ADDED)
        {
            return settings_.script_torrent_added_enabled;
        }

        if (i == TR_SCRIPT_ON_TORRENT_DONE)
        {
            return settings_.script_torrent_done_enabled;
        }

        return settings_.script_torrent_done_seeding_enabled;
    }

    constexpr std::string& scriptFilename(TrScript i)
    {
        if (i == TR_SCRIPT_ON_TORRENT_ADDED)
        {
            return settings_.script_torrent_added_filename;
        }

        if (i == TR_SCRIPT_ON_TORRENT_DONE)
        {
            return settings_.script_torrent_done_filename;
        }

        return settings_.script_torrent_done_seeding_filename;
    }

    void update_bandwidth(tr_direction dir);

    [[nodiscard]] tr_port randomPort() const;

    void onAdvertisedPeerPortChanged();

    struct init_data;
    void initImpl(init_data&);
    void setSettings(tr_variant const& settings_map, bool force);
    void setSettings(Settings&& settings, bool force);

    void closeImplPart1(std::promise<void>* closed_promise, std::chrono::time_point<std::chrono::steady_clock> deadline);
    void closeImplPart2(std::promise<void>* closed_promise, std::chrono::time_point<std::chrono::steady_clock> deadline);

    void on_now_timer();
    void on_queue_timer();
    void on_save_timer();

    static void onIncomingPeerConnection(tr_socket_t fd, void* vsession);

    friend class libtransmission::test::SessionTest;

    friend bool tr_blocklistExists(tr_session const* session);
    friend bool tr_sessionGetAntiBruteForceEnabled(tr_session const* session);
    friend bool tr_sessionIsPortForwardingEnabled(tr_session const* session);
    friend bool tr_sessionIsRPCEnabled(tr_session const* session);
    friend bool tr_sessionIsRPCPasswordEnabled(tr_session const* session);
    friend bool tr_sessionUsesAltSpeed(tr_session const* session);
    friend bool tr_sessionUsesAltSpeedTime(tr_session const* session);
    friend char const* tr_sessionGetRPCPassword(tr_session const* session);
    friend char const* tr_sessionGetRPCUsername(tr_session const* session);
    friend char const* tr_sessionGetRPCWhitelist(tr_session const* session);
    friend int tr_sessionGetAntiBruteForceThreshold(tr_session const* session);
    friend size_t tr_blocklistGetRuleCount(tr_session const* session);
    friend size_t tr_blocklistSetContent(tr_session* session, char const* content_filename);
    friend size_t tr_sessionGetAltSpeedBegin(tr_session const* session);
    friend size_t tr_sessionGetAltSpeedEnd(tr_session const* session);
    friend size_t tr_sessionGetCacheLimit_MB(tr_session const* session);
    friend size_t tr_sessionGetAltSpeed_KBps(tr_session const* session, tr_direction dir);
    friend tr_port_forwarding_state tr_sessionGetPortForwarding(tr_session const* session);
    friend tr_sched_day tr_sessionGetAltSpeedDay(tr_session const* session);
    friend tr_session* tr_sessionInit(char const* config_dir, bool message_queueing_enabled, tr_variant const& client_settings);
    friend uint16_t tr_sessionGetPeerPort(tr_session const* session);
    friend uint16_t tr_sessionGetRPCPort(tr_session const* session);
    friend uint16_t tr_sessionSetPeerPortRandom(tr_session* session);
    friend void tr_sessionClose(tr_session* session, size_t timeout_secs);
    friend tr_variant tr_sessionGetSettings(tr_session const* s);
    friend void tr_sessionLimitSpeed(tr_session* session, tr_direction dir, bool limited);
    friend void tr_sessionReloadBlocklists(tr_session* session);
    friend void tr_sessionSet(tr_session* session, tr_variant const& settings);
    friend void tr_sessionSetAltSpeedBegin(tr_session* session, size_t minutes_since_midnight);
    friend void tr_sessionSetAltSpeedDay(tr_session* session, tr_sched_day days);
    friend void tr_sessionSetAltSpeedEnd(tr_session* session, size_t minutes_since_midnight);
    friend void tr_sessionSetAltSpeedFunc(tr_session* session, tr_altSpeedFunc func, void* user_data);
    friend void tr_sessionSetAltSpeed_KBps(tr_session* session, tr_direction dir, size_t limit_kbyps);
    friend void tr_sessionSetAntiBruteForceEnabled(tr_session* session, bool is_enabled);
    friend void tr_sessionSetAntiBruteForceThreshold(tr_session* session, int max_bad_requests);
    friend void tr_sessionSetCacheLimit_MB(tr_session* session, size_t mbytes);
    friend void tr_sessionSetDHTEnabled(tr_session* session, bool enabled);
    friend void tr_sessionSetDeleteSource(tr_session* session, bool delete_source);
    friend void tr_sessionSetEncryption(tr_session* session, tr_encryption_mode mode);
    friend void tr_sessionSetIdleLimit(tr_session* session, uint16_t idle_minutes);
    friend void tr_sessionSetIdleLimited(tr_session* session, bool is_limited);
    friend void tr_sessionSetIncompleteFileNamingEnabled(tr_session* session, bool enabled);
    friend void tr_sessionSetLPDEnabled(tr_session* session, bool enabled);
    friend void tr_sessionSetPaused(tr_session* session, bool is_paused);
    friend void tr_sessionSetPeerLimit(tr_session* session, uint16_t max_global_peers);
    friend void tr_sessionSetPeerLimitPerTorrent(tr_session* session, uint16_t max_peers);
    friend void tr_sessionSetPeerPort(tr_session* session, uint16_t hport);
    friend void tr_sessionSetPeerPortRandomOnStart(tr_session* session, bool random);
    friend void tr_sessionSetPexEnabled(tr_session* session, bool enabled);
    friend void tr_sessionSetPortForwardingEnabled(tr_session* session, bool enabled);
    friend void tr_sessionSetQueueEnabled(tr_session* session, tr_direction dir, bool do_limit_simultaneous_torrents);
    friend void tr_sessionSetQueueSize(tr_session* session, tr_direction dir, size_t max_simultaneous_torrents);
    friend void tr_sessionSetQueueStalledEnabled(tr_session* session, bool is_enabled);
    friend void tr_sessionSetQueueStalledMinutes(tr_session* session, int minutes);
    friend void tr_sessionSetRPCCallback(tr_session* session, tr_rpc_func func, void* user_data);
    friend void tr_sessionSetRPCEnabled(tr_session* session, bool is_enabled);
    friend void tr_sessionSetRPCPassword(tr_session* session, char const* password);
    friend void tr_sessionSetRPCPasswordEnabled(tr_session* session, bool enabled);
    friend void tr_sessionSetRPCPort(tr_session* session, uint16_t hport);
    friend void tr_sessionSetRPCUsername(tr_session* session, char const* username);
    friend void tr_sessionSetRatioLimit(tr_session* session, double desired_ratio);
    friend void tr_sessionSetRatioLimited(tr_session* session, bool is_limited);
    friend void tr_sessionSetUTPEnabled(tr_session* session, bool enabled);
    friend void tr_sessionUseAltSpeed(tr_session* session, bool enabled);
    friend void tr_sessionUseAltSpeedTime(tr_session* session, bool enabled);

public:
    /// constexpr fields

    static constexpr std::array<std::tuple<tr_quark, tr_quark, TrScript>, 3> Scripts{
        { { TR_KEY_script_torrent_added_enabled_kebab, TR_KEY_script_torrent_added_filename_kebab, TR_SCRIPT_ON_TORRENT_ADDED },
          { TR_KEY_script_torrent_done_enabled, TR_KEY_script_torrent_done_filename, TR_SCRIPT_ON_TORRENT_DONE },
          { TR_KEY_script_torrent_done_seeding_enabled,
            TR_KEY_script_torrent_done_seeding_filename,
            TR_SCRIPT_ON_TORRENT_DONE_SEEDING } }
    };

private:
    /// const fields

    std::string const config_dir_;
    std::string const resume_dir_;
    std::string const torrent_dir_;
    std::string const blocklist_dir_;

    std::unique_ptr<tr_session_thread> const session_thread_;

    // depends-on: session_thread_
    std::unique_ptr<libtransmission::TimerMaker> const timer_maker_;

    /// trivial type fields

    Settings settings_;

    queue_start_callback_t queue_start_callback_ = nullptr;
    void* queue_start_user_data_ = nullptr;

    tr_session_idle_limit_hit_func idle_limit_hit_callback_ = nullptr;
    void* idle_limit_hit_user_data_ = nullptr;

    tr_session_ratio_limit_hit_func ratio_limit_hit_cb_ = nullptr;
    void* ratio_limit_hit_user_data_ = nullptr;

    tr_session_metadata_func got_metadata_cb_ = nullptr;
    void* got_metadata_user_data_ = nullptr;

    tr_torrent_completeness_func completeness_func_ = nullptr;
    void* completeness_func_user_data_ = nullptr;

    tr_rpc_func rpc_func_ = nullptr;
    void* rpc_func_user_data_ = nullptr;

    tr_altSpeedFunc alt_speed_active_changed_func_ = nullptr;
    void* alt_speed_active_changed_func_user_data_ = nullptr;

    // The local peer port that we bind a socket to for listening
    // to incoming peer connections. Usually the same as
    // `settings_.peer_port` but can differ if
    // `settings_.peer_port_random_on_start` is enabled.
    tr_port local_peer_port_;

    // The incoming peer port that's been opened on the public-facing
    // device. This is usually the same as localPeerPort() but can differ,
    // e.g. if the public device is a router that chose to use a different
    // port than the one requested by Transmission.
    tr_port advertised_peer_port_;

    bool is_closing_ = false;

    /// fields that aren't trivial,
    /// but are self-contained / don't hold references to others

    mutable std::recursive_mutex session_mutex_;

    tr_stats session_stats_{ config_dir_, time(nullptr) };

    tr_announce_list default_trackers_;

    tr_session_id session_id_;

    tr_open_files open_files_;

    libtransmission::Blocklists blocklists_;

private:
    /// other fields

    // depends-on: session_thread_, settings_.bind_address_ipv4, local_peer_port_, global_ip_cache (via tr_session::bind_address())
    std::optional<BoundSocket> bound_ipv4_;

    // depends-on: session_thread_, settings_.bind_address_ipv6, local_peer_port_, global_ip_cache (via tr_session::bind_address())
    std::optional<BoundSocket> bound_ipv6_;

public:
    // depends-on: settings_, announcer_udp_, global_ip_cache_
    // FIXME(ckerr): circular dependency udp_core -> announcer_udp -> announcer_udp_mediator -> udp_core
    std::unique_ptr<tr_udp_core> udp_core_;

    // monitors the "global pool" speeds
    tr_bandwidth top_bandwidth_{ true };

private:
    // depends-on: top_bandwidth_
    std::vector<std::pair<tr_interned_string, std::unique_ptr<tr_bandwidth>>> bandwidth_groups_;

    // depends-on: timer_maker_, settings_, local_peer_port_
    PortForwardingMediator port_forwarding_mediator_{ *this };
    std::unique_ptr<tr_port_forwarding> port_forwarding_ = tr_port_forwarding::create(port_forwarding_mediator_);

    // depends-on: session_thread_, top_bandwidth_
    AltSpeedMediator alt_speed_mediator_{ *this };
    tr_session_alt_speeds alt_speeds_{ alt_speed_mediator_ };

public:
    // depends-on: udp_core_
    struct struct_utp_context* utp_context = nullptr;

private:
    // depends-on: open_files_
    tr_torrents torrents_;

    // depends-on: settings_, session_thread_, timer_maker_, web_
    IPCacheMediator ip_cache_mediator_{ *this };
    tr_ip_cache ip_cache_{ ip_cache_mediator_ };

    // depends-on: settings_, session_thread_, torrents_, global_ip_cache (via tr_session::bind_address())
    WebMediator web_mediator_{ this };
    std::unique_ptr<tr_web> web_ = tr_web::create(this->web_mediator_);

public:
    // depends-on: settings_, open_files_, torrents_
    std::unique_ptr<Cache> cache = std::make_unique<Cache>(torrents_, Memory{ 2U, Memory::Units::MBytes });

private:
    // depends-on: timer_maker_, blocklists_, top_bandwidth_, utp_context, torrents_, web_
    std::unique_ptr<struct tr_peerMgr, void (*)(struct tr_peerMgr*)> peer_mgr_;

    // depends-on: peer_mgr_, advertised_peer_port_, torrents_
    LpdMediator lpd_mediator_{ *this };

    // depends-on: lpd_mediator_
    std::unique_ptr<tr_lpd> lpd_;

    // depends-on: udp_core_
    AnnouncerUdpMediator announcer_udp_mediator_{ *this };

    // depends-on: timer_maker_, torrents_, peer_mgr_
    DhtMediator dht_mediator_{ *this };

public:
    // depends-on: announcer_udp_mediator_
    std::unique_ptr<tr_announcer_udp> announcer_udp_ = tr_announcer_udp::create(announcer_udp_mediator_);

    // depends-on: settings_, torrents_, web_, announcer_udp_
    std::unique_ptr<tr_announcer> announcer_ = tr_announcer::create(this, *announcer_udp_);

    // depends-on: public_peer_port_, udp_core_, dht_mediator_
    std::unique_ptr<tr_dht> dht_;

private:
    // depends-on: session_thread_, timer_maker_, settings_, torrents_, web_
    std::unique_ptr<tr_rpc_server> rpc_server_;

    // depends-on: alt_speeds_, udp_core_, torrents_
    std::unique_ptr<libtransmission::Timer> now_timer_;

    // depends-on: torrents_
    std::unique_ptr<libtransmission::Timer> queue_timer_;

    // depends-on: torrents_
    std::unique_ptr<libtransmission::Timer> save_timer_;

    std::unique_ptr<tr_verify_worker> verifier_ = std::make_unique<tr_verify_worker>();

public:
    std::unique_ptr<libtransmission::Timer> utp_timer;
};
