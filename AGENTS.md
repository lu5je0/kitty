# AGENTS.md

本仓库是 kitty 的个人 fork，基于上游 master。包含两个自研特性和两处针对 macOS 26 的视觉修正。后续会定期合并上游主干，冲突时按下面的说明处理。

## 改动原则（所有改动都适用，优先级最高）

这个 fork 会**长期反复合并上游 master**，所以非必要的改动一律要写成**最容易合并**的形式。改代码前先想一遍：这段能不能不碰上游的行？

- **加，不要改**。新逻辑放进新函数/新文件，用包一层（wrapper）的方式接进去，让上游原函数一字不动。例：`kitty/glfw.c` 里 `cocoa_text_input_filter()` 包住上游的 `filter_option()`，而不是往 `filter_option()` 里塞 if。
- **只在文件末尾追加**。新的宏、常量、函数尽量放文件尾部，别插在上游的列表中间。例：`kitty/modes.h` 的 `DISABLE_IME` 追加在末尾。
- **不要挤进上游的声明列表**。结构体新字段单独起一行，别加在上游那种一行好几个成员的列表里（那种行上游几乎每次都会动）。例：`kitty/screen.h` 的 `bool mDISABLE_IME;`。
- **优先纯声明式**，避免加状态转换钩子。读的时候实时读 flag，正确性就不依赖上游的各种 reset/save/restore 路径，从而不用在那些函数里插代码。
- **删除行比新增行贵得多**。新增行 git 基本能自动合，改/删上游的行才是冲突源头。尽量把 diff 压成"纯新增"。
- **自研编号躲开上游号段**。私有模式号、OSC 号之类别紧挨着上游当前用的值，上游是顺序往上加的。
- **改完在 agents.md 记一笔**：动了哪些上游的行、为什么、合并冲突时该怎么取舍。

## 特性：原生标题栏 Tab 栏（native_titlebar_tabs，原 macos_titlebar_tabs）

**是什么**：新增布尔选项 `native_titlebar_tabs`（旧名 `macos_titlebar_tabs` 仍可用，走 deprecation alias）。开启后隐藏 kitty 原本用终端网格绘制的 tab bar，改为在原生标题栏中绘制 tab：macOS 用 Cocoa（NSTitlebarView），Wayland 用 CSD titlebar buffer（软件渲染）。效果类似 WezTerm 的 fancy tab bar：每个 tab 有标题和 × 关闭按钮、末尾有 + 新建按钮、带增删/切换/hover 动画（Wayland 动画待做）、空白区域可拖动窗口、中键点击关闭 tab。

**为什么**：kitty 的 tab bar 是 GL 内容区里的文本网格，无法与系统标题栏真正融合；希望获得原生观感（系统字体、窗口按钮同排、动画）。

**配色**：tab 颜色复用 kitty 现有选项 `active_tab_foreground/background`、`inactive_tab_foreground/background`（含 per-tab 覆盖），在 kitty.conf 修改后重载配置即时生效。

### 数据流

- Python → 原生层：`TabManager.mark_tab_bar_dirty()`（kitty/tabs.py）在每次 tab 创建/关闭/改标题/切 active 时调用 `update_native_titlebar_tabs()`，把 `(tab_id, title, is_active, needs_attention, fg, bg)` 元组经 `set_titlebar_tabs()`（kitty/glfw.c，原 `cocoa_set_titlebar_tabs`）推给：
  - macOS：`cocoa_update_titlebar_tabs()`（kitty/cocoa_window.m）
  - Wayland：`glfwWaylandSetTitlebarTabs()`（glfw/wl_titlebar_tabs.c，经 glfw-wrapper 动态加载，符号缺失时静默跳过）
- Cocoa → Python：点击事件走 `CocoaPendingAction` 机制，`TITLEBAR_TAB_ACTIVATE/CLOSE/NEW/DROP/DETACH` 五个动作（payload 为 "os_window_id tab_id" 等），在 kitty/child-monitor.c 转成 `call_boss`，由 kitty/boss.py 的 `titlebar_tab_*` 处理。
- Wayland → Python：glfw 侧命中测试后调 `_glfw.callbacks.titlebar_tab_action`（kitty/glfw.c 的 `titlebar_tab_action_callback` 注册），直接 `call_boss` 同一批 `titlebar_tab_*` handler，和 macOS 共用行为。
- Wayland 的 tab 标题文字：glfw 不做字体渲染，回调 `_glfw.callbacks.titlebar_tab_text`（kitty/glfw.c 的 `titlebar_tab_text_callback`，FreeType 12*scale px，居中逻辑在 kitty 侧算好 x_offset）。
- 网格 tab bar 隐藏：开启选项时 `TabManager.tab_bar_hidden = True`，复用 `tab_bar_style hidden` 的机制，C 层不预留空间，无需改 `os_window_regions`。

### 改动文件

| 文件 | 改动内容 |
|---|---|
| `kitty/options/definition.py` | 新增 `native_titlebar_tabs` 选项（Python-only，无 ctype）+ `macos_titlebar_tabs` deprecation alias |
| `kitty/options/utils.py` | 文件末尾追加 `deprecated_macos_titlebar_tabs_alias()` |
| `kitty/options/parse.py`、`kitty/options/types.py` | 生成文件，由 `kitty +launch gen config` 重新生成，勿手改 |
| `kitty/cocoa_window.m` | macOS 核心实现（"Titlebar tab bar" 折叠段，约 400 行）：`KittyTitlebarTabView`、`KittyTitlebarNewTabButton`、`KittyTitlebarTabBarView` 三个类 + `cocoa_update_titlebar_tabs()` 入口。含动画、hover 校准（防 tracking area 失效导致 hover 卡住）、`titlebarSeparatorStyle = None` 去除标题栏底部分隔线阴影 |
| `kitty/cocoa_window.h` | `TitlebarTabInfo` 结构、五个新 `CocoaPendingAction` 枚举、函数声明 |
| `kitty/glfw.c` | Python API `set_titlebar_tabs`（`NativeTabInfo` 宏共享 macOS/Wayland 解析循环）+ 注册到 module_methods；`titlebar_tab_text_callback` / `titlebar_tab_action_callback`（非 Apple 分支，在 `glfw_init` 里和 `glfwSetDrawTextFunction` 一起注册）；Wayland 分支透传 `forced_appearance`（`macos_titlebar_color` light/dark）并置 `w->wayland_titlebar_tabs_active`（底部圆角开关） |
| `kitty/child-monitor.c` | `process_cocoa_pending_actions` 中五个新 action 的 `call_boss` 转发 |
| `kitty/boss.py` | `titlebar_tab_activate/close/new/drop/detach/drag_out` handler；drop/detach 开头 `set_tab_being_dragged()` 取消挂起的 DND handoff（松键快于异步缩略图回调时，implicit grab 已失效，晚到的 start_drag 会 EPERM） |
| `kitty/tabs.py` | `native_titlebar_tabs_supported()`（`is_macos or is_wayland()`）、`use_native_titlebar_tabs` 属性、`update_native_titlebar_tabs()`（带 `last_native_titlebar_tabs_data` 去重缓存：一次切 tab 会触发 2–3 次 `mark_tab_bar_dirty`，相同数据不重推原生层；`apply_options` 里清缓存强制重推，因为 bar 颜色/appearance 是 C 侧推送时才读 options）、`tab_bar_hidden` 计算、`apply_options` 配置重载支持；**改了上游 `on_tab_drop_move` 3 行**：`window_geometry` 访问前加 `laid_out_once` 判断（native tabs 下网格 tab bar 从不 layout，否则跨窗口拖拽 AttributeError），合并冲突时保留该 guard |
| `kitty/fast_data_types.pyi` | `set_titlebar_tabs` 类型声明 |
| `setup.py` | 链接 `-framework QuartzCore`（Core Animation 需要）；Info.plist 加 `UIDesignRequiresCompatibility`（见下） |
| `glfw/wl_titlebar_tabs.{c,h}` | **Wayland 核心实现，纯 fork 专属文件，零冲突**：按 `window->id` 的模块内状态链表、macOS 同款布局算法、4×4 子采样抗锯齿绘制、命中测试与点击语义、`glfwWaylandSetTitlebarTabs` 导出（按 tab_id diff 保动画状态；强制 CSD、`visible_titlebar_height` 提到 33、立即重设 opaque region）。动画引擎：5 种 0.18s ease-out（位置/宽度、淡入/淡出、hover、变 active 配色）、文字 alpha 蒙版缓存、16ms 单例 timer + 动画期间 `wl_subsurface_set_desync`；非自定义色时用实测 macOS 标题栏色（深色聚焦 #393A39），`forced_appearance` 尊重 `macos_titlebar_color light/dark` |
| `glfw/wl_client_side_decorations.c` | 仅加钩子：include 1 行、`render_title_bar()` 5 行、`update_hovered_button()` 1 行、`handle_pointer_button()` 1 行、`handle_pointer_leave()` 1 行、`csd_free_all_resources()` 1 行、`buffer_release_event()` 2 行（tabs 激活时不销毁被 release 的 buffer——上游 release 即销毁并置 `buffer_destroyed`，配合 16ms 动画会导致"每帧全量重建全部 CSD buffer"：销毁正在显示的 buffer 造成点击 tab 闪烁 + 明显卡顿。保留逻辑在 `wl_titlebar_tabs_retain_released_buffer()`，它会把 `*_needs_to_be_destroyed` 收回为 true 以免泄漏） |
| `glfw/wl_window.c` | `update_regions()` 里 4 行：tabs 激活时 opaque region 挖掉底部两个 10×10 逻辑 px 角（配合 GL 圆角）；include 1 行 |
| `kitty/shaders.c` | 枚举加 `CORNER_MASK_PROGRAM`、`C()` 导出 1 行、新 static `draw_bottom_corner_masks()`（帧末把底部两角像素乘以圆覆盖率，`GL_ZERO/GL_SRC_ALPHA`；同函数里还画内容区的 1px 浅色内描边：左右/底边 + 底角弧）+ `stop_os_window_rendering()` 末尾 1 行调用 |
| `kitty/corner_mask_fragment.glsl` | **新文件，纯 fork 专属**：circle SDF coverage + `border_color` 描边模式，复用 `rounded_rect_vertex.glsl` |
| `kitty/shaders.py` | import + 1 行编译 `corner_mask` program |
| `kitty/state.h` | `OSWindow` 尾部单独一行 `bool wayland_titlebar_tabs_active;` |
| `glfw/glfw3.h` | preamble 加 `GLFWTitlebarTab`/`GLFWTitlebarTabAction`/两个回调 typedef（`} GLFWgamepadstate;` 之后）；GLFWAPI 声明区末尾加 `glfwSetTitlebarTab{Action,Text}Callback` |
| `glfw/internal.h` | `_glfw.callbacks` 尾部加 `titlebar_tab_action` / `titlebar_tab_text` 两行 |
| `glfw/input.c` | 文件末尾追加两个注册函数实现 |
| `glfw/glfw.py` | 硬编码清单加 `glfwWaylandSetTitlebarTabs` |
| `glfw/source-info.json` | wayland sources/headers 加新文件（保持 2 空格缩进） |
| `kitty/glfw-wrapper.{h,c}` | 生成文件，`cd glfw && python3 glfw.py` 重新生成，勿手改 |

### 注意事项（macOS）

- **必须经 kitty.app bundle 启动**（`open kitty/launcher/kitty.app` 或 Dock），直接跑裸二进制 `kitty/launcher/kitty` 时 Info.plist 不生效，窗口会是 macOS 26 的大圆角。
- ObjC 代码是 **MRR（非 ARC）**，注意手动 retain/release。
- 属性名不能以 `new` 开头（Cocoa 命名规则 + -Werror），所以是 `plusButton` 而不是 `newTabButton`。
- tab 垂直居中基于 `window.contentLayoutRect` 计算可见标题栏区间，不要改成按红绿灯按钮对齐（高标题栏下红绿灯不在垂直中点）。

### 注意事项（Wayland）

- 布局是 macOS 的**镜像**：窗口按钮在右侧（kitty 手绘），tab 从 x=0 开始，右侧给按钮留 `num_buttons*button_size + 8*scale`。
- **PT_PARITY = 7/6**：macOS 按 72dpi 排文字、Linux 实际 ~96dpi，同样的 font_size 下 Linux 终端格子占更多逻辑 px，macOS 常量原样用会显得 tab 栏偏小（实测同格子大小下 mac tab 48 物理 px vs 我们 41）。所以 wl_titlebar_tabs.c 的全部 tab 度量常量 = macOS 值 × 7/6（tab 高 24→28、字号 12→14 等），窗口圆角/按钮/阴影不乘。
- 标题栏高度：正常 CSD 是 24 逻辑 px；有 tab 时首次 `glfwWaylandSetTitlebarTabs` 会把**该窗口**的 `decs.metrics` 提到 33（macOS 实测 28：1px 浅描边叠在 bar 顶 + 1px bar + 24px tab + 2px bar，×7/6 取整 33）。并把 `decs.for_window_state.width` 清零强制重建 shm 缓冲。没改 `csd_initialize_metrics()` 本身。
- **1px 浅色窗口内描边**（macos.png 实测：顶边 white@0.30、侧/底 white@0.20）：标题栏部分（顶边+顶角弧+侧列）由 `wl_titlebar_tabs.c` 的 `draw_titlebar_border()` 画进 CSD buffer（在 `round_top_corners` 之前）；内容区左右/底边+底角弧由 `kitty/shaders.c` 的 `draw_bottom_corner_masks()` 里的描边 pass 画（`corner_mask_fragment.glsl` 新增 `border_color` uniform：a==0 走原裁切模式，a>0 走描边/实心模式，`GL_ONE/GL_ONE_MINUS_SRC_ALPHA`）。
- 左侧不画窗口 icon（曾画过，已移除，`_glfwPlatformSetWindowIcon` 的钩子行也删了）；tab 从 `TAB_BAR_LEFT_MARGIN`（8×7/6 逻辑 px，左侧没有红绿灯按钮所以比 macOS 的 16 小）开始。
- mutter 默认 SSD：`glfwWaylandSetTitlebarTabs` 里检测 `decs.serverSide` 时强制切 CLIENT_SIDE（照抄 `setXdgDecorations` 的 titlebar_hidden 分支）。
- **mutter 焦点重绘时序 bug**（现象：非拖动时阴影是未聚焦的半透明、tab 是灰色未聚焦配色）：CSD 的焦点相关绘制读 `_glfw.focusedWindowId`（键盘焦点），但上游重绘只由 xdg configure(activated) 触发；mutter 先发 configure 再发 `wl_keyboard.enter`，重绘时键盘焦点还没更新，之后也没人再触发。修法：`glfw/wl_init.c` 的 `keyboardHandleEnter/Leave` 里 `_glfwInputWindowFocus` 之后各插 1 行 `csd_change_title(window)`（内部按 focus_changed 去重；sync 的阴影 subsurface 靠焦点变化本身引发的下一次父 surface 提交生效）。合并冲突时这两行要保留。
- 所有 tab 几何都存 **scaled px**（`round(fscale*x)` 命中），分数缩放下勿混逻辑坐标。
- 文字/动作回调经 `_glfw.callbacks`（glfw 是独立 .so，不能直接调 kitty 函数）。
- 合并主干时 `glfw/wl_client_side_decorations.c` 的 7 处钩子要保留（6 处单行 + `buffer_release_event()` 开头 2 行的 buffer 保留早退）；`glfw/glfw3.h` 的 typedef 块和末尾两个 GLFWAPI 声明要保留。
- 动画已实现（同 macOS：0.18s ease-out ×5 种；见 wl_titlebar_tabs.c 的 Anim 引擎）。跨窗口拖拽（Stage 4，上游 mime 方案）尚未实现，见 todo.md。

## 特性：按 window 彻底禁用输入法

**是什么**：让终端里的程序按 kitty window（pane 粒度，不是 OS window）**彻底旁路输入法**——IME 完全收不到按键，无法组词、无候选窗、无 preedit。不是切换到英文输入源，输入法指示图标不变。macOS 和 Wayland 都生效（机制不同，见下），X11 不做。

**唯一通道：tui-bridge OSC 1337**

```
OSC 1337 ; SetUserVar=tui-bridge=<base64 of {"id":1,"module":"ime","method":"normal"|"insert","params":{}}> BEL
```

`method=normal` → 禁用 IME，`method=insert` → 启用。

选这个协议的唯一理由是**让已有配置零改动直接可用**：`~/.dotfiles/zsh/vi-im-switch.zsh` 和 nvim 的 `lu5je0.misc.ime.osc.backend` 本来就在发这个消息（原先是发给 tui-bridge daemon 的），现在 kitty 自己就认，两边 base64 字节完全一致。

早期版本还有一条 DEC 私有模式通道（59998，含 DECRQM 查询），**已移除**——实际没人查询，而且去掉它让 `kitty/modes.h` 和 `kitty/screen.c` 的模式相关代码完全回到上游原样。要观测状态用 Python 侧的只读属性 `screen.ime_disabled`。

**为什么用转义序列而不是 `kitty @`**：零进程开销、能穿透 SSH（远端 vim 直接写 stdout 即可，不需要转发 control socket，也不需要远端装 im-select 之类的本地工具）。

### 机制（关键，改动前必读）

状态挂在 `ScreenModes.mDISABLE_IME` 上。选这个结构体是因为它本来就是 per kitty window 的，而且 `do_screen_reset` 里的 `self->modes = empty_modes` 会顺手清掉它——RIS / `reset` 自动生效，不用加代码。

**不能直接复用现成的 `glfwSetIgnoreOSKeyboardProcessing()` 或 text input filter 返回 `1`**。在 `glfw/cocoa_window.m` 的 `keyDown:` 里，`process_text` 是唯一的 IME 闸门，但按键文本**只**在 `interpretKeyEvents:` → `insertText:` 回调里被写进 `_glfw.ns.text`；`UCKeyTranslate` 的输出写在局部 `text[256]` 里，仅用于判断 dead key 状态，从不写进 `_glfw.ns.text`。所以 `process_text == false` 的语义是"既旁路 IME，也不产生任何文本"——对 kitty keyboard modes（只按 keycode 匹配）没问题，但对本特性意味着连 ASCII 都打不出来。

因此扩展了 Cocoa text input filter 的返回值约定，新增 `2` = 旁路 IME 但仍从键盘布局生成文本（自己把 `UCKeyTranslate` 的 UTF-16 输出转 UTF-8 填进 `_glfw.ns.text`）。这样**不需要新增 glfw 导出函数**，`kitty/glfw-wrapper.{h,c}` 不用重新生成。

**两条踩过的坑，改这段前必看**：

1. **`UCKeyTranslate` 会忽略 Command / Control**，对 `cmd+i` 照样返回 `i`。而 macOS 自己是对的——`interpretKeyEvents:` 把这类 key equivalent 路由到 `doCommandBySelector: (noop:)`，不产生文本，kitty 才能发出 encoded key（`CSI 105;9u`）。所以生成文本时必须排除 `GLFW_MOD_SUPER | GLFW_MOD_CONTROL`，否则 `cmd+i` 会变成字面 `i` 发给子进程，vim 的 `<D-i>` 就没了。（Ctrl 组合当初没暴露，是因为它产出的控制字符被 `is_ascii_control_char()` 清掉了。）
2. **option-as-alt 必须优先于 IME 旁路判断**。option-as-alt 需要返回 `1`（完全不产生文本）；若先命中 IME 分支返回 `2`，会从键盘布局生成文本，US 布局下 `option+i` 就成了死键字符 `ˆ`。

**OSC 通道全程在 C 层，不碰 Python**。上游的 OSC 1337 是丢给 Python 的 `Window.osc_1337()` 处理 user vars 的（`kitty/window.py:1349`），但 IME 闸门在 `keyDown:` 里每次按键都要读，走 Python 太重。所以在 `vt-parser.c` 的 `case 1337` 前面拦一手，识别出 IME 消息就在 C 里 base64 解码 + 匹配 method，然后直接置 flag、`break` 掉不再往下走；认不出来的 payload 原样落到上游路径（所以其它 user var 不受影响，有回归测试守着）。

**纯声明式**：ObjC 侧每次按键实时读这个 flag，正确性不依赖任何状态转换钩子。`fork_ime_set_disabled()` 里的 `update_ime_position_for_window()` 调用纯属体验优化——只为丢弃"切换瞬间正在组的候选词"。

### 机制（Wayland）

macOS 那套"每键实时读 flag"在 Wayland 行不通：IME 走 zwp_text_input_v3 协议，**per-seat 全局对象、有状态**，组词时 compositor 根本不给客户端发 `wl_keyboard.key`，客户端唯一手段是 `zwp_text_input_v3_disable`。因此：

- glfw 侧（`glfw/wl_text_input.c`）加一个模块级 `fork_ime_inhibited` 标志 + 导出 `glfwWaylandSetIMEInhibited(bool)`（走 glfw.py 硬编码清单 + wrapper 动态加载，非 Wayland 后端符号为 NULL 静默跳过）。上游有两处会 re-enable，都已拦截：`text_input_enter`（compositor enter 时无条件 enable，插了一行早退）和 `_glfwPlatformUpdateIMEState` 的 `GLFW_IME_UPDATE_FOCUS` case（顶部插一行走 `fork_ime_force_disable()`，该 helper 是新增函数，镜像上游 else 分支的清理逻辑）。切换标志时若 `ime_focused` 会立即 disable / 恢复 enable。
- kitty 侧的同步点：`keys.c` 末尾新增 `fork_ime_sync_wayland_inhibit(OSWindow*)`（inhibit = 聚焦 OS window 的 active kitty window 的 `mDISABLE_IME`；非聚焦 OS window 不许推送，因为 text input 跟随键盘焦点）。它在两个 FOCUS 事件发送点**之前**被调：`update_ime_focus()` 开头 1 行、`kitty/glfw.c` `window_focus_callback` 里 1 行。OSC / Python setter / 窗口焦点切换全部汇聚到 `update_ime_focus`，所以 `fork-ime.h` 零改动即可生效。
- RIS 兜底：`do_screen_reset` 清 modes 前插了 1 行 `fork_ime_set_disabled(self, false)`——macOS 靠 `modes = empty_modes` 就够，但 Wayland 的 inhibit 状态在 glfw 里，必须走通知路径推一次（函数自带去重，flag 没置时是 no-op）。
- 候选框锚定：`prepare_ime_position_update_event`（keys.c）末尾插了几行 fork-local 覆盖——Wayland 组词时上报 `overlay_line.xstart`（preedit 第一个格子）而不是上游的 `cursor_x`（preedit 内部光标），否则每打一个字候选框跳一格；同时 `top` 下移 1/4 个 cell，避免候选框贴住 preedit 行。注意这个 1/4 cell 下移在 Wayland 下**无条件**生效（不只 overlay 激活时），否则组词前每帧上报的矩形和组词中的差 1/4 cell，候选框出现后会往下挪一下。
- 候选框首现位置（GNOME 上"先出现在上次打字的位置再跳过来"的 bug）：上游只在按键到达（子进程回显**之前**）或组词已开始后才推 `set_cursor_rectangle`，commit 后子进程异步回显、光标前移时**没有任何路径**推送新矩形，所以下次组词 compositor 手里还是旧矩形。修法：`keys.c` 末尾新增 `fork_ime_report_render_cursor()`，在 `child-monitor.c` `prepare_to_render_os_window()` 的 active window 分支插 1 行，每帧对聚焦窗口上报光标矩形；glfw 侧 `wl_text_input.c` 本来就按矩形去重（不变不 commit），零额外状态，也不会触发 GNOME done 事件循环（#5105）。

### 改动文件

| 文件 | 改动内容 |
|---|---|
| `kitty/fork-ime.h` | **新文件，纯 fork 专属**，零冲突。承载全部逻辑：base64 解码、极简 JSON 字段匹配、`fork_ime_set_disabled()`、以及 `ime_disabled` 属性的 getter/setter |
| `kitty/vt-parser.c` | 加 2 行、删 0 行：include `fork-ime.h`，以及 `case 1337:` 开头一行 `if (code == 1337 && fork_ime_handle_osc1337(...)) break;`（必须在 `START_DISPATCH` 之前） |
| `kitty/screen.h` | `ScreenModes` 里**单独一行** `bool mDISABLE_IME;`，不塞进上游那串 bool 列表，避免上游加模式时冲突 |
| `kitty/screen.c` | 加 2 行、删 0 行：include `fork-ime.h`，以及 getsetters 数组里的 `{"ime_disabled", ...}`（手写一行，`GETSET` 宏用不了因为它假定字段名和属性名一致） |
| `glfw/cocoa_window.m` | `keyDown:`/`flagsChanged:` 支持 filter 返回值 `2`；`keyDown:` 的 `else` 分支在 `ime_disabled` 时把 `UCKeyTranslate` 结果填进 `_glfw.ns.text`（排除 super/ctrl，见上面坑 1） |
| `glfw/cocoa_platform.h` | `GLFWcocoatextinputfilterfun` typedef 上方补返回值约定注释 |
| `kitty/glfw.c` | 上游的 `filter_option` **一字未改**，新增 `ime_disabled_for_focused_window()` + `cocoa_text_input_filter()` 包一层。唯一改的上游代码是安装那一行，改为**无条件安装** filter（原来只在 `macos_option_as_alt` 开启时装） |
| `kitty/window.py` | `Window` 类**末尾追加** `enable_ime` action（见下） |
| `kitty/fast_data_types.pyi` | `Screen.ime_disabled: bool` 类型声明，追加在属性列表末尾 |
| `kitty_tests/ime_mode.py` | 单元测试：两个 method、RIS 清除、**其它 user var 不被吞**、畸形 payload 被忽略、属性可写 |
| `kitty_tests/ime_e2e.py` | 端到端：真起一个 nvim（用真实 `~/.dotfiles` 配置）在 pty 里跑，验证 startup / InsertEnter / InsertLeave / CmdlineEnter / CmdlineLeave 五个状态。没有 nvim 或 dotfiles 时自动 skip |
| `glfw/wl_text_input.c` | Wayland 生效路径（**全部纯新增，零删改**）：static `fork_ime_inhibited`、`text_input_enter` 早退 1 行、FOCUS case 顶部拦截 1 行、新 helper `fork_ime_force_disable()`、文件末尾导出 `glfwWaylandSetIMEInhibited()`。冲突时全部保留 |
| `glfw/glfw.py` | 硬编码清单加 `glfwWaylandSetIMEInhibited` 1 行 |
| `kitty/keys.c` | `update_ime_focus()` 开头插 1 行 sync 调用；文件末尾追加 `fork_ime_sync_wayland_inhibit()` 和 `fork_ime_report_render_cursor()` 实现；`prepare_ime_position_update_event()` 末尾插 3 行（Wayland 候选框锚定 preedit 起点） |
| `kitty/state.h` | 文件末尾追加 `fork_ime_sync_wayland_inhibit`、`fork_ime_report_render_cursor` 声明 2 行 |
| `kitty/child-monitor.c`（Wayland 部分） | `prepare_to_render_os_window()` active window 分支插 1 行 `fork_ime_report_render_cursor()`（每帧上报光标矩形，修 GNOME 候选框首现在旧位置的 bug） |
| `kitty/glfw.c`（Wayland 部分） | `window_focus_callback` 里构造 FOCUS 事件前插 1 行 sync 调用 |
| `kitty/screen.c`（Wayland 部分） | `do_screen_reset` 清 modes 前插 1 行 `fork_ime_set_disabled(self, false)` |
| `kitty/glfw-wrapper.{h,c}` | 生成文件，`cd glfw && python3 glfw.py` 重新生成，勿手改 |

整体 merge footprint（macOS 部分）：**52 行新增、3 行删除**（`kitty/glfw.c` 的安装行 + `glfw/cocoa_window.m` 的两行 `process_text` 计算）。Wayland 部分全部是纯新增行（约 45 行）。`kitty/modes.h` 完全没动。

跑测试：`./test.py --module ime_mode` 和 `--module ime_e2e`。

### 逃生阀：`enable_ime` action

`Window.enable_ime` 只做一件事：`self.screen.ime_disabled = False`。用来救「rate limiter 把最后一次 enable 丢了」这类卡在禁用状态的情况。kitty.conf：

```
map ctrl+b>i enable_ime
```

- **一次性**，不是粘性覆盖。终端里的程序下一次发 `method=normal` 照样会再禁用（nvim 的 `ModeChanged` 很快就会来一次）。之所以不做粘性 override，是为了不在 `ScreenModes` 加第二个 bool，也不引入用户看不见的隐藏状态。
- action 不需要在任何地方注册：`parse_key_action` 对无参 action 直接透传函数名，`Boss.dispatch_action` 会在 Boss / Tab / Window 上依次 `getattr`。
- 因此 `screen.ime_disabled` 从只读变成可读写；setter 走 `fork_ime_set_disabled()`，和 OSC 路径同一个入口。

### 已知限制

- **macOS 和 Wayland 生效，X11 不做**（flag 能置但无效果；X11/ibus 走 `glfw/ibus_glfw.c`，需另做）。Wayland 上若设了 `GLFW_IM_MODULE=ibus`（强制走 ibus 而非 text-input-v3），同样不覆盖。
- **Wayland 的 inhibit 是 per-seat 全局状态**，靠焦点事件链同步到"聚焦 OS window 的 active kitty window"。跨 OS window 切换时，同一事件批内理论上有一瞬间用的是旧值（compositor 的 text_input enter 和 keyboard enter 处理顺序不保证），实际无感。
- `process_text == false` 时已带 `kUCKeyTranslateNoDeadKeysMask`，所以 **dead key / Option 组合键也一并禁用**——符合"彻底禁止"的预期。
- **tmux/screen 会吞掉这个 OSC**，需开 `allow-passthrough`。也正因如此 nvim 侧用 `TERM == 'xterm-kitty'` 判断——tmux 里 TERM 被改写，会自动退回其它 backend。
- **nvim 内置 terminal 里的程序管不了外层**。nvim 的终端模拟器不会把这个 OSC 转发给外层 kitty，所以在 `:terminal` 里跑的 zsh，它的 vi-mode 钩子对外层无效——那里的 IME 完全由外层 nvim 的 `TermEnter`/`TermLeave` 决定（即 terminal 模式下始终启用）。
- **退出时必须恢复**，不能只切 ASCII。kitty 是旁路 IME 而不是切输入源，所以留着禁用状态出去，用户切输入源也救不回来。`init.lua` 的 `VimLeavePre`/`VimSuspend` 走 `backend.on_exit()`：osc backend 先发 `insert` 解除旁路，再让 **tui-bridge**（本地 JSON-RPC 子进程，和 OSC 通道无关）把输入源切成 ASCII——切输入源这件事 kitty 做不到，且 kitty 会吞掉 ime 的 OSC，那条消息根本到不了 tui-bridge daemon。ssh 时跳过切换（helper 二进制在本机）。这条退出路径**故意绕过 rate limiter**：被丢掉就没有后续事件能补救了。zsh 的 `zle-line-init` 也会在下一个提示行 enable，但 vim 不总是从交互式 shell 起的（`git commit` 编辑器、脚本），所以 nvim 侧兜底必须有。
- `_glfw.ns.unicodeData` 为 NULL（TIS 不可用，极罕见）时无法生成文本，退化为不产生文本。

### 用户侧配置（~/.dotfiles/）

两边都发同一个 OSC，**没有任何 kitty 专属分支**：

- `vim/lua/lu5je0/misc/ime/osc/backend.lua`——原 `ssh/backend.lua` 改名而来（它本来就不只服务 ssh）。`init.lua` 的 `select_backend_module()` 在 `TERM == 'xterm-kitty'` 时优先选它，ssh 兜底也指向它。
  - 该 backend **不实现** `keeper`/`on_change`（IME 是硬禁用，不存在"用户偷偷切回中文"）。因此 `init.lua` 的初始状态同步必须放在 `config_keeper()` **外面**——那个函数对没有 keeper 的 backend 会提前 return，否则一进 vim 不会禁用。
  - `init.lua` 用**单个 `ModeChanged`** 事件从当前模式推导状态，而不是挂 `InsertEnter`/`InsertLeave`/`CmdlineEnter`/`CmdlineLeave`/`TermEnter`/`TermLeave` 六个边沿。硬禁用下漏掉任何一个边沿就是"某个模式打不出中文"且无法手动恢复；`ModeChanged` 不可能漏配对，还顺带覆盖了那六个从不上报的模式。另外 `BufEnter`/`WinEnter`/`FocusGained` 会重新断言一次，用来自愈内层 zsh 改过的状态。
  - **键位映射必须用 `<Cmd>` 而不是 `:cmd<CR>`**。走 cmdline 的映射每次按键都会进出 cmdline 模式，制造出虚假的模式跳变，导致映射"结束"在 cmdline 而不是真正的目的模式（`ext/terminal.lua` 里 toggleterm 那组键就踩了这个坑：切回 terminal 后 IME 停在禁用）。`<Cmd>` 完全不改变模式。真实的模式切换（`<Esc>`、`<C-\><C-n>`）该留就留。
  - **已知限制**：`init.lua` 保留了 rate limiter（7 次/0.5 秒），被限流的更新会**直接丢弃**。极快速地来回切模式有可能把最后一次 enable 丢掉，从而卡在禁用状态；靠下一次模式切换或 `BufEnter`/`WinEnter` 自愈。想彻底消除就把限流分支改成延后 550ms 重新 `sync`，而不是 return。
- `zsh/vi-im-switch.zsh`——包装 `vi-mode.zsh` 已有的 `zle-keymap-select` / `zle-line-init` / `zle-line-finish` 三个钩子（用 `functions -c` 复制原函数再调用，**不能直接重定义**，否则会干掉光标形状逻辑）。`zshrc` 里 `vi-mode.zsh` 先 source，顺序不能反。

## macOS 26 视觉修正（与上游有意分歧，合并主干时注意）

1. **回退上游 commit `b16221a1d`**（"macos: explicitly enable modern window corners on macOS 26"）：删除了 `glfw/cocoa_window.m` 中的 `apply_window_corner_curve()` 及其调用。合并主干如果冲突，**保持删除**。该提交在 glfw/glfw.py 中加的 QuartzCore 链接保留不动。
2. **`setup.py` Info.plist 增加 `UIDesignRequiresCompatibility=True`**：让 macOS 26 (Tahoe) 使用传统 UI 设计（小窗口圆角，非 Liquid Glass）。这是保留大圆角修正的关键，合并时保留。

## 合并主干流程

1. `git merge upstream/master`
2. 冲突处理：
   - `kitty/options/parse.py`、`types.py` 等生成文件：任选一边解决后，用 `./kitty/launcher/kitty +launch gen config` 重新生成即可
   - `glfw/cocoa_window.m`：保持 `apply_window_corner_curve` 删除状态；`keyDown:`/`flagsChanged:` 里的 `filter_result` / `ime_disabled` 改动要保留（上游经常动这几行的 `process_text` 计算）
   - `kitty/vt-parser.c`：`case 1337:` 那行 `fork_ime_handle_osc1337(...)` 拦截要保留，且必须在 `START_DISPATCH` 之前
   - `glfw/wl_text_input.c`：fork 的 4 处纯新增（`fork_ime_inhibited`、enter 早退行、FOCUS case 拦截行 + `fork_ime_force_disable()`、末尾 `glfwWaylandSetIMEInhibited()`）要保留；`kitty/keys.c` `update_ime_focus` 开头、`kitty/glfw.c` `window_focus_callback`、`kitty/screen.c` `do_screen_reset` 各 1 行 fork-local 调用要保留
   - 其余文件冲突按上表理解语义手动合
3. `./dev.sh build` 重新构建（需 Go 工具链；国内网络建议挂代理 `export https_proxy=...`）
4. `./test.py` 跑测试，尤其 `--module ime_mode` 和 `--module ime_e2e`
5. 验证：**`open -n kitty/launcher/kitty.app`**（一定要带 `-n`，否则 macOS 按 bundle id 匹配，只会把已在跑的 `/Applications/kitty.app` 切到前台，你测的是旧二进制）。检查 tab 点击/关闭/新建、窗口圆角、标题栏无分隔线阴影；切到中文输入法后在 vim 里 normal 模式应无法组词、按 `i` 进 insert 后恢复正常，且 `cmd+i` 这类 key equivalent 仍要生效（见"两条踩过的坑"）

## 部署

1. `./dev.sh build`
2. `./deploy.sh` — 删除 `/Applications/kitty.app` 旧版，复制新版过去
3. `open /Applications/kitty.app` 启动

## 用户配置（~/.dotfiles/kitty/）

- `macos.conf`：`macos_titlebar_tabs yes`
- `kitty.conf`：`macos_titlebar_color dark` + 中性灰 tab 配色（`active_tab_background #626366` 等）
