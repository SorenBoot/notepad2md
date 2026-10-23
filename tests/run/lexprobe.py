# -*- coding: utf-8 -*-
"""语言方案识别探针：打开一个文件，回报编辑器实际使用的 Scintilla 词法器 ID。

用法：python lexprobe.py <exe> <file>   → 打印 LEXER=<id>
"""
import ctypes, subprocess, sys, time

u = ctypes.WinDLL('user32')
u.SendMessageW.restype = ctypes.c_ssize_t
u.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)


def main():
    exe, path = sys.argv[1], sys.argv[2]
    p = subprocess.Popen([exe, path])
    lexer = -1
    text_len = 0
    scheme = ['']
    try:
        time.sleep(2.5)                     # 等窗口建好、文件载入完
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

        sci = [0]
        allsci = []

        def cb2(h, l):
            b = ctypes.create_unicode_buffer(128)
            u.GetClassNameW(ctypes.c_void_p(h), b, 128)
            if b.value == 'Scintilla':
                allsci.append(h)
                sci[0] = h
            return True
        u.EnumChildWindows(ctypes.c_void_p(main_wnd[0]), CB(cb2), 0)
        if len(sys.argv) > 3:
            t = ctypes.create_unicode_buffer(256)
            tc = ctypes.create_unicode_buffer(128)
            u.GetWindowTextW(ctypes.c_void_p(main_wnd[0]), t, 256)
            u.GetClassNameW(ctypes.c_void_p(main_wnd[0]), tc, 128)
            sys.stderr.write('main=%x class=%s title=%s scintillas=%d lens=%s\n                ' % (
                main_wnd[0], tc.value, t.value, len(allsci),
                [u.SendMessageW(ctypes.c_void_p(x), 2006, 0, 0) for x in allsci]))

        # 状态栏上的「语言方案」下拉框里就是方案名（JSON / JavaScript / ...）
        scheme = ['']

        def cb3(h, l):
            b = ctypes.create_unicode_buffer(128)
            u.GetClassNameW(ctypes.c_void_p(h), b, 128)
            if b.value == 'ComboBox':
                t = ctypes.create_unicode_buffer(256)
                u.GetWindowTextW(ctypes.c_void_p(h), t, 256)
                if t.value:
                    scheme[0] = t.value
            return True
        u.EnumChildWindows(ctypes.c_void_p(main_wnd[0]), CB(cb3), 0)

        if sci[0]:
            h = ctypes.c_void_p(sci[0])
            text_len = u.SendMessageW(h, 2006, 0, 0)      # SCI_GETLENGTH
            lexer = u.SendMessageW(h, 4002, 0, 0)         # SCI_GETLEXER
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except Exception:
            pass
    print('LEXER=%d LEN=%d SCHEME=%s' % (lexer, text_len, scheme[0]))


main()
