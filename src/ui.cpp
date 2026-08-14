#include "ui.h"

#include "app.h"
#include "utils.h"

#include <commctrl.h>
#include <cstdio>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#ifndef SB_SETTEXTCOLOR
#define SB_SETTEXTCOLOR (WM_USER + 13)
#endif
#ifndef SB_SETBKCOLOR
#define SB_SETBKCOLOR (WM_USER + 14)
#endif
#ifndef HDM_SETBKCOLOR
#define HDM_SETBKCOLOR (HDM_FIRST + 13)
#endif
#ifndef HDM_SETTEXTCOLOR
#define HDM_SETTEXTCOLOR (HDM_FIRST + 14)
#endif

namespace ui {

namespace {

BOOL CALLBACK EnumFontProc(HWND h, LPARAM lp) {
    SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
    return TRUE;
}

HWND MakeButton(HWND parent, int id, const wchar_t* text, DWORD style) {
    return CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | style,
                           0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, g_app.hInst, nullptr);
}

HWND MakeLabel(HWND parent, int id, const wchar_t* text) {
    return CreateWindowExW(0, L"STATIC", text,
                           WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                           0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, g_app.hInst, nullptr);
}

HWND MakeEdit(HWND parent, int id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                           0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, g_app.hInst, nullptr);
}

// 批量布局辅助：优先走 DeferWindowPos（减少拖动缩放时的闪烁），失败则直接定位
static HDWP Dwp(HDWP h, HWND w, int x, int y, int cx, int cy) {
    if (h) {
        return DeferWindowPos(h, w, nullptr, x, y, cx, cy,
                              SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SetWindowPos(w, nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
    return h;
}

}  // namespace

int GetDpi(HWND hwnd) {
    static auto fn = (UINT(WINAPI*)(HWND))(void*)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn) {
        int dpi = (int)fn(hwnd);
        if (dpi > 0) return dpi;
    }
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
}

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setCtx = (BOOL(WINAPI*)(HANDLE))(void*)GetProcAddress(
        user32, "SetProcessDpiAwarenessContext");
    if (setCtx) {
        // DPI_AWARENESS_CONTEXT_SYSTEM_AWARE（预览版 comctl 的 per-monitor 路径有崩溃风险）
        if (setCtx((HANDLE)-5)) return;
    }
    HMODULE shcore = GetModuleHandleW(L"shcore.dll");
    auto setAware = (HRESULT(WINAPI*)(int))(void*)GetProcAddress(
        shcore, "SetProcessDpiAwareness");
    if (setAware && SUCCEEDED(setAware(2))) return;
    SetProcessDPIAware();
}

HFONT MakeFont(int dpi) {
    return CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

HFONT MakeBoldFont(int dpi) {
    return CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void CreateControls(HWND parent) {
    g_app.hBtnBack = MakeButton(parent, IDC_BACK, L"后退", 0);
    g_app.hBtnFwd = MakeButton(parent, IDC_FWD, L"前进", 0);
    g_app.hBtnUp = MakeButton(parent, IDC_UP, L"向上", 0);
    g_app.hBtnRefresh = MakeButton(parent, IDC_REFRESH, L"刷新", 0);
    g_app.hBtnRebuild = MakeButton(parent, IDC_REBUILD, L"重建索引", 0);
    g_app.hBtnDark = MakeButton(parent, IDC_DARK, L"深色模式", 0);
    g_app.hChkHidden = CreateWindowExW(0, L"BUTTON", L"显示隐藏文件",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                       0, 0, 0, 0, parent, (HMENU)(INT_PTR)IDC_HIDDEN,
                                       g_app.hInst, nullptr);

    MakeLabel(parent, IDC_ADDR_LABEL, L"地址：");
    g_app.hAddr = MakeEdit(parent, IDC_ADDR);
    g_app.hBtnGoto = MakeButton(parent, IDC_GOTO, L"转到", 0);

    MakeLabel(parent, IDC_SEARCH_LABEL, L"搜索：");
    g_app.hSearch = MakeEdit(parent, IDC_SEARCH);
    g_app.hBtnSearch = MakeButton(parent, IDC_SEARCHBTN, L"搜索", 0);
    g_app.hBtnClear = MakeButton(parent, IDC_CLEAR, L"清除", 0);

    g_app.hTree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                                      TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
                                  0, 0, 0, 0, parent, (HMENU)(INT_PTR)IDC_TREE,
                                  g_app.hInst, nullptr);
    g_app.hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
                                  0, 0, 0, 0, parent, (HMENU)(INT_PTR)IDC_LIST,
                                  g_app.hInst, nullptr);
    ListView_SetExtendedListViewStyle(g_app.hList,
                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                          LVS_EX_HEADERDRAGDROP);
    g_app.hHeader = ListView_GetHeader(g_app.hList);
    // 主题化 Header 的 NM_CUSTOMDRAW/HDM_SETBKCOLOR 均不可靠，取消主题走经典样式
    SetWindowTheme(g_app.hHeader, L"", L"");
    g_app.hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                    WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                    0, 0, 0, 0, parent, (HMENU)(INT_PTR)IDC_STATUS,
                                    g_app.hInst, nullptr);
    SetWindowTheme(g_app.hTree, L"Explorer", nullptr);
    SetWindowTheme(g_app.hList, L"Explorer", nullptr);
    SetWindowTheme(g_app.hStatus, L"", nullptr);
    ApplyFont(parent);
}

void Layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right;
    int H = rc.bottom;
    int S = g_app.dpi * 100 / 96;  // 近似缩放基数
    auto s = [S](int v) { return MulDiv(v, S, 100); };

    // 状态栏固定贴在客户区底部（此前未定位，最大化/缩放时会错位）
    int statusH = s(24);
    int statusTop = H - statusH;
    HDWP hdwp = BeginDeferWindowPos(14);

    int gap = s(7);
    int btnH = s(26);
    int labelW = s(44);
    int margin = s(6);

    // --- 工具栏：固定按钮 + 自适应复选框 ---
    int x = margin;
    int y = s(4);
    auto place = [&](HWND h, int w) {
        hdwp = Dwp(hdwp, h, x, y, w, btnH);
        x += w + gap;
    };
    place(g_app.hBtnBack, s(52));
    place(g_app.hBtnFwd, s(52));
    place(g_app.hBtnUp, s(52));
    place(g_app.hBtnRefresh, s(52));
    place(g_app.hBtnRebuild, s(84));
    place(g_app.hBtnDark, s(84));
    int remain = W - x - margin;
    bool showChk = remain >= s(96);
    if (showChk) {
        int chkW = s(120);
        if (remain < chkW) chkW = remain;
        place(g_app.hChkHidden, chkW);
    }
    ShowWindow(g_app.hChkHidden, showChk ? SW_SHOW : SW_HIDE);

    // --- 地址行（右对齐：转到按钮靠右，Edit 弹性拉伸） ---
    y += btnH + s(6);
    int addrY = y;
    int gotoRight = W - margin;
    int gotoX = gotoRight - s(56);
    int addrEditLeft = margin + labelW + gap;
    int addrEditW = gotoX - gap - addrEditLeft;
    bool showGoto = addrEditW >= s(80);
    if (!showGoto) {
        addrEditW = W - margin - addrEditLeft;  // 隐藏转到按钮，Edit 用满剩余宽度
    }
    hdwp = Dwp(hdwp, GetDlgItem(hwnd, IDC_ADDR_LABEL), margin, addrY, labelW, btnH);
    hdwp = Dwp(hdwp, g_app.hAddr, addrEditLeft, addrY, addrEditW, btnH);
    if (showGoto) {
        hdwp = Dwp(hdwp, g_app.hBtnGoto, gotoX, addrY, s(56), btnH);
    }
    ShowWindow(g_app.hBtnGoto, showGoto ? SW_SHOW : SW_HIDE);

    // --- 搜索行（搜索/清除按钮靠右，Edit 弹性拉伸） ---
    y += btnH + s(6);
    int searchY = y;
    int clearRight = W - margin;
    int clearX = clearRight - s(56);
    int searchBtnX = clearX - gap - s(56);
    int searchEditLeft = margin + labelW + gap;
    int searchEditW = searchBtnX - gap - searchEditLeft;
    bool showSearchBtn = searchEditW >= s(80);
    if (!showSearchBtn) {
        searchEditW = clearX - gap - searchEditLeft;  // 隐藏搜索按钮，Edit 到清除按钮前
    }
    hdwp = Dwp(hdwp, GetDlgItem(hwnd, IDC_SEARCH_LABEL), margin, searchY, labelW, btnH);
    hdwp = Dwp(hdwp, g_app.hSearch, searchEditLeft, searchY, searchEditW, btnH);
    if (showSearchBtn) {
        hdwp = Dwp(hdwp, g_app.hBtnSearch, searchBtnX, searchY, s(56), btnH);
    }
    ShowWindow(g_app.hBtnSearch, showSearchBtn ? SW_SHOW : SW_HIDE);
    hdwp = Dwp(hdwp, g_app.hBtnClear, clearX, searchY, s(56), btnH);

    // --- 树与列表（树宽自适应，列表弹性） ---
    int top = y + btnH + s(6);
    int bottom = statusTop;
    int treeW = s(250);
    if (treeW > W / 3) treeW = W / 3;  // 窄窗口压缩树宽
    int listX = treeW + s(12);
    int listW = W - treeW - s(18);
    if (listW < s(80)) listW = s(80);  // 防御兜底
    hdwp = Dwp(hdwp, g_app.hStatus, 0, statusTop, W, statusH);
    hdwp = Dwp(hdwp, g_app.hTree, margin, top, treeW, bottom - top);
    hdwp = Dwp(hdwp, g_app.hList, listX, top, listW, bottom - top);

    if (hdwp) EndDeferWindowPos(hdwp);
    // 记录分隔线位置（树与列表间隙中间），由 WM_ERASEBKGND 绘制
    g_app.splitRect = {treeW + s(9), top, treeW + s(9) + s(2), bottom};
    AdjustListColumns(g_app.hList, listW);
    SetStatusParts(g_app.hStatus, W);
}

void ApplyFont(HWND parent) {
    if (!g_app.hFont) return;
    EnumChildWindows(parent, EnumFontProc, (LPARAM)g_app.hFont);
}

void SetDarkMode(bool dark) {
    g_app.darkMode = dark;
    if (!g_app.hwnd) return;

    if (g_app.hBrushSplit) DeleteObject(g_app.hBrushSplit);
    g_app.hBrushSplit = CreateSolidBrush(dark ? RGB(60, 60, 60) : RGB(205, 205, 205));

    BOOL d = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_app.hwnd, 20, &d, sizeof(d));  // DWMWA_USE_IMMERSIVE_DARK_MODE

    // 目录树/文件列表整体背景与文字色（行级颜色由 Custom Draw 处理）
    if (dark) {
        TreeView_SetBkColor(g_app.hTree, RGB(32, 32, 32));
        TreeView_SetTextColor(g_app.hTree, RGB(235, 235, 235));
        ListView_SetBkColor(g_app.hList, RGB(32, 32, 32));
        ListView_SetTextColor(g_app.hList, RGB(235, 235, 235));
        ListView_SetTextBkColor(g_app.hList, RGB(32, 32, 32));
    } else {
        // 显式恢复系统浅色（CLR_DEFAULT 在本系统上恢复失败，会残留深色背景）
        TreeView_SetBkColor(g_app.hTree, GetSysColor(COLOR_WINDOW));
        TreeView_SetTextColor(g_app.hTree, GetSysColor(COLOR_WINDOWTEXT));
        ListView_SetBkColor(g_app.hList, GetSysColor(COLOR_WINDOW));
        ListView_SetTextColor(g_app.hList, GetSysColor(COLOR_WINDOWTEXT));
        ListView_SetTextBkColor(g_app.hList, GetSysColor(COLOR_WINDOW));
    }

    SetWindowTextW(g_app.hBtnDark, dark ? L"浅色模式" : L"深色模式");
    InvalidateRect(g_app.hwnd, nullptr, TRUE);
    RedrawWindow(g_app.hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void SetListViewMode(HWND hlist, int mode) {
    g_app.listMode = mode;
    while (ListView_DeleteColumn(hlist, 0)) {
    }

    struct Col {
        const wchar_t* title;
        int width;
    };
    static const Col browseCols[] = {
        {L"名称", 260}, {L"大小", 105}, {L"类型", 140}, {L"修改日期", 150}};
    static const Col searchCols[] = {
        {L"名称", 220}, {L"路径", 420}, {L"大小", 105}, {L"修改日期", 150}};
    const Col* cols = (mode == LIST_MODE_SEARCH) ? searchCols : browseCols;
    size_t n = (mode == LIST_MODE_SEARCH) ? 4 : 4;

    for (size_t i = 0; i < n; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.iSubItem = (int)i;
        col.cx = MulDiv(cols[i].width, g_app.dpi, 96);
        col.pszText = (LPWSTR)cols[i].title;
        ListView_InsertColumn(hlist, (int)i, &col);
    }
    // 增删列可能导致 Header 控件被重建：重新获取句柄，并重挂子类/保持经典样式
    // （列头自绘依赖 g_app.hHeader，见 main.cpp ListProc）
    g_app.hHeader = ListView_GetHeader(hlist);
    ReattachHeaderSubclass();
}

// 根据列表实际宽度调整列宽：浏览模式弹性拉宽"名称"，搜索模式弹性拉宽"路径"
void AdjustListColumns(HWND hlist, int listW) {
    if (!hlist) return;
    auto s = [dpi = g_app.dpi](int v) { return MulDiv(v, dpi, 96); };
    if (g_app.listMode == LIST_MODE_SEARCH) {
        int fixed = s(220) + s(105) + s(150);  // 名称 + 大小 + 修改日期
        int pathW = listW - fixed;
        if (pathW < s(150)) pathW = s(150);
        ListView_SetColumnWidth(hlist, 1, pathW);
    } else {
        int fixed = s(105) + s(140) + s(150);  // 大小 + 类型 + 修改日期
        int nameW = listW - fixed;
        if (nameW < s(120)) nameW = s(120);
        ListView_SetColumnWidth(hlist, 0, nameW);
    }
}

void RefreshList(HWND hlist, size_t count) {
    ListView_SetItemCountEx(hlist, (int)count, LVSICF_NOSCROLL);
    InvalidateRect(hlist, nullptr, TRUE);
}

void SetStatusParts(HWND status, int cx) {
    if (!status || cx <= 0) return;
    int parts[4];
    parts[0] = cx * 38 / 100;
    parts[1] = cx * 72 / 100;
    parts[2] = cx * 88 / 100;
    parts[3] = -1;
    SendMessageW(status, SB_SETPARTS, 4, (LPARAM)parts);
    // SB_SETPARTS 会清空各格文本，布局后恢复恒定显示的 CPU 线程数
    SetStatusText(status, 2, g_app.statusThreadText);
}

void SetStatusText(HWND status, int part, const std::wstring& text) {
    if (!status) return;
    SendMessageW(status, SB_SETTEXT, part, (LPARAM)text.c_str());
}

std::wstring TypeString(const ListItem& item) {
    if (item.isDir) return L"文件夹";
    static std::unordered_map<std::wstring, std::wstring> cache;
    std::wstring key = util::GetExtensionLower(item.name);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    std::wstring probe = key.empty() ? L"file" : L"file" + key;
    SHFILEINFOW sfi{};
    SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                   SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES);
    std::wstring type = sfi.szTypeName;
    if (type.empty()) {
        if (key.empty()) {
            type = L"文件";
        } else {
            type = key.substr(1);
            CharUpperBuffW(&type[0], (DWORD)type.size());
            type += L" 文件";
        }
    }
    cache[key] = type;
    return type;
}

void IconCache::Attach(HWND hlist) {
    list_ = hlist;
    SHFILEINFOW sfi{};
    SHGetFileInfoW(L"", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                   SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    il_ = (HIMAGELIST)sfi.hIcon;
    if (il_) ListView_SetImageList(hlist, il_, LVSIL_SMALL);
    cache_.clear();
}

int IconCache::Get(const ListItem& item) {
    if (!il_) return -1;
    std::wstring key = item.isDir ? L"<dir>" : util::GetExtensionLower(item.name);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;

    SHFILEINFOW sfi{};
    std::wstring probe = item.isDir ? L"C:\\" : (key.empty() ? L"file" : L"file" + key);
    DWORD attr = item.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    SHGetFileInfoW(probe.c_str(), attr, &sfi, sizeof(sfi),
                   SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    int idx = sfi.iIcon;
    cache_[key] = idx;
    return idx;
}

}  // namespace ui
