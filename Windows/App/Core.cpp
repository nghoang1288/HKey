#include "Core.h"
#include "AutoCap.h"
#include "Site.h"
#include "KeyMap.h"
#include "Prefs.h"
#include "Macro.h"
#include "Engine.h"
#include <imm.h>
#ifndef IMC_GETOPENSTATUS
#define IMC_GETOPENSTATUS 0x0005   // lives in immdev.h in recent SDKs
#endif
#include <psapi.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include "resource.h"
#include <vector>
#include <string>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <map>
#include <mutex>
#include <thread>
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")

// Settings the engine reads (see Engine.h). Same defaults as the macOS app.
int vLanguage = 1, vSwitchKeyStatus = 0x7A000206, vCheckSpelling = 1, vUseModernOrthography = 1;
int vRestoreIfWrongSpelling = 1, vFixRecommendBrowser = 1, vUseSmartSwitchKey = 1, vOtherLanguage = 1;
int vAutoSwitchByTyping = 1, vPerformLayoutCompat = 0;
static int vTerminalDelayEnabled = 1;
static int vTerminalDelayMs = 15;
static int vAutoCapitalize = 1;
static vikey::AutoCapitalizeHelper _autoCapHelper;
static int vEmojiHotkey = 0;
static std::atomic<bool> vEmojiOpen{false};
static std::atomic<HWND> vEmojiWindow{nullptr};

namespace vikey { namespace core {

static void PlaySwitchSound(int language) {
    static const void* data[2] = {nullptr, nullptr};
    int i = language == 1 ? 1 : 0;
    if (!data[i]) {
        HRSRC r = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(i ? IDR_SOUND_VI : IDR_SOUND_EN), RT_RCDATA);
        HGLOBAL g = r ? LoadResource(GetModuleHandleW(nullptr), r) : nullptr;
        data[i] = g ? LockResource(g) : nullptr;
    }
    if (!data[i]) { MessageBeep(MB_OK); return; }
    PlaySoundW((LPCWSTR)data[i], nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// Marks keystrokes HKey posts itself, so the hook can skip them (dwExtraInfo).
static const ULONG_PTR VIKEY_EVENT_TAG = 0x0F341991;
static const int EMPTY_HOTKEY = 0xFE0000FE;

// Separate L/R modifier bits so releasing LShift while holding RShift preserves F_SHIFT.
enum : unsigned {
    F_LSHIFT   = 1u << 0,
    F_RSHIFT   = 1u << 1,
    F_LCONTROL = 1u << 2,
    F_RCONTROL = 1u << 3,
    F_LALT     = 1u << 4,
    F_RALT     = 1u << 5,
    F_LWIN     = 1u << 6,
    F_RWIN     = 1u << 7,
    F_CAPS     = 1u << 8,
    F_SHIFT    = F_LSHIFT | F_RSHIFT,
    F_CONTROL  = F_LCONTROL | F_RCONTROL,
    F_ALT      = F_LALT | F_RALT,
    F_WIN      = F_LWIN | F_RWIN,
};
#define OTHER_CONTROL_KEY ((_flag & (F_CONTROL | F_ALT | F_WIN)) != 0)

// Dedicated hook thread + message-only window so UI/Disk/IPC never stall WH_KEYBOARD_LL.
static std::thread _hookThread;
static std::atomic<DWORD> _hookThreadId{0};
static std::atomic<HWND> _hookHwnd{nullptr};
static std::atomic<bool> _hookRunning{false};
static HANDLE _hookReadyEvent = nullptr;

static const UINT WM_HOOK_SET_LANG        = WM_APP + 101;
static const UINT WM_HOOK_SET_LANG_QUIET  = WM_APP + 102;
static const UINT WM_HOOK_RELOAD_SETTINGS = WM_APP + 103;
static const UINT WM_HOOK_SITE_CHANGED    = WM_APP + 104;
static const UINT WM_HOOK_RESET_TYPING    = WM_APP + 105;
static const UINT WM_HOOK_INSERT_TEXT     = WM_APP + 106;
static const UINT WM_HOOK_RESYNC_MODS     = WM_APP + 107;

static HHOOK _kbHook = nullptr, _mouseHook = nullptr;
static HWINEVENTHOOK _focusHook = nullptr, _fgHook = nullptr;
static vKeyHookState* pData = nullptr;
static unsigned _flag = 0, _lastFlag = 0;
static Uint16 _keycode = 0;
static WORD _rawVkCode = 0;
static bool _rawExtended = false;
static bool _swallowedVks[256] = {};
static unsigned _maskPendingMods = 0; // F_ALT/F_WIN bits held when an Alt/Win hotkey fired

static std::atomic<int> _atomicLanguage{1};
static std::atomic<bool> _atomicBlacklistActive{false};
static std::atomic<bool> _recordingHotkey{false};
static std::mutex _frontAppMutex;
static std::wstring _frontAppSnapshot = L"UnknownApp";

static std::function<void(int)> _observer;
static std::function<void()> _emojiHandler;
static std::mutex _callbackMutex;

static std::atomic<HWND> _uiHwnd{nullptr};
static std::atomic<UINT> _uiAsyncMsg{0};

struct AsyncState {
    std::mutex mu;
    int pendingObserverLang = -1;
    int pendingInputMethod = -1;
    bool hasSmartSwitchSave = false;
    std::vector<Byte> smartSwitchBytes;
    std::map<std::wstring, int> tlCounters;
    bool pendingLogFlush = false;
    bool pendingEmoji = false;
    int pendingSoundLang = -1;
};
static AsyncState _async;

static void postUiAsync() {
    HWND h = _uiHwnd.load(std::memory_order_relaxed);
    UINT m = _uiAsyncMsg.load(std::memory_order_relaxed);
    if (h && m) PostMessageW(h, m, 0, 0);
}

static std::wstring _frontApp = L"UnknownApp";
static std::wstring _siteHost;
static bool _cachedIsBrowser = false;
static bool _cachedIsTerminal = false;
static bool _cachedTlKeysAllowed = true;
static bool _uipiBlocked = false;
static bool _otherInputLanguage = false;
static int _languageTemp = 0;
static std::vector<Byte> savedSmartSwitchKeyData;
static bool _isBlacklistActive = false;
static int _savedLangBeforeBlacklist = 1;

// ---- Auto switch by typing (same state machine as VikeyCore.mm) --------------------------
static Uint32 _enKeys[MAX_BUFF];
static int _enCount = 0;
static bool _enValid = true;
static bool _enAfterShortcut = false;
static bool _enAfterDouble = false;
static Uint32 _enPrevKeys[2][MAX_BUFF];
static int _enPrevCounts[2];
static int _enPrevN = 0;
static bool _enPrevRestored = false;

struct AutoFixUndo {
    bool active; int removeCount; wchar_t text[MAX_BUFF + 1]; int textCount; int language;
    Uint32 keys[MAX_BUFF]; int keyCount;
};
static AutoFixUndo _autoFixUndo = {};
static Uint32 _undoneKeys[MAX_BUFF];
static int _undoneCount = 0;

static void StartEnglishWord(bool valid) { _enCount = 0; _enValid = valid; _enAfterShortcut = false; }
static void ResetEnglishWord(bool valid) { _enAfterDouble = false; _undoneCount = 0; StartEnglishWord(valid); _enPrevN = 0; vAutoSwitchForget(); }

// ---- Typing log ------------------------------------------------------------------------
int vTypingLogKeys = 1, vTypingLogEnabled = 0;
static std::string _tlPending;
static std::mutex _tlMutex;
static wchar_t _tlWord[64]; static int _tlWordLen = 0; static int _tlWordLang = 1; static bool _tlWordTruncated = false;
static bool _tlSinceAuto = false;
static std::wstring _tlApp;

static std::string utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
std::wstring typingLogPath() { return prefs::appDataDir() + L"\\typing-log.jsonl"; }
static std::string TLTime() {
    SYSTEMTIME st; GetLocalTime(&st); TIME_ZONE_INFORMATION tz; DWORD r = GetTimeZoneInformation(&tz);
    long bias = -(tz.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : tz.StandardBias));
    char b[64]; snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02ld:%02ld", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, bias < 0 ? '-' : '+', labs(bias) / 60, labs(bias) % 60);
    return b;
}
static std::string TLToday() { SYSTEMTIME st; GetLocalTime(&st); char b[16]; snprintf(b, sizeof b, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay); return b; }
static void TLCount(const char* name) {
    if (!vTypingLogEnabled) return;
    std::string day = TLToday();
    std::wstring key = L"TLStat_" + std::wstring(day.begin(), day.end()) + L"_" + std::wstring(name, name + strlen(name));
    {
        std::lock_guard<std::mutex> lk(_async.mu);
        _async.tlCounters[key] += 1;
    }
    if (!_uiHwnd.load(std::memory_order_relaxed)) {
        processUiAsyncNotifications();
    }
}
void flushTypingLog() {
    processUiAsyncNotifications();
    std::string chunk;
    { std::lock_guard<std::mutex> g(_tlMutex); chunk.swap(_tlPending); }
    if (chunk.empty()) return;
    std::wstring path = typingLogPath();
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && (fad.nFileSizeHigh || fad.nFileSizeLow > 20u * 1024 * 1024)) {
        std::wstring old = path.substr(0, path.size() - 6) + L".1.jsonl";
        DeleteFileW(old.c_str()); MoveFileW(path.c_str(), old.c_str());
    }
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, chunk.data(), (DWORD)chunk.size(), &w, nullptr); CloseHandle(h);
}
void clearTypingLog() {
    { std::lock_guard<std::mutex> g(_tlMutex); _tlPending.clear(); }
    { std::lock_guard<std::mutex> lk(_async.mu); _async.tlCounters.clear(); _async.pendingLogFlush = false; }
    std::wstring path = typingLogPath(); DeleteFileW(path.c_str()); DeleteFileW((path.substr(0, path.size() - 6) + L".1.jsonl").c_str());
}
static bool computeTlAppAllowed(const std::wstring& appIn) {
    static const wchar_t* excluded[] = {
        L"1password.exe", L"bitwarden.exe", L"keepassxc.exe", L"keepass.exe", L"lastpass.exe", L"dashlane.exe", L"enpass.exe", L"nordpass.exe",
        L"windowsterminal.exe", L"wt.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"putty.exe", L"wezterm-gui.exe", L"alacritty.exe", L"conhost.exe", L"openconsole.exe", L"termius.exe" };
    std::wstring app = appIn; std::transform(app.begin(), app.end(), app.begin(), ::towlower);
    for (auto e : excluded) if (app == e) return false;
    return true;
}
static bool TLKeysAllowed() {
    if (!_cachedTlKeysAllowed) return false;
    if (site::focusedIsPassword()) return false;
    return vTypingLogKeys != 0;
}
static void TLWrite(std::string fields) {
    if (!vTypingLogEnabled) return;
    std::string line = "{" + fields + ",\"t\":\"" + TLTime() + "\",\"app\":\"" + jsonEscape(utf8(_tlApp)) + "\"";
    if (!_siteHost.empty()) line += ",\"site\":\"" + jsonEscape(utf8(_siteHost)) + "\"";
    line += "}\n";
    bool needFlush = false;
    {
        std::lock_guard<std::mutex> g(_tlMutex);
        _tlPending += line;
        needFlush = _tlPending.size() > 8192;
    }
    if (needFlush) {
        if (_uiHwnd.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::mutex> lk(_async.mu);
                _async.pendingLogFlush = true;
            }
            postUiAsync();
        } else {
            flushTypingLog();
        }
    }
}
static const char* TLLang(int lang) { return lang ? "vi" : "en"; }
static void TLEndWord(const std::string& reason) {
    if (_tlWordLen == 0) return;
    bool hasLetter = false; for (int x = 0; x < _tlWordLen; x++) if (_tlWord[x] != 0x232B) { hasLetter = true; break; }
    if (hasLetter) TLCount(_tlWordLang ? "words_vi" : "words_en");
    if (hasLetter && TLKeysAllowed()) {
        std::string f = std::string("\"e\":\"word\",\"lang\":\"") + TLLang(_tlWordLang) + "\",\"keys\":\"" + jsonEscape(utf8(std::wstring(_tlWord, _tlWordLen))) + "\",\"end\":\"" + jsonEscape(reason) + "\"";
        if (_tlWordTruncated) f += ",\"truncated\":true";
        TLWrite(f);
    }
    _tlWordLen = 0; _tlWordTruncated = false;
}
static void TLAppendChar(wchar_t c) {
    if (site::focusedIsPassword()) { _tlWordLen = 0; return; }
    if (_tlWordLen == 0) _tlWordLang = vLanguage;
    if (_tlWordLen < 64) _tlWord[_tlWordLen++] = c;
    else _tlWordTruncated = true;
}
static bool IsLetterKey(Uint16 key) {
    for (Uint16 k : {KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,KEY_L,KEY_M,KEY_N,KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,KEY_W,KEY_X,KEY_Y,KEY_Z})
        if (k == key) return true;
    return false;
}
static bool effectiveUpperForKey(Uint16 key, unsigned flags, bool autoCap = false) {
    bool shift = (flags & F_SHIFT) != 0;
    if (IsLetterKey(key)) {
        bool caps = (flags & F_CAPS) != 0;
        return (shift != caps) || autoCap;
    }
    return shift;
}
static std::string TLKeyName(Uint16 key, bool shift) {
    switch (key) {
    case KEY_SPACE: return "space";
    case KEY_RETURN: case KEY_ENTER: return "enter";
    case KEY_TAB: return "tab";
    case KEY_ESC: return "esc";
    case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN: return "arrow";
    }
    Uint16 ch = keyCodeToCharacter(key | (shift ? CAPS_MASK : 0));
    return ch ? utf8(std::wstring(1, (wchar_t)ch)) : "other";
}
static void TLNoteKey(Uint16 key, unsigned flags, bool autoCap = false) {
    if (!vTypingLogEnabled) return;
    _tlSinceAuto = false;
    bool shift = (flags & F_SHIFT) != 0;
    if (flags & (F_CONTROL | F_ALT | F_WIN)) {
        std::string combo; if (flags & F_CONTROL) combo += "Ctrl+"; if (flags & F_ALT) combo += "Alt+"; if (shift) combo += "Shift+"; if (flags & F_WIN) combo += "Win+";
        std::string k = key == KEY_DELETE ? "Backspace" : TLKeyName(key, false); std::transform(k.begin(), k.end(), k.begin(), ::toupper); combo += k;
        TLEndWord(combo); return;
    }
    if (key == KEY_DELETE) { TLAppendChar(0x232B); return; }
    bool letter = IsLetterKey(key);
    if (letter || (IS_NUMBER_KEY(key) && !shift)) {
        bool upper = effectiveUpperForKey(key, flags, autoCap);
        Uint16 ch = keyCodeToCharacter(key | (upper ? CAPS_MASK : 0));
        if (ch) TLAppendChar((wchar_t)ch);
        return;
    }
    TLEndWord(TLKeyName(key, shift));
}
static void TLNoteClick() { _tlSinceAuto = false; TLEndWord("click"); }
static void TLAuto(const char* rule, int to) { TLCount((std::string("auto_") + rule).c_str()); TLWrite(std::string("\"e\":\"auto\",\"rule\":\"") + rule + "\",\"to\":\"" + TLLang(to) + "\""); _tlSinceAuto = true; }
static void TLManual(const char* via, int to) {
    TLEndWord("switch"); bool afterAuto = _tlSinceAuto; _tlSinceAuto = false;
    TLCount("manual"); if (afterAuto) TLCount("manual_after_auto");
    TLWrite(std::string("\"e\":\"manual\",\"via\":\"") + via + "\",\"to\":\"" + TLLang(to) + "\"" + (afterAuto ? ",\"after_auto\":true" : ""));
}
static void TLUndo() { TLEndWord("undo"); _tlSinceAuto = false; TLCount("undo"); TLWrite(std::string("\"e\":\"undo\",\"via\":\"backspace\",\"to\":\"") + TLLang(_autoFixUndo.language) + "\""); }
static void TLContext(int to) { TLEndWord("context"); TLCount("context"); TLWrite(std::string("\"e\":\"context\",\"to\":\"") + TLLang(to) + "\""); }

// ---- Cached runtime settings (reloaded on UI thread, applied on hook thread) -------------
struct CoreSettings {
    int language = 1;
    int checkSpelling = 1;
    int restoreIfWrongSpelling = 1;
    int useModernOrthography = 1;
    int rotationalTone = 1;
    int useSmartSwitchKey = 1;
    int smartSwitchPerSite = 1;
    int autoSwitchByTyping = 1;
    int pauseAutoAfterManual = 1;
    int fixRecommendBrowser = 1;
    int otherLanguage = 1;
    int typingLogKeys = 1;
    int typingLogEnabled = 0;
    int switchKeyStatus = 0x7A000206;
    int emojiHotkey = 0;
    int terminalDelayEnabled = 1;
    int terminalDelayMs = 15;
    int autoCapitalize = 1;
    bool useMacro = true;
    bool blacklistEnabled = true;
    std::vector<std::wstring> blacklist;
};
static CoreSettings _cfg;

// ---- Front app / smart switch / UIPI / Anti-cheat fallback ---------------------------------
std::wstring frontAppName() {
    std::lock_guard<std::mutex> lk(_frontAppMutex);
    return _frontAppSnapshot;
}

static int getEffectiveDelayMs() {
    if (vTerminalDelayEnabled == 0) return 0;
    if (vTerminalDelayEnabled == 2 || _cachedIsTerminal) {
        return vTerminalDelayMs > 0 ? vTerminalDelayMs : 15;
    }
    return 0;
}

static std::wstring exeFromToolhelp(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return L"";
    PROCESSENTRY32W pe = {sizeof(pe)};
    std::wstring res;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                res = pe.szExeFile;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return res;
}

static DWORD getProcessIntegrityRid(HANDLE hProc) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(hProc, TOKEN_QUERY, &tok)) return 0;
    DWORD needed = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &needed);
    DWORD rid = 0;
    if (needed > 0) {
        std::vector<BYTE> buf(needed);
        if (GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), needed, &needed)) {
            TOKEN_MANDATORY_LABEL* tml = (TOKEN_MANDATORY_LABEL*)buf.data();
            UCHAR count = *GetSidSubAuthorityCount(tml->Label.Sid);
            if (count > 0) rid = *GetSidSubAuthority(tml->Label.Sid, count - 1);
        }
    }
    CloseHandle(tok);
    return rid;
}

static DWORD ownIntegrityRid() {
    static DWORD rid = 0;
    if (rid == 0) {
        rid = getProcessIntegrityRid(GetCurrentProcess());
        if (rid == 0) rid = SECURITY_MANDATORY_MEDIUM_RID;
    }
    return rid;
}

static DWORD resolveRealForegroundPid(HWND w, DWORD pid) {
    // For UWP apps hosted inside ApplicationFrameHost.exe, resolve child CoreWindow PID
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    bool isAppFrame = false;
    if (p) {
        wchar_t path[MAX_PATH] = {0}; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            const wchar_t* slash = wcsrchr(path, L'\\');
            const wchar_t* exe = slash ? slash + 1 : path;
            if (_wcsicmp(exe, L"ApplicationFrameHost.exe") == 0) isAppFrame = true;
        }
        CloseHandle(p);
    }
    if (!isAppFrame) return pid;

    struct ChildLookup { DWORD hostPid; DWORD realPid; } ctx = {pid, pid};
    EnumChildWindows(w, [](HWND ch, LPARAM lp) -> BOOL {
        auto* c = (ChildLookup*)lp;
        DWORD cpid = 0;
        GetWindowThreadProcessId(ch, &cpid);
        if (cpid && cpid != c->hostPid) {
            c->realPid = cpid;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.realPid;
}

static bool isExeInConfiguredBlacklist(const std::wstring& exeName) {
    if (!_cfg.blacklistEnabled || exeName.empty() || exeName == L"UnknownApp") return false;
    std::wstring name = exeName;
    size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    for (auto& c : name) c = (wchar_t)towlower(c);
    for (const auto& item : _cfg.blacklist) {
        if (item == name) return true;
    }
    return false;
}

static bool queryFrontMostApp() {
    HWND w = GetForegroundWindow();
    if (!w) return false;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[128] = {0};
    GetClassNameW(w, cls, 128);
    if (isIgnoredSystemUiWindow(cls, nullptr)) return false;
    HWND root = GetAncestor(w, GA_ROOT);
    if (root && root != w) {
        wchar_t rootCls[128] = {0};
        GetClassNameW(root, rootCls, 128);
        if (isIgnoredSystemUiWindow(rootCls, nullptr)) return false;
    }

    pid = resolveRealForegroundPid(w, pid);
    std::wstring name = L"UnknownApp";
    bool uipiBlocked = false;

    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) {
        wchar_t path[MAX_PATH] = {0}; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            std::wstring s(path);
            size_t i = s.find_last_of(L'\\');
            name = i == std::wstring::npos ? s : s.substr(i + 1);
        }
        DWORD targetRid = getProcessIntegrityRid(p);
        if (targetRid > ownIntegrityRid()) uipiBlocked = true;
        CloseHandle(p);
    } else if (pid != 0) {
        // Fallback for PPL / anti-cheat games (Valorant, CS2, LoL, Genshin...)
        std::wstring th = exeFromToolhelp(pid);
        if (!th.empty()) name = th;
    }

    if (isIgnoredSystemUiWindow(nullptr, name.c_str())) return false;
    _frontApp = name;
    _uipiBlocked = uipiBlocked;
    _cachedIsBrowser = site::isBrowser(_frontApp);
    _cachedIsTerminal = isTerminalOrElectronApp(_frontApp);
    _cachedTlKeysAllowed = computeTlAppAllowed(_frontApp);
    {
        std::lock_guard<std::mutex> lk(_frontAppMutex);
        _frontAppSnapshot = _frontApp;
    }
    return true;
}

static std::string SmartSwitchContextKey() {
    std::wstring k = _frontApp;
    if (!_siteHost.empty()) k += L"|" + _siteHost;
    return utf8(k);
}
static void saveSmartSwitchKeyData() {
    getSmartSwitchKeySaveData(savedSmartSwitchKeyData);
    {
        std::lock_guard<std::mutex> lk(_async.mu);
        _async.smartSwitchBytes = savedSmartSwitchKeyData;
        _async.hasSmartSwitchSave = true;
    }
    postUiAsync();
}
static void VKLanguageChanged(bool rememberForContext) {
    _atomicLanguage.store(vLanguage, std::memory_order_relaxed);
    if (rememberForContext && vUseSmartSwitchKey) {
        if (!(_cfg.smartSwitchPerSite && _cachedIsBrowser && _siteHost.empty())) {
            setAppInputMethodStatus(SmartSwitchContextKey(), vLanguage);
            saveSmartSwitchKeyData();
        }
    }
    {
        std::lock_guard<std::mutex> lk(_async.mu);
        _async.pendingInputMethod = vLanguage;
        _async.pendingObserverLang = vLanguage;
    }
    postUiAsync();
}
static void ApplySmartSwitch() {
    if (_cfg.smartSwitchPerSite && _cachedIsBrowser && _siteHost.empty()) {
        return; // Đang chờ UIA lấy host
    }
    _languageTemp = getAppInputMethodStatus(SmartSwitchContextKey(), vLanguage);
    if (_languageTemp == -1) {
        saveSmartSwitchKeyData();
    } else if ((_languageTemp & 0x01) != vLanguage) {
        vLanguage = _languageTemp & 0x01;
        VKLanguageChanged(false);
        TLContext(vLanguage);
        startNewSession();
    }
}

static void resyncModifierFlags() {
    auto checkDown = [](int vk, unsigned bit) {
        if ((_flag & bit) && !(GetAsyncKeyState(vk) & 0x8000)) {
            _flag &= ~bit;
        }
    };
    checkDown(VK_LSHIFT, F_LSHIFT);
    checkDown(VK_RSHIFT, F_RSHIFT);
    checkDown(VK_LCONTROL, F_LCONTROL);
    checkDown(VK_RCONTROL, F_RCONTROL);
    if ((_flag & F_LALT) && !(GetAsyncKeyState(VK_LMENU) & 0x8000) &&
        !((GetAsyncKeyState(VK_MENU) & 0x8000) && !(GetAsyncKeyState(VK_RMENU) & 0x8000))) {
        _flag &= ~F_LALT;
    }
    checkDown(VK_RMENU, F_RALT);
    checkDown(VK_LWIN, F_LWIN);
    checkDown(VK_RWIN, F_RWIN);
    if (GetKeyState(VK_CAPITAL) & 1) _flag |= F_CAPS; else _flag &= ~F_CAPS;
    _maskPendingMods &= _flag;
    memset(_swallowedVks, 0, sizeof(_swallowedVks));
}

static void updateOtherInputLanguage();
static void onForegroundChangedInternal() {
    resyncModifierFlags();
    _lastFlag = 0;
    _maskPendingMods = 0;
    updateOtherInputLanguage();
    if (!queryFrontMostApp()) return;
    _siteHost.clear(); macro::reset(); _autoCapHelper.reset();
    ResetEnglishWord(true); _autoFixUndo.active = false;
    TLEndWord("app"); _tlApp = _frontApp;
    _tlWordLen = 0;
    site::pollPassword();

    if (isExeInConfiguredBlacklist(_frontApp)) {
        if (!_isBlacklistActive) {
            _savedLangBeforeBlacklist = vLanguage;
            _isBlacklistActive = true;
            _atomicBlacklistActive.store(true, std::memory_order_relaxed);
            vLanguage = 0;
            _atomicLanguage.store(0, std::memory_order_relaxed);
            _autoFixUndo.active = false; _autoCapHelper.reset(); ResetEnglishWord(true);
            VKLanguageChanged(false);
        }
    } else {
        if (_isBlacklistActive) {
            _isBlacklistActive = false;
            _atomicBlacklistActive.store(false, std::memory_order_relaxed);
            if (vUseSmartSwitchKey) {
                ApplySmartSwitch();
            } else {
                vLanguage = _savedLangBeforeBlacklist ? 1 : 0;
                VKLanguageChanged(false);
            }
        } else if (vUseSmartSwitchKey) {
            ApplySmartSwitch();
        }
    }
    startNewSession();
}

static void onSiteChangedInternal(const std::wstring& host) {
    if (!host.empty() && !_cachedIsBrowser) return; // never attach browser host to non-browser app
    if (host == _siteHost) return;
    TLEndWord("site");
    _siteHost = host;

    int oldLang = vLanguage;
    if (vUseSmartSwitchKey && !_isBlacklistActive) ApplySmartSwitch();

    if (vLanguage != oldLang) {
        _autoCapHelper.reset();
        ResetEnglishWord(true);
        _autoFixUndo.active = false;
        startNewSession();
    } else if (vKeyCurrentWordLength() == 0) {
        _autoCapHelper.reset();
        ResetEnglishWord(true);
        _autoFixUndo.active = false;
        startNewSession();
    }
}

// ---- Other input language (Japanese, Chinese...) ------------------------------------------
static void updateOtherInputLanguage() {
    HWND w = GetForegroundWindow(); DWORD tid = w ? GetWindowThreadProcessId(w, nullptr) : 0;
    HKL hkl = GetKeyboardLayout(tid); LANGID lang = LOWORD((DWORD_PTR)hkl); WORD primary = PRIMARYLANGID(lang);
    _otherInputLanguage = !(primary == LANG_ENGLISH || primary == LANG_VIETNAMESE || primary == LANG_NEUTRAL);
}
static bool imeComposing() {
    HWND w = GetForegroundWindow(); HWND ime = w ? ImmGetDefaultIMEWnd(w) : nullptr;
    if (!ime) return false;
    DWORD_PTR r = 0; SendMessageTimeoutW(ime, WM_IME_CONTROL, IMC_GETOPENSTATUS, 0, SMTO_ABORTIFHUNG, 20, &r);
    return r != 0 && _otherInputLanguage;
}

// ---- Pause auto switch after a manual switch (D21) ------------------------------------------
static ULONGLONG _autoPauseUntil = 0;
static bool AutoSwitchPaused() { return GetTickCount64() < _autoPauseUntil; }
static void PauseAutoSwitchAfterManual() {
    if (!_cfg.pauseAutoAfterManual) return;
    _autoPauseUntil = GetTickCount64() + 5000;
    vAutoSwitchByTyping = 0;
}
static void checkPauseExpired() {
    if (_autoPauseUntil && !AutoSwitchPaused()) {
        _autoPauseUntil = 0;
        vAutoSwitchByTyping = _cfg.autoSwitchByTyping;
        StartEnglishWord(false);
    }
}

static CoreSettings readSettingsSnapshot() {
    CoreSettings s;
    s.language = prefs::getInt(L"InputMethod", 1) ? 1 : 0;
    s.checkSpelling = prefs::getInt(L"Spelling", 1);
    s.restoreIfWrongSpelling = prefs::getInt(L"RestoreIfInvalidWord", 1);
    s.useModernOrthography = prefs::getInt(L"ModernOrthography", 1);
    s.rotationalTone = prefs::getInt(L"RotationalTone", 1);
    s.useSmartSwitchKey = prefs::getInt(L"UseSmartSwitchKey", 1);
    s.smartSwitchPerSite = prefs::getInt(L"SmartSwitchPerSite", 1);
    s.autoSwitchByTyping = prefs::getInt(L"AutoSwitchByTyping", 1);
    s.pauseAutoAfterManual = prefs::has(L"PauseAutoAfterManual") ? prefs::getInt(L"PauseAutoAfterManual", 1) : 1;
    s.fixRecommendBrowser = prefs::getInt(L"FixRecommendBrowser", 1);
    s.otherLanguage = prefs::getInt(L"OtherLanguage", 1);
    s.typingLogKeys = prefs::getInt(L"TypingLogKeys", 1);
    s.typingLogEnabled = prefs::getInt(L"TypingLogEnabled", 0);
    int hotkey = prefs::getInt(L"SwitchKeyStatus", 0);
    s.switchKeyStatus = hotkey ? hotkey : 0x7A000206;
    s.emojiHotkey = prefs::getInt(L"EmojiPicker", 0) ? prefs::getInt(L"EmojiHotkey", 0) : 0;
    s.terminalDelayEnabled = prefs::getTerminalDelayEnabled();
    s.terminalDelayMs = prefs::getTerminalDelayMs();
    s.autoCapitalize = prefs::getAutoCapitalize();
    s.useMacro = prefs::getInt(L"UseMacro", 1) != 0;
    s.blacklistEnabled = prefs::isBlacklistEnabled();
    s.blacklist = prefs::getBlacklist();
    return s;
}

static void applySettingsOnHookThread(const CoreSettings& s) {
    _cfg = s;
    vLanguage = s.language;
    _atomicLanguage.store(vLanguage, std::memory_order_relaxed);
    vCheckSpelling = s.checkSpelling; vSetCheckSpelling();
    vRestoreIfWrongSpelling = s.restoreIfWrongSpelling;
    vUseModernOrthography = s.useModernOrthography;
    vRotationalTone = s.rotationalTone;
    vUseSmartSwitchKey = s.useSmartSwitchKey;
    vAutoSwitchByTyping = AutoSwitchPaused() ? 0 : s.autoSwitchByTyping;
    vFixRecommendBrowser = s.fixRecommendBrowser;
    vOtherLanguage = s.otherLanguage;
    vTypingLogKeys = s.typingLogKeys;
    vTypingLogEnabled = s.typingLogEnabled;
    vSwitchKeyStatus = s.switchKeyStatus;
    vEmojiHotkey = s.emojiHotkey;
    vTerminalDelayEnabled = s.terminalDelayEnabled;
    vTerminalDelayMs = s.terminalDelayMs;
    vAutoCapitalize = s.autoCapitalize;
    macro::setEnabled(s.useMacro, false);
    updateOtherInputLanguage();
    {
        std::lock_guard<std::mutex> lk(_async.mu);
        _async.pendingObserverLang = vLanguage;
    }
    postUiAsync();
}

// ---- Sending output ----------------------------------------------------------------------
static void inputKey(INPUT& in, WORD vk, bool down, DWORD extra = 0) {
    in = {}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk; in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | extra; in.ki.dwExtraInfo = VIKEY_EVENT_TAG;
}
// Unassigned Win32 VK (AutoHotkey's MenuMaskKey) sent with VIKEY_EVENT_TAG while Alt/Win is held
// so win32k disarms SC_KEYMENU (Chrome/Win32 menu bar focus) and Start Menu activation without
// producing WM_SYSCHAR/beep or modifier side effects.
static const WORD MENU_MASK_VK = 0xE8;
static void sendMenuMaskKey() {
    INPUT ins[2] = {};
    inputKey(ins[0], MENU_MASK_VK, true);
    inputKey(ins[1], MENU_MASK_VK, false);
    SendInput(2, ins, sizeof(INPUT));
}
static bool sendMenuMaskAndModifierUp(WORD modVk, WORD modScan, bool extended) {
    INPUT ins[3] = {};
    inputKey(ins[0], MENU_MASK_VK, true);
    inputKey(ins[1], MENU_MASK_VK, false);
    inputKey(ins[2], modVk, false, extended ? KEYEVENTF_EXTENDEDKEY : 0);
    ins[2].ki.wScan = modScan;
    return SendInput(3, ins, sizeof(INPUT)) == 3;
}
static void inputChar(INPUT& in, wchar_t c, bool down) {
    in = {}; in.type = INPUT_KEYBOARD; in.ki.wScan = c; in.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP); in.ki.dwExtraInfo = VIKEY_EVENT_TAG;
}
static std::vector<INPUT> _batch;
static void batchBackspaces(int n) {
    for (int i = 0; i < n; i++) {
        INPUT a, b;
        inputKey(a, VK_BACK, true);
        inputKey(b, VK_BACK, false);
        _batch.push_back(a);
        _batch.push_back(b);
    }
}
static void batchText(const wchar_t* text, int count) {
    for (int i = 0; i < count; i++) {
        if (text[i] == L'\r' && i + 1 < count && text[i + 1] == L'\n') continue; // normalize CRLF to single Return
        if (text[i] == L'\n' || text[i] == L'\r') {
            INPUT a, b;
            inputKey(a, VK_RETURN, true);
            inputKey(b, VK_RETURN, false);
            _batch.push_back(a);
            _batch.push_back(b);
            continue;
        }
        INPUT a, b;
        inputChar(a, text[i], true);
        inputChar(b, text[i], false);
        _batch.push_back(a);
        _batch.push_back(b);
    }
}
static void batchKey(WORD vk, bool shift, bool extended = false) {
    bool physicalShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool injectShift = shift && !physicalShift;
    DWORD extFlag = extended ? KEYEVENTF_EXTENDEDKEY : 0;
    INPUT a, b, c, d;
    if (injectShift) { inputKey(a, VK_SHIFT, true); _batch.push_back(a); }
    inputKey(c, vk, true, extFlag);
    inputKey(d, vk, false, extFlag);
    _batch.push_back(c);
    _batch.push_back(d);
    if (injectShift) { inputKey(b, VK_SHIFT, false); _batch.push_back(b); }
}
static void flushBatch() {
    if (!_batch.empty()) {
        UINT sent = SendInput((UINT)_batch.size(), _batch.data(), sizeof(INPUT));
        if (sent == 0 && GetLastError() == ERROR_ACCESS_DENIED) {
            _uipiBlocked = true;
        }
        _batch.clear();
    }
}
static void sleepWithTimerPrecision(int delayMs) {
    if (delayMs <= 0) return;
    if (delayMs > 25) delayMs = 25;
    timeBeginPeriod(1);
    Sleep(delayMs);
    timeEndPeriod(1);
}

static void batchEngineChars() {
    wchar_t text[MAX_BUFF + 2]; int n = 0;
    for (int k = pData->newCharCount - 1; k >= 0 && n < MAX_BUFF; k--) {
        Uint32 c = pData->charData[k];
        text[n++] = (c & CHAR_CODE_MASK) ? (wchar_t)(c & 0xFFFF) : (wchar_t)keyCodeToCharacter(c);
    }
    bool sendControlKey = false;
    if (pData->code == vRestore || pData->code == vRestoreAndStartNewSession) {
        bool upper = effectiveUpperForKey(_keycode, _flag);
        Uint16 ch = (_keycode == KEY_SPACE) ? L' ' : keyCodeToCharacter(_keycode | (upper ? CAPS_MASK : 0));
        if (ch != 0) text[n++] = (wchar_t)ch; else sendControlKey = true;
    }
    if (pData->code == vRestoreAndStartNewSession) startNewSession();
    if (n > 0) batchText(text, n);
    if (sendControlKey) {
        WORD vk = _rawVkCode ? _rawVkCode : vkOf(_keycode);
        if (vk) batchKey(vk, (_flag & F_SHIFT) != 0, _rawExtended);
    }
}

static void SendEngineOutput() {
    int delay = getEffectiveDelayMs();
    int bkCount = (pData->backspaceCount > 0 && pData->backspaceCount <= MAX_BUFF) ? pData->backspaceCount : 0;
    bool hasBrowserFix = (vFixRecommendBrowser && pData->extCode != 4 && _cachedIsBrowser);
    if (hasBrowserFix) { wchar_t e = 0x202F; batchText(&e, 1); bkCount++; }
    if (delay > 0 && bkCount > 0) {
        batchBackspaces(bkCount);
        flushBatch();
        sleepWithTimerPrecision(delay);
        batchEngineChars();
        flushBatch();
    } else {
        if (bkCount > 0) batchBackspaces(bkCount);
        batchEngineChars();
        flushBatch();
    }
}

// ---- Hotkey ------------------------------------------------------------------------------
static bool checkHotKey(int hotKeyData, bool checkKeyCode, unsigned flagsToCheck) {
    if ((hotKeyData & (~0x8000)) == EMPTY_HOTKEY) return false;
    if (HAS_CONTROL(hotKeyData) ^ GET_BOOL(flagsToCheck & F_CONTROL)) return false;
    if (HAS_OPTION(hotKeyData) ^ GET_BOOL(flagsToCheck & F_ALT)) return false;
    if (HAS_COMMAND(hotKeyData) ^ GET_BOOL(flagsToCheck & F_WIN)) return false;
    if (HAS_SHIFT(hotKeyData) ^ GET_BOOL(flagsToCheck & F_SHIFT)) return false;
    if (checkKeyCode && GET_SWITCH_KEY(hotKeyData) != _keycode) return false;
    return true;
}
static void switchLanguage(const char* via) {
    PauseAutoSwitchAfterManual();
    _autoFixUndo.active = false;
    _autoCapHelper.reset();
    macro::reset();
    ResetEnglishWord(true);
    vLanguage = vLanguage == 0 ? 1 : 0;
    TLManual(via, vLanguage);
    if (HAS_BEEP(vSwitchKeyStatus)) {
        std::lock_guard<std::mutex> lk(_async.mu);
        _async.pendingSoundLang = vLanguage;
    }
    VKLanguageChanged(true);
    startNewSession();
}
static void setLanguageByUserInternal(int lang) {
    if (vLanguage == lang) return;
    PauseAutoSwitchAfterManual(); _autoFixUndo.active = false; _autoCapHelper.reset(); macro::reset(); ResetEnglishWord(true);
    vLanguage = lang ? 1 : 0; TLManual("menu", vLanguage); VKLanguageChanged(true); startNewSession();
}
static void setLanguageQuietInternal(int lang) {
    _autoFixUndo.active = false; _autoCapHelper.reset(); macro::reset(); ResetEnglishWord(true);
    _autoPauseUntil = 0; vAutoSwitchByTyping = _cfg.autoSwitchByTyping;
    vLanguage = lang ? 1 : 0; VKLanguageChanged(false); startNewSession();
}

// ---- Auto switch helpers (same as VikeyCore.mm) ---------------------------------------------
static bool IsWhitespaceKey(Uint16 key) { return key == KEY_SPACE || key == KEY_RETURN || key == KEY_ENTER || key == KEY_TAB; }
static bool IsWordStartKey(Uint16 key, bool shift) { return IsWhitespaceKey(key) || (key == KEY_9 && shift) || key == KEY_LEFT_BRACKET || key == KEY_QUOTE; }
static wchar_t CharacterOf(Uint32 data) { return (data & CHAR_CODE_MASK) ? (wchar_t)(data & 0xFFFF) : (wchar_t)keyCodeToCharacter(data); }

static void RememberAutoFix(int correctedCount) {
    _autoFixUndo.active = false;
    if (vAutoSwitchUndoCount <= 0 || correctedCount <= 0) return;
    for (int x = 0; x < vAutoSwitchUndoCount; x++) if ((vAutoSwitchUndoData[x] & 0xFFFF) == KEY_SPACE && !(vAutoSwitchUndoData[x] & CHAR_CODE_MASK)) return;
    bool validBreak = (_keycode == KEY_SPACE) || (keyCodeToCharacter(_keycode | ((_flag & F_SHIFT) ? CAPS_MASK : 0)) != 0);
    if (!validBreak) return;
    int n = 0;
    for (int x = 0; x < vAutoSwitchUndoCount && n < MAX_BUFF; x++) { wchar_t c = CharacterOf(vAutoSwitchUndoData[x]); if (c == 0) return; _autoFixUndo.text[n++] = c; }
    _autoFixUndo.textCount = n;
    _autoFixUndo.keyCount = 0;
    for (int x = 0; x < vAutoSwitchUndoCount && x < MAX_BUFF; x++) {
        if (vAutoSwitchUndoData[x] & CHAR_CODE_MASK) { _autoFixUndo.keyCount = 0; break; }
        _autoFixUndo.keys[_autoFixUndo.keyCount++] = vAutoSwitchUndoData[x];
    }
    _autoFixUndo.removeCount = correctedCount + 1; _autoFixUndo.language = vLanguage; _autoFixUndo.active = true;
}
static bool UndoAutoFixWithBackspace() {
    if (!_autoFixUndo.active) return false;
    _autoFixUndo.active = false;
    int removeCount = _autoFixUndo.removeCount;
    int delay = getEffectiveDelayMs();
    if (vFixRecommendBrowser && _cachedIsBrowser) { wchar_t e = 0x202F; batchText(&e, 1); removeCount++; }
    if (delay > 0 && removeCount > 0) {
        batchBackspaces(removeCount);
        flushBatch();
        sleepWithTimerPrecision(delay);
        batchText(_autoFixUndo.text, _autoFixUndo.textCount);
        flushBatch();
    } else {
        batchBackspaces(removeCount);
        batchText(_autoFixUndo.text, _autoFixUndo.textCount);
        flushBatch();
    }
    TLUndo();
    if (vLanguage != _autoFixUndo.language) { vLanguage = _autoFixUndo.language; VKLanguageChanged(true); }
    startNewSession();
    ResetEnglishWord(false);
    if (vLanguage == 0 && _autoFixUndo.keyCount > 0) {
        memcpy(_enKeys, _autoFixUndo.keys, _autoFixUndo.keyCount * sizeof(Uint32));
        _enCount = _autoFixUndo.keyCount;
        _enValid = true;
        memcpy(_undoneKeys, _autoFixUndo.keys, _autoFixUndo.keyCount * sizeof(Uint32));
        _undoneCount = _autoFixUndo.keyCount;
    }
    return true;
}
static void AutoSwitchTo(int language, const char* rule) {
    if (vLanguage == language) return;
    TLAuto(rule, language); vLanguage = language; VKLanguageChanged(true); startNewSession(); vAutoSwitchForget();
}
static void ApplyAutoSwitchToEnglish(int correctedCount, bool atWordBreak) {
    vAutoSwitchRequest = -1;
    if (atWordBreak) RememberAutoFix(correctedCount); else _autoFixUndo.active = false;
    AutoSwitchTo(0, atWordBreak ? "vi_word_to_en" : "double_key");
    ResetEnglishWord(atWordBreak && IsWhitespaceKey(_keycode));
    if (atWordBreak && _keycode == KEY_SPACE && vAutoSwitchRestoredCount > 0) {
        memcpy(_enPrevKeys[0], vAutoSwitchRestoredKeys, vAutoSwitchRestoredCount * sizeof(Uint32));
        _enPrevCounts[0] = vAutoSwitchRestoredCount;
        _enPrevN = 1;
        _enPrevRestored = true;
    }
    _enAfterDouble = !atWordBreak;
}
static bool HandleEnglishKeyForAutoSwitch(bool autoCap = false) {
    if (OTHER_CONTROL_KEY) { ResetEnglishWord(true); _enAfterShortcut = (_flag & (F_CONTROL | F_WIN)) != 0; return false; }
    bool shift = (_flag & F_SHIFT) != 0;
    if (IsLetterKey(_keycode)) {
        bool upper = effectiveUpperForKey(_keycode, _flag, autoCap);
        if (_enCount < MAX_BUFF - 2) _enKeys[_enCount++] = _keycode | (upper ? CAPS_MASK : 0); else _enValid = false;
        _enAfterShortcut = false;
        return false;
    }
    if (_keycode == KEY_DELETE) {
        if (_enCount == 0) { _enPrevN = 0; _enAfterDouble = false; }
        if (_enCount > 0) _enCount--; else if (!_enAfterShortcut) _enValid = false;
        return false;
    }
    if (IS_NUMBER_KEY(_keycode) && !shift) {
        _enValid = _enCount == 0 && _enValid;
        _enCount = 0;
        _enPrevN = 0;
        _enAfterDouble = false;
        return false;
    }
    bool wordEnd = _keycode == KEY_SPACE || _keycode == KEY_COMMA || _keycode == KEY_DOT || _keycode == KEY_SEMICOLON || _keycode == KEY_RETURN || _keycode == KEY_ENTER ||
                   (shift && (_keycode == KEY_SLASH || _keycode == KEY_1 || _keycode == KEY_QUOTE || _keycode == KEY_0));
    int count = _enCount; bool valid = _enValid; bool afterDouble = _enAfterDouble; _enAfterDouble = false;
    if (afterDouble && wordEnd && vAutoSwitchFixDoubledKey(_enKeys, count)) {
        StartEnglishWord(IsWordStartKey(_keycode, shift)); _enPrevN = 0;
        int correctedCount = pData->newCharCount; SendEngineOutput(); RememberAutoFix(correctedCount); return true;
    }
    StartEnglishWord(IsWordStartKey(_keycode, shift));
    bool checked = wordEnd && valid && count >= 2 && !(vOtherLanguage && _otherInputLanguage);
    if (_undoneCount > 0) {
        if (count == _undoneCount && !memcmp(_enKeys, _undoneKeys, count * sizeof(Uint32))) checked = false;
        if (wordEnd) _undoneCount = 0;
    }
    const Uint32* previous[2] = {_enPrevKeys[0], _enPrevKeys[1]};
    vAutoSwitchPrevRestored = _enPrevRestored && _enPrevN >= 1;
    bool converted = checked && vAutoSwitchCheckEnglishWord(_enKeys, count, previous, _enPrevCounts, _enPrevN);
    if (!converted && _keycode == KEY_SPACE && valid && count >= 1) {
        _enPrevRestored = false;
        memcpy(_enPrevKeys[1], _enPrevKeys[0], sizeof(_enPrevKeys[0])); _enPrevCounts[1] = _enPrevCounts[0];
        memcpy(_enPrevKeys[0], _enKeys, count * sizeof(Uint32)); _enPrevCounts[0] = count; if (_enPrevN < 2) _enPrevN++;
    } else _enPrevN = 0;
    if (!converted) return false;
    int correctedCount = pData->newCharCount; SendEngineOutput(); RememberAutoFix(correctedCount); AutoSwitchTo(1, "en_word_to_vi");
    return true;
}

// ---- Hooks --------------------------------------------------------------------------------
static unsigned modifierBitOf(DWORD vk) {
    switch (vk) {
    case VK_LSHIFT:   return F_LSHIFT;
    case VK_RSHIFT:   return F_RSHIFT;
    case VK_SHIFT:    return F_LSHIFT;
    case VK_LCONTROL: return F_LCONTROL;
    case VK_RCONTROL: return F_RCONTROL;
    case VK_CONTROL:  return F_LCONTROL;
    case VK_LMENU:    return F_LALT;
    case VK_RMENU:    return F_RALT;
    case VK_MENU:     return F_LALT;
    case VK_LWIN:     return F_LWIN;
    case VK_RWIN:     return F_RWIN;
    default:          return 0;
    }
}

static LRESULT CALLBACK keyboardHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION) return CallNextHookEx(_kbHook, nCode, wParam, lParam);
    KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;
    if (k->dwExtraInfo == VIKEY_EVENT_TAG) return CallNextHookEx(_kbHook, nCode, wParam, lParam);
    bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    if (GetKeyState(VK_CAPITAL) & 1) _flag |= F_CAPS; else _flag &= ~F_CAPS;
    checkPauseExpired();

    if (unsigned modBit = modifierBitOf(k->vkCode)) {
        if (down) _flag |= modBit; else _flag &= ~modBit;
        bool needMaskThisUp = false;
        if (!down && (modBit & (F_ALT | F_WIN)) && (_maskPendingMods & modBit)) {
            _maskPendingMods &= ~modBit;
            needMaskThisUp = true;
        }
        if (!_isBlacklistActive && !_uipiBlocked && !_recordingHotkey.load(std::memory_order_relaxed)) {
            if (_lastFlag == 0 || _lastFlag < _flag) _lastFlag = _flag;
            else if (_lastFlag > _flag) {
                if (GET_SWITCH_KEY(vSwitchKeyStatus) == 0xFE && checkHotKey(vSwitchKeyStatus, false, _lastFlag)) {
                    unsigned triggeredMods = _lastFlag;
                    _lastFlag = 0;
                    switchLanguage("hotkey");
                    if (triggeredMods & (F_ALT | F_WIN)) {
                        if (modBit & (F_ALT | F_WIN)) {
                            needMaskThisUp = true;
                        } else if (_flag & (F_ALT | F_WIN)) {
                            _maskPendingMods |= (_flag & (F_ALT | F_WIN));
                            sendMenuMaskKey();
                        }
                    }
                } else {
                    _lastFlag = 0;
                }
            }
        } else {
            _lastFlag = 0;
        }
        if (needMaskThisUp && !_uipiBlocked) {
            if (sendMenuMaskAndModifierUp((WORD)k->vkCode, (WORD)k->scanCode, (k->flags & LLKHF_EXTENDED) != 0)) {
                return 1;
            }
        }
        return CallNextHookEx(_kbHook, nCode, wParam, lParam);
    }

    WORD vkByte = (WORD)(k->vkCode & 0xFF);
    if (!down) {
        if (_swallowedVks[vkByte]) {
            _swallowedVks[vkByte] = false;
            return 1;
        }
        return CallNextHookEx(_kbHook, nCode, wParam, lParam);
    }

    // Resync modifier bits if any control/alt/win appears stuck from Win+L / UAC
    if (OTHER_CONTROL_KEY) resyncModifierFlags();

    // Toggling CapsLock mid-word should not break the active word
    if (k->vkCode == VK_CAPITAL) {
        return CallNextHookEx(_kbHook, nCode, wParam, lParam);
    }

    _keycode = engineKey(k->vkCode);
    _rawVkCode = (WORD)k->vkCode;
    _rawExtended = (k->flags & LLKHF_EXTENDED) != 0;

    // Emoji picker hotkey
    if (!_recordingHotkey.load(std::memory_order_relaxed) && vEmojiHotkey &&
        GET_SWITCH_KEY(vEmojiHotkey) == _keycode &&
        HAS_CONTROL(vEmojiHotkey) == GET_BOOL(_flag & F_CONTROL) &&
        HAS_OPTION(vEmojiHotkey) == GET_BOOL(_flag & F_ALT) &&
        HAS_COMMAND(vEmojiHotkey) == GET_BOOL(_flag & F_WIN) &&
        HAS_SHIFT(vEmojiHotkey) == GET_BOOL(_flag & F_SHIFT)) {
        if (_flag & (F_ALT | F_WIN)) {
            _maskPendingMods |= (_flag & (F_ALT | F_WIN));
            sendMenuMaskKey();
        }
        {
            std::lock_guard<std::mutex> lk(_async.mu);
            _async.pendingEmoji = true;
        }
        postUiAsync();
        _swallowedVks[vkByte] = true;
        return 1;
    }

    HWND ew = vEmojiWindow.load(std::memory_order_relaxed);
    if (vEmojiOpen.load(std::memory_order_relaxed) && ew) {
        if (k->vkCode == VK_PACKET) PostMessageW(ew, WM_APP + 12, k->scanCode, 0);
        else PostMessageW(ew, WM_APP + 10, k->vkCode, (LPARAM)(((_flag & F_SHIFT) ? 1u : 0u) | ((_flag & F_CONTROL) ? 2u : 0u) | ((_flag & F_ALT) ? 4u : 0u) | ((_flag & F_WIN) ? 8u : 0u) | ((_flag & F_CAPS) ? 16u : 0u)));
        _swallowedVks[vkByte] = true;
        return 1;
    }

    if (_keycode == KEY_EMPTY) {
        _lastFlag = 0;
        _maskPendingMods = 0;
        macro::reset();
        _autoCapHelper.reset();
        ResetEnglishWord(true);
        startNewSession();
        return CallNextHookEx(_kbHook, nCode, wParam, lParam);
    }

    // Blacklisted apps or UIPI-elevated apps: pass keys through transparently
    if (_isBlacklistActive || _uipiBlocked) {
        _lastFlag = 0;
        _maskPendingMods = 0;
        return CallNextHookEx(_kbHook, nCode, wParam, lParam);
    }

    // Switch language hotkey (key+modifier)
    if (!_recordingHotkey.load(std::memory_order_relaxed)) {
        if (GET_SWITCH_KEY(vSwitchKeyStatus) != 0xFE &&
            GET_SWITCH_KEY(vSwitchKeyStatus) == _keycode &&
            checkHotKey(vSwitchKeyStatus, true, _flag)) {
            switchLanguage("hotkey");
            if (_flag & (F_ALT | F_WIN)) {
                _maskPendingMods |= (_flag & (F_ALT | F_WIN));
                sendMenuMaskKey();
            }
            _lastFlag = 0;
            _swallowedVks[vkByte] = true;
            return 1;
        }
    }
    _lastFlag = 0;
    _maskPendingMods = 0;

    // Backspace right after an automatic correction undoes it
    if (_keycode == KEY_DELETE && _autoFixUndo.active && !(OTHER_CONTROL_KEY)) {
        if (UndoAutoFixWithBackspace()) {
            _swallowedVks[vkByte] = true;
            return 1;
        }
    }
    _autoFixUndo.active = false;

    bool shift = (_flag & F_SHIFT) != 0;
    bool capsLock = (_flag & F_CAPS) != 0;
    bool autoCapThisKey = false;
    if (vAutoCapitalize) {
        autoCapThisKey = _autoCapHelper.processKey(_keycode, shift, capsLock, OTHER_CONTROL_KEY);
    }

    TLNoteKey(_keycode, _flag, autoCapThisKey);

    if (OTHER_CONTROL_KEY || _keycode == KEY_ESC || _keycode == KEY_LEFT || _keycode == KEY_RIGHT ||
        _keycode == KEY_UP || _keycode == KEY_DOWN || k->vkCode == VK_HOME || k->vkCode == VK_END) {
        macro::reset();
        _autoCapHelper.reset();
    } else if (_keycode == KEY_DELETE) {
        macro::onBackspace();
    } else if (macro::isEnabled() && macro::isDelimiter(k->vkCode, _keycode, shift)) {
        std::wstring repl;
        int shortcutLen = 0;
        if (macro::match(repl, shortcutLen, vLanguage != 0)) {
            int removeCount = shortcutLen;
            if (vLanguage != 0 && vKeyCurrentWordLength() > 0 && vKeyCurrentWordLength() <= shortcutLen) {
                removeCount = vKeyCurrentWordLength();
            }
            int delay = getEffectiveDelayMs();
            if (vFixRecommendBrowser && _cachedIsBrowser) { wchar_t e = 0x202F; batchText(&e, 1); removeCount++; }
            if (delay > 0 && removeCount > 0) {
                batchBackspaces(removeCount);
                flushBatch();
                sleepWithTimerPrecision(delay);
            } else if (removeCount > 0) {
                batchBackspaces(removeCount);
                flushBatch();
            }
            batchText(repl.c_str(), (int)repl.size());

            if (_keycode == KEY_SPACE) {
                batchKey(VK_SPACE, shift);
            } else if (_keycode == KEY_RETURN || _keycode == KEY_ENTER) {
                batchKey(VK_RETURN, shift);
            } else if (_keycode == KEY_TAB) {
                batchKey(VK_TAB, shift);
            } else {
                Uint16 ch = keyCodeToCharacter(_keycode | (shift ? CAPS_MASK : 0));
                if (ch) { wchar_t wc = (wchar_t)ch; batchText(&wc, 1); }
                else batchKey(_rawVkCode ? _rawVkCode : (WORD)k->vkCode, shift, _rawExtended);
            }
            flushBatch();

            startNewSession();
            ResetEnglishWord(true);
            _autoFixUndo.active = false;
            macro::reset();
            _swallowedVks[vkByte] = true;
            return 1;
        } else {
            macro::reset();
        }
    } else if (!(OTHER_CONTROL_KEY)) {
        bool upper = effectiveUpperForKey(_keycode, _flag, autoCapThisKey);
        Uint16 ch = keyCodeToCharacter(_keycode | (upper ? CAPS_MASK : 0));
        if (ch) macro::onChar((wchar_t)ch, capsLock);
    }

    if (vLanguage == 0) {
        if (vAutoSwitchByTyping && HandleEnglishKeyForAutoSwitch(autoCapThisKey)) {
            _swallowedVks[vkByte] = true;
            return 1;
        }
        if (autoCapThisKey) {
            Uint16 upperChar = keyCodeToCharacter(_keycode | CAPS_MASK);
            if (upperChar) {
                wchar_t wc = (wchar_t)upperChar;
                batchText(&wc, 1);
                flushBatch();
                _swallowedVks[vkByte] = true;
                return 1;
            }
        }
        return CallNextHookEx(_kbHook, nCode, wParam, lParam);
    }

    if (vOtherLanguage && _otherInputLanguage && imeComposing()) {
        return CallNextHookEx(_kbHook, nCode, wParam, lParam);
    }

    vAutoSwitchRequest = -1;
    int capsStatus = 0;
    if (IsLetterKey(_keycode)) {
        if (autoCapThisKey || (shift != capsLock)) {
            capsStatus = shift ? 1 : (capsLock ? 2 : 1);
        }
    } else {
        capsStatus = shift ? 1 : 0;
    }
    vKeyHandleEvent(vKeyEvent::Keyboard, vKeyEventState::KeyDown, _keycode,
                    (Uint8)capsStatus, OTHER_CONTROL_KEY);
    if (_keycode == KEY_DELETE && vKeyCurrentWordLength() == 0) {
        macro::reset();
    }
    if (pData->code == vDoNothing) {
        if (autoCapThisKey) {
            Uint16 upperChar = keyCodeToCharacter(_keycode | CAPS_MASK);
            if (upperChar) {
                wchar_t wc = (wchar_t)upperChar;
                batchText(&wc, 1);
                flushBatch();
                _swallowedVks[vkByte] = true;
                return 1;
            }
        }
        return CallNextHookEx(_kbHook, nCode, wParam, lParam);
    }
    if (pData->code == vWillProcess || pData->code == vRestore || pData->code == vRestoreAndStartNewSession) {
        bool autoSwitchAtBreak = vAutoSwitchRequest == 0 && !IsLetterKey(_keycode);
        int correctedCount = pData->newCharCount;
        SendEngineOutput();
        if (vAutoSwitchByTyping && vAutoSwitchRequest == 0) ApplyAutoSwitchToEnglish(correctedCount, autoSwitchAtBreak);
    }
    _swallowedVks[vkByte] = true;
    return 1;
}

static LRESULT CALLBACK mouseHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN)) {
        _lastFlag = 0;
        _maskPendingMods = 0;
        HWND ew = vEmojiWindow.load(std::memory_order_relaxed);
        if (vEmojiOpen.load(std::memory_order_relaxed) && ew) {
            RECT r; GetWindowRect(ew, &r); POINT p = ((MSLLHOOKSTRUCT*)lParam)->pt;
            if (!PtInRect(&r, p)) PostMessageW(ew, WM_APP + 11, 0, 0);
        }
        _autoFixUndo.active = false; TLNoteClick(); macro::reset(); _autoCapHelper.reset();
        if (vLanguage == 0) ResetEnglishWord(true);
        else vKeyHandleEvent(vKeyEvent::Mouse, vKeyEventState::MouseDown, 0);
    }
    return CallNextHookEx(_mouseHook, nCode, wParam, lParam);
}

static VOID CALLBACK foregroundHook(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    onForegroundChangedInternal();
}

static LRESULT CALLBACK hookMsgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_HOOK_SET_LANG: setLanguageByUserInternal((int)wp); return 0;
    case WM_HOOK_SET_LANG_QUIET: setLanguageQuietInternal((int)wp); return 0;
    case WM_HOOK_RELOAD_SETTINGS: {
        auto* s = (CoreSettings*)lp;
        if (s) { applySettingsOnHookThread(*s); delete s; }
        return 0;
    }
    case WM_HOOK_SITE_CHANGED: {
        auto* host = (std::wstring*)lp;
        if (host) { onSiteChangedInternal(*host); delete host; }
        return 0;
    }
    case WM_HOOK_RESET_TYPING:
        ResetEnglishWord(true); _autoFixUndo.active = false; _autoCapHelper.reset(); macro::reset(); startNewSession();
        return 0;
    case WM_HOOK_INSERT_TEXT: {
        auto* txt = (std::wstring*)lp;
        if (txt) {
            ResetEnglishWord(true); _autoFixUndo.active = false; _autoCapHelper.reset(); macro::reset(); startNewSession();
            batchText(txt->c_str(), (int)txt->size());
            flushBatch();
            delete txt;
        }
        return 0;
    }
    case WM_HOOK_RESYNC_MODS:
        resyncModifierFlags();
        _lastFlag = 0;
        _maskPendingMods = 0;
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void hookThreadProc(CoreSettings initialCfg, std::vector<BYTE> smartData) {
    _hookThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static const wchar_t* CLS = L"HKeyHookMsg";
    WNDCLASSW wc = {}; wc.lpfnWndProc = hookMsgProc; wc.hInstance = inst; wc.lpszClassName = CLS;
    RegisterClassW(&wc);
    HWND hw = CreateWindowExW(0, CLS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    _hookHwnd.store(hw, std::memory_order_relaxed);

    pData = (vKeyHookState*)vKeyInit();
    initSmartSwitchKey(smartData.data(), (int)smartData.size());
    static const std::vector<std::string> browserExes = {
        "chrome.exe", "msedge.exe", "brave.exe", "vivaldi.exe",
        "opera.exe", "opera_gx.exe", "firefox.exe", "chromium.exe", "arc.exe"
    };
    sanitizeSmartSwitchKeyEntries(browserExes);
    saveSmartSwitchKeyData();

    applySettingsOnHookThread(initialCfg);
    queryFrontMostApp();
    _tlApp = _frontApp;

    _kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, inst, 0);
    _mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, inst, 0);
    _fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundHook, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    _focusHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, [](HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) { site::pollPassword(); }, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    _hookRunning.store(_kbHook != nullptr, std::memory_order_relaxed);
    if (_hookReadyEvent) SetEvent(_hookReadyEvent);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (_kbHook) UnhookWindowsHookEx(_kbHook);
    if (_mouseHook) UnhookWindowsHookEx(_mouseHook);
    if (_fgHook) UnhookWinEvent(_fgHook);
    if (_focusHook) UnhookWinEvent(_focusHook);
    _kbHook = nullptr; _mouseHook = nullptr; _fgHook = nullptr; _focusHook = nullptr;
    _hookRunning.store(false, std::memory_order_relaxed);
    if (hw) DestroyWindow(hw);
    _hookHwnd.store(nullptr, std::memory_order_relaxed);
    _hookThreadId.store(0, std::memory_order_relaxed);
}

// ---- Public API ----------------------------------------------------------------------------
bool start() {
    if (_hookThread.joinable()) return _hookRunning.load(std::memory_order_relaxed);
    site::startBackgroundWorker();
    macro::init();
    CoreSettings initialCfg = readSettingsSnapshot();
    std::vector<BYTE> smartData = prefs::getBinary(L"smartSwitchKey");

    _hookReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    _hookThread = std::thread(hookThreadProc, std::move(initialCfg), std::move(smartData));
    if (_hookReadyEvent) {
        WaitForSingleObject(_hookReadyEvent, 3000);
        CloseHandle(_hookReadyEvent);
        _hookReadyEvent = nullptr;
    }
    return _hookRunning.load(std::memory_order_relaxed);
}

void stop() {
    site::stopBackgroundWorker();
    DWORD tid = _hookThreadId.load(std::memory_order_relaxed);
    if (tid) PostThreadMessageW(tid, WM_QUIT, 0, 0);
    if (_hookThread.joinable()) _hookThread.join();
    flushTypingLog();
}

bool isRunning() { return _hookRunning.load(std::memory_order_relaxed); }
int language() { return _atomicLanguage.load(std::memory_order_relaxed); }
bool isBlacklistActive() { return _atomicBlacklistActive.load(std::memory_order_relaxed); }
void setRecordingHotkey(bool recording) { _recordingHotkey.store(recording, std::memory_order_relaxed); }

void setUiNotificationWindow(HWND hwnd, UINT asyncMsg) {
    _uiHwnd.store(hwnd, std::memory_order_relaxed);
    _uiAsyncMsg.store(asyncMsg, std::memory_order_relaxed);
}

void processUiAsyncNotifications() {
    int obsLang = -1;
    int saveIm = -1;
    bool hasSs = false;
    std::vector<Byte> ssBytes;
    std::map<std::wstring, int> counters;
    bool doFlush = false;
    bool doEmoji = false;
    int soundLang = -1;
    {
        std::lock_guard<std::mutex> lk(_async.mu);
        obsLang = _async.pendingObserverLang; _async.pendingObserverLang = -1;
        saveIm = _async.pendingInputMethod; _async.pendingInputMethod = -1;
        if (_async.hasSmartSwitchSave) {
            hasSs = true;
            ssBytes.swap(_async.smartSwitchBytes);
            _async.hasSmartSwitchSave = false;
        }
        counters.swap(_async.tlCounters);
        doFlush = _async.pendingLogFlush; _async.pendingLogFlush = false;
        doEmoji = _async.pendingEmoji; _async.pendingEmoji = false;
        soundLang = _async.pendingSoundLang; _async.pendingSoundLang = -1;
    }

    if (soundLang >= 0) PlaySwitchSound(soundLang);
    if (hasSs) prefs::setBinary(L"smartSwitchKey", ssBytes.data(), (DWORD)ssBytes.size());
    if (saveIm >= 0) prefs::setInt(L"InputMethod", saveIm);
    for (const auto& kv : counters) {
        prefs::setInt(kv.first.c_str(), prefs::getInt(kv.first.c_str(), 0) + kv.second);
    }
    if (obsLang >= 0) {
        std::function<void(int)> obs;
        { std::lock_guard<std::mutex> lk(_callbackMutex); obs = _observer; }
        if (obs) obs(obsLang);
    }
    if (doEmoji) {
        std::function<void()> eh;
        { std::lock_guard<std::mutex> lk(_callbackMutex); eh = _emojiHandler; }
        if (eh) eh();
    }
    if (doFlush) {
        std::string chunk;
        { std::lock_guard<std::mutex> g(_tlMutex); chunk.swap(_tlPending); }
        if (!chunk.empty()) {
            std::wstring path = typingLogPath();
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && (fad.nFileSizeHigh || fad.nFileSizeLow > 20u * 1024 * 1024)) {
                std::wstring old = path.substr(0, path.size() - 6) + L".1.jsonl";
                DeleteFileW(old.c_str()); MoveFileW(path.c_str(), old.c_str());
            }
            HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, chunk.data(), (DWORD)chunk.size(), &w, nullptr); CloseHandle(h); }
        }
    }
}

void setLanguageByUser(int lang) {
    HWND hw = _hookHwnd.load(std::memory_order_relaxed);
    if (hw) PostMessageW(hw, WM_HOOK_SET_LANG, (WPARAM)lang, 0);
}
void setLanguageQuiet(int lang) {
    HWND hw = _hookHwnd.load(std::memory_order_relaxed);
    if (hw) SendMessageW(hw, WM_HOOK_SET_LANG_QUIET, (WPARAM)lang, 0);
}
void reloadSettings() {
    HWND hw = _hookHwnd.load(std::memory_order_relaxed);
    if (hw) {
        auto* s = new CoreSettings(readSettingsSnapshot());
        if (!PostMessageW(hw, WM_HOOK_RELOAD_SETTINGS, 0, (LPARAM)s)) delete s;
    }
}
void onForegroundChanged() {
    // Foreground WinEventHook is registered directly on the hook thread; kept for API compatibility.
}
void onSiteChanged(const std::wstring& host) {
    HWND hw = _hookHwnd.load(std::memory_order_relaxed);
    if (hw) {
        auto* h = new std::wstring(host);
        if (!PostMessageW(hw, WM_HOOK_SITE_CHANGED, 0, (LPARAM)h)) delete h;
    }
}
void setLanguageObserver(std::function<void(int)> f) {
    std::lock_guard<std::mutex> lk(_callbackMutex);
    _observer = std::move(f);
}
void setEmojiHotkeyHandler(std::function<void()> f) {
    std::lock_guard<std::mutex> lk(_callbackMutex);
    _emojiHandler = std::move(f);
}
void setEmojiPickerOpen(bool open, HWND picker) {
    vEmojiWindow.store(picker, std::memory_order_relaxed);
    vEmojiOpen.store(open, std::memory_order_relaxed);
}
void resetTyping() {
    HWND hw = _hookHwnd.load(std::memory_order_relaxed);
    if (hw) PostMessageW(hw, WM_HOOK_RESET_TYPING, 0, 0);
}
void insertText(const std::wstring& text) {
    HWND hw = _hookHwnd.load(std::memory_order_relaxed);
    if (hw) {
        auto* p = new std::wstring(text);
        if (!PostMessageW(hw, WM_HOOK_INSERT_TEXT, 0, (LPARAM)p)) delete p;
    }
}
void resyncModifiers() {
    HWND hw = _hookHwnd.load(std::memory_order_relaxed);
    if (hw) PostMessageW(hw, WM_HOOK_RESYNC_MODS, 0, 0);
}

}} // namespace vikey::core
