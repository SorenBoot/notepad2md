// 测试用：LexerModule 桩
#ifndef FAKE_LEXERMODULE_H
#define FAKE_LEXERMODULE_H

class LexerModule {
public:
  typedef void (*FnLexer)(unsigned int startPos, int length, int initStyle,
                          WordList *keywordlists[], Accessor &styler);
  LexerModule(int, FnLexer, const char *, FnLexer = 0, const char *const * = 0) {}
};

#endif
