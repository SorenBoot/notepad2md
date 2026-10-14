/*===========================================================================

  notepad2md — 预览控件模块
  用 MSHTML（系统 Trident 引擎）渲染 Markdown 生成的 HTML。

  为什么用 MSHTML 而不是 WebView2：
  - MSHTML 自 Win98 起随系统分发，目标机器零安装
  - in-proc ActiveX，无多进程开销，内存增量小
  - WebView2 Runtime 在 Win10 需单独安装（100MB+），违背轻量目标

  实现方式（纯 C，不依赖 ATL）：
  MinGW 未提供 ATL 头文件，无法用 AtlAxGetHost/AtlAxGetControl。
  故自行实现最小 IOleClientSite / IOleInPlaceSite，通过 OleCreate 创建
  Shell.Explorer 控件并托管在自建宿主窗口里。

  托管链路：
    OleCreate(CLSID_WebBrowser) -> IOleObject
      -> SetClientSite(g_pOleClientSite)
      -> QueryInterface(IID_IWebBrowser2)  <- 渲染接口
      -> DoVerb(OLEIVERB_INPLACEACTIVATE)  <- 创建窗口并就地激活

  渲染方式：
  用 IPersistStreamInit::Load 灌 HTML 流，不用 Navigate()。
  避免 file:// 协议、安全拦阻、缓存污染和临时文件。

  兼容性：
  - HTML 必须带 <meta http-equiv="X-UA-Compatible" content="IE=edge">
  - CSS 仅用 IE11 支持的特性

  ===========================================================================*/

#include <windows.h>
#include <ole2.h>
#include <oleidl.h>
#include <ocidl.h>
#include <exdisp.h>
#include <mshtml.h>
#include <mshtmhst.h>
#include <oleauto.h>

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "Preview.h"


#ifdef PREVIEW_DEBUG
static void dbg(const char *fmt, ...)
{
  char buf[512];
  va_list ap;
  FILE *fp;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  OutputDebugStringA(buf);
  fp = fopen("preview_debug.log", "a");
  if (fp) { fprintf(fp, "%s\n", buf); fclose(fp); }
}
#define DBG(...) dbg(__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif



/* ---- 常量 -------------------------------------------------------------- */

#define PREVIEW_CLASS  TEXT("notepad2mdHost")


/* ---- 接口定义（MinGW 头文件已提供，此处仅声明用到的）------------------- */

/* IID_IOleClientSite / IID_IOleInPlaceSite 在 MinGW 的 uuid 库中提供。
   但 IID_IOleInPlaceFrame / IID_IOleInPlaceUIWindow 可能缺失，
   显式定义以避免链接问题。这些 GUID 是 MSDN 固定值。 */

static const IID IID_IOleInPlaceFrame_ = {
  0x00000116, 0x0000, 0x0000,
  {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
};
static const IID IID_IOleInPlaceUIWindow_ = {
  0x00000115, 0x0000, 0x0000,
  {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
};

/* CLSID_WebBrowser {8856F961-340A-11D0-A96B-00C04FD705A2} */
static const CLSID CLSID_WebBrowser_ = {
  0x8856F961, 0x340A, 0x11D0,
  {0xA9, 0x6B, 0x00, 0xC0, 0x4F, 0xD7, 0x05, 0xA2}
};


/* ---- 剪贴板 -------------------------------------------------------------

  脚本里调 window.external.MDCopy(text)，最终落到这里。
  纯 JS 的 window.clipboardData / execCommand('Copy') 在本控件内实测
  均静默失败，必须借助宿主桥（IDocHostUIHandler::GetExternal）。
  参数由 IE 封送为 BSTR，多行文本与缩进都能完整保留。 */

static HWND g_hwndHostForClip = NULL;

static void CopyToClipboard(HWND hwnd, const WCHAR *text)
{
  size_t cch;
  HGLOBAL hMem;
  void *pDest;

  if (!text)
    return;

  if (!OpenClipboard(hwnd))
    return;

  EmptyClipboard();

  cch = wcslen(text);
  /* 末尾的换行（代码块常以 \n 结尾）去掉，贴回去更干净 */
  while (cch > 0 && (text[cch-1] == L'\n' || text[cch-1] == L'\r'))
    cch--;

  hMem = GlobalAlloc(GMEM_MOVEABLE, (cch + 1) * sizeof(WCHAR));
  if (hMem) {
    pDest = GlobalLock(hMem);
    if (pDest) {
      memcpy(pDest, text, cch * sizeof(WCHAR));
      ((WCHAR*)pDest)[cch] = L'\0';
      GlobalUnlock(hMem);
      /* 成功后所有权归剪贴板，不能再 GlobalFree */
      if (!SetClipboardData(CF_UNICODETEXT, hMem))
        GlobalFree(hMem);
    } else {
      GlobalFree(hMem);
    }
  }

  CloseClipboard();
}


/* ---- 宿主脚本桥：window.external ----------------------------------------

  实现一个最小 IDispatch，只认一个方法名 MDCopy。
  IE 以 DISPATCH_METHOD 调 Invoke，rgvarg[0] 就是按钮传进来的字符串（vt=8 BSTR）。
  对象独立计数：IE 会多次索取 GetExternal 并各自 AddRef，不能与 HostSite 共用计数。
*/

#define MD_DISPID_COPY 1
#define MD_DISPID_OPEN 2

typedef struct tagScriptBridge {
  IDispatch  disp;      /* 必须第一个成员 */
  LONG       refs;
} ScriptBridge;

static ScriptBridge *g_pBridge = NULL;


static HRESULT STDMETHODCALLTYPE Bridge_QueryInterface(IDispatch *This,
                                                       REFIID riid, void **ppv)
{
  if (!ppv) return E_POINTER;
  *ppv = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch)) {
    *ppv = This;
    This->lpVtbl->AddRef(This);
    return S_OK;
  }
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Bridge_AddRef(IDispatch *This)
{
  ScriptBridge *b = (ScriptBridge*)This;
  return (ULONG)InterlockedIncrement(&b->refs);
}

static ULONG STDMETHODCALLTYPE Bridge_Release(IDispatch *This)
{
  ScriptBridge *b = (ScriptBridge*)This;
  LONG r = InterlockedDecrement(&b->refs);
  if (r <= 0) {
    free(b);
    if (g_pBridge == b)
      g_pBridge = NULL;
  }
  return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE Bridge_GetTypeInfoCount(IDispatch *This, UINT *pctinfo)
{
  (void)This;
  if (!pctinfo) return E_POINTER;
  *pctinfo = 0;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Bridge_GetTypeInfo(IDispatch *This, UINT iTInfo,
                                                    LCID lcid, ITypeInfo **ppTInfo)
{
  (void)This; (void)iTInfo; (void)lcid;
  if (ppTInfo) *ppTInfo = NULL;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Bridge_GetIDsOfNames(IDispatch *This, REFIID riid,
                                                      LPOLESTR *rgszNames, UINT cNames,
                                                      LCID lcid, DISPID *rgDispId)
{
  (void)This; (void)riid; (void)lcid;
  if (!rgszNames || !rgDispId || cNames < 1) return E_INVALIDARG;
  if (rgszNames[0] && lstrcmpiW(rgszNames[0], L"MDCopy") == 0) {
    rgDispId[0] = MD_DISPID_COPY;
    return S_OK;
  }
  if (rgszNames[0] && lstrcmpiW(rgszNames[0], L"MDOpen") == 0) {
    rgDispId[0] = MD_DISPID_OPEN;
    return S_OK;
  }
  rgDispId[0] = DISPID_UNKNOWN;
  return DISP_E_UNKNOWNNAME;
}

static HRESULT STDMETHODCALLTYPE Bridge_Invoke(IDispatch *This, DISPID dispIdMember,
                                               REFIID riid, LCID lcid, WORD wFlags,
                                               DISPPARAMS *pDispParams, VARIANT *pVarResult,
                                               EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
  (void)This; (void)riid; (void)lcid; (void)pVarResult; (void)pExcepInfo;

  if (dispIdMember != MD_DISPID_COPY && dispIdMember != MD_DISPID_OPEN)
    return DISP_E_MEMBERNOTFOUND;

  if (!(wFlags & DISPATCH_METHOD))
    return DISP_E_MEMBERNOTFOUND;

  if (!pDispParams || pDispParams->cArgs < 1 || !pDispParams->rgvarg)
    return E_INVALIDARG;

  {
    VARIANT *pv = &pDispParams->rgvarg[0];   /* 参数按逆序：第一个参数在 rgvarg[0] */
    VARIANT vt;
    HRESULT hr;

    VariantInit(&vt);
    hr = VariantChangeType(&vt, pv, 0, VT_BSTR);
    if (SUCCEEDED(hr) && vt.vt == VT_BSTR) {
      if (dispIdMember == MD_DISPID_COPY)
        CopyToClipboard(g_hwndHostForClip, vt.bstrVal);
      else
        ShellExecuteW(g_hwndHostForClip, L"open", vt.bstrVal, NULL, NULL, SW_SHOWNORMAL);
      VariantClear(&vt);
      return S_OK;
    }
    VariantClear(&vt);
    if (puArgErr) *puArgErr = 0;
    return DISP_E_TYPEMISMATCH;
  }
}

static IDispatchVtbl g_vtblBridge = {
  Bridge_QueryInterface,
  Bridge_AddRef,
  Bridge_Release,
  Bridge_GetTypeInfoCount,
  Bridge_GetTypeInfo,
  Bridge_GetIDsOfNames,
  Bridge_Invoke
};

static IDispatch* Bridge_Get(void)
{
  if (!g_pBridge) {
    g_pBridge = (ScriptBridge*)calloc(1, sizeof(ScriptBridge));
    if (!g_pBridge)
      return NULL;
    g_pBridge->disp.lpVtbl = &g_vtblBridge;
    g_pBridge->refs = 1;
  }
  g_pBridge->disp.lpVtbl->AddRef(&g_pBridge->disp);
  return &g_pBridge->disp;
}


/* ---- IDocHostUIHandler -------------------------------------------------

  只为了提供 GetExternal（把 window.external 指到脚本桥）。
  其余方法必须给出「不接管」的返回值，否则 IE 会改变宿主化行为：
    - GetHostInfo 返 E_NOTIMPL：保留 IE 默认外观/行为（不接菜单/工具栏）
    - TranslateAccelerator 返 S_FALSE：不拦截键盘，按键照常给编辑器
    - ShowUI/HideUI/… 返 S_OK 或 S_FALSE：表示「无额外 UI 要显示」
*/

static HRESULT STDMETHODCALLTYPE UI_QueryInterface(IDocHostUIHandler *This,
                                                   REFIID riid, void **ppv);
static ULONG   STDMETHODCALLTYPE UI_AddRef(IDocHostUIHandler *This);
static ULONG   STDMETHODCALLTYPE UI_Release(IDocHostUIHandler *This);

static HRESULT STDMETHODCALLTYPE UI_ShowContextMenu(IDocHostUIHandler *This, DWORD dwID,
                                                    POINT *ppt, IUnknown *pcmdtReserved,
                                                    IDispatch *pdispReserved)
{
  (void)This; (void)dwID; (void)ppt; (void)pcmdtReserved; (void)pdispReserved;
  return S_FALSE;   /* 不接管：使用 IE 默认右键菜单 */
}

static HRESULT STDMETHODCALLTYPE UI_GetHostInfo(IDocHostUIHandler *This, DOCHOSTUIINFO *pInfo)
{
  (void)This; (void)pInfo;
  return E_NOTIMPL;   /* 宿主不提供，IE 用默认值 */
}

static HRESULT STDMETHODCALLTYPE UI_ShowUI(IDocHostUIHandler *This, DWORD dwID,
                                           IOleInPlaceActiveObject *pActiveObject,
                                           IOleCommandTarget *pCommandTarget,
                                           IOleInPlaceFrame *pFrame,
                                           IOleInPlaceUIWindow *pDoc)
{
  (void)This; (void)dwID; (void)pActiveObject; (void)pCommandTarget; (void)pFrame; (void)pDoc;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE UI_HideUI(IDocHostUIHandler *This)
{
  (void)This;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE UI_UpdateUI(IDocHostUIHandler *This)
{
  (void)This;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE UI_EnableModeless(IDocHostUIHandler *This, WINBOOL fEnable)
{
  (void)This; (void)fEnable;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE UI_OnDocWindowActivate(IDocHostUIHandler *This, WINBOOL fActivate)
{
  (void)This; (void)fActivate;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE UI_OnFrameWindowActivate(IDocHostUIHandler *This, WINBOOL fActivate)
{
  (void)This; (void)fActivate;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE UI_ResizeBorder(IDocHostUIHandler *This, LPCRECT prcBorder,
                                                 IOleInPlaceUIWindow *pUIWindow,
                                                 WINBOOL fRameWindow)
{
  (void)This; (void)prcBorder; (void)pUIWindow; (void)fRameWindow;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE UI_TranslateAccelerator(IDocHostUIHandler *This, LPMSG lpMsg,
                                                         const GUID *pguidCmdGroup, DWORD nCmdID)
{
  (void)This; (void)lpMsg; (void)pguidCmdGroup; (void)nCmdID;
  return S_FALSE;   /* 不拦截，交给主消息循环 */
}

static HRESULT STDMETHODCALLTYPE UI_GetOptionKeyPath(IDocHostUIHandler *This, LPOLESTR *pchKey,
                                                     DWORD dw)
{
  (void)This; (void)pchKey; (void)dw;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE UI_GetDropTarget(IDocHostUIHandler *This, IDropTarget *pDropTarget,
                                                  IDropTarget **ppDropTarget)
{
  (void)This; (void)pDropTarget;
  if (ppDropTarget) *ppDropTarget = NULL;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE UI_GetExternal(IDocHostUIHandler *This, IDispatch **ppDispatch)
{
  (void)This;
  if (!ppDispatch) return E_POINTER;
  *ppDispatch = Bridge_Get();
  DBG("UI_GetExternal -> %p", (void*)*ppDispatch);
  return *ppDispatch ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE UI_TranslateUrl(IDocHostUIHandler *This, DWORD dwTranslate,
                                                 LPWSTR pchURLIn, LPWSTR *ppchURLOut)
{
  (void)This; (void)dwTranslate; (void)pchURLIn;
  if (ppchURLOut) *ppchURLOut = NULL;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE UI_FilterDataObject(IDocHostUIHandler *This, IDataObject *pDO,
                                                     IDataObject **ppDORet)
{
  (void)This; (void)pDO;
  if (ppDORet) *ppDORet = NULL;
  return E_NOTIMPL;
}

static IDocHostUIHandlerVtbl g_vtblUI = {
  UI_QueryInterface,
  UI_AddRef,
  UI_Release,
  UI_ShowContextMenu,
  UI_GetHostInfo,
  UI_ShowUI,
  UI_HideUI,
  UI_UpdateUI,
  UI_EnableModeless,
  UI_OnDocWindowActivate,
  UI_OnFrameWindowActivate,
  UI_ResizeBorder,
  UI_TranslateAccelerator,
  UI_GetOptionKeyPath,
  UI_GetDropTarget,
  UI_GetExternal,
  UI_TranslateUrl,
  UI_FilterDataObject
};


/* ---- 宿主站点实现 ------------------------------------------------------ */

typedef struct tagHostSite {
  IOleInPlaceSite      site;      /* 必须第一个成员：vtable 布局兼容 */
  IOleClientSite       client;    /* 独立 vtable，单独计数 */
  IOleInPlaceFrame     frame;    /* 同时充当 IOleInPlaceUIWindow（见下）*/
  IDocHostUIHandler    uih;      /* 提供 window.external 等宿主化钩子 */
  LONG                 refs;

  HWND                 hwndHost;
  IOleObject          *pOleObj;
  IOleInPlaceObject   *pInPlaceObj;
  IWebBrowser2        *pWB;
  RECT                 rc;
} HostSite;


static HostSite *g_pSite = NULL;   /* 单实例：本工具只有一个预览控件 */

static BOOL bComInit         = FALSE;
static BOOL bActivated      = FALSE;   /* DoVerb 是否已成功（决定 Shutdown 是否调 Close） */
static BOOL bClassRegistered = FALSE;


/* ---- IUnknown ---------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE Site_QueryInterface(IOleInPlaceSite *This,
                                                      REFIID riid, void **ppv)
{
  HostSite *s = (HostSite*)This;

  DBG("Site_QI: {%08lX-...}", (unsigned long)riid->Data1);

  if (!ppv) return E_POINTER;
  *ppv = NULL;

  if (IsEqualIID(riid, &IID_IUnknown) ||
      IsEqualIID(riid, &IID_IOleInPlaceSite)) {
    *ppv = &s->site;
    DBG("  -> matched IOleInPlaceSite/Unknown, returning site");
  }
  else if (IsEqualIID(riid, &IID_IOleClientSite)) {
    *ppv = &s->client;
    DBG("  -> matched IOleClientSite, returning client");
  }
  /* Frame / UIWindow 是不同的 vtable 布局，必须返回各自成员。
     返回 site 会导致控件按 Frame 布局调用 IOleInPlaceSite 的函数指针，
     实测 E_FAIL（DoVerb 0x80004005）。 */
  else if (IsEqualIID(riid, &IID_IOleInPlaceFrame)) {
    *ppv = &s->frame;
    DBG("  -> matched IOleInPlaceFrame, returning frame");
  }
  else if (IsEqualIID(riid, &IID_IOleInPlaceUIWindow)) {
    /* 返回 frame 而非单独对象：IOleInPlaceFrame 继承自 IOleInPlaceUIWindow，
       前 8 项 vtable 完全一致，所以这是合法的 QI 结果；
       而调用方（ieframe）习惯把拿到的文档层站点直接当 IOleInPlaceFrame 用，
       若给它一个只有 8 项的 vtable，第 8 项 SetActiveObject 就会越界读到 NULL。 */
    *ppv = &s->frame;
    DBG("  -> matched IOleInPlaceUIWindow, returning frame");
  }
  /* 宿主化钩子：ieframe 每建一个新文档都会向 client site 重新要一次，
     所以在这里应答即可，不必担心文档重建。 */
  else if (IsEqualIID(riid, &IID_IDocHostUIHandler)) {
    *ppv = &s->uih;
    DBG("  -> matched IDocHostUIHandler");
  }
  else {
    DBG("  -> NO INTERFACE");
  }

  if (*ppv) {
    ((IUnknown*)*ppv)->lpVtbl->AddRef((IUnknown*)*ppv);
    return S_OK;
  }
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Site_AddRef(IOleInPlaceSite *This)
{
  return (ULONG)InterlockedIncrement(&((HostSite*)This)->refs);
}

static ULONG STDMETHODCALLTYPE Site_Release(IOleInPlaceSite *This)
{
  HostSite *s = (HostSite*)This;
  LONG r = InterlockedDecrement(&s->refs);
  if (r <= 0) {
    if (s->pWB)        s->pWB->lpVtbl->Release(s->pWB);
    if (s->pInPlaceObj) s->pInPlaceObj->lpVtbl->Release(s->pInPlaceObj);
    if (s->pOleObj)    s->pOleObj->lpVtbl->Release(s->pOleObj);
    free(s);
    g_pSite = NULL;
  }
  return (ULONG)r;
}

/* IOleClientSite 的 IUnknown：This 类型不同，需单独实现。
   关键：This 是 &s->client 而非结构体基址（第一个成员是 site），
   必须用 offsetof 回推。直接 (HostSite*)This 会把结构体中部当成基址，
   引用计数在垃圾内存上原子增减，减到 0 就释放垃圾指针 -> 堆损坏。 */
static HRESULT STDMETHODCALLTYPE Client_QueryInterface(IOleClientSite *This,
                                                       REFIID riid, void **ppv)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, client));
  return Site_QueryInterface(&s->site, riid, ppv);
}

static ULONG STDMETHODCALLTYPE Client_AddRef(IOleClientSite *This)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, client));
  return (ULONG)InterlockedIncrement(&s->refs);
}

static ULONG STDMETHODCALLTYPE Client_Release(IOleClientSite *This)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, client));
  return Site_Release(&s->site);
}

/* IOleInPlaceFrame 的 IUnknown */
static HRESULT STDMETHODCALLTYPE Frame_QueryInterface(IOleInPlaceFrame *This,
                                                      REFIID riid, void **ppv)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, frame));
  return Site_QueryInterface(&s->site, riid, ppv);
}

static ULONG STDMETHODCALLTYPE Frame_AddRef(IOleInPlaceFrame *This)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, frame));
  return (ULONG)InterlockedIncrement(&s->refs);
}

static ULONG STDMETHODCALLTYPE Frame_Release(IOleInPlaceFrame *This)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, frame));
  return Site_Release(&s->site);
}


/* ---- IDocHostUIHandler 的 IUnknown（在 HostSite 定义之后才能用 offsetof）-- */

static HRESULT STDMETHODCALLTYPE UI_QueryInterface(IDocHostUIHandler *This,
                                                   REFIID riid, void **ppv)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, uih));
  return Site_QueryInterface(&s->site, riid, ppv);
}

static ULONG STDMETHODCALLTYPE UI_AddRef(IDocHostUIHandler *This)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, uih));
  return (ULONG)InterlockedIncrement(&s->refs);
}

static ULONG STDMETHODCALLTYPE UI_Release(IDocHostUIHandler *This)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, uih));
  return Site_Release(&s->site);
}


/* ---- IOleWindow（IOleInPlaceSite 的基接口）----------------------------- */

static HRESULT STDMETHODCALLTYPE Site_GetWindow(IOleInPlaceSite *This, HWND *phwnd)
{
  HostSite *s = (HostSite*)This;
  DBG("Site_GetWindow -> 0x%p", s->hwndHost);
  if (!phwnd) return E_POINTER;
  *phwnd = s->hwndHost;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Site_ContextSensitiveHelp(IOleInPlaceSite *This, BOOL f)
{
  (void)This; (void)f;
  return E_NOTIMPL;
}


/* ---- IOleInPlaceSite --------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE Site_CanInPlaceActivate(IOleInPlaceSite *This)
{
  DBG("Site_CanInPlaceActivate -> S_OK");
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Site_OnInPlaceActivate(IOleInPlaceSite *This)
{
  DBG("Site_OnInPlaceActivate");
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Site_OnUIActivate(IOleInPlaceSite *This)
{
  DBG("Site_OnUIActivate");
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Site_GetWindowContext(
    IOleInPlaceSite *This,
    IOleInPlaceFrame **ppFrame, IOleInPlaceUIWindow **ppDoc,
    LPRECT prcPosRect, LPRECT prcClipRect,
    LPOLEINPLACEFRAMEINFO lpFrameInfo)
{
  HostSite *s = (HostSite*)This;

  DBG("Site_GetWindowContext rc=(%ld,%ld,%ld,%ld)",
      (long)s->rc.left, (long)s->rc.top, (long)s->rc.right, (long)s->rc.bottom);

  if (!ppFrame || !ppDoc || !prcPosRect || !prcClipRect || !lpFrameInfo)
    return E_POINTER;

  /* 两个槽都返回 frame 对象。
     ppDoc 的类型是 IOleInPlaceUIWindow，而 IOleInPlaceFrame 继承自它，
     所以同一个对象满足两者。宿主实现普遍这么做，因为调用方会把 ppDoc
     当 IOleInPlaceFrame 使用（ieframe 就是这样调 SetActiveObject 的）。 */
  *ppFrame = &s->frame;
  *ppDoc   = (IOleInPlaceUIWindow*)&s->frame;

  s->frame.lpVtbl->AddRef(&s->frame);
  s->frame.lpVtbl->AddRef(&s->frame);   /* ppDoc 也持一份引用 */

  *prcPosRect  = s->rc;
  *prcClipRect = s->rc;

  lpFrameInfo->cb           = sizeof(OLEINPLACEFRAMEINFO);
  lpFrameInfo->fMDIApp      = FALSE;
  lpFrameInfo->hwndFrame    = s->hwndHost;
  lpFrameInfo->haccel       = NULL;
  lpFrameInfo->cAccelEntries= 0;

  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Site_Scroll(IOleInPlaceSite *This, SIZE sz)
{
  (void)This; (void)sz;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Site_OnUIDeactivate(IOleInPlaceSite *This, BOOL f)
{
  (void)This; (void)f;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Site_OnInPlaceDeactivate(IOleInPlaceSite *This)
{
  (void)This;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Site_DiscardUndoState(IOleInPlaceSite *This)
{
  (void)This;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Site_DeactivateAndUndo(IOleInPlaceSite *This)
{
  (void)This;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Site_OnPosRectChange(IOleInPlaceSite *This, LPCRECT prc)
{
  HostSite *s = (HostSite*)This;
  if (!prc) return E_POINTER;
  s->rc = *prc;
  if (s->pInPlaceObj)
    s->pInPlaceObj->lpVtbl->SetObjectRects(s->pInPlaceObj, &s->rc, &s->rc);
  return S_OK;
}


/* ---- IOleClientSite ---------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE Client_SaveObject(IOleClientSite *This)
{
  (void)This;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Client_GetMoniker(
    IOleClientSite *This, DWORD dwAssign, DWORD dwWhich, IMoniker **ppmk)
{
  (void)This; (void)dwAssign; (void)dwWhich; (void)ppmk;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Client_GetContainer(
    IOleClientSite *This, IOleContainer **ppContainer)
{
  (void)This;
  if (!ppContainer) return E_POINTER;
  *ppContainer = NULL;
  return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE Client_ShowObject(IOleClientSite *This)
{
  (void)This;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Client_OnShowWindow(IOleClientSite *This, BOOL fShow)
{
  (void)This; (void)fShow;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Client_RequestNewObjectLayout(IOleClientSite *This)
{
  (void)This;
  return E_NOTIMPL;
}


/* ---- IOleInPlaceFrame -------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE Frame_GetWindow(IOleInPlaceFrame *This, HWND *phwnd)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, frame));
  if (!phwnd) return E_POINTER;
  *phwnd = s->hwndHost;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Frame_ContextSensitiveHelp(IOleInPlaceFrame *This, BOOL f)
{
  (void)This; (void)f;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Frame_GetBorder(IOleInPlaceFrame *This, LPRECT prc)
{
  (void)This; (void)prc;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Frame_RequestBorderSpace(
    IOleInPlaceFrame *This, LPCBORDERWIDTHS pbw)
{
  (void)This; (void)pbw;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Frame_SetBorderSpace(
    IOleInPlaceFrame *This, LPCBORDERWIDTHS pbw)
{
  (void)This; (void)pbw;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Frame_SetActiveObject(
    IOleInPlaceFrame *This, IOleInPlaceActiveObject *pActive, LPCOLESTR pszObj)
{
  (void)This; (void)pActive; (void)pszObj;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Frame_InsertMenus(
    IOleInPlaceFrame *This, HMENU hmenuShared, LPOLEMENUGROUPWIDTHS lpMenuW)
{
  (void)This; (void)hmenuShared; (void)lpMenuW;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Frame_SetMenu(
    IOleInPlaceFrame *This, HMENU hmenuShared, HOLEMENU holemenu, HWND hwndActive)
{
  (void)This; (void)hmenuShared; (void)holemenu; (void)hwndActive;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Frame_RemoveMenus(IOleInPlaceFrame *This, HMENU hmenuShared)
{
  (void)This; (void)hmenuShared;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Frame_SetStatusText(IOleInPlaceFrame *This, LPCOLESTR psz)
{
  (void)This; (void)psz;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Frame_EnableModeless(IOleInPlaceFrame *This, BOOL f)
{
  (void)This; (void)f;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Frame_TranslateAccelerator(
    IOleInPlaceFrame *This, LPMSG lpmsg, WORD wID)
{
  (void)This; (void)lpmsg; (void)wID;
  return S_FALSE;
}


/* ---- vtable 定义 ------------------------------------------------------- */

static IOleInPlaceSiteVtbl g_vtblSite = {
  Site_QueryInterface,
  Site_AddRef,
  Site_Release,
  Site_GetWindow,
  Site_ContextSensitiveHelp,
  Site_CanInPlaceActivate,
  Site_OnInPlaceActivate,
  Site_OnUIActivate,
  Site_GetWindowContext,
  Site_Scroll,
  Site_OnUIDeactivate,
  Site_OnInPlaceDeactivate,
  Site_DiscardUndoState,
  Site_DeactivateAndUndo,
  Site_OnPosRectChange
};

static IOleClientSiteVtbl g_vtblClient = {
  Client_QueryInterface,
  Client_AddRef,
  Client_Release,
  Client_SaveObject,
  Client_GetMoniker,
  Client_GetContainer,
  Client_ShowObject,
  Client_OnShowWindow,
  Client_RequestNewObjectLayout
};

static IOleInPlaceFrameVtbl g_vtblFrame = {
  Frame_QueryInterface,
  Frame_AddRef,
  Frame_Release,
  Frame_GetWindow,
  Frame_ContextSensitiveHelp,
  Frame_GetBorder,
  Frame_RequestBorderSpace,
  Frame_SetBorderSpace,
  Frame_SetActiveObject,
  Frame_InsertMenus,
  Frame_SetMenu,
  Frame_RemoveMenus,
  Frame_SetStatusText,
  Frame_EnableModeless,
  Frame_TranslateAccelerator
};

/* ---- 站点构造 ---------------------------------------------------------- */


static HostSite* Site_Create(HWND hwndHost)
{
  HostSite *s = (HostSite*)calloc(1, sizeof(HostSite));
  if (!s)
    return NULL;

  s->site.lpVtbl    = &g_vtblSite;
  s->client.lpVtbl  = &g_vtblClient;
  s->frame.lpVtbl   = &g_vtblFrame;
  s->uih.lpVtbl     = &g_vtblUI;
  s->refs           = 1;
  s->hwndHost       = hwndHost;
  GetClientRect(hwndHost, &s->rc);

  g_hwndHostForClip = hwndHost;
  g_pSite = s;
  return s;
}


/* ---- 宿主窗口过程 ------------------------------------------------------ */

static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch (msg) {
    case WM_SIZE:
      if (g_pSite) {
        GetClientRect(hwnd, &g_pSite->rc);
        if (g_pSite->pInPlaceObj)
          g_pSite->pInPlaceObj->lpVtbl->SetObjectRects(
            g_pSite->pInPlaceObj, &g_pSite->rc, &g_pSite->rc);
      }
      return 0;

    case WM_ERASEBKGND:
      /* MSHTML 覆盖整个客户区，避免闪烁 */
      return 1;
  }
  return DefWindowProc(hwnd, msg, wParam, lParam);
}


/* ---- 对外接口 ---------------------------------------------------------- */

BOOL Preview_Init(HINSTANCE hinst)
{
  WNDCLASSEX wc;
  HRESULT hr;

  if (bClassRegistered)
    return TRUE;

  memset(&wc, 0, sizeof(wc));
  wc.cbSize        = sizeof(wc);
  wc.style         = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc   = HostWndProc;
  wc.hInstance     = hinst;
  wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
  wc.lpszClassName = PREVIEW_CLASS;

  if (!RegisterClassEx(&wc))
    return FALSE;

  bClassRegistered = TRUE;

  /* MSHTML ActiveX 要求 STA */
  hr = CoInitialize(NULL);
  if (hr == S_OK || hr == S_FALSE)
    bComInit = TRUE;
  else if (hr != RPC_E_CHANGED_MODE)
    return FALSE;

  return TRUE;
}


HWND Preview_Create(HWND hwndParent, HINSTANCE hinst)
{
  HWND hwndHost;
  HostSite *site;
  HRESULT hr;
  IOleObject *pOle = NULL;
  RECT rcParent;

  if (!bClassRegistered)
    return NULL;

  /* WebBrowser 控件拒绝在零尺寸宿主里激活（DoVerb 返回 E_FAIL），
     创建时即用父窗口客户区大小。后续由 WM_SIZE 调整。 */
  GetClientRect(hwndParent, &rcParent);

  hwndHost = CreateWindowEx(
               0, PREVIEW_CLASS, NULL,
               WS_CHILD | WS_CLIPSIBLINGS,
               0, 0, rcParent.right, rcParent.bottom,
               hwndParent, (HMENU)IDC_PREVIEW, hinst, NULL);

  if (!hwndHost) {
    DBG("Preview_Create: CreateWindowEx host failed err=%lu", GetLastError());
    return NULL;
  }
  DBG("Preview_Create: host hwnd=0x%p", hwndHost);

  site = Site_Create(hwndHost);
  if (!site)
    return NULL;

  /* 1. 创建 MSHTML 控件对象（in-proc）*/
  hr = CoCreateInstance(&CLSID_WebBrowser_, NULL, CLSCTX_INPROC_SERVER,
                        &IID_IOleObject, (void**)&pOle);
  if (FAILED(hr)) {
    DBG("Preview_Create: CoCreateInstance(WebBrowser) failed hr=0x%08lX", (unsigned long)hr);
    return NULL;
  }
  DBG("Preview_Create: CoCreateInstance ok");

  site->pOleObj = pOle;

  /* 2. 绑定宿主站点 */
  hr = pOle->lpVtbl->SetClientSite(pOle, &site->client);
  if (FAILED(hr)) {
    DBG("Preview_Create: SetClientSite failed hr=0x%08lX", (unsigned long)hr);
    return NULL;
  }
  DBG("Preview_Create: SetClientSite ok");

  /* 嵌入式对象生命周期归容器管，且需显式转入运行态 */
  OleSetContainedObject((LPUNKNOWN)pOle, TRUE);
  DBG("Preview_Create: OleSetContainedObject ok");

  OleRun((LPUNKNOWN)pOle);
  DBG("Preview_Create: OleRun ok");

  /* 宿主名：控件内部可能用于文档标题等，补上以完全符合规范 */
  pOle->lpVtbl->SetHostNames(pOle, L"notepad2md", L"Preview");

  /* 3. 取渲染接口 */
  hr = pOle->lpVtbl->QueryInterface(pOle, &IID_IWebBrowser2, (void**)&site->pWB);
  if (FAILED(hr)) {
    DBG("Preview_Create: QI(IWebBrowser2) failed hr=0x%08lX", (unsigned long)hr);
    return NULL;
  }
  DBG("Preview_Create: QI(IWebBrowser2) ok");

  /* 4. 取 in-place 接口 */
  hr = pOle->lpVtbl->QueryInterface(pOle, &IID_IOleInPlaceObject,
                                    (void**)&site->pInPlaceObj);
  if (FAILED(hr)) {
    DBG("Preview_Create: QI(IOleInPlaceObject) failed hr=0x%08lX", (unsigned long)hr);
    return NULL;
  }
  DBG("Preview_Create: QI(IOleInPlaceObject) ok");

  /* 5. 就地激活 —— 此步才会真正创建控件窗口。
     注意 lindex 按 MSDN 规定必须为 -1（保留参数），传 0 会导致 E_FAIL。
     INPLACEACTIVATE 失败时退回 SHOW。 */
  hr = pOle->lpVtbl->DoVerb(pOle, OLEIVERB_INPLACEACTIVATE, NULL,
                            &site->client, -1, hwndHost, &site->rc);
  if (FAILED(hr)) {
    DBG("Preview_Create: DoVerb(INPLACEACTIVATE) failed hr=0x%08lX, retrying SHOW",
        (unsigned long)hr);
    hr = pOle->lpVtbl->DoVerb(pOle, OLEIVERB_SHOW, NULL,
                              &site->client, -1, hwndHost, &site->rc);
    if (FAILED(hr)) {
      DBG("Preview_Create: DoVerb(SHOW) failed hr=0x%08lX", (unsigned long)hr);
      return NULL;
    }
  }
  bActivated = TRUE;
  DBG("Preview_Create: DoVerb ok");

  /* 先导航到 about:blank 建立 MSHTML 文档对象。
     未导航过的浏览器没有文档，后续 Load 必返 E_FAIL（实测）。 */
  {
    VARIANT vNull;
    VariantInit(&vNull);
    site->pWB->lpVtbl->Navigate(site->pWB, L"about:blank",
                                 &vNull, &vNull, &vNull, &vNull);
    VariantClear(&vNull);
    DBG("Preview_Create: Navigate(about:blank) issued");
  }

  ShowWindow(hwndHost, SW_SHOW);
  DBG("Preview_Create: success hwnd=0x%p", hwndHost);
  return hwndHost;
}


/*
  用完整 HTML 文档刷新预览。
  返回 TRUE 成功；FALSE = 文档尚未就绪（about:blank 导航未完成），
  调用方可稍后重试（应用层的防抖定时器会自然重试）。

  实现：灌到 IHTMLDocument2 的 IPersistStreamInit 上。
  在浏览器对象上灌流（旧方案）在没有文档时必返 E_FAIL。
*/
static BOOL InstallLinkDelegate(void);

BOOL Preview_SetHtml(HWND hwnd, const char *html)
{
  IWebBrowser2 *pWB;
  IDispatch *pDisp = NULL;
  IHTMLDocument2 *pDoc = NULL;
  IPersistStreamInit *pPersist = NULL;
  IStream *pStream = NULL;
  HGLOBAL hGlobal;
  int len;
  HRESULT hr;

  if (!hwnd || !html || !g_pSite || !g_pSite->pWB)
    return FALSE;

  pWB = g_pSite->pWB;
  len = (int)strlen(html);
  if (len <= 0)
    return FALSE;

  /* 文档必须已建立 */
  hr = pWB->lpVtbl->get_Document(pWB, &pDisp);
  if (FAILED(hr) || !pDisp) {
    DBG("SetHtml: document not ready yet");
    return FALSE;
  }

  hr = pDisp->lpVtbl->QueryInterface(pDisp, &IID_IHTMLDocument2, (void**)&pDoc);
  pDisp->lpVtbl->Release(pDisp);
  if (FAILED(hr) || !pDoc) {
    DBG("SetHtml: QI(IHTMLDocument2) failed hr=0x%08lX", (unsigned long)hr);
    return FALSE;
  }

  hGlobal = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(len + 1));
  if (!hGlobal) {
    pDoc->lpVtbl->Release(pDoc);
    return FALSE;
  }

  {
    void *pDest = GlobalLock(hGlobal);
    if (!pDest) {
      GlobalFree(hGlobal);
      pDoc->lpVtbl->Release(pDoc);
      return FALSE;
    }
    memcpy(pDest, html, (size_t)len);
    ((char*)pDest)[len] = '\0';
    GlobalUnlock(hGlobal);
  }

  /* TRUE：流释放时自动释放 hGlobal */
  hr = CreateStreamOnHGlobal(hGlobal, TRUE, &pStream);
  if (FAILED(hr)) {
    GlobalFree(hGlobal);
    pDoc->lpVtbl->Release(pDoc);
    return FALSE;
  }

  hr = pDoc->lpVtbl->QueryInterface(pDoc, &IID_IPersistStreamInit, (void**)&pPersist);
  DBG("SetHtml: QI(IPersistStreamInit) hr=0x%08lX", (unsigned long)hr);
  if (SUCCEEDED(hr)) {
    hr = pPersist->lpVtbl->InitNew(pPersist);
    DBG("SetHtml: InitNew hr=0x%08lX", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
      hr = pPersist->lpVtbl->Load(pPersist, pStream);
      DBG("SetHtml: Load hr=0x%08lX (%d bytes)", (unsigned long)hr, len);
    }
    pPersist->lpVtbl->Release(pPersist);
  }

  pStream->lpVtbl->Release(pStream);
  pDoc->lpVtbl->Release(pDoc);

  if (SUCCEEDED(hr))
    InstallLinkDelegate();

  return SUCCEEDED(hr);
}


/*
  安装文档级链接点击委托：命中 <a>（且非 # 锚点）时调 window.external.MDOpen，
  拦截默认导航（否则预览控件会自己跳到外站，把预览页面“咝”掉）。

  为什么用 execScript 注入而不是给 <a> 加 onclick 属性：
  渲染测试对链接有严格断言（<a href="…">text</a>），加属性会弄坏它。
  文档级 handler 不受 innerHTML 增量刷新影响（只换子节点，文档对象不变）。
*/
static BOOL InstallLinkDelegate(void)
{
  IWebBrowser2 *pWB;
  IDispatch *pDisp = NULL;
  IHTMLDocument2 *pDoc2 = NULL;
  IHTMLWindow2 *pWin = NULL;
  BSTR bstrCode = NULL, bstrLang = NULL;
  VARIANT vRet;
  BOOL ok = FALSE;

  if (!g_pSite || !g_pSite->pWB)
    return FALSE;

  pWB = g_pSite->pWB;
  if (FAILED(pWB->lpVtbl->get_Document(pWB, &pDisp)) || !pDisp)
    return FALSE;
  if (FAILED(pDisp->lpVtbl->QueryInterface(pDisp, &IID_IHTMLDocument2, (void**)&pDoc2)) || !pDoc2)
    goto done;
  if (FAILED(pDoc2->lpVtbl->get_parentWindow(pDoc2, &pWin)) || !pWin)
    goto done;

  /* 链接外开；不改动 <a> 自身属性 */
  bstrCode = SysAllocString(
    L"document.onclick=function(e){var t=e?e.target:event.srcElement;"
    L"while(t&&t.tagName&&t.tagName.toLowerCase()!='a')t=t.parentNode;"
    L"if(!t||!t.href)return;"
    L"var h=t.getAttribute('href')||'';"
    L"if(h.charAt(0)=='#')return;"
    L"try{window.external.MDOpen(t.href);}catch(x){}"
    L"return false;};");
  bstrLang = SysAllocString(L"JavaScript");
  if (bstrCode && bstrLang) {
    VariantInit(&vRet);
    ok = SUCCEEDED(pWin->lpVtbl->execScript(pWin, bstrCode, bstrLang, &vRet));
    VariantClear(&vRet);
    DBG("InstallLinkDelegate ok=%d", (int)ok);
  }

done:
  if (bstrCode) SysFreeString(bstrCode);
  if (bstrLang) SysFreeString(bstrLang);
  if (pWin)  pWin->lpVtbl->Release(pWin);
  if (pDoc2) pDoc2->lpVtbl->Release(pDoc2);
  if (pDisp) pDisp->lpVtbl->Release(pDisp);
  return ok;
}


IWebBrowser2* Preview_GetBrowser(void)
{
  return g_pSite ? g_pSite->pWB : NULL;
}


/*
  把 UTF-8 片段写进 id="md-content" 容器的 innerHTML，并保持滚动位置。
  只改内容，不重载文档 —— 这是消除预览闪烁的关键。
*/
BOOL Preview_SetContent(HWND hwnd, const char *fragment)
{
  IWebBrowser2 *pWB;
  IDispatch *pDisp = NULL;
  IHTMLDocument3 *pDoc3 = NULL;
  IHTMLElement *pEl = NULL;
  IHTMLElement *pScrollEl = NULL;
  IHTMLElement2 *pScrollEl2 = NULL;
  BSTR bstrId = NULL, bstrHtml = NULL;
  WCHAR *pwch = NULL;
  int len, cch;
  LONG iScrollTop = 0;
  HRESULT hr;
  BOOL ok = FALSE;

  if (!hwnd || !fragment || !g_pSite || !g_pSite->pWB)
    return FALSE;

  pWB = g_pSite->pWB;
  len = (int)strlen(fragment);

  hr = pWB->lpVtbl->get_Document(pWB, &pDisp);
  if (FAILED(hr) || !pDisp)
    return FALSE;

  hr = pDisp->lpVtbl->QueryInterface(pDisp, &IID_IHTMLDocument3, (void**)&pDoc3);
  pDisp->lpVtbl->Release(pDisp);
  if (FAILED(hr) || !pDoc3)
    return FALSE;

  bstrId = SysAllocString(L"md-content");
  if (!bstrId)
    goto done;

  hr = pDoc3->lpVtbl->getElementById(pDoc3, bstrId, &pEl);
  if (FAILED(hr) || !pEl)
    goto done;   /* 外壳还没就绪，调用方会退回去做整页加载 */

  /* 记下当前滚动位置（标准模式下由 documentElement 承载）*/
  if (SUCCEEDED(pDoc3->lpVtbl->get_documentElement(pDoc3, &pScrollEl)) && pScrollEl) {
    if (SUCCEEDED(pScrollEl->lpVtbl->QueryInterface(pScrollEl, &IID_IHTMLElement2,
                                                     (void**)&pScrollEl2)) && pScrollEl2)
      pScrollEl2->lpVtbl->get_scrollTop(pScrollEl2, &iScrollTop);
  }

  /* UTF-8 -> UTF-16 */
  cch = MultiByteToWideChar(CP_UTF8, 0, fragment, len, NULL, 0);
  if (cch < 0)
    goto done;

  pwch = (WCHAR*)malloc(((size_t)cch + 1) * sizeof(WCHAR));
  if (!pwch)
    goto done;
  if (cch > 0)
    MultiByteToWideChar(CP_UTF8, 0, fragment, len, pwch, cch);
  pwch[cch] = L'\0';

  bstrHtml = SysAllocStringLen(pwch, (UINT)cch);
  if (!bstrHtml)
    goto done;

  hr = pEl->lpVtbl->put_innerHTML(pEl, bstrHtml);
  ok = SUCCEEDED(hr);
  DBG("SetContent: put_innerHTML hr=0x%08lX (%d bytes)", (unsigned long)hr, len);

  /* 恢复滚动位置 */
  if (ok && pScrollEl2)
    pScrollEl2->lpVtbl->put_scrollTop(pScrollEl2, iScrollTop);

done:
  if (bstrHtml)  SysFreeString(bstrHtml);
  if (bstrId)    SysFreeString(bstrId);
  if (pwch)      free(pwch);
  if (pScrollEl2) pScrollEl2->lpVtbl->Release(pScrollEl2);
  if (pScrollEl) pScrollEl->lpVtbl->Release(pScrollEl);
  if (pEl)       pEl->lpVtbl->Release(pEl);
  if (pDoc3)     pDoc3->lpVtbl->Release(pDoc3);
  return ok;
}


/*
  把预览滚动到第 idxHeading 个标题（0 基）。编辑器/预览滚动同步用。

  iTotalHeadings 为调用方扫描出的标题数；与 DOM 中实际标题数不符时直接放弃，
  避免滚到错误位置。
*/
BOOL Preview_ScrollToHeading(HWND hwnd, int idxHeading, int iTotalHeadings)
{
  IWebBrowser2 *pWB;
  IDispatch *pDisp = NULL;
  IHTMLDocument2 *pDoc2 = NULL;
  IHTMLWindow2 *pWin = NULL;
  BSTR bstrCode = NULL;
  BSTR bstrLang = NULL;
  WCHAR wchCode[320];
  VARIANT vRet;
  BOOL ok = FALSE;

  if (!hwnd || idxHeading < 0 || iTotalHeadings <= 0 || !g_pSite || !g_pSite->pWB)
    return FALSE;

  pWB = g_pSite->pWB;
  if (FAILED(pWB->lpVtbl->get_Document(pWB, &pDisp)) || !pDisp)
    return FALSE;

  if (FAILED(pDisp->lpVtbl->QueryInterface(pDisp, &IID_IHTMLDocument2, (void**)&pDoc2)) || !pDoc2)
    goto done;

  if (FAILED(pDoc2->lpVtbl->get_parentWindow(pDoc2, &pWin)) || !pWin)
    goto done;

  /* 让目标标题落在视口顶端下方 8px 处，而不是紧贴顶边。
     documentElement / body 两个都赋相同的绝对值：不同文档模式下生效的
     那个不同，另一个会忽略这个赋值。 */
  wsprintfW(wchCode,
    L"(function(){var h=document.querySelectorAll('h1,h2,h3,h4,h5,h6');"
    L"if(h.length!==%d||%d>=h.length)return;"
    L"var t=h[%d].getBoundingClientRect().top;"
    L"var d=document.documentElement,b=document.body;"
    L"var y=(d.scrollTop||b.scrollTop)+t-8;"
    L"if(y<0)y=0;"
    L"d.scrollTop=y;if(b)b.scrollTop=y;})();",
    iTotalHeadings, idxHeading, idxHeading);

  bstrCode = SysAllocString(wchCode);
  bstrLang = SysAllocString(L"JavaScript");
  if (bstrCode && bstrLang) {
    VariantInit(&vRet);
    ok = SUCCEEDED(pWin->lpVtbl->execScript(pWin, bstrCode, bstrLang, &vRet));
    VariantClear(&vRet);
  }

done:
  if (bstrCode) SysFreeString(bstrCode);
  if (bstrLang) SysFreeString(bstrLang);
  if (pWin)  pWin->lpVtbl->Release(pWin);
  if (pDoc2) pDoc2->lpVtbl->Release(pDoc2);
  if (pDisp) pDisp->lpVtbl->Release(pDisp);
  return ok;
}


BOOL Preview_Show(HWND hwnd, const char *html)
{
  return Preview_SetHtml(hwnd, html);
}


BOOL Preview_Clear(HWND hwnd)
{
  return Preview_Show(hwnd, "<html><body></body></html>");
}


void Preview_Shutdown(void)
{
  if (g_pSite) {
    /* 仅对成功激活过的控件调 Close。对未激活就失败的控件调 Close
       可能永久阻塞（实测挂死在 message loop）。 */
    if (bActivated && g_pSite->pOleObj)
      g_pSite->pOleObj->lpVtbl->Close(g_pSite->pOleObj, OLECLOSE_NOSAVE);
    g_pSite->site.lpVtbl->Release(&g_pSite->site);
    bActivated = FALSE;
  }
  if (bComInit) {
    CoUninitialize();
    bComInit = FALSE;
  }
  bClassRegistered = FALSE;
}
