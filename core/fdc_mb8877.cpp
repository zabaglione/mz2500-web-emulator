#include "core/fdc_mb8877.h"

#include <cstdio>
#include <cstdlib>

namespace mz {

// A driver may touch the data register before the transfer's first byte can
// possibly be ready -- e.g. a "clear the stale data register" read issued
// right after the command byte, well before read_latency_cycles_ has
// elapsed. Recording that touch's raw timestamp as the quiet-window's "last
// access" would arm a deadline (last_access + the command's own short quiet
// window) that can expire before byte 0 is even available, tripping the
// LOST DATA fallback before a single byte has been transferred. Clamp: the
// recorded access can never predate the moment the first byte becomes
// ready, so an early touch is treated as happening exactly then, not before.
static uint64_t clamp_access(uint64_t now, uint64_t first_byte_ready) {
    return now > first_byte_ready ? now : first_byte_ready;
}

void FdcMb8877::reset() {
    state_ = State::Idle;
    track_reg_ = sector_reg_ = data_reg_ = 0;
    done_status_ = 0;
    for (int i = 0; i < NUM_DRIVES; i++) phys_cyl_[i] = 0;
    side_ = 0;
    drive_ = 0;
    motor_ = false;
    single_density_ = false;
    command_single_density_ = false;
    busy_until_ = 0;
    read_valid_ = false;
    read_index_ = 0;
    read_start_ = 0;
    read_last_access_ = 0;
    write_index_ = 0;
    write_start_ = 0;
    write_last_access_ = 0;
    write_multiple_ = false;
    read_multiple_ = false;
    id_index_ = 0;
    id_last_access_ = 0;
    id_next_ = 0;
    id_track_drive_ = -1;
    id_track_cyl_ = -1;
    id_track_side_ = -1;
    track_stream_.clear();
    track_index_ = 0;
    track_last_access_ = 0;
    read_track_stream_.clear();
    read_track_index_ = 0;
    read_track_last_access_ = 0;
    step_dir_ = 1; // same default a freshly constructed controller has
    last_type1_ = true; // matches the chip's power-on/reset status format
}

// The disk keeps rotating whether or not the host ever comes back for the
// current sector's bytes, but a host that is merely slow -- an
// interrupt-driven loader draining one byte at a time with some jitter,
// say, rather than a tight busy-loop -- must not have its not-yet-read
// bytes silently discarded just because the sector's nominal transfer
// window has passed in total. So this fires on the host having gone quiet,
// not on total elapsed time: only once a FULL sector's worth of time has
// passed since the data register was *last accessed* (read_last_access_,
// refreshed on every reg-3 touch in read(), and reset to read_start_ when
// the command starts or the multi-sector walk moves to a new sector) does
// the chip consider the transfer abandoned. A host that keeps accessing the
// register -- however late each individual byte runs -- keeps pushing this
// deadline out and is never truncated. A host that never touches register 3
// at all (this file's BASIC-M25 case) sees read_last_access_ frozen at
// read_start_, so it trips the fallback at exactly the time this mechanism
// always has. Once tripped, the undrained bytes are lost (LOST DATA), and
// in multi-sector mode the search for the next sequential record has
// already begun, exactly as it would on real hardware.
void FdcMb8877::advance_read_realtime(uint64_t now) {
    while (read_index_ < 256 && now >= read_last_access_ + byte_cycles() * 256) {
        done_status_ |= ST_LOST;
        if (read_multiple_ && sector_reg_ < 255) {
            sector_reg_++;
            read_sector_ = sector_reg_;
            if (active_sector() != nullptr) {
                read_index_ = 0;
                read_start_ = byte_ready(256) + byte_cycles() * 2;
                read_last_access_ = read_start_; // fresh quiet window for the new sector
                // ST_REC_TYPE reflects the sector just landed on, not the
                // one the walk left behind; ST_LOST (already ORed in above)
                // stays latched for the rest of the command.
                if (active_sector_deleted()) done_status_ |= ST_REC_TYPE;
                else done_status_ &= (uint8_t)~ST_REC_TYPE;
                continue; // maybe that sector's whole window has *also* passed
            }
            // Record not found: search for a full revolution before giving
            // up, same convention as the CPU-driven continuation in read().
            done_status_ = ST_RNF;
            busy_until_ = now + CYC_PER_REV;
            read_index_ = 256;
            return;
        }
        read_index_ = 256;
        busy_until_ = byte_ready(256) + byte_cycles() * 2;
        return;
    }
}

// Same idea as advance_read_realtime(), for the write side: an undrained
// WRITE SECTOR (the host has gone quiet on register 3 -- see
// write_last_access_ below, updated on every reg-3 write in write() and
// reset to write_start_ when the command starts or the multi-sector walk
// moves to a new sector) still completes the transfer on schedule with LOST
// DATA set, same as advance_read_realtime() does for reads. Unlike real
// hardware, which would write whatever was sitting in the data register (or
// garbage) to the platter regardless of whether the CPU kept up, this code
// deliberately does NOT touch the sector's stored bytes in that case: the
// sector is left exactly as it was before the write started. Corrupting a
// user's disk image with unpredictable data on a timing edge is a worse
// outcome than a documented deviation from real hardware, so this is a
// conscious trade, not an oversight.
void FdcMb8877::advance_write_realtime(uint64_t now) {
    while (write_index_ < 256 && now >= write_last_access_ + byte_cycles() * 256) {
        done_status_ |= ST_LOST;
        if (write_multiple_ && sector_reg_ < 255) {
            sector_reg_++;
            read_sector_ = sector_reg_;
            if (write_target() != nullptr) {
                write_index_ = 0;
                write_start_ = byte_due(write_start_, 256) + byte_cycles() * 2;
                write_last_access_ = write_start_; // fresh quiet window for the new sector
                continue;
            }
            done_status_ = ST_RNF;
            busy_until_ = now + CYC_PER_REV;
            write_index_ = 256;
            return;
        }
        write_index_ = 256;
        busy_until_ = byte_due(write_start_, 256) + byte_cycles() * 2;
        return;
    }
}

// READ ADDRESS has no multi-record walk to fall back on: its whole transfer
// IS the six-byte ID field, so that is the "record" whose worth of quiet
// time (id_last_access_, refreshed on every reg-3 touch in read(), seeded to
// read_start_ when the command starts) completes it. A driver that issues
// READ ADDRESS purely to watch the sector register (which this command also
// updates, at command-issue time) and never touches register 3 at all would
// otherwise spin BUSY forever, the same failure mode advance_read_realtime()
// fixes for READ SECTOR.
void FdcMb8877::advance_readaddr_realtime(uint64_t now) {
    if (id_index_ < 6 && now >= id_last_access_ + byte_cycles() * 6) {
        done_status_ |= ST_LOST;
        id_index_ = 6;
        busy_until_ = byte_ready(6) + byte_cycles();
    }
}

// Same idea for WRITE TRACK: there is no sub-record to walk between (it is
// one continuous stream for the whole track), so the track stream itself is
// the "record" whose worth of quiet time (track_last_access_, refreshed on
// every reg-3 write in write(), seeded to write_start_ at command issue)
// completes the command. Completion here means the same thing any other
// WRITE TRACK exit path means: commit whatever bytes actually arrived in
// track_stream_ (possibly none), exactly as a FORCE INTERRUPT or the next
// command arriving mid-format already does.
void FdcMb8877::advance_writetrack_realtime(uint64_t now) {
    if (track_index_ < track_stream_bytes() &&
        now >= track_last_access_ + byte_cycles() * track_stream_bytes()) {
        done_status_ |= ST_LOST;
        commit_track_stream();
        track_index_ = track_stream_bytes();
        busy_until_ = byte_due(write_start_, track_stream_bytes()) + byte_cycles() * 2;
    }
}

// Same idea for READ TRACK: the synthesised stream for the whole track is
// the "record", so that is the quiet window read_track_last_access_ (seeded
// to read_start_ at command issue, refreshed on every reg-3 read in read())
// is measured against.
void FdcMb8877::advance_readtrack_realtime(uint64_t now) {
    const int total = (int)read_track_stream_.size();
    if (total > 0 && read_track_index_ < total &&
        now >= read_track_last_access_ + byte_cycles() * (uint64_t)total) {
        done_status_ |= ST_LOST;
        read_track_index_ = total;
        busy_until_ = byte_ready(total) + byte_cycles() * 2;
    }
}

uint8_t FdcMb8877::status_at(uint64_t now) {
    const uint8_t wp = disk_write_protected() ? ST_WP : 0;
    switch (state_) {
    case State::Idle:
        // Bit2 only means TRACK00 after a Type I command; after a Type
        // II/III command that same bit is LOST DATA and must not be
        // synthesised from head position, or a driver polling status after a
        // completed read/write would see a spurious LOST DATA whenever the
        // head happens to sit on track 0.
        return done_status_ | wp |
               (last_type1_ && phys_cyl_[drive_] == 0 ? ST_TRACK0 : 0);
    case State::TypeI:
        if (now < busy_until_) return ST_BUSY | wp;
        state_ = State::Idle;
        done_status_ = 0;
        return wp | (phys_cyl_[drive_] == 0 ? ST_TRACK0 : 0);
    case State::Read:
        if (!read_valid_) {
            // record-not-found spin
            if (now < busy_until_) return ST_BUSY;
            state_ = State::Idle;
            return done_status_;
        }
        advance_read_realtime(now);
        if (read_index_ >= 256) {
            // record-not-found spin (walked off the track), or CRC tail
            // after the last byte
            if (now < busy_until_) return ST_BUSY;
            state_ = State::Idle;
            return done_status_;
        }
        return ST_BUSY | (now >= byte_ready(read_index_) ? ST_DRQ : 0);
    case State::Write:
        advance_write_realtime(now);
        if (write_index_ >= 256) {
            // record-not-found spin, or the CRC tail after the last byte
            if (now < busy_until_) return ST_BUSY | wp;
            state_ = State::Idle;
            return done_status_ | wp;
        }
        return ST_BUSY | wp |
               (now >= byte_due(write_start_, write_index_) ? ST_DRQ : 0);
    case State::ReadAddr:
        // READ ADDRESS is a read-type command: bit6 carries WRITE PROTECT
        // only for write commands (see State::Write above) and reads as 0
        // here, same as State::Read.
        advance_readaddr_realtime(now);
        if (id_index_ >= 6) {
            if (now < busy_until_) return ST_BUSY;
            state_ = State::Idle;
            return done_status_;
        }
        return ST_BUSY | (now >= byte_ready(id_index_) ? ST_DRQ : 0);
    case State::WriteTrack:
        // WRITE TRACK is a write-type command: bit6 carries WRITE PROTECT
        // during its busy phase, same as State::Write.
        advance_writetrack_realtime(now);
        if (track_index_ >= track_stream_bytes()) {
            if (now < busy_until_) return ST_BUSY | wp;
            state_ = State::Idle;
            return done_status_ | wp;
        }
        return ST_BUSY | wp |
               (now >= byte_due(write_start_, track_index_) ? ST_DRQ : 0);
    case State::ReadTrack:
        // READ TRACK is a read-type command: bit6 reads as 0 here, same as
        // State::Read/State::ReadAddr above -- only write commands report
        // WRITE PROTECT.
        advance_readtrack_realtime(now);
        if (read_track_index_ >= (int)read_track_stream_.size()) {
            if (now < busy_until_) return ST_BUSY;
            state_ = State::Idle;
            return done_status_;
        }
        return ST_BUSY |
               (now >= byte_ready(read_track_index_) ? ST_DRQ : 0);
    }
    return 0;
}

uint8_t FdcMb8877::read(int reg, uint64_t now) {
    switch (reg) {
    case 0:
        return status_at(now);
    case 1:
        return track_reg_;
    case 2:
        return sector_reg_;
    case 3:
        // Any touch of the data register during an active READ SECTOR counts
        // as the host being alive, even if DRQ has not asserted yet for the
        // byte it is after -- advance_read_realtime()'s quiet-timeout must
        // not fire just because a poll happened to land a cycle early. The
        // clamp to read_start_ (byte 0's ready time) matters here too: an
        // early touch must not arm a deadline before byte 0 could exist.
        if (state_ == State::Read) read_last_access_ = clamp_access(now, read_start_);
        // Same idea for READ ADDRESS/READ TRACK and their own quiet-window
        // fallbacks (advance_readaddr_realtime()/advance_readtrack_realtime()).
        // READ ADDRESS's window is only 6 bytes wide (1152 cycles) against a
        // pre-data latency of tens of thousands of cycles, so an unclamped
        // early touch here would expire almost immediately -- long before
        // byte 0 is ready -- and abort the command with LOST DATA before any
        // ID byte is ever delivered.
        if (state_ == State::ReadAddr) id_last_access_ = clamp_access(now, read_start_);
        if (state_ == State::ReadTrack) read_track_last_access_ = clamp_access(now, read_start_);
        if (state_ == State::ReadAddr && id_index_ < 6 &&
            now >= byte_ready(id_index_)) {
            const uint8_t value = id_bytes_[id_index_++];
            if (id_index_ >= 6) busy_until_ = byte_ready(6) + byte_cycles();
            return value;
        }
        if (state_ == State::ReadTrack &&
            read_track_index_ < (int)read_track_stream_.size() &&
            now >= byte_ready(read_track_index_)) {
            const uint8_t value = read_track_stream_[read_track_index_++];
            if (read_track_index_ >= (int)read_track_stream_.size())
                busy_until_ = byte_ready(read_track_index_) + byte_cycles();
            return value;
        }
        if (state_ == State::Read && read_valid_ && read_index_ < 256 &&
            now >= byte_ready(read_index_)) {
            const uint8_t* sector = active_sector();
            const uint8_t value = sector ? sector[read_index_] : 0xFF;
            read_index_++;
            if (read_index_ >= 256) {
                if (read_multiple_ && sector_reg_ < 255) {
                    // multi-sector: the chip walks on to the next record
                    sector_reg_++;
                    read_sector_ = sector_reg_;
                    if (active_sector() != nullptr) {
                        read_index_ = 0;
                        read_start_ = byte_ready(256) + byte_cycles() * 2;
                        // ST_REC_TYPE reflects the sector just landed on.
                        if (active_sector_deleted()) done_status_ |= ST_REC_TYPE;
                        else done_status_ &= (uint8_t)~ST_REC_TYPE;
                        return value;
                    }
                    // Record not found: the chip searches for it for a full
                    // revolution before giving up, same as the initial
                    // search and matching WRITE SECTOR's multi-sector
                    // continuation -- not just the couple of byte periods
                    // the CRC tail after a *found* sector takes.
                    done_status_ = ST_RNF;
                    busy_until_ = now + CYC_PER_REV;
                    return value;
                }
                // busy stays up while the CRC bytes pass under the head
                busy_until_ = byte_ready(256) + byte_cycles() * 2;
            }
            return value;
        }
        return data_reg_;
    }
    return 0xFF;
}

void FdcMb8877::write(int reg, uint8_t value, uint64_t now) {
    switch (reg) {
    case 0: { // command
        const uint8_t cmd = value;
        // Any new command ends a format in progress - FORCE INTERRUPT, the
        // next seek, whatever the formatter sends. The chip itself would
        // stop at the index hole; this is where the track gets kept.
        if (state_ == State::WriteTrack) commit_track_stream();
        if ((cmd & 0xF0) == 0x00) { // RESTORE
            const int steps = phys_cyl_[drive_] > 0 ? phys_cyl_[drive_] : 1;
            stat_seeks++;
            stat_steps += steps;
            step_dir_ = -1;
            phys_cyl_[drive_] = 0;
            track_reg_ = 0;
            done_status_ = 0;
            last_type1_ = true;
            state_ = State::TypeI;
            busy_until_ = now + (uint64_t)steps * step_cycles_;
        } else if ((cmd & 0xF0) == 0x10) { // SEEK (target in data register)
            const int target = data_reg_;
            const int steps = std::abs(target - phys_cyl_[drive_]) + 1;
            stat_seeks++;
            stat_steps += steps;
            step_dir_ = (target >= phys_cyl_[drive_]) ? 1 : -1;
            phys_cyl_[drive_] = target;
            track_reg_ = (uint8_t)target;
            done_status_ = 0;
            last_type1_ = true;
            state_ = State::TypeI;
            busy_until_ = now + (uint64_t)steps * step_cycles_;
        } else if ((cmd & 0xE0) == 0x20 || (cmd & 0xE0) == 0x40 ||
                   (cmd & 0xE0) == 0x60) {
            // STEP (20h) repeats the last direction; STEP-IN (40h) moves
            // toward the spindle, STEP-OUT (60h) away from it. Bit4 (U)
            // says whether the track register follows the head.
            if ((cmd & 0xE0) == 0x40) step_dir_ = 1;
            else if ((cmd & 0xE0) == 0x60) step_dir_ = -1;
            int target = phys_cyl_[drive_] + step_dir_;
            if (target < 0) target = 0;
            if (target > 82) target = 82;
            stat_steps++;
            phys_cyl_[drive_] = target;
            if (cmd & 0x10) track_reg_ = (uint8_t)target;
            done_status_ = 0;
            last_type1_ = true;
            state_ = State::TypeI;
            busy_until_ = now + step_cycles_;
        } else if ((cmd & 0xE0) == 0x80) { // READ SECTOR
            command_single_density_ = single_density_;
            read_multiple_ = (cmd & 0x10) != 0;
            stat_reads++;
            last_type1_ = false;
            read_drive_ = drive_;
            read_cyl_ = phys_cyl_[drive_];
            read_side_ = side_;
            read_sector_ = sector_reg_;
            read_index_ = 0;
            state_ = State::Read;
            const bool found = active_sector() != nullptr && track_reg_ == phys_cyl_[drive_];
            read_valid_ = found;
            if (!found) {
                read_index_ = 256;
                done_status_ = ST_RNF;
                busy_until_ = now + CYC_PER_REV; // spins one revolution, then RNF
            } else {
                // ST_REC_TYPE (deleted-data mark) is the read-side
                // counterpart of WRITE SECTOR's a0 flag / set_deleted_mark():
                // a sector written with the mark must read back reporting
                // it, or firmware that writes a bad-sector mark and reads it
                // back to confirm never will.
                done_status_ = active_sector_deleted() ? ST_REC_TYPE : 0;
                read_start_ = now + read_latency_cycles_ + byte_cycles();
                read_last_access_ = read_start_; // quiet-window clock starts fresh
            }
        } else if ((cmd & 0xE0) == 0xA0) { // WRITE SECTOR
            command_single_density_ = single_density_;
            last_type1_ = false;
            write_multiple_ = (cmd & 0x10) != 0;
            read_drive_ = drive_;
            read_cyl_ = phys_cyl_[drive_];
            read_side_ = side_;
            read_sector_ = sector_reg_;
            write_index_ = 0;
            if (disk_write_protected()) {
                // the chip never starts the write; the status byte says why
                state_ = State::Idle;
                done_status_ = 0;
                break;
            }
            // Check the cheap, side-effect-free track-register match FIRST.
            // write_target() calls D88Disk::write_sector(), which marks the
            // disk dirty the instant it returns a valid pointer -- before
            // any byte is stored. If we probed it first and then discarded
            // the result because the track register didn't match, we'd
            // spuriously dirty the image for a write that never happened.
            const bool found = track_reg_ == phys_cyl_[drive_] &&
                               write_target() != nullptr;
            state_ = State::Write;
            if (!found) {
                write_index_ = 256;
                done_status_ = ST_RNF;
                busy_until_ = now + CYC_PER_REV;
            } else {
                D88Disk* d = disks_[read_drive_];
                if (d) d->set_deleted_mark(read_cyl_, read_side_, read_sector_,
                                           (cmd & 0x01) != 0,
                                           command_single_density_);
                done_status_ = 0;
                write_start_ = now + read_latency_cycles_ + byte_cycles();
                write_last_access_ = write_start_; // quiet-window clock starts fresh
            }
        } else if ((cmd & 0xF0) == 0xC0) { // READ ADDRESS
            command_single_density_ = single_density_;
            last_type1_ = false;
            read_drive_ = drive_;
            read_cyl_ = phys_cyl_[drive_];
            read_side_ = side_;
            id_index_ = 0;
            // The walk position only means anything relative to the track it
            // was last used on. If the head has moved to a different
            // drive/cylinder/side since the previous READ ADDRESS, restart
            // the walk from that track's first physical record instead of
            // continuing an offset that belonged to the old track.
            if (read_drive_ != id_track_drive_ || read_cyl_ != id_track_cyl_ ||
                read_side_ != id_track_side_) {
                id_next_ = 0;
                id_track_drive_ = read_drive_;
                id_track_cyl_ = read_cyl_;
                id_track_side_ = read_side_;
            }
            const D88Disk* d = disks_[read_drive_];
            const D88Disk::Sector* sec = nullptr;
            const int count = d ? d->sector_count(read_cyl_, read_side_) : 0;
            for (int scanned = 0; scanned < count; scanned++) {
                const D88Disk::Sector* candidate =
                    d->sector_at(read_cyl_, read_side_, id_next_++);
                if (candidate &&
                    ((candidate->density & 0x40) != 0) == command_single_density_) {
                    sec = candidate;
                    break;
                }
            }
            state_ = State::ReadAddr;
            if (!sec) {
                id_index_ = 6;
                done_status_ = ST_RNF;
                busy_until_ = now + CYC_PER_REV;
            } else {
                id_bytes_[0] = sec->c;
                id_bytes_[1] = sec->h;
                id_bytes_[2] = sec->r;
                id_bytes_[3] = sec->n;
                id_bytes_[4] = 0; // CRC is not modelled; the data is always good
                id_bytes_[5] = 0;
                sector_reg_ = sec->c; // the chip copies the track address here
                done_status_ = 0;
                read_start_ = now + read_latency_cycles_ + byte_cycles();
                id_last_access_ = read_start_; // quiet-window clock starts fresh
            }
        } else if ((cmd & 0xF0) == 0xF0) { // WRITE TRACK (physical format)
            command_single_density_ = single_density_;
            last_type1_ = false;
            read_drive_ = drive_;
            read_cyl_ = phys_cyl_[drive_];
            read_side_ = side_;
            track_stream_.clear();
            track_index_ = 0;
            if (disk_write_protected()) {
                state_ = State::Idle;
                done_status_ = 0;
                break;
            }
            state_ = State::WriteTrack;
            done_status_ = 0;
            write_start_ = now + read_latency_cycles_ + byte_cycles();
            track_last_access_ = write_start_; // quiet-window clock starts fresh
        } else if ((cmd & 0xF0) == 0xE0) { // READ TRACK
            command_single_density_ = single_density_;
            last_type1_ = false;
            read_drive_ = drive_;
            read_cyl_ = phys_cyl_[drive_];
            read_side_ = side_;
            read_track_stream_.clear();
            read_track_index_ = 0;
            const D88Disk* d = disks_[read_drive_];
            const uint8_t gap = command_single_density_ ? 0xFF : 0x4E;
            for (int i = 0; i < 40; i++) read_track_stream_.push_back(gap);
            const int count = d ? d->sector_count(read_cyl_, read_side_) : 0;
            for (int i = 0; i < count; i++) {
                const D88Disk::Sector* sec = d->sector_at(read_cyl_, read_side_, i);
                if (!sec) break;
                if (((sec->density & 0x40) != 0) != command_single_density_) continue;
                for (int g = 0; g < 12; g++) read_track_stream_.push_back(0x00);
                read_track_stream_.push_back(0xFE);
                read_track_stream_.push_back(sec->c);
                read_track_stream_.push_back(sec->h);
                read_track_stream_.push_back(sec->r);
                read_track_stream_.push_back(sec->n);
                read_track_stream_.push_back(0xF7);
                for (int g = 0; g < 22; g++) read_track_stream_.push_back(gap);
                read_track_stream_.push_back(sec->deleted ? 0xF8 : 0xFB);
                for (uint8_t b : sec->data) read_track_stream_.push_back(b);
                read_track_stream_.push_back(0xF7);
                for (int g = 0; g < 24; g++) read_track_stream_.push_back(gap);
            }
            state_ = State::ReadTrack;
            if (read_track_stream_.size() > 40) {
                // READ TRACK runs from one index pulse to the next. D88 stores
                // records rather than the encoded bit-cell stream, so pad a
                // short synthesis with the density-specific gap byte or cut a
                // long one at the next index boundary. This also keeps the
                // transfer at exactly one 300 rpm revolution: 6250 bytes at
                // MFM 250 kbps or 3125 bytes at FM 125 kbps.
                read_track_stream_.resize((size_t)track_stream_bytes(), gap);
                done_status_ = 0;
            } else {
                // Blank track: the stream is only the leading gap, so DRQ
                // genuinely asserts for those bytes. A driver that follows
                // this file's not-found convention (poll status, never
                // touch the data register, wait for BUSY to clear) would
                // otherwise never drain the gap bytes and BUSY would never
                // clear. Force the index straight to end-of-stream, the
                // same way READ SECTOR/WRITE SECTOR/READ ADDRESS force
                // their index/counter to its terminal value on a not-found
                // result, so status_at() takes the busy_until_-bounded path
                // immediately instead of waiting on byte drains.
                read_track_index_ = (int)read_track_stream_.size();
                done_status_ = ST_RNF;
                busy_until_ = now + CYC_PER_REV;
            }
            read_start_ = now + read_latency_cycles_ + byte_cycles();
            read_track_last_access_ = read_start_; // quiet-window clock starts fresh
        } else if ((cmd & 0xF0) == 0xD0) { // FORCE INTERRUPT
            // The MB8877 returns Type I status after FORCE INTERRUPT
            // (Oh!MZ / the datasheet's status-format table), so an idle
            // status read right afterward must report TRACK00/INDEX/SEEK
            // ERROR/HEAD LOADED, not whatever Type II/III command this
            // interrupted last set last_type1_ to -- otherwise a READ SECTOR
            // aborted by FORCE INTERRUPT with the head sitting on cylinder 0
            // would suppress TRACK00 until some later Type I command ran.
            last_type1_ = true;
            state_ = State::Idle;
            done_status_ = 0;
        }
        // No trailing "else": the branches above (RESTORE/SEEK/STEP family,
        // READ/WRITE SECTOR, READ ADDRESS, WRITE TRACK, READ TRACK, FORCE
        // INTERRUPT) partition all 256 possible command byte values between
        // their bitmasks, so every value this switch can ever see is
        // decoded by one of them. A branch here claiming to catch an
        // "unsupported command" would never actually run -- dead code that
        // reads as reachable when it is not.
        break;
    }
    case 1:
        track_reg_ = value;
        break;
    case 2:
        sector_reg_ = value;
        break;
    case 3:
        // Same reasoning as the read side in read(): any touch of the data
        // register during an active WRITE SECTOR counts as the host being
        // alive, so advance_write_realtime()'s quiet-timeout does not fire
        // just because a write happened to land a cycle before its DRQ
        // window opened. Clamped to write_start_ (byte 0's ready time) for
        // the same reason as the read side: an early touch must not arm a
        // deadline before byte 0 could exist.
        if (state_ == State::Write) write_last_access_ = clamp_access(now, write_start_);
        // Same idea for WRITE TRACK's own quiet-window fallback
        // (advance_writetrack_realtime()).
        if (state_ == State::WriteTrack) track_last_access_ = clamp_access(now, write_start_);
        if (state_ == State::WriteTrack && track_index_ < track_stream_bytes() &&
            now >= byte_due(write_start_, track_index_)) {
            track_stream_.push_back(value);
            track_index_++;
            if (track_index_ >= track_stream_bytes()) {
                commit_track_stream();
                busy_until_ = byte_due(write_start_, track_index_) + byte_cycles() * 2;
            }
        }
        if (state_ == State::Write && write_index_ < 256 &&
            now >= byte_due(write_start_, write_index_)) {
            uint8_t* dst = write_target();
            if (dst) dst[write_index_] = value;
            write_index_++;
            if (write_index_ >= 256) {
                if (write_multiple_ && sector_reg_ < 255) {
                    // multi-sector: the chip walks on to the next record
                    sector_reg_++;
                    read_sector_ = sector_reg_;
                    if (write_target() != nullptr) {
                        write_index_ = 0;
                        write_start_ = byte_due(write_start_, 256) + byte_cycles() * 2;
                        data_reg_ = value;
                        break;
                    }
                    // Record not found: the chip searches for it for a full
                    // revolution before giving up, same as the initial
                    // search when the command was first issued -- not just
                    // the couple of byte periods the CRC tail after a
                    // *found* sector takes.
                    done_status_ = ST_RNF;
                    busy_until_ = now + CYC_PER_REV;
                    data_reg_ = value;
                    break;
                }
                busy_until_ = byte_due(write_start_, 256) + byte_cycles() * 2;
            }
        }
        data_reg_ = value;
        break;
    }
}

// Parse the stream a formatter poured in. FEh introduces an ID field
// (C,H,R,N then a CRC placeholder); FBh or F8h introduces the data field,
// whose length the N byte gives. Everything else - gaps, sync bytes - is
// skipped. The order sectors appear in is the order the track keeps: the
// interleave belongs to the formatting program.
void FdcMb8877::commit_track_stream() {
    std::vector<D88Disk::Sector> sectors;
    const size_t n = track_stream_.size();
    size_t i = 0;
    // One summary line per format operation (this function runs once per
    // WRITE TRACK command, i.e. once per physical track formatted), not one
    // per offending sector -- a full-disk format lays down ~16 sectors per
    // track, so logging inside the loop below meant one unsupported N could
    // print ~2560 lines for a whole-disk format, all landing in the
    // browser's console. Collect the details of the first offender and a
    // count instead, and print exactly one line after the loop.
    int unsupported_size_count = 0;
    size_t unsupported_size_first_bytes = 0;
    uint8_t unsupported_size_first_n = 0, unsupported_size_first_c = 0,
            unsupported_size_first_h = 0, unsupported_size_first_r = 0;
    while (i < n) {
        if (track_stream_[i] != 0xFE) { i++; continue; }
        if (i + 5 > n) break;
        D88Disk::Sector sec;
        sec.c = track_stream_[i + 1];
        sec.h = track_stream_[i + 2];
        sec.r = track_stream_[i + 3];
        sec.n = track_stream_[i + 4];
        sec.density = command_single_density_ ? 0x40 : 0x00;
        i += 5;
        // walk to the data address mark
        while (i < n && track_stream_[i] != 0xFB && track_stream_[i] != 0xF8 &&
               track_stream_[i] != 0xFE) {
            i++;
        }
        if (i >= n || track_stream_[i] == 0xFE) continue; // ID with no data
        sec.deleted = track_stream_[i] == 0xF8 ? 0x10 : 0x00;
        i++;
        const size_t size = (size_t)128 << (sec.n & 3);
        // D88Disk::raw_sector()/write_sector() both refuse a sector whose
        // stored size is under SECTOR_SIZE (256 bytes) -- this codebase only
        // supports N=1. A format that lays down any other N (including N=0,
        // the MB8877's other common 128-byte code, or any N whose low two
        // bits happen to alias N=0 through the "& 3" above, e.g. N=4) still
        // gets *parsed and stored* here, but every subsequent READ/WRITE
        // SECTOR against it will report RECORD NOT FOUND -- a confusing
        // failure that gives no hint why. Name it here instead of leaving
        // the caller to guess.
        if (size != (size_t)D88Disk::SECTOR_SIZE) {
            if (unsupported_size_count == 0) {
                unsupported_size_first_bytes = size;
                unsupported_size_first_n = sec.n;
                unsupported_size_first_c = sec.c;
                unsupported_size_first_h = sec.h;
                unsupported_size_first_r = sec.r;
            }
            unsupported_size_count++;
        }
        const size_t avail = n - i > size ? size : n - i;
        sec.data.assign(track_stream_.begin() + i, track_stream_.begin() + i + avail);
        sec.data.resize(size, 0);
        i += avail;
        sectors.push_back(std::move(sec));
    }
    if (unsupported_size_count > 0) {
        std::fprintf(stderr,
                     "[fdc] format: unsupported sector size %zu bytes (N=%u) at "
                     "C=%u H=%u R=%u (+%d more this track) -- these sectors will read "
                     "back as RECORD NOT FOUND\n",
                     unsupported_size_first_bytes, unsupported_size_first_n,
                     unsupported_size_first_c, unsupported_size_first_h,
                     unsupported_size_first_r, unsupported_size_count - 1);
    }
    D88Disk* d = disks_[read_drive_];
    if (d && !sectors.empty()) d->format_track(read_cyl_, read_side_, sectors);
    track_stream_.clear();
}

void FdcMb8877::write_drive(uint8_t value) {
    motor_ = (value & 0x80) != 0 && (value & 0x04) != 0;
    drive_ = value & 0x01;
}

void FdcMb8877::write_side(uint8_t value) { side_ = value & 0x01; }

} // namespace mz
