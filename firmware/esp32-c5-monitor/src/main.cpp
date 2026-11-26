#include <Arduino.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include <cstring>

#include "frame_filter.h"

namespace {

QueueHandle_t events;
std::atomic<unsigned> dropped{0};
bool running = false;
std::atomic<char> selected_kind{0};
unsigned long last_ping = 0;
unsigned long last_hop = 0;
bool hopping = false;
uint8_t channel = 1;
char command[32];
size_t command_length = 0;
bool command_overflow = false;

void report_error(const char* operation, esp_err_t error);

void receive(void* buffer, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
    const auto* frame = packet->payload;
    if (packet->rx_ctrl.rx_state != 0 || packet->rx_ctrl.sig_len < 4) return;
    const size_t length = packet->rx_ctrl.sig_len - 4;
    MonitorEvent event{};
    if (!decode_monitor_frame(frame, length, selected_kind.load(), event)) return;
    event.channel = packet->rx_ctrl.channel;
    if (xQueueSend(events, &event, 0) != pdTRUE) ++dropped;
}

esp_err_t stop_radio() {
    if (!running) return ESP_OK;
    esp_err_t result = esp_wifi_set_promiscuous(false);
    const esp_err_t stopped = esp_wifi_stop();
    if (stopped != ESP_OK) {
        report_error("stop", stopped);
        Serial.flush();
        ESP.restart();
    }
    running = false;
    hopping = false;
    selected_kind = 0;
    const unsigned lost = dropped.exchange(0);
    if (lost) Serial.printf("DROP %u\n", lost);
    xQueueReset(events);
    return result != ESP_OK ? result : stopped;
}

void report_error(const char* operation, esp_err_t error) {
    Serial.printf("ERROR %s %d\n", operation, static_cast<int>(error));
}

void start_radio(char kind, uint8_t requested_channel, bool hop) {
    if (running) {
        Serial.println("ERROR already-running");
        return;
    }
    xQueueReset(events);
    dropped = 0;
    selected_kind = kind;
    wifi_promiscuous_filter_t filter{};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_err_t result = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (result == ESP_OK) result = esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    if (result == ESP_OK) result = esp_wifi_start();
    if (result != ESP_OK) {
        selected_kind = 0;
        report_error("start", result);
        return;
    }
    running = true;
    result = esp_wifi_set_channel(requested_channel, WIFI_SECOND_CHAN_NONE);
    if (result == ESP_OK) result = esp_wifi_set_promiscuous_filter(&filter);
    if (result == ESP_OK) result = esp_wifi_set_promiscuous_rx_cb(receive);
    if (result == ESP_OK) result = esp_wifi_set_promiscuous(true);
    if (result != ESP_OK) {
        const esp_err_t stop_result = stop_radio();
        report_error("configure", result);
        if (stop_result != ESP_OK) report_error("stop", stop_result);
        return;
    }
    last_ping = millis();
    last_hop = last_ping;
    channel = requested_channel;
    hopping = hop;
    if (hop) Serial.println("ACTIVE H");
    else Serial.printf("ACTIVE %u\n", static_cast<unsigned>(channel));
}

void handle_command() {
    command[command_length] = '\0';
    char kind = 0;
    uint8_t requested_channel = 1;
    bool hop = false;
    if (!strcmp(command, "HELLO")) {
        Serial.println("POSEIDON-C5 1");
    } else if (parse_monitor_start(command, kind, requested_channel, hop)) {
        start_radio(kind, requested_channel, hop);
    } else if (!strcmp(command, "PING")) {
        if (running) {
            last_ping = millis();
            Serial.println("PONG");
        }
    } else if (!strcmp(command, "STOP")) {
        const esp_err_t result = stop_radio();
        if (result == ESP_OK) Serial.println("STOPPED");
        else report_error("stop", result);
    } else {
        Serial.println("ERROR command");
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    esp_log_level_set("*", ESP_LOG_NONE);
    events = xQueueCreate(64, sizeof(MonitorEvent));
    if (!events) {
        Serial.println("ERROR queue");
        return;
    }
    esp_err_t result = esp_netif_init();
    if (result == ESP_OK) result = esp_event_loop_create_default();
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if (result == ESP_OK) result = esp_wifi_init(&config);
    if (result == ESP_OK) result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) {
        report_error("init", result);
        vQueueDelete(events);
        events = nullptr;
    }
}

void loop() {
    for (unsigned i = 0; i < 64 && Serial.available(); ++i) {
        const char ch = static_cast<char>(Serial.read());
        if (ch == '\n') {
            if (events && !command_overflow) handle_command();
            else Serial.println("ERROR unavailable-or-command-too-long");
            command_length = 0;
            command_overflow = false;
        } else if (ch != '\r') {
            if (command_length < sizeof(command) - 1) command[command_length++] = ch;
            else command_overflow = true;
        }
    }
    if (running && millis() - last_ping > 5000) {
        const esp_err_t result = stop_radio();
        if (result == ESP_OK) Serial.println("STOPPED timeout");
        else report_error("timeout-stop", result);
    }
    if (running && hopping && millis() - last_hop >= 1000) {
        const uint8_t next = channel == 1 ? 6 : channel == 6 ? 11 : 1;
        const esp_err_t result = esp_wifi_set_channel(next, WIFI_SECOND_CHAN_NONE);
        if (result != ESP_OK) {
            const esp_err_t stop_result = stop_radio();
            report_error("channel", result);
            if (stop_result != ESP_OK) report_error("stop", stop_result);
        } else {
            channel = next;
            last_hop = millis();
        }
    }
    if (running) {
        const unsigned lost = dropped.exchange(0);
        if (lost) Serial.printf("DROP %u\n", lost);
        MonitorEvent event{};
        for (unsigned i = 0; i < 12 && xQueueReceive(events, &event, 0) == pdTRUE; ++i) {
            if (selected_kind == 'P') {
                if (hopping) Serial.printf("P %u\n", static_cast<unsigned>(event.channel));
                else Serial.println("P");
            } else if (event.protected_frame) {
                Serial.printf("D %02x:%02x:%02x:%02x:%02x:%02x protected",
                              static_cast<unsigned>(event.bssid[0]), static_cast<unsigned>(event.bssid[1]),
                              static_cast<unsigned>(event.bssid[2]), static_cast<unsigned>(event.bssid[3]),
                              static_cast<unsigned>(event.bssid[4]), static_cast<unsigned>(event.bssid[5]));
            } else {
                Serial.printf("D %02x:%02x:%02x:%02x:%02x:%02x %u",
                              static_cast<unsigned>(event.bssid[0]), static_cast<unsigned>(event.bssid[1]),
                              static_cast<unsigned>(event.bssid[2]), static_cast<unsigned>(event.bssid[3]),
                              static_cast<unsigned>(event.bssid[4]), static_cast<unsigned>(event.bssid[5]),
                              static_cast<unsigned>(event.reason));
            }
            if (selected_kind != 'P') {
                if (hopping) Serial.printf(" %u\n", static_cast<unsigned>(event.channel));
                else Serial.println();
            }
        }
    }
    delay(10);
}
