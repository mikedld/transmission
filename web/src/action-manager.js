/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

export class ActionManager extends EventTarget {
  constructor() {
    super();
    this.actions = Object.seal({
      'deselect-all': {
        enabled: false,
        shortcut: 'D',
        text: 'Deselect all',
      },
      'disable-sequential-downloading': {
        enabled: true,
        text: 'Disable sequential downloading',
      },
      'enable-sequential-downloading': {
        enabled: true,
        text: 'Enable sequential downloading',
      },
      'move-bottom': { enabled: false, text: 'Move to the back of the queue' },
      'move-down': { enabled: false, text: 'Move down in the queue' },
      'move-top': { enabled: false, text: 'Move to the front of the queue' },
      'move-up': { enabled: false, text: 'Move up in the queue' },
      'open-torrent': {
        enabled: true,
        shortcut: 'O',
        text: 'Open torrent…',
      },
      'pause-all-torrents': { enabled: false, text: 'Pause all' },
      'pause-selected-torrents': {
        enabled: false,
        shortcut: 'U',
        text: 'Pause',
      },
      'reannounce-selected-torrents': {
        enabled: false,
        text: 'Ask tracker for more peers',
      },
      'remove-selected-torrents': { enabled: false, text: 'Remove from list…' },
      'resume-selected-torrents': {
        enabled: false,
        shortcut: 'R',
        text: 'Resume',
      },
      'resume-selected-torrents-now': { enabled: false, text: 'Resume now' },
      'select-all': {
        enabled: false,
        shortcut: 'A',
        text: 'Select all',
      },
      'show-about-dialog': { enabled: true, text: 'About' },
      'show-inspector': {
        enabled: false,
        shortcut: 'I',
        text: 'Torrent Inspector',
      },
      'show-labels-dialog': {
        enabled: false,
        shortcut: 'K',
        text: 'Edit Labels…',
      },
      'show-move-dialog': {
        enabled: false,
        shortcut: 'L',
        text: 'Set location…',
      },
      'show-overflow-menu': { enabled: true, text: 'More options…' },
      'show-preferences-dialog': {
        enabled: true,
        shortcut: 'P',
        text: 'Edit preferences',
      },
      'show-rename-dialog': {
        enabled: false,
        shortcut: 'N',
        text: 'Rename…',
      },
      'show-shortcuts-dialog': { enabled: true, text: 'Keyboard shortcuts' },
      'show-statistics-dialog': {
        enabled: true,
        shortcut: 'S',
        text: 'Statistics',
      },
      'start-all-torrents': { enabled: false, text: 'Start all' },
      'toggle-compact-rows': { enabled: true, text: 'Compact rows' },
      'toggle-contrast': { enabled: true, text: 'High contrast UI' },
      'trash-selected-torrents': {
        enabled: false,
        text: 'Trash data and remove from list…',
      },
      'verify-selected-torrents': {
        enabled: false,
        shortcut: 'V',
        text: 'Verify local data',
      },
    });
  }

  click(name) {
    if (this.isEnabled(name)) {
      const event_ = new Event('click');
      event_.action = name;
      this.dispatchEvent(event_);
    }
  }

  getActionForShortcut(shortcut) {
    for (const [name, properties] of Object.entries(this.actions)) {
      if (shortcut === properties.shortcut) {
        return name;
      }
    }
    return null;
  }

  // return a map of shortcuts to action names
  allShortcuts() {
    return new Map(
      Object.entries(this.actions)
        .filter(([, properties]) => properties.shortcut)
        .map(([name, properties]) => [properties.shortcut, name]),
    );
  }

  isEnabled(name) {
    return this._getAction(name).enabled;
  }

  text(name) {
    return this._getAction(name).text;
  }

  keyshortcuts(name) {
    return this._getAction(name).shortcut;
  }

  update(event_) {
    const counts = ActionManager._recount(event_.selected, event_.nonselected);
    this._updateStates(counts);
  }

  _getAction(name) {
    const action = this.actions[name];
    if (!action) {
      throw new Error(`no such action: ${name}`);
    }
    return action;
  }

  static _recount(selected, nonselected) {
    const total = selected.length + nonselected.length;
    const selected_paused = selected.filter((tor) => tor.isStopped()).length;
    const selected_active = selected.length - selected_paused;
    const nonselected_paused = nonselected.filter((tor) =>
      tor.isStopped(),
    ).length;
    const nonselected_active = nonselected.length - nonselected_paused;
    const paused = selected_paused + nonselected_paused;
    const active = selected_active + nonselected_active;
    const selected_queued = selected.filter((tor) => tor.isQueued()).length;

    return {
      active,
      nonselected_active,
      nonselected_paused,
      paused,
      selected: selected.length,
      selected_active,
      selected_paused,
      selected_queued,
      total,
    };
  }

  _updateStates(counts) {
    const set_enabled = (enabled, actions) => {
      for (const action of actions) {
        this._updateActionState(action, enabled);
      }
    };

    set_enabled(counts.selected_paused > 0, ['resume-selected-torrents']);

    set_enabled(counts.paused > 0, ['start-all-torrents']);

    set_enabled(counts.active > 0, ['pause-all-torrents']);

    set_enabled(counts.selected_paused > 0 || counts.selected_queued > 0, [
      'resume-selected-torrents-now',
    ]);

    set_enabled(counts.selected_active > 0, [
      'pause-selected-torrents',
      'reannounce-selected-torrents',
    ]);

    set_enabled(counts.selected > 0, [
      'deselect-all',
      'move-bottom',
      'move-down',
      'move-top',
      'move-up',
      'remove-selected-torrents',
      'show-inspector',
      'show-labels-dialog',
      'show-move-dialog',
      'trash-selected-torrents',
      'verify-selected-torrents',
    ]);

    set_enabled(counts.selected === 1, ['show-rename-dialog']);

    set_enabled(counts.selected < counts.total, ['select-all']);
  }

  _updateActionState(name, enabled) {
    const action = this.actions[name];
    if (!action) {
      throw new Error(`no such action: ${name}`);
    }

    if (action.enabled !== enabled) {
      action.enabled = enabled;

      const event = new Event('change');
      event.action = name;
      event.enabled = enabled;
      this.dispatchEvent(event);
    }
  }
}
