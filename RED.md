# Capa de red: ESP32 → Raspberry Pi con mTLS

```
ESP32 ──WiFi──► MQTT sobre TLS (puerto 8883) ──► Mosquitto en la Raspberry Pi
        el ESP32 valida al broker  +  el broker valida al ESP32
```

- **El ESP32 valida al broker** con `ca.crt` y con el nombre `semaforo-broker` del certificado del servidor. Se conecta por IP, así que no necesita mDNS.
- **El broker valida al ESP32**: Mosquitto exige un certificado de cliente firmado por la misma CA (`require_certificate true`). Usa su CN como nombre de usuario para la ACL (`use_identity_as_username true`) y también como client id (`use_username_as_clientid true`), así que un certificado no puede hacerse pasar por otro cliente.
- Las llaves son EC P-256. El broker acepta TLS 1.2 o superior.

## Tópicos

| Tópico | Quién publica | Contenido |
|---|---|---|
| `semaforo/int1/telemetria` | ESP32 | Un JSON por calle al final de cada verde (QoS 1) |
| `semaforo/int1/estado` | ESP32 / broker | `online` al conectar. Si la conexión se pierde, el broker publica `offline` (Last Will). Ambos son retenidos |
| `semaforo/int1/cmd` | Capa de aplicación | Reservado para comandos (el firmware todavía no lo usa) |

## Mensaje de telemetría

```json
{"device_id":"esp32-int1","boot_id":"9f3a01c2","ciclo":12,"calle":"A","ts":1763640000123,
 "distancia_cm":23.4,"prioridad":4,"t_verde_s":12,"vehiculos":5,"flujo_veh_s":0.42}
```

| Campo | Significado |
|---|---|
| `boot_id` | Valor aleatorio que cambia en cada arranque del ESP32 |
| `ciclo` | Contador de ciclos; vuelve a 1 en cada arranque |
| `ts` | Epoch en ms (UTC) del fin del verde. Vale `null` si SNTP aún no sincroniza; en ese caso, use la hora de llegada |
| `distancia_cm`, `prioridad`, `t_verde_s` | Entrada y salida de la decisión tomada al inicio del ciclo |
| `vehiculos`, `flujo_veh_s` | Lo medido por la barrera IR durante ese verde |

**Entrega "al menos una vez":** un registro puede llegar repetido, pero no se pierde mientras esté en el ESP32 (ver más abajo). El consumidor debe descartar duplicados usando como clave `(device_id, boot_id, ciclo, calle)`.

## Comportamiento del firmware (`src/red.c`)

- `tarea_control` encola el registro sin bloquear y `tarea_red` lo publica. El semáforo nunca espera a la red.
- Un registro solo se da por entregado cuando llega el **PUBACK** del broker. Si no llega en 15 s (por ejemplo, porque la Pi se apagó y la caída todavía no se detecta), se reenvía. Por eso puede haber duplicados.
- Mientras no hay confirmación, los registros esperan en una cola de 16 (8 ciclos). Si la cola se llena, se descarta el más antiguo.
- Prioridades: la tarea interna de esp-mqtt (TLS, reenvíos) y `tarea_red` corren en prioridad 3, por debajo de `tarea_control` (4), `tarea_sensores` (5) y `tarea_barreras` (6).
- El cliente MQTT reconecta solo. Keepalive de 30 s: el broker publica `offline` unos 45 s después de perder al ESP32.
- La red arranca después de las tareas del semáforo. Si el WiFi o el broker no están disponibles, el semáforo sigue en modo local. Una falta de memoria al iniciar el WiFi sí reinicia el ESP32, porque ESP-IDF aborta en ese caso.

## Puesta en marcha

### 1. PKI (una sola vez, en un solo equipo)

```bash
pki/generar_certs.sh <IP_DE_LA_PI>          # ej.: pki/generar_certs.sh 192.168.1.50
```

Crea `certs/` (está en `.gitignore`). Comparta con el equipo solo los archivos que necesita cada uno, por un medio privado:

| Destino | Archivos |
|---|---|
| Firmware ESP32 | `ca.crt`, `esp32.crt`, `esp32.key` |
| Raspberry Pi (Mosquitto) | `ca.crt`, `server.crt`, `server.key` |
| Consumidores | `ca.crt` + `monitor.*` (pruebas manuales) o `app.*` (capa de aplicación) |
| Nadie | `ca.key`: guárdela fuera del repositorio |

- Cada consumidor necesita su propio certificado: dos clientes con el mismo CN se expulsan entre sí. Para agregar otro, añádalo a `CONSUMIDORES` en el script y cree su bloque en `raspberry/mosquitto/acl`.
- Emitir certificados nuevos requiere `ca.key` en `certs/`. El script lo verifica.
- **Si cambia la IP de la Pi**, basta con actualizar `URI_BROKER` y volver a grabar el ESP32, porque el ESP32 valida el nombre `semaforo-broker`, no la IP. Solo los clientes que usan `-h <IP>` necesitan un `server.crt` nuevo (borre `server.*`, corra el script y reinstale el broker).
- En Windows, use Git Bash. Los scripts ya traen `MSYS_NO_PATHCONV=1` y `.gitattributes` los fuerza a LF.

### 2. Raspberry Pi

```bash
# en la Pi, dentro del repo y con certs/ copiado
sudo raspberry/instalar_broker.sh
```

Antes de instalar, el script verifica que `server.crt` sea de la CA, que esté vigente según el reloj de la Pi y que corresponda a `server.key`.

### 3. Firmware

```bash
cp src/config_red.example.h src/config_red.h   # editar WiFi, IP del broker, etc.
pio run -t upload -t monitor
```

- La plataforma está fijada en `espressif32 @ 8.0.0` (ESP-IDF 6.1). La primera compilación descarga `espressif/mqtt` desde el ESP Component Registry, porque desde ESP-IDF 6.0 el cliente MQTT no viene dentro de IDF.
- `sdkconfig.defaults` configura 4 MB de flash y una partición de aplicación de 1.5 MB.
- `ID_DISPOSITIVO` debe coincidir con el CN de `esp32.crt` (por defecto `esp32-int1`).

### 4. Verificación

En la Pi:

```bash
mosquitto_sub -h localhost -p 8883 --cafile certs/ca.crt \
  --cert certs/monitor.crt --key certs/monitor.key -t 'semaforo/#' -v
```

Para simular el ESP32 sin hardware, **solo con el ESP32 apagado**. Si está encendido, ambos tienen el mismo client id y el broker expulsaría al ESP32:

```bash
mosquitto_pub -h localhost -p 8883 --cafile certs/ca.crt \
  --cert certs/esp32.crt --key certs/esp32.key -q 1 \
  -t semaforo/int1/telemetria -m '{"prueba":1}'
```

Log del broker: `sudo tail -f /var/log/mosquitto/mosquitto.log`. En Debian, Mosquitto escribe ahí y no en journald.

## Problemas comunes

| Síntoma | Causa probable |
|---|---|
| El ESP32 muestra `Error de transporte/TLS` | Falló el handshake TLS. Revise que `ca.crt` sea de la misma CA, que `NOMBRE_SERVIDOR_TLS` sea `semaforo-broker` y que el broker escuche en `URI_BROKER` |
| El broker registra `peer did not return a certificate` | El cliente no envió certificado |
| El broker registra `certificate verify failed` | El certificado del cliente es de otra CA, **o el reloj de la Pi está atrasado** y el certificado todavía no es válido. Pasa con una Pi sin RTC ni internet. Revise `date` y configure NTP, o genere la PKI en la propia Pi |
| Conecta, pero los mensajes no llegan | El CN del certificado no coincide con el usuario de la ACL. Mosquitto descarta en silencio las publicaciones no autorizadas |
| El ESP32 se desconecta y reconecta cada pocos segundos | Otro cliente usa el mismo certificado (mismo CN, mismo client id) |
| No hay WiFi | El ESP32 solo usa 2.4 GHz con WPA2/WPA3 Personal (clave). Redes WPA2-Enterprise (usuario y contraseña, como la red de la universidad) no están soportadas: use un router o hotspot propio |

## Seguridad

- No suba `certs/` ni `src/config_red.h`.
- Con `ca.key`, cualquiera puede emitir certificados válidos para el broker.
- `esp32.key` queda en la flash sin cifrar. En producción habría que activar flash encryption y secure boot (fuera del alcance del curso).
- **Revocar un cliente:** quite su bloque de `/etc/mosquitto/acl` y ejecute `sudo systemctl reload mosquitto`. El certificado podrá seguir conectándose, pero sin publicar ni suscribirse. Para invalidarlo del todo, genere una CA nueva y vuelva a emitir todos los certificados (el script no lleva la base de datos que necesitaría una CRL).
- El ESP32 no revisa las fechas de vencimiento de los certificados (`MBEDTLS_HAVE_TIME_DATE` está desactivado). El broker sí.
