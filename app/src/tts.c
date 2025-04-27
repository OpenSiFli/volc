/**
  ******************************************************************************
  * @file   tts.c
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

#if PKG_USING_LIBHELIX
    #include "mp3dec.h"
#else
#error "should config PKG_USING_LIBHELIX"
#endif

#define MAX_WSOCK_HDR_LEN 512
#define MAX_AUDIO_DATA_LEN (4096 * 2)


#define TTS_HOST            "ai-gateway.vei.volces.com"
#define TTS_WSPATH          "/v1/realtime?model=doubao-tts"

// Please use your own tts token, applied in https://console.volcengine.com/vei/aigateway/tokens-list
#define TTS_TOKEN           "sk-e1fxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

#define MP3_MAIN_BUFFER_SIZE    (MAINBUF_SIZE * 4)
#define TTS_EVENT_DECODE        (1 << 0)

#define TTS_EVENT_ALL           (TTS_EVENT_DECODE)

typedef struct
{
    rt_thread_t     thread;
    struct rt_ringbuffer *rb_mp3;
    rt_event_t      event;
    uint32_t        sample_rate;
    uint8_t         *main_ptr;
    uint32_t        main_left;
    HMP3Decoder     decode_handle;
    uint8_t         base64_out[MAX_AUDIO_DATA_LEN];
    uint8_t         main_buf[MP3_MAIN_BUFFER_SIZE];
    uint8_t         decode_out[sizeof(short) * MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];
    audio_client_t  speaker;

    uint32_t        event_id;
    wsock_state_t   clnt;
    rt_sem_t        sem;
    uint8_t         is_connected;
    uint8_t         is_end;
    uint8_t         is_exit;
} tts_ws_t;

#if defined(__CC_ARM) || defined(__CLANG_ARM)
    L2_RET_BSS_SECT_BEGIN(g_tts_ws)
    static tts_ws_t g_tts_ws;
    L2_RET_BSS_SECT_END
#else
    static  tts_ws_t g_tts_ws L2_RET_BSS_SECT(g_tts_ws);
#endif


static int audio_callback_func(audio_server_callback_cmt_t cmd, void *callback_userdata, uint32_t reserved)
{
    tts_ws_t *thiz = callback_userdata;
    if (cmd == as_callback_cmd_cache_half_empty || cmd == as_callback_cmd_cache_empty )
    {
        //rt_event_send(thiz->event, TTS_EVENT_DECODE);
    }
    return 0;
}

static void speaker_on(tts_ws_t *thiz)
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
        pa.write_cache_size = 4096;
        thiz->speaker = audio_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX, &pa, audio_callback_func, thiz);
    }
}
static void speaker_off(tts_ws_t *thiz)
{
    if (thiz->speaker)
    {
        audio_close(thiz->speaker);
        thiz->speaker = NULL;
    }
}
void parse_response(const u8_t *data, u16_t len);

static err_t my_wsapp_fn(int code, char *buf, size_t len)
{
    if (code == WS_CONNECT)
    {
        int status = (uint16_t)(uint32_t)buf;
        if (status == 101)  // wss setup success
        {
            rt_sem_release(g_tts_ws.sem);
            g_tts_ws.is_connected = 1;
        }
    }
    else if (code == WS_DISCONNECT)
    {
        rt_kprintf("WebSocket closed\n");
        g_tts_ws.is_connected = 0;
        rt_sem_release(g_tts_ws.sem);
    }
    else if (code == WS_TEXT)
    {
        rt_kprintf("Got Text:%d", len);
        parse_response(buf, len);
    }
    else
    {
        RT_ASSERT(0);
    }
    return 0;
}

static void xz_button_event_handler(int32_t pin, button_action_t action)
{
    rt_kprintf("button(%d) %d:", pin, action);

    if (action == BUTTON_PRESSED)
    {

    }
    else if (action == BUTTON_RELEASED)
    {

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
static void audio_write_and_wait(tts_ws_t *thiz, uint8_t *data, uint32_t data_len)
{
    int ret;
    while (!thiz->is_exit)
    {
        ret = audio_write(thiz->speaker, data, data_len);
        if (ret)
        {
            break;
        }

        rt_thread_mdelay(10);
    }
}
static void thread_entry(void *p)
{
    int err;
    MP3FrameInfo mp3FrameInfo;
    tts_ws_t *thiz = &g_tts_ws;
    while (!thiz->is_exit)
    {
        rt_uint32_t evt = 0;
        rt_event_recv(thiz->event, TTS_EVENT_ALL, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_FOREVER, &evt);
        if (evt & TTS_EVENT_DECODE)
        {
            if (thiz->main_left < MAINBUF_SIZE)
            {
                if (thiz->main_ptr != &thiz->main_buf[0])
                {
                    memcpy(&thiz->main_buf[0], thiz->main_ptr, thiz->main_left);
                    thiz->main_ptr = &thiz->main_buf[0];
                }
                rt_size_t readed = rt_ringbuffer_get(thiz->rb_mp3, &thiz->main_buf[thiz->main_left], MP3_MAIN_BUFFER_SIZE - thiz->main_left);
                thiz->main_left += readed;
                if (!readed)
                {
                    if (!thiz->is_end)
                    {
                        continue;
                    }
                }
            }
            int offset = MP3FindSyncWord(thiz->main_ptr, thiz->main_left);
            if (offset >= 0)
            {
                thiz->main_ptr += offset;
                thiz->main_left -= offset;

                int err = MP3Decode(thiz->decode_handle, &thiz->main_ptr, &thiz->main_left, (short *)thiz->decode_out, 0);
                if (err)
                {
                    rt_kprintf("mp3 decode err=%d\n", err);
                    continue;
                }
            }
            else
            {
                if (thiz->is_end)
                {
                    break;
                }
            }
            MP3GetLastFrameInfo(thiz->decode_handle, &mp3FrameInfo);
            //rt_kprintf("samplereate=%d ch=%d\n", mp3FrameInfo.samprate, mp3FrameInfo.nChans);
            audio_write_and_wait(thiz, thiz->decode_out, mp3FrameInfo.outputSamps * sizeof(uint16_t));
        }
    }

    rt_thread_mdelay(200);

    speaker_off(thiz);
    thiz->is_end = 2;
}

static void xz_ws_audio_init( tts_ws_t *thiz)
{
    rt_kprintf("xz_audio_init\n");
    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, 15);

    thiz->event = rt_event_create("tts", RT_IPC_FLAG_FIFO);
    RT_ASSERT(thiz->event);
    thiz->rb_mp3 = rt_ringbuffer_create(10*1024);
    RT_ASSERT(thiz->rb_mp3);
    thiz->main_left = 0;
    thiz->is_end = 0;
    thiz->main_ptr = &thiz->main_buf[0];
    thiz->is_exit = 0;

    speaker_on(thiz);

    thiz->thread = rt_thread_create("tts",
                             thread_entry,
                             NULL,
                             2048,
                             RT_THREAD_PRIORITY_MIDDLE + RT_THREAD_PRIORITY_HIGHER,
                             RT_THREAD_TICK_DEFAULT);
    RT_ASSERT(thiz->thread);
    rt_thread_startup(thiz->thread);

    if (!thiz->decode_handle)
    {
        thiz->decode_handle = MP3InitDecoder();
        RT_ASSERT(thiz->decode_handle);
    }
    xz_button_init();
}

static char *my_json_string(cJSON *json, char *key)
{
    return cJSON_GetObjectItem(json, key)->valuestring;
}

void parse_response(const u8_t *data, u16_t len)
{
    cJSON *item = NULL;
    cJSON *root = NULL;
    tts_ws_t *thiz = &g_tts_ws;
    //rt_kputs(data);
    //rt_kputs("\r\n");
    root = cJSON_Parse(data);   /*json_data 为MQTT的原始数据*/
    if (!root)
    {
        rt_kprintf("Error before: [%s]\n", cJSON_GetErrorPtr());
        return;
    }

    char *type = my_json_string(root, "type");
    if (strcmp(type, "tts_session.updated") == 0)
    {
        item = cJSON_GetObjectItem(root, "sesson");
        thiz->sample_rate = atoi(my_json_string(item,"output_audio_rate"));
        rt_sem_release(thiz->sem);
        xz_ws_audio_init(thiz);
    }
    else if (strcmp(type, "response.audio.done") == 0)
    {
        thiz->is_end = 1;
        rt_sem_release(thiz->sem);
        rt_kprintf("session ended\n");
    }
    else if (strcmp(type, "response.audio.delta") == 0)
    {
        char *delta = my_json_string(root, "delta");
        int size=0;

        if (ERR_OK==mbedtls_base64_decode(&thiz->base64_out[0], MAX_AUDIO_DATA_LEN, &size,delta,strlen(delta)))
        {
            speaker_on(thiz);

            while (!thiz->is_exit)
            {
                if (rt_ringbuffer_space_len(thiz->rb_mp3) >= size)
                {
                    rt_ringbuffer_put(thiz->rb_mp3, thiz->base64_out, size);
                    break;
                }
                else
                {
                    rt_thread_mdelay(5);
                }
            }
        }
        else
        {
            rt_kprintf("base64 buf too small\r\n");
            RT_ASSERT(0);
        }
    }
    else
    {
        rt_kprintf("Unkown type: %s\n", type);
    }
    cJSON_Delete(root);
}

static const char *config_message =
"{"
    "\"type\": \"tts_session.update\","
    "\"session\": {"
        "\"voice\":\"zh_female_kailangjiejie_moon_bigtts\","
        "\"output_audio_format\": \"mp3\","
        "\"output_audio_sample_rate\": 16000 ,"
        "\"text_to_speech\": {"
          "\"model\": \"doubao-tts\" "
        "}"
    "}"
"}" ;


static const char *tts_req_fmt =
"{"
    "\"event_id\": \"event_%08d\", "
    "\"type\": \"input_text.append\","
    "\"delta\": \"%s\" "
"}";

static const char input_done[] = "{\"type\": \"input_text.done\"}";

static char tts_request[512];

void send_tts_request(char * text)
{
    tts_request[sizeof(tts_request) - 1] = '#';
    rt_snprintf(tts_request, sizeof(tts_request), tts_req_fmt, g_tts_ws.event_id++, text);
    RT_ASSERT(tts_request[sizeof(tts_request) - 1] == '#');
    rt_kprintf("Web socket write tts request %s\r\n", tts_request);

    wsock_write(&g_tts_ws.clnt, tts_request, strlen(tts_request),OPCODE_TEXT);
    wsock_write(&g_tts_ws.clnt, input_done, strlen(input_done),OPCODE_TEXT);
}

void tts(int argc, char **argv)
{
    tts_ws_t *thiz = &g_tts_ws;
    err_t err;

    memset(thiz, 0, sizeof(tts_ws_t));

    if (thiz->sem == NULL)
        thiz->sem = rt_sem_create("xz_ws", 0, RT_IPC_FLAG_FIFO);

    wsock_init(&thiz->clnt, 1, 1, my_wsapp_fn);
    err = wsock_connect(&thiz->clnt, MAX_WSOCK_HDR_LEN, TTS_HOST, TTS_WSPATH,
                        LWIP_IANA_PORT_HTTPS, TTS_TOKEN, NULL,
                        "Content-Type: application/json\r\n");
    rt_kprintf("Web socket connection %d\r\n", err);
    if (err == 0)
    {
        if (RT_EOK == rt_sem_take(thiz->sem, 5000))
        {
            if (thiz->is_connected)
            {
                rt_kprintf("Web socket write config %s\r\n", config_message);
                err = wsock_write(&thiz->clnt, config_message, strlen(config_message), OPCODE_TEXT);
                if (ERR_OK==err && rt_sem_take(thiz->sem, 5000)==RT_EOK) {
                    LOCK_TCPIP_CORE();
                    send_tts_request(argv[1]);
                    UNLOCK_TCPIP_CORE();
                }
                while (1)
                {
                    if (thiz->is_exit || thiz->is_end == 2)
                    {
                        rt_kprintf("Finish TTS exit =%d end=%d", thiz->is_exit, thiz->is_end);
                        rt_kprintf("Web socket disconnected\r\n");
                        wsock_close(&thiz->clnt, WSOCK_RESULT_OK,ERR_OK);
                        return;
                    }

                    if (RT_EOK==rt_sem_take(g_tts_ws.sem, 3000))
                        rt_kprintf("is_end =%d", thiz->is_end);
                    else
                        rt_kprintf("wait end=%d", thiz->is_end);

                    continue;
                }
            }
            else
            {
                rt_kprintf("Web socket disconnected\r\n");
                wsock_close(&thiz->clnt, WSOCK_RESULT_OK,ERR_OK);
            }
        }
        else
        {
            rt_kprintf("Web socket connected timeout\r\n");
        }

    }
}
MSH_CMD_EXPORT(tts, Text to speech)

/************************ (C) COPYRIGHT Sifli Technology *******END OF FILE****/

