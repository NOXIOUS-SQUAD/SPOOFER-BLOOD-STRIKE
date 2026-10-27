# SPOOFER BS — Hardware Identity Suite

Mega-spoofer para BloodStrike (NetEase Messiah Engine) con **panel profesional en C++ (ImGui + DX11)**, construido a partir de la evidencia del decomp (`C:\Users\anony\Desktop\decomp`).

**Diseño:** sin logs en disco · sin cache · borrado con zero-fill · temporales purgados · perfil reciente guardado en memoria.

---

## 1. Qué hace (blueprint de 5 capas)

Todas las capas se aplican a la vez desde la página **SPOOFER**, con vista previa antes de confirmar y backup automático de los valores originales.

| Capa | Vector neutralizado | Mecanismo |
|---|---|---|
| **L1** | `NEProtect.sys` (driver Ring-0) | Servicio `Start=4` (disabled) + lanzamiento con `__COMPAT_LAYER=RUNASINVOKER` → Medium IL, `CreateServiceW` falla con error 5 y el driver nunca carga |
| **L2** | Caches SQLite que sobreviven a reinstalar | Zero-fill + delete de `%LOCALAPPDATA%\NetEase`, `%APPDATA%\Netease\Mpay` (336-64-mpay.db, clientlog.db), `LocalLow\NetEase`, `%TEMP%\NetEase`, `%LOCALAPPDATA%\BloodStrike` (orbit.config.cache) |
| **L3** | GUIDs de `libenvsdk.dll` | Rota `MachineGuid`, `InstallDate`, `ProductId`, `HwProfileGuid` en registro |
| **L4** | Volume serial C: | Escritura raw al boot sector NTFS (offset 0x48) — equivalente a Volumeid.exe |
| **L5** | MAC del adaptador | `NetworkAddress` en `Control\Class\{4d36e972...}` + reinicio del adaptador vía netsh |

**Extras incluidos:**

- **Sinkhole de telemetría** — los 5 dominios `easebar.com` a `0.0.0.0` en `hosts` (bloque marcado con comentarios, reversible).
- **Limpieza de rastros** — `HKCU\Software\Netease`, claves `Tracing\BloodStrike_RAS*`, entradas MuiCache.
- **UDID preview** — muestra el `MD5(MAC+CPU_ID+DiskSerial+MachineGuid)` actual y cómo queda tras el spoof.
- **Restore** — revierte todo desde el perfil backup (registro, MAC, volume serial, servicio, hosts).

> El vector **SMBIOS** (`GetSystemFirmwareTable('RSMB')`) se muestra en el Dashboard pero **no es spoofable client-side** — se neutraliza impidiendo que `NEProtect.sys` lo lea (L1), no falseándolo.

## 2. Estructura

```
SPOOFER BS/
├── build.bat                  # compila todo (MSVC vcvars64 + rc del manifiesto)
├── SPOOFER_BS.manifest        # manifiesto: requireAdministrator + DPI
├── app.rc                     # recursos: ICON + RCDATA (logo)
├── manifest.rc                # RT_MANIFEST (solo build.bat; VS usa UACExecutionLevel)
├── app.ico                    # icono del exe (16–256 px)
├── logo_panel.png             # logo del panel (512 px, fondo recortado)
├── bin/
│   └── SPOOFER_BS.exe         # el panel
├── panel/
│   ├── imgui/                 # ImGui 1.92.4 + backends DX11/Win32 (de junior/)
│   └── src/
│       ├── main.cpp           # vista mínima + ventana AVANZADO (segundo HWND/contexto)
│       ├── neuron.h           # fondo neuronal animado (independiente, BackgroundDrawList)
│       ├── identity.h/.cpp    # vectores HWID, lectura, generadores, perfiles, MD5
│       ├── spoofer.h/.cpp     # motor de 5 capas + launcher + restore
│       ├── cleaner.h/.cpp     # temporales, rastros propios/juego, zero-fill
│       └── tinyjson.h         # JSON mínimo sin dependencias (perfiles)
├── python/
│   └── spoofer_reference.py   # port de referencia del motor (auditable, CLI)
├── profiles/                  # backups automáticos (JSON)
└── logs/                      # vacío a propósito: no se escribe nada
```

## 3. Compilar

### Opción A — Visual Studio 2022/2026 (Team Explorer)

Abre **`SPOOFER_BS.slnx`** (formato nuevo; en VS 2022 ≤ 17.12 activa antes *Herramientas → Opciones → Características en versión preliminar → "Usar el formato de archivo de solución .slnx"*) y pulsa **Compilar → Generar solución** (`Ctrl+Mayús+B`). Configuraciones disponibles: `Release|x64` y `Debug|x64`.

> Si tu VS 2022 no reconoce `.slnx`, abre **`SPOOFER_BS.sln`** (formato clásico equivalente, incluido como alternativa).

- Toolset `v143`, SDK `10.0.26100.0`, C++17, subsistema Windows (`WinMain`).
- Salida: `bin\SPOOFER_BS.exe` (igual que build.bat). Archivos intermedios: `obj\$(Configuration)\`.

### Opción B — script (sin IDE)

Requiere Visual Studio con el toolset x64 (probado con VS 2026 Community 18):

```bat
build.bat
```

Notas del build:
- El bat fija a mano las rutas del Windows SDK (`10.0.26100.0`) porque en algunos entornos `reg.exe` está bloqueado dentro de `vcvarsall.bat` y la detección del SDK falla.
- El manifiesto del script se compila con `rc.exe` (`manifest.rc` → `obj\manifest.res`) y se enlaza con `/MANIFEST:NO`. En **Visual Studio** no se usa ese .rc: el proyecto genera el manifiesto con `<UACExecutionLevel>RequireAdministrator</UACExecutionLevel>`. Mezclar ambos produce `recurso duplicado MANIFEST 1` y un fallo de CVTRES.
- `app.rc` (compartido por las dos vías) embebe el icono (`app.ico`, 16–256 px) y el logo del panel (`logo_panel.png`, RCDATA); el logo se decodifica en runtime con WIC y se sube como textura DX11.
- `identity.cpp` incluye `winsock2.h` **antes** de `iphlpapi.h`: los SDK nuevos ocultan `GetAdaptersAddresses` tras `#ifdef _WINSOCK2API_`.
- El SDK 26100 renombró `DXGI_USAGE_RENDER_TARGET_VIEW` → `DXGI_USAGE_RENDER_TARGET_OUTPUT`; `main.cpp` incluye un shim de compatibilidad.

## 4. Uso

La interfaz es mínima: solo se ven los 7 vectores (Nombre, MachineGuid, HwProfileGuid, ProductId, InstallDate, MAC, Volume serial), **GENERAR OTRO PERFIL SINTÉTICO** y **SPOOFEAR**, sobre fondo neuronal lima/negro. Sin log ni textos en pantalla: el estado se reduce a una línea que **solo aparece al spofear** (o si el panel detecta un problema).

1. **El exe se eleva solo** (manifiesto `requireAdministrator`); si por lo que sea no lo está, se relanza con `runas`.
2. **SPOOFEAR** → aplica las 5 capas (si no hay perfil objetivo, genera uno al vuelo con nombre aleatorio) y, al terminar, **lanza BloodStrike en Medium IL** con `__COMPAT_LAYER=RUNASINVOKER`. El botón queda en `SPOOFEANDO...` hasta que termina el worker.
   - La ruta se autodetecta: `Engine\Binaries\Win64\BloodStrike.exe` dentro de `bloodstrike` en cualquier disco fijo, `SteamLibrary\steamapps\common\BLOODSTRIKE`, `Program Files`, etc.; si solo hay `launcher.exe`, usa ese. Último recurso: barrido acotado (profundidad 4, 4000 carpetas por disco) buscando una carpeta con "bloodstrike" en el nombre.
   - Si el proceso `BloodStrike.exe` ya está vivo no se relanza. El lanzamiento se puede desactivar en AVANZADO → *Lanzar juego (RUNASINVOKER)*.
3. **GENERAR OTRO PERFIL SINTÉTICO** → rota los valores mostrados en la tarjeta sin tocar el sistema y **cambia el nombre del perfil por uno aleatorio distinto** en cada pulsación.
4. **≡ (arriba a la derecha)** → abre la ventana flotante **AVANZADO** (segundo HWND, no anidada): pestañas Panel (vectores), Spofear (capas + restaurar último backup) y Limpieza (basura y rastros).
5. **DISCORD (abajo, azul #5865F2)** → abre `https://discord.gg/CE8xPnpV6a` en el navegador por defecto (`ShellExecuteW "open"`). El icono de burbuja se dibuja con `ImDrawList`, sin assets extra.

La **consola vive solo en memoria** dentro de AVANZADO: no se escribe ningún archivo de log. Los únicos archivos en disco son los backups JSON automáticos.

## 5. Limitaciones conocidas

- **Baneo de cuenta:** imposible client-side (vive en el backend de NetEase). Esto cubre el **HWID ban**.
- Volume serial y hosts requieren C: desocupado y admin.
- Algunos adaptadores aplican la MAC al reiniciar Windows (el panel lo indica).
- Tras cambiar GUIDs de registro, `slui`/activación de Windows puede pedir reactivación.

## 6. Créditos

Evidencia: `decomp/16_spoofer/*` (feasibility, fingerprint, registry_keys, driver_blocking), `decomp/07_system_footprint/evidence/*`. Backend ImGui reutilizado de `T2M - FREE SOURCE/junior/`.
