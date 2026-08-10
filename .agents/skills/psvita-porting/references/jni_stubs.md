# Stubs de JNI y Manejo de Callbacks Asíncronos

En la arquitectura `so-loader`, el ejecutable de la Vita emula la capa JNI de Android interconectando llamadas entre la biblioteca nativa `.so` y las funciones C del loader.

---

## 1. Comportamiento Estándar de Stubs JNI

Muchos métodos de Java consultados por la biblioteca nativa a través de `GetMethodID` o `GetStaticMethodID` pueden retornar un stub `no-op` (vacío) si corresponden a analíticas, publicidad o servicios de Google Play.

> [!WARNING]
> No todas las funciones no implementadas son seguras de ignorar. Si el motor espera un callback de retorno, un stub no-op provocará que el juego **se cuelgue indefinidamente (freeze)** sin mostrar ningún mensaje de error ni crash en consola.

---

## 2. Detección del Patrón de Callback Pendiente

### Síntomas en el Log de Debug:
* La última línea del log indica el inicio de una acción (ej: `"Playing Video"`, `"Showing WebDialog"`, `"Requesting IAP"`).
* El juego se congela totalmente y no responde a ningún botón ni touch.
* No existe ningún reporte de crash (`.psp2dmp`) generado.

---

## 3. Implementación de Callbacks Automáticos en Stubs JNI

Busca en la biblioteca `.so` con `arm-vita-eabi-nm` la función exportada que el motor usa como listener o callback de finalización (ej: `onVideoCompleted`, `onDialogClosed`):

```bash
arm-vita-eabi-nm -D --defined-only libcocos2d.so | c++filt | grep -i video
# Salida: Java_org_cocos2dx_lib_Cocos2dxVideo_onVideoCompleted
```

Dispara inmediatamente dicho callback dentro del stub para simular que la operación asíncrona finalizó con éxito:

```c
void Stub_playVideo(jmethodID id, va_list args) {
    // Resolver el símbolo exportado en el .so
    void (*onCompleted)(JNIEnv *env, jobject thiz) =
        (void *) so_symbol(&engine_mod, "Java_org_cocos2dx_lib_Cocos2dxVideo_onVideoCompleted");
    
    if (onCompleted) {
        onCompleted(&jni, NULL); // Simular finalización inmediata
    }
}
```

---

## 4. Verificación de Tablas de Registro JNI

> [!TIP]
> Verifica que el stub esté registrado tanto en la tabla de nombres de métodos (`nameToMethodId`) como en la tabla correspondiente al tipo de retorno (`MethodsVoid`, `MethodsObject`, `MethodsInt`, etc.) implementada por el `so-loader`.
