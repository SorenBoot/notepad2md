/* ==========================================================================
   Markdown / YAML / Shell 词法器单测

   把三个自写词法器直接编进来，用内存 Accessor 收集涂色区间，
   断言每种语法元素落在正确的字符范围内（不依赖 GUI，可重复）。

   编译（在仓库根目录）：
     g++ -O1 -o /tmp/lexer_test \
         tests/run/test_lexers.cxx \
         -I tests/run/lexfake -I scintilla/include -I scintilla/lexlib \
         -DSCI_NAMESPACE=
   ========================================================================== */

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "SciLexer.h"

#include "../../scintilla/lexers/LexMarkdown.cxx"
#include "../../scintilla/lexers/LexYaml.cxx"
#include "../../scintilla/lexers/LexBash.cxx"

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

/* 断言 [from, to] 闭区间的样式都等于 style（按字节偏移） */
static void ExpectRange(Accessor &a, size_t from, size_t to, int style,
                        const char *what) {
  for (size_t i = from; i <= to && i < a.styles.size(); i++) {
    if (a.styles[i] != style) {
      char buf[256];
      snprintf(buf, sizeof(buf),
               "'%s' 偏移 %u 样式 %d，期望 %d（区间 %u..%u）",
               a.doc.c_str(), (unsigned)i, a.styles[i], style,
               (unsigned)from, (unsigned)to);
      Check(false, what, buf);
      return;
    }
  }
  Check(true, what);
}

/* 断言整段文档每个字节都被显式涂色（否则 Scintilla 会沿用上一轮的旧样式） */
static void ExpectFullyPainted(Accessor &a, const char *what) {
  size_t first = 0;
  size_t n = a.UnpaintedCount(&first);
  char buf[160];
  if (n) {
    snprintf(buf, sizeof(buf), "%u 个字节未涂色，首个偏移 %u（附近: %.20s）",
             (unsigned)n, (unsigned)first,
             first < a.doc.size() ? a.doc.c_str() + first : "");
  }
  Check(n == 0, what, buf);
}

static Accessor RunMarkdown(const std::string &text) {
  Accessor a;
  a.SetDoc(text.c_str(), text.size());
  WordList *kw[9] = {0};
  ColouriseMarkdownDoc(0, (int)text.size(), 0, kw, a);
  ExpectFullyPainted(a, "整段无未涂色空档");
  return a;
}

static Accessor RunYaml(const std::string &text) {
  Accessor a;
  a.SetDoc(text.c_str(), text.size());
  WordList *kw[9] = {0};
  ColouriseYamlDoc(0, (int)text.size(), 0, kw, a);
  ExpectFullyPainted(a, "整段无未涂色空档");
  return a;
}

static Accessor RunBash(const std::string &text, const char *keywords) {
  Accessor a;
  a.SetDoc(text.c_str(), text.size());
  WordList wl;
  wl.Set(keywords);
  WordList *kw[9] = {&wl, 0, 0, 0, 0, 0, 0, 0, 0};
  ColouriseShellDoc(0, (int)text.size(), 0, kw, a);
  ExpectFullyPainted(a, "整段无未涂色空档");
  return a;
}

static void ExpectNoOrderViolation(Accessor &a, const char *what) {
  char buf[128];
  if (a.orderViolation)
    snprintf(buf, sizeof(buf), "ColourTo(%u) 在 startSeg=%u 之后倒序",
             a.vPos, a.vSeg);
  Check(!a.orderViolation, what, buf);
}

/* ---- Markdown ---- */
static void TestMarkdown() {
  g_case = "md 粗体只覆盖 **...**";
  {
    std::string t = "a **bold** b\n";
    Accessor a = RunMarkdown(t);
    size_t p = t.find("**bold**");
    ExpectRange(a, 0, p - 1, SCE_MARKDOWN_DEFAULT, "粗体前文本保持默认");
    ExpectRange(a, p, p + 7, SCE_MARKDOWN_STRONG1, "**bold** 整段为粗体");
    ExpectRange(a, p + 8, t.size() - 2, SCE_MARKDOWN_DEFAULT, "粗体后文本保持默认");
    ExpectNoOrderViolation(a, "无 pos < startSeg 的倒序涂色");
  }

  g_case = "md 斜体只覆盖 *...*";
  {
    std::string t = "c *it* d\n";
    Accessor a = RunMarkdown(t);
    size_t p = t.find("*it*");
    ExpectRange(a, 0, p - 1, SCE_MARKDOWN_DEFAULT, "斜体前文本默认");
    ExpectRange(a, p, p + 3, SCE_MARKDOWN_EM1, "*it* 整段斜体");
    ExpectRange(a, p + 4, t.size() - 2, SCE_MARKDOWN_DEFAULT, "斜体后文本默认");
  }

  g_case = "md 行内代码";
  {
    std::string t = "e `code` f\n";
    Accessor a = RunMarkdown(t);
    size_t p = t.find("`code`");
    ExpectRange(a, 0, p - 1, SCE_MARKDOWN_DEFAULT, "代码前默认");
    ExpectRange(a, p, p + 5, SCE_MARKDOWN_CODEBK, "`code` 整段行内代码");
    ExpectRange(a, p + 6, t.size() - 2, SCE_MARKDOWN_DEFAULT, "代码后默认");
  }

  g_case = "md 标题 / 列表 / 引用 / 分隔线 / 链接";
  {
    std::string t = "# H1\n- item\n> quote\n---\n[x](http://a)\n";
    Accessor a = RunMarkdown(t);
    const char *s = t.c_str();
    ExpectRange(a, 0, 3, SCE_MARKDOWN_HEADER1, "# H1 为一级标题");
    size_t l2 = t.find("- item");
    ExpectRange(a, l2, l2, SCE_MARKDOWN_ULIST_ITEM, "- 标记为列表样式");
    ExpectRange(a, l2 + 1, l2 + 5, SCE_MARKDOWN_DEFAULT, "列表正文为默认样式");
    size_t l3 = t.find("> quote");
    ExpectRange(a, l3, l3 + 6, SCE_MARKDOWN_BLOCKQUOTE, "> quote 为引用");
    size_t l4 = t.find("---");
    ExpectRange(a, l4, l4 + 2, SCE_MARKDOWN_HRULE, "--- 为分隔线");
    size_t l5 = t.find("[x](http://a)");
    ExpectRange(a, l5, l5 + 12, SCE_MARKDOWN_LINK, "链接整段为链接样式");
    (void)s;
  }

  g_case = "md 列表正文与有序列表";
  {
    std::string t = "- a **b**" "\n" "1. two" "\n";
    Accessor a = RunMarkdown(t);
    ExpectRange(a, 0, 0, SCE_MARKDOWN_ULIST_ITEM, "- 标记");
    ExpectRange(a, 1, 1, SCE_MARKDOWN_DEFAULT, "标记后空格");
    ExpectRange(a, 2, 2, SCE_MARKDOWN_DEFAULT, "列表正文首字符");
    ExpectRange(a, 4, 8, SCE_MARKDOWN_STRONG1, "列表内粗体");
    size_t ol = t.find("1. two");
    ExpectRange(a, ol, ol + 1, SCE_MARKDOWN_OLIST_ITEM, "有序列表标记 1.");
    ExpectRange(a, ol + 2, ol + 5, SCE_MARKDOWN_DEFAULT, "有序列表正文");
  }

  g_case = "md 围栏代码块";
  {
    std::string t = "text\n```py\nprint(1)\n```\nafter\n";
    Accessor a = RunMarkdown(t);
    size_t fence = t.find("```py");
    ExpectRange(a, fence, fence + 4, SCE_MARKDOWN_CODE, "起始围栏为代码块");
    size_t body = t.find("print(1)");
    ExpectRange(a, body, body + 7, SCE_MARKDOWN_CODE, "围栏内代码行");
    size_t close = t.rfind("```");
    ExpectRange(a, close, close + 2, SCE_MARKDOWN_CODE, "结束围栏");
    size_t after = t.find("after");
    ExpectRange(a, after, after + 4, SCE_MARKDOWN_DEFAULT,
                "围栏结束后回到默认样式");
  }

  g_case = "md 中文多字节不错位";
  {
    std::string t = "这是 **粗体** 和 *斜体*，还有 `code`。\n";
    Accessor a = RunMarkdown(t);
    size_t p = t.find("**粗体**");
    ExpectRange(a, p, p + 9, SCE_MARKDOWN_STRONG1, "多字节粗体区间");
    ExpectRange(a, 0, p - 1, SCE_MARKDOWN_DEFAULT, "多字节前缀默认");
    size_t e = t.find("*斜体*");
    ExpectRange(a, e, e + 7, SCE_MARKDOWN_EM1, "多字节斜体区间");
    size_t c = t.find("`code`");
    ExpectRange(a, c, c + 5, SCE_MARKDOWN_CODEBK, "多字节之后的行内代码");
  }

  g_case = "md 空文档 / 单换行不崩";
  {
    std::string t = "";
    Accessor a = RunMarkdown(t);
    Check(a.paintCalls == 0, "空文档不涂色");
    std::string t2 = "\n\n\n";
    Accessor b = RunMarkdown(t2);
    ExpectNoOrderViolation(b, "纯换行文档无倒序涂色");
  }
}

/* ---- YAML ---- */
static void TestYaml() {
  g_case = "yaml 键与注释";
  {
    std::string t = "name: demo\n# note\nflag: true\n";
    Accessor a = RunYaml(t);
    size_t k = t.find("name");
    Check(a.styles[k] != 0, "key 有专门样式");
    size_t c = t.find('#');
    Check(a.styles[c] != 0, "# 注释有样式");
    ExpectNoOrderViolation(a, "无倒序涂色");
  }
  g_case = "yaml 文档标记与块标量";
  {
    std::string t = "---\nlist:\n  - one\nkey: |\n  text\n";
    Accessor a = RunYaml(t);
    Check(a.styles[0] != 0, "--- 文档标记有样式");
    ExpectNoOrderViolation(a, "无倒序涂色");
  }
  g_case = "yaml 空 / 长行不越界";
  {
    std::string t = "";
    Accessor a = RunYaml(t);
    Check(a.paintCalls == 0, "空文档不涂色");
    std::string t2(5000, 'a');
    t2 += ": 1\n";
    Accessor b = RunYaml(t2);
    ExpectNoOrderViolation(b, "超长行无倒序涂色");
  }
}

/* ---- Shell ---- */
static void TestBash() {
  const char *kw = "if then else fi for do done while case esac function echo "
                   "local export return";
  g_case = "sh 关键字 / 变量 / 字符串 / 注释";
  {
    std::string t = "if [ -f x ]; then\n  echo \"$NAME\"\nfi\n# c\n";
    Accessor a = RunBash(t, kw);
    Check(a.styles[0] == SCE_SH_WORD, "行首 if 为关键字");
    size_t s = t.find("\"$NAME\"");
    Check(a.styles[s] == SCE_SH_STRING, "双引号字符串");
    size_t c = t.find('#');
    Check(a.styles[c] == SCE_SH_COMMENTLINE, "# 为注释");
    ExpectNoOrderViolation(a, "无倒序涂色");
  }
  g_case = "sh 未闭合引号 / heredoc 不越界";
  {
    std::string t = "echo \"unterminated\ncat <<EOF\nbody\nEOF\n";
    Accessor a = RunBash(t, kw);
    ExpectNoOrderViolation(a, "未闭合引号无倒序涂色");
  }
  g_case = "sh 空文档";
  {
    std::string t = "";
    Accessor a = RunBash(t, kw);
    Check(a.paintCalls == 0, "空文档不涂色");
  }
}

int main() {
  printf("== 词法器单元测试 ==\n\n");
  TestMarkdown();
  TestYaml();
  TestBash();
  printf("\n通过 %d, 失败 %d\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
