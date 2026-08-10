# Depuración en Hardware Real y Análisis de Core Dumps (.psp2dmp)

En consola real PS Vita (a diferencia del emulador Vita3K) la forma más simple y confiable de depurar es combinar el **logging directo a archivo por ejecución** con el **análisis automatizado de coredumps (`.psp2dmp`)**.

---

## 1. Logging a archivo (un log único por ejecución)

> [!IMPORTANT]
> No uses un único `log.txt` acumulativo o estático. Es muy fácil confundirse bajando o leyendo un log viejo. Genera un nombre único con timestamp por cada ejecución.

```c
#include <psp2/rtc.h>   // o <time.h> si el loader usa newlib con USE_SCELIBC_IO

#ifdef DATA_PATH
static char log_file_path[256] = {0};
if (log_file_path[0] == '\0') {
    sceIoMkdir(DATA_PATH "logs", 0777);   // sceIo NO crea directorios intermedios automáticamente
    time_t t = time(NULL);
    sceClibSnprintf(log_file_path, sizeof(log_file_path), "%slogs/log_%u_.txt", DATA_PATH, (unsigned int)t);
}
int fd = sceIoOpen(log_file_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
if (fd >= 0) {
    sceIoWrite(fd, buffer, strlen(buffer));
    sceIoClose(fd);
}
#endif
```

> [!TIP]
> - `sceIoMkdir` falla silenciosamente si la carpeta ya existe — no hace falta verificar el error.
> - Guarda `log_file_path` en una variable `static` para que no cree un archivo nuevo por cada línea logueada.
> - Borra logs viejos de la tarjeta si cambiaste la ruta de logs para evitar falsos diagnósticos sobre datos desactualizados.

---

## 2. Diagnóstico Diferencial: Crash vs. Freeze (Colgada)

| Tipo de Problema | Síntoma en el Log | Causa Frecuente | Herramienta de Solución |
| :--- | :--- | :--- | :--- |
| **Crash Real** | El log se corta abruptamente. La consola genera un `.psp2dmp` en `ux0:data/`. | Data Abort (Puntero nulo `R0=0`/`R5=0`), acceso a memoria inválida, corrupción de heap. | Usar `parse_dump.py` en `porting_tools`. |
| **Freeze / Colgada** | El log se detiene tras una acción (`"Playing Video"`, `"Loading..."`), o repite un patrón. | El motor espera un callback JNI de Java que nunca llega (video, sonido, etc.). | Implementar stub JNI que dispare el callback (ver `jni_stubs.md`). |

---

## 3. Análisis Automatizado de Crash Dumps (`.psp2dmp`) con `parse_dump.py`

Cuando la consola crashea, la Vita genera un archivo `psp2core-<timestamp>-<offset>-eboot.bin.psp2dmp` en `ux0:data/`.

> [!TIP]
> Usa la herramienta automatizada **[`porting_tools/parse_dump.py`](../../porting_tools/parse_dump.py)** o la **Opción 11/Opción 2 de [`porting_tools/manage_vita.py`](../../porting_tools/manage_vita.py)** para analizar el dump en segundos.

### Ejecución del Análisis Automatizado:

```bash
# Analizar automáticamente el último dump descargado en logs/:
python3 porting_tools/parse_dump.py

# O especificar archivos explícitamente:
python3 porting_tools/parse_dump.py logs/Inotia3-psp2core-XXXXX.psp2dmp build/inotia3.elf inotia3-1-3-2/lib/armeabi/libgame.so
```

### ¿Qué hace `parse_dump.py` automáticamente?
1. **Resolución en `inotia3.elf`** (`0x81xxxxxx`): Llama a `arm-vita-eabi-addr2line` para mostrar el archivo C/C++ y número de línea exacto del loader.
2. **Resolución en `libgame.so`** (`0x98xxxxxx` / `.so` base): Auto-detecta la dirección de carga en RAM del `.so`, resuelve los símbolos dinámicos C++ (`demangled`), e identifica la función exacta (ej: `png_read_png + 0x20`).
3. **Desensamblado Thumb**: Muestra el bloque de código máquina alrededor del PC (Program Counter) y LR (Link Register), marcando la instrucción culpable (ej: `ldr r2, [r5, #4]`).
4. **Diagnóstico Causa Raíz**: Identifica automáticamente si un registro usado en la instrucción era `0x00000000` (dereferencia de puntero nulo).
5. **Reconstrucción del Call Stack**: Mapea las palabras de la pila (`SP`) para mostrar la cadena de llamadas completa.

---

## 4. Método de Análisis Manual con `vita-parse-core` (Fallback)

Si necesitas invocar directamente `vita-parse-core`:

```bash
export VITASDK=~/vitasdk && export PATH="$VITASDK/bin:$PATH"
python3 /Users/metalsyntax/vita-tools/vita-parse-core/main.py ruta/al/volcado.psp2dmp ruta/al/build/inotia3.elf
```

> [!WARNING]
> Recuerda que `vita-parse-core` nativamente solo resuelve símbolos del `.elf` (rango `0x81xxxxxx`). Para direcciones dentro del `.so` (rango `0x90000000` - `0x9fffffff`), resta la dirección base asignada al `.so` (`offset = addr - so_base`) y busca el símbolo en el `.so` con `arm-vita-eabi-objdump -T`.
