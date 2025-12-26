#include "openai_io.h"

#include "pico/stdlib.h" // Must be included before WiFi check to get board definitions

// OpenAI chat is only available on WiFi-enabled boards
#if defined(CYW43_WL_GPIO_LED_PIN)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parson.h"

#include "lwip/altcp.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "pico/util/queue.h"

// OpenAI API configuration
// API key should be defined via CMake: -DOPENAI_API_KEY="sk-..."
// or environment variable OPENAI_API_KEY at build time
#ifndef OPENAI_API_KEY
#warning "OPENAI_API_KEY not defined! Set environment variable OPENAI_API_KEY before building."
#define OPENAI_API_KEY ""
#endif

static const char* openai_api_key = OPENAI_API_KEY;
static const char* OPENAI_HOST = "api.openai.com";
static const uint16_t OPENAI_PORT = 443;

// Port definitions matching Linux reference
#define OAI_RESET_REQUEST 120
#define OAI_ADD_BYTE 121
#define OAI_RESET_RESPONSE 122
#define OAI_GET_STATUS 123
#define OAI_GET_BYTE 124
#define OAI_IS_COMPLETE 125
#define OAI_SET_LEN_LO 126
#define OAI_SET_LEN_HI 127

// Additional port mappings for input operations
#define OAI_GET_LEN_LO 121 // Get request buffer length low byte
#define OAI_GET_LEN_HI 122 // Get request buffer length high byte

// Status values matching Linux reference
#define OAI_EOF 0
#define OAI_WAITING 1
#define OAI_DATA_READY 2
#define OAI_FAILED 3
#define OAI_BUSY 4

// Buffer sizes - optimized for embedded device with limited RAM
// Increased recv_buf to handle bursts while Core 0 parses JSON with parson
#define REQUEST_CHUNK_SIZE 256
#define RESPONSE_CHUNK_SIZE 512 // Raw SSE frames can be ~300-400 bytes
#define TLS_RECV_BUF_SIZE 6144  // 6KB - min needed based on observed 5KB max
#define HTTP_BUF_SIZE 2048      // 2KB (max observed: 1.3KB)
#define SEND_BUF_SIZE 512       // 512B (max observed: 321B)

// Queue sizes
#define OUTBOUND_QUEUE_SIZE 2
#define BODY_CHUNK_QUEUE_SIZE 2 // Reverted to 2: Flow control (OAI_BUSY) handles overflow
#define INBOUND_QUEUE_SIZE 8    // 4KB - minimum needed to prevent deadlock during bursts

// Timeout configuration
#define OPENAI_TIMEOUT_MS 90000 // 90 second overall timeout
#define DNS_TIMEOUT_MS 10000    // 10 second DNS timeout

// Debug output - set to 1 to enable verbose debug, 0 to disable
#define DEBUG_OPENAI 0

#if DEBUG_OPENAI
#define DBG_PRINT(...) printf(__VA_ARGS__)
#else
#define DBG_PRINT(...) ((void)0)
#endif

// Request message (Core 0 -> Core 1) - now only carries content length
typedef struct
{
    size_t content_length; // Total JSON body length
    bool abort;
} openai_request_t;

// Response message (Core 1 -> Core 0)
typedef struct
{
    char data[RESPONSE_CHUNK_SIZE];
    size_t len;
    uint8_t status;
} openai_response_t;

// TLS connection state machine
typedef enum
{
    TLS_STATE_IDLE,
    TLS_STATE_DNS_RESOLVING,
    TLS_STATE_CONNECTING,
    TLS_STATE_TLS_HANDSHAKE,
    TLS_STATE_SENDING_HEADERS,
    TLS_STATE_STREAMING_BODY,
    TLS_STATE_RECEIVING,
    TLS_STATE_DONE,
    TLS_STATE_ERROR
} tls_state_t;

// State name strings for debug output
#if DEBUG_OPENAI
static const char* tls_state_names[] = {
    "IDLE",           "DNS_RESOLVING", "CONNECTING", "TLS_HANDSHAKE", "SENDING_HEADERS",
    "STREAMING_BODY", "RECEIVING",     "DONE",       "ERROR"};
#endif

// TLS connection context
typedef struct
{
    tls_state_t state;
    struct tcp_pcb* pcb;
    ip_addr_t server_ip;
    absolute_time_t start_time;
    absolute_time_t state_time;

    // mbedtls contexts
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    bool mbedtls_initialized;

    // Request data - now just tracks content length for streaming body
    size_t body_content_length; // Total body length to send
    size_t body_bytes_sent;     // Bytes of body sent so far
    bool http_headers_sent;     // Have we sent HTTP headers?

    // Partial chunk buffer for handling WANT_WRITE scenarios
    char partial_chunk_buf[RESPONSE_CHUNK_SIZE];
    size_t partial_chunk_len;
    size_t partial_chunk_offset;

    // TCP/TLS buffers
    char send_buf[SEND_BUF_SIZE];
    size_t send_len;
    size_t send_offset;

    char recv_buf[TLS_RECV_BUF_SIZE]; // Encrypted TCP data from network (needs 4KB for TLS handshake)
    size_t recv_len;                  // Total encrypted bytes in recv_buf
    size_t recv_offset;               // Read position for mbedtls (encrypted data)

    char http_buf[HTTP_BUF_SIZE]; // Decrypted HTTP response data
    size_t http_len;              // Total decrypted bytes in http_buf

    // HTTP parsing
    bool headers_complete;
    int http_status_code;
    // SSE streaming
    bool stream_done;
    size_t total_bytes_received;

    // Size tracking for optimization
    size_t max_recv_buf_used;
    size_t max_http_buf_used;
    size_t max_send_buf_used;
    size_t max_sse_line_size;
} openai_tls_t;

// State for port handling (Core 0)
typedef struct
{
    // Content length (sent via ports 126/127)
    uint16_t content_length;
    uint8_t content_length_lo; // Low byte received flag
    bool content_length_ready;

    // Chunk buffer for streaming body to Core 1
    char chunk_buffer[REQUEST_CHUNK_SIZE];
    size_t chunk_index;

    uint8_t status;
    bool request_pending;
    bool body_complete;

    // Response chunk buffer
    char response_buffer[RESPONSE_CHUNK_SIZE];
    size_t response_bytes_available;
    size_t response_position;
    bool response_complete;
} openai_port_state_t;

// Queues for inter-core communication
static queue_t outbound_queue;   // Core 0 -> Core 1: Request start (openai_request_t)
static queue_t body_chunk_queue; // Core 0 -> Core 1: Body chunks (openai_response_t)
static queue_t inbound_queue;    // Core 1 -> Core 0: Response data

// State variables
static openai_port_state_t port_state;
static openai_tls_t tls_ctx;

// === Helper Functions ===

static bool queue_add_nonblocking(queue_t* queue, const void* data)
{
    // Fully non-blocking - never stall Core 1's network loop
    // With 64-entry queue (16KB buffer), overflow should be rare
    if (queue_try_add(queue, data))
    {
        return true;
    }
    // Queue full - this is rare with large queue, but don't block
    DBG_PRINT("[OAI:Q] Queue full, data dropped\n");
    return false;
}

// Check if inbound queue has room for more data
// Used for backpressure - if queue is nearly full, stop processing to let Core 0 catch up
static bool inbound_queue_has_space(size_t chunks_needed)
{
    // Leave at least 2 slots free for status messages (EOF, FAILED)
    int available = INBOUND_QUEUE_SIZE - queue_get_level(&inbound_queue);
    return available > (int)(chunks_needed + 2);
}

static bool queue_content(const char* content, size_t len)
{
    if (len == 0 || content == NULL)
        return true;

    // Calculate how many chunks this will need
    size_t chunks_needed = (len + RESPONSE_CHUNK_SIZE - 2) / (RESPONSE_CHUNK_SIZE - 1);

    // Check if queue has room - if not, apply backpressure
    if (!inbound_queue_has_space(chunks_needed))
    {
        DBG_PRINT("[OAI:Q] Queue nearly full, applying backpressure\n");
        return false; // Signal caller to stop processing
    }

    size_t offset = 0;
    while (offset < len)
    {
        openai_response_t response;
        memset(&response, 0, sizeof(response));

        size_t chunk_len = len - offset;
        if (chunk_len > RESPONSE_CHUNK_SIZE - 1)
            chunk_len = RESPONSE_CHUNK_SIZE - 1;

        memcpy(response.data, content + offset, chunk_len);
        response.len = chunk_len;
        response.status = OAI_DATA_READY;

        queue_add_nonblocking(&inbound_queue, &response);

        offset += chunk_len;
    }
    return true;
}

static void send_status(uint8_t status)
{
    const char* status_names[] = {"EOF", "WAITING", "DATA_READY", "FAILED"};
    DBG_PRINT("[OAI:Q] Sending status: %s\n", status <= 3 ? status_names[status] : "UNKNOWN");

    openai_response_t response;
    memset(&response, 0, sizeof(response));
    response.status = status;
    response.len = 0;
    queue_add_nonblocking(&inbound_queue, &response);
}

// === SSE Frame Detection (Core 1) ===
// Minimal processing - just detect frame boundaries and queue raw frames

// Extract complete SSE frame from http_buf
// Returns malloc'd NUL-terminated frame string, or NULL if incomplete
// Caller must free the returned string
static char* sse_pop_frame(void)
{
    if (tls_ctx.http_len == 0)
        return NULL;

    // Find \n\n or \r\n\r\n delimiter marking end of SSE event
    size_t delim_len = 0;
    size_t frame_len = 0;

    // Check for \n\n
    for (size_t i = 0; i + 1 < tls_ctx.http_len; i++)
    {
        if (tls_ctx.http_buf[i] == '\n' && tls_ctx.http_buf[i + 1] == '\n')
        {
            delim_len = 2;
            frame_len = i;
            break;
        }
    }

    // If not found, check for \r\n\r\n
    if (!delim_len)
    {
        for (size_t i = 0; i + 3 < tls_ctx.http_len; i++)
        {
            if (tls_ctx.http_buf[i] == '\r' && tls_ctx.http_buf[i + 1] == '\n' && tls_ctx.http_buf[i + 2] == '\r' &&
                tls_ctx.http_buf[i + 3] == '\n')
            {
                delim_len = 4;
                frame_len = i;
                break;
            }
        }
    }

    if (!delim_len)
        return NULL; // Incomplete frame, wait for more data

    // Track max SSE frame size
    if (frame_len > tls_ctx.max_sse_line_size)
    {
        tls_ctx.max_sse_line_size = frame_len;
    }

    // Allocate and copy frame
    char* frame = (char*)malloc(frame_len + 1);
    if (!frame)
    {
        printf("[OpenAI:ERROR] malloc failed for SSE frame\n");
        return NULL;
    }
    memcpy(frame, tls_ctx.http_buf, frame_len);
    frame[frame_len] = '\0';

    // Consume frame + delimiter from buffer
    size_t consumed = frame_len + delim_len;
    memmove(tls_ctx.http_buf, tls_ctx.http_buf + consumed, tls_ctx.http_len - consumed);
    tls_ctx.http_len -= consumed;
    tls_ctx.http_buf[tls_ctx.http_len] = '\0';

    DBG_PRINT("[OAI:SSE] Popped frame (%zu bytes)\n", frame_len);
    return frame;
}

// Extract the data: payload from an SSE frame
// Returns pointer into frame (not allocated), or NULL
// Also sets *is_done if payload is [DONE]
static const char* extract_sse_data(const char* frame, bool* is_done)
{
    *is_done = false;

    // Find "data:" line (may have multiple lines, we want the data one)
    const char* data_line = strstr(frame, "data:");
    if (!data_line)
        return NULL;

    const char* payload = data_line + 5;
    // Skip optional space after colon
    while (*payload == ' ')
        payload++;

    // Check for [DONE] marker
    if (strncmp(payload, "[DONE]", 6) == 0)
    {
        *is_done = true;
        DBG_PRINT("[OAI:SSE] <<< [DONE] received >>>\n");
        printf("\n[OpenAI] Stream complete\n");
        return NULL;
    }

    return payload;
}

// Queue raw SSE frame data to Core 0 for JSON parsing
// Returns false if queue is full (backpressure)
static bool queue_sse_frame(const char* frame)
{
    bool is_done = false;
    const char* payload = extract_sse_data(frame, &is_done);

    if (is_done)
    {
        tls_ctx.stream_done = true;
        return true; // Don't queue [DONE], it's handled via EOF status
    }

    if (!payload)
        return true; // No data line, skip (comment or other field)

    // Check if queue has room
    if (!inbound_queue_has_space(1))
    {
        DBG_PRINT("[OAI:SSE] Queue full, backpressure\n");
        return false;
    }

    // Queue the JSON payload for Core 0 to parse
    size_t payload_len = strlen(payload);

    // Trim trailing \r or \n if present
    while (payload_len > 0 && (payload[payload_len - 1] == '\r' || payload[payload_len - 1] == '\n'))
    {
        payload_len--;
    }

    if (payload_len == 0)
        return true;

    // Split into chunks if needed (unlikely for single SSE event)
    openai_response_t response;
    memset(&response, 0, sizeof(response));

    size_t copy_len = payload_len;
    if (copy_len > RESPONSE_CHUNK_SIZE - 1)
    {
        copy_len = RESPONSE_CHUNK_SIZE - 1;
        printf("[OAI:WARN] SSE payload truncated (%zu > %d)\n", payload_len, RESPONSE_CHUNK_SIZE - 1);
    }

    memcpy(response.data, payload, copy_len);
    response.data[copy_len] = '\0';
    response.len = copy_len;
    response.status = OAI_DATA_READY;

    return queue_add_nonblocking(&inbound_queue, &response);
}

// Process received HTTP data - parse headers and extract SSE frames
// This runs on Core 1 - keep it minimal!
static void process_received_data(bool flush)
{
    char* data = tls_ctx.http_buf;
    size_t len = tls_ctx.http_len;

    // Parse HTTP headers first (only once)
    if (!tls_ctx.headers_complete)
    {
        char* header_end = strstr(data, "\r\n\r\n");
        if (header_end)
        {
            tls_ctx.headers_complete = true;

            // Parse status code
            char* http_line = strstr(data, "HTTP/");
            if (http_line)
            {
                char* status_start = strchr(http_line, ' ');
                if (status_start)
                {
                    tls_ctx.http_status_code = atoi(status_start + 1);
                    printf("[OpenAI] HTTP %d\n", tls_ctx.http_status_code);
                }
            }

            // Move past headers
            size_t header_len = (header_end - data) + 4;
            memmove(data, data + header_len, len - header_len);
            tls_ctx.http_len = len - header_len;
            tls_ctx.http_buf[tls_ctx.http_len] = '\0';
        }
        else
        {
            return; // Wait for complete headers
        }
    }

    // Pop and queue complete SSE frames
    char* frame;
    while ((frame = sse_pop_frame()) != NULL)
    {
        if (!queue_sse_frame(frame))
        {
            // Backpressure - we need to keep this frame
            // Put it back at the start of the buffer (it was already consumed)
            size_t frame_len = strlen(frame);
            if (frame_len + 2 + tls_ctx.http_len < HTTP_BUF_SIZE)
            {
                // Make room and prepend frame + delimiter
                memmove(tls_ctx.http_buf + frame_len + 2, tls_ctx.http_buf, tls_ctx.http_len);
                memcpy(tls_ctx.http_buf, frame, frame_len);
                tls_ctx.http_buf[frame_len] = '\n';
                tls_ctx.http_buf[frame_len + 1] = '\n';
                tls_ctx.http_len += frame_len + 2;
                tls_ctx.http_buf[tls_ctx.http_len] = '\0';
                DBG_PRINT("[OAI:SSE] Backpressure: prepended frame (%zu bytes)\n", frame_len);
            }
            else
            {
                // CRITICAL: Can't prepend, buffer too full - this should not happen
                printf("[OpenAI:ERROR] Buffer full, frame lost (%zu bytes)!\n", frame_len);
            }
            free(frame);
            break; // Stop processing, Core 0 needs to catch up
        }
        free(frame);

        if (tls_ctx.stream_done)
            break;
    }

    // On flush, process any remaining partial data as final frame
    if (flush && tls_ctx.http_len > 0)
    {
        DBG_PRINT("[OAI:SSE] Flushing %zu bytes as final frame\n", tls_ctx.http_len);
        queue_sse_frame(tls_ctx.http_buf);
        tls_ctx.http_len = 0;
        tls_ctx.http_buf[0] = '\0';
    }
}

// === mbedtls I/O callbacks (using lwIP TCP) ===

static int mbedtls_lwip_send(void* ctx, const unsigned char* buf, size_t len)
{
    struct tcp_pcb* pcb = (struct tcp_pcb*)ctx;

    if (pcb == NULL)
        return MBEDTLS_ERR_NET_CONN_RESET;

    u16_t sndbuf = tcp_sndbuf(pcb);
    if (sndbuf == 0)
        return MBEDTLS_ERR_SSL_WANT_WRITE;

    size_t to_send = len < sndbuf ? len : sndbuf;

    err_t err = tcp_write(pcb, buf, to_send, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK)
    {
        DBG_PRINT("[OAI:TLS] tcp_write error: %d\n", err);
        if (err == ERR_MEM)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    tcp_output(pcb);
    DBG_PRINT("[OAI:TLS] Sent %zu bytes via TCP\n", to_send);
    return (int)to_send;
}

static int mbedtls_lwip_recv(void* ctx, unsigned char* buf, size_t len)
{
    (void)ctx;

    if (tls_ctx.recv_offset >= tls_ctx.recv_len)
    {
        // No data available
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    size_t available = tls_ctx.recv_len - tls_ctx.recv_offset;
    size_t to_copy = len < available ? len : available;

    memcpy(buf, tls_ctx.recv_buf + tls_ctx.recv_offset, to_copy);
    tls_ctx.recv_offset += to_copy;

    DBG_PRINT("[OAI:TLS] Recv callback: %zu bytes (offset now %zu/%zu)\n", to_copy, tls_ctx.recv_offset,
              tls_ctx.recv_len);
    return (int)to_copy;
}

// === lwIP TCP callbacks ===

static err_t tcp_recv_callback(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err)
{
    (void)arg;

    if (err != ERR_OK)
    {
        if (p)
            pbuf_free(p);
        return err;
    }

    if (p == NULL)
    {
        // Connection closed
        printf("[OpenAI] Connection closed\n");
        tls_ctx.state = TLS_STATE_DONE;
        return ERR_OK;
    }

    // If buffer getting tight and we already consumed some, compact to free space
    if (tls_ctx.recv_offset > 0 && tls_ctx.recv_len > (TLS_RECV_BUF_SIZE * 3 / 4))
    {
        size_t remaining = tls_ctx.recv_len - tls_ctx.recv_offset;
        memmove(tls_ctx.recv_buf, tls_ctx.recv_buf + tls_ctx.recv_offset, remaining);
        tls_ctx.recv_len = remaining;
        tls_ctx.recv_offset = 0;
        DBG_PRINT("[OAI:TCP] Compacted recv_buf, %zu bytes remain\n", remaining);
    }

    // Copy data to receive buffer - track how much we actually store
    struct pbuf* q = p;
    size_t total_copied = 0;
    while (q != NULL)
    {
        size_t space = TLS_RECV_BUF_SIZE - tls_ctx.recv_len;
        size_t copy = q->len < space ? q->len : space;

        if (copy > 0)
        {
            memcpy(tls_ctx.recv_buf + tls_ctx.recv_len, q->payload, copy);
            tls_ctx.recv_len += copy;
            total_copied += copy;
        }

        // If we couldn't copy everything, stop - flow control will pause server
        if (copy < q->len)
        {
            DBG_PRINT("[OAI:TCP] Buffer full, flow control active\n");
            break;
        }

        q = q->next;
    }

    tls_ctx.total_bytes_received += total_copied;
    DBG_PRINT("[OAI:TCP] Received %u bytes (copied: %zu, buf: %zu/%d = %d%%)\n", p->tot_len, total_copied,
              tls_ctx.recv_len, TLS_RECV_BUF_SIZE, (int)((tls_ctx.recv_len * 100) / TLS_RECV_BUF_SIZE));

    // Track max recv_buf usage
    if (tls_ctx.recv_len > tls_ctx.max_recv_buf_used)
    {
        tls_ctx.max_recv_buf_used = tls_ctx.recv_len;
    }

    // Warn if buffer is getting full
    if (tls_ctx.recv_len > (TLS_RECV_BUF_SIZE * 3 / 4))
    {
        printf("[OpenAI:WARN] recv_buf at %d%% capacity (%zu/%d bytes)\n",
               (int)((tls_ctx.recv_len * 100) / TLS_RECV_BUF_SIZE), tls_ctx.recv_len, TLS_RECV_BUF_SIZE);
    }

    // TCP FLOW CONTROL: Only ACK bytes we actually stored
    // This closes TCP window when buffer is full, causing server to pause
    // When we process data and free buffer space, window reopens automatically
    if (total_copied > 0)
    {
        tcp_recved(pcb, total_copied);
    }
    pbuf_free(p);

    return ERR_OK;
}

static err_t tcp_connected_callback(void* arg, struct tcp_pcb* pcb, err_t err)
{
    (void)arg;
    (void)pcb;

    if (err != ERR_OK)
    {
        printf("[OpenAI] Connect error: %d\n", err);
        tls_ctx.state = TLS_STATE_ERROR;
        return err;
    }

    printf("[OpenAI] TCP connected\n");
    tls_ctx.state = TLS_STATE_TLS_HANDSHAKE;
    tls_ctx.state_time = get_absolute_time();

    return ERR_OK;
}

static void tcp_err_callback(void* arg, err_t err)
{
    (void)arg;
    printf("[OpenAI] TCP error: %d\n", err);
    tls_ctx.state = TLS_STATE_ERROR;
    tls_ctx.pcb = NULL; // PCB already freed
}

// DNS callback
static void dns_callback(const char* name, const ip_addr_t* addr, void* arg)
{
    (void)name;
    (void)arg;

    if (addr != NULL)
    {
        tls_ctx.server_ip = *addr;
        tls_ctx.state = TLS_STATE_CONNECTING;
        printf("[OpenAI] DNS: %s\n", ipaddr_ntoa(addr));
    }
    else
    {
        printf("[OpenAI] DNS failed\n");
        tls_ctx.state = TLS_STATE_ERROR;
    }
}

// === TLS State Machine ===

static void tls_cleanup(void)
{
    if (tls_ctx.pcb != NULL)
    {
        tcp_arg(tls_ctx.pcb, NULL);
        tcp_recv(tls_ctx.pcb, NULL);
        tcp_err(tls_ctx.pcb, NULL);
        tcp_close(tls_ctx.pcb);
        tls_ctx.pcb = NULL;
    }

    if (tls_ctx.mbedtls_initialized)
    {
        mbedtls_ssl_free(&tls_ctx.ssl);
        mbedtls_ssl_config_free(&tls_ctx.conf);
        mbedtls_ctr_drbg_free(&tls_ctx.ctr_drbg);
        mbedtls_entropy_free(&tls_ctx.entropy);
        tls_ctx.mbedtls_initialized = false;
    }
}

static bool start_request(size_t content_length)
{
    // Reset state
    tls_cleanup();
    memset(&tls_ctx, 0, sizeof(tls_ctx));
    tls_ctx.start_time = get_absolute_time();
    tls_ctx.state_time = tls_ctx.start_time;

    // Reset size tracking
    tls_ctx.max_recv_buf_used = 0;
    tls_ctx.max_http_buf_used = 0;
    tls_ctx.max_send_buf_used = 0;
    tls_ctx.max_sse_line_size = 0;

    // Store content length for streaming body
    tls_ctx.body_content_length = content_length;
    tls_ctx.body_bytes_sent = 0;
    tls_ctx.http_headers_sent = false;

    // Initialize mbedtls
    mbedtls_ssl_init(&tls_ctx.ssl);
    mbedtls_ssl_config_init(&tls_ctx.conf);
    mbedtls_ctr_drbg_init(&tls_ctx.ctr_drbg);
    mbedtls_entropy_init(&tls_ctx.entropy);
    tls_ctx.mbedtls_initialized = true;

    // Seed RNG
    int ret = mbedtls_ctr_drbg_seed(&tls_ctx.ctr_drbg, mbedtls_entropy_func, &tls_ctx.entropy,
                                    (const unsigned char*)"openai", 6);
    if (ret != 0)
    {
        printf("[OpenAI] RNG seed failed: %d\n", ret);
        return false;
    }

    // Configure TLS
    ret = mbedtls_ssl_config_defaults(&tls_ctx.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
    {
        printf("[OpenAI] TLS config failed: %d\n", ret);
        return false;
    }

    // WARNING: Certificate verification is disabled for simplicity
    // This makes the connection vulnerable to man-in-the-middle attacks
    // In production, use MBEDTLS_SSL_VERIFY_REQUIRED and provide CA certificates
    mbedtls_ssl_conf_authmode(&tls_ctx.conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&tls_ctx.conf, mbedtls_ctr_drbg_random, &tls_ctx.ctr_drbg);

    ret = mbedtls_ssl_setup(&tls_ctx.ssl, &tls_ctx.conf);
    if (ret != 0)
    {
        printf("[OpenAI] SSL setup failed: %d\n", ret);
        return false;
    }

    // Set hostname for SNI
    mbedtls_ssl_set_hostname(&tls_ctx.ssl, OPENAI_HOST);

    // Start DNS resolution
    tls_ctx.state = TLS_STATE_DNS_RESOLVING;
    DBG_PRINT("[OAI:STATE] -> DNS_RESOLVING\n");
    err_t err = dns_gethostbyname(OPENAI_HOST, &tls_ctx.server_ip, dns_callback, NULL);

    if (err == ERR_OK)
    {
        tls_ctx.state = TLS_STATE_CONNECTING;
        DBG_PRINT("[OAI:STATE] -> CONNECTING (DNS cached)\n");
        printf("[OpenAI] DNS cached\n");
    }
    else if (err != ERR_INPROGRESS)
    {
        printf("[OpenAI] DNS error: %d\n", err);
        return false;
    }
    else
    {
        DBG_PRINT("[OAI:STATE] DNS lookup in progress...\n");
    }

    return true;
}

static void poll_tls_state_machine(void)
{
    int ret;
    err_t err;
    static tls_state_t last_state = TLS_STATE_IDLE;

    // Log state transitions
    if (tls_ctx.state != last_state)
    {
        DBG_PRINT("[OAI:STATE] %s -> %s\n", tls_state_names[last_state], tls_state_names[tls_ctx.state]);
        last_state = tls_ctx.state;
    }

    // Check overall timeout
    int64_t elapsed_ms = absolute_time_diff_us(tls_ctx.start_time, get_absolute_time()) / 1000;
    if (elapsed_ms > OPENAI_TIMEOUT_MS)
    {
        printf("[OpenAI] Timeout after %lld ms\n", elapsed_ms);
        tls_ctx.state = TLS_STATE_ERROR;
    }

    switch (tls_ctx.state)
    {
        case TLS_STATE_IDLE:
            break;

        case TLS_STATE_DNS_RESOLVING:
            if (absolute_time_diff_us(tls_ctx.state_time, get_absolute_time()) / 1000 > DNS_TIMEOUT_MS)
            {
                printf("[OpenAI] DNS timeout\n");
                tls_ctx.state = TLS_STATE_ERROR;
            }
            break;

        case TLS_STATE_CONNECTING:
            // Only attempt connection if PCB not yet allocated
            if (tls_ctx.pcb != NULL)
            {
                // Already connecting, just wait for callback
                break;
            }

            // Create TCP PCB and connect (only once)
            tls_ctx.pcb = tcp_new();
            if (tls_ctx.pcb == NULL)
            {
                printf("[OpenAI] PCB alloc failed\n");
                tls_ctx.state = TLS_STATE_ERROR;
                break;
            }

            tcp_arg(tls_ctx.pcb, NULL);
            tcp_recv(tls_ctx.pcb, tcp_recv_callback);
            tcp_err(tls_ctx.pcb, tcp_err_callback);

            printf("[OpenAI] Connecting...\n");
            err = tcp_connect(tls_ctx.pcb, &tls_ctx.server_ip, OPENAI_PORT, tcp_connected_callback);
            if (err != ERR_OK)
            {
                printf("[OpenAI] Connect failed: %d\n", err);
                tls_ctx.state = TLS_STATE_ERROR;
            }
            else
            {
                tls_ctx.state_time = get_absolute_time();
                // State stays CONNECTING until callback fires
            }
            break;

        case TLS_STATE_TLS_HANDSHAKE:
            // Set up I/O callbacks
            mbedtls_ssl_set_bio(&tls_ctx.ssl, tls_ctx.pcb, mbedtls_lwip_send, mbedtls_lwip_recv, NULL);

            ret = mbedtls_ssl_handshake(&tls_ctx.ssl);
            if (ret == 0)
            {
                printf("[OpenAI] TLS handshake complete\n");
                tls_ctx.state = TLS_STATE_SENDING_HEADERS;
                tls_ctx.state_time = get_absolute_time();
            }
            else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            {
                printf("[OpenAI] Handshake failed: -0x%X\n", -ret);
                tls_ctx.state = TLS_STATE_ERROR;
            }
            else
            {
            }
            break;

        case TLS_STATE_SENDING_HEADERS:
            // Build HTTP headers only (no body) if not done
            if (tls_ctx.send_len == 0)
            {
                int ret_len = snprintf(tls_ctx.send_buf, SEND_BUF_SIZE,
                                       "POST /v1/chat/completions HTTP/1.1\r\n"
                                       "Host: %s\r\n"
                                       "Authorization: Bearer %s\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Content-Length: %zu\r\n"
                                       "Connection: close\r\n"
                                       "\r\n",
                                       OPENAI_HOST, openai_api_key, tls_ctx.body_content_length);
                if (ret_len < 0 || ret_len >= SEND_BUF_SIZE)
                {
                    printf("[OpenAI] HTTP headers error/truncated (ret=%d, max=%d)\n", ret_len, SEND_BUF_SIZE);
                    tls_ctx.state = TLS_STATE_ERROR;
                    break;
                }
                tls_ctx.send_len = (size_t)ret_len;
                printf("[OpenAI] Sending headers %zu bytes (body will be %zu bytes)\n", tls_ctx.send_len,
                       tls_ctx.body_content_length);

                // Track max send_buf usage
                if (tls_ctx.send_len > tls_ctx.max_send_buf_used)
                {
                    tls_ctx.max_send_buf_used = tls_ctx.send_len;
                }
            }

            // Send headers via TLS
            ret = mbedtls_ssl_write(&tls_ctx.ssl, (const unsigned char*)tls_ctx.send_buf + tls_ctx.send_offset,
                                    tls_ctx.send_len - tls_ctx.send_offset);
            if (ret > 0)
            {
                tls_ctx.send_offset += ret;
                if (tls_ctx.send_offset >= tls_ctx.send_len)
                {
                    printf("[OpenAI] Headers sent, waiting for body chunks\n");
                    tls_ctx.state = TLS_STATE_STREAMING_BODY;
                    tls_ctx.http_headers_sent = true;
                    tls_ctx.state_time = get_absolute_time();
                }
            }
            else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            {
                printf("[OpenAI] Send headers failed: -0x%X\n", -ret);
                tls_ctx.state = TLS_STATE_ERROR;
            }
            break;

        case TLS_STATE_STREAMING_BODY:
            // Poll for body chunks from Core 0 and write them to TLS
            {
                // First, try to send any partial chunk from previous iteration
                if (tls_ctx.partial_chunk_len > 0)
                {
                    ret = mbedtls_ssl_write(
                        &tls_ctx.ssl, (const unsigned char*)tls_ctx.partial_chunk_buf + tls_ctx.partial_chunk_offset,
                        tls_ctx.partial_chunk_len - tls_ctx.partial_chunk_offset);
                    if (ret > 0)
                    {
                        tls_ctx.partial_chunk_offset += ret;
                        tls_ctx.body_bytes_sent += ret;
                        DBG_PRINT("[OAI:BODY] Wrote partial %d bytes (total: %zu/%zu)\n", ret, tls_ctx.body_bytes_sent,
                                  tls_ctx.body_content_length);

                        if (tls_ctx.partial_chunk_offset >= tls_ctx.partial_chunk_len)
                        {
                            // Partial chunk fully sent, clear it
                            tls_ctx.partial_chunk_len = 0;
                            tls_ctx.partial_chunk_offset = 0;
                        }
                    }
                    else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
                    {
                        printf("[OpenAI] Partial chunk write failed: -0x%X\n", -ret);
                        tls_ctx.state = TLS_STATE_ERROR;
                        break;
                    }
                    // If still would block, don't try to get new chunk yet
                    if (tls_ctx.partial_chunk_len > 0)
                        break;
                }

                // Try to get a new chunk from queue
                openai_response_t chunk;
                if (queue_try_remove(&body_chunk_queue, &chunk))
                {
                    if (chunk.status == OAI_EOF && chunk.len == 0)
                    {
                        // End of body marker
                        printf("[OpenAI] Body complete (EOF marker), %zu bytes sent\n", tls_ctx.body_bytes_sent);
                        tls_ctx.state = TLS_STATE_RECEIVING;
                        tls_ctx.recv_len = 0;
                        tls_ctx.recv_offset = 0;
                        tls_ctx.state_time = get_absolute_time();
                    }
                    else if (chunk.len > 0)
                    {
                        // Debug: print chunk content
                        DBG_PRINT("[OAI:CHUNK] Got %zu bytes from queue\n", chunk.len);

                        // Try to write chunk data to TLS
                        ret = mbedtls_ssl_write(&tls_ctx.ssl, (const unsigned char*)chunk.data, chunk.len);
                        if (ret > 0)
                        {
                            tls_ctx.body_bytes_sent += ret;
                            DBG_PRINT("[OAI:BODY] Wrote %d bytes (total: %zu/%zu)\n", ret, tls_ctx.body_bytes_sent,
                                      tls_ctx.body_content_length);

                            // Check if we only sent part of the chunk
                            if ((size_t)ret < chunk.len)
                            {
                                // Save the remaining data for next iteration
                                size_t remaining = chunk.len - ret;
                                memcpy(tls_ctx.partial_chunk_buf, chunk.data + ret, remaining);
                                tls_ctx.partial_chunk_len = remaining;
                                tls_ctx.partial_chunk_offset = 0;
                                DBG_PRINT("[OAI:BODY] Partial write, %zu bytes buffered\n", remaining);
                            }
                        }
                        else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
                        {
                            // Would block - save entire chunk for later
                            memcpy(tls_ctx.partial_chunk_buf, chunk.data, chunk.len);
                            tls_ctx.partial_chunk_len = chunk.len;
                            tls_ctx.partial_chunk_offset = 0;
                            DBG_PRINT("[OAI:BODY] Write would block, chunk buffered\n");
                        }
                        else
                        {
                            printf("[OpenAI] Body write failed: -0x%X\n", -ret);
                            tls_ctx.state = TLS_STATE_ERROR;
                            break;
                        }

                        // Check if body is complete by byte count
                        if (tls_ctx.body_bytes_sent >= tls_ctx.body_content_length)
                        {
                            printf("[OpenAI] Body complete (byte count), %zu bytes sent\n", tls_ctx.body_bytes_sent);
                            tls_ctx.state = TLS_STATE_RECEIVING;
                            tls_ctx.recv_len = 0;
                            tls_ctx.recv_offset = 0;
                            tls_ctx.state_time = get_absolute_time();

                            // Drain any remaining items from body_chunk_queue (including EOF marker)
                            openai_response_t dummy;
                            while (queue_try_remove(&body_chunk_queue, &dummy))
                            {
                            }
                        }
                    }
                }
            }
            break;

        case TLS_STATE_RECEIVING:
            // Keep reading all available encrypted data
            while (true)
            {
                // CRITICAL: Process any existing data in http_buf FIRST
                // This drains the buffer and makes room for more decryption
                if (tls_ctx.http_len > 0)
                {
                    process_received_data(false);

                    if (tls_ctx.stream_done)
                    {
                        DBG_PRINT("[OAI:SSE] Stream marked done, transitioning to DONE\n");
                        tls_ctx.state = TLS_STATE_DONE;
                        break;
                    }
                }

                // Now check if we have space to decrypt more
                size_t space_available = HTTP_BUF_SIZE - tls_ctx.http_len - 1;

                if (space_available == 0)
                {
                    // Can't decrypt more right now - yield to let Core 0 drain queue
                    // DON'T break - we have encrypted data in recv_buf that MUST be processed
                    // Breaking here would leave data unprocessed and cause TLS corruption
                    DBG_PRINT("[OAI:TLS] http_buf full, yielding to let Core 0 catch up\n");
                    return; // Return from state machine, will retry next poll
                }

                ret = mbedtls_ssl_read(&tls_ctx.ssl, (unsigned char*)tls_ctx.http_buf + tls_ctx.http_len,
                                       space_available);
                if (ret > 0)
                {
                    DBG_PRINT("[OAI:TLS] Decrypted %d bytes (http_buf: %zu -> %zu, %d%% full)\n", ret, tls_ctx.http_len,
                              tls_ctx.http_len + ret, (int)(((tls_ctx.http_len + ret) * 100) / HTTP_BUF_SIZE));

                    tls_ctx.http_len += ret;
                    tls_ctx.http_buf[tls_ctx.http_len] = '\0';

                    // Track max http_buf usage
                    if (tls_ctx.http_len > tls_ctx.max_http_buf_used)
                    {
                        tls_ctx.max_http_buf_used = tls_ctx.http_len;
                    }

#if DEBUG_OPENAI
                    // Debug: Show what we decrypted (first 1025 chars, safely)
                    char preview[1026];
                    size_t preview_len = tls_ctx.http_len < 1025 ? tls_ctx.http_len : 1025;
                    memcpy(preview, tls_ctx.http_buf, preview_len);
                    preview[preview_len] = '\0';
                    printf("[OAI:TLS] Decrypted content (first %zu chars): %s\n", preview_len, preview);
#endif

                    // Warn if http_buf is getting full
                    if (tls_ctx.http_len > (HTTP_BUF_SIZE * 3 / 4))
                    {
                        printf("[OpenAI:WARN] http_buf at %d%% capacity (%zu/%d bytes)\n",
                               (int)((tls_ctx.http_len * 100) / HTTP_BUF_SIZE), tls_ctx.http_len, HTTP_BUF_SIZE);
                    }

                    // Encrypted data has been consumed by mbedtls - we can discard it
                    // Reset recv buffer for next encrypted chunk
                    if (tls_ctx.recv_offset >= tls_ctx.recv_len)
                    {
                        tls_ctx.recv_len = 0;
                        tls_ctx.recv_offset = 0;
                    }

                    // Continue loop to process this new data at top
                }
                else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0)
                {
                    DBG_PRINT("[OAI:TLS] Peer closed connection\n");
                    printf("[OpenAI] Server closed connection\n");

                    // CRITICAL: Flush any remaining data in parsing buffer
                    // This handles cases where the stream ends abruptly without a newline
                    process_received_data(true);

                    tls_ctx.state = TLS_STATE_DONE;
                    break;
                }
                else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
                {
                    // No more data available right now, exit loop and wait
                    break;
                }
                else
                {
                    printf("[OpenAI] Read error: -0x%X\n", -ret);

                    // CRITICAL: Flush remaining data even on error
                    // This rescues the final text chunk if the connection dies abruptly
                    process_received_data(true);

                    tls_ctx.state = TLS_STATE_ERROR;
                    break;
                }
            }
            break;

        case TLS_STATE_DONE:
            printf("[OpenAI] Complete, %zu bytes\n", tls_ctx.total_bytes_received);
            printf("\n=== BUFFER USAGE STATISTICS ===\n");
            printf("Max recv_buf used:  %zu / %d bytes (%d%%)\n", tls_ctx.max_recv_buf_used, TLS_RECV_BUF_SIZE,
                   (int)((tls_ctx.max_recv_buf_used * 100) / TLS_RECV_BUF_SIZE));
            printf("Max http_buf used:  %zu / %d bytes (%d%%)\n", tls_ctx.max_http_buf_used, HTTP_BUF_SIZE,
                   (int)((tls_ctx.max_http_buf_used * 100) / HTTP_BUF_SIZE));
            printf("Max send_buf used:  %zu / %d bytes (%d%%)\n", tls_ctx.max_send_buf_used, SEND_BUF_SIZE,
                   (int)((tls_ctx.max_send_buf_used * 100) / SEND_BUF_SIZE));
            printf("Max SSE line size:  %zu bytes\n", tls_ctx.max_sse_line_size);
            printf("================================\n\n");
            send_status(OAI_EOF);
            tls_cleanup();
            tls_ctx.state = TLS_STATE_IDLE;
            break;

        case TLS_STATE_ERROR:
            printf("[OpenAI] Error cleanup\n");
            send_status(OAI_FAILED);
            tls_cleanup();
            tls_ctx.state = TLS_STATE_IDLE;
            break;
    }
}

// === CORE 0: SSE Frame Parsing ===

// Fast extraction of content field from SSE JSON (lighter than parson)
// Returns allocated string with token content, or NULL if no content
// Caller must free the returned string
// Also sets *is_done if finish_reason detected
static char* parse_sse_frame(const char* json_payload, bool* is_done)
{
    *is_done = false;

    if (!json_payload || json_payload[0] == '\0')
        return NULL;

    // Check for finish_reason (stream complete)
    const char* finish = strstr(json_payload, "\"finish_reason\":");
    if (finish)
    {
        finish += 16; // Skip past "finish_reason":
        while (*finish == ' ' || *finish == '\t')
            finish++;

        // Check if it's not null
        if (strncmp(finish, "null", 4) != 0 && *finish != 'n')
        {
            *is_done = true;
            return NULL;
        }
    }

    // Fast search for "content":" in the JSON
    const char* content_start = strstr(json_payload, "\"content\":\"");
    if (!content_start)
        return NULL; // No content field

    content_start += 11; // Skip past "content":"

    // Find the ending quote (handle escaped quotes)
    const char* p = content_start;
    size_t len = 0;
    while (*p && len < 1024) // Safety limit
    {
        if (*p == '\\' && *(p + 1))
        {
            p += 2; // Skip escape sequence
            len += 2;
        }
        else if (*p == '"')
        {
            break; // Found end
        }
        else
        {
            p++;
            len++;
        }
    }

    if (len == 0)
        return NULL;

    // Allocate and copy content
    char* result = (char*)malloc(len + 1);
    if (!result)
        return NULL;

    memcpy(result, content_start, len);
    result[len] = '\0';

    // Decode common JSON escapes in-place
    size_t j = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (result[i] == '\\' && i + 1 < len)
        {
            switch (result[i + 1])
            {
                case 'n':
                    result[j++] = '\n';
                    i++;
                    break;
                case 'r':
                    result[j++] = '\r';
                    i++;
                    break;
                case 't':
                    result[j++] = '\t';
                    i++;
                    break;
                case '"':
                    result[j++] = '"';
                    i++;
                    break;
                case '\\':
                    result[j++] = '\\';
                    i++;
                    break;
                default:
                    result[j++] = result[i]; // Keep backslash for unknown escapes
                    break;
            }
        }
        else
        {
            result[j++] = result[i];
        }
    }
    result[j] = '\0';

    // Print token to console
    if (j > 0)
    {
        printf("%s", result);
        fflush(stdout);
    }

    return result;
}

// === CORE 0: Port Handlers ===

void openai_io_init(void)
{
    queue_init(&outbound_queue, sizeof(openai_request_t), OUTBOUND_QUEUE_SIZE);
    queue_init(&body_chunk_queue, sizeof(openai_response_t), BODY_CHUNK_QUEUE_SIZE); // For body streaming
    queue_init(&inbound_queue, sizeof(openai_response_t), INBOUND_QUEUE_SIZE);

    memset(&port_state, 0, sizeof(port_state));
    memset(&tls_ctx, 0, sizeof(tls_ctx));
    port_state.status = OAI_EOF;

    printf("[OpenAI] Initialized (mbedtls TLS)\n");
}

size_t openai_output(uint8_t port, uint8_t data, char* buffer, size_t buffer_length)
{
    (void)buffer;
    (void)buffer_length;

    switch (port)
    {
        case OAI_RESET_REQUEST:
            DBG_PRINT("[OAI:PORT] OUT 120: Reset request state\n");
            memset(port_state.chunk_buffer, 0, REQUEST_CHUNK_SIZE);
            port_state.chunk_index = 0;
            port_state.content_length = 0;
            port_state.content_length_lo = 0;
            port_state.content_length_ready = false;
            port_state.request_pending = false;
            port_state.body_complete = false;
            port_state.response_bytes_available = 0;
            port_state.response_position = 0;
            port_state.response_complete = false;
            port_state.status = OAI_WAITING;

            // Drain ALL queues to ensure clean state
            {
                openai_response_t dummy_resp;
                openai_request_t dummy_req;
                while (queue_try_remove(&inbound_queue, &dummy_resp))
                {
                }
                while (queue_try_remove(&body_chunk_queue, &dummy_resp))
                {
                }
                while (queue_try_remove(&outbound_queue, &dummy_req))
                {
                }
            }
            break;

        case OAI_ADD_BYTE:
            // Stream body data - accumulate in chunk buffer, send when full or null terminator
            if (data != 0)
            {
                if (port_state.chunk_index < REQUEST_CHUNK_SIZE - 1)
                {
                    port_state.chunk_buffer[port_state.chunk_index++] = (char)data;
                }

                // If chunk buffer is full, queue it immediately
                if (port_state.chunk_index >= REQUEST_CHUNK_SIZE - 1)
                {
                    // Queue the chunk to Core 1
                    openai_response_t chunk; // Reuse response struct for body chunks
                    memset(&chunk, 0, sizeof(chunk));
                    size_t copy_len =
                        port_state.chunk_index < RESPONSE_CHUNK_SIZE ? port_state.chunk_index : RESPONSE_CHUNK_SIZE - 1;
                    memcpy(chunk.data, port_state.chunk_buffer, copy_len);
                    chunk.len = copy_len;
                    chunk.status = OAI_DATA_READY;

                    if (!queue_try_add(&body_chunk_queue, &chunk))
                    {
                        printf("[OpenAI:ERROR] Failed to queue body chunk (queue full)\n");
                        // Mark error - body incomplete
                        port_state.status = OAI_FAILED;
                        return 0;
                    }
                    DBG_PRINT("[OAI:PORT] OUT 121: Queued chunk %zu bytes\n", copy_len);

                    // Reset chunk buffer
                    port_state.chunk_index = 0;
                }
            }
            else
            {
                // Null terminator - send any remaining data and mark body complete
                if (port_state.chunk_index > 0)
                {
                    openai_response_t chunk;
                    memset(&chunk, 0, sizeof(chunk));
                    size_t copy_len =
                        port_state.chunk_index < RESPONSE_CHUNK_SIZE ? port_state.chunk_index : RESPONSE_CHUNK_SIZE - 1;
                    memcpy(chunk.data, port_state.chunk_buffer, copy_len);
                    chunk.len = copy_len;
                    chunk.status = OAI_DATA_READY;

                    if (!queue_try_add(&body_chunk_queue, &chunk))
                    {
                        printf("[OpenAI:ERROR] Failed to queue final body chunk\n");
                        port_state.status = OAI_FAILED;
                        return 0;
                    }
                    DBG_PRINT("[OAI:PORT] OUT 121: Queued final chunk %zu bytes\n", copy_len);
                    port_state.chunk_index = 0;
                }

                // Signal end of body
                openai_response_t end_marker;
                memset(&end_marker, 0, sizeof(end_marker));
                end_marker.len = 0;
                end_marker.status = OAI_EOF; // Use EOF to signal end of body
                if (!queue_try_add(&body_chunk_queue, &end_marker))
                {
                    printf("[OpenAI:ERROR] Failed to queue EOF marker\n");
                    port_state.status = OAI_FAILED;
                    return 0;
                }

                port_state.body_complete = true;
                DBG_PRINT("[OAI:PORT] OUT 121: Body complete, queued end marker\n");
            }
            break;

        case OAI_RESET_RESPONSE:
        {
            DBG_PRINT("[OAI:PORT] OUT 122: Reset response buffer\n");
            port_state.response_bytes_available = 0;
            port_state.response_position = 0;
            port_state.response_complete = false;
            port_state.status = OAI_WAITING;

            openai_response_t dummy;
            while (queue_try_remove(&inbound_queue, &dummy))
            {
            }
            break;
        }

        case OAI_SET_LEN_LO:
            port_state.content_length_lo = data;
            DBG_PRINT("[OAI:PORT] OUT 126: Content length low byte = %u\n", data);
            break;

        case OAI_SET_LEN_HI:
            port_state.content_length = port_state.content_length_lo | ((uint16_t)data << 8);
            // Sanity check: reject unreasonably large content lengths
            if (port_state.content_length > 32768) // Max 32KB
            {
                printf("[OpenAI:ERROR] Content length %u exceeds maximum (32KB)\n", port_state.content_length);
                port_state.content_length = 0;
                port_state.content_length_ready = false;
            }
            else
            {
                port_state.content_length_ready = true;
            }
            DBG_PRINT("[OAI:PORT] OUT 127: Content length = %u\n", port_state.content_length);
            break;
    }

    return 0;
}

uint8_t openai_input(uint8_t port)
{
    uint8_t retVal = 0;

    switch (port)
    {
        case OAI_RESET_REQUEST:
            // Trigger API call - send content_length to Core 1 to start request
            DBG_PRINT("[OAI:PORT] IN 120: Trigger API, content_length_ready=%d, len=%u\n",
                      port_state.content_length_ready, port_state.content_length);
            if (port_state.content_length_ready && port_state.content_length > 0)
            {
                openai_request_t request;
                memset(&request, 0, sizeof(request));
                request.content_length = port_state.content_length;
                request.abort = false;

                DBG_PRINT("[OAI:PORT] Queueing request start to Core 1, content_length=%zu\n", request.content_length);
                if (queue_try_add(&outbound_queue, &request))
                {
                    port_state.request_pending = true; // Now waiting for body chunks
                    port_state.status = OAI_WAITING;
                    retVal = 1;
                    DBG_PRINT("[OAI:PORT] Request start queued!\n");
                }
                else
                {
                    DBG_PRINT("[OAI:PORT] FAILED to queue request start!\n");
                }
            }
            else
            {
                DBG_PRINT("[OAI:PORT] Content length not ready, returning 0\n");
            }
            break;

        case OAI_GET_LEN_LO:
            retVal = (uint8_t)(port_state.content_length & 0xFF);
            break;

        case OAI_GET_LEN_HI:
            retVal = (uint8_t)((port_state.content_length >> 8) & 0xFF);
            break;

        case OAI_GET_STATUS:
        {
            // Flow Control: Check if body chunk queue is full
            // If full, tell Altair to stop sending request data (return OAI_BUSY)
            if (queue_is_full(&body_chunk_queue))
            {
                // DBG_PRINT("[OAI:PORT] Body chunk queue full, returning BUSY\n");
                return OAI_BUSY;
            }

            if (port_state.response_bytes_available == 0)
            {
                openai_response_t response;
                if (queue_try_remove(&inbound_queue, &response))
                {
                    if (response.status == OAI_EOF || response.status == OAI_FAILED)
                    {
                        // Status-only message (no data to parse)
                        port_state.response_complete = true;
                        port_state.status = response.status;
                    }
                    else if (response.status == OAI_DATA_READY && response.len > 0)
                    {
                        // Raw JSON payload from Core 1 - parse it here on Core 0
                        bool is_done = false;
                        char* token = parse_sse_frame(response.data, &is_done);

                        if (is_done)
                        {
                            port_state.response_complete = true;
                            port_state.status = OAI_EOF;
                        }
                        else if (token)
                        {
                            // Copy extracted token to response buffer
                            size_t token_len = strlen(token);
                            if (token_len > RESPONSE_CHUNK_SIZE - 1)
                                token_len = RESPONSE_CHUNK_SIZE - 1;

                            memcpy(port_state.response_buffer, token, token_len);
                            port_state.response_buffer[token_len] = '\0';
                            port_state.response_bytes_available = token_len;
                            port_state.response_position = 0;
                            port_state.status = OAI_DATA_READY;

                            free(token);
                        }
                        else
                        {
                            // No content in this frame (e.g., role-only delta)
                            // Keep waiting for next frame
                            DBG_PRINT("[OAI:PARSE] Frame had no content, waiting for next\n");
                            port_state.status = OAI_WAITING;
                        }
                    }
                }
            }

            if (port_state.response_bytes_available > 0 &&
                port_state.response_position < port_state.response_bytes_available)
                retVal = OAI_DATA_READY;
            else if (port_state.response_complete)
                retVal = OAI_EOF;
            else
                retVal = OAI_WAITING;
            break;
        }

        case OAI_GET_BYTE:
        {
            if (port_state.response_bytes_available > 0 &&
                port_state.response_position < port_state.response_bytes_available)
            {
                retVal = (uint8_t)port_state.response_buffer[port_state.response_position++];

                if (port_state.response_position >= port_state.response_bytes_available)
                {
                    openai_response_t response;
                    if (queue_try_remove(&inbound_queue, &response))
                    {
                        if (response.status == OAI_EOF || response.status == OAI_FAILED)
                        {
                            port_state.response_complete = true;
                            port_state.status = response.status;
                            port_state.response_bytes_available = 0;
                            port_state.response_position = 0;
                        }
                        else if (response.status == OAI_DATA_READY && response.len > 0)
                        {
                            // Parse JSON and extract token
                            bool is_done = false;
                            char* token = parse_sse_frame(response.data, &is_done);

                            if (is_done)
                            {
                                port_state.response_complete = true;
                                port_state.status = OAI_EOF;
                                port_state.response_bytes_available = 0;
                                port_state.response_position = 0;
                            }
                            else if (token)
                            {
                                size_t token_len = strlen(token);
                                if (token_len > RESPONSE_CHUNK_SIZE - 1)
                                    token_len = RESPONSE_CHUNK_SIZE - 1;

                                memcpy(port_state.response_buffer, token, token_len);
                                port_state.response_buffer[token_len] = '\0';
                                port_state.response_bytes_available = token_len;
                                port_state.response_position = 0;
                                port_state.status = OAI_DATA_READY;

                                free(token);
                            }
                            else
                            {
                                // No content - waiting for next
                                port_state.response_bytes_available = 0;
                                port_state.response_position = 0;
                                port_state.status = OAI_WAITING;
                            }
                        }
                    }
                    else
                    {
                        port_state.response_bytes_available = 0;
                        port_state.response_position = 0;

                        if (port_state.status == OAI_DATA_READY && !port_state.response_complete)
                            port_state.status = OAI_WAITING;
                    }
                }
            }
            break;
        }

        case OAI_IS_COMPLETE:
            retVal = port_state.response_complete ? 1 : 0;
            break;
    }

    return retVal;
}

void openai_poll(void)
{
    static uint32_t poll_count = 0;
    static absolute_time_t last_debug = {0};

    poll_count++;

    // Print debug every 5 seconds
    if (absolute_time_diff_us(last_debug, get_absolute_time()) > 5000000)
    {
        last_debug = get_absolute_time();
    }

    if (tls_ctx.state != TLS_STATE_IDLE)
    {
        poll_tls_state_machine();
    }

    if (tls_ctx.state == TLS_STATE_IDLE)
    {
        openai_request_t request;
        if (queue_try_remove(&outbound_queue, &request))
        {
            DBG_PRINT("[OAI:POLL] Got request start from queue, content_length=%zu\n", request.content_length);

            if (request.abort)
            {
                DBG_PRINT("[OAI:POLL] Abort request, draining queues\n");
                openai_response_t dummy;
                while (queue_try_remove(&inbound_queue, &dummy))
                {
                }
                return;
            }

            printf("[OpenAI] Starting request, content_length=%zu\n", request.content_length);

            if (!start_request(request.content_length))
            {
                send_status(OAI_FAILED);
            }
        }
    }
}

#else // !CYW43_WL_GPIO_LED_PIN

#include <stddef.h>
#include <stdint.h>

void openai_io_init(void) {}

size_t openai_output(uint8_t port, uint8_t data, char* buffer, size_t buffer_length)
{
    (void)port;
    (void)data;
    (void)buffer;
    (void)buffer_length;
    return 0;
}

uint8_t openai_input(uint8_t port)
{
    (void)port;
    return 0;
}

void openai_poll(void) {}

#endif // CYW43_WL_GPIO_LED_PIN
