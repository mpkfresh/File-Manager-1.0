#pragma once

#include "platform/winekeystate.h"
#include <cstdio>

inline bool runWinEKeyStateTests()
{
    using filemanager::platform::WinEKeyState;
    bool passed = true;
    const auto check = [&passed](bool condition, const char* label) {
        if (!condition) { std::fprintf(stderr, "Win+E: %s\n", label); passed = false; }
    };
    for (const unsigned windowsKey : {0x5Bu, 0x5Cu}) {
        WinEKeyState state;
        check(!state.update(0x45, true).suppress, "ordinary E passes through");
        state.update(0x45, false);
        check(!state.update(windowsKey, true).suppress, "Windows modifier passes through");
        const auto trigger = state.update(0x45, true);
        check(trigger.suppress && trigger.launch, "either Windows key activates exactly Win+E");
        const auto repeat = state.update(0x45, true);
        check(repeat.suppress && !repeat.launch, "held E does not repeatedly launch");
        state.update(windowsKey, false);
        check(state.update(0x45, false).suppress, "captured E release remains paired after Win release");
        check(!state.update(0x45, true).suppress, "next ordinary E is not captured");
    }
    for (const unsigned extra : {0x10u, 0x11u, 0x12u, 0xA0u, 0xA1u, 0xA2u, 0xA3u, 0xA4u, 0xA5u}) {
        WinEKeyState state;
        state.update(0x5B, true);
        state.update(extra, true);
        const auto modified = state.update(0x45, true);
        check(!modified.suppress && !modified.launch, "additional modifiers leave the shortcut unchanged");
        state.update(extra, false);
        check(!state.update(0x45, true).launch, "removing a modifier during held E cannot activate");
        state.update(0x45, false);
        check(state.update(0x45, true).launch, "a fresh E can activate after the modifier is released");
    }
    WinEKeyState state;
    state.update(0x5B, true);
    check(!state.update(0x45, true, true).launch, "injected E never launches");
    check(!state.update(0x41, true).suppress, "unrelated physical key passes through");
    const auto physical = state.update(0x45, true);
    check(physical.launch, "ignored injected events do not consume physical E");
    state.abandonCapture();
    check(!state.update(0x45, false).suppress, "failed launch-queue posting preserves E release");
    state.reset();
    check(!state.update(0x45, true).launch, "reset clears held Windows keys");
    state.reset();
    state.update(0x5B, true);
    state.update(0x5C, true);
    state.update(0x5B, false);
    check(state.update(0x45, true).launch, "the second held Windows key stays active");
    return passed;
}
