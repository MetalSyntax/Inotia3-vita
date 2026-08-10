# Toolchain Gotchas y Workarounds de Compilación

---

## 1. Espacios en Rutas de Directorio y `vita-pack-vpk`

> [!CAUTION]
> Si la ruta del proyecto o cualquier directorio padre contiene **espacios en blanco** (ej: `/Volumes/Seagate/PSVITA Develop/`), la herramienta `vita-pack-vpk` invocada por CMake puede segmentar incorrectamente los argumentos, generando un VPK corrupto o fallando el build.

### Solución Recomendada (Build en Directorio Temporal `/tmp`):

```bash
# Compilar en un directorio temporal fuera de rutas con espacios
mkdir -p /tmp/inotia3-build && cd /tmp/inotia3-build
export VITASDK=/Users/metalsyntax/vitasdk
export PATH="$VITASDK/bin:$PATH"

cmake "/Volumes/Seagate/PSVITA Develop/Inotia3-vita" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)

# Copiar el VPK generado de vuelta al proyecto
cp inotia3.vpk "/Volumes/Seagate/PSVITA Develop/Inotia3-vita/build/"
```

---

## 2. Incompatibilidad de Políticas CMake (`CMAKE_POLICY_VERSION_MINIMUM`)

> [!IMPORTANT]
> Versiones recientes de CMake lanzan un error fatal si las dependencias vendoreadas antiguas definen `cmake_minimum_required` obsoleto.
> Añadir el flag `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` al invocar `cmake` resuelve este error sin modificar los archivos de dependencias de terceros.

---

## 3. Limpieza de Archivos Ocultos de macOS (`._*`)

> [!TIP]
> macOS crea archivos de atributos extendidos `._*` en sistemas de archivos no HFS+ / APFS. Estos archivos interfieren con la creación del paquete VPK. Ejecuta `clean_macos.sh` antes de empaquetar.
