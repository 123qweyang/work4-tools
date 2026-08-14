#include "index.h"

#include "app.h"
#include "utils.h"
#include "worker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <utility>

namespace {

thread_local std::vector<Entry> g_pendingFiles;

class FileWriter {
public:
    explicit FileWriter(HANDLE h) : h_(h) {}
    ~FileWriter() { Flush(); Close(); }

    void Close() {
        Flush();
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

    bool Raw(const void* p, DWORD n) {
        const BYTE* src = (const BYTE*)p;
        while (n > 0) {
            if (buf_.size() >= 65536) {
                if (!Flush()) return false;
            }
            size_t take = std::min<size_t>(65536 - buf_.size(), n);
            buf_.insert(buf_.end(), src, src + take);
            src += take;
            n -= (DWORD)take;
        }
        return true;
    }

    bool Flush() {
        if (buf_.empty()) return true;
        DWORD written = 0;
        bool ok = WriteFile(h_, buf_.data(), (DWORD)buf_.size(), &written, nullptr) &&
                  written == buf_.size();
        buf_.clear();
        return ok;
    }
    bool U32(uint32_t v) { return Raw(&v, 4); }
    bool U64(uint64_t v) { return Raw(&v, 8); }
    bool Str(const std::wstring& s) {
        uint64_t bytes = (uint64_t)s.size() * 2;
        if (!U64(bytes)) return false;
        return Raw(s.data(), (DWORD)bytes);
    }

private:
    HANDLE h_;
    std::vector<BYTE> buf_;
};

class FileReader {
public:
    explicit FileReader(HANDLE h) : h_(h) {}
    ~FileReader() { Close(); }

    void Close() {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

    bool Raw(void* p, DWORD n) {
        BYTE* dst = (BYTE*)p;
        while (n > 0) {
            if (pos_ >= len_) {
                if (!Refill()) return false;
            }
            size_t take = std::min<size_t>(len_ - pos_, n);
            memcpy(dst, buf_.data() + pos_, take);
            dst += take;
            pos_ += take;
            n -= (DWORD)take;
        }
        return true;
    }
    bool U32(uint32_t& v) { return Raw(&v, 4); }
    bool U64(uint64_t& v) { return Raw(&v, 8); }
    bool Str(std::wstring& s) {
        uint64_t bytes = 0;
        if (!U64(bytes)) return false;
        if (bytes > 4096 * 4096 || (bytes & 1)) return false;
        s.resize((size_t)bytes / 2);
        if (bytes == 0) return true;
        return Raw(&s[0], (DWORD)bytes);
    }

private:
    HANDLE h_;
    std::vector<BYTE> buf_;
    size_t pos_ = 0;
    size_t len_ = 0;

    bool Refill() {
        if (buf_.empty()) buf_.resize(262144);
        DWORD got = 0;
        if (!ReadFile(h_, buf_.data(), (DWORD)buf_.size(), &got, nullptr)) return false;
        pos_ = 0;
        len_ = got;
        return got > 0;
    }
};

}  // namespace

std::wstring BuildPath(const FileIndex& idx, uint32_t id) {
    std::vector<const std::wstring*> parts;
    uint32_t cur = id;
    size_t guard = 0;
    while (cur != 0 && guard < 128) {
        if (cur >= idx.entries.size()) return L"";
        parts.push_back(&idx.entries[cur].name);
        cur = idx.entries[cur].parent;
        ++guard;
    }
    if (cur != 0) return L"";
    std::wstring out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        const std::wstring& n = **it;
        if (out.empty()) {
            out = n;
        } else {
            out += L'\\';
            out += n;
        }
    }
    return out;
}

std::wstring IndexManager::IndexFilePath() {
    return util::GetExeDir() + L"\\index\\file-index.bin";
}

IndexManager::~IndexManager() {
    CancelAndWait();
}

std::shared_ptr<const FileIndex> IndexManager::Snapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return current_;
}

IndexProgress IndexManager::GetProgress() const {
    IndexProgress p;
    p.building = building_.load();
    p.files = progFiles_.load();
    p.dirs = progDirs_.load();
    p.skipped = progSkipped_.load();
    p.bytes = progBytes_.load();
    return p;
}

void IndexManager::StartAsyncLoad() {
    if (loadThread_.joinable()) return;
    loadThread_ = std::thread(&IndexManager::LoadLoop, this);
}

void IndexManager::StartRebuild() {
    cancel_.store(true);
    if (rebuildThread_.joinable()) rebuildThread_.join();
    if (loadThread_.joinable()) loadThread_.join();
    cancel_.store(false);
    rebuildThread_ = std::thread(&IndexManager::RebuildLoop, this);
}

void IndexManager::CancelAndWait() {
    cancel_.store(true);
    if (rebuildThread_.joinable()) rebuildThread_.join();
    if (loadThread_.joinable()) loadThread_.join();
}

void IndexManager::LoadLoop() {
    std::shared_ptr<FileIndex> idx = LoadFromDisk();
    if (idx) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!current_ || !current_->complete) current_ = std::move(idx);
    }
    PostMessageW(g_hwnd, WM_APP_INDEX_LOADED, idx ? 1 : 0, 0);
}

void IndexManager::RebuildLoop() {
    auto idx = std::make_shared<FileIndex>();
    idx->roots = util::GetFixedDrives();
    if (idx->roots.empty()) {
        building_.store(false);
        PostMessageW(g_hwnd, WM_APP_INDEX_DONE, 0, 0);
        return;
    }
    // 哨兵条目 id=0（name 为空，parent=0），保证所有条目 parent < id
    {
        Entry sentinel;
        sentinel.name = L"";
        sentinel.parent = 0;
        idx->entries.push_back(std::move(sentinel));
    }

    cancel_.store(false);
    building_.store(true);
    progFiles_.store(0);
    progDirs_.store(0);
    progSkipped_.store(0);
    progBytes_.store(0);

    unsigned hw = std::thread::hardware_concurrency();
    // 使用 Windows 默认调度策略：不绑定特定 CPU 核心，线程数取逻辑处理器数
    size_t threads = hw ? (size_t)hw : 4;
    ThreadPool pool(threads);
    util::WriteLogLine(L"rebuild start: threads=" + std::to_wstring(threads) +
                       L", affinity=default");

    struct Builder {
        FileIndex& idx;
        ThreadPool& pool;
        std::atomic<bool>& cancel;
        std::atomic<uint64_t>& files;
        std::atomic<uint64_t>& dirs;
        std::atomic<uint64_t>& skipped;
        std::atomic<uint64_t>& bytes;
        std::mutex m;
        // 扫描期间只记录，不直接构建 unordered_map（避免锁内 rehash）
        std::vector<std::pair<uint32_t, std::vector<uint32_t>>> childList;
        std::vector<std::pair<std::wstring, uint32_t>> pathList;

        Builder(FileIndex& i, ThreadPool& p, std::atomic<bool>& c,
                std::atomic<uint64_t>& f, std::atomic<uint64_t>& d,
                std::atomic<uint64_t>& s, std::atomic<uint64_t>& b)
            : idx(i), pool(p), cancel(c), files(f), dirs(d), skipped(s), bytes(b) {}

        void FlushFiles(std::vector<uint32_t>& childIds) {
            if (g_pendingFiles.empty()) return;
            {
                std::lock_guard<std::mutex> lk(m);
                for (auto& e : g_pendingFiles) {
                    uint32_t id = (uint32_t)idx.entries.size();
                    idx.entries.push_back(std::move(e));
                    childIds.push_back(id);
                }
            }
            g_pendingFiles.clear();
        }

        void AddFile(Entry e, std::vector<uint32_t>& childIds) {
            g_pendingFiles.push_back(std::move(e));
            if (g_pendingFiles.size() >= 256) FlushFiles(childIds);
        }

        uint32_t AddDirEntry(Entry e) {
            std::lock_guard<std::mutex> lk(m);
            uint32_t id = (uint32_t)idx.entries.size();
            idx.entries.push_back(std::move(e));
            return id;
        }

        void ScanDirInner(uint32_t dirId, std::wstring dir) {
            if (cancel.load()) return;
            std::wstring search = util::DirWildcard(dir);
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW(search.c_str(), FindExInfoBasic, &fd,
                                        FindExSearchNameMatch, nullptr,
                                        FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) {
                skipped.fetch_add(1);
                return;
            }

            std::vector<uint32_t> childIds;
            std::vector<std::pair<uint32_t, std::wstring>> subdirs;
            do {
                const wchar_t* nm = fd.cFileName;
                if (nm[0] == L'.' && (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0))) continue;

                bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    std::wstring nmLower = util::ToLower(nm);
                    if (nmLower == L"$recycle.bin" || nmLower == L"system volume information") {
                        continue;  // 系统保护目录
                    }
                    Entry e;
                    e.name = nm;
                    e.parent = dirId;
                    e.ft = fd.ftLastWriteTime;
                    e.attrs = fd.dwFileAttributes;
                    uint32_t id = AddDirEntry(std::move(e));
                    childIds.push_back(id);
                    subdirs.emplace_back(id, dir + L"\\" + nm);
                    dirs.fetch_add(1);
                } else {
                    Entry e;
                    e.name = nm;
                    e.parent = dirId;
                    e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                    e.ft = fd.ftLastWriteTime;
                    e.attrs = fd.dwFileAttributes;
                    AddFile(std::move(e), childIds);
                    files.fetch_add(1);
                    bytes.fetch_add(e.size);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);

            FlushFiles(childIds);
            {
                std::lock_guard<std::mutex> lk(m);
                childList.emplace_back(dirId, std::move(childIds));
                pathList.emplace_back(util::ToLower(util::NormalizeDir(dir)), dirId);
            }
            if (cancel.load()) return;

            std::vector<std::function<void()>> tasks;
            tasks.reserve(subdirs.size());
            for (auto& sd : subdirs) {
                tasks.push_back([this, id = sd.first, p = std::move(sd.second)]() mutable {
                    ScanDir(id, std::move(p));
                });
            }
            pool.EnqueueBatch(std::move(tasks));
        }

        void ScanDir(uint32_t dirId, std::wstring dir) {
            try {
                ScanDirInner(dirId, std::move(dir));
            } catch (const std::exception& e) {
                g_pendingFiles.clear();
                skipped.fetch_add(1);
                util::WriteLogLine(L"ScanDir exception: " + util::ToWide(e.what()) +
                                   L" @" + dir);
            } catch (...) {
                g_pendingFiles.clear();
                skipped.fetch_add(1);
                util::WriteLogLine(L"ScanDir unknown exception @" + dir);
            }
        }
    };

    Builder b(*idx, pool, cancel_, progFiles_, progDirs_, progSkipped_, progBytes_);
    {
        std::vector<std::function<void()>> tasks;
        tasks.reserve(idx->roots.size());
        for (auto& r : idx->roots) {
            Entry e;
            e.name = r;
            e.parent = 0;
            e.attrs = FILE_ATTRIBUTE_DIRECTORY;
            uint32_t id = b.AddDirEntry(std::move(e));
            b.dirs.fetch_add(1);
            {
                std::lock_guard<std::mutex> lk(b.m);
                b.childList.emplace_back(0, std::vector<uint32_t>{id});
                b.pathList.emplace_back(util::ToLower(r), id);
            }
            tasks.push_back([&b, id, p = r]() mutable { b.ScanDir(id, std::move(p)); });
        }
        pool.EnqueueBatch(std::move(tasks));
    }
    {
        auto tw0 = std::chrono::steady_clock::now();
        pool.WaitAll();
        auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - tw0)
                          .count();
        util::WriteLogLine(L"WaitAll done in " + std::to_wstring(waitMs) +
                           L" ms, pending=" + std::to_wstring(pool.Pending()));
    }

    if (!cancel_.load()) {
        // 锁外一次性构建目录索引（无竞争，rehash 不再阻塞扫描线程）
        util::WriteLogLine(L"building dir maps...");
        idx->children.reserve(b.childList.size());
        idx->byPath.reserve(b.pathList.size());
        for (auto& kv : b.childList) {
            idx->children.emplace(kv.first, std::move(kv.second));
        }
        for (auto& kv : b.pathList) {
            idx->byPath.emplace(std::move(kv.first), kv.second);
        }
        util::WriteLogLine(L"maps done: entries=" + std::to_wstring(idx->entries.size()) +
                           L", dirs=" + std::to_wstring(idx->byPath.size()));
        GetSystemTimeAsFileTime(&idx->builtAt);
        idx->complete = true;
        util::WriteLogLine(L"saving index...");
        SaveToDisk(*idx, cancel_);
        util::WriteLogLine(L"index saved");
        {
            std::lock_guard<std::mutex> lk(mutex_);
            current_ = std::move(idx);
        }
    }
    building_.store(false);
    PostMessageW(g_hwnd, WM_APP_INDEX_DONE, 0, 0);
}

void IndexManager::SaveToDisk(const FileIndex& idx, const std::atomic<bool>& cancel) const {
    std::wstring dir = util::GetExeDir() + L"\\index";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring tmp = dir + L"\\file-index.tmp";
    std::wstring final = dir + L"\\file-index.bin";

    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FileWriter w(h);

    bool ok = w.Raw("W4FI", 4);
    ok = ok && w.U32(2);  // 版本 2：紧凑条目结构
    uint64_t builtAtRaw = 0;
    memcpy(&builtAtRaw, &idx.builtAt, 8);
    ok = ok && w.U64(builtAtRaw);
    ok = ok && w.U64(idx.roots.size());
    for (const auto& r : idx.roots) ok = ok && w.Str(r);

    ok = ok && w.U64(idx.entries.size());
    for (const auto& e : idx.entries) {
        if (cancel.load()) {
            ok = false;
            break;
        }
        uint64_t ftRaw = 0;
        memcpy(&ftRaw, &e.ft, 8);
        ok = ok && w.Str(e.name);
        ok = ok && w.U32(e.parent);
        ok = ok && w.U64(e.size);
        ok = ok && w.U64(ftRaw);
        ok = ok && w.U32(e.attrs);
        if (!ok) break;
    }

    ok = ok && w.U64(idx.children.size());
    for (const auto& kv : idx.children) {
        if (cancel.load()) {
            ok = false;
            break;
        }
        ok = ok && w.U32(kv.first);
        ok = ok && w.U32((uint32_t)kv.second.size());
        for (uint32_t id : kv.second) ok = ok && w.U32(id);
        if (!ok) break;
    }

    ok = ok && w.U64(idx.byPath.size());
    for (const auto& kv : idx.byPath) {
        if (cancel.load()) {
            ok = false;
            break;
        }
        ok = ok && w.Str(kv.first);
        ok = ok && w.U32(kv.second);
        if (!ok) break;
    }

    w.Close();  // 先关闭句柄再改名
    if (ok) {
        MoveFileExW(tmp.c_str(), final.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } else {
        DeleteFileW(tmp.c_str());
    }
}

std::shared_ptr<FileIndex> IndexManager::LoadFromDisk() {
    HANDLE h = CreateFileW(IndexFilePath().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    FileReader r(h);

    char magic[4];
    uint32_t version = 0;
    if (!r.Raw(magic, 4) || memcmp(magic, "W4FI", 4) != 0) return nullptr;
    if (!r.U32(version) || version != 2) return nullptr;

    auto idx = std::make_shared<FileIndex>();
    uint64_t builtAtRaw = 0;
    if (!r.U64(builtAtRaw)) return nullptr;
    memcpy(&idx->builtAt, &builtAtRaw, 8);

    uint64_t nroots = 0;
    if (!r.U64(nroots) || nroots > 64) return nullptr;
    for (uint64_t i = 0; i < nroots; ++i) {
        std::wstring s;
        if (!r.Str(s)) return nullptr;
        idx->roots.push_back(std::move(s));
    }

    uint64_t n = 0;
    if (!r.U64(n) || n == 0 || n > 20000000) return nullptr;
    idx->entries.reserve((size_t)n);
    for (uint64_t i = 0; i < n; ++i) {
        Entry e;
        uint64_t ftRaw = 0;
        if (!r.Str(e.name)) return nullptr;
        if (!r.U32(e.parent)) return nullptr;
        if (!r.U64(e.size)) return nullptr;
        if (!r.U64(ftRaw)) return nullptr;
        if (!r.U32(e.attrs)) return nullptr;
        memcpy(&e.ft, &ftRaw, 8);
        if (i == 0) {
            if (e.parent != 0 || !e.name.empty()) return nullptr;  // 哨兵条目
        } else if (e.parent >= i) {
            return nullptr;  // 父 id 必须小于自身 id
        }
        idx->entries.push_back(std::move(e));
    }

    uint64_t nc = 0;
    if (!r.U64(nc) || nc > 5000000) return nullptr;
    idx->children.reserve((size_t)nc);
    for (uint64_t i = 0; i < nc; ++i) {
        uint32_t parent = 0;
        uint32_t cnt = 0;
        if (!r.U32(parent)) return nullptr;
        if (!r.U32(cnt) || cnt > 1000000) return nullptr;
        if (parent >= idx->entries.size()) return nullptr;
        std::vector<uint32_t> ids((size_t)cnt);
        for (uint32_t j = 0; j < cnt; ++j) {
            if (!r.U32(ids[j])) return nullptr;
            if (ids[j] >= idx->entries.size()) return nullptr;
        }
        idx->children[parent] = std::move(ids);
    }

    uint64_t np = 0;
    if (!r.U64(np) || np > 5000000) return nullptr;
    idx->byPath.reserve((size_t)np);
    for (uint64_t i = 0; i < np; ++i) {
        std::wstring key;
        uint32_t id = 0;
        if (!r.Str(key)) return nullptr;
        if (!r.U32(id) || id >= idx->entries.size()) return nullptr;
        idx->byPath[std::move(key)] = id;
    }

    if (idx->roots.empty()) idx->roots = util::GetFixedDrives();
    idx->complete = true;
    return idx;
}
