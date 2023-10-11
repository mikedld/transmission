// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

/*
 * This file is the bridge (or shim) between C++ Transmission library header and ObjC for ObjC->Swift bridging headers.
 * It must be importable by the lowest Swift version supported by Transmission, which is the one from latest Xcode installable on current RECOMMENDED_MACOSX_DEPLOYMENT_TARGET.
 *
 * Example, on December 31st 2022, using https://xcodereleases.com data:
 * - latest stable is Xcode 14.2, which has a RECOMMENDED_MACOSX_DEPLOYMENT_TARGET of macOS 10.14.6.
 * - latest Xcode for macOS 10.14.6 is Xcode 11.3.1 which supports up to Swift 5.1.3,
 * so this file needs to be importable in bridging headers with Swift 5.1.3 and newer.
 *
 * Documentation is in original library headers <libtransmission/transmission.h>
 */

#import "objc.h"

NS_ASSUME_NONNULL_BEGIN
TR_EXTERN_C_BEGIN

typedef size_t tr_file_index_t;
typedef uint32_t tr_piece_index_t;
typedef uint32_t tr_block_index_t;
typedef uint64_t tr_byte_index_t;
typedef uint32_t tr_tracker_tier_t;
typedef uint32_t tr_tracker_id_t;
typedef int tr_torrent_id_t;
typedef size_t tr_bytes_per_second_t;
typedef size_t tr_kilobytes_per_second_t;
typedef uint16_t tr_mode_t;

//struct c_tr_error
//{
//    int code;
//    char* message;
//};

typedef void* c_tr_session;

enum c_tr_encryption_mode
{
    C_TR_CLEAR_PREFERRED,
    C_TR_ENCRYPTION_PREFERRED,
    C_TR_ENCRYPTION_REQUIRED
};

extern int C_TR_RATIO_NA;
extern int C_TR_RATIO_INF;

void c_tr_sessionReloadBlocklists(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionClose(c_tr_session const _Nonnull* _Nonnull session, size_t timeout_secs);
char const* c_tr_sessionGetConfigDir(c_tr_session const _Nonnull* _Nonnull session);
char const* c_tr_sessionGetDownloadDir(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetDownloadDir(c_tr_session const _Nonnull* _Nonnull session, char const* download_dir);
char const* c_tr_sessionGetIncompleteDir(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetIncompleteDir(c_tr_session const _Nonnull* _Nonnull session, char const* dir);
bool c_tr_sessionIsIncompleteDirEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetIncompleteDirEnabled(c_tr_session const _Nonnull* _Nonnull session, bool enabled);
bool c_tr_sessionIsIncompleteFileNamingEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetIncompleteFileNamingEnabled(c_tr_session const _Nonnull* _Nonnull session, bool enabled);
bool c_tr_sessionIsRPCEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetRPCEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled);
uint16_t c_tr_sessionGetRPCPort(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetRPCPort(c_tr_session const _Nonnull* _Nonnull session, uint16_t port);
char const* c_tr_sessionGetRPCWhitelist(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetRPCWhitelist(c_tr_session const _Nonnull* _Nonnull session, char const* whitelist);
bool c_tr_sessionGetRPCWhitelistEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetRPCWhitelistEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled);
char const* c_tr_sessionGetRPCPassword(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetRPCPassword(c_tr_session const _Nonnull* _Nonnull session, char const* password);
char const* c_tr_sessionGetRPCUsername(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetRPCUsername(c_tr_session const _Nonnull* _Nonnull session, char const* username);
bool c_tr_sessionIsRPCPasswordEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetRPCPasswordEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled);
void c_tr_sessionSetDefaultTrackers(c_tr_session const _Nonnull* _Nonnull session, char const* trackers);

struct c_tr_session_stats
{
    float ratio;
    uint64_t uploadedBytes;
    uint64_t downloadedBytes;
    uint64_t filesAdded;
    uint64_t sessionCount;
    uint64_t secondsActive;
};

struct c_tr_session_stats c_tr_sessionGetStats(c_tr_session const _Nonnull* _Nonnull session);
struct c_tr_session_stats c_tr_sessionGetCumulativeStats(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionClearStats(c_tr_session const _Nonnull* _Nonnull session);

bool c_tr_sessionIsPexEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetPexEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled);
bool c_tr_sessionIsDHTEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetDHTEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled);
bool c_tr_sessionIsUTPEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetUTPEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled);
bool c_tr_sessionIsLPDEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetLPDEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled);
size_t c_tr_sessionGetCacheLimit_MB(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetCacheLimit_MB(c_tr_session const _Nonnull* _Nonnull session, size_t mb);
enum c_tr_encryption_mode c_tr_sessionGetEncryption(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetEncryption(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_encryption_mode mode);

bool c_tr_sessionIsPortForwardingEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetPortForwardingEnabled(c_tr_session const _Nonnull* _Nonnull session, bool enabled);
uint16_t c_tr_sessionGetPeerPort(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetPeerPort(c_tr_session const _Nonnull* _Nonnull session, uint16_t port);
uint16_t c_tr_sessionSetPeerPortRandom(c_tr_session const _Nonnull* _Nonnull session);
bool c_tr_sessionGetPeerPortRandomOnStart(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetPeerPortRandomOnStart(c_tr_session const _Nonnull* _Nonnull session, bool random);

enum c_tr_port_forwarding_state
{
    C_TR_PORT_ERROR,
    C_TR_PORT_UNMAPPED,
    C_TR_PORT_UNMAPPING,
    C_TR_PORT_MAPPING,
    C_TR_PORT_MAPPED
};

enum c_tr_port_forwarding_state c_tr_sessionGetPortForwarding(c_tr_session const _Nonnull* _Nonnull session);

enum c_tr_direction
{
    C_TR_UP = 0,
    C_TR_DOWN = 1
};

tr_kilobytes_per_second_t c_tr_sessionGetSpeedLimit_KBps( // clang-format-15 compatibility
    c_tr_session const _Nonnull* _Nonnull session,
    enum c_tr_direction dir);
void c_tr_sessionSetSpeedLimit_KBps( // clang-format-15 compatibility
    c_tr_session const _Nonnull* _Nonnull session,
    enum c_tr_direction dir,
    tr_kilobytes_per_second_t limit);
bool c_tr_sessionIsSpeedLimited(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir);
void c_tr_sessionLimitSpeed(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir, bool limited);
tr_kilobytes_per_second_t c_tr_sessionGetAltSpeed_KBps(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir);
void c_tr_sessionSetAltSpeed_KBps( // clang-format-15 compatibility
    c_tr_session const _Nonnull* _Nonnull session,
    enum c_tr_direction dir,
    tr_kilobytes_per_second_t limit);
bool c_tr_sessionUsesAltSpeed(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionUseAltSpeed(c_tr_session const _Nonnull* _Nonnull session, bool enabled);
bool c_tr_sessionUsesAltSpeedTime(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionUseAltSpeedTime(c_tr_session const _Nonnull* _Nonnull session, bool enabled);
size_t c_tr_sessionGetAltSpeedBegin(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetAltSpeedBegin(c_tr_session const _Nonnull* _Nonnull session, size_t minutes_since_midnight);
size_t c_tr_sessionGetAltSpeedEnd(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetAltSpeedEnd(c_tr_session const _Nonnull* _Nonnull session, size_t minutes_since_midnight);

enum c_tr_sched_day
{
    C_TR_SCHED_SUN = (1 << 0),
    C_TR_SCHED_MON = (1 << 1),
    C_TR_SCHED_TUES = (1 << 2),
    C_TR_SCHED_WED = (1 << 3),
    C_TR_SCHED_THURS = (1 << 4),
    C_TR_SCHED_FRI = (1 << 5),
    C_TR_SCHED_SAT = (1 << 6),
    C_TR_SCHED_WEEKDAY = (C_TR_SCHED_MON | C_TR_SCHED_TUES | C_TR_SCHED_WED | C_TR_SCHED_THURS | C_TR_SCHED_FRI),
    C_TR_SCHED_WEEKEND = (C_TR_SCHED_SUN | C_TR_SCHED_SAT),
    C_TR_SCHED_ALL = (C_TR_SCHED_WEEKDAY | C_TR_SCHED_WEEKEND)
};

enum c_tr_sched_day c_tr_sessionGetAltSpeedDay(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetAltSpeedDay(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_sched_day day);
double c_tr_sessionGetRawSpeed_KBps(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir);
bool c_tr_sessionIsRatioLimited(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetRatioLimited(c_tr_session const _Nonnull* _Nonnull session, bool is_limited);
double c_tr_sessionGetRatioLimit(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetRatioLimit(c_tr_session const _Nonnull* _Nonnull session, double desired_ratio);
bool c_tr_sessionIsIdleLimited(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetIdleLimited(c_tr_session const _Nonnull* _Nonnull session, bool is_limited);
uint16_t c_tr_sessionGetIdleLimit(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetIdleLimit(c_tr_session const _Nonnull* _Nonnull session, uint16_t idle_minutes);
uint16_t c_tr_sessionGetPeerLimit(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetPeerLimit(c_tr_session const _Nonnull* _Nonnull session, uint16_t max_global_peers);
uint16_t c_tr_sessionGetPeerLimitPerTorrent(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetPeerLimitPerTorrent(c_tr_session const _Nonnull* _Nonnull session, uint16_t max_peers);
bool c_tr_sessionGetPaused(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetPaused(c_tr_session const _Nonnull* _Nonnull session, bool is_paused);
void c_tr_sessionSetDeleteSource(c_tr_session const _Nonnull* _Nonnull session, bool delete_source);
//tr_priority_t c_tr_torrentGetPriority(c_tr_torrent const* tor);
//void c_tr_torrentSetPriority(c_tr_torrent*, tr_priority_t priority);
int c_tr_sessionGetAntiBruteForceThreshold(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetAntiBruteForceThreshold(c_tr_session const _Nonnull* _Nonnull session, int max_bad_requests);
bool c_tr_sessionGetAntiBruteForceEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetAntiBruteForceEnabled(c_tr_session const _Nonnull* _Nonnull session, bool enabled);

size_t c_tr_sessionGetQueueSize(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir);
void c_tr_sessionSetQueueSize( // clang-format-15 compatibility
    c_tr_session const _Nonnull* _Nonnull session,
    enum c_tr_direction dir,
    size_t max_simultaneous_torrents);
bool c_tr_sessionGetQueueEnabled(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir);
void c_tr_sessionSetQueueEnabled( // clang-format-15 compatibility
    c_tr_session const _Nonnull* _Nonnull session,
    enum c_tr_direction dir,
    bool do_limit_simultaneous_torrents);
size_t c_tr_sessionGetQueueStalledMinutes(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetQueueStalledMinutes(c_tr_session const _Nonnull* _Nonnull session, int minutes);
bool c_tr_sessionGetQueueStalledEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_sessionSetQueueStalledEnabled(c_tr_session const _Nonnull* _Nonnull session, bool enabled);

enum c_tr_script
{
    C_TR_SCRIPT_ON_TORRENT_ADDED,
    C_TR_SCRIPT_ON_TORRENT_DONE,
    C_TR_SCRIPT_ON_TORRENT_DONE_SEEDING,
    C_TR_SCRIPT_N_TYPES
};
char const* c_tr_sessionGetScript(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_script type);
void c_tr_sessionSetScript(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_script type, char const* script_filename);
bool c_tr_sessionIsScriptEnabled(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_script type);
void c_tr_sessionSetScriptEnabled(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_script type, bool enabled);

size_t c_tr_blocklistSetContent(c_tr_session const _Nonnull* _Nonnull session, char const* content_filename);
size_t c_tr_blocklistGetRuleCount(c_tr_session const _Nonnull* _Nonnull session);
bool c_tr_blocklistExists(c_tr_session const _Nonnull* _Nonnull session);
bool c_tr_blocklistIsEnabled(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_blocklistSetEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled);
char const* c_tr_blocklistGetURL(c_tr_session const _Nonnull* _Nonnull session);
void c_tr_blocklistSetURL(c_tr_session const _Nonnull* _Nonnull session, char const* url);

extern char const* C_DEFAULT_BLOCKLIST_FILENAME;

//enum c_tr_ratiolimit
//{
//    C_TR_RATIOLIMIT_GLOBAL = 0,
//    C_TR_RATIOLIMIT_SINGLE = 1,
//    C_TR_RATIOLIMIT_UNLIMITED = 2
//};
//
//enum c_tr_ratiolimit c_tr_torrentGetRatioMode(c_tr_torrent const* tor);
//void c_tr_torrentSetRatioMode(c_tr_torrent* tor, enum c_tr_ratiolimit mode);
//double c_tr_torrentGetRatioLimit(c_tr_torrent const* tor);
//void c_tr_torrentSetRatioLimit(c_tr_torrent* tor, double desired_ratio);
//bool c_tr_torrentGetSeedRatio(c_tr_torrent const* tor, double* ratio);
//
//enum c_tr_idlelimit
//{
//    C_TR_IDLELIMIT_GLOBAL = 0,
//    C_TR_IDLELIMIT_SINGLE = 1,
//    C_TR_IDLELIMIT_UNLIMITED = 2
//};
//
//enum c_tr_idlelimit c_tr_torrentGetIdleMode(c_tr_torrent const* tor);
//void c_tr_torrentSetIdleMode(c_tr_torrent* tor, enum c_tr_idlelimit mode);
//uint16_t c_tr_torrentGetIdleLimit(c_tr_torrent const* tor);
//void c_tr_torrentSetIdleLimit(c_tr_torrent* tor, uint16_t idle_minutes);
//bool c_tr_torrentGetSeedIdle(c_tr_torrent const* tor, uint16_t* minutes);
//uint16_t c_tr_torrentGetPeerLimit(c_tr_torrent const* tor);
//void c_tr_torrentSetPeerLimit(c_tr_torrent* tor, uint16_t max_connected_peers);
//
//enum c_tr_completeness
//{
//    C_TR_LEECH,
//    C_TR_SEED,
//    C_TR_PARTIAL_SEED
//};

TR_EXTERN_C_END
NS_ASSUME_NONNULL_END
