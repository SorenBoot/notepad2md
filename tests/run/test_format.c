/* ==========================================================================
   格式化引擎（Format.c / Format_Json.c / Format_Xml.c / Format_Sql.c /
   Format_Ini.c）的单元测试。

   不依赖 Win32、不依赖 exe，直接把生产代码编进来测。核心是三条不变量：

     1) 幂等        format(format(x)) == format(x)
     2) 语义零损失  minify(format(x)) == minify(x)
     3) token 原样  去掉所有空白字节后，输出与输入完全一致
                    （格式化只允许搬动空白，一个 token 都不许改）

   第 3 条是最狠的一条：它同时保证键顺序、数字字面形式（1.0 / 1e3 / 0x1F）、
   重复键、Unicode 转义、注释、XML 属性顺序与实体写法全都没被动过。
   ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Format.h"

static int g_pass = 0;
static int g_fail = 0;
static const char *g_case = "";

/* ------------------------------------------------------------------ 框架 */

static void fail(const char *what)
{
  g_fail++;
  printf("  FAIL [%s] %s\n", g_case, what);
}

static void ok(void)
{
  g_pass++;
}

/* 把 buf 打印成可见形式（便于看空白）*/
static void dump(const char *p, int cb)
{
  int i;
  printf("        |");
  for (i = 0; i < cb; i++) {
    unsigned char c = (unsigned char)p[i];
    if (c == '\r') printf("\\r");
    else if (c == '\n') { printf("\\n\n        |"); }
    else if (c == '\t') printf("\\t");
    else putchar(c);
  }
  printf("|\n");
}

/* 去掉所有 ASCII 空白后的副本 */
static char *strip_ws(const char *p, int cb, int *pcbOut)
{
  char *pOut = (char *)malloc((size_t)cb + 1);
  int i, n = 0;
  for (i = 0; i < cb; i++) {
    char c = p[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
        c == '\v' || c == '\f')
      continue;
    pOut[n++] = c;
  }
  pOut[n] = '\0';
  if (pcbOut) *pcbOut = n;
  return pOut;
}

static void check_equal(const char *pGot, int cbGot,
                        const char *pWant, int cbWant, const char *what)
{
  if (cbGot != cbWant || memcmp(pGot, pWant, (size_t)cbWant) != 0) {
    fail(what);
    printf("        got  (%d):", cbGot); dump(pGot, cbGot);
    printf("        want (%d):", cbWant); dump(pWant, cbWant);
  } else
    ok();
}

/* ------------------------------------------------------------------ 运行器 */

typedef struct {
  int  iKind;
  const char *pszName;
  const char *pIn;
  int  cbIn;      /* 0 = 用 strlen(pszIn) */
  const char *pszWant;      /* 期望的格式化结果；NULL 表示不检查具体形状 */
  const char *pszMinified;  /* 期望的压缩结果；NULL 表示不检查 */
} CASE;

/* 三种检查跑一遍。pszWant 非空时还比对精确输出 */
static void run_case(const CASE *c)
{
  FMTSTYLE style;
  FMTRESULT f1, f2, m1, m2;
  FMTMAP map;
  FMTERR err;
  char *pA, *pB;
  int cbA, cbB;
  int iKind = c->iKind;
  int cbIn  = c->cbIn;

  style.szEol   = "\n";
  style.iIndent = 2;

  if (cbIn <= 0)
    cbIn = (int)strlen(c->pIn);

  g_case = c->pszName;

  memset(&map, 0, sizeof(map));
  err.iCode = FMT_E_OK;
  err.iPos  = 0;

  if (!Fmt_Format(iKind, c->pIn, cbIn, &style, &f1, &map, &err)) {
    fail("format 失败");
    printf("        code=%s pos=%d\n", Fmt_ErrName(err.iCode), err.iPos);
    return;
  }
  ok();

  /* 精确形状 */
  if (c->pszWant)
    check_equal(f1.p, f1.cb, c->pszWant, (int)strlen(c->pszWant), "格式化形状不符");

  /* 幂等 */
  {
    FMTSTYLE s2 = style;
    if (!Fmt_Format(iKind, f1.p, f1.cb, &s2, &f2, NULL, NULL)) {
      fail("format(format(x)) 失败");
      Fmt_ResultFree(&f1);
      return;
    }
    check_equal(f2.p, f2.cb, f1.p, f1.cb, "不幂等");
    Fmt_ResultFree(&f2);
  }

  /* token 原样：剥掉空白后必须逐字节相同 */
  pA = strip_ws(c->pIn, cbIn, &cbA);
  pB = strip_ws(f1.p, f1.cb, &cbB);
  check_equal(pB, cbB, pA, cbA, "token 被改动了（去掉空白后不一致）");
  free(pA);
  free(pB);

  /* 语义零损失 */
  if (!Fmt_Minify(iKind, c->pIn, cbIn, &style, &m1, NULL, NULL)) {
    fail("minify(x) 失败");
    Fmt_ResultFree(&f1);
    return;
  }
  if (!Fmt_Minify(iKind, f1.p, f1.cb, &style, &m2, NULL, NULL)) {
    fail("minify(format(x)) 失败");
    Fmt_ResultFree(&f1);
    Fmt_ResultFree(&m1);
    return;
  }
  check_equal(m2.p, m2.cb, m1.p, m1.cb, "minify(format(x)) != minify(x)");

  if (c->pszMinified)
    check_equal(m1.p, m1.cb, c->pszMinified, (int)strlen(c->pszMinified),
                "压缩结果不符");

  /* 压缩结果同样不许动 token */
  pA = strip_ws(c->pIn, cbIn, &cbA);
  pB = strip_ws(m1.p, m1.cb, &cbB);
  check_equal(pB, cbB, pA, cbA, "压缩时 token 被改动了");
  free(pA);
  free(pB);

  Fmt_ResultFree(&m1);
  Fmt_ResultFree(&m2);
  Fmt_ResultFree(&f1);
}

/* 期望失败的用例 */
static void run_bad(int iKind, const char *pszName, const char *pszIn,
                    int iWantCode)
{
  FMTSTYLE style;
  FMTRESULT res;
  FMTERR err;

  g_case = pszName;

  style.szEol   = "\n";
  style.iIndent = 2;

  err.iCode = FMT_E_OK;
  err.iPos  = 0;

  if (Fmt_Format(iKind, pszIn, (int)strlen(pszIn), &style, &res, NULL, &err)) {
    fail("本该失败却成功了");
    printf("        out:"); dump(res.p, res.cb);
    Fmt_ResultFree(&res);
    return;
  }
  ok();

  if (iWantCode != 0 && err.iCode != iWantCode) {
    fail("错误码不对");
    printf("        got %s, want %s\n", Fmt_ErrName(err.iCode),
           Fmt_ErrName(iWantCode));
  } else
    ok();

  if (res.p != NULL) {
    fail("失败时不该有输出");
    Fmt_ResultFree(&res);
  } else
    ok();
}

/* ------------------------------------------------------------------ JSON */

static void TestJson(void)
{
  static const CASE cases[] = {
    { FMT_KIND_JSON, "json-对象",
      "{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":null}}",
      0,
      "{\n  \"a\": 1,\n  \"b\": [1, 2, 3],\n  \"c\": {\n    \"d\": null\n  }\n}",
      "{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":null}}" },

    { FMT_KIND_JSON, "json-中文与转义",
      "{\"名字\":\"测试\",\"esc\":\"a\\\"b\\u4e2d\"}",
      0,
      "{\n  \"名字\": \"测试\",\n  \"esc\": \"a\\\"b\\u4e2d\"\n}",
      NULL },

    { FMT_KIND_JSON, "json-数字字面形式原样",
      "{\"a\":1.0,\"b\":1e3,\"c\":0x1F,\"d\":.5,\"e\":+1,\"f\":-0}",
      0,
      NULL, NULL },

    { FMT_KIND_JSON, "json-重复键保留",
      "{\"k\":1,\"k\":2,\"k\":3}",
      0, NULL, NULL },

    { FMT_KIND_JSON, "json-空容器",
      "{\"a\":{},\"b\":[]}",
      0,
      "{\n  \"a\": {},\n  \"b\": []\n}",
      NULL },

    { FMT_KIND_JSON, "jsonc-注释",
      "{\n// 行注释\n\"a\":1, /* 块注释 */ \"b\":2,\n}",
      0, NULL, NULL },

    { FMT_KIND_JSON, "json5-单引号无引号键尾随逗号",
      "{a:'x',b:[1,2,],}",
      0, NULL, NULL },

    { FMT_KIND_JSON, "jsonl-每行一个对象",
      "{\"a\":1}\n{\"a\":2}",
      0, "{\n  \"a\": 1\n}\n{\n  \"a\": 2\n}", NULL },

    { FMT_KIND_JSON, "json-顶层数组",
      "[1,2,3]",
      0, "[1, 2, 3]", "[1,2,3]" },

    { FMT_KIND_JSON, "json-长数组竖排",
      "[{\"a\":1},{\"b\":2}]",
      0,
      "[\n  {\n    \"a\": 1\n  },\n  {\n    \"b\": 2\n  }\n]", NULL },
  };

  int i;
  for (i = 0; i < (int)(sizeof(cases)/sizeof(cases[0])); i++)
    run_case(&cases[i]);

  run_bad(FMT_KIND_JSON, "json-缺值", "{\"a\":}", FMT_E_EXPECTED_VALUE);
  run_bad(FMT_KIND_JSON, "json-缺冒号", "{\"a\" 1}", FMT_E_EXPECTED_COLON);
  run_bad(FMT_KIND_JSON, "json-尾随垃圾", "{\"a\":1} x", FMT_E_TRAILING_DATA);
  run_bad(FMT_KIND_JSON, "json-字符串未闭合", "{\"a\":\"x}",
          FMT_E_UNTERMINATED_STRING);
  run_bad(FMT_KIND_JSON, "json-括号不配对", "{\"a\":[1,2}", 0);
  run_bad(FMT_KIND_JSON, "json-块注释未闭合", "{\"a\":1}/*", FMT_E_UNTERMINATED_COMMENT);
}

/* ------------------------------------------------------------------ XML */

static void TestXml(void)
{
  static const CASE cases[] = {
    { FMT_KIND_XML, "xml-嵌套元素",
      "<a><b><c/></b></a>",
      0,
      "<a>\n  <b>\n    <c/>\n  </b>\n</a>",
      "<a><b><c/></b></a>" },

    { FMT_KIND_XML, "xml-属性与单引号原样",
      "<a  x=\"1\"   y='2'>t</a>",
      0,
      "<a x=\"1\" y='2'>t</a>", NULL },

    { FMT_KIND_XML, "xml-混合内容整体不拆",
      "<p>前 <b>粗</b> 后</p>",
      0, "<p>前 <b>粗</b> 后</p>", NULL },

    { FMT_KIND_XML, "xml-注释原样",
      "<r><!-- 注释 --><a/></r>",
      0, "<r>\n  <!-- 注释 -->\n  <a/>\n</r>", NULL },

    { FMT_KIND_XML, "xml-CDATA原样",
      "<r><d><![CDATA[ a < b ]]></d></r>",
      0, "<r>\n  <d><![CDATA[ a < b ]]></d>\n</r>", NULL },

    { FMT_KIND_XML, "xml-pre原样",
      "<r><pre>  保留\n    缩进   </pre></r>",
      0, "<r>\n  <pre>  保留\n    缩进   </pre>\n</r>", NULL },

    { FMT_KIND_XML, "xml-space-preserve",
      "<r><d xml:space=\"preserve\">  a  b  </d></r>",
      0, NULL, NULL },

    { FMT_KIND_XML, "xml-实体原样",
      "<a>&lt;&amp;&#65;</a>",
      0, "<a>&lt;&amp;&#65;</a>", NULL },

    { FMT_KIND_XML, "xml-声明",
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<a/>",
      0,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<a/>", NULL },

    { FMT_KIND_XML, "xml-中文",
      "<根><子>文本</子></根>",
      0, "<根>\n  <子>文本</子>\n</根>", NULL },

    { FMT_KIND_HTML, "html-void元素不带斜杠",
      "<div><br><img src=\"x\"><meta charset=\"utf-8\"></div>",
      0, NULL, NULL },

    { FMT_KIND_HTML, "html-正文里的小于号",
      "<html><body><p>a &lt; b</p></body></html>",
      0, NULL, NULL },
  };

  int i;
  for (i = 0; i < (int)(sizeof(cases)/sizeof(cases[0])); i++)
    run_case(&cases[i]);

  run_bad(FMT_KIND_XML, "xml-标签不配对", "<a><b></a>", FMT_E_TAG_MISMATCH);
  run_bad(FMT_KIND_XML, "xml-标签没闭合", "<a><b>", FMT_E_UNTERMINATED_TAG);
  run_bad(FMT_KIND_XML, "xml-只有空白", "   \n  ", FMT_E_EMPTY);
}

/* ------------------------------------------------------------------ SQL */

static void TestSql(void)
{
  static const CASE cases[] = {
    { FMT_KIND_SQL, "sql-子句断行",
      "select a,b from t where a=1 and b=2 order by a desc limit 10;",
      0,
      "select a, b\nfrom t\nwhere a = 1\nand b = 2\norder by a desc\nlimit 10;",
      NULL },

    { FMT_KIND_SQL, "sql-字符串里的引号与分号",
      "select 'it''s; ok', \"a b\" from t;",
      0, NULL, NULL },

    { FMT_KIND_SQL, "sql-行注释强制换行",
      "select 1; -- 注释\nselect 2;",
      0, NULL, NULL },

    { FMT_KIND_SQL, "sql-块注释原样",
      "select /* a  b */ 1;",
      0, NULL, NULL },

    { FMT_KIND_SQL, "sql-子查询展开",
      "select * from (select id from t where x=1) s;",
      0,
      "select *\nfrom (\n  select id\n  from t\n  where x = 1\n) s;",
      NULL },

    { FMT_KIND_SQL, "sql-普通括号不拆行",
      "select * from t where a in (1,2,3) and f(x,y)>0;",
      0, NULL, NULL },

    { FMT_KIND_SQL, "sql-点号两边不加空格",
      "select s.a,t.b from s left join t on s.id=t.id;",
      0, NULL, NULL },

    { FMT_KIND_SQL, "sql-美元引用",
      "create function f() returns int as $$ select 1; $$ language sql;",
      0, NULL, NULL },

    { FMT_KIND_SQL, "sql-反引号与方括号",
      "select `a b`, [c d] from t;",
      0, NULL, NULL },

    { FMT_KIND_SQL, "sql-关键字大小写不动",
      "SELECT A FROM T WHERE B=1;",
      0,
      "SELECT A\nFROM T\nWHERE B = 1;", NULL },

    { FMT_KIND_SQL, "sql-2024标准多语句",
      "insert into t(a,b) values(1,'x');\nupdate t set a=2 where b='y';",
      0, NULL, NULL },
  };

  int i;
  for (i = 0; i < (int)(sizeof(cases)/sizeof(cases[0])); i++)
    run_case(&cases[i]);

  run_bad(FMT_KIND_SQL, "sql-字符串未闭合", "select 'abc", FMT_E_UNTERMINATED_STRING);
  run_bad(FMT_KIND_SQL, "sql-块注释未闭合", "select 1 /* x", FMT_E_UNTERMINATED_COMMENT);
  run_bad(FMT_KIND_SQL, "sql-括号不配对", "select (1;", FMT_E_UNBALANCED);
}

/* ------------------------------------------------------------------ INI */

static void TestIni(void)
{
  static const CASE cases[] = {
    { FMT_KIND_INI, "ini-紧凑风格保持紧凑",
      "[a]\nk=1\nj = 2  \n",
      0,
      "[a]\nk=1\nj=2\n", NULL },

    { FMT_KIND_INI, "ini-带空格风格保持带空格",
      "[a]\nk = 1\nj   =   2\n",
      0,
      "[a]\nk = 1\nj = 2\n", NULL },

    { FMT_KIND_INI, "ini-段间空行",
      "[a]\nk=1\n[b]\nj=2\n",
      0,
      "[a]\nk=1\n\n[b]\nj=2\n", NULL },

    { FMT_KIND_INI, "ini-值里的等号与井号",
      "[a]\nurl=http://x/?a=1#f\npwd=\"a=b#c\"\n",
      0, NULL, NULL },

    { FMT_KIND_INI, "ini-注释原样",
      "; 注释\n# 也注释\n[a]\nk=1\n",
      0, NULL, NULL },

    { FMT_KIND_INI, "toml-多行数组原样",
      "a = [\n  \"x\",\n  \"y\",\n]\nb = 1\n",
      0, NULL, NULL },

    { FMT_KIND_INI, "toml-多行字符串原样",
      "a = \"\"\"\n多行\n  文本\n\"\"\"\nb = 1\n",
      0, NULL, NULL },

    { FMT_KIND_INI, "env-全紧凑不加空格",
      "A=1\nB=2\nC=3\n",
      0, "A=1\nB=2\nC=3\n", "A=1\nB=2\nC=3\n" },
  };

  int i;
  for (i = 0; i < (int)(sizeof(cases)/sizeof(cases[0])); i++)
    run_case(&cases[i]);

  run_bad(FMT_KIND_INI, "ini-空文档", "", FMT_E_EMPTY);
}

/* ------------------------------------------------------------------ 识别 */

static void TestDetect(void)
{
  static const struct {
    const char *pszIn;
    int iKind;
  } cases[] = {
    { "{\"a\": 1}",                    FMT_KIND_JSON },
    { "[1,2,3]",                       FMT_KIND_JSON },
    { "<?xml version=\"1.0\"?><a/>",   FMT_KIND_XML },
    { "<root><a/></root>",             FMT_KIND_XML },
    { "select 1;",                     FMT_KIND_SQL },
    { "  -- 注释\n  SELECT 1",         FMT_KIND_SQL },
    { "[sec]\nk=v\n",                  FMT_KIND_INI },
    { "KEY=value\nOTHER=1\n",          FMT_KIND_INI },
    { "hello world",                   FMT_KIND_NONE },
    { "",                              FMT_KIND_NONE },

    /* 反例：这些不是我们要的格式，认错了就会把源码重排。
       尤其是 .c / .bat 这类“未支持语言”，只要内容长得像就会被猜中。 */
    { "{\n  int x = 1;\n  int y = 2;\n}\n", FMT_KIND_NONE },   /* C 裸块 */
    { "{\n  不配对的\n}\n",          FMT_KIND_NONE },   /* 散文的花括号 */
    { "set A=1\nset B=2\n",           FMT_KIND_NONE },   /* 批处理 */
    { "int main(void)\n{\n  return 0;\n}\n", FMT_KIND_NONE },
    { "const cfg = {\n  a: 1,\n};\n",  FMT_KIND_NONE },   /* JS 对象字面量 */
    { "all: build\n\tcc -o x x.c\n",  FMT_KIND_NONE },   /* makefile */
    { "#ifndef X_H\n#define N 1\n#endif\n", FMT_KIND_NONE },

    /* 但“等号前留空格”是合法 ini，不能被上面那条规则误伤 */
    { "name = value\nage = 30\n",      FMT_KIND_INI },
  };

  int i;
  g_case = "识别";
  for (i = 0; i < (int)(sizeof(cases)/sizeof(cases[0])); i++) {
    int k = Fmt_DetectKind(cases[i].pszIn, (int)strlen(cases[i].pszIn));
    if (k != cases[i].iKind) {
      fail("识别类型不符");
      printf("        in=<%s> got %d want %d\n", cases[i].pszIn, k,
             cases[i].iKind);
    } else
      ok();
  }
}

/* ------------------------------------------------------------------ 工具 */

static void TestLineCol(void)
{
  const char *p = "ab\r\ncd\nef";
  int iLine, iCol;

  g_case = "行号列号";

  Fmt_OffsetToLineCol(p, (int)strlen(p), 0, &iLine, &iCol);
  if (iLine != 0 || iCol != 0) { fail("0 应该是 1行1列"); printf("        got %d:%d\n", iLine, iCol); } else ok();

  Fmt_OffsetToLineCol(p, (int)strlen(p), 3, &iLine, &iCol);
  if (iLine != 1 || iCol != 0) { fail("3 应该是 2行1列"); printf("        got %d:%d\n", iLine, iCol); } else ok();

  Fmt_OffsetToLineCol(p, (int)strlen(p), 5, &iLine, &iCol);
  if (iLine != 1 || iCol != 1) { fail("5 应该是 2行2列"); printf("        got %d:%d\n", iLine, iCol); } else ok();
}

/* 光标映射：格式化前后落在同一个 token 上 */
static void TestCaret(void)
{
  const char *pIn = "{\"aaa\":1,\"bbb\":2}";
  FMTSTYLE style;
  FMTRESULT res;
  FMTMAP map;
  char *pOut;
  int i, iCaretIn = -1, iCaretOut;

  g_case = "光标映射";

  style.szEol   = "\n";
  style.iIndent = 2;

  memset(&map, 0, sizeof(map));
  if (!Fmt_Format(FMT_KIND_JSON, pIn, (int)strlen(pIn), &style, &res, &map, NULL)) {
    fail("format 失败");
    return;
  }

  if (map.c <= 0) {
    fail("没有生成映射");
    Fmt_ResultFree(&res);
    Fmt_MapFree(&map);
    return;
  }
  ok();

  /* 光标放在 "bbb" 的 b 上 */
  for (i = 0; i < (int)strlen(pIn); i++)
    if (pIn[i] == 'b') { iCaretIn = i; break; }

  iCaretOut = Fmt_MapCaret(&map, iCaretIn, (int)strlen(pIn), res.cb);

  pOut = res.p;
  if (iCaretOut < 0 || iCaretOut >= res.cb || pOut[iCaretOut] != 'b') {
    fail("光标没落在同一个 token 上");
    printf("        in=%d out=%d\n", iCaretIn, iCaretOut);
    if (iCaretOut >= 0 && iCaretOut < res.cb)
      printf("        该位置字符是 '%c'\n", pOut[iCaretOut]);
  } else
    ok();

  Fmt_ResultFree(&res);
  Fmt_MapFree(&map);
}

/* 压缩永不含换行（除了必须保留的：XML 原样区、SQL 行注释）*/
static void TestMinifyNoEol(void)
{
  const char *pIn = "{\"a\":\n1,\n\"b\":[\n1,\n2\n]\n}";
  FMTSTYLE style;
  FMTRESULT res;
  int i;

  g_case = "压缩无换行";

  style.szEol   = "\n";
  style.iIndent = 2;

  if (!Fmt_Minify(FMT_KIND_JSON, pIn, (int)strlen(pIn), &style, &res, NULL, NULL)) {
    fail("minify 失败");
    return;
  }

  for (i = 0; i < res.cb; i++)
    if (res.p[i] == '\n' || res.p[i] == '\r') break;

  if (i != res.cb) {
    fail("压缩结果里还有换行");
    dump(res.p, res.cb);
  } else
    ok();

  check_equal(res.p, res.cb, "{\"a\":1,\"b\":[1,2]}", 17, "压缩结果不符");
  Fmt_ResultFree(&res);
}

/* 行尾沿用文件原有风格 */
static void TestEol(void)
{
  const char *pIn = "{\"a\":1,\"b\":2}";
  FMTSTYLE style;
  FMTRESULT res;
  int i, nCrlf = 0;

  g_case = "行尾风格";

  style.szEol   = "\r\n";
  style.iIndent = 2;

  if (!Fmt_Format(FMT_KIND_JSON, pIn, (int)strlen(pIn), &style, &res, NULL, NULL)) {
    fail("format 失败");
    return;
  }

  for (i = 0; i + 1 < res.cb; i++)
    if (res.p[i] == '\r' && res.p[i+1] == '\n') nCrlf++;

  if (nCrlf != 3) {
    fail("应当有 3 个 CRLF");
    printf("        nCrlf=%d\n", nCrlf);
    dump(res.p, res.cb);
  } else
    ok();

  Fmt_ResultFree(&res);
}

/* 超大输入直接拒绝，不去猜 */
static void TestTooBig(void)
{
  FMTSTYLE style;
  FMTERR err;
  char *p;

  g_case = "超大输入";

  style.szEol   = "\n";
  style.iIndent = 2;
  err.iCode = FMT_E_OK;

  p = (char *)malloc(FMT_MAX_INPUT + 16);
  memset(p, ' ', FMT_MAX_INPUT + 15);
  p[FMT_MAX_INPUT + 15] = '\0';

  if (Fmt_Format(FMT_KIND_JSON, p, FMT_MAX_INPUT + 15, &style, NULL, NULL, &err)) {
    fail("不该成功");
  } else if (err.iCode != FMT_E_TOO_BIG) {
    fail("应为 TOO_BIG");
    printf("        got %s\n", Fmt_ErrName(err.iCode));
  } else
    ok();

  free(p);
}

/* 错误码都有名字（防止漏加）*/
static void TestErrNames(void)
{
  int i;
  g_case = "错误码名称";
  for (i = FMT_E_OK; i <= FMT_E_UNBALANCED; i++) {
    const char *psz = Fmt_ErrName(i);
    if (!psz || !*psz || strcmp(psz, "?") == 0) {
      fail("错误码没有名字");
      printf("        code=%d\n", i);
    } else
      ok();
  }
}

/* ------------------------------------------------------------------ main */

int main(void)
{
  printf("格式化引擎单元测试\n");

  printf(" [JSON]\n");  TestJson();
  printf(" [XML]\n");   TestXml();
  printf(" [SQL]\n");   TestSql();
  printf(" [INI]\n");   TestIni();
  printf(" [识别]\n");  TestDetect();
  printf(" [工具]\n");  TestLineCol();
  printf(" [光标]\n");  TestCaret();
  printf(" [压缩]\n");  TestMinifyNoEol();
  printf(" [行尾]\n");  TestEol();
  printf(" [上限]\n");  TestTooBig();
  printf(" [错误码]\n"); TestErrNames();

  printf("\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
  return (g_fail == 0) ? 0 : 1;
}
