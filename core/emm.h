// MZ-1R37 640KB EMM: a bare RAM board behind two I/O ports (16-bit decode).
//
//   ACh write: EMM address[19:16] = bus A15-A8 (low nibble),
//              EMM address[15:8]  = written data
//   ADh read / write: EMM address[7:0] = bus A15-A8, data on D7-D0
//
// There is no auto-increment; the primary I/O map is explicit about that.
// The 20-bit address space is 1MB but the board carries 640KB - the rest
// reads back as open bus (FFh) and swallows writes, which is also what a
// probe sees when it walks off the end of the RAM.
//
// Contents survive a CPU reset (it is just RAM with no reset line) and are
// zero-filled at power-on for determinism.
#pragma once

#include <cstdint>
#include <vector>

namespace mz {

class Emm {
public:
    static constexpr uint32_t SIZE = 640 * 1024;

    void latch(uint8_t a15_8, uint8_t value) {
        hi_ = ((uint32_t)(a15_8 & 0x0F) << 16) | ((uint32_t)value << 8);
    }
    uint8_t read(uint8_t a15_8) const {
        const uint32_t a = hi_ | a15_8;
        return a < SIZE ? ram_[a] : 0xFF;
    }
    void write(uint8_t a15_8, uint8_t value) {
        const uint32_t a = hi_ | a15_8;
        if (a < SIZE) ram_[a] = value;
    }

private:
    uint32_t hi_ = 0; // address bits 19:8, held by the ACh latch
    std::vector<uint8_t> ram_ = std::vector<uint8_t>(SIZE, 0);
};

} // namespace mz
