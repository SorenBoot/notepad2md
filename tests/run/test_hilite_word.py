#!/usr/bin/env python3
"""同词高亮 E2E：双击选词后，全文相同词被涂上 indicator #2。

不限格式：同一个文件内容在 .txt / .log / .c 下行为应一致。
"""
import argparse
import ctypes
import ctypes.wintypes as W
import os
import subprocess
import sys
import tempfile
import time

u = ctypes.WinDLL("user32", use_last_error=True)
WM_COMMAND, WM_CLOSE = 0x0111, 0x0010
SCI_GETLENGTH = 2006
SCI_SETSEL = 2160
SCI_SETINDICATORCURRENT = 2500
SCI_INDICATORVALUEAT = 2507
SCI_INDICATORSTART = 2508
SCI_INDICATOREND = 2509
SCI_INDICATORALLONFOR = 2506
SCI_SETTARGETSTART = 2141
SCI_SETTARGETEND = 2142
IND = 2

ap = argparse.ArgumentParser()
ap.add_argument("--exe", default="bin/notepad2md.exe")
args = ap.parse_args()
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = args.exe if os.path.isabs(args.exe) else os.path.join(ROOT, args.exe)
RESULTS = []


def record(name, ok, detail=""):
    RESULTS.append((name, ok))
    print("  %s %s%s" % ("PASS" if ok else "FAIL", name,
                         "" if ok else "  <- " + detail))


def find_main(pid, to=30):
    box = []

    @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def cb(h, _):
        p = W.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid:
            n = ctypes.create_unicode_buffer(64)
            u.GetClassNameW(h, n, 64)
            if n.value == "notepad2md":
                box.append(h)
        return True

    end = time.time() + to
    while time.time() < end:
        box.clear()
        u.EnumWindows(cb, 0)
        if box:
            return box[0]
        time.sleep(0.2)
    return None


def find_edit(hw):
    box = []

    @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def cb(c, _):
        n = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(c, n, 64)
        if n.value == "Scintilla":
            box.append(c)
            return False
        return True

    u.EnumChildWindows(hw, cb, 0)
    return box[0] if box else None


def find_dialog(pid, to=3):
    box = []

    @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def cb(h, _):
        p = W.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid:
            n = ctypes.create_unicode_buffer(64)
            u.GetClassNameW(h, n, 64)
            if n.value == "#32770" and u.IsWindowVisible(h):
                box.append(h)
                return False
        return True

    end = time.time() + to
    while time.time() < end:
        box.clear()
        u.EnumWindows(cb, 0)
        if box:
            return box[0]
        time.sleep(0.15)
    return None


def painted_at(e, off, n):
    """[off, off+n) 是否每个字节都被 indicator 涂了"""
    for i in range(off, off + n):
        # SCI_INDICATORVALUEAT(indicator, position)
        if not u.SendMessageW(e, SCI_INDICATORVALUEAT, IND, i, 0):
            return False
    return True


def painted_len(e, off, limit):
    """从 off 起被涂了多长"""
    n = 0
    # SCI_INDICATORVALUEAT(indicator, position)
    while off + n < limit and u.SendMessageW(e, SCI_INDICATORVALUEAT, IND, off + n, 0):
        n += 1
    return n


def run_case(name, content, sel_start, sel_end, word, occurrences):
    p = os.path.join(tempfile.mkdtemp(), name)
    with open(p, "wb") as f:
        f.write(content)
    pr = subprocess.Popen([EXE, p])
    try:
        hw = find_main(pr.pid)
        d = find_dialog(pr.pid, 2)
        if d:
            u.PostMessageW(d, WM_COMMAND, 6, 0)   # IDYES
            time.sleep(0.5)
        e = None
        for _ in range(60):
            time.sleep(0.25)
            e = find_edit(hw)
            if e and u.SendMessageW(e, SCI_GETLENGTH, 0, 0) >= len(content):
                break
        if not e:
            record("%s 文档加载" % name, False, "编辑窗口为空")
            return

        # 打开功能：Ctrl+Alt+W 不可直接发，用菜单命令
        u.PostMessageW(hw, WM_COMMAND, 40450, 0)
        time.sleep(0.6)

        # 模拟双击选词
        u.SendMessageW(e, SCI_SETSEL, sel_start, sel_end)
        time.sleep(1.0)

        cb = len(word)
        good = 0
        for off in occurrences:
            if painted_at(e, off, cb):
                good += 1
        record("%s 同词高亮 %d/%d 处" % (name, good, len(occurrences)),
               good == len(occurrences))

        # 不该涂的地方：选一个不属于该词的位置
        # 找第一个非词字符后的一个非匹配词
        bad = 0
        for off in range(0, len(content) - cb):
            if content[off:off + cb] == word:
                continue
            # 只查那些明显不是词一部分的位置
            if off > 0 and chr(content[off - 1]).isalnum():
                continue
            if painted_len(e, off, len(content)) >= cb and \
               content[off:off + cb] != word:
                bad += 1
                break
        record("%s 不误涂其它词" % name, bad == 0)

        # 取消选区后应该清干净
        u.SendMessageW(e, SCI_SETSEL, 0, 0)
        time.sleep(0.8)
        left = painted_len(e, occurrences[0] if occurrences else 0, len(content))
        record("%s 取消选区后清除" % name, left == 0, "残留 %d 字节" % left)
    finally:
        try:
            u.PostMessageW(hw, WM_CLOSE, 0, 0)
            time.sleep(0.3)
            pr.wait(4)
        except Exception:
            pr.kill()


# 同一个内容、不同扩展名：不限格式
# alpha 出现在偏移 0, 23, 42（注意不是 40：40 是 zeta 里的 a）
DOC = b"alpha beta gamma\ndelta alpha epsilon\nzeta alpha eta\n"
print("== 同词高亮 E2E ==")
run_case("a.txt", DOC, 23, 28, b"alpha", [0, 23, 42])
run_case("b.log", DOC, 23, 28, b"alpha", [0, 23, 42])
run_case("c.c", DOC, 23, 28, b"alpha", [0, 23, 42])

# 半个词：选完整的 alpha(0..5)，只该涂 0 和 15；
# 6 是 alphabet 里的 alpha（不是独立词），不该涂
run_case("d.txt", b"alpha alphabet alpha\n", 0, 5, b"alpha", [0, 15])

nfail = sum(1 for _, ok in RESULTS if not ok)
print("\n同词高亮 E2E: 通过 %d, 失败 %d" % (len(RESULTS) - nfail, nfail))
sys.exit(0 if nfail == 0 else 1)
