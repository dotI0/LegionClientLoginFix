#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwchar>
#include <string>
#include "Cache.h"

namespace
{
    struct File
    {
        explicit File(HANDLE handle) : Value(handle) { }
        ~File() { if (Value != INVALID_HANDLE_VALUE) CloseHandle(Value); }
        File(File const&) = delete;
        File& operator=(File const&) = delete;
        HANDLE const Value;
    };

    struct Search
    {
        explicit Search(HANDLE handle) : Value(handle) { }
        ~Search() { if (Value != INVALID_HANDLE_VALUE) FindClose(Value); }
        Search(Search const&) = delete;
        Search& operator=(Search const&) = delete;
        HANDLE const Value;
    };

    DWORD FinalPath(HANDLE file, std::wstring& path)
    {
        DWORD const size = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!size)
            return GetLastError();
        path.resize(size);
        DWORD const written = GetFinalPathNameByHandleW(file, path.data(), size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!written)
            return GetLastError();
        if (written >= size)
            return ERROR_INSUFFICIENT_BUFFER;
        path.resize(written);
        return ERROR_SUCCESS;
    }

    bool Missing(DWORD error)
    {
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    DWORD RemoveCacheEntry(std::wstring const& path, unsigned depth)
    {
        if (depth > 64)
            return ERROR_DIRECTORY;
        // Open the link itself, not its target, and keep parent directories open without delete sharing.
        File entry(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (entry.Value == INVALID_HANDLE_VALUE)
        {
            DWORD const error = GetLastError();
            return Missing(error) ? ERROR_SUCCESS : error;
        }

        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(entry.Value, FileAttributeTagInfo, &attributes, sizeof(attributes)))
            return GetLastError();
        if (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return ERROR_NOT_SUPPORTED;
        bool const directory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!depth && !directory)
            return ERROR_DIRECTORY;

        std::wstring final;
        if (DWORD const error = FinalPath(entry.Value, final))
            return error;
        if (_wcsicmp(path.c_str(), final.c_str()) != 0)
            return ERROR_INVALID_NAME;

        if (directory)
        {
            WIN32_FIND_DATAW data{};
            std::wstring const children = final + L"\\";
            Search search(FindFirstFileW((children + L"*").c_str(), &data));
            if (search.Value == INVALID_HANDLE_VALUE)
            {
                DWORD const error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND)
                    return error;
            }
            else
            {
                do
                {
                    if (std::wcscmp(data.cFileName, L".") == 0 || std::wcscmp(data.cFileName, L"..") == 0)
                        continue;
                    if (DWORD const error = RemoveCacheEntry(children + data.cFileName, depth + 1))
                        return error;
                } while (FindNextFileW(search.Value, &data));
                DWORD const error = GetLastError();
                if (error != ERROR_NO_MORE_FILES)
                    return error;
            }
        }

        FILE_DISPOSITION_INFO disposition{ TRUE };
        return SetFileInformationByHandle(entry.Value, FileDispositionInfo, &disposition, sizeof(disposition)) ?
            ERROR_SUCCESS : GetLastError();
    }
}

DWORD ClearClientCache(HANDLE clientImage, wchar_t const* clientDirectory, wchar_t const* clientName)
{
    // Use the selected executable handle to anchor deletion to the actual installation.
    std::wstring path = clientDirectory;
    if (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    File directory(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (directory.Value == INVALID_HANDLE_VALUE)
        return GetLastError();

    std::wstring root, image;
    if (DWORD const error = FinalPath(directory.Value, root))
        return error;
    if (DWORD const error = FinalPath(clientImage, image))
        return error;
    if (root.empty() || root.back() != L'\\')
        root += L'\\';
    if (_wcsicmp((root + clientName).c_str(), image.c_str()) != 0)
        return ERROR_INVALID_NAME;

    std::wstring const cache = root + L"Cache";
    if (GetFileAttributesW(cache.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        DWORD const error = GetLastError();
        return Missing(error) ? ERROR_SUCCESS : error;
    }

    DWORD const data = GetFileAttributesW((root + L"Data").c_str());
    if (data == INVALID_FILE_ATTRIBUTES || !(data & FILE_ATTRIBUTE_DIRECTORY))
        return ERROR_DIRECTORY;
    if (DWORD const error = RemoveCacheEntry(cache, 0))
        return error;
    Sleep(1000);
    if (GetFileAttributesW(cache.c_str()) != INVALID_FILE_ATTRIBUTES)
        return ERROR_DIR_NOT_EMPTY;
    DWORD const error = GetLastError();
    return Missing(error) ? ERROR_SUCCESS : error;
}
