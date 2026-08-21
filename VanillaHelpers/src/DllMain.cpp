// This file is part of VanillaHelpers.
//
// VanillaHelpers is free software: you can redistribute it and/or modify it under the terms of the
// GNU Lesser General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// VanillaHelpers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lessed General Public License along with
// VanillaHelpers. If not, see <https://www.gnu.org/licenses/>.

#include "Allocator.h"
#include "Blips.h"
#include "Common.h"
#include "FileIO.h"
#include "Game.h"
#include "MinHook.h"
#include "Morph.h"
#include "Offsets.h"
#include "TexBridge.h"
#include "Texture.h"

#include <string>

static Game::InitializeGlobal_t InitializeGlobal_o = nullptr;
static Game::FrameScript_Initialize_t FrameScript_Initialize_o = nullptr;
static Game::LoadScriptFunctions_t LoadScriptFunctions_o = nullptr;
static Game::CGGameUI_Shutdown_t CGGameUI_Shutdown_o = nullptr;

static void __fastcall InvalidFunctionPtrCheck_h() {}

static bool __fastcall InitializeGlobal_h() {
    bool ok = InitializeGlobal_o();
    Texture::InstallCharacterSkin();
    return ok;
}

static bool __fastcall FrameScript_Initialize_h() {
    FrameScript_Initialize_o();
    const std::string luaScript =
        "VANILLAHELPERS_VERSION=" + std::to_string(VANILLAHELPERS_VERSION_VALUE) +
        "\nVANILLA_HELPERS_VERSION=" + std::to_string(VANILLAHELPERS_VERSION_VALUE);
    Game::FrameScript_Execute(luaScript.c_str(), "VanillaHelpers.lua");
    return true;
}

static void __fastcall LoadScriptFunctions_h() {
    LoadScriptFunctions_o();
    FileIO::RegisterLuaFunctions();
    Blips::RegisterLuaFunctions();
    Morph::RegisterLuaFunctions();
}

static void __fastcall CGGameUI_Shutdown_h() {
    TexBridge::Shutdown(false);
    Morph::Reset();
    CGGameUI_Shutdown_o();
    Blips::Reset();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        Allocator::Initialize();
        Texture::Initialize();

        if (MH_Initialize() != MH_OK)
            return FALSE;

        auto *target = reinterpret_cast<LPVOID>(Offsets::FUN_INVALID_FUNCTION_PTR_CHECK);
        if (MH_CreateHook(target, static_cast<LPVOID>(InvalidFunctionPtrCheck_h), nullptr) != MH_OK)
            return FALSE;
        if (MH_EnableHook(target) != MH_OK)
            return FALSE;

        HOOK_FUNCTION(Offsets::FUN_INITIALIZE_GLOBAL, InitializeGlobal_h, InitializeGlobal_o);
        HOOK_FUNCTION(Offsets::FUN_FRAME_SCRIPT_INITIALIZE, FrameScript_Initialize_h,
                      FrameScript_Initialize_o);
        HOOK_FUNCTION(Offsets::FUN_LOAD_SCRIPT_FUNCTIONS, LoadScriptFunctions_h,
                      LoadScriptFunctions_o);
        HOOK_FUNCTION(Offsets::FUN_CGGAMEUI_SHUTDOWN, CGGameUI_Shutdown_h, CGGameUI_Shutdown_o);

        if (!Allocator::InstallHooks())
            return FALSE;
        if (!Texture::InstallHooks())
            return FALSE;

        // TextureServer64 integration: launch server + connect to shared memory.
        // Non-fatal — if server is unavailable, textures load via the original path.
        TexBridge::Initialize(hModule);
        TexBridge::EnsureServerRunning();
        TexBridge::InstallHooks();
    } else if (reason == DLL_PROCESS_DETACH) {
        // lpReserved != nullptr means the process is terminating (e.g. the user quit the
        // game and the OS called ExitProcess), as opposed to an explicit FreeLibrary() call.
        // On process termination, every thread except the one calling ExitProcess has already
        // been forcibly killed *before* this notification runs, so:
        //   - TexBridge::Shutdown(true)'s WaitForSingleObject on the worker thread can hit a
        //     thread that died mid-lock, and the SRWLock acquires later in Shutdown() can hang
        //     forever waiting on a lock that will never be released (SRWLocks are not
        //     abandoned like mutexes on thread death).
        //   - Its synchronous named-pipe write to the TextureServer64 process, and its batch of
        //     ReleaseD3DTexture() calls, both funnel back into Allocator.cpp's hooked/patched
        //     SMem paths at exactly the moment MH_Uninitialize() below would be unhooking them,
        //     leaving a half-reverted allocator state — the same asymmetry (permanent
        //     Common::PatchBytes patches, revertible MinHook hooks) discussed for the original
        //     VanillaHelpers DLL, but exercised harder here by all the extra teardown work.
        // None of this cleanup is needed when the whole process address space is about to be
        // torn down anyway, so skip it entirely on real process termination and only run it on
        // a genuine explicit unload.
        if (lpReserved == nullptr) {
            TexBridge::Shutdown(true);
            MH_Uninitialize();
        }
    }
    return TRUE;
}
