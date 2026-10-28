/*
 * Live dictation: the board's microphone through esp32-pie-asr (Kroko, a streaming English transducer) to a web page
 * over Ethernet. The weights come from the SD card's top folder if it has them, else from the "model" flash partition.
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "asr_service.hpp"
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "transcript.hpp"

static constexpr size_t MODEL_BYTES = 26651392; /* kroko-en-q24-v1.bin */
static constexpr uint32_t MODEL_CRC = 0xbd3ff13c;
static constexpr float MIC_GAIN_DB = 54;        /* pie-asr's best on the EV board's microphone: tune for this one */

extern const char index_html[] asm("_binary_index_html_start");

/* ---- the weights from flash when the card had none, then listening ---- */

static void model_task(void *)
{
    /* asr::run() tries the SD card first: a waiting reason means it found no weights there */
    while (asr::state() == asr::WAITING_MODEL && !*asr::waiting_reason())
        vTaskDelay(pdMS_TO_TICKS(50));
    if (asr::state() == asr::WAITING_MODEL) {
        const esp_partition_t *p =
            esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
        uint8_t *dst = p && p->size >= MODEL_BYTES ? asr::model_load_begin(MODEL_BYTES, "flash") : nullptr;
        bool ok = dst != nullptr;
        for (size_t done = 0; ok && done < MODEL_BYTES;) {
            const size_t k = std::min<size_t>(MODEL_BYTES - done, 1 << 20);
            ok = esp_partition_read(p, done, dst + done, k) == ESP_OK;
            done += k;
            asr::model_load_progress(static_cast<float>(done) / MODEL_BYTES);
        }
        if (dst && !asr::model_load_end(ok, MODEL_CRC))
            printf("no valid weights in the model partition: flash models/kroko-en-q24-v1.bin at 0x310000\n");
    }
    for (asr::State s; (s = asr::state()) == asr::LOADING_MODEL || s == asr::PREPARING;)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (asr::state() == asr::IDLE)
        asr::request_start();
    vTaskDelete(nullptr);
}

/* ---- Ethernet (IP101GRI; the P4's default RMII pins are this board's) ---- */

static void on_ip(void *, esp_event_base_t, int32_t, void *data)
{
    const ip_event_got_ip_t *e = static_cast<ip_event_got_ip_t *>(data);
    printf("dictation page: http://" IPSTR "/\n", IP2STR(&e->ip_info.ip));
}

static void ethernet_start()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_config_t nc = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&nc);
    esp_netif_set_hostname(netif, "dictation");
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = 51;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth;
    ESP_ERROR_CHECK(esp_eth_driver_install(&cfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, nullptr));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}

/* ---- the web page and its transcript feed ---- */

static constexpr size_t CAP = 16384;
static char *s_buf; /* PSRAM; the server runs one handler at a time */

/* s as a JSON string's contents at b[o]; returns the new end */
static size_t esc(char *b, size_t o, const char *s)
{
    for (; *s && o + 8 < CAP; s++) {
        const unsigned char c = *s;
        if (c == '"' || c == '\\')
            b[o++] = '\\', b[o++] = c;
        else if (c == '\n')
            b[o++] = '\\', b[o++] = 'n';
        else if (c < 0x20)
            o += snprintf(b + o, CAP - o, "\\u%04x", c);
        else
            b[o++] = c;
    }
    return o;
}

static esp_err_t page_get(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_sendstr(r, index_html);
}

/* GET /t?g=<generation>&w=<words the page has>: the words after those (reset: start over), the word being written,
 * the state */
static esp_err_t text_get(httpd_req_t *r)
{
    char q[64], v[16];
    uint32_t gen = UINT32_MAX, have = 0;
    if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "g", v, sizeof v) == ESP_OK)
            gen = strtoul(v, nullptr, 10);
        if (httpd_query_key_value(q, "w", v, sizeof v) == ESP_OK)
            have = strtoul(v, nullptr, 10);
    }
    char *b = s_buf;
    size_t o = 0;
    transcript::lock();
    const uint32_t g = transcript::generation(), base = transcript::dropped_words();
    const bool reset = g != gen;
    if (reset)
        have = 0;
    const int n = transcript::words();
    int i = std::min<int>(have > base ? have - base : 0, n); /* words dropped before the page saw them: skipped */
    o += snprintf(b + o, CAP - o, "{\"g\":%u,\"reset\":%d,\"add\":\"", (unsigned)g, reset);
    for (; i < n && o + 1024 < CAP; i++) {
        const transcript::Word &w = transcript::word(i);
        if (i && transcript::word(i - 1).line != w.line)
            b[o++] = '\\', b[o++] = 'n';
        else if (w.space && (i || have))
            b[o++] = ' ';
        o = esc(b, o, transcript::text(w));
    }
    o += snprintf(b + o, CAP - o, "\",\"w\":%u,\"t\":\"", (unsigned)(base + i));
    o = esc(b, o, transcript::tentative());
    transcript::unlock();
    const asr::State st = asr::state();
    o += snprintf(b + o, CAP - o, "\",\"s\":\"%s\",\"x\":%.2f,\"note\":\"", asr::state_name(st),
                  asr::stats().x_real_time);
    o = esc(b, o, st == asr::FAILED ? asr::failure() : st == asr::WAITING_MODEL ? asr::waiting_reason() : "");
    o += snprintf(b + o, CAP - o, "\"}");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, b, o);
}

static void web_start()
{
    s_buf = static_cast<char *>(heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM));
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t h;
    ESP_ERROR_CHECK(httpd_start(&h, &cfg));
    const httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_get, .user_ctx = nullptr};
    const httpd_uri_t text = {.uri = "/t", .method = HTTP_GET, .handler = text_get, .user_ctx = nullptr};
    httpd_register_uri_handler(h, &page);
    httpd_register_uri_handler(h, &text);
}

extern "C" void app_main(void)
{
    asr::Config c;
    c.model_file = "kroko-en-q24-v1.bin";
    c.model_bytes = MODEL_BYTES;
    c.model_crc = MODEL_CRC;
    c.default_phrases = ""; /* no phrase list: nothing is written to the card */
    asr::configure(c);
    transcript::init();
    transcript::set_rules(0.5f, 1.5f); /* pie-asr's: a word is final after 0.5 s, a new line after a 1.5 s pause */
    asr::set_gain(MIC_GAIN_DB);
    ethernet_start();
    web_start();
    xTaskCreate(model_task, "model", 32 * 1024, nullptr, 2, nullptr);
    asr::run(); /* never returns */
}
