#include "core/d88.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mz {

namespace {
constexpr int TRACK_TABLE = 0x20;
constexpr size_t TRACK_HEADER_SIZE = 16;
constexpr int MAX_RECORDS_PER_TRACK = 1024;

uint32_t rd32(const std::vector<uint8_t>& d, size_t off) {
    return d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) | (uint32_t)(d[off + 3]) << 24;
}
uint16_t rd16(const std::vector<uint8_t>& d, size_t off) {
    return d[off] | (d[off + 1] << 8);
}
void wr32(std::vector<uint8_t>& d, size_t off, uint32_t v) {
    d[off] = (uint8_t)(v & 0xFF);
    d[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    d[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    d[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}
void wr16(std::vector<uint8_t>& d, size_t off, uint16_t v) {
    d[off] = (uint8_t)(v & 0xFF);
    d[off + 1] = (uint8_t)((v >> 8) & 0xFF);
}
} // namespace

bool D88Disk::nominal_sector_size(uint8_t n, size_t& size) {
    constexpr int shift_limit = static_cast<int>(sizeof(size_t) * 8) - 7;
    if (n >= shift_limit) return false;
    size = static_cast<size_t>(128) << n;
    return true;
}

size_t D88Disk::fdc_transfer_size(const Sector& sector) {
    if (!fdc_transfer_supported(sector)) return 0;
    size_t size = 0;
    return nominal_sector_size(sector.n, size) ? size : 0;
}

bool D88Disk::fdc_transfer_supported(const Sector& sector) {
    size_t size = 0;
    // MB8877-compatible records in this core are N=0..3. The stored data
    // may be longer than the nominal field, but never shorter.
    return sector.n <= 3 && nominal_sector_size(sector.n, size) &&
           sector.data.size() >= size;
}

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

std::vector<uint8_t> D88Disk::make_unformatted() {
    std::vector<uint8_t> d(HEADER_SIZE, 0);
    d[0x1A] = 0x00; // not write protected
    d[0x1B] = 0x10; // 2DD
    wr32(d, 0x1C, HEADER_SIZE);
    return d; // track table stays all zero: every track unformatted
}

void D88Disk::eject() {
    loaded_ = false;
    for (int t = 0; t < TRACK_COUNT; t++) tracks_[t].sectors.clear();
    write_protected_ = false;
    dirty_ = false;
    load_report_ = LoadReport{};
    name_.assign(17, 0);
    media_type_ = 0x10;
}

bool D88Disk::load(std::vector<uint8_t> bytes) {
    loaded_ = false;
    if (bytes.size() < HEADER_SIZE) {
        std::fprintf(stderr, "[d88] image too small (%zu bytes)\n", bytes.size());
        // Leave no trace of whatever was mounted before: a caller that goes
        // on to use this disk after a failed load (e.g. the frontend
        // swapping media and only checking the return value loosely) must
        // see an empty, non-protected disk, not the previous image's
        // tracks/protection flag resurrected under a new name. Without this,
        // a WRITE TRACK issued after a failed insert could bring the OLD
        // disk's remaining tracks back to life inside what looks like a
        // fresh image.
        eject();
        return false;
    }
    // Disk name: offset 0x00-0x10, 17 bytes, so serialize() can hand the
    // image back with the name the user gave it instead of a zeroed field.
    name_.assign(bytes.begin(), bytes.begin() + 17);
    media_type_ = bytes[0x1B];
    write_protected_ = bytes[0x1A] != 0;
    dirty_ = false;
    load_report_ = LoadReport{};
    const uint32_t declared_image_size = rd32(bytes, 0x1C);
    if (declared_image_size != 0 && declared_image_size != bytes.size())
        load_report_.image_size_mismatch = true;
    for (int t = 0; t < TRACK_COUNT; t++) tracks_[t].sectors.clear();

    std::vector<size_t> track_offsets(TRACK_COUNT, 0);
    for (int track = 0; track < TRACK_COUNT; track++) {
        track_offsets[track] = rd32(bytes, TRACK_TABLE + track * 4);
    }

    for (int track = 0; track < TRACK_COUNT; track++) {
        size_t off = track_offsets[track];
        if (off == 0) continue; // unformatted track
        if (off < HEADER_SIZE || off + TRACK_HEADER_SIZE > bytes.size()) {
            load_report_.structural_error = true;
            load_report_.invalid_track_offsets++;
            continue;
        }

        // A malformed record must not consume bytes belonging to another
        // track. Use the next track-table offset as a hard parsing boundary.
        size_t track_end = bytes.size();
        for (size_t candidate : track_offsets) {
            if (candidate > off && candidate < track_end) track_end = candidate;
        }
        const int declared_count = rd16(bytes, off + 4);
        if (declared_count > MAX_RECORDS_PER_TRACK)
            load_report_.structural_error = true;
        const int parse_count = std::min(declared_count, MAX_RECORDS_PER_TRACK);
        int parsed_count = 0;
        uint16_t first_count = 0;
        bool count_mismatch = false;

        for (int s = 0; s < parse_count; s++) {
            if (off + TRACK_HEADER_SIZE > track_end ||
                off + TRACK_HEADER_SIZE > bytes.size()) {
                load_report_.structural_error = true;
                break;
            }
            Sector sec;
            sec.c = bytes[off + 0];
            sec.h = bytes[off + 1];
            sec.r = bytes[off + 2];
            sec.n = bytes[off + 3];
            const uint16_t sector_count = rd16(bytes, off + 4);
            if (s == 0) first_count = sector_count;
            else if (sector_count != first_count) count_mismatch = true;
            sec.density = bytes[off + 6];
            sec.deleted = bytes[off + 7];
            sec.status = bytes[off + 8];
            const uint16_t declared_size = rd16(bytes, off + 14);
            const size_t data_begin = off + TRACK_HEADER_SIZE;
            const size_t available = track_end > data_begin ? track_end - data_begin : 0;
            const size_t actual_size = std::min<size_t>(declared_size, available);
            sec.data.assign(bytes.begin() + data_begin,
                            bytes.begin() + data_begin + actual_size);
            tracks_[track].sectors.push_back(std::move(sec));
            parsed_count++;
            load_report_.records++;

            const Sector& stored = tracks_[track].sectors.back();
            size_t nominal = 0;
            if (!nominal_sector_size(stored.n, nominal) || stored.n > 3)
                load_report_.unsupported_n_records++;
            else if (stored.data.size() < nominal)
                load_report_.short_records++;
            if (actual_size < declared_size) {
                load_report_.truncated_records++;
                load_report_.structural_error = true;
            }
            if (actual_size < declared_size || off + TRACK_HEADER_SIZE + declared_size > track_end)
                break;
            off += TRACK_HEADER_SIZE + declared_size;
        }
        if (parsed_count != declared_count || count_mismatch) {
            load_report_.track_count_mismatches++;
            load_report_.structural_error = true;
        }
    }
    loaded_ = true;
    return true;
}

std::vector<uint8_t> D88Disk::serialize() const {
    std::vector<uint8_t> d(HEADER_SIZE, 0);
    std::copy(name_.begin(), name_.end(), d.begin()); // offset 0x00-0x10
    d[0x1A] = write_protected_ ? 0x10 : 0x00;
    d[0x1B] = media_type_;
    for (int track = 0; track < TRACK_COUNT; track++) {
        const Track& t = tracks_[track];
        if (t.sectors.empty()) continue; // table entry stays 0
        wr32(d, TRACK_TABLE + track * 4, (uint32_t)d.size());
        for (const Sector& sec : t.sectors) {
            const size_t hdr = d.size();
            d.resize(hdr + 16, 0);
            d[hdr + 0] = sec.c;
            d[hdr + 1] = sec.h;
            d[hdr + 2] = sec.r;
            d[hdr + 3] = sec.n;
            wr16(d, hdr + 4, (uint16_t)t.sectors.size());
            d[hdr + 6] = sec.density;
            d[hdr + 7] = sec.deleted;
            d[hdr + 8] = sec.status;
            wr16(d, hdr + 14, (uint16_t)sec.data.size());
            d.insert(d.end(), sec.data.begin(), sec.data.end());
        }
    }
    wr32(d, 0x1C, (uint32_t)d.size());
    return d;
}

const D88Disk::Sector* D88Disk::find_sector(int cylinder, int side, int sector) const {
    if (!loaded_) return nullptr;
    if (side < 0 || side > 1) return nullptr;
    const int track = track_index(cylinder, side);
    if (track < 0 || track >= TRACK_COUNT) return nullptr;
    for (const Sector& s : tracks_[track].sectors)
        if (s.r == sector) return &s;
    return nullptr;
}

const D88Disk::Sector* D88Disk::find_sector(int cylinder, int side, int sector,
                                            bool single_density) const {
    if (!loaded_) return nullptr;
    if (side < 0 || side > 1) return nullptr;
    const int track = track_index(cylinder, side);
    if (track < 0 || track >= TRACK_COUNT) return nullptr;
    for (const Sector& s : tracks_[track].sectors) {
        if (s.r == sector && ((s.density & 0x40) != 0) == single_density)
            return &s;
    }
    return nullptr;
}

const D88Disk::Sector* D88Disk::sector(int cylinder, int side, int record) const {
    if (!loaded_ || side < 0 || side > 1) return nullptr;
    const int track = track_index(cylinder, side);
    if (track < 0 || track >= TRACK_COUNT) return nullptr;
    for (const Sector& s : tracks_[track].sectors)
        if (s.c == cylinder && s.h == side && s.r == record) return &s;
    return nullptr;
}

const D88Disk::Sector* D88Disk::sector(int cylinder, int side, int record,
                                       bool single_density) const {
    if (!loaded_ || side < 0 || side > 1) return nullptr;
    const int track = track_index(cylinder, side);
    if (track < 0 || track >= TRACK_COUNT) return nullptr;
    for (const Sector& s : tracks_[track].sectors) {
        if (s.c == cylinder && s.h == side && s.r == record &&
            ((s.density & 0x40) != 0) == single_density)
            return &s;
    }
    return nullptr;
}

const uint8_t* D88Disk::raw_sector(int cylinder, int side, int sector) const {
    const Sector* s = find_sector(cylinder, side, sector);
    if (!s || !fdc_transfer_supported(*s)) return nullptr;
    return s->data.data();
}

const uint8_t* D88Disk::raw_sector(int cylinder, int side, int sector,
                                   bool single_density) const {
    const Sector* s = find_sector(cylinder, side, sector, single_density);
    if (!s || !fdc_transfer_supported(*s)) return nullptr;
    return s->data.data();
}

bool D88Disk::read_decoded(int lba, uint8_t out[SECTOR_SIZE]) const {
    const int track = lba / SECTORS_PER_TRACK;
    const int sector = lba % SECTORS_PER_TRACK + 1;
    const Sector* sec = this->sector(track / 2, track & 1, sector);
    if (!sec || fdc_transfer_size(*sec) != SECTOR_SIZE) return false;
    return read_decoded(track / 2, track & 1, sector, out, SECTOR_SIZE);
}

bool D88Disk::read_decoded(int cylinder, int side, int record, uint8_t* out,
                           size_t capacity) const {
    const Sector* sec = sector(cylinder, side, record);
    if (!out || !sec || !fdc_transfer_supported(*sec)) return false;
    const size_t size = fdc_transfer_size(*sec);
    if (capacity < size) return false;
    for (size_t i = 0; i < size; i++) out[i] = sec->data[i] ^ 0xFF;
    return true;
}

bool D88Disk::is_iplpro_compatible() const {
    uint8_t header[SECTOR_SIZE] = {};
    if (!read_decoded(0, 1, 1, header, sizeof(header))) return false;
    if (header[0] != 0x01 || std::memcmp(header + 1, "IPLPRO", 6) != 0)
        return false;
    for (int r = 1; r <= SECTORS_PER_TRACK; r++) {
        if (!read_decoded(0, 0, r, header, sizeof(header))) return false;
        if (!read_decoded(1, 0, r, header, sizeof(header))) return false;
    }
    return true;
}

D88Disk::Sector* D88Disk::find_sector_mut(int cylinder, int side, int sector) {
    return const_cast<Sector*>(find_sector(cylinder, side, sector));
}

D88Disk::Sector* D88Disk::find_sector_mut(int cylinder, int side, int sector,
                                          bool single_density) {
    return const_cast<Sector*>(find_sector(cylinder, side, sector, single_density));
}

uint8_t* D88Disk::write_sector(int cylinder, int side, int sector) {
    if (write_protected_) return nullptr;
    Sector* s = find_sector_mut(cylinder, side, sector);
    if (!s || !fdc_transfer_supported(*s)) return nullptr;
    dirty_ = true;
    return s->data.data();
}

uint8_t* D88Disk::write_sector(int cylinder, int side, int sector,
                               bool single_density) {
    if (write_protected_) return nullptr;
    Sector* s = find_sector_mut(cylinder, side, sector, single_density);
    if (!s || !fdc_transfer_supported(*s)) return nullptr;
    dirty_ = true;
    return s->data.data();
}

uint8_t* D88Disk::write_record(int cylinder, int side, int sector,
                                bool single_density) {
    if (write_protected_) return nullptr;
    const Sector* found = this->sector(cylinder, side, sector, single_density);
    Sector* s = const_cast<Sector*>(found);
    if (!s || !fdc_transfer_supported(*s)) return nullptr;
    dirty_ = true;
    return s->data.data();
}

bool D88Disk::set_deleted_mark(int cylinder, int side, int sector, bool deleted) {
    if (write_protected_) return false;
    Sector* s = find_sector_mut(cylinder, side, sector);
    if (!s) return false;
    s->deleted = deleted ? 0x10 : 0x00;
    dirty_ = true;
    return true;
}

bool D88Disk::set_deleted_mark(int cylinder, int side, int sector, bool deleted,
                               bool single_density) {
    if (write_protected_) return false;
    Sector* s = find_sector_mut(cylinder, side, sector, single_density);
    if (!s) return false;
    s->deleted = deleted ? 0x10 : 0x00;
    dirty_ = true;
    return true;
}

bool D88Disk::set_deleted_mark_record(int cylinder, int side, int sector,
                                      bool deleted, bool single_density) {
    if (write_protected_) return false;
    const Sector* found = this->sector(cylinder, side, sector, single_density);
    Sector* s = const_cast<Sector*>(found);
    if (!s) return false;
    s->deleted = deleted ? 0x10 : 0x00;
    dirty_ = true;
    return true;
}

bool D88Disk::deleted_mark(int cylinder, int side, int sector) const {
    const Sector* s = find_sector(cylinder, side, sector);
    return s && s->deleted != 0;
}

bool D88Disk::deleted_mark(int cylinder, int side, int sector,
                           bool single_density) const {
    const Sector* s = find_sector(cylinder, side, sector, single_density);
    return s && s->deleted != 0;
}

bool D88Disk::deleted_mark_record(int cylinder, int side, int sector,
                                  bool single_density) const {
    const Sector* s = this->sector(cylinder, side, sector, single_density);
    return s && s->deleted != 0;
}

const D88Disk::Sector* D88Disk::sector_at(int cylinder, int side, int index) const {
    if (!loaded_ || side < 0 || side > 1) return nullptr;
    const int track = track_index(cylinder, side);
    if (track < 0 || track >= TRACK_COUNT) return nullptr;
    const Track& t = tracks_[track];
    if (t.sectors.empty()) return nullptr;
    return &t.sectors[(size_t)index % t.sectors.size()];
}

int D88Disk::sector_count(int cylinder, int side) const {
    if (!loaded_ || side < 0 || side > 1) return 0;
    const int track = track_index(cylinder, side);
    if (track < 0 || track >= TRACK_COUNT) return 0;
    return (int)tracks_[track].sectors.size();
}

bool D88Disk::format_track(int cylinder, int side, const std::vector<Sector>& sectors) {
    if (write_protected_) return false;
    if (side < 0 || side > 1) return false;
    const int track = track_index(cylinder, side);
    if (track < 0 || track >= TRACK_COUNT) return false;
    tracks_[track].sectors = sectors;
    for (const Sector& sec : sectors) {
        load_report_.records++;
        size_t nominal = 0;
        if (!nominal_sector_size(sec.n, nominal) || sec.n > 3)
            load_report_.unsupported_n_records++;
        else if (sec.data.size() < nominal)
            load_report_.short_records++;
    }
    loaded_ = true;
    dirty_ = true;
    return true;
}

} // namespace mz
