// Native replacement for the MZ-2500 IPL ROM boot sequence.
//
// This is host-side code, not Z80 code, and contains no ROM-derived bytes.
// It re-implements the documented IPLPRO floppy boot convention used by this
// repository's build tools (games/neko_can_run/tools/make_d88.py,
// shared/mz2500/d88.py):
//
//   C=0/H=1/R=1: header 01h "IPLPRO", 12-byte title,
//     [0x18:0x1A] load & entry address, [0x20] physical bank for the payload,
//     [0x30:0x37] CPU block 0-6 bank assignments
//   C=0/H=0/R=1..16 and C=1/H=0/R=1..16: the 8KB boot payload
//
// It also establishes the initial device state the real IPL leaves behind:
// CPU block 7 mapped to bank 0Fh, kanji bank register = 0 (bank 39h reads as
// PCG RAM), PCG/VRAM zero-cleared, interrupts masked.
#include <cstdio>
#include <cstring>

#include "core/mz1m10.h"
#include "core/mz2500.h"

namespace mz {

void Mz2500::reset_peripherals_for_boot() {
    // IPL starts a new machine-time epoch. Establish it before any device
    // reset calls update_ppi_outputs(), which flushes the OPN to cpu_.cyc.
    // Leaving the previous session's cycle count here made a freshly reset
    // OPN regenerate that entire elapsed interval as stale audio.
    cpu_.cyc = 0;
    fdc_.reset();
    opn_.reset();
    update_boot_sense_inputs();
    adpcm_.reset();
    opn_addr_ = 0;
    std::memset(opn_regs_, 0, sizeof(opn_regs_));
    std::memset(fm_keyon_, 0, sizeof(fm_keyon_));

    mouse_.reset();
    sio_.reset();
    sio_dtr_[0] = sio_dtr_[1] = false;
    write_sio_clock_control(0);

    pit_counting_ = false;
    for (auto& channel : pit_) channel = PitChannel{};
    pit_next_fire_ = 0;
    for (auto& pending : int_pending_) pending = false;
    for (auto& vector : int_vectors_) vector = 0;
    int_select_ = 0;
    tick_next_[0] = tick_next_[1] = 0;
    last_vblank_frame_ = ~0ULL;

    pio_a_ = 0;
    std::memset(pio_ctrl_, 0, sizeof(pio_ctrl_));
    std::memset(ppi_, 0, sizeof(ppi_));
    // The firmware's no-media CMT path sends STOP/PLAY through port A
    // without first writing a mode-set word. Start the IPL-visible recorder
    // wiring in its normal configuration: A/C output and B input.
    ppi_control_ = 0x82;
    cmt_.reset(0);       // media and head position survive RESET; motor stops
    update_ppi_outputs();
    printer_.reset(0);   // external output already accepted survives RESET
    sasi_.reset_machine(); // target media and option ROM survive RESET
    bank_mode_ = 0;
    compat_vram_control_ = 0;
    compat_background_ = 0;
    compat_text_colour_ = 7;
    compat_graphics_mask_ = 0;
    joy_enable_ = 0;
    joy_mask_ = 0;

    crtc_index_ = 0;
    font_size_ = 0;
    std::memset(crtc_regs_, 0, sizeof(crtc_regs_));
    crtc_vwin_written_ = false;
    crtc_hwin_written_ = false;
    cg_mask_ = 0;

    std::memset(gde_regs_, 0, sizeof(gde_regs_));
    gde_index_ = 0;
    gde_autoinc_ = false;
    gde_busy_until_ = 0;
    std::memset(gvram_latch_, 0, sizeof(gvram_latch_));
    for (auto& line : raster_line_) line = RasterLineState{};
    cpu_half_cycle_ = 0;
    step_external_wait_ = 0;
    cpu_step_active_ = false;
}

bool Mz2500::boot_from_disk() {
    D88Disk& boot_disk = disks_[0]; // the IPL boots from drive FD1
    if (!boot_disk.loaded()) {
        std::fprintf(stderr, "[ipl] no disk mounted\n");
        return false;
    }

    uint8_t header[D88Disk::SECTOR_SIZE];
    if (!boot_disk.read_decoded(0, 1, 1, header, sizeof(header))) {
        std::fprintf(stderr, "[ipl] cannot read boot header sector (C=0 H=1 R=1)\n");
        return false;
    }
    if (header[0] != 0x01 || std::memcmp(header + 1, "IPLPRO", 6) != 0) {
        std::fprintf(stderr, "[ipl] not a bootable disk (IPLPRO signature missing)\n");
        return false;
    }

    mem_.clear(); // native bootstrap policy: clear main RAM, VRAM, and PCG
    reset_peripherals_for_boot();

    // The native bootstrap replaces firmware which programs the 8255 to
    // MZ-2500 mode: A and both halves of C output, B input (82h).
    ppi_control_ = 0x82;
    update_ppi_outputs();
    // CRTC reg 00h as real firmware leaves it: 25 rows, one text
    // page, 8-colour text over 16-colour graphics.
    crtc_regs_[0x00] = 0x05;
    // port F6h graphic mask: all three guns on, matching what the real IPL
    // leaves behind.
    cg_mask_ = 0x07;
    // MZ-1M10 palette RAM as the firmware leaves it (core/mz1m10.h). It is
    // not cleared: the board keeps whatever was written to it, and the
    // firmware writes a full table on the way through, so a program that
    // programs only the entries it uses finds the rest already sensible.
    // Zeroing it here instead put every unwritten entry at black, and the
    // renderer switches the whole screen - text included - to the board as
    // soon as one entry is written, so such a program came up blank.
    mz1m10_firmware_palette(palette_);
    // ...and the flag that says a *program* has written the board. The
    // renderer routes the screen through the RGB444 entries only from then
    // on, so until it flips, a machine whose program never touches the
    // board (one running in its "no palette board" mode, say) shows the
    // fixed digital colours rather than the table above.
    palette_written_ = false;
    // This path skips the real firmware, so provide the handoff observed
    // from a real-ROM boot: mixer/direction 7Fh (port A output, port B input)
    // and port A 04h (digital palette). Keep these platform bootstrap writes
    // out of the program-write trace.
    opn_.set_ssg_io_handoff(0x7F, 0x04);
    opn_regs_[0x07] = 0x7F;
    opn_regs_[0x0E] = 0x04;

    const uint8_t payload_bank = header[0x20] & 0x3F;
    uint8_t* dest = mem_.bank_ptr(payload_bank);
    for (int r = 1; r <= D88Disk::SECTORS_PER_TRACK; r++) {
        if (!boot_disk.read_decoded(0, 0, r,
                                    dest + (r - 1) * D88Disk::SECTOR_SIZE,
                                    D88Disk::SECTOR_SIZE))
            return false;
        if (!boot_disk.read_decoded(1, 0, r,
                                    dest + (D88Disk::SECTORS_PER_TRACK + r - 1) *
                                        D88Disk::SECTOR_SIZE,
                                    D88Disk::SECTOR_SIZE))
            return false;
    }

    for (int block = 0; block < 7; block++) mem_.set_map(block, header[0x30 + block]);
    mem_.set_map(7, 0x0F);
    mem_.set_kanji_bank(0);
    int_select_ = 0;
    // the real IPL leaves an identity text CLUT behind (text works from the
    // firmware onward); the game only rewrites it in set_palette
    for (int i = 0; i < 16; i++) crtc_regs_[0x80 + i] = static_cast<uint8_t>(i);

    // CPU state at the IPL's jump: interrupts disabled, IM 0.
    z80_init(&cpu_);
    cpu_.read_byte = cb_read;
    cpu_.write_byte = cb_write;
    cpu_.port_in = cb_in;
    cpu_.port_out = cb_out;
    cpu_.reti = cb_reti;
    cpu_.userdata = this;
    cpu_.pc = static_cast<uint16_t>(header[0x18] | (header[0x19] << 8));
    frame_origin_ = 0;
    frames_ = 0;
    cpu_half_cycle_ = 0;
    idle_frames_remaining_ = boot_delay_frames_;

    if (trace_boot_) {
        char title[13] = {};
        std::memcpy(title, header + 7, 12);
        std::fprintf(stderr,
                     "[ipl] IPLPRO \"%s\" -> 8KB into bank %02Xh, blocks "
                     "0-6 = %02X %02X %02X %02X %02X %02X %02X, PC=%04Xh\n",
                     title, payload_bank, header[0x30], header[0x31], header[0x32],
                     header[0x33], header[0x34], header[0x35], header[0x36], cpu_.pc);
    }
    return true;
}

bool Mz2500::boot_with_real_ipl() {
    if (!mem_.has_ipl_rom()) {
        std::fprintf(stderr, "[ipl] no IPL ROM loaded\n");
        return false;
    }

    // RESET changes the memory-controller latches, not RAM cells. The real
    // IPL now gets to perform its own RAM test and optional GRAM/PCG setup;
    // this also preserves the documented reset-without-GRAM-clear path.
    mem_.reset_control();
    reset_peripherals_for_boot();

    // Do not pre-apply values observed at the end of firmware startup here.
    // CRTC 00h=05h, F6h=07h, and OPN 07h/0Eh are firmware handoff state.
    // The recorder PPI's 82h boot baseline is the exception: the real IPL's
    // no-media path sends its STOP/PLAY sequence before any PPI mode-set.
    // Every later value is still established by the ROM in emulated time.
    // MZ-1M10 palette RAM is not on the CPU RESET line. Keep both its bytes
    // and the fact that it has been programmed; the real IPL may overwrite
    // the table, but the machine core must not do that before the ROM runs.

    // hardware reset bank map: IPL ROM at 0000-7FFF, RAM 04-07 above
    static const uint8_t reset_map[8] = {0x34, 0x35, 0x36, 0x37, 0x04, 0x05, 0x06, 0x07};
    for (int block = 0; block < 8; block++) mem_.set_map(block, reset_map[block]);

    z80_init(&cpu_);
    cpu_.read_byte = cb_read;
    cpu_.write_byte = cb_write;
    cpu_.port_in = cb_in;
    cpu_.port_out = cb_out;
    cpu_.reti = cb_reti;
    cpu_.userdata = this;
    cpu_.pc = 0x0000;
    frame_origin_ = 0;
    frames_ = 0;
    cpu_half_cycle_ = 0;
    idle_frames_remaining_ = 0; // the real firmware takes its real time

    if (trace_boot_) std::fprintf(stderr, "[ipl] real IPL boot, PC=0000h\n");
    return true;
}

} // namespace mz
