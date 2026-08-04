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
    // kind: 0 = IPL (32KB -> banks 34h-37h), 2 = kanji (bank 39h window
    // paged by CFh), 3 = dictionary (bank 3Ah window paged by CEh).
    void load_ipl_rom(const uint8_t* data, size_t size);
    void load_kanji_rom(const uint8_t* data, size_t size) { kanji_rom_.assign(data, data + size); }
    const std::vector<uint8_t>& kanji_rom() const { return kanji_rom_; }
    void load_dict_rom(const uint8_t* data, size_t size) { dict_rom_.assign(data, data + size); }
    bool has_ipl_rom() const { return ipl_rom_loaded_; }

    // Expansion hardware presence (real machines shipped without these):
    // expansion RAM = banks 10h-1Fh (the 128KB->256KB upgrade), expansion
    // GRAM = the MZ-1R27 board, banks 28h-2Fh, which doubles the graphics
    // V-RAM to 128KB and is what 640x400x16 needs. Banks 20h-27h are the
    // standard 64KB and are always present - Oh!MZ's bank table names the
    // 30h/31h read-modify-write window "standard" and 32h/33h, the window
    // onto 28h-2Fh, "option". Absent banks read FFh and swallow writes,
    // like empty sockets.
    void set_expansion_ram(bool on) { expansion_ram_ = on; }
    void set_expansion_gram(bool on) { expansion_gram_ = on; }

private:
    std::vector<uint8_t> phys_;
    std::vector<uint8_t> kanji_rom_;
    std::vector<uint8_t> dict_rom_;
    uint8_t map_[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t selector_ = 0;
    uint8_t kanji_bank_ = 0;
    uint8_t dict_bank_ = 0;
    bool ipl_rom_loaded_ = false;
    bool expansion_ram_ = true;
    bool expansion_gram_ = true;
    bool warned_rom_bank_[NUM_BANKS] = {};

    bool bank_absent(int bank) const {
        if (!expansion_ram_ && bank >= 0x10 && bank <= 0x1F) return true;
        if (!expansion_gram_ && bank >= 0x28 && bank <= 0x2F) return true;
        return false;
    }
};

} // namespace mz
