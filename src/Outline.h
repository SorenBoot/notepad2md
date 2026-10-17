/*===========================================================================

  notepad2md — 大纲面板模块接口

  从 Markdown 文本中抽取标题，以缩进列表的形式显示在侧方面板中；
  点击某一项把编辑器跳到对应行。

  与 Preview.c 的关系：本模块只读文本、不碰 HTML，因此可以在预览
  未开启时独立工作。

  ===========================================================================*/

#ifndef MD_OUTLINE_OUTLINE_H
#define MD_OUTLINE_OUTLINE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 大纲面板及其内部 ListBox 的资源 ID，供主窗口布局与消息判断使用 */
#define IDC_OUTLINE       0xE702
#define IDC_OUTLINE_LIST  0xE703

/*
  初始化大纲模块。调用一次，在任何其他 Outline_* 之前。
  注册面板窗口类。
*/
BOOL Outline_Init(HINSTANCE hinst);

/*
  在父窗口内创建大纲面板。

  hwndParent —— 主框架窗口
  hinst      —— 应用实例
  返回面板窗口句柄，失败返回 NULL
*/
HWND Outline_Create(HWND hwndParent, HINSTANCE hinst);

/*
  根据 Markdown 文本重建大纲内容。

  hwnd   —— Outline_Create 返回的面板句柄
  mdText —— UTF-8 的 Markdown 源文本（NUL 结尾），可为 NULL
*/
void Outline_Update(HWND hwnd, const char *mdText);

/*
  大纲项被双击时，主窗口据此取得目标行号。

  hwnd —— 面板句柄
  idx  —— 列表索引
  返回 —— 0 based 行号；idx 无效时返回 -1
*/
int Outline_GetLine(HWND hwnd, int idx);

/*
  编辑器滚动时，主窗口把当前所在章节回填给面板以高亮之。

  hwnd —— 面板句柄
  idx  —— 列表索引；-1 表示清除高亮
*/
void Outline_SetCurrent(HWND hwnd, int idx);

#ifdef __cplusplus
}
#endif

#endif   /* MD_OUTLINE_OUTLINE_H */
