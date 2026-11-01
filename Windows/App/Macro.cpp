#include "Macro.h"
#include "Prefs.h"
#include "KeyMap.h"
#include <shlobj.h>
#include <shellapi.h>
#include <vector>
#include <map>
#include <algorithm>
#include <fstream>

namespace vikey { namespace macro {

static bool _enabled = true;
static std::wstring _activeFilePath;
static FILETIME _lastModTime = {0, 0};
static std::map<std::wstring, std::wstring> _table; // lowercased shortcut -> replacement
static std::wstring _buffer;
static bool _bufferCapsLock = false;

static std::wstring toLowerStr(const std::wstring& s) {
    std::wstring out = s;
    CharLowerBuffW(out.data(), (DWORD)out.size());
    return out;
}

static std::wstring getExeDir() {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring p = exePath;
    size_t pos = p.find_last_of(L"\\/");
    return pos != std::wstring::npos ? p.substr(0, pos) : L".";
}

static bool fileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

static FILETIME getFileModTime(const std::wstring& path) {
    FILETIME ft = {0, 0};
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        ft = fad.ftLastWriteTime;
    }
    return ft;
}

std::wstring macroFilePath() {
    if (!_activeFilePath.empty() && fileExists(_activeFilePath)) {
        return _activeFilePath;
    }

    // 1. Check directory of Vikey.exe (Portable mode)
    std::wstring exeDir = getExeDir();
    std::wstring portable = exeDir + L"\\macro.txt";
    if (fileExists(portable)) {
        _activeFilePath = portable;
        return portable;
    }

    // 2. Fallback to %APPDATA%\Vikey\macro.txt
    wchar_t appData[MAX_PATH] = {0};
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData) == S_OK) {
        std::wstring adFile = std::wstring(appData) + L"\\Vikey\\macro.txt";
        if (fileExists(adFile)) {
            _activeFilePath = adFile;
            return adFile;
        }
    }

    // 3. Fallback to %LOCALAPPDATA%\Vikey\macro.txt
    std::wstring ladFile = prefs::appDataDir() + L"\\macro.txt";
    if (fileExists(ladFile)) {
        _activeFilePath = ladFile;
        return ladFile;
    }

    // 4. File does not exist yet. Check if portable exe directory is writable
    HANDLE hTest = CreateFileW(portable.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hTest != INVALID_HANDLE_VALUE) {
        CloseHandle(hTest);
        DeleteFileW(portable.c_str());
        _activeFilePath = portable;
        return portable;
    }

    // Fallback: writable AppData directory
    if (appData[0] != 0) {
        std::wstring dir = std::wstring(appData) + L"\\Vikey";
        CreateDirectoryW(dir.c_str(), nullptr);
        _activeFilePath = dir + L"\\macro.txt";
        return _activeFilePath;
    }

    _activeFilePath = ladFile;
    return ladFile;
}

static void createSampleFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    // UTF-8 BOM
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    WriteFile(h, bom, sizeof(bom), &written, nullptr);

    const char sample[] =
        "; ===================================================================\r\n"
        "; BẢNG GÕ TẮT VIKEY (macro.txt)\r\n"
        "; Định dạng hỗ trợ: phím_tắt:từ_thay_thế hoặc phím_tắt||từ_thay_thế\r\n"
        "; Tự động giữ hoa/thường: k -> không, K -> Không, K(Caps) -> KHÔNG\r\n"
        "; ===================================================================\r\n"
        "k:không\r\n"
        "dc:được\r\n"
        "ng:người\r\n"
        "vn:Việt Nam\r\n"
        "vk:Vikey\r\n"
        "fbb:https://facebook.com\r\n";

    WriteFile(h, sample, (DWORD)(sizeof(sample) - 1), &written, nullptr);
    CloseHandle(h);
}

void reload() {
    _table.clear();
    std::wstring path = macroFilePath();
    if (!fileExists(path)) {
        createSampleFile(path);
    }

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD size = GetFileSize(h, nullptr);
    if (size == 0) {
        CloseHandle(h);
        createSampleFile(path);
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        size = GetFileSize(h, nullptr);
    }
    if (size == 0 || size == INVALID_FILE_SIZE) {
        CloseHandle(h);
        return;
    }

    std::vector<BYTE> data(size);
    DWORD readBytes = 0;
    if (!ReadFile(h, data.data(), size, &readBytes, nullptr) || readBytes == 0) {
        CloseHandle(h);
        return;
    }
    CloseHandle(h);

    _lastModTime = getFileModTime(path);

    // Decode file to UTF-16
    std::wstring text;
    if (readBytes >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        // UTF-16 LE
        size_t wcharCount = (readBytes - 2) / sizeof(wchar_t);
        text.assign((const wchar_t*)(data.data() + 2), wcharCount);
    } else {
        // UTF-8 (with or without BOM)
        size_t offset = 0;
        if (readBytes >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
            offset = 3;
        }
        int len = MultiByteToWideChar(CP_UTF8, 0, (const char*)(data.data() + offset), (int)(readBytes - offset), nullptr, 0);
        if (len > 0) {
            text.resize(len);
            MultiByteToWideChar(CP_UTF8, 0, (const char*)(data.data() + offset), (int)(readBytes - offset), &text[0], len);
        } else {
            // Fallback ANSI
            len = MultiByteToWideChar(CP_ACP, 0, (const char*)(data.data() + offset), (int)(readBytes - offset), nullptr, 0);
            if (len > 0) {
                text.resize(len);
                MultiByteToWideChar(CP_ACP, 0, (const char*)(data.data() + offset), (int)(readBytes - offset), &text[0], len);
            }
        }
    }

    // Parse lines
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find_first_of(L"\r\n", start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        start = (end < text.size() && text[end] == L'\r' && end + 1 < text.size() && text[end + 1] == L'\n') ? end + 2 : end + 1;

        // Trim leading whitespace
        size_t first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos) continue;
        line = line.substr(first);

        // Skip comments or headers
        if (line.empty() || line[0] == L';' || line[0] == L'#' || line.rfind(L"//", 0) == 0 || line.rfind(L"<<", 0) == 0) {
            continue;
        }

        // Search separator: '||' first (Unikey/EVKey format), then ':'
        std::wstring shortcut, repl;
        size_t sep = line.find(L"||");
        if (sep != std::wstring::npos) {
            shortcut = line.substr(0, sep);
            repl = line.substr(sep + 2);
        } else {
            sep = line.find(L':');
            if (sep != std::wstring::npos) {
                shortcut = line.substr(0, sep);
                repl = line.substr(sep + 1);
            }
        }

        if (sep == std::wstring::npos) continue;

        // Trim shortcut
        size_t sEnd = shortcut.find_last_not_of(L" \t");
        if (sEnd != std::wstring::npos) shortcut = shortcut.substr(0, sEnd + 1);
        if (shortcut.empty()) continue;

        // Unescape \n and \t in replacement if any
        std::wstring unescaped;
        for (size_t i = 0; i < repl.size(); i++) {
            if (repl[i] == L'\\' && i + 1 < repl.size()) {
                if (repl[i + 1] == L'n') { unescaped += L"\r\n"; i++; }
                else if (repl[i + 1] == L't') { unescaped += L"\t"; i++; }
                else { unescaped += repl[i]; }
            } else {
                unescaped += repl[i];
            }
        }

        _table[toLowerStr(shortcut)] = unescaped;
    }
}

void init() {
    _enabled = prefs::getInt(L"UseMacro", 1) != 0;
    reload();
}

bool isEnabled() {
    return _enabled;
}

void setEnabled(bool on) {
    _enabled = on;
    prefs::setInt(L"UseMacro", on ? 1 : 0);
    if (!on) reset();
}

void openMacroFile() {
    std::wstring path = macroFilePath();
    if (!fileExists(path)) {
        createSampleFile(path);
    }
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void onChar(wchar_t c, bool capsLock) {
    if (!_enabled) return;
    if (c == 0 || c < 32) return;
    if (_buffer.empty()) {
        _bufferCapsLock = capsLock;
    }
    if (_buffer.size() < 64) {
        _buffer.push_back(c);
    } else {
        _buffer.clear();
        _bufferCapsLock = false;
    }
}

void onBackspace() {
    if (!_buffer.empty()) {
        _buffer.pop_back();
        if (_buffer.empty()) _bufferCapsLock = false;
    }
}

void reset() {
    _buffer.clear();
    _bufferCapsLock = false;
}

bool isDelimiter(WORD vk, unsigned short engineKey, bool shift) {
    if (engineKey == KEY_SPACE || engineKey == KEY_RETURN || engineKey == KEY_ENTER || engineKey == KEY_TAB) {
        return true;
    }
    if (engineKey == KEY_COMMA || engineKey == KEY_DOT || engineKey == KEY_SEMICOLON ||
        engineKey == KEY_SLASH || engineKey == KEY_QUOTE) {
        return true;
    }
    if (shift && (engineKey == KEY_1 || engineKey == KEY_SLASH || engineKey == KEY_SEMICOLON)) { // !, ?, :
        return true;
    }
    if (vk == VK_SPACE || vk == VK_RETURN || vk == VK_TAB || vk == VK_OEM_COMMA || vk == VK_OEM_PERIOD ||
        vk == VK_OEM_1 || vk == VK_OEM_2 || vk == VK_OEM_7) {
        return true;
    }
    return false;
}

static std::wstring applyCasing(const std::wstring& typed, const std::wstring& templateRepl, bool capsLock) {
    if (typed.empty() || templateRepl.empty()) return templateRepl;

    // Check if typed shortcut is all uppercase letters
    bool allUpper = true;
    bool hasLetter = false;
    for (wchar_t c : typed) {
        if (iswalpha(c)) {
            hasLetter = true;
            if (!iswupper(c)) { allUpper = false; break; }
        }
    }

    if (hasLetter && allUpper) {
        // If single letter: only uppercase entirely if CapsLock was on;
        // if typed with Shift, it's Title Case (e.g. K -> Không)
        if (typed.size() == 1 && !capsLock) {
            std::wstring res = templateRepl;
            CharUpperBuffW(&res[0], 1);
            return res;
        }

        std::wstring res = templateRepl;
        CharUpperBuffW(res.data(), (DWORD)res.size());
        return res;
    }

    // Check if first character is uppercase (Title Case)
    if (iswupper(typed[0])) {
        std::wstring res = templateRepl;
        CharUpperBuffW(&res[0], 1);
        return res;
    }

    return templateRepl;
}

bool match(std::wstring& outReplacement, int& outShortcutLen) {
    if (!_enabled || _buffer.empty()) return false;

    // Hot-reload check on disk modification
    FILETIME currentMod = getFileModTime(macroFilePath());
    if (CompareFileTime(&currentMod, &_lastModTime) != 0) {
        reload();
    }

    std::wstring key = toLowerStr(_buffer);
    auto it = _table.find(key);
    if (it != _table.end()) {
        outShortcutLen = (int)_buffer.size();
        outReplacement = applyCasing(_buffer, it->second, _bufferCapsLock);
        return true;
    }

    return false;
}

size_t count() {
    return _table.size();
}

}} // namespace vikey::macro
