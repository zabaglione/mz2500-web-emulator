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
    pit_counting_ = false;
    for (auto& c : pit_) c = PitChannel{};
    for (auto& p : int_pending_) p = false;
    for (auto& v : int_vectors_) v = 0;
    tick_next_[0] = tick_next_[1] = 0;
    pio_a_ = 0;
    joy_mask_ = 0;
    std::memset(crtc_regs_, 0, sizeof(crtc_regs_));
    std::memset(gde_regs_, 0, sizeof(gde_regs_));
    std::memset(palette_, 0, sizeof(palette_));
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
    pit_counting_ = false;
    for (auto& c : pit_) c = PitChannel{};
    for (auto& p : int_pending_) p = false;
    for (auto& v : int_vectors_) v = 0;
    tick_next_[0] = tick_next_[1] = 0;
    pio_a_ = 0;
    joy_mask_ = 0;
    std::memset(crtc_regs_, 0, sizeof(crtc_regs_));
    std::memset(gde_regs_, 0, sizeof(gde_regs_));
    std::memset(palette_, 0, sizeof(palette_));
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
