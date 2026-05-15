#include "Nexus.h"

#include <Psapi.h>
#include <Windows.h>

#include <cstdio>
#include <array>
#include <cstdint>
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

namespace {

constexpr const char* kChannel = "Lang5NexusCn";
constexpr const char* kToggleKeybind = "KB_LANG5_NEXUS_CN_TOGGLE";
constexpr const char* kValidateLanguageAnchor = "ValidateLanguage(language)";
constexpr const char* kViewAdvanceTextAnchor = "ViewAdvanceText";
constexpr uint32_t kChineseLanguageId = 5;

AddonAPI_t* g_api = nullptr;
bool g_loaded = false;
bool g_chineseEnabled = false;
bool g_hasPendingApply = false;
bool g_pendingEnable = false;
bool g_waitingForDeferredCall = false;
uint32_t g_originalLanguage = 0;
uint32_t* g_originalLanguagePtr = nullptr;

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
    AppendU8(code, 0x48); AppendU8(code, 0xB8); AppendU64(code, reinterpret_cast<uint64_t>(jumpBack)); // mov rax, imm64
    AppendU8(code, 0xFF); AppendU8(code, 0xE0); // jmp rax

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
        Log(LOGL_WARNING, "Deferred caller hook is not installed.");
        return false;
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

    if (!g_hasPendingApply) {
        return;
    }

    bool enable = g_pendingEnable;
    g_hasPendingApply = false;
    ApplyChinese(enable);
}
#endif

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

    g_loaded = ResolveLanguageSetter();
    if (!g_loaded) {
        Alert("Lang5 Nexus CN: address scan failed.");
        return;
    }
    ResolveCallerHookDiagnostics();

#if LANG5_ENABLE_DEFERRED_CALLER_HOOK
    if (!InstallDeferredCallerHook()) {
        Alert("Lang5 Nexus CN: deferred caller hook failed.");
        return;
    }
#endif

#if LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
    if (g_api->GUI_Register) {
        g_api->GUI_Register(RT_PostRender, OnPostRender);
    }
    Alert("Lang5 Nexus CN: ready. Press ALT+SHIFT+C to queue Chinese UI toggle.");
#else
    Alert("Lang5 Nexus CN: diagnostic build ready. Unsafe toggle disabled.");
#endif
}

void AddonUnload() {
    Log(LOGL_INFO, "Unloading.");

    if (g_api && g_api->InputBinds_Deregister) {
        g_api->InputBinds_Deregister(kToggleKeybind);
    }

#if LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
    if (g_api && g_api->GUI_Deregister) {
        g_api->GUI_Deregister(OnPostRender);
    }
#endif

#if LANG5_ENABLE_DEFERRED_CALLER_HOOK
    UninstallDeferredCallerHook();
#endif

#if LANG5_ENABLE_UNSAFE_LANGUAGE_CALL
    if (g_loaded && g_chineseEnabled) {
        SetLanguage(g_originalLanguage);
    }
#endif

    g_loaded = false;
    g_chineseEnabled = false;
    g_hasPendingApply = false;
    g_pendingEnable = false;
    g_waitingForDeferredCall = false;
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
    g_addonDef.Version = AddonVersion_t{0, 1, 0, 0};
    g_addonDef.Author = "Local prototype based on cy-sp-howard/lang5";
    g_addonDef.Description = "Experimental simplified Chinese UI selector. Memory access.";
    g_addonDef.Load = AddonLoad;
    g_addonDef.Unload = AddonUnload;
    g_addonDef.Flags = static_cast<EAddonFlags>(AF_IsVolatile | AF_LaunchOnly);
    g_addonDef.Provider = UP_None;
    g_addonDef.UpdateLink = "";
    return &g_addonDef;
}
