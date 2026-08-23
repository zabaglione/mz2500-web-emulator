// MB8877 floppy disk controller model for Type I-IV commands used by the
// MZ-2500 core.
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
// Timing is represented in 6 MHz CPU cycles. Disk bytes continue to arrive
// at 32 us (MFM) or 64 us (FM) intervals whether or not the CPU services
// DRQ. The one-byte data register therefore loses a newly arriving read
// byte when still full; write requests substitute zero when missed, except
// that an unserviced first WRITE SECTOR request terminates the command.
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
    static constexpr uint64_t CYC_INDEX_PULSE = 6'000;  // 1 ms index hole
    static constexpr uint64_t CYC_TYPE1_SETTLE = 180'000; // 30 ms at 1 MHz CLK

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
    // INTRQ is modelled at the controller boundary. MZ CPU wiring remains a
    // separate board-level concern; callers may sample the pin explicitly.
    bool interrupt_request(uint64_t now);

    void write_drive(uint8_t value, uint64_t now = 0); // DCh: D7 motor, D2 enable, D1:D0 number
    void write_side(uint8_t value, uint64_t now = 0);  // port DDh
    void set_internal_drive_bank(bool high, uint64_t now = 0);
    void set_single_density(bool single) { single_density_ = single; }
    bool single_density() const { return single_density_; }
    // HLT is an external input. The MZ integration currently holds it true;
    // tests and future board wiring can drive the input explicitly.
    void set_head_load_timing(bool high, uint64_t now = 0) {
        head_load_timing_ = high;
        if (high) head_load_timing_since_ = now;
    }
    bool head_load_timing() const { return head_load_timing_; }

    bool motor_on() const { return motor_; }
    bool drive_selected() const {
        return drive_select_enabled_ && drive_mapped_;
    }
    int selected_drive() const { return drive_; }
    int raw_drive_number() const { return raw_drive_number_; }
    bool drive_mapped() const { return drive_mapped_; }
    // access-lamp bitmask, bit n = drive n LED (motor + drive select)
    uint8_t lamp_mask() const {
        return motor_ && drive_select_enabled_ && drive_mapped_
            ? static_cast<uint8_t>(1 << drive_) : 0;
    }
    int physical_cylinder() const { return phys_cyl_[drive_]; }

    // pre-data latency per READ SECTOR command (ID search, gaps); calibration knob
    void set_read_latency_us(uint32_t us) { read_latency_cycles_ = (uint64_t)us * 6; }
    uint32_t read_latency_us() const { return (uint32_t)(read_latency_cycles_ / 6); }
    // Calibration override used by the CLI. Zero restores the documented
    // 1 MHz rates (6/12/20/30 ms selected by command bits r1:r0).
    void set_step_time_us(uint32_t us) {
        step_override_cycles_ = (uint64_t)us * 6;
    }

    // Diagnostic compatibility gate for real-media loader testing. Real
    // MZ-2500 observations showed that the first READ SECTOR immediately
    // after radial head motion or a side change must request the Type-II E
    // delay. The ordinary emulator remains permissive; the CLI can enable
    // this gate to make missing settle requests deterministic before a disk
    // is taken back to real hardware.
    void set_require_explicit_head_settle(bool required) {
        require_explicit_head_settle_ = required;
    }

    // access-pattern counters for timing calibration
    uint64_t stat_reads = 0;
    uint64_t stat_seeks = 0;
    uint64_t stat_steps = 0;
    uint64_t stat_read_successes = 0;
    uint64_t stat_read_first_drqs = 0;
    uint64_t stat_read_max_first_drq_cycles = 0;
    uint64_t stat_read_sequential_gaps = 0;
    uint64_t stat_read_max_sequential_gap_cycles = 0;

    // Deterministic test-only fault injection. The command number is counted
    // over READ SECTOR commands, not individual bytes or retries.
    void set_read_fault(uint64_t command, bool persistent) {
        read_fault_command_ = command;
        read_fault_persistent_ = persistent;
        read_fault_used_ = false;
        read_fault_count_ = 0;
        read_fault_active_ = false;
    }
    uint64_t read_fault_count() const { return read_fault_count_; }
    uint8_t track_register() const { return track_reg_; }
    int selected_side() const { return side_; }

private:
    enum class State { Idle, TypeI, Read, Write, ReadAddr, WriteTrack, ReadTrack };
    enum class StatusClass { TypeI, Read, Write };

    uint8_t status_at(uint64_t now);
    bool raw_index_pulse(uint64_t now) const {
        return selected_drive_ready() && index_origin_valid_ &&
               now >= index_origin_ &&
               ((now - index_origin_) % CYC_PER_REV) < CYC_INDEX_PULSE;
    }
    bool index_pulse(uint64_t now) const {
        return status_class_ == StatusClass::TypeI && raw_index_pulse(now);
    }
    void advance_read_realtime(uint64_t now);
    void advance_write_realtime(uint64_t now);
    void advance_readaddr_realtime(uint64_t now);
    void advance_writetrack_realtime(uint64_t now);
    void advance_readtrack_realtime(uint64_t now);
    void advance_type1(uint64_t now);
    void update_force_interrupt(uint64_t now);
    void complete_command(uint64_t now);
    void update_head_unload(uint64_t now);
    void begin_head_idle_unload(uint64_t now);
    void cancel_head_idle_unload();
    void offer_read_byte(uint8_t value);
    bool begin_type23(uint64_t now);
    bool selected_drive_ready() const;
    bool active_drive_ready() const;
    bool head_engaged() const {
        return head_loaded_ && head_load_timing_;
    }
    uint64_t type1_step_cycles(uint8_t command) const {
        if (step_override_cycles_ != 0) return step_override_cycles_;
        constexpr uint64_t rates[4] = {
            6'000ULL * 6, 12'000ULL * 6,
            20'000ULL * 6, 30'000ULL * 6
        };
        return rates[command & 0x03];
    }
    uint64_t type23_delay(uint8_t command) const {
        // Mini-floppy operation uses a 1 MHz controller clock, so the
        // datasheet's 15 ms E delay at 2 MHz doubles to 30 ms.
        return (command & 0x04) ? 30'000ULL * 6 : 0;
    }
    int id_search_revolutions_for_current_id() const;
    uint64_t id_search_timeout() const {
        return static_cast<uint64_t>(record_search_revolutions_) * CYC_PER_REV;
    }
    uint64_t next_index_cycle(uint64_t now) const;
    uint64_t byte_cycles() const {
        return command_single_density_ ? CYC_PER_BYTE_FM : CYC_PER_BYTE_MFM;
    }
    uint64_t byte_ready(int index) const {
        return read_start_ + (uint64_t)index * byte_cycles();
    }
    const D88Disk::Sector* active_record() const {
        const D88Disk* d = disks_[read_drive_];
        return d ? d->record_on_track(read_cyl_, read_side_, command_track_,
                                      static_cast<uint8_t>(read_sector_),
                                      command_single_density_,
                                      command_compare_side_, command_side_id_)
                 : nullptr;
    }
    int active_transfer_size() const {
        const D88Disk::Sector* record = active_record();
        return record ? static_cast<int>(D88Disk::fdc_transfer_size(*record)) : 0;
    }
    const uint8_t* active_sector() const {
        const D88Disk::Sector* record = active_record();
        return record && D88Disk::fdc_transfer_supported(*record)
            ? record->data.data() : nullptr;
    }
    // Whether the sector READ SECTOR is currently (or about to be) serving
    // carries the deleted-data mark WRITE SECTOR's a0 flag left on it. Read
    // side of the same field set_deleted_mark()/format_track() maintain, so
    // ST_REC_TYPE in the completion status agrees with what READ TRACK
    // renders as F8h vs FBh.
    bool active_sector_deleted() const {
        const D88Disk::Sector* record = active_record();
        return record && record->deleted != 0;
    }
    // Last direction a STEP command moved the head, so a bare STEP repeats it.
    int step_dir_ = -1; // Master Reset's implicit RESTORE points outward
    bool disk_write_protected() const {
        if (!drive_mapped_) return false;
        const D88Disk* d = disks_[drive_];
        return d && d->write_protected();
    }

    D88Disk* disks_[NUM_DRIVES] = {nullptr, nullptr};
    State state_ = State::Idle;

    uint8_t track_reg_ = 0;
    uint8_t sector_reg_ = 0;
    uint8_t data_reg_ = 0;
    uint8_t done_status_ = 0; // error bits latched for when the op completes
    // The status register reuses its bits by command class. Read and write
    // commands also differ at bit 6: it is unused after reads, but reports
    // write protect after writes. Keep that class after BUSY falls so later
    // status reads cannot reinterpret INDEX as DRQ or expose write protect
    // on a completed read.
    StatusClass status_class_ = StatusClass::TypeI;
    bool head_loaded_ = false;
    bool head_load_timing_ = true;
    uint64_t head_load_timing_since_ = 0;
    bool head_idle_counting_ = false;
    uint64_t head_idle_sample_cycle_ = 0;
    uint8_t head_idle_index_count_ = 0;
    bool type1_verify_ = false;
    bool type1_verify_started_ = false;
    uint64_t type1_motion_start_ = 0;
    uint64_t type1_motion_end_ = 0;
    uint64_t type1_step_interval_ = 0;
    int type1_steps_total_ = 0;
    int type1_steps_applied_ = 0;
    int type1_drive_ = 0;
    int type1_motion_direction_ = 0;
    bool type1_update_track_ = false;
    bool type1_restore_ = false;
    uint8_t type1_verify_result_ = 0;

    int phys_cyl_[NUM_DRIVES] = {0, 0};
    int side_ = 0;
    int drive_ = 0;
    int raw_drive_number_ = 0;
    bool internal_drive_bank_high_ = false;
    bool drive_mapped_ = true;
    bool motor_ = false;
    bool drive_select_enabled_ = false;
    uint64_t index_origin_ = 0;
    bool index_origin_valid_ = false;
    bool single_density_ = false;
    // DEh is sampled when a Type II/III command starts. A mid-transfer port
    // write affects the next command, not the record or bit rate in flight.
    bool command_single_density_ = false;
    bool command_compare_side_ = false;
    uint8_t command_side_id_ = 0;
    uint8_t command_track_ = 0;
    bool require_explicit_head_settle_ = false;
    bool head_settle_pending_ = false;
    bool head_settle_violation_ = false;
    uint64_t head_settle_ready_cycle_ = 0;

    uint64_t read_fault_command_ = 0;
    bool read_fault_persistent_ = false;
    bool read_fault_used_ = false;
    uint64_t read_fault_count_ = 0;
    bool read_fault_active_ = false;

    uint64_t busy_until_ = 0;
    bool type23_started_ = false;
    uint64_t type23_ready_cycle_ = 0;
    // A missing ID is not reported until the controller has exhausted the
    // command's rotational search interval. Keep that pending condition
    // separate from status bits that are already observable while BUSY.
    bool record_search_failed_ = false;
    uint8_t record_search_revolutions_ = 4;
    // READ SECTOR learns the deleted-data mark at the start of the data
    // field and the CRC result at its end, rather than exposing both at
    // command issue time.
    uint8_t pending_read_status_ = 0;
    // The MZ-2500's real MB8876 retains the raw Type-I HLD/TR00 latch bits
    // through an E=1 READ SECTOR issued immediately after SEEK. They share
    // bit positions with Type-II Record Type/Lost Data, but are residue, not
    // transfer results. Keep them separate from done_status_ so real errors
    // and the observed chip-family quirk remain independently testable.
    uint8_t read_status_residue_ = 0;
    bool intrq_ = false;
    uint8_t force_interrupt_mask_ = 0;
    bool force_last_ready_ = false;
    uint64_t force_index_sample_cycle_ = 0;

    // active READ SECTOR (position latched at command time, data looked up
    // per byte so a swapped disk cannot dangle)
    bool read_valid_ = false;
    int read_drive_ = 0;
    int read_cyl_ = 0;
    int read_side_ = 0;
    int read_sector_ = 1;
    int read_index_ = 0;
    int read_transfer_size_ = 0;
    uint64_t read_start_ = 0; // cycle at which byte 0 becomes available
    bool data_reg_full_ = false;
    uint64_t read_command_cycle_ = 0;
    bool read_first_drq_counted_ = false;
    bool read_completion_counted_ = false;
    bool previous_read_data_valid_ = false;
    uint64_t previous_read_data_cycle_ = 0;
    int previous_read_drive_ = 0;
    int previous_read_cyl_ = 0;
    int previous_read_side_ = 0;
    int previous_read_sector_ = 0;

    // active WRITE SECTOR
    uint8_t* write_target() {
        D88Disk* d = disks_[read_drive_];
        return d ? d->write_record_on_track(
                       read_cyl_, read_side_, command_track_,
                       static_cast<uint8_t>(read_sector_),
                       command_single_density_, command_compare_side_,
                       command_side_id_)
                 : nullptr;
    }
    bool write_multiple_ = false;
    bool read_multiple_ = false;
    int write_index_ = 0;
    int write_transfer_size_ = 0;
    uint64_t write_start_ = 0;
    bool write_data_full_ = false;
    bool write_started_ = false;
    bool write_deleted_ = false;
    uint64_t byte_due(uint64_t start, int index) const {
        return start + (uint64_t)index * byte_cycles();
    }

    // active READ ADDRESS: the six ID bytes and how many have been taken
    uint8_t id_bytes_[6] = {};
    int id_index_ = 0;
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
    bool write_track_done_ = false;
    uint64_t write_track_next_due_ = 0;
    uint64_t write_track_end_ = 0;
    static constexpr int TRACK_STREAM_BYTES_MFM = 6250;
    static constexpr int TRACK_STREAM_BYTES_FM = 3125;
    int track_stream_bytes() const {
        return command_single_density_ ? TRACK_STREAM_BYTES_FM
                                       : TRACK_STREAM_BYTES_MFM;
    }
    void commit_track_stream();

    // active READ TRACK
    std::vector<uint8_t> read_track_stream_;
    int read_track_index_ = 0;

    // defaults calibrated black-box against EmuZ-2500 (P2/P4): solved from
    // three milestones (audio_boot, title with and without boot preload) so
    // both load-heavy and seek-heavy access patterns land on EmuZ's frames
    uint64_t read_latency_cycles_ = 16'480 * 6;
    uint64_t step_override_cycles_ = 0;
};

} // namespace mz
