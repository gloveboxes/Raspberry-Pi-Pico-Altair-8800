#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum lengths for WiFi credentials
#define WIFI_CONFIG_SSID_MAX_LEN 32
#define WIFI_CONFIG_PASSWORD_MAX_LEN 63

// WiFi configuration structure stored in flash
typedef struct
{
    uint32_t magic;                                  // Magic number for validation (0x57494649 = "WIFI")
    char ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];         // SSID (null-terminated)
    char password[WIFI_CONFIG_PASSWORD_MAX_LEN + 1]; // Password (null-terminated)
    uint32_t checksum;                               // CRC32 checksum for validation
} wifi_config_t;

// Initialize the WiFi configuration system
void wifi_config_init(void);

// Check if valid WiFi credentials are stored in flash
bool wifi_config_exists(void);

// Load WiFi credentials from flash
// Returns true if valid credentials were loaded
bool wifi_config_load(char* ssid, size_t ssid_len, char* password, size_t password_len);

// Save WiFi credentials to flash
// Returns true if successfully saved
bool wifi_config_save(const char* ssid, const char* password);

// Clear WiFi credentials from flash
// Returns true if successfully cleared
bool wifi_config_clear(void);

// Prompt user for WiFi credentials via serial console
// Waits up to timeout_ms for 'Y' input, then prompts for SSID and password
// Returns true if credentials were entered and saved
bool wifi_config_prompt_and_save(uint32_t timeout_ms);
