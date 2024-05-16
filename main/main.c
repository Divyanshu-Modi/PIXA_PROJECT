// SPDX-License-Identifier: GPL-3.0
// Author: Divyanshu-Modi <divyan.m05@gmail.com>

#define pr_fmt "main"

#include "audio_common.h"
#include "audio_pipeline.h"
#include "board.h"
#include "es7210.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_vad.h"
#include "esp_wifi.h"
#include "filter_resample.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2s_stream.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "pr_log.h"
#include "raw_stream.h"
#include "sdkconfig.h"

#define API_KEY CONFIG_API_KEY
#define COMP_MAX 3
#define MQTT_URL ""

static bool ip_got = false;
audio_element_handle_t raw_read = NULL;
esp_mqtt_client_handle_t client = NULL;

extern const uint8_t ssl_start[]   asm("_binary_isrgrootx1_pem_start");
extern const uint8_t ssl_end[]   asm("_binary_isrgrootx1_pem_end");

static inline void wifi_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *data)
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

static inline void wifi_init(void)
{
	const char *ssid = "Sentinel_Wlan";
	const char *pass = "Suryan1358";
	wifi_config_t wifi_cfg = { 0 };
	wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();

	nvs_flash_init();
	nvs_flash_erase();
	nvs_flash_init();

	esp_netif_init();
	esp_event_loop_create_default();
	esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
	esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
	esp_netif_create_default_wifi_sta();
	esp_wifi_init(&wifi);
	strcpy((char*)wifi_cfg.sta.ssid, ssid);
	strcpy((char*)wifi_cfg.sta.password, pass);
	wifi_cfg.sta.threshold.authmode  = WIFI_AUTH_WPA2_WPA3_PSK;
	wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
	esp_wifi_set_mode(WIFI_MODE_STA);
	esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);
	esp_wifi_start();
	esp_wifi_connect();

	// wait for wifi to actually connect
	while (!ip_got)
		vTaskDelay(250 / portTICK_PERIOD_MS);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
	pr_debug(pr_fmt,"Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
	esp_mqtt_event_handle_t event = event_data;
	esp_mqtt_client_handle_t client = event->client;
	int msg_id;
	switch ((esp_mqtt_event_id_t)event_id) {
	case MQTT_EVENT_CONNECTED:
		pr_info(pr_fmt,"MQTT_EVENT_CONNECTED");
		// raw_stream_read(raw_read, (char *)vad_buff, 2048 * sizeof(short));
		msg_id = esp_mqtt_client_publish(client, "pixa/input/", "##", 2, 1, 0);
		pr_info(pr_fmt,"sent publish successful, msg_id=%d", msg_id);

		msg_id = esp_mqtt_client_subscribe(client, "pixa/output/#", 1);
		pr_info(pr_fmt,"sent subscribe successful, msg_id=%d", msg_id);
		break;
	case MQTT_EVENT_DISCONNECTED:
		pr_info(pr_fmt,"MQTT_EVENT_DISCONNECTED");
		break;

	case MQTT_EVENT_SUBSCRIBED:
		pr_info(pr_fmt,"MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
		// msg_id = esp_mqtt_client_publish(client, "pixa/input/", ( const char *)vad_buff, 4096 * sizeof(short), 1, 0);
		// pr_info(pr_fmt,"sent publish successful, msg_id=%d", msg_id);
		break;
	case MQTT_EVENT_UNSUBSCRIBED:
		pr_info(pr_fmt,"MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_PUBLISHED:
		pr_info(pr_fmt,"MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
		break;
	// case MQTT_EVENT_DATA:
	//     pr_info(pr_fmt,"MQTT_EVENT_DATA");
	//     // printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
	//     // printf("DATA=%.*s\r\n", event->data_len, event->data);
	//     break;
	case MQTT_EVENT_ERROR:
		pr_info(pr_fmt,"MQTT_EVENT_ERROR");
		if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
			pr_info(pr_fmt,"Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
		break;
	default:
		break;
	}
}

void mqtt_init(void)
{
	esp_mqtt_client_config_t mqtt_cfg = {
		.broker.address.uri = "mqtts://5888ad13de3949caba2ccf33e5dcc2ba.s1.eu.hivemq.cloud",
		// .broker.address.port = 8883,
		.broker.verification.certificate = (const char *)ssl_start,
		.credentials.username = "client",
		.credentials.authentication.password = "Client@123",
	};

	client = esp_mqtt_client_init(&mqtt_cfg);
	pr_info(pr_fmt, "MQTT clinet initialised!");
	if (client) {
		esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
		esp_mqtt_client_start(client);
	}
}

void raw_read_task(void *param)
{
	int msg_id = 0;
	uint8_t index = 0;
	int8_t *vad_buff = (int8_t *)malloc(2 *  8192 * sizeof(int8_t));
	vad_handle_t vad_inst = vad_create(VAD_MODE_3);
	vad_state_t vad_state = { 0 };
	char *name = "pixa/input/";
	char pixa_name[30] = "";
	// afe_vad_state_t vad_state = { 0 };
	// esp_afe_sr_data_t *afe_data = NULL;
	// esp_afe_sr_iface_t *afe = (esp_afe_sr_iface_t *)&ESP_AFE_VC_HANDLE;
	// afe_config_t afe_config = AFE_CONFIG_DEFAULT();
	// afe_config.aec_init = false;
	// afe_config.voice_communication_init = true;
	// afe_config.vad_mode = VAD_MODE_2;
	// afe_config.voice_communication_agc_gain = 7;
	// afe_config.voice_communication_agc_init = true;
	// afe_fetch_result_t *ret = NULL;

	// afe_data = afe->create_from_config(&afe_config);

	while (1) {
		raw_stream_read(raw_read, (char *)vad_buff, 8192 * sizeof(int8_t));
		// afe->feed(afe_data, vad_buff);
		// ret = afe->fetch(afe_data);
		vad_state = vad_process(vad_inst, (int16_t *)vad_buff, 16000, 30);
		if (vad_state == VAD_SPEECH) {
			index += 1;
			sprintf(pixa_name, "%s%d", name, index);
			// pr_info(pr_fmt, "index: %s", pixa_name);
			msg_id = esp_mqtt_client_publish(client, pixa_name, ( const char *)vad_buff,8192 * sizeof(int8_t), 1, 0);
			vTaskDelay(10 / portTICK_PERIOD_MS);
			pr_info(pr_fmt,"sent publish successful, msg_id=%d", msg_id);
		}
	}
}

void adc_init(void)
{
	esp_err_t ret = ESP_OK;
	const char *plink[COMP_MAX] = {"i2s", "filter", "raw"};
	audio_pipeline_cfg_t pline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
	i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(CODEC_ADC_I2S_PORT, AUDIO_HAL_44K_SAMPLES, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_READER);
	rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
	raw_stream_cfg_t raw_cfg = { AUDIO_STREAM_READER, RAW_STREAM_RINGBUFFER_SIZE };
	audio_board_handle_t board = (audio_board_handle_t)malloc(sizeof(audio_board_handle_t));
	audio_element_handle_t filter = (audio_element_handle_t)malloc(sizeof(audio_element_handle_t));
	audio_element_handle_t i2s_stream = (audio_element_handle_t)malloc(sizeof(audio_element_handle_t));
	audio_pipeline_handle_t pipeline = (audio_pipeline_handle_t)malloc(sizeof(audio_pipeline_handle_t));

	board = audio_board_init();
	if (!board) {
		pr_err(pr_fmt, "failed to start codec es7210!");
		return;
	}

	ret = audio_hal_ctrl_codec(board->adc_hal, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);
	if (!ret) {
		pr_err(pr_fmt, "failed to initialise adc hal!");
		goto hal_err;
	}

	es7210_mic_select(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2);
	ret = es7210_adc_get_gain();

	ret = es7210_adc_set_gain(10);
	ret = es7210_adc_get_gain();


	pr_info(pr_fmt, "ADC hal initialised successfully!");

	pipeline = audio_pipeline_init(&pline_cfg);
	if (!pipeline) {
		pr_err(pr_fmt, "failed to initialise pipe!");
		goto hal_err;
	}

	pr_info(pr_fmt, "Pipeline initialised successfully!");

	i2s_stream = i2s_stream_init(&i2s_cfg);
	if (!i2s_stream) {
		goto i2s_err;
		pr_err(pr_fmt, "failed to init i2s stream!");
	}

	ret = audio_pipeline_register(pipeline, i2s_stream, plink[0]);
	if (ret) {
		pr_err(pr_fmt, "pipeline registration for i2s failed");
		goto i2s_err;
	}

	rsp_cfg.src_rate = 16000; // Set the src data rate to 48KHz
	filter = rsp_filter_init(&rsp_cfg);
	if (!filter) {
		pr_err(pr_fmt, "failed to init filter!");
		goto filter_err;
	}

	ret = audio_pipeline_register(pipeline, filter, plink[1]);
	if (ret) {
		pr_err(pr_fmt, "pipeline registration for rsp filter failed");
		goto filter_err;
	}

	raw_read = raw_stream_init(&raw_cfg);
	if (!raw_read) {
		pr_err(pr_fmt, "failed to raw init stream!");
		goto raw_err;
	}

	ret = audio_pipeline_register(pipeline, raw_read, plink[2]);
	if (ret) {
		pr_err(pr_fmt, "pipeline registration for raw reader failed");
		goto raw_err;
	}

	audio_pipeline_link(pipeline, &plink[0], COMP_MAX);
	audio_pipeline_run(pipeline);

	xTaskCreate( raw_read_task, "RAW_TASK", 4096 , NULL, 0, NULL );
	return;

raw_err:
filter_err:
i2s_err:
hal_err:
	audio_board_deinit(board);
}

void app_main(void)
{
	wifi_init(); // Initialise wifi
	mqtt_init();
	adc_init();

	pr_info(pr_fmt, "main_task");
}
