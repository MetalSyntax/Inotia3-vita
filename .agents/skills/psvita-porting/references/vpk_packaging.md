# Errores de Empaquetado e Instalación del VPK

Un archivo `.vpk` puede fallar durante la instalación en la PS Vita (típicamente al ~99%) debido a problemas en la estructura de los assets de la LiveArea o en la configuración de `template.xml`.

---

## 1. Error `0x8010113D` (LiveArea Assets Incompatibles)

Este error ocurre cuando las imágenes del LiveArea (`icon0.png`, `pic0.png`, `startup.png`, `bg0.png`) no cumplen con la especificación exacta de la PS Vita.

> [!IMPORTANT]
> La PS Vita requiere obligatoriamente que las imágenes de la LiveArea estén guardadas como **PNG Indexados de 8 bits (Paleta de 256 colores)**.

### Verificación del Formato de Imagen con Python:

```python
import struct

def check_png_indexed(path):
    with open(path, "rb") as f:
        data = f.read()
    # Leer campos del chunk IHDR (byte 16 en adelante)
    w, h, bitdepth, colortype = struct.unpack(">IIBB", data[16:26])
    # colortype == 3 indica paleta indexada (requerido por la Vita)
    print(f"{path}: {w}x{h}, BitDepth: {bitdepth}, ColorType: {colortype} {'[OK]' if colortype == 3 else '[INVALIDO]'}")
```

### Conversión Automatizada con `pngquant`:
```bash
pngquant --force --ext .png 256 icon0.png pic0.png startup.png bg0.png
```

---

## 2. Configuración de `template.xml` y `style="a1"`

> [!WARNING]
> En la LiveArea, el atributo `style="a1"` en `template.xml` es el estándar recomendado para homebrew. Evita usar estilos como `"psmobile"`, ya que imponen restricciones de validación adicionales durante la creación de la burbuja.

---

## 3. Checklist de Verificación de Integridad del VPK

1. Descomprime el `.vpk` (es un archivo `.zip` estándar) y confirma la existencia y tamaño de:
   - `eboot.bin`
   - `sce_sys/param.sfo`
   - `sce_sys/icon0.png` (8-bit indexed)
   - `sce_sys/livearea/contents/template.xml`
2. Revisa que ningún archivo de la LiveArea tenga tamaño 0 bytes.
