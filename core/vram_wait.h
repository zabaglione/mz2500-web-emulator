// MZ-2500 memory access weights.
//
// MZ2500_IO_Map.pdf's port B5h table has a "Weight at the time of access"
// column against every memory block. Most blocks cost "+1 during M1 cycle",
// which a Z80 core already charges as part of the opcode fetch; the video
// memory blocks are the exception and carry a weight on every access:
//
//   20h-27h standard graphic VRAM               +1 (*)
//   28h-2Fh extended graphic VRAM               +1 (*)
//   30h-31h standard graphic VRAM, read-modify-write  +2 (*)
//   32h-33h extended graphic VRAM, read-modify-write  +2 (*)
//   38h     text VRAM                           +1 (*)
//   39h     kanji ROM / PCG                     +2 (*)
//
// The (*) footnote reads: "M1 cycle is not possible. If you access during
// the display period, a separate weight will be charged until the blanking
// period." That second, much larger wait is a separate mechanism and lives
// in display_stall_cycles() below.
#pragma once

#include <cstdint>

#include "core/timing.h"

namespace mz {

// Fixed weight in T-states the bank table charges for one CPU access to
// `bank`. Zero for every block whose weight is "+1 during M1 cycle", since
// that is the ordinary opcode-fetch cost the CPU core already accounts for.
constexpr int bank_access_wait(int bank) {
    if (bank >= 0x20 && bank <= 0x2F) return 1; // graphic VRAM, direct
    if (bank >= 0x30 && bank <= 0x33) return 2; // graphic VRAM, read-modify-write
    if (bank == 0x38) return 1;                 // text VRAM
    if (bank == 0x39) return 2;                 // kanji ROM / PCG
    return 0;
}

// Cycles the CPU is held off the video bus when it asks for it while the
// controller is scanning, given its absolute cycle count. Zero once the
// access already falls inside a blanking period.
//
// The G-CRTC reads VRAM continuously to feed the display and only hands the
// bus over during blanking, so an access issued in the display period does
// not fail - it stalls, and completes at the start of the next blanking
// window. Within a display raster that window is the horizontal blanking at
// the end of the line; from VBLANK_START_LINE on, nothing is being scanned
// and the whole line is the CPU's.
//
// The size of that window is HBLANK_CYCLES, 70 T-states, which is what a
// period magazine article measured on the machine (core/timing.h). The I/O
// map's own X1 Center note is the same window seen from a program's side:
// "24KHz: memory write cycle + 50 clocks + memory write cycle / 15KHz:
// memory write cycle + 91 clocks + memory write cycle" - two Z80 write
// cycles and 50 free clocks between them is 56 T-states of guaranteed room
// inside a 70-state window at 24 kHz. The note adds that the
// read-modify-write windows' extra weight does not change those numbers:
// the +1/+2 above is paid inside the window, not on top of it.
constexpr int display_stall_cycles(uint64_t cycle) {
    const int in_frame = static_cast<int>(cycle % CYCLES_PER_FRAME);
    const int line = line_of_cycle(in_frame);
    if (line >= VBLANK_START_LINE) return 0;
    const int display_end = line_start_cycle(line + 1) - HBLANK_CYCLES;
    return in_frame < display_end ? display_end - in_frame : 0;
}

} // namespace mz
