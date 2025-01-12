/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { Formatter } from './formatter.js';
import {
  addCheckedClass,
  makeUUID,
  setEnabled,
  setTextContent,
} from './utils.js';

export class FileRow extends EventTarget {
  isDone() {
    return this.fields.have >= this.fields.size;
  }

  isEditable() {
    return this.fields.torrent.getFileCount() > 1 && !this.isDone();
  }

  refreshWantedHTML() {
    const e = this.elements.root;
    e.classList.toggle('skip', !this.fields.isWanted);
    e.classList.toggle('complete', this.isDone());
    setEnabled(e.checkbox, this.isEditable());
    e.checkbox.checked = this.fields.isWanted;
  }

  refreshProgressHTML() {
    const { size, have } = this.fields;
    const pct = 100 * (size ? have / size : 1);
    const fmt = Formatter;
    const c = `${fmt.size(have)} of ${fmt.size(size)} (${fmt.percentString(
      pct,
      1,
    )}%)`;
    setTextContent(this.elements.progress, c);
  }

  refresh() {
    let have = 0;
    let high = false;
    let low = false;
    let normal = false;
    let size = 0;
    let wanted = false;

    // loop through the file_indices that affect this row
    const files = this.fields.torrent.getFiles();
    for (const index of this.fields.indices) {
      const file = files[index];
      have += file.bytesCompleted;
      size += file.length;
      wanted |= file.wanted;
      switch (file.priority.toString()) {
        case '-1':
          low = true;
          break;
        case '1':
          high = true;
          break;
        default:
          normal = true;
          break;
      }
    }

    addCheckedClass(this.elements.priority_low_button, low);
    addCheckedClass(this.elements.priority_normal_button, normal);
    addCheckedClass(this.elements.priority_high_button, high);

    if (this.fields.have !== have || this.fields.size !== size) {
      this.fields.have = have;
      this.fields.size = size;
      this.refreshProgressHTML();
    }

    if (this.fields.isWanted !== wanted) {
      this.fields.isWanted = wanted;
      this.refreshWantedHTML();
    }
  }

  fireWantedChanged(wanted) {
    const e = new Event('wantedToggled');
    e.indices = [...this.fields.indices];
    e.wanted = wanted;
    this.dispatchEvent(e);
  }

  firePriorityChanged(priority) {
    const e = new Event('priorityToggled');
    e.indices = [...this.fields.indices];
    e.priority = priority;
    this.dispatchEvent(e);
  }

  createRow(torrent, subtree) {
    const root = document.createElement('li');
    root.classList.add('inspector-torrent-file-list-entry');
    root.subtree = subtree;
    this.elements.root = root;

    const right_click = (e_) => {
      if (this.controller.handler) {
        this.controller.handler.classList.remove('selected');
      }
      setTimeout(() => {
        root.classList.add('selected');
      }, 0);

      let file_path = subtree.name;
      let sub = subtree.parent;
      while (sub.name) {
        root.subdir = true;
        file_path = `${sub.name}/${file_path}`;
        const { parent } = sub;
        sub = parent;
      }

      root.file_path = file_path;
      this.controller.handler = root;

      const menu_items = ['show-rename-dialog', null, 'copy-name'];
      this.controller.context_menu('#inspector', menu_items);
      e_.preventDefault();
    };
    this.controller.pointer_event(root, right_click);

    let e = document.createElement('input');
    const check_id = makeUUID();
    e.type = 'checkbox';
    e.className = 'file-wanted-control';
    e.title = 'Download file';
    e.id = check_id;
    e.addEventListener('change', (event_) =>
      this.fireWantedChanged(event_.target.checked),
    );
    root.checkbox = e;
    root.append(e);

    e = document.createElement('label');
    e.className = 'inspector-torrent-file-list-entry-name';
    e.setAttribute('for', check_id);
    setTextContent(e, subtree.name);
    root.append(e);
    root.name_container = e;

    e = document.createElement('div');
    e.className = 'inspector-torrent-file-list-entry-progress';
    root.append(e);
    this.elements.progress = e;

    e = document.createElement('div');
    e.className = 'file-priority-radiobox';
    const box = e;

    const priority_click_listener = (event_) =>
      this.firePriorityChanged(event_.target.value);

    e = document.createElement('input');
    e.type = 'radio';
    e.value = '-1';
    e.className = 'low';
    e.title = 'Low Priority';
    e.addEventListener('click', priority_click_listener);
    this.elements.priority_low_button = e;
    box.append(e);

    e = document.createElement('input');
    e.type = 'radio';
    e.value = '0';
    e.className = 'normal';
    e.title = 'Normal Priority';
    e.addEventListener('click', priority_click_listener);
    this.elements.priority_normal_button = e;
    box.append(e);

    e = document.createElement('input');
    e.type = 'radio';
    e.value = '1';
    e.title = 'High Priority';
    e.className = 'high';
    e.addEventListener('click', priority_click_listener);
    this.elements.priority_high_button = e;
    box.append(e);

    root.append(box);

    root.style.paddingLeft = `${subtree.depth * 20}px`;

    this.refresh();
  }

  /// PUBLIC

  getElement() {
    return this.elements.root;
  }

  constructor(controller, torrent, subtree) {
    super();

    this.controller = controller;
    this.fields = {
      have: 0,
      indices: subtree.file_indices,
      isWanted: true,
      // priorityHigh: false,
      // priorityLow: false,
      // priorityNormal: false,
      size: 0,
      torrent,
    };
    this.elements = {
      priority_high_button: null,
      priority_low_button: null,
      priority_normal_button: null,
      progress: null,
      root: null,
    };
    this.createRow(torrent, subtree);
  }
}
