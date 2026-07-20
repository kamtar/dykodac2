#pragma once

#include "audio/audio_format.hpp"

namespace board {
class ClockManager {
public:
    void initialize_safe() noexcept;
    void select(audio::ClockFamily family) noexcept;
    void mark_stable() noexcept { stable_ = true; }
    void invalidate() noexcept { stable_ = false; }
    audio::ClockFamily selected() const noexcept { return selected_; }
    bool stable() const noexcept { return stable_; }
private:
    audio::ClockFamily selected_{audio::ClockFamily::Family44k1};
    bool stable_{false};
};
} // namespace board

