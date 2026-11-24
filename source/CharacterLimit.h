#pragma once

#include <windows.h>
#include <cstdint>

namespace WowLogin
{
    inline constexpr std::uintptr_t LogOption = 1;
    inline constexpr std::uintptr_t FiftyCharactersOption = 2;
    inline constexpr DWORD CharacterLimitFailure = 0x100;

    enum class CharacterLimitResult : DWORD
    {
        Active = 0,
        Unsupported = 1,
        NoWindow = 2,
        AlreadyStarted = 3,
        HookFailed = 4,
        PostFailed = 5,
        TimerFailed = 6,
        TimedOut = 7
    };

    CharacterLimitResult EnableFiftyCharacterSlots(std::uintptr_t image, unsigned build, HMODULE module);
}
