#pragma once

#include "devices/dac_registers.hpp"
#include "audio/audio_format.hpp"
#include <array>
#include <cstdint>

namespace devices {
struct DacCommand { std::array<std::uint8_t, 3> bytes; };
DacCommand write_command(dac_registers::Register reg, std::uint8_t value) noexcept;
std::uint8_t attenuation_from_uac2_db(std::int16_t db_8_8) noexcept;

class DacTransport {
public:
    virtual ~DacTransport() = default;
    virtual void initialize() noexcept = 0;
    virtual void set_reset(bool asserted) noexcept = 0;
    virtual void set_chip_select(bool asserted) noexcept = 0;
    virtual bool transfer(const DacCommand& command) noexcept = 0;
};

struct Cs4398Timing {
    std::uint32_t reset_assert_ms{10U};
    std::uint32_t reset_release_ms{10U};
    std::uint32_t chip_select_setup_ms{1U};
    std::uint32_t inter_write_ms{1U};
};

struct Cs4398DefaultConfiguration {
    static constexpr std::array<DacCommand, 5> bootstrap{{
        {{{dac_registers::write_command, static_cast<std::uint8_t>(dac_registers::Register::Control8),
            static_cast<std::uint8_t>(dac_registers::Control8::MutedBootstrap)}}},
        {{{dac_registers::write_command, static_cast<std::uint8_t>(dac_registers::Register::Control2),
            static_cast<std::uint8_t>(dac_registers::Control2::LegacyBootstrap)}}},
        {{{dac_registers::write_command, static_cast<std::uint8_t>(dac_registers::Register::Control7),
            static_cast<std::uint8_t>(dac_registers::Control7::LegacyBootstrap)}}},
        {{{dac_registers::write_command, static_cast<std::uint8_t>(dac_registers::Register::Control8),
            static_cast<std::uint8_t>(dac_registers::Control8::RunningControlPort)}}},
        {{{dac_registers::write_command, static_cast<std::uint8_t>(dac_registers::Register::MuteControl),
            static_cast<std::uint8_t>(dac_registers::MuteControl::BothChannelsMuted)}}},
    }};
};

class Cs4398 {
public:
    enum class State : std::uint8_t {
        SafeReset, WaitingReset, WaitingRelease, WaitingChipSelect,
        WaitingInterWrite, ReadyMuted, Fault
    };

    explicit Cs4398(DacTransport& transport, Cs4398Timing timing = {}) noexcept
        : transport_(transport), timing_(timing) {}
    void begin(std::uint32_t now_ms) noexcept;
    void poll(std::uint32_t now_ms) noexcept;
    State state() const noexcept { return state_; }
    std::uint32_t deadline_ms() const noexcept { return deadline_ms_; }
    std::size_t completed_writes() const noexcept { return command_index_; }
    bool ready_muted() const noexcept { return state_ == State::ReadyMuted; }
    bool set_mute(bool muted) noexcept;
    bool set_attenuation(std::int16_t db_8_8) noexcept;
    bool configure_format(const audio::FormatConfig& format) noexcept;
    void safe_fault() noexcept;
    bool muted() const noexcept { return muted_; }
    std::int16_t attenuation_db_8_8() const noexcept { return attenuation_db_8_8_; }

private:
    static bool reached(std::uint32_t now, std::uint32_t deadline) noexcept;
    DacTransport& transport_;
    Cs4398Timing timing_;
    State state_{State::SafeReset};
    std::uint32_t deadline_ms_{0U};
    std::size_t command_index_{0U};
    bool muted_{true};
    std::int16_t attenuation_db_8_8_{0};
};
} // namespace devices
