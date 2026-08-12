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
    static constexpr int TRACK_COUNT = 164; // D88 track table entries
    static constexpr int HEADER_SIZE = 0x2B0;

    // One sector as the ID field describes it, plus its stored bytes. Data
    // is held exactly as it sits on the image (inverted), so the FDC's read
    // and write paths are symmetric and never double-invert.
    struct Sector {
        uint8_t c = 0, h = 0, r = 0, n = 1;
        uint8_t density = 0x00; // 00h = double density
        uint8_t deleted = 0x00; // 10h = deleted data mark
        uint8_t status = 0x00;  // FDC status recorded at dump time
        std::vector<uint8_t> data;
    };
    struct Track {
        std::vector<Sector> sectors;
    };

    bool load_file(const std::string& path);
    bool load(std::vector<uint8_t> bytes);
    void eject();
    bool loaded() const { return loaded_; }

    // A blank, never-formatted 2DD image: the 0x2B0 header with an all-zero
    // track table. Every read reports record-not-found until a format lays
    // tracks down, which is what a brand-new floppy does.
    static std::vector<uint8_t> make_unformatted();

    // Rebuild the D88 byte stream from the parsed tracks.
    std::vector<uint8_t> serialize() const;

    // Physical addressing (sector is 1-based as in the ID field). Returns
    // nullptr when the sector does not exist on the mounted image.
    const uint8_t* raw_sector(int cylinder, int side, int sector) const;
    const uint8_t* raw_sector(int cylinder, int side, int sector,
                              bool single_density) const;

    // Logical block addressing used by the build tools:
    // track = lba/16 (D88 track index), sector = lba%16 + 1.
    bool read_decoded(int lba, uint8_t out[SECTOR_SIZE]) const;

    // Writable view of a sector's stored bytes. Returns nullptr when the
    // sector is not on the disk (record not found), its stored size is
    // smaller than SECTOR_SIZE (same guard as raw_sector(), since callers
    // index up to SECTOR_SIZE-1), or the disk is write protected. Marks the
    // image dirty so the frontend knows to persist it.
    //
    // Lifetime: the returned pointer aliases Sector::data inside a
    // std::vector<Sector>, so it is valid only until the next call that
    // structurally mutates the disk (load(), or any future track-format
    // operation) — those can reallocate or reorder the backing storage. A
    // caller that holds the pointer across emulated time (e.g. a controller
    // state machine writing one byte per cycle) must not cache it; re-call
    // write_sector() to get a fresh pointer instead.
    uint8_t* write_sector(int cylinder, int side, int sector);
    uint8_t* write_sector(int cylinder, int side, int sector,
                          bool single_density);

    // Set or clear the deleted-data mark the FDC reports on the next read.
    bool set_deleted_mark(int cylinder, int side, int sector, bool deleted);
    bool set_deleted_mark(int cylinder, int side, int sector, bool deleted,
                          bool single_density);

    // Whether a sector currently carries the deleted-data mark (false when
    // the sector does not exist). READ SECTOR consults this to report
    // ST_REC_TYPE, the read-side counterpart of set_deleted_mark() above --
    // the two must agree on the same field or a driver that marks a bad
    // record and reads it back to detect that never will.
    bool deleted_mark(int cylinder, int side, int sector) const;
    bool deleted_mark(int cylinder, int side, int sector,
                      bool single_density) const;

    // Lay a track down, replacing whatever was there. The sector order is
    // kept exactly as handed in - the interleave belongs to the software
    // doing the formatting, not to this emulator.
    bool format_track(int cylinder, int side, const std::vector<Sector>& sectors);

    // ID field of the n-th record physically on a track, for READ ADDRESS.
    // Returns nullptr when the track holds no such record.
    const Sector* sector_at(int cylinder, int side, int index) const;

    // How many physical records a track holds (0 for an unformatted track
    // or an out-of-range cylinder/side). Lets a caller walk sector_at()
    // 0..count-1 without relying on the modulo wraparound sector_at() uses
    // internally to detect "back at the start".
    int sector_count(int cylinder, int side) const;

    bool write_protected() const { return write_protected_; }
    void set_write_protected(bool on) { write_protected_ = on; dirty_ = true; }

    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

private:
    static int track_index(int cylinder, int side) { return cylinder * 2 + side; }
    const Sector* find_sector(int cylinder, int side, int sector) const;
    const Sector* find_sector(int cylinder, int side, int sector,
                              bool single_density) const;
    Sector* find_sector_mut(int cylinder, int side, int sector);
    Sector* find_sector_mut(int cylinder, int side, int sector,
                            bool single_density);

    Track tracks_[TRACK_COUNT];
    bool loaded_ = false;
    bool write_protected_ = false;
    bool dirty_ = false;

    // Header fields captured verbatim by load() so serialize() can hand
    // them back instead of writing zeroed/hardcoded defaults.
    std::vector<uint8_t> name_ = std::vector<uint8_t>(17, 0); // offset 0x00-0x10
    uint8_t media_type_ = 0x10;                               // offset 0x1B, 0x10 = 2DD
};

} // namespace mz
