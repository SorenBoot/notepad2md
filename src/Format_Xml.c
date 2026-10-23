//=============================================================================
//
//  Format_Xml.c — XML / HTML 的格式化、压缩、校验
//
//  这个格式最容易出事，所以规则定得很保守：
//
//    1) 只重排"元素之间"的空白。文本节点里的空白一个都不动 ——
//       <b>a</b> <b>b</b> 中间那个空格是内容，不是排版，动了就改数据。
//
//    2) 混合内容（元素里既有子元素又有非空白文本）整体原样输出，不拆行。
//       典型如 <p>看这里 <a href="x">链接</a> 谢谢</p>，硬拆会多出空白。
//
//    3) <pre> / <textarea> / <script> / <style> 内容原样，
//       xml:space="preserve" 的子树原样。
//
//    4) 本来就写在一行、且不长的元素整体保留一行 —— 作者的排版意图优先。
//
//    5) 属性顺序、引号种类、实体写法（&amp; vs &#38;）、自闭合写法
//       （<a/> 与 <a></a>）一律不改，只在属性之间留一个空格。
//
//  HTML 用宽松模式：容忍未闭合标签（<li> / <p> 之类）与空元素（<br>），
//  不因为不合法就拒绝整理 —— 现实里的 HTML 大多不合 XML 规范。
//
//=============================================================================

#include <stdlib.h>
#include <string.h>

#include "Format_Int.h"

#define XMAX_ATTRS 128

typedef struct {

  const char *p;
  int cb;
  int i;

  FMTC *pC;
  int   bTolerant;   /* HTML 宽松模式 */
  int   bPreserve;   /* 继承下来的 xml:space="preserve" */

} XP;

/* 内容扫描结果 */
typedef struct {
  int iContentStart;
  int iContentEnd;   /* 指向 </name> 的 '<'；-1 = 没找到 */
  int iTagEnd;       /* </name> 之后的位置；-1 = 没找到 */
  int bHasText;      /* 内容里是否有非空白文本 */
  int bImplicit;     /* 被祖先标签隐式闭合（HTML）*/
} XSCAN;

/* 起点标签解析结果 */
typedef struct {
  int iNameStart;
  int cbName;
  int iTagEnd;       /* '>' 之后 */
  int bSelfClose;
} XTAG;

/*=============================================================================
  基础
=============================================================================*/

/* 标签名允许非 ASCII 字节：<根><子> 这样的中文标签是合法 XML，
   而本模块全程不解码，多字节内容一律原样搬运。 */
static int xIsNameStart(int c) {
  return (Fmt_IsAlpha(c) || c == '_' || c == ':' || c >= 0x80);
}

static int xIsNameChar(int c) {
  return (Fmt_IsAlpha(c) || Fmt_IsDigit(c) || c == '_' || c == ':' ||
          c == '-' || c == '.' || c >= 0x80);
}

static int xMatchICase(const char *p, int cb, const char *psz) {
  int i, n = (int)strlen(psz);
  if (cb < n) return 0;
  for (i = 0; i < n; i++) {
    int a = (unsigned char)p[i], b = (unsigned char)psz[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return 0;
  }
  return 1;
}

static int xNameEq(const char *p, int cb, const char *q, int cbQ) {
  int i;
  if (cb != cbQ) return 0;
  for (i = 0; i < cb; i++) {
    int a = (unsigned char)p[i], b = (unsigned char)q[i];
    /* HTML 标签名不区分大小写 */
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return 0;
  }
  return 1;
}

/* HTML 里不需要闭合的空元素 */
static int xIsVoid(const char *p, int cb) {
  static const char *apsz[] = {
    "area","base","br","col","embed","hr","img","input","link","meta",
    "param","source","track","wbr","basefont","frame","isindex"
  };
  int i;
  for (i = 0; i < (int)(sizeof(apsz)/sizeof(apsz[0])); i++)
    if (xMatchICase(p, cb, apsz[i]) && (int)strlen(apsz[i]) == cb)
      return 1;
  return 0;
}

/* 内容按原样处理、不参与缩进的元素 */
static int xIsRawText(const char *p, int cb) {
  static const char *apsz[] = { "script","style","pre","textarea","xmp","plaintext" };
  int i;
  for (i = 0; i < (int)(sizeof(apsz)/sizeof(apsz[0])); i++)
    if (xMatchICase(p, cb, apsz[i]) && (int)strlen(apsz[i]) == cb)
      return 1;
  return 0;
}

/*=============================================================================
  扫描（只看不写）
=============================================================================*/

/* 解析起点标签。x->i 必须指向 '<'。失败返回 0 */
static int xParseStartTag(XP *x, XTAG *t) {

  int i = x->i + 1;
  int iName;

  if (i >= x->cb || !xIsNameStart((unsigned char)x->p[i]))
    return 0;

  iName = i;
  while (i < x->cb && xIsNameChar((unsigned char)x->p[i])) i++;

  t->iNameStart = iName;
  t->cbName     = i - iName;
  t->bSelfClose = 0;

  /* 属性：一路扫到 '>' */
  for (;;) {

    while (i < x->cb && Fmt_IsSpace((unsigned char)x->p[i])) i++;
    if (i >= x->cb)
      return 0;

    if (x->p[i] == '>') { i++; break; }

    if (x->p[i] == '/' && i + 1 < x->cb && x->p[i + 1] == '>') {
      t->bSelfClose = 1;
      i += 2;
      break;
    }

    /* 属性名 */
    if (!(xIsNameStart((unsigned char)x->p[i]) || x->p[i] == '@' || x->p[i] == '#'))
      i++;                                   /* 容错：跳过怪字符 */
    else {
      while (i < x->cb && (xIsNameChar((unsigned char)x->p[i]) ||
                           x->p[i] == '@' || x->p[i] == '#')) i++;
      {
        int iSave = i;
        while (i < x->cb && Fmt_IsSpace((unsigned char)x->p[i])) i++;
        if (i < x->cb && x->p[i] == '=') {
          i++;
          while (i < x->cb && Fmt_IsSpace((unsigned char)x->p[i])) i++;
          if (i < x->cb && (x->p[i] == '"' || x->p[i] == '\'')) {
            char q = x->p[i++];
            while (i < x->cb && x->p[i] != q) i++;
            if (i < x->cb) i++;
          }
          else {
            while (i < x->cb && !Fmt_IsSpace((unsigned char)x->p[i]) &&
                   x->p[i] != '>' && x->p[i] != '/') i++;
          }
        }
        else {
          i = iSave;                          /* 没有 '='：布尔属性 */
        }
      }
    }
  }

  t->iTagEnd = i;
  return 1;
}

/* 从 iFrom 起找 </name>，返回结束标签之后的位置；找不到返回 -1 */
static int xFindEndTag(XP *x, int iFrom, const char *pszName, int cbName) {

  int i = iFrom;

  while (i + 1 < x->cb) {
    if (x->p[i] == '<' && x->p[i+1] == '/') {
      int j = i + 2;
      while (j < x->cb && xIsNameChar((unsigned char)x->p[j])) j++;
      if (j - (i + 2) == cbName && xNameEq(x->p + i + 2, cbName, pszName, cbName)) {
        while (j < x->cb && x->p[j] != '>') j++;
        return (j < x->cb) ? j + 1 : x->cb;
      }
      i = j;
      continue;
    }
    i++;
  }

  return -1;
}

/* 从 x->i 起扫内容，找到本元素的结束标签。
   pszName/cbName 是本元素的名字；aiNameOff/aiNameLen 是嵌套名栈。 */
static int xScanContent(XP *x, const char *pszName, int cbName, XSCAN *s) {

  int i;
  int aOff[FMT_MAX_DEPTH];
  int aLen[FMT_MAX_DEPTH];
  int nStack = 0;

  memset(s, 0, sizeof(XSCAN));
  s->iContentStart = x->i;
  s->iContentEnd   = -1;
  s->iTagEnd       = -1;

  i = x->i;

  while (i < x->cb) {

    if (x->p[i] != '<') {
      /* 文本：扫到下一个 '<' */
      int i0 = i;
      while (i < x->cb && x->p[i] != '<') i++;
      /* 只有"自己这一层"的文本才算混合内容 ——
         子元素内部的文字是子元素的内容，跟父元素要不要拆行无关 */
      if (nStack == 0) {
        int k;
        for (k = i0; k < i; k++)
          if (!Fmt_IsSpace((unsigned char)x->p[k])) { s->bHasText = 1; break; }
      }
      continue;
    }

    /* 注释 */
    if (i + 3 < x->cb && x->p[i+1] == '!' && x->p[i+2] == '-' && x->p[i+3] == '-') {
      i += 4;
      while (i + 2 < x->cb &&
             !(x->p[i] == '-' && x->p[i+1] == '-' && x->p[i+2] == '>')) i++;
      if (i + 2 >= x->cb) { x->i = x->cb; return 0; }
      i += 3;
      continue;
    }

    /* CDATA：算文本 */
    if (i + 8 < x->cb && x->p[i+1] == '!' && xMatchICase(x->p + i, 9, "<![CDATA[")) {
      i += 9;
      while (i + 2 < x->cb &&
             !(x->p[i] == ']' && x->p[i+1] == ']' && x->p[i+2] == '>')) i++;
      if (i + 2 >= x->cb) { x->i = x->cb; return 0; }
      i += 3;
      /* 和普通文本一样，只有本层的 CDATA 才算混合内容；
         子元素里的 CDATA 不该把父元素钉死成原样输出 */
      if (nStack == 0)
        s->bHasText = 1;
      continue;
    }

    /* DOCTYPE 等 <!...>（内部子集可能带 [ ]）*/
    if (i + 1 < x->cb && x->p[i+1] == '!') {
      int nBr = 0;
      i += 2;
      while (i < x->cb) {
        if (x->p[i] == '[') nBr++;
        else if (x->p[i] == ']') { if (nBr > 0) nBr--; }
        else if (x->p[i] == '>' && nBr == 0) { i++; break; }
        i++;
      }
      continue;
    }

    /* 处理指令 <? ... ?> */
    if (i + 1 < x->cb && x->p[i+1] == '?') {
      i += 2;
      while (i + 1 < x->cb && !(x->p[i] == '?' && x->p[i+1] == '>')) i++;
      i = (i + 1 < x->cb) ? i + 2 : x->cb;
      continue;
    }

    /* 结束标签 */
    if (i + 1 < x->cb && x->p[i+1] == '/') {
      int iName = i + 2;
      int i2 = iName;
      while (i2 < x->cb && xIsNameChar((unsigned char)x->p[i2])) i2++;
      {
        int cb2 = i2 - iName;
        int iTagEnd = i2;
        while (iTagEnd < x->cb && x->p[iTagEnd] != '>') iTagEnd++;
        if (iTagEnd < x->cb) iTagEnd++;

        if (nStack == 0) {
          /* 栈空了 —— 这个结束标签要么是本元素的，要么是祖先的 */
          if (xNameEq(x->p + iName, cb2, pszName, cbName)) {
            s->iContentEnd = i;
            s->iTagEnd     = iTagEnd;
            x->i = iTagEnd;
            return 1;
          }
          /* 结束标签对不上：这里绝不猜。猜错就是悄悄改坏文档，
             所以宁可直接报错，让用户去修那行标签 */
          if (x->pC) Fmt_SetErr(x->pC, FMT_E_TAG_MISMATCH, i);
          return 0;
        }

        /* 栈非空：弹到匹配的名字 */
        {
          int k, iFound = -1;
          for (k = nStack - 1; k >= 0; k--)
            if (xNameEq(x->p + aOff[k], aLen[k], x->p + iName, cb2)) { iFound = k; break; }
          if (iFound >= 0) nStack = iFound;
          else {
            /* 栈里找不到同名标签：有标签没闭合 */
            if (x->pC) Fmt_SetErr(x->pC, FMT_E_TAG_MISMATCH, i);
            return 0;
          }
        }
        i = iTagEnd;
        continue;
      }
    }

    /* 起点标签 */
    {
      XTAG t;
      int iSave = x->i;
      x->i = i;
      if (!xParseStartTag(x, &t)) {
        /* 不是标签，当普通文本里的 '<'（HTML 里很常见，比如 a < b）*/
        i++;
        x->i = iSave;
        if (nStack == 0) s->bHasText = 1;
        continue;
      }
      i = t.iTagEnd;
      x->i = iSave;

      /* <script> / <style> / <pre> / <textarea>：内容不是标记，整块跳过。
         不跳的话，脚本里的 a < b、字符串里的 "</div>" 都会把扫描带偏 */
      if (!t.bSelfClose && xIsRawText(x->p + t.iNameStart, t.cbName)) {
        int j = xFindEndTag(x, t.iTagEnd, x->p + t.iNameStart, t.cbName);
        if (j < 0) {
          if (x->pC) Fmt_SetErr(x->pC, FMT_E_UNTERMINATED_TAG, t.iNameStart - 1);
          return 0;
        }
        i = j;
        continue;
      }

      if (!t.bSelfClose && !(x->bTolerant && xIsVoid(x->p + t.iNameStart, t.cbName))) {
        if (nStack < FMT_MAX_DEPTH) {
          aOff[nStack] = t.iNameStart;
          aLen[nStack] = t.cbName;
          nStack++;
        }
      }
    }
  }

  if (x->pC) Fmt_SetErr(x->pC, FMT_E_UNTERMINATED_TAG,
                        s->iContentStart > 0 ? s->iContentStart - 1 : 0);
  return 0;
}

/*=============================================================================
  输出
=============================================================================*/

/* 把起点标签规范化后写出去：属性之间一个空格，属性值原样 */
static void xEmitStartTag(XP *x, int iTagStart, int iTagEnd, int bSelfClose) {

  int i = iTagStart + 1;
  int iName;

  while (i < iTagEnd && xIsNameChar((unsigned char)x->p[i])) i++;
  iName = i;

  Fmt_Tok(x->pC, iTagStart, "<", 1);
  Fmt_Tok(x->pC, iTagStart + 1, x->p + iTagStart + 1, iName - (iTagStart + 1));

  /* 属性重复一遍解析，只为写出；解析器已确认结构合法 */
  for (;;) {

    while (i < iTagEnd && Fmt_IsSpace((unsigned char)x->p[i])) i++;
    if (i >= iTagEnd)
      break;
    if (x->p[i] == '>')
      break;
    if (x->p[i] == '/' && i + 1 < iTagEnd && x->p[i + 1] == '>')
      break;

    {
      int iAttr = i;
      if (!(xIsNameStart((unsigned char)x->p[i]) || x->p[i] == '@' || x->p[i] == '#')) {
        i++;
        continue;
      }
      while (i < iTagEnd && (xIsNameChar((unsigned char)x->p[i]) ||
                             x->p[i] == '@' || x->p[i] == '#')) i++;
      {
        int iAfterName = i;
        int j = i;
        while (j < iTagEnd && Fmt_IsSpace((unsigned char)x->p[j])) j++;
        if (j < iTagEnd && x->p[j] == '=') {
          j++;
          while (j < iTagEnd && Fmt_IsSpace((unsigned char)x->p[j])) j++;
          if (j < iTagEnd && (x->p[j] == '"' || x->p[j] == '\'')) {
            char q = x->p[j++];
            while (j < iTagEnd && x->p[j] != q) j++;
            if (j < iTagEnd) j++;
          }
          else {
            while (j < iTagEnd && !Fmt_IsSpace((unsigned char)x->p[j]) &&
                   x->p[j] != '>' && x->p[j] != '/') j++;
          }
          i = j;
          Fmt_PutC(x->pC, ' ');
          Fmt_Tok(x->pC, iAttr, x->p + iAttr, i - iAttr);
          /* 值里有空白不要紧，原样；这里只在名字与 = 之间补了规范写法 */
        }
        else {
          i = iAfterName;
          Fmt_PutC(x->pC, ' ');
          Fmt_Tok(x->pC, iAttr, x->p + iAttr, i - iAttr);
        }
      }
    }
  }

  if (bSelfClose) {
    Fmt_Tok(x->pC, iTagEnd - 2, "/>", 2);
  }
  else {
    Fmt_Tok(x->pC, iTagEnd - 1, ">", 1);
  }
}

static int xmlNode(XP *x, int iDepth);

/* 元素。x->i 指向起点标签的 '<' */
static int xmlElement(XP *x, int iDepth) {

  int iTagStart = x->i;
  int bWroteChild = 0;   /* 有没有真的写出子节点（空元素要贴着结束标签）*/
  XTAG t;
  XSCAN s;
  FMTC *pC = x->pC;

  if (!xParseStartTag(x, &t)) {
    Fmt_SetErr(pC, FMT_E_UNTERMINATED_TAG, iTagStart);
    return 0;
  }

  Fmt_IndentTo(pC, iDepth);

  if (t.bSelfClose) {
    xEmitStartTag(x, iTagStart, t.iTagEnd, 1);
    x->i = t.iTagEnd;
    return 1;
  }

  /* HTML 空元素（<meta> / <br> / <img> ...）：本来就没有内容 */
  if (x->bTolerant && xIsVoid(x->p + t.iNameStart, t.cbName)) {
    xEmitStartTag(x, iTagStart, t.iTagEnd, 0);
    x->i = t.iTagEnd;
    return 1;
  }

  x->i = t.iTagEnd;

  if (!xScanContent(x, x->p + t.iNameStart, t.cbName, &s))
    return 0;

  /* 原本写在一行且不长的元素不再整块原样输出 —— 那样嵌套结构永远捞不到
     重新排版的时机（<a><b><c/></b></a> 按一下格式化什么都不变，很莫名其妙）。
     叶子元素带文本时下面的 bHasText 分支仍然会把它整行输出，所以
     <p>hello</p> 这类写法不会被拆开。 */

  xEmitStartTag(x, iTagStart, t.iTagEnd, 0);

  {
    int bRawText  = xIsRawText(x->p + t.iNameStart, t.cbName);
    int bPreserve = x->bPreserve;

    /* 属性里写了 xml:space="preserve"，整棵子树原样 */
    {
      int k;
      for (k = t.iNameStart; k + 9 < t.iTagEnd; k++) {
        if (x->p[k] == 'x' && xMatchICase(x->p + k, 9, "xml:space")) {
          int j = k + 9;
          while (j < t.iTagEnd && Fmt_IsSpace((unsigned char)x->p[j])) j++;
          if (j < t.iTagEnd && x->p[j] == '=') {
            j++;
            while (j < t.iTagEnd && Fmt_IsSpace((unsigned char)x->p[j])) j++;
            if (j < t.iTagEnd && (x->p[j] == '"' || x->p[j] == '\'')) {
              int iVal = j + 1;
              int iValEnd = iVal;
              while (iValEnd < t.iTagEnd && x->p[iValEnd] != x->p[j]) iValEnd++;
              if (xMatchICase(x->p + iVal, iValEnd - iVal, "preserve"))
                bPreserve = 1;
            }
          }
        }
      }
    }

    if (s.bHasText || bPreserve || bRawText) {

      /* 混合内容 / 原样区：内容一个字节都不动。
         连首尾的换行也不动 —— <pre> 的行数、行内空格都是有意义的，
         普通混合内容则原样保留刚好能把结束标签送回它原来的位置 */
      if (s.iContentEnd > s.iContentStart)
        Fmt_Tok(pC, s.iContentStart, x->p + s.iContentStart,
                s.iContentEnd - s.iContentStart);
    }
    else {

      /* 纯元素内容：一个子节点一行。
         xScanContent 把 x->i 留在了结束标签之后，这里要倒回内容起点 */
      x->bPreserve = bPreserve;
      x->i = s.iContentStart;

      while (x->i < ((s.iContentEnd >= 0) ? s.iContentEnd : x->cb)) {

        if (x->p[x->i] != '<') {
          /* 纯空白（扫描已确认没有非空白文本），跳过 */
          x->i++;
          continue;
        }
        Fmt_Newline(pC);
        if (!xmlNode(x, iDepth + 1))
          return 0;
        bWroteChild = 1;
      }

      x->bPreserve = bPreserve;
    }
  }

  /* 结束标签。混合内容紧贴内容走，纯元素内容另起一行 */
  if (s.iTagEnd > 0) {

    int bVerbatim = (s.bHasText || xIsRawText(x->p + t.iNameStart, t.cbName)
                     || x->bPreserve);

    /* <x></x> / <x>   </x> 这种空元素贴着写，不要拆成三行 */
    if (!bVerbatim && bWroteChild) {
      Fmt_Newline(pC);
      Fmt_IndentTo(pC, iDepth);
    }

    Fmt_Tok(pC, s.iContentEnd, x->p + s.iContentEnd, s.iTagEnd - s.iContentEnd);
    x->i = s.iTagEnd;
  }

  return 1;
}

/* 注释 / CDATA / PI / DOCTYPE：整块原样输出 */
static int xmlMisc(XP *x, int iDepth) {

  int i = x->i;
  int iEnd = -1;

  if (i + 3 < x->cb && x->p[i+1] == '!' && x->p[i+2] == '-' && x->p[i+3] == '-') {
    int j = i + 4;
    while (j + 2 < x->cb &&
           !(x->p[j] == '-' && x->p[j+1] == '-' && x->p[j+2] == '>')) j++;
    iEnd = (j + 2 < x->cb) ? j + 3 : x->cb;
  }
  else if (i + 8 < x->cb && xMatchICase(x->p + i, 9, "<![CDATA[")) {
    int j = i + 9;
    while (j + 2 < x->cb &&
           !(x->p[j] == ']' && x->p[j+1] == ']' && x->p[j+2] == '>')) j++;
    iEnd = (j + 2 < x->cb) ? j + 3 : x->cb;
  }
  else if (i + 1 < x->cb && x->p[i+1] == '?') {
    int j = i + 2;
    while (j + 1 < x->cb && !(x->p[j] == '?' && x->p[j+1] == '>')) j++;
    iEnd = (j + 1 < x->cb) ? j + 2 : x->cb;
  }
  else if (i + 1 < x->cb && x->p[i+1] == '!') {
    int j = i + 2;
    int nBr = 0;
    while (j < x->cb) {
      if (x->p[j] == '[') nBr++;
      else if (x->p[j] == ']') { if (nBr > 0) nBr--; }
      else if (x->p[j] == '>' && nBr == 0) { j++; break; }
      j++;
    }
    iEnd = j;
  }

  if (iEnd < 0)
    return 0;

  Fmt_IndentTo(x->pC, iDepth);
  Fmt_Tok(x->pC, i, x->p + i, iEnd - i);
  x->i = iEnd;
  return 1;
}

static int xmlNode(XP *x, int iDepth) {

  int i = x->i;

  if (i >= x->cb)
    return 0;

  if (x->p[i] != '<') {
    /* 文本节点：原样（只会出现在混合内容里，已经在别处处理）*/
    int j = i;
    while (j < x->cb && x->p[j] != '<') j++;
    Fmt_IndentTo(x->pC, iDepth);
    Fmt_Tok(x->pC, i, x->p + i, j - i);
    x->i = j;
    return 1;
  }

  /* 注释 / CDATA / PI / DOCTYPE */
  if (i + 1 < x->cb &&
      (x->p[i+1] == '!' || x->p[i+1] == '?')) {
    if (xmlMisc(x, iDepth))
      return 1;
  }

  /* 孤立的结束标签（HTML 宽松模式下可能碰到）*/
  if (i + 1 < x->cb && x->p[i+1] == '/') {
    int j = i + 2;
    while (j < x->cb && x->p[j] != '>') j++;
    if (j < x->cb) j++;
    Fmt_IndentTo(x->pC, iDepth);
    Fmt_Tok(x->pC, i, x->p + i, j - i);
    x->i = j;
    return 1;
  }

  return xmlElement(x, iDepth);
}

/*=============================================================================
  入口
=============================================================================*/

FMT_BOOL FmtXml_Do(FMTC *pC, int iFlags, FMTERR *pErr) {

  XP x;
  int i = 0;
  int bSawRoot = 0;

  memset(&x, 0, sizeof(XP));
  x.p  = pC->pIn;
  x.cb = pC->cbIn;
  x.pC = pC;

  if (x.cb >= 3 && (unsigned char)x.p[0] == 0xEF &&
                   (unsigned char)x.p[1] == 0xBB &&
                   (unsigned char)x.p[2] == 0xBF)
    i = 3;

  /* HTML 宽松模式：根元素是 html，或者根本是个片段（常见于 .html 片段）*/
  {
    int j = i;
    int bHtml = 0;
    if (j + 8 < x.cb && xMatchICase(x.p + j, 8, "<!DOCTYPE"))
      bHtml = xMatchICase(x.p + j, x.cb - j, "<!DOCTYPE html");
    x.bTolerant = bHtml ? 1 : 0;
    /* 调用方已经从语法方案知道这是 HTML（.html 文件），直接宽松 */
    if (iFlags & FMT_F_HTML)
      x.bTolerant = 1;
    if (!x.bTolerant) {
      /* 找第一个元素名，是 html 就当 HTML 处理 */
      int k = i;
      while (k < x.cb) {
        if (x.p[k] == '<' && k + 1 < x.cb && xIsNameStart((unsigned char)x.p[k+1])) {
          if (xMatchICase(x.p + k + 1, 4, "html") || xMatchICase(x.p + k + 1, 4, "body"))
            x.bTolerant = 1;
          break;
        }
        if (x.p[k] == '<' && k + 1 < x.cb &&
            (x.p[k+1] == '!' || x.p[k+1] == '?')) {
          /* 跳过前言 */
          int j2 = k + 2;
          while (j2 < x.cb && x.p[j2] != '>') j2++;
          k = (j2 < x.cb) ? j2 + 1 : x.cb;
          continue;
        }
        k++;
      }
    }
  }

  x.i = i;

  for (;;) {

    /* 跳过空白（顶层空白全部重新生成）*/
    while (x.i < x.cb && Fmt_IsSpace((unsigned char)x.p[x.i]))
      x.i++;

    if (x.i >= x.cb)
      break;

    if (bSawRoot)
      Fmt_Newline(pC);

    if (x.p[x.i] != '<') {
      /* 顶层裸文本：HTML 片段里会有，原样输出 */
      int j = x.i;
      while (j < x.cb && x.p[j] != '<') j++;
      Fmt_IndentTo(pC, 0);
      Fmt_Tok(pC, x.i, x.p + x.i, j - x.i);
      x.i = j;
      continue;
    }

    if (!xmlNode(&x, 0))
      return 0;

    bSawRoot = 1;
  }

  if (!bSawRoot) {
    Fmt_SetErr(pC, FMT_E_EMPTY, 0);
    return 0;
  }

  if (pErr) {
    pErr->iCode = FMT_E_OK;
    pErr->iPos  = 0;
  }

  return 1;
}

int FmtXml_Detect(const char *pIn, int cbIn) {

  int i = 0;

  if (cbIn >= 3 && (unsigned char)pIn[0] == 0xEF &&
                   (unsigned char)pIn[1] == 0xBB &&
                   (unsigned char)pIn[2] == 0xBF)
    i = 3;

  while (i < cbIn && Fmt_IsSpace((unsigned char)pIn[i])) i++;

  if (i >= cbIn || pIn[i] != '<')
    return 0;

  if (i + 1 >= cbIn)
    return 0;

  /* <?xml ... ?> / <!DOCTYPE ...> / <tag */
  if (pIn[i+1] == '?' || pIn[i+1] == '!')
    return 1;

  if (xIsNameStart((unsigned char)pIn[i+1]))
    return 1;

  return 0;
}

// End of Format_Xml.c
