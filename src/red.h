#pragma once

#include <stdbool.h>
#include <stdint.h>


// --------------------------------------------------
// registro que se publica al final del verde de cada calle
//
// distancia_cm, prioridad y tiempo_verde_s son la entrada
// y la salida de la decision tomada al inicio del ciclo.
// vehiculos y flujo se midieron durante ese verde.
// --------------------------------------------------

typedef struct
{
    uint32_t ciclo;
    char calle;
    float distancia_cm;
    int prioridad;
    int tiempo_verde_s;
    int vehiculos;
    float flujo;

    // lo llena red_publicar_calle (epoch en ms, 0 si no hay hora)
    int64_t ts_ms;
} registro_calle_t;


// inicia wifi, sntp, cliente mqtt con mtls y la tarea de red.
// si falla, el semaforo sigue funcionando en modo local.
bool red_iniciar(void);

// encola el registro sin bloquear; si la cola esta llena
// descarta el registro mas antiguo.
void red_publicar_calle(const registro_calle_t *registro);
