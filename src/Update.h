//=============================================================================
//
//  Update.h
//
//  「检查更新」的 Win32 部分：后台线程拉 GitHub API → 弹窗 → 打开下载页。
//
#ifndef UPDATE_H
#define UPDATE_H

// 后台线程拉完后 PostMessage 给主窗口，wParam = 状态，lParam = 结果（主线程负责释放）
#define WM_UPDATECHECK_DONE (WM_USER+4)

#define UPDATE_OK_AVAILABLE  1   // 有新版
#define UPDATE_OK_LATEST     2   // 已最新
#define UPDATE_FAILED        3   // 网络失败

// 发起检查。已经有一次在跑就直接忽略。
void Update_Check(HWND hwnd);

// 主窗口收到 WM_UPDATECHECK_DONE 时调用。
void Update_OnDone(WPARAM wParam,LPARAM lParam);

#endif
