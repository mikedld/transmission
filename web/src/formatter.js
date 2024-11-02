/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

const plural_rules = new Intl.PluralRules();
const current_locale = plural_rules.resolvedOptions().locale;
const number_format = new Intl.NumberFormat(current_locale);

const kilo = 1000;
const mem_formatters = [
  new Intl.NumberFormat(current_locale, {
    maximumFractionDigits: 0,
    style: 'unit',
    unit: 'byte',
  }),
  new Intl.NumberFormat(current_locale, {
    maximumFractionDigits: 0,
    style: 'unit',
    unit: 'kilobyte',
  }),
  new Intl.NumberFormat(current_locale, {
    maximumFractionDigits: 0,
    style: 'unit',
    unit: 'megabyte',
  }),
  new Intl.NumberFormat(current_locale, {
    maximumFractionDigits: 2,
    style: 'unit',
    unit: 'gigabyte',
  }),
  new Intl.NumberFormat(current_locale, {
    maximumFractionDigits: 2,
    style: 'unit',
    unit: 'terabyte',
  }),
  new Intl.NumberFormat(current_locale, {
    maximumFractionDigits: 2,
    style: 'unit',
    unit: 'petabyte',
  }),
];

const fmt_kBps = new Intl.NumberFormat(current_locale, {
  maximumFractionDigits: 2,
  style: 'unit',
  unit: 'kilobyte-per-second',
});
const fmt_MBps = new Intl.NumberFormat(current_locale, {
  maximumFractionDigits: 2,
  style: 'unit',
  unit: 'megabyte-per-second',
});

export const Formatter = {
  /** Round a string of a number to a specified number of decimal places */
  _toTruncFixed(number, places) {
    const returnValue = Math.floor(number * 10 ** places) / 10 ** places;
    return returnValue.toFixed(places);
  },

  countString(msgid, msgid_plural, n) {
    return `${this.number(n)} ${this.ngettext(msgid, msgid_plural, n)}`;
  },

  // Formats a memory size into a human-readable string
  // @param {Number} bytes the filesize in bytes
  // @return {String} human-readable string
  mem(bytes) {
    if (bytes < 0) {
      return 'Unknown';
    }
    if (bytes === 0) {
      return 'None';
    }

    let size = bytes;
    for (const nf of mem_formatters) {
      if (size < kilo) {
        return nf.format(size);
      }
      size /= kilo;
    }

    return 'E2BIG';
  },

  ngettext(msgid, msgid_plural, n) {
    return plural_rules.select(n) === 'one' ? msgid : msgid_plural;
  },

  number(number) {
    return number_format.format(number);
  },

  // format a percentage to a string
  percentString(x, decimal_places) {
    decimal_places = x < 100 ? decimal_places : 0;
    return this._toTruncFixed(x, decimal_places);
  },

  /*
   *   Format a ratio to a string
   */
  ratioString(x) {
    if (x === -1) {
      return 'None';
    }
    if (x === -2) {
      return '&infin;';
    }
    return this.percentString(x, 1);
  },

  /**
   * Formats a disk capacity or file size into a human-readable string
   * @param {Number} bytes the filesize in bytes
   * @return {String} human-readable string
   */
  size(bytes) {
    return this.mem(bytes);
  },

  speed(KBps) {
    return KBps < 999.95 ? fmt_kBps.format(KBps) : fmt_MBps.format(KBps / 1000);
  },

  speedBps(Bps) {
    return this.speed(this.toKBps(Bps));
  },

  stringSanitizer(str) {
    return ['E2BIG', 'NaN'].some((badStr) => str.includes(badStr)) ? `…` : str;
  },

  timeInterval(seconds, granular_depth = 3) {
    const days = Math.floor(seconds / 86_400);
    let buffer = [];
    if (days) {
      buffer.push(this.countString('day', 'days', days));
    }

    const hours = Math.floor((seconds % 86_400) / 3600);
    if (days || hours) {
      buffer.push(this.countString('hour', 'hours', hours));
    }

    const minutes = Math.floor((seconds % 3600) / 60);
    if (days || hours || minutes) {
      buffer.push(this.countString('minute', 'minutes', minutes));
      buffer = buffer.slice(0, granular_depth);
      return buffer.length > 1
        ? `${buffer.slice(0, -1).join(', ')} and ${buffer.slice(-1)}`
        : buffer[0];
    }

    return this.countString('second', 'seconds', Math.floor(seconds % 60));
  },

  timestamp(seconds) {
    if (!seconds) {
      return 'N/A';
    }

    const myDate = new Date(seconds * 1000);
    const now = new Date();

    let date = '';
    let time = '';

    const sameYear = now.getFullYear() === myDate.getFullYear();
    const sameMonth = now.getMonth() === myDate.getMonth();

    const dateDiff = now.getDate() - myDate.getDate();
    if (sameYear && sameMonth && Math.abs(dateDiff) <= 1) {
      if (dateDiff === 0) {
        date = 'Today';
      } else if (dateDiff === 1) {
        date = 'Yesterday';
      } else {
        date = 'Tomorrow';
      }
    } else {
      date = myDate.toDateString();
    }

    let hours = myDate.getHours();
    let period = 'AM';
    if (hours > 12) {
      hours = hours - 12;
      period = 'PM';
    }
    if (hours === 0) {
      hours = 12;
    }
    if (hours < 10) {
      hours = `0${hours}`;
    }
    let minutes = myDate.getMinutes();
    if (minutes < 10) {
      minutes = `0${minutes}`;
    }
    seconds = myDate.getSeconds();
    if (seconds < 10) {
      seconds = `0${seconds}`;
    }

    time = [hours, minutes, seconds].join(':');

    return [date, time, period].join(' ');
  },

  toKBps(Bps) {
    return Math.floor(Bps / kilo);
  },
};
