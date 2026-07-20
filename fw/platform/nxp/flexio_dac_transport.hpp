#pragma once

#include "devices/dac.hpp"

namespace platform::nxp {
class FlexioDacTransport final : public devices::DacTransport {
public:
    void initialize() noexcept override;
    void set_reset(bool asserted) noexcept override;
    void set_chip_select(bool asserted) noexcept override;
    bool transfer(const devices::DacCommand& command) noexcept override;
};
} // namespace platform::nxp
