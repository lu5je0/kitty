# TODO: 把原生标题栏 Tab 栏移植到 Wayland

分支 `wayland-titlebar-tabs`。目标：Wayland 下做出和 macOS **视觉完全一致**的标题栏 tab 栏。
参考效果图 `macos.png`（仓库根目录）。

> **前提**：字体渲染和红绿灯（窗口按钮）的差异已被明确接受，不追求一致。
> 其余（几何、圆角、配色、动画时长与曲线、交互行为）要求逐项对齐。
> 用户后续追加的偏好：第一个 tab 距左缘 8px（不再是 0）；窗口按钮用 KDE 尺寸的紧凑风格。

**当前状态（2026-08-05，最新提交已推送 origin）**：Stage 1-3 全部完成并验证 ——
静态渲染 + 命中/点击语义（Stage 2）、5 种 0.18s ease-out 动画（真机中间帧 + `.wl-test/anim_harness.c`
离屏 10 项断言）、配色逐像素对齐 macos.png（bar #393A39 / active tab #626366，`forced_appearance`
尊重 `macos_titlebar_color dark`）、四角圆角（顶角 CSD、底角 GL corner mask）、标题省略号截断、
拖拽闪动修复（常驻 desync）、紧凑窗口按钮（无圆底细线风格，chevron 臂 5.0 / × 臂 4.5）。
剩余：hover/拖拽滑入人工确认、跨窗口拖拽（Stage 4，上游 mime 方案）、mutter/sway 实测、分数缩放实测。
ghost 拖出栏外跟随 Y 已确认**不做**（见"已知会退化的点"）。

---

## 已完成（本次在 macOS 上做的，只是铺路，不含 Wayland 实现）

一个提交，纯重命名 + 预留跨平台入口，**macOS 行为零变化**：

| 文件 | 改动 |
|---|---|
| `kitty/options/definition.py` | 选项 `macos_titlebar_tabs` → `native_titlebar_tabs`，文档改写为跨平台措辞；追加 `add_deprecation('deprecated_macos_titlebar_tabs_alias', 'macos_titlebar_tabs')` |
| `kitty/options/utils.py` | 文件末尾追加 `deprecated_macos_titlebar_tabs_alias()`，旧名仍可用（只 log 一次） |
| `kitty/options/parse.py` / `types.py` | 由 `./kitty/launcher/kitty +launch gen config` 重新生成，勿手改 |
| `kitty/glfw.c` | Python API `cocoa_set_titlebar_tabs` → `set_titlebar_tabs`；非 Apple 分支留 `TODO(wayland)` |
| `kitty/fast_data_types.pyi` | 同步重命名 |
| `kitty/tabs.py` | 新增模块级 `native_titlebar_tabs_supported()`；`use_native_titlebar_tabs` 改读新选项；调用点改 `set_titlebar_tabs` |

**故意留的一处 TODO（已解决）**：`kitty/tabs.py` 的 `native_titlebar_tabs_supported()` 已改回
`return is_macos or is_wayland()`（Wayland 渲染器已落地）。

顺带：`~/.dotfiles/kitty/macos.conf` 里还是 `macos_titlebar_tabs yes`，走 alias 仍生效，有空改成 `native_titlebar_tabs yes`（那是另一个仓库）。

---

## macOS 侧确切规格（从 `kitty/cocoa_window.m:1395` 起的折叠段提取，Wayland 要逐项复刻）

### 常量
```
kTabMaxWidth      200      kTabMinWidth   60
kTabHeight        24       kTabSpacing    4
kTabCornerRadius  6        kNewTabButtonWidth 28
kTabBarLeftMargin 84       (让位红绿灯；Wayland 上左边距 8（用户要求），按钮在右侧)
右内边距          8        (trailingAnchor constant:-8)
kTabAnimationDuration 0.18 曲线 ease-out
kTabDetachMargin  40       (撕下阈值)
```

### 布局算法
```
available  = bar_width - kNewTabButtonWidth - kTabSpacing
tab_width  = n>0 ? MIN(200, MAX(60, (available - 4*(n-1)) / n)) : 200
y          = 可见标题栏垂直中点 - kTabHeight/2
x 从 0 开始，每个 tab 占 tab_width，步进 tab_width + 4
+ 按钮紧跟最后一个 tab，宽 28 高 24
```
注意：垂直居中基于「可见标题栏区间」算，macOS 上是 `contentLayoutRect` 顶到窗口顶；
Wayland 上就是 titlebar buffer 的整个高度（`visible_titlebar_height`）。

### 单个 tab 的绘制
- 圆角矩形填充，半径 6，颜色 = `bg_rgb`
- hover 且非 active：bg 朝白（若 luminance<0.5）或黑混合 **0.08**
- needs_attention 且非 active：前景色改 systemOrange
- 标题：系统字体 12pt，**居中对齐**，尾部截断；文字框 = `(8, (h-16)/2-1, max(0, w-8-22), 16)`
- 关闭按钮矩形 = `(w-20, (h-14)/2, 14, 14)`
  - close hover：用 fg@0.25 填充**圆形**
  - × 描边：`close_hovered ? fg : fg@0.6`，线宽 **1.2**，在 14×14 内 inset **4.25**
  - close 的命中区是上述矩形再外扩 2px（即 18×18）

### + 按钮
- 圆角矩形半径 6；hover 底色 = 深色主题 `white@0.12` / 浅色 `black@0.08`
- + 描边：secondaryLabelColor，线宽 1.2，从中心向四方各伸 **arm=5.0**

### 动画（全部 0.18s / ease-out）
- 位置与宽度变化：插值
- 新增 tab：不做位移动画，直接就位 + alpha 0→1
- 关闭 tab：alpha →0，动画结束后移除
- hover 背景色：`CABasicAnimation` on backgroundColor
- 切 active：变为 active 的那个 tab 走一次带动画的配色应用

### 交互
- 左键 release 落在 close 命中区 → CLOSE，否则 → ACTIVATE
- 中键 release → CLOSE
- + 按钮 release → NEW
- 拖拽阈值：`|dx| >= 4 || |dy| >= 4`
- 拖拽开始时先发 ACTIVATE，并把 ghost 提到最上层
- ghost 跟随光标 X（带抓取偏移 `dragGrabOffsetX`），Y 锁在栏内；
  一旦光标超出栏上下 40px，Y 也跟随光标（**ghost 会画到标题栏外面**）
- drop index：`moving_right ? ghost_right > midX(tab) : midX(tab) < ghost_left`，排除被拖的那个
- 松手时 `p.y < -40 || p.y > bar_h+40` → DETACH，否则 → DROP 到 last_index
- 标题栏空白区仍可拖动窗口（`mouseDownCanMoveWindow = YES`）

---

## Wayland 侧已勘明的落点（都已核对过行号）

- CSD 实现：`glfw/wl_client_side_decorations.c`（900 行）
- titlebar 是一个 **wl_subsurface + shm ARGB8888 双缓冲**，
  尺寸 `window->wl.width × visible_titlebar_height`，位置 `(0, -visible_titlebar_height)`
  见 `create_shm_buffers()` 和 `ensure_csd_resources()`
- 尺寸常量：`csd_initialize_metrics()`（约 176 行）硬编码 `width=12, top=36`，
  `visible_titlebar_height = top - width = 24` —— 恰好等于 kTabHeight。
  想要「24 高的 tab 居中在更高的标题栏里」就调大 `top`，几何算式全部从 metrics 派生
- 重绘并提交 titlebar 的入口：`csd_change_title()`；父表面提交用 `commit_window_surface_if_safe()`
  （`glfw/wl_window.c:3163` 附近有现成用法）
- 指针事件入口：`csd_handle_pointer_event()`（约 884 行），
  `button` 为 -1/-2/-3 分别表示 move/enter/leave
- hover 与命中现有范式：`decs.close.left/width` + `update_hovered_button()`，
  坐标用 **scaled px**（`round(fscale * x)`）
- 现成的绘制工具：`patch_titlebar_with_alpha_mask()`、`render_line()`、
  `render_button()`（自带 4x 超采样 + `downsample()`）—— 圆角矩形和 × / + 都能复用这套
- 文字渲染：`render_single_line()`（`kitty/freetype_render_ui_text.c:439`）
  签名 `(ctx, text, sz_px, fg, bg, buf, width, height, x_offset, y_offset, right_margin, center_runs)`。
  **注意它会自己把 `x_offset..width-right_margin` 的矩形填成 bg**，
  且 `y_offset` 会按 `(height - text_height)/2` 自动垂直居中。
  颜色按 ARGB 直接传（`draw_text_callback` 现在就是这么用的，和 shm 的 ARGB8888 一致）。
  另有 `draw_window_title()` 能回填 `actual_width`，可用来做省略号截断
- 动画驱动：事件循环自带 timer —— `addTimer/removeTimer/toggleTimer/changeTimerInterval`
  （`glfw/backend_utils.h:97-101`）。**不要自己造 timer**
- 强制 CSD（GNOME/mutter 默认 SSD）：照 `decs.titlebar_hidden` 那条路，
  `glfw/wl_window.c:1017-1027` 里 `zxdg_toplevel_decoration_v1_set_mode(..., CLIENT_SIDE)`

### glfw 导出与 wrapper 生成（关键，别踩）
- glfw 是单独的 .so，kitty 通过 `kitty/glfw-wrapper.{h,c}` 动态加载
- 生成命令：`cd glfw && python3 glfw.py`（纯 Python，任何平台可跑）
- `glfw/glfw.py:289 generate_wrappers()`：
  - 自动抓 `glfw3.h` 里所有 `^GLFWAPI ...;`
  - **另有一份硬编码清单**（约 302-338 行）放平台专属函数，
    `glfwWaylandSetTitlebarColor` / `glfwWaylandSetTitlebarHidden` 就在里面 —— 新的 Wayland 导出加到这里
  - `preamble = src[p+2:first]`，`first` = **第一个** `GLFWAPI` 的位置。
    所以给 wrapper 用的 struct/typedef 必须放在 `glfw3.h` 里 **第一个 GLFWAPI 之前**。
    推荐插在 `} GLFWgamepadstate;` 之后、`GLFW API functions` 横幅之前（`glfw3.h` 约 2064 行）
- 缺符号是安全的：wrapper 里 `dlsym` 失败会清 error，函数指针为 NULL，
  调用点按 `if (global_state.is_wayland && glfwWaylandXxx)` 守卫（`kitty/glfw.c:1576` 有范例）

---

## 接下来要做的（按顺序）

### Stage 1 收尾 —— ✅ 已完成（2026-08-04）
1. ✅ `glfw/glfw3.h` preamble：`GLFWTitlebarTab`、`GLFWTitlebarTabAction`（带 `GLFW_TITLEBAR_TAB_` 前缀）、
   `GLFWtitlebartabactionfun(window, action, tab_id, index)`、`GLFWtitlebartabtextfun(window, text, sz_px, fg, bg, buf, w, h, x_off, y_off, right_margin)`
2. ✅ 注册函数声明加在 Vulkan 段 `#endif` 之后；实现追加在 `glfw/input.c` 末尾；`_glfw.callbacks` 尾部加两个成员
3. ✅ `glfw.py` 清单加 `glfwWaylandSetTitlebarTabs`
4. ✅ wrapper 已重新生成（纯新增 39 行）
5. ✅ `kitty/glfw.c`：`set_titlebar_tabs` 用 `NativeTabInfo` 宏共享解析循环；非 Apple 分支
   `if (global_state.is_wayland && glfwWaylandSetTitlebarTabs)` 转发
6. ✅ 文字回调 `titlebar_tab_text_callback`（**居中逻辑在 kitty 侧做**：先 `freetype_text_width_for_single_line`
   算宽再挪 x_offset，`render_single_line` 的 center_runs 参数传 false，多 run 文本不会各自居中）；
   动作回调 `titlebar_tab_action_callback` 五个 action 拼 payload 直呼 `call_boss`（NEW 的 payload 是 "os_window_id 0"，
   `_titlebar_tab_payload` 要求恰好两个数）
7. ✅ `tabs.py` 已改回 `is_macos or is_wayland()`

### Stage 2：Wayland 静态渲染与命中 —— ✅ 已完成（2026-08-04），待真机验证
8. ✅ `glfw/wl_titlebar_tabs.{c,h}` 已建，加进 `source-info.json`（注意保持 2 空格缩进）
9. ✅ 模块内链表 `all_states` 按 `window->id` 索引；`csd_free_all_resources()` 头部一行清理
10-11. ✅ 没用 alpha 蒙版 patch 方案，改为**解析覆盖率**（每像素 4×4 子采样）：
    `rounded_rect_coverage` / `segment_coverage` / `circle_coverage` + `blend_argb`；
    每个 tab 先在 scratch Canvas 里填 bg → 文字回调 → close 圆/×，再 `canvas_composite_rounded` 进 bar
12. ✅ `render_title_bar()` 钩子（4 行 + include）：available = buffer.width - num_buttons*button_size - 8*fscale
13. ✅ `update_hovered_button()` 加 1 行；`handle_pointer_button()` 开头 1 行转发（含 BTN_MIDDLE，
    wl_init.c 的 pointerHandleButton 对 CSD focus 的所有 button 都会转发过来，无需改它）；
    `handle_pointer_leave()` 加 1 行清 hover
14. ✅ 双击最大化/拖窗/右键菜单只在 tab 命中失败时走上游逻辑（转发函数返回 false）
15. ✅ 强制 CSD 放在 `glfwWaylandSetTitlebarTabs` 里：`decs.serverSide` 时 set_mode(CLIENT_SIDE) + csd_set_visible
16. **标题栏加高的做法**：没改 `csd_initialize_metrics()`（避免所有窗口受影响），而是首次收到
    tabs 时把该窗口 `decs.metrics.top` 改为 `width + 38`（`visible_titlebar_height=38`，24px tab 居中余 7px），
    并把 `decs.for_window_state.width = 0` 强制 `ensure_csd_resources()` 重建缓冲。
    **风险**：若首次 set 晚于首次 configure，xdg geometry 可能一帧不一致 —— 真机验证点之一
17. 构建验证：`python3 setup.py build` 通过（-Werror），`nm -D glfw-wayland.so` 三个符号都在，
    `./test.py --module ime_mode` 通过。构建需要 `fonts/SymbolsNerdFontMono-Regular.ttf`（repo 根 fonts/，勿提交）

### Stage 2 真机验证 —— ✅ 已完成（2026-08-04，kwin/Fedora，spectacle 截图比对）

实测修出来的问题与新增（都已验证）：
- **标题栏底色不生效**：`glfwWaylandSetTitlebarColor` 在 `decs.serverSide` 时直接丢弃（kwin 初始 SSD，颜色调用发生在窗口创建时、强制 CSD 之前）。修法：`glfwWaylandSetTitlebarTabs` 增加 `bar_color/use_system_color` 参数，kitty 侧从 `w->last_window_chrome` 透传，函数内强制 CSD 后走 `csd_set_titlebar_color()`。**不要**用「tabs 显示后再调一次 `set_os_window_chrome`」的方案——它会被 `last_window_chrome` 的去重挡掉
- **标题栏高度**：38 → **28** 逻辑 px（从 macos.png 逐像素实测：24px tab 上下各 2px）
- **窗口顶角圆角**：`round_top_corners()` 预乘 alpha 切角，半径 10 逻辑 px。**必须在窗口按钮之后执行**（上游 drawb 会用不透明像素盖掉右上角）——现在整个 bar（含按钮）都由 `wl_titlebar_tabs_render_bar()` 画，顺序已保证。底部两角是 GL 主表面，CSD 侧切不了（要么 kwin 特效，要么 kitty GL 改动），目前方角
- **紧凑窗口按钮**（用户要求 KDE 尺寸）：上游按钮是 bar 高度见方（28px bar 下太大），改为自绘 Breeze 风格：cell 宽 28、图标臂 5、hover 圆底直径 20，min=下箭头/max=上箭头（最大化时反转）/close=×（hover 红圆底）。`decs.minimize/maximize/close.left/width` 仍被写入，上游命中逻辑照常工作
- **左边距**（用户要求）：第一个 tab 距左缘 `TAB_BAR_LEFT_MARGIN = 8` 逻辑 px
- **栏内拖动重排 + 撕下**（用户要求）：press 记录 grab offset，位移 ≥4px 进入拖动（先发 ACTIVATE，同 macOS），ghost 跟随光标钳制在 bar 内每帧重绘；松手时光标纵向超出 bar±40px → DETACH，否则按 ghost 中心所在 slot 计算 index → DROP。**跨窗口拖拽仍未做**（上游 mime 拖拽那套，见 Stage 4 原计划）
- 离屏回归（`.wl-test/harness.c`，9 项全过）：ACTIVATE/CLOSE/中键/NEW/空白区不吞事件/拖出无动作/拖动激活/DROP index/DETACH

颜色实测（kwin 截图 vs macos.png）：bar #393A39 ✓、active tab #626366 ✓、标题字 #F0F0F1 ✓、× alpha 0.6 ✓

### 仍待做
- hover 动画/拖拽滑入真机手动确认（指针事件无法脚本化；引擎与其它动画共用，中间帧已验）
- mutter（强制 CSD 路径）、sway/hyprland 实测；分数缩放真机实测
- 跨窗口拖拽（复用上游 mime 方案，见 Stage 4）

### Stage 3：动画 —— ✅ 已完成（2026-08-04，kwin 真机中间帧截图验证）

实现（全部收在 `glfw/wl_titlebar_tabs.c`，零新钩子）：
- **tab 状态持久化 + 按 tab_id diff**：`glfwWaylandSetTitlebarTabs` 不再全量重建；
  被移除的 tab 标记 `dying` 冻结原位淡出，淡完在 timer tick 里 reap
- 5 种动画全部 0.18s / ease-out cubic（`Anim {from,to,start,active}` 小引擎）：
  位置/宽度插值（目标在 render 里算；bar 宽或 fscale 变化时**快照不动画**，防 resize 拖尾）、
  新 tab 原位 alpha 0→1（窗口首批 tab 不淡入）、关 tab alpha→0、
  hover 背景渐变（close hover 保持瞬时，同 macOS）、变 active 的 tab 配色过渡（其它 tab 快照）
- **文字 alpha 蒙版缓存**：标题以白字黑底经现有回调渲染一次（宽 = 200-8-22 逻辑 px），
  取 G 通道 + 紧致包围盒；动画帧只逐像素 blend，不跑 FreeType。title/fscale/tab_h 变化时失效（重命名已实测）
- **timer**：模块级单例 `addTimer` 16ms，有动画才 enable；tick 里对动画中的窗口
  `wl_subsurface_set_desync` + `csd_change_title()`（desync 下子表面提交即时生效）；
  全部结束后关 timer 并 `set_sync` 恢复（保 resize 原子性）。实测动画结束后进程 CPU 0.0%
- 拖拽 DROP/DETACH 松手时把被拖 tab 的 move_x 置为 ghost 位置 → 新列表到达后从 ghost 滑入新 slot
- `rounded_rect_coverage` 加内部像素快速路径（不跑 16 子采样）

顺带（同日，用户要求）：
- **窗口按钮重绘**：去 hover 圆底（含 close 红圆）、全不透明细线、chevron 臂 6.5/rise 0.4、
  × 臂 5.5、线宽 1.4（hover 加粗 1.45×），对齐用户给的参考截图
- **配色对齐 macOS**（用户报 tab 栏颜色对不上。根因：mac 用 `macos_titlebar_color dark`，
  Wayland 读的是 `wayland_titlebar_color`，未设 → 跟随 kwin 浅色方案）：
  kitty/glfw.c 的 `set_titlebar_tabs` 透传 `forced_appearance`（macos_titlebar_color 负值取负：1=light 2=dark），
  `glfwWaylandSetTitlebarTabs` 加第 6 参（glfw.py 清单已改、wrapper 已重新生成）；
  render_bar 非自定义色时用实测 macOS 标题栏色：深色聚焦 `#393A39`（vs macos.png 逐像素 ✓，
  active tab #626366 ✓），未聚焦 #2C2C2C、浅色 #ECECEC/#F6F6F6 为近似值；
  显式设置 `wayland_titlebar_color` 仍最优先
- **底部窗口圆角**（GL 主表面，kwin 截图验证 ✓）：帧末 corner mask pass ——
  新 shader `kitty/corner_mask_fragment.glsl`（circle SDF 输出 coverage，复用 `rounded_rect_vertex.glsl`），
  `CORNER_MASK_PROGRAM` 注册（shaders.c 枚举 + C() 导出 + shaders.py 编译 + pyi）；
  `stop_os_window_rendering()` 末尾调 `draw_bottom_corner_masks()`（`glBlendFunc(GL_ZERO, GL_SRC_ALPHA)`
  把预乘像素乘以覆盖率，半径 10×scale 与 `WINDOW_TOP_CORNER_RADIUS` 一致）；
  开关是 `OSWindow.wayland_titlebar_tabs_active`（set_titlebar_tabs 里置位）。
  opaque region：`wl_window.c update_regions()` 挖掉两个 10×10 逻辑 px 角 +
  `glfwWaylandSetTitlebarTabs` 里立即重设一次（update_regions 只在建窗/resize 跑）。
  EGL surface 永远带 alpha（GLFW 默认 alphaBits=8），opacity==1 也能用
- **标题尾部截断加省略号**（kwin 截图验证 ✓ `…trunca…`）：溢出时不再硬裁剪，
  文字画到 `box_w - ell_w` 后接缓存的 `…` 蒙版（`ellipsis_cache` 按 sz_px/tab_h 全局缓存一份），
  对齐 macOS 的 NSLineBreakByTruncatingTail
- **拖拽闪动修复**（用户实测反馈）：sync（指针路径随父表面上屏）与 desync（timer 即时上屏）
  提交交错导致新旧缓冲乱序显示。改为 tab 栏激活期间子表面**常驻 desync**（render_bar 里按
  `desynced_subsurface` 指针变化重申，子表面重建后自动恢复），不再来回切换。
  代价：resize 时标题栏与主表面原子性略降，可接受
- **离屏动画 harness**（`.wl-test/anim_harness.c`，已随仓库提交）：假时钟（替身 `monotonic_()`）驱动
  wl_titlebar_tabs.c 真实代码，10 项断言：初始布局配色、hover 渐入/渐出中间值、拖拽激活、
  DROP index、重排滑入中间帧与收敛、垂死 tab 淡出与 reap、timer 自动停。
  编译：`gcc -D_GLFW_WAYLAND -DHAS_MEMFD_CREATE -I../glfw -I.. anim_harness.c ../glfw/wl_titlebar_tabs.c
  $(pkg-config --cflags dbus-1 xkbcommon) $(pkg-config --cflags --libs wayland-client) -lm`

### Stage 4：拖拽与撕下
20. **先读上游代码再动手**：上游已有完整的跨平台 tab 拖拽 ——
    `kitty/tabs.py:1796 start_tab_drag()`、`handle_tab_bar_mouse()`（1828）、
    `on_tab_drop_move()`（1723）、`on_tab_drop()`（1763）、
    `boss.py:2069` 的「拖到 tab 栏外就 detach」、mime `application/net.kovidgoyal.kitty-tab-<pid>`、
    `glfwStartDrag()`（`glfw3.h:4995`，Wayland 的 `wl_data_device` 全套在 `wl_window.c:2600+`）。
    **跨窗口拖拽和撕下一律复用这套，不要重写。**
21. 栏内重排：自己处理 press→motion→release（同一表面上是隐式抓取，事件拿得到），
    drop index 算法照 macOS 那条公式
22. ghost 要能画到标题栏外面 → 给它**单独一个 subsurface**，尺寸 tab_w×tab_h，
    位置相对主表面任意摆。这是 Wayland 下唯一能复刻 macOS 观感的做法
23. 撕下：`p.y` 超出栏上下 40px 时松手 → detach

### 收尾
24. 更新 `AGENTS.md`：特性小节标题里的 `macos_titlebar_tabs` 改名，
    数据流补 Wayland 那一路，改动文件表加新文件，合并主干流程补新的冲突点
25. 多合成器实测：**mutter（默认 SSD，必须验证强制 CSD 生效）**、KDE/kwin、sway 或 hyprland
26. **分数缩放实测**（1.25x / 1.5x）：所有 tab rect 必须存 scaled px，命中测试 `round(fscale*x)`

---

## 已知会退化的点（记录，不必修）

- **拖拽 ghost 锁在标题栏内**（macOS 上超出栏 ±40px 后 ghost 跟随光标画到栏外）：
  需要独立 subsurface，无法自动化测试且协议错误会杀掉整个连接。
  用户已确认**接受，不做**（2026-08-05）。拖出 ±40px 松手撕下（DETACH）功能本身正常
- 字体：Wayland 用 kitty 的 FreeType + 终端字体，不是桌面 UI 字体（`glfw/linux_desktop_settings.c` 里没有任何字体相关代码，要拿 UI 字体得自己读 gsettings / kdeglobals）
- 窗口按钮在**右侧**且是 kitty 手绘的直线，不是 macOS 红绿灯；因此 tab 从 x=0 开始、右侧给按钮留空，布局相当于 macOS 的镜像
- 标题栏无 vibrancy：CSD 的 titlebar buffer 是 `| 0xff000000` 的纯不透明色

## 构建与验证

```sh
./dev.sh build            # 需要 Go 工具链；国内网络挂代理 export https_proxy=...
./test.py                 # 尤其 --module ime_mode 和 --module ime_e2e（另一个特性的回归）
cd glfw && python3 glfw.py   # 只在动过 glfw3.h 导出或 glfw.py 清单后需要
./kitty/launcher/kitty +launch gen config   # 只在动过 options/definition.py 后需要
```

macOS 上验证一定要 `open -n kitty/launcher/kitty.app`（**必须带 `-n`**，否则只是把已在跑的
`/Applications/kitty.app` 切前台，测的是旧二进制）。
