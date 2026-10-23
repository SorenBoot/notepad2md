//=============================================================================
//
//  Format_Int.h — 格式化核心的内部约定（各 *Format_*.c 共用）
//
//  这里放"输出器"和"字符分类"。所有格式器只通过 FMTC 写输出，
//  绝不直接碰缓冲区，这样：
//    · 缩进 / 行尾 / 映射 / 越界检查只在一个地方实现
//    · 压缩模式只是把"写缩进"和"写换行"变成空操作，格式器代码不用分叉
//
//=============================================================================

#ifndef FORMAT_INT_H
#define FORMAT_INT_H

#include "Format.h"

/* 单行放得下的阈值：短数组/短对象保持一行比竖排好读 */
#define FMT_SHORT_LINE  80

/* 运行标志 */
#define FMT_F_MINIFY  0x01   /* 压缩模式 */
#define FMT_F_HTML    0x02   /* 按 HTML 宽松模式处理（允许 void 元素）*/

/*---------------------------------------------------------------------------
  输出上下文
---------------------------------------------------------------------------*/

typedef struct {

  /* 输入 */
  const char *pIn;
  int         cbIn;

  /* 输出（动态增长）*/
  char *pOut;
  int   cbOut;
  int   cbMax;
  int   bOom;          /* 扩容失败：后续写入全部丢弃，最后统一报 FMT_E_NOMEM */

  /* 风格 */
  const char *szEol;
  int         cchEol;
  int         iIndent;  /* 已夹紧 */

  /* 状态 */
  int  iDepth;          /* 当前嵌套深度（缩进用）*/
  int  bLineStart;      /* 1 = 位于行首（下一个 token 前要写缩进）*/
  int  iLineIndent;     /* 本行已写的缩进列数 */
  int  bMinify;
  int  iFlags;

  /* 错误 */
  int  iErrPos;
  int  iErrCode;

  /* 位置映射，可为 NULL */
  FMTMAP *pMap;

} FMTC;

/*---------------------------------------------------------------------------
  输出
---------------------------------------------------------------------------*/

void Fmt_Put(FMTC *pC, const char *p, int cb);   /* 原样写字节，解除行首状态 */
void Fmt_PutC(FMTC *pC, char ch);
void Fmt_Newline(FMTC *pC);                      /* 压缩模式/已在行首时为空操作 */
void Fmt_ForceNewline(FMTC *pC);                 /* 强制换行（压缩模式也写）*/
void Fmt_IndentTo(FMTC *pC, int iDepth);         /* 仅在行首时写缩进 */

/* 写一个 token，并记录它在输入中的位置（iInOff）以便还原光标 */
void Fmt_Tok(FMTC *pC, int iInOff, const char *p, int cb);

/* 写"一个空格"，但压缩模式下不写；用于 token 之间的可省空白 */
void Fmt_Space(FMTC *pC);

void Fmt_SetErr(FMTC *pC, int iCode, int iPos);
int  Fmt_HasErr(FMTC *pC);

/*---------------------------------------------------------------------------
  字符分类（自己实现，不受 setlocale 影响）
---------------------------------------------------------------------------*/

int Fmt_IsSpace(int ch);     /* ' ' \t \r \n \v \f */
int Fmt_IsDigit(int ch);
int Fmt_IsAlpha(int ch);
int Fmt_IsWordChar(int ch);  /* 字母 / 数字 / _ / $ */

/* 不分大小写的比较：等长才算相等 */
int Fmt_EqICase(const char *p, int cb, const char *pszUpper);

/*---------------------------------------------------------------------------
  各格式器（内部）
---------------------------------------------------------------------------*/

FMT_BOOL FmtJson_Do(FMTC *pC, int iFlags, FMTERR *pErr);
FMT_BOOL FmtXml_Do (FMTC *pC, int iFlags, FMTERR *pErr);
FMT_BOOL FmtSql_Do (FMTC *pC, int iFlags, FMTERR *pErr);
FMT_BOOL FmtIni_Do (FMTC *pC, int iFlags, FMTERR *pErr);
FMT_BOOL FmtLog_Do (FMTC *pC, int iFlags, FMTERR *pErr);

/* 各格式器的内容嗅探，返回 0/1 */
int FmtJson_Detect(const char *pIn, int cbIn);
int FmtXml_Detect (const char *pIn, int cbIn);
int FmtSql_Detect (const char *pIn, int cbIn);
int FmtIni_Detect (const char *pIn, int cbIn);

#endif // FORMAT_INT_H

// End of Format_Int.h
