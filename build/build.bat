@echo off
REM ==========================================================================
REM  notepad2md 编译脚本 — MinGW-w64
REM  双击此文件即可编译，无需安装 make
REM ==========================================================================
setlocal enabledelayedexpansion

REM ---- 工具链路径（如你的 w64devkit 装别处，改这里）----
if "%W64DEVKIT%"=="" set W64DEVKIT=C:\Tools\W64DEVKIT\w64devkit\w64devkit

if not exist "%W64DEVKIT%\bin\gcc.exe" (
    echo [错误] 未找到 gcc.exe，请检查路径：%W64DEVKIT%
    echo        可设置环境变量 W64DEVKIT 指向你的 w64devkit 目录
    pause
    exit /b 1
)

set PATH=%W64DEVKIT%\bin;%PATH%
cd /d "%~dp0"

echo ============================================================
echo   notepad2md Build  ^(MinGW-w64 x64^)
echo ============================================================
echo.

REM ---- 输出目录 ----
if not exist ..\bin mkdir ..\bin
if not exist obj   mkdir obj

REM ---- 编译选项 ----
REM STATIC_BUILD : Scintilla 静态链接
REM SCI_LEXER    : 启用 lexer
REM _WIN32_WINNT=0x0601 : Windows 7 兼容底线
set DEF=-DSTATIC_BUILD -DSCI_LEXER -D_UNICODE -DUNICODE -D_WINDOWS -D_CRT_SECURE_NO_WARNINGS -D_WIN32_WINNT=0x0601 -DWINVER=0x0601
set INC=-I..\scintilla\include -I..\scintilla\lexlib -I..\scintilla\lexers -I..\scintilla\src -I..\src -I..\md4c -I..\res
set OPT=-O2 -fno-ident -w
set LDF=-m64 -static -static-libgcc -static-libstdc++ -Wl,-subsystem,windows
set LIB=-lcomctl32 -lshlwapi -limm32 -luuid -lole32 -loleaut32 -lversion -lmshtml -lwininet -lgdi32 -lcomdlg32 -luser32 -lshell32 -ladvapi32

set OBJS=

echo [1/4] 编译 Notepad2 本体 ...
for %%F in (Notepad2 Edit Dialogs Styles Dlapi Helpers) do (
    gcc %DEF% %INC% %OPT% -c ..\src\%%F.c -o obj\%%F.o || goto :err
    set OBJS=!OBJS! obj\%%F.o
)

echo [2/4] 编译 Markdown / 大纲 / 运行命令 / 格式化模块 ...
for %%F in (Markdown Preview Outline RunCmd Format Format_Json Format_Xml Format_Sql Format_Ini Format_Log FormatCmd UpdateCheck Update) do (
    gcc %DEF% %INC% %OPT% -c ..\src\%%F.c -o obj\%%F.o || goto :err
    set OBJS=!OBJS! obj\%%F.o
)

echo [3/4] 编译 Scintilla ...
REM 注意：Notepad2.c 是 C，Print.cpp 是 C++
g++ %DEF% %INC% %OPT% -c ..\src\Print.cpp -o obj\Print.o || goto :err
set OBJS=!OBJS! obj\Print.o

for %%F in (
    AutoComplete CallTip Catalogue CellBuffer CharClassify ContractionState
    Decoration Document Editor ExternalLexer Indicator KeyMap LineMarker
    PerLine PositionCache RESearch RunStyles ScintillaBase Selection Style
    UniConversion ViewStyle XPM
) do (
    g++ %DEF% %INC% %OPT% -c ..\scintilla\src\%%F.cxx -o obj\Sci_%%F.o || goto :err
    set OBJS=!OBJS! obj\Sci_%%F.o
)

for %%F in (Accessor CharacterSet LexerBase LexerModule LexerSimple PropSetSimple StyleContext WordList) do (
    g++ %DEF% %INC% %OPT% -c ..\scintilla\lexlib\%%F.cxx -o obj\LexLib_%%F.o || goto :err
    set OBJS=!OBJS! obj\LexLib_%%F.o
)

for %%F in (LexAsm LexBash LexCPP LexCSS LexConf LexHTML LexMarkdown LexOthers LexPascal LexPerl LexPowerShell LexPython LexSQL LexVB LexYaml LexLog) do (
    g++ %DEF% %INC% %OPT% -c ..\scintilla\lexers\%%F.cxx -o obj\Lex_%%F.o || goto :err
    set OBJS=!OBJS! obj\Lex_%%F.o
)

g++ %DEF% %INC% %OPT% -c ..\scintilla\win32\PlatWin.cxx     -o obj\PlatWin.o     || goto :err
g++ %DEF% %INC% %OPT% -c ..\scintilla\win32\ScintillaWin.cxx -o obj\ScintillaWin.o || goto :err
set OBJS=!OBJS! obj\PlatWin.o obj\ScintillaWin.o

gcc %DEF% %INC% %OPT% -c ..\md4c\md4c.c -o obj\md4c.o || goto :err
set OBJS=!OBJS! obj\md4c.o

echo [4/4] 编译资源并链接 ...
windres %DEF% -I..\res -I..\src -o obj\Notepad2_res.o ..\src\Notepad2.rc || goto :err
set OBJS=!OBJS! obj\Notepad2_res.o

g++ %LDF% -o ..\bin\notepad2md.exe !OBJS! %LIB% || goto :err

echo.
echo ============================================================
echo   编译成功 ^!^!
echo   输出：..\bin\notepad2md.exe
echo   生成文件大小（字节）：
for %%I in (..\bin\notepad2md.exe) do @echo     %%~zI bytes
echo ============================================================
pause
exit /b 0

:err
echo.
echo [错误] 编译失败，请检查上方输出
pause
exit /b 1
