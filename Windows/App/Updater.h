#pragma once
#include <windows.h>
namespace vikey { namespace updater {
const UINT WM_UPDATER = WM_APP + 4;   // posted to the owner: wParam = Event
enum Event { EvUpToDate = 1, EvFailed, EvAvailable, EvDownloaded };
void start(HWND owner);               // first check after a short delay, then daily
void checkNow(HWND owner);            // manual check; the result is shown to the user
void onEvent(WPARAM ev);              // the owner's handler for WM_UPDATER
bool installPendingAndQuit();         // at quit: runs a downloaded, verified installer; true if it did
bool installPendingAtLaunch();        // at launch: a verified newer installer waits → run it and exit
}}

namespace hkey { namespace updater {
    using namespace vikey::updater;
}}
