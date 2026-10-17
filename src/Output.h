/*===========================================================================

  notepad2md — 命令输出面板模块接口

  编辑器底部的小面板：跑一条命令，把 stdout/stderr 实时显示出来。
  输出里出现「文件:行号」时双击该行可跳到编辑器对应位置。

  刻意不为（保持轻量，也避免 Win7 兼容问题）：
    - 不做终端模拟器（不嵌 conhost，不用 ConPTY）
    - 不解析 ANSI 转义序列（纯文本）
    - 不支持交互式输入（stdin 接 NUL，交互程序会自行退出或弹自己的窗口）

  ===========================================================================*/

#ifndef MD_OUTPUT_OUTPUT_H
#define MD_OUTPUT_OUTPUT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 面板内部控件的资源 ID */
#define IDC_OUTPUT_CMD    0xE710
#define IDC_OUTPUT_RUN    0xE711
#define IDC_OUTPUT_STOP   0xE712
#define IDC_OUTPUT_CLEAR  0xE713
#define IDC_OUTPUT_LOG    0xE714

/*
  初始化模块。调用一次，在任何其他 Output_* 之前。
*/
BOOL Output_Init(HINSTANCE hinst);

/*
  在父窗口内创建输出面板。

  hwndParent —— 主框架窗口
  hinst      —— 应用实例
*/
HWND Output_Create(HWND hwndParent, HINSTANCE hinst);

/*
  执行一条命令。异步返回：立即结束，输出通过面板显示。

  wszCmd —— 命令行（宽字符，不含前导空格）；NULL 或空串则什么都不做
*/
void Output_Run(const WCHAR *wszCmd);

/*
  请求中止当前运行的命令。若没有命令在跑则什么都不做。
*/
void Output_Stop(void);

/*
  把一行文本追加到输出框。供内部与外部共用。

  wszText —— 要显示的文字（不含换行）
  bNewLine—— 是否在文字后补回车换行
*/
void Output_AppendText(const WCHAR *wszText, BOOL bNewLine);

/*
  取当前是否正在执行命令（用于主窗口更新按钮状态）。
*/
BOOL Output_IsRunning(void);

/*
  取得某一项输出行的跳转目标。

  idx      —— 输出框中的行索引（0 based）
  wszFile  —— 接收文件名的缓冲
  cchFile  —— 缓冲区宽字符数
  piLine   —— 接收 1 based 行号
  返回 TRUE 表示该行是「文件:行号」格式且参数有效；FALSE 表示不可跳转
*/
BOOL Output_GetGotoTarget(int idx, WCHAR *wszFile, int cchFile, int *piLine);

/*
  取得面板内命令下拉框的句柄（主窗口用它做ini持久化）。
*/
HWND Output_GetComboBox(void);

#ifdef __cplusplus
}
#endif

#endif   /* MD_OUTPUT_OUTPUT_H */
