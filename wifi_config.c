#include "wifi_config.h"

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include <stdio.h>
#include <string.h>

// Flash storage offset (last 4KB sector of 4MB flash)
// Pico 2 W has 4MB flash, so we use offset at 4MB - 4KB
#define WIFI_CONFIG_FLASH_OFFSET (4 * 1024 * 1024 - FLASH_SECTOR_SIZE)
#define WIFI_CONFIG_MAGIC 0x57494649  // "WIFI" in hex

// Simple CRC32 implementation
static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
        {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

// Calculate checksum for config (excludes the checksum field itself)
static uint32_t wifi_config_calculate_checksum(const wifi_config_t *config)
{
    // Calculate CRC over everything except the checksum field
    size_t data_size = offsetof(wifi_config_t, checksum);
    return crc32((const uint8_t *)config, data_size);
}

void wifi_config_init(void)
{
    // Nothing to initialize - flash is always available
}

bool wifi_config_exists(void)
{
    // Read the config from flash
    const wifi_config_t *flash_config = (const wifi_config_t *)(XIP_BASE + WIFI_CONFIG_FLASH_OFFSET);

    // Check magic number
    if (flash_config->magic != WIFI_CONFIG_MAGIC)
    {
        return false;
    }

    // Verify checksum
    uint32_t calculated = wifi_config_calculate_checksum(flash_config);
    if (calculated != flash_config->checksum)
    {
        return false;
    }

    // Check that SSID is not empty
    if (flash_config->ssid[0] == '\0' || flash_config->ssid[0] == 0xFF)
    {
        return false;
    }

    return true;
}

bool wifi_config_load(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    if (!ssid || ssid_len == 0 || !password || password_len == 0)
    {
        return false;
    }

    if (!wifi_config_exists())
    {
        return false;
    }

    const wifi_config_t *flash_config = (const wifi_config_t *)(XIP_BASE + WIFI_CONFIG_FLASH_OFFSET);

    // Copy credentials
    strncpy(ssid, flash_config->ssid, ssid_len - 1);
    ssid[ssid_len - 1] = '\0';

    strncpy(password, flash_config->password, password_len - 1);
    password[password_len - 1] = '\0';

    return true;
}

bool wifi_config_save(const char *ssid, const char *password)
{
    if (!ssid || !password)
    {
        return false;
    }

    // Validate lengths
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);

    if (ssid_len == 0 || ssid_len > WIFI_CONFIG_SSID_MAX_LEN)
    {
        printf("Error: SSID length must be 1-%d characters\n", WIFI_CONFIG_SSID_MAX_LEN);
        return false;
    }

    if (password_len > WIFI_CONFIG_PASSWORD_MAX_LEN)
    {
        printf("Error: Password length must be 0-%d characters\n", WIFI_CONFIG_PASSWORD_MAX_LEN);
        return false;
    }

    // Prepare config structure
    wifi_config_t config = {0};
    config.magic = WIFI_CONFIG_MAGIC;
    strncpy(config.ssid, ssid, WIFI_CONFIG_SSID_MAX_LEN);
    config.ssid[WIFI_CONFIG_SSID_MAX_LEN] = '\0';
    strncpy(config.password, password, WIFI_CONFIG_PASSWORD_MAX_LEN);
    config.password[WIFI_CONFIG_PASSWORD_MAX_LEN] = '\0';
    config.checksum = wifi_config_calculate_checksum(&config);

    // Erase and write flash sector
    // CRITICAL: Interrupts must be disabled during flash operations
    printf("Writing WiFi credentials to flash...\n");
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(WIFI_CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(WIFI_CONFIG_FLASH_OFFSET, (const uint8_t *)&config, sizeof(wifi_config_t));
    restore_interrupts(ints);

    printf("WiFi credentials saved successfully\n");
    return true;
}

bool wifi_config_clear(void)
{
    printf("Clearing WiFi credentials from flash...\n");
    
    // Erase the sector
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(WIFI_CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    printf("WiFi credentials cleared\n");
    return true;
}

bool wifi_config_prompt_and_save(uint32_t timeout_ms)
{
    printf("\n");
    printf("========================================\n");
    printf("  WiFi Configuration\n");
    printf("========================================\n");
    printf("\n");
    printf("Press 'Y' within %lu seconds to enter WiFi credentials...\n", (unsigned long)(timeout_ms / 1000));
    printf("Press ENTER to skip and continue...\n");

    // Wait for 'Y' input with timeout
    absolute_time_t start_time = get_absolute_time();
    absolute_time_t last_dot_time = start_time;
    bool configure = false;

    while (absolute_time_diff_us(start_time, get_absolute_time()) < (int64_t)(timeout_ms * 1000))
    {
        // Print a dot every second
        absolute_time_t current_time = get_absolute_time();
        if (absolute_time_diff_us(last_dot_time, current_time) >= 1000000)
        {
            printf(".");
            fflush(stdout);
            last_dot_time = current_time;
        }

        int c = getchar_timeout_us(10000);  // Check every 10ms
        if (c != PICO_ERROR_TIMEOUT)
        {
            if (c == 'Y' || c == 'y')
            {
                configure = true;
                printf("\nY\n");
                break;
            }
            else if (c == '\r' || c == '\n')
            {
                printf("\nSkipping WiFi configuration\n\n");
                return false;
            }
        }
        tight_loop_contents();
    }

    if (!configure)
    {
        printf("\nTimeout - skipping WiFi configuration\n\n");
        return false;
    }

    // Prompt for SSID
    printf("\nEnter WiFi SSID (max %d characters): ", WIFI_CONFIG_SSID_MAX_LEN);
    char ssid[WIFI_CONFIG_SSID_MAX_LEN + 1] = {0};
    size_t ssid_idx = 0;

    while (ssid_idx < WIFI_CONFIG_SSID_MAX_LEN)
    {
        int c = getchar_timeout_us(60 * 1000 * 1000);  // 60 second timeout
        if (c == PICO_ERROR_TIMEOUT)
        {
            printf("\nTimeout - WiFi configuration cancelled\n\n");
            return false;
        }

        if (c == '\r' || c == '\n')
        {
            printf("\n");
            break;
        }
        else if (c == 0x7F || c == 0x08)  // Backspace/Delete
        {
            if (ssid_idx > 0)
            {
                ssid_idx--;
                ssid[ssid_idx] = '\0';
                printf("\b \b");  // Erase character from display
            }
        }
        else if (c >= 0x20 && c < 0x7F)  // Printable ASCII
        {
            ssid[ssid_idx++] = (char)c;
            putchar(c);
        }
    }

    if (ssid_idx == 0)
    {
        printf("Error: SSID cannot be empty\n\n");
        return false;
    }

    // Prompt for password
    printf("Enter WiFi password (max %d characters): ", WIFI_CONFIG_PASSWORD_MAX_LEN);
    char password[WIFI_CONFIG_PASSWORD_MAX_LEN + 1] = {0};
    size_t password_idx = 0;

    while (password_idx < WIFI_CONFIG_PASSWORD_MAX_LEN)
    {
        int c = getchar_timeout_us(60 * 1000 * 1000);  // 60 second timeout
        if (c == PICO_ERROR_TIMEOUT)
        {
            printf("\nTimeout - WiFi configuration cancelled\n\n");
            return false;
        }

        if (c == '\r' || c == '\n')
        {
            printf("\n");
            break;
        }
        else if (c == 0x7F || c == 0x08)  // Backspace/Delete
        {
            if (password_idx > 0)
            {
                password_idx--;
                password[password_idx] = '\0';
                printf("\b \b");  // Erase character from display
            }
        }
        else if (c >= 0x20 && c < 0x7F)  // Printable ASCII
        {
            password[password_idx++] = (char)c;
            putchar('*');  // Echo asterisk for password
        }
    }

    printf("\n");
    printf("Saving credentials (SSID: %s)...\n", ssid);

    // Save to flash
    if (wifi_config_save(ssid, password))
    {
        printf("WiFi configuration saved successfully!\n\n");
        return true;
    }
    else
    {
        printf("Failed to save WiFi configuration\n\n");
        return false;
    }
}
