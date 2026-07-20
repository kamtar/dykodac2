#pragma once

#include "audio/audio_format.hpp"
#include "audio/stream_buffer.hpp"

namespace audio {
class AudioEngine {
public:
    bool start_silence(const FormatConfig& config) noexcept;
    bool start(const FormatConfig& config, StreamBuffer& stream) noexcept;
    void stop() noexcept;
    void attach_stream(StreamBuffer& stream) noexcept { stream_ = &stream; }
    bool running() const noexcept { return running_; }
    std::uint32_t dma_completions() const noexcept { return dma_completions_; }
    std::uint32_t dma_errors() const noexcept { return dma_errors_; }
    std::uint32_t underruns() const noexcept { return underruns_; }
    void on_dma_complete(bool success) noexcept;
private:
    bool submit_silence() noexcept;
    volatile std::uint32_t dma_completions_{0U};
    volatile std::uint32_t dma_errors_{0U};
    volatile std::uint32_t underruns_{0U};
    volatile bool running_{false};
    StreamBuffer* stream_{nullptr};
    std::size_t transfer_bytes_{0U};
};
} // namespace audio
