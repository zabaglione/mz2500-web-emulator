#include "core/fdc_mb8877.h"

#include <cstdio>
#include <cstdlib>

namespace mz {

void FdcMb8877::reset() {
    state_ = State::Idle;
    track_reg_ = sector_reg_ = data_reg_ = 0;
    done_status_ = 0;
    phys_cyl_ = 0;
    side_ = 0;
    drive_ = 0;
    motor_ = false;
    busy_until_ = 0;
    read_ptr_ = nullptr;
    read_index_ = 0;
    read_start_ = 0;
}

uint8_t FdcMb8877::status_at(uint64_t now) {
    switch (state_) {
    case State::Idle:
        return done_status_ | (phys_cyl_ == 0 ? ST_TRACK0 : 0);
    case State::TypeI:
        if (now < busy_until_) return ST_BUSY;
        state_ = State::Idle;
        done_status_ = 0;
        return phys_cyl_ == 0 ? ST_TRACK0 : 0;
    case State::Read:
        if (read_ptr_ == nullptr || read_index_ >= 256) {
            // record-not-found spin, or CRC tail after the last byte
            if (now < busy_until_) return ST_BUSY;
            state_ = State::Idle;
            return done_status_;
        }
        return ST_BUSY | (now >= byte_ready(read_index_) ? ST_DRQ : 0);
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
        if (state_ == State::Read && read_ptr_ != nullptr && read_index_ < 256 &&
            now >= byte_ready(read_index_)) {
            const uint8_t value = read_ptr_[read_index_];
            read_index_++;
            if (read_index_ >= 256) {
                // busy stays up while the CRC bytes pass under the head
                busy_until_ = byte_ready(256) + CYC_PER_BYTE * 2;
                done_status_ = 0;
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
        if ((cmd & 0xF0) == 0x00) { // RESTORE
            const int steps = phys_cyl_ > 0 ? phys_cyl_ : 1;
            stat_seeks++;
            stat_steps += steps;
            phys_cyl_ = 0;
            track_reg_ = 0;
            done_status_ = 0;
            state_ = State::TypeI;
            busy_until_ = now + (uint64_t)steps * step_cycles_;
        } else if ((cmd & 0xF0) == 0x10) { // SEEK (target in data register)
            const int target = data_reg_;
            const int steps = std::abs(target - phys_cyl_) + 1;
            stat_seeks++;
            stat_steps += steps;
            phys_cyl_ = target;
            track_reg_ = (uint8_t)target;
            done_status_ = 0;
            state_ = State::TypeI;
            busy_until_ = now + (uint64_t)steps * step_cycles_;
        } else if ((cmd & 0xE0) == 0x80) { // READ SECTOR (single)
            stat_reads++;
            const uint8_t* raw =
                disk_ ? disk_->raw_sector(phys_cyl_, side_, sector_reg_) : nullptr;
            state_ = State::Read;
            read_index_ = 0;
            if (raw == nullptr || track_reg_ != phys_cyl_) {
                read_ptr_ = nullptr;
                read_index_ = 256;
                done_status_ = ST_RNF;
                busy_until_ = now + CYC_PER_REV; // spins one revolution, then RNF
            } else {
                read_ptr_ = raw;
                done_status_ = 0;
                read_start_ = now + read_latency_cycles_ + CYC_PER_BYTE;
            }
        } else if ((cmd & 0xF0) == 0xD0) { // FORCE INTERRUPT
            state_ = State::Idle;
            done_status_ = 0;
        } else {
            std::fprintf(stderr, "[fdc] unsupported command %02X\n", cmd);
            state_ = State::Idle;
            done_status_ = 0;
        }
        break;
    }
    case 1:
        track_reg_ = value;
        break;
    case 2:
        sector_reg_ = value;
        break;
    case 3:
        data_reg_ = value;
        break;
    }
}

void FdcMb8877::write_drive(uint8_t value) {
    motor_ = (value & 0x80) != 0 && (value & 0x04) != 0;
    drive_ = value & 0x03;
}

void FdcMb8877::write_side(uint8_t value) { side_ = value & 0x01; }

} // namespace mz
