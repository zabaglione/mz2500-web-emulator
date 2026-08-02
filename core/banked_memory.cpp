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
    std::memset(phys_.data(), 0, phys_.size());
    selector_ = 0;
    kanji_bank_ = 0;
}

uint8_t BankedMemory::read(uint16_t addr) {
    const int bank = map_[addr >> 13];
    if (bank == 0x39 && (kanji_bank_ & 0x80)) {
        if (!warned_rom_bank_[bank]) {
            warned_rom_bank_[bank] = true;
            std::fprintf(stderr, "[mem] read of bank 39h with kanji ROM selected (CFh bit7=1) - no ROM present\n");
        }
        return 0xFF;
    }
    if (is_rom_bank(bank)) {
        if (!warned_rom_bank_[bank]) {
            warned_rom_bank_[bank] = true;
            std::fprintf(stderr, "[mem] read of absent ROM bank %02Xh\n", bank);
        }
        return 0xFF;
    }
    return phys_[bank * BANK_SIZE + (addr & 0x1FFF)];
}

void BankedMemory::write(uint16_t addr, uint8_t value) {
    const int bank = map_[addr >> 13];
    if (is_rom_bank(bank)) {
        if (!warned_rom_bank_[bank]) {
            warned_rom_bank_[bank] = true;
            std::fprintf(stderr, "[mem] write to ROM bank %02Xh ignored\n", bank);
        }
        return;
    }
    phys_[bank * BANK_SIZE + (addr & 0x1FFF)] = value;
}

} // namespace mz
