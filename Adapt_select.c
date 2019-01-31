/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2018 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
//#include "mp3_decoder.h"
//#include "aac_decoder.h"
#include "audio_hal.h"
#include "audio_player.h"


#define SELECT_MP3_DECODER 1
#define SELECT_AAC_DECODER 2

#if defined SELECT_AAC_DECODER
#include "aac_decoder.h"
    static const char *TAG = "HTTP_SELECT_AAC_EXAMPLE";
    static const char *selected_decoder_name = "aac";
//    static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.aac";
#else
#include "mp3_decoder.h"
    static const char *TAG = "HTTP_SELECT_MP3_EXAMPLE";
    static const char *selected_decoder_name = "mp3";
//    static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.mp3";
#endif 


//static const char *TAG = "AUDIO_PLAYER";

typedef struct audio_player {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t http_stream_reader, i2s_stream_writer, selected_decoder;
    audio_event_iface_handle_t evt;
    audio_hal_handle_t hal;
    bool run;
    bool playing;
    player_event event_handler;
} audio_player_t;


#define DEFAULT_PLAYER_TASK_STACK (4*1024)
#define DEFAULT_PLAYER_TASK_PRIO (5)

static esp_err_t audio_player_send_event(audio_player_handle_t player, player_event_t event)
{
    if (player->event_handler) {
        return player->event_handler(player, event);
    }
    return ESP_OK;
}

static void _audio_player_task(void *pv)
{


    audio_player_handle_t ap = (audio_player_handle_t) pv;

    while (ap->run) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(ap->evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }
     
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                && msg.source == (void *) ap->selected_decoder
                && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
                && (int)msg.data == AEL_STATUS_STATE_RUNNING) {
            audio_player_send_event(ap, PLAYER_EVENT_PLAY);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                && msg.source == (void *) ap->selected_decoder
                && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
                && (int)msg.data == AEL_STATUS_STATE_PAUSED) {
            audio_player_send_event(ap, PLAYER_EVENT_PAUSE);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                && msg.source == (void *) ap->selected_decoder
                && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(ap->selected_decoder, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from %s decoder, sample_rates=%d, bits=%d, ch=%d",
                     selected_decoder_name, music_info.sample_rates, music_info.bits, music_info.channels);

            audio_element_setinfo(ap->i2s_stream_writer, &music_info);
            i2s_stream_set_clk(ap->i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                && msg.source == (void *)ap->i2s_stream_writer
                && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
                && (int)msg.data == AEL_STATUS_STATE_STOPPED
                && ap->playing)
        {
            ESP_LOGI(TAG, "Stop pipeline");         
            ESP_LOGW(TAG, "[ * ] Stop event received");         
            audio_player_send_event(ap, PLAYER_EVENT_STOP);
            audio_pipeline_stop(ap->pipeline);
            audio_pipeline_wait_for_stop(ap->pipeline);
            audio_element_reset_state(ap->selected_decoder);
            audio_element_reset_state(ap->i2s_stream_writer);
            audio_pipeline_reset_ringbuffer(ap->pipeline);
            audio_pipeline_reset_items_state(ap->pipeline);
            ap->playing = false;
        }
    }
    vTaskDelete(NULL);
}

audio_player_handle_t audio_player_init(audio_player_config_t *config)
{

    audio_player_handle_t ap = calloc(1, sizeof(audio_player_t));
    AUDIO_MEM_CHECK(TAG, ap, NULL);

    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
    audio_hal_codec_config_t audio_hal_codec_cfg =  AUDIO_HAL_ES8388_DEFAULT();
    ap->hal = audio_hal_init(&audio_hal_codec_cfg, 0);
    AUDIO_MEM_CHECK(TAG, ap->hal, goto _audio_init_failed);
    audio_hal_ctrl_codec(ap->hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    ap->pipeline = audio_pipeline_init(&pipeline_cfg);
    AUDIO_MEM_CHECK(TAG, ap->pipeline, goto _audio_init_failed);
    mem_assert(ap->pipeline);//new

    ESP_LOGI(TAG, "[2.1] Create http stream to read data");
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    ap->http_stream_reader = http_stream_init(&http_cfg);
    AUDIO_MEM_CHECK(TAG, ap->http_stream_reader, goto _audio_init_failed);
    
    ESP_LOGI(TAG, "[2.2] Create %s decoder to decode %s file", selected_decoder_name, selected_decoder_name);

#if defined SELECT_AAC_DECODER
    aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
    ap->selected_decoder = aac_decoder_init(&aac_cfg);
    AUDIO_MEM_CHECK(TAG, ap->selected_decoder, goto _audio_init_failed);
#elif defined SELECT_MP3_DECODER
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    ap->selected_decoder = mp3_decoder_init(&mp3_cfg);
    AUDIO_MEM_CHECK(TAG, ap->selected_decoder, goto _audio_init_failed);
#else  
    wav_decoder_cfg_t wav_cfg = DEFAULT_WAV_DECODER_CONFIG();
    selected_decoder = wav_decoder_init(&wav_cfg);
#endif 
//End new
    
    ESP_LOGI(TAG, "[2.3] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    ap->i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
    audio_pipeline_register(ap->pipeline, ap->http_stream_reader, "http"); 
    audio_pipeline_register(ap->pipeline, ap->selected_decoder,    selected_decoder_name);
    audio_pipeline_register(ap->pipeline, ap->i2s_stream_writer,  "i2s");

    ESP_LOGI(TAG, "[2.5] Link it together http_stream-->%s_decoder-->i2s_stream-->[codec_chip]", selected_decoder_name);
    audio_pipeline_link(ap->pipeline, (const char *[]) {"http", selected_decoder_name, "i2s"}, 3);

    ESP_LOGI(TAG, "[ 3 ] Setup event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    ap->evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[3.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(ap->pipeline, ap->evt);

    ap->run = true;
    ap->playing = false;
    ap->event_handler = config->event_handler;
    int task_stack = config->task_stack ? config->task_stack : DEFAULT_PLAYER_TASK_STACK;
    int task_prio = config->task_prio ? config->task_prio : DEFAULT_PLAYER_TASK_PRIO;
    if (xTaskCreate(_audio_player_task, "player_task", task_stack, ap, task_prio, NULL) != pdTRUE) {
        ESP_LOGE(TAG, "Error creating the Player task");
        goto _audio_init_failed;
    }

    return ap;
_audio_init_failed:
    return NULL;
}

int audio_player_vol(audio_player_handle_t ap, int vol_set)
{
    int vol_get = 0;
    if (vol_set >= 0) {
        audio_hal_set_volume(ap->hal, vol_set);
    }
    audio_hal_get_volume(ap->hal, &vol_get);
    return vol_get;
}

esp_err_t audio_player_stop(audio_player_handle_t ap)
{
    if (ap->playing) {
        audio_pipeline_stop(ap->pipeline);
        audio_pipeline_wait_for_stop(ap->pipeline);
        audio_element_reset_state(ap->selected_decoder); //new
//        audio_element_reset_state(ap->aac_decoder);
        audio_element_reset_state(ap->i2s_stream_writer);
        audio_pipeline_reset_ringbuffer(ap->pipeline);
        audio_pipeline_reset_items_state(ap->pipeline);
        ap->playing = false;
    }
    return ESP_OK;
}

esp_err_t audio_player_play(audio_player_handle_t ap, const char *url)
{
    audio_player_stop(ap);
    if (url) {
        audio_element_set_uri(ap->http_stream_reader, url);
        audio_pipeline_run(ap->pipeline);
        ap->playing = true;
    }
    return ESP_OK;
}

esp_err_t audio_player_pause(audio_player_handle_t ap)
{
    audio_pipeline_pause(ap->pipeline);
    return ESP_OK;
}

esp_err_t audio_player_resume(audio_player_handle_t ap)
{
    audio_pipeline_resume(ap->pipeline);
    return ESP_OK;
}

int audio_player_time_played_seconds(audio_player_handle_t ap)
{
    audio_element_info_t info;
    if (ap == NULL) {
        return 0;
    }
    if (audio_element_getinfo(ap->i2s_stream_writer, &info) != ESP_OK) {
        return 0;
    }
    int time_sec = info.byte_pos / (info.sample_rates * info.channels * info.bits / 8);
    return time_sec;
}

int audio_player_time_totals_seconds(audio_player_handle_t ap)
{
    audio_element_info_t info;
    if (ap == NULL) {
        return 0;
    }
    if (audio_element_getinfo(ap->i2s_stream_writer, &info) != ESP_OK) {
        return 0;
    }
    int time_sec = info.byte_pos / (info.sample_rates * info.channels * info.bits / 8);
    return time_sec;
}

esp_err_t audio_player_deinit(audio_player_handle_t ap)
{
    if (ap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ap->run = false;
    //TODO: Waiting for task finish

    audio_pipeline_unregister(ap->pipeline, ap->http_stream_reader);
    audio_pipeline_unregister(ap->pipeline, ap->i2s_stream_writer);
    audio_pipeline_unregister(ap->pipeline, ap->selected_decoder);


    audio_pipeline_terminate(ap->pipeline);
    /* Terminate the pipeline before removing the listener */
    audio_pipeline_remove_listener(ap->pipeline);
    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(ap->evt);
    /* Release all resources */
    audio_pipeline_deinit(ap->pipeline);
    audio_element_deinit(ap->http_stream_reader);
    audio_element_deinit(ap->i2s_stream_writer);
    audio_element_deinit(ap->selected_decoder);
    audio_hal_deinit(ap->hal, 0);
    free(ap);
    return ESP_OK;
}
