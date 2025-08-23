#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "wifi_provisioning/wifi_config.h"
#include "driver/gpio.h"
#define WIFI_SSID "EFAISA24"
#define WIFI_PASS "1q2w3e4r5t6y"

static const char *TAG = "BARSTATUS";

static char response_buffer[4096];
static int response_len = 0;

char consulta_url[128] = "http://192.168.88.196/api/barstatus";
int consulta_intervalo = 5000; // ms
char etiqueta_json[32] = "ALARMA";
int rele_gpio = 1;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#include "esp_http_server.h"
void rele_init();
esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0)
        return ESP_FAIL;
    buf[ret] = 0;

    // Espera formato: url=...&intervalo=...&etiqueta=...&gpio=...
    sscanf(buf, "url=%127[^&]&intervalo=%d&etiqueta=%31[^&]&gpio=%d", consulta_url, &consulta_intervalo, etiqueta_json, &rele_gpio);
    rele_init();

    httpd_resp_sendstr(req, "Configuracion actualizada");
    return ESP_OK;
}

esp_err_t config_get_handler(httpd_req_t *req)
{
    char resp[1024];
    snprintf(resp, sizeof(resp),
             "<form method='POST'>"
             "URL: <input name='url' value='%s'><br>"
             "Intervalo (ms): <input name='intervalo' value='%d'><br>"
             "Etiqueta JSON: <input name='etiqueta' value='%s'><br>"
             "GPIO Rele: <input name='gpio' value='%d'><br>"
             "<input type='submit'></form>",
             consulta_url, consulta_intervalo, etiqueta_json, rele_gpio);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

void start_webserver()
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_start(&server, &config);

    httpd_uri_t uri_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = config_get_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &uri_get);

    httpd_uri_t uri_post = {
        .uri = "/",
        .method = HTTP_POST,
        .handler = config_post_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &uri_post);
}
esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        // Copia los datos recibidos, chunked o no

        if (response_len + evt->data_len < sizeof(response_buffer))
        {
            memcpy(response_buffer + response_len, evt->data, evt->data_len);
            response_len += evt->data_len;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

void fetch_and_log_barstatus(void *pvParameters)
{
    while (1)
    {
        response_len = 0;
        memset(response_buffer, 0, sizeof(response_buffer));
        esp_http_client_config_t config = {
            .url = consulta_url,
            .timeout_ms = 10000,
            .event_handler = _http_event_handler,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "User-Agent", "ESP32");
        esp_http_client_set_header(client, "Accept", "application/json");
        esp_err_t err = esp_http_client_perform(client);

        if (err == ESP_OK)
        {
            int CodeStatus = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "HTTP GET Status = %d", CodeStatus);
            ESP_LOGI(TAG, "Total bytes read: %d", response_len);
            // ESP_LOGI(TAG, "Received Buffer: %s", response_buffer);

            if (CodeStatus == 200 && response_len > 0)
            {
                cJSON *json = cJSON_Parse(response_buffer);
                if (json)
                {
                    cJSON *barstatus = cJSON_GetObjectItem(json, "barstatus");
                    cJSON *field = NULL;
                    if (barstatus && cJSON_IsObject(barstatus))
                    {
                        cJSON *campo = cJSON_GetObjectItem(barstatus, etiqueta_json);
                        if (campo && cJSON_IsBool(campo))
                        {
                            gpio_set_level(rele_gpio, cJSON_IsTrue(campo) ? 1 : 0);
                            ESP_LOGI(TAG, "Rele %d: %s", rele_gpio, cJSON_IsTrue(campo) ? "ON" : "OFF");
                        }

                        field = cJSON_GetObjectItem(barstatus, "ALARMA");
                        ESP_LOGI(TAG, "ALARMA: %s", cJSON_IsBool(field) ? (cJSON_IsTrue(field) ? "true" : "false") : "N/A");
                        field = cJSON_GetObjectItem(barstatus, "FALLA");
                        ESP_LOGI(TAG, "FALLA: %s", cJSON_IsBool(field) ? (cJSON_IsTrue(field) ? "true" : "false") : "N/A");
                        field = cJSON_GetObjectItem(barstatus, "DESCONEXION");
                        ESP_LOGI(TAG, "DESCONEXION: %s", cJSON_IsBool(field) ? (cJSON_IsTrue(field) ? "true" : "false") : "N/A");
                        field = cJSON_GetObjectItem(barstatus, "TIERRA");
                        ESP_LOGI(TAG, "TIERRA: %s", cJSON_IsBool(field) ? (cJSON_IsTrue(field) ? "true" : "false") : "N/A");
                        field = cJSON_GetObjectItem(barstatus, "TEST");
                        ESP_LOGI(TAG, "TEST: %s", cJSON_IsBool(field) ? (cJSON_IsTrue(field) ? "true" : "false") : "N/A");
                        field = cJSON_GetObjectItem(barstatus, "BATERIA");
                        ESP_LOGI(TAG, "BATERIA: %d", cJSON_IsNumber(field) ? field->valueint : -1);
                        field = cJSON_GetObjectItem(barstatus, "ALIMENTACION");
                        ESP_LOGI(TAG, "ALIMENTACION: %s", cJSON_IsBool(field) ? (cJSON_IsTrue(field) ? "true" : "false") : "N/A");
                        field = cJSON_GetObjectItem(barstatus, "RED");
                        ESP_LOGI(TAG, "RED: %s", cJSON_IsBool(field) ? (cJSON_IsTrue(field) ? "true" : "false") : "N/A");
                        field = cJSON_GetObjectItem(barstatus, "EXTINCION");
                        ESP_LOGI(TAG, "EXTINCION: %s", cJSON_IsBool(field) ? (cJSON_IsTrue(field) ? "true" : "false") : "N/A");
                        field = cJSON_GetObjectItem(barstatus, "SIREN");
                        ESP_LOGI(TAG, "SIREN: %s", cJSON_IsBool(field) ? (cJSON_IsTrue(field) ? "true" : "false") : "N/A");
                        field = cJSON_GetObjectItem(barstatus, "FIRMWARE");
                        ESP_LOGI(TAG, "FIRMWARE: %d", cJSON_IsNumber(field) ? field->valueint : -1);
                    }
                    field = cJSON_GetObjectItem(json, "MAC");
                    ESP_LOGI(TAG, "MAC: %s", cJSON_IsString(field) ? field->valuestring : "N/A");
                    cJSON_Delete(json);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to parse JSON");
                }
            }
            else
            {
                ESP_LOGE(TAG, "Empty response or non-200 status");
            }
        }
        else
        {
            ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
        vTaskDelay(pdMS_TO_TICKS(consulta_intervalo));
    }
}

static TaskHandle_t barstatus_task_handle = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "WiFi STA Start: Connecting...");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG, "WiFi Disconnected: Reconnecting...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected to WiFi, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (barstatus_task_handle == NULL)
        {
            xTaskCreate(&fetch_and_log_barstatus, "fetch_barstatus", 4096 * 2, NULL, 5, &barstatus_task_handle);
            start_webserver();
        }
    }
}

void wifi_init_sta(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler,
                                        NULL,
                                        &instance_got_ip);

    wifi_config_t wifi_config = { 0 };

    // Leer credenciales de NVS (guardadas por el provisioning BLE)
    size_t ssid_len = sizeof(wifi_config.sta.ssid);
    size_t pass_len = sizeof(wifi_config.sta.password);
    nvs_handle_t nvs_handle;
    if (nvs_open("wifi", NVS_READONLY, &nvs_handle) == ESP_OK) {
        nvs_get_str(nvs_handle, "ssid", (char *)wifi_config.sta.ssid, &ssid_len);
        nvs_get_str(nvs_handle, "pass", (char *)wifi_config.sta.password, &pass_len);
        nvs_close(nvs_handle);
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
}

void start_ble_provisioning()
{
    // ESP_ERROR_CHECK(esp_netif_init());
    // ESP_ERROR_CHECK(esp_event_loop_create_default());
    // esp_netif_create_default_wifi_sta();

    // Inicializa el driver WiFi antes del provisioning manager
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE};

    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    const char *pop = "abcd1234";
    const char *service_name = "PROV_ESP32";
    const char *service_key = NULL;

    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key));
}

void rele_init()
{
    gpio_pad_select_gpio(rele_gpio);
    gpio_set_direction(rele_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(rele_gpio, 0);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta(); // Solo aquí

    bool provisioned = false;
    wifi_prov_mgr_is_provisioned(&provisioned);

    if (!provisioned)
    {
        ESP_LOGI(TAG, "Starting BLE provisioning...");
        start_ble_provisioning();

        do
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            wifi_prov_mgr_is_provisioned(&provisioned);
        } while (!provisioned);

        ESP_LOGI(TAG, "Provisioning finished, deinit and start WiFi STA...");
        wifi_prov_mgr_deinit();
        wifi_init_sta();
    }
    else
    {
        ESP_LOGI(TAG, "Already provisioned, starting WiFi STA...");
        wifi_init_sta();
    }
}
