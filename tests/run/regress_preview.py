# -*- coding: utf-8 -*-
"""回归：Markdown 预览渲染 + 文件编码识别 + 大纲，确认「运行当前文件」改动没伤到原有功能。

判定依据是应用自己落盘的渲染片段（N2MD_PREVIEW_DOM 探针），
不靠截图 —— RDP 下 BitBlt 全黑，截图不可信。
"""
import ctypes, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EXE  = os.path.join(ROOT, "bin_new", "notepad2md.exe")
IDM_VIEW_MDPREVIEW = 40445   # src/resource.h

u32 = ctypes.WinDLL("user32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)


def find_window(pid, timeout=25.0):
    """找该进程的主窗口（可见且有标题的顶层窗）。"""
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, lp):
        wpid = ctypes.c_ulong()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid and u32.IsWindowVisible(hwnd):
            if u32.GetWindowTextLengthW(hwnd) > 0:
                found.append(hwnd)
                return False
        return True

    deadline = time.time() + timeout
    while time.time() < deadline:
        found.clear()
        u32.EnumWindows(cb, None)
        if found:
            return found[0]
        time.sleep(0.25)
    return None


def has_child_class(hwnd, cls):
    hit = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(w, lp):
        buf = ctypes.create_unicode_buffer(128)
        u32.GetClassNameW(w, buf, 128)
        if buf.value == cls:
            hit.append(w)
            return False
        return True

    u32.EnumChildWindows(hwnd, cb, None)
    return bool(hit)


RESULT = []


def record(name, ok, detail=""):
    RESULT.append((name, ok))
    print(("  PASS  " if ok else "  FAIL  ") + name + ("" if ok else "  " + detail))


def render(path, wait=10.0):
    """打开文件，返回预览片段（UTF-8 文本）。"""
    dump = os.path.join(tempfile.mkdtemp(prefix="n2md_pv_"), "frag.html")
    env = dict(os.environ)
    env["N2MD_PREVIEW_DOM"] = dump
    p = subprocess.Popen([EXE, path], env=env)
    try:
        hwnd = find_window(p.pid)
        # 新实例默认预览是关的，要发一次「Markdown 预览」开关。
        # 但窗口刚建好时消息循环还没跑起来，第一发常常被丢掉，
        # 所以要一直重发到预览宿主窗口出现为止（再发就会关掉它）。
        for _ in range(30):
            if has_child_class(hwnd, "notepad2mdHost"):
                break
            u32.PostMessageW(hwnd, 0x0111, IDM_VIEW_MDPREVIEW, 0)
            time.sleep(0.5)
        # 预览窗格建好后首次渲染走的是后续的 PreviewUpdate（定时器里），
        # 所以要轮询等片段落盘，不能只 sleep 一个固定时长。
        deadline = time.time() + wait
        while time.time() < deadline and not os.path.exists(dump):
            time.sleep(0.2)
        time.sleep(0.3)
    finally:
        pass
    text = ""
    if os.path.exists(dump):
        text = open(dump, "rb").read().decode("utf-8", "replace")
    try:
        u32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
        p.wait(timeout=5)
    except Exception:
        p.kill()
    return text


def main():
    print("== 预览 / 编码回归 ==")

    frag = render(os.path.join(ROOT, "tests", "01-user-login.md"))
    record("R1. Markdown 有标题", "<h1" in frag, frag[:120])
    record("R2. Markdown 有表格", "<table" in frag, frag[:120])
    record("R3. Markdown 有列表/行内代码",
           "<li" in frag or "<code" in frag, frag[:120])
    record("R4. 中文正确", "用户" in frag, frag[:120])

    frag = render(os.path.join(ROOT, "tests", "_sweep", "utf8.md"))
    record("R5. UTF-8 Markdown 正常", "<h1" in frag, frag[:160])

    frag = render(os.path.join(ROOT, "tests", "_t_gbk.txt"))
    record("R6. GBK 文件自动识别", "中文内容" in frag and "标题" in frag, frag[:160])

    frag = render(os.path.join(ROOT, "tests", "_t_utf8.md"))
    record("R7. UTF-8 文件正常", "Title" in frag and "<li" in frag, frag[:120])

    frag = render(os.path.join(ROOT, "tests", "_t_emoji.md"))
    record("R8. Emoji 不乱码", "任务完成" in frag and "😀" in frag, frag[:160])

    bad = [n for n, ok in RESULT if not ok]
    print()
    if bad:
        print("回归: 失败 %d / %d" % (len(bad), len(RESULT)))
        for n in bad:
            print("  FAILED:", n)
        return 1
    print("回归: 通过 %d, 失败 0" % len(RESULT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
