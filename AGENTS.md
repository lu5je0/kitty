# AGENTS.md

本仓库是 kitty 的个人 fork，基于上游 master（0.48.2 开发版）。包含一个自研特性和两处针对 macOS 26 的视觉修正。后续会定期合并上游主干，冲突时按下面的说明处理。

## 特性：macOS 原生标题栏 Tab 栏（macos_titlebar_tabs）

**是什么**：新增布尔选项 `macos_titlebar_tabs`。开启后隐藏 kitty 原本用终端网格绘制的 tab bar，改为在 macOS 原生标题栏（NSTitlebarView）中用 Cocoa 绘制 tab，效果类似 WezTerm 的 fancy tab bar：每个 tab 有标题和 × 关闭按钮、末尾有 + 新建按钮、带增删/切换/hover 动画、空白区域可拖动窗口、中键点击关闭 tab。

**为什么**：kitty 的 tab bar 是 GL 内容区里的文本网格，无法与 macOS 标题栏真正融合；希望获得原生观感（系统字体、红绿灯同排、动画）。

**配色**：tab 颜色复用 kitty 现有选项 `active_tab_foreground/background`、`inactive_tab_foreground/background`（含 per-tab 覆盖），在 kitty.conf 修改后重载配置即时生效。

### 数据流

- Python → Cocoa：`TabManager.mark_tab_bar_dirty()`（kitty/tabs.py）在每次 tab 创建/关闭/改标题/切 active 时调用 `update_native_titlebar_tabs()`，把 `(tab_id, title, is_active, needs_attention, fg, bg)` 元组经 `cocoa_set_titlebar_tabs()`（kitty/glfw.c）推给 `cocoa_update_titlebar_tabs()`（kitty/cocoa_window.m）。
- Cocoa → Python：点击事件走 `CocoaPendingAction` 机制，新增 `TITLEBAR_TAB_ACTIVATE/CLOSE/NEW` 三个动作（payload 为 "os_window_id tab_id"），在 kitty/child-monitor.c 转成 `call_boss`，由 kitty/boss.py 的 `titlebar_tab_activate/close/new` 处理。
- 网格 tab bar 隐藏：开启选项时 `TabManager.tab_bar_hidden = True`，复用 `tab_bar_style hidden` 的机制，C 层不预留空间，无需改 `os_window_regions`。

### 改动文件

| 文件 | 改动内容 |
|---|---|
| `kitty/options/definition.py` | 新增 `macos_titlebar_tabs` 选项（Python-only，无 ctype） |
| `kitty/options/parse.py`、`kitty/options/types.py` | 生成文件，由 `kitty +launch gen config` 重新生成，勿手改 |
| `kitty/cocoa_window.m` | 核心实现（"Titlebar tab bar" 折叠段，约 400 行）：`KittyTitlebarTabView`、`KittyTitlebarNewTabButton`、`KittyTitlebarTabBarView` 三个类 + `cocoa_update_titlebar_tabs()` 入口。含动画、hover 校准（防 tracking area 失效导致 hover 卡住）、`titlebarSeparatorStyle = None` 去除标题栏底部分隔线阴影 |
| `kitty/cocoa_window.h` | `TitlebarTabInfo` 结构、三个新 `CocoaPendingAction` 枚举、函数声明 |
| `kitty/glfw.c` | Python API `cocoa_set_titlebar_tabs`（仿 `cocoa_minimize_os_window` 的模式）+ 注册到 module_methods |
| `kitty/child-monitor.c` | `process_cocoa_pending_actions` 中三个新 action 的 `call_boss` 转发 |
| `kitty/boss.py` | `titlebar_tab_activate/close/new` 三个 handler |
| `kitty/tabs.py` | `use_native_titlebar_tabs` 属性、`update_native_titlebar_tabs()`、`tab_bar_hidden` 计算、`apply_options` 配置重载支持 |
| `kitty/fast_data_types.pyi` | `cocoa_set_titlebar_tabs` 类型声明 |
| `setup.py` | 链接 `-framework QuartzCore`（Core Animation 需要）；Info.plist 加 `UIDesignRequiresCompatibility`（见下） |

### 注意事项

- **必须经 kitty.app bundle 启动**（`open kitty/launcher/kitty.app` 或 Dock），直接跑裸二进制 `kitty/launcher/kitty` 时 Info.plist 不生效，窗口会是 macOS 26 的大圆角。
- ObjC 代码是 **MRR（非 ARC）**，注意手动 retain/release。
- 属性名不能以 `new` 开头（Cocoa 命名规则 + -Werror），所以是 `plusButton` 而不是 `newTabButton`。
- tab 垂直居中基于 `window.contentLayoutRect` 计算可见标题栏区间，不要改成按红绿灯按钮对齐（高标题栏下红绿灯不在垂直中点）。

## macOS 26 视觉修正（与上游有意分歧，合并主干时注意）

1. **回退上游 commit `b16221a1d`**（"macos: explicitly enable modern window corners on macOS 26"）：删除了 `glfw/cocoa_window.m` 中的 `apply_window_corner_curve()` 及其调用。合并主干如果冲突，**保持删除**。该提交在 glfw/glfw.py 中加的 QuartzCore 链接保留不动。
2. **`setup.py` Info.plist 增加 `UIDesignRequiresCompatibility=True`**：让 macOS 26 (Tahoe) 使用传统 UI 设计（小窗口圆角，非 Liquid Glass）。这是保留大圆角修正的关键，合并时保留。

## 合并主干流程

1. `git merge upstream/master`
2. 冲突处理：
   - `kitty/options/parse.py`、`types.py` 等生成文件：任选一边解决后，用 `./kitty/launcher/kitty +launch gen config` 重新生成即可
   - `glfw/cocoa_window.m`：保持 `apply_window_corner_curve` 删除状态
   - 其余文件冲突按上表理解语义手动合
3. `./dev.sh build` 重新构建（需 Go 工具链；国内网络建议挂代理 `export https_proxy=...`）
4. 验证：`open kitty/launcher/kitty.app`，检查 tab 点击/关闭/新建、窗口圆角、标题栏无分隔线阴影

## 用户配置（~/.dotfiles/kitty/）

- `macos.conf`：`macos_titlebar_tabs yes`
- `kitty.conf`：`macos_titlebar_color dark` + 中性灰 tab 配色（`active_tab_background #626366` 等）
