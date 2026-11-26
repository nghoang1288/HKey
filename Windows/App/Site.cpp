#include "Site.h"
#include "Core.h"
#include "Prefs.h"
#undef WIN32_LEAN_AND_MEAN   // UIAutomation needs the OLE/RPC types windows.h leaves out
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <thread>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace vikey { namespace site {

bool isBrowser(const std::wstring& exeIn) {
    std::wstring exe = exeIn;
    std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);
    static const wchar_t* list[] = {
        L"chrome.exe", L"msedge.exe", L"brave.exe", L"vivaldi.exe",
        L"opera.exe", L"opera_gx.exe", L"firefox.exe", L"chromium.exe", L"arc.exe"
    };
    for (auto b : list) if (exe == b) return true;
    return false;
}

static std::atomic<HWND> _notifyHwnd{nullptr};
static std::atomic<UINT> _notifyMsg{0};
void setNotificationWindow(HWND hwnd, UINT msg) {
    _notifyHwnd.store(hwnd, std::memory_order_relaxed);
    _notifyMsg.store(msg, std::memory_order_relaxed);
}

static bool looksLikeUrl(const std::wstring& v) {
    if (v.empty() || v.size() > 2048) return false;
    for (wchar_t c : v) {
        if (c <= 32) return false;
    }
    if (v.rfind(L"http://", 0) == 0 || v.rfind(L"https://", 0) == 0) return true;
    size_t dot = v.find(L'.');
    return dot != std::wstring::npos && dot > 0 && dot + 1 < v.size();
}

static std::wstring hostOf(std::wstring v) {
    if (v.rfind(L"file:", 0) == 0) return L"";   // a local file has no site
    size_t p = v.find(L"://");
    if (p != std::wstring::npos) v = v.substr(p + 3);
    size_t endAuth = v.find_first_of(L"/?# \t\r\n");
    if (endAuth != std::wstring::npos) v = v.substr(0, endAuth);
    size_t at = v.rfind(L'@');
    if (at != std::wstring::npos) v = v.substr(at + 1);
    size_t colon = v.find(L':');
    if (colon != std::wstring::npos) v = v.substr(0, colon);
    std::transform(v.begin(), v.end(), v.begin(), ::towlower);
    if (v.rfind(L"www.", 0) == 0) v = v.substr(4);
    if (v.find(L'.') == std::wstring::npos && v != L"localhost") return L"";
    return v;
}

static bool ownWindowInFront() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static std::wstring exeForWindow(HWND fg) {
    if (!fg) return L"";
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid || pid == GetCurrentProcessId()) return L"";
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return L"";
    wchar_t buf[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring exe;
    if (QueryFullProcessImageNameW(hProc, 0, buf, &size)) {
        const wchar_t* slash = wcsrchr(buf, L'\\');
        exe = slash ? (slash + 1) : buf;
    }
    CloseHandle(hProc);
    return exe;
}

// Background thread state
static std::thread _workerThread;
static std::atomic<bool> _stopWorker{false};
static bool _wakeRequested = false;   // guarded by _wakeMutex
static std::mutex _wakeMutex;
static std::condition_variable _wakeCv;
static std::mutex _lifecycleMutex;

// Cached values - Fail-closed: default to password/unsafe until determined
static std::atomic<bool> _focusPassword{true};
static std::atomic<bool> _focusDetermined{false};
static std::mutex _hostMutex;
static std::wstring _cachedHost;

bool focusedIsPassword() {
    if (ownWindowInFront()) return false;
    if (!_focusDetermined.load(std::memory_order_relaxed)) return true; // Fail-closed
    return _focusPassword.load(std::memory_order_relaxed);
}

std::wstring currentHost() {
    std::lock_guard<std::mutex> lock(_hostMutex);
    return _cachedHost;
}

static void pollPasswordInternal(IUIAutomation* uia) {
    if (ownWindowInFront()) {
        _focusPassword.store(false, std::memory_order_relaxed);
        _focusDetermined.store(true, std::memory_order_relaxed);
        return;
    }
    if (!uia) {
        _focusPassword.store(true, std::memory_order_relaxed);
        _focusDetermined.store(false, std::memory_order_relaxed);
        return;
    }
    IUIAutomationElement* el = nullptr;
    if (FAILED(uia->GetFocusedElement(&el)) || !el) {
        _focusPassword.store(true, std::memory_order_relaxed);
        _focusDetermined.store(false, std::memory_order_relaxed);
        return;
    }
    BOOL pw = FALSE;
    el->get_CurrentIsPassword(&pw);
    el->Release();
    _focusPassword.store(pw != FALSE, std::memory_order_relaxed);
    _focusDetermined.store(true, std::memory_order_relaxed);
}

static bool isAddressBarHint(IUIAutomationElement* el) {
    BSTR aid = nullptr;
    bool hit = false;
    if (SUCCEEDED(el->get_CurrentAutomationId(&aid)) && aid) {
        if (wcsstr(aid, L"Omnibox") || wcsstr(aid, L"urlbar") || wcsstr(aid, L"address") || wcsstr(aid, L"Address")) {
            hit = true;
        }
        SysFreeString(aid);
    }
    if (hit) return true;
    BSTR name = nullptr;
    if (SUCCEEDED(el->get_CurrentName(&name)) && name) {
        if (wcsstr(name, L"Address") || wcsstr(name, L"address") || wcsstr(name, L"Tìm kiếm") || wcsstr(name, L"URL")) {
            hit = true;
        }
        SysFreeString(name);
    }
    return hit;
}

static bool currentHostInternal(IUIAutomation* uia) {
    if (ownWindowInFront()) return false;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    wchar_t cls[128] = {0};
    GetClassNameW(fg, cls, 128);
    if (core::isIgnoredSystemUiWindow(cls, nullptr)) return false;

    std::wstring exe = exeForWindow(fg);
    if (core::isIgnoredSystemUiWindow(nullptr, exe.c_str())) return false;
    if (!isBrowser(exe) || !uia) {
        std::lock_guard<std::mutex> lock(_hostMutex);
        bool changed = !_cachedHost.empty();
        _cachedHost.clear();
        return changed;
    }

    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->ElementFromHandle(fg, &root)) || !root) return false;

    std::wstring host;
    std::wstring fallbackHost;
    VARIANT vt; vt.vt = VT_I4; vt.lVal = UIA_EditControlTypeId;
    IUIAutomationCondition* cond = nullptr;
    if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, &cond)) && cond) {
        IUIAutomationElementArray* arr = nullptr;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
            int n = 0; arr->get_Length(&n);
            int limit = (n > 256) ? 256 : n;
            for (int i = 0; i < limit; i++) {
                IUIAutomationElement* el = nullptr;
                if (FAILED(arr->GetElement(i, &el)) || !el) continue;
                IUIAutomationValuePattern* vp = nullptr;
                if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern, (void**)&vp)) && vp) {
                    BSTR b = nullptr;
                    if (SUCCEEDED(vp->get_CurrentValue(&b)) && b) {
                        std::wstring v(b, SysStringLen(b));
                        SysFreeString(b);
                        if (looksLikeUrl(v)) {
                            std::wstring h = hostOf(v);
                            if (!h.empty()) {
                                if (isAddressBarHint(el)) {
                                    host = h;
                                    vp->Release();
                                    el->Release();
                                    break;
                                }
                                if (fallbackHost.empty()) fallbackHost = h;
                            }
                        }
                    }
                    vp->Release();
                }
                el->Release();
            }
            arr->Release();
        }
        cond->Release();
    }
    root->Release();

    if (host.empty()) host = fallbackHost;

    // Discard stale UIA scan if user switched windows while UIA was scanning
    if (GetForegroundWindow() != fg) return false;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(_hostMutex);
        if (_cachedHost != host) {
            _cachedHost = host;
            changed = true;
        }
    }
    return changed;
}

static void workerLoop() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    }

    while (!_stopWorker.load(std::memory_order_relaxed)) {
        {
            std::lock_guard<std::mutex> lock(_wakeMutex);
            _wakeRequested = false;
        }

        pollPasswordInternal(uia);
        bool perSite = prefs::getInt(L"SmartSwitchPerSite", 1) != 0;
        if (perSite) {
            bool hostChanged = currentHostInternal(uia);
            HWND hwnd = _notifyHwnd.load(std::memory_order_relaxed);
            UINT msg = _notifyMsg.load(std::memory_order_relaxed);
            if (hostChanged && hwnd && msg) {
                PostMessageW(hwnd, msg, 0, 0);
            }
        }

        std::unique_lock<std::mutex> lock(_wakeMutex);
        _wakeCv.wait_for(lock, std::chrono::milliseconds(5000), [] {
            return _wakeRequested || _stopWorker.load(std::memory_order_relaxed);
        });
    }

    if (uia) {
        uia->Release();
        uia = nullptr;
    }
    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
}

void startBackgroundWorker() {
    std::lock_guard<std::mutex> lk(_lifecycleMutex);
    if (_workerThread.joinable()) return;
    _stopWorker.store(false);
    _workerThread = std::thread(workerLoop);
}

void stopBackgroundWorker() {
    std::lock_guard<std::mutex> lk(_lifecycleMutex);
    _stopWorker.store(true);
    {
        std::lock_guard<std::mutex> wlk(_wakeMutex);
        _wakeRequested = true;
    }
    _wakeCv.notify_all();
    if (_workerThread.joinable()) {
        _workerThread.join();
    }
}

static void wakeWorker() {
    {
        std::lock_guard<std::mutex> lock(_wakeMutex);
        _wakeRequested = true;
    }
    _wakeCv.notify_one();
}

void pollPassword() {
    startBackgroundWorker();
    if (ownWindowInFront()) {
        _focusPassword.store(false, std::memory_order_relaxed);
        _focusDetermined.store(true, std::memory_order_relaxed);
        return;
    }
    _focusDetermined.store(false, std::memory_order_relaxed);
    wakeWorker();
}

void poll() {
    startBackgroundWorker();
    if (ownWindowInFront()) {
        _focusPassword.store(false, std::memory_order_relaxed);
        _focusDetermined.store(true, std::memory_order_relaxed);
        return;
    }
    if (!prefs::getInt(L"SmartSwitchPerSite", 1)) {
        HWND hwnd = _notifyHwnd.load(std::memory_order_relaxed);
        UINT msg = _notifyMsg.load(std::memory_order_relaxed);
        bool hadHost = false;
        {
            std::lock_guard<std::mutex> lock(_hostMutex);
            hadHost = !_cachedHost.empty();
            _cachedHost.clear();
        }
        if (hadHost && hwnd && msg) PostMessageW(hwnd, msg, 0, 0);
        return;
    }
    HWND fg = GetForegroundWindow();
    if (!fg || !isBrowser(exeForWindow(fg))) {
        std::lock_guard<std::mutex> lock(_hostMutex);
        _cachedHost.clear();
        return;
    }
    wakeWorker();
}

}} // namespace vikey::site
