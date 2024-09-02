/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { Formatter } from './formatter.js';
import {
  OutsideClickListener,
  createTextualTabsContainer,
  makeUUID,
  setEnabled,
  setTextContent,
} from './utils.js';

export class PrefsDialog extends EventTarget {
  static _initTimeDropDown(e) {
    for (let index = 0; index < 24 * 4; ++index) {
      const hour = index / 4;
      const mins = (index % 4) * 15;
      const value = index * 15;
      const content = `${hour}:${mins || '00'}`;
      e.options[index] = new Option(content, value);
    }
  }

  static _initDayDropDown(e) {
    const options = [
      ['Everyday', '127'],
      ['Weekdays', '62'],
      ['Weekends', '65'],
      ['Sunday', '1'],
      ['Monday', '2'],
      ['Tuesday', '4'],
      ['Wednesday', '8'],
      ['Thursday', '16'],
      ['Friday', '32'],
      ['Saturday', '64'],
    ];
    for (let index = 0; options[index]; ++index) {
      const [text, value] = options[index];
      e.options[index] = new Option(text, value);
    }
  }

  _checkPort() {
    for (const [key, element] of Object.entries(
      this.elements.network.port_status_label,
    )) {
      delete element.dataset.open;
      setTextContent(element, 'Checking...');
      this.remote.checkPort(
        key,
        (response) => this._onPortChecked(key, response),
        this,
      );
    }
  }

  _onPortChecked(ipProtocol, response) {
    if (this.closed) {
      return;
    }

    const element = this.elements.network.port_status_label[ipProtocol];
    const is_open = response.arguments['port-is-open'] || false;
    element.dataset.open = is_open;
    if ('port-is-open' in response.arguments) {
      setTextContent(element, is_open ? 'Open' : 'Closed');
    } else {
      setTextContent(element, 'Error');
    }
  }

  _setBlocklistButtonEnabled(b) {
    const e = this.elements.peers.blocklist_update_button;
    setEnabled(e, b);
    e.value = b ? 'Update' : 'Updating...';
  }

  static _getValue(e) {
    if (e.tagName === 'TEXTAREA') {
      return e.value;
    }

    switch (e.type) {
      case 'checkbox':
      case 'radio':
        return e.checked;

      case 'number':
      case 'select-one':
      case 'text':
      case 'url': {
        const string = e.value;
        if (Number.parseInt(string, 10).toString() === string) {
          return Number.parseInt(string, 10);
        }
        if (Number.parseFloat(string).toString() === string) {
          return Number.parseFloat(string);
        }
        return string;
      }

      default:
        return null;
    }
  }

  _onMaybePortChanged(key) {
    if (key === 'peer-port' || key === 'port-forwarding-enabled') {
      this._checkPort();
    }
  }

  // this callback is for controls whose changes can be applied
  // immediately, like checkboxs, radioboxes, and selects
  _onControlChanged(event_) {
    const { key } = event_.target.dataset;
    this.remote.savePrefs({
      [key]: PrefsDialog._getValue(event_.target),
    });
    this._onMaybePortChanged(key);
  }

  _onDialogClosed() {
    this.dispatchEvent(new Event('closed'));
  }

  // update the dialog's controls
  _update() {
    this._setBlocklistButtonEnabled(true);

    for (const [key, value] of Object.entries(
      this.session_manager.session_properties,
    )) {
      for (const element of this.elements.root.querySelectorAll(
        `[data-key="${key}"]`,
      )) {
        if (key === 'blocklist_size') {
          const n = Formatter.number(value);
          element.innerHTML = `Blocklist has <span class="blocklist-size-number">${n}</span> rules`;
          setTextContent(this.elements.peers.blocklist_update_button, 'Update');
        } else {
          switch (element.type) {
            case 'checkbox':
            case 'radio':
              element.checked = value;
              break;
            case 'text':
            case 'textarea':
            case 'url':
            case 'email':
            case 'number':
            case 'search':
              // don't change the text if the user's editing it.
              // it's very annoying when that happens!
              if (element !== document.activeElement) {
                // eslint-disable-next-line eqeqeq
                if (element.value != value) {
                  this._onMaybePortChanged(key);
                }
                element.value = value;
              }
              break;
            case 'select-one':
              element.value = value;
              break;
            default:
              console.log(element.type);
              break;
          }
        }
      }
    }
  }

  shouldAddedTorrentsStart() {
    return this.data.elements.root.find('#start-added-torrents')[0].checked;
  }

  static _createCheckAndLabel(id, text) {
    const root = document.createElement('div');
    root.id = id;

    const check = document.createElement('input');
    check.id = makeUUID();
    check.type = 'checkbox';
    root.append(check);

    const label = document.createElement('label');
    label.textContent = text;
    label.setAttribute('for', check.id);
    root.append(label);

    return { check, label, root };
  }

  static _enableIfChecked(element, check) {
    const callback = () => {
      if (element.tagName === 'INPUT') {
        setEnabled(element, check.checked);
      } else {
        element.classList.toggle('disabled', !check.checked);
      }
    };
    check.addEventListener('change', callback);
    callback();
  }

  static _getProtocolHandlerRegistered() {
    return localStorage.getItem('protocol-handler-registered') === 'true';
  }

  static _updateProtocolHandlerButton(button) {
    button.removeAttribute('disabled');
    button.removeAttribute('title');

    if (PrefsDialog._getProtocolHandlerRegistered()) {
      button.textContent = 'Remove Browser Handler';
      if (!('unregisterProtocolHandler' in navigator)) {
        button.setAttribute(
          'title',
          'Your browser does not support removing protocol handlers. This button only allows you to re-register a handler.',
        );
      }
    } else {
      button.textContent = 'Add Browser Handler';
      button.removeAttribute('title');
      if (!('registerProtocolHandler' in navigator)) {
        button.setAttribute('disabled', true);
        button.setAttribute(
          'title',
          'Your browser does not support protocol handlers',
        );
      }
    }
  }

  static _toggleProtocolHandler(button) {
    const handlerUrl = new URL(window.location.href);
    handlerUrl.search = 'addtorrent=%s';
    if (this._getProtocolHandlerRegistered()) {
      navigator.unregisterProtocolHandler?.('magnet', handlerUrl.toString());
      localStorage.removeItem('protocol-handler-registered');
      PrefsDialog._updateProtocolHandlerButton(button);
    } else {
      navigator.registerProtocolHandler(
        'magnet',
        handlerUrl.toString(),
        'Transmission Web',
      );
      localStorage.setItem('protocol-handler-registered', 'true');
      PrefsDialog._updateProtocolHandlerButton(button);
    }
  }

  static _createTorrentsPage() {
    const root = document.createElement('div');
    root.classList.add('prefs-torrents-page');

    let label = document.createElement('div');
    label.textContent = 'Downloading';
    label.classList.add('section-label');
    root.append(label);

    label = document.createElement('label');
    label.textContent = 'Download to:';
    root.append(label);

    let input = document.createElement('input');
    input.type = 'text';
    input.id = makeUUID();
    input.dataset.key = 'download_dir';
    label.setAttribute('for', input.id);
    root.append(input);
    const download_dir = input;

    let cal = PrefsDialog._createCheckAndLabel(
      'incomplete-dir-div',
      'Use temporary folder:',
    );
    cal.check.title =
      'Separate folder to temporarily store downloads until they are complete.';
    cal.check.dataset.key = 'incomplete-dir-enabled';
    cal.label.title = cal.check.title;
    root.append(cal.root);
    const incomplete_dir_check = cal.check;

    input = document.createElement('input');
    input.type = 'text';
    input.dataset.key = 'incomplete-dir';
    root.append(input);
    PrefsDialog._enableIfChecked(input, cal.check);
    const incomplete_dir_input = input;

    cal = PrefsDialog._createCheckAndLabel('autostart-div', 'Start when added');
    cal.check.dataset.key = 'start-added-torrents';
    root.append(cal.root);
    const autostart_check = cal.check;

    cal = PrefsDialog._createCheckAndLabel(
      'suffix-div',
      `Append "part" to incomplete files' names`,
    );
    cal.check.dataset.key = 'rename-partial-files';
    root.append(cal.root);
    const suffix_check = cal.check;

    cal = PrefsDialog._createCheckAndLabel(
      'download-queue-div',
      'Download queue size:',
    );
    cal.check.dataset.key = 'download_queue_enabled';
    root.append(cal.root);
    const download_queue_check = cal.check;

    input = document.createElement('input');
    input.type = 'number';
    input.dataset.key = 'download_queue_size';
    root.append(input);
    PrefsDialog._enableIfChecked(input, cal.check);
    const download_queue_input = input;

    label = document.createElement('div');
    label.textContent = 'Seeding';
    label.classList.add('section-label');
    root.append(label);

    cal = PrefsDialog._createCheckAndLabel(
      'stop-ratio-div',
      'Stop seeding at ratio:',
    );
    cal.check.dataset.key = 'seedRatioLimited';
    root.append(cal.root);
    const stop_ratio_check = cal.check;

    input = document.createElement('input');
    input.type = 'number';
    input.min = '0.1';
    input.step = 'any';
    input.dataset.key = 'seed_ratio_limit';
    root.append(input);
    PrefsDialog._enableIfChecked(input, cal.check);
    const stop_ratio_input = input;

    cal = PrefsDialog._createCheckAndLabel(
      'stop-idle-div',
      'Stop seeding if idle for N mins:',
    );
    cal.check.dataset.key = 'idle-seeding-limit-enabled';
    root.append(cal.root);
    const stop_idle_check = cal.check;

    input = document.createElement('input');
    input.type = 'number';
    input.min = '0.1';
    input.step = 'any';
    input.dataset.key = 'idle-seeding-limit';
    root.append(input);
    PrefsDialog._enableIfChecked(input, cal.check);
    const stop_idle_input = input;

    label = document.createElement('div');
    label.textContent = 'Magnet Protocol Handler';
    label.classList.add('section-label');
    root.append(label);

    const button = document.createElement('button');
    button.classList.add('register-handler-button');
    PrefsDialog._updateProtocolHandlerButton(button);
    root.append(button);
    const register_handler_button = button;

    return {
      autostart_check,
      download_dir,
      download_queue_check,
      download_queue_input,
      incomplete_dir_check,
      incomplete_dir_input,
      register_handler_button,
      root,
      stop_idle_check,
      stop_idle_input,
      stop_ratio_check,
      stop_ratio_input,
      suffix_check,
    };
  }

  static _createSpeedPage() {
    const root = document.createElement('div');
    root.classList.add('prefs-speed-page');

    let label = document.createElement('div');
    label.textContent = 'Speed Limits';
    label.classList.add('section-label');
    root.append(label);

    let cal = PrefsDialog._createCheckAndLabel(
      'upload-speed-div',
      'Upload (kB/s):',
    );
    cal.check.dataset.key = 'speed-limit-up-enabled';
    root.append(cal.root);
    const upload_speed_check = cal.check;

    let input = document.createElement('input');
    input.type = 'number';
    input.dataset.key = 'speed-limit-up';
    root.append(input);
    PrefsDialog._enableIfChecked(input, cal.check);
    const upload_speed_input = input;

    cal = PrefsDialog._createCheckAndLabel(
      'download-speed-div',
      'Download (kB/s):',
    );
    cal.check.dataset.key = 'speed-limit-down-enabled';
    root.append(cal.root);
    const download_speed_check = cal.check;

    input = document.createElement('input');
    input.type = 'number';
    input.dataset.key = 'speed-limit-down';
    root.append(input);
    PrefsDialog._enableIfChecked(input, cal.check);
    const download_speed_input = input;

    label = document.createElement('div');
    label.textContent = 'Alternative Speed Limits';
    label.classList.add('section-label', 'alt-speed-section-label');
    root.append(label);

    label = document.createElement('div');
    label.textContent =
      'Override normal speed limits manually or at scheduled times';
    label.classList.add('alt-speed-label');
    root.append(label);

    label = document.createElement('label');
    label.textContent = 'Upload (kB/s):';
    root.append(label);

    input = document.createElement('input');
    input.type = 'number';
    input.dataset.key = 'alt_speed_up';
    input.id = makeUUID();
    label.setAttribute('for', input.id);
    root.append(input);
    const alt_upload_speed_input = input;

    label = document.createElement('label');
    label.textContent = 'Download (kB/s):';
    root.append(label);

    input = document.createElement('input');
    input.type = 'number';
    input.dataset.key = 'alt_speed_down';
    input.id = makeUUID();
    label.setAttribute('for', input.id);
    root.append(input);
    const alt_download_speed_input = input;

    cal = PrefsDialog._createCheckAndLabel('alt-times-div', 'Scheduled times');
    cal.check.dataset.key = 'alt_speed_time_enabled';
    root.append(cal.root);
    const alt_times_check = cal.check;

    label = document.createElement('label');
    label.textContent = 'From:';
    PrefsDialog._enableIfChecked(label, cal.check);
    root.append(label);

    let select = document.createElement('select');
    select.id = makeUUID();
    select.dataset.key = 'alt_speed_time_begin';
    PrefsDialog._initTimeDropDown(select);
    label.setAttribute('for', select.id);
    root.append(select);
    PrefsDialog._enableIfChecked(select, cal.check);
    const alt_from_select = select;

    label = document.createElement('label');
    label.textContent = 'To:';
    PrefsDialog._enableIfChecked(label, cal.check);
    root.append(label);

    select = document.createElement('select');
    select.id = makeUUID();
    select.dataset.key = 'alt_speed_time_end';
    PrefsDialog._initTimeDropDown(select);
    label.setAttribute('for', select.id);
    root.append(select);
    PrefsDialog._enableIfChecked(select, cal.check);
    const alt_to_select = select;

    label = document.createElement('label');
    label.textContent = 'On days:';
    PrefsDialog._enableIfChecked(label, cal.check);
    root.append(label);

    select = document.createElement('select');
    select.id = makeUUID();
    select.dataset.key = 'alt_speed_time_day';
    PrefsDialog._initDayDropDown(select);
    label.setAttribute('for', select.id);
    root.append(select);
    PrefsDialog._enableIfChecked(select, cal.check);
    const alt_days_select = select;

    return {
      alt_days_select,
      alt_download_speed_input,
      alt_from_select,
      alt_times_check,
      alt_to_select,
      alt_upload_speed_input,
      download_speed_check,
      download_speed_input,
      root,
      upload_speed_check,
      upload_speed_input,
    };
  }

  static _createPeersPage() {
    const root = document.createElement('div');
    root.classList.add('prefs-peers-page');

    let label = document.createElement('div');
    label.textContent = 'Connections';
    label.classList.add('section-label');
    root.append(label);

    label = document.createElement('label');
    label.textContent = 'Max peers per torrent:';
    root.append(label);

    let input = document.createElement('input');
    input.type = 'number';
    input.dataset.key = 'peer-limit-per-torrent';
    input.id = makeUUID();
    label.setAttribute('for', input.id);
    root.append(input);
    const max_peers_per_torrent_input = input;

    label = document.createElement('label');
    label.textContent = 'Max peers overall:';
    root.append(label);

    input = document.createElement('input');
    input.type = 'number';
    input.dataset.key = 'peer-limit-global';
    input.id = makeUUID();
    label.setAttribute('for', input.id);
    root.append(input);
    const max_peers_overall_input = input;

    label = document.createElement('div');
    label.textContent = 'Options';
    label.classList.add('section-label');
    root.append(label);

    label = document.createElement('label');
    label.textContent = 'Encryption mode:';
    root.append(label);

    const select = document.createElement('select');
    select.id = makeUUID();
    select.dataset.key = 'encryption';
    select.options[0] = new Option('Prefer encryption', 'preferred');
    select.options[1] = new Option('Allow encryption', 'tolerated');
    select.options[2] = new Option('Require encryption', 'required');
    root.append(select);
    const encryption_select = select;

    let cal = PrefsDialog._createCheckAndLabel(
      'use-pex-div',
      'Use PEX to find more peers',
    );
    cal.check.title =
      "PEX is a tool for exchanging peer lists with the peers you're connected to.";
    cal.check.dataset.key = 'pex-enabled';
    cal.label.title = cal.check.title;
    root.append(cal.root);
    const pex_check = cal.check;

    cal = PrefsDialog._createCheckAndLabel(
      'use-dht-div',
      'Use DHT to find more peers',
    );
    cal.check.title = 'DHT is a tool for finding peers without a tracker.';
    cal.check.dataset.key = 'dht_enabled';
    cal.label.title = cal.check.title;
    root.append(cal.root);
    const dht_check = cal.check;

    cal = PrefsDialog._createCheckAndLabel(
      'use-lpd-div',
      'Use LPD to find more peers',
    );
    cal.check.title = 'LPD is a tool for finding peers on your local network.';
    cal.check.dataset.key = 'lpd-enabled';
    cal.label.title = cal.check.title;
    root.append(cal.root);
    const lpd_check = cal.check;

    label = document.createElement('div');
    label.textContent = 'Blocklist';
    label.classList.add('section-label');
    root.append(label);

    cal = PrefsDialog._createCheckAndLabel(
      'blocklist-enabled-div',
      'Enable blocklist:',
    );
    cal.check.dataset.key = 'blocklist_enabled';
    root.append(cal.root);
    const blocklist_enabled_check = cal.check;

    input = document.createElement('input');
    input.type = 'url';
    input.value = 'http://www.example.com/blocklist';
    input.dataset.key = 'blocklist_url';
    root.append(input);
    PrefsDialog._enableIfChecked(input, cal.check);
    const blocklist_url_input = input;

    label = document.createElement('label');
    label.textContent = 'Blocklist has {n} rules';
    label.dataset.key = 'blocklist_size';
    label.classList.add('blocklist-size-label');
    PrefsDialog._enableIfChecked(label, cal.check);
    root.append(label);

    const button = document.createElement('button');
    button.classList.add('blocklist-update-button');
    button.textContent = 'Update';
    root.append(button);
    PrefsDialog._enableIfChecked(button, cal.check);
    const blocklist_update_button = button;

    return {
      blocklist_enabled_check,
      blocklist_update_button,
      blocklist_url_input,
      dht_check,
      encryption_select,
      lpd_check,
      max_peers_overall_input,
      max_peers_per_torrent_input,
      pex_check,
      root,
    };
  }

  static _createNetworkPage() {
    const root = document.createElement('div');
    root.classList.add('prefs-network-page');

    let label = document.createElement('div');
    label.textContent = 'Listening Port';
    label.classList.add('section-label');
    root.append(label);

    label = document.createElement('label');
    label.textContent = 'Peer listening port:';
    root.append(label);

    const input = document.createElement('input');
    input.type = 'number';
    input.dataset.key = 'peer-port';
    input.id = makeUUID();
    label.setAttribute('for', input.id);
    root.append(input);
    const port_input = input;

    const port_status_div = document.createElement('div');
    port_status_div.classList.add('port-status');
    label = document.createElement('label');
    label.textContent = 'IPv4 port is';
    port_status_div.append(label);
    const port_status_label_ipv4 = document.createElement('label');
    port_status_label_ipv4.textContent = '?';
    port_status_label_ipv4.classList.add('port-status-label');
    port_status_div.append(port_status_label_ipv4);
    port_status_div.append(document.createElement('br'));
    label = document.createElement('label');
    label.textContent = 'IPv6 port is';
    port_status_div.append(label);
    const port_status_label_ipv6 = document.createElement('label');
    port_status_label_ipv6.textContent = '?';
    port_status_label_ipv6.classList.add('port-status-label');
    port_status_div.append(port_status_label_ipv6);
    root.append(port_status_div);

    let cal = PrefsDialog._createCheckAndLabel(
      'randomize-port',
      'Randomize port on launch',
    );
    cal.check.dataset.key = 'peer-port-random-on-start';
    root.append(cal.root);
    const random_port_check = cal.check;

    cal = PrefsDialog._createCheckAndLabel(
      'port-forwarding',
      'Use port forwarding from my router',
    );
    cal.check.dataset.key = 'port-forwarding-enabled';
    root.append(cal.root);
    const port_forwarding_check = cal.check;

    label = document.createElement('div');
    label.textContent = 'Options';
    label.classList.add('section-label');
    root.append(label);

    cal = PrefsDialog._createCheckAndLabel(
      'utp-enabled',
      'Enable uTP for peer communication',
    );
    cal.check.dataset.key = 'utp-enabled';
    root.append(cal.root);
    const utp_check = cal.check;

    label = document.createElement('div');
    label.textContent = 'Default Public Trackers';
    label.classList.add('section-label');
    root.append(label);

    const tracker_labels = [
      'Trackers to use on all public torrents.',
      'To add a backup URL, add it on the next line after a primary URL.',
      'To add a new primary URL, add it after a blank line.',
    ];
    for (const text of tracker_labels) {
      label = document.createElement('label');
      label.classList.add('default-trackers-label');
      label.textContent = text;
      label.setAttribute('for', 'default-trackers');
      root.append(label);
    }

    const textarea = document.createElement('textarea');
    textarea.dataset.key = 'default_trackers';
    textarea.id = 'default-trackers';
    root.append(textarea);
    const default_trackers_textarea = textarea;

    return {
      default_trackers_textarea,
      port_forwarding_check,
      port_input,
      port_status_label: {
        ipv4: port_status_label_ipv4,
        ipv6: port_status_label_ipv6,
      },
      random_port_check,
      root,
      utp_check,
    };
  }

  static _create() {
    const pages = {
      network: PrefsDialog._createNetworkPage(),
      peers: PrefsDialog._createPeersPage(),
      speed: PrefsDialog._createSpeedPage(),
      torrents: PrefsDialog._createTorrentsPage(),
    };

    const elements = createTextualTabsContainer('prefs-dialog', [
      ['prefs-tab-torrent', pages.torrents.root, 'Torrents'],
      ['prefs-tab-speed', pages.speed.root, 'Speed'],
      ['prefs-tab-peers', pages.peers.root, 'Peers'],
      ['prefs-tab-network', pages.network.root, 'Network'],
    ]);

    return { ...elements, ...pages };
  }

  constructor(session_manager, remote) {
    super();

    this.closed = false;
    this.session_manager = session_manager;
    this.remote = remote;
    this.update_from_session = () => this._update();

    this.elements = PrefsDialog._create();
    this.elements.peers.blocklist_update_button.addEventListener(
      'click',
      (event_) => {
        setTextContent(event_.target, 'Updating blocklist...');
        this.remote.updateBlocklist();
        this._setBlocklistButtonEnabled(false);
      },
    );
    this.elements.torrents.register_handler_button.addEventListener(
      'click',
      (event_) => {
        PrefsDialog._toggleProtocolHandler(event_.currentTarget);
      },
    );
    this.elements.dismiss.addEventListener('click', () => this.close());
    this.outside = new OutsideClickListener(this.elements.root);
    this.outside.addEventListener('click', () => this.close());

    Object.seal(this);

    // listen for user input
    const on_change = this._onControlChanged.bind(this);
    const walk = (o) => {
      for (const element of Object.values(o)) {
        if (element.tagName === 'INPUT') {
          switch (element.type) {
            case 'checkbox':
            case 'radio':
            case 'number':
            case 'text':
            case 'url':
              element.addEventListener('change', on_change);
              break;
            default:
              console.trace(`unhandled input: ${element.type}`);
              break;
          }
        } else if (
          element.tagName === 'TEXTAREA' ||
          element.tagName === 'SELECT'
        ) {
          element.addEventListener('change', on_change);
        }
      }
    };
    walk(this.elements.network);
    walk(this.elements.peers);
    walk(this.elements.speed);
    walk(this.elements.torrents);

    this.session_manager.addEventListener(
      'session-change',
      this.update_from_session,
    );
    this.update_from_session();

    document.body.append(this.elements.root);
  }

  close() {
    if (!this.closed) {
      this.outside.stop();
      this.session_manager.removeEventListener(
        'session-change',
        this.update_from_session,
      );
      this.elements.root.remove();
      dispatchEvent(new Event('close'));
      for (const key of Object.keys(this)) {
        this[key] = null;
      }
      this.closed = true;
    }
  }
}
