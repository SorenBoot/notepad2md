# notepad2md

轻量级 Markdown 编辑 + 实时预览工具。单文件 exe，静态链接，**零运行时依赖**。

基于 [Notepad2 4.2.25](https://www.flos-freeware.ch/)（Florian Balmer）与 Scintilla 2.24，使用 MinGW-w64 构建。

## 特性

- **单 exe，约 2.9 MB**（含静态 C/C++ 运行时），拷贝即用，无需安装
- **零运行时依赖** —— 只依赖 Windows 系统 DLL（Win7 起全部自带）
- **内存占用约 40 MB**（开启预览时；MSHTML 引擎约占 25 MB），空载约 15 MB
- **实时 Markdown 预览** —— 左右分栏，边打边看，`Ctrl+Alt+M` 开关
- **可拖动分隔条** —— 拖动调整编辑区/预览宽度，双击复位 50/50，比例自动记忆
- **代码块复制按钮** —— 预览里的代码块带语言标签与「复制」按钮，一键复制源码
- **预览内链接直接外开** —— 点击预览里的链接用系统浏览器打开，不再在窗格内导航
- **简体中文界面** —— 菜单、对话框、状态栏、提示信息全部汉化
- Scintilla 编辑引擎 —— 完整保留 Notepad2 的快捷键、语法高亮、查找替换
- 窗口置顶（`Alt+T`）
- 代码折叠、列选择、正则替换、编码转换

![界面截图](docs/screenshot.png)

### 界面语言

资源文件里同时打包了**简体中文**和**英文**两套界面，Windows 会按系统区域自动选择：

- 系统区域为中文（中国大陆）→ 显示中文界面
- 其他区域 → 显示英文界面

如要在中文系统上强制使用英文界面，可用 [Resource Hacker](http://www.angusj.com/resourcehacker/)
等工具调整资源语言，或把 `src/Notepad2.rc` 里中文块结尾的 `#endif` 注释掉后重新编译。

## Markdown 预览

- **工具栏最右侧的按钮**，或 **`Ctrl+Alt+M`**，或菜单 **查看(V) → Markdown 预览**
- 左右分栏，中间有可拖动分隔条：
  - **拖动**调整两栏宽度；
  - **双击**复位到 50/50；
  - 比例按千分比存进 ini（`PreviewSplitPermille`），下次启动沿用
- **打开 `.md` / `.markdown` / `.mdown` / `.mkd` 文件时自动开启**
- 输入后约 120 ms 自动刷新（合并连续按键，不会每敲一个字都重排）
- **增量刷新，不闪屏**：第一次加载完整文档，之后只改内容容器的 `innerHTML`，
  不重载页面，因此不会白闪，滚动位置也会保留
- 支持 GitHub 风格语法：表格、任务列表、删除线、脚注、围栏代码块
- **代码块带复制按钮**：围栏代码块渲染为「语言标签 + 复制按钮 + 代码」的卡片，
  点「复制」把源码写入剪贴板，按钮短暂变为「已复制」；按钮文案随界面语言切换
- **预览里的链接用系统浏览器打开**（不在预览窗格内跳转），由宿主侧的
  `document.onclick` 委托实现，不修改 `<a>` 本身
- **中文文件无需担心编码**：UTF-8 与 GBK 等本地代码页都会自动转成 UTF-8 渲染
- 安全：HTML 标签被转义（`NOHTML`），预览不会执行脚本

预览使用系统自带 **MSHTML（Trident）** 引擎，而非 WebView2：不额外分发运行时、进程内渲染、内存更省。代价是 CSS 支持止步于 IE11 水平（不支持 CSS 变量 / Grid / `flex gap`）。

## 系统要求

| 项目 | 要求 |
|---|---|
| 操作系统 | Windows 7 SP1 / 8 / 8.1 / 10 / 11（x64） |
| 架构 | x64 |
| 运行时 | **无**（静态链接 libgcc / libstdc++ / winpthread） |

不支持 32 位 Windows。MSHTML 预览渲染依赖系统组件，Win7 起均内置。

## 编译

### 方式一：双击构建（推荐，无需 make）

```
build\build.bat
```

产物输出到 `bin\notepad2md.exe`。

### 方式二：make

```bash
cd build
make                # Release x64
make DEBUG=1        # Debug
make clean
make run
```

### 工具链要求

**MinGW-w64**，GCC 11 或更高。推荐使用便携版：

- [w64devkit](https://github.com/skeeto/w64devkit)（约 100 MB，绿色解压即用）

默认工具链路径为 `C:\Tools\W64DEVKIT\w64devkit\w64devkit`，如装别处：

```bash
# build.bat
set W64DEVKIT=你的路径
```

或

```bash
make W64DEVKIT=/d/your/w64devkit
```

## 目录结构

```
notepad2md/
├── build/              构建脚本（Makefile + build.bat）
├── src/                源码
│   ├── Notepad2.c      主程序（原版未改动）
│   ├── Edit.c          编辑区逻辑
│   ├── Dialogs.c       对话框
│   ├── Styles.c        配色方案
│   ├── Dlapi.c         目录/文件列表 API
│   ├── Helpers.c       工具函数
│   ├── Print.cpp       打印
│   ├── Markdown.c      Markdown 解析（新增）
│   └── Preview.c       预览渲染（新增）
├── scintilla/          编辑引擎（仅编译必需文件）
│   ├── include/
│   ├── lexlib/
│   ├── lexers/
│   ├── src/
│   └── win32/
├── md4c/               Markdown 解析器（MIT）
├── res/                图标 / 光标 / 位图 / manifest
├── docs/               文档
└── LICENSE-THIRD-PARTY/  第三方许可证
```

## 相对原版 Notepad2 的改动

为使其可用 MinGW-w64 编译，做了以下**最小必要**修改，未改动任何功能逻辑：

| 文件 | 改动 | 原因 |
|---|---|---|
| 9 个源文件 | `_WIN32_WINNT` 0x0500/0x0501 → 0x0601 | 兼容底线从 XP 提升到 Win7 |
| `src/Edit.c` | `FARPROC` → 显式函数指针类型 + 强转 | GCC 16 的 `FARPROC` 为严格无参类型 |
| `src/Helpers.c` | 同上，另 `GetThemedDialogFont` 内联调用改为类型化 | 同上 |
| `src/Helpers.h` | `Toolbar_SetButtons` 参数 `void*` → `LPCTBBUTTON` | 与实现一致，MSVC 容忍而 GCC 不容忍 |
| `src/Dlapi.c` | COM 接口调用加 `(void**)` / `(const ITEMIDLIST**)` 强转 | MinGW 的 `shlobj.h` 参数类型更严格 |
| `src/Print.cpp` | 补 `max`/`min` 宏 | MinGW 的 `minwindef.h` 不提供 |
| 4 个文件 | ISO-8859 非 ASCII 字符 → ASCII | GCC 默认按 UTF-8 读源码，`0xA9` 等字节非法 |
| `src/Notepad2.c` | 新增 `PreviewUpdate` / `PreviewToggle`，`MsgSize` 增加分栏，`SCN_MODIFIED` 防抖，菜单项与 ini 持久化 | notepad2md 功能接入 |
| `src/Notepad2.rc` / `resource.h` | 新增菜单项 `Markdown Preview`、加速键 `Ctrl+Alt+M`、错误字符串 | 同上 |
| `src/Notepad2.rc` | 末尾新增 `LANGUAGE LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED` 中文资源块 | 中文界面 |
| `res/Notepad2.ico` | 换成含 128×128 的多尺寸图标 | 大图标不再模糊 |
| `res/Toolbar.bmp` | 23 → 24 个图标，新增分栏预览图标 | notepad2md 功能 |
| `build/Makefile` | ① windres 规则补上 `res/` 依赖；② `REBARBANDINFO.cbSize` 用兼容值 | 见下 |

### 两个 MinGW 特有的坑（已修）

1. **工具栏完全看不见** —— MinGW 的 `commctrl.h` 在 `NTDDI_VERSION >= 0x06000000` 时会给
   `REBARBANDINFO` 追加 `rcChevronLocation` / `uChevronState`，`sizeof` 变成 128 字节，
   而系统的 comctl32 只认传统的 112 字节布局，于是 `RB_INSERTBAND` **静默失败**（返回 0、
   不插入），rebar 始终没有 band，工具栏宽度为 0。
   修法：向 rebar 报告传统布局大小（`REBARBANDINFO_LEGACY_SIZE`）。

2. **改了图标不生效** —— windres 规则原先只依赖 `.rc` 与 `resource.h`，修改 `res/` 下的
   图标/位图只会重新链接而不重新编译资源，exe 里还是旧图。已把 `res/` 下所有文件列为依赖。

> 注意：用 `-DPREVIEW_DEBUG` / `-DBAR_DEBUG` 做过调试构建后，请先 `make clean`
> 再构建发布版，否则调试版的 `.o` 会被复用（里面带着写日志文件的代码）。

所有改动均可在 git 历史中逐条审查。

## 许可证

本项目采用与原版相同的 BSD 风格许可证，见 [LICENSE](LICENSE)。

第三方组件许可证见 [LICENSE-THIRD-PARTY/](LICENSE-THIRD-PARTY/)：

- Notepad2 —— BSD 风格
- Scintilla —— 类 BSD
- md4c —— MIT

## 致谢

- [Florian Balmer](https://www.flos-freeware.ch/) —— Notepad2 原作者
- [Neil Hodgson](https://www.scintilla.org/) —— Scintilla
- [Martin Mitas](https://github.com/mity) —— md4c
