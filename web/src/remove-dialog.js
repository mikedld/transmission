/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { createDialogContainer } from './utils.js';

export class RemoveDialog extends EventTarget {
  constructor(options) {
    super();

    // options: remote, torrents
    this.options = options;
    this.options.trash = false;
    this.elements = RemoveDialog._create(options);
    this.elements.dismiss.addEventListener('click', () => this._onDismiss());
    this.elements.confirm.addEventListener('click', () => this._onConfirm());
    document.body.append(this.elements.root);
    this.elements.dismiss.focus();
  }

  close() {
    if (!this.closed) {
      this.elements.root.remove();
      this.dispatchEvent(new Event('close'));
      for (const key of Object.keys(this)) {
        delete this[key];
      }
      this.closed = true;
    }
  }

  _onDismiss() {
    this.close();
  }

  _onConfirm() {
    const { remote, torrents, trash } = this.options;
    if (torrents.length > 0) {
      remote.removeTorrents(torrents, trash);
    }

    this.close();
  }

  static _create(options) {
    const { torrents } = options;
    const elements = createDialogContainer('remove-dialog');
    const { confirm, heading, message, workarea } = elements;

    heading.textContent =
      torrents.length === 1
        ? `Remove ${torrents[0].getName()}?`
        : `Remove ${torrents.length} transfers?`;

    const check = document.createElement('input');
    check.id = 'delete-local-data-check';
    check.type = 'checkbox';
    check.checked = false;
    message.append(check);

    const label = document.createElement('label');
    label.id = 'delete-local-data-label';
    label.setAttribute('for', check.id);
    label.textContent = 'Delete downloaded data';
    message.append(label);

    const body = document.createElement('div');
    const rewrite = (checked) => {
      if (checked && torrents.length === 1) {
        body.textContent =
          'All data downloaded for this torrent will be deleted. Are you sure you want to remove it?';
      } else if (checked) {
        body.textContent =
          'All data downloaded for these torrents will be deleted. Are you sure you want to remove them?';
      } else if (torrents.length === 1) {
        body.textContent =
          'Once removed, continuing the transfer will require the torrent file. Are you sure you want to remove it?';
      } else {
        body.textContent =
          'Once removed, continuing the transfers will require the torrent files. Are you sure you want to remove them?';
      }
      confirm.textContent = checked ? 'Delete' : 'Remove';
    };
    rewrite(check.checked);
    check.addEventListener('click', () => {
      options.trash = check.checked;
      rewrite(check.checked);
    });
    workarea.append(body);
    return elements;
  }
}
