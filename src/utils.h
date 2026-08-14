#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace util {

std::wstring ToLower(const std::wstring& s);
std::wstring ToWide(const std::string& s);
std::wstring NormalizeDir(const std::wstring& p);
std::wstring GetParentDir(const std::wstring& dir);
std::wstring GetBaseName(const std::wstring& path);
std::wstring AddLongPathPrefix(const std::wstring& path);
std::wstring DirWildcard(const std::wstring& dir);
std::wstring GetExtensionLower(const std::wstring& name);
bool HasWildcard(const std::wstring& s);
bool GlobMatchLower(const std::wstring& pattern, const std::wstring& text);
bool MatchesQuery(const std::wstring& name, const std::wstring& queryLower);
std::vector<std::wstring> GetFixedDrives();
std::wstring FormatSize(uint64_t size);
std::wstring FormatTime(FILETIME ft);
std::wstring FormatCount(uint64_t n);
std::wstring GetExeDir();
bool IsHiddenOrSystem(uint32_t attrs);
void WriteLogLine(const std::wstring& msg);

}  // namespace util
