# Empaquetado de Assets: APK/OBB vs. Archivos Sueltos

En ports basados en `so-loader` y motores como `cocos2d-x`, la estructura y empaquetado de assets impacta directamente en los tiempos de carga, el espacio en tarjeta de memoria y el rendimiento de E/S de la consola.

---

## 1. Decisiones de Empaquetado: Archivos Sueltos vs. Contenedores `.apk`/`.obb`

| Estrategia | Ventajas | Desventajas | Cuándo Usar |
| :--- | :--- | :--- | :--- |
| **Archivos Sueltos** | Fácil inspección y edición de assets individuales. | Miles de archivos pequeños ralentizan el sistema de archivos FAT32/exFAT de la Vita. | En fase de desarrollo inicial o cuando hay pocos assets. |
| **Mínimo `.apk` / `.obb`** | Cargas de E/S ultra rápidas, paquete limpio en `ux0:data/`. | Requiere crear el ZIP contenedor con las rutas internas esperadas. | **Recomendado para Release**. |

---

## 2. Fallback a ZIP en `CCFileUtils::getFileData`

El motor cocos2d-x suele buscar assets en dos niveles:
1. Intenta abrir el archivo directamente desde el sistema de archivos (`fopen`).
2. Si falla, cae al mecanismo de fallback buscando el asset dentro del paquete de assets `.apk`/`.obb` (que es un archivo de estructura ZIP).

> [!IMPORTANT]
> **No copies el `.apk` / `.obb` original completo** de Android si contiene assets sobrantes de otras resoluciones o plataformas. Arma un `.apk`/`.obb` comprimido de tamaño mínimo que contenga únicamente las carpetas y assets requeridos.

---

## 3. Generación de `.obb` Mínimo con Python

Usa compresión `ZIP_STORED` (sin compresión DEFLATE adicional) para imágenes `.png` y audios `.ogg` que ya están comprimidos. Esto ahorra ciclos de CPU al descomprimir en tiempo de ejecución:

```python
import zipfile
import os

# STORED: la mayoría del contenido (png, ogg) ya está comprimido
with zipfile.ZipFile("main.obb", "w", zipfile.ZIP_STORED) as z:
    for root, dirs, files in os.walk("Data"):
        for fname in files:
            full = os.path.join(root, fname)
            rel = os.path.relpath(full, "Data")
            # El prefijo interno (ej: Data_960_576) debe coincidir exactamente con el log del juego
            z.write(full, f"Data_960_576/{rel}")
```

---

## 4. Cómo Confirmar el Prefijo de Rutas Internas (Sin Adivinar)

> [!TIP]
> Revisa el log de ejecución en tiempo de real. Cuando el motor recurre al ZIP/OBB, suele loguear la ruta completa que busca abrir:
> `fullPath = Data_960_576/Texture/Menu/buttons.png`
> El string `Data_960_576/` es el prefijo exacto que debes usar dentro del archivo ZIP/OBB.
