#include "board/output_relay.hpp"
#include "board/target_board.hpp"

namespace board {
void OutputRelay::initialize_safe() noexcept { disconnect(); }
void OutputRelay::disconnect() noexcept { target::set_relay_connected(false); connected_ = false; }
void OutputRelay::connect() noexcept { target::set_relay_connected(true); connected_ = true; }
} // namespace board

