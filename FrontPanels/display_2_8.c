/* Pico Display 2.8" LCD Support for Altair 8800 Emulator
 * Displays Altair front panel LEDs using custom async ST7789 driver
 */

#include "display_2_8.h"

#ifdef DISPLAY_2_8_SUPPORT

#include "st7789_async.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>

void display_2_8_init(void)
{
    // Initialize async ST7789 driver
    if (!st7789_async_init())
    {
        printf("Failed to initialize async ST7789 driver\n");
    }
}

void display_2_8_update(const char* ssid, const char* ip)
{
    // Not implemented for async driver
}

void display_2_8_set_cpu_led(bool cpu_running)
{
    // RGB LED not implemented in async driver
}

void display_2_8_init_front_panel(void)
{
    // Just clear to black - drawing happens in show_front_panel
    st7789_async_clear(rgb332(0, 0, 0));
    st7789_async_update();
}

void display_2_8_show_front_panel(uint16_t address, uint8_t data, uint16_t status)
{
    // Clear entire screen to black every frame (fast with async DMA!)
    st7789_async_clear(rgb332(0, 0, 0));

    const int LED_SIZE = 15;
    const int LED_SPACING_STATUS = 32;
    const int LED_SPACING_ADDRESS = 20;
    const int LED_SPACING_DATA = 20;

    color_t LED_ON = rgb332(255, 0, 0);   // Bright red
    color_t LED_OFF = rgb332(40, 0, 0);   // Dark red
    color_t TEXT = rgb332(255, 255, 255); // White

    // STATUS LEDs (10 LEDs) - Row 1
    const char* status_labels[] = {"INT ", "WO  ", "STCK", "HLTA", "OUT ", "M1  ", "INP ", "MEMR", "PROT", "INTE"};
    int x_status = 10;
    int y_status = 35;

    // Section label with text
    st7789_async_text("STATUS", 5, y_status - 15, rgb332(255, 255, 255));
    st7789_async_fill_rect(0, y_status - 5, 320, 3, rgb332(255, 255, 255));

    for (int i = 0; i < 10; i++)
    {
        int bit = i; // Bit 9 on left, Bit 0 on right
        bool led_state = (status >> bit) & 1;
        st7789_async_fill_rect(x_status, y_status, LED_SIZE, LED_SIZE, led_state ? LED_ON : LED_OFF);

        // Add label below LED (status_labels[9] is INTE/Bit 9, status_labels[0] is INT/Bit 0)
        st7789_async_text(status_labels[bit], x_status - 8, y_status + LED_SIZE + 2, rgb332(100, 100, 100));

        x_status += LED_SPACING_STATUS;
    }

    // ADDRESS LEDs (16 LEDs) - Row 2
    int x_addr = 2;
    int y_addr = 100;

    // Section label with text
    st7789_async_text("ADDRESS", 5, y_addr - 15, rgb332(255, 255, 255));
    st7789_async_fill_rect(0, y_addr - 5, 320, 3, rgb332(255, 255, 255));

    for (int i = 15; i >= 0; i--)
    {
        int bit = 15 - i; // Reverse LED order
        bool led_state = (address >> bit) & 1;
        st7789_async_fill_rect(x_addr, y_addr, LED_SIZE, LED_SIZE, led_state ? LED_ON : LED_OFF);

        // Add bit label below LED (A15-A0)
        // Note: LEDs go from A15 (left) to A0 (right), but due to RTL text we need to flip
        char label[4];
        int label_bit = 15 - i; // Flip the bit number for label
        if (label_bit >= 10)
        {
            label[0] = 'A';
            label[1] = '1';
            label[2] = '0' + (label_bit - 10);
            label[3] = '\0';
        }
        else
        {
            label[0] = 'A';
            label[1] = '0' + label_bit;
            label[2] = ' ';
            label[3] = '\0';
        }
        // Position label directly at LED position (display is rotated, text renders RTL)
        st7789_async_text(label, x_addr - 2, y_addr + LED_SIZE + 2, rgb332(100, 100, 100));

        x_addr += LED_SPACING_ADDRESS;
    }

    // DATA LEDs (8 LEDs) - Row 3
    int x_data = 2;
    int y_data = 170;

    // Section label with text
    st7789_async_text("DATA", 5, y_data - 15, rgb332(255, 255, 255));
    st7789_async_fill_rect(0, y_data - 5, 320, 3, rgb332(255, 255, 255));

    for (int i = 7; i >= 0; i--)
    {
        int bit = 7 - i; // Reverse LED order
        bool led_state = (data >> bit) & 1;
        st7789_async_fill_rect(x_data, y_data, LED_SIZE, LED_SIZE, led_state ? LED_ON : LED_OFF);

        // Add bit label below LED (D7-D0)
        // Note: LEDs go from D7 (left) to D0 (right), but due to RTL text we need to flip
        char label[3];
        int label_bit = 7 - i; // Flip the bit number for label
        label[0] = 'D';
        label[1] = '0' + label_bit;
        label[2] = '\0';
        // Since text renders RTL, add text width (2 chars * 6 = 12 pixels)
        st7789_async_text(label, x_data + 4, y_data + LED_SIZE + 2, rgb332(100, 100, 100));

        x_data += LED_SPACING_DATA;
    }

    // Bottom status line: WiFi IP and Altair logo
#ifdef CYW43_WL_GPIO_LED_PIN // Only show WiFi IP on WiFi-enabled boards
    const char* ip = wifi_get_ip_address();
    if (ip != NULL)
    {
        char ip_text[32];
        snprintf(ip_text, sizeof(ip_text), "WiFi: %s", ip);
        st7789_async_text(ip_text, 5, 220, rgb332(255, 255, 255));
    }
#endif

    // Altair 8800 logo on the right side (made bolder by drawing multiple times)
    st7789_async_text("ALTAIR 8800", 250, 20, rgb332(255, 255, 255));

    // Non-blocking update! Returns immediately
    st7789_async_update();
}

void display_2_8_get_stats(uint64_t* skipped_updates)
{
    uint64_t updates, skipped;
    st7789_async_get_stats(&updates, &skipped);
    if (skipped_updates)
    {
        *skipped_updates = skipped;
    }
}

#endif // DISPLAY_2_8_SUPPORT
