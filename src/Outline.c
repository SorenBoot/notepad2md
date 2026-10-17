/*===========================================================================

  notepad2md — 大纲面板实现

  扫描 Markdown 文本中的 ATX 标题（# ~ ######），把它们放进一个 ListBox。
  每项按层级加两个全角空格的缩进，便于看出文档结构；双击跳到对应行。

  与 md4c 判断不一致的取舍：
    这里只认 ATX 标题（行首 #），不处理 Setext 标题（=== / --- 下划线）。
    这与主窗口里 PreviewScanHeadings() 的做法保持一致 —— 宁可漏，不可错。
    漏掉的表现是某一节在大纲里没有，但预览和滚动同步都不受影响。

  ===========================================================================*/

#include <windows.h>
#include <windowsx.h>
#include <string.h>
#include <stdio.h>
#include "Outline.h"
#include "resource.h"

/* COUNTOF 在 Notepad2 的 Dialogs.h 里定义，本模块独立编译故自带一份 */
#ifndef COUNTOF
#define COUNTOF(a) (sizeof(a)/sizeof(a[0]))
#endif

#define OUTLINE_MAX_ITEMS   4096

/* 每项的数据。列表索引与本表一一对应。 */
typedef struct {
  int iLine;      /* 0 based 行号 */
  int iLevel;     /* 1 ~ 6 */
} OUTLINE_ITEM;

static HINSTANCE g_hinst;

static HWND        g_hwndPanel = NULL;
static HWND        g_hwndList  = NULL;
static ATOM        g_atomClass = 0;
static WNDPROC     g_pfnListProc = NULL;

static OUTLINE_ITEM g_aItems[OUTLINE_MAX_ITEMS];
static int          g_nItems = 0;


/* ---------------------------------------------------------------------------
   取出一行 ATX 标题的文字部分（UTF-8），转成宽字符，并加上层级缩进。

   q     —— 指向 '#' 之后的第一个字符（标题文字起点）
   pEnd  —— 行尾（不含换行符）
   wszOut/ cchOut —— 输出缓冲
--------------------------------------------------------------------------- */
static void BuildItemText(const char *q, const char *pEnd, int iLevel,
                          WCHAR *wszOut, int cchOut)
{
  char sz[512];
  int  n = 0;
  int  i, nPad;

  wszOut[0] = L'\0';
  if (q && pEnd && q < pEnd) {

    /* CRLF 文件：行尾可能带 \r，先剔掉，否则标题文字末尾会出现回车 */
    while (pEnd > q && (*(pEnd-1) == '\r' || *(pEnd-1) == '\n'))
      pEnd--;
    if (q >= pEnd)
      pEnd = q;

    /* 跳过文字前空白 */
    while (q < pEnd && (*q == ' ' || *q == '\t'))
      q++;

    while (q < pEnd && n < (int)sizeof(sz) - 1)
      sz[n++] = *q++;
    sz[n] = '\0';

    /* 去尾部空白 */
    while (n > 0 && (sz[n-1] == ' ' || sz[n-1] == '\t'))
      sz[--n] = '\0';

    /* 去闭合式 ATX 的结尾 '#'（如 "## 标题 ##"）*/
    if (n > 0 && sz[n-1] == '#') {
      int m = n;
      while (m > 0 && sz[m-1] == '#')
        m--;
      if (m == 0 || sz[m-1] == ' ' || sz[m-1] == '\t') {
        while (m > 0 && (sz[m-1] == ' ' || sz[m-1] == '\t'))
          m--;
        n = m;
        sz[n] = '\0';
      }
    }
  }

  /* 转宽字符；空标题给个占位，免得列表里出现空行 */
  {
    WCHAR wszText[512];
    int cch;
    wszText[0] = L'\0';
    if (n > 0) {
      /* 注意：传显式字节数时 MultiByteToWideChar 不会补 NUL，
         必须自己终结，否则后面 lstrcpy 会把未初始化内存当文字拷出去 */
      cch = MultiByteToWideChar(CP_UTF8, 0, sz, n, wszText, COUNTOF(wszText) - 1);
      if (cch > 0)
        wszText[cch] = L'\0';
      else
        wszText[0] = L'\0';
    }
    else
      lstrcpy(wszText, L"(空标题)");

    /* 每级两个全角空格 */
    if (iLevel < 1) iLevel = 1;
    if (iLevel > 6) iLevel = 6;
    nPad = (iLevel - 1) * 2;

    if (nPad + (int)lstrlen(wszText) >= cchOut - 1)
      nPad = cchOut - 1 - (int)lstrlen(wszText);
    if (nPad < 0)
      nPad = 0;

    for (i = 0; i < nPad; i++)
      wszOut[i] = 0x3000;         /* 全角空格 U+3000 */
    lstrcpy(wszOut + nPad, wszText);
  }
}


/* ---------------------------------------------------------------------------
   扫描文本，重建 g_aItems / g_nItems
--------------------------------------------------------------------------- */
static void Rebuild(const char *pText)
{
  const char *p = pText;
  int iLine = 0;
  int iFence = 0;      /* 围栏长度，0 表示不在围栏内 */

  g_nItems = 0;

  if (!pText)
    return;

  while (*p) {
    const char *pEnd = p;
    const char *q;
    int iIndent = 0;
    int iRun;

    while (*pEnd && *pEnd != '\n')
      pEnd++;

    q = p;
    while (q < pEnd && *q == ' ') {
      q++;
      iIndent++;
    }

    /* 围栏代码块的开头 / 结尾，跳过 */
    if (pEnd - q >= 3 && (*q == '`' || *q == '~')) {
      char chFence = *q;
      iRun = 0;
      while (q + iRun < pEnd && q[iRun] == chFence)
        iRun++;
      if (iRun >= 3) {
        if (iFence == 0)
          iFence = iRun;
        else if (iRun >= iFence)
          iFence = 0;
        goto next_line;
      }
    }

    if (iFence == 0 && iIndent < 4 && *q == '#') {
      iRun = 0;
      while (q + iRun < pEnd && q[iRun] == '#')
        iRun++;
      if (iRun >= 1 && iRun <= 6 &&
          (q + iRun >= pEnd || q[iRun] == ' ' || q[iRun] == '\t')) {
        if (g_nItems < OUTLINE_MAX_ITEMS) {
          WCHAR wsz[600];
          BuildItemText(q + iRun, pEnd, iRun, wsz, COUNTOF(wsz));
          if (ListBox_AddString(g_hwndList, wsz) >= 0) {
            /* 把行号挂到列表项上，双击时直接取，不必在模块里维护索引 */
            ListBox_SetItemData(g_hwndList,g_nItems,(LPARAM)iLine);
            g_aItems[g_nItems].iLine  = iLine;
            g_aItems[g_nItems].iLevel = iRun;
            g_nItems++;
          }
        }
      }
    }

  next_line:
    if (*pEnd == '\n') {
      p = pEnd + 1;
      iLine++;
    }
    else
      break;
  }
}


/* ---------------------------------------------------------------------------
   子类化 ListBox：把双击转成通知发给面板
--------------------------------------------------------------------------- */
static LRESULT CALLBACK OutlineListProc(HWND hwnd, UINT umsg,
                                        WPARAM wParam, LPARAM lParam)
{
  if (umsg == WM_LBUTTONDBLCLK) {
    int idx = ListBox_GetCurSel(hwnd);
    if (idx != LB_ERR) {
      HWND hwndPanel = GetParent(hwnd);
      if (hwndPanel)
        SendMessage(GetParent(hwndPanel), WM_COMMAND,
                    MAKEWPARAM(IDM_OUTLINE_GOTO, 0), (LPARAM)idx);
    }
    return 0;
  }
  return CallWindowProc(g_pfnListProc, hwnd, umsg, wParam, lParam);
}


/* ---------------------------------------------------------------------------
   面板窗口过程
--------------------------------------------------------------------------- */
static LRESULT CALLBACK OutlineWndProc(HWND hwnd, UINT umsg,
                                       WPARAM wParam, LPARAM lParam)
{
  switch (umsg) {

    case WM_CREATE:
      g_hwndPanel = hwnd;
      g_hwndList = CreateWindowEx(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
                    LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                    0, 0, 0, 0, hwnd, (HMENU)IDC_OUTLINE_LIST, g_hinst, NULL);
      if (!g_hwndList)
        return -1;

      /* 用系统字体，避免默认的丑陋点阵 */
      {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessage(g_hwndList, WM_SETFONT, (WPARAM)hFont, FALSE);
      }

      g_pfnListProc = (WNDPROC)SetWindowLongPtr(g_hwndList, GWLP_WNDPROC,
                                             (LONG_PTR)OutlineListProc);
      return 0;

    case WM_SIZE:
      if (g_hwndList)
        MoveWindow(g_hwndList, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
      return 0;

    case WM_SETFONT:
      /* 主窗口设置字体时同步给列表 */
      if (g_hwndList)
        SendMessage(g_hwndList, WM_SETFONT, wParam, lParam);
      return 0;

    case WM_DESTROY:
      g_hwndPanel = NULL;
      g_hwndList  = NULL;
      return 0;
  }
  return DefWindowProc(hwnd, umsg, wParam, lParam);
}


/* ---------------------------------------------------------------------------
   公开接口
--------------------------------------------------------------------------- */

BOOL Outline_Init(HINSTANCE hinst)
{
  WNDCLASSEX wc;

  g_hinst = hinst;

  if (g_atomClass)
    return TRUE;

  memset(&wc, 0, sizeof(wc));
  wc.cbSize        = sizeof(wc);
  wc.style         = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc   = OutlineWndProc;
  wc.hInstance     = hinst;
  wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = L"Notepad2mdOutlinePanel";

  g_atomClass = RegisterClassEx(&wc);
  return (g_atomClass != 0);
}


HWND Outline_Create(HWND hwndParent, HINSTANCE hinst)
{
  if (!g_atomClass && !Outline_Init(hinst))
    return NULL;

  return CreateWindowEx(0, L"Notepad2mdOutlinePanel", NULL,
                        WS_CHILD | WS_CLIPCHILDREN,
                        0, 0, 0, 0, hwndParent, NULL, hinst, NULL);
}


void Outline_Update(HWND hwnd, const char *mdText)
{
  if (!hwnd || !g_hwndList)
    return;

  ListBox_ResetContent(g_hwndList);
  Rebuild(mdText);

  /* 默认选中第一项，方便一眼看到文档开头 */
  if (g_nItems > 0)
    ListBox_SetCurSel(g_hwndList, 0);
}


int Outline_GetLine(HWND hwnd, int idx)
{
  if (idx < 0 || idx >= g_nItems)
    return -1;
  return g_aItems[idx].iLine;
}


void Outline_SetCurrent(HWND hwnd, int idx)
{
  if (!g_hwndList)
    return;

  if (idx < 0 || idx >= g_nItems) {
    ListBox_SetCurSel(g_hwndList, -1);
    return;
  }

  ListBox_SetCurSel(g_hwndList, idx);
  /* 往上留一点余量，避免选中项贴顶看不见 */
  ListBox_SetTopIndex(g_hwndList, idx > 2 ? idx - 2 : 0);
}
