#pragma once
#include <cstddef>
#include <cstdint>

namespace boot::flash {
bool initialize() noexcept;
std::uint32_t jedec_id() noexcept;
bool erase_sector(std::uint32_t address) noexcept;
bool program(std::uint32_t address, const void* data, std::size_t size) noexcept;
bool erased(std::uint32_t address, std::size_t size) noexcept;
void invalidate_ahb() noexcept;
}
