#include "audio/stream_buffer.hpp"

#include <cstring>

#if !defined(DYKODAC_HOST_TEST)
extern "C" {
#include "MIMXRT1011.h"
}
#endif

namespace {
class InterruptGuard {
public:
    InterruptGuard() noexcept {
#if !defined(DYKODAC_HOST_TEST)
        primask_ = __get_PRIMASK();
        __disable_irq();
        __DMB();
#endif
    }
    ~InterruptGuard() noexcept {
#if !defined(DYKODAC_HOST_TEST)
        __DMB();
        if (primask_ == 0U) __enable_irq();
#endif
    }
private:
#if !defined(DYKODAC_HOST_TEST)
    std::uint32_t primask_{0U};
#endif
};
}

namespace audio {
bool StreamBuffer::push(const std::uint8_t* data, std::size_t size) noexcept {
    // TinyUSB is the producer in main context and eDMA is the consumer in an
    // ISR. Keep the bounded memcpy and all three indices in one critical
    // section so neither side can observe a partially published packet.
    InterruptGuard guard;
    if (size > capacity_bytes - used_) { ++overflows_; return false; }
    const std::size_t first = (size < capacity_bytes - write_) ? size : capacity_bytes - write_;
    std::memcpy(&storage_[write_], data, first);
    std::memcpy(&storage_[0], data + first, size - first);
    write_ = (write_ + size) % capacity_bytes;
    used_ += size;
    return true;
}

std::size_t StreamBuffer::pop(std::uint8_t* data, std::size_t size) noexcept {
    InterruptGuard guard;
    const std::size_t count = size < used_ ? size : used_;
    const std::size_t first = count < capacity_bytes - read_ ? count : capacity_bytes - read_;
    std::memcpy(data, &storage_[read_], first);
    std::memcpy(data + first, &storage_[0], count - first);
    if (count < size) { std::memset(data + count, 0, size - count); ++underruns_; }
    read_ = (read_ + count) % capacity_bytes;
    used_ -= count;
    return count;
}

void StreamBuffer::clear() noexcept {
    InterruptGuard guard;
    read_ = 0U;
    write_ = 0U;
    used_ = 0U;
}

std::size_t StreamBuffer::size() const noexcept {
    InterruptGuard guard;
    return used_;
}

std::size_t StreamBuffer::free() const noexcept {
    InterruptGuard guard;
    return capacity_bytes - used_;
}
} // namespace audio
