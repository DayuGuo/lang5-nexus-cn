# Lang5 Nexus CN

Experimental Nexus addon prototype based on the memory-discovery approach from [cy-sp-howard/lang5](https://github.com/cy-sp-howard/lang5).

The goal is to investigate whether Lang5's simplified Chinese Guild Wars 2 UI selection can be moved from a Blish HUD module into a native Nexus addon.

## Current Scope

Implemented:

- Nexus native addon DLL
- toggle keybind registration
- ImGui options-panel checkbox for the auto-start preference
- persisted settings file (`settings.txt`)
- optional auto-enable Chinese on launch
- GW2 main-module memory scan
- `ValidateLanguage(language)` anchor discovery
- language setter target discovery
- `ViewAdvanceText` caller hook-point discovery
- diagnostic logging for current GW2 build offsets
- experimental deferred caller hook matching the original Lang5 call strategy more closely

Not implemented:

- simplified-to-traditional conversion
- text replacement hook
- dictionary JSON loading
- Blish HUD module compatibility
- stable release packaging

## Risk

This is a memory-access addon. It scans and patches the GW2 process. Use only for local research.

This project is not approved by ArenaNet, Raidcore, Nexus, Blish HUD, or the original Lang5 author. It may crash after any GW2 update.

The addon is marked volatile and launch-only. If Nexus disables volatile addons after a game update, leave it disabled until offsets are revalidated.

## Keybinds And Settings

One keybind is registered (default, rebindable in Nexus):

```text
ALT+SHIFT+C   Toggle Chinese UI (queued; only mutates in the experimental build)
```

The "auto-enable Chinese on launch" preference is exposed as a checkbox in the addon's
Nexus options panel (open Nexus, go to the addon's options). Toggling it saves to:

```text
<GW2>/addons/Lang5NexusCn/settings.txt
```

Format is a single line:

```text
auto_enable_chinese=1
```

When the preference is on and the experimental (unsafe) build is loaded, the addon
waits a short delay after load, then queues the Chinese toggle through the deferred
caller hook automatically. In the diagnostic build the preference is still saved, but
no mutation happens — it takes effect once you load the experimental build.

### ImGui options panel

The checkbox is drawn with Dear ImGui **1.80**, vendored verbatim from the Nexus
source tree (`thirdparty/imgui/`) so the struct layouts match the Nexus-hosted ImGui
context exactly. The addon adopts the context via `ImGui::SetCurrentContext` and
`ImGui::SetAllocatorFunctions` in its load function. If a future Nexus release changes
its bundled ImGui version, re-vendor the matching sources from
`RaidcoreGG/Nexus/thirdparty/imgui` before rebuilding.

## Build Modes

The CMake options are intentionally explicit:

```text
LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
LANG5_ENABLE_DEFERRED_CALLER_HOOK
LANG5_ENABLE_DIRECT_SETTER_CALL
LANG5_ENABLE_MEMORY_WRITE
```

Default builds are diagnostic-only. They resolve addresses and log them, but do not mutate game memory.

The only currently useful experimental mutation path is:

```text
LANG5_ENABLE_UNSAFE_LANGUAGE_CALL=ON
LANG5_ENABLE_DEFERRED_CALLER_HOOK=ON
LANG5_ENABLE_DIRECT_SETTER_CALL=OFF
LANG5_ENABLE_MEMORY_WRITE=OFF
```

Do not use the direct setter or memory-write modes for normal testing. Both crashed during investigation.

## Build On Windows

From a Visual Studio x64 developer prompt:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The DLL will be under:

```text
build\bin\Release\Lang5NexusCn.dll
```

## Cross Build On macOS

Install the toolchain:

```sh
brew install mingw-w64
```

Build the diagnostic DLL:

```sh
cmake -S . -B build-mingw-gcc -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-mingw-gcc --config Release
```

Build the deferred-hook experiment:

```sh
cmake -S . -B build-mingw-deferred \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
  -DLANG5_ENABLE_UNSAFE_LANGUAGE_CALL=ON \
  -DLANG5_ENABLE_DEFERRED_CALLER_HOOK=ON \
  -DLANG5_ENABLE_DIRECT_SETTER_CALL=OFF \
  -DLANG5_ENABLE_MEMORY_WRITE=OFF
cmake --build build-mingw-deferred --config Release
```

The MinGW build is linked statically to avoid missing runtime DLLs such as `libwinpthread-1.dll`.

## Testing Order

1. Start with only Nexus and this addon.
2. Disable ArcDPS, ReShade, Blish HUD Lang5, and other memory/language addons.
3. Test the diagnostic DLL first: `dist/Lang5NexusCn.dll`.
4. Check Nexus logs for `Lang5NexusCn`.
5. Only after diagnostic loading is stable, test `build-mingw-deferred/bin/Lang5NexusCn.dll`.
6. If GW2 crashes, remove the DLL from the Nexus addon folder before restarting.

Useful diagnostic log lines:

```text
ValidateLanguage anchor RVA
Language setter target RVA
ViewAdvanceText caller hook point RVA
ViewAdvanceText original call target RVA
Installed deferred caller hook.
Queued language setter through deferred caller hook. language=5
Deferred language setter executed.
```

## Implementation Notes

The original Lang5 module does not call GW2's language setter directly from its Blish HUD keybind path. It installs a caller trampoline near `ViewAdvanceText`, queues a function pointer and argument, and lets GW2 execute that call from a game-owned path.

That distinction matters:

- direct setter call crashed during Nexus testing
- direct global language value write crashed with `CtlDropList.cpp(733) Assertion: entry`
- deferred caller hook loaded and queued successfully in Nexus testing

The Nexus prototype therefore keeps the dangerous direct paths behind separate compile-time flags and uses the deferred caller hook for the current experiment.

## Attribution

This prototype is based on [cy-sp-howard/lang5](https://github.com/cy-sp-howard/lang5). The original project is a Blish HUD module and contains the memory-discovery and deferred-call strategy that this Nexus prototype is trying to reproduce.

`include/Nexus.h` is copied from the Nexus API and is covered by the bundled `NEXUS_API_LICENSE` file.
