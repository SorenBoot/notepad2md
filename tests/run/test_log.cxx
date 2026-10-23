/* ==========================================================================
   日志词法器单测（LexLog.cxx）

   断言：日期时间、四档级别（含缩写与 [ ] ( ) < > 包裹）、数字/IP、
   引号字符串、路径、key=value 的键、注释、括号组都落在正确的字符范围。

   编译（在仓库根目录）：
     g++ -O1 -o /tmp/log_test tests/run/test_log.cxx \
         -I tests/run/lexfake -I scintilla/include -I scintilla/lexlib \
         -DSCI_NAMESPACE=
   ========================================================================== */

#include <stdio.h>
#include <string.h>
#include <string>

#include "SciLexer.h"

#include "../../scintilla/lexers/LexLog.cxx"

static int g_pass = 0, g_fail = 0;
static const char *g_case = "";

static void Check(bool ok, const char *what, const std::string &detail = "") {
  if (ok) {
    g_pass++;
  } else {
    g_fail++;
    printf("  FAIL  [%s] %s%s%s\n", g_case, what,
           detail.empty() ? "" : "  <- ", detail.c_str());
  }
}

static Accessor MakeAccessor(const char *pszDoc) {
  Accessor a;
  a.doc = pszDoc;
  a.styles.assign(a.doc.size(), 0);
  a.painted.assign(a.doc.size(), 0);
  return a;
}

/* 跑一遍词法器 */
static void Run(Accessor &a) {
  ColouriseLogDoc(0, (int)a.doc.size(), SCE_LOG_DEFAULT, NULL, a);
}

/* 断言 [from,to] 闭区间都涂成 style */
static void ExpectRange(Accessor &a, size_t from, size_t to, int style,
                        const char *what) {
  for (size_t i = from; i <= to && i < a.styles.size(); i++) {
    if (a.styles[i] != style) {
      char buf[256];
      snprintf(buf, sizeof(buf), "偏移 %u 样式 %d，期望 %d（区间 %u..%u）",
               (unsigned)i, a.styles[i], style, (unsigned)from, (unsigned)to);
      Check(false, what, buf);
      return;
    }
  }
  Check(true, what);
}

/* 整个文档都必须被显式涂过 */
static void ExpectFullyPainted(Accessor &a) {
  size_t first = 0;
  size_t n = a.UnpaintedCount(&first);
  if (n) {
    char buf[160];
    snprintf(buf, sizeof(buf), "偏移 %u 起有 %u 字节没涂色", (unsigned)first,
             (unsigned)n);
    Check(false, "全文无未涂色空档", buf);
  } else {
    Check(true, "全文无未涂色空档");
  }
}

/* 找到子串的起始偏移，找不到返回 -1 */
static int FindOf(const char *pszDoc, const char *pszSub) {
  const char *p = strstr(pszDoc, pszSub);
  return p ? (int)(p - pszDoc) : -1;
}

/* ---------------------------------------------------------------- 用例 */

static void TestDateTime(void) {

  g_case = "日期时间";

  {
    const char *doc = "2024-01-01 10:00:00 INFO 启动\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    ExpectRange(a, 0, 18, SCE_LOG_DATE, "ISO 日期 + 时间");
    ExpectFullyPainted(a);
  }

  {
    const char *doc = "[2024-01-01T10:00:00.123Z] ERROR boom\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    int i = FindOf(doc, "2024");
    ExpectRange(a, (size_t)i, (size_t)i + 22, SCE_LOG_DATE, "ISO T + 毫秒 + Z");
    ExpectFullyPainted(a);
  }

  {
    const char *doc = "2024/01/01 10:00:00,123 WARN 慢\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    ExpectRange(a, 0, 22, SCE_LOG_DATE, "斜杠日期 + 逗号毫秒");
    ExpectFullyPainted(a);
  }

  {
    const char *doc = "Jan 01 10:00:00 INFO 月份缩写\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    ExpectRange(a, 0, 14, SCE_LOG_DATE, "月份缩写 + 时间");
    ExpectFullyPainted(a);
  }

  {
    const char *doc = "01-01 10:00:00.456 INFO 缺年份\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    ExpectRange(a, 0, 17, SCE_LOG_DATE, "缺年份日期 + 毫秒");
    ExpectFullyPainted(a);
  }

  {
    /* 只认得到日期：涂日期，剩下给默认色，不能崩 */
    const char *doc = "2024-01-01 普通文本\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    ExpectRange(a, 0, 9, SCE_LOG_DATE, "只有日期");
    ExpectFullyPainted(a);
  }
}

static void TestLevels(void) {

  g_case = "级别";

  struct { const char *doc; int style; const char *what; } cases[] = {
    { "2024-01-01 10:00:00 INFO  启动\n",   SCE_LOG_LEVEL_INFO,  "INFO" },
    { "2024-01-01 10:00:00 [INFO] 启动\n",  SCE_LOG_LEVEL_INFO,  "[INFO]" },
    { "2024-01-01 10:00:00 inf  启动\n",    SCE_LOG_LEVEL_INFO,  "inf 缩写" },
    { "2024-01-01 10:00:00 [INF] 启动\n",   SCE_LOG_LEVEL_INFO,  "[INF] 缩写" },
    { "2024-01-01 10:00:00 (info) 启动\n",  SCE_LOG_LEVEL_INFO,  "(info)" },
    { "2024-01-01 10:00:00 ERROR 炸\n",     SCE_LOG_LEVEL_ERROR, "ERROR" },
    { "2024-01-01 10:00:00 err  炸\n",      SCE_LOG_LEVEL_ERROR, "err" },
    { "2024-01-01 10:00:00 [ERR] 炸\n",     SCE_LOG_LEVEL_ERROR, "[ERR]" },
    { "2024-01-01 10:00:00 WARN  慢\n",     SCE_LOG_LEVEL_WARN,  "WARN" },
    { "2024-01-01 10:00:00 warning 慢\n",   SCE_LOG_LEVEL_WARN,  "warning" },
    { "2024-01-01 10:00:00 [WRN] 慢\n",     SCE_LOG_LEVEL_WARN,  "[WRN]" },
    { "2024-01-01 10:00:00 FATAL 死\n",     SCE_LOG_LEVEL_FATAL, "FATAL" },
    { "2024-01-01 10:00:00 ftl  死\n",      SCE_LOG_LEVEL_FATAL, "ftl" },
    { "2024-01-01 10:00:00 DEBUG 细节\n",   SCE_LOG_LEVEL_DEBUG, "DEBUG" },
    { "2024-01-01 10:00:00 dbg  细节\n",    SCE_LOG_LEVEL_DEBUG, "dbg" },
    { "2024-01-01 10:00:00 TRACE 细节\n",   SCE_LOG_LEVEL_DEBUG, "TRACE" },
    { "2024-01-01 10:00:00 [trc] 细节\n",   SCE_LOG_LEVEL_DEBUG, "[trc]" },
    { "2024-01-01 10:00:00 NOTICE 提示\n",  SCE_LOG_LEVEL_INFO,  "NOTICE" },
  };

  for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
    const char *doc = cases[k].doc;
    Accessor a = MakeAccessor(doc);
    Run(a);

    /* 级别词一定紧跟日期时间之后，从日期后第一个非空白字符开始找 */
    int i = 20;                                  /* "2024-01-01 10:00:00 " 的长度 */
    while (doc[i] == '[' || doc[i] == '(' || doc[i] == '<' ||
           doc[i] == ' ' || doc[i] == ']' || doc[i] == ')' || doc[i] == '>')
      i++;
    /* 往回退到包裹符起点 */
    int iStart = i;
    while (iStart > 20 &&
           (doc[iStart-1] == '[' || doc[iStart-1] == '(' || doc[iStart-1] == '<'))
      iStart--;

    /* 找到级别词的结束位置 */
    int iEnd = i;
    while (doc[iEnd] && doc[iEnd] != ' ' && doc[iEnd] != '\n' &&
           doc[iEnd] != '\t')
      iEnd++;

    ExpectRange(a, (size_t)iStart, (size_t)iEnd - 1, cases[k].style,
                cases[k].what);
    ExpectFullyPainted(a);
  }
}

static void TestLevelNotMatched(void) {

  g_case = "级别反例";

  /* "informative" 里的 inf 不是独立词，整行都该是默认色 */
  {
    const char *doc = "informative message\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    for (size_t i = 0; i < a.styles.size(); i++) {
      if (a.styles[i] == SCE_LOG_LEVEL_INFO ||
          a.styles[i] == SCE_LOG_LEVEL_DEBUG) {
        Check(false, "informative 不当成 inf");
        return;
      }
    }
    Check(true, "informative 不当成 inf");
  }

  /* 未配对的 '[' 只是普通括号 */
  {
    const char *doc = "2024-01-01 10:00:00 [INFO 启动\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    Check(true, "未配对括号不崩");
  }
}

static void TestInline(void) {

  g_case = "行内";

  {
    const char *doc = "2024-01-01 10:00:00 INFO req id=42 ok\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    int i = FindOf(doc, "id");
    ExpectRange(a, (size_t)i, (size_t)i + 1, SCE_LOG_KEY, "key=value 的键");
    ExpectFullyPainted(a);
  }

  {
    const char *doc = "GET /api/v1/users 200 12.5ms\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    int i = FindOf(doc, "/api");
    ExpectRange(a, (size_t)i, (size_t)i + 12, SCE_LOG_PATH, "路径");
    ExpectFullyPainted(a);
  }

  {
    const char *doc = "msg \"hello world\" done\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    int i = FindOf(doc, "\"hello");
    ExpectRange(a, (size_t)i, (size_t)i + 12, SCE_LOG_STRING, "双引号字符串");
    ExpectFullyPainted(a);
  }

  {
    const char *doc = "connect 10.0.0.1:8080 ok\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    int i = FindOf(doc, "10.0.0.1");
    ExpectRange(a, (size_t)i, (size_t)i + 10, SCE_LOG_NUMBER, "IP:端口");
    ExpectFullyPainted(a);
  }

  {
    const char *doc = "at com.foo.Bar(Bar.java:42)\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    int i = FindOf(doc, "(");
    ExpectRange(a, (size_t)i, (size_t)i + 12, SCE_LOG_CONTEXT, "位置括号组");
    ExpectFullyPainted(a);
  }

  {
    const char *doc = "2024-01-01 10:00:00 INFO x # 备注\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    int i = FindOf(doc, "#");
    ExpectRange(a, (size_t)i, (size_t)strlen(doc) - 2, SCE_LOG_COMMENT, "井号注释");
    ExpectFullyPainted(a);
  }
}

static void TestRobust(void) {

  g_case = "健壮性";

  /* 堆栈：缩进必须保留、不能被涂掉 */
  {
    const char *doc = "java.lang.NullPointerException\n"
                      "    at com.foo.Bar(Bar.java:42)\n"
                      "    at com.foo.Baz(Baz.java:7)\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    ExpectRange(a, 0, 30, SCE_LOG_DEFAULT, "堆栈首行默认色");
    ExpectFullyPainted(a);
  }

  /* 空行与 CRLF */
  {
    const char *doc = "a\r\n\r\nb\r\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    ExpectFullyPainted(a);
  }

  /* 单行无换行结尾 */
  {
    const char *doc = "2024-01-01 10:00:00 INFO no-eol";
    Accessor a = MakeAccessor(doc);
    Run(a);
    ExpectRange(a, 0, 18, SCE_LOG_DATE, "无换行结尾的日期");
    ExpectFullyPainted(a);
  }

  /* 全是空白 */
  {
    const char *doc = "   \n\t\n";
    Accessor a = MakeAccessor(doc);
    Run(a);
    ExpectFullyPainted(a);
  }
}

int main(void) {
  printf(" [日志词法器]\n");
  TestDateTime();
  TestLevels();
  TestLevelNotMatched();
  TestInline();
  TestRobust();
  printf("\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
