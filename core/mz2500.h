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

    // User-provided ROM images (kind: 0=ipl, 1=cg, 2=kanji, 3=dict). Never
    // bundled; the owner supplies the files at runtime.
    void set_rom(int kind, const uint8_t* data, size_t size);

    // Expansion-board configuration (kind: 0=expansion RAM 256KB,
    // 1=expansion GRAM second screen, 2=MZ-1M10 4096-colour palette board).
    // Takes effect immediately; RAM/GRAM changes want a RESET to be sane.
    void set_hw_option(int kind, bool on) {
        switch (kind) {
        case 0: mem_.set_expansion_ram(on); break;
        case 1: mem_.set_expansion_gram(on); break;
        case 2: mz1m10_present_ = on; break;
        }
    }
    bool has_ipl_rom() const { return mem_.has_ipl_rom(); }

    // Authentic cold boot through a user-provided IPL ROM: reset bank map
    // {34h-37h, 04-07}, PC=0000h. Experimental - exercises whatever hardware
    // the firmware touches. Implemented in core/dummy_ipl.cpp.
    bool boot_with_real_ipl();

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

    // Machine state snapshot as JSON (debug panel / future tooling).
    // Returns the number of bytes written (excluding the terminator).
    size_t debug_json(char* buf, size_t cap);

    // Firmware forensics: when PC first reaches `addr`, dump the recent
    // execution and I/O history to stderr (CLI --trace-trap).
    void set_trap_watch(uint16_t addr) { trap_watch_ = addr; trap_hit_ = false; }
    void dump_forensics(const char* why);

private:
    static uint8_t cb_read(void* ud, uint16_t addr);
    static void cb_write(void* ud, uint16_t addr, uint8_t value);
    static uint8_t cb_in(z80* z, uint16_t port);
    static void cb_out(z80* z, uint16_t port, uint8_t value);

    uint8_t io_in(uint16_t port);
    void io_out(uint16_t port, uint8_t value);
    uint8_t blank_flags() const; // port F4h read: bit0 VBLANK, bit1 HBLANK
    void log_port_once(uint16_t port, const char* dir);
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
    uint64_t gde_busy_until_ = 0; // hardware GRAM clear in progress
    uint8_t palette_[32] = {};    // port AEh, indexed by B register
    // interrupt controller (C6h/C7h) + 8253 ch0. MZSD is the only client:
    // C6h bit2 enables the i8253 source, bit6 selects it as the C7h vector
    // destination; other sources (CRTC etc.) are unused by this game.
    // Interrupt controller: C6h high nibble selects which source's vector
    // register the next C7h write lands in (bit7..4 -> source 3..0), low
    // nibble enables sources (bit3..0). Pairs decoded from the firmware:
    // source 3 = CRTC, source 2 = i8253, sources 1/0 = periodic system
    // ticks the firmware's ISRs expect (identity still under study).
    uint8_t int_select_ = 0;      // port C6h latch
    uint8_t int_vectors_[4] = {}; // per-source IM2 vector bytes
    bool int_pending_[4] = {};
    uint64_t tick_next_[2] = {0, 0}; // next fire for sources 0 and 1

    // 8253, all three channels driven from CPU/192 (31.25 kHz). Channel 0
    // feeds the interrupt controller (MZSD's 125 Hz heartbeat); the firmware
    // additionally programs ch1/ch2 and polls their counters for delays.
    struct PitChannel {
        uint8_t control = 0;   // last mode word
        uint16_t reload = 0;   // 0 means 65536
        uint8_t wr_phase = 0;  // lo/hi assembly state
        uint8_t rd_phase = 0;
        uint16_t latch = 0;
        bool latched = false;
        bool counting = false;
        uint64_t start_cyc = 0;
        uint32_t count() const { return reload ? reload : 0x10000; }
    };
    PitChannel pit_[3];
    bool pit_counting_ = false;   // ch0 interrupt scheduling
    uint64_t pit_next_fire_ = 0;
    uint16_t pit_current(int ch) const;
    void pit_write_control(uint8_t value);
    void pit_write_counter(int ch, uint8_t value);
    uint8_t pit_read_counter(int ch);
    uint8_t pio_a_ = 0;           // port E8h latch
    uint8_t bank_mode_ = 0;       // port B7h latch
    uint8_t ppi_[3] = {};         // 8255 A/B/C latches (E0h-E2h)
    uint8_t pio_ctrl_[2] = {};    // Z80 PIO control words (E9h/EBh)
    bool mz1m10_present_ = true;  // 4096-colour palette board option
    uint8_t joy_enable_ = 0;      // port EFh
    uint8_t key_rows_[16] = {};   // pressed bits per matrix row
    uint8_t joy_mask_ = 0;        // pressed joystick bits

    // forensics rings (always recorded; cheap)
    static constexpr int PC_RING = 128;
    static constexpr int IO_RING = 64;
    uint16_t pc_ring_[PC_RING] = {};
    int pc_ring_pos_ = 0;
    struct IoEvent { uint64_t cyc; uint16_t port; uint8_t value; bool out; };
    IoEvent io_ring_[IO_RING] = {};
    int io_ring_pos_ = 0;
    int trap_watch_ = -1;
    bool trap_hit_ = false;

    uint64_t frame_origin_ = 0;
    uint64_t frames_ = 0;
    int boot_delay_frames_ = DEFAULT_BOOT_DELAY_FRAMES;
    int idle_frames_remaining_ = 0;
    bool trace_boot_ = false;
    bool warned_in_[256] = {};
    bool warned_out_[256] = {};
};

} // namespace mz
