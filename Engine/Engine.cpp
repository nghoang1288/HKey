//
//  Engine.cpp
//  Vikey (based on OpenKey)
//
//  Created by Tuyen on 1/18/19.
//  Copyright © 2019 Tuyen Mai. All rights reserved.
//
#include <iostream>
#include <algorithm>
#include "Engine.h"
#include <string.h>
#include <cmath>
#include <deque>
#include "AutoSwitchData.h"
#include "BigramData.h"
#include <string>
#include <unordered_map>
#include <cstdio>

using namespace std;

static vector<Uint8> _charKeyCode = {
    KEY_BACKQUOTE, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUALS,
    KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET, KEY_BACK_SLASH,
    KEY_SEMICOLON, KEY_QUOTE, KEY_COMMA, KEY_DOT, KEY_SLASH
};

static vector<Uint8> _breakCode = {
    KEY_ESC, KEY_TAB, KEY_ENTER, KEY_RETURN, KEY_LEFT, KEY_RIGHT, KEY_DOWN, KEY_UP, KEY_COMMA, KEY_DOT,
    KEY_SLASH, KEY_SEMICOLON, KEY_QUOTE, KEY_BACK_SLASH, KEY_MINUS, KEY_EQUALS, KEY_BACKQUOTE, KEY_TAB
    // The engine always uses the macOS key codes (platforms/mac.h); every platform layer maps its own
    // codes to these (Windows: VK_* -> KEY_*, with Insert/Home/End/Delete/PgUp/PgDn sent as KEY_ESC).
};


//Telex keys: tones s f r x j, doubled a o e (circumflex), w (horn/breve), d (đ), z (remove tone)
int vRotationalTone = 0;
static const Uint16 TelexKey[11] = {KEY_S, KEY_F, KEY_R, KEY_X, KEY_J, KEY_A, KEY_O, KEY_E, KEY_W, KEY_D, KEY_Z};

#define IS_KEY_Z(key) (TelexKey[10] == key)
#define IS_KEY_D(key) (TelexKey[9] == key)
#define IS_KEY_W(key) (TelexKey[8] == key)
#define IS_KEY_DOUBLE(key) (TelexKey[5] == key || TelexKey[6] == key || TelexKey[7] == key)
#define IS_KEY_S(key) (TelexKey[0] == key)
#define IS_KEY_F(key) (TelexKey[1] == key)
#define IS_KEY_R(key) (TelexKey[2] == key)
#define IS_KEY_X(key) (TelexKey[3] == key)
#define IS_KEY_J(key) (TelexKey[4] == key)

#define IS_MARK_KEY(keyCode) (keyCode == KEY_S || keyCode == KEY_F || keyCode == KEY_R || keyCode == KEY_J || keyCode == KEY_X)
#define IS_BRACKET_KEY(key) (key == KEY_LEFT_BRACKET || key == KEY_RIGHT_BRACKET)

#define VSI vowelStartIndex
#define VEI vowelEndIndex
#define VWSM vowelWillSetMark
#define hBPC HookState.backspaceCount
#define hNCC HookState.newCharCount
#define hCode HookState.code
#define hExt HookState.extCode
#define hData HookState.charData
#define GET getCharacterCode

//Data to sendback to main program
vKeyHookState HookState;

//private data
/**
 * data structure of each element in TypingWord (Uint64)
 * first 2 byte is character code or key code.
 * bit 16: has caps or not
 * bit 17: has tone ^ or not
 * bit 18: has tone w or not
 * bit 19 - > 23: has mark or not (Sắc, huyền, hỏi, ngã, nặng)
 * bit 24: is standalone key? (w, [, ])
 * bit 25: is character code or keyboard code; 1: character code; 0: keycode
 */
static Uint32 TypingWord[MAX_BUFF];
static Byte _index = 0;
struct TypingStateSnapshot {
    vector<Uint32> word;
    vector<Uint32> keys;
};
static constexpr size_t MAX_TYPING_HISTORY = 32;
static vector<Uint32> _longWordHelper; //save the word when _index >= MAX_BUFF
static deque<TypingStateSnapshot> _typingStates; //Aug 28th, 2019: typing helper, save long state of Typing word, can go back and modify the word
vector<Uint32> _typingStatesData;

/**
 * Use for restore key if invalid word
 */
static Uint32 KeyStates[MAX_BUFF];
//auto switch: keys Telex undid and dropped from the key states ("eee" -> "ee", "deepseek"), and where each
//was, oldest first (each position counts the keys kept at that moment)
#define AS_MAX_DROPS 4
static int _asDropN = 0;
static int _asDropPos[AS_MAX_DROPS];
static Uint32 _asDropKey[AS_MAX_DROPS];
/** The keys as typed: the key states with the dropped keys put back. Returns how many. */
static int asStatesWithDrops(Uint32* full, const int& maxN);
static Byte _stateIndex = 0;

static bool tempDisableKey = false;
static int capsElem;
static int key;
static int markElem;
static bool isCorect = false;
static bool isChanged = false;
static Byte vowelCount = 0;
static Byte vowelStartIndex = 0;
static Byte vowelEndIndex = 0;
static Byte vowelWillSetMark = 0;
static int i, ii, iii;
static int j;
static int k, kk;
static int l;
static bool isRestoredW;
static Uint16 keyForAEO;
static bool isCheckedGrammar;
static bool _isCaps = false;
static int _spaceCount = 0; //add: July 30th, 2019
static bool _isCharKeyCode;
static vector<Uint32> _specialChar;
static bool _useSpellCheckingBefore;

//function prototype
void findAndCalculateVowel(const bool& forGrammar=false);
void insertMark(const Uint32& markMask, const bool& canModifyFlag=true);

#define AS_MISSING -1000
#define AS_KEY_LEN 40
static void asInitLetters();
static bool asRawWord(const Uint32* keys, const int& count, char* out);
static inline int asEnglishScore(const char* word);

wstring utf8ToWideString(const string& str) {
    wstring out;
    size_t idx = 0;
    while (idx < str.size()) {
        Uint32 cp = 0;
        unsigned char c = (unsigned char)str[idx];
        if (c < 0x80) { cp = c; idx += 1; }
        else if ((c & 0xE0) == 0xC0 && idx + 1 < str.size()) {
            cp = ((c & 0x1F) << 6) | ((unsigned char)str[idx + 1] & 0x3F); idx += 2;
        } else if ((c & 0xF0) == 0xE0 && idx + 2 < str.size()) {
            cp = ((c & 0x0F) << 12) | (((unsigned char)str[idx + 1] & 0x3F) << 6) | ((unsigned char)str[idx + 2] & 0x3F); idx += 3;
        } else if ((c & 0xF8) == 0xF0 && idx + 3 < str.size()) {
            cp = ((c & 0x07) << 18) | (((unsigned char)str[idx + 1] & 0x3F) << 12) | (((unsigned char)str[idx + 2] & 0x3F) << 6) | ((unsigned char)str[idx + 3] & 0x3F); idx += 4;
        } else { cp = 0xFFFD; idx += 1; }
        if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back((wchar_t)(0xD800 + (cp >> 10)));
            out.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back((wchar_t)cp);
        }
    }
    return out;
}

string wideStringToUtf8(const wstring& str) {
    string out;
    for (size_t idx = 0; idx < str.size(); idx++) {
        Uint32 cp = (Uint32)str[idx];
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && idx + 1 < str.size()) {
            Uint32 lo = (Uint32)str[idx + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); idx++; }
        }
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else { out.push_back((char)(0xF0 | (cp >> 18))); out.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    }
    return out;
}

void* vKeyInit() {
    _index = 0;
    _stateIndex = 0;
    memset(TypingWord, 0, sizeof(TypingWord));
    memset(KeyStates, 0, sizeof(KeyStates));
    _useSpellCheckingBefore = vCheckSpelling;
    _typingStatesData.clear();
    _typingStates.clear();
    _longWordHelper.clear();
    return &HookState;
}

bool isWordBreak(const vKeyEvent& event, const vKeyEventState& state, const Uint16& data) {
    if (event == vKeyEvent::Mouse)
        return true;
    for (i = 0; i < _breakCode.size(); i++) {
        if (_breakCode[i] == data) {
            return true;
        }
    }
    return false;
}


void setKeyData(const Byte& index, const Uint16& keyCode, const bool& isCaps) {
    if (index < 0 || index >= MAX_BUFF)
        return;
    TypingWord[index] = keyCode | (isCaps ? CAPS_MASK : 0);
}

bool _spellingOK = false;
bool _spellingFlag = false;
bool _spellingVowelOK = false;
Byte _spellingEndIndex = 0;

void checkSpelling(const bool& forceCheckVowel=false) {
    _spellingOK = false;
    _spellingVowelOK = true;
    _spellingEndIndex = _index;
    
    if (_index > 0 && CHR(_index-1) == KEY_RIGHT_BRACKET) {
        _spellingEndIndex = _index-1;
    }
    
    if (_spellingEndIndex > 0) {
        j = 0;
        //Check first consonant
        if (IS_CONSONANT(CHR(0))) {
            for (i = 0; i < _consonantTable.size(); i++) {
                _spellingFlag = false;
                if (_spellingEndIndex < _consonantTable[i].size())
                    _spellingFlag = true;
                for (j = 0; j < _consonantTable[i].size(); j++) {
                    if (_spellingEndIndex > j &&
                        _consonantTable[i][j] != CHR(j)) {
                        _spellingFlag = true;
                        break;
                    }
                }
                if (_spellingFlag)
                    continue;
                
                break;
            }
        }
        
        if (j == _spellingEndIndex){ //for "d" case
            _spellingOK = true;
        }
        
        //check next vowel
        k = j;
        VSI = k;
        //August 23rd, 2019: fix case "que't"
        if (CHR(VSI) == KEY_U && k > 0 && k < _spellingEndIndex-1 && CHR(VSI-1) == KEY_Q) {
            k = k + 1;
            j = k;
            VSI = k;
        } else if (_spellingEndIndex >= 3 && CHR(0) == KEY_G && CHR(1) == KEY_I && IS_CONSONANT(CHR(2))) {
            VSI = k = j = 1; //Sep 28th: fix gìn
        }
        for (l = 0; l < 3; l++) {
            if (k < _spellingEndIndex && !IS_CONSONANT(CHR(k))) {
                k++;
                VEI = k;
            }
        }
        if (k > j) { //has vowel,
            _spellingVowelOK = false;
            //check correct combined vowel
            if (k - j > 1 && forceCheckVowel) {
                vector<vector<Uint32>>& vowelSet = _vowelCombine[CHR(j)];
                for (l = 0; l < vowelSet.size(); l++) {
                    _spellingFlag = false;
                    for (ii = 1; ii < vowelSet[l].size(); ii++) {
                        if (j + ii - 1 < _spellingEndIndex && vowelSet[l][ii] != ((CHR(j + ii - 1) | (TypingWord[j + ii - 1] & TONEW_MASK) | (TypingWord[j + ii - 1] & TONE_MASK)))) {
                            _spellingFlag = true;
                            break;
                        }
                    }
                    if (_spellingFlag || (k < _spellingEndIndex && !vowelSet[l][0]) || (j + ii - 1 < _spellingEndIndex && !IS_CONSONANT(CHR(j + ii - 1))))
                        continue;
                    
                    _spellingVowelOK = true;
                    break;
                }
            } else if (!IS_CONSONANT(CHR(j))) {
                _spellingVowelOK = true;
            }
            
            //continue check last consonant
            for (ii = 0; ii < _endConsonantTable.size(); ii++) {
                _spellingFlag = false;
   
                for (j = 0; j < _endConsonantTable[ii].size(); j++) {
                    if (_spellingEndIndex > k+j &&
                        _endConsonantTable[ii][j] != CHR(k + j)) {
                        _spellingFlag = true;
                        break;
                    }
                }
                if (_spellingFlag)
                    continue;
                
                if (k + j >= _spellingEndIndex) {
                    _spellingOK = true;
                    break;
                }
            }
            
            //limit: end consonant "ch", "t" can not use with "~", "`", "?"
            if (_spellingOK) {
                if (_index >= 3 && CHR(_index-1) == KEY_H && CHR(_index-2) == KEY_C && !((TypingWord[_index-3] & MARK1_MASK) || (TypingWord[_index-3] & MARK5_MASK) || !(TypingWord[_index-3] & MARK_MASK))) {
                    _spellingOK = false;
                } else if (_index >= 2 && CHR(_index-1) == KEY_T && !((TypingWord[_index-2] & MARK1_MASK) || (TypingWord[_index-2] & MARK5_MASK) || !(TypingWord[_index-2] & MARK_MASK))) {
                    _spellingOK = false;
                }
            }
        }
    } else {
        _spellingOK = true;
    }
    tempDisableKey = !(_spellingOK && _spellingVowelOK);
    
    //cout<<"spelling vowel: "<<(_spellingVowelOK ? "OK": "Err")<<endl;
    //cout<<"spelling: "<<(_spellingOK ? "OK": "Err")<<endl<<endl;
}

void checkGrammar(const int& deltaBackSpace) {
    if (_index <= 1 || _index >= MAX_BUFF)
        return;
    
    findAndCalculateVowel(true);
    if (vowelCount == 0)
        return;
    
    isCheckedGrammar = false;
    
    int startPos = VSI;
    
    // 1. Fast typing auto-fix: inverted end consonants (gn -> ng, hn -> nh, hc -> ch)
    // E.g. "khôgn" -> "không", "mìhn" -> "mình", "tíhc" -> "tích", "trogn" -> "trong"
    if (_index >= 3) {
        Uint16 c1 = CHR(_index - 2);
        Uint16 c2 = CHR(_index - 1);
        bool inverted = false;
        if (c1 == KEY_G && c2 == KEY_N) inverted = true;      // gn -> ng
        else if (c1 == KEY_H && c2 == KEY_N) inverted = true; // hn -> nh
        else if (c1 == KEY_H && c2 == KEY_C) inverted = true; // hc -> ch

        if (inverted && !IS_CONSONANT(CHR(_index - 3))) {
            bool hasVnMark = false;
            for (int x = 0; x < _index; x++) {
                if (TypingWord[x] & (MARK_MASK | TONE_MASK | TONEW_MASK)) { hasVnMark = true; break; }
                if (CHR(x) == KEY_D && (TypingWord[x] & TONE_MASK)) { hasVnMark = true; break; }
            }
            bool isEnglishWord = false;
            if (!hasVnMark) {
                asInitLetters();
                char raw[AS_KEY_LEN];
                if (asRawWord(KeyStates, _stateIndex, raw) && asEnglishScore(raw) != AS_MISSING) {
                    isEnglishWord = true;
                }
            }
            if (!isEnglishWord) {
                std::swap(TypingWord[_index - 2], TypingWord[_index - 1]);
                if (_stateIndex >= 2) {
                    std::swap(KeyStates[_stateIndex - 2], KeyStates[_stateIndex - 1]);
                }
                if (vCheckSpelling) {
                    checkSpelling(true);
                    if (tempDisableKey) {
                        // Not a valid Vietnamese word even after swap (e.g. English word "design", "foreign")
                        // Revert swap immediately to keep raw keystrokes untouched
                        std::swap(TypingWord[_index - 2], TypingWord[_index - 1]);
                        if (_stateIndex >= 2) {
                            std::swap(KeyStates[_stateIndex - 2], KeyStates[_stateIndex - 1]);
                        }
                        checkSpelling(true);
                    } else {
                        isCheckedGrammar = true;
                        tempDisableKey = false;
                    }
                } else {
                    isCheckedGrammar = true;
                    tempDisableKey = false;
                }
            }
        }
    }

    // 2. Diphthong uo horn and tone alignment (e.g. "ngừoi" -> "người", "thuơn" -> "thươn")
    if (_index >= 3) {
        for (int idx = _index - 1; idx >= 0; idx--) {
            if (CHR(idx) == KEY_N || CHR(idx) == KEY_C || CHR(idx) == KEY_I ||
                CHR(idx) == KEY_M || CHR(idx) == KEY_P || CHR(idx) == KEY_T ||
                (idx + 1 < _index && CHR(idx) == KEY_N && CHR(idx + 1) == KEY_G)) {
                if (idx - 2 >= 0 && CHR(idx - 1) == KEY_O && CHR(idx - 2) == KEY_U) {
                    if ((TypingWord[idx - 1] & TONEW_MASK) ^ (TypingWord[idx - 2] & TONEW_MASK)) {
                        TypingWord[idx - 2] |= TONEW_MASK;
                        TypingWord[idx - 1] |= TONEW_MASK;
                        isCheckedGrammar = true;
                    }
                    // Transfer any mark from u to o (which is now ơ)
                    Uint32 uMark = TypingWord[idx - 2] & MARK_MASK;
                    if (uMark) {
                        TypingWord[idx - 2] &= ~MARK_MASK;
                        TypingWord[idx - 1] |= uMark;
                        isCheckedGrammar = true;
                    }
                    if (isCheckedGrammar) break;
                }
            }
        }
    }

    // 3. Adjacent key typo: "nhĩng" -> "những", "cĩng" -> "cũng", "đĩng" -> "đứng"
    bool handledAdj = false;
    if (_index >= 4 && CHR(_index - 1) == KEY_G && CHR(_index - 2) == KEY_N && CHR(_index - 3) == KEY_I) {
        Uint32 iMark = TypingWord[_index - 3] & MARK_MASK;
        if (iMark) {
            bool isNh = (_index >= 5 && CHR(_index - 4) == KEY_H && CHR(_index - 5) == KEY_N);
            bool isC  = (_index == 4 && CHR(0) == KEY_C);
            bool isD  = (_index == 4 && CHR(0) == KEY_D);
            if (isNh || isC || isD) {
                bool hasOtherVnMark = false;
                for (int x = 0; x < _index; x++) {
                    if (x != _index - 3 && (TypingWord[x] & (MARK_MASK | TONE_MASK | TONEW_MASK))) { hasOtherVnMark = true; break; }
                }
                bool isEnglishWord = false;
                if (!hasOtherVnMark) {
                    asInitLetters();
                    char raw[AS_KEY_LEN];
                    if (asRawWord(KeyStates, _stateIndex, raw) && asEnglishScore(raw) != AS_MISSING) {
                        isEnglishWord = true;
                    }
                }
                if (!isEnglishWord) {
                    bool isDdung = (isD && (TypingWord[0] & TONE_MASK)); // 'đ'
                    bool useHorn = (isNh || isDdung);
                    Uint32 capsMask = TypingWord[_index - 3] & CAPS_MASK;
                    TypingWord[_index - 3] = KEY_U | capsMask | (useHorn ? TONEW_MASK : 0) | iMark;
                    for (int x = _stateIndex - 1; x >= 0; x--) {
                        if ((KeyStates[x] & CHAR_MASK) == KEY_I) {
                            KeyStates[x] = KEY_U | (KeyStates[x] & CAPS_MASK);
                            break;
                        }
                    }
                    isCheckedGrammar = true;
                    tempDisableKey = false;
                    handledAdj = true;
                    if (vCheckSpelling) checkSpelling();
                }
            }
        }
    }

    
    //check mark
    if (_index >= 2 && !handledAdj) {
        for (int idx = startPos; idx <= VEI; idx++) {
            if (TypingWord[idx] & MARK_MASK) {
                Uint32 mark = TypingWord[idx] & MARK_MASK;
                TypingWord[idx] &= ~MARK_MASK;
                insertMark(mark, false);
                if (idx != vowelWillSetMark)
                    isCheckedGrammar = true;
                break;
            }
        }
    }
    
    //re-arrange data to sendback
    if (isCheckedGrammar) {
        if (hCode == vDoNothing)
            hCode = vWillProcess;
        hBPC = 0;
        
        for (int idx = _index - 1; idx >= startPos; idx--) {
            hBPC++;
            hData[_index - 1 - idx] = GET(TypingWord[idx]);
        }
        hNCC = hBPC;
        hBPC += deltaBackSpace;
        hExt = 4;
    }
}

void insertKey(const Uint16& keyCode, const bool& isCaps, const bool& isCheckSpelling=true) {
    if (_index >= MAX_BUFF) {
        _longWordHelper.push_back(TypingWord[0]); //save long word
        //left shift
        for (iii = 0; iii < MAX_BUFF - 1; iii++) {
            TypingWord[iii] = TypingWord[iii + 1];
        }
        setKeyData(_index-1, keyCode, isCaps);
    } else {
        setKeyData(_index++, keyCode, isCaps);
    }
    
    if (vCheckSpelling && isCheckSpelling)
        checkSpelling();
    
    //allow d after consonant
    if (keyCode == KEY_D && _index - 2 >= 0 && IS_CONSONANT(CHR(_index - 2)))
        tempDisableKey = false;
}

void insertState(const Uint16& keyCode, const bool& isCaps) {
    if (_stateIndex >= MAX_BUFF) {
        //left shift
        for (iii = 0; iii < MAX_BUFF - 1; iii++) {
            KeyStates[iii] = KeyStates[iii + 1];
        }
        KeyStates[_stateIndex-1] = keyCode | (isCaps ? CAPS_MASK : 0);
    } else {
        KeyStates[_stateIndex++] = keyCode | (isCaps ? CAPS_MASK : 0);
    }
}

static void pushTypingHistory(const TypingStateSnapshot& snap) {
    if (_typingStates.size() >= MAX_TYPING_HISTORY) {
        _typingStates.pop_front();
    }
    _typingStates.push_back(snap);
}

void saveWord() {
    if (_index > 0) {
        if (_longWordHelper.size() > 0) { //save long word first
            TypingStateSnapshot snap;
            for (size_t idx = 0; idx < _longWordHelper.size(); idx++) {
                if (idx != 0 && idx % MAX_BUFF == 0) {
                    pushTypingHistory(snap);
                    snap.word.clear();
                }
                snap.word.push_back(_longWordHelper[idx]);
            }
            pushTypingHistory(snap);
            _longWordHelper.clear();
        }
        TypingStateSnapshot snap;
        snap.word.assign(TypingWord, TypingWord + _index);
        snap.keys.assign(KeyStates, KeyStates + _stateIndex);
        pushTypingHistory(snap);
    }
}

void saveWord(const Uint32& keyCode, const int& count) {
    TypingStateSnapshot snap;
    snap.word.assign(count > 0 ? count : 0, keyCode);
    pushTypingHistory(snap);
}

void saveSpecialChar() {
    TypingStateSnapshot snap;
    snap.word = _specialChar;
    pushTypingHistory(snap);
    _specialChar.clear();
}

void restoreLastTypingState() {
    if (!_typingStates.empty()) {
        TypingStateSnapshot snap = std::move(_typingStates.back());
        _typingStates.pop_back();
        if (!snap.word.empty()) {
            if (snap.word[0] == KEY_SPACE) {
                _spaceCount = (int)snap.word.size();
                _index = 0;
                _stateIndex = 0;
            } else if (std::find(_charKeyCode.begin(), _charKeyCode.end(), (Uint16)snap.word[0]) != _charKeyCode.end()) {
                _index = 0;
                _stateIndex = 0;
                _specialChar = std::move(snap.word);
                checkSpelling();
            } else {
                size_t wn = snap.word.size() > MAX_BUFF ? MAX_BUFF : snap.word.size();
                for (size_t idx = 0; idx < wn; idx++) {
                    TypingWord[idx] = snap.word[idx];
                }
                _index = (Byte)wn;
                size_t kn = snap.keys.size() > MAX_BUFF ? MAX_BUFF : snap.keys.size();
                for (size_t idx = 0; idx < kn; idx++) {
                    KeyStates[idx] = snap.keys[idx];
                }
                _stateIndex = (Byte)kn;
                if (vCheckSpelling) checkSpelling();
            }
        }
    }
}

static bool _afterDigit = false;   //the word started right after a digit: a lone w is a unit (100W), not ư (D33)

void startNewSession() {
    _asDropN = 0;
    _index = 0;
    hBPC = 0;
    hNCC = 0;
    tempDisableKey = false;
    _stateIndex = 0;
    _afterDigit = false;
    memset(TypingWord, 0, sizeof(TypingWord));
    memset(KeyStates, 0, sizeof(KeyStates));
    _longWordHelper.clear();
}

int vKeyCurrentWordLength() {
    return _index;
}

void checkCorrectVowel(vector<vector<Uint16>>& charset, int& i, int& k, const Uint16& markKey) {
    //ignore "qu" case
    if (_index >= 2 && CHR(_index-1) == KEY_U && CHR(_index-2) == KEY_Q) {
        isCorect = false;
        return;
    }
    k = _index - 1;
    for (j = (int)charset[i].size() - 1; j >= 0; j--) {
        if (charset[i][j] != CHR(k)) {
            isCorect = false;
            return;
        }
        k--;
        if (k < 0)
            break;
    }
    
    //limit mark for end consonant: "C", "T"
    if (isCorect && charset[i].size() > 1 && (IS_KEY_F(markKey) || IS_KEY_X(markKey) || IS_KEY_R(markKey))) {
        if (charset[i][1] == KEY_C || charset[i][1] == KEY_T) {
            isCorect = false;
        } else if (charset[i].size() > 2 && (charset[i][2] == KEY_T)) {
            isCorect = false;
        }
    }
    
    if (isCorect && k >= 0) {
        if (CHR(k) == CHR(k+1)) {
            isCorect = false;
        }
    }
}

Uint32 getCharacterCode(const Uint32& data) {
    // Flat O(1) lookup table matching _unicodeTable[baseKey][toneSlot][elem]
    // toneSlot: 0 = base, 1 = TONE_MASK (^), 2 = TONEW_MASK (horn/breve)
    static const Uint16 kUnicodeLut[36][3][14] = {
        /* 0: KEY_A */ {
            {0x00C2, 0x00E2, 0x0102, 0x0103, 0x00C1, 0x00E1, 0x00C0, 0x00E0, 0x1EA2, 0x1EA3, 0x00C3, 0x00E3, 0x1EA0, 0x1EA1},
            {0x1EA4, 0x1EA5, 0x1EA6, 0x1EA7, 0x1EA8, 0x1EA9, 0x1EAA, 0x1EAB, 0x1EAC, 0x1EAD, 0, 0, 0, 0},
            {0x1EAE, 0x1EAF, 0x1EB0, 0x1EB1, 0x1EB2, 0x1EB3, 0x1EB4, 0x1EB5, 0x1EB6, 0x1EB7, 0, 0, 0, 0},
        },
        /* 1 */ {},
        /* 2: KEY_D */ {
            {0x0110, 0x0111, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
            {}, {},
        },
        /* 3..13 */ {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
        /* 14: KEY_E */ {
            {0x00CA, 0x00EA, 0x0000, 0x0000, 0x00C9, 0x00E9, 0x00C8, 0x00E8, 0x1EBA, 0x1EBB, 0x1EBC, 0x1EBD, 0x1EB8, 0x1EB9},
            {0x1EBE, 0x1EBF, 0x1EC0, 0x1EC1, 0x1EC2, 0x1EC3, 0x1EC4, 0x1EC5, 0x1EC6, 0x1EC7, 0, 0, 0, 0},
            {},
        },
        /* 15 */ {},
        /* 16: KEY_Y */ {
            {0x00DD, 0x00FD, 0x1EF2, 0x1EF3, 0x1EF6, 0x1EF7, 0x1EF8, 0x1EF9, 0x1EF4, 0x1EF5, 0, 0, 0, 0},
            {}, {},
        },
        /* 17..30 */ {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
        /* 31: KEY_O */ {
            {0x00D4, 0x00F4, 0x01A0, 0x01A1, 0x00D3, 0x00F3, 0x00D2, 0x00F2, 0x1ECE, 0x1ECF, 0x00D5, 0x00F5, 0x1ECC, 0x1ECD},
            {0x1ED0, 0x1ED1, 0x1ED2, 0x1ED3, 0x1ED4, 0x1ED5, 0x1ED6, 0x1ED7, 0x1ED8, 0x1ED9, 0, 0, 0, 0},
            {0x1EDA, 0x1EDB, 0x1EDC, 0x1EDD, 0x1EDE, 0x1EDF, 0x1EE0, 0x1EE1, 0x1EE2, 0x1EE3, 0, 0, 0, 0},
        },
        /* 32: KEY_U */ {
            {0x0000, 0x0000, 0x01AF, 0x01B0, 0x00DA, 0x00FA, 0x00D9, 0x00F9, 0x1EE6, 0x1EE7, 0x0168, 0x0169, 0x1EE4, 0x1EE5},
            {},
            {0x1EE8, 0x1EE9, 0x1EEA, 0x1EEB, 0x1EEC, 0x1EED, 0x1EEE, 0x1EEF, 0x1EF0, 0x1EF1, 0, 0, 0, 0},
        },
        /* 33 */ {},
        /* 34: KEY_I */ {
            {0x00CD, 0x00ED, 0x00CC, 0x00EC, 0x1EC8, 0x1EC9, 0x0128, 0x0129, 0x1ECA, 0x1ECB, 0, 0, 0, 0},
            {}, {},
        },
        /* 35 */ {},
    };
    int cElem = (data & CAPS_MASK) ? 0 : 1;
    Uint32 baseKey = data & CHAR_MASK;
    if (baseKey >= 36) return data & ~STANDALONE_MASK;
    if (data & MARK_MASK) {
        int mElem = -2;
        switch (data & MARK_MASK) {
            case MARK1_MASK: mElem = 0; break;
            case MARK2_MASK: mElem = 2; break;
            case MARK3_MASK: mElem = 4; break;
            case MARK4_MASK: mElem = 6; break;
            case MARK5_MASK: mElem = 8; break;
        }
        mElem += cElem;
        if ((baseKey == KEY_A || baseKey == KEY_O || baseKey == KEY_U || baseKey == KEY_E) &&
            (data & (TONE_MASK | TONEW_MASK)) == 0) {
            mElem += 4;
        }
        int toneSlot = (data & TONE_MASK) ? 1 : ((data & TONEW_MASK) ? 2 : 0);
        if (baseKey == KEY_D || (baseKey == KEY_U && toneSlot == 1) || (baseKey == KEY_E && toneSlot == 2) ||
            ((baseKey == KEY_I || baseKey == KEY_Y) && toneSlot != 0) || mElem < 0 || mElem >= 14) {
            return data & ~STANDALONE_MASK;
        }
        Uint16 ch = kUnicodeLut[baseKey][toneSlot][mElem];
        return ch ? (ch | CHAR_CODE_MASK) : (data & ~STANDALONE_MASK);
    }
    if (data & TONE_MASK) {
        if (baseKey != KEY_A && baseKey != KEY_O && baseKey != KEY_U && baseKey != KEY_E && baseKey != KEY_D &&
            baseKey != KEY_I && baseKey != KEY_Y) {
            return data & ~STANDALONE_MASK;
        }
        Uint16 ch = kUnicodeLut[baseKey][0][cElem];
        return (ch | CHAR_CODE_MASK);
    }
    if (data & TONEW_MASK) {
        if (baseKey != KEY_A && baseKey != KEY_O && baseKey != KEY_U && baseKey != KEY_E && baseKey != KEY_D &&
            baseKey != KEY_I && baseKey != KEY_Y) {
            return data & ~STANDALONE_MASK;
        }
        Uint16 ch = kUnicodeLut[baseKey][0][cElem + 2];
        return (ch | CHAR_CODE_MASK);
    }
    return data & ~STANDALONE_MASK;
}

void findAndCalculateVowel(const bool& forGrammar) {
    vowelCount = 0;
    VSI = VEI = 0;
    for (iii = _index - 1; iii >= 0; iii--) {
        if (IS_CONSONANT(CHR(iii))) {
            if (vowelCount > 0)
                break;
        } else {  //is vowel
            if (vowelCount == 0)
                VEI = iii;
            if (!forGrammar) {
                if ((iii-1 >= 0 && (CHR(iii) == KEY_I && CHR(iii-1) == KEY_G)) ||
                    (iii-1 >= 0 && (CHR(iii) == KEY_U && CHR(iii-1) == KEY_Q))) {
                    break;
                }
            }
            VSI = iii;
            vowelCount++;
        }
    }
    //August 26th, 2019: don't count "u" at "q u" as a vowel
    if (VSI - 1 >= 0 && CHR(VSI) == KEY_U && CHR(VSI-1) == KEY_Q) {
        VSI++;
        vowelCount--;
    }
}

void removeMark() {
    findAndCalculateVowel(true);
    isChanged = false;
    if (_index > 0) {
        for (i = VSI; i <= VEI; i++) {
            if (TypingWord[i] & MARK_MASK) {
                TypingWord[i] &= ~MARK_MASK;
                isChanged = true;
            }
        }
    }
    if (isChanged) {
        hCode = vWillProcess;
        hBPC = 0;
        
        for (i = _index - 1; i >= VSI; i--) {
            hBPC++;
            hData[_index - 1 - i] = GET(TypingWord[i]);
        }
        hNCC = hBPC;
    } else {
        hCode = vDoNothing;
    }
}

bool canHasEndConsonant() {
    auto it = _vowelCombine.find(CHR(VSI));
    if (it == _vowelCombine.end()) return false;
    const vector<vector<Uint32>>& vo = it->second;
    for (size_t vIdx = 0; vIdx < vo.size(); vIdx++) {
        int pos = VSI;
        size_t eIdx = 1;
        for (; eIdx < vo[vIdx].size(); eIdx++) {
            if (pos > VEI || ((CHR(pos) | (TypingWord[pos] & (TONE_MASK | TONEW_MASK))) != vo[vIdx][eIdx])) {
                break;
            }
            pos++;
        }
        if (eIdx >= vo[vIdx].size()) {
            return vo[vIdx][0] == 1;
        }
    }
    return false;
}

void handleModernMark() {
    //default
    VWSM = VEI;
    hBPC = (_index - VEI);
    
    //rule 2
    if (vowelCount == 3 && ((CHR(VSI) == KEY_O && CHR(VSI+1) == KEY_A && CHR(VSI+2) == KEY_I) ||
                            (CHR(VSI) == KEY_U && CHR(VSI+1) == KEY_Y && CHR(VSI+2) == KEY_U) ||
                            (CHR(VSI) == KEY_O && CHR(VSI+1) == KEY_E && CHR(VSI+2) == KEY_O) ||
                            (CHR(VSI) == KEY_U && CHR(VSI+1) == KEY_Y && CHR(VSI+2) == KEY_A))) {
        VWSM = VSI + 1;
        hBPC = _index - VWSM;
    } else if ((CHR(VSI) == KEY_O && CHR(VSI+1) == KEY_I) ||
               (CHR(VSI) == KEY_A && CHR(VSI+1) == KEY_I) ||
               (CHR(VSI)== KEY_U && CHR(VSI+1) == KEY_I) ) {
        
        VWSM = VSI;
        hBPC = _index - VWSM;
    } else if (VEI > 0 && CHR(VEI-1) == KEY_A && CHR(VEI) == KEY_Y) {
        VWSM = VEI - 1;
        hBPC = (_index - VEI) + 1;
    } else if (CHR(VSI) == KEY_U && CHR(VSI+1) == KEY_O) {
        VWSM = VSI + 1;
        hBPC = _index - VWSM;
    } else if (CHR(VSI+1) == KEY_O || CHR(VSI+1) == KEY_U) {
        VWSM = VEI - 1;
        hBPC = (_index - VEI) + 1;
    } else if (CHR(VSI) == KEY_O || CHR(VSI) == KEY_U) {
        VWSM = VEI;
        hBPC = (_index - VEI);
    }
    
    //rule 3.1
    Uint32 v1Masked = TypingWord[VSI+1] & (CHAR_MASK | TONE_MASK | TONEW_MASK);
    Uint32 v0Masked = TypingWord[VSI] & (CHAR_MASK | TONE_MASK | TONEW_MASK);
    if ((CHR(VSI) == KEY_I && v1Masked == (KEY_E | TONE_MASK)) ||
        (CHR(VSI) == KEY_Y && v1Masked == (KEY_E | TONE_MASK)) ||
        (CHR(VSI) == KEY_U && v1Masked == (KEY_O | TONE_MASK)) ||
        (v0Masked == (KEY_U | TONEW_MASK) && v1Masked == (KEY_O | TONEW_MASK))) {
        
        if (VSI+2 < _index) {
            if (CHR(VSI+2) == KEY_P || CHR(VSI+2) == KEY_T ||
                CHR(VSI+2) == KEY_M || CHR(VSI+2) == KEY_N ||
                CHR(VSI+2) == KEY_O || CHR(VSI+2) == KEY_U ||
                CHR(VSI+2) == KEY_I || CHR(VSI+2) == KEY_C ||
                (VSI+3 < _index && CHR(VSI+2) == KEY_C && CHR(VSI+3) == KEY_H) ||
                (VSI+3 < _index && CHR(VSI+2) == KEY_N && CHR(VSI+3) == KEY_H) ||
                (VSI+3 < _index && CHR(VSI+2) == KEY_N && CHR(VSI+3) == KEY_G)) {
                
                VWSM = VSI + 1;
                hBPC = _index - VWSM;
            } else {
                VWSM = VSI;
                hBPC = _index - VWSM;
            }
        } else {
            VWSM = VSI;
            hBPC = _index - VWSM;
        }
    }
    //rule 3.2: ưu (cứu, lựu) puts the mark on ư; ia/ya/ua are handled by rule 4 below
    else if (v0Masked == (KEY_U | TONEW_MASK) && CHR(VSI+1) == KEY_U) {
        
        VWSM = VSI;
        hBPC = _index - VWSM;
    }
    
    //rule 4
    if (vowelCount == 2) {
        if (((CHR(VSI) == KEY_I) && (CHR(VSI+1) == KEY_A)) ||
            ((CHR(VSI) == KEY_I) && (CHR(VSI+1) == KEY_U)) ||
            ((CHR(VSI) == KEY_I) && (CHR(VSI+1) == KEY_O))) {
            
            if (VSI == 0 || (CHR(VSI-1) != KEY_G)) { //dont have G
                VWSM = VSI;
                hBPC = _index - VWSM;
            } else {
                VWSM = VSI + 1;
                hBPC = _index - VWSM;
            }
        } else if ((CHR(VSI) == KEY_U) && (CHR(VSI+1) == KEY_A)) {
            if (VSI == 0 || (CHR(VSI-1) != KEY_Q)) { //dont have Q
                if (VEI + 1 >= _index || !canHasEndConsonant()) {
                    VWSM = VSI;
                    hBPC = _index - VWSM;
                }
            } else {
                VWSM = VSI + 1;
                hBPC = _index - VWSM;
            }
        } else if ((CHR(VSI) == KEY_O) && (CHR(VSI+1) == KEY_O)) { //thoong
            VWSM = VEI;
            hBPC = _index - VWSM;
        }
    }
}

void handleOldMark() {
    //default
    if (vowelCount == 0 && CHR(VEI) == KEY_I)
        VWSM = VEI;
    else
        VWSM = VSI;
    hBPC = (_index - VWSM);
    
    //rule 2
    if (vowelCount == 3 || (VEI + 1 < _index && IS_CONSONANT(CHR(VEI + 1)) && canHasEndConsonant())) {
        VWSM = VSI + 1;
        hBPC = _index - VWSM;
    }
    
    //rule 3
    for (ii = VSI; ii <= VEI; ii++) {
        if ((CHR(ii) == KEY_E && TypingWord[ii] & TONE_MASK) || (CHR(ii) == KEY_O && TypingWord[ii] & TONEW_MASK)) {
            VWSM = ii;
            hBPC = _index - VWSM;
            break;
        }
    }
    
    hNCC = hBPC;
}

void insertMark(const Uint32& markMask, const bool& canModifyFlag) {
    vowelCount = 0;
    
    if (canModifyFlag)
        hCode = vWillProcess;
    hBPC = hNCC = 0;
    
    findAndCalculateVowel();
    VWSM = 0;
    
    //detect mark position
    if (vowelCount <= 1) {
        VWSM = VEI;
        if (VSI < VEI && vowelCount == 0) VSI = VEI;
        hBPC = (_index - VEI);
    } else { //vowel = 2 or 3
        if (vUseModernOrthography == 0)
            handleOldMark();
        else
            handleModernMark();
        if (TypingWord[VEI] & TONE_MASK || TypingWord[VEI] & TONEW_MASK)
            vowelWillSetMark = VEI;
    }
    
    //send data
    kk = _index - 1 - VSI;
    //if duplicate same mark -> restore
    if (TypingWord[VWSM] & markMask) {
        
        TypingWord[VWSM] &= ~MARK_MASK;
        if (canModifyFlag)
            hCode = vRestore;
        for (ii = VSI; ii < _index; ii++) {
            TypingWord[ii] &= ~MARK_MASK;
            hData[kk--] = GET(TypingWord[ii]);
        }
        //_index = 0;
        hBPC = _index - VSI;
        if (!vRotationalTone)
            tempDisableKey = true;
        else
            tempDisableKey = false;
    } else {
        //remove other mark
        TypingWord[VWSM] &= ~MARK_MASK;
        
        //add mark
        TypingWord[VWSM] |= markMask;
        for (ii = VSI; ii < _index; ii++) {
            if (ii != VWSM) { //remove mark for other vowel
                TypingWord[ii] &= ~MARK_MASK;
            }
            hData[kk--] = GET(TypingWord[ii]);
        }
        
        hBPC = _index - VSI;
    }
    hNCC = hBPC;
}

void insertD(const Uint16& data, const bool& isCaps) {
    hCode = vWillProcess;
    hBPC = 0;
    for (ii = _index - 1; ii >= 0; ii--) {
        hBPC++;
        if (CHR(ii) == KEY_D) { //reverse unicode char
            if (TypingWord[ii] & TONE_MASK) {
                //restore and disable temporary
                hCode = vRestore;
                TypingWord[ii] &= ~TONE_MASK;
                hData[_index - 1 - ii] = GET(TypingWord[ii]) & ~STANDALONE_MASK;
                tempDisableKey = true;
                break;
            } else {
                TypingWord[ii] |= TONE_MASK;
                hData[_index - 1 - ii] = GET(TypingWord[ii]);
            }
            break;
        } else { //preresent old char
            hData[_index - 1 - ii] = GET(TypingWord[ii]);
        }
    }
    hNCC = hBPC;
}

void insertAOE(const Uint16& data, const bool& isCaps) {
    findAndCalculateVowel();
    
    //remove W tone
    int minChanged = _index;
    for (ii = VSI; ii <= VEI; ii++) {
        if (TypingWord[ii] & TONEW_MASK) {
            TypingWord[ii] &= ~TONEW_MASK;
            if (ii < minChanged) minChanged = ii;
        }
    }
    
    hCode = vWillProcess;
    hBPC = 0;
    
    for (ii = _index - 1; ii >= 0; ii--) {
        hBPC++;
        if (CHR(ii) == data) { //reverse unicode char
            if (TypingWord[ii] & TONE_MASK) {
                //restore and disable temporary
                hCode = vRestore;
                TypingWord[ii] &= ~TONE_MASK;
                hData[_index - 1 - ii] = GET(TypingWord[ii]) & ~STANDALONE_MASK;
                if (data != KEY_O) //case thoòng
                    tempDisableKey = true;
            } else {
                TypingWord[ii] |= TONE_MASK;
                if (!IS_KEY_D(data))
                    TypingWord[ii] &= ~TONEW_MASK;
                hData[_index - 1 - ii] = GET(TypingWord[ii]);
            }
            for (int m = ii - 1; m >= minChanged; m--) {
                hBPC++;
                hData[_index - 1 - m] = GET(TypingWord[m]);
            }
            break;
        } else { //preresent old char
            hData[_index - 1 - ii] = GET(TypingWord[ii]);
        }
    }
    hNCC = hBPC;
}

void insertW(const Uint16& data, const bool& isCaps) {
    isRestoredW = false;
    
    findAndCalculateVowel();
    
    if (vowelCount > 1) {
        Uint32 savedTones[MAX_BUFF];
        for (ii = VSI; ii <= VEI; ii++) {
            savedTones[ii] = TypingWord[ii] & TONE_MASK;
            TypingWord[ii] &= ~TONE_MASK;
        }
        hBPC = _index - VSI;
        hNCC = hBPC;
        
        if (((TypingWord[VSI] & TONEW_MASK) && (TypingWord[VSI+1] & TONEW_MASK)) ||
            ((TypingWord[VSI] & TONEW_MASK) && CHR(VSI+1) == KEY_I) ||
            ((TypingWord[VSI] & TONEW_MASK) && CHR(VSI+1) == KEY_A) ||
            ((TypingWord[VSI+1] & TONEW_MASK) && ((CHR(VSI) == KEY_U && CHR(VSI+1) == KEY_O) || (CHR(VSI) == KEY_O && CHR(VSI+1) == KEY_A) || (CHR(VSI) == KEY_I && CHR(VSI+1) == KEY_O)))) {
            //restore and disable temporary
            hCode = vRestore;
            
            for (ii = VSI; ii < _index; ii++) {
                TypingWord[ii] &= ~TONEW_MASK;
                hData[_index - 1 - ii] = GET(TypingWord[ii]) & ~STANDALONE_MASK;
            }
            isRestoredW = true;
            tempDisableKey = true;
        } else {
            hCode = vWillProcess;
            
            if ((CHR(VSI) == KEY_U && CHR(VSI+1) == KEY_O)) {
                if (VSI - 2 >= 0 && CHR(VSI - 2) == KEY_T && CHR(VSI - 1) == KEY_H) {
                    TypingWord[VSI+1] |= TONEW_MASK;
                    if (VSI + 2 < _index && CHR(VSI+2) == KEY_N) {
                        TypingWord[VSI] |= TONEW_MASK;
                    }
                } else if (VSI - 1 >= 0 && CHR(VSI - 1) == KEY_Q) {
                    TypingWord[VSI+1] |= TONEW_MASK;
                } else {
                    TypingWord[VSI] |= TONEW_MASK;
                    TypingWord[VSI+1] |= TONEW_MASK;
                }
            } else if ((CHR(VSI) == KEY_U && CHR(VSI+1) == KEY_A) ||
                       (CHR(VSI) == KEY_U && CHR(VSI+1) == KEY_I) ||
                       (CHR(VSI) == KEY_U && CHR(VSI+1) == KEY_U) ||
                       (CHR(VSI) == KEY_O && CHR(VSI+1) == KEY_I)) {
                TypingWord[VSI] |= TONEW_MASK;
            } else if ((CHR(VSI) == KEY_I && CHR(VSI+1) == KEY_O) ||
                       (CHR(VSI) == KEY_O && CHR(VSI+1) == KEY_A)) {
                TypingWord[VSI+1] |= TONEW_MASK;
            } else {
                //don't do anything: keep TONE_MASK intact so TypingWord matches screen
                for (ii = VSI; ii <= VEI; ii++) {
                    TypingWord[ii] |= savedTones[ii];
                }
                tempDisableKey = true;
                isChanged = false;
                hCode = vDoNothing;
                return;
            }
            
            for (ii = VSI; ii < _index; ii++) {
                hData[_index - 1 - ii] = GET(TypingWord[ii]);
            }
        }
        
        return;
    }
    
    //remove ^ tone
    for (ii = VSI; ii <= VEI; ii++) {
        TypingWord[ii] &= ~TONE_MASK;
    }
    
    hCode = vWillProcess;
    hBPC = 0;
    
    for (ii = _index - 1; ii >= 0; ii--) {
        if (ii < VSI)
            break;
        hBPC++;
        switch (CHR(ii)) {
            case KEY_A:
            case KEY_U:
            case KEY_O:
                if (TypingWord[ii] & TONEW_MASK) {
                    //restore and disable temporary
                    if (TypingWord[ii] & STANDALONE_MASK) {
                        hCode = vWillProcess;
                        if (CHR(ii) == KEY_U){
                            TypingWord[ii] = KEY_W | ((TypingWord[ii] & CAPS_MASK) ? CAPS_MASK : 0);
                        } else if (CHR(ii) == KEY_O) {
                            hCode = vRestore;
                            TypingWord[ii] = KEY_O | ((TypingWord[ii] & CAPS_MASK) ? CAPS_MASK : 0);
                            isRestoredW = true;
                        }
                        hData[_index - 1 - ii] = GET(TypingWord[ii]) & ~STANDALONE_MASK;
                    } else {
                        hCode = vRestore;
                        TypingWord[ii] &= ~TONEW_MASK;
                        hData[_index - 1 - ii] = GET(TypingWord[ii]) & ~STANDALONE_MASK;
                        isRestoredW = true;
                    }
                    
                    tempDisableKey = true;
                } else {
                    TypingWord[ii] |= TONEW_MASK;
                    TypingWord[ii] &= ~TONE_MASK;
                    hData[_index - 1 - ii] = GET(TypingWord[ii]);
                }
                break;
                
            default:
                hData[_index - 1 - ii] = GET(TypingWord[ii]);
                break;
        }
    }
    hNCC = hBPC;
    
    if (isRestoredW) {
        //_index = 0;
    }
}

void reverseLastStandaloneChar(const Uint32& keyCode, const bool& isCaps) {
    hCode = vWillProcess;
    hBPC = 0;
    hNCC = 1;
    hExt = 4;
    TypingWord[_index - 1] = (keyCode | TONEW_MASK | STANDALONE_MASK | (isCaps ? CAPS_MASK : 0));
    hData[0] = GET(TypingWord[_index - 1]);
}

void checkForStandaloneChar(const Uint16& data, const bool& isCaps, const Uint32& keyWillReverse) {
    if (_index > 0 && CHR(_index - 1) == keyWillReverse && (TypingWord[_index - 1] & TONEW_MASK)) {
        hCode = vWillProcess;
        hBPC = 1;
        hNCC = 1;
        TypingWord[_index - 1] = data | (isCaps ? CAPS_MASK : 0);
        hData[0] = GET(TypingWord[_index - 1]);
        return;
    }
    
    //check standalone w -> ư
    
    if (_index > 0 && CHR(_index-1) == KEY_U && keyWillReverse == KEY_O) {
        insertKey(keyWillReverse, isCaps);
        reverseLastStandaloneChar(keyWillReverse, isCaps);
        return;
    }
    
    if (_index == 0 && _afterDigit && data == KEY_W) { insertKey(data, isCaps); return; }
    if (_index == 0) { //zero char
        insertKey(data, isCaps, false);
        reverseLastStandaloneChar(keyWillReverse, isCaps);
        return;
    } else if (_index == 1) { //1 char
        for (i = 0; i < _standaloneWbad.size(); i++) {
            if (CHR(0) == _standaloneWbad[i]) {
                insertKey(data, isCaps);
                return;
            }
        }
        insertKey(data, isCaps, false);
        reverseLastStandaloneChar(keyWillReverse, isCaps);
        return;
    } else if (_index == 2) {
        for (i = 0; i < _doubleWAllowed.size(); i++) {
            if (CHR(0) == _doubleWAllowed[i][0] && CHR(1) == _doubleWAllowed[i][1]) {
                insertKey(data, isCaps, false);
                reverseLastStandaloneChar(keyWillReverse, isCaps);
                return;
            }
        }
        insertKey(data, isCaps);
        return;
    }
    
    insertKey(data, isCaps);
}


void handleMainKey(const Uint16& data, const bool& isCaps) {
    //if is Z key, remove mark
    if (IS_KEY_Z(data)) {
        removeMark();
        if (!isChanged) {
            insertKey(data, isCaps);
        }
        return;
    }
    
    if (data == KEY_LEFT_BRACKET) { //standalone key [
        checkForStandaloneChar(data, isCaps, KEY_O);
        return;
    }
    
    if (data == KEY_RIGHT_BRACKET) { //standalone key }
        checkForStandaloneChar(data, isCaps, KEY_U);
        return;
    }
    
    //if is D key
    if (IS_KEY_D(data)) {
        isCorect = false;
        isChanged = false;
        k = _index;
        for (i = 0; i < _consonantD.size(); i++) {
            if (_index < _consonantD[i].size())
                continue;
            isCorect = true;
            checkCorrectVowel(_consonantD, i, k, data);
            
            //allow d after consonant
            if (!isCorect && _index - 2 >= 0 && CHR(_index-1) == KEY_D && IS_CONSONANT(CHR(_index-2))) {
                isCorect = true;
            }
            if (isCorect) {
                isChanged = true;
                insertD(data, isCaps);
                break;
            }
        }
    
        if (!isChanged) {
            hCode = vDoNothing;
            hBPC = 0;
            hNCC = 0;
            hExt = 3;
            insertKey(data, isCaps);
        }
        return;
    }
    
    //if is mark key
    if (IS_MARK_KEY(data)) {
        if (vRotationalTone) {
            int existingMarkVowel = -1;
            Uint32 existingMark = 0;
            bool hasVowelAfterConsonant = false;
            bool seenConsonant = false;
            for (int m = 0; m < _index; m++) {
                if (TypingWord[m] & MARK_MASK) {
                    existingMarkVowel = m;
                    existingMark = TypingWord[m] & MARK_MASK;
                    break;
                }
            }
            if (existingMarkVowel != -1) {
                for (int m = existingMarkVowel + 1; m < _index; m++) {
                    if (IS_CONSONANT(CHR(m))) {
                        seenConsonant = true;
                    } else if (seenConsonant) {
                        hasVowelAfterConsonant = true;
                        break;
                    }
                }
            }
            if (existingMarkVowel != -1 && !hasVowelAfterConsonant) {
                Uint32 newMark = 0;
                if (IS_KEY_S(data)) newMark = MARK1_MASK;
                else if (IS_KEY_F(data)) newMark = MARK2_MASK;
                else if (IS_KEY_R(data)) newMark = MARK3_MASK;
                else if (IS_KEY_X(data)) newMark = MARK4_MASK;
                else if (IS_KEY_J(data)) newMark = MARK5_MASK;

                if (newMark != 0) {
                    if (newMark == existingMark) {
                        insertMark(newMark);
                        tempDisableKey = false;
                        return;
                    } else {
                        for (int m = 0; m < _index; m++) {
                            TypingWord[m] &= ~MARK_MASK;
                        }
                        insertMark(newMark);
                        tempDisableKey = false;
                        return;
                    }
                }
            }
        }

        for (auto& vowelEntry : _vowelForMark) {
            vector<vector<Uint16>>& charset = vowelEntry.second;
            isCorect = false;
            isChanged = false;
            k = _index;
            for (l = 0; l < charset.size(); l++) {
                if (_index < charset[l].size())
                    continue;
                isCorect = true;
                checkCorrectVowel(charset, l, k, data);

                if (isCorect) {
                    isChanged = true;
                    if (IS_KEY_S(data))
                        insertMark(MARK1_MASK);
                    else if (IS_KEY_F(data))
                        insertMark(MARK2_MASK);
                    else if (IS_KEY_R(data))
                        insertMark(MARK3_MASK);
                    else if (IS_KEY_X(data))
                        insertMark(MARK4_MASK);
                    else if (IS_KEY_J(data))
                        insertMark(MARK5_MASK);
                    break;
                }
            }

            if (isCorect) {
                break;
            }
        }

        if (!isChanged) {
            hCode = vDoNothing;
            hBPC = 0;
            hNCC = 0;
            hExt = 3;
            insertKey(data, isCaps);
        }

        return;
    }
    
    //check Vowel
    keyForAEO = data;
    vector<vector<Uint16>>& charset = _vowel[keyForAEO];
    isCorect = false;
    isChanged = false;
    k = _index;
    for (i = 0; i < charset.size(); i++) {
        if (_index < charset[i].size())
            continue;
        isCorect = true;
        checkCorrectVowel(charset, i, k, data);
        
        if (isCorect) {
            isChanged = true;
            if (IS_KEY_DOUBLE(data)) {
                insertAOE(keyForAEO, isCaps);
            } else if (IS_KEY_W(data)) {
                insertW(keyForAEO, isCaps);
            }
            break;
        }
    }
    
    if (!isChanged) {
        hCode = vDoNothing;
        hBPC = 0;
        hNCC = 0;
        hExt = 3;
        if (data == KEY_W) {
            checkForStandaloneChar(data, isCaps, KEY_U);
        } else {
            insertKey(data, isCaps);
        }
    }
}


static int asStatesWithDrops(Uint32* full, const int& maxN);
bool checkRestoreIfWrongSpelling(const int& handleCode) {
    if (_index >= 3) {
        Uint16 c1 = CHR(_index - 2);
        Uint16 c2 = CHR(_index - 1);
        if ((c1 == KEY_G && c2 == KEY_N) || (c1 == KEY_H && c2 == KEY_N) || (c1 == KEY_H && c2 == KEY_C)) {
            if (!IS_CONSONANT(CHR(_index - 3))) {
                bool hasVnMark = false;
                for (int x = 0; x < _index; x++) {
                    if (TypingWord[x] & (MARK_MASK | TONE_MASK | TONEW_MASK)) { hasVnMark = true; break; }
                    if (CHR(x) == KEY_D && (TypingWord[x] & TONE_MASK)) { hasVnMark = true; break; }
                }
                bool isEnglishWord = false;
                if (!hasVnMark) {
                    asInitLetters();
                    char raw[AS_KEY_LEN];
                    if (asRawWord(KeyStates, _stateIndex, raw) && asEnglishScore(raw) != AS_MISSING) {
                        isEnglishWord = true;
                    }
                }
                if (!isEnglishWord) {
                    std::swap(TypingWord[_index - 2], TypingWord[_index - 1]);
                    if (_stateIndex >= 2) {
                        std::swap(KeyStates[_stateIndex - 2], KeyStates[_stateIndex - 1]);
                    }
                    if (vCheckSpelling) checkSpelling();
                    if (!tempDisableKey) {
                        hCode = vWillProcess;
                        hBPC = _index;
                        hNCC = _index;
                        for (int x = 0; x < _index; x++) {
                            hData[_index - 1 - x] = GET(TypingWord[x]);
                        }
                        return true;
                    } else {
                        std::swap(TypingWord[_index - 2], TypingWord[_index - 1]);
                        if (_stateIndex >= 2) {
                            std::swap(KeyStates[_stateIndex - 2], KeyStates[_stateIndex - 1]);
                        }
                    }
                }
            }
        }
    }
    for (ii = 0; ii < _index; ii++) {
        if (!IS_CONSONANT(CHR(ii)) &&
            (TypingWord[ii] & MARK_MASK || TypingWord[ii] & TONE_MASK || TypingWord[ii] & TONEW_MASK)) {
            hCode = handleCode;
            hBPC = _index;
            hNCC = _stateIndex;
            for (i = 0; i < _stateIndex; i++) {
                TypingWord[i] = KeyStates[i];
                hData[_stateIndex - 1 - i] = TypingWord[i];
            }
            _index = _stateIndex;
            return true;
        }
    }
    return false;
}

void vSetCheckSpelling() {
    _useSpellCheckingBefore = vCheckSpelling;
}


/*==========================================================================================================*/
/* Auto switch language from typing behavior.
 * Everything here runs only at a word break (or on a doubled mark key), never per letter, and
 * uses fixed tables compiled into the binary: no allocation, no file or system calls.
 *
 * Words are compared as a Telex-normalized ASCII key: circumflex -> doubled vowel (aa ee oo),
 * breve/horn -> letter + w (aw ow uw), đ -> dd, tone letter (s f r x j) at the end.
 * That makes the check independent of the code table and of old/new tone placement.
 */
#define AS_MISSING (-1000)
#define AS_KEY_LEN 40
//Scores are 10*log10(occurrences per million). When the keys read as a word in both languages,
//leave the current language only if the word in the current language is rare (below its floor)
//and the other reading is at least AS_PREFER points (AS_PREFER/10 orders of magnitude) more common.
#ifndef AS_PREFER
#define AS_PREFER 10
#endif
#ifndef AS_VN_FLOOR
#define AS_VN_FLOOR 10      //Vietnamese words used at least ~10 per million are never switched away from
#endif
#ifndef AS_EN_FLOOR
#define AS_EN_FLOOR 99      //no floor: a tie converts when the Vietnamese reading is AS_PREFER points more common (D22; was 0)
#endif

int vAutoSwitchRequest = -1;
Uint32 vAutoSwitchUndoData[MAX_BUFF];
int vAutoSwitchUndoCount = 0;

static char _asLetter[256];
static bool _asLetterReady = false;
static bool _asWordStartOk = true;   //the current word began after a space, Enter, Tab or a fresh start (not inside foo.bar or my_var)

/* Look-back: when a word confidently switches the language, the 1-2 words typed just before it
 * (separated by single spaces) are fixed too if they were ambiguous, e.g. "thí make" -> "this make".
 * An earlier word follows the switch unless its current reading is AS_LOOKBACK_MARGIN points more common. */
#ifndef AS_LOOKBACK_WORDS
#define AS_LOOKBACK_WORDS 2
#endif
#ifndef AS_PLAIN_MARGIN
#define AS_PLAIN_MARGIN 15    //"thí makes": the earlier word must read English ~30x more often than Vietnamese (this vs thí)
#endif
#ifndef AS_LOOKBACK_MARGIN
#define AS_LOOKBACK_MARGIN 10
#endif
struct AsWord {
    Uint32 raw[MAX_BUFF];   //keys as typed
    int rawCount;
    Uint32 shown[MAX_BUFF]; //what is on screen (engine character data)
    int shownCount;
    int en, vn;             //scores of the raw keys as English and of the Vietnamese reading
    bool lookbackOk;        //could follow a switch to English
    bool same;              //looks the same in both modes (nothing to fix)
};
static AsWord _asPrev[AS_LOOKBACK_WORDS];
static int _asPrevCount = 0;

void vAutoSwitchForget() {
    _asPrevCount = 0;
    _asWordStartOk = true;   //a fresh context (language switched, app changed): the next word stands on its own
}

static void asInitLetters() {
    if (_asLetterReady) return;
    memset(_asLetter, 0, sizeof(_asLetter));
    const Uint16 keys[26] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
                             KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
    for (int n = 0; n < 26; n++) _asLetter[keys[n] & 0xFF] = (char)('a' + n);
    _asLetterReady = true;
}

bool vKeyGetCurrentWordForAutoCorrect(char* rawLetters, int maxLetters, int* capsCount, bool* firstCap,
                                      Uint16* screenChars, int maxScreenChars, int* screenLen) {
    if (!rawLetters || maxLetters < 4 || !capsCount || !firstCap || !screenChars || maxScreenChars < 1 || !screenLen)
        return false;
    if (!_asWordStartOk || _afterDigit || !_longWordHelper.empty() ||
        _index <= 0 || _stateIndex < 3 || _stateIndex >= maxLetters || _index > maxScreenChars)
        return false;
    asInitLetters();
    int caps = 0;
    bool fCap = (KeyStates[0] & CAPS_MASK) != 0;
    for (int i = 0; i < _stateIndex; i++) {
        Uint16 kc = (Uint16)(KeyStates[i] & CHAR_MASK);
        char ch = (kc < 256) ? _asLetter[kc] : 0;
        if (!ch) return false;
        rawLetters[i] = ch;
        if (KeyStates[i] & CAPS_MASK) caps++;
    }
    rawLetters[_stateIndex] = 0;
    for (int i = 0; i < _index; i++) {
        Uint32 code = getCharacterCode(TypingWord[i]);
        Uint16 u = (code & CHAR_CODE_MASK) ? (Uint16)(code & 0xFFFF) : keyCodeToCharacter(code);
        if (u == 0) return false;
        screenChars[i] = u;
    }
    *capsCount = caps;
    *firstCap = fCap;
    *screenLen = _index;
    return true;
}

static inline char asLetterOf(const Uint32& data) {
    Uint32 code = data & CHAR_MASK;
    return code < 256 ? _asLetter[code] : 0;
}

static int asLookup(const char* words, const unsigned int* offsets, const signed char* scores, const int& count, const char* word) {
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int c = strcmp(words + offsets[mid], word);
        if (c == 0) return scores[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return AS_MISSING;
}

static inline int asVietnameseScore(const char* key) {
    return asLookup(kAutoSwitchVnWords, kAutoSwitchVnOffsets, kAutoSwitchVnScores, kAutoSwitchVnCount, key);
}

static inline int asEnglishScore(const char* word) {
    return asLookup(kAutoSwitchEnWords, kAutoSwitchEnOffsets, kAutoSwitchEnScores, kAutoSwitchEnCount, word);
}

/** Programming identifiers from Tools/autoswitch/en_code.txt (sorted). In EN mode their score of 40
 *  keeps them English; in VN mode they must not beat valid Vietnamese words like "núm" or "séc" (D6). */
static bool asIsCodeWord(const char* word) {
    static const char* const kCodeWords[] = {
        "arcs", "asc", "buf", "chars", "cors", "cur", "defs", "esc",
        "hist", "hono", "nums", "osc", "refs", "secs", "uri", "var", "vars"
    };
    int lo = 0, hi = (int)(sizeof(kCodeWords) / sizeof(kCodeWords[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int c = strcmp(kCodeWords[mid], word);
        if (c == 0) return true;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

/** Normalized key of the word in TypingWord; transformed = it has a tone, ^, w-horn/breve or đ. */
static bool asWordKey(char* out, bool& transformed) {
    transformed = false;
    if (_index == 0 || _longWordHelper.size() > 0) return false;
    int n = 0;
    char tone = 0;
    for (int x = 0; x < _index; x++) {
        Uint32 t = TypingWord[x];
        char c = asLetterOf(t);
        if (c == 0 || n + 3 >= AS_KEY_LEN) return false;
        out[n++] = c;
        if ((t & TONE_MASK) && (c == 'a' || c == 'e' || c == 'o' || c == 'd')) {
            out[n++] = c;
            transformed = true;
        } else if ((t & TONEW_MASK) && (c == 'a' || c == 'o' || c == 'u')) {
            out[n++] = 'w';
            transformed = true;
        }
        switch (t & MARK_MASK) {
            case MARK1_MASK: tone = 's'; break;
            case MARK2_MASK: tone = 'f'; break;
            case MARK3_MASK: tone = 'r'; break;
            case MARK4_MASK: tone = 'x'; break;
            case MARK5_MASK: tone = 'j'; break;
        }
    }
    if (tone) {
        out[n++] = tone;
        transformed = true;
    }
    out[n] = 0;
    return true;
}

/** Lowercase letters of raw key codes; false if any key isn't a letter. */
static bool asRawWord(const Uint32* keys, const int& count, char* out) {
    if (count <= 0 || count >= AS_KEY_LEN) return false;
    for (int x = 0; x < count; x++) {
        out[x] = asLetterOf(keys[x]);
        if (out[x] == 0) return false;
    }
    out[count] = 0;
    return true;
}

static inline bool asEnabled() {
    return vAutoSwitchByTyping;
}

/** Vietnamese mode: a word was ended by a single space and stays Vietnamese. Keep it for look-back. */
int vAutoSwitchLastSide = -1;

// ---- Word pairs for ties (D30, experiment) ---------------------------------------------------------------
// For a word that is a word both ways (bus/bú, been/bên), how often each reading follows the previous word
// in real text ("cho con bú" vs "the bus"). Loaded at run time for now (tests); off when not loaded.
#ifndef AS_BIGRAM_W
#define AS_BIGRAM_W 2.0f   //tuned on the val split (D29): 1 -> 0.128%, 2 -> 0.124%, 3 -> 0.124% wrong words
#endif
static std::unordered_map<std::string, int>* _asBigrams = nullptr;   //loaded at run time (tests) instead of the built-in table
static float _asBigramWeight = AS_BIGRAM_W;
static int _asBigramMin = 3;
static int asBuiltInPair(const char* key) {
    int lo = 0, hi = kBigramCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(kBigramKeys + kBigramOffsets[mid], key);
        if (c == 0) return (int)lround(exp2(kBigramLogCounts[mid] / 8.0));
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
static int asBuiltInPair2(const char* a, const char* b) {
    char buf[128];
    size_t la = strlen(a), lb = strlen(b);
    if (la + 1 + lb >= sizeof(buf)) return 0;
    memcpy(buf, a, la); buf[la] = '\t'; memcpy(buf + la + 1, b, lb + 1);
    return asBuiltInPair(buf);
}
void vAutoSwitchSetBigrams(const char* path, const float& weight, const int& minCount) {
    _asBigramWeight = weight; _asBigramMin = minCount;
    if (!path) return;
    FILE* f = fopen(path, "r"); if (!f) return;
    if (!_asBigrams) _asBigrams = new std::unordered_map<std::string, int>();
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* t1 = strchr(line, '\t'); if (!t1) continue; char* t2 = strchr(t1 + 1, '\t'); if (!t2) continue;
        *t2 = 0; (*_asBigrams)[std::string(line)] += atoi(t2 + 1);
    }
    fclose(f);
}
static std::string asUtf8(const Uint16* t, int n) {
    std::string s;
    for (int i = 0; i < n; i++) { unsigned c = t[i];
        if (c < 0x80) s += (char)c; else if (c < 0x800) { s += (char)(0xC0 | c >> 6); s += (char)(0x80 | (c & 63)); }
        else { s += (char)(0xE0 | c >> 12); s += (char)(0x80 | ((c >> 6) & 63)); s += (char)(0x80 | (c & 63)); } }
    return s;
}
/** The word as it shows (lowercase): the letters, or their Vietnamese reading. */
static bool asSurface(const char* letters, const bool& vietnamese, std::string& out) {
    if (!vietnamese) { out = letters; return true; }
    Uint16 t[64]; int n = 0;
    if (vAutoSwitchWordScore(1, letters, t, &n) <= AS_MISSING || n <= 0) return false;
    out = asUtf8(t, n); return true;
}
static std::string asResultUtf8(const Uint32* result, int resultCount) {
    Uint16 t[MAX_BUFF];
    for (int i = 0; i < resultCount; i++) {
        t[i] = (result[i] & CHAR_CODE_MASK) ? (Uint16)(result[i] & 0xFFFF) : keyCodeToCharacter(result[i]);
    }
    return asUtf8(t, resultCount);
}
// Triples (D34, experiment): the two words before. Loaded at run time for now; off when not loaded.
static std::unordered_map<std::string, int>* _asTrigrams = nullptr;
static float _asTrigramWeight = 0;
static int _asTrigramMin = 3;
void vAutoSwitchSetTrigrams(const char* path, const float& weight, const int& minCount) {
    _asTrigramWeight = weight; _asTrigramMin = minCount;
    if (!path) return;
    FILE* f = fopen(path, "r"); if (!f) return;
    if (!_asTrigrams) _asTrigrams = new std::unordered_map<std::string, int>();
    char line[384];
    while (fgets(line, sizeof line, f)) {
        char* t1 = strchr(line, '\t'); if (!t1) continue; char* t2 = strchr(t1 + 1, '\t'); if (!t2) continue;
        char* t3 = strchr(t2 + 1, '\t'); if (!t3) continue;
        *t3 = 0; (*_asTrigrams)[std::string(line)] += atoi(t3 + 1);
    }
    fclose(f);
}
/** Like asBigramVote, with the two words before ("cho con" + bú/bus). 0 when the triple is too rare. */
static int asTrigramVote(const char* p2Letters, const bool& p2Vietnamese, const char* p1Letters, const bool& p1Vietnamese, const char* letters, const char* vnSurface = nullptr) {
    if (!_asTrigrams || _asTrigramWeight <= 0) return 0;
    std::string a, b, vnBuf;
    if (!asSurface(p2Letters, p2Vietnamese, a) || !asSurface(p1Letters, p1Vietnamese, b)) return 0;
    if (!vnSurface) { if (!asSurface(letters, true, vnBuf)) return 0; vnSurface = vnBuf.c_str(); }
    auto get = [&](const char* w) { auto it = _asTrigrams->find(a + "\t" + b + "\t" + w); return it == _asTrigrams->end() ? 0 : it->second; };
    int cv = get(vnSurface), ce = get(letters);
    if (cv + ce < _asTrigramMin) return 0;
    double v = 10 * log10((cv + 0.5) / (ce + 0.5));
    if (v > 30) v = 30; if (v < -30) v = -30;
    return (int)lround(v * _asTrigramWeight);
}

/** > 0: the Vietnamese reading follows prev more often, < 0: the English one (10 = 10x); 0: no data. */
static int asBigramVote(const char* prevLetters, const bool& prevVietnamese, const char* letters, const char* vnSurface = nullptr) {
    if (_asBigramWeight <= 0) return 0;
    std::string p, vnBuf;
    if (!asSurface(prevLetters, prevVietnamese, p)) return 0;
    if (!vnSurface) { if (!asSurface(letters, true, vnBuf)) return 0; vnSurface = vnBuf.c_str(); }
    auto get = [&](const char* w) {
        if (!_asBigrams) return asBuiltInPair2(p.c_str(), w);
        auto it = _asBigrams->find(p + "\t" + w); return it == _asBigrams->end() ? 0 : it->second; };
    int cv = get(vnSurface), ce = get(letters);
    if (cv + ce < _asBigramMin) return 0;
    double v = 10 * log10((cv + 0.5) / (ce + 0.5));
    if (v > 30) v = 30; if (v < -30) v = -30;
    return (int)lround(v * _asBigramWeight);
}

// ---- Capitalised mid-sentence ties (D37) ------------------------------------------------------------------
// "Max"/"Mã", "Bar"/"Bả", "Docs"/"Dóc": in the middle of a sentence a capital means a name or a brand, and
// real Vietnamese text says which reading people write that way. Loaded at run time for now; off when not loaded.
static std::unordered_map<std::string, int>* _asCapMid = nullptr;
static int _asCapMidMin = 5;
void vAutoSwitchSetCapitalised(const char* path, const int& minCount) {
    _asCapMidMin = minCount;
    if (!path) return;
    FILE* f = fopen(path, "r"); if (!f) return;
    if (!_asCapMid) _asCapMid = new std::unordered_map<std::string, int>();
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char* t = strchr(line, '\t'); if (!t) continue; *t = 0;
        (*_asCapMid)[std::string(line)] += atoi(t + 1);
    }
    fclose(f);
}
#ifndef AS_CAP_MARGIN
#define AS_CAP_MARGIN 10   //a capitalised reading this much more common mid-sentence (10x) decides
#endif
/** Only the first key is a capital ("Max", not "MAX" or "max"). */
static bool asCapitalisedOnly(const Uint32* keys, const int& count) {
    if (count < 2 || !(keys[0] & CAPS_MASK)) return false;
    for (int x = 1; x < count; x++) if (keys[x] & CAPS_MASK) return false;
    return true;
}
/** > 0: the capitalised Vietnamese reading ("Mã") is the one written mid-sentence, < 0: the English one ("Max"). */
static int asCapVote(const char* letters) {
    if (!_asCapMid) return 0;
    std::string vn;
    if (!asSurface(letters, true, vn)) return 0;
    auto get = [&](const std::string& w) { auto it = _asCapMid->find(w); return it == _asCapMid->end() ? 0 : it->second; };
    int cv = get(vn), ce = get(letters);
    if (cv + ce < _asCapMidMin) return 0;
    double v = 10 * log10((cv + 0.5) / (ce + 0.5));
    if (v > 30) v = 30; if (v < -30) v = -30;
    return (int)lround(v);
}

#ifndef AS_NEXT_PAIR
#define AS_NEXT_PAIR 20   //how sure (tuned on val: 10 -> 0.122%, 20 -> 0.120%) (20 = 100x) the following word must be of the Vietnamese reading to flip a word kept English (D35)
#endif
/** Look-forward (D35): the word after a word kept English. > 0 when the Vietnamese reading of the earlier word
 *  ("í", "cả") is followed by this word more often than the English one ("is", "car"); 0 without data. */
static int asNextPairVote(const char* prevLetters, const char* nextLetters, const char* prevSurface = nullptr, const char* nextSurface = nullptr) {
    if (_asBigramWeight <= 0) return 0;
    std::string pvBuf, nxBuf;
    if (!prevSurface) { if (!asSurface(prevLetters, true, pvBuf)) return 0; prevSurface = pvBuf.c_str(); }
    if (!nextSurface) { if (!asSurface(nextLetters, true, nxBuf)) return 0; nextSurface = nxBuf.c_str(); }
    auto get = [&](const char* a) {
        if (!_asBigrams) return asBuiltInPair2(a, nextSurface);
        auto it = _asBigrams->find(std::string(a) + "\t" + nextSurface); return it == _asBigrams->end() ? 0 : it->second; };
    int cv = get(prevSurface), ce = get(prevLetters);
    if (cv + ce < _asBigramMin) return 0;
    double v = 10 * log10((cv + 0.5) / (ce + 0.5));
    if (v > 30) v = 30; if (v < -30) v = -30;
    return (int)lround(v * _asBigramWeight);
}

static void asRememberWord() {
#ifdef AS_NO_LOOKBACK
    _asPrevCount = 0; return;
#endif
    if (!asEnabled()) return;
    asInitLetters();
    AsWord w;
    w.lookbackOk = false;
    w.same = false;
    w.rawCount = _stateIndex;
    w.shownCount = _index;
    if (_stateIndex <= 0 || _index <= 0 || _stateIndex >= MAX_BUFF || _longWordHelper.size() > 0) { _asPrevCount = 0; return; }
    for (int x = 0; x < _stateIndex; x++) w.raw[x] = KeyStates[x];
    for (int x = 0; x < _index; x++) w.shown[x] = GET(TypingWord[x]);
    vAutoSwitchLastSide = _asWordStartOk ? vAutoSwitchWordSide(KeyStates, _stateIndex) : -1;
    char word[AS_KEY_LEN], raw[AS_KEY_LEN];
    bool transformed;
    w.en = w.vn = AS_MISSING;
    if (_asWordStartOk && _stateIndex >= _index && asWordKey(word, transformed) && asRawWord(KeyStates, _stateIndex, raw)) {
        w.en = asEnglishScore(raw);
        if (transformed) {        //ambiguous: a real word both ways, and English not much rarer
            w.vn = asVietnameseScore(word);
            w.lookbackOk = w.en != AS_MISSING && w.vn != AS_MISSING && w.en >= w.vn - AS_LOOKBACK_MARGIN;
        } else {                  //looks the same in both modes: look-back passes through English-only words ("makes")
            w.lookbackOk = w.en != AS_MISSING && asVietnameseScore(raw) == AS_MISSING;
            w.same = true;
        }
    }
    for (int x = AS_LOOKBACK_WORDS - 1; x > 0; x--) _asPrev[x] = _asPrev[x - 1];
    _asPrev[0] = w;
    if (_asPrevCount < AS_LOOKBACK_WORDS) _asPrevCount++;
}

/** Vietnamese mode, at a word break: are the raw keys an English word the user clearly meant? */
static Uint32 _asDoubleKeys[MAX_BUFF];   //the keys of the word a doubled tone key switched to English, both presses last
static int _asDoubleCount = 0;
static int _asDoubleFirst = -1;      //where the first press is in _asDoubleKeys
static bool _asDoubleBoth = false;   //both presses are on screen
static bool asFewCapitals(const Uint32* keys, const int& count);
static bool _asApostrophe = false;   //the word ends at an apostrophe: allow contraction stems

static int asStatesWithDrops(Uint32* full, const int& maxN) {
    //newest drop first: an older drop's position never counts a newer dropped key
    int n = _stateIndex;
    if (n > maxN) n = maxN;
    memcpy(full, KeyStates, n * sizeof(Uint32));
    for (int d = _asDropN - 1; d >= 0; d--) {
        int p = _asDropPos[d];
        if (p < 0 || p > n || n + 1 > maxN) continue;
        memmove(full + p + 1, full + p, (n - p) * sizeof(Uint32));
        full[p] = _asDropKey[d]; n++;
    }
    return n;
}

static bool asShouldRestoreToEnglishCore() {
    if (!asEnabled() || !_asWordStartOk) return false;
    asInitLetters();
    if (_stateIndex < 2 || _stateIndex < _index) return false;
    char word[AS_KEY_LEN], raw[AS_KEY_LEN];
    bool transformed;
    bool wordOk = asWordKey(word, transformed);
    //Telex dropped a key on the way ("seemed" -> "semed", "cheese" -> "chees"): with it back, an English word?
    if (_asDropN > 0 && _stateIndex >= 3 && _stateIndex + _asDropN < MAX_BUFF - 1) {   //4+ keys: "aaa" stays Telex "aa"
        Uint32 full[MAX_BUFF];
        int n = asStatesWithDrops(full, MAX_BUFF);
            bool savedTemp = tempDisableKey;
        checkSpelling(true);
        bool invalid = tempDisableKey;   //the shown word is no syllable ("chée")
        tempDisableKey = savedTemp;
        if (asRawWord(full, n, raw) && asEnglishScore(raw) != AS_MISSING && (!wordOk || !transformed || invalid || asVietnameseScore(word) == AS_MISSING)) {
            memcpy(KeyStates, full, n * sizeof(Uint32));   //restore puts these keys back
            _stateIndex = n;
            _asDropN = 0;
            return true;
        }
    }
    if (!wordOk) return false;
    if (!transformed) {
        //no marks left, but Telex ate a key on the way ("seemed" -> "semed", "cheese" -> "chees"): put back an English word
        if (asRawWord(KeyStates, _stateIndex, raw) && strcmp(raw, word) != 0 && asEnglishScore(raw) != AS_MISSING)
            return true;
#ifndef AS_PLAIN_AFTER_AMBIGUOUS
#define AS_PLAIN_AFTER_AMBIGUOUS 1
#endif
#if AS_PLAIN_AFTER_AMBIGUOUS
        //An English-only word that looks the same in both modes ("makes") right after an ambiguous word that
        //stayed Vietnamese ("thí" = this): together they are English. Switch now and fix the earlier word.
        if (_asPrevCount < 1 || !_asPrev[0].lookbackOk || _asPrev[0].same) return false;
        if (_asPrev[0].en < _asPrev[0].vn + AS_PLAIN_MARGIN) return false;   //the earlier word must lean English
        if (!asRawWord(KeyStates, _stateIndex, raw)) return false;
        return asEnglishScore(raw) != AS_MISSING && asVietnameseScore(raw) == AS_MISSING;
#else
        return false;
#endif
    }
    if (!asRawWord(KeyStates, _stateIndex, raw)) return false;
    int en = asEnglishScore(raw);
    int vn = asVietnameseScore(word);
    //a capitalised word mid-sentence (D37): which capitalised reading real text uses
    if (_asCapMid && vn != AS_MISSING && _asPrevCount >= 1 && asCapitalisedOnly(KeyStates, _stateIndex)) {   //names need not be English words (Los, Las)
        int cv = asCapVote(raw);
        if (cv <= -AS_CAP_MARGIN) return true;
        if (cv >= AS_CAP_MARGIN) return false;
    }

#ifndef AS_ACRONYM
#define AS_ACRONYM 1
#endif
#ifndef AS_ACRONYM_VN
#define AS_ACRONYM_VN 5     //below ~3 per million: tuned on Tinhte (acronyms) vs all-caps Vietnamese news
#endif
#ifndef AS_ACRONYM_VN_AMID
#define AS_ACRONYM_VN_AMID 15   //right after a lowercase word ("con NAS này"): Caps Lock Vietnamese comes in runs (D27)
#endif
#if AS_ACRONYM
    //2+ capitals (NAS, HDD, SATA, GoPro) that Telex turned into a rare or invalid syllable: an acronym or a name (D23)
    //not inside a run of capitals: no earlier word, or a lowercase one
    bool afterLower = _asPrevCount < 1 || _asPrev[0].rawCount < 1;
    if (!afterLower) { afterLower = true; for (int x = 0; x < _asPrev[0].rawCount; x++) if (_asPrev[0].raw[x] & CAPS_MASK) afterLower = false; }
    if (!asFewCapitals(KeyStates, _stateIndex) && (vn == AS_MISSING || vn < (afterLower ? AS_ACRONYM_VN_AMID : AS_ACRONYM_VN))) {
        bool vowelChanged = false;   //Vietnamese acronyms only have đ changed (VĐV, HĐND, BĐS): they stay
        for (int x = 0; x < _index; x++) {
            char c = asLetterOf(TypingWord[x]);
            if ((c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y') && (TypingWord[x] & (MARK_MASK | TONE_MASK | TONEW_MASK)))
                vowelChanged = true;
        }
        if (vowelChanged) return true;
    }
#endif
    if (en == AS_MISSING && _asApostrophe) {             //a contraction stem: doesn't, isn't, wasn't -> is it "does" + n?
        size_t n = strlen(raw);
        if (n >= 3 && raw[n - 1] == 'n') { raw[n - 1] = 0; en = asEnglishScore(raw); raw[n - 1] = 'n'; }
    }
#ifndef AS_RESTORE_UNKNOWN
#define AS_RESTORE_UNKNOWN 3   //min letters for restoring a word that is neither Vietnamese nor in the English list
#endif
#if AS_RESTORE_UNKNOWN > 0
    //not a Vietnamese syllable and not a known English word either: a foreign word (TestFlight, analytics)
    if (en == AS_MISSING) {
        if (vn != AS_MISSING || _stateIndex < AS_RESTORE_UNKNOWN) return false;
#ifndef AS_UNKNOWN_ALLOW_MODIFIERS
        //Vietnamese-only modifiers (â ê ô ă ơ ư đ) mean the user was writing Vietnamese: a typo, not a foreign
        //word. Unless the keys have several vowel groups: no single syllable has that (analytics -> "ânlytics").
        int vowelGroups = 0;
        bool inVowel = false;
        for (int x = 0; x < _stateIndex; x++) {
            char c = asLetterOf(KeyStates[x]);
            bool v = c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
            if (v && !inVowel) vowelGroups++;
            inVowel = v;
        }
        if (vowelGroups <= 1)
            for (int x = 0; x < _index; x++) if (TypingWord[x] & (TONE_MASK | TONEW_MASK)) return false;
#endif
        //rare but valid syllables (riệt, chịt) are not in the list: require the spelling rules to fail too
        bool savedTemp = tempDisableKey;
        checkSpelling(true);
        bool invalid = tempDisableKey;
        tempDisableKey = savedTemp;
        return invalid;
    }
#else
    if (en == AS_MISSING) return false;
#endif
#ifndef AS_VN_EXTREME
#define AS_VN_EXTREME 99    //an English word this many points more common wins even over a common Vietnamese word
#endif
#ifndef AS_TIE_PREV
#define AS_TIE_PREV 5       //a tie leaning English (3x) goes English right after a clearly English word (D22)
#endif
    //word pairs (D30): the reading that follows the previous word more often in real text
    if ((_asBigramWeight > 0 || _asTrigrams) && en != AS_MISSING && vn != AS_MISSING && _asPrevCount >= 1) {
        char praw[AS_KEY_LEN], p2raw[AS_KEY_LEN];
        if (asRawWord(_asPrev[0].raw, _asPrev[0].rawCount, praw)) {
            int vote = 0;
            if (_asPrevCount >= 2 && asRawWord(_asPrev[1].raw, _asPrev[1].rawCount, p2raw))
                vote = asTrigramVote(p2raw, !_asPrev[1].same, praw, !_asPrev[0].same, raw);
            if (vote == 0) vote = asBigramVote(praw, !_asPrev[0].same, raw);
            if (vote != 0) return en - vn - vote >= AS_PREFER && (vn < AS_VN_FLOOR || vote <= -AS_PREFER);
        }
    }
    if (en != AS_MISSING && vn != AS_MISSING && en - vn >= AS_TIE_PREV && _asPrevCount >= 1 &&
        _asPrev[0].same && _asPrev[0].en != AS_MISSING && _asPrev[0].vn == AS_MISSING)
        return true;   //"video call thí" -> "this"
    // D6: code identifiers in en_code.txt protect English mode, never override valid Vietnamese words in VN mode.
    if (vn != AS_MISSING && asIsCodeWord(raw)) return false;
    return vn == AS_MISSING || (vn < AS_VN_FLOOR && en - vn >= AS_PREFER) || en - vn >= AS_VN_EXTREME;
}

/** Whether to put the word back to the keys typed. A foreign word restored as typed gets back the keys
 *  Telex undid on the way too ("deepseek", not "deepsek"); a known English word without them stays. */
static bool asShouldRestoreToEnglish() {
    if (!asShouldRestoreToEnglishCore()) return false;
    if (_asDropN > 0 && _stateIndex >= 3 && _stateIndex + _asDropN < MAX_BUFF - 1) {
        char raw[AS_KEY_LEN];
        Uint32 full[MAX_BUFF];
        int n = asStatesWithDrops(full, MAX_BUFF - 1);
        //only for words of 2+ vowel groups (deep-seek): in one syllable "ooo" is the Telex undo ("cuooocf" -> "cuoocf")
        int groups = 0; bool inV = false;
        for (int x = 0; x < n; x++) { char c = asLetterOf(full[x]); bool v = c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y'; if (v && !inV) groups++; inV = v; }
        if (groups >= 2 && !(asRawWord(KeyStates, _stateIndex, raw) && asEnglishScore(raw) != AS_MISSING)) {
            memcpy(KeyStates, full, n * sizeof(Uint32));
            _stateIndex = n;
            _asDropN = 0;
        }
    }
    return true;
}

/** Before a hyphen: a rare w-word ("ưi") starts an English compound ("Wi-Fi", D23). */
static bool asHyphenRestore() {
    if (!asEnabled() || !_asWordStartOk || _stateIndex < 2 || _stateIndex < _index) return false;
    asInitLetters();
    char word[AS_KEY_LEN];
    bool transformed;
    bool ok = asWordKey(word, transformed);
    if (!ok || !transformed) return false;
    //only words starting with w (Telex "ư"): "nơ-ron", "rơ-moóc", "táp-lô" are Vietnamese loanwords
    if (asLetterOf(KeyStates[0]) != 'w') return false;
    int vn = asVietnameseScore(word);
    return vn == AS_MISSING || vn < AS_VN_FLOOR;
}

/** Puts the raw keys back (like "restore if wrong spelling") and asks for English.
 *  Ambiguous words just before it (look-back) are put back to their keys too. */
bool vAutoSwitchPrevRestored = false;        //previous[0] of the next English-word check is a word auto switch put back to English
Uint32 vAutoSwitchRestoredKeys[MAX_BUFF];   //the keys of the word just put back to English (D35: a later word can look back on it)
int vAutoSwitchRestoredCount = 0;
static void asRestoreToEnglish(const int& handleCode) {
    vAutoSwitchRestoredCount = 0;
    if (_stateIndex > 0 && _stateIndex < MAX_BUFF) {
        memcpy(vAutoSwitchRestoredKeys, KeyStates, _stateIndex * sizeof(Uint32));
        vAutoSwitchRestoredCount = _stateIndex;
    }
    //how many earlier words follow the switch: consecutive, ambiguous, and fitting in the output buffer.
    //Not after an acronym (2+ capitals): Vietnamese text is full of them, so they say nothing about the
    //words before ("bên ASL", "của USA" stay Vietnamese: Tony, D42)
    int back = 0, newCount = _stateIndex, deleteCount = _index;
    bool acronym = !asFewCapitals(KeyStates, _stateIndex);
    while (!acronym && back < _asPrevCount && _asPrev[back].lookbackOk &&
           newCount + _asPrev[back].rawCount + 1 < MAX_BUFF && deleteCount + _asPrev[back].shownCount + 1 < MAX_BUFF) {
        newCount += _asPrev[back].rawCount + 1;
        deleteCount += _asPrev[back].shownCount + 1;
        back++;
    }
    while (back > 0 && _asPrev[back - 1].same) {         //no need to retype unchanged words at the far end
        back--;
        newCount -= _asPrev[back].rawCount + 1;
        deleteCount -= _asPrev[back].shownCount + 1;
    }
    Uint32 out[MAX_BUFF];
    int n = 0;
    vAutoSwitchUndoCount = 0;
    for (int b = back - 1; b >= 0; b--) {
        for (int x = 0; x < _asPrev[b].rawCount; x++) out[n++] = _asPrev[b].raw[x];
        out[n++] = KEY_SPACE;
        for (int x = 0; x < _asPrev[b].shownCount; x++) vAutoSwitchUndoData[vAutoSwitchUndoCount++] = _asPrev[b].shown[x];
        vAutoSwitchUndoData[vAutoSwitchUndoCount++] = KEY_SPACE;
    }
    for (int x = 0; x < _index && vAutoSwitchUndoCount < MAX_BUFF; x++) {
        vAutoSwitchUndoData[vAutoSwitchUndoCount++] = GET(TypingWord[x]);
    }
    for (i = 0; i < _stateIndex; i++) {
        TypingWord[i] = KeyStates[i];
        out[n++] = KeyStates[i];
    }
    hCode = handleCode;
    hBPC = deleteCount;
    hNCC = n;
    for (int x = 0; x < n; x++) hData[n - 1 - x] = out[x];
    _index = _stateIndex;
    _asPrevCount = 0;
    vAutoSwitchRequest = 0;
}

/** Break keys that end a word for auto switch (not arrows, tab, esc, mouse or shortcuts). */
static bool asIsSwitchBreak(const vKeyEvent& event, const Uint16& data, const Uint8& capsStatus, const bool& otherControlKey) {
    if (event != vKeyEvent::Keyboard || otherControlKey) return false;
    return data == KEY_COMMA || data == KEY_DOT || data == KEY_SEMICOLON || data == KEY_ENTER || data == KEY_RETURN ||
           data == KEY_QUOTE ||                                 //closing quote, or an apostrophe: doesn't, we're
           (data == KEY_SLASH && capsStatus == 1) || (data == KEY_1 && capsStatus == 1) || (data == KEY_0 && capsStatus == 1) ||
           (data == KEY_9 && capsStatus == 1);                  //"(" right after a word: web(làm (D27)
}

/** Everything the engine keeps between keys, so a word can be replayed on a scratch session. */
struct AsEngineState {
    Uint32 word[MAX_BUFF], states[MAX_BUFF];
    Byte index, stateIndex;
    bool tempDisable, useSpellingBefore, wordStartOk, afterDigit;
    int dropN; int dropPos[AS_MAX_DROPS]; Uint32 dropKey[AS_MAX_DROPS];
    int spaceCount, checkSpelling, language, autoSwitchByTyping, rotationalTone;
    int autoSwitchRequest, autoSwitchLastSide;
    int doubleCount, doubleFirst; bool doubleBoth; Uint32 doubleKeys[MAX_BUFF];
    Byte vowelCnt, vsi, vei, vwsm, spellingEnd;
    bool spellingOk, spellingFlag, spellingVowelOk;
    vector<Uint32> longWord, special, statesData;
    deque<TypingStateSnapshot> typing;
    vKeyHookState hook;
};

static void asSaveState(AsEngineState& st) {
    memcpy(st.word, TypingWord, sizeof(TypingWord));
    memcpy(st.states, KeyStates, sizeof(KeyStates));
    st.index = _index; st.stateIndex = _stateIndex;
    st.tempDisable = tempDisableKey; st.useSpellingBefore = _useSpellCheckingBefore;
    st.wordStartOk = _asWordStartOk; st.afterDigit = _afterDigit;
    st.dropN = _asDropN; memcpy(st.dropPos, _asDropPos, sizeof(_asDropPos)); memcpy(st.dropKey, _asDropKey, sizeof(_asDropKey));
    st.spaceCount = _spaceCount; st.checkSpelling = vCheckSpelling;
    st.language = vLanguage; st.autoSwitchByTyping = vAutoSwitchByTyping; st.rotationalTone = vRotationalTone;
    st.autoSwitchRequest = vAutoSwitchRequest; st.autoSwitchLastSide = vAutoSwitchLastSide;
    st.doubleCount = _asDoubleCount; st.doubleFirst = _asDoubleFirst; st.doubleBoth = _asDoubleBoth;
    memcpy(st.doubleKeys, _asDoubleKeys, sizeof(_asDoubleKeys));
    st.vowelCnt = vowelCount; st.vsi = vowelStartIndex; st.vei = vowelEndIndex; st.vwsm = vowelWillSetMark;
    st.spellingOk = _spellingOK; st.spellingFlag = _spellingFlag; st.spellingVowelOk = _spellingVowelOK; st.spellingEnd = _spellingEndIndex;
    st.longWord.swap(_longWordHelper);
    st.special.swap(_specialChar);
    st.statesData.swap(_typingStatesData);
    st.typing.swap(_typingStates);
    st.hook = std::move(HookState);
}

static void asRestoreState(AsEngineState& st) {
    memcpy(TypingWord, st.word, sizeof(TypingWord));
    memcpy(KeyStates, st.states, sizeof(KeyStates));
    _index = st.index; _stateIndex = st.stateIndex;
    tempDisableKey = st.tempDisable; _useSpellCheckingBefore = st.useSpellingBefore;
    _asWordStartOk = st.wordStartOk; _afterDigit = st.afterDigit;
    _asDropN = st.dropN; memcpy(_asDropPos, st.dropPos, sizeof(_asDropPos)); memcpy(_asDropKey, st.dropKey, sizeof(_asDropKey));
    _spaceCount = st.spaceCount; vCheckSpelling = st.checkSpelling;
    vLanguage = st.language; vAutoSwitchByTyping = st.autoSwitchByTyping; vRotationalTone = st.rotationalTone;
    vAutoSwitchRequest = st.autoSwitchRequest; vAutoSwitchLastSide = st.autoSwitchLastSide;
    _asDoubleCount = st.doubleCount; _asDoubleFirst = st.doubleFirst; _asDoubleBoth = st.doubleBoth;
    memcpy(_asDoubleKeys, st.doubleKeys, sizeof(_asDoubleKeys));
    vowelCount = st.vowelCnt; vowelStartIndex = st.vsi; vowelEndIndex = st.vei; vowelWillSetMark = st.vwsm;
    _spellingOK = st.spellingOk; _spellingFlag = st.spellingFlag; _spellingVowelOK = st.spellingVowelOk; _spellingEndIndex = st.spellingEnd;
    _longWordHelper.swap(st.longWord);
    _specialChar.swap(st.special);
    _typingStatesData.swap(st.statesData);
    _typingStates.swap(st.typing);
    HookState = std::move(st.hook);
}

#ifdef AS_TESTING
//test harness only: snapshot the whole engine so a word can be tried and taken back
void* vTestSaveState() { AsEngineState* st = new AsEngineState(); asSaveState(*st); return st; }
void vTestRestoreState(void* p) { AsEngineState* st = (AsEngineState*)p; asRestoreState(*st); delete st; }
void vTestFreeState(void* p) { delete (AsEngineState*)p; }
#endif

/** Replays raw keys through the Telex engine on a scratch session. Returns the Vietnamese score of
 *  the result (AS_MISSING if it isn't a real syllable or nothing changed) and the result characters. */
static int asReplay(const Uint32* keys, const int& count, Uint32* result, int& resultCount, bool* changed = NULL) {
    resultCount = 0;
    AsEngineState saved;
    asSaveState(saved);
    _spaceCount = 0;
    vAutoSwitchByTyping = 0;
    vCheckSpelling = 1;
    vRotationalTone = 0;
    startNewSession();
    for (int x = 0; x < count; x++) {
        vKeyHandleEvent(vKeyEvent::Keyboard, vKeyEventState::KeyDown, (Uint16)(keys[x] & CHAR_MASK), (keys[x] & CAPS_MASK) ? 1 : 0, false);
    }
    int vn = AS_MISSING;
    char word[AS_KEY_LEN];
    bool transformed;
    bool keyOk = asWordKey(word, transformed);
    if (changed) *changed = keyOk ? transformed : true;
    if (keyOk && transformed) {
        vn = asVietnameseScore(word);
        if (vn != AS_MISSING) {
            for (int x = 0; x < _index; x++) result[resultCount++] = GET(TypingWord[x]);
        }
    }
    asRestoreState(saved);
    return vn;
}

static bool asFewCapitals(const Uint32* keys, const int& count) {
    int capitals = 0;
    for (int x = 0; x < count; x++) {
        if (!(keys[x] & CAPS_MASK)) continue;
        //DD is one letter, Đ: "DDaf Nawngx" is Đà Nẵng, while DDR, DDoS still have two capitals (D39)
        if (x > 0 && (keys[x] & CHAR_MASK) == KEY_D && (keys[x - 1] & CHAR_MASK) == KEY_D && (keys[x - 1] & CAPS_MASK)) continue;
        capitals++;
    }
    return capitals < 2;   //USA, EST, IRS, camelCase: acronyms and identifiers stay as typed
}

bool vAutoSwitchCheckEnglishWord(const Uint32* keys, const int& count,
                                 const Uint32* const* previous, const int* previousCounts, const int& previousN) {
    if (!asEnabled() || count < 2 || count >= MAX_BUFF - 1) return false;
    asInitLetters();
    char raw[AS_KEY_LEN];
    if (!asRawWord(keys, count, raw)) return false;
    //2+ capitals: acronyms and identifiers stay as typed. All capitals (Caps Lock Vietnamese, "COONG") may still
    //convert, but only to a common syllable and never when the keys are English (USA, EST stay): D23
    bool allCaps = true;
    for (int x = 0; x < count; x++) if (!(keys[x] & CAPS_MASK)) allCaps = false;
    if (!asFewCapitals(keys, count) && !allCaps) return false;
    int en = asEnglishScore(raw);

    Uint32 result[MAX_BUFF];
    int resultCount;
    int vn = asReplay(keys, count, result, resultCount);
    std::string vnSurface;
    if (vn != AS_MISSING && resultCount > 0) vnSurface = asResultUtf8(result, resultCount);
    const char* vnSurfPtr = vnSurface.empty() ? nullptr : vnSurface.c_str();
    //word pairs (D30): "cho con bus" -> bú, "the bus" stays
    int pairVote = 0;
    if ((_asBigramWeight > 0 || _asTrigrams) && en != AS_MISSING && vn != AS_MISSING && previousN >= 1 && previousCounts[0] >= 1) {
        char praw[AS_KEY_LEN];
        char p2raw[AS_KEY_LEN];
        if (asRawWord(previous[0], previousCounts[0], praw)) {
            if (previousN >= 2 && previousCounts[1] >= 1 && asRawWord(previous[1], previousCounts[1], p2raw))
                pairVote = asTrigramVote(p2raw, false, praw, false, raw, vnSurfPtr);
            if (pairVote == 0) pairVote = asBigramVote(praw, false, raw, vnSurfPtr);
        }
    }
    int capVote = 0;   //D37: a capitalised word mid-sentence goes with the capitalised reading real text uses
    if (_asCapMid && vn != AS_MISSING && previousN >= 1 && asCapitalisedOnly(keys, count)) {
        capVote = asCapVote(raw);
        if (capVote <= -AS_CAP_MARGIN) return false;
    }
    if (vn == AS_MISSING || !(en == AS_MISSING || capVote >= AS_CAP_MARGIN || (en < AS_EN_FLOOR && vn - en + pairVote >= AS_PREFER))) return false;
#ifndef AS_CAPS_VN_FLOOR
#define AS_CAPS_VN_FLOOR 15   //all capitals convert only to a common syllable (IRS stays, not "Í"): D23, D27
#endif
    if (count > 1 && allCaps && (en != AS_MISSING || vn < AS_CAPS_VN_FLOOR)) return false;
    //a lone all-caps word converts when the Telex marks it as Vietnamese: THOONG, THUOWNGF, VIEETJ (Tony, D41).
    //Acronyms rarely double a vowel or put w after a vowel, and the floor above already keeps rare syllables out
    bool telexMarked = false;
    for (int x = 0; raw[x] && raw[x + 1]; x++) {
        char a = raw[x] | 0x20, b = raw[x + 1] | 0x20;
        if ((a == b && (a == 'a' || a == 'e' || a == 'o' || a == 'd')) || (b == 'w' && (a == 'a' || a == 'o' || a == 'u'))) telexMarked = true;
    }
    if (count > 1 && allCaps && !(count >= 4 && telexMarked)) {   //only inside a run of capitals (Caps Lock on): a lone HW, CWS stays
        bool prevCaps = previousN >= 1 && previousCounts[0] >= 2;
        for (int x = 0; prevCaps && x < previousCounts[0]; x++) if (!(previous[0][x] & CAPS_MASK)) prevCaps = false;
        if (!prevCaps) return false;
    }
    //a tie right after a clearly English word stays English ("the theme", "of rust"): D22
#ifndef AS_PAIR_OVERRIDES_D22
#define AS_PAIR_OVERRIDES_D22 10   //word pairs this sure of the Vietnamese reading (10x) beat D22: "team bên US" (D34)
#endif
    bool hasCachedPrev0 = false;
    int cachedPrev0Pen = AS_MISSING, cachedPrev0Pvn = AS_MISSING;
    bool cachedPrev0Changed = false;
    Uint32 cachedPrev0Out[MAX_BUFF]; int cachedPrev0OutCount = 0;
    if (en != AS_MISSING && previousN >= 1 && previousCounts[0] >= 1 && pairVote < AS_PAIR_OVERRIDES_D22 && !vAutoSwitchPrevRestored && capVote < AS_CAP_MARGIN) {
        char praw[AS_KEY_LEN];
        if (asRawWord(previous[0], previousCounts[0], praw)) {
            cachedPrev0Pen = asEnglishScore(praw);
            cachedPrev0Pvn = asReplay(previous[0], previousCounts[0], cachedPrev0Out, cachedPrev0OutCount, &cachedPrev0Changed);
            hasCachedPrev0 = true;
            if (cachedPrev0Pen != AS_MISSING && (cachedPrev0Pvn == AS_MISSING || cachedPrev0Pen - cachedPrev0Pvn >= AS_PREFER)) return false;
        }
    }

    //look-back: ambiguous words just before it (kept English because they are English words too)
    Uint32 out[MAX_BUFF];
    int n = 0, deleteCount = count, back = 0;
    Uint32 earlier[AS_LOOKBACK_WORDS][MAX_BUFF];
    int earlierCount[AS_LOOKBACK_WORDS];
    bool earlierSame[AS_LOOKBACK_WORDS];
    for (; back < previousN && back < AS_LOOKBACK_WORDS; back++) {
        const Uint32* pk = previous[back];
        int pc = previousCounts[back];
        char praw[AS_KEY_LEN];
        if (pc < 2 || !asRawWord(pk, pc, praw) || !asFewCapitals(pk, pc)) break;
        int pen, pvn;
        bool changed;
        if (back == 0 && hasCachedPrev0) {
            pen = cachedPrev0Pen;
            pvn = cachedPrev0Pvn;
            changed = cachedPrev0Changed;
            earlierCount[0] = cachedPrev0OutCount;
            if (cachedPrev0OutCount > 0) memcpy(earlier[0], cachedPrev0Out, cachedPrev0OutCount * sizeof(Uint32));
        } else {
            pen = asEnglishScore(praw);
            pvn = asReplay(pk, pc, earlier[back], earlierCount[back], &changed);
        }
        earlierSame[back] = !changed;
        if (!changed) {           //looks the same in both modes: pass through Vietnamese-only words ("cho")
            if (pen != AS_MISSING || asVietnameseScore(praw) == AS_MISSING) break;
            earlierCount[back] = pc;
            for (int x = 0; x < pc; x++) earlier[back][x] = pk[x];
        } else if (pen == AS_MISSING || pvn == AS_MISSING) {
            break;
        } else {
            int npv = 0;
            if (back == 0) {
                std::string prevSurf = asResultUtf8(earlier[0], earlierCount[0]);
                npv = asNextPairVote(praw, raw, prevSurf.empty() ? nullptr : prevSurf.c_str(), vnSurfPtr);
            }
            if (back == 0 && vAutoSwitchPrevRestored ? (npv < AS_NEXT_PAIR) :
                (pvn < pen - AS_LOOKBACK_MARGIN && !(back == 0 && npv >= AS_NEXT_PAIR))) {
                break;
            }
        }
        if (resultCount + n + earlierCount[back] + 1 >= MAX_BUFF || deleteCount + pc + 1 >= MAX_BUFF) break;
        n += earlierCount[back] + 1;
        deleteCount += pc + 1;
    }
    while (back > 0 && earlierSame[back - 1]) {          //no need to retype unchanged words at the far end
        back--;
        deleteCount -= previousCounts[back] + 1;
    }
    n = 0;
    vAutoSwitchUndoCount = 0;
    for (int b = back - 1; b >= 0; b--) {
        for (int x = 0; x < earlierCount[b]; x++) out[n++] = earlier[b][x];
        out[n++] = KEY_SPACE;
        for (int x = 0; x < previousCounts[b]; x++) vAutoSwitchUndoData[vAutoSwitchUndoCount++] = previous[b][x];
        vAutoSwitchUndoData[vAutoSwitchUndoCount++] = KEY_SPACE;
    }
    for (int x = 0; x < resultCount; x++) out[n++] = result[x];
    for (int x = 0; x < count; x++) vAutoSwitchUndoData[vAutoSwitchUndoCount++] = keys[x];

    hCode = vRestore;          //platform sends the new text, then the break key itself
    hBPC = (Byte)deleteCount;
    hNCC = (Byte)n;
    hExt = 3;
    for (int x = 0; x < n; x++) hData[n - 1 - x] = out[x];
    return true;
}

bool vAutoSwitchCheckEnglishWord(const Uint32* keys, const int& count) {
    return vAutoSwitchCheckEnglishWord(keys, count, NULL, NULL, 0);
}
/*==========================================================================================================*/


void vKeyHandleEvent(const vKeyEvent& event,
                     const vKeyEventState& state,
                     const Uint16& data,
                     const Uint8& capsStatus,
                     const bool& otherControlKey) {
    _isCaps = (capsStatus == 1 || //shift
               capsStatus == 2); //caps lock
    if ((IS_NUMBER_KEY(data) && capsStatus == 1)
        || otherControlKey || isWordBreak(event, state, data) || (IS_NUMBER_KEY(data) && (_index == 0 || _spaceCount > 0))) {   //a digit after a space starts a new word: "mua 1casi" -> "1cái" (D33); glued to letters (abc1def) it stays part of them
        hCode = vDoNothing;
        hBPC = 0;
        hNCC = 0;
        hExt = 1; //word break
        
        _asApostrophe = data == KEY_QUOTE && capsStatus != 1;
        bool asSwitch = (asIsSwitchBreak(event, data, capsStatus, otherControlKey) && asShouldRestoreToEnglish()) ||
                        (data == KEY_MINUS && capsStatus != 1 && !otherControlKey && asHyphenRestore());
        _asApostrophe = false;
        if (asSwitch) {
            asRestoreToEnglish(vRestoreAndStartNewSession); //auto switch: an English word typed in Vietnamese mode
        } else if (!otherControlKey && event != vKeyEvent::Mouse && vRestoreIfWrongSpelling && isWordBreak(event, state, data)) { //restore key if wrong spelling with break-key
            if (!tempDisableKey && vCheckSpelling) {
                checkSpelling(true); //force check spelling
            }
            if (tempDisableKey && !checkRestoreIfWrongSpelling(vRestoreAndStartNewSession)) {
                hCode = vDoNothing;
            }
        }
        
        _isCharKeyCode = state == KeyDown && std::find(_charKeyCode.begin(), _charKeyCode.end(), data) != _charKeyCode.end();
        if (!_isCharKeyCode) { //clear all line cache
            _specialChar.clear();
            _typingStates.clear();
        } else { //check and save current word
            if (_spaceCount > 0) {
                saveWord(KEY_SPACE, _spaceCount);
                _spaceCount = 0;
            } else {
                saveWord();
            }
            _specialChar.push_back(data | (_isCaps ? CAPS_MASK : 0));
            hExt = 3;//normal word
        }
        
        if (hCode == vDoNothing || event == vKeyEvent::Mouse) {
            startNewSession();
            vCheckSpelling = _useSpellCheckingBefore;
        }
        _asPrevCount = 0;
        //the next word is a whole word only after whitespace, a click, a shortcut or an opening ( [ { " ',
        //not after . _ - / etc.
        _afterDigit = IS_NUMBER_KEY(data) && capsStatus != 1;
        _asWordStartOk = event == vKeyEvent::Mouse || otherControlKey || data == KEY_ENTER || data == KEY_RETURN || data == KEY_TAB ||
                         (data == KEY_9 && capsStatus == 1) || data == KEY_LEFT_BRACKET || data == KEY_QUOTE;
    } else if (data == KEY_SPACE) {
        _afterDigit = false;
        if (!tempDisableKey && vCheckSpelling) {
            checkSpelling(true); //force check spelling
        }
        if (asShouldRestoreToEnglish()) { //auto switch: an English word typed in Vietnamese mode
            asRestoreToEnglish(vRestore);
            _spaceCount++;
        } else if (vRestoreIfWrongSpelling && tempDisableKey) { //restore key if wrong spelling
            if (!checkRestoreIfWrongSpelling(vRestore)) {
                hCode = vDoNothing;
            }
            _spaceCount++;
        } else { //do nothing with SPACE KEY
            hCode = vDoNothing;
            _spaceCount++;
        }
        //look-back memory: only words separated by a single space count
        if (hCode == vDoNothing && _spaceCount == 1) asRememberWord(); else if (_spaceCount > 1) _asPrevCount = 0;
        //save word
        if (_spaceCount == 1) {
            if (_specialChar.size() > 0) {
                saveSpecialChar();
            } else {
                saveWord();
            }
        }
        vCheckSpelling = _useSpellCheckingBefore;
        _asWordStartOk = true;
    } else if (data == KEY_DELETE) {
        _asPrevCount = 0;
        hCode = vDoNothing;
        hExt = 2; //delete
        if (_specialChar.size() > 0) {
            _specialChar.pop_back();
            if (_specialChar.size() == 0) {
                restoreLastTypingState();
            }
        } else if (_spaceCount > 0) { //previous char is space
            _spaceCount--;
            if (_spaceCount == 0) { //restore word
                restoreLastTypingState();
            }
        } else {
            if (_stateIndex > 0) {
                _stateIndex--;
                while (_asDropN > 0 && _stateIndex < _asDropPos[_asDropN - 1]) _asDropN--;
            }
            if (_index > 0){
                _index--;
                if (_longWordHelper.size() > 0) {
                    //right shift
                    for (i = MAX_BUFF - 1; i > 0; i--) {
                        TypingWord[i] = TypingWord[i-1];
                    }
                    TypingWord[0] = _longWordHelper.back();
                    _longWordHelper.pop_back();
                    _index++;
                }
                if (vCheckSpelling)
                    checkSpelling();
            }
            hBPC = 0;
            hNCC = 0;
            hExt = 2; //delete key
            if (_index == 0) {
                startNewSession();
                _specialChar.clear();
                restoreLastTypingState();
            } else { //August 23rd continue check grammar
                checkGrammar(1);
            }
        }
    } else { //START AND CHECK KEY
        if (_spaceCount > 0) {
            hBPC = 0;
            hNCC = 0;
            hExt = 0;
            startNewSession();
            //continute save space
            saveWord(KEY_SPACE, _spaceCount);
            _spaceCount = 0;
        } else if (_specialChar.size() > 0) {
            saveSpecialChar();
        }

        insertState(data, _isCaps); //save state
        
        if (!IS_SPECIALKEY(data) || tempDisableKey) { //do nothing
            hCode = vDoNothing;
            hBPC = 0;
            hNCC = 0;
            hExt = 3; //normal key
            insertKey(data, _isCaps);
        } else { //check and update key
            //restore state
            hCode = vDoNothing;
            hExt = 3; //normal key
            handleMainKey(data, _isCaps);
            //auto switch: the same tone key pressed twice undoes the tone, a strong sign of English
            if (hCode == vRestore && asEnabled() && IS_MARK_KEY(data)) {
                vAutoSwitchRequest = 0;
                //Switching to English: put the whole word back to the keys typed, not just the undone tone
                //("tomorr", not "tômrr"). The second press is added after these by the caller (vRestore).
                //Like other Telex input methods the first press goes ("ver"), unless both make an English word
                //already ("err", "boss"); the end of the word decides for good (D19, vAutoSwitchFixDoubledKey).
                if (_stateIndex >= 2 && _stateIndex <= MAX_BUFF && _index <= MAX_BUFF) {
                    int first = -1;                                  //the press that set the tone
                    for (int x = _stateIndex - 2; x >= 0; x--)
                        if ((KeyStates[x] & CHAR_MASK) == data) { first = x; break; }
                    char raw[AS_KEY_LEN];
                    asInitLetters();
                    bool both = first < 0 || (asRawWord(KeyStates, _stateIndex, raw) && asEnglishScore(raw) != AS_MISSING);
                    hBPC = _index;
                    hNCC = 0;
                    for (int x = _stateIndex - 2; x >= 0; x--)
                        if (both || x != first) hData[hNCC++] = KeyStates[x];
                    //remember the word for the end-of-word decision
                    _asDoubleCount = _stateIndex;
                    _asDoubleFirst = first;
                    _asDoubleBoth = both;
                    memcpy(_asDoubleKeys, KeyStates, _stateIndex * sizeof(Uint32));
                }
            }
        }

        if (!IS_KEY_D(data)) {
            if (hCode == vDoNothing) {
                checkGrammar(-1);
            } else {
                checkGrammar(0);
            }
        }
        
        if (hCode == vRestore) {
            insertKey(data, _isCaps);
            if (_stateIndex > 0 && vAutoSwitchRequest != 0) {
                if (_asDropN == AS_MAX_DROPS) { memmove(_asDropPos, _asDropPos + 1, sizeof(int) * (AS_MAX_DROPS - 1)); memmove(_asDropKey, _asDropKey + 1, sizeof(Uint32) * (AS_MAX_DROPS - 1)); _asDropN--; }
                _asDropPos[_asDropN] = _stateIndex - 1; _asDropKey[_asDropN] = KeyStates[_stateIndex - 1]; _asDropN++;
            }
            _stateIndex--;
        }
        
        //case [ ]
        if (hCode != vDoNothing && IS_BRACKET_KEY(data) && IS_BRACKET_KEY((Uint16)hData[0])) {
            if (_index - (hCode == vWillProcess ? hBPC : 0) > 0) {
                _index--;
                saveWord();
            }
            _index = 0;
            tempDisableKey = false;
            _stateIndex = 0;
            hExt = 3;
            _specialChar.push_back(data | (_isCaps ? CAPS_MASK : 0));
        }
    }
    
    //Debug
    //cout<<"index "<<(int)_index<< ", stateIndex "<<(int)_stateIndex<<", word "<<_typingStates.size()<<", long word "<<_longWordHelper.size()<< endl;
    //cout<<"backspace "<<(int)hBPC<<endl;
    //cout<<"new char "<<(int)hNCC<<endl<<endl;
}

bool vAutoSwitchFixDoubledKey(const Uint32* keys, const int& count) {
    int n = _asDoubleCount;
    _asDoubleCount = 0;
    if (!asEnabled() || n < 2 || _asDoubleFirst < 0 || count < 0 || n + count >= AS_KEY_LEN || n + count >= MAX_BUFF) return false;
    asInitLetters();
    Uint32 full[MAX_BUFF], single[MAX_BUFF];
    memcpy(full, _asDoubleKeys, n * sizeof(Uint32));
    for (int x = 0; x < count; x++) full[n + x] = keys[x];
    int total = n + count, m = 0;
    for (int x = 0; x < total; x++) if (x != _asDoubleFirst) single[m++] = full[x];
    char rawFull[AS_KEY_LEN], rawSingle[AS_KEY_LEN];
    if (!asRawWord(full, total, rawFull) || !asRawWord(single, m, rawSingle)) return false;
    bool enFull = asEnglishScore(rawFull) != AS_MISSING, enSingle = asEnglishScore(rawSingle) != AS_MISSING;
    const Uint32* shown = _asDoubleBoth ? full : single;
    int shownCount = _asDoubleBoth ? total : m;
    const Uint32* fixed;
    int fixedCount;
    if (_asDoubleBoth && !enFull && enSingle) { fixed = single; fixedCount = m; }            //"mixx" -> "mix"
    else if (!_asDoubleBoth && enFull && !enSingle) { fixed = full; fixedCount = total; }   //"terible" -> "terrible"
    else if (!_asDoubleBoth && !enSingle && !asFewCapitals(full, total)) { fixed = full; fixedCount = total; }   //ASUS, not "AUS"
    else return false;
    if (fixedCount < 1) return false;
    vAutoSwitchUndoCount = 0;   //Backspace right after puts back what was on screen
    for (int x = 0; x < shownCount; x++) vAutoSwitchUndoData[vAutoSwitchUndoCount++] = shown[x];
    hCode = vRestore;          //platform sends the new text, then the break key itself
    hBPC = (Byte)shownCount;
    hNCC = (Byte)fixedCount;
    hExt = 3;
    for (int x = 0; x < fixedCount; x++) hData[fixedCount - 1 - x] = fixed[x];
    return true;
}

// ---- Next-key probabilities for touch keyboards (fat-finger model, D25) ---------------------------
// The dictionaries are sorted, so the words starting with a prefix are one contiguous range; with a
// running sum of word weights, the mass of every next letter is two binary searches.

static double* asCumulative(const signed char* scores, int count) {
    double* cum = new double[count + 1];
    cum[0] = 0;
    for (int i = 0; i < count; i++) cum[i + 1] = cum[i] + pow(10.0, scores[i] / 10.0);
    return cum;
}

static int asLowerBound(const char* words, const unsigned int* offsets, int count, const char* key, int keyLen) {
    int lo = 0, hi = count;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (strcmp(words + offsets[mid], key) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/** Words of the list that start with prefix: [first, last). */
static void asPrefixRange(const char* words, const unsigned int* offsets, int count, const char* prefix, int len, int& first, int& last) {
    char hiKey[AS_KEY_LEN + 2];
    first = asLowerBound(words, offsets, count, prefix, len);
    memcpy(hiKey, prefix, len);
    hiKey[len] = '\x7f'; hiKey[len + 1] = 0;           //just after every word with this prefix
    last = asLowerBound(words, offsets, count, hiKey, len + 1);
}

void vAutoSwitchNextKeyMass(const int& vietnamese, const char* prefix, double out[27]) {
    static double* enCum = asCumulative(kAutoSwitchEnScores, kAutoSwitchEnCount);
    static double* vnCum = asCumulative(kAutoSwitchVnScores, kAutoSwitchVnCount);
    const char* words = vietnamese ? kAutoSwitchVnWords : kAutoSwitchEnWords;
    const unsigned int* offsets = vietnamese ? kAutoSwitchVnOffsets : kAutoSwitchEnOffsets;
    const signed char* scores = vietnamese ? kAutoSwitchVnScores : kAutoSwitchEnScores;
    int count = vietnamese ? kAutoSwitchVnCount : kAutoSwitchEnCount;
    double* cum = vietnamese ? vnCum : enCum;
    int len = (int)strlen(prefix);
    for (int i = 0; i < 27; i++) out[i] = 0;
    if (len >= AS_KEY_LEN - 2) return;
    char key[AS_KEY_LEN + 2];
    memcpy(key, prefix, len);
    for (int c = 0; c < 26; c++) {
        key[len] = 'a' + c; key[len + 1] = 0;
        int a, b;
        asPrefixRange(words, offsets, count, key, len + 1, a, b);
        out[c] = cum[b] - cum[a];
    }
    int s = asLookup(words, offsets, scores, count, prefix);   //the prefix is a whole word: the word can end here
    out[26] = s == AS_MISSING ? 0 : pow(10.0, s / 10.0);
}

// ---- Dictionary access for suggestions and auto-correct (D26) ------------------------------------

static Uint16 asKeyOfLetter(char c) {
    static const Uint16 k[26] = {KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,KEY_L,KEY_M,KEY_N,
                                 KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,KEY_W,KEY_X,KEY_Y,KEY_Z};
    return (c >= 'a' && c <= 'z') ? k[c - 'a'] : 0;
}

int vAutoSwitchWordScore(const int& vietnamese, const char* letters, Uint16* rendered, int* renderedLen) {
    int n = (int)strlen(letters);
    if (rendered && renderedLen) *renderedLen = 0;
    if (n == 0 || n >= AS_KEY_LEN - 1) return AS_MISSING;
    asInitLetters();
    if (!vietnamese) {
        int s = asEnglishScore(letters);
        if (s != AS_MISSING && rendered && renderedLen) { for (int i = 0; i < n; i++) rendered[i] = letters[i]; *renderedLen = n; }
        return s;
    }
    Uint32 keys[AS_KEY_LEN], result[MAX_BUFF];
    for (int i = 0; i < n; i++) { keys[i] = asKeyOfLetter(letters[i]); if (!keys[i] && letters[i] != 'a') return AS_MISSING; }
    int count = 0;
    bool changed = false;
    int vn = asReplay(keys, n, result, count, &changed);
    if (vn != AS_MISSING) {
        if (rendered && renderedLen) {
            for (int i = 0; i < count; i++) rendered[i] = (result[i] & CHAR_CODE_MASK) ? (Uint16)(result[i] & 0xFFFF) : keyCodeToCharacter(result[i]);
            *renderedLen = count;
        }
        return vn;
    }
    if (!changed) {                      //plain syllables that look the same in Telex: cho, anh, ban
        vn = asVietnameseScore(letters);
        if (vn != AS_MISSING && rendered && renderedLen) { for (int i = 0; i < n; i++) rendered[i] = letters[i]; *renderedLen = n; }
    }
    return vn;
}

int vAutoSwitchCompletions(const int& vietnamese, const char* prefix, char out[][32], int* scores, const int& maxOut) {
    if (!prefix || !out || !scores || maxOut <= 0) return 0;
    const char* words = vietnamese ? kAutoSwitchVnWords : kAutoSwitchEnWords;
    const unsigned int* offsets = vietnamese ? kAutoSwitchVnOffsets : kAutoSwitchEnOffsets;
    const signed char* sc = vietnamese ? kAutoSwitchVnScores : kAutoSwitchEnScores;
    int count = vietnamese ? kAutoSwitchVnCount : kAutoSwitchEnCount;
    int len = (int)strlen(prefix);
    if (len == 0 || len >= 30) return 0;
    int a, b;
    asPrefixRange(words, offsets, count, prefix, len, a, b);
    int n = 0;
    for (int i = a; i < b; i++) {
        int s = sc[i];
        if (n == maxOut && s <= scores[n - 1]) continue;
        const char* w = words + offsets[i];
        if (strlen(w) >= 32) continue;
        int pos = n < maxOut ? n++ : n - 1;
        while (pos > 0 && scores[pos - 1] < s) { strcpy(out[pos], out[pos - 1]); scores[pos] = scores[pos - 1]; pos--; }
        strcpy(out[pos], w); scores[pos] = s;
    }
    return n;
}

bool vAutoSwitchIsVietnameseSpelling(const char* letters) {
    if (!letters) return false;
    int n = (int)strlen(letters);
    if (n == 0 || n >= MAX_BUFF - 1) return false;
    Uint32 keys[MAX_BUFF];
    for (int i = 0; i < n; i++) { keys[i] = asKeyOfLetter(letters[i]); if (!keys[i] && letters[i] != 'a') return false; }
    AsEngineState saved;
    asSaveState(saved);
    _spaceCount = 0;
    vAutoSwitchByTyping = 0;
    vCheckSpelling = 1;
    vRotationalTone = 0;
    startNewSession();
    for (int x = 0; x < n; x++) vKeyHandleEvent(vKeyEvent::Keyboard, vKeyEventState::KeyDown, (Uint16)keys[x], 0, false);
    checkSpelling(true);
    bool valid = !tempDisableKey;
    asRestoreState(saved);
    return valid;
}

/** Early Switch (D28): which language a finished word can only be. 1 = only Vietnamese (a Vietnamese reading,
 *  no English word: "tôi", "khong"), 0 = only English (an English word with no Vietnamese reading: "hello",
 *  "makes"), -1 = both or neither ("an", "con", names). */
int vAutoSwitchWordSide(const Uint32* keys, const int& count) {
    if (!asEnabled() || count < 2 || count >= MAX_BUFF - 1) return -1;
    asInitLetters();
    char raw[AS_KEY_LEN];
    if (!asRawWord(keys, count, raw) || !asFewCapitals(keys, count)) return -1;
    int en = asEnglishScore(raw);
    Uint32 result[MAX_BUFF];
    int resultCount;
    int vn = asReplay(keys, count, result, resultCount);
    if (en != AS_MISSING && vn == AS_MISSING) return 0;
    if (vn != AS_MISSING && en == AS_MISSING) return 1;
    return -1;
}

