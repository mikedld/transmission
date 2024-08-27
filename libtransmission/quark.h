// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstddef> // size_t
#include <optional>
#include <string_view>

/* Quarks — a 2-way association between a string and a unique integer identifier */
using tr_quark = size_t;

/*
 * Predefined Quarks.
 *
 * TODO: it would be nice to clean up all the naming inconsistencies
 * between RPC and settings. This will be a mess and we shouldn't be
 * in a hurry to do it.
 */
enum
{
    TR_KEY_NONE, /* represented as an empty string */
    TR_KEY_activeTorrentCount, /* rpc */
    TR_KEY_activity_date_kebab, /* resume file (legacy) */
    TR_KEY_activity_date_camel, /* rpc (deprecated) */
    TR_KEY_activity_date, /* rpc, resume file */
    TR_KEY_added, /* pex */
    TR_KEY_added_date_kebab, /* resume file (legacy) */
    TR_KEY_added_f, /* pex */
    TR_KEY_added6, /* pex */
    TR_KEY_added6_f, /* pex */
    TR_KEY_added_date_camel, /* rpc (deprecated) */
    TR_KEY_added_date, /* rpc, resume file */
    TR_KEY_address, /* rpc */
    TR_KEY_alt_speed_down, /* rpc, settings */
    TR_KEY_alt_speed_enabled, /* rpc, settings */
    TR_KEY_alt_speed_time_begin, /* rpc, settings */
    TR_KEY_alt_speed_time_day, /* rpc, settings */
    TR_KEY_alt_speed_time_enabled, /* rpc, settings */
    TR_KEY_alt_speed_time_end, /* rpc, settings */
    TR_KEY_alt_speed_up, /* rpc, settings */
    TR_KEY_announce, /* metainfo */
    TR_KEY_announce_ip, /* metainfo, settings */
    TR_KEY_announce_ip_enabled, /* metainfo, settings */
    TR_KEY_announce_list, /* metainfo */
    TR_KEY_announceState, /* rpc */
    TR_KEY_anti_brute_force_enabled, /* rpc */
    TR_KEY_anti_brute_force_threshold, /* rpc */
    TR_KEY_arguments, /* rpc */
    TR_KEY_availability, // rpc
    TR_KEY_bandwidth_priority_kebab,
    TR_KEY_bandwidth_priority_camel,
    TR_KEY_bandwidth_priority,
    TR_KEY_beginPiece,
    TR_KEY_bind_address_ipv4,
    TR_KEY_bind_address_ipv6,
    TR_KEY_bitfield,
    TR_KEY_blocklist_date,
    TR_KEY_blocklist_enabled,
    TR_KEY_blocklist_size,
    TR_KEY_blocklist_updates_enabled,
    TR_KEY_blocklist_url,
    TR_KEY_blocks,
    TR_KEY_bytesCompleted,
    TR_KEY_cache_size_mb,
    TR_KEY_clientIsChoked,
    TR_KEY_clientIsInterested,
    TR_KEY_clientName,
    TR_KEY_comment,
    TR_KEY_comment_utf_8,
    TR_KEY_compact_view,
    TR_KEY_complete,
    TR_KEY_config_dir,
    TR_KEY_cookies,
    TR_KEY_corrupt,
    TR_KEY_corrupt_ever_camel,
    TR_KEY_corrupt_ever,
    TR_KEY_created_by,
    TR_KEY_created_by_utf_8,
    TR_KEY_creation_date,
    TR_KEY_creator,
    TR_KEY_cumulative_stats,
    TR_KEY_current_stats,
    TR_KEY_date,
    TR_KEY_date_created_camel,
    TR_KEY_date_created,
    TR_KEY_default_trackers,
    TR_KEY_delete_local_data,
    TR_KEY_desired_available_camel,
    TR_KEY_desired_available,
    TR_KEY_destination,
    TR_KEY_details_window_height,
    TR_KEY_details_window_width,
    TR_KEY_dht_enabled,
    TR_KEY_dnd,
    TR_KEY_done_date_kebab,
    TR_KEY_done_date_camel,
    TR_KEY_done_date,
    TR_KEY_download_dir_kebab,
    TR_KEY_download_dir_free_space,
    TR_KEY_download_queue_enabled,
    TR_KEY_download_queue_size,
    TR_KEY_downloadCount,
    TR_KEY_download_dir_camel,
    TR_KEY_download_limit_camel,
    TR_KEY_download_limited_camel,
    TR_KEY_downloadSpeed,
    TR_KEY_download_dir,
    TR_KEY_download_limit,
    TR_KEY_download_limited,
    TR_KEY_downloaded,
    TR_KEY_downloaded_bytes,
    TR_KEY_downloadedBytes,
    TR_KEY_downloaded_ever_camel,
    TR_KEY_downloaded_ever,
    TR_KEY_downloaders,
    TR_KEY_downloading_time_seconds,
    TR_KEY_dropped,
    TR_KEY_dropped6,
    TR_KEY_e,
    TR_KEY_edit_date_camel,
    TR_KEY_edit_date,
    TR_KEY_encoding,
    TR_KEY_encryption,
    TR_KEY_endPiece,
    TR_KEY_error,
    TR_KEY_error_string_camel,
    TR_KEY_error_string,
    TR_KEY_eta,
    TR_KEY_eta_idle_camel,
    TR_KEY_eta_idle,
    TR_KEY_fields,
    TR_KEY_file_count,
    TR_KEY_fileStats,
    TR_KEY_filename,
    TR_KEY_files,
    TR_KEY_files_added,
    TR_KEY_files_unwanted_kebab,
    TR_KEY_files_wanted_kebab,
    TR_KEY_filesAdded,
    TR_KEY_files_unwanted,
    TR_KEY_files_wanted,
    TR_KEY_filter_mode,
    TR_KEY_filter_text,
    TR_KEY_filter_trackers,
    TR_KEY_flagStr,
    TR_KEY_flags,
    TR_KEY_format,
    TR_KEY_fromCache,
    TR_KEY_fromDht,
    TR_KEY_fromIncoming,
    TR_KEY_fromLpd,
    TR_KEY_fromLtep,
    TR_KEY_fromPex,
    TR_KEY_fromTracker,
    TR_KEY_group,
    TR_KEY_hasAnnounced,
    TR_KEY_hasScraped,
    TR_KEY_hashString,
    TR_KEY_have,
    TR_KEY_haveUnchecked,
    TR_KEY_haveValid,
    TR_KEY_honors_session_limits_camel,
    TR_KEY_honors_session_limits,
    TR_KEY_host,
    TR_KEY_id,
    TR_KEY_id_timestamp,
    TR_KEY_idle_limit,
    TR_KEY_idle_mode,
    TR_KEY_idle_seeding_limit,
    TR_KEY_idle_seeding_limit_enabled,
    TR_KEY_ids,
    TR_KEY_incomplete,
    TR_KEY_incomplete_dir,
    TR_KEY_incomplete_dir_enabled,
    TR_KEY_info,
    TR_KEY_inhibit_desktop_hibernation,
    TR_KEY_ipProtocol,
    TR_KEY_ipv4,
    TR_KEY_ipv6,
    TR_KEY_isBackup,
    TR_KEY_isDownloadingFrom,
    TR_KEY_isEncrypted,
    TR_KEY_isFinished,
    TR_KEY_isIncoming,
    TR_KEY_isPrivate,
    TR_KEY_isStalled,
    TR_KEY_isUTP,
    TR_KEY_isUploadingTo,
    TR_KEY_is_queued,
    TR_KEY_labels,
    TR_KEY_lastAnnouncePeerCount,
    TR_KEY_lastAnnounceResult,
    TR_KEY_lastAnnounceStartTime,
    TR_KEY_lastAnnounceSucceeded,
    TR_KEY_lastAnnounceTime,
    TR_KEY_lastAnnounceTimedOut,
    TR_KEY_lastScrapeResult,
    TR_KEY_lastScrapeStartTime,
    TR_KEY_lastScrapeSucceeded,
    TR_KEY_lastScrapeTime,
    TR_KEY_lastScrapeTimedOut,
    TR_KEY_leecherCount,
    TR_KEY_leftUntilDone,
    TR_KEY_length,
    TR_KEY_location,
    TR_KEY_lpd_enabled,
    TR_KEY_m,
    TR_KEY_magnetLink,
    TR_KEY_main_window_height,
    TR_KEY_main_window_is_maximized,
    TR_KEY_main_window_layout_order,
    TR_KEY_main_window_width,
    TR_KEY_main_window_x,
    TR_KEY_main_window_y,
    TR_KEY_manualAnnounceTime,
    TR_KEY_max_peers,
    TR_KEY_maxConnectedPeers,
    TR_KEY_memory_bytes,
    TR_KEY_memory_units,
    TR_KEY_message_level,
    TR_KEY_metadataPercentComplete,
    TR_KEY_metadata_size,
    TR_KEY_metainfo,
    TR_KEY_method,
    TR_KEY_min_request_interval,
    TR_KEY_move,
    TR_KEY_msg_type,
    TR_KEY_mtimes,
    TR_KEY_name,
    TR_KEY_name_utf_8,
    TR_KEY_nextAnnounceTime,
    TR_KEY_nextScrapeTime,
    TR_KEY_nodes,
    TR_KEY_nodes6,
    TR_KEY_open_dialog_dir,
    TR_KEY_p,
    TR_KEY_path,
    TR_KEY_path_utf_8,
    TR_KEY_paused,
    TR_KEY_pausedTorrentCount,
    TR_KEY_peer_congestion_algorithm,
    TR_KEY_peer_limit_kebab,
    TR_KEY_peer_limit_global,
    TR_KEY_peer_limit_per_torrent,
    TR_KEY_peer_port,
    TR_KEY_peer_port_random_high,
    TR_KEY_peer_port_random_low,
    TR_KEY_peer_port_random_on_start,
    TR_KEY_peer_socket_tos,
    TR_KEY_peerIsChoked,
    TR_KEY_peerIsInterested,
    TR_KEY_peer_limit,
    TR_KEY_peers,
    TR_KEY_peers2,
    TR_KEY_peers2_6,
    TR_KEY_peersConnected,
    TR_KEY_peersFrom,
    TR_KEY_peersGettingFromUs,
    TR_KEY_peersSendingToUs,
    TR_KEY_percentComplete,
    TR_KEY_percentDone,
    TR_KEY_pex_enabled,
    TR_KEY_pidfile,
    TR_KEY_piece,
    TR_KEY_piece_length,
    TR_KEY_pieceCount,
    TR_KEY_pieceSize,
    TR_KEY_pieces,
    TR_KEY_play_download_complete_sound,
    TR_KEY_port,
    TR_KEY_port_forwarding_enabled,
    TR_KEY_port_is_open,
    TR_KEY_preallocation,
    TR_KEY_preferred_transport,
    TR_KEY_primary_mime_type,
    TR_KEY_priorities,
    TR_KEY_priority,
    TR_KEY_priority_high_kebab,
    TR_KEY_priority_low_kebab,
    TR_KEY_priority_normal_kebab,
    TR_KEY_priority_high,
    TR_KEY_priority_low,
    TR_KEY_priority_normal,
    TR_KEY_private,
    TR_KEY_progress,
    TR_KEY_prompt_before_exit,
    TR_KEY_queue_move_bottom,
    TR_KEY_queue_move_down,
    TR_KEY_queue_move_top,
    TR_KEY_queue_move_up,
    TR_KEY_queue_stalled_enabled,
    TR_KEY_queue_stalled_minutes,
    TR_KEY_queue_position_camel,
    TR_KEY_queue_position,
    TR_KEY_rateDownload,
    TR_KEY_rateToClient,
    TR_KEY_rateToPeer,
    TR_KEY_rateUpload,
    TR_KEY_ratio_limit,
    TR_KEY_ratio_limit_enabled,
    TR_KEY_ratio_mode,
    TR_KEY_read_clipboard,
    TR_KEY_recent_download_dir_1,
    TR_KEY_recent_download_dir_2,
    TR_KEY_recent_download_dir_3,
    TR_KEY_recent_download_dir_4,
    TR_KEY_recent_relocate_dir_1,
    TR_KEY_recent_relocate_dir_2,
    TR_KEY_recent_relocate_dir_3,
    TR_KEY_recent_relocate_dir_4,
    TR_KEY_recheckProgress,
    TR_KEY_remote_session_enabled,
    TR_KEY_remote_session_host,
    TR_KEY_remote_session_https,
    TR_KEY_remote_session_password,
    TR_KEY_remote_session_port,
    TR_KEY_remote_session_requres_authentication,
    TR_KEY_remote_session_username,
    TR_KEY_removed,
    TR_KEY_rename_partial_files,
    TR_KEY_reqq,
    TR_KEY_result,
    TR_KEY_rpc_authentication_required,
    TR_KEY_rpc_bind_address,
    TR_KEY_rpc_enabled,
    TR_KEY_rpc_host_whitelist,
    TR_KEY_rpc_host_whitelist_enabled,
    TR_KEY_rpc_password,
    TR_KEY_rpc_port,
    TR_KEY_rpc_socket_mode,
    TR_KEY_rpc_url,
    TR_KEY_rpc_username,
    TR_KEY_rpc_version,
    TR_KEY_rpc_version_minimum,
    TR_KEY_rpc_version_semver,
    TR_KEY_rpc_whitelist,
    TR_KEY_rpc_whitelist_enabled,
    TR_KEY_scrape,
    TR_KEY_scrape_paused_torrents_enabled,
    TR_KEY_scrapeState,
    TR_KEY_script_torrent_added_enabled,
    TR_KEY_script_torrent_added_filename,
    TR_KEY_script_torrent_done_enabled,
    TR_KEY_script_torrent_done_filename,
    TR_KEY_script_torrent_done_seeding_enabled,
    TR_KEY_script_torrent_done_seeding_filename,
    TR_KEY_seconds_active,
    TR_KEY_secondsActive,
    TR_KEY_secondsDownloading,
    TR_KEY_secondsSeeding,
    TR_KEY_seed_queue_enabled,
    TR_KEY_seed_queue_size,
    TR_KEY_seed_idle_limit_camel,
    TR_KEY_seed_idle_mode_camel,
    TR_KEY_seed_ratio_limit_camel,
    TR_KEY_seedRatioLimited,
    TR_KEY_seed_ratio_mode_camel,
    TR_KEY_seed_idle_limit,
    TR_KEY_seed_idle_mode,
    TR_KEY_seed_ratio_limit,
    TR_KEY_seed_ratio_mode,
    TR_KEY_seederCount,
    TR_KEY_seeding_time_seconds,
    TR_KEY_sequential_download,
    TR_KEY_session_count,
    TR_KEY_session_id,
    TR_KEY_sessionCount,
    TR_KEY_show_backup_trackers,
    TR_KEY_show_extra_peer_details,
    TR_KEY_show_filterbar,
    TR_KEY_show_notification_area_icon,
    TR_KEY_show_options_window,
    TR_KEY_show_statusbar,
    TR_KEY_show_toolbar,
    TR_KEY_show_tracker_scrapes,
    TR_KEY_sitename,
    TR_KEY_size_bytes,
    TR_KEY_size_units,
    TR_KEY_sizeWhenDone,
    TR_KEY_sleep_per_seconds_during_verify,
    TR_KEY_socket_address,
    TR_KEY_sort_mode,
    TR_KEY_sort_reversed,
    TR_KEY_source,
    TR_KEY_speed,
    TR_KEY_speed_Bps,
    TR_KEY_speed_bytes,
    TR_KEY_speed_limit_down,
    TR_KEY_speed_limit_down_enabled,
    TR_KEY_speed_limit_up,
    TR_KEY_speed_limit_up_enabled,
    TR_KEY_speed_units,
    TR_KEY_start_added_torrents,
    TR_KEY_start_minimized,
    TR_KEY_startDate,
    TR_KEY_start_paused,
    TR_KEY_status,
    TR_KEY_statusbar_stats,
    TR_KEY_tag,
    TR_KEY_tcp_enabled,
    TR_KEY_tier,
    TR_KEY_time_checked,
    TR_KEY_torrent_added,
    TR_KEY_torrent_added_notification_command,
    TR_KEY_torrent_added_notification_enabled,
    TR_KEY_torrent_added_verify_mode,
    TR_KEY_torrent_complete_notification_command,
    TR_KEY_torrent_complete_notification_enabled,
    TR_KEY_torrent_complete_sound_command,
    TR_KEY_torrent_complete_sound_enabled,
    TR_KEY_torrent_duplicate,
    TR_KEY_torrent_get_kebab,
    TR_KEY_torrent_set_kebab,
    TR_KEY_torrent_set_location_kebab,
    TR_KEY_torrentCount,
    TR_KEY_torrentFile,
    TR_KEY_torrent_get,
    TR_KEY_torrent_set,
    TR_KEY_torrent_set_location,
    TR_KEY_torrents,
    TR_KEY_totalSize,
    TR_KEY_total_size,
    TR_KEY_tracker_add_camel,
    TR_KEY_tracker_list_camel,
    TR_KEY_tracker_remove_camel,
    TR_KEY_tracker_replace_camel,
    TR_KEY_trackerStats,
    TR_KEY_tracker_add,
    TR_KEY_tracker_list,
    TR_KEY_tracker_remove,
    TR_KEY_tracker_replace,
    TR_KEY_trackers,
    TR_KEY_trash_can_enabled,
    TR_KEY_trash_original_torrent_files,
    TR_KEY_umask,
    TR_KEY_units,
    TR_KEY_upload_slots_per_torrent,
    TR_KEY_upload_limit_camel,
    TR_KEY_upload_limited_camel,
    TR_KEY_uploadRatio,
    TR_KEY_uploadSpeed,
    TR_KEY_upload_limit,
    TR_KEY_upload_limited,
    TR_KEY_upload_only,
    TR_KEY_uploaded,
    TR_KEY_uploaded_bytes,
    TR_KEY_uploadedBytes,
    TR_KEY_uploadedEver,
    TR_KEY_url_list,
    TR_KEY_use_global_speed_limit,
    TR_KEY_use_speed_limit,
    TR_KEY_user_has_given_informed_consent,
    TR_KEY_ut_comment,
    TR_KEY_ut_holepunch,
    TR_KEY_ut_metadata,
    TR_KEY_ut_pex,
    TR_KEY_ut_recommend,
    TR_KEY_utp_enabled,
    TR_KEY_v,
    TR_KEY_version,
    TR_KEY_wanted,
    TR_KEY_watch_dir,
    TR_KEY_watch_dir_enabled,
    TR_KEY_watch_dir_force_generic,
    TR_KEY_webseeds,
    TR_KEY_webseedsSendingToUs,
    TR_KEY_yourip,
    TR_N_KEYS
};

/**
 * Find the quark that matches the specified string
 *
 * @return true if the specified string exists as a quark
 */
[[nodiscard]] std::optional<tr_quark> tr_quark_lookup(std::string_view key);

/**
 * Get the string view that corresponds to the specified quark.
 *
 * Note: this view is guaranteed to be zero-terminated at view[std::size(view)]
 */
[[nodiscard]] std::string_view tr_quark_get_string_view(tr_quark quark);

/**
 * Create a new quark for the specified string. If a quark already
 * exists for that string, it is returned so that no duplicates are
 * created.
 */
[[nodiscard]] tr_quark tr_quark_new(std::string_view str);

/**
 * Get the replacement quark from old deprecated quarks.
 *
 * Note: Temporary shim just for the transition period to snake_case.
 */
[[nodiscard]] tr_quark tr_quark_convert(tr_quark quark);
