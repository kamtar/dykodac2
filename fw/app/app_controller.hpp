#pragma once

#include <cstdint>

namespace app {
enum class State : std::uint8_t { BootSafe, Initializing, IdleMuted, PreparingStream, Playing, SwitchingRate, Stopping, FaultMuted };
enum class Event : std::uint8_t { Initialize, Initialized, StreamRequested, AudioStable, RateRequested, StopRequested, Stopped, Fault, ClearFault, Detach, Suspend, DmaFailure, ClockFailure, RepeatedUnderrun };

struct SafetyOutputs {
    bool relay_connected;
    bool dac_muted;
    bool clock_valid;
    bool audio_running;
};

class AppController {
public:
    State state() const noexcept { return state_; }
    SafetyOutputs outputs() const noexcept { return outputs_; }
    bool dispatch(Event event) noexcept;
private:
    void enter_muted(State state) noexcept;
    State state_{State::BootSafe};
    SafetyOutputs outputs_{false, true, false, false};
};
} // namespace app
