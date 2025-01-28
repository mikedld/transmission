/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { createDialogContainer, setTextContent } from './utils.js';

export class RenameDialog extends EventTarget {
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
    if (torrents.length !== 1) {
      console.trace();
      return;
    }

    const { handler } = this.controller;
    this.torrents = torrents;
    this.elements = RenameDialog._create();
    this.elements.dismiss.addEventListener('click', () => this._onDismiss());
    this.elements.confirm.addEventListener('click', () => this._onConfirm());
    this.elements.entry.value =
      handler === null ? torrents[0].getName() : handler.subtree.name;
    document.body.append(this.elements.root);

    this.elements.entry.focus();
  }

  close() {
    const { handler } = this.controller;
    if (handler) {
      handler.classList.remove('selected');
    }
    this.elements.root.remove();

    this.dispatchEvent(new Event('close'));

    delete this.controller;
    delete this.remote;
    delete this.elements;
    delete this.torrents;
  }

  _onDismiss() {
    this.close();
  }

  _onConfirm() {
    const { handler } = this.controller;
    const [tor] = this.torrents;
    const file_path = handler ? handler.file_path : tor.getName();
    const new_name = this.elements.entry.value;
    this.remote.renameTorrent(
      [tor.getId()],
      file_path,
      new_name,
      (response) => {
        if (response.result === 'success') {
          const args = response.arguments;
          if (handler) {
            handler.subtree.name = args.name;
            setTextContent(handler.name_container, args.name);
            if (handler.subdir) {
              const file = tor.getIndividualFile(file_path);
              if (file) {
                const dir = file.name.slice(
                  0,
                  Math.max(0, file.name.lastIndexOf('/') + 1),
                );
                file.name = `${dir}${args.name}`;
              }
            } else {
              tor.refresh(args);
            }
          } else {
            tor.refresh(args);
          }
        }
      },
    );

    this.close();
  }

  static _create() {
    const elements = createDialogContainer('rename-dialog');
    elements.root.setAttribute('aria-label', 'Rename Torrent');
    elements.heading.textContent = 'Enter new name:';
    elements.confirm.textContent = 'Rename';

    const label = document.createElement('label');
    label.setAttribute('for', 'torrent-rename-name');
    label.textContent = 'Enter new name:';
    elements.workarea.append(label);

    const entry = document.createElement('input');
    entry.setAttribute('type', 'text');
    entry.id = 'torrent-rename-name';
    elements.entry = entry;
    elements.workarea.append(entry);

    return elements;
  }
}
