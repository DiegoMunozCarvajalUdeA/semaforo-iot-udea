#!/usr/bin/env bash
# Genera la PKI para mTLS entre el ESP32 y la Raspberry Pi:
#   ca.crt / ca.key           CA propia (ca.key nunca sale del equipo que la genera)
#   server.crt / server.key   broker Mosquitto en la Raspberry Pi
#   esp32.crt / esp32.key     cliente ESP32 (se embeben en el firmware)
#   monitor.crt / monitor.key cliente para pruebas manuales (mosquitto_sub)
#   app.crt / app.key         cliente para el consumidor de la capa de aplicacion
#
# Uso: pki/generar_certs.sh <IP_RASPBERRY> [ID_ESP32] [DIAS]
#   pki/generar_certs.sh 192.168.1.50 esp32-int1
#
# Solo genera lo que falta: para regenerar un certificado, borre su .crt y su .key.
# Emitir certificados nuevos requiere ca.key en el directorio de salida.
# La salida va a certs/ (en .gitignore) o a $CERTS_DIR si esta definido.
set -euo pipefail

# Git Bash (Windows) convierte "/CN=..." en una ruta si no se desactiva
export MSYS_NO_PATHCONV=1

USO="Uso: $0 <IP_RASPBERRY> [ID_ESP32] [DIAS]"
IP_PI="${1:?$USO}"
ID_ESP32="${2:-esp32-int1}"
DIAS="${3:-825}"
NOMBRE_SERVIDOR="semaforo-broker"   # debe coincidir con NOMBRE_SERVIDOR_TLS del firmware
CONSUMIDORES="monitor app"          # un certificado por consumidor (el CN es su client id)

error() {
    echo "Error: $*" >&2
    exit 1
}

# validar argumentos antes de tocar nada

if [[ ! "$IP_PI" =~ ^([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})$ ]]; then
    error "IP invalida: '$IP_PI' (se espera IPv4, ej. 192.168.1.50)"
fi

for octeto in "${BASH_REMATCH[@]:1}"; do
    (( 10#$octeto <= 255 )) || error "IP invalida: '$IP_PI'"
done

[[ "$ID_ESP32" =~ ^[A-Za-z0-9._-]+$ ]] || error "ID_ESP32 invalido: '$ID_ESP32' (solo letras, numeros, . _ -)"
[[ "$DIAS" =~ ^[0-9]+$ ]] || error "DIAS invalido: '$DIAS'"

RAIZ="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${CERTS_DIR:-$RAIZ/certs}"

mkdir -p "$DIR"
cd "$DIR"
umask 077

CNF="$(mktemp)"
trap 'rm -f "$CNF" ./*.tmp ./*.csr' EXIT

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

# ejecuta openssl y solo muestra su salida si falla
openssl_silencioso() {
    local salida
    if ! salida="$(openssl "$@" 2>&1)"; then
        echo "$salida" >&2
        error "fallo: openssl $1"
    fi
}

# llave EC P-256 en formato PKCS#8 (la aceptan mbedTLS y OpenSSL)
generar_llave() {
    openssl ecparam -name prime256v1 -genkey -noout | openssl pkcs8 -topk8 -nocrypt -out "$1"
    [[ -s "$1" ]] || error "no se pudo generar la llave $1"
}

# firma un certificado con la CA: <nombre> <CN> <seccion de extensiones>.
# escribe en archivos .tmp y solo los renombra si todo salio bien
emitir() {
    local nombre="$1" cn="$2" extensiones="$3"

    if [[ -s "$nombre.crt" && -s "$nombre.key" ]]; then
        echo "= $nombre.crt ya existe (borre $nombre.crt y $nombre.key para regenerarlo)"
        return
    fi

    [[ -s ca.key ]] || error "falta ca.key en $DIR: se necesita para emitir $nombre.crt"

    generar_llave "$nombre.key.tmp"
    openssl_silencioso req -new -key "$nombre.key.tmp" -subj "/CN=$cn" -config "$CNF" -out "$nombre.csr"
    openssl_silencioso x509 -req -in "$nombre.csr" -CA ca.crt -CAkey ca.key -CAcreateserial \
        -days "$DIAS" -sha256 -extfile "$CNF" -extensions "$extensiones" -out "$nombre.crt.tmp"
    [[ -s "$nombre.crt.tmp" ]] || error "no se pudo emitir $nombre.crt"

    chmod 644 "$nombre.crt.tmp"
    mv "$nombre.key.tmp" "$nombre.key"
    mv "$nombre.crt.tmp" "$nombre.crt"
    rm -f "$nombre.csr"
    echo "+ $nombre.crt (CN=$cn)"
}

# ca

if [[ -s ca.crt ]]; then
    echo "= ca.crt ya existe, se reutiliza la CA"
elif [[ -e ca.key ]]; then
    error "existe ca.key pero no ca.crt en $DIR; revise el directorio antes de continuar"
else
    generar_llave ca.key.tmp
    openssl_silencioso req -x509 -new -key ca.key.tmp -sha256 -days 3650 -subj "/CN=Semaforo IoT CA" \
        -config "$CNF" -extensions v3_ca -out ca.crt.tmp
    chmod 644 ca.crt.tmp
    mv ca.key.tmp ca.key
    mv ca.crt.tmp ca.crt
    echo "+ ca.crt"
fi

# certificados

emitir server "$NOMBRE_SERVIDOR" v3_servidor
emitir esp32 "$ID_ESP32" v3_cliente

for consumidor in $CONSUMIDORES; do
    emitir "$consumidor" "$consumidor" v3_cliente
done

# el esp32 valida el nombre semaforo-broker, no la ip; la ip solo importa
# a clientes que se conectan con -h <IP>

if ! openssl x509 -in server.crt -noout -text | grep -Eq "IP Address:${IP_PI//./\\.}(,|$)"; then
    echo "Aviso: server.crt no incluye la IP $IP_PI. Al ESP32 no le afecta (valida el nombre"
    echo "       $NOMBRE_SERVIDOR); los clientes que usen -h $IP_PI fallaran. Para incluirla,"
    echo "       borre server.crt y server.key, vuelva a correr el script y reinstale el broker."
fi

# shellcheck disable=SC2086
openssl verify -CAfile ca.crt server.crt esp32.crt $(printf '%s.crt ' $CONSUMIDORES)

cat <<RESUMEN

Certificados en $DIR
  ESP32 (se embeben al compilar): ca.crt esp32.crt esp32.key
  Raspberry Pi (Mosquitto):       ca.crt server.crt server.key
  Consumidores:                   ca.crt + monitor.* (pruebas) / app.* (capa de aplicacion)
  ca.key: guardela fuera del repositorio y no la copie a ningun dispositivo.
RESUMEN
