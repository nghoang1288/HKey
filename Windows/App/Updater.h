#pragma once
#include <windows.h>
namespace vikey { namespace updater {
const UINT WM_UPDATER = WM_APP + 4;   // posted to the owner: wParam = Event, lParam = manual (1/0)
enum Event { EvUpToDate = 1, EvFailed, EvAvailable, EvDownloaded };
int handleUpdateCommandLine(LPWSTR cmdLine);  // returns >= 0 if launched as a portable update helper and should exit
void start(HWND owner);                       // first check after a short delay, then daily
void checkNow(HWND owner);                    // manual check; the result is shown to the user
void onEvent(WPARAM ev, LPARAM lp = 1);       // the owner's handler for WM_UPDATER
bool installPendingAndQuit();                 // at quit: applies a downloaded, verified update; true if it did
bool installPendingAtLaunch();                // at launch: a verified newer update waits -> apply it and exit
}}

namespace hkey { namespace updater {
    using namespace vikey::updater;
}}
