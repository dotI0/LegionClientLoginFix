#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "WatcherRepair.h"

namespace
{
    constexpr unsigned Socket = 0x90, RegisteredSocket = 0x98, State = 0xA4;
    constexpr unsigned CallbackLock = 0x190, CallbackInFlight = 0x1B8, WatchLock = 0x1E0, ExternalSocket = 0x241;
    constexpr unsigned DispatcherLock = 0xC18, LinkOffset = 0x1C8;
    constexpr LONG Connecting = 2, Closed = 6;

    using ClearCallback = void (*)(void*, void*, bool);
    using Unlink = void (*)(void*, void*);

    template<class T>
    T& Field(void* object, unsigned offset)
    {
        return *reinterpret_cast<T*>(static_cast<unsigned char*>(object) + offset);
    }

    void* Global(std::uintptr_t image, std::uintptr_t offset)
    {
        return *reinterpret_cast<void* volatile*>(image + offset);
    }

    struct TrySection
    {
        TrySection(void* object, unsigned offset) : Section(&Field<CRITICAL_SECTION>(object, offset)),
            Entered(TryEnterCriticalSection(Section) != FALSE) { }
        ~TrySection() { if (Entered) LeaveCriticalSection(Section); }
        TrySection(TrySection const&) = delete;
        TrySection& operator=(TrySection const&) = delete;
        CRITICAL_SECTION* const Section;
        bool const Entered;
    };
}

WatcherRepairOutcome RepairDownloaderWatcher(std::uintptr_t image, WowLogin::ClientProfile const& profile,
    void* downloader, std::uintptr_t socket, void* watcher)
{
    using Result = WatcherRepairOutcome;
    if (!watcher || !downloader || Global(image, profile.DownloaderGlobal) != downloader)
        return Result::NotApplicable;

    // The original REMOVE transfers the existing watcher reference to the native queue.
    if (Field<unsigned char>(watcher, ExternalSocket) != 1 || Field<LONG>(watcher, State) != Connecting)
        return Result::NotApplicable;
    TrySection watchGuard(watcher, WatchLock);
    if (!watchGuard.Entered)
        return Result::Busy;
    if (Field<unsigned char>(watcher, ExternalSocket) != 1 || Field<LONG>(watcher, State) != Connecting ||
        static_cast<LONG64>(socket) < 0 || Field<std::uintptr_t>(watcher, Socket) != socket)
        return Result::GuardRejected;

    void* const dispatcher = Global(image, profile.DispatcherGlobal);
    if (!dispatcher)
        return Result::GuardRejected;
    TrySection dispatcherGuard(dispatcher, DispatcherLock);
    if (!dispatcherGuard.Entered)
        return Result::Busy;
    if (Global(image, profile.DispatcherGlobal) != dispatcher || Field<LONG>(dispatcher, 0xC40) != static_cast<LONG>(LinkOffset) ||
        Field<unsigned char>(dispatcher, 0x10) != 0 || Field<std::uintptr_t>(watcher, RegisteredSocket) != socket ||
        !Field<void*>(watcher, LinkOffset) || !Field<void*>(watcher, LinkOffset + 8) ||
        InterlockedCompareExchange(&Field<LONG>(watcher, 0), 0, 0) < 2)
        return Result::GuardRejected;

    TrySection callbackGuard(watcher, CallbackLock);
    if (!callbackGuard.Entered || Field<LONG>(watcher, CallbackInFlight) != 0)
        return Result::Busy;

    // These native helpers reenter sections we already own. The queue still releases its own reference.
    reinterpret_cast<ClearCallback>(image + profile.ClearReadCallback)(watcher, nullptr, true);
    // A dispatcher pass can still hold the old object. Invalidate its copy, not the actual Windows socket.
    InterlockedExchange64(&Field<LONG64>(watcher, Socket), -1);
    InterlockedExchange(&Field<LONG>(watcher, State), Closed);
    reinterpret_cast<Unlink>(image + profile.UnlinkWatcher)(dispatcher, watcher);
    return Result::Detached;
}

