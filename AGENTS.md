# 仓库指南（Repository Guidelines）

## 项目结构与模块组织

- `src/` — C++17 源码：
  - `main.cpp` — Win32 窗口过程、消息循环、导航、搜索界面、自定义绘制
  - `ui.cpp` — 控件创建、布局、深色模式、图标、列表列
  - `index.cpp` — 多线程全盘扫描、内存索引、二进制持久化
  - `search.cpp` — 并行索引搜索与实时扫描兜底
  - `worker.cpp` — 可复用任务池（任务内可再派发子任务）
  - `utils.cpp` — 路径/字符串工具、格式化、日志
  - `app.h` — 共享 `App` 状态与控件 ID
- `resources/` — `app.rc`、`app.manifest`、`app.ico`
- `tools/` — PowerShell 验证脚本与图标生成器
- `build/` — CMake 输出；可执行文件为 `build/work4-tools.exe`；运行时索引数据在 `build/index/`

## 构建、测试与开发命令

- 构建：`powershell -ExecutionPolicy Bypass -File build.ps1`
  - 配置 CMake（MinGW Makefiles、Release），用 g++ 编译，并刷新桌面快捷方式。
- 冒烟测试：`powershell -ExecutionPolicy Bypass -File tools\smoke-test.ps1`
  - 验证窗口创建、目录浏览、索引构建和搜索。
- 稳定性检查：`tools\resize-test.ps1`（缩放/最大化）、`tools\layout-check.ps1`（控件几何）。
- 功能检查：`tools\verify-dark-highlight.ps1`（深色模式与高亮）。
- 重新生成图标：`python tools\make-icon.py`。

## 编码风格与命名约定

- C++17，4 空格缩进，不使用 Tab。
- 使用带 `W` 后缀的显式 Win32 API（如 `CreateWindowExW`），界面文案使用中文。
- 文件内辅助函数放在匿名命名空间；界面状态集中在全局 `g_app`。
- 不引入第三方库，仅链接系统 DLL（`comctl32`、`shell32`、`dwmapi`、`uxtheme`）。
- MinGW 注意：部分 `commctrl.h` 宏缺失（如 `SB_SETTEXTCOLOR`、`HDM_SETBKCOLOR`），需在本地用 `WM_USER`/`HDM_FIRST` 数值补充定义。

## 测试指南

- 每个功能都应在 `tools/` 下配套 PowerShell 验证脚本，通过 P/Invoke 驱动界面。
- 禁止跨进程向状态栏发送 `SB_GETTEXT`——会触发本预览版 comctl32 的崩溃。
- 颜色类改动必须用像素采样验证（`GetDC`/`GetPixel`，或用 PIL 分析截图），不能只靠肉眼。
- 改动完成前必须运行 `smoke-test.ps1`。

## 提交与 Pull Request 约定

采用 Conventional Commits 风格，一个逻辑改动一个提交：

- `feat(ui): 新增深色模式切换按钮`
- `fix(index): 修复浅色模式目录树背景残留`
- `docs: 同步过时描述`

PR 需说明改动内容与原因，UI 改动附截图，并报告已运行的验证脚本。
提交身份为仓库级配置（`user.name=123qweyang`），不依赖全局 Git 配置。

## 安全与配置提示

- 索引文件包含完整文件路径，请保持 `build/index/` 仅本地使用。
- 深色模式偏好保存在注册表 `HKCU\Software\Work4Tools`。
- 无需管理员权限；扫描会跳过 `$Recycle.Bin` 与 `System Volume Information`。
