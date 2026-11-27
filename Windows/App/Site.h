// Per-website memory: the host of the active tab of the browser in front, read through UI Automation.
#pragma once
#include <windows.h>
#include <string>

namespace vikey { namespace site {
bool isBrowser(const std::wstring& exe);
void setNotificationWindow(HWND hwnd, UINT msg);
/// Reads the address bar of the foreground browser; empty when not a browser or no URL.
std::wstring currentHost();
/// Call every second while a browser is in front (and shortly after clicks / tab shortcuts).
void poll();
/// True while a password field has the focus or focus state is undetermined (Fail-Closed).
bool focusedIsPassword();
/// True only when UIA has explicitly confirmed the focused control is a password field (Fail-Open).
bool isConfirmedPasswordField();
/// Re-checks the focused control now (call on focus changes; poll() does it too).
void pollPassword();
/// Background worker thread lifecycle
void startBackgroundWorker();
void stopBackgroundWorker();
}}

namespace hkey { namespace site {
    using namespace vikey::site;
}}
