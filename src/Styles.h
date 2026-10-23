/******************************************************************************
*
*
* Notepad2
*
* Styles.h
*   Scintilla Style Management
*
* See Readme.txt for more information about this source code.
* Please send me your comments to this work.
*
* See License.txt for details about distribution and modification.
*
*                                              (c) Florian Balmer 1996-2011
*                                                  florian.balmer@gmail.com
*                                               http://www.flos-freeware.ch
*
*
******************************************************************************/


typedef struct _editstyle
{
  union
  {
    INT32 iStyle;
    UINT8 iStyle8[4];
  };
  int rid;
  WCHAR* pszName;
  WCHAR* pszDefault;
  WCHAR  szValue[128];

} EDITSTYLE, *PEDITSTYLE;


typedef struct _keywordlist
{
  char *pszKeyWords[9];

} KEYWORDLIST, *PKEYWORDLIST;


typedef struct _editlexer
{
  int iLexer;
  int rid;
  WCHAR* pszName;
  WCHAR* pszDefExt;
  WCHAR  szExtensions[128];
  PKEYWORDLIST pKeyWords;
  EDITSTYLE    Styles[];

} EDITLEXER, *PEDITLEXER;


// Number of Lexers in pLexArray
//
// 这个数必须与 src/Styles.c 里 pLexArray[] 的项数严格一致。
// 数组是定长的，写小了不会报错，只会把排在前面的 lexer 悄悄挤掉
// （表现是“某个扩展名突然变成默认语法”），所以下面加了编译期断言。
#define NUMLEXERS 29

/* pLexArray[] 的项数：用 sizeof 反推，不匹配就编译失败 */
#if defined(__GNUC__)
#define NUMLEXERS_COMPILE_ASSERT(cond, name) \
  typedef char numlexers_assert_##name[(cond) ? 1 : -1]
#else
#define NUMLEXERS_COMPILE_ASSERT(cond, name)
#endif


void   Style_Load();
void   Style_Save();
BOOL   Style_Import(HWND);
BOOL   Style_Export(HWND);
void   Style_SetLexer(HWND,PEDITLEXER);
void   Style_SetLongLineColors(HWND);
void   Style_SetCurrentLineBackground(HWND);
void   Style_SetLexerFromFile(HWND,LPCWSTR);
void   Style_SetLexerFromName(HWND,LPCWSTR,LPCWSTR);
void   Style_SetDefaultLexer(HWND);
void   Style_SetHTMLLexer(HWND);
void   Style_SetXMLLexer(HWND);
void   Style_SetLexerFromID(HWND,int);
void   Style_SetDefaultFont(HWND);
void   Style_ToggleUse2ndDefault(HWND);
BOOL   Style_GetUse2ndDefault(HWND);
void   Style_SetIndentGuides(HWND,BOOL);
BOOL   Style_GetOpenDlgFilterStr(LPWSTR,int);
BOOL   Style_StrGetFont(LPCWSTR,LPWSTR,int);
BOOL   Style_StrGetFontQuality(LPCWSTR,LPWSTR,int);
BOOL   Style_StrGetCharSet(LPCWSTR,int*);
BOOL   Style_StrGetSize(LPCWSTR,int*);
BOOL   Style_StrGetSizeStr(LPCWSTR,LPWSTR,int);
BOOL   Style_StrGetColor(BOOL,LPCWSTR,int*);
BOOL   Style_StrGetCase(LPCWSTR,int*);
BOOL   Style_StrGetAlpha(LPCWSTR,int*);
BOOL   Style_SelectFont(HWND,LPWSTR,int,BOOL);
BOOL   Style_SelectColor(HWND,BOOL,LPWSTR,int);
void   Style_SetStyles(HWND,int,LPCWSTR);
void   Style_SetFontQuality(HWND,LPCWSTR);
void   Style_GetCurrentLexerName(LPWSTR,int);
int    Style_GetLexerIconId(PEDITLEXER);
void   Style_AddLexerToTreeView(HWND,PEDITLEXER);
INT_PTR CALLBACK Styles_ConfigDlgProc(HWND,UINT,WPARAM,LPARAM);
void   Style_ConfigDlg(HWND);
INT_PTR CALLBACK Style_SelectLexerDlgProc(HWND,UINT,WPARAM,LPARAM);
void   Style_SelectLexerDlg(HWND);


// End of Style.h
