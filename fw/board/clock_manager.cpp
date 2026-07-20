#include "board/clock_manager.hpp"
#include "board/target_board.hpp"

namespace board {
void ClockManager::initialize_safe() noexcept {
    selected_ = audio::ClockFamily::Family44k1;
    stable_ = false;
    target::select_clock_family(selected_);
}

void ClockManager::select(audio::ClockFamily family) noexcept {
    stable_ = false;
    selected_ = family;
    target::select_clock_family(family);
}
} // namespace board

