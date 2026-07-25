# Building Lang5 Nexus CN (Developers)

This document is for developers building the addon from source. Players should
read [README.md](README.md) instead.

This is an experimental Nexus addon prototype based on the memory-discovery
approach from [cy-sp-howard/lang5](https://github.com/cy-sp-howard/lang5). It
investigates moving Lang5's simplified/traditional Chinese GW2 UI feature from a
Blish HUD module into a native Nexus addon.

## Current Scope

Implemented:

- Nexus native addon DLL
- toggle keybind registration (switch GW2 to simplified Chinese)
- ImGui options-panel checkboxes (auto-start; traditional on/off)
- persisted settings file (`settings.txt`)
- GW2 main-module memory scan
- `ValidateLanguage(language)` anchor discovery + language setter target
- `ViewAdvanceText` caller hook-point discovery + deferred caller hook
- `CParser::Validate` text-path hook-point discovery
- simplified-to-traditional conversion (dictionary loading + text hook)
- diagnostic logging for current GW2 build offsets

Not implemented:

- Blish HUD module compatibility
- automated release packaging / CI
- in-game editing / reload of the conversion dictionary

## How The Traditional Conversion Works

The conversion hooks GW2's `CParser::Validate` text path and rewrites simplified
Chinese runs to traditional as strings are finalized. Unlike the original (which
injects hand-written shellcode), the conversion logic here is plain C++ in
`src/TextConverter.cpp`; a small register-preserving trampoline installed through
the Nexus-provided MinHook API calls it with the string pointer (`rax`) at
`CParser::Validate ref + 0xAA`, where GW2 executes `mov rbp, rax`.

### Dictionaries

The base mapping `data/jianfan.json` is embedded into the DLL at build time:
`cmake/EmbedFile.cmake` generates `generated/jianfan_embedded.h` in the build
tree, and `Lang5LoadDictionaries` parses it from memory at addon load. Players
therefore only need to copy the DLL. Updating the base dictionary means
rebuilding the DLL.

On top of the embedded base, optional override files are read from the addon
directory at load:

```text
<GW2>/addons/Lang5NexusCn/jianfan.json   (optional, replaces/extends the embedded base mapping)
<GW2>/addons/Lang5NexusCn/add.json        (optional, multi-character / length-changing rules)
<GW2>/addons/Lang5NexusCn/user.json       (optional, your own overrides)
```

Later files override earlier ones on duplicate inputs. Each file is an array of
`{"i":"简","o":"繁"}` objects, where
`i` is the simplified input and `o` the traditional output. `jianfan.json` is
edited from [kfcd/fanjian](https://github.com/kfcd/fanjian) (CC-BY 3.0).

`jianfan.json` is entirely 1:1 in length, so conversion is a safe in-place
rewrite. Entries in `add.json`/`user.json` may make a string longer (e.g.
`全屏` → `全螢幕`); those are written back into the game's string buffer just as
the original Lang5 does, which relies on GW2's parse buffer having headroom. Keep
custom rules conservative.

Because conversion runs left-to-right and rewrites each character as it is
scanned, a multi-character rule only fires when its non-final characters are not
themselves single-character rules. For example `全屏` → `全螢幕` works, but
`服务器` → `伺服器` does not, because `务` → `務` is applied first. This matches
the original Lang5 behavior.

## Build Modes

The CMake options are intentionally explicit:

```text
LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
LANG5_ENABLE_DEFERRED_CALLER_HOOK
LANG5_ENABLE_DIRECT_SETTER_CALL
LANG5_ENABLE_MEMORY_WRITE
LANG5_ENABLE_TEXT_CONVERSION
```

Default builds are diagnostic-only: they resolve addresses and log them but do
not mutate game memory. The known-good, fully functional player build is:

```text
LANG5_ENABLE_UNSAFE_LANGUAGE_CALL=ON
LANG5_ENABLE_DEFERRED_CALLER_HOOK=ON
LANG5_ENABLE_TEXT_CONVERSION=ON
LANG5_ENABLE_DIRECT_SETTER_CALL=OFF
LANG5_ENABLE_MEMORY_WRITE=OFF
```

Do not use the direct setter or memory-write modes: both crashed during
investigation (the direct global language write crashed with
`CtlDropList.cpp(733) Assertion: entry`).

## Build On Windows

From a Visual Studio x64 developer prompt:

```bat
cmake -S . -B build -A x64 ^
  -DLANG5_ENABLE_UNSAFE_LANGUAGE_CALL=ON ^
  -DLANG5_ENABLE_DEFERRED_CALLER_HOOK=ON ^
  -DLANG5_ENABLE_TEXT_CONVERSION=ON
cmake --build build --config Release
```

The DLL will be under `build\bin\Release\Lang5NexusCn.dll`.

## Cross Build On macOS

```sh
brew install mingw-w64

cmake -S . -B build-mingw-player \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
  -DLANG5_ENABLE_UNSAFE_LANGUAGE_CALL=ON \
  -DLANG5_ENABLE_DEFERRED_CALLER_HOOK=ON \
  -DLANG5_ENABLE_TEXT_CONVERSION=ON
cmake --build build-mingw-player --config Release
```

The MinGW build is linked statically to avoid missing runtime DLLs such as
`libwinpthread-1.dll`.

## ImGui Vendoring

The options panel is drawn with Dear ImGui **1.80**, vendored verbatim from the
Nexus source tree (`thirdparty/imgui/`) so the struct layouts match the
Nexus-hosted ImGui context exactly. The addon adopts the context via
`ImGui::SetCurrentContext` / `ImGui::SetAllocatorFunctions` in its load function.
If a future Nexus release changes its bundled ImGui version, re-vendor the
matching sources from `RaidcoreGG/Nexus/thirdparty/imgui` before rebuilding.

## Testing Order

1. Start with only Nexus and this addon.
2. Disable ArcDPS, ReShade, Blish HUD Lang5, and other memory/language addons.
3. Build a diagnostic DLL first (no flags) and confirm it loads.
4. Check Nexus logs for `Lang5NexusCn`.
5. Only after diagnostic loading is stable, test the mutating build.
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
CParser::Validate anchor RVA
Text converter hook point RVA
Loaded N conversion rules in M buckets.
Created text converter hook.
Enabled text converter hook.
```

## Implementation Notes

The original Lang5 module does not call GW2's language setter directly from its
Blish HUD keybind path. It installs a caller trampoline near `ViewAdvanceText`,
queues a function pointer and argument, and lets GW2 execute that call from a
game-owned path. That distinction matters:

- direct setter call crashed during Nexus testing
- direct global language value write crashed with `CtlDropList.cpp(733) Assertion: entry`
- deferred caller hook loaded and queued successfully in Nexus testing

The Nexus prototype therefore keeps the dangerous direct paths behind separate
compile-time flags and uses the deferred caller hook for the current experiment.

## Attribution

Based on [cy-sp-howard/lang5](https://github.com/cy-sp-howard/lang5), a Blish HUD
module that contains the memory-discovery and deferred-call strategy this Nexus
prototype reproduces. The `jianfan.json` mapping is edited from
[kfcd/fanjian](https://github.com/kfcd/fanjian) (CC-BY 3.0).

`include/Nexus.h` is copied from the Nexus API and is covered by the bundled
`NEXUS_API_LICENSE` file.
