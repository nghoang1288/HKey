#define _HAS_STD_BYTE 0
#include <windows.h>
#include "Engine.h"
#include "Driver.h"
#include "Prefs.h"
#include "Core.h"
#include "AutoCap.h"
#include <cstdio>
#include <cassert>
#include <string>
#include <vector>

int vLanguage = 1, vSwitchKeyStatus = 0, vCheckSpelling = 1, vUseModernOrthography = 1, vRestoreIfWrongSpelling = 1,
    vFixRecommendBrowser = 1, vUseSmartSwitchKey = 1, vOtherLanguage = 1, vAutoSwitchByTyping = 0;

static Uint16 K(char c, bool& isCaps) {
    static const Uint16 L[26] = {KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,KEY_L,KEY_M,KEY_N,KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,KEY_W,KEY_X,KEY_Y,KEY_Z};
    if (c >= 'A' && c <= 'Z') { isCaps = true; return L[c - 'A']; }
    isCaps = false;
    return c == ' ' ? KEY_SPACE : c == '<' ? KEY_DELETE : (c >= 'a' && c <= 'z' ? L[c - 'a'] : 0);
}

static std::string typeWithRotational(const char* keys, int rotational) {
    vRotationalTone = rotational;
    VikeyDriver d;
    vLanguage = 1;
    d.resetContext();
    std::u16string t;
    for (const char* p = keys; *p; p++) {
        bool isCaps = false;
        Uint16 code = K(*p, isCaps);
        VKEdit e = d.press(code, isCaps, 0);
        for (int i = 0; i < e.deleteCount && !t.empty(); i++) t.pop_back();
        t += e.text;
    }
    std::string o;
    for (char16_t c : t) {
        unsigned u = c;
        if (u < 0x80) o += (char)u;
        else if (u < 0x800) { o += (char)(0xC0 | u >> 6); o += (char)(0x80 | (u & 63)); }
        else { o += (char)(0xE0 | u >> 12); o += (char)(0x80 | ((u >> 6) & 63)); o += (char)(0x80 | (u & 63)); }
    }
    return o;
}

int main() {
    printf("=== RUNNING UNIT TESTS ===\n\n");
    int failed = 0;

    // Test 1: Rotational tone enabled
    printf("--- Test 1: Rotational Tone Cycles (vRotationalTone = 1) ---\n");
    struct { const char* keys; const char* expected; } rotCases[] = {
        {"toas", "toá"},
        {"toasf", "toà"},
        {"toasfr", "toả"},
        {"toasfrx", "toã"},
        {"toasfrxj", "toạ"},
        {"toasfrxjs", "toá"},
        {"chuyeens", "chuyến"},
        {"chuyeensf", "chuyền"},
        {"chuyeensfr", "chuyển"},
        {"chuyeensfrx", "chuyễn"},
        {"chuyeensfrxj", "chuyện"},
        {"chuyeensfrxjs", "chuyến"},
        {"tieeps", "tiếp"},
        {"tieepsf", "tiềp"},
        {"tieepsr", "tiểp"},
        {"tieepsx", "tiễp"},
        {"tieepsj", "tiệp"},
        {"toets", "toét"},
        {"toetsf", "toèt"},
        {"toetsj", "toẹt"},
        {"duowngs", "dướng"},
        {"duowngsf", "dường"},
        {"duowngsr", "dưởng"},
        {"hoas", "hoá"},
        {"hoasf", "hoà"},
        {"thuyrs", "thuý"},
        {"thuyrsf", "thuỳ"},
        {"TOAS", "TOÁ"},
        {"TOASF", "TOÀ"},
        {"Toas", "Toá"},
        {"Toasf", "Toà"},
    };

    for (const auto& c : rotCases) {
        std::string got = typeWithRotational(c.keys, 1);
        if (got != c.expected) {
            printf("[FAIL] '%s': got '%s', want '%s'\n", c.keys, got.c_str(), c.expected);
            failed++;
        } else {
            printf("[PASS] '%s' -> '%s'\n", c.keys, got.c_str());
        }
    }

    // Test 2: Same mark key undoes tone
    printf("\n--- Test 2: Duplicate Tone Key Undo ---\n");
    struct { const char* keys; const char* expected; } undoCases[] = {
        {"toass", "toas"},
        {"hooss", "hoos"},
        {"tieepss", "tieeps"},
    };
    for (const auto& c : undoCases) {
        std::string got = typeWithRotational(c.keys, 1);
        if (got != c.expected) {
            printf("[FAIL] '%s': got '%s', want '%s'\n", c.keys, got.c_str(), c.expected);
            failed++;
        } else {
            printf("[PASS] '%s' -> '%s'\n", c.keys, got.c_str());
        }
    }

    // Test 2b: Rotational Tone disabled (vRotationalTone = 0)
    printf("\n--- Test 2b: Rotational Tone Disabled (vRotationalTone = 0) ---\n");
    {
        std::string gotBlocked = typeWithRotational("toetsf", 0);
        printf("[PASS] vRotationalTone=0: 'toetsf' -> '%s' (f rejected by T end-consonant phonetic rule)\n", gotBlocked.c_str());
        if (gotBlocked != "toétf") {
            printf("[FAIL] expected 'toétf', got '%s'\n", gotBlocked.c_str());
            failed++;
        }
    }

    // Test 3: Blacklist prefs
    printf("\n--- Test 3: Blacklist Prefs and Query ---\n");
    using namespace vikey::prefs;
    setBlacklistEnabled(true);
    if (!isBlacklistEnabled()) { printf("[FAIL] isBlacklistEnabled false\n"); failed++; }
    else printf("[PASS] isBlacklistEnabled == true\n");

    auto bl = getBlacklist();
    printf("Current blacklist size: %zu\n", bl.size());
    if (bl.empty()) { printf("[FAIL] getBlacklist empty\n"); failed++; }
    else printf("[PASS] getBlacklist populated\n");

    // Test default items
    assert(isAppBlacklisted(L"photoshop.exe"));
    assert(isAppBlacklisted(L"Photoshop.EXE"));
    assert(isAppBlacklisted(L"C:\\Adobe\\photoshop.exe"));
    assert(isAppBlacklisted(L"valorant.exe"));
    assert(isAppBlacklisted(L"cs2.exe"));
    assert(!isAppBlacklisted(L"notepad.exe"));
    assert(!isAppBlacklisted(L"word.exe"));
    printf("[PASS] Default apps matching correctly (case-insensitive & paths)\n");

    // Test custom blacklist update
    std::vector<std::wstring> customList = {L"mygame.exe", L"blender.exe"};
    setBlacklist(customList);
    assert(isAppBlacklisted(L"mygame.exe"));
    assert(isAppBlacklisted(L"D:\\Steam\\steamapps\\common\\MyGame\\MyGame.exe"));
    assert(isAppBlacklisted(L"blender.exe"));
    assert(!isAppBlacklisted(L"photoshop.exe")); // not in custom list
    printf("[PASS] Custom blacklist set/get working properly\n");

    // Reset default blacklist
    std::vector<std::wstring> def = {
        L"photoshop.exe", L"illustrator.exe", L"acad.exe", L"valorant.exe",
        L"league of legends.exe", L"cs2.exe", L"dota2.exe", L"genshinimpact.exe", L"overwatch.exe"
    };
    setBlacklist(def);
    assert(isAppBlacklisted(L"photoshop.exe"));
    assert(!isAppBlacklisted(L"mygame.exe"));
    printf("[PASS] Reset to default blacklist succeeded\n");

    // Test toggle off
    setBlacklistEnabled(false);
    assert(!isAppBlacklisted(L"photoshop.exe"));
    setBlacklistEnabled(true);
    assert(isAppBlacklisted(L"photoshop.exe"));
    printf("[PASS] Blacklist enable/disable toggle working\n");

    // Test 4: Terminal and Electron App Detection
    printf("\n--- Test 4: Terminal and Electron App Detection ---\n");
    using namespace vikey::core;
    assert(isTerminalOrElectronApp(L"code.exe"));
    assert(isTerminalOrElectronApp(L"Code.exe"));
    assert(isTerminalOrElectronApp(L"C:\\Users\\User\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe"));
    assert(isTerminalOrElectronApp(L"cursor.exe"));
    assert(isTerminalOrElectronApp(L"Cursor.exe"));
    assert(isTerminalOrElectronApp(L"windowsterminal.exe"));
    assert(isTerminalOrElectronApp(L"WindowsTerminal.exe"));
    assert(isTerminalOrElectronApp(L"cmd.exe"));
    assert(isTerminalOrElectronApp(L"powershell.exe"));
    assert(isTerminalOrElectronApp(L"pwsh.exe"));
    assert(isTerminalOrElectronApp(L"alacritty.exe"));
    assert(isTerminalOrElectronApp(L"wezterm-gui.exe"));
    assert(isTerminalOrElectronApp(L"ghostty.exe"));
    assert(isTerminalOrElectronApp(L"idea64.exe"));
    assert(isTerminalOrElectronApp(L"pycharm64.exe"));
    assert(!isTerminalOrElectronApp(L"notepad.exe"));
    assert(!isTerminalOrElectronApp(L"word.exe"));
    assert(!isTerminalOrElectronApp(L"chrome.exe"));
    assert(!isTerminalOrElectronApp(L"photoshop.exe"));
    assert(!isTerminalOrElectronApp(L""));
    printf("[PASS] Terminal and Electron apps detected accurately\n");

    // Test 5: Terminal Delay and AutoCap Preferences
    printf("\n--- Test 5: Terminal Delay & AutoCap Prefs ---\n");
    setTerminalDelayEnabled(1);
    assert(getTerminalDelayEnabled() == 1);
    setTerminalDelayMs(20);
    assert(getTerminalDelayMs() == 20);
    setTerminalDelayMs(15);
    assert(getTerminalDelayMs() == 15);
    setAutoCapitalize(1);
    assert(getAutoCapitalize() == 1);
    setAutoCapitalize(0);
    assert(getAutoCapitalize() == 0);
    setAutoCapitalize(1);
    assert(getAutoCapitalize() == 1);
    printf("[PASS] Terminal Delay & AutoCap Prefs get/set working\n");

    // Test 6: Auto-Capitalization Helper State Machine
    printf("\n--- Test 6: Auto-Capitalization Helper State Machine ---\n");
    vikey::AutoCapitalizeHelper ac;

    // Sentence 1: Period + Space + Letter -> AutoCap
    ac.reset();
    assert(!ac.processKey(KEY_C, false, false, false)); // 'c'
    assert(!ac.processKey(KEY_H, false, false, false)); // 'h'
    assert(!ac.processKey(KEY_A, false, false, false)); // 'a'
    assert(!ac.processKey(KEY_O, false, false, false)); // 'o'
    assert(!ac.processKey(KEY_DOT, false, false, false)); // '.'
    assert(!ac.processKey(KEY_SPACE, false, false, false)); // ' '
    bool shouldCap1 = ac.processKey(KEY_B, false, false, false); // 'b'
    assert(shouldCap1); // Should capitalize 'B'
    printf("[PASS] Period + space + letter auto-capitalizes\n");

    // Follow-up letter in same word should NOT auto-cap
    assert(!ac.processKey(KEY_A, false, false, false)); // 'a'
    assert(!ac.processKey(KEY_N, false, false, false)); // 'n'

    // Sentence 2: Question mark + Space + Letter -> AutoCap
    assert(!ac.processKey(KEY_SLASH, true, false, false)); // '?' (Shift + /)
    assert(!ac.processKey(KEY_SPACE, false, false, false)); // ' '
    bool shouldCap2 = ac.processKey(KEY_T, false, false, false); // 't'
    assert(shouldCap2);
    printf("[PASS] Question mark + space + letter auto-capitalizes\n");

    // Sentence 3: Exclamation mark + Space + Letter -> AutoCap
    ac.reset();
    assert(!ac.processKey(KEY_1, true, false, false)); // '!' (Shift + 1)
    assert(!ac.processKey(KEY_SPACE, false, false, false)); // ' '
    assert(ac.processKey(KEY_V, false, false, false)); // 'v'
    printf("[PASS] Exclamation + space + letter auto-capitalizes\n");

    // Sentence 4: Enter (Newline) -> AutoCap next line
    ac.reset();
    assert(!ac.processKey(KEY_RETURN, false, false, false)); // Enter
    assert(ac.processKey(KEY_H, false, false, false)); // 'h'
    printf("[PASS] Enter newline auto-capitalizes first letter\n");

    // Sentence 5: Decimal number: 3.14 should NOT capitalize
    ac.reset();
    assert(!ac.processKey(KEY_3, false, false, false)); // '3'
    assert(!ac.processKey(KEY_DOT, false, false, false)); // '.'
    assert(!ac.processKey(KEY_1, false, false, false)); // '1'
    assert(!ac.processKey(KEY_4, false, false, false)); // '4'
    assert(!ac.processKey(KEY_SPACE, false, false, false)); // ' '
    // After "3.14 ", typing next letter should NOT auto-cap because '.' was followed by '1' (not space)
    assert(!ac.processKey(KEY_K, false, false, false));
    printf("[PASS] Decimal number (3.14) does NOT trigger auto-capitalization\n");

    // Sentence 6: Numbered list: "1. " SHOULD auto-cap
    ac.reset();
    assert(!ac.processKey(KEY_1, false, false, false)); // '1'
    assert(!ac.processKey(KEY_DOT, false, false, false)); // '.'
    assert(!ac.processKey(KEY_SPACE, false, false, false)); // ' '
    assert(ac.processKey(KEY_D, false, false, false)); // 'd' -> should cap
    printf("[PASS] Numbered list item (1. ) DOES trigger auto-capitalization\n");

    // Sentence 7: URL / extension: "test.com" should NOT auto-cap
    ac.reset();
    assert(!ac.processKey(KEY_T, false, false, false));
    assert(!ac.processKey(KEY_DOT, false, false, false));
    assert(!ac.processKey(KEY_C, false, false, false)); // 'c' immediately after dot
    assert(!ac.processKey(KEY_O, false, false, false));
    assert(!ac.processKey(KEY_M, false, false, false));
    printf("[PASS] URL domain (test.com) does NOT auto-capitalize\n");

    // Sentence 8: Abbreviation: "tp. " should NOT auto-cap
    ac.reset();
    assert(!ac.processKey(KEY_T, false, false, false)); // 't'
    assert(!ac.processKey(KEY_P, false, false, false)); // 'p'
    assert(!ac.processKey(KEY_DOT, false, false, false)); // '.'
    assert(!ac.processKey(KEY_SPACE, false, false, false)); // ' '
    assert(!ac.processKey(KEY_H, false, false, false)); // 'h' -> should NOT cap
    printf("[PASS] Abbreviation (tp. ) does NOT trigger auto-capitalization\n");

    // Sentence 9: Quote after dot: ." Chào
    ac.reset();
    assert(!ac.processKey(KEY_DOT, false, false, false)); // '.'
    assert(!ac.processKey(KEY_QUOTE, true, false, false)); // '"'
    assert(!ac.processKey(KEY_SPACE, false, false, false)); // ' '
    assert(ac.processKey(KEY_C, false, false, false)); // 'c' -> should cap
    printf("[PASS] Punctuation followed by quote (.\" ) auto-capitalizes\n");

    // Sentence 10: Backspace undo auto-cap
    ac.reset();
    assert(!ac.processKey(KEY_DOT, false, false, false));
    assert(!ac.processKey(KEY_SPACE, false, false, false));
    assert(ac.processKey(KEY_A, false, false, false)); // Auto-capped
    // User hits Backspace immediately to undo
    assert(!ac.processKey(KEY_DELETE, false, false, false));
    // User re-types 'a' lowercase -> should NOT cap again!
    assert(!ac.processKey(KEY_A, false, false, false));
    printf("[PASS] Backspace immediate undo cancels auto-capitalization\n");

    printf("\n===================================\n");
    if (failed == 0) {
        printf("ALL TESTS PASSED SUCCESSFULLY! (0 failures)\n");
    } else {
        printf("SOME TESTS FAILED! (%d failures)\n", failed);
    }
    return failed;
}
