#include "utils.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cstring>

namespace util {

std::wstring ToLower(const std::wstring& s) {
    if (s.empty()) return s;
    std::wstring out = s;
    CharLowerBuffW(&out[0], (DWORD)out.size());
    return out;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"<non-utf8>";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring NormalizeDir(const std::wstring& p) {
    std::wstring s = p;
    while (s.size() > 2 && s.back() == L'\\') s.pop_back();
    return s;
}

std::wstring GetParentDir(const std::wstring& dir) {
    if (dir.size() <= 2) return dir;
    size_t pos = dir.find_last_of(L'\\');
    if (pos == std::wstring::npos) return dir;
    if (pos == 2 && dir[1] == L':') return dir.substr(0, 2);
    if (pos == 0) return dir.substr(0, 1);
    return dir.substr(0, pos);
}

std::wstring GetBaseName(const std::wstring& path) {
    size_t pos = path.find_last_of(L'\\');
    if (pos == std::wstring::npos) return path;
    if (pos + 1 >= path.size()) return path;
    return path.substr(pos + 1);
}

std::wstring AddLongPathPrefix(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0 || path.rfind(L"\\\\.\\", 0) == 0) return path;
    if (path.size() >= 2 && path[1] == L':') {
        if (path.size() == 2) return L"\\\\?\\" + path + L"\\";  // 盘符根必须带尾斜杠
        return L"\\\\?\\" + path;
    }
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return path;
}

std::wstring DirWildcard(const std::wstring& dir) {
    std::wstring p = AddLongPathPrefix(dir);
    if (p.back() == L'\\') return p + L"*";
    return p + L"\\*";
}

std::wstring GetExtensionLower(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    size_t slash = name.find_last_of(L'\\');
    if (dot == std::wstring::npos) return L"";
    if (slash != std::wstring::npos && dot < slash) return L"";
    return ToLower(name.substr(dot));
}

bool HasWildcard(const std::wstring& s) {
    return s.find(L'*') != std::wstring::npos || s.find(L'?') != std::wstring::npos;
}

static bool GlobRec(const wchar_t* p, const wchar_t* t) {
    while (*p) {
        if (*p == L'*') {
            while (p[1] == L'*') ++p;
            if (!p[1]) return true;
            for (const wchar_t* s = t;; ++s) {
                if (GlobRec(p + 1, s)) return true;
                if (!*s) return false;
            }
        } else if (*p == L'?') {
            if (!*t) return false;
            ++p;
            ++t;
        } else {
            if (*p != *t) return false;
            ++p;
            ++t;
        }
    }
    return *t == 0;
}

bool GlobMatchLower(const std::wstring& pattern, const std::wstring& text) {
    return GlobRec(pattern.c_str(), text.c_str());
}

bool MatchesQuery(const std::wstring& name, const std::wstring& queryLower) {
    if (queryLower.empty()) return true;
    std::wstring nameLower;
    wchar_t buf[512];
    if (name.size() < 512) {
        wcscpy(buf, name.c_str());
        CharLowerBuffW(buf, (DWORD)name.size());
        nameLower = buf;
    } else {
        nameLower = ToLower(name);
    }
    if (HasWildcard(queryLower)) return GlobMatchLower(queryLower, nameLower);
    return wcsstr(nameLower.c_str(), queryLower.c_str()) != nullptr;
}

std::vector<std::wstring> GetFixedDrives() {
    std::vector<std::wstring> drives;
    DWORD mask = GetLogicalDrives();
    for (DWORD i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = L"A:\\";
        root[0] = (wchar_t)(L'A' + i);
        if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED) {
            drives.push_back(root.substr(0, 2));
        }
    }
    return drives;
}

std::wstring FormatSize(uint64_t size) {
    if (size < 1024) {
        wchar_t b[48];
        swprintf(b, 48, L"%llu B", (unsigned long long)size);
        return b;
    }
    static const wchar_t* units[] = {L"KB", L"MB", L"GB", L"TB", L"PB"};
    double v = (double)size;
    int u = -1;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    wchar_t b[64];
    swprintf(b, 64, L"%.1f %ls", v, units[u]);
    return b;
}

std::wstring FormatTime(FILETIME ft) {
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local)) return L"";
    if (!FileTimeToSystemTime(&local, &st)) return L"";
    wchar_t b[64];
    swprintf(b, 64, L"%04d-%02d-%02d %02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}

std::wstring FormatCount(uint64_t n) {
    std::wstring s = std::to_wstring(n);
    std::wstring out;
    int c = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (c && c % 3 == 0) out.push_back(L',');
        out.push_back(*it);
        ++c;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH + 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t pos = p.find_last_of(L'\\');
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

bool IsHiddenOrSystem(uint32_t attrs) {
    return (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
}

void WriteLogLine(const std::wstring& msg) {
    HANDLE h = CreateFileW((GetExeDir() + L"\\index\\build.log").c_str(),
                           FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t head[64];
    swprintf(head, 64, L"[%02d:%02d:%02d.%03d] ",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::wstring line = head + msg + L"\r\n";
    DWORD written = 0;
    WriteFile(h, line.c_str(), (DWORD)(line.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
}

}  // namespace util
