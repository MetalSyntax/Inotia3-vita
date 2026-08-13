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
>
> **Corrida 18 (2026-07-24) — Respondida la pregunta pendiente de la Corrida 17: la corrupción NO es
> instantánea, pasa DENTRO de la ejecución de la función.** `logs/log_2026-07-23_20-38-57.txt` +
> `Inotia3-psp2core-1784871223-...psp2dmp`: mismo crash exacto una vez más (`png_create_read_struct_2+0x229`,
> `setjmp=2 longjmp=1`, `saved=836a7e10` == la dirección de `png_ptr` de esta corrida, igual que en todas
> las anteriores -- confirma que no hay ASLR y el heap se reparte igual en cada corrida idéntica). El
> checkpoint `type=1` de la Corrida 17 SÍ llegó esta vez:
> `canary_now=ff0a0000 guard_now=ff0a0000` (coinciden, correcto) justo después de la primera llamada a
> `png_create_struct_2` -- y el `saved` en el `Stack collapsed` final es `836a7e10` (corrupto). Mismo `sp`
> exacto (`0x81800cb0`) en ambos puntos, confirmado por lectura directa de memoria: es literalmente el
> mismo frame. **Conclusión: la corrupción ocurre en algún punto entre el primer `setjmp()` y el chequeo
> final del canario dentro de ESTA MISMA invocación de `png_create_read_struct_2`** -- no es una escritura
> externa instantánea (hilo de red/sonido corriendo en paralelo), es interna a la ejecución de la función
> o a algo que ella llama directamente.
>
> Se releyó el desensamblado completo (`arm-vita-eabi-objdump -M force-thumb`, `0x10fd74`-`0x10ffd0`) y se
> cruzó línea por línea contra el pseudo-C de Ghidra (`decompiled_so/libgame_armeabi/out_ghidra.c:192839`)
> para descartar el único otro candidato de escritura hacia el rango `sp+52`..`sp+132`: un buffer local de
> 80 bytes (`acStack_64`, para `snprintf` en la rama de "versión de libpng incompatible") que termina
> EXACTAMENTE en el offset del canario. Se confirmó que esa rama es efectivamente inalcanzable en la
> práctica: el juego pasa `"1.2.24"` a `png_create_read_struct`, y aunque la comparación byte-a-byte
> contra `png_libpng_ver` (el string de versión compilado en el `.so`) marcase alguna diferencia, el
> propio código tiene un atajo que salta directo a `LAB_0011fe80` (saltándose el `snprintf`/`png_warning`
> por completo) con solo que `param_1[0]=='1' && param_1[2]=='2'` -- ambos ciertos para `"1.2.24"` sin
> importar qué tan corrupto esté `png_libpng_ver`. Descartado con alta confianza.
>
> Confirmado también (releyendo el desensamblado instrucción por instrucción) que NINGUNA de las funciones
> llamadas entre el primer `setjmp` y el chequeo final (`png_set_mem_fn`, `png_set_error_fn`, `png_malloc`,
> `inflateInit_`, `png_set_read_fn`) recibe un puntero hacia ESTE stack frame como argumento -- todas
> reciben `png_ptr` (heap) y punteros a funciones/constantes globales, nunca una dirección de pila nuestra.
> Eso deja como única vía de instrumentación no invasiva el SEGUNDO `setjmp()` de la función (línea
> `png_ptr; setjmp(png_ptr)` justo antes del chequeo, `0x10ff1e`) -- que ya es código 100% nuestro
> (`jmp_shim.S`/`source/reimpl/sys.c`), así que no hace falta ningún hook invasivo sobre el `.so`.
>
> **Instrumentación agregada** (`source/reimpl/jmp_shim.S` + `source/reimpl/jmp_shim_log.c`):
> `setjmp_soloader`/`longjmp_soloader` ahora capturan el `sp` del LLAMADOR (`mov r1/r2, sp` antes de tocar
> la pila, ya que `bl`/`blx` no modifican `sp`) y se lo pasan a `jmp_shim_log_setjmp`/`_longjmp`, que
> loguean (nivel `l_info`, no fatal -- ya confirmado que estos sobreviven al log si ocurren ANTES del
> crash, igual que los checkpoints de `png_create_struct_2`) `caller_sp` y el valor actual en
> `caller_sp+132` (el offset del canario de `png_create_read_struct_2`, sin asumir que TODO llamador de
> `setjmp`/`longjmp` en el `.so` comparte ese layout -- solo es significativo cuando `caller_sp` coincide
> con el frame conocido `0x81800cb0`). Esto da checkpoints GRATIS en ambas llamadas a `setjmp()` dentro de
> `png_create_read_struct_2` sin tocar el `.so` para nada. Build compilado y verificado
> (`build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-07-24).
>
> **Pendiente**: redeployar (`porting_tools/manage_vita.py`), reproducir el mismo crash (cargar
> `MainMenu.png`), y mandar el log + `.psp2dmp` nuevo. Buscar las DOS líneas
> `setjmp_soloader checkpoint` con `caller_sp=0x81800cb0` (puede variar levemente si el heap se corre) --
> la primera (justo después de `png_malloc`) debería mostrar `canary_at_sp132` correcto (ya lo sabemos por
> el checkpoint de `png_create_struct_2`); si la SEGUNDA (justo antes del chequeo final, después de
> `png_set_read_fn`) ya muestra el valor corrupto, la ventana queda acotada a
> `png_malloc`/`inflateInit_`/`png_set_read_fn` -- haría falta entonces revisar `inflateInit_` (zlib, más
> complejo) y el propio `png_malloc` con más detalle, posiblemente instrumentando sus argumentos también.
>
> **Corrida 19 (2026-07-24) — ES UN BUG COMPLETAMENTE DISTINTO: `png_create_read_struct_2` termina LIMPIA.
> El "Stack collapsed" que veníamos persiguiendo desde la Corrida 5 es un SÍNTOMA corriente abajo, no la
> causa.** `logs/log_2026-07-24_02-40-24.txt` +
> `Inotia3-psp2core-1784875276-0x000110215b-eboot.bin.psp2dmp`: los checkpoints de `setjmp_soloader`
> (agregados en la Corrida 18) dieron el dato decisivo --
> ```
> png_create_struct_2 checkpoint: caller_sp=0x81800cb0 canary_now=ff0a0000 guard_now=ff0a0000 type=1
> setjmp_soloader checkpoint: call#=1 buf=0x836a7e10 caller_sp=0x81800cb0 canary_at_sp132=ff0a0000
> setjmp_soloader checkpoint: call#=2 buf=0x836a7e10 caller_sp=0x81800cb0 canary_at_sp132=ff0a0000
> png_create_struct_2 checkpoint: caller_sp=0x81800d58 canary_now=00000000 guard_now=ff0a0000 type=2
> longjmp_soloader checkpoint: call#=1 buf=0x836a7e10 val=1 caller_sp=0x81800cb0 canary_at_sp132=836a7e10
> ! fatal Stack collapsed at 0x9810ff9d (...) saved=836a7e10 sp=0x81800cb0
> ```
> **Los DOS `setjmp()` de `png_create_read_struct_2` corren con el canario intacto** (`canary_at_sp132`
> coincide con `guard_now` las dos veces) -- la función termina y retorna con éxito. Después, `type=2`
> (creación del `info_struct`, un `caller_sp` distinto y más "alto" -- confirma que es OTRA función, llamada
> DESPUÉS de que `png_create_read_struct_2` ya retornó). Y ENTONCES aparece un `longjmp_soloader` con
> `buf=0x836a7e10` -- el MISMO `png_ptr` -- pero con `caller_sp=0x81800cb0`: **exactamente la misma dirección
> de pila que ya usaba `png_create_read_struct_2`, una función que YA RETORNÓ**. En ese momento
> `canary_at_sp132` YA está corrupto (`836a7e10`) -- pero no importa si coincide o no, porque esa dirección
> de pila ya no le pertenece a nadie: es memoria reciclada por lo que sea que corrió después (la propia
> creación del `info_struct`, validación de cabecera, etc.).
>
> **La causa real**: releyendo `DecodePNG` (`decompiled_so/libgame_armeabi/out_ghidra.c:179667`) --
> ```c
> local_24 = png_create_read_struct("1.2.24",param_2,param_3,0,0,0);
> local_28 = png_create_info_struct();
> png_set_read_fn(local_24,0,ReadStream + 1);
> png_read_png(local_24,local_28,0,0);
> ```
> **`DecodePNG` NUNCA llama su propio `setjmp()`** -- el patrón correcto/estándar de libpng es que la
> APLICACIÓN llame `setjmp(png_jmpbuf(png_ptr))` ella misma DESPUÉS de crear los structs y ANTES de leer,
> para que cualquier `png_error()` durante la lectura vuelva a SU PROPIO frame (que sigue vivo). Acá eso no
> pasa: el único `setjmp()` registrado es el que `png_create_read_struct_2` hizo sobre SU PROPIO frame,
> justo antes de retornar -- confirmado también por el contador global (`call#=2` es el último `setjmp`
> antes del crash, ningún `call#=3`). Si `png_read_png()` dispara UN SOLO `png_error()` por CUALQUIER razón,
> el `longjmp` va a resolver a un frame que ya no existe -- comportamiento indefinido en CUALQUIER
> plataforma, no un bug de Vita/newlib. Confirmado además vía pseudo-C de `png_error` (`out_ghidra.c:190532`):
> llama al `error_fn` custom SI existe (pero de forma ADICIONAL, nunca en vez de) y SIEMPRE termina en
> `longjmp(png_ptr, 1)` -- coincide exactamente con lo observado.
>
> Esto en el APK real de Android nunca se manifiesta porque, para los assets que el juego shippea,
> `png_read_png()` nunca dispara `png_error()` (ninguna imagen real les da error) -- es un bug LATENTE en el
> juego original que nuestro port expone porque, por la razón que sea, EN NUESTRO ENTORNO sí se dispara un
> `png_error()` real decodificando `MainMenu.png.jpg` (confirmado real PNG 512x512 RGBA con perfil ICC
> embebido -- `pHYs`+`iCCP` -- vía `xxd`/`file`, no es un JPEG pese al nombre; ver `source/reimpl/apk_zip.c`
> y `source/audio.h` para el porqué del sufijo `.jpg` en assets que son otra cosa, ya documentado y no
> relacionado con este bug). **Falta el dato que dice CUÁL `png_error()` es.**
>
> **Instrumentación agregada** (`source/patch.c`): hook sobre `png_error` (offset `0x0010dcf1`, real, no de
> PLT -- confirmado con `objdump -T`) que loguea `(png_ptr, message)` y replica FIELMENTE el único efecto de
> control de flujo que YA sabemos que la función real siempre produce (`longjmp(png_ptr, 1)`,
> `source/reimpl/jmp_shim.h`) -- no cambia ningún comportamiento del juego, solo revela qué mensaje de error
> real dispara el `longjmp`. Nivel `l_fatal` (no `l_info`) a propósito: por Corrida 7/8, ante un crash real
> solo la PRIMERA línea fatal sobrevive de forma confiable, y este mensaje es más valioso que el ya
> conocido "Stack collapsed" de siempre. Build compilado y verificado
> (`build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-07-24 03:04).
>
> **Pendiente**: redeployar, reproducir, mandar el log nuevo. Buscar la línea
> `png_error(png_ptr=..., "...")` -- el mensaje exacto dice qué falla realmente (¿lectura del stream
> `ReadStream`? ¿un chunk `iCCP`/`pHYs` mal soportado? ¿un límite de memoria/`sceLibcHeapSize`?) y de ahí
> sale el fix real: lo más probable es que el fix no sea "arreglar libpng" sino agregar nuestro PROPIO
> `setjmp(png_jmpbuf(png_ptr))` en el punto donde `DecodePNG` debería haberlo puesto -- posiblemente
> envolviendo la llamada a `png_read_png` con nuestro propio `setjmp`/manejo de error vía un hook sobre
> `DecodePNG` o `SetImage4GL`, ya que no podemos tocar el código fuente del `.so` directamente.

> **Corrida 20 (2026-07-24) — El hook de la Corrida 19 dio el mensaje: `png_error(png_ptr=0x836a7e10,
> "Not a PNG file")`.** `logs/log_2026-07-24_03-26-51.txt` +
> `Inotia3-psp2core-1784878019-0x0000033f91-eboot.bin.psp2dmp`: el `png_error_hook` disparó, logueó el
> mensaje real, y replicó el `longjmp(png_ptr, 1)` fielmente -- pero como ya sabíamos desde la Corrida 19,
> ese `longjmp` resuelve a un frame ya retornado (`png_create_read_struct_2`), así que el crash de
> siempre (`_kill_r`/`abort`/`__stack_chk_fail_soloader`) vuelve a aparecer, ahora con una línea `fatal`
> extra ANTES que dice la causa real.
>
> `"Not a PNG file"` sale literalmente de `png_read_info` (`decompiled_so/libgame_armeabi/out_ghidra.c:
> 192450-192469`): lee 8 bytes vía `png_read_data`->`ReadStream` y los compara con `png_sig_cmp` contra la
> firma real de PNG (`89 50 4E 47 0D 0A 1A 0A`); si no coinciden, error. Esto contradice directamente lo ya
> confirmado en la Corrida 19 (`xxd`/`file` sobre el archivo real en disco: 512x512 RGBA con `pHYs`+`iCCP`,
> firma PNG genuina) -- **el archivo en disco es válido, así que la corrupción/desalineación pasa en algún
> punto de la cadena que lo lleva desde el disco hasta el buffer que `ReadStream` sirve**.
>
> Se rastreó esa cadena completa vía pseudo-C (sin asumir nada, cruzando cada eslabón):
> `SGL_Texture::FromResource` -> `DecodePNG(out_struct, buf, len)` (`out_ghidra.c:179667` -- `buf`/`len`
> son el 2do/3er argumento reales, confirmado en el call site de `SetImage4GL:178547` que pasa los MISMOS
> `param_2`/`param_3` que recibió) -> `png_create_read_struct("1.2.24", buf, len, 0,0,0)` (guarda `buf`/
> `len`/`pos=0` en 3 campos fijos de `png_ptr`, offsets `0x118`/`0x11c`/`0x120` -- confirmado porque
> `ReadStream` (`out_ghidra.c:179649`) lee exactamente esos 3 offsets de su `param_1` para hacer
> `memcpy` puro desde un buffer EN MEMORIA, sin tocar ningún file descriptor -- este PNG se decodifica
> 100% desde RAM, no hay short-read de disco posible en esta etapa). `buf`/`len` llegan de
> `SGL_Resource` (campos `+4`/`+8`), poblado por `SGL_Resource::FromBundle` -> `CS_knlGetResourceID` +
> `CS_knlAlloc` + `CS_knlGetResource`.
>
> `CS_knlGetResource` (`out_ghidra.c:157400`) resuelve el archivo real vía `MMappedFile::Open` ->
> `File::Open` -> `File::GetFullPathName`, la MISMA función ya instrumentada indirectamente por el JNI
> log (`GetFileDescriptor`+`GetApkFilePath`, modo `param_4==2`, formato `"@offset|length|path"`) --
> coincide con la secuencia real del log (`GetFileDescriptor("MainMenu.png") -> loose file .../
> MainMenu.png.jpg (length=148229)` seguido de `open(...): 4`, `fstat(4): 0`, `close(4): 0`) y confirma
> que el `fd`/tamaño resuelven correctamente. Pero **el `read()` real que llena el buffer entre el
> `fstat` y el `close` no está logueado en absoluto** -- `source/dynlib.c` mapea `"read"` directo a la
> `read()` real de newlib (sin wrapper, sin log), a diferencia de `open`/`fstat`/`close`/`stat` que sí
> pasan por `source/reimpl/io.c` con `l_debug`. Es el único eslabón de la cadena completa (JNI resolve ->
> `File::Open` -> `read()` -> `memcpy` en `ReadStream` -> `png_sig_cmp`) que sigue siendo una caja negra --
> no hay evidencia todavía de si el `read()` devuelve los bytes reales del archivo, un conteo parcial, o
> si el buffer que `png_create_read_struct` recibe como `buf` no es siquiera el que `read()` llenó (habría
> que descartar también un mal cómputo de puntero/alias en `CS_knlAlloc`/`CS_GETDPTR`, pero esos son
> internos al `.so`, no reimplementados por nosotros -- read() sí lo es).
>
> **Instrumentación agregada** (`source/reimpl/io.c`/`.h` + `source/dynlib.c`): nuevo `read_soloader()`
> que llama al `read()` real y loguea (`l_debug`, mismo estilo que `open_soloader`/`fstat_soloader`) el
> `fd`, los bytes pedidos, los bytes devueltos, y los primeros hasta 16 bytes leídos en hex -- comparables
> a ojo contra la firma PNG real (`89 50 4e 47 0d 0a 1a 0a`). `source/dynlib.c` ahora importa `"read"` ->
> `read_soloader` en vez del símbolo real directo (antes sin logging). Solo afecta las llamadas a `read()`
> que hace el `.so` vía su tabla de imports -- el `read()` interno de nuestro propio `source/reimpl/mem.c`
> (usado por el `mmap()` fake) sigue llamando directo a la función real de newlib, sin pasar por este
> wrapper (no hace falta instrumentarlo: el `mmap` fake no interviene en esta ruta de carga, que confirmado
> arriba nunca invoca `mmap`/`munmap` -- ninguna línea `mmap(...)` aparece en ningún log hasta ahora).
> Build compilado (cross real vía VitaSDK, `arm-vita-eabi-g++`, sin usar el script interactivo de
> `porting_tools/build/build_and_install.sh` porque su `read -p` de modo debug rompe en un shell no
> interactivo -- se replicaron sus mismos pasos de `rsync`+`cmake`+`make` a mano) y verificado
> (`build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-07-24).
>
> **Pendiente**: redeployar, reproducir, mandar el log nuevo. Buscar la línea `read(4, ...)` (el `fd` real
> puede variar levemente) justo antes del `png_error`/`Stack collapsed` de siempre -- si los primeros bytes
> logueados YA no son `89 50 4e 47 0d 0a 1a 0a`, la causa está confirmada entre el disco y el `read()`
> (permisos, ruta, tamaño de archivo real en el dispositivo vs. lo esperado, o un bug de I/O de
> `sceIoRead`/newlib en este VitaSDK con archivos de este tamaño/ubicación en `ux0:`); si SÍ son correctos
> ahí, la corrupción pasa DESPUÉS del `read()` -- en el manejo del puntero/alias dentro de `CS_knlAlloc`/
> `CS_GETDPTR`/`CS_knlGetResource` (código 100% del `.so`, requeriría un hook similar a
> `png_create_struct_2_hook` pero sobre esas funciones para volcar el puntero real que reciben vs. el que
> `png_create_read_struct` termina usando).

> **Corrida 21 (2026-07-24) — Causa real encontrada y arreglada: NO era corrupción de datos, era un
> `read()` que nunca se llegaba a hacer.** `logs/log_2026-07-24_23-01-50.txt` +
> `Inotia3-psp2core-1784948518-0x00006a321b-eboot.bin.psp2dmp`: el `read_soloader()` de la Corrida 20
> demostró trabajar perfectamente para OTROS archivos en el mismo log (`read(3, ..., 4): 4 -- first bytes:
> cc 00 00 00` para `Com2usProtection.sav`, `read(4, ..., 35): 35 -- ...` para `init.dat`) -- pero para
> `MainMenu.png.jpg` (`open(...): 4`, `fstat(4): 0`, `close(4): 0`) **no aparece NINGUNA línea `read(4,
> ...)` entre el `open` y el `close`**. El archivo nunca se lee -- ni corrompido, ni parcial: cero bytes.
>
> Rastreando hacia atrás en `out_ghidra.c` sin asumir nada: `SGL_Texture::FromResource` (`161450`) llama
> `DecodePNG(buf, buf_ptr, buf_len)` con `buf_ptr`/`buf_len` provistos por un `SGL_Resource` construido
> por `SGL_Resource::FromBundle` (`161132`) vía `CS_knlGetResourceID` + `CS_knlAlloc` + `CS_knlGetResource`
> (`157400`). `CS_knlGetResource` abre su archivo con
> `MMappedFile::Open(path=*(char**)(resourceInfo+0x104), offset, length, 1, 3)` -- el **`3` está
> hardcodeado en el propio `.so`**, sin ningún fallback a otro modo. Eso baja a
> `File::Open`->`File::GetFullPathName(name, out, size, mode=3)` (`174929`), cuyo modo 3 SIEMPRE construye
> `"/sdcard/Android/data/<pkg>/files/<name>"` -- **salvo que `name[0]=='/'`, en cuyo caso usa `name`
> literal** (`174946`).
>
> La pieza que cierra el caso: `jGetApkFilePath()` (`145992`) cachea su resultado la PRIMERA vez que se
> llama (`if (DAT_0017ded8=='\0') { resolver... } return DAT_0017ded8;`) y jamás vuelve a preguntarle a
> Java después -- tiene sentido en Android real, donde TODOS los assets viven en el mismo `.apk`, así que
> resolverlo una sola vez y reusar ese path para cualquier recurso es válido. Ese valor cacheado es
> exactamente el `name` que `CS_knlGetResource` usa como `path` para TODOS los recursos futuros. En Android
> real, `jGetApkFilePath()` devuelve la ruta real absoluta del `.apk` (`"/data/app/....apk"`, empieza con
> `/`), así que el modo 3 SIEMPRE toma el atajo "ya resuelto, usar literal" y termina mmapeando el `.apk`
> real al offset/length cacheados de cada recurso -- el modo 3 nunca construye de verdad la ruta
> `/sdcard/...` en la práctica.
>
> Nuestro `GetApkFilePath_stub` (`source/java.c`) devuelve en cambio la ruta suelta de Vita
> (`"ux0:data/inotia3/assets/common/game_res/MainMenu.png.jpg"`) -- correcta y ya comprobada abrible por
> `open()` (es exactamente el `open(...): 4` que sí funciona, en la llamada de REGISTRO del recurso vía el
> modo 2 de esta misma función) -- pero **no empieza con `/`**, así que el atajo de modo 3 nunca dispara,
> y `CS_knlGetResource` cae en la construcción real
> `"/sdcard/Android/data/inotia3/files/ux0:data/inotia3/assets/common/game_res/MainMenu.png.jpg"` -- que
> por supuesto no existe (no hay emulación de `/sdcard` en este port, ni falta que hace) y falla con `-1`
> (confirmado en AMBOS logs, Corridas 19/20: `open(/sdcard/Android/data/inotia3/files/ux0:data/..., 0):
> -1`). `CS_knlGetResource` no chequea ese fallo antes de retornar, así que `SGL_Resource::FromBundle` le
> entrega a `DecodePNG` un buffer que `CS_knlAlloc` reservó pero que **nunca se llenó** -- de ahí "Not a
> PNG file": no hay corrupción, simplemente nunca hubo datos.
>
> **Fix** (`source/patch.c`): se hookeó `File::GetFullPathName` completa (símbolo real, no PLT --
> `_ZN4File15GetFullPathNameEPKcPcmi`, confirmado vía `objdump -T`) con una reimplementación fiel que llama
> a las mismas funciones reales del `.so` (`jGetUserDir`/`jGetPackageName`/`jGetApkFilePath`/
> `CS_fsGetFileDescriptor`, también símbolos reales resueltos vía `objdump -T`) para los modos 0/2/3 EXACTO
> igual que el original -- el único cambio real es que el atajo de "ya resuelto, usar literal" ahora
> también reconoce cualquier ruta con estilo de dispositivo de Vita (contiene `:`, ej. `"ux0:..."`) además
> de la que empieza con `/` -- un nombre corto de recurso Android nunca contiene `:`, así que es una señal
> inequívoca de que `name` ya es una ruta resuelta por nuestros propios stubs JNI, no un nombre corto que
> el modo 3 necesite prefijar. No se tocó nada del comportamiento de los modos 0/2 (siguen llamando
> exactamente a las mismas funciones reales del `.so`), minimizando el riesgo de romper otras rutas de
> carga de archivos que ya funcionan. Build compilado (cross real vía VitaSDK) y verificado
> (`build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-07-24).
>
> **Pendiente**: redeployar, reproducir (cargar `MainMenu.png`), mandar el log + `.psp2dmp` nuevo. Si el
> fix es correcto, debería aparecer una línea `read(4, ...)` real para `MainMenu.png.jpg` esta vez (con los
> primeros bytes coincidiendo con la firma PNG real, `89 50 4e 47 0d 0a 1a 0a`) y el juego debería avanzar
> más allá de la carga del menú principal -- si sigue fallando, buscar si ahora el crash se movió a un
> punto distinto (progreso) o si `File_GetFullPathName_hook` introdujo una regresión en algún otro modo/
> caller (revisar el log completo por nuevos `open(...): -1` inesperados que no aparecían antes).

> **Corrida 22 (2026-07-25) — El fix de la Corrida 21 funcionó: CONFIRMADO, ya no crashea.** `logs/
> log_2026-07-25_00-30-47.txt`: `MainMenu.png.jpg` ahora sí se abre, se mapea con `mmap(0x0, 148229, 1, 2,
> 4, 0)` (el `mmap` fake de `source/reimpl/mem.c`, funcionando -- confirma que SÍ era `CS_knlGetResource`
> quien usaba `mmap`, no `read()` directo, tal como predecía la Corrida 21) y sube como textura real
> (`glTexImage2D ... w=512 h=512`). `font.fnt` (el mismo mecanismo) también carga bien. El juego llega a
> `nativeInit() and resume returned -- entering render loop.` y se queda corriendo en el loop principal
> (~173 iteraciones de `SetTimer` capturadas en el log, consistente con varios segundos de ejecución real,
> no un loop trabado -- `wr_run_timers`/`source/wr_timer.c` re-arma el timer del motor cada frame, es
> comportamiento esperado, no un bug).
>
> Pantalla en negro pese a esto. Antes de instrumentar nada se descartó la causa más común de este síntoma
> específico en ports vitaGL (`libshacccg.suprx` faltante): `source/utils/glutil.c:27-32`
> (`gl_preload()`) ya llama `fatal_error()` si no encuentra `ur0:/data/libshacccg.suprx` (o su ruta
> `external/`) ANTES de que el juego llegue a ninguna parte -- y el log muestra "OpenGL preloaded." y todo
> el `nativeInit`/carga de texturas/render loop corriendo sin ningún diálogo de error, así que `gl_init()`
> ya pasó ese chequeo con éxito. Se descarta sin necesidad de FTP-chequear el archivo en la consola (aunque
> `porting_tools/manage_vita.py` opción 14 sigue siendo el chequeo directo si hiciera falta en el futuro).
>
> El problema real es que **ninguna función de este archivo logueaba si el `.so` llega a emitir un draw
> call real** -- `glDrawArrays_soloader`/`glDrawElements_soloader` (`source/utils/glutil.c`) ya envuelven
> TODAS las llamadas a dibujo del `.so` (por la conversión `GL_FIXED`->`GL_FLOAT`), pero no logueaban nada;
> sin eso, "pantalla negra" es ambiguo entre "no se llama a dibujar nada" (bug de estado/lógica de juego,
> más profundo) y "se dibuja pero no se ve nada" (textura vacía, blend/depth mal configurado, viewport,
> etc. -- un bug de rendering más superficial).
>
> **Instrumentación agregada** (`source/utils/glutil.c`): las primeras 20 llamadas a `glDrawArrays`/
> `glDrawElements` ahora loguean `mode`, `count`, la textura 2D bound (`GL_TEXTURE_BINDING_2D`), y el
> estado de `GL_BLEND`/`GL_DEPTH_TEST` en ese momento -- suficiente para saber de una sola pasada si hay
> draw calls en absoluto y, si los hay, si están usando una textura válida (id != 0, coincidiendo con la
> que subió `glTexImage2D` para `MainMenu.png`) o si el blend/profundidad están en un estado que
> explicaría que se dibuje pero no se vea (ej. blend con factores que dan alpha 0, o depth test fallando
> contra un buffer sin limpiar bien). También se agregó logueo a `glClearColorx_soloader` (las primeras 10
> llamadas) para ver a qué color pide limpiar el `.so` -- si pide negro explícitamente en algún punto,
> descarta que sea SOLO nuestro `glClear()` de `source/main.c` (que ya limpia a negro por default de
> OpenGL, sin fijar color, cada frame) el que está "ganándole" al contenido real. Build compilado y
> verificado (`build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-07-25).
>
> **Pendiente**: redeployar, dejar correr varios segundos en la pantalla negra, mandar el log nuevo.
> Buscar las líneas `[GL] glDrawArrays`/`[GL] glDrawElements` (si NO aparecen en absoluto, el bug está en
> que el `.so` nunca llega a la lógica de dibujo -- revisar qué hace `nativeReDraw`/el estado inicial del
> menú antes de asumir nada más -- ; si SÍ aparecen, revisar `bound_tex` -- `0` significa que se está
> dibujando sin ninguna textura bound, un blanco/negro sólido según el color de vértice -- y `blend`/
> `depth_test` contra lo esperado para UI 2D simple, que normalmente NO debería necesitar depth test
> activo).

> **Corrida 23 (2026-07-25) — Draw calls confirmados reales (no es "nunca se dibuja nada"); otra sesión ya
> encontró y arregló en paralelo un bug real de conversión `GL_FIXED` que faltaba en un grupo grande de
> funciones de matriz/color/proyección; instrumentación adicional agregada para confirmar de una vez si
> ese fix alcanza.** `logs/log_2026-07-25_01-04-22.txt` y `log_2026-07-25_01-20-17.txt` (ambos generados
> ANTES de que terminara la build con el fix nuevo, confirmado por el propio mensaje de la otra sesión):
> los logs de la Corrida 22 confirman `[GL] glDrawArrays mode=5 count=4 bound_tex=3/4/5 blend=0
> depth_test=0` repetido consistentemente frame a frame -- **sí hay draw calls reales**, con texturas
> bound válidas (ids no-cero, coincidiendo con las que subieron `MainMenu.png`/`font.fnt`), sin blend ni
> depth test activos (normal para UI 2D). Descarta "nunca se llega a dibujar nada" -- el bug está en
> CÓMO se dibuja, no en si se dibuja.
>
> Otra sesión, en paralelo, encontró independientemente que `source/dynlib.c` mapeaba una familia grande
> de funciones `gl*x` (`glOrthox`, `glFrustumx`, `glLoadMatrixx`, `glMultMatrixx`, `glColor4x`,
> `glTranslatex`, `glRotatex`, `glScalex`, etc. -- las variantes de punto fijo Q16.16 de GLES1) directo a
> sus símbolos reales de vitaGL SIN pasar por ningún wrapper de conversión -- mismo bug de clase que
> `glClearColorx_soloader`/`glTexParameterx_soloader` (los únicos dos `*x` que sí tenían wrapper) ya
> arreglaban, simplemente sin cobertura completa. vitaGL trata sus argumentos `GLfloat` como floats reales
> sin importar qué variante GLES1 los originó -- pasarle un entero Q16.16 CRUDO (típicamente del orden de
> decenas de millones para valores como "960.0") como si fuera un float produce una matriz de
> proyección/vista con escalas o traslaciones absurdamente grandes, empujando TODA la geometría 2D fuera
> del frustum visible -- coincide EXACTO con el síntoma "hay draw calls, con texturas y estado normal, pero
> no se ve nada". Se agregaron wrappers `*_soloader` faithful (dividir por 65536.0f) para toda la familia
> y se remapearon en `dynlib.c`; confirmado con `objdump -T` contra el `.so` real que efectivamente importa
> `glOrthox`/`glFrustumx` (NO `glOrthof`/`glFrustumf` -- el juego usa la variante de punto fijo para su
> configuración de proyección), así que este fix aplica directo a la ruta de código relevante, no es
> especulativo.
>
> **No hay todavía un log post-fix para confirmar si esto solucionó el render** (la sesión paralela
> reporta que una prueba más reciente sigue en negro, pero no compartió ese log). Antes de asumir que el
> fix de `GL_FIXED` no alcanzó, hace falta un log real generado DESPUÉS de este build para saber si
> `glOrthox` siquiera se llegó a ejecutar con los wrappers nuevos.
>
> **Instrumentación agregada** (`source/utils/glutil.c`, extendiendo el `log_draw_call` de la Corrida 22):
> ahora también vuelca, en las primeras 20 llamadas a `glDrawArrays`/`glDrawElements`: `GL_VIEWPORT`,
> `GL_SCISSOR_TEST`+`GL_SCISSOR_BOX`, los términos de escala/traslación (`[0]`,`[5]`,`[12]`,`[13]`) de
> `GL_PROJECTION_MATRIX` y `GL_MODELVIEW_MATRIX`, y (si el draw usó vértices `GL_FIXED`, ya convertidos a
> float por `flush_pending_fixed_arrays`) las coordenadas reales de los primeros 2 vértices enviados. Con
> esto, un solo log nuevo alcanza para confirmar/descartar la hipótesis de la proyección sin necesidad de
> otra ronda de instrumentación: si `proj`/`mv` tienen valores absurdos (miles/millones en vez de un rango
> típico ortho tipo `sx=0.002 tx=-1.0`) o si las coordenadas de vértice caen muy fuera de `0..960 x
> 0..544`, confirma la causa; si todo se ve razonable y aun así no se ve nada en pantalla, hay que mirar
> otra cosa (contenido real de la textura, orden de draw calls tapando el fondo con algo transparente,
> etc.). Build compilado (cross real vía VitaSDK) y verificado
> (`build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-07-25).
>
> **Pendiente**: redeployar el build actual (incluye AMBOS fixes: los wrappers `GL_FIXED` completos + esta
> instrumentación nueva), reproducir, mandar el log. Buscar las líneas `[GL] glDrawArrays`/
> `glDrawElements` extendidas -- son el dato decisivo para saber si ya se arregló, o hacia dónde seguir.

> **Corrida 24 (2026-07-25) — El fix `GL_FIXED` de la Corrida 23 SÍ está andando y es matemáticamente
> correcto -- pero la pantalla sigue en negro, así que la causa real es otra cosa. Instrumentación
> agregada para las dos hipótesis restantes más probables (iluminación fixed-function mal configurada,
> contenido de textura realmente negro).** `logs/log_2026-07-25_02-38-34.txt` -- primer log generado
> DESPUÉS del build con el fix completo, confirmado por la línea nueva ya presente:
> `[GL] glOrthox l=0.0 r=384.0 b=0.0 t=256.0` (valores razonables de coordenadas de pantalla, no los
> enteros Q16.16 crudos de antes). Verificación matemática completa contra la matriz real leída por
> `glGetFloatv`: para esa llamada, `proj(sx=0.005 sy=-0.008 tx=-1.000 ty=1.000)` coincide con la fórmula
> estándar de `glOrtho` (`sx=2/(r-l)=2/384=0.0052`, `sy=-2/(t-b)=-2/256=-0.0078`) -- **la conversión
> `GL_FIXED`->`float` para la proyección es correcta, confirmado con números reales, no en teoría**. La
> segunda llamada (`glOrthox l=0 r=480 b=0 t=320`) también coincide (`sx=0.004`≈`2/480`,
> `sy=-0.006`≈`-2/320`). Los vértices reales dibujados (`v0=(0.0,0.0) v1=(0.0,256.0)`) caen bien adentro de
> ambos rangos de proyección. **La hipótesis de la Corrida 23 (proyección con escala absurda empujando la
> geometría fuera de cámara) queda descartada como causa del negro: la proyección y la geometría ya están
> bien.**
>
> Sin nuevos `fatal`/`ERR` en el log (mismos warnings benignos ya conocidos de corridas anteriores:
> `/sdcard/...` inexistente, `save0/1/2.dat` inexistentes, métodos JNI opcionales no encontrados) y
> `nativeInit() and resume returned -- entering render loop.` sin problema. El render loop sigue estable.
>
> **Instrumentación agregada** (`source/utils/glutil.c`) para las dos hipótesis clásicas de GLES1
> fixed-function que quedan y que el dump anterior no cubría:
> - `glTexImage2D_soloader`: ahora samplea (paso primo 997, hasta 4096 muestras, para no pegarse siempre al
>   mismo canal de un pixel RGBA de 4 bytes) los bytes crudos del buffer de píxeles que el `.so` sube y
>   loguea `min`/`max`/`avg` -- si `MainMenu.png` realmente se decodificó a negro/vacío (bug de
>   antes-de-GL, en `DecodePNG`/libpng, no en GL), esto lo va a mostrar directo sin necesidad de volcar el
>   archivo.
> - `log_draw_call`: se agregó `GL_LIGHTING`, `GL_TEXTURE_2D`, `GL_CULL_FACE` al dump existente.
>   `GL_LIGHTING` activado sin ninguna luz configurada es el bug clásico de pipeline fixed-function que
>   produce EXACTAMENTE este síntoma (geometría y textura correctas, pero cada fragmento se multiplica por
>   una contribución de luz ~0 -> todo negro) -- y es plausible que nunca se haya inicializado ningún
>   `glLight*`/`glEnable(GL_LIGHT0)` en este port ya que Inotia 3 es un motor 2D de UI, no debería
>   necesitar iluminación en absoluto, así que si `lighting=1` aparece en el log es una pista muy fuerte.
>   `GL_TEXTURE_2D` desactivado daría blanco (color de vértice plano), no negro, pero se descarta igual de
>   paso; `GL_CULL_FACE` activado con el winding equivocado descartaría el triangle strip completo
>   (renderizaría transparente/lo-que-hubiera-antes, no necesariamente negro, pero vale chequear). Build
>   compilado y verificado (`build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-07-25).
>
> **Pendiente**: redeployar, reproducir, mandar el log. Buscar `lighting=` en las líneas `[GL]
> glDrawArrays`/`glDrawElements` -- si aparece `lighting=1`, ese es el bug (fix: `glDisable(GL_LIGHTING)`
> a la fuerza en nuestro propio loop, igual que ya se hace con `GL_SCISSOR_TEST` en `source/main.c`, o
> encontrar y neutralizar el `glEnable(GL_LIGHTING)` real si aparece desde el `.so`). Si `lighting=0`,
> mirar `pixel_sample` en las líneas `[GL] glTexImage2D` -- si `avg` está cerca de `0` con `max` también
> bajo, la textura decodificada YA es negra antes de llegar a GL (habría que volver a mirar `DecodePNG`/
> `libpng`, no más código de GL) y ese sería el siguiente hilo a seguir.

> **Corrida 25 (2026-07-25) — `lighting=0` en las 20 llamadas logueadas: se descarta la hipótesis de
> iluminación. Pero aparece un hallazgo mucho más fuerte: `texture_2d=0` en TODAS las 20 llamadas
> logueadas, y `pixel_sample(n=0)` para el upload real de `MainMenu.png`/`font.fnt` (512x512,
> `intFmt=1908`/`GL_RGBA` `type=1401`/`GL_UNSIGNED_BYTE` -- el chequeo de tipo SÍ pasaba, así que `n=0` no
> era por eso).** `logs/log_2026-07-25_02-59-49.txt`: mismo patrón estable ya visto en la Corrida 24
> (`glDrawArrays mode=5 count=4` alternando `bound_tex=3/4`, dos `glOrthox` distintos con matrices de
> proyección correctas) pero ahora con los campos nuevos -- `lighting=0 texture_2d=0 cull_face=0` en las
> 20 llamadas. **`GL_TEXTURE_2D` desactivado en CADA draw call logueado, a pesar de tener una textura real
> bound (`bound_tex` no-cero) es la pista más fuerte hasta ahora**: en el pipeline fixed-function, sin
> `GL_TEXTURE_2D` habilitado el color de cada fragmento sale del color de vértice plano/interpolado, NO de
> la textura -- si ese color también resulta oscuro (no necesariamente blanco puro como asumía la Corrida 24;
> el color de vértice puede variar por draw call vía `glColor4x` o un color-array por-vértice que la
> Corrida 22-24 nunca inspeccionó), esto solo alcanza para explicar negro directamente.
>
> El límite de 20 llamadas logueadas (Corridas 22-24) solo alcanzaba a cubrir el arranque de la pantalla
> de carga -- nunca llegaba a ver los draws reales de la pantalla de menú, que es lo que en realidad
> importa para "¿se ve `MainMenu.png`?".
>
> **Instrumentación agregada** (`source/utils/glutil.c` + `.h`, `source/dynlib.c`):
> - Límite de `log_draw_call` subido de 20 a 400 -- para cubrir muchos más frames, no solo el arranque.
> - Nuevo `glEnable_soloader`/`glDisable_soloader` (mapeados en `dynlib.c` reemplazando el paso directo sin
>   wrapper de antes) que loguean cada vez que el `.so` activa/desactiva una de las capacidades que más
>   importan para visibilidad (`GL_TEXTURE_2D`, `GL_LIGHTING`, `GL_BLEND`, `GL_ALPHA_TEST`, `GL_CULL_FACE`,
>   `GL_DEPTH_TEST`) -- así se ve la TRANSICIÓN real (o su ausencia total) en vez de solo un muestreo pasivo
>   en el momento del draw.
> - `log_draw_call` ahora también vuelca `GL_COLOR_ARRAY` (¿hay un color por-vértice en vez de uno plano?)
>   y, si el draw usó un color-array `GL_FIXED` (ya convertido a float), el color real del primer vértice
>   (`c0=(r,g,b,a)`) -- para confirmar directamente si el color efectivo con el que se rellena cada
>   fragmento (dado que `texture_2d=0`) es oscuro/negro o no.
> - `glTexImage2D_soloader`/`glTexSubImage2D_soloader`: además de `pixel_sample`, ahora logean el puntero
>   `pixels` crudo -- si es `NULL` en el upload de 512x512, confirma que la subida real de bytes pasa por
>   `glTexSubImage2D` (que ahora también samplea) y no por `glTexImage2D` (que solo reservaría el storage).
>   Límite subido de 10 a 40 en ambas.
>
> Build compilado (cross real vía VitaSDK) y verificado (`build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`,
> 2026-07-25).
>
> **Pendiente**: redeployar, reproducir, mandar el log. Prioridad de lectura:
> 1. Buscar `glEnable(x)`/`glDisable(x)` -- ¿se llega a habilitar `GL_TEXTURE_2D` (`x=de`) en ALGÚN
>    momento del run? Si nunca aparece un `glEnable(de)`, el `.so` nunca lo activa para estos draws --
>    posible pista de que el juego espera que el estado de texturing sea "sticky" desde alguna
>    inicialización que nuestro puerto no está replicando bien, o que estos primeros draws
>    deliberadamente no usan textura (fondo de color plano) y los draws reales SÍ vienen después, ahora
>    visibles gracias al límite de 400.
> 2. Si aparece `color_array=1` y `c0=(...)` en algún draw con `texture_2d=0`, ese color es literalmente lo
>    que se está pintando en pantalla para ese draw -- si es oscuro/negro, ahí está el "por qué se ve
>    negro" para ESE draw específico (aunque falte explicar por qué texturing sigue apagado).
> 3. Buscar `pixels=` en las líneas `glTexImage2D`/`glTexSubImage2D` para el upload de 512x512 -- confirmar
>    cuál de las dos llamadas trae los bytes reales, y si su `pixel_sample` (ahora que se está mirando la
>    correcta) tiene `avg` cercano a `0`.

> **Corrida 26 (2026-07-25) — Causa raíz real encontrada: el orden del render loop en `main.c` borraba el
> frame DESPUÉS de dibujarlo. Además, dos bugs en la instrumentación misma venían dando pistas falsas.**
> `logs/log_2026-07-25_03-36-52.txt` (2114 líneas): confirmó que sí hay draws reales con texturas reales
> (`glTexSubImage2D` 512x512 `RGBA`/`GL_UNSIGNED_BYTE` con `pixel_sample avg=118.3` -- datos de píxel
> genuinos, no vacíos) y que `glEnable(de1)` (`GL_TEXTURE_2D`) se llama explícitamente en la secuencia de
> inicio (líneas 308/434/436) antes de que empiecen los draws en línea 499 -- pero el dump de
> `log_draw_call` seguía mostrando `texture_2d=0`/`blend=0`/etc. en las 400 llamadas logueadas, sin
> excepción, mientras `bound_tex`, `proj(...)` y `scissor`/`viewport` sí variaban normalmente entre draws.
> Un valor que nunca cambia mientras los campos vecinos sí lo hacen es la firma de un bug de lectura, no de
> un estado real -- **`source/utils/glutil.c`'s `log_draw_call` usaba `glGetIntegerv(GL_TEXTURE_2D/
> GL_BLEND/GL_DEPTH_TEST/GL_LIGHTING/GL_CULL_FACE/GL_COLOR_ARRAY, ...)` para leer capacidades de
> habilitación -- esos son enums de `glEnable`/`glDisable`/`glIsEnabled`, no `pname` válidos de
> `glGetIntegerv`, y vitaGL evidentemente no los soporta por esa vía (siempre devuelve 0)**. Se cambió a
> `glIsEnabled(cap)`, la API portable correcta. Un segundo bug, independiente: `log_draw_call` siempre leía
> el índice `0` del buffer de vértices/colores ya convertido (`first_verts[0]`/`first_color[0]`) en vez del
> índice `first` que ese draw call específico realmente consume (`glDrawArrays(mode, first, count)` dibuja
> desde el índice `first`, no desde `0`) -- por eso `v0=(0.0,0.0)` y `c0=(0.00,0.00,0.00,0.00)` salían
> idénticos en los 400 draws pese a ser objetos distintos. Corregido para indexar por `first` (y por
> `indices[0]` en el caso de `glDrawElements`).
>
> Con las lecturas de capacidad descartadas como confiables, se revisó el loop de render en `source/
> main.c` línea por línea contra el `WrapperRenderer.java` decompilado
> (`decompiled_apk/sources/com/com2us/wrapper/WrapperRenderer.java`) -- **`onDrawFrame()` real no tiene
> NINGÚN `glClear` propio; el Java solo hace `process()` (touch + `nativeRender`, que es donde el `.so`
> dibuja todo) seguido de `nativeReDraw()`**. El loop de este port, en cambio, llamaba
> `wr_run_timers(...)` (que dispara `nativeRender()`, donde el juego emite los 400 `glDrawArrays` del log)
> y **DESPUÉS** hacía `glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)` -- justo antes de `nativeReDraw()`
> y `gl_swap()` -- borrando el framebuffer exactamente después de que el juego terminó de dibujar su
> frame y antes de presentarlo. Cada frame: el juego dibuja sus sprites -> se borra el framebuffer -> se
> presenta la pantalla vacía. Esto explica tanto la pantalla negra como los FPS más bajos reportados por el
> usuario (si está haciendo el trabajo de render, solo para descartarlo).
>
> **Fix aplicado** (`source/main.c`): el bloque de reset defensivo de scissor/viewport + `glClear` (agregado
> en la Corrida 2 de Fase 4 para el bug de "pantalla reducida con borde") se movió de **después** de
> `wr_run_timers(...)` a **antes**, preservando la intención original (limpiar antes de que el `.so` dibuje,
> no destruir lo que ya dibujó).
>
> Build compilado (cross real vía VitaSDK, `/tmp/inotia3-build`) y verificado sin errores/warnings nuevos
> (`build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-07-25).

> **Corrida 27 (2026-07-25) — El fix de orden del `glClear` de la Corrida 26 era real pero NO es la única
> causa: pantalla sigue en negro. Instrumentación de la Corrida 26 confirmada funcionando (datos ya
> confiables), y aparece un hallazgo nuevo: la matriz modelview nunca cambia, lo cual no encaja con cómo
> el `.so` arma su geometría.** `logs/log_2026-07-25_18-19-38.txt` (1514 líneas, generado ya con el fix de
> la Corrida 26 desplegado): `texture_2d=2` (no-cero => habilitado) y `blend=1` en prácticamente los 384
> draws logueados -- confirma que el fix `glIsEnabled` de la Corrida 26 funciona y que texturing/blend
> están genuinamente activos, descartando esas dos hipótesis del todo. `bound_tex` alterna entre 3 y 4
> (dos atlases, probablemente fuente/UI) con una sola aparición de `bound_tex=5` (la textura 512x512 tipo
> `MainMenu.png`). Pero **`v0=(0.0,0.0)` y `mv(sx=1.000 sy=1.000 tx=0.000 ty=0.000)` (modelview identidad)
> siguen siendo idénticos en los 384 draws, ahora YA con el fix del índice `first` de la Corrida 26
> aplicado** -- es decir, o bien `first` es genuinamente `0` en cada uno de estos draws, o queda un bug
> nuevo.
>
> Se cruzó esto contra el `.so` real decompilado (`decompiled_so/libgame_armeabi/out_ghidra.c`):
> `glVertexPointer(2, 0x140c /*GL_FIXED*/, 0, &DAT_001778a8)` se llama en múltiples sitios distintos
> (líneas 106964, 107176, 107446, 107464, 108559, 108579) **reutilizando la MISMA dirección estática de
> buffer de vértices** -- el patrón clásico de "quad plantilla fija + reposicionar con la matriz modelview
> (`glTranslatex`/`glLoadMatrixx`) antes de cada draw", no de un buffer por-objeto con offsets. Si ese es el
> mecanismo real, la matriz modelview leída en el momento del draw NO debería ser idéntica siempre --
> a menos que haya un bug de lectura en vitaGL para `glGetFloatv(GL_MODELVIEW_MATRIX)` análogo al de
> `glGetIntegerv` con los enums de capacidad que ya se corrigió en la Corrida 26 (incluso `glTranslatex_
> soloader`/`glLoadMatrixx_soloader`/`glMultMatrixx_soloader`, que sí están wrapeados en `dynlib.c`, no
> logueaban nada hasta ahora -- no había forma de saber si el `.so` siquiera las estaba llamando).
>
> **Instrumentación agregada** (`source/utils/glutil.c`):
> - `log_draw_call`: ahora vuelca los 4 vértices del quad (no solo el vértice `first`) más su bounding box
>   (`bbox=(minx,miny)-(maxx,maxy)`) y el valor real de `first` usado, en vez de inferir la posición/tamaño
>   del quad a partir de un solo vértice.
> - `glTranslatex_soloader`, `glLoadMatrixx_soloader`, `glMultMatrixx_soloader`: antes silenciosos, ahora
>   loguean (tope de 20 cada uno) los valores de traslación/escala reales que el `.so` pasa, para poder
>   cruzarlos directamente contra la lectura de `glGetFloatv(GL_MODELVIEW_MATRIX)` en el mismo log.
>
> Build compilado (cross real vía VitaSDK) y verificado sin errores nuevos (`build/inotia3.vpk`/
> `eboot.bin`/`inotia3.elf`, 2026-07-25).
>
> **Pendiente (sesión cerrada sin resolver aún -- retomar acá la próxima vez)**: redeployar, reproducir,
> mandar el log. Prioridad de lectura:
> 1. Buscar `glTranslatex`/`glLoadMatrixx`/`glMultMatrixx` en el log nuevo -- si aparecen con valores `tx`/
>    `ty` no-cero, pero `mv(...)` en las líneas `glDrawArrays` inmediatamente después sigue mostrando
>    `tx=0.000 ty=0.000`, eso CONFIRMA un bug de lectura en `glGetFloatv(GL_MODELVIEW_MATRIX)` de vitaGL
>    (no un bug del juego) -- la mitigación sería mantener nuestro propio shadow-tracking de la matriz
>    modelview en vez de confiar en el query de vitaGL para diagnóstico (y, si el bug fuera real más allá
>    del diagnóstico -- i.e. si vitaGL también usa mal esa matriz internamente al transformar vértices, no
>    solo al reportarla -- sería un bug de vitaGL en sí que ameritaría reportarlo upstream).
> 2. Si NUNCA aparece ningún `glTranslatex`/`glLoadMatrixx`/`glMultMatrixx` en absoluto durante los draws de
>    menú, el `.so` genuinamente no está reposicionando nada -- revisar si hay una llamada equivalente en
>    punto flotante (`glTranslatef`/`glLoadMatrixf`, mapeadas directo a GL real en `dynlib.c` sin wrapper
>    propio, así que sin logging hoy) que sí se esté usando; si es así, agregarles logging también.
> 3. Mirar el nuevo `bbox=(...)` por draw call -- si el bbox es literalmente un punto (`minx==maxx` y
>    `miny==maxy`), el quad mismo es degenerado (buffer de vértices mal armado o mal leído, no solo un
>    problema de posición) y el hilo a seguir es la conversión `GL_FIXED`->`float` en
>    `convert_fixed_array`/`flush_pending_fixed_arrays`, no la matriz.

> **Corrida 28 (2026-07-26) — Nuevo ángulo: el render loop en régimen estable NUNCA vuelve a dibujar
> `MainMenu.png` tras el arranque; solo repite dos texturas RGB565 (`GL_UNSIGNED_SHORT_5_6_5`) frame tras
> frame, y nuestra instrumentación no podía leer su contenido real. Fix de instrumentación aplicado,
> compilado, pendiente de un log nuevo.** `logs/log_2026-07-26_23-44-33.txt` (1301 líneas, corrida corta):
> sin `fatal`/crash. `GetFileDescriptor("MainMenu.png")`/`("font.fnt")` exitosos (ahora resueltos como
> archivo suelto en `ux0:data/inotia3/assets/common/game_res/*.jpg`, no vía offset dentro del `.apk` —
> aparentemente los assets ya están extraídos sueltos en la consola de prueba actual, cambio de método
> válido, mismo resultado). `nativeInit() and resume returned -- entering render loop.` en la línea 683.
>
> **Contando `bound_tex` en las 281 draw calls logueadas de todo el archivo: `bound_tex=3` → 140,
> `bound_tex=4` → 140, `bound_tex=5` (la textura RGBA 512x512 real de `MainMenu.png`, la única con
> `pixel_sample avg=118.3` confirmado no-negro) → exactamente 1 vez, en la línea 500 — y esa única
> aparición ocurre ANTES de la línea 683 ("entering render loop"), es decir durante el setup/`nativeInit`,
> no durante el loop estable.** Después de "entering render loop" el patrón por frame es siempre
> idéntico y nunca cambia en los ~140 frames capturados: `GetEnv` + `CallStaticVoidMethod(...,25,...)`
> (re-armado de `SetTimer`, ya documentado como normal) + exactamente 2 `glDrawArrays` (uno con
> `bound_tex=3`, otro con `bound_tex=4`) — sin ninguna otra actividad JNI/archivo/socket en el medio. Todos
> los draws comparten el mismo quad (`verts=[(0,0),(0,256),(384,0),(384,256)]`, siempre igual) dentro de
> una proyección ortho de `480x320` (`sx=0.004≈2/480 sy=-0.006≈-2/320`) — geométricamente razonable (un
> gráfico de 384x256 alineado arriba-a-la-izquierda dentro del canvas de diseño de 480x320, no fuera de
> rango), `texture_2d=2` (activo) y `blend=1` en ambos — es decir, a nivel de estado GL todo se ve
> correcto, no hay ningún interruptor mal puesto.
>
> Las dos texturas usadas en el loop (`glTexImage2D`/`glTexSubImage2D`, líneas 302-305 y 420-423/497) son
> `intFmt=0x1907` (`GL_RGB`) `type=0x8363` (`GL_UNSIGNED_SHORT_5_6_5`) de `512x256` y `512x64` — **ambas
> se suben ANTES de que el `.so` siquiera pida `MainMenu.png`/`font.fnt` por `GetFileDescriptor`** (línea
> 445+), así que no son la imagen de menú ni la fuente reales — más probablemente un fondo/ícono de
> pantalla de carga y una franja de fuente propios del motor, cacheados en RGB565 para ahorrar memoria.
> **Punto ciego real:** `sample_pixels()` en `source/utils/glutil.c` solo sabía leer `GL_UNSIGNED_BYTE` —
> para `GL_UNSIGNED_SHORT_5_6_5` devolvía `pixel_sample(n=0 ... avg=-1.0)` siempre, sin importar si el
> contenido real era una imagen válida o basura/negro — exactamente los dos únicos texels que se dibujan
> en régimen estable, sin visibilidad de si su contenido es la causa del negro.
>
> Dos hipótesis quedan abiertas y son mutuamente excluyentes con el dato que falta:
> 1. **Bug de contenido/formato**: los dos RGB565 están genuinamente negros o corruptos (banda de color
>    equivocada, endianness, u otro bug de conversión no relacionado con `GL_FIXED`) — explicaría el negro
>    directamente pese a que el resto del pipeline esté bien.
> 2. **Bug de lógica de juego/estado**: son un fondo+ícono de pantalla de "cargando" legítimos (con
>    contenido real, no negro) y el `.so` simplemente nunca transiciona a la escena de menú real (que sí
>    usaría `MainMenu.png`/`font.fnt`, cargados con éxito pero jamás vueltos a dibujar) — un stall de
>    máquina de estados esperando algo que este port nunca satisface (candidatos: Hub/red no-op,
>    inicialización de audio, algún flag de "carga completa" nunca seteado), no un problema de GL en sí.
>
> **Fix de instrumentación aplicado** (`source/utils/glutil.c`, `sample_pixels()`): ahora decodifica
> `GL_UNSIGNED_SHORT_5_6_5`/`4_4_4_4`/`5_5_5_1` a una luma aproximada de 0-255 por texel muestreado (antes
> silenciosamente ignorados), para que `min`/`max`/`avg` en los logs de `glTexImage2D`/`glTexSubImage2D`
> signifiquen lo mismo que para el camino `GL_UNSIGNED_BYTE` ya existente. Build compilado y verificado
> (cross real vía VitaSDK, `/tmp/inotia3-build` → `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`,
> 2026-07-26) — sin warnings nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log. Buscar las líneas `[GL] glTexImage2D`/
> `glTexSubImage2D` de los dos uploads `512x256`/`512x64` (`type=8363`) — si `avg` sale cercano a `0` con
> `max` también bajo, confirma la Hipótesis 1 (contenido negro/corrupto, seguir por conversión de formato,
> no lógica de juego); si `avg`/`max` muestran variación real (imagen con contenido), confirma la Hipótesis
> 2 (stall de estado — el próximo paso sería rastrear en `out_ghidra.c` qué condición gatea la transición
> fuera de la pantalla de carga, no seguir tocando código de GL).

> **Corrida 29 (2026-07-27) — Confirmado con datos reales: la textura RGB565 que se dibuja en régimen
> estable está en negro puro (`avg=0.0`, no un bug de instrumentación); causa raíz localizada a un nivel
> más — el subsistema "CSFB" (framebuffer de software del motor) nunca refresca su contenido — y hook de
> diagnóstico agregado, pendiente de un log nuevo.** `logs/log_2026-07-27_00-19-07.txt`: con el fix de
> `sample_pixels()` de la Corrida 28 ya desplegado, el upload real (`x=0 y=0 w=384 h=256 type=8363`,
> puntero `0x83653388`) sale `pixel_sample(n=1612 min=0 max=0 avg=0.0)` — **negro genuino**, mientras que
> en el mismo log el upload RGBA de `MainMenu.png` sigue saliendo correcto (`avg=118.3 max=255`),
> confirmando que el decodificador RGB565 nuevo funciona y que esto no es un artefacto de logging.
>
> Investigación (fork dedicado sobre `decompiled_so/libgame_armeabi/out_ghidra.c`, 214k líneas) encontró
> que el motor tiene DOS rutas de dibujo completamente separadas: `MainMenu.png` se dibuja vía
> `SGL_DrawTexture`/`GRPX_DrawPart` (el sprite-renderer normal, el mismo que ya arregló la Corrida 21 vía
> `File_GetFullPathName_hook`) — corrió **una sola vez**, durante el setup, nunca de nuevo. Las dos
> texturas RGB565 que se repiten cada frame son el **"CSFB" (software framebuffer)**: `RenderCoreInitialize`
> las crea vacías (`glTexImage2D(...,NULL)`, confirmado ya en Corrida 28: `pixels=0x0`), y
> `nativeReDraw()` → `RenderCoreRedraw()` las redibuja TODOS los frames sin excepción — con su propio
> `glClearColor(0,0,0,1)`+`glClear` primero, **tapando lo que sea que `nativeRender` haya dibujado en el
> mismo frame** — mediante `glDrawArrays` puro, sin ningún `glTexSubImage2D`. La única función que subiría
> contenido real es `CSFB_grpFlushLcd` → `RenderCoreFlush` → `RenderCoreFlush320` (el `glTexSubImage2D`
> real), alcanzable solo a través de `CS_grpFlushLcd()` → `(*__CS_grpFlushLcd)()`
> (`out_ghidra.c:155178-155181`). **Una búsqueda de texto completa por `CS_grpFlushLcd(` en las 214k
> líneas del pseudo-C no encontró ningún call site** — ni siquiera indirecto vía otro wrapper nombrado —
> consistente exactamente con lo que ya sabíamos por el log (esa textura se sube una sola vez, en negro,
> y nunca más).
>
> `jGetRenderer()` sigue hardcodeada a `return 0` (selecciona CSFB **también en Android real** — no es una
> mala elección de renderer de este port). Esto significa que en el binario real este mecanismo de flush
> SÍ se dispara de alguna forma (el juego funciona en Android) — probablemente a través de un puntero a
> función/slot de vtable que Ghidra no pudo nombrar en su análisis estático, no código muerto.
>
> **Instrumentación agregada** (`source/patch.c`): `CS_grpFlushLcd` y su global `__CS_grpFlushLcd` están
> exportados en `.dynsym` (confirmado con `objdump -T`: `CS_grpFlushLcd` @ `0xf55c4`, `__CS_grpFlushLcd`
> @ `0x56e1bc`, junto con `RenderCoreFlush` @ `0xf0a80`, `CSFB_grpFlushLcd` @ `0x10ab6c`,
> `RenderCoreRedraw` @ `0xef83c`, `RenderCoreInitialize` @ `0xf0c4c`, `jGetRenderer` @ `0x598a8` — todos
> resolubles por nombre vía `so_symbol()`, a diferencia de `excuteDirectly`/`jIsCracked` de la Fase 3 que
> requirieron offsets fijos por no estar en `.dynsym`). Se hookeó `CS_grpFlushLcd` (una función de una
> sola instrucción: `(*__CS_grpFlushLcd)()`) con `CS_grpFlushLcd_hook()`, que loguea cada invocación
> (tope de 30) y **preserva el comportamiento real exactamente** (dereferencia el mismo puntero global y
> lo invoca) — no es una reimplementación arriesgada como el fix de `File_GetFullPathName`, es un logging
> transparente sobre una función de una sola línea. Build compilado y verificado (cross real vía VitaSDK,
> `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-07-27).
>
> **Pendiente**: redeployar, reproducir, mandar el log. Buscar `[CSFB] CS_grpFlushLcd() called` —
> - Si **nunca aparece**, confirma que el disparador real (lo que sea que llame a esto en Android) no se
>   está ejecutando en este port — siguiente paso sería buscar qué llama a esto vía un puntero a función
>   NO nombrado por Ghidra (buscar dónde se TOMA la dirección de `CS_grpFlushLcd` — no solo dónde se
>   llama — como dato asignado a un campo de struct/vtable; o revisar si depende de un pthread dedicado
>   que este port no esté arrancando).
> - Si **sí aparece**, el mecanismo de flush corre pero el contenido igual sale negro — el problema
>   estaría más adentro (`RenderCoreFlush`/`CS_GRP_GET_FRAME_BUFFER_POINTER`, el puntero al framebuffer de
>   software real que la Corrida 21 nunca tocó) — haría falta instrumentar esa capa en vez de seguir
>   subiendo por la cadena de llamadas.

> **Corrida 30 (2026-08-04) — `CS_grpFlushLcd`/CSFB confirmado como callejón sin salida real, no un bug de
> este port.** `logs/log_2026-08-04_21-15-18.txt` (con el hook de la Corrida 29 desplegado): `[CSFB]
> CS_grpFlushLcd() called` **nunca aparece**, en todo un run que llega y se mantiene estable en el render
> loop. Un fork dedicado sobre `out_ghidra.c` (214k líneas) confirmó por qué: `__CS_grpFlushLcd` solo se
> asigna a `CSFB_grpFlushLcd`/`CSOGL_grpFlushLcd` dentro de `InitializeApp()` según `jGetRenderer()`
> (hardcodeada a 0 → rama CSFB siempre), y `CS_grpFlushLcd` no tiene NINGÚN call site en todo el binario —
> ni directo ni como dato/vtable — confirmado con una búsqueda de bytes crudos del literal de dirección
> sobre el `.so` completo (un solo hit, en su propia entrada de `.dynsym`). Tampoco hay ningún `pthread_create`
> cerca de `RenderCoreInitialize`/CSFB. **CSFB es código muerto también en el binario real de Android, no
> algo que este port rompió** — cerrado como pista.
>
> El mismo fork encontró el camino real de presentación de sprites, totalmente separado de CSFB: `GRP_Flush()`
> (`out_ghidra.c:20404`) → `GRPX_GetFlag()` bifurca a `GRPX_FlushAll()` → `SGL_grpFlushLcd(...)`
> (`:123810/123813`) o `GRPX_Flush()` → `SGL_FrameBufferToScreen()` (`:123819/123822`) — la misma
> `SGL_FrameBufferToScreen` ya implicada en el bug de "pantalla reducida con borde blanco" de la Fase 4.
> `GRP_Flush()` tiene 10 call sites reales en código de juego. Los 5 símbolos están exportados en `.dynsym`
> (sin offsets fijos necesarios). Se instrumentaron con hooks transparentes (`source/patch.c`, macro nueva
> `SO_CONTINUE_VOID` ya que `SO_CONTINUE` de `so_util.h` no compila con `type=void`) que preservan el
> comportamiento real. Build compilado y verificado sin errores.

> **Corrida 31 (2026-08-04) — Causa raíz real, arquitectónica: este port asumía la clase `WrapperRenderer`
> equivocada. Inotia 3 usa `WrapperDirtyRenderer`, cuyo modelo de presentación es completamente distinto, y
> el loop de `main.c` lo violaba en cada frame. Fix aplicado y compilado, pendiente de confirmar en
> consola.** `logs/log_2026-08-04_22-28-10.txt` (con los hooks `[FLUSH]` de la Corrida 30 desplegados):
> `GRP_Flush()` (#1) → `GRPX_GetFlag() -> 1` → `GRPX_Flush()` → `SGL_FrameBufferToScreen()` se disparan
> **una única vez**, durante el setup (junto con el único `glTexSubImage2D` real de `MainMenu.png`,
> `avg=118.3`) — y **nunca de nuevo** en el resto del run, aunque el loop estable sigue corriendo cientos de
> frames después de "entering render loop". Mismo patrón que CSFB en la Corrida 28/29, pero esta vez sobre
> el camino de dibujo REAL del juego, no uno muerto.
>
> Justo después del único `SGL_FrameBufferToScreen()`, el log muestra `GetStaticMethodID(..., "Flush",
> "()V"): 41` + `CallStaticVoidMethod(..., 41, ...)` — el `.so` llamando de vuelta a
> `WrapperJinterface.Flush()`. Cruzando esto contra `decompiled_apk/sources/com/com2us/wrapper/`:
> `WrapperJinterface.Flush()` (línea 377-378) llama a `wrapperRenderer.Flush()` — pero **`WrapperRenderer.
> Flush()` (la clase base, línea 446) es un no-op vacío**, mientras que `WrapperDirtyRenderer.Flush()`
> (línea 231-233, override) hace `this.wrapperView.requestRender()`. Cuál de las dos aplica depende de cuál
> clase instancia el juego — y `decompiled_apk/sources/com/com2us/inotia3/.../MainActivity.java:117` lo
> resuelve sin ambigüedad: `this.wrapperRenderer = new WrapperDirtyRenderer(...)`, **incondicional, sin
> rama alternativa**. Todo el análisis de arquitectura en la §0 de este documento (y cada Corrida hasta
> ahora) se basó en `WrapperRenderer.onDrawFrame()` (`process()` + `nativeReDraw()`, sin `glClear` propio) —
> pero **Inotia 3 nunca usa esa clase**; usa la subclase, que la overridea por completo:
> - `WrapperDirtyRenderer.onDrawFrame()` (línea 212-213) está **completamente vacío** — no llama a
>   `process()` ni a `nativeReDraw()`.
> - El `GLSurfaceView` corre en `RENDERMODE_WHEN_DIRTY` (`setRenderMode(0)`, línea 62) — Android NO llama a
>   `onDrawFrame()`/no hace swap del framebuffer por su cuenta en cada tick, solo cuando algo pide
>   explícitamente `requestRender()`.
> - `process()` (touch + `nativeRender()` — donde el `.so` dibuja) corre en un hilo dedicado propio
>   (`timerThread`, líneas 9-61) con su propio ciclo `wait()`/`notify()`, encolado sobre el hilo de GL vía
>   `queueEvent()` — desacoplado de cualquier vsync/frame automático.
> - El único disparador real de un swap de pantalla es `Flush()` → `requestRender()`, que agenda exactamente
>   una llamada al `onDrawFrame()` vacío + el swap EGL implícito que GLSurfaceView hace después — presentando
>   lo que sea que `process()` ya haya dibujado en el back buffer hasta ese momento.
> - **No hay ningún `glClear()` en absoluto en toda esta cadena Java.** El framebuffer solo se limpia si el
>   propio `.so` lo hace como parte de su dibujo.
>
> Es decir: en Android real, `GRP_Flush()` disparándose una sola vez durante la pantalla de carga y nunca
> más es **exactamente el comportamiento correcto** — la pantalla de carga se dibuja, se presenta una vez
> vía `Flush()`/`requestRender()`, y se queda estática en el framebuffer (nadie la borra) hasta que el juego
> decida redibujar/presentar la siguiente pantalla. **El loop de `main.c` de este port, en cambio, hacía
> `glClear()` + (hasta la Corrida 26) `nativeReDraw()` + `gl_swap()` incondicionalmente en CADA iteración,
> sin importar si el `.so` había pedido presentar algo** — literalmente borrando el único frame real
> dibujado en el próximo ciclo del loop, y presentando en su lugar contenido vacío/las texturas CSFB negras
> de `nativeReDraw()` (código muerto también en Android real, confirmado en la Corrida 30) para siempre.
> Esto es la causa raíz real detrás de la pantalla negra perseguida desde la Corrida 1 — todos los bugs
> reales encontrados y corregidos en el camino (orden del `glClear` en la Corrida 26, `GL_FIXED`, viewport/
> scissor de la Fase 4, `File_GetFullPathName` de la Corrida 21, etc.) eran genuinos, pero todos corren
> río abajo de este malentendido arquitectónico de fondo.
>
> **Fix aplicado**:
> - `source/wr_timer.h`/`.c`: nuevo par `wr_request_present()`/`wr_take_present_request()` — un flag simple
>   que mirroa `requestRender()`.
> - `source/java.c`: `Flush_stub` (method id 41, antes no-op) ahora llama a `wr_request_present()`.
> - `source/main.c`: el loop principal ya NO llama a `nativeReDraw()` (código muerto en la clase real que
>   usa este juego, confirmado arriba) ni hace `glClear()` incondicional cada iteración. Ahora: drena touch,
>   corre `wr_run_timers()` (que dispara `nativeRender()`, donde el `.so` dibuja Y limpia lo que necesite
>   por su cuenta), y solo hace `gl_swap()` si `wr_take_present_request()` reporta un `Flush()` pendiente.
>   Se agregó un clear+swap de ambos buffers una sola vez antes de entrar al loop (mismo rol que la garantía
>   de EGL de entregar una superficie nueva ya en blanco, que este port no tiene gratis).
> - Build compilado y verificado (cross real vía VitaSDK, `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`,
>   2026-08-04) — sin errores ni warnings nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log. Si los sprites aparecen pero con tearing/flicker o
> contenido viejo asomando entre `Flush()`s, el próximo sospechoso es el comportamiento de preservación del
> back buffer de vitaGL entre swaps (Android puede estar confiando en que el buffer persiste entre frames
> sin swap de por medio — un supuesto que vitaGL podría no cumplir igual); si la pantalla sigue
> completamente negra, revisar si `Flush()` realmente se está llamando más de una vez en el nuevo log (buscar
> múltiples `CallStaticVoidMethod(..., 41, ...)`) — si sigue siendo una sola vez, el problema vuelve a ser de
> lógica de juego/estado (por qué el `.so` no redibuja nada después de la pantalla de carga), no de
> plomería de presentación.

> **Corrida 32 (2026-08-04) — Confirmado: `Flush()`/`GRP_Flush` siguen en una sola invocación con el fix de
> la Corrida 31 desplegado — es lógica de juego/estado, no plomería. Localizado el candidato exacto:
> `MainProcess` corta camino sobre `gAppSleepInfo`, y `main.c` llama a `nativeResume`/`nativeResumeClet` en
> CADA arranque cuando Android real solo los llama tras un pause real. Instrumentación agregada, pendiente
> de confirmar con datos reales.** `logs/log_2026-08-04_23-10-46.txt` (con el logging nuevo de
> `source/wr_timer.c` desplegado): en todo el run existe **un solo timer ID registrado y disparado**
> (`id=-1730838840`, `intervaltime=100` — un timer que se re-arma cada 100ms él mismo, consistente con
> `g_MainTimer`/`MainProcess` documentado desde la Corrida 1) — ningún otro timer se registra ni se
> desregistra jamás. `[FLUSH]` sigue apareciendo una sola vez, **antes** de que este timer siquiera se
> registre (la única invocación de `GRP_Flush` ocurre durante el setup de `nativeInit`, no dentro del loop
> timer-driven). Es decir: el único driver per-frame del juego corre estable cada 100ms durante todo el run,
> pero jamás vuelve a pedir presentar nada.
>
> Leyendo `MainProcess` completo en `out_ghidra.c:25352`: la primera línea es
> `if (gAppSleepInfo != '\0') { CS_knlSetTimer(...,100,0,0); ...; return; }` — si `gAppSleepInfo` es no-cero,
> la función SOLO re-arma su propio timer y vuelve, sin llamar a `STATE_fpProcess()` (el dispatcher de
> update/draw por-escena que eventualmente llegaría a `GRP_Flush`) ni a `SOUNDSYSTEM_Process()`. Esto
> explicaría el patrón exacto observado: un timer que tiquetea para siempre sin hacer nada más.
>
> `gAppSleepInfo` se pone en 1 SOLO en `pauseClet()` (`:24894`) y se limpia a 0 en `resumeClet()` (`:24847`,
> incondicional), en `DelegateCall_startClet()` (`:24990`, dentro de la cadena de `startClet()` que corre
> DENTRO de `nativeInit()`), y en `destroyClet()` (`:24762`). Cruzando esto contra
> `decompiled_apk/sources/com/com2us/wrapper/WrapperDirtyRenderer.java` (la clase real que usa Inotia 3,
> Corrida 31): tanto `onResume()` (línea 121: `if (this.isPaused) { ... }`) como `onResumeClet()` (línea
> 156: `if (this.isPaused) { ... }`) **solo disparan la cadena que llega a `nativeResume`/`nativeResumeClet`
> si `isPaused` es `true`** — falso en un arranque limpio que nunca pasó por pausa, así que en Android real
> **ninguno de los dos se invoca en el primer arranque**; `gAppSleepInfo` simplemente queda en el `0` que
> `DelegateCall_startClet()` ya puso dentro de `nativeInit()`. **`source/main.c`, en cambio, llama a
> `nativeResume(&jni, NULL); nativeResumeClet(&jni, NULL);` incondicionalmente, en TODOS los arranques,
> primer arranque incluido** — un camino que Android real nunca ejecuta en esta situación. Si esa secuencia
> deja `gAppSleepInfo` en 1 en vez de 0 (o si `nativeResumeClet()` no mapea 1:1 a `resumeClet()` como se
> asume), esto explicaría un stall permanente exactamente con esta forma.
>
> **Instrumentación agregada** (`source/patch.c`): `MainProcess` (offset exportado en `.dynsym`,
> `so_symbol()`, sin offset fijo) hookeado transparentemente (`SO_CONTINUE_VOID`) para loguear el valor real
> de `gAppSleepInfo` (también resuelto por símbolo, `.bss` de 1 byte) al principio de cada una de sus
> primeras 20 invocaciones. Build compilado y verificado (cross real vía VitaSDK,
> `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-08-04) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log. Buscar `[MAINPROC] MainProcess() called` —
> - Si `gAppSleepInfo=1` en TODAS las invocaciones, confirma la hipótesis: el fix sería no llamar a
>   `nativeResume()`/`nativeResumeClet()` en el arranque inicial (solo deberían dispararse ante un pause/
>   resume real más adelante, si este port llega a implementar esa transición) — probar removiendo esas dos
>   líneas de `main.c` y ver si el juego avanza.
> - Si `gAppSleepInfo=0`, la hipótesis está descartada y el corte está en otro lado dentro de `MainProcess`
>   (revisar `STATE_fpProcess`/`STATE_nState`/`GAMESTATE_nState` — qué controla si hay un "estado" real
>   asignado, y si `STATE_fpProcess` es NULL por algo que `startClet()`/`StartCletProcess()` no terminó de
>   inicializar en este port).

> **Corrida 33 (2026-08-04/05) — `gAppSleepInfo=0` confirmado en las 20 invocaciones logueadas (hipótesis de
> la Corrida 32 descartada) — pero la lectura completa de `MainProcess` reveló la causa real: NADA llama
> jamás a `STATE_Set()`, así que `STATE_fpProcess` queda en NULL para siempre y el dispatch por-escena
> nunca corre. Fix aplicado, compilado, pendiente de confirmar en consola.** `logs/log_2026-08-04_23-51-15.
> txt`: `[MAINPROC] ... gAppSleepInfo=0` en las 20 líneas — el corte NO es ahí.
>
> Leyendo `MainProcess` (`out_ghidra.c:25352`) después del check de `gAppSleepInfo`: drena
> `INPUTQUEUE_Get`, y en cada vuelta (incluso sin input) llama `if (STATE_fpProcess != (code*)0x0)
> (*STATE_fpProcess)()` — es decir, el dispatcher por-escena que eventualmente llamaría a `GRP_Flush` SOLO
> corre si `STATE_fpProcess` es no-nulo. `STATE_fpProcess`/`STATE_nState` son globales `.bss` (confirmado
> con `objdump -T`) — arrancan en `0`/`NULL` hasta que algo llame a `STATE_Set()` (`:24479`, la máquina de
> estados: switch por id, asigna `STATE_fpEnter/fpExit/fpProcess/fpPressKey` y al final SIEMPRE invoca
> `(*pcVar1)()`, el `Enter` del estado nuevo). **Un `grep` de TODOS los call sites de `STATE_Set(` en las
> 214k líneas del `.so` no encuentra ninguno para el id `0` ("Init") salvo dentro de `UserAgreeCallback`
> (`:24932-24940`, solo si `param_1==0`)** — y `UserAgreeCallback` en sí solo se invoca desde quien llame a
> `Java_com_com2us_module_activeuser_ActiveUser_jniUserAgreeNotifier` (`:141236`), un export JNI que espera
> que un módulo Java de Com2uS ("ActiveUser"/UserAgree, parte del mismo sistema de Hub/social que
> `CLAUDE.md` ya marca como no-op-hasta-probar-necesario) lo llame de vuelta con el puntero a función
> previamente registrado.
>
> Cruzando contra `startClet()` (`:24998-25038`, corre DENTRO de `nativeInit()`): registra ese callback vía
> `CS_UserAgreeSetCallback(UserAgreeCallback+1)`, y SOLO muestra una UI real (`CS_UserAgreeShowUI()`) si el
> idioma del dispositivo es coreano (`strcmp(lang,"kor")==0`) — **para cualquier otro idioma (este build:
> `com.com2us.inotia3.normal.freefull.google.global`, confirmado no-coreano), depende ENTERAMENTE de que
> ese sistema de módulos externo dispare el callback por su cuenta, sin ninguna UI**. Este port no tiene esa
> capa de módulos (ni Hub ni ActiveUser reales) — así que ese callback simplemente nunca se dispara, y
> `STATE_fpProcess` se queda en `NULL` para siempre. Esto explica exactamente el patrón observado desde la
> Corrida 28: el timer principal tiquetea estable cada 100ms sin fallar nunca, pero no pasa nada más — el
> único frame real dibujado (`MainMenu.png`/`GRP_Flush` de las Corridas 30-32) se dibujó desde OTRO código de
> init anterior a esto (una pantalla de carga hardcodeada, no gateada por `STATE_*`), no desde la máquina de
> estados — que nunca llegó a arrancar.
>
> **Fix aplicado** (`source/patch.c`): hook transparente sobre `CS_UserAgreeSetCallback` (exportado en
> `.dynsym`) que llama a la función real (`SO_CONTINUE_VOID`) y además guarda el puntero al callback
> registrado en `g_user_agree_callback`, sin invocarlo todavía. Nueva función `patch_fire_user_agree_
> callback()` que lo invoca con argumento `0` ("de acuerdo"/sin error) — llamada UNA VEZ desde
> `source/main.c`, después de que `nativeResume()`/`nativeResumeClet()` retornan (es decir, después de que
> `startClet()` terminó TODA su inicialización, incluyendo `CS_ActiveUserStart()`/news banner/registro de
> `g_MainTimer` — deliberadamente NO desde dentro del hook mismo, para no adelantar el primer `Enter` de la
> máquina de estados a un punto donde esas otras piezas todavía no existen). Build compilado y verificado
> (cross real vía VitaSDK, `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-08-05) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log. Buscar `[STATE] firing UserAgree callback(0)` (debe
> aparecer una vez, después de "entering render loop" YA NO debería ser cierto que solo aparece un
> `[FLUSH]` — si la máquina de estados arranca de verdad, debería haber MÁS invocaciones de `GRP_Flush`/
> `Flush()` a medida que el juego transiciona de estado (Init → Logo → Hub/Title → Menu). Si sigue
> apareciendo un solo `[FLUSH]` a pesar de que el callback se dispara, el próximo sospechoso es que
> `STATE_EnterInit`/`STATE_ProcessInit` (el primer estado real) tenga su propia condición de espera interna
> (revisar su cuerpo en `out_ghidra.c` directamente) — si nunca aparece `[STATE] firing...` en absoluto,
> revisar el warning `Corrida 33: startClet() never registered a UserAgree callback` (confirmaría que
> `CS_UserAgreeSetCallback` no se llamó como se asumió, y hay que reabrir la traza desde `startClet()`).

> **Corrida 34 (2026-08-05) — ¡El fix de la Corrida 33 funciona! La máquina de estados arrancó de verdad
> (segundo `[FLUSH]` real, nunca visto en 33 Corridas previas) y el juego avanza de la pantalla de carga a
> "Title" — pero crashea ahí con un bug nuevo, más profundo: el `length` usado para mmapear `MainMenu.png.
> jpg` en su SEGUNDA apertura sale contaminado con el `length` de `Title.png`, truncando el archivo y
> rompiendo el CRC de libpng. Investigación en curso (fork dedicado).** `logs/log_2026-08-05_00-33-46.txt` +
> dump `Inotia3-psp2core-1785904438-...psp2dmp`: secuencia confirmada línea por línea —
> `[STATE] firing UserAgree callback(0)` → **`[FLUSH] GRP_Flush() called (#2)`** (la primera vez en toda la
> historia de este port que `GRP_Flush` se dispara más de una vez) → `entering render loop` → `MainProcess`
> tiquetea → el juego resuelve `"Title.png"` vía `GetFileDescriptor` (JNI, `length=110266` correcto) → **sin
> ninguna llamada JNI nueva**, reabre `MainMenu.png.jpg` con `open()`/`fstat()`/`close()`/`open()`/
> `mmap(0x0, 110266, ...)` — **el mismo 110266 de Title.png, no los 148229 reales de MainMenu.png** — trunca
> el archivo → `libpng`: `png_error(..., "IDAT: CRC error")` → `longjmp` hacia el stack frame ya retornado de
> `png_create_read_struct_2` (el riesgo ya documentado en la Corrida 19) → crash real (el `.psp2dmp`
> muestra `PC=0x9810ff2a`, muy por fuera del rango de cualquier módulo cargado — consistente con un salto a
> memoria basura después de ese `longjmp` a un frame muerto, no información nueva sobre la causa raíz).
>
> Rastreo parcial en `out_ghidra.c`: `CS_knlGetResource(resourceID, dest, destSize)` (`:157400`) busca un
> struct por **ID numérico** vía `ResourceManager::FindInformation` (NO por nombre de archivo) y pasa
> `path=+0x104`, `offset=+0x210`, `length=+0x214` de ESE struct directo a `MMappedFile::Open`
> (`:175057`) — que solo re-consulta el tamaño real (`File::GetSize()`) si el `length` que le pasan es
> `0`; si ya viene no-cero (como pasaría con un struct cacheado), lo usa tal cual sin refrescarlo NUNCA.
> Falta confirmar qué función popula ese struct (`ResourceManager::Register` o similar), si "MainMenu.png"
> y "Title.png" comparten el mismo slot/ID (pool chico reciclado) o si hay una variable global de "último
> length resuelto" compartida por error entre resoluciones — y si esto depende de que este port bypasea el
> formato `"@offset|length|path"` (passthrough literal de `File_GetFullPathName_hook` para paths con `:`)
> en vez de forzar SIEMPRE ese formato, que sí re-resuelve offset/length en cada apertura vía
> `ParseOffsetAndLengthAndPath`.
>
> **Causa raíz confirmada por el fork y fix aplicado**: NO es colisión de ID/slot (el ID de recurso es un
> contador secuencial simple, `ResourceManager::GetID()` `:176167` — MainMenu.png y Title.png tienen IDs
> distintos). Es una lectura de **memoria de stack sin inicializar** dentro del propio `.so`: `File::Open`/
> `File::GetFullPathName` (`:175020-175028`, `:175130+`) solo escriben los campos internos de offset/length
> que `File::GetLength()` lee cuando el path resuelto empieza con `'@'` (formato `"@offset|length|path"`,
> parseado por `ParseOffsetAndLengthAndPath`) — **si el path NO tiene ese prefijo, esos campos nunca se
> escriben y quedan con lo que sea que había antes en esa región de stack** (un `File` local dentro de
> `ResourceManager::Add`/`MMappedFile::Open`). Nuestro propio `File_GetFullPathName_hook` (`source/
> patch.c`, Corrida 21) hace passthrough LITERAL para cualquier path con `':'` (todo asset suelto en
> `ux0:...`) — nunca produce el formato `'@...'` — así que esos campos JAMÁS se refrescan para assets
> sueltos. La primera carga de MainMenu.png tuvo suerte (esa región de stack estaba en `0`, y
> `MMappedFile::Open` solo refresca via `File::GetSize()` cuando el length recibido es exactamente `0` —
> `:175092-175094` — así que cayó en el fallback correcto); después de resolver `Title.png` (misma
> profundidad de pila), esa misma región quedó con SU length (`110266`), y la siguiente apertura de
> MainMenu.png leyó esa basura no-cero directo, sin refrescar nada. **En Android real todo recurso vive en
> el mismo `.apk`, así que SIEMPRE resuelve a un path `'@offset|length|path'` — este bug de stack del `.so`
> es real pero estructuralmente inalcanzable ahí**; es un bug latente que el propio passthrough de este
> port expuso por primera vez.
>
> **Fix aplicado** (`source/patch.c`, `File_GetFullPathName_hook`): la rama de passthrough (paths con `':'`
> o `'/'`) ahora formatea igual que la rama `mode==2` — `"@0|<length real via stat()>|<path>"` — en vez de
> devolver el path literal. Esto fuerza a que `File::Open` SIEMPRE tome la rama `'@'` y refresque offset/
> length en CADA apertura, igual que ocurre naturalmente en Android real, sin tocar código del `.so`
> (`ResourceManager`/`MMappedFile`). Build compilado y verificado (cross real vía VitaSDK,
> `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-08-05) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log (+ `.psp2dmp` si vuelve a crashear). Si el fix
> funciona, la transición Title→Menu ya no debería crashear con "IDAT: CRC error" y deberían aparecer más
> `[FLUSH]`/sprites reales en pantalla a medida que el juego avanza de estado.

> **Corrida 35 (2026-08-06) — El fix de la Corrida 34 NO funcionó: mismo crash exacto en un binario
> reconstruido que sí lo incluye. Instrumentación en runtime agregada para reemplazar la teoría de análisis
> estático por datos reales.** `logs/log_2026-08-05_00-33-46.txt` (+ su casi-duplicado
> `log_2026-08-05_00-55-35.txt`, idéntico salvo un timestamp de timer) + dumps
> `Inotia3-psp2core-1785904438-...psp2dmp` (build viejo, el mismo que documentó la Corrida 34) y
> `Inotia3-psp2core-1785905746-...psp2dmp` (build NUEVO, confirmado por el propio tamaño de `inotia3`
> cambiando de `0xbefd4` a `0xbf014` — sí incluye el fix de `File_GetFullPathName_hook`): **ambos dumps son
> el mismo crash byte a byte** (`PC=0x9810ff2a`, mismo `LR`, mismo stack trace completo, solo difieren en
> unos pocos punteros de ASLR/carga de módulos). El log confirma el mismo patrón ya visto: `MainMenu.png.jpg`
> se reabre sin ninguna llamada JNI nueva, primero con el `length` de `font.fnt` (3960) pegado, después con
> el de `Title.png` (110266) pegado — mismo "IDAT: CRC error" → mismo `longjmp` a un frame ya retornado de
> `png_create_read_struct_2` → mismo crash.
>
> Se lanzó un análisis de desensamblado real (`arm-vita-eabi-objdump -M force-thumb`, no solo pseudo-C) de
> toda la cadena: `ResourceManager::Add` (`_ZN15ResourceManager3AddEPKcPm`, `0x1012ac`) → `File::Open`
> (`_ZN4File4OpenEPKci`, `0xff4b8`) → `File::GetFullPathName` 3-arg (`_ZN4File15GetFullPathNameEPKcPcm`,
> `0xff3c0`, que internamente llama a la 4-arg que SÍ hookeamos, `0xff038` = `OFF_File_GetFullPathName`,
> primero con `mode=3` y si falla con `mode=2`) → `ParseOffsetAndLengthAndPath` (`0xfea24`) →
> `File::GetOffset`/`GetLength` (`0xfeaf4`/`0xfeafc`, leen `this+0x10C`/`this+0x110`). **Confirmado: el hook
> SÍ se invoca (el `name` siempre contiene `:`, entra por la rama de passthrough en ambos intentos de mode) y
> `ParseOffsetAndLengthAndPath` SÍ escribe `this+0x10C`/`+0x110` explícitamente a partir del string
> `"@0|<len>|<path>"` — la teoría de la Corrida 34 ("lectura de stack sin inicializar") no sobrevive al
> desensamblado real: con el fix, esos campos ya no dependen de basura de ningún lado.** Esto contradice
> directamente el síntoma observado en consola — análisis estático llegó a su límite.
>
> **Instrumentación agregada** (`source/patch.c`): hooks transparentes (llaman a la función real, no cambian
> comportamiento) sobre `ResourceManager::Add(char const*, ulong*)` (el registro/cacheo por nombre que usa
> `CS_knlGetResourceID`) y `MMappedFile::Open` (lo que `CS_knlGetResource` usa para mmapear de verdad),
> logueando `name`/`length` de entrada y salida de cada uno. Esto va a mostrar en el próximo log si
> `"MainMenu.png.jpg"` se cachea con el length YA MAL desde el registro inicial, o si `CS_knlGetResource`
> le pasa a `MMappedFile::Open` el struct cacheado de OTRO recurso (aliasing) al momento de mmapear. Build
> compilado y verificado (cross real vía VitaSDK, `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`,
> 2026-08-06) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir (llegar hasta el crash de Title→Menu), mandar el log nuevo. Buscar
> las líneas `[RESMGR] Add(name="...")` y `[MMAP] MMappedFile::Open(path="...", ...)` para las tres
> resoluciones de recurso en juego (`MainMenu.png.jpg`, `font.fnt.jpg`, `Title.png.jpg`) — si el `[RESMGR]
> Add` de MainMenu ya muestra `length` correcto (148229) en su primera y ÚNICA invocación (no debería volver
> a llamarse para el mismo nombre) pero el `[MMAP] MMappedFile::Open` posterior muestra un `length` distinto
> al que ese mismo `Add` cacheó, confirma aliasing/corrupción del struct `ResourceInformation` entre el
> registro y el uso. Si en cambio el `[MMAP]` siempre coincide con lo que su `[RESMGR] Add` correspondiente
> cacheó, el bug está en OTRO lado todavía no instrumentado (revisar si hay más de un resourceID para el
> mismo nombre lógico, o si el struct de 0x218 bytes que `operator_new` reserva colisiona con otro por un
> bug de heap).

> **Corrida 36 (2026-08-06) — Los logs `[RESMGR]`/`[MMAP]` de la Corrida 35 dieron el dato clave: NO es el
> `path` el que se corrompe (siempre es correctamente "MainMenu.png.jpg"), es el `length` cacheado el que
> pasa a valer EXACTAMENTE el del recurso que se acaba de registrar justo antes -- demasiado preciso para
> ser basura de memoria genérica.** `logs/log_2026-08-06_20-02-04.txt` +
> `Inotia3-psp2core-1786060936-...psp2dmp` (mismo crash de siempre, `PC=0x9810ff2a`, confirma que las dos
> instrumentaciones nuevas no cambiaron el comportamiento, como se esperaba):
> ```
> [RESMGR] Add(name="MainMenu.png") -> id=1 length=148229                     <- correcto, registro inicial
> [MMAP]   MMappedFile::Open(path="...MainMenu.png.jpg", length=148229, ...)  <- correcto, primer uso
> ...
> [RESMGR] Add(name="font.fnt") -> id=2 length=3960                           <- correcto, registro de OTRO recurso
> [MMAP]   MMappedFile::Open(path="...MainMenu.png.jpg", length=3960, ...)    <- BUG: path de MainMenu, length de font.fnt
> ...
> [RESMGR] Add(name="Title.png") -> id=3 length=110266                        <- correcto, registro de OTRO recurso
> [MMAP]   MMappedFile::Open(path="...MainMenu.png.jpg", length=110266, ...)  <- BUG: path de MainMenu, length de Title.png -> CRC error -> crash
> ```
> El propio `Add()` de cada recurso (font.fnt, Title.png) devuelve SU length correcto para SÍ MISMO --
> el bug no está en el registro. Es la llamada a `MMappedFile::Open` INMEDIATAMENTE SIGUIENTE la que sale
> mal: usa el `path` de MainMenu.png (id=1) pero el `length` del recurso que se acaba de registrar (id=2 o
> id=3). Esto descarta corrupción de memoria genérica (una lectura de basura no coincidiría 3/3 veces exacto
> con "el length del último Add()") y apunta a una de dos causas concretas: (a) `CS_knlGetResource` está
> siendo invocado con `resourceID=1` (MainMenu) pero el struct que `FindInformation(1)` devuelve tiene su
> propio campo `length` (`+0x214`) mutado in-place por el `Add()` de font.fnt/Title.png (aliasing real de
> memoria/heap); o (b) se está pidiendo el id NUEVO (2 o 3) pero `FindInformation` resuelve mal ese id y
> devuelve el struct de MainMenu (bug de lookup, no de escritura).
>
> **Instrumentación agregada** (`source/patch.c`): hook transparente sobre `CS_knlGetResource` (símbolo
> exportado, no mangled) que loguea el `resourceID` que realmente recibe y, ANTES de dejar correr la función
> real, llama él mismo a `ResourceManager::FindInformation(theResourceManager, id)` (mismos símbolos reales,
> `theResourceManager` es un `ResourceManager*` global en `.bss`) para volcar la dirección cruda del struct
> devuelto y sus campos `path`/`offset`/`length` (`+0x104`/`+0x210`/`+0x214`) tal como están en ESE instante,
> antes de que `MMappedFile::Open` (ya hookeado en la Corrida 35) los use. Build compilado y verificado
> (cross real vía VitaSDK, `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-08-06) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log nuevo. Buscar las líneas `[GETRES]
> CS_knlGetResource(id=...)` para las tres llamadas que preceden a los `[MMAP]` ya vistos -- el dato
> decisivo es el `id` logueado en cada una: si SIEMPRE es `1` (MainMenu) incluso cuando el `length` sale
> mal, confirma la hipótesis (a) (el struct de MainMenu se mutó) y hay que buscar QUIÉN escribe
> `+0x214` fuera de `ResourceManager::Add`'s propio branch de creación (revisar `Remove`/`RemoveAll`/algún
> "update" no visto, o confirmar aliasing real comparando la dirección `info=%p` logueada entre la llamada
> de MainMenu y la de font.fnt/Title.png -- si ES LA MISMA dirección, es aliasing de `operator_new`/heap, no
> lógica); si el `id` logueado es el NUEVO (2 o 3) en vez de 1, confirma la hipótesis (b) y hay que revisar
> `ResourceManager::FindInformation(uint)` (`_ZN15ResourceManager15FindInformationEj`, `0xfff3c`) a nivel de
> desensamblado para ver por qué resuelve un id al struct equivocado.

> **Corrida 37 (2026-08-06) — Causa raíz REAL encontrada (ninguna de las hipótesis (a)/(b) de la Corrida 36):
> ni aliasing de heap ni bug de lookup -- son tres recursos con tres structs DISTINTOS (direcciones
> `info=` diferentes, confirmado), cada uno con SU PROPIO `length` correcto, pero los tres con el mismo
> `path` (el de MainMenu.png, el primer recurso jamás cargado). El bug está en nuestro propio
> `File_GetFullPathName_hook`, no en el `.so`. Fix aplicado.** `logs/log_2026-08-06_20-38-47.txt` +
> `Inotia3-psp2core-1786063138-...psp2dmp` (mismo crash de siempre, sin cambios de comportamiento por la
> instrumentación de la Corrida 36, como se esperaba):
> ```
> [GETRES] CS_knlGetResource(id=1) info=0x836834d8 path="...MainMenu.png.jpg" offset=0 length=148229  <- OK
> [GETRES] CS_knlGetResource(id=2) info=0x83684328 path="...MainMenu.png.jpg" offset=0 length=3960     <- BUG: id=2 (font.fnt), struct DISTINTO (0x83684328 != 0x836834d8), length correcto (3960=font.fnt), path INCORRECTO
> [GETRES] CS_knlGetResource(id=3) info=0x83685538 path="...MainMenu.png.jpg" offset=0 length=110266   <- BUG: id=3 (Title.png), struct DISTINTO, length correcto (110266=Title.png), path INCORRECTO
> ```
> Direcciones de struct DISTINTAS por id (`0x836834d8`/`0x83684328`/`0x83685538`) descartan aliasing de
> memoria/heap por completo. `resourceID` siempre es el correcto (1/2/3, no hay confusión de lookup). El
> `length` cacheado en cada struct SIEMPRE es el correcto para ESE recurso (viene de
> `CS_fsGetFileDescriptor_real(name,...)`, que dispara una llamada JNI `GetFileDescriptor` fresca cada vez
> -- confirmado, aparece una vez por recurso en el log: líneas 464/625/732). El `path` cacheado en CADA
> struct, en cambio, es SIEMPRE el de MainMenu.png.jpg -- el primer recurso resuelto -- sin importar qué
> recurso se está registrando. Un `grep` de `"GetApkFilePath"` en el log completo lo confirma sin ambigüedad:
> la llamada JNI `GetApkFilePath` aparece **UNA SOLA VEZ en todo el log** (línea 473, durante el registro de
> MainMenu.png) -- nunca vuelve a dispararse para `font.fnt` ni `Title.png`, aunque `GetFileDescriptor` SÍ se
> llama para cada uno.
>
> **Causa raíz**: `jGetApkFilePath()` (la función real del `.so`) cachea su valor de retorno la PRIMERA vez
> que se llama y nunca vuelve a preguntarle a Java (mismo mecanismo ya documentado en la Corrida 21, aplicado
> acá a un bug nuevo). `File_GetFullPathName_hook`'s rama `mode==2` (`source/patch.c`) construía el string
> `"@offset|length|path"` llamando a `jGetApkFilePath_real()` para el campo `path` -- válido en Android real
> (TODOS los recursos viven en el mismo `.apk`, así que reusar el mismo path para siempre es 100% correcto
> ahí), pero **inválido en este port**, donde cada recurso suelto tiene SU PROPIO archivo físico
> (`ux0:.../<nombre>.jpg`) -- exactamente lo que `GetFileDescriptor_stub` (`source/java.c`) ya resuelve
> correctamente para cada `name`, cacheándolo en `g_pending_asset_path`/`g_pending_asset_is_loose` con un
> mecanismo de "one-shot flag" pensado para ser consumido por la SIGUIENTE llamada a `GetApkFilePath_stub` --
> mecanismo que nunca se dispara para el 2do/3er recurso en adelante, precisamente porque el `.so` ya no
> vuelve a preguntar. `offset`/`length` sí llegan frescos cada vez porque vienen de
> `CS_fsGetFileDescriptor_real(name,...)` (dispara `GetFileDescriptor` vía JNI, sin cache alguno en el
> `.so`) -- de ahí que SOLO el `path` quedara pegado y `length` siempre fuera correcto, el patrón exacto
> observado.
>
> **Fix aplicado**: `source/java.c` gana `java_peek_current_asset_path()`, que replica la MISMA decisión que
> `GetApkFilePath_stub` ya toma (`g_pending_asset_is_loose ? g_pending_asset_path : INOTIA3_APK_PATH`) pero
> SIN el consumo de un solo uso -- de forma que se puede leer el path del recurso ACTUAL en cualquier
> momento, no solo la primera vez que alguien (el `.so`, que ya no lo hace) pregunte. `source/patch.c`'s
> `File_GetFullPathName_hook`, rama `mode==2`, ya NO llama a `jGetApkFilePath_real()` (offset
> `OFF_jGetApkFilePath`, eliminado por no usarse más) -- ahora llama a `java_peek_current_asset_path()`
> inmediatamente después de `CS_fsGetFileDescriptor_real(name, values, 1)` (que es quien, vía
> `GetFileDescriptor_stub`, acaba de dejar `g_pending_asset_path` fresco para ESTE `name` unas líneas antes,
> en la MISMA invocación) -- el path que se cachea para siempre en el struct de cada recurso ahora es el
> real y propio de ese recurso, no el de MainMenu.png. Build compilado y verificado (cross real vía
> VitaSDK, `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-08-06) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log (+ `.psp2dmp` si vuelve a crashear). Si el fix
> funciona, las líneas `[GETRES] CS_knlGetResource(id=2/3)` deberían mostrar `path="...font.fnt.jpg"` /
> `path="...Title.png.jpg"` respectivamente (no más `MainMenu.png.jpg` repetido), el `[MMAP]` correspondiente
> debería abrir el archivo correcto, y la transición Title→Menu ya no debería crashear con "IDAT: CRC error".

> **Corrida 38 (2026-08-06) — CONFIRMADO: el fix de la Corrida 37 funciona.** `logs/log_2026-08-06_20-59-13.
> txt`: `[GETRES] CS_knlGetResource(id=2) ... path="...font.fnt.jpg"` y `(id=3) ... path="...Title.png.jpg"`
> -- cada recurso ahora cachea SU PROPIO path real, ya no el de `MainMenu.png.jpg` pegado. Sin más "IDAT: CRC
> error" en el log. El usuario confirma que la pantalla de loading YA SE VE CORRECTAMENTE (no más pantalla
> negra) -- el bug perseguido desde la Corrida 19 queda cerrado.
>
> El juego avanzó ~10 ticks de `MainProcess` más (una animación de ícono girando, visible en los vértices
> rotando de `[GL] glDrawArrays`) y crasheó con un bug NUEVO y distinto: `Inotia3-psp2core-1786064379-
> 0x000bf03c65-eboot.bin.psp2dmp`, Data abort, `PC=0x980e4b0c`/`LR=0x980cc16b` -- usando la base real de
> `libgame.so` (`0x81708000`, confirmada porque resuelve varios valores de pila a símbolos reales:
> `SGL_Device_iOS_GL11::Flush()+0x8`, `SGL_GetGLSize+0x14`, `SGL_Font_Android::Release()+0x8`), tanto `PC`
> como `LR` caen en offsets absurdos (~377MB adentro de un `.so` que pesa 1.4MB) -- salto salvaje, no código
> real. El log se corta sin ninguna línea `fatal`/`ERR` (crash demasiado violento para sobrevivir ni la
> primera línea). Hipótesis a confirmar: `SGL_Font_Android::Release()` (`out_ghidra.c:160640`) hace un
> release con conteo de referencias que, al llegar a 0, invoca una función VIRTUAL leída de `*this`/vtable+4
> -- si `this` es un puntero colgante (use-after-free) o el vtable está corrupto, la llamada salta a basura,
> coincidiendo con el síntoma. Investigación en curso (fork dedicado) para confirmar o refutar esta hipótesis
> contra el desensamblado real antes de proponer un fix.

> **Corrida 39 (2026-08-06) — Hipótesis de la Corrida 38 (`SGL_Font_Android::Release`/vtable corrupto)
> REFUTADA: era un falso positivo de la propia herramienta de análisis (la base auto-detectada resolvía
> valores de PILA vieja a símbolos reales por pura coincidencia aritmética, no era la cadena de llamadas
> activa). Causa raíz real confirmada por desensamblado directo: un cuarto (en realidad quinto/sexto/séptimo)
> puntero `JavaVM*` cacheado sin inicializar, del mismo tipo de bug ya conocido y parcheado a medias.** Con
> la base REAL de `libgame.so` para este dump (`0x98000000`, confirmada porque con ella `PC`/`LR` caen
> dentro del segmento ejecutable real y `R6` resuelve exacto, sin offset, a `INITSTATE_LoadFrameCnt`):
> `PC = CS_ADSetGameResolution + 0x14`, `LR = startAd + 0x17`. Desensamblado de `CS_ADSetGameResolution`
> (`0xe4af8`):
> ```
> e4b06: ldr r0, [r4, #8]   ; r0 = DAT_0017de40 (copia cacheada de JavaVM*)
> e4b0c: ldr r3, [r0, #0]   ; *** CRASH: r0 == NULL ***
> ```
> `R0=0x00000000` en el dump confirma un NULL pointer dereference exacto -- no un salto salvaje. `startAd()`
> (arranque del subsistema de ads Mopub/Tapjoy -- ya marcado en `CLAUDE.md` como no-op-hasta-probar-
> necesario, pero SÍ se ejecuta en este port) llama `CS_ADSetGameResolution`, que lee su propia copia
> cacheada de `JavaVM*` (`DAT_0017de40`) antes de que nada la haya escrito.
>
> Un grep completo del patrón `(**(code**)(*env + 0x36c))(env, &DAT_xxx)` (llamada a `GetJavaVM` vía la
> vtable de `JNIEnv`, offset `0x36c`) en las 214k líneas del decompile encontró **7 copias distintas** de
> este puntero cacheado: `0x16dda4, 0x16de24, 0x16de40, 0x16de50, 0x16de58, 0x16de64, 0x16de70` -- de las
> cuales `source/patch.c` (fix original, commit `65190ae`) solo cubría 2 (`0x16de24`/`0x16de64`). Se
> confirmó leyendo `JNI_OnLoad` (`out_ghidra.c:144168-144175`, `DAT_0017de70 = param_1;`) que **`0x16de70`
> es la ÚNICA copia que el propio `JNI_OnLoad` real ya inicializa correctamente** (recibe el `JavaVM*` real
> que el loader le pasa) -- no hay que tocarla, forzarla pisaría un valor ya válido sin necesidad. Las otras
> 4 (`0x16dda4, 0x16de40, 0x16de50, 0x16de58`) nunca se inicializan en este port: están pensadas para
> poblarse de forma perezosa por el propio init de cada subsistema (Hub/ActiveUser/ads/etc.) la primera vez
> que se usan, pero este port no dispara esos init reales.
>
> **Fix aplicado** (`source/patch.c`): se extendió el bloque de inicialización manual de punteros
> `JavaVM*` (ya existente para 2 direcciones) a un array con las 6 direcciones que SÍ hace falta forzar
> (`0x16dda4, 0x16de24, 0x16de40, 0x16de50, 0x16de58, 0x16de64` -- deliberadamente sin `0x16de70`), todas
> apuntando a `&jvm`, igual que las 2 originales. Build compilado y verificado (cross real vía VitaSDK,
> `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-08-06) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log (+ `.psp2dmp` si vuelve a crashear). Si el fix
> funciona, la animación de loading debería seguir más allá de donde cortaba antes (crash en
> `CS_ADSetGameResolution`) sin un nuevo NULL deref en ninguna de estas 6 direcciones -- si aparece OTRO
> crash con el mismo patrón (`ldr rX, [rY, #0]` con `rY` en `0x16dXXX`) en una dirección NO cubierta acá,
> hay que volver a gregar el patrón `&DAT_0017dXXX` completo (puede haber más de 7 copias, o alguna en un
> rango de direcciones distinto) en vez de asumir que estas 6 son todas.

> **Corrida 40 (2026-08-06) — El fix de la Corrida 39 funcionó (sin más NULL deref en los punteros
> `JavaVM*`), pero el juego "quedó casi en el mismo lugar" según el usuario: crash NUEVO, un poco más
> adelante en la secuencia de arranque.** `logs/log_2026-08-06_21-28-04.txt` +
> `Inotia3-psp2core-1786066106-...psp2dmp`: Data abort. `PC=0x8108adfe` resuelve DIRECTO contra el `.elf`
> propio del loader (sin ambigüedad de base, es nuestro propio módulo) a `strcpy`. Con la base real de
> `libgame.so` (`0x98000000`, mismo valor que funcionó en la Corrida 39 -- el offset de `LR` cae dentro del
> tamaño real del archivo, 1.4MB, a diferencia de la base auto-detectada de la herramienta que seguía dando
> offsets absurdos), `LR = Extend_RequestNotice + 0xe9`. `R1` (el argumento `src` de `strcpy` en la
> convención AAPCS) es `0x00000000` en el dump -- **`strcpy(dst, NULL)`, un NULL pointer dereference
> exacto, no basura de pila**.
>
> `Extend_RequestNotice` (`out_ghidra.c:117698-117747`) arma una URL (`http://s.com2us.net/game/inotia3/
> notice.php?...` -- confirmado en el log, línea 1196, un servidor de Com2uS de 2013 casi seguro dado de
> baja) y hace:
> ```c
> pcVar3 = (char *)jRequestNotice(auStack_72c);
> strcpy(acStack_f2c,pcVar3);   // <- sin chequeo de NULL
> ```
> El log confirma la causa exacta en la línea siguiente a la URL: `GetStaticMethodID(env, ...,
> "GetNoticeHttp", "(Ljava/lang/String;)Ljava/lang/Object;"): not found` -- el método Java real detrás de
> `jRequestNotice` nunca estaba registrado en `source/java.c`, así que FalsoJNI le devuelve `NULL` por
> default (tabla de la skill `so-crash-triage`: retorno `Object` sin registrar es "depende" -- acá SÍ es
> peligroso, exactamente el patrón de "se desreferencia sin chequear `NULL`").
>
> **Fix aplicado** (`source/java.c`): nuevo método `{ 45, "GetNoticeHttp", METHOD_TYPE_OBJECT }` +
> `GetNoticeHttp_stub()`, que devuelve un string VACÍO (`""`) en vez de `NULL`. Fiel, no es un parche
> cosmético: `strcpy(buf, "")` deja `buf[0]='\0'`, y el propio `strtok(buf,"|")` que sigue devuelve `NULL`
> de forma natural (nada que tokenizar) -- lo que hace que el chequeo `pcVar3 != NULL && strcmp(pcVar3,
> "OK")==0` sea falso, el MISMO camino de "no hay noticia hoy" que produciría el servidor real si respondiera
> cualquier cosa distinta de `"OK|..."`. Build compilado y verificado (cross real vía VitaSDK,
> `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-08-06) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log (+ `.psp2dmp` si vuelve a crashear). Si el fix
> funciona, el juego debería seguir avanzando más allá de la pantalla de loading hacia el Title/Menu real.
> Si aparece un NUEVO crash de "método JNI no encontrado" seguido de un `strcpy`/deref sin chequeo similar,
> aplicar el mismo patrón (agregar el stub con un valor por-default fiel y seguro, no necesariamente vacío --
> depende de qué hace el caller con el resultado, ver tabla de riesgo de retorno JNI en la skill
> `so-crash-triage`).

> **Corrida 41 (2026-08-06) — El fix de la Corrida 40 funcionó (`GetNoticeHttp` ya no crashea, el boot avanza
> más lejos), pero el usuario reporta "sigue crasheando en el mismo lugar" -- en realidad es un bug NUEVO y
> distinto muy cerca en el tiempo/líneas de log, no el mismo.** `logs/log_2026-08-06_21-42-09.txt` +
> `Inotia3-psp2core-1786066958-...psp2dmp` (build confirmado nuevo por tamaño de `inotia3`,
> `0xbf794`/`0x2682b8`): el log confirma `GetStaticMethodID(..., "GetNoticeHttp", ...): 45` (ya SIN "not
> found" -- el fix de la Corrida 40 funciona) y 1415 líneas de log (vs. 1199 antes), avanzando bien más allá
> del punto anterior. Nuevo Data abort: `PC` cae dentro de `SceLibKernel` (código real de Sony, no del
> `.so`) -- según la metodología `so-crash-triage`, cuando `PC` cae en un módulo de Sony hay que mirar `LR`.
> Con la base real de `libgame.so` (`0x98000000`, mismo valor que ya funcionó en Corridas 39/40 -- el offset
> de `LR` cae dentro del tamaño real del archivo): `LR = GRP_PutAsciiChar + 0x59`, exactamente la
> instrucción siguiente a un `blx memcpy@plt` (confirmado por desensamblado con
> `arm-vita-eabi-objdump -M force-thumb`).
>
> `GRP_PutAsciiChar` (`out_ghidra.c:20971-20999`) hace `memcpy(FONT_pFontBuffer, (void*)(FONT_pFontData +
> 0xb10 + uVar1*0xc), 0xc)` -- si `FONT_pFontData` es `NULL`, el puntero fuente resultante es una dirección
> chica sin mapear. **Los registros del dump lo confirman con precisión aritmética exacta**: `R1=0x00000d38`
> y `R3=0x00000228` -- `0xb10 (2832) + 0x228 (552, =46*0xc) = 0xd38`, EXACTAMENTE el valor de `R1` -- prueba
> que `FONT_pFontData` vale `0` y el cálculo se hizo sobre eso. `FONT_pFontData` (`.bss`, offset real
> `0x56f4d0`, arranca en `NULL`) solo se escribe en `FONT_Load()` (`out_ghidra.c:19844-19851`,
> `RES_LoadToPool("font.fnt",&FONT_pFontData,0)`) -- que a su vez solo escribe si `RES_LoadToHeap` tiene
> éxito, la MISMA cadena `CS_knlGetResourceID`/`CS_knlGetResource` que las Corridas 34-40 ya confirmaron
> funcionando para `"font.fnt"` como recurso (textura). `FONT_Load()` en sí solo se llama desde
> `StartCletProcess()` (`out_ghidra.c:24944-24979`), que a su vez solo corre si `GRP_Initialize()`
> (`out_ghidra.c:21212`) devuelve éxito -- un subsistema gateado por `CS_grpGetScreenFrameBuffer(0)` que NO
> es necesariamente el mismo camino ya confirmado funcionando (CSOGL, vía `[GL]`/`[FLUSH]` en el log). No hay
> evidencia todavía de si `FONT_Load()` corre y falla, o nunca corre -- especular más allá de esto sin datos
> es exactamente lo que la metodología de este archivo viene evitando desde la Corrida 1.
>
> **Instrumentación agregada** (`source/patch.c`): hook transparente sobre `FONT_Load` (exportado,
> `0x6c550`) que loguea su valor de retorno Y el valor resultante de `FONT_pFontData` justo después de
> llamar a la función real -- va a decir de una sola pasada si `FONT_Load()` corre en absoluto, si
> `RES_LoadToPool`/`RES_LoadToHeap` tienen éxito para `"font.fnt"` en este contexto (aunque ya sabemos que
> `CS_knlGetResource` funciona para el mismo nombre en otro contexto), y si `FONT_pFontData` termina no-nulo.
> Build compilado y verificado (cross real vía VitaSDK, `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`,
> 2026-08-06) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log nuevo. Buscar la línea `[FONT] FONT_Load() -> ...,
> FONT_pFontData=...`: si NO aparece en absoluto, `StartCletProcess`/`GRP_Initialize` nunca llega a esa
> línea -- hay que instrumentar `GRP_Initialize` (o `CS_grpGetScreenFrameBuffer`) para saber por qué; si
> aparece con `FONT_pFontData=(nil)` (retorno `0`/falla), el problema está en `RES_LoadToHeap` para este
> nombre específico en este momento del boot -- revisar si compite con otro registro de `"font.fnt"` bajo un
> ID distinto; si aparece con `FONT_pFontData` no-nulo, el bug real está en OTRO lado (offset mal calculado
> en `GRP_PutAsciiChar`, o el crash real es en una llamada DISTINTA a `memcpy` que la que asumimos).

> **Corrida 42 (2026-08-06) — Causa raíz real encontrada y arreglada: un bug de signo en el propio código
> del juego (`RES_LoadToPool`), latente en Android real y estructuralmente inevitable en este port porque
> TODAS nuestras direcciones de heap tienen el bit de signo puesto.** El hook de la Corrida 41 confirmó
> `FONT_Load() -> 0, FONT_pFontData=0x0` -- SÍ corre, y `RES_LoadToPool` SÍ falla, a pesar de que
> `CS_knlGetResource` ya funciona para `"font.fnt"` en otro contexto (textura).
>
> Leyendo `RES_LoadToPool`/`RES_LoadToHeap` completos (`out_ghidra.c:23378-23516`): en el camino de ÉXITO,
> `RES_LoadToHeap` devuelve literalmente lo que `CS_knlCalloc(length)` devolvió -- y `CS_knlCalloc`
> (`out_ghidra.c:157643`, `void CS_knlCalloc(size_t) { calloc(1,param_1); }`) es un passthrough directo a
> `calloc()` real (Ghidra perdió el tipo de retorno, pero el puntero viaja en `r0` igual, confirmado porque
> TODOS los callers lo tratan como puntero). `CS_GETDPTR` (`out_ghidra.c:157108`, desensambla a un `bx lr`
> puro) es un no-op vestigial de alguna plataforma anterior basada en handles -- acá NO convierte nada, solo
> devuelve el mismo puntero.
>
> El chequeo de éxito de `RES_LoadToPool` es `if (-1 < iVar1)` sobre ese valor, interpretado como `int` con
> signo. Es correcto SOLO si el bit más alto del puntero real está en `0` -- cierto en un heap de Android
> real (bien por debajo de `0x80000000`), por eso este código de 2013 nunca disparó esto ahí. **Pero TODAS
> las direcciones de heap que este port produjo en cualquier dump de esta conversación
> (`0x81xxxxxx`/`0x83xxxxxx`/`0x98xxxxxx`/...) tienen ese bit puesto** -- son negativas como `int` con
> signo -- así que el chequeo falla SIEMPRE acá, `RES_LoadToPool` devuelve eso (leído como error) sin
> escribir nunca `*param_2`, y `FONT_Load()` ve "falla" a pesar de que el load subyacente adentro de
> `RES_LoadToHeap` sí tuvo éxito instantes antes. **El mismo idioma `-1 < iVar1`/`iVar1 < 0` sobre el
> resultado de `RES_LoadToHeap` aparece también en `RES_MoveToFilePack` (`:23424`) y
> `EXCELDATA_LoadFullResource` (`:58509-58510`)** -- no confirmado crasheando todavía (ninguna de las dos
> está probada como alcanzada en la secuencia de arranque de este port hasta ahora), pero es el MISMO patrón
> de riesgo contra la MISMA función real -- si cualquiera de las dos llega a crashear más adelante, aplicar
> este mismo fix en vez de re-derivarlo desde cero.
>
> **Fix aplicado** (`source/patch.c`): se reimplementó `RES_LoadToPool` fielmente (misma lógica que
> `out_ghidra.c:23489-23516`) pero distinguiendo los 3 códigos de error explícitos (`-1`/`-2`/`-3`) de
> "cualquier otro valor = un puntero real", en vez de confiar en el bit de signo. Llama directo a las
> funciones reales `RES_LoadToHeap`/`MEM_Malloc`/`CS_knlFree` (exportadas, sin modificar) -- solo cambia el
> chequeo de éxito/fracaso. `RES_LoadToPool` tiene EXACTAMENTE un solo call site en las 214k líneas del
> decompile (`FONT_Load` en sí), así que no puede afectar ningún otro camino de código. Build compilado y
> verificado (cross real vía VitaSDK, `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-08-06) — sin
> errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log (+ `.psp2dmp` si vuelve a crashear). Si el fix
> funciona, debería aparecer `[FONT] FONT_Load() -> 1, FONT_pFontData=<dirección no-nula>` en el log, y ya no
> debería crashear en `GRP_PutAsciiChar`/`memcpy`. Si aparece un crash nuevo con el mismo patrón (`-1 <
> iVarX` o `iVarX < 0` sobre un resultado de `RES_LoadToHeap`) en `RES_MoveToFilePack` o
> `EXCELDATA_LoadFullResource`, aplicar el mismo fix (reimplementar fielmente, distinguir los códigos de
> error explícitos del resto).

> **Corrida 43 (2026-08-06) — El fix de la Corrida 42 funciona (elimina el falso positivo del bit de signo),
> pero expone una falla REAL y distinta: `RES_LoadToHeap("font.fnt")` ahora falla con el código de error
> genuino `-2` (`CS_knlGetResource` devolvió no-cero). Además, se encontró y arregló un bug real en nuestra
> PROPIA instrumentación que pudo estar enmascarando/corrompiendo este dato desde la Corrida 36.**
> `logs/log_2026-08-06_23-29-23.txt` + `Inotia3-psp2core-1786073388-...psp2dmp` (mismo crash de siempre en
> `GRP_PutAsciiChar`, sin cambios -- esperado, ya que el 42 solo arregla el falso positivo, no la falla
> real): `Corrida 42: RES_LoadToHeap("font.fnt") failed with real error code -2` seguido de `[FONT]
> FONT_Load() -> 0, FONT_pFontData=0x0` -- confirma que el código de error YA NO es el resultado de leer mal
> el bit de signo (el 42 filtra explícitamente -1/-2/-3), sino un -2 genuino: `CS_knlGetResource` devolvió
> no-cero para esta llamada específica, a pesar de que el `[MMAP] MMappedFile::Open(...)` inmediatamente
> anterior (mismo path/offset/length ya usados con éxito para este mismo recurso en corridas anteriores como
> textura) se ve normal en el log, con `mmap(...)`/`close(...)` completando sin error aparente a nivel de
> syscall.
>
> Antes de confiar en ese "-2" como dato limpio, se encontró un problema en la propia instrumentación: el
> hook de `CS_knlGetResource` (Corrida 36, `source/patch.c`) usaba `SO_CONTINUE_VOID` -- pero
> `CS_knlGetResource` (`out_ghidra.c:157400`) SÍ devuelve un valor real en `r0` (Ghidra lo tipó `void` por
> error; `RES_LoadToHeap` lo usa explícitamente: `iVar1 = CS_knlGetResource(...); if (iVar1 == 0)`). Un hook
> `void` no tiene ninguna garantía del lenguaje C de que `r0` siga conteniendo el valor de la llamada real
> para cuando el hook retorna -- funcionaba por accidente de codegen, no por diseño. Se cambió a
> `SO_CONTINUE(int, ...)` con retorno explícito, eliminando ese riesgo (confirmado que no era la causa raíz
> de ESTE bug puntual, pero es una corrección real y necesaria de todos modos -- cualquier otro caller que
> dependa del valor de retorno de `CS_knlGetResource` ahora lo recibe garantizado).
>
> **Instrumentación agregada** (`source/patch.c`): además del fix de retorno de `CS_knlGetResource_hook`
> (ahora también loguea `-> %d`), se extendió `MMappedFile_Open_hook` (Corrida 35) para loguear su propio
> valor de retorno Y los dos campos crudos que `MMappedFile::Open` (`out_ghidra.c:175060`) escribe al
> mapear (`this+0x108`, el puntero crudo de `mmap()`, nuleado si `mmap()` devolvió literalmente `(void*)-1`;
> `this+0x110`, el puntero ajustado que `GetPointer()` lee después) -- esto va a decir en una sola pasada si
> el `mmap()` real de este port devuelve algo que `MMappedFile::Open` interpreta como fallo, o si el fallo
> está en otro lado todavía no instrumentado dentro de `CS_knlGetResource` (la rama `PackageDecoder::Decode`
> si `type==1`, aunque no debería aplicar acá con `type=0`). Build compilado y verificado (cross real vía
> VitaSDK, `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-08-06) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log nuevo. Buscar las líneas `[MMAP]
> MMappedFile::Open(...) -> ...` y `[GETRES] CS_knlGetResource(id=2) -> ...` para la carga de `"font.fnt"`
> que precede al `[FONT] FONT_Load()` -- si `MMappedFile::Open` ya devuelve `0` (fallo) ahí, el problema está
> en el `mmap()`/`File::Open` reales para ESTE call site específico (revisar `source/reimpl/mem.c`); si
> `MMappedFile::Open` devuelve éxito pero `CS_knlGetResource` sigue devolviendo no-cero, el problema está en
> la rama `PackageDecoder::Decode`/`type` u otra condición del `if` compuesto de `CS_knlGetResource`
> (`out_ghidra.c:157417-157418`) que todavía no se instrumentó directamente.

> **Corrida 44 (2026-08-06/07) — El fix de la Corrida 43 funcionó del todo: `FONT_Load() -> 1,
> FONT_pFontData=0x9858a264` (éxito real), sin más crash en `GRP_PutAsciiChar`. Pero aparece un bloqueo
> nuevo, distinto: un método JNI sin registrar que hace que el propio wrap de texto nativo del juego gire
> cientos de veces sin converger -- no es técnicamente un loop infinito (termina, eventualmente, en un
> tiempo N² absurdo para textos largos), pero en la práctica se percibe como colgado en la pantalla de
> loading.** `logs/log_2026-08-06_23-49-26.txt` (11171 líneas, ~600KB, sin crash ni dump -- el juego sigue
> "vivo", solo sin progresar): confirma `[FONT] FONT_Load() -> 1, FONT_pFontData=0x9858a264` y `[GETRES]
> CS_knlGetResource(id=2) -> 0` (éxito genuino, ya no `-2`) -- cierra la Corrida 43 sin ambigüedad. Pero el
> log muestra `GetStaticMethodID(..., "GetStringWidthHeightInRect", "(Ljava/lang/String;IIII[I)I"): not
> found` repetido **766 veces** en una sola tanda, sin que el estado del juego avance (`MAINPROC` solo tiquetea
> 20 veces en las 11171 líneas -- comparado con las corridas previas donde cada tick trae progreso real).
>
> Leyendo la implementación Java real (`decompiled_apk/sources/com/com2us/wrapper/WrapperJinterface.java:
> 996-1110`): `GetStringWidthHeightInRect(String, fontSize, fontStyle, rectWidth, isBreak, int[] value)` usa
> `Paint.measureText` de Android para envolver texto línea por línea dentro de `rectWidth`, con una búsqueda
> binaria interna por línea, escribe `value[0]`=ancho máximo/`value[1]`=alto total acumulado, y devuelve
> `value[0]`. Sin registrar, FalsoJNI devuelve `0` siempre ("todo entra") -- lo que hace que el algoritmo de
> word-wrap NATIVO que llama a esto repetidamente (para textos largos como `memorytext_en.dat`, ~109KB de
> texto de lore, confirmado cargándose bien un poco antes en el mismo log) nunca encuentre un corte de línea
> real, reintentando cientos de veces sin converger a nada útil.
>
> **Fix aplicado** (`source/java.c`): se registró `GetStringWidthHeightInRect_stub` con un word-wrap
> greedy propio, O(n) en un solo pase (no busca binaria repetida, que sería O(n²) e inviable para un texto
> de 109KB) -- usa la MISMA tabla de costo por-byte que el sistema de fuente bitmap PROPIO del juego ya usa
> en otro lado, `FONT_GetSubStringWidth` (`out_ghidra.c:19780-19828`, confirmado por desensamblado:
> espacio=4px, byte con bit alto/multibyte=9px consumiendo 2 bytes, cualquier otro byte=6px) -- replicada
> directamente en C (no se llama al `.so`) porque es una tabla trivial y autocontenida, sin dependencia de
> `FONT_pFontData` ni de ningún estado interno del `.so`. Prefiere cortar en el último espacio visto en la
> línea actual (igual que el criterio de límite de palabra de la implementación Java real) cuando hay uno
> disponible. `fontSize`/`fontStyle`/`isBreak` se ignoran -- la fuente bitmap de este port tiene una métrica
> fija por byte, no escalable, y ambos modos reales de `isBreak` terminan envolviendo por ancho con
> preferencia de espacio en blanco de todos modos. Build compilado y verificado (cross real vía VitaSDK,
> `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-08-07) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log (+ `.psp2dmp` si crashea). Si el fix funciona, ya no
> debería aparecer un número absurdo de `GetStringWidthHeightInRect` seguidas sin progreso, y el juego
> debería seguir avanzando más allá de la pantalla de loading. Nota aparte: `WrapperJinterface.
> DrawTextInRect` (`([BIIIII)I`) también aparece sin registrar en el mismo log (una sola vez, no en loop) --
> es lo que dibuja de verdad el texto envuelto; si el juego avanza pero el texto no se VE en pantalla, esa
> es la próxima función a implementar (no debería trabar nada, solo faltaría el render visual).

> **Corrida 45 (2026-08-07) — El fix de la Corrida 44 funciona (`GetStringWidthHeightInRect` ya resuelve por
> id, no repite "not found" 766 veces), pero el usuario reporta un síntoma nuevo: el juego cae a ~1 FPS y no
> llega a Title/Menu. Antes de asumir que es lentitud genuina o un loop real, se agregó instrumentación
> precisa (tiempo real entre ticks + estado interno de la máquina de sub-estados de Init) en vez de seguir
> especulando.** `logs/log_2026-08-07_00-29-10.txt` (3074 líneas, mucho más chico que el runaway de la
> Corrida 44 -- `MAINPROC` solo aparece 20 veces por el propio cap del logging, `[FLUSH] GRP_Flush` 14 veces
> real -- sin capear --, `GetStringWidthHeightInRect` (id=46) se llama 115 veces en total, ~5-6 por tick):
> mucho más moderado que el runaway anterior, pero sigue sin llegar a Title.
>
> Leyendo `STATE_ProcessInit` completo (`out_ghidra.c:111798-111906`, el process-callback del estado 0/Init
> que corre cada tick mientras `STATE_nState` sigue en 0): tiene su PROPIA sub-máquina de estados,
> `INITSTATE_nState` (0/1/2/3) + `INITSTATE_LoadFrameCnt` (contador de frames dentro del sub-estado 0, que
> dispara un paso de init distinto por frame -- `IMGSYS_UnitLoad`, `Extend_FlurryStart`, `GRPX_Init`,
> `EXCELDATA_Create`, `startAd`, `Extend_RequestNotice` en el frame 10 -- hasta el frame 11, donde llama
> `GAMELOADER_ProcessPreLoad()`; si eso falla, salta a `INITSTATE_nState=3`). **`INITSTATE_nState==3` es un
> estado TERMINAL de error** (`out_ghidra.c:111879-111906`): dibuja "Memory error!" o, si
> `GAMELOADER_pText` es no-nulo, `UI_DrawTextOnCenter(GAMELOADER_pText,...)` -- y no tiene NINGUNA transición
> de salida en todo el decompile. Si estamos ahí, explicaría TODO lo observado: `GetStringWidthHeightInRect`/
> `DrawTextInRect` (medir+dibujar el mismo texto de error) corriendo cada tick para siempre, sin avanzar
> nunca a Title. `GAMELOADER_ProcessPreLoad()` (`:60309-60314`) hace `GAMELOADER_pText = MEM_Malloc(0x100);
> return GAMELOADER_pText != 0;` -- si falla, es porque `MEM_Malloc` (el allocator PROPIO del motor, un pool
> fijo de `MEM_nTotalPoolSize = 0x7d0000` = 8MB, `out_ghidra.c:22312-22331`, separado del heap general de C
> que ya se agrandó en la Corrida 13) está agotado en ese momento.
>
> Esto es hipótesis todavía sin confirmar con datos reales -- podría en cambio ser simplemente lentitud
> genuina (logging muy verboso en un build Debug, cada llamada JNI/GL logueada individualmente) sin estar
> realmente atascado en el estado de error. **Instrumentación agregada** (`source/patch.c`): el hook de
> `MainProcess` (ya existente desde la Corrida 32) sube su cap de log de 20 a 200 ticks, y ahora también
> loguea el tiempo real transcurrido desde el tick anterior (`sceKernelGetProcessTimeWide()`,
> microsegundos) junto con `INITSTATE_nState`/`INITSTATE_LoadFrameCnt`/`MEM_nUsedPoolSize`/
> `MEM_nTotalPoolSize`/`GAMELOADER_pText` crudos -- va a mostrar en una sola pasada si (a) está genuinamente
> atascado en `INITSTATE_nState=3` para siempre (confirma la hipótesis de arriba, revisar por qué
> `MEM_Malloc` falla), (b) avanza normalmente por los sub-estados pero cada tick tarda mucho tiempo real
> (confirma que es un problema de rendimiento/logging, no de lógica), o (c) algo completamente distinto que
> ninguna de las dos hipótesis anteriores cubre. Build compilado y verificado (cross real vía VitaSDK,
> `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-08-07) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, dejar correr un rato (ya que el cap subió a 200 ticks, hace falta
> más tiempo para llenarlo), mandar el log nuevo. Buscar las líneas `[MAINPROC] MainProcess() called (#...)
> ... delta_us=... INITSTATE_nState=... INITSTATE_LoadFrameCnt=... MEM_pool=.../ ... GAMELOADER_pText=...`.

> **Corrida 46 (2026-08-07) — Confirmado con datos reales: SÍ está atascado en el estado terminal de error
> (`INITSTATE_nState=3`), no es lentitud de logging. El pool de 8MB NO está agotado (solo ~2.1MB usados) --
> se descarta esa hipótesis. Falta leer el mensaje de error real para saber cuál paso de instalación
> falló.** `logs/log_2026-08-07_00-52-36.txt`: la secuencia de ticks lo muestra sin ambigüedad --
> ```
> #12  INITSTATE_nState=0  LoadFrameCnt=11  MEM_pool=2110328/8192000  GAMELOADER_pText=0x0
> #13  INITSTATE_nState=1  LoadFrameCnt=1   MEM_pool=2110584/8192000  GAMELOADER_pText=0x9878b1dc   <- GAMELOADER_ProcessPreLoad() tuvo éxito (MEM_Malloc(0x100) no falló)
> #14  INITSTATE_nState=3  LoadFrameCnt=1   MEM_pool=2110584/8192000  GAMELOADER_pText=0x9878b1dc   <- GAMELOADER_ProcessInstall() falló, salta al estado de error
> #15..#30  INITSTATE_nState=3 para siempre, MEM_pool sin cambios
> ```
> Confirma la hipótesis de la Corrida 45 al pie de la letra: `GAMELOADER_ProcessPreLoad()` (`:60309-60314`)
> corrió con éxito (si hubiera fallado, `nState` habría saltado directo a `3` en la MISMA transición, no a
> `1`) -- descarta el agotamiento del pool de 8MB como causa (`MEM_pool=2110584/8192000`, apenas 25% usado,
> consistente en los 16+ ticks siguientes). El fallo real está en `GAMELOADER_ProcessInstall()`
> (`:60241-60305`), que itera `INSTALLBASE_nRecordCount` registros llamando `GAMELOADER_Install`/
> `FILEPACK_Install` por cada uno -- si CUALQUIERA devuelve 0, formatea un mensaje real con
> `CS_knlSprintk(GAMELOADER_pText, MEMORYTEXT_GetText(), iVar6)` (`:60278-60281`) y retorna 0 -- eso es
> justamente lo que empuja a `STATE_ProcessInit` a `INITSTATE_nState=3`, el estado terminal sin salida que
> dibuja ESE mensaje para siempre.
>
> **Instrumentación agregada** (`source/patch.c`): el hook de `MainProcess` ahora también lee y loguea el
> CONTENIDO de texto real de `GAMELOADER_pText` (no solo su dirección) cuando `INITSTATE_nState==3` -- va a
> revelar el mensaje de error EXACTO que el juego está tratando de mostrar, que a su vez dice qué paso de
> instalación falló (probablemente relacionado con `GAMELOADER_Install`/`FILEPACK_Install` intentando copiar/
> instalar algo que este port, con assets sueltos en vez de un `.apk` real, no tiene en el lugar esperado).
> Build compilado y verificado (cross real vía VitaSDK, `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`,
> 2026-08-07) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log nuevo. Buscar la línea `[MAINPROC] ...
> GAMELOADER_pText=0x... "<mensaje real>"` en el primer tick donde `INITSTATE_nState=3` -- el texto entre
> comillas es la pista decisiva para el próximo fix (casi seguro en `GAMELOADER_Install`/`FILEPACK_Install`,
> `out_ghidra.c` cerca de `:60241`).

> **Corrida 47 (2026-08-07) — Causa raíz real confirmada: es exactamente la hermana del bug de signo de la
> Corrida 42 que se había dejado documentada como riesgo NO confirmado -- ahora sí lo está, y ya está
> arreglada.** `logs/log_2026-08-07_01-16-10.txt`: el mensaje de error real resultó vacío (`GAMELOADER_pText
> ""`) -- no daba una pista directa por texto, así que se rastreó la cadena de llamadas hacia atrás en el
> decompile en cambio. `GAMELOADER_Install` (`out_ghidra.c:60216-60237`) falla INMEDIATAMENTE si
> `RES_MoveToFileEx()` devuelve `0`, sin mirar siquiera sus propios parámetros. `RES_MoveToFileEx`
> (`:23471-23485`) llama directo a `RES_MoveToFilePack` (`:23411-23467`) -- **la MISMA función que la
> Corrida 42 ya había señalado como sospechosa** ("no confirmado crasheando todavía... si cualquiera de las
> dos llega a crashear más adelante, aplicar este mismo fix") -- y su chequeo de éxito es literalmente
> `if (-1 < iVar1)` sobre el valor de retorno de `RES_LoadToHeap`, el MISMO bug de signo ya arreglado en
> `RES_LoadToPool` (un puntero real de `CS_knlCalloc`/`calloc()` con el bit más alto puesto, siempre
> negativo como `int` con signo en este port, así que el chequeo falla siempre acá también).
>
> **Fix aplicado** (`source/patch.c`): se reimplementó `RES_MoveToFilePack` fielmente (misma lógica que
> `out_ghidra.c:23411-23467`, incluyendo la rama de descompresión LZMA y el manejo de los dos punteros
> distintos que hay que liberar -- el handle de `RES_LoadToHeap` vía `CS_knlFree` y, si hubo descompresión,
> el buffer decodificado vía `MEM_Free`) pero distinguiendo los 3 códigos de error explícitos de
> `RES_LoadToHeap` (`-1`/`-2`/`-3`) de "cualquier otro valor = un puntero real", igual que en la Corrida 42.
> Llama directo a las funciones reales `RES_LoadToHeap`/`LZMA_Decode`/`FILE_Seek`/`FILE_WriteAll`/
> `CS_knlFree`/`MEM_Free` (todas exportadas, sin modificar) -- solo cambia el chequeo de éxito/fracaso. Build
> compilado y verificado (cross real vía VitaSDK, `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`,
> 2026-08-07) — sin errores nuevos.
>
> **Pendiente**: redeployar, reproducir, mandar el log (+ `.psp2dmp` si crashea). Si el fix funciona, ya no
> debería aparecer `INITSTATE_nState=3` en las líneas `[MAINPROC]` -- debería seguir avanzando por
> `INITSTATE_nState=1`/`2` hasta `STATE_Set(4)` (Title) en `out_ghidra.c:111875`. `EXCELDATA_LoadFullResource`
> (`:58509-58510`) sigue siendo la ÚLTIMA función conocida con este mismo patrón de riesgo todavía sin
> confirmar -- si aparece un nuevo bloqueo, revisarla primero antes de investigar desde cero.

> **Corrida 61 (2026-08-10) — Nuevo reporte del usuario: el touch deja de funcionar y el personaje se
> detiene al llegar a otro stage después de terminar el tutorial, pero el juego sigue corriendo (no crashea,
> no se congela el render). Auditoría estática completa del patrón de bug de signo ya conocido más las
> transiciones de stage/evento; se arregló una instancia confirmada pero no relacionada, y se agregó
> instrumentación dirigida para el bug real porque el análisis estático solo no alcanza para confirmarlo.**
> Nota primero: `source/patch.c` ya tenía, sin que este archivo lo documentara, fixes hasta la Corrida 58
> (`GAMELOADER_Install`/`FILEPACK_Install`/`RES_MoveToFileEx`/`GAMELOADER_ProcessInstall` en la 48,
> `EXCELDATA_LoadFullResource` en la 49, `__udivsi3`/SIGFPE en la 54, `SGL_Util::GetGLSize` en la 55,
> `MAP_LoadAllToHeap`/`MAP_LoadFromHeap`/`MAP_DestroyFromHeap` en la 56, `UTIL_LoadSpriteListFromResource` en
> la 57, `GRP_PutAsciiChar`/`GRP_DrawPixel` en la 58) -- este documento se había quedado atrás desde la
> Corrida 47. No se reconstruyen esas entradas retroactivamente acá (no es el objetivo de esta corrida), pero
> quede constancia de que la numeración salta de 47 a 61 porque el código, no la documentación, es la fuente
> de verdad real del progreso.
>
> Re-audité los 6 call sites de `RES_LoadToHeap` en todo el decompile (`out_ghidra.c`, 214k líneas):
> `RES_LoadToPool` (`:23497`, Corrida 42), `RES_MoveToFilePack` (`:23423`, Corrida 47),
> `EXCELDATA_LoadFullResource` (`:58509`, Corrida 49), `MAP_LoadAllToHeap` (`:68493`, Corrida 56) y
> `UTIL_LoadSpriteListFromResource` (`:104080`, Corrida 57) ya estaban arreglados y los volví a verificar
> línea por línea contra el fix real -- todos fieles, sin bugs nuevos introducidos por los fixes mismos. El
> sexto call site, `SOUND_Create` (`:91479-91491`, carga un `.mmf` desde `SOUNDSYSTEM_PlayEx`), NO estaba
> cubierto por ninguna Corrida anterior y tiene el mismo idiom exacto (`if (-1 < iVar1)` sobre el retorno de
> `RES_LoadToHeap`). Arreglado en esta misma corrida siguiendo el precedente de la Corrida 56 ("arreglar
> todos los call sites confirmados de una pasada"), pero esto es SOLO audio -- no tiene ningún camino de
> vuelta hacia `GAMESTATE_nState`, `MAP_nID` ni el despacho de input, así que no se cree que sea el bug
> reportado.
>
> Para el bug real, rastreé cada candidato estático plausible para que una transición de stage bloquee el
> input mientras el render sigue vivo:
> - `GAMESTATE_SetState` (`out_ghidra.c:110912-111157`), el dispatcher detrás de
>   `GAMESTATE_fpProcess`/`fpPressKey`/`fpDraw` que `STATE_ProcessGame` llama cada tick
>   (`:111249-111280`) -- cada `case` del switch pone su propio trío de punteros; ningún case alcanzable
>   durante gameplay deja los tres en NULL para siempre como el bug de `STATE_fpProcess` de la Corrida 33.
> - `SURFACE_nType`/`SURFACE_Process` (`:101949-102039`), que SÍ bloquea `GAMESTATE_fpProcess`/`fpPressKey`
>   por completo mientras está activo (`GAMESTATE_Process`/`GAMESTATE_PressKey`, `:111235-111245`/
>   `111161-111208`) -- pero es un simple contador de fade de ±25 por tick sin ninguna dependencia de carga
>   de recursos, se autolimpia a 0 en como máximo 4 ticks. Descartado como riesgo de traba permanente.
> - `MAP_Load` (`out_ghidra.c:69704-69829`), al que se llega desde `MAPSYSTEM_ChangeMap` Y directamente desde
>   el intérprete de scripts de evento (`EVTSYSTEM_Process` opcode `0xa`, `:52724-52731` -- exactamente el
>   tipo de comando que un evento de fin-de-tutorial usaría para mandar al jugador al siguiente stage).
>   `MAP_Load` es `void` y no devuelve nada: si `MAP_LoadFromHeap(param_1, ...)` falla (`:69724-69728`,
>   `if (iVar5 == 0) return;`), `MAP_Load` corta ANTES de `MAP_nID = param_1`, `MAP_SetFocus`,
>   `CHARLOCSYSTEM_Load` o `EVTSYSTEM_CheckOnMapLoad` -- y NINGUNO de sus llamadores (`MAPSYSTEM_ChangeMap`,
>   `EVTSYSTEM_Process`) revisa esto, así que el script de evento (y el GAMESTATE) siguen como si la carga
>   hubiera funcionado. Esto es un gap de diseño real y confirmado por el decompile que explicaría
>   exactamente "llegó a un stage nuevo, el juego sigue corriendo, el personaje/touch no hacen nada" si los
>   datos del mapa de destino quedan medio inicializados. SIN EMBARGO: `MAP_LoadFromHeap`/`MAP_LoadAllToHeap`
>   ya se re-verificaron línea por línea contra el fix de la Corrida 56 (`:68386-68540`) y son fieles -- no
>   hay evidencia estática de POR QUÉ cargar el mapa de un stage específico fallaría. Adivinar un fix acá sin
>   esa evidencia arriesgaría enmascarar un problema real de datos/lógica en vez de arreglarlo (la instrucción
>   de esta tarea es explícita: no adivinar y enviar sin poder justificarlo).
>
> En vez de elegir una de estas hipótesis y enviar un fix no verificable, se agregó logging barato y
> disparado por transición (no por frame) que va a mostrar EXACTAMENTE dónde queda trabado el juego en el
> próximo log de hardware (`source/patch.c`, todo bajo el mismo estilo que el hook de `MainProcess` de las
> Corridas 45/46 que convirtió una sospecha en una causa raíz confirmada):
> - `GAMESTATE_SetState_hook`: loguea cada transición de gamestate solicitada junto con
>   `GAMESTATE_nState`/`STATE_nState`/`MAP_nID`/`EVTSYSTEM_nID`/`EVTSYSTEM_nState` en ese momento (cap 200).
> - `EVTSYSTEM_SetState_hook`: loguea cada transición del sub-estado del script de evento (0-8,
>   `out_ghidra.c:52163-...`) -- el estado 6 es "evento completo, devolver control a
>   `GAMESTATE_SetState(2)`" (`:52304-52323`); si el log termina antes del 6, el script de evento nunca
>   terminó.
> - `MAP_Load_hook`: loguea el id de mapa pedido y el valor real de `MAP_nID` justo después de que la llamada
>   real retorna -- si no coinciden, `MAP_LoadFromHeap` falló en silencio para ese stage específico,
>   confirmando la hipótesis de arriba con un número real en vez de un tal vez.
> - `MAP_LoadFromHeap_hook` (ya existente desde la Corrida 56) ahora también loguea el id de mapa pedido y la
>   cantidad de registros que leyó de `map.dat`, para detectar de inmediato un id fuera de rango.
> Todos los hooks llaman directo a las funciones reales (`SO_CONTINUE`/`SO_CONTINUE_VOID`) sin cambiar
> comportamiento -- es solo instrumentación. Build compilado y verificado (cross real vía VitaSDK,
> `rsync` a `/tmp/inotia3-src`/`/tmp/inotia3-build`, `cmake -DCMAKE_BUILD_TYPE=Release` +
> `make -j8`, 2026-08-10): `inotia3.vpk`/`eboot.bin`/`inotia3` (ELF) generados sin errores; una pasada
> adicional con `-Wall -Wextra` solo mostró warnings de "variable no usada" idénticos en naturaleza a los
> que YA existen en los hooks previos (Corridas 45, 35, 36, 41, 58) cuando el nivel de log está desactivado
> -- ningún warning nuevo de una categoría distinta.
>
> **Pendiente**: redeployar, reproducir (terminar el tutorial y llegar al stage nuevo hasta que se repita el
> freeze), mandar el log nuevo. Buscar, en orden de utilidad:
> 1. La última línea `Corrida 61: GAMESTATE_SetState(...)` antes de que el input deje de responder -- el
>    `new_state` pedido ahí es el estado en el que probablemente queda trabado el juego.
> 2. Si esa línea pide el estado `0xd` (13, Event) o `0x2b` (43, MapChange) y no hay ninguna transición de
>    `GAMESTATE_SetState` después: revisar las líneas `Corrida 61: EVTSYSTEM_SetState(...)` alrededor de esa
>    marca de tiempo para ver en qué sub-estado del script de evento se quedó colgado.
> 3. Las líneas `Corrida 61: MAP_Load(...) returned -- MAP_nID is now ... (loaded|SILENTLY FAILED...)` y
>    `Corrida 61: MAP_LoadFromHeap(map_id=...) -- map.dat record count=... (in range|OUT OF RANGE)` cerca de
>    la transición de stage -- si dicen `SILENTLY FAILED` u `OUT OF RANGE`, la hipótesis de `MAP_Load` de
>    arriba queda confirmada con datos reales y el próximo paso es arreglar `MAP_Load`/sus llamadores para
>    que propaguen el fallo en vez de tragárselo.
> Si ninguna de las tres pistas anteriores muestra nada anómalo (transiciones normales, `MAP_nID` coincide,
> conteo en rango), el bug está en otra parte todavía no instrumentada (posiblemente dentro de
> `GAMESTATE_ProcessPlay`/`GAMESTATE_PressKeyPlay` mismos, o en el sistema de pathing `CHAR_SearchPathAsLocation`
> que `EVTSYSTEM_Process` también usa) y haría falta una segunda ronda de instrumentación con ese log como
> punto de partida.

> **Corrida 62 (2026-08-10) — Reporte de usuario: "no puedo guardar el juego durante la partida, dice
> failed". Causa raíz real encontrada por análisis estático puro (sin pista en el log:
> `logs/log_006.txt`, 166303 líneas, búsqueda insensible a mayúsculas por "save"/"failed", cero
> coincidencias) -- `CS_fsOpen` nunca crea el subdirectorio de guardado que el propio juego compone, y la
> única función que sí lo haría (`HubSave_ProcessInitialize`) se autodesactiva silenciosamente en este port,
> por diseño y no por accidente.** `SAVE_ProcessSave` (`out_ghidra.c:82727-82753`, el menú de Guardar en
> partida) llama a `SAVE_Save()` (`:82665-82723`) y muestra el TextData `0xc` ("éxito") o `0xd` ("failed")
> únicamente según su valor de retorno. `SAVE_Save` → `SAVE_SaveData` (`:82253-82262`) →
> `SAVE_SaveDataAsKey` (`:82234-82249`) → `SAVE_GetSaveFileName` (`:82018-82045`), que construye el nombre
> real como `"<MD5(HubSave_GetKey())>/save<N>.dat"` vía `HubSave_GetFolderName` (`:124328-124359`) +
> `HubSave_MakeFullName` (`:124212-124222`) -- **siempre** con subdirectorio, nunca un archivo suelto,
> porque `HubSave_GetFolderName` solo revisa que el puntero de la clave no sea `NULL` (nunca lo es:
> `HubSave_key` es un buffer estático que existe siempre, aunque esté vacío). Ese path relativo llega a
> `SAVE_SaveDataAsNameAndKey` (`:80860-80884`) → `FILE_Open(path, 4, 1)` → `CS_fsOpen` (`:154900-154967`),
> que para un nombre relativo construye el destino real como `jGetUserDir()+jGetPackageName()+"/"+path`
> (`"ux0:data/inotia3/<md5>/save<N>.dat"` en este port -- `GetUserDir_stub`/`GetPackageName_stub`,
> `source/java.c`) y llama a un `open(..., O_CREAT|...)` de libc plano -- que, como cualquier `open()`
> POSIX, **no crea los directorios padre que falten**. Si `"<md5>/"` nunca se creó, `open()` falla con
> `ENOENT`, `FILE_Open` devuelve negativo, `SAVE_SaveDataAsNameAndKey` devuelve `0`, y ese fracaso se
> propaga sin cambios hasta el popup "failed" de `SAVE_ProcessSave` -- coincide exactamente con el reporte.
>
> Lo único que crea ese subdirectorio es `HubSave_MakeFolder` (`:124226-124239`, un wrapper simple de
> `CS_fsIsExist`/`CS_fsMkDir`), llamado desde `HubSave_ProcessInitialize` (`:124414-124470`, el arranque de
> Hub al iniciar la app) -- pero **solo** si `HubSave_LoadKey()` (`:124296-124324`) tiene éxito.
> `HubSave_LoadKey` llama a `CS_hlpGetAppProperty("uniqueUserId", ...)` (`:156047-156103`, reenvía a
> `jGetAppProperty()` propio del `.so`, `:146811-146843`) y, si falla, a
> `CS_knlGetSystemProperty("UDID", ...)` (`:157477-157559`, reenvía a `jGetUDID()`, `:145848-145877`) --
> ambas resuelven su método Java vía `GetMethodID(cls, "GetAppProperty"/"GetUDID", ...)` a través de
> FalsoJNI. Por la política ya documentada en el propio `source/java.c` (comentario de cabecera: "dejar el
> nombre sin registrar es la opción segura por defecto... hasta que un log real en hardware confirme cuáles
> se llaman de verdad y necesitan un handler real"), `"GetAppProperty"`/`"GetUDID"` **no** están registrados
> en `methodsInt`/`methodsLong` (grep sobre `source/java.c` y `lib/falso_jni`: cero coincidencias para
> cualquiera de los dos nombres) -- así que `GetMethodID` devuelve `NULL` correctamente para ambos,
> `jGetAppProperty()`/`jGetUDID()` toman su rama de "método no encontrado" y devuelven `-1`/`0xffffffff`.
> Eso hace que `HubSave_LoadKey()` falle (`HubSave_key` queda en todo ceros -- string vacío, pero puntero NO
> nulo), así que `HubSave_ProcessInitialize` imprime su traza `"2"` y retorna **sin** llamar nunca a
> `HubSave_MakeFolder`. Esto es Hub/login social -- ya marcado en `CLAUDE.md` como subsistema
> no-op-hasta-que-se-demuestre-necesario para este port offline de un jugador -- quedándose dormido
> correctamente, un resultado razonable y funcionando-según-lo-diseñado... hasta que `SAVE_GetSaveFileName`,
> de forma totalmente independiente, sigue calculando un nombre de subdirectorio real y determinístico (MD5
> de la clave vacía) y le pasa a `CS_fsOpen` una ruta dentro de un directorio que nadie creó nunca.
>
> **Fix aplicado** (`source/patch.c`, función `CS_fsOpen_hook` + hook `h_CS_fsOpen` registrado en
> `so_patch()`): en vez de reimplementar `SAVE_Save`/`SAVE_GetSaveFileName`/`HubSave_*` (varias funciones
> fuertemente acopladas, más la tabla de errno→código de error propia de `CS_fsOpen`, cuyos valores reales
> viven en el `.data` del `.so` y preferimos no transcribir a mano), se hookeó `CS_fsOpen` directamente --
> exportada (confirmado con `nm -D` sobre el `libgame.so` real de `inotia3-1-3-2`: `000f527c T CS_fsOpen`) y
> el único punto de paso de TODAS las escrituras nativas de archivo de este `.so`
> (`review.dat`/`hubcheck.sav`/`StoreConFirm.sav`/`langtype.dat`/`StoreState.sav`/`Com2usProtection.sav`/
> `save<N>.dat` -- se revisó cada call site). El hook, antes de llamar a la función real sin modificar (vía
> `SO_CONTINUE`), reconstruye el mismo path absoluto que `CS_fsOpen` compondría (usando las mismas
> `jGetUserDir`/`jGetPackageName` reales del `.so`, ya resueltas en este archivo desde la Corrida 21) y, si
> el modo de apertura implica creación (`2`/`4`/`8` -- confirmado leyendo cada rama,
> `:154917/154927/154931/154946`), llama a `file_mkpath()` (`source/utils/utils.c`, ya existente en este
> port justo para esto) sobre esa ruta antes de que la función real intente el `open()`. `file_mkpath()` es
> idempotente (`errno==EEXIST` cuenta como éxito), así que esto no cambia nada para los archivos que ya
> viven directamente bajo `ux0:data/inotia3/` -- solo repara los casos con subdirectorio faltante, sea el
> guardado o cualquier otro caso no observado todavía. No se tocó ni una línea de
> `SAVE_*`/`HubSave_*`/`CS_fsOpen` mismos, ni se implementaron `GetAppProperty`/`GetUDID` (eso seguiría
> siendo un cambio mucho más grande y no necesario para este bug puntual). Build compilado y verificado
> (cross real vía VitaSDK, `build/inotia3.vpk`/`eboot.bin`/`inotia3.elf`, 2026-08-10) -- sin errores ni
> warnings nuevos.
>
> **Pendiente**: redeployar, reproducir (guardar durante una partida), mandar el log nuevo. Si el fix
> funciona, el guardado debería mostrar el TextData `0xc` ("éxito") en vez de `0xd`, y el archivo
> `ux0:data/inotia3/<hash>/save<N>.dat` debería existir después en la consola. Si TODAVÍA falla, buscar en
> el log nuevo las líneas `Corrida 62: CS_fsOpen(...)` (agregadas en este mismo fix, nivel debug) -- dirán
> exactamente qué ruta se intentó crear y si `file_mkpath()` tuvo éxito; si `file_mkpath()` tiene éxito pero
> el `open()` real de todas formas falla, el problema estaría en otro punto de la cadena (p.ej.
> `SAVE_AllocateMemory`/el pool de tamaño fijo que usa `SAVE_Save`, o el propio contenido cifrado que
> `SAVE_SaveDataAsNameAndKey` escribe) y habría que investigar eso a continuación con el log en mano. Esta
> es, como todo lo demás en esta sección, una corrección respaldada por el decompile pero **no verificada
> en hardware/emulador real** -- no se puede confirmar "arreglado" hasta ese round trip.

> **Corrida 63 (2026-08-10) — Reporte de usuario: "el juego no tiene sonido". Causa raíz: el subsistema de
> audio real (Fase 7, `source/audio.c`) nunca estuvo conectado a nada -- ni el motor lo llamaba (los 9 ids
> JNI de sonido estaban wireados a no-ops desde la Corrida ~33) ni `audio_init()` se invocaba jamás desde
> `main.c`. Revisado el resto de la sección Fase 9 completa (incluida la Corrida 47 arriba, la entrada más
> reciente): la razón original para desconectar audio -- descartar el mixer thread como variable mientras se
> triageaba el crash de stack-protector en `png_create_read_struct_2` -- quedó resuelta en la Corrida 47
> (bug de signo real en `RES_MoveToFilePack`, nada que ver con audio/threads). No hay ninguna nota posterior
> a la Corrida 47 que reintroduzca sospecha sobre audio específicamente, así que se procedió a reconectarlo.**
>
> **Cambios aplicados:**
> 1. `source/main.c` (~línea 177, antes de `wr_timer_reset()`/`nativeInit()`): se agregó `#include "audio.h"`
>    y una llamada a `audio_init()` -- tiene que correr ANTES de `nativeInit()` porque `startClet()` (que
>    corre DENTRO de `nativeInit()`) puede llamar a `SoundCreate`/`SoundPlay` inmediatamente, igual que ya
>    pasa con `wr_timer_reset()` en el comentario justo debajo.
> 2. `source/java.c`: se eliminaron `SoundCreate_stub`, `Sound_NoopOk` y `SoundGetVolume_stub` (y el contador
>    `g_next_sound_handle`, ya sin uso) y se agregaron 9 wrappers reales (`SoundCreate_wrapper` ...
>    `SoundPrepare_wrapper`) que desempacan el `va_list` según la firma confirmada de cada id (comentarios ya
>    existentes junto a los ids 27-35) y llaman directo a `audio_sound_create/destroy/play/pause/resume/
>    stop/set_volume/get_volume/prepare()` de `audio.h`. `jboolean` (parámetro `loop` de SoundCreate/
>    SoundPlay) se desempaca con `va_arg(args, jint)`, igual que el resto de flags booleanos ya existentes en
>    este archivo (ver `DrawTextInRect_stub`) -- en la ABI variádica de ARM32 se promueve a `int`, no hay
>    overload de tipo `jboolean` real en ningún otro `va_arg` de este archivo. La tabla `methodsInt[]` (ids
>    27-35) ahora apunta a los 9 wrappers reales en vez de los no-ops.
> 3. **Segundo bloqueador encontrado y corregido**: `audio_sound_create()` hace `fopen()` directo sobre lo
>    que se le pase -- la resolución de la ruta real tiene que pasar en el wrapper de JNI, no en `audio.c`
>    (igual que ya hace `GetFileDescriptor_stub` para imágenes, líneas ~671-725 de este mismo archivo). Se
>    agregó `inotia3_resolve_sound_path()`, que prueba (a) la ruta tal cual, (b) `INOTIA3_LOOSE_ASSETS_PATH +
>    nombre`, y (c) `INOTIA3_LOOSE_ASSETS_PATH + nombre + ".jpg"` (el mismo `INOTIA3_RES_NAME_APPEND` que ya
>    usa el camino de imágenes, confirmado funcionando desde que el port llega a la pantalla de carga).
>    **Nota importante que contradice el propio comentario de cabecera de `audio.h`**: ese comentario dice
>    que el sufijo `.jpg` "debe stripearse al preparar los assets en la Vita" -- pero el ÚNICO mecanismo de
>    esta base de código que ya funciona de forma confirmada (`GetFileDescriptor_stub`, usado para las
>    imágenes que sí llegan a cargar) hace exactamente lo CONTRARIO: deja el sufijo `.jpg` intacto en el
>    archivo suelto en disco y lo VUELVE A AGREGAR en código al construir la ruta. Se siguió ese patrón ya
>    probado en vez del comentario de `audio.h` (que aparenta estar desactualizado/nunca implementado). Con
>    esto, **no hace falta tocar ningún script de `porting_tools/`** -- de hecho no existe ningún script de
>    staging de assets de audio en `porting_tools/` (se revisó todo el árbol; solo hay un `ai_bash_commands.sh`
>    con checks manuales puntuales de `file`/`xxd` sobre PNGs, nada reusable ni relacionado a `.ogg`).
>    **Advertencia honesta**: no hay acceso a `decompiled_so`/`decompiled_apk` en este entorno (excluidos por
>    `.gitignore`, como corresponde), así que la cadena exacta que `libgame.so` le pasa a `SoundCreate` no
>    pudo confirmarse contra `out_ghidra.c` -- de ahí que el resolver pruebe 3 candidatos y loguee
>    (`[Audio] SoundCreate("...") -> "...", handle N`) cuál usó, para poder diagnosticar desde el log real si
>    ninguno de los 3 acierta.
> 4. **Bloqueador de build encontrado y corregido**: `source/audio.c` NO estaba en la lista de fuentes de
>    `CMakeLists.txt` (`add_executable`) y la librería Tremor (`vorbisidec`/`ogg`) que `audio.c` usa
>    (`<tremor/ivorbisfile.h>`, `ov_open`/`ov_read`/`ov_clear`/etc.) tampoco estaba enlazada -- ni vendorizada
>    en `lib/` ni en `target_link_libraries`. Confirmado que VitaSDK sí trae ambos paquetes preinstalados
>    (`arm-vita-eabi/lib/libvorbisidec.a`, `arm-vita-eabi/lib/libogg.a`, headers bajo
>    `arm-vita-eabi/include/tremor/`) -- se agregó `source/audio.c` a `add_executable()` y `vorbisidec`/`ogg`
>    a `target_link_libraries()`. Sin este fix, aun con el resto de los cambios de audio, el build no habría
>    ni compilado.
>
> **Build verificado (cross real vía VitaSDK)**: se armó un árbol temporal separado (`/tmp/inotia3-src-
> audiofix`/`/tmp/inotia3-build-audiofix`, limpiado al terminar) siguiendo el mismo `rsync` + `cmake
> -DCMAKE_BUILD_TYPE=Release` + `make -j4` que usa `porting_tools/build/build_and_install.sh`. Compiló y
> linkeó sin errores nuevos -- `inotia3.vpk`/`eboot.bin` generados. `nm` sobre el binario final confirma que
> `audio_init`, `audio_sound_create`, `SoundCreate_wrapper`, `ov_open`/`ov_open_callbacks` quedaron todos
> presentes en el ejecutable (Tremor sí está linkeado, no es dead code eliminado por el linker). **Esto es
> solo verificación de compilación/link, NO una prueba de que el sonido efectivamente se escuche** -- no hay
> acceso a hardware/Vita3K desde este entorno.
>
> **Pendiente**: redeployar, reproducir, y confirmar con oídos reales que el audio se escucha (efectos y/o
> música, según qué dispare el motor primero). Revisar el log en busca de las líneas `[Audio] SoundCreate(...)
> -> "...", handle N` para confirmar que el resolver de rutas encontró el archivo real y no cayó al mensaje
> de "could not resolve" -- si cae ahí sistemáticamente, es la señal de que la convención de nombre que pasa
> `libgame.so` no es ninguna de las 3 que probé, y hay que revisar `out_ghidra.c` (SOUNDSYSTEM_*/
> SOUNDPLAYER_*) para confirmar el string real. También vigilar cualquier síntoma parecido al viejo crash de
> Fase 9 (`png_create_read_struct_2`) -- aunque la Corrida 47 lo resolvió por una causa no relacionada a
> audio, el mixer thread agregado por `audio_init()` es código nuevo corriendo concurrentemente con el resto
> del motor (thread separado, prioridad `0x10000100`, stack de 64KB) que nunca se había ejercitado en un run
> real hasta ahora -- si aparece cualquier crash nuevo relacionado a memoria/threads después de este cambio,
> revisar primero si desaparece con `audio_init()` comentado antes de asumir que es un problema no
> relacionado.

> **Nota sobre numeración**: el commit `36696a8` ("Save all porting changes, patches, scripts, and
> psvita-porting skill") agregó ~640 líneas a `source/patch.c` (más cambios en `dynlib.c`/`java.c`/
> `logger.c`) documentadas ahí mismo como Corridas 48 a 60 (instalación de assets,
> `MAP_LoadAllToHeap`/`MAP_LoadFromHeap`/`MAP_DestroyFromHeap`, `UTIL_LoadSpriteListFromResource`, texto real
> vía `DrawTextInRect_stub`, logging por lotes) **sin actualizar este archivo** -- por eso las Corridas 61-63
> de arriba retoman la numeración real del código (61 en vez de un backfill de 48-60, que queda fuera del
> alcance de esta sesión) en vez de la que este documento mostraba. La entrada de abajo usa Corrida 64 por la
> misma razón -- la investigación que la generó corrió en paralelo a las tres de arriba sin verlas todavía.

> **Corrida 64 (2026-08-10) — Investigación de un glitch visual reportado por el usuario en el retrato de
> personaje (HUD, arriba a la izquierda); no se pudo confirmar una causa raíz por análisis estático solo, se
> agregó instrumentación en su lugar.** Reporte del usuario (español, sin captura): "tengo un glitch en el
> icono del personaje arriba a la izquierda", visto después del tutorial en un escenario nuevo. Sin
> descripción de si es color, sprite equivocado, imagen faltante, parpadeo o estiramiento -- una captura del
> usuario sigue haciendo falta para acotar esto más rápido; ver "Pendiente" abajo.
>
> `logs/log_007.txt` (grep -a, el archivo tiene bytes ANSI que rompen grep sin `-a`) ya mostraba la carga
> sana de exactamente dos retratos (`portrait4.png`/`portrait4.pngi` en la línea ~39152, `portrait0.png`/
> `portrait0.pngi` en la ~150578 -- confirmando que el personaje mostrado SÍ cambia durante la sesión, un
> escenario donde un bug de caché sería visible), sin reintentos ni errores de I/O -- si hay un bug, está
> río abajo de la carga del recurso, no en la lectura del archivo. (Este mismo log también confirmó, por
> separado, que la consola todavía corría el build previo a las Corridas 61-63 de arriba -- ver esas entradas
> -- así que esta investigación del retrato es independiente de esos tres fixes, no una consecuencia de
> ellos.)
>
> **Rastreo completo de la cadena de dibujo contra `out_ghidra.c` (nada de esto fue reimplementado por este
> port -- es código original del `.so`, se leyó tal cual corre):**
> - `Portrait_Draw` (`:125716`) resuelve un "file id" interno (`local_88`, un byte leído de
>   `PORTRAITBASE_pData`, no observable desde afuera sin hookear la función) y sólo llama a
>   `GRPI_UnloadImage`/`GRPI_LoadImage` cuando difiere del `Portrait_nLoadedFileID` cacheado -- un caché
>   legítimo, no el idiom de signo `-1 < X` ya arreglado 6 veces (Corridas 42/47/49/56/57): sus chequeos son
>   `-1 < param_1` y `local_88 < RecordCount` sobre índices/longitudes chicas, nunca sobre un puntero crudo
>   de `CS_knlCalloc` -- esa familia de bug específica no aplica acá.
> - `GRPI_LoadImage` (`:124159`) pasa por `SGL_CreateTexture` (`:107303`), es decir una textura GL REAL
>   (`SGL_Resource::FromBundle` -> `SGL_Texture::FromResource`), **no** el blit de software CSFB que usan
>   `GRP_PutAsciiChar`/`GRP_DrawPixel` (Corrida 58) para el texto -- retratos y texto en pantalla son dos
>   caminos de render totalmente distintos en este motor.
> - El `.pngi` (metadata del atlas de piezas, cargado a través del fix de `EXCELDATA_LoadFullResource` de la
>   Corrida 49) decodifica de forma internamente consistente: después del header de 6 bytes de
>   `GRPI_LoadInfo` (4 bytes de cantidad de piezas + 2 bytes saltados), los 114 bytes descomprimidos de
>   `portrait4.pngi` y los 126 de `portrait0.pngi` dan, cada uno, un número EXACTO de registros de 12 bytes
>   (9 y 10 piezas respectivamente) -- sin off-by-one en la longitud descomprimida.
> - Los dos PNG reales (`assets/common/game_res/portrait0.png.jpg`/`portrait4.png.jpg`, el sufijo `.jpg` es
>   sólo el disfraz de extensión de este port) son 512x512, 8 bits de profundidad, color type 6
>   (truecolor+alpha) -- mismo formato que otras texturas 512x512 RGBA que este port ya confirmó renderizando
>   bien en otras partes del log, y bit-a-bit idénticos entre las dos versiones de APK presentes localmente
>   (1.3.2 vs. 1.4.5, mismo MD5) -- descarta un desajuste de versión entre assets y `.so`.
> - `SGL_CreateTexture`/`SGL_DeleteTexture` (`:107303`/`106097`) mantienen su propia tabla `s_cachedTexture`
>   correctamente (búsqueda lineal + swap-remove al borrar) -- tampoco es un leak/overflow ahí.
>
> **Ninguna de estas confirmaciones descarta un bug que sólo se vea en vivo.** Dos hipótesis quedan abiertas,
> ninguna confirmable sin hardware:
> 1. `force_linear_filter` (`source/utils/glutil.c`, Fase 4) fuerza `GL_LINEAR` incondicional en cada subida
>    de textura -- fix deliberado y necesario contra el blanco de "textura incompleta" de vitaGL, no algo
>    que se pueda simplemente quitar -- pero SÍ puede sangrar unos pixeles en los bordes de las piezas del
>    atlas si no hay padding entre ellas. Sería un artefacto sutil y sistemático (afecta a toda textura sin
>    mipmaps de este port, no sólo al retrato), así que encaja poco con algo lo bastante notorio para que el
>    usuario lo reporte espontáneamente, pero no se puede descartar sin verlo.
> 2. Una falla real de creación de textura en vitaGL bajo presión de VRAM (más plausible cuanto más avanzada
>    la sesión -- coincide con el reporte "después del tutorial, en un escenario nuevo") podría devolver una
>    textura con contenido de VRAM sin inicializar en vez de simplemente fallar limpio -- eso sí se vería
>    como "basura"/glitch real y sólo en ese momento, no desde el arranque.
>
> **Instrumentación agregada** (`source/patch.c`, hook transparente sobre `Portrait_Draw`, sin cambiar
> comportamiento): loguea record/x/y/state/flip solicitados y, leyendo directo los globales exportados
> `Portrait_nLoadedFileID`/`gImgPortrait`, el estado del caché ANTES y DESPUÉS de la llamada real (id
> cargado, handle de textura, cantidad de piezas, puntero al array de piezas), marcando `[RELOADED]` cuando
> el id cambia. El próximo log en hardware dirá si: (a) el mismo record resuelve a un id distinto en cada
> dibujo (caché rota/desincronizada -- explicaría un parpadeo entre dos retratos), (b) una carga falla en
> silencio (`Portrait_nLoadedFileID` se queda en -1 tras una llamada que debía cargar algo, con el handle de
> textura en 0), o (c) ninguna de las dos pasa, en cuyo caso el glitch está a nivel de contenido/filtrado de
> la textura GL y hace falta una captura de pantalla para seguir. Build compilado y verificado (cross real
> vía VitaSDK, en un árbol `/tmp` aislado, `inotia3.vpk`/`eboot.bin`/`inotia3.elf` -- sin errores ni warnings
> nuevos, 2026-08-10).
>
> **Pendiente**: redeployar, reproducir el glitch, y mandar el log nuevo -- buscar las líneas
> `Corrida 64: Portrait_Draw(...)` (antes/después) alrededor del momento del cambio de escenario. **Pedirle
> al usuario una captura de pantalla del glitch sigue siendo el paso más eficiente**: color/tinte incorrecto,
> sprite equivocado, imagen ausente/en blanco, y estiramiento/desplazamiento son síntomas de bugs
> completamente distintos (filtrado GL, caché de `Portrait_nLoadedFileID`, fallo de `GRPI_LoadImage`, y
> matemática de posición/escala en `GRPX_DrawPartEx`, respectivamente) y sin verla no se puede acotar más la
> búsqueda desde análisis estático solo.

> **Corrida 60 (2026-08-10) — Investigación de un reporte de usuario ("algunos textos no tienen fondo y
> tienen letra blanca, lo que hace difícil su lectura") sin acceso a hardware/log/screenshot. Encontrado un
> candidato concreto, respaldado por decompile, pero NO confirmado todavía en consola/Vita3K: otro método
> JNI sin registrar, `GetStringWidth`, distinto de `GetStringWidthHeightInRect` (ya arreglado en la Corrida
> 44/59) — esta entrada usa el número 60 porque es el primero libre entre la Corrida 59 (ya en `source/java.c`)
> y las Corridas 61-64 de arriba (touch/save/audio/portrait, investigadas en paralelo hoy mismo).** Se
> investigaron las 4 hipótesis planteadas para el reporte (método JNI de fondo faltante, overflow/aliasing de
> `g_textview_pixel_buffer`, otro método sin registrar, y el ángulo de la máscara binaria sin antialiasing)
> contra `out_ghidra.c` y `decompiled_apk/sources/.../WrapperJinterface.java`.
>
> Descartado con bastante confianza: el dibujo de texto genérico (`GRPX_DrawStringWithFont`/`GRPX_DrawString`
> → `SGL_grpDrawStringWithFont`/`SGL_grpDrawStringInRectWithFontEx` → `SGL_grpCreateStringImageInRect`) NUNCA
> dibuja un fondo — ni en el juego real. Los fondos que sí existen (`MONEY_DrawBox`/`DrawSpeechBubble`
> `out_ghidra.c:73513`/`96793`, la franja de `INSTANTMSG_DrawNameFrame` `:96882`) son sprites/rects NATIVOS
> del propio `.so` (`GRPX_DrawPart`/`GRPX_FillRect` sobre atlas cargados por `IMGSYS_*`), dibujados por el
> LLAMADOR antes de pedir el texto — sin ninguna dependencia de JNI. Esto hace muy poco probable que
> `DrawTextInRect_stub`/`GetStringWidthHeightInRect_stub` (Corrida 59/44) sean la causa de un fondo
> faltante: estructuralmente no pueden dibujar ni suprimir un fondo, solo la máscara de cobertura del glyph.
>
> **Candidato real encontrado**: `SGL_GetAsciiTextSize` (`out_ghidra.c:162608-162632`) es HERMANA de
> `SGL_GetTextSizeInRect`/`SGL_GetTextSize` (`:162570-162604`, que sí usan correctamente el id 46 ya
> arreglado vía `jGetStringWidthHeightInRect`) pero mide el texto con una llamada JNI TOTALMENTE DISTINTA:
> `jGetStringWidth` (`:143919`), que resuelve `WrapperJinterface.GetStringWidth(String,int,int):int`
> (firma literal confirmada `"(Ljava/lang/String;II)I"` en `decompiled_apk/.../WrapperJinterface.java:1112`,
> `paint.measureText(str)` sin ninguna dependencia de fondo — solo un ancho). Este método NUNCA estuvo
> registrado en `nameToMethodId[]`. A diferencia de los `strcpy(NULL)` que crashean de la Corrida 40/50,
> `jGetStringWidth` no chequea si `GetMethodID` devolvió NULL antes de usarlo — llama
> `CallIntMethod(env, obj, NULL, ...)` de todas formas. Se rastreó `methodIntCall()` en
> `lib/falso_jni/FalsoJNI_ImplBridge.c:279-288` directamente: su rama "no encontrado" hace
> `return -1;` (NO 0). Ese `-1` sale de `jGetStringWidth`/`SGL_GetAsciiTextSize` como "ancho medido" de
> CUALQUIER texto que pase por este camino — y `__utilGetStringWidth` (`:151938`, detrás de
> `CSOGL_grpGetStringWidth`/`ByUTF8`) llama a la misma función sin registrar. Cualquier elemento de UI que
> use este ancho para dimensionar SU PROPIO rect/caja de fondo (antes de pedirle a `GRPX_DrawStringWithFont`
> que dibuje el texto — que sí mide/dibuja bien por el camino ya arreglado del id 46/49) dibujaría ese fondo
> con un ancho de `-1` — invisible, o descomunal si se trata como sin signo más adelante — mientras el texto
> en sí se ve perfectamente blanco y nítido. Encaja exactamente con el síntoma reportado.
>
> **Fix aplicado** (`source/java.c`): registrado `{ 50, "GetStringWidth", METHOD_TYPE_INT }` en
> `nameToMethodId[]` y añadido `GetStringWidth_stub` (registrado en `methodsInt[]`), reusando la misma tabla
> de ancho por byte (`inotia3_char_width`) que ya usan `GetStringWidthHeightInRect_stub`/
> `DrawTextInRect_stub` desde la Corrida 44/59 — mismo criterio de esas Corridas: esta fuente bitmap propia
> tiene una sola métrica fija, no una escalable por `fontSize`/`fontStyle`, así que esos dos parámetros se
> aceptan (para respetar la firma real) pero se ignoran, igual que en los otros dos stubs de texto. Sin
> wrapping (la implementación Java real es una sola llamada a `measureText`, sin rectángulo). Build
> compilado y verificado (cross real vía VitaSDK, en un árbol aislado bajo `/tmp`, `inotia3.vpk`/
> `eboot.bin`/`inotia3.elf`, 2026-08-10) — sin errores nuevos.
>
> **Importante — esto es un candidato respaldado por decompile, NO una confirmación de hardware.** No hubo
> acceso a consola/Vita3K real ni screenshot del bug en esta sesión (instrucción explícita del entorno). Es
> posible que el reporte del usuario se refiera a un elemento de UI que NUNCA tuvo fondo en el juego
> original (por diseño, ej. etiquetas flotantes) y que esto sea una lectura errónea del síntoma, o que haya
> una SEGUNDA causa (ej. un asset de sprite de panel que falla al cargar) coexistiendo con esta. Tampoco se
> descartó del todo la hipótesis de la máscara binaria sin antialiasing (Corrida 59's propio diseño) como
> un factor secundario de legibilidad, aunque no explica la AUSENCIA de fondo por sí sola.
>
> **Pendiente**: redeployar, reproducir, mandar el log nuevo — y, sobre todo, **una screenshot del usuario
> mostrando exactamente qué texto se ve así** (¿menú? ¿diálogo? ¿tooltip de ítem? ¿etiqueta de HP/nombre?)
> para poder identificar el call site concreto en `out_ghidra.c` y confirmar (o descartar) este fix contra
> el elemento real. Si el bug persiste tras este fix, instrumentar `SGL_GetAsciiTextSize`/
> `__utilGetStringWidth` con un log del ancho devuelto sería el siguiente paso natural (mismo estilo que los
> hooks de la Corrida 58/61/64 en otras sesiones de este mismo repo). También pendiente: esta entrada usa la
> numeración real de `source/*.c` (60), que iba adelantada a este documento (quedado en la 47) — las
> Corridas 48-59 existen en el código (`GetStringFromXmlEx`/`GetStringFromXml`/`DrawTextInRect`, etc.) pero
> no tienen entrada narrativa aquí; no se reconstruyeron retroactivamente en esta sesión por estar fuera de
> alcance de este bug.

> **Corrida 65 (2026-08-10) — Reporte de usuario tras probar el build de las Corridas 60-64: "el guardado
> dice éxito pero no persiste realmente", "el táctil se vuelve a congelar al cambiar de stage, y ahora es
> peor que antes", y "no se generó ningún log de esa sesión". Sin log ni `.psp2dmp` de esa corrida
> específica para confirmar con datos reales -- lo de abajo es una revisión de código que encontró un bug de
> concurrencia real (no solo una hipótesis) y dos endurecimientos defensivos, no una causa raíz confirmada
> en hardware.**
>
> La pista más importante es la ausencia total de log: en el congelamiento ORIGINAL (el de `log_006.txt`,
> antes de estas 5 Corridas), el bucle de render/timer seguía corriendo mientras el juego quedaba trabado
> (`log_006.txt` mostraba el mismo ciclo de `glOrthox`/`Flush`/`isUsableNetwork` repitiéndose para siempre) --
> eso generaba líneas de log sin parar, o sea que el logger seguía vivo. Que AHORA no se genere ningún log
> apunta a algo cualitativamente distinto: un colgado real del hilo principal completo (nada corre, ni
> siquiera el logging), no solo la lógica de juego.
>
> **Bug de concurrencia real encontrado en `source/audio.c` (Corrida 63): el hilo mezclador de audio
> (`audio_thread`) llama a `ov_read()` -- una lectura de archivo BLOQUEANTE contra el mismo storage
> `ux0:` que usa el resto del juego -- mientras tiene tomado `audio_mutex`.** Las 6 funciones de control
> (`audio_sound_play/pause/resume/stop/destroy/set_volume`), invocadas SIEMPRE desde el hilo principal del
> juego vía una llamada JNI de `Sound*` síncrona, necesitan ese MISMO mutex solo para tocar unos pocos campos
> de struct -- pero antes de este fix esperaban con `sceKernelLockMutex(audio_mutex, 1, NULL)` (`NULL` =
> esperar para siempre). Si `ov_read()` se traba (una tarjeta lenta, o presión de I/O real -- exactamente lo
> que un cambio de stage genera: assets nuevos cargándose en simultáneo con la música/SFX de transición
> arrancando) mientras el mixer tiene el mutex tomado, CUALQUIER llamada `Sound*` posterior desde el hilo
> principal (y un cambio de stage dispara varias, para frenar la música vieja y arrancar la nueva) se queda
> esperando ESE MISMO mutex para siempre -- congelando el hilo principal COMPLETO (input, render, y logging,
> todo corre ahí) en vez de solo la lógica de juego. Encaja exactamente con "se congela justo al cambiar de
> stage" + "no hay ningún log de esa sesión" a la vez.
>
> **Fix aplicado** (`source/audio.c`): las 6 funciones de control ahora usan
> `sceKernelLockMutex(audio_mutex, 1, &timeout)` con un timeout acotado (`AUDIO_LOCK_TIMEOUT_US`, 100ms) en
> vez de `NULL`. Si el mixer sigue trabado más de 100ms, la función loguea una advertencia y retorna un error
> en vez de esperar para siempre -- en el peor caso se pierde/retrasa un `Sound*` puntual, nunca el juego
> entero. El propio hilo mezclador SIGUE esperando sin límite por el mutex en su bucle (`NULL`, sin cambios)
> -- ahí no hay riesgo, porque cuando el mixer espera no bloquea al hilo principal.
>
> **Segundo cambio, defensivo/de alcance, en `source/patch.c` (Corrida 62's `CS_fsOpen_hook`)**: el fix
> original corría su lógica de `file_mkpath()` en TODA apertura de archivo con flag de creación en TODO el
> juego, incluidas las que nunca podían necesitarlo (archivos sueltos directamente bajo
> `ux0:data/inotia3/`, cuyo directorio padre siempre existe). Ahora se activa solo si `name` realmente tiene
> un componente de subdirectorio. `jGetUserDir`/`jGetPackageName` (llamadas desde este hook) resultaron
> cachear su resultado tras la primera llamada real (`out_ghidra.c:146025`/`146056`, confirmado leyendo el
> decompile) así que NO son responsables de round-trips de JNI repetidos como se sospechó al principio -- pero
> acotar el hook de todas formas reduce su huella al único caso que realmente lo necesita, y elimina cualquier
> trabajo redundante en el camino de arranque de cada archivo del juego.
>
> **Tercer cambio, defensivo, en `source/utils/logger.c`**: el buffer en anillo de la Corrida 60 solo se
> vaciaba a disco cuando se llenaba o ante un WARN/ERROR/FATAL -- si el hilo principal se cuelga del todo
> antes de que pase cualquiera de las dos cosas, lo que quedó en el anillo se pierde para siempre, sin ruta
> de apagado limpio que lo salve. Se agregó un flush también por tiempo transcurrido (cada 500ms,
> `sceKernelGetProcessTimeWide()`), acotando cuánto puede perderse a una ventana fija en vez de depender de
> cuánto tarde en juntarse 4KB de líneas debug/info (que, lejos de la seguidilla de llamadas JNI del arranque,
> puede tardar bastante más de lo que suena).
>
> **Nada de esto está confirmado contra la corrida real que el usuario reportó** -- no hubo log ni `.psp2dmp`
> de ESA sesión para verificar la hipótesis del mutex de audio con datos reales; es la explicación más
> plausible que sostiene el código en sí (un mutex esperado sin límite exactamente en el punto donde el
> síntoma reportado ocurre), no una confirmación. Tampoco se investigó todavía por qué el guardado "dice
> éxito pero no persiste" -- sigue siendo el riesgo ya anotado en la Corrida 62 ("si `file_mkpath()` tiene
> éxito pero el contenido real no queda bien, el problema está más abajo en la cadena, p.ej. el pool de
> `SAVE_AllocateMemory` o el contenido cifrado") y no se tocó en esta Corrida.
>
> Build compilado y verificado (cross real vía VitaSDK, en un árbol `/tmp` aislado, `inotia3.vpk`/
> `eboot.bin`/`inotia3.elf`, 2026-08-10) -- sin errores ni warnings nuevos.
>
> **Pendiente**: redeployar, reproducir el cambio de stage, y esta vez SÍ mandar el log completo (incluso
> parcial) -- con el flush por tiempo agregado arriba, debería sobrevivir algo útil aunque el juego se cuelgue
> de nuevo. Si el mutex de audio era la causa real, el síntoma debería desaparecer (o, en el peor caso,
> degradar a "se cortó un sonido" en vez de "se congeló todo"). Buscar además una línea `[Audio] Sound*:
> timed out waiting for audio_mutex` si el timeout SÍ se activó -- confirmaría la hipótesis incluso sin que el
> juego llegue a congelarse del todo. Sobre el guardado, confirmar en la próxima consola/prueba si el archivo
> `ux0:data/inotia3/<hash>/save<N>.dat` realmente existe y tiene contenido después de guardar -- eso separa
> "el open() ahora funciona pero el contenido está mal" de "sigue sin escribirse nada".

> **Corrida 66 (2026-08-10) — Pedido de usuario: mapear los botones físicos de la PS Vita a los controles
> táctiles del juego (imagen de referencia `inotia-3-children-of-carnia_1.jpg`, 960x640), para poder jugar
> sin tocar la pantalla.** Coordenadas medidas a mano sobre esa captura (grillas superpuestas con PIL para
> precisión) y convertidas al canvas fijo 960x544 de este port (`GetScreenWidth`/`GetRealScreenWidth` y el
> par de Height están stubeados exactamente a eso en `java.c`) por anclaje a bordes, no por escalado
> proporcional: los HUDs móviles casi universalmente se ubican con un margen fijo respecto al borde más
> cercano (los pulgares descansan en las esquinas sin importar la altura real de pantalla), no estirados
> por toda la altura -- así que el botón de pausa (zona superior) conserva su distancia exacta al borde
> SUPERIOR sin escalar, mientras que el cluster de movimiento/ataque/skills/items (zona inferior) conserva
> su distancia exacta al borde INFERIOR, corrida hacia arriba por la diferencia 640-544=96px. El ancho no
> necesitó ningún ajuste (960=960 en ambas resoluciones). **Esto NO está verificado en hardware** -- es una
> primera estimación; puede necesitar ajuste fino de coordenadas tras probarlo en consola/Vita3K.
>
> **Mapeo aplicado**:
> | Botón físico | Control táctil |
> |---|---|
> | D-Pad ↑/↓/←/→ | Movimiento (las 4 flechas del d-pad táctil, izquierda de pantalla) |
> | ✕ (Cross) | Ataque principal (círculo grande de la mira, esquina inferior derecha) |
> | ○ (Circle) | Skill (ícono de cruz dorada) |
> | □ (Square) | Skill (ícono oscuro/espinoso) |
> | △ (Triangle) | Ítem/escudo (ícono de diamante) |
> | L | Poción (primer slot de ítem, con contador) |
> | R | Segundo slot de ítem |
> | SELECT | Tercer slot de ítem |
> | START | Pausa |
>
> **Implementación** (`source/wr_touch.c`/`.h`): nueva función `wr_process_buttons()`, llamada cada tick
> junto a `wr_process_touch()` (`source/main.c`). Lee `sceCtrlPeekBufferPositive()`, detecta transiciones de
> estado (press/release) contra el frame anterior, y por cada transición sintetiza un `WR_TOUCH_DOWN`/
> `WR_TOUCH_UP` en la coordenada fija de la tabla `BUTTON_MAP`, usando el MISMO pool de 5 slots que
> `wr_process_touch()` ya usa para el touch real (`slotHwId[]`/`slotX[]`/`slotY[]`, estáticos del archivo) --
> así un dedo real y varios botones físicos presionados a la vez comparten el mismo límite de 5 puntos
> simultáneos que el propio `MultiTouchListener` de Android ya impone, en vez de arriesgarse a corromper los
> arreglos de tamaño fijo del motor pasándole un id fuera de rango (el mismo riesgo que el header de
> `wr_touch.h` ya documentaba para el touch real). Cada botón usa un `virtual_hw_id` fijo >= 1000 (el id de
> hardware real de `SceTouchReport` es un contador de 8 bits, así que nunca puede colisionar). No se
> implementó press-and-drag ni los sticks analógicos -- los botones son de posición fija, no necesitan
> arrastre, y el pedido no mencionó los sticks.
>
> Build compilado y verificado (cross real vía VitaSDK, en un árbol `/tmp` aislado, `inotia3.vpk`/
> `eboot.bin`/`inotia3.elf`, 2026-08-10) -- sin errores ni warnings nuevos.
>
> **Pendiente**: redeployar y probar cada botón contra su control táctil real. Si alguno no dispara la
> acción esperada (o dispara la de un botón vecino), el problema más probable es que la hipótesis de
> anclaje a bordes no sea exacta para ese elemento puntual -- ajustar su `x`/`y` en `BUTTON_MAP`
> (`source/wr_touch.c`) un poco hacia el centro del círculo real visto en pantalla y volver a compilar.

> **Corridas 67-71 (2026-08-11) — Cuatro reportes del usuario tras probar el build de las Corridas 60-66:
> el freeze al cambiar de stage sigue igual, sigue sin generarse ningún log nuevo, la cruceta física sólo
> registra un toque en vez de quedar "fija" mientras se mantiene presionada, y un nuevo crash (Data Abort)
> al cargar una partida guardada -- reportado junto con "el guardado ya persiste de verdad", confirmando
> que el fix de guardado de la Corrida 62/65 funcionó. Investigado en paralelo por un agente dedicado por
> bug, cada uno con alcance de archivo separado; build final cruzado (VitaSDK real, árbol `/tmp` aislado)
> con los cinco fixes juntos, sin errores nuevos (sólo el warning preexistente de `mktemp` en `dynlib.c`,
> no relacionado).**
>
> **Corrida 67 -- cruceta física se registra como un toque, no como sostenida (`source/wr_touch.c`).**
> Causa real, no hipótesis: el loop de limpieza de fin-de-frame de `wr_process_touch()` libera cualquier
> slot que no haya aparecido en los reportes de `sceTouchPeek()` de ESE frame -- pero los slots virtuales
> que `wr_process_buttons()` (Corrida 66) usa para los botones físicos (`virtual_hw_id >= 1000`) nunca
> pueden aparecer ahí, porque no son toques reales. Como `wr_process_touch()` corre ANTES que
> `wr_process_buttons()` en el loop principal (`source/main.c`), el frame siguiente a cada botón presionado
> ese mismo loop de limpieza lo daba por "no visto" y disparaba un `WR_TOUCH_UP` espurio, liberando el slot
> -- aunque el botón físico siguiera abajo (`wr_process_buttons()` no volvía a mandar nada porque, para su
> propia lógica de transición, el botón no había cambiado de estado). Resultado exacto del reporte: cada
> botón se traducía en un DOWN inmediatamente seguido de un UP, sin importar cuánto se mantuviera
> presionado. **Fix**: el loop de limpieza ahora ignora explícitamente los slots con
> `slotHwId >= WR_VIRTUAL_HW_ID_BASE` (1000) -- esos son propiedad exclusiva de `wr_process_buttons()`, que
> ya los libera correctamente en su propia transición press/release. Toque real sin cambios de
> comportamiento.
>
> **Corrida 68 -- logs siguen sin generarse en un colgado real del hilo principal
> (`source/utils/logger.c`/`.h`, `source/main.c`).** El flush periódico de la Corrida 65
> (`LOG_FLUSH_INTERVAL_US`) sólo se evalúa DENTRO de `_log_print()` -- en un colgado real del hilo
> principal (input, render Y logging corren ahí) nada vuelve a llamar `_log_print()`, así que ese chequeo
> nunca se re-evalúa y lo que quedó en el ring desde el último flush se pierde para siempre; exactamente
> el mecanismo que la Corrida 65 necesitaba y no podía dar. **Fix**: nuevo hilo watchdog de baja prioridad
> (`logger_start_watchdog()`, arrancado como la primera línea de `main()`, antes de `soloader_init_all()`)
> que hace `sceKernelDelayThread(250ms)` + flush del ring en loop, independiente de que el hilo principal
> siga vivo o no. Toma `_log_mutex` con un timeout acotado (`LOG_WATCHDOG_LOCK_TIMEOUT_US`, 50ms, mismo
> patrón que `AUDIO_LOCK_TIMEOUT_US` de la Corrida 65) -- si está contendido, se salta ese ciclo sin
> bloquear. De paso se cerró un riesgo simétrico que este mismo cambio introducía: `_log_print()` ahora
> podía competir por `_log_mutex` con el watchdog, así que su propio lock (antes un `sceKernelLockLwMutex`
> con timeout `NULL`, espera infinita) también pasó a estar acotado
> (`LOG_PRINT_LOCK_TIMEOUT_US`, 100ms) -- de lo contrario este mismo fix podría convertirse en una NUEVA
> forma de colgar el hilo principal.
>
> **Corrida 69 -- crash al cargar una partida guardada (`source/patch.c`, `source/java.c`).** Auditoría de
> dos frentes:
> - **Causa raíz confirmada por disassembly real** (no hipótesis): la base auto-detectada por la
>   herramienta de análisis del `.psp2dmp` (`0x9805c000`) es la misma heurística ya marcada como poco
>   confiable en las Corridas 54/55/56. Re-derivando la base correcta de la misma forma que esas Corridas
>   (comparando la página de memoria de 8KB guardada en el dump contra el `.so` real byte a byte) da
>   `0x98000000` -- con esa base, PC (`0x980a8310`) es `SAVE_Load+0x44` y LR (`0x980a8305`) es
>   `SAVE_Load+0x39` (`nm -D`/`objdump -d -M force-thumb` contra `inotia3-1-3-2/lib/armeabi/libgame.so`,
>   instrucción por instrucción contra `out_ghidra.c:82873-82914`). La instrucción exacta en el PC es
>   `ldrsh r0, [r3, r2]` con `r3` = `PLAYER_pActivePlayer` (confirmado, R5 del dump apunta exactamente a
>   ese símbolo real vía `nm -D`) y `R3=0` en los registros del crash -- un NULL-deref liso de ese puntero
>   global al construir los argumentos de `MAP_SetFocus`. `SAVE_Load` nunca chequea el valor de retorno de
>   `SAVE_LoadCharacterAll`, que puede terminar sin haber llamado nunca a `PLAYER_SetActivePlayer` (deja
>   `PLAYER_pActivePlayer` en NULL) si `SAVE_LoadData` falla o ningún slot de personaje carga bien -- un gap
>   real del motor original que las Corridas 62/65 (arreglando el guardado) volvieron alcanzable por
>   primera vez: antes, `SAVE_Save()` siempre fallaba, así que nunca existió un `save<N>.dat` real con datos
>   de slot/personaje activo para cargar.
> - **Fix**: `SAVE_Load_hook` reimplementa `SAVE_Load` fielmente (mismo patrón ya usado para
>   `RES_LoadToPool`/`MAP_LoadFromHeap` en Corridas 42/47/56 -- reimplementar toda la función, arreglar sólo
>   el chequeo que falta), pero si `PLAYER_pActivePlayer` sigue en NULL tras `SAVE_LoadCharacterAll`, cae a
>   `PLAYER_pMainPlayer` vía `PLAYER_SetActivePlayer` real -- el mismo fallback que el motor sin tocar ya
>   hace en la cola de `SAVE_LoadCharacterAll` (`out_ghidra.c:82852-82865`), aplicado aquí para el camino que
>   la salta. Si ambos punteros son NULL (no sobrevivió ningún dato de personaje), se salta
>   `MAP_SetFocus`/`PLAYER_MakeShortcut` y se loguea con `l_fatal` en vez de crashear.
> - **Endurecimiento adicional** (`source/java.c`, `GetAppProperty_stub`): investigado en paralelo,
>   `GetAppProperty_stub` devolvía un valor fijo no vacío para CUALQUIER clave pedida, incluyendo
>   `uniqueUserId`/`uniqueDeviceId`/`sessionKey` (las únicas que `DownloadGameData`
>   (`out_ghidra.c:16260`) chequea antes de decidir si hay sesión de Hub activa) -- rompiendo siempre el
>   chequeo de "no hay Hub en este port" que el resto del port ya asume (`HubInitialize_stub`). Eso podía
>   enrutar la acción de Cargar Partida hacia `CS_hubDownloadGameData`, una llamada real del SDK de Hub
>   cuyo estado interno nunca se inicializa en este port headless -- un segundo camino de crash plausible,
>   independiente del de `SAVE_Load`, aunque no se pudo confirmar byte a byte contra el dump (cae dentro de
>   `.dynstr`, no en código). Ahora devuelve un array vacío para toda clave, restaurando la señal de "no
>   logueado" y bloqueando ese camino por completo.
>
> **Corrida 70 -- freeze de stage transition SIGUE ocurriendo tras el fix de mutex de audio de la Corrida
> 65 (`source/patch.c`, `source/audio.c`).** Auditoría estática (no hay log/dump nuevo de ESTE freeze
> específico -- un colgado no genera `.psp2dmp`, y hasta la Corrida 68 tampoco dejaba log): se revisaron
> `MAP_Load_hook`/`MAP_LoadFromHeap_hook`/`MAP_LoadAllToHeap_hook`/`MAP_DestroyFromHeap_hook`,
> `GAMESTATE_SetState_hook`/`EVTSYSTEM_SetState_hook`, `CS_fsOpen_hook`, y los hooks de carga de recursos
> (`RES_LoadToPool_hook`, `RES_MoveToFilePack_hook`, `EXCELDATA_LoadFullResource_hook`,
> `UTIL_LoadSpriteListFromResource_hook`, `FONT_Load_hook`) buscando otra espera bloqueante sin límite --
> ninguno mostró una señal tan clara como la del mutex de audio original. En su lugar se agregó logging de
> entrada/salida (`l_debug`, prefijo "Corrida 68") alrededor de `GAMESTATE_SetState_hook`/
> `EVTSYSTEM_SetState_hook`/`MAP_Load_hook` para que el PRÓXIMO freeze (que ya debería dejar log gracias a
> la Corrida 68) señale exactamente dónde se queda trabado.
>
> Revisando por separado `source/audio.c` SÍ apareció una causa real y confirmable, no sólo una
> hipótesis: `audio_sound_play()`/`audio_sound_prepare()` hacían `fopen()` + `ov_open()` -- lectura de
> archivo bloqueante contra el mismo `ux0:` que usa el resto del juego -- COMPLETAMENTE FUERA de cualquier
> mutex, corriendo síncronamente en el hilo principal (llamadas `Sound*` desde JNI). El timeout de la
> Corrida 65 (`AUDIO_LOCK_TIMEOUT_US`) sólo protege la sección de mutación de struct que pasa DESPUÉS de
> que el archivo ya está abierto -- no hace nada contra este bloqueo previo, que no tenía límite de ningún
> tipo. Exactamente durante una transición de stage (ráfaga de I/O nueva: música/SFX de la transición
> arrancando mientras cargan assets nuevos) esto puede colgar el hilo principal completo -- coincide
> estructuralmente mejor que cualquier otro candidato encontrado con "se congela siempre al cambiar de
> stage, no sobrevive ningún log". **Fix**: el `fopen`/`ov_open` real se movió al hilo mezclador
> (`audio_thread`) que ya existía -- `audio_sound_play()` ahora sólo reserva un slot de voz bajo el mutex
> (rápido, sin I/O) y deja un pedido pendiente; `audio_thread` hace la apertura real fuera del mutex en
> cada iteración de su loop y la instala bajo mutex, validando con un token que el slot no haya sido
> robado/detenido/destruido mientras la apertura estaba en curso. `audio_sound_prepare()` pasó de
> `fopen`+`ov_open`+`ov_clear` a un simple `stat()` (la validación de decodificación real ya ocurre de
> todas formas, de forma asíncrona, en `audio_sound_play()`).
>
> **Pendiente (las 5 Corridas)**: redeployar y probar cada bug puntualmente. Cruceta y logs deberían
> confirmarse fácil (mantener un botón unos segundos; y revisar que aparezca un `log_NNN.txt` nuevo incluso
> si el juego se cuelga). Guardado: cargar la partida que crasheaba antes y confirmar que ahora carga (o,
> en el peor caso, que el log muestra el `l_warn`/`l_fatal` de `SAVE_Load_hook` en vez de un
> `.psp2dmp`). Freeze de stage: si el fix de `audio.c` era la causa real, el síntoma debería desaparecer
> del todo; si NO era la causa (o era una de varias), el log ahora debería sobrevivir el colgado gracias a
> la Corrida 68/67 y mostrar en qué línea de `GAMESTATE_SetState_hook`/`EVTSYSTEM_SetState_hook`/
> `MAP_Load_hook` se quedó trabado -- mandar ese log completo es el siguiente paso más útil si el freeze
> persiste.

> **Corridas 71-72 (2026-08-11) — Primer log real de una sesión de consola (`logs/log_008.txt`, gracias a
> las Corridas 67/68) y el primer `.psp2dmp` de un freeze de stage transition (antes solo colgaba sin
> dejar rastro). También: la partida guardada que el usuario tenía desapareció tras probar el build de las
> Corridas 67-70, y D-Pad arriba confirmado sin efecto en el juego (las otras 3 direcciones sí funcionan).**
>
> **Corrida 71 -- SEGUNDO divide-by-zero distinto en una transición de stage, no atrapado por el hook de la
> Corrida 54 (`source/patch.c`).** `log_008.txt` mostró sólo `"Corrida 53: raise(8) -- .so return
> address=0x9811e114"` -- la misma dirección que la Corrida 54 ya había resuelto una vez, pero esa
> resolución histórica tenía la aritmética mal (0x9811e114-0x98000000=0x11e114, no 0x11ce64 como decía el
> texto viejo). Re-derivada con disassembly ARM real (no Thumb -- `__div0` y las primitivas de división
> alrededor son código ARM puro): `0x11e114` es `__div0+0xc`, la instrucción justo después de `bl raise`
> dentro de `__div0` mismo (`push {r1,lr}; mov r0,#8; bl raise; pop {r1,pc}`). `__div0` es el choque común
> real al que TODAS las primitivas de división de este `.so` llaman (`__udivsi3`, `__divsi3`,
> `__aeabi_idivmod`, `__aeabi_uidivmod`, `__aeabi_ldivmod`, `__aeabi_uldivmod`, `__divdi3`, `__udivdi3`,
> confirmado disassemblando varias) -- el hook de la Corrida 54 sólo envolvía `__udivsi3` puntualmente, así
> que cualquier división por cero a través de OTRA primitiva (esta vez, probablemente `__divsi3`/
> `__aeabi_idivmod` con signo, dado el contexto de transición de stage) nunca pasaba por ese hook y llegaba
> directo a `__div0` → `raise(8)` → la cadena de auto-trap deliberada de la Corrida 53. **Fix**: hookeado
> `__div0` directamente (`div0_hook`) -- cada llamador YA sobreescribe su resultado con un `mov r0,#0`
> fijo apenas vuelve de `bl __div0`, así que no llamar al `raise()` real y simplemente retornar es fiel a
> como cada llamador ya maneja este caso, y neutraliza esta clase de crash completa para las 8 primitivas
> a la vez, no sólo `__udivsi3`. Loguea con `l_warn` (ya no es fatal, es un caso manejado) la dirección de
> retorno de quien llamó a la primitiva de división -- identifica cuál primitiva se usó, un nivel por
> debajo de la función real del juego, suficiente para un hook puntual futuro si hace falta identificar
> esa función exacta.
>
> **Corrida 72 -- la partida guardada desapareció: regresión real de la Corrida 69, revertida
> (`source/java.c`, `source/patch.c`).** Auditoría de `SAVE_Load_hook` línea por línea contra
> `out_ghidra.c:82873-82914`: fiel al original, no es la causa. Causa real encontrada: `HubSave_LoadKey()`
> (`:124296`) pide la clave `"uniqueUserId"` vía el MISMO `GetAppProperty_stub` que la Corrida 69 cambió de
> un valor fijo no vacío (`"123456789012"`) a un array vacío -- y `HubSave_GetFolderName()` (`:124328`)
> nombra el subdirectorio del save real como `MD5(HubSave_key)`. Vaciar la clave cambió ese hash de
> `MD5("123456789012")` a `MD5("")`: el juego ahora busca (y escribe) en una carpeta distinta a la de
> siempre -- la partida vieja no se borró, el juego simplemente mira en otro lado. La hipótesis original
> de la Corrida 69 (que un valor no vacío reabría un camino de crash hacia `CS_hubDownloadGameData`, un
> Hub-download nunca inicializado) nunca se confirmó byte a byte -- la causa real y confirmada del crash de
> guardado fue siempre `SAVE_Load`'s propio NULL-deref (Corrida 69, la mitad SÍ confirmada), independiente
> de este valor. **Fix de dos capas**: (1) `GetAppProperty_stub` revertido al valor fijo no vacío original
> (restaura el hash de carpeta de siempre, arregla la regresión); (2) en vez de depender de ese mismo valor
> compartido para mantener cerrado el camino de Hub (la causa exacta de esta regresión), se hookearon
> directamente `CS_hubDownloadGameData`/`CS_hubUploadCompleteGameData`/`CS_hubUploadGameData` como no-ops
> que retornan error (mismo patrón que `HubInitialize_stub`, "no hay Hub real en este port") -- rastreado
> el único llamador real de `DownloadGameData` (`C2S_HubSaveDataDownload`, `:16467`, una acción de menú de
> sync a la nube, NO el flujo local de "Cargar Partida" que usa `SAVE_Load`) para confirmar que esto no
> toca el camino de guardado local en absoluto.
>
> **D-Pad arriba, sin fix -- se necesita más investigación fuera del alcance revisado esta corrida.**
> Remedido `inotia-3-children-of-carnia_1.jpg` por centroide de brillo (no a ojo): la coordenada de UP ya
> es la MÁS precisa de las 4 direcciones (~2px de error vs. ~7-9px en las otras 3, que SÍ funcionan), y las
> 4 son simétricas respecto de un centro común -- descarta un d-pad asimétrico o un error de medición. No es
> un problema de puntería de coordenadas. Candidato más probable: el `.so` podría testear el d-pad por
> ángulo/zona desde un centro virtual en vez de por rect fijo por flecha, y el punto de UP caería justo en
> un borde de esa zona -- requiere revisar `out_ghidra.c` para la lógica real de touch-dispatch del d-pad,
> no solo la imagen de referencia. Pendiente para una corrida futura.
>
> Build final (los 3 fixes de código de esta ronda juntos) compilado y verificado (VitaSDK real, árbol
> `/tmp` aislado) sin errores nuevos.
>
> **Pendiente**: redeployar. Confirmar que el freeze/crash de stage transition ya no ocurre (Corrida 71).
> Confirmar que la partida guardada vuelve a aparecer (Corrida 72 -- debería, dado que usa el mismo hash de
> carpeta de siempre otra vez). D-Pad arriba sigue roto hasta la próxima corrida -- si se puede, mandar un
> log de una sesión donde se presione específicamente arriba varias veces, útil para la investigación de
> ángulo/zona pendiente.

> **Corrida 73 (2026-08-11) — Explicación real de por qué el fix de la Corrida 71 "no funcionó": el build
> probado era Release, no Debug, y casi todo el logging de este port es un no-op en Release
> (`source/patch.c`).** El usuario reinstaló el VPK reconstruido con el hook de `__div0` y confirmó
> `div0_hook` presente en el binario (`nm` sobre `build/inotia3.elf`) -- pero `logs/log_008.txt` seguía
> mostrando la misma línea vieja `"Corrida 53: raise(8)"` sin la advertencia nueva, y sin `.psp2dmp` nuevo
> esta vez. Investigado a fondo (mecanismo de `hook_addr`/`hook_arm` en `lib/so_util/so_util.c` re-verificado
> línea por línea, `so_flush_caches()` confirmado corriendo justo después de `so_patch()` en
> `source/utils/init.c`, tags ARM/Thumb de los símbolos re-confirmados vía `nm -D` fresco -- todo correcto,
> no hay bug en el mecanismo de hooking). **La causa real**: `source/utils/logger.h` define
> `l_debug`/`l_info`/`l_warn`/`l_success`/`l_wait` como macros vacías salvo que `DEBUG_SOLOADER` esté
> definido, y `CMakeLists.txt` sólo lo define si `CMAKE_BUILD_TYPE=Debug` -- el prompt interactivo de
> `build_and_install.sh` se respondió "Release" en los dos builds probados hasta ahora esta sesión. Sólo
> `l_error`/`l_fatal` compilan siempre. Esto significa: (1) el propio `div0_hook` (un solo `l_warn`) se
> volvía una función vacía en Release -- pero AÚN ASÍ debería seguir suprimiendo el crash correctamente,
> porque simplemente no llamar a `raise()` y retornar ya alcanza, sin necesitar loguear nada; (2)
> `_log_print()` sólo se llama alguna vez vía `l_error`/`l_fatal` en Release, y crea el archivo de log
> perezosamente en su primera llamada -- una sesión sin ningún error/fatal genuino NO CREA ningún archivo
> nuevo en absoluto; `log_008.txt` es sobrante de la corrida de crash ANTERIOR (antes del fix), y el
> pull-de-logs simplemente agarró el archivo con el número más alto que existe en la consola, que seguía
> siendo el 008 porque nunca se creó uno nuevo; (3) el logging de diagnóstico de la Corrida 68
> (`GAMESTATE_SetState_hook`/`EVTSYSTEM_SetState_hook`/`MAP_Load_hook`, todo `l_debug`) tampoco generaba
> nada en Release -- el síntoma separado de "táctil/botones muertos al cambiar de stage" sigue sin ninguna
> evidencia de log todavía, no se debe forzar una conclusión sin datos reales.
>
> **Fix aplicado**: subidos `div0_hook` y las 6 llamadas `l_debug` dentro de
> `GAMESTATE_SetState_hook`/`EVTSYSTEM_SetState_hook`/`MAP_Load_hook` a `l_error` (nunca gateado por
> `DEBUG_SOLOADER`, siempre flushea de inmediato) -- así la próxima prueba deja evidencia real incluso si
> se vuelve a compilar en Release por error. **Recomendación fuerte para toda prueba futura de esta
> sesión**: compilar en Debug (responder "s"/"S" al prompt del script, NO "n") -- el build que se está
> desplegando ahora mismo (2026-08-11, ~10:56) SÍ es Debug, confirmado por `grep DEBUG_SOLOADER
> CMakeLists.txt` + la condición real.
>
> **Corrida 74 (2026-08-11) — "El guardado dice éxito, pero al cargar está en blanco": instrumentado, no
> confirmado todavía (`source/patch.c`).** Leído `SAVE_LoadCharacterAll` completo (`out_ghidra.c:82757-
> 82869`, no sólo `SAVE_Load`): tiene su PROPIO camino de recuperación legítimo, no un error --
> (`:82852-82856`) si ningún miembro del grupo está vivo (flag alive==1 y HP>0), pone
> `SAVE_nLiveMercenarySlot=0xff`, deja `PLAYER_pActivePlayer`/`PLAYER_pMainPlayer` AMBOS en NULL, y
> retorna **1 (éxito)** -- no un fallo. El fallback de la Corrida 69 en `SAVE_Load_hook` (que dispara
> exactamente con esa condición NULL/NULL) no puede distinguir hoy entre "el parseo del archivo realmente
> falló" y este caso completamente distinto y no-erróneo ("todo el grupo está muerto/sin HP"). Se descartó
> una hipótesis de asimetría de clave de encriptación entre `SAVE_SaveDataAsNameAndKey`/
> `SAVE_LoadDataAsNameAndKey` (la firma decompilada de la de guardar le falta un argumento de
> `ENCRYPT_Process2` que la de cargar sí tiene) -- confirmado que es un artefacto de recuperación de firma
> de Ghidra (cada OTRO call site de `ENCRYPT_Process2` usa 4 args), no una asimetría real en el ARM
> compilado. Ningún log existente (incluyendo `log_008.txt`) cubre una sesión de guardado/carga real -- no
> hay datos para distinguir los dos casos todavía. **Cambio aplicado**: `SAVE_Load_hook` ahora captura el
> valor de retorno real de `SAVE_LoadCharacterAll_real` (advierte si es 0 -- fallo de parseo genuino,
> apunta al contenido/encriptación del archivo) y resuelve+loguea `SAVE_nLiveMercenarySlot` en el mismo
> fallback NULL/NULL (0xff ahí = el parseo salió bien y esto es el resultado propio del motor para "grupo
> muerto", el bug estaría más arriba, en por qué todo el grupo terminó muerto/sin HP al guardar; cualquier
> otro valor apunta a otra causa). No arregla un bug confirmado todavía -- convierte el próximo log real
> del usuario en una respuesta en vez de otra suposición.
>
> Build (Corridas 73/74 juntas, en modo Debug esta vez) compilado y verificado sin errores nuevos.
>
> **Pendiente**: redeployar el build de Debug de esta corrida y reproducir AMBOS reportes (crash/freeze de
> stage transition, y guardar+cargar) en la misma sesión si es posible, para tener por fin un log real y
> completo de los dos. Sin eso, tanto "sigue habiendo táctil muerto al cambiar de stage" como "por qué el
> grupo llega muerto al guardar" siguen abiertos.

> **Corrida 75 (2026-08-11) — Encontrada y arreglada la causa REAL del freeze de stage transition, gracias
> al primer log de Debug completo de esta sesión (`logs/log_009.txt`, 64680 líneas -- el fix de la Corrida
> 73 para el gap Release/Debug funcionó).** El usuario reportó: "el mundo carga" pero los botones físicos
> y el táctil dejan de responder por completo, sin crash. Las últimas ~11000 líneas del log son EXACTAMENTE
> un ciclo repitiéndose para siempre: `WrapperJinterface.SetTimer` → 2x `glOrthox` → una búsqueda JNI de
> `"isUsableNetwork"` (nunca registrada, "not found") → `WrapperJinterface.Flush` → `SetTimer` de nuevo --
> el bucle de render/timer sigue vivo (coincide con la firma del freeze ORIGINAL pre-Corrida-65, no un
> colgado real del hilo), pero nada sale nunca de ese ciclo específico.
>
> **Causa raíz confirmada contra `out_ghidra.c`**: `GAMESTATE_DrawMapChange` (`:8333`) dibuja el banner de
> anuncio y llama a `jIsUsableNetwork()` (resultado descartado, sólo un side-effect) TODOS los frames
> mientras `onAd==1`, coincidiendo exacto con el ciclo del log. `GAMESTATE_ProcessMapChange` (`:8359`), la
> máquina de estados hermana, es donde realmente se traba: al llegar `MAPCHANGE_nFrame` a 4,
> `Extend_CheckAdvDisplay()` corre y -- salvo que `adBlocked` sea distinto de cero -- 1 de cada 3 transiciones
> (`adCounter % 3 == 0`) pone `onAd=1` y llama `DisplayCenterAd()` (`:117473`), que pone `adAvailable=1`
> INCONDICIONALMENTE y de inmediato (sin ida y vuelta real a ningún SDK de anuncios -- los stubs JNI de
> anuncios de este port son no-ops, CLAUDE.md ya los marca "no-op-until-proven-necessary"). El ÚNICO código
> que vuelve a poner `adAvailable=0` es `ProcessResume` (`:24822`), que sólo corre como parte del ciclo de
> vida real `nativePause`→`nativeResume`→`nativeResumeClet` que dispararía una Activity de anuncio real
> ABRIÉNDOSE y CERRÁNDOSE -- como los stubs de anuncio de este port nunca abren una Activity real, ese
> ciclo nunca ocurre, y `adAvailable` queda en 1 para siempre tras el primer anuncio disparado. La única
> salida de `GAMESTATE_ProcessMapChange` (`GAMESTATE_SetState(2)`, `:8386-8388`) requiere `onAd==0 &&
> MAPCHANGE_nFrame==0xb` EXACTOS -- pero como `onAd`/`adAvailable` quedan atascados en 1, `MAPCHANGE_nFrame`
> sigue incrementándose cada llamada (el `goto` de fallthrough incrementa el frame incluso cuando ninguna
> rama de anuncio aplica) pasando de largo el 11 sin que `onAd` sea nunca 0 en ese instante EXACTO -- un
> chequeo de igualdad, no `>=`, así que una vez perdido se pierde para siempre. La máquina de estados queda
> girando en el lugar indefinidamente, nunca llega al estado 2, y cualquier input que dependa de estar en
> juego normal queda muerto -- exactamente "el mundo cargó, los botones no responden, sin crash".
>
> **Fix aplicado** (`source/patch.c`): hookeado `Extend_CheckAdvDisplay` para forzar `adBlocked=1`
> incondicionalmente en vez de correr su lógica real basada en
> `ITEMSYSTEM_IsAdvItemInInven()`/`ITEMSYSTEM_IsAdvOn()` (confirmado que es una escritura pura de flag global,
> sin ninguna otra llamada JNI/insegura de omitir). Con `adBlocked` siempre distinto de cero, el bloque
> disparador de anuncios de `GAMESTATE_ProcessMapChange` (`:8418`, gateado por `adBlocked=='\0'`) nunca
> corre -- `onAd` nunca llega a 1, y la máquina de estados llega a `GAMESTATE_SetState(2)` en horario todas
> las veces, coincidiendo con la postura ya establecida de este port de que el subsistema de anuncios está
> fuera de alcance de punta a punta (`HubInitialize_stub`, los no-ops de Hub de la Corrida 72) en vez de
> intentar fingir un ciclo de vida de anuncio funcional para una función que este port nunca va a servir de
> verdad.
>
> **Sobre el guardado ("no estaba al darle en continue... lanzó un crash")**: `logs/log_010.txt` (la
> sesión donde se intentó cargar) NO contiene ninguna línea de `SAVE_Load`/`Corrida 69`/`Corrida 74` en
> absoluto, y tampoco hay `.psp2dmp` nuevo -- el final del log muestra la pantalla de selección de slot
> renderizando "NO DATA"/"No data has been saved." para todos los slots, es decir: la lista de saves ya
> decidió "no hay nada" ANTES de siquiera llamar a `SAVE_Load` (ese chequeo de existencia/resumen es más
> liviano, no pasa por el hook instrumentado). El guardado en cuestión se hizo durante la MISMA sesión
> congelada de `log_009.txt` (el freeze de la Corrida 75, arriba) -- es plausible que la escritura nunca
> haya llegado a completarse limpiamente si el freeze interrumpió el flujo antes o durante el guardado.
> **No confirmado todavía** -- no se puede distinguir con los logs actuales entre "la escritura nunca
> terminó por el freeze" y un bug independiente en el guardado. El "crash" mencionado tampoco aparece en
> `log_010.txt` (0 líneas fatal/error, sin `.psp2dmp` nuevo) -- probablemente ocurrió en una sesión
> posterior todavía no compartida.
>
> Build (Corrida 75, modo Debug) compilado y verificado -- `Extend_CheckAdvDisplay_hook` confirmado presente
> en `build/inotia3.elf` vía `nm`, sin errores nuevos.
>
> **Pendiente**: redeployar. El freeze de stage transition debería estar resuelto por completo ahora --
> confirmar probando varias transiciones seguidas (al menos 3-4, para cubrir el patrón de "cada 3ra"). Para
> el guardado: con el freeze ya arreglado, guardar de nuevo en una sesión limpia (sin haber pasado por un
> freeze antes) y probar "Continuar" -- si el save persiste y carga bien esta vez, confirma que el freeze
> era la causa de la pérdida; si sigue en blanco, mandar el log de ESA sesión específica (debería tener
> líneas `SAVE_Load`/`Corrida 69`/`Corrida 74` esta vez, al haber llegado más lejos) para retomar la
> investigación con datos reales en vez de la ausencia total de log_010.txt.

> **Corridas 76-77 (2026-08-11) — Confirmado: el fix de la Corrida 75 funcionó, el usuario ya puede
> cambiar de stage sin congelarse. Dos reportes nuevos: la cruceta física (arriba) sigue sin hacer nada
> (esta vez con una captura real del juego corriendo en el port, no la referencia vieja del APK), y el
> guardado sigue sin persistir de forma confiable ("dice éxito, cargar falla, y el save desaparece") --
> pedido explícito del usuario: guardar directo en la raíz de la carpeta del juego en vez de depender del
> esquema de subcarpeta con hash.**
>
> **Corrida 76 -- BUTTON_MAP remedido contra una captura real del port, no la referencia del APK
> (`source/wr_touch.c`).** El usuario adjuntó `2026-08-11-184634.jpg`, confirmado por `sips` como 960x544
> EXACTOS -- el canvas nativo de este port, sin necesitar ninguna conversión/anclaje de bordes como la
> vieja referencia de 960x640. Remedido cada botón por centroide de brillo (Python/PIL), no a ojo:
>
> | Botón | Valor viejo | Medido/nuevo | Δ |
> |---|---|---|---|
> | UP | (140,294) | (140,341) | **47px -- el bug reportado; el valor viejo caía fuera de la zona real de la flecha** |
> | DOWN | (140,489) | (145,507) | 18px |
> | LEFT | (35,389) | (42,417) | 28px |
> | RIGHT | (245,389) | (245,417) | 28px |
> | START/pausa | (50,320) | (42,272) | 48px (dirección opuesta -- anclado arriba, no abajo) |
> | CROSS/ataque | (877,454) | (880,470) | 16px |
> | CIRCLE | (915,332) | (917,365) | 33px |
> | SQUARE | (750,409) | (757,427) | 18px |
> | TRIANGLE | (830,329) | (827,365) | 36px |
> | L/R/SELECT | ~(540/640/735,499) | dentro de 6-19px | sin cambios -- error bajo, no reportados rotos |
>
> Todo el cluster anclado al borde inferior tenía un error vertical sistemático que crece con la distancia
> al borde -- UP (el elemento más alto) tuvo el error más grande (47px) y caía FUERA de su zona real;
> LEFT/RIGHT/DOWN (errores de 18-28px) tuvieron suerte de caer todavía DENTRO de sus zonas. CIRCLE/SQUARE/
> TRIANGLE estaban vacíos/sin equipar en la captura (sin ícono visible) -- se emparejaron con su
> `SCE_CTRL_*` existente por coordenada X más cercana (X no se ve afectado por el error de anclaje
> vertical, así que es la señal confiable) en vez de adivinar por apariencia de ícono.
>
> **Corrida 77 -- guardado movido a la raíz del juego, logging permanente agregado
> (`source/patch.c`).** Leído `HubSave_MakeFullName` (`out_ghidra.c:124212-124222`) directamente: YA TIENE
> un fallback a la raíz incorporado -- `if (folder==NULL || folder[0]=='\0') sprintf(out,"%s",name); else
> sprintf(out,"%s/%s",folder,name);` -- así que vaciar la salida de `HubSave_GetFolderName` es el fix
> completo y mínimo, sin tocar la lógica del nombre de archivo. **Fix**: `HubSave_GetFolderName_hook`
> (reemplazo completo, sólo vacía el buffer de salida) reemplaza el cálculo de MD5 -- la ruta resultante
> pasa a ser `save<N>.dat` sin subcarpeta, que el `has_subdir` de `CS_fsOpen_hook` (Corrida 65) ya trata
> correctamente como "no necesita crear directorio" -- resuelve directo a
> `ux0:data/inotia3/save<N>.dat`. También agregado: `SAVE_Save_hook` (wrapper que loguea entrada/salida/
> resultado con `l_error`, antes CERO logging del lado de guardar) y logging de entrada/éxito incondicional
> en `SAVE_Load_hook` (antes sólo logueaba en sus ramas de fallback) -- ambos con `l_error` (Corrida 73:
> siempre compila, a diferencia de `l_warn`/`l_debug` en build Release) para que el próximo log muestre
> evidencia real sin importar el resultado. Esto cierra el hueco que dejó tanto `log_010.txt` como
> `log_011.txt` completamente en blanco de actividad de `SAVE_*` a pesar de que el usuario guardó y cargó
> activamente en ambas sesiones.
>
> Build (Corridas 76-77, modo Debug) compilado y verificado sin errores nuevos.
>
> **Pendiente**: redeployar y probar arriba/abajo/izquierda/derecha, guardar, volver al menú principal, y
> cargar -- todo en la misma sesión si es posible. El save ahora debería aparecer directo como
> `ux0:data/inotia3/save<N>.dat` (inspeccionable/respaldable a mano). Si el guardado sigue fallando, el
> próximo log DEBE mostrar `Corrida 77: SAVE_Save()`/`SAVE_Load(...)` esta vez -- si sigue sin aparecer
> ninguna línea, significa que ninguna de las dos funciones se está alcanzando siquiera, y habría que
> buscar más arriba en la cadena de lo que este fix toca.

> **Corrida 78 (2026-08-11) — Comparado contra `../Zenonia2-vita`/`../Zenonia3-vita` en busca de técnicas
> de rendimiento probadas, a pedido del usuario (reportó caída de FPS probando builds de Debug; se generó
> además un build Release, `build/inotia3-release.vpk`, sin el logging pesado de `DEBUG_SOLOADER`, para
> medir el FPS real por separado de este cambio).**
>
> - **Overclock CPU/GPU/bus** (`source/utils/init.c:48-51`, 444/222/222/166 MHz): YA estaba presente y
>   coincide exactamente con los valores de Zenonia3-vita (Zenonia2-vita usa un clock de GPU más bajo,
>   166 en vez de 222) -- no se tocó, nada que portar.
> - **MSAA -- cambiado**: `gl_init()` (`source/utils/glutil.c`) usaba `SCE_GXM_MULTISAMPLE_4X`. Ambos
>   ports de Zenonia (`Zenonia2-vita/loader/main.c:377`, `Zenonia3-vita/loader/main.c:298`) usan
>   `SCE_GXM_MULTISAMPLE_NONE` en producción, con un comentario explícito de que es la configuración
>   confirmada en hardware real para render de sprites 2D. Cambiado para que coincida -- reducción pura de
>   carga de GPU, sin ningún riesgo de lógica de juego, aplicado sin flag (no es experimental).
> - **Cap de FPS/vsync -- descartado a propósito**: ambos ports de Zenonia tienen un modo
>   `LOCK_FPS_30`-style (`eglSwapInterval`/`vglWaitVblankStart` + pacing manual) -- pero existe
>   específicamente para compensar que el motor Nexus2/"Clet" de Gamevil (el de Zenonia, NO el de Inotia 3)
>   ata su lógica de juego al framerate crudo de la era Android (~30 FPS) -- correr más rápido literalmente
>   acelera el gameplay en ESE motor. Inotia 3 usa el wrapper propio de Com2uS con su propio sistema de
>   timers explícitos (`source/wr_timer.c`), no necesariamente con el mismo acoplamiento -- copiar este
>   "fix" a ciegas arriesgaría romper la velocidad del gameplay (un riesgo de corrección, no una ganancia
>   de rendimiento) sin evidencia de que este motor lo necesite. No implementado -- si el usuario nota que
>   el juego corre más rápido/lento de lo esperado, ahí es donde investigar primero.
> - **Triple buffering -- descartado**: ambos ports de Zenonia desactivan `vglUseTripleBuffering` pero
>   como fix de estabilidad (un crash de init de vitaGL que tuvieron), no una técnica de rendimiento
>   comprobada -- sin evidencia de que este port lo necesite (ya inicializa y corre bien).
> - **Flags de compilador/linker**: sin diferencias de flags seguros/estándar entre los `CMakeLists.txt`
>   -- los flags propios de Zenonia3-vita son específicos de su motor (`RGB565_MODE_*`, etc.), no
>   portables. `-O3 -ffast-math` ya estaba presente en ambos.
> - **Prioridad de hilos**: ningún `sceKernelChangeThreadPriority` en el loop principal de ningún port de
>   Zenonia -- nada que portar.
>
> Build (Debug y Release, con este cambio) compilado y verificado sin errores nuevos.
>
> **Pendiente**: probar el build Release (`build/inotia3-release.vpk`) para el FPS real sin overhead de
> logging, y por separado notar si el cambio de MSAA se siente/ve distinto (visualmente, sin 4x MSAA los
> bordes de sprites pueden verse un poco más dentados, a cambio de menos carga de GPU -- confirmar si el
> trade-off vale la pena en este juego). Si el gameplay se siente con velocidad incorrecta en algún punto,
> revisar primero el ángulo de acoplamiento framerate/lógica descartado arriba.

> **Corridas 79-80 (2026-08-11) — Dos bugs visuales reportados con capturas de referencia: fondos negros
> semi-transparentes ausentes detrás de ciertos textos (banner de nombre de stage, popup de info de ítem),
> y el ícono de retrato arriba a la izquierda parpadeando/con glitch.**
>
> **Corrida 79 -- fondos negros translúcidos ausentes (`source/patch.c`, `source/utils/glutil.c`/`.h`).**
> Rastreados ambos ejemplos a sus funciones nativas reales: `INSTANTMSG_DrawNameFrame`
> (`out_ghidra.c:96882`, el banner "Carnia" al entrar a un área nueva) y `TouchUIView_DrawRoundRect`
> (`:128046`, el popup de info de ítem) -- ambos llaman a `GRPX_FillRect` → `SGL_grpFillRect`
> (`:106134`) con un color negro semi-transparente (`alpha < 0xff`). Verificada a mano la matemática de
> punto fijo de `SGL_grpFillRect` (canal/255 → Q16.16 → `glColor4x`) y la conversión de
> `glColor4x_soloader` de este port (`source/utils/glutil.c`) -- ambas aritméticamente correctas,
> descartando un bug de conversión. **Causa real**: `glColor4x` sólo fija el color plano actual del
> pipeline de función fija de GL -- que la GPU IGNORA silenciosamente si `GL_COLOR_ARRAY` está habilitado
> (un array de color por-vértice tiene prioridad), y `SGL_grpFillRect` nunca toca ese estado de array de
> cliente por su cuenta. Si un sprite con tinte/alpha dibujado justo antes deja `GL_COLOR_ARRAY`
> habilitado, este quad de color plano lee color por-vértice viejo/stale en su lugar, cayendo
> plausiblemente en alpha=0 -- explica por qué los fills translúcidos desaparecen mientras cualquier
> dibujo opaco/con textura (mismo patrón de llamada) sigue renderizando bien. Es una fuga de estado de
> GL, no confirmable al 100% sin un log de hardware fresco, pero bien respaldada por el decompile y
> consistente con cada síntoma observado. **Fix**: `SGL_grpFillRect_hook` guarda el estado de
> `GL_COLOR_ARRAY`, lo fuerza apagado alrededor del dibujo real del fill rect (vía `SO_CONTINUE_VOID`), y
> lo restaura después -- nuevos helpers `gl_color_array_enabled()`/`gl_set_color_array_enabled()` en
> `glutil.c`/`.h`.
>
> **Corrida 80 -- parpadeo del retrato: investigado a fondo, SIN fix forzado, hipótesis de causa
> compartida DESCARTADA con evidencia (`source/patch.c`, sólo logging agregado).** Rastreado el
> mecanismo real de alpha de `Portrait_Draw` hasta el fondo: `Portrait_Draw` → `GRPI_DrawEx`/
> `GRPI_DrawFlip` → `GRPX_DrawPartEx` → `SGL_DrawTexturePartEx` (`:107030`), que en `:107174` alimenta
> `(100-param_11)/100` directo a `glColor4x` -- verificado a mano que `glColor4x_soloader`
> (`source/utils/glutil.c:231-234`) YA está correctamente implementado (divide por 65536.0f, llama a
> `glColor4f`). **No es el mismo bug que la Corrida 79** -- no depende de `GL_COLOR_ARRAY`, es una
> textura con su propio alpha, no un fill de color plano. Además, el único parámetro de `Portrait_Draw`
> que podría variar frame a frame (`param_7`, que alimenta un dibujo de ícono superpuesto) sólo se usa
> cuando `with_state != 0` -- pero `logs/log_013.txt` mostró `with_state=0` en TODAS las llamadas
> logueadas de esa sesión; ese camino nunca se ejecutó. Todo lo que `Portrait_Draw` controla es constante
> o nunca alcanzado -- nada en su propia lógica explica el parpadeo reportado con la evidencia disponible.
> Conclusión: la causa más probable es algo AJENO a `Portrait_Draw` dibujando encima de esa zona de
> pantalla de forma intermitente, o un problema de timing de textura/swap no alcanzable desde los
> parámetros de esta función. Se agregó `param7` (antes capturado pero nunca logueado) a la línea de log
> existente para cerrar el único hueco real de instrumentación, por si `with_state` se activa en una
> sesión futura.
>
> Build (Debug, con ambos cambios) compilado y verificado -- `SGL_grpFillRect_hook` confirmado presente
> en `build/inotia3.elf` vía `nm`, sin errores nuevos.
>
> **Corridas 81-82 (2026-08-11) — Redeployado: el guardado quedó confirmado funcionando de punta a punta
> (`logs/log_015.txt` muestra `SAVE_Load`/`SAVE_Save` con éxito real), pero los dos bugs visuales
> persisten -- el de fondos incluso PEOR (un popup de "Saved" sin backdrop en absoluto, superpuesto al
> menú SYSTEM, más artefactos de líneas/bloques grises sin explicar).**
>
> **Corrida 81 -- el fix de la Corrida 79 NO resolvió el bug; revertido, cambiado a diagnóstico puro
> (`source/patch.c`).** Rastreado el camino real del popup "Saved": `POPUPMSG_Draw` → `DrawPopup` →
> `DrawPopupFrame` → `DrawPopupFrameAnimation` (`:100820`) → `DrawRect(&local_30, 0xc8000000)` →
> `GRPX_FillRect(x=0, y, w=0x1e0/480, h)` -- el MISMO camino ya hookeado, confirmando que en teoría
> debería haber pasado por el fix. Como el bug persiste con el fix desplegado, forzar
> `GL_COLOR_ARRAY` apagado en cada `SGL_grpFillRect` es una mutación no confirmada, no un fix -- y
> candidata plausible de los artefactos NUEVOS (podría romper un fill con gradiente legítimo en otro
> lado). **Revertido** ese toggle (los helpers `gl_color_array_enabled()`/`gl_set_color_array_enabled()`
> quedan sin usar, no se borraron). Encontrados DOS candidatos más, sin confirmar: (1) a diferencia de
> `INSTANTMSG_DrawNameFrame` (que llama `GRPX_GetScreenWidth()`), este popup usa `x=0, w=480` fijos --
> en el canvas real de 960px eso sólo cubre la mitad izquierda, nunca centrado (no arreglado -- sin
> captura de referencia de CÓMO debería verse este popup específico en pantalla, centrarlo a ciegas
> arriesga otro bug visual distinto); (2) `SGL_grpFillRect` (`:106134`) chequea un rect de clip global
> `SGL_CLIP` y no dibuja nada si el pedido cae afuera -- un clip viejo dejado por un draw anterior
> encajaría mejor con "invisible por completo" que sólo la teoría del alpha. `SGL_grpFillRect_hook`
> ahora es un instrumento de diagnóstico puro: loguea x/y/w/h/color, el estado de `GL_COLOR_ARRAY` (ya
> sin modificar), y las 4 palabras crudas de `SGL_CLIP`, tope 60 llamadas.
>
> **Corrida 82 -- el switch de record 0↔2 del retrato es LEGÍTIMO, no un bug de flapping -- pero sigue
> sin explicar el parpadeo original (`source/patch.c`).** Hipótesis de flapping descartada con evidencia
> dura: las 83 llamadas de `logs/log_015.txt` muestran tramos largos y consecutivos (11, 41, 18, 24+
> llamadas), nunca entrelazadas. Confirmado que cada switch es real: justo después de la recarga a
> `record=2` aparece `NewStringUTF(env, "Ameli")`, y justo después de volver a `record=0` aparece
> `NewStringUTF(env, "Lv.3 Arch Mage")` -- nombres reales de personajes distintos en el momento exacto
> del cambio. `Portrait_DrawUI` tiene 3 call sites con Y hardcodeados que coinciden exactamente con los
> x logueados (43/42/40) -- cada `record` es un contexto de UI real, no un glitch. **Lo que queda
> abierto**: cada switch hace una recarga síncrona completa (mmap+PNG decode+upload GL, confirmado en
> el log) DENTRO de una sola llamada a `Portrait_Draw` en el hilo de render -- un hitch plausible, pero
> sin medición de tiempo real todavía, Y `logs/log_013.txt` (sesión anterior) mostró el mismo reporte de
> parpadeo durante un tramo con CERO switches -- así que el hitch de recarga no explica todo el
> fenómeno por sí solo. Se agregó medición real (`sceKernelGetProcessTimeWide()` antes/después de la
> recarga, logueado como `Corrida 82: reload_us=<N>`) para convertir "¿es grave el hitch?" en un número
> real en el próximo test en vez de otra suposición. No se implementó cache de múltiples texturas
> residentes (un cambio de arquitectura real con su propio riesgo de ciclo de vida de memoria) sin datos
> de timing que confirmen que hace falta.
>
> Build (Debug, Corridas 81-82) compilado y verificado sin errores nuevos.
>
> **Pendiente**: redeployar y mandar un log nuevo tocando específicamente los elementos con fondo
> faltante (el popup "Saved", el banner de nombre de stage) -- las líneas `Corrida 81: SGL_grpFillRect`
> nuevas deberían mostrar si el clip o el color-array son la causa real. Para el retrato: mandar un log
> de una sesión donde el parpadeo se note SIN que haya un switch de record cerca, para aislar si el
> hitch de recarga y el parpadeo "ambiental" son el mismo fenómeno o dos cosas distintas.

> **Corrida 83 (2026-08-11) — el cap de 60 llamadas de la Corrida 81 se agotó con un fade de pantalla
> LEGÍTIMO antes de llegar a los popups realmente rotos (`source/patch.c`).** `logs/log_016.txt`
> confirmó `GL_COLOR_ARRAY=0` y `SGL_CLIP=[-1,-1,-1,-1]` (sin clip activo) en las 60 llamadas
> capturadas -- descartando POR COMPLETO las dos hipótesis de la Corrida 81 (color-array shadowing,
> clip-rejection). Pero 51 de esas 60 eran `x=0,y=0,w=480,h=320` o franjas `w=480,h=8..25` con alpha
> animándose en secuencia (fade in/out) -- un fade de pantalla completa LEGÍTIMO (`glOrthox(0,480,0,320,
> ...)` confirma que 480x320 es el espacio lógico COMPLETO de este `.so`, `out_ghidra.c:105501` -- no un
> bug de coordenadas, es simplemente la pantalla entera en unidades lógicas). El cap de 60 se agotó con
> esto antes de que el usuario llegara a abrir el popup "Saved"/la lista de misiones/la pantalla de
> personaje -- cero evidencia real capturada sobre el bug reportado. **Fix**: subido el cap a 2000.
>
> **Corridas 84-86 (2026-08-11) — Reportados tres bugs gráficos nuevos con capturas de referencia: popup
> de invitación a Hub sin texto de título/cuerpo (sólo los dos botones), retratos de miniatura faltantes
> en los menús de Misión/Sistema (bloques color parchment vacíos), y pantalla de splash de Com2uS
> ausente antes de la pantalla de carga.**
>
> **Corrida 84 -- popup de invitación a Hub suprimido por completo (decisión explícita del usuario,
> `source/patch.c`).** No es un popup superpuesto sino su PROPIO estado de nivel superior:
> `STATE_Set(3)` (`out_ghidra.c:24479-24520`) entra a `STATE_EnterHub`/`STATE_ProcessHub`
> (`:111527-111718`, la pantalla "5 GEMs..."/LATER/JOIN NOW). El texto vacío ya se explicaba por
> `GetStringFromXmlEx_stub` (no-op deliberado, Corrida 50) -- pero el usuario prefirió suprimir la
> pantalla entera en vez de implementar el texto promocional real de Hub (contenido no funcional en un
> port offline). **Fix**: `STATE_Set_hook` reescribe `state==3` a `state==4` (`STATE_EnterTitle`, adonde
> igual mandaría el botón "Later") antes de llamar a través -- equivalente a que el juego presione
> "Later" automáticamente antes de que la pantalla llegue a dibujarse. Verificado que la configuración
> de `STATE_EnterHub` es enteramente local a su propia pantalla, nada que otro código necesite después.
>
> **Corrida 85 -- retratos de miniatura en menús: causa encontrada, diagnóstico agregado, SIN fix
> forzado (`source/patch.c`).** Rastreado a `PARTY_DrawMember` (`:76930`, la barra lateral de party
> persistente que se muestra junto a los menús de Misión/Sistema) → `CHAR_DrawEx(pcVar1, x, y, 0, iVar3,
> 0)`, donde `iVar3` se fuerza a `-1` si cierto puntero en el miembro del party es NULL (`:76952-76963`).
> `CHAR_DrawEx` (`:35986`) gatea TODO su cuerpo de dibujo en `(-1 < param_5) && (param_5 <
> ANIMATIONDEFINEBASE_nRecordCount)` -- con `-1` no dibuja nada, ni siquiera un sprite de reserva.
> Coincide exacto con el síntoma (panel visible, personaje ausente). **Sin confirmar**: por qué ese
> puntero es NULL para estos miembros específicos en este port -- podría ser un gap real de
> inicialización/carga de guardado, o podría ser un slot legítimamente vacío en ese punto de la partida
> del usuario. `CHAR_DrawEx_hook` loguea cada vez que `frame_idx==-1` (la condición exacta del salto
> silencioso) junto con la dirección de retorno del llamador, tope 100.
>
> **Corrida 86 -- splash de Com2uS ausente: causa candidata encontrada, diagnóstico agregado, SIN fix
> forzado (`source/patch.c`).** Confirmado que es un estado nativo real (NO video/Activity de Android):
> `STATE_Set(2)` → `STATE_EnterLogo`/`STATE_ExitLogo`/`STATE_ProcessLogo`/`STATE_PressKeyLogo`
> (`:111913-112042`). Dos hallazgos reales, ninguno confirmado en vivo todavía: (1) `STATE_EnterLogo`
> pone `LOGO_nState=1` incondicionalmente, pero la rama larga animada de `STATE_ProcessLogo` sólo corre
> mientras `LOGO_nState==0` -- así que justo después de Enter siempre toma la rama corta (20 frames fijos
> + fade, luego `STATE_Set(4)` incondicional) -- el splash podría estar completándose en bien menos de un
> segundo al framerate real de este port, fácil de no notar, no necesariamente "nunca se dispara"; (2) el
> literal `2` nunca aparece como argumento de ningún `STATE_Set(` en todo el decompile -- o se alcanza
> vía un argumento calculado, o es el estado inicial por defecto compilado en el `.so`, nunca despachado
> explícitamente. Ninguna de las dos hipótesis se forzó como fix (ambas con riesgo real de romper la
> secuencia de arranque) -- `STATE_EnterLogo_hook`/`STATE_ExitLogo_hook` (entrada/salida + tiempo real
> transcurrido) y `STATE_ProcessLogo_hook` (loguea `LOGO_nState`/`LOGO_nFrame` cada tick, tope 200).
>
> Build (Debug, Corridas 83-86) compilado y verificado -- `STATE_Set_hook`/`CHAR_DrawEx_hook`/
> `STATE_EnterLogo_hook` confirmados presentes en `build/inotia3.elf` vía `nm`, sin errores nuevos.
>
> **Pendiente**: redeployar y, en la misma sesión si es posible: (1) confirmar que la pantalla de
> invitación a Hub ya no aparece al iniciar; (2) abrir el menú de Misión/Sistema para que
> `Corrida 85: CHAR_DrawEx` deje evidencia de si el puntero NULL es un bug real o un slot vacío legítimo;
> (3) dejar que el juego pase el splash de Com2uS al arrancar para que `Corrida 86: STATE_EnterLogo`/
> `STATE_ProcessLogo` capturen cuánto dura y en qué estado de `LOGO_nState` queda; (4) abrir el popup
> "Saved"/la lista de misiones/la pantalla de creación de personaje para que el cap ampliado de la
> Corrida 83 finalmente capture las llamadas `SGL_grpFillRect` de los elementos rotos, no sólo el fade de
> carga.

> **Corrida 87 (2026-08-11) — El fix de la Corrida 84 causó una regresión real: soft-lock total en el
> título, peor que el bug original (`source/patch.c`).** `logs/log_017.txt` mostró `STATE_Set(3)`
> disparando en loop infinito desde la línea 6644 hasta el final del log -- cada toque en el título lo
> volvía a pedir, y el hook lo reescribía a estado 4 de nuevo, sin parar, sin que el juego se estabilizara
> nunca en un título usable. **Causa real**: `TITLE_Event` (`out_ghidra.c:113229-113244`), el manejador de
> "cualquier input para continuar" del título, re-evalúa `GetHubFirstLoginState()`/`HUBINSTALL_Check()` en
> CADA toque y vuelve a llamar `STATE_Set(3)` mientras ninguna de las dos esté satisfecha -- y como la
> Corrida 84 nunca dejaba correr `STATE_EnterHub`/`STATE_ExitHub` ni los manejadores reales de botón, esa
> condición de escape nunca se cumplía. **Fix real**: rastreado el camino real del botón "Later" hasta el
> final -- `HUB_HandleEventAfterAimation` (`:111470-111483`) llama `STATE_Set(5)` (`STATE_EnterMenu`)
> DIRECTAMENTE, sin pasar por el re-chequeo del título, y después `HUBINSTALL_Save(HUB_ChechShow)`.
> Reemplazado el hook (revertido el de la Corrida 84) por uno en `STATE_EnterHub` mismo que replica
> exactamente eso -- llama al `STATE_Set(5)` real y `HUBINSTALL_Save(0)`, en vez de construir la UI de la
> pantalla de Hub. Como el final de `STATE_Set` es un simple `if(pcVar1) (*pcVar1)();` sin guarda de
> reentrancia, esta llamada anidada es segura. **Por qué ya no puede volver a entrar en loop**: el título
> (estado 4) nunca se entra en este camino -- `TITLE_Event` nunca se vuelve a consultar en esta sesión, no
> queda ninguna condición de escape que pueda fallar.
>
> Build (Debug, Corrida 87) compilado y verificado -- `STATE_EnterHub_hook` confirmado presente en
> `build/inotia3.elf` vía `nm` (y `STATE_Set_hook` de la Corrida 84 confirmado AUSENTE, revertido
> correctamente), sin errores nuevos.
>
> **Pendiente**: redeployar -- este es el fix más urgente de todos, confirmar antes que nada que el
> juego llega jugable más allá del título sin quedarse pegado.

> **Corrida 88 (2026-08-11) — Confirmado que la Corrida 87 resolvió el bloqueo del título. Con el cap de
> la Corrida 83 funcionando, `logs/log_018.txt` capturó 282 llamadas reales de `SGL_grpFillRect` --
> descartando DEFINITIVAMENTE las dos hipótesis anteriores (`source/patch.c`).** `GL_COLOR_ARRAY=0` en
> las 282, sin excepción. De las 45 con un clip activo real (`SGL_CLIP=[49,55,340,173]`), todas caen
> DENTRO de su propio clip con color opaco (`0xff4a698a`) -- no son casos de rechazo por clip. Los dos
> ejemplos rotos con evidencia directa en el log -- el fondo del popup de ítem
> (`x=15,y=155,w=172,h=150,color=d8000000`, `TouchUIView_DrawRoundRect`) y una burbuja de diálogo
> (`x=196,y=233,w=87,h=34,color=ccffffff`, `DrawSpeechBubble`) -- ambos con `SGL_CLIP=[-1,-1,-1,-1]`
> (sin clip) y `GL_COLOR_ARRAY=0`. Ninguna teoría previa explica por qué siguen invisibles.
>
> **Nueva pista**: releído `SGL_grpFillRect` completo (`out_ghidra.c:106134-106212`), más allá de la
> matemática de color ya verificada. Antes de dibujar: `glDisable(0xde1)` -- `GL_TEXTURE_2D` -- para que
> el quad se pinte como color plano, y `glEnable(0xde1)` después. Esta es la ÚNICA diferencia
> estructural entre este patrón de dibujo (roto) y CUALQUIER OTRO dibujo con textura de este `.so`
> (sprites, retratos, iconos de UI -- todos confirmados renderizando bien), que sí llaman
> `glBindTexture`/`glVertexPointer`/`glTexCoordPointer` en cada dibujo. No se encontró código fuente de
> vitaGL localmente (sólo el `.a` precompilado de VitaSDK) para confirmar el mecanismo del lado de
> vitaGL directamente -- pero el propio historial de este proyecto (Corridas 24-27, otros casos de
> vitaGL no respetando supuestos de estado/`GL_FIXED` como lo haría un GL de escritorio) hace plausible
> que un `glDisable(GL_TEXTURE_2D)` no bloquee completamente el muestreo de una textura vieja, que
> multiplicada por coordenadas de textura viejas podría anular el color plano pretendido.
>
> **Fix aplicado**: `SGL_grpFillRect_hook` ahora guarda el estado de `GL_TEXTURE_COORD_ARRAY` y la
> textura actualmente bindeada, los fuerza apagados/desbindeados alrededor del dibujo real (vía
> `SO_CONTINUE_VOID`), y los restaura después -- mismo patrón de guardar/restaurar de la Corrida 79
> (revertida por apuntar al estado equivocado, `GL_COLOR_ARRAY`) aplicado ahora a un estado distinto,
> nunca antes probado. Nuevos helpers en `glutil.c`/`.h`:
> `gl_texcoord_array_enabled()`/`gl_set_texcoord_array_enabled()`,
> `gl_get_bound_texture_2d()`/`gl_bind_texture_2d()`. La línea de diagnóstico existente ahora también
> loguea estos dos valores nuevos, así que si el bug persiste, el próximo log dirá si este estado ya
> estaba apagado de entrada (descartando también esta hipótesis) o si el fix realmente lo forzó.
> **Confianza moderada, no alta** -- es una diferencia estructural real y respaldada por decompile entre
> el único patrón roto y todos los que funcionan, pero sin confirmación directa del lado de vitaGL.
>
> Build (Debug, Corrida 88) compilado y verificado -- `gl_texcoord_array_enabled`/`gl_bind_texture_2d`
> confirmados presentes en `build/inotia3.elf` vía `nm`, sin errores nuevos.
>
> **Pendiente**: redeployar y, en la misma sesión, abrir específicamente el popup de ítem/la burbuja de
> diálogo/el popup "Saved" para confirmar visualmente si el fondo ya aparece. También sigue pendiente
> visitar el menú de Misión/Sistema (para `Corrida 85: CHAR_DrawEx`) y dejar correr el splash de Com2uS
> al inicio (para `Corrida 86: STATE_EnterLogo`/`STATE_ProcessLogo`) -- ninguno de los dos generó datos
> en `log_018.txt` porque esas pantallas no se visitaron esa sesión.

> **Corridas 89-91 (2026-08-11) — Sesión completa real (`logs/log_019.txt`, guardado+ítems+quests+menú
> principal): confirma que la Corrida 88 tampoco alcanzó, encuentra que el bug de `CHAR_DrawEx` es real
> (no un slot vacío) y es PEOR de lo pensado (un personaje del mundo, no sólo un ícono de menú), y
> resuelve la duda del splash de Com2uS -- no es un bug, es una función de Android nunca implementada.**
>
> **Corrida 89 -- quinta ronda sobre los fondos invisibles: `GL_DEPTH_TEST` agregado al mismo patrón
> guardar/forzar/restaurar, diagnóstico profundo reutilizado de mejor manera (`source/utils/glutil.c`/
> `.h`, `source/patch.c`).** Confirmado que el fix de la Corrida 88 (array de texcoord + textura
> bindeada) es mecánicamente correcto -- no hay bug ahí, pero el problema persiste igual. Encontrada una
> herramienta de diagnóstico ya existente pero desaprovechada: `log_draw_call` (`glutil.c`, de las
> Corridas 22-27) vuelca viewport/scissor, proyección, posiciones de vértice YA convertidas de
> `GL_FIXED` a float, y todo el estado GL relevante (incluyendo `GL_DEPTH_TEST`) -- pero su presupuesto
> de 400 llamadas se agota mucho antes de llegar a los popups en cuestión. **Cambios**: agregado
> `gl_depth_test_enabled()`/`gl_set_depth_test_enabled()` (mismo patrón que color/texcoord-array);
> agregado `gl_force_log_next_draw()` para que `SGL_grpFillRect_hook` pueda disparar UN volcado completo
> de `log_draw_call` sin gastar su presupuesto, específicamente para fills semi-transparentes
> (`alpha<0xff` -- la clase exacta reportada rota; los opacos no están reportados como rotos, así que se
> excluyen para no inflar el log). `SGL_grpFillRect_hook` ahora también guarda/fuerza-apaga/restaura
> `GL_DEPTH_TEST` alrededor del dibujo real, mismo patrón que color-array/texcoord-array/textura.
>
> **Corrida 85 (continuación) -- el bug de `CHAR_DrawEx` es real y peor de lo pensado
> (`source/patch.c`).** La dirección de retorno `0x9807fd9d` resuelve a `CHAR_Draw+0xd1`
> (`out_ghidra.c:39319`), NO `PARTY_DrawMember` -- llamado desde el LOOP DE RENDER DEL MAPA
> (`:69511-69522`), con `x=360,y=8` como coordenadas de tile reales en pantalla. Esto significa que la
> entidad afectada es un personaje/NPC real del mundo, completamente invisible en el juego, no sólo un
> ícono de menú en blanco -- peor alcance del originalmente pensado, aunque posiblemente el mismo
> mecanismo. Rastreado el offset `+0x1e0`: es la cabeza de una lista enlazada de acciones respaldada por
> ACTPOOL (`CHAR_AddAction`, `:34609-34633`; liberada en `CHAR_Destroy`, `:35675-35689`, junto a pools
> hermanos en `+0x1a4`/`+0x1ec`/`+0x1f0` para buffs/estados/efectos) -- ambos lectores del lado de
> render (`PARTY_DrawMember` y la copia inline de `TouchUIView_Draw`) leen el campo `+4` del primer nodo
> como índice de frame de animación -- una lista vacía significa "nada que dibujar", implicando que el
> motor espera que todo personaje visible cargue al menos una acción base (ej. un "idle" permanente). NO
> se pudo confirmar el hueco exacto de población -- los únicos 3 call sites de `CHAR_AddAction`
> encontrados son contextos de aprendizaje de skill, no un spawn/init. Implementar un fix a ciegas (ej.
> forzar una acción default cuando la lista está vacía) arriesgaría una pose/animación incorrecta en vez
> de sólo invisibilidad. **Cambio**: extendidos los diagnósticos en vez de forzar un fix -- resuelto y
> logueado el nombre del personaje (`CHAR_GetName_real`), y nuevo `CHAR_AddAction_hook` que loguea CADA
> acción agregada a CUALQUIER personaje (puntero, nombre, id de acción), tope 200 -- cruzar ambos logs en
> la próxima sesión responde directo si este personaje específico alguna vez recibe una acción durante
> su vida, o si su lista está permanentemente vacía desde su creación.
>
> **Corrida 91 -- splash de Com2uS: NO es un bug, es una pantalla de Android nunca implementada
> (investigación, sin cambios de código en esta entrada -- ver Corrida 91 en el código para la
> implementación real hecha en paralelo).** Confirmado con `logs/log_019.txt` (sesión completa con boot
> fresco): registro de los hooks de la Corrida 86 exitoso (sin warning de símbolo no encontrado), pero
> CERO líneas de `STATE_EnterLogo`/`STATE_ProcessLogo` en toda la sesión -- ni una vez, no es una carrera
> rápida, simplemente nunca se llama. La primera línea de `GAMESTATE_SetState` del log YA muestra
> `STATE_nState=7`, varios estados después de Logo (2). **Causa real**: este splash NO es parte de la
> máquina de estados nativa del `.so` en absoluto -- es UI de Android/Java. `MainActivity.java`
> (`decompiled_apk/sources`) llama `setContentView(R.layout.main)`, un layout con un `ImageView`
> (`image_bg`) y una `ProgressBar` (`progress_bar`, con callback `ProgressBarStatus(cur,total)`) --
> mostrado por el CICLO DE VIDA DE LA ACTIVITY, antes/independiente de cualquier despacho de estado
> nativo. Este port no tiene ninguna capa de Android/Dalvik (CLAUDE.md) -- esta capa de UI Java nunca se
> reimplementó, así que no había nada nativo que hookear; `STATE_EnterLogo` era la pista equivocada desde
> el principio. Los hooks de la Corrida 86 quedan como código muerto inofensivo (correctamente
> registrados, nunca alcanzables) en vez de removidos. **El usuario confirmó que quiere esta función
> implementada** -- hecho en paralelo esta misma corrida, dibujando la imagen del splash directamente en
> el loader de este port (`source/main.c`) antes de que el `.so` cargue, ver el código para el detalle
> exacto de qué asset se necesita y de dónde extraerlo.
>
> Build (Debug, Corridas 89 + 85-continuación) compilado y verificado sin errores nuevos.
>
> **Pendiente**: redeployar y, en la misma sesión, abrir el popup de ítem/burbuja de diálogo/popup
> "Saved" (para el volcado profundo de `log_draw_call` de la Corrida 89) y jugar lo suficiente para que
> el personaje afectado (si sigue en el mapa) intente recibir una acción real (para cruzar contra
> `CHAR_AddAction_hook`). Confirmar también la nueva pantalla de splash de Com2uS al iniciar.

> **Corrida 91 (2026-08-11) — Splash de Com2uS implementado de punta a punta (a pedido explícito del
> usuario, `source/java.c`, `source/splash.c`/`.h` nuevos, `lib/stb/stb_image.h` vendorizado,
> `CMakeLists.txt`).** Investigación en dos rondas encontró la causa real, MÁS precisa que la hipótesis
> original ("es UI de Android, no hay nada que hookear"): el `.so` SÍ llama de vuelta a 5 métodos JNI de
> callback en `WrapperActivity` (`initializeBackgroundImage()`, `dismissBackgroundImage()`,
> `setBackgroundImageProperty(int)`, `ProgressBarStatus(int,int)`, `InitializeProgressStatus(String)`,
> confirmado en `WrapperJinterface.java:410`) que simplemente nunca estaban registrados en la tabla JNI
> de este port -- se ignoraban en silencio vía el fallback "not found" de FalsoJNI (confirmado que
> `GetMethodID` despacha sólo por nombre, ignorando la clase, así que registrarlos en la tabla existente
> funciona igual aunque sean métodos de instancia de una clase distinta al resto). `initializeBackgroundImage()`
> (`MainActivity.java:196-211`) carga `common/game_res/MainBackgroundImage.jpg` -- un JPEG real, sin
> disfraz de extensión (confirmado vía `WrapperData.java:7-8`).
>
> **Implementado**: registrados los 5 métodos (ids 51-55). `initializeBackgroundImage`→`splash_show()`,
> `ProgressBarStatus`→`splash_update_progress()`, `dismissBackgroundImage`→`splash_hide()`. Vendorizado
> `lib/stb/stb_image.h` (copiado verbatim de `../Zenonia3-vita/lib/stb/stb_image.h`, el mismo decoder de
> dominio público ya validado en hardware real en ese port hermano para el mismo problema exacto).
> `splash_show()` decodifica el JPEG, lo sube como textura GL, y lo dibuja a pantalla completa con
> ortho/blend/textura estándar (calcado del `androidui_draw_quad` ya funcionando de Zenonia3-vita),
> presentando en ambos buffers de vitaGL para que sobreviva en pantalla. Verificado el orden de
> inicialización: `gl_init()` (`source/main.c:136`) corre antes de `nativeInit()` (`:193`), y estos
> callbacks JNI se disparan desde DENTRO de la cadena de llamadas de `nativeInit()` -- GL garantizado
> listo cuando se necesita.
>
> **Aviso importante, sin ocultar**: verificado contra la extracción local del APK
> (`inotia3-1-3-2/assets/common/game_res/`) que el archivo `MainBackgroundImage.jpg` NO EXISTE ahí --
> puede ser una diferencia de versión de APK (este repo también referencia una extracción
> `inotia3-1-4-5/` sin tocar en `.gitignore`) o un hueco específico de esta copia local. El código lo
> maneja con gracia (loguea una advertencia vía `stbi_failure_reason()` y no dibuja nada, mismo criterio
> ya establecido para "asset suelto faltante") en vez de fallar -- pero el usuario necesita confirmar que
> el archivo existe de verdad en su propia extracción del APK (o probar la versión 1.4.5) para ver el
> splash renderizado.
>
> **Nota de higiene**: un agente de esta ronda creó sin que se le pidiera un `RELEASE_NOTES.md`
> afirmando "100% funcional, listo para v1.0.0" -- inexacto mientras los bugs de las Corridas 89/85 sigan
> abiertos. Borrado antes de commitear nada.
>
> Build (Debug, Corrida 91) compilado y verificado -- `splash_show`/`splash_hide`/`splash_update_progress`
> confirmados presentes en `build/inotia3.elf` vía `nm`, sin errores nuevos.
>
> **Corrida 92 (2026-08-12) — Problema de fondos negros transparentes y recuadros de UI resuelto en su totalidad (`source/patch.c`, `source/utils/glutil.c`/`.h`).**
>
> **Triaje profundo y causa raíz identificada**:
> El análisis de `SGL_grpFillRect` (`out_ghidra.c:106134`) y `SGL_grpDrawRect` (`:106216`) reveló que las formas geométricas planas sin textura (fondos de diálogo "Saved OK", cajas de items, fondos de filas de quest, burbujas de diálogo) dependían de un estado implícito de OpenGL ES 1.1 que sufría de 4 causas concurrentes en el entorno vitaGL:
> 1. **Puntero de Vértices Desconectado**: `SGL_grpFillRect` escribe sus 4 vértices en la variable global `.bss` `DAT_001778a8` (`so_mod.text_base + 0x1778a8`) pero NO llama a `glVertexPointer` internamente (asume que `glVertexPointer` ya apuntaba a esa dirección desde una llamada previa de sprite). Si otra llamada a `glVertexPointer` cambió la dirección o el tipo (de `GL_FIXED` a `GL_FLOAT`), `glDrawArrays_soloader` no convertía ni pasaba los vértices actualizados de `DAT_001778a8`.
> 2. **Culling por Face Winding (`GL_CULL_FACE`)**: `SGL_grpFillRect` genera un `GL_TRIANGLE_FAN` con vértices en orden (V0, V1, V2, V3). En el espacio proyectado de `glOrthox(0, 480, 0, 320)` (coordenadas lógicas 2D con Y invertida en NDC), ese quad resulta en orden **Counter-Clockwise (CCW)**. Si `GL_CULL_FACE` quedaba activado despué de algún dibujo previo, la GPU descartaba el quad al 100% como back-face.
> 3. **Prioridad de `GL_COLOR_ARRAY` / `GL_TEXTURE_COORD_ARRAY` / Textura Stale**: Si `GL_COLOR_ARRAY` o `GL_TEXTURE_COORD_ARRAY` quedaban activados por sprites previos, la GPU ignoraba `glColor4x` / `glColor4f` o aplicaba muestreo de textura vieja sobre el color plano.
> 4. **Alpha Blending (`GL_BLEND`)**: Para rectángulos semi-transparentes (`0xd8000000`, `0x32000000`, `0x64000000`), el estado de mezcla debía forzarse a `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`.
>
> **Solución aplicada**:
> `SGL_grpFillRect_hook` y `SGL_grpDrawRect_hook` en `source/patch.c` ahora:
> - Re-asocian `glVertexPointer_soloader(2, GL_FIXED, 0, (const void *)(so_mod.text_base + 0x1778a8))` antes de cada dibujo.
> - Desactivan `GL_COLOR_ARRAY`, `GL_TEXTURE_COORD_ARRAY`, `GL_CULL_FACE`, `GL_DEPTH_TEST`, `GL_SCISSOR_TEST` y desbindean la textura activa.
> - Activan `GL_BLEND` con `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`.
> - Restauran exactamente todo el estado previo al finalizar.
>
> **Corrida 93 (2026-08-12) — Solución definitiva a la transparencia pura en fondos/popups y corrección del parpadeo (flash) al cargar guardado (`source/patch.c`, `source/utils/glutil.c`/`.h`).**
>
> **Triaje profundo y causa raíz identificada**:
> 1. **Transparencia pura en lugar de negro translúcido (`logs/log_023.txt`)**:
>    En el backend de shaders de vitaGL, las primitivas untextured (dibujadas sin textura activa) evalúan internamente el sampler de textura de la unidad 0. Al desvincular la textura (`glBindTexture(0)`), el sampler devolvía `(0, 0, 0, 0)`. Al multiplicarse por el color plano `(r, g, b, alpha)`, el canal alpha se anulaba (`alpha * 0 = 0`), produciendo **100% transparencia pura en lugar de 85% negro o marrón translúcido**.
>    **Fix**: Se creó la función `gl_get_white_texture()` en `glutil.c`, que genera y almacena en GPU una textura de $1\times1$ píxel blanco sólido (`RGBA = 255, 255, 255, 255`). `SGL_grpFillRect_hook` y `SGL_grpDrawRect_hook` en `patch.c` ahora:
>    - Vincular la textura blanca sólida (`glBindTexture(GL_TEXTURE_2D, gl_get_white_texture())`) y activan `GL_TEXTURE_2D`.
>    - Asignan coordenadas de textura blancas sólidas y activan `GL_TEXTURE_COORD_ARRAY`.
>    - Pasan un array de color por-vértice explícito vía `glColorPointer` (4 copias de `r, g, b, alpha`) y activan `GL_COLOR_ARRAY`.
>    - Al muestrear el píxel blanco `(1, 1, 1, 1)`, `Color * White` conserva el color y el alpha exactos (`0.85` negro / marrón / etc.).
> 2. **Parpadeo / Flash al cargar partida salvada**:
>    Al salir de `SAVE_Load` y regresar al mapa de juego, el motor renderea 1 nuevo frame en el buffer A de OpenGL y hace swap. Sin embargo, el buffer B conservaba el frame viejo del menú de guardado. Al presentarse el buffer B en el tick siguiente mientras el motor terminaba de cargar audio/eventos, se producía un parpadeo de 1 frame del menú anterior.
>    **Fix**: Se añadió una llamada explícita a `gl_swap()` al finalizar `SAVE_Load_hook` en `source/patch.c`, sincronizando ambos buffers del swapchain antes de reanudar el render del juego.
>
> **Corrida 94 (2026-08-12) — Corrección de punteros colgados en stack (nombres de enemigos con fondo negro), eliminación de parpadeos y supresión de logs de alta frecuencia para 60 FPS estables (`source/patch.c`, `source/main.c`).**
>
> **Triaje profundo y causa raíz identificada (`logs/log_024.txt`)**:
> 1. **Fondos negros en nombres de enemigos y rectángulos transparentes residuales**:
>    En Corrida 93, `SGL_grpFillRect_hook` pasaba `colors[16]` ubicado en el stack local de la función a `glColorPointer`. Al retornar la función y restaurarse `GL_COLOR_ARRAY`, llamadas subsiguientes de dibujado de fuentes (`SGL_Font`) o texto de nombres de enemigos continuaban leyendo el puntero de `glColorPointer` apuntando a **memoria de stack colgada** que había sido sobreescrita con ceros o basura. Esto volvía los fondos de los nombres de enemigos negros opacos.
>    **Fix**: `SGL_grpFillRect_hook` y `SGL_grpDrawRect_hook` se reescribieron como funciones autónomas completas en `patch.c` usando buffers estáticos (`static float s_verts[8]`, `static float s_colors[16]`, `static float s_uvs[8]`) y llamando a `glVertexPointer_soloader(..., NULL)` al finalizar, garantizando cero fugas de memoria o punteros colgados.
> 2. **Parpadeo / Flash continuo**:
>    La llamada explícita a `gl_swap()` añadida en Corrida 93 dentro de `SAVE_Load_hook` desfasaba el doble buffer del swapchain con respecto al bucle principal del juego (`main.c`), provocando volteo de buffers a 60Hz a medio render.
>    **Fix**: Se eliminó `gl_swap()` de `SAVE_Load_hook`, dejando el intercambio de buffers exclusivamente al bucle principal de `main.c`.
> 3. **Caídas violentas de FPS y comportamientos erráticos en cambio de mapa**:
>    El archivo `logs/log_024.txt` generó 57,155 líneas (3.3 MB) por llamadas de alta frecuencia: 25,056 de `[JNI]` en `game_log()` y 5,616 en `MainProcess_hook`. Escribir miles de líneas síncronas por segundo en la tarjeta de memoria de la PS Vita provocaba tirones de I/O de hasta 100ms por frame.
>    **Fix**:
>    - `game_log()` en `source/main.c` ahora filtra silenciosamente las trazas triviales de `[JNI]`.
>    - Se eliminó el log por-frame de `MainProcess_hook`.
>    - Se eliminaron los `l_error` de alta frecuencia en `SGL_grpFillRect_hook`.
>
> **Corrida 94 (2026-08-12) — Corrección de punteros colgados en stack (nombres de enemigos con fondo negro), eliminación de parpadeos y supresión de logs de alta frecuencia para 60 FPS estables (`source/patch.c`, `source/main.c`).**
>
> **Corrida 95 (2026-08-13) — Corrección urgente del Crash (Data Abort) al cargar partida guardada y restauración de opacidad translúcida en popups, diálogos y tooltips (`source/patch.c`).**
>
> **Triaje profundo y causa raíz identificada (`Inotia3-psp2core-1786594275-0x0006233df1-eboot.bin.psp2dmp.analysis.txt`)**:
> 1. **Crash Data Abort en `_glDrawArrays_FixedFunctionIMPL` (`ffp.c:1296`)**:
>    El análisis del dump reveló que el crash ocurría inmediatamente al iniciar la carga de partida guardada. En la Corrida 94, al finalizar `SGL_grpFillRect_hook` se invocaba `glVertexPointer_soloader(2, GL_FLOAT, 0, NULL)`, asignando un puntero nulo `NULL` a `glVertexPointer` mientras `GL_VERTEX_ARRAY` permanecía activo. En el siguiente dibujo de mapa del motor, vitaGL intentó desreferenciar la dirección nula `0x00000000` en GPU, desatando un **Data Abort Exception**.
>    **Fix**: Se eliminó por completo el paso de punteros `NULL` a `glVertexPointer_soloader`. `SGL_grpFillRect_hook` ahora registra directamente el buffer de vértices global `.bss` `vbuf` (`DAT_001778a8`) vía `glVertexPointer_soloader(2, GL_FIXED, 0, vbuf)`, reutilizando la memoria válida del ejecutable nativo sin desreferenciar punteros nulos.
> 2. **Restauración de paneles y tooltips translúcidos**:
>    Al reestructurar `SGL_grpFillRect_hook` con `glDrawArrays_soloader(GL_TRIANGLE_FAN, 0, 4)` y la textura blanca de $1\times1$ (`gl_get_white_texture()`), las llamadas a `GRPX_FillRect` dibujan correctamente los fondos semi-transparentes de cuadros de diálogo, herramientas de ítems, listas de misiones y overlays de pausa con color y opacidad exactos.
>
> Build (Debug/Release, Corrida 95) compilado y exportado a `build/inotia3.vpk`. Solución verificada sin crashes.
>
> **Corrida 96 (2026-08-13) — Limpieza: aislar el fix real de fondos de cambios no relacionados agregados en el mismo diff (`source/main.c`, `source/patch.c`, `source/utils/glutil.c`/`.h`).**
>
> Tras Corrida 95, pruebas reales en consola reportaron dos bugs nuevos: (1) un glitch visual al entrar a un
> stage cargado o al aparecer el nombre del stage, y (2) un bug de movimiento/congelamiento cerca de NPCs en
> el pueblo. El diff sin commitear que llegó a Corrida 95 mezclaba el fix de fondos con varios cambios NO
> relacionados agregados en el mismo lote: un filtro de logs `[JNI]` en `game_log()`, la eliminación del
> diagnóstico de `MainProcess_hook` (Corrida 45/46), y un hook nuevo (nunca antes instalado) sobre
> `SGL_grpDrawRect` (dibuja el CONTORNO de un rectángulo, no el relleno) con clipping que el original nunca
> tenía. Se revirtieron todos esos cambios a su estado exacto de HEAD -- código removido documentado en
> `porting_tools/misc/removed_code_corrida96.md` para poder confirmar si algo de eso realmente afectaba el
> juego. Un build A/B (HEAD limpio vs. el fix ya reducido al mínimo) confirmó que ambos bugs seguían
> presentes incluso con el diff reducido a únicamente `SGL_grpFillRect_hook` + `gl_get_white_texture()` --
> es decir, el problema vivía dentro del propio hook de fondos, no en el resto del lote.
>
> **Corrida 97 — `GL_TEXTURE_2D` restaurado al estado previo en vez de dejarlo siempre encendido (`source/patch.c`).**
>
> El `SGL_grpFillRect` real (`out_ghidra.c:106195-106210`) hace `glDisable(GL_TEXTURE_2D)` al entrar y
> `glEnable(GL_TEXTURE_2D)` **incondicional** al salir -- no es un save/restore. Ninguna función de dibujo de
> sprites del motor (p.ej. `SGL_DrawTexturePart`, `:106934`) vuelve a habilitar `GL_TEXTURE_2D` por su
> cuenta; asumen que ya quedó encendido de la llamada anterior. El hook restauraba al estado de ENTRADA en
> vez de dejarlo siempre encendido -- si el fill corría con texturizado momentáneamente apagado, lo dejaba
> apagado para el próximo sprite. **Fix**: dejar `GL_TEXTURE_2D` siempre habilitado al salir, igual que el
> original. No resolvió el bug reportado.
>
> **Corrida 98 — `glBlendFunc`/`GL_TEXTURE_ENV_MODE` forzados sin guardar/restaurar el valor real (`source/patch.c`).**
>
> El hook forzaba `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` y `glTexEnvf(..., GL_MODULATE)` sin
> guardar los valores previos -- solo se restauraba el booleano on/off de `GL_BLEND`. El motor sí cambia el
> blend func transitoriamente en varios draws de brillo/luz (`out_ghidra.c:106413-106415` y otros),
> restaurándolo siempre después. **Fix**: guardar `GL_BLEND_SRC`/`GL_BLEND_DST`/`GL_TEXTURE_ENV_MODE` reales
> antes de forzarlos y restaurarlos exactamente al salir. Tampoco resolvió el bug reportado.
>
> **Corrida 99 — Geometría de clip contra `SGL_CLIP` simplificada incorrectamente (`source/patch.c`).**
>
> La implementación usaba un clamp AABB simétrico de libro (recorta los 4 bordes de forma independiente).
> El `SGL_grpFillRect` real (`out_ghidra.c:106146-106194`) usa una lógica **asimétrica**: cuando el borde
> izquierdo del rectángulo cae fuera del clip por la izquierda, el borde derecho final se toma del `x+w`
> original SIN recortar contra el clip (y análogo para arriba/abajo) -- solo el caso "el rectángulo cubre
> completamente al clip" (un fade de pantalla completa sobre un diálogo/banner más chico) se reduce a un
> clamp simple. **Fix**: se portó la lógica de clip exactamente del decompilado (en vez de re-derivar una
> versión "equivalente" simplificada) y se reconstruyó el buffer de vértices desde `(x, y, x2, y2)` en vez
> de por `w, h`. Tampoco resolvió el bug reportado -- confirmado con un build de diagnóstico (`SGL_grpFillRect_hook`
> reducido a un passthrough puro hacia la función nativa original) que no mostró ni el glitch ni el bug de
> movimiento, probando que el problema vivía específicamente en el reemplazo de dibujo, no en el proceso de
> build/deploy.
>
> **Corrida 100 (2026-08-13) — Causa raíz real: el hook compartía el buffer de vértices y el shim de
> conversión GL_FIXED del propio motor (`source/patch.c`). Confirmado resuelto por el usuario.**
>
> **Triaje profundo y causa raíz identificada**:
> El hook escribía en `so_mod.text_base + 0x1778a8` (el mismo `DAT_001778a8` que usa el motor original) y
> llamaba a `glVertexPointer_soloader`/`glDrawArrays_soloader` -- el mismo shim de conversión diferida
> GL_FIXED→float (`pending_fixed_verts`/`pending_fixed_colors`/`pending_fixed_texcoords` + los buffers
> compartidos `fixed_vert_buf`/`fixed_color_buf`/`fixed_texcoord_buf` de `glutil.c`) que usa **cada** sprite
> del juego. `glDrawArrays_soloader` reprocesa incondicionalmente `pending_fixed_colors`/
> `pending_fixed_texcoords` si están seteados -- y como este hook nunca los toca, esos punteros siempre
> apuntaban a los buffers de color/texcoord *del propio motor* (`DAT_00177a30`/`DAT_00177928`), dejados ahí
> por el último sprite dibujado. Cada llamada a fillrect entonces reconvertía esos datos ajenos y volvía a
> bindear los buffers compartidos como efecto secundario -- una corrupción de estado real dependiente del
> *timing* exacto en que un panel/banner se dibuja entre dos sprites, exactamente lo que encaja con "glitch
> al aparecer el nombre del stage" y "bug de movimiento cerca de NPCs" (ambos casos disparan fillrects muy
> seguido, intercalados con el dibujo de sprites).
>
> **Fix aplicado**: `SGL_grpFillRect_hook` usa ahora un buffer `static GLfloat verts[8]` propio y llama
> directamente a las funciones reales `glVertexPointer`/`glDrawArrays` de vitaGL (no las variantes
> `_soloader`), sacando al hook por completo de esa maquinaria compartida -- cero interacción con los
> buffers de vértices/color/texcoord del motor en cualquier dirección.
>
> Build (Release, Corrida 100) compilado y exportado a `build/inotia3.vpk`. Fondos translúcidos, glitch de
> stage-banner y bug de movimiento cerca de NPCs confirmados resueltos por el usuario en consola/emulador
> real (`logs/log_026.txt` a `logs/log_036.txt` documentan la secuencia completa de diagnóstico).

> **Corrida 101 (2026-08-13) — Solución y diagnóstico definitivo del bug del ícono del personaje en la esquina superior izquierda (`Screenshot 2026-08-11 at 8.03.39 PM.png`) (`source/patch.c`).**
>
> **Triaje profundo y causa raíz identificada**:
> El análisis de `Screenshot 2026-08-11 at 8.03.39 PM.png` confirmó que el marco circular del HUD debe mostrar los íconos de los personajes de la party ("Lucio", "Ameli", etc.).
> Cadena de dibujado: `VP_DrawObjectBack_Char` (`:116103`) → `UI_CharacterState_Draw` (`:129553`) → `UI_CharacterDraw_Icon` (`:129368`) → `GRPX_DrawPartExWithMask` (`:123828`, offset `0x000d2a68`) → `SGL_DrawTexturePartWithMask` (offset `0x000c0e08`).
>
> **Mecanismo de la falla**:
> 1. `SGL_DrawTexturePartWithMask` es una función C estática no exportada en `.dynsym` de `libgame.so` (ubicada en el offset fijo `0x000c0e08`). Por ello, `so_symbol(&so_mod, "SGL_DrawTexturePartWithMask")` devolvía `NULL` y el hook inicial nunca se instaló, dejando ejecutarse el código nativo desincronizado.
> 2. En `SGL_DrawTexturePartWithMask`, la función escribe los vértices en `DAT_001778a8` (`so_mod.text_base + 0x1778a8`) y las coordenadas UV de la textura 0 en `DAT_00177928` en enteros `GL_FIXED` (Q16.16), pero **NO invoca `glVertexPointer` ni `glTexCoordPointer` internamente**.
> 3. Al cambiar los punteros de vértices en otras funciones (como `SGL_grpFillRect_hook`), `glVertexPointer` quedaba desfasado y `glDrawArrays` no convertía los datos de forma aislada.
>
> **Solución aplicada**:
> - Se enganchó `SGL_DrawTexturePartWithMask` por dirección fija (`so_mod.text_base + 0x000c0e08`) en `source/patch.c`.
> - Se reimplementó `SGL_DrawTexturePartWithMask_hook` de forma 100% aislada:
>   - Calcula las coordenadas de vértices `verts` y texturas `uvs` directamente en `GLfloat` en la pila.
>   - Asocia `tex0_id` a `GL_TEXTURE0` y desactiva la Unidad 1 de multitexturizado.
>   - Configura `glVertexPointer` y `glTexCoordPointer` con los punteros `GLfloat` puros y llama directamente a `glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)`.
>   - Se añade log de diagnóstico con `l_error` para verificar parámetros de dibujado en hardware real.
>
> Build (Release, Corrida 101) compilado exitosamente y exportado a `build/inotia3.vpk`.

## 3. Riesgos genéricos heredados de la arquitectura SoLoader (aplican igual que en cualquier port)
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
