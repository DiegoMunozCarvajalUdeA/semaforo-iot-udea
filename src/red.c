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
#include "esp_random.h"
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

// la tarea interna de esp-mqtt (tls, reenvios) queda por debajo
// de tarea_control (4), tarea_sensores (5) y tarea_barreras (6)
#define PRIORIDAD_TAREA_MQTT 3
#define PRIORIDAD_TAREA_RED 3

// un registro solo se da por entregado cuando llega su PUBACK.
// esp-mqtt borra del outbox lo no confirmado a los 30 s, asi que
// no basta con que esp_mqtt_client_publish devuelva un id valido
#define ESPERA_PUBACK_MS 15000
#define LONGITUD_COLA_ACKS 8
#define INTENTOS_RECORDADOS 8

// antes de esta fecha (2024-01-01) se asume que no hay hora sincronizada
#define EPOCA_MINIMA_VALIDA 1704067200LL

#define BIT_WIFI_CONECTADO BIT0
#define BIT_MQTT_CONECTADO BIT1


static const char *TAG_RED = "RED";

static EventGroupHandle_t eventos_red;
static QueueHandle_t cola_red;
static QueueHandle_t cola_acks;
static esp_mqtt_client_handle_t cliente_mqtt;
static volatile bool red_activa = false;

// aleatorio por arranque: (device_id, boot_id, ciclo, calle) identifica
// cada registro aunque ciclo vuelva a 1 tras un reinicio. qos 1 puede
// entregar duplicados y el consumidor los descarta con esta clave
static uint32_t id_arranque;


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
        "{\"device_id\":\"%s\",\"boot_id\":\"%08lx\",\"ciclo\":%lu,\"calle\":\"%c\",\"ts\":%s,"
        "\"distancia_cm\":%.1f,\"prioridad\":%d,\"t_verde_s\":%d,"
        "\"vehiculos\":%d,\"flujo_veh_s\":%.2f}",
        ID_DISPOSITIVO,
        (unsigned long)id_arranque,
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

    case MQTT_EVENT_PUBLISHED:
        // PUBACK recibido: avisar a tarea_red (si la cola se llena se
        // pierde el aviso y el registro solo se reenvia)
        xQueueSend(cola_acks, &evento->msg_id, 0);
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
        .task = {
            .priority = PRIORIDAD_TAREA_MQTT,
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
// esperar el PUBACK de cualquiera de los envios del registro actual.
// los PUBACK de otros mensajes ("online", registros anteriores) se ignoran
// --------------------------------------------------

static bool esperar_puback(
    const int *ids_enviados,
    int cantidad)
{
    TimeOut_t inicio;
    TickType_t restante = pdMS_TO_TICKS(ESPERA_PUBACK_MS);
    int id_confirmado;

    vTaskSetTimeOutState(&inicio);

    while (xTaskCheckForTimeOut(&inicio, &restante) == pdFALSE)
    {
        if (xQueueReceive(cola_acks, &id_confirmado, restante) != pdTRUE)
        {
            break;
        }

        for (int i = 0; i < cantidad; i++)
        {
            if (ids_enviados[i] == id_confirmado)
            {
                return true;
            }
        }
    }

    return false;
}


// --------------------------------------------------
// publicar un registro hasta que el broker confirme (qos 1).
// si no llega PUBACK se reenvia: puede haber duplicados, nunca perdidas
// mientras el registro siga en la cola o en esta funcion
// --------------------------------------------------

static void publicar_hasta_confirmar(
    const registro_calle_t *registro,
    const char *json,
    int longitud)
{
    int ids_enviados[INTENTOS_RECORDADOS];
    int intentos = 0;

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

        if (id_mensaje < 0)
        {
            ESP_LOGW(TAG_RED, "Fallo la publicacion, reintentando");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ids_enviados[intentos % INTENTOS_RECORDADOS] = id_mensaje;
        intentos = intentos + 1;

        int recordados = (intentos < INTENTOS_RECORDADOS) ? intentos : INTENTOS_RECORDADOS;

        if (esperar_puback(ids_enviados, recordados))
        {
            ESP_LOGI(
                TAG_RED,
                "Confirmado por el broker: ciclo %lu calle %c",
                (unsigned long)registro->ciclo,
                registro->calle);
            return;
        }

        ESP_LOGW(
            TAG_RED,
            "Sin PUBACK en %d s, se reenvia ciclo %lu calle %c",
            ESPERA_PUBACK_MS / 1000,
            (unsigned long)registro->ciclo,
            registro->calle);
    }
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

        publicar_hasta_confirmar(&registro, json, longitud);
    }
}


// --------------------------------------------------
// api publica
// --------------------------------------------------

bool red_iniciar(void)
{
    eventos_red = xEventGroupCreate();
    cola_red = xQueueCreate(LONGITUD_COLA_RED, sizeof(registro_calle_t));
    cola_acks = xQueueCreate(LONGITUD_COLA_ACKS, sizeof(int));

    if ((eventos_red == NULL) || (cola_red == NULL) || (cola_acks == NULL))
    {
        ESP_LOGE(TAG_RED, "No se pudieron crear las colas o el grupo de eventos");
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

    // con la radio encendida esp_random() usa ruido real
    id_arranque = esp_random();

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

    if (xTaskCreate(tarea_red, "tarea_red", 4096, NULL, PRIORIDAD_TAREA_RED, NULL) != pdPASS)
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
