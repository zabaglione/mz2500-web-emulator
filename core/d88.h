// D88 disk image parser.
//
// MZ-2500 game disks in this repository are 2DD: 80 cylinders x 2 sides x
// 16 sectors x 256 bytes. Sector data is stored on disk with all bits
// inverted (logical value XOR FF) to match the inverted FDC data bus of the
// real machine — see shared/mz2500/d88.py.
//
// Two access paths, kept deliberately separate to avoid double-inversion bugs:
//  - raw_sector():   stored bytes as-is (inverted). Served by the FDC, whose
//                    data register goes through the inverted CPU bus.
//  - read_decoded(): stored XOR FF (logical). Used by the native dummy IPL.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mz {

class D88Disk {
public:
    static constexpr int SECTOR_SIZE = 256;
    static constexpr int SECTORS_PER_TRACK = 16;
    static constexpr int TRACK_COUNT = 160; // cylinder*2 + side

    bool load_file(const std::string& path);
    bool load(std::vector<uint8_t> bytes);
    bool loaded() const { return loaded_; }

    // Physical addressing (sector is 1-based as in the ID field). Returns
    // nullptr when the sector does not exist on the mounted image.
    const uint8_t* raw_sector(int cylinder, int side, int sector) const;

    // Logical block addressing used by the build tools:
    // track = lba/16 (D88 track index), sector = lba%16 + 1.
    bool read_decoded(int lba, uint8_t out[SECTOR_SIZE]) const;

private:
    std::vector<uint8_t> data_;
    // offset of the 256 data bytes for [track][sector-1]; -1 when absent
    std::vector<int32_t> sector_off_;
    bool loaded_ = false;
};

} // namespace mz
