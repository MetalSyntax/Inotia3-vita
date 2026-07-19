# Plan de Port de Inotia 3 para PS Vita

Este plan es **modificable** — actualizarlo a medida que se confirman o descartan hipótesis en consola
real/Vita3K, igual que se hizo en `../Zenonia2-vita/plan_zenonia_port.md` y
`../Zenonia3-vita/plan_zenonia3_port.md`.

> **Corrección importante sobre la premisa inicial (2026-07-18):** se pidió tratar el motor de Inotia 3
> como "parecido" al de Zenonia 2/3. El análisis de símbolos (Fase 0 de este documento) **contradice
> eso**: Inotia 3 es un juego de **Com2uS** (`com.com2us.*`, `Com2usProtection`, `www.com2us.com`), no de
> Gamevil — el motor de Zenonia 2/3 es "Nexus2/Clet" de Gamevil (`com.gamevil.nexus2.Natives`), un ABI JNI
> completamente distinto (namespace, nombres de método, firmas). Lo que sí se reutiliza de los ports de
> Zenonia es la **arquitectura genérica de SoLoader/FalsoJNI/vitaGL** documentada en la skill
> `psvita-porting` — no el código concreto del loader. Este plan trata a Inotia 3 como un port nuevo desde
> cero, con su propio análisis de motor real.

## 0. Qué se confirmó del motor real (Fase 1 del análisis — completada)

Análisis realizado sobre `inotia3-1-3-2/lib/armeabi/libgame.so` (stripped, ELF32 ARM) con `objdump -T`
(dynsym) y `strings`, contrastado contra `decompiled_apk/sources/com/com2us/wrapper/*.java` (jadx). El
pseudo-C completo queda en `decompiled_so/libgame_armeabi/out_ghidra.c` (Ghidra headless vía
`devrvk/so-decompiler`, corrido en Docker — ver Fase 1 más abajo para reproducirlo).

### Identidad del motor
- **Publisher/engine real: Com2uS**, no Gamevil. El wrapper nativo es `com.com2us.wrapper.*`
  (`WrapperRenderer`, `WrapperJinterface`, `WrapperActivity`, `WrapperData`, `WrapperEventHandler`,
  `SoundManager`, `ModuleManager`) — 44 símbolos `Java_com_com2us_*` exportados, ninguno bajo `gamevil`.
- El término **"Clet"** aparece igual que en Zenonia (`nativeResumeClet`, `onCletStarted/Paused/Resumed/
  Destroyed` en `ModuleManager`) pese a ser un publisher distinto — probablemente herencia compartida de
  plataformas Java coreanas de la era feature-phone (WIPI/GVM), **no evidencia de mismo motor**. No asumir
  ninguna otra similitud a partir de este término.
- Sin `RegisterNatives` explícito buscado aún — pendiente confirmar en `out_ghidra.c` si la resolución de
  nativos es 100% por convención `dlsym` (como Zenonia) o si el `.so` registra dinámicamente; con 44
  exports `Java_com_com2us_*` ya visibles en la tabla de símbolos dinámicos, lo más probable es que sea
  igual por convención de nombre — confirmar antes de asumir.

### Ciclo de vida nativo real (bien distinto al `handleCletEvent` único de Zenonia)
Fuente: `WrapperRenderer.java` (métodos `native` declarados + su uso real en `onSurfaceCreated`/
`onDrawFrame`/`onPause`/`onResume`):

1. `nativePreInit(int[] parameter, int screenW, int screenH)` — se llama **antes** que `nativeInit`, en
   `onSurfaceCreated`, con el ancho/alto real de pantalla del device Android.
2. `nativeInit(long[] systemCurrentTime, int[] appUpTime, int[] pixelBuffer, float[] gyro, Object activity)`
   — recibe arrays por-referencia (Dalvik primitive arrays, no un struct) más el objeto `Activity` completo
   como `jobject` — este último es un patrón que puede necesitar un stub de JNI más elaborado que un simple
   valor de retorno (FalsoJNI tiene que devolver *algo* usable como referencia de objeto opaca).
3. Loop de frame: `nativeEvent(type, p1, p2, touchCount)` por cada evento de input encolado, luego
   `nativeRender(timerID)` para cada timer venido, luego `nativeReDraw()` una vez por frame
   (`onDrawFrame`).
4. Ciclo de vida de pausa/resume en 3 pasos, no uno solo: `nativePause()` → (más adelante) `nativeResume()`
   → (más adelante todavía, tras esperar que la app esté "corriendo") `nativeResumeClet()`. Portar esta
   máquina de estados tal cual (`rendererState` 0-6 en `WrapperRenderer.java`) es más seguro que
   simplificarla a un solo evento de resume — el propio código Android la implementa con sleeps/threads
   por una razón (esperar que el hilo de render esté listo).
5. `nativeDone()` al destruir.

### Gráficos: mismo tipo de riesgo que Zenonia (pipeline fijo GLES1), pero confirmarlo en assets propios
Imports de `.so` confirman **pipeline fijo GLES1 completo con variantes de punto fijo** (`glClearColorx`,
`glColor4x`, `glFrustumx`, `glOrthox`, `glLoadMatrixx`, `glMultMatrixx`, `glRotatex/glScalex/glTranslatex`,
`glPointSizex`, `glLineWidthx`, `glTexEnvx`) + `glVertexPointer`/`glTexCoordPointer`/`glColorPointer`/
`glNormalPointer` + `glDrawArrays`/`glDrawElements` — **sin** `glCreateShader`/`glShaderSource` en ningún
lado del binario. Es decir: **no hay shaders GLSL que traducir** — `translate_shaders.py` y el flujo
GLSL→Cg de `manage_vita.py` (que sí existía en el port de Dungeon Hunter 2) **no aplican a este juego** y
fueron removidos de `manage_vita.py` al adaptarlo (ver Fase 5 de `psvita-port-init`). Riesgos esperables,
por analogía con Zenonia (mismo tipo de pipeline, no mismo código):
- Necesita los mismos wrappers de conversión de formato de textura si el juego sube en `RGB565` u otro
  formato no nativo de vitaGL.
- `GL_FIXED` (los `x` al final de cada función de arriba) hay que traducirlo a `GL_FLOAT` en algún punto del
  wrapper — confirmar si `glDrawArrays`/`glDrawElements` reciben directamente punteros `GL_FIXED` (como en
  Zenonia) revisando `out_ghidra.c`.
- Filtros de textura incompletos → pantalla blanca (mismo síntoma que Zenonia Fase 10) si no se fuerza
  `GL_LINEAR` en cada `glTexImage2D`.

### Assets: más simple que Zenonia — sin JNI custom de lectura
Imports de `.so` muestran **libc estándar para I/O**: `fopen`, `fread`, `fclose`, `fstat`, `stat`, `lstat`,
`opendir`/`readdir`, `mkdir` — **no** hay un equivalente a `readAssets`/`readAssete` de Zenonia (lectura vía
JNI de un `AssetManager` de Android). Esto sugiere que el juego espera los assets como **archivos sueltos
en el filesystem** (no empaquetados dentro del `.apk`/`.obb` como ZIP) — confirmar la ruta base real
buscando en `assets/common/` del APK extraído y en las llamadas iniciales de `WrapperActivity`/
`WrapperJinterface`. Si se confirma, el trabajo de la Fase de Assets es **más simple** que en Zenonia: solo
hookear `fopen`/`stat`/`opendir` para redirigir a `ux0:data/inotia3/assets/`, sin el truco de header Dalvik
de 16 bytes que sí hacía falta para el `AssetManager` simulado de Zenonia.

### Input: touch estándar de Android, hasta 5 puntos
`WrapperEventHandler.java` usa `MotionEvent`/`MotionEventCompat` de Android estándar (no una lectura cruda
de HAL), con `MAX_TOUCH_POINT = 5` (multitouch real) o `1` según una rama del código — confirmar cuál rama
corre en este build. Los tipos de evento empaquetados en `nativeEvent(type,...)` incluyen al menos dos
valores "negativos altos" (`-2147483645`/`-2147483642`, es decir `0x80000003`/`0x80000006`) que
probablemente codifican `ACTION_POINTER_DOWN`/`UP` con el índice de puntero embebido al estilo
`MotionEventCompat.ACTION_POINTER_INDEX_MASK` — pendiente de mapear con precisión leyendo
`WrapperEventHandler.onTouch()` completo antes de implementar el wrapper de `sceTouchPeek`.

### Networking: presente, tratar como no-op/best-effort
Imports incluyen `socket`/`connect`/`getaddrinfo`/`send`/`recvfrom` — parte del `HttpManager`/hub de
Com2uS (login, noticias, IAP). No es necesario para que el juego arranque; stub/no-op como analítica,
salvo que algún camino de arranque bloquee esperando una respuesta de red (confirmar en consola, mismo
método que Zenonia: un fallo a la vez).

### Métodos JNI exportados por el `.so` (44 total, `Java_com_com2us_*`)
Agrupados por clase Java (ver `objdump -T ... | grep Java_` para la lista exacta con offsets):
- `WrapperRenderer` — ciclo de vida ya descripto arriba (`nativeInit/PreInit/Event/Render/ReDraw/Pause/
  Resume/ResumeClet/Done/CheckQueueEvent`).
- `WrapperJinterface` — `nativeDefaultUserInput`, `nativeUserInput`, `nativeDownloadProcess`,
  `nativeForceStopDownload`, `nativeWebviewCB`, `nativeConnectCB`.
- `HubConstant` (Com2uS Hub — login/social) — 7 métodos `callHub*`/`callCSHubInitialize`.
- `InApp` (compras in-app) — `jniInAppInitialize/Uninitialize`, `resultPostInApp/License/Target` — **no
  implementar** salvo que bloquee el arranque; no hay tienda en este port.
- `ActiveUser`, `WrapperProtection` (`Com2usProtection` anti-tamper), `TapjoyManager`, `ADManager`/
  `MopubManager` (ads), `NewsBanner` — todos candidatos a no-op/stub silencioso, ninguno debería ser
  necesario para el gameplay offline.

## 1. Guía de Decompilación (ya ejecutada — reproducible)

```bash
# Java del APK
jadx -d decompiled_apk inotia3-1-3-2.apk

# Símbolos dinámicos del .so (no requiere toolchain ARM)
objdump -T inotia3-1-3-2/lib/armeabi/libgame.so | grep "Java_"   # exports JNI
objdump -T inotia3-1-3-2/lib/armeabi/libgame.so | grep "UND"     # imports (libc/GL/sockets/etc.)
strings inotia3-1-3-2/lib/armeabi/libgame.so | grep -iE "com2us|gamevil|clet"

# Pseudo-C vía Ghidra headless en Docker -- el 2do argumento es un DIRECTORIO,
# la herramienta escribe out_ghidra.c/out_ghidra.h/out_angr.c adentro (pasarle
# un nombre terminado en ".c" crea un directorio con ese nombre, no un archivo)
mkdir -p decompiled_so/libgame_armeabi
docker run --rm --platform linux/amd64 \
  -v "$(pwd)/inotia3-1-3-2/lib/armeabi:/input" \
  -v "$(pwd)/decompiled_so/libgame_armeabi:/output" \
  devrvk/so-decompiler decompile /input/libgame.so /output
```

También automatizado en `porting_tools/build/decompile_all.sh` (adaptado a las rutas de este proyecto).

> [!NOTE]
> El decompile de Ghidra/angr tarda varios minutos (angr hace recuperación de convenciones de llamada
> función por función) — lanzarlo en background y seguir con el análisis de símbolos mientras corre.

## 2. Plan del Port (Hitos y Fases)

### Fase 1: Análisis Estático de Símbolos e Inicialización JNI — ✅ Completada
Ver sección 0. Resultado: motor Com2uS propio, ciclo de vida nativo de 3 pasos para pause/resume, pipeline
GLES1 fijo sin shaders, I/O de archivos estándar (sin JNI de assets custom), 44 exports JNI mapeados a sus
clases Java. **Este port arranca desde cero** — no hay código de otro port para forkear directamente, solo
arquitectura genérica de `psvita-porting`.

### Fase 2: Bootstrap del Loader — ✅ Compila (2026-07-19), pendiente primera corrida

> **Estado real:** el boilerplate resultó estar mucho más completo de lo que parecía a primera vista — no
> era una plantilla en blanco. `utils/init.c::soloader_init_all()` ya hacía **todo** el bootstrap genérico
> (`so_file_load` → `settings_load` → `so_relocate` → `resolve_imports` → `so_patch` → `so_flush_caches` →
> `so_initialize` → `gl_preload` → `jni_init`) y `source/dynlib.c` ya traía prácticamente el 100% de la
> tabla de imports confirmada en la Fase 0 (todo el pipeline GLES1 fijo, libc, sockets, pthread, etc.) —
> solo faltaban `inet_addr` y un stub de `statfs` (agregados). Lo que sí faltaba por completo:
> `lib/falso_jni/` estaba **vacío** en disco (el submódulo git nunca se vendorizó en esta copia de trabajo)
> — se copió el vendoring completo (MIT, v-atamanenko) desde `../Zenonia2-vita/lib/falso_jni/`.
>
> Un fork dedicado barrió `decompiled_so/libgame_armeabi/out_ghidra.c` (214k líneas) buscando toda llamada
> JNIEnv que el `.so` hace *hacia* "Java" (no las 44 exportadas, esas se llaman directo por símbolo desde
> `main.c`). Hallazgo clave: **de ~160 llamadas JNI encontradas, ninguna pertenece a `WrapperRenderer`** —
> son todas de subsistemas opcionales (Hub, IAP, Tapjoy, ads, news banner, stats de memoria de un HUD de
> debug) que ya estaban marcados como "no-op hasta que se demuestre necesario". Ghidra no pudo resolver el
> nombre de la mayoría (firmas JNI quedaron como punteros `DAT_xxxxx` sin texto recuperable) — **solo se
> registraron en `java.c` los 6 casos con firma JNI literal confirmada** (`HubInitialize`, `alertPopup`,
> `setGameResolution`, `adCreate`, `setAdProperty`, `newsBannerInit`, todos no-ops seguros). El resto queda
> deliberadamente sin registrar: un método no registrado hace que `GetMethodID` devuelva `NULL` (que el
> motor debería chequear antes de llamar), mientras que registrarlo con un tipo de retorno *adivinado* mal
> puede ser peor (`methodIntCall` devuelve `-1` para un id no encontrado en su tabla específica, que el
> motor podría leer como `true`). Ver el header-comment de `source/java.c` para el detalle completo.
>
> **`source/main.c` reescrito** con la secuencia real: `JNI_OnLoad` → `nativePreInit(parameter[3], 960,
> 544)` → `nativeInit(systemCurrentTime[1], appUpTime[1], pixel[1024*1024], gyro[3], activity_placeholder)`
> (tamaños de `pixel`/`gyro` confirmados leyendo `WrapperJinterface.java`/`GyroManager.java` — `pixel` es un
> buffer de glifos de texto de 1024×1024 ints, ~4MB, el mismo patrón que `text_rendering.md` de la skill
> `psvita-porting`) → loop llamando solo `nativeReDraw()` + `gl_swap()` por ahora (sin `nativeEvent`/
> `nativeRender` todavía — eso es Fase 5, Input).
>
> **Compilación real verificada** con el toolchain de VitaSDK instalado (`~/vitasdk`) vía `cmake`+`make` en
> `/tmp/inotia3-build` (mismo workaround de ruta-con-espacio que usa `build_and_install.sh`): **build
> exitoso, `inotia3.vpk`/`eboot.bin` generados sin errores** (solo warnings inofensivos de `mktemp`
> deprecated y un cast en `FalsoJNI_ImplBridge.c` que es del vendoring, no nuestro). Se agregó
> `game_log()` en `main.c` (bridge hacia `l_info` de `utils/logger.h`) porque `FalsoJNI_Logger.c` vendorizado
> esperaba esa función para sus propios logs `[JNI]`/`[JNI WARN]`/`[JNI ERR]`.
>
> **Sin confirmar todavía (bloquea Fase 3, no Fase 2):**
> - Convención real de rutas de archivo: el fork no pudo determinar con confianza si `fopen`/`stat` esperan
>   rutas absolutas, relativas a `assets/`, o algo construido desde un callback tipo `getFilesDir()` — no se
>   encontró ningún lookup JNI con firma `Ljava/io/File;` en los ~160 casos revisados (argumento débil en
>   contra de esa última hipótesis, no una confirmación). `source/dynlib.c` todavía no tiene hooks de ruta
>   específicos de Inotia 3 — hace falta un primer log de consola/Vita3K mostrando qué ruta pide el juego
>   antes de escribir el hook, no adivinar.
> - ABI real de los arrays que recibe `nativeInit` (Dalvik `ArrayObject` de 16 bytes crudo, como Zenonia, vs.
>   `Get/ReleaseXxxArrayElements` reales vía JNIEnv) — se pasaron punteros C planos como hipótesis de
>   trabajo; si el primer run crashea dentro de `nativeInit` con un patrón de memoria corrupta, este es el
>   sospechoso número uno.
> - El objeto `activity` pasado a `nativeInit` es un sentinel (`&fake_activity_obj`), no un `jobject` real —
>   si el `.so` intenta `Call*Method` sobre él, el nombre pedido necesita un handler nuevo en `java.c`.
>
> `CMakeLists.txt` ya tiene `VITA_APP_NAME "Inotia 3"`, `VITA_TITLEID "INOTIA3PV"`, `VITA_VPKNAME "inotia3"`,
> `DATA_PATH "ux0:data/inotia3/"`, `SO_PATH "${DATA_PATH}libgame.so"`, proyecto `inotia3`.

### Fase 3: Primera Puesta en Marcha (Vita3K primero, consola real después)
Metodología igual a `psvita-porting`/`so-crash-triage`: **un log a la vez, un bug a la vez**, log con
timestamp por corrida en `ux0:data/inotia3/logs/`.

### Fase 4: Gráficos (Wrappers GLES1 de punto fijo)
Ver sección 0 (Gráficos). Sin shaders que traducir — enfocarse en conversión de formato de textura,
`GL_FIXED`→`GL_FLOAT`, y filtros de textura completos para evitar pantalla blanca.

### Fase 5: Input (Touch, hasta 5 puntos)
Confirmar el mapeo exacto de `type`/`p1`/`p2`/`touchCount` de `nativeEvent` leyendo
`WrapperEventHandler.onTouch()` completo (los dos casos, `MAX_TOUCH_POINT` 1 y 5) antes de implementar el
wrapper de `sceTouchPeek`. Aplican las lecciones genéricas de `psvita-porting` (`references/
input_handling.md`): touch apagado por default, nunca pasar el ID crudo de 8 bits del touch de Vita,
mapear a slots 0..4 estables.

### Fase 6: Assets
Confirmar hipótesis de la sección 0 (archivos sueltos vía libc estándar, no ZIP vía JNI) inspeccionando
`assets/common/` del APK extraído y el flujo real de arranque en `WrapperActivity`. Si se confirma, el
trabajo es solo de hooks de rutas (`fopen_hook`/`stat_hook`/`opendir_hook`), sin parseo de formato Dalvik.

### Fase 7: Audio (.mmf / .ogg vía `SOUNDSYSTEM_*` propio de Com2uS)
El `.so` importa un sistema de sonido custom completo (`SOUNDSYSTEM_Play*/Stop*/PlayBGM*`,
`SOUNDPLAYER_*`, tablas `SOUNDBGMBASE_*`/`SOUNDEFFECTBASE_*`/`SOUNDINFOBASE_*`) — **no** es
cocosdenshion/OpenAL genérico, hay que entender este sistema propio (probablemente definido dentro del
mismo `.so`, no importado de otra parte — revisar si son símbolos `UND` reales o internos re-exportados)
antes de mapear a `sceAudioOut`. Formatos de audio reales confirmados con `strings`: `.mmf` y `.ogg` — usar
Tremor para `.ogg`. No hay código de test de audio heredado de otro port aplicable aquí (ver
`porting_tools/tests/run_tests.sh`, dejado como placeholder) — escribir uno nuevo contra el decoder real
elegido antes de dar por buena la implementación.

### Fase 8: Construcción y LiveArea
`extras/livearea/` ya tiene los 4 PNG + `template.xml` del boilerplate genérico — reemplazar por arte real
de Inotia 3 antes de la primera instalación en consola (evitar el ícono genérico "SoLoBoP").

### Fase 9: Pruebas y Depuración en Consola Real
Igual metodología que Zenonia2/3 (`port_progress.md` de ambos como referencia de **clase de bugs**, no de
código): logging a archivo por corrida, `so-crash-triage` para cruzar `.so` real + `out_ghidra.c` + Java
decompilado ante cualquier crash con `.psp2dmp`.

## 3. Riesgos genéricos heredados de la arquitectura SoLoader (aplican igual que en cualquier port)
- Rutas con espacio en `PSVITA Develop` → build en `/tmp` (`porting_tools/build/build_and_install.sh` ya lo
  resuelve).
- `pthread` necesita `-Wl,--whole-archive ... --no-whole-archive` (FalsoJNI usa `pthread_mutex_t`).
- `libshacccg.suprx` debe estar en `ur0:data/` en la consola de destino (requisito de vitaGL).
- Mutex/condvar estáticos de Bionic en cero (`PTHREAD_MUTEX_INITIALIZER`) pueden crashear contra PTE de
  VitaSDK si algún código (libstdc++ interno o el propio motor) los usa sin inicialización explícita — ver
  el bug real documentado en `Zenonia3-vita/plan_zenonia3_port.md` §4 antes de "descubrirlo" de nuevo si
  aparece un crash dentro de una función `pthread_*` con el primer campo en `0`.

## 4. Riesgos específicos de Inotia 3 (no confirmados en Zenonia, propios de este motor)
- El argumento `Object activity` completo pasado a `nativeInit` (no solo arrays primitivos) — confirmar qué
  métodos de `Activity` el `.so` invoca de vuelta sobre ese `jobject` vía JNI antes de asumir que un stub
  opaco alcanza.
- Máquina de estados de pause/resume en 3 pasos (`nativePause`→`nativeResume`→`nativeResumeClet`, con
  esperas activas de hasta 6 segundos en el código Android real) — simplificarla incorrectamente puede
  dejar el motor esperando un estado que nunca llega.
- Sistema de sonido `SOUNDSYSTEM_*`/`SOUNDPLAYER_*` propio de Com2uS, no un middleware conocido — mapear su
  contrato real (via `out_ghidra.c`) antes de intentar interceptarlo.
- Superficie JNI grande de subsistemas opcionales (Hub, IAP, ads, Tapjoy) que **no existían** en Zenonia —
  confirmar cuáles de estos 44 exports son realmente invocados durante el arranque/gameplay offline antes
  de invertir tiempo implementando alguno.
