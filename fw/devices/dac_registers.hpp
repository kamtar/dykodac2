#pragma once

#include <cstdint>

namespace devices::dac_registers {
enum class Register : std::uint8_t {
    Control2 = 0x02,
    MuteControl = 0x04,
    AttenuationLeft = 0x05,
    AttenuationRight = 0x06,
    Control7 = 0x07,
    Control8 = 0x08,
};
inline constexpr std::uint8_t write_command = 0x98U;

enum class Control2 : std::uint8_t { LegacyBootstrap = 0x10U };
enum class Control7 : std::uint8_t { LegacyBootstrap = 0xB0U };
enum class MuteControl : std::uint8_t {
    // Keep PCM/DSD auto-mute disabled. The audio engine continuously supplies
    // digital zero while the host is idle, so DAC silence detection must not
    // change the settled analog output state.
    BothChannelsMuted = 0x18U,
    BothChannelsUnmuted = 0x00U,
};
enum class Control8 : std::uint8_t {
    MutedBootstrap = 0xC0U,
    RunningControlPort = 0x40U,
};
} // namespace devices::dac_registers
