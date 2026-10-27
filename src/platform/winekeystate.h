#pragma once

#include <array>

namespace filemanager::platform {

// Only shortcut state is retained. No text, key history, or timestamps are collected.
class WinEKeyState final
{
public:
    struct Decision { bool suppress = false; bool launch = false; };
    Decision update(unsigned key, bool down, bool injected = false)
    {
        if (injected) return {};
        for (unsigned i = 0; i < modifierKeys.size(); ++i) {
            if (key == modifierKeys[i]) {
                modifiers_[i] = down;
                return {};
            }
        }
        if (key != 0x45) return {};
        if (!down) {
            const bool suppress = captured_;
            captured_ = false;
            eDown_ = false;
            return {suppress, false};
        }
        if (eDown_) return {captured_, false};
        eDown_ = true;
        const bool windows = modifiers_[0] || modifiers_[1];
        bool extra = false;
        for (unsigned i = 2; i < modifiers_.size(); ++i) extra |= modifiers_[i];
        captured_ = windows && !extra;
        return {captured_, captured_};
    }
    void abandonCapture() { captured_ = false; }
    void reset() { modifiers_ = {}; eDown_ = false; captured_ = false; }

private:
    static constexpr std::array<unsigned, 11> modifierKeys{
        0x5B, 0x5C, 0x10, 0x11, 0x12, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    std::array<bool, 11> modifiers_{};
    bool eDown_ = false;
    bool captured_ = false;
};

} // namespace filemanager::platform
