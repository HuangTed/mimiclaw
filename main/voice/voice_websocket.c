#include "voice_websocket.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
// #include "freertos/event_groups.h"
#include "esp_log.h"
// #include "esp_wifi.h"
// #include "esp_event.h"
// #include "nvs_flash.h"
#include "driver/i2s_std.h"
#include "esp_websocket_client.h"
#include "esp_timer.h"

// ============ 配置参数 ============
#define WEBSOCKET_URI       "ws://192.168.110.168:7860"  // 服务器地址

// WebSocket 心跳和超时配置
#define WS_PING_INTERVAL_MS     5000    // Ping 心跳间隔（5秒）
#define WS_SEND_TIMEOUT_MS      1000    // 发送超时（1秒）
#define WS_RECONNECT_TIMEOUT_MS 5000    // 重连超时（5秒）
#define WS_KEEP_ALIVE_IDLE      5       // TCP Keep-Alive 空闲时间（秒）
#define WS_KEEP_ALIVE_INTERVAL  3       // TCP Keep-Alive 探测间隔（秒）
#define WS_KEEP_ALIVE_COUNT     3       // TCP Keep-Alive 探测次数

// I2S 引脚配置 (INMP441)
#define I2S_WS              42
#define I2S_SCK             41
#define I2S_SD              2
#define I2S_PORT            I2S_NUM_0

// 音频配置：16kHz, 16bit, 单声道（Moonshine 推荐）
#define SAMPLE_RATE         16000
#define BITS_PER_SAMPLE     16
#define I2S_CHANNEL_NUM     1

// 缓冲区配置
#define I2S_BUFFER_SIZE     512         // I2S DMA 缓冲区大小（样本数）
#define WEBSOCKET_BUF_SIZE  4096        // WebSocket 缓冲区大小
#define BUFFER_POOL_SIZE    40          // 缓冲池大小（与队列深度匹配）

static const char *TAG = "VOICE_WS";

// 全局变量
static esp_websocket_client_handle_t ws_client = NULL;
static QueueHandle_t audio_queue = NULL;
static volatile bool is_connected = false;
static i2s_chan_handle_t i2s_rx_chan = NULL;  // I2S 接收通道句柄

// ============ 缓冲池管理 ============
static int16_t buffer_pool[BUFFER_POOL_SIZE][I2S_BUFFER_SIZE];  // 预分配缓冲池
static QueueHandle_t free_buffer_queue = NULL;                  // 空闲缓冲区队列

// 初始化缓冲池
static void buffer_pool_init(void)
{
    free_buffer_queue = xQueueCreate(BUFFER_POOL_SIZE, sizeof(int16_t *));
    if (!free_buffer_queue) {
        ESP_LOGE(TAG, "Failed to create free buffer queue");
        return;
    }
    
    // 将所有缓冲区放入空闲队列
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        int16_t *buf = buffer_pool[i];
        xQueueSend(free_buffer_queue, &buf, 0);
    }
    
    ESP_LOGI(TAG, "Buffer pool initialized: %d buffers", BUFFER_POOL_SIZE);
}

// 从缓冲池获取一个空闲缓冲区
static int16_t* buffer_pool_get(TickType_t timeout)
{
    int16_t *buf = NULL;
    xQueueReceive(free_buffer_queue, &buf, timeout);
    return buf;
}

// 将缓冲区归还到缓冲池
static void buffer_pool_put(int16_t *buf)
{
    if (buf != NULL) {
        xQueueSend(free_buffer_queue, &buf, 0);
    }
}

// ============ WebSocket 事件处理 ============
static void websocket_event_handler(void *handler_args, esp_event_base_t base, 
                                   int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ WebSocket CONNECTED to %s", WEBSOCKET_URI);
            is_connected = true;
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "❌ WebSocket DISCONNECTED (close code may be in server logs)");
            is_connected = false;
            break;
            
        case WEBSOCKET_EVENT_DATA:
            // 接收服务器返回的识别结果
            if (data->op_code == 0x01) { // 文本帧
                ESP_LOGI(TAG, "📥 收到文本: len=%d, data=%.*s", data->data_len, 
                         data->data_len > 100 ? 100 : data->data_len, data->data_ptr);
            } else if (data->op_code == 0x02) { // 二进制帧
                ESP_LOGD(TAG, "📥 收到二进制: len=%d", data->data_len);
            } else if (data->op_code == 0x09) { // Ping
                ESP_LOGD(TAG, "📥 收到 Ping, 自动回复 Pong");
            } else if (data->op_code == 0x0A) { // Pong
                ESP_LOGD(TAG, "📥 收到 Pong 确认");
            } else {
                ESP_LOGD(TAG, "📥 收到数据: op_code=%d, len=%d", data->op_code, data->data_len);
            }
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "⚠️ WebSocket ERROR");
            // ESP-IDF v5.5.2: error_handle no longer has error_description field
            break;
            
        default:
            ESP_LOGD(TAG, "WebSocket event: %ld", event_id);
            break;
    }
}

static void websocket_init(void)
{
    esp_websocket_client_config_t ws_cfg = {
        .uri = WEBSOCKET_URI,
        .buffer_size = WEBSOCKET_BUF_SIZE,
        .reconnect_timeout_ms = WS_RECONNECT_TIMEOUT_MS,
        // WebSocket 层 Ping/Pong 心跳（关键：解决 10 秒断开问题）
        .ping_interval_sec = WS_PING_INTERVAL_MS / 1000,
        .pingpong_timeout_sec = 3,
        // TCP 层 Keep-Alive（更积极的配置）
        .keep_alive_enable = true,
        .keep_alive_idle = WS_KEEP_ALIVE_IDLE,
        .keep_alive_interval = WS_KEEP_ALIVE_INTERVAL,
        .keep_alive_count = WS_KEEP_ALIVE_COUNT,
    };
    
    ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, 
                                  websocket_event_handler, NULL);
    esp_websocket_client_start(ws_client);
    
    ESP_LOGI(TAG, "WebSocket client initialized, connecting to %s", WEBSOCKET_URI);
    ESP_LOGI(TAG, "Ping interval: %dms, Keep-alive: idle=%ds interval=%ds count=%d",
             WS_PING_INTERVAL_MS, WS_KEEP_ALIVE_IDLE, WS_KEEP_ALIVE_INTERVAL, WS_KEEP_ALIVE_COUNT);
}

// ============ I2S 音频采集 ============
static void i2s_init(void)
{
    // I2S 通道配置
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = I2S_BUFFER_SIZE;
    
    // 创建 I2S 接收通道
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &i2s_rx_chan));
    
    // 标准模式配置
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK,
            .ws = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    // 初始化标准模式
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_chan, &std_cfg));
    
    // 启用通道
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_chan));
    
    ESP_LOGI(TAG, "I2S initialized: %dHz, 16bit, mono", SAMPLE_RATE);
}

// ============ 音频采集任务 ============
static void audio_capture_task(void *pvParameters)
{
    int16_t *audio_buffer = buffer_pool_get(portMAX_DELAY);
    size_t bytes_read;
    
    if (!audio_buffer) {
        ESP_LOGE(TAG, "Failed to get buffer from pool");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Audio capture task started");
    
    while (1) {
        // 从 I2S 读取音频数据
        esp_err_t ret = i2s_channel_read(i2s_rx_chan, audio_buffer, 
                                         I2S_BUFFER_SIZE * sizeof(int16_t),
                                         &bytes_read, portMAX_DELAY);
        
        if (ret == ESP_OK && bytes_read > 0) {
            // 将数据发送到队列（非阻塞）
            if (xQueueSend(audio_queue, &audio_buffer, 0) != pdTRUE) {
                // 队列满，丢弃数据并归还缓冲区
                ESP_LOGW(TAG, "Audio queue full, dropping frame");
                buffer_pool_put(audio_buffer);
            }
            // 从缓冲池获取新缓冲区用于下一次采集
            audio_buffer = buffer_pool_get(pdMS_TO_TICKS(100));
            if (!audio_buffer) {
                ESP_LOGE(TAG, "Buffer pool exhausted");
                break;
            }
        }
    }
    
    buffer_pool_put(audio_buffer);
    vTaskDelete(NULL);
}

// ============ WebSocket 发送任务 ============
static void websocket_send_task(void *pvParameters)
{
    int16_t *audio_data;
    int send_count = 0;
    int send_fail_count = 0;
    int64_t last_log_time = esp_timer_get_time();
    
    ESP_LOGI(TAG, "WebSocket send task started");
    
    while (1) {
        // 从队列获取音频数据（带超时）
        if (xQueueReceive(audio_queue, &audio_data, pdMS_TO_TICKS(100)) == pdTRUE) {
            
            // 检查 WebSocket 连接状态
            bool client_connected = esp_websocket_client_is_connected(ws_client);
            
            if (!is_connected || !client_connected) {
                buffer_pool_put(audio_data);  // 归还缓冲区
                if (!client_connected && is_connected) {
                    ESP_LOGW(TAG, "Client not connected but flag is true, waiting for reconnect...");
                }
                vTaskDelay(pdMS_TO_TICKS(50));  // 等待重连
                continue;
            }
            
            // 发送二进制音频数据（使用配置的超时时间）
            int bytes_to_send = I2S_BUFFER_SIZE * sizeof(int16_t);
            int ret = esp_websocket_client_send_bin(ws_client,
                                                    (const char *)audio_data,
                                                    bytes_to_send,
                                                    pdMS_TO_TICKS(WS_SEND_TIMEOUT_MS));
            
            if (ret < 0) {
                send_fail_count++;
                ESP_LOGW(TAG, "WebSocket send failed: ret=%d, fails=%d", ret, send_fail_count);
                
                // 连续失败多次，可能连接已断开
                if (send_fail_count >= 5) {
                    ESP_LOGE(TAG, "Too many send failures, connection may be broken");
                    send_fail_count = 0;
                }
            } else if (ret != bytes_to_send) {
                // 部分发送（不应该发生，但记录一下）
                ESP_LOGW(TAG, "Partial send: %d/%d bytes", ret, bytes_to_send);
                send_count++;
            } else {
                // 发送成功
                send_count++;
                send_fail_count = 0;  // 重置失败计数
            }
            
            buffer_pool_put(audio_data);  // 归还缓冲区到缓冲池
            
            // 每 10 秒打印一次统计
            int64_t now = esp_timer_get_time();
            if (now - last_log_time > 10000000) { // 10秒
                ESP_LOGI(TAG, "发送统计: %d 帧, 失败: %d, 连接状态: %s", 
                         send_count, send_fail_count, 
                         esp_websocket_client_is_connected(ws_client) ? "OK" : "断开");
                send_count = 0;
                last_log_time = now;
            }
        }
    }
    
    vTaskDelete(NULL);
}

// ============ 主函数 ============
void voice_start(void)
{
    // 创建音频队列（深度 20，防止网络波动导致数据丢失）
    audio_queue = xQueueCreate(20, sizeof(int16_t *));
    if (!audio_queue) {
        ESP_LOGE(TAG, "Failed to create audio queue");
        return;
    }
    
    // 初始化缓冲池（必须在创建任务之前）
    buffer_pool_init();
    if (!free_buffer_queue) {
        ESP_LOGE(TAG, "Failed to initialize buffer pool");
        return;
    }
    
    // 初始化各模块
    websocket_init();
    i2s_init();
    
    // 创建任务
    // 音频采集任务：高优先级，确保实时性
    xTaskCreatePinnedToCore(audio_capture_task, "audio_cap", 4096, NULL, 
                            configMAX_PRIORITIES - 1, NULL, 1);
    
    // WebSocket 发送任务：中等优先级
    xTaskCreatePinnedToCore(websocket_send_task, "ws_send", 4096, NULL, 
                            5, NULL, 0);
    
    ESP_LOGI(TAG, "Voice WebSocket system initialized");
}