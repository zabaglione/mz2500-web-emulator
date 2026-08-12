// MZ-1E30 SASI host adapter and a single direct-access target.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mz {

class SasiController {
public:
    enum class Phase {
        BusFree,
        Command,
        DataOut,
        DataIn,
        Status,
        MessageIn,
    };

    static constexpr size_t MAX_IMAGE_SIZE = 512u * 1024u * 1024u;

    static uint32_t infer_block_size(size_t image_size);
    bool load_image(const uint8_t* data, size_t size, uint32_t block_size = 0);
    bool create_blank(size_t size, uint32_t block_size);
    void eject();

    bool loaded() const { return !image_.empty(); }
    uint32_t block_size() const { return block_size_; }
    const std::vector<uint8_t>& image() const { return image_; }
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }
    bool write_protected() const { return write_protected_; }
    void set_write_protected(bool on) { write_protected_ = on; }

    void set_target_id(uint8_t id) { target_id_ = id & 7; }
    uint8_t target_id() const { return target_id_; }

    // A4h data and A5h selection/status. A4h accesses generate the ACK
    // handshake in hardware; the next status read observes that pulse, and
    // the following read observes the target's next REQ.
    uint8_t read_data();
    void write_data(uint8_t value);
    uint8_t read_status();
    void write_selection(uint8_t value);
    Phase phase() const { return phase_; }

    // Optional BIOS ROM window at A8h/A9h. The address is assembled from
    // the high byte of the 16-bit I/O address and never auto-increments.
    void load_bios_rom(const uint8_t* data, size_t size);
    void write_bios_latch(uint8_t port_high, uint8_t value);
    uint8_t read_bios(uint8_t port_high) const;
    bool has_bios_rom() const { return !bios_rom_.empty(); }

    // Bus reset preserves target media, option ROM contents, and the host
    // adapter's separate BIOS address latch.
    void reset_interface();
    // Machine RESET also resets the host-adapter address latch.
    void reset_machine();

private:
    enum class DataOutKind { None, WriteBlocks, InitCharacteristics,
                             WriteLong };

    void finish_handshake();
    void begin_handshake(Phase next);
    Phase execute_command();
    void commit_data_out();
    Phase command_error(uint8_t sense, uint32_t lba = 0);
    uint32_t command_lba() const;
    uint32_t command_blocks() const;
    bool transfer_in_bounds(uint32_t lba, uint32_t blocks) const;
    uint8_t status_bits() const;

    std::vector<uint8_t> image_;
    uint32_t block_size_ = 256;
    bool dirty_ = false;
    bool write_protected_ = false;
    uint8_t target_id_ = 0;

    Phase phase_ = Phase::BusFree;
    Phase next_phase_ = Phase::BusFree;
    bool req_ = false;
    bool ack_ = false;
    bool handshake_pending_ = false;
    uint8_t selection_latch_ = 0;
    uint8_t data_latch_ = 0;
    uint8_t status_byte_ = 0;
    uint8_t message_byte_ = 0;
    uint8_t sense_code_ = 0;
    uint32_t sense_lba_ = 0;

    std::vector<uint8_t> command_;
    std::vector<uint8_t> transfer_;
    size_t transfer_pos_ = 0;
    uint32_t transfer_lba_ = 0;
    uint32_t transfer_blocks_ = 0;
    DataOutKind data_out_kind_ = DataOutKind::None;
    std::vector<uint8_t> bios_rom_;
    uint32_t bios_address_latch_ = 0;
};

} // namespace mz
