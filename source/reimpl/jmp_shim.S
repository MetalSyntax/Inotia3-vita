/*
 * jmp_shim.S
 *
 * Fase 9 (crash real en consola, 2026-07-21): un stack-protector check dentro
 * de png_create_read_struct_2 (libpng, bundleado dentro de libgame.so) fallo
 * -- __stack_chk_fail detectado en decompiled_so/libgame_armeabi/out_ghidra.c
 * justo despues de la segunda llamada a setjmp() de esa funcion. dynlib.c ya
 * traia un comentario "TODO: May have different struct size?" sobre mapear
 * setjmp/longjmp directo a los del newlib de VitaSDK -- el .so fue compilado
 * contra el jmp_buf de Android Bionic (tamano/layout fijo, desconocido para
 * nosotros pero definido en tiempo de compilacion dentro del .so), y
 * png_struct reserva espacio para ese jmp_buf embebido asumiendo el tamano de
 * Bionic. Si el jmp_buf real de newlib de VitaSDK es mas grande, setjmp()
 * escribe mas alla de lo que el .so reservo para ese campo, corrompiendo los
 * campos siguientes de png_struct (heap) -- consistente con que el crash
 * aparezca recien al final de la funcion (canary check), no en la llamada a
 * setjmp() en si.
 *
 * Fix: implementar setjmp/longjmp nosotros mismos, en vez de confiar en
 * ningun jmp_buf interno (ni el de Bionic ni el de newlib) -- guardamos
 * unicamente el set minimo de registros callee-saved de AAPCS (r4-r11, sp,
 * lr: 10 words = 40 bytes) en el buffer que el LLAMADOR (el .so) ya paso.
 * Cualquier jmp_buf real (Bionic incluido) reserva AL MENOS ese tamano --
 * es el minimo que CUALQUIER setjmp/longjmp que respete la ABI de ARM
 * necesita para funcionar -- asi que esto nunca escribe mas alla de lo que
 * el .so ya reservo, sea cual sea el tamano real de su jmp_buf original.
 *
 * No hay soporte de guardar la mascara de senales (sigjmp_buf) -- libpng no
 * lo necesita (usa setjmp/longjmp lisos, no sigsetjmp/siglongjmp).
 */

.syntax unified
.thumb

// Fase 9 (Corrida 5, 2026-07-21): the stack-protector check inside
// png_create_read_struct_2 kept failing even with the fix below verified
// correct, so each call is logged (capped, jmp_shim_log.c) before doing the
// real work -- r0/lr (setjmp) or r0/r1 (longjmp) are pushed/popped around
// the logging call since any C function call clobbers r0-r3/r12/lr per
// AAPCS; neither push/pop touches r4-r11/sp, so the actual setjmp/longjmp
// logic below is unchanged from the verified version.

.global setjmp_soloader
.thumb_func
setjmp_soloader:
    push {r0, lr}
    bl jmp_shim_log_setjmp
    pop {r0, lr}
    stmia r0!, {r4-r11}
    mov r2, sp
    str r2, [r0]
    str lr, [r0, #4]
    movs r0, #0
    bx lr

.global longjmp_soloader
.thumb_func
longjmp_soloader:
    push {r0, r1}
    bl jmp_shim_log_longjmp
    pop {r0, r1}
    ldmia r0!, {r4-r11}
    ldr r2, [r0]
    mov sp, r2
    ldr lr, [r0, #4]
    movs r0, r1
    cmp r0, #0
    bne 1f
    movs r0, #1
1:
    bx lr
