/* ==========================================================================
   RunCmd 纯逻辑单测（命令推断 / 变量展开）

   链接真实的 RunCmd.o，只给 Preview / Notepad2 的外部符号打桩，
   所以测的是生产代码本身。
   ========================================================================== */

#include <windows.h>
#include <ole2.h>
#include <exdisp.h>
#include <mshtml.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "scintilla.h"
#include "RunCmd.h"

/* ---- 桩：RunCmd.c 依赖的外部符号 ---- */
HWND  hwndMain = NULL;
HWND  hwndPreview = NULL;
BOOL  bShowPreview = FALSE;
BOOL  bPreviewDocLoaded = FALSE;
BOOL  bSaveBeforeRunningTools = FALSE;

BOOL Preview_SetHtml(HWND h, const char *html) { (void)h; (void)html; return TRUE; }
IWebBrowser2* Preview_GetBrowser(void) { return NULL; }
void PreviewToggle(BOOL b) { (void)b; }
void PreviewUpdate(void) {}
BOOL StatusSetText(HWND h, UINT p, LPCWSTR t) { (void)h; (void)p; (void)t; return TRUE; }
BOOL FileSave(BOOL a, BOOL b, BOOL c, BOOL d) { (void)a; (void)b; (void)c; (void)d; return TRUE; }

/* ---- 迷你框架 ---- */
static int g_pass = 0, g_fail = 0;
static const char *g_case = "";

static void checkW(const wchar_t *got, const wchar_t *want, const char *what)
{
  if (wcscmp(got, want) != 0) {
    g_fail++;
    printf("  FAIL [%s] %s\n        got  = %ls\n        want = %ls\n",
           g_case, what, got, want);
  } else
    g_pass++;
}

static void checkTrue(BOOL cond, const char *what)
{
  if (!cond) {
    g_fail++;
    printf("  FAIL [%s] %s\n", g_case, what);
  } else
    g_pass++;
}

/* ---- 用例 ---- */

static void TestExpandVars(void)
{
  wchar_t out[MAX_PATH * 4];

  g_case = "ExpandVars";

  RunCmd_ExpandVarsForTest(L"D:\\tmp\\hello.py", L"python \"%FILE%\"", out, 1024);
  checkW(out, L"python \"D:\\tmp\\hello.py\"", "%FILE% 带引号");

  RunCmd_ExpandVarsForTest(L"D:\\tmp\\hello.py", L"%DIR%", out, 1024);
  checkW(out, L"D:\\tmp", "%DIR%");

  RunCmd_ExpandVarsForTest(L"D:\\tmp\\hello.py", L"%NAME%", out, 1024);
  checkW(out, L"hello", "%NAME% 去扩展名");

  RunCmd_ExpandVarsForTest(L"D:\\tmp\\hello.py", L"%EXT%", out, 1024);
  checkW(out, L"py", "%EXT% 无点");

  RunCmd_ExpandVarsForTest(L"D:\\tmp\\a.js", L"%file%|%dir%|%name%|%ext%", out, 1024);
  checkW(out, L"D:\\tmp\\a.js|D:\\tmp|a|js", "变量名大小写不敏感");

  /* 砍掉的变量必须原样保留，不能吞字符 */
  RunCmd_ExpandVarsForTest(L"D:\\tmp\\a.js", L"%LINE% %SEL%", out, 1024);
  checkW(out, L"%LINE% %SEL%", "未知变量原样保留");

  RunCmd_ExpandVarsForTest(L"D:\\tmp\\Makefile", L"%NAME%.%EXT%", out, 1024);
  checkW(out, L"Makefile.", "无扩展名文件");

  RunCmd_ExpandVarsForTest(L"C:\\a b\\x.py", L"%FILE% %FILE%", out, 1024);
  checkW(out, L"C:\\a b\\x.py C:\\a b\\x.py", "空格路径原样展开");

  RunCmd_ExpandVarsForTest(L"D:\\tmp\\a.py", L"", out, 1024);
  checkW(out, L"", "空模板");

  /* 中文路径不能被转码破坏 */
  RunCmd_ExpandVarsForTest(L"D:\\\u4e2d\u6587\\\u4f60\u597d.py", L"%DIR%|%NAME%", out, 1024);
  checkW(out, L"D:\\\u4e2d\u6587|\u4f60\u597d", "中文路径保持完整");
}

static void TestBuildCommand(void)
{
  wchar_t out[MAX_PATH * 4];
  BOOL ok;

  g_case = "BuildCommand";

  ok = RunCmd_BuildCommandForTest(L"D:\\tmp\\hello.py", out, 1024);
  checkTrue(ok, ".py 应有映射");
  checkW(out, L"python \"D:\\tmp\\hello.py\"", ".py -> python");

  ok = RunCmd_BuildCommandForTest(L"D:\\tmp\\a.pyw", out, 1024);
  checkTrue(ok && wcsstr(out, L"python") != NULL, ".pyw -> python");

  ok = RunCmd_BuildCommandForTest(L"D:\\tmp\\a.js", out, 1024);
  checkTrue(ok && wcsstr(out, L"node") != NULL, ".js -> node");

  ok = RunCmd_BuildCommandForTest(L"D:\\tmp\\a.mjs", out, 1024);
  checkTrue(ok && wcsstr(out, L"node") != NULL, ".mjs -> node");

  ok = RunCmd_BuildCommandForTest(L"D:\\tmp\\a.ts", out, 1024);
  checkTrue(ok && wcsstr(out, L"node") != NULL, ".ts -> node");

  ok = RunCmd_BuildCommandForTest(L"D:\\tmp\\a.bat", out, 1024);
  checkTrue(ok && wcsstr(out, L"cmd /c") != NULL, ".bat -> cmd /c");

  ok = RunCmd_BuildCommandForTest(L"D:\\tmp\\a.cmd", out, 1024);
  checkTrue(ok && wcsstr(out, L"cmd /c") != NULL, ".cmd -> cmd /c");

  ok = RunCmd_BuildCommandForTest(L"D:\\tmp\\a.ps1", out, 1024);
  checkTrue(ok && wcsstr(out, L"powershell") != NULL, ".ps1 -> powershell");

  ok = RunCmd_BuildCommandForTest(L"D:\\tmp\\A.PY", out, 1024);
  checkTrue(ok && wcsstr(out, L"python") != NULL, "扩展名大小写不敏感");

  /* 无映射且无项目文件：必须明确返回 FALSE（宿主据此提示，不静默失败）*/
  ok = RunCmd_BuildCommandForTest(L"D:\\tmp\\a.qqq", out, 1024);
  checkTrue(!ok, "未知扩展名应返回 FALSE");
}

static void TestProjectProbe(void)
{
  wchar_t dir[MAX_PATH], file[MAX_PATH], proj[MAX_PATH], out[MAX_PATH * 4];
  BOOL ok;
  HANDLE h;
  DWORD w;

  g_case = "ProjectProbe";

  GetTempPathW(MAX_PATH, dir);
  wcscat(dir, L"n2md_runcmd_test");
  CreateDirectoryW(dir, NULL);

  wcscpy(proj, dir);
  wcscat(proj, L"\\Demo.csproj");
  h = CreateFileW(proj, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (h != INVALID_HANDLE_VALUE) {
    WriteFile(h, "<Project/>", 10, &w, NULL);
    CloseHandle(h);
  }

  wcscpy(file, dir);
  wcscat(file, L"\\Program.cs");

  ok = RunCmd_BuildCommandForTest(file, out, 1024);
  checkTrue(ok, ".csproj 同目录应探测到项目");
  checkTrue(ok && wcsstr(out, L"dotnet run --project") != NULL,
            "应生成 dotnet run --project");

  /* 子目录里的文件应向上找到项目（Python 无映射，靠 csproj 命中）*/
  {
    wchar_t sub[MAX_PATH], subfile[MAX_PATH];
    wcscpy(sub, dir);
    wcscat(sub, L"\\src");
    CreateDirectoryW(sub, NULL);
    wcscpy(subfile, sub);
    wcscat(subfile, L"\\Util.cs");
    ok = RunCmd_BuildCommandForTest(subfile, out, 1024);
    checkTrue(ok && wcsstr(out, L"dotnet run --project") != NULL,
              "子目录向上探测 .csproj");
  }
}

/* 解码层：管道里可能是 UTF-8，也可能是 GBK(ACP)。
   最坑的情况是 GBK 中文的字节恰好是「合法 UTF-8」（如 中文 = d6 d0 ce c4），
   只做严格校验会通过，然后解出希腊/西里尔字母 —— 用户看到的就是乱码。 */
static void TestDecode(void)
{
  /* 「中文输出测试」的 GBK 字节与对应 UTF-8 字节 */
  static const unsigned char gbkCn[] =
    {0xd6,0xd0,0xce,0xc4,0xca,0xe4,0xb3,0xf6,0xb2,0xe2,0xca,0xd4,0};
  static const unsigned char utf8Cn[] =
    {0xe4,0xb8,0xad,0xe6,0x96,0x87,0xe8,0xbe,0x93,0xe5,0x87,0xba,
     0xe6,0xb5,0x8b,0xe8,0xaf,0x95,0};
  char out[512];
  int n;

  g_case = "Decode";

  n = RunCmd_DecodeForTest((const char*)gbkCn, (int)sizeof(gbkCn) - 1,
                           out, sizeof(out));
  checkTrue(n > 0 && strcmp(out, (const char*)utf8Cn) == 0,
            "GBK 中文按 ACP 还原");

  n = RunCmd_DecodeForTest((const char*)utf8Cn, (int)sizeof(utf8Cn) - 1,
                           out, sizeof(out));
  checkTrue(n == (int)sizeof(utf8Cn) - 1 &&
            strcmp(out, (const char*)utf8Cn) == 0,
            "UTF-8 中文不被改写");

  n = RunCmd_DecodeForTest("hello", 5, out, sizeof(out));
  checkTrue(n == 5 && strcmp(out, "hello") == 0, "ASCII 原样");

  n = RunCmd_DecodeForTest("", 0, out, sizeof(out));
  checkTrue(n == 0 && out[0] == 0, "空输入安全");

  /* 单字节 0xE9 不是合法 UTF-8，走 ACP 分支也不能崩 */
  n = RunCmd_DecodeForTest("caf\xe9", 4, out, sizeof(out));
  checkTrue(n > 0, "非法 UTF-8 不崩");

  /* 混排：ASCII 前缀 + GBK 中文 */
  {
    static const unsigned char mixed[] =
      {'o','k',' ',0xd6,0xd0,0xce,0xc4,0};
    n = RunCmd_DecodeForTest((const char*)mixed, (int)sizeof(mixed) - 1,
                             out, sizeof(out));
    checkTrue(n == 3 + 6 && strncmp(out, "ok ", 3) == 0 &&
              strcmp(out + 3, (const char*)"\xe4\xb8\xad\xe6\x96\x87") == 0,
              "混排行正确");
  }
}


/* 存日志：写出来的必须是 UTF-8 BOM + 原文，记事本才认中文 */
static void TestSaveLog(void)
{
  WCHAR wszTmp[MAX_PATH];
  char buf[512];
  DWORD nRead = 0;
  HANDLE hFile;
  BOOL bOk;

  g_case = "SaveLog";

  GetTempPathW(MAX_PATH,wszTmp);
  lstrcatW(wszTmp,L"n2md_savelog_test.txt");
  DeleteFileW(wszTmp);

  /* 先喂两行进日志缓冲（走正常解码路径） */
  RunCmd_AppendForTest("hello",FALSE);
  RunCmd_AppendForTest("\xe4\xb8\xad\xe6\x96\x87",FALSE);

  bOk = RunCmd_WriteLogForTest(wszTmp);
  checkTrue(bOk, "写盘成功");
  if (!bOk)
    return;

  hFile = CreateFileW(wszTmp,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL,NULL);
  checkTrue(hFile != INVALID_HANDLE_VALUE, "能读回");
  if (hFile == INVALID_HANDLE_VALUE)
    return;
  ReadFile(hFile,buf,sizeof(buf) - 1,&nRead,NULL);
  CloseHandle(hFile);
  buf[nRead] = '\0';

  checkTrue(nRead > 3 && (unsigned char)buf[0] == 0xEF &&
            (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF,
            "带 UTF-8 BOM");
  checkTrue(strstr(buf + 3,"hello") != NULL, "内容含 hello");
  checkTrue(strstr(buf + 3,"\xe4\xb8\xad\xe6\x96\x87") != NULL, "内容含中文");

  DeleteFileW(wszTmp);
}

int main(void)
{
  printf("== RunCmd 单元测试 ==\n");
  TestExpandVars();
  TestBuildCommand();
  TestProjectProbe();
  TestDecode();
  TestSaveLog();
  printf("\n通过 %d, 失败 %d\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
