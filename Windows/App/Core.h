// The keyboard core on Windows: the low-level keyboard/mouse hooks that feed the Telex engine,
// smart switch per app/website, auto switch by typing, and the typing log.
#pragma once
#include <windows.h>
#include <cwctype>
#include <string>
#include <functional>

namespace vikey { namespace core {

bool start();                       // install hooks on a dedicated high-priority hook thread
void stop();
bool isRunning();
int language();                     // 0 English, 1 Vietnamese (thread-safe atomic read)
bool isBlacklistActive();           // true if front app is currently blacklisted
void setLanguageByUser(int lang);   // from the tray menu / settings window
void setLanguageQuiet(int lang);    // tests: set the language without the 5 s pause or a log entry
void reloadSettings();              // after a setting changed in the registry
void onForegroundChanged();         // called by the foreground WinEvent hook
void onSiteChanged(const std::wstring& host);   // per-site memory (Phase 3)
std::wstring frontAppName();
void setRecordingHotkey(bool recording);        // pauses switch/emoji hotkey interception during UI recording
void setUiNotificationWindow(HWND hwnd, UINT asyncMsg); // async tray/registry/log flush notifications
void processUiAsyncNotifications(); // executed on UI thread when asyncMsg arrives

inline bool isIgnoredSystemUiWindow(const wchar_t* className, const wchar_t* exeName) {
    if (className && *className) {
        static const wchar_t* const ignoredClasses[] = {
            L"Shell_TrayWnd",
            L"Shell_SecondaryTrayWnd",
            L"NotifyIconOverflowWindow",
            L"TopLevelWindowForOverflowXamlIsland",
            L"Shell_InputSwitchTopLevelWindow",
            L"Shell_InputSwitchDismissOverlay",
            L"TaskListThumbnailWnd",
        };
        for (const wchar_t* c : ignoredClasses) {
            if (_wcsicmp(className, c) == 0) return true;
        }
    }
    if (exeName && *exeName) {
        std::wstring name(exeName);
        size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        for (auto& ch : name) ch = (wchar_t)towlower(ch);
        if (name == L"shellexperiencehost.exe" || name == L"shellhost.exe") return true;
    }
    return false;
}
inline bool isTerminalOrElectronApp(const std::wstring& app) {
    if (app.empty() || app == L"UnknownApp") return false;
    std::wstring name = app;
    size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    for (auto& c : name) c = (wchar_t)towlower(c);

    static const wchar_t* termApps[] = {
        L"code.exe", L"cursor.exe", L"windsurf.exe", L"zed.exe",
        L"windowsterminal.exe", L"wt.exe", L"openconsole.exe",
        L"conhost.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe",
        L"alacritty.exe", L"wezterm-gui.exe", L"wezterm.exe", L"hyper.exe",
        L"tabby.exe", L"putty.exe", L"kitty.exe", L"ghostty.exe", L"warp.exe",
        L"mintty.exe", L"wsl.exe", L"idea64.exe", L"pycharm64.exe",
        L"webstorm64.exe", L"clion64.exe",
        L"discord.exe", L"slack.exe", L"notion.exe", L"obsidian.exe"
    };
    for (const auto* t : termApps) {
        if (name == t) return true;
    }
    return false;
}

/// Called on the UI thread whenever the language changes (menu icon, settings window).
void setLanguageObserver(std::function<void(int)> f);

// Emoji picker (Settings.cpp): called when its hotkey is pressed (EmojiPicker / EmojiHotkey prefs).
void setEmojiHotkeyHandler(std::function<void()> f);
/// While open, key downs go to the picker window as WM_APP+10 and a click outside it posts WM_APP+11
void setEmojiPickerOpen(bool open, HWND picker);
/// Forget the word being typed
void resetTyping();
/// Types text into the focused app
void insertText(const std::wstring& text);
/// Resync modifier state (e.g., after sleep/resume)
void resyncModifiers();

// Typing log
std::wstring typingLogPath();
void flushTypingLog();
void clearTypingLog();

}} // namespace vikey::core

namespace hkey { namespace core {
    using namespace vikey::core;
}}
