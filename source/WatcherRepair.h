#pragma once

#include "ClientProfile.h"

enum class WatcherRepairOutcome : unsigned
{
    NotApplicable,
    Busy,
    GuardRejected,
    Detached
};

WatcherRepairOutcome RepairDownloaderWatcher(std::uintptr_t image, WowLogin::ClientProfile const& profile,
    void* downloader, std::uintptr_t socket, void* watcher);
