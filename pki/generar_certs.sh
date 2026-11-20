#!/usr/bin/env bash
# Genera la PKI para mTLS entre el ESP32 y la Raspberry Pi:
#   ca.crt / ca.key           CA propia (ca.key nunca sale del equipo que la genera)
#   server.crt / server.key   broker Mosquitto en la Raspberry Pi
#   esp32.crt / esp32.key     cliente ESP32 (se embeben en el firmware)
#   monitor.crt / monitor.key cliente para pruebas y consumidores en la Pi
#
# Uso: pki/generar_certs.sh <IP_RASPBERRY> [ID_ESP32] [DIAS]
#   pki/generar_certs.sh 192.168.1.50 esp32-int1
#
# Solo genera lo que falta: para regenerar un certificado, borre sus archivos.
# La salida va a certs/ (en .gitignore) o a $CERTS_DIR si esta definido.
set -euo pipefail

IP_PI="${1:?Uso: $0 <IP_RASPBERRY> [ID_ESP32] [DIAS]}"
ID_ESP32="${2:-esp32-int1}"
DIAS="${3:-825}"
NOMBRE_SERVIDOR="semaforo-broker"   # debe coincidir con NOMBRE_SERVIDOR_TLS del firmware

RAIZ="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${CERTS_DIR:-$RAIZ/certs}"

mkdir -p "$DIR"
cd "$DIR"
umask 077

CNF="$(mktemp)"
trap 'rm -f "$CNF"' EXIT

cat > "$CNF" <<CNF
[req]
distinguished_name = dn
prompt = no

[dn]
CN = sin-nombre

[v3_ca]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash

[v3_servidor]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:$NOMBRE_SERVIDOR, DNS:raspberrypi.local, DNS:localhost, IP:$IP_PI, IP:127.0.0.1
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid

[v3_cliente]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = clientAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
CNF

# llave EC P-256 en formato PKCS#8 (la aceptan mbedTLS y OpenSSL)
generar_llave() {
    openssl ecparam -name prime256v1 -genkey -noout | openssl pkcs8 -topk8 -nocrypt -out "$1"
}

# firma un certificado con la CA: <nombre> <CN> <seccion de extensiones>
emitir() {
    local nombre="$1" cn="$2" extensiones="$3"

    if [[ -f "$nombre.crt" ]]; then
        echo "= $nombre.crt ya existe (borre $nombre.crt y $nombre.key para regenerarlo)"
        return
    fi

    generar_llave "$nombre.key"
    openssl req -new -key "$nombre.key" -subj "/CN=$cn" -config "$CNF" -out "$nombre.csr"
    openssl x509 -req -in "$nombre.csr" -CA ca.crt -CAkey ca.key -CAcreateserial \
        -days "$DIAS" -sha256 -extfile "$CNF" -extensions "$extensiones" -out "$nombre.crt" 2>/dev/null
    rm -f "$nombre.csr"
    chmod 644 "$nombre.crt"
    echo "+ $nombre.crt (CN=$cn)"
}

if [[ -f ca.crt ]]; then
    echo "= ca.crt ya existe, se reutiliza la CA"
else
    generar_llave ca.key
    openssl req -x509 -new -key ca.key -sha256 -days 3650 -subj "/CN=Semaforo IoT CA" \
        -config "$CNF" -extensions v3_ca -out ca.crt
    chmod 644 ca.crt
    echo "+ ca.crt"
fi

emitir server "$NOMBRE_SERVIDOR" v3_servidor
emitir esp32 "$ID_ESP32" v3_cliente
emitir monitor monitor v3_cliente

openssl verify -CAfile ca.crt server.crt esp32.crt monitor.crt

cat <<RESUMEN

Certificados en $DIR
  ESP32 (se embeben al compilar): ca.crt esp32.crt esp32.key
  Raspberry Pi (Mosquitto):       ca.crt server.crt server.key
  Pruebas/consumidores:           ca.crt monitor.crt monitor.key
  ca.key: guardela fuera del repositorio y no la copie a ningun dispositivo.
RESUMEN
