/******************************************************************************
*
* notepad2md
*
* RunCmd.c
*   「运行当前文件」：起一个无窗口子进程，把 stdout/stderr 增量灌进预览窗格。
*
*   约束（跟产品定位一致）：
*     · 不建任何新窗口/控件，输出复用 MSHTML 预览窗格
*     · 子进程 → 双管道 → 读线程 → PostMessage → 主线程追加 DOM
*     · 只支持 4 个变量：%FILE% %DIR% %NAME% %EXT%
*     · 5000 行截断；UTF-8 严格校验优先，失败回退系统 ACP
*
*   为什么追加而不是每次重建 innerHTML：
*     每次重设 #runlog 的 innerHTML 是 O(n) 字符串解析，1 万行时累计 O(n²)，
*     实测会卡死。这里用 insertAdjacentHTML('beforeend', delta) 只解析增量。
*
*                                    Notepad2 (c) Florian Balmer 1996-2011
*                                    notepad2md (c) SorenBoot 2025-2026
*
******************************************************************************/

#include <windows.h>
#include <mshtml.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scintilla.h"
#include "RunCmd.h"
#include "Preview.h"
#include "Notepad2.h"
#include "Helpers.h"
#include "resource.h"

/* Notepad2.c 里的跨模块全局（该文件未导出声明，这里直接 extern）*/
extern HWND  hwndMain;
extern HWND  hwndPreview;
extern BOOL  bShowPreview;
extern BOOL  bPreviewDocLoaded;
extern BOOL  bSaveBeforeRunningTools;

#define RUNCMD_MAX_LINES     5000             /* 渲染上限，超出截断 */
#define RUNCMD_LINE_BUF      8192             /* 单行缓冲；超长行按段切 */
#define RUNCMD_FLUSH_MS      120              /* DOM 追加节流 */
#define RUNCMD_LOG_BUDGET    (8*1024*1024)    /* 日志上限，防内存失控 */


/* ---------------------------------------------------------------------------
   状态
--------------------------------------------------------------------------- */

static HWND   g_hwndMain    = NULL;
static HWND   g_hwndStatus  = NULL;
static WCHAR  g_wszFile[MAX_PATH] = L"";

static BOOL   g_bRunning       = FALSE;
static BOOL   g_bCancel        = FALSE;
static BOOL   g_bOutputMode    = FALSE;   /* 预览窗格归运行输出所有 */
/* g_pDelta 不清空：它同时充当「重放源」。g_cbDeltaSent 记录已经灌进
   DOM 的水位，flush 只发新增部分，整页重建后从 0 整体重发。
   这样重放的是原始 HTML，不用再一次转义（转义会把 <span> 打回纯文本）。*/
static int    g_cbDeltaSent = 0;
static int    g_iLogEnc = 0;   /* 输出编码：0=自动 1=UTF-8 2=ACP（runcmd.ini 可覆盖）*/
static BOOL   g_bRunningShown  = FALSE;   /* 输出文档外壳是否已灌入 */

static HANDLE g_hProc     = NULL;
static HANDLE g_hOutRd    = NULL;
static HANDLE g_hErrRd    = NULL;
/* 给子进程接一根 stdin 管道。以前 si.hStdInput = NULL，子进程拿到的是
   无效句柄：批处理里的 set /p、pause 一律读不到东西，表现为「不能输入」。
   现在父进程持有写端，用户在输出页底部输入框敲一行就写进去。*/
static HANDLE g_hInWr     = NULL;
static DWORD  g_dwExitCode = 0;
static DWORD  g_dwStartTick = 0;

/* 纯文本日志 */
static char  *g_pLog = NULL;
static int    g_cbLog = 0, g_cbLogCap = 0;

/* 待追加到 DOM 的 HTML 增量 */
static char  *g_pDelta = NULL;
static int    g_cbDelta = 0, g_cbDeltaCap = 0;
/* delta 里是否已经开过行。MSHTML 的 insertAdjacentHTML 会把 HTML 源码里的
   换行符当普通空白吞掉（实测三行输出渲染成一行），所以换行必须写成显式
   <br>；换行放在「行首」而不是「行尾」，最后一行结束才不会多出空行。*/
static BOOL   g_bDeltaAnyLine = FALSE;

static int    g_iLines = 0;               /* 已渲染行数 */
static BOOL   g_bTruncated = FALSE;
static BOOL   g_bTruncNotice = FALSE;
static BOOL   g_bDirty = FALSE;
static BOOL   g_bHeaderDirty = FALSE;

/* 需要在 delta 里重放既有内容（整页重建后）*/
static BOOL   g_bReplayAll = FALSE;


/* ---------------------------------------------------------------------------
   可增长缓冲区
--------------------------------------------------------------------------- */

static BOOL BufReserve(char **pp, int *pCap, int cbNeed)
{
  char *p;
  int cap;

  if (cbNeed <= *pCap)
    return TRUE;
  cap = *pCap ? *pCap : 4096;
  while (cap < cbNeed)
    cap *= 2;
  p = (char*)realloc(*pp,(size_t)cap);
  if (!p)
    return FALSE;
  *pp = p;
  *pCap = cap;
  return TRUE;
}

static BOOL BufAppend(char **pp, int *pLen, int *pCap, const char *data, int cb)
{
  if (cb <= 0)
    return TRUE;
  if (!BufReserve(pp,pCap,*pLen + cb + 1))
    return FALSE;
  memcpy(*pp + *pLen,data,(size_t)cb);
  *pLen += cb;
  (*pp)[*pLen] = '\0';
  return TRUE;
}

static BOOL BufAppendStr(char **pp, int *pLen, int *pCap, const char *s)
{
  return BufAppend(pp,pLen,pCap,s,(int)strlen(s));
}


/* ---------------------------------------------------------------------------
   UTF-8 严格校验 / 解码
--------------------------------------------------------------------------- */

/* 合法返回序列字节数，非法返回 -1。拒绝超长编码、代理区、>U+10FFFF。 */
static int Utf8CheckSeq(const unsigned char *p, int n)
{
  int need, i;
  unsigned cp;
  unsigned char c = p[0];

  if (c < 0x80)
    return 1;
  if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; if (cp < 2) return -1; }
  else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
  else return -1;

  if (n < need + 1)
    return -1;
  for (i = 1; i <= need; i++) {
    if ((p[i] & 0xC0) != 0x80)
      return -1;
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  if (need == 2 && cp < 0x800)    return -1;
  if (need == 3 && cp < 0x10000)  return -1;
  if (cp >= 0xD800 && cp <= 0xDFFF) return -1;
  if (cp > 0x10FFFF) return -1;
  return need + 1;
}

static BOOL Utf8IsValid(const char *s, int n)
{
  int i = 0;
  while (i < n) {
    int k = Utf8CheckSeq((const unsigned char*)s + i,n - i);
    if (k < 0)
      return FALSE;
    i += k;
  }
  return TRUE;
}

/* 把一段字节按指定代码页转成 UTF-8 追加。iEnc: 1=UTF-8, 2=ACP */
static BOOL AppendWithEnc(char **dst, int *pLen, int *pCap,
                          const char *src, int n, UINT uCP)
{
  int cchW, cbUtf8;
  WCHAR *pwch;
  char *putf8;
  BOOL ok;

  if (n <= 0)
    return TRUE;
  cchW = MultiByteToWideChar(uCP,0,src,n,NULL,0);
  if (cchW <= 0)
    return BufAppend(dst,pLen,pCap,src,n);
  pwch = (WCHAR*)malloc(((size_t)cchW + 1) * sizeof(WCHAR));
  if (!pwch)
    return FALSE;
  MultiByteToWideChar(uCP,0,src,n,pwch,cchW);
  cbUtf8 = WideCharToMultiByte(CP_UTF8,0,pwch,cchW,NULL,0,NULL,NULL);
  if (cbUtf8 <= 0) {
    free(pwch);
    return BufAppend(dst,pLen,pCap,src,n);
  }
  putf8 = (char*)malloc((size_t)cbUtf8);
  if (!putf8) {
    free(pwch);
    return FALSE;
  }
  WideCharToMultiByte(CP_UTF8,0,pwch,cchW,putf8,cbUtf8,NULL,NULL);
  ok = BufAppend(dst,pLen,pCap,putf8,cbUtf8);
  free(putf8);
  free(pwch);
  return ok;
}

/* 统计宽串里 CJK 字数与「可疑区」字数（U+0080..U+07FF）。
   可疑区就是 GBK 字节被误当 UTF-8 解出来的典型落点。 */
static void ScoreWide(const WCHAR *w, int n, int *pCjk, int *pSus)
{
  int i, cjk = 0, sus = 0;
  for (i = 0; i < n; i++) {
    WCHAR c = w[i];
    if ((c >= 0x3040 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
        (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF))
      cjk++;
    else if (c >= 0x0080 && c <= 0x07FF)
      sus++;
  }
  *pCjk = cjk;
  *pSus = sus;
}

/* 判定这行该按 UTF-8 还是 ACP 解。

   为什么不能只做「UTF-8 严格校验」：GBK 中文的字节经常本身就是合法
   UTF-8 序列（例如 中文 = d6 d0 ce c4），严格校验会通过，然后被解成
   一串希腊/西里尔字母 —— 就是用户看到的乱码。所以要再看解出来的字
   落在哪个区：出现 CJK 才是真 UTF-8；只有可疑区、且按 ACP 解能解出
   CJK，就说明原本是 ACP。 */
static BOOL LooksLikeUtf8(const char *src, int n)
{
  int cchW, cjk = 0, sus = 0, cjkA = 0, susA = 0;
  WCHAR *pwch;
  BOOL b;

  if (g_iLogEnc == 2)
    return FALSE;
  if (g_iLogEnc == 1)
    return TRUE;
  if (!Utf8IsValid(src,n))
    return FALSE;

  cchW = MultiByteToWideChar(CP_UTF8,0,src,n,NULL,0);
  if (cchW <= 0)
    return TRUE;
  pwch = (WCHAR*)malloc(((size_t)cchW + 1) * sizeof(WCHAR));
  if (!pwch)
    return TRUE;
  MultiByteToWideChar(CP_UTF8,0,src,n,pwch,cchW);
  ScoreWide(pwch,cchW,&cjk,&sus);
  free(pwch);

  if (cjk > 0 || sus == 0)
    return TRUE;

  /* 只有可疑区：按 ACP 再解一次，能出 CJK 就判给 ACP */
  cchW = MultiByteToWideChar(CP_ACP,0,src,n,NULL,0);
  if (cchW <= 0)
    return TRUE;
  pwch = (WCHAR*)malloc(((size_t)cchW + 1) * sizeof(WCHAR));
  if (!pwch)
    return TRUE;
  MultiByteToWideChar(CP_ACP,0,src,n,pwch,cchW);
  ScoreWide(pwch,cchW,&cjkA,&susA);
  free(pwch);
  b = (BOOL)(cjkA <= cjk);
  return b;
}

/* 追加解码后的文本。编码判定见 LooksLikeUtf8。 */
static BOOL AppendDecoded(char **dst, int *pLen, int *pCap,
                          const char *src, int n)
{
  if (n <= 0)
    return TRUE;
  if (LooksLikeUtf8(src,n))
    return BufAppend(dst,pLen,pCap,src,n);
  return AppendWithEnc(dst,pLen,pCap,src,n,CP_ACP);
}



/* ---------------------------------------------------------------------------
   HTML 转义
--------------------------------------------------------------------------- */

static BOOL HtmlAppendEscaped(char **pp, int *pLen, int *pCap,
                              const char *s, int n)
{
  int i, run = 0;

  for (i = 0; i < n; i++) {
    const char *rep = NULL;

    switch (s[i]) {
      case '&':  rep = "&amp;";  break;
      case '<':  rep = "&lt;";   break;
      case '>':  rep = "&gt;";   break;
      case '"':  rep = "&quot;"; break;
      default:   break;
    }
    if (!rep) {
      run++;
      continue;
    }
    if (run > 0) {
      if (!BufAppend(pp,pLen,pCap,s + i - run,run))
        return FALSE;
      run = 0;
    }
    if (!BufAppendStr(pp,pLen,pCap,rep))
      return FALSE;
  }
  if (run > 0 && !BufAppend(pp,pLen,pCap,s + n - run,run))
    return FALSE;
  return TRUE;
}

/* 在输出流里另起一行。HTML 源码里的 "\n" 会被 MSHTML 当空白吃掉，
   只有 <br> 才是真换行；而 <br> 写在行首，收尾就不会多空行。 */
static void DeltaNewLine(void)
{
  if (g_bDeltaAnyLine)
    BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,"<br>");
  g_bDeltaAnyLine = TRUE;
}


/* ---------------------------------------------------------------------------
   错误行识别
--------------------------------------------------------------------------- */

static BOOL Contains(const char *hay, const char *needle)
{
  return hay && strstr(hay,needle) != NULL;
}

/* 错误定位行：
     File "D:\tmp\t.py", line 5, in main
     src\Foo.cs(12,34): error CS0103: ...
     at Foo.Bar() in C:\x\Foo.cs:line 12
     at Object.<anonymous> (D:\t.js:3:9)                          */
static BOOL IsErrorLine(const char *line)
{
  if (!line || !*line)
    return FALSE;
  if (Contains(line,"File \"") && Contains(line,"line "))
    return TRUE;
  if (Contains(line,": error ") || Contains(line,": fatal error "))
    return TRUE;
  if (Contains(line," in ") && Contains(line,":line "))
    return TRUE;
  if (!strncmp(line,"    at ",7) && Contains(line,":"))
    return TRUE;
  if (!strncmp(line,"at ",3) && Contains(line,":"))
    return TRUE;
  return FALSE;
}

/* 错误标题行（Traceback 首行、异常名行）*/
static BOOL IsErrorHead(const char *line)
{
  const char *p;

  if (!line || !*line)
    return FALSE;
  if (Contains(line,"Traceback (most recent call last)"))
    return TRUE;

  /* "NameError: name 'x' is not defined" —— 首段全为标识符/点，后接 ": " */
  p = line;
  while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
         (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == ' ')
    p++;
  if (p > line && p[0] == ':' && p[1] == ' ')
    return TRUE;
  return FALSE;
}


/* ---------------------------------------------------------------------------
   DOM 操作
--------------------------------------------------------------------------- */

static IHTMLElement* GetElementById_(const WCHAR *wszId)
{
  IWebBrowser2 *pWB;
  IDispatch *pDisp = NULL;
  IHTMLDocument3 *pDoc3 = NULL;
  IHTMLElement *pEl = NULL;
  BSTR bstrId;

  pWB = Preview_GetBrowser();
  if (!pWB)
    return NULL;
  if (FAILED(pWB->lpVtbl->get_Document(pWB,&pDisp)) || !pDisp)
    return NULL;
  if (FAILED(pDisp->lpVtbl->QueryInterface(pDisp,&IID_IHTMLDocument3,
                                           (void**)&pDoc3)) || !pDoc3) {
    pDisp->lpVtbl->Release(pDisp);
    return NULL;
  }
  bstrId = SysAllocString(wszId);
  if (bstrId) {
    if (FAILED(pDoc3->lpVtbl->getElementById(pDoc3,bstrId,&pEl)))
      pEl = NULL;
    SysFreeString(bstrId);
  }
  pDoc3->lpVtbl->Release(pDoc3);
  pDisp->lpVtbl->Release(pDisp);
  return pEl;
}

/* UTF-8 → BSTR */
static BSTR BstrFromUtf8(const char *s, int cb)
{
  int cchW;
  WCHAR *pwch;
  BSTR b;

  if (!s || cb <= 0)
    return SysAllocString(L"");
  cchW = MultiByteToWideChar(CP_UTF8,0,s,cb,NULL,0);
  if (cchW <= 0)
    return SysAllocString(L"");
  pwch = (WCHAR*)malloc(((size_t)cchW + 1) * sizeof(WCHAR));
  if (!pwch)
    return SysAllocString(L"");
  MultiByteToWideChar(CP_UTF8,0,s,cb,pwch,cchW);
  pwch[cchW] = L'\0';
  b = SysAllocStringLen(pwch,(UINT)cchW);
  free(pwch);
  return b;
}

/* 取 #runlog；万一取不到（文档还没就绪、或被整页重建掉了），
   就在 #md-content 下补一个，保证输出不会静默丢失。 */
static IHTMLElement* GetLogElement(void)
{
  IHTMLElement *pEl = GetElementById_(L"runlog");
  IHTMLElement *pParent;
  IDispatch *pDisp = NULL;
  IHTMLDocument2 *pDoc = NULL;
  IWebBrowser2 *pWB;
  BSTR bstrTag = NULL, bstrId = NULL;
  IHTMLDOMNode *pParentNode = NULL, *pChildNode = NULL;

  if (pEl)
    return pEl;

  pWB = Preview_GetBrowser();
  if (!pWB)
    return NULL;
  if (FAILED(pWB->lpVtbl->get_Document(pWB,&pDisp)) || !pDisp)
    return NULL;
  if (FAILED(pDisp->lpVtbl->QueryInterface(pDisp,&IID_IHTMLDocument2,
                                           (void**)&pDoc)) || !pDoc) {
    pDisp->lpVtbl->Release(pDisp);
    return NULL;
  }

  pParent = GetElementById_(L"md-content");
  if (pParent) {
    bstrTag = SysAllocString(L"pre");
    bstrId  = SysAllocString(L"runlog");
    if (bstrTag && bstrId &&
        SUCCEEDED(pDoc->lpVtbl->createElement(pDoc,bstrTag,&pEl)) && pEl) {
      pEl->lpVtbl->put_id(pEl,bstrId);
      /* 追加一个空文本节点，绕开 MSHTML「空元素查不到」的限制 */
      if (SUCCEEDED(pParent->lpVtbl->QueryInterface(pParent,&IID_IHTMLDOMNode,
                                                    (void**)&pParentNode)) &&
          pParentNode &&
          SUCCEEDED(pEl->lpVtbl->QueryInterface(pEl,&IID_IHTMLDOMNode,
                                                (void**)&pChildNode)) &&
          pChildNode) {
        pParentNode->lpVtbl->appendChild(pParentNode,pChildNode,NULL);
        pChildNode->lpVtbl->Release(pChildNode);
      }
      if (pParentNode) pParentNode->lpVtbl->Release(pParentNode);
    }
    if (bstrTag) SysFreeString(bstrTag);
    if (bstrId)  SysFreeString(bstrId);
    pParent->lpVtbl->Release(pParent);
  }
  pDoc->lpVtbl->Release(pDoc);
  pDisp->lpVtbl->Release(pDisp);
  return pEl;
}

static BOOL AppendHtmlToLog(const char *html, int cbHtml)
{
  IHTMLElement *pEl;
  BSTR bstrWhere, bstrHtml;
  BOOL ok = FALSE;

  pEl = GetLogElement();
  if (!pEl)
    return FALSE;

  bstrWhere = SysAllocString(L"beforeend");
  bstrHtml  = BstrFromUtf8(html,cbHtml);
  if (bstrWhere && bstrHtml)
    ok = SUCCEEDED(pEl->lpVtbl->insertAdjacentHTML(pEl,bstrWhere,bstrHtml));
  if (bstrWhere) SysFreeString(bstrWhere);
  if (bstrHtml)  SysFreeString(bstrHtml);
  pEl->lpVtbl->Release(pEl);
  return ok;
}

static BOOL SetElementHtml(const WCHAR *wszId, const char *html, int cbHtml)
{
  IHTMLElement *pEl;
  BSTR bstrHtml;
  BOOL ok = FALSE;

  pEl = GetElementById_(wszId);
  if (!pEl)
    return FALSE;
  bstrHtml = BstrFromUtf8(html,cbHtml);
  if (bstrHtml)
    ok = SUCCEEDED(pEl->lpVtbl->put_innerHTML(pEl,bstrHtml));
  if (bstrHtml)
    SysFreeString(bstrHtml);
  pEl->lpVtbl->Release(pEl);
  return ok;
}

static BOOL ExecJs(const WCHAR *wszCode)
{
  IWebBrowser2 *pWB;
  IDispatch *pDisp = NULL;
  IHTMLDocument2 *pDoc2 = NULL;
  IHTMLWindow2 *pWin = NULL;
  BSTR bstrCode = NULL, bstrLang = NULL;
  VARIANT vRet;
  BOOL ok = FALSE;

  pWB = Preview_GetBrowser();
  if (!pWB)
    return FALSE;
  if (FAILED(pWB->lpVtbl->get_Document(pWB,&pDisp)) || !pDisp)
    return FALSE;
  if (FAILED(pDisp->lpVtbl->QueryInterface(pDisp,&IID_IHTMLDocument2,
                                           (void**)&pDoc2)) || !pDoc2) {
    pDisp->lpVtbl->Release(pDisp);
    return FALSE;
  }
  if (SUCCEEDED(pDoc2->lpVtbl->get_parentWindow(pDoc2,&pWin)) && pWin) {
    bstrCode = SysAllocString(wszCode);
    bstrLang = SysAllocString(L"JavaScript");
    if (bstrCode && bstrLang) {
      VariantInit(&vRet);
      ok = SUCCEEDED(pWin->lpVtbl->execScript(pWin,bstrCode,bstrLang,&vRet));
      VariantClear(&vRet);
    }
    if (bstrCode) SysFreeString(bstrCode);
    if (bstrLang) SysFreeString(bstrLang);
    pWin->lpVtbl->Release(pWin);
  }
  if (pDoc2) pDoc2->lpVtbl->Release(pDoc2);
  pDisp->lpVtbl->Release(pDisp);
  return ok;
}

/* 滚到底（页面里 __mdLock 为真时不跟随）*/
static void ScrollToBottom(void)
{
  ExecJs(L"try{if(!window.__mdLock){var d=document.documentElement,b=document.body;"
         L"if(d)d.scrollTop=d.scrollHeight;if(b)b.scrollTop=b.scrollHeight;}}catch(e){}");
}


/* ---------------------------------------------------------------------------
   输出文档外壳
--------------------------------------------------------------------------- */

static const char CSS_RUN[] =
  "html,body{align-items:stretch;}\n"
  "body{font-family:'Segoe UI',Tahoma,sans-serif;font-size:13px;color:#1a1a1a;"
    "background:#fff;margin:0;padding:0;}\n"
  "#runhead{display:flex;align-items:center;gap:8px;padding:5px 10px;"
    "background:#fafafa;border-bottom:1px solid #e4e4e4;font-size:12px;color:#6a6a6a;"
    "position:sticky;top:0;z-index:2;}\n"
  "#runhead .dot{width:8px;height:8px;border-radius:50%;background:#c8c8c8;"
    "flex:0 0 auto;display:inline-block;}\n"
  "#runhead .dot.run{background:#f0a000;}\n"
  "#runhead .dot.ok{background:#107c10;}\n"
  "#runhead .dot.err{background:#c50f1f;}\n"
  "#runhead .dot.stop{background:#c04a00;}\n"
  "#runhead .mode{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;"
    "white-space:nowrap;}\n"
  "#runhead button{font-family:inherit;font-size:11px;padding:2px 8px;background:#fff;"
    "border:1px solid #cfcfcf;border-radius:3px;color:#444;cursor:pointer;}\n"
  "#runhead button:hover{background:#eef4fa;border-color:#b8d7f0;color:#17517e;}\n"
  "#runhead button.on{background:#dceaf8;border-color:#7fb3e0;color:#10456e;"
    "font-weight:bold;}\n"
  "#runlog{margin:0;padding:10px 14px 62px;font-family:Consolas,'Courier New',monospace;"
    "font-size:13px;line-height:1.55;white-space:pre-wrap;word-break:break-all;}\n"
  /* 底部输入条：给子进程的 stdin 用（批处理的 set /p、pause 靠它）*/
  "#runinbar{position:fixed;left:0;right:0;bottom:0;display:flex;gap:6px;"
    "align-items:center;padding:6px 10px;background:#fafafa;"
    "border-top:1px solid #e4e4e4;z-index:3;}\n"
  "#runin{flex:1;min-width:0;font-family:Consolas,'Courier New',monospace;"
    "font-size:12px;padding:3px 6px;border:1px solid #cfcfcf;border-radius:3px;"
    "color:#1a1a1a;}\n"
  "#runin:focus{outline:none;border-color:#7fb3e0;}\n"
  "#runinbar button{font-family:inherit;font-size:11px;padding:3px 10px;"
    "background:#fff;border:1px solid #cfcfcf;border-radius:3px;color:#444;"
    "cursor:pointer;}\n"
  "#runinbar button:hover{background:#eef4fa;border-color:#b8d7f0;color:#17517e;}\n"
  ".run-cmd{color:#8a8a8a;}\n"
  ".run-in{color:#0a6ebd;}\n"
  ".run-ok{color:#107c10;font-weight:bold;}\n"
  ".run-err{color:#c50f1f;}\n"
  ".run-stop{color:#c04a00;font-weight:bold;}\n"
  ".run-errline{background:#fdeaea;}\n";

static const char JS_BRIDGE[] =
  "window.__mdCopyLog=function(){try{"
    "var t=document.getElementById('runlog').innerText;"
    "window.external.MDCopy(t);}catch(e){}"
    "var b=document.getElementById('btnCopy');"
    "if(b){b.innerHTML='\xe5\xb7\xb2\xe5\xa4\x8d\xe5\x88\xb6';"
    "setTimeout(function(){b.innerHTML='\xe5\xa4\x8d\xe5\x88\xb6';},1200);}};"
  "window.__mdSaveLog=function(){try{window.external.MDSaveLog();}catch(e){}};"
  "window.__mdSendInput=function(){try{"
    "var i=document.getElementById('runin');if(!i)return;"
    "window.external.MDInput(i.value);i.value='';}"
    "catch(e){}};"
  "window.__mdEndInput=function(){try{window.external.MDInputEOF();}catch(e){}};"
  "window.__mdToggleLock=function(){window.__mdLock=!window.__mdLock;"
    "var b=document.getElementById('btnLock');if(b){"
    "b.className=window.__mdLock?'on':'';"
    "b.innerHTML=window.__mdLock?'\xe9\x94\x81\xe5\xae\x9a\xc2\xb7\xe5\xbc\x80'"
    ":'\xe6\xbb\x9a\xe5\x8a\xa8\xe9\x94\x81\xe5\xae\x9a';}};";

/* 状态点 class + 文字后缀 */
static const char* StateClass(void)
{
  if (g_bRunning) return "run";
  if (g_bCancel)  return "stop";
  return (g_dwExitCode == 0) ? "ok" : "err";
}

/* 标题栏 HTML（追加到 out）*/
static void AppendHeader(char **out, int *len, int *cap)
{
  char tchBuf[600];
  char *pszCmd = NULL;

  if (g_wszFile[0]) {
    int cb = WideCharToMultiByte(CP_UTF8,0,g_wszFile,-1,NULL,0,NULL,NULL);
    if (cb > 0) {
      pszCmd = (char*)malloc((size_t)cb);
      if (pszCmd)
        WideCharToMultiByte(CP_UTF8,0,g_wszFile,-1,pszCmd,cb,NULL,NULL);
    }
  }

  BufAppendStr(out,len,cap,"<span class=\"dot ");
  BufAppendStr(out,len,cap,StateClass());
  BufAppendStr(out,len,cap,"\"></span><span class=\"mode\">");

  if (pszCmd) {
    HtmlAppendEscaped(out,len,cap,pszCmd,(int)strlen(pszCmd));
    free(pszCmd);
  }

  if (g_bRunning) {
    /* 不能写 wsprintfA("...%.1fs", double)：wsprintf 不支持浮点，
       会吐出乱码并中途截断，连带把后面的 <pre id="runlog"> 一起吃掉。
       用整数毫秒算「秒.十分之一秒」，既不引 CRT 浮点也更好读。 */
    DWORD dwMs = GetTickCount() - g_dwStartTick;
    wsprintfA(tchBuf," &mdash; running %lu.%lus",
              (unsigned long)(dwMs / 1000),
              (unsigned long)((dwMs % 1000) / 100));
    BufAppendStr(out,len,cap,tchBuf);
  }
  else if (g_bCancel) {
    wsprintfA(tchBuf," &mdash; \xe5\xb7\xb2\xe5\x81\x9c\xe6\xad\xa2 (exit=%lu)",
              (unsigned long)g_dwExitCode);
    BufAppendStr(out,len,cap,tchBuf);
  }
  else {
    wsprintfA(tchBuf," &mdash; exit=%lu",(unsigned long)g_dwExitCode);
    BufAppendStr(out,len,cap,tchBuf);
  }

  BufAppendStr(out,len,cap,"</span>"
    "<button id=\"btnCopy\" type=\"button\" onclick=\"window.__mdCopyLog()\">"
    "\xe5\xa4\x8d\xe5\x88\xb6</button>"
    "<button id=\"btnSave\" type=\"button\" onclick=\"window.__mdSaveLog()\">"
    "\xe5\xad\x98\xe6\x97\xa5\xe5\xbf\x97</button>"
    "<button id=\"btnLock\" type=\"button\" onclick=\"window.__mdToggleLock()\">"
    "\xe6\xbb\x9a\xe5\x8a\xa8\xe9\x94\x81\xe5\xae\x9a</button>");
}

/* 完整输出文档 */
static char* BuildRunDoc(void)
{
  char *doc = NULL;
  int len = 0, cap = 0;

  BufAppendStr(&doc,&len,&cap,
    "<!DOCTYPE html><html><head><meta http-equiv=\"Content-Type\" "
    "content=\"text/html; charset=utf-8\"><style>");
  BufAppendStr(&doc,&len,&cap,CSS_RUN);
  BufAppendStr(&doc,&len,&cap,"</style></head><body><div id=\"md-content\">");
  BufAppendStr(&doc,&len,&cap,"<div id=\"runhead\">");
  AppendHeader(&doc,&len,&cap);
  BufAppendStr(&doc,&len,&cap,/* 这个 <!--log--> 占位不是装饰：MSHTML 的 IHTMLDocument3::getElementById
       对「完全空」的元素返回 NULL（实测 <pre id=runlog></pre> 取不到，
       加一个子节点后立刻可取）。留个注释节点既不可见，又让元素始终可查。 */
    "</div><pre id=\"runlog\"><!--log--></pre>"
    /* 底部输入条：把一行写进子进程的 stdin。批处理的 set /p、pause
       以前因为 stdin 是空句柄直接失败，现在能真正交互。*/
    "<div id=\"runinbar\"><input id=\"runin\" type=\"text\" autocomplete=\"off\""
    " placeholder=\"\xe8\xbe\x93\xe5\x85\xa5\xe5\x90\x8e\xe5\x9b\x9e\xe8\xbd\xa6\xe5\x8f\x91\xe9\x80\x81 stdin\""
    " onkeydown=\"if(event.keyCode==13){window.__mdSendInput();return false;}\">"
    "<button type=\"button\" onclick=\"window.__mdSendInput()\">\xe5\x8f\x91\xe9\x80\x81"
    "</button>"
    /* 结束输入：关掉 stdin 写端，让「读到 EOF 才收尾」的程序能正常退出 */
    "<button type=\"button\" onclick=\"window.__mdEndInput()\">"
    "\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\x93\xe5\x85\xa5</button></div></div>");
  BufAppendStr(&doc,&len,&cap,"<script>");
  BufAppendStr(&doc,&len,&cap,JS_BRIDGE);
  BufAppendStr(&doc,&len,&cap,"</script></body></html>");
  return doc;
}


/* ---------------------------------------------------------------------------
   日志
--------------------------------------------------------------------------- */

static void ResetSession(void)
{
  if (g_pLog)   { free(g_pLog);   g_pLog = NULL; }
  if (g_pDelta) { free(g_pDelta); g_pDelta = NULL; }
  g_cbLog = g_cbLogCap = 0;
  g_cbDelta = g_cbDeltaCap = 0;
  g_cbDeltaSent = 0;
  g_bDeltaAnyLine = FALSE;
  g_iLines = 0;
  g_bTruncated = FALSE;
  g_bTruncNotice = FALSE;
  g_bDirty = FALSE;
  g_bHeaderDirty = FALSE;
  g_bReplayAll = FALSE;
}

/* 接收一行（UTF-8，不含换行）*/
/* 收一行原始字节：先解码成 UTF-8，再分类、转义、入缓冲。
   注意 pszRaw 是管道原始字节，可能是 UTF-8，也可能是 GBK(ACP)。
   以前这里直接用原始字节，等于解码层形同虚设，中文必然乱码。 */
static void AcceptLine(const char *pszRaw, int cbRaw, BOOL bErr)
{
  const char *cls;
  const char *pszUtf8;
  int cbUtf8;
  char *pDec = NULL;
  int cbDec = 0, cbDecCap = 0;

  if (AppendDecoded(&pDec,&cbDec,&cbDecCap,pszRaw,cbRaw)) {
    pszUtf8 = pDec ? pDec : "";
    cbUtf8  = cbDec;
  } else {
    pszUtf8 = pszRaw;
    cbUtf8  = cbRaw;
  }

  if (g_iLines >= RUNCMD_MAX_LINES || g_cbLog >= RUNCMD_LOG_BUDGET) {
    g_bTruncated = TRUE;
    if (!g_bTruncNotice) {
      DeltaNewLine();
      BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,
        "[<span class=\"run-stop\">output truncated at 5000 lines</span>]");
      BufAppendStr(&g_pLog,&g_cbLog,&g_cbLogCap,
                   "\r\n[output truncated at 5000 lines]\r\n");
      g_bTruncNotice = TRUE;
      g_bDirty = TRUE;
    }
    if (pDec) free(pDec);
    return;
  }

  /* 顺序有讲究：先认「错误行」（traceback 的 File "...", line N、
     error CS1234 之类），它们即使走 stderr 也要拿到更醒目的底色；
     没命中再看是不是 stderr，最后才是普通行。 */
  if (IsErrorLine(pszUtf8))
    cls = "run-errline";
  else if (IsErrorHead(pszUtf8) || bErr)
    cls = "run-err";
  else
    cls = NULL;

  DeltaNewLine();
  if (cls) {
    BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,"<span class=\"");
    BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,cls);
    BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,"\">");
  }
  HtmlAppendEscaped(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,pszUtf8,cbUtf8);
  if (cls)
    BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,"</span>");

  BufAppend(&g_pLog,&g_cbLog,&g_cbLogCap,pszUtf8,cbUtf8);
  BufAppend(&g_pLog,&g_cbLog,&g_cbLogCap,"\r\n",2);

  if (pDec)
    free(pDec);

  g_iLines++;
  g_bDirty = TRUE;
}


/* ---------------------------------------------------------------------------
   主线程刷新
--------------------------------------------------------------------------- */

/* 只要还有没落地的输出（刚追加失败/整页待重放），就保活刷新定时器。
   以前在 OnDoneMessage 里无条件 KillTimer，最后一次 flush 撞上异步解析就
   永久丢输出 —— 定时器按需续命才是对的。 */
static void EnsureFlushTimer(void)
{
  if (!g_hwndMain)
    return;
  if (g_bDirty || g_bReplayAll)
    SetTimer(g_hwndMain,ID_RUNCMDTIMER,RUNCMD_FLUSH_MS,NULL);
}

void RunCmd_Flush(void)
{
  if (!g_bOutputMode)
    return;

  if (!g_bRunningShown) {
    char *doc = BuildRunDoc();
    if (doc) {
      g_bRunningShown = Preview_SetHtml(hwndPreview,doc);
      free(doc);
    }
    if (!g_bRunningShown)
      return;
    g_bReplayAll = TRUE;
  }

  if (g_bHeaderDirty) {
    char *h = NULL;
    int len = 0, cap = 0;
    g_bHeaderDirty = FALSE;
    AppendHeader(&h,&len,&cap);
    if (h) {
      SetElementHtml(L"runhead",h,len);
      free(h);
    }
  }

  /* 整页重建后要把已积累的内容全部补回去。
     注意：Preview_SetHtml 走 IPersistStreamInit::Load 是异步解析，
     刚返回时新文档往往还没建好，append 会失败。失败就必须保留标志，
     下个 tick 再试 —— 否则这段输出就永久丢了（表现为 #runlog 只剩占位符）。 */
  if (g_bReplayAll) {
    BOOL bDone = TRUE;

    if (g_cbDelta > 0)
      bDone = AppendHtmlToLog(g_pDelta,g_cbDelta);
    if (bDone) {
      g_bReplayAll = FALSE;
      g_cbDeltaSent = g_cbDelta;
      ScrollToBottom();
    }
    return;
  }

  if (g_bDirty && g_cbDelta > g_cbDeltaSent) {
    if (AppendHtmlToLog(g_pDelta + g_cbDeltaSent,g_cbDelta - g_cbDeltaSent)) {
      g_bDirty = FALSE;
      g_cbDeltaSent = g_cbDelta;
      ScrollToBottom();
    }
    /* 追加失败：g_bDirty 保持 TRUE，下个 tick 重试 */
  }
  else if (g_bDirty) {
    g_bDirty = FALSE;
  }

  /* 全部落地了就把定时器停掉，空闲时零开销 */
  if (!g_bDirty && !g_bReplayAll && g_hwndMain)
    KillTimer(g_hwndMain,ID_RUNCMDTIMER);
}


/* ---------------------------------------------------------------------------
   读线程
--------------------------------------------------------------------------- */

typedef struct tagStreamCtx {
  HANDLE hRead;
  BOOL   bErr;
  char   buf[RUNCMD_LINE_BUF];
  int    len;
} StreamCtx;

static void PostLine(BOOL bErr, const char *s, int n)
{
  char *copy = (char*)malloc((size_t)n + 1);
  if (!copy)
    return;
  if (n > 0)
    memcpy(copy,s,(size_t)n);
  copy[n] = '\0';
  if (!g_hwndMain ||
      !PostMessage(g_hwndMain,WM_RUNCMD_OUTPUT,(WPARAM)(bErr ? 1 : 0),(LPARAM)copy))
    free(copy);
}

static void EmitLine(StreamCtx *ctx)
{
  int n = ctx->len;
  if (n > 0 && ctx->buf[n - 1] == '\r')
    n--;
  PostLine(ctx->bErr,ctx->buf,n);
  ctx->len = 0;
}

static DWORD WINAPI ReadThreadProc(LPVOID lpParam)
{
  StreamCtx *ctx = (StreamCtx*)lpParam;
  char buf[4096];
  DWORD nRead;

  while (ReadFile(ctx->hRead,buf,sizeof(buf),&nRead,NULL) && nRead > 0) {
    DWORD i;
    for (i = 0; i < nRead; i++) {
      char c = buf[i];
      if (c == '\n')
        EmitLine(ctx);
      else if (ctx->len < RUNCMD_LINE_BUF - 1)
        ctx->buf[ctx->len++] = c;
      else {
        EmitLine(ctx);          /* 超长行切段，不静默丢弃 */
        ctx->buf[ctx->len++] = c;
      }
    }
  }
  /* 管道 EOF 时残留的 0 长度尾巴不要当空行投递，否则界面会多一条
     莫名其妙的空错误行（表现为末尾一个空的 run-err span）。 */
  if (ctx->len > 0)
    EmitLine(ctx);
  CloseHandle(ctx->hRead);
  free(ctx);
  return 0;
}

static DWORD WINAPI WaitThreadProc(LPVOID lpParam)
{
  HANDLE hProc = (HANDLE)lpParam;
  DWORD dwCode = 0;

  WaitForSingleObject(hProc,INFINITE);
  GetExitCodeProcess(hProc,&dwCode);
  if (g_hwndMain)
    PostMessage(g_hwndMain,WM_RUNCMD_DONE,(WPARAM)dwCode,(LPARAM)hProc);
  else
    CloseHandle(hProc);
  return 0;
}


/* ---------------------------------------------------------------------------
   命令构造
--------------------------------------------------------------------------- */

/* 变量展开。%FILE% 带引号，%DIR%/%NAME%/%EXT% 不带。 */
/* 测试钩子：往日志缓冲里塞一行（跳过管道），用来单测存盘/分类 */
void RunCmd_AppendForTest(const char *pszLine, BOOL bErr)
{
  if (pszLine)
    AcceptLine(pszLine,(int)strlen(pszLine),bErr);
}

/* 测试钩子：暴露解码层，方便单测覆盖「GBK 字节恰好是合法 UTF-8」这类坑 */
int RunCmd_DecodeForTest(const char *pRaw, int cbRaw, char *pOut, int cbOut)
{
  char *buf = NULL;
  int len = 0, cap = 0;
  int n;

  if (!pOut || cbOut <= 0)
    return 0;
  pOut[0] = '\0';
  if (!AppendDecoded(&buf,&len,&cap,pRaw,cbRaw)) {
    if (buf) free(buf);
    return 0;
  }
  n = len < cbOut - 1 ? len : cbOut - 1;
  if (n > 0 && buf)
    memcpy(pOut,buf,(size_t)n);
  pOut[n] = '\0';
  if (buf)
    free(buf);
  return n;
}

void RunCmd_ExpandVarsForTest(const WCHAR *wszFile, const WCHAR *wszIn,
                              WCHAR *wszOut, int cchOut)
{
  WCHAR wszDir[MAX_PATH], wszBase[MAX_PATH], wszExt[64];
  const WCHAR *pDot;
  int n = 0;

  if (!wszOut || cchOut <= 0)
    return;
  wszOut[0] = L'\0';
  if (!wszIn || !wszFile)
    return;

  lstrcpynW(wszDir,wszFile,MAX_PATH);
  PathRemoveFileSpecW(wszDir);

  lstrcpynW(wszBase,PathFindFileNameW(wszFile),MAX_PATH);
  pDot = PathFindExtensionW(wszBase);
  if (pDot && *pDot) {
    lstrcpynW(wszExt,pDot + 1,COUNTOF(wszExt));
    ((WCHAR*)pDot)[0] = L'\0';
  }
  else
    wszExt[0] = L'\0';

  while (*wszIn && n < cchOut - 1) {
    if (*wszIn == L'%') {
      const WCHAR *p = wszIn + 1;
      const WCHAR *q = wcschr(p,L'%');
      if (q && (q - p) > 0 && (q - p) < 16) {
        WCHAR tag[16];
        int i, cnt = (int)(q - p);
        /* 不能把 CharUpperW 直接作用在 p 上：它会原地改写，
           p 可能指向字符串字面量（只读段），会直接崩。 */
        for (i = 0; i < cnt; i++) {
          WCHAR c = p[i];
          tag[i] = (c >= L'a' && c <= L'z') ? (WCHAR)(c - L'a' + L'A') : c;
        }
        tag[cnt] = L'\0';

        if (!lstrcmpW(tag,L"FILE")) {
          /* %FILE% 展开为不带引号的完整路径，引号由模板负责：
             内置模板都已写成 "\"%FILE%\""，用户 ini 里也照此办理。
             这样避免「模板带引号 + 变量也带引号」的双引号事故。 */
          WCHAR full[MAX_PATH];
          lstrcpynW(full,wszFile,MAX_PATH);
          PathUnquoteSpacesW(full);
          n += wsprintfW(wszOut + n,L"%s",full);
          wszIn = q + 1;
          continue;
        }
        if (!lstrcmpW(tag,L"DIR")) {
          n += wsprintfW(wszOut + n,L"%s",wszDir);
          wszIn = q + 1;
          continue;
        }
        if (!lstrcmpW(tag,L"NAME")) {
          n += wsprintfW(wszOut + n,L"%s",wszBase);
          wszIn = q + 1;
          continue;
        }
        if (!lstrcmpW(tag,L"EXT")) {
          n += wsprintfW(wszOut + n,L"%s",wszExt);
          wszIn = q + 1;
          continue;
        }
      }
    }
    wszOut[n++] = *wszIn++;
  }
  wszOut[n] = L'\0';
}

/* 在 dir 及上 3 层找扩展名为 ext 的项目文件（如 .csproj）*/
static BOOL FindUpwards(const WCHAR *wszDir, const WCHAR *wszExt,
                        WCHAR *wszOut, int cchOut)
{
  WCHAR wszCur[MAX_PATH];
  int level;

  lstrcpynW(wszCur,wszDir,MAX_PATH);
  for (level = 0; level < 4; level++) {
    WIN32_FIND_DATAW wfd;
    HANDLE hFind;
    WCHAR wszPattern[MAX_PATH + 16];

    wsprintfW(wszPattern,L"%s\\*%s",wszCur,wszExt);
    hFind = FindFirstFileW(wszPattern,&wfd);
    if (hFind != INVALID_HANDLE_VALUE) {
      BOOL bFound = FALSE;
      do {
        if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
          wsprintfW(wszOut,L"%s\\%s",wszCur,wfd.cFileName);
          bFound = TRUE;
          break;
        }
      } while (FindNextFileW(hFind,&wfd));
      FindClose(hFind);
      if (bFound)
        return TRUE;
    }
    {
      WCHAR wszParent[MAX_PATH];
      lstrcpynW(wszParent,wszCur,MAX_PATH);
      PathRemoveFileSpecW(wszParent);
      if (!*wszParent || !lstrcmpW(wszParent,wszCur))
        break;
      lstrcpynW(wszCur,wszParent,MAX_PATH);
    }
  }
  return FALSE;
}

/* runcmd.ini（exe 同目录）里的 [runcmd] 映射 */
static BOOL IniLookup(const WCHAR *wszExt, WCHAR *wszOut, int cchOut)
{
  WCHAR wszIni[MAX_PATH], wszKey[64], wszVal[MAX_PATH * 2];
  int i;

  GetModuleFileNameW(NULL,wszIni,COUNTOF(wszIni));
  PathRemoveFileSpecW(wszIni);
  PathAppendW(wszIni,L"runcmd.ini");
  if (GetFileAttributesW(wszIni) == INVALID_FILE_ATTRIBUTES)
    return FALSE;

  for (i = 0; wszExt[i] && i < COUNTOF(wszKey) - 1; i++)
    for (i = 0; wszExt[i] && i < COUNTOF(wszKey) - 1; i++) {
      WCHAR c = wszExt[i];
      wszKey[i] = (c >= L'A' && c <= L'Z') ? (WCHAR)(c - L'A' + L'a') : c;
    }
  wszKey[i] = L'\0';

  wszVal[0] = L'\0';
  if (wszKey[0])
    GetPrivateProfileStringW(L"runcmd",wszKey,L"",wszVal,COUNTOF(wszVal),wszIni);
  if (!wszVal[0])
    GetPrivateProfileStringW(L"runcmd",L"default",L"",wszVal,COUNTOF(wszVal),wszIni);
  if (!wszVal[0])
    return FALSE;

  lstrcpynW(wszOut,wszVal,cchOut);
  return TRUE;
}

/* 优先级：runcmd.ini > 扩展名内置映射 > 项目文件探测 */
BOOL RunCmd_BuildCommandForTest(const WCHAR *wszFile, WCHAR *wszOut, int cchOut)
{
  WCHAR wszDir[MAX_PATH], wszExt[64], wszTpl[MAX_PATH * 2];
  const WCHAR *pDot;

  if (!wszFile || !*wszFile || !wszOut || cchOut <= 0)
    return FALSE;
  wszOut[0] = L'\0';

  lstrcpynW(wszDir,wszFile,MAX_PATH);
  PathRemoveFileSpecW(wszDir);

  pDot = PathFindExtensionW(wszFile);
  lstrcpynW(wszExt,(pDot && *pDot) ? pDot + 1 : L"",COUNTOF(wszExt));

  if (IniLookup(wszExt,wszTpl,COUNTOF(wszTpl))) {
    RunCmd_ExpandVarsForTest(wszFile,wszTpl,wszOut,cchOut);
    return wszOut[0] != L'\0';
  }

  if (!lstrcmpiW(wszExt,L"py") || !lstrcmpiW(wszExt,L"pyw")) {
    RunCmd_ExpandVarsForTest(wszFile,L"python \"%FILE%\"",wszOut,cchOut);
    return TRUE;
  }
  if (!lstrcmpiW(wszExt,L"js") || !lstrcmpiW(wszExt,L"mjs") ||
      !lstrcmpiW(wszExt,L"ts")) {
    RunCmd_ExpandVarsForTest(wszFile,L"node \"%FILE%\"",wszOut,cchOut);
    return TRUE;
  }
  if (!lstrcmpiW(wszExt,L"bat") || !lstrcmpiW(wszExt,L"cmd")) {
    RunCmd_ExpandVarsForTest(wszFile,L"cmd /c \"%FILE%\"",wszOut,cchOut);
    return TRUE;
  }
  if (!lstrcmpiW(wszExt,L"ps1")) {
    RunCmd_ExpandVarsForTest(wszFile,
      L"powershell -NoProfile -ExecutionPolicy Bypass -File \"%FILE%\"",
      wszOut,cchOut);
    return TRUE;
  }

  {
    WCHAR wszProj[MAX_PATH], wszProjDir[MAX_PATH];
    if (FindUpwards(wszDir,L".csproj",wszProj,COUNTOF(wszProj)) ||
        FindUpwards(wszDir,L".sln",wszProj,COUNTOF(wszProj))) {
      lstrcpynW(wszProjDir,wszProj,MAX_PATH);
      PathRemoveFileSpecW(wszProjDir);
      wsprintfW(wszOut,L"dotnet run --project \"%s\"",wszProjDir);
      return TRUE;
    }
  }

  return FALSE;
}


/* ---------------------------------------------------------------------------
   启动 / 停止
--------------------------------------------------------------------------- */

static void RunCmd_LogState(const char *tag);
static BOOL RunCmd_LogDom(const char *tag)
{
  const char *path = getenv("N2MD_RUNCMD_LOG");
  const char *domPath = getenv("N2MD_RUNCMD_DOM");
  IHTMLElement *pEl;
  BSTR b = NULL;
  int nLog = -1, nHead = -1, buttons = 0, nInBar = 0;
  FILE *fp;

  if (!path || !path[0])
    return FALSE;

  pEl = GetLogElement();
  if (pEl) {
    if (SUCCEEDED(pEl->lpVtbl->get_innerHTML(pEl,&b)) && b) {
      nLog = (int)wcslen(b);
      if (domPath && domPath[0]) {
        FILE *fd = fopen(domPath,"wb");
        if (fd) {
          int cb = WideCharToMultiByte(CP_UTF8,0,b,-1,NULL,0,NULL,NULL);
          if (cb > 0) {
            char *u = (char*)malloc((size_t)cb);
            if (u) {
              WideCharToMultiByte(CP_UTF8,0,b,-1,u,cb,NULL,NULL);
              fwrite(u,1,(size_t)(cb - 1),fd);
              free(u);
            }
          }
          fclose(fd);
        }
      }
      SysFreeString(b); b = NULL;
    }
    pEl->lpVtbl->Release(pEl);
  }

  pEl = GetElementById_(L"runhead");
  if (pEl) {
    if (SUCCEEDED(pEl->lpVtbl->get_innerHTML(pEl,&b)) && b) {
      nHead = (int)wcslen(b);
      buttons = (wcsstr(b,L"btnCopy") != NULL)
              + (wcsstr(b,L"btnSave") != NULL) * 2
              + (wcsstr(b,L"btnLock") != NULL) * 4;
      SysFreeString(b); b = NULL;
    }
    pEl->lpVtbl->Release(pEl);
  }

  /* 输入条是否存在。它不在 #runhead 里，单独查一遍：
     没有这条记录的话，「输入框坏了」只能靠人眼看出来。*/
  pEl = GetElementById_(L"runinbar");
  if (pEl) {
    if (SUCCEEDED(pEl->lpVtbl->get_innerHTML(pEl,&b)) && b) {
      nInBar = (wcsstr(b,L"runin") != NULL) * 1
             + (wcsstr(b,L"__mdSendInput") != NULL) * 2
             + (wcsstr(b,L"__mdEndInput") != NULL) * 4;
      SysFreeString(b); b = NULL;
    }
    pEl->lpVtbl->Release(pEl);
  }

  /* 探针：把纯文本日志也导出，用于区分「解码错」和「渲染错」 */
  if (domPath && domPath[0] && g_pLog) {
    char tchTxt[MAX_PATH];
    FILE *ft;
    lstrcpynA(tchTxt,domPath,sizeof(tchTxt) - 5);
    lstrcatA(tchTxt,".txt");
    ft = fopen(tchTxt,"wb");
    if (ft) { fwrite(g_pLog,1,(size_t)g_cbLog,ft); fclose(ft); }
  }

  /* 只有真读到 #runlog 才落这一行：测试脚本按 "dom" 前缀等待，
     过早落一行 runlog=-1 会被当成最终结果，误判成没渲染。 */
  if (nLog < 0)
    return FALSE;

  fp = fopen(path,"a");
  if (!fp)
    return FALSE;
  fprintf(fp,"%s runlog=%d runhead=%d buttons=%d inputbar=%d\n",
          tag,nLog,nHead,buttons,nInBar);
  fclose(fp);

  /* 顺带验证 JS 桥：点一次「滚动锁定」，看按钮 class 是否变成 on，再点回来。
     能证明页内脚本、按钮 onclick、window.__mdLock 三者是通的。
     单独落一行，JS 那边出问题也不影响上面那条主记录。 */
  if (!lstrcmpA(tag,"dom")) {
    IHTMLElement *pBtn;
    BSTR bCls = NULL;
    ExecJs(L"__mdToggleLock()");
    pBtn = GetElementById_(L"btnLock");
    if (pBtn) {
      if (SUCCEEDED(pBtn->lpVtbl->get_className(pBtn,&bCls)) && bCls) {
        fp = fopen(path,"a");
        if (fp) { fprintf(fp,"lock class=%ls\n",bCls); fclose(fp); }
        SysFreeString(bCls);
      }
      pBtn->lpVtbl->Release(pBtn);
    }
    ExecJs(L"__mdToggleLock()");

    /* 再验一次「复制」：页内脚本取 innerText 交给 window.external.MDCopy，
       主进程真正写进剪贴板。测试脚本会去读剪贴板核对内容。 */
    ExecJs(L"__mdCopyLog()");
    fp = fopen(path,"a");
    if (fp) { fprintf(fp,"copydone\n"); fclose(fp); }
  }
  return TRUE;
}


/* ---------------------------------------------------------------------------
   对外接口
--------------------------------------------------------------------------- */

BOOL RunCmd_Init(void)
{
  g_bRunning = FALSE;
  g_bCancel = FALSE;
  g_bOutputMode = FALSE;
  g_bRunningShown = FALSE;
  g_hProc = NULL;
  g_hOutRd = g_hErrRd = NULL;
  g_dwExitCode = 0;
  g_dwStartTick = 0;
  ResetSession();
  return TRUE;
}

void RunCmd_Shutdown(void)
{
  if (g_hwndMain)
    KillTimer(g_hwndMain,ID_RUNCMDTIMER);
  if (g_bRunning)
    RunCmd_Stop();
  if (g_hProc) {
    CloseHandle(g_hProc);
    g_hProc = NULL;
  }
  if (g_hOutRd) { CloseHandle(g_hOutRd); g_hOutRd = NULL; }
  if (g_hErrRd) { CloseHandle(g_hErrRd); g_hErrRd = NULL; }
  g_bRunning = FALSE;
  g_bOutputMode = FALSE;
  g_bRunningShown = FALSE;
  ResetSession();
}

void RunCmd_SetCurrentFile(const WCHAR *wszPath)
{
  if (wszPath)
    lstrcpynW(g_wszFile,wszPath,MAX_PATH);
  else
    g_wszFile[0] = L'\0';
}

BOOL RunCmd_IsRunning(void)
{
  return g_bRunning;
}

BOOL RunCmd_OutputActive(void)
{
  return g_bOutputMode;
}

/* ---------------------------------------------------------------------------
   启动进程
--------------------------------------------------------------------------- */

/* 起子进程，stdout/stderr 各一根管道，各配一个读线程，外加一个等退出的线程。
   全程 CREATE_NO_WINDOW —— 不能弹出黑框，这是「轻量编辑器」的底线。 */
static BOOL StartProcess(const WCHAR *wszCmd)
{
  SECURITY_ATTRIBUTES sa;
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  HANDLE hOutWr = NULL, hErrWr = NULL, hInRd = NULL;
  StreamCtx *ctxOut = NULL, *ctxErr = NULL;
  WCHAR wszCmdLine[MAX_PATH * 4];
  BOOL ok = FALSE;
  int cbUtf8;

  g_hwndStatus = g_hwndMain ? GetDlgItem(g_hwndMain,IDC_STATUSBAR) : NULL;

  ResetSession();
  g_bCancel = FALSE;
  g_bOutputMode = TRUE;
  g_bRunningShown = FALSE;
  g_dwExitCode = 0;
  g_dwStartTick = GetTickCount();

  /* 输出借用预览窗格：没开就先打开，然后整页重建 */
  if (!bShowPreview || !IsWindow(hwndPreview))
    PreviewToggle(TRUE);
  bPreviewDocLoaded = FALSE;

  /* 回显命令行，让用户看到到底跑了什么（变量展开后的样子） */
  cbUtf8 = WideCharToMultiByte(CP_UTF8,0,wszCmd,-1,NULL,0,NULL,NULL);
  if (cbUtf8 > 1) {
    char *uCmd = (char*)malloc((size_t)cbUtf8);
    if (uCmd) {
      WideCharToMultiByte(CP_UTF8,0,wszCmd,-1,uCmd,cbUtf8,NULL,NULL);
      DeltaNewLine();
      BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,"<span class=\"run-cmd\">");
      BufAppend(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,uCmd,cbUtf8 - 1);
      BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,"</span>");
      BufAppendStr(&g_pLog,&g_cbLog,&g_cbLogCap,"> ");
      BufAppend(&g_pLog,&g_cbLog,&g_cbLogCap,uCmd,cbUtf8 - 1);
      BufAppendStr(&g_pLog,&g_cbLog,&g_cbLogCap,"\r\n");
      free(uCmd);
    }
  }
  g_bDirty = TRUE;
  RunCmd_Flush();

  if (g_hProc) {
    CloseHandle(g_hProc);
    g_hProc = NULL;
  }
  if (g_hOutRd) { CloseHandle(g_hOutRd); g_hOutRd = NULL; }
  if (g_hErrRd) { CloseHandle(g_hErrRd); g_hErrRd = NULL; }
  if (g_hInWr)  { CloseHandle(g_hInWr);  g_hInWr  = NULL; }

  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;

  /* 子进程的 stdin。父进程留写端（不继承），子进程拿可继承的读端，
     否则 set /p / pause 读到的永远是 EOF。*/
  if (!CreatePipe(&hInRd,&g_hInWr,&sa,0))
    goto fail;
  SetHandleInformation(g_hInWr,HANDLE_FLAG_INHERIT,0);

  if (!CreatePipe(&g_hOutRd,&hOutWr,&sa,0))
    goto fail;
  SetHandleInformation(g_hOutRd,HANDLE_FLAG_INHERIT,0);

  if (!CreatePipe(&g_hErrRd,&hErrWr,&sa,0))
    goto fail;
  SetHandleInformation(g_hErrRd,HANDLE_FLAG_INHERIT,0);

  ctxOut = (StreamCtx*)calloc(1,sizeof(StreamCtx));
  ctxErr = (StreamCtx*)calloc(1,sizeof(StreamCtx));
  if (!ctxOut || !ctxErr)
    goto fail;

  memset(&si,0,sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput  = hInRd;
  si.hStdOutput = hOutWr;
  si.hStdError  = hErrWr;

  memset(&pi,0,sizeof(pi));
  lstrcpynW(wszCmdLine,wszCmd,COUNTOF(wszCmdLine));
  if (!CreateProcessW(NULL,wszCmdLine,NULL,NULL,TRUE,CREATE_NO_WINDOW,
                      NULL,NULL,&si,&pi))
    goto fail;

  /* 父进程必须关掉写端，否则读端永远等不到 EOF，界面会一直「运行中」*/
  CloseHandle(hOutWr); hOutWr = NULL;
  CloseHandle(hErrWr); hErrWr = NULL;
  CloseHandle(hInRd);  hInRd  = NULL;   /* 子进程已经拿到，父端这份多余 */

  g_hProc = pi.hProcess;
  CloseHandle(pi.hThread);

  ctxOut->hRead = g_hOutRd;
  ctxOut->bErr  = FALSE;
  ctxErr->hRead = g_hErrRd;
  ctxErr->bErr  = TRUE;

  {
    HANDLE hT;
    hT = CreateThread(NULL,0,ReadThreadProc,ctxOut,0,NULL);
    if (hT) { CloseHandle(hT); ctxOut = NULL; } else goto fail;
    hT = CreateThread(NULL,0,ReadThreadProc,ctxErr,0,NULL);
    if (hT) { CloseHandle(hT); ctxErr = NULL; } else goto fail;
    hT = CreateThread(NULL,0,WaitThreadProc,g_hProc,0,NULL);
    if (hT) CloseHandle(hT); else goto fail;
  }

  g_bRunning = TRUE;
  ok = TRUE;

  if (g_hwndStatus)
    StatusSetText(g_hwndStatus,STATUS_HELP,L"运行中…");
  RunCmd_LogState("started");
  EnsureFlushTimer();

  /* 仅测试：给子进程注入一行 stdin，验证「页内输入框 → JS → 桥 → 管道」*/
  if (g_hwndMain && getenv("N2MD_RUNCMD_INPUT"))
    SetTimer(g_hwndMain,ID_RUNCMDINTIMER,1200,NULL);

fail:
  if (!ok) {
    if (hOutWr) CloseHandle(hOutWr);
    if (hErrWr) CloseHandle(hErrWr);
    if (hInRd) CloseHandle(hInRd);
    if (ctxOut) {
      if (ctxOut->hRead) { CloseHandle(ctxOut->hRead); g_hOutRd = NULL; }
      free(ctxOut);
    }
    if (ctxErr) {
      if (ctxErr->hRead) { CloseHandle(ctxErr->hRead); g_hErrRd = NULL; }
      free(ctxErr);
    }
    g_bRunning = FALSE;
    if (g_hOutRd) { CloseHandle(g_hOutRd); g_hOutRd = NULL; }
    if (g_hErrRd) { CloseHandle(g_hErrRd); g_hErrRd = NULL; }
    if (g_hInWr)  { CloseHandle(g_hInWr);  g_hInWr  = NULL; }
    g_bDirty = TRUE;
    DeltaNewLine();
    BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,
      "<span class=\"run-err\">\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5"
      "</span>");
    BufAppendStr(&g_pLog,&g_cbLog,&g_cbLogCap,"\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1"
      "\xe8\xb4\xa5\r\n");
    RunCmd_Flush();
    EnsureFlushTimer();
    if (g_hwndStatus)
      StatusSetText(g_hwndStatus,STATUS_HELP,L"启动失败");
  }
  return ok;
}


BOOL RunCmd_Run(HWND hwndMain, BOOL bSaveFirst)
{
  WCHAR wszCmd[MAX_PATH * 4];

  g_hwndMain = hwndMain;
  g_hwndStatus = hwndMain ? GetDlgItem(hwndMain,IDC_STATUSBAR) : NULL;

  if (!g_wszFile[0]) {
    if (g_hwndStatus)
      StatusSetText(g_hwndStatus,STATUS_HELP,L"先保存文件再运行");
    return FALSE;
  }

  /* 运行中再触发：先停掉旧的（单实例语义）*/
  if (g_bRunning)
    RunCmd_Stop();

  if (bSaveFirst && bSaveBeforeRunningTools) {
    if (!FileSave(FALSE,TRUE,FALSE,FALSE))
      return FALSE;
  }

  if (!RunCmd_BuildCommandForTest(g_wszFile,wszCmd,COUNTOF(wszCmd))) {
    g_dwExitCode = 1;
    g_bCancel = FALSE;
    g_bRunning = FALSE;
    g_bOutputMode = TRUE;
    g_bRunningShown = FALSE;
    if (!bShowPreview || !IsWindow(hwndPreview))
      PreviewToggle(TRUE);
    bPreviewDocLoaded = FALSE;
    DeltaNewLine();
    BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,
      "<span class=\"run-err\">\xe6\x97\xa0\xe6\xb3\x95\xe6\x8e\xa8\xe6\x96\xad"
      "\xe8\xaf\xa5\xe6\x96\x87\xe4\xbb\xb6\xe7\x9a\x84\xe8\xbf\x90\xe8\xa1\x8c"
      "\xe6\x96\xb9\xe5\xbc\x8f\xe3\x80\x82</span>");
    BufAppendStr(&g_pLog,&g_cbLog,&g_cbLogCap,
      "\xce\xde\xe6\xb3\x95\xe6\x8e\xa8\xe6\x96\xad\xe8\xaf\xa5\xe6\x96\x87 "
      "\xe4\xbb\xb6\xe7\x9a\x84\xe8\xbf\x90\xe8\xa1\x8c\xe6\x96\xb9\xe5\xbc\x8f"
      "\xe3\x80\x82\r\n");
    g_bDirty = TRUE;
    RunCmd_Flush();
    if (g_hwndStatus)
      StatusSetText(g_hwndStatus,STATUS_HELP,L"无法推断运行方式");
    return FALSE;
  }

  return StartProcess(wszCmd);
}

BOOL RunCmd_Stop(void)
{
  if (!g_bRunning)
    return FALSE;

  g_bCancel = TRUE;
  if (g_hProc)
    TerminateProcess(g_hProc,1);

  /* 关掉写端：万一子进程正阻塞在读 stdin（pause / set /p），
     也能立即收到 EOF，不会卡着不退。*/
  if (g_hInWr) { CloseHandle(g_hInWr); g_hInWr = NULL; }

  if (g_hwndStatus)
    StatusSetText(g_hwndStatus,STATUS_HELP,L"已停止");
  return TRUE;
}

/* pause / choice 这类命令不读 stdin，而是读「控制台输入缓冲区」。
   CREATE_NO_WINDOW 下子进程仍有一个隐藏控制台（只是无窗口），所以只要
   父进程临时 Attach 上去，用 WriteConsoleInputW 敲键，才算真把输入送到。
   注意：AttachConsole 会把本进程的标准句柄指向那个控制台，用完必须
   FreeConsole，否则后续 CreateProcess 会拿到错的继承句柄。*/
static void SendConsoleKeys(const WCHAR *wszText)
{
  DWORD pid;
  HANDLE hCon;
  INPUT_RECORD *ir;
  int cch, i, n = 0;
  DWORD wr = 0;
  BOOL ok = FALSE;

  if (!g_hProc)
    return;
  pid = GetProcessId(g_hProc);
  if (!pid)
    return;
  if (!AttachConsole(pid))
    return;

  hCon = CreateFileW(L"CONIN$",GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
  if (hCon != INVALID_HANDLE_VALUE) {
    cch = wszText ? lstrlenW(wszText) : 0;
    ir = (INPUT_RECORD*)calloc((size_t)(cch * 2 + 2),sizeof(INPUT_RECORD));
    if (ir) {
      for (i = 0; i < cch; i++) {
        SHORT vk = VkKeyScanW(wszText[i]);
        WORD  wVk = (vk == -1) ? 0 : (WORD)LOBYTE(vk);

        ZeroMemory(&ir[n],sizeof(INPUT_RECORD));
        ir[n].EventType = KEY_EVENT;
        ir[n].Event.KeyEvent.bKeyDown = TRUE;
        ir[n].Event.KeyEvent.wVirtualKeyCode = wVk;
        ir[n].Event.KeyEvent.uChar.UnicodeChar = wszText[i];
        n++;
        ZeroMemory(&ir[n],sizeof(INPUT_RECORD));
        ir[n].EventType = KEY_EVENT;
        ir[n].Event.KeyEvent.bKeyDown = FALSE;
        ir[n].Event.KeyEvent.wVirtualKeyCode = wVk;
        ir[n].Event.KeyEvent.uChar.UnicodeChar = wszText[i];
        n++;
      }
      /* 最后一个回车：pause 就等这个 */
      ZeroMemory(&ir[n],sizeof(INPUT_RECORD));
      ir[n].EventType = KEY_EVENT;
      ir[n].Event.KeyEvent.bKeyDown = TRUE;
      ir[n].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
      ir[n].Event.KeyEvent.uChar.UnicodeChar = L'\r';
      n++;
      ZeroMemory(&ir[n],sizeof(INPUT_RECORD));
      ir[n].EventType = KEY_EVENT;
      ir[n].Event.KeyEvent.bKeyDown = FALSE;
      ir[n].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
      ir[n].Event.KeyEvent.uChar.UnicodeChar = L'\r';
      n++;

      ok = WriteConsoleInputW(hCon,ir,(DWORD)n,&wr);
      free(ir);
    }
    CloseHandle(hCon);
  }
  FreeConsole();
  (void)ok;
}

/* 把用户敲的一行写进子进程的 stdin。
   编码跟输出编码步调一致：默认 ACP（cmd 读的就是 ANSI 代码页），
   runcmd.ini 里指定 utf8 就用 UTF-8（node / python 这类子进程需要）。*/
BOOL RunCmd_SendInput(const WCHAR *wszText)
{
  UINT cp;
  char *p;
  char *u = NULL;
  int cb, cbU;
  DWORD wr = 0;

  if (!wszText || !g_hInWr)
    return FALSE;

  cp = (g_iLogEnc == 1) ? CP_UTF8 : CP_ACP;
  cb = WideCharToMultiByte(cp,0,wszText,-1,NULL,0,NULL,NULL);
  if (cb <= 0)
    return FALSE;

  p = (char*)malloc((size_t)cb + 2);
  if (!p)
    return FALSE;
  WideCharToMultiByte(cp,0,wszText,-1,p,cb,NULL,NULL);
  /* 末尾补 CRLF：cmd 的 set /p 和 pause 都等一个回车 */
  p[cb - 1] = '\r';
  p[cb]     = '\n';
  WriteFile(g_hInWr,p,(DWORD)(cb + 1),&wr,NULL);
  free(p);

  /* 再往控制台敲一遍：pause / choice 读的是控制台缓冲区，不读 stdin。
     两条路都送，两边都能醒：set /p 拿管道，pause 拿按键。*/
  SendConsoleKeys(wszText);

  /* 回显，否则用户不知道自己发没发出去 */
  cbU = WideCharToMultiByte(CP_UTF8,0,wszText,-1,NULL,0,NULL,NULL);
  if (cbU > 1) {
    u = (char*)malloc((size_t)cbU);
    if (u) {
      WideCharToMultiByte(CP_UTF8,0,wszText,-1,u,cbU,NULL,NULL);
      DeltaNewLine();
      BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,"<span class=\"run-in\">\xc2\xbb ");
      HtmlAppendEscaped(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,u,cbU - 1);
      BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,"</span>");
      BufAppendStr(&g_pLog,&g_cbLog,&g_cbLogCap,"> ");
      BufAppend(&g_pLog,&g_cbLog,&g_cbLogCap,u,cbU - 1);
      BufAppendStr(&g_pLog,&g_cbLog,&g_cbLogCap,"\r\n");
      free(u);
      g_bDirty = TRUE;
      RunCmd_Flush();
    }
  }

  return TRUE;
}

/* 关掉 stdin 写端（子进程收到 EOF）。
   子进程退出、用户点「结束输入」、以及停止运行时都要调用：
   否则「读到 EOF 才收尾」的程序会一直挂着，只能强杀。*/
void RunCmd_CloseStdin(void)
{
  if (g_hInWr) { CloseHandle(g_hInWr); g_hInWr = NULL; }
}

void RunCmd_OnOutputMessage(WPARAM wParam, LPARAM lParam)
{
  char *psz = (char*)lParam;
  BOOL bErr = (wParam != 0);

  if (!psz)
    return;
  if (!g_bOutputMode) {
    free(psz);
    return;
  }

  AcceptLine(psz,(int)strlen(psz),bErr);
  free(psz);

  if (g_hwndMain && g_bDirty)
    SetTimer(g_hwndMain,ID_RUNCMDTIMER,RUNCMD_FLUSH_MS,NULL);
}

void RunCmd_OnDoneMessage(WPARAM wParam)
{
  char tchLine[256];
  const char *cls, *label;
  /* dSec 已改为整数毫秒计算，见下方 wsprintfA */

  g_dwExitCode = (DWORD)wParam;
  g_bRunning = FALSE;
  RunCmd_CloseStdin();


  /* 先让读线程把尾巴投递完，再收尾 */
  {
    MSG msg;
    int spin;
    for (spin = 0; spin < 20; spin++) {
      BOOL bGot = FALSE;
      while (PeekMessage(&msg,NULL,0,0,PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        bGot = TRUE;
      }
      if (!bGot && spin > 0)
        break;
      Sleep(5);
    }
  }

  cls   = g_bCancel ? "run-stop" : (g_dwExitCode == 0 ? "run-ok" : "run-err");
  label = g_bCancel ? "\xe2\x8f\xb9 \xe5\xb7\xb2\xe5\x81\x9c\xe6\xad\xa2"
                    : (g_dwExitCode == 0 ? "\xe2\x9c\x93 \xe5\xae\x8c\xe6\x88\x90"
                                         : "\xe2\x9c\x97 \xe5\xa4\xb1\xe8\xb4\xa5");

  /* 同样不能用 %.2f：wsprintf 不支持浮点，会打出 「· fs」这种残缺文本 */
  {
    DWORD dwMs = g_dwStartTick ? (GetTickCount() - g_dwStartTick) : 0;
    /* 两份：一份进 DOM（HTML，行首 <br>），一份进纯文本日志。
       以前两者共用 tchLine，结果「存日志」存出来的 .txt 里混着 <span> 标签。*/
    DeltaNewLine();
    wsprintfA(tchLine,"<span class=\"%s\">%s (exit=%lu) \xc2\xb7 %lu.%02lus</span>",
              cls,label,(unsigned long)g_dwExitCode,
              (unsigned long)(dwMs / 1000),(unsigned long)((dwMs % 1000) / 10));
    BufAppendStr(&g_pDelta,&g_cbDelta,&g_cbDeltaCap,tchLine);
    wsprintfA(tchLine,"%s (exit=%lu) \xc2\xb7 %lu.%02lus\r\n",
              label,(unsigned long)g_dwExitCode,
              (unsigned long)(dwMs / 1000),(unsigned long)((dwMs % 1000) / 10));
    BufAppendStr(&g_pLog,&g_cbLog,&g_cbLogCap,tchLine);
  }
  g_bDirty = TRUE;

  RunCmd_Flush();
  EnsureFlushTimer();   /* flush 没落地就让它继续重试 */
  {
    char *h = NULL;
    int len = 0, cap = 0;
    AppendHeader(&h,&len,&cap);
    if (h) {
      SetElementHtml(L"runhead",h,len);
      free(h);
    }
  }

  if (g_hwndStatus) {
    WCHAR wszStatus[128];
    if (g_bCancel)
      lstrcpyW(wszStatus,L"已停止");
    else if (g_dwExitCode == 0)
      lstrcpyW(wszStatus,L"完成 (exit=0)");
    else
      wsprintfW(wszStatus,L"失败 (exit=%lu)",(unsigned long)g_dwExitCode);
    StatusSetText(g_hwndStatus,STATUS_HELP,wszStatus);
  }

  g_dwStartTick = 0;
  RunCmd_LogState("done");
  /* DOM 探针延后一点：SetHtml 走 IPersistStreamInit::Load 是异步解析，
     立刻读会拿到空文档，误判成「没渲染」。 */
  if (getenv("N2MD_RUNCMD_LOG") && g_hwndMain) {
    KillTimer(g_hwndMain,ID_RUNCMDDOMTIMER);
    RunCmd_OnDomTimer(g_hwndMain);   /* 第一次立即试，失败则自动续期 */
  }
}

/* 轮询而不是一次性读：Preview_SetHtml 走 IPersistStreamInit::Load 是异步解析，
   固定延时读会撞上「文档正在被替换」的窗口，偶发拿不到元素。
   每 300ms 试一次，读到 #runlog 就收工，最多 20 次（约 6 秒）。 */
void RunCmd_OnDomTimer(HWND hwnd)
{
  static int s_iTries = 0;

  if (s_iTries == 0)
    SetTimer(hwnd,ID_RUNCMDDOMTIMER,300,NULL);
  s_iTries++;

  if (RunCmd_LogDom("dom") || s_iTries >= 20) {
    KillTimer(hwnd,ID_RUNCMDDOMTIMER);
    s_iTries = 0;
  }
}

/* 仅测试用：设了 N2MD_RUNCMD_INPUT 时，运行开始后 1.2 秒把该文本「敲」进
   页面底部的输入框。
   为什么不直接调 RunCmd_SendInput：那样只验证了写管道，验不到「页内 JS →
   window.external.MDInput → IDispatch 桥 → 写 stdin」这条完整链路。
   这里先给 #runin 赋值，再调与按钮相同的 __mdSendInput()。*/
void RunCmd_OnInputTimer(HWND hwnd)
{
  const char *pIn = getenv("N2MD_RUNCMD_INPUT");
  WCHAR wszJs[2048];
  WCHAR wszText[512];
  int i, n = 0;

  KillTimer(hwnd,ID_RUNCMDINTIMER);
  if (!pIn || !pIn[0] || !g_bRunning)
    return;

  /* 环境变量在 Windows 里是 UTF-16，getenv 会转成 ANSI 给你。
     以前把 getenv 的字节当 UTF-8 喂给 MultiByteToWideChar，
     中文输入直接变问号。这里直接取宽字符。*/
  if (GetEnvironmentVariableW(L"N2MD_RUNCMD_INPUT",wszText,COUNTOF(wszText)) <= 0)
    return;
  /* 环境变量没法表达「空字符串」（设成空等于删掉变量），
     所以留个记号：{{EMPTY}} 表示就敲一个回车。*/
  if (!lstrcmpW(wszText,L"{{EMPTY}}"))
    wszText[0] = L'\0';
  /* {{EOF}} 表示不敲字，直接关写端（验证「读到 EOF 才收尾」的程序）*/
  if (!lstrcmpW(wszText,L"{{EOF}}"))
    return (void)ExecJs(L"window.__mdEndInput();");

  lstrcpyW(wszJs,L"document.getElementById('runin').value='");
  n = lstrlenW(wszJs);
  for (i = 0; wszText[i] && n < (int)COUNTOF(wszJs) - 64; i++) {
    WCHAR c = wszText[i];
    if (c == L'\'' || c == L'\\') {
      wszJs[n++] = L'\\';
      wszJs[n++] = c;
    }
    else if (c == L'\r' || c == L'\n') {
      continue;
    }
    else
      wszJs[n++] = c;
  }
  wszJs[n] = L'\0';
  lstrcatW(wszJs,L"';window.__mdSendInput();");

  ExecJs(wszJs);
}

BOOL RunCmd_CloseOutput(void)
{
  if (!g_bOutputMode)
    return FALSE;
  if (g_bRunning)
    RunCmd_Stop();

  g_bOutputMode = FALSE;
  g_bRunningShown = FALSE;
  if (g_hwndMain)
    KillTimer(g_hwndMain,ID_RUNCMDTIMER);

  /* 让预览下次整页重载 Markdown */
  bPreviewDocLoaded = FALSE;
  if (bShowPreview && IsWindow(hwndPreview))
    PreviewUpdate();
  return TRUE;
}

/* ---------------------------------------------------------------------------
   E2E 探针：仅在设了 N2MD_RUNCMD_LOG 时向该路径追加一行状态。

   为什么需要它：GUI 测试没法直接读进程内的 g_bRunning / g_dwExitCode，
   而读 MSHTML 的 DOM 在 RDP/无桌面环境下也不可靠。让被验程序自己报数，
   测试只断言文本，避免「看起来跑通了」的假阳性。
--------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
   E2E 探针：把输出文档的 DOM 事实写进日志，并在设了 N2MD_RUNCMD_DOM 时
   把 #runlog 的 innerHTML 原样导出。

   为什么需要它：只看「抓到多少行」无法区分「输出存进了缓冲区」和
   「输出真的渲染进了预览窗格」。GUI 测试读不到进程内的 DOM，
   所以在被验程序里自报，测试只做文本断言。
--------------------------------------------------------------------------- */


static void RunCmd_LogState(const char *tag)
{
  const char *path = getenv("N2MD_RUNCMD_LOG");
  FILE *fp;

  if (!path || !path[0])
    return;
  fp = fopen(path,"a");
  if (!fp)
    return;
  fprintf(fp,"%s running=%d exit=%lu lines=%d log=%d trunc=%d active=%d flen=%d\n",
          tag,(int)g_bRunning,(unsigned long)g_dwExitCode,g_iLines,g_cbLog,
          (int)g_bTruncated,(int)g_bOutputMode,(int)wcslen(g_wszFile));
  fclose(fp);
}


/* 把纯文本日志写成 UTF-8 BOM 的 .txt。
   单独拆出来是为了能单测：弹文件对话框那条路没法自动化，写盘这段可以。 */
static BOOL WriteLogToFile(const WCHAR *wszPath)
{
  HANDLE hFile;
  DWORD dwWritten;

  if (!wszPath || !*wszPath || !g_pLog)
    return FALSE;

  hFile = CreateFileW(wszPath,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL,NULL);
  if (hFile == INVALID_HANDLE_VALUE)
    return FALSE;

  /* UTF-8 BOM：记事本等工具才能正确识别中文 */
  WriteFile(hFile,"\xEF\xBB\xBF",3,&dwWritten,NULL);
  if (g_cbLog > 0)
    WriteFile(hFile,g_pLog,(DWORD)g_cbLog,&dwWritten,NULL);
  CloseHandle(hFile);
  return TRUE;
}

BOOL RunCmd_SaveLog(HWND hwndMain)
{
  WCHAR wszPath[MAX_PATH], wszFilter[256];
  OPENFILENAMEW ofn;
  WCHAR *pDot;

  if (!g_pLog || g_cbLog <= 0) {
    StatusSetText(GetDlgItem(hwndMain,IDC_STATUSBAR),STATUS_HELP,L"没有可保存的输出");
    return FALSE;
  }

  lstrcpynW(wszPath,g_wszFile,MAX_PATH);
  pDot = PathFindExtensionW(wszPath);
  if (pDot && *pDot)
    *pDot = L'\0';
  lstrcatW(wszPath,L".log.txt");

  /* 双 NUL 结尾的过滤器 */
  lstrcpyW(wszFilter,L"Text Files (*.txt)");
  lstrcpyW(wszFilter + lstrlenW(wszFilter) + 1,L"*.txt");
  lstrcpyW(wszFilter + lstrlenW(wszFilter) + 1,L"All Files (*.*)");
  lstrcpyW(wszFilter + lstrlenW(wszFilter) + 1,L"*.*");
  lstrcpyW(wszFilter + lstrlenW(wszFilter) + 1,L"");

  ZeroMemory(&ofn,sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner   = hwndMain;
  ofn.lpstrFilter = wszFilter;
  ofn.lpstrFile   = wszPath;
  ofn.nMaxFile    = COUNTOF(wszPath);
  ofn.lpstrDefExt = L"txt";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
              OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

  if (!GetSaveFileNameW(&ofn))
    return FALSE;

  if (!WriteLogToFile(wszPath)) {
    StatusSetText(GetDlgItem(hwndMain,IDC_STATUSBAR),STATUS_HELP,L"保存失败");
    return FALSE;
  }

  StatusSetText(GetDlgItem(hwndMain,IDC_STATUSBAR),STATUS_HELP,L"日志已保存");
  return TRUE;
}

/* 测试钩子：跳过对话框直接写盘 */
BOOL RunCmd_WriteLogForTest(const WCHAR *wszPath)
{
  return WriteLogToFile(wszPath);
}

/* 供测试读取纯文本日志长度（不导出到 .h，测试用正则在源里找）*/
int RunCmd_GetLogLength(void)
{
  return g_cbLog;
}
