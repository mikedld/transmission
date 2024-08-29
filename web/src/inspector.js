/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { FileRow } from './file-row.js';
import { Formatter } from './formatter.js';
import { Torrent } from './torrent.js';
import { Utils, createTextualTabsContainer, setTextContent } from './utils.js';

const peer_column_classes = [
  'encryption',
  'speed-up',
  'speed-down',
  'percent-done',
  'status',
  'peer-address',
  'peer-app-name',
];

export class Inspector extends EventTarget {
  constructor(controller) {
    super();

    this.closed = false;
    this.controller = controller;
    this.elements = this._create();
    this.current_page = this.elements.info.root;
    this.interval = setInterval(this._refreshTorrents.bind(this), 3000);
    this.name = 'inspector';
    this.selection_listener = (event_) => this._setTorrents(event_.selected);
    this.torrent_listener = () => this._updateCurrentPage();
    this.torrents = [];
    this.file_torrent = null;
    this.file_torrent_n = null;
    this.file_rows = null;
    this.elements.dismiss.addEventListener('click', () => this.close());
    Object.seal(this);

    controller.addEventListener(
      'torrent-selection-changed',
      this.selection_listener,
    );
    this._setTorrents(this.controller.getSelectedTorrents());

    document.body.append(this.elements.root);
  }

  close() {
    if (!this.closed) {
      clearInterval(this.interval);
      this._setTorrents([]);
      this.elements.root.remove();
      this.controller.removeEventListener(
        'torrent-selection-changed',
        this.selection_listener,
      );
      this.dispatchEvent(new Event('close'));
      for (const property of Object.keys(this)) {
        this[property] = null;
      }
      this.closed = true;
    }
  }

  static _createInfoPage() {
    const root = document.createElement('div');
    root.classList.add('inspector-info-page');
    const elements = { root };

    const append_section_title = (text) => {
      const label = document.createElement('div');
      label.textContent = text;
      label.classList.add('section-label');
      root.append(label);
    };

    const append_row = (text) => {
      const lhs = document.createElement('label');
      setTextContent(lhs, text);
      root.append(lhs);

      const rhs = document.createElement('span');
      root.append(rhs);
      return rhs;
    };

    append_section_title('Activity');
    let rows = [
      ['have', 'Have:'],
      ['availability', 'Availability:'],
      ['uploaded', 'Uploaded:'],
      ['downloaded', 'Downloaded:'],
      ['state', 'State:'],
      ['running_time', 'Running time:'],
      ['remaining_time', 'Remaining:'],
      ['last_activity', 'Last activity:'],
      ['error', 'Error:'],
    ];
    for (const [name, text] of rows) {
      elements[name] = append_row(text);
    }

    append_section_title('Details');
    rows = [
      ['name', 'Name:'],
      ['size', 'Size:'],
      ['location', 'Location:'],
      ['hash', 'Hash:'],
      ['privacy', 'Privacy:'],
      ['origin', 'Origin:'],
      ['date_added', 'Date added:'],
      ['magnet_link', 'Magnet:'],
      ['comment', 'Comment:'],
      ['labels', 'Labels:'],
    ];
    for (const [name, text] of rows) {
      elements[name] = append_row(text);
    }

    return elements;
  }

  static _createListPage(list_type, list_id) {
    const root = document.createElement('div');
    const list = document.createElement(list_type);
    list.id = list_id;
    root.append(list);
    return { list, root };
  }

  static _createTiersPage() {
    return Inspector._createListPage('div', 'inspector-tiers-list');
  }

  static _createFilesPage() {
    return Inspector._createListPage('ul', 'inspector-file-list');
  }

  static _createPeersPage() {
    const table = document.createElement('table');
    table.classList.add('peer-list');
    const thead = document.createElement('thead');
    const tr = document.createElement('tr');
    const names = ['', 'Up', 'Down', 'Done', 'Status', 'Address', 'Client'];
    for (const [index, name] of names.entries()) {
      const th = document.createElement('th');
      const classname = peer_column_classes[index];
      if (classname === 'encryption') {
        th.dataset.encrypted = true;
      }
      th.classList.add(classname);
      setTextContent(th, name);
      tr.append(th);
    }
    const tbody = document.createElement('tbody');
    thead.append(tr);
    table.append(thead);
    table.append(tbody);
    return {
      root: table,
      tbody,
    };
  }

  _create() {
    const pages = {
      files: Inspector._createFilesPage(),
      info: Inspector._createInfoPage(),
      peers: Inspector._createPeersPage(),
      tiers: Inspector._createTiersPage(),
    };

    const on_activated = (page) => {
      this.current_page = page;
      this._updateCurrentPage();
    };

    const elements = createTextualTabsContainer(
      'inspector',
      [
        ['inspector-tab-info', pages.info.root, 'Info'],
        ['inspector-tab-peers', pages.peers.root, 'Peers'],
        ['inspector-tab-tiers', pages.tiers.root, 'Tiers'],
        ['inspector-tab-files', pages.files.root, 'Files'],
      ],
      on_activated.bind(this),
    );

    return { ...elements, ...pages };
  }

  _setTorrents(torrents) {
    // update the inspector when a selected torrent's data changes.
    const key = 'dataChanged';
    const callback = this.torrent_listener;
    for (const t of this.torrents) {
      t.removeEventListener(key, callback);
    }
    this.torrents = [...torrents];
    for (const t of this.torrents) {
      t.addEventListener(key, callback);
    }

    this._refreshTorrents();
    this._updateCurrentPage();
  }

  static _needsExtraInfo(torrents) {
    return torrents.some((tor) => !tor.hasExtraInfo());
  }

  _refreshTorrents() {
    const { controller, torrents } = this;
    const ids = torrents.map((t) => t.getId());

    if (ids && ids.length > 0) {
      const fields = ['id', ...Torrent.Fields.StatsExtra];
      if (Inspector._needsExtraInfo(torrents)) {
        fields.push(...Torrent.Fields.InfoExtra);
      }

      controller.updateTorrents(ids, fields);
    }
  }

  _updateCurrentPage() {
    const { current_page, elements } = this;
    switch (current_page) {
      case elements.files.root:
        this._updateFiles();
        break;
      case elements.info.root:
        this._updateInfo();
        break;
      case elements.peers.root:
        this._updatePeers();
        break;
      case elements.tiers.root:
        this._updateTiers();
        break;
      default:
        console.warn('unexpected page');
        console.log(current_page);
    }
  }

  _updateInfo() {
    const none = 'None';
    const mixed = 'Mixed';
    const unknown = 'Unknown';
    const fmt = Formatter;
    const now = Date.now();
    const { elements: e, torrents } = this;
    const sizeWhenDone = torrents.reduce(
      (accumulator, t) => accumulator + t.getSizeWhenDone(),
      0,
    );

    // state
    let string = null;
    if (torrents.length === 0) {
      string = none;
    } else if (torrents.every((t) => t.isFinished())) {
      string = 'Finished';
    } else if (torrents.every((t) => t.isStopped())) {
      string = 'Paused';
    } else {
      const get = (t) => t.getStateString();
      const first = get(torrents[0]);
      string = torrents.every((t) => get(t) === first) ? first : mixed;
    }
    setTextContent(e.info.state, string);
    const stateString = string;

    // have
    if (torrents.length === 0) {
      string = none;
    } else {
      const verified = torrents.reduce(
        (accumulator, t) => accumulator + t.getHaveValid(),
        0,
      );
      const unverified = torrents.reduce(
        (accumulator, t) => accumulator + t.getHaveUnchecked(),
        0,
      );
      const leftUntilDone = torrents.reduce(
        (accumulator, t) => accumulator + t.getLeftUntilDone(),
        0,
      );

      const d =
        100 *
        (sizeWhenDone ? (sizeWhenDone - leftUntilDone) / sizeWhenDone : 1);
      string = fmt.percentString(d, 1);

      if (unverified) {
        string = `${fmt.size(verified)} of ${fmt.size(
          sizeWhenDone,
        )} (${string}%), ${fmt.size(unverified)} Unverified`;
      } else if (leftUntilDone) {
        string = `${fmt.size(verified)} of ${fmt.size(
          sizeWhenDone,
        )} (${string}%)`;
      } else {
        string = `${fmt.size(verified)} (100%)`;
      }
    }

    setTextContent(e.info.have, fmt.stringSanitizer(string));

    // availability
    if (torrents.length === 0) {
      string = none;
    } else if (sizeWhenDone === 0) {
      string = none;
    } else {
      const available = torrents.reduce(
        (accumulator, t) => t.getHave() + t.getDesiredAvailable(),
        0,
      );
      string = `${fmt.percentString((100 * available) / sizeWhenDone, 1)}%`;
    }
    setTextContent(e.info.availability, fmt.stringSanitizer(string));

    // downloaded
    if (torrents.length === 0) {
      string = none;
    } else {
      const d = torrents.reduce(
        (accumulator, t) => accumulator + t.getDownloadedEver(),
        0,
      );
      const f = torrents.reduce(
        (accumulator, t) => accumulator + t.getFailedEver(),
        0,
      );
      string = f
        ? `${fmt.size(d)} (+${fmt.size(f)} discarded after failed checksum)`
        : fmt.size(d);
    }

    setTextContent(e.info.downloaded, fmt.stringSanitizer(string));

    // uploaded
    if (torrents.length === 0) {
      string = none;
    } else {
      const uploaded = torrents.reduce(
        (accumulator, t) => accumulator + t.getUploadedEver(),
        0,
      );
      const denominator =
        torrents.reduce(
          (accumulator, t) => accumulator + t.getSizeWhenDone(),
          0,
        ) ||
        torrents.reduce((accumulator, t) => accumulator + t.getHaveValid(), 0);
      string = `${fmt.size(uploaded)} (Ratio: ${fmt.ratioString(
        Utils.ratio(uploaded, denominator),
      )})`;
    }
    setTextContent(e.info.uploaded, string);

    // running time
    if (torrents.length === 0) {
      string = none;
    } else if (torrents.every((t) => t.isStopped())) {
      string = stateString;
    } else {
      const get = (t) => t.getStartDate();
      const first = get(torrents[0]);
      string = torrents.every((t) => get(t) === first)
        ? fmt.timeInterval(now / 1000 - first)
        : mixed;
    }
    setTextContent(e.info.running_time, string);

    // remaining time
    if (torrents.length === 0) {
      string = none;
    } else {
      const get = (t) => t.getETA();
      const first = get(torrents[0]);
      if (!torrents.every((t) => get(t) === first)) {
        string = mixed;
      } else if (first < 0) {
        string = unknown;
      } else {
        string = fmt.timeInterval(first);
      }
    }
    setTextContent(e.info.remaining_time, string);

    // last active at
    if (torrents.length === 0) {
      string = none;
    } else {
      const latest = torrents.reduce(
        (accumulator, t) => Math.max(accumulator, t.getLastActivity()),
        -1,
      );
      const now_seconds = Math.floor(now / 1000);
      if (0 < latest && latest <= now_seconds) {
        const idle_secs = now_seconds - latest;
        string =
          idle_secs < 5 ? 'Active now' : `${fmt.timeInterval(idle_secs)} ago`;
      } else {
        string = none;
      }
    }
    setTextContent(e.info.last_activity, string);

    // error
    if (torrents.length === 0) {
      string = none;
    } else {
      const get = (t) => t.getErrorString();
      const first = get(torrents[0]);
      string = torrents.every((t) => get(t) === first) ? first : mixed;
    }
    setTextContent(e.info.error, string || none);

    // torrent name
    if (torrents.length === 1) {
      string = torrents[0].getName();
    } else {
      string = torrents.length > 0 ? mixed : none;
    }
    setTextContent(e.info.name, string);

    // size
    if (torrents.length === 0) {
      string = none;
    } else {
      const size = torrents.reduce(
        (accumulator, t) => accumulator + t.getTotalSize(),
        0,
      );
      if (size) {
        const get = (t) => t.getPieceSize();
        const pieceCount = torrents.reduce(
          (accumulator, t) => accumulator + t.getPieceCount(),
          0,
        );
        const pieceString = fmt.number(pieceCount);
        const pieceSize = get(torrents[0]);
        string = torrents.every((t) => get(t) === pieceSize)
          ? `${fmt.size(size)} (${pieceString} pieces @ ${fmt.mem(pieceSize)})`
          : `${fmt.size(size)} (${pieceString} pieces)`;
      } else {
        string = 'None';
      }
    }
    setTextContent(e.info.size, fmt.stringSanitizer(string));

    // hash
    if (torrents.length === 0) {
      string = none;
    } else {
      const get = (t) => t.getHashString();
      const first = get(torrents[0]);
      string = torrents.every((t) => get(t) === first) ? first : mixed;
    }
    setTextContent(e.info.hash, string);

    // privacy
    if (torrents.length === 0) {
      string = none;
    } else {
      const get = (t) => t.getPrivateFlag();
      const first = get(torrents[0]);
      if (!torrents.every((t) => get(t) === first)) {
        string = mixed;
      } else if (first) {
        string = 'Private to this tracker -- DHT and PEX disabled';
      } else {
        string = 'Public torrent';
      }
    }
    setTextContent(e.info.privacy, string);

    // comment
    if (torrents.length === 0) {
      string = none;
    } else {
      const get = (t) => t.getComment();
      const first = get(torrents[0]);
      string = torrents.every((t) => get(t) === first) ? first : mixed;
    }
    string = string || none;
    if (string.startsWith('https://') || string.startsWith('http://')) {
      string = encodeURI(string);
      e.info.comment.innerHTML = `<a href="${string}" target="_blank" >${string}</a>`;
    } else {
      setTextContent(e.info.comment, string);
    }

    // labels
    string = torrents.length === 0 ? none : torrents[0].getLabels().join(', ');
    setTextContent(e.info.labels, string);

    // origin
    if (torrents.length === 0) {
      string = none;
    } else {
      let get = (t) => t.getCreator();
      const creator = get(torrents[0]);
      const mixed_creator = !torrents.every((t) => get(t) === creator);

      get = (t) => t.getDateCreated();
      const date = get(torrents[0]);
      const mixed_date = !torrents.every((t) => get(t) === date);

      const empty_creator = !creator || creator.length === 0;
      const empty_date = !date;
      if (mixed_creator || mixed_date) {
        string = mixed;
      } else if (empty_creator && empty_date) {
        string = unknown;
      } else if (empty_date && !empty_creator) {
        string = `Created by ${creator}`;
      } else if (empty_creator && !empty_date) {
        string = `Created on ${new Date(date * 1000).toDateString()}`;
      } else {
        string = `Created by ${creator} on ${new Date(
          date * 1000,
        ).toDateString()}`;
      }
    }
    setTextContent(e.info.origin, string);

    // location
    if (torrents.length === 0) {
      string = none;
    } else {
      const get = (t) => t.getDownloadDir();
      const first = get(torrents[0]);
      string = torrents.every((t) => get(t) === first) ? first : mixed;
    }
    setTextContent(e.info.location, string);

    // date_added
    if (torrents.length === 0) {
      string = none;
    } else {
      const get = (t) => t.getDateAdded();
      const first = get(torrents[0]);
      string = torrents.every((t) => get(t) === first)
        ? new Date(first * 1000).toLocaleString(navigator.language, {
            day: '2-digit',
            hour: '2-digit',
            hour12: false,
            minute: '2-digit',
            month: 'short',
            second: '2-digit',
            timeZoneName: 'short',
            weekday: 'short',
            year: 'numeric',
          })
        : mixed;
    }
    setTextContent(e.info.date_added, string);

    // magnet_link
    if (torrents.length === 0) {
      setTextContent(e.info.magnet_link, none);
    } else if (torrents.length > 1) {
      setTextContent(e.info.magnet_link, mixed);
    } else {
      const link = torrents[0].getMagnetLink();
      e.info.magnet_link.innerHTML = `<a class="inspector-info-magnet" href="${link}"><button></button></a>`;
    }
  }

  ///  PEERS PAGE

  static _peerStatusTitle(flag_string) {
    const texts = Object.seal({
      '?': "We unchoked this peer, but they're not interested",
      D: 'Downloading from this peer',
      E: 'Encrypted Connection',
      H: 'Peer was discovered through Distributed Hash Table (DHT)',
      I: 'Peer is an incoming connection',
      K: "Peer has unchoked us, but we're not interested",
      O: 'Optimistic unchoke',
      T: 'Peer is connected via uTP',
      U: 'Uploading to peer',
      X: 'Peer was discovered through Peer Exchange (PEX)',
      d: "We would download from this peer if they'd let us",
      u: "We would upload to this peer if they'd ask",
    });

    return [...flag_string]
      .filter((ch) => texts[ch])
      .map((ch) => `${ch}: ${texts[ch]}`)
      .join('\n');
  }

  _updatePeers() {
    const fmt = Formatter;
    const { elements, torrents } = this;
    const { tbody } = elements.peers;

    const cell_setters = [
      (peer, td) => {
        td.dataset.encrypted = peer.is_encrypted;
      },
      (peer, td) =>
        setTextContent(
          td,
          peer.rate_to_peer ? fmt.speedBps(peer.rate_to_peer) : '',
        ),
      (peer, td) =>
        setTextContent(
          td,
          peer.rate_to_client ? fmt.speedBps(peer.rate_to_client) : '',
        ),
      (peer, td) => setTextContent(td, `${Math.floor(peer.progress * 100)}%`),
      (peer, td) => {
        setTextContent(td, peer.flag_str);
        td.setAttribute('title', Inspector._peerStatusTitle(peer.flag_str));
      },
      (peer, td) => {
        setTextContent(td, peer.address);
        td.setAttribute('title', peer.address);
      },
      (peer, td) => {
        setTextContent(td, peer.client_name);
        td.setAttribute('title', peer.client_name);
      },
    ];

    const rows = [];
    for (const tor of torrents) {
      // torrent name
      const tortr = document.createElement('tr');
      tortr.classList.add('torrent-row');
      const tortd = document.createElement('td');
      tortd.setAttribute('colspan', cell_setters.length);
      setTextContent(tortd, tor.getName());
      tortr.append(tortd);
      rows.push(tortr);

      // peers
      for (const peer of tor.getPeers()) {
        const tr = document.createElement('tr');
        tr.classList.add('peer-row');
        for (const [index, setter] of cell_setters.entries()) {
          const td = document.createElement('td');
          td.classList.add(peer_column_classes[index]);
          setter(peer, td);
          tr.append(td);
        }
        rows.push(tr);
      }

      // TODO: modify instead of rebuilding wholesale?
      while (tbody.firstChild) {
        tbody.firstChild.remove();
      }
      tbody.append(...rows);
    }
  }

  /// TRACKERS PAGE

  static getAnnounceState(tracker) {
    switch (tracker.announceState) {
      case Torrent._TrackerActive:
        return 'Announce in progress';
      case Torrent._TrackerWaiting: {
        const timeUntilAnnounce = Math.max(
          0,
          tracker.nextAnnounceTime - Date.now() / 1000,
        );
        return `Next announce in ${Formatter.timeInterval(timeUntilAnnounce)}`;
      }
      case Torrent._TrackerQueued:
        return 'Announce is queued';
      case Torrent._TrackerInactive:
        return tracker.isBackup
          ? 'Tracker will be used as a backup'
          : 'Announce not scheduled';
      default:
        return `unknown announce state: ${tracker.announceState}`;
    }
  }

  static lastAnnounceStatus(tracker) {
    let lastAnnounceLabel = 'Last Announce';
    let lastAnnounce = ['N/A'];

    if (tracker.hasAnnounced) {
      const lastAnnounceTime = Formatter.timestamp(tracker.lastAnnounceTime);
      if (tracker.lastAnnounceSucceeded) {
        lastAnnounce = [
          lastAnnounceTime,
          ' (got ',
          Formatter.countString('peer', 'peers', tracker.lastAnnouncePeerCount),
          ')',
        ];
      } else {
        lastAnnounceLabel = 'Announce error';
        lastAnnounce = [
          tracker.lastAnnounceResult ? `${tracker.lastAnnounceResult} - ` : '',
          lastAnnounceTime,
        ];
      }
    }
    return {
      label: lastAnnounceLabel,
      value: lastAnnounce.join(''),
    };
  }

  static lastScrapeStatus(tracker) {
    let lastScrapeLabel = 'Last Scrape';
    let lastScrape = 'N/A';

    if (tracker.hasScraped) {
      const lastScrapeTime = Formatter.timestamp(tracker.lastScrapeTime);
      if (tracker.lastScrapeSucceeded) {
        lastScrape = lastScrapeTime;
      } else {
        lastScrapeLabel = 'Scrape error';
        lastScrape =
          (tracker.lastScrapeResult ? `${tracker.lastScrapeResult} - ` : '') +
          lastScrapeTime;
      }
    }
    return {
      label: lastScrapeLabel,
      value: lastScrape,
    };
  }

  static _getOrigin(tracker) {
    try {
      // `new URL` fails on FF and Chrome when the scheme is 'udp',
      // so munge the URL to be something that won't break
      const udp_prefix = 'udp://';
      const is_udp = tracker.announce.startsWith(udp_prefix);
      if (is_udp) {
        const http_prefix = 'http://';
        const munged = tracker.announce.replace(udp_prefix, http_prefix);
        return new URL(munged).origin.replace(http_prefix, udp_prefix);
      }
      return new URL(tracker.announce).origin;
    } catch {
      return [tracker.sitename || tracker.host || tracker.announce];
    }
  }

  _updateTiers() {
    const na = 'N/A';
    const { list } = this.elements.tiers;
    const { torrents } = this;

    const rows = [];
    for (const tor of torrents) {
      // if >1 torrent to be shown, give a title
      if (torrents.length > 1) {
        const title = document.createElement('div');
        title.classList.add('tier-list-torrent');
        setTextContent(title, tor.getName());
        rows.push(title);
      }

      for (const tracker of tor.getTrackers()) {
        const announceState = Inspector.getAnnounceState(tracker);
        const lastAnnounceStatusHash = Inspector.lastAnnounceStatus(tracker);
        const lastScrapeStatusHash = Inspector.lastScrapeStatus(tracker);

        const tier_div = document.createElement('div');
        tier_div.classList.add('tier-list-row');

        let element = document.createElement('div');
        const site = Inspector._getOrigin(tracker);
        element.classList.add('tier-list-tracker');
        setTextContent(element, `${site} - tier ${tracker.tier + 1}`);
        element.setAttribute('title', tracker.announce);
        tier_div.append(element);

        element = document.createElement('div');
        element.classList.add('tier-announce');
        setTextContent(
          element,
          `${lastAnnounceStatusHash.label}: ${lastAnnounceStatusHash.value}`,
        );
        tier_div.append(element);

        element = document.createElement('div');
        element.classList.add('tier-seeders');
        setTextContent(
          element,
          `Seeders: ${tracker.seederCount > -1 ? tracker.seederCount : na}`,
        );
        tier_div.append(element);

        element = document.createElement('div');
        element.classList.add('tier-state');
        setTextContent(element, announceState);
        tier_div.append(element);

        element = document.createElement('div');
        element.classList.add('tier-leechers');
        setTextContent(
          element,
          `Leechers: ${tracker.leecherCount > -1 ? tracker.leecherCount : na}`,
        );
        tier_div.append(element);

        element = document.createElement('div');
        element.classList.add('tier-scrape');
        setTextContent(
          element,
          `${lastScrapeStatusHash.label}: ${lastScrapeStatusHash.value}`,
        );
        tier_div.append(element);

        element = document.createElement('div');
        element.classList.add('tier-downloads');
        setTextContent(
          element,
          `Downloads: ${
            tracker.downloadCount > -1 ? tracker.downloadCount : na
          }`,
        );
        tier_div.append(element);

        rows.push(tier_div);
      }
    }

    // TODO: modify instead of rebuilding wholesale?
    while (list.firstChild) {
      list.firstChild.remove();
    }
    list.append(...rows);
  }

  ///  FILES PAGE

  _changeFileCommand(fileIndices, command) {
    const { controller, file_torrent } = this;
    const torrentId = file_torrent.getId();
    controller.changeFileCommand(torrentId, fileIndices, command);
  }

  _onFileWantedToggled(event_) {
    const { indices, wanted } = event_;
    this._changeFileCommand(
      indices,
      wanted ? 'files_wanted' : 'files_unwanted',
    );
  }

  _onFilePriorityToggled(event_) {
    const { indices, priority } = event_;

    let command = null;
    switch (priority.toString()) {
      case '-1':
        command = 'priority_low';
        break;
      case '1':
        command = 'priority_high';
        break;
      default:
        command = 'priority_normal';
    }

    this._changeFileCommand(indices, command);
  }

  _clearFileList() {
    const { list } = this.elements.files;
    while (list.firstChild) {
      list.firstChild.remove();
    }

    this.file_torrent = null;
    this.file_torrent_n = null;
    this.file_rows = null;
  }

  static createFileTreeModel(tor) {
    const leaves = [];
    const tree = {
      children: {},
      file_indices: [],
    };

    for (const [index, file] of tor.getFiles().entries()) {
      const { name } = file;
      const tokens = name.split('/');
      let walk = tree;
      for (const [index_, token] of tokens.entries()) {
        let sub = walk.children[token];
        if (!sub) {
          walk.children[token] = sub = {
            children: {},
            depth: index_,
            file_indices: [],
            name: token,
            parent: walk,
          };
        }
        walk = sub;
      }
      walk.file_index = index;
      delete walk.children;
      leaves.push(walk);
    }

    for (const leaf of leaves) {
      const { file_index } = leaf;
      let walk = leaf;
      do {
        walk.file_indices.push(file_index);
        walk = walk.parent;
      } while (walk);
    }

    return tree;
  }

  addNodeToView(tor, parent, sub) {
    const row = new FileRow(tor, sub.depth, sub.name, sub.file_indices);
    row.addEventListener('wantedToggled', this._onFileWantedToggled.bind(this));
    row.addEventListener(
      'priorityToggled',
      this._onFilePriorityToggled.bind(this),
    );
    this.file_rows.push(row);
    parent.append(row.getElement());
  }

  addSubtreeToView(tor, parent, sub) {
    if (sub.parent) {
      this.addNodeToView(tor, parent, sub);
    }
    if (sub.children) {
      for (const value of Object.values(sub.children)) {
        this.addSubtreeToView(tor, parent, value);
      }
    }
  }

  _updateFiles() {
    const { list } = this.elements.files;
    const { file_rows, file_torrent, file_torrent_n, torrents } = this;

    // only show one torrent at a time
    if (torrents.length !== 1) {
      this._clearFileList();
      return;
    }

    const [tor] = torrents;
    const n = tor.getFiles().length;
    if (tor !== file_torrent || n !== file_torrent_n) {
      // rebuild the file list...
      this._clearFileList();
      this.file_torrent = tor;
      this.file_torrent_n = n;
      this.file_rows = [];
      const fragment = document.createDocumentFragment();
      const tree = Inspector.createFileTreeModel(tor);
      this.addSubtreeToView(tor, fragment, tree);
      list.append(fragment);
    } else {
      // ...refresh the already-existing file list
      for (const row of file_rows) {
        row.refresh();
      }
    }
  }
}
