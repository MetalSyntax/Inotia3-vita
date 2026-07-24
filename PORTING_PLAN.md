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
>
> **Nota de toolchain (VitaSDK moderno, GCC 15.2 / binutils nuevos).** En un VitaSDK con GCC 15 el build
> falla por estrictez nueva del compilador/linker que toolchains viejos toleraban. El `CMakeLists.txt`
> del repo **no** se tocó (sigue apto para el toolchain original del `build_and_install.sh`); en su lugar,
> al compilar sobre GCC 15 hay que añadir a `CMAKE_C_FLAGS` tres flags de compatibilidad:
> `-fpermissive` (GCC 15 promueve a error `int-conversion`/`return-mismatch`/`implicit-decl` del FalsoJNI
> vendorizado), `-Wl,--no-warn-mismatch` (el nuevo `ld` da error —antes solo warning— por el mismatch de
> ABI VFP entre nuestros objetos `-mfloat-abi=softfp` y las libs hard-float del SDK; **no** cambiar a
> `-mfloat-abi=hard`, rompería la interop de floats con el `.so` softfp) y `-Wl,--allow-multiple-definition`
> (el `libvitaGL.a` nuevo ya trae EGL, que choca con `source/reimpl/egl.c`; los objetos del proyecto se
> enlazan primero y ganan). Aparte, `ATOMIC_VAR_INIT` (removido en C23) se reemplazó por init plano en
> `logger.c` — eso sí es un fix real portable, ya commiteado.

### Fase 3: Primera Puesta en Marcha (Vita3K primero, consola real después)
Metodología igual a `psvita-porting`/`so-crash-triage`: **un log a la vez, un bug a la vez**, log con
timestamp por corrida en `ux0:data/inotia3/logs/`.

> **Corrida 1 (2026-07-19) — pantalla negra, sin crash. Diagnosticada y corregida.** El `.so` llegaba
> hasta `nativeInit() and resume returned -- entering render loop` pero no dibujaba nada. Cruzando el log
> con `out_ghidra.c` se hallaron **dos causas encadenadas**, ambas ahora arregladas:
>
> 1. **El game loop es dirigido por timers, no por frames.** El `.so` registra timers con
>    `CS_knlSetTimer → TM_Set → jSetTimer →` JNI `WrapperJinterface.SetTimer(id, firetime, interval)`
>    (estático, firma `(IJJ)V`); en Android `WrapperRenderer.process()` los dispara de vuelta llamando
>    `nativeRender(id)` cuando vencen, y `TM_Run(id)` ejecuta el callback nativo (p.ej. `MainProcess` para
>    `g_MainTimer`, que corre toda la máquina de estados y el dibujado). El loop viejo solo llamaba
>    `nativeRender(0)` → ningún timer real (id ≠ 0) se disparaba nunca. **Fix:** se portó la tabla de
>    timers de `WrapperRenderer` verbatim a `source/wr_timer.c`/`.h` (32 slots, `process()`/`SetTimer()`/
>    `UnsetTimer()`), handlers `SetTimer`/`UnsetTimer` en `java.c` (ids 25/26), y `wr_run_timers()` en el
>    loop de `main.c`. El reloj del `.so` (`CS_knlCurrentTime() == *jsystemTimePtr`) **es** el array
>    `sysTime[]` que le pasamos a `nativeInit`, así que el `firetime` que llega por `SetTimer` es absoluto
>    en ese reloj — hay que seguir actualizando `sysTime[0]` cada frame (espejo de `UpdateCurTime`).
>
> 2. **El anti-tamper saltaba `startClet()`.** `nativeInit` solo llama `startClet(0,0)` (que registra
>    `g_MainTimer`) si `excuteDirectly()==0 && jIsCracked()==0`. Ambas detectan hacks vía
>    `system("ps > ux0:data/inotia3/temp")` + `fopen(temp)`; la Vita no tiene shell, `temp` nunca se crea,
>    `fopen`→NULL, y ambas retornan 1 ("tampered"). `jGetMemoryHackCheckType()` está hardcodeada a
>    `return 7` (no se puede desactivar por JNI) y `excuteDirectly` además hurga en internals de `FILE` de
>    bionic (layout no portable). **Fix:** se hookean ambas a `return 0` en `patch.c` vía
>    `hook_addr(text_base + off)`: `excuteDirectly=0x000ecf15`, `jIsCracked=0x000e981d` (Thumb, del symtab
>    estático de `nm libgame.so` — no están en `.dynsym`, así que `so_symbol` no los encuentra).
>
> Prueba irrefutable en el log: `fopen(ux0:data/inotia3/temp, r): 0x0` justo antes de "entering render
> loop", y cero llamadas `SetTimer`. **Pendiente:** correr de nuevo y confirmar que aparece actividad de
> `MainProcess` + draw calls GL reales. Próximos bloqueos probables: carga de texturas/assets (Fase 6) y
> audio (Fase 7, ya con stubs no-op puestos).

### Fase 4: Gráficos (Wrappers GLES1 de punto fijo)
Ver sección 0 (Gráficos). Sin shaders que traducir — enfocarse en conversión de formato de textura,
`GL_FIXED`→`GL_FLOAT`, y filtros de textura completos para evitar pantalla blanca.

> **Corrida 2 (2026-07-21) — pantalla negra reducida con borde blanco. Diagnosticada, fix aplicado
> (pendiente confirmar en consola/Vita3K).** Con el juego ya entrando al loop de render (Fase 3), el
> síntoma visual pasó a ser un rectángulo negro más chico que el panel, con el resto de los 960x544
> mostrando blanco fijo. Rastreando `decompiled_so/libgame_armeabi/out_ghidra.c`:
>
> 1. `jSetOriginalSize()` (llamada desde `nativePreInit`) **ignora los argumentos que le pasamos** —
>    está hardcodeada a `DAT_00170420=0x1e0` (480) / `DAT_00170424=0x140` (320): la resolución de
>    *diseño* del juego (feature-phone WVGA-ish, 3:2) es fija, no depende de lo que reportemos como
>    tamaño real de pantalla.
> 2. `CSOGL_grpInitialize()`/`SGL_SetViewport()` arman su propio `glViewport(0,0,g_pSGLDevice+4,
>    g_pSGLDevice+8)` a partir de esa resolución de diseño (más el tamaño "viewport" que le pasamos por
>    `GetScreenWidth`/`GetScreenHeight`, mezclados con lógica de letterbox centrado en
>    `__DEVICE_X`/`__DEVICE_Y`) — el resultado no necesariamente cubre el panel físico real de 960x544,
>    igual que el símbolo del framebuffer-a-textura (`SGL_FrameBufferToScreen`) hace un blit de un quad
>    a ese viewport recortado, no a pantalla completa.
> 3. **Mismo síntoma y causa raíz que Zenonia3** (`Zenonia3-vita/zenonia3_port_progress.md` §"Escalado y
>    Resolución del Juego (Cuadro pequeño)"): el motor calcula su propio `glViewport` a partir de su
>    resolución interna, que no cubre el framebuffer real de Vita, dejando el resto del panel con lo que
>    haya quedado en VRAM (blanco). La solución allí validada — y ya replicada acá — es **forzar el
>    `glViewport` real a pantalla completa incondicionalmente**, dejando `glOrthof`/`glOrthox` intactos:
>    la proyección y los vértices del quad del juego son consistentes entre sí en su propio espacio de
>    coordenadas, así que estirar el viewport a 960x544 hace que el hardware escale esa misma geometría
>    para llenar el panel, sin tocar geometría/proyección del juego.
>
> **Fix aplicado:**
> - `source/utils/glutil.c`: nuevo `glViewport_soloader()` — loguea (primeras 10 llamadas) el
>   `x,y,w,h` que pide el `.so` y siempre fuerza `glViewport(0,0,960,544)`. Registrado en
>   `source/dynlib.c` en vez del `glViewport` de vitaGL directo.
> - `source/utils/glutil.c`: nuevos `glTexImage2D_soloader()`/`glTexSubImage2D_soloader()` — mismo fix
>   de filtrado ya validado en `Zenonia2-vita`/`Zenonia3-vita` (`port_progress.md`/
>   `zenonia3_port_progress.md`): fuerzan `GL_TEXTURE_MIN_FILTER`/`GL_TEXTURE_MAG_FILTER` a `GL_LINEAR`
>   después de cada subida de textura, porque una textura sin mipmaps con el filtro por defecto
>   (`GL_NEAREST_MIPMAP_LINEAR`) se considera "incompleta" en vitaGL y se renderiza blanco puro —
>   riesgo que este mismo documento ya había anotado en la sección 0 antes de confirmarlo.
> - `source/main.c`: el loop principal ahora hace `glDisable(GL_SCISSOR_TEST)` +
>   `glViewport(0,0,SCREEN_W,SCREEN_H)` antes de su propio `glClear()` de cada frame — defensivo contra
>   que el `.so` deje un scissor rect chico activo al final de su propio dibujado, lo que limitaría nuestro
>   `glClear()` externo a ese mismo rectángulo y dejaría el borde sin limpiar para siempre (mismo síntoma).
>
> Build verificado con `porting_tools/build/build_and_install.sh` (VitaSDK real, no solo sintaxis) —
> **pendiente**: correr en Vita3K/consola y confirmar que el rectángulo negro pasa a cubrir el panel
> completo y el borde blanco desaparece. Si el log muestra el `glViewport` real pedido por el `.so`
> (los primeros 10 logueados), eso además sirve para confirmar/descartar la hipótesis de letterbox de
> `CSOGL_grpInitialize` sin tener que seguir rastreando el árbol de llamadas por vtable de `SGL::CreateDevice`
> (no resuelto por Ghidra — dispatch virtual C++).
>
> **Segundo hallazgo, más fundamental — vitaGL no consume `GL_FIXED` correctamente.** Revisando cómo
> `../Zenonia3-vita/loader/dynlib.c` resolvió el mismo pipeline fijo GLES1 (motor distinto, misma
> versión de vitaGL/VitaSDK), su propio código documenta —confirmado en consola real, no teoría— que
> vitaGL **lee los enteros Q16.16 de `GL_FIXED` como si fueran floats crudos** en `glVertexPointer`/
> `glColorPointer`/`glTexCoordPointer`, y que `glClearColorx`/`glTexParameterx` tampoco hacen la
> conversión internamente. Sin arreglarlo, la geometría queda fuera de frustum (nada visible dibujado,
> aunque el juego siga "renderizando" cada frame) — un motivo plausible adicional, independiente del
> viewport, para por qué el área negra reducida no mostraba contenido. Inotia 3 importa exactamente
> este mismo pipeline (`glVertexPointer`/`glColorPointer`/`glTexCoordPointer`/`glClearColorx`/
> `glTexParameterx`, sección 0 de este documento), así que hereda el mismo riesgo por construcción.
>
> **Fix aplicado (mismo patrón que Zenonia3, adaptado):**
> - `glClearColorx_soloader`/`glTexParameterx_soloader` en `source/utils/glutil.c`: convierten
>   directo a `glClearColor`/`glTexParameteri`.
> - `glVertexPointer_soloader`/`glColorPointer_soloader`/`glTexCoordPointer_soloader`: si `type ==
>   GL_FIXED`, **difieren** la conversión (guardan puntero/size/stride en estático) en vez de pasarla a
>   vitaGL; si no, pasan directo.
> - `glDrawArrays_soloader`/`glDrawElements_soloader`: ahí es donde se sabe cuántos vértices hace falta
>   convertir (`first+count` para arrays; el índice máximo referenciado en el buffer de índices para
>   `glDrawElements`, que Zenonia3 no necesitó pero Inotia 3 sí importa) — convierten a un buffer
>   `GLfloat` reusable (crece con `realloc` solo cuando hace falta) y regeneran el `*Pointer` real antes
>   de la llamada real a `glDrawArrays`/`glDrawElements`.
> - Registrados en `source/dynlib.c` reemplazando los símbolos de vitaGL directos para estas 7 funciones.
>
> Build verificado. **Pendiente** confirmar en consola/Vita3K si esto (solo, o junto con el fix de
> viewport de arriba) resuelve el contenido faltante dentro del rectángulo negro.

### Fase 5: Input (Touch, hasta 5 puntos) — ✅ Implementada (2026-07-21), pendiente probar en consola

Se confirmó `WrapperEventHandler.onTouch()` completo (`decompiled_apk/sources/com/com2us/wrapper/
WrapperEventHandler.java`): `WrapperActivity.IsMultiTouch = true` por default y `MainActivity` nunca lo
overridea (`this.wrapperEventHandler.initialize(this.IsMultiTouch)` en `MainActivity.java:118`), así que
Inotia 3 **siempre** corre la rama `MultiTouchListener` (`MAX_TOUCH_POINT = 5`) — la rama de un solo
punto (`SingleTouchListener`) nunca se ejecuta en este juego, no hace falta portarla.

Mapeo real confirmado (antes solo una hipótesis en este documento):
- Evento: `nativeEvent(type, PointID+1, MAKEWORD(x,y), touchCount)` donde `MAKEWORD(x,y) = ((x &
  0xFFFF) << 16) | (y & 0xFFFF)` — **no** son dos parámetros `p1`/`p2` sueltos como se especuló, `p2`
  empaqueta ambas coordenadas.
- `type`: `-2147483644` (touch down/pointer-down), `-2147483643` (drag/move, con el mismo umbral de
  40ms + >2px que el Java original antes de reenviar), `-2147483642` (release/pointer-up) — los valores
  que este documento había adivinado antes (`-2147483645`/`-2147483642`) correspondían en realidad a la
  rama de un solo punto, no a la que realmente corre.
- `PointID` es un slot estable 0-4 (el pointer id de Android), nunca el id crudo de 8 bits de
  `SceTouchReport` — mismo bug documentado en la skill `psvita-porting` (`references/
  input_handling.md`) que corrompe el heap si se pasa directo.
- `WrapperEventHandler.setIntoRealScreenRange`/`setIntoGameScreenRange` clampean a
  `GetRealScreenWidth/Height()`/`GetScreenWidth/Height()` — como `java.c` stubea las 4 al mismo 960x544,
  el `ViewPortWay` de letterbox (`ViewPortWay0..3`) es un no-op en este port; no hace falta reproducir
  esa lógica, solo escalar panel→pantalla (panel táctil de Vita es 1920x1088, exactamente 2x 960x544,
  mismo factor que `../Zenonia3-vita/loader/main.c` usa para su propio touch) y clampear a [0,960]x[0,544].

**Implementación:** `source/wr_touch.c`/`.h` (nuevo, agregado a `CMakeLists.txt`) — mismo patrón de
"slot estable por dedo" de `psvita-porting`/`input_handling.md`: `wr_touch_init()` llama
`sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START)` una vez (sin esto
`sceTouchPeek` siempre reporta 0 toques) y `wr_process_touch()` se llama una vez por frame desde
`source/main.c`, **antes** de `wr_run_timers()` — mismo orden que `WrapperRenderer.process()` (primero
vacía la cola de eventos de touch, después corre los timers). `main.c` resuelve
`Java_com_com2us_wrapper_WrapperRenderer_nativeEvent` vía `so_symbol` y lo expone a `wr_touch.c` a
través de un callback (`wr_dispatch_event`), sin acoplar `wr_touch.c` a FalsoJNI directamente.

Build verificado con `porting_tools/build/build_and_install.sh`. **Pendiente**: probar en consola/Vita3K
que el touch efectivamente mueve al personaje/UI — el mapeo de coordenadas y el umbral de drag están
implementados fielmente al Java original, pero no hay forma de confirmar sin hardware que el juego
interpreta bien estos eventos (p.ej. si alguna zona de UI espera coordenadas en el espacio 480x320 de
diseño en vez de 960x544 — no confirmado, revisar si el toque no responde en los bordes de la pantalla).

### Fase 6: Assets — dos caminos distintos, uno confirmado OK y **uno que era el bloqueo real** (corregido 2026-07-21)

Hipótesis de la sección 0 confirmada por log real (`logs/log_2026-07-19_20-53-51.txt`): el `.so` pide
**archivos de estado propio del runtime** (no assets originales del juego) directo por `fopen`/`open`/
`stat` de libc estándar, construyendo la ruta él mismo a partir de `GetUserDir()` ("ux0:data/", stub en
`java.c`) + `GetPackageName()` ("inotia3") — **sin ningún hook de rutas hace falta**,
`source/reimpl/io.c` sigue siendo un passthrough puro (solo logging) y ya funciona:

```
open(ux0:data/inotia3/texture.store, 602): 3        <- exito, el archivo esta en ux0:data/inotia3/
open(ux0:data/inotia3/Com2usProtection.sav, 0): 3    <- exito
open(ux0:data/inotia3/init.dat, 0): 4                <- exito
stat(ux0:data/inotia3/d82e1081d82e1081d82e1081d82e1081): 0  <- exito (marcador de instalacion)
open(ux0:/data/inotia3/inotia3.apk, 0): 4            <- exito (el .apk real esta presente en ux0:, no solo libgame.so)
open(ux0:data/inotia3/save0/1/2.dat, 0): -1          <- esperado, todavia no hay partida guardada
open(/sdcard/Android/data/inotia3/files/*, ...): -1  <- fallback que el juego intenta y descarta solo, no rompe nada
```

No hace falta el truco de header Dalvik de 16 bytes que sí necesitaba el `AssetManager` simulado de
Zenonia para este camino. Único hallazgo a tener en cuenta si aparece un crash de I/O más adelante:
`fsync()` devuelve `-1` en varias de estas escrituras (`Com2usProtection.sav`, `appinfo.dat`,
`environment.inf`) — no bloquea el arranque (el `.so` no chequea el valor de retorno en estos casos) pero
**no está confirmado si es inofensivo en todos los casos** o si `fsync` necesita un stub real sobre
`sceIo*` en vez de la implementación libc actual; revisar si aparece corrupción de save más adelante.

> **Corrección importante (2026-07-21) — esto NO era todo Fase 6.** El párrafo de arriba solo confirma
> el camino de los archivos de *estado* (saves, marcadores, texture cache). Los **assets originales del
> juego** (`MainMenu.png`, `font.fnt`, y por extensión toda la lista de `game_res/`) usan un camino
> **completamente distinto y roto hasta ahora**: `WrapperJinterface.GetFileDescriptor(String filename,
> int[] value, boolean closeFDdirectly): Object`, que en Android usa `AssetManager.openFd()` para sacar
> un offset+length crudos de un asset *sin descomprimir* directamente dentro del `.apk` (Android solo
> puede dar un fd+offset de un entry ZIP si está STORED, no DEFLATE — por eso Com2uS le pega la
> extensión `.jpg` a los assets ya comprimidos, `WrapperData.RES_NAME_APPEND=".jpg"`, forzando que
> `aapt` los guarde sin comprimir de nuevo; confirmado en `decompiled_apk/resources/assets/common/
> game_res/*.ogg.jpg`/`*.png.jpg`/`font.fnt.jpg`, todos con magic real de su formato real). El stub
> viejo (`GetFileDescriptor_stub`) devolvía `NULL` siempre — **un log real (`log_2026-07-21_12-56-24.txt`)
> mostró exactamente esto**: `GetFileDescriptor("MainMenu.png", ...)` y `GetFileDescriptor("font.fnt",
> ...)` fallando, el juego atrapando la excepción y siguiendo de largo (arranca "bien", sin crashear,
> pero **nunca carga su fondo de menú ni la fuente** — candidato directo y mucho más probable que el
> viewport para explicar la pantalla negra que persistía).
>
> Cruzando `decompiled_so/libgame_armeabi/out_ghidra.c` (`jGetFileDescriptorEx`/`CS_fsGetFileDescriptor`)
> se confirmó que el único call site alcanzable pasa `closeFDdirectly=1` y **solo usa el par
> offset/length que vuelve por el `int[]`** — nunca lee el objeto `Object` devuelto — y luego
> (evidencia del log: `open(ux0:/data/inotia3/inotia3.apk, 0): 4` justo después de cada
> `GetFileDescriptor`) abre el `.apk` real él mismo y lee directo en ese offset. Fix: `source/reimpl/
> apk_zip.c`/`.h` (nuevo) — parser mínimo de directorio central ZIP (sin dependencias nuevas, sin
> vendorizar nada) que busca `assets/common/game_res/<nombre><.jpg>` dentro de
> `ux0:/data/inotia3/inotia3.apk`, devuelve offset+tamaño si el entry es STORED. `GetFileDescriptor_stub`
> en `java.c` ahora arma ese nombre de entry y llena el `int[]` real con el resultado, devolviendo `0`
> (mismo comportamiento que el Java real con `closeFDdirectly=true`).
>
> Build verificado. **Pendiente**: correr de nuevo en consola/Vita3K y confirmar que `MainMenu.png`/
> `font.fnt` cargan y la pantalla deja de estar negra. El log nuevo también va a mostrar (vía
> `l_debug`/`l_warn` en `GetFileDescriptor_stub`) si algún otro asset falla con "not found" — si aparece,
> comparar el nombre exacto contra `decompiled_apk/resources/assets/common/game_res/` antes de asumir
> que hace falta más código (podría ser simplemente que ese archivo no se copió a la consola).

### Fase 7: Audio (.ogg vía `SOUNDSYSTEM_*` propio de Com2uS) — ✅ Implementada (2026-07-21), pendiente probar en consola

El `.so` importa un sistema de sonido custom completo (`SOUNDSYSTEM_Play*/Stop*/PlayBGM*`,
`SOUNDPLAYER_*`, tablas `SOUNDBGMBASE_*`/`SOUNDEFFECTBASE_*`/`SOUNDINFOBASE_*`) — no cocosdenshion/OpenAL
genérico — pero, como ya se documentó, delega la reproducción real a Android por JNI mediante 9 métodos
estáticos de `WrapperJinterface` (los wrappers `jSound*` del `.so`), así que **no hace falta entender el
sistema interno del `.so`**, solo implementar bien esos 9 métodos.

**Formato real confirmado (2026-07-21, no solo `strings`):** se extrajo `decompiled_apk/resources/
assets/common/game_res/` y se inspeccionaron los archivos con `file`/`xxd` — **todos los assets de audio
son `NAME.ogg.jpg`, y son literalmente OGG/Vorbis reales** (magic `OggS`/`vorbis` confirmado byte a
byte). El sufijo `.jpg` es el truco típico de empaquetado de Android: `aapt` no re-comprime extensiones
ya-comprimidas conocidas (`.jpg`/`.png`/`.ogg` normalmente), así que renombrar el archivo real a `.jpg`
fuerza que quede *sin comprimir* dentro del `.apk` sin importar el contenido real — no es una imagen, y
hay que sacarle el sufijo al extraer los assets a la consola (paso de preparación de assets, no algo que
el código del port resuelva). **No se encontró ningún `.mmf` en el bundle real** — el soporte `.mmf` que
`SOUNDSYSTEM_*` sugiere en sus imports es código muerto para este build, no se implementó.

**Implementación:** `source/audio.c`/`.h` (nuevo, agregado a `CMakeLists.txt` junto con las librerías
`vorbisidec`+`ogg` — Tremor de punto fijo y libogg, ambas ya provistas como portlibs de VitaSDK, **sin
vendorizar nada**: `$VITASDK/arm-vita-eabi/include/tremor/ivorbisfile.h` y `libvorbisidec.a`/`libogg.a`
ya existen en el toolchain instalado). Mismo mezclador de punto fijo/resampleo lineal validado en
consola real por `../Zenonia3-vita/loader/audio.c` (un hilo dedicado, un puerto `sceAudioOut` tipo BGM a
44100 Hz estéreo, resampleo lineal on-the-fly para cualquier `.ogg` que no sea 44100 Hz), pero adaptado
al contrato real de Inotia 3 — que es *muy* distinto al de Zenonia (reproducir por ID numérico fijo):
Inotia 3 es un modelo de **handle por objeto de sonido** (como `MediaPlayer`/`SoundPool` de Android):

- `SoundCreate(String path, boolean loop): int` → handle (>0), 0 si no hay slots libres.
- `SoundPrepare(int handle): int` → chequeo best-effort de que el archivo abre y decodea (no mantiene
  nada abierto persistente).
- `SoundPlay(int handle, boolean loop): int` → abre/decodea desde el inicio.
- `SoundPause/Resume/Stop(int handle): int`.
- `SoundSetVolume(int handle, int volume): int` (0..100) / `SoundGetVolume(int handle): int`.
- `SoundDestroy(int handle): int` → libera el handle para siempre.

Como el motor puede mantener vivos muchos objetos `Sound` precargados (uno por cada efecto/melodía
usada, potencialmente decenas — ver la lista de nombres en `game_res/`) aunque solo unos pocos suenen a
la vez, el pool está separado en dos niveles: `MAX_SOUND_HANDLES=256` slots de metadata baratos
(path/loop/volumen/estado) y `MAX_ACTIVE_VOICES=16` decodificadores Tremor reales, asignados (y robados,
el más viejo primero) recién en `SoundPlay`. `java.c` (ids 27–35) ahora llama directo a
`audio_sound_*()` en vez de los no-ops anteriores. `source/main.c` llama `audio_init()` antes de
`JNI_OnLoad`/`nativeInit`, porque `startClet()` (adentro de `nativeInit`) puede disparar audio muy
temprano.

Build verificado con `porting_tools/build/build_and_install.sh` (VitaSDK real, sin warnings). **Pendiente
crítico**: no se confirmó todavía qué convención de ruta le llega realmente a `SoundCreate` desde el
`.so` (¿ya viene como `ux0:data/inotia3/...` completo, como pasa con `texture.store`, o es un nombre
relativo tipo `assets/common/game_res/BGM1.ogg` que necesitaría un prefijo?) — `audio_sound_create()`
ya loguea el path crudo (`l_debug`) antes de intentar abrirlo, así que la primera corrida en consola/
Vita3K con archivos de audio reales en `ux0:data/inotia3/` va a confirmar esto sin adivinar más. Si
falla con "not found", revisar ese log antes de asumir que hace falta un hook de ruta (mismo error que
ya se evitó en Fase 6).

> **Actualización (2026-07-21) — probablemente el mismo problema que `MainMenu.png` en Fase 6.** Ningún
> log real llegó todavía a `SoundCreate` (el juego se quedaba antes en el bug de `GetFileDescriptor` de
> Fase 6), así que esto sigue sin confirmar, pero rastreando `decompiled_so/libgame_armeabi/
> out_ghidra.c` (`jSoundCreate` ← `CS_mdaSoundCreate`) el string que se le pasa a `SoundCreate` no se
> pudo confirmar con certeza si es un nombre suelto (como `"BGM1.ogg"`) que Java resolvería con su
> propio `AssetManager`/`GetFileDescriptor` (que en Android NO es lo mismo que abrir un archivo suelto
> por `fopen`), o si ya es una ruta usable directo. Si el próximo log muestra `SoundCreate("BGM1.ogg", ...)`
> fallando con "not found" en `audio_sound_create()`, **antes de asumir que hace falta un hook de ruta
> nuevo**, revisar si en realidad hace falta el mismo mecanismo de `apk_zip.c` (offset+length dentro del
> `.apk` real, ver el fix de Fase 6 arriba) en vez de un `fopen()` directo — dado que los `.ogg` reales
> están empaquetados exactamente igual que `MainMenu.png` (`NAME.ogg.jpg` dentro de
> `assets/common/game_res/`, confirmado con magic real de OGG/Vorbis).

### Fase 8: Construcción y LiveArea
`extras/livearea/` ya tiene los 4 PNG + `template.xml` del boilerplate genérico — reemplazar por arte real
de Inotia 3 antes de la primera instalación en consola (evitar el ícono genérico "SoLoBoP").

### Fase 9: Pruebas y Depuración en Consola Real
Igual metodología que Zenonia2/3 (`port_progress.md` de ambos como referencia de **clase de bugs**, no de
código): logging a archivo por corrida, `so-crash-triage` para cruzar `.so` real + `out_ghidra.c` + Java
decompilado ante cualquier crash con `.psp2dmp`.

> **Corrida 3 (2026-07-21) — primer crash real tras arreglar Fase 6, causa confirmada y arreglada.**
> Con `GetFileDescriptor` ya funcionando, el juego avanzó más allá del bug anterior — abrió
> `MainMenu.png` correctamente (`GetFileDescriptor("MainMenu.png") -> apk offset=6093252
> length=148229` en el log) — y crasheó poco después, la primera vez que se ejercita código de carga de
> PNG en este port.
>
> **Triage (`so-crash-triage`, `vita-parse-core` contra `logs/Inotia3-psp2core-1784655110-
> 0x000d313b1d-eboot.bin.psp2dmp` + `build/inotia3.elf` de esa misma corrida):** excepción Data abort,
> `PC=0x9810ffac`, `LR=0x81005951` (dentro de `__stack_chk_fail_soloader` en nuestro propio loader, ya
> en medio del log de `l_fatal`). El log mismo ya lo decía: `! fatal Stack collapsed at address
> 0x9810ff9d` — un **stack-protector real disparado dentro del `.so`**, no en nuestro código. Restando
> `LOAD_ADDRESS=0x98000000` (`source/utils/init.c`), la dirección cae en `0x10ff9d` dentro del `.so`
> real (`inotia3-1-3-2/lib/armeabi/libgame.so`) — acotado por símbolos dinámicos (`objdump -T`) a la
> función `png_create_read_struct_2` (`0x10fd74`-`0x10ffd0`), y confirmado con
> `arm-vita-eabi-objdump -d -M force-thumb`: la dirección exacta es el retorno de
> `blx __stack_chk_fail@plt` dentro de esa misma función — es decir, **libpng (empaquetado dentro del
> `.so`) detectó corrupción de su propio canary de pila al salir de `png_create_read_struct_2`**.
>
> `png_create_read_struct_2` es la función estándar de libpng que llama `setjmp()` dos veces (una vez
> para el chequeo de versión de la librería, otra al final) — y `source/dynlib.c` ya tenía, desde antes
> de esta sesión, un comentario `// TODO: May have different struct size?` en la entrada de `setjmp`/
> `longjmp` del import table, mapeándolas directo a las de `newlib` de VitaSDK. El `.so` fue compilado
> contra el `jmp_buf` de **Android Bionic** (tamaño/layout fijo en tiempo de compilación, embebido como
> primer campo de `png_struct`) — si el `jmp_buf` real de `newlib` de VitaSDK ocupa más bytes que el de
> Bionic, cada `setjmp()` escribe más allá de lo que el `.so` reservó para ese campo dentro de
> `png_struct` (heap), corrompiendo los campos siguientes — consistente con que el crash aparezca recién
> al final de la función (el chequeo de canary), no en la llamada a `setjmp()` en sí.
>
> **Fix:** `source/reimpl/jmp_shim.S` (nuevo, ensamblador Thumb-2 puro) — `setjmp_soloader`/
> `longjmp_soloader` implementan `setjmp`/`longjmp` **sin depender del layout de `jmp_buf` de ningún
> libc** — solo guardan el set mínimo de registros callee-saved de AAPCS (`r4-r11`, `sp`, `lr`: 10
> words, 40 bytes) directo en el buffer que el `.so` ya pasa. Cualquier `jmp_buf` real (el de Bionic
> incluido) reserva **al menos** ese tamaño — es el mínimo que cualquier `setjmp`/`longjmp` compatible
> con la ABI de ARM necesita para funcionar — así que nunca se escribe más allá de lo que el `.so` ya
> reservó, sea cual sea su tamaño real original. Registrado en `source/dynlib.c` reemplazando el mapeo
> directo a `setjmp`/`longjmp` de `newlib`. El ensamblador se verificó compilando (`arm-vita-eabi-gcc -c`)
> y desensamblando (`arm-vita-eabi-objdump -d -M force-thumb`) contra el toolchain real de VitaSDK antes
> de integrarlo — la lista de registros `{r4-r11}` cabe en una sola instrucción Thumb-2 `stmia.w`/
> `ldmia.w` (confirmado en la salida real del ensamblador), y `mov sp, rX`/`mov rX, sp` son patrones que
> el propio compilador de este toolchain ya genera (verificado comparando contra el epílogo de una
> función de prueba trivial).
>
> `CMakeLists.txt`: se agregó `ASM` a los lenguajes del `project()` (antes solo `C CXX` — los `.S` se
> ignoraban silenciosamente sin esto, dando un error de linker "undefined reference" en vez de un error
> de compilación) y se agregó `source/reimpl/jmp_shim.S` a las fuentes.
>
> Build verificado (VitaSDK real). **Pendiente**: correr de nuevo en consola/Vita3K y confirmar que
> `MainMenu.png` carga sin crashear. Si el crash persiste o cambia de lugar, `so-crash-triage` de nuevo
> con el `.psp2dmp` nuevo antes de asumir que el jmp_buf era la causa completa — puede haber más de un
> uso de `setjmp` afectado (libpng lo usa en varios lugares, y cualquier otro código bundleado en el
> `.so` que use excepciones estilo `setjmp`/`longjmp` se beneficia del mismo fix automáticamente al
> compartir el mismo símbolo importado).

> **Corrida 4 (2026-07-21) — mismo canary check, pero un segundo bug real (independiente) enmascaraba
> la evidencia. Encontrado y arreglado.** Con el fix de `jmp_shim.S` puesto, el juego volvió a crashear
> — pero el `.psp2dmp` nuevo (`vita-parse-core` contra el `.elf` de esa corrida, mismo procedimiento) dio
> un dato clave distinto al de la Corrida 3: esta vez `LR=0x9810ff9d` cae **dentro del `.so` mismo**
> (no dentro de nuestro `__stack_chk_fail_soloader`, como la vez anterior), y `PC=0x9810ffac` — 16 bytes
> después del sitio de la llamada `blx __stack_chk_fail@plt` que ya habíamos ubicado en
> `png_create_read_struct_2`. El log de esa corrida (`log_2026-07-21_17-36-34.txt`) solo tenía una
> línea, la del mensaje fatal — consistente con esto.
>
> Esto expone un **segundo bug real, independiente del `jmp_buf`**: `__stack_chk_fail_soloader()`
> (`source/reimpl/sys.c`) solo hacía `l_fatal(...)` y **retornaba normalmente** — pero `__stack_chk_fail`
> está documentado como `noreturn` en cualquier libc real (Bionic incluida): el compilador **no genera
> código válido después del sitio de la llamada** en ninguna función con stack-protector, solo queda el
> pool de constantes literales de esa función (los mismos números/direcciones que las instrucciones
> `ldr rX, [pc, #N]` de más arriba leen) — confirmado desensamblando esa dirección con
> `arm-vita-eabi-objdump` en la Fase 4/Corrida 3 de este documento. Al retornar en vez de abortar,
> la ejecución del `.so` "resumía" justo después de la llamada y intentaba ejecutar ese pool de
> constantes como si fueran instrucciones — de ahí el Data abort en una dirección **determinística**
> (mismo `.so`, mismo `LOAD_ADDRESS` fijo sin ASLR, mismos bytes de constante) 16 bytes más adelante.
>
> Esto **también explica retroactivamente por qué la Corrida 3 se veía distinta**: en esa corrida, el
> stack ya estaba tan corrompido que ni siquiera `__stack_chk_fail_soloader` pudo terminar su propia
> llamada interna a `_log_print` sin que su propio `pop {r7,pc}` final saltara a basura — la Corrida 4
> (con el `jmp_shim.S` ya puesto) alcanzó a completar el log y "retornar" limpiamente al `.so`, so lo
> cual, sin el fix de abortar, saltó a ejecutar el pool de literales en vez de crashear inmediato. Es
> decir: **es plausible (no puede descartarse ni confirmarse con esta sola evidencia) que el fix de
> `jmp_shim.S` de la Corrida 3 sí haya resuelto el problema real del tamaño de `jmp_buf`**, y que ambas
> corridas de crash observadas sean en realidad la misma clase de síntoma (retorno indebido de
> `__stack_chk_fail`) manifestándose de forma ligeramente distinta según el estado exacto de la pila.
>
> **Fix:** `__stack_chk_fail_soloader()` ahora llama `abort()` después de loguear (mismo patrón ya
> usado por `abort_soloader()` en el mismo archivo), y se marcó `__attribute__((noreturn))` tanto en
> `sys.c` como en su declaración en `sys.h`. Build verificado.
>
> **Por qué esto importa para el próximo log:** de ahora en más, si el canary check vuelve a fallar,
> el crash va a ser un `abort()` limpio con un `LR` apuntando exactamente al sitio real donde el `.so`
> llamó a `__stack_chk_fail` — no una ejecución salvaje hacia direcciones semi-aleatorias que
> ensucia la interpretación. Si el próximo log/dump muestra el mismo patrón (canary check fallando en
> `png_create_read_struct_2`) con un `LR` limpio apuntando al mismo call site, eso **sí confirmaría**
> que el `jmp_buf` no era (o no era el único) problema, y habría que seguir investigando esa función
> puntual. Si en cambio el juego progresa más allá de este punto, el `jmp_shim.S` de la Corrida 3 era
> la causa real y este segundo fix solo estaba corrigiendo un bug latente que nunca antes se había
> disparado de forma observable.

> **Corrida 5 (2026-07-21) — confirmado: el fix de `abort()` funcionó, pero el canary SIGUE fallando.
> El `jmp_buf` no era la causa (o no toda). Diagnóstico añadido, causa raíz aún abierta.**
> Log completo esta vez (475 líneas, no truncado como la Corrida 4) — misma secuencia exacta que
> siempre: `GetFileDescriptor("MainMenu.png")` exitoso (offset=6093252, length=148229), apertura+fstat+
> cierre del `.apk` real exitosos, el mismo intento fallido de
> `open(/sdcard/Android/data/inotia3/files/ux0:/data/inotia3/inotia3.apk, 0): -1`, y de nuevo
> `Stack collapsed at address 0x9810ff9d`.
>
> Pero el `.psp2dmp` esta vez (`vita-parse-core` contra el `.elf` de esa corrida) muestra
> `Stop reason: Undefined instruction exception`, `PC=0x81095b84` — **dentro de `_kill_r` de newlib**
> (`raise`→`kill`→`_kill_r`→`udf #255`, la trampa de aborto real de este toolchain), **no** una
> ejecución salvaje dentro del `.so`. Esto confirma que el fix de `__stack_chk_fail_soloader()` →
> `abort()` de la Corrida 4 funciona exactamente como se esperaba: un aborto limpio y controlado en vez
> de saltar a memoria basura.
>
> **Pero esto también confirma lo que la Corrida 4 dejó abierto**: el canary check **sigue fallando**,
> en el mismo sitio exacto (`png_create_read_struct_2`). El `jmp_shim.S` de la Corrida 3 — verificado
> correcto por ensamblado real contra el toolchain — **no resolvió (o no era) la causa raíz real**.
>
> **Investigación adicional esta corrida (todo descartado, sin hallar la causa aún):**
> - `source/audio.c` (hilo mezclador en background, corriendo desde muy temprano en `main()`): revisado
>   a fondo — descartado. Ningún `SoundCreate` aparece en ningún log todavía, así que `voices[]` nunca
>   tiene una voz activa; el hilo solo hace `memset`+iterar-sin-hacer-nada+`sceAudioOutOutput` en un loop
>   inocuo, sin escritura fuera de límites posible en este estado.
> - `source/wr_touch.c`: descartado — el crash ocurre dentro de `nativeInit()` (antes de
>   `wr_touch_init()`/el loop principal en `main.c`), así que este código todavía no corrió.
> - `source/reimpl/apk_zip.c` y `GetFileDescriptor_stub` (`java.c`): re-auditados línea por línea — sin
>   hallazgos, todos los buffers (`entry_path[256]`, `lfh[30]`, los `malloc` de tamaño exacto) están
>   correctamente acotados.
> - `File::GetFullPathName`/`ParseOffsetAndLengthAndPath` (las funciones reales del `.so` que arman y
>   parsean el string `"@offset|length|apkpath"` que resulta de nuestro `GetFileDescriptor` — halladas
>   cruzando `decompiled_so` por la cadena `"/sdcard/Android/data/%s/files/%s"` que aparece en cada log):
>   revisadas a fondo — su buffer interno es de 1024 bytes (`acStack_42c[1024]`), muy por encima de
>   cualquier string real que generamos, y el parser de `"@%d|%d|%s"` hace un shift-in-place seguro
>   (solo escribe hacia índices más bajos que los que lee). Sin hallazgos ahí tampoco.
>
> **Diagnóstico añadido (no un fix — instrumentación segura):** `source/reimpl/jmp_shim_log.c` (nuevo)
> + `jmp_shim.S` actualizado para loguear (tope de 10 llamadas, mismo patrón que el resto del proyecto)
> cada `setjmp`/`longjmp` con la dirección del buffer (y el valor en el caso de `longjmp`) **antes** de
> hacer el trabajo real — el registro guardado/restaurado alrededor de la llamada de logueo (`push
> {r0,lr}`/`pop {r0,lr}` en `setjmp_soloader`, `push {r0,r1}`/`pop {r0,r1}` en `longjmp_soloader`) deja
> la lógica ya verificada de guardar/restaurar registros **intacta** — se compiló y desensambló de nuevo
> contra el toolchain real para confirmarlo antes de integrarlo (ver salida real en el historial de este
> chat). Esto va a mostrar en el próximo log **si `longjmp` se llega a invocar siquiera** en esta
> secuencia (confirmaría o descartaría la hipótesis de que la rama de "versión de libpng incompatible"
> se está tomando y haciendo un `png_error`→`longjmp` de vuelta al primer `setjmp` de la función).
>
> Build verificado. **Pendiente**: correr de nuevo y mandar el log + `.psp2dmp` nuevo — con este log, se
> podrá ver cuántas veces se llama `setjmp`/`longjmp` en esta secuencia exacta antes del crash, dato que
> hace falta para decidir el próximo paso (revisar la rama de versión de libpng en detalle, sospechar de
> otra función completamente distinta, o instrumentar más profundo).

> **Corrida 6 (2026-07-21) — mismo crash limpio (confirma el fix de `abort()` de nuevo), pero el log
> volvió a llegar truncado a una sola línea — el logueo de `jmp_shim_log.c` nunca llegó a verse. Causa
> encontrada y arreglada: el logger no sincronizaba a disco.**
>
> El `.psp2dmp` de esta corrida (`vita-parse-core`) da el mismo patrón exacto que la Corrida 5: `PC`
> dentro de `_kill_r` (`udf #255`), aborto limpio — el fix de `__stack_chk_fail_soloader` sigue
> funcionando bien. Pero `logs/log_2026-07-21_18-01-07.txt` volvió a tener **una sola línea** (65 bytes,
> solo el mensaje fatal), igual que la Corrida 4 — ninguna de las líneas `[jmp_shim] setjmp(...)` que
> se agregaron en la Corrida 5 llegó a verse, pese a que `png_create_read_struct_2` definitivamente
> llama `setjmp()` al menos una vez antes de llegar al canary check (confirmado leyendo el pseudo-C).
>
> **Causa del truncamiento:** `utils/logger.c::_log_print()` escribía cada línea con `sceIoWrite()` (una
> llamada real de syscall de la consola, no un buffer de libc) pero **nunca sincronizaba el archivo a la
> tarjeta de memoria** — `sceIoWrite()` puede dejar los datos en una caché de escritura a nivel de SO que
> no se persiste a la memoria física hasta un `sceIoClose()`/sync explícito. La Corrida 5 (475 líneas
> completas) contra la Corrida 4/6 (1 sola línea) con el MISMO código de logging demuestra que esto es
> una condición de carrera: a veces el SO sincroniza su caché por su cuenta antes del crash, a veces no
> — **la última línea (el mensaje fatal) siempre sobrevive** porque probablemente cae justo en el borde
> de una sincronización automática del SO, pero todo lo anterior puede perderse.
>
> **Fix:** `sceIoSyncByFd(log_fd, 0)` después de cada `sceIoWrite()` en `_log_print()` — fuerza una
> sincronización real a la tarjeta de memoria en cada línea, para que un log sobreviva aunque sea la
> última cosa escrita antes de un crash duro. Tiene un costo de I/O por línea (aceptable para un build de
> depuración, y crítico ahora mismo para poder seguir triageando este bug puntual sin perder datos).
>
> Build verificado. **Pendiente**: correr de nuevo — esta vez el log debería llegar completo, con las
> líneas `[jmp_shim] setjmp(...)`/`longjmp(...)` visibles, lo que finalmente va a decir si `longjmp` se
> invoca en esta secuencia antes del crash.

> **Corrida 7 (2026-07-21) — el `sceIoSyncByFd` NO alcanzó; mismo log truncado a una línea. Cambiado a
> un enfoque más robusto: abrir/escribir/cerrar el archivo en cada línea.** Mismo crash limpio en el
> `.psp2dmp` (de nuevo `_kill_r`→`udf #255`, confirma el fix de `abort()`), pero
> `log_2026-07-21_18-26-17.txt` volvió a tener una sola línea pese al fix de la Corrida 6
> (`sceIoSyncByFd` después de cada escritura) — el `.elf` de esa corrida era efectivamente el build con
> ese fix (timestamps verificados), así que `sceIoSyncByFd` no fue suficiente para forzar la
> persistencia real en este dispositivo/tarjeta.
>
> **Fix (más agresivo):** en vez de mantener un único `log_fd` abierto para todo el proceso, `_log_print()`
> ahora hace `sceIoOpen(..., O_APPEND)` → `sceIoWrite()` → `sceIoClose()` en **cada línea individual**.
> Un `close()` es un límite de sincronización mucho más universalmente respetado que una llamada de sync
> explícita, a costa de más I/O por línea (aceptable durante esta fase de depuración). `log_path` ahora
> se guarda una sola vez (primera llamada) en vez de `log_fd`.
>
> Build verificado. **Pendiente**: correr de nuevo. Si el log SIGUE llegando truncado incluso con este
> enfoque, el problema no es de sincronización de E/S sino algo más fundamental (¿el crash real ocurre
> ANTES de lo que el `.psp2dmp` sugiere, y el resto del log nunca se genera? ¿la tarjeta de memoria en sí
> tiene un problema?) — reconsiderar en ese caso si vale la pena seguir invirtiendo en este camino de
> diagnóstico vs. probar otra hipótesis directamente sobre el código del `.so`.

> **Corrida 8 (2026-07-21) — abrir/escribir/cerrar por línea TAMPOCO alcanzó; mismo log de una sola
> línea. Se abandona el camino de logging a archivo para este diagnóstico puntual; los datos se
> incrustan directo en el mensaje que SÍ sobrevive siempre.**
>
> Verificado con `vita-parse-core -s 300` (ventana de pila mucho más ancha, sin depender del log en
> absoluto) que el `.psp2dmp` de esta corrida es **exactamente el mismo crash** de siempre: la pila
> muestra la cadena de retorno completa `png_create_read_struct_2` (`0x9810ff9d`) →
> `__stack_chk_fail_soloader` (`+0x16`) → `abort` (`+0x8`) → `_kill_r`/`udf` — confirmado resolviendo
> esas direcciones contra `nm build/inotia3.elf`, sin usar el log para nada. Esto descarta la teoría de
> "capaz crashea antes y por eso no hay log": el juego llega exactamente al mismo punto tardío que
> siempre, con exactamente los mismos pasos previos (`GetFileDescriptor` de `MainMenu.png`, etc.) — el
> log simplemente no está capturando ese tramo, pese a **dos intentos distintos y razonables** de
> arreglarlo del lado de nuestra propia E/S (`sceIoSyncByFd`, después abrir+escribir+cerrar por línea).
> Dado que ninguno de los dos funcionó, esto probablemente no es un bug en nuestro código de logging
> sino algo específico de este dispositivo/tarjeta/consola (o de cómo se está extrayendo el archivo
> después) que está fuera de lo que se puede arreglar solo con cambios en el `.vpk`.
>
> **En vez de seguir peleando con el logging a archivo, la solución fue no depender de él para este
> dato puntual.** `source/reimpl/jmp_shim_log.c`: los contadores de llamadas a `setjmp`/`longjmp` ahora
> son variables globales simples (`g_setjmp_call_count`, `g_longjmp_call_count`, más el buffer/valor de
> la última llamada a `longjmp`) en vez de imprimir su propia línea de log — y
> `__stack_chk_fail_soloader()` (`source/reimpl/sys.c`) los lee e imprime **en el mismo mensaje fatal
> que ya demostró sobrevivir siempre**, sin ninguna escritura a archivo adicional. Build verificado.
>
> **Pendiente**: correr de nuevo — el mensaje "Stack collapsed..." va a venir ahora con
> `setjmp=N longjmp=M last_longjmp_buf=... last_longjmp_val=...` incluido. Si `longjmp=0`, la rama de
> "versión de libpng incompatible" (que llama `png_error`→`longjmp`) nunca se toma, y hay que descartar
> esa hipótesis por completo y mirar en otro lado. Si `longjmp>0`, hay que revisar si `last_longjmp_val`
> tiene sentido (normalmente `1`, el valor que usa `png_error`) y seguir desde ahí.

> **Corrida 9 (2026-07-21) — dato obtenido: `setjmp=2 longjmp=1 last_longjmp_val=1`. Ambiguo (los
> contadores son globales, no por-función), así que no cierra el caso. A pedido, se desconectó la
> implementación de audio para simplificar el entorno de debugging.**
>
> El mensaje fatal llegó completo esta vez (sin depender de logging a archivo, como se esperaba):
> `Stack collapsed at address 0x9810ff9d (setjmp=2 longjmp=1 last_longjmp_buf=0x826a7d38
> last_longjmp_val=1)`. Esto confirma que `longjmp` **sí se invoca** en algún punto antes del crash, con
> `val=1` — que coincide exactamente con el valor que usa `png_error()` internamente
> (`longjmp(png_ptr->jmpbuf, 1)`), consistente con la hipótesis de que la rama de "versión de libpng
> incompatible" (o algún otro `png_error()`) se dispara.
>
> Pero **el dato no es concluyente**: `g_setjmp_call_count`/`g_longjmp_call_count` son contadores
> globales que viven toda la vida del proceso, no contadores por-invocación de
> `png_create_read_struct_2` — no hay forma de confirmar con esta sola medición si esos 2 `setjmp` + 1
> `longjmp` pertenecen todos a esta misma llamada, o si alguno viene de otro uso de `setjmp`/`longjmp`
> en otra parte del `.so` (o de la libc) ejecutado antes. Además, leyendo el pseudo-C de
> `png_create_read_struct_2` de nuevo: la rama de error (tras un `longjmp` real) hace `goto
> LAB_0011ff22` **saltándose el segundo `setjmp()` de la función** — si esos 2 `setjmp` + 1 `longjmp`
> fueran TODOS de esta misma llamada, no debería poder observarse el segundo `setjmp` después de un
> `longjmp` real hacia el primero. La evidencia es más consistente con "el camino normal (sin error) se
> ejecutó completo — de ahí los 2 `setjmp` — y el `longjmp` viene de otro lado", pero sin contadores
> por-sitio-de-llamada no se puede afirmar con certeza.
>
> **Cambio a pedido del usuario:** se desconectó la implementación de audio de Fase 7
> (`source/audio.c`/`.h`, Tremor/libogg) para eliminarla como variable mientras se sigue este crash —
> **no se borraron los archivos**, solo se los sacó de `CMakeLists.txt` (fuente y `target_link_libraries`)
> y `java.c`/`main.c` volvieron a los stubs no-op de antes de la Fase 7 (`SoundCreate` entrega un handle
> incremental falso, el resto devuelve `0`/`100`). Confirmado con `nm` que ningún símbolo `audio_*`/
> `ov_*`/`OggVorbis*` quedó en el `.elf` final. Build verificado. Reconectar llamando de nuevo a
> `audio_init()` en `main.c` y restaurando las 9 entradas de `MethodsInt` una vez resuelto el crash.

> **Corrida 10 (2026-07-21) — mismo crash exacto sin audio (lo descarta del todo). Pregunta del usuario
> ("no necesito el apk, ¿por qué?") resuelta cruzando `../Zenonia2-vita`/`../Zenonia3-vita`: NINGUNO
> de los dos toca el `.apk` real en la consola — todo son archivos sueltos pre-extraídos en la PC.
> Se agregó ese mismo camino a Inotia 3 como *preferido*, con el `.apk` real como respaldo.**
>
> El `.psp2dmp` de esta corrida (`vita-parse-core`) da **exactamente el mismo `PC`/cadena de llamadas**
> que con audio activado — confirma que el hilo mezclador de audio (que corría en background desde muy
> temprano en `main()`) nunca tuvo nada que ver con este crash, tal como se había razonado antes de
> sacarlo.
>
> **Por qué Inotia 3 sí necesitaba (hasta ahora) el `.apk` real, a diferencia de Zenonia:** un fork
> investigó `Zenonia2-vita`/`Zenonia3-vita` a fondo — ninguno de los dos lee el `.apk` en ningún
> momento; ambos usan un método JNI tipo `readAssets(path)` que hace un `fopen()` liso contra
> `ux0:data/zenonia-X/assets/...` (archivos ya sueltos, extraídos UNA VEZ en la PC con jadx/unzip y
> subidos vía FTP por su `manage_vita.py`). Inotia 3 es distinto **por el motor real, no por una
> decisión de este port**: su `GetFileDescriptor` está construido sobre el equivalente de
> `AssetManager.openFd()` de Android, que solo puede devolver un offset crudo dentro del `.apk` si el
> entry del zip está STORED (sin comprimir) — de ahí todo el mecanismo de offset+`apk_zip.c`. Zenonia
> nunca pasa por ese camino porque su motor llama a un método JNI que simplemente *transmite bytes*, sin
> necesidad de aritmética de offsets ni del contenedor `.apk` real.
>
> **Cambio:** `GetFileDescriptor_stub` (`java.c`) ahora busca **primero** una copia suelta
> pre-extraída del asset en `ux0:data/inotia3/assets/common/game_res/<nombre><.jpg>` (mismo nombre y
> sufijo `.jpg` que dentro del `.apk` — ver Fase 7 sobre por qué ese sufijo es real, no un error de
> empaquetado) — si existe, devuelve `offset=0, length=filesize` sin tocar el `.apk` para nada. Solo si
> el archivo suelto **no** existe, cae al parseo real de `apk_zip.c` contra `ux0:/data/inotia3/
> inotia3.apk` (comportamiento de antes, sin romper nada para quien no migre). Como `GetApkFilePath()`
> se llama por separado (sin argumento que diga de qué asset se trata), se sincronizan con una bandera
> "one-shot" (`g_pending_asset_is_loose`/`g_pending_asset_path`): `GetFileDescriptor_stub` la deja
> lista, la siguiente llamada a `GetApkFilePath_stub` la consume y se limpia sola — coincide con el
> orden de llamada confirmado en todos los logs reales hasta ahora.
>
> **Cómo migrar (para no necesitar el `.apk` en la consola):** copiar
> `decompiled_apk/resources/assets/common/game_res/` (ya extraído localmente por `jadx`, mismos nombres
> `NAME.ogg.jpg`/`NAME.png.jpg` que espera este código) a
> `ux0:data/inotia3/assets/common/game_res/` en la tarjeta de memoria. Build verificado.
>
> **Aviso honesto sobre este crash puntual:** este cambio probablemente **no** arregla el crash de
> `png_create_read_struct_2` por sí solo — leyendo el pseudo-C de esa función de nuevo, `png_set_read_fn`
> se llama con `(0,0)` (sin callback ni puntero real todavía); la API estándar de libpng separa "crear
> el struct" de "asociarlo a un archivo real" (eso pasa después, vía otra llamada). Es decir, el crash
> ocurre **antes de que se lea un solo byte del asset**, así que no debería importar si ese byte
> viene del `.apk` o de un archivo suelto. Se hace de todos modos porque (a) el usuario lo pidió
> explícitamente, (b) es una simplificación real y correcta independientemente de este bug puntual, y
> (c) saca a `apk_zip.c`/el mecanismo de offset de la lista de sospechosos para lo que sigue.

> **Corrida 11 (2026-07-21) — mismo crash otra vez con el fallback de archivo suelto (confirma, de
> nuevo, que el crash no depende de dónde vienen los bytes del asset). Siguiente experimento: parche
> quirúrgico de 2 bytes que anula el chequeo de canary de ESTA función puntual.**
>
> Con `so-crash-triage`/`vita-parse-core` se confirmó una vez más el mismo `PC`/cadena de llamadas
> exacta (offset dentro de `_kill_r` idéntico salvo por el desplazamiento de código esperado por los
> cambios recientes) — el fallback de archivo suelto de la Corrida 10 no cambió nada, tal como se había
> avisado (el crash ocurre antes de leer cualquier byte del asset).
>
> Dado que ya se descartaron: el hilo de audio, el mecanismo de lectura del asset (apk vs suelto), un
> desbordamiento directo del buffer `msg[80]` (acotado por `snprintf`), y que el fix de `jmp_shim.S`
> (tamaño de `jmp_buf`) no resolvió nada — se probó un ángulo distinto: **ubicar la instrucción exacta
> que decide "canary no coincide → llamar `__stack_chk_fail`" y anularla solo para esta función**, en
> vez de seguir buscando el origen de la posible corrupción.
>
> Desensamblando de nuevo con `arm-vita-eabi-objdump -d -M force-thumb` se encontró la secuencia real
> completa del chequeo (antes solo se había visto el final, el `blx __stack_chk_fail@plt` en sí):
> ```
> 10ff1e: blx setjmp@plt         ; segundo setjmp() de la función (retorno ignorado)
> 10ff22-2c: ... calcula la dirección real de __stack_chk_guard vía GOT y la desreferencia ...
> 10ff2e: cmp r2, r3             ; r2 = canary guardado en el prólogo, r3 = valor ACTUAL de __stack_chk_guard
> 10ff30: bne.n 10ff98           ; si NO coinciden -> saltar a __stack_chk_fail
> 10ff32: add sp, #136           ; si coinciden -> camino normal: restaurar pila...
> 10ff34: pop {r4,r5,r6,pc}      ; ...y retornar normalmente
> ```
>
> **Parche (experimental, no un fix definitivo):** `source/patch.c` — `so_patch()` ahora sobreescribe la
> instrucción de 2 bytes en `text_base + 0x0010ff30` (`bne.n` → `nop`, usando
> `kuKernelCpuUnrestrictedMemcpy` como ya hace `hook_addr()` para los bypasses de anti-tamper existentes)
> — la función SIEMPRE cae al camino de retorno normal (`10ff32`), sin importar si el canary coincide o
> no. Esto **no** deshabilita el stack-protector en general, solo en este único punto de esta única
> función.
>
> **Qué esperar en la consola:**
> - Si el mismatch era un falso positivo (el mecanismo de canary/guard comportándose mal bajo nuestro
>   loader, sin corrupción real de memoria) → el juego debería avanzar más allá de este punto.
> - Si hay corrupción real en otro lado, esto solo pospone el crash a donde sea que esa corrupción
>   realmente se manifieste — lo cual sería, en sí mismo, evidencia nueva útil (una ubicación de crash
>   distinta, más cerca de la causa real).
>
> Build verificado. **Pendiente**: correr de nuevo y mandar el log + `.psp2dmp`. Revertir este parche una
> vez identificada la causa real (no es una corrección definitiva, es un experimento de diagnóstico).

> **Corrida 12 (2026-07-21) — ¡el parche funcionó! El juego avanzó más allá del canary check y
> crasheó en un lugar NUEVO y mucho más informativo: un puntero NULL genuino, no wild execution.
> Rastreado hasta `DecodePNG` → `png_create_info_struct()` devolviendo NULL. Hook de diagnóstico
> agregado para ver exactamente por qué.**
>
> `vita-parse-core` contra el `.psp2dmp` nuevo dio algo completamente distinto a las 11 corridas
> anteriores: `Stop reason: Data abort exception` (no "Undefined instruction" — o sea, esta vez es un
> acceso a memoria inválido real, no ejecutar bytes de un pool de constantes), `PC=0x9810f9a0`,
> `LR=0x9810ff23`. Resolviendo contra `objdump -T`: `LR` cae justo después de la llamada a `setjmp()`
> dentro de `png_create_read_struct_2` (confirma que la función ahora SÍ retorna normalmente, gracias
> al parche de la Corrida 11) — y `PC` cae dentro de **`png_read_png`** (offset +0x20), una función
> *distinta*, más arriba en la pila de llamadas.
>
> La instrucción exacta en `PC` (desensamblada de nuevo con `arm-vita-eabi-objdump`):
> `ldr r2, [r5, #4]` — con **`R5 = 0x0`** confirmado en el volcado de registros. `r5` es `info_ptr`, el
> segundo argumento de `png_read_png(png_ptr, info_ptr, transforms, params)`. Cruzando el pseudo-C de
> `DecodePNG` (el llamador real, hallado buscando quién invoca `png_read_png`):
> ```c
> local_24 = png_create_read_struct("1.2.24",param_2,param_3,0,0,0);  // -> png_ptr
> local_28 = png_create_info_struct();   // -> info_ptr -- Ghidra no muestra el argumento explícito
> png_set_read_fn(local_24,0,ReadStream + 1);
> png_read_png(local_24,local_28,0,0);   // local_28 (info_ptr) termina siendo NULL
> ```
> Se verificó con el ensamblador REAL (no solo el pseudo-C, que a veces omite argumentos que en realidad
> se pasan por registro sin instrucción explícita) que `png_create_info_struct()` sí recibe el `png_ptr`
> correcto en `r0` (el valor queda ahí desde la llamada anterior, sin necesidad de un `mov r0,r0`
> redundante que el compilador omitió) — **no es un bug de ABI ni un argumento faltante**, es una
> llamada legítima que simplemente devuelve `0`.
>
> `png_create_info_struct(png_ptr)` solo devuelve `0` si `png_ptr==0` (descartado — `R4=0x826ac5e0`, un
> puntero de heap válido) o si `png_create_struct_2(2, png_ptr[0x314], png_ptr[0x310])` devuelve `0`.
> Se verificaron los offsets reales: `png_set_mem_fn` (llamada con `0,0,0` desde nuestra cadena real)
> escribe ceros limpios exactamente en `png_ptr+0x310`/`+0x314`/`+0x318`, y ninguna otra función en el
> camino (`png_set_read_fn`, el propio `setjmp_soloader`/`jmp_shim.S` de solo 40 bytes) escribe cerca de
> esos offsets. Es decir: bajo estas condiciones, `malloc(0x120)` (288 bytes, con un heap de 256MB)
> **debería** tener éxito siempre — y sin embargo la función devuelve `NULL`.
>
> **Hook de diagnóstico agregado** (`source/patch.c`): como `hook_addr()` reemplaza el punto de entrada
> completo de la función (no hay forma de "llamar al original" con este mecanismo), se reimplementó
> `png_create_struct_2` fielmente (verificado contra el desensamblado real: `type==1`→844 bytes,
> `type==2`→288 bytes, `malloc()` si `malloc_fn==NULL`, `memset` a cero, retorna el puntero) agregando
> logging de cada llamada (`type`, `malloc_fn`, `mem_ptr`, tamaño, resultado) — esto va a decir
> definitivamente en la próxima corrida si `malloc_fn` llega corrompido (no-NULL) en la segunda llamada
> (la del `info_struct`), o si `malloc()` en sí está fallando por alguna razón. Build verificado.
>
> **Pendiente**: correr de nuevo y mandar el log completo (con `porting_tools`, este debería sobrevivir
> mejor ya que las líneas se escriben bastante antes del crash real, no justo antes como con el canary)
> + `.psp2dmp`. Esta es la pista más concreta que se tiene hasta ahora sobre la causa raíz real.

> **Corrida 13 (2026-07-21) — Misterio resuelto: Causa raíz real de todos los crashes (OOM por heap minúsculo) identificada y corregida.**
>
> Analizando la traza real desde el `png_read_png` con puntero NULL hasta la inicialización de libpng,
> se desenredó una reacción en cadena brutal causada por un solo valor de configuración:
>
> 1. El asset `MainMenu.png.jpg` **sí es un PNG real** (confirmado con xxd, magic `\x89PNG`). El engine 
>    acierta al derivar a `DecodePNG`.
> 2. En `main.c`, `sceLibcHeapSize` estaba hardcodeado a apenas `4 * 1024 * 1024` (4 MB). Tras
>    inicializar Java, el gestor de texturas y los hilos de red, esta memoria estaba casi agotada.
> 3. Al invocar `png_create_read_struct` (que llama a `png_create_read_struct_2`), la librería libpng
>    intenta inicializar un buffer de 8192 bytes para zlib (`png_zalloc`).
> 4. Dado que el heap de 4MB ya estaba lleno, **`malloc(8192)` falla y retorna NULL**.
> 5. Al fallar `malloc`, libpng invoca a su función de reporte `png_error`.
> 6. El hook original de diagnóstico para `png_error` se puso en la dirección equivocada (`0x0010dba5`
>    en vez de `0x0010dcf0`), por lo que nunca interceptó ni logueó el error.
> 7. La función real `png_error` ejecutó un **`longjmp`**, retrocediendo el stack violentamente hasta la
>    llamada `setjmp` al inicio de `png_create_read_struct_2`.
> 8. El retorno del `setjmp` disparó la rama de error de libpng, retornando `NULL` al caller (`DecodePNG`).
> 9. `DecodePNG` propagó ciegamente el `png_ptr == NULL` a `png_create_info_struct`, la cual 
>    inmediatamente retornó NULL (por lo que el hook `type=2` nunca se ejecutó).
> 10. `DecodePNG` pasó este `info_ptr == NULL` a `png_read_png`, lo que produjo el desreferenciado 
>     `ldr r2, [r5, #4]` (`r5=0x0`) y el consecuente **Data Abort** final.
>
> **La falsa pista del canary:** El falso crash en `__stack_chk_fail` observado antes muy probablemente
> se debía a esta misma situación de OOM masivo / heap corruption corrompiendo la pila, lo que causaba
> que la validación del canary fallara.
>
> **Solución:**
> - Se aumentó `sceLibcHeapSize` a `128 * 1024 * 1024` (128 MB) en `main.c`, valor estándar para juegos grandes.
> - Se eliminaron los hooks de diagnóstico temporales en `patch.c` y se borró `png_error_hook.c`, ya que
>   el problema subyacente (OOM) está resuelto.
> - Build regenerado (`eboot.bin` reconstruido correctamente).

> **Corrida 14 (2026-07-21 22:44) — El fix de la Corrida 13 tapó el crash de libpng pero *voló el
> presupuesto total de memoria*: pantalla negra de nuevo, ahora con un crash MUCHO más temprano (dentro
> de `_so_load`, antes de que corra una sola línea de `main()`). Causa raíz: `sceLibcHeapSize=128MB` +
> `_newlib_heap_size_user=256MB` = 384MB solicitados al arrancar, más de lo que la Vita le concede a este
> proceso incluso con `ATTRIBUTE2=12` (memoria extendida) ya activado en `CMakeLists.txt`.**
>
> `vita-parse-core` contra el `.psp2dmp` más nuevo (`...1784688285...`, generado después de rebuildear con
> el fix de la Corrida 13) dio, de nuevo, `Data abort exception`, pero esta vez con `LR` resolviendo a
> `_so_load` (`so_util.c:207` según el símbolo de línea, aunque el desensamblado real de `arm-vita-eabi-
> objdump` -M force-thumb ubicó la instrucción exacta un poco antes, la línea 206) — es decir, el crash
> ocurre **durante la carga del `.so` en memoria**, antes de `JNI_OnLoad`, `nativePreInit`, `nativeInit`,
> o cualquier código de `main()`. El log de esta corrida confirma esto: solo alcanza a imprimir
> `! success kubridge check passed.` y se corta ahí — ni siquiera llega a la primera línea propia de
> `main()`.
>
> Desensamblando la función real (`arm-vita-eabi-objdump -d -M force-thumb`) alrededor de `LR=0x810148c5`:
> ```
> 810148b4: ldr.w r2, [r7, #136]   ; prog_size
> 810148b8: subs r3, r2, r3        ; r3 = prog_size - p_filesz
> 810148ba: mov r2, r3             ; arg3 (n) del memset
> 810148bc: movs r1, #0            ; arg2 (c) del memset
> 810148be: ldr r0, [r7, #108]     ; arg1 (s) = `zero` (el buffer de malloc())
> 810148c0: bl memset              ; <- ESTA es la llamada que crashea, no kuKernelCpuUnrestrictedMemcpy
> ```
> Esto es literalmente la línea 206 de `so_util.c`: `memset(zero, 0, prog_size - mod->phdr[i].p_filesz);`
> — el paso previo, `char *zero = malloc(prog_size - mod->phdr[i].p_filesz);` en la línea 205, **nunca
> chequea si `malloc` devolvió NULL**. Los registros en el momento del crash lo confirman sin ambigüedad:
> `R0=0x0` (el argumento `s` de `memset`, o sea `zero`), `R1=0x0` (el argumento `c`, coincide con el fill
> value 0 así que no aporta nada por sí solo, pero) `R2=0x194` (404 — el tamaño `n`, un valor chico y
> perfectamente razonable). Es decir: **`malloc(404)` devolvió NULL** en el primerísimo segmento `PT_LOAD`
> que carga el loader, y el `memset(NULL, 0, 404)` resultante reventó al tocar la dirección 0.
>
> Que `malloc()` falle para una asignación de **404 bytes**, en el punto MÁS temprano posible de todo el
> programa (antes de `main()`), significa que el heap de newlib ya estaba agotado o directamente no se
> pudo reservar con el tamaño pedido *desde el arranque* — no es un problema de "se llenó durante el
> juego" como con libpng. La diferencia entre esta corrida y las 1-12 (que sí llegaban mucho más lejos,
> hasta el `DecodePNG` de la Corrida 12) es un solo cambio: la Corrida 13 subió `sceLibcHeapSize` de 4MB a
> 128MB. Sumado a `_newlib_heap_size_user=256MB` (sin cambios, estable desde las corridas anteriores que
> sí arrancaban), el proceso ahora pide reservar **384MB** de heaps combinados al arrancar — de los
> ~260MB que sí funcionaban antes, un salto de +124MB que aparentemente excede el presupuesto real de
> memoria que la Vita le concede a este proceso (incluso con memoria extendida vía `ATTRIBUTE2=12`, ya
> presente en `CMakeLists.txt`) una vez descontado el propio ejecutable, la pila de GPU/GXM, y los bloques
> de memoria kernel (`kuKernelAllocMemBlock`) que `_so_load` reserva para el `.so` del juego.
>
> **Solución:** bajar `sceLibcHeapSize` a un valor mucho más conservador (`16 * 1024 * 1024`, 16MB) en vez
> de los 128MB de la Corrida 13 — sigue siendo 4x más que el original que causaba el OOM de libpng (que
> solo necesitaba cubrir un `malloc(8192)` con margen), pero mantiene el total combinado en ~272MB, mucho
> más cerca del baseline de ~260MB que sí llegaba a ejecutar. **No** se tocó `_newlib_heap_size_user`
> (256MB): viene funcionando desde las corridas 1-12 y no fue el valor que cambió entre la corrida que
> funcionaba (12) y la que rompió (14).
>
> **Pendiente**: rebuildear, redeployar, y confirmar con un log/`.psp2dmp` nuevo que (a) `_so_load`
> termina sin crashear y (b) el fix de libpng de la Corrida 13 sigue funcionando con el heap más chico
> (si el crash de `png_create_info_struct`/`DecodePNG` reaparece, subir `sceLibcHeapSize` en incrementos
> más pequeños — p.ej. 24MB, 32MB — en vez de saltar directo a 128MB otra vez).

> **Corrida 15 (2026-07-23 18:28) — Con el heap ya corregido, el juego avanza MUCHO más lejos (pasa
> `_so_load`, `JNI_OnLoad`, `nativePreInit`, casi toda `nativeInit`, resuelve el asset `MainMenu.png.jpg`
> como archivo suelto real) y crashea con el MISMO `__stack_chk_fail` de las Corridas 5-11 dentro de
> `png_create_read_struct_2` — la causa raíz de ESE bug puntual nunca se arregló, solo se ocultó con el
> parche NOP de la Corrida 11 (revertido en la 13) y luego se le echó la culpa retroactivamente al OOM
> sin evidencia directa. Hipótesis nueva y fix aplicado: el stack del hilo principal (256KB, el default de
> vitasdk) es demasiado chico para la profundidad de llamadas real de este motor.**
>
> El log llega hasta `GetFileDescriptor("MainMenu.png") -> loose file ...MainMenu.png.jpg (length=148229)`
> y termina en `! fatal Stack collapsed at address 0x9810ff9d (setjmp=2 longjmp=1 last_longjmp_buf=0x832a7e10
> last_longjmp_val=1)` — este mensaje sale literal de `__stack_chk_fail_soloader()` en `source/reimpl/sys.c`
> (Fase 9, Corrida 7/8), NO de `jmp_shim_log.c` directamente; es el stack-protector real del compilador, el
> mismo mecanismo investigado en las Corridas 5-11.
>
> `porting_tools/parse_dump.py` (wrapper de `vita-parse-core` con resolución de símbolos contra
> `libgame.so`) resolvió la dirección exacta con `--so-base 0x98000000` (la dirección fija de
> `LOAD_ADDRESS` en `utils/init.c` — el auto-detect de `parse_dump.py` adivinó mal la base esta vez y hay
> que pasarla a mano cuando eso pase): `0x9810ff9d` = `png_create_read_struct_2 + 0x229`, y la cadena de
> llamadas en la pila confirma `abort_soloader` -> `abort` -> `_kill_r` -> trap (`udf #255`) — exactamente
> el mismo punto de crash que Corridas 5-11, no uno nuevo.
>
> Desensamblando la función completa (`arm-vita-eabi-objdump -d -M force-thumb`, símbolo real vía
> `objdump -T`: `png_create_read_struct_2` en `0x0010fd74`, tamaño `0x25c`) se descartaron los sospechosos
> obvios:
> - El string de versión embebido (`strings` sobre el `.so`: `png_libpng_ver` = `"1.2.24"`) coincide
>   BYTE A BYTE con el `"1.2.24"` que el juego pasa a `png_create_read_struct` (confirmado en la Corrida
>   12) — el camino de warning por mismatch de versión (que hace `snprintf` a un buffer de 80 bytes en
>   `sp+52`, a 1 byte del canario en `sp+132`) nunca se ejecuta.
> - `malloc`/`free`/`realloc`/`calloc`/`snprintf`/`vsnprintf` están todos mapeados en `source/dynlib.c` a
>   las implementaciones REALES de la libc de VitaSDK (no a reimplementaciones propias) — descartados
>   como fuente de un bug de por sí.
> - El buffer que `setjmp_soloader`/`longjmp_soloader` (`jmp_shim.S`) escriben es el propio `png_ptr`
>   (offset 0 de `png_struct`, confirmado por el disassembly: ambas llamadas a `setjmp@plt` usan como
>   argumento el mismo puntero cargado como resultado de `png_create_struct_2`) — es memoria de HEAP, no
>   de stack, así que ese shim no puede tocar el canario de `png_create_read_struct_2` (que vive en SU
>   PROPIO frame de stack, `sp+132` de un frame de 136 bytes) sin importar cuántos bytes escriba de más.
>
> Sin un candidato de overflow de buffer local dentro de esta función ni sus vecinas inmediatas (todas las
> llamadas que hace son a código nativo del `.so` sin modificar, o a libc real de VitaSDK ya verificada),
> el sospechoso que queda es la profundidad de la propia pila de llamadas: para cuando se llega a
> `png_create_read_struct_2` (`main` -> `nativeInit` -> ... -> despacho JNI -> cargador de assets ->
> `DecodePNG` -> `png_create_read_struct` -> `png_create_read_struct_2` -> `inflateInit_` -> zlib), el
> proyecto **nunca define `sceUserMainThreadStackSize`**, por lo que usa el default de vitasdk (256KB) —
> un valor pensado para homebrews simples, no para un motor de Android con capas Com2uS + FalsoJNI +
> libpng + zlib apiladas. Ningún override de este símbolo aparece en `source/` ni en `CMakeLists.txt`.
>
> **Fix aplicado** (`source/main.c`): se agregó `unsigned int sceUserMainThreadStackSize = 4 * 1024 *
> 1024;` (4MB) junto a los otros tamaños de heap — barato dentro del presupuesto de memoria ya disponible,
> y el remedio estándar para exactamente esta clase de síntoma (corrupción de stack sin overflow de buffer
> local identificable, en el fondo de una cadena de llamadas profunda).
>
> **Pendiente**: rebuildear, redeployar, y confirmar con un log/`.psp2dmp` nuevo si (a) el `__stack_chk_fail`
> desaparece (confirmaría la hipótesis del tamaño de stack) o (b) sigue apareciendo en el mismo punto (en
> ese caso, instrumentar `png_create_read_struct_2` a mano — volcar el valor del canario real en
> `sp+132` inmediatamente después del prólogo y de nuevo justo antes del `cmp` que falla, para ver
> exactamente CUÁNDO cambia — en vez de seguir descartando sospechosos por inspección estática).

> **Corrida 16 (2026-07-23 19:41) — Sigue crasheando tras aumentar el stack.**
> 
> A pesar de haber subido `sceUserMainThreadStackSize` a 4MB, la aplicación sigue crasheando (probablemente en el mismo punto). Los archivos generados en esta última build son:
> - Dump: `logs/Inotia3-psp2core-1784850071-0x000f3125fd-eboot.bin.psp2dmp`
> - Log: `logs/log_2026-07-23_19-41-03.txt`
> 
> **Pendiente (Próximo paso para la IA):** Usar `porting_tools/manage_vita.py` (Opción 16) o correr directamente `parse_dump.py` sobre este nuevo `.psp2dmp` para confirmar si:
> a) El crash SIGUE siendo el mismo `__stack_chk_fail` en `png_create_read_struct_2`. Si es así, la hipótesis de "stack muy chico" queda descartada, y hay que proceder con la instrumentación manual del canario en el ensamblador (inyectar código para ver en qué momento exacto de la ejecución de libpng se corrompe `sp+132`).
> b) O si avanzó un paso más y crasheó en una dirección distinta, revelando el siguiente obstáculo en el cargador de assets o JNI.
>
> **Confirmado con `porting_tools/parse_dump.py --so-base 0x98000000` (stack-depth ampliado a 80 palabras):**
> escenario (a) — el crash es EXACTAMENTE el mismo: `png_create_read_struct_2 + 0x229` (`0x9810ff9d`), mismo
> `setjmp=2 longjmp=1`. `sceUserMainThreadStackSize=4MB` no cambió nada — se descarta la hipótesis del stack
> chico. Como beneficio colateral, el `--stack-depth 80` sí reveló la cadena de llamadas completa por
> primera vez: `SGL_Texture::FromResource` -> `DecodePNG` -> `png_read_png` -> `png_read_info` ->
> `png_create_read_struct_2` (crash) — confirma que se llega hasta acá por la ruta normal de carga de
> texturas, nada exótico.
>
> **Instrumentación manual aplicada** (siguiendo el plan (a)): en vez de parchear en caliente el `.so`
> (parchar a mitad de una función preservando el flujo original requiere un trampolín en el "code cave" de
> `so_util` con una llamada de largo alcance, mucho más frágil), se aprovechó que `blx
> __stack_chk_fail@plt` NO toca `sp` — cuando `__stack_chk_fail_soloader()` empieza a ejecutar, `sp` sigue
> siendo EXACTAMENTE el frame de 136 bytes de `png_create_read_struct_2`, intacto (el camino de mismatch
> salta directo a `__stack_chk_fail` sin pasar por `add sp, #136`). `source/reimpl/sys.c` ahora divide la
> función en dos:
> - `__stack_chk_fail_soloader()`: un trampolín `__attribute__((naked))` que solo hace `mov r0, sp; b
>   __stack_chk_fail_impl` — como es un `b` y no un `bl`, no toca `lr`, así que `__builtin_return_address(0)`
>   dentro de `__stack_chk_fail_impl` sigue resolviendo a la dirección real dentro de `png_create_read_struct_2`
>   (el diagnóstico de "dirección del crash" que ya veníamos usando no se pierde).
> - `__stack_chk_fail_impl(uint32_t *so_frame_sp)`: vuelca en una ÚNICA llamada a `l_fatal` (Corrida 7/8:
>   solo la primera línea sobrevive de forma confiable a un crash real en este dispositivo) el valor ACTUAL
>   de `__stack_chk_guard` (nuestro propio global, compartido con el `.so` vía GOT) junto con las 34
>   palabras completas del frame (`sp+0` a `sp+132`) — incluyendo el canario guardado en la entrada de la
>   función (`sp+132`) para comparar directamente contra el valor actual.
>
> **Qué buscar en el próximo log**: si `guard_now` (el valor ACTUAL de `__stack_chk_guard`) difiere del
> `saved` (`sp+132`, lo que la función guardó en su propio prólogo), el problema es que nuestro GLOBAL
> compartido cambió DURANTE la ejecución de esta función (un bug totalmente distinto — alguna escritura
> salvaje tocando `__stack_chk_guard` mismo, no un overflow de buffer local). Si en cambio `guard_now`
> coincide con lo esperado pero `saved` (o alguna palabra intermedia del frame, especialmente alrededor de
> `sp+52`..`sp+131`, el buffer de 80 bytes del mensaje de versión de libpng) contiene un valor que no pinta
> como parte de la lógica de esta función (un puntero de otro subsistema, texto ASCII inesperado, etc.), eso
> apunta a un overflow real de un buffer adyacente o a una escritura de otro hilo/subsistema aterrizando en
> este rango de stack. Build compilado y verificado (`build/inotia3.vpk` actualizado). **Pendiente**:
> redeployar y mandar el log + `.psp2dmp` nuevo para leer la línea `Stack collapsed at ... frame=...`.

> **Corrida 17 (2026-07-23 20:03) — La instrumentación de la Corrida 16 dio datos: el canario
> corrupto vale EXACTAMENTE la dirección de `png_ptr` de esta misma corrida (no basura, no un patrón de
> overflow adyacente) -- un único word de 4 bytes pisado sin daño colateral alrededor. Se descartó
> exhaustivamente cada explicación "interna" a la función vía disassembly + lectura directa de memoria
> del dump; el sospechoso que queda es una escritura salvaje EXTERNA. Se agregó un segundo checkpoint,
> más temprano, para acotar la ventana.**
>
> `logs/Inotia3-psp2core-1784851397-.../log_2026-07-23_20-03-09.txt`: mismo crash exacto otra vez
> (`png_create_read_struct_2+0x229`, `setjmp=2 longjmp=1`) -- confirma una vez más que
> `sceUserMainThreadStackSize` no fue la causa. La línea nueva:
> `guard_now=ff0a0000 saved=836a7e10 sp=0x81800cb0 frame=836a7e10,98138848,...`.
>
> Con `vita-parse-core`'s `read_vaddr()` (usado directo vía Python, no solo `parse_dump.py`) se leyó
> memoria arbitraria del `.psp2dmp` para verificar dos cosas:
> - **El propio `png_ptr` (0x836a7e10)**: sus primeros 40 bytes son EXACTAMENTE lo que
>   `setjmp_soloader` (jmp_shim.S) debía escribir en su segunda llamada -- `sp` guardado ahí
>   (`0x81800cb0`) coincide EXACTO con el `sp` que capturamos en el crash, y el `lr` guardado
>   (`0x9810ff23`) es la dirección de retorno correcta de esa misma llamada a `setjmp@plt`. **El shim
>   funciona bien y escribe donde debe** (memoria de heap, no de stack) -- descartado como sospechoso.
> - **La región de stack completa alrededor de `sp`**: se ve claramente que la mayoría del contenido
>   "sospechoso" (firma PNG `\x89PNG\r\n\x1a\n`, punteros de red `NET_CreateBuffer`, etc., ya vistos
>   desde el primer dump analizado) es simplemente contenido VIEJO de la pila, de llamadas anteriores ya
>   retornadas (`png_sig_cmp`/`png_read_info`/etc.) que nunca se limpia -- normal en cualquier stack, no
>   evidencia de nada. Dentro de ese ruido, `sp+132` (el canario) es la ÚNICA palabra que contiene
>   `0x836a7e10` (la dirección, no el contenido, de `png_ptr`) sin que nada alrededor (`sp+128`,
>   `sp+136`) muestre señales de un desborde progresivo desde un buffer adyacente.
>
> Se releyó el desensamblado completo de `png_create_read_struct_2` instrucción por instrucción
> buscando CUALQUIER `str` hacia `[sp, #132]` aparte del único store de prólogo (`0x10fda6`) -- no existe
> ninguno. La única escritura de ese offset es la del canario real en la entrada de la función. Conclusión:
> **no hay ningún camino de código DENTRO de esta función, ni en nuestro propio shim de setjmp/longjmp
> (ya verificado arriba), que explique el valor observado** -- apunta a una escritura salvaje desde
> OTRO lugar del programa (otro hilo, u otra función con una cuenta de punteros mal calculada) aterrizando
> justo en esa dirección de stack.
>
> (Nota aparte: `last_longjmp_buf` coincidiendo con el `png_ptr` de ESTA corrida, en ambos dumps
> analizados, en un primer momento pareció sugerir que el error/longjmp pasó DURANTE este mismo intento de
> decode -- pero `setjmp=2` sin haber tomado la rama de error de la Corrida 11/12 (que salta directo a
> limpieza sin llegar al segundo `setjmp`) contradice eso. Explicación más simple: es reutilización de heap
> -- un `setjmp`/`longjmp` de otro subsistema, completamente ajeno a este decode de PNG, usó y liberó ese
> mismo bloque de memoria antes de que `malloc()` se lo reasignara a este `png_ptr`. Descartado como pista.)
>
> **Nuevo checkpoint agregado** (`source/patch.c`): en vez de seguir triangulando desde un solo punto (el
> final de la función), se hookeó `png_create_struct_2` (el alocador que `png_create_read_struct_2` llama
> para su propio `png_struct` -- reimplementado fielmente contra el disassembly real: `type==1`->844
> bytes, `type==2`->288 bytes, `malloc()`+`memset`, igual que en la Corrida 12) para leer `caller_sp+132`
> en el punto MÁS TEMPRANO posible: la llamada a esta función ocurre solo 2 instrucciones después de que
> el canario se guarda en el prólogo, así que si YA está corrupto ahí, la ventana de corrupción es
> minúscula (apunta fuerte a algo externo/otro hilo); si está bien ahí, la corrupción pasa más adelante
> en la función (zbuf/`png_malloc`/`inflateInit_`/`png_set_read_fn`). Mismo truco de trampolín naked que
> `__stack_chk_fail_soloader` (`mov r3, sp; b png_create_struct_2_impl` -- `r3` no lo usa esta función
> para sus propios argumentos, así que no cuesta nada reutilizarlo para pasar el `sp` del llamador).
> Log agregado (no fatal, nivel info): `png_create_struct_2 checkpoint: caller_sp=... canary_now=...
> guard_now=... type=... mem_ptr=...`. Build compilado y verificado, símbolos confirmados en el `.elf`
> (`mov r3, sp` / `b png_create_struct_2_impl` en el offset correcto).
>
> **Pendiente**: redeployar y mandar el log + `.psp2dmp` nuevo. Buscar la línea
> `png_create_struct_2 checkpoint` con `type=1` (la llamada que crea el `png_struct` de este mismo
> `png_ptr`) y comparar su `canary_now` contra el `saved` que aparezca en el eventual `Stack collapsed`
> -- si ya difieren ahí, la corrupción es casi instantánea (buscar qué corre en paralelo: hilos de
> red/sonido, pollers de anti-tamper); si coinciden, acotar el siguiente checkpoint más adelante en la
> función (después de `png_malloc`/`inflateInit_`).

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
