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
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') {
        const int index = name[0] - 'a' + 1; // A = position 1 of row 4
        return {4 + index / 8, index % 8};
    }
    return {-1, -1};
}

} // namespace mz
