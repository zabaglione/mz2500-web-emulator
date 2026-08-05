// Native replacement for the MZ-2500 IPL ROM boot sequence.
//
// This is host-side code, not Z80 code, and contains no ROM-derived bytes.
// It re-implements the documented IPLPRO floppy boot convention used by this
// repository's build tools (games/neko_can_run/tools/make_d88.py,
// shared/mz2500/d88.py):
//
//   LBA 16 (cyl 0 / side 1 / sector 1): header 01h "IPLPRO", 12-byte title,
//     [0x18:0x1A] load & entry address, [0x20] physical bank for the payload,
//     [0x30:0x37] CPU block 0-6 bank assignments
//   LBA 0-15 and 32-47: the 8KB boot payload
//
// It also establishes the initial device state the real IPL leaves behind:
// CPU block 7 mapped to bank 0Fh, kanji bank register = 0 (bank 39h reads as
// PCG RAM), PCG/VRAM zero-cleared, interrupts masked.
#include <cstdio>
#include <cstring>

#include "core/mz1m10.h"
#include "core/mz2500.h"

namespace mz {

bool Mz2500::boot_from_disk() {
    D88Disk& boot_disk = disks_[0]; // the IPL boots from drive FD1
    if (!boot_disk.loaded()) {
        std::fprintf(stderr, "[ipl] no disk mounted\n");
        return false;
    }

    uint8_t header[D88Disk::SECTOR_SIZE];
    if (!boot_disk.read_decoded(16, header)) {
        std::fprintf(stderr, "[ipl] cannot read boot header sector (LBA 16)\n");
        return false;
    }
    if (header[0] != 0x01 || std::memcmp(header + 1, "IPLPRO", 6) != 0) {
        std::fprintf(stderr, "[ipl] not a bootable disk (IPLPRO signature missing)\n");
        return false;
    }

    mem_.clear(); // main RAM, VRAM, PCG all zero
    fdc_.reset();
    opn_.reset();
    mouse_.reset(); // no pending movement/buttons must survive a reboot
    // The DTR edge-tracking state must also start fresh: if it stayed at
    // whatever level the driver last left it before the reboot, the first
    // post-reboot write to reach that same level would be seen as "no
    // edge" and swallow the driver's first packet request.
    sio_dtr_[0] = sio_dtr_[1] = false;
    // A three-byte mouse packet queued but not yet drained (e.g. the driver
    // strobed DTR, then the host reset mid-frame before reading all three
    // bytes back) must not survive a reboot either: the fresh driver's first
    // read of channel B would get the OLD packet's leftover byte(s) instead
    // of its own, desynchronising its packet phase for however long it takes
    // to notice.
    sio_[0].rx_head = sio_[0].rx_tail = 0;
    sio_[1].rx_head = sio_[1].rx_tail = 0;
    pit_counting_ = false;
    for (auto& c : pit_) c = PitChannel{};
    for (auto& p : int_pending_) p = false;
    for (auto& v : int_vectors_) v = 0;
    tick_next_[0] = tick_next_[1] = 0;
    pio_a_ = 0;
    joy_mask_ = 0;
    std::memset(crtc_regs_, 0, sizeof(crtc_regs_));
    // CRTC reg 00h as real firmware leaves it: 25 rows, one text
    // page, 8-colour text over 16-colour graphics.
    crtc_regs_[0x00] = 0x05;
    // ...and with it the record of who owns the text display windows: a
    // freshly reset CRTC register file has never been programmed, so the
    // renderer shows the full screen rather than the shut window that
    // SL = EL / SC = EC would otherwise mean (core/renderer.cpp).
    crtc_vwin_written_ = false;
    crtc_hwin_written_ = false;
    // port F6h graphic mask: all three guns on, matching what the real IPL
    // leaves behind.
    cg_mask_ = 0x07;
    std::memset(gde_regs_, 0, sizeof(gde_regs_));
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
    std::memset(opn_regs_, 0, sizeof(opn_regs_));

    const uint8_t payload_bank = header[0x20] & 0x3F;
    uint8_t* dest = mem_.bank_ptr(payload_bank);
    for (int i = 0; i < 16; i++) {
        if (!boot_disk.read_decoded(i, dest + i * D88Disk::SECTOR_SIZE)) return false;
    }
    for (int i = 0; i < 16; i++) {
        if (!boot_disk.read_decoded(32 + i, dest + (16 + i) * D88Disk::SECTOR_SIZE)) return false;
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
    cpu_.userdata = this;
    cpu_.pc = static_cast<uint16_t>(header[0x18] | (header[0x19] << 8));
    frame_origin_ = 0;
    frames_ = 0;
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

    mem_.clear(); // RAM/VRAM/PCG zero; the ROM banks survive
    fdc_.reset();
    opn_.reset();
    mouse_.reset(); // no pending movement/buttons must survive a reboot
    // The DTR edge-tracking state must also start fresh: if it stayed at
    // whatever level the driver last left it before the reboot, the first
    // post-reboot write to reach that same level would be seen as "no
    // edge" and swallow the driver's first packet request.
    sio_dtr_[0] = sio_dtr_[1] = false;
    // A three-byte mouse packet queued but not yet drained (e.g. the driver
    // strobed DTR, then the host reset mid-frame before reading all three
    // bytes back) must not survive a reboot either: the fresh driver's first
    // read of channel B would get the OLD packet's leftover byte(s) instead
    // of its own, desynchronising its packet phase for however long it takes
    // to notice.
    sio_[0].rx_head = sio_[0].rx_tail = 0;
    sio_[1].rx_head = sio_[1].rx_tail = 0;
    pit_counting_ = false;
    for (auto& c : pit_) c = PitChannel{};
    for (auto& p : int_pending_) p = false;
    for (auto& v : int_vectors_) v = 0;
    tick_next_[0] = tick_next_[1] = 0;
    pio_a_ = 0;
    joy_mask_ = 0;
    std::memset(crtc_regs_, 0, sizeof(crtc_regs_));
    // CRTC reg 00h as real firmware leaves it: 25 rows, one text
    // page, 8-colour text over 16-colour graphics.
    crtc_regs_[0x00] = 0x05;
    // ...and with it the record of who owns the text display windows: a
    // freshly reset CRTC register file has never been programmed, so the
    // renderer shows the full screen rather than the shut window that
    // SL = EL / SC = EC would otherwise mean (core/renderer.cpp).
    crtc_vwin_written_ = false;
    crtc_hwin_written_ = false;
    // port F6h graphic mask: all three guns on, matching what the real IPL
    // leaves behind.
    cg_mask_ = 0x07;
    std::memset(gde_regs_, 0, sizeof(gde_regs_));
    // The palette board keeps its contents across a reset; on this path the
    // firmware about to run programs the table itself, so seeding it with
    // the same table (core/mz1m10.h) only makes the two boot paths agree.
    mz1m10_firmware_palette(palette_);
    // ...and the flag that says a program has written the board, which the
    // renderer needs before it routes the screen through the RGB444
    // entries. A reboot must clear it, or a program that never touches the
    // board would inherit the previous one's palette selection.
    palette_written_ = false;
    std::memset(opn_regs_, 0, sizeof(opn_regs_));
    int_select_ = 0;
    mem_.set_kanji_bank(0);

    // hardware reset bank map: IPL ROM at 0000-7FFF, RAM 04-07 above
    static const uint8_t reset_map[8] = {0x34, 0x35, 0x36, 0x37, 0x04, 0x05, 0x06, 0x07};
    for (int block = 0; block < 8; block++) mem_.set_map(block, reset_map[block]);

    z80_init(&cpu_);
    cpu_.read_byte = cb_read;
    cpu_.write_byte = cb_write;
    cpu_.port_in = cb_in;
    cpu_.port_out = cb_out;
    cpu_.userdata = this;
    cpu_.pc = 0x0000;
    frame_origin_ = 0;
    frames_ = 0;
    idle_frames_remaining_ = 0; // the real firmware takes its real time

    if (trace_boot_) std::fprintf(stderr, "[ipl] real IPL boot, PC=0000h\n");
    return true;
}

} // namespace mz
