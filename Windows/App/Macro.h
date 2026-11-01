#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace vikey { namespace macro {

// Initialize macro system (loads macro.txt from portable dir or %APPDATA%\HKey\macro.txt)
void init();

// Reload macro table from disk (called automatically if file timestamp changes)
void reload();

// Check if macro expansion is enabled
bool isEnabled();
void setEnabled(bool enabled);

// Get the resolved path to macro.txt
std::wstring macroFilePath();

// Open macro.txt in default text editor (notepad)
void openMacroFile();

// Keystroke tracking: feeds keys to the macro buffer
void onChar(wchar_t c, bool capsLock = false);
void onBackspace();
void reset();

// Check if a character is a macro delimiter (Space, Enter, Tab, punctuation...)
bool isDelimiter(WORD vk, unsigned short engineKey, bool shift);

// Check if current buffer matches any macro shortcut
// If match, returns true and fills replacement text (with case matched) and shortcut length
bool match(std::wstring& outReplacement, int& outShortcutLen);

// Number of loaded macros
size_t count();

}} // namespace vikey::macro

namespace hkey { namespace macro {
    using namespace vikey::macro;
}}
