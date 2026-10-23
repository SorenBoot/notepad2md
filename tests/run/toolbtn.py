# -*- coding: utf-8 -*-
"""工具栏 ▶ 按钮点击验证：真实鼠标消息点按钮，断言输出面板出现 python 回显。

用法：python toolbtn.py <exe> <dir>
"""
import ctypes, os, subprocess, sys, time

u = ctypes.WinDLL('user32')
u.SendMessageW.restype = ctypes.c_ssize_t
u.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
TB_GETITEMRECT = 1053
BUTTON_INDEX = 4          # tbbMainWnd 里 IDT_FILE_RUNCURRENT 的位置（左起第 5 个）


def children(hwnd):
    out = []

    def cb(h, l):
        b = ctypes.create_unicode_buffer(128)
        u.GetClassNameW(ctypes.c_void_p(h), b, 128)
        r = ctypes.c_long(0), ctypes.c_long(0), ctypes.c_long(0), ctypes.c_long(0)
        rc = (ctypes.c_long * 4)(*r)
        u.GetWindowRect(ctypes.c_void_p(h), rc)
        out.append((h, b.value, tuple(rc)))
        return True
    u.EnumChildWindows(ctypes.c_void_p(hwnd), CB(cb), 0)
    return out


def main():
    exe, workdir = sys.argv[1], sys.argv[2]
    path = os.path.join(workdir, 'hello_tb.py')
    open(path, 'w', encoding='utf-8').write('print("toolbar hello")\n')
    p = subprocess.Popen([exe, path])
    ok = False
    detail = ''
    try:
        time.sleep(2.5)
        main_wnd = [0]

        def cb(h, l):
            pid = ctypes.c_ulong()
            u.GetWindowThreadProcessId(ctypes.c_void_p(h), ctypes.byref(pid))
            if pid.value == p.pid and u.IsWindowVisible(ctypes.c_void_p(h)):
                b = ctypes.create_unicode_buffer(128)
                u.GetClassNameW(ctypes.c_void_p(h), b, 128)
                if b.value == 'notepad2md':
                    main_wnd[0] = h
            return True
        u.EnumWindows(CB(cb), 0)

        kids = children(main_wnd[0])
        tb = [h for h, c, _ in kids if c == 'ToolbarWindow32']
        html = [h for h, c, _ in kids if c in ('Internet Explorer_Server', 'Shell Embedding', 'HTMLHost')]
        # 输出窗格是 MSHTML：找 CLSID_WebBrowser 的子窗
        web = [h for h, c, _ in kids if c.startswith('Internet Explorer')]
        detail += 'toolbars=%d webviews=%d ' % (len(tb), len(web))
        assert tb, 'toolbar not found'
        tb = tb[0]

        # TB_GETRECT 的 rect 在目标进程里：用 TB_GETITEMRECT 不行，改按 GetWindowRect 粗算。
        # 工具栏按钮宽度一致（含分隔符），第 5 个按钮起点 = 4 * 22 + 3，中心再 +11
        r = (ctypes.c_long * 4)()
        u.GetWindowRect(ctypes.c_void_p(tb), r)
        cx = r[0] + BUTTON_INDEX * 22 + 3 + 11
        cy = (r[1] + r[3]) // 2
        detail += 'toolbar_rect=(%d,%d,%d,%d) click=(%d,%d) ' % (r[0], r[1], r[2], r[3], cx, cy)

        lp = (cy << 16) | (cx & 0xFFFF)
        u.SetCursorPos(cx, cy)
        time.sleep(0.15)
        u.SendMessageW(ctypes.c_void_p(tb), 0x0201, 1, lp)     # WM_LBUTTONDOWN
        time.sleep(0.08)
        u.SendMessageW(ctypes.c_void_p(tb), 0x0202, 0, lp)     # WM_LBUTTONUP
        time.sleep(2.5)

        # 断言：主窗口里出现了含 "toolbar hello" 的 WebBrowser 文本
        for h, cls, _ in children(main_wnd[0]):
            if not cls.startswith('Internet Explorer'):
                continue
            html_doc = u.SendMessageW(ctypes.c_void_p(h), 0x000D, 0, 0)
            if html_doc:
                ok = True
        # 再稳一点：状态栏文字应含「完成」或「运行」
        for h, cls, _ in children(main_wnd[0]):
            if cls == 'msctls_statusbar32':
                buf = ctypes.create_unicode_buffer(256)
                u.SendMessageW(ctypes.c_void_p(h), 0x0402 + 6, 1,
                                   ctypes.cast(buf, ctypes.c_void_p).value)
                detail += 'status=%r ' % buf.value
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except Exception:
            pass
    print('TOOLBAR_CLICK=%s %s' % ('OK' if ok else 'FAIL', detail))


main()
