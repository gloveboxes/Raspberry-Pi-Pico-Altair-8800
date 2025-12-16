#include "ws.h"

#include "pico_ws_server/web_socket_server.h"
#include "pico/time.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
    static constexpr uint16_t WS_SERVER_PORT = 8088;
    static constexpr uint32_t WS_MAX_CLIENTS = 1;
    static constexpr size_t WS_FRAME_PAYLOAD = 256;
    static constexpr uint32_t WS_PING_INTERVAL_MS = 5000;
    static constexpr uint8_t WS_MAX_MISSED_PONGS = 3;

    struct ws_context_t
    {
        ws_callbacks_t callbacks;
    };

    static ws_context_t g_ws_context = {};
    static bool g_ws_initialized = false;
    static bool g_ws_running = false;
    static size_t g_ws_active_clients = 0;
    static std::unique_ptr<WebSocketServer> g_ws_server;
    static uint32_t g_ws_last_conn_id = 0;
    static absolute_time_t g_ws_next_ping_deadline;
    static uint8_t g_ws_pending_pings = 0;
    static uint8_t g_ws_missed_pongs = 0;

    static inline void reset_ping_state(void)
    {
        g_ws_pending_pings = 0;
        g_ws_missed_pongs = 0;
        g_ws_next_ping_deadline = make_timeout_time_ms(WS_PING_INTERVAL_MS);
    }

    static inline void mark_connection_closed(void)
    {
        g_ws_active_clients = 0;
        g_ws_last_conn_id = 0;
        reset_ping_state();
    }

    static void send_ping_if_due(void)
    {
        if (!g_ws_running || !g_ws_server || g_ws_active_clients == 0 || g_ws_last_conn_id == 0)
        {
            return;
        }

        absolute_time_t now = get_absolute_time();
        // Run only when now has reached or passed the deadline
        if (absolute_time_diff_us(now, g_ws_next_ping_deadline) > 0)
        {
            return; // Not time yet
        }

        if (g_ws_pending_pings > 0)
        {
            ++g_ws_missed_pongs;
            if (g_ws_missed_pongs > WS_MAX_MISSED_PONGS)
            {
                printf("WebSocket missed %u pongs, closing connection %u\n", g_ws_missed_pongs, g_ws_last_conn_id);
                g_ws_server->close(g_ws_last_conn_id);
                mark_connection_closed();
                return;
            }
        }

            if (g_ws_server->sendPing(g_ws_last_conn_id, nullptr, 0))
        {
            ++g_ws_pending_pings;
            printf("WebSocket sent PING (pending=%u, missed=%u)\n", g_ws_pending_pings, g_ws_missed_pongs);
        }
            else
            {
                ++g_ws_missed_pongs;
                printf("WebSocket PING send failed (missed=%u)\n", g_ws_missed_pongs);
                if (g_ws_missed_pongs > WS_MAX_MISSED_PONGS)
                {
                    printf("WebSocket closing connection %u after send failure\n", g_ws_last_conn_id);
                    g_ws_server->close(g_ws_last_conn_id);
                    mark_connection_closed();
                    return;
                }
            }

        g_ws_next_ping_deadline = delayed_by_ms(now, WS_PING_INTERVAL_MS);
    }

    void handle_connect(WebSocketServer &server, uint32_t conn_id)
    {
        ++g_ws_active_clients;
        g_ws_last_conn_id = conn_id;
        reset_ping_state();
        printf("WebSocket client connected (id=%u)\n", conn_id);

        ws_context_t *ctx = static_cast<ws_context_t *>(server.getCallbackExtra());
        if (ctx && ctx->callbacks.on_client_connected)
        {
            ctx->callbacks.on_client_connected(ctx->callbacks.user_data);
        }
    }

    void handle_close(WebSocketServer &server, uint32_t conn_id)
    {
        if (g_ws_active_clients > 0)
        {
            --g_ws_active_clients;
        }
        if (conn_id == g_ws_last_conn_id)
        {
            g_ws_last_conn_id = 0;
            reset_ping_state();
        }
        printf("WebSocket client closed (id=%u)\n", conn_id);

        ws_context_t *ctx = static_cast<ws_context_t *>(server.getCallbackExtra());
        if (ctx && ctx->callbacks.on_client_disconnected)
        {
            ctx->callbacks.on_client_disconnected(ctx->callbacks.user_data);
        }
    }

    void handle_message(WebSocketServer &server, uint32_t conn_id, const void *data, size_t len)
    {
        ws_context_t *ctx = static_cast<ws_context_t *>(server.getCallbackExtra());
        if (!ctx)
        {
            return;
        }

        bool keep_open = true;
        if (ctx->callbacks.on_receive)
        {
            keep_open = ctx->callbacks.on_receive(static_cast<const uint8_t *>(data), len, ctx->callbacks.user_data);
        }

        if (!keep_open)
        {
            server.close(conn_id);
        }
    }

    void handle_pong(WebSocketServer &server, uint32_t conn_id, const void *data, size_t len)
    {
        (void)data;
        (void)len;

        if (conn_id != g_ws_last_conn_id)
        {
            return;
        }

        reset_ping_state();
        printf("WebSocket received PONG from %u\n", conn_id);
    }
} // namespace

extern "C"
{

    void ws_init(const ws_callbacks_t *callbacks)
    {
        if (!callbacks)
        {
            std::memset(&g_ws_context, 0, sizeof(g_ws_context));
            g_ws_initialized = false;
            return;
        }

        g_ws_context.callbacks = *callbacks;
        g_ws_initialized = true;
    }

    bool ws_start(void)
    {
        if (!g_ws_initialized)
        {
            printf("WebSocket server not initialized\n");
            return false;
        }

        if (g_ws_running)
        {
            return true;
        }

        if (!g_ws_server)
        {
            g_ws_server = std::make_unique<WebSocketServer>(WS_MAX_CLIENTS);
            g_ws_server->setCallbackExtra(&g_ws_context);
            g_ws_server->setConnectCallback(handle_connect);
            g_ws_server->setCloseCallback(handle_close);
            g_ws_server->setMessageCallback(handle_message);
            g_ws_server->setPongCallback(handle_pong);
            g_ws_server->setTcpNoDelay(true);  // Disable Nagle's algorithm for low latency
        }

        g_ws_active_clients = 0;
        if (!g_ws_server->startListening(WS_SERVER_PORT))
        {
            printf("Failed to start WebSocket server on port %u\n", WS_SERVER_PORT);
            g_ws_server.reset();
            g_ws_running = false;
            return false;
        }

        g_ws_running = true;
        printf("WebSocket server listening on port %u\n", WS_SERVER_PORT);
        return true;
    }

    bool ws_is_running(void)
    {
        return g_ws_running && g_ws_server != nullptr;
    }

    bool ws_has_active_clients(void)
    {
        return g_ws_active_clients > 0;
    }

    void ws_poll_incoming(void)
    {
        if (!g_ws_running || !g_ws_server)
        {
            return;
        }

        if (g_ws_active_clients == 0 || !g_ws_context.callbacks.on_output)
        {
            return;
        }

        g_ws_server->popMessages();
    }

    void ws_poll_outgoing(void)
    {
        if (!g_ws_running || !g_ws_server)
        {
            return;
        }

        if (g_ws_active_clients == 0 || !g_ws_context.callbacks.on_output)
        {
            return;
        }

        // Heartbeat: send PING every WS_PING_INTERVAL_MS, close after WS_MAX_MISSED_PONGS
        send_ping_if_due();

        uint8_t payload[WS_FRAME_PAYLOAD];

        size_t payload_len = g_ws_context.callbacks.on_output(payload, sizeof(payload), g_ws_context.callbacks.user_data);
        if (payload_len == 0)
        {
            return;
        }
        // printf("WebSocket sending %zu bytes\n", payload_len);
        if (!g_ws_server->broadcastMessage(payload, payload_len))
        {
            // Send failed - likely due to full send buffer or network congestion
            // For real-time terminal output, we drop the data rather than queue it
            printf("WebSocket send failed, dropping %zu bytes\n", payload_len);
        }
    }
}