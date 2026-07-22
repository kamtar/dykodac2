#include "boot/flexspi_flash.hpp"
#include "platform/nxp/update_layout.hpp"
#include <cstring>

extern "C" {
#include "MIMXRT1011.h"
#include "evkmimxrt1010_flexspi_nor_config.h"
}

namespace boot::flash {
namespace {
enum : std::uint32_t { ReadStatus=1U, WriteEnable=2U, EraseSector=3U, PageProgram=4U, ReadJedec=5U };
constexpr std::uint32_t lut_key=0x5AF05AF0U;
constexpr std::uint32_t errors=FLEXSPI_INTR_IPCMDGE_MASK|FLEXSPI_INTR_IPCMDERR_MASK|FLEXSPI_INTR_SEQTIMEOUT_MASK;

void update_lut(std::uint32_t sequence, std::uint32_t a, std::uint32_t b=0U) {
    FLEXSPI->LUTKEY=lut_key;FLEXSPI->LUTCR=2U;
    FLEXSPI->LUT[sequence*4U]=a;FLEXSPI->LUT[sequence*4U+1U]=b;
    FLEXSPI->LUTKEY=lut_key;FLEXSPI->LUTCR=1U;
}

bool command(std::uint32_t sequence,std::uint32_t address,std::size_t size) {
    FLEXSPI->INTR=0xFFFFFFFFU;
    FLEXSPI->IPCR0=address;
    FLEXSPI->IPCR1=FLEXSPI_IPCR1_IDATSZ(static_cast<std::uint32_t>(size))|FLEXSPI_IPCR1_ISEQID(sequence);
    FLEXSPI->IPCMD=FLEXSPI_IPCMD_TRG_MASK;
    std::uint32_t timeout=20'000'000U;
    while((FLEXSPI->INTR&FLEXSPI_INTR_IPCMDDONE_MASK)==0U && --timeout!=0U){}
    const auto status=FLEXSPI->INTR;FLEXSPI->INTR=status;
    return timeout!=0U && (status&errors)==0U;
}

bool write_enable(std::uint32_t address){return command(WriteEnable,address,0U);}

std::uint8_t status() {
    FLEXSPI->IPRXFCR|=FLEXSPI_IPRXFCR_CLRIPRXF_MASK;
    if(!command(ReadStatus,platform::nxp::update::flash_base,1U))return 0xFFU;
    return static_cast<std::uint8_t>(FLEXSPI->RFDR[0]);
}

bool wait_ready(){std::uint32_t n=20'000'000U;while(--n!=0U){if((status()&1U)==0U)return true;}return false;}
}

bool initialize() noexcept {
    update_lut(ReadStatus,FLEXSPI_LUT_SEQ(CMD_SDR,FLEXSPI_1PAD,0x05U,READ_SDR,FLEXSPI_1PAD,1U));
    update_lut(WriteEnable,FLEXSPI_LUT_SEQ(CMD_SDR,FLEXSPI_1PAD,0x06U,STOP,0U,0U));
    update_lut(EraseSector,FLEXSPI_LUT_SEQ(CMD_SDR,FLEXSPI_1PAD,0x20U,RADDR_SDR,FLEXSPI_1PAD,24U));
    update_lut(PageProgram,FLEXSPI_LUT_SEQ(CMD_SDR,FLEXSPI_1PAD,0x02U,RADDR_SDR,FLEXSPI_1PAD,24U),
               FLEXSPI_LUT_SEQ(WRITE_SDR,FLEXSPI_1PAD,4U,STOP,0U,0U));
    update_lut(ReadJedec,FLEXSPI_LUT_SEQ(CMD_SDR,FLEXSPI_1PAD,0x9FU,READ_SDR,FLEXSPI_1PAD,3U));
    return jedec_id()!=0U && jedec_id()!=0xFFFFFFU;
}

std::uint32_t jedec_id() noexcept {
    FLEXSPI->IPRXFCR|=FLEXSPI_IPRXFCR_CLRIPRXF_MASK;
    if(!command(ReadJedec,platform::nxp::update::flash_base,3U))return 0U;
    return FLEXSPI->RFDR[0]&0xFFFFFFU;
}

void invalidate_ahb() noexcept {
    SCB_CleanInvalidateDCache();
    FLEXSPI->MCR0|=FLEXSPI_MCR0_SWRESET_MASK;
    while((FLEXSPI->MCR0&FLEXSPI_MCR0_SWRESET_MASK)!=0U){}
    SCB_InvalidateICache();__DSB();__ISB();
}

bool erase_sector(std::uint32_t address) noexcept {
    if((address&(platform::nxp::update::sector_size-1U))!=0U)return false;
    if(!write_enable(address)||!command(EraseSector,address,0U)||!wait_ready())return false;
    invalidate_ahb();return erased(address,platform::nxp::update::sector_size);
}

bool program(std::uint32_t address,const void* source,std::size_t size) noexcept {
    if(size==0U||size>128U||
       ((address&255U)+size)>platform::nxp::update::page_size)return false;
    if(!write_enable(address))return false;
    const auto* p=static_cast<const std::uint8_t*>(source);
    FLEXSPI->IPTXFCR|=FLEXSPI_IPTXFCR_CLRIPTXF_MASK;
    for(std::size_t i=0;i<size;i+=4U){std::uint32_t word=0xFFFFFFFFU;const auto n=(size-i)<4U?(size-i):4U;std::memcpy(&word,p+i,n);FLEXSPI->TFDR[i/4U]=word;}
    if(!command(PageProgram,address,size)||!wait_ready())return false;
    invalidate_ahb();return std::memcmp(reinterpret_cast<const void*>(address),source,size)==0;
}

bool erased(std::uint32_t address,std::size_t size) noexcept {
    const auto* p=reinterpret_cast<const std::uint8_t*>(address);for(std::size_t i=0;i<size;++i)if(p[i]!=0xFFU)return false;return true;
}
} // namespace boot::flash
