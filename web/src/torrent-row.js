/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { Formatter } from './formatter.js';
import { Torrent } from './torrent.js';
import { setTextContent } from './utils.js';

const TorrentRendererHelper = {
  createIcon: (torrent) => {
    const icon = document.createElement('div');
    icon.classList.add('icon');
    icon.dataset.iconMimeType = torrent
      .getPrimaryMimeType()
      .split('/', 1)
      .pop();
    icon.dataset.iconMultifile = torrent.getFileCount() > 1 ? 'true' : 'false';
    return icon;
  },

  formatDL: (t) => {
    return `▼ ${Formatter.speedBps(t.getDownloadSpeed())}`;
  },
  formatETA: (t) => {
    const eta = t.getETA();
    if (eta < 0 || eta >= 999 * 60 * 60) {
      return '';
    }
    return `ETA: ${Formatter.timeInterval(eta, 1)}`;
  },
  formatLabels: (t, label) => {
    const labels = t.getLabels();
    label.innerHTML = '';
    for (const label_ of labels) {
      const s = document.createElement('span');
      s.classList.add('torrent-label');
      s.textContent = label_;
      label.append(s);
    }
  },
  formatUL: (t) => {
    return `▲ ${Formatter.speedBps(t.getUploadSpeed())}`;
  },
  getProgressInfo: (controller, t) => {
    const status = t.getStatus();
    const classList = ['torrent-progress-bar'];
    let percent = null;

    if (status === Torrent._StatusStopped) {
      classList.push('paused');
    }

    if (t.needsMetaData()) {
      classList.push('magnet');
      percent = t.getMetadataPercentComplete() * 100;
    } else if (status === Torrent._StatusCheck) {
      classList.push('verify');
      percent = t.getRecheckProgress() * 100;
    } else if (t.getLeftUntilDone() > 0) {
      classList.push('leech');
      percent = t.getPercentDone() * 100;
    } else {
      classList.push('seed');
      const seed_ratio_limit = t.seedRatioLimit(controller);
      percent =
        seed_ratio_limit > 0
          ? (t.getUploadRatio() * 100) / seed_ratio_limit
          : 100;
    }
    if (t.isQueued()) {
      classList.push('queued');
    }

    return {
      classList,
      percent,
    };
  },

  renderProgressbar: (controller, t, progressbar) => {
    const info = TorrentRendererHelper.getProgressInfo(controller, t);
    const pct_str = `${Formatter.percentString(info.percent, 2)}%`;
    progressbar.className = info.classList.join(' ');
    progressbar.style.setProperty('--progress', pct_str);
    progressbar.dataset.progress = pct_str;
  },
};

///

export class TorrentRendererFull {
  static getPeerDetails(t) {
    const fmt = Formatter;

    const error = t.getErrorMessage();
    if (error) {
      return error;
    }

    if (t.isDownloading()) {
      const peer_count = t.getPeersConnected();
      const webseed_count = t.getWebseedsSendingToUs();

      if (webseed_count && peer_count) {
        // Downloading from 2 of 3 peer(s) and 2 webseed(s)
        return [
          'Downloading from',
          t.getPeersSendingToUs(),
          'of',
          fmt.countString('peer', 'peers', peer_count),
          'and',
          fmt.countString('web seed', 'web seeds', webseed_count),
          '–',
          TorrentRendererHelper.formatDL(t),
          TorrentRendererHelper.formatUL(t),
        ].join(' ');
      }
      if (webseed_count) {
        // Downloading from 2 webseed(s)
        return [
          'Downloading from',
          fmt.countString('web seed', 'web seeds', webseed_count),
          '–',
          TorrentRendererHelper.formatDL(t),
          TorrentRendererHelper.formatUL(t),
        ].join(' ');
      }

      // Downloading from 2 of 3 peer(s)
      return [
        'Downloading from',
        t.getPeersSendingToUs(),
        'of',
        fmt.countString('peer', 'peers', peer_count),
        '–',
        TorrentRendererHelper.formatDL(t),
        TorrentRendererHelper.formatUL(t),
      ].join(' ');
    }

    if (t.isSeeding()) {
      return [
        'Seeding to',
        t.getPeersGettingFromUs(),
        'of',
        fmt.countString('peer', 'peers', t.getPeersConnected()),
        '-',
        TorrentRendererHelper.formatUL(t),
      ].join(' ');
    }

    if (t.isChecking()) {
      return [
        'Verifying local data (',
        Formatter.percentString(100 * t.getRecheckProgress(), 1),
        '% tested)',
      ].join('');
    }

    return t.getStateString();
  }

  static getProgressDetails(controller, t) {
    if (t.needsMetaData()) {
      let MetaDataStatus = 'retrieving';
      if (t.isStopped()) {
        MetaDataStatus = 'needs';
      }
      const percent = 100 * t.getMetadataPercentComplete();
      return [
        `Magnetized transfer - ${MetaDataStatus} metadata (`,
        Formatter.percentString(percent, 1),
        '%)',
      ].join('');
    }

    const sizeWhenDone = t.getSizeWhenDone();
    const totalSize = t.getTotalSize();
    const is_done = t.isDone() || t.isSeeding();
    const c = [];

    if (is_done) {
      if (totalSize === sizeWhenDone) {
        // seed: '698.05 MiB'
        c.push(Formatter.size(totalSize));
      } else {
        // partial seed: '127.21 MiB of 698.05 MiB (18.2%)'
        c.push(
          Formatter.size(sizeWhenDone),
          ' of ',
          Formatter.size(t.getTotalSize()),
          ' (',
          t.getPercentDoneStr(),
          '%)',
        );
      }
      // append UL stats: ', uploaded 8.59 GiB (Ratio: 12.3)'
      c.push(
        ', uploaded ',
        Formatter.size(t.getUploadedEver()),
        ' (Ratio ',
        Formatter.ratioString(t.getUploadRatio()),
        ')',
      );
    } else {
      // not done yet
      c.push(
        Formatter.size(sizeWhenDone - t.getLeftUntilDone()),
        ' of ',
        Formatter.size(sizeWhenDone),
        ' (',
        t.getPercentDoneStr(),
        '%)',
      );
    }

    // maybe append eta
    if (!t.isStopped() && (!is_done || t.seedRatioLimit(controller) > 0)) {
      c.push(' - ');
      const eta = t.getETA();
      if (eta < 0 || eta >= 999 * 60 * 60 /* arbitrary */) {
        c.push('remaining time unknown');
      } else {
        c.push(Formatter.timeInterval(t.getETA(), 1), ' remaining');
      }
    }

    return c.join('');
  }

  // eslint-disable-next-line class-methods-use-this
  render(controller, t, root) {
    const is_stopped = t.isStopped();

    root.classList.toggle('paused', is_stopped);

    // name
    let e = root._name_container;
    setTextContent(e, t.getName());

    // labels
    TorrentRendererHelper.formatLabels(t, root._labels_container);

    // progress details
    e = root._progress_details_container;
    e.innerHTML = TorrentRendererFull.getProgressDetails(controller, t);

    // progressbar
    TorrentRendererHelper.renderProgressbar(controller, t, root._progressbar);
    root._progressbar.classList.add('full');

    // peer details
    const has_error = t.getError() !== Torrent._ErrNone;
    e = root._peer_details_container;
    e.classList.toggle('error', has_error);
    setTextContent(e, TorrentRendererFull.getPeerDetails(t));

    // pause/resume button
    e = root._toggle_running_button;
    e.alt = is_stopped ? 'Resume' : 'Pause';
    e.dataset.action = is_stopped ? 'resume' : 'pause';
  }

  // eslint-disable-next-line class-methods-use-this
  createRow(torrent) {
    const root = document.createElement('li');
    root.className = 'torrent';

    const icon = TorrentRendererHelper.createIcon(torrent);

    const name = document.createElement('div');
    name.className = 'torrent-name';
    const labels = document.createElement('div');
    labels.className = 'torrent-labels';
    const details = document.createElement('div');
    details.className = 'torrent-progress-details';
    const progress = document.createElement('div');
    progress.classList.add('torrent-progress');
    const progressbar = document.createElement('div');
    progressbar.classList.add('torrent-progress-bar', 'full');
    progress.append(progressbar);
    const peers = document.createElement('div');
    peers.className = 'torrent-peer-details';
    const button = document.createElement('a');
    button.className = 'torrent-pauseresume-button';
    progress.append(button);

    root.append(icon);
    root.append(name);
    root.append(labels);
    root.append(details);
    root.append(progress);
    root.append(peers);

    root._icon = icon;
    root._name_container = name;
    root._labels_container = labels;
    root._progress_details_container = details;
    root._progressbar = progressbar;
    root._peer_details_container = peers;
    root._toggle_running_button = button;

    return root;
  }
}

///

export class TorrentRendererCompact {
  static getPeerDetails(t) {
    const errorMessage = t.getErrorMessage();
    if (errorMessage) {
      return errorMessage;
    }
    if (t.isDownloading()) {
      const have_dn = t.getDownloadSpeed() > 0;
      const have_up = t.getUploadSpeed() > 0;

      if (!have_up && !have_dn) {
        return 'Idle';
      }

      const s = [`${TorrentRendererHelper.formatETA(t)} `];
      if (have_dn) {
        s.push(TorrentRendererHelper.formatDL(t));
      }
      if (have_up) {
        s.push(TorrentRendererHelper.formatUL(t));
      }
      return s.join(' ');
    }
    if (t.isSeeding()) {
      return `Ratio: ${Formatter.ratioString(
        t.getUploadRatio(),
      )}, ${TorrentRendererHelper.formatUL(t)}`;
    }
    return t.getStateString();
  }

  // eslint-disable-next-line class-methods-use-this
  render(controller, t, root) {
    root.classList.toggle('paused', t.isStopped());

    // name
    let e = root._name_container;
    setTextContent(e, t.getName());

    // labels
    TorrentRendererHelper.formatLabels(t, root._labels_container);

    // progressbar
    TorrentRendererHelper.renderProgressbar(controller, t, root._progressbar);
    root._progressbar.classList.add('compact');

    // peer details
    const has_error = t.getError() !== Torrent._ErrNone;
    e = root._details_container;
    e.classList.toggle('error', has_error);
    setTextContent(e, TorrentRendererCompact.getPeerDetails(t));
  }

  // eslint-disable-next-line class-methods-use-this
  createRow(torrent) {
    const progressbar = document.createElement('div');
    progressbar.classList.add('torrent-progress-bar', 'compact');

    const icon = TorrentRendererHelper.createIcon(torrent);

    const details = document.createElement('div');
    details.className = 'torrent-peer-details compact';

    const labels = document.createElement('div');
    labels.className = 'torrent-labels compact';

    const name = document.createElement('div');
    name.className = 'torrent-name compact';

    const root = document.createElement('li');
    root.append(progressbar);
    root.append(details);
    root.append(labels);
    root.append(name);
    root.append(icon);
    root.className = 'torrent compact';
    root._progressbar = progressbar;
    root._details_container = details;
    root._labels_container = labels;
    root._name_container = name;
    return root;
  }
}

///

export class TorrentRow {
  constructor(view, controller, torrent) {
    this._view = view;
    this._torrent = torrent;
    this._element = view.createRow(torrent);

    const update = () => this.render(controller);
    this._torrent.addEventListener('dataChanged', update);
    update();
  }

  getElement() {
    return this._element;
  }

  render(controller) {
    const tor = this.getTorrent();
    if (tor) {
      this._view.render(controller, tor, this.getElement());
    }
  }

  isSelected() {
    return this.getElement().classList.contains('selected');
  }

  getTorrent() {
    return this._torrent;
  }

  getTorrentId() {
    return this.getTorrent().getId();
  }
}
