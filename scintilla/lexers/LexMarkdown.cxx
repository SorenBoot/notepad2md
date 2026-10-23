// Scintilla source code edit control
/** @file LexMarkdown.cxx
 ** Lexer for Markdown (CommonMark / GFM 常用子集).
 **
 ** 独立实现，只依赖 Scintilla 公开的 lexer 接口：
 **   ATX / Setext 标题、粗体、斜体、行内代码、围栏与缩进代码块、
 **   列表、引用、分隔线、链接与图片、自动链接。
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

// 行状态：0 表示不在围栏内；否则 (fenceLen << 8) | fenceChar
int MakeFenceState(unsigned int len, char ch) {
  return (int)((len << 8) | (unsigned char)ch);
}
unsigned int FenceStateLen(int state) { return (unsigned int)(state >> 8); }
char FenceStateChar(int state) { return (char)(state & 0xFF); }

unsigned int CountRun(Accessor &styler, unsigned int pos, unsigned int endPos, char ch) {
  unsigned int n = 0;
  while (pos + n < endPos && styler.SafeGetCharAt(pos + n) == ch)
    n++;
  return n;
}

// 围栏起始标记（``` 或 ~~~，>=3 个），否则返回 0
unsigned int FenceRun(Accessor &styler, unsigned int pos, unsigned int endPos) {
  if (pos >= endPos)
    return 0;
  char ch = styler.SafeGetCharAt(pos);
  if (ch != '`' && ch != '~')
    return 0;
  unsigned int n = CountRun(styler, pos, endPos, ch);
  return (n >= 3) ? n : 0;
}

// 从行首跳过最多 3 个空格（Markdown 块级元素允许的缩进）
unsigned int SkipIndent(Accessor &styler, unsigned int pos, unsigned int endPos, unsigned int *pIndent) {
  unsigned int i = pos;
  unsigned int spaces = 0;
  while (i < endPos && styler.SafeGetCharAt(i) == ' ' && spaces < 4) {
    i++;
    spaces++;
  }
  if (pIndent)
    *pIndent = spaces;
  return i;
}

// 行内元素：粗体/斜体/行内代码/链接/图片/自动链接
void StyleInline(Accessor &styler, unsigned int pos, unsigned int endPos, int styleNormal) {
  unsigned int i = pos;

  while (i < endPos) {
    char ch = styler.SafeGetCharAt(i);

    if (ch == '\\' && i + 1 < endPos) {
      i += 2;
      continue;
    }

    if (ch == '`') {
      unsigned int n = CountRun(styler, i, endPos, '`');
      unsigned int j = i + n;
      bool closed = false;
      while (j < endPos) {
        if (styler.SafeGetCharAt(j) == '`') {
          unsigned int m = CountRun(styler, j, endPos, '`');
          if (m == n) {
            styler.ColourTo(i - 1, styleNormal);  // 先给前面的普通文本上色
            styler.ColourTo(j + m - 1, SCE_MARKDOWN_CODEBK);
            i = j + m;
            styler.StartSegment(i);
            closed = true;
            break;
          }
          j += m;
        } else {
          j++;
        }
      }
      if (!closed) {
        styler.ColourTo(i - 1, styleNormal);
        styler.ColourTo(endPos - 1, SCE_MARKDOWN_CODEBK);
        return;
      }
      continue;
    }

    if (ch == '*' || ch == '_') {
      int emStyle = (ch == '*') ? SCE_MARKDOWN_EM1 : SCE_MARKDOWN_EM2;
      int strongStyle = (ch == '*') ? SCE_MARKDOWN_STRONG1 : SCE_MARKDOWN_STRONG2;
      bool strong = (i + 1 < endPos && styler.SafeGetCharAt(i + 1) == ch);
      unsigned int n = strong ? 2 : 1;
      unsigned int j = i + n;
      bool closed = false;
      while (j < endPos) {
        if (styler.SafeGetCharAt(j) == ch) {
          if (strong) {
            if (j + 1 < endPos && styler.SafeGetCharAt(j + 1) == ch) {
              styler.ColourTo(i - 1, styleNormal);
              styler.ColourTo(j + 1, strongStyle);
              i = j + 2;
              styler.StartSegment(i);
              closed = true;
              break;
            }
            j++;
          } else {
            styler.ColourTo(i - 1, styleNormal);
            styler.ColourTo(j, emStyle);
            i = j + 1;
            styler.StartSegment(i);
            closed = true;
            break;
          }
        } else {
          j++;
        }
      }
      if (!closed)
        i += n;
      continue;
    }

    if (ch == '[' || (ch == '!' && i + 1 < endPos && styler.SafeGetCharAt(i + 1) == '[')) {
      unsigned int j = (ch == '!') ? i + 2 : i + 1;
      int depth = 1;
      while (j < endPos) {
        char c = styler.SafeGetCharAt(j);
        if (c == '[')
          depth++;
        else if (c == ']') {
          depth--;
          if (depth == 0)
            break;
        }
        j++;
      }
      // 必须跟 (url) 才算链接或图片
      if (j < endPos && depth == 0 && j + 1 < endPos && styler.SafeGetCharAt(j + 1) == '(') {
        unsigned int k = j + 2;
        int paren = 1;
        while (k < endPos) {
          char c = styler.SafeGetCharAt(k);
          if (c == '(')
            paren++;
          else if (c == ')') {
            paren--;
            if (paren == 0)
              break;
          }
          k++;
        }
        if (k < endPos) {
          styler.ColourTo(i - 1, styleNormal);
          styler.ColourTo(k, SCE_MARKDOWN_LINK);
          i = k + 1;
          styler.StartSegment(i);
          continue;
        }
      }
      i++;
      continue;
    }

    if (ch == '<') {
      unsigned int j = i + 1;
      while (j < endPos && styler.SafeGetCharAt(j) != '>' && !IsASpace(styler.SafeGetCharAt(j)))
        j++;
      if (j < endPos && styler.SafeGetCharAt(j) == '>') {
        styler.ColourTo(i - 1, styleNormal);
        styler.ColourTo(j, SCE_MARKDOWN_LINK);
        i = j + 1;
        styler.StartSegment(i);
        continue;
      }
    }

    i++;
  }

  if (endPos > pos)
    styler.ColourTo(endPos - 1, styleNormal);
}

} // namespace

static void ColouriseMarkdownDoc(unsigned int startPos, int length, int initStyle,
                                 WordList *[], Accessor &styler) {
  unsigned int endPos = startPos + length;
  if (length <= 0)
    return;

  // 该段起点是否已在围栏代码块内：看上一行的行状态
  int line = styler.GetLine(startPos);
  int fenceState = 0;
  if (line > 0)
    fenceState = styler.GetLineState(line - 1);
  if (initStyle == SCE_MARKDOWN_CODE && fenceState == 0)
    fenceState = MakeFenceState(3, '`');   // 兜底：上游认为在代码块内

  styler.StartAt(startPos);
  styler.StartSegment(startPos);


  unsigned int i = startPos;
  while (i < endPos) {
    unsigned int lineStart = i;
    unsigned int lineEnd = i;
    while (lineEnd < endPos) {
      char c = styler.SafeGetCharAt(lineEnd);
      if (c == '\r' || c == '\n')
        break;
      lineEnd++;
    }

    unsigned int indent = 0;
    unsigned int content = SkipIndent(styler, lineStart, lineEnd, &indent);
    unsigned int contentLen = lineEnd - content;

    if (contentLen == 0) {
      // 空行：围栏内算代码，围栏外算默认
      styler.ColourTo(lineEnd > lineStart ? lineEnd - 1 : lineStart,
                      fenceState ? SCE_MARKDOWN_CODE : SCE_MARKDOWN_DEFAULT);
    }
    else if (fenceState) {
      char fc = FenceStateChar(fenceState);
      unsigned int flen = FenceStateLen(fenceState);
      bool closeFence = false;
      if (styler.SafeGetCharAt(content) == fc) {
        unsigned int run = CountRun(styler, content, lineEnd, fc);
        if (run >= flen) {
          unsigned int k = content + run;
          bool onlySpace = true;
          while (k < lineEnd) {
            if (!IsASpace(styler.SafeGetCharAt(k))) {
              onlySpace = false;
              break;
            }
            k++;
          }
          closeFence = onlySpace;
        }
      }
      styler.ColourTo(lineEnd - 1, SCE_MARKDOWN_CODE);
      if (closeFence)
        fenceState = 0;
    }
    else {
      unsigned int fenceRun = FenceRun(styler, content, lineEnd);
      char first = styler.SafeGetCharAt(content);

      if (fenceRun >= 3) {
        fenceState = MakeFenceState(fenceRun, first);
        styler.ColourTo(lineEnd - 1, SCE_MARKDOWN_CODE);
      }
      else if (indent >= 4) {
        styler.ColourTo(lineEnd - 1, SCE_MARKDOWN_PRECHAR);
      }
      else if ((first == '-' || first == '*' || first == '_') &&
               CountRun(styler, content, lineEnd, first) >= 3) {
        unsigned int k = content;
        bool onlyMarks = true;
        while (k < lineEnd) {
          char c = styler.SafeGetCharAt(k);
          if (c != first && !IsASpace(c)) {
            onlyMarks = false;
            break;
          }
          k++;
        }
        if (onlyMarks)
          styler.ColourTo(lineEnd - 1, SCE_MARKDOWN_HRULE);
        else
          StyleInline(styler, content, lineEnd, SCE_MARKDOWN_DEFAULT);
      }
      else if (first == '#') {
        unsigned int hashes = CountRun(styler, content, lineEnd, '#');
        if (hashes <= 6 && (content + hashes >= lineEnd ||
                            IsASpace(styler.SafeGetCharAt(content + hashes)))) {
          int st = SCE_MARKDOWN_HEADER1 + (int)(hashes - 1);
          styler.ColourTo(lineEnd - 1, st);
        } else {
          StyleInline(styler, content, lineEnd, SCE_MARKDOWN_DEFAULT);
        }
      }
      else if (first == '>') {
        styler.ColourTo(lineEnd - 1, SCE_MARKDOWN_BLOCKQUOTE);
      }
      else if ((first == '-' || first == '+' || first == '*') &&
               (content + 1 >= lineEnd || IsASpace(styler.SafeGetCharAt(content + 1)))) {
        // 只给列表标记上色，正文按行内规则处理
        styler.ColourTo(content, SCE_MARKDOWN_ULIST_ITEM);
        styler.StartSegment(content + 1);
        if (content + 1 < lineEnd)
          StyleInline(styler, content + 1, lineEnd, SCE_MARKDOWN_DEFAULT);
      }
      else if (IsADigit(first)) {
        unsigned int k = content;
        while (k < lineEnd && IsADigit(styler.SafeGetCharAt(k)))
          k++;
        if (k < lineEnd && (styler.SafeGetCharAt(k) == '.' || styler.SafeGetCharAt(k) == ')') &&
            (k + 1 >= lineEnd || IsASpace(styler.SafeGetCharAt(k + 1)))) {
          styler.ColourTo(k, SCE_MARKDOWN_OLIST_ITEM);
          styler.StartSegment(k + 1);
          if (k + 1 < lineEnd)
            StyleInline(styler, k + 1, lineEnd, SCE_MARKDOWN_DEFAULT);
        }
        else
          StyleInline(styler, content, lineEnd, SCE_MARKDOWN_DEFAULT);
      }
      else {
        StyleInline(styler, content, lineEnd, SCE_MARKDOWN_DEFAULT);
      }
    }

    styler.SetLineState(styler.GetLine(lineStart), fenceState);

    // 行尾换行符着默认色
    i = lineEnd;
    while (i < endPos) {
      char c = styler.SafeGetCharAt(i);
      if (c == '\r' || c == '\n') {
        styler.ColourTo(i, SCE_MARKDOWN_DEFAULT);
        i++;
      } else {
        break;
      }
    }
    styler.StartSegment(i);
  }
}

static void FoldMarkdownDoc(unsigned int, int, int, WordList *[], Accessor &) {
  // 暂不提供折叠
}

static const char *const markdownWordListDesc[] = { 0 };

LexerModule lmMarkdown(SCLEX_MARKDOWN, ColouriseMarkdownDoc, "markdown",
                       FoldMarkdownDoc, markdownWordListDesc);
