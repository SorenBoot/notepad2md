// Scintilla source code edit control
/** @file LexYaml.cxx
 ** Lexer for YAML.
 **
 ** 独立实现：注释、文档边界、键、字符串、数字、锚点/别名、标签、
 ** 列表符号（-）与运算符。
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

bool IsYamlBreak(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\0' || ch == '\r' || ch == '\n';
}

// 值区域的着色：引号、数字、布尔/null、锚点、标签
void StyleYamlValue(Accessor &styler, unsigned int pos, unsigned int endPos) {
  unsigned int i = pos;
  // 不在此重置分段起点：让冒号后的空白一并被涂成默认色，避免留下未上色空档

  while (i < endPos) {
    char ch = styler.SafeGetCharAt(i);

    if (ch == '#') {
      styler.ColourTo(i - 1 >= pos ? i - 1 : pos, SCE_YAML_DEFAULT);
      styler.StartSegment(i);
      styler.ColourTo(endPos - 1, SCE_YAML_COMMENT);
      return;
    }

    if (ch == '"' || ch == '\'') {
      char q = ch;
      unsigned int j = i + 1;
      while (j < endPos) {
        char c = styler.SafeGetCharAt(j);
        if (q == '"' && c == '\\') {
          j += 2;
          continue;
        }
        if (c == q) {
          if (q == '\'' && j + 1 < endPos && styler.SafeGetCharAt(j + 1) == '\'') {
            j += 2;           // 单引号里的 '' 是转义
            continue;
          }
          break;
        }
        j++;
      }
      if (j < endPos) {
        styler.ColourTo(j, SCE_YAML_DEFAULT);
        i = j + 1;
        styler.StartSegment(i);
        continue;
      }
      styler.ColourTo(endPos - 1, SCE_YAML_DEFAULT);
      return;
    }

    if ((ch == '&' || ch == '*') && i + 1 < endPos && !IsYamlBreak(styler.SafeGetCharAt(i + 1))) {
      unsigned int j = i + 1;
      while (j < endPos && !IsYamlBreak(styler.SafeGetCharAt(j)))
        j++;
      styler.ColourTo(j - 1, SCE_YAML_REFERENCE);
      i = j;
      styler.StartSegment(i);
      continue;
    }

    if (ch == '!') {
      unsigned int j = i + 1;
      while (j < endPos && !IsYamlBreak(styler.SafeGetCharAt(j)))
        j++;
      styler.ColourTo(j - 1, SCE_YAML_KEYWORD);
      i = j;
      styler.StartSegment(i);
      continue;
    }

    if (ch == '|' || ch == '>') {
      styler.ColourTo(i, SCE_YAML_OPERATOR);
      i++;
      styler.StartSegment(i);
      continue;
    }

    i++;
  }

  if (endPos > pos)
    styler.ColourTo(endPos - 1, SCE_YAML_DEFAULT);
}

// 行首缩进宽度
unsigned int YamlIndent(Accessor &styler, unsigned int pos, unsigned int endPos) {
  unsigned int n = 0;
  while (pos + n < endPos && styler.SafeGetCharAt(pos + n) == ' ')
    n++;
  return n;
}

} // namespace

static void ColouriseYamlDoc(unsigned int startPos, int length, int initStyle,
                             WordList *keywordLists[], Accessor &styler) {
  unsigned int endPos = startPos + length;
  if (length <= 0)
    return;

  WordList &keywords = *keywordLists[0];
  bool inBlockScalar = (initStyle == SCE_YAML_TEXT);

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

    unsigned int indent = YamlIndent(styler, lineStart, lineEnd);
    unsigned int content = lineStart + indent;
    unsigned int contentLen = lineEnd - content;

    if (contentLen == 0) {
      styler.ColourTo(lineEnd > lineStart ? lineEnd - 1 : lineStart,
                      inBlockScalar ? SCE_YAML_TEXT : SCE_YAML_DEFAULT);
    }
    else {
      char first = styler.SafeGetCharAt(content);

      // 块标量（| 或 >）后续内容按文本着色，遇到缩进回退才退出
      if (inBlockScalar) {
        if (indent > 0) {
          styler.ColourTo(lineEnd - 1, SCE_YAML_TEXT);
        } else {
          inBlockScalar = false;
        }
      }

      if (!inBlockScalar) {
        if (first == '#') {
          styler.ColourTo(lineEnd - 1, SCE_YAML_COMMENT);
        }
        else if (contentLen >= 3 && first == '-' &&
                 styler.SafeGetCharAt(content + 1) == '-' &&
                 styler.SafeGetCharAt(content + 2) == '-' &&
                 (contentLen == 3 || IsYamlBreak(styler.SafeGetCharAt(content + 3)))) {
          styler.ColourTo(lineEnd - 1, SCE_YAML_DOCUMENT);
        }
        else if (contentLen >= 3 && first == '.' &&
                 styler.SafeGetCharAt(content + 1) == '.' &&
                 styler.SafeGetCharAt(content + 2) == '.' &&
                 (contentLen == 3 || IsYamlBreak(styler.SafeGetCharAt(content + 3)))) {
          styler.ColourTo(lineEnd - 1, SCE_YAML_DOCUMENT);
        }
        else if (first == '%') {
          styler.ColourTo(lineEnd - 1, SCE_YAML_KEYWORD);
        }
        else {
          // 找 key 的结束冒号（需跟空白或行尾，避免 http:// 里的冒号）
          unsigned int k = content;
          unsigned int colon = 0;
          bool foundColon = false;
          if (first != '-' && first != '[' && first != '{' && first != '"' && first != '\'') {
            while (k < lineEnd) {
              char c = styler.SafeGetCharAt(k);
              if (c == '#' && k > content && IsYamlBreak(styler.SafeGetCharAt(k - 1)))
                break;
              if (c == ':' &&
                  (k + 1 >= lineEnd || IsYamlBreak(styler.SafeGetCharAt(k + 1)))) {
                colon = k;
                foundColon = true;
                break;
              }
              k++;
            }
          }

          // 列表项：- 后跟空白
          if (first == '-' && (contentLen == 1 || IsYamlBreak(styler.SafeGetCharAt(content + 1)))) {
            styler.ColourTo(content, SCE_YAML_OPERATOR);
            styler.StartSegment(content + 1);
            styler.ColourTo(lineEnd - 1, SCE_YAML_DEFAULT);
          }
          else if (foundColon) {
            unsigned int keyEnd = colon;
            // 去掉键尾空白
            while (keyEnd > content && IsYamlBreak(styler.SafeGetCharAt(keyEnd - 1)))
              keyEnd--;
            styler.ColourTo(keyEnd > content ? keyEnd - 1 : content, SCE_YAML_IDENTIFIER);
            styler.StartSegment(keyEnd);
            styler.ColourTo(colon, SCE_YAML_OPERATOR);
            styler.StartSegment(colon + 1);
            // 值部分
            unsigned int vStart = colon + 1;
            while (vStart < lineEnd && IsYamlBreak(styler.SafeGetCharAt(vStart)) && 
                   styler.SafeGetCharAt(vStart) != '#')
              vStart++;
            if (vStart < lineEnd) {
              char v0 = styler.SafeGetCharAt(vStart);
              // 块标量起始
              if ((v0 == '|' || v0 == '>') && (vStart + 1 >= lineEnd ||
                  IsYamlBreak(styler.SafeGetCharAt(vStart + 1)))) {
                styler.ColourTo(lineEnd - 1, SCE_YAML_OPERATOR);
                inBlockScalar = true;
              } else {
                StyleYamlValue(styler, vStart, lineEnd);
              }
            } else {
              styler.ColourTo(lineEnd - 1, SCE_YAML_DEFAULT);
            }
          }
          else {
            // 无冒号：可能是纯值 / 关键字 / 多行字符串
            unsigned int t = content;
            while (t < lineEnd && !IsYamlBreak(styler.SafeGetCharAt(t)))
              t++;
            char buf[64];
            unsigned int n = t - content;
            if (n > 0 && n < sizeof(buf)) {
              for (unsigned int b = 0; b < n; b++)
                buf[b] = styler.SafeGetCharAt(content + b);
              buf[n] = '\0';
              if (keywords.InList(buf)) {
                styler.ColourTo(t - 1, SCE_YAML_KEYWORD);
                styler.StartSegment(t);
                StyleYamlValue(styler, t, lineEnd);
              } else {
                StyleYamlValue(styler, content, lineEnd);
              }
            } else {
              StyleYamlValue(styler, content, lineEnd);
            }
          }
        }
      }
    }

    styler.SetLineState(styler.GetLine(lineStart), inBlockScalar ? SCE_YAML_TEXT : 0);

    i = lineEnd;
    while (i < endPos) {
      char c = styler.SafeGetCharAt(i);
      if (c == '\r' || c == '\n') {
        styler.ColourTo(i, SCE_YAML_DEFAULT);
        i++;
      } else {
        break;
      }
    }
    styler.StartSegment(i);
  }
}

static void FoldYamlDoc(unsigned int, int, int, WordList *[], Accessor &) {
}

static const char *const yamlWordListDesc[] = {
  "True/False keywords",
  0
};

LexerModule lmYaml(SCLEX_YAML, ColouriseYamlDoc, "yaml", FoldYamlDoc, yamlWordListDesc);
