#include "wifi.h"

#include "pico/cyw43_arch.h"
#include "pico/error.h"
#include "pico/stdlib.h"

#include "cyw43.h"

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include <stdio.h>
#include <string.h>

#ifndef PICO_DEFAULT_WIFI_SSID
#define PICO_DEFAULT_WIFI_SSID ""
#endif

#ifndef PICO_DEFAULT_WIFI_PASSWORD
#define PICO_DEFAULT_WIFI_PASSWORD ""
#endif

#ifndef WIFI_SSID
#define WIFI_SSID PICO_DEFAULT_WIFI_SSID
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD PICO_DEFAULT_WIFI_PASSWORD
#endif

#ifndef WIFI_AUTH
#define WIFI_AUTH CYW43_AUTH_WPA2_AES_PSK
#endif

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WIFI_CONNECT_RETRIES 3

static bool wifi_hw_ready = false;
static bool wifi_connected = false;

static void wifi_print_ip(void)
{
    char buffer[32];
    if (wifi_get_ip(buffer, sizeof(buffer)))
    {
        printf("Wi-Fi connected. IP address: %s\n", buffer);
    }
}

static const char* wifi_error_to_string(int err)
{
    switch (err)
    {
        case PICO_OK:
#ifdef PICO_ERROR_NONE
        case PICO_ERROR_NONE:
#endif
            return "OK";
        case PICO_ERROR_GENERIC:
            return "generic failure";
        case PICO_ERROR_TIMEOUT:
            return "timeout";
        case PICO_ERROR_NO_DATA:
            return "no data";
        case PICO_ERROR_NOT_PERMITTED:
            return "not permitted";
        case PICO_ERROR_INVALID_ARG:
            return "invalid argument";
        case PICO_ERROR_IO:
            return "i/o error";
        case PICO_ERROR_BADAUTH:
            return "bad credentials";
        case PICO_ERROR_CONNECT_FAILED:
            return "connection failed";
        case PICO_ERROR_INSUFFICIENT_RESOURCES:
            return "insufficient resources";
        case PICO_ERROR_INVALID_ADDRESS:
            return "invalid address";
        case PICO_ERROR_BAD_ALIGNMENT:
            return "bad alignment";
        case PICO_ERROR_INVALID_STATE:
            return "invalid state";
        case PICO_ERROR_BUFFER_TOO_SMALL:
            return "buffer too small";
        case PICO_ERROR_PRECONDITION_NOT_MET:
            return "precondition not met";
        case PICO_ERROR_MODIFIED_DATA:
            return "modified data";
        case PICO_ERROR_INVALID_DATA:
            return "invalid data";
        case PICO_ERROR_NOT_FOUND:
            return "not found";
        case PICO_ERROR_UNSUPPORTED_MODIFICATION:
            return "unsupported modification";
        case PICO_ERROR_LOCK_REQUIRED:
            return "lock required";
        case PICO_ERROR_VERSION_MISMATCH:
            return "version mismatch";
        case PICO_ERROR_RESOURCE_IN_USE:
            return "resource in use";
        default:
            return "unknown";
    }
}

static const char* wifi_link_status_to_string(int status)
{
    switch (status)
    {
        case CYW43_LINK_DOWN:
            return "link down";
        case CYW43_LINK_JOIN:
            return "joined (no IP)";
        case CYW43_LINK_NOIP:
            return "no IP yet";
        case CYW43_LINK_UP:
            return "link up";
        case CYW43_LINK_FAIL:
            return "link failure";
        case CYW43_LINK_NONET:
            return "network not found";
        case CYW43_LINK_BADAUTH:
            return "auth failure";
        default:
            return "status unknown";
    }
}

static void wifi_log_failure_details(int attempt, int err)
{
    printf("Wi-Fi attempt %d failed (error %d: %s)\n", attempt, err, wifi_error_to_string(err));
    printf("    SSID length: %u, password length: %u, auth: 0x%08x\n", (unsigned)strlen(WIFI_SSID),
           (unsigned)strlen(WIFI_PASSWORD), (unsigned)WIFI_AUTH);

    int link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
    printf("    Last link status: %d (%s)\n", link, wifi_link_status_to_string(link));

    int32_t rssi = 0;
    int rssi_err = cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    if (rssi_err == PICO_OK)
    {
        printf("    RSSI: %ld dBm\n", (long)rssi);
    }
    else
    {
        printf("    RSSI unavailable (error %d: %s)\n", rssi_err, wifi_error_to_string(rssi_err));
    }
}

bool wifi_init(void)
{
    if (!wifi_hw_ready)
    {
        if (cyw43_arch_init())
        {
            printf("cyw43 initialization failed\n");
            return false;
        }

        wifi_hw_ready = true;
        cyw43_arch_enable_sta_mode();
    }

    if (wifi_connected)
    {
        return true;
    }

    printf("Connecting to Wi-Fi SSID '%s'...\n", WIFI_SSID);

    for (int attempt = 1; attempt <= WIFI_CONNECT_RETRIES; ++attempt)
    {
        int err = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, WIFI_AUTH, WIFI_CONNECT_TIMEOUT_MS);

        if (err == 0)
        {
            wifi_connected = true;
            wifi_print_ip();
            return true;
        }

        wifi_log_failure_details(attempt, err);
        sleep_ms(2000);
    }

    printf("Unable to connect to Wi-Fi. Terminal will remain on USB only.\n");
    return false;
}

bool wifi_is_ready(void)
{
    return wifi_hw_ready;
}

bool wifi_is_connected(void)
{
    return wifi_connected;
}

const char* wifi_get_ssid(void)
{
    return WIFI_SSID;
}

bool wifi_get_ip(char* buffer, size_t length)
{
    if (!wifi_hw_ready || !buffer || length == 0)
    {
        return false;
    }

    bool ok = false;

    cyw43_arch_lwip_begin();
    struct netif* netif = &cyw43_state.netif[CYW43_ITF_STA];
    if (netif && netif_is_up(netif))
    {
        const ip4_addr_t* addr = netif_ip4_addr(netif);
        if (addr)
        {
            ok = ip4addr_ntoa_r(addr, buffer, length) != NULL;
        }
    }
    cyw43_arch_lwip_end();

    return ok;
}
