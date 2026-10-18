// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Arduino.h>

// switch the uart from shell to binary bridge mode
void bridgeRun(uint32_t baud, const String &ssid, const String &pass);
