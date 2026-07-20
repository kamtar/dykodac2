#include "usb/usb_device.hpp"

#if defined(DYKODAC_HOST_TEST)
namespace usb {
EventRecord UsbDevice::take_event() noexcept {
    if (event_count_ == 0U) return {};
    const EventRecord result = events_[event_read_];
    event_read_ = static_cast<std::uint8_t>((event_read_ + 1U) % event_capacity);
    --event_count_;
    return result;
}
bool UsbDevice::post_event(EventRecord event) noexcept {
    if (event_count_ == event_capacity) { ++dropped_events_; return false; }
    events_[event_write_] = event;
    event_write_ = static_cast<std::uint8_t>((event_write_ + 1U) % event_capacity);
    ++event_count_;
    return true;
}
} // namespace usb
#endif
