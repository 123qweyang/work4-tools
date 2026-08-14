#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "index.h"

constexpr size_t kSearchLimit = 100000;

struct ListItem {
    std::wstring path;
    std::wstring name;
    uint64_t size = 0;
    FILETIME ft{};
    uint32_t attrs = 0;
    bool isDir = false;
    bool hidden = false;
};

struct SearchPayload {
    int generation = 0;
    std::wstring query;
    std::vector<ListItem> items;
    uint64_t elapsedMs = 0;
    bool truncated = false;
    bool fromIndex = false;
};

// 后台执行搜索（内部起线程，完成后 PostMessage WM_APP_SEARCH_DONE）
void StartBackgroundSearch(const std::wstring& query, int generation, bool showHidden,
                           std::shared_ptr<const FileIndex> snapshot);

// 实时枚举单个目录（索引未覆盖时浏览兜底）
bool EnumerateDirRealtime(const std::wstring& dir, bool showHidden,
                          std::vector<ListItem>& out);
