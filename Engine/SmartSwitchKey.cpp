//
//  SmartSwitchKey.cpp
//  Vikey (based on OpenKey)
//
//  Created by Tuyen on 8/13/19.
//  Copyright © 2019 Tuyen Mai. All rights reserved.
//

#include "SmartSwitchKey.h"
#include <map>
#include <iostream>
#include <memory.h>

//main data, i use `map` because it has O(Log(n))
static map<string, Int8> _smartSwitchKeyData;
static string _cacheKey = ""; //use cache for faster
static Int8 _cacheData = 0; //use cache for faster

void initSmartSwitchKey(const Byte* pData, const int& size) {
    _smartSwitchKeyData.clear();
    _cacheKey.clear();
    _cacheData = 0;
    if (pData == NULL || size < 2) return;
    Uint16 count = 0;
    Uint32 cursor = 0;
    memcpy(&count, pData + cursor, 2);
    cursor += 2;

    Uint8 bundleIdSize = 0;
    Uint8 value = 0;
    for (int i = 0; i < count; i++) {
        if (cursor + 1 > (Uint32)size) break;
        bundleIdSize = pData[cursor++];
        if (cursor + bundleIdSize + 1 > (Uint32)size) break;
        string bundleId((const char*)pData + cursor, bundleIdSize);
        cursor += bundleIdSize;
        value = pData[cursor++];
        _smartSwitchKeyData[bundleId] = value;
    }
}

void sanitizeSmartSwitchKeyEntries(const std::vector<std::string>& browserExeList) {
    for (const auto& exe : browserExeList) {
        _smartSwitchKeyData.erase(exe);
    }
    for (auto it = _smartSwitchKeyData.begin(); it != _smartSwitchKeyData.end(); ) {
        if (it->first.find(' ') != string::npos || it->first.find('\t') != string::npos) {
            it = _smartSwitchKeyData.erase(it);
        } else {
            ++it;
        }
    }
    _cacheKey.clear();
    _cacheData = 0;
}

void getSmartSwitchKeySaveData(vector<Byte>& outData) {
    outData.clear();
    vector<std::pair<string, Int8>> validEntries;
    for (std::map<string, Int8>::iterator it = _smartSwitchKeyData.begin(); it != _smartSwitchKeyData.end(); ++it) {
        if (it->first.length() > 0 && it->first.length() <= 255) {
            validEntries.push_back(*it);
        }
    }
    Uint16 count = (Uint16)validEntries.size();
    outData.push_back((Byte)(count & 0xFF));
    outData.push_back((Byte)((count >> 8) & 0xFF));
    
    for (size_t i = 0; i < validEntries.size(); i++) {
        outData.push_back((Byte)validEntries[i].first.length());
        for (size_t j = 0; j < validEntries[i].first.length(); j++) {
            outData.push_back(validEntries[i].first[j]);
        }
        outData.push_back(validEntries[i].second);
    }
}

int getAppInputMethodStatus(const string& bundleId, const int& currentInputMethod) {
    if (_cacheKey.compare(bundleId) == 0) {
        return _cacheData;
    }
    if (_smartSwitchKeyData.find(bundleId) != _smartSwitchKeyData.end()) {
        _cacheKey = bundleId;
        _cacheData = _smartSwitchKeyData[bundleId];
        return _cacheData;
    }
    _cacheKey = bundleId;
    _cacheData = currentInputMethod;
    _smartSwitchKeyData[bundleId] = _cacheData;
    return -1;
}

void setAppInputMethodStatus(const string& bundleId, const int& language) {
    _smartSwitchKeyData[bundleId] = language;
    _cacheKey = bundleId;
    _cacheData = language;
}
