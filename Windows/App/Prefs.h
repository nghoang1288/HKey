// Settings in HKCU\Software\HKey
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace vikey { namespace prefs {

int getInt(const wchar_t* key, int fallback);
bool has(const wchar_t* key);
void setInt(const wchar_t* key, int value);
std::vector<BYTE> getBinary(const wchar_t* key);
void setBinary(const wchar_t* key, const BYTE* data, DWORD size);
std::wstring getString(const wchar_t* key, const std::wstring& fallback);
void setString(const wchar_t* key, const std::wstring& value);
void remove(const wchar_t* key);

/// %LOCALAPPDATA%\HKey, created if needed.
std::wstring appDataDir();

/// Run at login via HKCU\...\Run.
bool launchAtLogin();
void setLaunchAtLogin(bool on);

/// Blacklist / Game mode
bool isBlacklistEnabled();
void setBlacklistEnabled(bool on);
std::vector<std::wstring> getBlacklist();
void setBlacklist(const std::vector<std::wstring>& list);
bool isAppBlacklisted(const std::wstring& exeName);

/// Terminal & Electron key delay
int getTerminalDelayEnabled();
void setTerminalDelayEnabled(int mode);
int getTerminalDelayMs();
void setTerminalDelayMs(int ms);

/// Auto capitalization
int getAutoCapitalize();
void setAutoCapitalize(int on);

/// Settings migration (OpenKey / Vikey)
void migrateFromOpenKey();

}} // namespace vikey::prefs

namespace hkey { namespace prefs {
    using namespace vikey::prefs;
}}
