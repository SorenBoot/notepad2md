#!/usr/bin/env python3
"""src/Notepad2.rc 编码分块校验

为什么需要：这个 .rc 不是单一编码，它靠 `#pragma code_page(N)` 切块，
windres 按块内的 code_page 解释字节。往错块里写错编码，编译能过，
但界面文字变成乱码（曾经把 8 行中文按 GBK 写进了 UTF-8 块，
工具栏提示显示成 "���е�ǰ�ļ�"）。

规则：
  65001 (UTF-8)  → 该块每行必须是合法 UTF-8
  1252          → 该块每行必须是纯 ASCII（1252 与 ASCII 在 <0x80 完全重合）
  其它 (如 936)  → 按该代码页严格解码

用法:
  python tests/run/check_rc_encoding.py [--file src/Notepad2.rc]
退出码 0 = 全部合规，1 = 有违规行。
"""

import argparse
import os
import re
import sys

PRAGMA_RE = re.compile(rb'^\s*#pragma\s+code_page\s*\(\s*(\d+)\s*\)')

# windres 认的代码页号 → python 编解码器名
CODECS = {
    65001: "utf-8",
    1252: "cp1252",
    936: "gbk",
    932: "cp932",
    950: "cp950",
    54936: "gb18030",
    20936: "gb2312",
}


def check(path):
    with open(path, "rb") as f:
        raw = f.read()

    bad = []
    cp = 1252           # 微软资源脚本的缺省代码页
    cp_line = 0         # 当前块从哪一行开始

    for lineno, line in enumerate(raw.split(b"\n"), 1):
        m = PRAGMA_RE.match(line)
        if m:
            cp = int(m.group(1))
            cp_line = lineno
            # pragma 行本身必须纯 ASCII，否则连它自己都没法被解释
            if any(b >= 0x80 for b in line):
                bad.append((lineno, cp, "code_page 指令行含非 ASCII 字节"))
            continue

        if not line:
            continue

        codec = CODECS.get(cp)
        if codec is None:
            # 不认识的代码页：至少要能过一遍 latin-1 兜底检查
            # （不能判定，只提示一次，不当作失败）
            codec = "latin-1"

        # 1252 块收紧成「必须纯 ASCII」：cp1252 能解出一堆高位字节，
        # 而 windres 在 1252 块里遇到它们只会吐出乱码，不会报错。
        if cp == 1252:
            hi = [b for b in line if b >= 0x80]
            if hi:
                bad.append((
                    lineno, cp,
                    "1252 块里出现高位字节 %s，必须是纯 ASCII：%r"
                    % (" ".join("%02X" % b for b in hi[:6]), line.strip()[:70])
                ))
            continue

        try:
            line.decode(codec)
        except UnicodeDecodeError as e:
            snippet = line.strip()[:70]
            bad.append((
                lineno, cp,
                "%s 解码失败 (%s) 于第 %d 字节：%r"
                % (codec, e.reason, e.start, snippet)
                + ("" if cp != 1252 else "  ← 1252 块必须是纯 ASCII")
            ))
            continue

        # 65001 额外收紧：合法 UTF-8 里也不该出现落单的代理区/替换字符
        if cp == 65001:
            text = line.decode("utf-8")
            if "\ufffd" in text:
                bad.append((lineno, cp, "含 U+FFFD 替换字符：%r" % text.strip()[:70]))
    return bad, cp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default=None)
    args = ap.parse_args()

    path = args.file
    if not path:
        # 不要求先 cd 到仓库根：默认找 src/Notepad2.rc，找不到就相对本脚本 ../.. 再找一次
        path = "src/Notepad2.rc"
        if not os.path.exists(path):
            here = os.path.dirname(os.path.abspath(__file__))
            path = os.path.join(here, "..", "..", "src", "Notepad2.rc")

    try:
        bad, last_cp = check(path)
    except OSError as e:
        print("读不到 %s: %s" % (path, e))
        return 2

    if not bad:
        print("  PASS  rc 编码分块全部合规（末块 code_page=%d）" % last_cp)
        print("rc 编码: 通过 1, 失败 0")
        return 0

    for lineno, cp, why in bad:
        print("  FAIL  第 %d 行 (code_page=%d): %s" % (lineno, cp, why))
    print("rc 编码: 通过 0, 失败 1（违规 %d 行）" % len(bad))
    return 1


if __name__ == "__main__":
    sys.exit(main())
