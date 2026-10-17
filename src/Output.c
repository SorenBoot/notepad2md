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
#include <stdio.h>
#include <string.h>
#include <strsafe.h>
#include <shlwapi.h>
#include "Output.h"
#include "resource.h"

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
  if (iswalpha(q[0]) && q[1] == L':') {
    wszFile[0] = q[0];
    wszFile[1] = L':';
    cch = 2;
    q += 2;
  }
  while (*q && *q != L':' && *q != L'(' && *q != L' ' &&
         cch < MAX_PATH - 1) {
    wszFile[cch++] = *q++;
  }
  wszFile[cch] = 0;

  /* 行号：从文件名结束的位置往后读 */
  pStop = q;
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
      HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

      g_hwndPanel = hwnd;

      g_hwndCombo = CreateWindowEx(0,L"COMBOBOX",NULL,
                     WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_AUTOHSCROLL |
                     WS_VSCROLL | WS_TABSTOP,
                     0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_CMD,g_hinst,NULL);

      CreateWindowEx(0,L"BUTTON",L"运行",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                     0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_RUN,g_hinst,NULL);

      CreateWindowEx(0,L"BUTTON",L"停止",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                     0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_STOP,g_hinst,NULL);

      CreateWindowEx(0,L"BUTTON",L"清空",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                     0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_CLEAR,g_hinst,NULL);

      g_hwndEdit = CreateWindowEx(WS_EX_CLIENTEDGE,L"EDIT",NULL,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                    ES_READONLY | ES_AUTOVSCROLL,
                    0,0,0,0,hwnd,(HMENU)IDC_OUTPUT_LOG,g_hinst,NULL);

      /* 子类化输出框，捕获双击以跳转到错误行 */
      g_pfnEditProc = (WNDPROC)SetWindowLongPtr(g_hwndEdit,GWLP_WNDPROC,
                                               (LONG_PTR)OutputEditProc);

      SendMessage(g_hwndCombo,WM_SETFONT,(WPARAM)hFont,FALSE);
      SendMessage(g_hwndEdit, WM_SETFONT,(WPARAM)hFont,FALSE);
      return 0;
    }

    case WM_SIZE: {
      int cx = LOWORD(lParam);
      int cy = HIWORD(lParam);
      int hRow = 22;

      if (g_hwndCombo)
        MoveWindow(g_hwndCombo,0,0,cx - 3 * 60 - 12,hRow,TRUE);
      if (GetDlgItem(hwnd,IDC_OUTPUT_RUN))
        MoveWindow(GetDlgItem(hwnd,IDC_OUTPUT_RUN),cx - 3*60 - 8,0,56,hRow,TRUE);
      if (GetDlgItem(hwnd,IDC_OUTPUT_STOP))
        MoveWindow(GetDlgItem(hwnd,IDC_OUTPUT_STOP),cx - 2*60 - 4,0,56,hRow,TRUE);
      if (GetDlgItem(hwnd,IDC_OUTPUT_CLEAR))
        MoveWindow(GetDlgItem(hwnd,IDC_OUTPUT_CLEAR),cx - 60,0,56,hRow,TRUE);
      if (g_hwndEdit)
        MoveWindow(g_hwndEdit,0,hRow + 4,cx,cy - hRow - 4,TRUE);
      return 0;
    }

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
      g_hwndPanel = NULL;
      g_hwndEdit  = NULL;
      g_hwndCombo = NULL;
      return 0;
  }
  return DefWindowProc(hwnd,umsg,wParam,lParam);
}


/* ---------------------------------------------------------------------------
   读取线程：把管道里的字节按行切出，转成宽字符后追加到输出框，
   同时登记每行是否可跳转。
--------------------------------------------------------------------------- */
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
        cch = MultiByteToWideChar(CP_UTF8,0,szLine,nLine,wsz,COUNTOF(wsz) - 1);
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
    cch = MultiByteToWideChar(CP_UTF8,0,szLine,nLine,wsz,COUNTOF(wsz) - 1);
    if (cch > 0) {
      wsz[cch] = L'\0';
      
      PostAppend(wsz,TRUE);
    }
  }

  return dwTotal ? 1 : 0;
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
  size_t cch;
  DWORD  dwExit;

  if (!wszCmd || !*wszCmd)
    return;

  if (g_bRunning) {
    DoAppend(L"已有命令正在运行，请先停止。",TRUE);
    return;
  }

  cch = lstrlenW(wszCmd);
  wszCopy = LocalAlloc(LPTR,(cch + 8) * sizeof(WCHAR));
  if (!wszCopy)
    return;

  /* 前面垫一个空格是 CreateProcessW 的可写缓冲区惯例（此处直接拷贝即可） */
  lstrcpyW(wszCopy,wszCmd);

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
  if (hNul == INVALID_HANDLE_VALUE)
    hNul = NULL;

  memset(&si,0,sizeof(si));
  si.cb          = sizeof(si);
  si.dwFlags     = STARTF_USESTDHANDLES;
  if (hNul)
    si.hStdInput = hNul;              /* stdin 给 NUL：交互程序读不到输入会退出 */
  else
    si.hStdInput = INVALID_HANDLE_VALUE;   /* 没有输入可用 */
  si.hStdOutput  = hOutWr;
  si.hStdError   = hOutWr;

  InterlockedExchange(&g_lCancel,0);
  g_nLines  = 0;
  g_bRunning = TRUE;
  EnableButtons(TRUE);

  if (g_hwndEdit)
    SetWindowText(g_hwndEdit,L"");

  DoAppend(wszCmd,TRUE);
  DoAppend(L"--------------------------------------------------",TRUE);

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
  if (!g_atomClass && !Output_Init(hinst))
    return NULL;

  g_hwndOwner = hwndParent;

  return CreateWindowEx(0,L"Notepad2mdOutputPanel",NULL,
                        WS_CHILD | WS_CLIPCHILDREN,
                        0,0,0,0,hwndParent,NULL,hinst,NULL);
}
