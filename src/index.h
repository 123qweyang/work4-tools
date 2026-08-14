#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// 索引条目：文件与目录共用（目录通过 attrs 的 DIRECTORY 位区分）。
// 目录层级用 parent 链表示，不存完整路径，大幅降低内存占用。
struct Entry {
    std::wstring name;    // 文件名或目录名；entries[0] 为哨兵
    uint32_t parent = 0;  // 父目录条目 id（0 = 根哨兵）
    uint64_t size = 0;
    FILETIME ft{};
    uint32_t attrs = 0;
};

struct FileIndex {
    std::vector<Entry> entries;  // entries[0] 为哨兵，id 即下标
    // 目录条目 id -> 子项条目 id
    std::unordered_map<uint32_t, std::vector<uint32_t>> children;
    // 小写完整目录路径 -> 目录条目 id（仅目录）
    std::unordered_map<std::wstring, uint32_t> byPath;
    std::vector<std::wstring> roots;
    FILETIME builtAt{};
    bool complete = false;
};

// 沿 parent 链拼接完整路径（如 "C:\foo\bar.txt"）
std::wstring BuildPath(const FileIndex& idx, uint32_t id);

struct IndexProgress {
    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t skipped = 0;
    uint64_t bytes = 0;
    bool building = false;
};

class IndexManager {
public:
    IndexManager() = default;
    ~IndexManager();

    void StartAsyncLoad();   // 后台加载磁盘索引（启动时调用一次）
    void StartRebuild();     // 取消旧任务并启动全盘重建
    void CancelAndWait();

    std::shared_ptr<const FileIndex> Snapshot() const;
    IndexProgress GetProgress() const;
    bool IsBuilding() const { return building_.load(); }
    static std::wstring IndexFilePath();

private:
    void LoadLoop();
    void RebuildLoop();
    std::shared_ptr<FileIndex> LoadFromDisk();
    void SaveToDisk(const FileIndex& idx, const std::atomic<bool>& cancel) const;

    mutable std::mutex mutex_;
    std::shared_ptr<FileIndex> current_;
    std::atomic<bool> building_{false};
    std::atomic<bool> cancel_{false};
    std::thread loadThread_;
    std::thread rebuildThread_;

    std::atomic<uint64_t> progFiles_{0};
    std::atomic<uint64_t> progDirs_{0};
    std::atomic<uint64_t> progSkipped_{0};
    std::atomic<uint64_t> progBytes_{0};
};
