// Copie este archivo a src/config_red.h y ajuste los valores.
// src/config_red.h esta en .gitignore: no suba credenciales al repositorio.

#pragma once


// red wifi (el esp32 solo usa 2.4 GHz)

#define WIFI_SSID "mi-red"
#define WIFI_PASSWORD "mi-clave"


// broker mosquitto en la raspberry pi (puerto tls 8883)

#define URI_BROKER "mqtts://192.168.1.50:8883"


// nombre que se valida en el certificado del servidor (SAN DNS).
// permite conectarse por ip sin depender de mDNS.
// debe coincidir con NOMBRE_SERVIDOR de pki/generar_certs.sh

#define NOMBRE_SERVIDOR_TLS "semaforo-broker"


// debe coincidir con el CN de certs/esp32.crt:
// mosquitto usa ese CN como usuario para aplicar la ACL

#define ID_DISPOSITIVO "esp32-int1"
#define TOPICO_BASE "semaforo/int1"


// servidor ntp. si la red no tiene internet, use la ip de la
// raspberry pi (con chrony configurado como servidor ntp)

#define SERVIDOR_NTP "pool.ntp.org"
