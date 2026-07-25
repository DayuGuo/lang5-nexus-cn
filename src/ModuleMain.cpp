#include "Nexus.h"
#include "TextConverter.h"
#include "imgui.h"

#include <Psapi.h>
#include <Windows.h>

#include <cstdio>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifndef LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
#define LANG5_ENABLE_UNSAFE_LANGUAGE_CALL 0
#endif

#ifndef LANG5_ENABLE_DIRECT_SETTER_CALL
#define LANG5_ENABLE_DIRECT_SETTER_CALL 0
#endif

#ifndef LANG5_ENABLE_MEMORY_WRITE
#define LANG5_ENABLE_MEMORY_WRITE 0
#endif

#ifndef LANG5_ENABLE_DEFERRED_CALLER_HOOK
#define LANG5_ENABLE_DEFERRED_CALLER_HOOK 0
#endif

#ifndef LANG5_ENABLE_TEXT_CONVERSION
#define LANG5_ENABLE_TEXT_CONVERSION 0
#endif

namespace {

constexpr const char* kChannel = "Lang5NexusCn";
constexpr const char* kToggleKeybind = "KB_LANG5_NEXUS_CN_TOGGLE";
constexpr const char* kValidateLanguageAnchor = "ValidateLanguage(language)";
constexpr const char* kViewAdvanceTextAnchor = "ViewAdvanceText";
constexpr const char* kCParserValidateAnchor =
    "CParser::Validate(sourceBuffer.Ptr(), sourceBuffer.Term(), true ) == sourceBuffer.Term()";
constexpr const char* kAddonDirName = "Lang5NexusCn";
constexpr const char* kSettingsFileName = "settings.txt";
constexpr const char* kAutoEnableKey = "auto_enable_chinese";
constexpr const char* kEnableTraditionalKey = "enable_traditional";
constexpr uint32_t kChineseLanguageId = 5;

// Instruction expected at the text-converter hook point (mov rbp, rax). Matches
// the original lang5 _textConverterOriginBytes; used to detect GW2 layout drift.
constexpr uint8_t kTextConverterExpectedBytes[3] = {0x48, 0x8B, 0xE8};

// Number of PostRender frames to wait after load before auto-applying Chinese,
// so the game and the deferred caller path are fully live first.
constexpr int kAutoApplyDelayFrames = 120;

AddonAPI_t* g_api = nullptr;
bool g_loaded = false;
bool g_chineseEnabled = false;
bool g_hasPendingApply = false;
bool g_pendingEnable = false;
bool g_waitingForDeferredCall = false;
bool g_autoEnableChinese = false;
bool g_traditionalEnabled = false;
uint32_t g_originalLanguage = 0;
uint32_t* g_originalLanguagePtr = nullptr;

// Text-converter hook state. The hook point is resolved (and logged) in every
// build; installation only happens in the experimental text-conversion build.
uint8_t* g_textConverterHookPoint = nullptr;
#if LANG5_ENABLE_TEXT_CONVERSION
void* g_converterStub = nullptr;
void* g_converterTrampoline = nullptr;
bool g_converterHookCreated = false;
bool g_converterEnabled = false;
#endif

#if LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
bool g_autoApplyScheduled = false;
int g_autoApplyDelayFrames = 0;
#endif

using LanguageSetterFn = void(__fastcall*)(uint32_t languageId);
LanguageSetterFn g_languageSetter = nullptr;
uint8_t* g_viewAdvanceTextHookPoint = nullptr;
uint8_t* g_viewAdvanceTextOriginalCallTarget = nullptr;

#if LANG5_ENABLE_DEFERRED_CALLER_HOOK
struct PendingCallBuffer {
    uintptr_t Function;
    uint64_t Arg0;
    uint64_t CompletedCount;
};

PendingCallBuffer g_pendingCall{};
void* g_callerCodeCave = nullptr;
std::array<uint8_t, 5> g_callerHookBackup{};
bool g_callerHookInstalled = false;
#endif

bool IsMemoryRangeUsable(void* ptr, size_t size, bool executable);
bool IsReadableAddress(void* ptr, size_t size);
#if LANG5_ENABLE_MEMORY_WRITE
bool WriteLanguageValue(uint32_t languageId);
#endif

void Log(ELogLevel level, const char* message) {
    if (g_api && g_api->Log) {
        g_api->Log(level, kChannel, message);
    }
}

void Alert(const char* message) {
    if (g_api && g_api->GUI_SendAlert) {
        g_api->GUI_SendAlert(message);
    }
}

void BuildSettingsPath(char* out, size_t outSize) {
    if (out && outSize > 0) {
        out[0] = '\0';
    }

    if (!out || outSize == 0 || !g_api || !g_api->Paths_GetAddonDirectory) {
        return;
    }

    const char* dir = g_api->Paths_GetAddonDirectory(kAddonDirName);
    if (!dir) {
        return;
    }

    CreateDirectoryA(dir, nullptr);
    std::snprintf(out, outSize, "%s\\%s", dir, kSettingsFileName);
}

void LoadSettings() {
    char path[MAX_PATH]{};
    BuildSettingsPath(path, sizeof(path));
    if (path[0] == '\0') {
        return;
    }

    FILE* file = std::fopen(path, "rb");
    if (!file) {
        return;
    }

    char line[128]{};
    while (std::fgets(line, sizeof(line), file)) {
        char* separator = std::strchr(line, '=');
        if (!separator) {
            continue;
        }

        *separator = '\0';
        const char* key = line;
        const char* value = separator + 1;
        if (std::strcmp(key, kAutoEnableKey) == 0) {
            g_autoEnableChinese = std::atoi(value) != 0;
        } else if (std::strcmp(key, kEnableTraditionalKey) == 0) {
            g_traditionalEnabled = std::atoi(value) != 0;
        }
    }

    std::fclose(file);
}

void SaveSettings() {
    char path[MAX_PATH]{};
    BuildSettingsPath(path, sizeof(path));
    if (path[0] == '\0') {
        Log(LOGL_WARNING, "Could not resolve settings path; preference not saved.");
        return;
    }

    FILE* file = std::fopen(path, "wb");
    if (!file) {
        Log(LOGL_WARNING, "Could not open settings file for writing.");
        return;
    }

    std::fprintf(file, "%s=%d\n", kAutoEnableKey, g_autoEnableChinese ? 1 : 0);
    std::fprintf(file, "%s=%d\n", kEnableTraditionalKey, g_traditionalEnabled ? 1 : 0);
    std::fclose(file);
}

void LogAddress(ELogLevel level, const char* label, const void* ptr) {
    char message[160]{};
    std::snprintf(message, sizeof(message), "%s: 0x%p", label, ptr);
    Log(level, message);
}

void LogModuleOffset(ELogLevel level, const char* label, const uint8_t* ptr, const uint8_t* base) {
    char message[160]{};
    uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t start = reinterpret_cast<uintptr_t>(base);
    if (value < start) {
        std::snprintf(message, sizeof(message), "%s RVA: outside module", label);
        Log(level, message);
        return;
    }

    uintptr_t offset = value - start;
    std::snprintf(message, sizeof(message), "%s RVA: 0x%llX", label, static_cast<unsigned long long>(offset));
    Log(level, message);
}

void LogBytes(ELogLevel level, const char* label, const uint8_t* ptr, size_t count) {
    if (!IsMemoryRangeUsable(const_cast<uint8_t*>(ptr), count, false)) {
        Log(level, "Requested byte dump is not readable.");
        return;
    }

    char message[256]{};
    int written = std::snprintf(message, sizeof(message), "%s:", label);
    if (written < 0) {
        return;
    }

    size_t used = static_cast<size_t>(written);
    for (size_t i = 0; i < count && used + 4 < sizeof(message); ++i) {
        int next = std::snprintf(message + used, sizeof(message) - used, " %02X", ptr[i]);
        if (next < 0) {
            return;
        }
        used += static_cast<size_t>(next);
    }

    Log(level, message);
}

void LogRel32(ELogLevel level, const char* label, const uint8_t* displacementAddress, const uint8_t* base) {
    if (!IsReadableAddress(const_cast<uint8_t*>(displacementAddress), sizeof(int32_t))) {
        Log(level, "rel32 displacement is not readable.");
        return;
    }

    int32_t displacement = 0;
    std::memcpy(&displacement, displacementAddress, sizeof(displacement));
    uint8_t* target = const_cast<uint8_t*>(displacementAddress) + sizeof(displacement) + displacement;
    uintptr_t sourceRva = reinterpret_cast<uintptr_t>(displacementAddress) - reinterpret_cast<uintptr_t>(base);
    uintptr_t targetRva = reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(base);

    char message[180]{};
    std::snprintf(
        message,
        sizeof(message),
        "%s source RVA=0x%llX rel=%lld target RVA=0x%llX",
        label,
        static_cast<unsigned long long>(sourceRva),
        static_cast<long long>(displacement),
        static_cast<unsigned long long>(targetRva));
    Log(level, message);
}

#if LANG5_ENABLE_DEFERRED_CALLER_HOOK
struct CodeBuffer {
    std::array<uint8_t, 512> Bytes{};
    size_t Size = 0;
};

void AppendU8(CodeBuffer& code, uint8_t value) {
    if (code.Size < code.Bytes.size()) {
        code.Bytes[code.Size++] = value;
    }
}

void AppendU32(CodeBuffer& code, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        AppendU8(code, static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendU64(CodeBuffer& code, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        AppendU8(code, static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

bool WriteProtectedMemory(void* target, const void* source, size_t size) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    std::memcpy(target, source, size);
    FlushInstructionCache(GetCurrentProcess(), target, size);

    DWORD restoredProtect = 0;
    return VirtualProtect(target, size, oldProtect, &restoredProtect) != 0;
}

bool BuildRel32Jump(uint8_t* source, uint8_t* target, std::array<uint8_t, 5>* out) {
    intptr_t delta = target - (source + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        return false;
    }

    (*out)[0] = 0xE9;
    int32_t rel = static_cast<int32_t>(delta);
    std::memcpy(out->data() + 1, &rel, sizeof(rel));
    return true;
}

bool AppendRel32Jump(CodeBuffer& code, uint8_t* codeBase, uint8_t* target) {
    uint8_t* source = codeBase + code.Size;
    intptr_t delta = target - (source + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        return false;
    }

    AppendU8(code, 0xE9);
    AppendU32(code, static_cast<uint32_t>(static_cast<int32_t>(delta)));
    return true;
}

void* AllocNearMemory(uint8_t* target, size_t size) {
    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    uintptr_t granularity = sysInfo.dwAllocationGranularity;
    uintptr_t targetAddress = reinterpret_cast<uintptr_t>(target);
    uintptr_t minAddress = targetAddress > 0x7FFF0000ULL ? targetAddress - 0x7FFF0000ULL : granularity;
    uintptr_t maxAddress = targetAddress + 0x7FFF0000ULL;

    minAddress &= ~(granularity - 1);
    maxAddress &= ~(granularity - 1);

    for (uintptr_t address = minAddress; address < maxAddress; address += granularity) {
        void* result = VirtualAlloc(
            reinterpret_cast<void*>(address),
            size,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE);
        if (result) {
            return result;
        }
    }

    return nullptr;
}
#endif

bool GetMainModuleRange(uint8_t** base, size_t* size) {
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) {
        return false;
    }

    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) {
        return false;
    }

    *base = static_cast<uint8_t*>(info.lpBaseOfDll);
    *size = static_cast<size_t>(info.SizeOfImage);
    return *base != nullptr && *size > 0;
}

bool IsInRange(uint8_t* ptr, uint8_t* base, size_t size) {
    uintptr_t start = reinterpret_cast<uintptr_t>(base);
    uintptr_t end = start + size;
    uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
    return end >= start && value >= start && value < end;
}

bool IsReadableProtect(DWORD protect) {
    if ((protect & PAGE_GUARD) || (protect & PAGE_NOACCESS)) {
        return false;
    }

    switch (protect & 0xFF) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool IsExecutableProtect(DWORD protect) {
    if ((protect & PAGE_GUARD) || (protect & PAGE_NOACCESS)) {
        return false;
    }

    switch (protect & 0xFF) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool IsMemoryRangeUsable(void* ptr, size_t size, bool executable) {
    if (!ptr || size == 0) {
        return false;
    }

    uintptr_t current = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t end = current + size;
    if (end < current) {
        return false;
    }

    while (current < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(current), &mbi, sizeof(mbi))) {
            return false;
        }

        if (mbi.State != MEM_COMMIT) {
            return false;
        }

        bool usable = executable ? IsExecutableProtect(mbi.Protect) : IsReadableProtect(mbi.Protect);
        if (!usable) {
            return false;
        }

        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= current) {
            return false;
        }

        current = regionEnd;
    }

    return true;
}

bool IsReadableAddress(void* ptr, size_t size) {
    return IsMemoryRangeUsable(ptr, size, false);
}

bool IsExecutableAddress(void* ptr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) {
        return false;
    }

    return mbi.State == MEM_COMMIT && IsExecutableProtect(mbi.Protect);
}

uint8_t* FindBytes(uint8_t* base, size_t size, const uint8_t* pattern, size_t patternSize) {
    if (!base || !pattern || patternSize == 0 || size < patternSize) {
        return nullptr;
    }

    for (size_t i = 0; i <= size - patternSize; ++i) {
        if (std::memcmp(base + i, pattern, patternSize) == 0) {
            return base + i;
        }
    }

    return nullptr;
}

uint8_t* FindBytesInMemory(uint8_t* base, size_t size, const uint8_t* pattern, size_t patternSize, bool executable) {
    if (!base || !pattern || patternSize == 0 || size < patternSize) {
        return nullptr;
    }

    uintptr_t moduleStart = reinterpret_cast<uintptr_t>(base);
    uintptr_t moduleEnd = moduleStart + size;
    if (moduleEnd < moduleStart) {
        return nullptr;
    }

    uintptr_t current = moduleStart;
    while (current < moduleEnd) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(current), &mbi, sizeof(mbi))) {
            break;
        }

        uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t regionEnd = regionStart + mbi.RegionSize;
        if (regionEnd <= current) {
            break;
        }

        bool usable = mbi.State == MEM_COMMIT && (executable ? IsExecutableProtect(mbi.Protect) : IsReadableProtect(mbi.Protect));
        if (usable) {
            uintptr_t scanStart = regionStart > moduleStart ? regionStart : moduleStart;
            uintptr_t scanEnd = regionEnd < moduleEnd ? regionEnd : moduleEnd;
            if (scanEnd > scanStart && scanEnd - scanStart >= patternSize) {
                uint8_t* found = FindBytes(
                    reinterpret_cast<uint8_t*>(scanStart),
                    static_cast<size_t>(scanEnd - scanStart),
                    pattern,
                    patternSize);
                if (found) {
                    return found;
                }
            }
        }

        current = regionEnd;
    }

    return nullptr;
}

uint8_t* FindAsciiLiteral(uint8_t* base, size_t size, const char* text) {
    return FindBytesInMemory(
        base,
        size,
        reinterpret_cast<const uint8_t*>(text),
        std::strlen(text),
        false);
}

bool IsRel32CallOperand(uint8_t* displacementAddress) {
    return displacementAddress &&
        IsReadableAddress(displacementAddress - 1, 5) &&
        displacementAddress[-1] == 0xE8;
}

uint8_t* FollowRel32(uint8_t* displacementAddress) {
    if (!IsReadableAddress(displacementAddress, sizeof(int32_t))) {
        return nullptr;
    }

    int32_t displacement = 0;
    std::memcpy(&displacement, displacementAddress, sizeof(displacement));
    return displacementAddress + sizeof(displacement) + displacement;
}

uint8_t* FindLeaRipRefTo(uint8_t* base, size_t size, uint8_t* target) {
    if (!base || !target || size < 7) {
        return nullptr;
    }

    uintptr_t moduleStart = reinterpret_cast<uintptr_t>(base);
    uintptr_t moduleEnd = moduleStart + size;
    if (moduleEnd < moduleStart) {
        return nullptr;
    }

    uintptr_t current = moduleStart;
    while (current < moduleEnd) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(current), &mbi, sizeof(mbi))) {
            break;
        }

        uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t regionEnd = regionStart + mbi.RegionSize;
        if (regionEnd <= current) {
            break;
        }

        bool executable = mbi.State == MEM_COMMIT && IsExecutableProtect(mbi.Protect);
        if (executable) {
            uintptr_t scanStart = regionStart > moduleStart ? regionStart : moduleStart;
            uintptr_t scanEnd = regionEnd < moduleEnd ? regionEnd : moduleEnd;
            if (scanEnd > scanStart && scanEnd - scanStart >= 7) {
                for (uintptr_t cursor = scanStart; cursor <= scanEnd - 7; ++cursor) {
                    uint8_t* instr = reinterpret_cast<uint8_t*>(cursor);
                    if (instr[0] != 0x48 || instr[1] != 0x8D || instr[2] != 0x0D) {
                        continue;
                    }

                    uint8_t* displacementAddress = instr + 3;
                    if (FollowRel32(displacementAddress) == target) {
                        return displacementAddress;
                    }
                }
            }
        }

        current = regionEnd;
    }

    return nullptr;
}

bool ResolveLanguageSetter() {
    uint8_t* base = nullptr;
    size_t size = 0;
    if (!GetMainModuleRange(&base, &size)) {
        Log(LOGL_CRITICAL, "Failed to read main module range.");
        return false;
    }

    uint8_t* anchor = FindAsciiLiteral(base, size, kValidateLanguageAnchor);
    if (!anchor) {
        Log(LOGL_WARNING, "Could not find ValidateLanguage anchor string.");
        return false;
    }
    LogAddress(LOGL_INFO, "ValidateLanguage anchor", anchor);
    LogModuleOffset(LOGL_INFO, "ValidateLanguage anchor", anchor, base);

    uint8_t* parentBlock = FindLeaRipRefTo(base, size, anchor);
    if (!parentBlock) {
        Log(LOGL_WARNING, "Could not find RIP-relative reference to ValidateLanguage anchor.");
        return false;
    }
    LogAddress(LOGL_INFO, "ValidateLanguage RIP displacement", parentBlock);
    LogModuleOffset(LOGL_INFO, "ValidateLanguage RIP displacement", parentBlock, base);
    LogBytes(LOGL_INFO, "Reference instruction bytes", parentBlock - 3, 32);

    uint8_t* originLangPtrAddress = parentBlock + 0x0B;
    uint8_t* setterTargetAddress = parentBlock + 0x24;
    LogRel32(LOGL_INFO, "Original language rel32", originLangPtrAddress, base);
    LogRel32(LOGL_INFO, "Language setter rel32", setterTargetAddress, base);

    if (!IsInRange(originLangPtrAddress, base, size) || !IsInRange(setterTargetAddress, base, size)) {
        Log(LOGL_WARNING, "Derived language offsets are outside the module range.");
        return false;
    }

    if (!IsReadableAddress(originLangPtrAddress, sizeof(int32_t)) || !IsReadableAddress(setterTargetAddress, sizeof(int32_t))) {
        Log(LOGL_WARNING, "Derived language displacement addresses are not readable.");
        return false;
    }

    if (!IsRel32CallOperand(setterTargetAddress)) {
        Log(LOGL_WARNING, "Derived language setter displacement is not a call rel32 operand. GW2 layout may have changed.");
        return false;
    }

    uint8_t* originLangPtr = FollowRel32(originLangPtrAddress);
    uint8_t* setterTarget = FollowRel32(setterTargetAddress);

    if (!originLangPtr || !setterTarget || !IsInRange(setterTarget, base, size)) {
        Log(LOGL_WARNING, "Derived language setter target is invalid.");
        return false;
    }

    if (!IsExecutableAddress(setterTarget)) {
        Log(LOGL_WARNING, "Derived language setter target is not executable.");
        return false;
    }

    if (!IsReadableAddress(originLangPtr, sizeof(uint32_t))) {
        Log(LOGL_WARNING, "Derived original language pointer is not readable.");
        return false;
    }

    g_originalLanguagePtr = reinterpret_cast<uint32_t*>(originLangPtr);
    g_originalLanguage = *g_originalLanguagePtr;
    g_languageSetter = reinterpret_cast<LanguageSetterFn>(setterTarget);

    LogAddress(LOGL_INFO, "Original language pointer", reinterpret_cast<void*>(g_originalLanguagePtr));
    LogAddress(LOGL_INFO, "Language setter target", reinterpret_cast<void*>(g_languageSetter));
    LogModuleOffset(LOGL_INFO, "Original language pointer", reinterpret_cast<uint8_t*>(g_originalLanguagePtr), base);
    LogModuleOffset(LOGL_INFO, "Language setter target", setterTarget, base);
    LogBytes(LOGL_INFO, "Language setter bytes 00-15", setterTarget, 16);
    LogBytes(LOGL_INFO, "Language setter bytes 16-31", setterTarget + 16, 16);
    char languageMessage[96]{};
    std::snprintf(languageMessage, sizeof(languageMessage), "Original language value: %u", static_cast<unsigned>(g_originalLanguage));
    Log(LOGL_INFO, languageMessage);
    Log(LOGL_INFO, "Resolved language setter.");
    return true;
}

void ResolveCallerHookDiagnostics() {
    uint8_t* base = nullptr;
    size_t size = 0;
    if (!GetMainModuleRange(&base, &size)) {
        Log(LOGL_WARNING, "Caller hook diagnostics could not read main module range.");
        return;
    }

    uint8_t* anchor = FindAsciiLiteral(base, size, kViewAdvanceTextAnchor);
    if (!anchor) {
        Log(LOGL_WARNING, "Could not find ViewAdvanceText anchor string.");
        return;
    }

    LogAddress(LOGL_INFO, "ViewAdvanceText anchor", anchor);
    LogModuleOffset(LOGL_INFO, "ViewAdvanceText anchor", anchor, base);

    uint8_t* ref = FindLeaRipRefTo(base, size, anchor);
    if (!ref) {
        Log(LOGL_WARNING, "Could not find RIP-relative reference to ViewAdvanceText anchor.");
        return;
    }

    LogAddress(LOGL_INFO, "ViewAdvanceText RIP displacement", ref);
    LogModuleOffset(LOGL_INFO, "ViewAdvanceText RIP displacement", ref, base);

    uint8_t* hookPoint = ref - 0x08;
    if (!IsInRange(hookPoint, base, size) || !IsReadableAddress(hookPoint, 48)) {
        Log(LOGL_WARNING, "Derived ViewAdvanceText caller hook point is not readable.");
        return;
    }

    LogAddress(LOGL_INFO, "ViewAdvanceText caller hook point", hookPoint);
    LogModuleOffset(LOGL_INFO, "ViewAdvanceText caller hook point", hookPoint, base);
    LogBytes(LOGL_INFO, "ViewAdvanceText hook bytes 00-15", hookPoint, 16);
    LogBytes(LOGL_INFO, "ViewAdvanceText hook bytes 16-31", hookPoint + 16, 16);
    LogBytes(LOGL_INFO, "ViewAdvanceText hook bytes 32-47", hookPoint + 32, 16);

    if (hookPoint[0] != 0xE8) {
        Log(LOGL_WARNING, "ViewAdvanceText hook point does not start with a call instruction.");
        return;
    }

    if (hookPoint + 5 != ref - 3 || ref[-3] != 0x48 || ref[-2] != 0x8D || ref[-1] != 0x0D) {
        Log(LOGL_WARNING, "ViewAdvanceText hook point no longer matches expected call + lea layout.");
        return;
    }

    uint8_t* originalCallTarget = FollowRel32(hookPoint + 1);
    if (!originalCallTarget || !IsInRange(originalCallTarget, base, size)) {
        Log(LOGL_WARNING, "ViewAdvanceText original call target is invalid.");
        return;
    }

    g_viewAdvanceTextHookPoint = hookPoint;
    g_viewAdvanceTextOriginalCallTarget = originalCallTarget;
    LogAddress(LOGL_INFO, "ViewAdvanceText original call target", originalCallTarget);
    LogModuleOffset(LOGL_INFO, "ViewAdvanceText original call target", originalCallTarget, base);
}

// Discover the CParser::Validate text path where GW2 finalizes a parsed UI
// string, so the simplified->traditional converter can run on it. Mirrors the
// original lang5: reference to the assertion string, then +0xAA lands on the
// `mov rbp, rax` that hands the string pointer (rax) onward. Always compiled so
// the RVA is logged even in diagnostic builds; installation is gated separately.
void ResolveTextConverterHookPoint() {
    uint8_t* base = nullptr;
    size_t size = 0;
    if (!GetMainModuleRange(&base, &size)) {
        Log(LOGL_WARNING, "Text converter diagnostics could not read main module range.");
        return;
    }

    uint8_t* anchor = FindAsciiLiteral(base, size, kCParserValidateAnchor);
    if (!anchor) {
        Log(LOGL_WARNING, "Could not find CParser::Validate anchor string.");
        return;
    }
    LogAddress(LOGL_INFO, "CParser::Validate anchor", anchor);
    LogModuleOffset(LOGL_INFO, "CParser::Validate anchor", anchor, base);

    uint8_t* ref = FindLeaRipRefTo(base, size, anchor);
    if (!ref) {
        Log(LOGL_WARNING, "Could not find RIP-relative reference to CParser::Validate anchor.");
        return;
    }
    LogAddress(LOGL_INFO, "CParser::Validate RIP displacement", ref);
    LogModuleOffset(LOGL_INFO, "CParser::Validate RIP displacement", ref, base);

    uint8_t* hookPoint = ref + 0xAA;
    if (!IsInRange(hookPoint, base, size) || !IsReadableAddress(hookPoint, 16)) {
        Log(LOGL_WARNING, "Derived text converter hook point is not readable.");
        return;
    }
    LogAddress(LOGL_INFO, "Text converter hook point", hookPoint);
    LogModuleOffset(LOGL_INFO, "Text converter hook point", hookPoint, base);
    LogBytes(LOGL_INFO, "Text converter hook bytes 00-15", hookPoint, 16);

    if (std::memcmp(hookPoint, kTextConverterExpectedBytes, sizeof(kTextConverterExpectedBytes)) != 0) {
        Log(LOGL_WARNING, "Text converter hook point is not 'mov rbp, rax'. GW2 layout may have changed.");
        return;
    }

    g_textConverterHookPoint = hookPoint;
    Log(LOGL_INFO, "Resolved text converter hook point.");
}

#if LANG5_ENABLE_TEXT_CONVERSION
void ConverterLog(int level, const char* message) {
    Log(static_cast<ELogLevel>(level), message);
}

// Emit the register-preserving trampoline that calls Lang5ConvertStringInPlace
// with the string pointer (rax) and then continues into MinHook's trampoline,
// which runs the relocated original instruction(s) and returns to the game.
size_t BuildConverterStub(uint8_t* buf, void* convFn, void* trampoline) {
    size_t n = 0;
    auto u8 = [&](uint8_t b) { buf[n++] = b; };
    auto u64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            u8(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
        }
    };

    u8(0x55);                               // push rbp
    u8(0x48); u8(0x89); u8(0xE5);           // mov rbp, rsp
    u8(0x9C);                               // pushfq
    u8(0x50);                               // push rax
    u8(0x51);                               // push rcx
    u8(0x52);                               // push rdx
    u8(0x41); u8(0x50);                     // push r8
    u8(0x41); u8(0x51);                     // push r9
    u8(0x41); u8(0x52);                     // push r10
    u8(0x41); u8(0x53);                     // push r11
    u8(0x48); u8(0x89); u8(0xC1);           // mov rcx, rax  (arg0 = string ptr)
    u8(0x48); u8(0x83); u8(0xE4); u8(0xF0); // and rsp, -16  (align)
    u8(0x48); u8(0x83); u8(0xEC); u8(0x20); // sub rsp, 0x20 (shadow space)
    u8(0x48); u8(0xB8); u64(reinterpret_cast<uint64_t>(convFn)); // mov rax, convFn
    u8(0xFF); u8(0xD0);                     // call rax
    u8(0x4C); u8(0x8B); u8(0x5D); u8(0xC0); // mov r11, [rbp-0x40]
    u8(0x4C); u8(0x8B); u8(0x55); u8(0xC8); // mov r10, [rbp-0x38]
    u8(0x4C); u8(0x8B); u8(0x4D); u8(0xD0); // mov r9,  [rbp-0x30]
    u8(0x4C); u8(0x8B); u8(0x45); u8(0xD8); // mov r8,  [rbp-0x28]
    u8(0x48); u8(0x8B); u8(0x55); u8(0xE0); // mov rdx, [rbp-0x20]
    u8(0x48); u8(0x8B); u8(0x4D); u8(0xE8); // mov rcx, [rbp-0x18]
    u8(0x48); u8(0x8B); u8(0x45); u8(0xF0); // mov rax, [rbp-0x10]
    u8(0xFF); u8(0x75); u8(0xF8);           // push qword [rbp-0x08]  (flags)
    u8(0x9D);                               // popfq
    u8(0x48); u8(0x89); u8(0xEC);           // mov rsp, rbp
    u8(0x5D);                               // pop rbp
    u8(0xFF); u8(0x25); u8(0x00); u8(0x00); u8(0x00); u8(0x00); // jmp qword [rip+0]
    u64(reinterpret_cast<uint64_t>(trampoline));                // absolute jump target

    return n;
}

bool CreateConverterHook() {
    if (g_converterHookCreated) {
        return true;
    }
    if (!g_api || !g_api->MinHook_Create || !g_api->MinHook_Enable || !g_textConverterHookPoint) {
        Log(LOGL_WARNING, "Text converter hook prerequisites are missing (MinHook or hook point).");
        return false;
    }

    g_converterStub = VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_converterStub) {
        Log(LOGL_CRITICAL, "Failed to allocate text converter stub.");
        return false;
    }

    EMHStatus status = g_api->MinHook_Create(
        g_textConverterHookPoint, g_converterStub, &g_converterTrampoline);
    if (status != MH_OK || !g_converterTrampoline) {
        char message[96]{};
        std::snprintf(message, sizeof(message), "MinHook_Create failed for text converter. status=%d", static_cast<int>(status));
        Log(LOGL_CRITICAL, message);
        VirtualFree(g_converterStub, 0, MEM_RELEASE);
        g_converterStub = nullptr;
        g_converterTrampoline = nullptr;
        return false;
    }

    size_t stubLen = BuildConverterStub(
        static_cast<uint8_t*>(g_converterStub),
        reinterpret_cast<void*>(&Lang5ConvertStringInPlace),
        g_converterTrampoline);
    FlushInstructionCache(GetCurrentProcess(), g_converterStub, stubLen);

    g_converterHookCreated = true;
    LogAddress(LOGL_INFO, "Text converter stub", g_converterStub);
    LogAddress(LOGL_INFO, "Text converter trampoline", g_converterTrampoline);
    Log(LOGL_INFO, "Created text converter hook.");
    return true;
}

bool SetConverterEnabled(bool enable) {
    if (!enable) {
        if (g_converterHookCreated && g_converterEnabled && g_api && g_api->MinHook_Disable) {
            g_api->MinHook_Disable(g_textConverterHookPoint);
            g_converterEnabled = false;
            Log(LOGL_INFO, "Disabled text converter hook.");
        }
        return true;
    }

    if (!Lang5DictionariesReady()) {
        Log(LOGL_WARNING, "Traditional conversion requested, but no dictionaries are loaded.");
        return false;
    }
    if (!g_converterHookCreated && !CreateConverterHook()) {
        return false;
    }
    if (!g_converterEnabled) {
        EMHStatus status = g_api->MinHook_Enable(g_textConverterHookPoint);
        if (status != MH_OK) {
            char message[96]{};
            std::snprintf(message, sizeof(message), "MinHook_Enable failed for text converter. status=%d", static_cast<int>(status));
            Log(LOGL_CRITICAL, message);
            return false;
        }
        g_converterEnabled = true;
        Log(LOGL_INFO, "Enabled text converter hook.");
    }
    return true;
}

void DestroyConverterHook() {
    if (g_converterHookCreated && g_api) {
        if (g_converterEnabled && g_api->MinHook_Disable) {
            g_api->MinHook_Disable(g_textConverterHookPoint);
        }
        // Give any in-flight game-thread conversion time to return before the
        // stub memory is released.
        Sleep(50);
        if (g_api->MinHook_Remove) {
            g_api->MinHook_Remove(g_textConverterHookPoint);
        }
    }

    g_converterEnabled = false;
    g_converterHookCreated = false;
    if (g_converterStub) {
        VirtualFree(g_converterStub, 0, MEM_RELEASE);
        g_converterStub = nullptr;
    }
    g_converterTrampoline = nullptr;
}
#endif // LANG5_ENABLE_TEXT_CONVERSION

#if LANG5_ENABLE_DEFERRED_CALLER_HOOK
void CheckDeferredCallCompletion() {
    if (!g_waitingForDeferredCall) {
        return;
    }

    if (g_pendingCall.Function != 0) {
        return;
    }

    g_waitingForDeferredCall = false;
    char message[160]{};
    std::snprintf(
        message,
        sizeof(message),
        "Deferred language setter executed. count=%llu currentLanguage=%u",
        static_cast<unsigned long long>(g_pendingCall.CompletedCount),
        g_originalLanguagePtr ? static_cast<unsigned>(*g_originalLanguagePtr) : 9999U);
    Log(LOGL_INFO, message);
}

bool InstallDeferredCallerHook() {
    if (!g_languageSetter || !g_viewAdvanceTextHookPoint || !g_viewAdvanceTextOriginalCallTarget) {
        Log(LOGL_WARNING, "Deferred caller hook prerequisites are missing.");
        return false;
    }

    if (g_callerHookInstalled) {
        return true;
    }

    std::memcpy(g_callerHookBackup.data(), g_viewAdvanceTextHookPoint, g_callerHookBackup.size());

    uint8_t* jumpBack = g_viewAdvanceTextHookPoint + g_callerHookBackup.size();
    g_callerCodeCave = AllocNearMemory(g_viewAdvanceTextHookPoint, 512);
    if (!g_callerCodeCave) {
        Log(LOGL_CRITICAL, "Failed to allocate deferred caller code cave.");
        return false;
    }

    CodeBuffer code{};

    // Preserve caller state, run one queued function call, then execute the
    // original call instruction that the hook overwrites.
    AppendU8(code, 0x9C);             // pushfq
    AppendU8(code, 0x50);             // push rax
    AppendU8(code, 0x53);             // push rbx
    AppendU8(code, 0x51);             // push rcx
    AppendU8(code, 0x52);             // push rdx
    AppendU8(code, 0x41); AppendU8(code, 0x50); // push r8
    AppendU8(code, 0x41); AppendU8(code, 0x51); // push r9
    AppendU8(code, 0x41); AppendU8(code, 0x52); // push r10
    AppendU8(code, 0x41); AppendU8(code, 0x53); // push r11
    AppendU8(code, 0x48); AppendU8(code, 0x83); AppendU8(code, 0xEC); AppendU8(code, 0x28); // sub rsp, 0x28

    AppendU8(code, 0x48); AppendU8(code, 0xBB); AppendU64(code, reinterpret_cast<uint64_t>(&g_pendingCall)); // mov rbx, imm64
    AppendU8(code, 0x48); AppendU8(code, 0x8B); AppendU8(code, 0x03); // mov rax, [rbx]
    AppendU8(code, 0x48); AppendU8(code, 0x85); AppendU8(code, 0xC0); // test rax, rax

    size_t jeOffset = code.Size;
    AppendU8(code, 0x0F); AppendU8(code, 0x84); AppendU32(code, 0); // je skip

    AppendU8(code, 0x8B); AppendU8(code, 0x4B); AppendU8(code, 0x08); // mov ecx, [rbx+8]
    AppendU8(code, 0xFF); AppendU8(code, 0xD0); // call rax
    AppendU8(code, 0x48); AppendU8(code, 0xFF); AppendU8(code, 0x43); AppendU8(code, 0x10); // inc qword [rbx+0x10]
    AppendU8(code, 0x48); AppendU8(code, 0xC7); AppendU8(code, 0x03); AppendU32(code, 0); // mov qword [rbx], 0

    size_t skipOffset = code.Size;
    int32_t jeRel = static_cast<int32_t>(skipOffset - (jeOffset + 6));
    std::memcpy(code.Bytes.data() + jeOffset + 2, &jeRel, sizeof(jeRel));

    AppendU8(code, 0x48); AppendU8(code, 0x83); AppendU8(code, 0xC4); AppendU8(code, 0x28); // add rsp, 0x28
    AppendU8(code, 0x41); AppendU8(code, 0x5B); // pop r11
    AppendU8(code, 0x41); AppendU8(code, 0x5A); // pop r10
    AppendU8(code, 0x41); AppendU8(code, 0x59); // pop r9
    AppendU8(code, 0x41); AppendU8(code, 0x58); // pop r8
    AppendU8(code, 0x5A);             // pop rdx
    AppendU8(code, 0x59);             // pop rcx
    AppendU8(code, 0x5B);             // pop rbx
    AppendU8(code, 0x58);             // pop rax
    AppendU8(code, 0x9D);             // popfq

    AppendU8(code, 0x48); AppendU8(code, 0xB8); AppendU64(code, reinterpret_cast<uint64_t>(g_viewAdvanceTextOriginalCallTarget)); // mov rax, imm64
    AppendU8(code, 0xFF); AppendU8(code, 0xD0); // call rax
    if (!AppendRel32Jump(code, static_cast<uint8_t*>(g_callerCodeCave), jumpBack)) {
        Log(LOGL_CRITICAL, "Deferred caller return jump is outside rel32 range.");
        VirtualFree(g_callerCodeCave, 0, MEM_RELEASE);
        g_callerCodeCave = nullptr;
        return false;
    }

    std::memcpy(g_callerCodeCave, code.Bytes.data(), code.Size);
    FlushInstructionCache(GetCurrentProcess(), g_callerCodeCave, code.Size);

    std::array<uint8_t, 5> hookJump{};
    if (!BuildRel32Jump(g_viewAdvanceTextHookPoint, static_cast<uint8_t*>(g_callerCodeCave), &hookJump)) {
        Log(LOGL_CRITICAL, "Deferred caller code cave is outside rel32 jump range.");
        return false;
    }

    if (!WriteProtectedMemory(g_viewAdvanceTextHookPoint, hookJump.data(), hookJump.size())) {
        Log(LOGL_CRITICAL, "Failed to install deferred caller hook.");
        return false;
    }

    g_callerHookInstalled = true;
    LogAddress(LOGL_INFO, "Deferred caller code cave", g_callerCodeCave);
    Log(LOGL_INFO, "Installed deferred caller hook.");
    return true;
}

void UninstallDeferredCallerHook() {
    if (g_callerHookInstalled && g_viewAdvanceTextHookPoint) {
        WriteProtectedMemory(g_viewAdvanceTextHookPoint, g_callerHookBackup.data(), g_callerHookBackup.size());
    }

    g_callerHookInstalled = false;
    g_pendingCall = {};

    if (g_callerCodeCave) {
        VirtualFree(g_callerCodeCave, 0, MEM_RELEASE);
        g_callerCodeCave = nullptr;
    }
}
#endif

#if LANG5_ENABLE_MEMORY_WRITE
bool WriteLanguageValue(uint32_t languageId) {
    if (!g_originalLanguagePtr) {
        Log(LOGL_WARNING, "Original language pointer is not resolved.");
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(g_originalLanguagePtr, sizeof(uint32_t), PAGE_READWRITE, &oldProtect)) {
        Log(LOGL_CRITICAL, "Failed to make language value writable.");
        Alert("Lang5 Nexus CN: language value write failed.");
        return false;
    }

    *g_originalLanguagePtr = languageId;

    DWORD restoredProtect = 0;
    if (!VirtualProtect(g_originalLanguagePtr, sizeof(uint32_t), oldProtect, &restoredProtect)) {
        Log(LOGL_WARNING, "Failed to restore language value page protection.");
    }

    char message[96]{};
    std::snprintf(message, sizeof(message), "Wrote language value: %u", static_cast<unsigned>(languageId));
    Log(LOGL_INFO, message);
    return true;
}
#endif

#if LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
bool SetLanguage(uint32_t languageId) {
#if LANG5_ENABLE_DEFERRED_CALLER_HOOK
    if (!g_callerHookInstalled) {
        if (!InstallDeferredCallerHook()) {
            Alert("Lang5 Nexus CN: deferred caller hook failed; toggle cancelled.");
            return false;
        }
    }

    g_pendingCall.Arg0 = languageId;
    g_pendingCall.Function = reinterpret_cast<uintptr_t>(g_languageSetter);
    g_waitingForDeferredCall = true;
    char message[128]{};
    std::snprintf(message, sizeof(message), "Queued language setter through deferred caller hook. language=%u", static_cast<unsigned>(languageId));
    Log(LOGL_INFO, message);
    return true;
#elif LANG5_ENABLE_MEMORY_WRITE
    return WriteLanguageValue(languageId);
#elif LANG5_ENABLE_DIRECT_SETTER_CALL
    if (!g_languageSetter) {
        Log(LOGL_WARNING, "Language setter is not resolved.");
        return false;
    }

#if defined(_MSC_VER)
    __try {
        g_languageSetter(languageId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log(LOGL_CRITICAL, "Exception while calling language setter.");
        Alert("Lang5 Nexus CN: language setter failed.");
        return false;
    }
#else
    // MinGW does not support MSVC SEH syntax. Keep this path small and only
    // use it after address scanning has succeeded.
    g_languageSetter(languageId);
#endif

    return true;
#else
    (void)languageId;
    Log(LOGL_WARNING, "Unsafe language change requested, but no experimental mutation mode is enabled.");
    Alert("Lang5 Nexus CN: unsafe mutation mode is disabled.");
    return false;
#endif
}
#endif

#if LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
void ApplyChinese(bool enable) {
    uint32_t target = enable ? kChineseLanguageId : g_originalLanguage;
    if (SetLanguage(target)) {
        g_chineseEnabled = enable;
        Alert(enable ? "Lang5 Nexus CN: Chinese UI enabled." : "Lang5 Nexus CN: language restored.");
    }
}

void OnPostRender() {
#if LANG5_ENABLE_DEFERRED_CALLER_HOOK
    CheckDeferredCallCompletion();
#endif

    // Auto-enable Chinese shortly after launch when the preference is set.
    if (g_autoApplyScheduled && !g_chineseEnabled && !g_hasPendingApply) {
        if (g_autoApplyDelayFrames > 0) {
            --g_autoApplyDelayFrames;
        } else {
            g_autoApplyScheduled = false;
            g_pendingEnable = true;
            g_hasPendingApply = true;
            Log(LOGL_INFO, "Auto-enabling Chinese UI now.");
        }
    }

    if (!g_hasPendingApply) {
        return;
    }

    bool enable = g_pendingEnable;
    g_hasPendingApply = false;
    ApplyChinese(enable);
}
#endif

void OnOptionsRender() {
    if (ImGui::Checkbox("Auto-enable Chinese UI on launch", &g_autoEnableChinese)) {
        SaveSettings();
        Alert(g_autoEnableChinese
            ? "Lang5 Nexus CN: auto-enable Chinese on launch is ON."
            : "Lang5 Nexus CN: auto-enable Chinese on launch is OFF.");
    }

    ImGui::TextDisabled("Takes effect on the next game launch.");
    ImGui::TextDisabled("ALT+SHIFT+C toggles the Chinese UI right now.");
#if !LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
    ImGui::TextDisabled("Diagnostic build: preference is saved but no switching happens.");
#endif

    ImGui::Separator();
#if LANG5_ENABLE_TEXT_CONVERSION
    // Default is simplified (unchecked). Checking it converts the simplified
    // Chinese UI text to traditional in real time. The Nexus-bundled ImGui font
    // has no CJK glyphs, so this panel stays ASCII-only to avoid "????".
    if (ImGui::Checkbox("Enable Traditional Chinese", &g_traditionalEnabled)) {
        if (g_traditionalEnabled) {
            if (SetConverterEnabled(true)) {
                SaveSettings();
                Alert("Lang5 Nexus CN: Traditional Chinese ON.");
            } else {
                g_traditionalEnabled = false;
                SaveSettings();
                Alert("Lang5 Nexus CN: could not enable traditional; check logs (dictionaries?).");
            }
        } else {
            SetConverterEnabled(false);
            SaveSettings();
            Alert("Lang5 Nexus CN: back to Simplified Chinese.");
        }
    }
    ImGui::TextDisabled("Converts the simplified Chinese UI to traditional.");
    if (!Lang5DictionariesReady()) {
        ImGui::TextDisabled("Dictionaries not loaded; check the Nexus log for details.");
    }
#else
    ImGui::TextDisabled("Traditional Chinese conversion needs the experimental build.");
#endif
}

void OnToggleKeybind(const char* identifier, bool isRelease) {
    if (isRelease || std::strcmp(identifier, kToggleKeybind) != 0) {
        return;
    }

    if (!g_loaded) {
        Alert("Lang5 Nexus CN: not ready; address scan failed.");
        return;
    }

#if LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
    g_pendingEnable = !g_chineseEnabled;
    g_hasPendingApply = true;
    Alert(g_pendingEnable ? "Lang5 Nexus CN: Chinese UI toggle queued." : "Lang5 Nexus CN: language restore queued.");
#else
    Log(LOGL_WARNING, "Toggle ignored because unsafe call support is disabled.");
    Alert("Lang5 Nexus CN: diagnostic build loaded; toggle is disabled.");
#endif
}

void AddonLoad(AddonAPI_t* api) {
    g_api = api;

    if (!g_api) {
        return;
    }

    Log(LOGL_INFO, "Loading.");

    if (g_api->InputBinds_RegisterWithString) {
        g_api->InputBinds_RegisterWithString(kToggleKeybind, OnToggleKeybind, "ALT+SHIFT+C");
    }

    LoadSettings();

    // Adopt the Nexus-owned ImGui context so the options-panel checkbox renders
    // into the same ImGui instance instead of creating a private one.
    if (g_api->ImguiContext) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(g_api->ImguiContext));
        ImGui::SetAllocatorFunctions(
            reinterpret_cast<void* (*)(size_t, void*)>(g_api->ImguiMalloc),
            reinterpret_cast<void (*)(void*, void*)>(g_api->ImguiFree));
        if (g_api->GUI_Register) {
            g_api->GUI_Register(RT_OptionsRender, OnOptionsRender);
        }
    }

    g_loaded = ResolveLanguageSetter();
    if (!g_loaded) {
        Alert("Lang5 Nexus CN: address scan failed.");
        return;
    }
    ResolveCallerHookDiagnostics();
    ResolveTextConverterHookPoint();

#if LANG5_ENABLE_TEXT_CONVERSION
    if (g_api->Paths_GetAddonDirectory) {
        const char* addonDir = g_api->Paths_GetAddonDirectory(kAddonDirName);
        if (addonDir) {
            CreateDirectoryA(addonDir, nullptr);
            Lang5LoadDictionaries(addonDir, ConverterLog);
        }
    }

    if (g_traditionalEnabled) {
        if (SetConverterEnabled(true)) {
            Alert("Lang5 Nexus CN: Traditional Chinese enabled.");
        } else {
            g_traditionalEnabled = false;
            Alert("Lang5 Nexus CN: traditional preference on, but converter could not start (see logs).");
        }
    }
#endif

#if LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
    if (g_api->GUI_Register) {
        g_api->GUI_Register(RT_PostRender, OnPostRender);
    }

    if (g_autoEnableChinese) {
        g_autoApplyScheduled = true;
        g_autoApplyDelayFrames = kAutoApplyDelayFrames;
        Log(LOGL_INFO, "Auto-enable Chinese preference is on; scheduling launch toggle.");
        Alert("Lang5 Nexus CN: ready. Auto-enabling Chinese UI shortly.");
    } else {
        Alert("Lang5 Nexus CN: ready. ALT+SHIFT+C toggles Chinese; see addon options for auto-start.");
    }
#else
    Alert(g_autoEnableChinese
        ? "Lang5 Nexus CN: diagnostic build. Auto-start preference is ON (takes effect in experimental build)."
        : "Lang5 Nexus CN: diagnostic build ready. Unsafe toggle disabled.");
#endif
}

void AddonUnload() {
    Log(LOGL_INFO, "Unloading.");

    if (g_api && g_api->InputBinds_Deregister) {
        g_api->InputBinds_Deregister(kToggleKeybind);
    }

    if (g_api && g_api->GUI_Deregister) {
        g_api->GUI_Deregister(OnOptionsRender);
    }

#if LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
    if (g_api && g_api->GUI_Deregister) {
        g_api->GUI_Deregister(OnPostRender);
    }
#endif

#if LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
#if LANG5_ENABLE_DEFERRED_CALLER_HOOK
    if (g_loaded && g_chineseEnabled) {
        Log(LOGL_WARNING, "Skipping deferred language restore during unload; deferred calls cannot complete after hook removal.");
    }
#else
    if (g_loaded && g_chineseEnabled) {
        SetLanguage(g_originalLanguage);
    }
#endif
#endif

#if LANG5_ENABLE_DEFERRED_CALLER_HOOK
    UninstallDeferredCallerHook();
#endif

#if LANG5_ENABLE_TEXT_CONVERSION
    DestroyConverterHook();
    Lang5ClearDictionaries();
#endif
    g_textConverterHookPoint = nullptr;

    g_loaded = false;
    g_chineseEnabled = false;
    g_hasPendingApply = false;
    g_pendingEnable = false;
    g_waitingForDeferredCall = false;
#if LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
    g_autoApplyScheduled = false;
    g_autoApplyDelayFrames = 0;
#endif
    g_originalLanguage = 0;
    g_originalLanguagePtr = nullptr;
    g_languageSetter = nullptr;
    g_api = nullptr;
}

AddonDefinition_t g_addonDef{};

} // namespace

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef() {
    g_addonDef.Signature = 0x4C35434E; // "L5CN"; use a unique positive id if accepted by Raidcore.
    g_addonDef.APIVersion = NEXUS_API_VERSION;
    g_addonDef.Name = "Lang5 Nexus CN";
    g_addonDef.Version = AddonVersion_t{0, 3, 1, 0};
    g_addonDef.Author = "DayuGuo";
    g_addonDef.Description = "Experimental simplified Chinese UI selector with optional traditional-Chinese conversion and auto-enable on launch. Memory access.";
    g_addonDef.Load = AddonLoad;
    g_addonDef.Unload = AddonUnload;
    g_addonDef.Flags = static_cast<EAddonFlags>(AF_IsVolatile | AF_LaunchOnly);
    g_addonDef.Provider = UP_GitHub;
    g_addonDef.UpdateLink = "https://github.com/DayuGuo/lang5-nexus-cn";
    return &g_addonDef;
}
