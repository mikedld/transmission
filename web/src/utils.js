/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import isEqual from 'lodash.isequal';

export const Utils = {
  /** Given a numerator and denominator, return a ratio string */
  ratio(numerator, denominator) {
    let result = Math.floor((100 * numerator) / denominator) / 100;

    // check for special cases
    if (
      result === Number.POSITIVE_INFINITY ||
      result === Number.NEGATIVE_INFINITY
    ) {
      result = -2;
    } else if (Number.isNaN(result)) {
      result = -1;
    }

    return result;
  },
};

function toggleClass(buttons, button, pages, page, callback) {
  for (const element of buttons.children) {
    element.classList.toggle('selected', element === button);
  }
  for (const element of pages.children) {
    element.classList.toggle('hidden', element !== page);
  }
  if (callback) {
    callback(page);
  }
}

export function createTextualTabsContainer(id, tabs, callback) {
  const root = document.createElement('div');
  root.id = id;
  root.classList.add('tabs-container');

  const buttons = document.createElement('div');
  buttons.classList.add('tabs-buttons');
  root.append(buttons);

  const dismiss = document.createElement('button');
  dismiss.classList.add('tabs-container-close');
  dismiss.innerHTML = '&times;';
  root.append(dismiss);

  const pages = document.createElement('div');
  pages.classList.add('tabs-pages');
  root.append(pages);

  const button_array = [];
  for (const [button_id, page, tabname] of tabs) {
    const button = document.createElement('button');
    button.id = button_id;
    button.classList.add('tabs-button');
    button.setAttribute('type', 'button');
    button.textContent = tabname;
    buttons.append(button);
    button_array.push(button);

    page.classList.add('hidden', 'tabs-page');
    pages.append(page);

    button.addEventListener('click', () =>
      toggleClass(buttons, button, pages, page, callback),
    );
  }

  button_array[0].classList.add('selected');
  pages.children[0].classList.remove('hidden');

  return {
    buttons: button_array,
    dismiss,
    root,
  };
}

export function createDialogContainer(id) {
  const root = document.createElement('dialog');
  root.classList.add('dialog-container', 'popup', id);
  root.open = true;
  root.setAttribute('role', 'dialog');

  const win = document.createElement('div');
  win.classList.add('dialog-window');
  root.append(win);

  const logo = document.createElement('div');
  logo.classList.add('dialog-logo');
  win.append(logo);

  const heading = document.createElement('div');
  heading.classList.add('dialog-heading');
  win.append(heading);

  const message = document.createElement('div');
  message.classList.add('dialog-message');
  win.append(message);

  const workarea = document.createElement('div');
  workarea.classList.add('dialog-workarea');
  win.append(workarea);

  const buttons = document.createElement('div');
  buttons.classList.add('dialog-buttons');
  win.append(buttons);

  const bbegin = document.createElement('span');
  bbegin.className = 'flexible-space';
  buttons.append(bbegin);

  const dismiss = document.createElement('button');
  dismiss.classList.add('dialog-dismiss-button');
  dismiss.textContent = 'Cancel';
  buttons.append(dismiss);

  const confirm = document.createElement('button');
  confirm.textContent = 'OK';
  buttons.append(confirm);

  win.addEventListener('keyup', ({ key }) => {
    if (key === 'Enter') {
      confirm.click();
    }
  });

  return {
    confirm,
    dismiss,
    heading,
    message,
    root,
    workarea,
  };
}

export function makeUUID() {
  if (typeof crypto.randomUUID === 'function') {
    return crypto.randomUUID();
  }

  // source: https://stackoverflow.com/a/2117523/6568470
  return ([1e7] + -1e3 + -4e3 + -8e3 + -1e11).replaceAll(/[018]/g, (c) =>
    (
      c ^
      (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (c / 4)))
    ).toString(16),
  );
}

export function createSection(title) {
  const root = document.createElement('fieldset');
  root.classList.add('section');

  const legend = document.createElement('legend');
  legend.classList.add('title');
  legend.textContent = title;
  root.append(legend);

  const content = document.createElement('div');
  content.classList.add('content');
  root.append(content);

  return { content, root };
}

export function createInfoSection(title, labels) {
  const children = [];
  const { root, content } = createSection(title);

  for (const label_text of labels) {
    const label_element = document.createElement('label');
    label_element.textContent = label_text;
    content.append(label_element);

    const item = document.createElement('div');
    item.id = makeUUID();
    content.append(item);
    label_element.setAttribute('for', item.id);
    children.push(item);
  }

  return { children, root };
}

export function debounce(callback, wait = 100) {
  let timeout = null;
  return (...arguments_) => {
    if (!timeout) {
      timeout = setTimeout(() => {
        timeout = null;
        callback(...arguments_);
      }, wait);
    }
  };
}

export function deepEqual(a, b) {
  return isEqual(a, b);
}

function setOrDeleteAttribute(element, attribute, b) {
  if (b) {
    element.setAttribute(attribute, true);
  } else {
    element.removeAttribute(attribute);
  }
}
export function setEnabled(element, b) {
  setOrDeleteAttribute(element, 'disabled', !b);
}

export function addCheckedClass(element, b) {
  element.classList.toggle('checked', b);
}

export class OutsideClickListener extends EventTarget {
  constructor(element) {
    super();
    this.listener = (event_) => {
      if (!element.contains(event_.target)) {
        this.dispatchEvent(new MouseEvent(event_.type, event_));
        event_.preventDefault();
      }
    };
    Object.seal(this);
    this.start();
  }
  start() {
    setTimeout(() => document.addEventListener('click', this.listener), 0);
  }
  stop() {
    document.removeEventListener('click', this.listener);
  }
}

export function setTextContent(e, text) {
  if (e.textContent !== text) {
    e.textContent = text;
  }
}
