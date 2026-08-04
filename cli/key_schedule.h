// Pure, testable computation of "which matrix positions must be held down
// at frame N" from every window-based CLI input source (--key-pulse and
// --type, including the SHIFT each typed character needs).
//
// This is split out of main.cpp so two overlapping sources holding the same
// key cannot fight over its state: without it, --key-pulse and --type each
// wrote machine.set_key() directly from their own start/end windows, so
// whichever source's window happened to end sooner would silently release a
// key another source still wanted held. Computing the whole frame's desired
// state up front and applying it once (see main.cpp's per-frame loop) means
// a key stays down as long as ANY source's window covers the frame, and the
// union logic itself is small enough to unit test directly - see
// tests/unit/test_key_schedule.cpp.
#pragma once

#include <vector>

#include "core/keyboard.h"

namespace mz_cli {

struct KeyPulse {
    mz::KeyPos pos;
    long start;
    long end;
};

struct TypedKey {
    mz::KeyPos pos;
    bool shift;
    long start;
    long end;
};

// SHIFT's own matrix position. Shared so main.cpp and this header agree on
// where a typed character's implicit SHIFT lands.
inline mz::KeyPos shift_key_pos() { return {11, 2}; }

// Returns every matrix position that must be held down at `frame`, as the
// union of all key_pulses windows ([start,end)) and all typed windows
// ([start,end), each also asserting shift_key_pos() while t.shift is set).
// A position held by more than one source appears exactly once.
inline std::vector<mz::KeyPos> keys_down_at_frame(
    long frame,
    const std::vector<KeyPulse>& key_pulses,
    const std::vector<TypedKey>& typed) {
    std::vector<mz::KeyPos> down;
    auto add = [&](mz::KeyPos pos) {
        for (const auto& p : down) {
            if (p.row == pos.row && p.bit == pos.bit) return;
        }
        down.push_back(pos);
    };
    for (const auto& k : key_pulses) {
        if (frame >= k.start && frame < k.end) add(k.pos);
    }
    for (const auto& t : typed) {
        if (frame >= t.start && frame < t.end) {
            add(t.pos);
            if (t.shift) add(shift_key_pos());
        }
    }
    return down;
}

} // namespace mz_cli
