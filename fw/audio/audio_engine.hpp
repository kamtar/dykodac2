#pragma once

#include "audio/audio_format.hpp"
#include "audio/polyphase_upsampler.hpp"
#include "audio/stream_buffer.hpp"
#include "audio/volume.hpp"

namespace audio {
struct DmaDiagnosticSnapshot {
    std::uint32_t captured_ms{0U};
    std::uint32_t dmamux_chcfg{0U};
    std::uint32_t dma_es{0U};
    std::uint32_t tcd_saddr{0U};
    std::uint32_t tcd_daddr{0U};
    std::uint32_t tcd_nbytes{0U};
    std::uint32_t sai_tcsr{0U};
    std::uint32_t sai_tcr1{0U};
    std::uint32_t sai_tcr2{0U};
    std::uint32_t sai_tcr3{0U};
    std::uint32_t sai_tfr0{0U};
    std::uint16_t dma_erq{0U};
    std::uint16_t dma_int{0U};
    std::uint16_t dma_hrs{0U};
    std::uint16_t tcd_citer{0U};
    std::uint16_t tcd_biter{0U};
    std::uint16_t tcd_csr{0U};
};

struct RefillTimingSnapshot {
    std::uint32_t count{0U};
    std::uint64_t total_cycles{0U};
    std::uint32_t maximum_cycles{0U};
    std::uint32_t last_cycles{0U};
    volatile std::uint8_t maximum_pending{0U};
    volatile std::uint32_t coalesced_callbacks{0U};
    volatile std::uint32_t queue_drains{0U};
};

class AudioEngine {
public:
    static constexpr std::uint8_t dma_buffer_count = 4U;
    // Power-on host volume; matches usb::UsbDevice's initial control state.
    // main() also applies the USB control value explicitly before streaming.
    static constexpr std::int16_t default_volume_db_8_8 = 0;
    AudioEngine() noexcept { set_volume_db(default_volume_db_8_8); }
    // Host volume in UAC2 1/256 dB units, clamped to [-127 dB, 0 dB] and
    // applied as an MCU gain with fixed headroom (see config/board_config.hpp).
    // Must be called from main context: process() runs only from service()
    // and start(), which are main-context too, so no hand-off is needed.
    void set_volume_db(std::int16_t db_8_8) noexcept {
        volume_db_8_8_ = clamp_volume_db_8_8(db_8_8);
        upsampler_.set_gain_target(mcu_gain_from_uac2_db(volume_db_8_8_));
    }
    std::int16_t volume_db() const noexcept { return volume_db_8_8_; }
    bool start_silence(const FormatConfig& config) noexcept;
    bool start(const FormatConfig& config, StreamBuffer& stream) noexcept;
    void service() noexcept;
    void stop() noexcept;
    void attach_stream(StreamBuffer& stream) noexcept { stream_ = &stream; }
    void detach_stream() noexcept { stream_ = nullptr; }
    bool running() const noexcept { return running_; }
    std::uint32_t dma_completions() const noexcept { return dma_completions_; }
    std::uint32_t dma_errors() const noexcept { return dma_errors_; }
    std::uint32_t underruns() const noexcept { return underruns_; }
    void capture_dma_diagnostics(std::uint32_t now_ms) noexcept;
    const DmaDiagnosticSnapshot& dma_diagnostics() const noexcept { return dma_diagnostics_; }
    const RefillTimingSnapshot& refill_timing() const noexcept { return refill_timing_; }
    void on_dma_complete(bool success, bool queue_empty, std::uint8_t completed,
                         std::uint8_t observed) noexcept;
private:
    std::uint8_t* prepare_next_buffer() noexcept;
    std::uint8_t* timed_prepare_next_buffer() noexcept;
    bool take_refill_request() noexcept;
    void reset_refill_timing() noexcept;
    void record_refill_cycles(std::uint32_t cycles) noexcept;
    void conceal_underrun(std::uint8_t* buffer, std::size_t received_bytes) noexcept;
    volatile std::uint32_t dma_completions_{0U};
    volatile std::uint32_t dma_errors_{0U};
    volatile std::uint32_t underruns_{0U};
    volatile bool running_{false};
    volatile std::uint8_t refill_requests_{0U};
    StreamBuffer* stream_{nullptr};
    std::size_t transfer_bytes_{0U};
    std::size_t transfer_frames_{0U};
    PolyphaseUpsampler upsampler_{};
    std::int16_t volume_db_8_8_{default_volume_db_8_8};
    bool concealment_active_{false};
    DmaDiagnosticSnapshot dma_diagnostics_{};
    RefillTimingSnapshot refill_timing_{};
};
} // namespace audio
