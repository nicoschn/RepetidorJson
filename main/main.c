#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include "nvs_flash.h"
#include "esp_event.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include <esp_wifi.h>
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "wifi_provisioning/wifi_config.h"
#include "driver/gpio.h"

static const char *TAG = "BARSTATUS";

static char response_buffer[2048];
static int response_len = 0;

#define MAX_URLS 10
#define URL_LEN 128

// char consulta_url[URL_LEN]; // Main URL buffer
char consulta_urls[MAX_URLS][URL_LEN];
int num_urls = 0;

int consulta_intervalo = 5000; // ms
#define MAX_ETIQUETAS 16

typedef struct
{
    char etiqueta[32];
    int gpio;
    char objeto[32];
} etiqueta_gpio_t;

etiqueta_gpio_t etiquetas[MAX_ETIQUETAS];
int num_etiquetas = 0;
char gpio_activo_tag[MAX_ETIQUETAS][32] = {0}; // Guarda el tag que activó cada GPIO
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#include "esp_http_server.h"
void rele_init();
void LoadConfig();
esp_err_t index_html_handler(httpd_req_t *req)
{
    FILE *f = fopen("/spiffs/index.html", "r");
    if (!f)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "index.html not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); // End response
    return ESP_OK;
}
esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0)
        return ESP_FAIL;
    buf[ret] = 0;

    cJSON *json = cJSON_Parse(buf);
    if (!json)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_FAIL;
    }

    cJSON *jintervalo = cJSON_GetObjectItem(json, "intervalo");
    cJSON *jetiquetas = cJSON_GetObjectItem(json, "etiquetas"); // <-- array de etiquetas
    cJSON *jurls = cJSON_GetObjectItem(json, "urls");           // <-- array de URLs

    if (cJSON_IsNumber(jintervalo) && cJSON_IsArray(jetiquetas) && cJSON_IsArray(jurls))
    {
        consulta_intervalo = jintervalo->valueint;

        // Etiquetas
        num_etiquetas = MIN(cJSON_GetArraySize(jetiquetas), MAX_ETIQUETAS);
        for (int i = 0; i < num_etiquetas; i++)
        {
            cJSON *item = cJSON_GetArrayItem(jetiquetas, i);
            cJSON *jet = cJSON_GetObjectItem(item, "etiqueta");
            cJSON *jgp = cJSON_GetObjectItem(item, "gpio");
            cJSON *job = cJSON_GetObjectItem(item, "objeto"); // Nuevo campo

            if (cJSON_IsString(jet) && cJSON_IsNumber(jgp) && cJSON_IsString(job))
            {
                strncpy(etiquetas[i].etiqueta, jet->valuestring, sizeof(etiquetas[i].etiqueta) - 1);
                etiquetas[i].etiqueta[sizeof(etiquetas[i].etiqueta) - 1] = 0;
                etiquetas[i].gpio = jgp->valueint;
                strncpy(etiquetas[i].objeto, job->valuestring, sizeof(etiquetas[i].objeto) - 1);
                etiquetas[i].objeto[sizeof(etiquetas[i].objeto) - 1] = 0;
                gpio_pad_select_gpio(etiquetas[i].gpio);
                gpio_set_direction(etiquetas[i].gpio, GPIO_MODE_OUTPUT);
                gpio_set_level(etiquetas[i].gpio, 0);
            }
        }

        // URLs
        num_urls = MIN(cJSON_GetArraySize(jurls), MAX_URLS);
        for (int i = 0; i < num_urls; i++)
        {
            cJSON *jurl = cJSON_GetArrayItem(jurls, i);
            if (cJSON_IsString(jurl))
            {
                strncpy(consulta_urls[i], jurl->valuestring, URL_LEN - 1);
                consulta_urls[i][URL_LEN - 1] = 0;
            }
        }

        // Guardar en NVS
        nvs_handle_t nvs;
        if (nvs_open("config", NVS_READWRITE, &nvs) == ESP_OK)
        {
            nvs_set_i32(nvs, "intervalo", consulta_intervalo);

            // Etiquetas
            cJSON *arr_et = cJSON_CreateArray();
            for (int i = 0; i < num_etiquetas; i++)
            {
                cJSON *obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "etiqueta", etiquetas[i].etiqueta);
                cJSON_AddNumberToObject(obj, "gpio", etiquetas[i].gpio);
                cJSON_AddStringToObject(obj, "objeto", etiquetas[i].objeto); // Nuevo campo
                cJSON_AddItemToArray(arr_et, obj);
            }
            char *etiquetas_json = cJSON_PrintUnformatted(arr_et);
            nvs_set_str(nvs, "etiquetas", etiquetas_json);
            cJSON_Delete(arr_et);
            free(etiquetas_json);

            // URLs
            cJSON *arr_urls = cJSON_CreateArray();
            for (int i = 0; i < num_urls; i++)
            {
                cJSON_AddItemToArray(arr_urls, cJSON_CreateString(consulta_urls[i]));
            }
            char *urls_json = cJSON_PrintUnformatted(arr_urls);
            nvs_set_str(nvs, "urls", urls_json);
            cJSON_Delete(arr_urls);
            free(urls_json);

            nvs_commit(nvs);
            nvs_close(nvs);
    
        }
        httpd_resp_sendstr(req, "Configuracion actualizada");
        // recargar la configuracion en memoria
        LoadConfig();
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid fields");
    }
    cJSON_Delete(json);
    return ESP_OK;
}
esp_err_t config_get_handler(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "intervalo", consulta_intervalo);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < num_etiquetas; i++)
    {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "etiqueta", etiquetas[i].etiqueta);
        cJSON_AddNumberToObject(obj, "gpio", etiquetas[i].gpio);
        cJSON_AddStringToObject(obj, "objeto", etiquetas[i].objeto); // Nuevo campo
        cJSON_AddItemToArray(arr, obj);
    }
    cJSON_AddItemToObject(json, "etiquetas", arr);

    cJSON *arr_urls = cJSON_CreateArray();
    for (int i = 0; i < num_urls; i++)
    {
        cJSON_AddItemToArray(arr_urls, cJSON_CreateString(consulta_urls[i]));
    }
    cJSON_AddItemToObject(json, "urls", arr_urls);

    const char *resp = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);

    cJSON_Delete(json);
    free((void *)resp);
    return ESP_OK;
}
#include "esp_spiffs.h"

void init_spiffs()
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true};
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
}
#include "dirent.h"
void listar_spiffs()
{
    DIR *dir = opendir("/spiffs");
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        ESP_LOGI(TAG, "Archivo SPIFFS: %s", ent->d_name);
    }
    closedir(dir);
}
void start_webserver()
{
    init_spiffs();
    listar_spiffs();
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_start(&server, &config);

    // Sirve index.html en "/"
    httpd_uri_t uri_html = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_html_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &uri_html);

    // Configuración GET en "/api/config"
    httpd_uri_t uri_get = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &uri_get);

    // Configuración POST en "/api/config"
    httpd_uri_t uri_post = {
        .uri = "/api/config",
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
        for (int u = 0; u < num_urls; u++)
        {
            response_len = 0;
            memset(response_buffer, 0, sizeof(response_buffer));
            esp_http_client_config_t config = {
                .url = consulta_urls[u],
                .timeout_ms = 5000,
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
                ESP_LOGI(TAG, "Received Buffer: %s", response_buffer);

                if (CodeStatus == 200 && response_len > 0)
                {
                    cJSON *json = cJSON_Parse(response_buffer);
                    if (json)
                    {
                        for (int i = 0; i < num_etiquetas; i++)
                        {
                            cJSON *objeto_json = cJSON_GetObjectItem(json, etiquetas[i].objeto);
                            if (objeto_json && cJSON_IsObject(objeto_json))
                            {
                                cJSON *campo = cJSON_GetObjectItem(objeto_json, etiquetas[i].etiqueta);
                                if (campo && cJSON_IsBool(campo))
                                {
                                    int valor = cJSON_IsTrue(campo) ? 1 : 0;
                                    int gpio = etiquetas[i].gpio;

                                    if (valor == 1)
                                    {
                                        // Si se activa, guarda el tag
                                        gpio_set_level(gpio, 1);
                                        strncpy(gpio_activo_tag[i], etiquetas[i].etiqueta, sizeof(gpio_activo_tag[i]) - 1);
                                        gpio_activo_tag[i][sizeof(gpio_activo_tag[i]) - 1] = 0;
                                        ESP_LOGI(TAG, "GPIO %d (%s/%s): ON", gpio, etiquetas[i].objeto, etiquetas[i].etiqueta);
                                    }
                                    else
                                    {
                                        // Solo apaga si el tag que lo activó es el mismo
                                        if (strcmp(gpio_activo_tag[i], etiquetas[i].etiqueta) == 0)
                                        {
                                            gpio_set_level(gpio, 0);
                                            gpio_activo_tag[i][0] = 0; // Limpia el registro
                                            ESP_LOGI(TAG, "GPIO %d (%s/%s): OFF", gpio, etiquetas[i].objeto, etiquetas[i].etiqueta);
                                        }
                                        else
                                        {
                                            ESP_LOGI(TAG, "GPIO %d: Ignorado OFF por tag distinto (%s)", gpio, etiquetas[i].etiqueta);
                                        }
                                    }
                                }
                            }
                        }
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
        }
        vTaskDelay(pdMS_TO_TICKS(consulta_intervalo));
    }
}
static TaskHandle_t barstatus_task_handle = NULL;

void rele_init()
{
    for (int i = 0; i < num_etiquetas; i++)
    {
        if (etiquetas[i].gpio < 0 || etiquetas[i].gpio > 39)
        {
            ESP_LOGW(TAG, "GPIO %d fuera de rango, se ignora", etiquetas[i].gpio);
            continue;
        }
        gpio_pad_select_gpio(etiquetas[i].gpio);
        gpio_set_direction(etiquetas[i].gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(etiquetas[i].gpio, 0);
    }
}
#include <esp_netif.h>

/* Event handler for catching system events */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
#ifdef CONFIG_EXAMPLE_RESET_PROV_MGR_ON_FAILURE
    static int retries;
#endif
    if (event_base == WIFI_PROV_EVENT)
    {
        switch (event_id)
        {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started");
            break;
        case WIFI_PROV_CRED_RECV:
        {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "Received Wi-Fi credentials"
                          "\n\tSSID     : %s\n\tPassword : %s",
                     (const char *)wifi_sta_cfg->ssid,
                     (const char *)wifi_sta_cfg->password);
            break;
        }
        case WIFI_PROV_CRED_FAIL:
        {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s"
                          "\n\tPlease reset to factory and retry provisioning",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
#ifdef CONFIG_EXAMPLE_RESET_PROV_MGR_ON_FAILURE
            retries++;
            if (retries >= CONFIG_EXAMPLE_PROV_MGR_MAX_RETRY_CNT)
            {
                ESP_LOGI(TAG, "Failed to connect with provisioned AP, reseting provisioned credentials");
                wifi_prov_mgr_reset_sm_state_on_failure();
                retries = 0;
            }
#endif
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
#ifdef CONFIG_EXAMPLE_RESET_PROV_MGR_ON_FAILURE
            retries = 0;
#endif
            break;
        case WIFI_PROV_END:
            /* De-initialize manager once provisioning is finished */
            wifi_prov_mgr_deinit();
            esp_restart(); // Restart the device after provisioning
            break;
        default:
            break;
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        /* Signal main application to continue execution */
        gpio_set_level(GPIO_NUM_22, 1);
        if (barstatus_task_handle == NULL)
        {
            xTaskCreate(&fetch_and_log_barstatus, "fetch_barstatus", 4096, NULL, 5, &barstatus_task_handle);
            LoadConfig();
            start_webserver();
        }
        // xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        gpio_set_level(GPIO_NUM_22, 0);
        ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
        esp_wifi_connect();
    }
}

static void wifi_init_sta(void)
{
    /* Start Wi-Fi in station mode */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}
static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    const char *ssid_prefix = "PROV_";
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

/* Handler for the optional provisioning endpoint registered by the application.
 * The data format can be chosen by applications. Here, we are using plain ascii text.
 * Applications can choose to use other formats like protobuf, JSON, XML, etc.
 */
esp_err_t custom_prov_data_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                   uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    if (inbuf)
    {
        ESP_LOGI(TAG, "Received data: %.*s", inlen, (char *)inbuf);
    }
    char response[] = "SUCCESS";
    *outbuf = (uint8_t *)strdup(response);
    if (*outbuf == NULL)
    {
        ESP_LOGE(TAG, "System out of memory");
        return ESP_ERR_NO_MEM;
    }
    *outlen = strlen(response) + 1; /* +1 for NULL terminating byte */

    return ESP_OK;
}
void app_wifi_init(void)
{
    /* Initialize NVS partition */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        /* NVS partition was truncated
         * and needs to be erased */
        ESP_ERROR_CHECK(nvs_flash_erase());

        /* Retry nvs_flash_init */
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Initialize TCP/IP */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Initialize the event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // wifi_event_group = xEventGroupCreate();

    /* Register our event handler for Wi-Fi, IP and Provisioning related events */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    /* Initialize Wi-Fi including netif with default config */

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // Obtener la MAC y formar el hostname
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "Efaisa-Rep-%02X%02X", mac[4], mac[5]);

    esp_netif_set_hostname(sta_netif, hostname);
    /* Configuration for the provisioning manager */
    wifi_prov_mgr_config_t config = {
        /* What is the Provisioning Scheme that we want ?
         * wifi_prov_scheme_softap or wifi_prov_scheme_ble */

        .scheme = wifi_prov_scheme_ble,

        /* Any default scheme specific event handler that you would
         * like to choose. Since our example application requires
         * neither BT nor BLE, we can choose to release the associated
         * memory once provisioning is complete, or not needed
         * (in case when device is already provisioned). Choosing
         * appropriate scheme specific event handler allows the manager
         * to take care of this automatically. This can be set to
         * WIFI_PROV_EVENT_HANDLER_NONE when using wifi_prov_scheme_softap*/
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM

    };

    /* Initialize provisioning manager with the
     * configuration parameters set above */
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    /* If device is not yet provisioned start provisioning service */
    if (!provisioned)
    {
        ESP_LOGI(TAG, "Starting provisioning");

        /* What is the Device Service Name that we want
         * This translates to :
         *     - Wi-Fi SSID when scheme is wifi_prov_scheme_softap
         *     - device name when scheme is wifi_prov_scheme_ble
         */
        char service_name[12];
        get_device_service_name(service_name, sizeof(service_name));

        /* What is the security level that we want (0 or 1):
         *      - WIFI_PROV_SECURITY_0 is simply plain text communication.
         *      - WIFI_PROV_SECURITY_1 is secure communication which consists of secure handshake
         *          using X25519 key exchange and proof of possession (pop) and AES-CTR
         *          for encryption/decryption of messages.
         */
        wifi_prov_security_t security = WIFI_PROV_SECURITY_1;

        /* Do we want a proof-of-possession (ignored if Security 0 is selected):
         *      - this should be a string with length > 0
         *      - NULL if not used
         */
        const char *pop = "abcd1234";

        /* What is the service key (could be NULL)
         * This translates to :
         *     - Wi-Fi password when scheme is wifi_prov_scheme_softap
         *          (Minimum expected length: 8, maximum 64 for WPA2-PSK)
         *     - simply ignored when scheme is wifi_prov_scheme_ble
         */
        const char *service_key = NULL;

        /* This step is only useful when scheme is wifi_prov_scheme_ble. This will
         * set a custom 128 bit UUID which will be included in the BLE advertisement
         * and will correspond to the primary GATT service that provides provisioning
         * endpoints as GATT characteristics. Each GATT characteristic will be
         * formed using the primary service UUID as base, with different auto assigned
         * 12th and 13th bytes (assume counting starts from 0th byte). The client side
         * applications must identify the endpoints by reading the User Characteristic
         * Description descriptor (0x2901) for each characteristic, which contains the
         * endpoint name of the characteristic */
        uint8_t custom_service_uuid[] = {
            /* LSB <---------------------------------------
             * ---------------------------------------> MSB */
            0xb4,
            0xdf,
            0x5a,
            0x1c,
            0x3f,
            0x6b,
            0xf4,
            0xbf,
            0xea,
            0x4a,
            0x82,
            0x03,
            0x04,
            0x90,
            0x1a,
            0x02,
        };

        /* If your build fails with linker errors at this point, then you may have
         * forgotten to enable the BT stack or BTDM BLE settings in the SDK (e.g. see
         * the sdkconfig.defaults in the example project) */
        wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);

        /* An optional endpoint that applications can create if they expect to
         * get some additional custom data during provisioning workflow.
         * The endpoint name can be anything of your choice.
         * This call must be made before starting the provisioning.
         */
        wifi_prov_mgr_endpoint_create("custom-data");
        /* Start provisioning service */
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key));

        /* The handler for the optional endpoint created above.
         * This call must be made after starting the provisioning, and only if the endpoint
         * has already been created above.
         */
        wifi_prov_mgr_endpoint_register("custom-data", custom_prov_data_handler, NULL);
    }
    else
    {
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");

        /* We don't need the manager as device is already provisioned,
         * so let's release it's resources */
        wifi_prov_mgr_deinit();

        /* Start Wi-Fi station */
        wifi_init_sta();
    }

    /* Wait for Wi-Fi connection */

    // xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, false, true, portMAX_DELAY);

    // Eliminar tarea de LEDs
    // if (provisioned){   }
    // stop_led_task = true;
}

void LoadConfig()
{
    nvs_handle_t nvs;
    int32_t tmp_intervalo = consulta_intervalo;

    if (nvs_open("config", NVS_READONLY, &nvs) == ESP_OK)
    {
        // Cargar intervalo
        nvs_get_i32(nvs, "intervalo", &tmp_intervalo);
        consulta_intervalo = tmp_intervalo;

        // Cargar etiquetas
        char etiquetas_json[512] = {0};
        size_t len_et = sizeof(etiquetas_json);
        if (nvs_get_str(nvs, "etiquetas", etiquetas_json, &len_et) == ESP_OK)
        {
            cJSON *arr = cJSON_Parse(etiquetas_json);
            if (arr && cJSON_IsArray(arr))
            {
                num_etiquetas = MIN(cJSON_GetArraySize(arr), MAX_ETIQUETAS);
                for (int i = 0; i < num_etiquetas; i++)
                {
                    cJSON *item = cJSON_GetArrayItem(arr, i);
                    cJSON *jet = cJSON_GetObjectItem(item, "etiqueta");
                    cJSON *jgp = cJSON_GetObjectItem(item, "gpio");
                    cJSON *job = cJSON_GetObjectItem(item, "objeto");
                    if (cJSON_IsString(jet) && cJSON_IsNumber(jgp) && cJSON_IsString(job))
                    {
                        strncpy(etiquetas[i].etiqueta, jet->valuestring, sizeof(etiquetas[i].etiqueta) - 1);
                        etiquetas[i].etiqueta[sizeof(etiquetas[i].etiqueta) - 1] = 0;
                        etiquetas[i].gpio = jgp->valueint;
                        strncpy(etiquetas[i].objeto, job->valuestring, sizeof(etiquetas[i].objeto) - 1);
                        etiquetas[i].objeto[sizeof(etiquetas[i].objeto) - 1] = 0;
                    }
                }
            }
            cJSON_Delete(arr);
        }

        // Cargar URLs
        char urls_json[1024] = {0};
        size_t len_urls = sizeof(urls_json);
        if (nvs_get_str(nvs, "urls", urls_json, &len_urls) == ESP_OK)
        {
            cJSON *arr = cJSON_Parse(urls_json);
            if (arr && cJSON_IsArray(arr))
            {
                num_urls = MIN(cJSON_GetArraySize(arr), MAX_URLS);
                for (int i = 0; i < num_urls; i++)
                {
                    cJSON *jurl = cJSON_GetArrayItem(arr, i);
                    if (cJSON_IsString(jurl))
                    {
                        strncpy(consulta_urls[i], jurl->valuestring, URL_LEN - 1);
                        consulta_urls[i][URL_LEN - 1] = 0;
                    }
                }
            }
            cJSON_Delete(arr);
        }

        nvs_close(nvs);
        rele_init();
    }
}
void app_main(void)
{
    app_wifi_init();
}
