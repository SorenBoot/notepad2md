# ============================================================================
#  MDPreView — Markdown 编辑 + 实时预览（基于 Notepad2 4.2.25 + Scintilla 2.24）
#  MinGW-w64 构建，产物：单 exe，静态链接，无运行时依赖
#
#  用法（在 build/ 目录下执行）：
#    make           编译 Release x64（默认）
#    make clean     清理构建产物
#    make run       编译并运行
#
#  工具链路径可用 W64DEVKIT 环境变量覆盖，例如：
#    make W64DEVKIT=/d/Other/w64devkit
# ============================================================================

# ---- 工具链 ---------------------------------------------------------------
W64DEVKIT ?= C:/Tools/W64DEVKIT/w64devkit/w64devkit
export PATH := $(W64DEVKIT)/bin:$(PATH)

CC      := gcc
CXX     := g++
RC      := windres
LD      := g++

# ---- 平台与编译选项 --------------------------------------------------------
ARCH        := -m64
# _WIN32_WINNT=0x0601 -> Windows 7 为兼容底线（原版 Notepad2 用 XP，此处提升到 Win7）
WINNT       := -D_WIN32_WINNT=0x0601 -DWINVER=0x0601
# STATIC_BUILD: Scintilla 静态链接进 exe，不产生 SciLexer.dll
# SCI_LEXER:    启用内置 lexer 注册表
DEFINES     := -DSTATIC_BUILD -DSCI_LEXER -D_UNICODE -DUNICODE -D_WINDOWS \
               -D_CRT_SECURE_NO_WARNINGS $(WINNT)

INCLUDES    := -I../scintilla/include -I../scintilla/lexlib -I../scintilla/lexers \
               -I../scintilla/src -I../src -I../md4c -I../res

# -O2 兼顾体积与速度；-fno-ident 去掉编译器标识字符串
OPT         := -O2 -fno-ident
WARN        := -w

# 静态链接 libgcc/libstdc++，确保目标机器零依赖
LDFLAGS     := -m64 -static -static-libgcc -static-libstdc++ -Wl,-subsystem,windows
LDLIBS      := -lcomctl32 -lshlwapi -limm32 -luuid -lole32 -loleaut32 \
               -lversion -lmshtml -lwininet -lgdi32 -lcomdlg32 -luser32 \
               -lshell32 -ladvapi32

OUTDIR      := ../bin
TARGET      := $(OUTDIR)/MDPreView.exe

# ---- 源文件 ---------------------------------------------------------------
# Notepad2 本体
N2_C_SRC   := \
	../src/Notepad2.c \
	../src/Edit.c \
	../src/Dialogs.c \
	../src/Styles.c \
	../src/Dlapi.c \
	../src/Helpers.c
N2_CXX_SRC := ../src/Print.cpp

# Scintilla 核心
SCI_SRC := \
	../scintilla/src/AutoComplete.cxx \
	../scintilla/src/CallTip.cxx \
	../scintilla/src/Catalogue.cxx \
	../scintilla/src/CellBuffer.cxx \
	../scintilla/src/CharClassify.cxx \
	../scintilla/src/ContractionState.cxx \
	../scintilla/src/Decoration.cxx \
	../scintilla/src/Document.cxx \
	../scintilla/src/Editor.cxx \
	../scintilla/src/ExternalLexer.cxx \
	../scintilla/src/Indicator.cxx \
	../scintilla/src/KeyMap.cxx \
	../scintilla/src/LineMarker.cxx \
	../scintilla/src/PerLine.cxx \
	../scintilla/src/PositionCache.cxx \
	../scintilla/src/RESearch.cxx \
	../scintilla/src/RunStyles.cxx \
	../scintilla/src/ScintillaBase.cxx \
	../scintilla/src/Selection.cxx \
	../scintilla/src/Style.cxx \
	../scintilla/src/UniConversion.cxx \
	../scintilla/src/ViewStyle.cxx \
	../scintilla/src/XPM.cxx \
	../scintilla/lexlib/Accessor.cxx \
	../scintilla/lexlib/CharacterSet.cxx \
	../scintilla/lexlib/LexerBase.cxx \
	../scintilla/lexlib/LexerModule.cxx \
	../scintilla/lexlib/LexerSimple.cxx \
	../scintilla/lexlib/PropSetSimple.cxx \
	../scintilla/lexlib/StyleContext.cxx \
	../scintilla/lexlib/WordList.cxx \
	../scintilla/win32/PlatWin.cxx \
	../scintilla/win32/ScintillaWin.cxx

# Scintilla lexers（仅 vcxproj 中列出的 12 个）
SCI_LEXERS := \
	../scintilla/lexers/LexAsm.cxx \
	../scintilla/lexers/LexCPP.cxx \
	../scintilla/lexers/LexCSS.cxx \
	../scintilla/lexers/LexConf.cxx \
	../scintilla/lexers/LexHTML.cxx \
	../scintilla/lexers/LexOthers.cxx \
	../scintilla/lexers/LexPascal.cxx \
	../scintilla/lexers/LexPerl.cxx \
	../scintilla/lexers/LexPowerShell.cxx \
	../scintilla/lexers/LexPython.cxx \
	../scintilla/lexers/LexSQL.cxx \
	../scintilla/lexers/LexVB.cxx

# Markdown 功能模块（新增）
MD_SRC := \
	../src/Markdown.c \
	../src/Preview.c \
	../md4c/md4c.c

C_OBJ   := $(N2_C_SRC:.c=.o) $(MD_SRC:.c=.o)
CXX_OBJ := $(N2_CXX_SRC:.cpp=.o) $(SCI_SRC:.cxx=.o) $(SCI_LEXERS:.cxx=.o)
RES_OBJ := obj/Notepad2_res.o
OBJ     := $(C_OBJ) $(CXX_OBJ) $(RES_OBJ)

# ---- 规则 -----------------------------------------------------------------
.PHONY: all clean run dirs

all: dirs $(TARGET)

dirs:
	@mkdir -p $(OUTDIR) obj

$(TARGET): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "==> $(notdir $@) 构建完成"

%.o: %.c
	$(CC) $(ARCH) $(DEFINES) $(INCLUDES) $(OPT) $(WARN) -c $< -o $@

%.o: %.cxx
	$(CXX) $(ARCH) $(DEFINES) $(INCLUDES) $(OPT) $(WARN) -c $< -o $@

%.o: %.cpp
	$(CXX) $(ARCH) $(DEFINES) $(INCLUDES) $(OPT) $(WARN) -c $< -o $@

$(RES_OBJ): ../src/Notepad2.rc ../src/resource.h
	$(RC) $(DEFINES) -I../res -I../src -o $@ $<

run: all
	./$(TARGET)

clean:
	find ../src ../md4c ../scintilla -name '*.o' -delete
	rm -rf obj $(OUTDIR)
	@echo "==> 清理完成"
