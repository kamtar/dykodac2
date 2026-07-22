#pragma once

#include <cstddef>
#include <cstdint>

namespace platform::nxp::update {

inline constexpr std::uint32_t flash_base = 0x60000000U;
// AT25SF081B: 8 Mbit (1 MiB) total serial NOR.
inline constexpr std::uint32_t flash_size = 1024U * 1024U;
inline constexpr std::uint32_t boot_size = 64U * 1024U;
inline constexpr std::uint32_t slot_size = 384U * 1024U;
inline constexpr std::uint32_t slot_a = flash_base + boot_size;
inline constexpr std::uint32_t slot_b = slot_a + slot_size;
inline constexpr std::uint32_t metadata_0 = slot_b + slot_size;
inline constexpr std::uint32_t metadata_1 = metadata_0 + 4096U;
inline constexpr std::uint32_t sector_size = 4096U;
inline constexpr std::uint32_t page_size = 256U;
inline constexpr std::uint32_t image_header_size = 512U;
inline constexpr std::uint32_t maximum_payload = slot_size - image_header_size;
static_assert(metadata_1 + sector_size <= flash_base + flash_size, "update layout exceeds physical flash");

inline constexpr std::uint32_t image_magic = 0x4F4B5944U; // "DYKO"
inline constexpr std::uint32_t state_magic = 0x54535944U; // "DYST"
inline constexpr std::uint32_t product_id = 0x1010D2A3U;
inline constexpr std::uint32_t enter_updater_token = 0x55504431U;
inline constexpr std::uint32_t confirm_trial_token = 0x43464D31U;
inline constexpr std::uint32_t trial_marker = 0x54524C31U;
inline constexpr std::uint32_t reset_mailbox_address = 0x2020FFE0U;
inline constexpr std::uint8_t no_slot = 0xFFU;
inline constexpr std::uint8_t maximum_trial_attempts = 3U;

struct ResetMailbox {
    std::uint32_t token;
    std::uint32_t token_inverse;
    std::uint32_t trial;
    std::uint32_t trial_inverse;
};
static_assert(sizeof(ResetMailbox) == 16U, "reset mailbox ABI");

struct ImageHeader {
    std::uint32_t magic;
    std::uint16_t format_version;
    std::uint16_t header_size;
    std::uint32_t product;
    std::uint32_t image_version;
    std::uint32_t payload_size;
    std::uint32_t vector_offset;
    std::uint32_t load_address;
    std::uint8_t sha256[32];
    std::uint32_t header_crc32;
    std::uint8_t reserved[448];
};
static_assert(sizeof(ImageHeader) == image_header_size, "image header ABI");

struct BootState {
    std::uint32_t magic;
    std::uint16_t format_version;
    std::uint16_t size;
    std::uint32_t sequence;
    std::uint8_t active_slot;
    std::uint8_t pending_slot;
    std::uint8_t trial_attempts;
    std::uint8_t flags;
    std::uint32_t active_version;
    std::uint32_t pending_version;
    std::uint32_t last_reset_status;
    std::uint32_t crc32;
    std::uint8_t reserved[224];
};
static_assert(sizeof(BootState) == page_size, "boot state ABI");

constexpr std::uint32_t slot_address(std::uint8_t slot) noexcept {
    return slot == 0U ? slot_a : slot_b;
}

std::uint32_t crc32(const void* data, std::size_t size) noexcept;

} // namespace platform::nxp::update
