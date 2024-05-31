// SPDX-License-Identifier: GPL-3.0
// Author: Divyanshu-Modi <divyan.m05@gmail.com>

#define pr_fmt "PIXA"

#include "audio_common.h"
#include "audio_mem.h"
#include "audio_pipeline.h"

#include "board.h"
#include "es7210.h"
#include "es8311.h"
#include "esp_heap_caps.h"
#include "esp_vad.h"
#include "esp_wifi.h"
#include "equalizer.h"
#include "filter_resample.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2s_stream.h"
#include "mqtt_client.h"
#include "model_path.h"
#include "nvs_flash.h"
#include "pr_log.h"
#include "raw_stream.h"
#include "sdkconfig.h"
#include "freertos/ringbuf.h"

#define RAW_CFG_BUFFER 4 * 2048
#define PIXA_IP_BUF_MAX 16
#define READ_TASK_BUF 2048 * sizeof(short)
#define PLINK_MAX 3
#define PLINK_PA_MAX 3
#define MQTT_BUFFER_SIZE (65 * 1024)

#define KHZ(hz) (hz * 1000)

#define SPEAKER_I2S_FREQ KHZ(16)
#define MIC_I2S_FREQ 2500

typedef struct {
	audio_element_handle_t i2s;
	audio_element_handle_t rsp;
	audio_element_handle_t raw;
	audio_pipeline_handle_t pipe;
} pixa_device_t;

typedef struct {
	audio_element_handle_t i2s;
	// Doesn't support 11 khz
	// audio_element_handle_t rsp;
	audio_element_handle_t raw;
	audio_element_handle_t eq;
	audio_pipeline_handle_t pipe;
} pixa_pa_device_t;

#define TOTAL_SIZE 256 * 1024
static char *data_buffer = NULL;
static size_t data_buffer_index = 0;

// SSL CERTIFICATE FOR MQTT SERVER
extern const uint8_t ssl_start[] asm("_binary_isrgrootx1_pem_start");
extern const uint8_t ssl_end[] asm("_binary_isrgrootx1_pem_end");

static bool ip_got = false;
bool data_incoming = false;
pixa_device_t *pixa = NULL;
pixa_pa_device_t *pixa_pa = NULL;
esp_mqtt_client_handle_t client;

void mic_init(pixa_device_t *pixa_ai);
void spkr_init(pixa_pa_device_t *pixa_ai);

void mqtt_task(void *data)
{
	vad_handle_t vad_inst = vad_create(VAD_MODE_3);
	vad_state_t vad_state = { 0 };
	char pixa_name[PIXA_IP_BUF_MAX] = "";
	const char *name = "pixa/input/";
	int16_t *vad_buff = (int16_t *)malloc(READ_TASK_BUF);
	int msg_id = 0;
	uint8_t index = 0;

	while (1) {
		if (data_incoming) { 
			vTaskDelay(20 / portTICK_PERIOD_MS);
			continue;
		}

		raw_stream_read(pixa->raw, (char *)vad_buff, READ_TASK_BUF);
		vad_state = vad_process(vad_inst, (int16_t *)vad_buff, KHZ(16), 30);
		if (vad_state == VAD_SPEECH) {
			++index;
			sprintf(pixa_name, "%s%d", name, index);
			msg_id = esp_mqtt_client_publish(client, pixa_name, ( const char *)vad_buff, READ_TASK_BUF, 1, 0);
			pr_info(pr_fmt,"sent publish successful, msg_id=%d", msg_id);
		} else if (index) {
			++index;
			sprintf(pixa_name, "%s%d", name, index);
			msg_id = esp_mqtt_client_publish(client, pixa_name, ( const char *)"##", 2, 1, 0);
			pr_info(pr_fmt,"sent publish successful, msg_id=%d", msg_id);
			index=0;
		}
	}
}

static void mqtt_evt_handler(void *args, esp_event_base_t base, int32_t id, void *edata)
{
	esp_mqtt_event_handle_t event = edata;
	esp_mqtt_client_handle_t client = event->client;
	esp_mqtt_event_id_t mqtt_id = id;
	int msg_id;

	switch (mqtt_id) {
	case MQTT_EVENT_CONNECTED:
		pr_info(pr_fmt,"MQTT_EVENT_CONNECTED");
		msg_id = esp_mqtt_client_subscribe(client, "pixa/output/#", 1);
		if (msg_id)
			pr_info(pr_fmt,"subscribed successfully, msg_id=%d", msg_id);
		break;
	case MQTT_EVENT_DISCONNECTED:
		pr_info(pr_fmt,"MQTT_EVENT_DISCONNECTED");
		break;
	case MQTT_EVENT_SUBSCRIBED:
		pr_info(pr_fmt,"MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_UNSUBSCRIBED:
		pr_info(pr_fmt,"MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_PUBLISHED:
		pr_debug(pr_fmt,"MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_ERROR:
		pr_info(pr_fmt,"MQTT_EVENT_ERROR");
		if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
			pr_info(pr_fmt,"Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
		break;
	case MQTT_EVENT_DATA:
		if (event->data_len == 2) {
			pr_warn(pr_fmt, "DATA: %d", event->total_data_len);
			raw_stream_write(pixa_pa->raw, data_buffer, data_buffer_index);
			memset(data_buffer, 0, TOTAL_SIZE);
			data_buffer_index = 0;
			data_incoming = false;
		} else if (!data_incoming)
			data_incoming = true;

		pr_warn(pr_fmt, "datalen: %d", event->data_len);
		if (data_buffer_index + event->data_len <= TOTAL_SIZE) {
			memcpy(data_buffer + data_buffer_index, event->data, event->data_len);
			data_buffer_index += event->data_len;
		} else {
			pr_warn(pr_fmt, "Buffer full or data chunk too large");
			raw_stream_write(pixa_pa->raw, data_buffer, TOTAL_SIZE);
			memset(data_buffer, 0, TOTAL_SIZE);
			data_buffer_index = 0;
			memcpy(data_buffer + data_buffer_index, event->data, event->data_len);
			data_buffer_index += event->data_len;
		}

		break;
	default:
		data_incoming = false;
		break;
	}
}

static inline void wifi_evt_handler(void *args, esp_event_base_t base, int32_t event_id, void *data)
{
	static uint8_t retries = 0;

	switch(event_id) {
	case WIFI_EVENT_STA_START:
		pr_info(pr_fmt, "CONNECTING....");
		break;
	case WIFI_EVENT_STA_CONNECTED:
		pr_info(pr_fmt, "CONNECTED");
		break;
	case WIFI_EVENT_STA_DISCONNECTED:
		pr_info(pr_fmt, "LOST CONNECTION");
		if (retries < 5) {
			esp_wifi_connect();
			retries++;
			pr_info(pr_fmt, "RECONNECTING....");
		}
		break;
	case IP_EVENT_STA_GOT_IP:
		ip_got = true;
		pr_info(pr_fmt, "GOT IP");
		break;
	default:
		break;
	}
}

static inline esp_err_t wifi_init(void)
{
	esp_err_t ret = ESP_OK;
	const char *ssid = CONFIG_PIXA_SSID;
	const char *pass = CONFIG_PIXA_PASS;
	wifi_config_t wifi_cfg = { 0 };
	wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();

	strcpy((char*)wifi_cfg.sta.ssid, ssid);
	strcpy((char*)wifi_cfg.sta.password, pass);

	ret = nvs_flash_init();
	if (ret) {
		pr_warn(pr_fmt, "NVS init failed! erasing nvs, retrying");
		ret = nvs_flash_erase();
		if (ret) {
			pr_err(pr_fmt, "nvs erase failed!");
			return ESP_FAIL;
		}

		ret = nvs_flash_init();
		if (ret) {
			pr_err(pr_fmt, "nvs init failed! abort wifi init");
			return ESP_FAIL;
		}
	}

	esp_netif_init();
	esp_event_loop_create_default();
	esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt_handler, NULL);
	esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt_handler, NULL);

	esp_netif_create_default_wifi_sta();
	esp_wifi_init(&wifi);
	wifi_cfg.sta.threshold.authmode  = WIFI_AUTH_WPA2_WPA3_PSK;
	wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
	esp_wifi_set_mode(WIFI_MODE_STA);
	esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);
	esp_wifi_start();
	esp_wifi_connect();

	// wait for wifi to actually connect
	while (!ip_got)
		vTaskDelay(250 / portTICK_PERIOD_MS);

	return ESP_OK;
}

void mic_init(pixa_device_t *pixa_ai)
{
	const char *plink[PLINK_MAX] = {"i2s", "filter", "raw"};
	audio_pipeline_cfg_t pline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
	i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(CODEC_ADC_I2S_PORT, MIC_I2S_FREQ, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_READER);
	rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
	rsp_cfg.src_rate = KHZ(16);
	/* Uncomment when the microphone's 16 khz audio is understood by the server
	rsp_cfg.src_rate = KHZ(44.1);
	rsp_cfg.dest_rate = KHZ(16);
	*/
	rsp_cfg.dest_ch = 2;
	rsp_cfg.max_indata_bytes = READ_TASK_BUF;
	rsp_cfg.out_len_bytes = READ_TASK_BUF;
	raw_stream_cfg_t raw_cfg = { AUDIO_STREAM_READER, RAW_CFG_BUFFER };
	esp_err_t ret = ESP_OK;
	audio_hal_codec_i2s_iface_t i2s_f_cfg = {
		.mode = AUDIO_HAL_MODE_MASTER,
		.fmt = AUDIO_HAL_I2S_DSP,
		.samples = AUDIO_HAL_44K_SAMPLES,
		.bits = AUDIO_HAL_BIT_LENGTH_16BITS,
	};

	// Select mic bias 1 and 2 as the hardware is connected to them
	es7210_mic_select(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2);
	// Set gain to 9 db
	es7210_adc_set_gain(9);
	es7210_adc_config_i2s(AUDIO_HAL_CODEC_MODE_ENCODE, &i2s_f_cfg);

	pixa_ai->i2s = i2s_stream_init(&i2s_cfg);
	if (!pixa_ai->i2s) {
		pr_err(pr_fmt, "failed to init i2s stream!");
		return;
	}

	pixa_ai->rsp = rsp_filter_init(&rsp_cfg);
	if (!pixa_ai->rsp) {
		pr_err(pr_fmt, "failed to init filter!");
		return;
	}

	pixa_ai->raw = raw_stream_init(&raw_cfg);
	if (!pixa_ai->raw) {
		pr_err(pr_fmt, "failed to raw init stream!");
		return;
	}

	pixa_ai->pipe = audio_pipeline_init(&pline_cfg);
	if (!pixa_ai->pipe) {
		pr_err(pr_fmt, "failed to initialise pipe!");
		return;
	}

	ret = audio_pipeline_register(pixa_ai->pipe, pixa_ai->i2s, plink[0]);
	if (ret) {
		pr_err(pr_fmt, "pipeline registration for i2s failed");
		return;
	}

	ret = audio_pipeline_register(pixa_ai->pipe, pixa_ai->rsp, plink[1]);
	if (ret) {
		pr_err(pr_fmt, "pipeline registration for rsp filter failed");
		return;
	}

	ret = audio_pipeline_register(pixa_ai->pipe, pixa_ai->raw, plink[2]);
	if (ret) {
		pr_err(pr_fmt, "pipeline registration for raw reader failed");
		return;
	}

	audio_pipeline_link(pixa_ai->pipe, &plink[0], PLINK_MAX);
	audio_pipeline_run(pixa_ai->pipe);
}

void spkr_init(pixa_pa_device_t *pixa_ai)
{
	const char *pa_plink[PLINK_PA_MAX] = {"pa_raw", "pa_eq", "pa_i2s"};
	i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_1, SPEAKER_I2S_FREQ, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_WRITER);
	audio_pipeline_cfg_t pline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    equalizer_cfg_t eq_cfg = DEFAULT_EQUALIZER_CONFIG();
    eq_cfg.set_gain = (int[]) {3, 3, 2, 1, 0, 0, 1, 2, 3, 4};
	// rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
	// rsp_cfg.src_rate = KHZ(16);
	// rsp_cfg.dest_rate = KHZ(16);
	// rsp_cfg.max_indata_bytes = TOTAL_SIZE;
	// rsp_cfg.out_len_bytes = TOTAL_SIZE;
    raw_stream_cfg_t raw_cfg = { AUDIO_STREAM_WRITER, TOTAL_SIZE };
	esp_err_t ret = ESP_OK;

	pixa_ai->raw = raw_stream_init(&raw_cfg);
	if (!pixa_ai->raw) {
		pr_err(pr_fmt, "failed to raw init stream!");
		return;
	}

	pixa_ai->eq = equalizer_init(&eq_cfg);
	if (!pixa_ai->eq) {
		pr_err(pr_fmt, "failed to init eq write stream!");
		return;
	}

	// pixa_ai->rsp = rsp_filter_init(&rsp_cfg);
	// if (!pixa_ai->i2s) {
	// 	pr_err(pr_fmt, "failed to init rsp write stream!");
	// 	return;
	// }

	pixa_ai->i2s = i2s_stream_init(&i2s_cfg);
	if (!pixa_ai->i2s) {
		pr_err(pr_fmt, "failed to init i2s write stream!");
		return;
	}

	pixa_ai->pipe = audio_pipeline_init(&pline_cfg);
	if (!pixa_ai->pipe) {
		pr_err(pr_fmt, "failed to initialise pipe!");
		return;
	}

	ret = audio_pipeline_register(pixa_ai->pipe, pixa_ai->raw, pa_plink[0]);
	if (ret) {
		pr_err(pr_fmt, "pipeline registration for raw reader failed");
		return;
	}

	ret = audio_pipeline_register(pixa_ai->pipe, pixa_ai->eq, pa_plink[1]);
	if (ret) {
		pr_err(pr_fmt, "pipeline registration for eq reader failed");
		return;
	}

	// ret = audio_pipeline_register(pixa_ai->pipe, pixa_ai->rsp, pa_plink[2]);
	// if (ret) {
	// 	pr_err(pr_fmt, "pipeline registration for eq reader failed");
	// 	return;
	// }

	ret = audio_pipeline_register(pixa_ai->pipe, pixa_ai->i2s, pa_plink[2]);
	if (ret) {
		pr_err(pr_fmt, "pipeline registration for i2s reader failed");
		return;
	}

	audio_pipeline_link(pixa_ai->pipe, &pa_plink[0], PLINK_PA_MAX);
	audio_pipeline_run(pixa_ai->pipe);
}

void app_main(void)
{
	esp_mqtt_client_config_t mqtt_cfg = {
		.broker.address.uri = CONFIG_PIXA_MQTT_SERVER,
		.broker.verification.certificate = (const char *)ssl_start,
		.credentials.username = CONFIG_PIXA_MQTT_USERNAME,
		.credentials.authentication.password = CONFIG_PIXA_MQTT_PASSWORD,
		.buffer.size = MQTT_BUFFER_SIZE,
	};
	esp_err_t ret = ESP_OK;
	audio_board_handle_t board = NULL;
	pixa = (pixa_device_t *)heap_caps_malloc(sizeof(pixa_device_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT);
	pixa_pa = (pixa_pa_device_t *)heap_caps_malloc(sizeof(pixa_pa_device_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT);

	board = audio_board_init();
	if (!board) {
		pr_err(pr_fmt, "failed to start codec es7210!");
		return;
	}

	ret = audio_hal_ctrl_codec(board->audio_hal, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);
	pr_warn(pr_fmt, "initialise encode hal! ret: %d", ret);

	ret = audio_hal_ctrl_codec(board->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
	pr_warn(pr_fmt, "initialise decode hal! ret: %d", ret);

	wifi_init();
	mic_init(pixa);
	spkr_init(pixa_pa);

	// Adjust pa to output the max possible volume
	audio_hal_set_volume(board->audio_hal, 100);

	client = esp_mqtt_client_init(&mqtt_cfg);
	if (client) {
		esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_evt_handler, NULL);
		esp_mqtt_client_start(client);
	}

	data_buffer = (char *)heap_caps_malloc(TOTAL_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT);

	pr_info(pr_fmt, "MQTT clinet initialised!");

	xTaskCreatePinnedToCore(mqtt_task, "MQTT_TASK", 8192 , NULL, 1, NULL, 0);

	// LOOP indefinately
	while (1)
		vTaskDelay(20 / portTICK_PERIOD_MS);
}
