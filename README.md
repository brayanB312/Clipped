# Clipped — Historial Ultraligero de Portapapeles para Windows (Edición Retro Win98)

**Clipped** es una utilidad nativa de escritorio para Windows (C++20 + Win32 + DirectX 11 + SQLite3) que mantiene un historial persistente e instantáneo de todo lo que copias (**Textos e Imágenes**), con estética clásica de **Windows 95 / 98**, consumiendo **`< 1 MB` de RAM (`~0.15 MB` en reposo)** y **`0.0%` de CPU** en segundo plano.

---

## Características Principales

- **Texto e Imágenes (PNG en RAM/SQLite):** Guarda automáticamente textos UTF-8 y capturas/imágenes (`CF_DIB`) comprimidas a PNG sin pérdida.
- **Consumo Cero en Segundo Plano:** Usa `AddClipboardFormatListener` y `WaitMessage()` para dormir a **0.0% de CPU y 0.0% de GPU** mientras está oculto en el System Tray.
- **Auto-Pegado Instantáneo (`Ctrl + Alt + V`):**
  - Presiona **`Ctrl + Alt + V`** en cualquier aplicación para abrir la ventana flotante.
  - Filtra textos escribiendo o navega con las flechas **`↑` / `↓`** y presiona **`Enter`** (o haz clic) para pegar directamente en tu ventana activa.
- **Deduplicación y Elementos Fijados:** Si vuelves a copiar algo que ya existía, sube al primer lugar sin duplicarse. Puedes fijar (`Fijar`) elementos importantes para que nunca se borren.
- **100% Autónomo (Sin dependencias externas):** Compilado estáticamente (`/MT`), no requiere instalar *Visual C++ Redistributable*.

---

## Instalación Rápida (Usuario Final)

1. Ve a la sección [**Releases**](../../releases) de este repositorio en GitHub.
2. Descarga **`ClippedSetup.exe`** (o `Clipped-Portable-x64.zip` si prefieres un único ejecutable portable).
3. Ejecuta **`ClippedSetup.exe`**:
   - Se instalará en tu cuenta de usuario (sin pedir permisos de administrador).
   - Quedará **inmediatamente activo en segundo plano en la bandeja del sistema (System Tray)** y configurado para iniciar con Windows.
4. Presiona **`Ctrl + Alt + V`** en cualquier momento para abrir tu historial.

---

## Controles Rápidos

| Acción | Atajo / Control |
| :--- | :--- |
| **Abrir / Ocultar Clipped** | `Ctrl + Alt + V` o Clic izquierdo en el ícono del System Tray |
| **Moverse por la lista** | Flechas `↑` / `↓` |
| **Pegar elemento seleccionado** | `Enter` o Clic izquierdo sobre el elemento |
| **Cerrar ventana sin pegar** | `Escape` o Clic fuera de la ventana |
| **Opciones (Inicio con Windows / Salir)** | Clic derecho en el ícono del System Tray |

---

## Compilar desde Código Fuente (Desarrolladores)

### Requisitos
- Windows 10 / 11 (x64)
- Visual Studio 2022 (con carga de trabajo *Desarrollo para el escritorio con C++*) y CMake 3.20+

### Comandos
```powershell
cmake -B build -A x64
cmake --build build --config Release
```
El ejecutable autónomo se generará en `build\Release\Clipped.exe`.

---

## Cómo publicar una nueva versión en GitHub Releases

El repositorio incluye un flujo automático de GitHub Actions ([`.github/workflows/release.yml`](.github/workflows/release.yml)) que compila `Clipped.exe` y genera el instalador `ClippedSetup.exe` en la nube cada vez que subes un tag de versión:

```powershell
git add .
git commit -m "Release v1.0.0"
git tag v1.0.0
git push origin main --tags
```
