// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "objc_transmission.h"
#import <libtransmission/transmission.h>

int C_TR_RATIO_NA = TR_RATIO_NA;
int C_TR_RATIO_INF = TR_RATIO_INF;

void c_tr_sessionReloadBlocklists(c_tr_session const _Nonnull* _Nonnull session)
{
    tr_sessionReloadBlocklists((tr_session*)session);
}
void c_tr_sessionClose(c_tr_session const _Nonnull* _Nonnull session, size_t timeout_secs)
{
    tr_sessionClose((tr_session*)session, timeout_secs);
}
char const* c_tr_sessionGetConfigDir(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetConfigDir((tr_session*)session);
}
char const* c_tr_sessionGetDownloadDir(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetDownloadDir((tr_session*)session);
}
void c_tr_sessionSetDownloadDir(c_tr_session const _Nonnull* _Nonnull session, char const* download_dir)
{
    tr_sessionSetDownloadDir((tr_session*)session, download_dir);
}
char const* c_tr_sessionGetIncompleteDir(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetIncompleteDir((tr_session*)session);
}
void c_tr_sessionSetIncompleteDir(c_tr_session const _Nonnull* _Nonnull session, char const* dir)
{
    tr_sessionSetIncompleteDir((tr_session*)session, dir);
}
bool c_tr_sessionIsIncompleteDirEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionIsIncompleteDirEnabled((tr_session*)session);
}
void c_tr_sessionSetIncompleteDirEnabled(c_tr_session const _Nonnull* _Nonnull session, bool enabled)
{
    tr_sessionSetIncompleteDirEnabled((tr_session*)session, enabled);
}
bool c_tr_sessionIsIncompleteFileNamingEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionIsIncompleteFileNamingEnabled((tr_session*)session);
}
void c_tr_sessionSetIncompleteFileNamingEnabled(c_tr_session const _Nonnull* _Nonnull session, bool enabled)
{
    tr_sessionSetIncompleteFileNamingEnabled((tr_session*)session, enabled);
}
bool c_tr_sessionIsRPCEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionIsRPCEnabled((tr_session*)session);
}
void c_tr_sessionSetRPCEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled)
{
    tr_sessionSetRPCEnabled((tr_session*)session, is_enabled);
}
uint16_t c_tr_sessionGetRPCPort(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetRPCPort((tr_session*)session);
}
void c_tr_sessionSetRPCPort(c_tr_session const _Nonnull* _Nonnull session, uint16_t hport)
{
    tr_sessionSetRPCPort((tr_session*)session, hport);
}
char const* c_tr_sessionGetRPCWhitelist(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetRPCWhitelist((tr_session*)session);
}
void c_tr_sessionSetRPCWhitelist(c_tr_session const _Nonnull* _Nonnull session, char const* whitelist)
{
    tr_sessionSetRPCWhitelist((tr_session*)session, whitelist);
}
bool c_tr_sessionGetRPCWhitelistEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetRPCWhitelistEnabled((tr_session*)session);
}
void c_tr_sessionSetRPCWhitelistEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled)
{
    tr_sessionSetRPCWhitelistEnabled((tr_session*)session, is_enabled);
}
char const* c_tr_sessionGetRPCPassword(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetRPCPassword((tr_session*)session);
}
void c_tr_sessionSetRPCPassword(c_tr_session const _Nonnull* _Nonnull session, char const* password)
{
    tr_sessionSetRPCPassword((tr_session*)session, password);
}
char const* c_tr_sessionGetRPCUsername(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetRPCUsername((tr_session*)session);
}
void c_tr_sessionSetRPCUsername(c_tr_session const _Nonnull* _Nonnull session, char const* username)
{
    tr_sessionSetRPCUsername((tr_session*)session, username);
}
bool c_tr_sessionIsRPCPasswordEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionIsRPCPasswordEnabled((tr_session*)session);
}
void c_tr_sessionSetRPCPasswordEnabled(c_tr_session const _Nonnull* _Nonnull session, bool enabled)
{
    tr_sessionSetRPCPasswordEnabled((tr_session*)session, enabled);
}
void c_tr_sessionSetDefaultTrackers(c_tr_session const _Nonnull* _Nonnull session, char const* trackers)
{
    tr_sessionSetDefaultTrackers((tr_session*)session, trackers);
}

// https://stackoverflow.com/questions/3995940/casting-one-c-structure-into-another
static inline struct c_tr_session_stats c_tr_session_stats(struct tr_session_stats in)
{
    return (struct c_tr_session_stats){ in.ratio,      in.uploadedBytes, in.downloadedBytes,
                                        in.filesAdded, in.sessionCount,  in.secondsActive };
}

struct c_tr_session_stats c_tr_sessionGetStats(c_tr_session const _Nonnull* _Nonnull session)
{
    return c_tr_session_stats(tr_sessionGetStats((tr_session*)session));
}
struct c_tr_session_stats c_tr_sessionGetCumulativeStats(c_tr_session const _Nonnull* _Nonnull session)
{
    return c_tr_session_stats(tr_sessionGetCumulativeStats((tr_session*)session));
}
void c_tr_sessionClearStats(c_tr_session const _Nonnull* _Nonnull session)
{
    tr_sessionClearStats((tr_session*)session);
}

bool c_tr_sessionIsPexEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionIsPexEnabled((tr_session*)session);
}
void c_tr_sessionSetPexEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled)
{
    tr_sessionSetPexEnabled((tr_session*)session, is_enabled);
}
bool c_tr_sessionIsDHTEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionIsDHTEnabled((tr_session*)session);
}
void c_tr_sessionSetDHTEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled)
{
    tr_sessionSetDHTEnabled((tr_session*)session, is_enabled);
}
bool c_tr_sessionIsUTPEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionIsUTPEnabled((tr_session*)session);
}
void c_tr_sessionSetUTPEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled)
{
    tr_sessionSetUTPEnabled((tr_session*)session, is_enabled);
}
bool c_tr_sessionIsLPDEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionIsLPDEnabled((tr_session*)session);
}
void c_tr_sessionSetLPDEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled)
{
    tr_sessionSetLPDEnabled((tr_session*)session, is_enabled);
}
size_t c_tr_sessionGetCacheLimit_MB(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetCacheLimit_MB((tr_session*)session);
}
void c_tr_sessionSetCacheLimit_MB(c_tr_session const _Nonnull* _Nonnull session, size_t mb)
{
    tr_sessionSetCacheLimit_MB((tr_session*)session, mb);
}
enum c_tr_encryption_mode c_tr_sessionGetEncryption(c_tr_session const _Nonnull* _Nonnull session)
{
    return (enum c_tr_encryption_mode)tr_sessionGetEncryption((tr_session*)session);
}
void c_tr_sessionSetEncryption(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_encryption_mode mode)
{
    tr_sessionSetEncryption((tr_session*)session, (enum tr_encryption_mode)mode);
}

#pragma mark Incoming Peer Connections Port

bool c_tr_sessionIsPortForwardingEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionIsPortForwardingEnabled((tr_session*)session);
}
void c_tr_sessionSetPortForwardingEnabled(c_tr_session const _Nonnull* _Nonnull session, bool enabled)
{
    tr_sessionSetPortForwardingEnabled((tr_session*)session, enabled);
}
uint16_t c_tr_sessionGetPeerPort(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetPeerPort((tr_session*)session);
}
void c_tr_sessionSetPeerPort(c_tr_session const _Nonnull* _Nonnull session, uint16_t port)
{
    tr_sessionSetPeerPort((tr_session*)session, port);
}
uint16_t c_tr_sessionSetPeerPortRandom(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionSetPeerPortRandom((tr_session*)session);
}
bool c_tr_sessionGetPeerPortRandomOnStart(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetPeerPortRandomOnStart((tr_session*)session);
}
void c_tr_sessionSetPeerPortRandomOnStart(c_tr_session const _Nonnull* _Nonnull session, bool random)
{
    tr_sessionSetPeerPortRandomOnStart((tr_session*)session, random);
}

enum c_tr_port_forwarding_state c_tr_sessionGetPortForwarding(c_tr_session const _Nonnull* _Nonnull session)
{
    return (enum c_tr_port_forwarding_state)tr_sessionGetPortForwarding((tr_session*)session);
}

tr_kilobytes_per_second_t c_tr_sessionGetSpeedLimit_KBps(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir)
{
    return tr_sessionGetSpeedLimit_KBps((tr_session*)session, (tr_direction)dir);
}
void c_tr_sessionSetSpeedLimit_KBps(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir, tr_kilobytes_per_second_t limit)
{
    tr_sessionSetSpeedLimit_KBps((tr_session*)session, (tr_direction)dir, limit);
}
bool c_tr_sessionIsSpeedLimited(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir)
{
    return tr_sessionIsSpeedLimited((tr_session*)session, (enum tr_direction)dir);
}
void c_tr_sessionLimitSpeed(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir, bool limited)
{
    tr_sessionLimitSpeed((tr_session*)session, (tr_direction)dir, limited);
}
tr_kilobytes_per_second_t c_tr_sessionGetAltSpeed_KBps(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir)
{
    return tr_sessionGetAltSpeed_KBps((tr_session*)session, (tr_direction)dir);
}
void c_tr_sessionSetAltSpeed_KBps(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir, tr_bytes_per_second_t limit)
{
    tr_sessionSetAltSpeed_KBps((tr_session*)session, (tr_direction)dir, limit);
}
bool c_tr_sessionUsesAltSpeed(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionUsesAltSpeed((tr_session*)session);
}
void c_tr_sessionUseAltSpeed(c_tr_session const _Nonnull* _Nonnull session, bool enabled)
{
    tr_sessionUseAltSpeed((tr_session*)session, enabled);
}
bool c_tr_sessionUsesAltSpeedTime(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionUsesAltSpeedTime((tr_session*)session);
}
void c_tr_sessionUseAltSpeedTime(c_tr_session const _Nonnull* _Nonnull session, bool enabled)
{
    tr_sessionUseAltSpeedTime((tr_session*)session, enabled);
}
size_t c_tr_sessionGetAltSpeedBegin(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetAltSpeedBegin((tr_session*)session);
}
void c_tr_sessionSetAltSpeedBegin(c_tr_session const _Nonnull* _Nonnull session, size_t minutes_since_midnight)
{
    tr_sessionSetAltSpeedBegin((tr_session*)session, minutes_since_midnight);
}
size_t c_tr_sessionGetAltSpeedEnd(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetAltSpeedEnd((tr_session*)session);
}
void c_tr_sessionSetAltSpeedEnd(c_tr_session const _Nonnull* _Nonnull session, size_t minutes_since_midnight)
{
    tr_sessionSetAltSpeedEnd((tr_session*)session, minutes_since_midnight);
}

enum c_tr_sched_day c_tr_sessionGetAltSpeedDay(c_tr_session const _Nonnull* _Nonnull session)
{
    return (c_tr_sched_day)tr_sessionGetAltSpeedDay((tr_session*)session);
}
void c_tr_sessionSetAltSpeedDay(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_sched_day day)
{
    tr_sessionSetAltSpeedDay((tr_session*)session, (tr_sched_day)day);
}
double c_tr_sessionGetRawSpeed_KBps(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir)
{
    return tr_sessionGetRawSpeed_KBps((tr_session*)session, (enum tr_direction)dir);
}
bool c_tr_sessionIsRatioLimited(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionIsRatioLimited((tr_session*)session);
}
void c_tr_sessionSetRatioLimited(c_tr_session const _Nonnull* _Nonnull session, bool is_limited)
{
    tr_sessionSetRatioLimited((tr_session*)session, is_limited);
}
double c_tr_sessionGetRatioLimit(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetRatioLimit((tr_session*)session);
}
void c_tr_sessionSetRatioLimit(c_tr_session const _Nonnull* _Nonnull session, double desired_ratio)
{
    tr_sessionSetRatioLimit((tr_session*)session, desired_ratio);
}
bool c_tr_sessionIsIdleLimited(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionIsIdleLimited((tr_session*)session);
}
void c_tr_sessionSetIdleLimited(c_tr_session const _Nonnull* _Nonnull session, bool is_limited)
{
    tr_sessionSetIdleLimited((tr_session*)session, is_limited);
}
uint16_t c_tr_sessionGetIdleLimit(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetIdleLimit((tr_session*)session);
}
void c_tr_sessionSetIdleLimit(c_tr_session const _Nonnull* _Nonnull session, uint16_t idle_minutes)
{
    tr_sessionSetIdleLimit((tr_session*)session, idle_minutes);
}
uint16_t c_tr_sessionGetPeerLimit(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetPeerLimit((tr_session*)session);
}
void c_tr_sessionSetPeerLimit(c_tr_session const _Nonnull* _Nonnull session, uint16_t max_global_peers)
{
    tr_sessionSetPeerLimit((tr_session*)session, max_global_peers);
}
uint16_t c_tr_sessionGetPeerLimitPerTorrent(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetPeerLimitPerTorrent((tr_session*)session);
}
void c_tr_sessionSetPeerLimitPerTorrent(c_tr_session const _Nonnull* _Nonnull session, uint16_t max_peers)
{
    tr_sessionSetPeerLimitPerTorrent((tr_session*)session, max_peers);
}
bool c_tr_sessionGetPaused(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetPaused((tr_session*)session);
}
void c_tr_sessionSetPaused(c_tr_session const _Nonnull* _Nonnull session, bool is_paused)
{
    tr_sessionSetPaused((tr_session*)session, is_paused);
}
void c_tr_sessionSetDeleteSource(c_tr_session const _Nonnull* _Nonnull session, bool delete_source)
{
    tr_sessionSetDeleteSource((tr_session*)session, delete_source);
}
int c_tr_sessionGetAntiBruteForceThreshold(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetAntiBruteForceThreshold((tr_session*)session);
}
void c_tr_sessionSetAntiBruteForceThreshold(c_tr_session const _Nonnull* _Nonnull session, int max_bad_requests)
{
    tr_sessionSetAntiBruteForceThreshold((tr_session*)session, max_bad_requests);
}
bool c_tr_sessionGetAntiBruteForceEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetAntiBruteForceEnabled((tr_session*)session);
}
void c_tr_sessionSetAntiBruteForceEnabled(c_tr_session const _Nonnull* _Nonnull session, bool enabled)
{
    tr_sessionSetAntiBruteForceEnabled((tr_session*)session, enabled);
}

size_t c_tr_sessionGetQueueSize(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir)
{
    return tr_sessionGetQueueSize((tr_session*)session, (enum tr_direction)dir);
}
void c_tr_sessionSetQueueSize(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir, size_t max_simultaneous_torrents)
{
    tr_sessionSetQueueSize((tr_session*)session, (tr_direction)dir, max_simultaneous_torrents);
}
bool c_tr_sessionGetQueueEnabled(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir)
{
    return tr_sessionGetQueueEnabled((tr_session*)session, (enum tr_direction)dir);
}
void c_tr_sessionSetQueueEnabled(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_direction dir, bool do_limit_simultaneous_torrents)
{
    tr_sessionSetQueueEnabled((tr_session*)session, (tr_direction)dir, do_limit_simultaneous_torrents);
}
size_t c_tr_sessionGetQueueStalledMinutes(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetQueueStalledMinutes((tr_session*)session);
}
void c_tr_sessionSetQueueStalledMinutes(c_tr_session const _Nonnull* _Nonnull session, int minutes)
{
    tr_sessionSetQueueStalledMinutes((tr_session*)session, minutes);
}
bool c_tr_sessionGetQueueStalledEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_sessionGetQueueStalledEnabled((tr_session*)session);
}
void c_tr_sessionSetQueueStalledEnabled(c_tr_session const _Nonnull* _Nonnull session, bool enabled)
{
    tr_sessionSetQueueStalledEnabled((tr_session*)session, enabled);
}

#pragma mark -

char const* c_tr_sessionGetScript(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_script type)
{
    return tr_sessionGetScript((tr_session*)session, (enum TrScript)type);
}
void c_tr_sessionSetScript(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_script type, char const* script_filename)
{
    tr_sessionSetScript((tr_session*)session, (enum TrScript)type, script_filename);
}
bool c_tr_sessionIsScriptEnabled(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_script type)
{
    return tr_sessionIsScriptEnabled((tr_session*)session, (enum TrScript)type);
}
void c_tr_sessionSetScriptEnabled(c_tr_session const _Nonnull* _Nonnull session, enum c_tr_script type, bool enabled)
{
    tr_sessionSetScriptEnabled((tr_session*)session, (enum TrScript)type, enabled);
}

#pragma mark -

size_t c_tr_blocklistSetContent(c_tr_session const _Nonnull* _Nonnull session, char const* content_filename)
{
    return tr_blocklistSetContent((tr_session*)session, content_filename);
}
size_t c_tr_blocklistGetRuleCount(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_blocklistGetRuleCount((tr_session*)session);
}
bool c_tr_blocklistExists(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_blocklistExists((tr_session*)session);
}
bool c_tr_blocklistIsEnabled(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_blocklistIsEnabled((tr_session*)session);
}
void c_tr_blocklistSetEnabled(c_tr_session const _Nonnull* _Nonnull session, bool is_enabled)
{
    tr_blocklistSetEnabled((tr_session*)session, is_enabled);
}
char const* c_tr_blocklistGetURL(c_tr_session const _Nonnull* _Nonnull session)
{
    return tr_blocklistGetURL((tr_session*)session);
}
void c_tr_blocklistSetURL(c_tr_session const _Nonnull* _Nonnull session, char const* url)
{
    tr_blocklistSetURL((tr_session*)session, url);
}

char const* C_DEFAULT_BLOCKLIST_FILENAME = DEFAULT_BLOCKLIST_FILENAME;

#pragma mark -
