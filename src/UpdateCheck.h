//=============================================================================
//
//  UpdateCheck.h
//
//  「检查更新」里与 Win32 无关的纯逻辑：从 GitHub release JSON 里抓一个字段、
//  比较版本号、把发布说明压成能塞进弹窗的几行。
//
//  单独成模块是为了能脱离 GUI 做单元测试（tests/run/test_update.c）。
//
#ifndef UPDATECHECK_H
#define UPDATECHECK_H

// 在 JSON 里找 "pszKey": "值"，反转义后按 UTF-8 写入 pszOut。
// 返回 1 = 找到，0 = 没找到/格式不对。pszOut 一定以 0 结尾。
int UpdateExtractJsonString(const char *pszJson,int cbJson,
                            const char *pszKey,char *pszOut,int cchOut);

// 逐段数字比较版本号：<0 = A 旧，0 = 相同，>0 = A 新。
// 容忍 "v" 前缀、段数不等（1.0 == 1.0.0）、非数字段按 0 算。
int UpdateVersionCmp(const char *pszA,const char *pszB);

// 从 .../releases/tag/v0.0.2 取出 v0.0.2。成功返回 1。
int UpdateExtractTagFromUrl(const char *pszUrl,char *pszOut,int cchOut);

// 把各种代理写法转成 WinINet 认的格式。成功返回 1。
int UpdateNormalizeProxy(const char *pszRaw,char *pszOut,int cchOut);

// NO_PROXY 匹配。命中（应直连）返回 1。
int UpdateHostBypassed(const char *pszHost,const char *pszNoProxy);

// 远端版本是否比本地新。任一为空返回 0。
int UpdateIsNewer(const char *pszLocal,const char *pszRemote);

// 把 Markdown 风格的发布说明压成纯文本，最多 iMaxLines 行。
// 去掉 **粗体** / `代码` / # 标题记号，连续空行压成一行。
// 返回实际写入的行数；内容为空返回 0（调用方改用「无发布说明」）。
int UpdateFormatNotes(const char *pszBody,char *pszOut,int cchOut,int iMaxLines);

#endif
