// MZ-2500 machine: the single wiring point. Owns the CPU, memory, disk and
// peripheral modules and dispatches I/O port accesses. Ports the game never
// touches are logged once and otherwise ignored.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/banked_memory.h"
#include "core/d88.h"
#include "core/fdc_mb8877.h"
#include "core/gcrtc.h"
#include "core/mouse.h"
#include "core/opn.h"
#include "core/timing.h"
#include "core/vram_wait.h"

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

    // Put an unformatted floppy in a drive. Every read reports record-not-
    // found until a format lays tracks down - a brand-new disk, in other
    // words, and the only thing write tests are ever pointed at.
    bool insert_blank_disk(int drive) {
        return disks_[drive & 1].load(D88Disk::make_unformatted());
    }

    // The current contents of a drive's disk as a D88 image, so the frontend
    // can persist what the machine has written.
    std::vector<uint8_t> disk_image(int drive) const {
        return disks_[drive & 1].serialize();
    }
    bool disk_dirty(int drive) const { return disks_[drive & 1].dirty(); }
    void clear_disk_dirty(int drive) { disks_[drive & 1].clear_dirty(); }
    void set_disk_write_protected(int drive, bool on) {
        disks_[drive & 1].set_write_protected(on);
    }
    bool disk_write_protected(int drive) const {
        return disks_[drive & 1].write_protected();
    }

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
    void set_trace_io(bool v) { trace_io_ = v; }

    // VRAM wait profiler. A development aid for fitting game code to the
    // wait model in core/vram_wait.h: every charged video access is
    // attributed to the address of the instruction that made it, so a
    // program can be told which of its loops the controller is holding off
    // the bus, and for how long. Off - and unallocated - until enabled.
    struct StallSite {
        uint16_t pc = 0;     // instruction that made the accesses
        uint64_t hits = 0;   // accesses it made
        uint64_t stall = 0;  // cycles waiting for a blanking window
        uint64_t weight = 0; // cycles of the bank table's fixed weight
    };
    void enable_stall_profile(bool on);
    void reset_stall_profile();
    // Heaviest first by stall; sites that never touched video are omitted.
    std::vector<StallSite> stall_sites() const;
    // Frames run since the counters were last reset, so a caller can report
    // the cost per frame rather than a total that depends on run length.
    uint64_t stall_profile_frames() const { return frames_ - stall_origin_frame_; }

    // Loop monitor: cycles between consecutive executions of one address.
    // Point it at a main loop's top and it measures how long one pass
    // actually costs, which is the number that decides whether the program
    // holds its frame rate. Unlike counting the program's own frame
    // counter it does not care what the program was doing, so two builds
    // stay comparable after their gameplay has diverged.
    struct LoopStats {
        uint64_t passes = 0;
        uint64_t total = 0;   // cycles summed over all passes
        uint64_t worst = 0;
        uint64_t over = 0;    // passes that did not fit in one frame
        uint64_t over_total = 0; // their cycles, so their own mean is available
    };
    // Measures start_pc -> end_pc. Pass the same address for both to time a
    // whole pass; pass the top of the work and the point it starts waiting
    // to time the work alone, without the wait for the next frame.
    void watch_loop(int start_pc, int end_pc) {
        loop_start_ = start_pc;
        loop_end_ = end_pc;
        loop_ = LoopStats{};
        loop_last_ = 0;
    }
    const LoopStats& loop_stats() const { return loop_; }

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
    // Joystick port EFh, active low: bit0-3 = U/D/L/R, bit4 = trigger 2,
    // bit5 = trigger 1. What a trigger means is the software's business:
    // NEKO CAN RUN deliberately jumps on trigger 2 and lets its CONFIG
    // screen swap the pair.
    void set_joystick_mask(uint8_t mask) { joy_mask_ = mask; }

    // Write a byte directly into the CPU address space (test scenarios'
    // --memory-poke; same semantics as the EmuZ runner's flag)
    void poke_memory(uint16_t addr, uint8_t value) { mem_.write(addr, value); }

    OpnYm2203& opn() { return opn_; }

    // Feed a byte to a SIO channel's receiver, as a device on the line
    // would. Channel B is where the mouse arrives; used to probe what the
    // system's mouse driver makes of a byte stream.
    void sio_receive(int channel, uint8_t value) { sio_[channel & 1].push(value); }

    // Host mouse input. Movement accumulates until the driver strobes DTR;
    // the ratio the machine applies on top is the software's setting and
    // this emulator does not touch it.
    void mouse_move(int dx, int dy) { mouse_.move(dx, dy); }
    void mouse_button(int index, bool down) { mouse_.set_button(index, down); }

    // Test-only hooks for the SIO/OPN mouse wiring, standing in for the CPU
    // without booting real firmware - same idea as poke_memory()/
    // sio_receive() above, but narrow on purpose. An earlier version of
    // this exposed io_out_direct()/io_in_direct(), reaching every I/O port
    // (including the B7h bank-mode switch that drops the boot-ROM overlay,
    // the FDC command registers, the RTC and the GDE); these instead touch
    // exactly the signals the mouse packet path depends on: DTR, the OPN
    // gate byte, and SIO channel B's receive queue.
    void test_set_sio_dtr(int channel, bool high) { sio_dtr_changed(channel & 1, high); }
    void test_set_opn_port_a(uint8_t value) { opn_regs_[0x0E] = value; }
    bool test_sio_channel_b_has_data() const { return !sio_[1].rx_empty(); }
    uint8_t test_sio_channel_b_read_byte() { return sio_[1].pop(); }

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

    void charge_access_wait(int bank);
    // Is the controller fetching the layer this memory block feeds, on the
    // raster the CPU is on? Only then does it hold the bus long enough to
    // stall the access. See charge_access_wait() in mz2500.cpp.
    bool layer_scanning(int bank, int line) const;
    bool graphics_scanning(int line) const;
    bool text_scanning(int line) const;
    WinKind text_vwin_kind() const {
        return win_kind(crtc_vwin_written_, crtc_regs_[3], crtc_regs_[5]);
    }
    uint8_t io_in(uint16_t port);
    uint8_t io_in_raw(uint16_t port);
    uint8_t rtc_read(int reg);
    void rtc_write(int reg, uint8_t value);
    void gvram_rmw_write(int bank, uint16_t off, uint8_t value);
    uint8_t gvram_rmw_read(int bank, uint16_t off);
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
    // port F5h data (includes the graphic palette at 80h+). Register 00h
    // starts at the value every MZ-2500 IPL leaves it at - 25 rows, one text
    // page, 8-colour text over 16-colour graphics - so that a program which
    // never writes it finds the machine the way real firmware hands it over.
    uint8_t crtc_regs_[256] = {0x05};
    // Has anything written the text display window pairs (03h/05h
    // vertical, 07h/08h horizontal)? A window programmed shut and a
    // window never programmed look the same in the registers, so the
    // renderer needs the distinction kept here (core/renderer.cpp).
    bool crtc_vwin_written_ = false;
    bool crtc_hwin_written_ = false;
    // port F6h: bit3 MG, bit2 GE, bit1 RE, bit0 BE (BE also gates the I
    // plane in 16-colour mode). Starts with all three guns enabled, which
    // is what the firmware finds and (mostly) never changes.
    uint8_t cg_mask_ = 0x07;
    uint8_t font_size_ = 0;       // port F7h
    uint8_t gde_index_ = 0;       // port BCh register number (7 bits)
    bool gde_autoinc_ = false;    // BCh bit7: bump reg number after each write
    uint8_t gde_regs_[32] = {};   // port BDh, internal registers 00-1Fh
    uint64_t gde_busy_until_ = 0; // hardware GRAM clear in progress
    uint8_t gvram_latch_[4] = {}; // ports BCh-BFh: last plane bytes read
    // GDEHS/GDEHE (registers 0Ch/0Dh) are the horizontal display window.
    // The controller compares them against its dot counter as it scans, so
    // the value that matters is the one in force on the raster being drawn,
    // not the one left in the register when the frame ends. A program that
    // reshapes the window every raster - the way a non-rectangular mask over
    // a picture is made - depends on that, and it parks the window shut in
    // vertical blanking between passes. Reading the registers once per frame
    // samples exactly that parked value and blanks the whole screen, so keep
    // what each line saw instead.
    uint8_t hwin_line_[LINES_PER_FRAME][2] = {};
    int current_line() const;
    void seed_hwin_lines();

    // Z80B SIO. Channel A is the RS-232C 9-pin port (A0h data, A1h command
    // and status); channel B is the 25-pin port, or the mouse when OPN port
    // A bit3 is set (A2h/A3h). Nothing is wired to either yet, so the model
    // is the register file and a status byte that says "no character has
    // arrived, the transmitter is idle" - which is what an absent device
    // looks like, and is not what open bus was telling the firmware.
    struct SioChannel {
        uint8_t regs[8] = {};
        uint8_t pointer = 0; // next register the command port addresses
        uint8_t rx[16] = {};
        uint8_t rx_head = 0, rx_tail = 0;
        bool rx_empty() const { return rx_head == rx_tail; }
        void push(uint8_t v) {
            const uint8_t n = (uint8_t)((rx_tail + 1) & 15);
            if (n == rx_head) return; // full: the real chip would overrun
            rx[rx_tail] = v;
            rx_tail = n;
        }
        uint8_t pop() {
            if (rx_empty()) return 0;
            const uint8_t v = rx[rx_head];
            rx_head = (uint8_t)((rx_head + 1) & 15);
            return v;
        }
    };
    SioChannel sio_[2];
    Mouse mouse_;
    // The mouse shares channel B with the 25-pin RS-232C port; OPN port A
    // bit3 is the switch (Oh!MZ p299). With the switch open the line is
    // empty, which is what an unplugged port looks like.
    bool mouse_connected() const { return (opn_regs_[0x0E] & 0x08) != 0; }
    void sio_dtr_changed(int ch, bool dtr);
    bool sio_dtr_[2] = {false, false};
    uint8_t sio_status(int ch) const;
    uint8_t palette_[32] = {};    // port AEh, indexed by B register
    bool palette_written_ = false; // MZ-1M10 palette RAM has been programmed
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
    uint64_t last_vblank_frame_ = ~0ULL; // frame whose VBLANK interrupt fired
    uint8_t rtc_mode_ = 0;        // RP5C15 register Dh (bank select bit0)
    uint8_t rtc_bank1_[13] = {};  // RP5C15 bank-1 latches (alarm/12h/leap)

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

    // VRAM wait profiler state. One counter per code address, allocated only
    // while profiling so neither the browser build nor an ordinary run pays
    // for it. insn_pc_ is the address run_frame() is about to execute, which
    // is what a memory callback needs: by the time the callback fires the
    // CPU's own PC has already moved past the operand bytes.
    struct StallCounters {
        uint64_t hits[0x10000] = {};
        uint64_t stall[0x10000] = {};
        uint64_t weight[0x10000] = {};
    };
    std::unique_ptr<StallCounters> stall_counters_;
    uint16_t insn_pc_ = 0;
    uint64_t stall_origin_frame_ = 0;
    int loop_start_ = -1;
    int loop_end_ = -1;
    uint64_t loop_last_ = 0;
    LoopStats loop_;

    int trap_watch_ = -1;
    bool trap_hit_ = false;

    uint64_t frame_origin_ = 0;
    uint64_t frames_ = 0;
    int boot_delay_frames_ = DEFAULT_BOOT_DELAY_FRAMES;
    int idle_frames_remaining_ = 0;
    bool trace_boot_ = false;
    bool trace_io_ = false;
    bool warned_in_[256] = {};
    bool warned_out_[256] = {};
};

} // namespace mz
