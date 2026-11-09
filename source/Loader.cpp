#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include "Cache.h"
#include "ClientProfile.h"

static_assert(sizeof(void*) == 8, "Build with the x64 toolchain");

namespace
{
    wchar_t constexpr ClientName[] = L"Wow-64.exe";
    wchar_t constexpr ProbeName[] = L"WowLoginRepair.dll";
    constexpr ULONGLONG StartupWaitMs = 90000;

    struct Handle
    {
        explicit Handle(HANDLE value) : Value(value) { }
        ~Handle() { if (*this) CloseHandle(Value); }
        Handle(Handle const&) = delete;
        Handle& operator=(Handle const&) = delete;
        explicit operator bool() const { return Value && Value != INVALID_HANDLE_VALUE; }
        HANDLE const Value;
    };

    struct StartupLog
    {
        ~StartupLog() { if (File) std::fclose(File); }

        bool Open(std::wstring const& directory)
        {
            wchar_t name[48]{};
            swprintf_s(name, _countof(name), L"WowLoginLauncher-%lu.log", GetCurrentProcessId());
            return _wfopen_s(&File, (directory + name).c_str(), L"w, ccs=UTF-8") == 0;
        }

        void Write(wchar_t const* message) const
        {
            if (!File)
                return;
            SYSTEMTIME utc{};
            GetSystemTime(&utc);
            std::fwprintf(File, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ %ls\n",
                unsigned(utc.wYear), unsigned(utc.wMonth), unsigned(utc.wDay), unsigned(utc.wHour),
                unsigned(utc.wMinute), unsigned(utc.wSecond), unsigned(utc.wMilliseconds), message);
            std::fflush(File);
        }

        std::FILE* File = nullptr;
    };

    StartupLog Log;

    int Fail(wchar_t const* operation, DWORD error = ERROR_SUCCESS)
    {
        wchar_t message[384]{};
        if (error)
            swprintf_s(message, _countof(message), L"%ls (Windows error %lu).", operation, error);
        else
            swprintf_s(message, _countof(message), L"%ls.", operation);
        Log.Write(message);
        std::fwprintf(stderr, L"%ls\n", message);
        MessageBoxW(nullptr, message, L"WoW Login Fix", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::wstring Parent(std::wstring const& path)
    {
        auto const slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash + 1);
    }

    bool IsFile(std::wstring const& path)
    {
        DWORD const attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::wstring FindClient(std::wstring const& directory, DWORD& error)
    {
        std::wstring const beside = directory + ClientName;
        if (IsFile(beside))
            return beside;

        std::wstring const parent = Parent(directory.substr(0, directory.size() - 1));
        if (!parent.empty() && IsFile(parent + ClientName))
            return parent + ClientName;

        wchar_t path[MAX_PATH]{};
        OPENFILENAMEW picker{};
        picker.lStructSize = sizeof(picker);
        picker.lpstrFilter = L"WoW client (*.exe)\0*.exe\0\0";
        picker.lpstrFile = path;
        picker.nMaxFile = _countof(path);
        picker.lpstrTitle = L"Select your WoW client executable";
        picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
        if (GetOpenFileNameW(&picker))
            return path;
        error = CommDlgExtendedError();
        return {};
    }

    std::wstring QuoteArgument(std::wstring const& argument)
    {
        std::wstring quoted = L"\"";
        std::size_t backslashes = 0;
        for (wchar_t const character : argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            quoted.append(character == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
            quoted += character;
            backslashes = 0;
        }
        quoted.append(backslashes * 2, L'\\');
        quoted += L'"';
        return quoted;
    }

    bool ReadModules(DWORD pid, std::vector<MODULEENTRY32W>& modules)
    {
        modules.clear();
        for (unsigned attempt = 0; attempt < 5; ++attempt)
        {
            Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
            if (!snapshot)
            {
                if (GetLastError() == ERROR_BAD_LENGTH)
                {
                    Sleep(10);
                    continue;
                }
                return false;
            }

            MODULEENTRY32W module{};
            module.dwSize = sizeof(module);
            if (!Module32FirstW(snapshot.Value, &module))
                return false;

            do
            {
                modules.push_back(module);
            } while (Module32NextW(snapshot.Value, &module));
            return GetLastError() == ERROR_NO_MORE_FILES;
        }
        return false;
    }

    template<class T>
    bool ReadRemote(HANDLE process, std::uintptr_t address, T& value)
    {
        SIZE_T read = 0;
        return ReadProcessMemory(process, reinterpret_cast<void const*>(address), &value, sizeof(value), &read) &&
            read == sizeof(value);
    }

    std::uintptr_t ReadyDownloader(HANDLE process, MODULEENTRY32W const& image,
        WowLogin::ClientProfile const*& profile, wchar_t const*& detail)
    {
        auto const base = reinterpret_cast<std::uintptr_t>(image.modBaseAddr);
        detail = L"The downloader callback signature does not match a supported build.";
        for (WowLogin::ClientProfile const& candidate : WowLogin::Profiles)
        {
            std::array<unsigned char, WowLogin::ThunkBytes.size()> thunk{};
            if (image.modBaseSize < candidate.DownloaderGlobal + sizeof(std::uintptr_t) ||
                image.modBaseSize < candidate.CallbackThunk + thunk.size() ||
                !ReadRemote(process, base + candidate.CallbackThunk, thunk) || thunk != WowLogin::ThunkBytes)
                continue;

            std::uintptr_t downloader = 0, multi = 0, callback = 0, context = 0;
            DWORD magic = 0;
            if (!ReadRemote(process, base + candidate.DownloaderGlobal, downloader) || !downloader ||
                !ReadRemote(process, downloader + 8, multi) || !multi)
            {
                detail = L"The downloader was not initialized or could not be read.";
                continue;
            }
            if (!ReadRemote(process, multi, magic) || magic != 0xBAB1E ||
                !ReadRemote(process, multi + 0x28, callback) || callback != base + candidate.CallbackThunk ||
                !ReadRemote(process, multi + 0x30, context) || context != downloader)
            {
                detail = L"The downloader state or callback layout does not match.";
                continue;
            }
            profile = &candidate;
            detail = L"The downloader did not remain ready across consecutive checks.";
            return multi;
        }
        return 0;
    }

    WowLogin::ClientProfile const* WaitForClient(HANDLE process, DWORD pid, wchar_t const* imageName)
    {
        std::uintptr_t previous = 0;
        WowLogin::ClientProfile const* previousProfile = nullptr;
        std::vector<MODULEENTRY32W> modules;
        wchar_t const* detail = nullptr;
        ULONGLONG const deadline = GetTickCount64() + StartupWaitMs;
        for (;;)
        {
            std::uintptr_t ready = 0;
            WowLogin::ClientProfile const* profile = nullptr;
            detail = L"Could not enumerate the client's loaded modules.";
            if (ReadModules(pid, modules))
            {
                detail = L"The selected executable was not found among the client's loaded modules.";
                for (MODULEENTRY32W const& module : modules)
                    if (_wcsicmp(module.szModule, imageName) == 0)
                    {
                        ready = ReadyDownloader(process, module, profile, detail);
                        break;
                    }
            }
            if (ready && ready == previous && profile == previousProfile)
                return profile;
            previous = ready;
            previousProfile = profile;
            DWORD const wait = WaitForSingleObject(process, 200);
            if (wait != WAIT_TIMEOUT || GetTickCount64() >= deadline)
            {
                Log.Write(detail);
                Log.Write(wait == WAIT_OBJECT_0 ? L"The client exited before the fix could be loaded." :
                    wait == WAIT_TIMEOUT ? L"Timed out waiting for a compatible downloader." :
                    L"Could not wait for the client process.");
                break;
            }
        }
        return nullptr;
    }

    int StartProbe(HANDLE process, MODULEENTRY32W const& module, wchar_t const* path, bool log)
    {
        Log.Write(L"Repair DLL is loaded in WoW. Checking its entry point.");
        // The probe only installs its callback when the explicit entry point is invoked.
        HMODULE local = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!local)
            return Fail(L"Could not inspect the probe DLL", GetLastError());
        FARPROC const entry = GetProcAddress(local, "WowLoginRepairStart");
        if (!entry)
        {
            FreeLibrary(local);
            return Fail(L"The DLL and starter do not match; rebuild both with source\\build.cmd");
        }
        auto const rva = reinterpret_cast<std::uintptr_t>(entry) - reinterpret_cast<std::uintptr_t>(local);
        constexpr SIZE_T SignatureSize = 16;
        unsigned char signature[SignatureSize]{};
        SIZE_T read = 0;
        auto const remote = reinterpret_cast<std::uintptr_t>(module.modBaseAddr) + rva;
        bool const matched = module.modBaseSize >= SignatureSize && rva <= module.modBaseSize - SignatureSize &&
            ReadProcessMemory(process, reinterpret_cast<void const*>(remote), signature, sizeof(signature), &read) &&
            read == sizeof(signature) && std::memcmp(signature, reinterpret_cast<void const*>(entry), sizeof(signature)) == 0;
        FreeLibrary(local);
        if (!matched)
            return Fail(L"The loaded probe does not match this DLL; restart the test client");

        Log.Write(L"Activating the login fix.");
        void* const options = reinterpret_cast<void*>(static_cast<std::uintptr_t>(log));
        Handle start(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(remote), options, 0, nullptr));
        if (!start)
            return Fail(L"Could not start diagnostics in the client", GetLastError());
        if (WaitForSingleObject(start.Value, 10000) != WAIT_OBJECT_0)
            return Fail(L"Diagnostics did not initialize; close the test client normally");
        DWORD status = 0;
        if (!GetExitCodeThread(start.Value, &status))
            return Fail(L"Could not read the diagnostic status", GetLastError());
        if (status == 1)
        {
            if (log)
            {
                wchar_t message[96]{};
                swprintf_s(message, _countof(message), L"WoW login fix active. DLL log: WowLoginRepair-%lu.log",
                    module.th32ProcessID);
                Log.Write(message);
                std::wprintf(L"%ls\n", message);
            }
            else
                std::wprintf(L"WoW login fix active. Logging disabled.\n");
            return 0;
        }
        switch (status)
        {
            case 2: return Fail(L"Diagnostics were already started; restart WoW for a fresh trace");
            case 3: return Fail(L"Runtime code signature mismatch; no callback changed");
            case 4: return Fail(L"Downloader not initialized or unexpected layout; no callback changed");
            case 5: return Fail(L"Unexpected callback memory protection; no callback changed");
            case 6: return Fail(L"Could not open or write the diagnostic log; no callback changed");
            case 7: return Fail(L"Could not start the log writer; no callback changed");
            case 8: return Fail(L"Callback changed before installation; no callback changed by the probe");
            case 9: return Fail(L"Watcher repair signature mismatch; nothing was installed");
            case 10: return Fail(L"Could not retain the repair DLL; nothing was installed");
            default:
                wchar_t message[64]{};
                swprintf_s(message, _countof(message), L"Unexpected diagnostic exit status: 0x%08lX.", status);
                Log.Write(message);
                std::fwprintf(stderr, L"%ls\n", message);
                return 1;
        }
    }
}

int wmain(int argc, wchar_t* argv[])
{
    wchar_t launcherPath[MAX_PATH]{};
    DWORD const launcherLength = GetModuleFileNameW(nullptr, launcherPath, _countof(launcherPath));
    if (!launcherLength || launcherLength >= _countof(launcherPath))
        return Fail(L"Could not resolve the launcher directory");
    std::wstring const directory = Parent(launcherPath);
    if (directory.empty())
        return Fail(L"Could not resolve the launcher directory");
    std::wstring selection;
    bool log = false, high = false, clearCache = false, forwardOnly = false;
    bool missingClientPath = false;
    std::vector<wchar_t const*> gameArguments;
    for (int i = 1; i < argc; ++i)
    {
        wchar_t const* const option = argv[i];
        if (forwardOnly)
            gameArguments.push_back(option);
        else if (std::wcscmp(option, L"--") == 0)
            forwardOnly = true;
        else if (std::wcscmp(option, L"-log") == 0)
            log = true;
        else if (std::wcscmp(option, L"-high") == 0)
            high = true;
        else if (std::wcscmp(option, L"-clear-cache") == 0)
            clearCache = true;
        else if (std::wcscmp(option, L"--client") == 0)
        {
            if (++i == argc || !argv[i][0])
                missingClientPath = true;
            else
                selection = argv[i];
        }
        else
            gameArguments.push_back(option);
    }
    if (log && !Log.Open(directory))
        MessageBoxW(nullptr, L"Could not create the launcher log next to the DLL. Starting anyway.",
            L"WoW Login Fix", MB_OK | MB_ICONWARNING);
    Log.Write(L"Launcher started with -log.");
    if (missingClientPath)
        return Fail(L"--client requires a path to the WoW executable");
    std::wstring const probePath = directory + ProbeName;
    if (probePath.size() >= MAX_PATH || !IsFile(probePath))
        return Fail(L"WowLoginRepair.dll must be next to the launcher");

    if (selection.empty())
    {
        DWORD pickerError = ERROR_SUCCESS;
        selection = FindClient(directory, pickerError);
        if (selection.empty())
        {
            if (pickerError)
                return Fail(L"Could not select the WoW executable", pickerError);
            Log.Write(L"Client selection cancelled. WoW was not started.");
            return 0;
        }
    }

    wchar_t fullPath[MAX_PATH]{};
    wchar_t* leaf = nullptr;
    DWORD const fullLength = GetFullPathNameW(selection.c_str(), _countof(fullPath), fullPath, &leaf);
    if (!fullLength || fullLength >= _countof(fullPath) || !leaf ||
        std::wcslen(leaf) <= 4 || _wcsicmp(leaf + std::wcslen(leaf) - 4, L".exe") != 0)
        return Fail(L"Select a WoW executable using a standard Windows path");
    std::wstring const gameDirectory = Parent(fullPath);
    if (gameDirectory.empty())
        return Fail(L"Could not resolve the WoW working directory");

    std::wstring command = QuoteArgument(fullPath);
    for (wchar_t const* argument : gameArguments)
    {
        command += L' ';
        command += QuoteArgument(argument);
    }

    PROCESS_INFORMATION created{};
    if (clearCache)
    {
        Log.Write(L"Checking the client cache (-clear-cache).");
        Handle image(CreateFileW(fullPath, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!image)
            return Fail(L"Could not open the WoW executable", GetLastError());
        if (DWORD const error = ClearClientCache(image.Value, gameDirectory.c_str(), leaf))
            return Fail(L"Could not safely remove the WoW Cache folder; WoW was not started. Close other WoW clients or omit -clear-cache", error);
        Log.Write(L"The client cache is absent.");
    }
    Log.Write(high ? L"Starting WoW with high process priority." : L"Starting WoW without a priority override.");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(fullPath, command.data(), nullptr, nullptr, FALSE, high ? HIGH_PRIORITY_CLASS : 0, nullptr,
        gameDirectory.c_str(), &startup, &created))
        return Fail(L"Could not start the WoW executable", GetLastError());

    Handle process(created.hProcess);
    CloseHandle(created.hThread);
    DWORD const pid = created.dwProcessId;
    if (log)
    {
        wchar_t message[96]{};
        swprintf_s(message, _countof(message), L"WoW started (PID %lu). Waiting for its downloader.", pid);
        Log.Write(message);
    }
    BOOL wow64 = FALSE;
    if (IsWow64Process(process.Value, &wow64) && wow64)
        return Fail(L"This client is 32-bit; WoW was started without the login fix");
    WowLogin::ClientProfile const* const profile = WaitForClient(process.Value, pid, leaf);
    if (!profile)
        return Fail(L"No compatible downloader was detected; the login fix was not activated");

    if (log)
    {
        wchar_t message[112]{};
        swprintf_s(message, _countof(message), L"Compatible downloader detected (Legion 7.3.5.%u). Preparing to load the repair DLL.",
            profile->Build);
        Log.Write(message);
    }
    FARPROC const localLoadLibrary = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner = nullptr;
    if (!localLoadLibrary || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(localLoadLibrary), &owner))
        return Fail(L"Could not resolve the local LoadLibraryW implementation", GetLastError());
    wchar_t ownerPath[MAX_PATH]{};
    DWORD const ownerLength = GetModuleFileNameW(owner, ownerPath, MAX_PATH);
    if (!ownerLength || ownerLength >= MAX_PATH)
        return Fail(L"Could not resolve the system module path");

    std::vector<MODULEENTRY32W> modules;
    if (!ReadModules(pid, modules))
        return Fail(L"Could not inspect the client modules");

    // LoadLibraryW can be forwarded to another system DLL. Use its real owner and the target's base.
    auto const rva = reinterpret_cast<std::uintptr_t>(localLoadLibrary) - reinterpret_cast<std::uintptr_t>(owner);
    LPTHREAD_START_ROUTINE remoteLoadLibrary = nullptr;
    for (MODULEENTRY32W const& module : modules)
    {
        if (_wcsicmp(module.szModule, ProbeName) == 0)
        {
            if (_wcsicmp(module.szExePath, probePath.c_str()) != 0)
                return Fail(L"A different DLL with the same name is already loaded");
            return StartProbe(process.Value, module, probePath.c_str(), log);
        }
        if (_wcsicmp(module.szExePath, ownerPath) == 0 && rva < module.modBaseSize)
            remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<std::uintptr_t>(module.modBaseAddr) + rva);
    }
    if (!remoteLoadLibrary)
        return Fail(L"Could not find the matching system module in the client");

    SIZE_T const pathBytes = (probePath.size() + 1) * sizeof(wchar_t);
    void* const remotePath = VirtualAllocEx(process.Value, nullptr, pathBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remotePath)
        return Fail(L"Could not reserve space for the probe path", GetLastError());

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(process.Value, remotePath, probePath.c_str(), pathBytes, &bytesWritten) || bytesWritten != pathBytes)
    {
        DWORD const error = GetLastError();
        VirtualFreeEx(process.Value, remotePath, 0, MEM_RELEASE);
        return Fail(L"Could not pass the probe path to the client", error);
    }

    Handle thread(CreateRemoteThread(process.Value, nullptr, 0, remoteLoadLibrary, remotePath, 0, nullptr));
    if (!thread)
    {
        DWORD const error = GetLastError();
        VirtualFreeEx(process.Value, remotePath, 0, MEM_RELEASE);
        return Fail(L"Windows did not start the probe loader thread", error);
    }

    DWORD const wait = WaitForSingleObject(thread.Value, 10000);
    if (wait != WAIT_OBJECT_0)
    {
        // The thread can still be using the path. Do not free it or terminate the client.
        return Fail(L"Probe load did not finish; close the test client normally before retrying",
            wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS);
    }
    VirtualFreeEx(process.Value, remotePath, 0, MEM_RELEASE);

    // The thread exit code cannot hold a full x64 module handle; check the loaded module instead.
    if (!ReadModules(pid, modules))
        return Fail(L"Could not verify the probe; check whether the test client closed");
    for (MODULEENTRY32W const& module : modules)
    {
        if (_wcsicmp(module.szExePath, probePath.c_str()) == 0)
            return StartProbe(process.Value, module, probePath.c_str(), log);
    }
    return Fail(L"The probe was not loaded; do not retry with elevated permissions");
}
