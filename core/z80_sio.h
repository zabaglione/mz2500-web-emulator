// Zilog Z80 SIO/0 dual-channel serial controller.
//
// The MZ-2500 uses the asynchronous facilities for its two RS-232C ports
// and reuses channel B for the mouse. This model keeps the CPU-visible
// register, FIFO, timing and interrupt contracts independent from any host
// transport. A browser terminal or Web Serial adapter only drains completed
// TxByte records and injects already-decoded receive bytes.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mz {

class Z80Sio {
public:
    enum RxError : uint8_t {
        RX_OK = 0,
        RX_PARITY_ERROR = 0x10,
        RX_OVERRUN_ERROR = 0x20,
        RX_FRAMING_ERROR = 0x40,
    };

    enum class Parity : uint8_t { None, Odd, Even };

    struct TxByte {
        uint8_t value = 0;
        uint8_t data_bits = 8;
        Parity parity = Parity::None;
        uint8_t stop_half_bits = 2; // 2=1 bit, 3=1.5 bits, 4=2 bits
        uint32_t baud = 0;
        uint64_t completed_cycle = 0;
    };

    Z80Sio() { reset(); }

    // External reset. The MZ clock generator is outside the SIO, so clock
    // rates survive this call; all SIO registers, FIFOs, line outputs and
    // interrupt state do not.
    void reset(uint64_t cycle = 0);

    uint8_t read_data(int channel, uint64_t cycle);
    // Returns machine cycles spent on /WAIT when the holding register was
    // full. If the transmitter has no finite completion deadline (disabled
    // with a full buffer), the byte remains unaccepted and the return is 0.
    uint64_t write_data(int channel, uint8_t value, uint64_t cycle);
    uint8_t read_control(int channel, uint64_t cycle);
    void write_control(int channel, uint8_t value, uint64_t cycle);

    // TxC/RxC are common per channel on the MZ-2500. WR4 selects x1/x16/
    // x32/x64 beneath this input frequency.
    void set_clock_hz(int channel, uint32_t hz);
    uint32_t clock_hz(int channel) const;
    uint32_t baud(int channel) const;
    int receive_bits(int channel) const;
    int transmit_bits(int channel) const;
    uint8_t stop_half_bits(int channel) const;
    Parity parity(int channel) const;
    bool receiver_enabled(int channel) const;
    bool transmitter_enabled(int channel) const;

    // Move shift-register deadlines up to `cycle`. Completed characters are
    // retained until the host transport drains them.
    void advance(uint64_t cycle);
    bool pop_transmitted(int channel, TxByte& value);
    size_t transmitted_available(int channel) const;
    uint64_t transmitted_overruns(int channel) const;

    // An injected byte represents a complete character at RxD. Disabled
    // receivers discard it, as does Auto Enables while DCD is inactive.
    void receive_byte(int channel, uint8_t value, uint8_t errors,
                      uint64_t cycle);
    // Host transports commonly deliver several already-buffered bytes at
    // once. Queue them here so the SIO exposes each character to the CPU at
    // the programmed line rate instead of overflowing the three-byte FIFO
    // in one browser callback.
    bool queue_receive_byte(int channel, uint8_t value, uint8_t errors,
                            uint64_t cycle);
    bool rx_available(int channel) const;

    // Logical modem states use the RS-232C meaning: true means asserted.
    // The SIO pins are active-low, and RR0 reports their inverted pin level,
    // so an asserted CTS/DCD appears as a set RR0 bit.
    void set_modem_inputs(int channel, bool cts, bool dcd, bool sync,
                          uint64_t cycle);
    void set_break_input(int channel, bool active, uint64_t cycle);
    bool dtr(int channel) const;
    bool rts(int channel) const;
    bool break_active(int channel) const;

    // Z80 daisy-chain boundary. acknowledge_interrupt() selects the highest
    // eligible internal source, returns its vector, and enters under-service
    // state. reti() releases the highest-priority source under service.
    bool interrupt_pending(uint64_t cycle);
    bool acknowledge_interrupt(uint64_t cycle, uint8_t& vector);
    void reti();

private:
    static constexpr size_t RX_FIFO_SIZE = 3;
    static constexpr size_t RX_INPUT_SIZE = 4096;
    static constexpr size_t TX_OUTPUT_SIZE = 1024;

    struct RxByte {
        uint8_t value = 0;
        uint8_t errors = RX_OK;
    };

    struct Channel {
        uint8_t regs[8] = {};
        uint8_t pointer = 0;

        std::array<RxByte, RX_FIFO_SIZE> rx{};
        size_t rx_count = 0;
        uint8_t error_latch = 0;
        bool first_rx_armed = true;
        bool rx_special_pending = false;

        std::array<RxByte, RX_INPUT_SIZE> rx_input{};
        size_t rx_input_head = 0;
        size_t rx_input_count = 0;
        uint64_t rx_input_next = 0;
        uint64_t rx_input_overruns = 0;

        bool tx_buffer_full = false;
        uint8_t tx_buffer = 0;
        bool tx_shift_active = false;
        bool tx_underrun_eom = true;
        TxByte tx_shift{};
        uint64_t tx_shift_end = 0;

        std::array<TxByte, TX_OUTPUT_SIZE> tx_output{};
        size_t tx_output_head = 0;
        size_t tx_output_count = 0;
        uint64_t tx_output_overruns = 0;

        uint32_t clock_hz = 307'200;
        bool cts = false;
        bool dcd = false;
        bool sync = false;
        bool break_input = false;
        bool rts_output = false;
    };

    enum InterruptLevel : int {
        A_RX = 0,
        A_TX,
        A_EXT,
        B_RX,
        B_TX,
        B_EXT,
        INTERRUPT_LEVELS,
    };

    struct InterruptState {
        bool pending = false;
        bool in_service = false;
    };

    Channel channel_[2];
    InterruptState interrupt_[INTERRUPT_LEVELS];

    static int index(int channel) { return channel & 1; }
    static int rx_level(int channel) { return channel ? B_RX : A_RX; }
    static int tx_level(int channel) { return channel ? B_TX : A_TX; }
    static int ext_level(int channel) { return channel ? B_EXT : A_EXT; }

    void reset_channel(int channel);
    void execute_wr0(int channel, uint8_t value, uint64_t cycle);
    void write_register(int channel, int reg, uint8_t value, uint64_t cycle);
    uint8_t rr0(int channel) const;
    uint8_t rr1(int channel) const;
    uint8_t rr2() const;

    int clock_multiplier(int channel) const;
    uint64_t character_cycles(int channel) const;
    uint64_t receive_character_cycles(int channel) const;

    void try_start_transmit(int channel, uint64_t cycle);
    void complete_transmit(int channel, uint64_t cycle);
    void queue_transmitted(int channel, const TxByte& value);
    void accept_received(int channel, uint8_t value, uint8_t errors);
    void update_receive_interrupt(int channel);
    void set_external_pending(int channel);
    int highest_eligible_interrupt() const;
    uint8_t vector_for_level(int level) const;
};

inline Z80Sio::RxError operator|(Z80Sio::RxError a, Z80Sio::RxError b) {
    return static_cast<Z80Sio::RxError>(static_cast<uint8_t>(a) |
                                        static_cast<uint8_t>(b));
}

} // namespace mz
