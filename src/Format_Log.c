//=============================================================================
//
//  Format_Log.c — 纯文本 / .log 文件的格式化与压缩
//
//  日志没有统一的语法：时间戳、级别、堆栈、JSON 片段、多行异常可以任意混排。
//  任何"按字段重排"的尝试都是在猜，猜错就把数据改坏了。所以这里只做
//  唯一一件对所有日志都安全的事 —— **整理空白**：
//
//    · 去掉每行行尾空白（含尾部 \t 与全角空格之外的一切 ASCII 空白）
//    · 连续空行压成一个空行
//    · 文件开头不留空行
//    · 文件结尾恰好一个行尾（沿用输入的行尾风格）
//    · 行首缩进原样保留 —— 缩进的堆栈信息是有意义的
//
//  压缩：把空行全部丢掉，只保留非空行（行结构不动，因为换行是日志的骨架）。
//
//  由此两条硬不变量仍然成立：
//    minify(format(x)) == minify(x)     —— 两侧都只剩"去尾空白 + 去空行"后的行序列
//    format(format(x)) == format(x)     —— 再整理一遍不会再有变化
//
//=============================================================================

#include <stdlib.h>
#include <string.h>

#include "Format_Int.h"

typedef struct {
  const char *p;
  int cb;
} LINE;

/* 去掉行尾空白，返回有效内容长度（行本身不含 \r\n） */
static int logTrimEnd(const char *p, int cb) {

  while (cb > 0) {
    char c = p[cb - 1];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f')
      cb--;
    else
      break;
  }

  return cb;
}

FMT_BOOL FmtLog_Do(FMTC *pC, int iFlags, FMTERR *pErr) {

  LINE *pLines = NULL;
  int nLines = 0, nMax = 0;
  int i, iStart;
  int bMinify = (iFlags & FMT_F_MINIFY) ? 1 : 0;

  (void)pErr;

  /* --- 切行（与 Format_Ini.c 同一套：\n 断行，剥掉行尾的 \r）--- */
  iStart = 0;
  if (pC->cbIn >= 3 && (unsigned char)pC->pIn[0] == 0xEF &&
                       (unsigned char)pC->pIn[1] == 0xBB &&
                       (unsigned char)pC->pIn[2] == 0xBF)
    iStart = 3;

  for (i = iStart; ; i++) {

    int bEnd = (i >= pC->cbIn);

    if (bEnd || pC->pIn[i] == '\n') {

      int iEnd = i;
      if (iEnd > iStart && pC->pIn[iEnd-1] == '\r') iEnd--;

      if (nLines >= nMax) {
        int nNew = (nMax > 0) ? nMax * 2 : 256;
        LINE *pNew = (LINE *)realloc(pLines, (size_t)nNew * sizeof(LINE));
        if (!pNew) { free(pLines); Fmt_SetErr(pC, FMT_E_NOMEM, 0); return 0; }
        pLines = pNew;
        nMax = nNew;
      }

      pLines[nLines].p  = pC->pIn + iStart;
      pLines[nLines].cb = iEnd - iStart;
      nLines++;

      iStart = i + 1;
      if (bEnd) break;
    }
  }

  if (nLines == 0) {
    free(pLines);
    Fmt_SetErr(pC, FMT_E_EMPTY, 0);
    return 0;
  }

  /* --- 输出 --- */
  {
    int bPrevBlank = 1;      /* 开头不留空行 */
    int bWroteAny = 0;       /* 有没有写出过内容 */

    for (i = 0; i < nLines; i++) {

      const char *pLine = pLines[i].p;
      int cbTrim = logTrimEnd(pLine, pLines[i].cb);

      /* 空行（压缩时整行丢掉）*/
      if (cbTrim == 0) {
        if (!bMinify && !bPrevBlank) {
          Fmt_ForceNewline(pC);
          bPrevBlank = 1;
        }
        continue;
      }

      Fmt_Tok(pC, 0, pLine, cbTrim);
      Fmt_ForceNewline(pC);
      bPrevBlank = 0;
      bWroteAny = 1;
    }

    /* 全空白的文档：留下一个空行。否则输出 0 字节，再格式化一次会被当成
       空文档报错，幂等性就破了。 */
    if (!bWroteAny && !bMinify)
      Fmt_ForceNewline(pC);
  }

  free(pLines);
  return 1;
}
