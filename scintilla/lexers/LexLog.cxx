// Scintilla source code edit control
/** @file LexLog.cxx
 ** Lexer for log files (server / application / access logs).
 **
 ** 日志没有统一格式，所以这个 lexer 只做"识别得到就涂、识别不出就留白"，
 ** 绝不改字节、也绝不影响编辑。按行扫描，每行按下面的顺序试探：
 **
 **   1) 日期时间   2024-01-01 10:00:00 / 2024-01-01T10:00:00.123Z /
 **                 [2024/01/01 10:00:00] / Jan 01 10:00:00,123
 **   2) 日志级别   INFO / WARN / ERROR / DEBUG / TRACE / FATAL / NOTICE，
 **                 大小写不敏感，可带 [ ] ( ) < > 包裹，也认缩写 inf / inf /
 **                 dbg / trc / wrn / err / ftl —— 只要整词独立就算。
 **                 按严重程度分成四档配色，一眼能挑出错误。
 **   3) 行内 token 注释（// 与 #）、引号字符串、数字与 IP/端口、
 **                 key=value 的键、文件路径与 (Foo.java:42) 这类位置。
 **
 ** 缩进与内容一律保留原样 —— 堆栈的缩进分层是可读性的一部分。
 **/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <ctype.h>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
#include "CharacterSet.h"
#include "LexerModule.h"

#ifdef SCI_NAMESPACE
using namespace Scintilla;
#endif

namespace {

/* ------------------------------------------------------------------ 工具 */

inline bool IsLogBlank(char ch) {
  return ch == ' ' || ch == '\t';
}

inline bool IsLogWordChar(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '_';
}

inline char LogLower(char ch) {
  return (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
}

/* 从 i 起、长度 cb 的串是否等于 psz（大小写不敏感） */
bool LogEqI(Accessor &styler, unsigned int i, const char *psz) {
  unsigned int n = 0;
  while (psz[n]) {
    char c = styler.SafeGetCharAt((int)(i + n));
    if (LogLower(c) != LogLower(psz[n]))
      return false;
    n++;
  }
  /* 后一个字符不能还是词字符，否则 "infx" 会被当成 "inf" */
  return !IsLogWordChar(styler.SafeGetCharAt((int)(i + n)));
}

/* 级别 -> 样式。返回 0 表示这个串不是级别 */
int LogLevelStyle(Accessor &styler, unsigned int i) {

  /* 严重档 */
  if (LogEqI(styler, i, "fatal") || LogEqI(styler, i, "ftl") ||
      LogEqI(styler, i, "critical") || LogEqI(styler, i, "crit") ||
      LogEqI(styler, i, "severe") || LogEqI(styler, i, "emergency") ||
      LogEqI(styler, i, "alert") || LogEqI(styler, i, "emerg"))
    return SCE_LOG_LEVEL_FATAL;

  if (LogEqI(styler, i, "error") || LogEqI(styler, i, "err") ||
      LogEqI(styler, i, "exception") || LogEqI(styler, i, "exc") ||
      LogEqI(styler, i, "failed") || LogEqI(styler, i, "fail"))
    return SCE_LOG_LEVEL_ERROR;

  if (LogEqI(styler, i, "warn") || LogEqI(styler, i, "warning") ||
      LogEqI(styler, i, "wrn"))
    return SCE_LOG_LEVEL_WARN;

  if (LogEqI(styler, i, "info") || LogEqI(styler, i, "inf") ||
      LogEqI(styler, i, "information") || LogEqI(styler, i, "notice") ||
      LogEqI(styler, i, "log") || LogEqI(styler, i, "stdout"))
    return SCE_LOG_LEVEL_INFO;

  if (LogEqI(styler, i, "debug") || LogEqI(styler, i, "dbg") ||
      LogEqI(styler, i, "trace") || LogEqI(styler, i, "trc") ||
      LogEqI(styler, i, "verbose") || LogEqI(styler, i, "vrb") ||
      LogEqI(styler, i, "fine") || LogEqI(styler, i, "config"))
    return SCE_LOG_LEVEL_DEBUG;

  return 0;
}

/* 级别两侧的包裹符：'[' ']' '(' ')' '<' '>' */
bool LogIsLevelWrap(char ch) {
  return ch == '[' || ch == ']' || ch == '(' || ch == ')' ||
         ch == '<' || ch == '>';
}

/* 在 i 处（已确认是级别词）涂掉整个词，返回词的结束位置（不含） */
unsigned int LogColourLevel(Accessor &styler, unsigned int i,
                            unsigned int endPos) {
  unsigned int j = i;
  while (j < endPos && IsLogWordChar(styler.SafeGetCharAt(j)))
    j++;
  styler.ColourTo((int)(j - 1), SCE_LOG_LEVEL_ERROR);   /* 先涂，后面再细分 */
  return j;
}

/* ------------------------------------------------------------------ 日期时间 */

/* 数字序列（长度 1..4） */
unsigned int LogDigits(Accessor &styler, unsigned int i,
                       unsigned int endPos) {
  unsigned int j = i;
  while (j < endPos && styler.SafeGetCharAt(j) >= '0' && styler.SafeGetCharAt(j) <= '9')
    j++;
  return j - i;
}

/* 尝试从 i 开始匹配一个日期或时间；匹配到则返回结束位置，否则返回 i */
unsigned int LogTryDateTime(Accessor &styler, unsigned int i,
                            unsigned int endPos) {

  /* 2024-01-01 / 2024/01/01 / 2024.01.01 */
  if (LogDigits(styler, i, endPos) == 4) {
    unsigned int j = i + 4;
    if (j < endPos && (styler.SafeGetCharAt(j) == '-' || styler.SafeGetCharAt(j) == '/' ||
                       styler.SafeGetCharAt(j) == '.')) {
      unsigned int k = j + 1;
      unsigned int n2 = LogDigits(styler, k, endPos);
      if (n2 >= 1 && n2 <= 2) {
        unsigned int m = k + n2;
        if (m < endPos && styler.SafeGetCharAt(m) == styler.SafeGetCharAt(j)) {
          unsigned int p = m + 1;
          unsigned int n3 = LogDigits(styler, p, endPos);
          if (n3 >= 1 && n3 <= 2)
            return p + n3;
        }
      }
    }
    return i;
  }

  /* 纯时间 10:00:00 / 10:00（小时 1..2 位，后面必须跟冒号）。
     这一段必须排在两位日期之前：否则 "01-01" 的规则会先把
     "10:00:00" 的 "10" 当成日期吃掉，时间就没人认了。 */
  {
    unsigned int n1 = LogDigits(styler, i, endPos);
    if (n1 == 1 || n1 == 2) {
      unsigned int j = i + n1;
      if (j < endPos && styler.SafeGetCharAt(j) == ':') {
        unsigned int k = j + 1;
        unsigned int n2 = LogDigits(styler, k, endPos);
        if (n2 == 2) {
          unsigned int m = k + n2;
          if (m < endPos && styler.SafeGetCharAt(m) == ':') {
            unsigned int p = m + 1;
            unsigned int n3 = LogDigits(styler, p, endPos);
            if (n3 == 2)
              return p + n3;
          }
          return m;      /* 10:00 */
        }
      }
    }
  }

  /* 01-01 / 01/01（日期缺年份）*/
  if (LogDigits(styler, i, endPos) == 2) {
    unsigned int j = i + 2;
    if (j < endPos && (styler.SafeGetCharAt(j) == '-' || styler.SafeGetCharAt(j) == '/')) {
      unsigned int k = j + 1;
      unsigned int n2 = LogDigits(styler, k, endPos);
      if (n2 == 2)
        return k + n2;
    }
    return i;
  }

  /* Jan 01 / Mon Jan 01 之类：三个字母的月份缩写 */
  {
    static const char *const aMonths[] = {
      "jan", "feb", "mar", "apr", "may", "jun",
      "jul", "aug", "sep", "oct", "nov", "dec"
    };
    if (i + 3 <= endPos) {
      char c0 = LogLower(styler.SafeGetCharAt(i));
      char c1 = LogLower(styler.SafeGetCharAt(i + 1));
      char c2 = LogLower(styler.SafeGetCharAt(i + 2));
      for (int k = 0; k < 12; k++) {
        if (c0 == aMonths[k][0] && c1 == aMonths[k][1] && c2 == aMonths[k][2]) {
          unsigned int j = i + 3;
          /* 后面跟空白 + 1..2 位数字才算（否则 "Mar" 只是个人名/词）*/
          if (j < endPos && styler.SafeGetCharAt(j) == ' ') {
            unsigned int p = j + 1;
            unsigned int n = LogDigits(styler, p, endPos);
            if (n == 1 || n == 2)
              return p + n;
          }
          return i;
        }
      }
    }
  }

  return i;
}

/* ------------------------------------------------------------------ 主循环 */

void ColouriseLogDoc(unsigned int startPos, int length, int initStyle,
                     WordList *[], Accessor &styler) {

  unsigned int endPos = (unsigned int)(startPos + length);

  unsigned int i = startPos;

  /* 必须显式把涂色起点对齐到本次着色的起始位置。
     LexAccessor 跨调用复用 startSeg：不重置的话第一次 ColourTo 会从
     上一次遗留的 startSeg 算长度，多出来的字节会写穿文档末尾，
     Scintilla 直接断言 [endStyled < Length()]。 */
  if (length <= 0)
    return;
  styler.StartAt(startPos);
  styler.StartSegment(startPos);

  /* 从上一次着色的行内继续时，先把残留的样式收尾 */
  if (initStyle != SCE_LOG_DEFAULT) {
    unsigned int j = i;
    while (j < endPos && styler.SafeGetCharAt(j) != '\n' && styler.SafeGetCharAt(j) != '\r')
      j++;
    styler.ColourTo((int)(j - 1), SCE_LOG_DEFAULT);
    i = j;
  }

  while (i < endPos) {

    /* --- 行首：先找日期，再找级别 --- */
    unsigned int iLine = i;
    unsigned int k = i;

    while (k < endPos && IsLogBlank(styler.SafeGetCharAt(k)))
      k++;

    /* 可选的包裹符 [ ( < 让日期也能被认出来 */
    unsigned int kBody = k;
    if (kBody < endPos && (styler.SafeGetCharAt(kBody) == '[' || styler.SafeGetCharAt(kBody) == '(' ||
                           styler.SafeGetCharAt(kBody) == '<'))
      kBody++;

    unsigned int afterDate = LogTryDateTime(styler, kBody, endPos);

    if (afterDate > kBody) {
      unsigned int j = afterDate;

      /* 毫秒 .123 / ,123 */
      if (j < endPos && (styler.SafeGetCharAt(j) == '.' || styler.SafeGetCharAt(j) == ',')) {
        unsigned int p = j + 1;
        unsigned int n = LogDigits(styler, p, endPos);
        if (n >= 1 && n <= 9)
          j = p + n;
      }

      /* 日期与时间之间的分隔：T / t / Z / z / 空格。
         只有当后面真的还跟着内容时才吃进去 —— 否则 "2024-01-01 "
         行尾那个空格会被算进日期，日期色就多出一格。 */
      if (j < endPos) {
        char c = styler.SafeGetCharAt(j);
        unsigned int p = j + 1;
        if ((c == 'T' || c == 't' || c == 'Z' || c == 'z' || c == ' ')) {
          if (c == 'T' || c == 't' || c == 'Z' || c == 'z') {
            /* ISO：T 后面必须跟数字（时间）才算 */
            if (p < endPos && styler.SafeGetCharAt(p) >= '0' &&
                                styler.SafeGetCharAt(p) <= '9')
              j = p;
          } else {
            /* 空格：后面必须跟时间 */
            unsigned int afterTime = LogTryDateTime(styler, p, endPos);
            if (afterTime > p)
              j = p;
          }
        }
      }

      /* 10:00:00 跟在日期后面 */
      {
        unsigned int afterTime = LogTryDateTime(styler, j, endPos);
        if (afterTime > j)
          j = afterTime;
      }

      /* 时间后面的毫秒 .123 / ,123（logback 的 "10:00:00,123"） */
      if (j < endPos && (styler.SafeGetCharAt(j) == '.' || styler.SafeGetCharAt(j) == ',')) {
        unsigned int p = j + 1;
        unsigned int n = LogDigits(styler, p, endPos);
        if (n >= 1 && n <= 9)
          j = p + n;
      }
      /* 时区 +08:00 / +0800 */
      if (j < endPos && (styler.SafeGetCharAt(j) == '+' || styler.SafeGetCharAt(j) == '-')) {
        unsigned int p = j + 1;
        unsigned int n = LogDigits(styler, p, endPos);
        if (n == 2 || n == 4)
          j = p + n;
        else if (n == 1) {
          unsigned int q = p + 1;
          if (q < endPos && styler.SafeGetCharAt(q) == ':') {
            unsigned int r = q + 1;
            unsigned int n2 = LogDigits(styler, r, endPos);
            if (n2 == 2)
              j = r + n2;
          }
        }
      }
      if (j < endPos && styler.SafeGetCharAt(j) == ']')
        j++;

      styler.ColourTo((int)(k - 1), SCE_LOG_DEFAULT);
      styler.ColourTo((int)(j - 1), SCE_LOG_DATE);
      i = j;
      k = i;
      while (k < endPos && IsLogBlank(styler.SafeGetCharAt(k)))
        k++;
    }

    /* 级别：可带包裹符 */
    if (k < endPos) {
      unsigned int kWord = k;
      if (kWord < endPos && LogIsLevelWrap(styler.SafeGetCharAt(kWord)))
        kWord++;

      int iLv = LogLevelStyle(styler, kWord);
      if (iLv) {
        unsigned int j = kWord;
        while (j < endPos && IsLogWordChar(styler.SafeGetCharAt(j)))
          j++;
        /* 包裹符必须成对，否则只是普通文本里的括号 */
        if (kWord > k) {
          char chClose = (styler.SafeGetCharAt(k) == '[') ? ']' :
                         (styler.SafeGetCharAt(k) == '(') ? ')' : '>';
          if (j < endPos && styler.SafeGetCharAt(j) == chClose)
            j++;
          else
            iLv = 0;
        }
        if (iLv) {
          if (k > i)
            styler.ColourTo((int)(k - 1), SCE_LOG_DEFAULT);
          styler.ColourTo((int)(j - 1), iLv);
          i = j;
        }
      }
    }

    /* --- 行内扫剩余部分 --- */
    {
      unsigned int iTok = i;

      while (iTok < endPos && styler.SafeGetCharAt(iTok) != '\n' && styler.SafeGetCharAt(iTok) != '\r') {

        char ch = styler.SafeGetCharAt(iTok);

        /* 注释：// 或 # 开头（前面是空白或行首）*/
        if ((ch == '#' || (ch == '/' && iTok + 1 < endPos &&
                           styler.SafeGetCharAt(iTok + 1) == '/')) &&
            (iTok == iLine || IsLogBlank(styler.SafeGetCharAt(iTok - 1)))) {
          unsigned int j = iTok;
          while (j < endPos && styler.SafeGetCharAt(j) != '\n' && styler.SafeGetCharAt(j) != '\r')
            j++;
          styler.ColourTo((int)(j - 1), SCE_LOG_COMMENT);
          iTok = j;
          break;
        }

        /* 引号字符串 */
        if (ch == '"' || ch == '\'') {
          char q = ch;
          unsigned int j = iTok + 1;
          while (j < endPos && styler.SafeGetCharAt(j) != q &&
                 styler.SafeGetCharAt(j) != '\n' && styler.SafeGetCharAt(j) != '\r') {
            if (styler.SafeGetCharAt(j) == '\\' && j + 1 < endPos)
              j++;
            j++;
          }
          if (j < endPos && styler.SafeGetCharAt(j) == q)
            j++;
          styler.ColourTo((int)(j - 1), SCE_LOG_STRING);
          iTok = j;
          continue;
        }

        /* 路径：/usr/local 或 C:\x 或 ./x */
        if (ch == '/' || ch == '\\' ||
            (ch == '.' && iTok + 1 < endPos && styler.SafeGetCharAt(iTok + 1) == '/')) {
          unsigned int j = iTok;
          while (j < endPos && styler.SafeGetCharAt(j) != '\n' && styler.SafeGetCharAt(j) != '\r' &&
                 styler.SafeGetCharAt(j) != ' ' && styler.SafeGetCharAt(j) != '\t' &&
                 styler.SafeGetCharAt(j) != '"' && styler.SafeGetCharAt(j) != '\'' &&
                 styler.SafeGetCharAt(j) != ')' && styler.SafeGetCharAt(j) != ']')
            j++;
          if (j - iTok >= 2) {
            styler.ColourTo((int)(j - 1), SCE_LOG_PATH);
            iTok = j;
            continue;
          }
        }

        /* key=value / key: value 的键。
           两个限制：键必须以字母或 _ 开头（否则 "10:00:00" 里的
           "10" 会被当成键），而且整段都得是词字符（"a.b" 这种
           带点的名字更像路径，交给后面的规则）。 */
        if (IsLogWordChar(ch)) {
          unsigned int j = iTok;
          bool bKeyStart = ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                            ch == '_');
          while (j < endPos && IsLogWordChar(styler.SafeGetCharAt(j)))
            j++;
          if (bKeyStart && j < endPos &&
              (styler.SafeGetCharAt(j) == '=' || styler.SafeGetCharAt(j) == ':')) {
            styler.ColourTo((int)(j - 1), SCE_LOG_KEY);
            iTok = j + 1;
            continue;
          }
          /* 数字与 IP：2024.1.2.3:8080 / 12.5 / 0x1F / 10:00:00 */
          bool bDigitRun = (ch >= '0' && ch <= '9');
          if (bDigitRun) {
            unsigned int m = iTok;
            while (m < endPos && ((styler.SafeGetCharAt(m) >= '0' && styler.SafeGetCharAt(m) <= '9') ||
                                  styler.SafeGetCharAt(m) == '.' || styler.SafeGetCharAt(m) == ':' ||
                                  styler.SafeGetCharAt(m) == ',' ||
                                  (styler.SafeGetCharAt(m) == 'x' && m == iTok + 1) ||
                                  (styler.SafeGetCharAt(m) >= 'a' && styler.SafeGetCharAt(m) <= 'f' &&
                                   (styler.SafeGetCharAt(m - 1) == 'x' ||
                                    styler.SafeGetCharAt(m - 1) == '0'))))
              m++;
            if (m > j) {
              styler.ColourTo((int)(m - 1), SCE_LOG_NUMBER);
              iTok = m;
              continue;
            }
          }
          styler.ColourTo((int)(j - 1), SCE_LOG_DEFAULT);
          iTok = j;
          continue;
        }

        /* (Foo.java:42) 与 [thread-1] 这类括号组 */
        if (ch == '(' || ch == '[') {
          char close = (ch == '(') ? ')' : ']';
          unsigned int j = iTok + 1;
          while (j < endPos && styler.SafeGetCharAt(j) != close &&
                 styler.SafeGetCharAt(j) != '\n' && styler.SafeGetCharAt(j) != '\r')
            j++;
          if (j < endPos && styler.SafeGetCharAt(j) == close && (j - iTok) < 200) {
            styler.ColourTo((int)j, SCE_LOG_CONTEXT);
            iTok = j + 1;
            continue;
          }
        }

        /* 其它：逐个字符 */
        styler.ColourTo((int)iTok, SCE_LOG_DEFAULT);
        iTok++;
      }

      if (iTok > i)
        i = iTok;
    }

    /* 行尾（含换行符本身）*/
    while (i < endPos && (styler.SafeGetCharAt(i) == '\n' || styler.SafeGetCharAt(i) == '\r'))
      i++;
    styler.ColourTo((int)(i - 1), SCE_LOG_DEFAULT);
  }
}

void FoldLogDoc(unsigned int, int, int, WordList *[], Accessor &) {
}

static const char *const logWordListDesc[] = {
  0
};

} // namespace

LexerModule lmLog(SCLEX_LOG, ColouriseLogDoc, "log", FoldLogDoc, logWordListDesc);
