/*===========================================================================

  notepad2md — 运行当前文件（进程执行 + 输出捕获）

  设计要点：
    · 不建任何新窗口/面板，输出灌进现有 MSHTML 预览窗格
    · 进程与管道逻辑在本模块；HTML 渲染交给 Preview
    · 只支持 4 个变量：%FILE% %DIR% %NAME% %EXT%
    · 单实例：运行中再触发先 TerminateProcess 再起新的

  ===========================================================================*/

#ifndef MD_RUNCMD_H
#define MD_RUNCMD_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 读线程 / 等待线程 → 主窗口的消息 */
#define WM_RUNCMD_OUTPUT  (WM_USER+2)
#define WM_RUNCMD_DONE    (WM_USER+3)

/* 运行前准备/清理 */
BOOL RunCmd_Init(void);
void RunCmd_Shutdown(void);

/* 让 RunCmd 知道当前文件（决定命令映射与工作目录）*/
void RunCmd_SetCurrentFile(const WCHAR *wszPath);

/* 按当前文件推断并执行；返回 TRUE 表示已启动 */
BOOL RunCmd_Run(HWND hwndMain, BOOL bSaveFirst);

/* 停止正在运行的进程；没在跑返回 FALSE */
BOOL RunCmd_Stop(void);

/* 把一行文本写进正在运行的子进程的 stdin（批处理的 set /p、pause 靠它）。
   没在运行、或写端已关则返回 FALSE。*/
BOOL RunCmd_SendInput(const WCHAR *wszText);

/* 关掉子进程 stdin 的写端（发 EOF）。「读到 EOF 才收尾」的程序靠它优雅结束 */
void RunCmd_CloseStdin(void);

/* 是否有进程在跑 */
BOOL RunCmd_IsRunning(void);

/* 预览窗格当前是否被运行输出占用（PreviewUpdate 靠它让路）*/
BOOL RunCmd_OutputActive(void);

/* 关掉输出、恢复 Markdown 预览 */
BOOL RunCmd_CloseOutput(void);

/* 仅测试：N2MD_RUNCMD_INPUT 存在时，延迟把文本从页面输入框送进 stdin */
void RunCmd_OnInputTimer(HWND hwnd);

/* 主线程：把积累的增量刷进 DOM（由 WM_TIMER 驱动）*/
void RunCmd_Flush(void);

/* 主线程：读线程投递来的一行 */
void RunCmd_OnOutputMessage(WPARAM wParam, LPARAM lParam);

/* 主线程：DOM 探针定时器（仅测试路径）*/
void RunCmd_OnDomTimer(HWND hwnd);

/* 主线程：进程结束 */
void RunCmd_OnDoneMessage(WPARAM wParam);

/* 「存日志」：另存为 UTF-8 .txt */
BOOL RunCmd_SaveLog(HWND hwndMain);

/* 以下两个为可单测的纯逻辑（表驱动测试直接调）*/
/* 测试钩子：把管道原始字节按内置规则解码成 UTF-8，返回长度 */
int  RunCmd_DecodeForTest(const char *pRaw, int cbRaw, char *pOut, int cbOut);
void RunCmd_AppendForTest(const char *pszLine, BOOL bErr);
BOOL RunCmd_WriteLogForTest(const WCHAR *wszPath);
void RunCmd_ExpandVarsForTest(const WCHAR *wszFile, const WCHAR *wszIn,
                              WCHAR *wszOut, int cchOut);
BOOL RunCmd_BuildCommandForTest(const WCHAR *wszFile,
                                WCHAR *wszOut, int cchOut);

#ifdef __cplusplus
}
#endif

#endif  /* MD_RUNCMD_H */
