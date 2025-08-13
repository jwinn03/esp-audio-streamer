/*
 * WebSocket Audio Streamer for ESP32 using ESP-IDF and FreeRTOS
 *
 * This application captures audio from an I2S microphone and streams it
 * over a WebSocket connection to a server. It uses three FreeRTOS tasks:
 * 1. wifi_task: Connects the device to a WiFi network.
 * 2. websocket_task: Establishes and maintains the WebSocket connection.
 * 3. i2s_stream_task: Reads audio data from the I2S microphone and sends it.
 *
 * An event group is used to synchronize the tasks, ensuring that audio streaming
 * only begins after both WiFi and WebSocket connections are successfully established.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_websocket_client.h"

// ========================================================================================
// --- CONFIGURATION ---
// ========================================================================================

// WiFi Configuration
#define WIFI_SSID      "W2 Network"
#define WIFI_PASS      "phan8496"

// WebSocket Server Configuration
// NOTE: Use "ws://" protocol. For secure "wss://", additional configuration is needed.
#define WEBSOCKET_URI  "ws://192.168.5.28:8080" // <-- IMPORTANT: Change to your server's IP

// I2S Microphone Configuration (for a typical INMP441)
#define I2S_WS_PIN     6
#define I2S_SD_PIN     4
#define I2S_SCK_PIN    5
#define I2S_PORT       I2S_NUM_0

// Audio Configuration
#define SAMPLE_RATE    (44100)
#define BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_16BIT
#define AUDIO_BUFFER_SIZE (4096) // Size of the buffer to hold audio data chunks

// --- NEW: Hardware Switch Configuration ---
// Connect a switch between this pin and GND.
// When switch is closed (pin is LOW), transmission is ON.
// When switch is open (pin is HIGH due to pull-up), transmission is OFF.
#define TRANSMIT_SWITCH_PIN GPIO_NUM_39

// ========================================================================================
// --- GLOBAL VARIABLES & DEFINITIONS ---
// ========================================================================================

static const char *TAG = "AUDIO_STREAM";

// Event group to signal connection statuses
static EventGroupHandle_t app_event_group;
#define WIFI_CONNECTED_BIT    BIT0
#define WEBSOCKET_CONNECTED_BIT BIT1

// WebSocket client handle
static esp_websocket_client_handle_t client;

// ========================================================================================
// --- TASK 1: WIFI CONNECTION ---
// ========================================================================================

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected from WiFi. Retrying...");
        xEventGroupClearBits(app_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(app_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting WiFi Task");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize network stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // Configure and start WiFi
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi task finished initialization. Waiting for connection...");
    
    // This task has completed its setup, it can be deleted or suspended.
    // For this example, we let it run in the background to handle reconnections.
    vTaskDelete(NULL);
}

// ========================================================================================
// --- TASK 2: WEBSOCKET CLIENT ---
// ========================================================================================

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
            xEventGroupSetBits(app_event_group, WEBSOCKET_CONNECTED_BIT);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
            xEventGroupClearBits(app_event_group, WEBSOCKET_CONNECTED_BIT);
            // The client will attempt to reconnect automatically due to `disable_auto_reconnect: false`
            break;
        case WEBSOCKET_EVENT_DATA:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_DATA");
            ESP_LOGI(TAG, "Received opcode=%d", data->op_code);
            if (data->op_code == 0x08 && data->data_len == 2) {
                ESP_LOGW(TAG, "Received closed message with code=%d", 256 * data->data_ptr[0] + data->data_ptr[1]);
            } else {
                ESP_LOGW(TAG, "Received=%.*s", data->data_len, (char *)data->data_ptr);
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
            break;
    }
}

void websocket_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting WebSocket Task");

    // Wait for WiFi to be connected
    ESP_LOGI(TAG, "WebSocket task waiting for WiFi...");
    xEventGroupWaitBits(app_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected, starting WebSocket client.");

    esp_websocket_client_config_t websocket_cfg = {
        .uri = WEBSOCKET_URI,
        // Set a longer timeout, useful for debugging
        .reconnect_timeout_ms = 10000, 
        // By default, the client will attempt to reconnect automatically
        // .disable_auto_reconnect = false, 
    };

    client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
    esp_websocket_client_start(client);

    // This task has completed its setup, it can be deleted or suspended.
    // For this example, we let it run in the background to handle reconnections.
    vTaskDelete(NULL);
}


// ========================================================================================
// --- TASK 3: I2S AUDIO STREAMING ---
// ========================================================================================

void i2s_stream_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting I2S Stream Task");

    // Configure the GPIO pin for hardware switch
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << TRANSMIT_SWITCH_PIN);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1; // Enable internal pull-up resistor
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Transmit switch configured on GPIO %d", TRANSMIT_SWITCH_PIN);

    // Configure I2S
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,

        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD_PIN
    };

    // Install and start I2S driver
    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin_config));

    // Allocate buffer for audio data
    uint8_t* audio_buffer = (uint8_t*) calloc(AUDIO_BUFFER_SIZE, sizeof(uint8_t));
    if (audio_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for audio buffer");
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "I2S Stream task waiting for connections...");
    // Wait for both WiFi and WebSocket to be connected
    xEventGroupWaitBits(app_event_group, WIFI_CONNECTED_BIT | WEBSOCKET_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Connections established, starting audio stream.");

    while (1) {
        size_t bytes_read = 0;
        // Read audio data from I2S into the buffer
        esp_err_t result = i2s_read(I2S_PORT, audio_buffer, AUDIO_BUFFER_SIZE, &bytes_read, portMAX_DELAY);

        if (result == ESP_OK && bytes_read > 0) {
            // Check the state of transmit switch
            if (gpio_get_level(TRANSMIT_SWITCH_PIN) == 0) { // Pin is LOW (switch closed), so transmit
                // Check if WebSocket is connected before sending
                if (esp_websocket_client_is_connected(client)) {
                    // Send the raw audio data as a binary WebSocket message
                    int err = esp_websocket_client_send_bin(client, (const char *)audio_buffer, bytes_read, portMAX_DELAY);
                    if (err < 0) {
                        ESP_LOGE(TAG, "WebSocket send error: %d", err);
                    }
                }
            }
        } else {
            ESP_LOGE(TAG, "I2S read error: %d", result);
        }
        // Small delay to prevent watchdog timer from triggering and to yield to other tasks
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // Cleanup
    free(audio_buffer);
    vTaskDelete(NULL);
}

// ========================================================================================
// --- MAIN ---
// ========================================================================================

void app_main(void) {
    ESP_LOGI(TAG, "Starting App Main");

    app_event_group = xEventGroupCreate();

    // Create the tasks
    xTaskCreate(&wifi_task, "wifi_task", 4096, NULL, 5, NULL);
    xTaskCreate(&websocket_task, "websocket_task", 8192, NULL, 5, NULL);
    xTaskCreate(&i2s_stream_task, "i2s_stream_task", 10240, NULL, 12, NULL); // Higher priority for audio
}
