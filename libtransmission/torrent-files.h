// This file Copyright © 2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstddef>
#include <cstdint> // uint64_t
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "transmission.h"

#include "file.h"
#include "tr-strbuf.h"

struct tr_error;

/**
 * A simple collection of files & utils for finding them, moving them, etc.
 */
struct tr_torrent_files
{
public:
    [[nodiscard]] bool empty() const noexcept
    {
        return std::empty(files_);
    }

    [[nodiscard]] size_t fileCount() const noexcept
    {
        return std::size(files_);
    }

    [[nodiscard]] uint64_t fileSize(tr_file_index_t file_index) const
    {
        return files_.at(file_index).size_;
    }

    [[nodiscard]] constexpr auto totalSize() const noexcept
    {
        return total_size_;
    }

    [[nodiscard]] std::string const& path(tr_file_index_t file_index) const
    {
        return files_.at(file_index).path_;
    }

    void setPath(tr_file_index_t file_index, std::string_view path)
    {
        files_.at(file_index).setPath(path);
    }

    void reserve(size_t n_files)
    {
        files_.reserve(n_files);
    }

    void shrinkToFit()
    {
        files_.shrink_to_fit();
    }

    void clear() noexcept
    {
        files_.clear();
        total_size_ = uint64_t{};
    }

    tr_file_index_t add(std::string_view path, uint64_t file_size)
    {
        auto const ret = static_cast<tr_file_index_t>(std::size(files_));
        files_.emplace_back(path, file_size);
        total_size_ += file_size;
        return ret;
    }

    bool move(
        std::string_view old_top_in,
        std::string_view top_in,
        double volatile* setme_progress,
        std::string_view log_name = "",
        tr_error** error = nullptr) const;

    using FileFunc = std::function<void(char const* filename)>;
    void remove(std::string_view top_in, std::string_view tmpdir_prefix, FileFunc const& func) const;

    struct FoundFile : public tr_sys_path_info
    {
    public:
        FoundFile(tr_sys_path_info info, tr_pathbuf&& filename_in, size_t base_len_in)
            : tr_sys_path_info{ info }
            , filename_{ std::move(filename_in) }
            , base_len_{ base_len_in }
        {
        }

        [[nodiscard]] constexpr auto const& filename() const noexcept
        {
            // /home/foo/Downloads/torrent/01-file-one.txt
            return filename_;
        }

        [[nodiscard]] constexpr auto base() const noexcept
        {
            // /home/foo/Downloads
            return filename_.sv().substr(0, base_len_);
        }

        [[nodiscard]] constexpr auto subpath() const noexcept
        {
            // torrent/01-file-one.txt
            return filename_.sv().substr(base_len_ + 1);
        }

    private:
        tr_pathbuf filename_;
        size_t base_len_;
    };

    [[nodiscard]] std::optional<FoundFile> find(tr_file_index_t, std::string_view const* search_paths, size_t n_paths) const;
    [[nodiscard]] bool hasAnyLocalData(std::string_view const* search_paths, size_t n_paths) const;

    static constexpr std::string_view PartialFileSuffix = ".part";

private:
    struct file_t
    {
    public:
        void setPath(std::string_view subpath)
        {
            path_ = subpath;
        }

        file_t(std::string_view path, uint64_t size)
            : path_{ path }
            , size_{ size }
        {
        }

        std::string path_;
        uint64_t size_ = 0;
    };

    std::vector<file_t> files_;
    uint64_t total_size_ = 0;
};
