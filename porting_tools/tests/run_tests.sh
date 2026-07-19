#!/bin/bash
# Host-side pre-build test suite -- placeholder para Inotia 3.
#
# Este script se heredó de un port anterior (Prince of Persia Classic /
# cocos2d-x) y probaba decodificación de audio contra libcocos2d.so /
# libcocosdenshion.so y una convención de rutas "Extra/Audio/" que no existen
# en este proyecto. Inotia 3 usa un sistema de sonido propio de Com2uS
# (SOUNDSYSTEM_*, ver PORTING_PLAN.md) con audio en .mmf y .ogg -- no hay
# todavía código de test para ese pipeline, así que no se puede adaptar
# mecánicamente sin escribirlo de cero.
#
# Cuando se implemente la carga de audio del loader (Fase de Audio en
# PORTING_PLAN.md), escribir acá un test equivalente contra el decoder real
# que se use (ej. Tremor para .ogg) antes de reactivar este script.
echo "[run_tests.sh] Sin tests de audio para Inotia 3 todavía -- ver comentario en este archivo."
exit 0
