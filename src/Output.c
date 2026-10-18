/*===========================================================================
  notepad2md — 命令输出面板
  在编辑器底部开一条面板，把外部命令的标准输出/标准错误实时显示出来。
  输出里出现「文件:行号」时双击该行，编辑器跳到对应位置。
  设计取舍（刻意为之，不是偷懒）：
    - stdin 重定向到 NUL：交互式程序（ssh、python REPL）不会挂死，
      它们会因为读不到输入而退出，或自行弹出自己的控制台窗口。
    - 不解析 ANSI 转义：纯文本显示，颜色丢失但内容不丢。
    - 不做终端模拟器：Windows 上嵌 conhost 不可靠，ConPTY 在 Win7 不存在，
      这里只是一个「CreateProcess + 匿名管道」的读取器。
    - 因此本面板不是终机的替代品，只服务于
      「跑个命令看结果、顺着错误行回去改代码」这个编辑器场景。
  ===========================================================================*/
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
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
static HWND      g_hwndDir = NULL;       /* 工作目录输入框 */
static HFONT     g_hFontMono = NULL;     /* 输出区等宽字体 */
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
void Output_SetWorkDir(const WCHAR *wszPath);
void Output_RefreshStatus(void);

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
      PostMessage(GetParent(hwnd),WM_APP_OUTPUT_GOTO,0,(LPARAM)iLine);
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
    HWND hwndPanel = GetParent(hwnd);
    if (hwndPanel)
      PostMessage(hwndPanel,WM_COMMAND,
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
static void DoAppend(const WCHAR *wszText, BOOL bNewLine)
{
  int iLen;
  int iLineIdx;
  if (!g_hwndEdit || !wszText)
    return;

  /* 超限时从头砍一半，防止无限增长 */
  iLen = GetWindowTextLength(g_hwndEdit);
  if (iLen > OUTPUT_MAX_TEXT) {
    SendMessage(g_hwndEdit,EM_SETSEL,0,OUTPUT_MAX_TEXT / 2);
    SendMessage(g_hwndEdit,EM_REPLACESEL,TRUE,(LPARAM)L"");
    iLen = GetWindowTextLength(g_hwndEdit);
  }
  /* 本行索引：新文字总是追加在末尾。末尾字符所在行号 +1 ——
     缓冲区以换行结尾时（每次都补 CRLF），末尾字符属于上一行，
     新文字从下一行开始，所以要 +1。 */
  iLineIdx = 0;
  if (iLen > 0)
    iLineIdx = (int)SendMessage(g_hwndEdit,EM_LINEFROMCHAR,(WPARAM)(iLen - 1),0) + 1;
  SendMessage(g_hwndEdit,EM_REPLACESEL,FALSE,(LPARAM)wszText);
  if (bNewLine)
    SendMessage(g_hwndEdit,EM_REPLACESEL,FALSE,(LPARAM)L"\r\n");
  RegisterLine(iLineIdx,wszText);
}
static void EnableButtons(BOOL bRunning)
{
  EnableWindow(GetDlgItem(g_hwndPanel,IDC_OUTPUT_RUN), !bRunning);
  EnableWindow(GetDlgItem(g_hwndPanel,IDC_OUTPUT_STOP), bRunning);
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

      /* 目录框：留空 = 跟随当前文件；填了就在该目录下跑命令 */
      CreateWindowEx(0,L"STATIC",L"目录:",
                     WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                     0,0,0,0,hwnd,(HMENU)-1,g_hinst,NULL);
      g_hwndDir = CreateWindowEx(WS_EX_CLIENTEDGE,L"EDIT",NULL,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_DIR,g_hinst,NULL);

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

      g_hwndEdit = CreateWindowEx(WS_EX_CLIENTEDGE,L"EDIT",NULL,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                    ES_READONLY | ES_AUTOVSCROLL,
                    0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_LOG,g_hinst,NULL);

      /* 子类化输出框：捕获双击跳转 + 回车直接运行命令 */
      g_pfnEditProc = (WNDPROC)SetWindowLongPtr(g_hwndEdit,GWLP_WNDPROC,
                                               (LONG_PTR)OutputEditProc);
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

      Output_RefreshStatus();
      return 0;
    }

    case WM_SIZE: {
      int cx = LOWORD(lParam);
      int cy = HIWORD(lParam);
      int y = 0;
      int hStatus = 18;
      int hRow = 24;

      /* 状态条 */
      if (g_hwndStatus)
        MoveWindow(g_hwndStatus,4,y,cx - 8,hStatus,TRUE);
      y += hStatus;

      /* 目录行：「目录:」标签 + 输入框 */
      if (GetDlgItem(hwnd,-1))
        MoveWindow(GetDlgItem(hwnd,-1),4,y + 3,34,hRow - 4,TRUE);
      if (g_hwndDir)
        MoveWindow(g_hwndDir,38,y,cx - 42,hRow - 2,TRUE);
      y += hRow;

      /* 命令行 + 按钮 */
      if (g_hwndCombo)
        MoveWindow(g_hwndCombo,4,y,cx - 3 * 62 - 16,hRow - 2,TRUE);
      if (GetDlgItem(hwnd,IDC_OUTPUT_RUN))
        MoveWindow(GetDlgItem(hwnd,IDC_OUTPUT_RUN),cx - 3*62 - 10,y,58,hRow - 2,TRUE);
      if (GetDlgItem(hwnd,IDC_OUTPUT_STOP))
        MoveWindow(GetDlgItem(hwnd,IDC_OUTPUT_STOP),cx - 2*62 - 6,y,58,hRow - 2,TRUE);
      if (GetDlgItem(hwnd,IDC_OUTPUT_CLEAR))
        MoveWindow(GetDlgItem(hwnd,IDC_OUTPUT_CLEAR),cx - 62 - 2,y,58,hRow - 2,TRUE);
      y += hRow;

      /* 输出区占剩下的全部 */
      if (g_hwndEdit)
        MoveWindow(g_hwndEdit,2,y,cx - 4,cy - y - 2,TRUE);
      return 0;
    }

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
          if (g_hwndEdit)
            SetWindowText(g_hwndEdit,L"");
          g_nLines = 0;
          memset(g_aLines,0,sizeof(g_aLines));
          return 0;
        case IDC_OUTPUT_DIR:
          /* 目录框内容变了：立刻采用，下次运行即生效 */
          if (HIWORD(wParam) == EN_CHANGE && g_hwndDir) {
            WCHAR wszDir[MAX_PATH];
            wszDir[0] = L'\0';
            GetWindowText(g_hwndDir,wszDir,COUNTOF(wszDir));
            Output_SetWorkDir(wszDir);
          }
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
      HWND hwndOwner = GetParent(hwnd);
      if (hwndOwner)
        PostMessage(hwndOwner,WM_COMMAND,
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

   默认跟随当前文件所在目录 —— 用户打开 D:\笔记\a.c 后跑 gcc / build.bat /
   make，都期望相对于 D:\笔记\ 而不是 exe 所在的 bin\ 目录。
   没打开文件时退回 exe 所在目录（g_wchWorkingDirectory）。

   用户可以在「目录」输入框里手改，改完后命令就在那个目录下跑。
   输入框为空表示「跟随当前文件」。
--------------------------------------------------------------------------- */

static WCHAR wszWorkDir[MAX_PATH] = L"";   /* 用户覆盖的工作目录；空 = 跟随文件 */
static OutputRunHook g_pfnRunHook = NULL;   /* 运行前确认回调 */
static void         *g_pHookUser = NULL;

void Output_SetWorkDir(const WCHAR *wszPath)
{
  lstrcpyn(wszWorkDir,wszPath ? wszPath : L"",COUNTOF(wszWorkDir));
  Output_RefreshStatus();
}


void Output_SetRunHook(OutputRunHook pfnHook, void *pUser)
{
  g_pfnRunHook = pfnHook;
  g_pHookUser  = pUser;
}


const WCHAR *Output_GetWorkDir(void)
{
  return wszWorkDir;
}


/*
  算出本次命令真正使用的工作目录。

  优先级：用户手填 > 当前文件所在目录 > exe 所在目录
*/
static void ResolveWorkDir(WCHAR *wszOut, int cchOut)
{
  wszOut[0] = L'\0';

  if (wszWorkDir[0]) {
    lstrcpyn(wszOut,wszWorkDir,cchOut);
    return;
  }
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

  if (wszCurFilePath[0])
    wsprintfW(wszText,L"%%FILE%% = %s",wszCurFilePath);
  else
    lstrcpyW(wszText,L"当前没有打开的文件，%FILE% / %DIR% / %NAME% 展开为空");

  /* 同时显示命令将在哪个目录下跑 —— 少了这一句，
     用户写 gcc foo.c 失败时根本不知道去哪找原因 */
  ResolveWorkDir(wszDir,COUNTOF(wszDir));
  if (wszDir[0])
    wsprintfW(wszText + lstrlenW(wszText),L"    工作目录: %s",wszDir);

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

  /* 工作目录：跟随当前文件所在目录，除非用户在目录框里手改过 */
  ResolveWorkDir(wszWorkDir2,COUNTOF(wszWorkDir2));

  InterlockedExchange(&g_lCancel,0);
  g_nLines  = 0;
  g_bRunning = TRUE;
  EnableButtons(TRUE);
  if (g_hwndEdit)
    SetWindowText(g_hwndEdit,L"");
  DoAppend(wszCmd,TRUE);
  DoAppend(L"--------------------------------------------------",TRUE);

  /* 切换工作目录：跟随当前文件所在目录，除非用户在目录框里手改过。
     这里用 SetCurrentDirectory 而不是 CreateProcessW 的 lpCurrentDirectory
     —— 后者对「存在但不可用」的路径会直接返回 ERROR_INVALID_PARAMETER，
     把整条命令判死；前者失败也能继续跑（退回 exe 目录），不会误杀。
     跑完立刻恢复，不影响程序其它部分。 */
  wszPrevDir[0] = L'\0';
  ResolveWorkDir(wszWorkDir2,COUNTOF(wszWorkDir2));
  if (wszWorkDir2[0] && GetFileAttributesW(wszWorkDir2) != INVALID_FILE_ATTRIBUTES) {
    GetCurrentDirectoryW(COUNTOF(wszPrevDir),wszPrevDir);
    SetCurrentDirectoryW(wszWorkDir2);
  }

  if (!CreateProcessW(NULL,wszCopy,NULL,NULL,TRUE,
                      CREATE_NO_WINDOW,NULL,NULL,&si,&pi))
  {
    WCHAR wszErr[128];
    wsprintfW(wszErr,L"[启动失败] 错误码 %lu",GetLastError());
    DoAppend(wszErr,TRUE);
    DoAppend(L"提示：命令必须是可执行程序或 .cmd/.bat 批处理。",TRUE);
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

  /* 等待进程退出（带取消轮询，最多等到它自己结束） */
  for (;;) {
    if (WaitForSingleObject(pi.hProcess,100) == WAIT_OBJECT_0)
      break;
    if (InterlockedCompareExchange(&g_lCancel,0,0)) {
      TerminateProcess(pi.hProcess,1);
      break;
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
    DoAppend(wszDone,TRUE);
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