#include "Prefs.h"
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <sstream>

namespace vikey { namespace prefs {

static const wchar_t* ROOT = L"Software\\HKey";

static HKEY open(REGSAM access) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ROOT, 0, nullptr, REG_OPTION_NON_VOLATILE, access, nullptr, &h, nullptr) != ERROR_SUCCESS) return nullptr;
    return h;
}

bool has(const wchar_t* key) {
    HKEY h = open(KEY_READ); if (!h) return false;
    DWORD type = 0, size = 0;
    bool ok = RegQueryValueExW(h, key, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
    RegCloseKey(h); return ok;
}

int getInt(const wchar_t* key, int fallback) {
    HKEY h = open(KEY_READ); if (!h) return fallback;
    DWORD v = 0, size = sizeof(v), type = 0;
    int r = fallback;
    if (RegQueryValueExW(h, key, nullptr, &type, (LPBYTE)&v, &size) == ERROR_SUCCESS && type == REG_DWORD) r = (int)v;
    RegCloseKey(h); return r;
}

void setInt(const wchar_t* key, int value) {
    HKEY h = open(KEY_WRITE); if (!h) return;
    DWORD v = (DWORD)value;
    RegSetValueExW(h, key, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(h);
}

std::vector<BYTE> getBinary(const wchar_t* key) {
    std::vector<BYTE> out;
    HKEY h = open(KEY_READ); if (!h) return out;
    DWORD size = 0, type = 0;
    if (RegQueryValueExW(h, key, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size > 0) {
        out.resize(size);
        if (RegQueryValueExW(h, key, nullptr, &type, out.data(), &size) != ERROR_SUCCESS) out.clear();
    }
    RegCloseKey(h); return out;
}

void setBinary(const wchar_t* key, const BYTE* data, DWORD size) {
    HKEY h = open(KEY_WRITE); if (!h) return;
    RegSetValueExW(h, key, 0, REG_BINARY, data, size);
    RegCloseKey(h);
}

std::wstring getString(const wchar_t* key, const std::wstring& fallback) {
    HKEY h = open(KEY_READ); if (!h) return fallback;
    wchar_t buf[1024]; DWORD size = sizeof(buf), type = 0;
    std::wstring r = fallback;
    if (RegQueryValueExW(h, key, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS && type == REG_SZ) r.assign(buf, size / 2 ? size / 2 - 1 : 0);
    RegCloseKey(h); return r;
}

void setString(const wchar_t* key, const std::wstring& value) {
    HKEY h = open(KEY_WRITE); if (!h) return;
    RegSetValueExW(h, key, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
}

void remove(const wchar_t* key) {
    HKEY h = open(KEY_WRITE); if (!h) return;
    RegDeleteValueW(h, key); RegCloseKey(h);
}

std::wstring appDataDir() {
    wchar_t path[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path);
    std::wstring dir = std::wstring(path) + L"\\HKey";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool launchAtLogin() {
    HKEY h; if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD size = 0; bool ok = RegQueryValueExW(h, L"HKey", nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS;
    RegCloseKey(h); return ok;
}

void setLaunchAtLogin(bool on) {
    HKEY h; if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &h) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(h, L"HKey", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(h, L"HKey");
    RegCloseKey(h);
}

static const wchar_t* TASK_NAME = L"HKeyAutoStartAdmin";

bool isTaskSchedulerAdminAutostart() {
    int regVal = getInt(L"RunAsAdminTask", -1);
    std::wstring cmd = L"schtasks.exe /Query /TN \"" + std::wstring(TASK_NAME) + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    bool exists = false;
    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DWORD waitRes = WaitForSingleObject(pi.hProcess, 1500);
        DWORD exitCode = 1;
        if (waitRes == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode == 0) {
            exists = true;
        } else if (waitRes == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else if (regVal == 1) {
        exists = true;
    }
    return exists;
}

bool setTaskSchedulerAdminAutostart(bool on) {
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        // Note: schtasks /Create /TR requires single quotes inside double quotes when path has spaces
        std::wstring params = L"/Create /TN \"" + std::wstring(TASK_NAME) + L"\" /TR \"'" + exe + L"'\" /SC ONLOGON /RL HIGHEST /F";

        SHELLEXECUTEINFOW sei = {sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = L"schtasks.exe";
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;

        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 5000);
            DWORD exitCode = 1;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);
            if (exitCode == 0) {
                setLaunchAtLogin(false);
                setInt(L"RunAsAdminTask", 1);
                return true;
            }
        }
        return false;
    } else {
        std::wstring params = L"/Delete /TN \"" + std::wstring(TASK_NAME) + L"\" /F";

        SHELLEXECUTEINFOW sei = {sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = L"schtasks.exe";
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;

        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 5000);
            CloseHandle(sei.hProcess);
        }
        setInt(L"RunAsAdminTask", 0);
        return true;
    }
}

static const wchar_t* DEFAULT_BLACKLIST = L"photoshop.exe;illustrator.exe;acad.exe;valorant.exe;league of legends.exe;cs2.exe;dota2.exe;genshinimpact.exe;overwatch.exe";

bool isBlacklistEnabled() {
    return getInt(L"BlacklistEnabled", 1) != 0;
}

void setBlacklistEnabled(bool on) {
    setInt(L"BlacklistEnabled", on ? 1 : 0);
}

static std::wstring toLower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

std::vector<std::wstring> getBlacklist() {
    std::wstring raw = getString(L"Blacklist", DEFAULT_BLACKLIST);
    std::vector<std::wstring> list;
    std::wstringstream ss(raw);
    std::wstring item;
    while (std::getline(ss, item, L';')) {
        size_t first = item.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) continue;
        size_t last = item.find_last_not_of(L" \t\r\n");
        std::wstring clean = toLower(item.substr(first, last - first + 1));
        if (!clean.empty()) {
            if (std::find(list.begin(), list.end(), clean) == list.end()) {
                list.push_back(clean);
            }
        }
    }
    return list;
}

void setBlacklist(const std::vector<std::wstring>& list) {
    std::wstring raw;
    for (size_t i = 0; i < list.size(); i++) {
        std::wstring item = toLower(list[i]);
        if (item.empty()) continue;
        if (!raw.empty()) raw += L";";
        raw += item;
    }
    setString(L"Blacklist", raw);
}

bool isAppBlacklisted(const std::wstring& exeName) {
    if (!isBlacklistEnabled()) return false;
    if (exeName.empty() || exeName == L"UnknownApp") return false;
    std::wstring name = exeName;
    size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    name = toLower(name);

    auto list = getBlacklist();
    for (const auto& item : list) {
        if (item == name) return true;
    }
    return false;
}

void migrateFromOpenKey() {
    if (has(L"MigratedFromPrevious")) return;
    setInt(L"MigratedFromPrevious", 1);

    // Try migrating from Vikey first
    HKEY hVikey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Vikey", 0, KEY_READ, &hVikey) == ERROR_SUCCESS) {
        for (DWORD i = 0;; i++) {
            wchar_t valName[256]; DWORD valLen = 256, type = 0;
            BYTE data[4096]; DWORD dataLen = sizeof(data);
            if (RegEnumValueW(hVikey, i, valName, &valLen, nullptr, &type, data, &dataLen) != ERROR_SUCCESS) break;
            if (!has(valName)) {
                if (type == REG_DWORD && dataLen == sizeof(DWORD)) setInt(valName, *(int*)data);
                else if (type == REG_SZ) setString(valName, (wchar_t*)data);
                else if (type == REG_BINARY) setBinary(valName, data, dataLen);
            }
        }
        RegCloseKey(hVikey);
        return;
    }

    // Fallback: migrate from OpenKey
    HKEY h; if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\TuyenMai\\OpenKey", 0, KEY_READ, &h) != ERROR_SUCCESS) return;
    struct { const wchar_t* from; const wchar_t* to; } map[] = {
        {L"vLanguage", L"InputMethod"}, {L"vCheckSpelling", L"Spelling"}, {L"vRestoreIfWrongSpelling", L"RestoreIfInvalidWord"},
        {L"vUseModernOrthography", L"ModernOrthography"}, {L"vUseSmartSwitchKey", L"UseSmartSwitchKey"}, {L"vSwitchKeyStatus", L"SwitchKeyStatus"},
    };
    for (auto& m : map) {
        DWORD v = 0, size = sizeof(v), type = 0;
        if (RegQueryValueExW(h, m.from, nullptr, &type, (LPBYTE)&v, &size) == ERROR_SUCCESS && type == REG_DWORD && !has(m.to)) setInt(m.to, (int)v);
    }
    DWORD size = 0, type = 0;
    if (RegQueryValueExW(h, L"smartSwitchKey", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size > 0 && !has(L"smartSwitchKey")) {
        std::vector<BYTE> d(size);
        if (RegQueryValueExW(h, L"smartSwitchKey", nullptr, &type, d.data(), &size) == ERROR_SUCCESS) setBinary(L"smartSwitchKey", d.data(), size);
    }
    RegCloseKey(h);
}

}} // namespace vikey::prefs
