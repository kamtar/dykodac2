#pragma once

// HID configuration protocol (64-byte OUT request -> 64-byte IN response).
// Mirrored by tools/dykodac_config/protocol.py; keep both in step.

#include <array>
#include <cstddef>
#include <cstdint>

namespace usb::config {

inline constexpr std::uint16_t protocol_version = 1U;
inline constexpr std::array<std::uint8_t, 8> request_prefix{{'D','Y','K','O','-','C','F','G'}};
inline constexpr std::uint32_t response_magic = 0x52434B44U; // "DKCR"
inline constexpr std::size_t chunk_bytes = 48U;

enum class Opcode : std::uint8_t {
    Info = 1U,     // status block in data
    Read = 2U,     // active (RAM) settings bytes [offset, offset + length)
    Write = 3U,    // staging bytes [offset, offset + length) from data
    Apply = 4U,    // validate staging, make it active ("write into RAM")
    Save = 5U,     // firmware-side save; FlashUnavailable on RT1011 (host writes via flashloader)
    Load = 6U,     // flash -> active and staging
    Defaults = 7U, // factory defaults -> active and staging (RAM only)
    PeekRom = 0x20U, // diagnostic: `length` bytes of boot ROM at data[0..3]
};

enum class Status : std::uint8_t {
    Ok = 0U, BadRequest = 1U, BadRange = 2U, InvalidSettings = 3U,
    FlashError = 4U, NoFlashRecord = 5U, FlashUnavailable = 6U,
};

struct Request {
    std::array<std::uint8_t, 8> prefix;
    std::uint8_t opcode;
    std::uint8_t tag;
    std::uint16_t offset;
    std::uint8_t length;
    std::array<std::uint8_t, 3> reserved;
    std::array<std::uint8_t, chunk_bytes> data;
};
static_assert(sizeof(Request) == 64U, "config request must be one HID packet");

struct Response {
    std::uint32_t magic{response_magic};
    std::uint8_t opcode{0U};
    std::uint8_t tag{0U};
    std::uint8_t status{0U};
    std::uint8_t reserved0{0U};
    std::uint16_t offset{0U};
    std::uint8_t length{0U};
    std::uint8_t reserved1{0U};
    std::uint32_t aux{0U}; // Apply: settings::Invalid; flash ops: driver status
    std::array<std::uint8_t, chunk_bytes> data{};
};
static_assert(sizeof(Response) == 64U, "config response must be one HID packet");

// Info flags.
inline constexpr std::uint8_t flag_flash_available = 0x01U; // firmware can Save itself
inline constexpr std::uint8_t flag_flash_record = 0x02U;
inline constexpr std::uint8_t flag_unsaved = 0x04U;      // active != last flash record
inline constexpr std::uint8_t flag_streaming = 0x08U;
inline constexpr std::uint8_t flag_bit_exact = 0x10U;    // bypass copying samples untouched
inline constexpr std::uint8_t flag_staging_dirty = 0x20U; // staging != active

struct Info {
    std::uint16_t protocol{protocol_version};
    std::uint16_t settings_size{0U};
    std::uint16_t settings_version{0U};
    std::uint8_t eq_bands{0U};
    std::uint8_t flags{0U};
    std::uint32_t flash_sequence{0U};
    std::uint32_t source_rate_hz{0U};
    std::uint32_t output_rate_hz{0U};
    std::int16_t host_volume_db_8_8{0};
    std::int16_t headroom_db_8_8{0};
    float dsp_boost_db{0.0F};
    float effective_headroom_db{0.0F};
    std::uint8_t app_state{0U};
    std::uint8_t upsampler_bypass_active{0U};
    std::uint8_t last_flash_status{0U};
    std::uint8_t reserved{0U};
    std::uint32_t reserved1{0U};
};
static_assert(sizeof(Info) <= chunk_bytes, "info must fit one response");

inline bool is_request(const std::uint8_t* data, std::size_t size) noexcept {
    if (size < sizeof(Request)) return false;
    for (std::size_t i = 0U; i < request_prefix.size(); ++i) if (data[i] != request_prefix[i]) return false;
    return true;
}

} // namespace usb::config
