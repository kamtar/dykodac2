#pragma once

namespace board {
class OutputRelay {
public:
    void initialize_safe() noexcept;
    void disconnect() noexcept;
    void connect() noexcept;
    bool connected() const noexcept { return connected_; }
private:
    bool connected_{false};
};
} // namespace board

