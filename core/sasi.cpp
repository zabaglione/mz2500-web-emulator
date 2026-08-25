#include "core/sasi.h"

#include <algorithm>
#include <cstring>

namespace mz {

uint32_t SasiController::infer_block_size(size_t image_size) {
    // RaSCSI's MZ-1F23 compatibility image is the known exception to the
    // ordinary 256-byte SASI raw-image convention.
    if (image_size == 22437888u) return 1024;
    return (image_size != 0 && image_size % 256 == 0) ? 256 : 0;
}

bool SasiController::load_image(const uint8_t* data, size_t size,
                                uint32_t block_size) {
    if (!data || size == 0 || size > MAX_IMAGE_SIZE) return false;
    const bool auto_size = (block_size == 0);
    if (auto_size) block_size = infer_block_size(size);
    // The canonical EH-SASI 256-byte image is exactly the same 22,437,888
    // bytes as RaSCSI's 1024-byte MZ-1F23 image. Tie-break by content (only
    // when the caller asked for auto): an enhanced-driver partition-table
    // signature at 256-byte LAD 3 marks a 256-byte image ("EHSASI " plus a
    // YYYYMMDD format stamp). An explicit block size always wins.
    if (auto_size && size == 22437888u && block_size == 1024) {
        static const uint8_t kSignature[7] = {'E', 'H', 'S', 'A', 'S', 'I', ' '};
        if (std::memcmp(data + 3 * 256, kSignature, sizeof(kSignature)) == 0)
            block_size = 256;
    }
    if ((block_size != 256 && block_size != 512 && block_size != 1024) ||
        size % block_size != 0)
        return false;
    image_.assign(data, data + size);
    block_size_ = block_size;
    dirty_ = false;
    write_protected_ = false;
    reset_interface();
    return true;
}

bool SasiController::create_blank(size_t size, uint32_t block_size) {
    if (size == 0 || size > MAX_IMAGE_SIZE ||
        (block_size != 256 && block_size != 512 && block_size != 1024) ||
        size % block_size != 0)
        return false;
    image_.assign(size, 0);
    block_size_ = block_size;
    dirty_ = true;
    write_protected_ = false;
    reset_interface();
    return true;
}

void SasiController::eject() {
    image_.clear();
    dirty_ = false;
    write_protected_ = false;
    reset_interface();
}

void SasiController::reset_interface() {
    phase_ = Phase::BusFree;
    next_phase_ = Phase::BusFree;
    req_ = false;
    ack_ = false;
    handshake_pending_ = false;
    selection_latch_ = 0;
    data_latch_ = 0;
    status_byte_ = 0;
    message_byte_ = 0;
    command_.clear();
    transfer_.clear();
    transfer_pos_ = 0;
    transfer_lba_ = 0;
    transfer_blocks_ = 0;
    data_out_kind_ = DataOutKind::None;
    sense_code_ = 0;
    sense_lba_ = 0;
}

void SasiController::reset_machine() {
    reset_interface();
    bios_address_latch_ = 0;
}

void SasiController::load_bios_rom(const uint8_t* data, size_t size) {
    if (!data || size == 0) bios_rom_.clear();
    else bios_rom_.assign(data, data + size);
}

void SasiController::write_bios_latch(uint8_t port_high, uint8_t value) {
    bios_address_latch_ = (static_cast<uint32_t>(port_high & 0x0F) << 16) |
                          (static_cast<uint32_t>(value) << 8);
}

uint8_t SasiController::read_bios(uint8_t port_high) const {
    const uint32_t address = bios_address_latch_ | port_high;
    return address < bios_rom_.size() ? bios_rom_[address] : 0xFF;
}

uint8_t SasiController::status_bits() const {
    if (phase_ == Phase::BusFree) return 0;
    uint8_t value = 0x20; // BSY
    if (req_) value |= 0x80;
    if (ack_) value |= 0x40;
    switch (phase_) {
    case Phase::Command: value |= 0x08; break;
    case Phase::DataOut: break;
    case Phase::DataIn: value |= 0x04; break;
    case Phase::Status: value |= 0x0C; break;
    case Phase::MessageIn: value |= 0x1C; break;
    case Phase::BusFree: break;
    }
    return value;
}

uint8_t SasiController::read_status() {
    const uint8_t value = status_bits();
    // Return the ACK/no-REQ state once before the target presents the next
    // byte or phase. This makes the otherwise automatic A4h acknowledge
    // visible through A5h exactly as the adapter's status register permits.
    if (handshake_pending_) finish_handshake();
    return value;
}

void SasiController::write_selection(uint8_t value) {
    const bool old_sel = (selection_latch_ & 0x20) != 0;
    selection_latch_ = value;
    if (value & 0x08) {
        reset_interface();
        selection_latch_ = value;
        return;
    }
    const bool new_sel = (value & 0x20) != 0;
    if (!old_sel && new_sel && phase_ == Phase::BusFree &&
        (data_latch_ & (1u << target_id_))) {
        phase_ = Phase::Command;
        next_phase_ = Phase::Command;
        req_ = true;
        ack_ = false;
        handshake_pending_ = false;
        command_.clear();
        transfer_.clear();
        transfer_pos_ = 0;
        data_out_kind_ = DataOutKind::None;
    }
}

void SasiController::begin_handshake(Phase next) {
    next_phase_ = next;
    req_ = false;
    ack_ = true;
    handshake_pending_ = true;
}

void SasiController::finish_handshake() {
    if (!handshake_pending_) return;
    handshake_pending_ = false;
    ack_ = false;
    phase_ = next_phase_;
    req_ = phase_ != Phase::BusFree;
    if (phase_ == Phase::BusFree) {
        command_.clear();
        transfer_.clear();
        transfer_pos_ = 0;
        data_out_kind_ = DataOutKind::None;
    }
}

uint32_t SasiController::command_lba() const {
    if (command_.size() < 4) return 0;
    return (static_cast<uint32_t>(command_[1] & 0x1F) << 16) |
           (static_cast<uint32_t>(command_[2]) << 8) | command_[3];
}

uint32_t SasiController::command_blocks() const {
    if (command_.size() < 5) return 0;
    return command_[4] ? command_[4] : 256;
}

bool SasiController::transfer_in_bounds(uint32_t lba, uint32_t blocks) const {
    if (!loaded() || blocks == 0) return false;
    const uint64_t first = static_cast<uint64_t>(lba) * block_size_;
    const uint64_t bytes = static_cast<uint64_t>(blocks) * block_size_;
    return first <= image_.size() && bytes <= image_.size() - first;
}

SasiController::Phase SasiController::command_error(uint8_t sense,
                                                     uint32_t lba) {
    sense_code_ = sense;
    sense_lba_ = lba;
    status_byte_ = 0x02; // CHECK CONDITION
    transfer_.clear();
    transfer_pos_ = 0;
    data_out_kind_ = DataOutKind::None;
    return Phase::Status;
}

SasiController::Phase SasiController::execute_command() {
    transfer_.clear();
    transfer_pos_ = 0;
    transfer_lba_ = command_lba();
    transfer_blocks_ = command_blocks();
    data_out_kind_ = DataOutKind::None;
    status_byte_ = 0;
    message_byte_ = 0;

    const uint8_t opcode = command_[0];
    const uint8_t lun = command_[1] >> 5;
    if (lun != 0 && opcode != 0x03) return command_error(0x04);

    switch (opcode) {
    case 0x00: // TEST UNIT READY
    case 0x01: // REZERO UNIT
        return loaded() ? Phase::Status : command_error(0x04);

    case 0x03: { // REQUEST SENSE, legacy four-byte format
        const size_t allocation = command_[4] < 4 ? 4 : command_[4];
        transfer_.assign(allocation, 0);
        transfer_[0] = sense_code_;
        if (allocation > 1) transfer_[1] = static_cast<uint8_t>((sense_lba_ >> 16) & 0x1F);
        if (allocation > 2) transfer_[2] = static_cast<uint8_t>(sense_lba_ >> 8);
        if (allocation > 3) transfer_[3] = static_cast<uint8_t>(sense_lba_);
        sense_code_ = 0;
        sense_lba_ = 0;
        return Phase::DataIn;
    }

    case 0x04: // Xebec FORMAT DRIVE
        if (!loaded()) return command_error(0x04);
        if (write_protected_) return command_error(0x03);
        // A raw HDF has no physical track headers or defect map. Preserve the
        // controller-visible result specified by the S1410 manual: FORMAT
        // places 6Ch in every sector data field.
        std::fill(image_.begin(), image_.end(), 0x6C);
        dirty_ = true;
        return Phase::Status;

    case 0x05: // CHECK TRACK FORMAT
    case 0x06: // FORMAT TRACK (logical image has no track metadata)
    case 0x07: // FORMAT BAD TRACK (no bad-track map in a raw image)
        if (!loaded()) return command_error(0x04);
        if ((opcode == 0x06 || opcode == 0x07) && write_protected_)
            return command_error(0x03);
        return Phase::Status;

    case 0x08: { // READ(6)
        if (!transfer_in_bounds(transfer_lba_, transfer_blocks_))
            return command_error(loaded() ? 0x21 : 0x04, transfer_lba_);
        const size_t offset = static_cast<size_t>(transfer_lba_) * block_size_;
        const size_t bytes = static_cast<size_t>(transfer_blocks_) * block_size_;
        transfer_.assign(image_.begin() + offset, image_.begin() + offset + bytes);
        return Phase::DataIn;
    }

    case 0x0A: // WRITE(6)
        if (write_protected_) return command_error(0x03, transfer_lba_);
        if (!transfer_in_bounds(transfer_lba_, transfer_blocks_))
            return command_error(loaded() ? 0x21 : 0x04, transfer_lba_);
        transfer_.assign(static_cast<size_t>(transfer_blocks_) * block_size_, 0);
        data_out_kind_ = DataOutKind::WriteBlocks;
        return Phase::DataOut;

    case 0x0B: // SEEK
        return transfer_in_bounds(transfer_lba_, 1)
            ? Phase::Status : command_error(loaded() ? 0x21 : 0x04, transfer_lba_);

    case 0x0C: // Xebec INITIALIZE DRIVE CHARACTERISTICS
        transfer_.assign(8, 0);
        data_out_kind_ = DataOutKind::InitCharacteristics;
        return Phase::DataOut;

    case 0x0D: // READ ECC BURST ERROR LENGTH
        transfer_.assign(1, 0);
        return Phase::DataIn;

    case 0xE0: // Xebec RAM diagnostic
    case 0xE3: // Xebec drive diagnostic
    case 0xE4: // Xebec controller diagnostic
        return loaded() ? Phase::Status : command_error(0x04);

    case 0xE5: { // READ LONG: raw data plus four synthetic ECC bytes
        if (!transfer_in_bounds(transfer_lba_, 1))
            return command_error(loaded() ? 0x21 : 0x04, transfer_lba_);
        const size_t offset = static_cast<size_t>(transfer_lba_) * block_size_;
        transfer_.assign(image_.begin() + offset,
                         image_.begin() + offset + block_size_);
        transfer_.insert(transfer_.end(), 4, 0);
        return Phase::DataIn;
    }

    case 0xE6: // WRITE LONG
        if (write_protected_) return command_error(0x03, transfer_lba_);
        if (!transfer_in_bounds(transfer_lba_, 1))
            return command_error(loaded() ? 0x21 : 0x04, transfer_lba_);
        transfer_.assign(block_size_ + 4, 0);
        transfer_blocks_ = 1;
        data_out_kind_ = DataOutKind::WriteLong;
        return Phase::DataOut;

    default:
        return command_error(0x20, transfer_lba_); // invalid command
    }
}

void SasiController::commit_data_out() {
    switch (data_out_kind_) {
    case DataOutKind::WriteBlocks: {
        const size_t offset = static_cast<size_t>(transfer_lba_) * block_size_;
        std::copy(transfer_.begin(), transfer_.end(), image_.begin() + offset);
        dirty_ = true;
        break;
    }
    case DataOutKind::WriteLong: {
        const size_t offset = static_cast<size_t>(transfer_lba_) * block_size_;
        std::copy_n(transfer_.begin(), block_size_, image_.begin() + offset);
        dirty_ = true;
        break;
    }
    case DataOutKind::InitCharacteristics:
    case DataOutKind::None:
        break;
    }
    data_out_kind_ = DataOutKind::None;
}

void SasiController::write_data(uint8_t value) {
    if (handshake_pending_) finish_handshake();
    if (phase_ == Phase::BusFree) {
        data_latch_ = value;
        return;
    }
    if (!req_) return;

    if (phase_ == Phase::Command) {
        command_.push_back(value);
        const Phase next = command_.size() < 6 ? Phase::Command : execute_command();
        begin_handshake(next);
        return;
    }
    if (phase_ == Phase::DataOut) {
        if (transfer_pos_ < transfer_.size()) transfer_[transfer_pos_++] = value;
        Phase next = Phase::DataOut;
        if (transfer_pos_ >= transfer_.size()) {
            commit_data_out();
            next = Phase::Status;
        }
        begin_handshake(next);
    }
}

uint8_t SasiController::read_data() {
    if (handshake_pending_) finish_handshake();
    if (!req_) return data_latch_;

    uint8_t value = data_latch_;
    Phase next = phase_;
    switch (phase_) {
    case Phase::DataIn:
        if (transfer_pos_ < transfer_.size()) value = transfer_[transfer_pos_++];
        next = transfer_pos_ < transfer_.size() ? Phase::DataIn : Phase::Status;
        break;
    case Phase::Status:
        value = status_byte_;
        next = Phase::MessageIn;
        break;
    case Phase::MessageIn:
        value = message_byte_;
        next = Phase::BusFree;
        break;
    default:
        return data_latch_;
    }
    data_latch_ = value;
    begin_handshake(next);
    return value;
}

} // namespace mz
