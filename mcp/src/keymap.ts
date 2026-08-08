// MZ-2500 key matrix mapping, ported from core/keyboard.h (the C++ side is
// the source of truth; positions verified black-box against BASIC's echo).

export interface KeyPos {
  row: number;
  bit: number;
}

export const SHIFT_POS: KeyPos = { row: 11, bit: 2 };

const NAMED: Record<string, KeyPos> = {
  space: { row: 3, bit: 1 },
  up: { row: 3, bit: 3 },
  down: { row: 3, bit: 4 },
  left: { row: 3, bit: 5 },
  right: { row: 3, bit: 6 },
  cr: { row: 3, bit: 2 },
  return: { row: 3, bit: 2 },
  enter: { row: 3, bit: 2 },
  tab: { row: 3, bit: 0 },
  break: { row: 3, bit: 7 },
  plus: { row: 1, bit: 6 },
  colon: { row: 9, bit: 2 },
  semicolon: { row: 9, bit: 3 },
  minus: { row: 9, bit: 4 },
  at: { row: 9, bit: 5 },
  lbracket: { row: 9, bit: 6 },
  slash: { row: 4, bit: 0 },
  rbracket: { row: 10, bit: 0 },
  del: { row: 10, bit: 3 },
  bs: { row: 10, bit: 4 },
  backspace: { row: 10, bit: 4 },
  esc: { row: 10, bit: 5 },
  caret: { row: 7, bit: 3 },
  yen: { row: 7, bit: 4 },
  underscore: { row: 7, bit: 5 },
  period: { row: 7, bit: 6 },
  comma: { row: 7, bit: 7 },
  shift: SHIFT_POS,
  ctrl: { row: 11, bit: 4 },
  copy: { row: 10, bit: 1 },
  home: { row: 10, bit: 2 },
  clr: { row: 10, bit: 2 },
  kpstar: { row: 10, bit: 6 },
  kpslash: { row: 10, bit: 7 },
  kpminus: { row: 1, bit: 7 },
  graph: { row: 11, bit: 0 },
  lock: { row: 11, bit: 1 },
  kana: { row: 11, bit: 3 },
  muhenkan: { row: 12, bit: 0 },
  henkan: { row: 12, bit: 1 },
  help: { row: 13, bit: 1 },
  algo: { row: 13, bit: 0 },
  kp8: { row: 1, bit: 2 },
  kp9: { row: 1, bit: 3 },
};

// F1-F10, digits, tenkey, letters are generated to match key_from_name()
export function keyFromName(nameRaw: string): KeyPos | null {
  const name = nameRaw.toLowerCase();
  if (name in NAMED) return NAMED[name];
  if (/^f([1-9]|10)$/.test(name)) {
    const n = parseInt(name.slice(1), 10) - 1; // F1=(0,0)..F8=(0,7), F9/F10=(1,0/1)
    return n < 8 ? { row: 0, bit: n } : { row: 1, bit: n - 8 };
  }
  if (/^[0-9]$/.test(name)) {
    const d = name.charCodeAt(0) - 0x30;
    return { row: 8 + Math.floor(d / 8), bit: d & 7 };
  }
  if (/^kp[0-7]$/.test(name)) return { row: 2, bit: parseInt(name[2], 10) };
  if (/^[a-z]$/.test(name)) {
    const index = name.charCodeAt(0) - 0x61 + 1; // A = position 1 of row 4
    return { row: 4 + Math.floor(index / 8), bit: index % 8 };
  }
  return null;
}

export interface CharKey {
  pos: KeyPos;
  shift: boolean;
}

const SHIFTED: Record<string, KeyPos> = {
  '"': { row: 8, bit: 2 },
  "!": { row: 8, bit: 1 },
  "#": { row: 8, bit: 3 },
  $: { row: 8, bit: 4 },
  "%": { row: 8, bit: 5 },
  "&": { row: 8, bit: 6 },
  "'": { row: 8, bit: 7 },
  "(": { row: 9, bit: 0 },
  ")": { row: 9, bit: 1 },
  "*": { row: 9, bit: 2 },
  "+": { row: 9, bit: 3 },
  "=": { row: 9, bit: 4 },
  "?": { row: 4, bit: 0 },
  "<": { row: 7, bit: 7 },
  ">": { row: 7, bit: 6 },
};

const PLAIN: Record<string, KeyPos> = {
  " ": { row: 3, bit: 1 },
  "\n": { row: 3, bit: 2 },
  "\r": { row: 3, bit: 2 },
  ":": { row: 9, bit: 2 },
  ";": { row: 9, bit: 3 },
  "-": { row: 9, bit: 4 },
  "@": { row: 9, bit: 5 },
  "[": { row: 9, bit: 6 },
  "/": { row: 4, bit: 0 },
  "]": { row: 10, bit: 0 },
  "^": { row: 7, bit: 3 },
  "\\": { row: 7, bit: 4 }, // the yen key
  _: { row: 7, bit: 5 },
  ".": { row: 7, bit: 6 },
  ",": { row: 7, bit: 7 },
};

// Where a printable character sits, and whether SHIFT is held to get it.
// Returns null for characters this keyboard cannot produce.
export function keyForChar(ch: string): CharKey | null {
  if (ch >= "A" && ch <= "Z") {
    const lower = keyForChar(ch.toLowerCase());
    return lower ? { pos: lower.pos, shift: true } : null;
  }
  if (ch >= "a" && ch <= "z") {
    const index = ch.charCodeAt(0) - 0x61 + 1;
    return { pos: { row: 4 + Math.floor(index / 8), bit: index % 8 }, shift: false };
  }
  if (ch >= "0" && ch <= "9") {
    const d = ch.charCodeAt(0) - 0x30;
    return { pos: { row: 8 + Math.floor(d / 8), bit: d & 7 }, shift: false };
  }
  if (ch in PLAIN) return { pos: PLAIN[ch], shift: false };
  if (ch in SHIFTED) return { pos: SHIFTED[ch], shift: true };
  return null;
}
