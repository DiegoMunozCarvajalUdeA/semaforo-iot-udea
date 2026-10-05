#include <stdio.h>
#include <math.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"


// --------------------------------------------------
// pines de las barreras
// --------------------------------------------------

#define PIN_BARRERA_A GPIO_NUM_33
#define PIN_BARRERA_B GPIO_NUM_32


// --------------------------------------------------
// semaforo calle a
// --------------------------------------------------

#define PIN_ROJO_A      GPIO_NUM_25
#define PIN_AMARILLO_A  GPIO_NUM_26
#define PIN_VERDE_A     GPIO_NUM_27


// --------------------------------------------------
// semaforo calle b
// --------------------------------------------------

#define PIN_ROJO_B      GPIO_NUM_14
#define PIN_AMARILLO_B  GPIO_NUM_12
#define PIN_VERDE_B     GPIO_NUM_13


// --------------------------------------------------
// sensores sharp
//
// gpio34 = adc1 canal 6
// gpio35 = adc1 canal 7
// --------------------------------------------------

#define CANAL_SHARP_A ADC_CHANNEL_6
#define CANAL_SHARP_B ADC_CHANNEL_7


// --------------------------------------------------
// barreras
// --------------------------------------------------

#define NIVEL_ACTIVO_BARRERA_A 0
#define NIVEL_ACTIVO_BARRERA_B 0


// --------------------------------------------------
// parametros
// --------------------------------------------------

#define NUM_MUESTRAS_MEDIA 10

#define TIEMPO_AMARILLO 2
#define TIEMPO_TODO_ROJO 1

#define LIMITE_FLUJO_ALTO 0.40f


// --------------------------------------------------
// adc
// --------------------------------------------------

static adc_oneshot_unit_handle_t adc1_handle;


// --------------------------------------------------
// mutex
// --------------------------------------------------

static SemaphoreHandle_t mutex_datos;


// --------------------------------------------------
// variables compartidas
// --------------------------------------------------

static float distancia_a = 80.0f;
static float distancia_b = 80.0f;

static int vehiculos_ciclo_a = 0;
static int vehiculos_ciclo_b = 0;

static float flujo_anterior_a = 0.0f;
static float flujo_anterior_b = 0.0f;

static bool verde_a_activo = false;
static bool verde_b_activo = false;


// --------------------------------------------------
// etiquetas
// --------------------------------------------------

static const char *TAG_SHARP = "SHARP";
static const char *TAG_BARRERA = "BARRERA";
static const char *TAG_CONTROL = "CONTROL";


// --------------------------------------------------
// convertir voltaje a distancia aproximada
// --------------------------------------------------

static float calcular_distancia(float voltaje)
{
    if (voltaje < 0.35f)
    {
        return 80.0f;
    }

    float distancia =
        26.7f * powf(voltaje, -1.175f);


    if (distancia < 10.0f)
    {
        distancia = 10.0f;
    }


    if (distancia > 80.0f)
    {
        distancia = 80.0f;
    }


    return distancia;
}


// --------------------------------------------------
// controlar semaforo a
// --------------------------------------------------

static void poner_semaforo_a(
    int rojo,
    int amarillo,
    int verde)
{
    gpio_set_level(PIN_ROJO_A, rojo);
    gpio_set_level(PIN_AMARILLO_A, amarillo);
    gpio_set_level(PIN_VERDE_A, verde);
}


// --------------------------------------------------
// controlar semaforo b
// --------------------------------------------------

static void poner_semaforo_b(
    int rojo,
    int amarillo,
    int verde)
{
    gpio_set_level(PIN_ROJO_B, rojo);
    gpio_set_level(PIN_AMARILLO_B, amarillo);
    gpio_set_level(PIN_VERDE_B, verde);
}


// --------------------------------------------------
// calcular prioridad usando cola y flujo juntos
// --------------------------------------------------

static int calcular_prioridad(
    float distancia,
    float flujo)
{
    bool cola_grande;
    bool cola_media;
    bool flujo_alto;

    cola_grande = distancia <= 25.0f;
    cola_media = (distancia > 25.0f) && (distancia <= 45.0f);
    flujo_alto = flujo >= LIMITE_FLUJO_ALTO;

    // cola grande + flujo alto

    if (cola_grande && flujo_alto)
    {
        return 5;
    }

    // cola grande + flujo bajo

    if (cola_grande && !flujo_alto)
    {
        return 4;
    }

    // cola media + flujo alto

    if (cola_media && flujo_alto)
    {
        return 4;
    }

    // cola media + flujo bajo

    if (cola_media && !flujo_alto)
    {
        return 3;
    }

    // cola pequeña + flujo alto

    if ((!cola_grande) && (!cola_media) && flujo_alto)
    {
        return 2;
    }


    // cola pequeña + flujo bajo

    return 1;
}


// --------------------------------------------------
// calcular tiempo de verde
// --------------------------------------------------

static int calcular_tiempo_verde(
    int prioridad_propia,
    int prioridad_otra)
{
    int diferencia = prioridad_propia - prioridad_otra;
    int tiempo_verde;

    if (diferencia >= 2)
    {
        tiempo_verde = 14;
    }
    else if (diferencia == 1)
    {
        tiempo_verde = 12;
    }
    else if (diferencia == 0)
    {
        tiempo_verde = 10;
    }
    else if (diferencia == -1)
    {
        tiempo_verde = 8;
    }
    else
    {
        tiempo_verde = 6;
    }

    return tiempo_verde;
}


// --------------------------------------------------
// tarea de los dos sharp
// --------------------------------------------------

static void tarea_sensores(void *pvParameters)
{
    int muestras_a[NUM_MUESTRAS_MEDIA] = {0};
    int muestras_b[NUM_MUESTRAS_MEDIA] = {0};

    int indice_a = 0;
    int indice_b = 0;

    int cantidad_a = 0;
    int cantidad_b = 0;

    int suma_a = 0;
    int suma_b = 0;


    while (1)
    {
        int raw_a = 0;
        int raw_b = 0;

        esp_err_t resultado_a =
            adc_oneshot_read(
                adc1_handle,
                CANAL_SHARP_A,
                &raw_a);

        esp_err_t resultado_b =
            adc_oneshot_read(
                adc1_handle,
                CANAL_SHARP_B,
                &raw_b);


        // sharp a

        if (resultado_a == ESP_OK)
        {
            suma_a = suma_a - muestras_a[indice_a];
            muestras_a[indice_a] = raw_a;
            suma_a = suma_a + raw_a;
            indice_a = indice_a + 1;


            if (indice_a >= NUM_MUESTRAS_MEDIA)
            {
                indice_a = 0;
            }

            if (cantidad_a < NUM_MUESTRAS_MEDIA)
            {
                cantidad_a = cantidad_a + 1;
            }
        }

        // sharp b

        if (resultado_b == ESP_OK)
        {
            suma_b = suma_b - muestras_b[indice_b];
            muestras_b[indice_b] = raw_b;
            suma_b =suma_b + raw_b;
            indice_b = indice_b + 1;

            if (indice_b >= NUM_MUESTRAS_MEDIA)
            {
                indice_b = 0;
            }

            if (cantidad_b < NUM_MUESTRAS_MEDIA)
            {
                cantidad_b = cantidad_b + 1;
            }
        }

        // calcular valores filtrados

        if ((cantidad_a > 0) && (cantidad_b > 0))
        {
            float promedio_a = (float)suma_a / (float)cantidad_a;
            float promedio_b = (float)suma_b / (float)cantidad_b;
            float voltaje_a = promedio_a * 3.3f / 4095.0f;
            float voltaje_b = promedio_b * 3.3f / 4095.0f;
            float nueva_distancia_a = calcular_distancia(voltaje_a);
            float nueva_distancia_b = calcular_distancia(voltaje_b);

            if (xSemaphoreTake(
                    mutex_datos,
                    pdMS_TO_TICKS(20)) == pdTRUE)
            {
                distancia_a = nueva_distancia_a;
                distancia_b = nueva_distancia_b;
                xSemaphoreGive(mutex_datos);
            }


            ESP_LOGI(
                TAG_SHARP,
                "A: %.1f cm | B: %.1f cm",
                nueva_distancia_a,
                nueva_distancia_b);
        }


        vTaskDelay(
            pdMS_TO_TICKS(500));
    }
}


// --------------------------------------------------
// tarea de las dos barreras
// --------------------------------------------------

static void tarea_barreras(void *pvParameters)
{
    int anterior_a = gpio_get_level(PIN_BARRERA_A);
    int anterior_b = gpio_get_level(PIN_BARRERA_B);
    TickType_t ultima_a = 0;
    TickType_t ultima_b = 0;

    while (1)
    {
        int actual_a = gpio_get_level(PIN_BARRERA_A);
        int actual_b = gpio_get_level(PIN_BARRERA_B);
        TickType_t ahora = xTaskGetTickCount();

        // barrera a

        if ((actual_a == NIVEL_ACTIVO_BARRERA_A) && (anterior_a != NIVEL_ACTIVO_BARRERA_A))
        {
            if ((ultima_a == 0) || ((ahora - ultima_a) > pdMS_TO_TICKS(200)))
            {
                int total_a = 0;
                bool contado_a = false;

                if (xSemaphoreTake(mutex_datos, pdMS_TO_TICKS(20)) == pdTRUE)
                {
                    if (verde_a_activo)
                    {
                        vehiculos_ciclo_a = vehiculos_ciclo_a + 1;
                        total_a = vehiculos_ciclo_a;
                        contado_a = true;
                    }
                    xSemaphoreGive(mutex_datos);
                }


                if (contado_a)
                {
                    ESP_LOGI(
                        TAG_BARRERA,
                        "Vehiculo A | Total: %d",
                        total_a);
                }


                ultima_a = ahora;
            }
        }


        // barrera b

        if ((actual_b == NIVEL_ACTIVO_BARRERA_B) && (anterior_b != NIVEL_ACTIVO_BARRERA_B))
        {
            if ((ultima_b == 0) || ((ahora - ultima_b) > pdMS_TO_TICKS(200)))
            {
                int total_b = 0;
                bool contado_b = false;

                if (xSemaphoreTake(
                        mutex_datos,
                        pdMS_TO_TICKS(20)) == pdTRUE)
                {
                    if (verde_b_activo)
                    {
                        vehiculos_ciclo_b = vehiculos_ciclo_b + 1;
                        total_b = vehiculos_ciclo_b;
                        contado_b = true;
                    }

                    xSemaphoreGive(mutex_datos);
                }


                if (contado_b)
                {
                    ESP_LOGI(
                        TAG_BARRERA,
                        "Vehiculo B | Total: %d",
                        total_b);
                }


                ultima_b = ahora;
            }
        }


        anterior_a = actual_a;
        anterior_b = actual_b;


        vTaskDelay(
            pdMS_TO_TICKS(10));
    }
}


// --------------------------------------------------
// tarea de control
// --------------------------------------------------

static void tarea_control(void *pvParameters)
{
    poner_semaforo_a(1, 0, 0);
    poner_semaforo_b(1, 0, 0);


    ESP_LOGI(
        TAG_CONTROL,
        "Sistema iniciado");


    vTaskDelay(
        pdMS_TO_TICKS(1000));


    while (1)
    {
        float distancia_actual_a = 80.0f;
        float distancia_actual_b = 80.0f;
        float flujo_actual_a = 0.0f;
        float flujo_actual_b = 0.0f;


        // leer datos de ambas calles

        if (xSemaphoreTake(
                mutex_datos,
                pdMS_TO_TICKS(20)) == pdTRUE)
        {
            distancia_actual_a = distancia_a;
            distancia_actual_b = distancia_b;
            flujo_actual_a = flujo_anterior_a;
            flujo_actual_b = flujo_anterior_b;
            xSemaphoreGive(mutex_datos);
        }


        // calcular prioridades de ambas calles

        int prioridad_a = calcular_prioridad(distancia_actual_a, flujo_actual_a);
        int prioridad_b = calcular_prioridad(distancia_actual_b, flujo_actual_b);


        // calcular ambos tiempos antes de iniciar el ciclo

        int tiempo_a = calcular_tiempo_verde(prioridad_a, prioridad_b);
        int tiempo_b = calcular_tiempo_verde(prioridad_b, prioridad_a);

        ESP_LOGI(
            TAG_CONTROL,
            "================================");

        ESP_LOGI(
            TAG_CONTROL,
            "A -> %.1f cm | %.2f veh/s | prioridad %d",
            distancia_actual_a,
            flujo_actual_a,
            prioridad_a);


        ESP_LOGI(
            TAG_CONTROL,
            "B -> %.1f cm | %.2f veh/s | prioridad %d",
            distancia_actual_b,
            flujo_actual_b,
            prioridad_b);


        ESP_LOGI(
            TAG_CONTROL,
            "Decision -> verde A: %d s | verde B: %d s",
            tiempo_a,
            tiempo_b);


        // ==================================================
        // verde calle a
        // ==================================================

        if (xSemaphoreTake(
                mutex_datos,
                pdMS_TO_TICKS(20)) == pdTRUE)
        {
            vehiculos_ciclo_a = 0;

            verde_a_activo = true;
            verde_b_activo = false;

            xSemaphoreGive(mutex_datos);
        }


        poner_semaforo_a(0, 0, 1);
        poner_semaforo_b(1, 0, 0);


        ESP_LOGI(
            TAG_CONTROL,
            "A VERDE durante %d s",
            tiempo_a);


        vTaskDelay(pdMS_TO_TICKS(tiempo_a * 1000));


        // terminar verde a

        int total_a = 0;
        float nuevo_flujo_a = 0.0f;


        if (xSemaphoreTake(
                mutex_datos,
                pdMS_TO_TICKS(20)) == pdTRUE)
        {
            verde_a_activo = false;
            total_a = vehiculos_ciclo_a;
            nuevo_flujo_a = (float)total_a / (float)tiempo_a;
            flujo_anterior_a = nuevo_flujo_a;

            xSemaphoreGive(mutex_datos);
        }


        ESP_LOGI(
            TAG_CONTROL,
            "Fin A -> %d vehiculos / %d s = %.2f veh/s",
            total_a,
            tiempo_a,
            nuevo_flujo_a);


        // amarillo a

        poner_semaforo_a(0, 1, 0);
        poner_semaforo_b(1, 0, 0);

        vTaskDelay(pdMS_TO_TICKS(TIEMPO_AMARILLO * 1000));


        // ambos rojos
        poner_semaforo_a(1, 0, 0);
        poner_semaforo_b(1, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(TIEMPO_TODO_ROJO * 1000));


        // ==================================================
        // verde calle b
        // ==================================================

        if (xSemaphoreTake(
                mutex_datos,
                pdMS_TO_TICKS(20)) == pdTRUE)
        {
            vehiculos_ciclo_b = 0;
            verde_a_activo = false;
            verde_b_activo = true;
            xSemaphoreGive(mutex_datos);
        }


        poner_semaforo_a(1, 0, 0);
        poner_semaforo_b(0, 0, 1);


        ESP_LOGI(
            TAG_CONTROL,
            "B VERDE durante %d s",
            tiempo_b);

        vTaskDelay(pdMS_TO_TICKS(tiempo_b * 1000));


        // terminar verde b

        int total_b = 0;
        float nuevo_flujo_b = 0.0f;


        if (xSemaphoreTake(
                mutex_datos,
                pdMS_TO_TICKS(20)) == pdTRUE)
        {
            verde_b_activo = false;
            total_b = vehiculos_ciclo_b;
            nuevo_flujo_b = (float)total_b / (float)tiempo_b;
            flujo_anterior_b = nuevo_flujo_b;
            xSemaphoreGive(mutex_datos);
        }


        ESP_LOGI(
            TAG_CONTROL,
            "Fin B -> %d vehiculos / %d s = %.2f veh/s",
            total_b,
            tiempo_b,
            nuevo_flujo_b);


        // amarillo b

        poner_semaforo_a(1, 0, 0);
        poner_semaforo_b(0, 1, 0);
        vTaskDelay(pdMS_TO_TICKS(TIEMPO_AMARILLO * 1000));


        // ambos rojos

        poner_semaforo_a(1, 0, 0);
        poner_semaforo_b(1, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(TIEMPO_TODO_ROJO * 1000));
    }
}


// --------------------------------------------------
// inicializar gpio
// --------------------------------------------------

static void inicializar_gpio(void)
{
    gpio_config_t salidas = {
        .pin_bit_mask =
            (1ULL << PIN_ROJO_A) |
            (1ULL << PIN_AMARILLO_A) |
            (1ULL << PIN_VERDE_A) |
            (1ULL << PIN_ROJO_B) |
            (1ULL << PIN_AMARILLO_B) |
            (1ULL << PIN_VERDE_B),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };


    ESP_ERROR_CHECK(gpio_config(&salidas));


    gpio_config_t barreras = {

        .pin_bit_mask =
            (1ULL << PIN_BARRERA_A) |
            (1ULL << PIN_BARRERA_B),

        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };


    ESP_ERROR_CHECK(
        gpio_config(&barreras));

    poner_semaforo_a(1, 0, 0);
    poner_semaforo_b(1, 0, 0);
}


// --------------------------------------------------
// inicializar adc
// --------------------------------------------------

static void inicializar_adc(void)
{
    adc_oneshot_unit_init_cfg_t configuracion_adc = {
        .unit_id = ADC_UNIT_1
    };


    ESP_ERROR_CHECK(
        adc_oneshot_new_unit(
            &configuracion_adc,
            &adc1_handle));


    adc_oneshot_chan_cfg_t configuracion_canal = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12
    };


    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(
            adc1_handle,
            CANAL_SHARP_A,
            &configuracion_canal));


    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(
            adc1_handle,
            CANAL_SHARP_B,
            &configuracion_canal));
}


// --------------------------------------------------
// programa principal
// --------------------------------------------------

void app_main(void)
{
    ESP_LOGI(
        "MAIN",
        "Iniciando sistema de semaforos inteligentes");

    mutex_datos = xSemaphoreCreateMutex();

    if (mutex_datos == NULL)
    {
        ESP_LOGE(
            "MAIN",
            "No se pudo crear el mutex");

        return;
    }


    inicializar_gpio();
    inicializar_adc();


    xTaskCreate(
        tarea_sensores,
        "tarea_sensores",
        4096,
        NULL,
        5,
        NULL);


    xTaskCreate(
        tarea_barreras,
        "tarea_barreras",
        4096,
        NULL,
        6,
        NULL);


    xTaskCreate(
        tarea_control,
        "tarea_control",
        4096,
        NULL,
        4,
        NULL);
}