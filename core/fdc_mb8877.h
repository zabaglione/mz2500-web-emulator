// MB8877 floppy disk controller — the subset the NEKO CAN RUN boot chain
// uses: RESTORE, SEEK, single READ SECTOR, polled BUSY/DRQ status.
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
// Timing is a coarse model in CPU cycles (6 MHz): 32 us per data byte
// (250 kbps MFM), plus FIXED per-READ and per-seek-step latencies
// calibrated black-box against EmuZ-2500 so the game's load-driven state
// transitions land on the same frame numbers. Late data-register reads
// never lose data, which is lenient vs. real hardware but safe for
// polled loaders.
#pragma once

#include <cstdint>

#include "core/d88.h"

namespace mz {

class FdcMb8877 {
public:
    static constexpr int NUM_DRIVES = 2;

    static constexpr uint64_t CYC_PER_BYTE = 192;       // 32 us
    static constexpr uint64_t CYC_PER_REV = 1'200'000;  // 200 ms (300 rpm)
    static constexpr uint64_t CYC_PER_STEP = 18'000;    // 3 ms

    // status bits (logical)
    static constexpr uint8_t ST_BUSY = 0x01;
    static constexpr uint8_t ST_DRQ = 0x02;
    static constexpr uint8_t ST_TRACK0 = 0x04;
    static constexpr uint8_t ST_RNF = 0x10;

    void attach(int drive, const D88Disk* disk) { disks_[drive & 1] = disk; }
    void reset();

    // reg: 0=status/command, 1=track, 2=sector, 3=data
    uint8_t read(int reg, uint64_t now);
    void write(int reg, uint8_t value, uint64_t now);

    void write_drive(uint8_t value); // port DCh (bit7 motor, low bits drive)
    void write_side(uint8_t value);  // port DDh

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
    enum class State { Idle, TypeI, Read };

    uint8_t status_at(uint64_t now);
    uint64_t byte_ready(int index) const { return read_start_ + (uint64_t)index * CYC_PER_BYTE; }
    const uint8_t* active_sector() const {
        const D88Disk* d = disks_[read_drive_];
        return d ? d->raw_sector(read_cyl_, read_side_, read_sector_) : nullptr;
    }

    const D88Disk* disks_[NUM_DRIVES] = {nullptr, nullptr};
    State state_ = State::Idle;

    uint8_t track_reg_ = 0;
    uint8_t sector_reg_ = 0;
    uint8_t data_reg_ = 0;
    uint8_t done_status_ = 0; // error bits latched for when the op completes

    int phys_cyl_[NUM_DRIVES] = {0, 0};
    int side_ = 0;
    int drive_ = 0;
    bool motor_ = false;

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

    // defaults calibrated black-box against EmuZ-2500 (P2/P4): solved from
    // three milestones (audio_boot, title with and without boot preload) so
    // both load-heavy and seek-heavy access patterns land on EmuZ's frames
    uint64_t read_latency_cycles_ = 16'480 * 6;
    uint64_t step_cycles_ = 27'830 * 6;
};

} // namespace mz
