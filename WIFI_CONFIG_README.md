# WiFi Configuration - Runtime Setup

## Overview

The Altair 8800 emulator now supports runtime WiFi configuration through flash storage, eliminating the need to hardcode credentials at build time. This makes it easy to move the device between different networks without recompiling.

## How It Works

### Flash Storage
- WiFi credentials (SSID and password) are stored in the **last 4KB sector** of flash memory
- Location: offset `(4MB - 4KB)` for Pico 2 W
- Data structure includes:
  - Magic number (`0x57494649` = "WIFI") for validation
  - SSID (max 32 characters)
  - Password (max 63 characters)
  - CRC32 checksum for integrity verification
- Flash survives reboots and power cycles
- ~100,000 write cycles per sector

### Security Note
⚠️ **Flash storage is NOT encrypted** - credentials are stored in plain text. Anyone with physical access to the device can read the flash memory. This is suitable for home/development use but not for high-security environments.

## Usage

### Setup and Configuration

1. **Build and flash** the firmware to your Pico 2 W
2. **Connect** via USB serial console (115200 baud)
3. **Wait** for the startup prompt (appears after 3 seconds):

```
========================================
  WiFi Configuration
========================================

Press 'Y' within 15 seconds to enter WiFi credentials...
```

4. **Press 'Y'** to enter configuration mode, or wait for timeout to use existing credentials
5. **Enter your SSID** when prompted:
```
Enter WiFi SSID (max 32 characters): YourNetworkName
```

6. **Enter your password** (displayed as asterisks):
```
Enter WiFi password (max 63 characters): ************
```

7. Credentials are **saved to flash** and WiFi connects automatically

### Using Stored Credentials

On subsequent boots:
- You can **press 'Y'** within 15 seconds to update/change credentials
- Or **wait for timeout** to use existing stored credentials
- The prompt appears on every boot, giving you flexibility to reconfigure anytime

### Changing WiFi Networks

To switch networks or update credentials:

1. At startup, simply **press 'Y'** when you see the configuration prompt
2. Enter the new SSID and password
3. New credentials overwrite the old ones in flash
4. WiFi connects to the new network

### No Hardcoded Credentials

WiFi credentials are **no longer hardcoded** at build time:
- All credentials must be entered via the serial console
- This improves security by not storing passwords in source code
- Makes it easy to share firmware builds without exposing network credentials

## API Reference

### `wifi_config.h` Functions

```c
// Initialize WiFi config system (call once at startup)
void wifi_config_init(void);

// Check if valid credentials exist in flash
bool wifi_config_exists(void);

// Load credentials from flash
bool wifi_config_load(char *ssid, size_t ssid_len, 
                      char *password, size_t password_len);

// Save credentials to flash
bool wifi_config_save(const char *ssid, const char *password);

// Clear credentials from flash
bool wifi_config_clear(void);

// Interactive prompt (with timeout in milliseconds)
bool wifi_config_prompt_and_save(uint32_t timeout_ms);
```

## Implementation Details

### Flash Write Safety
- Flash writes **disable interrupts** temporarily to prevent corruption
- Uses `flash_range_erase()` to erase 4KB sector
- Uses `flash_range_program()` to write data
- Automatic validation via CRC32 checksum

### Memory Layout
```
Flash Memory (4MB total for Pico 2 W)
├─ 0x00000000 - Program code and data
├─ ...
└─ 0x003FF000 - WiFi config (last 4KB sector)
   ├─ Magic (4 bytes): 0x57494649
   ├─ SSID (33 bytes): null-terminated string
   ├─ Password (64 bytes): null-terminated string
   └─ Checksum (4 bytes): CRC32
```

### Integration Points

1. **`main.c`**: Startup logic checks for credentials and prompts user
2. **`websocket_console.c`**: Loads credentials from flash before WiFi connect
3. **`wifi_config.c`**: Core flash storage implementation
4. **`CMakeLists.txt`**: Links `hardware_flash` and `hardware_sync` libraries

## Troubleshooting

### "Flash write failed"
- Check that flash offset doesn't overlap with program code
- Verify sufficient free flash space

### "WiFi connect failed"
- Verify SSID is correct (case-sensitive)
- Check password is correct
- Ensure WiFi network is 2.4GHz (CYW43 doesn't support 5GHz)
- Check signal strength

### "Timeout - skipping WiFi configuration"
- User didn't press 'Y' within 15 seconds
- Serial console connection delay
- Try resetting and responding faster

### Clear Credentials Programmatically
If you need to clear credentials without the console:
```c
wifi_config_clear();  // Erases the flash sector
```

## Example Sessions

### First Boot (No Stored Credentials)

```
*** USB Serial Active ***
========================================
  Altair 8800 Emulator - Pico 2 W
========================================

No WiFi credentials found in flash storage.

========================================
  WiFi Configuration
========================================

Press 'Y' within 15 seconds to enter WiFi credentials...
Y

Enter WiFi SSID (max 32 characters): MyHomeNetwork
Enter WiFi password (max 63 characters): ************
Saving credentials (SSID: MyHomeNetwork)...
Writing WiFi credentials to flash...
WiFi credentials saved successfully!

Launched network task on core 1
[Core1] Initializing CYW43...
[Core1] Using stored credentials from flash
[Core1] Connecting to Wi-Fi SSID 'MyHomeNetwork'...
[Core1] Wi-Fi connected. IP: 192.168.1.100
Wi-Fi connected. IP: 192.168.1.100
```

### Subsequent Boot (Using Stored Credentials)

```
*** USB Serial Active ***
========================================
  Altair 8800 Emulator - Pico 2 W
========================================

WiFi credentials found in flash storage.

========================================
  WiFi Configuration
========================================

Press 'Y' within 15 seconds to enter WiFi credentials...
[timeout - no key pressed]
Timeout - skipping WiFi configuration

Using stored WiFi credentials
Launched network task on core 1
[Core1] Connecting to Wi-Fi SSID 'MyHomeNetwork'...
[Core1] Wi-Fi connected. IP: 192.168.1.100
```

### Changing Networks

```
WiFi credentials found in flash storage.

========================================
  WiFi Configuration
========================================

Press 'Y' within 15 seconds to enter WiFi credentials...
Y

Enter WiFi SSID (max 32 characters): OfficeNetwork
Enter WiFi password (max 63 characters): ************
Saving credentials (SSID: OfficeNetwork)...
Writing WiFi credentials to flash...
WiFi credentials saved successfully!
```

## Build Configuration

No WiFi credentials are stored in the build configuration:

```cmake
# CMakeLists.txt - credentials now empty by default
set(WIFI_SSID "" CACHE STRING "Default Wi-Fi SSID (empty - configure at runtime)")
set(WIFI_PASSWORD "" CACHE STRING "Default Wi-Fi password (empty - configure at runtime)")
```

All credentials are entered at runtime via the serial console.

## Future Enhancements

Possible improvements:
- Encryption of stored credentials (requires key management)
- Support for multiple network profiles
- Web-based configuration interface
- WPS (WiFi Protected Setup) support
- Captive portal for initial setup
