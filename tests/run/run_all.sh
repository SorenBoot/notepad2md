#!/bin/sh
# 一条命令跑完全部自动化测试（rc 编码 / 词法器 / 格式化引擎 / 格式识别 /
# 运行命令 / 格式化 E2E / 预览）
#
# 用法：
#   sh tests/run/run_all.sh               # 用 bin_new/notepad2md.exe
#   sh tests/run/run_all.sh bin/notepad2md.exe
#
set -u
cd "$(dirname "$0")/../.." || exit 1
EXE="${1:-bin_new/notepad2md.exe}"
if [ ! -f "$EXE" ]; then
  echo "找不到 $EXE，先构建：cd build && make OUTDIR=../bin_new"
  exit 1
fi

export PATH="/c/Tools/W64DEVKIT/w64devkit/w64devkit/bin:$PATH"
FAIL=0

# 第 0 步：.rc 分块编码（2 秒，纯静态检查，不依赖 exe）。
# 放最前面：编码写错会让所有界面文字变乱码，而编译照样通过。
echo "======== 0/6 rc 分块编码校验 ========"
python3 tests/run/check_rc_encoding.py || FAIL=1

echo "======== 1/6 词法器单元测试（不依赖 GUI） ========"
g++ -O1 -w -o /tmp/lexer_test tests/run/test_lexers.cxx scintilla/lexlib/WordList.cxx \
    -I tests/run/lexfake -I scintilla/include -I scintilla/lexlib || FAIL=1
/tmp/lexer_test || FAIL=1

echo "======== 2/6 格式化引擎单元测试（不依赖 GUI） ========"
gcc -O1 -Wall -Wextra -Wno-unused-parameter -std=c99 \
    -o /tmp/format_test tests/run/test_format.c \
    src/Format.c src/Format_Json.c src/Format_Xml.c src/Format_Sql.c src/Format_Ini.c \
    src/Format_Log.c \
    -Isrc || FAIL=1
/tmp/format_test || FAIL=1

echo "======== 3/6 文件格式识别（GUI 探针） ========"
python3 tests/run/test_lexer_map.py --exe "$EXE" || FAIL=1

echo "======== 4/6 运行当前文件 E2E ========"
python3 tests/run/e2e_runcmd.py --exe "$EXE" || FAIL=1

echo "======== 5/6 格式化 / 压缩 / 校验 E2E ========"
python3 tests/run/e2e_format.py --exe "$EXE" || FAIL=1

echo "======== 6/6 预览回归 ========"
python3 tests/run/regress_preview.py --exe "$EXE" || FAIL=1

echo
if [ "$FAIL" -eq 0 ]; then
  echo "全部测试通过"
else
  echo "有用例失败，见上方输出"
fi
exit "$FAIL"
