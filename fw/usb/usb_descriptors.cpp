#include "tusb.h"
#include "usb/usb_descriptors.h"
#include "usb/descriptor_model.hpp"

#include <cstring>
#include <cstdint>

namespace {
tusb_desc_device_t const device{
    sizeof(tusb_desc_device_t), TUSB_DESC_DEVICE, 0x0200, TUSB_CLASS_MISC,
    MISC_SUBCLASS_COMMON, MISC_PROTOCOL_IAD, CFG_TUD_ENDPOINT0_SIZE,
    usb::descriptor_model::vid, usb::descriptor_model::pid, 0x0100, 1, 2, 3, 1
};
std::uint8_t const hid_report[] = { TUD_HID_REPORT_DESC_GENERIC_INOUT(CFG_TUD_HID_EP_BUFSIZE) };
constexpr std::uint16_t total_length = TUD_CONFIG_DESC_LEN + DYKODAC_AUDIO_DESC_LEN + TUD_HID_INOUT_DESC_LEN;
std::uint8_t const configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, total_length, 0, 100),
    DYKODAC_AUDIO_DESCRIPTOR(CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX),
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_MAINTENANCE, 5, HID_ITF_PROTOCOL_NONE,
        sizeof(hid_report), 0x02, 0x82, CFG_TUD_HID_EP_BUFSIZE, 4)
};
char const* strings[] = {nullptr, "Dyko", "DykoDAC2 UAC2+Maintenance", "0001", "DykoDAC2 Audio", "DykoDAC2 Maintenance"};
std::uint16_t string_buffer[33];
}

extern "C" std::uint8_t const* tud_descriptor_device_cb() { return reinterpret_cast<std::uint8_t const*>(&device); }
extern "C" std::uint8_t const* tud_descriptor_configuration_cb(std::uint8_t) { return configuration; }
extern "C" std::uint8_t const* tud_hid_descriptor_report_cb(std::uint8_t) { return hid_report; }
extern "C" std::uint16_t const* tud_descriptor_string_cb(std::uint8_t index, std::uint16_t) {
    if (index == 0U) { string_buffer[1] = 0x0409U; string_buffer[0] = (TUSB_DESC_STRING << 8U) | 4U; return string_buffer; }
    if (index >= sizeof(strings) / sizeof(strings[0])) return nullptr;
    const char* source = strings[index];
    std::size_t count = std::strlen(source); if (count > 32U) count = 32U;
    for (std::size_t i = 0; i < count; ++i) string_buffer[i + 1U] = static_cast<std::uint8_t>(source[i]);
    string_buffer[0] = static_cast<std::uint16_t>((TUSB_DESC_STRING << 8U) | (2U * count + 2U));
    return string_buffer;
}
