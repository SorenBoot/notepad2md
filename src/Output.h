/*===========================================================================

  notepad2md — 命令输出面板模块接口

  编辑器底部的小面板：跑一条命令，把 stdout/stderr 实时显示出来。
  输出里出现「文件:行号」时双击该行可跳到编辑器对应位置。

  输出控件是 RichEdit（MSFTEDIT_CLASS），解析 ANSI 颜色转义，
  gcc / git / ls 的颜色能还原出来。

  刻意不为（保持轻量，也避免 Win7 兼容问题）：
    - 不做终端模拟器（不嵌 conhost，不用 ConPTY）：不支持光标定位、
      全屏程序、交互输入（stdin 接 NUL，交互程序会自行退出）
    - 工作目录固定跟随当前文件，不提供手填覆盖

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
#define IDC_OUTPUT_STATUS 0xE715
#define IDC_OUTPUT_DOCKBTN 0xE717   /* 停靠/独立切换按钮 */

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

  wszCmd —— 命令行（宽字符，不含前导空格）；NULL 或空串则什么都不做。
             支持变量替换：
               %FILE%  当前文件全路径
               %DIR%   当前文件所在目录
               %NAME%  文件名（不含扩展名）
               %EXT%   扩展名（不含点，如 c）
               %LINE%  光标所在行（1 based）
               %SEL%   当前选中的文本
               %%      百分号本身

             路径含空格时请自己加引号，例如：
               gcc -c "%FILE%" -o "%DIR%%NAME%.exe"
             模块不做自动加引号 —— %DIR%%NAME%.o 这类跨变量写法
             无法逐变量正确加引号，自动加反而制造更难查的错。
*/
void Output_Run(const WCHAR *wszCmd);

/*
  主窗口在编辑器状态变化时调用，让 %LINE% / %SEL% 保持最新。
*/
void Output_SetEditorState(int iLine, int iSelStart, int iSelEnd);

/*
  运行命令前的确认钩子。

  主窗口装上它做「文件有未保存修改，是否先保存？」之类的确认。
  返回 TRUE 才允许运行；FALSE 则本次跳过。传 NULL 表示无需确认。
*/
typedef BOOL (*OutputRunHook)(void *pUser);
void Output_SetRunHook(OutputRunHook pfnHook, void *pUser);

/*
  主窗口在打开/新建文件时调用，设置 %FILE% / %DIR% / %NAME% / %EXT% 的值。

  wszPath —— 文件全路径；空串表示尚未保存的新文档
*/
void Output_SetCurrentFile(const WCHAR *wszPath);

/* 刷新顶部信息条（显示 %FILE% 当前指向）。主窗口切换文件时自动调用。 */
void Output_RefreshStatus(void);

/* 主窗口在选中文本变化时调用，设置 %SEL% */
void Output_SetSelection(const WCHAR *wszText);

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

/*
  面板是否处于独立（浮动）窗口状态。

  嵌入时面板是主窗口的子窗口，停靠在底部；
  独立时变成带边框的顶层窗口，可以自由拖动、缩放、最大化。
*/
BOOL Output_IsFloating(void);

/*
  切换嵌入 / 独立。

  bFloat —— TRUE 独立窗口，FALSE 停靠回主窗口底部
  切换后主窗口需要重新布局（嵌入时给它留位置，独立时不留）。
*/
void Output_SetFloating(BOOL bFloat);

/*
  面板关闭请求的回调（独立窗口点关闭按钮时）。

  主窗口装上它，把菜单勾选状态同步掉。
*/
typedef void (*OutputCloseHook)(void *pUser);
void Output_SetCloseHook(OutputCloseHook pfnHook, void *pUser);

/*
  浮动窗口的矩形。主窗口在启动时用 ini 里的值调 Set，退出时用 Get
  取当前值落盘。

  没设过时面板浮在主窗口内部（720x380）。
*/
void Output_SetFloatRect(const RECT *prc);
void Output_GetFloatRect(RECT *prc);

#ifdef __cplusplus
}
#endif

#endif   /* MD_OUTPUT_OUTPUT_H */
