#include "settings.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

static void str_setting(nvs_handle_t h, const char *key, const char *compiled, char *out, size_t n)
{
    out[0] = 0;
    size_t len = n;
    bool have = nvs_get_str(h, key, out, &len) == ESP_OK;
    if (compiled && compiled[0] && (!have || strcmp(out, compiled))) {
        strlcpy(out, compiled, n);
        nvs_set_str(h, key, out);
        ESP_LOGI(TAG, "%s: saved from build config", key);
    } else if (!have) {
        ESP_LOGW(TAG, "%s: not set", key);
    }
}

void settings_load(settings_t *s)
{
    memset(s, 0, sizeof *s);
    nvs_handle_t h;
    if (nvs_open("intercom", NVS_READWRITE, &h) != ESP_OK) { ESP_LOGE(TAG, "nvs_open failed"); return; }
    str_setting(h, "ssid", CONFIG_INTERCOM_WIFI_SSID, s->ssid, sizeof s->ssid);
    str_setting(h, "pass", CONFIG_INTERCOM_WIFI_PASSWORD, s->pass, sizeof s->pass);
    str_setting(h, "token", CONFIG_INTERCOM_TOKEN, s->token, sizeof s->token);
    str_setting(h, "bus_host", CONFIG_INTERCOM_BUS_HOST, s->bus_host, sizeof s->bus_host);
    str_setting(h, "bus_ip", CONFIG_INTERCOM_BUS_FALLBACK_IP, s->bus_ip, sizeof s->bus_ip);
    str_setting(h, "ssid2", CONFIG_INTERCOM_WIFI2_SSID, s->ssid2, sizeof s->ssid2);
    str_setting(h, "pass2", CONFIG_INTERCOM_WIFI2_PASSWORD, s->pass2, sizeof s->pass2);
    str_setting(h, "bus_url", "", s->remote_url, sizeof s->remote_url);   /* NVS only: set with POST /bus */
    char link[8] = "";
    size_t ll = sizeof link;
    if (nvs_get_str(h, "link", link, &ll) == ESP_OK) {
        s->ble_link = !strcmp(link, "ble");
        nvs_erase_key(h, "link");                     /* one boot only */
        ESP_LOGW(TAG, "link: %s for this boot", s->ble_link ? "PHONE (BLE only)" : "AUTO");
    }
    uint16_t port = 0;
    if (nvs_get_u16(h, "bus_port", &port) != ESP_OK || port != CONFIG_INTERCOM_BUS_PORT) {
        port = CONFIG_INTERCOM_BUS_PORT;
        nvs_set_u16(h, "bus_port", port);
    }
    s->bus_port = port;
    nvs_commit(h);
    nvs_close(h);
}

bool settings_save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("intercom", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = (val && val[0]) ? nvs_set_str(h, key, val) : nvs_erase_key(h, key);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool settings_link_ble_next_boot(void) { return settings_save_str("link", "ble"); }
