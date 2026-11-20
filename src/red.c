#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#include "red.h"

#if __has_include("config_red.h")
#include "config_red.h"
#else
#error "Falta src/config_red.h: copie src/config_red.example.h a src/config_red.h y ajuste los valores"
#endif


// --------------------------------------------------
// certificados embebidos en el firmware (ver CMakeLists.txt)
// --------------------------------------------------

extern const char ca_crt_inicio[] asm("_binary_ca_crt_start");
extern const char esp32_crt_inicio[] asm("_binary_esp32_crt_start");
extern const char esp32_key_inicio[] asm("_binary_esp32_key_start");


// --------------------------------------------------
// topicos
// --------------------------------------------------

#define TOPICO_TELEMETRIA TOPICO_BASE "/telemetria"
#define TOPICO_ESTADO TOPICO_BASE "/estado"


// --------------------------------------------------
// parametros
// --------------------------------------------------

#define LONGITUD_COLA_RED 16
#define KEEPALIVE_MQTT_S 30
#define TAMANO_JSON 256

// antes de esta fecha (2024-01-01) se asume que no hay hora sincronizada
#define EPOCA_MINIMA_VALIDA 1704067200LL

#define BIT_WIFI_CONECTADO BIT0
#define BIT_MQTT_CONECTADO BIT1


static const char *TAG_RED = "RED";

static EventGroupHandle_t eventos_red;
static QueueHandle_t cola_red;
static esp_mqtt_client_handle_t cliente_mqtt;
static bool red_activa = false;


// --------------------------------------------------
// hora actual en ms (0 si sntp aun no sincroniza)
// --------------------------------------------------

static int64_t marca_tiempo_ms(void)
{
    struct timeval ahora;

    gettimeofday(&ahora, NULL);

    if ((int64_t)ahora.tv_sec < EPOCA_MINIMA_VALIDA)
    {
        return 0;
    }

    return (int64_t)ahora.tv_sec * 1000 + ahora.tv_usec / 1000;
}


// --------------------------------------------------
// convertir registro a json
// --------------------------------------------------

static int formatear_json(
    const registro_calle_t *registro,
    char *json,
    size_t tamano)
{
    char ts[24];

    if (registro->ts_ms > 0)
    {
        snprintf(ts, sizeof(ts), "%lld", (long long)registro->ts_ms);
    }
    else
    {
        // sin hora: la raspberry usa la hora de llegada
        strcpy(ts, "null");
    }

    return snprintf(
        json,
        tamano,
        "{\"device_id\":\"%s\",\"ciclo\":%lu,\"calle\":\"%c\",\"ts\":%s,"
        "\"distancia_cm\":%.1f,\"prioridad\":%d,\"t_verde_s\":%d,"
        "\"vehiculos\":%d,\"flujo_veh_s\":%.2f}",
        ID_DISPOSITIVO,
        (unsigned long)registro->ciclo,
        registro->calle,
        ts,
        registro->distancia_cm,
        registro->prioridad,
        registro->tiempo_verde_s,
        registro->vehiculos,
        registro->flujo);
}


// --------------------------------------------------
// eventos wifi
// --------------------------------------------------

static void manejar_wifi(
    void *argumento,
    esp_event_base_t base,
    int32_t id,
    void *datos)
{
    if ((base == WIFI_EVENT) && (id == WIFI_EVENT_STA_START))
    {
        esp_wifi_connect();
    }
    else if ((base == WIFI_EVENT) && (id == WIFI_EVENT_STA_DISCONNECTED))
    {
        xEventGroupClearBits(eventos_red, BIT_WIFI_CONECTADO);
        ESP_LOGW(TAG_RED, "WiFi desconectado, reintentando");
        esp_wifi_connect();
    }
    else if ((base == IP_EVENT) && (id == IP_EVENT_STA_GOT_IP))
    {
        ip_event_got_ip_t *evento = (ip_event_got_ip_t *)datos;

        ESP_LOGI(TAG_RED, "IP obtenida: " IPSTR, IP2STR(&evento->ip_info.ip));
        xEventGroupSetBits(eventos_red, BIT_WIFI_CONECTADO);
    }
}


// --------------------------------------------------
// eventos mqtt
// --------------------------------------------------

static void manejar_mqtt(
    void *argumento,
    esp_event_base_t base,
    int32_t id,
    void *datos)
{
    esp_mqtt_event_handle_t evento = (esp_mqtt_event_handle_t)datos;

    switch ((esp_mqtt_event_id_t)id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG_RED, "Conectado al broker con mTLS");

        // estado retenido; el broker publica "offline" (last will)
        // si la conexion se pierde sin desconexion limpia
        esp_mqtt_client_publish(evento->client, TOPICO_ESTADO, "online", 0, 1, 1);
        xEventGroupSetBits(eventos_red, BIT_MQTT_CONECTADO);
        break;

    case MQTT_EVENT_DISCONNECTED:
        xEventGroupClearBits(eventos_red, BIT_MQTT_CONECTADO);
        ESP_LOGW(TAG_RED, "Desconectado del broker");
        break;

    case MQTT_EVENT_ERROR:
        if (evento->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGE(
                TAG_RED,
                "Error de transporte/TLS: esp_tls=0x%x tls_stack=0x%x errno=%d",
                (int)evento->error_handle->esp_tls_last_esp_err,
                (int)evento->error_handle->esp_tls_stack_err,
                (int)evento->error_handle->esp_transport_sock_errno);
        }
        else if (evento->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        {
            ESP_LOGE(
                TAG_RED,
                "El broker rechazo la conexion: codigo %d",
                (int)evento->error_handle->connect_return_code);
        }
        break;

    default:
        break;
    }
}


// --------------------------------------------------
// inicializar wifi en modo estacion
// --------------------------------------------------

static esp_err_t iniciar_wifi(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG_RED, "esp_netif_init");

    esp_err_t resultado = esp_event_loop_create_default();

    if ((resultado != ESP_OK) && (resultado != ESP_ERR_INVALID_STATE))
    {
        return resultado;
    }

    if (esp_netif_create_default_wifi_sta() == NULL)
    {
        return ESP_FAIL;
    }

    wifi_init_config_t configuracion_inicial = WIFI_INIT_CONFIG_DEFAULT();

    ESP_RETURN_ON_ERROR(esp_wifi_init(&configuracion_inicial), TAG_RED, "esp_wifi_init");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &manejar_wifi, NULL, NULL),
        TAG_RED,
        "registro eventos wifi");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &manejar_wifi, NULL, NULL),
        TAG_RED,
        "registro eventos ip");

    wifi_config_t configuracion_wifi = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG_RED, "esp_wifi_set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &configuracion_wifi), TAG_RED, "esp_wifi_set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG_RED, "esp_wifi_start");

    return ESP_OK;
}


// --------------------------------------------------
// inicializar cliente mqtt con tls mutuo
// --------------------------------------------------

static esp_err_t iniciar_mqtt(void)
{
    const esp_mqtt_client_config_t configuracion = {
        .broker = {
            .address.uri = URI_BROKER,
            .verification = {
                // ca que firmo el certificado del broker
                .certificate = ca_crt_inicio,
                .common_name = NOMBRE_SERVIDOR_TLS,
            },
        },
        .credentials = {
            .client_id = ID_DISPOSITIVO,
            .authentication = {
                // certificado y llave propios del esp32 (la parte "mutua")
                .certificate = esp32_crt_inicio,
                .key = esp32_key_inicio,
            },
        },
        .session = {
            .keepalive = KEEPALIVE_MQTT_S,
            .last_will = {
                .topic = TOPICO_ESTADO,
                .msg = "offline",
                .qos = 1,
                .retain = 1,
            },
        },
    };

    cliente_mqtt = esp_mqtt_client_init(&configuracion);

    if (cliente_mqtt == NULL)
    {
        return ESP_FAIL;
    }

    // tarea_red arranca el cliente cuando haya wifi
    return esp_mqtt_client_register_event(cliente_mqtt, ESP_EVENT_ANY_ID, manejar_mqtt, NULL);
}


// --------------------------------------------------
// tarea de red: saca registros de la cola y los publica
// --------------------------------------------------

static void tarea_red(void *pvParameters)
{
    registro_calle_t registro;
    char json[TAMANO_JSON];

    // arrancar mqtt solo con wifi; despues el cliente reconecta solo
    xEventGroupWaitBits(
        eventos_red,
        BIT_WIFI_CONECTADO,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);

    esp_err_t resultado = esp_mqtt_client_start(cliente_mqtt);

    if (resultado != ESP_OK)
    {
        ESP_LOGE(TAG_RED, "esp_mqtt_client_start: %s", esp_err_to_name(resultado));
        vTaskDelete(NULL);
    }

    while (1)
    {
        if (xQueueReceive(cola_red, &registro, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        int longitud = formatear_json(&registro, json, sizeof(json));

        if ((longitud < 0) || (longitud >= (int)sizeof(json)))
        {
            ESP_LOGE(TAG_RED, "JSON demasiado largo, registro descartado");
            continue;
        }

        // reintentar hasta que el broker acepte el mensaje
        while (1)
        {
            xEventGroupWaitBits(
                eventos_red,
                BIT_MQTT_CONECTADO,
                pdFALSE,
                pdTRUE,
                portMAX_DELAY);

            int id_mensaje = esp_mqtt_client_publish(
                cliente_mqtt,
                TOPICO_TELEMETRIA,
                json,
                longitud,
                1,
                0);

            if (id_mensaje >= 0)
            {
                ESP_LOGI(TAG_RED, "Publicado ciclo %lu calle %c", (unsigned long)registro.ciclo, registro.calle);
                break;
            }

            ESP_LOGW(TAG_RED, "Fallo la publicacion, reintentando");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}


// --------------------------------------------------
// api publica
// --------------------------------------------------

bool red_iniciar(void)
{
    eventos_red = xEventGroupCreate();
    cola_red = xQueueCreate(LONGITUD_COLA_RED, sizeof(registro_calle_t));

    if ((eventos_red == NULL) || (cola_red == NULL))
    {
        ESP_LOGE(TAG_RED, "No se pudo crear la cola o el grupo de eventos");
        return false;
    }

    esp_err_t resultado = nvs_flash_init();

    if ((resultado == ESP_ERR_NVS_NO_FREE_PAGES) || (resultado == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        nvs_flash_erase();
        resultado = nvs_flash_init();
    }

    if (resultado != ESP_OK)
    {
        ESP_LOGE(TAG_RED, "nvs_flash_init: %s", esp_err_to_name(resultado));
        return false;
    }

    if (iniciar_wifi() != ESP_OK)
    {
        return false;
    }

    esp_sntp_config_t configuracion_sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(SERVIDOR_NTP);

    if (esp_netif_sntp_init(&configuracion_sntp) != ESP_OK)
    {
        // sin hora los mensajes salen con "ts": null
        ESP_LOGW(TAG_RED, "No se pudo iniciar SNTP");
    }

    if (iniciar_mqtt() != ESP_OK)
    {
        ESP_LOGE(TAG_RED, "No se pudo iniciar el cliente MQTT");
        return false;
    }

    // prioridad 3: por debajo de control, sensores y barreras
    if (xTaskCreate(tarea_red, "tarea_red", 4096, NULL, 3, NULL) != pdPASS)
    {
        ESP_LOGE(TAG_RED, "No se pudo crear tarea_red");
        return false;
    }

    red_activa = true;

    return true;
}


void red_publicar_calle(const registro_calle_t *registro)
{
    if (!red_activa)
    {
        return;
    }

    registro_calle_t copia = *registro;

    copia.ts_ms = marca_tiempo_ms();

    if (xQueueSend(cola_red, &copia, 0) != pdTRUE)
    {
        registro_calle_t descartado;

        xQueueReceive(cola_red, &descartado, 0);
        xQueueSend(cola_red, &copia, 0);

        ESP_LOGW(TAG_RED, "Cola de red llena, se descarta el registro mas antiguo");
    }
}
