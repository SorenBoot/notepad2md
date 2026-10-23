#!/usr/bin/env python3
"""大 .log 文件的性能与稳定性测试（真实 GUI）。

日志天生容易几十 MB，所以必须验：
  1) 打开不崩、无断言弹窗
  2) 打开耗时可接受
  3) 滚动到不同区域后着色仍然正确（增量着色跨 chunk）
  4) 全文着色（SCI_COLOURISE 0,-1）不崩、不出错样式
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
IDYES = 6
SCI_GETLENGTH, SCI_GETSTYLEAT = 2006, 2010
SCI_GETLEXER, SCI_GETFIRSTVISIBLELINE = 4002, 2152
SCI_LINESCROLL, SCI_COLOURISE, SCI_GOTOPOS = 2168, 4003, 2390
SCI_GETENDSTYLED = 2028

ap = argparse.ArgumentParser()
ap.add_argument("--exe", default="bin/notepad2md.exe")
ap.add_argument("--mb", type=int, default=20)
args = ap.parse_args()

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = args.exe if os.path.isabs(args.exe) else os.path.join(ROOT, args.exe)
if not os.path.exists(EXE):
    print("找不到 exe: " + EXE)
    sys.exit(2)

RESULTS = []


def record(name, ok, detail=""):
    RESULTS.append((name, ok))
    print("  %s %s%s" % ("PASS" if ok else "FAIL", name,
                         "" if ok else "  <- " + detail))


def find_main(pid, to=60):
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


def dialog_text(h):
    o = []

    @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def cb(c, _):
        n = u.GetWindowTextLengthW(c)
        if n:
            b = ctypes.create_unicode_buffer(n + 1)
            u.GetWindowTextW(c, b, n + 1)
            if b.value.strip():
                o.append(b.value)
        return True

    u.EnumChildWindows(h, cb, 0)
    return " | ".join(o)


# ---- 造一个真实感的大日志 ----
LEVELS = ["INFO", "WARN", "ERROR", "DEBUG", "TRACE", "FATAL", "NOTICE"]
print("生成 %.0f MB 测试日志..." % args.mb)
lines = []
size = 0
i = 0
while size < args.mb * 1024 * 1024:
    lv = LEVELS[i % len(LEVELS)]
    if i % 97 == 0:
        line = "    at com.example.svc.Worker.run(Worker.java:%d)" % (i % 500)
    elif i % 13 == 0:
        line = '2024-01-01 10:00:%02d,%03d %-5s [worker-%d] {"id":%d,"ok":true}' % (
            i % 60, i % 1000, lv, i % 8, i)
    else:
        line = "2024-01-01 10:00:%02d %-5s [worker-%d] handled request in %d ms" % (
            i % 60, lv, i % 8, i % 900)
    lines.append(line)
    size += len(line) + 1
    i += 1
doc = ("\n".join(lines) + "\n").encode()
tmp = tempfile.mkdtemp()
path = os.path.join(tmp, "big.log")
with open(path, "wb") as f:
    f.write(doc)
print("实际 %.1f MB, %d 行" % (len(doc) / 1e6, len(lines)))

t0 = time.time()
pr = subprocess.Popen([EXE, path])
hw = find_main(pr.pid, 60)
open_s = time.time() - t0
record("窗口起来（%.1fs）" % open_s, hw is not None and open_s < 30)

# Notepad2 原版行为：超过 Settings2/FileLoadWarningMB（默认 1MB）打开时要确认。
# 这不是错误，按“是”继续即可。
d = find_dialog(pr.pid, 3)
if d:
    record("大文件打开需确认（原版行为，非错误）", True)
    u.PostMessageW(d, WM_COMMAND, IDYES, 0)
    time.sleep(0.5)
    d2 = find_dialog(pr.pid, 1.5)
    if d2:
        u.PostMessageW(d2, WM_CLOSE, 0, 0)
        time.sleep(0.4)
else:
    record("无需确认即打开", True)

# 等文档真正加载完
e = None
loaded = None
for _ in range(200):
    time.sleep(0.25)
    e = find_edit(hw)
    if e:
        n = u.SendMessageW(e, SCI_GETLENGTH, 0, 0)
        if n >= len(doc):
            loaded = time.time() - t0
            break
record("文档全部加载（%.1fs, %d/%d 字节）" % (loaded or -1,
       u.SendMessageW(e, SCI_GETLENGTH, 0, 0) if e else 0, len(doc)),
       loaded is not None and loaded < 60)

# 断言弹窗？
d = find_dialog(pr.pid, 2)
record("无断言/错误弹窗", d is None,
       dialog_text(d)[:120] if d else "")
if d:
    u.PostMessageW(d, WM_CLOSE, 0, 0)
    time.sleep(0.4)

# 滚动到几个位置，检查着色仍正确
for frac, want_level in ((0.0, None), (0.5, None), (0.99, None)):
    pos = int(len(doc) * frac)
    u.SendMessageW(e, SCI_GOTOPOS, pos, 0)
    time.sleep(1.2)
    # 找当前可见区域第一行的样式
    line = u.SendMessageW(e, SCI_GETFIRSTVISIBLELINE, 0, 0)
    # 用 SCI_GOTOPOS 后位置附近取样式
    st = u.SendMessageW(e, SCI_GETSTYLEAT, pos, 0)
    ok = st in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)   # 全部合法样式
    record("滚动到 %d%% 处着色正常（样式 %d）" % (frac * 100, st), ok,
           "异常样式 %d" % st)

# 全文强制着色：这是最容易暴露越界的地方
t1 = time.time()
u.SendMessageW(e, SCI_COLOURISE, 0, -1)
time.sleep(0.5)
# 等着色完成
for _ in range(80):
    time.sleep(0.25)
    if u.SendMessageW(e, SCI_GETENDSTYLED, 0, 0) >= len(doc):
        break
full_s = time.time() - t1
endstyled = u.SendMessageW(e, SCI_GETENDSTYLED, 0, 0)
record("全文着色完成（%.1fs, endStyled=%d/%d）" % (full_s, endstyled, len(doc)),
       endstyled >= len(doc))

d = find_dialog(pr.pid, 2)
record("全文着色后无断言弹窗", d is None,
       dialog_text(d)[:120] if d else "")
if d:
    u.PostMessageW(d, WM_CLOSE, 0, 0)
    time.sleep(0.4)

# 抽查若干位置的样式是否合理（日期/级别/默认）
bad = 0
checked = 0
for frac in (0.05, 0.25, 0.5, 0.75, 0.95):
    pos = int(len(doc) * frac)
    st = u.SendMessageW(e, SCI_GETSTYLEAT, pos, 0)
    checked += 1
    if st not in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
        bad += 1
        print("        异常: frac=%.2f pos=%d 样式=%d 上下文=%r" %
              (frac, pos, st, doc[max(0, pos - 20):pos + 20]))
record("抽查 %d 处样式都合法" % checked, bad == 0, "%d 处异常" % bad)

try:
    u.PostMessageW(hw, WM_CLOSE, 0, 0)
    time.sleep(0.4)
    pr.wait(6)
except Exception:
    pr.kill()

nfail = sum(1 for _, ok in RESULTS if not ok)
print("\n大日志测试: 通过 %d, 失败 %d" % (len(RESULTS) - nfail, nfail))
sys.exit(0 if nfail == 0 else 1)
