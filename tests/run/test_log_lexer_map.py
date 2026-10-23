#!/usr/bin/env python3
"""GUI 探针：等文档真正加载后，检查 lexer 与逐字节样式。"""
import argparse
import ctypes
import ctypes.wintypes as W
import os
import subprocess
import sys
import tempfile
import time

u = ctypes.WinDLL("user32", use_last_error=True)
SCI_GETLENGTH, SCI_GETSTYLEAT = 2006, 2010
SCI_GETLEXER = 4002
SCI_COLOURISE = 4003

STYLE_NAMES = {
    0: "默认", 1: "日期", 2: "FATAL", 3: "ERROR", 4: "WARN",
    5: "INFO", 6: "DEBUG", 7: "数字", 8: "字符串", 9: "路径",
    10: "键", 11: "注释", 12: "括号组",
}

ap = argparse.ArgumentParser()
ap.add_argument("--exe", default="bin_new/notepad2md.exe")
args = ap.parse_args()
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = args.exe if os.path.isabs(args.exe) else os.path.join(ROOT, args.exe)
if not os.path.exists(EXE):
    print("找不到 exe: " + EXE)
    sys.exit(2)

RESULTS = []


def find_edit(pid):
    """按已知可用的方式找编辑窗口：先 EnumWindows 找主窗口，再枚举其子窗口。"""
    main = [None]

    @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def cb_main(h, _):
        p = W.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid:
            n = ctypes.create_unicode_buffer(64)
            u.GetClassNameW(h, n, 64)
            if n.value == "notepad2md":
                main[0] = h
        return True

    u.EnumWindows(cb_main, 0)
    if not main[0]:
        return None

    box = []

    @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def cb_kid(c, _):
        n = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(c, n, 64)
        if n.value == "Scintilla":
            box.append(c)
            return False
        return True

    u.EnumChildWindows(main[0], cb_kid, 0)
    return box[0] if box else None


def probe(name, content, expect_lexer):
    p = os.path.join(tempfile.mkdtemp(), name)
    with open(p, "wb") as f:
        f.write(content)
    pr = subprocess.Popen([EXE, p])
    try:
        e = None
        for _ in range(60):
            time.sleep(0.25)
            e = find_edit(pr.pid)
            if e and u.SendMessageW(e, SCI_GETLENGTH, 0, 0) > 0:
                break
        if not e or u.SendMessageW(e, SCI_GETLENGTH, 0, 0) == 0:
            record("%s 文档加载" % name, False, "编辑窗口为空")
            return
        u.SendMessageW(e, SCI_COLOURISE, 0, -1)
        time.sleep(0.3)
        lx = u.SendMessageW(e, SCI_GETLEXER, 0, 0)
        record("%s lexer=%d" % (name, lx), lx == expect_lexer,
               "期望 %d" % expect_lexer)
        if name.endswith(".log"):
            off = 0
            for l in content.decode().split("\n"):
                if not l:
                    break
                st = u.SendMessageW(e, SCI_GETSTYLEAT, off, 0)
                if l[:4].isdigit():
                    record("log 行首是日期色", st == SCE_LOG_DATE,
                           "样式 %d" % st)
                for k, want in (("INFO", 5), ("ERROR", 3),
                                ("DEBUG", 6)):
                    j = l.find(k)
                    if j >= 0:
                        got = u.SendMessageW(e, SCI_GETSTYLEAT, off + j, 0)
                        record("log %s 级别配色" % k, got == want,
                               "样式 %d，期望 %d" % (got, want))
                # 缩进的堆栈行必须保持默认色
                if l.startswith("    "):
                    got = u.SendMessageW(e, SCI_GETSTYLEAT, off + 4, 0)
                    record("log 堆栈缩进不动", got == 0, "样式 %d" % got)
                off += len(l) + 1
    finally:
        try:
            u.PostMessageW(find_edit(pr.pid) or 0, 0, 0, 0)
            pr.wait(3)
        except Exception:
            pr.kill()


def record(name, ok, detail=""):
    RESULTS.append((name, ok))
    print("  %s %s%s" % ("PASS" if ok else "FAIL", name,
                         "" if ok else "  <- " + detail))


print("== 日志词法器接线（GUI 探针） ==")
SCE_LOG_DATE = 1
probe("a.yaml", b"key: 1\n", 48)
probe("a.log", b"2024-01-01 10:00:00 INFO hello\n"
               b"2024-01-01 10:00:01 ERROR boom\n"
               b"2024-01-01 10:00:02 DEBUG x\n"
               b"    at com.foo.Bar(Bar.java:42)\n", 101)
probe("a.rs", b"fn main() {}\n", 3)
probe("a.sql", b"select 1;\n", 7)

nfail = sum(1 for _, ok in RESULTS if not ok)
print("\nGUI 探针: 通过 %d, 失败 %d" % (len(RESULTS) - nfail, nfail))
sys.exit(0 if nfail == 0 else 1)
