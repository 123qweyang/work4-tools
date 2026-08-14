#pragma once

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <unordered_map>

#include "search.h"

namespace ui {

int GetDpi(HWND hwnd);
void EnableDpiAwareness();
HFONT MakeFont(int dpi);
HFONT MakeBoldFont(int dpi);
void CreateControls(HWND parent);
void Layout(HWND hwnd);
void ApplyFont(HWND parent);
void SetDarkMode(bool dark);
void SetListViewMode(HWND hlist, int mode);
void ReattachHeaderSubclass();  // 列头子类挂接（Header 可能被 ListView 重建，需重挂）
void AdjustListColumns(HWND hlist, int listW);
void RefreshList(HWND hlist, size_t count);
void SetStatusParts(HWND status, int cx);
void SetStatusText(HWND status, int part, const std::wstring& text);
std::wstring TypeString(const ListItem& item);

class IconCache {
public:
    void Attach(HWND hlist);
    int Get(const ListItem& item);
    HIMAGELIST ImageList() const { return il_; }

private:
    HWND list_ = nullptr;
    HIMAGELIST il_ = nullptr;
    std::unordered_map<std::wstring, int> cache_;
};

}  // namespace ui
