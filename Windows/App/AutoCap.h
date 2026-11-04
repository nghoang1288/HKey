#pragma once
#include <cwctype>
#include <string>
#include "Engine.h"

namespace vikey {

enum class AutoCapState {
    IDLE = 0,
    TRIGGERED = 1,
    ARMED = 2
};

struct AutoCapitalizeHelper {
    AutoCapState state = AutoCapState::IDLE;
    bool justApplied = false;
    std::wstring recentWord;
    wchar_t lastChar = 0;

    void reset() {
        state = AutoCapState::IDLE;
        justApplied = false;
        recentWord.clear();
        lastChar = 0;
    }

    static bool isAbbreviation(const std::wstring& word) {
        static const wchar_t* abbrevs[] = {
            L"tp", L"th.s", L"dr", L"mr", L"mrs", L"ms",
            L"ts", L"bs", L"ks", L"gs", L"tt", L"etc", L"vv", L"eg", L"ie"
        };
        std::wstring lower = word;
        for (auto& c : lower) c = (wchar_t)towlower(c);
        for (const auto* a : abbrevs) {
            if (lower == a) return true;
        }
        return false;
    }

    static bool isLetterKey(Uint16 key) {
        for (Uint16 k : {KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,KEY_L,KEY_M,KEY_N,KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,KEY_W,KEY_X,KEY_Y,KEY_Z}) {
            if (k == key) return true;
        }
        return false;
    }

    // Process a key event. Returns true if this key should be auto-capitalized.
    bool processKey(Uint16 key, bool shift, bool capsLock, bool otherControl) {
        if (otherControl) {
            reset();
            return false;
        }

        if (key == KEY_DELETE) {
            if (justApplied) {
                // User pressed Backspace immediately after auto-cap: cancel and don't re-capitalize
                justApplied = false;
                state = AutoCapState::IDLE;
            } else if (state == AutoCapState::ARMED) {
                // Backspaced the space after punctuation: back to TRIGGERED
                state = AutoCapState::TRIGGERED;
            }
            if (!recentWord.empty()) recentWord.pop_back();
            lastChar = 0;
            return false;
        }

        justApplied = false;

        // Enter key always triggers ARMED for the next sentence line
        if (key == KEY_RETURN || key == KEY_ENTER) {
            state = AutoCapState::ARMED;
            recentWord.clear();
            lastChar = L'\n';
            return false;
        }

        // Punctuation: ? (Shift + /), ! (Shift + 1)
        if (shift && (key == KEY_SLASH || key == KEY_1)) {
            state = AutoCapState::TRIGGERED;
            recentWord.clear();
            lastChar = (key == KEY_SLASH) ? L'?' : L'!';
            return false;
        }

        // Punctuation: . (un-shifted)
        if (!shift && key == KEY_DOT) {
            if (isAbbreviation(recentWord)) {
                state = AutoCapState::IDLE;
            } else {
                state = AutoCapState::TRIGGERED;
            }
            recentWord.clear();
            lastChar = L'.';
            return false;
        }

        if (state == AutoCapState::TRIGGERED) {
            if (key == KEY_SPACE || key == KEY_QUOTE || (shift && key == KEY_9) || key == KEY_LEFT_BRACKET) {
                state = AutoCapState::ARMED;
                lastChar = L' ';
                return false;
            }
            // Any other key breaks the punctuation trigger (e.g. extension or URL like test.com or decimal 3.14)
            state = AutoCapState::IDLE;
        } else if (state == AutoCapState::ARMED) {
            if (key == KEY_SPACE || key == KEY_QUOTE || (shift && key == KEY_9) || key == KEY_LEFT_BRACKET) {
                return false;
            }
        }

        if (isLetterKey(key)) {
            Uint16 ch = keyCodeToCharacter(key);
            if (ch) recentWord += (wchar_t)ch;
            lastChar = (wchar_t)ch;

            if (state == AutoCapState::ARMED) {
                state = AutoCapState::IDLE;
                if (shift || capsLock) {
                    // User explicitly holds Shift or CapsLock is on
                    return false;
                }
                justApplied = true;
                return true;
            }
            return false;
        }

        // Numbers & others
        if (IS_NUMBER_KEY(key) && !shift) {
            lastChar = (wchar_t)(L'0' + (key - KEY_0));
        } else {
            lastChar = 0;
        }
        recentWord.clear();
        state = AutoCapState::IDLE;
        return false;
    }
};

} // namespace vikey
