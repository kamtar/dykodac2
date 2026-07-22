#pragma once

#include <cstddef>
#include <cstdint>

namespace platform::nxp::crypto {
class Sha256 {
public:
    Sha256() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    void finish(std::uint8_t digest[32]) noexcept;
private:
    void transform(const std::uint8_t block[64]) noexcept;
    std::uint32_t state_[8];
    std::uint64_t bytes_{0U};
    std::uint8_t block_[64]{};
    std::size_t used_{0U};
};
}
