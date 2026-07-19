// Simplified-to-traditional Chinese conversion for the Lang5 Nexus CN addon.
//
// This mirrors the memory-discovery/text-replacement approach of the original
// cy-sp-howard/lang5 Blish HUD module, but does the actual character conversion
// in plain C++ instead of hand-written shellcode. Lang5ConvertStringInPlace is
// invoked from a register-preserving trampoline installed at the GW2
// CParser::Validate text path (see ModuleMain.cpp); it must be reentrant,
// allocation-free, and never throw.

#pragma once

#include <cstdint>

extern "C" {

// Convert a null-terminated UTF-16 string in place: every simplified-Chinese
// run covered by the loaded dictionaries is rewritten to its traditional form.
// Safe to call with a null pointer or before dictionaries are loaded (no-op).
// Called on the GW2 render/parse thread from the converter trampoline.
void Lang5ConvertStringInPlace(char16_t* str);

} // extern "C"

// Log severity levels passed to Lang5LogFn. These intentionally match Nexus'
// ELogLevel numeric values so ModuleMain can forward them directly.
enum Lang5LogLevel {
    LANG5_LOG_CRITICAL = 1,
    LANG5_LOG_WARNING = 2,
    LANG5_LOG_INFO = 3,
};

using Lang5LogFn = void (*)(int level, const char* message);

// Load conversion dictionaries. The length-preserving base mapping
// (jianfan.json) is embedded in the DLL at build time; files in <dir> are
// optional overrides layered on top (in order, later wins on duplicate inputs):
//   <dir>\jianfan.json   replaces/extends the embedded base mapping
//   <dir>\add.json        optional, may change string length
//   <dir>\user.json       optional user overrides, may change string length
// Returns the number of unique rules loaded. dir and logFn may be null.
int Lang5LoadDictionaries(const char* dir, Lang5LogFn logFn);

// True once at least one rule is loaded and the converter may be enabled.
bool Lang5DictionariesReady();

// Release all loaded rules. Call before reloading or on unload.
void Lang5ClearDictionaries();
