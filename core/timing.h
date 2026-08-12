// MZ-2500 timing constants. Single source of truth for the whole core.
//
// The Z80B runs at 6 MHz. The machine's published video timing for the
// 24 kHz mode is a frame of 448 lines of which 400 display, refreshed at
// 55.486447 Hz - which makes the horizontal frequency 448 x 55.486447 =
// 24.86 kHz, the "24 kHz" the mode is named for, and a line 6,000,000 /
// 24,858 = 241.4 CPU cycles. A period magazine article measured the same
// line from the CPU's side and got "about 170 states of scanning plus about
// 70 states of blanking" at 6 MHz, i.e. about 240 states a line.
//
// 448 does not divide a whole number of cycles into any frame near that
// rate, and it should not: the dot clock is not an integer multiple of the
// CPU clock, so real line boundaries fall between CPU cycles. So the frame
// total is the constant and the line boundaries are derived from it -
// 108,160 cycles a frame (55.473 Hz, 0.024% slow against 55.486447) over
// 448 lines, which is 241.43 cycles a line: alternately 241 and 242,
// averaging what the hardware does. line_start_cycle() below is where that
// division lives, and nothing outside this header assumes a line is a fixed
// number of cycles.
//
// The 8253 PIT input clock is 31.25 kHz (CPU/192); the MZSD sound driver
// programs ch0 to divide by 250 for its 125 Hz interrupt.
#pragma once

#include <cstdint>

namespace mz {

constexpr uint64_t CPU_HZ = 6'000'000;

constexpr int CYCLES_PER_FRAME = 108'160;
constexpr int LINES_PER_FRAME = 448;

constexpr double FRAMES_PER_SEC =
    static_cast<double>(CPU_HZ) / CYCLES_PER_FRAME; // ~55.47

// 400 of the 448 lines carry picture; the last 48 are vertical blanking.
constexpr int VBLANK_START_LINE = 400;

// The 15 kHz / 200-line monitor timing used by MZ-2000 and MZ-80B
// compatibility mode. The hardware runs 262 lines at 60.99 Hz. Device time
// in this core stays on the MZ-2500's 6 MHz master axis even while the Z80 is
// divided to 4 MHz, hence round(6 MHz / 60.99) ticks per frame.
constexpr int CYCLES_PER_FRAME_15KHZ = 98'377;
constexpr int LINES_PER_FRAME_15KHZ = 262;
constexpr int VBLANK_START_LINE_15KHZ = 200;
constexpr int HBLANK_CYCLES_15KHZ = 110;

constexpr int line_start_cycle_for(int line, int cycles, int lines) {
    return static_cast<int>(((int64_t)line * cycles + lines - 1) / lines);
}

constexpr int line_of_cycle_for(int in_frame, int cycles, int lines) {
    return static_cast<int>((int64_t)in_frame * lines / cycles);
}

// Position on the machine's current video timebase. run_frame() advances
// frame_origin only after the last Z80 instruction completes, so that
// instruction can already be a few cycles into the following frame.
constexpr int cycle_in_frame(uint64_t cycle, uint64_t frame_origin,
                             int cycles_per_frame) {
    return cycle < frame_origin || cycles_per_frame <= 0
        ? 0
        : static_cast<int>((cycle - frame_origin) %
                           static_cast<uint64_t>(cycles_per_frame));
}

// Horizontal blanking, at the end of every line. The article above measured
// it directly at 6 MHz - 70 states against 170 of scanning - and that is a
// measurement of the window the CPU gets, which is exactly what the VRAM
// wait model needs (core/vram_wait.h). It is deliberately not the
// dot-derived figure: the published dot timing is 864 dots a line of which
// 200 blank, which would be 23.1% of the line rather than the measured
// 29.2%.
constexpr int HBLANK_CYCLES = 70;

// First cycle of `line`, counted from the start of the frame. Line
// boundaries fall between CPU cycles (see above), so this rounds up; that
// makes line_of_cycle() its exact inverse, and makes
// line_start_cycle(LINES_PER_FRAME) land on CYCLES_PER_FRAME.
constexpr int line_start_cycle(int line) {
    return line_start_cycle_for(line, CYCLES_PER_FRAME, LINES_PER_FRAME);
}

// Which line a cycle offset within the frame belongs to.
constexpr int line_of_cycle(int in_frame) {
    return line_of_cycle_for(in_frame, CYCLES_PER_FRAME, LINES_PER_FRAME);
}

// 8253 PIT input clock divider: 6 MHz / 192 = 31.25 kHz.
constexpr int PIT_CLOCK_DIV = 192;

// YM2203 master clock on the MZ-2500.
constexpr uint32_t OPN_CLOCK_HZ = 2'000'000;

// MZ-1E35 ADPCM board (Y8950): the I/O map gives the master clock as
// 3.58 MHz - the NTSC colourburst crystal.
constexpr uint32_t ADPCM_CLOCK_HZ = 3'579'545;

} // namespace mz
