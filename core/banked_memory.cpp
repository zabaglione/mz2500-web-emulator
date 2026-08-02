#include "core/banked_memory.h"

#include <cstdio>
#include <cstring>

namespace mz {

namespace {
bool is_rom_bank(int bank) {
    return (bank >= 0x34 && bank <= 0x37) || bank == 0x3A || (bank >= 0x3C && bank <= 0x3F);
}
} // namespace

void BankedMemory::clear() {
    // ROM banks (34h-37h) keep their contents across a reset
    for (int bank = 0; bank < NUM_BANKS; bank++) {
        if (!(ipl_rom_loaded_ && bank >= 0x34 && bank <= 0x37))
            std::memset(phys_.data() + bank * BANK_SIZE, 0, BANK_SIZE);
    }
    selector_ = 0;
    kanji_bank_ = 0;
    dict_bank_ = 0;
}

void BankedMemory::load_ipl_rom(const uint8_t* data, size_t size) {
    if (size > 0x8000) size = 0x8000;
    std::memset(bank_ptr(0x34), 0xFF, 4 * BANK_SIZE);
    std::memcpy(bank_ptr(0x34), data, size);
    ipl_rom_loaded_ = true;
}

uint8_t BankedMemory::read(uint16_t addr) {
    const int bank = map_[addr >> 13];
    if (bank == 0x39 && (kanji_bank_ & 0x80)) {
        if (kanji_rom_.empty()) {
            if (!warned_rom_bank_[bank]) {
                warned_rom_bank_[bank] = true;
                std::fprintf(stderr, "[mem] kanji ROM selected (CFh bit7=1) but none loaded\n");
            }
            return 0xFF;
        }
        // experimental paging: CFh low bits select an 8KB page of the ROM
        const size_t off = ((size_t)(kanji_bank_ & 0x1F) * BANK_SIZE + (addr & 0x1FFF)) %
                           kanji_rom_.size();
        return kanji_rom_[off];
    }
    if (bank == 0x3A && !dict_rom_.empty()) {
        const size_t off = ((size_t)(dict_bank_ & 0x1F) * BANK_SIZE + (addr & 0x1FFF)) %
                           dict_rom_.size();
        return dict_rom_[off];
    }
    if (is_rom_bank(bank)) {
        if (ipl_rom_loaded_ && bank >= 0x34 && bank <= 0x37)
            return phys_[bank * BANK_SIZE + (addr & 0x1FFF)];
        if (!warned_rom_bank_[bank]) {
            warned_rom_bank_[bank] = true;
            std::fprintf(stderr, "[mem] read of absent ROM bank %02Xh\n", bank);
        }
        return 0xFF;
    }
    if (bank_absent(bank)) return 0xFF;
    return phys_[bank * BANK_SIZE + (addr & 0x1FFF)];
}

void BankedMemory::write(uint16_t addr, uint8_t value) {
    int bank = map_[addr >> 13];
    if (bank >= 0x34 && bank <= 0x37) {
        // ROM overlay in the MZ tradition: reads hit the IPL ROM, writes fall
        // through to the RAM bank underneath - the firmware stacks and works
        // in low RAM before switching the ROM out
        bank &= 0x03;
    } else if (is_rom_bank(bank)) {
        if (!warned_rom_bank_[bank]) {
            warned_rom_bank_[bank] = true;
            std::fprintf(stderr, "[mem] write to ROM bank %02Xh ignored\n", bank);
        }
        return;
    }
    if (bank_absent(bank)) return;
    phys_[bank * BANK_SIZE + (addr & 0x1FFF)] = value;
}

} // namespace mz
