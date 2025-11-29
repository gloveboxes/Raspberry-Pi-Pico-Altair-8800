#include "websocket_console.h"

#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"

#include "ws.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define WS_TX_QUEUE_DEPTH 4096
#define WS_RX_QUEUE_DEPTH 128

static queue_t ws_rx_queue;
static queue_t ws_tx_queue;
static bool console_initialized = false;
static bool console_running = false;

static void websocket_console_core1_entry(void);
static bool websocket_console_handle_input(const uint8_t *payload, size_t payload_len, void *user_data);
static size_t websocket_console_supply_output(uint8_t *buffer, size_t max_len, void *user_data);
static void websocket_console_on_client_connected(void *user_data);
static void websocket_console_on_client_disconnected(void *user_data);
static void websocket_console_clear_tx_queue(void);
static void websocket_console_clear_queues(void);

void websocket_console_init(void)
{
    if (console_initialized)
    {
        return;
    }

    queue_init(&ws_rx_queue, sizeof(uint8_t), WS_RX_QUEUE_DEPTH);
    queue_init(&ws_tx_queue, sizeof(uint8_t), WS_TX_QUEUE_DEPTH);

    ws_callbacks_t callbacks = {
        .on_receive = websocket_console_handle_input,
        .on_output = websocket_console_supply_output,
        .on_client_connected = websocket_console_on_client_connected,
        .on_client_disconnected = websocket_console_on_client_disconnected,
        .user_data = NULL,
    };
    ws_init(&callbacks);

    console_initialized = true;
}

void websocket_console_start(void)
{
    if (!console_initialized)
    {
        websocket_console_init();
    }

    if (console_running)
    {
        return;
    }

    cyw43_arch_lwip_begin();
    bool ok = ws_start();
    cyw43_arch_lwip_end();

    if (!ok)
    {
        printf("Failed to start WebSocket console\n");
        return;
    }

    multicore_launch_core1(websocket_console_core1_entry);
    console_running = true;
    printf("WebSocket console listening\n");
}

bool websocket_console_is_running(void)
{
    return console_running && ws_is_running();
}

void websocket_console_enqueue_output(uint8_t value)
{
    if (!console_initialized)
    {
        return;
    }

    if (!ws_has_active_clients())
    {
        websocket_console_clear_tx_queue();
        return;
    }

    if (!queue_try_add(&ws_tx_queue, &value))
    {
        uint8_t discard = 0;
        if (queue_try_remove(&ws_tx_queue, &discard))
        {
            queue_try_add(&ws_tx_queue, &value);
        }
    }
}

bool websocket_console_try_dequeue_input(uint8_t *value)
{
    if (!console_initialized || !value)
    {
        return false;
    }

    return queue_try_remove(&ws_rx_queue, value);
}

static void websocket_console_core1_entry(void)
{
    while (true)
    {
        cyw43_arch_lwip_begin();
        ws_poll();
        cyw43_arch_lwip_end();
        tight_loop_contents();
        sleep_us(100);
    }
}

static bool websocket_console_handle_input(const uint8_t *payload, size_t payload_len, void *user_data)
{
    (void)user_data;

    if (!payload)
    {
        return false;
    }

    for (size_t i = 0; i < payload_len; ++i)
    {
        uint8_t ch = payload[i];
        if (ch == '\n')
        {
            ch = '\r';
        }
        if (!queue_try_add(&ws_rx_queue, &ch))
        {
            uint8_t discard = 0;
            if (queue_try_remove(&ws_rx_queue, &discard))
            {
                queue_try_add(&ws_rx_queue, &ch);
            }
        }
    }

    return true;
}

static size_t websocket_console_supply_output(uint8_t *buffer, size_t max_len, void *user_data)
{
    (void)user_data;

    size_t count = 0;
    while (count < max_len && queue_try_remove(&ws_tx_queue, &buffer[count]))
    {
        ++count;
    }

    return count;
}

static void websocket_console_on_client_connected(void *user_data)
{
    (void)user_data;
}

static void websocket_console_on_client_disconnected(void *user_data)
{
    (void)user_data;
    websocket_console_clear_queues();
}

static void websocket_console_clear_tx_queue(void)
{
    uint8_t discard = 0;
    while (queue_try_remove(&ws_tx_queue, &discard))
    {
    }
}

static void websocket_console_clear_queues(void)
{
    websocket_console_clear_tx_queue();

    uint8_t discard = 0;
    while (queue_try_remove(&ws_rx_queue, &discard))
    {
    }
}
