# TODO: 把原生标题栏 Tab 栏移植到 Wayland

分支 `wayland-titlebar-tabs`。目标：Wayland 下做出和 macOS **视觉完全一致**的标题栏 tab 栏。
参考效果图 `macos.png`（仓库根目录）。

> **前提**：字体渲染和红绿灯（窗口按钮）的差异已被明确接受，不追求一致。
> 其余（几何、圆角、配色、动画时长与曲线、交互行为）要求逐项对齐。

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

**故意留的一处 TODO**：`kitty/tabs.py` 的 `native_titlebar_tabs_supported()` 目前 `return is_macos`。
Wayland 渲染器落地后改成 `return is_macos or is_wayland()`，并把 `is_wayland` 加回 `from .constants import` 那行。
现在不改是为了避免中间状态下在 Wayland 开了选项 → 网格 tab bar 被隐藏但又没有原生 tab 栏画出来（等于完全没有 tab 栏）。

顺带：`~/.dotfiles/kitty/macos.conf` 里还是 `macos_titlebar_tabs yes`，走 alias 仍生效，有空改成 `native_titlebar_tabs yes`（那是另一个仓库）。

---

## macOS 侧确切规格（从 `kitty/cocoa_window.m:1395` 起的折叠段提取，Wayland 要逐项复刻）

### 常量
```
kTabMaxWidth      200      kTabMinWidth   60
kTabHeight        24       kTabSpacing    4
kTabCornerRadius  6        kNewTabButtonWidth 28
kTabBarLeftMargin 84       (让位红绿灯；Wayland 上左边距 0，按钮在右侧)
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

### Stage 1 收尾（剩下的）
1. `glfw/glfw3.h` 在 preamble 尾部（`} GLFWgamepadstate;` 之后）追加：
   - `typedef struct GLFWTitlebarTab { unsigned long long tab_id; const char *title; bool is_active, needs_attention; unsigned int fg, bg; } GLFWTitlebarTab;`
   - `typedef enum GLFWTitlebarTabAction { ACTIVATE, CLOSE, NEW, DROP, DETACH }`
   - 动作回调与 tab 文字回调的 typedef
2. `glfw/glfw3.h` 在所有 `GLFWAPI` 声明的**末尾**追加跨平台注册函数
   （仿 `glfwSetDrawTextFunction`，`glfw3.h:2110`；实现放 `glfw/input.c`，
   状态存 `_glfw.callbacks`，见 `glfw/internal.h:652`）
3. `glfw/glfw.py` 硬编码清单里加 `void glfwWaylandSetTitlebarTabs(GLFWwindow*, const GLFWTitlebarTab*, size_t)`
4. `cd glfw && python3 glfw.py` 重新生成 wrapper
5. `kitty/glfw.c`：`set_titlebar_tabs` 的非 Apple 分支转发到 `glfwWaylandSetTitlebarTabs`；
   用 `#ifdef __APPLE__ #define NativeTabInfo TitlebarTabInfo #else ... GLFWTitlebarTab #endif`
   让解析循环只写一份
6. `kitty/glfw.c`：实现 tab 文字回调（用 `freetype_render_ctx(false)` + `render_single_line`，
   字号 `12 * scale` px 以对齐 macOS 的 12pt）并在 `glfw_init` 附近注册；
   实现动作回调 → `call_boss(titlebar_tab_activate, "s", "os_window_id tab_id")`，
   **直接复用 `kitty/boss.py` 已有的五个 handler**，macOS 和 Wayland 共用行为
7. `kitty/tabs.py`：把 `native_titlebar_tabs_supported()` 改回 `is_macos or is_wayland()`

### Stage 2：Wayland 静态渲染与命中
8. 新建 `glfw/wl_titlebar_tabs.{c,h}`（**纯 fork 专属文件，零冲突**），
   并加进 `glfw/source-info.json` 的 `wayland.sources`
9. 状态用**模块内的链表**按 `window->id` 索引，**不要动 `glfw/wl_platform.h`**（省一处合并冲突）；
   在 `csd_free_all_resources()` 里挂一行清理
10. 实现 `render_rounded_rect()` 生成 alpha 蒙版 + 一个「位图源」版本的
    `patch_titlebar_with_alpha_mask()`（现有那个只支持纯色 fg）
11. 渲染顺序：整条填标题栏底色 → 逐个 tab（先在 tab 尺寸的 scratch buffer 里填 bg + 画文字 + 画 ×，
    再透过圆角蒙版合成进 titlebar buffer）→ + 按钮
12. 在 `render_title_bar()` 里**只加 4 行钩子**（不改上游任何行）：
    ```c
    if (wl_titlebar_tabs_active(window)) { wl_titlebar_tabs_render(window, output); goto render_buttons; }
    ```
    放在上游那段 `if (window->wl.title && ...)` 之前，让上游的 `render_buttons:` 继续画窗口按钮。
    bar 可用宽度 = `buf_width - num_buttons*button_size - 8*scale`
13. `update_hovered_button()` / `handle_pointer_button()` 各在开头加一行转发；
    `handle_pointer_button()` 目前只处理 `BTN_LEFT`/`BTN_RIGHT`，**要加 `BTN_MIDDLE`**
14. 保留原有标题栏行为：双击最大化、空白区 `xdg_toplevel_move` 拖窗、右键 `xdg_toplevel_show_window_menu`，
    只在非 tab 区域生效
15. 开启选项时强制 CSD

### Stage 3：动画
16. `wl_subsurface_set_desync(decs.titlebar.subsurface)` —— 子表面默认是同步模式，
    必须靠父表面提交才生效；desync 后 titlebar 可独立提交，不和 GL 出帧互相干扰
17. 一个 ~60Hz 的 repeating timer，有任何 bar 在动画时才 enable；
    tick 里推进插值 → `csd_change_title()`
18. **把每个 tab 的标题文字缓存成 alpha 蒙版**。动画每帧重新跑 FreeType 太贵，
    缓存后动画帧退化成纯 blit
19. 逐项对齐上面「动画」小节的 5 种动画

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
