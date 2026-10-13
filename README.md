# MDPreView

轻量级 Markdown 编辑 + 实时预览工具。单文件 exe，静态链接，**零运行时依赖**。

基于 [Notepad2 4.2.25](https://www.flos-freeware.ch/)（Florian Balmer）与 Scintilla 2.24，使用 MinGW-w64 构建。

## 特性

- **单 exe，约 2.7 MB**（含静态 C/C++ 运行时），拷贝即用，无需安装
- **零运行时依赖** —— 只依赖 Windows 系统 DLL（Win7 起全部自带）
- **内存占用约 40 MB**（开启预览时；MSHTML 引擎约占 25 MB），空载约 15 MB
- **实时 Markdown 预览** —— 左右分栏，边打边看，`Ctrl+Alt+M` 开关
- Scintilla 编辑引擎 —— 完整保留 Notepad2 的快捷键、语法高亮、查找替换
- 窗口置顶（`Alt+T`）
- 代码折叠、列选择、正则替换、编码转换

## Markdown 预览

- **`Ctrl+Alt+M`** 或菜单 **查看 → Markdown Preview** 开关预览
- 左右各占一半，**打开 `.md` / `.markdown` / `.mdown` / `.mkd` 文件时自动开启**
- 输入后约 120 ms 自动刷新（合并连续按键，不会每敲一个字都重排）
- 支持 GitHub 风格语法：表格、任务列表、删除线、脚注、围栏代码块
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

产物输出到 `bin\MDPreView.exe`。

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
MDPreView/
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
| `src/Notepad2.c` | 新增 `PreviewUpdate` / `PreviewToggle`，`MsgSize` 增加分栏，`SCN_MODIFIED` 防抖，菜单项与 ini 持久化 | MDPreView 功能接入 |
| `src/Notepad2.rc` / `resource.h` | 新增菜单项 `Markdown Preview`、加速键 `Ctrl+Alt+M`、错误字符串 | 同上 |

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
