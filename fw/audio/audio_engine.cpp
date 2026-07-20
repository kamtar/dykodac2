#include "audio/audio_engine.hpp"

#include "board/target_board.hpp"
#include <cstring>

extern "C" {
#include "fsl_dmamux.h"
#include "fsl_edma.h"
#include "fsl_iomuxc.h"
#include "fsl_sai.h"
#include "fsl_sai_edma.h"
}

namespace {
constexpr std::uint32_t dma_channel = 0U;
__attribute__((section(".dma"), aligned(32))) std::uint8_t dma_audio[2][384]{};
std::uint8_t dma_index = 0U;
edma_handle_t dma_handle{};
sai_edma_handle_t sai_handle{};

void dma_callback(I2S_Type*, sai_edma_handle_t*, status_t status, void* user_data) {
    const bool completed = status == kStatus_SAI_TxBusy || status == kStatus_SAI_TxIdle;
    static_cast<audio::AudioEngine*>(user_data)->on_dma_complete(completed);
}
}

namespace audio {
bool AudioEngine::submit_silence() noexcept {
    auto* buffer = dma_audio[dma_index];
    dma_index ^= 1U;
    if (stream_) {
        const std::size_t read = stream_->pop(buffer, transfer_bytes_);
        if (read != transfer_bytes_) ++underruns_;
    } else {
        std::memset(buffer, 0, transfer_bytes_);
    }
    sai_transfer_t transfer{buffer, transfer_bytes_};
    return SAI_TransferSendEDMA(SAI1, &sai_handle, &transfer) == kStatus_Success;
}

bool AudioEngine::start_silence(const FormatConfig& config) noexcept {
    static StreamBuffer empty;
    empty.clear();
    const bool result = start(config, empty);
    stream_ = nullptr;
    underruns_ = 0U;
    return result;
}

bool AudioEngine::start(const FormatConfig& config, StreamBuffer& stream) noexcept {
    if (config.format.channels != 2U || config.format.container_bits != 32U ||
        config.format.valid_bits != 24U) return false;

    stream_ = &stream;
    transfer_bytes_ = static_cast<std::size_t>(config.dma_frames_per_transfer) * bytes_per_frame(config.format);
    if (transfer_bytes_ > sizeof(dma_audio[0])) return false;
    board::target::initialize_audio_pins();
    IOMUXC_SetSaiMClkClockSource(IOMUXC_GPR, kIOMUXC_GPR_SAI1MClk1Sel, 3U);

    // Match the hardware-verified prototype sequence: initialize SAI before
    // the DMA routing and keep the channel at the adapter's bare-metal IRQ
    // priority.
    SAI_Init(SAI1);
    DMAMUX_Init(DMAMUX);
    DMAMUX_SetSource(DMAMUX, dma_channel, kDmaRequestMuxSai1Tx);
    DMAMUX_EnableChannel(DMAMUX, dma_channel);
    edma_config_t dma_config{};
    EDMA_GetDefaultConfig(&dma_config);
    EDMA_Init(DMA0, &dma_config);
    NVIC_SetPriority(DMA0_IRQn, 25U);
    EDMA_CreateHandle(&dma_handle, DMA0, dma_channel);

    SAI_TransferTxCreateHandleEDMA(SAI1, &sai_handle, dma_callback, this, &dma_handle);
    sai_transceiver_t sai_config{};
    SAI_GetClassicI2SConfig(&sai_config, kSAI_WordWidth32bits, kSAI_Stereo, kSAI_Channel0Mask);
    sai_config.masterSlave = kSAI_Master;
    sai_config.syncMode = kSAI_ModeAsync;
    sai_config.bitClock.bclkSource = kSAI_BclkSourceMclkDiv;
    sai_config.fifo.fifoWatermark = static_cast<std::uint8_t>(FSL_FEATURE_SAI_FIFO_COUNT - 1U);
    SAI_TransferTxSetConfigEDMA(SAI1, &sai_handle, &sai_config);
    SAI_TxSetBitClockRate(SAI1, config.oscillator_hz, config.format.sample_rate_hz,
                          config.format.container_bits, config.format.channels);

    dma_completions_ = 0U;
    dma_errors_ = 0U;
    underruns_ = 0U;
    // The prototype keeps one transfer outstanding and queues its successor
    // from the completion callback. Submitting two here enables dynamic
    // scatter/gather, whose next TCD lives in cacheable OCRAM.
    running_ = true;
    if (!submit_silence()) running_ = false;
    return running_;
}

void AudioEngine::stop() noexcept {
    if (!running_) return;
    running_ = false;
    SAI_TransferAbortSendEDMA(SAI1, &sai_handle);
    SAI_Deinit(SAI1);
    DMAMUX_DisableChannel(DMAMUX, dma_channel);
}

void AudioEngine::on_dma_complete(bool success) noexcept {
    // Abort can race with an already-pending completion interrupt. Once stop
    // publishes running_=false, that stale callback must not refill/requeue.
    if (!running_) return;
    if (!success) {
        ++dma_errors_;
        running_ = false;
        return;
    }
    ++dma_completions_;
    if (!submit_silence()) {
        ++dma_errors_;
        running_ = false;
    }
}
} // namespace audio
