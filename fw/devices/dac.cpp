#include "devices/dac.hpp"

namespace devices {
DacCommand write_command(dac_registers::Register reg, std::uint8_t value) noexcept {
    return {{{dac_registers::write_command, static_cast<std::uint8_t>(reg), value}}};
}

std::uint8_t attenuation_from_uac2_db(std::int16_t db_8_8) noexcept {
    // DAC uses positive 0.5 dB attenuation steps. Clamp host control to
    // 0 .. -127.5 dB without floating point.
    if (db_8_8 >= 0) return 0U;
    constexpr std::int32_t half_db = 128;
    std::int32_t magnitude = -static_cast<std::int32_t>(db_8_8);
    std::int32_t steps = (magnitude + half_db / 2) / half_db;
    if (steps > 255) steps = 255;
    return static_cast<std::uint8_t>(steps);
}

bool Cs4398::reached(std::uint32_t now, std::uint32_t deadline) noexcept {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

void Cs4398::begin(std::uint32_t now_ms) noexcept {
    transport_.initialize();
    transport_.set_chip_select(false);
    transport_.set_reset(true);
    command_index_ = 0U;
    muted_ = true;
    deadline_ms_ = now_ms + timing_.reset_assert_ms;
    state_ = State::WaitingReset;
}

bool Cs4398::set_mute(bool muted) noexcept {
    if (state_ != State::ReadyMuted) return false;
    transport_.set_chip_select(true);
    const bool ok = transport_.transfer(write_command(dac_registers::Register::MuteControl,
        // Preserve PCM/DSD auto-mute (bits 7:6). MUTE_A and MUTE_B are bits
        // 4 and 3 respectively: D8 mutes both; C0 unmutes both.
        muted ? 0xD8U : 0xC0U));
    transport_.set_chip_select(false);
    if (!ok) { safe_fault(); return false; }
    muted_ = muted;
    return true;
}

bool Cs4398::set_attenuation(std::int16_t db_8_8) noexcept {
    if (state_ != State::ReadyMuted) return false;
    if (db_8_8 > 0) db_8_8 = 0;
    if (db_8_8 < -0x7F80) db_8_8 = -0x7F80;
    const std::uint8_t value = attenuation_from_uac2_db(db_8_8);
    transport_.set_chip_select(true);
    const bool left = transport_.transfer(write_command(dac_registers::Register::AttenuationLeft, value));
    transport_.set_chip_select(false);
    transport_.set_chip_select(true);
    const bool right = transport_.transfer(write_command(dac_registers::Register::AttenuationRight, value));
    transport_.set_chip_select(false);
    if (!left || !right) { safe_fault(); return false; }
    attenuation_db_8_8_ = db_8_8;
    return true;
}

bool Cs4398::configure_format(const audio::FormatConfig& format) noexcept {
    if (state_ != State::ReadyMuted || format.dac_mode != 0U) return false;
    transport_.set_chip_select(true);
    const bool ok = transport_.transfer(write_command(dac_registers::Register::Control7, 0xB0U));
    transport_.set_chip_select(false);
    if (!ok) safe_fault();
    return ok;
}

void Cs4398::safe_fault() noexcept {
    transport_.set_chip_select(false);
    transport_.set_reset(true);
    muted_ = true;
    state_ = State::Fault;
}

void Cs4398::poll(std::uint32_t now_ms) noexcept {
    if (!reached(now_ms, deadline_ms_)) return;
    switch (state_) {
    case State::WaitingReset:
        transport_.set_reset(false);
        deadline_ms_ = now_ms + timing_.reset_release_ms;
        state_ = State::WaitingRelease;
        break;
    case State::WaitingRelease:
    case State::WaitingInterWrite:
        transport_.set_chip_select(true);
        deadline_ms_ = now_ms + timing_.chip_select_setup_ms;
        state_ = State::WaitingChipSelect;
        break;
    case State::WaitingChipSelect:
        if (!transport_.transfer(Cs4398DefaultConfiguration::bootstrap[command_index_])) {
            transport_.set_chip_select(false);
            transport_.set_reset(true);
            state_ = State::Fault;
            break;
        }
        transport_.set_chip_select(false);
        ++command_index_;
        if (command_index_ == Cs4398DefaultConfiguration::bootstrap.size()) {
            state_ = State::ReadyMuted;
        } else {
            deadline_ms_ = now_ms + timing_.inter_write_ms;
            state_ = State::WaitingInterWrite;
        }
        break;
    default:
        break;
    }
}
} // namespace devices
