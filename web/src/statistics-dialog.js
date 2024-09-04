/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { Formatter } from './formatter.js';
import {
  Utils,
  setTextContent,
  createDialogContainer,
  createInfoSection,
} from './utils.js';

export class StatisticsDialog extends EventTarget {
  constructor(remote) {
    super();

    this.remote = remote;

    const updateDaemon = () =>
      this.remote.loadDaemonStats((data) => this._update(data.arguments));
    const delay_msec = 5000;
    this.interval = setInterval(updateDaemon, delay_msec);
    updateDaemon();

    this.elements = StatisticsDialog._create();
    this.elements.dismiss.addEventListener('click', () => this._onDismiss());
    document.body.append(this.elements.root);
    this.elements.dismiss.focus();
  }

  close() {
    if (!this.closed) {
      clearInterval(this.interval);
      this.elements.root.remove();
      for (const key of Object.keys(this)) {
        delete this[key];
      }
      this.closed = true;
    }
  }

  _onDismiss() {
    this.close();
  }

  _update(stats) {
    console.log(stats);
    const fmt = Formatter;

    let s = stats.current_stats;
    let ratio = Utils.ratio(s.uploaded_bytes, s.downloaded_bytes);
    setTextContent(this.elements.session.up, fmt.size(s.uploaded_bytes));
    setTextContent(this.elements.session.down, fmt.size(s.downloaded_bytes));
    this.elements.session.ratio.innerHTML = fmt.ratioString(ratio);
    setTextContent(
      this.elements.session.time,
      fmt.timeInterval(s.secondsActive),
    );

    s = stats.cumulative_stats;
    ratio = Utils.ratio(s.uploaded_bytes, s.downloaded_bytes);
    setTextContent(this.elements.total.up, fmt.size(s.uploaded_bytes));
    setTextContent(this.elements.total.down, fmt.size(s.downloaded_bytes));
    this.elements.total.ratio.innerHTML = fmt.ratioString(ratio);
    setTextContent(this.elements.total.time, fmt.timeInterval(s.secondsActive));
  }

  static _create() {
    const elements = createDialogContainer('statistics-dialog');
    const { confirm, dismiss, heading, root, workarea } = elements;
    confirm.remove();
    dismiss.textContent = 'Close';
    delete elements.confirm;

    const heading_text = 'Statistics';
    root.setAttribute('aria-label', heading_text);
    heading.textContent = heading_text;

    const labels = ['Uploaded:', 'Downloaded:', 'Ratio:', 'Running time:'];
    let section = createInfoSection('Current session', labels);
    const [sup, sdown, sratio, stime] = section.children;
    const session = (elements.session = {});
    session.up = sup;
    session.down = sdown;
    session.ratio = sratio;
    session.time = stime;
    workarea.append(section.root);

    section = createInfoSection('Total', labels);
    const [tup, tdown, tratio, ttime] = section.children;
    const total = (elements.total = {});
    total.up = tup;
    total.down = tdown;
    total.ratio = tratio;
    total.time = ttime;
    workarea.append(section.root);

    return elements;
  }
}
