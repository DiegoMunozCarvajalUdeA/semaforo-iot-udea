#!/usr/bin/env bash
# Instala Mosquitto con mTLS en la Raspberry Pi (Raspberry Pi OS / Debian).
# Uso, en la Pi y desde la raiz del repositorio, con certs/ ya copiado:
#   sudo raspberry/instalar_broker.sh
set -euo pipefail

RAIZ="$(cd "$(dirname "$0")/.." && pwd)"

if [[ $EUID -ne 0 ]]; then
    echo "Ejecute con sudo" >&2
    exit 1
fi

error() {
    echo "Error: $*" >&2
    exit 1
}

for archivo in ca.crt server.crt server.key; do
    [[ -s "$RAIZ/certs/$archivo" ]] || error "falta certs/$archivo o esta vacio (genere la PKI con pki/generar_certs.sh)"
done

# el certificado debe ser de la CA, estar vigente segun el reloj de la Pi
# ("not yet valid" = reloj atrasado) y corresponder a la llave
openssl verify -CAfile "$RAIZ/certs/ca.crt" "$RAIZ/certs/server.crt" ||
    error "certs/server.crt no es valido para certs/ca.crt (revise tambien la hora: date)"

if [[ "$(openssl x509 -in "$RAIZ/certs/server.crt" -noout -pubkey)" != \
      "$(openssl pkey -in "$RAIZ/certs/server.key" -pubout)" ]]; then
    error "certs/server.key no corresponde a certs/server.crt"
fi

# mosquitto no acepta finales de linea CRLF (archivos editados en Windows)
for archivo in raspberry/mosquitto/mtls.conf raspberry/mosquitto/acl; do
    if grep -q $'\r' "$RAIZ/$archivo"; then
        error "$archivo tiene finales de linea CRLF; conviertalo con: sed -i 's/\\r$//' $archivo"
    fi
done

apt-get update
apt-get install -y mosquitto mosquitto-clients

install -d -m 755 /etc/mosquitto/certs
install -m 644 "$RAIZ/certs/ca.crt" "$RAIZ/certs/server.crt" /etc/mosquitto/certs/
install -m 600 -o mosquitto -g mosquitto "$RAIZ/certs/server.key" /etc/mosquitto/certs/
install -m 600 -o mosquitto -g mosquitto "$RAIZ/raspberry/mosquitto/acl" /etc/mosquitto/acl
install -m 644 "$RAIZ/raspberry/mosquitto/mtls.conf" /etc/mosquitto/conf.d/mtls.conf

systemctl enable mosquitto
systemctl restart mosquitto
systemctl --no-pager --lines=5 status mosquitto

echo
echo "Log del broker: sudo tail -f /var/log/mosquitto/mosquitto.log"
