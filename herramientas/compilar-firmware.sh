#!/usr/bin/env bash
# Compila el firmware X-RTbox para cada chip y deja la imagen completa
# (bootloader + particiones + programa, desde 0x0) en los assets de la app.
set -u
URL="https://espressif.github.io/arduino-esp32/package_esp32_index.json"
ASSETS="android/app/src/main/assets/firmware"
OUT="firmware-out"
mkdir -p "$ASSETS" "$OUT"

arduino-cli core update-index --additional-urls "$URL"
# Ultima version 3.x del core (la 4.0 cambio la API de Bluetooth)
VER=$(arduino-cli core search esp32 --all --additional-urls "$URL" | grep 'esp32:esp32' | grep -oE '\b3\.[0-9]+\.[0-9]+\b' | sort -V | tail -1)
if [ -z "$VER" ]; then VER="3.3.0"; fi
echo "Usando el core esp32 $VER"
echo "core=$VER" >> "$GITHUB_OUTPUT"
arduino-cli core install "esp32:esp32@$VER" --additional-urls "$URL" || exit 1
python3 -m pip install --quiet --break-system-packages esptool 2>/dev/null || python3 -m pip install --quiet --user esptool || true

# Version del firmware = huella del codigo fuente (la app la compara con la del ESP32)
SRC=esp32/XRTboxESP32
rm -f "$SRC/xrtbox_version.h"
FWV=$(cat "$SRC"/*.ino "$SRC"/*.cpp "$SRC"/*.h | sha1sum | cut -c1-8)
echo "#define XRTBOX_FW_VERSION \"$FWV\"" > "$SRC/xrtbox_version.h"
printf '%s' "$FWV" > "$ASSETS/version.txt"
echo "Version del firmware: $FWV"

build() {  # $1 nombre, $2 fqbn, $3 direccion del bootloader
  local name="$1" fqbn="$2" boot="$3" dir="build/$1"
  echo "::group::Firmware $name"
  mkdir -p "$dir"
  local log="$dir/compilacion.log"
  if arduino-cli compile --fqbn "$fqbn" --output-dir "$dir" --additional-urls "$URL" --warnings default esp32/XRTboxESP32 > "$log" 2>&1; then
    tail -n 15 "$log"
    local merged
    merged=$(ls "$dir"/*.merged.bin 2>/dev/null | head -1)
    if [ -z "$merged" ]; then
      # cores antiguos: unir las piezas a mano
      local app0
      app0=$(ls "$HOME"/.arduino15/packages/esp32/hardware/esp32/*/tools/partitions/boot_app0.bin 2>/dev/null | head -1)
      merged="$dir/manual.merged.bin"
      python3 -m esptool --chip "$name" merge_bin -o "$merged" \
        "$boot" "$dir"/XRTboxESP32.ino.bootloader.bin 0x8000 "$dir"/XRTboxESP32.ino.partitions.bin \
        0xe000 "$app0" 0x10000 "$dir"/XRTboxESP32.ino.bin || merged=""
    fi
    if [ -n "$merged" ] && [ -f "$merged" ]; then
      cp "$merged" "$ASSETS/$name.bin"
      cp "$merged" "$OUT/X-RTbox-firmware-$name.bin"
      echo "Firmware $name listo: $(stat -c %s "$merged") bytes"
    else
      echo "::warning::No se pudo generar la imagen completa del firmware $name"
    fi
  else
    cat "$log"
    # los errores del compilador salen como anotaciones (se leen sin entrar al registro)
    grep -E "error:|Error:|error de|undefined reference" "$log" | head -n 9 | while IFS= read -r l; do
      l="${l//'%'/'%25'}"
      l="${l#"$PWD/"}"
      echo "::error title=Firmware $name::$l"
    done
    echo "::warning::El firmware no compila para $name"
  fi
  echo "::endgroup::"
}

build esp32   "esp32:esp32:esp32:PartitionScheme=huge_app"                 0x1000
build esp32s3 "esp32:esp32:esp32s3:PartitionScheme=huge_app,CDCOnBoot=cdc" 0x0
build esp32c3 "esp32:esp32:esp32c3:PartitionScheme=huge_app,CDCOnBoot=cdc" 0x0
build esp32c6 "esp32:esp32:esp32c6:PartitionScheme=huge_app,CDCOnBoot=cdc" 0x0

ls -la "$ASSETS"
if [ ! -f "$ASSETS/esp32.bin" ]; then
  echo "::error::No se pudo compilar el firmware para el ESP32 clasico (revisa el registro de arriba)"
fi
exit 0
