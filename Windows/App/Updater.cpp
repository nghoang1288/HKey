#include "Updater.h"
#include "Prefs.h"
#include "resource.h"
#include <winhttp.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <fstream>
#include <ctime>

#pragma comment(lib, "winhttp.lib")

namespace vikey { namespace updater {

#ifndef HKEY_VERSION
#define HKEY_VERSION "1.0.25"
#endif

#define WIDEN2(x) L ## x
#define WIDEN(x) WIDEN2(x)
#define HKEY_VERSION_W WIDEN(HKEY_VERSION)

#if defined(_M_ARM64)
static const wchar_t* ARCH = L"arm64";
#else
static const wchar_t* ARCH = L"x64";
#endif

static const wchar_t* DEFAULT_REPO = L"nghoang1288/HKey";

struct Release {
    std::wstring version;
    std::wstring url;
    std::wstring releasePage;
    std::wstring notes;
    unsigned long long length = 0;
    bool isInstaller = false;
};

static HWND _owner = nullptr;
static std::atomic<bool> _busy{false};
static bool _manual = false;
static Release _found;
static std::wstring _pendingPath;
static std::wstring _lastError;

// ---- helpers -------------------------------------------------------------------------------------
static std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

/// "1.0.25" vs "1.0.9": numeric per part
static int compareVersions(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && a[i] != L'.') { if (iswdigit(a[i])) x = x * 10 + (a[i] - L'0'); i++; }
        while (j < b.size() && b[j] != L'.') { if (iswdigit(b[j])) y = y * 10 + (b[j] - L'0'); j++; }
        if (x != y) return x < y ? -1 : 1;
        if (i < a.size()) i++;
        if (j < b.size()) j++;
    }
    return 0;
}

static std::string jsonValue(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\":";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    pos += pattern.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) pos++;
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        pos++;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                pos++;
                if (json[pos] == 'n') val += '\n';
                else if (json[pos] == 'r') val += '\r';
                else if (json[pos] == 't') val += '\t';
                else if (json[pos] == '"') val += '"';
                else if (json[pos] == '\\') val += '\\';
                else val += json[pos];
            } else {
                val += json[pos];
            }
            pos++;
        }
        return val;
    } else {
        size_t end = json.find_first_of(",}\r\n", pos);
        if (end == std::string::npos) end = json.size();
        return json.substr(pos, end - pos);
    }
}

// Parse release info from GitHub Releases API response
static bool parseGitHubRelease(const std::string& json, Release& out) {
    std::string tag = jsonValue(json, "tag_name");
    if (tag.empty()) return false;
    if (tag.front() == 'v' || tag.front() == 'V') tag.erase(0, 1);
    out.version = widen(tag);
    out.releasePage = widen(jsonValue(json, "html_url"));
    if (out.releasePage.empty()) {
        std::wstring repo = prefs::getString(L"GitHubRepo", DEFAULT_REPO);
        out.releasePage = L"https://github.com/" + repo + L"/releases";
    }
    out.notes = widen(jsonValue(json, "body"));

    // Find suitable asset in "assets": [...]
    size_t aPos = json.find("\"assets\":");
    std::string archStr = narrow(ARCH);
    if (aPos != std::string::npos) {
        size_t cur = aPos;
        while ((cur = json.find("\"name\":", cur)) != std::string::npos) {
            size_t objStart = json.rfind('{', cur);
            size_t objEnd = json.find('}', cur);
            if (objStart == std::string::npos || objEnd == std::string::npos) break;
            std::string assetObj = json.substr(objStart, objEnd - objStart + 1);
            std::string name = jsonValue(assetObj, "name");
            std::string downloadUrl = jsonValue(assetObj, "browser_download_url");
            std::string sizeStr = jsonValue(assetObj, "size");

            // Look for matching architecture or setup
            bool archMatch = (name.find(archStr) != std::string::npos) || (name.find("Setup.exe") != std::string::npos);
            bool isSetup = (name.find("Setup.exe") != std::string::npos) || (name.find(".exe") != std::string::npos);

            if (archMatch && !downloadUrl.empty()) {
                out.url = widen(downloadUrl);
                out.length = _strtoui64(sizeStr.c_str(), nullptr, 10);
                out.isInstaller = isSetup;
                if (isSetup) break; // Prefer installer
            }
            cur = objEnd + 1;
        }
    }
    return true;
}

// ---- HTTP (WinHTTP) ------------------------------------------------------------------------------
static bool httpGet(const std::wstring& url, std::vector<unsigned char>& out, int& statusCode, unsigned long long maxBytes = 200ull << 20) {
    out.clear();
    statusCode = 0;
    URL_COMPONENTS uc = {sizeof(uc)};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    HINTERNET s = WinHttpOpen(L"HKey/" HKEY_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (r) {
        std::wstring headers = L"Accept: application/vnd.github.v3+json\r\n";
        if (WinHttpSendRequest(r, headers.c_str(), (DWORD)headers.size(), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = 0, sl = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl, WINHTTP_NO_HEADER_INDEX);
            statusCode = (int)status;
            if (status == 200) {
                ok = true;
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(r, &avail) && avail) {
                    size_t at = out.size();
                    out.resize(at + avail);
                    DWORD got = 0;
                    if (!WinHttpReadData(r, out.data() + at, avail, &got)) { ok = false; break; }
                    out.resize(at + got);
                    if (out.size() > maxBytes) { ok = false; break; }
                }
            } else if (status == 404) {
                ok = true; // No releases published yet
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

static std::wstring updatesDir() {
    std::wstring d = prefs::appDataDir() + L"\\updates";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static std::wstring installerPath(const Release& r) {
    return updatesDir() + L"\\HKey-" + r.version + L"-" + ARCH + L"-Setup.exe";
}

static void rememberPending(const std::wstring& path, const std::wstring& version) {
    prefs::setString(L"PendingUpdatePath", path.c_str());
    prefs::setString(L"PendingUpdateVersion", version.c_str());
    _pendingPath = path;
}

static void forgetPending() {
    prefs::remove(L"PendingUpdatePath");
    prefs::remove(L"PendingUpdateVersion");
    _pendingPath.clear();
}

static bool download(const Release& r) {
    if (r.url.empty()) return false;
    std::wstring path = installerPath(r);
    std::vector<unsigned char> file;
    int code = 0;
    if (!httpGet(r.url, file, code) || code != 200) {
        _lastError = L"Không tải được bản cập nhật từ GitHub.";
        return false;
    }
    if (r.length && file.size() != r.length) {
        _lastError = L"File tải về không đúng kích thước.";
        return false;
    }
    std::ofstream f(path.c_str(), std::ios::binary);
    f.write((const char*)file.data(), file.size());
    f.close();
    if (!f) {
        _lastError = L"Không ghi được file cập nhật.";
        return false;
    }
    rememberPending(path, r.version);
    return true;
}

static bool runInstaller(const std::wstring& path, bool silent) {
    std::wstring args = std::wstring(silent ? L"/VERYSILENT" : L"/SILENT") + L" /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /RELAUNCH=1";
    HINSTANCE h = ShellExecuteW(nullptr, L"open", path.c_str(), args.c_str(), nullptr, silent ? SW_HIDE : SW_SHOW);
    return (INT_PTR)h > 32;
}

// ---- checks ---------------------------------------------------------------------------------------
static void checkThread(bool manual) {
    std::wstring repo = prefs::getString(L"GitHubRepo", DEFAULT_REPO);
    std::wstring apiUrl = L"https://api.github.com/repos/" + repo + L"/releases/latest";
    std::vector<unsigned char> data;
    int statusCode = 0;
    Event ev = EvFailed;

    if (httpGet(apiUrl, data, statusCode, 4 << 20)) {
        prefs::setInt(L"LastUpdateCheck", (int)time(nullptr));
        if (statusCode == 404 || data.empty()) {
            ev = EvUpToDate;
        } else {
            std::string json(data.begin(), data.end());
            Release r;
            if (!parseGitHubRelease(json, r) || compareVersions(r.version, widen(HKEY_VERSION)) <= 0) {
                ev = EvUpToDate;
            } else {
                _found = r;
                if (manual) ev = EvAvailable;
                else ev = (r.isInstaller && download(r)) ? EvDownloaded : EvFailed;
            }
        }
    } else {
        _lastError = L"Không kiểm tra được cập nhật. Kiểm tra kết nối mạng rồi thử lại.";
    }
    _manual = manual;
    _busy = false;
    PostMessageW(_owner, WM_UPDATER, ev, 0);
}

static void check(bool manual) {
    if (_busy.exchange(true)) return;
    std::thread(checkThread, manual).detach();
}

void onEvent(WPARAM ev) {
    if (!_manual) return;
    std::wstring cur = widen(HKEY_VERSION);
    switch (ev) {
    case EvUpToDate:
        MessageBoxW(nullptr, (L"Bạn đang dùng bản mới nhất của HKey (" + cur + L").").c_str(), L"HKey", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        break;
    case EvFailed:
        MessageBoxW(nullptr, _lastError.c_str(), L"HKey", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        break;
    case EvAvailable: {
        std::wstring head = L"HKey " + _found.version + L" đã có", body = L"Bạn đang dùng " + cur + L".";
        if (!_found.notes.empty()) body += L"\n\n" + _found.notes;
        if (_found.isInstaller) {
            body += L"\n\nHKey sẽ tải về, cài đặt và tự mở lại.";
        } else {
            body += L"\n\nBấm Cập nhật để mở trang tải bản mới trên GitHub.";
        }

        TASKDIALOG_BUTTON btns[] = {{IDYES, L"Cập nhật"}, {IDNO, L"Để sau"}};
        TASKDIALOGCONFIG cfg = {sizeof(cfg)}; cfg.hInstance = GetModuleHandleW(nullptr);
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        cfg.pszWindowTitle = L"HKey"; cfg.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
        cfg.pszMainInstruction = head.c_str(); cfg.pszContent = body.c_str();
        cfg.pButtons = btns; cfg.cButtons = 2; cfg.nDefaultButton = IDYES;
        int pressed = 0; TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
        if (pressed != IDYES) break;

        if (_found.isInstaller) {
            _busy = true;
            std::thread([] {
                bool ok = download(_found);
                _busy = false; _manual = true;
                PostMessageW(_owner, WM_UPDATER, ok ? EvDownloaded : EvFailed, 0);
            }).detach();
        } else {
            ShellExecuteW(nullptr, L"open", _found.releasePage.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break; }
    case EvDownloaded:
        if (runInstaller(_pendingPath, false)) {
            forgetPending();
            PostMessageW(_owner, WM_CLOSE, 0, 0);
        } else {
            MessageBoxW(nullptr, L"Không chạy được trình cài đặt.", L"HKey", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        }
        break;
    }
}

static const UINT_PTR TIMER_UPDATE = 0x7001;
static void CALLBACK timerProc(HWND, UINT, UINT_PTR, DWORD) {
    if (!prefs::getInt(L"AutoUpdate", 1)) return;
    int last = prefs::getInt(L"LastUpdateCheck", 0);
    if (time(nullptr) - last >= 24 * 3600) check(false);
}

void start(HWND owner) {
    _owner = owner;
    _pendingPath = prefs::getString(L"PendingUpdatePath", L"");
    if (!_pendingPath.empty() && GetFileAttributesW(_pendingPath.c_str()) == INVALID_FILE_ATTRIBUTES) forgetPending();
    SetTimer(owner, TIMER_UPDATE, 60 * 60 * 1000, timerProc);
    std::thread([] {
        Sleep(20000);
        if (prefs::getInt(L"AutoUpdate", 1) && time(nullptr) - prefs::getInt(L"LastUpdateCheck", 0) >= 24 * 3600) check(false);
    }).detach();
}

void checkNow(HWND owner) {
    _owner = owner;
    check(true);
}

bool installPendingAndQuit() {
    if (_pendingPath.empty()) return false;
    std::wstring v = prefs::getString(L"PendingUpdateVersion", L"");
    if (compareVersions(v, widen(HKEY_VERSION)) <= 0) {
        DeleteFileW(_pendingPath.c_str());
        forgetPending();
        return false;
    }
    bool ok = runInstaller(_pendingPath, true);
    forgetPending();
    return ok;
}

bool installPendingAtLaunch() {
    std::wstring p = prefs::getString(L"PendingUpdatePath", L""), v = prefs::getString(L"PendingUpdateVersion", L"");
    if (p.empty()) return false;
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES || compareVersions(v, widen(HKEY_VERSION)) <= 0) {
        DeleteFileW(p.c_str());
        forgetPending();
        return false;
    }
    forgetPending();
    return runInstaller(p, true);
}

}} // namespace vikey::updater

namespace hkey { namespace updater {
    using namespace vikey::updater;
}}
