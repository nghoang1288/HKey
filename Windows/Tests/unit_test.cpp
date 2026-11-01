#define _HAS_STD_BYTE 0
#include <windows.h>
#include "Engine.h"
#include "Driver.h"
#include "Prefs.h"
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

    printf("\n===================================\n");
    if (failed == 0) {
        printf("ALL TESTS PASSED SUCCESSFULLY! (0 failures)\n");
    } else {
        printf("SOME TESTS FAILED! (%d failures)\n", failed);
    }
    return failed;
}
