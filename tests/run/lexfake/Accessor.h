// 测试用：内存 Accessor，只实现 lexer 实际用到的接口
#ifndef FAKE_ACCESSOR_H
#define FAKE_ACCESSOR_H

#include <string.h>
#include <string>
#include <vector>

class WordList;

class Accessor {
public:
  std::string doc;
  std::vector<int> styles;      // 逐字节样式
  std::vector<int> lineStates;
  std::vector<char> painted;    // 该字节是否被显式涂过（防止“空档”留旧样式）

  unsigned int startSeg;
  unsigned int paintCalls;
  bool orderViolation;          // 出现 pos < startSeg 的越界涂色
  unsigned vPos, vSeg;          // 首次越界的 pos / startSeg

  Accessor() : startSeg(0), paintCalls(0), orderViolation(false), vPos(0), vSeg(0) {}

  void SetDoc(const char *text, size_t len) {
    doc.assign(text, len);
    styles.assign(len, 0);
    lineStates.assign(len + 1, 0);
    painted.assign(len, 0);
  }

  char SafeGetCharAt(unsigned int pos) const {
    if (pos >= doc.size())
      return '\0';
    return doc[pos];
  }

  int GetLine(unsigned int pos) const {
    int line = 0;
    for (unsigned int i = 0; i < pos && i < doc.size(); i++)
      if (doc[i] == '\n')
        line++;
    return line;
  }

  unsigned int LineStart(int line) const {
    if (line <= 0)
      return 0;
    int seen = 0;
    for (unsigned int i = 0; i < doc.size(); i++) {
      if (doc[i] == '\n') {
        seen++;
        if (seen == line)
          return i + 1;
      }
    }
    return (unsigned int)doc.size();
  }

  unsigned int LineEnd(int line) const {
    unsigned int p = LineStart(line);
    while (p < doc.size() && doc[p] != '\n' && doc[p] != '\r')
      p++;
    return p;
  }

  void StartAt(unsigned int pos) { startSeg = pos; }
  void StartSegment(unsigned int pos) { startSeg = pos; }

  void ColourTo(unsigned int pos, int style) {
    // 与 Scintilla LexAccessor::ColourTo 一致：pos == startSeg-1 表示空区间，直接跳过
    if (pos == startSeg - 1)
      return;
    paintCalls++;
    if (pos < startSeg) {
      if (!orderViolation) {
        orderViolation = true;    // Scintilla 里这是 assert，会把区间算错
        vPos = pos;
        vSeg = startSeg;
      }
      return;
    }
    for (unsigned int i = startSeg; i <= pos && i < styles.size(); i++) {
      styles[i] = style;
      painted[i] = 1;
    }
    startSeg = pos + 1;
  }

  /* 未被显式涂色的字节数：Scintilla 会保留旧样式，必须为 0 */
  size_t UnpaintedCount(size_t *pFirst) const {
    size_t n = 0;
    for (size_t i = 0; i < painted.size(); i++) {
      if (!painted[i]) {
        if (n == 0 && pFirst)
          *pFirst = i;
        n++;
      }
    }
    return n;
  }

  int GetLineState(int line) const {
    if (line < 0 || line >= (int)lineStates.size())
      return 0;
    return lineStates[line];
  }
  void SetLineState(int line, int state) {
    if (line >= 0 && line < (int)lineStates.size())
      lineStates[line] = state;
  }
};

#endif
