#pragma once

#include <stdbool.h>
#include <stddef.h>

bool wifi_init(void);
bool wifi_is_ready(void);
bool wifi_is_connected(void);
const char* wifi_get_ssid(void);
bool wifi_get_ip(char* buffer, size_t length);
