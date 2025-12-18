#include "wifi.h"

#include "pico/cyw43_arch.h"
#include "pico/error.h"
#include "pico/stdlib.h"

#include "cyw43.h"

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include <stdio.h>
#include <string.h>

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

bool wifi_is_ready(void)
{
    return wifi_hw_ready;
}

bool wifi_is_connected(void)
{
    return wifi_connected;
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
