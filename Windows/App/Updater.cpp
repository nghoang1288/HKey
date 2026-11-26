#include "Updater.h"
#include "Prefs.h"
#include "resource.h"
#include <winhttp.h>
#include <shellapi.h>
#include <commctrl.h>
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "bcrypt.lib")

namespace vikey { namespace updater {

#ifndef HKEY_VERSION
#define HKEY_VERSION "1.2.1"
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
    std::string assetName;
    std::wstring url;
    std::wstring sha256Url;
    std::string sha256;       // 64 lowercase hex chars
    std::wstring releasePage;
    std::wstring notes;
    unsigned long long length = 0;
    bool isInstaller = false;
};

static std::atomic<HWND> _owner{nullptr};
static std::atomic<bool> _busy{false};
static std::atomic<bool> _stopping{false};
static std::mutex _stateMu;
static Release _found;
static std::wstring _pendingPath;
static std::wstring _lastError;

// ---- helpers -------------------------------------------------------------------------------------
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

static bool isSha256Hex(const std::string& s) {
    if (s.size() != 64) return false;
    for (unsigned char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

static bool endsWith(const std::string& s, const char* suffix) {
    size_t slen = strlen(suffix);
    return s.size() >= slen && s.compare(s.size() - slen, slen, suffix) == 0;
}

static std::wstring currentExePath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return len > 0 ? std::wstring(buf, len) : L"";
}

static std::wstring dirOfPath(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos != std::wstring::npos ? path.substr(0, pos) : L".";
}

static bool fileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool isInstalledCopy() {
    std::wstring exe = currentExePath();
    if (exe.empty()) return false;
    return fileExists(dirOfPath(exe) + L"\\unins000.exe");
}

static bool isHttpsUrl(const std::string& u) {
    return startsWith(u, "https://");
}

/// "1.0.26" vs "1.0.9": numeric per part
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

static std::wstring sanitizeVersion(const std::wstring& v) {
    std::wstring out;
    out.reserve(v.size());
    for (wchar_t c : v) {
        if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'.' || c == L'_' || c == L'-') {
            out.push_back(c);
        }
    }
    if (out.empty() || out.find(L"..") != std::wstring::npos) return L"";
    return out;
}

static size_t skipJsonWs(const std::string& s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    return i;
}

static std::string parseJsonString(const std::string& s, size_t& pos) {
    if (pos >= s.size() || s[pos] != '"') return "";
    pos++;
    std::string val;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            pos++;
            char c = s[pos];
            if (c == 'n') val += '\n';
            else if (c == 'r') val += '\r';
            else if (c == 't') val += '\t';
            else if (c == '"' || c == '\\' || c == '/') val += c;
            else if (c == 'u' && pos + 4 < s.size()) {
                char hex[5] = {s[pos + 1], s[pos + 2], s[pos + 3], s[pos + 4], 0};
                unsigned cp = (unsigned)strtoul(hex, nullptr, 16);
                pos += 4;
                if (cp < 0x80) val += (char)cp;
                else if (cp < 0x800) { val += (char)(0xC0 | (cp >> 6)); val += (char)(0x80 | (cp & 0x3F)); }
                else { val += (char)(0xE0 | (cp >> 12)); val += (char)(0x80 | ((cp >> 6) & 0x3F)); val += (char)(0x80 | (cp & 0x3F)); }
            } else {
                val += c;
            }
        } else {
            val += s[pos];
        }
        pos++;
    }
    if (pos < s.size() && s[pos] == '"') pos++;
    return val;
}

static size_t skipJsonValue(const std::string& s, size_t pos) {
    pos = skipJsonWs(s, pos);
    if (pos >= s.size()) return pos;
    if (s[pos] == '"') { parseJsonString(s, pos); return pos; }
    if (s[pos] == '{' || s[pos] == '[') {
        char open = s[pos], close = (open == '{') ? '}' : ']';
        int depth = 0; bool inStr = false; bool esc = false;
        for (; pos < s.size(); pos++) {
            char c = s[pos];
            if (esc) { esc = false; continue; }
            if (c == '\\' && inStr) { esc = true; continue; }
            if (c == '"') { inStr = !inStr; continue; }
            if (!inStr) {
                if (c == open) depth++;
                else if (c == close && --depth == 0) return pos + 1;
            }
        }
        return pos;
    }
    while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']' && s[pos] != ' ' && s[pos] != '\r' && s[pos] != '\n' && s[pos] != '\t') pos++;
    return pos;
}

/// Extracts a top-level field from a JSON object `{ ... }`, ignoring nested objects/strings.
static std::string jsonValue(const std::string& obj, const std::string& targetKey) {
    size_t i = skipJsonWs(obj, 0);
    if (i >= obj.size() || obj[i] != '{') return "";
    i++;
    while (i < obj.size()) {
        i = skipJsonWs(obj, i);
        if (i >= obj.size() || obj[i] == '}') break;
        if (obj[i] == ',') { i++; continue; }
        if (obj[i] != '"') break;
        std::string key = parseJsonString(obj, i);
        i = skipJsonWs(obj, i);
        if (i >= obj.size() || obj[i] != ':') break;
        i = skipJsonWs(obj, i + 1);
        if (key == targetKey) {
            if (i < obj.size() && obj[i] == '"') return parseJsonString(obj, i);
            size_t end = skipJsonValue(obj, i);
            return obj.substr(i, end - i);
        }
        i = skipJsonValue(obj, i);
    }
    return "";
}

static size_t findMatchingBrace(const std::string& str, size_t start) {
    int depth = 0;
    bool inString = false;
    bool escape = false;
    for (size_t i = start; i < str.size(); i++) {
        char c = str[i];
        if (escape) { escape = false; continue; }
        if (c == '\\' && inString) { escape = true; continue; }
        if (c == '"') { inString = !inString; continue; }
        if (!inString) {
            if (c == '{') depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) return i;
            }
        }
    }
    return std::string::npos;
}

static std::string extractDigestSha256(const std::string& digest) {
    if (startsWith(digest, "sha256:")) {
        std::string hex = toLowerAscii(digest.substr(7));
        if (isSha256Hex(hex)) return hex;
    }
    return "";
}

static std::string parseSha256Sums(const std::string& body, const std::string& assetName) {
    std::istringstream ss(body);
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.size() < 66) continue;
        std::string hex = toLowerAscii(line.substr(0, 64));
        if (!isSha256Hex(hex)) continue;
        if (line[64] != ' ' && line[64] != '\t') continue;
        size_t pos = 64;
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) pos++;
        if (pos < line.size() && line[pos] == '*') pos++;
        std::string fname = line.substr(pos);
        size_t slash = fname.find_last_of("/\\");
        if (slash != std::string::npos) fname = fname.substr(slash + 1);
        if (fname == assetName) return hex;
    }
    return "";
}

// Parse release info from GitHub Releases API response
static bool parseGitHubRelease(const std::string& json, Release& out) {
    std::string tag = jsonValue(json, "tag_name");
    if (tag.empty()) return false;
    if (tag.front() == 'v' || tag.front() == 'V') tag.erase(0, 1);
    out.version = sanitizeVersion(widen(tag));
    if (out.version.empty()) return false;
    out.releasePage = widen(jsonValue(json, "html_url"));
    if (out.releasePage.empty()) {
        std::wstring repo = prefs::getString(L"GitHubRepo", DEFAULT_REPO);
        out.releasePage = L"https://github.com/" + repo + L"/releases";
    }
    out.notes = widen(jsonValue(json, "body"));

    const bool installed = isInstalledCopy();
    const std::string archStr = narrow(ARCH);
    int bestRank = 0;

    std::string assetsJson = jsonValue(json, "assets");
    if (!assetsJson.empty() && assetsJson.front() == '[') {
        size_t cur = 1;
        while (cur < assetsJson.size()) {
            size_t objStart = assetsJson.find('{', cur);
            if (objStart == std::string::npos) break;
            size_t objEnd = findMatchingBrace(assetsJson, objStart);
            if (objEnd == std::string::npos) break;
            std::string assetObj = assetsJson.substr(objStart, objEnd - objStart + 1);
            std::string name = jsonValue(assetObj, "name");
            std::string downloadUrl = jsonValue(assetObj, "browser_download_url");
            std::string sizeStr = jsonValue(assetObj, "size");
            std::string digestSha = extractDigestSha256(jsonValue(assetObj, "digest"));

            if (isHttpsUrl(downloadUrl)) {
                if (name == "SHA256SUMS.txt") {
                    out.sha256Url = widen(downloadUrl);
                } else {
                    bool archMatch = (name.find(archStr) != std::string::npos);
                    bool isSetup = endsWith(name, "-Setup.exe") || (name.find("Setup.exe") != std::string::npos);
                    bool isExe = endsWith(name, ".exe");

                    int rank = 0;
                    bool candidateInstaller = false;
                    if (installed) {
                        if (archMatch && isSetup) { rank = 2; candidateInstaller = true; }
                    } else {
                        if (archMatch && isExe && !isSetup) { rank = 2; candidateInstaller = false; }
                        else if (archStr == "x64" && name == "HKey.exe") { rank = 1; candidateInstaller = false; }
                    }

                    if (rank > bestRank) {
                        bestRank = rank;
                        out.assetName = name;
                        out.url = widen(downloadUrl);
                        out.length = _strtoui64(sizeStr.c_str(), nullptr, 10);
                        out.sha256 = digestSha;
                        out.isInstaller = candidateInstaller;
                    }
                }
            }
            cur = objEnd + 1;
        }
    }
    return !out.url.empty();
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
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return false;

    HINTERNET s = WinHttpOpen(L"HKey/" HKEY_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;

    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
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
                    if (at + avail > maxBytes) { ok = false; break; }
                    out.resize(at + avail);
                    DWORD got = 0;
                    if (!WinHttpReadData(r, out.data() + at, avail, &got)) { ok = false; break; }
                    out.resize(at + got);
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

// ---- Integrity & Paths ---------------------------------------------------------------------------

static std::string sha256Hex(const unsigned char* data, size_t len) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objLen = 0, hashLen = 0, cbData = 0;
    std::string hex;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) goto cleanup;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cbData, 0) < 0) goto cleanup;
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &cbData, 0) < 0) goto cleanup;
    if (hashLen != 32) goto cleanup;

    {
        std::vector<unsigned char> hashObj(objLen);
        std::vector<unsigned char> digest(hashLen);
        if (BCryptCreateHash(alg, &hash, hashObj.data(), objLen, nullptr, 0, 0) < 0) goto cleanup;
        if (len > 0 && BCryptHashData(hash, (PUCHAR)data, (ULONG)len, 0) < 0) goto cleanup;
        if (BCryptFinishHash(hash, digest.data(), hashLen, 0) < 0) goto cleanup;

        static const char digits[] = "0123456789abcdef";
        hex.reserve(64);
        for (unsigned char b : digest) {
            hex.push_back(digits[(b >> 4) & 0xF]);
            hex.push_back(digits[b & 0xF]);
        }
    }

cleanup:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

static bool resolveExpectedSha256(Release& r, std::wstring& errOut) {
    std::string expected = toLowerAscii(r.sha256);
    if (!r.sha256Url.empty()) {
        std::vector<unsigned char> sumsData;
        int code = 0;
        if (httpGet(r.sha256Url, sumsData, code, 64 * 1024) && code == 200 && !sumsData.empty()) {
            std::string body(sumsData.begin(), sumsData.end());
            std::string fromSums = parseSha256Sums(body, r.assetName);
            if (!fromSums.empty()) {
                if (!expected.empty() && expected != fromSums) {
                    errOut = L"Mã kiểm tra toàn vẹn SHA-256 trên GitHub không khớp nhau.";
                    return false;
                }
                expected = fromSums;
            }
        }
    }
    if (!isSha256Hex(expected)) {
        errOut = L"Bản phát hành thiếu mã xác thực toàn vẹn SHA-256.";
        return false;
    }
    r.sha256 = expected;
    return true;
}

static std::wstring updatesDir() {
    std::wstring d = prefs::appDataDir() + L"\\updates";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static std::wstring stagedFilePath(const Release& r) {
    std::wstring suffix = r.isInstaller ? L"-Setup.exe" : L".exe";
    return updatesDir() + L"\\HKey-" + r.version + L"-" + ARCH + suffix;
}

static bool isPathInUpdatesDir(const std::wstring& path) {
    std::wstring uDir = updatesDir();
    wchar_t fullPath[MAX_PATH] = {};
    wchar_t fullUDir[MAX_PATH] = {};
    if (!GetFullPathNameW(path.c_str(), MAX_PATH, fullPath, nullptr)) return false;
    if (!GetFullPathNameW(uDir.c_str(), MAX_PATH, fullUDir, nullptr)) return false;

    std::wstring p(fullPath);
    std::wstring d(fullUDir);
    std::transform(p.begin(), p.end(), p.begin(), ::towlower);
    std::transform(d.begin(), d.end(), d.begin(), ::towlower);
    if (d.empty()) return false;
    if (d.back() != L'\\') d += L'\\';
    return p.rfind(d, 0) == 0;
}

static bool verifySignatureNotTampered(const std::wstring& filePath) {
    WINTRUST_FILE_INFO fileData = {};
    fileData.cbStruct = sizeof(WINTRUST_FILE_INFO);
    fileData.pcwszFilePath = filePath.c_str();

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA winTrustData = {};
    winTrustData.cbStruct = sizeof(WINTRUST_DATA);
    winTrustData.dwUIChoice = WTD_UI_NONE;
    winTrustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    winTrustData.dwUnionChoice = WTD_CHOICE_FILE;
    winTrustData.pFile = &fileData;
    winTrustData.dwStateAction = WTD_STATEACTION_VERIFY;
    winTrustData.dwProvFlags = WTD_SAFER_FLAG;

    LONG status = WinVerifyTrust(nullptr, &action, &winTrustData);
    winTrustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &winTrustData);

    return status != TRUST_E_BAD_DIGEST &&
           status != TRUST_E_EXPLICIT_DISTRUST &&
           status != CERT_E_REVOKED &&
           status != CERT_E_EXPIRED;
}

static bool verifyPeAndHash(const unsigned char* data, size_t size, const std::string& expectedSha256) {
    if (!data || size < 1024) return false;
    if (data[0] != 'M' || data[1] != 'Z') return false;
    if (!isSha256Hex(expectedSha256)) return false;
    return sha256Hex(data, size) == toLowerAscii(expectedSha256);
}

static bool verifyStagedFileOnDisk(const std::wstring& path, const std::string& expectedSha256, std::wstring& errOut) {
    if (!isPathInUpdatesDir(path)) {
        errOut = L"Đường dẫn file cập nhật không hợp lệ.";
        DeleteFileW(path.c_str());
        return false;
    }
    std::ifstream in(path.c_str(), std::ios::binary | std::ios::ate);
    if (!in) {
        errOut = L"Không đọc được file cập nhật đã tải.";
        return false;
    }
    std::streamsize sz = in.tellg();
    if (sz < 1024 || sz > (std::streamsize)(200ull << 20)) {
        in.close();
        DeleteFileW(path.c_str());
        errOut = L"Kích thước file cập nhật không hợp lệ.";
        return false;
    }
    in.seekg(0, std::ios::beg);
    std::vector<unsigned char> buf((size_t)sz);
    if (!in.read((char*)buf.data(), sz)) {
        in.close();
        errOut = L"Lỗi khi đọc file cập nhật.";
        return false;
    }
    in.close();

    if (!verifyPeAndHash(buf.data(), buf.size(), expectedSha256)) {
        DeleteFileW(path.c_str());
        errOut = L"File cập nhật không khớp mã SHA-256 (có thể hỏng hoặc bị thay đổi).";
        return false;
    }
    if (!verifySignatureNotTampered(path)) {
        DeleteFileW(path.c_str());
        errOut = L"Chữ ký số của file cập nhật bị hỏng hoặc không đáng tin.";
        return false;
    }
    return true;
}

static void rememberPending(const std::wstring& path, const Release& r) {
    prefs::setString(L"PendingUpdatePath", path.c_str());
    prefs::setString(L"PendingUpdateVersion", r.version.c_str());
    prefs::setString(L"PendingUpdateSha256", widen(r.sha256).c_str());
    prefs::setInt(L"PendingUpdateIsInstaller", r.isInstaller ? 1 : 0);
    std::lock_guard<std::mutex> lk(_stateMu);
    _pendingPath = path;
}

static void forgetPending() {
    prefs::remove(L"PendingUpdatePath");
    prefs::remove(L"PendingUpdateVersion");
    prefs::remove(L"PendingUpdateSha256");
    prefs::remove(L"PendingUpdateIsInstaller");
    std::lock_guard<std::mutex> lk(_stateMu);
    _pendingPath.clear();
}

static bool download(Release& r, std::wstring& errOut) {
    if (r.url.empty()) {
        errOut = L"Không tìm thấy đường dẫn tải bản cập nhật.";
        return false;
    }
    if (!resolveExpectedSha256(r, errOut)) return false;

    std::vector<unsigned char> file;
    int code = 0;
    if (!httpGet(r.url, file, code) || code != 200) {
        errOut = L"Không tải được bản cập nhật từ GitHub.";
        return false;
    }
    if (r.length && file.size() != r.length) {
        errOut = L"File tải về không đúng kích thước.";
        return false;
    }
    if (!verifyPeAndHash(file.data(), file.size(), r.sha256)) {
        errOut = L"File tải về không vượt qua kiểm tra toàn vẹn SHA-256.";
        return false;
    }

    std::wstring path = stagedFilePath(r);
    if (!isPathInUpdatesDir(path)) {
        errOut = L"Đường dẫn file cập nhật không hợp lệ.";
        return false;
    }
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    f.write((const char*)file.data(), file.size());
    f.close();
    if (!f) {
        DeleteFileW(path.c_str());
        errOut = L"Không ghi được file cập nhật.";
        return false;
    }
    if (!verifySignatureNotTampered(path)) {
        DeleteFileW(path.c_str());
        errOut = L"Chữ ký số của file cài đặt bị hỏng hoặc bị can thiệp.";
        return false;
    }
    rememberPending(path, r);
    return true;
}

static bool canWriteToDir(const std::wstring& dir) {
    std::wstring probe = dir + L"\\.hkey_write_test_" + std::to_wstring(GetCurrentProcessId());
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static bool spawnProcess(const std::wstring& exePath, const std::wstring& cmdArgs, const std::wstring& workDir) {
    std::wstring cmdLine = L"\"" + exePath + L"\"";
    if (!cmdArgs.empty()) cmdLine += L" " + cmdArgs;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exePath.c_str(), &cmdLine[0], nullptr, nullptr, FALSE, 0, nullptr,
                        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

static bool launchPendingUpdate(const std::wstring& path, const std::string& expectedSha256, bool isInstaller,
                                bool silent, bool relaunch, std::wstring& errOut) {
    if (!verifyStagedFileOnDisk(path, expectedSha256, errOut)) return false;

    if (isInstaller) {
        std::wstring args = std::wstring(silent ? L"/VERYSILENT" : L"/SILENT") +
                            L" /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS" +
                            (relaunch ? L" /RELAUNCH=1" : L"");
        HINSTANCE h = ShellExecuteW(nullptr, L"open", path.c_str(), args.c_str(), nullptr, silent ? SW_HIDE : SW_SHOW);
        if ((INT_PTR)h <= 32) {
            errOut = L"Không chạy được trình cài đặt.";
            return false;
        }
        return true;
    }

    std::wstring targetExe = currentExePath();
    if (targetExe.empty()) {
        errOut = L"Không xác định được đường dẫn HKey hiện tại.";
        return false;
    }
    std::wstring targetDir = dirOfPath(targetExe);
    std::wstring args = L"--apply-update \"" + targetExe + L"\" " +
                        std::to_wstring(GetCurrentProcessId()) + L" " + (relaunch ? L"1" : L"0");

    if (canWriteToDir(targetDir)) {
        if (spawnProcess(path, args, targetDir)) return true;
    }
    HINSTANCE h = ShellExecuteW(nullptr, L"runas", path.c_str(), args.c_str(), targetDir.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32) {
        errOut = L"Không chạy được tiến trình cập nhật bản Portable.";
        return false;
    }
    return true;
}

static int applyStagedPortableUpdate(const std::wstring& targetExe, DWORD oldPid, bool relaunch) {
    if (targetExe.empty()) return 1;

    if (oldPid != 0 && oldPid != GetCurrentProcessId()) {
        HANDLE hOld = OpenProcess(SYNCHRONIZE, FALSE, oldPid);
        if (hOld) {
            WaitForSingleObject(hOld, 15000);
            CloseHandle(hOld);
        }
    }

    std::wstring selfPath = currentExePath();
    std::wstring backupPath = targetExe + L".old";
    bool replaced = false;

    for (int attempt = 0; attempt < 30; attempt++) {
        if (fileExists(targetExe)) {
            if (!MoveFileExW(targetExe.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                Sleep(150);
                continue;
            }
        }
        if (CopyFileW(selfPath.c_str(), targetExe.c_str(), FALSE)) {
            replaced = true;
            break;
        }
        Sleep(150);
    }

    if (!replaced) {
        if (fileExists(backupPath) && !fileExists(targetExe)) {
            MoveFileExW(backupPath.c_str(), targetExe.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        return 1;
    }

    forgetPending();
    if (relaunch) {
        std::wstring cleanupArgs = L"--cleanup-update \"" + selfPath + L"\"";
        spawnProcess(targetExe, cleanupArgs, dirOfPath(targetExe));
    } else {
        DeleteFileW(backupPath.c_str());
    }
    return 0;
}

int handleUpdateCommandLine(LPWSTR) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return -1;

    int rc = -1;
    if (argc >= 5 && wcscmp(argv[1], L"--apply-update") == 0) {
        std::wstring targetExe = argv[2];
        DWORD oldPid = wcstoul(argv[3], nullptr, 10);
        bool relaunch = wcstoul(argv[4], nullptr, 10) != 0;
        rc = applyStagedPortableUpdate(targetExe, oldPid, relaunch);
    } else if (argc >= 3 && wcscmp(argv[1], L"--cleanup-update") == 0) {
        std::wstring stagedPath = argv[2];
        std::wstring backupPath = currentExePath() + L".old";
        for (int i = 0; i < 10; i++) {
            Sleep(150);
            bool bDone = !fileExists(backupPath) || DeleteFileW(backupPath.c_str());
            bool sDone = !isPathInUpdatesDir(stagedPath) || !fileExists(stagedPath) || DeleteFileW(stagedPath.c_str());
            if (bDone && sDone) break;
        }
        forgetPending();
    }

    LocalFree(argv);
    return rc;
}

// ---- checks ---------------------------------------------------------------------------------------
static long long getLastUpdateCheck() {
    std::wstring s = prefs::getString(L"LastUpdateCheck64", L"");
    if (!s.empty()) return std::wcstoll(s.c_str(), nullptr, 10);
    return (long long)prefs::getInt(L"LastUpdateCheck", 0);
}

static void setLastUpdateCheck(long long ts) {
    prefs::setString(L"LastUpdateCheck64", std::to_wstring(ts));
}

static void cleanupStaleUpdates(const std::wstring& keepPath) {
    std::wstring dir = updatesDir();
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\HKey-*.exe").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (!keepPath.empty() && _wcsicmp(full.c_str(), keepPath.c_str()) == 0) continue;
        DeleteFileW(full.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void checkThread(bool manual) {
    std::wstring repo = prefs::getString(L"GitHubRepo", DEFAULT_REPO);
    std::wstring apiUrl = L"https://api.github.com/repos/" + repo + L"/releases/latest";
    std::vector<unsigned char> data;
    int statusCode = 0;
    Event ev = EvFailed;
    std::wstring err;
    Release foundRel;

    if (httpGet(apiUrl, data, statusCode, 4 << 20)) {
        setLastUpdateCheck((long long)time(nullptr));
        if (statusCode == 404 || data.empty()) {
            ev = EvUpToDate;
        } else {
            std::string json(data.begin(), data.end());
            if (!parseGitHubRelease(json, foundRel)) {
                err = L"Không tìm thấy bản phát hành phù hợp cho kiến trúc máy của bạn.";
                ev = EvFailed;
            } else if (compareVersions(foundRel.version, widen(HKEY_VERSION)) <= 0) {
                ev = EvUpToDate;
            } else if (manual) {
                ev = EvAvailable;
            } else {
                ev = download(foundRel, err) ? EvDownloaded : EvFailed;
            }
        }
    } else {
        err = L"Không kiểm tra được cập nhật. Kiểm tra kết nối mạng rồi thử lại.";
    }

    {
        std::lock_guard<std::mutex> lk(_stateMu);
        if (ev == EvAvailable || ev == EvDownloaded) _found = foundRel;
        _lastError = err;
    }
    _busy = false;
    HWND owner = _owner.load(std::memory_order_relaxed);
    if (!_stopping.load(std::memory_order_relaxed) && owner && IsWindow(owner)) {
        PostMessageW(owner, WM_UPDATER, ev, manual ? 1 : 0);
    }
}

static void check(bool manual) {
    if (_stopping.load(std::memory_order_relaxed) || _busy.exchange(true)) return;
    std::thread(checkThread, manual).detach();
}

static bool applyAndClose(const Release& rel, bool silent) {
    std::wstring path;
    {
        std::lock_guard<std::mutex> lk(_stateMu);
        path = _pendingPath;
    }
    if (path.empty()) path = prefs::getString(L"PendingUpdatePath", L"");
    std::string sha = rel.sha256;
    if (sha.empty()) sha = narrow(prefs::getString(L"PendingUpdateSha256", L""));
    bool isInst = rel.isInstaller;
    if (prefs::has(L"PendingUpdateIsInstaller")) isInst = prefs::getInt(L"PendingUpdateIsInstaller", 0) != 0;

    std::wstring err;
    if (launchPendingUpdate(path, sha, isInst, silent, true, err)) {
        forgetPending();
        HWND owner = _owner.load(std::memory_order_relaxed);
        if (owner && IsWindow(owner)) PostMessageW(owner, WM_CLOSE, 0, 0);
        return true;
    }
    MessageBoxW(nullptr, err.empty() ? L"Không chạy được bản cập nhật." : err.c_str(), L"HKey", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    return false;
}

void onEvent(WPARAM ev, LPARAM lp) {
    bool manual = (lp != 0);
    std::wstring cur = widen(HKEY_VERSION);
    Release rel;
    std::wstring err;
    {
        std::lock_guard<std::mutex> lk(_stateMu);
        rel = _found;
        err = _lastError;
    }

    if (!manual) {
        if (ev != EvDownloaded) return;
        if (prefs::getString(L"LastPromptedUpdateVersion", L"") == rel.version) return;
        prefs::setString(L"LastPromptedUpdateVersion", rel.version.c_str());

        std::wstring head = L"HKey " + rel.version + L" đã sẵn sàng";
        std::wstring body = L"Bạn đang dùng " + cur + L". HKey đã tải về và xác thực toàn vẹn SHA-256 cho bản mới.";
        if (!rel.notes.empty()) body += L"\n\n" + rel.notes;
        body += L"\n\nCập nhật ngay để tự thay thế và khởi động lại HKey, hoặc để đến khi thoát app.";

        TASKDIALOG_BUTTON btns[] = {{IDYES, L"Cập nhật ngay"}, {IDNO, L"Để khi thoát"}};
        TASKDIALOGCONFIG cfg = {sizeof(cfg)}; cfg.hInstance = GetModuleHandleW(nullptr);
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        cfg.pszWindowTitle = L"HKey"; cfg.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
        cfg.pszMainInstruction = head.c_str(); cfg.pszContent = body.c_str();
        cfg.pButtons = btns; cfg.cButtons = 2; cfg.nDefaultButton = IDYES;
        int pressed = 0;
        TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
        if (pressed == IDYES) applyAndClose(rel, true);
        return;
    }

    switch (ev) {
    case EvUpToDate:
        MessageBoxW(nullptr, (L"Bạn đang dùng bản mới nhất của HKey (" + cur + L").").c_str(), L"HKey", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        break;
    case EvFailed:
        MessageBoxW(nullptr, err.c_str(), L"HKey", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        break;
    case EvAvailable: {
        std::wstring head = L"HKey " + rel.version + L" đã có";
        std::wstring body = L"Bạn đang dùng " + cur + L".";
        if (!rel.notes.empty()) body += L"\n\n" + rel.notes;
        body += L"\n\nHKey sẽ tải về, xác thực SHA-256, cài đặt và tự mở lại.";

        TASKDIALOG_BUTTON btns[] = {{IDYES, L"Cập nhật"}, {IDNO, L"Để sau"}};
        TASKDIALOGCONFIG cfg = {sizeof(cfg)}; cfg.hInstance = GetModuleHandleW(nullptr);
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        cfg.pszWindowTitle = L"HKey"; cfg.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
        cfg.pszMainInstruction = head.c_str(); cfg.pszContent = body.c_str();
        cfg.pButtons = btns; cfg.cButtons = 2; cfg.nDefaultButton = IDYES;
        int pressed = 0; TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
        if (pressed != IDYES) break;

        if (!rel.url.empty()) {
            if (_busy.exchange(true)) break;
            std::thread([rel]() mutable {
                std::wstring dlErr;
                bool ok = download(rel, dlErr);
                {
                    std::lock_guard<std::mutex> lk(_stateMu);
                    if (ok) _found = rel;
                    _lastError = dlErr;
                }
                _busy = false;
                HWND owner = _owner.load(std::memory_order_relaxed);
                if (!_stopping.load(std::memory_order_relaxed) && owner && IsWindow(owner)) {
                    PostMessageW(owner, WM_UPDATER, ok ? EvDownloaded : EvFailed, 1);
                }
            }).detach();
        } else {
            ShellExecuteW(nullptr, L"open", rel.releasePage.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break; }
    case EvDownloaded:
        applyAndClose(rel, false);
        break;
    }
}

static const UINT_PTR TIMER_UPDATE = 0x7001;
static void CALLBACK timerProc(HWND, UINT, UINT_PTR, DWORD) {
    if (!prefs::getInt(L"AutoUpdate", 1)) return;
    long long last = getLastUpdateCheck();
    if ((long long)time(nullptr) - last >= 24LL * 3600) check(false);
}

void start(HWND owner) {
    _stopping.store(false, std::memory_order_relaxed);
    _owner.store(owner, std::memory_order_relaxed);
    std::wstring staleOld = currentExePath() + L".old";
    if (fileExists(staleOld)) DeleteFileW(staleOld.c_str());

    std::wstring pending = prefs::getString(L"PendingUpdatePath", L"");
    if (!pending.empty() && !fileExists(pending)) {
        forgetPending();
        pending.clear();
    } else {
        std::lock_guard<std::mutex> lk(_stateMu);
        _pendingPath = pending;
    }
    cleanupStaleUpdates(pending);

    SetTimer(owner, TIMER_UPDATE, 60 * 60 * 1000, timerProc);
    std::thread([] {
        Sleep(20000);
        if (_stopping.load(std::memory_order_relaxed)) return;
        if (prefs::getInt(L"AutoUpdate", 1) && (long long)time(nullptr) - getLastUpdateCheck() >= 24LL * 3600) check(false);
    }).detach();
}

void checkNow(HWND owner) {
    _owner.store(owner, std::memory_order_relaxed);
    check(true);
}

static bool installPending(bool relaunch) {
    std::wstring p = prefs::getString(L"PendingUpdatePath", L"");
    if (p.empty()) {
        std::lock_guard<std::mutex> lk(_stateMu);
        p = _pendingPath;
    }
    if (p.empty()) return false;

    std::wstring v = prefs::getString(L"PendingUpdateVersion", L"");
    std::string sha = narrow(prefs::getString(L"PendingUpdateSha256", L""));
    bool isInst = prefs::has(L"PendingUpdateIsInstaller")
        ? (prefs::getInt(L"PendingUpdateIsInstaller", 0) != 0)
        : endsWith(narrow(p), "-Setup.exe");

    if (!fileExists(p) || compareVersions(v, widen(HKEY_VERSION)) <= 0) {
        DeleteFileW(p.c_str());
        forgetPending();
        return false;
    }

    std::wstring err;
    if (launchPendingUpdate(p, sha, isInst, true, relaunch, err)) {
        forgetPending();
        return true;
    }
    return false;
}

bool installPendingAndQuit() {
    return installPending(false);
}

bool installPendingAtLaunch() {
    return installPending(true);
}

}} // namespace vikey::updater

namespace hkey { namespace updater {
    using namespace vikey::updater;
}}
