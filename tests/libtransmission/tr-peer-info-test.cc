// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <ctime>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

#include <libtransmission/transmission.h>

#include <libtransmission/net.h>
#include <libtransmission/peer-mgr.h>

#include "gtest/gtest.h"

using namespace std::literals;

using PeerInfoTest = ::testing::Test;

TEST_F(PeerInfoTest, mergeConnectable)
{
    // Same as the truth table in tr_peer_info::merge()
    static auto constexpr Tests = std::array{
        std::pair{ std::tuple{ std::optional{ true }, true, std::optional{ true }, true }, std::optional{ true } },
        std::pair{ std::tuple{ std::optional{ true }, true, std::optional{ true }, false }, std::optional{ true } },
        std::pair{ std::tuple{ std::optional{ true }, true, std::optional{ false }, false }, std::optional<bool>{} },
        std::pair{ std::tuple{ std::optional{ true }, true, std::optional<bool>{}, true }, std::optional{ true } },
        std::pair{ std::tuple{ std::optional{ true }, true, std::optional<bool>{}, false }, std::optional{ true } },
        std::pair{ std::tuple{ std::optional{ true }, false, std::optional{ true }, true }, std::optional{ true } },
        std::pair{ std::tuple{ std::optional{ true }, false, std::optional{ true }, false }, std::optional{ true } },
        std::pair{ std::tuple{ std::optional{ true }, false, std::optional{ false }, false }, std::optional<bool>{} },
        std::pair{ std::tuple{ std::optional{ true }, false, std::optional<bool>{}, true }, std::optional{ true } },
        std::pair{ std::tuple{ std::optional{ true }, false, std::optional<bool>{}, false }, std::optional{ true } },
        std::pair{ std::tuple{ std::optional{ false }, false, std::optional{ true }, true }, std::optional<bool>{} },
        std::pair{ std::tuple{ std::optional{ false }, false, std::optional{ true }, false }, std::optional<bool>{} },
        std::pair{ std::tuple{ std::optional{ false }, false, std::optional{ false }, false }, std::optional{ false } },
        std::pair{ std::tuple{ std::optional{ false }, false, std::optional<bool>{}, true }, std::optional<bool>{} },
        std::pair{ std::tuple{ std::optional{ false }, false, std::optional<bool>{}, false }, std::optional{ false } },
        std::pair{ std::tuple{ std::optional<bool>{}, true, std::optional{ true }, true }, std::optional{ true } },
        std::pair{ std::tuple{ std::optional<bool>{}, true, std::optional{ true }, false }, std::optional{ true } },
        std::pair{ std::tuple{ std::optional<bool>{}, true, std::optional{ false }, false }, std::optional<bool>{} },
        std::pair{ std::tuple{ std::optional<bool>{}, true, std::optional<bool>{}, true }, std::optional<bool>{} },
        std::pair{ std::tuple{ std::optional<bool>{}, true, std::optional<bool>{}, false }, std::optional<bool>{} },
        std::pair{ std::tuple{ std::optional<bool>{}, false, std::optional{ true }, true }, std::optional{ true } },
        std::pair{ std::tuple{ std::optional<bool>{}, false, std::optional{ true }, false }, std::optional{ true } },
        std::pair{ std::tuple{ std::optional<bool>{}, false, std::optional{ false }, false }, std::optional{ false } },
        std::pair{ std::tuple{ std::optional<bool>{}, false, std::optional<bool>{}, true }, std::optional<bool>{} },
        std::pair{ std::tuple{ std::optional<bool>{}, false, std::optional<bool>{}, false }, std::optional<bool>{} },
    };
    static_assert(std::size(Tests) == 25U);

    for (auto const& [condition, result] : Tests)
    {
        auto const& [this_connectable, this_connected, that_connectable, that_connected] = condition;

        auto info_this = tr_peer_info{ tr_address{}, 0, TR_PEER_FROM_PEX, {} };
        auto info_that = tr_peer_info{ tr_address{}, 0, TR_PEER_FROM_PEX, {} };

        if (this_connectable)
        {
            info_this.set_connectable(*this_connectable);
        }
        info_this.set_connected(time_t{}, this_connected);

        if (that_connectable)
        {
            info_that.set_connectable(*that_connectable);
        }
        info_that.set_connected(time_t{}, that_connected);

        info_this.merge(info_that);

        EXPECT_EQ(info_this.is_connectable(), result);
    }
}

TEST_F(PeerInfoTest, updateCanonicalPriority)
{
    static auto constexpr Tests = std::array{
        // crc32-c(624C14007BD50000), default mask
        std::tuple{ "123.213.32.10:51413"sv, "98.76.54.32:6881"sv, uint32_t{ 0xEC2D7224 } },
        // crc32-c(7BD520007BD52140), /16 mask
        std::tuple{ "123.213.32.10:51413"sv, "123.213.33.234:6881"sv, uint32_t{ 0xF61850A9 } },
        // crc32-c(7BD5200A7BD520EA), /24 mask
        std::tuple{ "123.213.32.10:51413"sv, "123.213.32.234:6881"sv, uint32_t{ 0x99568189 } },
        // crc32-c(1AE1C8D5), peer port
        std::tuple{ "123.213.32.10:51413"sv, "123.213.32.10:6881"sv, uint32_t{ 0x9F852E9F } },
        // crc32-c(2A032880F177010550441004000005542A032880F27701455044100400000145), default mask
        std::tuple{ "[2a03:2880:f277:1cd:face:b00c:0:167]:51413"sv,
                    "[2a03:2880:f177:185:face:b00c:0:25de]:6881"sv,
                    uint32_t{ 0xE4D0F2E2 } },
        // crc32-c(2A032880F177F14550441004000001452A032880F177F2055044100400000554), /48 mask
        std::tuple{ "[2a03:2880:f177:f1cd:face:b00c:0:167]:51413"sv,
                    "[2a03:2880:f177:f285:face:b00c:0:25de]:6881"sv,
                    uint32_t{ 0xCD993D35 } },
        // crc32-c(2A032880F177018550441004000005542A032880F17701CD5044100400000145), /56 mask
        std::tuple{ "[2a03:2880:f177:1cd:face:b00c:0:167]:51413"sv,
                    "[2a03:2880:f177:185:face:b00c:0:25de]:6881"sv,
                    uint32_t{ 0xF47E9889 } },
        // crc32-c(2A032880F17701CDFA441004000001452A032880F17701CDFB44100400000554), /64 mask
        std::tuple{ "[2a03:2880:f177:1cd:face:b00c:0:167]:51413"sv,
                    "[2a03:2880:f177:1cd:fbce:b00c:0:25de]:6881"sv,
                    uint32_t{ 0x2BC35C30 } },
        // crc32-c(2A032880F17701CDFACE1004000001452A032880F17701CDFACF100400000554), /72 mask
        std::tuple{ "[2a03:2880:f177:1cd:face:b00c:0:167]:51413"sv,
                    "[2a03:2880:f177:1cd:facf:b00c:0:25de]:6881"sv,
                    uint32_t{ 0x04BFAA11 } },
        // crc32-c(2A032880F17701CDFACEB005000005542A032880F17701CDFACEB50400000145), /80 mask
        std::tuple{ "[2a03:2880:f177:1cd:face:b50c:0:167]:51413"sv,
                    "[2a03:2880:f177:1cd:face:b00d:0:25de]:6881"sv,
                    uint32_t{ 0xA0F96012 } },
        // crc32-c(2A032880F17701CDFACEB00C000001452A032880F17701CDFACEB00D00000554), /88 mask
        std::tuple{ "[2a03:2880:f177:1cd:face:b00c:0:167]:51413"sv,
                    "[2a03:2880:f177:1cd:face:b00d:0:25de]:6881"sv,
                    uint32_t{ 0x47FC5342 } },
        // crc32-c(2A032880F17701CDFACEB00C005505542A032880F17701CDFACEB00CFF000145), /96 mask
        std::tuple{ "[2a03:2880:f177:1cd:face:b00c:ff00:167]:51413"sv,
                    "[2a03:2880:f177:1cd:face:b00c:ff:25de]:6881"sv,
                    uint32_t{ 0x51BB5B16 } },
        // crc32-c(2A032880F17701CDFACEB00C000001452A032880F17701CDFACEB00C00010554), /104 mask
        std::tuple{ "[2a03:2880:f177:1cd:face:b00c:0:167]:51413"sv,
                    "[2a03:2880:f177:1cd:face:b00c:1:25de]:6881"sv,
                    uint32_t{ 0xDAACAE90 } },
        // crc32-c(2A032880F17701CDFACEB00C000001452A032880F17701CDFACEB00C00002554), /112 mask
        std::tuple{ "[2a03:2880:f177:1cd:face:b00c:0:167]:51413"sv,
                    "[2a03:2880:f177:1cd:face:b00c:0:25de]:6881"sv,
                    uint32_t{ 0x0066DFEC } },
        // crc32-c(2A032880F17701CDFACEB00C000001452A032880F17701CDFACEB00C00001654), /120 mask
        std::tuple{ "[2a03:2880:f177:1cd:face:b00c:0:167]:51413"sv,
                    "[2a03:2880:f177:1cd:face:b00c:0:16de]:6881"sv,
                    uint32_t{ 0x74CF65F6 } },
        // crc32-c(1AE11AE2), peer port
        std::tuple{ "[2a03:2880:f177:1cd:face:b00c:0:167]:6882"sv,
                    "[2a03:2880:f177:1cd:face:b00c:0:167]:6881"sv,
                    uint32_t{ 0x67F8FE57 } },
    };

    for (auto [client_sockaddr_str, peer_sockaddr_str, expected] : Tests)
    {
        auto client_sockaddr = tr_socket_address::from_string(client_sockaddr_str);
        auto peer_sockaddr = tr_socket_address::from_string(peer_sockaddr_str);
        EXPECT_TRUE(client_sockaddr && peer_sockaddr) << "Test case is bugged";
        if (!client_sockaddr || !peer_sockaddr)
        {
            continue;
        }

        auto const info = tr_peer_info{ *peer_sockaddr,
                                        0,
                                        TR_PEER_FROM_PEX,
                                        client_sockaddr->address(),
                                        [&client_sockaddr]
                                        {
                                            return client_sockaddr->port();
                                        } };
        EXPECT_EQ(info.get_canonical_priority(), expected);
    }
}
