---
name: psvita-porting
description: Guía práctica, basada en bugs reales encontrados y resueltos en consola física, para portar juegos Android (especialmente motores basados en cocos2d-x + un "soloader" que carga los .so nativos directamente) a PS Vita. Cubre debugging en hardware real, empaquetado de assets (apk/obb vs. archivos sueltos), stubs de JNI, manejo de input táctil, y errores de instalación de VPK.
---

# Port de juegos Android a PS Vita (soloader + cocos2d-x)

Esta skill resume patrones y bugs **confirmados en consola física** (no solo en emulador) durante el port de
un juego Android basado en cocos2d-x a PS Vita usando la arquitectura típica de "soloader" (un ejecutable
Vita nativo que carga los `.so` de Android sin relinkearlos, resolviendo sus imports contra una tabla de
funciones propia y emulando JNI con algo como FalsoJNI). La mayoría de estos bugs no son específicos de un
juego puntual — son errores de una clase entera de ports armados con este patrón.

## Cuándo usar esta skill

- Estás portando (o depurando) un juego Android a PS Vita con un soloader (`so_util`, FalsoJNI o similar).
- El juego usa cocos2d-x (o un motor similar que separa "assets sueltos" de "assets dentro del .apk/.obb").
- Tenés acceso a hardware real para probar, no solo a un emulador (Vita3K) — varios de estos bugs **solo se
  manifiestan en consola real** (el emulador es más permisivo en algunos casos, más estricto/distinto en
  otros).

## Contenido

- **[references/hardware_debugging.md](references/hardware_debugging.md)** — Cómo diagnosticar crashes y
  colgadas en consola real sin plugins de red: logging a archivo por ejecución, y el método iterativo de
  "un `fopen(...): 0x0` a la vez" para no adivinar a ciegas.
- **[references/asset_packaging.md](references/asset_packaging.md)** — La decisión `.apk`/`.obb` vs. archivos
  sueltos, el mecanismo de fallback a ZIP de `CCFileUtils::getFileData`, y cómo armar `.apk`/`.obb` mínimos
  o completos con Python en vez de usar los archivos originales pesados.
- **[references/jni_stubs.md](references/jni_stubs.md)** — Patrones para escribir stubs de métodos JNI que
  el motor llama y no están implementados: cuándo un no-op simple alcanza y cuándo hace falta disparar un
  callback de "completado" para no colgar el motor.
- **[references/input_handling.md](references/input_handling.md)** — Touch e input de botones: el touch está
  apagado por default en la Vita, y un bug sutil de ID de touch puede dejar el input "trabado" para siempre.
- **[references/vpk_packaging.md](references/vpk_packaging.md)** — Errores de instalación del `.vpk`
  (LiveArea, `pngquant`, `template.xml`) que no tienen nada que ver con el gameplay.
- **[references/toolchain_gotchas.md](references/toolchain_gotchas.md)** — Problemas de build específicos de
  este tipo de proyecto (rutas con espacios, políticas de CMake, symlinks que desaparecen).
