// MB8877 floppy disk controller — the subset the NEKO CAN RUN boot chain
// uses: RESTORE, SEEK, STEP/STEP-IN/STEP-OUT, single READ SECTOR, polled
// BUSY/DRQ status.
//
// Two drives are wired (port DCh selects; each drive keeps its own head
// position). Register values through this interface are LOGICAL chip
// values; the machine's I/O dispatcher applies the MZ-2500 bus inversion
// for ports D8h-DBh. The data register during READ serves *stored* D88
// bytes (which are written inverted on disk), so after the bus inversion
// the CPU receives logical data — exactly like real hardware reading an
// inverted-polarity platter through an inverted bus. Sector data is
// re-looked-up on every byte so a hot disk swap can never leave a
// dangling pointer.
//
// Timing is a coarse model in CPU cycles (6 MHz): 32 us per MFM data byte
// (250 kbps) or 64 us per FM byte (125 kbps), plus fixed per-READ and
// per-seek-step latencies
// calibrated black-box against EmuZ-2500 so the game's load-driven state
// transitions land on the same frame numbers. Late data-register reads
// never lose data, which is lenient vs. real hardware but safe for
// polled loaders -- except when the host goes fully quiet on the data
// register for a whole sector's transfer time during an active READ/WRITE
// SECTOR, in which case the real-time fallback (advance_read_realtime()/
// advance_write_realtime() in the .cpp) completes the transfer on its own
// and does set LOST DATA, same as real hardware would.
#pragma once

#include <cstdint>
#include <vector>

#include "core/d88.h"

namespace mz {

class FdcMb8877 {
public:
    static constexpr int NUM_DRIVES = 2;

    static constexpr uint64_t CYC_PER_BYTE_MFM = 192;   // 32 us, 250 kbps
    static constexpr uint64_t CYC_PER_BYTE_FM = 384;    // 64 us, 125 kbps
    static constexpr uint64_t CYC_PER_BYTE = CYC_PER_BYTE_MFM;
    static constexpr uint64_t CYC_PER_REV = 1'200'000;  // 200 ms (300 rpm)
    static constexpr uint64_t CYC_PER_STEP = 18'000;    // 3 ms

    // Status bits. The chip reuses the same byte for two meanings: bits 1-5
    // read one way after a Type I command (seek family) and another after a
    // Type II/III command (read, write, format).
    static constexpr uint8_t ST_BUSY = 0x01;
    // Type I
    static constexpr uint8_t ST_INDEX = 0x02;
    static constexpr uint8_t ST_TRACK0 = 0x04;
    static constexpr uint8_t ST_SEEK_ERR = 0x10;
    static constexpr uint8_t ST_HEAD = 0x20;
    // Type II / III
    static constexpr uint8_t ST_DRQ = 0x02;
    static constexpr uint8_t ST_LOST = 0x04;
    static constexpr uint8_t ST_RNF = 0x10;
    static constexpr uint8_t ST_REC_TYPE = 0x20;    // read: deleted data mark
    static constexpr uint8_t ST_WRITE_FAULT = 0x20; // write
    // both
    static constexpr uint8_t ST_CRC = 0x08;
    static constexpr uint8_t ST_WP = 0x40;
    static constexpr uint8_t ST_NOT_READY = 0x80;

    void attach(int drive, D88Disk* disk) { disks_[drive & 1] = disk; }
    void reset();

    // reg: 0=status/command, 1=track, 2=sector, 3=data
    uint8_t read(int reg, uint64_t now);
    void write(int reg, uint8_t value, uint64_t now);

    void write_drive(uint8_t value); // port DCh (bit7 motor, low bits drive)
    void write_side(uint8_t value);  // port DDh
    void set_single_density(bool single) { single_density_ = single; }
    bool single_density() const { return single_density_; }

    bool motor_on() const { return motor_; }
    int selected_drive() const { return drive_; }
    // access-lamp bitmask, bit n = drive n LED (motor + drive select)
    uint8_t lamp_mask() const { return motor_ ? (uint8_t)(1 << drive_) : 0; }
    int physical_cylinder() const { return phys_cyl_[drive_]; }

    // pre-data latency per READ SECTOR command (ID search, gaps); calibration knob
    void set_read_latency_us(uint32_t us) { read_latency_cycles_ = (uint64_t)us * 6; }
    uint32_t read_latency_us() const { return (uint32_t)(read_latency_cycles_ / 6); }
    void set_step_time_us(uint32_t us) { step_cycles_ = (uint64_t)us * 6; }

    // access-pattern counters for timing calibration
    uint64_t stat_reads = 0;
    uint64_t stat_seeks = 0;
    uint64_t stat_steps = 0;

private:
    enum class State { Idle, TypeI, Read, Write, ReadAddr, WriteTrack, ReadTrack };

    uint8_t status_at(uint64_t now);
    // Real MB8877 hardware is clocked by the rotating disk, not by whether
    // the CPU (or a DMA channel) ever reads/writes a byte through the data
    // register: bytes arrive and depart on schedule regardless, and one the
    // host never picks up (or supplies) in time is simply lost -- LOST DATA
    // -- not a reason for BUSY to hang forever. Some firmware issues a
    // multi-sector command purely to watch the sector register catch up to
    // a target value and never touches the data register at all, so
    // completion has to be driven by elapsed time here, exactly as it would
    // be driven by the spinning platter on real hardware, not solely by how
    // many bytes the CPU has explicitly moved through register 3.
    void advance_read_realtime(uint64_t now);
    void advance_write_realtime(uint64_t now);
    // Same quiet-window idea as advance_read_realtime()/advance_write_realtime()
    // above, for the three Type II/III commands that have no CPU-driven
    // multi-record walk to fall back on at all: a driver that issues one of
    // these and then polls status/other registers without ever touching
    // register 3 would otherwise spin BUSY forever, exactly the failure mode
    // that hung the format utility for READ/WRITE SECTOR before those two
    // gained their fallback. Each treats its own whole transfer (the 6-byte
    // ID field; the track stream) as the "record" whose worth of quiet time
    // completes the command, since none of these three has a smaller
    // sub-record unit to repeat between the way multi-sector read/write do.
    void advance_readaddr_realtime(uint64_t now);
    void advance_writetrack_realtime(uint64_t now);
    void advance_readtrack_realtime(uint64_t now);
    uint64_t byte_cycles() const {
        return command_single_density_ ? CYC_PER_BYTE_FM : CYC_PER_BYTE_MFM;
    }
    uint64_t byte_ready(int index) const {
        return read_start_ + (uint64_t)index * byte_cycles();
    }
    const uint8_t* active_sector() const {
        const D88Disk* d = disks_[read_drive_];
        return d ? d->raw_sector(read_cyl_, read_side_, read_sector_,
                                 command_single_density_)
                 : nullptr;
    }
    // Whether the sector READ SECTOR is currently (or about to be) serving
    // carries the deleted-data mark WRITE SECTOR's a0 flag left on it. Read
    // side of the same field set_deleted_mark()/format_track() maintain, so
    // ST_REC_TYPE in the completion status agrees with what READ TRACK
    // renders as F8h vs FBh.
    bool active_sector_deleted() const {
        const D88Disk* d = disks_[read_drive_];
        return d && d->deleted_mark(read_cyl_, read_side_, read_sector_,
                                    command_single_density_);
    }
    // Last direction a STEP command moved the head, so a bare STEP repeats it.
    int step_dir_ = 1;
    bool disk_write_protected() const {
        const D88Disk* d = disks_[drive_];
        return d && d->write_protected();
    }

    D88Disk* disks_[NUM_DRIVES] = {nullptr, nullptr};
    State state_ = State::Idle;

    uint8_t track_reg_ = 0;
    uint8_t sector_reg_ = 0;
    uint8_t data_reg_ = 0;
    uint8_t done_status_ = 0; // error bits latched for when the op completes
    // Which command family last ran, so an idle status read knows which
    // meaning bits 1/2/4/5 carry: Type I (seek family) reports INDEX/TRACK00/
    // SEEK ERROR/HEAD LOADED, Type II/III (read/write/format) reports
    // DRQ/LOST DATA/RECORD NOT FOUND/RECORD TYPE. Defaults to Type I to match
    // the chip's power-on/reset status format.
    bool last_type1_ = true;

    int phys_cyl_[NUM_DRIVES] = {0, 0};
    int side_ = 0;
    int drive_ = 0;
    bool motor_ = false;
    bool single_density_ = false;
    // DEh is sampled when a Type II/III command starts. A mid-transfer port
    // write affects the next command, not the record or bit rate in flight.
    bool command_single_density_ = false;

    uint64_t busy_until_ = 0;

    // active READ SECTOR (position latched at command time, data looked up
    // per byte so a swapped disk cannot dangle)
    bool read_valid_ = false;
    int read_drive_ = 0;
    int read_cyl_ = 0;
    int read_side_ = 0;
    int read_sector_ = 1;
    int read_index_ = 0;
    uint64_t read_start_ = 0; // cycle at which byte 0 becomes available
    // Cycle of the most recent access to the data register (reg 3) for the
    // transfer in flight; the real-time fallback in advance_read_realtime()
    // fires only once a full sector's worth of time has passed since this
    // without a single access, not simply once total elapsed time exceeds a
    // sector window -- so a host draining steadily but slowly is never
    // truncated. Reset to read_start_ when a READ SECTOR command starts, and
    // again when advance_read_realtime()'s OWN timeout-driven walk moves to
    // the next sector (so a driver that never touches the register at all
    // still trips the fallback at exactly the same time on every sector, not
    // just the first). A CPU-driven multi-sector walk -- the host actually
    // draining bytes through register 3 -- does NOT get an explicit reset
    // here at the sector boundary; it does not need one, since every one of
    // those reg-3 accesses already refreshes this timestamp to `now` on its
    // own (see the top of the reg==3 case in read()).
    uint64_t read_last_access_ = 0;

    // active WRITE SECTOR
    uint8_t* write_target() {
        D88Disk* d = disks_[read_drive_];
        return d ? d->write_sector(read_cyl_, read_side_, read_sector_,
                                   command_single_density_)
                 : nullptr;
    }
    bool write_multiple_ = false;
    bool read_multiple_ = false;
    int write_index_ = 0;
    uint64_t write_start_ = 0;
    // Same idea as read_last_access_, for the write side (advance_write_realtime()).
    uint64_t write_last_access_ = 0;
    uint64_t byte_due(uint64_t start, int index) const {
        return start + (uint64_t)index * byte_cycles();
    }

    // active READ ADDRESS: the six ID bytes and how many have been taken
    uint8_t id_bytes_[6] = {};
    int id_index_ = 0;
    // Cycle of the most recent register-3 access during this READ ADDRESS,
    // seeded to read_start_ at command issue; advance_readaddr_realtime()'s
    // fallback fires once a whole ID field's worth of time (6 bytes) has
    // passed since without one, same idea as read_last_access_ above but
    // scaled to this command's own (much shorter) transfer.
    uint64_t id_last_access_ = 0;
    int id_next_ = 0; // which record on the track comes round next
    // Which drive/cylinder/side id_next_'s walk position belongs to. A seek,
    // side change, or drive switch moves the head to a different track, so
    // the next READ ADDRESS on a track other than this one must restart the
    // walk from that track's first physical record rather than carry over
    // an offset that belonged to wherever the head used to be. Initialised
    // to an impossible cylinder so the very first READ ADDRESS after
    // construction/reset always starts fresh regardless of where the head
    // happens to sit.
    int id_track_drive_ = -1;
    int id_track_cyl_ = -1;
    int id_track_side_ = -1;

    // active WRITE TRACK: the raw stream the formatter pours in, parsed into
    // sectors when the track's worth of bytes has gone by
    std::vector<uint8_t> track_stream_;
    int track_index_ = 0;
    static constexpr int TRACK_STREAM_BYTES_MFM = 6250;
    static constexpr int TRACK_STREAM_BYTES_FM = 3125;
    int track_stream_bytes() const {
        return command_single_density_ ? TRACK_STREAM_BYTES_FM
                                       : TRACK_STREAM_BYTES_MFM;
    }
    void commit_track_stream();
    // Cycle of the most recent register-3 write during this WRITE TRACK,
    // seeded to write_start_ at command issue; advance_writetrack_realtime()'s
    // fallback fires once a whole track stream's worth of time has passed
    // since without one -- there is no smaller sub-record to walk between
    // for a format stream, so the "record" this scales to is the whole track.
    uint64_t track_last_access_ = 0;

    // active READ TRACK
    std::vector<uint8_t> read_track_stream_;
    int read_track_index_ = 0;
    // Same idea as track_last_access_ above, for the read side
    // (advance_readtrack_realtime()).
    uint64_t read_track_last_access_ = 0;

    // defaults calibrated black-box against EmuZ-2500 (P2/P4): solved from
    // three milestones (audio_boot, title with and without boot preload) so
    // both load-heavy and seek-heavy access patterns land on EmuZ's frames
    uint64_t read_latency_cycles_ = 16'480 * 6;
    uint64_t step_cycles_ = 27'830 * 6;
};

} // namespace mz
