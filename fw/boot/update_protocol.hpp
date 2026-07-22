#pragma once
#include <cstdint>

namespace boot::protocol {
inline constexpr std::uint32_t magic = 0x554B5944U; // "DYKU"
enum class Command : std::uint8_t { Info=1, Begin=2, Data=3, End=4, Abort=5, Reboot=6 };
enum class Status : std::uint8_t { Ok=0, BadPacket=1, BadState=2, Flash=3, Verify=4, WrongImage=5, Range=6 };
struct Report {
    std::uint32_t magic;
    std::uint8_t command;
    std::uint8_t sequence;
    std::uint8_t status;
    std::uint8_t length;
    std::uint32_t offset;
    std::uint8_t data[48];
    std::uint32_t crc32;
};
static_assert(sizeof(Report)==64U,"HID report ABI");
}
