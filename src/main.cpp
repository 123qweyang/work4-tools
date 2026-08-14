#include "app.h"
#include "ui.h"
#include "utils.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

App g_app;
HWND g_hwnd = nullptr;

namespace {

extern "C" USHORT __stdcall RtlCaptureStackBackTrace(ULONG, ULONG, PVOID*, PULONG);

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    std::wstring logPath = util::GetExeDir() + L"\\index\\crash.log";
    CreateDirectoryW((util::GetExeDir() + L"\\index").c_str(), nullptr);
    HANDLE h = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        wchar_t buf[256];
        swprintf(buf, 256, L"code=0x%08X addr=%p\n",
                 ep->ExceptionRecord->ExceptionCode,
                 ep->ExceptionRecord->ExceptionAddress);
        DWORD written = 0;
        WriteFile(h, buf, (DWORD)(wcslen(buf) * sizeof(wchar_t)), &written, nullptr);
        PVOID frames[48];
        USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) {
            swprintf(buf, 256, L"[%u] %p\n", (unsigned)i, frames[i]);
            WriteFile(h, buf, (DWORD)(wcslen(buf) * sizeof(wchar_t)), &written, nullptr);
        }
        CloseHandle(h);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct TreeNodeData {
    std::wstring path;
    bool loaded = false;
};

std::wstring DisplayPath(const std::wstring& dir) {
    if (dir.size() == 2 && dir[1] == L':') return dir + L"\\";
    return dir;
}

std::wstring GetWindowTextStr(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}

void UpdateButtons() {
    EnableWindow(g_app.hBtnBack, g_app.histPos > 0);
    EnableWindow(g_app.hBtnFwd, g_app.histPos + 1 < g_app.history.size());
}

void SortItems() {
    int col = g_app.sortCol;
    int mode = g_app.listMode;
    bool asc = g_app.sortAsc;
    std::sort(g_app.listItems.begin(), g_app.listItems.end(),
              [col, mode, asc](const ListItem& a, const ListItem& b) {
                  // 文件夹始终排在文件前面
                  if (a.isDir != b.isDir) return a.isDir > b.isDir;
                  int c = 0;
                  if (col == 0) {
                      c = _wcsicmp(a.name.c_str(), b.name.c_str());
                  } else if (col == 1) {
                      if (mode == LIST_MODE_SEARCH) {
                          c = _wcsicmp(a.path.c_str(), b.path.c_str());
                      } else {
                          c = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
                      }
                  } else if (col == 2) {
                      if (mode == LIST_MODE_SEARCH) {
                          c = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
                      } else {
                          std::wstring ta = ui::TypeString(a);
                          std::wstring tb = ui::TypeString(b);
                          c = _wcsicmp(ta.c_str(), tb.c_str());
                      }
                  } else {
                      c = CompareFileTime(&a.ft, &b.ft);
                  }
                  if (c == 0) c = _wcsicmp(a.name.c_str(), b.name.c_str());
                  return asc ? c < 0 : c > 0;
              });
}

void RefreshList() {
    ui::RefreshList(g_app.hList, g_app.listItems.size());
    ui::SetStatusText(g_app.hStatus, 3,
                      util::FormatCount(g_app.listItems.size()) + L" 项");
}

void LoadDir(const std::wstring& path) {
    g_app.listItems.clear();
    bool loaded = false;
    auto snap = g_app.index.Snapshot();
    if (snap && snap->complete) {
        auto pit = snap->byPath.find(util::ToLower(path));
        if (pit != snap->byPath.end()) {
            auto cit = snap->children.find(pit->second);
            if (cit != snap->children.end()) {
                g_app.listItems.reserve(cit->second.size());
                for (uint32_t id : cit->second) {
                    if (id >= snap->entries.size()) continue;
                    const Entry& e = snap->entries[id];
                    if (!g_app.showHidden && util::IsHiddenOrSystem(e.attrs)) continue;
                    ListItem item;
                    item.path = BuildPath(*snap, id);
                    item.name = e.name;
                    item.size = e.size;
                    item.ft = e.ft;
                    item.attrs = e.attrs;
                    item.isDir = (e.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    item.hidden = util::IsHiddenOrSystem(e.attrs);
                    g_app.listItems.push_back(std::move(item));
                }
            }
            loaded = true;
        }
    }
    if (!loaded) {
        EnumerateDirRealtime(path, g_app.showHidden, g_app.listItems);
    }
    SortItems();
    RefreshList();
}

void NavigateTo(const std::wstring& path, bool pushHistory = true) {
    std::wstring dir = util::NormalizeDir(path);
    DWORD attrs = GetFileAttributesW(util::AddLongPathPrefix(dir).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        ui::SetStatusText(g_app.hStatus, 0, L"路径无效：" + dir);
        return;
    }

    if (!g_app.searchMode && pushHistory) {
        if (g_app.history.empty() || g_app.history.back() != dir) {
            if (g_app.histPos == (size_t)-1) {
                g_app.history.clear();
            } else {
                g_app.history.resize(g_app.histPos + 1);
            }
            g_app.history.push_back(dir);
            g_app.histPos = g_app.history.size() - 1;
        }
    }
    g_app.currentDir = dir;
    SetWindowTextW(g_app.hAddr, DisplayPath(dir).c_str());
    LoadDir(dir);
    UpdateButtons();
    ui::SetStatusText(g_app.hStatus, 0, L"浏览：" + DisplayPath(dir));
}

void GoBack() {
    if (g_app.histPos > 0 && g_app.histPos <= g_app.history.size()) {
        --g_app.histPos;
        NavigateTo(g_app.history[g_app.histPos], false);
    }
}

void GoForward() {
    if (g_app.histPos + 1 < g_app.history.size()) {
        ++g_app.histPos;
        NavigateTo(g_app.history[g_app.histPos], false);
    }
}

void SetBrowseMode(bool reload = true) {
    if (!g_app.searchMode && g_app.listMode == LIST_MODE_BROWSE) return;
    g_app.searchMode = false;
    g_app.lastQuery.clear();
    if (g_app.listMode != LIST_MODE_BROWSE) {
        ui::SetListViewMode(g_app.hList, LIST_MODE_BROWSE);
    }
    g_app.sortCol = 0;
    g_app.sortAsc = true;
    if (reload && !g_app.currentDir.empty()) LoadDir(g_app.currentDir);
}

void DoSearch() {
    std::wstring q = GetWindowTextStr(g_app.hSearch);
    while (!q.empty() && (q.front() == L' ' || q.front() == L'\t')) q.erase(q.begin());
    while (!q.empty() && (q.back() == L' ' || q.back() == L'\t')) q.pop_back();
    if (q.empty()) {
        SetBrowseMode();
        return;
    }

    g_app.searchMode = true;
    g_app.lastQuery = q;
    ++g_app.searchGen;
    if (g_app.listMode != LIST_MODE_SEARCH) {
        ui::SetListViewMode(g_app.hList, LIST_MODE_SEARCH);
    }
    g_app.listItems.clear();
    RefreshList();
    ui::SetStatusText(g_app.hStatus, 0, L"正在搜索“" + q + L"”…");
    auto snap = g_app.index.Snapshot();
    StartBackgroundSearch(q, g_app.searchGen, g_app.showHidden, snap);
}

void ClearSearch() {
    KillTimer(g_app.hwnd, IDT_DEBOUNCE);
    g_app.suppressEdit = true;
    SetWindowTextW(g_app.hSearch, L"");
    g_app.suppressEdit = false;
    SetBrowseMode();
}

void NavigateToText(const std::wstring& text) {
    std::wstring t = text;
    if (t.size() >= 2 && t.front() == L'"' && t.back() == L'"') {
        t = t.substr(1, t.size() - 2);
    }
    if (t.empty()) return;
    DWORD attrs = GetFileAttributesW(util::AddLongPathPrefix(t).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        ui::SetStatusText(g_app.hStatus, 0, L"路径不存在：" + t);
        return;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        NavigateTo(t);
    } else {
        ShellExecuteW(g_app.hwnd, L"open", t.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void ActivateItem(int idx) {
    if (idx < 0 || (size_t)idx >= g_app.listItems.size()) return;
    const ListItem& it = g_app.listItems[idx];
    if (it.isDir) {
        if (g_app.searchMode) {
            g_app.suppressEdit = true;
            SetWindowTextW(g_app.hSearch, L"");
            g_app.suppressEdit = false;
            SetBrowseMode(false);
        }
        NavigateTo(it.path);
    } else {
        ShellExecuteW(g_app.hwnd, L"open", it.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void CopyPath(const std::wstring& path) {
    if (!OpenClipboard(g_app.hwnd)) return;
    EmptyClipboard();
    size_t bytes = (path.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg) {
        wchar_t* dst = (wchar_t*)GlobalLock(hg);
        if (dst) {
            wcscpy(dst, path.c_str());
            GlobalUnlock(hg);
            SetClipboardData(CF_UNICODETEXT, hg);
        } else {
            GlobalFree(hg);
        }
    }
    CloseClipboard();
}

// 绘制一个文本段，超出右边界时逐字符截断并补省略号
static int DrawSeg(HDC hdc, const std::wstring& seg, int x, int y, int right,
                   HFONT font, COLORREF color) {
    SelectObject(hdc, font);
    SetTextColor(hdc, color);
    SIZE sz{};
    if (!GetTextExtentPoint32W(hdc, seg.c_str(), (int)seg.size(), &sz)) {
        TextOutW(hdc, x, y, seg.c_str(), (int)seg.size());
        return x + (int)seg.size() * 8;
    }
    if (x + sz.cx <= right) {
        TextOutW(hdc, x, y, seg.c_str(), (int)seg.size());
        return x + sz.cx;
    }
    // 放不下：逐字符绘制到边界
    for (size_t i = 0; i < seg.size(); ++i) {
        SIZE csz{};
        GetTextExtentPoint32W(hdc, &seg[i], 1, &csz);
        if (x + csz.cx > right - 12) {
            TextOutW(hdc, x, y, L"...", 3);
            return right;
        }
        TextOutW(hdc, x, y, &seg[i], 1);
        x += csz.cx;
    }
    return x;
}

// 高亮绘制：匹配段使用粗体 + 高亮色，其余段普通字体
static void DrawHighlightedText(HDC hdc, const std::wstring& text,
                                const std::wstring& queryLower, const RECT& rc,
                                HFONT normalFont, HFONT boldFont,
                                COLORREF normalColor, COLORREF hlColor,
                                int iconW, int iconIdx) {
    SetBkMode(hdc, TRANSPARENT);
    int x = rc.left + 4;
    int right = rc.right - 4;
    if (iconW > 0) {
        HIMAGELIST il = g_app.icons.ImageList();
        if (il && iconIdx >= 0) {
            ImageList_Draw(il, iconIdx, hdc, rc.left + 2,
                           rc.top + (rc.bottom - rc.top - 16) / 2, ILD_TRANSPARENT);
        }
        x += iconW;
    }
    if (x >= right) return;

    std::vector<size_t> matches;
    std::wstring lower = util::ToLower(text);
    size_t pos = 0;
    while ((pos = lower.find(queryLower, pos)) != std::wstring::npos) {
        matches.push_back(pos);
        pos += queryLower.size();
    }
    if (matches.empty()) {
        SelectObject(hdc, normalFont);
        SetTextColor(hdc, normalColor);
        DrawTextW(hdc, text.c_str(), -1, const_cast<RECT*>(&rc),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        return;
    }

    HFONT oldFont = (HFONT)SelectObject(hdc, normalFont);
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    int y = rc.top + (rc.bottom - rc.top - tm.tmHeight) / 2;

    size_t ti = 0;
    size_t mi = 0;
    while (ti < text.size() && x < right) {
        size_t next = (mi < matches.size()) ? matches[mi] : text.size();
        if (ti < next) {
            x = DrawSeg(hdc, text.substr(ti, next - ti), x, y, right,
                        normalFont, normalColor);
            ti = next;
        } else if (mi < matches.size()) {
            size_t end = matches[mi] + queryLower.size();
            x = DrawSeg(hdc, text.substr(matches[mi], end - matches[mi]), x, y, right,
                        boldFont, hlColor);
            ti = end;
            ++mi;
        } else {
            break;
        }
    }
    SelectObject(hdc, oldFont);
}

static bool LoadDarkPref() {
    DWORD v = 0;
    DWORD sz = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Work4Tools", L"DarkMode",
                        RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS && v != 0;
}

static void SaveDarkPref(bool dark) {
    DWORD v = dark ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Work4Tools", L"DarkMode",
                    REG_DWORD, &v, sizeof(v));
}

void ShowContextMenu(int sx, int sy) {
    POINT pt{sx, sy};
    ScreenToClient(g_app.hList, &pt);
    LVHITTESTINFO ht{};
    ht.pt = pt;
    int idx = ListView_HitTest(g_app.hList, &ht);
    if (idx < 0) idx = ListView_GetNextItem(g_app.hList, -1, LVNI_SELECTED);
    if (idx < 0 || (size_t)idx >= g_app.listItems.size()) return;
    ListView_SetItemState(g_app.hList, idx, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"打开");
    AppendMenuW(menu, MF_STRING, 2, L"打开所在位置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"复制路径");
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                             sx, sy, 0, g_app.hwnd, nullptr);
    DestroyMenu(menu);

    const ListItem& it = g_app.listItems[idx];
    if (cmd == 1) {
        ActivateItem(idx);
    } else if (cmd == 2) {
        std::wstring args = L"/select,\"" + it.path + L"\"";
        ShellExecuteW(g_app.hwnd, L"open", L"explorer.exe", args.c_str(), nullptr,
                      SW_SHOWNORMAL);
    } else if (cmd == 3) {
        CopyPath(it.path);
    }
}

void InsertTreeChildren(HTREEITEM hItem, TreeNodeData* data) {
    if (data->loaded) return;
    data->loaded = true;

    std::vector<std::wstring> subdirs;
    bool fromIndex = false;
    auto snap = g_app.index.Snapshot();
    if (snap && snap->complete) {
        auto pit = snap->byPath.find(util::ToLower(data->path));
        if (pit != snap->byPath.end()) {
            auto cit = snap->children.find(pit->second);
            if (cit != snap->children.end()) {
                for (uint32_t id : cit->second) {
                    if (id >= snap->entries.size()) continue;
                    const Entry& e = snap->entries[id];
                    if (e.attrs & FILE_ATTRIBUTE_DIRECTORY) {
                        subdirs.push_back(BuildPath(*snap, id));
                    }
                }
            }
            fromIndex = true;
        }
    }
    if (!fromIndex) {
        std::wstring search = util::DirWildcard(data->path);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(search.c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const wchar_t* nm = fd.cFileName;
                if (nm[0] == L'.' &&
                    (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0)))
                    continue;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    subdirs.push_back(data->path + L"\\" + nm);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    for (auto& sd : subdirs) {
        auto* d = new TreeNodeData{sd, false};
        TVINSERTSTRUCTW ins{};
        ins.hParent = hItem;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
        std::wstring name = util::GetBaseName(sd);
        ins.item.pszText = (LPWSTR)name.c_str();
        ins.item.lParam = (LPARAM)d;
        ins.item.cChildren = 1;
        TreeView_InsertItem(g_app.hTree, &ins);
    }
    if (!subdirs.empty()) TreeView_SortChildren(g_app.hTree, hItem, 0);
}

void RefreshTreeRoots() {
    TreeView_DeleteAllItems(g_app.hTree);
    std::vector<std::wstring> roots;
    auto snap = g_app.index.Snapshot();
    if (snap && snap->complete && !snap->roots.empty()) {
        roots = snap->roots;
    } else {
        roots = util::GetFixedDrives();
    }
    for (auto& r : roots) {
        auto* d = new TreeNodeData{r, false};
        TVINSERTSTRUCTW ins{};
        ins.hParent = TVI_ROOT;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
        std::wstring label = DisplayPath(r);
        ins.item.pszText = (LPWSTR)label.c_str();
        ins.item.lParam = (LPARAM)d;
        ins.item.cChildren = 1;
        TreeView_InsertItem(g_app.hTree, &ins);
    }
}

void BuildMenu(HWND hwnd) {
    HMENU bar = CreateMenu();
    HMENU mFile = CreatePopupMenu();
    AppendMenuW(mFile, MF_STRING, IDM_EXIT, L"退出");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mFile, L"文件");

    HMENU mView = CreatePopupMenu();
    AppendMenuW(mView, MF_STRING, IDM_REFRESH, L"刷新");
    AppendMenuW(mView, MF_STRING, IDM_HIDDEN, L"显示隐藏文件");
    AppendMenuW(mView, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mView, MF_STRING, IDM_REBUILD, L"重建索引");
    AppendMenuW(mView, MF_STRING, IDM_INDEXDIR, L"打开索引目录");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mView, L"查看");

    HMENU mHelp = CreatePopupMenu();
    AppendMenuW(mHelp, MF_STRING, IDM_ABOUT, L"关于");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mHelp, L"帮助");
    SetMenu(hwnd, bar);
}

void UpdateStatusBar() {
    IndexProgress p = g_app.index.GetProgress();
    if (!g_app.searchMode) {
        ui::SetStatusText(g_app.hStatus, 0,
                          g_app.currentDir.empty() ? L"就绪" : L"浏览：" + DisplayPath(g_app.currentDir));
    }
    std::wstring s1;
    if (p.building) {
        s1 = L"索引构建中：" + util::FormatCount(p.files) + L" 项";
    } else {
        auto snap = g_app.index.Snapshot();
        if (snap && snap->complete) {
            s1 = L"索引就绪：" + util::FormatCount(snap->entries.size()) + L" 项";
        } else {
            s1 = L"索引未就绪，正在等待后台构建";
        }
    }
    ui::SetStatusText(g_app.hStatus, 1, s1);
}

LRESULT CALLBACK AddrEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        PostMessageW(g_app.hwnd, WM_COMMAND, IDC_GOTO, 0);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE && !g_app.currentDir.empty()) {
        SetWindowTextW(h, DisplayPath(g_app.currentDir).c_str());
        return 0;
    }
    return CallWindowProcW(g_app.origAddrProc, h, m, w, l);
}

LRESULT CALLBACK SearchEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        PostMessageW(g_app.hwnd, WM_COMMAND, IDC_SEARCHBTN, 0);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        ClearSearch();
        return 0;
    }
    return CallWindowProcW(g_app.origSearchProc, h, m, w, l);
}

// ListView 子类：拦截其子控件 Header 的 NM_CUSTOMDRAW。
// Header 的直接父窗口是 ListView——它把通知转发给主窗口，但不会把主窗口的
// CDRF 返回值回传给 Header，导致列头自绘失效（深色模式下列头仍为亮色）。
// 因此在 ListView 窗口过程内直接处理并返回 CDRF，让绘制指令直达 Header。
LRESULT CALLBACK ListProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NOTIFY) {
        NMHDR* nm = (NMHDR*)l;
        if (nm->hwndFrom == g_app.hHeader && nm->code == NM_CUSTOMDRAW) {
            // 列头（名称/大小/类型/修改日期）完全自绘，适配深浅色模式
            // （Header 的 NMCUSTOMDRAW 不提供 clrText/clrTextBk，必须自绘）
            NMCUSTOMDRAW* cd = (NMCUSTOMDRAW*)l;
            if (cd->dwDrawStage == CDDS_PREPAINT) {
                return CDRF_NOTIFYITEMDRAW;
            }
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                HDC hdc = cd->hdc;
                RECT rc = cd->rc;
                const bool dark = g_app.darkMode;
                COLORREF bg = dark ? RGB(32, 32, 32)
                                   : (COLORREF)GetSysColor(COLOR_WINDOW);
                if (cd->uItemState & CDIS_SELECTED) {
                    bg = dark ? RGB(48, 48, 48) : RGB(232, 232, 232);
                }
                HBRUSH br = CreateSolidBrush(bg);
                FillRect(hdc, &rc, br);
                DeleteObject(br);

                wchar_t buf[128] = {};
                HDITEMW hdi{};
                hdi.mask = HDI_TEXT;
                hdi.pszText = buf;
                hdi.cchTextMax = 127;
                if (SendMessageW(nm->hwndFrom, HDM_GETITEMW, (WPARAM)(int)cd->dwItemSpec,
                                 (LPARAM)&hdi)) {
                    SetBkMode(hdc, TRANSPARENT);
                    SetTextColor(hdc, dark ? RGB(235, 235, 235)
                                           : (COLORREF)GetSysColor(COLOR_WINDOWTEXT));
                    HGDIOBJ oldFont = nullptr;
                    if (g_app.hFont) {
                        oldFont = (HGDIOBJ)SelectObject(hdc, g_app.hFont);
                    }
                    RECT tr = rc;
                    tr.left += MulDiv(8, g_app.dpi, 96);
                    tr.right -= MulDiv(4, g_app.dpi, 96);
                    DrawTextW(hdc, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    if (oldFont) SelectObject(hdc, oldFont);
                }
                // 列间分隔线：画在本项右缘（相邻项背景从其 rc.left 起，不会覆盖此线）。
                // 不用 ITEMPOSTPAINT——CDRF_SKIPDEFAULT 会抑制后续通知，实测 POSTPAINT 不触发。
                COLORREF sep = dark ? RGB(70, 70, 70) : RGB(205, 205, 205);
                HPEN pen = CreatePen(PS_SOLID, 1, sep);
                HGDIOBJ oldPen = (HGDIOBJ)SelectObject(hdc, pen);
                MoveToEx(hdc, rc.right - 1, rc.top, nullptr);
                LineTo(hdc, rc.right - 1, rc.bottom);
                SelectObject(hdc, oldPen);
                DeleteObject(pen);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return CallWindowProcW(g_app.origListProc, h, m, w, l);
}

// Header 子类：经典样式的 3D 顶边框（亮白 + 灰两条线）在深色模式下刺眼，用背景色覆盖。
LRESULT CALLBACK HeaderProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        LRESULT r = CallWindowProcW(g_app.origHeaderProc, h, m, w, l);
        HDC hdc = GetDC(h);
        RECT rc;
        GetClientRect(h, &rc);
        COLORREF bg = g_app.darkMode ? RGB(32, 32, 32)
                                     : (COLORREF)GetSysColor(COLOR_WINDOW);
        HBRUSH br = CreateSolidBrush(bg);
        RECT top{rc.left, rc.top, rc.right, rc.top + 2};
        FillRect(hdc, &top, br);
        DeleteObject(br);
        ReleaseDC(h, hdc);
        return r;
    }
    return CallWindowProcW(g_app.origHeaderProc, h, m, w, l);
}

}  // namespace

void ui::ReattachHeaderSubclass() {
    if (!g_app.hHeader) return;
    SetWindowTheme(g_app.hHeader, L"", L"");
    // 同一窗口重复挂接时 SetWindowLongPtrW 会返回 HeaderProc 自身，
    // 此时不能覆写 origHeaderProc（否则 CallWindowProcW 无限递归导致栈溢出）
    WNDPROC prev = (WNDPROC)SetWindowLongPtrW(g_app.hHeader, GWLP_WNDPROC, (LONG_PTR)HeaderProc);
    if (prev != HeaderProc) {
        g_app.origHeaderProc = prev;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_app.hwnd = hwnd;
            g_hwnd = hwnd;
            g_app.dpi = ui::GetDpi(hwnd);
            g_app.hFont = ui::MakeFont(g_app.dpi);
            g_app.hFontBold = ui::MakeBoldFont(g_app.dpi);
            g_app.hBrushDark = CreateSolidBrush(RGB(32, 32, 32));
            g_app.hBrushSplit = CreateSolidBrush(RGB(205, 205, 205));
            BuildMenu(hwnd);
            ui::CreateControls(hwnd);
            // 列头自绘需在 ListView 窗口过程内处理（见 ListProc 注释）
            g_app.origListProc =
                (WNDPROC)SetWindowLongPtrW(g_app.hList, GWLP_WNDPROC, (LONG_PTR)ListProc);
            // CPU 线程数在运行期间不会变化，只初始化计算一次
            g_app.statusThreadText =
                L"CPU线程数 " + std::to_wstring(std::thread::hardware_concurrency());
            ui::SetStatusText(g_app.hStatus, 2, g_app.statusThreadText);
            g_app.origAddrProc =
                (WNDPROC)SetWindowLongPtrW(g_app.hAddr, GWLP_WNDPROC, (LONG_PTR)AddrEditProc);
            g_app.origSearchProc =
                (WNDPROC)SetWindowLongPtrW(g_app.hSearch, GWLP_WNDPROC, (LONG_PTR)SearchEditProc);
            g_app.icons.Attach(g_app.hList);
            ui::SetListViewMode(g_app.hList, LIST_MODE_BROWSE);
            RefreshTreeRoots();
            auto drives = util::GetFixedDrives();
            if (!drives.empty()) NavigateTo(drives[0], false);
            if (LoadDarkPref()) ui::SetDarkMode(true);
            g_app.index.StartAsyncLoad();
            SetTimer(hwnd, IDT_PROGRESS, 500, nullptr);
            return 0;
        }

        case WM_SIZE:
            ui::Layout(hwnd);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = (MINMAXINFO*)lParam;
            mm->ptMinTrackSize.x = MulDiv(640, g_app.dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(480, g_app.dpi, 96);
            return 0;
        }

        case WM_ERASEBKGND:
            {
                HDC hdc = (HDC)wParam;
                RECT rc;
                GetClientRect(hwnd, &rc);
                if (g_app.darkMode) {
                    FillRect(hdc, &rc, g_app.hBrushDark);
                } else {
                    FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
                }
                // 树与列表之间的竖向分隔线
                if (g_app.hBrushSplit && g_app.splitRect.right > g_app.splitRect.left) {
                    FillRect(hdc, &g_app.splitRect, g_app.hBrushSplit);
                }
                return 1;
            }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            // 树与列表之间的竖向分隔线（背景由 WM_ERASEBKGND 填充）
            if (g_app.hBrushSplit && g_app.splitRect.right > g_app.splitRect.left) {
                FillRect(hdc, &g_app.splitRect, g_app.hBrushSplit);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            if (g_app.darkMode) {
                HDC hdc = (HDC)wParam;
                SetTextColor(hdc, RGB(235, 235, 235));
                SetBkColor(hdc, RGB(32, 32, 32));
                SetBkMode(hdc, OPAQUE);
                return (LRESULT)g_app.hBrushDark;
            }
            break;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
            if (g_app.darkMode) {
                HDC hdc = (HDC)wParam;
                SetTextColor(hdc, RGB(235, 235, 235));
                SetBkColor(hdc, RGB(32, 32, 32));
                SetBkMode(hdc, OPAQUE);
                return (LRESULT)g_app.hBrushDark;
            }
            break;

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
            if (dis->CtlType != ODT_BUTTON || !dis->hwndItem) break;
            // "显示隐藏文件"复选框：自绘勾选框 + 文字，适配深浅色模式
            // （主题化复选框忽略 CTLCOLOR 文字色，深色模式下文字会保持黑色不可见）
            if (dis->CtlID == (UINT)IDC_HIDDEN) {
                HDC hdc = dis->hDC;
                RECT rc = dis->rcItem;
                // BS_OWNERDRAW 按钮不保存勾选状态（BM_GETCHECK 恒为 0），
                // 状态由应用自己的 g_app.showHidden 管理
                bool checked = g_app.showHidden;
                bool disabled = (dis->itemState & ODS_DISABLED) != 0;
                bool focus = (dis->itemState & ODS_FOCUS) != 0;
                const bool dark = g_app.darkMode;

                COLORREF bg = dark ? RGB(32, 32, 32)
                                   : (COLORREF)GetSysColor(COLOR_WINDOW);
                COLORREF fg = disabled ? (dark ? RGB(120, 120, 120) : RGB(160, 160, 160))
                                       : (dark ? RGB(235, 235, 235) : RGB(25, 25, 25));
                COLORREF boxBorder = disabled ? (dark ? RGB(80, 80, 80) : RGB(190, 190, 190))
                                              : (dark ? RGB(130, 130, 130) : RGB(110, 110, 110));
                COLORREF boxFill = checked ? (dark ? RGB(56, 189, 248) : RGB(0, 102, 204))
                                           : (dark ? RGB(45, 45, 45) : RGB(250, 250, 250));

                HBRUSH bgBr = CreateSolidBrush(bg);
                FillRect(hdc, &rc, bgBr);
                DeleteObject(bgBr);

                int box = MulDiv(15, g_app.dpi, 96);
                int boxX = rc.left + MulDiv(4, g_app.dpi, 96);
                int boxY = rc.top + (rc.bottom - rc.top - box) / 2;

                HBRUSH bf = CreateSolidBrush(boxFill);
                HPEN bp = CreatePen(PS_SOLID, 1, boxBorder);
                HGDIOBJ oPen = SelectObject(hdc, bp);
                HGDIOBJ oBr = SelectObject(hdc, bf);
                Rectangle(hdc, boxX, boxY, boxX + box, boxY + box);
                SelectObject(hdc, oPen);
                SelectObject(hdc, oBr);
                DeleteObject(bp);
                DeleteObject(bf);

                if (checked) {
                    HPEN cp = CreatePen(PS_SOLID, std::max(1, MulDiv(2, g_app.dpi, 96)),
                                        RGB(255, 255, 255));
                    HGDIOBJ oP = SelectObject(hdc, cp);
                    int cx = boxX + MulDiv(3, g_app.dpi, 96);
                    int cy = boxY + box / 2 + MulDiv(1, g_app.dpi, 96);
                    int mx = boxX + box / 2 - MulDiv(1, g_app.dpi, 96);
                    int my = boxY + box - MulDiv(4, g_app.dpi, 96);
                    int ex = boxX + box - MulDiv(3, g_app.dpi, 96);
                    int ey = boxY + MulDiv(4, g_app.dpi, 96);
                    MoveToEx(hdc, cx, cy, nullptr);
                    LineTo(hdc, mx, my);
                    LineTo(hdc, ex, ey);
                    SelectObject(hdc, oP);
                    DeleteObject(cp);
                }

                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, fg);
                HFONT oldFont = (HFONT)SelectObject(hdc, g_app.hFont);
                wchar_t text[64];
                GetWindowTextW(dis->hwndItem, text, 64);
                RECT tr = rc;
                tr.left = boxX + box + MulDiv(7, g_app.dpi, 96);
                DrawTextW(hdc, text, -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(hdc, oldFont);
                if (focus) {
                    RECT fr = tr;
                    InflateRect(&fr, 1, 2);
                    DrawFocusRect(hdc, &fr);
                }
                return TRUE;
            }
            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            bool hover = (dis->itemState & ODS_HOTLIGHT) != 0;
            bool disabled = (dis->itemState & ODS_DISABLED) != 0;
            bool focus = (dis->itemState & ODS_FOCUS) != 0;

            COLORREF bg, fg, border;
            if (g_app.darkMode) {
                bg = pressed ? RGB(28, 28, 28)
                             : (hover ? RGB(62, 62, 62) : RGB(45, 45, 45));
                fg = disabled ? RGB(120, 120, 120) : RGB(235, 235, 235);
                border = hover ? RGB(115, 115, 115) : RGB(70, 70, 70);
            } else {
                bg = pressed ? RGB(224, 224, 224)
                             : (hover ? RGB(249, 249, 249) : RGB(243, 243, 243));
                fg = disabled ? RGB(160, 160, 160) : RGB(25, 25, 25);
                border = hover ? RGB(140, 140, 140) : RGB(196, 196, 196);
            }

            HBRUSH br = CreateSolidBrush(bg);
            HPEN pen = CreatePen(PS_SOLID, 1, border);
            HGDIOBJ oPen = SelectObject(hdc, pen);
            HGDIOBJ oBr = SelectObject(hdc, br);
            RoundRect(hdc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, 6, 6);
            SelectObject(hdc, oPen);
            SelectObject(hdc, oBr);
            DeleteObject(pen);
            DeleteObject(br);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, fg);
            HFONT oldFont = (HFONT)SelectObject(hdc, g_app.hFont);
            wchar_t text[64];
            GetWindowTextW(dis->hwndItem, text, 64);
            RECT tr = rc;
            if (pressed) {
                tr.top += 1;
                tr.left += 1;
            }
            DrawTextW(hdc, text, -1, &tr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (focus) {
                RECT fr = rc;
                InflateRect(&fr, -3, -3);
                DrawFocusRect(hdc, &fr);
            }
            SelectObject(hdc, oldFont);
            return TRUE;
        }

        case WM_DPICHANGED: {
            g_app.dpi = HIWORD(wParam);
            if (g_app.hFont) DeleteObject(g_app.hFont);
            g_app.hFont = ui::MakeFont(g_app.dpi);
            if (g_app.hFontBold) DeleteObject(g_app.hFontBold);
            g_app.hFontBold = ui::MakeBoldFont(g_app.dpi);
            ui::ApplyFont(hwnd);
            RECT* r = (RECT*)lParam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ui::Layout(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_BACK:
                    GoBack();
                    break;
                case IDC_FWD:
                    GoForward();
                    break;
                case IDC_UP:
                    if (!g_app.currentDir.empty())
                        NavigateTo(util::GetParentDir(g_app.currentDir));
                    break;
                case IDC_REFRESH:
                case IDM_REFRESH:
                    if (g_app.searchMode) {
                        DoSearch();
                    } else if (!g_app.currentDir.empty()) {
                        LoadDir(g_app.currentDir);
                    }
                    break;
                case IDC_REBUILD:
                case IDM_REBUILD:
                    if (MessageBoxW(hwnd, L"确定要重建全盘文件索引吗？\n重建期间旧索引仍可继续搜索。",
                                    L"重建索引", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        g_app.index.StartRebuild();
                    }
                    break;
                case IDC_DARK:
                    ui::SetDarkMode(!g_app.darkMode);
                    SaveDarkPref(g_app.darkMode);
                    break;
                case IDC_HIDDEN:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        // 自绘复选框（BS_OWNERDRAW）不保存勾选状态，直接翻转应用状态
                        g_app.showHidden = !g_app.showHidden;
                        InvalidateRect(g_app.hChkHidden, nullptr, TRUE);
                        CheckMenuItem(GetMenu(hwnd), IDM_HIDDEN,
                                      MF_BYCOMMAND |
                                          (g_app.showHidden ? MF_CHECKED : MF_UNCHECKED));
                        if (g_app.searchMode) {
                            DoSearch();
                        } else if (!g_app.currentDir.empty()) {
                            LoadDir(g_app.currentDir);
                        }
                    }
                    break;
                case IDM_HIDDEN: {
                    g_app.showHidden = !g_app.showHidden;
                    InvalidateRect(g_app.hChkHidden, nullptr, TRUE);
                    CheckMenuItem(GetMenu(hwnd), IDM_HIDDEN,
                                  MF_BYCOMMAND |
                                      (g_app.showHidden ? MF_CHECKED : MF_UNCHECKED));
                    if (g_app.searchMode) {
                        DoSearch();
                    } else if (!g_app.currentDir.empty()) {
                        LoadDir(g_app.currentDir);
                    }
                    break;
                }
                case IDC_GOTO: {
                    std::wstring t = GetWindowTextStr(g_app.hAddr);
                    NavigateToText(t);
                    break;
                }
                case IDC_SEARCHBTN:
                    KillTimer(hwnd, IDT_DEBOUNCE);
                    DoSearch();
                    break;
                case IDC_CLEAR:
                    ClearSearch();
                    break;
                case IDC_SEARCH:
                    if (HIWORD(wParam) == EN_CHANGE && !g_app.suppressEdit) {
                        SetTimer(hwnd, IDT_DEBOUNCE, 300, nullptr);
                    }
                    break;
                case IDC_ADDR:
                    break;
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    break;
                case IDM_INDEXDIR: {
                    std::wstring dir = util::GetExeDir() + L"\\index";
                    CreateDirectoryW(dir.c_str(), nullptr);
                    ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                }
                case IDM_ABOUT:
                    MessageBoxW(hwnd,
                                L"Work4 文件快搜 v1.0\n\n"
                                L"C++17 + Win32 原生实现，多线程全盘索引，毫秒级搜索。\n"
                                L"索引文件位置：程序目录\\index\\file-index.bin",
                                L"关于", MB_OK | MB_ICONINFORMATION);
                    break;
            }
            return 0;

        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)lParam;
            if (nm->hwndFrom == g_app.hTree) {
                switch (nm->code) {
                    case TVN_ITEMEXPANDING: {
                        NMTREEVIEWW* tv = (NMTREEVIEWW*)lParam;
                        if (tv->action == TVE_EXPAND) {
                            TreeNodeData* d = (TreeNodeData*)tv->itemNew.lParam;
                            if (d && !d->loaded) InsertTreeChildren(tv->itemNew.hItem, d);
                        }
                        return 0;
                    }
                    case TVN_SELCHANGED: {
                        NMTREEVIEWW* tv = (NMTREEVIEWW*)lParam;
                        TreeNodeData* d = (TreeNodeData*)tv->itemNew.lParam;
                        if (d) {
                            if (g_app.searchMode) {
                                g_app.suppressEdit = true;
                                SetWindowTextW(g_app.hSearch, L"");
                                g_app.suppressEdit = false;
                                SetBrowseMode(false);
                            }
                            NavigateTo(d->path);
                        }
                        return 0;
                    }
                    case TVN_DELETEITEM: {
                        NMTREEVIEWW* tv = (NMTREEVIEWW*)lParam;
                        delete (TreeNodeData*)tv->itemOld.lParam;
                        return 0;
                    }
                    case NM_CUSTOMDRAW: {
                        NMTVCUSTOMDRAW* cd = (NMTVCUSTOMDRAW*)lParam;
                        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                            return g_app.darkMode ? CDRF_NOTIFYITEMDRAW : CDRF_DODEFAULT;
                        }
                        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && g_app.darkMode) {
                            bool selected =
                                (TreeView_GetItemState(g_app.hTree,
                                                       (HTREEITEM)cd->nmcd.dwItemSpec,
                                                       TVIS_SELECTED) &
                                 TVIS_SELECTED) != 0;
                            cd->clrText = selected
                                              ? (COLORREF)GetSysColor(COLOR_HIGHLIGHTTEXT)
                                              : RGB(235, 235, 235);
                            cd->clrTextBk = selected
                                                ? (COLORREF)GetSysColor(COLOR_HIGHLIGHT)
                                                : RGB(32, 32, 32);
                            return CDRF_NEWFONT;
                        }
                        break;
                    }
                }
            } else if (nm->hwndFrom == g_app.hList) {
                switch (nm->code) {
                    case LVN_GETDISPINFOW: {
                        NMLVDISPINFOW* di = (NMLVDISPINFOW*)lParam;
                        int idx = di->item.iItem;
                        if (idx < 0 || (size_t)idx >= g_app.listItems.size()) break;
                        const ListItem& it = g_app.listItems[idx];
                        if (di->item.mask & LVIF_TEXT) {
                            std::wstring text;
                            switch (di->item.iSubItem) {
                                case 0:
                                    text = it.name;
                                    break;
                                case 1:
                                    if (g_app.listMode == LIST_MODE_SEARCH) {
                                        text = it.path;
                                    } else {
                                        text = it.isDir ? L"" : util::FormatSize(it.size);
                                    }
                                    break;
                                case 2:
                                    if (g_app.listMode == LIST_MODE_SEARCH) {
                                        text = it.isDir ? L"" : util::FormatSize(it.size);
                                    } else {
                                        text = ui::TypeString(it);
                                    }
                                    break;
                                default:
                                    text = util::FormatTime(it.ft);
                                    break;
                            }
                            if (text.size() >= 2047) text.resize(2046);
                            wcscpy(g_app.dispBuf, text.c_str());
                            di->item.pszText = g_app.dispBuf;
                        }
                        if (di->item.mask & LVIF_IMAGE) {
                            di->item.iImage = g_app.icons.Get(it);
                        }
                        return 0;
                    }
                    case LVN_COLUMNCLICK: {
                        NMLISTVIEW* lv = (NMLISTVIEW*)lParam;
                        if (g_app.sortCol == lv->iSubItem) {
                            g_app.sortAsc = !g_app.sortAsc;
                        } else {
                            g_app.sortCol = lv->iSubItem;
                            g_app.sortAsc = true;
                        }
                        SortItems();
                        InvalidateRect(g_app.hList, nullptr, TRUE);
                        return 0;
                    }
                    case LVN_ITEMACTIVATE: {
                        NMITEMACTIVATE* ia = (NMITEMACTIVATE*)lParam;
                        ActivateItem(ia->iItem);
                        return 0;
                    }
                    case NM_CUSTOMDRAW: {
                        NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
                        bool highlight = g_app.searchMode &&
                                         !g_app.lastQuery.empty() &&
                                         !util::HasWildcard(g_app.lastQuery);
                        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                            return (g_app.darkMode || highlight) ? CDRF_NOTIFYITEMDRAW
                                                                 : CDRF_DODEFAULT;
                        }
                        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                            if (highlight) return CDRF_NOTIFYSUBITEMDRAW;
                            if (g_app.darkMode) {
                                bool selected =
                                    (ListView_GetItemState(
                                         g_app.hList, (int)cd->nmcd.dwItemSpec,
                                         LVIS_SELECTED) &
                                     LVIS_SELECTED) != 0;
                                cd->clrText = selected
                                                  ? (COLORREF)GetSysColor(COLOR_HIGHLIGHTTEXT)
                                                  : RGB(235, 235, 235);
                                cd->clrTextBk = selected
                                                    ? (COLORREF)GetSysColor(COLOR_HIGHLIGHT)
                                                    : RGB(32, 32, 32);
                                return CDRF_NEWFONT;
                            }
                            return CDRF_DODEFAULT;
                        }
                        if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                            if (!highlight) return CDRF_DODEFAULT;
                            int row = (int)cd->nmcd.dwItemSpec;
                            int col = cd->iSubItem;
                            if (row < 0 || (size_t)row >= g_app.listItems.size())
                                return CDRF_DODEFAULT;
                            if (col != 0 && col != 1) return CDRF_DODEFAULT;

                            const ListItem& it = g_app.listItems[row];
                            std::wstring text = (col == 0) ? it.name : it.path;
                            RECT rc{};
                            ListView_GetSubItemRect(g_app.hList, row, col, LVIR_BOUNDS, &rc);

                            bool selected =
                                (ListView_GetItemState(g_app.hList, row, LVIS_SELECTED) &
                                 LVIS_SELECTED) != 0;
                            COLORREF bgColor =
                                selected ? (COLORREF)GetSysColor(COLOR_HIGHLIGHT)
                                         : (g_app.darkMode ? RGB(32, 32, 32)
                                                           : (COLORREF)GetSysColor(COLOR_WINDOW));
                            HBRUSH bgBrush = CreateSolidBrush(bgColor);
                            FillRect(cd->nmcd.hdc, &rc, bgBrush);
                            DeleteObject(bgBrush);

                            COLORREF normalFg =
                                selected ? (COLORREF)GetSysColor(COLOR_HIGHLIGHTTEXT)
                                         : (g_app.darkMode ? RGB(235, 235, 235)
                                                           : (COLORREF)GetSysColor(COLOR_WINDOWTEXT));
                            COLORREF hlFg = selected ? RGB(255, 215, 90)
                                                     : (g_app.darkMode ? RGB(56, 189, 248)
                                                                       : RGB(0, 102, 204));
                            int iconW = 0;
                            int iconIdx = -1;
                            if (col == 0) {
                                iconW = MulDiv(22, g_app.dpi, 96);
                                iconIdx = g_app.icons.Get(it);
                            }
                            DrawHighlightedText(cd->nmcd.hdc, text,
                                                util::ToLower(g_app.lastQuery), rc,
                                                g_app.hFont, g_app.hFontBold,
                                                normalFg, hlFg, iconW, iconIdx);
                            return CDRF_SKIPDEFAULT;
                        }
                        break;
                    }
                }
            }
            break;
        }

        case WM_CONTEXTMENU:
            if ((HWND)wParam == g_app.hList) {
                ShowContextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;
            }
            break;

        case WM_TIMER:
            if (wParam == IDT_PROGRESS) {
                UpdateStatusBar();
            } else if (wParam == IDT_DEBOUNCE) {
                KillTimer(hwnd, IDT_DEBOUNCE);
                DoSearch();
            }
            return 0;

        case WM_APP_INDEX_LOADED:
            RefreshTreeRoots();
            UpdateStatusBar();
            g_app.index.StartRebuild();
            return 0;

        case WM_APP_INDEX_DONE:
            RefreshTreeRoots();
            if (g_app.searchMode) {
                DoSearch();
            } else if (!g_app.currentDir.empty()) {
                LoadDir(g_app.currentDir);
            }
            UpdateStatusBar();
            return 0;

        case WM_APP_SEARCH_DONE: {
            SearchPayload* p = (SearchPayload*)lParam;
            if (!p) break;
            if ((int)wParam != g_app.searchGen) {
                delete p;
                break;
            }
            g_app.listItems = std::move(p->items);
            g_app.sortCol = 0;
            g_app.sortAsc = true;
            SortItems();
            ui::RefreshList(g_app.hList, g_app.listItems.size());
            std::wstring st = L"搜索“" + p->query + L"”：";
            if (p->fromIndex) st += L"索引检索 ";
            st += util::FormatCount(g_app.listItems.size()) + L" 个结果（" +
                  std::to_wstring(p->elapsedMs) + L" ms）";
            if (p->truncated) st += L"（结果过多，已截断）";
            ui::SetStatusText(g_app.hStatus, 0, st);
            ui::SetStatusText(g_app.hStatus, 3,
                              util::FormatCount(g_app.listItems.size()) + L" 项");
            delete p;
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, IDT_PROGRESS);
            KillTimer(hwnd, IDT_DEBOUNCE);
            g_app.index.CancelAndWait();
            if (g_app.hFont) {
                DeleteObject(g_app.hFont);
                g_app.hFont = nullptr;
            }
            if (g_app.hFontBold) {
                DeleteObject(g_app.hFontBold);
                g_app.hFontBold = nullptr;
            }
            if (g_app.hBrushDark) {
                DeleteObject(g_app.hBrushDark);
                g_app.hBrushDark = nullptr;
            }
            if (g_app.hBrushSplit) {
                DeleteObject(g_app.hBrushSplit);
                g_app.hBrushSplit = nullptr;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_app.hInst = hInstance;
    SetUnhandledExceptionFilter(CrashHandler);
    ui::EnableDpiAwareness();

    INITCOMMONCONTROLSEX icc{sizeof(icc),
                             ICC_WIN95_CLASSES | ICC_TREEVIEW_CLASSES |
                                 ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Work4ToolsSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"Work4ToolsMainWnd", nullptr);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(mutex);
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"Work4ToolsMainWnd";
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    int dpi = ui::GetDpi(nullptr);
    int w = MulDiv(1280, dpi, 96);
    int h = MulDiv(820, dpi, 96);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Work4 文件快搜",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(mutex);
    return (int)msg.wParam;
}
