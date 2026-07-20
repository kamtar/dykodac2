#include "app/app_controller.hpp"

namespace app {
void AppController::enter_muted(State state) noexcept {
    state_ = state;
    outputs_ = {false, true, false, false};
}

bool AppController::dispatch(Event event) noexcept {
    if (event == Event::Fault || event == Event::DmaFailure || event == Event::ClockFailure ||
        event == Event::RepeatedUnderrun) { enter_muted(State::FaultMuted); return true; }
    if (event == Event::Detach || event == Event::Suspend) { enter_muted(State::IdleMuted); return true; }
    switch (state_) {
    case State::BootSafe:
        if (event == Event::Initialize) { enter_muted(State::Initializing); return true; }
        break;
    case State::Initializing:
        if (event == Event::Initialized) { enter_muted(State::IdleMuted); return true; }
        break;
    case State::IdleMuted:
        if (event == Event::StreamRequested) {
            state_ = State::PreparingStream;
            outputs_ = {false, true, false, true};
            return true;
        }
        break;
    case State::PreparingStream:
        if (event == Event::AudioStable) {
            state_ = State::Playing;
            outputs_ = {true, false, true, true};
            return true;
        }
        if (event == Event::StopRequested) { enter_muted(State::Stopping); return true; }
        break;
    case State::Playing:
        if (event == Event::RateRequested) { enter_muted(State::SwitchingRate); return true; }
        if (event == Event::StopRequested) { enter_muted(State::Stopping); return true; }
        break;
    case State::SwitchingRate:
        if (event == Event::StreamRequested) {
            state_ = State::PreparingStream;
            outputs_.audio_running = true;
            return true;
        }
        break;
    case State::Stopping:
        if (event == Event::Stopped) { enter_muted(State::IdleMuted); return true; }
        break;
    case State::FaultMuted:
        if (event == Event::ClearFault) { enter_muted(State::IdleMuted); return true; }
        break;
    }
    return false;
}
} // namespace app
