/*===========================================================================

  MDPreView — 预览控件模块
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
  fp = fopen("C:\\Project\\MDPreView\\preview_debug.log", "a");
  if (fp) { fprintf(fp, "%s\n", buf); fclose(fp); }
}
#define DBG(...) dbg(__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif



/* ---- 常量 -------------------------------------------------------------- */

#define PREVIEW_CLASS  TEXT("MDPreViewHost")


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


/* ---- 宿主站点实现 ------------------------------------------------------ */

typedef struct tagHostSite {
  IOleInPlaceSite      site;      /* 必须第一个成员：vtable 布局兼容 */
  IOleClientSite       client;    /* 独立 vtable，单独计数 */
  IOleInPlaceFrame     frame;
  IOleInPlaceUIWindow  uiw;
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
    *ppv = &s->uiw;
    DBG("  -> matched IOleInPlaceUIWindow, returning uiw");
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

  *ppFrame = &s->frame;
  *ppDoc   = &s->uiw;           /* 文档层站点：返回真接口，不能是 NULL */

  s->frame.lpVtbl->AddRef(&s->frame);
  s->uiw.lpVtbl->AddRef(&s->uiw);

  *prcPosRect  = s->rc;
  *prcClipRect = s->rc;

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


/* ---- IOleInPlaceUIWindow ----------------------------------------------- */
/* 布局：IUnknown(3) + IOleWindow(2) + GetBorder/RequestBorderSpace/
   SetBorderSpace(3)。与 IOleInPlaceFrame 不同，无菜单/状态栏方法。 */

static HRESULT STDMETHODCALLTYPE UIW_QueryInterface(IOleInPlaceUIWindow *This,
                                                      REFIID riid, void **ppv)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, uiw));
  return Site_QueryInterface(&s->site, riid, ppv);
}

static ULONG STDMETHODCALLTYPE UIW_AddRef(IOleInPlaceUIWindow *This)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, uiw));
  return (ULONG)InterlockedIncrement(&s->refs);
}

static ULONG STDMETHODCALLTYPE UIW_Release(IOleInPlaceUIWindow *This)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, uiw));
  return Site_Release(&s->site);
}

static HRESULT STDMETHODCALLTYPE UIW_GetWindow(IOleInPlaceUIWindow *This, HWND *phwnd)
{
  HostSite *s = (HostSite*)((char*)This - offsetof(HostSite, uiw));
  if (!phwnd) return E_POINTER;
  *phwnd = s->hwndHost;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE UIW_ContextSensitiveHelp(IOleInPlaceUIWindow *This, BOOL f)
{
  (void)This; (void)f;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE UIW_GetBorder(IOleInPlaceUIWindow *This, LPRECT prc)
{
  (void)This; (void)prc;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE UIW_RequestBorderSpace(
    IOleInPlaceUIWindow *This, LPCBORDERWIDTHS pbw)
{
  (void)This; (void)pbw;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE UIW_SetBorderSpace(
    IOleInPlaceUIWindow *This, LPCBORDERWIDTHS pbw)
{
  (void)This; (void)pbw;
  return E_NOTIMPL;
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

static IOleInPlaceUIWindowVtbl g_vtblUIW = {
  UIW_QueryInterface,
  UIW_AddRef,
  UIW_Release,
  UIW_GetWindow,
  UIW_ContextSensitiveHelp,
  UIW_GetBorder,
  UIW_RequestBorderSpace,
  UIW_SetBorderSpace
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
  s->uiw.lpVtbl     = &g_vtblUIW;
  s->refs           = 1;
  s->hwndHost       = hwndHost;
  GetClientRect(hwndHost, &s->rc);

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
  pOle->lpVtbl->SetHostNames(pOle, L"MDPreView", L"Preview");

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

  return SUCCEEDED(hr);
}


IWebBrowser2* Preview_GetBrowser(void)
{
  return g_pSite ? g_pSite->pWB : NULL;
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
