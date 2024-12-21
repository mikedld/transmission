/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { Prefs } from './prefs.js';
import { createDialogContainer, makeUUID } from './utils.js';

export class Appearance extends EventTarget {
  constructor(prefs, action_manager) {
    super();

    this.action_manager = action_manager;

    this.prefs_listener = this._onPrefsChange.bind(this);
    this.prefs = prefs;
    this.prefs.addEventListener('change', this.prefs_listener);

    this.elements = this._create();
    this.elements.dismiss.addEventListener('click', () => this.close());
    document.body.append(this.elements.root);
    this.elements.dismiss.focus();
  }

  close() {
    this.elements.root.remove();
    this.dispatchEvent(new Event('close'));
    delete this.elements;
  }

  _onPrefsChange(event_) {
    switch (event_.key) {
      case Prefs.SortDirection:
      case Prefs.SortMode:
        this.root.querySelector(`[data-pref="${event_.key}"]`).value =
          event_.value;
        break;
      default:
        break;
    }
  }

  _create() {
    const elements = createDialogContainer('dis-appearance');
    const { dismiss, heading, message } = elements;

    heading.textContent = 'Appearance';
    dismiss.textContent = 'Close';

    // contrast

    let legend = document.createElement('h4');
    message.append(legend);
    legend.textContent = 'Theme';

    let div = document.createElement('div');
    div.classList.add('table-row');
    message.append(div);

    const add_check = (text, listener) => {
      const check = document.createElement('input');
      check.id = makeUUID();
      check.type = 'checkbox';

      const label = document.createElement('label');
      label.htmlFor = check.id;
      label.textContent = text;

      div.append(check, label);
      listener(check);
    };

    const add_radio = (name, text, className, style, listener) => {
      const input = document.createElement('input');
      input.id = makeUUID();
      input.name = name;
      input.type = 'radio';
      input.value = style;

      const label = document.createElement('label');
      label.htmlFor = input.id;
      label.textContent = text;

      div.append(input, label, document.createElement('BR'));

      listener(input, className);
    };

    let listener = (e) => {
      e.checked = this.prefs.contrast_mode === Prefs.ContrastMore;
      e.addEventListener('change', (event_) => {
        const { checked } = event_.target;
        this.prefs.contrast_mode = checked
          ? Prefs.ContrastMore
          : Prefs.ContrastLess;
      });
    };

    add_check(this.action_manager.text('toggle-contrast'), listener);

    // highlight color

    legend = document.createElement('h4');
    message.append(legend);
    legend.textContent = 'Highlight color';

    div = document.createElement('div');
    div.classList.add('table-row');
    message.append(div);

    listener = (e, className) => {
      e.checked = !className || document.body.classList.contains(className);
      e.addEventListener('change', (event_) => {
        const { value } = event_.target;
        this.prefs.highlight_color = value;
      });
    };

    add_radio(
      'highlight-color',
      'Accent color from system',
      null,
      'AccentColor',
      listener,
    );
    add_radio(
      'highlight-color',
      'Highlight color from system',
      'highlight-system',
      'Highlight',
      listener,
    );
    add_radio('highlight-color', 'Legacy', 'highlight-legacy', null, listener);

    elements.confirm.remove();
    delete elements.confirm;

    return elements;
  }
}
