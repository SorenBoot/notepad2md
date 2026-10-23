//=============================================================================
//
//  Format_Sql.c — SQL 的格式化、压缩、校验
//
//  同样只动空白。SQL 里"会被格式化毁掉"的东西比 JSON 还多，所以下手很轻：
//
//    · 字符串（'...' 里的 '' 转义）、双引号标识符、反引号、[方括号]、
//      $$..$$ / $tag$..$tag$ 里的字节一律不动
//    · -- 行注释后强制换行，压缩时也一样（否则会把后面的语句吃进注释）
//    · 关键字大小写不动 —— 有人全大写有人全小写，那是风格不是排版
//    · 括号里的普通表达式不拆行，只规整空格；只有子查询（括号里出现
//      SELECT/WITH）才展开成缩进块
//    · 只在这些子句前断行：SELECT / FROM / WHERE / GROUP BY / ORDER BY /
//      HAVING / LIMIT / UNION / JOIN / ON / SET / VALUES / RETURNING ...
//    · 一条语句一行一条，语句之间空一行
//
//  "括号前留不留空格"看用户原来怎么写的（count(*) 与 IN (1, 2) 都保留），
//  这样幂等性天然成立：我们的输出会让同一判断得出同一结果。
//
//=============================================================================

#include <stdlib.h>
#include <string.h>

#include "Format_Int.h"

enum {
  ST_EOF = 0,
  ST_BAD,
  ST_WORD,
  ST_NUMBER,
  ST_STRING,        /* '...' "..." `...` [...] $$...$$ */
  ST_LPAREN,
  ST_RPAREN,
  ST_COMMA,
  ST_SEMI,
  ST_OP,            /* 其它符号：= <> || :: + - * / ... */
  ST_LINECOMMENT,
  ST_BLOCKCOMMENT
};

typedef struct {

  const char *p;
  int cb;
  int i;

  int iTokStart;
  int iTokLen;
  int bWsBefore;     /* 本 token 之前有空白 */
  int bEolBefore;

} SC;

/* 会另起一行的关键字 */
static const char *aszBreak[] = {
  "SELECT","FROM","WHERE","GROUP","ORDER","HAVING","LIMIT","OFFSET",
  "UNION","EXCEPT","INTERSECT","INSERT","UPDATE","DELETE","VALUES","SET",
  "RETURNING","WITH","JOIN","LEFT","RIGHT","FULL","CROSS","INNER","WINDOW",
  "FETCH","ON","AND","OR"
};

/* 这些词跟在某些词后面时不另起一行（GROUP BY / UNION ALL / LEFT JOIN）*/
typedef struct {
  const char *pszWord;
  const char *pszAfter;
} KWNOKEEP;

static const KWNOKEEP aKeep[] = {
  { "BY",    "GROUP" },
  { "BY",    "ORDER" },
  { "BY",    "PARTITION" },
  { "ALL",   "UNION" },
  { "FROM",  "DELETE" },
  { "FROM",  "DISTINCT" },
  { "INTO",  "INSERT" },
  { "JOIN",  "LEFT" },
  { "JOIN",  "RIGHT" },
  { "JOIN",  "FULL" },
  { "JOIN",  "CROSS" },
  { "JOIN",  "INNER" },
  { "JOIN",  "OUTER" },
  { "OUTER", "LEFT" },
  { "OUTER", "RIGHT" },
  { "OUTER", "FULL" },
  { "SET",   "CHARACTER" },
  { "VALUES","DEFAULT" }
};

/*=============================================================================
  词法
=============================================================================*/

static int sIsWordStart(int c) {
  return (Fmt_IsAlpha(c) || c == '_' || c >= 0x80);
}

static int sIsWordChar(int c) {
  return (Fmt_IsAlpha(c) || Fmt_IsDigit(c) || c == '_' || c == '$' || c >= 0x80);
}

/* '...' —— 内部 '' 表示一个单引号；反斜杠转义也认（MySQL 风格）*/
static int sScanQuoted(SC *q, int iQuote, int bBackslash) {

  int i = q->i + 1;

  while (i < q->cb) {
    if (bBackslash && q->p[i] == '\\') { i += 2; continue; }
    if (q->p[i] == iQuote) {
      if (i + 1 < q->cb && q->p[i + 1] == iQuote) { i += 2; continue; }
      i++;
      q->iTokLen = i - q->iTokStart;
      q->i = i;
      return ST_STRING;
    }
    i++;
  }

  /* 跨行也算：SQL 字符串确实可以跨行 */
  q->iTokLen = i - q->iTokStart;
  q->i = i;
  return ST_BAD;
}

/* $tag$ ... $tag$ */
static int sScanDollar(SC *q) {

  int iTag = q->i;
  int i = q->i + 1;

  while (i < q->cb && sIsWordChar((unsigned char)q->p[i])) i++;
  if (i >= q->cb || q->p[i] != '$')
    return 0;                        /* 不是美元引号，交给普通符号处理 */

  {
    int cbTag = i + 1 - iTag;        /* 含首尾两个 $ */
    int j = i + 1;

    while (j + cbTag <= q->cb) {
      if (q->p[j] == '$' && memcmp(q->p + j, q->p + iTag, (size_t)cbTag) == 0) {
        j += cbTag;
        q->iTokLen = j - q->iTokStart;
        q->i = j;
        return ST_STRING;
      }
      j++;
    }
  }

  q->iTokLen = q->cb - q->iTokStart;
  q->i = q->cb;
  return ST_BAD;
}

static int sScan(SC *q) {

  int ch;

  q->bWsBefore = 0;
  q->bEolBefore = 0;

  for (;;) {
    if (q->i >= q->cb) {
      q->iTokStart = q->cb;
      q->iTokLen = 0;
      return ST_EOF;
    }
    ch = (unsigned char)q->p[q->i];
    if (!Fmt_IsSpace(ch))
      break;
    q->bWsBefore = 1;
    if (ch == '\r' || ch == '\n')
      q->bEolBefore = 1;
    q->i++;
  }

  q->iTokStart = q->i;
  q->iTokLen = 1;
  ch = (unsigned char)q->p[q->i];

  switch (ch) {
    case '(': q->i++; return ST_LPAREN;
    case ')': q->i++; return ST_RPAREN;
    case ',': q->i++; return ST_COMMA;
    case ';': q->i++; return ST_SEMI;
    case '\'': return sScanQuoted(q, '\'', 1);
    case '"':  return sScanQuoted(q, '"',  0);
    case '`':  return sScanQuoted(q, '`',  0);
    case '[': {
      int i = q->i + 1;
      while (i < q->cb && q->p[i] != ']') i++;
      if (i < q->cb) i++;
      q->iTokLen = i - q->iTokStart;
      q->i = i;
      return (i <= q->cb && q->p[i-1] == ']') ? ST_STRING : ST_BAD;
    }
  }

  if (ch == '-' && q->i + 1 < q->cb && q->p[q->i + 1] == '-') {
    int i = q->i + 2;
    while (i < q->cb && q->p[i] != '\n' && q->p[i] != '\r') i++;
    q->iTokLen = i - q->iTokStart;
    q->i = i;
    return ST_LINECOMMENT;
  }

  if (ch == '/' && q->i + 1 < q->cb && q->p[q->i + 1] == '*') {
    int i = q->i + 2;
    while (i + 1 < q->cb && !(q->p[i] == '*' && q->p[i + 1] == '/')) i++;
    if (i + 1 >= q->cb) {
      q->iTokLen = q->cb - q->iTokStart;
      q->i = q->cb;
      return ST_BAD;
    }
    i += 2;
    q->iTokLen = i - q->iTokStart;
    q->i = i;
    return ST_BLOCKCOMMENT;
  }

  if (ch == '$') {
    int r = sScanDollar(q);
    if (r)
      return r;
  }

  if (sIsWordStart(ch)) {
    int i = q->i;
    while (i < q->cb && sIsWordChar((unsigned char)q->p[i])) i++;
    q->iTokLen = i - q->iTokStart;
    q->i = i;
    return ST_WORD;
  }

  if (Fmt_IsDigit(ch) || (ch == '.' && q->i + 1 < q->cb &&
                          Fmt_IsDigit((unsigned char)q->p[q->i + 1]))) {
    int i = q->i;
    while (i < q->cb && (Fmt_IsDigit((unsigned char)q->p[i]) ||
                         Fmt_IsAlpha((unsigned char)q->p[i]) ||
                         q->p[i] == '.' || q->p[i] == '_' ||
                         ((q->p[i] == '+' || q->p[i] == '-') &&
                          i > q->i &&
                          (q->p[i-1] == 'e' || q->p[i-1] == 'E')))) i++;
    q->iTokLen = i - q->iTokStart;
    q->i = i;
    return ST_NUMBER;
  }

  /* 其它符号，按连续同类合并（->> :: <= >= <> != || @@ 等）*/
  {
    int i = q->i + 1;
    static const char *apsz[] = { "->>", "->", "::", "<=", ">=", "<>", "!=", "||", "&&", "@@" };
    int k;
    for (k = 0; k < (int)(sizeof(apsz)/sizeof(apsz[0])); k++) {
      int n = (int)strlen(apsz[k]);
      if (i - 1 + n <= q->cb && memcmp(q->p + q->i, apsz[k], (size_t)n) == 0) {
        i = q->i + n;
        break;
      }
    }
    q->iTokLen = i - q->iTokStart;
    q->i = i;
    return ST_OP;
  }
}

/*=============================================================================
  关键字
=============================================================================*/

static int sIsBreak(const char *p, int cb) {

  int i;
  for (i = 0; i < (int)(sizeof(aszBreak)/sizeof(aszBreak[0])); i++)
    if (Fmt_EqICase(p, cb, aszBreak[i]) && (int)strlen(aszBreak[i]) == cb)
      return 1;
  return 0;
}

/* 本 token 前面要不要断行（prev 是上一个词的字节）*/
static int sKeepInline(const char *p, int cb, const char *pPrev, int cbPrev) {

  int i;
  if (cbPrev <= 0) return 0;
  for (i = 0; i < (int)(sizeof(aKeep)/sizeof(aKeep[0])); i++)
    if (Fmt_EqICase(p, cb, aKeep[i].pszWord) &&
        (int)strlen(aKeep[i].pszWord) == cb &&
        Fmt_EqICase(pPrev, cbPrev, aKeep[i].pszAfter) &&
        (int)strlen(aKeep[i].pszAfter) == cbPrev)
      return 1;
  return 0;
}

/* 括号里出现 SELECT/WITH（本层）→ 是个子查询，要展开 */
static int sParenIsBlock(SC *q) {

  int iSave      = q->i;
  int iSaveStart = q->iTokStart;
  int iSaveLen   = q->iTokLen;
  int iSaveWs    = q->bWsBefore;
  int iSaveEol   = q->bEolBefore;
  int nDepth     = 0;
  int bBlock     = 0;

  for (;;) {
    int t = sScan(q);
    if (t == ST_EOF || t == ST_BAD)
      break;
    if (t == ST_LPAREN) { nDepth++; continue; }
    if (t == ST_RPAREN) {
      if (nDepth == 0) break;
      nDepth--;
      continue;
    }
    if (nDepth == 0 && t == ST_WORD &&
        (Fmt_EqICase(q->p + q->iTokStart, q->iTokLen, "SELECT") ||
         Fmt_EqICase(q->p + q->iTokStart, q->iTokLen, "WITH"))) {
      bBlock = 1;
      break;
    }
  }

  q->i         = iSave;
  q->iTokStart = iSaveStart;
  q->iTokLen   = iSaveLen;
  q->bWsBefore = iSaveWs;
  q->bEolBefore= iSaveEol;

  return bBlock;
}

/*=============================================================================
  输出
=============================================================================*/

typedef struct {

  FMTC *pC;
  int   iDepth;
  int   aBlock[FMT_MAX_DEPTH + 1];   /* 每层括号是不是子查询 */
  int   iPrevKind;
  int   bPrevWasAtom;
  int   bPrevTight;                  /* 上一个 token 是 . :: -> ->> */
  int   bPrevOpenParen;
  char  szPrev[64];                  /* 上一个词（判断 GROUP BY 用）*/
  int   cbPrev;

} SQ;

/* . :: -> ->> 两侧不留空格 */
static int sIsTightOp(const char *p, int cb) {
  if (cb == 1 && p[0] == '.') return 1;
  if (cb == 2 && p[0] == ':' && p[1] == ':') return 1;
  if (cb == 2 && p[0] == '-' && p[1] == '>') return 1;
  if (cb == 3 && p[0] == '-' && p[1] == '-' && p[2] == '>') return 1;
  if (cb == 2 && p[0] == '@' && p[1] == '@') return 1;
  return 0;
}

/* 这个 token 前面要不要空格（不含"括号前"那种看用户写法的）*/
static int sWantSpace(SQ *q, int t, const char *p, int cb) {

  if (t == ST_RPAREN || t == ST_COMMA || t == ST_SEMI)
    return 0;
  if (q->bPrevOpenParen || q->bPrevTight)
    return 0;
  if (t == ST_OP && sIsTightOp(p, cb))
    return 0;

  return 1;
}

/* 写分隔：行首不用写；压缩模式下"两个实心 token"之间仍然必须有空格，
   否则 SELECTa 这种拼在一起的垃圾就出来了 */
static void sSep(SQ *q, int t, const char *p, int cb, int bWant) {

  int bNeed = bWant;

  if (q->bPrevWasAtom &&
      (t == ST_WORD || t == ST_NUMBER || t == ST_STRING))
    bNeed = 1;

  if (!bNeed || q->pC->bLineStart)
    return;

  if (q->pC->bMinify)
    Fmt_PutC(q->pC, ' ');
  else
    Fmt_Space(q->pC);
}

/* 断行（压缩模式下什么都不做，让后面的 sSep 补那个空格）*/
static void sBreak(SQ *q) {
  if (q->pC->bMinify)
    return;
  Fmt_Newline(q->pC);
  Fmt_IndentTo(q->pC, q->iDepth);
}

/* 当前是否处在"可以断子句"的位置：所有未闭合的括号都是子查询。
   aBlock[0] 是虚拟的语句层，真正的括号从 1 开始 */
static int sClauseAllowed(SQ *q) {

  int i;
  for (i = 1; i <= q->iDepth; i++)
    if (!q->aBlock[i])
      return 0;
  return 1;
}

FMT_BOOL FmtSql_Do(FMTC *pC, int iFlags, FMTERR *pErr) {

  SQ q;
  SC s;
  SC *pS = &s;
  int t;
  int bFirstTok = 1;

  (void)iFlags;

  memset(&q, 0, sizeof(SQ));
  q.pC = pC;
  q.aBlock[0] = 1;

  {
    memset(pS, 0, sizeof(SC));
    pS->p  = pC->pIn;
    pS->cb = pC->cbIn;
    pS->i  = 0;

    if (pS->cb >= 3 && (unsigned char)pS->p[0] == 0xEF &&
                      (unsigned char)pS->p[1] == 0xBB &&
                      (unsigned char)pS->p[2] == 0xBF)
      pS->i = 3;

    for (;;) {

      t = sScan(pS);

      if (t == ST_EOF)
        break;

      if (t == ST_BAD) {
        if (pS->iTokLen > 0 &&
            (pS->p[pS->iTokStart] == 0x27 || pS->p[pS->iTokStart] == '"' ||
             pS->p[pS->iTokStart] == '`'  || pS->p[pS->iTokStart] == '[' ||
             pS->p[pS->iTokStart] == '$'))
          Fmt_SetErr(pC, FMT_E_UNTERMINATED_STRING, pS->iTokStart);
        else
          Fmt_SetErr(pC, FMT_E_UNTERMINATED_COMMENT, pS->iTokStart);
        return 0;
      }

      switch (t) {

        case ST_LINECOMMENT:
          if (!bFirstTok)
            sBreak(&q);
          Fmt_IndentTo(pC, q.iDepth);
          Fmt_Tok(pC, pS->iTokStart, pS->p + pS->iTokStart, pS->iTokLen);
          Fmt_ForceNewline(pC);          /* 压缩时也必须换行 */
          break;

        case ST_LPAREN: {
          int bBlock = sParenIsBlock(pS);
          if (q.iDepth < FMT_MAX_DEPTH) {
            q.iDepth++;
            q.aBlock[q.iDepth] = bBlock;
          }
          /* 括号前留不留空格，遵循用户原来的写法：count(*) 与 IN (1) 都保留 */
          sSep(&q, t, "(", 1, pS->bWsBefore);
          Fmt_Tok(pC, pS->iTokStart, "(", 1);
          if (bBlock)
            sBreak(&q);
          break;
        }

        case ST_RPAREN: {
          int bBlock = q.aBlock[q.iDepth];
          if (q.iDepth > 0)
            q.iDepth--;
          if (bBlock)
            sBreak(&q);                 /* 退到外层缩进再写 ')' */
          Fmt_Tok(pC, pS->iTokStart, ")", 1);
          break;
        }

        case ST_COMMA:
          Fmt_Tok(pC, pS->iTokStart, ",", 1);
          if (q.iDepth > 0 && q.aBlock[q.iDepth])
            sBreak(&q);
          break;

        case ST_SEMI:
          Fmt_Tok(pC, pS->iTokStart, ";", 1);
          if (!pC->bMinify) {
            Fmt_Newline(pC);
            Fmt_ForceNewline(pC);        /* 语句之间空一行 */
          }
          break;

        case ST_WORD: {
          const char *pTok = pS->p + pS->iTokStart;
          int cbTok = pS->iTokLen;
          int bBreak = sIsBreak(pTok, cbTok) &&
                       sClauseAllowed(&q) &&
                       !sKeepInline(pTok, cbTok, q.szPrev, q.cbPrev);
          if (bBreak && !bFirstTok)
            sBreak(&q);
          /* 断行之后再 sSep 一次：压缩模式下断行是空的，
             那个空格还得补，否则会写出 namefrom 这种连体词 */
          sSep(&q, t, pTok, cbTok, bFirstTok ? 0 : sWantSpace(&q, t, pTok, cbTok));
          Fmt_Tok(pC, pS->iTokStart, pTok, cbTok);
          break;
        }

        default: {
          const char *pTok = pS->p + pS->iTokStart;
          int cbTok = pS->iTokLen;
          sSep(&q, t, pTok, cbTok, bFirstTok ? 0 : sWantSpace(&q, t, pTok, cbTok));
          Fmt_Tok(pC, pS->iTokStart, pTok, cbTok);
          break;
        }
      }

      /* 记住上一个词，供 GROUP BY 之类的判断 */
      if (t == ST_WORD && pS->iTokLen < (int)sizeof(q.szPrev)) {
        memcpy(q.szPrev, pS->p + pS->iTokStart, (size_t)pS->iTokLen);
        q.cbPrev = pS->iTokLen;
      }
      else if (t != ST_BLOCKCOMMENT) {
        q.cbPrev = 0;
      }

      q.bPrevTight     = (t == ST_OP && sIsTightOp(pS->p + pS->iTokStart, pS->iTokLen));
      q.bPrevOpenParen = (t == ST_LPAREN);
      q.bPrevWasAtom   = (t == ST_WORD || t == ST_NUMBER || t == ST_STRING);
      q.iPrevKind      = t;
      bFirstTok        = 0;
    }

  }

  if (q.iDepth != 0) {
    Fmt_SetErr(pC, FMT_E_UNBALANCED, pC->cbIn);
    return 0;
  }

  if (pErr) {
    pErr->iCode = FMT_E_OK;
    pErr->iPos  = 0;
  }

  return 1;
}

int FmtSql_Detect(const char *pIn, int cbIn) {

  static const char *apsz[] = {
    "SELECT","INSERT","UPDATE","DELETE","CREATE","ALTER","DROP","TRUNCATE",
    "WITH","MERGE","EXPLAIN","PRAGMA","BEGIN","COMMIT","GRANT","REVOKE","CALL"
  };
  int i = 0;
  int k;

  if (cbIn >= 3 && (unsigned char)pIn[0] == 0xEF &&
                   (unsigned char)pIn[1] == 0xBB &&
                   (unsigned char)pIn[2] == 0xBF)
    i = 3;

  /* 跳过空白与注释 */
  for (;;) {
    while (i < cbIn && Fmt_IsSpace((unsigned char)pIn[i])) i++;
    if (i + 1 < cbIn && pIn[i] == '-' && pIn[i+1] == '-') {
      i += 2;
      while (i < cbIn && pIn[i] != '\n' && pIn[i] != '\r') i++;
      continue;
    }
    if (i + 1 < cbIn && pIn[i] == '/' && pIn[i+1] == '*') {
      i += 2;
      while (i + 1 < cbIn && !(pIn[i] == '*' && pIn[i+1] == '/')) i++;
      i = (i + 1 < cbIn) ? i + 2 : cbIn;
      continue;
    }
    break;
  }

  if (i >= cbIn)
    return 0;

  {
    int iEnd = i;
    while (iEnd < cbIn && (Fmt_IsAlpha((unsigned char)pIn[iEnd]) ||
                           pIn[iEnd] == '_')) iEnd++;
    if (iEnd == i)
      return 0;
    for (k = 0; k < (int)(sizeof(apsz)/sizeof(apsz[0])); k++)
      if (Fmt_EqICase(pIn + i, iEnd - i, apsz[k]) &&
          (int)strlen(apsz[k]) == iEnd - i)
        return 1;
  }

  return 0;
}

// End of Format_Sql.c
