/* @license This file Copyright © Charles Kerr, Dave Perrett, Malcolm Jarvis and Bruno Bierbaumer
   It may be used under GPLv2 (SPDX: GPL-2.0-only).
   License text can be found in the licenses/ folder. */

import { AlertDialog } from './alert-dialog.js';

export const RPC = {
  _DaemonVersion: 'version',
  _DownSpeedLimit: 'speed-limit-down',
  _DownSpeedLimited: 'speed-limit-down-enabled',
  _JsonRpcVersion: '2.0',
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
        switch (response.status) {
          case 409: {
            const error = new Error(Remote._SessionHeader);
            error.header = response.headers.get(Remote._SessionHeader);
            throw error;
          }
          case 204:
            return null;
          default:
            return response.json();
        }
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
      id: 'webui',
      jsonrpc: RPC._JsonRpcVersion,
      method: 'session-get',
    };
    this.sendRequest(o, callback, context);
  }

  checkPort(ipProtocol, callback, context) {
    const o = {
      id: 'webui',
      jsonrpc: RPC._JsonRpcVersion,
      method: 'port-test',
      params: {
        ipProtocol,
      },
    };
    this.sendRequest(o, callback, context);
  }

  renameTorrent(torrentIds, oldpath, newname, callback, context) {
    const o = {
      id: 'webui',
      jsonrpc: RPC._JsonRpcVersion,
      method: 'torrent-rename-path',
      params: {
        ids: torrentIds,
        name: newname,
        path: oldpath,
      },
    };
    this.sendRequest(o, callback, context);
  }

  setLabels(torrentIds, labels, callback) {
    const params = {
      ids: torrentIds,
      labels,
    };
    this.sendRequest(
      {
        id: 'webui',
        jsonrpc: RPC._JsonRpcVersion,
        method: 'torrent-set',
        params,
      },
      callback,
    );
  }

  loadDaemonStats(callback, context) {
    const o = {
      id: 'webui',
      jsonrpc: RPC._JsonRpcVersion,
      method: 'session-stats',
    };
    this.sendRequest(o, callback, context);
  }

  updateTorrents(torrentIds, fields, callback, context) {
    const o = {
      id: 'webui',
      jsonrpc: RPC._JsonRpcVersion,
      method: 'torrent-get',
      params: {
        fields,
        format: 'table',
      },
    };
    if (torrentIds) {
      o.params.ids = torrentIds;
    }
    this.sendRequest(o, (response) => {
      const { torrents, removed } = response.result;
      callback.call(context, torrents, removed);
    });
  }

  getFreeSpace(dir, callback, context) {
    const o = {
      id: 'webui',
      jsonrpc: RPC._JsonRpcVersion,
      method: 'free-space',
      params: { path: dir },
    };
    this.sendRequest(o, (response) => {
      const { path, 'size-bytes': size_bytes } = response.result;
      callback.call(context, path, size_bytes);
    });
  }

  changeFileCommand(torrentId, fileIndices, command) {
    const params = {
      ids: [torrentId],
    };
    params[command] = fileIndices;
    this.sendRequest(
      {
        jsonrpc: RPC._JsonRpcVersion,
        method: 'torrent-set',
        params,
      },
      () => {
        this._controller.refreshTorrents([torrentId]);
      },
    );
  }

  sendTorrentSetRequests(method, torrent_ids, params, callback, context) {
    params ||= {};
    params.ids = torrent_ids;
    const o = {
      id: 'webui',
      jsonrpc: RPC._JsonRpcVersion,
      method,
      params,
    };
    this.sendRequest(o, callback, context);
  }

  sendTorrentActionRequests(method, torrent_ids, callback, context) {
    this.sendTorrentSetRequests(method, torrent_ids, null, callback, context);
  }

  startTorrents(torrent_ids, noqueue, callback, context) {
    const name = noqueue ? 'torrent-start-now' : 'torrent-start';
    this.sendTorrentActionRequests(name, torrent_ids, callback, context);
  }
  stopTorrents(torrent_ids, callback, context) {
    this.sendTorrentActionRequests(
      'torrent-stop',
      torrent_ids,
      callback,
      context,
    );
  }

  moveTorrents(torrent_ids, new_location, callback, context) {
    this.sendTorrentSetRequests(
      'torrent-set-location',
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
      jsonrpc: RPC._JsonRpcVersion,
      method: 'torrent-remove',
      params: {
        'delete-local-data': trash,
        ids: [],
      },
    };

    if (torrents) {
      for (let index = 0, length_ = torrents.length; index < length_; ++index) {
        o.params.ids.push(torrents[index].getId());
      }
    }
    this.sendRequest(o, () => {
      this._controller.refreshTorrents();
    });
  }
  verifyTorrents(torrent_ids, callback, context) {
    this.sendTorrentActionRequests(
      'torrent-verify',
      torrent_ids,
      callback,
      context,
    );
  }
  reannounceTorrents(torrent_ids, callback, context) {
    this.sendTorrentActionRequests(
      'torrent-reannounce',
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
      jsonrpc: RPC._JsonRpcVersion,
      method: 'torrent-add',
      params: {
        filename: url,
        paused: options.paused,
      },
    };
    this.sendRequest(o, () => {
      this._controller.refreshTorrents();
    });
  }
  savePrefs(params) {
    const o = {
      jsonrpc: RPC._JsonRpcVersion,
      method: 'session-set',
      params,
    };
    this.sendRequest(o, () => {
      this._controller.loadDaemonPrefs();
    });
  }
  updateBlocklist() {
    const o = {
      jsonrpc: RPC._JsonRpcVersion,
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
