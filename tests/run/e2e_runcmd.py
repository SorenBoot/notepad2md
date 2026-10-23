#!/usr/bin/env python3
"""运行当前文件 — 端到端验收脚本

不依赖桌面截图（RDP 下 BitBlt 全黑）。做法：
  1. 准备测试文件（py / js / bat / csproj / 中文 / 长输出 / 报错）
  2. 设 N2MD_RUNCMD_LOG 让被验程序自己把状态写进日志
  3. 用 Win32 消息驱动 exe：打开文件 → 发 IDM_FILE_RUNCURRENT → 等 done
  4. 断言退出码、行数、截断标志、日志文件内容

用法:  python tests/run/e2e_runcmd.py [--exe PATH]
"""

import argparse
import ctypes
import ctypes.wintypes as wintypes
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

WM_COMMAND = 0x0111
WM_CLOSE = 0x0010
IDM_FILE_RUNCURRENT = 40454
CMD_ESCAPE = 20000      # 加速键 Esc 派发的命令号（Notepad2.rc:511）

RESULTS = []


def record(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    tail = ("  " + detail) if (detail and not ok) else ""
    print(("  PASS  " if ok else "  FAIL  ") + name + tail)


# ---------------------------------------------------------------- 窗口查找

def find_window(pid, timeout=25.0):
    """找该进程的主窗口（有标题、可见、属主线程的顶层窗）。"""
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_proc(hwnd, _):
        wpid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid and user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            if n > 0:
                found.append(hwnd)
                return False
        return True

    deadline = time.time() + timeout
    while time.time() < deadline:
        found.clear()
        user32.EnumWindows(enum_proc, 0)
        if found:
            return found[0]
        time.sleep(0.25)
    return None


def read_clipboard():
    """读 CF_UNICODETEXT，用来断言「复制」按钮真的把日志写进了剪贴板。"""
    u = ctypes.WinDLL("user32")
    k = ctypes.WinDLL("kernel32")
    u.OpenClipboard.argtypes = [ctypes.c_void_p]
    u.GetClipboardData.restype = ctypes.c_void_p
    u.GetClipboardData.argtypes = [ctypes.c_uint]
    k.GlobalLock.restype = ctypes.c_void_p
    k.GlobalLock.argtypes = [ctypes.c_void_p]
    k.GlobalUnlock.argtypes = [ctypes.c_void_p]
    if not u.OpenClipboard(None):
        return ""
    text = ""
    try:
        h = u.GetClipboardData(13)
        if h:
            p = k.GlobalLock(ctypes.c_void_p(h))
            if p:
                text = ctypes.c_wchar_p(p).value or ""
                k.GlobalUnlock(ctypes.c_void_p(h))
    finally:
        u.CloseClipboard()
    return text


def wait_for_log(path, want_tag, timeout=40.0):
    """等日志里出现 want_tag 行并返回它。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    for line in f:
                        if line.startswith(want_tag):
                            return line.strip()
            except OSError:
                pass
        time.sleep(0.15)
    return None


def parse_kv(line):
    return dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)


# ---------------------------------------------------------------- 单次运行

class Runner:
    def __init__(self, exe, workdir):
        self.exe = exe
        self.workdir = workdir
        self.log = os.path.join(workdir, "probe.log")
        self.domfile = os.path.join(workdir, "probe.dom.html")
        self.n = 0
        self.last_dom = None
        self.last_html = ""

    def run_once(self, target, wait_done=True, stop_after=None, timeout=60,
                 esc_after=None, stdin_text=None):
        """启动 exe 打开 target，触发运行，返回 (started_kv, done_kv, status_text)。

        esc_after —— 运行多久后投递 CMD_ESCAPE（等价按 Esc），用于验证停止路径。
        stdin_text —— 非 None 时设 N2MD_RUNCMD_INPUT，由程序自己把文本\n                      从页面底部输入框（#runin → __mdSendInput → MDInput）
                      送进子进程 stdin；空串用 "{{EMPTY}}"（环境变量为空等于删除）。
        """
        self.n += 1
        for f in (self.log, self.domfile):
            if os.path.exists(f):
                os.remove(f)

        env = os.environ.copy()
        env["N2MD_RUNCMD_LOG"] = self.log
        env["N2MD_RUNCMD_DOM"] = self.domfile
        env.pop("N2MD_RUNCMD_INPUT", None)
        if stdin_text is not None:
            env["N2MD_RUNCMD_INPUT"] = stdin_text if stdin_text else "{{EMPTY}}"
        # 关掉「运行前保存」的弹窗路径干扰：文件本来就没有修改，无需保存
        proc = subprocess.Popen([self.exe, target], env=env)
        try:
            hwnd = find_window(proc.pid)
            if not hwnd:
                return None, None, "window not found"

            time.sleep(1.2)  # 等文件加载、预览初始化
            if stop_after is not None:
                time.sleep(stop_after)

            user32.PostMessageW(hwnd, WM_COMMAND, IDM_FILE_RUNCURRENT, 0)

            started = wait_for_log(self.log, "started", timeout=timeout)

            if esc_after is not None:
                time.sleep(esc_after)
                user32.PostMessageW(hwnd, WM_COMMAND, CMD_ESCAPE, 0)

            done = None
            if wait_done:
                done = wait_for_log(self.log, "done", timeout=timeout)
            dom = wait_for_log(self.log, "dom", timeout=6) if wait_done else None
            self.last_dom = parse_kv(dom) if dom else None
            self.last_html = ""
            if os.path.exists(self.domfile):
                try:
                    with open(self.domfile, encoding="utf-8", errors="replace") as f:
                        self.last_html = f.read()
                except OSError:
                    pass
            return (parse_kv(started) if started else None,
                    parse_kv(done) if done else None,
                    None)
        finally:
            try:
                if hwnd:
                    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
                proc.wait(timeout=8)
            except Exception:
                proc.kill()


# ---------------------------------------------------------------- 用例

def prepare(workdir):
    files = {}

    files["hello.py"] = (
        'import sys\n'
        'print("hello from python")\n'
        'print("中文输出测试")\n'
        'print("to stderr", file=sys.stderr)\n'
    )

    files["hello.js"] = 'console.log("hello from node");\nconsole.error("node err");\n'

    files["hello.bat"] = '@echo off\r\necho bat ok\r\nexit /b 0\r\n'

    # 多行输出：以前 MSHTML 会把 HTML 里的 \n 当空白吞掉，三行渲染成一行
    files["multi.bat"] = (
        '@echo off\r\n'
        'echo line one\r\n'
        'echo line two\r\n'
        'echo line three\r\n'
    )

    # 交互读入：set /p 读 stdin（以前 si.hStdInput=NULL，直接失败）
    files["ask.bat"] = (
        '@echo off\r\n'
        'set /p NAME=name? \r\n'
        'echo got=[%NAME%]\r\n'
    )

    # 读 stdin 到 EOF 才收尾：验证「结束输入」能把写端关掉
    files["eof.py"] = (
        'import sys\n'
        'd = sys.stdin.read()\n'
        'print("eof len", len(d))\n'
    )

    # pause 读的是控制台输入缓冲区，不读 stdin
    files["paus.bat"] = (
        '@echo off\r\n'
        'echo before\r\n'
        'pause\r\n'
        'echo after pause\r\n'
    )

    files["fail.py"] = (
        'def boom():\n'
        '    raise ValueError("intentional failure")\n'
        'boom()\n'
    )

    files["many.py"] = (
        'for i in range(12000):\n'
        '    print("line %05d" % i)\n'
    )

    files["slow.py"] = (
        'import time\n'
        'for i in range(120):\n'
        '    print("tick", i, flush=True)\n'
        '    time.sleep(0.2)\n'
    )

    for name, text in files.items():
        path = os.path.join(workdir, name)
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)

    # .csproj 项目
    projdir = os.path.join(workdir, "csdemo")
    os.makedirs(projdir, exist_ok=True)
    with open(os.path.join(projdir, "csdemo.csproj"), "w", encoding="utf-8") as f:
        f.write('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>'
                '<OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework>'
                '</PropertyGroup></Project>')
    with open(os.path.join(projdir, "Program.cs"), "w", encoding="utf-8") as f:
        f.write('class P{static void Main(){System.Console.WriteLine("hello from dotnet");}}')

    # 无映射扩展名
    with open(os.path.join(workdir, "noexec.zzz"), "w", encoding="utf-8") as f:
        f.write("nothing\n")

    return files, projdir


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(
        os.path.dirname(__file__), "..", "..", "bin_new", "notepad2md.exe"))
    ap.add_argument("--only", default=None)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("exe 不存在: " + exe)
        return 2

    workdir = tempfile.mkdtemp(prefix="n2md_run_")
    files, projdir = prepare(workdir)
    r = Runner(exe, workdir)

    only = args.only

    def want(name):
        return only is None or only == name

    # ---- 1. hello.py：正常输出 + stderr + 中文 ----
    if want("py"):
        st, dn, err = r.run_once(os.path.join(workdir, "hello.py"))
        ok = bool(st) and bool(dn) and dn.get("exit") == "0"
        record("1. py 运行成功 exit=0", ok, str(dn))
        if dn:
            record("1b. py 有输出行", int(dn["lines"]) >= 3, "lines=" + dn["lines"])
            record("1c. 未触发截断", dn["trunc"] == "0")
        dx = r.last_dom or {}
        record("1d. 输出进了 #runlog", int(dx.get("runlog", -1)) > 0,
               "runlog=" + dx.get("runlog", "?"))
        record("1e. 三个按钮都在", dx.get("buttons") == "7",
               "buttons=" + str(dx.get("buttons")))
        record("1f. runhead 已渲染", int(dx.get("runhead", -1)) > 0,
               "runhead=" + str(dx.get("runhead")))

        html = r.last_html
        record("1g. 中文不乱码", "中文输出测试" in html,
               "缺: 中文输出测试")
        record("1h. stderr 标红", "run-err" in html and "to stderr" in html)
        record("1i. 命令行回显", "run-cmd" in html)
        record("1j. 完成状态行", "run-ok" in html and "exit=0" in html)
        probe = open(r.log, encoding="utf-8", errors="replace").read()
        record("1l. 滚动锁定按钮可切换", "lock class=on" in probe, probe[-200:])
        record("1m. 复制进剪贴板", "hello from python" in read_clipboard(),
               read_clipboard()[:60])
        record("1k. HTML 已转义", "&lt;" not in html or "<SCRIPT" not in html.upper(),
               "输出里出现了未转义的原始标记")

    # ---- 2. stderr 被捕获（fail.py 的 traceback 走 stderr）----
    if want("fail"):
        st, dn, err = r.run_once(os.path.join(workdir, "fail.py"))
        ok = bool(dn) and dn.get("exit") != "0"
        record("2. 非零退出码被如实上报", ok, "exit=" + (dn or {}).get("exit", "?"))
        if dn:
            record("2b. 失败输出非空", int(dn["lines"]) > 0, "lines=" + dn["lines"])
        html = r.last_html
        record("2c. 失败状态行标红", "run-err" in html and "exit=" in html)
        record("2d. traceback 行高亮", "run-errline" in html,
               "没识别出错误行")

    # ---- 3. node ----
    if want("js"):
        st, dn, err = r.run_once(os.path.join(workdir, "hello.js"))
        ok = bool(dn) and dn.get("exit") == "0" and int(dn.get("lines", 0)) >= 2
        record("3. js 经 node 运行", ok, str(dn))

    # ---- 4. bat ----
    if want("bat"):
        st, dn, err = r.run_once(os.path.join(workdir, "hello.bat"))
        ok = bool(dn) and dn.get("exit") == "0"
        record("4. bat 经 cmd /c 运行", ok, str(dn))

    # ---- 5. dotnet 项目探测 ----
    if want("csproj"):
        st, dn, err = r.run_once(os.path.join(projdir, "Program.cs"), timeout=180)
        ok = bool(st) and bool(dn) and dn.get("exit") == "0"
        record("5. .csproj 探测到并 dotnet run", ok, str(dn))

    # ---- 6. 无映射不崩 ----
    if want("nomap"):
        st, dn, err = r.run_once(os.path.join(workdir, "noexec.zzz"), wait_done=False, timeout=8)
        proc_alive = False
        # 没有 started 是预期；关键是 exe 还活着（没崩）
        st2 = r.run_once(os.path.join(workdir, "hello.py"))
        record("6. 无映射扩展名不崩，后续仍可运行",
               st is None and bool(st2[1]) and st2[1].get("exit") == "0",
               "st=" + str(st) + " next=" + str(st2[1]))

    # ---- 7. 万行输出截断 5000 ----
    if want("trunc"):
        st, dn, err = r.run_once(os.path.join(workdir, "many.py"), timeout=120)
        ok = bool(dn) and dn.get("exit") == "0" and dn.get("trunc") == "1"
        record("7. 12000 行输出被截断", ok, str(dn))
        if dn:
            record("7b. 行数正好 5000", dn["lines"] == "5000", "lines=" + dn["lines"])

    # ---- 8. 运行中 Esc 停止 ----
    if want("stop"):
        # slow.py 跑 24 秒；1.5 秒后发 Esc（CMD_ESCAPE 走 WM_KEYDOWN 不好模拟，
        # 直接发 Esc 键消息不可靠，改用 WM_CLOSE 前先验证 stop 路径：
        # 这里用「运行中再触发」验证单实例语义 + 手动停止）
        st, dn, err = r.run_once(os.path.join(workdir, "slow.py"), esc_after=1.5, timeout=60)
        record("8. 长任务能启动", bool(st), str(st))
        ok = bool(dn) and dn.get("exit") != "0"
        record("8b. Esc 后进程被终止（exit!=0）", ok, "exit=" + (dn or {}).get("exit", "?"))
        if st and dn:
            record("8c. 停止前已有部分输出", int(dn["lines"]) > 0, "lines=" + dn["lines"])

    # ---- 9. 多行输出真的是多行（回归：MSHTML 吞 \n）----
    if want("lines"):
        st, dn, err = r.run_once(os.path.join(workdir, "multi.bat"))
        record("9. 多行 bat 运行成功", bool(dn) and dn.get("exit") == "0", str(dn))
        html = r.last_html
        # 命令行回显 1 行 + 三行输出 + 完成状态 1 行 => 换行应大于等于 3 个
        n_br = html.lower().count("<br")
        record("9b. 每个输出行之间有换行（<br>）", n_br >= 3,
               "<br>=%d  html=%r" % (n_br, html[:200]))
        record("9c. 三行内容没被拼成一行",
               all(html.count(t) == 1 for t in ("line one", "line two", "line three"))
               and "line one line two" not in html,
               "html=%r" % html[:260])
        # 纯文本日志不能混进 HTML 标签（以前存日志会存出 <span class=...>）
        txt = ""
        if os.path.exists(r.domfile):
            try:
                with open(r.domfile + ".txt", encoding="utf-8", errors="replace") as f:
                    txt = f.read()
            except OSError:
                pass
        record("9d. 纯文本日志不含 HTML 标签",
               txt != "" and "<span" not in txt and "<br" not in txt,
               "txt=%r" % txt[:200])
        record("9e. 纯文本日志按行分隔",
               txt.count("\r\n") + txt.count("\n") >= 4,
               "txt=%r" % txt[:200])
        # 输入条真的渲染出来了：input + 发送 + 结束输入 三样都在（bits=7）
        record("9f. 底部输入条已渲染",
               (r.last_dom or {}).get("inputbar") == "7",
               "inputbar=" + str((r.last_dom or {}).get("inputbar")))

    # ---- 10. set /p 交互输入 ----
    if want("input"):
        st, dn, err = r.run_once(os.path.join(workdir, "ask.bat"),
                                 stdin_text="hello world", timeout=60)
        record("10. set /p 交互输入可用",
               bool(dn) and "got=[hello world]" in r.last_html,
               "html=%r" % r.last_html[:260])
        record("10b. 输入行有回显", "run-in" in r.last_html)

    # ---- 11. 中文输入（验证不是拿 ACP 字节当 UTF-8 解）----
    if want("inputcn"):
        st, dn, err = r.run_once(os.path.join(workdir, "ask.bat"),
                                 stdin_text="\u5f20\u4e09", timeout=60)
        record("11. 中文输入不乱码",
               bool(dn) and "got=[\u5f20\u4e09]" in r.last_html,
               "html=%r" % r.last_html[:260])

    # ---- 12. pause 能被回车放行 ----
    if want("pause"):
        st, dn, err = r.run_once(os.path.join(workdir, "paus.bat"),
                                 stdin_text="", timeout=60)
        record("12. pause 收到回车后继续",
               bool(dn) and "after pause" in r.last_html,
               "html=%r" % r.last_html[:300])

    # ---- 13. 「结束输入」发 EOF，读 stdin 的程序能收尾 ----
    if want("eof"):
        st, dn, err = r.run_once(os.path.join(workdir, "eof.py"),
                                 stdin_text="{{EOF}}", timeout=60)
        record("13. 结束输入后读到 EOF 正常退出",
               bool(dn) and dn.get("exit") == "0" and "eof len 0" in r.last_html,
               "html=%r done=%s" % (r.last_html[:200], dn))

    print()
    passed = sum(1 for _, ok, _ in RESULTS if ok)
    failed = len(RESULTS) - passed
    print("E2E: 通过 %d, 失败 %d" % (passed, failed))
    for name, ok, detail in RESULTS:
        if not ok:
            print("  FAILED: %s  %s" % (name, detail))

    shutil.rmtree(workdir, ignore_errors=True)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
