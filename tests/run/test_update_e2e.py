"""
检查更新 E2E：用本地假服务器冒充 GitHub API，验证三种弹窗。

为什么不用真网络：限流、离线都会让测试红，而且没法测"404 / 乱码 / 连不上"
这些恰恰最容易写错的分支。

做法：NOTEPAD2MD_UPDATE_URL 环境变量把接口地址指到本地 http.server。
（这个环境变量只覆盖 URL，发版程序本身不带任何后门行为。）

用法：
  python3 tests/run/test_update_e2e.py --exe bin/notepad2md.exe
"""
import argparse
import ctypes
import ctypes.wintypes as wintypes
import http.server
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

user32 = ctypes.windll.user32

WM_COMMAND = 0x0111
WM_CLOSE = 0x0010
IDM_HELP_CHECKUPDATE = 40501

PASS = 0
FAIL = 0


def ok(name, cond, extra=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  PASS  " + name)
    else:
        FAIL += 1
        print("  FAIL  " + name + ("  " + str(extra) if extra else ""))


# ---------------------------------------------------------------- 假 GitHub

class Handler(http.server.BaseHTTPRequestHandler):
    """/latest 走重定向（版本检测），/api 返回 JSON（发布说明）。

    g_latest = (状态码, Location 或正文)
    g_api    = (状态码, 正文)
    """

    def do_GET(self):
        if self.path.startswith("/api"):
            status, body = g_api
        elif self.path.startswith("/latest"):
            status, body = g_latest
        else:
            # 重定向的落点：真实 GitHub 返回的是 HTML 页面，这里给个空 200
            status, body = (200, "<html>release page</html>")

        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(status)
        if status in (301, 302, 303, 307):
            self.send_header("Location", body)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):
        pass


g_latest = (302, "/releases/tag/v0.0.2")
g_api = (200, "{}")


def start_server():
    srv = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv, port


def release_json(tag, body=None):
    d = {
        "url": "https://api.github.com/repos/SorenBoot/notepad2md/releases/1",
        "tag_name": tag,
        "name": tag,
        "html_url": "https://github.com/SorenBoot/notepad2md/releases/tag/" + tag,
        "assets": [
            {
                "name": "notepad2md.exe",
                "browser_download_url": "https://example.invalid/notepad2md.exe",
                "size": 3096446,
            }
        ],
    }
    if body is not None:
        d["body"] = body
    return json.dumps(d, ensure_ascii=False)


# ---------------------------------------------------------------- GUI 探针

EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


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
    return "\n".join(out)


def find_main_window(pid, timeout=15):
    found = []

    @EnumProc
    def enum_proc(hwnd, _):
        wpid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid:
            cls = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(hwnd, cls, 64)
            if cls.value.lower().startswith("notepad2"):
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


def find_dialog(pid, timeout=15):
    found = []

    @EnumProc
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


def run_case(exe, name, latest, api, want_substrings, forbid=(), timeout=20, max_len=0,
             extra_env=None):
    """启动 exe → 点「检查更新」→ 读弹窗文字 → 断言。"""
    global g_latest, g_api
    g_latest = latest
    g_api = api

    tmp = tempfile.mkdtemp(prefix="n2upd_")
    env = dict(os.environ)
    env["NOTEPAD2MD_UPDATE_URL"] = URL + "/latest"
    env["NOTEPAD2MD_UPDATE_NOTES_URL"] = URL + "/api"
    # 别让外面的代理设置干扰用例
    for k in ("HTTPS_PROXY", "HTTP_PROXY", "ALL_PROXY", "https_proxy", "http_proxy",
              "all_proxy", "NO_PROXY", "no_proxy"):
        env.pop(k, None)
    if extra_env:
        env.update(extra_env)

    proc = subprocess.Popen([exe], cwd=tmp, env=env)
    try:
        hwnd = find_main_window(proc.pid)
        if not hwnd:
            ok(name + " [主窗口]", False)
            return
        user32.PostMessageW(hwnd, WM_COMMAND, IDM_HELP_CHECKUPDATE, 0)

        dlg = find_dialog(proc.pid, timeout=timeout)
        if not dlg:
            ok(name + " [弹窗出现]", False)
            return

        time.sleep(0.3)  # 让控件文字填好
        text = dialog_text(dlg)
        user32.PostMessageW(dlg, WM_CLOSE, 0, 0)
        time.sleep(0.3)

        for s in want_substrings:
            ok('%s 含 "%s"' % (name, s), s in text, text.replace(chr(10), " / "))
        for s in forbid:
            ok('%s 不含 "%s"' % (name, s), s not in text, text.replace(chr(10), " / "))
        if max_len:
            # MsgBox 内部缓冲 1024 字符，超了就会截断甚至写坏
            ok("%s 文字长度 <= %d" % (name, max_len), len(text) <= max_len, len(text))
    finally:
        try:
            proc.terminate()
        except Exception:
            pass
        time.sleep(0.5)


def main():
    global URL
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join("bin", "notepad2md.exe"))
    args = ap.parse_args()
    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        print("找不到 " + exe)
        return 1

    srv, port = start_server()
    URL = "http://127.0.0.1:%d" % port
    print("假 GitHub: " + URL)

    def tag(t):
        return (302, "/releases/tag/" + t)

    try:
        # 1. 有新版，带发布说明
        run_case(
            exe, "有新版", tag("v9.9.9"),
            (200, release_json("v9.9.9",
                               chr(10).join([
                                   "## 新增",
                                   "**检查更新** 功能",
                                   "",
                                   "## 修复",
                                   "- `INI` 配色",
                               ]) + chr(10))),
            ["9.9.9", "检查更新 功能", "INI 配色"],
        )

        # 2. 已是最新：API 故意给 500，也不该影响结论
        run_case(exe, "已最新", tag("v0.0.2"), (500, "boom"), ["最新"], forbid=["9.9.9"])

        # 3. 远端比本地旧 —— 不能提示有新版
        run_case(exe, "远端更旧", tag("v0.0.1"), (200, "{}"), ["最新"])

        # 4. 有新版但 API 被限流（403）：版本照报，说明缺失但不崩
        run_case(exe, "API 限流", tag("v9.9.9"),
                 (403, '{"message":"API rate limit exceeded"}'), ["9.9.9"])

        # 5. 有新版但 API 返回乱码
        run_case(exe, "API 乱码", tag("v9.9.9"), (200, "<html>oops</html>"), ["9.9.9"])

        # 6. body 为 null
        run_case(exe, "无发布说明", tag("v9.9.9"),
                 (200, '{"tag_name":"v9.9.9","body":null}'), ["9.9.9"])

        # 7. 发布页 404（仓库没有 Release）
        run_case(exe, "发布页 404", (404, "Not Found"), (200, "{}"), ["GitHub"])

        # 8. 重定向到的地址里没有 /tag/
        run_case(exe, "重定向异常", (302, "/whatever"), (200, "{}"), ["GitHub"])

        # 9. 连接被拒（指向一个没人监听的端口）
        dead = socket.socket()
        dead.bind(("127.0.0.1", 0))
        dead_port = dead.getsockname()[1]
        dead.close()
        saved = URL
        URL = "http://127.0.0.1:%d" % dead_port
        run_case(exe, "连不上", tag("v9.9.9"), (200, "{}"), ["GitHub"])
        URL = saved

        # 10. 中文发布说明 + 转义
        run_case(exe, "中文说明", tag("v9.9.8"),
                 (200, '{"body":"\\u4fee\\u590d\\uff1a\\u65e5\\u5fd7\\u914d\\u8272"}'),
                 ["9.9.8", "修复：日志配色"])

        # 11. 代理连不上 -> 自动兜底直连，仍然能查到
        dead_proxy = socket.socket()
        dead_proxy.bind(("127.0.0.1", 0))
        dead_proxy_port = dead_proxy.getsockname()[1]
        dead_proxy.close()
        run_case(exe, "代理不通兜底直连", tag("v9.9.5"), (200, "{}"), ["9.9.5"],
                 extra_env={"HTTPS_PROXY": "http://127.0.0.1:%d" % dead_proxy_port})

        # 12. NO_PROXY 命中 -> 直接不过代理
        run_case(exe, "NO_PROXY 直连", tag("v9.9.4"), (200, "{}"), ["9.9.4"],
                 extra_env={"HTTPS_PROXY": "http://127.0.0.1:%d" % dead_proxy_port,
                            "NO_PROXY": "127.0.0.1"})

        # 13. 裸 host:port 写法的代理也能识别（同样走兜底）
        run_case(exe, "裸 host:port 代理", tag("v9.9.3"), (200, "{}"), ["9.9.3"],
                 extra_env={"HTTPS_PROXY": "127.0.0.1:%d" % dead_proxy_port})

        # 14. 超长发布说明：必须截断，不能把 1024 字符的缓冲写爆
        long_body = chr(10).join(
            "第%d行 说明文字说明文字说明文字说明文字说明文字说明文字说明文字" % i
            for i in range(200))
        run_case(exe, "超长说明", tag("v9.9.7"),
                 (200, release_json("v9.9.7", long_body)),
                 ["9.9.7"], max_len=1024)
    finally:
        srv.shutdown()

    print("")
    print("检查更新 E2E: 通过 %d, 失败 %d" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
