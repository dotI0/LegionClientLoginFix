#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "ClientProfile.h"
#ifdef WOW_LOGIN_REPAIR
#include "WatcherRepair.h"
#endif

namespace
{
    using Callback = int (*)(void*, std::uintptr_t, int, void*, void*);
    constexpr int PollRemove = 4;
    constexpr unsigned BufferCapacity = 256;
    constexpr unsigned FileCapacity = 20000;
    constexpr unsigned WatchCapacity = 64;
    constexpr unsigned TrackCapacity = 32;
    constexpr ULONGLONG TrackDuration = 35000;
    constexpr ULONGLONG FastSampleDuration = 500;
#ifdef WOW_LOGIN_REPAIR
    constexpr wchar_t LogName[] = L"WowLoginRepair-%lu.log";
    constexpr char TraceName[] = "downloader watcher repair (callback only)";
#else
    constexpr wchar_t LogName[] = L"WowLoginProbe-%lu.log";
    constexpr char TraceName[] = "downloader/dispatcher probe v2";
#endif

    struct WatchState
    {
        std::uintptr_t Address = 0;
        std::uintptr_t Socket = 0;
        std::uintptr_t Event = 0;
        std::uintptr_t Next = 0;
        int State = -1;
        bool CallbackPresent = false;
        bool External = false;
    };

    struct Record
    {
        ULONGLONG Tick;
        DWORD Thread;
        std::uintptr_t Socket;
        void* Watch;
        int Action;
        int Result;
        int QueueBefore;
        int QueueAfter;
        WatchState AtRemove;
#ifdef WOW_LOGIN_REPAIR
        WatcherRepairOutcome Repair = WatcherRepairOutcome::NotApplicable;
#endif
    };

    std::atomic<Callback> Original{ nullptr };
    std::atomic<bool> Started{ false };
    std::atomic<bool> Running{ false };
    std::atomic<unsigned> Dropped{ 0 };
    SRWLOCK BufferLock = SRWLOCK_INIT;
    Record Buffer[BufferCapacity]{};
    unsigned Buffered = 0;
    std::uintptr_t ImageBase = 0;
    WowLogin::ClientProfile const* ActiveProfile = nullptr;
    HANDLE Wake = nullptr;

    bool ReadBytes(std::uintptr_t address, void* result, SIZE_T size)
    {
        SIZE_T bytes = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void const*>(address), result, size, &bytes) &&
            bytes == size;
    }

    template<class T>
    bool Read(std::uintptr_t address, T& result)
    {
        return ReadBytes(address, &result, sizeof(result));
    }

    template<std::size_t N>
    bool MatchesGuards(std::uintptr_t base, WowLogin::Signature const (&guards)[N])
    {
        char bytes[32];
        for (WowLogin::Signature const& guard : guards)
            if (guard.Bytes.size() > sizeof(bytes) ||
                !ReadBytes(base + guard.Rva, bytes, guard.Bytes.size()) ||
                std::memcmp(bytes, guard.Bytes.data(), guard.Bytes.size()) != 0)
                return false;
        return true;
    }

    template<class T>
    T Field(unsigned char const* bytes, unsigned offset)
    {
        T value;
        std::memcpy(&value, bytes + offset, sizeof(value));
        return value;
    }

    bool ReadWatch(std::uintptr_t address, WatchState& watch)
    {
        constexpr unsigned Start = 0x90;
        unsigned char bytes[0x242 - Start];
        if (!address || address % alignof(void*) || !Read(address + Start, bytes) || bytes[0x241 - Start] > 1)
            return false;
        watch = { address, Field<std::uintptr_t>(bytes, 0), Field<std::uintptr_t>(bytes, 0x218 - Start),
            Field<std::uintptr_t>(bytes, 0x1D0 - Start), Field<int>(bytes, 0xA4 - Start),
            Field<std::uintptr_t>(bytes, 0xA8 - Start) != 0, bytes[0x241 - Start] != 0 };
        return true;
    }

    struct DispatcherHead
    {
        int LinkOffset;
        unsigned char Padding[12];
        std::uintptr_t First;
    };
    static_assert(offsetof(DispatcherHead, First) == 0x10 && sizeof(DispatcherHead) == 0x18);

    bool ReadDispatcher(WatchState (&list)[WatchCapacity], unsigned& count)
    {
        std::uintptr_t dispatcher = 0;
        DispatcherHead head{};
        if (!Read(ImageBase + ActiveProfile->DispatcherGlobal, dispatcher) || !dispatcher ||
            !Read(dispatcher + 0xC40, head) || head.LinkOffset != 0x1C8)
            return false;

        // The client can mutate the list while we sample it. Never take its locks or follow an unbounded list.
        count = 0;
        for (std::uintptr_t next = head.First; next && !(next & 1); next = list[count++].Next)
        {
            if (count == WatchCapacity || !ReadWatch(next, list[count]))
                return false;
            for (unsigned i = 0; i < count; ++i)
                if (list[i].Address == next)
                    return false;
        }

        DispatcherHead after{};
        std::uintptr_t stillDispatcher = 0;
        return Read(dispatcher + 0xC40, after) && after.First == head.First && after.LinkOffset == head.LinkOffset &&
            Read(ImageBase + ActiveProfile->DispatcherGlobal, stillDispatcher) && stillDispatcher == dispatcher;
    }

    struct RemovalQueue
    {
        std::uint32_t Count;
        std::uint32_t Padding;
        std::uintptr_t Entries;
    };
    static_assert(offsetof(RemovalQueue, Entries) == 8 && sizeof(RemovalQueue) == 0x10);

    bool ReadQueue(std::uintptr_t (&entries)[WatchCapacity], unsigned& count)
    {
        std::uintptr_t downloader = 0, stillDownloader = 0;
        RemovalQueue queue{}, after{};
        if (!Read(ImageBase + ActiveProfile->DownloaderGlobal, downloader) || !downloader ||
            !Read(downloader + 0xC8, queue) || queue.Count > WatchCapacity ||
            (queue.Count && (!queue.Entries || !ReadBytes(queue.Entries, entries, queue.Count * sizeof(entries[0])))) ||
            !Read(downloader + 0xC8, after) || after.Count != queue.Count || after.Entries != queue.Entries ||
            !Read(ImageBase + ActiveProfile->DownloaderGlobal, stillDownloader) || stillDownloader != downloader)
            return false;
        count = queue.Count;
        return true;
    }

    int PendingRemovals(void* downloader)
    {
        int count = 0;
        return downloader && Read(reinterpret_cast<std::uintptr_t>(downloader) + 0xC8, count) ? count : -1;
    }

    int Observe(void* easy, std::uintptr_t socket, int action, void* downloader, void* watch)
    {
        Callback const original = Original.load(std::memory_order_acquire);
        bool const recording = Running.load(std::memory_order_relaxed);
#ifndef WOW_LOGIN_REPAIR
        if (!recording)
            return original(easy, socket, action, downloader, watch);
#endif

        DWORD const saved = GetLastError();
        Record record{};
        if (recording)
        {
            record = { GetTickCount64(), GetCurrentThreadId(), socket, watch, action, 0,
                action == PollRemove ? PendingRemovals(downloader) : -1, -1, {} };
            if (action == PollRemove && watch)
                ReadWatch(reinterpret_cast<std::uintptr_t>(watch), record.AtRemove);
        }
#ifdef WOW_LOGIN_REPAIR
        if (action == PollRemove && watch)
            record.Repair = RepairDownloaderWatcher(ImageBase, *ActiveProfile, downloader, socket, watch);
#endif
        SetLastError(saved);
        int const result = original(easy, socket, action, downloader, watch);
        DWORD const originalError = GetLastError();

        if (recording && Running.load(std::memory_order_relaxed))
        {
            record.Result = result;
            if (action == PollRemove)
                record.QueueAfter = PendingRemovals(downloader);
            if (TryAcquireSRWLockExclusive(&BufferLock))
            {
                if (Buffered < BufferCapacity)
                    Buffer[Buffered++] = record;
                else
                    Dropped.fetch_add(1, std::memory_order_relaxed);
                ReleaseSRWLockExclusive(&BufferLock);
            }
            else
                Dropped.fetch_add(1, std::memory_order_relaxed);
            if (record.AtRemove.Address)
                SetEvent(Wake);
        }
        SetLastError(originalError);
        return result;
    }

    bool Write(HANDLE file, char const* text, int length)
    {
        DWORD written = 0;
        return length > 0 && WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr) &&
            written == static_cast<DWORD>(length);
    }

    struct Output
    {
        HANDLE File;
        unsigned Count = 0;

        void Line(char const* format, ...)
        {
            if (Count == FileCapacity || !Running.load(std::memory_order_relaxed))
                return;
            char line[320];
            va_list args;
            va_start(args, format);
            int const length = std::vsnprintf(line, sizeof(line), format, args);
            va_end(args);
            if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(line) || !Write(File, line, length))
                Running.store(false, std::memory_order_relaxed);
            else
                ++Count;
        }
    };

    struct Track
    {
        WatchState Before;
        ULONGLONG RemoveTick = 0;
        ULONGLONG NextReport = 0;
        WatchState Last;
        int Queue = -2;
        int Listed = -2;
    };

    struct Overlap
    {
        std::uintptr_t External = 0;
        std::uintptr_t Normal = 0;
        std::uintptr_t Socket = 0;
        std::uintptr_t ExternalEvent = 0;
        std::uintptr_t NormalEvent = 0;
        ULONGLONG Since = 0;
        ULONGLONG NextReport = 0;

        bool SameAs(Overlap const& other) const
        {
            return External == other.External && Normal == other.Normal && Socket == other.Socket &&
                ExternalEvent == other.ExternalEvent && NormalEvent == other.NormalEvent;
        }
    };

    void Sample(Output& out, Track (&tracked)[TrackCapacity], Overlap (&previous)[WatchCapacity],
        unsigned& previousCount, bool& dispatcherSeen, ULONGLONG now)
    {
        WatchState list[WatchCapacity];
        unsigned count = 0;
        if (!ReadDispatcher(list, count))
            return;
        if (!dispatcherSeen)
        {
            out.Line("dispatcher tick=%llu entries=%u\r\n", now, count);
            dispatcherSeen = true;
        }

        std::uintptr_t entries[WatchCapacity]{};
        unsigned queued = 0;
        bool haveQueue = false;
        for (Track const& track : tracked)
            if (track.Before.Address && now - track.RemoveTick <= TrackDuration)
            {
                haveQueue = ReadQueue(entries, queued);
                break;
            }

        for (Track& track : tracked)
        {
            if (!track.Before.Address)
                continue;
            if (now - track.RemoveTick > TrackDuration)
            {
                track = {};
                continue;
            }

            WatchState current;
            int listed = 0;
            for (unsigned i = 0; i < count; ++i)
                if (list[i].Address == track.Before.Address)
                {
                    current = list[i];
                    listed = current.Event == track.Before.Event ? 1 : 2;
                    break;
                }
            int inQueue = haveQueue ? 0 : -1;
            for (unsigned i = 0; i < queued && haveQueue; ++i)
                if (entries[i] == track.Before.Address)
                    inQueue = 1;

            if (track.Queue == inQueue && track.Listed == listed && track.Last.State == current.State &&
                track.Last.Socket == current.Socket && track.Last.Event == current.Event &&
                track.Last.CallbackPresent == current.CallbackPresent && now < track.NextReport)
                continue;

            out.Line("sample tick=%llu remove_tick=%llu watch=%p age_ms=%llu in_queue=%d listed=%d "
                "state=%d socket=0x%llX event=0x%llX callback=%u\r\n",
                now, track.RemoveTick, reinterpret_cast<void*>(track.Before.Address), now - track.RemoveTick,
                inQueue, listed, current.State, static_cast<unsigned long long>(current.Socket),
                static_cast<unsigned long long>(current.Event), unsigned(current.CallbackPresent));
            track.NextReport = now + 3000;
            track.Last = current;
            track.Queue = inQueue;
            track.Listed = listed;
        }

        Overlap active[WatchCapacity];
        unsigned activeCount = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            WatchState const& external = list[i];
            if (!external.External || external.CallbackPresent || !external.Socket || external.Socket == static_cast<std::uintptr_t>(-1))
                continue;
            for (unsigned j = 0; j < count; ++j)
            {
                WatchState const& other = list[j];
                if (other.External || other.Socket != external.Socket || other.Event == external.Event)
                    continue;
                Overlap overlap{ external.Address, other.Address, external.Socket, external.Event, other.Event, now, now };
                for (unsigned k = 0; k < previousCount; ++k)
                    if (overlap.SameAs(previous[k]))
                    {
                        overlap.Since = previous[k].Since;
                        overlap.NextReport = previous[k].NextReport;
                        break;
                    }
                if (now >= overlap.NextReport)
                {
                    out.Line("overlap tick=%llu age_ms=%llu socket=0x%llX external=%p external_state=%d external_event=0x%llX "
                        "normal=%p normal_state=%d normal_event=0x%llX\r\n",
                        now, now - overlap.Since, static_cast<unsigned long long>(overlap.Socket), reinterpret_cast<void*>(overlap.External),
                        external.State, static_cast<unsigned long long>(overlap.ExternalEvent),
                        reinterpret_cast<void*>(overlap.Normal), other.State, static_cast<unsigned long long>(overlap.NormalEvent));
                    overlap.NextReport = now + 3000;
                }
                active[activeCount++] = overlap;
                break;
            }
        }
        for (unsigned i = 0; i < previousCount; ++i)
        {
            bool present = false;
            for (unsigned j = 0; j < activeCount && !present; ++j)
                present = previous[i].SameAs(active[j]);
            if (!present)
                out.Line("overlap-ended tick=%llu age_ms=%llu socket=0x%llX external=%p normal=%p\r\n",
                    now, now - previous[i].Since, static_cast<unsigned long long>(previous[i].Socket),
                    reinterpret_cast<void*>(previous[i].External), reinterpret_cast<void*>(previous[i].Normal));
        }
        std::memcpy(previous, active, sizeof(Overlap) * activeCount);
        previousCount = activeCount;
    }

    DWORD WINAPI WriteRecords(void* data)
    {
        HANDLE const file = data;
        Output out{ file };
        Track tracked[TrackCapacity]{};
        Overlap previous[WatchCapacity]{};
        unsigned trackIndex = 0;
        unsigned previousCount = 0;
        bool dispatcherSeen = false;
        ULONGLONG nextSample = 0;
        ULONGLONG fastUntil = 0;
        while (Running.load(std::memory_order_relaxed) && out.Count < FileCapacity)
        {
            Record batch[BufferCapacity];
            AcquireSRWLockExclusive(&BufferLock);
            unsigned const size = Buffered;
            std::memcpy(batch, Buffer, sizeof(Record) * size);
            Buffered = 0;
            ReleaseSRWLockExclusive(&BufferLock);

            unsigned const dropped = Dropped.exchange(0, std::memory_order_relaxed);
            if (dropped)
                out.Line("dropped=%u\r\n", dropped);
            for (unsigned i = 0; i < size; ++i)
            {
                Record const& r = batch[i];
                out.Line("callback tick=%llu tid=%lu socket=0x%llX action=%d watch=%p result=%d "
                    "pending_before=%d pending_after=%d state_at_remove=%d watch_socket=0x%llX watch_event=0x%llX\r\n",
                    r.Tick, r.Thread, static_cast<unsigned long long>(r.Socket), r.Action, r.Watch,
                    r.Result, r.QueueBefore, r.QueueAfter, r.AtRemove.State,
                    static_cast<unsigned long long>(r.AtRemove.Socket), static_cast<unsigned long long>(r.AtRemove.Event));
#ifdef WOW_LOGIN_REPAIR
                if (r.Repair != WatcherRepairOutcome::NotApplicable)
                {
                    char const* result = r.Repair == WatcherRepairOutcome::Detached ? "detached" :
                        (r.Repair == WatcherRepairOutcome::Busy ? "busy" : "guard-rejected");
                    out.Line("repair tick=%llu watch=%p socket=0x%llX result=%s\r\n",
                        r.Tick, r.Watch, static_cast<unsigned long long>(r.Socket), result);
                }
#endif
                if (r.AtRemove.Address)
                {
                    tracked[trackIndex] = { r.AtRemove, r.Tick, 0, {}, -2, -2 };
                    trackIndex = (trackIndex + 1) % TrackCapacity;
                    fastUntil = r.Tick + FastSampleDuration;
                    nextSample = 0;
                }
            }

            ULONGLONG const now = GetTickCount64();
            if (now >= nextSample)
            {
                Sample(out, tracked, previous, previousCount, dispatcherSeen, now);
                nextSample = now + (now < fastUntil ? 10 : 100);
            }
            WaitForSingleObject(Wake, now < fastUntil ? 10 : 100);
        }
        if (out.Count == FileCapacity)
        {
            constexpr char limit[] = "record limit reached; logging stopped\r\n";
            Write(file, limit, sizeof(limit) - 1);
        }
        Running.store(false, std::memory_order_relaxed);
        CloseHandle(file);
        return 0;
    }

    HANDLE OpenLog(HMODULE module)
    {
        wchar_t path[MAX_PATH]{};
        DWORD const length = GetModuleFileNameW(module, path, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return INVALID_HANDLE_VALUE;
        wchar_t* const slash = std::wcsrchr(path, L'\\');
        if (!slash || swprintf_s(slash + 1, MAX_PATH - (slash + 1 - path), LogName, GetCurrentProcessId()) < 0)
            return INVALID_HANDLE_VALUE;
        return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    DWORD StartLogging(HMODULE module)
    {
        HANDLE const log = OpenLog(module);
        if (log == INVALID_HANDLE_VALUE)
            return 6;
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        char header[200];
        int const length = sprintf_s(header, sizeof(header),
            "WoW 7.3.5.%u %s; UTC=%04u-%02u-%02uT%02u:%02u:%02uZ tick=%llu; action 4=REMOVE\r\n",
            ActiveProfile->Build, TraceName, unsigned(utc.wYear), unsigned(utc.wMonth), unsigned(utc.wDay), unsigned(utc.wHour),
            unsigned(utc.wMinute), unsigned(utc.wSecond), GetTickCount64());
        if (!Write(log, header, length))
        {
            CloseHandle(log);
            return 6;
        }

        // A callback can still signal the event while the writer is stopping.
        Wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Running.store(true, std::memory_order_relaxed);
        HANDLE const writer = Wake ? CreateThread(nullptr, 0, WriteRecords, log, 0, nullptr) : nullptr;
        if (!writer)
        {
            Running.store(false, std::memory_order_relaxed);
            if (Wake)
            {
                CloseHandle(Wake);
                Wake = nullptr;
            }
            CloseHandle(log);
            return 7;
        }
        CloseHandle(writer);
        return 0;
    }

    bool Install(void* volatile* slot, void* replacement, void* expected)
    {
        __try
        {
            return InterlockedCompareExchangePointer(slot, replacement, expected) == expected;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI WowLoginRepairStart(void* options)
{
    if (Started.load())
        return 2;

    auto const base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!base)
        return 3;
    WowLogin::ClientProfile const* profile = nullptr;
    DWORD signatureError = 3;
    for (WowLogin::ClientProfile const& candidate : WowLogin::Profiles)
    {
        std::array<unsigned char, WowLogin::ThunkBytes.size()> thunk{};
        if (!Read(base + candidate.CallbackThunk, thunk) || thunk != WowLogin::ThunkBytes ||
            !MatchesGuards(base, candidate.DownloaderGuards))
            continue;
#ifdef WOW_LOGIN_REPAIR
        if (!MatchesGuards(base, candidate.RepairGuards))
        {
            signatureError = 9;
            continue;
        }
#endif
        profile = &candidate;
        break;
    }
    if (!profile)
        return signatureError;

    std::uintptr_t downloader = 0, multi = 0, context = 0, callback = 0;
    DWORD magic = 0;
    if (!Read(base + profile->DownloaderGlobal, downloader) || !downloader || !Read(downloader + 8, multi) || !multi ||
        !Read(multi, magic) || magic != 0xBAB1E || !Read(multi + 0x30, context) || context != downloader ||
        !Read(multi + 0x28, callback) || callback != base + profile->CallbackThunk)
        return 4;

    auto const slot = reinterpret_cast<void* volatile*>(multi + 0x28);
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<void const*>(multi + 0x28), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE || memory.Protect != PAGE_READWRITE ||
        (multi + 0x28) % alignof(void*) != 0)
        return 5;

    if (Started.exchange(true))
        return 2;
    ImageBase = base;
    ActiveProfile = profile;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&BufferLock), &module))
        return 10;
    if (options)
        if (DWORD const error = StartLogging(module))
            return error;

    Original.store(reinterpret_cast<Callback>(callback), std::memory_order_release);
    if (!Install(slot, reinterpret_cast<void*>(&Observe), reinterpret_cast<void*>(callback)))
    {
        Running.store(false, std::memory_order_relaxed);
        if (Wake)
            SetEvent(Wake);
        return 8;
    }
    return 1;
}
