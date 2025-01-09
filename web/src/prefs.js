/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { debounce } from './utils.js';

export class Prefs extends EventTarget {
  constructor() {
    super();

    this._cache = {};

    this.dispatchPrefsChange = debounce((key, old_value, value) => {
      const event = new Event('change');
      Object.assign(event, { key, old_value, value });
      this.dispatchEvent(event);
    });

    for (const [key, default_value] of Object.entries(Prefs._Defaults)) {
      // populate the cache...
      this._set(key, Prefs._getCookie(key, default_value));

      // add property getter/setters...
      Object.defineProperty(this, key.replaceAll('-', '_'), {
        get: () => this._get(key),
        set: (value) => {
          this._set(key, value);
        },
      });
    }

    Object.seal(this);
  }

  entries() {
    return Object.entries(this._cache);
  }

  keys() {
    return Object.keys(this._cache);
  }

  _get(key) {
    const { _cache } = this;
    if (!Object.prototype.hasOwnProperty.call(_cache, key)) {
      throw new Error(key);
    }
    return _cache[key];
  }

  _set(key, value) {
    const { _cache } = this;
    const old_value = _cache[key];
    if (old_value !== value) {
      _cache[key] = value;
      Prefs._setCookie(key, value);
      this.dispatchPrefsChange(key, old_value, value);
    }
  }

  static _setCookie(key, value) {
    const date = new Date();
    date.setFullYear(date.getFullYear() + 1);
    // eslint-disable-next-line unicorn/no-document-cookie
    document.cookie = `${key}=${value}; SameSite=Strict; expires=${date.toGMTString()}`;
  }

  static _getCookie(key, fallback) {
    const value = Prefs._readCookie(key);
    if (value === null) {
      return fallback;
    }

    const type = typeof fallback;
    if (type === 'boolean') {
      if (value === 'true') {
        return true;
      }
      if (value === 'false') {
        return false;
      }
      return fallback;
    }
    if (type === 'number') {
      const f = Number.parseFloat(value);
      return Number.isNaN(f) ? fallback : f;
    }
    return value;
  }

  static _readCookie(key) {
    const value = `; ${document.cookie}`;
    const parts = value.split(`; ${key}=`);
    return parts.length === 2 ? parts.pop().split(';').shift() : null;
  }
}

Prefs.AltSpeedEnabled = 'alt-speed-enabled';
Prefs.DisplayCompact = 'compact';
Prefs.DisplayFull = 'full';
Prefs.DisplayMode = 'display-mode';
Prefs.ContrastLess = 'less';
Prefs.ContrastMore = 'more';
Prefs.ContrastMode = 'contrast-mode';
Prefs.FilterActive = 'active';
Prefs.FilterAll = 'all';
Prefs.FilterDownloading = 'downloading';
Prefs.FilterFinished = 'finished';
Prefs.FilterMode = 'filter-mode';
Prefs.FilterPaused = 'paused';
Prefs.FilterSeeding = 'seeding';
Prefs.HighlightColor = 'highlight-color';
Prefs.NotificationsEnabled = 'notifications-enabled';
Prefs.RefreshRate = 'refresh-rate-sec';
Prefs.SortAscending = 'ascending';
Prefs.SortByActivity = 'activity';
Prefs.SortByAge = 'age';
Prefs.SortByName = 'name';
Prefs.SortByProgress = 'progress';
Prefs.SortByQueue = 'queue';
Prefs.SortByRatio = 'ratio';
Prefs.SortBySize = 'size';
Prefs.SortByState = 'state';
Prefs.SortDescending = 'descending';
Prefs.SortDirection = 'sort-direction';
Prefs.SortMode = 'sort-mode';

Prefs._Defaults = {
  [Prefs.AltSpeedEnabled]: false,
  [Prefs.DisplayMode]: Prefs.DisplayFull,
  [Prefs.ContrastMode]: window.matchMedia('(prefers-contrast: more)').matches
    ? Prefs.ContrastMore
    : Prefs.ContrastLess,
  [Prefs.FilterMode]: Prefs.FilterAll,
  [Prefs.HighlightColor]: 'AccentColor',
  [Prefs.NotificationsEnabled]: false,
  [Prefs.RefreshRate]: 5,
  [Prefs.SortDirection]: Prefs.SortAscending,
  [Prefs.SortMode]: Prefs.SortByName,
};
