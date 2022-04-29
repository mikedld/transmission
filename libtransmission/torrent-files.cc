// This file Copyright © 2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "transmission.h"

#include "error.h"
#include "log.h"
#include "torrent-files.h"
#include "utils.h"

using namespace std::literals;

namespace
{

using file_func_t = std::function<void(char const* filename)>;

bool isDirectory(char const* path)
{
    auto info = tr_sys_path_info{};
    return tr_sys_path_get_info(path, 0, &info) && (info.type == TR_SYS_PATH_IS_DIRECTORY);
}

bool isEmptyDirectory(char const* path)
{
    if (!isDirectory(path))
    {
        return false;
    }

    if (auto const odir = tr_sys_dir_open(path); odir != TR_BAD_SYS_DIR)
    {
        char const* name_cstr = nullptr;
        while ((name_cstr = tr_sys_dir_read_name(odir)) != nullptr)
        {
            auto const name = std::string_view{ name_cstr };
            if (name != "." && name != "..")
            {
                tr_sys_dir_close(odir);
                return false;
            }
        }
        tr_sys_dir_close(odir);
    }

    return true;
}

void depthFirstWalk(char const* path, file_func_t const& func, std::optional<int> max_depth = {})
{
    if (isDirectory(path) && (!max_depth || *max_depth > 0))
    {
        if (auto const odir = tr_sys_dir_open(path); odir != TR_BAD_SYS_DIR)
        {
            char const* name_cstr = nullptr;
            while ((name_cstr = tr_sys_dir_read_name(odir)) != nullptr)
            {
                auto const name = std::string_view{ name_cstr };
                if (name == "." || name == "..")
                {
                    continue;
                }

                depthFirstWalk(tr_pathbuf{ path, '/', name }.c_str(), func, max_depth ? *max_depth - 1 : max_depth);
            }

            tr_sys_dir_close(odir);
        }
    }

    func(path);
}

bool isJunkFile(std::string_view filename)
{
    auto const base = tr_sys_path_basename(filename);

#ifdef __APPLE__
    // check for resource forks. <http://web.archive.org/web/20101010051608/http://support.apple.com/kb/TA20578>
    if (tr_strvStartsWith(base, "._"sv))
    {
        return true;
    }
#endif

    auto constexpr Files = std::array<std::string_view, 3>{
        ".DS_Store"sv,
        "Thumbs.db"sv,
        "desktop.ini"sv,
    };

    return std::find(std::begin(Files), std::end(Files), base) != std::end(Files);
}

} // unnamed namespace

///

std::optional<tr_torrent_files::FoundFile> tr_torrent_files::find(
    tr_file_index_t file_index,
    std::string_view const* search_paths,
    size_t n_paths) const
{
    auto filename = tr_pathbuf{};
    auto file_info = tr_sys_path_info{};
    auto const& subpath = path(file_index);

    for (size_t path_idx = 0; path_idx < n_paths; ++path_idx)
    {
        auto const base = search_paths[path_idx];

        filename.assign(base, '/', subpath);
        if (tr_sys_path_get_info(filename, 0, &file_info))
        {
            return FoundFile{ file_info, std::move(filename), std::size(base) };
        }

        filename.assign(filename, base, '/', subpath, PartialFileSuffix);
        if (tr_sys_path_get_info(filename, 0, &file_info))
        {
            return FoundFile{ file_info, std::move(filename), std::size(base) };
        }
    }

    return {};
}

bool tr_torrent_files::hasAnyLocalData(std::string_view const* search_paths, size_t n_paths) const
{
    for (tr_file_index_t i = 0, n = fileCount(); i < n; ++i)
    {
        if (find(i, search_paths, n_paths))
        {
            return true;
        }
    }

    return false;
}

///

bool tr_torrent_files::move(
    std::string_view old_parent_in,
    std::string_view parent_in,
    double volatile* setme_progress,
    std::string_view log_name,
    tr_error** error) const
{
    if (setme_progress != nullptr)
    {
        *setme_progress = 0.0;
    }

    auto const old_parent = tr_pathbuf{ old_parent_in };
    auto const parent = tr_pathbuf{ parent_in };
    tr_logAddTrace(fmt::format(FMT_STRING("Moving files from '{:s}' to '{:s}'"), old_parent, parent), log_name);

    if (tr_sys_path_is_same(old_parent, parent))
    {
        return true;
    }

    if (!tr_sys_dir_create(parent, TR_SYS_DIR_CREATE_PARENTS, 0777, error))
    {
        return false;
    }

    auto const search_paths = std::array<std::string_view, 1>{ old_parent.sv() };

    auto const total_size = totalSize();
    auto err = bool{};
    auto bytes_moved = uint64_t{};

    for (tr_file_index_t i = 0, n = fileCount(); i < n; ++i)
    {
        auto const found = find(i, std::data(search_paths), std::size(search_paths));
        if (!found)
        {
            continue;
        }

        auto const& old_path = found->filename();
        auto const path = tr_pathbuf{ parent, '/', found->subpath() };
        tr_logAddTrace(fmt::format(FMT_STRING("Found file #{:d} '{:s}'"), i, old_path), log_name);

        if (tr_sys_path_is_same(old_path, path))
        {
            continue;
        }

        tr_logAddTrace(fmt::format(FMT_STRING("Moving file #{:d} to '{:s}'"), i, old_path, path), log_name);
        if (!tr_moveFile(old_path, path, error))
        {
            err = true;
            break;
        }

        if (setme_progress != nullptr && total_size > 0U)
        {
            bytes_moved += fileSize(i);
            *setme_progress = static_cast<double>(bytes_moved) / total_size;
        }
    }

    // after moving the files, remove any leftover empty directories
    if (!err)
    {
        auto const remove_empty_directories = [](char const* filename)
        {
            if (isEmptyDirectory(filename))
            {
                tr_sys_path_remove(filename, nullptr);
            }
        };

        remove(old_parent, "transmission-removed", remove_empty_directories);
    }

    return !err;
}

///

/**
 * This convoluted code does something (seemingly) simple:
 * remove the torrent's local files.
 *
 * Fun complications:
 * 1. Try to preserve the directory hierarchy in the recycle bin.
 * 2. If there are nontorrent files, don't delete them...
 * 3. ...unless the other files are "junk", such as .DS_Store
 */
void tr_torrent_files::remove(std::string_view parent_in, std::string_view tmpdir_prefix, FileFunc const& func) const
{
    auto const parent = tr_pathbuf{ parent_in };

    // don't try to delete local data if the directory's gone missing
    if (!tr_sys_path_exists(parent))
    {
        return;
    }

    // make a tmpdir
    auto tmpdir = tr_pathbuf{ parent, '/', tmpdir_prefix, "__XXXXXX"sv };
    tr_sys_dir_create_temp(std::data(tmpdir));

    // move the local data to the tmpdir
    auto const search_paths = std::array<std::string_view, 1>{ parent.sv() };
    for (tr_file_index_t idx = 0, n_files = fileCount(); idx < n_files; ++idx)
    {
        if (auto const found = find(idx, std::data(search_paths), std::size(search_paths)); found)
        {
            tr_moveFile(found->filename(), tr_pathbuf{ tmpdir, '/', found->subpath() });
        }
    }

    // Make a list of the top-level torrent files & folders
    // because we'll need it below in the 'remove junk' phase
    auto top_files = std::set<std::string>{};
    depthFirstWalk(
        tmpdir,
        [&parent, &tmpdir, &top_files](char const* filename)
        {
            if (tmpdir != filename)
            {
                top_files.emplace(tr_pathbuf{ parent, '/', tr_sys_path_basename(filename) });
            }
        },
        1);

    auto const func_wrapper = [&tmpdir, &func](char const* filename)
    {
        if (tmpdir != filename)
        {
            func(filename);
        }
    };

    // Remove the tmpdir.
    // Since `func` might send files to a recycle bin, try to preserve
    // the folder hierarchy by removing top-level files & folders first.
    // But that can fail -- e.g. `func` might refuse to remove nonempty
    // directories -- so plan B is to remove everything bottom-up.
    depthFirstWalk(tmpdir, func_wrapper, 1);
    depthFirstWalk(tmpdir, func_wrapper);
    tr_sys_path_remove(tmpdir);

    // OK we've removed the local data.
    // What's left are empty folders, junk, and user-generated files.
    // Remove the first two categories and leave the third alone.
    auto const remove_junk = [](char const* filename)
    {
        if (isEmptyDirectory(filename) || isJunkFile(filename))
        {
            tr_sys_path_remove(filename);
        }
    };
    for (auto const& filename : top_files)
    {
        depthFirstWalk(filename.c_str(), remove_junk);
    }
}
