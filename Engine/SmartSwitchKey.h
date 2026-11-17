//
//  SmartSwitchKey.h
//  Vikey (based on OpenKey)
//
//  Created by Tuyen on 8/13/19.
//  Copyright © 2019 Tuyen Mai. All rights reserved.
//

#ifndef SmartSwitchKey_h
#define SmartSwitchKey_h

#include "DataType.h"
#include <string>
#include <vector>

using namespace std;

void initSmartSwitchKey(const Byte* pData, const int& size);
void sanitizeSmartSwitchKeyEntries(const std::vector<std::string>& browserExeList);

/**
 * convert all data to save on disk
 */
void getSmartSwitchKeySaveData(vector<Byte>& outData);

/**
 * find and get language input method, if don't has set @currentInputMethod value for this app
 * return:
 * -1: don't have this bundleId
 * 0: English
 * 1: Vietnamese
 */
int getAppInputMethodStatus(const string& bundleId, const int& currentInputMethod);

/**
 * Set default language for this @bundleId
 */
void setAppInputMethodStatus(const string& bundleId, const int& language);

#endif /* SmartSwitchKey_h */
