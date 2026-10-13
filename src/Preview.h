/*===========================================================================

  MDPreView — 预览控件模块接口

  ===========================================================================*/

#ifndef MD_PREVIEW_PREVIEW_H
#define MD_PREVIEW_PREVIEW_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 预览控件在资源中的 ID。主窗口布局用。 */
#define IDC_PREVIEW   0xE701

/*
  初始化预览模块。调用一次，在任何其他 Preview_* 之前。
  注册宿主窗口类并初始化 COM（STA）。
*/
BOOL Preview_Init(HINSTANCE hinst);

/*
  在父窗口内创建 MSHTML 预览控件。

  hwndParent —— 主框架窗口
  hinst      —— 应用实例
  返回宿主窗口句柄，失败返回 NULL
*/
HWND Preview_Create(HWND hwndParent, HINSTANCE hinst);

/*
  用完整 HTML 文档刷新预览。

  hwnd —— Preview_Create 返回的宿主窗口句柄
  html —— 完整 HTML 文档（UTF-8，NUL 结尾）
*/
BOOL Preview_SetHtml(HWND hwnd, const char *html);

/* 同 Preview_SetHtml */
BOOL Preview_Show(HWND hwnd, const char *html);

/* 清空预览内容 */
BOOL Preview_Clear(HWND hwnd);

/*
  取得内部 IWebBrowser2 接口（引用计数不变，调用方不得 Release）。
  供集成测试查询 DOM、以及后续的滚动同步等功能使用。
*/
IWebBrowser2* Preview_GetBrowser(void);

/*
  销毁预览模块。程序退出前调用一次。
  释放控件、注销窗口类、反初始化 COM。
*/
void Preview_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif  /* MD_PREVIEW_PREVIEW_H */
