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
static_assert(audio::AudioEngine::dma_buffer_count == SAI_XFER_QUEUE_SIZE,
              "audio buffer count must fit the SAI loop TCD pool");
// DMA completion must be able to preempt USB. The four-entry circular TCD ring
// keeps running independently while main context regenerates only retired
// buffers; active buffers must never be rewritten to "catch up".
constexpr std::uint32_t dma_irq_priority = 1U;
// Four buffers keep 4 ms of output queued. They live in cacheable OCRAM so the
// fixed DTCM coefficient bank and 8 KiB stack remain untouched; each completed
// buffer is explicitly cleaned before eDMA reads it.
alignas(32) std::uint8_t dma_audio[audio::AudioEngine::dma_buffer_count][1536]{};
// Exact 4x conversion consumes 44 or 48 source frames per approximately
// one-millisecond output transfer. Long FIR history lives inside the FFT FDL.
constexpr std::size_t maximum_upsampler_input_frames = 48U;
__attribute__((section(".dma"), aligned(32)))
std::uint8_t upsampler_input[maximum_upsampler_input_frames * 8U]{};
std::uint8_t dma_index = 0U;
edma_handle_t dma_handle{};
// The SAI loop driver uses this handle's embedded TCD pool for its permanent
// scatter/gather ring. Keep it in DMA-visible, non-cacheable DTCM.
__attribute__((section(".dma"), aligned(32))) sai_edma_handle_t sai_handle{};

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

void dma_callback(I2S_Type*, sai_edma_handle_t* handle, status_t status, void* user_data) {
    const bool completed = status == kStatus_SAI_TxBusy || status == kStatus_SAI_TxIdle;
    // A permanent ring releases one software slot per IRQ. Keep the SDK's raw
    // queue estimate only for diagnostics; it must not cause burst refills.
    static_cast<audio::AudioEngine*>(user_data)->on_dma_complete(
        completed, status == kStatus_SAI_TxIdle, 1U, handle->completedTransferCount);
}
}

namespace audio {
void AudioEngine::reset_refill_timing() noexcept {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    refill_timing_ = {};
}

void AudioEngine::record_refill_cycles(std::uint32_t cycles) noexcept {
    ++refill_timing_.count;
    refill_timing_.total_cycles += cycles;
    refill_timing_.last_cycles = cycles;
    if (cycles > refill_timing_.maximum_cycles) refill_timing_.maximum_cycles = cycles;
}

std::uint8_t* AudioEngine::prepare_next_buffer() noexcept {
    auto* buffer = dma_audio[dma_index];
    dma_index = static_cast<std::uint8_t>(
        (dma_index + 1U) & (audio::AudioEngine::dma_buffer_count - 1U));
    if (stream_) {
        const std::size_t input_frames = upsampler_.input_frames_needed(transfer_frames_);
        const std::size_t input_bytes = input_frames * pcm_bytes_per_frame;
        const std::size_t read = stream_->pop(upsampler_input, input_bytes);
        const std::size_t produced_input = read / pcm_bytes_per_frame;
        const std::size_t produced = upsampler_.process(upsampler_input, produced_input,
                                                        buffer, transfer_frames_);
        if (produced < transfer_frames_)
            std::memset(buffer + produced * pcm_bytes_per_frame, 0,
                        (transfer_frames_ - produced) * pcm_bytes_per_frame);
        if (read != input_bytes || produced != transfer_frames_) {
            ++underruns_;
            conceal_underrun(buffer, produced * pcm_bytes_per_frame);
        } else if (concealment_active_) {
            fade(buffer, 0U, fade_frames < transfer_frames_ ? fade_frames : transfer_frames_, true);
            concealment_active_ = false;
        }
    } else {
        const std::size_t input_frames = upsampler_.input_frames_needed(transfer_frames_);
        std::memset(upsampler_input, 0, input_frames * pcm_bytes_per_frame);
        const std::size_t produced = upsampler_.process(upsampler_input, input_frames,
                                                        buffer, transfer_frames_);
        if (produced != transfer_frames_)
            std::memset(buffer + produced * pcm_bytes_per_frame, 0,
                        (transfer_frames_ - produced) * pcm_bytes_per_frame);
    }
    SCB_CleanDCache_by_Addr(reinterpret_cast<std::uint32_t*>(buffer),
                            static_cast<std::int32_t>(transfer_bytes_));
    return buffer;
}

std::uint8_t* AudioEngine::timed_prepare_next_buffer() noexcept {
    const std::uint32_t started = DWT->CYCCNT;
    auto* const result = prepare_next_buffer();
    record_refill_cycles(DWT->CYCCNT - started);
    return result;
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
    transfer_frames_ = config.dma_frames_per_transfer;
    if (!upsampler_.configure(config.format.sample_rate_hz, config.output_sample_rate_hz)) return false;
    // Coefficients depend on the source rate; the boost (and so the gain)
    // is settled before the first buffer is generated.
    redesign();
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
    NVIC_SetPriority(DMA0_IRQn, dma_irq_priority);
    EDMA_CreateHandle(&dma_handle, DMA0, dma_channel);

    SAI_TransferTxCreateHandleEDMA(SAI1, &sai_handle, dma_callback, this, &dma_handle);
    sai_transceiver_t sai_config{};
    SAI_GetClassicI2SConfig(&sai_config, kSAI_WordWidth32bits, kSAI_Stereo, kSAI_Channel0Mask);
    sai_config.masterSlave = kSAI_Master;
    sai_config.syncMode = kSAI_ModeAsync;
    sai_config.bitClock.bclkSource = kSAI_BclkSourceMclkDiv;
    sai_config.fifo.fifoWatermark = static_cast<std::uint8_t>(FSL_FEATURE_SAI_FIFO_COUNT - 1U);
    SAI_TransferTxSetConfigEDMA(SAI1, &sai_handle, &sai_config);
    SAI_TxSetBitClockRate(SAI1, config.output_oscillator_hz, config.output_sample_rate_hz,
                          config.format.container_bits, config.format.channels);

    dma_completions_ = 0U;
    dma_errors_ = 0U;
    underruns_ = 0U;
    reset_refill_timing();
    // Build a permanent four-entry hardware TCD ring. Unlike the transactional
    // queue, it cannot stop between a completion IRQ and its software refill.
    refill_requests_ = 0U;
    dma_index = 0U;
    sai_transfer_t loop_transfers[AudioEngine::dma_buffer_count]{};
    for (std::size_t index = 0U; index < AudioEngine::dma_buffer_count; ++index) {
        loop_transfers[index].data = timed_prepare_next_buffer();
        loop_transfers[index].dataSize = transfer_bytes_;
    }
    running_ = true;
    if (SAI_TransferSendLoopEDMA(SAI1, &sai_handle, loop_transfers,
                                 AudioEngine::dma_buffer_count) != kStatus_Success) {
        stop();
        return false;
    }
    return true;
}

bool AudioEngine::take_refill_request() noexcept {
    const std::uint32_t primask = __get_PRIMASK();
    __disable_irq();
    __DMB();
    const bool pending = refill_requests_ != 0U;
    if (pending) --refill_requests_;
    __DMB();
    if (primask == 0U) __enable_irq();
    return pending;
}

void AudioEngine::service() noexcept {
    while (running_ && take_refill_request()) {
        (void)timed_prepare_next_buffer();
        if (loudness_delay_refills_ != 0U && --loudness_delay_refills_ == 0U) update_loudness();
    }
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
    if (loudness_delay_refills_ != 0U) {
        loudness_delay_refills_ = 0U;
        update_loudness();
    }
    refill_requests_ = 0U;
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

void AudioEngine::on_dma_complete(bool success, bool queue_empty, std::uint8_t completed,
                                  std::uint8_t observed) noexcept {
    // Abort can race with an already-pending completion interrupt. Once stop
    // publishes running_=false, that stale callback must not refill/requeue.
    if (!running_) return;
    if (!success) {
        ++dma_errors_;
        // Leave running_ set until the main state machine observes the error
        // and calls stop(), which aborts and deinitializes the active path.
        return;
    }
    if (completed == 0U) {
        ++dma_errors_;
        return;
    }
    if (observed > 1U) ++refill_timing_.coalesced_callbacks;
    if (queue_empty) ++refill_timing_.queue_drains;
    dma_completions_ += completed;
    // FIR generation is intentionally deferred to main context. The circular
    // DMA ring continues feeding SAI while retired blocks are regenerated.
    const std::uint16_t pending = static_cast<std::uint16_t>(refill_requests_) + completed;
    refill_requests_ = static_cast<std::uint8_t>(pending > 0xFFU ? 0xFFU : pending);
    if (refill_requests_ > refill_timing_.maximum_pending)
        refill_timing_.maximum_pending = refill_requests_;
}
} // namespace audio
