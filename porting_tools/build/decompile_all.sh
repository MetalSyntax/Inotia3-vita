#!/bin/bash
# Script para decompilar el APK y los .so de Inotia 3.
# Ver PORTING_PLAN.md Fase 1 para el detalle de esta guía.

BASE_DIR="/Volumes/Seagate/PSVITA Develop/Inotia3-vita"
APK_FILE="${BASE_DIR}/inotia3-1-3-2.apk"
LIB_DIR="${BASE_DIR}/inotia3-1-3-2/lib"
APK_OUT_DIR="${BASE_DIR}/decompiled_apk"
SO_OUT_BASE="${BASE_DIR}/decompiled_so"

mkdir -p "$APK_OUT_DIR"

echo "Iniciando decompilación con JADX..."
if command -v jadx >/dev/null 2>&1; then
  jadx -d "$APK_OUT_DIR" "$APK_FILE"
else
  docker run --rm -v "${BASE_DIR}:/app" ubuntu:latest bash -c "
    cd /tmp
    apt-get update && apt-get install -y wget unzip default-jre
    wget -qO- https://github.com/skylot/jadx/releases/download/v1.4.7/jadx-1.4.7.zip > jadx.zip
    unzip -q jadx.zip -d jadx
    ./jadx/bin/jadx -d /app/decompiled_apk /app/inotia3-1-3-2.apk
  "
fi
echo "JADX Finalizado. Resultados en ${APK_OUT_DIR}"

echo "Iniciando decompilación de .so con Ghidra y Angr (so-decompiler)..."
if [ ! -d "$LIB_DIR" ]; then
    echo "Directorio de librerías no encontrado en: $LIB_DIR"
else
    # so_out incluye el ABI (basename del directorio padre, ej. armeabi-v7a) porque
    # un .so con el mismo nombre puede existir en más de un ABI -- sin el ABI en el
    # nombre de carpeta, la segunda decompilación pisaría los resultados de la primera.
    find "$LIB_DIR" -type f -name "*.so" | while read -r so_file; do
        so_name=$(basename "$so_file")
        abi=$(basename "$(dirname "$so_file")")
        so_out="${SO_OUT_BASE}/${so_name%.so}_${abi}"
        mkdir -p "$so_out"

        # Calcular la ruta relativa para el contenedor
        rel_so_path="${so_file#$BASE_DIR/}"
        rel_so_out="${so_out#$BASE_DIR/}"

        echo "Decompilando $so_name ($abi)..."
        # El segundo argumento es un DIRECTORIO de salida -- la herramienta escribe
        # out_ghidra.c/out_ghidra.h/out_angr.c adentro (pasarle un nombre con
        # extensión ".c" crea un directorio con ese nombre, no un archivo).
        docker run --rm --platform linux/amd64 -v "${BASE_DIR}:/app" devrvk/so-decompiler decompile "/app/$rel_so_path" "/app/$rel_so_out"
    done
fi

echo "Decompilación de archivos .so finalizada. Resultados en ${SO_OUT_BASE}/<lib>_<abi>/{out_ghidra.c,out_ghidra.h,out_angr.c}"
