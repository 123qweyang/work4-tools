#include "search.h"

#include "app.h"
#include "utils.h"
#include "worker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace {

ListItem MakeItem(const FileIndex& idx, uint32_t id) {
    const Entry& e = idx.entries[id];
    ListItem it;
    it.path = BuildPath(idx, id);
    it.name = e.name;
    it.size = e.size;
    it.ft = e.ft;
    it.attrs = e.attrs;
    it.isDir = (e.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    it.hidden = util::IsHiddenOrSystem(e.attrs);
    return it;
}

void SortByName(std::vector<ListItem>& items) {
    std::sort(items.begin(), items.end(), [](const ListItem& a, const ListItem& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        int c = _wcsicmp(a.name.c_str(), b.name.c_str());
        if (c == 0) c = _wcsicmp(a.path.c_str(), b.path.c_str());
        return c < 0;
    });
}

void DoIndexSearch(const FileIndex& idx, const std::wstring& queryLower, bool showHidden,
                   SearchPayload& payload) {
    const auto& entries = idx.entries;
    unsigned hw = std::thread::hardware_concurrency();
    size_t n = hw ? (size_t)hw : 4;
    if (n > 64) n = 64;

    std::atomic<uint64_t> collected{0};
    std::vector<std::vector<ListItem>> buckets(n);
    std::vector<std::thread> threads;
    threads.reserve(n);

    for (size_t t = 0; t < n; ++t) {
        threads.emplace_back([&, t]() {
            size_t start = entries.size() * t / n;
            size_t end = entries.size() * (t + 1) / n;
            auto& out = buckets[t];
            for (size_t i = start; i < end; ++i) {
                if (collected.load() >= kSearchLimit) break;
                const Entry& e = entries[i];
                if (!showHidden && util::IsHiddenOrSystem(e.attrs)) continue;
                if (!util::MatchesQuery(e.name, queryLower)) continue;
                if (collected.fetch_add(1) >= kSearchLimit) {
                    collected.fetch_sub(1);
                    break;
                }
                out.push_back(MakeItem(idx, (uint32_t)i));
            }
        });
    }
    for (auto& th : threads) th.join();

    size_t total = 0;
    for (auto& b : buckets) total += b.size();
    payload.items.reserve(std::min(total, kSearchLimit));
    for (auto& b : buckets) {
        payload.items.insert(payload.items.end(),
                             std::make_move_iterator(b.begin()),
                             std::make_move_iterator(b.end()));
    }
    if (payload.items.size() > kSearchLimit) {
        payload.items.resize(kSearchLimit);
        payload.truncated = true;
    }
    SortByName(payload.items);
}

void RealtimeScanDir(ThreadPool& pool, std::wstring dir, const std::wstring& queryLower,
                     bool showHidden, std::atomic<uint64_t>& count,
                     std::vector<ListItem>& out, std::mutex& m) {
    if (count.load() >= kSearchLimit) return;
    std::wstring search = util::DirWildcard(dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(search.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subdirs;
    do {
        const wchar_t* nm = fd.cFileName;
        if (nm[0] == L'.' && (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0))) continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool hidden = util::IsHiddenOrSystem(fd.dwFileAttributes);

        if (isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            std::wstring nmLower = util::ToLower(nm);
            if (nmLower == L"$recycle.bin" || nmLower == L"system volume information") continue;
            subdirs.push_back(dir + L"\\" + nm);
        }
        if (!showHidden && hidden) continue;
        if (!util::MatchesQuery(nm, queryLower)) continue;
        if (count.fetch_add(1) >= kSearchLimit) {
            count.fetch_sub(1);
            break;
        }

        ListItem it;
        it.path = dir + L"\\" + nm;
        it.name = nm;
        it.isDir = isDir;
        it.size = isDir ? 0 : ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        it.ft = fd.ftLastWriteTime;
        it.attrs = fd.dwFileAttributes;
        it.hidden = hidden;
        std::lock_guard<std::mutex> lk(m);
        out.push_back(std::move(it));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (count.load() >= kSearchLimit) return;
    std::vector<std::function<void()>> tasks;
    tasks.reserve(subdirs.size());
    for (auto& s : subdirs) {
        tasks.push_back([&, s = std::move(s)]() mutable {
            RealtimeScanDir(pool, std::move(s), queryLower, showHidden, count, out, m);
        });
    }
    pool.EnqueueBatch(std::move(tasks));
}

void DoRealtimeSearch(const std::wstring& queryLower, bool showHidden, SearchPayload& payload) {
    std::vector<std::wstring> roots;
    auto snap = g_app.index.Snapshot();
    if (snap && snap->complete && !snap->roots.empty()) {
        roots = snap->roots;
    } else {
        roots = util::GetFixedDrives();
    }
    unsigned hw = std::thread::hardware_concurrency();
    ThreadPool pool(hw ? (size_t)hw : 4);
    std::atomic<uint64_t> count{0};
    std::vector<ListItem> out;
    std::mutex m;

    std::vector<std::function<void()>> tasks;
    tasks.reserve(roots.size());
    for (auto& r : roots) {
        tasks.push_back([&, r = r]() {
            RealtimeScanDir(pool, r, queryLower, showHidden, count, out, m);
        });
    }
    pool.EnqueueBatch(std::move(tasks));
    pool.WaitAll();

    payload.items = std::move(out);
    if (payload.items.size() > kSearchLimit) {
        payload.items.resize(kSearchLimit);
        payload.truncated = true;
    }
    SortByName(payload.items);
}

}  // namespace

void StartBackgroundSearch(const std::wstring& query, int generation, bool showHidden,
                           std::shared_ptr<const FileIndex> snapshot) {
    std::thread([query, generation, showHidden, snapshot]() {
        auto t0 = std::chrono::steady_clock::now();
        auto* payload = new SearchPayload();
        payload->generation = generation;
        payload->query = query;

        std::wstring queryLower = util::ToLower(query);
        bool usedIndex = snapshot && snapshot->complete && !snapshot->entries.empty();
        if (usedIndex) {
            payload->fromIndex = true;
            DoIndexSearch(*snapshot, queryLower, showHidden, *payload);
        } else {
            DoRealtimeSearch(queryLower, showHidden, *payload);
        }

        auto t1 = std::chrono::steady_clock::now();
        payload->elapsedMs =
            (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        PostMessageW(g_hwnd, WM_APP_SEARCH_DONE, (WPARAM)generation, (LPARAM)payload);
    }).detach();
}

bool EnumerateDirRealtime(const std::wstring& dir, bool showHidden,
                          std::vector<ListItem>& out) {
    std::wstring search = util::DirWildcard(dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(search.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;

    do {
        const wchar_t* nm = fd.cFileName;
        if (nm[0] == L'.' && (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0))) continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool hidden = util::IsHiddenOrSystem(fd.dwFileAttributes);
        if (!showHidden && hidden) continue;

        ListItem it;
        it.path = dir + L"\\" + nm;
        it.name = nm;
        it.isDir = isDir;
        it.size = isDir ? 0 : ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        it.ft = fd.ftLastWriteTime;
        it.attrs = fd.dwFileAttributes;
        it.hidden = hidden;
        out.push_back(std::move(it));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}
