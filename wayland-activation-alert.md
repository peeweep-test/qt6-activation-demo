# Qt6 Wayland Activation 与 Alert 机制技术分析

## 概述

在 Wayland 协议中，窗口激活（Activation）和提醒（Alert）是两个密切相关的窗口管理概念。与 X11 不同，Wayland 的设计哲学是"compositor 全权负责窗口管理"，客户端不能自行决定焦点归属。因此，Qt6 在 Wayland 上实现激活和提醒需要依赖特定的 Wayland 协议扩展。

本文档基于 qtbase 源码，详细分析 Qt6 中这两个机制的完整实现。

---

## 1. 窗口激活（Window Activation）

### 1.1 背景：X11 vs Wayland

**X11**：客户端可以通过 `_NET_ACTIVE_WINDOW` 直接请求激活自己或其他窗口，compositor 通常会遵守。

**Wayland**：客户端不能自行设置键盘焦点。激活必须通过 **xdg-activation-v1** 协议，以"令牌"（token）机制请求 compositor 授权。这是防止焦点窃取（focus stealing）的核心安全机制。

### 1.2 xdg-activation-v1 协议

协议定义文件：[`xdg-activation-v1.xml`](src/3rdparty/wayland/protocols/xdg-activation/xdg-activation-v1.xml)

该协议定义了两个接口：

| 接口 | 角色 |
|------|------|
| `xdg_activation_v1` | 全局接口，提供 `get_activation_token` 和 `activate` 请求 |
| `xdg_activation_token_v1` | 令牌对象，通过 `set_serial`/`set_app_id`/`set_surface` 配置后 `commit`，compositor 回复 `done` 事件返回令牌字符串 |

**激活流程（协议层面）**：

```
发起方客户端                    Compositor                   被激活方客户端
    |                              |                              |
    |--- get_activation_token --->|                              |
    |<-- xdg_activation_token_v1 --|                              |
    |                              |                              |
    |--- set_serial(seat, serial)->|                              |
    |--- set_app_id(app_id) ------>|                              |
    |--- set_surface(surface) ---->|                              |
    |--- commit() ---------------->|                              |
    |<-- done(token) -------------|                              |
    |                              |                              |
    |  (通过 D-Bus/IPC 传递 token)  |                              |
    |-------------------------------+-- token -------------------->|
    |                              |                              |
    |                              |<--- activate(token, surface) -|
    |                              |                              |
```

关键点：
- `set_serial(seat, serial)`：绑定触发激活的用户输入事件，compositor 用此判断请求合法性
- `set_app_id(app_id)`：标识被激活的应用
- `set_surface(surface)`：标识发起方的 surface
- `activate` 请求：被激活方持有 token 后，向 compositor 发送激活请求，compositor 自行决定是否授予焦点

### 1.3 Qt6 C++ 协议绑定层

#### 1.3.1 自动生成的协议绑定

Qt 使用 `wayland-scanner` 工具从 XML 生成 C++ 绑定代码，产出 `qwayland-xdg-activation-v1.h`（位于构建目录）。该文件包含：
- `QtWayland::xdg_activation_v1` — 全局接口封装
- `QtWayland::xdg_activation_token_v1` — 令牌对象封装

这些类继承自 `QtWaylandClient` 内部的 Wayland proxy 基类，提供类型安全的 C++ API。

#### 1.3.2 手写的 C++ 封装

**[`qwaylandxdgactivationv1_p.h`](src/plugins/platforms/wayland/plugins/shellintegration/xdg-shell/qwaylandxdgactivationv1_p.h)**

两个关键类：

```cpp
// 令牌提供者：继承自动生成的 xdg_activation_token_v1 + QObject
// 重写 done() 回调，转为 Qt 信号
class QWaylandXdgActivationTokenV1 : public QObject,
                                      public QtWayland::xdg_activation_token_v1
{
    void xdg_activation_token_v1_done(const QString &token) override {
        Q_EMIT done(token);
    }
Q_SIGNALS:
    void done(const QString &token);
};

// 激活管理器：继承自动生成的 xdg_activation_v1
class QWaylandXdgActivationV1 : public QtWayland::xdg_activation_v1
{
    QWaylandXdgActivationTokenV1 *requestXdgActivationToken(
        QWaylandDisplay *display,
        wl_surface *surface,
        std::optional<uint32_t> serial,
        const QString &app_id);
};
```

**[`qwaylandxdgactivationv1.cpp`](src/plugins/platforms/wayland/plugins/shellintegration/xdg-shell/qwaylandxdgactivationv1.cpp)**

`requestXdgActivationToken` 的实现：

```cpp
QWaylandXdgActivationTokenV1 *
QWaylandXdgActivationV1::requestXdgActivationToken(
    QWaylandDisplay *display, wl_surface *surface,
    std::optional<uint32_t> serial, const QString &app_id)
{
    auto wl = get_activation_token();       // 协议请求：创建令牌对象
    auto provider = new QWaylandXdgActivationTokenV1;
    provider->init(wl);

    if (surface)
        provider->set_surface(surface);     // 设置发起方 surface

    if (!app_id.isEmpty())
        provider->set_app_id(app_id);       // 设置应用标识

    if (serial && display->lastInputDevice())
        provider->set_serial(*serial, display->lastInputDevice()->wl_seat());
                                            // 绑定用户输入事件

    provider->commit();                     // 提交请求
    return provider;
}
```

`★ Insight ─────────────────────────────────────`
**令牌生命周期设计**：`QWaylandXdgActivationTokenV1` 对象在 `done` 信号触发后通过 `QObject::deleteLater` 自动销毁。这是一种常见的安全模式——令牌是一次性的，使用后立即销毁防止重放攻击。同时 `serial` 参数使用 `std::optional<uint32_t>` 表示可选，因为某些场景（如 alert 唤醒）没有对应的用户输入事件。
`─────────────────────────────────────────────────`

---

## 2. 激活的完整调用链

### 2.1 路径 A：应用内窗口激活 (`QWindow::requestActivate()`)

这是最常见的路径，用户点击一个窗口或应用代码调用 `window->requestActivate()`。

```
QWindow::requestActivate()                     [src/gui/kernel/qwindow.cpp:1318]
  │ 检查 Qt::WindowDoesNotAcceptFocus 标志
  │
  └─> QPlatformWindow::requestActivateWindow() [虚函数调用]
       │
       └─> QWaylandWindow::requestActivateWindow()  [src/plugins/platforms/wayland/qwaylandwindow.cpp:1598]
            │
            └─> QWaylandShellSurface::requestActivate()  [虚函数调用]
                 │
                 └─> QWaylandXdgSurface::requestActivate()  [src/plugins/.../qwaylandxdgshell.cpp:584]
                      │
                      ├─ 情况1: 有缓存的 m_activationToken
                      │   └─> activation->activate(token, surface)
                      │       令牌来自 setXdgActivationToken() 预设
                      │
                      ├─ 情况2: 环境变量 XDG_ACTIVATION_TOKEN 非空
                      │   └─> activation->activate(token, surface)
                      │       qunsetenv("XDG_ACTIVATION_TOKEN")  // 一次性消费
                      │
                      └─ 情况3: 需要实时请求令牌
                          │
                          ├─ 获取焦点窗口的 Wayland 表示
                          ├─ 从 lastInputDevice 获取 serial
                          ├─ activation->requestXdgActivationToken(display, surface, serial, appId)
                          │     创建令牌对象，设置 surface/app_id/serial，commit
                          │
                          └─ connect(tokenProvider, &done, [this](token) {
                                 activation->activate(token, window->wlSurface());
                             });
                             // 异步：compositor 回复 done 后发送 activate
```

**情况 1** 对应跨进程激活（如从启动器启动应用），token 由外部传入。
**情况 2** 对应由桌面环境（如 GNOME Shell）通过环境变量传递 token 启动的进程。
**情况 3** 对应应用内部窗口切换，需要实时向 compositor 请求令牌。

### 2.2 路径 B：窗口首次显示时的自动激活

```
QWaylandWindow::setVisible(true)                 [qwaylandwindow.cpp:644]
  │
  └─> mShellSurface->requestActivateOnShow()      [qwaylandwindow.cpp:656]
       │
       └─> QWaylandXdgSurface::requestActivateOnShow()  [qwaylandxdgshell.cpp:624]
            │ 过滤条件：
            │   - Qt::ToolTip / Qt::Popup / Qt::SplashScreen → 不激活
            │   - Qt::WindowDoesNotAcceptFocus → 不激活
            │   - _q_showWithoutActivating 属性 → 不激活
            │
            └─> requestActivate()  // 走路径 A 的逻辑
```

`★ Insight ─────────────────────────────────────`
**过滤逻辑的重要性**：`requestActivateOnShow()` 对 Tooltip、Popup、SplashScreen 窗口类型跳过激活，因为这些窗口按设计不应获取焦点。`_q_showWithoutActivating` 是内部属性，允许窗口显示但不抢占焦点——这在通知弹窗等场景中至关重要。
`─────────────────────────────────────────────────`

### 2.3 路径 C：鼠标点击激活

```
compositor 将键盘焦点赋予窗口
  │
  └─> QWaylandInputDevice::Keyboard::keyboard_enter()  [qwaylandinputdevice.cpp:1299]
       │
       └─> mParent->mQDisplay->handleKeyboardFocusChanged(mParent)  [line:1322]
            │
            └─> QWaylandDisplay::handleKeyboardFocusChanged()  [qwaylanddisplay.cpp:996]
                 │
                 ├─ keyboardFocus = inputDevice->keyboardFocus()
                 ├─ handleWindowActivated(keyboardFocus)   // 激活新焦点窗口
                 │    └─> mActiveWindows.append(window)
                 │    └─> requestWaylandSync()
                 │
                 └─ handleWindowDeactivated(mLastKeyboardFocus)  // 去激活旧窗口
                      └─> mActiveWindows.removeOne(window)
                      └─> requestWaylandSync()
```

同时，`QWaylandXdgSurface::Toplevel::xdg_toplevel_configure()` 也处理激活状态变化（通过 `XDG_TOPLEVEL_STATE_ACTIVATED` 状态位）：

```cpp
// qwaylandxdgshell.cpp:147
void QWaylandXdgSurface::Toplevel::xdg_toplevel_configure(int32_t w, int32_t h, wl_array *states)
{
    for (size_t i = 0; i < numStates; i++) {
        switch (xdgStates[i]) {
        case XDG_TOPLEVEL_STATE_ACTIVATED:
            m_pending.states |= Qt::WindowActive;
            break;
        // ...
        }
    }
}
```

在 `applyConfigure()` 中：

```cpp
// qwaylandxdgshell.cpp:78
void QWaylandXdgSurface::Toplevel::applyConfigure()
{
    if ((m_pending.states & Qt::WindowActive) && !(m_applied.states & Qt::WindowActive)
        && !m_xdgSurface->m_window->display()->isKeyboardAvailable())
        m_xdgSurface->m_window->display()->handleWindowActivated(m_xdgSurface->m_window);
    // ...
}
```

### 2.4 路径 D：同步焦点通知到 Qt 事件系统

```
QWaylandDisplay::requestWaylandSync()
  │ 发送 wl_display.sync 请求
  │
  └─> 回调: QWaylandDisplay::handleWaylandSync()  [qwaylanddisplay.cpp:1035]
       │
       ├─ activeWindow = mActiveWindows.last()->window()
       │
       └─> QWindowSystemInterface::handleFocusWindowChanged(activeWindow)
            // 通知 QtGui 层焦点窗口变更
            // 产生 QEvent::FocusIn / QEvent::FocusOut / QEvent::WindowActivate 等
```

`★ Insight ─────────────────────────────────────`
**Wayland Sync 回调的作用**：`requestWaylandSync()` 使用 `wl_display.sync()` 确保在处理完所有待处理 Wayland 事件后再更新 Qt 的焦点状态。这解决了一个微妙的竞态问题：如果快速收到 activate → deactivate 两个事件，直接处理可能导致最后一个事件丢失（被 QWindowSystemInterface 事件队列合并）。通过 sync 延迟到事件循环的"安全点"再处理，确保最终状态正确。
`─────────────────────────────────────────────────`

---

## 3. Alert（提醒/注意力请求）

### 3.1 背景

在 X11 上，`_NET_WM_STATE_DEMANDS_ATTENTION` 告知窗口管理器某个窗口需要用户注意（例如收到新消息）。窗口管理器通常会在任务栏上闪烁该窗口的图标。

在 Wayland 上，没有专门的 urgency/alert 协议。Qt6 的做法是**复用 xdg-activation-v1 协议**来实现类似效果。

### 3.2 调用链

```
QWindow::alert(int msec)                            [src/gui/kernel/qwindow.cpp:3223]
  │ 如果已是 alert 状态或已激活，直接返回
  │
  ├─> platformWindow->setAlertState(true)
  │    │
  │    └─> QWaylandWindow::setAlertState(true)     [qwaylandwindow.cpp:702]
  │         │
  │         └─> QWaylandXdgSurface::setAlertState(true)  [qwaylandxdgshell.cpp:661]
  │              │
  │              ├─ m_alertState = true
  │              │
  │              └─ activation->requestXdgActivationToken(
  │                     display, surface,
  │                     std::nullopt,  // 注意：没有 serial！
  │                     appId)
  │                  │
  │                  └─ connect(done, [this](token) {
  │                         activation->activate(token, surface);
  │                     });
  │
  └─> 如果 msec > 0，启动 QTimer::singleShot(msec, _q_clearAlert)
       // 定时器到期后调用 platformWindow->setAlertState(false)
```

### 3.3 Alert 实现的关键特点

1. **无 serial 参数**：与正常激活不同，alert 不绑定到任何用户输入事件（`std::nullopt`）。这意味着 compositor 可能会拒绝此请求（因为无法验证请求的合法性），这是预期行为——compositor 可以选择是否响应该提醒。

2. **仍然使用 `activate()`**：alert 最终仍然调用 `xdg_activation_v1.activate()`。compositor 如何处理取决于实现：
   - 某些 compositor（如 GNOME 的 `mutter`）会显示一个视觉提示（如任务栏闪烁）而不真正转移焦点
   - 某些 compositor 可能会直接授予焦点
   - 某些 compositor 可能完全忽略

3. **与正常激活使用相同的 token 流程**：从 compositor 的角度来看，alert 和正常激活使用完全相同的协议消息。区别仅在于 token 请求时是否提供了 `serial`。

4. **自动清除**：`QWindow::alert(msec)` 支持定时自动清除 alert 状态。`msec = 0` 表示不清除（需要手动调用或窗口激活时自动清除）。

### 3.4 与 X11 的对比

| 特性 | X11 | Wayland (Qt6) |
|------|-----|---------------|
| 协议 | `_NET_WM_STATE_DEMANDS_ATTENTION` (EWMH) | `xdg-activation-v1` (复用) |
| 保证性 | 窗口管理器应处理 | compositor 可能忽略 |
| 焦点转移 | 不转移焦点 | compositor 决定 |
| Token | 不需要 | 需要（但无 serial） |
| API | `QWindow::alert(int msec)` | `QWindow::alert(int msec)` (相同) |

---

## 4. XDG_ACTIVATION_TOKEN 环境变量

### 4.1 机制

`XDG_ACTIVATION_TOKEN` 是 FreeDesktop.org 标准的环境变量机制，用于**跨进程传递激活令牌**。当桌面环境启动一个应用时，可以通过此环境变量传递激活令牌，让新启动的应用能够合法地请求焦点。

### 4.2 Qt6 中的使用场景

#### 场景 A：桌面环境启动应用

桌面环境（如 GNOME Shell、KDE Plasma）启动应用时设置 `XDG_ACTIVATION_TOKEN` 环境变量。Qt 应用在 `requestActivate()` 时自动消费：

```cpp
// qwaylandxdgshell.cpp:591
} else if (const auto token = qEnvironmentVariable("XDG_ACTIVATION_TOKEN"); !token.isEmpty()) {
    activation->activate(token, window()->wlSurface());
    qunsetenv("XDG_ACTIVATION_TOKEN");  // 一次性消费后立即清除
    return true;
}
```

#### 场景 B：通过 D-Bus System Tray 激活

`QStatusNotifierItemAdaptor`（StatusNotifierItem D-Bus 接口的实现）接收 compositor 发送的 `ProvideXdgActivationToken` 信号：

```cpp
// qstatusnotifieritemadaptor.cpp:140
void QStatusNotifierItemAdaptor::ProvideXdgActivationToken(const QString &token)
{
    qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
}
```

这用于系统托盘图标点击激活应用窗口的场景。

#### 场景 C：打开 URL / 启动外部进程

[`qdesktopunixservices.cpp`](src/gui/platform/unix/qdesktopunixservices.cpp) 在启动外部应用时传递激活令牌：

```cpp
// 方式1：通过环境变量
qputenv("XDG_ACTIVATION_TOKEN", xdgActivationToken.toUtf8());
::system(command);
qunsetenv("XDG_ACTIVATION_TOKEN");

// 方式2：通过 QProcess 环境
env.insert("XDG_ACTIVATION_TOKEN", xdgActivationToken);
process.setEnvironment(env.toStringList());

// 方式3：通过 XDG Desktop Portal D-Bus 接口
options.insert("activation_token", xdgActivationToken);
```

#### 场景 D：通过 Qt NativeInterface 获取令牌

外部代码可以通过 Qt 的 NativeInterface 获取激活令牌，用于传递给子进程或其他应用：

```cpp
// qplatformwindow_p.h:157 (QNativeInterface::Private::QWaylandWindow)
Q_SIGNALS:
    void xdgActivationTokenCreated(const QString &token);

// qwaylandwindow.cpp:240
void requestXdgActivationToken(uint serial) override;
void setXdgActivationToken(const QString &token) override;
```

使用模式（在 `qdesktopunixservices.cpp` 中）：

```cpp
auto waylandWindow = dynamic_cast<QNativeInterface::Private::QWaylandWindow *>(window->handle());
QObject::connect(waylandWindow,
    &QNativeInterface::Private::QWaylandWindow::xdgActivationTokenCreated,
    waylandWindow, [callback](const QString &token) {
        callback(token);  // 拿到 token 后启动子进程
    }, Qt::SingleShotConnection);
waylandWindow->requestXdgActivationToken(lastInputSerial);
```

---

## 5. 架构层次总结

```
┌─────────────────────────────────────────────────────────────┐
│                    应用层 (Application)                      │
│                                                             │
│  QWindow::requestActivate()  /  QWindow::alert(int msec)   │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│               Qt 平台抽象层 (QPA)                           │
│                                                             │
│  QPlatformWindow::requestActivateWindow()                   │
│  QPlatformWindow::setAlertState()                           │
│  QPlatformWindow::requestXdgActivationToken()               │
└──────────────────────────┬──────────────────────────────────┘
                           │ (virtual dispatch)
┌──────────────────────────▼──────────────────────────────────┐
│            Wayland 平台插件                                  │
│                                                             │
│  QWaylandWindow  →  QWaylandShellSurface                    │
│       │                    │                                │
│       │              QWaylandXdgSurface                      │
│       │              (requestActivate / setAlertState)       │
│                      │                                      │
│                      ▼                                      │
│              QWaylandXdgActivationV1                         │
│              (requestXdgActivationToken / activate)          │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│           自动生成的 Wayland 协议绑定                         │
│                                                             │
│  QtWayland::xdg_activation_v1                                │
│  QtWayland::xdg_activation_token_v1                          │
│  (由 wayland-scanner 从 xdg-activation-v1.xml 生成)         │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│              Wayland 协议 (wire format)                      │
│                                                             │
│  xdg_activation_v1.get_activation_token                     │
│  xdg_activation_token_v1.set_serial / set_app_id / commit   │
│  xdg_activation_v1.activate(token, surface)                 │
└─────────────────────────────────────────────────────────────┘
```

---

## 6. 激活状态管理

### 6.1 活跃窗口追踪

`QWaylandDisplay` 维护活跃窗口列表：

```cpp
// qwaylanddisplay_p.h:392
QList<QWaylandWindow *> mActiveWindows;
```

| 操作 | 方法 | 说明 |
|------|------|------|
| 判断窗口是否活跃 | `isWindowActivated(window)` | 检查是否在 `mActiveWindows` 中 |
| 窗口激活 | `handleWindowActivated(window)` | 追加到列表，触发 `requestWaylandSync()` |
| 窗口去激活 | `handleWindowDeactivated(window)` | 从列表移除，触发 `requestWaylandSync()` |
| 窗口销毁 | `handleWindowDestroyed(window)` | 如在列表中则去激活 |
| 同步到 Qt | `handleWaylandSync()` | 取列表末尾窗口调用 `QWindowSystemInterface::handleFocusWindowChanged()` |

### 6.2 键盘焦点 vs 激活状态

在 Wayland 上，窗口激活状态有两个来源：

1. **键盘焦点变化**（主要）：compositor 通过 `wl_keyboard.enter` 通知客户端键盘焦点变化，`QWaylandInputDevice::Keyboard::keyboard_enter()` → `handleKeyboardFocusChanged()` → `handleWindowActivated()`

2. **XDG toplevel configure 事件**（辅助）：compositor 在 `xdg_toplevel.configure` 事件中通过 `XDG_TOPLEVEL_STATE_ACTIVATED` 状态位告知激活状态变化。这作为键盘焦点不可用时的备用路径。

```cpp
// qwaylandxdgshell.cpp:80
if ((m_pending.states & Qt::WindowActive) && !(m_applied.states & Qt::WindowActive)
    && !m_xdgSurface->m_window->display()->isKeyboardAvailable())
    m_xdgSurface->m_window->display()->handleWindowActivated(m_xdgSurface->m_window);
```

条件 `!isKeyboardAvailable()` 确保两条路径不会冲突——当键盘协议可用时，以键盘焦点为准。

### 6.3 子表面（SubSurface）的激活

对于嵌套在父窗口内的子表面，有独立的激活追踪：

```cpp
// qwaylanddisplay_p.h
QPointer<QWaylandWindow> mActiveSubSurface;

void setActiveSubSurface(QWaylandWindow *window);

// qwaylandwindow.cpp:1982
void QWaylandWindow::handleMousePressActivation()
{
    // 子表面被点击时，不改变顶层窗口的激活状态，
    // 只更新活跃的子表面指针
    if (isSubsurface && currentActiveSubSurface != this && toplevelIsActive(this))
        mDisplay->setActiveSubSurface(this);
}
```

---

## 7. 关键文件索引

| 文件 | 角色 |
|------|------|
| [`src/3rdparty/wayland/protocols/xdg-activation/xdg-activation-v1.xml`](src/3rdparty/wayland/protocols/xdg-activation/xdg-activation-v1.xml) | xdg-activation-v1 协议定义 |
| [`src/plugins/platforms/wayland/plugins/shellintegration/xdg-shell/qwaylandxdgactivationv1_p.h`](src/plugins/platforms/wayland/plugins/shellintegration/xdg-shell/qwaylandxdgactivationv1_p.h) | C++ 封装头文件 |
| [`src/plugins/platforms/wayland/plugins/shellintegration/xdg-shell/qwaylandxdgactivationv1.cpp`](src/plugins/platforms/wayland/plugins/shellintegration/xdg-shell/qwaylandxdgactivationv1.cpp) | C++ 封装实现 |
| [`src/plugins/platforms/wayland/plugins/shellintegration/xdg-shell/qwaylandxdgshell.cpp`](src/plugins/platforms/wayland/plugins/shellintegration/xdg-shell/qwaylandxdgshell.cpp) | XDG Shell 集成（核心激活/Alert 逻辑） |
| [`src/plugins/platforms/wayland/plugins/shellintegration/xdg-shell/qwaylandxdgshell_p.h`](src/plugins/platforms/wayland/plugins/shellintegration/xdg-shell/qwaylandxdgshell_p.h) | XDG Shell 集成头文件 |
| [`src/plugins/platforms/wayland/qwaylandwindow.cpp`](src/plugins/platforms/wayland/qwaylandwindow.cpp) | Wayland 窗口实现 |
| [`src/plugins/platforms/wayland/qwaylandwindow_p.h`](src/plugins/platforms/wayland/qwaylandwindow_p.h) | Wayland 窗口头文件 |
| [`src/plugins/platforms/wayland/qwaylandshellsurface_p.h`](src/plugins/platforms/wayland/qwaylandshellsurface_p.h) | ShellSurface 抽象接口 |
| [`src/plugins/platforms/wayland/qwaylandshellsurface.cpp`](src/plugins/platforms/wayland/qwaylandshellsurface.cpp) | ShellSurface 基类实现 |
| [`src/plugins/platforms/wayland/qwaylanddisplay.cpp`](src/plugins/platforms/wayland/qwaylanddisplay.cpp) | 活跃窗口管理、Wayland Sync |
| [`src/plugins/platforms/wayland/qwaylanddisplay_p.h`](src/plugins/platforms/wayland/qwaylanddisplay_p.h) | Display 头文件（mActiveWindows） |
| [`src/plugins/platforms/wayland/qwaylandinputdevice.cpp`](src/plugins/platforms/wayland/qwaylandinputdevice.cpp) | 键盘焦点事件处理 |
| [`src/gui/kernel/qwindow.cpp`](src/gui/kernel/qwindow.cpp) | QWindow 公共 API |
| [`src/gui/kernel/qplatformwindow.cpp`](src/gui/kernel/qplatformwindow.cpp) | QPlatformWindow 基类 |
| [`src/gui/kernel/qplatformwindow_p.h`](src/gui/kernel/qplatformwindow_p.h) | QNativeInterface::Private::QWaylandWindow |
| [`src/gui/platform/unix/qdesktopunixservices.cpp`](src/gui/platform/unix/qdesktopunixservices.cpp) | XDG token 跨进程传递 |
| [`src/gui/platform/unix/dbustray/qstatusnotifieritemadaptor.cpp`](src/gui/platform/unix/dbustray/qstatusnotifieritemadaptor.cpp) | SNI D-Bus token 接收 |
| `qwayland-xdg-activation-v1.h` (构建目录) | wayland-scanner 自动生成的绑定代码 |
