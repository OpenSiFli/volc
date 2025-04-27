/**
  ******************************************************************************
  * @file   chat.c
  * @author Sifli software development team
  ******************************************************************************
*/
/**
 * @attention
 * Copyright (c) 2024 - 2025,  Sifli Technology
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Sifli integrated circuit
 *    in a product or a software update for such product, must reproduce the above
 *    copyright notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Sifli nor the names of its contributors may be used to endorse
 *    or promote products derived from this software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Sifli integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY SIFLI TECHNOLOGY "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SIFLI TECHNOLOGY OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include <rtthread.h>
#include "lwip/api.h"
#include "lwip/apps/websocket_client.h"
#include "lwip/apps/mqtt_priv.h"
#include "lwip/apps/mqtt.h"
#include "lwip/tcpip.h"
#include "mbedtls/base64.h"
#include "bf0_hal.h"
#include "bts2_global.h"
#include "bts2_app_pan.h"
#include <cJSON.h>
#include "button.h"
#include "audio_server.h"
#include "mem_section.h"

#define MAX_WSOCK_HDR_LEN 512
#define MAX_AUDIO_DATA_LEN 4096

#define CHAT_MIC_FRAME_LEN          (320)  //100ms
#define CHAT_FRAME_ENCODE_LEN       (CHAT_MIC_FRAME_LEN * 4 / 3 + 128)   //buffer.append

#define CHAT_HOST            "ai-gateway.vei.volces.com"
#define CHAT_WSPATH          "/v1/realtime?model=AG-voice-chat-agent"

// Please use your own tts token, applied in https://console.volcengine.com/vei/aigateway/tokens-list
#define CHAT_TOKEN           "sk-555796f8bf044e93a28f9a511a0175e7ec9zzkk9h5s16yj0"

#define CHAT_EVENT_MIC_RX         (1 << 0)
#define CHAT_EVENT_SPK_TX         (1 << 1)
#define CHAT_EVENT_DOWNLINK       (1 << 2)
#define CHAT_EVENT_MIC_CLOSE      (1 << 3)

#define CHAT_EVENT_ALL            (CHAT_EVENT_MIC_RX | CHAT_EVENT_SPK_TX | CHAT_EVENT_DOWNLINK|CHAT_EVENT_MIC_CLOSE)

typedef enum
{
    CT_CONNECTING,
    CT_SESSION_CREATED,
    CT_SESSION_UPDATED,
    CT_BUFFER_APPEND,
    CT_RESPONSE_CREATE,
    CT_RESPONSE_DONE,
} chat_state;

typedef struct
{
    rt_event_t              event;
    struct rt_ringbuffer    *rb_mic;
    uint32_t                mic_rx_count;

    rt_thread_t     thread;
    audio_client_t  speaker;
    audio_client_t  mic;
    uint32_t        sample_rate;
    uint32_t        frame_duration;
    uint32_t        event_id;
    wsock_state_t   clnt;
    rt_sem_t        sem;
    chat_state      state;
    uint8_t         is_connected;
    uint8_t         is_exit;
    uint8_t         text[WSMSG_MAXSIZE];
    uint8_t         encode_in[CHAT_MIC_FRAME_LEN];
    uint8_t         encode_out[CHAT_FRAME_ENCODE_LEN]; //base64
} chat_ws_t;

#if defined(__CC_ARM) || defined(__CLANG_ARM)
    L2_RET_BSS_SECT_BEGIN(g_thiz)
    static  chat_ws_t g_thiz;
    L2_RET_BSS_SECT_END
#else
    static  chat_ws_t g_thiz L2_RET_BSS_SECT(g_thiz);
#endif

static void parse_response(const u8_t *data, u16_t len);

static const char buffer_append[] = "{\"type\": \"input_audio_buffer.append\",\"audio\" : \"";


static int mic_callback(audio_server_callback_cmt_t cmd, void *callback_userdata, uint32_t reserved)
{
    //this was called every 10ms
    chat_ws_t *thiz = &g_thiz;

    if (cmd == as_callback_cmd_data_coming)
    {
        audio_server_coming_data_t *p = (audio_server_coming_data_t *)reserved;
        rt_ringbuffer_put(thiz->rb_mic, p->data, p->data_len);
        thiz->mic_rx_count += 320;

        if (thiz->mic_rx_count >= CHAT_MIC_FRAME_LEN)
        {
            thiz->mic_rx_count = 0;
            rt_event_send(thiz->event, CHAT_EVENT_MIC_RX);
        }
    }
    return 0;
}

static void mic_on(chat_ws_t *thiz)
{
    if (!thiz->mic)
    {
        audio_parameter_t pa = {0};
        pa.write_bits_per_sample = 16;
        pa.write_channnel_num = 1;
        pa.read_bits_per_sample = 16;
        pa.read_channnel_num = 1;
        pa.write_samplerate = 16000;
        pa.read_samplerate = 16000;
        pa.write_cache_size = 30000;
        thiz->mic = audio_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_RX, &pa, mic_callback, NULL);
    }
}
static void mic_off(chat_ws_t *thiz)
{
    if (thiz->mic)
    {
        audio_close(thiz->mic);
        thiz->mic = NULL;
    }
}

static void speaker_on(chat_ws_t *thiz)
{
    if (!thiz->speaker)
    {
        audio_parameter_t pa = {0};
        pa.write_bits_per_sample = 16;
        pa.write_channnel_num = 1;
        pa.read_bits_per_sample = 16;
        pa.read_channnel_num = 1;
        pa.write_samplerate = 16000;
        pa.read_samplerate = 16000;
        pa.write_cache_size = 30000;
        thiz->speaker = audio_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX, &pa, NULL, NULL);
    }
}
static void speaker_off(chat_ws_t *thiz)
{
    if (thiz->speaker)
    {
        audio_close(thiz->speaker);
        thiz->speaker = NULL;
    }
}
static const char buffer_commit[] = "{\"type\": \"input_audio_buffer.commit\"}";
static const char response_create[] = "{\"type\": \"response.create\", \"response\": {\"modalities\": [\"text\", \"audio\"]}}";
static const char response_cancel[] = "{\"type\": \"response.cancel\",}";

static void thread_entry(void *p)
{
    int err;
    chat_ws_t *thiz = &g_thiz;
    while (!thiz->is_exit)
    {
        rt_uint32_t evt = 0;
        rt_event_recv(thiz->event, CHAT_EVENT_ALL, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_FOREVER, &evt);
        if (evt & CHAT_EVENT_MIC_CLOSE)
        {
            evt &= ~CHAT_EVENT_MIC_RX;
            while (rt_ringbuffer_get(thiz->rb_mic, thiz->encode_in, CHAT_MIC_FRAME_LEN) > 0)
            {
                ;
            }
            LOCK_TCPIP_CORE();
            err_t err = wsock_write(&thiz->clnt, buffer_commit, strlen(buffer_commit), OPCODE_TEXT);
            UNLOCK_TCPIP_CORE();
            rt_thread_mdelay(10);
            LOCK_TCPIP_CORE();
            err = wsock_write(&thiz->clnt, response_create, strlen(response_create), OPCODE_TEXT);
            UNLOCK_TCPIP_CORE();
            thiz->state = CT_RESPONSE_CREATE;
        }
        if ((evt & CHAT_EVENT_MIC_RX))
        {
            if (thiz->state == CT_RESPONSE_CREATE)
            {
                LOCK_TCPIP_CORE();
                err_t err = wsock_write(&thiz->clnt, response_cancel, strlen(response_cancel), OPCODE_TEXT);
                UNLOCK_TCPIP_CORE();
                thiz->state = CT_BUFFER_APPEND;
            }

            if (rt_ringbuffer_data_len(thiz->rb_mic) >= CHAT_MIC_FRAME_LEN)
            {
                size_t olen = 0;
                size_t len;
                len = rt_ringbuffer_get(thiz->rb_mic, thiz->encode_in, CHAT_MIC_FRAME_LEN);
                RT_ASSERT(len == CHAT_MIC_FRAME_LEN);
                len = strlen(buffer_append);
                memcpy(thiz->encode_out, buffer_append, len);
                int ret = mbedtls_base64_encode(thiz->encode_out + len, CHAT_FRAME_ENCODE_LEN - len, &olen, thiz->encode_in, CHAT_MIC_FRAME_LEN);
                RT_ASSERT(!ret);
                len += olen;
                strcpy(thiz->encode_out + len, "\"}");
                len += 2;
                RT_ASSERT(WSMSG_MAXSIZE >= len);
                LOCK_TCPIP_CORE();
                err_t err = wsock_write(&thiz->clnt, thiz->encode_out , len, OPCODE_TEXT);
                UNLOCK_TCPIP_CORE();
                rt_kprintf("send audio ret = %d len=%d\n", err, len);

            }
        }
    }
}
static err_t my_wsapp_fn(int code, char *buf, size_t len)
{
    chat_ws_t *thiz = &g_thiz;

    if (code == WS_CONNECT)
    {
        int status = (uint16_t)(uint32_t)buf;
        if (status == 101)  // wss setup success
        {
            thiz->is_connected = 1;
        }
    }
    else if (code == WS_DISCONNECT)
    {
        rt_kprintf("WebSocket closed\n");
        thiz->is_connected = 0;
        rt_sem_release(thiz->sem);
    }
    else if (code == WS_TEXT)
    {
        rt_kprintf("Got Text:%d\n", len);
        RT_ASSERT(len<(WSMSG_MAXSIZE-1));
        memcpy(thiz->text, buf, len);
        thiz->text[len]='\0';
        parse_response(buf, len);
    }
    else
    {
        rt_kprintf("Got Code=%d\n", code);
    }
    return 0;
}

static void xz_button_event_handler(int32_t pin, button_action_t action)
{
    rt_kprintf("button(%d) %d:", pin, action);
    chat_ws_t *thiz = &g_thiz;
    if (action == BUTTON_PRESSED)
    {
        mic_on(thiz);
    }
    else if (action == BUTTON_RELEASED)
    {
        mic_off(thiz);
        rt_event_send(thiz->event, CHAT_EVENT_MIC_CLOSE);
    }
}


static void xz_button_init(void)
{
    static int initialized = 0;

    if (initialized == 0)
    {
        button_cfg_t cfg;
        cfg.pin = BSP_KEY1_PIN;

        cfg.active_state = BSP_KEY1_ACTIVE_HIGH;
        cfg.mode = PIN_MODE_INPUT;
        cfg.button_handler = xz_button_event_handler;
        int32_t id = button_init(&cfg);
        RT_ASSERT(id >= 0);
        RT_ASSERT(SF_EOK == button_enable(id));
        initialized = 1;
    }
}

static void xz_ws_audio_init()
{
    chat_ws_t *thiz = &g_thiz;
    rt_kprintf("chat_audio_init\n");
    thiz->sample_rate = 16000;
    thiz->frame_duration = 100;
    thiz->event = rt_event_create("doubchat", RT_IPC_FLAG_FIFO);
    RT_ASSERT(thiz->event);
    thiz->rb_mic = rt_ringbuffer_create(CHAT_MIC_FRAME_LEN * 2);
    RT_ASSERT(thiz->rb_mic);
    thiz->is_exit = 0;
    thiz->thread = rt_thread_create("doubchat",
                             thread_entry,
                             NULL,
                             4096,
                             RT_THREAD_PRIORITY_MIDDLE + RT_THREAD_PRIORITY_HIGHER,
                             RT_THREAD_TICK_DEFAULT);
    RT_ASSERT(thiz->thread);
    rt_thread_startup(thiz->thread);

    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, 15);

    xz_button_init();
}

static char *my_json_string(cJSON *json, char *key)
{
    return cJSON_GetObjectItem(json, key)->valuestring;
}

static void parse_response(const u8_t *data, u16_t len)
{
    cJSON *item = NULL;
    cJSON *root = NULL;
    chat_ws_t *thiz = &g_thiz;
    rt_kputs(data);
    rt_kputs("--parse_response--\r\n");
    root = cJSON_Parse(data);
    if (!root)
    {
        rt_kprintf("Error before: [%s]\n", cJSON_GetErrorPtr());
        return;
    }

    char *type = my_json_string(root, "type");

    if (strcmp(type, "session.created") == 0)
    {
        rt_kprintf("session.created\n");
        thiz->state = CT_SESSION_CREATED;
        rt_sem_release(thiz->sem);
    }
    else if (strcmp(type, "session.updated") == 0)
    {
        rt_kprintf("session.updated\n");
        thiz->state = CT_SESSION_UPDATED;
        rt_sem_release(thiz->sem);
        xz_ws_audio_init();
    }
    else if (strcmp(type, "response.created") == 0)
    {
    }
    else if (strcmp(type, "response.audio.delta") == 0)
    {
        rt_kprintf("response.audio.delta\n");
        char *delta = my_json_string(root, "delta");
        static uint8_t audio_data[MAX_AUDIO_DATA_LEN];
        int size=0;
        if (ERR_OK==mbedtls_base64_decode(audio_data,MAX_AUDIO_DATA_LEN,&size,delta,strlen(delta)))
        {
            rt_kprintf("Audio data:%d\r\n",size);
            audio_write(thiz->speaker, audio_data, size);
        }
    }
    else if (strcmp(type, "response.audio_transcript.delta") == 0)
    {
        char *delta = my_json_string(root, "delta");
        rt_kputs("\r\n");
        rt_kputs(delta);
        rt_kputs("\r\n");
    }
    else if (strcmp(type, "response.done") == 0)
    {
        thiz->state =CT_RESPONSE_DONE;
        speaker_off(thiz);
    }
    else
    {
        rt_kprintf("skip type:%s\n", type);
    }
    cJSON_Delete(root);
}

static const char *session_update =
"{"
    "\"type\": \"session.update\","
    "\"session\": {"
        "\"modalities\": [\"text\", \"audio\"],"
        "\"voice\":\"zh_female_tianmeixiaoyuan_moon_bigtts\","
        "\"input_audio_format\": \"pcm16\","
        "\"output_audio_format\": \"pcm16\""
    "}"
"}" ;

static void chat(int argc, char **argv)
{

    err_t err;
    chat_ws_t *thiz = &g_thiz;
    memset(thiz, 0, sizeof(chat_ws_t));

    if (thiz->sem == NULL)
        thiz->sem = rt_sem_create("xz_ws", 0, RT_IPC_FLAG_FIFO);

    wsock_init(&thiz->clnt, 1, 1, my_wsapp_fn);

    thiz->state = CT_CONNECTING;

    err = wsock_connect(&thiz->clnt, MAX_WSOCK_HDR_LEN, CHAT_HOST, CHAT_WSPATH,
                        LWIP_IANA_PORT_HTTPS, CHAT_TOKEN, NULL,
                        "Content-Type: application/json\r\n");
    rt_kprintf("Web socket connection %d\r\n", err);
    if (err)
    {
        goto Exit;;
    }
    // 1. wait create
    if (RT_EOK != rt_sem_take(thiz->sem, 30000)
        || thiz->state != CT_SESSION_CREATED)
    {
        rt_kprintf("wait session create fail\n");
        if (thiz->is_connected)
        {
            rt_kprintf("Web socket disconnected\r\n");
            wsock_close(&thiz->clnt, WSOCK_RESULT_OK,ERR_OK);
        }
        goto Exit;;
    }
    // 2. send upate
    rt_kprintf("send update:\r\n");
    rt_kputs(session_update);
    rt_kprintf("\r\n\r\n");

    LOCK_TCPIP_CORE();
    err = wsock_write(&thiz->clnt, session_update, strlen(session_update), OPCODE_TEXT);
    UNLOCK_TCPIP_CORE();

    if (RT_EOK != rt_sem_take(thiz->sem, 20000)
        || thiz->state != CT_SESSION_UPDATED)
    {
        rt_kprintf("wait session upate fail\n");
        wsock_close(&thiz->clnt, WSOCK_RESULT_OK,ERR_OK);
        goto Exit;
    }
    else
    {
        rt_kprintf("\n\nPress Key1 and Talk, release Key1 and Listen\n\n");
        return;
    }
Exit:
    rt_kprintf("\nexit chat\n");
}
MSH_CMD_EXPORT(chat, doubao voice chat)



/************************ (C) COPYRIGHT Sifli Technology *******END OF FILE****/

