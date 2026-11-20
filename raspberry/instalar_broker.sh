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

for archivo in ca.crt server.crt server.key; do
    if [[ ! -f "$RAIZ/certs/$archivo" ]]; then
        echo "Falta certs/$archivo (genere la PKI con pki/generar_certs.sh)" >&2
        exit 1
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
