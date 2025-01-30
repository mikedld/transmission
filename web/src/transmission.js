/* @license This file Copyright © Charles Kerr, Dave Perrett, Malcolm Jarvis and Bruno Bierbaumer
   It may be used under GPLv2 (SPDX: GPL-2.0-only).
   License text can be found in the licenses/ folder. */

import { AboutDialog } from './about-dialog.js';
import { ContextMenu } from './context-menu.js';
import { Formatter } from './formatter.js';
import { Inspector } from './inspector.js';
import { MoveDialog } from './move-dialog.js';
import { OpenDialog } from './open-dialog.js';
import { OverflowMenu } from './overflow-menu.js';
import { Prefs } from './prefs.js';
import { PrefsDialog } from './prefs-dialog.js';
import { Remote, RPC } from './remote.js';
import { RemoveDialog } from './remove-dialog.js';
import { RenameDialog } from './rename-dialog.js';
import { LabelsDialog } from './labels-dialog.js';
import { ShortcutsDialog } from './shortcuts-dialog.js';
import { StatisticsDialog } from './statistics-dialog.js';
import { Torrent } from './torrent.js';
import {
  TorrentRow,
  TorrentRendererCompact,
  TorrentRendererFull,
} from './torrent-row.js';
import {
  newOpts,
  debounce,
  deepEqual,
  setEnabled,
  setTextContent,
} from './utils.js';

export class Transmission extends EventTarget {
  constructor(action_manager, notifications, prefs) {
    super();

    // Initialize the helper classes
    this.action_manager = action_manager;
    this.notifications = notifications;
    this.prefs = prefs;
    this.remote = new Remote(this);

    this.addEventListener('torrent-selection-changed', (event_) =>
      this.action_manager.update(event_),
    );

    // Initialize the implementation fields
    this.filterText = '';
    this._torrents = {};
    this._rows = [];
    this.oldTrackers = [];
    this.dirtyTorrents = new Set();

    this.changeStatus = false;
    this.refilterSoon = debounce(() => this._refilter(false));
    this.refilterAllSoon = debounce(() => this._refilter(true));

    this.pointer_device = Object.seal({
      is_touch_device: 'ontouchstart' in window,
      long_press_callback: null,
      x: 0,
      y: 0,
    });
    this.popup = Array.from({ length: Transmission.max_popups }).fill(null);

    // listen to actions
    // TODO: consider adding a mutator listener here to see dynamic additions
    for (const element of document.querySelectorAll(`button[data-action]`)) {
      const { action } = element.dataset;
      setEnabled(element, this.action_manager.isEnabled(action));
      element.addEventListener('click', () => {
        this.action_manager.click(action);
      });
    }

    document
      .querySelector('#filter-tracker')
      .addEventListener('change', (event_) => {
        this.setFilterTracker(event_.target.value);
      });

    this.action_manager.addEventListener('change', (event_) => {
      for (const element of document.querySelectorAll(
        `[data-action="${event_.action}"]`,
      )) {
        setEnabled(element, event_.enabled);
      }
    });

    this.action_manager.addEventListener('click', (event_) => {
      switch (event_.action) {
        case 'deselect-all':
          this._deselectAll();
          break;
        case 'move-bottom':
          this._moveBottom();
          break;
        case 'move-down':
          this._moveDown();
          break;
        case 'move-top':
          this._moveTop();
          break;
        case 'move-up':
          this._moveUp();
          break;
        case 'open-torrent':
          this.setCurrentPopup(new OpenDialog(this, this.remote));
          break;
        case 'pause-all-torrents':
          this._stopTorrents(this._getAllTorrents());
          break;
        case 'pause-selected-torrents':
          this._stopTorrents(this.getSelectedTorrents());
          break;
        case 'reannounce-selected-torrents':
          this._reannounceTorrents(this.getSelectedTorrents());
          break;
        case 'remove-selected-torrents':
          this._removeSelectedTorrents(false);
          break;
        case 'resume-selected-torrents':
          this._startSelectedTorrents(false);
          break;
        case 'resume-selected-torrents-now':
          this._startSelectedTorrents(true);
          break;
        case 'select-all':
          this._selectAll();
          break;
        case 'show-about-dialog':
          this.setCurrentPopup(new AboutDialog(this.version_info));
          break;
        case 'show-inspector':
          if (this.popup[0] instanceof Inspector) {
            this.popup[0].close();
          } else {
            this.setCurrentPopup(new Inspector(this), 0);
          }
          break;
        case 'show-move-dialog':
          this.setCurrentPopup(new MoveDialog(this, this.remote));
          break;
        case 'show-overflow-menu':
          if (
            this.popup[Transmission.default_popup_level] instanceof OverflowMenu
          ) {
            this.popup[Transmission.default_popup_level].close();
          } else {
            this.setCurrentPopup(
              new OverflowMenu(
                this,
                this.prefs,
                this.remote,
                this.action_manager,
              ),
            );
          }
          break;
        case 'show-preferences-dialog':
          this.setCurrentPopup(new PrefsDialog(this, this.remote), 0);
          break;
        case 'show-shortcuts-dialog':
          this.setCurrentPopup(new ShortcutsDialog(this.action_manager));
          break;
        case 'show-statistics-dialog':
          this.setCurrentPopup(new StatisticsDialog(this.remote));
          break;
        case 'show-rename-dialog':
          this.setCurrentPopup(new RenameDialog(this, this.remote));
          break;
        case 'show-labels-dialog':
          this.setCurrentPopup(new LabelsDialog(this, this.remote));
          break;
        case 'start-all-torrents':
          this._startTorrents(this._getAllTorrents());
          break;
        case 'toggle-compact-rows':
          this.prefs.display_mode =
            this.prefs.display_mode === Prefs.DisplayCompact
              ? Prefs.DisplayFull
              : Prefs.DisplayCompact;
          break;
        case 'trash-selected-torrents':
          this._removeSelectedTorrents(true);
          break;
        case 'verify-selected-torrents':
          this._verifyTorrents(this.getSelectedTorrents());
          break;
        default:
          console.warn(`unhandled action: ${event_.action}`);
      }
    });

    let e = document.querySelector('#filter-mode');
    // Initialize filter options
    newOpts(e, null, [['All', Prefs.FilterAll]]);
    newOpts(e, 'status', [
      ['Active', Prefs.FilterActive],
      ['Downloading', Prefs.FilterDownloading],
      ['Seeding', Prefs.FilterSeeding],
      ['Paused', Prefs.FilterPaused],
      ['Finished', Prefs.FilterFinished],
      ['Error', Prefs.FilterError],
    ]);
    newOpts(e, 'list', [
      ['Private torrents', Prefs.FilterPrivate],
      ['Public torrents', Prefs.FilterPublic],
    ]);

    // listen to filter changes
    e.value = this.prefs.filter_mode;
    e.addEventListener('change', (event_) => {
      this.prefs.filter_mode = event_.target.value;
      this.refilterAllSoon();
    });

    e = document.querySelector('#filter-tracker');
    newOpts(e, null, [['All', Prefs.FilterAll]]);

    document.addEventListener('keydown', this._keyDown.bind(this));
    document.addEventListener('keyup', this._keyUp.bind(this));
    e = document.querySelector('#torrent-container');
    e.addEventListener('click', (e_) => {
      if (this.popup[Transmission.default_popup_level]) {
        this.setCurrentPopup(null);
      }
      if (e_.target === e_.currentTarget) {
        this._deselectAll();
      }
    });
    e.addEventListener('dblclick', () => {
      if (!this.popup[0] || this.popup[0].name !== 'inspector') {
        this.action_manager.click('show-inspector');
      }
    });
    e.addEventListener('dragenter', Transmission._dragenter);
    e.addEventListener('dragover', Transmission._dragenter);
    e.addEventListener('drop', this._drop.bind(this));
    this._setupSearchBox();

    this.elements = {
      torrent_list: document.querySelector('#torrent-list'),
    };

    const context_menu = () => {
      // open context menu
      const popup = new ContextMenu(this.action_manager);
      this.setCurrentPopup(popup);

      const boundingElement = document.querySelector('#torrent-container');
      const bounds = boundingElement.getBoundingClientRect();
      const x = Math.min(
        this.pointer_device.x,
        bounds.right + globalThis.scrollX - popup.root.clientWidth,
      );
      const y = Math.min(
        this.pointer_device.y,
        bounds.bottom + globalThis.scrollY - popup.root.clientHeight,
      );
      popup.root.style.left = `${Math.max(x, 0)}px`;
      popup.root.style.top = `${Math.max(y, 0)}px`;
    };

    const right_click = (event_) => {
      if (this.pointer_device.is_touch_device && event_.touches.length > 1) {
        return;
      }

      // if not already, highlight the torrent
      let row_element = event_.target;
      while (row_element && !row_element.classList.contains('torrent')) {
        row_element = row_element.parentNode;
      }
      const row = this._rows.find((r) => r.getElement() === row_element);
      if (row && !row.isSelected()) {
        this._setSelectedRow(row);
      }

      context_menu();
      event_.preventDefault();
    };

    if (this.pointer_device.is_touch_device) {
      const touch = this.pointer_device;
      this.elements.torrent_list.addEventListener('touchstart', (event_) => {
        touch.x = event_.touches[0].pageX;
        touch.y = event_.touches[0].pageY;

        if (touch.long_press_callback) {
          clearTimeout(touch.long_press_callback);
          touch.long_press_callback = null;
        } else {
          touch.long_press_callback = setTimeout(
            right_click.bind(this),
            500,
            event_,
          );
        }
      });
      this.elements.torrent_list.addEventListener('touchend', () => {
        clearTimeout(touch.long_press_callback);
        touch.long_press_callback = null;
        setTimeout(() => {
          const popup = this.popup[Transmission.default_popup_level];
          if (popup) {
            popup.root.style.pointerEvents = 'auto';
          }
        }, 1);
      });
      this.elements.torrent_list.addEventListener('touchmove', (event_) => {
        touch.x = event_.touches[0].pageX;
        touch.y = event_.touches[0].pageY;

        clearTimeout(touch.long_press_callback);
        touch.long_press_callback = null;
      });
      this.elements.torrent_list.addEventListener('contextmenu', (event_) => {
        event_.preventDefault();
      });
    } else {
      this.elements.torrent_list.addEventListener('mousemove', (event_) => {
        this.pointer_device.x = event_.pageX;
        this.pointer_device.y = event_.pageY;
      });
      this.elements.torrent_list.addEventListener('contextmenu', (event_) => {
        right_click(event_);
        const popup = this.popup[Transmission.default_popup_level];
        if (popup) {
          popup.root.style.pointerEvents = 'auto';
        }
      });
    }

    // Get preferences & torrents from the daemon
    this.loadDaemonPrefs();
    this._initializeTorrents();
    this.refreshTorrents();
    this.togglePeriodicSessionRefresh(true);

    // this.updateButtonsSoon();

    this.prefs.addEventListener('change', ({ key, value }) =>
      this._onPrefChanged(key, value),
    );
    for (const [key, value] of this.prefs.entries()) {
      this._onPrefChanged(key, value);
    }
  }

  _openTorrentFromUrl() {
    setTimeout(() => {
      const addTorrent = new URLSearchParams(window.location.search).get(
        'addtorrent',
      );
      if (addTorrent) {
        this.setCurrentPopup(new OpenDialog(this, this.remote, addTorrent));
        const newUrl = new URL(window.location);
        newUrl.search = '';
        window.history.pushState('', '', newUrl.toString());
      }
    }, 0);
  }

  loadDaemonPrefs() {
    this.remote.loadDaemonPrefs((data) => {
      this.session_properties = data.arguments;
      this._openTorrentFromUrl();
    });
  }

  get session_properties() {
    return this._session_properties;
  }
  set session_properties(o) {
    if (deepEqual(this._session_properties, o)) {
      return;
    }

    this._session_properties = Object.seal(o);
    const event = new Event('session-change');
    event.session_properties = o;
    this.dispatchEvent(event);

    // TODO: maybe have this in a listener handler?
    this._updateGuiFromSession(o);
  }

  _setupSearchBox() {
    const e = document.querySelector('#torrent-search');
    const blur_token = 'blur';
    e.classList.add(blur_token);
    e.addEventListener('blur', () => e.classList.add(blur_token));
    e.addEventListener('focus', () => e.classList.remove(blur_token));
    e.addEventListener('keyup', () => this._setFilterText(e.value));
  }

  _onPrefChanged(key, value) {
    switch (key) {
      case Prefs.DisplayMode: {
        this.torrentRenderer =
          value === 'compact'
            ? new TorrentRendererCompact()
            : new TorrentRendererFull();
        this.refilterAllSoon();
        break;
      }
      case Prefs.ContrastMode: {
        // Add custom class to the body/html element to get the appropriate contrast color scheme
        document.body.classList.remove('contrast-more');
        document.body.classList.remove('contrast-less');
        document.body.classList.add(`contrast-${value}`);
        // this.refilterAllSoon();
        break;
      }

      case Prefs.FilterMode:
      case Prefs.SortDirection:
      case Prefs.SortMode:
        this.refilterAllSoon();
        break;

      case Prefs.RefreshRate: {
        clearInterval(this.refreshTorrentsInterval);
        const callback = this.refreshTorrents.bind(this);
        const pref = this.prefs.refresh_rate_sec;
        const msec = pref > 0 ? pref * 1000 : 1000;
        this.refreshTorrentsInterval = setInterval(callback, msec);
        break;
      }

      default:
        /*noop*/
        break;
    }
  }

  /// UTILITIES

  static get max_popups() {
    return 2;
  }

  static get default_popup_level() {
    return Transmission.max_popups - 1;
  }

  _getAllTorrents() {
    return Object.values(this._torrents);
  }

  static _getTorrentIds(torrents) {
    return torrents.map((t) => t.getId());
  }

  seedRatioLimit() {
    const p = this.session_properties;
    if (p && p.seedRatioLimited) {
      return p.seedRatioLimit;
    }
    return -1;
  }

  /// SELECTION

  _getSelectedRows() {
    return this._rows.filter((r) => r.isSelected());
  }

  getSelectedTorrents() {
    return this._getSelectedRows().map((r) => r.getTorrent());
  }

  _getSelectedTorrentIds() {
    return Transmission._getTorrentIds(this.getSelectedTorrents());
  }

  _setSelectedRow(row) {
    const e_sel = row ? row.getElement() : null;
    for (const e of this.elements.torrent_list.children) {
      e.classList.toggle('selected', e === e_sel);
    }
    this._dispatchSelectionChanged();
  }

  _selectRow(row) {
    row.getElement().classList.add('selected');
    this._dispatchSelectionChanged();
  }

  _deselectRow(row) {
    row.getElement().classList.remove('selected');
    this._dispatchSelectionChanged();
  }

  _selectAll() {
    for (const e of this.elements.torrent_list.children) {
      e.classList.add('selected');
    }
    this._dispatchSelectionChanged();
  }

  _deselectAll() {
    for (const e of this.elements.torrent_list.children) {
      e.classList.remove('selected');
    }
    this._dispatchSelectionChanged();
    delete this._last_torrent_clicked;
  }

  _indexOfLastTorrent() {
    return this._rows.findIndex(
      (row) => row.getTorrentId() === this._last_torrent_clicked,
    );
  }

  // Select a range from this row to the last clicked torrent
  _selectRange(row) {
    const last = this._indexOfLastTorrent();

    if (last === -1) {
      this._selectRow(row);
    } else {
      // select the range between the previous & current
      const next = this._rows.indexOf(row);
      const min = Math.min(last, next);
      const max = Math.max(last, next);
      for (let index = min; index <= max; ++index) {
        this._selectRow(this._rows[index]);
      }
    }

    this._dispatchSelectionChanged();
  }

  _dispatchSelectionChanged() {
    const nonselected = [];
    const selected = [];
    for (const r of this._rows) {
      (r.isSelected() ? selected : nonselected).push(r.getTorrent());
    }

    const event = new Event('torrent-selection-changed');
    event.nonselected = nonselected;
    event.selected = selected;
    this.dispatchEvent(event);
  }

  /*--------------------------------------------
   *
   *  E V E N T   F U N C T I O N S
   *
   *--------------------------------------------*/

  static _createKeyShortcutFromKeyboardEvent(event_) {
    const a = [];
    if (event_.ctrlKey) {
      a.push('Control');
    }
    if (event_.altKey) {
      a.push('Alt');
    }
    if (event_.metaKey) {
      a.push('Meta');
    }
    if (event_.shiftKey) {
      a.push('Shift');
    }
    a.push(event_.key.length === 1 ? event_.key.toUpperCase() : event_.key);
    return a.join('+');
  }

  // Process key events
  _keyDown(event_) {
    const { ctrlKey, keyCode, metaKey, shiftKey, target } = event_;

    // look for a shortcut
    const is_input_focused = ['INPUT', 'TEXTAREA'].includes(target.tagName);
    if (!is_input_focused) {
      const shortcut = Transmission._createKeyShortcutFromKeyboardEvent(event_);
      const action = this.action_manager.getActionForShortcut(shortcut);
      if (action) {
        event_.preventDefault();
        this.action_manager.click(action);
        return;
      }
    }

    const esc_key = keyCode === 27; // esc key pressed
    if (esc_key && this.popup.some(Boolean)) {
      this.setCurrentPopup(null, 0);
      event_.preventDefault();
      return;
    }

    const any_popup_active = document.querySelector('.popup:not(.hidden)');
    const rows = this._rows;

    // Some shortcuts can only be used if the following conditions are met:
    // 1. when no input fields are focused
    // 2. when no other dialogs are visible
    // 3. when the meta or ctrl key isn't pressed (i.e. opening dev tools shouldn't trigger the info panel)
    if (!is_input_focused && !any_popup_active && !metaKey && !ctrlKey) {
      const shift_key = keyCode === 16; // shift key pressed
      const up_key = keyCode === 38; // up key pressed
      const dn_key = keyCode === 40; // down key pressed
      if ((up_key || dn_key) && rows.length > 0) {
        const last = this._indexOfLastTorrent();
        const anchor = this._shift_index;
        const min = 0;
        const max = rows.length - 1;
        let index = last;

        if (dn_key && index + 1 <= max) {
          ++index;
        } else if (up_key && index - 1 >= min) {
          --index;
        }

        const r = rows[index];

        if (anchor >= 0) {
          // user is extending the selection
          // with the shift + arrow keys...
          if (
            (anchor <= last && last < index) ||
            (anchor >= last && last > index)
          ) {
            this._selectRow(r);
          } else if (
            (anchor >= last && index > last) ||
            (anchor <= last && last > index)
          ) {
            this._deselectRow(rows[last]);
          }
        } else {
          if (shiftKey) {
            this._selectRange(r);
          } else {
            this._setSelectedRow(r);
          }
        }
        if (r) {
          this._last_torrent_clicked = r.getTorrentId();
          r.getElement().scrollIntoView();
          event_.preventDefault();
        }
      } else if (shift_key) {
        this._shift_index = this._indexOfLastTorrent();
      }
    }
  }

  _keyUp(event_) {
    if (event_.keyCode === 16) {
      // shift key pressed
      delete this._shift_index;
    }
  }

  static _dragenter(event_) {
    if (event_.dataTransfer && event_.dataTransfer.types) {
      const copy_types = new Set(['text/uri-list', 'text/plain']);
      if (
        event_.dataTransfer.types.some((type) => copy_types.has(type)) ||
        event_.dataTransfer.types.includes('Files')
      ) {
        event_.stopPropagation();
        event_.preventDefault();
        event_.dataTransfer.dropEffect = 'copy';
        return false;
      }
    } else if (event_.dataTransfer) {
      event_.dataTransfer.dropEffect = 'none';
    }
    return true;
  }

  static _isValidURL(string) {
    try {
      const url = new URL(string);
      return Boolean(url);
    } catch {
      return false;
    }
  }

  shouldAddedTorrentsStart() {
    return this.session_properties['start-added-torrents'];
  }

  _drop(event_) {
    const paused = !this.shouldAddedTorrentsStart();

    if (!event_.dataTransfer || !event_.dataTransfer.types) {
      return true;
    }

    const type = event_.dataTransfer.types.findLast((t) =>
      ['text/uri-list', 'text/plain'].includes(t),
    );
    for (const uri of event_.dataTransfer
      .getData(type)
      .split('\n')
      .map((string) => string.trim())
      .filter((string) => Transmission._isValidURL(string))) {
      this.remote.addTorrentByUrl(uri, paused);
    }

    const { files } = event_.dataTransfer;

    if (files.length > 0) {
      this.setCurrentPopup(new OpenDialog(this, this.remote, '', files));
    }
    event_.preventDefault();
    return false;
  }

  // turn the periodic ajax session refresh on & off
  togglePeriodicSessionRefresh(enabled) {
    if (!enabled && this.sessionInterval) {
      clearInterval(this.sessionInterval);
      delete this.sessionInterval;
    }
    if (enabled) {
      this.loadDaemonPrefs();
      if (!this.sessionInterval) {
        const msec = 8000;
        this.sessionInterval = setInterval(
          this.loadDaemonPrefs.bind(this),
          msec,
        );
      }
    }
  }

  _setFilterText(search) {
    this.filterText = search ? search.trim() : null;
    this.refilterAllSoon();
  }

  _onTorrentChanged(event_) {
    if (this.changeStatus) {
      this._dispatchSelectionChanged();
      this.changeStatus = false;
    }

    // update our dirty fields
    const tor = event_.currentTarget;
    this.dirtyTorrents.add(tor.getId());

    // enqueue ui refreshes
    this.refilterSoon();
  }

  updateTorrents(ids, fields) {
    this.remote.updateTorrents(ids, fields, (table, removed_ids) => {
      const needinfo = [];

      const keys = table.shift();
      const o = {};
      for (const row of table) {
        for (const [index, key] of keys.entries()) {
          o[key] = row[index];
        }
        const { id } = o;
        let t = this._torrents[id];
        if (t) {
          const needed = t.needsMetaData();
          t.refresh(o);
          if (needed && !t.needsMetaData()) {
            needinfo.push(id);
          }
        } else {
          t = this._torrents[id] = new Torrent(o);
          t.addEventListener('dataChanged', this._onTorrentChanged.bind(this));
          this.dirtyTorrents.add(id);
          // do we need more info for this torrent?
          if (!('name' in t.fields) || !('status' in t.fields)) {
            needinfo.push(id);
          }
        }
      }

      if (needinfo.length > 0) {
        // whee, new torrents! get their initial information.
        const more_fields = [
          'id',
          ...Torrent.Fields.Metadata,
          ...Torrent.Fields.Stats,
        ];
        this.updateTorrents(needinfo, more_fields);
        this.refilterSoon();
      }

      if (removed_ids) {
        this._deleteTorrents(removed_ids);
        this.refilterSoon();
      }
    });
  }
  /*
TODO: fix this when notifications get fixed
        t.notifyOnFieldChange('status', (newValue, oldValue) => {
          if (
            oldValue === Torrent._StatusDownload &&
            (newValue === Torrent._StatusSeed || newValue === Torrent._StatusSeedWait)
          ) {
            $(this).trigger('downloadComplete', [t]);
          } else if (
            oldValue === Torrent._StatusSeed &&
            newValue === Torrent._StatusStopped &&
            t.isFinished()
          ) {
            $(this).trigger('seedingComplete', [t]);
          } else {
            $(this).trigger('statusChange', [t]);
          }
        });
*/

  refreshTorrents() {
    const fields = ['id', ...Torrent.Fields.Stats];
    this.updateTorrents('recently-active', fields);
  }

  _initializeTorrents() {
    const fields = ['id', ...Torrent.Fields.Metadata, ...Torrent.Fields.Stats];
    this.updateTorrents(null, fields);
  }

  _onRowClicked(event_) {
    const meta_key = event_.metaKey || event_.ctrlKey,
      { row } = event_.currentTarget;

    if (this.popup[Transmission.default_popup_level]) {
      this.setCurrentPopup(null);
    }

    // Prevents click carrying to parent element
    // which deselects all on click
    event_.stopPropagation();

    if (event_.shiftKey) {
      this._selectRange(row);
      // Need to deselect any selected text
      window.focus();

      // Apple-Click, not selected
    } else if (!row.isSelected() && meta_key) {
      this._selectRow(row);

      // Regular Click, not selected
    } else if (!row.isSelected()) {
      this._setSelectedRow(row);

      // Apple-Click, selected
    } else if (row.isSelected() && meta_key) {
      this._deselectRow(row);

      // Regular Click, selected
    } else if (row.isSelected()) {
      this._setSelectedRow(row);
    }

    this._last_torrent_clicked = row.getTorrentId();
  }

  _deleteTorrents(ids) {
    if (ids && ids.length > 0) {
      for (const id of ids) {
        this.dirtyTorrents.add(id);
        delete this._torrents[id];
      }
      this.refilterSoon();
    }
  }

  _removeSelectedTorrents(trash) {
    const torrents = this.getSelectedTorrents();
    if (torrents.length > 0) {
      this.setCurrentPopup(
        new RemoveDialog({ remote: this.remote, torrents, trash }),
      );
    }
  }

  _startSelectedTorrents(force) {
    this._startTorrents(this.getSelectedTorrents(), force);
  }

  _startTorrents(torrents, force) {
    this.changeStatus = true;
    this.remote.startTorrents(
      Transmission._getTorrentIds(torrents),
      force,
      this.refreshTorrents,
      this,
    );
  }
  _verifyTorrents(torrents) {
    this.remote.verifyTorrents(
      Transmission._getTorrentIds(torrents),
      this.refreshTorrents,
      this,
    );
  }

  _reannounceTorrents(torrents) {
    this.remote.reannounceTorrents(
      Transmission._getTorrentIds(torrents),
      this.refreshTorrents,
      this,
    );
  }

  _stopTorrents(torrents) {
    this.changeStatus = true;
    this.remote.stopTorrents(
      Transmission._getTorrentIds(torrents),
      () => {
        setTimeout(() => {
          this.refreshTorrents();
        }, 500);
      },
      this,
    );
  }
  changeFileCommand(torrentId, rowIndices, command) {
    this.remote.changeFileCommand(torrentId, rowIndices, command);
  }

  // Queue
  _moveTop() {
    this.remote.moveTorrentsToTop(
      this._getSelectedTorrentIds(),
      this.refreshTorrents,
      this,
    );
  }
  _moveUp() {
    this.remote.moveTorrentsUp(
      this._getSelectedTorrentIds(),
      this.refreshTorrents,
      this,
    );
  }
  _moveDown() {
    this.remote.moveTorrentsDown(
      this._getSelectedTorrentIds(),
      this.refreshTorrents,
      this,
    );
  }
  _moveBottom() {
    this.remote.moveTorrentsToBottom(
      this._getSelectedTorrentIds(),
      this.refreshTorrents,
      this,
    );
  }

  ///

  _updateGuiFromSession(o) {
    const [, version, checksum] = o.version.match(/(.*)\s\(([\da-f]+)\)/);
    this.version_info = {
      checksum,
      version,
    };

    const element = document.querySelector('#toolbar-overflow');
    element.classList.toggle('alt-speed-enabled', o[RPC._TurtleState]);
  }

  _updateStatusbar() {
    const fmt = Formatter;
    const torrents = this._getAllTorrents();

    const u = torrents.reduce(
      (accumulator, tor) => accumulator + tor.getUploadSpeed(),
      0,
    );
    const d = torrents.reduce(
      (accumulator, tor) => accumulator + tor.getDownloadSpeed(),
      0,
    );
    const string = fmt.countString('Transfer', 'Transfers', this._rows.length);

    setTextContent(document.querySelector('#speed-up-label'), fmt.speedBps(u));
    setTextContent(document.querySelector('#speed-dn-label'), fmt.speedBps(d));
    setTextContent(document.querySelector('#filter-count'), string);
  }

  static _displayName(hostname) {
    let name = hostname;
    if (name.length > 0) {
      name = name.charAt(0).toUpperCase() + name.slice(1);
    }
    return name;
  }

  _updateFilterSelect() {
    const trackers = this._getTrackerCounts();
    const sitenames = Object.keys(trackers).sort();

    // Update select box only when list of trackers has changed
    if (
      sitenames.length !== this.oldTrackers.length ||
      sitenames.some((ele, idx) => ele !== this.oldTrackers[idx])
    ) {
      this.oldTrackers = sitenames;

      const a = [
        ['All', Prefs.FilterAll, !this.filterTracker],
        ...sitenames.map((sitename) => [
          Transmission._displayName(sitename),
          sitename,
          sitename === this.filterTracker,
        ]),
      ];

      const e = document.querySelector('#filter-tracker');
      while (e.firstChild) {
        e.lastChild.remove();
      }
      newOpts(e, null, a);
    }
  }

  /// FILTER

  sortRows(rows) {
    const torrents = rows.map((row) => row.getTorrent());
    const id2row = rows.reduce((accumulator, row) => {
      accumulator[row.getTorrent().getId()] = row;
      return accumulator;
    }, {});
    Torrent.sortTorrents(
      torrents,
      this.prefs.sort_mode,
      this.prefs.sort_direction,
    );
    for (const [index, tor] of torrents.entries()) {
      rows[index] = id2row[tor.getId()];
    }
  }

  _refilter(rebuildEverything) {
    const { sort_mode, sort_direction, filter_mode } = this.prefs;
    const filter_tracker = this.filterTracker;
    const renderer = this.torrentRenderer;
    const list = this.elements.torrent_list;

    let filter_text = null;
    let labels = null;
    const m = /^labels:([\w,-\s]*)(.*)$/.exec(this.filterText);
    if (m) {
      filter_text = m[2].trim();
      labels = m[1].split(',');
    } else {
      filter_text = this.filterText;
      labels = [];
    }

    const countRows = () => [...list.children].length;
    const countSelectedRows = () =>
      [...list.children].reduce(
        (n, e) => (n + e.classList.contains('selected') ? 1 : 0),
        0,
      );
    const old_row_count = countRows();
    const old_sel_count = countSelectedRows();

    this._updateFilterSelect();

    if (rebuildEverything) {
      while (list.firstChild) {
        list.firstChild.remove();
      }
      this._rows = [];
      this.dirtyTorrents = new Set(Object.keys(this._torrents));
    }

    // rows that overlap with dirtyTorrents need to be refiltered.
    // those that don't are 'clean' and don't need refiltering.
    const clean_rows = [];
    let dirty_rows = [];
    for (const row of this._rows) {
      if (this.dirtyTorrents.has(row.getTorrentId())) {
        dirty_rows.push(row);
      } else {
        clean_rows.push(row);
      }
    }

    // remove the dirty rows from the dom
    for (const row of dirty_rows) {
      row.getElement().remove();
    }

    // drop any dirty rows that don't pass the filter test
    const temporary = [];
    for (const row of dirty_rows) {
      const id = row.getTorrentId();
      const t = this._torrents[id];
      if (t && t.test(filter_mode, filter_tracker, filter_text, labels)) {
        temporary.push(row);
      }
      this.dirtyTorrents.delete(id);
    }
    dirty_rows = temporary;

    // make new rows for dirty torrents that pass the filter test
    // but don't already have a row
    for (const id of this.dirtyTorrents.values()) {
      const t = this._torrents[id];
      if (t && t.test(filter_mode, filter_tracker, filter_text, labels)) {
        const row = new TorrentRow(renderer, this, t);
        const e = row.getElement();
        e.row = row;
        dirty_rows.push(row);
        e.addEventListener('click', this._onRowClicked.bind(this));
      }
    }

    // sort the dirty rows
    this.sortRows(dirty_rows);

    // now we have two sorted arrays of rows
    // and can do a simple two-way sorted merge.
    const rows = [];
    const cmax = clean_rows.length;
    const dmax = dirty_rows.length;
    const frag = document.createDocumentFragment();
    let ci = 0;
    let di = 0;
    while (ci !== cmax || di !== dmax) {
      let push_clean = null;
      if (ci === cmax) {
        push_clean = false;
      } else if (di === dmax) {
        push_clean = true;
      } else {
        const c = Torrent.compareTorrents(
          clean_rows[ci].getTorrent(),
          dirty_rows[di].getTorrent(),
          sort_mode,
          sort_direction,
        );
        push_clean = c < 0;
      }

      if (push_clean) {
        rows.push(clean_rows[ci++]);
      } else {
        const row = dirty_rows[di++];
        const e = row.getElement();

        if (ci === cmax) {
          frag.append(e);
        } else {
          list.insertBefore(e, clean_rows[ci].getElement());
        }

        rows.push(row);
      }
    }
    list.append(frag);

    // update our implementation fields
    this._rows = rows;
    this.dirtyTorrents.clear();

    this._updateStatusbar();
    if (
      old_sel_count !== countSelectedRows() ||
      old_row_count !== countRows()
    ) {
      this._dispatchSelectionChanged();
    }
  }

  setFilterTracker(sitename) {
    const e = document.querySelector('#filter-tracker');
    e.value = sitename;

    this.filterTracker = sitename === Prefs.FilterAll ? '' : sitename;
    this.refilterAllSoon();
  }

  _getTrackerCounts() {
    const counts = {};

    for (const torrent of this._getAllTorrents()) {
      for (const tracker of torrent.getTrackers()) {
        const { sitename } = tracker;
        counts[sitename] = (counts[sitename] || 0) + 1;
      }
    }

    return counts;
  }

  ///

  setCurrentPopup(popup, level = Transmission.default_popup_level) {
    for (let index = level; index < Transmission.max_popups; index++) {
      if (this.popup[index]) {
        this.popup[index].close();
      }
    }

    this.popup[level] = popup;

    if (this.popup[level]) {
      const listener = () => {
        if (this.popup[level]) {
          this.popup[level].removeEventListener('close', listener);
          this.popup[level] = null;
        }
      };
      this.popup[level].addEventListener('close', listener);
    }
  }
}
