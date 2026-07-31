/*
 * Capstone — Dual-core IPC pipeline
 * ============================================================
 * Theme: Satellite SAT-RKH-1
 * ============================================================
 */

#ifndef USE_WEBSERVER
#define USE_WEBSERVER 1
#endif

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

#include "esp_random.h"

#if USE_WEBSERVER
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#endif

#define BUTTON_GPIO GPIO_NUM_18

#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define CONFIG_LOG_MAXIMUM_LEVEL  5

static const char *TAG = "SAT-RKH-1";

/* ---------- IPC objects (created in app_main, used everywhere) ---------- */
static QueueHandle_t      data_q;        
static EventGroupHandle_t evt_group;
static TaskHandle_t       responder_handle;

/* Event-group bit definitions */
#define EV_BIT_DATA_PRODUCED  (1 << 0)
#define EV_BIT_DATA_PROCESSED (1 << 1)

/* Per-task heartbeats — proof of life for the monitor. Single 32-bit reads are
 * atomic on Xtensa, so the monitor can read these without a lock (App 6's topic). */
static volatile uint32_t hb_prod, hb_cons, hb_coord, hb_resp;
static uint64_t wcet_prod_us, wcet_cons_us, wcet_coord_us, wcet_resp_us;
/* consistent period method
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(10);
    vTaskDelayUntil(&last, period);
*/

#define MEASURE_WCET(_max_var, ...) do {                       \
    int64_t _t0 = esp_timer_get_time();                          \
    __VA_ARGS__;                                                      \
    int64_t _dt = esp_timer_get_time() - _t0;                    \
    if ((uint64_t)_dt > (_max_var)) (_max_var) = (uint64_t)_dt;  \
} while (0)

/* Satellite Telemetry Packet */
typedef struct {
    uint32_t timestamp_ms;
    uint16_t packet_id;
    int16_t  bus_voltage_mv;   /* e.g., 3300 mV */
    int8_t   temp_celsius;     /* e.g., ambient sensor reading */
} sat_telemetry_t;

/* Shared state for web monitor */
static sat_telemetry_t   last_item_global;
static SemaphoreHandle_t last_item_mutex;

//static SemaphoreHandle_t btn_sem;

static volatile int64_t isr_entry_time_us;
static volatile uint64_t latency_max_us;

/* ---------- Producer task (Core 1) ----------*/
static void temp_sensor_task(void *arg)
{
    uint16_t packet_counter = 0;

    for (;;) {
      
        sat_telemetry_t packet;

        /* Pure workload measurement */
        MEASURE_WCET(wcet_prod_us, {
            volatile float filtered_temp = 0.0f;
            for(int i = 0; i < 500; i++) {
                filtered_temp += (float)i * 0.015f; 
            }

            packet.timestamp_ms   = (uint32_t)(esp_timer_get_time() / 1000);
            packet.packet_id      = ++packet_counter;
            packet.bus_voltage_mv = 3300 + (int)(esp_random() % 100) - 50;
            packet.temp_celsius   = 20 + (int)(esp_random() % 35);
        });

            if (xQueueSend(data_q, &packet, pdMS_TO_TICKS(10)) == pdTRUE) {
                xEventGroupSetBits(evt_group, EV_BIT_DATA_PRODUCED);
            } else {
                ESP_LOGW(TAG, "Sensor Queue full, Dropping packet #%u", packet.packet_id);
                while (uxQueueSpacesAvailable(data_q) < 10) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            hb_prod++;

        vTaskDelay(pdMS_TO_TICKS(50)); /* 20 Hz loop */
    }
}

/* ---------- Consumer task (Core 1) ----------*/
static void temp_monitor_task(void *arg)
{
    sat_telemetry_t packet;

    for (;;) {
        /* Block waiting for queue items up to 100ms */
        if (xQueueReceive(data_q, &packet, pdMS_TO_TICKS(100)) == pdTRUE) {
            
            MEASURE_WCET(wcet_cons_us, {
                /* PADDING: Simulate data decryption and CRC-32 checksum generation */
                volatile uint32_t checksum = 0xFFFFFFFF;
                for (int i = 0; i < 1000; i++) {
                    checksum ^= (i << 2);
                    checksum = (checksum >> 1) | (checksum << 31); /* bitwise rotation */
                }
            });
                if (packet.temp_celsius > 50) {
                    ESP_LOGW(TAG, "Thermal threshold warning on packet #%u: %d C at time %lu", 
                             packet.packet_id, packet.temp_celsius, (unsigned long)packet.timestamp_ms);
                }

                if (last_item_mutex != NULL) {
                    xSemaphoreTake(last_item_mutex, portMAX_DELAY);
                    last_item_global = packet; 
                    xSemaphoreGive(last_item_mutex);
                }

                xEventGroupSetBits(evt_group, EV_BIT_DATA_PROCESSED);
                hb_cons++;
            
        }
    }
}

/* ---------- Coordinator task (Core 1) ----------
 * Waits for BOTH event bits to be set, then signals the responder via direct
 * task notification.
 */
static void log_task(void *arg)
{
    const EventBits_t wait_mask = EV_BIT_DATA_PRODUCED | EV_BIT_DATA_PROCESSED;
    for (;;) {
        EventBits_t got = xEventGroupWaitBits(evt_group, wait_mask,
                                              pdTRUE,   /* clear on exit */
                                              pdTRUE,   /* wait for ALL */
                                              portMAX_DELAY);
        if ((got & wait_mask) == wait_mask) {
          MEASURE_WCET(wcet_coord_us, {
            /* PADDING: Simulate formatting log data into 4KB Flash memory blocks */
            volatile int flash_hash = 0x55AA55AA;
            for(int i = 0; i < 800; i++) {
              flash_hash = (flash_hash * 33) ^ i;
            }
          });
            ESP_LOGI(TAG, "Internal Check Cycle Complete, Sending Data to Ground Control"); 
            xTaskNotifyGive(responder_handle);
            hb_coord++;
        }
    }
}

/* ---------- Responder task (Core 1) ----------
 * Wakes via direct task notification from coordinator OR from button ISR.
 */
static void antenna_task(void *arg)
{
    for (;;) {
        uint32_t n = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (n == 0) continue;
        MEASURE_WCET(wcet_resp_us, {
            /* PADDING: Simulate RF Forward Error Correction (FEC) interleaving */
            volatile uint8_t fec_buffer[128] = {0};
            for(int i = 0; i < 128; i++) {
                for(int j = 0; j < 15; j++) {
                    fec_buffer[i] ^= (uint8_t)(j * 7);
                }
            }
        });
            ESP_LOGI(TAG, "Antenna transmitting, Ground Control notified (count=%lu)", (unsigned long)n);
            hb_resp++;
        
        //int64_t wake = esp_timer_get_time();
        //int64_t lat = wake - isr_entry_time_us;
        //if ((uint64_t)lat > latency_max_us) latency_max_us = (uint64_t)lat;

        /*ESP_LOGI(TAG, "Antenna transmitting, Groung Control notified (count=%lu) notify latency=%lld us (max=%llu)", 
        (unsigned long)n, (long long)lat, (unsigned long long)latency_max_us);*/

        /*if (xSemaphoreTake(btn_sem, portMAX_DELAY) == pdTRUE) {
          int64_t wake = esp_timer_get_time();
          int64_t lat = wake - isr_entry_time_us;
          if ((uint64_t)lat > latency_max_us) latency_max_us = (uint64_t)lat;
          ESP_LOGI(TAG, "Antenna transmitting, Groung Control notified, semaphore latency=%lld us (max=%llu)", 
          (long long)lat, (unsigned long long)latency_max_us);
          hb_resp++;
        }*/
    }
}

/* ---------- Button ISR — notify responder directly ---------- */
static volatile int64_t last_edge_us;
static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - last_edge_us < 200000) return;
    last_edge_us = now;
    //isr_entry_time_us = now;

    BaseType_t woken = pdFALSE;
    //xSemaphoreGiveFromISR(btn_sem, &woken);
    vTaskNotifyGiveFromISR(responder_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

#if USE_WEBSERVER
/* ---------- Web monitor task (Core 0)  [USE_WEBSERVER = 1] ----------*/

/* ---------- Web Server Configuration ---------- */
#define HTTP_PORT         80
#define WIFI_SSID         "Wokwi-GUEST"
#define WIFI_PASS         ""             /* Wokwi virtual AP is open */

/* ---------- HTTP handler: live JSON state ---------- 
 * Returns JSON containing queue depth, event bits, and per-task heartbeats.
 */
static esp_err_t handle_state(httpd_req_t *req)
{
    /* INCREASED BUFFER: JSON string is much longer now to accomodate WCET */
    char buf[512]; 
    UBaseType_t depth = uxQueueMessagesWaiting(data_q);
    EventBits_t bits  = xEventGroupGetBits(evt_group);
    
    sat_telemetry_t local_item = {0};
    
    if (last_item_mutex != NULL) {
        xSemaphoreTake(last_item_mutex, portMAX_DELAY);
        local_item = last_item_global; 
        xSemaphoreGive(last_item_mutex);
    }

    int n = snprintf(buf, sizeof(buf),
        "{\"q_depth\":%u,\"evt_bits\":\"0x%02x\","
        "\"hb_p\":%lu,\"hb_c\":%lu,\"hb_co\":%lu,\"hb_r\":%lu,"
        "\"wcet_p\":%llu,\"wcet_c\":%llu,\"wcet_co\":%llu,\"wcet_r\":%llu,"
        "\"t_stamp\":%lu,\"p_id\":%u,\"bus_v\":%d,\"temp_c\":%d}",
        (unsigned)depth, (unsigned)bits,
        (unsigned long)hb_prod, (unsigned long)hb_cons,
        (unsigned long)hb_coord, (unsigned long)hb_resp,
        (unsigned long long)wcet_prod_us, (unsigned long long)wcet_cons_us,
        (unsigned long long)wcet_coord_us, (unsigned long long)wcet_resp_us,
        (unsigned long)local_item.timestamp_ms, (unsigned)local_item.packet_id, 
        (int)local_item.bus_voltage_mv, (int)local_item.temp_celsius);
        
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* ---------- HTTP handler: root page (HTML shell only) ---------- */
static esp_err_t handle_root(httpd_req_t *req)
{
    static const char html[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\"><head>"
        "<meta charset=\"utf-8\">"
        "<title>SAT-RKH-1 IPC Monitor</title>"
        "<style>"
        "  body { font-family: -apple-system, sans-serif; background: #FAFAF5; color: #1A1A1A; padding: 2rem; }"
        "  h1 { color: #6B4F09; border-bottom: 3px solid #FFC904; padding-bottom: 4px; }"
        "  .container { display: flex; flex-direction: row; gap: 2rem; align-items: flex-start; }"
        "  .panel { flex: 1; }"
        "  h2 { color: #4A3A05; font-size: 1.2rem; margin-bottom: 0.5rem; }"
        "  table { border-collapse: collapse; width: 100%; background: white; box-shadow: 0 1px 3px rgba(0,0,0,0.1); }"
        "  th, td { text-align: left; padding: 12px; border-bottom: 1px solid #eee; }"
        "  th { color: #6B4F09; background: #fdfdf9; }"
        "  .meta { font-variant-numeric: tabular-nums; font-weight: 600; }"
        "</style></head>"
        "<body>"
        "<h1>SAT-RKH-1 IPC Monitor</h1>"
        
        "<div class=\"container\">"
        
        "  <div class=\"panel\">"
        "    <h2>Pipeline & Telemetry</h2>"
        "    <table>"
        "      <tr><th>Queue Depth</th><td id=\"q_depth\" class=\"meta\">0</td></tr>"
        "      <tr><th>Last Packet ID</th><td id=\"p_id\" class=\"meta\">-</td></tr>"
        "      <tr><th>Timestamp (ms)</th><td id=\"t_stamp\" class=\"meta\">-</td></tr>"
        "      <tr><th>Bus Voltage (mV)</th><td id=\"bus_v\" class=\"meta\">-</td></tr>"
        "      <tr><th>Temp (°C)</th><td id=\"temp_c\" class=\"meta\">-</td></tr>"
        "      <tr><th>Event Bits</th><td id=\"evt_bits\" class=\"meta\">0x00</td></tr>"
        "    </table>"
        "  </div>"

        "  <div class=\"panel\">"
        "    <h2>System Health & Synchronization</h2>"
        "    <table>"
        "      <tr><th>Task Node</th><th>Heartbeat</th><th>WCET (&micro;s)</th></tr>"
        "      <tr><td>Temp Sensor (Prod)</td><td id=\"hb_p\" class=\"meta\">0</td><td id=\"wcet_p\" class=\"meta\">0</td></tr>"
        "      <tr><td>Temp Monitor (Cons)</td><td id=\"hb_c\" class=\"meta\">0</td><td id=\"wcet_c\" class=\"meta\">0</td></tr>"
        "      <tr><td>Coordinator (Log)</td><td id=\"hb_co\" class=\"meta\">0</td><td id=\"wcet_co\" class=\"meta\">0</td></tr>"
        "      <tr><td>Antenna (Resp)</td><td id=\"hb_r\" class=\"meta\">0</td><td id=\"wcet_r\" class=\"meta\">0</td></tr>"
        "    </table>"
        "  </div>"
        
        "</div>"

        "<div style=\"margin-top: 1rem; color: #666;\">"
        "  <p>Polling at ~1 Hz via JSON endpoint.</p>"
        "</div>"

        "<script>"
        "async function poll(){"
        "  try{"
        "    const r = await fetch('/state',{cache:'no-store'});"
        "    const s = await r.json();"
        "    document.getElementById('q_depth').textContent = s.q_depth;"
        "    document.getElementById('p_id').textContent = s.p_id;"
        "    document.getElementById('t_stamp').textContent = s.t_stamp;"
        "    document.getElementById('bus_v').textContent = s.bus_v;"
        "    document.getElementById('temp_c').textContent = s.temp_c;"
        "    document.getElementById('evt_bits').textContent = s.evt_bits;"
        
        "    document.getElementById('hb_p').textContent = s.hb_p;"
        "    document.getElementById('hb_c').textContent = s.hb_c;"
        "    document.getElementById('hb_co').textContent = s.hb_co;"
        "    document.getElementById('hb_r').textContent = s.hb_r;"
        
        "    document.getElementById('wcet_p').textContent = s.wcet_p;"
        "    document.getElementById('wcet_c').textContent = s.wcet_c;"
        "    document.getElementById('wcet_co').textContent = s.wcet_co;"
        "    document.getElementById('wcet_r').textContent = s.wcet_r;"
        "  }catch(e){}"
        "}"
        "setInterval(poll, 1000);"
        "poll();"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---------- Start Webserver ---------- */
static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.core_id = 0;                    /* networking on Core 0 */
    cfg.task_priority = 5;
    cfg.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = handle_root,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t state = {
            .uri = "/state",
            .method = HTTP_GET,
            .handler = handle_state,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &state);

        ESP_LOGI(TAG, "HTTP server started on port %d", HTTP_PORT);
    } else {
        ESP_LOGE(TAG, "HTTP server failed to start");
    }
    return server;
}

/* ---------- Wi-Fi event handler ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        start_webserver();
    }
}

/* ---------- Wi-Fi Init ---------- */
static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------- Web monitor task (Core 0)  [USE_WEBSERVER = 1] ---------- */
static void webmonitor_task(void *arg)
{
    ESP_LOGI(TAG, "[webmon] Starting Web Server (USE_WEBSERVER=1)");
    
    /* Initialize Wi-Fi and trigger the webserver setup once connected */
    wifi_init_sta();

    /* Let the task idle; all routing is handled by the httpd background tasks */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#else
/* ---------- Serial monitor task (Core 0)  [USE_WEBSERVER = 0] ----------
 * Provided and working. Prints the same state the web monitor will show, so the
 * pipeline is observable in Wokwi with no Wi-Fi. This is your baseline; the web
 * monitor (USE_WEBSERVER=1) renders the identical fields over HTTP.
 */
static void serial_monitor_task(void *arg)
{
    for (;;) {
        UBaseType_t depth = uxQueueMessagesWaiting(data_q);
        EventBits_t bits  = xEventGroupGetBits(evt_group);
        ESP_LOGI(TAG,
                 "[monitor] q_depth=%u  evt=0x%02x  hb: prod=%lu cons=%lu coord=%lu resp=%lu",
                 (unsigned)depth, (unsigned)bits,
                 (unsigned long)hb_prod, (unsigned long)hb_cons,
                 (unsigned long)hb_coord, (unsigned long)hb_resp);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
#endif /* USE_WEBSERVER */

/* ---------- app_main ---------- */
void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "==== SAT-RKH-1 starting — IPC pipeline ====");

    /* Initialize the mutex before it is used by any task */
    last_item_mutex = xSemaphoreCreateMutex();
    
    //btn_sem = xSemaphoreCreateBinary();

#if USE_WEBSERVER
    ESP_LOGI(TAG, "Monitor: WEB (USE_WEBSERVER=1) — implement webmonitor_task (Core 0)");
#else
    ESP_LOGI(TAG, "Monitor: SERIAL (USE_WEBSERVER=0) — Core-0 summary once/sec, no Wi-Fi");
#endif

    /* Queue Depth: 20 items
    * Item Size: sizeof(sat_telemetry_t)
    */
    data_q = xQueueCreate(20, sizeof(sat_telemetry_t));

    evt_group = xEventGroupCreate();

    /* Tasks on Core 1 (real-time plane). 8192-byte stacks: any task that calls
     * ESP_LOGI needs headroom for the vprintf formatting (2048 overflows). */
    xTaskCreatePinnedToCore(antenna_task,   "antenna",   8192, NULL, 12, &responder_handle, APP_CPU_NUM);
    xTaskCreatePinnedToCore(temp_sensor_task,    "sensor",   8192, NULL,  8, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(temp_monitor_task,    "monitor",   8192, NULL,  8, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(log_task, "log",  8192, NULL,  9, NULL, APP_CPU_NUM);

    /* Observability plane on Core 0 (networking plane) */
#if USE_WEBSERVER
    xTaskCreatePinnedToCore(webmonitor_task,    "webmon",  8192, NULL, 4, NULL, PRO_CPU_NUM);
#else
    xTaskCreatePinnedToCore(serial_monitor_task, "serialmon", 4096, NULL, 4, NULL, PRO_CPU_NUM);
#endif

    /* Button ISR */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);
}
