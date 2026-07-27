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

constexpr std::size_t pcm_bytes_per_frame = 8U;
constexpr std::size_t fade_frames = 32U;

void scale_frame(std::uint8_t* frame, std::uint16_t gain) {
    for (std::size_t channel = 0U; channel < 2U; ++channel) {
        auto* sample_bytes = frame + channel * 4U;
        std::int32_t sample = static_cast<std::int32_t>(sample_bytes[1]) |
                              (static_cast<std::int32_t>(sample_bytes[2]) << 8U) |
                              (static_cast<std::int32_t>(sample_bytes[3]) << 16U);
        if ((sample & 0x00800000L) != 0) sample |= static_cast<std::int32_t>(0xFF000000UL);
        const auto scaled = static_cast<std::int32_t>(
            (static_cast<std::int64_t>(sample) * gain) / 32767);
        sample_bytes[1] = static_cast<std::uint8_t>(scaled);
        sample_bytes[2] = static_cast<std::uint8_t>(scaled >> 8U);
        sample_bytes[3] = static_cast<std::uint8_t>(scaled >> 16U);
    }
}

void fade(std::uint8_t* data, std::size_t first_frame, std::size_t frames, bool fade_in) {
    if (frames == 0U) return;
    for (std::size_t index = 0U; index < frames; ++index) {
        const auto numerator = fade_in ? index : frames - index - 1U;
        const auto denominator = frames > 1U ? frames - 1U : 1U;
        const auto gain = static_cast<std::uint16_t>((numerator * 32767U) / denominator);
        scale_frame(data + (first_frame + index) * pcm_bytes_per_frame, gain);
    }
}

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
        if (read != transfer_bytes_) {
            ++underruns_;
            conceal_underrun(buffer, read);
        } else if (concealment_active_) {
            fade(buffer, 0U, fade_frames < transfer_bytes_ / pcm_bytes_per_frame ? fade_frames : transfer_bytes_ / pcm_bytes_per_frame, true);
            concealment_active_ = false;
        }
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
    concealment_active_ = false;
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

void AudioEngine::conceal_underrun(std::uint8_t* buffer, std::size_t received_bytes) noexcept {
    // Do not splice a late partial fragment into a silence gap.  Wait for a
    // complete DMA block, then fade it in.  On the first short read, fade the
    // end of valid PCM to digital zero rather than creating a step at the DAC.
    if (concealment_active_) {
        std::memset(buffer, 0, transfer_bytes_);
        return;
    }
    const std::size_t received_frames = received_bytes / pcm_bytes_per_frame;
    const std::size_t count = fade_frames < received_frames ? fade_frames : received_frames;
    fade(buffer, received_frames - count, count, false);
    concealment_active_ = true;
}

void AudioEngine::stop() noexcept {
    if (!running_) return;
    running_ = false;
    SAI_TransferAbortSendEDMA(SAI1, &sai_handle);
    SAI_Deinit(SAI1);
    DMAMUX_DisableChannel(DMAMUX, dma_channel);
}

void AudioEngine::capture_dma_diagnostics(std::uint32_t now_ms) noexcept {
    const auto& tcd = DMA0->TCD[dma_channel];
    dma_diagnostics_.captured_ms = now_ms;
    dma_diagnostics_.dmamux_chcfg = DMAMUX->CHCFG[dma_channel];
    dma_diagnostics_.dma_es = DMA0->ES;
    dma_diagnostics_.tcd_saddr = tcd.SADDR;
    dma_diagnostics_.tcd_daddr = tcd.DADDR;
    dma_diagnostics_.tcd_nbytes = tcd.NBYTES_MLNO;
    dma_diagnostics_.sai_tcsr = SAI1->TCSR;
    dma_diagnostics_.sai_tcr1 = SAI1->TCR1;
    dma_diagnostics_.sai_tcr2 = SAI1->TCR2;
    dma_diagnostics_.sai_tcr3 = SAI1->TCR3;
    dma_diagnostics_.sai_tfr0 = SAI1->TFR[0];
    dma_diagnostics_.dma_erq = static_cast<std::uint16_t>(DMA0->ERQ);
    dma_diagnostics_.dma_int = static_cast<std::uint16_t>(DMA0->INT);
    dma_diagnostics_.dma_hrs = static_cast<std::uint16_t>(DMA0->HRS);
    dma_diagnostics_.tcd_citer = tcd.CITER_ELINKNO;
    dma_diagnostics_.tcd_biter = tcd.BITER_ELINKNO;
    dma_diagnostics_.tcd_csr = tcd.CSR;
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
