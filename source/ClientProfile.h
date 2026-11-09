#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace WowLogin
{
    struct Signature
    {
        template<std::size_t N>
        constexpr Signature(std::uintptr_t rva, char const (&bytes)[N]) : Rva(rva), Bytes(bytes, N - 1) { }

        std::uintptr_t Rva;
        std::string_view Bytes;
    };

    struct ClientProfile
    {
        unsigned Build;
        std::uintptr_t DownloaderGlobal;
        std::uintptr_t DispatcherGlobal;
        std::uintptr_t CallbackThunk;
        std::uintptr_t ClearReadCallback;
        std::uintptr_t UnlinkWatcher;
        Signature DownloaderGuards[7];
        Signature RepairGuards[10];
    };

    inline constexpr std::array<unsigned char, 8> ThunkBytes = { 0xE9, 0x6B, 0xFC, 0xFF, 0xFF, 0xCC, 0xCC, 0xCC };

    // The two builds share object layouts, but their code, globals and import thunks have different RVAs.
    inline constexpr ClientProfile Profiles[] =
    {
        {
            26972, 0x1C7DEF8, 0x1C7DAC8, 0xD58180, 0xD50830, 0xD51D80,
            {
                { 0xD57192, "\x4C\x8D\x05\xE7\x0F\x00\x00\x48\x8B\xC8\xBA\x21\x4E\x00\x00" },
                { 0x19656B, "\x48\x89\x05\x86\x79\xAE\x01" },
                { 0xD57E04, "\x49\x8B\xF1\x41\x8B\xD8\x48\x8B\xEA\x41\x83\xF8\x04" },
                { 0xD50630, "\x48\x8B\x0D\x91\xD4\xF2\x00" },
                { 0xD5262F, "\x49\x8B\x9D\x50\x0C\x00\x00" },
                { 0xD5267B, "\x49\x63\x85\x40\x0C\x00\x00\x4C\x8B\x7C\x18\x08" },
                { 0xD5865D, "\x44\x89\x7B\x10\x44\x89\x3B\x4C\x89\x7B\x08" }
            },
            {
                { 0xD50850, "\x48\x8D\x8B\x90\x01\x00\x00\xE8\x14\x3E\x13\x00\x83\xBB\xB8\x01\x00\x00\x00\x74\x33" },
                { 0xD50898, "\x48\x89\xAB\xA8\x00\x00\x00\x48\xC7\x83\xC0\x01\x00\x00\x00\x00\x00\x00" },
                { 0xD4F00D, "\x48\x81\xC1\xE0\x01\x00\x00\xE8\x57\x56\x13\x00" },
                { 0xD4F0D1, "\xC7\x86\xA4\x00\x00\x00\x06\x00\x00\x00" },
                { 0xE84670, "\x48\xFF\x25\xB9\x50\x41\x00" },
                { 0x3ED230, "\x48\xFF\x25\x01\xC5\xEA\x00" },
                { 0xD51D80, "\x48\x89\x5C\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xEC\x20" },
                { 0xD51D92, "\x48\x81\xC1\x18\x0C\x00\x00\x48\x8B\xFA\xE8\xCF\x28\x13\x00" },
                { 0xD51E21, "\xE8\x7A\x07\x00\x00" },
                { 0xD525B6, "\x48\xC7\x82\x98\x00\x00\x00\xFF\xFF\xFF\xFF\xE8\xCA\xDE\xFF\xFF" }
            }
        },
        {
            26365, 0x1C75EF8, 0x1C75AC8, 0xD47600, 0xD3FCE0, 0xD41200,
            {
                { 0xD46612, "\x4C\x8D\x05\xE7\x0F\x00\x00\x48\x8B\xC8\xBA\x21\x4E\x00\x00" },
                { 0x1889EB, "\x48\x89\x05\x06\xD5\xAE\x01" },
                { 0xD47284, "\x49\x8B\xF1\x41\x8B\xD8\x48\x8B\xEA\x41\x83\xF8\x04" },
                { 0xD3E4DC, "\x48\x8B\x0D\xE5\x75\xF3\x00" },
                { 0xD41AAF, "\x49\x8B\x9D\x50\x0C\x00\x00" },
                { 0xD41AFB, "\x49\x63\x85\x40\x0C\x00\x00\x4C\x8B\x7C\x18\x08" },
                { 0xD47ADD, "\x44\x89\x7B\x10\x44\x89\x3B\x4C\x89\x7B\x08" }
            },
            {
                { 0xD3FD00, "\x48\x8D\x8B\x90\x01\x00\x00\xE8\x04\xC9\x69\xFF\x83\xBB\xB8\x01\x00\x00\x00\x74\x33" },
                { 0xD3FD48, "\x48\x89\xAB\xA8\x00\x00\x00\x48\xC7\x83\xC0\x01\x00\x00\x00\x00\x00\x00" },
                { 0xD3E4BD, "\x48\x81\xC1\xE0\x01\x00\x00\xE8\x47\xE1\x69\xFF" },
                { 0xD3E581, "\xC7\x86\xA4\x00\x00\x00\x06\x00\x00\x00" },
                { 0x3DC610, "\x48\xFF\x25\x29\xB1\xEA\x00" },
                { 0xE73AB0, "\x48\xFF\x25\x91\x3C\x41\x00" },
                { 0xD41200, "\x48\x89\x5C\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xEC\x20" },
                { 0xD41212, "\x48\x81\xC1\x18\x0C\x00\x00\x48\x8B\xFA\xE8\xEF\xB3\x69\xFF" },
                { 0xD412A1, "\xE8\x7A\x07\x00\x00" },
                { 0xD41A36, "\x48\xC7\x82\x98\x00\x00\x00\xFF\xFF\xFF\xFF\xE8\xFA\xDE\xFF\xFF" }
            }
        }
    };
}
