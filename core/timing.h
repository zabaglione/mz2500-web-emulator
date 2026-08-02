// MZ-2500 timing constants. Single source of truth for the whole core.
//
// The Z80B runs at 6 MHz. Frame geometry approximates the measured EmuZ-2500
// rate of 55.49 Hz: 416 cycles/line x 260 lines = 108,160 cycles ≈ 55.47 Hz.
// The 8253 PIT input clock is 31.25 kHz (CPU/192); the MZSD sound driver
// programs ch0 to divide by 250 for its 125 Hz interrupt.
#pragma once

#include <cstdint>

namespace mz {

constexpr uint64_t CPU_HZ = 6'000'000;

constexpr int CYCLES_PER_LINE = 416;
constexpr int LINES_PER_FRAME = 260;
constexpr int CYCLES_PER_FRAME = CYCLES_PER_LINE * LINES_PER_FRAME; // 108,160

constexpr double FRAMES_PER_SEC =
    static_cast<double>(CPU_HZ) / CYCLES_PER_FRAME; // ~55.47

// Display covers lines 0-199; VBLANK flag (port F4h bit0) is set on 200-259.
constexpr int VBLANK_START_LINE = 200;
// HBLANK flag (port F4h bit1) is set during the tail of each line.
constexpr int HBLANK_CYCLES = 104;

// 8253 PIT input clock divider: 6 MHz / 192 = 31.25 kHz.
constexpr int PIT_CLOCK_DIV = 192;

// YM2203 master clock on the MZ-2500.
constexpr uint32_t OPN_CLOCK_HZ = 2'000'000;

} // namespace mz
