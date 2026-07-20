#include "platform/nxp/flexio_dac_transport.hpp"

#include "board/target_board.hpp"

extern "C" {
#include "fsl_clock.h"
#include "fsl_flexio_spi.h"
}

namespace {
constexpr std::uint32_t flexio_mux = 3U;
constexpr std::uint32_t flexio_predivider = 4U;
constexpr std::uint32_t flexio_divider = 7U;

FLEXIO_SPI_Type spi{
    FLEXIO1, 2U, 1U, 3U, 4U, {0U, 1U}, {0U, 1U}
};
}

namespace platform::nxp {
void FlexioDacTransport::initialize() noexcept {
    CLOCK_SetMux(kCLOCK_Flexio1Mux, flexio_mux);
    CLOCK_SetDiv(kCLOCK_Flexio1PreDiv, flexio_predivider);
    CLOCK_SetDiv(kCLOCK_Flexio1Div, flexio_divider);
    flexio_spi_master_config_t config{};
    config.enableMaster = true;
    config.enableInDoze = false;
    config.enableInDebug = true;
    config.enableFastAccess = false;
    config.baudRate_Bps = 2'000'000U;
    config.phase = kFLEXIO_SPI_ClockPhaseFirstEdge;
    config.dataMode = kFLEXIO_SPI_8BitMode;
    const std::uint32_t source_hz = CLOCK_GetFreq(kCLOCK_Usb1PllClk) /
        (flexio_predivider + 1U) / (flexio_divider + 1U);
    FLEXIO_SPI_MasterInit(&spi, &config, source_hz);
    FLEXIO_SPI_Enable(&spi, true);
}

void FlexioDacTransport::set_reset(bool asserted) noexcept { board::target::set_dac_reset(asserted); }
void FlexioDacTransport::set_chip_select(bool asserted) noexcept { board::target::set_dac_chip_select(asserted); }

bool FlexioDacTransport::transfer(const devices::DacCommand& command) noexcept {
    flexio_spi_transfer_t transfer{};
    transfer.txData = const_cast<std::uint8_t*>(command.bytes.data());
    transfer.rxData = nullptr;
    transfer.dataSize = command.bytes.size();
    transfer.flags = kFLEXIO_SPI_8bitMsb;
    return FLEXIO_SPI_MasterTransferBlocking(&spi, &transfer) == kStatus_Success;
}
} // namespace platform::nxp
