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

const icon32 = {
  fill: 'none',
  height: 32,
  stroke: 'currentColor',
  'stroke-linecap': 'round',
  'stroke-linejoin': 'round',
  'stroke-width': 2,
  viewBox: '0 0 24 24',
  width: 32,
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

function namespace(tagname, attrs) {
  const e = document.createElementNS('http://www.w3.org/2000/svg', tagname);

  for (const attr of Object.keys(attrs)) {
    e.setAttribute(attr, attrs[attr]);
  }

  return e;
}

function renderIcon(...svgca) {
  // SVG Command Array
  const svg = namespace('svg', icon32);
  for (const [tagname, attrs] of svgca) {
    svg.append(namespace(tagname, attrs));
  }
  return svg;
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
  bbegin.classList.add('dialog-buttons-begin');
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

  const bend = document.createElement('span');
  bend.classList.add('dialog-buttons-end');
  buttons.append(bend);

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

export const icon = Object.freeze({
  delete: () => {
    return renderIcon(
      ['polyline', { points: '3 6 5 6 21 6' }],
      [
        'path',
        {
          d: 'M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2',
        },
      ],
      ['line', { x1: 10, x2: 10, y1: 11, y2: 17 }],
      ['line', { x1: 14, x2: 14, y1: 11, y2: 17 }],
    );
  },
  inspector: () => {
    const svg = namespace('svg', {
      fill: 'none',
      'fill-opacity': 1,
      height: 26,
      stroke: 'currentColor',
      viewBox: '-1 -1 26 26',
      width: 26,
    });
    const g = namespace('g', {});
    g.append(
      namespace('circle', {
        cx: 12,
        cy: 12,
        r: 12,
        'stroke-linecap': 'round',
        'stroke-linejoin': 'round',
        'stroke-width': 2,
      }),
      namespace('path', {
        d: 'M 11.88208 4.883789 C 12.326418 4.883789 12.702391 5.039305 13.01001 5.350342 C 13.317628 5.6613785 13.471436 6.035642 13.471436 6.4731445 C 13.471436 6.910647 13.31592 7.283202 13.004883 7.59082 C 12.693846 7.898439 12.319582 8.052246 11.88208 8.052246 C 11.444578 8.052246 11.072023 7.898439 10.764404 7.59082 C 10.456786 7.283202 10.302979 6.910647 10.302979 6.4731445 C 10.302979 6.035642 10.456786 5.6613785 10.764404 5.350342 C 11.072023 5.039305 11.444578 4.883789 11.88208 4.883789 Z M 13.317627 9.528809 L 13.317627 17.126953 C 13.317627 17.803714 13.39624 18.236083 13.553467 18.424072 C 13.710694 18.612061 14.018308 18.719726 14.476318 18.74707 L 14.476318 19.11621 L 9.298096 19.11621 L 9.298096 18.74707 C 9.721926 18.733398 10.036376 18.610353 10.241455 18.37793 C 10.378175 18.220702 10.446533 17.803714 10.446533 17.126953 L 10.446533 11.52832 C 10.446533 10.851559 10.367921 10.41919 10.210693 10.231201 C 10.053466 10.043212 9.74927 9.935547 9.298096 9.908203 L 9.298096 9.528809 Z',
        fill: 'currentColor',
      }),
    );
    svg.append(g);
    return svg;
  },
  open: () => {
    return renderIcon(
      [
        'path',
        {
          d: 'M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z',
        },
      ],
      ['line', { x1: 12, x2: 12, y1: 11, y2: 17 }],
      ['line', { x1: 9, x2: 15, y1: 14, y2: 14 }],
    );
  },
  overflow: () => {
    return renderIcon(
      ['line', { x1: 3, x2: 21, y1: 12, y2: 12 }],
      ['line', { x1: 3, x2: 21, y1: 6, y2: 6 }],
      ['line', { x1: 3, x2: 21, y1: 18, y2: 18 }],
    );
  },
  pause: () => {
    return renderIcon(
      ['rect', { height: 16, width: 4, x: 6, y: 4 }],
      ['rect', { height: 16, width: 4, x: 14, y: 4 }],
    );
  },
  speedDown: () => {
    return renderIcon(['polyline', { points: '6 9 12 15 18 9' }]);
  },
  speedUp: () => {
    return renderIcon(['polyline', { points: '18 15 12 9 6 15' }]);
  },
  start: () => {
    return renderIcon(['polyline', { points: '5 3 19 12 5 21 5 3' }]);
  },
});

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
