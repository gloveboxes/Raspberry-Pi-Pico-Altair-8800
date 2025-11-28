#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*ws_receive_cb_t)(const uint8_t *payload, size_t payload_len, void *user_data);
typedef size_t (*ws_output_cb_t)(uint8_t *buffer, size_t max_len, void *user_data);

typedef struct
{
    ws_receive_cb_t on_receive;
    ws_output_cb_t on_output;
    void *user_data;
} ws_callbacks_t;

#ifdef __cplusplus
extern "C"
{
#endif

void ws_init(const ws_callbacks_t *callbacks);
bool ws_start(void);
bool ws_is_running(void);
void ws_poll(void);

#ifdef __cplusplus
}
#endif
