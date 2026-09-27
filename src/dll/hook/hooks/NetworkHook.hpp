#pragma once

#include <array>

#include "dll/hook/Hook.hpp"

namespace velyx {

// Every datagram the game sends or receives passes through four functions in
// ws2_32, whichever way the game was built to call them. Standing in all four is how
// the connection can be measured without a single signature: the ping, the address of
// the server and the gaps between its answers are all in the traffic itself, not in
// the game's memory.
class NetworkHook final : public Hook {
public:
    NetworkHook();

    bool install() override;
    void uninstall() override;

private:
    std::array<void*, 4> targets_{};
};

}
