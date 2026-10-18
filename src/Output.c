/*===========================================================================
  notepad2md — 命令输出面板
  在编辑器底部开一条面板，把外部命令的标准输出/标准错误实时显示出来。
  输出里出现「文件:行号」时双击该行，编辑器跳到对应位置。
  设计取舍（刻意为之，不是偷懒）：
    - stdin 重定向到 NUL：交互式程序（ssh、python REPL）不会挂死，
      它们会因为读不到输入而退出，或自行弹出自己的控制台窗口。
    - 输出控件用 RichEdit（MSFTEDIT_CLASS），解析 ANSI 颜色转义，
      把 gcc / git / ls 的颜色还原出来。深色底是刻意的：
      亮黄、亮青这些色在浅色底上根本看不清。
    - 不是终端模拟器：不支持光标定位、全屏程序、交互输入。
      只服务于「跑个命令看结果、顺着错误行回去改代码」这个编辑器场景。
  ===========================================================================*/
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <richedit.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <strsafe.h>
#include <shlwapi.h>
#include "scintilla.h"
#include "Output.h"
#include "resource.h"
#include "Helpers.h"

/* exe 所在目录，来自 Notepad2.c。Output 模块不直接依赖 Notepad2.h，
   这里按需声明，避免把整个主程序头文件拖进来。 */
extern WCHAR g_wchWorkingDirectory[MAX_PATH];

/* COUNTOF 在 Notepad2 的 Dialogs.h 里定义，本模块独立编译故自带一份 */
#ifndef COUNTOF
#define COUNTOF(a) (sizeof(a)/sizeof(a[0]))
#endif
#define OUTPUT_MAX_TEXT   (512 * 1024)   /* 文本上限，超出从头截断 */
#define OUTPUT_MAX_LINES   4096          /* 可跳转行登记表容量 */
#define OUTPUT_PIPE_CHUNK  4096

/* 面板内部用的自定义消息 */
#define WM_APP_OUTPUT      (WM_APP + 0x071)   /* 追加文本 */
#define WM_APP_OUTPUT_GOTO (WM_APP + 0x072)   /* 双击跳转 */
static HINSTANCE g_hinst;
static HWND      g_hwndPanel = NULL;
static HWND      g_hwndEdit  = NULL;     /* 只读输出框 */
static HWND      g_hwndCombo = NULL;     /* 命令下拉 */
static ATOM      g_atomClass = 0;
static HWND      g_hwndOwner = NULL;
static WNDPROC   g_pfnEditProc = NULL;   /* 输出框原过程，子类化用 */
static WNDPROC   g_pfnComboProc = NULL;  /* 命令框原过程，子类化用 */
static HWND      g_hwndStatus = NULL;    /* 顶部信息条 */
static HFONT     g_hFontMono = NULL;     /* 输出区等宽字体 */
static BOOL      g_bFloating = FALSE;    /* 是否独立窗口 */
static RECT      g_rcFloat = {0,0,0,0};  /* 浮动时的窗口矩形（切回嵌入前记下） */
static OutputCloseHook g_pfnCloseHook = NULL;  /* 关闭回调 */
static void           *g_pCloseUser = NULL;
static HANDLE    g_hReadThread = NULL;
static HANDLE    g_hProc       = NULL;
static HANDLE    g_hOutRd      = NULL;
static volatile LONG g_lCancel = 0;
static BOOL      g_bRunning    = FALSE;

/* 一行输出对应一条记录：是否可跳转、目标文件与行号 */
typedef struct {
  BOOL  bGoto;
  WCHAR wszFile[MAX_PATH];
  int   iLine;
} LINEINFO;
static LINEINFO g_aLines[OUTPUT_MAX_LINES];
static int      g_nLines = 0;

/* 后面才定义的几个接口，这里前置声明，供上面的窗口过程使用 */
static const WCHAR *CurFileName(void);
void Output_RefreshStatus(void);
static void LayoutChildren(HWND hwnd, int cx, int cy);

/* ---------------------------------------------------------------------------
   登记一行：识别「文件:行号」格式，认不出就当普通行。
   支持两类最常见的：
     foo.c:23:5: error: ...        (GCC / clang)
     foo.c(23,5): error C2065: ... (MSVC)
   idx 是「输出框里的视觉行号」，由 DoAppend 逐行传入 ——
   这样双击时 EDIT 报上来的行号才能和登记表对上。
--------------------------------------------------------------------------- */
static void RegisterLine(int idx, const WCHAR *wszLine)
{
  LINEINFO *p;
  const WCHAR *q;
  const WCHAR *pStop;
  WCHAR wszFile[MAX_PATH];
  int   cch = 0;
  int   iLine = 0;
  WCHAR *pEnd = NULL;
  BOOL  bOk = FALSE;
  if (idx < 0 || idx >= OUTPUT_MAX_LINES)
    return;

  /* 每写一行就登记一次，认不出 file:line 的也占一个槽（bGoto=FALSE），
     保证登记表下标与输出框里的视觉行号严格一致 */
  p = &g_aLines[idx];
  memset(p,0,sizeof(*p));
  if (idx + 1 > g_nLines)
    g_nLines = idx + 1;
  if (!wszLine || !*wszLine)
    return;

  /* 行首文件名：扫到 ':'（gcc 风格）或 '('（MSVC 风格）为止。
     盘符冒号必须跳过：C:\diroo.c:23 的第一个 ':' 属于盘符，
     按它切只会得到 "C"，后面全盘皆输。 */
  q = wszLine;

  /* 从行首解析「文件名 + 行号」。
     形如  C:\My Dir\foo.c:23:5: error: ...
     - 文件名含空格，不能用空格分隔
     - 盘符冒号必须跳过
     - 行号是「第一个位于路径之后的 :数字」，不是最后一个
    （最后一个 :5 是列号，会把行号取错）
     做法：从位置 2 起找第一个「后面紧跟数字」的 ':' 或 '('。 */
  {
    const WCHAR *pBest = NULL;
    const WCHAR *p = q;
    int iStart = (iswalpha(q[0]) && q[1] == L':') ? 2 : 0;
    for (p = q + iStart; *p; p++) {
  if ((*p == L':' || *p == L'(') && iswdigit(p[1])) {
    pBest = p;
    break;
  }
    }
    if (pBest) {
  cch = (int)(pBest - q);
  if (cch > MAX_PATH - 1)
    cch = MAX_PATH - 1;
  memcpy(wszFile, q, (size_t)cch * sizeof(WCHAR));
  wszFile[cch] = 0;
  pStop = pBest;
    }
    else {
  wszFile[0] = 0;
  cch = 0;
  pStop = q;
    }
  }
have_stop:
  if (*pStop == L':') {
    iLine = (int)wcstol(pStop + 1,&pEnd,10);
    if (pEnd && pEnd != pStop + 1)
  bOk = TRUE;
  }
  else if (*pStop == L'(') {
    iLine = (int)wcstol(pStop + 1,&pEnd,10);
    if (pEnd && *pEnd == L',')
  bOk = TRUE;
  }
  /* 文件名里得带 '.' 或路径分隔，否则 "12:xx" 这类数字前缀会误判 */
  if (bOk && cch > 0 && iLine > 0 &&
  (StrChr(wszFile,L'.') || StrChr(wszFile,L'\\') ||
   StrChr(wszFile,L'/')))
  {
    p->bGoto = TRUE;
    lstrcpyn(p->wszFile,wszFile,MAX_PATH);
    p->iLine = iLine;
  }
}
/* ---------------------------------------------------------------------------
   子类化输出框：双击某行时，把该行索引发给主窗口去跳转。
--------------------------------------------------------------------------- */
static LRESULT CALLBACK OutputEditProc(HWND hwnd, UINT umsg,
                                   WPARAM wParam, LPARAM lParam)
{
  if (umsg == WM_LBUTTONDBLCLK) {
    /* 取当前选区起点所在的行，EDIT 的选区就是 caret 位置 */
    DWORD dwStart = 0, dwEnd = 0;
    SendMessage(hwnd,EM_GETSEL,(WPARAM)&dwStart,(LPARAM)&dwEnd);
    {
  int iLine = (int)SendMessage(hwnd,EM_LINEFROMCHAR,(WPARAM)dwStart,0);
  PostMessage(g_hwndOwner,WM_APP_OUTPUT_GOTO,0,(LPARAM)iLine);
    }
    return 0;
  }
  return CallWindowProc(g_pfnEditProc,hwnd,umsg,wParam,lParam);
}


/* ---------------------------------------------------------------------------
   子类化命令框：回车直接运行，不用离开键盘去点按钮。
--------------------------------------------------------------------------- */
static LRESULT CALLBACK OutputComboProc(HWND hwnd, UINT umsg,
                                    WPARAM wParam, LPARAM lParam)
{
  if (umsg == WM_KEYDOWN && wParam == VK_RETURN) {
    if (g_hwndPanel)
      PostMessage(g_hwndPanel,WM_COMMAND,
                  MAKEWPARAM(IDC_OUTPUT_RUN,BN_CLICKED),0);
    return 0;
  }
  return CallWindowProc(g_pfnComboProc,hwnd,umsg,wParam,lParam);
}
/* ---------------------------------------------------------------------------
   面板过程
--------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
   跨线程追加文本。
   读取线程不能直接 SendMessage 给 EDIT —— 那会与主线程的等待循环互相
   阻塞（读线程等主线程泵消息，主线程等读线程结束）。改为 PostMessage
   异步投递，由主线程在自己的消息循环里落地。
--------------------------------------------------------------------------- */
typedef struct {
  WCHAR *wsz;      /* 调用方分配、所有权转交 */
  BOOL   bNewLine;
} APPENDMSG;
static void PostAppend(const WCHAR *wszText, BOOL bNewLine)
{
  APPENDMSG *p = (APPENDMSG*)LocalAlloc(LPTR,sizeof(APPENDMSG));
  int cch;
  if (!p || !g_hwndPanel)
    return;
  cch = lstrlenW(wszText) + 1;
  p->wsz = (WCHAR*)LocalAlloc(LPTR,cch * sizeof(WCHAR));
  if (!p->wsz) {
    LocalFree(p);
    return;
  }
  lstrcpyW(p->wsz,wszText);
  p->bNewLine = bNewLine;
  if (!PostMessage(g_hwndPanel,WM_APP_OUTPUT,0,(LPARAM)p)) {
    LocalFree(p->wsz);
    LocalFree(p);
  }
}
/* 主线程里真正把文字写进 EDIT，并顺带登记这一行 */
static void HistoryAdd(const WCHAR *wszCmd);

/* ---------------------------------------------------------------------------
   颜色

   输出区是 RichEdit，可以逐段上色。取 VS 深色主题那套，配深色底。
   为什么必须深色底：ANSI 的亮黄、亮青、亮绿是按深色终端调的，
   放到浅色底上对比度极低，白字加黄等于看不见。
--------------------------------------------------------------------------- */
#define OUT_BG        RGB(0x1E,0x1E,0x1E)   /* 底色 */
#define OUT_FG        RGB(0xD4,0xD4,0xD4)   /* 默认前景 */
#define OUT_COLOR_CMD RGB(0x4E,0xC9,0xB0)   /* 命令行本身 */
#define OUT_COLOR_DIM RGB(0x80,0x80,0x80)   /* 分隔线、退出码 */

/* ANSI 16 色 → RGB（xterm 默认调色板） */
static const COLORREF g_aAnsi[16] = {
  RGB(0x00,0x00,0x00), RGB(0xCD,0x31,0x31), RGB(0x0D,0xBC,0x79), RGB(0xE5,0xE5,0x10),
  RGB(0x24,0x72,0xC8), RGB(0xBC,0x3F,0xBC), RGB(0x11,0xA8,0xCD), RGB(0xE5,0xE5,0xE5),
  RGB(0x66,0x66,0x66), RGB(0xF1,0x4C,0x4C), RGB(0x23,0xD1,0x8B), RGB(0xF5,0xF5,0x43),
  RGB(0x3B,0x8E,0xEA), RGB(0xD6,0x70,0xD6), RGB(0x29,0xB8,0xDB), RGB(0xFF,0xFF,0xFF)
};

/* 解析 SGR 参数串（ESC[...m 里 [...] 那一段），更新前景色/粗体。
   只处理最常用的：0 重置、1 粗体、22 取消粗体、30-37/90-97 前景色、
   39 默认前景。背景色 40-47 也收下 —— 否则某些工具用背景高亮标错行，
   我们不认就会把整段渲染成怪色。 */
static void ApplySgr(const WCHAR *wszParams, COLORREF *pFg, BOOL *pbBold,
                 COLORREF *pBg, BOOL *pbHasBg)
{
  const WCHAR *p = wszParams;
  if (!*p) {                     /* ESC[m == ESC[0m */
    *pFg = OUT_FG; *pbBold = FALSE; *pbHasBg = FALSE;
    return;
  }
  while (*p) {
    int n = 0;
    while (*p >= L'0' && *p <= L'9') { n = n * 10 + (*p - L'0'); p++; }
    if (*p == L';') p++;

    if (n == 0)       { *pFg = OUT_FG; *pbBold = FALSE; *pbHasBg = FALSE; }
    else if (n == 1)    *pbBold = TRUE;
    else if (n == 22)   *pbBold = FALSE;
    else if (n == 39) { *pFg = OUT_FG; *pbBold = FALSE; }
    else if (n == 49)   *pbHasBg = FALSE;
    else if (n >= 30 && n <= 37)  { *pFg = g_aAnsi[n - 30];      *pbBold = FALSE; }
    else if (n >= 90 && n <= 97)  { *pFg = g_aAnsi[n - 90 + 8];  *pbBold = FALSE; }
    else if (n >= 40 && n <= 47)  { *pBg = g_aAnsi[n - 40];      *pbHasBg = TRUE; }
    else if (n >= 100 && n <= 107){ *pBg = g_aAnsi[n - 100 + 8]; *pbHasBg = TRUE; }
    /* 其它（下划线、闪烁、256 色扩展）一律忽略：不认识就不动色，
   总比乱涂好 */
    if (!*p) break;
  }
}

/*
  把一行带 ANSI 转义的文本写进 RichEdit，并逐段上色。

  返回这一行去掉转义后的纯文本（供 RegisterLine 识别 file:line）——
  调用方提供缓冲。纯文本必须和显示内容一致，否则双击跳转的行号会错位。
*/
static void AppendColored(const WCHAR *wszIn, BOOL bNewLine,
                      WCHAR *wszPlain, int cchPlain)
{
  COLORREF fg = OUT_FG, bg = OUT_BG;
  BOOL bBold = FALSE, bHasBg = FALSE;
  int  nPlain = 0;
  const WCHAR *p = wszIn;

  if (cchPlain > 0) wszPlain[0] = L'\0';

  /* 先把光标移到末尾，之后每段都用 EM_REPLACESEL 追加 */
  {
    int iLen = GetWindowTextLengthW(g_hwndEdit);
    SendMessage(g_hwndEdit,EM_SETSEL,(WPARAM)iLen,(LPARAM)iLen);
  }

  while (*p) {
    if (*p == 0x1B && p[1] == L'[') {
  /* ESC [ ... m  —— 只认以 'm' 结尾的 SGR，其余（光标移动、清屏）
     连同参数一起丢掉，不把控制码当文字显示 */
  const WCHAR *q = p + 2;
  WCHAR wszParams[64];
  int   nParam = 0;
  while (*q && *q != L'm' && *q != L';' && !(*q >= L'0' && *q <= L'9') &&
         *q != L'?' && nParam < (int)COUNTOF(wszParams) - 1)
    q++;
  /* 收参数 */
  {
    const WCHAR *r = p + 2;
    nParam = 0;
    while (*r && *r != L'm' && nParam < (int)COUNTOF(wszParams) - 1) {
      if ((*r >= L'0' && *r <= L'9') || *r == L';')
        wszParams[nParam++] = *r;
      else if (*r != L'?')
        break;              /* 不是 SGR（比如 ESC[2J），别乱吃 */
      r++;
    }
    wszParams[nParam] = L'\0';
    if (*r == L'm') {
      ApplySgr(wszParams,&fg,&bBold,&bg,&bHasBg);
      p = r + 1;
      continue;             /* 转义吃掉，不显示 */
    }
  }
  /* 不是 SGR：整个转义序列跳过不显示，但不当成文字 */
  {
    const WCHAR *r = p + 1;
    if (*r == L'[') {
      r++;
      while (*r && !((*r >= L'@' && *r <= L'~')))
        r++;
      if (*r) r++;
      p = r;
      continue;
    }
  }
  /* 落单的 ESC：丢弃 */
  p++;
  continue;
    }

    /* 收集一段同色文字，一次写进去，避免逐字符发消息 */
    {
  WCHAR wszRun[1024];
  int   nRun = 0;
  while (*p && *p != 0x1B && nRun < (int)COUNTOF(wszRun) - 1) {
    wszRun[nRun++] = *p;
    if (nPlain < cchPlain - 1)
      wszPlain[nPlain++] = *p;
    p++;
  }
  wszRun[nRun] = L'\0';
  if (nRun > 0) {
    CHARFORMAT2W cf;
    memset(&cf,0,sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR | CFM_BOLD;
    cf.crTextColor = fg;
    cf.dwEffects   = bBold ? CFE_BOLD : 0;
    SendMessage(g_hwndEdit,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&cf);
    SendMessage(g_hwndEdit,EM_REPLACESEL,FALSE,(LPARAM)wszRun);
  }
    }
  }

  if (bNewLine) {
    CHARFORMAT2W cf;
    memset(&cf,0,sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR | CFM_BOLD;
    cf.crTextColor = OUT_FG;
    cf.dwEffects   = 0;
    SendMessage(g_hwndEdit,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&cf);
    SendMessage(g_hwndEdit,EM_REPLACESEL,FALSE,(LPARAM)L"\r\n");
  }

  if (cchPlain > 0)
    wszPlain[nPlain] = L'\0';
}

/*
  bStyle：0 = 普通输出（按内容上色），1 = 命令行（青色），2 = 灰色（分隔线、退出码）
*/
static void DoAppendEx(const WCHAR *wszText, BOOL bNewLine, int iStyle)
{
  WCHAR wszPlain[2048];
  int   iLen;
  int   iLineIdx;

  if (!g_hwndEdit || !wszText)
    return;

  /* 超限时从头砍一半，防止无限增长 */
  iLen = GetWindowTextLengthW(g_hwndEdit);
  if (iLen > OUTPUT_MAX_TEXT) {
    SendMessage(g_hwndEdit,EM_SETSEL,0,OUTPUT_MAX_TEXT / 2);
    SendMessage(g_hwndEdit,EM_REPLACESEL,TRUE,(LPARAM)L"");
    iLen = GetWindowTextLengthW(g_hwndEdit);
  }

  /* 本行索引：写入前先把插入点定位到末尾并记下该位置，
     写完后用这个位置反查行号。

     不要用「末尾字符行号 +1」推算：RichEdit 的 EM_LINEFROMCHAR
     对 CRLF 末尾和文末位置的处理与普通 EDIT 不一致，实测会偏一位，
     双击跳转就跑到下一行去了。用固定位置反查才是准的。 */
  iLineIdx = 0;
  {
    int iEnd = GetWindowTextLengthW(g_hwndEdit);
    SendMessage(g_hwndEdit,EM_SETSEL,(WPARAM)iEnd,(LPARAM)iEnd);
    if (iEnd > 0)
  iLineIdx = (int)SendMessage(g_hwndEdit,EM_LINEFROMCHAR,(WPARAM)iEnd,0);
  }

  if (iStyle == 0) {
    AppendColored(wszText,bNewLine,wszPlain,COUNTOF(wszPlain));
  }
  else {
    /* 命令行/分隔线：整行一个颜色。仍然过一遍 ANSI 剥离 ——
   命令本身可能带转义（echo 给下游上色那种），
   直接把 ESC 原字符画到控件里会变成豆腐块。 */
    WCHAR wszStripped[2048];
    const WCHAR *pSrc = wszText;
    int nOut = 0;

    while (*pSrc && nOut < (int)COUNTOF(wszStripped) - 1) {
  if (*pSrc == 0x1B) {
    /* 跳过整个转义序列 */
    const WCHAR *r = pSrc + 1;
    if (*r == L'[') {
      r++;
      while (*r && !(*r >= L'@' && *r <= L'~'))
        r++;
      if (*r) r++;
    }
    else if (*r) {
      r++;
    }
    pSrc = r;
    continue;
  }
  wszStripped[nOut++] = *pSrc++;
    }
    wszStripped[nOut] = L'\0';

    {
  CHARFORMAT2W cf;
  memset(&cf,0,sizeof(cf));
  cf.cbSize = sizeof(cf);
  cf.dwMask = CFM_COLOR | CFM_BOLD;
  cf.crTextColor = (iStyle == 1) ? OUT_COLOR_CMD : OUT_COLOR_DIM;
  cf.dwEffects   = (iStyle == 1) ? CFE_BOLD : 0;
  SendMessage(g_hwndEdit,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&cf);
  SendMessage(g_hwndEdit,EM_REPLACESEL,FALSE,(LPARAM)wszStripped);
  if (bNewLine) {
    cf.crTextColor = OUT_FG;
    cf.dwEffects   = 0;
    SendMessage(g_hwndEdit,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&cf);
    SendMessage(g_hwndEdit,EM_REPLACESEL,FALSE,(LPARAM)L"\r\n");
  }
    }
    lstrcpynW(wszPlain,wszStripped,COUNTOF(wszPlain));
  }

  RegisterLine(iLineIdx,wszPlain);
}

static void DoAppend(const WCHAR *wszText, BOOL bNewLine)
{
  DoAppendEx(wszText,bNewLine,0);
}
static void EnableButtons(BOOL bRunning)
{
  EnableWindow(GetDlgItem(g_hwndPanel,IDC_OUTPUT_RUN), !bRunning);
  EnableWindow(GetDlgItem(g_hwndPanel,IDC_OUTPUT_STOP), bRunning);
}
/* ---------------------------------------------------------------------------
   独立窗口的外观。嵌入时是光板子窗口（边框由主窗口提供），
   独立时要补上标题栏和可缩放边框，否则没法拖大小。
--------------------------------------------------------------------------- */
static DWORD PanelStyle(void)
{
  if (g_bFloating)
    return WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
           WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
  return WS_CHILD | WS_CLIPCHILDREN;
}


/* ---------------------------------------------------------------------------
   切换嵌入 / 独立。

   换父窗口用 SetParent()。RichEdit 的内容存在控件自己的缓冲区里，
   跟着窗口走，所以文本、滚动位置、着色都不丢 —— 不需要保存再重灌。

   唯一要自己补的是窗口样式：子窗口和顶层窗口的样式位不兼容，
   必须用 SetWindowLongPtr 整体换掉，否则浮动后没有标题栏、
   或者停靠后变成一个乱飘的顶层小窗。
--------------------------------------------------------------------------- */
void Output_SetFloating(BOOL bFloat)
{
  if (!g_hwndPanel || !IsWindow(g_hwndPanel))
    return;
  if (bFloat == g_bFloating)
    return;

  /* 从浮动切回嵌入前，把浮动窗口的矩形记下来，下次浮动时还原 */
  if (!bFloat && g_bFloating)
    GetWindowRect(g_hwndPanel, &g_rcFloat);

  g_bFloating = bFloat;

  /* 先脱离原来的父子关系，再改样式，最后挂到新父窗口。
     顺序反了会让窗口在改样式瞬间按旧样式重算一次位置，闪一下。 */
  SetParent(g_hwndPanel, NULL);
  SetWindowLongPtr(g_hwndPanel, GWL_STYLE, PanelStyle());

  if (g_bFloating) {
    RECT rc;
    int x, y, cx, cy;

    /* 记住上次浮动的窗口位置和大小（由主窗口从 ini 恢复进来）；
       没有记录时浮到主窗口内部。 */
    if (g_rcFloat.right > g_rcFloat.left && g_rcFloat.bottom > g_rcFloat.top) {
      x  = g_rcFloat.left;
      y  = g_rcFloat.top;
      cx = g_rcFloat.right - g_rcFloat.left;
      cy = g_rcFloat.bottom - g_rcFloat.top;
    }
    else {
      GetWindowRect(g_hwndOwner, &rc);
      x  = rc.left + 80;
      y  = rc.top + 80;
      cx = 720;
      cy = 380;
    }

    SetWindowPos(g_hwndPanel, HWND_TOP, x, y, cx, cy,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    SetWindowTextW(g_hwndPanel, L"命令输出");
  }
  else {
    SetParent(g_hwndPanel, g_hwndOwner);
    SetWindowLongPtr(g_hwndPanel, GWL_STYLE, PanelStyle());
    SetWindowPos(g_hwndPanel, NULL, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  /* 主窗口重新布局：嵌入时给它留位置，独立时把那块空间收回来 */
  SendMessage(g_hwndOwner, WM_SIZE, SIZE_RESTORED, 0);
}


BOOL Output_IsFloating(void)
{
  return g_bFloating;
}


void Output_SetCloseHook(OutputCloseHook pfnHook, void *pUser)
{
  g_pfnCloseHook = pfnHook;
  g_pCloseUser  = pUser;
}


void Output_SetFloatRect(const RECT *prc)
{
  if (prc && prc->right > prc->left && prc->bottom > prc->top)
    g_rcFloat = *prc;
}


void Output_GetFloatRect(RECT *prc)
{
  if (!prc)
    return;

  /* 浮动中就直接取实时矩形，用户可能刚拖过还没切回嵌入。
     顺手更新 g_rcFloat，这样切回嵌入后记录的仍是最后的位置。 */
  if (g_bFloating && g_hwndPanel && IsWindow(g_hwndPanel)) {
    GetWindowRect(g_hwndPanel, prc);
    g_rcFloat = *prc;
  }
  else
    *prc = g_rcFloat;
}


/* ---------------------------------------------------------------------------
   子控件布局。WM_SIZE 和浮动/嵌入切换都调它，避免两套坐标各写一遍。
--------------------------------------------------------------------------- */
static void LayoutChildren(HWND hwnd, int cx, int cy)
{

  /* 布局统一用同一套边距和间距，避免各控件对不齐：
     外边距 6px，行间距 4px，按钮等宽 58px。 */
  int nPad    = 6;
  int hStatus = 18;
  int hRow    = 24;
  int nBtnW   = 58;
  int nGap    = 4;
  int y       = nPad;
  int xRun, xStop, xClear;

  /* 状态条 */
  if (g_hwndStatus)
    MoveWindow(g_hwndStatus,nPad,y,cx - 2 * nPad,hStatus,TRUE);
  y += hStatus + nGap;

  /* 命令行 + 右侧三个按钮，从右往左排 */
  xClear = cx - nPad - nBtnW;
  xStop  = xClear - nGap - nBtnW;
  xRun   = xStop  - nGap - nBtnW;
  if (g_hwndCombo)
    MoveWindow(g_hwndCombo,nPad,y,xRun - nPad - nGap,hRow - 2,TRUE);
  if (GetDlgItem(hwnd,IDC_OUTPUT_RUN))
    MoveWindow(GetDlgItem(hwnd,IDC_OUTPUT_RUN),xRun,y,nBtnW,hRow - 2,TRUE);
  if (GetDlgItem(hwnd,IDC_OUTPUT_STOP))
    MoveWindow(GetDlgItem(hwnd,IDC_OUTPUT_STOP),xStop,y,nBtnW,hRow - 2,TRUE);
  if (GetDlgItem(hwnd,IDC_OUTPUT_CLEAR))
    MoveWindow(GetDlgItem(hwnd,IDC_OUTPUT_CLEAR),xClear,y,nBtnW,hRow - 2,TRUE);
  y += hRow;

  /* 输出区占剩下的全部 */
  if (g_hwndEdit)
    MoveWindow(g_hwndEdit,nPad,y,cx - 2 * nPad,cy - y - nPad,TRUE);
}


static LRESULT CALLBACK OutputWndProc(HWND hwnd, UINT umsg,
                                      WPARAM wParam, LPARAM lParam)
{
  switch (umsg) {
    case WM_CREATE: {
      HDC hdc;
      HFONT hFont;
      LOGFONTW lf;

      g_hwndPanel = hwnd;

      /* 顶部信息条：告诉用户 %FILE% 当前指向谁，免得猜 */
      g_hwndStatus = CreateWindowEx(0,L"STATIC",NULL,
                       WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
                       0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_STATUS,g_hinst,NULL);

      /* 目录框已移除：默认就跟随当前文件，%DIR% 也能显式指定，
         多一个输入框只是多一处要填的地方。 */

      g_hwndCombo = CreateWindowEx(0,L"COMBOBOX",NULL,
                     WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_AUTOHSCROLL |
                     WS_VSCROLL | WS_TABSTOP,
                     0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_CMD,g_hinst,NULL);

      CreateWindowEx(0,L"BUTTON",L"运行",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                     0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_RUN,g_hinst,NULL);

      /* 停止按钮初始禁用 —— 没命令在跑时点了没意义，禁用比点了没反应好 */
      CreateWindowEx(0,L"BUTTON",L"停止",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP |
                     WS_DISABLED,
                     0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_STOP,g_hinst,NULL);

      CreateWindowEx(0,L"BUTTON",L"清空",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                     0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_CLEAR,g_hinst,NULL);

      /* 输出区：用 RichEdit 而不是 EDIT —— 只有 RichEdit 能逐段上色，
         才能还原 gcc / git / ls 的 ANSI 颜色。
         必须加载 msftedit.dll，否则 MSFTEDIT_CLASS 建不出来。 */
      LoadLibraryW(L"Msftedit.dll");
      g_hwndEdit = CreateWindowExW(WS_EX_CLIENTEDGE,MSFTEDIT_CLASS,NULL,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                    ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                    ES_NOHIDESEL,
                    0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_LOG,g_hinst,NULL);

      /* 子类化输出框：捕获双击跳转 + 回车直接运行命令 */
      if (g_hwndEdit) {
        g_pfnEditProc = (WNDPROC)SetWindowLongPtr(g_hwndEdit,GWLP_WNDPROC,
                                                 (LONG_PTR)OutputEditProc);
      }
      g_pfnComboProc = (WNDPROC)SetWindowLongPtr(g_hwndCombo,GWLP_WNDPROC,
                                                 (LONG_PTR)OutputComboProc);

      /* 界面字体：跟随系统，和主程序其他控件一致 */
      hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
      SendMessage(g_hwndStatus,WM_SETFONT,(WPARAM)hFont,FALSE);
      SendMessage(g_hwndCombo, WM_SETFONT,(WPARAM)hFont,FALSE);
      SendMessage(GetDlgItem(hwnd,IDC_OUTPUT_RUN),  WM_SETFONT,(WPARAM)hFont,FALSE);
      SendMessage(GetDlgItem(hwnd,IDC_OUTPUT_STOP), WM_SETFONT,(WPARAM)hFont,FALSE);
      SendMessage(GetDlgItem(hwnd,IDC_OUTPUT_CLEAR),WM_SETFONT,(WPARAM)hFont,FALSE);

      /* 输出区用等宽字体：错误信息的
            foo.c:7:5: error: ...
               7 |     return a;
                 |     ^~~
         列号、插入符才能对齐，否则一片散沙。取不到 Consolas 就用系统等宽。 */
      hdc = GetDC(hwnd);
      memset(&lf,0,sizeof(lf));
      lf.lfHeight = -MulDiv(9,GetDeviceCaps(hdc,LOGPIXELSY),72);
      lf.lfWeight = FW_NORMAL;
      lf.lfCharSet = DEFAULT_CHARSET;
      lf.lfQuality = CLEARTYPE_QUALITY;
      lstrcpyW(lf.lfFaceName,L"Consolas");
      g_hFontMono = CreateFontIndirectW(&lf);
      if (!g_hFontMono) {
        /* 回退：等宽字体（中英文都存在的选择） */
        lstrcpyW(lf.lfFaceName,L"Courier New");
        g_hFontMono = CreateFontIndirectW(&lf);
      }
      ReleaseDC(hwnd,hdc);
      if (g_hFontMono)
        SendMessage(g_hwndEdit,WM_SETFONT,(WPARAM)g_hFontMono,FALSE);

      /* RichEdit 背景色：EM_SETBKGNDCOLOR 是 RichEdit 独有的，
         普通 EDIT 没有。底色必须跟 ANSI 调色板配套，否则亮黄亮青看不清。 */
      if (g_hwndEdit) {
        SendMessage(g_hwndEdit,EM_SETBKGNDCOLOR,0,(LPARAM)OUT_BG);
        /* 默认文字色：不先设一次，空文档时插入点后面是系统默认黑字，
           在深色底上直接隐形 */
        {
          CHARFORMAT2W cf;
          memset(&cf,0,sizeof(cf));
          cf.cbSize = sizeof(cf);
          cf.dwMask = CFM_COLOR;
          cf.crTextColor = OUT_FG;
          SendMessage(g_hwndEdit,EM_SETCHARFORMAT,SCF_ALL | SCF_DEFAULT,(LPARAM)&cf);
        }
        /* 左/右留白，否则贴边很难看 */
        SendMessage(g_hwndEdit,EM_SETMARGINS,
                    EC_LEFTMARGIN | EC_RIGHTMARGIN,MAKELPARAM(6,6));
      }

      Output_RefreshStatus();
      return 0;
    }

    case WM_SIZE:
      LayoutChildren(hwnd,LOWORD(lParam),HIWORD(lParam));
      return 0;

    case WM_GETMINMAXINFO: {
      /* 独立窗口时给个最小尺寸，否则拖到 0x0 后按钮全挤没了 */
      MINMAXINFO *pmmi = (MINMAXINFO*)lParam;
      pmmi->ptMinTrackSize.x = 340;
      pmmi->ptMinTrackSize.y = 150;
      return 0;
    }

    case WM_CLOSE:
      /* 只有独立窗口才有自己的关闭按钮；
         嵌入时是子窗口，收不到这个 —— 但万一收到也不能销毁，
         主窗口的 hwndOutput 会变野指针。 */
      if (g_bFloating) {
        Output_SetFloating(FALSE);
        if (g_pfnCloseHook)
          g_pfnCloseHook(g_pCloseUser);
      }
      return 0;

    case WM_CTLCOLORSTATIC:
      /* 状态条用父窗口的面色，文字用灰色，不抢焦点 */
      if ((HWND)lParam == g_hwndStatus) {
        SetBkMode((HDC)wParam,TRANSPARENT);
        SetTextColor((HDC)wParam,GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
      }
      break;

    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case IDC_OUTPUT_RUN: {
          WCHAR wszCmd[1024];
          wszCmd[0] = L'\0';
          if (g_hwndCombo)
            GetWindowText(g_hwndCombo,wszCmd,COUNTOF(wszCmd));
          Output_Run(wszCmd);
          return 0;
        }
        case IDC_OUTPUT_STOP:
          Output_Stop();
          return 0;
        case IDC_OUTPUT_CLEAR:
          if (g_hwndEdit) {
            SetWindowText(g_hwndEdit,L"");
            /* 清空后把默认色重新设一遍：RichEdit 清空后段落格式会留旧值，
               不重设的话下次输出可能续到上一段留下的颜色上 */
            {
              CHARFORMAT2W cf;
              memset(&cf,0,sizeof(cf));
              cf.cbSize = sizeof(cf);
              cf.dwMask = CFM_COLOR | CFM_BOLD;
              cf.crTextColor = OUT_FG;
              cf.dwEffects   = 0;
              SendMessage(g_hwndEdit,EM_SETCHARFORMAT,SCF_ALL | SCF_DEFAULT,(LPARAM)&cf);
            }
          }
          g_nLines = 0;
          memset(g_aLines,0,sizeof(g_aLines));
          return 0;
      }
      break;

    case WM_APP_OUTPUT: {
      /* 读取线程投递过来的文本：在这里落地，然后释放 */
      APPENDMSG *p = (APPENDMSG*)lParam;
      if (p) {
        if (p->wsz)
          DoAppend(p->wsz,p->bNewLine);
        if (p->wsz)
          LocalFree(p->wsz);
        LocalFree(p);
      }
      return 0;
    }

    case WM_APP_OUTPUT_GOTO: {
      /* 双击了某一行，转成主窗口的命令（lParam = 行索引） */
      if (g_hwndOwner)
        PostMessage(g_hwndOwner,WM_COMMAND,
                    MAKEWPARAM(IDM_OUTPUT_GOTOLINE,0),(LPARAM)lParam);
      return 0;
    }

    case WM_DESTROY:
      Output_Stop();
      if (g_hFontMono) {
        DeleteObject(g_hFontMono);
        g_hFontMono = NULL;
      }
      g_hwndPanel = NULL;
      g_hwndEdit  = NULL;
      g_hwndCombo = NULL;
      g_hwndStatus = NULL;
      return 0;
  }
  return DefWindowProc(hwnd,umsg,wParam,lParam);
}
/* ---------------------------------------------------------------------------
   读取线程：把管道里的字节按行切出，转成宽字符后追加到输出框，
   同时登记每行是否可跳转。

/* ---------------------------------------------------------------------------
   管道字节 → 宽字符
   外部命令的输出编码不确定：有的按 UTF-8（新版 git、clang），
   有的按控制台代码页（老 gcc、中文 Windows 上的 MSVC 是 GBK/936）。
   先按 UTF-8 严格解，失败再按控制台代码页解 —— 两条路都试过还不行
   才当 OEM 处理，尽量不让中文路径变成乱码。
--------------------------------------------------------------------------- */
static int DecodeOutputLine(const char *sz, int n, WCHAR *wsz, int cchOut)
{
  int cch;
  if (n <= 0 || !sz || !wsz || cchOut < 1)
    return 0;

  /* UTF-8：mbcs 里没有错误标志，用 WC_ERR_INVALID_CHARS 需要 Vista+，
     这里用 GetACP 兜底所以直接尝试即可 */
  cch = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,sz,n,wsz,cchOut - 1);
  if (cch > 0) {
    wsz[cch] = 0;
    return cch;
  }
  /* 控制台代码页 */
  cch = MultiByteToWideChar(GetConsoleOutputCP(),0,sz,n,wsz,cchOut - 1);
  if (cch > 0) {
    wsz[cch] = 0;
    return cch;
  }
  /* 系统 ANSI 代码页 */
  cch = MultiByteToWideChar(CP_ACP,0,sz,n,wsz,cchOut - 1);
  if (cch > 0) {
    wsz[cch] = 0;
    return cch;
  }
  wsz[0] = 0;
  return 0;
}
static DWORD WINAPI ReadThreadProc(LPVOID lpParam)
{
  char   buf[OUTPUT_PIPE_CHUNK];
  char   szLine[1024];
  int    nLine = 0;
  DWORD  nRead;
  DWORD  dwTotal = 0;
  while (g_hwndPanel && ReadFile((HANDLE)lpParam,buf,sizeof(buf),&nRead,NULL) && nRead > 0) {
    int i;
    dwTotal += nRead;
    for (i = 0; i < (int)nRead; i++) {
      char c = buf[i];
      if (c == '\r')
        continue;
      if (c == '\n') {
        WCHAR wsz[1024];
        int   cch;
        szLine[nLine] = '\0';
        cch = DecodeOutputLine(szLine,nLine,wsz,COUNTOF(wsz));
        if (cch > 0) {
          wsz[cch] = L'\0';
          PostAppend(wsz,TRUE);
        }
        nLine = 0;
      }
      else {
        if (nLine < (int)sizeof(szLine) - 1)
          szLine[nLine++] = c;
      }
    }
    if (InterlockedCompareExchange(&g_lCancel,0,0))
      break;
  }
  if (nLine > 0) {
    WCHAR wsz[1024];
    int   cch;
    szLine[nLine] = '\0';
    cch = DecodeOutputLine(szLine,nLine,wsz,COUNTOF(wsz));
    if (cch > 0) {
      wsz[cch] = L'\0';
      PostAppend(wsz,TRUE);
    }
  }
  return dwTotal ? 1 : 0;
}
/* ---------------------------------------------------------------------------
   变量替换
   命令里的 %FILE% / %DIR% / %NAME% / %EXT% / %LINE% / %SEL% 在此展开。
   展开发生在 Output_Run 之前，因此替换结果就是实际执行的命令行。
   主窗口通过 Output_SetEditorState 汇报编辑器状态，本模块只知道
   「当前文件路径、光标行、选区」这三组值，不关心编辑器怎么实现。
--------------------------------------------------------------------------- */
/* 主窗口汇报的编辑器状态 */
static WCHAR wszCurFilePath[MAX_PATH] = L"";
static int   g_iCurLine = 1;
static int   g_iSelStart = 0;
static int   g_iSelEnd   = 0;

/* 当前选中的文字（主窗口提供）。为空表示无选区。 */
static WCHAR wszSelection[4096] = L"";
void Output_SetEditorState(int iLine, int iSelStart, int iSelEnd)
{
  g_iCurLine  = (iLine > 0) ? iLine : 1;
  g_iSelStart = iSelStart;
  g_iSelEnd   = iSelEnd;
}
/* ---------------------------------------------------------------------------
   工作目录

   固定跟随当前文件所在目录 —— 用户打开 D:\笔记.c 后跑 gcc / build.bat /
   make，都期望相对于 D:\笔记\ 而不是 exe 所在的 bin\ 目录。
   没打开文件时退回 exe 所在目录（g_wchWorkingDirectory）。

   不提供手填覆盖：一个空着的输入框只会让人问「这填什么」。
   真要换目录，命令里写 cd /d 或 %DIR% 就够了。
--------------------------------------------------------------------------- */

static OutputRunHook g_pfnRunHook = NULL;   /* 运行前确认回调 */
static void         *g_pHookUser = NULL;

void Output_SetRunHook(OutputRunHook pfnHook, void *pUser)
{
  g_pfnRunHook = pfnHook;
  g_pHookUser  = pUser;
}


/*
  算出本次命令使用的工作目录：当前文件所在目录，没有文件就用 exe 目录。
*/
static void ResolveWorkDir(WCHAR *wszOut, int cchOut)
{
  wszOut[0] = L'\0';

  if (wszCurFilePath[0]) {
    lstrcpyn(wszOut,wszCurFilePath,cchOut);
    /* 去掉文件名，只留目录 */
    {
      WCHAR *p = wszOut + lstrlenW(wszOut);
      while (p > wszOut && *(p-1) != L'\\' && *(p-1) != L'/')
        p--;
      if (p > wszOut)
        *p = L'\0';
    }
    return;
  }
  lstrcpyn(wszOut,g_wchWorkingDirectory,cchOut);
}

void Output_SetCurrentFile(const WCHAR *wszPath)
{
  lstrcpyn(wszCurFilePath,wszPath ? wszPath : L"",COUNTOF(wszCurFilePath));
  Output_RefreshStatus();
}


/*
  刷新顶部信息条：显示 %FILE% 当前指向哪个文件。
  没打开文件时提示一句，免得用户对着 %FILE% 发呆。
*/
void Output_RefreshStatus(void)
{
  WCHAR wszText[MAX_PATH * 2 + 80];
  WCHAR wszDir[MAX_PATH];

  if (!g_hwndStatus || !IsWindow(g_hwndStatus))
    return;

  /* 只显示目录 + 文件名，不把完整路径再说一遍 ——
     文件路径里已经包含目录，两个都列是重复信息，还挤占宽度。 */
  if (wszCurFilePath[0]) {
    const WCHAR *pName = CurFileName();
    ResolveWorkDir(wszDir,COUNTOF(wszDir));
    if (wszDir[0])
      wsprintfW(wszText,L"%s%s",wszDir,pName);
    else
      lstrcpynW(wszText,pName,COUNTOF(wszText));
  }
  else {
    ResolveWorkDir(wszDir,COUNTOF(wszDir));
    if (wszDir[0])
      wsprintfW(wszText,L"未打开文件，命令在 %s 下运行",wszDir);
    else
      lstrcpyW(wszText,L"未打开文件");
  }

  SetWindowText(g_hwndStatus,wszText);
}
void Output_SetSelection(const WCHAR *wszText)
{
  lstrcpyn(wszSelection,wszText ? wszText : L"",COUNTOF(wszSelection));
}
static const WCHAR *CurFileName(void)
{
  const WCHAR *p = wszCurFilePath;
  const WCHAR *pSlash = NULL;
  if (!*p)
    return L"";

  /* 找最后一个路径分隔符 */
  {
    const WCHAR *q = p;
    while (*q) {
      if (*q == L'\\' || *q == L'/')
        pSlash = q;
      q++;
    }
  }
  return pSlash ? pSlash + 1 : p;
}
/* 展开单个变量到 wszOut；返回是否认识该变量名（不含 %） */
static BOOL ExpandVar(const WCHAR *wszName, int cchName,
                      WCHAR *wszOut, int cchOut)
{
  WCHAR wszNum[32];
  wszOut[0] = L'\0';
  if (cchName == 4 && !_wcsnicmp(wszName,L"FILE",4)) {
    lstrcpyn(wszOut,wszCurFilePath,cchOut);
    return TRUE;
  }
  if (cchName == 3 && !_wcsnicmp(wszName,L"DIR",3)) {
    lstrcpyn(wszOut,wszCurFilePath,cchOut);
    {
      WCHAR *p = wszOut + lstrlen(wszOut);
      while (p > wszOut && *(p-1) != L'\\' && *(p-1) != L'/')
        p--;
      if (p > wszOut)
        *p = L'\0';
    }
    return TRUE;
  }
  if (cchName == 4 && !_wcsnicmp(wszName,L"NAME",4)) {
    const WCHAR *p = CurFileName();
    int n = 0;
    while (p[n] && p[n] != L'.' && n < cchOut - 1) {
      wszOut[n] = p[n];
      n++;
    }
    wszOut[n] = L'\0';
    return TRUE;
  }
  if (cchName == 3 && !_wcsnicmp(wszName,L"EXT",3)) {
    const WCHAR *p = CurFileName();
    const WCHAR *pDot = NULL;
    while (*p) {
      if (*p == L'.')
        pDot = p;
      p++;
    }
    lstrcpyn(wszOut,pDot ? pDot + 1 : L"",cchOut);
    return TRUE;
  }
  if (cchName == 4 && !_wcsnicmp(wszName,L"LINE",4)) {
    wsprintfW(wszNum,L"%d",g_iCurLine);
    lstrcpyn(wszOut,wszNum,cchOut);
    return TRUE;
  }
  if (cchName == 3 && !_wcsnicmp(wszName,L"SEL",3)) {
    lstrcpyn(wszOut,wszSelection,cchOut);
    return TRUE;
  }
  return FALSE;
}
/*
  把 wszIn 里的 %VAR% 展开到 wszOut。
  规则：
    %%         → 一个字面 '%'
    %VAR%      → 变量；不认识 VAR 时原样保留（不含 %），并在调试日志里提一句
    孤立的 %   → 原样保留
*/
static void ExpandVariables(const WCHAR *wszIn, WCHAR *wszOut, int cchOut)
{
  int i = 0, n = 0;
  if (!wszIn || !wszOut || cchOut < 1)
    return;
  while (wszIn[i] && n < cchOut - 1) {
    if (wszIn[i] != L'%') {
      wszOut[n++] = wszIn[i++];
      continue;
    }
    /* %% → 百分号 */
    if (wszIn[i+1] == L'%') {
      wszOut[n++] = L'%';
      i += 2;
      continue;
    }
    /* 找配对的 % */
    {
      int j = i + 1;
      int cchName = 0;
      WCHAR wszVal[MAX_PATH + 4096];
      while (wszIn[j] && wszIn[j] != L'%' && cchName < 32) {
        j++;
        cchName++;
      }
      if (wszIn[j] == L'%' && cchName > 0) {
        /* 完整的一对：尝试展开 */
        if (ExpandVar(wszIn + i + 1, cchName, wszVal, COUNTOF(wszVal))) {
          int k;
          /* 原样展开，不自动加引号。
             理由：%DIR%%NAME%.o 这类跨变量的写法无法逐变量加引号
             （引号会把后半段切到外面），自动加反而制造更难查的错。
             需要引号时用户自己写 "%FILE%"，行为可预期。 */
          for (k = 0; wszVal[k] && n < cchOut - 1; k++)
            wszOut[n++] = wszVal[k];
          i = j + 1;
          continue;
        }
        /* 不认识的变量：原样输出（含百分号），不破坏用户输入 */
        wszOut[n++] = wszIn[i];
        i++;
        continue;
      }
      /* 没有配对的 %：原样输出 */
      wszOut[n++] = wszIn[i++];
    }
  }
  wszOut[n] = L'\0';
}
/* ---------------------------------------------------------------------------
   执行命令（核心）
   流程：建匿名管道 → CreateProcess 重定向 stdout/stderr、stdin 接 NUL
        → 起读取线程 → 等进程结束 → 收尾。
--------------------------------------------------------------------------- */
void Output_Run(const WCHAR *wszCmd)
{
  SECURITY_ATTRIBUTES sa;
  STARTUPINFOW        si;
  PROCESS_INFORMATION pi;
  HANDLE hOutWr = NULL, hInRd = NULL, hNul = NULL;
  WCHAR *wszCopy = NULL;
  WCHAR *wszExpanded = NULL;
  WCHAR wszWorkDir2[MAX_PATH];
  WCHAR wszPrevDir[MAX_PATH];
  size_t cch;
  DWORD  dwExit;
  if (!wszCmd || !*wszCmd)
    return;
  if (g_bRunning) {
    DoAppend(L"已有命令正在运行，请先停止。",TRUE);
    return;
  }

  /* 运行前确认：主窗口借此提示「有未保存的修改，先保存吗？」。
     用户否掉就不跑，绝不擅自改动人家的文件。 */
  if (g_pfnRunHook && !g_pfnRunHook(g_pHookUser))
    return;
  /* 先展开 %FILE% 等变量，展开后的文本才是真正执行的命令行 */
  cch = lstrlenW(wszCmd);
  wszExpanded = LocalAlloc(LPTR,(cch * 4 + MAX_PATH + 4096 + 8) * sizeof(WCHAR));
  if (!wszExpanded)
    return;
  ExpandVariables(wszCmd,wszExpanded,(int)(cch * 4 + MAX_PATH + 4096 + 4));

  /* .bat / .cmd 不能直接被 CreateProcess 执行（会返回
     ERROR_BAD_EXE_FORMAT 或文件找不到），必须经 cmd /c 包一层。
     用户写 build.bat 是再自然不过的事，不包就是给他埋雷。 */
  {
    const WCHAR *pExe = wszExpanded;
    int nExe = 0;
    WCHAR *pTail;
    /* 跳过前导引号，取可执行部分 */
    if (*pExe == L'"')
      pExe++;
    while (pExe[nExe] && pExe[nExe] != L' ' && pExe[nExe] != L'"')
      nExe++;
    if (nExe > 4 &&
        (!_wcsnicmp(pExe + nExe - 4,L".bat",4) ||
         !_wcsnicmp(pExe + nExe - 4,L".cmd",4)))
    {
      WCHAR *wszWrapped = LocalAlloc(LPTR,(lstrlenW(wszExpanded) + 16) * sizeof(WCHAR));
      if (wszWrapped) {
        lstrcpyW(wszWrapped,L"cmd /c ");
        lstrcatW(wszWrapped,wszExpanded);
        LocalFree(wszExpanded);
        wszExpanded = wszWrapped;
      }
    }
  }

  wszCopy = LocalAlloc(LPTR,(lstrlenW(wszExpanded) + 8) * sizeof(WCHAR));
  if (!wszCopy) {
    LocalFree(wszExpanded);
    return;
  }
  lstrcpyW(wszCopy,wszExpanded);
  LocalFree(wszExpanded);
  memset(&sa,0,sizeof(sa));
  sa.nLength        = sizeof(sa);
  sa.bInheritHandle = TRUE;
  if (!CreatePipe(&g_hOutRd,&hOutWr,&sa,0))
    goto done;

  /* 子进程的 stdout 句柄必须可继承，孙句柄不可继承 */
  if (!SetHandleInformation(g_hOutRd,HANDLE_FLAG_INHERIT,0))
    goto done;

  /* stdin 接 NUL 设备：交互式程序读不到输入会自行退出 */
  hNul = CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
                     &sa,OPEN_EXISTING,0,NULL);
  if (hNul == INVALID_HANDLE_VALUE) {
    hNul = NULL;
  }
  memset(&si,0,sizeof(si));
  si.cb          = sizeof(si);
  si.dwFlags     = STARTF_USESTDHANDLES;
  if (hNul)
    si.hStdInput = hNul;              /* stdin 给 NUL：交互程序读不到输入会退出 */
  else
    si.hStdInput = INVALID_HANDLE_VALUE;   /* 没有输入可用 */
  si.hStdOutput  = hOutWr;
  si.hStdError   = hOutWr;

  /* 工作目录：跟随当前文件所在目录 */
  ResolveWorkDir(wszWorkDir2,COUNTOF(wszWorkDir2));

  InterlockedExchange(&g_lCancel,0);
  g_nLines  = 0;
  g_bRunning = TRUE;
  EnableButtons(TRUE);
  if (g_hwndEdit)
    SetWindowText(g_hwndEdit,L"");
  DoAppendEx(wszCmd,TRUE,1);
  DoAppendEx(L"--------------------------------------------------",TRUE,2);

  /* 切换工作目录：跟随当前文件所在目录。
     这里用 SetCurrentDirectory 而不是 CreateProcessW 的 lpCurrentDirectory
     —— 后者对「存在但不可用」的路径会直接返回 ERROR_INVALID_PARAMETER，
     把整条命令判死；前者失败也能继续跑（退回 exe 目录），不会误杀。
     跑完立刻恢复，不影响程序其它部分。 */
  wszPrevDir[0] = L'\0';
  if (wszWorkDir2[0] && GetFileAttributesW(wszWorkDir2) != INVALID_FILE_ATTRIBUTES) {
    GetCurrentDirectoryW(COUNTOF(wszPrevDir),wszPrevDir);
    SetCurrentDirectoryW(wszWorkDir2);
  }

  if (!CreateProcessW(NULL,wszCopy,NULL,NULL,TRUE,
                      CREATE_NO_WINDOW,NULL,NULL,&si,&pi))
  {
    WCHAR wszErr[128];
    wsprintfW(wszErr,L"[启动失败] 错误码 %lu",GetLastError());
    DoAppendEx(wszErr,TRUE,2);
    DoAppendEx(L"提示：命令必须是可执行程序或 .cmd/.bat 批处理。",TRUE,2);
    g_bRunning = FALSE;
    EnableButtons(FALSE);
    goto done;
  }
  g_hProc = pi.hProcess;
  CloseHandle(pi.hThread);

  /* 记入历史（用用户原始输入，不存展开后的路径，便于换文件后复用）。
     放在 CreateProcess 之后：HistoryAdd 会操作下拉框、触发控件通知，
     放在前面时会把 STARTUPINFO 和工作目录缓冲一起搅乱（实测
     CreateProcessW 返回 ERROR_INVALID_PARAMETER）。 */
  HistoryAdd(wszCmd);

  /* 写端交给子进程，本进程不再持有，读取线程读到 EOF 即结束 */
  if (hOutWr) {
    CloseHandle(hOutWr);
    hOutWr = NULL;
  }
  g_hReadThread = CreateThread(NULL,0,ReadThreadProc,g_hOutRd,0,NULL);

  /* 等进程结束。这里必须一边等一边泵消息 —— 读取线程用 PostMessage
     投递文本，主线程不泵消息的话，输出会全堆在队列里，
     等命令跑完才一口气蹦出来（实测一条 4 秒的命令，输出也是 4 秒后
     才出现，中间一片空白）。

     不直接调 MsgWaitForMultipleObjects 是因为还要处理取消；
     用 PeekMessage 轮询就够，输出量再大也吃得下。 */
  for (;;) {
    MSG msg;

    /* 先把队列里已到达的输出落地 */
    while (PeekMessage(&msg,NULL,0,0,PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }

    if (WaitForSingleObject(pi.hProcess,50) == WAIT_OBJECT_0)
      break;
    if (InterlockedCompareExchange(&g_lCancel,0,0)) {
      TerminateProcess(pi.hProcess,1);
      break;
    }
  }

  /* 进程结束了，但管道里可能还有尾巴。再泵几轮，让读取线程
     把剩下的行投递完，否则最后几行会丢。 */
  {
    int nSpins;
    for (nSpins = 0; nSpins < 40; nSpins++) {
      MSG msg;
      BOOL bGot = FALSE;
      while (PeekMessage(&msg,NULL,0,0,PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        bGot = TRUE;
      }
      if (!bGot && g_hReadThread &&
          WaitForSingleObject(g_hReadThread,0) == WAIT_OBJECT_0)
        break;
      Sleep(10);
    }
  }

  /* 恢复原工作目录 */
  if (wszPrevDir[0])
    SetCurrentDirectoryW(wszPrevDir);

  GetExitCodeProcess(pi.hProcess,&dwExit);

  /* 等读取线程把缓冲区排空 */
  if (g_hReadThread) {
    WaitForSingleObject(g_hReadThread,3000);
    CloseHandle(g_hReadThread);
    g_hReadThread = NULL;
  }
  {
    WCHAR wszDone[128];
    if (InterlockedCompareExchange(&g_lCancel,0,0))
      wsprintfW(wszDone,L"[已中止] 退出码 %lu",dwExit);
    else
      wsprintfW(wszDone,L"[完成] 退出码 %lu",dwExit);
    DoAppendEx(wszDone,TRUE,2);
  }
  CloseHandle(pi.hProcess);
  g_hProc = NULL;
  g_bRunning = FALSE;
  EnableButtons(FALSE);
done:
  if (wszCopy)
    LocalFree(wszCopy);
  if (g_hOutRd) {
    CloseHandle(g_hOutRd);
    g_hOutRd = NULL;
  }
  if (hOutWr)
    CloseHandle(hOutWr);
  if (hNul)
    CloseHandle(hNul);
}
void Output_Stop(void)
{
  InterlockedExchange(&g_lCancel,1);
  if (g_hProc)
    TerminateProcess(g_hProc,1);
}
void Output_AppendText(const WCHAR *wszText, BOOL bNewLine)
{
  DoAppend(wszText,bNewLine);
}
BOOL Output_IsRunning(void)
{
  return g_bRunning;
}
HWND Output_GetComboBox(void)
{
  return g_hwndCombo;
}
BOOL Output_GetGotoTarget(int idx, WCHAR *wszFile, int cchFile, int *piLine)
{
  LINEINFO *p;
  if (idx < 0 || idx >= g_nLines || !wszFile || cchFile < 1 || !piLine)
    return FALSE;
  p = &g_aLines[idx];
  if (!p->bGoto)
    return FALSE;
  lstrcpynW(wszFile,p->wszFile,cchFile);
  *piLine = p->iLine;
  return TRUE;
}
/* ---------------------------------------------------------------------------
   命令历史持久化（注册表 HKCU）
   存最近 OUTPUT_HISTORY_MAX 条命令：
      HKCU\Software\notepad2md\Output
        CmdHistory1 = "gcc -c %FILE% -o %DIR%%NAME%.exe"
        CmdHistory2 = "git status"
   用注册表而不是 ini：本程序默认不生成 ini 文件（设置走注册表是
   Notepad2 的传统），ini 路径在未按 F7 保存前根本不存在，
   WritePrivateProfileString 会静默失败。注册表则始终可用。
--------------------------------------------------------------------------- */
#define OUTPUT_HISTORY_MAX 16
#define OUTPUT_REG_KEY     L"Software\\notepad2md\\Output"
static HKEY OpenHistoryKey(BOOL bCreate)
{
  HKEY hKey;
  LONG lRet;
  REGSAM sam = KEY_READ;
  if (bCreate)
    sam |= KEY_WRITE;
  lRet = RegCreateKeyExW(HKEY_CURRENT_USER,OUTPUT_REG_KEY,0,NULL,
                         REG_OPTION_NON_VOLATILE,sam,NULL,&hKey,NULL);
  if (lRet != ERROR_SUCCESS)
    return NULL;
  return hKey;
}
static void HistoryLoad(void)
{
  WCHAR wszKey[32];
  WCHAR wszCmd[1024];
  HKEY hKey;
  DWORD cbData, dwType;
  int i;
  if (!g_hwndCombo)
    return;
  ComboBox_ResetContent(g_hwndCombo);
  hKey = OpenHistoryKey(FALSE);
  if (!hKey)
    return;

  /* 从后往前读，保证最新命令排在列表最前 */
  for (i = OUTPUT_HISTORY_MAX; i >= 1; i--) {
    wsprintfW(wszKey,L"CmdHistory%d",i);
    cbData = sizeof(wszCmd);
    dwType = REG_SZ;
    wszCmd[0] = L'\0';
    if (RegQueryValueExW(hKey,wszKey,NULL,&dwType,(LPBYTE)wszCmd,&cbData)
        == ERROR_SUCCESS && wszCmd[0])
      ComboBox_AddString(g_hwndCombo,wszCmd);
  }
  RegCloseKey(hKey);
}
static void HistoryAdd(const WCHAR *wszCmd)
{
  WCHAR wszList[OUTPUT_HISTORY_MAX][1024];
  WCHAR wszKey[32];
  HKEY hKey;
  int n = 0;
  int i;
  if (!g_hwndCombo || !wszCmd || !*wszCmd)
    return;

  /* 现有列表去重（剔除与本次相同的项），新的排最前 */
  {
    int nExist = ComboBox_GetCount(g_hwndCombo);
    for (i = 0; i < nExist && n < OUTPUT_HISTORY_MAX - 1; i++) {
      ComboBox_GetLBText(g_hwndCombo,i,wszList[n]);
      if (lstrcmpi(wszList[n],wszCmd) != 0)
        n++;
    }
  }
  for (i = n; i > 0; i--)
    lstrcpyn(wszList[i],wszList[i-1],COUNTOF(wszList[i]));
  lstrcpyn(wszList[0],wszCmd,COUNTOF(wszList[0]));
  n++;
  if (n > OUTPUT_HISTORY_MAX)
    n = OUTPUT_HISTORY_MAX;

  /* 写注册表 */
  hKey = OpenHistoryKey(TRUE);
  if (hKey) {
    for (i = 0; i < n; i++) {
      wsprintfW(wszKey,L"CmdHistory%d",i + 1);
      RegSetValueExW(hKey,wszKey,0,REG_SZ,(CONST BYTE*)wszList[i],
                     (DWORD)((lstrlenW(wszList[i]) + 1) * sizeof(WCHAR)));
    }
    /* 清掉本次没用到的旧键 */
    for (; i < OUTPUT_HISTORY_MAX; i++) {
      wsprintfW(wszKey,L"CmdHistory%d",i + 1);
      RegDeleteValueW(hKey,wszKey);
    }
    RegCloseKey(hKey);
  }
  /* 刷新下拉（内容与注册表一致） */
  ComboBox_ResetContent(g_hwndCombo);
  for (i = 0; i < n; i++)
    ComboBox_AddString(g_hwndCombo,wszList[i]);
  SetWindowText(g_hwndCombo,wszCmd);
}
/* ---------------------------------------------------------------------------
   模块级初始化与面板创建
--------------------------------------------------------------------------- */
BOOL Output_Init(HINSTANCE hinst)
{
  WNDCLASSEX wc;
  g_hinst = hinst;
  if (g_atomClass)
    return TRUE;
  memset(&wc,0,sizeof(wc));
  wc.cbSize        = sizeof(wc);
  wc.style         = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc   = OutputWndProc;
  wc.hInstance     = hinst;
  wc.hCursor       = LoadCursor(NULL,IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = L"Notepad2mdOutputPanel";
  g_atomClass = RegisterClassEx(&wc);
  return (g_atomClass != 0);
}
HWND Output_Create(HWND hwndParent, HINSTANCE hinst)
{
  HWND hwnd;
  if (!g_atomClass && !Output_Init(hinst))
    return NULL;
  g_hwndOwner = hwndParent;
  hwnd = CreateWindowEx(0,L"Notepad2mdOutputPanel",NULL,
                        WS_CHILD | WS_CLIPCHILDREN,
                        0,0,0,0,hwndParent,NULL,hinst,NULL);
  if (hwnd)
    HistoryLoad();
  return hwnd;
}