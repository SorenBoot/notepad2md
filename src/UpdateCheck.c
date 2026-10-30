//=============================================================================
//
//  UpdateCheck.c
//
//  纯逻辑，零 Win32 依赖 —— 这样 tests/run/test_update.c 能直接编进去跑。
//
#include <stdlib.h>
#include <string.h>

#include "UpdateCheck.h"

//=============================================================================
//
//  NextUtf8()
//
//  pszOut 还剩 cchOut 字节空间时写入一个 Unicode 码点，返回写入字节数。
//
static int NextUtf8(unsigned int uChar,char *pszOut,int cchOut)
{
  if (uChar < 0x80) {
    if (cchOut < 1) return 0;
    pszOut[0] = (char)uChar;
    return 1;
  }
  if (uChar < 0x800) {
    if (cchOut < 2) return 0;
    pszOut[0] = (char)(0xC0 | (uChar >> 6));
    pszOut[1] = (char)(0x80 | (uChar & 0x3F));
    return 2;
  }
  if (uChar < 0x10000) {
    if (cchOut < 3) return 0;
    pszOut[0] = (char)(0xE0 | (uChar >> 12));
    pszOut[1] = (char)(0x80 | ((uChar >> 6) & 0x3F));
    pszOut[2] = (char)(0x80 | (uChar & 0x3F));
    return 3;
  }
  if (cchOut < 4) return 0;
  pszOut[0] = (char)(0xF0 | (uChar >> 18));
  pszOut[1] = (char)(0x80 | ((uChar >> 12) & 0x3F));
  pszOut[2] = (char)(0x80 | ((uChar >> 6) & 0x3F));
  pszOut[3] = (char)(0x80 | (uChar & 0x3F));
  return 4;
}

static int ReadHex4(const char *p)
{
  int i, v = 0;
  for (i = 0; i < 4; i++) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9')      v += c - '0';
    else if (c >= 'a' && c <= 'f') v += c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v += c - 'A' + 10;
    else return -1;
  }
  return v;
}

//=============================================================================
//
//  UpdateExtractJsonString()
//
//  不做完整 JSON 解析，只找 "键": "值"。GitHub 的 release 对象里
//  tag_name / body 都只出现一次，够用。
//
int UpdateExtractJsonString(const char *pszJson,int cbJson,
                            const char *pszKey,char *pszOut,int cchOut)
{
  const char *p,*pEnd,*pFound = NULL;
  int cbKey,iOut = 0;

  if (!pszJson || !pszKey || !pszOut || cchOut < 2)
    return 0;

  pszOut[0] = '\0';
  cbKey = (int)strlen(pszKey);
  p = pszJson;
  pEnd = pszJson + cbJson;

  // 找 "键"（连同两侧引号一起匹配，避免撞到 body 里的同名文字）
  while (p + cbKey + 2 <= pEnd) {
    if (*p == '"' && p[cbKey + 1] == '"' && memcmp(p + 1,pszKey,cbKey) == 0) {
      pFound = p + cbKey + 2;
      break;
    }
    p++;
  }
  if (!pFound)
    return 0;

  // 跳过空白和冒号
  p = pFound;
  while (p < pEnd && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  if (p >= pEnd || *p != ':')
    return 0;
  p++;
  while (p < pEnd && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;

  // null（body 可能为 null）当成空串
  if (pEnd - p >= 4 && memcmp(p,"null",4) == 0)
    return 1;

  if (p >= pEnd || *p != '"')
    return 0;
  p++;

  while (p < pEnd && *p != '"') {
    unsigned int uChar;
    int n;

    if (cchOut - iOut < 5)
      break;

    if (*p != '\\') {
      pszOut[iOut++] = *p++;
      continue;
    }

    p++;
    if (p >= pEnd)
      break;

    switch (*p) {
      case 'n': pszOut[iOut++] = '\n'; p++; break;
      case 'r': pszOut[iOut++] = '\r'; p++; break;
      case 't': pszOut[iOut++] = '\t'; p++; break;
      case 'b': pszOut[iOut++] = '\b'; p++; break;
      case 'f': pszOut[iOut++] = '\f'; p++; break;
      case '"': pszOut[iOut++] = '"';  p++; break;
      case '\\':pszOut[iOut++] = '\\'; p++; break;
      case '/': pszOut[iOut++] = '/';  p++; break;
      case 'u':
        if (pEnd - p < 5) { p = pEnd; break; }
        n = ReadHex4(p + 1);
        if (n < 0) { pszOut[iOut++] = 'u'; p++; break; }
        uChar = (unsigned int)n;
        p += 5;
        // UTF-16 代理对拼成一个码点
        if (uChar >= 0xD800 && uChar <= 0xDBFF &&
            pEnd - p >= 6 && p[0] == '\\' && p[1] == 'u') {
          int nLow = ReadHex4(p + 2);
          if (nLow >= 0xDC00 && nLow <= 0xDFFF) {
            uChar = 0x10000 + ((uChar - 0xD800) << 10) + ((unsigned int)nLow - 0xDC00);
            p += 6;
          }
        }
        iOut += NextUtf8(uChar,pszOut + iOut,cchOut - iOut);
        break;
      default:
        // 未知转义：原样保留反斜杠后的字符
        pszOut[iOut++] = *p++;
        break;
    }
  }

  pszOut[iOut] = '\0';
  return 1;
}

//=============================================================================
//
//  UpdateVersionCmp()
//
int UpdateVersionCmp(const char *pszA,const char *pszB)
{
  if (!pszA || !pszB)
    return 0;

  // 跳过 "v" 之类的前缀
  while (*pszA && (*pszA < '0' || *pszA > '9')) pszA++;
  while (*pszB && (*pszB < '0' || *pszB > '9')) pszB++;

  for (;;) {
    long lA = 0,lB = 0;

    while (*pszA >= '0' && *pszA <= '9') { lA = lA * 10 + (*pszA - '0'); pszA++; }
    while (*pszB >= '0' && *pszB <= '9') { lB = lB * 10 + (*pszB - '0'); pszB++; }

    if (lA != lB)
      return (lA < lB) ? -1 : 1;

    // 跳过 "." 等分隔符，进入下一段
    while (*pszA && (*pszA < '0' || *pszA > '9')) pszA++;
    while (*pszB && (*pszB < '0' || *pszB > '9')) pszB++;

    if (!*pszA && !*pszB)
      return 0;
  }
}

//=============================================================================
//
//  UpdateIsNewer()
//
int UpdateIsNewer(const char *pszLocal,const char *pszRemote)
{
  if (!pszLocal || !pszRemote || !*pszLocal || !*pszRemote)
    return 0;
  return (UpdateVersionCmp(pszRemote,pszLocal) > 0) ? 1 : 0;
}

//=============================================================================
//
//  UpdateFormatNotes()
//
//  发布说明是我们自己写的 Markdown。弹窗里不需要排版，只求可读：
//  去掉 ** / ` / # 这些记号，空行压一行，多余行截断加 "..."。
//
int UpdateFormatNotes(const char *pszBody,char *pszOut,int cchOut,int iMaxLines)
{
  int iOut = 0,iLines = 0;
  const char *p;
  int bPendingBlank = 0,bTruncated = 0;

  if (!pszOut || cchOut < 2)
    return 0;

  pszOut[0] = '\0';
  if (!pszBody || !*pszBody || iMaxLines <= 0)
    return 0;

  p = pszBody;
  while (*p) {
    const char *pLineEnd;
    int cchTab;
    char szLine[512];
    int iLine = 0;

    // 取一行
    pLineEnd = p;
    while (*pLineEnd && *pLineEnd != '\n') pLineEnd++;

    // 去行首的 # 标题记号
    while (*p == '#') p++;
    while (*p == ' ' || *p == '\t') p++;

    // 逐字符复制，跳过 ** 和 `
    for (; p < pLineEnd; p++) {
      if (*p == '*' && p + 1 < pLineEnd && p[1] == '*') { p++; continue; }
      if (*p == '`') continue;
      if (*p == '\r') continue;
      if (iLine < (int)sizeof(szLine) - 1)
        szLine[iLine++] = *p;
    }

    // 去行尾空格
    while (iLine > 0 && (szLine[iLine - 1] == ' ' || szLine[iLine - 1] == '\t'))
      iLine--;
    szLine[iLine] = '\0';

    if (iLine == 0) {
      // 空行：只记一笔，等真要输出下一行时补一个空行（连续空行不叠加）
      if (iLines > 0)
        bPendingBlank = 1;
    }
    else {
      if (iLines >= iMaxLines) { bTruncated = 1; break; }
      cchTab = (int)strlen(szLine);
      if (cchOut - iOut < cchTab + 5) { bTruncated = 1; break; }
      if (iLines > 0) {
        pszOut[iOut++] = '\n';
        if (bPendingBlank)
          pszOut[iOut++] = '\n';
      }
      memcpy(pszOut + iOut,szLine,cchTab);
      iOut += cchTab;
      iLines++;
      bPendingBlank = 0;
    }

    p = pLineEnd;
    if (*p == '\n') p++;
  }

  pszOut[iOut] = '\0';

  if (bTruncated) {
    if (cchOut - iOut >= 5) {
      memcpy(pszOut + iOut," ...",4);
      iOut += 4;
      pszOut[iOut] = '\0';
    }
  }

  return iLines;
}

//=============================================================================
//
//  UpdateExtractTagFromUrl()
//
//  从 .../releases/tag/v0.0.2 里取最后一段。比走 API 稳：
//  GitHub 的 API 对匿名请求限流（60 次/小时/IP），而发布页重定向不限。
//
int UpdateExtractTagFromUrl(const char *pszUrl,char *pszOut,int cchOut)
{
  const char *p,*pTag = NULL,*pEnd;
  int i = 0;

  if (!pszOut || cchOut < 2)
    return 0;
  pszOut[0] = '\0';
  if (!pszUrl || !*pszUrl)
    return 0;

  for (p = pszUrl; *p; p++) {
    if (p[0] == '/' && p[1] == 't' && p[2] == 'a' && p[3] == 'g' && p[4] == '/')
      pTag = p + 5;
  }
  if (!pTag || !*pTag)
    return 0;

  pEnd = pTag;
  while (*pEnd && *pEnd != '/' && *pEnd != '?' && *pEnd != '#')
    pEnd++;

  while (pTag < pEnd && i < cchOut - 1)
    pszOut[i++] = *pTag++;
  pszOut[i] = '\0';
  return (i > 0) ? 1 : 0;
}

// 大小写无关比较，比较前 n 个字符（n == -1 时比到结尾）
static int StrcmpNIA(const char *pszA,const char *pszB,int n)
{
  int i = 0;
  if (!pszA || !pszB)
    return pszA == pszB ? 0 : 1;
  for (;;) {
    unsigned char a = (unsigned char)pszA[i],b = (unsigned char)pszB[i];
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
    if (n >= 0 && i >= n)
      return 0;
    if (a != b)
      return (int)a - (int)b;
    if (a == 0)
      return 0;
    i++;
  }
}

static int Strcasecmp(const char *pszA,const char *pszB)
{
  return StrcmpNIA(pszA,pszB,-1);
}

//=============================================================================
//
//  UpdateNormalizeProxy()
//
//  把各种写法的代理转成 WinINet 认的格式：
//    http://127.0.0.1:10809/  ->  http://127.0.0.1:10809
//    127.0.0.1:10809          ->  http://127.0.0.1:10809
//    socks5://127.0.0.1:1080  ->  socks=127.0.0.1:1080
//    socks://...              ->  socks=...
//  返回 1 = 拿到可用代理；0 = 空值/不认得的写法。
//
int UpdateNormalizeProxy(const char *pszRaw,char *pszOut,int cchOut)
{
  const char *p;
  int i = 0,bSocks = 0;

  if (!pszOut || cchOut < 2)
    return 0;
  pszOut[0] = '\0';
  if (!pszRaw)
    return 0;

  p = pszRaw;
  while (*p == ' ' || *p == '\t')
    p++;

  if (0 == StrcmpNIA(p,"http://",7))
    p += 7;
  else if (0 == StrcmpNIA(p,"https://",8))
    p += 8;
  else if (0 == StrcmpNIA(p,"socks5h://",10)) {
    p += 10;
    bSocks = 1;
  }
  else if (0 == StrcmpNIA(p,"socks5://",9)) {
    p += 9;
    bSocks = 1;
  }
  else if (0 == StrcmpNIA(p,"socks://",8)) {
    p += 8;
    bSocks = 1;
  }

  {
    // WinINet 认 URL 形式：http://host:port 对 http 与 https 都适用；
    // socks 需要 socks=host:port。
    const char *pfx = bSocks ? "socks=" : "http://";
    while (*pfx && i < cchOut - 1)
      pszOut[i++] = *pfx++;
  }

  // 去掉尾部斜杠和空白；中间出现空白就认为写坏了
  {
    const char *pEnd = p + (int)strlen(p);
    while (pEnd > p && (pEnd[-1] == '/' || pEnd[-1] == ' ' || pEnd[-1] == '\t'))
      pEnd--;
    if (pEnd == p)
      return 0;
    while (p < pEnd) {
      if (*p == ' ' || *p == '\t') {
        pszOut[0] = '\0';
        return 0;
      }
      if (i >= cchOut - 1)
        break;
      pszOut[i++] = *p++;
    }
  }

  pszOut[i] = '\0';
  return (i > 0) ? 1 : 0;
}

//=============================================================================
//
//  UpdateHostBypassed()
//
//  NO_PROXY 的简易匹配：逗号分隔，支持 * 、精确域名、.后缀 / 后缀、
//  以及 host:port（只比 host 部分）。localhost / 127.0.0.1 永远直连。
//
int UpdateHostBypassed(const char *pszHost,const char *pszNoProxy)
{
  const char *p;
  int cchHost;

  if (!pszHost || !*pszHost)
    return 0;

  // 本机地址不必过代理
  if (0 == StrcmpNIA(pszHost,"localhost",9) && (pszHost[9] == '\0' || pszHost[9] == ':'))
    return 1;
  if (0 == StrcmpNIA(pszHost,"127.0.0.1",9) && (pszHost[9] == '\0' || pszHost[9] == ':'))
    return 1;
  if (0 == StrcmpNIA(pszHost,"::1",3) && pszHost[3] == '\0')
    return 1;

  if (!pszNoProxy || !*pszNoProxy)
    return 0;

  cchHost = (int)strlen(pszHost);
  p = pszNoProxy;
  while (*p) {
    const char *pStart,*pEnd;
    int cchEntry;
    char szEntry[128];
    int i;

    while (*p == ' ' || *p == '\t' || *p == ',')
      p++;
    if (!*p)
      break;
    pStart = p;
    while (*p && *p != ',')
      p++;
    pEnd = p;
    while (pEnd > pStart && (pEnd[-1] == ' ' || pEnd[-1] == '\t'))
      pEnd--;

    cchEntry = (int)(pEnd - pStart);
    if (cchEntry <= 0 || cchEntry >= (int)sizeof(szEntry))
      continue;
    for (i = 0; i < cchEntry; i++)
      szEntry[i] = pStart[i];
    szEntry[i] = '\0';

    if (0 == Strcasecmp(szEntry,"*"))
      return 1;

    // 去掉 host:port 里的端口
    {
      char *pColon = szEntry;
      while (*pColon && *pColon != ':')
        pColon++;
      *pColon = '\0';
    }
    if (!szEntry[0])
      continue;

    // 去掉前导点：.example.com 视作 example.com 及其子域
    {
      const char *pDom = szEntry;
      int cchDom;
      if (*pDom == '.')
        pDom++;
      cchDom = (int)strlen(pDom);
      if (cchDom <= 0)
        continue;
      if (cchDom == cchHost && 0 == StrcmpNIA(pDom,pszHost,cchDom))
        return 1;
      // 后缀匹配：host 必须以 "."+domain 结尾
      if (cchHost > cchDom &&
          0 == StrcmpNIA(pszHost + cchHost - cchDom,pDom,cchDom) &&
          pszHost[cchHost - cchDom - 1] == '.')
        return 1;
    }
  }

  return 0;
}
