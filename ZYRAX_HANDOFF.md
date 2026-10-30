# Zyrax — Handoff para continuar en otra sesión

## Regla de oro

- Repo activo: `/home/nahuel/Zyrax`
- **NO tocar** `/home/nahuel/Proyectos/Zyrax`: es una copia vieja con cambios sin
  commitear y la capa Vulkan rota (`ffx_fsr1.h:40: syntax error`).
- No hacer `git commit`, `git push` ni `amend` salvo pedido explícito.
- Hacer **una fase por sesión**. Al terminar una fase, actualizar este archivo y
  dejar el repo compilando.
- No simular features. Si algo no está implementado, decirlo y deshabilitarlo en
  la UI.

## Estado al crear este handoff

- HEAD: `d56c105 style: agregar icono oficial de la app con el perrito`
- `git status`: limpio salvo `ZYRAX_HANDOFF.md` (untracked). Fase 1 aplicada en `vk-layer/src/layer.cpp` (sin commitear).
- `bun run build`: OK.
- `cargo test --manifest-path src-tauri/Cargo.toml`: OK (2 tests).
- `bun run layer:build`: OK.
- `cargo clippy --manifest-path src-tauri/Cargo.toml --all-targets -- -D warnings`:
  **FALLA** con 3 errores (ver Fase 5).

## Progreso por fase

- [x] Fase 1: HUD y ajustes del overlay (PushConstants + `ui2` con `static_assert` 112 bytes,
  `ControlState` con `overlayFps`/`overlayScale`/`overlayPos` (fps y scale default `true`),
  parseo de `overlay_fps`/`overlay_scale`/`overlay_pos` en el control file,
  helpers `overlayShowFps()`/`overlayShowScale()`/`overlayPosition()` que respetan
  `ZYRAX_FSR_OVERLAY_FPS`/`ZYRAX_FSR_OVERLAY_SCALE`/`ZYRAX_FSR_OVERLAY_POS`,
  `processFrame()` escribe `flags` en `ui[3]` y `ui2` con extent; sin recreate de pipeline).
  Verificado: `layer:build`, `build`, `cargo test` OK.
- [x] Fase 2: Features no implementadas (DLSS, Windows DXGI, frame generation).
  - UI (`src/main.ts`): DLSS eliminado del selector de upscalers (solo off/fsr); frame generation mostrado como no disponible y controles deshabilitados; `attachToGame` siempre envía `frameGeneration: false`.
  - Backend (`src-tauri/src/runtime.rs`): si llega `frame_generation == true`, se deja en false y se advierte "Frame generation is not implemented; attached with FSR only."
  - Backend (`src-tauri/src/runtime.rs`): en plataformas no Linux, `attach_to_game` devuelve temprano "The DXGI hook is not implemented; the Zyrax Vulkan layer is Linux-only." antes de comprobar si la capa está instalada.
  - Backend (`src-tauri/src/layer.rs`): `write_layer_control` también devuelve ese mensaje en plataformas no Linux como segunda barrera.
  - `README.md`: título, features, diseño y roadmap corregidos; DLSS/DXGI/frame generation marcados como no implementados, sin simular.
  Verificado: `layer:build`, `build`, `cargo test` OK. El target Windows no está instalado en esta máquina, así que el bloque `#[cfg(not(target_os = "linux"))]` no se compiló localmente; revisar en CI.
- [x] Fase 3: Perfiles por juego.
  - `src/lib.ts` (nuevo): `normalizeExe`, `profileMatchesGame` y `escapeHtml`.
  - `src/main.ts`: al aplicar un perfil guarda `gameExe` normalizado; al escanear auto-selecciona el perfil que coincide y el juego, sin auto-attach; dashboard y perfiles muestran `BOUND TO: …`; botón `CLEAR GAME BINDING`.
  Verificado: `layer:build`, `build`, `cargo test` OK. Sin tests TS todavía (Fase 5).
- [x] Fase 4: XSS / CSP.
  - `src/lib.ts`: agregado `escapeAttr` (delegates a `escapeHtml`).
  - `src/main.ts`: todos los valores dinámicos en `innerHTML` usan `escapeHtml` (incluidos `runtime.message`, `runtime.upscaler`, `active.name`/`upscaler`, `g.name/source/launcher`, GPU, `layer.message`); atributos dinámicos (`p.id`, `hotkey`) usan `escapeAttr`.
  - `src-tauri/tauri.conf.json`: `withGlobalTauri: false`; CSP restrictiva con `default-src 'self'`, `script-src 'self'`, `object-src 'none'`, `frame-ancestors 'none'`, etc.; `connect-src` incluye `ipc:` y `http://ipc.localhost` para que funcione el IPC de Tauri; `devCsp` permite Vite HMR en `localhost:5173`.
  - `src/lib.test.ts` (nuevo): 18 tests de `escapeHtml`, `escapeAttr`, `normalizeExe`, `profileMatchesGame`.
  - `src/bun-test.d.ts` (nuevo): declaraciones de tipos para `bun:test` para que `tsc` compile.
  - `package.json`: agregado script `test: "bun test"`.
  Verificado: `layer:build`, `build`, `cargo test`, `bun test` OK; `bun run tauri info` parsea la CSP e incluye `ipc:`/`http://ipc.localhost`.
- [ ] Fase 5: Clippy, tests, scripts y CI.
- [ ] Fase 6: Stats atómicos, GPU honesta, CPU fiable.
- [ ] Fase 7: Limpieza.

## Alcance

Implementar las correcciones reales y dejar la app honesta, testeada y con CI
verde.

**Fuera de alcance / no simular:**
- DLSS.
- Hook DXGI / soporte real de Windows.
- Frame generation real.

Para esas tres: deshabilitarlas o marcarlas como no implementadas en UI/README.

---

## Hallazgos y archivos clave

### 1. HUD roto (Fase 1)

- `vk-layer/src/layer.cpp:109` define `PushConstants` con 6 `uvec4`
  (`con0..con3`, `rcas`, `ui`), pero el shader declara 7 (`ui` + `ui2`).
- `vk-layer/shaders/fsr_rcas.frag:18-19` declara `uvec4 ui; uvec4 ui2;`.
- `vk-layer/src/layer.cpp:876` pone `sc->pc.ui[3] = sc->extent.height`, pero el
  shader lee `pc.ui.w` como flags (`fsr_rcas.frag:53`).
- `ui2` nunca se rellena; el shader usa `pc.ui2.x/y` como tamaño
  (`fsr_rcas.frag:80`), así que el HUD cree que la pantalla es 0x0.
- El layer escribe `overlay_fps`, `overlay_scale` y `overlay_pos` en el control
  (`src-tauri/src/layer.rs:214`), pero `refreshControl()` solo parsea
  `enabled`, `pid`, `scale`, `sharpness`, `overlay` y `frame_gen`
  (`vk-layer/src/layer.cpp:313-329`). Los ajustes de elementos/posición se
  ignoran.

**Fix esperado:**
- Añadir `uint32_t ui2[4];` a `PushConstants` y `static_assert` de 112 bytes.
- Añadir a `ControlState`: `overlayFps`, `overlayScale`, `overlayPos`.
- Parsear `overlay_fps`, `overlay_scale`, `overlay_pos` (clamp 0..3).
- Incluirlos en `anyChanged`, no en `pipelineChanged` (sin recreate).
- Helpers `overlayShowFps()`, `overlayShowScale()`, `overlayPosition()`.
- En `processFrame()`:
  - `flags = (fps?1:0) | (scale?2:0) | ((pos & 3) << 2)`
  - `ui[0] = overlayEnabled() ? 1 : 0`
  - `ui[1] = fps`
  - `ui[2] = scale * 100`
  - `ui[3] = flags`
  - `ui2[0] = extent.width`
  - `ui2[1] = extent.height`
  - `ui2[2] = ui2[3] = 0`
- Mapeo de posición: 0 bottom-left, 1 top-left, 2 bottom-right, 3 top-right.
  Ya coincide con el writer de Rust.

### 2. Features no implementadas (Fase 2)

- DLSS: `src-tauri/src/runtime.rs:84` rechaza todo lo que no sea FSR.
- Windows: `src-tauri/src/layer.rs:186` devuelve error porque la capa es Linux.
- Frame generation: `frame_gen` se parsea en `layer.cpp:329` pero nunca se usa;
  `frameGenMultiplier` solo se guarda (`src-tauri/src/profiles.rs:16`,
  `src/main.ts:843`).

**Fix esperado:**
- UI: DLSS y frame generation marcados como no disponibles; no permitir
  seleccionarlos ni enviar `frameGeneration: true`.
- Backend: si llega `frame_generation == true`, dejarlo en false y avisar
  “Frame generation is not implemented; attached with FSR only.”
- Windows: mensaje claro de que el hook DXGI no está implementado.
- README/roadmap: corregir promesas y marcar solo lo que funciona.

### 3. Perfiles por juego (Fase 3)

- `src-tauri/src/profiles.rs:11` tiene `game_exe`, pero nunca se asigna ni se usa.
- `src/main.ts:884` crea perfiles con `gameExe: null`.
- No hay matching entre juego detectado y perfil.

**Fix esperado:**
- Helpers puros en `src/lib.ts`: `normalizeExe`, `profileMatchesGame`.
- Al aplicar un perfil a un juego, guardar `gameExe`.
- Al escanear, seleccionar automáticamente el perfil que coincida (sin
  auto-attach).
- Mostrar “BOUND TO: …” y botón “CLEAR GAME BINDING”.

### 4. XSS / CSP (Fase 4)

- `src/main.ts:181,221,505,680` insertan valores dinámicos con `innerHTML` sin
  escapar.
- `src-tauri/tauri.conf.json:13` tiene `withGlobalTauri: true`.
- `src-tauri/tauri.conf.json:27` tiene `csp: null`.

**Fix esperado:**
- `escapeHtml` ya existe en `src/lib.ts`; agregar `escapeAttr` y usar ambos en todo valor dinámico.
- `withGlobalTauri: false`.
- CSP restrictiva; `devCsp` para Vite HMR si hace falta.
- Tests de escape en `src/lib.test.ts`.

### 5. Clippy, tests, scripts y CI (Fase 5)

Errores actuales de clippy:
- `src-tauri/src/layer.rs:174`: demasiados argumentos en `write_layer_control`.
- `src-tauri/src/runtime.rs:71`: demasiados argumentos en `attach_to_game`.
- `src-tauri/src/processes.rs:70`: usar `sort_by_key` con `Reverse`.

**Fix esperado:**
- Struct `LayerControl` para el control file.
- Struct `AttachRequest` con `#[serde(rename_all = "camelCase")]`; actualizar
  `src/api.ts` y `src/main.ts`.
- `processes.sort_by_key(|p| std::cmp::Reverse(p.memory_mb))`.
- Tests Rust para heurísticas de `games.rs`, validaciones y control file.
- `src/lib.test.ts` con `bun:test`.
- Scripts en `package.json`: `typecheck`, `test`, `check`.
- CI Linux/Windows: `bun test`, `cargo clippy --all-targets -- -D warnings`,
  `cargo fmt --check`, `cargo test`.

### 6. Stats atómicos, GPU honesta, CPU fiable (Fase 6)

- `vk-layer/src/layer.cpp:929` escribe stats directo con `fopen(..., "w")`.
  **Fix:** escribir a `<path>.tmp.<pid>` y `rename`.
- `src-tauri/src/gpu.rs:27` anuncia DirectX 11/12 en Linux y DLSS/FSR solo por
  fabricante. **Fix:** `apis_for_platform(vendor, is_linux)`:
  - Linux: `Vulkan`, `OpenGL` (opcional `FSR (Vulkan layer)`).
  - Windows: `Vulkan`, `OpenGL`, `DirectX 11`, `DirectX 12`.
- `src-tauri/src/processes.rs:78` crea un `System` nuevo y refresca una vez;
  `cpu_usage` suele ser 0. **Fix:** `ProcessSampler` con `Mutex<System>` en
  estado Tauri y refrescos con intervalo.

### 7. Limpieza (Fase 7)

- Eliminar `src/vulkan/layer.cpp` (0 bytes, duplicado obsoleto).
- Cambiar textos “SYSTEM-WIDE” por “FOR THIS USER” (instala en
  `~/.local/share/vulkan/implicit_layer.d`, ver `src-tauri/src/layer.rs:108`).
- Revisar referencias viejas y TODOs.
- No tocar la copia vieja.

---

## Orden de trabajo y verificación

Después de cada fase:

```bash
cd /home/nahuel/Zyrax
git status --short
git diff --stat
bun run layer:build
bun run build
cargo test --manifest-path src-tauri/Cargo.toml
```

Al final de Fase 5 en adelante:

```bash
bun run typecheck
bun test
cargo fmt --manifest-path src-tauri/Cargo.toml -- --check
cargo clippy --manifest-path src-tauri/Cargo.toml --all-targets -- -D warnings
```

Prueba manual del HUD (si hay entorno Vulkan):

```bash
cd /home/nahuel/Zyrax
bun run layer:build
cd vk-layer && ./install.sh
ZYRAX_FSR_ENABLE=1 ZYRAX_FSR_OVERLAY=1 ZYRAX_FSR_DEBUG=1 vkcube
```

Control file de prueba:

```ini
version=1
enabled=1
pid=<pid de vkcube>
scale=1.5
sharpness=0.5
overlay=1
overlay_fps=1
overlay_scale=1
overlay_pos=3
frame_gen=0
```

Verificar HUD arriba a la derecha y cambios en caliente de `overlay_*`.

---

## Mensaje para la próxima sesión

Fase 4 verificada y completa: `layer:build`, `build`, `cargo test` y `bun test` OK.

Copiar y pegar:

```text
Estoy en /home/nahuel/Zyrax. Leé ZYRAX_HANDOFF.md completo, revisá
`git status --short` y `git diff`, y continuá con la Fase 5 (Clippy,
tests, scripts y CI: arreglar los 3 errores de clippy en
layer.rs:174, runtime.rs:71 y processes.rs:70 con structs
LayerControl/AttachRequest, agregar scripts typecheck/test/check en
package.json, CI Linux/Windows). No toques
/home/nahuel/Proyectos/Zyrax. No hagas commit. Después de cada cambio
executá `bun run layer:build`, `bun run build` y `cargo test`, y pegá
la salida real. Cuando termine la Fase 5, actualizá este archivo y
dejá el mensaje para la Fase 6.
```

Cuando termines una fase, actualizá este archivo: marcá lo hecho, lo pendiente y
la próxima fase. Así cada sesión arranca sin arrastrar contexto.
