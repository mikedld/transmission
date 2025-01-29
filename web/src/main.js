/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { ActionManager } from './action-manager.js';
import { Notifications } from './notifications.js';
import { Prefs } from './prefs.js';
import { Transmission } from './transmission.js';
import { debounce } from './utils.js';

import '../assets/css/transmission-app.scss';

function main() {
  const action_manager = new ActionManager();
  const prefs = new Prefs();
  const notifications = new Notifications(prefs);
  const transmission = new Transmission(action_manager, notifications, prefs);

  const scroll_soon = debounce(() =>
    transmission.elements.torrent_list.scrollTo(0, 1),
  );
  globalThis.addEventListener('load', scroll_soon);
  globalThis.addEventListener('orientationchange', scroll_soon);
}

document.addEventListener('DOMContentLoaded', main);
