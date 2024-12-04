/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { createDialogContainer } from './utils.js';

export class LabelsDialog extends EventTarget {
  constructor(controller, remote) {
    super();

    this.controller = controller;
    this.remote = remote;
    this.elements = {};
    this.torrents = [];

    this.show();
  }

  show() {
    const torrents = this.controller.getSelectedTorrents();
    if (torrents.length === 0) {
      console.error('At least one selected torrent expected.');
      return;
    }
    const [first] = torrents;

    this.torrents = torrents;
    this.elements = LabelsDialog._create();
    this.elements.dismiss.addEventListener('click', () => this._onDismiss());
    this.elements.confirm.addEventListener('click', () => this._onConfirm());
    this.elements.entry.value = first.getLabels().join(', ');
    document.body.append(this.elements.root);

    this.elements.entry.focus();
  }

  close() {
    this.elements.root.remove();
    this.dispatchEvent(new Event('close'));

    delete this.elements;
    delete this.torrents;
  }

  _onDismiss() {
    this.close();
  }

  _onConfirm() {
    const { torrents } = this;
    const { remote } = this;
    const ids = torrents.map((t) => t.getId());
    const { elements } = this;
    const { entry } = elements;
    const { value } = entry;
    const labels = value
      .split(',')
      .map((l) => l.trim())
      .filter((l) => l.length > 0);

    remote.setLabels(ids, labels, (response) => {
      if (response.result === 'success') {
        for (const t of torrents) {
          t.refresh({ labels });
        }
      }
    });
    this.close();
  }

  static _create() {
    const elements = createDialogContainer('labels-dialog');
    elements.root.setAttribute('aria-label', 'Edit Labels');
    elements.heading.textContent = 'Edit Labels:';
    elements.confirm.textContent = 'Save';

    const label = document.createElement('label');
    label.setAttribute('for', 'torrent-labels');
    label.textContent = 'Labels:';
    elements.workarea.append(label);

    const entry = document.createElement('input');
    entry.setAttribute('type', 'text');
    entry.id = 'torrent-labels';
    elements.entry = entry;
    elements.workarea.append(entry);

    return elements;
  }
}
