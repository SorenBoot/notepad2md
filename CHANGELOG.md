# 变更记录

本项目沿用 [语义化版本](https://semver.org/lang/zh-CN/)。

## [0.0.2] - 2026-10-24

自 0.0.1 以来新增四项用户可见能力，并修复若干问题。

### 新增

**格式整理（JSON / XML / SQL / INI / TOML / 纯文本）**

- 顶层「格式」菜单：**格式化**（`Ctrl+Alt+F`）、**压缩**（`Ctrl+Alt+Shift+F`）、**校验**（`Ctrl+Alt+V`）
- 支持 JSON / JSONC / JSON5 / JSON Lines、XML / HTML、SQL、INI / TOML / .env、纯文本 / `.log`
- 按当前语法方案自动选择，认不出时按内容嗅探
- **只动空白，不动字节**：不解析成对象树再重新序列化，因此键顺序、数字字面形式
  （`1.0` / `1e3` / `0x1F`）、大整数精度、重复键、Unicode 转义、注释、XML 属性顺序
  与实体写法全部原样保留
- 有选区只处理选区；行尾沿用文件；缩进跟随当前 Tab 宽度；整个动作一个撤销步
- 出错时原文一个字节都不动，弹窗给出「第几行第几列」并把光标移过去
- 超过 8 MB 弹窗确认，16 MB 以上拒绝
- `.log` / 纯文本：去掉行尾空白、连续空行压成一个，保留行首缩进（堆栈靠缩进分层）

**日志语法高亮**

- 新增「Log」语法方案：`.log` / `.logfile` 扩展名，以及无扩展名的
  `messages` / `syslog` / `dmesg` / `kern` / `auth` / `boot.log` 等文件名
- 着色：日期时间（ISO / `T` / 毫秒 / 时区 / 月份缩写 / 缺年份）、四档级别
  （FATAL / ERROR / WARN / INFO / DEBUG，含 `[INFO]`、`inf`、`[INF]`、`(warn)` 等写法）、
  数字与 IP:端口、引号字符串、`key=value` 的键、`//` 与 `#` 注释、`(Foo.java:42)` 位置
- 堆栈缩进原样不动
- 21 MB / 33 万行的日志实测：打开 2.7 s，全文着色 1.0 s

**同词高亮（不限格式）**

- 双击选中一个词，全文相同的词立即高亮（`Ctrl+Alt+W`，默认开）
- 不依赖任何语法方案；只高亮完整独立的词（`alphabet` 里的 `alpha` 不涂）
- 多选 / 矩形选择 / 非完整单词时自动清除

**运行当前文件**

- `F5` 直接运行当前文件，输出进下方输出面板
- 按扩展名选解释器（`.py` / `.js` / `.bat` / `.cmd` / `.ps1` / `.sh` …），
  无扩展名时按 shebang 识别
- 输出面板：彩色输出、错误行点击跳转、长行折行、停靠/浮动/缩放且几何记忆
- 面板变量 `%FILE%` `%DIR%` `%NAME%` `%EXT%` `%LINE%` `%SEL%`，命令历史持久化

**Markdown 大纲**

- 侧栏标题列表，点击跳转，随编辑实时更新

**其它**

- Explorer 右键菜单「用 notepad2md 编辑」（只写 HKCU，可注销）

### 修复

- **`NUMLEXERS` 定长数组截断**：新增语法方案时 `pLexArray[NUMLEXERS]` 会静默挤掉末尾项，
  表现为「某个扩展名突然变成默认语法」。数组改为不定长，并加编译期断言。
- **`SCLEX_LOG` 撞号**：初版与 `SCLEX_FORTH` 同为 52，改为 101。
- **`ColouriseLogDoc` 未调 `StartAt/StartSegment`**：`LexAccessor` 跨调用复用 `startSeg`，
  导致首次 `ColourTo` 写穿文档末尾，Scintilla 断言 `[endStyled < Length()]`，`.log` 无法打开。
- **同词高亮 `SCI_WORDSTARTPOSITION/ENDPOSITION` 语义**：两者都以传入位置自身为基准，
  对选区 `[s,e)` 须用 `WORDENDPOSITION(s)` 求词尾、`WORDSTARTPOSITION(e-1)` 求词首。
- **同词高亮 `SCI_SETINDICATORVALUE`**：`DecorationFillRange` 用「当前值」（默认 0），
  不显式设为 1 时 `SCI_INDICATORFILLRANGE` 静默无效。
- **`.log` 的 ini 迁移**：老用户 ini 里缓存的 `[Default Text] FileNameExtensions` 含 `log`，
  会静默继续走默认方案。现在加载时自动把 `log` 从 Default Text 的扩展列表移除。
- **`build.bat` 源列表过期**：引用不存在的 `Output.c`、缺 `RunCmd.c` 与 3 个 lexer，
  双击编译必然失败。
- **`build.bat` 大小显示**：全角冒号的 UTF-8 字节在 GBK 控制台下与 `%%` 粘成非法双字节
  序列，导致 cmd 报「命令行替换中的路径包含无效的格式」。

### 测试

自动化测试从 5 步扩展到 11 步：

```
rc 编码 / 词法器 61 / 日志词法器 68 / 格式化引擎 357 /
格式识别 30 / 日志接线 18 / 大日志 10 / 同词高亮 18 /
运行命令 37 / 格式化 E2E 17 / 预览回归 8
```

`sh tests/run/run_all.sh bin/notepad2md.exe`

## [0.0.1] - 2026-10-16

首个版本：Notepad2 4.2.25 + Scintilla 2.24 基线，加入 Markdown 实时预览、
无 BOM 编码自动识别（UTF-8 / GBK / Big5 等）、简体中文界面。
