/* @license This file Copyright © Charles Kerr, Dave Perrett, Malcolm Jarvis and Bruno Bierbaumer
   It may be used under GPLv2 (SPDX: GPL-2.0-only).
   License text can be found in the licenses/ folder. */

import { AlertDialog } from './alert-dialog.js';

export const RPC = {
  _DaemonVersion: 'version',
  _DownSpeedLimit: 'speed-limit-down',
  _DownSpeedLimited: 'speed-limit-down-enabled',
  _QueueMoveBottom: 'queue-move-bottom',
  _QueueMoveDown: 'queue-move-down',
  _QueueMoveTop: 'queue-move-top',
  _QueueMoveUp: 'queue-move-up',
  _Root: '../rpc',
  _TurtleDownSpeedLimit: 'alt-speed-down',
  _TurtleState: 'alt-speed-enabled',
  _TurtleUpSpeedLimit: 'alt-speed-up',
  _UpSpeedLimit: 'speed-limit-up',
  _UpSpeedLimited: 'speed-limit-up-enabled',
};

export class Remote {
  // TODO: decouple from controller
  constructor(controller) {
    this._connection_alert = null;
    this._controller = controller;
    this._session_id = '';
  }

  sendRequest(data, callback, context) {
    const headers = new Headers();
    headers.append('cache-control', 'no-cache');
    headers.append('content-type', 'application/json');
    headers.append('pragma', 'no-cache');
    if (this._session_id) {
      headers.append(Remote._SessionHeader, this._session_id);
    }

    let response_argument = null;
    fetch(RPC._Root, {
      body: JSON.stringify(data),
      headers,
      method: 'POST',
    })
      .then((response) => {
        response_argument = response;
        if (response.status === 409) {
          const error = new Error(Remote._SessionHeader);
          error.header = response.headers.get(Remote._SessionHeader);
          throw error;
        }
        return response.json();
      })
      .then((payload) => {
        if (callback) {
          callback.call(context, payload, response_argument);
        }

        if (this._connection_alert) {
          this._connection_alert.close();
          this._connection_alert = null;
        }
      })
      .catch((error) => {
        if (error.message === Remote._SessionHeader) {
          // copy the session header and try again
          this._session_id = error.header;
          this.sendRequest(data, callback, context);
          return;
        }
        console.trace(error);
        this._controller.togglePeriodicSessionRefresh(false);

        this._connection_alert = new AlertDialog({
          heading: 'Connection failed',
          message:
            'Could not connect to the server. You may need to reload the page to reconnect.',
        });
        this._controller.setCurrentPopup(this._connection_alert);
      });
  }

  // TODO: return a Promise
  loadDaemonPrefs(callback, context) {
    const o = {
      method: 'session-get',
    };
    this.sendRequest(o, callback, context);
  }

  checkPort(ipProtocol, callback, context) {
    const o = {
      arguments: {
        ipProtocol,
      },
      method: 'port-test',
    };
    this.sendRequest(o, callback, context);
  }

  renameTorrent(torrentIds, oldpath, newname, callback, context) {
    const o = {
      arguments: {
        ids: torrentIds,
        name: newname,
        path: oldpath,
      },
      method: 'torrent_rename_path',
    };
    this.sendRequest(o, callback, context);
  }

  setLabels(torrentIds, labels, callback) {
    const args = {
      ids: torrentIds,
      labels,
    };
    this.sendRequest({ arguments: args, method: 'torrent_set' }, callback);
  }

  loadDaemonStats(callback, context) {
    const o = {
      method: 'session-stats',
    };
    this.sendRequest(o, callback, context);
  }

  updateTorrents(torrentIds, fields, callback, context) {
    const o = {
      arguments: {
        fields,
        format: 'table',
      },
      method: 'torrent_get',
    };
    if (torrentIds) {
      o.arguments.ids = torrentIds;
    }
    this.sendRequest(o, (response) => {
      const arguments_ = response['arguments'];
      callback.call(context, arguments_.torrents, arguments_.removed);
    });
  }

  getFreeSpace(dir, callback, context) {
    const o = {
      arguments: {
        path: dir,
      },
      method: 'free-space',
    };
    this.sendRequest(o, (response) => {
      const arguments_ = response['arguments'];
      callback.call(context, arguments_.path, arguments_['size-bytes']);
    });
  }

  changeFileCommand(torrentId, fileIndices, command) {
    const arguments_ = {
      ids: [torrentId],
    };
    arguments_[command] = fileIndices;
    this.sendRequest(
      {
        arguments: arguments_,
        method: 'torrent_set',
      },
      () => {
        this._controller.refreshTorrents([torrentId]);
      },
    );
  }

  sendTorrentSetRequests(method, torrent_ids, arguments_, callback, context) {
    if (!arguments_) {
      arguments_ = {};
    }
    arguments_['ids'] = torrent_ids;
    const o = {
      arguments: arguments_,
      method,
    };
    this.sendRequest(o, callback, context);
  }

  sendTorrentActionRequests(method, torrent_ids, callback, context) {
    this.sendTorrentSetRequests(method, torrent_ids, null, callback, context);
  }

  startTorrents(torrent_ids, noqueue, callback, context) {
    const name = noqueue ? 'torrent_start_now' : 'torrent_start';
    this.sendTorrentActionRequests(name, torrent_ids, callback, context);
  }
  stopTorrents(torrent_ids, callback, context) {
    this.sendTorrentActionRequests(
      'torrent_stop',
      torrent_ids,
      callback,
      context,
    );
  }

  moveTorrents(torrent_ids, new_location, callback, context) {
    this.sendTorrentSetRequests(
      'torrent_set_location',
      torrent_ids,
      {
        location: new_location,
        move: true,
      },
      callback,
      context,
    );
  }

  removeTorrents(torrents, trash) {
    const o = {
      arguments: {
        delete_local_data: trash,
        ids: [],
      },
      method: 'torrent_remove',
    };

    if (torrents) {
      for (let index = 0, length_ = torrents.length; index < length_; ++index) {
        o.arguments.ids.push(torrents[index].getId());
      }
    }
    this.sendRequest(o, () => {
      this._controller.refreshTorrents();
    });
  }
  verifyTorrents(torrent_ids, callback, context) {
    this.sendTorrentActionRequests(
      'torrent_verify',
      torrent_ids,
      callback,
      context,
    );
  }
  reannounceTorrents(torrent_ids, callback, context) {
    this.sendTorrentActionRequests(
      'torrent_reannounce',
      torrent_ids,
      callback,
      context,
    );
  }
  addTorrentByUrl(url, options) {
    if (/^[\da-f]{40}$/i.test(url)) {
      url = `magnet:?xt=urn:btih:${url}`;
    }
    const o = {
      arguments: {
        filename: url,
        paused: options.paused,
      },
      method: 'torrent_add',
    };
    this.sendRequest(o, () => {
      this._controller.refreshTorrents();
    });
  }
  savePrefs(arguments_) {
    const o = {
      arguments: arguments_,
      method: 'session-set',
    };
    this.sendRequest(o, () => {
      this._controller.loadDaemonPrefs();
    });
  }
  updateBlocklist() {
    const o = {
      method: 'blocklist-update',
    };
    this.sendRequest(o, () => {
      this._controller.loadDaemonPrefs();
    });
  }

  // Added queue calls
  moveTorrentsToTop(torrent_ids, callback, context) {
    this.sendTorrentActionRequests(
      RPC._QueueMoveTop,
      torrent_ids,
      callback,
      context,
    );
  }
  moveTorrentsToBottom(torrent_ids, callback, context) {
    this.sendTorrentActionRequests(
      RPC._QueueMoveBottom,
      torrent_ids,
      callback,
      context,
    );
  }
  moveTorrentsUp(torrent_ids, callback, context) {
    this.sendTorrentActionRequests(
      RPC._QueueMoveUp,
      torrent_ids,
      callback,
      context,
    );
  }
  moveTorrentsDown(torrent_ids, callback, context) {
    this.sendTorrentActionRequests(
      RPC._QueueMoveDown,
      torrent_ids,
      callback,
      context,
    );
  }
}

Remote._SessionHeader = 'X-Transmission-Session-Id';
