//=============================================================================
//
//  Update.c
//
//  「检查更新」。
//
//  只做三件事：问 GitHub 最新 tag 是多少、比一下版本号、有新版就打开下载页。
//  不自动下载、不替换 exe —— 单 exe 便携程序自替换风险远大于收益。
//
//  网络部分用系统自带的 WinINet（和预览面板同一个库，零新增依赖）。
//  拉数据在后台线程做，拉完 PostMessage 回主线程弹窗，界面不会卡住。
//
//  测试友好：环境变量 NOTEPAD2MD_UPDATE_URL 可覆盖接口地址，
//  tests/run/test_update_e2e.py 用它指向本地假服务器。
//
#include <windows.h>
#include <commctrl.h>
#include <wininet.h>
#include <string.h>

#include "scintilla.h"
#include "scilexer.h"
#include "notepad2.h"
#include "dialogs.h"
#include "helpers.h"
#include "resource.h"
#include "Update.h"
#include "UpdateCheck.h"
#include "version.h"

extern HWND hwndMain;

// 版本检测走发布页重定向：/releases/latest 会 302 到 /releases/tag/<tag>。
// 这条路径不限流。GitHub 的 API 对匿名请求只给 60 次/小时/IP，
// 共享出口 IP 下很容易 403，所以版本号绝不能依赖它。
#define UPDATE_URL_DEFAULT  L"https://github.com/SorenBoot/notepad2md/releases/latest"
#define UPDATE_URL_ENV      L"NOTEPAD2MD_UPDATE_URL"

// 发布说明只能从 API 拿（尽力而为：限流/失败就不显示，不影响版本提示）。
#define UPDATE_NOTES_DEFAULT L"https://api.github.com/repos/SorenBoot/notepad2md/releases/latest"
#define UPDATE_NOTES_ENV     L"NOTEPAD2MD_UPDATE_NOTES_URL"
#define UPDATE_REPO_URL     L"https://github.com/SorenBoot/notepad2md/releases/tag/"
#define UPDATE_UA           L"notepad2md/" VERSION_FILEVERSION_SHORT

// 一次请求最多读这么多，GitHub 的 release 对象通常 10KB 上下，
// 带大量发布说明也远低于此。防止异常响应把内存吃光。
#define UPDATE_MAX_BODY     (256 * 1024)

// 弹窗正文：MsgBox 内部用 1024 字符的定长缓冲 + wvsprintf（不做长度检查），
// 所以说明必须自己截短，留出标题行和版本号的空间。
#define UPDATE_NOTES_CHARS  600

typedef struct {
  int  iStatus;
  WCHAR szTag[64];
  WCHAR szNotes[UPDATE_NOTES_CHARS];
  BOOL bNewer;
} UPDATERESULT;

static volatile LONG lRunning = 0;

//=============================================================================
//
//  Utf8ToWide()
//
static void Utf8ToWide(const char *pszUtf8,WCHAR *pszOut,int cchOut)
{
  int n,cb;
  if (cchOut <= 0) return;
  pszOut[0] = L'\0';
  if (!pszUtf8 || !*pszUtf8) return;

  // 目标缓冲装不下时 MultiByteToWideChar 会直接失败，这里逐步退字节，
  // 宁可尾部截断也别把整段说明丢掉。
  cb = (int)strlen(pszUtf8);
  for (;;) {
    n = MultiByteToWideChar(CP_UTF8,0,pszUtf8,cb,pszOut,cchOut - 1);
    if (n > 0) {
      pszOut[n] = L'\0';
      return;
    }
    if (cb <= 0)
      break;
    cb--;
  }
  pszOut[0] = L'\0';
}

//=============================================================================
//
//  HttpGet()
//
//  返回 1 = 拿到 200 响应体；0 = 失败（无网络 / 非 200 / 读失败）。
//
static int HttpGet(const WCHAR *pszUrl,char *pszBuf,int cbBuf,int *pcbGot,
                   WCHAR *pszFinalUrl,int cchFinal)
{
  HINTERNET hNet,hUrl;
  DWORD dwStatus = 0,dwLen = sizeof(dwStatus);
  int cbTotal = 0,bOk = 0;

  if (pcbGot)
    *pcbGot = 0;
  if (pszFinalUrl && cchFinal > 0)
    pszFinalUrl[0] = L'\0';

  hNet = InternetOpenW(UPDATE_UA,INTERNET_OPEN_TYPE_PRECONFIG,NULL,NULL,0);
  if (!hNet)
    return 0;

  // GitHub 要求带 User-Agent，否则 403；Accept 固定成 v3 JSON 免得到处变。
  hUrl = InternetOpenUrlW(hNet,pszUrl,
    L"Accept: application/vnd.github+json\r\n",
    -1,
    INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI,
    0);
  if (!hUrl) {
    InternetCloseHandle(hNet);
    return 0;
  }

  if (!HttpQueryInfoW(hUrl,HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                      &dwStatus,&dwLen,NULL) || dwStatus != 200)
    goto cleanup;

  // 重定向已被 WinINet 跟随，这里拿到的是最终地址
  if (pszFinalUrl && cchFinal > 0) {
    DWORD cbUrl = (DWORD)(cchFinal * sizeof(WCHAR));
    if (!InternetQueryOptionW(hUrl,INTERNET_OPTION_URL,pszFinalUrl,&cbUrl))
      pszFinalUrl[0] = L'\0';
    else
      pszFinalUrl[cchFinal - 1] = L'\0';
  }

  // 只要最终地址时不必下载正文
  if (!pszBuf || cbBuf <= 0) {
    bOk = 1;
    goto cleanup;
  }

  for (;;) {
    DWORD dwChunk = 0;

    if (!InternetReadFile(hUrl,pszBuf + cbTotal,cbBuf - 1 - cbTotal,&dwChunk))
      goto cleanup;
    if (dwChunk == 0)
      break;
    cbTotal += (int)dwChunk;
    if (cbTotal >= cbBuf - 1)
      break;
  }

  pszBuf[cbTotal] = '\0';
  *pcbGot = cbTotal;
  bOk = 1;

cleanup:
  InternetCloseHandle(hUrl);
  InternetCloseHandle(hNet);
  return bOk;
}

//=============================================================================
//
//  UpdateThreadProc()
//
static DWORD WINAPI UpdateThreadProc(LPVOID lParam)
{
  WCHAR szUrl[1024],szFinalUrl[1024];
  WCHAR szNotesUrl[1024];
  char *pszJson = NULL;
  char szTag[64],szNotes[1400],szFmt[1400],szLocal[32];
  UPDATERESULT *pResult;
  int cbJson = 0;

  pResult = (UPDATERESULT *)lParam;
  pResult->iStatus = UPDATE_FAILED;
  pResult->bNewer = FALSE;
  pResult->szTag[0] = L'\0';
  pResult->szNotes[0] = L'\0';

  if (GetEnvironmentVariableW(UPDATE_URL_ENV,szUrl,COUNTOF(szUrl)) == 0 ||
      !szUrl[0])
    lstrcpynW(szUrl,UPDATE_URL_DEFAULT,COUNTOF(szUrl));

  // 第一步：只要最终地址（不下载正文），从中取 tag。
  if (!HttpGet(szUrl,NULL,0,NULL,szFinalUrl,COUNTOF(szFinalUrl)))
    goto done;

  {
    // 宽字符 URL 转窄串再解析，tag 本身是 ASCII
    char szFinalUrlA[2048];
    WideCharToMultiByte(CP_ACP,0,szFinalUrl,-1,szFinalUrlA,sizeof(szFinalUrlA),NULL,NULL);
    if (!UpdateExtractTagFromUrl(szFinalUrlA,szTag,sizeof(szTag)))
      goto done;
  }

  Utf8ToWide(szTag,pResult->szTag,COUNTOF(pResult->szTag));
  if (!pResult->szTag[0])
    goto done;

  WideCharToMultiByte(CP_ACP,0,VERSION_FILEVERSION_SHORT,-1,szLocal,sizeof(szLocal),NULL,NULL);
  pResult->bNewer = UpdateIsNewer(szLocal,szTag) ? TRUE : FALSE;
  pResult->iStatus = pResult->bNewer ? UPDATE_OK_AVAILABLE : UPDATE_OK_LATEST;

  // 第二步：只有确实有新版本才去要发布说明。
  // API 限流（403）或无网络都只是「没说明」，不影响前面的结论。
  if (!pResult->bNewer)
    goto done;

  if (GetEnvironmentVariableW(UPDATE_NOTES_ENV,szNotesUrl,COUNTOF(szNotesUrl)) == 0 ||
      !szNotesUrl[0])
    lstrcpynW(szNotesUrl,UPDATE_NOTES_DEFAULT,COUNTOF(szNotesUrl));

  pszJson = (char *)LocalAlloc(LPTR,UPDATE_MAX_BODY);
  if (!pszJson)
    goto done;

  if (!HttpGet(szNotesUrl,pszJson,UPDATE_MAX_BODY,&cbJson,NULL,0))
    goto done;

  if (UpdateExtractJsonString(pszJson,cbJson,"body",szNotes,sizeof(szNotes))) {
    if (UpdateFormatNotes(szNotes,szFmt,sizeof(szFmt),10) > 0)
      Utf8ToWide(szFmt,pResult->szNotes,COUNTOF(pResult->szNotes));
  }

done:
  if (pszJson)
    LocalFree(pszJson);
  PostMessage(hwndMain,WM_UPDATECHECK_DONE,pResult->iStatus,(LPARAM)pResult);
  return 0;
}

//=============================================================================
//
//  Update_Check()
//
void Update_Check(HWND hwnd)
{
  HANDLE hThread;
  UPDATERESULT *pResult;

  if (hwnd)
    hwndMain = hwnd;

  // 连点两下不要起两个线程
  if (InterlockedExchange(&lRunning,1) != 0)
    return;

  pResult = (UPDATERESULT *)LocalAlloc(LPTR,sizeof(UPDATERESULT));
  if (!pResult) {
    InterlockedExchange(&lRunning,0);
    return;
  }

  hThread = CreateThread(NULL,0,UpdateThreadProc,pResult,0,NULL);
  if (!hThread) {
    LocalFree(pResult);
    InterlockedExchange(&lRunning,0);
    return;
  }
  CloseHandle(hThread);
}

//=============================================================================
//
//  Update_OnDone()
//
//  主线程：弹窗。有新版给「是/否」，点「是」开浏览器到 Releases 页。
//
void Update_OnDone(WPARAM wParam,LPARAM lParam)
{
  UPDATERESULT *pResult = (UPDATERESULT *)lParam;
  int iStatus = (int)wParam;

  InterlockedExchange(&lRunning,0);

  if (!pResult) {
    MsgBox(MBINFO,IDS_UPDATE_FAILED);
    return;
  }

  pResult->szTag[COUNTOF(pResult->szTag) - 1] = L'\0';
  pResult->szNotes[COUNTOF(pResult->szNotes) - 1] = L'\0';

  if (iStatus == UPDATE_OK_AVAILABLE) {
    WCHAR szNotes[COUNTOF(pResult->szNotes) + 128];

    if (pResult->szNotes[0])
      lstrcpynW(szNotes,pResult->szNotes,COUNTOF(szNotes));
    else if (!GetString(IDS_UPDATE_NO_NOTES,szNotes,COUNTOF(szNotes)))
      szNotes[0] = L'\0';

    if (MsgBox(MBYESNO,IDS_UPDATE_AVAILABLE,
               VERSION_FILEVERSION_SHORT,pResult->szTag,szNotes) == IDYES) {
      WCHAR szUrl[512];
      lstrcpynW(szUrl,UPDATE_REPO_URL,COUNTOF(szUrl));
      // tag 只含 ASCII 字母数字和点，宽窄转换在这是安全的
      {
        WCHAR *pDst = szUrl + lstrlenW(szUrl);
        int i;
        int cchLeft = COUNTOF(szUrl) - lstrlenW(szUrl);
        for (i = 0; pResult->szTag[i] && i < cchLeft - 1; i++)
          pDst[i] = pResult->szTag[i];
        pDst[i] = L'\0';
      }
      ShellExecuteW(NULL,L"open",szUrl,NULL,NULL,SW_SHOWNORMAL);
    }
  }
  else if (iStatus == UPDATE_OK_LATEST) {
    MsgBox(MBINFO,IDS_UPDATE_LATEST,VERSION_FILEVERSION_SHORT);
  }
  else {
    MsgBox(MBINFO,IDS_UPDATE_FAILED);
  }

  LocalFree(pResult);
}
