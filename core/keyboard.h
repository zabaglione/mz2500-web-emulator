// Key-name to matrix-position table for the MZ-2500 keyboard.
//
// Derived empirically (black-box) by pulsing named keys in EmuZ-2500 and
// reading back the game's keys_cur bitmask, cross-checked against matrix
// comments in games/neko_can_run/tools/resident_cache.asm (B=row4 bit2,
// M=row5 bit5, W=row6 bit7): the letters run linearly from A=(4,1) to
// Z=(7,2), and row 3 carries SPACE plus the cursor cluster.
#pragma once

#include <cctype>
#include <string>

namespace mz {

struct KeyPos {
    int row;
    int bit;
};

// returns {-1,-1} for unknown names
inline KeyPos key_from_name(std::string name) {
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name == "space") return {3, 1};
    if (name == "up") return {3, 3};
    if (name == "down") return {3, 4};
    if (name == "left") return {3, 5};
    if (name == "right") return {3, 6};
    if (name == "cr" || name == "return" || name == "enter") return {3, 2};
    if (name == "tab") return {3, 0};
    if (name == "break") return {3, 7};
    if (name == "plus") return {1, 6};       // tenkey +
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
        return {8 + (name[0] - '0') / 8, (name[0] - '0') & 7};
    if (name.size() == 3 && name.compare(0, 2, "kp") == 0 && name[2] >= '0' && name[2] <= '7')
        return {2, name[2] - '0'};
    if (name == "kp8") return {1, 2};
    if (name == "kp9") return {1, 3};
    if (name == "colon") return {9, 2};
    if (name == "semicolon") return {9, 3};
    if (name == "minus") return {9, 4};
    if (name == "at") return {9, 5};
    if (name == "lbracket") return {9, 6};
    // Main-keyboard slash is row 4 bit 0, not row 9 bit 7. The I/O map's
    // key-matrix table prints "/" in both cells, but only (4,0) echoes a
    // slash at the BASIC prompt; (9,7) answers nothing at all, so no key
    // is wired there. Verified by pulsing each cell on its own.
    if (name == "slash") return {4, 0};
    if (name == "rbracket") return {10, 0};
    if (name == "del") return {10, 3};
    if (name == "bs" || name == "backspace") return {10, 4};
    if (name == "esc") return {10, 5};
    if (name == "caret") return {7, 3};
    if (name == "yen") return {7, 4};
    if (name == "underscore") return {7, 5};
    if (name == "period") return {7, 6};
    if (name == "comma") return {7, 7};
    if (name == "shift") return {11, 2};
    if (name == "ctrl") return {11, 4};
    // The following ten matrix positions (per MZ2500_IO_Map.pdf's "Key
    // matrix" table, port E8h/EAh, strobe rows 10-13) had no name at all
    // before this comment was added - key_from_name() simply didn't know
    // them. Positions taken verbatim from that table; "home"/"clr" are two
    // names for the same key cap (row 10 bit 2), which is silkscreened
    // HOME/CLR on the physical keyboard.
    if (name == "copy") return {10, 1};      // row 10 bit 1: COPY
    if (name == "home" || name == "clr") return {10, 2}; // row 10 bit 2: HOME/CLR
    if (name == "kpstar") return {10, 6};    // row 10 bit 6: tenkey *
    if (name == "kpslash") return {10, 7};   // row 10 bit 7: tenkey /
    // Row 1 bit 7 is blank in the I/O map's table, but it echoes the
    // tenkey minus at the BASIC prompt. Found by sweeping the matrix.
    if (name == "kpminus") return {1, 7};
    if (name == "graph") return {11, 0};     // row 11 bit 0: GRAPH
    if (name == "lock") return {11, 1};      // row 11 bit 1: LOCK
    if (name == "kana") return {11, 3};      // row 11 bit 3: KANA
    if (name == "muhenkan") return {12, 0};  // row 12 bit 0: 無変換
    if (name == "henkan") return {12, 1};    // row 12 bit 1: 変換
    if (name == "help") return {13, 1};      // row 13 bit 1: HELP
    // ALGO ("アルゴ機能" - the icon menu of small system utilities,
    // MZ2500_UserManual.pdf's "アルゴ機能の選択" section) has no label at
    // all in the IO map's key matrix table (row 13 bit 0 is blank there).
    // Identified empirically: pulsing row 13 bit 0 alone while BASIC-M25 is
    // sitting at its "Ok" prompt (booted via the real IPL) pops up an icon
    // row (calculator, memo, disk, star icons) at the bottom of the screen
    // that stays on screen unchanged for 1600+ further frames - not a
    // transient glitch. No other one of the 112 matrix positions produced
    // that popup.
    if (name == "algo") return {13, 0};      // row 13 bit 0: ALGO (undocumented in the IO map)
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') {
        const int index = name[0] - 'a' + 1; // A = position 1 of row 4
        return {4 + index / 8, index % 8};
    }
    return {-1, -1};
}

// Where a printable character sits on the matrix, and whether SHIFT is held
// to get it. Verified against BASIC's own echo: SHIFT+2 is the double quote.
// Returns {-1,-1} for characters this keyboard cannot produce.
inline KeyPos key_for_char(char c, bool& shift) {
    shift = false;
    if (c >= 'A' && c <= 'Z') {
        shift = true;
        c = (char)(c - 'A' + 'a');
    }
    if (c >= 'a' && c <= 'z') {
        const int index = c - 'a' + 1;
        return {4 + index / 8, index % 8};
    }
    if (c >= '0' && c <= '9') return {8 + (c - '0') / 8, (c - '0') & 7};
    switch (c) {
    case ' ': return {3, 1};
    case '\n': case '\r': return {3, 2};
    case ':': return {9, 2};
    case ';': return {9, 3};
    case '-': return {9, 4};
    case '@': return {9, 5};
    case '[': return {9, 6};
    case '/': return {4, 0};
    case ']': return {10, 0};
    case '^': return {7, 3};
    case '\\': return {7, 4};
    case '_': return {7, 5};
    case '.': return {7, 6};
    case ',': return {7, 7};
    // Of the shifted symbols only the double quote is confirmed against
    // BASIC's own echo; the rest follow the key caps. If one of them turns
    // out to print something else, fix it here rather than working around it
    // at the call site.
    case '"': shift = true; return {8, 2};  // SHIFT + 2 (verified)
    case '!': shift = true; return {8, 1};  // SHIFT + 1
    case '#': shift = true; return {8, 3};
    case '$': shift = true; return {8, 4};
    case '%': shift = true; return {8, 5};
    case '&': shift = true; return {8, 6};
    case '\'': shift = true; return {8, 7};
    case '(': shift = true; return {9, 0};  // SHIFT + 8
    case ')': shift = true; return {9, 1};  // SHIFT + 9
    case '*': shift = true; return {9, 2};  // SHIFT + :
    case '+': shift = true; return {9, 3};  // SHIFT + ;
    case '=': shift = true; return {9, 4};  // SHIFT + -
    case '?': shift = true; return {4, 0};  // SHIFT + /
    case '<': shift = true; return {7, 7};  // SHIFT + ,
    case '>': shift = true; return {7, 6};  // SHIFT + .
    default: return {-1, -1};
    }
}

} // namespace mz
