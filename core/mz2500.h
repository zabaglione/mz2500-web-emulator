// MZ-2500 machine: the single wiring point. Owns the CPU, memory, disk and
// peripheral modules and dispatches I/O port accesses. Ports the game never
// touches are logged once and otherwise ignored.
#pragma once

#include <cstdint>
#include <string>

#include "core/banked_memory.h"
#include "core/d88.h"
#include "core/fdc_mb8877.h"
#include "core/opn.h"
#include "core/timing.h"

extern "C" {
#include "z80/z80.h"
}

namespace mz {

class Mz2500 {
public:
    // P2 calibration: with the FDC read latency at its default, both EmuZ
    // and this core hit audio_boot at frame 562 and title BGM at frame 1804.
    static constexpr int DEFAULT_BOOT_DELAY_FRAMES = 249;

    Mz2500();

    // Two floppy drives (FD1 = drive 0, FD2 = drive 1). Inserting is a hot
    // swap: it never resets the machine, so mid-game disk changes work.
    bool insert_disk(int drive, const std::string& path);
    bool insert_disk_bytes(int drive, std::vector<uint8_t> bytes) {
        return disks_[drive & 1].load(std::move(bytes));
    }
    bool insert_disk(const std::string& path) { return insert_disk(0, path); }

    // Native replacement for the IPL ROM boot sequence ("dummy IPL"):
    // interprets the IPLPRO header on the mounted disk and starts the CPU at
    // the load address, with the initial device state the real IPL leaves
    // behind. Implemented in core/dummy_ipl.cpp.
    bool boot_from_disk();

    void run_frame();

    // Compose the current screen into a 640x400 RGBA buffer (renderer.cpp).
    void render(uint8_t* rgba) const;

    uint64_t frames() const { return frames_; }
    uint64_t cycles() const { return cpu_.cyc; }
    const z80& cpu() const { return cpu_; }
    BankedMemory& memory() { return mem_; }
    uint8_t read_memory(uint16_t addr) { return mem_.read(addr); }

    void set_trace_boot(bool v) { trace_boot_ = v; }

    // Frames the real IPL ROM spends (RAM check, drive spin-up, header load)
    // before jumping to the boot sector. Calibrated against EmuZ-2500 so
    // frame-numbered input pulses line up between the two emulators.
    void set_boot_delay_frames(int frames) { boot_delay_frames_ = frames; }
    FdcMb8877& fdc() { return fdc_; }

    // Keyboard matrix (active-low rows read through PIO port B / EAh; the
    // row is the low nibble of the E8h latch). Empirically verified vs
    // EmuZ: row 3 = space/cursor cluster, letters run A=(4,1) .. Z=(7,2).
    void set_key(int row, int bit, bool down) {
        if (down) key_rows_[row & 15] |= (uint8_t)(1 << bit);
        else key_rows_[row & 15] &= (uint8_t)~(1 << bit);
    }
    // Joystick port EFh, active low: bit0-3 = U/D/L/R, bit4 = trigger2
    // (dash), bit5 = trigger1 (jump)
    void set_joystick_mask(uint8_t mask) { joy_mask_ = mask; }

    // Write a byte directly into the CPU address space (test scenarios'
    // --memory-poke; same semantics as the EmuZ runner's flag)
    void poke_memory(uint16_t addr, uint8_t value) { mem_.write(addr, value); }

    OpnYm2203& opn() { return opn_; }

private:
    static uint8_t cb_read(void* ud, uint16_t addr);
    static void cb_write(void* ud, uint16_t addr, uint8_t value);
    static uint8_t cb_in(z80* z, uint16_t port);
    static void cb_out(z80* z, uint16_t port, uint8_t value);

    uint8_t io_in(uint16_t port);
    void io_out(uint16_t port, uint8_t value);
    uint8_t blank_flags() const; // port F4h read: bit0 VBLANK, bit1 HBLANK
    void log_port_once(uint16_t port, const char* dir);
    void pit_write_counter(uint8_t value);
    void service_interrupts();

    z80 cpu_{};
    BankedMemory mem_;
    D88Disk disks_[FdcMb8877::NUM_DRIVES];
    FdcMb8877 fdc_;
    OpnYm2203 opn_;

    // register latches for devices that later phases bring to life
    uint8_t opn_addr_ = 0;
    uint8_t opn_regs_[256] = {};
    uint8_t crtc_index_ = 0;      // port F4h write
    uint8_t crtc_regs_[256] = {}; // port F5h data (includes CLUT at 80h+)
    uint8_t cg_mask_ = 0;         // port F6h
    uint8_t font_size_ = 0;       // port F7h
    uint8_t gde_index_ = 0;       // port BCh
    uint8_t gde_regs_[32] = {};   // port BDh
    uint8_t palette_[32] = {};    // port AEh, indexed by B register
    // interrupt controller (C6h/C7h) + 8253 ch0. MZSD is the only client:
    // C6h bit2 enables the i8253 source, bit6 selects it as the C7h vector
    // destination; other sources (CRTC etc.) are unused by this game.
    uint8_t int_select_ = 0;      // port C6h
    uint8_t int_vector_ = 0;      // port C7h
    bool pit_int_pending_ = false;

    // 8253 ch0, mode 2: input clock CPU/192 (31.25 kHz)
    uint16_t pit_reload_ = 0;     // 0 means 65536
    uint8_t pit_write_phase_ = 0; // 0 = expect low byte, 1 = expect high
    bool pit_counting_ = false;
    uint64_t pit_next_fire_ = 0;
    uint8_t pio_a_ = 0;           // port E8h latch
    uint8_t joy_enable_ = 0;      // port EFh
    uint8_t key_rows_[16] = {};   // pressed bits per matrix row
    uint8_t joy_mask_ = 0;        // pressed joystick bits

    uint64_t frame_origin_ = 0;
    uint64_t frames_ = 0;
    int boot_delay_frames_ = DEFAULT_BOOT_DELAY_FRAMES;
    int idle_frames_remaining_ = 0;
    bool trace_boot_ = false;
    bool warned_in_[256] = {};
    bool warned_out_[256] = {};
};

} // namespace mz
