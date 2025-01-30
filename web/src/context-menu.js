/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { OutsideClickListener, setEnabled } from './utils.js';

export class ContextMenu extends EventTarget {
  constructor(action_manager) {
    super();

    this.action_listener = this._update.bind(this);
    this.action_manager = action_manager;
    this.action_manager.addEventListener('change', this.action_listener);

    Object.assign(this, this._create());

    this.outside = new OutsideClickListener(this.root);
    this.outside.addEventListener('click', () => this.close());

    this.show();
  }

  show() {
    for (const [action, item] of Object.entries(this.actions)) {
      setEnabled(item, this.action_manager.isEnabled(action));
    }
    document.body.append(this.root);
  }

  close() {
    if (!this.closed) {
      this.outside.stop();
      this.action_manager.removeEventListener('change', this.action_listener);
      this.root.remove();
      this.dispatchEvent(new Event('close'));
      for (const key of Object.keys(this)) {
        delete this[key];
      }
      this.closed = true;
    }
  }

  _update(event_) {
    const e = this.actions[event_.action];
    if (e) {
      setEnabled(e, event_.enabled);
    }
  }

  _create() {
    const root = document.createElement('div');
    root.role = 'menu';
    root.classList.add('context-menu', 'popup');
    root.addEventListener('contextmenu', (e_) => {
      e_.preventDefault();
    });
    root.style.pointerEvents = 'none';

    const actions = {};
    const new_item = (action, warn = false) => {
      const item = document.createElement('div');
      const text = this.action_manager.text(action);
      item.role = 'menuitem';
      if (warn) {
        item.classList.add('context-menuitem', 'warning');
      } else {
        item.classList.add('context-menuitem');
      }
      item.dataset.action = action;
      item.textContent = text;
      const keyshortcuts = this.action_manager.keyshortcuts(action);
      if (keyshortcuts) {
        item.setAttribute('aria-keyshortcuts', keyshortcuts);
      }
      item.addEventListener('click', () => {
        this.action_manager.click(action);
        this.close();
      });
      actions[action] = item;
      return item;
    };

    const new_separator = () => {
      const item = document.createElement('div');
      item.classList.add('context-menu-separator');
      return item;
    };

    const new_submenu = (text, ...items) => {
      const item = document.createElement('div');
      item.className = 'context-menuitem';
      item.textContent = text;

      const arrow = document.createElement('div');
      arrow.className = 'arrow';
      item.append(arrow);

      const submenu = document.createElement('div');
      submenu.className = 'submenu';
      arrow.append(submenu);

      const open = document.createElement('div');
      open.className = 'open right';
      submenu.append(open);
      open.append(...items.map((t) => new_item(t)));

      item.addEventListener('click', (e_) => {
        const t = item.lastChild.lastChild;

        if (
          !e_.target.classList.contains('right') &&
          !e_.target.parentNode.classList.contains('right') &&
          !e_.target.classList.contains('left') &&
          !e_.target.parentNode.classList.contains('left') &&
          t.style.display === 'block'
        ) {
          t.style.display = 'none';
          return;
        }

        for (const p of root.querySelectorAll('.submenu')) {
          p.style.display = 'none';
        }

        t.style.display = 'block';
        const where = item.getBoundingClientRect();
        const wheret = t.lastChild.getBoundingClientRect();
        const y = Math.min(
          0,
          document.documentElement.clientHeight -
            window.visualViewport.offsetTop -
            where.top -
            t.clientHeight +
            3,
        );
        const x = Math.min(
          0,
          document.documentElement.clientWidth -
            window.visualViewport.offsetLeft -
            where.right -
            t.clientWidth,
        );

        t.style.top = `${y}px`;
        if (x) {
          t.lastChild.className = 'open left';
          t.style.left = `${-where.width - wheret.width}px`;
        } else {
          t.lastChild.className = 'open right';
          t.style.left = `${x}px`;
        }
      });

      return item;
    };

    root.append(
      new_item('resume-selected-torrents'),
      new_item('resume-selected-torrents-now'),
      new_item('pause-selected-torrents'),
      new_separator(),
      new_submenu(
        'Move in the queue',
        'move-top',
        'move-up',
        'move-down',
        'move-bottom',
      ),
      new_separator(),
      new_item('remove-selected-torrents', true),
      new_item('trash-selected-torrents', true),
      new_separator(),
      new_item('verify-selected-torrents'),
      new_item('show-move-dialog'),
      new_item('show-rename-dialog'),
      new_item('show-labels-dialog'),
      new_separator(),
      new_item('reannounce-selected-torrents'),
      new_separator(),
      new_submenu('Select operation', 'select-all', 'deselect-all'),
    );

    return { actions, root };
  }
}
