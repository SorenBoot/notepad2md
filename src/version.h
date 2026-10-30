/******************************************************************************
*
*
* notepad2md
*
* version.h
*   notepad2md version information
*
* 本项目基于 Notepad2 4.2.25（Florian Balmer，BSD 风格许可证）修改而来，
* 增加了 Markdown 实时预览、编码猜测/重开等功能。原始版权声明保留于下方，
* 以符合上游许可证要求。
*
* See License.txt for details about distribution and modification.
*
*                                    Notepad2 (c) Florian Balmer 1996-2011
*                                        florian.balmer@gmail.com
*                                     http://www.flos-freeware.ch
*
*
******************************************************************************/


#define VERSION_FILEVERSION_NUM      0,0,3,0
#define VERSION_FILEVERSION_SHORT    L"0.0.3"
#define VERSION_FILEVERSION_LONG     L"notepad2md 0.0.3"
#define VERSION_LEGALCOPYRIGHT_SHORT L"Copyright (c) 2004-2011 Florian Balmer"
#define VERSION_LEGALCOPYRIGHT_LONG  L"Copyright (c) 2025-2026 SorenBoot; based on Notepad2 (c) 2004-2011 Florian Balmer"
/* 派生项目自身版权行，显示在 About 对话框原作者版权下方 */
#define VERSION_LEGALCOPYRIGHT_MINE  L"Copyright (c) 2025-2026 SorenBoot"
#if defined(_M_AMD64) || defined(__x86_64__) || defined(_WIN64)
#define VERSION_FILEDESCRIPTION      L"notepad2md x64"
#elif defined(_M_IX86) || defined(__i386__)
#define VERSION_FILEDESCRIPTION      L"notepad2md x86"
#else
#define VERSION_FILEDESCRIPTION      L"notepad2md"
#endif
#define VERSION_INTERNALNAME         L"notepad2md"
#define VERSION_ORIGINALFILENAME     L"notepad2md.exe"
#define VERSION_AUTHORNAME           L"Florian Balmer"
#define VERSION_WEBPAGEDISPLAY       L"flo's freeware - http://www.flos-freeware.ch"
#define VERSION_EMAILDISPLAY         L"florian.balmer@gmail.com"

/* ---- 派生项目（notepad2md）信息 ---------------------------------------- */

/* 「基于…修改」一行，显示在 About 对话框版本号下方 */
#define VERSION_MODIFIED_FROM        L"基于 Notepad2 4.2.25 (Florian Balmer) 修改"
#define VERSION_MODIFIED_FROM_EN     L"Based on Notepad2 4.2.25 (Florian Balmer)"

/* 派生项目维护者与主页（主页指向公开仓库地址） */
#define VERSION_MAINTAINER           L"SorenBoot"
#define VERSION_MAINTAINER_WEB       L"https://github.com/SorenBoot/notepad2md"
