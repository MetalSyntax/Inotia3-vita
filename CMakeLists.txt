cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(inotia3 C CXX ASM)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Vita application configuration
set(VITA_APP_NAME "Inotia 3")
set(VITA_TITLEID "INOTIA3PV")
set(VITA_VPKNAME "inotia3")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# Optional: used for "send" and "dump" targets
set(PSVITAIP "192.168.3.15" CACHE STRING "PSVita IP (for FTP access)")
set(PARSECOREPATH "$ENV{PARSECOREPATH}" CACHE STRING "vita-parse-core main.py")

# Game-specific definitions
set(DATA_PATH "ux0:data/inotia3/" CACHE STRING "Path to data (with trailing /)")
set(SO_PATH "${DATA_PATH}libgame.so" CACHE STRING "Path to .so")
add_definitions(-DDATA_PATH="${DATA_PATH}" -DSO_PATH="${SO_PATH}")

option(USE_SCELIBC_IO "Use SceLibcBridge for IO and some other std functions" ON)
if (USE_SCELIBC_IO)
  add_definitions(-DUSE_SCELIBC_IO)
endif()

set(SHADER_FORMAT "GLSL" CACHE STRING "Preferred shader format (one of 'GLSL', 'CG', 'GXP')")
if (${SHADER_FORMAT} STREQUAL "GLSL")
  add_definitions(-DUSE_GLSL_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "CG")
  add_definitions(-DUSE_CG_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "GXP")
  add_definitions(-DUSE_GXP_SHADERS)
else()
  message(FATAL_ERROR "SHADER_FORMAT option can only be one of: 'GLSL', 'CG', 'GXP'")
endif()

set(valid_dump_formats GLSL CG)
option(DUMP_COMPILED_SHADERS "When using GLSL or CG shader format, cache compiled shaders on disk" ON)
if (DUMP_COMPILED_SHADERS)
  if (SHADER_FORMAT IN_LIST valid_dump_formats)
    add_definitions(-DDUMP_COMPILED_SHADERS)
  else()
    message(WARNING "DUMP_COMPILED_SHADERS has no effect for the chosen shader format ${SHADER_FORMAT}")
  endif()
endif()

# Unique debug definition for loader's logging to not interfere with anything
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_SOLOADER)
  # Verbose FalsoJNI logging (every JNI call does a vsnprintf, twice) --
  # only worth the cost in a Debug build.
  add_definitions(-DFALSOJNI_DEBUGLEVEL=0)
else()
  # Release: skip the ALL/INFO log bodies at compile time (see
  # FalsoJNI_Logger.c's #if guards) instead of just no-op'ing them through
  # l_info -- the vsnprintf formatting cost was paid either way before this
  # change, on every JNI call, even in "Release" builds.
  add_definitions(-DFALSOJNI_DEBUGLEVEL=2)
endif()

## vitaGL vendorizada (misma de Prince of Persia / Zenonia 3/4: upstream cd3791e
## + mods de compat, ver vendor/vitaGL/VENDORED.md) en vez del libvitaGL.a global
## del SDK, que se recompilo el 2026-09-03 con flags desconocidos -> pantalla negra.
## SOFTFP_ABI=1 es OBLIGATORIO: el loader usa softfp (default de vitasdk).
set(VITAGL_VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/vitaGL)
set(VITAGL_MAKE_FLAGS_STR "SOFTFP_ABI=1 NO_DEBUG=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1 HAVE_GLSL_UBOS=1 SAMPLERS_SPEEDHACK=1 DRAW_SPEEDHACK=2")
separate_arguments(VITAGL_MAKE_FLAGS UNIX_COMMAND "${VITAGL_MAKE_FLAGS_STR}")
include(ExternalProject)
ExternalProject_Add(
  vitaGL_vendored
  SOURCE_DIR ${VITAGL_VENDOR_DIR}
  BUILD_IN_SOURCE 1
  CONFIGURE_COMMAND ""
  BUILD_COMMAND make ${VITAGL_MAKE_FLAGS}
  INSTALL_COMMAND ""
)
add_library(vitaGL_vendored_lib STATIC IMPORTED)
set_target_properties(vitaGL_vendored_lib PROPERTIES IMPORTED_LOCATION ${VITAGL_VENDOR_DIR}/libvitaGL.a)
add_dependencies(vitaGL_vendored_lib vitaGL_vendored)

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -O3 -g -ffast-math -Wno-deprecated -Wl,--allow-multiple-definition")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=gnu++20 -Wno-write-strings -Wno-psabi")

# Experimental: neither ../Zenonia2-vita nor ../Zenonia3-vita use -flto -- this
# is untested territory, not a port of a confirmed sibling technique. LTO can
# change inlining/reordering across translation units in ways that -O3 alone
# doesn't, which is a real (if unlikely) risk around so_util's manual .so
# relocation and FalsoJNI's function-pointer stubs. OFF by default: opt in
# with -DENABLE_LTO=ON in a separate build dir and compare .velf size + FPS
# on hardware before ever making this the default.
option(ENABLE_LTO "Experimental: build with link-time optimization (-flto)" OFF)
if (ENABLE_LTO)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -flto")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto -O3")
endif()

add_executable(${CMAKE_PROJECT_NAME}
               source/main.c
               source/dynlib.c
               source/java.c
               source/patch.c
               source/wr_timer.c
               source/wr_touch.c
               source/overlay.c
               source/audio.c
               source/splash.c
               source/reimpl/apk_zip.c
               source/reimpl/jmp_shim.S
               source/reimpl/jmp_shim_log.c
               source/reimpl/bits/_ctype.c
               source/reimpl/egl.c
               source/reimpl/errno.c
               source/reimpl/io.c
               source/reimpl/log.c
               source/reimpl/mem.c
               source/reimpl/pthr.c
               source/reimpl/sys.c
               source/reimpl/time64.c
               source/reimpl/asset_manager.cpp
               source/utils/dialog.c
               source/utils/glutil.c
               source/utils/init.c
               source/utils/logger.c
               source/utils/settings.c
               source/utils/remap_menu.c
               source/utils/utils.c
               lib/falso_jni/FalsoJNI.c
               lib/falso_jni/FalsoJNI_ImplBridge.c
               lib/falso_jni/FalsoJNI_Logger.c
               lib/sha1/sha1.c
               lib/fios/fios.c
               lib/so_util/so_util.c)

if (ENABLE_LTO)
  # These three declare real libstdc++/libc runtime symbols as opaque
  # `extern void *` just to take their address for the loader's own
  # import/export tables (see dynlib.c's _Znwj/__throw_bad_alloc/etc, patch.c,
  # reimpl/sys.c). Fine for a normal per-TU compile+link (the linker only
  # matches by name), but -flto's whole-program merge cross-checks types and
  # hard-errors on "redeclared as function" the moment any LTO'd TU also sees
  # the real declaration (e.g. via asset_manager.cpp's C++ headers). Excluding
  # just these three from LTO keeps them as ordinary non-LTO objects, which
  # link into an LTO binary fine since only LTO objects participate in that
  # merge -- confirmed as the actual (not hypothetical) failure mode when
  # -flto was first tried here.
  set_source_files_properties(source/dynlib.c source/patch.c source/reimpl/sys.c
                               PROPERTIES COMPILE_OPTIONS "-fno-lto")
endif()

add_subdirectory(lib/libc_bridge)
add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib
                           PUBLIC ${VITAGL_VENDOR_DIR}/source
                           )

target_include_directories(${CMAKE_PROJECT_NAME}
                           PUBLIC ${CMAKE_SOURCE_DIR}/source
                           )

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      stdc++
                      vitaGL_vendored_lib
                      vitashark
                      SceShaccCgExt
                      taihen_stub
                      SceShaccCg_stub
                      vorbisidec
                      ogg
                      z
                      m
                      ScePgf_stub
                      mathneon
                      kubridge_stub
                      ${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a
                      SceHid_stub
                      SceTouch_stub
                      SceMotion_stub
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceAudio_stub
                      SceAudioIn_stub
                      SceCommonDialog_stub
                      SceDisplay_stub
                      SceKernelDmacMgr_stub
                      SceFios2_stub
                      SceCtrl_stub
                      SceGxm_stub
                      ScePower_stub
                      SceTouch_stub
                      SceVshBridge_stub
                      )

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
                VERSION ${VITA_VERSION}
                NAME ${VITA_APP_NAME}
                FILE ${CMAKE_SOURCE_DIR}/extras/cpuinfo cpuinfo
                ${CMAKE_SOURCE_DIR}/extras/meminfo meminfo
                ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png sce_sys/pic0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/startup.png sce_sys/livearea/contents/startup.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png sce_sys/livearea/contents/bg0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/template.xml sce_sys/livearea/contents/template.xml
                )

add_custom_target(send
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ${VITA_TITLEID} | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(send_kvdb
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ux0:/app/${VITA_TITLEID}/eboot.bin | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(dump
                  COMMAND ${CMAKE_SOURCE_DIR}/extras/scripts/get_dump.sh ${PSVITAIP}
                  COMMAND python3 ${PARSECOREPATH} coredump ${CMAKE_PROJECT_NAME}
                  )

add_custom_target(reboot
                  COMMAND echo reboot | nc ${PSVITAIP} 1338
                  )
