# Manejo de Input Táctil y Botones Físicos en PS Vita

---

## 1. Activación Obligatoria del Sensor Táctil

> [!IMPORTANT]
> El sensor táctil de la PS Vita viene desactivado por defecto al arrancar una aplicación nativa.
> Si no se activa explícitamente, `sceTouchPeek()` y `sceTouchRead()` **siempre devolverán `reportNum = 0`**.

Llama a esta función una sola vez durante la inicialización de la ventana/render:

```c
// Obligatorio antes del loop principal para recibir eventos táctiles
sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
```

---

## 2. Prevención de Corrupción de Heap por IDs de Touch

> [!CAUTION]
> `SceTouchReport::id` (`psp2/touch.h`) es un contador cíclico de 8 bits (0-255) que se incrementa en cada evento. **NUNCA envíes el `id` del hardware directamente a funciones de motores como `nativeTouchesBegin/Move/End`**.

Si el motor usa el ID recibido para indexar arreglos internos de tamaño fijo (típicamente de tamaño 5 en Android), pasar un ID mayor a 4 **escribirá fuera de los límites de memoria y corromperá el heap silenciosamente**, produciendo crashes aleatorios más adelante (ej. en llamadas a `free()`).

### Implementación Correcta con Asignación de Slots Estables (0-4):

```c
int lastX[5] = {-1, -1, -1, -1, -1};
int lastY[5] = {-1, -1, -1, -1, -1};
int slotHwId[5] = {-1, -1, -1, -1, -1};  // ID crudo del hardware ocupando el slot (-1 = libre)

int seen[5] = {0, 0, 0, 0, 0};
for (int r = 0; r < touch.reportNum && r < 5; r++) {
    int hwId = touch.report[r].id;
    int x = touch.report[r].x; 
    int y = touch.report[r].y;

    int slot = -1;
    for (int s = 0; s < 5; s++) if (slotHwId[s] == hwId) { slot = s; break; }
    if (slot == -1) {
        for (int s = 0; s < 5; s++) if (slotHwId[s] == -1) { slot = s; break; }
        if (slot == -1) continue;  // Más de 5 toques simultáneos -> se ignora el exceso
        slotHwId[slot] = hwId;
        lastX[slot] = -1; lastY[slot] = -1;
    }
    seen[slot] = 1;

    if (lastX[slot] == -1) 
        nativeTouchesBegin(jniEnv, NULL, slot, x, y);       // <- Usar slot (0-4), NO hwId
    else if (lastX[slot] != x || lastY[slot] != y) 
        nativeTouchesMove(jniEnv, NULL, slot, x, y);
    
    lastX[slot] = x; 
    lastY[slot] = y;
}

// Liberar slots correspondientes a dedos retirados
for (int s = 0; s < 5; s++) {
    if (slotHwId[s] != -1 && !seen[s]) {
        nativeTouchesEnd(jniEnv, NULL, s, lastX[s], lastY[s]);
        lastX[s] = -1; lastY[s] = -1; slotHwId[s] = -1;
    }
}
```

---

## 3. Touches Sintéticos para Joysticks/Botones Virtuales

> [!WARNING]
> No asignes un ID fijo fuera de rango (como ID `5`) para toques sintéticos mapeados a botones físicos. Mantenlos compitiendo dentro de los mismos 5 slots estándar asignándoles un ID virtual único (ej: `-2`).

```c
if (padWantsMovimiento) {
    reportHwId[reportCount] = -2;  // ID virtual estable para el joystick físico
    reportX[reportCount] = posX;
    reportY[reportCount] = posY;
    reportCount++;
}
```

---

## 4. Mapeo de Botones Físicos a Keycodes de Android

Mapea los botones de la Vita a keycodes estándar de Android (`KEYCODE_DPAD_LEFT=21`, `KEYCODE_DPAD_RIGHT=22`, `KEYCODE_BACK=4`, etc.) controlando los cambios de estado (Edge Triggering):

```c
if ((current_pad & SCE_CTRL_LEFT) && !(oldpad & SCE_CTRL_LEFT)) 
    nativeKeyDown(jniEnv, NULL, 21);
if (!(current_pad & SCE_CTRL_LEFT) && (oldpad & SCE_CTRL_LEFT)) 
    nativeKeyUp(jniEnv, NULL, 21);
```
