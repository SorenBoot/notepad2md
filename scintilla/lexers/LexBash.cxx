// Scintilla source code edit control
/** @file LexBash.cxx
 ** Lexer for shell scripts (bash / sh / zsh 常用子集).
 **
 ** 独立实现：注释、字符串（单/双引号、$'...'）、变量（$VAR / ${...} / $1 / $@）、
 ** 命令替换 / 反引号、here-document、数字、操作符、关键字。
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

bool IsShellBreak(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\0' || ch == '\r' || ch == '\n';
}

bool IsVarStart(char ch) {
  return isalpha((unsigned char)ch) || ch == '_';
}

bool IsVarChar(char ch) {
  return isalnum((unsigned char)ch) || ch == '_';
}

} // namespace

static void ColouriseShellDoc(unsigned int startPos, int length, int initStyle,
                              WordList *keywordLists[], Accessor &styler) {
  unsigned int endPos = startPos + length;
  if (length <= 0)
    return;

  WordList &keywords = *keywordLists[0];
  int state = initStyle;
  unsigned int i = startPos;

  styler.StartAt(startPos);
  styler.StartSegment(startPos);


  while (i < endPos) {
    char ch = styler.SafeGetCharAt(i);
    char next = styler.SafeGetCharAt(i + 1);

    // 行尾统一复位（here-doc 体由 line state 处理，这里只管单行状态）
    if (ch == '\r' || ch == '\n') {
      if (state != SCE_SH_HERE_Q) {
        styler.ColourTo(i, SCE_SH_DEFAULT);
        state = SCE_SH_DEFAULT;
        styler.StartSegment(i + 1);
      }
      i++;
      continue;
    }

    switch (state) {
      case SCE_SH_DEFAULT: {
        if (ch == '#') {
          unsigned int j = i;
          while (j < endPos) {
            char c = styler.SafeGetCharAt(j);
            if (c == '\r' || c == '\n')
              break;
            j++;
          }
          styler.ColourTo(i - 1, SCE_SH_DEFAULT);
          styler.ColourTo(j - 1, SCE_SH_COMMENTLINE);
          i = j;
          styler.StartSegment(i);
          continue;
        }

        if (ch == '\'') {
          unsigned int j = i + 1;
          while (j < endPos) {
            char c = styler.SafeGetCharAt(j);
            if (c == '\'')
              break;
            j++;
          }
          styler.ColourTo(i - 1, SCE_SH_DEFAULT);
          styler.ColourTo(j < endPos ? j : endPos - 1, SCE_SH_STRING);
          i = (j < endPos) ? j + 1 : endPos;
          styler.StartSegment(i);
          continue;
        }

        if (ch == '"') {
          // 双引号内允许变量与转义，交给 SCE_SH_STRING 整段着色后单独标变量
          unsigned int j = i + 1;
          while (j < endPos) {
            char c = styler.SafeGetCharAt(j);
            if (c == '\\' && j + 1 < endPos) {
              j += 2;
              continue;
            }
            if (c == '"')
              break;
            j++;
          }
          styler.ColourTo(i - 1, SCE_SH_DEFAULT);
          styler.ColourTo(j < endPos ? j : endPos - 1, SCE_SH_STRING);
          i = (j < endPos) ? j + 1 : endPos;
          styler.StartSegment(i);
          continue;
        }

        if (ch == '`') {
          unsigned int j = i + 1;
          while (j < endPos && styler.SafeGetCharAt(j) != '`')
            j++;
          styler.ColourTo(i - 1, SCE_SH_DEFAULT);
          styler.ColourTo(j < endPos ? j : endPos - 1, SCE_SH_BACKTICKS);
          i = (j < endPos) ? j + 1 : endPos;
          styler.StartSegment(i);
          continue;
        }

        if (ch == '$') {
          unsigned int j = i + 1;
          if (j < endPos && styler.SafeGetCharAt(j) == '{') {
            j++;
            while (j < endPos && styler.SafeGetCharAt(j) != '}')
              j++;
            j = (j < endPos) ? j + 1 : endPos;
            styler.ColourTo(j - 1, SCE_SH_SCALAR);
            i = j;
            styler.StartSegment(i);
            continue;
          }
          if (j < endPos && IsVarStart(styler.SafeGetCharAt(j))) {
            while (j < endPos && IsVarChar(styler.SafeGetCharAt(j)))
              j++;
            styler.ColourTo(j - 1, SCE_SH_SCALAR);
            i = j;
            styler.StartSegment(i);
            continue;
          }
          if (j < endPos && (isdigit((unsigned char)styler.SafeGetCharAt(j)) ||
                             styler.SafeGetCharAt(j) == '@' || styler.SafeGetCharAt(j) == '*' ||
                             styler.SafeGetCharAt(j) == '#' || styler.SafeGetCharAt(j) == '?' ||
                             styler.SafeGetCharAt(j) == '!' || styler.SafeGetCharAt(j) == '$')) {
            styler.ColourTo(j, SCE_SH_SCALAR);
            i = j + 1;
            styler.StartSegment(i);
            continue;
          }
          i++;
          continue;
        }

        if (isdigit((unsigned char)ch)) {
          unsigned int j = i;
          while (j < endPos && (isalnum((unsigned char)styler.SafeGetCharAt(j)) ||
                               styler.SafeGetCharAt(j) == '.'))
            j++;
          styler.ColourTo(j - 1, SCE_SH_NUMBER);
          i = j;
          styler.StartSegment(i);
          continue;
        }

        if (IsVarStart(ch) ) {
          unsigned int j = i;
          while (j < endPos && IsVarChar(styler.SafeGetCharAt(j)))
            j++;
          unsigned int n = j - i;
          char buf[64];
          if (n < sizeof(buf)) {
            for (unsigned int b = 0; b < n; b++)
              buf[b] = styler.SafeGetCharAt(i + b);
            buf[n] = '\0';
            if (keywords.InList(buf)) {
              styler.ColourTo(j - 1, SCE_SH_WORD);
              i = j;
              styler.StartSegment(i);
              continue;
            }
          }
          styler.ColourTo(j - 1, SCE_SH_IDENTIFIER);
          i = j;
          styler.StartSegment(i);
          continue;
        }

        if (strchr("=+-*/%!<>&|^~?:;()[]{}", ch)) {
          // here-document 起始：<< 或 <<-
          if (ch == '<' && next == '<') {
            unsigned int j = i + 2;
            if (j < endPos && styler.SafeGetCharAt(j) == '-')
              j++;
            while (j < endPos && IsShellBreak(styler.SafeGetCharAt(j)))
              j++;
            unsigned int dstart = j;
            while (j < endPos && !IsShellBreak(styler.SafeGetCharAt(j)) &&
                   !strchr(";|&<>()", styler.SafeGetCharAt(j)))
              j++;
            if (j > dstart) {
              styler.ColourTo(dstart - 1, SCE_SH_OPERATOR);
              styler.StartSegment(dstart);
              styler.ColourTo(j - 1, SCE_SH_HERE_DELIM);
              i = j;
              styler.StartSegment(i);
              continue;
            }
          }
          styler.ColourTo(i, SCE_SH_OPERATOR);
          i++;
          styler.StartSegment(i);
          continue;
        }

        i++;
        continue;
      }

      default:
        i++;
        continue;
    }
  }

  if (endPos > startPos)
    styler.ColourTo(endPos - 1, state == SCE_SH_DEFAULT ? SCE_SH_DEFAULT : state);
}

static void FoldShellDoc(unsigned int, int, int, WordList *[], Accessor &) {
}

static const char *const shellWordListDesc[] = {
  "Shell keywords",
  0
};

LexerModule lmBash(SCLEX_BASH, ColouriseShellDoc, "bash", FoldShellDoc, shellWordListDesc);
