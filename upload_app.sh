#!/usr/bin/env bash
# Uygulamayı (firmware/) derle ve UART üzerinden bootloader'a yolla.
# Ön koşul: bootloader'ı ./build_and_flash.sh ile en az bir kez FPGA'ya yazmış olmalısın.
# Picocom (varsa) önce kapat, sonra yeniden aç.

set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
PORT="${1:-/dev/ttyUSB1}"

echo "==> [1/2] firmware derleniyor"
make -C firmware clean
make -C firmware

echo "==> [2/2] $PORT üzerinden upload"
echo "    (Hata alırsan reset (S1) basıp tekrar dene.)"
python3 tools/upload.py firmware/firmware.bin "$PORT"

echo
echo "==> Tamam. Çıktıyı görmek için:"
echo "      picocom -b 115200 $PORT"
