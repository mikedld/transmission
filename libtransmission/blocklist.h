// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <algorithm> // std::any_of
#include <cstddef> // size_t
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility> // for std::pair
#include <vector>

#include "libtransmission/tr-macros.h" // for TR_CONSTEXPR20
#include "libtransmission/net.h" // for tr_address
#include "libtransmission/observable.h"

namespace libtransmission
{

class Blocklists
{
public:
    Blocklists() = default;

    [[nodiscard]] bool contains(tr_address const& addr) const noexcept
    {
        return std::any_of(
            std::begin(blocklists_),
            std::end(blocklists_),
            [&addr](auto const& blocklist) { return blocklist.enabled() && blocklist.contains(addr); });
    }

    [[nodiscard]] TR_CONSTEXPR20 auto num_lists() const noexcept
    {
        return std::size(blocklists_);
    }

    [[nodiscard]] TR_CONSTEXPR20 auto num_rules() const noexcept
    {
        return std::accumulate(
            std::begin(blocklists_),
            std::end(blocklists_),
            size_t{},
            [](int sum, auto& cur) { return sum + std::size(cur); });
    }

    void load(std::string_view folder, bool is_enabled);
    void set_enabled(bool is_enabled);
    size_t update_primary_blocklist(std::string_view external_file, bool is_enabled);

    template<typename Observer>
    [[nodiscard]] auto observe_changes(Observer observer)
    {
        return changed_.observe(std::move(observer));
    }

private:
    class Blocklist
    {
    public:
        static std::optional<Blocklist> saveNew(std::string_view external_file, std::string_view bin_file, bool is_enabled);

        Blocklist() = default;

        Blocklist(std::string_view bin_file, bool is_enabled)
            : bin_file_{ bin_file }
            , is_enabled_{ is_enabled }
        {
        }

        [[nodiscard]] bool contains(tr_address const& addr) const;

        [[nodiscard]] size_t size() const
        {
            ensureLoaded();

            return std::size(rules_);
        }

        [[nodiscard]] constexpr bool enabled() const noexcept
        {
            return is_enabled_;
        }

        constexpr void setEnabled(bool is_enabled) noexcept
        {
            is_enabled_ = is_enabled;
        }

        [[nodiscard]] constexpr auto const& binFile() const noexcept
        {
            return bin_file_;
        }

    private:
        void ensureLoaded() const;

        mutable std::vector<std::pair<tr_address, tr_address>> rules_;

        std::string bin_file_;
        bool is_enabled_ = false;
    };

    std::vector<Blocklist> blocklists_;

    std::string folder_;

    libtransmission::SimpleObservable<> changed_;

    [[nodiscard]] static std::vector<Blocklist> load_folder(std::string_view folder, bool is_enabled);
};
} // namespace libtransmission
