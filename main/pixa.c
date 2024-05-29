// SPDX-License-Identifier: GPL-3.0
// Author: Divyanshu-Modi <divyan.m05@gmail.com>

#define pr_fmt "PIXA"

#include "audio_common.h"
#include "audio_pipeline.h"
#include "board.h"
#include "es7210.h"
#include "esp_ns.h"
#include "esp_nsn_models.h"
#include "esp_nsn_iface.h"
#include "esp_vad.h"
#include "esp_wifi.h"
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

#define RAW_CFG_BUFFER 7 * 2048
#define PIXA_IP_BUF_MAX 16
#define READ_TASK_BUF 2048 * sizeof(int16_t)
#define PLINK_MAX 3
#define KHZ(hz) (hz * 1000)

// SSL CERTIFICATE FOR MQTT SERVER
extern const uint8_t ssl_start[] asm("_binary_isrgrootx1_pem_start");
extern const uint8_t ssl_end[] asm("_binary_isrgrootx1_pem_end");

static bool ip_got = false;

typedef struct pixa_buf {
	audio_element_handle_t raw;
	esp_mqtt_client_handle_t client;
} pixa_t;

void mqtt_task(void *data)
{
	vad_handle_t vad_inst = vad_create(VAD_MODE_4);
	vad_state_t vad_state = { 0 };
	srmodel_list_t *model = esp_srmodel_init("model");
	char pixa_name[PIXA_IP_BUF_MAX] = "";
	const char *name = "pixa/input/";
	const char *ns_model = esp_srmodel_filter(model, ESP_NSNET_PREFIX, NULL);
	ns_handle_t ns_proc = (ns_handle_t)malloc(sizeof(ns_handle_t));
	int16_t *vad_buff = (int16_t *)malloc(READ_TASK_BUF);
	int16_t *ovad_buff = (int16_t *)malloc(READ_TASK_BUF);
	int msg_id = 0;
	uint8_t index = 0;
	pixa_t *pbuf = (pixa_t *)data;
	esp_nsn_iface_t ns = ESP_NSN_HANDLE;

	esp_nsn_data_t *buf = ns.create((char *)ns_model);
	ns_proc = ns_pro_create(10, 2, 16000);

	pr_info(pr_fmt, "model: %s", ns_model);

	while (1) {
		raw_stream_read(pbuf->raw, (char *)vad_buff, READ_TASK_BUF);
		ns.process(buf, vad_buff, ovad_buff);
		memset(vad_buff, 0, READ_TASK_BUF);
		ns_process(ns_proc, ovad_buff, vad_buff);

		vad_state = vad_process(vad_inst, (int16_t *)vad_buff, KHZ(16), 30);
		if (vad_state == VAD_SPEECH) {
			++index;
			sprintf(pixa_name, "%s%d", name, index);
			msg_id = esp_mqtt_client_publish(pbuf->client, pixa_name, ( const char *)vad_buff, READ_TASK_BUF, 1, 0);
			vTaskDelay(10 / portTICK_PERIOD_MS);
			pr_info(pr_fmt,"sent publish successful, msg_id=%d", msg_id);
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
	default:
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

void app_main(void)
{
	esp_mqtt_client_config_t mqtt_cfg = {
		.broker.address.uri = CONFIG_PIXA_MQTT_SERVER,
		.broker.verification.certificate = (const char *)ssl_start,
		.credentials.username = CONFIG_PIXA_MQTT_USERNAME,
		.credentials.authentication.password = CONFIG_PIXA_MQTT_PASSWORD,
	};
	const char *plink[PLINK_MAX] = {"i2s", "filter", "raw"};
	esp_err_t ret = ESP_OK;

	audio_pipeline_cfg_t pline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();

	i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(CODEC_ADC_I2S_PORT, AUDIO_HAL_44K_SAMPLES, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_READER);
	rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
	raw_stream_cfg_t raw_cfg = { AUDIO_STREAM_READER, RAW_CFG_BUFFER };

	audio_board_handle_t board = { 0 };
	audio_pipeline_handle_t pipeline = { 0 };

	audio_element_handle_t filter = { 0 };
	audio_element_handle_t i2s_stream = { 0 };

	pixa_t *pbuf = (pixa_t *)malloc(sizeof(pixa_t));

	board = audio_board_init();
	if (!board) {
		pr_err(pr_fmt, "failed to start codec es7210!");
		return;
	}

	ret = audio_hal_ctrl_codec(board->adc_hal, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);
	if (!ret) {
		pr_err(pr_fmt, "failed to initialise adc hal!");
		return;
	}

	// Select mic bias 1 and 2 as the hardware is connected to them
	es7210_mic_select(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2);
	// Set gain to 10 db
	es7210_adc_set_gain(10);

	i2s_stream = i2s_stream_init(&i2s_cfg);
	if (!i2s_stream) {
		pr_err(pr_fmt, "failed to init i2s stream!");
		return;
	}

	rsp_cfg.src_rate = KHZ(16); // Set the src data rate to 48KHz
	filter = rsp_filter_init(&rsp_cfg);
	if (!filter) {
		pr_err(pr_fmt, "failed to init filter!");
		return;
	}

	pbuf->raw = raw_stream_init(&raw_cfg);
	if (!pbuf->raw) {
		pr_err(pr_fmt, "failed to raw init stream!");
		return;
	}

	pipeline = audio_pipeline_init(&pline_cfg);
	if (!pipeline) {
		pr_err(pr_fmt, "failed to initialise pipe!");
		return;
	}

	ret = audio_pipeline_register(pipeline, i2s_stream, plink[0]);
	if (ret) {
		pr_err(pr_fmt, "pipeline registration for i2s failed");
		return;
	}

	ret = audio_pipeline_register(pipeline, filter, plink[1]);
	if (ret) {
		pr_err(pr_fmt, "pipeline registration for rsp filter failed");
		return;
	}

	ret = audio_pipeline_register(pipeline, pbuf->raw, plink[2]);
	if (ret) {
		pr_err(pr_fmt, "pipeline registration for raw reader failed");
		return;
	}

	audio_pipeline_link(pipeline, &plink[0], PLINK_MAX);
	audio_pipeline_run(pipeline);

	wifi_init();

	pbuf->client = esp_mqtt_client_init(&mqtt_cfg);
	if (pbuf->client) {
		esp_mqtt_client_register_event(pbuf->client, ESP_EVENT_ANY_ID, mqtt_evt_handler, NULL);
		esp_mqtt_client_start(pbuf->client);
	}

	pr_info(pr_fmt, "MQTT clinet initialised!");

	xTaskCreate(mqtt_task, "MQTT_TASK", 8192 , pbuf, 0, NULL );

	// LOOP indefinately
	while (1)
		vTaskDelay(20 / portTICK_PERIOD_MS);
}
