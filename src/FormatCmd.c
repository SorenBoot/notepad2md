/******************************************************************************
*
*
* notepad2md
*
* FormatCmd.c
*   GUI glue for the document formatter (Format.c and friends).
*
*   The formatter itself is pure C with no Win32 dependency (see Format.h),
*   so everything that touches the editor lives here: read the text, pick the
*   kind from the current syntax scheme, call the engine, put the result back
*   as a single undoable step, restore the caret, translate error codes.
*
*   Behaviour contract (unchanged from the approved design):
*     - a selection is formatted in place; with no selection the whole
*       document is formatted
*     - the file's own line ending is reused, indentation comes from the
*       current tab settings
*     - on any syntax error the document is left byte-for-byte untouched
*       and the caret jumps to the offending line/column
*
*
******************************************************************************/
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include "scintilla.h"
#include "scilexer.h"
#include "notepad2.h"
#include "edit.h"
#include "styles.h"
#include "dialogs.h"
#include "helpers.h"
#include "resource.h"
#include "Format.h"
#include "FormatCmd.h"


extern HWND  hwndMain;
extern HWND  hwndEdit;
extern int   iIndentWidth;   /* 当前文件的缩进覆盖，0 = 用 Tab 宽度 */
extern int   iTabWidth;

/* 语法方案 -> 格式化器的 rid（不是 lexer ID：JSON 借用了 SCLEX_CPP）*/
#define RID_HTML     63001
#define RID_XML      63002
#define RID_INI      63015
#define RID_SQL      63018
#define RID_JSON     63022
#define RID_LOG      63028

/* 8 MB 起提醒确认，16 MB 以上直接拒绝 —— 引擎上限(FMT_MAX_INPUT)也设为 16 MB */
#define FMT_BIGFILE_WARN  (8*1024*1024)
#define FMT_BIGFILE_LIMIT (16*1024*1024)


//=============================================================================
//
//  FormatKindFromLexer()
//
static int FormatKindFromLexer(void)
{
  extern PEDITLEXER pLexCurrent;

  if (!pLexCurrent)
    return FMT_KIND_NONE;

  switch (pLexCurrent->rid) {
    case RID_JSON: return FMT_KIND_JSON;
    case RID_XML:  return FMT_KIND_XML;
    case RID_HTML: return FMT_KIND_HTML;  /* XML 的宽松变体：void 元素可以不写 /> */
    case RID_SQL:  return FMT_KIND_SQL;
    case RID_INI:  return FMT_KIND_INI;
    /* RID_LOG（日志方案）不提供格式化：日志只做配色，
       格式化会改坏日志的行结构，且 README 已声明不支持 */
  }

  return FMT_KIND_NONE;
}


//=============================================================================
//
//  FormatLexerIsPlain()
//
//  当前语法方案是不是"认不出类型"的纯文本。只有这种情况下才允许按
//  内容去猜类型 —— 否则编辑一个 .c 文件时内容万一长得像 JSON，
//  一按快捷键就把源码重排了。已知但未支持的语言直接报"不支持"。
//
static int FormatLexerIsPlain(void)
{
  extern PEDITLEXER pLexCurrent;
  extern EDITLEXER  lexDefault;

  return (pLexCurrent == &lexDefault);
}


//=============================================================================
//
//  FormatErrMsgId()
//
static UINT FormatErrMsgId(int iCode)
{
  switch (iCode) {
    case FMT_E_NOMEM:                return IDS_FMT_ERR_NOMEM;
    case FMT_E_EMPTY:                return IDS_FMT_ERR_EMPTY;
    case FMT_E_TOO_BIG:              return IDS_FMT_ERR_TOOBIG;
    case FMT_E_TOO_DEEP:             return IDS_FMT_ERR_TOODEEP;
    case FMT_E_UNTERMINATED_STRING:  return IDS_FMT_ERR_STRING;
    case FMT_E_UNTERMINATED_COMMENT: return IDS_FMT_ERR_COMMENT;
    case FMT_E_TAG_MISMATCH:
    case FMT_E_UNTERMINATED_TAG:     return IDS_FMT_ERR_TAG;
    case FMT_E_UNBALANCED:           return IDS_FMT_ERR_BRACKET;
  }
  return IDS_FMT_ERR_SYNTAX;
}


//=============================================================================
//
//  FormatReportError()
//
//  报错并把光标挪到出错的那一行，方便直接改。
//
static void FormatReportError(HWND hwnd,int iBase,const char *p,int cb,FMTERR *pErr)
{
  WCHAR  szMsg[512];
  int    iLine, iCol;

  if (!GetString(FormatErrMsgId(pErr->iCode),szMsg,COUNTOF(szMsg)))
    szMsg[0] = L'\0';

  Fmt_OffsetToLineCol(p,cb,pErr->iPos,&iLine,&iCol);

  if (pErr->iCode != FMT_E_NOMEM && pErr->iCode != FMT_E_TOO_BIG) {
    int iPos = iBase + min(pErr->iPos,cb);
    SendMessage(hwnd,SCI_GOTOPOS,(WPARAM)iPos,0);
    EditEnsureSelectionVisible(hwnd);
  }

  MsgBox(MBINFO,IDS_FMT_FAILED,szMsg,iLine + 1,iCol + 1);
}


//=============================================================================
//
//  FormatDoCmd()
//
//  iCmd: IDM_FORMAT_DOC / IDM_FORMAT_MINIFY / IDM_FORMAT_VALIDATE
//
void FormatDoCmd(HWND hwnd,int iCmd)
{
  int        iSelStart, iSelEnd, iCurPos, iAnchorPos;
  int        cbIn, iKind, iIndent, iEolMode;
  int        bSelection;
  char      *pIn;
  char      *pszText;
  FMTRESULT  res;
  FMTMAP     map;
  FMTSTYLE   style;
  FMTERR     err;
  FMT_BOOL   bOk;

  iSelStart  = (int)SendMessage(hwnd,SCI_GETSELECTIONSTART,0,0);
  iSelEnd    = (int)SendMessage(hwnd,SCI_GETSELECTIONEND,0,0);
  iCurPos    = (int)SendMessage(hwnd,SCI_GETCURRENTPOS,0,0);
  iAnchorPos = (int)SendMessage(hwnd,SCI_GETANCHOR,0,0);

  bSelection = (iSelEnd > iSelStart);

  if (!bSelection) {
    iSelStart = 0;
    iSelEnd   = (int)SendMessage(hwnd,SCI_GETLENGTH,0,0);
  }

  cbIn = iSelEnd - iSelStart;

  if (cbIn <= 0) {
    MsgBox(MBINFO,IDS_FMT_ERR_EMPTY);
    return;
  }

  /* 8 MB 以上不猜：先弹窗确认；16 MB 以上直接拒绝，免得赌输了惨 */
  if (cbIn > FMT_BIGFILE_LIMIT) {
    MsgBox(MBINFO, IDS_FMT_ERR_TOOBIG);
    return;
  }
  if (cbIn > FMT_BIGFILE_WARN &&
      InfoBox(MBYESNO, L"FmtBigFile", IDS_FMT_BIGFILE_CONFIRM) != IDYES) {
    return;
  }

  /* --- 取文本 --- */
  pIn = (char *)GlobalAlloc(GPTR,cbIn + 2);
  if (!pIn)
    return;

  {
    struct TextRange tr;
    int cbGot;
    tr.chrg.cpMin = iSelStart;
    tr.chrg.cpMax = iSelEnd;
    tr.lpstrText  = pIn;
    cbGot = (int)SendMessage(hwnd,SCI_GETTEXTRANGE,0,(LPARAM)&tr);
    if (cbGot > 0) cbIn = cbGot;
  }

  /* --- 判断类型：看当前语法方案，认不出时再按内容猜 --- */
  iKind = FormatKindFromLexer();

  if (iKind == FMT_KIND_NONE && FormatLexerIsPlain()) {
    /* 纯文本（.txt / 无扩展名）：内容像 JSON/XML/SQL/INI 就按那个来，
       否则按日志整理空白。日志方案（.log）本身不格式化，走「不支持」 */
    iKind = Fmt_DetectKind(pIn,cbIn);
    if (iKind == FMT_KIND_NONE)
      iKind = FMT_KIND_LOG;
  }

  if (iKind == FMT_KIND_NONE) {
    GlobalFree(pIn);
    MsgBox(MBINFO,IDS_FMT_UNSUPPORTED);
    return;
  }

  /* --- 仅是校验 --- */
  if (iCmd == IDM_FORMAT_VALIDATE) {

    err.iCode = FMT_E_OK;
    err.iPos  = 0;

    if (Fmt_Validate(iKind,pIn,cbIn,&err))
      MsgBox(MBINFO,IDS_FMT_OK);
    else
      FormatReportError(hwnd,iSelStart,pIn,cbIn,&err);

    GlobalFree(pIn);
    return;
  }

  /* --- 风格：沿用文件的行尾与当前的缩进设置 --- */
  iEolMode = (int)SendMessage(hwnd,SCI_GETEOLMODE,0,0);
  switch (iEolMode) {
    case SC_EOL_LF:   style.szEol = "\n";   break;
    case SC_EOL_CR:   style.szEol = "\r";   break;
    default:          style.szEol = "\r\n"; break;
  }

  /* 缩进宽度：跟 Notepad2 自己的算法一致 —— 文件的缩进覆盖优先，
     没有就用 Tab 宽度（SCI_SETINDENT / SCI_SETTABWIDTH 也是这么给的）*/
  iIndent = iIndentWidth ? iIndentWidth : iTabWidth;
  if (iIndent < FMT_INDENT_MIN) iIndent = FMT_INDENT_MIN;
  if (iIndent > FMT_INDENT_MAX) iIndent = FMT_INDENT_MAX;
  style.iIndent = iIndent;

  res.p  = NULL;
  res.cb = 0;
  memset(&map,0,sizeof(FMTMAP));
  err.iCode = FMT_E_OK;
  err.iPos  = 0;

  if (iCmd == IDM_FORMAT_MINIFY)
    bOk = Fmt_Minify(iKind,pIn,cbIn,&style,&res,&map,&err);
  else
    bOk = Fmt_Format(iKind,pIn,cbIn,&style,&res,&map,&err);

  if (!bOk) {
    if (err.iCode == FMT_E_OK) err.iCode = FMT_E_UNEXPECTED_CHAR;
    FormatReportError(hwnd,iSelStart,pIn,cbIn,&err);
    Fmt_ResultFree(&res);
    GlobalFree(pIn);
    return;
  }

  /* --- 没变化就别碰文档（省一次空的撤销步）--- */
  if (res.cb == cbIn && memcmp(res.p,pIn,(size_t)cbIn) == 0) {
    Fmt_ResultFree(&res);
    Fmt_MapFree(&map);
    GlobalFree(pIn);
    return;
  }

  GlobalFree(pIn);

  /* --- 写回：一次撤销，光标按 token 映射回到同一处 --- */
  {
    int cbTarget = iSelEnd - iSelStart;

    /* 光标落在被格式化的区间里，就按 token 序号搬过去；
       落在区间外（比如格式化选区时光标在别处）就随文本平移 */
    int iNewCur = iCurPos;
    int iNewAnchor = iAnchorPos;

    if (iCurPos >= iSelStart && iCurPos <= iSelEnd)
      iNewCur = iSelStart + Fmt_MapCaret(&map,iCurPos - iSelStart,cbTarget,res.cb);
    else if (iCurPos > iSelEnd)
      iNewCur = iCurPos + (res.cb - cbTarget);

    if (iAnchorPos >= iSelStart && iAnchorPos <= iSelEnd)
      iNewAnchor = iSelStart + Fmt_MapCaret(&map,iAnchorPos - iSelStart,cbTarget,res.cb);
    else if (iAnchorPos > iSelEnd)
      iNewAnchor = iAnchorPos + (res.cb - cbTarget);

    /* REPLACETARGET 要明确长度，正好省掉结尾 NUL 的麻烦 */
    pszText = (char *)GlobalAlloc(GPTR,res.cb + 2);
    if (pszText) {
      memcpy(pszText,res.p,(size_t)res.cb);

      SendMessage(hwnd,SCI_BEGINUNDOACTION,0,0);
      SendMessage(hwnd,SCI_SETTARGETSTART,(WPARAM)iSelStart,0);
      SendMessage(hwnd,SCI_SETTARGETEND,(WPARAM)iSelEnd,0);
      SendMessage(hwnd,SCI_REPLACETARGET,(WPARAM)res.cb,(LPARAM)pszText);
      SendMessage(hwnd,SCI_SETSEL,(WPARAM)iNewAnchor,(LPARAM)iNewCur);
      SendMessage(hwnd,SCI_ENDUNDOACTION,0,0);

      GlobalFree(pszText);
      EditEnsureSelectionVisible(hwnd);
    }
  }

  Fmt_ResultFree(&res);
  Fmt_MapFree(&map);
}

// End of FormatCmd.c
