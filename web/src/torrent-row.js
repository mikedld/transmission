/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { Formatter } from './formatter.js';
import { Torrent } from './torrent.js';
import { setTextContent } from './utils.js';

const TorrentRendererHelper = {
  createRow: (torrent) => {
    const root = document.createElement('li');
    root.className = 'torrent';

    const elements = [
      ['icon', 'icon'],
      ['name', 'torrent-name'],
      ['labels', 'torrent-labels'],
      ['progress_details', 'torrent-progress-details'],
      ['progressbar', 'torrent-progress-bar'],
      ['peer_details', 'torrent-peer-details'],
    ];

    for (const [name, className] of elements) {
      const e = document.createElement('div');
      e.className = className;
      root.append(e);
      root[name] = e;
    }

    TorrentRendererHelper.updateIcon(root.icon, torrent);

    return root;
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
  getProgressInfo: (controller, t) => {
    const status = t.getStatus();
    const classList = ['torrent-progress-bar'];
    let percent = 100;
    let ratio = null;

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
      if (status !== Torrent._StatusStopped) {
        const seed_ratio_limit = t.seedRatioLimit(controller);
        ratio =
          seed_ratio_limit > 0
            ? (t.getUploadRatio() * 100) / seed_ratio_limit
            : 100;
      }
    }
    if (t.isQueued()) {
      classList.push('queued');
    }

    return {
      classList,
      percent,
      ratio,
    };
  },
  renderProgressbar: (controller, t, progressbar) => {
    const info = TorrentRendererHelper.getProgressInfo(controller, t);
    const percent = Math.min(info.ratio || info.percent, 100);
    const pct_str = `${Formatter.percentString(percent, 2)}%`;
    progressbar.className = info.classList.join(' ');
    progressbar.style.setProperty('--progress', pct_str);
    progressbar.dataset.progress = info.ratio ? '100%' : pct_str;
  },
  symbol: { down: '▼', up: '▲' },
  updateIcon: (e, torrent) => {
    e.dataset.iconMimeType = torrent.getPrimaryMimeType().split('/', 1).pop();
    e.dataset.iconMultifile = torrent.getFileCount() > 1 ? 'true' : 'false';
  },
};

///

export class TorrentRendererFull {
  static renderPeerDetails(t, peer_details) {
    const fmt = Formatter;

    const has_error = t.getError() !== Torrent._ErrNone;
    peer_details.classList.toggle('error', has_error);

    const error = t.getErrorMessage();
    if (error) {
      setTextContent(peer_details, error);
    } else if (t.isDownloading()) {
      const peer_count = t.getPeersConnected();
      const webseed_count = t.getWebseedsSendingToUs();
      const s = ['Downloading from'];
      if (peer_count) {
        s.push(
          t.getPeersSendingToUs(),
          'of',
          fmt.countString('peer', 'peers', peer_count),
        );
        if (webseed_count) {
          s.push('and');
        }
      }
      if (webseed_count) {
        s.push(fmt.countString('web seed', 'web seeds', webseed_count));
      }
      s.push(
        '-',
        TorrentRendererHelper.symbol.down,
        fmt.speedBps(t.getDownloadSpeed()),
        TorrentRendererHelper.symbol.up,
        fmt.speedBps(t.getUploadSpeed()),
      );
      setTextContent(peer_details, s.join(' '));
    } else if (t.isSeeding()) {
      const str = [
        'Seeding to',
        t.getPeersGettingFromUs(),
        'of',
        fmt.countString('peer', 'peers', t.getPeersConnected()),
        '-',
        TorrentRendererHelper.symbol.up,
        fmt.speedBps(t.getUploadSpeed()),
      ].join(' ');
      setTextContent(peer_details, str);
    } else if (t.isChecking()) {
      const str = [
        'Verifying local data (',
        fmt.percentString(100 * t.getRecheckProgress(), 1),
        '% tested)',
      ].join('');
      setTextContent(peer_details, str);
    } else {
      setTextContent(peer_details, t.getStateString());
    }
  }

  static renderProgressDetails(controller, t, progress_details) {
    const fmt = Formatter;

    if (t.needsMetaData()) {
      let MetaDataStatus = 'retrieving';
      if (t.isStopped()) {
        MetaDataStatus = 'needs';
      }
      const percent = 100 * t.getMetadataPercentComplete();
      const str = [
        'Magnetized transfer - ',
        MetaDataStatus,
        ' metadata (',
        fmt.percentString(percent, 1),
        '%)',
      ].join('');
      setTextContent(progress_details, str);
      return;
    }

    const sizeWhenDone = t.getSizeWhenDone();
    const totalSize = t.getTotalSize();
    const is_done = t.isDone() || t.isSeeding();
    const s = [];

    if (is_done) {
      if (totalSize === sizeWhenDone) {
        // seed: '698.05 MiB'
        s.push(fmt.size(totalSize));
      } else {
        // partial seed: '127.21 MiB of 698.05 MiB (18.2%)'
        s.push(
          fmt.size(sizeWhenDone),
          ' of ',
          fmt.size(t.getTotalSize()),
          ' (',
          t.getPercentDoneStr(),
          '%)',
        );
      }
      // append UL stats: ', uploaded 8.59 GiB (Ratio: 12.3)'
      s.push(
        ', uploaded ',
        fmt.size(t.getUploadedEver()),
        ' (Ratio: ',
        fmt.ratioString(t.getUploadRatio()),
        ')',
      );
    } else {
      // not done yet
      s.push(
        fmt.size(sizeWhenDone - t.getLeftUntilDone()),
        ' of ',
        fmt.size(sizeWhenDone),
        ' (',
        t.getPercentDoneStr(),
        '%)',
      );
    }

    // maybe append eta
    if (!t.isStopped() && (!is_done || t.seedRatioLimit(controller) > 0)) {
      s.push(' - ');
      const eta = t.getETA();
      if (eta < 0 || eta >= 999 * 60 * 60 /* arbitrary */) {
        s.push('remaining time unknown');
      } else {
        s.push(fmt.timeInterval(t.getETA(), 1), ' remaining');
      }
    }

    setTextContent(progress_details, s.join(''));
  }

  // eslint-disable-next-line class-methods-use-this
  render(controller, torrent, root) {
    root.classList.toggle('paused', torrent.isStopped());
    const { labels, name, peer_details, progressbar, progress_details } = root;

    // name
    setTextContent(name, torrent.getName());

    // labels
    TorrentRendererHelper.formatLabels(torrent, labels);

    // progress details
    TorrentRendererFull.renderProgressDetails(
      controller,
      torrent,
      progress_details,
    );

    // progressbar
    TorrentRendererHelper.renderProgressbar(controller, torrent, progressbar);

    // peer details
    TorrentRendererFull.renderPeerDetails(torrent, peer_details);
  }
}

///

export class TorrentRendererCompact {
  static renderPeerDetails(t, peer_details) {
    const fmt = Formatter;

    const has_error = t.getError() !== Torrent._ErrNone;
    peer_details.classList.toggle('error', has_error);

    const error = t.getErrorMessage();
    if (error) {
      setTextContent(peer_details, error);
    } else if (t.isDownloading()) {
      const have_dn = t.getDownloadSpeed() > 0;
      const have_up = t.getUploadSpeed() > 0;

      if (!have_up && !have_dn) {
        setTextContent(peer_details, 'Idle');
      } else {
        const s = [TorrentRendererHelper.formatETA(t)];
        if (have_dn) {
          s.push(
            TorrentRendererHelper.symbol.down,
            fmt.speedBps(t.getDownloadSpeed()),
          );
        }
        if (have_up) {
          s.push(
            TorrentRendererHelper.symbol.up,
            fmt.speedBps(t.getUploadSpeed()),
          );
        }
        setTextContent(peer_details, s.join(' '));
      }
    } else if (t.isSeeding()) {
      const str = [
        'Ratio:',
        fmt.ratioString(t.getUploadRatio()),
        '-',
        TorrentRendererHelper.symbol.up,
        fmt.speedBps(t.getUploadSpeed()),
      ].join(' ');
      setTextContent(peer_details, str);
    } else {
      setTextContent(peer_details, t.getStateString());
    }
  }

  // eslint-disable-next-line class-methods-use-this
  render(controller, torrent, root) {
    root.classList.toggle('paused', torrent.isStopped());
    const { labels, name, peer_details, progressbar } = root;

    // name
    setTextContent(name, torrent.getName());

    // labels
    TorrentRendererHelper.formatLabels(torrent, labels);

    // peer details
    TorrentRendererCompact.renderPeerDetails(torrent, peer_details);

    // progressbar
    TorrentRendererHelper.renderProgressbar(controller, torrent, progressbar);
  }
}

///

export class TorrentRow {
  constructor(controller, torrent) {
    this._torrent = torrent;
    this._element = TorrentRendererHelper.createRow(torrent);

    const update = () => this.updateRow(controller);
    this._torrent.addEventListener('dataChanged', update);
    update();
  }

  getElement() {
    return this._element;
  }

  updateRow(controller) {
    const tor = this.getTorrent();
    if (tor) {
      controller.torrentRenderer.render(controller, tor, this.getElement());
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
