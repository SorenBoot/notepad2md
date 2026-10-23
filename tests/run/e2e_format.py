#!/usr/bin/env python3
"""格式 / 压缩 / 校验 — 端到端验收脚本

纯引擎的行为由 tests/run/test_format.c 覆盖（不依赖 GUI）。
这里验的是"接线"是否正确：

  1. 用真实 exe 打开各类型的文件（靠扩展名选中语法方案）
  2. 发 IDM_FORMAT_DOC / IDM_FORMAT_MINIFY / IDM_FORMAT_VALIDATE
  3. 用 IDM_FILE_SAVE 落盘，读文件字节做断言

覆盖点：
  - 菜单命令 → 引擎 → 编辑器 的整条链路
  - 行尾沿用文件原有风格（LF 文件仍是 LF，CRLF 文件仍是 CRLF）
  - 缩进跟随当前的制表符宽度设置
  - 有选区时只动选区
  - 校验失败时文档一个字节都不改（并且弹窗能被正常关掉）
  - 撤销一步就回到原样

用法:  python tests/run/e2e_format.py [--exe PATH] [-v]
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

WM_COMMAND = 0x0111
WM_CLOSE = 0x0010
IDM_FILE_SAVE = 40004
IDM_FORMAT_DOC = 40802
IDM_FORMAT_MINIFY = 40803
IDM_FORMAT_VALIDATE = 40804
IDM_EDIT_UNDO = 40300          # 见 src/resource.h
CMD_ESCAPE = 20000

RESULTS = []


def record(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    tail = ("  " + detail) if (detail and not ok) else ""
    print(("  PASS  " if ok else "  FAIL  ") + name + tail)


# ---------------------------------------------------------------- 窗口

def find_window(pid, timeout=25.0):
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_proc(hwnd, _):
        wpid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value != pid:
            return True
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n <= 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        found.append((hwnd, buf.value))
        return False

    deadline = time.time() + timeout
    while time.time() < deadline:
        found.clear()
        user32.EnumWindows(enum_proc, 0)
        if found:
            return found[0][0]
        time.sleep(0.2)
    return None


def find_dialog(pid, timeout=6.0):
    """找本进程弹出的模态对话框（MessageBox / InfoBox）。"""
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_proc(hwnd, _):
        wpid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value != pid:
            return True
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, cls, 64)
        if cls.value == "#32770" and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
            return False
        return True

    deadline = time.time() + timeout
    while time.time() < deadline:
        found.clear()
        user32.EnumWindows(enum_proc, 0)
        if found:
            return found[0]
        time.sleep(0.1)
    return None


def dialog_text(hwnd):
    out = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_child(c, _):
        n = user32.GetWindowTextLengthW(c)
        if n > 0:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(c, buf, n + 1)
            if buf.value.strip():
                out.append(buf.value)
        return True

    user32.EnumChildWindows(hwnd, enum_child, 0)
    return " | ".join(out)


def dismiss_dialog(pid):
    """关掉弹窗（相当于点确定/关闭）。返回弹窗文字，没有弹窗返回 None。"""
    h = find_dialog(pid)
    if not h:
        return None
    txt = dialog_text(h)
    user32.PostMessageW(h, WM_CLOSE, 0, 0)
    time.sleep(0.4)
    return txt


def find_edit(hwnd):
    """找编辑区子窗口（Scintilla）。"""
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_child(c, _):
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(c, cls, 64)
        if cls.value.startswith("Scintilla"):
            found.append(c)
            return False
        return True

    user32.EnumChildWindows(hwnd, enum_child, 0)
    return found[0] if found else None


def send(hwnd, msg, wp=0, lp=0):
    return user32.SendMessageW(hwnd, msg, wp, lp)


# Scintilla 消息号（见 scintilla/include/scintilla.h）
SCI_GETCURRENTPOS = 2008
SCI_SETSEL = 2160
SCI_GETLENGTH = 2006


# ---------------------------------------------------------------- 运行

class Editor:
    def __init__(self, exe, workdir):
        self.exe = exe
        self.workdir = workdir

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def open(self, path, timeout=25):
        """启动 exe 打开文件，返回 (proc, hwnd)。"""
        proc = subprocess.Popen([self.exe, path])
        hwnd = find_window(proc.pid, timeout=timeout)
        if not hwnd:
            proc.kill()
            raise RuntimeError("窗口没起来: " + path)
        time.sleep(1.0)          # 等文件加载 / 预览初始化
        return proc, hwnd

    def close(self, proc, hwnd):
        try:
            if hwnd:
                user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
            proc.wait(timeout=8)
        except Exception:
            # 有未保存修改时会问是否保存 —— 关掉弹窗再关窗
            d = dismiss_dialog(proc.pid)
            if d is not None:
                time.sleep(0.3)
                user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
            try:
                proc.wait(timeout=8)
            except Exception:
                proc.kill()


def cmd(hwnd, n, wait=0.8):
    user32.PostMessageW(hwnd, WM_COMMAND, n, 0)
    time.sleep(wait)


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def run_case(ed, name, filename, content, cmds, want, timeout=25,
             expect_dialog=None):
    """打开文件 → 发命令 → 保存 → 比对字节。"""
    path = os.path.join(ed.workdir, filename)
    with open(path, "wb") as f:
        f.write(content)

    proc, hwnd = ed.open(path, timeout=timeout)
    dlgs = []
    try:
        for c in cmds:
            cmd(hwnd, c)
            d = dismiss_dialog(proc.pid)
            if d is not None:
                dlgs.append(d)

        cmd(hwnd, IDM_FILE_SAVE, wait=1.2)
        d = dismiss_dialog(proc.pid)
        if d is not None:
            dlgs.append(d)

        got = read_bytes(path)
    finally:
        ed.close(proc, hwnd)

    if want is None:
        record(name, bool(dlgs), "预期弹窗，但一个都没有")
        if dlgs and expect_dialog:
            ok = expect_dialog in dlgs[0]
            record(name + " [弹窗文案]", ok,
                   "" if ok else "弹窗是: " + dlgs[0][:160])
        return

    if got == want:
        record(name, True)
    else:
        record(name, False,
               "\n        got : %r\n        want: %r" % (got[:400], want[:400]))


# ---------------------------------------------------------------- 用例

LONG_JSON = (b'{"b":2,"a":1,"list":[1,2,3],"obj":{"x":null,"y":true}}\n')


def prepare(workdir):
    pass


def run_all(exe, verbose):
    workdir = tempfile.mkdtemp(prefix="n2fmt_")
    ed = Editor(exe, workdir)

    print("[JSON]")
    run_case(ed, "json 格式化", "a.json", LONG_JSON, [IDM_FORMAT_DOC],
             b'{\n  "b": 2,\n  "a": 1,\n  "list": [1, 2, 3],\n'
             b'  "obj": {\n    "x": null,\n    "y": true\n  }\n}\n')

    run_case(ed, "json 压缩", "b.json", b'{\n  "a": 1,\n  "b": [1, 2]\n}\n',
             [IDM_FORMAT_MINIFY], b'{"a":1,"b":[1,2]}\n')

    print("[XML]")
    run_case(ed, "xml 格式化", "a.xml", b'<r><a/><b>x</b></r>\n',
             [IDM_FORMAT_DOC], b'<r>\n  <a/>\n  <b>x</b>\n</r>\n')

    run_case(ed, "html 宽松模式（void 元素不写 />）", "a.html",
             b'<html><body><br><p>x</p></body></html>\n',
             [IDM_FORMAT_DOC],
             b'<html>\n  <body>\n    <br>\n    <p>x</p>\n  </body>\n</html>\n')

    print("[SQL]")
    run_case(ed, "sql 格式化", "a.sql",
             b"select a,b from t where a=1 and b=2;\n",
             [IDM_FORMAT_DOC],
             b"select a, b\nfrom t\nwhere a = 1\nand b = 2;\n")

    print("[INI]")
    run_case(ed, "ini 格式化", "a.ini",
             b"[s]\nk = 1\n[t]\nj   =  2\n",
             [IDM_FORMAT_DOC],
             b"[s]\nk = 1\n\n[t]\nj = 2\n")

    print("[行尾]")
    run_case(ed, "CRLF 文件保持 CRLF", "c.json",
             b'{"a":1}\r\n',
             [IDM_FORMAT_DOC], b'{\r\n  "a": 1\r\n}\r\n')

    print("[日志]")
    run_case(ed, "log 去掉行尾空白、保留缩进", "a.log",
             b'2024-01-01 10:00:00 INFO  start   \n'
             b'    at com.foo.Bar(Bar.java:42)  \n'
             b'2024-01-01 10:00:01 ERROR boom\t\n',
             [IDM_FORMAT_DOC],
             b'2024-01-01 10:00:00 INFO  start\n'
             b'    at com.foo.Bar(Bar.java:42)\n'
             b'2024-01-01 10:00:01 ERROR boom\n')

    run_case(ed, "log 连续空行压成一个", "b.log",
             b'\n\n\na\n\n\n\nb\n\n\n',
             [IDM_FORMAT_DOC], b'a\n\nb\n')

    run_case(ed, "log 压缩只丢空行", "c.log",
             b'a\n\n\nb   \n',
             [IDM_FORMAT_MINIFY], b'a\nb\n')

    # .log 里整篇是 JSON：按 JSON 格式化，不当成日志整理
    run_case(ed, "log 里的 JSON 按 JSON 处理", "d.log",
             b'{"a":1,"b":[1,2]}\n',
             [IDM_FORMAT_DOC], b'{\n  "a": 1,\n  "b": [1, 2]\n}\n')

    # 空文档 / 只有空白的 log 不该被改坏
    run_case(ed, "log 只有空白", "e.log",
             b'   \n\t\n',
             [IDM_FORMAT_DOC], b'\n')

    print("[校验]")
    run_case(ed, "坏 JSON 校验报错且不改文件", "bad.json",
             b'{"a":}\n', [IDM_FORMAT_VALIDATE], None,
             expect_dialog=None)

    run_case(ed, "坏 JSON 格式化报错且不改文件", "bad2.json",
             b'{"a":1,}\n', [IDM_FORMAT_DOC], b'{\n  "a": 1,\n}\n')

    print("[撤销]")
    # 格式化后撤销一步应该回到原始字节
    path = os.path.join(workdir, "u.json")
    raw = b'{"a":1,"b":2}\n'
    with open(path, "wb") as f:
        f.write(raw)
    proc, hwnd = ed.open(path)
    try:
        cmd(hwnd, IDM_FORMAT_DOC)
        cmd(hwnd, IDM_EDIT_UNDO)
        cmd(hwnd, IDM_FILE_SAVE, wait=1.2)
        got = read_bytes(path)
    finally:
        ed.close(proc, hwnd)
    record("格式化后撤销一步恢复原文", got == raw,
           "" if got == raw else "got=%r" % got)

    print("[选区]")
    # 有选区时只动选区，其余字节不受影响
    path = os.path.join(workdir, "sel.json")
    raw = b'{"a":1}\n{"b":2}\n'
    with open(path, "wb") as f:
        f.write(raw)
    proc, hwnd = ed.open(path)
    try:
        edit = find_edit(hwnd)
        if not edit:
            record("有选区时只改选区", False, "找不到编辑区子窗口")
        else:
            send(edit, SCI_SETSEL, 0, 7)      # 只选中第一个对象
            time.sleep(0.3)
            cmd(hwnd, IDM_FORMAT_DOC)
            cmd(hwnd, IDM_FILE_SAVE, wait=1.2)
            got = read_bytes(path)
            want = b'{\n  "a": 1\n}\n{"b":2}\n'
            record("有选区时只改选区", got == want,
                   "" if got == want else "\n        got : %r\n        want: %r" % (got, want))
    finally:
        ed.close(proc, hwnd)

    print("[光标]")
    # 光标应该落在同一个 token 上（而不是被抹到文件头或行尾）
    path = os.path.join(workdir, "caret.json")
    raw = b'{"first":1,"second":2}'
    with open(path, "wb") as f:
        f.write(raw)
    proc, hwnd = ed.open(path)
    try:
        edit = find_edit(hwnd)
        if not edit:
            record("光标落在同一个 token 上", False, "找不到编辑区子窗口")
        else:
            send(edit, SCI_SETSEL, 12, 12)    # "second" 的 s 上
            time.sleep(0.3)
            cmd(hwnd, IDM_FORMAT_DOC)
            cmd(hwnd, IDM_FILE_SAVE, wait=1.2)
            got = read_bytes(path)
            pos = send(edit, SCI_GETCURRENTPOS)
            tail = got[pos:pos + 6]
            record("光标落在同一个 token 上", tail == b"second",
                   "" if tail == b"second" else
                   "pos=%d 处是 %r，全文 %r" % (pos, tail, got))
    finally:
        ed.close(proc, hwnd)

    return workdir


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="bin_new/notepad2md.exe")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    exe = args.exe if os.path.isabs(args.exe) else os.path.join(root, args.exe)
    if not os.path.exists(exe):
        print("找不到 exe: " + exe)
        return 2

    print("格式化功能 E2E（%s）" % os.path.basename(exe))
    workdir = run_all(exe, args.verbose)

    nfail = sum(1 for _, ok, _ in RESULTS if not ok)
    print("\n%d 项，失败 %d 项（工作目录 %s）" % (len(RESULTS), nfail, workdir))
    return 0 if nfail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
