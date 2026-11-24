#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include "CharacterLimit.h"
#include "ClientProfile.h"

namespace
{
    struct ScriptProfile
    {
        unsigned Build;
        std::uintptr_t Execute;
        std::uintptr_t LuaState;
        WowLogin::Signature Guards[3];
    };

    constexpr ScriptProfile ScriptProfiles[] =
    {
        {
            26972, 0x1D2A60, 0x18C1348,
            {
                { 0x1D2A60, "\x48\x89\x5C\x24\x08\x48\x89\x6C\x24\x10\x48\x89\x74\x24\x18\x57\x41\x56\x41\x57\x48\x83\xEC\x20" },
                { 0x1D2A78, "\xFF\x05\x4A\x3B\x70\x01\x48\x8B\x3D\xC3\xE8\x6E\x01" },
                { 0x4C96A0, "\x45\x33\xC0\x48\x8B\xD0\x48\x8B\xC8\xE8\xB2\x93\xD0\xFF" }
            }
        },
        {
            26365, 0x1C45D0, 0x18B9348,
            {
                { 0x1C45D0, "\x48\x89\x5C\x24\x08\x48\x89\x6C\x24\x10\x48\x89\x74\x24\x18\x57\x41\x56\x41\x57\x48\x83\xEC\x20" },
                { 0x1C45E8, "\xFF\x05\xDA\x9F\x70\x01\x48\x8B\x3D\x53\x4D\x6F\x01" },
                { 0x4BD7F0, "\x45\x33\xC0\x48\x8B\xD0\x48\x8B\xC8\xE8\xD2\x6D\xD0\xFF" }
            }
        }
    };

    // Touch only the stock GlueXML limit. Existing nonstandard limits are left alone.
    constexpr char LimitScript[] =
        "if MAX_CHARACTERS_PER_REALM==16 and type(CharacterSelect_OnLoad)=='function' "
        "and type(UpdateCharacterList)=='function' then "
        "MAX_CHARACTERS_PER_REALM=50 "
        "if CharacterSelect and CharacterSelect:IsShown() then UpdateCharacterList() end end";

    using ExecuteScript = void (*)(char const*, char const*, void*);

    std::atomic<bool> Requested{ false };
    std::atomic<bool> Cancelled{ false };
    std::atomic<HHOOK> BootstrapHook{ nullptr };
    std::atomic<WowLogin::CharacterLimitResult> SetupResult{ WowLogin::CharacterLimitResult::TimedOut };
    HWND ClientWindow = nullptr;
    DWORD ClientThread = 0;
    UINT BootstrapMessage = 0;
    UINT_PTR Timer = 0;
    unsigned char TimerTag = 0;
    HANDLE Ready = nullptr;
    std::uintptr_t Image = 0;
    ScriptProfile const* Profile = nullptr;

    bool ReadBytes(std::uintptr_t address, void* result, SIZE_T size)
    {
        SIZE_T read = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void const*>(address), result, size, &read) && read == size;
    }

    bool Matches(std::uintptr_t image, ScriptProfile const& profile)
    {
        char bytes[32];
        for (WowLogin::Signature const& guard : profile.Guards)
            if (guard.Bytes.size() > sizeof(bytes) || !ReadBytes(image + guard.Rva, bytes, guard.Bytes.size()) ||
                std::memcmp(bytes, guard.Bytes.data(), guard.Bytes.size()) != 0)
                return false;
        return true;
    }

    BOOL CALLBACK FindClientWindow(HWND window, LPARAM)
    {
        DWORD pid = 0;
        DWORD const thread = GetWindowThreadProcessId(window, &pid);
        wchar_t name[64]{};
        if (pid == GetCurrentProcessId() && thread && GetClassNameW(window, name, _countof(name)) &&
            std::wcscmp(name, L"GxWindowClass") == 0 && !GetWindow(window, GW_OWNER))
        {
            ClientWindow = window;
            ClientThread = thread;
            return FALSE;
        }
        return TRUE;
    }

    void CALLBACK ApplyLimit(HWND window, UINT, UINT_PTR timer, DWORD)
    {
        DWORD const savedError = GetLastError();
        if (Cancelled.load(std::memory_order_acquire))
        {
            KillTimer(window, timer);
            SetLastError(savedError);
            return;
        }
        if (GetCurrentThreadId() != ClientThread || window != ClientWindow || !Profile)
            return;

        std::uintptr_t state = 0;
        if (ReadBytes(Image + Profile->LuaState, &state, sizeof(state)) && state)
        {
            // This callback runs on the game's window/UI thread, never on the loader thread.
            reinterpret_cast<ExecuteScript>(Image + Profile->Execute)(LimitScript, "JirokyCharacterLimit", nullptr);
        }
        SetLastError(savedError);
    }

    LRESULT CALLBACK Bootstrap(int code, WPARAM removed, LPARAM data)
    {
        DWORD const savedError = GetLastError();
        HHOOK const installed = BootstrapHook.load(std::memory_order_acquire);
        if (installed && code == HC_ACTION && removed == PM_REMOVE)
        {
            auto const message = reinterpret_cast<MSG*>(data);
            if (message && message->hwnd == ClientWindow && message->message == BootstrapMessage)
            {
                message->message = WM_NULL;
                message->wParam = 0;
                message->lParam = 0;
                if (!Cancelled.load(std::memory_order_acquire))
                {
                    Timer = SetTimer(ClientWindow, reinterpret_cast<UINT_PTR>(&TimerTag), 1000, ApplyLimit);
                    if (Cancelled.load(std::memory_order_acquire))
                    {
                        if (Timer)
                            KillTimer(ClientWindow, Timer);
                    }
                    else if (Timer)
                    {
                        ApplyLimit(ClientWindow, WM_TIMER, Timer, 0);
                        SetupResult.store(WowLogin::CharacterLimitResult::Active, std::memory_order_release);
                    }
                    else
                        SetupResult.store(WowLogin::CharacterLimitResult::TimerFailed, std::memory_order_release);
                }
                if (HHOOK const hook = BootstrapHook.exchange(nullptr, std::memory_order_acq_rel))
                    UnhookWindowsHookEx(hook);
                SetEvent(Ready);
            }
        }
        SetLastError(savedError);
        return CallNextHookEx(nullptr, code, removed, data);
    }
}

WowLogin::CharacterLimitResult WowLogin::EnableFiftyCharacterSlots(std::uintptr_t image, unsigned build, HMODULE module)
{
    ScriptProfile const* selected = nullptr;
    for (ScriptProfile const& candidate : ScriptProfiles)
        if (candidate.Build == build && Matches(image, candidate))
        {
            selected = &candidate;
            break;
        }
    if (!selected)
        return CharacterLimitResult::Unsupported;
    if (Requested.exchange(true, std::memory_order_acq_rel))
        return CharacterLimitResult::AlreadyStarted;

    Image = image;
    Profile = selected;
    ULONGLONG const windowDeadline = GetTickCount64() + 4000;
    do
    {
        EnumWindows(FindClientWindow, 0);
        if (ClientWindow)
            break;
        Sleep(50);
    } while (GetTickCount64() < windowDeadline);
    if (!ClientWindow)
        return CharacterLimitResult::NoWindow;

    // Keep this event valid for a callback already in flight during a timeout.
    Ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BootstrapMessage = RegisterWindowMessageW(L"LegionClientLoginFix.CharacterLimit.Start");
    if (!Ready || !BootstrapMessage)
        return CharacterLimitResult::HookFailed;
    HHOOK const hook = SetWindowsHookExW(WH_GETMESSAGE, Bootstrap, module, ClientThread);
    if (!hook)
        return CharacterLimitResult::HookFailed;
    BootstrapHook.store(hook, std::memory_order_release);
    if (!PostMessageW(ClientWindow, BootstrapMessage, 0, 0))
    {
        Cancelled.store(true, std::memory_order_release);
        if (HHOOK const pending = BootstrapHook.exchange(nullptr, std::memory_order_acq_rel))
            UnhookWindowsHookEx(pending);
        return CharacterLimitResult::PostFailed;
    }
    if (WaitForSingleObject(Ready, 3000) != WAIT_OBJECT_0)
    {
        Cancelled.store(true, std::memory_order_release);
        if (HHOOK const pending = BootstrapHook.exchange(nullptr, std::memory_order_acq_rel))
            UnhookWindowsHookEx(pending);
        return CharacterLimitResult::TimedOut;
    }
    return SetupResult.load(std::memory_order_acquire);
}
