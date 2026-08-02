#include "core/d88.h"

#include <cstdio>
#include <cstring>

namespace mz {

namespace {
constexpr int HEADER_SIZE = 0x2B0;
constexpr int TRACK_TABLE = 0x20;
constexpr int TRACK_TABLE_ENTRIES = 164;

uint32_t rd32(const std::vector<uint8_t>& d, size_t off) {
    return d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) | (uint32_t)(d[off + 3]) << 24;
}
uint16_t rd16(const std::vector<uint8_t>& d, size_t off) {
    return d[off] | (d[off + 1] << 8);
}
} // namespace

bool D88Disk::load_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "[d88] cannot open %s\n", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes(size > 0 ? size : 0);
    if (size > 0 && std::fread(bytes.data(), 1, size, f) != static_cast<size_t>(size)) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    return load(std::move(bytes));
}

bool D88Disk::load(std::vector<uint8_t> bytes) {
    loaded_ = false;
    if (bytes.size() < HEADER_SIZE) {
        std::fprintf(stderr, "[d88] image too small (%zu bytes)\n", bytes.size());
        return false;
    }
    data_ = std::move(bytes);
    sector_off_.assign(TRACK_COUNT * SECTORS_PER_TRACK, -1);

    for (int track = 0; track < TRACK_COUNT && track < TRACK_TABLE_ENTRIES; track++) {
        uint32_t off = rd32(data_, TRACK_TABLE + track * 4);
        if (off == 0) continue; // unformatted track
        // Walk the chained sector blocks: 16-byte header + data.
        for (int s = 0; s < SECTORS_PER_TRACK; s++) {
            if (off + 16 > data_.size()) break;
            const uint8_t sector_id = data_[off + 2]; // R from the ID field
            const uint16_t size = rd16(data_, off + 14);
            if (off + 16 + size > data_.size()) break;
            if (size == SECTOR_SIZE && sector_id >= 1 && sector_id <= SECTORS_PER_TRACK) {
                sector_off_[track * SECTORS_PER_TRACK + (sector_id - 1)] =
                    static_cast<int32_t>(off + 16);
            }
            off += 16 + size;
        }
    }
    loaded_ = true;
    return true;
}

const uint8_t* D88Disk::raw_sector(int cylinder, int side, int sector) const {
    if (!loaded_) return nullptr;
    if (cylinder < 0 || cylinder >= TRACK_COUNT / 2) return nullptr;
    if (side < 0 || side > 1) return nullptr;
    if (sector < 1 || sector > SECTORS_PER_TRACK) return nullptr;
    const int track = cylinder * 2 + side;
    const int32_t off = sector_off_[track * SECTORS_PER_TRACK + (sector - 1)];
    if (off < 0) return nullptr;
    return data_.data() + off;
}

bool D88Disk::read_decoded(int lba, uint8_t out[SECTOR_SIZE]) const {
    const int track = lba / SECTORS_PER_TRACK;
    const int sector = lba % SECTORS_PER_TRACK + 1;
    const uint8_t* raw = raw_sector(track / 2, track & 1, sector);
    if (!raw) return false;
    for (int i = 0; i < SECTOR_SIZE; i++) out[i] = raw[i] ^ 0xFF;
    return true;
}

} // namespace mz
