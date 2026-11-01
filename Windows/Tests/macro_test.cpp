#include <windows.h>
#include <cassert>
#include <iostream>
#include <string>
#include "../App/Macro.h"

// Simple test assertions
#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED: " << #cond << " at line " << __LINE__ << std::endl; \
        return 1; \
    } \
} while(0)

int main() {
    std::cout << "Testing Vikey Macro subsystem..." << std::endl;

    // 1. Initialize
    vikey::macro::init();
    std::cout << "Loaded " << vikey::macro::count() << " macros." << std::endl;
    TEST_ASSERT(vikey::macro::count() > 0);

    // 2. Test lowercase lookup
    vikey::macro::reset();
    vikey::macro::onChar(L'k');
    std::wstring repl;
    int len = 0;
    TEST_ASSERT(vikey::macro::match(repl, len));
    TEST_ASSERT(repl == L"không");
    TEST_ASSERT(len == 1);

    // 3. Test Title Case lookup (Shift+k)
    vikey::macro::reset();
    vikey::macro::onChar(L'K', false);
    TEST_ASSERT(vikey::macro::match(repl, len));
    TEST_ASSERT(repl == L"Không");
    TEST_ASSERT(len == 1);

    // 3b. Test All-Caps single char (CapsLock on: K -> KHÔNG)
    vikey::macro::reset();
    vikey::macro::onChar(L'K', true);
    TEST_ASSERT(vikey::macro::match(repl, len));
    TEST_ASSERT(repl == L"KHÔNG");
    TEST_ASSERT(len == 1);

    // 4. Test Multi-char lowercase: "dc" -> "được"
    vikey::macro::reset();
    vikey::macro::onChar(L'd');
    vikey::macro::onChar(L'c');
    TEST_ASSERT(vikey::macro::match(repl, len));
    TEST_ASSERT(repl == L"được");
    TEST_ASSERT(len == 2);

    // 5. Test Multi-char Title Case: "Dc" -> "Được"
    vikey::macro::reset();
    vikey::macro::onChar(L'D');
    vikey::macro::onChar(L'c');
    TEST_ASSERT(vikey::macro::match(repl, len));
    TEST_ASSERT(repl == L"Được");
    TEST_ASSERT(len == 2);

    // 6. Test Multi-char All Uppercase: "DC" -> "ĐƯỢC"
    vikey::macro::reset();
    vikey::macro::onChar(L'D');
    vikey::macro::onChar(L'C');
    TEST_ASSERT(vikey::macro::match(repl, len));
    TEST_ASSERT(repl == L"ĐƯỢC");
    TEST_ASSERT(len == 2);

    // 7. Test Backspace handling
    vikey::macro::reset();
    vikey::macro::onChar(L'k');
    vikey::macro::onChar(L'a'); // typed "ka"
    vikey::macro::onBackspace(); // backspaced to "k"
    TEST_ASSERT(vikey::macro::match(repl, len));
    TEST_ASSERT(repl == L"không");

    // 8. Test Delimiters
    TEST_ASSERT(vikey::macro::isDelimiter(VK_SPACE, 49, false)); // Space
    TEST_ASSERT(vikey::macro::isDelimiter(VK_RETURN, 36, false)); // Enter
    TEST_ASSERT(vikey::macro::isDelimiter(VK_TAB, 48, false)); // Tab
    TEST_ASSERT(vikey::macro::isDelimiter(VK_OEM_COMMA, 43, false)); // ,
    TEST_ASSERT(vikey::macro::isDelimiter(VK_OEM_PERIOD, 47, false)); // .

    // 9. Test Non-matching word
    vikey::macro::reset();
    vikey::macro::onChar(L'x');
    vikey::macro::onChar(L'y');
    vikey::macro::onChar(L'z');
    TEST_ASSERT(!vikey::macro::match(repl, len));

    std::cout << "All Vikey Macro unit tests PASSED successfully!" << std::endl;
    return 0;
}
