# Capa de red: ESP32 → Raspberry Pi con mTLS

```
ESP32 ──WiFi──► MQTT sobre TLS (puerto 8883) ──► Mosquitto en la Raspberry Pi
        el ESP32 valida al broker  +  el broker valida al ESP32
```

- **El ESP32 valida al broker** con `ca.crt` y con el nombre `semaforo-broker` del certificado del servidor. Se conecta por IP, así que no necesita mDNS.
- **El broker valida al ESP32**: Mosquitto exige un certificado de cliente firmado por la misma CA (`require_certificate true`) y usa su CN como nombre de usuario para la ACL (`use_identity_as_username true`).
- Las llaves son EC P-256, y el broker acepta TLS 1.2 o superior.

## Tópicos

| Tópico | Quién publica | Contenido |
|---|---|---|
| `semaforo/int1/telemetria` | ESP32 | Un JSON por calle al final de cada verde (QoS 1) |
| `semaforo/int1/estado` | ESP32 / broker | `online` al conectar. Si la conexión se pierde, el broker publica `offline` (Last Will). Ambos son retenidos |
| `semaforo/int1/cmd` | Capa de aplicación | Reservado para comandos (el firmware todavía no lo usa) |

## Mensaje de telemetría

```json
{"device_id":"esp32-int1","ciclo":12,"calle":"A","ts":1763640000123,
 "distancia_cm":23.4,"prioridad":4,"t_verde_s":12,"vehiculos":5,"flujo_veh_s":0.42}
```

| Campo | Significado |
|---|---|
| `ciclo` | Contador de ciclos desde el arranque |
| `ts` | Epoch en ms (UTC) del fin del verde. Vale `null` si SNTP aún no sincroniza; en ese caso, use la hora de llegada |
| `distancia_cm`, `prioridad`, `t_verde_s` | Entrada y salida de la decisión tomada al inicio del ciclo |
| `vehiculos`, `flujo_veh_s` | Lo medido por la barrera IR durante ese verde |

## Comportamiento del firmware (`src/red.c`)

- `tarea_control` encola el registro sin bloquear y `tarea_red` (prioridad 3, la más baja) lo publica. El semáforo nunca espera a la red.
- La cola guarda 16 registros (8 ciclos). Mientras no hay conexión, los registros esperan ahí. Si la cola se llena, se descarta el más antiguo.
- El cliente MQTT reconecta solo. Keepalive de 30 s: el broker publica `offline` unos 45 s después de perder al ESP32.
- Si la red no inicia, el semáforo sigue funcionando en modo local.

## Puesta en marcha

### 1. PKI (una sola vez, en un solo equipo)

```bash
pki/generar_certs.sh <IP_DE_LA_PI>          # ej.: pki/generar_certs.sh 192.168.1.50
```

Crea `certs/` (está en `.gitignore`). Comparta con el equipo solo los archivos que necesita cada uno, por un medio privado:

| Destino | Archivos |
|---|---|
| Firmware ESP32 | `ca.crt`, `esp32.crt`, `esp32.key` |
| Raspberry Pi | `ca.crt`, `server.crt`, `server.key` (y `monitor.*` para consumidores) |
| Nadie | `ca.key`: guárdela fuera del repositorio |

La Pi debe tener una IP fija (por ejemplo, con una reserva DHCP en el router). Si la IP cambia, borre `certs/server.*` y vuelva a correr el script.

### 2. Raspberry Pi

```bash
# en la Pi, dentro del repo y con certs/ copiado
sudo raspberry/instalar_broker.sh
```

### 3. Firmware

```bash
cp src/config_red.example.h src/config_red.h   # editar WiFi, IP del broker, etc.
pio run -t upload -t monitor
```

La primera compilación descarga `espressif/mqtt` desde el ESP Component Registry, porque desde ESP-IDF 6.0 el cliente MQTT no viene dentro de IDF. `ID_DISPOSITIVO` debe coincidir con el CN de `esp32.crt` (por defecto `esp32-int1`).

### 4. Verificación

En la Pi:

```bash
mosquitto_sub -h localhost -p 8883 --cafile certs/ca.crt \
  --cert certs/monitor.crt --key certs/monitor.key -t 'semaforo/#' -v
```

Para simular el ESP32 sin hardware:

```bash
mosquitto_pub -h localhost -p 8883 --cafile certs/ca.crt \
  --cert certs/esp32.crt --key certs/esp32.key -i esp32-int1 -q 1 \
  -t semaforo/int1/telemetria -m '{"prueba":1}'
```

Ver el log del broker: `sudo journalctl -u mosquitto -f`.

## Problemas comunes

| Síntoma | Causa probable |
|---|---|
| El ESP32 muestra `Error de transporte/TLS` | Falló el handshake TLS. Revise que `ca.crt` sea de la misma CA, que `NOMBRE_SERVIDOR_TLS` sea `semaforo-broker` y que el broker escuche en `URI_BROKER` |
| El broker registra `peer did not return a certificate` | El cliente no envió certificado |
| El broker registra `certificate verify failed` | El certificado del cliente es de otra CA |
| Conecta, pero los mensajes no llegan | El CN del certificado no coincide con el usuario de la ACL. Mosquitto descarta en silencio las publicaciones no autorizadas |
| No hay WiFi | El ESP32 solo usa 2.4 GHz y WPA2 o superior |

## Seguridad

- No suba `certs/` ni `src/config_red.h`.
- Con `ca.key`, cualquiera puede emitir certificados válidos para el broker.
- `esp32.key` queda en la flash sin cifrar. En producción habría que activar flash encryption y secure boot (fuera del alcance del curso).
- Para revocar un dispositivo, use `crlfile` en Mosquitto o regenere la CA.
