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
