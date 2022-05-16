#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include "nvs_flash.h"
#include "esp_adc_cal.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "sdkconfig.h"
#include "DHT22.h"
#include "mqtt_client.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "freertos/semphr.h"
#include "freertos/queue.h"


#define NO_OF_SAMPLES   15          //Multisampling
#define NUM_TIMERS 5                //timers for task repetition

//defined in IDF config as safety precaution
#define WIFI_SSID     	CONFIG_ESP_WIFI_SSID
#define WIFI_PASS     	CONFIG_ESP_WIFI_PASS
#define WIFI_CHANNEL 	CONFIG_ESP_WIFI_CHANNEL
#define MAX_STA_CONN   	CONFIG_ESP_MAX_STA_CONN

//how many samples of DHT22 have been recorded
uint32_t dht_readCount = 1;
uint32_t adc_reading = 0;

//array to hold timers
TimerHandle_t xTimers[ NUM_TIMERS ];

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,&wifi_event_handler,NULL,NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d", WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
	esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_publish(client, "/topic/qos1", "data_3", 0, 1, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
        ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_BROKER_URL,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void vTimerResolve(TimerHandle_t pxTimer) {
    
    int32_t lArrayIndex;

    //index of xTimers[] for expired timer
    lArrayIndex = (int32_t) pvTimerGetTimerID(pxTimer);

    //if expired timer is still active, report error
    if(xTimerIsTimerActive(pxTimer) == pdTRUE) {
         ESP_LOGI(TAG, "TIMER_RESOLVE_ERROR");
    }

}

void DHT22_task(void *pvParameter) {

	int dht_readCount = 1;
	printf( "Reading DHT22\n\n");	
	printf("Sample #%d\n", dht_readCount++);
	
	int ret = readDHT();	
	errorHandler(ret);

	printf( "Humidity: %.1f\n", getHumidity() );
	printf( "Temperature: %.1f\n", getTemperature()*1.8+32);
	
	//cannot read from the DHT22 twice within a 2 second window
	//vTaskDelay( 3000 / portTICK_RATE_MS );

}

void ADC_task() {

	//Multisample 15 times before returning a reading
	for (int i = 0; i < NO_OF_SAMPLES; i++) {
		adc_reading += adc1_get_raw((adc1_channel_t)ADC_CHANNEL_6);
	}
	
	adc_reading /= NO_OF_SAMPLES;

	//Convert adc_reading to voltage in mV
	uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);
	vTaskDelay(pdMS_TO_TICKS(1000));

}

void app_main() {

	//initialize flash memory
	nvs_flash_init();
	vTaskDelay( 1000 / portTICK_RATE_MS );	//set task delay to 1 second
	
	//initialize ADC
	adc1_config_width(ADC_WIDTH_BIT_12);
	adc1_config_channel_atten(ADC_CHANNEL_6, ADC_ATTEN_11db);

    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, ADC_ATTEN_11db, ADC_WIDTH_BIT_12, DEFAULT_VREF, adc_chars);
    print_char_val_type(val_type);
 
	xTaskCreate( &ADC_task, "ADC_task", 2048, NULL, 5, NULL );
    TimerHandle_txTimerCreate(*ADC_timer, portTICK_PERIOD_MS, uxAutoReload, xTimers[0], pxCallbackFunction);
    vTimerSetReloadMode(ADC_timer, pdTRUE);

	//create task to read from DHT22
	gpio_set_direction(4, GPIO_MODE_OUTPUT);
	xTaskCreate( &DHT22_task, "DHT22_task", 2048, NULL, 5, NULL );
    TimerHandle_txTimerCreate(*DHT22_timer, portTICK_PERIOD_MS, uxAutoReload, xTimers[1], pxCallbackFunction);
    vTimerSetReloadMode(DHT22_timer, pdTRUE);

	//initialize other GPIO
	gpio_set_direction(0, GPIO_MODE_OUTPUT);
	gpio_set_direction(2, GPIO_MODE_OUTPUT);
	gpio_set_direction(15, GPIO_MODE_OUTPUT);
    
    //set 3 timers for each GPIO, one for each pump and one for the servo
    for (int i=2; i<5; i++) {
        TimerHandle_txTimerCreate(&xTimers[i], portTICK_PERIOD_MS, uxAutoReload, xTimers[i], pxCallbackFunction);
        vTimerSetReloadMode(&xTimers[i], pdTRUE);
    }

	//initialize WiFi and MQTT
    wifi_init_softap();
	mqtt_app_start();

	//loop while tasks schedule and repeat
	while(1) {
		if (mqtt_get_suback_data(msg_buf, &msg_data_len) <= 0) {
			ESP_LOGE(TAG, "Failed to acquire suback data");
			return ESP_FAIL;
		}
	}

}

