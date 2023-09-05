// This file Copyright © 2009-2023 Juliusz Chroboczek.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint> // uint16_t
#include <cstring> // memcpy()
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple> // std::tie()
#include <utility>

#ifdef _WIN32
#include <ws2tcpip.h>
#undef gai_strerror
#define gai_strerror gai_strerrorA
#else
#include <sys/socket.h> /* socket(), bind() */
#include <netdb.h>
#include <netinet/in.h> /* sockaddr_in */
#endif

#include <fmt/core.h>

#include "libtransmission/transmission.h"

#include "libtransmission/crypto-utils.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/peer-mgr.h" // for tr_peerMgrCompactToPex()
#include "libtransmission/quark.h"
#include "libtransmission/timer.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-dht.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/variant.h"
#include "libtransmission/utils.h" // for tr_time(), _()

using namespace std::literals;

// the dht library needs us to implement these:
extern "C"
{

    // This function should return true when a node is blacklisted.
    // We don't support using a blacklist with the DHT in Transmission,
    // since massive (ab)use of this feature could harm the DHT. However,
    // feel free to add support to your private copy as long as you don't
    // redistribute it.
    int dht_blacklisted(sockaddr const* /*sa*/, int /*salen*/)
    {
        return 0;
    }

    void dht_hash(
        void* hash_return,
        int hash_size,
        void const* v1,
        int len1,
        void const* v2,
        int len2,
        void const* v3,
        int len3)
    {
        auto* setme = reinterpret_cast<std::byte*>(hash_return);
        std::fill_n(static_cast<char*>(hash_return), hash_size, '\0');

        auto const sv1 = std::string_view{ static_cast<char const*>(v1), size_t(len1) };
        auto const sv2 = std::string_view{ static_cast<char const*>(v2), size_t(len2) };
        auto const sv3 = std::string_view{ static_cast<char const*>(v3), size_t(len3) };
        auto const digest = tr_sha1::digest(sv1, sv2, sv3);
        std::copy_n(std::data(digest), std::min(size_t(hash_size), std::size(digest)), setme);
    }

    int dht_random_bytes(void* buf, size_t size)
    {
        tr_rand_buffer(buf, size);
        return static_cast<int>(size);
    }

    int dht_sendto(int sockfd, void const* buf, int len, int flags, struct sockaddr const* to, int tolen)
    {
        return static_cast<int>(sendto(sockfd, static_cast<char const*>(buf), len, flags, to, tolen));
    }

#if defined(_WIN32) && !defined(__MINGW32__)
    int dht_gettimeofday(struct timeval* tv, [[maybe_unused]] struct timezone* tz)
    {
        TR_ASSERT(tz == nullptr);

        auto const d = std::chrono::system_clock::now().time_since_epoch();
        auto const s = std::chrono::duration_cast<std::chrono::seconds>(d);
        tv->tv_sec = s.count();
        tv->tv_usec = std::chrono::duration_cast<std::chrono::microseconds>(d - s).count();

        return 0;
    }
#endif

} // extern "C"

class tr_dht_impl final : public tr_dht
{
private:
    using Node = tr_socket_address;
    using Nodes = std::deque<Node>;
    using Id = std::array<unsigned char, 20>;

    enum class SwarmStatus
    {
        Stopped,
        Broken,
        Poor,
        Firewalled,
        Good
    };

public:
    tr_dht_impl(Mediator& mediator, tr_port peer_port, tr_socket_t udp4_socket, tr_socket_t udp6_socket)
        : peer_port_{ peer_port }
        , udp4_socket_{ udp4_socket }
        , udp6_socket_{ udp6_socket }
        , mediator_{ mediator }
        , state_filename_{ tr_pathbuf{ mediator_.config_dir(), "/dht.dat" } }
        , announce_timer_{ mediator_.timer_maker().create([this]() { on_announce_timer(); }) }
        , bootstrap_timer_{ mediator_.timer_maker().create([this]() { on_bootstrap_timer(); }) }
        , periodic_timer_{ mediator_.timer_maker().create([this]() { on_periodic_timer(); }) }
    {
        tr_logAddDebug(fmt::format("Starting DHT on port {port}", fmt::arg("port", peer_port.host())));

        // load up the bootstrap nodes
        if (tr_sys_path_exists(state_filename_.c_str()))
        {
            std::tie(id_, bootstrap_queue_) = load_state(state_filename_);
        }
        get_nodes_from_bootstrap_file(tr_pathbuf{ mediator_.config_dir(), "/dht.bootstrap"sv }, bootstrap_queue_);
        get_nodes_from_name("dht.transmissionbt.com", tr_port::from_host(6881), bootstrap_queue_);
        bootstrap_timer_->start_single_shot(100ms);

        mediator_.api().init(udp4_socket_, udp6_socket_, std::data(id_), nullptr);

        on_announce_timer();
        announce_timer_->start_repeating(1s);

        on_periodic_timer();
    }

    tr_dht_impl(tr_dht_impl&&) = delete;
    tr_dht_impl(tr_dht_impl const&) = delete;
    tr_dht_impl& operator=(tr_dht_impl&&) = delete;
    tr_dht_impl& operator=(tr_dht_impl const&) = delete;

    ~tr_dht_impl() override
    {
        tr_logAddTrace("Uninitializing DHT");

        // Since we only save known good nodes,
        // only overwrite older data if we know enough nodes.
        if (is_ready(AF_INET) || is_ready(AF_INET6))
        {
            save_state();
        }

        mediator_.api().uninit();
        tr_logAddTrace("Done uninitializing DHT");
    }

    void add_node(tr_address const& addr, tr_port port) override
    {
        if (addr.is_ipv4())
        {
            auto sin = sockaddr_in{};
            sin.sin_family = AF_INET;
            sin.sin_addr = addr.addr.addr4;
            sin.sin_port = port.network();
            mediator_.api().ping_node(reinterpret_cast<sockaddr*>(&sin), sizeof(sin));
        }
        else if (addr.is_ipv6())
        {
            auto sin6 = sockaddr_in6{};
            sin6.sin6_family = AF_INET6;
            sin6.sin6_addr = addr.addr.addr6;
            sin6.sin6_port = port.network();
            mediator_.api().ping_node(reinterpret_cast<sockaddr*>(&sin6), sizeof(sin6));
        }
    }

    void handle_message(unsigned char const* msg, size_t msglen, struct sockaddr* from, socklen_t fromlen) override
    {
        auto const call_again_in_n_secs = periodic(msg, msglen, from, fromlen);

        // Being slightly late is fine,
        // and has the added benefit of adding some jitter.
        auto const interval = call_again_in_n_secs + std::chrono::milliseconds{ tr_rand_int(1000U) };
        periodic_timer_->start_single_shot(interval);
    }

private:
    [[nodiscard]] constexpr tr_socket_t udpSocket(int af) const noexcept
    {
        switch (af)
        {
        case AF_INET:
            return udp4_socket_;

        case AF_INET6:
            return udp6_socket_;

        default:
            return TR_BAD_SOCKET;
        }
    }

    [[nodiscard]] SwarmStatus swarm_status(int family, int* const setme_node_count = nullptr) const
    {
        if (udpSocket(family) == TR_BAD_SOCKET)
        {
            if (setme_node_count != nullptr)
            {
                *setme_node_count = 0;
            }

            return SwarmStatus::Stopped;
        }

        int good = 0;
        int dubious = 0;
        int incoming = 0;
        mediator_.api().nodes(family, &good, &dubious, nullptr, &incoming);

        if (setme_node_count != nullptr)
        {
            *setme_node_count = good + dubious;
        }

        if (good < 4 || good + dubious <= 8)
        {
            return SwarmStatus::Broken;
        }

        if (good < 40)
        {
            return SwarmStatus::Poor;
        }

        if (incoming < 8)
        {
            return SwarmStatus::Firewalled;
        }

        return SwarmStatus::Good;
    }

    [[nodiscard]] static constexpr auto is_ready(SwarmStatus const status)
    {
        return status >= SwarmStatus::Firewalled;
    }

    [[nodiscard]] bool is_ready(int af) const noexcept
    {
        return is_ready(swarm_status(af));
    }

    [[nodiscard]] bool is_ready() const noexcept
    {
        return is_ready(AF_INET) && is_ready(AF_INET6);
    }

    ///

    // how long to wait between adding nodes during bootstrap
    [[nodiscard]] static constexpr auto bootstrap_interval(size_t n_added)
    {
        // Our DHT code is able to take up to 9 nodes in a row without
        // dropping any. After that, it takes some time to split buckets.
        // So ping the first 8 nodes quickly, then slow down.
        if (n_added < 8U)
        {
            return 2s;
        }

        if (n_added < 16U)
        {
            return 15s;
        }

        return 40s;
    }

    void on_bootstrap_timer()
    {
        // Since we don't want to abuse our bootstrap nodes,
        // we don't ping them if the DHT is in a good state.
        if (is_ready() || std::empty(bootstrap_queue_))
        {
            return;
        }

        auto [address, port] = bootstrap_queue_.front();
        bootstrap_queue_.pop_front();
        add_node(address, port);
        ++n_bootstrapped_;

        bootstrap_timer_->start_single_shot(bootstrap_interval(n_bootstrapped_));
    }

    ///

    [[nodiscard]] auto announce_torrent(tr_sha1_digest_t const& info_hash, int af, tr_port port)
    {
        auto const* dht_hash = reinterpret_cast<unsigned char const*>(std::data(info_hash));
        auto const rc = mediator_.api().search(dht_hash, port.host(), af, callback, this);
        auto const announce_again_in_n_secs = rc < 0 ? 5s + std::chrono::seconds{ tr_rand_int(5U) } :
                                                       25min + std::chrono::seconds{ tr_rand_int(3U * 60U) };
        return announce_again_in_n_secs;
    }

    void on_announce_timer()
    {
        // don't announce if the swarm isn't ready
        if (swarm_status(AF_INET) < SwarmStatus::Poor && swarm_status(AF_INET6) < SwarmStatus::Poor)
        {
            return;
        }

        auto const now = tr_time();
        for (auto const id : mediator_.torrents_allowing_dht())
        {
            auto& times = announce_times_[id];

            if (auto& announce_after = times.ipv4_announce_after; announce_after < now)
            {
                auto const announce_again_in_n_secs = announce_torrent(mediator_.torrent_info_hash(id), AF_INET, peer_port_);
                announce_after = now + std::chrono::seconds{ announce_again_in_n_secs }.count();
            }

            if (auto& announce_after = times.ipv6_announce_after; announce_after < now)
            {
                auto const announce_again_in_n_secs = announce_torrent(mediator_.torrent_info_hash(id), AF_INET6, peer_port_);
                announce_after = now + std::chrono::seconds{ announce_again_in_n_secs }.count();
            }
        }
    }

    ///

    void on_periodic_timer()
    {
        auto const call_again_in_n_secs = periodic(nullptr, 0, nullptr, 0);

        // Being slightly late is fine,
        // and has the added benefit of adding some jitter.
        auto const interval = call_again_in_n_secs + std::chrono::milliseconds{ tr_rand_int(1000U) };
        periodic_timer_->start_single_shot(interval);
    }

    [[nodiscard]] std::chrono::seconds periodic(
        unsigned char const* msg,
        size_t msglen,
        struct sockaddr const* from,
        socklen_t fromlen)
    {
        TR_ASSERT_MSG(msglen == 0 || msg[msglen] == '\0', "libdht requires zero-terminated msg");

        auto call_again_in_n_secs = time_t{};
        mediator_.api().periodic(msg, msglen, from, static_cast<int>(fromlen), &call_again_in_n_secs, callback, this);
        return std::chrono::seconds{ call_again_in_n_secs };
    }

    static auto remove_bad_pex(std::vector<tr_pex>&& pex)
    {
        static constexpr auto IsBadPex = [](tr_pex const& candidate)
        {
            // paper over a bug in some DHT implementation that gives port 1.
            // Xref: https://github.com/transmission/transmission/issues/527
            return candidate.socket_address.port_ == tr_port::from_host(1);
        };

        pex.erase(std::remove_if(std::begin(pex), std::end(pex), IsBadPex), std::end(pex));
        return pex;
    }

    static void callback(void* vself, int event, unsigned char const* info_hash, void const* data, size_t data_len)
    {
        auto* const self = static_cast<tr_dht_impl*>(vself);
        auto hash = tr_sha1_digest_t{};
        std::copy_n(reinterpret_cast<std::byte const*>(info_hash), std::size(hash), std::data(hash));

        if (event == DHT_EVENT_VALUES)
        {
            auto const pex = remove_bad_pex(tr_pex::from_compact_ipv4(data, data_len, nullptr, 0));
            self->mediator_.add_pex(hash, std::data(pex), std::size(pex));
        }
        else if (event == DHT_EVENT_VALUES6)
        {
            auto const pex = remove_bad_pex(tr_pex::from_compact_ipv6(data, data_len, nullptr, 0));
            self->mediator_.add_pex(hash, std::data(pex), std::size(pex));
        }
    }

    ///

    void save_state() const
    {
        auto constexpr MaxNodes = int{ 300 };
        auto constexpr PortLen = size_t{ 2 };
        auto constexpr CompactAddrLen = size_t{ 4 };
        auto constexpr CompactLen = size_t{ CompactAddrLen + PortLen };
        auto constexpr Compact6AddrLen = size_t{ 16 };
        auto constexpr Compact6Len = size_t{ Compact6AddrLen + PortLen };

        auto sins4 = std::array<struct sockaddr_in, MaxNodes>{};
        auto sins6 = std::array<struct sockaddr_in6, MaxNodes>{};
        auto num4 = int{ MaxNodes };
        auto num6 = int{ MaxNodes };
        auto const n = mediator_.api().get_nodes(std::data(sins4), &num4, std::data(sins6), &num6);
        tr_logAddTrace(fmt::format("Saving {} ({} + {}) nodes", n, num4, num6));

        auto benc = tr_variant::make_map(3U);
        tr_variantDictAddRaw(&benc, TR_KEY_id, std::data(id_), std::size(id_));

        if (num4 > 0)
        {
            auto compact = std::array<char, MaxNodes * CompactLen>{};
            char* out = std::data(compact);
            for (auto const* in = std::data(sins4), *end = in + num4; in != end; ++in)
            {
                memcpy(out, &in->sin_addr, CompactAddrLen);
                out += CompactAddrLen;
                memcpy(out, &in->sin_port, PortLen); // saved in network byte order
                out += PortLen;
            }

            tr_variantDictAddRaw(&benc, TR_KEY_nodes, std::data(compact), out - std::data(compact));
        }

        if (num6 > 0)
        {
            auto compact6 = std::array<char, MaxNodes * Compact6Len>{};
            char* out6 = std::data(compact6);
            for (auto const* in = std::data(sins6), *end = in + num6; in != end; ++in)
            {
                memcpy(out6, &in->sin6_addr, Compact6AddrLen);
                out6 += Compact6AddrLen;
                memcpy(out6, &in->sin6_port, PortLen); // saved in network byte order
                out6 += PortLen;
            }

            tr_variantDictAddRaw(&benc, TR_KEY_nodes6, std::data(compact6), out6 - std::data(compact6));
        }

        tr_variant_serde::benc().to_file(benc, state_filename_);
    }

    [[nodiscard]] static std::pair<Id, Nodes> load_state(std::string_view filename)
    {
        // Note that DHT ids need to be distributed uniformly,
        // so it should be something truly random
        auto id = tr_rand_obj<Id>();

        auto nodes = Nodes{};

        if (auto otop = tr_variant_serde::benc().parse_file(filename); otop)
        {
            auto& top = *otop;

            if (auto sv = std::string_view{}; tr_variantDictFindStrView(&top, TR_KEY_id, &sv) && std::size(sv) == std::size(id))
            {
                std::copy(std::begin(sv), std::end(sv), std::begin(id));
            }

            size_t raw_len = 0U;
            std::byte const* raw = nullptr;
            if (tr_variantDictFindRaw(&top, TR_KEY_nodes, &raw, &raw_len) && raw_len % 6 == 0)
            {
                auto* walk = raw;
                auto const* const end = raw + raw_len;
                while (walk < end)
                {
                    auto addr = tr_address{};
                    auto port = tr_port{};
                    std::tie(addr, walk) = tr_address::from_compact_ipv4(walk);
                    std::tie(port, walk) = tr_port::from_compact(walk);
                    nodes.emplace_back(addr, port);
                }
            }

            if (tr_variantDictFindRaw(&top, TR_KEY_nodes6, &raw, &raw_len) && raw_len % 18 == 0)
            {
                auto* walk = raw;
                auto const* const end = raw + raw_len;
                while (walk < end)
                {
                    auto addr = tr_address{};
                    auto port = tr_port{};
                    std::tie(addr, walk) = tr_address::from_compact_ipv6(walk);
                    std::tie(port, walk) = tr_port::from_compact(walk);
                    nodes.emplace_back(addr, port);
                }
            }
        }

        return std::make_pair(id, nodes);
    }

    ///

    static void get_nodes_from_bootstrap_file(std::string_view filename, Nodes& nodes)
    {
        auto in = std::ifstream{ std::string{ filename } };
        if (!in.is_open())
        {
            return;
        }

        // format is each line has host, a space char, and port number
        auto line = std::string{};
        while (std::getline(in, line))
        {
            auto line_stream = std::istringstream{ line };
            auto addrstr = std::string{};
            auto hport = uint16_t{};
            line_stream >> addrstr >> hport;

            if (line_stream.bad() || std::empty(addrstr))
            {
                tr_logAddWarn(fmt::format(
                    _("Couldn't parse '{filename}' line: '{line}'"),
                    fmt::arg("filename", filename),
                    fmt::arg("line", line)));
            }
            else
            {
                get_nodes_from_name(addrstr.c_str(), tr_port::from_host(hport), nodes);
            }
        }
    }

    static void get_nodes_from_name(char const* name, tr_port port_in, Nodes& nodes)
    {
        auto hints = addrinfo{};
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_family = AF_UNSPEC;
        hints.ai_protocol = 0;
        hints.ai_flags = 0;

        auto port_str = std::array<char, 16>{};
        *fmt::format_to(std::data(port_str), FMT_STRING("{:d}"), port_in.host()) = '\0';

        addrinfo* info = nullptr;
        if (int const rc = getaddrinfo(name, std::data(port_str), &hints, &info); rc != 0)
        {
            tr_logAddWarn(fmt::format(
                _("Couldn't look up '{address}:{port}': {error} ({error_code})"),
                fmt::arg("address", name),
                fmt::arg("port", port_in.host()),
                fmt::arg("error", gai_strerror(rc)),
                fmt::arg("error_code", rc)));
            return;
        }

        for (auto* infop = info; infop != nullptr; infop = infop->ai_next)
        {
            if (auto addrport = tr_socket_address::from_sockaddr(infop->ai_addr); addrport)
            {
                nodes.emplace_back(*addrport);
            }
        }

        freeaddrinfo(info);
    }

    ///

    tr_port const peer_port_;
    tr_socket_t const udp4_socket_;
    tr_socket_t const udp6_socket_;

    Mediator& mediator_;
    std::string const state_filename_;
    std::unique_ptr<libtransmission::Timer> const announce_timer_;
    std::unique_ptr<libtransmission::Timer> const bootstrap_timer_;
    std::unique_ptr<libtransmission::Timer> const periodic_timer_;

    Id id_ = {};

    Nodes bootstrap_queue_;
    size_t n_bootstrapped_ = 0;

    struct AnnounceInfo
    {
        time_t ipv4_announce_after = 0;
        time_t ipv6_announce_after = 0;
    };

    std::map<tr_torrent_id_t, AnnounceInfo> announce_times_;
};

[[nodiscard]] std::unique_ptr<tr_dht> tr_dht::create(
    Mediator& mediator,
    tr_port peer_port,
    tr_socket_t udp4_socket,
    tr_socket_t udp6_socket)
{
    return std::make_unique<tr_dht_impl>(mediator, peer_port, udp4_socket, udp6_socket);
}
