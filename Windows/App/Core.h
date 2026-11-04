// The keyboard core on Windows: the low-level keyboard/mouse hooks that feed the Telex engine,
// smart switch per app/website, auto switch by typing, and the typing log.
#pragma once
#include <windows.h>
#include <string>
#include <functional>

namespace vikey { namespace core {

bool start();                       // install hooks (needs a message loop on the calling thread)
void stop();
bool isRunning();
int language();                     // 0 English, 1 Vietnamese
bool isBlacklistActive();           // true if front app is currently blacklisted
void setLanguageByUser(int lang);   // from the tray menu / settings window
void setLanguageQuiet(int lang);    // tests: set the language without the 5 s pause or a log entry
void reloadSettings();              // after a setting changed in the registry
void onForegroundChanged();         // called by the foreground WinEvent hook
void onSiteChanged(const std::wstring& host);   // per-site memory (Phase 3)
std::wstring frontAppName();
inline bool isTerminalOrElectronApp(const std::wstring& app) {
    if (app.empty() || app == L"UnknownApp") return false;
    std::wstring name = app;
    size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    for (auto& c : name) c = (wchar_t)towlower(c);

    static const wchar_t* termApps[] = {
        L"code.exe", L"cursor.exe", L"windowsterminal.exe", L"openconsole.exe",
        L"conhost.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe",
        L"alacritty.exe", L"wezterm-gui.exe", L"wezterm.exe", L"hyper.exe",
        L"tabby.exe", L"putty.exe", L"kitty.exe", L"ghostty.exe",
        L"mintty.exe", L"wsl.exe", L"idea64.exe", L"pycharm64.exe",
        L"webstorm64.exe", L"clion64.exe"
    };
    for (const auto* t : termApps) {
        if (name == t) return true;
    }
    return false;
}

/// Called on the UI thread whenever the language changes (menu icon, settings window).
void setLanguageObserver(std::function<void(int)> f);

// Emoji picker (Settings.cpp): called on the hook thread when its hotkey is pressed (EmojiPicker / EmojiHotkey prefs).
void setEmojiHotkeyHandler(std::function<void()> f);
/// While open, key downs go to the picker window as WM_APP+10 and a click outside it posts WM_APP+11
void setEmojiPickerOpen(bool open, HWND picker);
/// Forget the word being typed
void resetTyping();
/// Types text into the focused app
void insertText(const std::wstring& text);

// Typing log
std::wstring typingLogPath();
void flushTypingLog();
void clearTypingLog();

}} // namespace vikey::core

namespace hkey { namespace core {
    using namespace vikey::core;
}}
