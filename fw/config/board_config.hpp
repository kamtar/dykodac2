#pragma once

#include <cstdint>

namespace config::board {

// These values intentionally reproduce the 2022 firmware. They remain
// provisional until polarity is measured on the assembled board.
inline constexpr bool relay_disconnected_level = false;
inline constexpr bool relay_connected_level = true;
inline constexpr bool oscillator_44k1_level = false;
inline constexpr bool oscillator_48k_level = true;
inline constexpr bool dac_reset_asserted_level = false;
inline constexpr bool dac_chip_select_asserted_level = false;
// As-built prototype hack: package pin 60 (GPIO_AD_00/GPIO1_IO14)
// enables the external analog DC/DC when driven high.
inline constexpr bool dcdc_disabled_level = false;
inline constexpr bool dcdc_enabled_level = true;

inline constexpr std::uint32_t relay_release_us = 20'000U;
inline constexpr std::uint32_t oscillator_settle_us = 50'000U;
inline constexpr std::uint32_t dac_settle_us = 20'000U;
inline constexpr std::uint32_t stable_audio_us = 100'000U;

// Output level budget, in UAC2 1/256 dB units.
//
// Host volume is applied in the MCU (digital gain + TPDF dither before the
// single 24-bit quantisation). The CS4398 attenuation registers 05h/06h are
// written only with this fixed ceiling, never with the host volume. The
// ceiling protects headphones and hearing from firmware faults (for example
// a full-scale glitch) because the TPA6120 headphone amplifier has far more
// gain than any headphone needs.
//
//   maximum possible output  = DAC full scale + dac_ceiling_db_8_8
//   host volume 0 dB         = DAC full scale + dac_ceiling + mcu_headroom
//                            = -12.25 dB re DAC full scale with the values below
//
// mcu_headroom keeps reconstructed intersample peaks (about +3 dBFS for a
// full-scale fs/4 tone at 45 degrees) from clipping in the MCU even at host
// volume 0 dB. Exactly -3.00 dB is not enough: the host test measures the
// steady fs/4 tone at +3.0003 dBFS and its burst onset at +3.09 dBFS after
// reconstruction, so -3.25 dB (exact in 1/256 dB) is the smallest round
// value that never clips there.
// Raise or lower the ceiling to taste for the headphones in use; the DAC
// accepts 0 .. -127.5 dB in 0.5 dB steps.
inline constexpr std::int16_t dac_ceiling_db_8_8 = -9 * 256;
inline constexpr std::int16_t mcu_headroom_db_8_8 = -3 * 256 - 64; // -3.25 dB
static_assert(dac_ceiling_db_8_8 <= 0 && mcu_headroom_db_8_8 <= 0,
              "the level budget may only attenuate");

} // namespace config::board
