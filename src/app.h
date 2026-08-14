#pragma once

#include <windows.h>
#include <cstddef>
#include <string>
#include <vector>

#include "index.h"
#include "search.h"
#include "ui.h"

// 控件/命令 ID
enum {
    IDC_BACK = 1001,
    IDC_FWD,
    IDC_UP,
    IDC_REFRESH,
    IDC_REBUILD,
    IDC_HIDDEN,
    IDC_ADDR_LABEL = 1007,
    IDC_SEARCH_LABEL,
    IDC_DARK = 1009,
    IDC_ADDR = 1010,
    IDC_GOTO,
    IDC_SEARCH,
    IDC_SEARCHBTN,
    IDC_CLEAR,
    IDC_TREE = 1020,
    IDC_LIST,
    IDC_STATUS,
    IDM_EXIT = 2001,
    IDM_REFRESH,
    IDM_HIDDEN,
    IDM_REBUILD,
    IDM_ABOUT,
    IDM_INDEXDIR
};

enum {
    IDT_PROGRESS = 1,
    IDT_DEBOUNCE = 2
};

enum {
    WM_APP_INDEX_LOADED = WM_APP + 1,
    WM_APP_INDEX_DONE = WM_APP + 2,
    WM_APP_SEARCH_DONE = WM_APP + 3
};

enum {
    LIST_MODE_BROWSE = 0,
    LIST_MODE_SEARCH = 1
};

struct App {
    HINSTANCE hInst = nullptr;
    HWND hwnd = nullptr;
    HWND hTree = nullptr;
    HWND hList = nullptr;
    HWND hHeader = nullptr;
    HWND hAddr = nullptr;
    HWND hSearch = nullptr;
    HWND hStatus = nullptr;
    HWND hBtnBack = nullptr;
    HWND hBtnFwd = nullptr;
    HWND hBtnUp = nullptr;
    HWND hBtnRefresh = nullptr;
    HWND hBtnRebuild = nullptr;
    HWND hBtnDark = nullptr;
    HWND hChkHidden = nullptr;
    HWND hBtnGoto = nullptr;
    HWND hBtnSearch = nullptr;
    HWND hBtnClear = nullptr;
    HFONT hFont = nullptr;
    HFONT hFontBold = nullptr;
    HBRUSH hBrushDark = nullptr;
    HBRUSH hBrushSplit = nullptr;
    WNDPROC origAddrProc = nullptr;
    WNDPROC origSearchProc = nullptr;
    WNDPROC origListProc = nullptr;
    WNDPROC origHeaderProc = nullptr;

    int dpi = 96;
    IndexManager index;
    ui::IconCache icons;

    std::vector<std::wstring> history;
    size_t histPos = (size_t)-1;
    std::wstring currentDir;

    std::vector<ListItem> listItems;
    bool searchMode = false;
    std::wstring lastQuery;
    int searchGen = 0;
    bool showHidden = false;
    bool darkMode = false;
    bool suppressEdit = false;
    int sortCol = 0;
    bool sortAsc = true;
    int listMode = LIST_MODE_BROWSE;
    RECT splitRect{};
    std::wstring statusThreadText;  // CPU 线程数文本，运行期间恒定，只初始化一次
    wchar_t dispBuf[2048];
};

extern App g_app;
extern HWND g_hwnd;
