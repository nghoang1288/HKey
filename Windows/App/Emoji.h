// Emoji picker: Direct2D + DirectWrite popup drawn borderless with WS_EX_NOACTIVATE.
#pragma once
#include <windows.h>

namespace vikey { namespace emoji {
void toggle();                  // opens the picker, or closes it if open
void languageChanged(int lang); // updates the picker's language segment if open
void settingsChanged();         // after EmojiPicker / EmojiHotkey prefs change
}}

namespace hkey { namespace emoji {
    using namespace vikey::emoji;
}}
