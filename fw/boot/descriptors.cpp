#include "tusb.h"
#include <cstdint>
#include <cstring>

namespace {
constexpr std::uint16_t vid=0x1209U,pid=0xD2A4U;
tusb_desc_device_t const device{sizeof(tusb_desc_device_t),TUSB_DESC_DEVICE,0x0200,TUSB_CLASS_MISC,
    MISC_SUBCLASS_COMMON,MISC_PROTOCOL_IAD,64,vid,pid,0x0100,1,2,3,1};
std::uint8_t const report_desc[]={TUD_HID_REPORT_DESC_GENERIC_INOUT(64)};
std::uint8_t const config[]={TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_HID_INOUT_DESC_LEN,0,100),
    TUD_HID_INOUT_DESCRIPTOR(0,4,HID_ITF_PROTOCOL_NONE,sizeof(report_desc),0x01,0x81,64,1)};
char const* strings[]={nullptr,"Dyko","DykoDAC2 Firmware Updater","UPDATER","Firmware Update"};
std::uint16_t string_buffer[33];
}
extern "C" std::uint8_t const* tud_descriptor_device_cb(){return reinterpret_cast<const std::uint8_t*>(&device);}
extern "C" std::uint8_t const* tud_descriptor_configuration_cb(std::uint8_t){return config;}
extern "C" std::uint8_t const* tud_hid_descriptor_report_cb(std::uint8_t){return report_desc;}
extern "C" std::uint16_t const* tud_descriptor_string_cb(std::uint8_t index,std::uint16_t){
    if(index==0U){string_buffer[1]=0x0409U;string_buffer[0]=(TUSB_DESC_STRING<<8U)|4U;return string_buffer;}
    if(index>=sizeof(strings)/sizeof(strings[0]))return nullptr;const char* s=strings[index];std::size_t n=std::strlen(s);if(n>32U)n=32U;
    for(std::size_t i=0;i<n;++i)string_buffer[i+1U]=static_cast<std::uint8_t>(s[i]);
    string_buffer[0]=static_cast<std::uint16_t>((TUSB_DESC_STRING<<8U)|(2U*n+2U));return string_buffer;
}
