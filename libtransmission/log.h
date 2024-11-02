// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <chrono>
#include <cstddef> // size_t
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

// ---

enum tr_log_level
{
    // No logging at all
    TR_LOG_OFF,

    // Errors that prevent Transmission from running
    TR_LOG_CRITICAL,

    // Errors that could prevent a single torrent from running, e.g. missing
    // files or a private torrent's tracker responding "unregistered torrent"
    TR_LOG_ERROR,

    // Smaller errors that don't stop the overall system,
    // e.g. unable to preallocate a file, or unable to connect to a tracker
    // when other trackers are available
    TR_LOG_WARN,

    // User-visible info, e.g. "torrent completed" or "running script"
    TR_LOG_INFO,

    // Debug messages
    TR_LOG_DEBUG,

    // High-volume debug messages, e.g. tracing peer protocol messages
    TR_LOG_TRACE
};

std::optional<tr_log_level> tr_logGetLevelFromKey(std::string_view key);

// ---

struct tr_log_message
{
    tr_log_level level;

    // location in the source code
    std::string_view file;
    long line;

    // when the message was generated
    std::chrono::system_clock::time_point when;

    // torrent name or code module name associated with the message
    std::string name;

    // the message
    std::string message;

    // linked list of messages
    struct tr_log_message* next;
};

// ---

#define TR_LOG_MAX_QUEUE_LENGTH 10000

void tr_logSetQueueEnabled(bool is_enabled);

[[nodiscard]] tr_log_message* tr_logGetQueue();

void tr_logFreeQueue(tr_log_message* freeme);

// ---

void tr_logSetLevel(tr_log_level);

[[nodiscard]] tr_log_level tr_logGetLevel();

[[nodiscard]] bool tr_logLevelIsActive(tr_log_level level);

// ---

void tr_logAddMessage(
    char const* source_file,
    long source_line,
    tr_log_level level,
    std::string&& msg,
    std::string_view module_name = {});

#define tr_logAddLevel(level, ...) \
    do \
    { \
        if (tr_logLevelIsActive(level)) \
        { \
            tr_logAddMessage(__FILE__, __LINE__, level, __VA_ARGS__); \
        } \
    } while (0)

#define tr_logAddCritical(...) tr_logAddLevel(TR_LOG_CRITICAL, __VA_ARGS__)
#define tr_logAddError(...) tr_logAddLevel(TR_LOG_ERROR, __VA_ARGS__)
#define tr_logAddWarn(...) tr_logAddLevel(TR_LOG_WARN, __VA_ARGS__)
#define tr_logAddInfo(...) tr_logAddLevel(TR_LOG_INFO, __VA_ARGS__)
#define tr_logAddDebug(...) tr_logAddLevel(TR_LOG_DEBUG, __VA_ARGS__)
#define tr_logAddTrace(...) tr_logAddLevel(TR_LOG_TRACE, __VA_ARGS__)

// ---

std::string_view tr_logGetTimeStr(std::chrono::system_clock::time_point now, char* buf, size_t buflen);
std::string_view tr_logGetTimeStr(char* buf, size_t buflen);
