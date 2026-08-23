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

#include <array>
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
    static constexpr int LEGACY_TRACK_COUNT = 160;
    static constexpr int LEGACY_HEADER_SIZE = 0x2A0;

    // One sector as the ID field describes it, plus its stored bytes. Data
    // is held exactly as it sits on the image (inverted), so the FDC's read
    // and write paths are symmetric and never double-invert.
    struct Sector {
        uint8_t c = 0, h = 0, r = 0, n = 1;
        uint8_t density = 0x00; // 00h = double density
        uint8_t deleted = 0x00; // 10h = deleted data mark
        uint8_t status = 0x00;  // FDC status recorded at dump time
        std::array<uint8_t, 5> reserved = {};
        std::vector<uint8_t> data;
    };

    // Facts found while parsing an image. A short record is retained so the
    // image can be diagnosed and serialized again, but it is not a usable
    // MB8877 transfer record. N >= 4 is also retained for inspection and
    // serialization, but is outside the FDC transfer range implemented here.
    struct LoadReport {
        bool structural_error = false;
        bool image_size_mismatch = false;
        size_t trailing_bytes = 0;
        int invalid_track_offsets = 0;
        int duplicate_track_offsets = 0;
        int track_count_mismatches = 0;
        int records = 0;
        int short_records = 0;
        int truncated_records = 0;
        int unsupported_n_records = 0;
        int over_capacity_tracks = 0;
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

    // D88's N field describes 128 << N bytes. This reports the nominal size
    // for any shift that fits in size_t; the FDC-specific range is exposed by
    // fdc_transfer_size()/fdc_transfer_supported().
    static bool nominal_sector_size(uint8_t n, size_t& size);
    static size_t fdc_transfer_size(const Sector& sector);
    static bool fdc_transfer_supported(const Sector& sector);

    const LoadReport& load_report() const { return load_report_; }
    bool has_structural_error() const {
        return load_report_.structural_error || load_report_.image_size_mismatch;
    }
    bool has_unsupported_records() const {
        return load_report_.unsupported_n_records != 0 ||
               load_report_.short_records != 0 ||
               load_report_.over_capacity_tracks != 0;
    }

    // Locate the physical record by the ID field, including C/H/R and
    // density. sector_at() below intentionally remains a physical-order
    // accessor for READ ADDRESS and READ TRACK.
    const Sector* sector(int cylinder, int side, int record) const;
    const Sector* sector(int cylinder, int side, int record,
                         bool single_density) const;

    // Search the records physically present under the selected head. The
    // ID field's C is compared with the controller Track Register, while H
    // is compared with the command's S flag only when side comparison is
    // enabled. This separation is required for deliberately mismatched ID
    // fields and other non-canonical D88 layouts.
    const Sector* record_on_track(int physical_cylinder, int physical_side,
                                  uint8_t id_c, uint8_t id_r,
                                  bool single_density, bool compare_side,
                                  uint8_t id_h) const;

    // Physical addressing (sector is 1-based as in the ID field). Returns
    // nullptr when the sector does not exist on the mounted image.
    const uint8_t* raw_sector(int cylinder, int side, int sector) const;
    const uint8_t* raw_sector(int cylinder, int side, int sector,
                              bool single_density) const;

    // Logical block addressing used by the build tools:
    // track = lba/16 (D88 track index), sector = lba%16 + 1.
    bool read_decoded(int lba, uint8_t out[SECTOR_SIZE]) const;

    // Coordinate addressing used by boot code and variable-record media.
    // The output receives the record's nominal FDC transfer length after the
    // D88 storage inversion. The caller supplies the output capacity.
    bool read_decoded(int cylinder, int side, int record, uint8_t* out,
                      size_t capacity) const;

    // IPLPRO header and payload availability check.  Legacy single-bank
    // images use C=0/H=0 then C=1/H=0.  Multi-bank images use the firmware
    // source order C=n/H=0 then C=n+1/H=1 for each listed destination bank.
    // No LBA arithmetic is used for this check.
    bool is_iplpro_compatible() const;

    // Writable view of a sector's stored bytes. Returns nullptr when the
    // sector is not on the disk, its N/data pair is not a supported complete
    // MB8877 transfer record, or the disk is write protected. Marks the image
    // dirty so the frontend knows to persist it.
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
    // Same writable view, but matching the complete C/H/R ID and density.
    // The legacy write_sector() overloads retain their physical-track lookup
    // contract for existing callers.
    uint8_t* write_record(int cylinder, int side, int sector,
                          bool single_density);
    uint8_t* write_record_on_track(int physical_cylinder, int physical_side,
                                   uint8_t id_c, uint8_t id_r,
                                   bool single_density, bool compare_side,
                                   uint8_t id_h);

    // Set or clear the deleted-data mark the FDC reports on the next read.
    bool set_deleted_mark(int cylinder, int side, int sector, bool deleted);
    bool set_deleted_mark(int cylinder, int side, int sector, bool deleted,
                          bool single_density);
    bool set_deleted_mark_record(int cylinder, int side, int sector,
                                 bool deleted, bool single_density);
    bool set_deleted_mark_on_track(int physical_cylinder, int physical_side,
                                   uint8_t id_c, uint8_t id_r,
                                   bool single_density, bool compare_side,
                                   uint8_t id_h, bool deleted);

    // Whether a sector currently carries the deleted-data mark (false when
    // the sector does not exist). READ SECTOR consults this to report
    // ST_REC_TYPE, the read-side counterpart of set_deleted_mark() above --
    // the two must agree on the same field or a driver that marks a bad
    // record and reads it back to detect that never will.
    bool deleted_mark(int cylinder, int side, int sector) const;
    bool deleted_mark(int cylinder, int side, int sector,
                      bool single_density) const;
    bool deleted_mark_record(int cylinder, int side, int sector,
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
    void refresh_record_diagnostics();

    Track tracks_[TRACK_COUNT];
    bool loaded_ = false;
    bool write_protected_ = false;
    bool dirty_ = false;
    LoadReport load_report_;

    // Header fields captured verbatim by load() so serialize() can hand
    // them back instead of writing zeroed/hardcoded defaults.
    std::vector<uint8_t> name_ = std::vector<uint8_t>(17, 0); // offset 0x00-0x10
    std::array<uint8_t, 9> header_reserved_ = {};              // offset 0x11-0x19
    uint8_t media_type_ = 0x10;                               // offset 0x1B, 0x10 = 2DD
    int header_size_ = HEADER_SIZE;
    int track_table_entries_ = TRACK_COUNT;
    std::vector<uint8_t> trailing_data_;
};

} // namespace mz
