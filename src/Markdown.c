/*===========================================================================

  MDPreView — Markdown 渲染模块
  将 Markdown 文本转换为 HTML，供 MSHTML 预览控件显示。

  依赖：md4c（MIT, Martin Mitas）— https://github.com/mity/md4c

  设计要点：
  - md4c 输出的文本段非零终止，必须按 size 处理并做 HTML 转义
  - md4c 全部按 UTF-8 处理，生成的 HTML 声明 charset=utf-8
  - 动态缓冲区，避免固定大小限制
  - 异常/畸形输入不得导致崩溃（返回错误码即可）

  ===========================================================================*/

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "md4c.h"
#include "Markdown.h"


/* ---- 内部：动态输出缓冲区 ---------------------------------------------- */

typedef struct tagOUTBUF {
  char  *data;
  int    len;
  int    cap;
  BOOL   failed;
} OUTBUF;


static void outbuf_init(OUTBUF *ob)
{
  ob->cap   = 4096;
  ob->len   = 0;
  ob->failed = FALSE;
  ob->data  = (char*)malloc(ob->cap);
  if (!ob->data) {
    ob->cap = 0;
    ob->failed = TRUE;
  } else {
    ob->data[0] = '\0';
  }
}


static void outbuf_free(OUTBUF *ob)
{
  if (ob->data)
    free(ob->data);
  ob->data = NULL;
  ob->cap = ob->len = 0;
}


static BOOL outbuf_reserve(OUTBUF *ob, int extra)
{
  int need;
  char *p;

  if (ob->failed)
    return FALSE;
  if (extra < 0)
    return FALSE;

  need = ob->len + extra + 1;
  if (need <= ob->cap)
    return TRUE;

  /* 翻倍增长，减少 realloc 次数 */
  while (need > ob->cap)
    ob->cap *= 2;

  p = (char*)realloc(ob->data, ob->cap);
  if (!p) {
    ob->failed = TRUE;
    return FALSE;
  }
  ob->data = p;
  return TRUE;
}


static void outbuf_append(OUTBUF *ob, const char *s, int len)
{
  if (!outbuf_reserve(ob, len))
    return;
  if (len > 0)
    memcpy(ob->data + ob->len, s, len);
  ob->len += len;
  ob->data[ob->len] = '\0';
}


static void outbuf_puts(OUTBUF *ob, const char *s)
{
  outbuf_append(ob, s, (int)strlen(s));
}


/* HTML 转义：md4c 的文本段可能包含 < > & " ' */
static void outbuf_append_escaped(OUTBUF *ob, const char *s, int len)
{
  int i;

  for (i = 0; i < len; i++) {
    switch (s[i]) {
      case '<':  outbuf_puts(ob, "&lt;");   break;
      case '>':  outbuf_puts(ob, "&gt;");   break;
      case '&':  outbuf_puts(ob, "&amp;");  break;
      case '"':  outbuf_puts(ob, "&quot;"); break;
      case '\'': outbuf_puts(ob, "&#39;");  break;
      default:   outbuf_append(ob, s + i, 1);
    }
  }
}


/* URL 属性转义：比文本转义宽松，& 需单独处理避免破坏已有实体 */
static void outbuf_append_url(OUTBUF *ob, const char *s, int len)
{
  int i;

  for (i = 0; i < len; i++) {
    switch (s[i]) {
      case '"':  outbuf_puts(ob, "&quot;"); break;
      case '<':  outbuf_puts(ob, "&lt;");   break;
      case '>':  outbuf_puts(ob, "&gt;");   break;
      case '\'': outbuf_puts(ob, "&#39;");  break;
      /* & 不做转义：URL 中的查询串已是合法实体形式，
         转义会破坏它。但单独 & 需转义以防 HTML 解析歧义。*/
      default:   outbuf_append(ob, s + i, 1);
    }
  }
}


/* ---- md4c 回调 ---------------------------------------------------------- */

static int cb_enter_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
  OUTBUF *ob = (OUTBUF*)userdata;

  switch (type) {
    case MD_BLOCK_DOC:   break;
    case MD_BLOCK_QUOTE: outbuf_puts(ob, "<blockquote>"); break;
    case MD_BLOCK_UL:    outbuf_puts(ob, "<ul>");          break;
    case MD_BLOCK_OL:
      outbuf_puts(ob, "<ol>");
      break;
    case MD_BLOCK_LI: {
      MD_BLOCK_LI_DETAIL *d = (MD_BLOCK_LI_DETAIL*)detail;
      outbuf_puts(ob, "<li");
      /* 任务列表：用 class 标记，CSS 渲染为勾选框 */
      if (d && d->is_task) {
        outbuf_puts(ob, d->task_mark == ' ' ? " class=\"task unchecked\">" : " class=\"task checked\">");
      } else {
        outbuf_puts(ob, ">");
      }
      break;
    }
    case MD_BLOCK_HR:    outbuf_puts(ob, "<hr>");          break;
    case MD_BLOCK_H: {
      MD_BLOCK_H_DETAIL *d = (MD_BLOCK_H_DETAIL*)detail;
      char tag[8];
      if (d->level < 1) d->level = 1;
      if (d->level > 6) d->level = 6;
      snprintf(tag, sizeof(tag), "<h%d>", d->level);
      outbuf_puts(ob, tag);
      break;
    }
    case MD_BLOCK_CODE: {
      MD_BLOCK_CODE_DETAIL *d = (MD_BLOCK_CODE_DETAIL*)detail;
      /* lang 可能非零终止，需限定长度 */
      if (d->lang.text && d->lang.size > 0) {
        outbuf_puts(ob, "<pre><code class=\"language-");
        outbuf_append_escaped(ob, d->lang.text, (int)d->lang.size);
        outbuf_puts(ob, "\">");
      } else {
        outbuf_puts(ob, "<pre><code>");
      }
      break;
    }
    case MD_BLOCK_HTML:
      /* 原始 HTML 块：原样透传（md4c 已确认其为合法 HTML）*/
      break;
    case MD_BLOCK_P:     outbuf_puts(ob, "<p>"); break;

    case MD_BLOCK_TABLE:  outbuf_puts(ob, "<table>"); break;
    case MD_BLOCK_THEAD:  outbuf_puts(ob, "<thead>"); break;
    case MD_BLOCK_TBODY:  outbuf_puts(ob, "<tbody>"); break;
    case MD_BLOCK_TR:     outbuf_puts(ob, "<tr>");    break;
    case MD_BLOCK_TH:     outbuf_puts(ob, "<th>");    break;
    case MD_BLOCK_TD:     outbuf_puts(ob, "<td>");    break;

    default:
      /* 未处理的块类型：忽略，不让它中断渲染 */
      break;
  }
  return 0;
}


static int cb_leave_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
  OUTBUF *ob = (OUTBUF*)userdata;

  switch (type) {
    case MD_BLOCK_DOC:   break;
    case MD_BLOCK_QUOTE: outbuf_puts(ob, "</blockquote>"); break;
    case MD_BLOCK_UL:    outbuf_puts(ob, "</ul>");          break;
    case MD_BLOCK_OL:    outbuf_puts(ob, "</ol>");          break;
    case MD_BLOCK_LI:    outbuf_puts(ob, "</li>");          break;
    case MD_BLOCK_HR:    break;
    case MD_BLOCK_H: {
      MD_BLOCK_H_DETAIL *d = (MD_BLOCK_H_DETAIL*)detail;
      char tag[8];
      int lv = d->level;
      if (lv < 1) lv = 1;
      if (lv > 6) lv = 6;
      snprintf(tag, sizeof(tag), "</h%d>", lv);
      outbuf_puts(ob, tag);
      break;
    }
    case MD_BLOCK_CODE:   outbuf_puts(ob, "</code></pre>"); break;
    case MD_BLOCK_HTML:   break;
    case MD_BLOCK_P:      outbuf_puts(ob, "</p>"); break;

    case MD_BLOCK_TABLE:  outbuf_puts(ob, "</table>"); break;
    case MD_BLOCK_THEAD:  outbuf_puts(ob, "</thead>"); break;
    case MD_BLOCK_TBODY:  outbuf_puts(ob, "</tbody>"); break;
    case MD_BLOCK_TR:     outbuf_puts(ob, "</tr>");    break;
    case MD_BLOCK_TH:     outbuf_puts(ob, "</th>");    break;
    case MD_BLOCK_TD:     outbuf_puts(ob, "</td>");    break;

    default:
      break;
  }
  return 0;
}


static int cb_enter_span(MD_SPANTYPE type, void *detail, void *userdata)
{
  OUTBUF *ob = (OUTBUF*)userdata;

  switch (type) {
    case MD_SPAN_EM:      outbuf_puts(ob, "<em>");     break;
    case MD_SPAN_STRONG:  outbuf_puts(ob, "<strong>"); break;
    case MD_SPAN_CODE:    outbuf_puts(ob, "<code>");   break;
    case MD_SPAN_DEL:     outbuf_puts(ob, "<del>");    break;
    case MD_SPAN_U:       outbuf_puts(ob, "<u>");      break;
    case MD_SPAN_A: {
      MD_SPAN_A_DETAIL *d = (MD_SPAN_A_DETAIL*)detail;
      outbuf_puts(ob, "<a href=\"");
      if (d->href.text && d->href.size > 0)
        outbuf_append_url(ob, d->href.text, (int)d->href.size);
      outbuf_puts(ob, "\"");
      if (d->title.text && d->title.size > 0) {
        outbuf_puts(ob, " title=\"");
        outbuf_append_escaped(ob, d->title.text, (int)d->title.size);
        outbuf_puts(ob, "\"");
      }
      outbuf_puts(ob, ">");
      break;
    }
    case MD_SPAN_IMG: {
      MD_SPAN_IMG_DETAIL *d = (MD_SPAN_IMG_DETAIL*)detail;
      outbuf_puts(ob, "<img src=\"");
      if (d->src.text && d->src.size > 0)
        outbuf_append_url(ob, d->src.text, (int)d->src.size);
      outbuf_puts(ob, "\" alt=\"");
      /* alt 内容由后续 text 回调填充，这里先开标签 */
      break;
    }
    case MD_SPAN_WIKILINK: {
      MD_SPAN_WIKILINK_DETAIL *d = (MD_SPAN_WIKILINK_DETAIL*)detail;
      outbuf_puts(ob, "<a href=\"");
      if (d->target.text && d->target.size > 0)
        outbuf_append_url(ob, d->target.text, (int)d->target.size);
      outbuf_puts(ob, "\">");
      break;
    }
    default:
      break;
  }
  return 0;
}


static int cb_leave_span(MD_SPANTYPE type, void *detail, void *userdata)
{
  OUTBUF *ob = (OUTBUF*)userdata;

  switch (type) {
    case MD_SPAN_EM:      outbuf_puts(ob, "</em>");     break;
    case MD_SPAN_STRONG:  outbuf_puts(ob, "</strong>"); break;
    case MD_SPAN_CODE:    outbuf_puts(ob, "</code>");   break;
    case MD_SPAN_DEL:     outbuf_puts(ob, "</del>");    break;
    case MD_SPAN_U:       outbuf_puts(ob, "</u>");      break;
    case MD_SPAN_A:       outbuf_puts(ob, "</a>");      break;
    case MD_SPAN_WIKILINK: outbuf_puts(ob, "</a>");     break;
    case MD_SPAN_IMG:
      /* 关闭 alt 属性和 img 标签 */
      outbuf_puts(ob, "\">");
      break;
    default:
      break;
  }
  return 0;
}


static int cb_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata)
{
  OUTBUF *ob = (OUTBUF*)userdata;

  switch (type) {
    case MD_TEXT_NORMAL:
    case MD_TEXT_CODE:
      outbuf_append_escaped(ob, text, (int)size);
      break;

    case MD_TEXT_BR:
      outbuf_puts(ob, "<br>");
      break;
    case MD_TEXT_SOFTBR:
      outbuf_puts(ob, "\n");
      break;

    case MD_TEXT_ENTITY:
      /* md4c 已解码为 UTF-8 字节，转义输出 */
      outbuf_append_escaped(ob, text, (int)size);
      break;

    case MD_TEXT_NULLCHAR:
      outbuf_puts(ob, "&#0;");
      break;

    case MD_TEXT_HTML:
      /* 内联原始 HTML：透传 */
      outbuf_append(ob, text, (int)size);
      break;

    case MD_TEXT_LATEXMATH:
      outbuf_append_escaped(ob, text, (int)size);
      break;

    default:
      break;
  }
  return 0;
}


static void render_debug_log(const char *msg, const char *text, MD_SIZE size)
{
#ifdef MD_DEBUG
  FILE *fp = fopen("C:\\Project\\MDPreView\\md_debug.log", "a");
  if (fp) {
    fprintf(fp, "%s [%u bytes]: %.*s\n", msg, (unsigned)size,
            (int)(size > 200 ? 200 : size), text);
    fclose(fp);
  }
#else
  (void)msg; (void)text; (void)size;
#endif
}


/* ---- 对外接口 ---------------------------------------------------------- */

/*
  将 Markdown 文本转为完整 HTML 文档。

  mdtext   —— UTF-8 或本地代码页的 Markdown 源文本（以 NUL 结尾）
  返回     —— malloc 分配的完整 HTML 字符串，调用方用 Markdown_FreeHtml 释放
              失败返回 NULL

  注意：md4c 要求输入为 UTF-8。若输入不是 UTF-8，需先由调用方转换。
*/
char* Markdown_Render(const char *mdtext)
{
  OUTBUF body;
  MD_PARSER parser;
  MD_SIZE size;
  int rc;

  if (!mdtext)
    return NULL;

  size = (MD_SIZE)strlen(mdtext);

  /* 空文档也输出合法 HTML 骨架，避免 MSHTML 加载空内容报错 */
  outbuf_init(&body);
  if (body.failed)
    return NULL;

  memset(&parser, 0, sizeof(parser));
  parser.abi_version          = 0;
  /* MD_DIALECT_GITHUB + MD_FLAG_NOHTML：
     - GITHUB 方言提供表格/删除线/任务列表
     - NOHTML 禁用原始 HTML 透传。若不设此位，md4c 会把 <script> 等
       原样输出，MSHTML 加载后会执行 —— 构成存储型 XSS。
       禁用后 <script> 被当普通文本，由 outbuf_append_escaped() 转义。 */
  parser.flags                = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
  parser.enter_block          = cb_enter_block;
  parser.leave_block          = cb_leave_block;
  parser.enter_span           = cb_enter_span;
  parser.leave_span           = cb_leave_span;
  parser.text                 = cb_text;
  parser.debug_log            = NULL;
  parser.syntax               = NULL;

  rc = md_parse(mdtext, size, &parser, &body);

  if (rc != 0 || body.failed) {
    render_debug_log("md_parse failed rc=", mdtext, size);
    outbuf_free(&body);
    return NULL;
  }

  /* 组装完整 HTML 文档。
     样式为 IE11 兼容写法：不用 CSS 变量、不用 Grid、flexbox 用旧语法。 */
  {
    OUTBUF doc;
    outbuf_init(&doc);
    if (doc.failed) {
      outbuf_free(&body);
      return NULL;
    }

    outbuf_puts(&doc,
      "<!DOCTYPE html>\n"
      "<html><head>\n"
      "<meta http-equiv=\"X-UA-Compatible\" content=\"IE=edge\">\n"
      "<meta charset=\"utf-8\">\n"
      "<style>\n"
      "html,body{margin:0;padding:0;}\n"
      "body{\n"
      "  font-family:'Segoe UI',Tahoma,Arial,sans-serif;\n"
      "  font-size:14px;\n"
      "  line-height:1.5;\n"
      "  color:#202020;\n"
      "  background:#ffffff;\n"
      "  padding:12px 16px;\n"
      "  word-wrap:break-word;\n"
      "}\n"
      "h1,h2,h3,h4,h5,h6{\n"
      "  font-family:'Segoe UI Semibold','Segoe UI',Tahoma,sans-serif;\n"
      "  margin:16px 0 8px 0;\n"
      "  line-height:1.25;\n"
      "  font-weight:600;\n"
      "}\n"
      "h1{font-size:26px;border-bottom:1px solid #e0e0e0;padding-bottom:6px;}\n"
      "h2{font-size:21px;border-bottom:1px solid #eaeaea;padding-bottom:4px;}\n"
      "h3{font-size:17px;}\n"
      "h4{font-size:15px;}\n"
      "h5{font-size:14px;}\n"
      "h6{font-size:13px;color:#555;}\n"
      "p{margin:0 0 10px 0;}\n"
      "a{color:#0b57a4;text-decoration:none;}\n"
      "a:hover{text-decoration:underline;}\n"
      "code{\n"
      "  font-family:Consolas,'Courier New',monospace;\n"
      "  font-size:13px;\n"
      "  background:#f0f0f0;\n"
      "  padding:1px 4px;\n"
      "  border-radius:3px;\n"
      "}\n"
      "pre{\n"
      "  background:#f6f8fa;\n"
      "  border:1px solid #e0e0e0;\n"
      "  border-radius:4px;\n"
      "  padding:10px 12px;\n"
      "  overflow:auto;\n"
      "  margin:0 0 12px 0;\n"
      "}\n"
      "pre code{\n"
      "  background:none;\n"
      "  padding:0;\n"
      "  border-radius:0;\n"
      "  font-size:13px;\n"
      "  line-height:1.45;\n"
      "}\n"
      "blockquote{\n"
      "  margin:0 0 12px 0;\n"
      "  padding:4px 14px;\n"
      "  border-left:4px solid #d0d7de;\n"
      "  color:#57606a;\n"
      "  background:#f8f9fa;\n"
      "}\n"
      "blockquote p:last-child{margin-bottom:0;}\n"
      "table{\n"
      "  border-collapse:collapse;\n"
      "  margin:0 0 12px 0;\n"
      "  display:table;\n"
      "}\n"
      "th,td{\n"
      "  border:1px solid #d0d7de;\n"
      "  padding:5px 12px;\n"
      "  text-align:left;\n"
      "}\n"
      "th{background:#f0f2f5;font-weight:600;}\n"
      "tr:nth-child(2n){background:#fafbfc;}\n"
      "ul,ol{margin:0 0 12px 0;padding-left:26px;}\n"
      "li{margin:2px 0;}\n"
      "li.task{list-style:none;margin-left:-18px;}\n"
      "li.task:before{content:\"\\2610\";margin-right:6px;}\n"
      "li.task.checked:before{content:\"\\2611\";color:#1a7f37;}\n"
      "hr{border:none;border-top:1px solid #d8dee4;margin:16px 0;}\n"
      "img{max-width:100%;height:auto;}\n"
      "del{color:#8c959f;}\n"
      "</style>\n"
      "</head>\n"
      "<body>\n");

    /* md4c 生成的 body 片段 */
    if (body.len > 0)
      outbuf_append(&doc, body.data, body.len);

    outbuf_puts(&doc, "\n</body></html>\n");

    outbuf_free(&body);

    if (doc.failed) {
      outbuf_free(&doc);
      return NULL;
    }
    return doc.data;   /* 所有权转移给调用方 */
  }
}


void Markdown_FreeHtml(char *html)
{
  if (html)
    free(html);
}
