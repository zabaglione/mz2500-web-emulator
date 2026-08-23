#include "core/fdc_mb8877.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mz {

namespace {

uint16_t crc16_byte(uint16_t crc, uint8_t value) {
    crc ^= static_cast<uint16_t>(value) << 8;
    for (int bit = 0; bit < 8; bit++)
        crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                             : static_cast<uint16_t>(crc << 1);
    return crc;
}

uint16_t field_crc(bool single_density, uint8_t mark,
                   const uint8_t* bytes, size_t size) {
    uint16_t crc = 0xFFFF;
    if (!single_density) {
        crc = crc16_byte(crc, 0xA1);
        crc = crc16_byte(crc, 0xA1);
        crc = crc16_byte(crc, 0xA1);
    }
    crc = crc16_byte(crc, mark);
    for (size_t i = 0; i < size; i++) crc = crc16_byte(crc, bytes[i]);
    return crc;
}

uint16_t id_crc(bool single_density, const D88Disk::Sector& sector) {
    const uint8_t id[4] = {sector.c, sector.h, sector.r, sector.n};
    return field_crc(single_density, 0xFE, id, sizeof(id));
}

bool d88_data_crc_error(const D88Disk::Sector& sector) {
    // The public D88 format description defines B0h explicitly as a data
    // CRC error. Other status values are not interpreted without an equally
    // reliable mapping.
    return sector.status == 0xB0;
}

} // namespace

void FdcMb8877::reset() {
    state_ = State::Idle;
    track_reg_ = 0;
    sector_reg_ = 1;
    data_reg_ = 0;
    done_status_ = 0;
    for (int i = 0; i < NUM_DRIVES; i++) phys_cyl_[i] = 0;
    side_ = 0;
    drive_ = 0;
    raw_drive_number_ = 0;
    internal_drive_bank_high_ = false;
    drive_mapped_ = true;
    motor_ = false;
    drive_select_enabled_ = false;
    index_origin_ = 0;
    index_origin_valid_ = false;
    single_density_ = false;
    command_single_density_ = false;
    command_compare_side_ = false;
    command_side_id_ = 0;
    command_track_ = 0;
    head_settle_pending_ = false;
    head_settle_violation_ = false;
    head_settle_ready_cycle_ = 0;
    read_fault_active_ = false;
    busy_until_ = 0;
    type23_started_ = false;
    type23_ready_cycle_ = 0;
    record_search_failed_ = false;
    record_search_revolutions_ = 4;
    pending_read_status_ = 0;
    read_status_residue_ = 0;
    intrq_ = false;
    force_interrupt_mask_ = 0;
    force_last_ready_ = false;
    force_index_sample_cycle_ = 0;
    read_valid_ = false;
    read_index_ = 0;
    read_transfer_size_ = 0;
    read_start_ = 0;
    data_reg_full_ = false;
    read_command_cycle_ = 0;
    read_first_drq_counted_ = false;
    read_completion_counted_ = false;
    previous_read_data_valid_ = false;
    previous_read_data_cycle_ = 0;
    previous_read_drive_ = 0;
    previous_read_cyl_ = 0;
    previous_read_side_ = 0;
    previous_read_sector_ = 0;
    write_index_ = 0;
    write_transfer_size_ = 0;
    write_start_ = 0;
    write_data_full_ = false;
    write_started_ = false;
    write_deleted_ = false;
    write_multiple_ = false;
    read_multiple_ = false;
    id_index_ = 0;
    id_next_ = 0;
    id_track_drive_ = -1;
    id_track_cyl_ = -1;
    id_track_side_ = -1;
    track_stream_.clear();
    track_index_ = 0;
    write_track_done_ = false;
    write_track_next_due_ = 0;
    write_track_end_ = 0;
    read_track_stream_.clear();
    read_track_index_ = 0;
    // Releasing Master Reset executes RESTORE, whose last direction is out.
    // The physical seek is collapsed to its completed track-0 result here.
    step_dir_ = -1;
    status_class_ = StatusClass::TypeI;
    head_loaded_ = false;
    head_load_timing_ = true;
    head_load_timing_since_ = 0;
    head_idle_counting_ = false;
    head_idle_sample_cycle_ = 0;
    head_idle_index_count_ = 0;
    type1_verify_ = false;
    type1_verify_started_ = false;
    type1_motion_start_ = 0;
    type1_motion_end_ = 0;
    type1_step_interval_ = 0;
    type1_steps_total_ = 0;
    type1_steps_applied_ = 0;
    type1_drive_ = 0;
    type1_motion_direction_ = 0;
    type1_update_track_ = false;
    type1_restore_ = false;
    type1_verify_result_ = 0;
}

bool FdcMb8877::selected_drive_ready() const {
    if (!drive_mapped_) return false;
    const D88Disk* disk = disks_[drive_];
    return motor_ && drive_select_enabled_ && disk && disk->loaded();
}

bool FdcMb8877::active_drive_ready() const {
    if (!drive_mapped_) return false;
    const D88Disk* disk = disks_[read_drive_];
    return motor_ && drive_select_enabled_ && drive_ == read_drive_ &&
           disk && disk->loaded();
}

int FdcMb8877::id_search_revolutions_for_current_id() const {
    if (!command_compare_side_) return 4;
    const D88Disk* disk = disks_[read_drive_];
    const int count = disk ? disk->sector_count(read_cyl_, read_side_) : 0;
    bool side_mismatch = false;
    for (int i = 0; i < count; i++) {
        const D88Disk::Sector* candidate =
            disk->sector_at(read_cyl_, read_side_, i);
        if (!candidate || candidate->c != command_track_ ||
            candidate->r != static_cast<uint8_t>(read_sector_) ||
            (((candidate->density & 0x40) != 0) != command_single_density_))
            continue;
        if ((candidate->h & 1) == (command_side_id_ & 1)) return 4;
        side_mismatch = true;
    }
    return side_mismatch ? 5 : 4;
}

uint64_t FdcMb8877::next_index_cycle(uint64_t now) const {
    if (!index_origin_valid_ || now <= index_origin_) return index_origin_;
    const uint64_t elapsed = now - index_origin_;
    const uint64_t remainder = elapsed % CYC_PER_REV;
    return remainder == 0 ? now : now + (CYC_PER_REV - remainder);
}

void FdcMb8877::cancel_head_idle_unload() {
    head_idle_counting_ = false;
    head_idle_index_count_ = 0;
}

void FdcMb8877::begin_head_idle_unload(uint64_t now) {
    head_idle_counting_ = head_loaded_;
    head_idle_sample_cycle_ = now;
    head_idle_index_count_ = 0;
}

void FdcMb8877::update_head_unload(uint64_t now) {
    if (!head_idle_counting_ || !head_loaded_ || state_ != State::Idle)
        return;
    if (now < head_idle_sample_cycle_) {
        // Test harnesses and machine resets may restart their cycle epoch.
        head_idle_sample_cycle_ = now;
        return;
    }

    // HLD is released after 15 actual IP rising edges while the controller
    // is idle. Wall-clock time with a stopped/unselected/not-ready drive is
    // deliberately excluded because no index pulses exist in that interval.
    if (selected_drive_ready() && index_origin_valid_ && now >= index_origin_) {
        uint64_t first_edge = index_origin_;
        if (head_idle_sample_cycle_ >= index_origin_) {
            const uint64_t elapsed = head_idle_sample_cycle_ - index_origin_;
            first_edge = index_origin_ +
                (elapsed / CYC_PER_REV + 1) * CYC_PER_REV;
        }
        if (first_edge <= now) {
            const uint64_t edges = 1 + (now - first_edge) / CYC_PER_REV;
            const uint64_t total = head_idle_index_count_ + edges;
            head_idle_index_count_ = static_cast<uint8_t>(
                std::min<uint64_t>(total, 15));
        }
    }
    head_idle_sample_cycle_ = now;
    if (head_idle_index_count_ >= 15) {
        head_loaded_ = false;
        cancel_head_idle_unload();
    }
}

bool FdcMb8877::begin_type23(uint64_t now) {
    if (type23_started_) return true;
    if (now < type23_ready_cycle_ || !head_load_timing_) return false;

    const uint64_t start =
        std::max(type23_ready_cycle_, head_load_timing_since_);
    type23_started_ = true;
    switch (state_) {
    case State::Read:
        if (head_settle_violation_ || read_fault_active_) {
            read_valid_ = false;
            record_search_failed_ = read_fault_active_;
            record_search_revolutions_ = 1;
            busy_until_ = start + read_latency_cycles_;
        } else if (read_valid_)
            read_start_ = start + read_latency_cycles_ + byte_cycles();
        else
            busy_until_ = start + id_search_timeout();
        break;
    case State::Write:
        if (record_search_failed_)
            busy_until_ = start + id_search_timeout();
        else
            write_start_ = start + read_latency_cycles_ + byte_cycles();
        break;
    case State::ReadAddr:
        if (record_search_failed_)
            busy_until_ = start + 4 * CYC_PER_REV;
        else
            read_start_ = start + read_latency_cycles_ + byte_cycles();
        break;
    case State::WriteTrack:
        write_start_ = next_index_cycle(start);
        write_track_next_due_ = write_start_;
        write_track_end_ = write_start_ + CYC_PER_REV;
        break;
    case State::ReadTrack:
        read_start_ = next_index_cycle(start) + byte_cycles();
        break;
    default:
        break;
    }
    return true;
}

void FdcMb8877::advance_type1(uint64_t now) {
    if (state_ != State::TypeI) return;

    if (type1_steps_applied_ < type1_steps_total_ &&
        now >= type1_motion_start_) {
        const uint64_t elapsed = now - type1_motion_start_;
        const uint64_t interval = std::max<uint64_t>(type1_step_interval_, 1);
        const int due = std::min<int>(
            type1_steps_total_, static_cast<int>(elapsed / interval) + 1);
        while (type1_steps_applied_ < due) {
            if (type1_restore_) {
                track_reg_ = static_cast<uint8_t>(track_reg_ - 1);
            } else if (type1_update_track_) {
                track_reg_ = static_cast<uint8_t>(
                    track_reg_ + type1_motion_direction_);
            }
            phys_cyl_[type1_drive_] = std::clamp(
                phys_cyl_[type1_drive_] + type1_motion_direction_, 0, 82);
            type1_steps_applied_++;
            stat_steps++;
        }
    }
    if (now < type1_motion_end_) return;

    if (type1_restore_) track_reg_ = 0;
    if (!type1_verify_) return;

    // V loads the head near the end even when h was clear. Verification then
    // observes the external HLT input after the 30 ms settle interval used by
    // a 1 MHz mini-floppy controller clock.
    head_loaded_ = true;
    const uint64_t settle_end = type1_motion_end_ + CYC_TYPE1_SETTLE;
    if (now < settle_end || !head_load_timing_) return;
    if (!type1_verify_started_) {
        const uint64_t start =
            std::max(settle_end, head_load_timing_since_);
        const D88Disk* disk = disks_[type1_drive_];
        bool matching_id = false;
        const int count = disk
            ? disk->sector_count(phys_cyl_[type1_drive_], side_) : 0;
        for (int i = 0; i < count; i++) {
            const D88Disk::Sector* candidate =
                disk->sector_at(phys_cyl_[type1_drive_], side_, i);
            if (candidate &&
                (((candidate->density & 0x40) != 0) == command_single_density_) &&
                candidate->c == track_reg_) {
                matching_id = true;
                break;
            }
        }
        if (matching_id) {
            // Standard D88 preserves ID values but has no unambiguous field
            // for an ID-field CRC fault, so matching C and density are the
            // strongest verification this record model can perform.
            type1_verify_result_ = 0;
            busy_until_ = start + read_latency_cycles_;
        } else {
            type1_verify_result_ = ST_SEEK_ERR;
            busy_until_ = start + 5 * CYC_PER_REV;
        }
        type1_verify_started_ = true;
    }
    if (now >= busy_until_) done_status_ = type1_verify_result_;
}

void FdcMb8877::complete_command(uint64_t now) {
    if (state_ == State::Read && !read_completion_counted_) {
        if (read_first_drq_counted_ && read_index_ >= read_transfer_size_ &&
            (done_status_ & (ST_LOST | ST_CRC | ST_RNF)) == 0) {
            stat_read_successes++;
        }
        read_completion_counted_ = true;
    }
    state_ = State::Idle;
    const uint64_t completion =
        busy_until_ != 0 && busy_until_ <= now ? busy_until_ : now;
    begin_head_idle_unload(completion);
    update_head_unload(now);
    intrq_ = true;
}

void FdcMb8877::update_force_interrupt(uint64_t now) {
    if (force_interrupt_mask_ == 0) return;

    const bool ready = selected_drive_ready();
    if (ready != force_last_ready_) {
        if (ready && (force_interrupt_mask_ & 0x01)) intrq_ = true;
        if (!ready && (force_interrupt_mask_ & 0x02)) intrq_ = true;
        force_last_ready_ = ready;
    }

    if (force_interrupt_mask_ & 0x04) {
        if (now < force_index_sample_cycle_) {
            force_index_sample_cycle_ = now;
        } else if (selected_drive_ready() && index_origin_valid_ &&
                   now >= index_origin_) {
            uint64_t first_edge = index_origin_;
            if (force_index_sample_cycle_ >= index_origin_) {
                const uint64_t elapsed =
                    force_index_sample_cycle_ - index_origin_;
                first_edge = index_origin_ +
                    (elapsed / CYC_PER_REV + 1) * CYC_PER_REV;
            }
            if (first_edge <= now) intrq_ = true;
        }
        force_index_sample_cycle_ = now;
    }
}

bool FdcMb8877::interrupt_request(uint64_t now) {
    (void)status_at(now);
    update_force_interrupt(now);
    return intrq_;
}

void FdcMb8877::offer_read_byte(uint8_t value) {
    if (data_reg_full_) {
        // A newly assembled character is transferred into DR even if the
        // host has not read the previous contents. The older byte is lost.
        done_status_ |= ST_LOST;
    }
    data_reg_ = value;
    data_reg_full_ = true;
}

void FdcMb8877::advance_read_realtime(uint64_t now) {
    if (state_ != State::Read || !begin_type23(now)) return;
    while (state_ == State::Read && read_valid_) {
        if (!active_drive_ready()) {
            done_status_ &= static_cast<uint8_t>(~ST_NOT_READY);
            read_valid_ = false;
            busy_until_ = now;
            return;
        }
        const uint8_t* sector = active_sector();
        if (!sector) {
            record_search_failed_ = true;
            record_search_revolutions_ = static_cast<uint8_t>(
                id_search_revolutions_for_current_id());
            read_valid_ = false;
            busy_until_ = now + id_search_timeout();
            return;
        }
        while (read_index_ < read_transfer_size_ &&
               now >= byte_ready(read_index_)) {
            if (!read_first_drq_counted_) {
                const uint64_t latency =
                    byte_ready(read_index_) - read_command_cycle_;
                stat_read_first_drqs++;
                stat_read_max_first_drq_cycles = std::max(
                    stat_read_max_first_drq_cycles, latency);
                read_first_drq_counted_ = true;
            }
            offer_read_byte(sector[read_index_]);
            read_index_++;
        }
        if (read_index_ < read_transfer_size_) return;

        const uint64_t sector_end =
            byte_ready(read_transfer_size_) + byte_cycles() * 2;
        busy_until_ = sector_end;
        if (now < sector_end) return;
        done_status_ = static_cast<uint8_t>(
            (done_status_ & ~(ST_REC_TYPE | ST_CRC)) |
            pending_read_status_);
        if ((done_status_ & ST_CRC) || !read_multiple_ || sector_reg_ == 255)
            return;

        sector_reg_++;
        read_sector_ = sector_reg_;
        if (!active_sector()) {
            record_search_failed_ = true;
            record_search_revolutions_ = static_cast<uint8_t>(
                id_search_revolutions_for_current_id());
            pending_read_status_ = 0;
            done_status_ &= static_cast<uint8_t>(~(ST_REC_TYPE | ST_CRC));
            read_valid_ = false;
            busy_until_ = sector_end + id_search_timeout();
            return;
        }
        read_transfer_size_ = active_transfer_size();
        read_index_ = 0;
        read_start_ = sector_end;
        const D88Disk::Sector* next_record = active_record();
        pending_read_status_ = active_sector_deleted() ? ST_REC_TYPE : 0;
        if (next_record && d88_data_crc_error(*next_record))
            pending_read_status_ |= ST_CRC;
        done_status_ &= static_cast<uint8_t>(~(ST_REC_TYPE | ST_CRC));
    }
}

void FdcMb8877::advance_write_realtime(uint64_t now) {
    if (state_ != State::Write || !begin_type23(now)) return;
    if (record_search_failed_ && write_index_ >= write_transfer_size_) return;
    while (state_ == State::Write && write_index_ < write_transfer_size_) {
        if (!active_drive_ready()) {
            done_status_ &= static_cast<uint8_t>(~ST_NOT_READY);
            write_index_ = write_transfer_size_;
            busy_until_ = now;
            return;
        }
        const uint64_t deadline =
            byte_due(write_start_, write_index_ + 1);
        if (now < deadline) return;

        if (write_index_ == 0 && !write_data_full_) {
            // Without the first byte the write gate never opens.
            done_status_ |= ST_LOST;
            write_index_ = write_transfer_size_;
            busy_until_ = deadline;
            return;
        }

        const uint8_t value = write_data_full_ ? data_reg_ : 0x00;
        if (!write_data_full_) done_status_ |= ST_LOST;
        if (!write_started_) {
            D88Disk* disk = disks_[read_drive_];
            if (disk) {
                disk->set_deleted_mark_on_track(
                    read_cyl_, read_side_, command_track_,
                    static_cast<uint8_t>(read_sector_),
                    command_single_density_, command_compare_side_,
                    command_side_id_, write_deleted_);
            }
            write_started_ = true;
        }
        uint8_t* target = write_target();
        if (!target) {
            record_search_failed_ = true;
            record_search_revolutions_ = static_cast<uint8_t>(
                id_search_revolutions_for_current_id());
            write_index_ = write_transfer_size_;
            busy_until_ = now + id_search_timeout();
            return;
        }
        target[write_index_++] = value;
        write_data_full_ = false;
    }

    if (state_ != State::Write || write_index_ < write_transfer_size_) return;
    const uint64_t sector_end =
        byte_due(write_start_, write_transfer_size_) + byte_cycles() * 2;
    busy_until_ = sector_end;
    if (now < sector_end || !write_multiple_ || sector_reg_ == 255) return;

    sector_reg_++;
    read_sector_ = sector_reg_;
    if (!active_record() || !D88Disk::fdc_transfer_supported(*active_record())) {
        record_search_failed_ = true;
        record_search_revolutions_ = static_cast<uint8_t>(
            id_search_revolutions_for_current_id());
        busy_until_ = sector_end + id_search_timeout();
        return;
    }
    write_transfer_size_ = active_transfer_size();
    write_index_ = 0;
    write_start_ = sector_end;
    write_data_full_ = false;
    write_started_ = false;
}

void FdcMb8877::advance_readaddr_realtime(uint64_t now) {
    if (state_ != State::ReadAddr) return;
    if (!begin_type23(now)) return;
    if (record_search_failed_) return;
    if (!active_drive_ready()) {
        done_status_ &= static_cast<uint8_t>(~ST_NOT_READY);
        id_index_ = 6;
        busy_until_ = now;
        return;
    }
    while (id_index_ < 6 && now >= byte_ready(id_index_)) {
        offer_read_byte(id_bytes_[id_index_]);
        id_index_++;
    }
    if (id_index_ >= 6) busy_until_ = byte_ready(6) + byte_cycles();
}

void FdcMb8877::advance_writetrack_realtime(uint64_t now) {
    if (state_ != State::WriteTrack) return;
    if (!begin_type23(now)) return;
    if (!active_drive_ready()) {
        done_status_ &= static_cast<uint8_t>(~ST_NOT_READY);
        write_track_done_ = true;
        track_stream_.clear();
        busy_until_ = now;
        return;
    }
    while (!write_track_done_ && now >= write_track_next_due_) {
        if (write_track_next_due_ >= write_track_end_) {
            commit_track_stream();
            write_track_done_ = true;
            busy_until_ = write_track_end_;
            return;
        }
        if (track_index_ == 0 && !write_data_full_) {
            done_status_ |= ST_LOST;
            write_track_done_ = true;
            track_stream_.clear();
            busy_until_ = write_start_;
            return;
        }
        if (!write_data_full_) done_status_ |= ST_LOST;
        const uint8_t value = write_data_full_ ? data_reg_ : 0x00;
        track_stream_.push_back(value);
        write_data_full_ = false;
        track_index_++;
        const uint64_t encoded_bytes = value == 0xF7 ? 2 : 1;
        write_track_next_due_ += encoded_bytes * byte_cycles();
        if (write_track_next_due_ >= write_track_end_) {
            commit_track_stream();
            write_track_done_ = true;
            busy_until_ = write_track_end_;
        }
    }
}

void FdcMb8877::advance_readtrack_realtime(uint64_t now) {
    const int total = (int)read_track_stream_.size();
    if (state_ != State::ReadTrack || total <= 0) return;
    if (!begin_type23(now)) return;
    if (!active_drive_ready()) {
        done_status_ &= static_cast<uint8_t>(~ST_NOT_READY);
        read_track_index_ = total;
        busy_until_ = now;
        return;
    }
    while (read_track_index_ < total &&
           now >= byte_ready(read_track_index_)) {
        offer_read_byte(read_track_stream_[read_track_index_]);
        read_track_index_++;
    }
    if (read_track_index_ >= total)
        busy_until_ = byte_ready(total - 1);
}

uint8_t FdcMb8877::status_at(uint64_t now) {
    update_head_unload(now);
    const uint8_t wp = disk_write_protected() ? ST_WP : 0;
    const uint8_t not_ready = selected_drive_ready() ? 0 : ST_NOT_READY;
    const uint8_t index = index_pulse(now) ? ST_INDEX : 0;
    switch (state_) {
    case State::Idle:
        if (status_class_ == StatusClass::TypeI) {
            return done_status_ | wp | not_ready | index |
                   (phys_cyl_[drive_] == 0 ? ST_TRACK0 : 0) |
                   (head_engaged() ? ST_HEAD : 0);
        }
        if (status_class_ == StatusClass::Write)
            return done_status_ | wp | not_ready;
        return done_status_ | read_status_residue_ | not_ready |
               (data_reg_full_ ? ST_DRQ : 0);
    case State::TypeI:
        advance_type1(now);
        if ((type1_verify_ && !type1_verify_started_) || now < busy_until_)
            return ST_BUSY | wp | not_ready | index |
                   (phys_cyl_[drive_] == 0 ? ST_TRACK0 : 0) |
                   (head_engaged() ? ST_HEAD : 0);
        complete_command(now);
        return done_status_ | wp | not_ready | index |
               (phys_cyl_[drive_] == 0 ? ST_TRACK0 : 0) |
               (head_engaged() ? ST_HEAD : 0);
    case State::Read:
        advance_read_realtime(now);
        if (!type23_started_)
            return ST_BUSY | done_status_ | read_status_residue_ | not_ready;
        if (!read_valid_) {
            // record-not-found spin
            if (now < busy_until_)
                return ST_BUSY | done_status_ | read_status_residue_ | not_ready |
                       (data_reg_full_ ? ST_DRQ : 0);
            if (record_search_failed_) done_status_ |= ST_RNF;
            complete_command(now);
            return done_status_ | read_status_residue_ | not_ready |
                   (data_reg_full_ ? ST_DRQ : 0);
        }
        if (read_index_ >= read_transfer_size_) {
            // record-not-found spin (walked off the track), or CRC tail
            // after the last byte
            if (now < busy_until_)
                return ST_BUSY | done_status_ | read_status_residue_ | not_ready |
                       (data_reg_full_ ? ST_DRQ : 0);
            complete_command(now);
            return done_status_ | read_status_residue_ | not_ready |
                   (data_reg_full_ ? ST_DRQ : 0);
        }
        return ST_BUSY | done_status_ | read_status_residue_ | not_ready |
               (data_reg_full_ ? ST_DRQ : 0);
    case State::Write:
        advance_write_realtime(now);
        if (!type23_started_) return ST_BUSY | done_status_ | wp | not_ready;
        if (write_index_ >= write_transfer_size_) {
            // record-not-found spin, or the CRC tail after the last byte
            if (now < busy_until_)
                return ST_BUSY | done_status_ | wp | not_ready;
            if (record_search_failed_) done_status_ |= ST_RNF;
            complete_command(now);
            return done_status_ | wp | not_ready;
        }
        return ST_BUSY | done_status_ | wp | not_ready |
               (type23_started_ && now >= write_start_ && !write_data_full_
                    ? ST_DRQ : 0);
    case State::ReadAddr:
        // READ ADDRESS is a read-type command: bit6 carries WRITE PROTECT
        // only for write commands (see State::Write above) and reads as 0
        // here, same as State::Read.
        advance_readaddr_realtime(now);
        if (!type23_started_) return ST_BUSY | done_status_ | not_ready;
        if (id_index_ >= 6) {
            if (now < busy_until_)
                return ST_BUSY | done_status_ | not_ready |
                       (data_reg_full_ ? ST_DRQ : 0);
            if (record_search_failed_) done_status_ |= ST_RNF;
            complete_command(now);
            return done_status_ | not_ready |
                   (data_reg_full_ ? ST_DRQ : 0);
        }
        return ST_BUSY | done_status_ | not_ready |
               (data_reg_full_ ? ST_DRQ : 0);
    case State::WriteTrack:
        // WRITE TRACK is a write-type command: bit6 carries WRITE PROTECT
        // during its busy phase, same as State::Write.
        advance_writetrack_realtime(now);
        if (write_track_done_) {
            if (now < busy_until_)
                return ST_BUSY | done_status_ | wp | not_ready;
            complete_command(now);
            return done_status_ | wp | not_ready;
        }
        return ST_BUSY | done_status_ | wp | not_ready |
               (!write_data_full_ ? ST_DRQ : 0);
    case State::ReadTrack:
        // READ TRACK is a read-type command: bit6 reads as 0 here, same as
        // State::Read/State::ReadAddr above -- only write commands report
        // WRITE PROTECT.
        advance_readtrack_realtime(now);
        if (!type23_started_) return ST_BUSY | done_status_ | not_ready;
        if (read_track_index_ >= (int)read_track_stream_.size()) {
            if (now < busy_until_)
                return ST_BUSY | done_status_ | not_ready |
                       (data_reg_full_ ? ST_DRQ : 0);
            complete_command(now);
            return done_status_ | not_ready |
                   (data_reg_full_ ? ST_DRQ : 0);
        }
        return ST_BUSY | done_status_ | not_ready |
               (data_reg_full_ ? ST_DRQ : 0);
    }
    return 0;
}

uint8_t FdcMb8877::read(int reg, uint64_t now) {
    switch (reg) {
    case 0: {
        const uint8_t status = status_at(now);
        update_force_interrupt(now);
        intrq_ = false;
        return status;
    }
    case 1:
        return track_reg_;
    case 2:
        return sector_reg_;
    case 3:
        if (state_ == State::Read) advance_read_realtime(now);
        if (state_ == State::ReadAddr) advance_readaddr_realtime(now);
        if (state_ == State::ReadTrack) advance_readtrack_realtime(now);
        if (data_reg_full_) {
            if (state_ == State::Read &&
                read_index_ >= read_transfer_size_) {
                previous_read_data_valid_ = true;
                previous_read_data_cycle_ = now;
                previous_read_drive_ = read_drive_;
                previous_read_cyl_ = read_cyl_;
                previous_read_side_ = read_side_;
                previous_read_sector_ = read_sector_;
            }
            data_reg_full_ = false;
            done_status_ &= static_cast<uint8_t>(~ST_DRQ);
            return data_reg_;
        }
        return data_reg_;
    }
    return 0xFF;
}

void FdcMb8877::write(int reg, uint8_t value, uint64_t now) {
    update_head_unload(now);
    switch (reg) {
    case 0: { // command
        const uint8_t cmd = value;
        const bool force_interrupt = (cmd & 0xF0) == 0xD0;
        // Busy may have expired since the caller's last status read. Advance
        // once before applying the rule that only FORCE INTERRUPT is legal
        // while a command is still active.
        uint8_t prior_status = 0;
        if (state_ != State::Idle) prior_status = status_at(now);
        const bool was_busy = state_ != State::Idle;
        if (state_ != State::Idle && !force_interrupt) return;
        if ((cmd & 0xE0) != 0x80) previous_read_data_valid_ = false;
        if (!force_interrupt) {
            intrq_ = false;
            force_interrupt_mask_ = 0;
            busy_until_ = 0;
            record_search_failed_ = false;
            record_search_revolutions_ = 4;
            pending_read_status_ = 0;
            read_status_residue_ = 0;
        }
        if (force_interrupt && state_ == State::WriteTrack) {
            // Standard D88 cannot represent a partially overwritten magnetic
            // revolution. Treat WRITE TRACK as a transaction and commit only
            // at the closing index; Force Interrupt discards the whole prefix.
            track_stream_.clear();
            write_track_done_ = true;
        }
        const auto begin_type1 = [&](int steps, int direction,
                                     bool update_track, bool restore) {
            type1_verify_ = (cmd & 0x04) != 0;
            type1_verify_started_ = false;
            type1_verify_result_ = 0;
            command_single_density_ = single_density_;
            // h=1 asserts HLD immediately. h=0,V=0 releases it, while
            // h=0,V=1 retains an already asserted HLD and may assert it near
            // the end of movement if it was low.
            if (cmd & 0x08) head_loaded_ = true;
            else if (!type1_verify_) head_loaded_ = false;
            cancel_head_idle_unload();
            done_status_ = 0;
            status_class_ = StatusClass::TypeI;
            state_ = State::TypeI;
            type1_motion_start_ = now;
            type1_step_interval_ = type1_step_cycles(cmd);
            type1_steps_total_ = steps;
            type1_steps_applied_ = 0;
            type1_drive_ = drive_;
            type1_motion_direction_ = direction;
            type1_update_track_ = update_track;
            type1_restore_ = restore;
            type1_motion_end_ =
                now + static_cast<uint64_t>(steps) * type1_step_interval_;
            if (require_explicit_head_settle_ && steps > 0) {
                head_settle_pending_ = true;
                head_settle_ready_cycle_ =
                    type1_motion_end_ + CYC_TYPE1_SETTLE;
            }
            busy_until_ = type1_motion_end_;
            // The documented Type-I flow issues the first STEP pulse before
            // waiting for r1:r0's inter-step delay.
            advance_type1(now);
        };
        if ((cmd & 0xF0) == 0x00) { // RESTORE
            const int steps = phys_cyl_[drive_];
            stat_seeks++;
            step_dir_ = -1;
            data_reg_ = 0;
            track_reg_ = steps == 0 ? 0 : 0xFF;
            begin_type1(steps, -1, false, true);
        } else if ((cmd & 0xF0) == 0x10) { // SEEK (target in data register)
            const int target = data_reg_;
            const int delta = target - static_cast<int>(track_reg_);
            const int steps = std::abs(delta);
            stat_seeks++;
            if (steps != 0) step_dir_ = delta > 0 ? 1 : -1;
            begin_type1(steps, steps == 0 ? 0 : step_dir_, true, false);
        } else if ((cmd & 0xE0) == 0x20 || (cmd & 0xE0) == 0x40 ||
                   (cmd & 0xE0) == 0x60) {
            // STEP (20h) repeats the last direction; STEP-IN (40h) moves
            // toward the spindle, STEP-OUT (60h) away from it. Bit4 (U)
            // says whether the track register follows the head.
            if ((cmd & 0xE0) == 0x40) step_dir_ = 1;
            else if ((cmd & 0xE0) == 0x60) step_dir_ = -1;
            begin_type1(1, step_dir_, (cmd & 0x10) != 0, false);
        } else if ((cmd & 0xE0) == 0x80) { // READ SECTOR
            head_settle_violation_ = false;
            if (require_explicit_head_settle_ && head_settle_pending_) {
                head_settle_violation_ =
                    now < head_settle_ready_cycle_ && (cmd & 0x04) == 0;
                head_settle_pending_ = false;
            }
            // Real MZ-2500 observation: when E=1 follows an idle Type-I
            // command, MB8876 exposes the old raw HLD/TR00 bit positions
            // throughout the read (25h while busy, 24h on completion at
            // track zero). Flux proved a normal DAM and a byte-perfect
            // transfer, so these are not Record Type/Lost Data. FD179X
            // documentation promises command-time clearing, but Fujitsu's
            // datasheet only says the bits clear "when updated"; preserve
            // the measured MB8876 behavior without contaminating true error
            // latches in done_status_.
            //
            // The measurement was always taken with the READ issued right
            // after the Type I command's BUSY dropped. A third-party
            // MZ-80B/2000/2500 boot loader in real-hardware use pauses
            // 60 ms after every SEEK, then requires a status of exactly 00h
            // once its E=1 multiple-record read is stopped by FORCE
            // INTERRUPT. So the residue is bounded by the chip's own 30 ms
            // settle interval after the Type I motion: a Type II command
            // written later than that reports a clean Type II status.
            if ((cmd & 0x04) && status_class_ == StatusClass::TypeI &&
                now < type1_motion_end_ + CYC_TYPE1_SETTLE) {
                if (head_engaged()) read_status_residue_ |= ST_REC_TYPE;
                if (phys_cyl_[drive_] == 0) read_status_residue_ |= ST_LOST;
            }
            head_loaded_ = true;
            cancel_head_idle_unload();
            command_single_density_ = single_density_;
            command_compare_side_ = (cmd & 0x02) != 0;
            command_side_id_ = static_cast<uint8_t>((cmd >> 3) & 1);
            command_track_ = track_reg_;
            read_multiple_ = (cmd & 0x10) != 0;
            stat_reads++;
            read_fault_active_ = false;
            const bool inject_read_fault =
                read_fault_command_ != 0 &&
                (read_fault_persistent_ ? stat_reads >= read_fault_command_
                                        : (!read_fault_used_ &&
                                           stat_reads == read_fault_command_));
            if (inject_read_fault) {
                read_fault_active_ = true;
                read_fault_used_ = true;
                read_fault_count_++;
            }
            status_class_ = StatusClass::Read;
            type23_started_ = false;
            type23_ready_cycle_ = now + type23_delay(cmd);
            read_drive_ = drive_;
            read_cyl_ = phys_cyl_[drive_];
            read_side_ = side_;
            read_sector_ = sector_reg_;
            if (previous_read_data_valid_ &&
                previous_read_drive_ == read_drive_ &&
                previous_read_cyl_ == read_cyl_ &&
                previous_read_side_ == read_side_ &&
                previous_read_sector_ < 255 &&
                read_sector_ == previous_read_sector_ + 1 &&
                now >= previous_read_data_cycle_) {
                const uint64_t gap = now - previous_read_data_cycle_;
                stat_read_sequential_gaps++;
                stat_read_max_sequential_gap_cycles = std::max(
                    stat_read_max_sequential_gap_cycles, gap);
            }
            // The marker describes only the immediately following READ
            // command. A failed R+1 attempt must not make its later retry look
            // like one very long normal-path inter-sector gap.
            previous_read_data_valid_ = false;
            read_command_cycle_ = now;
            read_first_drq_counted_ = false;
            read_completion_counted_ = false;
            read_index_ = 0;
            data_reg_full_ = false;
            read_transfer_size_ = active_transfer_size();
            if (!selected_drive_ready()) {
                complete_command(now);
                read_valid_ = false;
                done_status_ = 0;
                break;
            }
            state_ = State::Read;
            const D88Disk::Sector* record = active_record();
            const bool found = record && D88Disk::fdc_transfer_supported(*record);
            read_valid_ = found && !head_settle_violation_;
            if (head_settle_violation_) {
                // Reproduce the real-machine observation at the loader
                // boundary: BUSY is accepted, no DRQ arrives, and the
                // command ends without fabricating an RNF/CRC media error.
                read_index_ = read_transfer_size_;
                record_search_failed_ = false;
                done_status_ = 0;
                if (head_engaged()) read_status_residue_ |= ST_REC_TYPE;
            } else if (!found) {
                read_index_ = read_transfer_size_;
                record_search_failed_ = true;
                record_search_revolutions_ = static_cast<uint8_t>(
                    id_search_revolutions_for_current_id());
                done_status_ = 0;
            } else {
                // ST_REC_TYPE (deleted-data mark) is the read-side
                // counterpart of WRITE SECTOR's a0 flag / set_deleted_mark():
                // a sector written with the mark must read back reporting
                // it, or firmware that writes a bad-sector mark and reads it
                // back to confirm never will.
                pending_read_status_ =
                    active_sector_deleted() ? ST_REC_TYPE : 0;
                if (d88_data_crc_error(*record))
                    pending_read_status_ |= ST_CRC;
                done_status_ = 0;
            }
        } else if ((cmd & 0xE0) == 0xA0) { // WRITE SECTOR
            head_loaded_ = true;
            cancel_head_idle_unload();
            command_single_density_ = single_density_;
            command_compare_side_ = (cmd & 0x02) != 0;
            command_side_id_ = static_cast<uint8_t>((cmd >> 3) & 1);
            command_track_ = track_reg_;
            status_class_ = StatusClass::Write;
            type23_started_ = false;
            type23_ready_cycle_ = now + type23_delay(cmd);
            write_multiple_ = (cmd & 0x10) != 0;
            read_drive_ = drive_;
            read_cyl_ = phys_cyl_[drive_];
            read_side_ = side_;
            read_sector_ = sector_reg_;
            write_index_ = 0;
            write_data_full_ = false;
            write_started_ = false;
            write_deleted_ = (cmd & 0x01) != 0;
            write_transfer_size_ = active_transfer_size();
            if (!selected_drive_ready()) {
                complete_command(now);
                done_status_ = 0;
                break;
            }
            if (disk_write_protected()) {
                // the chip never starts the write; the status byte says why
                complete_command(now);
                done_status_ = 0;
                break;
            }
            const D88Disk::Sector* record = active_record();
            const bool found = record && D88Disk::fdc_transfer_supported(*record);
            state_ = State::Write;
            if (!found) {
                write_index_ = write_transfer_size_;
                record_search_failed_ = true;
                record_search_revolutions_ = static_cast<uint8_t>(
                    id_search_revolutions_for_current_id());
                done_status_ = 0;
            } else {
                done_status_ = 0;
            }
        } else if ((cmd & 0xF0) == 0xC0) { // READ ADDRESS
            head_loaded_ = true;
            cancel_head_idle_unload();
            command_single_density_ = single_density_;
            command_compare_side_ = false;
            command_side_id_ = 0;
            command_track_ = track_reg_;
            status_class_ = StatusClass::Read;
            type23_started_ = false;
            type23_ready_cycle_ = now + type23_delay(cmd);
            read_drive_ = drive_;
            read_cyl_ = phys_cyl_[drive_];
            read_side_ = side_;
            id_index_ = 0;
            data_reg_full_ = false;
            if (!selected_drive_ready()) {
                complete_command(now);
                done_status_ = 0;
                break;
            }
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
                    ((candidate->density & 0x40) != 0) == command_single_density_ &&
                    D88Disk::fdc_transfer_supported(*candidate)) {
                    sec = candidate;
                    break;
                }
            }
            state_ = State::ReadAddr;
            if (!sec) {
                id_index_ = 6;
                record_search_failed_ = true;
                done_status_ = 0;
            } else {
                id_bytes_[0] = sec->c;
                id_bytes_[1] = sec->h;
                id_bytes_[2] = sec->r;
                id_bytes_[3] = sec->n;
                const uint16_t crc = id_crc(command_single_density_, *sec);
                id_bytes_[4] = static_cast<uint8_t>(crc >> 8);
                id_bytes_[5] = static_cast<uint8_t>(crc);
                sector_reg_ = sec->c; // the chip copies the track address here
                done_status_ = 0;
            }
        } else if ((cmd & 0xF0) == 0xF0) { // WRITE TRACK (physical format)
            head_loaded_ = true;
            cancel_head_idle_unload();
            command_single_density_ = single_density_;
            command_compare_side_ = false;
            command_side_id_ = 0;
            command_track_ = track_reg_;
            status_class_ = StatusClass::Write;
            type23_started_ = false;
            type23_ready_cycle_ = now + type23_delay(cmd);
            read_drive_ = drive_;
            read_cyl_ = phys_cyl_[drive_];
            read_side_ = side_;
            track_stream_.clear();
            track_index_ = 0;
            write_track_done_ = false;
            write_track_next_due_ = 0;
            write_track_end_ = 0;
            write_data_full_ = false;
            if (!selected_drive_ready()) {
                complete_command(now);
                done_status_ = 0;
                break;
            }
            if (disk_write_protected()) {
                complete_command(now);
                done_status_ = 0;
                break;
            }
            state_ = State::WriteTrack;
            done_status_ = 0;
        } else if ((cmd & 0xF0) == 0xE0) { // READ TRACK
            head_loaded_ = true;
            cancel_head_idle_unload();
            command_single_density_ = single_density_;
            command_compare_side_ = false;
            command_side_id_ = 0;
            command_track_ = track_reg_;
            status_class_ = StatusClass::Read;
            type23_started_ = false;
            type23_ready_cycle_ = now + type23_delay(cmd);
            read_drive_ = drive_;
            read_cyl_ = phys_cyl_[drive_];
            read_side_ = side_;
            read_track_stream_.clear();
            read_track_index_ = 0;
            data_reg_full_ = false;
            if (!selected_drive_ready()) {
                complete_command(now);
                done_status_ = 0;
                break;
            }
            const D88Disk* d = disks_[read_drive_];
            const uint8_t gap = command_single_density_ ? 0xFF : 0x4E;
            const int count = d ? d->sector_count(read_cyl_, read_side_) : 0;
            bool unsupported_record = false;
            for (int i = 0; i < count; i++) {
                const D88Disk::Sector* sec =
                    d->sector_at(read_cyl_, read_side_, i);
                if (!sec) break;
                if (((sec->density & 0x40) != 0) != command_single_density_)
                    continue;
                if (!D88Disk::fdc_transfer_supported(*sec)) {
                    unsupported_record = true;
                    break;
                }
            }
            if (unsupported_record) {
                // N>=4 and short records remain parseable/serializable, but
                // no FDC command is allowed to expose them as transferable.
                done_status_ = ST_RNF;
                complete_command(now);
                break;
            }
            for (int i = 0; i < 40; i++) read_track_stream_.push_back(gap);
            for (int i = 0; i < count; i++) {
                const D88Disk::Sector* sec = d->sector_at(read_cyl_, read_side_, i);
                if (!sec) break;
                if (((sec->density & 0x40) != 0) != command_single_density_) continue;
                const size_t transfer_size = D88Disk::fdc_transfer_size(*sec);
                const int sync_zeros = command_single_density_ ? 6 : 12;
                for (int g = 0; g < sync_zeros; g++)
                    read_track_stream_.push_back(0x00);
                if (!command_single_density_) {
                    read_track_stream_.push_back(0xA1);
                    read_track_stream_.push_back(0xA1);
                    read_track_stream_.push_back(0xA1);
                }
                read_track_stream_.push_back(0xFE);
                read_track_stream_.push_back(sec->c);
                read_track_stream_.push_back(sec->h);
                read_track_stream_.push_back(sec->r);
                read_track_stream_.push_back(sec->n);
                const uint16_t record_id_crc =
                    id_crc(command_single_density_, *sec);
                read_track_stream_.push_back(
                    static_cast<uint8_t>(record_id_crc >> 8));
                read_track_stream_.push_back(
                    static_cast<uint8_t>(record_id_crc));
                const int gap_two = command_single_density_ ? 11 : 22;
                for (int g = 0; g < gap_two; g++)
                    read_track_stream_.push_back(gap);
                for (int g = 0; g < sync_zeros; g++)
                    read_track_stream_.push_back(0x00);
                if (!command_single_density_) {
                    read_track_stream_.push_back(0xA1);
                    read_track_stream_.push_back(0xA1);
                    read_track_stream_.push_back(0xA1);
                }
                const uint8_t data_mark = sec->deleted ? 0xF8 : 0xFB;
                read_track_stream_.push_back(data_mark);
                for (size_t b = 0; b < transfer_size; b++)
                    read_track_stream_.push_back(sec->data[b]);
                uint16_t record_data_crc = field_crc(
                    command_single_density_, data_mark,
                    sec->data.data(), transfer_size);
                if (d88_data_crc_error(*sec)) record_data_crc ^= 0x0001;
                read_track_stream_.push_back(
                    static_cast<uint8_t>(record_data_crc >> 8));
                read_track_stream_.push_back(
                    static_cast<uint8_t>(record_data_crc));
                const int gap_three = command_single_density_ ? 10 : 24;
                for (int g = 0; g < gap_three; g++)
                    read_track_stream_.push_back(gap);
            }
            state_ = State::ReadTrack;
            // READ TRACK runs from one index pulse to the next. A blank track
            // is still a readable revolution of gap bytes; RNF is not part of
            // the command's documented status behavior. D88 stores records,
            // not the raw bit-cell stream, so pad a short canonical synthesis
            // or cut a long one at the density-specific index boundary.
            read_track_stream_.resize((size_t)track_stream_bytes(), gap);
            done_status_ = 0;
        } else if ((cmd & 0xF0) == 0xD0) { // FORCE INTERRUPT
            if (was_busy) {
                // During BUSY only BUSY itself is cleared; retain the
                // interrupted command's status format and latched bits.
                done_status_ = prior_status &
                    static_cast<uint8_t>(~(ST_BUSY | ST_NOT_READY | ST_WP));
            } else {
                // An idle FORCE INTERRUPT updates the register in Type I
                // format. I0-I3 affect INTRQ conditions, whose MZ wiring is
                // outside this core's currently observable interface.
                status_class_ = StatusClass::TypeI;
                done_status_ = 0;
            }
            state_ = State::Idle;
            begin_head_idle_unload(now);
            write_data_full_ = false;
            record_search_failed_ = false;
            pending_read_status_ = 0;
            intrq_ = false;
            force_interrupt_mask_ = cmd & 0x07;
            force_last_ready_ = selected_drive_ready();
            force_index_sample_cycle_ = now;
            if (cmd & 0x08) intrq_ = true;
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
        if (state_ != State::Idle) (void)status_at(now);
        if (state_ == State::Idle) track_reg_ = value;
        break;
    case 2:
        if (state_ != State::Idle) (void)status_at(now);
        if (state_ == State::Idle) sector_reg_ = value;
        break;
    case 3:
        if (state_ == State::Write) advance_write_realtime(now);
        if (state_ == State::Write && type23_started_ &&
            write_index_ < write_transfer_size_ && now >= write_start_ &&
            !write_data_full_) {
            data_reg_ = value;
            write_data_full_ = true;
            break;
        }
        if (state_ == State::WriteTrack) advance_writetrack_realtime(now);
        if (state_ == State::WriteTrack && !write_track_done_ &&
            !write_data_full_) {
            data_reg_ = value;
            write_data_full_ = true;
            break;
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
    bool invalid = false;
    int unsupported_n_count = 0;
    uint8_t first_bad_n = 0, first_bad_c = 0, first_bad_h = 0, first_bad_r = 0;
    const char* first_bad_reason = nullptr;
    while (i < n) {
        if (track_stream_[i] != 0xFE) { i++; continue; }
        if (i + 5 > n) {
            invalid = true;
            first_bad_reason = first_bad_reason ? first_bad_reason : "incomplete ID";
            break;
        }
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
        if (i >= n || track_stream_[i] == 0xFE) {
            invalid = true;
            first_bad_reason = first_bad_reason ? first_bad_reason : "ID without data";
            break;
        }
        sec.deleted = track_stream_[i] == 0xF8 ? 0x10 : 0x00;
        i++;
        size_t size = 0;
        if (!D88Disk::nominal_sector_size(sec.n, size) || sec.n > 3) {
            invalid = true;
            unsupported_n_count++;
            if (!first_bad_reason) {
                first_bad_reason = "unsupported N";
                first_bad_n = sec.n;
                first_bad_c = sec.c;
                first_bad_h = sec.h;
                first_bad_r = sec.r;
            }
            break;
        }
        if (n - i < size) {
            invalid = true;
            first_bad_reason = first_bad_reason ? first_bad_reason : "incomplete data";
            break;
        }
        sec.data.assign(track_stream_.begin() + i,
                        track_stream_.begin() + i + size);
        i += size;
        sectors.push_back(std::move(sec));
    }

    if (invalid || sectors.empty()) {
        if (invalid) {
            if (unsupported_n_count > 0) {
                std::fprintf(stderr,
                             "[fdc] format rejected: %s N=%u at C=%u H=%u R=%u\n",
                             first_bad_reason ? first_bad_reason : "invalid format",
                             first_bad_n, first_bad_c, first_bad_h, first_bad_r);
            } else {
                std::fprintf(stderr, "[fdc] format rejected: %s\n",
                             first_bad_reason ? first_bad_reason : "invalid format");
            }
        }
        track_stream_.clear();
        return;
    }

    D88Disk* d = disks_[read_drive_];
    if (d) d->format_track(read_cyl_, read_side_, sectors);
    track_stream_.clear();
}

void FdcMb8877::write_drive(uint8_t value, uint64_t now) {
    update_head_unload(now);
    update_force_interrupt(now);
    const bool next_motor = (value & 0x80) != 0;
    if (next_motor && !motor_) {
        index_origin_ = now;
        index_origin_valid_ = true;
    } else if (!next_motor) {
        index_origin_valid_ = false;
    }
    motor_ = next_motor;
    drive_select_enabled_ = (value & 0x04) != 0;
    raw_drive_number_ = value & 0x03;
    const int first_internal = internal_drive_bank_high_ ? 2 : 0;
    const int mapped = raw_drive_number_ - first_internal;
    drive_mapped_ = mapped >= 0 && mapped < NUM_DRIVES;
    if (drive_mapped_) drive_ = mapped;
    update_force_interrupt(now);
}

void FdcMb8877::set_internal_drive_bank(bool high, uint64_t now) {
    update_head_unload(now);
    update_force_interrupt(now);
    internal_drive_bank_high_ = high;
    const int first_internal = internal_drive_bank_high_ ? 2 : 0;
    const int mapped = raw_drive_number_ - first_internal;
    drive_mapped_ = mapped >= 0 && mapped < NUM_DRIVES;
    if (drive_mapped_) drive_ = mapped;
    update_force_interrupt(now);
}

void FdcMb8877::write_side(uint8_t value, uint64_t now) {
    const int next = value & 0x01;
    if (require_explicit_head_settle_ && next != side_) {
        head_settle_pending_ = true;
        head_settle_ready_cycle_ = now + CYC_TYPE1_SETTLE;
    }
    side_ = next;
}

} // namespace mz
