// MZ-2500 machine: the single wiring point. Owns the CPU, memory, disk and
// peripheral modules and dispatches I/O port accesses. Ports the game never
// touches are logged once and otherwise ignored.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/adpcm.h"
#include "core/banked_memory.h"
#include "core/cmt.h"
#include "core/d88.h"
#include "core/emm.h"
#include "core/fdc_mb8877.h"
#include "core/gcrtc.h"
#include "core/mouse.h"
#include "core/opn.h"
#include "core/printer.h"
#include "core/sasi.h"
#include "core/timing.h"
#include "core/vram_wait.h"
#include "core/z80_sio.h"

extern "C" {
#include "z80/z80.h"
}

namespace mz {

class Mz2500 {
public:
    // P2 calibration: with the FDC read latency at its default, both EmuZ
    // and this core hit audio_boot at frame 562 and title BGM at frame 1804.
    static constexpr int DEFAULT_BOOT_DELAY_FRAMES = 249;

    enum class DiskBootProfile : uint8_t {
        NoDisk = 0,
        IplProCompatible = 1,
        RealIplRequired = 2,
        Invalid = 3,
    };

    Mz2500();

    // Two floppy drives (FD1 = drive 0, FD2 = drive 1). Inserting is a hot
    // swap: it never resets the machine, so mid-game disk changes work.
    bool insert_disk(int drive, const std::string& path);
    bool insert_disk_bytes(int drive, std::vector<uint8_t> bytes) {
        return disks_[drive & 1].load(std::move(bytes));
    }
    bool insert_disk(const std::string& path) { return insert_disk(0, path); }
    void eject_disk(int drive) { disks_[drive & 1].eject(); }
    bool disk_loaded(int drive) const { return disks_[drive & 1].loaded(); }
    DiskBootProfile disk_boot_profile(int drive) const {
        const D88Disk& disk = disks_[drive & 1];
        if (!disk.loaded()) return DiskBootProfile::NoDisk;
        if (disk.has_structural_error() || disk.has_unsupported_records())
            return DiskBootProfile::Invalid;
        return disk.is_iplpro_compatible()
            ? DiskBootProfile::IplProCompatible
            : DiskBootProfile::RealIplRequired;
    }

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

    // Front-panel RESET: pulse only the Z80 reset input. Unlike IPL, this
    // deliberately preserves RAM, bank mapping, video state, and peripheral
    // latches so resident system software can handle its reset vector.
    void system_reset();

    // User-provided ROM images (kind: 0=ipl, 1=retired/reserved, 2=kanji, 3=dict,
    // 4=MZ-1E30 SASI BIOS). Never
    // bundled; the owner supplies the files at runtime.
    void set_rom(int kind, const uint8_t* data, size_t size);

    // Expansion-board configuration (kind: 0=expansion RAM 256KB,
    // 1=expansion GRAM second screen, 2=MZ-1M10 4096-colour palette board,
    // 3=MZ-1E35 ADPCM board, 4=MZ-1R37 640K EMM, 5=MZ-1E30 SASI).
    // Takes effect immediately; RAM/GRAM changes want an IPL to be sane.
    void set_hw_option(int kind, bool on) {
        switch (kind) {
        case 0: mem_.set_expansion_ram(on); break;
        case 1: mem_.set_expansion_gram(on); break;
        case 2: mz1m10_present_ = on; break;
        case 3: adpcm_present_ = on; break;
        case 4: emm_present_ = on; break;
        case 5: sasi_present_ = on; break;
        }
    }
    bool has_ipl_rom() const { return mem_.has_ipl_rom(); }
    bool has_kanji_rom() const { return mem_.has_kanji_rom(); }

    // Rear-panel compatibility selector sampled by the IPL. Mode 0 is the
    // native MZ-2500 position, 1 is MZ-2000/2200 and 2 is MZ-80B. The
    // selected legacy modes run the Z80 at 4 MHz; firmware later programs
    // the independent memory (B7h) and display (CRTC register 0Fh) mode
    // latches. Set this before IPL/boot, as on the physical machine.
    void set_boot_mode(int mode);
    int boot_mode() const { return boot_mode_; }
    int memory_compat_mode() const;
    int display_compat_mode() const;
    int effective_display_mode() const;
    int frame_cycles() const;
    int frame_lines() const;
    int visible_lines() const;

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

    // Built-in data recorder. WAV preserves arbitrary pulse widths; standard
    // MZ logical images are encoded into the corresponding MZ-2000/MZ-80B
    // comparator waveform when inserted.
    bool insert_cmt_wav(const uint8_t* data, size_t size) {
        return cmt_.load_wav(data, size);
    }
    bool insert_cmt_mzf(const uint8_t* data, size_t size) {
        return cmt_.load_mzf(data, size, boot_mode_ == 2);
    }
    bool create_blank_cmt(uint32_t seconds) { return cmt_.create_blank(seconds, 22050); }
    void eject_cmt() { cmt_.eject(); }
    void cmt_manual_command(int command) {
        if (command >= 0 && command <= 3)
            cmt_.manual_command(static_cast<CmtDeck::Transport>(command), cpu_.cyc);
    }
    int cmt_transport() const { return static_cast<int>(cmt_.transport()); }
    bool cmt_loaded() const { return cmt_.tape_loaded(); }
    bool cmt_recording() const { return cmt_.recording(); }
    bool cmt_write_protected() const { return cmt_.write_protected(); }
    void set_cmt_write_protected(bool on) { cmt_.set_write_protected(on); }
    uint64_t cmt_position_ms() { return cmt_.position_ms(cpu_.cyc); }
    uint64_t cmt_duration_ms() const { return cmt_.duration_ms(); }
    bool cmt_dirty() const { return cmt_.dirty(); }
    void clear_cmt_dirty() { cmt_.clear_dirty(); }
    std::vector<uint8_t> cmt_wav_image() const { return cmt_.wav_image(); }
    CmtDeck& cmt() { return cmt_; }

    // Parallel printer capture. The host receives exactly the byte stream
    // accepted through FEh/FFh, without assuming a text encoding or printer
    // command language.
    const std::vector<uint8_t>& printer_output() const { return printer_.output(); }
    bool printer_dirty() const { return printer_.dirty(); }
    void clear_printer_dirty() { printer_.clear_dirty(); }
    void clear_printer_output() { printer_.clear_output(); }
    void set_printer_online(bool on) { printer_.set_online(on, cpu_.cyc); }
    bool printer_online() const { return printer_.online(); }
    uint64_t printer_dropped_bytes() const { return printer_.dropped_bytes(); }

    // MZ-1E30 SASI target. Images are raw logical blocks; block size is
    // explicit except for the well-known MZ-1F23 1024-byte image size.
    bool insert_sasi_image(const uint8_t* data, size_t size, uint32_t block_size) {
        return sasi_.load_image(data, size, block_size);
    }
    bool create_blank_sasi(size_t size, uint32_t block_size) {
        return sasi_.create_blank(size, block_size);
    }
    void eject_sasi() { sasi_.eject(); }
    bool sasi_loaded() const { return sasi_.loaded(); }
    uint32_t sasi_block_size() const { return sasi_.block_size(); }
    const std::vector<uint8_t>& sasi_image() const { return sasi_.image(); }
    bool sasi_dirty() const { return sasi_.dirty(); }
    void clear_sasi_dirty() { sasi_.clear_dirty(); }
    bool sasi_write_protected() const { return sasi_.write_protected(); }
    void set_sasi_write_protected(bool on) { sasi_.set_write_protected(on); }
    void set_sasi_target_id(uint8_t id) { sasi_.set_target_id(id); }
    uint8_t sasi_target_id() const { return sasi_.target_id(); }
    int sasi_phase() const { return static_cast<int>(sasi_.phase()); }

    bool set_adpcm_ram_size(uint32_t size) { return adpcm_.set_adpcm_ram_size(size); }
    uint32_t adpcm_ram_size() const { return adpcm_.adpcm_ram_size(); }
    void set_adpcm_gpio_inputs(uint8_t value) { adpcm_.set_gpio_inputs(value); }
    uint8_t adpcm_gpio_direction() const { return adpcm_.gpio_direction(); }
    uint8_t adpcm_gpio_outputs() const { return adpcm_.gpio_output_pins(); }
    uint8_t adpcm_gpio_pins() const { return adpcm_.gpio_pins(); }
    bool adpcm_adc_enabled() const { return adpcm_.adc_enabled(); }
    size_t queue_adpcm_input(const float* samples, size_t count, uint32_t rate) {
        return adpcm_present_ ? adpcm_.queue_adc_samples(samples, count, rate) : 0;
    }
    void clear_adpcm_input() { adpcm_.clear_adc_samples(); }
    void set_adpcm_mix_gain(float gain) { adpcm_.set_mix_gain(gain); }

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
    AdpcmY8950& adpcm() { return adpcm_; }

    // Final audio mix: the OPN stream is the master count, the ADPCM board
    // is added on top. Both resamplers target the same host rate and are
    // flushed to the same cycle at frame end, so their sample counts track
    // within a bounded few samples - the min-drain below never accumulates.
    size_t read_audio(float* out, size_t max_samples);

    // One knob for both chips: the mix above depends on the two resamplers
    // sharing a host rate, so the rate is set through here and never on a
    // single chip.
    void set_audio_rate(uint32_t rate) {
        opn_.set_output_rate(rate);
        adpcm_.set_output_rate(rate);
    }

    // Feed one complete character from an external serial line to the SIO.
    // The receiver's WR3 enable and Auto Enables/DCD state are honoured.
    void sio_receive(int channel, uint8_t value);
    bool sio_queue_receive(int channel, uint8_t value);
    bool sio_pop_transmitted(int channel, Z80Sio::TxByte& value) {
        return sio_.pop_transmitted(channel, value);
    }
    size_t sio_transmitted_available(int channel) const {
        return sio_.transmitted_available(channel);
    }
    uint32_t sio_baud(int channel) const { return sio_.baud(channel); }
    int sio_receive_bits(int channel) const { return sio_.receive_bits(channel); }
    int sio_transmit_bits(int channel) const { return sio_.transmit_bits(channel); }
    uint8_t sio_stop_half_bits(int channel) const {
        return sio_.stop_half_bits(channel);
    }
    Z80Sio::Parity sio_parity(int channel) const { return sio_.parity(channel); }
    bool sio_receiver_enabled(int channel) const {
        return sio_.receiver_enabled(channel);
    }
    bool sio_transmitter_enabled(int channel) const {
        return sio_.transmitter_enabled(channel);
    }
    bool sio_rs232_connected(int channel) const {
        return (channel & 1) == 0 || !mouse_connected();
    }
    bool sio_dtr(int channel) const { return sio_.dtr(channel); }
    bool sio_rts(int channel) const { return sio_.rts(channel); }
    bool sio_break_active(int channel) const {
        return sio_.break_active(channel);
    }
    void sio_set_modem_inputs(int channel, bool cts, bool dcd) {
        sio_.set_modem_inputs(channel, cts, dcd, false, cpu_.cyc);
    }

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
    void test_set_sio_dtr(int channel, bool high);
    void test_enable_sio_receiver(int channel);
    uint8_t test_sio_port_in(uint16_t port) {
        const uint8_t low = port & 0xFF;
        if ((low >= 0xA0 && low <= 0xA3) ||
            (low >= 0xB0 && low <= 0xB3))
            return io_in_raw(port);
        return 0xFF;
    }
    void test_sio_port_out(uint16_t port, uint8_t value) {
        const uint8_t low = port & 0xFF;
        if ((low >= 0xA0 && low <= 0xA3) ||
            (low >= 0xB0 && low <= 0xB3) || low == 0xCD)
            io_out(port, value);
    }
    void test_set_opn_port_a(uint8_t value) {
        opn_addr_ = 0x07;
        opn_regs_[0x07] |= 0x40;
        opn_.write_address(0x07, cpu_.cyc);
        opn_.write_data(opn_regs_[0x07], cpu_.cyc);
        opn_addr_ = 0x0E;
        opn_regs_[0x0E] = value;
        opn_.write_address(0x0E, cpu_.cyc);
        opn_.write_data(value, cpu_.cyc);
    }
    bool test_sio_channel_b_has_data() const { return sio_.rx_available(1); }
    uint8_t test_sio_channel_b_read_byte() { return sio_.read_data(1, cpu_.cyc); }

    // Test-only I/O access, narrowed to the option boards' ports (ACh/ADh
    // EMM, 98h/99h ADPCM). The mouse hooks above explain why the full I/O
    // map stays closed to tests.
    uint8_t test_option_board_in(uint16_t port) {
        const uint8_t low = port & 0xFF;
        if (low == 0xAD || low == 0x98 || low == 0x99) return io_in_raw(port);
        return 0xFF;
    }
    void test_option_board_out(uint16_t port, uint8_t value) {
        const uint8_t low = port & 0xFF;
        if (low == 0xAC || low == 0xAD || low == 0x98 || low == 0x99)
            io_out(port, value);
    }
    uint8_t test_cmt_port_in(uint16_t port) {
        return (port & 0xFF) >= 0xE0 && (port & 0xFF) <= 0xE3
            ? io_in_raw(port) : 0xFF;
    }
    void test_cmt_port_out(uint16_t port, uint8_t value) {
        if ((port & 0xFF) >= 0xE0 && (port & 0xFF) <= 0xE3)
            io_out(port, value);
    }
    uint8_t test_printer_port_in(uint16_t port) {
        return (port & 0xFF) == 0xFE ? io_in_raw(port) : 0xFF;
    }
    void test_printer_port_out(uint16_t port, uint8_t value) {
        if ((port & 0xFF) == 0xFE || (port & 0xFF) == 0xFF)
            io_out(port, value);
    }
    uint8_t test_sasi_port_in(uint16_t port) {
        const uint8_t low = port & 0xFF;
        return (low == 0xA4 || low == 0xA5 || low == 0xA9)
            ? io_in_raw(port) : 0xFF;
    }
    void test_sasi_port_out(uint16_t port, uint8_t value) {
        const uint8_t low = port & 0xFF;
        if (low == 0xA4 || low == 0xA5 || low == 0xA8)
            io_out(port, value);
    }
    uint8_t test_compat_port_in(uint16_t port) {
        const uint8_t low = port & 0xFF;
        return (low == 0xC9 || (low >= 0xF4 && low <= 0xF7))
            ? io_in_raw(port) : 0xFF;
    }
    void test_compat_port_out(uint16_t port, uint8_t value) {
        const uint8_t low = port & 0xFF;
        if (low == 0xB7 || low == 0xC8 || low == 0xC9 || low == 0xE8 ||
            (low >= 0xF4 && low <= 0xF7))
            io_out(port, value);
    }
    uint8_t test_compat_memory_read(uint16_t address);
    void test_compat_memory_write(uint16_t address, uint8_t value);
    void test_reset_peripherals() { reset_peripherals_for_boot(); }

    // Machine state snapshot as JSON (debug panel / future tooling).
    // Returns the number of bytes written (excluding the terminator).
    size_t debug_json(char* buf, size_t cap);

    // Decode the text layer to UTF-8, one line per displayed row, following
    // the same CRTC roll/page state as the renderer (renderer.cpp). Kanji-ROM
    // ANK cells come back as their characters; kanji cells and PCG art cells
    // as placeholders. Returns bytes written (excluding the terminator).
    size_t screen_text(char* buf, size_t cap) const;

    // Observability for external tooling (MCP server): the OPN register
    // shadow io_out keeps, the FM key-on slot masks tracked from reg 28h
    // writes, and the BEEP speaker line (8255 port C bit2).
    const uint8_t* opn_reg_shadow() const { return opn_regs_; }
    uint8_t fm_keyon(int ch) const { return fm_keyon_[ch % 3]; }
    bool beep_on() const { return ppi_port_c_high(2); }

    // Firmware forensics: when PC first reaches `addr`, dump the recent
    // execution and I/O history to stderr (CLI --trace-trap).
    void set_trap_watch(uint16_t addr) { trap_watch_ = addr; trap_hit_ = false; }
    void dump_forensics(const char* why);

private:
    // Reset CPU-external device state shared by both boot paths. Firmware
    // handoff values belong in boot_from_disk(), after this boot baseline;
    // boot_with_real_ipl() must let the ROM establish them itself, except for
    // the recorder PPI wiring needed by the IPL's pre-mode-set CMT path.
    void reset_peripherals_for_boot();
    static uint8_t cb_read(void* ud, uint16_t addr);
    static void cb_write(void* ud, uint16_t addr, uint8_t value);
    static uint8_t cb_in(z80* z, uint16_t port);
    static void cb_out(z80* z, uint16_t port, uint8_t value);
    static void cb_reti(z80* z);

    void charge_access_wait(int bank);
    // Is the controller fetching the layer this memory block feeds, on the
    // raster the CPU is on? Only then does it hold the bus long enough to
    // stall the access. See charge_access_wait() in mz2500.cpp.
    bool layer_scanning(int bank, int line) const;
    int display_stall_cycles_current() const;
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
    void clear_gvram_window();
    int decode_display_compat_mode(uint8_t mode_register) const;
    void render_compat_line(uint8_t* row, int y, int mode) const;
    bool compat_window(uint16_t addr, int& bank, uint16_t& offset) const;
    void charge_compat_vram_wait();
    void update_boot_sense_inputs();
    void io_out(uint16_t port, uint8_t value);
    uint8_t blank_flags() const; // port F4h read: bit0 VBLANK, bit1 HBLANK
    void log_port_once(uint16_t port, const char* dir);
    void service_interrupts();

    z80 cpu_{};
    BankedMemory mem_;
    D88Disk disks_[FdcMb8877::NUM_DRIVES];
    FdcMb8877 fdc_;
    OpnYm2203 opn_;
    AdpcmY8950 adpcm_;
    CmtDeck cmt_;
    PrinterPort printer_;
    SasiController sasi_;

    // register latches for devices that later phases bring to life
    uint8_t opn_addr_ = 0;
    uint8_t opn_regs_[256] = {};
    uint8_t fm_keyon_[3] = {};    // reg 28h slot masks per FM channel
    uint8_t crtc_index_ = 0;      // port F4h write
    // port F5h data (includes the graphic palette at 80h+). The object and
    // hardware-reset baseline is neutral; the native bootstrap supplies the
    // observed firmware handoff separately.
    uint8_t crtc_regs_[256] = {};
    // Has anything written the text display window pairs (03h/05h
    // vertical, 07h/08h horizontal)? A window programmed shut and a
    // window never programmed look the same in the registers, so the
    // renderer needs the distinction kept here (core/renderer.cpp).
    bool crtc_vwin_written_ = false;
    bool crtc_hwin_written_ = false;
    // port F6h: bit3 MG, bit2 GE, bit1 RE, bit0 BE (BE also gates the I
    // plane in 16-colour mode). The native bootstrap supplies the observed
    // firmware handoff value; a real-ROM boot starts from the neutral reset
    // baseline and lets the ROM program it.
    uint8_t cg_mask_ = 0;
    uint8_t font_size_ = 0;       // port F7h
    // Legacy character-controller registers reached through F4h-F7h once
    // CRTC register 0Fh changes the display decode. In 80B mode one byte
    // also drives the memory controller's display/access selection.
    uint8_t compat_vram_control_ = 0;
    uint8_t compat_background_ = 0;
    uint8_t compat_text_colour_ = 7;
    uint8_t compat_graphics_mask_ = 0;
    uint8_t gde_index_ = 0;       // port BCh register number (7 bits)
    bool gde_autoinc_ = false;    // BCh bit7: bump reg number after each write
    uint8_t gde_regs_[32] = {};   // port BDh, internal registers 00-1Fh
    uint64_t gde_busy_until_ = 0; // hardware GRAM clear in progress
    uint8_t gvram_latch_[4] = {}; // ports BCh-BFh: last plane bytes read
    int current_line() const;
    int frame_line_start(int line) const;
    // The browser presents a completed frame after the CPU has already run
    // all of it. Keep the display-facing register and palette state that was
    // in force on each visible raster so a mid-frame write does not rewrite
    // the rows that the real video circuitry has already scanned.
    struct RasterLineState {
        uint8_t crtc[256] = {};
        uint8_t gde[32] = {};
        uint8_t palette[32] = {};
        uint8_t cg_mask = 0;
        uint8_t font_size = 0;
        uint8_t pio_a = 0;
        uint8_t ppi_a = 0;
        uint8_t ppi_control = 0x9B;
        uint8_t ppi_c = 0;
        uint8_t opn_port_a = 0;
        uint8_t compat_vram_control = 0;
        uint8_t compat_background = 0;
        uint8_t compat_text_colour = 7;
        uint8_t compat_graphics_mask = 0;
        bool crtc_vwin_written = false;
        bool crtc_hwin_written = false;
        bool palette_written = false;
        bool opn_port_a_output = false;
    };
    RasterLineState raster_line_[VBLANK_START_LINE] = {};
    void seed_raster_lines();
    int raster_write_start_line() const;

    // Z80B SIO. Channel A is the 9-pin RS-232C port; channel B is the
    // 25-pin port or the mouse selected by OPN port A bit3.
    Z80Sio sio_;
    Mouse mouse_;
    // The mouse shares channel B with the 25-pin RS-232C port; OPN port A
    // bit3 is the switch (Oh!MZ p299). With the switch open the line is
    // empty, which is what an unplugged port looks like.
    bool mouse_connected() const {
        return opn_.port_a_is_output() && (opn_.port_a_pins() & 0x08) != 0;
    }
    void sio_dtr_changed(int ch, bool dtr);
    void sio_write_control(int ch, uint8_t value);
    bool decode_sio_port(uint8_t port, int& channel, bool& control) const;
    void write_sio_clock_control(uint8_t value);
    bool sio_dtr_[2] = {false, false};
    uint8_t sio_clock_control_ = 0; // CDh: address select + A/B TxRxC dividers
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
        bool loaded = false;
        bool counting = false;
        bool terminal = false;
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
    void pit_start_counter(int ch);
    void pit_gate_strobe();
    uint8_t pio_a_ = 0;           // port E8h latch
    uint8_t bank_mode_ = 0;       // port B7h latch
    int boot_mode_ = 0;           // rear-panel selector: native/2000/80B
    uint8_t ppi_[3] = {};         // 8255 A/B/C latches (E0h-E2h)
    uint8_t ppi_control_ = 0x9B;  // reset: mode 0, all ports are inputs
    bool ppi_port_a_output() const { return (ppi_control_ & 0x10) == 0; }
    bool ppi_port_b_output() const { return (ppi_control_ & 0x02) == 0; }
    bool ppi_port_c_output(int bit) const {
        return (ppi_control_ & (bit < 4 ? 0x01 : 0x08)) == 0;
    }
    bool ppi_port_c_high(int bit) const {
        return ppi_port_c_output(bit) && (ppi_[2] & (1 << bit)) != 0;
    }
    void ppi_write_control(uint8_t value);
    void update_ppi_outputs();
    uint8_t pio_ctrl_[2] = {};    // Z80 PIO control words (E9h/EBh)
    bool mz1m10_present_ = true;  // 4096-colour palette board option
    bool adpcm_present_ = true;   // MZ-1E35 ADPCM board option
    bool emm_present_ = true;     // MZ-1R37 640K EMM option
    bool sasi_present_ = true;    // MZ-1E30 SASI interface option
    Emm emm_;
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
    // cpu_.cyc is the 6 MHz machine-time axis shared by video, sound and
    // peripherals. A legacy-mode Z80 T-state occupies 1.5 of those ticks;
    // this remainder preserves the half tick between instructions. Waits
    // added by memory callbacks are already machine-time ticks and are not
    // scaled a second time.
    uint8_t cpu_half_cycle_ = 0;
    uint64_t step_external_wait_ = 0;
    bool cpu_step_active_ = false;
    // A hardware IPL starts the ROM before its RAM check has completed. Keep
    // the immediate post-reset state observable, then apply that check just
    // before the first emulated IPL frame executes.
    bool real_ipl_ram_init_pending_ = false;
    int boot_delay_frames_ = DEFAULT_BOOT_DELAY_FRAMES;
    int idle_frames_remaining_ = 0;
    bool trace_boot_ = false;
    bool trace_io_ = false;
    bool warned_in_[256] = {};
    bool warned_out_[256] = {};
};

} // namespace mz
