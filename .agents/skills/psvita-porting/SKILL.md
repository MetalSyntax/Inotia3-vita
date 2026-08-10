---
name: psvita-porting
description: Guía práctica y automatizada, basada en soluciones confirmadas en consola física PS Vita, para portar juegos Android (especialmente motores cocos2d-x con arquitectura so-loader + FalsoJNI). Activar en tareas de depuración en hardware real, análisis automatizado de crash dumps (.psp2dmp, psp2core), uso de parse_dump.py y manage_vita.py, empaquetado de assets (apk/obb), stubs de JNI con callbacks, input táctil (sceTouch/slots), errores de VPK (LiveArea, 0x8010113D) y gotchas de compilación (rutas con espacios).
---

# Porting de Juegos Android a PS Vita (so-loader + cocos2d-x)

Esta skill consolida flujos de trabajo, utilidades automatizadas y patrones de resolución de errores **probados en consola física PS Vita**. Aplica a puertos basados en *so-loader* (ejecutables Vita nativos que cargan bibliotecas `.so` de Android sin relinkear, enlazando imports mediante tablas de funciones y emulando JNI con FalsoJNI/so_util).

---

## ⚡ Guía de Diagnóstico Rápido (Troubleshooting Matrix)

Si te encuentras con un problema específico en el port, consulta el documento de referencia correspondiente:

| Síntoma / Problema | Causa Probable | Documento / Herramienta de Referencia |
| :--- | :--- | :--- |
| **Crash repentino** en consola real con archivo `.psp2dmp` en `ux0:data/`. | Data Abort, puntero nulo (`R0=0`, `R5=0`), instrucción inválida o acceso prohibido. | **[`references/hardware_debugging.md`](references/hardware_debugging.md)**<br>Herramienta: **[`porting_tools/parse_dump.py`](file:///Volumes/Seagate/PSVITA%20Develop/Inotia3-vita/porting_tools/parse_dump.py)** |
| **El juego se congela (freeze)** sin mostrar crash ni continuar el log. | Un método JNI no implementado espera un callback de retorno que nunca llega. | **[`references/jni_stubs.md`](references/jni_stubs.md)** |
| **El touch no responde** en consola física pero los botones sí. | `sceTouchSetSamplingState` no fue llamado al iniciar, o se usó el ID crudo del touch. | **[`references/input_handling.md`](references/input_handling.md)** |
| **Corrupción aleatoria de Heap** que crashea en `free()`. | Pasar IDs crudos de `SceTouchReport` (>4) a `nativeTouchesBegin/End` escribiendo fuera del arreglo. | **[`references/input_handling.md`](references/input_handling.md)** |
| **Error `0x8010113D`** al instalar el `.vpk` en VitaShell (~99%). | Imágenes LiveArea no están en formato PNG indexado de 8 bits (paleta 256) o `template.xml` mal configurado. | **[`references/vpk_packaging.md`](references/vpk_packaging.md)** |
| **Fallo al empaquetar VPK** con `vita-pack-vpk`. | La ruta del proyecto contiene espacios en blanco (`/Volumes/.../`). | **[`references/toolchain_gotchas.md`](references/toolchain_gotchas.md)** |
| **Archivos/Assets no se encuentran** en el juego (`fopen: 0x0`). | Falta el prefijo correcto de ruta interna en el `.obb`/`.apk` o en el fallback ZIP. | **[`references/asset_packaging.md`](references/asset_packaging.md)** |

---

## 🛠️ Herramientas Integradas en el Proyecto

- **[`porting_tools/parse_dump.py`](file:///Volumes/Seagate/PSVITA%20Develop/Inotia3-vita/porting_tools/parse_dump.py)**: Analizador automatizado de crash dumps `.psp2dmp`. Resuelve símbolos dinámicos C++ en el `.so` y en el `.elf`, desensambla el punto de crash y diagnostica la causa raíz automáticamente.
- **[`porting_tools/manage_vita.py`](file:///Volumes/Seagate/PSVITA%20Develop/Inotia3-vita/porting_tools/manage_vita.py)**: CLI interactivo para despliegue por FTP, descarga de logs, verificación de `libshacccg.suprx` y ejecución automatizada del análisis de dumps (Opción 11).

---

## 📚 Índice de Documentos de Referencia Deep-Dive

1. **[hardware_debugging.md](references/hardware_debugging.md)**
   - Sistema de logs por timestamp (`log_<timestamp>_.txt`).
   - Uso automatizado de `parse_dump.py` con `vita-parse-core`.
   - Método iterativo "un fallo a la vez".

2. **[asset_packaging.md](references/asset_packaging.md)**
   - Estrategias de empaquetado (`.apk`/`.obb` mínimos vs. archivos sueltos).
   - Generación de `.obb` con Python (`ZIP_STORED`).
   - Identificación de prefijos de rutas internas en `CCFileUtils`.

3. **[input_handling.md](references/input_handling.md)**
   - Inicialización del sensor táctil con `sceTouchSetSamplingState`.
   - Algoritmo de asignación de slots estables (0-4) para evitar corrupción de heap.
   - Touches sintéticos para joysticks virtuales y edge-triggering de botones físicos.

4. **[jni_stubs.md](references/jni_stubs.md)**
   - Identificación de funciones JNI que requieren callbacks de retorno (`onVideoCompleted`, etc.).
   - Disparo automático de callbacks para evitar congelamientos (freezes).

5. **[vpk_packaging.md](references/vpk_packaging.md)**
   - Diagnóstico del error `0x8010113D`.
   - Verificación de paleta indexada de 8 bits en PNGs con Python/`pngquant`.
   - Ajustes de `template.xml` (`style="a1"`).

6. **[toolchain_gotchas.md](references/toolchain_gotchas.md)**
   - Workaround para compilar sin errores de rutas con espacios (build en `/tmp/`).
   - `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` para compatibilidad de CMake.
   - Limpieza de basura de macOS (`._*`).
