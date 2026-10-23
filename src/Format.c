//=============================================================================
//
//  Format.c — 格式化核心的公共部分：输出器、位置映射、类型分派、工具
//
//  这里没有任何 Win32 依赖，tests/run/test_format.c 直接把它和
//  Format_*.c 一起编译就能跑。
//
//  输出器为什么要"只增不改"：
//    格式化只是把 token 之间的空白重新摆一遍，token 字节一个不动。
//    因此整条链路都只做「追加」——没有回退、没有重排，任何时刻缓冲区里
//    的前缀都是最终结果的合法前缀。这让出错时的定位变得简单：一旦发现
//    语法错，直接丢弃整个结果，原文毫发无伤。
//
//=============================================================================

#include <stdlib.h>
#include <string.h>

#include "Format_Int.h"

/*=============================================================================
  字符分类
=============================================================================*/

int Fmt_IsSpace(int ch) {
  return (ch == ' '  || ch == '\t' || ch == '\r' ||
          ch == '\n' || ch == '\v' || ch == '\f');
}

int Fmt_IsDigit(int ch) {
  return (ch >= '0' && ch <= '9');
}

int Fmt_IsAlpha(int ch) {
  return ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'));
}

int Fmt_IsWordChar(int ch) {
  return (Fmt_IsAlpha(ch) || Fmt_IsDigit(ch) || ch == '_' || ch == '$');
}

/* 与全大写关键字表比较（不含结尾 NUL）*/
int Fmt_EqICase(const char *p, int cb, const char *pszUpper) {
  int i;
  for (i = 0; i < cb; i++) {
    char ch = p[i];
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (pszUpper[i] == '\0' || ch != pszUpper[i]) return 0;
  }
  return (pszUpper[cb] == '\0');
}

/*=============================================================================
  输出器
=============================================================================*/

static int Fmt_Grow(FMTC *pC, int cbNeed) {

  int cbNew;
  char *pNew;

  if (pC->bOom)
    return 0;

  /* 预留一倍余量，避免逐字节扩容；空缓冲区给一个起步大小 */
  cbNew = (pC->cbMax > 0) ? pC->cbMax : 256;
  while (cbNew < cbNeed) {
    if (cbNew > (1 << 28)) {          /* 512 MB 封顶，防跑飞 */
      pC->bOom = 1;
      return 0;
    }
    cbNew *= 2;
  }

  pNew = (char *)realloc(pC->pOut, (size_t)cbNew);
  if (!pNew) {
    pC->bOom = 1;
    return 0;
  }

  pC->pOut  = pNew;
  pC->cbMax = cbNew;
  return 1;
}

void Fmt_Put(FMTC *pC, const char *p, int cb) {

  if (cb <= 0 || pC->bOom)
    return;

  if (pC->cbOut + cb > pC->cbMax) {
    if (!Fmt_Grow(pC, pC->cbOut + cb))
      return;
  }

  memcpy(pC->pOut + pC->cbOut, p, (size_t)cb);
  pC->cbOut += cb;
  pC->bLineStart  = 0;
  pC->iLineIndent = 0;          /* 这行已经有内容了 */
}

void Fmt_PutC(FMTC *pC, char ch) {
  char t = ch;
  Fmt_Put(pC, &t, 1);
}

/* 换行。已经在行首就什么都不做 —— 这条幂等性让各格式器不必关心
   "上一个 token 是不是刚好写过换行"，省掉一大票边界判断。 */
void Fmt_Newline(FMTC *pC) {

  if (pC->bMinify)
    return;

  if (pC->bLineStart)
    return;

  Fmt_Put(pC, pC->szEol, pC->cchEol);
  pC->bLineStart  = 1;
  pC->iLineIndent = 0;
}

/* 强制换行：压缩模式下也写。只有"行注释之后"需要 —— 少了这个换行，
   压缩会把注释后面的整段内容都吃进注释里。 */
void Fmt_ForceNewline(FMTC *pC) {
  Fmt_Put(pC, pC->szEol, pC->cchEol);
  pC->bLineStart  = 1;
  pC->iLineIndent = 0;
}

void Fmt_IndentTo(FMTC *pC, int iDepth) {

  int n;

  if (pC->bMinify || !pC->bLineStart)
    return;

  if (iDepth < 0) iDepth = 0;

  /* 已经缩进够了就什么都不做 —— 这样"先缩进、再断行、再缩进"
     不会写出两倍缩进，也不会把已经写好的缩进再写一遍 */
  n = iDepth * pC->iIndent;
  if (n <= pC->iLineIndent)
    return;

  while (pC->iLineIndent < n) {
    int iNext = pC->iLineIndent + 1;   /* Fmt_PutC 会把计数清 0，先算好 */
    Fmt_PutC(pC, ' ');
    pC->iLineIndent = iNext;
  }

  pC->bLineStart = 1;           /* 缩进本身不算"这行有内容" */
}

static int Fmt_MapReserve(FMTMAP *pMap, int cNeed);

/* 写一个 token，并记下它在输入/输出里的位置，供光标还原用 */
void Fmt_Tok(FMTC *pC, int iInOff, const char *p, int cb) {

  if (cb <= 0 || pC->bOom)
    return;

  if (pC->pMap && Fmt_MapReserve(pC->pMap, 1)) {
    pC->pMap->piIn [pC->pMap->c] = iInOff;
    pC->pMap->piOut[pC->pMap->c] = pC->cbOut;
    pC->pMap->piLen[pC->pMap->c] = cb;
    pC->pMap->c++;
  }

  Fmt_Put(pC, p, cb);
}

void Fmt_Space(FMTC *pC) {

  if (pC->bMinify)
    return;

  Fmt_PutC(pC, ' ');
}

void Fmt_SetErr(FMTC *pC, int iCode, int iPos) {

  /* 只记第一个错误：后面的都是它的连锁反应，报出来反而误导 */
  if (pC->iErrCode == FMT_E_OK) {
    pC->iErrCode = iCode;
    pC->iErrPos  = iPos;
  }
}

int Fmt_HasErr(FMTC *pC) {
  return (pC->iErrCode != FMT_E_OK);
}

/*=============================================================================
  位置映射
=============================================================================*/

static int Fmt_MapReserve(FMTMAP *pMap, int cNeed) {

  int cMax;
  int *piIn, *piOut, *piLen;

  if (pMap->c + cNeed <= pMap->cMax)
    return 1;

  cMax = (pMap->cMax > 0) ? pMap->cMax : 64;
  while (cMax < pMap->c + cNeed) cMax *= 2;

  piIn  = (int *)realloc(pMap->piIn,  (size_t)cMax * sizeof(int));
  piOut = (int *)realloc(pMap->piOut, (size_t)cMax * sizeof(int));
  piLen = (int *)realloc(pMap->piLen, (size_t)cMax * sizeof(int));

  if (!piIn || !piOut || !piLen)
    return 0;

  pMap->piIn  = piIn;
  pMap->piOut = piOut;
  pMap->piLen = piLen;
  pMap->cMax  = cMax;
  return 1;
}

void Fmt_MapFree(FMTMAP *p) {
  if (!p) return;
  if (p->piIn)  free(p->piIn);
  if (p->piOut) free(p->piOut);
  if (p->piLen) free(p->piLen);
  p->piIn = p->piOut = p->piLen = NULL;
  p->c = p->cMax = 0;
}

int Fmt_MapCaret(const FMTMAP *pMap, int iInCaret, int cbIn, int cbOut) {

  int lo, hi, mid, best = -1;

  if (!pMap || pMap->c <= 0)
    return iInCaret;

  /* 二分找最后一个 token 起点 <= 光标 */
  lo = 0;
  hi = pMap->c - 1;
  while (lo <= hi) {
    mid = (lo + hi) / 2;
    if (pMap->piIn[mid] <= iInCaret) {
      best = mid;
      lo = mid + 1;
    }
    else {
      hi = mid - 1;
    }
  }

  if (best < 0)
    return 0;

  {
    int iDelta = iInCaret - pMap->piIn[best];
    if (iDelta > pMap->piLen[best])
      iDelta = pMap->piLen[best];
    if (iDelta < 0)
      iDelta = 0;
    {
      int iOut = pMap->piOut[best] + iDelta;
      if (iOut > cbOut) iOut = cbOut;
      return iOut;
    }
  }
}

/*=============================================================================
  工具
=============================================================================*/

void Fmt_ResultFree(FMTRESULT *p) {
  if (!p) return;
  if (p->p) free(p->p);
  p->p = NULL;
  p->cb = 0;
}

void Fmt_OffsetToLineCol(const char *p, int cb, int iPos, int *piLine, int *piCol) {

  int iLine = 0;
  int iCol  = 0;
  int i;

  if (iPos > cb) iPos = cb;
  if (iPos < 0)  iPos = 0;

  for (i = 0; i < iPos; i++) {
    if (p[i] == '\r') {
      iLine++;
      iCol = 0;
      if (i + 1 < iPos && p[i + 1] == '\n')
        i++;
    }
    else if (p[i] == '\n') {
      iLine++;
      iCol = 0;
    }
    else {
      iCol++;
    }
  }

  if (piLine) *piLine = iLine;
  if (piCol)  *piCol  = iCol;
}

const char *Fmt_ErrName(int iCode) {

  switch (iCode) {
    case FMT_E_OK:                  return "ok";
    case FMT_E_NOMEM:               return "out-of-memory";
    case FMT_E_EMPTY:               return "empty-document";
    case FMT_E_UNEXPECTED_EOF:      return "unexpected-end-of-input";
    case FMT_E_UNEXPECTED_CHAR:     return "unexpected-character";
    case FMT_E_EXPECTED_KEY:        return "expected-key";
    case FMT_E_EXPECTED_COLON:      return "expected-colon";
    case FMT_E_EXPECTED_VALUE:      return "expected-value";
    case FMT_E_EXPECTED_COMMA:      return "expected-comma";
    case FMT_E_TRAILING_DATA:       return "trailing-data";
    case FMT_E_UNTERMINATED_STRING: return "unterminated-string";
    case FMT_E_UNTERMINATED_COMMENT:return "unterminated-comment";
    case FMT_E_TOO_DEEP:            return "nesting-too-deep";
    case FMT_E_TOO_BIG:             return "input-too-big";
    case FMT_E_TAG_MISMATCH:        return "tag-mismatch";
    case FMT_E_UNTERMINATED_TAG:    return "unterminated-tag";
    case FMT_E_UNBALANCED:          return "unbalanced-brackets";
  }
  return "unknown";
}

/*=============================================================================
  主流程
=============================================================================*/

static void Fmt_InitCtx(FMTC *pC, const char *pIn, int cbIn,
                        const FMTSTYLE *pStyle, int iFlags, FMTMAP *pMap) {

  memset(pC, 0, sizeof(FMTC));

  pC->pIn     = pIn;
  pC->cbIn    = cbIn;
  pC->iFlags  = iFlags;
  pC->bMinify = (iFlags & FMT_F_MINIFY) ? 1 : 0;
  pC->pMap    = pMap;
  pC->iErrCode = FMT_E_OK;
  pC->bLineStart = 1;

  pC->szEol  = (pStyle && pStyle->szEol && pStyle->szEol[0]) ? pStyle->szEol : "\r\n";
  pC->cchEol = (int)strlen(pC->szEol);

  pC->iIndent = (pStyle && pStyle->iIndent > 0) ? pStyle->iIndent : 2;
  if (pC->iIndent < FMT_INDENT_MIN) pC->iIndent = FMT_INDENT_MIN;
  if (pC->iIndent > FMT_INDENT_MAX) pC->iIndent = FMT_INDENT_MAX;
}

static FMT_BOOL Fmt_Run(int iKind, const char *pIn, int cbIn,
                        const FMTSTYLE *pStyle, int iFlags,
                        FMTRESULT *pOut, FMTMAP *pMap, FMTERR *pErr) {

  FMTC C;
  FMT_BOOL bOk = 0;

  if (pErr) {
    pErr->iPos  = 0;
    pErr->iCode = FMT_E_OK;
  }
  if (pOut) {
    pOut->p  = NULL;
    pOut->cb = 0;
  }
  if (pMap) {
    pMap->piIn = pMap->piOut = pMap->piLen = NULL;
    pMap->c = pMap->cMax = 0;
  }

  if (!pIn || cbIn <= 0) {
    if (pErr) pErr->iCode = FMT_E_EMPTY;
    return 0;
  }
  if (cbIn > FMT_MAX_INPUT) {
    if (pErr) pErr->iCode = FMT_E_TOO_BIG;
    return 0;
  }

  /* 映射表按"平均 6 字节一个 token"估算容量，省掉反复扩容 */
  if (pMap && !Fmt_MapReserve(pMap, cbIn / 6 + 16)) {
    if (pErr) pErr->iCode = FMT_E_NOMEM;
    return 0;
  }

  Fmt_InitCtx(&C, pIn, cbIn, pStyle, iFlags, pMap);

  switch (iKind) {
    case FMT_KIND_JSON: bOk = FmtJson_Do(&C, iFlags, pErr); break;
    case FMT_KIND_XML:  bOk = FmtXml_Do (&C, iFlags, pErr); break;
    case FMT_KIND_HTML: bOk = FmtXml_Do (&C, iFlags | FMT_F_HTML, pErr); break;
    case FMT_KIND_SQL:  bOk = FmtSql_Do (&C, iFlags, pErr); break;
    case FMT_KIND_INI:  bOk = FmtIni_Do (&C, iFlags, pErr); break;
    default:
      if (pErr) pErr->iCode = FMT_E_UNEXPECTED_CHAR;
      bOk = 0;
      break;
  }

  if (C.bOom) {
    if (pErr) { pErr->iCode = FMT_E_NOMEM; pErr->iPos = 0; }
    bOk = 0;
  }
  else if (!bOk || Fmt_HasErr(&C)) {
    if (pErr && pErr->iCode == FMT_E_OK) {
      pErr->iCode = C.iErrCode ? C.iErrCode : FMT_E_UNEXPECTED_CHAR;
      pErr->iPos  = C.iErrPos;
    }
    bOk = 0;
  }

  if (!bOk) {
    if (C.pOut) { free(C.pOut); C.pOut = NULL; }
    if (pMap && pMap->piIn) {
      free(pMap->piIn); free(pMap->piOut); free(pMap->piLen);
      pMap->piIn = pMap->piOut = pMap->piLen = NULL;
      pMap->c = pMap->cMax = 0;
    }
    return 0;
  }

  /* 收尾：先裁掉末尾的所有空白（格式化过程中难免留下空行尾巴），
     再按输入原来有没有行尾，补回恰好一个 —— 文件本来以换行结尾，
     格式化后也以换行结尾，不会被悄悄吃掉一个字节。
     format 与 minify 走同一条路，所以两条不变量都不受影响。 */
  {
    int bEndedWithEol = 0;
    int k = cbIn;

    while (k > 0 && (pIn[k-1] == ' ' || pIn[k-1] == 0x09)) k--;
    if (k > 0 && (pIn[k-1] == 0x0D || pIn[k-1] == 0x0A))
      bEndedWithEol = 1;

    while (C.cbOut > 0 && Fmt_IsSpace((unsigned char)C.pOut[C.cbOut - 1]))
      C.cbOut--;

    if (bEndedWithEol && C.cbOut > 0)
      Fmt_Put(&C, C.szEol, C.cchEol);   /* 末尾换行不算 token，不进映射表 */
  }

  /* 收尾：去掉最后一个换行（格式化结果以换行结尾时，缩进会留空行尾巴）*/
  if (pOut) {
    pOut->p  = C.pOut;
    pOut->cb = C.cbOut;
  }
  else if (C.pOut) {
    free(C.pOut);
  }

  return 1;
}

FMT_BOOL Fmt_Format(int iKind, const char *pIn, int cbIn, const FMTSTYLE *pStyle,
                    FMTRESULT *pOut, FMTMAP *pMap, FMTERR *pErr) {
  return Fmt_Run(iKind, pIn, cbIn, pStyle, 0, pOut, pMap, pErr);
}

FMT_BOOL Fmt_Minify(int iKind, const char *pIn, int cbIn, const FMTSTYLE *pStyle,
                    FMTRESULT *pOut, FMTMAP *pMap, FMTERR *pErr) {
  return Fmt_Run(iKind, pIn, cbIn, pStyle, FMT_F_MINIFY, pOut, pMap, pErr);
}

FMT_BOOL Fmt_Validate(int iKind, const char *pIn, int cbIn, FMTERR *pErr) {

  FMTRESULT R;
  FMT_BOOL bOk = Fmt_Format(iKind, pIn, cbIn, NULL, &R, NULL, pErr);
  Fmt_ResultFree(&R);
  return bOk;
}

/* 内容嗅探：语法方案给不出结论时的兜底 */
int Fmt_DetectKind(const char *pIn, int cbIn) {

  if (!pIn || cbIn <= 0)
    return FMT_KIND_NONE;

  if (FmtJson_Detect(pIn, cbIn)) return FMT_KIND_JSON;
  if (FmtXml_Detect (pIn, cbIn)) return FMT_KIND_XML;
  if (FmtSql_Detect (pIn, cbIn)) return FMT_KIND_SQL;
  if (FmtIni_Detect (pIn, cbIn)) return FMT_KIND_INI;

  return FMT_KIND_NONE;
}

// End of Format.c
