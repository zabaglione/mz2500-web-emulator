// MZ-2500 banked memory: the CPU sees 8 blocks of 8KB, each mapped to one of
// 64 physical 8KB banks via ports B4h (block selector) and B5h (bank value).
//
// Bank usage on this machine (as far as NEKO CAN RUN is concerned):
//   00h-1Fh main RAM (256KB)   20h-27h CG VRAM planes
//   38h     text VRAM          39h     PCG RAM
//   34h-37h IPL ROM, 3Ah dictionary ROM, 3Ch-3Fh phone ROM — absent here;
//   reads return FFh and are logged once (clean-room build carries no ROMs).
//
// Quirk faithfully implemented: reading OR writing port B5h auto-increments
// the block selector (games/neko_can_run/tools/platform_engine.asm:392
// depends on this when saving/restoring the block-5 mapping).
//
// Bank 39h reads are gated by kanji-bank register CFh bit7: 0 = PCG RAM
// (the reset state the dummy IPL guarantees), 1 = kanji ROM (absent).
#pragma once

#include <cstdint>
#include <vector>

namespace mz {

class BankedMemory {
public:
    static constexpr int BANK_SIZE = 0x2000;
    static constexpr int NUM_BANKS = 0x40;

    BankedMemory() : phys_(NUM_BANKS * BANK_SIZE, 0) {}

    void clear();
    // The real IPL's RAM check initializes the main RAM before handing
    // control to the boot program. Video RAM and PCG are separate storage
    // and are intentionally not touched by this operation.
    void clear_main_ram();
    // Test-only equivalent of clear_main_ram(): fill the physical main-RAM
    // banks, including the optional expansion banks, without touching VRAM,
    // PCG, ROM, or device state.
    void fill_main_ram(uint8_t value);
    // Test-only: fill EVERY writable physical bank - main RAM, GVRAM,
    // TVRAM and PCG - with seeded pseudo-random bytes. Only the IPL ROM
    // banks (34h-37h) are spared. Models a machine whose RAM content is
    // arbitrary at power-on: nothing may rely on any initial value.
    void fill_all_ram_random(uint32_t seed) {
        uint32_t x = seed ? seed : 0xA5A5A5A5u;
        for (int bank = 0; bank < 0x40; bank++) {
            if (bank >= 0x34 && bank <= 0x37) continue;  // IPL ROM
            uint8_t* p = phys_.data() + bank * BANK_SIZE;
            for (int i = 0; i < BANK_SIZE; i++) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                p[i] = static_cast<uint8_t>(x);
            }
        }
    }
    void set_test_main_ram_fill(uint8_t value) {
        test_main_ram_fill_enabled_ = true;
        test_main_ram_fill_value_ = value;
    }
    void reset_control() {
        selector_ = 0;
        kanji_bank_ = 0;
        dict_bank_ = 0;
    }

    uint8_t read(uint16_t addr);
    void write(uint16_t addr, uint8_t value);

    uint8_t* bank_ptr(int bank) { return phys_.data() + bank * BANK_SIZE; }
    const uint8_t* bank_ptr(int bank) const { return phys_.data() + bank * BANK_SIZE; }

    void set_map(int block, uint8_t bank) { map_[block & 7] = bank & 0x3F; }
    uint8_t map_of(int block) const { return map_[block & 7]; }

    // port B4h
    void out_b4(uint8_t v) { selector_ = v & 7; }
    uint8_t in_b4() const { return selector_; }
    // port B5h (selector auto-increments on both read and write)
    void out_b5(uint8_t v) {
        map_[selector_] = v & 0x3F;
        selector_ = (selector_ + 1) & 7;
    }
    uint8_t in_b5() {
        const uint8_t v = map_[selector_];
        selector_ = (selector_ + 1) & 7;
        return v;
    }

    void set_kanji_bank(uint8_t v) { kanji_bank_ = v; }
    uint8_t kanji_bank() const { return kanji_bank_; }
    void set_dict_bank(uint8_t v) { dict_bank_ = v; }

    // User-provided ROM images (never bundled; the owner supplies files).
    // kind: 0 = IPL (32KB -> banks 34h-37h), 1 = retired CG slot kept only
    // to preserve the public kind numbering, 2 = kanji (bank 39h window
    // paged by CFh and compatibility glyph source), 3 = dictionary
    // (bank 3Ah window paged by CEh).
    void load_ipl_rom(const uint8_t* data, size_t size);
    void load_kanji_rom(const uint8_t* data, size_t size) { kanji_rom_.assign(data, data + size); }
    const std::vector<uint8_t>& kanji_rom() const { return kanji_rom_; }
    void load_dict_rom(const uint8_t* data, size_t size) { dict_rom_.assign(data, data + size); }
    bool has_ipl_rom() const { return ipl_rom_loaded_; }
    bool has_kanji_rom() const { return !kanji_rom_.empty(); }

    // Expansion hardware presence (real machines shipped without these):
    // expansion RAM = banks 10h-1Fh (the 128KB->256KB upgrade), expansion
    // GRAM = the MZ-1R27 board, banks 28h-2Fh, which doubles the graphics
    // V-RAM to 128KB and is what 640x400x16 needs. Banks 20h-27h are the
    // standard 64KB and are always present - Oh!MZ's bank table names the
    // 30h/31h read-modify-write window "standard" and 32h/33h, the window
    // onto 28h-2Fh, "option". Absent banks read FFh and swallow writes,
    // like empty sockets.
    void set_expansion_ram(bool on) { expansion_ram_ = on; }
    // Diagnostic-only model for an absent expansion card whose bank selects
    // mirror the installed 00h-0Fh RAM instead of behaving as open bus.
    void set_absent_main_ram_alias(bool on) { absent_main_ram_alias_ = on; }
    void set_expansion_gram(bool on) { expansion_gram_ = on; }
    bool bank_present(int bank) const { return !bank_absent(bank & 0x3F); }

private:
    std::vector<uint8_t> phys_;
    std::vector<uint8_t> kanji_rom_;
    std::vector<uint8_t> dict_rom_;
    uint8_t map_[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t selector_ = 0;
    uint8_t kanji_bank_ = 0;
    uint8_t dict_bank_ = 0;
    bool ipl_rom_loaded_ = false;
    bool test_main_ram_fill_enabled_ = false;
    uint8_t test_main_ram_fill_value_ = 0;
    bool expansion_ram_ = true;
    bool absent_main_ram_alias_ = false;
    bool expansion_gram_ = true;
    bool warned_rom_bank_[NUM_BANKS] = {};

    bool bank_absent(int bank) const {
        if (!expansion_ram_ && bank >= 0x10 && bank <= 0x1F &&
            !absent_main_ram_alias_) return true;
        if (!expansion_gram_ && bank >= 0x28 && bank <= 0x2F) return true;
        return false;
    }
};

} // namespace mz
