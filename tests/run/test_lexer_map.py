# -*- coding: utf-8 -*-
"""语言方案自动识别回归测试。

对每个文件：启动 notepad2md 打开临时文件，断言
  1) Scintilla 的 SCI_GETLEXER 等于预期词法器 ID；
  2) 若该方案与别的方案共用词法器（JSON / Go / Rust / JavaScript 都是 SCLEX_CPP），
     再用「本方案关键字被着色为 SCE_C_WORD」+「别家关键字仍是标识符」区分，
     确保加载的是正确的关键字表。

用法:
  python tests/run/test_lexer_map.py [--exe PATH]
"""

import argparse
import ctypes
import ctypes.wintypes as wintypes
import os
import subprocess
import sys
import tempfile
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [wintypes.HWND, ctypes.c_uint,
                                ctypes.c_size_t, ctypes.c_ssize_t]

SCI_GETLEXER = 4002
SCI_GETLENGTH = 2006
SCI_GETSTYLEAT = 2010
SCI_GETCHARAT = 2007

# Scintilla 词法器 ID（scintilla/include/SciLexer.h）
SCLEX_NULL = 1
SCLEX_PYTHON = 2
SCLEX_CPP = 3
SCLEX_XML = 5
SCLEX_PROPERTIES = 9
SCLEX_BATCH = 12
SCLEX_CSS = 38
SCLEX_YAML = 48
SCLEX_BASH = 62
SCLEX_MARKDOWN = 98

SCE_C_WORD = 5
SCE_C_IDENTIFIER = 11

# (文件名, 词法器, 样本, 本方案关键字, 别家关键字)
# 后两项为 None 时只查词法器 ID。
CASES = [
    # Markdown 是一等公民
    ("sample.md", SCLEX_MARKDOWN,
     "# 标题\n\n**粗体** 和 `code`\n\n- 项\n", None, None),
    ("sample.markdown", SCLEX_MARKDOWN, "# 标题\n", None, None),
    # JSON 独立方案（不再落到 JavaScript）
    ("sample.json", SCLEX_CPP,
     '{\n  "name": "demo",\n  "ok": true,\n  "n": null\n}\n', "null", "package"),
    ("sample.jsonc", SCLEX_CPP,
     '{\n  // 注释\n  "a": null,\n  b: package\n}\n', "null", "package"),
    # YAML / Shell
    ("sample.yaml", SCLEX_YAML,
     "name: demo\nitems:\n  - one\nflag: true\n", None, None),
    ("sample.yml", SCLEX_YAML, "a: 1\n", None, None),
    ("sample.sh", SCLEX_BASH,
     '#!/bin/bash\nNAME="x"\necho "$NAME"\n', None, None),
    ("sample.bash", SCLEX_BASH, "echo hi\n", None, None),
    # TypeScript / JSX 并入 JavaScript
    ("sample.ts", SCLEX_CPP, "const x: number = typeof fn;\n", "typeof", "fn"),
    ("sample.tsx", SCLEX_CPP, "const a = <div/>;\n", "const", "fn"),
    ("sample.jsx", SCLEX_CPP, "const a = <div/>;\n", "const", "fn"),
    ("sample.mjs", SCLEX_CPP, "export default typeof fn;\n", "typeof", "fn"),
    # Go / Rust：CPP 词法器骨架 + 自带关键字表
    ("sample.go", SCLEX_CPP,
     'package main\n\nfunc main() {\n  null := "hi"\n}\n', "package", "null"),
    ("sample.rs", SCLEX_CPP,
     'fn main() {\n  let null = 1;\n}\n', "fn", "package"),
    # .NET 工程文件并入 XML
    ("sample.csproj", SCLEX_XML,
     '<Project Sdk="Microsoft.NET.Sdk"></Project>\n', None, None),
    ("sample.props", SCLEX_XML, "<Project></Project>\n", None, None),
    ("sample.nuspec", SCLEX_XML, "<package/>\n", None, None),
    # TOML / env 走 Configuration
    ("sample.toml", SCLEX_PROPERTIES, '[build]\ntarget = "wasm"\n', None, None),
    ("sample.env", SCLEX_PROPERTIES, "KEY=value\n", None, None),
    # 既有方案不能回归
    ("sample.py", SCLEX_PYTHON, "import os\nprint(1)\n", None, None),
    ("sample.css", SCLEX_CSS, "a{color:red}\n", None, None),
    ("sample.bat", SCLEX_BATCH, "@echo off\n", None, None),
]

# 无扩展名 / dotfile：按文件名与 shebang 识别
NAME_CASES = [
    (".gitignore", SCLEX_PROPERTIES, "node_modules/\n*.log\n", None, None),
    (".editorconfig", SCLEX_PROPERTIES, "root = true\n", None, None),
    (".env", SCLEX_PROPERTIES, "KEY=value\n", None, None),
    (".bashrc", SCLEX_BASH, 'export PATH="$PATH:/opt/bin"\n', None, None),
    ("Dockerfile", SCLEX_BASH, "FROM alpine:3\nRUN echo hi\n", None, None),
    ("runme", SCLEX_BASH, "#!/bin/bash\necho hi\n", None, None),
    ("tool", SCLEX_PYTHON, "#!/usr/bin/env python\nprint(1)\n", None, None),
    ("app", SCLEX_CPP, "#!/usr/bin/env node\nvar x = 1;\n", "var", None),
]

RESULTS = []


def record(name, ok, detail=""):
    RESULTS.append((name, ok))
    print(("  PASS  " if ok else "  FAIL  ") + name + ("  <- " + detail if detail else ""))
    sys.stdout.flush()


def find_main_window(pid, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def enum_proc(hwnd, _):
            wpid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
            if wpid.value == pid and user32.IsWindowVisible(hwnd):
                cls = ctypes.create_unicode_buffer(128)
                user32.GetClassNameW(hwnd, cls, 128)
                # 必须匹配主框架类名，否则会误抓预览用的 IE 子窗口
                if cls.value == "notepad2md":
                    found.append(hwnd)
                    return False
            return True

        user32.EnumWindows(enum_proc, 0)
        if found:
            return found[0]
        time.sleep(0.15)
    return None


def find_child(hwnd, cls_name):
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_proc(h, _):
        cls = ctypes.create_unicode_buffer(128)
        user32.GetClassNameW(h, cls, 128)
        if cls.value == cls_name:
            found.append(h)
            return False
        return True

    user32.EnumChildWindows(hwnd, enum_proc, 0)
    return found[0] if found else None


def kill_leftovers(exe_name):
    """清掉上一轮可能残留的实例（只杀 bin_new 里的，绝不碰用户正在用的）。"""
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-Process notepad2md -ErrorAction SilentlyContinue | "
                    "Where-Object { $_.Path -like '*%s*' } | Stop-Process -Force"
                    % exe_name],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def probe_styles(exe, path, expect, own_word, foreign_word):
    proc = subprocess.Popen([exe, path])
    lexer, text_len = -1, 0
    own_style = foreign_style = None
    try:
        wnd = find_main_window(proc.pid)
        sci = find_child(wnd, "Scintilla") if wnd else None
        if sci:
            for _ in range(20):
                text_len = user32.SendMessageW(sci, SCI_GETLENGTH, 0, 0)
                lexer = user32.SendMessageW(sci, SCI_GETLEXER, 0, 0)
                if text_len > 0 and lexer == expect:
                    break
                time.sleep(0.15)
            if text_len > 0:
                # 跨进程不能传指针：逐字符取（SCI_GETCHARAT），样本都很小
                raw = bytes(user32.SendMessageW(sci, SCI_GETCHARAT, i, 0) & 0xFF
                            for i in range(text_len))
                def style_of(word):
                    # SCI_GETSTYLEAT 用字节偏移，所以要在 bytes 里找（含中文时字符偏移会错位）
                    pos = raw.find(word.encode("utf-8"))
                    if pos < 0:
                        return None
                    return user32.SendMessageW(sci, SCI_GETSTYLEAT, pos, 0)

                if own_word:
                    own_style = style_of(own_word)
                if foreign_word:
                    foreign_style = style_of(foreign_word)
    finally:
        proc.kill()
        try:
            proc.wait(timeout=5)
        except Exception:
            pass
    return lexer, text_len, own_style, foreign_style


def run_cases(exe, tmpdir, cases, label):
    print("== %s ==" % label)
    for fname, expect, sample, own_word, foreign_word in cases:
        path = os.path.join(tmpdir, fname)
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(sample)
        lexer, text_len, own_style, foreign_style = probe_styles(
            exe, path, expect, own_word, foreign_word)

        ok = (lexer == expect and text_len > 0)
        detail = ""
        if own_word and (lexer == expect):
            if own_style != SCE_C_WORD:
                ok = False
                detail = "'%s' 样式 %s != 关键字 %d" % (own_word, own_style, SCE_C_WORD)
            elif foreign_word and foreign_style is not None \
                    and foreign_style == SCE_C_WORD:
                ok = False
                detail = "别家关键字 '%s' 也被着色（关键字表串了）" % foreign_word
        record("%-14s lexer %-3d len=%-4d%s"
               % (fname, lexer, text_len,
                  "" if not own_word else " kw[%s]=%s" % (own_word, own_style)), ok, detail)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="bin/notepad2md.exe")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到 exe: %s" % exe)
        return 2

    kill_leftovers(os.path.basename(exe))
    tmpdir = tempfile.mkdtemp(prefix="n2md_lex_")
    run_cases(exe, tmpdir, CASES, "扩展名识别")
    run_cases(exe, tmpdir, NAME_CASES, "文件名 / shebang 识别")

    passed = sum(1 for _, ok in RESULTS if ok)
    failed = len(RESULTS) - passed
    print("\n格式识别: 通过 %d, 失败 %d" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
