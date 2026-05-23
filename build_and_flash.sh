#!/usr/bin/env bash
# Bootloader'ı bitstream'e göm + Tang Nano 9K flash.
# Bunu sadece bootloader değiştiyse veya ilk kurulumda çalıştır.
# Uygulamayı (firmware/) güncellemek için: ./upload_app.sh

set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

echo "==> [1/4] bootloader derleniyor"
make -C bootloader clean
make -C bootloader

echo "==> [2/4] bootloader hex'i synth yoluna kopyalanıyor"
cp bootloader/bootldr.hex picorv32/firmware.hex

echo "==> [3/4] bitstream sentez + PnR + pack"
source /home/tugmirk/oss-cad-suite/environment
make -f Makefile.oss clean
make -f Makefile.oss

echo "==> [4/4] flash"
make -f Makefile.oss flash

echo "==> Tamam. Reset (S1) sonra picocom ile bağlan:"
echo "      picocom -b 115200 /dev/ttyUSB1"
echo "    'BOOT>' promptu görmen lazım. Sonra: ./upload_app.sh ile uygulama yolla."
