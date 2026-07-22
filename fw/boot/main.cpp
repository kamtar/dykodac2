#include "boot/flexspi_flash.hpp"
#include "boot/update_protocol.hpp"
#include "platform/nxp/mpu_config.hpp"
#include "platform/nxp/sha256.hpp"
#include "platform/nxp/update_layout.hpp"
#include "tusb.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include "clock_config.h"
#include "fsl_clock.h"
#include "MIMXRT1011.h"
extern std::uint32_t __ram_vectors;
void SystemInit(void);
}

namespace {
using namespace platform::nxp;
using update::BootState;
using update::ImageHeader;

BootState state{};
std::uint32_t state_address=0U;
boot::protocol::Report response{};
bool response_pending=false;
bool receiving=false;
bool flash_initialized=false;
std::uint8_t receive_slot=update::no_slot;
std::uint32_t receive_size=0U;
std::uint32_t receive_next=0U;

volatile update::ResetMailbox& mailbox(){
    return *reinterpret_cast<volatile update::ResetMailbox*>(update::reset_mailbox_address);
}

bool equal_digest(const std::uint8_t* a,const std::uint8_t* b){std::uint8_t x=0U;for(unsigned i=0;i<32U;++i)x|=a[i]^b[i];return x==0U;}

bool valid_header(std::uint8_t slot,bool full_hash) {
    const auto base=update::slot_address(slot);const auto& h=*reinterpret_cast<const ImageHeader*>(base);
    if(h.magic!=update::image_magic||h.format_version!=1U||h.header_size!=sizeof(ImageHeader)||h.product!=update::product_id||
       h.payload_size==0U||h.payload_size>update::maximum_payload||h.vector_offset!=update::image_header_size||
       h.load_address!=base+update::image_header_size)return false;
    ImageHeader copy=h;const auto expected=copy.header_crc32;copy.header_crc32=0U;
    if(update::crc32(&copy,sizeof(copy))!=expected)return false;
    const auto* vectors=reinterpret_cast<const std::uint32_t*>(base+h.vector_offset);
    if(vectors[0]<0x20000000U||vectors[0]>0x20210000U||vectors[1]<base||vectors[1]>=base+update::slot_size)return false;
    if(!full_hash)return true;
    crypto::Sha256 sha;sha.update(reinterpret_cast<const void*>(base+h.vector_offset),h.payload_size);std::uint8_t digest[32];sha.finish(digest);
    return equal_digest(digest,h.sha256);
}

bool valid_state(const BootState& s){if(s.magic!=update::state_magic||s.format_version!=1U||s.size!=sizeof(BootState))return false;BootState c=s;const auto crc=c.crc32;c.crc32=0U;return update::crc32(&c,sizeof(c))==crc;}

void load_state(){
    const auto& a=*reinterpret_cast<const BootState*>(update::metadata_0);const auto& b=*reinterpret_cast<const BootState*>(update::metadata_1);
    const bool va=valid_state(a),vb=valid_state(b);
    if(va&&(!vb||a.sequence>=b.sequence)){state=a;state_address=update::metadata_0;}
    else if(vb){state=b;state_address=update::metadata_1;}
    else{state={};state.magic=update::state_magic;state.format_version=1U;state.size=sizeof(BootState);state.active_slot=0U;state.pending_slot=update::no_slot;state_address=0U;}
}

bool ensure_flash(){
    if(!flash_initialized)flash_initialized=boot::flash::initialize();
    return flash_initialized;
}

bool save_state(){
    if(!ensure_flash())return false;
    const auto target=state_address==update::metadata_0?update::metadata_1:update::metadata_0;
    ++state.sequence;state.crc32=0U;state.crc32=update::crc32(&state,sizeof(state));
    if(!boot::flash::erase_sector(target)||!boot::flash::program(target,&state,128U)||
       !boot::flash::program(target+128U,reinterpret_cast<const std::uint8_t*>(&state)+128U,128U))return false;
    state_address=target;return true;
}

[[noreturn]] void jump_to(std::uint8_t slot,bool trial){
    const auto base=update::slot_address(slot);const auto& h=*reinterpret_cast<const ImageHeader*>(base);
    const auto* vectors=reinterpret_cast<const std::uint32_t*>(base+h.vector_offset);
    const auto marker=trial?update::trial_marker:0U;
    mailbox().trial=marker;mailbox().trial_inverse=~marker;
    SCB_CleanDCache_by_Addr(reinterpret_cast<std::uint32_t*>(update::reset_mailbox_address),sizeof(update::ResetMailbox));
    SNVS->LPGPR[1]=marker;
    __disable_irq();SysTick->CTRL=0U;NVIC->ICER[0]=0xFFFFFFFFU;NVIC->ICER[1]=0xFFFFFFFFU;NVIC->ICER[2]=0xFFFFFFFFU;NVIC->ICER[3]=0xFFFFFFFFU;
    SCB_CleanInvalidateDCache();SCB->VTOR=base+h.vector_offset;__set_MSP(vectors[0]);__DSB();__ISB();
    reinterpret_cast<void(*)()>(vectors[1])();for(;;)__asm volatile("wfi");
}

void choose_image(){
    load_state();state.last_reset_status=SRC->SRSR;
    const auto ram_token=mailbox().token;
    const bool ram_token_valid=mailbox().token_inverse==~ram_token;
    mailbox().token=0U;mailbox().token_inverse=~0U;
    SCB_CleanDCache_by_Addr(reinterpret_cast<std::uint32_t*>(update::reset_mailbox_address),sizeof(update::ResetMailbox));
    const auto token=ram_token_valid?ram_token:SNVS->LPGPR[0];SNVS->LPGPR[0]=0U;
    if(token==update::confirm_trial_token&&state.pending_slot!=update::no_slot&&valid_header(state.pending_slot,true)){
        state.active_slot=state.pending_slot;state.active_version=reinterpret_cast<const ImageHeader*>(update::slot_address(state.active_slot))->image_version;
        state.pending_slot=update::no_slot;state.pending_version=0U;state.trial_attempts=0U;save_state();
    }
    if(token==update::enter_updater_token)return;
    if(state.pending_slot!=update::no_slot){
        if(valid_header(state.pending_slot,true)&&state.trial_attempts<update::maximum_trial_attempts){++state.trial_attempts;save_state();jump_to(state.pending_slot,true);}
        state.pending_slot=update::no_slot;state.pending_version=0U;state.trial_attempts=0U;save_state();
    }
    if(state.active_slot<2U&&valid_header(state.active_slot,true))jump_to(state.active_slot,false);
    const std::uint8_t other=state.active_slot==0U?1U:0U;if(valid_header(other,true)){state.active_slot=other;save_state();jump_to(other,false);}
}

void reply(boot::protocol::Command command,std::uint8_t sequence,boot::protocol::Status status,std::uint32_t offset=0U){
    response={};response.magic=boot::protocol::magic;response.command=static_cast<std::uint8_t>(command);response.sequence=sequence;response.status=static_cast<std::uint8_t>(status);response.offset=offset;
    response.data[0]=state.active_slot;response.data[1]=state.pending_slot;response.data[2]=state.trial_attempts;response.data[3]=receive_slot;
    std::memcpy(response.data+4,&receive_size,4);const auto id=boot::flash::jedec_id();std::memcpy(response.data+8,&id,4);
    response.crc32=0U;response.crc32=update::crc32(&response,60U);response_pending=true;
}

bool erase_image(std::uint8_t slot,std::uint32_t size){const auto base=update::slot_address(slot);const auto end=(size+update::sector_size-1U)&~(update::sector_size-1U);for(std::uint32_t o=0U;o<end;o+=update::sector_size)if(!boot::flash::erase_sector(base+o))return false;return true;}

void handle(const boot::protocol::Report& r){
    auto copy=r;const auto crc=copy.crc32;copy.crc32=0U;
    if(r.magic!=boot::protocol::magic||update::crc32(&copy,60U)!=crc){reply(static_cast<boot::protocol::Command>(r.command),r.sequence,boot::protocol::Status::BadPacket);return;}
    const auto cmd=static_cast<boot::protocol::Command>(r.command);
    if(cmd==boot::protocol::Command::Info){reply(cmd,r.sequence,boot::protocol::Status::Ok,receive_next);return;}
    if(cmd==boot::protocol::Command::Abort){receiving=false;reply(cmd,r.sequence,boot::protocol::Status::Ok);return;}
    if(cmd==boot::protocol::Command::Reboot){reply(cmd,r.sequence,boot::protocol::Status::Ok);NVIC_SystemReset();}
    if(cmd==boot::protocol::Command::Begin){
        std::uint32_t size=0U;std::memcpy(&size,r.data,4);const std::uint8_t slot=state.active_slot==0U?1U:0U;
        if(size<sizeof(ImageHeader)||size>update::slot_size){reply(cmd,r.sequence,boot::protocol::Status::Range);return;}
        receiving=false;if(!erase_image(slot,size)){reply(cmd,r.sequence,boot::protocol::Status::Flash);return;}
        receive_slot=slot;receive_size=size;receive_next=0U;receiving=true;reply(cmd,r.sequence,boot::protocol::Status::Ok);return;
    }
    if(cmd==boot::protocol::Command::Data){
        if(!receiving||r.offset!=receive_next||r.length==0U||r.length>sizeof(r.data)||r.offset+r.length>receive_size){reply(cmd,r.sequence,boot::protocol::Status::BadState,receive_next);return;}
        if(!boot::flash::program(update::slot_address(receive_slot)+r.offset,r.data,r.length)){reply(cmd,r.sequence,boot::protocol::Status::Flash,receive_next);return;}
        receive_next+=r.length;reply(cmd,r.sequence,boot::protocol::Status::Ok,receive_next);return;
    }
    if(cmd==boot::protocol::Command::End){
        if(!receiving||receive_next!=receive_size||!valid_header(receive_slot,true)){reply(cmd,r.sequence,boot::protocol::Status::Verify,receive_next);return;}
        const auto& h=*reinterpret_cast<const ImageHeader*>(update::slot_address(receive_slot));state.pending_slot=receive_slot;state.pending_version=h.image_version;state.trial_attempts=0U;
        if(!save_state()){reply(cmd,r.sequence,boot::protocol::Status::Flash);return;}receiving=false;reply(cmd,r.sequence,boot::protocol::Status::Ok,receive_next);return;
    }
    reply(cmd,r.sequence,boot::protocol::Status::BadPacket);
}
}

extern "C" void boot_default_handler(){for(;;)__asm volatile("wfi");}
extern "C" void USB_OTG1_IRQHandler(){tusb_int_handler(0U,true);}
extern "C" std::uint16_t tud_hid_get_report_cb(std::uint8_t,std::uint8_t,hid_report_type_t,std::uint8_t* buffer,std::uint16_t requested){const auto n=requested<sizeof(response)?requested:sizeof(response);std::memcpy(buffer,&response,n);return static_cast<std::uint16_t>(n);}
extern "C" void tud_hid_set_report_cb(std::uint8_t,std::uint8_t,hid_report_type_t type,const std::uint8_t* buffer,std::uint16_t size){if(type!=HID_REPORT_TYPE_OUTPUT||size!=64U)return;boot::protocol::Report r;std::memcpy(&r,buffer,sizeof(r));handle(r);}

extern "C" [[noreturn]] void boot_reset(){
    SystemInit();SCB->VTOR=reinterpret_cast<std::uint32_t>(&__ram_vectors);platform::nxp::configure_mpu();BOARD_InitBootClocks();
    choose_image();
    // Reaching here means update mode was requested or neither application
    // validates. Flash IP-command support is optional for enumeration: a NOR
    // mismatch must be reported over HID, not turn normal boot into a brick.
    ensure_flash();
    CLOCK_EnableUsbhs0PhyPllClock(kCLOCK_Usbphy480M,24'000'000U);CLOCK_EnableUsbhs0Clock(kCLOCK_Usb480M,24'000'000U);
    USBPHY->CTRL|=USBPHY_CTRL_SET_ENUTMILEVEL2_MASK|USBPHY_CTRL_SET_ENUTMILEVEL3_MASK;USBPHY->PWD=0U;
    tusb_rhport_init_t init{TUSB_ROLE_DEVICE,TUSB_SPEED_HIGH};tusb_init(0U,&init);
    for(;;){tud_task();if(response_pending&&tud_hid_ready()&&tud_hid_report(0U,&response,sizeof(response)))response_pending=false;__asm volatile("wfi");}
}
