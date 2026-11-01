#include "stdafx.h"
#include "Startup.hpp"

namespace Startup {
namespace {
    // Learn & Play, December 2024. RVAs stay valid across ASLR layouts.
    constexpr DWORD kTimestamp = 0x67497d00;
    constexpr DWORD kImageSize = 0x330c000;
    // Before the saved-credentials branch, so cached credentials cannot start
    // another automatic sign-in attempt when the user chose offline startup.
    constexpr uintptr_t kPromptSite = 0x4ca79a;
    constexpr uintptr_t kFinishPrompt = 0x4cabe0;
    constexpr uintptr_t kPromptCleanup = 0x4ca900;
    constexpr uintptr_t kPromptResume = 0x4ca7a5;
    constexpr uintptr_t kOriginalPrompt = 0x4ca85c;
    constexpr uintptr_t kProfileServiceVtable = 0xddd894;
    constexpr uintptr_t kOfflineSite = 0x4bf4ec;
    constexpr uintptr_t kOfflineContinue = 0x4bf518;
    constexpr uintptr_t kProfileStartupState = 0xcc;
    constexpr DWORD kLocalProfileReady = 4;

    uintptr_t gameBase = 0;
    uintptr_t finishPrompt = 0;
    uintptr_t promptCleanup = 0;
    uintptr_t promptResume = 0;
    uintptr_t originalPrompt = 0;
    uintptr_t offlineContinue = 0;

    template <size_t N>
    bool Matches(uintptr_t rva, const BYTE (&expected)[N]) {
        BYTE actual[N]{};
        SIZE_T read = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(gameBase + rva),
            actual, N, &read) && read == N && memcmp(actual, expected, N) == 0;
    }

    int __cdecl ContinueWithLocalProfile(void* controller) {
        uintptr_t vtable = 0;
        DWORD state = 0;
        const auto address = reinterpret_cast<uintptr_t>(controller);
        if (!MemUtil::TryRead(address, vtable) || vtable != gameBase + kProfileServiceVtable ||
            !MemUtil::TryRead(address + kProfileStartupState, state) || state != 0)
            return 0; // Replay the displaced comparison/branch and keep the original flow.

        // The game's completion callback keeps the real account state. An offline
        // player stays offline; this does not report a successful Ubisoft login.
        using Complete = void(__thiscall*)(void*, int, int);
        reinterpret_cast<Complete>(finishPrompt)(controller, 0, 0);
        MemUtil::TryRead(address + kProfileStartupState, state);
        LOG_INFO("(STARTUP) Optional Ubisoft sign-in completed locally; state=" << state << ".\n");
        return state == kLocalProfileReady;
    }

    __declspec(naked) void SkipOptionalPrompt() {
        __asm {
            pushfd
            pushad
            push ebx
            call ContinueWithLocalProfile
            add esp, 4
            test eax, eax
            jz original
            popad
            popfd
            jmp dword ptr [promptCleanup]
        original:
            popad
            popfd
            cmp byte ptr [esp+17h], 0
            je showOriginalPrompt
            jmp dword ptr [promptResume]
        showOriginalPrompt:
            jmp dword ptr [originalPrompt]
        }
    }

    void __cdecl LogOfflineContinuation() {
        LOG_INFO("(STARTUP) Following RendezVous.SkipLogin; profile auto-connect not requested.\n");
    }

    __declspec(naked) void SkipProfileAutoConnect() {
        __asm {
            pushfd
            pushad
            call LogOfflineContinuation
            popad
            popfd
            jmp dword ptr [offlineContinue]
        }
    }

    bool InstallPromptSkip() {
        const BYTE site[] = {0x80,0x7c,0x24,0x17,0,0x0f,0x84,0xb7,0,0,0};
        const BYTE callback[] = {0xc7,0x81,0xcc,0,0,0,4,0,0,0};
        const BYTE cleanup[] = {0x8d,0x4c,0x24,0x30,0xc6,0x84,0x24,0x18,1,0,0,2};
        if (!Matches(kPromptSite, site) || !Matches(kFinishPrompt, callback) || !Matches(kPromptCleanup, cleanup)) {
            LOG_WARNING("(STARTUP) Sign-in skip signature mismatch; keeping the original prompt.\n");
            return false;
        }
        finishPrompt = gameBase + kFinishPrompt;
        promptCleanup = gameBase + kPromptCleanup;
        promptResume = gameBase + kPromptResume;
        originalPrompt = gameBase + kOriginalPrompt;
        if (!MemUtil::PlaceHook(reinterpret_cast<void*>(gameBase + kPromptSite), SkipOptionalPrompt, sizeof(site))) {
            LOG_ERROR("(STARTUP) Could not install optional sign-in skip.\n");
            return false;
        }
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(gameBase + kPromptSite), sizeof(site));
        LOG_INFO("(STARTUP) Optional Ubisoft sign-in skip installed.\n");
        return true;
    }

    void InstallOfflineStartup() {
        // This compare/branch follows the game's RendezVous.SkipLogin property
        // query. Its original skip target omits the request and leaves the
        // connection-requested flag unset, avoiding the later wait/failure UI.
        const BYTE branch[] = {0x80,0x7c,0x24,0x3b,0,0x74,0x25};
        const BYTE continuation[] = {0xe8,0x13,0x8c,0xff,0xff,0x8b,0x10,0x8b,0xc8};
        if (!Matches(kOfflineSite, branch) || !Matches(kOfflineContinue, continuation)) {
            LOG_WARNING("(STARTUP) Offline startup signature mismatch; keeping profile auto-connect unchanged.\n");
            return;
        }
        offlineContinue = gameBase + kOfflineContinue;
        if (!MemUtil::PlaceHook(reinterpret_cast<void*>(gameBase + kOfflineSite), SkipProfileAutoConnect, sizeof(branch))) {
            LOG_ERROR("(STARTUP) Could not install offline startup.\n");
            return;
        }
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(gameBase + kOfflineSite), sizeof(branch));
        LOG_INFO("(STARTUP) Offline profile startup installed.\n");
    }
}

void Install() {
    const bool offline = Settings::ReturnSettingValue("OfflineStartup") == "on";
    const bool skipPrompt = offline || Settings::ReturnSettingValue("SkipOptionalUbisoftLogin") == "on";
    if (!skipPrompt) return;

    gameBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 pe{};
    if (!MemUtil::TryRead(gameBase, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !MemUtil::TryRead(gameBase + dos.e_lfanew, pe) || pe.Signature != IMAGE_NT_SIGNATURE ||
        pe.FileHeader.TimeDateStamp != kTimestamp || pe.OptionalHeader.SizeOfImage != kImageSize) {
        LOG_WARNING("(STARTUP) Native startup options support the December 2024 Learn & Play executable only; keeping original startup.\n");
        return;
    }
    // Keep both parts together for Offline mode: a failed sign-in hook must not
    // leave a partially enabled mode that still prompts for account credentials.
    if (InstallPromptSkip() && offline)
        InstallOfflineStartup();
}
}
