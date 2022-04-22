// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "transmission.h"

#include "net.h"

struct event;
struct evhttp;
struct tr_variant;
struct tr_rpc_address;
struct libdeflate_compressor;

class tr_rpc_server
{
public:
    tr_rpc_server(tr_session* session, tr_variant* settings);
    ~tr_rpc_server();

    tr_rpc_server(tr_rpc_server&) = delete;
    tr_rpc_server(tr_rpc_server&&) = delete;
    tr_rpc_server& operator=(tr_rpc_server&) = delete;
    tr_rpc_server& operator=(tr_rpc_server&&) = delete;

    [[nodiscard]] constexpr tr_port port() const noexcept
    {
        return port_;
    }

    void setPort(tr_port) noexcept;

    [[nodiscard]] constexpr auto isEnabled() const noexcept
    {
        return is_enabled_;
    }

    void setEnabled(bool is_enabled);

    [[nodiscard]] constexpr auto isWhitelistEnabled() const noexcept
    {
        return is_whitelist_enabled_;
    }

    constexpr void setWhitelistEnabled(bool is_whitelist_enabled) noexcept
    {
        is_whitelist_enabled_ = is_whitelist_enabled;
    }

    [[nodiscard]] constexpr auto const& whitelist() const noexcept
    {
        return whitelist_str_;
    }

    void setWhitelist(std::string_view whitelist);

    [[nodiscard]] constexpr auto const& username() const noexcept
    {
        return username_;
    }

    void setUsername(std::string_view username);

    [[nodiscard]] constexpr auto isPasswordEnabled() const noexcept
    {
        return is_password_enabled_;
    }

    void setPasswordEnabled(bool enabled);

    [[nodiscard]] constexpr auto const& getSaltedPassword() const noexcept
    {
        return salted_password_;
    }

    void setPassword(std::string_view salted) noexcept;

    [[nodiscard]] constexpr auto isAntiBruteForceEnabled() const noexcept
    {
        return is_anti_brute_force_enabled_;
    }

    void setAntiBruteForceEnabled(bool enabled) noexcept;

    [[nodiscard]] constexpr auto getAntiBruteForceLimit() const noexcept
    {
        return anti_brute_force_limit_;
    }

    constexpr void setAntiBruteForceLimit(int limit) noexcept
    {
        anti_brute_force_limit_ = limit;
    }

    std::shared_ptr<libdeflate_compressor> compressor;

    [[nodiscard]] constexpr auto const& url() const noexcept
    {
        return url_;
    }

    void setUrl(std::string_view url);

    [[nodiscard]] std::string getBindAddress() const;

    [[nodiscard]] constexpr auto socketMode() const noexcept
    {
        return socket_mode_;
    }

    std::vector<std::string> hostWhitelist;
    std::vector<std::string> whitelist_;
    std::string salted_password_;
    std::string username_;
    std::string whitelist_str_;
    std::string url_;

    std::unique_ptr<struct tr_rpc_address> bindAddress;

    struct event* start_retry_timer = nullptr;
    struct evhttp* httpd = nullptr;
    tr_session* const session;

    int anti_brute_force_limit_ = 0;
    int login_attempts_ = 0;
    int start_retry_counter = 0;
    static int constexpr DefaultRpcSocketMode = 0750;
    int socket_mode_ = DefaultRpcSocketMode;

    tr_port port_;

    bool is_anti_brute_force_enabled_ = false;
    bool is_enabled_ = false;
    bool isHostWhitelistEnabled = false;
    bool is_password_enabled_ = false;
    bool is_whitelist_enabled_ = false;
};
