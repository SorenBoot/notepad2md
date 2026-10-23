//=============================================================================
//
//  Format.h — 文档格式化（"只动空白"的安全重排）
//
//  设计要点（这是整个功能的地基）：
//    绝不用「解析成对象树 → 重新序列化」。那会静默改坏用户数据：
//    键顺序、数字字面形式（1.0 / 1e3）、大整数精度、重复键、Unicode 转义、
//    注释、XML 属性顺序与实体写法、文本节点空白。
//
//    正确做法是「词法流重写」：切 token → 只重新摆放 token 之间的空白，
//    token 的原始字节一个都不改。由此得到两条可直接当测试的硬不变量：
//      1) minify(format(x)) == minify(x)      语义零损失（字节级可比）
//      2) format(format(x)) == format(x)      幂等
//
//  本文件只声明不依赖 Win32 的纯函数，方便 tests/run/test_format.c 直接链接。
//
//=============================================================================

#ifndef FORMAT_H
#define FORMAT_H

#ifndef FMT_BOOL
typedef int FMT_BOOL;
#endif

/* 缩进宽度的合理范围；越界由调用方收敛（核心函数自己也会夹紧） */
#define FMT_INDENT_MIN   1
#define FMT_INDENT_MAX  16

/* 输入上限：超过就不猜、不格式化（界面先弹窗确认 8–16 MB，16 MB 以上直接拒绝） */
#define FMT_MAX_INPUT    (16*1024*1024)

/* 嵌套深度上限：递归下降的栈保护（JSON 数组嵌套、XML 元素嵌套） */
#define FMT_MAX_DEPTH    200

/*---------------------------------------------------------------------------
  结果
---------------------------------------------------------------------------*/

/* 格式化结果。成功时 p 由 malloc 分配，调用方用 Fmt_ResultFree() 释放。
   p 不保证以 '\0' 结尾，长度一律看 cb。 */
typedef struct {
  char *p;
  int   cb;
} FMTRESULT;

/* 格式化风格 */
typedef struct {
  const char *szEol;     /* "\r\n" / "\n" / "\r" —— 必须沿用文件原有行尾 */
  int         iIndent;   /* 每层缩进空格数，1..16；越界自动夹紧 */
} FMTSTYLE;

/*---------------------------------------------------------------------------
  错误
---------------------------------------------------------------------------*/

/* 错误码。核心不开 Win32 资源，返回码；界面负责翻译成本地化文字。 */
enum {
  FMT_E_OK = 0,
  FMT_E_NOMEM,               /* 内存不足 */
  FMT_E_EMPTY,               /* 空文档 / 只有空白 */
  FMT_E_UNEXPECTED_EOF,      /* 内容意外结束 */
  FMT_E_UNEXPECTED_CHAR,     /* 不该出现的字符 */
  FMT_E_EXPECTED_KEY,        /* 此处应是键名 */
  FMT_E_EXPECTED_COLON,      /* 此处应是冒号 */
  FMT_E_EXPECTED_VALUE,      /* 此处应是值 */
  FMT_E_EXPECTED_COMMA,      /* 此处应是逗号 */
  FMT_E_TRAILING_DATA,       /* 顶层结构之后还有多余内容 */
  FMT_E_UNTERMINATED_STRING, /* 字符串没有闭合引号 */
  FMT_E_UNTERMINATED_COMMENT,/* 块注释没有闭合 */
  FMT_E_TOO_DEEP,            /* 嵌套过深 */
  FMT_E_TOO_BIG,             /* 超过 FMT_MAX_INPUT */
  FMT_E_TAG_MISMATCH,        /* 标签不配对 */
  FMT_E_UNTERMINATED_TAG,    /* 标签没有闭合的 > */
  FMT_E_UNBALANCED           /* 括号 / 引号不配对 */
};

typedef struct {
  int iPos;    /* 出错处在输入中的字节偏移（iCode == FMT_E_OK 时无意义） */
  int iCode;   /* FMT_E_* */
} FMTERR;

/*---------------------------------------------------------------------------
  文档类型
---------------------------------------------------------------------------*/

enum {
  FMT_KIND_NONE = 0,
  FMT_KIND_JSON,
  FMT_KIND_XML,
  FMT_KIND_HTML,   /* XML 的宽松变体：<br> / <img> 这类 void 元素可以不写 "/>" */
  FMT_KIND_SQL,
  FMT_KIND_INI
};

/*---------------------------------------------------------------------------
  token 位置映射（格式化后还原光标 / 选区）

  格式化只重排空白、不改 token 字节，所以第 n 个 token 在输入和输出里是
  同一个东西；记下每个 token 的两端位置，就能把输入侧的光标精确搬到输出侧。
  不需要映射时给 NULL。
---------------------------------------------------------------------------*/

typedef struct {
  int *piIn;    /* token 在输入中的起始偏移 */
  int *piOut;   /* 同一 token 在输出中的起始偏移 */
  int *piLen;   /* token 字节长度（两侧相同） */
  int  c;       /* 已记录数 */
  int  cMax;    /* 容量 */
} FMTMAP;

/*---------------------------------------------------------------------------
  统一入口

  iKind    FMT_KIND_*
  pIn/cbIn 原始字节。可以是 UTF-8，也可以是系统代码页（GBK 等）——
           核心只识别 ASCII 空白与结构字符，多字节内容一律原样搬运，
           全程不解码，因此绝不会把中文写坏。
  pStyle   NULL 时用默认（"\r\n" + 2 空格）。
  pOut     成功时得到结果；失败时不动。
  pMap     可为 NULL。
  pErr     可为 NULL。
---------------------------------------------------------------------------*/

FMT_BOOL Fmt_Format(int iKind, const char *pIn, int cbIn, const FMTSTYLE *pStyle,
                    FMTRESULT *pOut, FMTMAP *pMap, FMTERR *pErr);

FMT_BOOL Fmt_Minify(int iKind, const char *pIn, int cbIn, const FMTSTYLE *pStyle,
                    FMTRESULT *pOut, FMTMAP *pMap, FMTERR *pErr);

/* 只校验不产出（内部就是跑一遍格式化后丢掉结果，保证与格式化同一套规则）*/
FMT_BOOL Fmt_Validate(int iKind, const char *pIn, int cbIn, FMTERR *pErr);

/* 内容嗅探。猜不出返回 FMT_KIND_NONE。
   只在"当前语法方案给不出结论"时作为兜底，不抢语法高亮的判断。 */
int Fmt_DetectKind(const char *pIn, int cbIn);

/*---------------------------------------------------------------------------
  工具
---------------------------------------------------------------------------*/

void Fmt_ResultFree(FMTRESULT *p);
void Fmt_MapFree(FMTMAP *p);

/* 输入侧光标偏移 → 输出侧光标偏移。映射为空时原样返回。 */
int  Fmt_MapCaret(const FMTMAP *pMap, int iInCaret, int cbIn, int cbOut);

/* 字节偏移 → 0 基行号 / 0 基列号（供 GUI 定位错误；按 \r\n \n \r 计行）*/
void Fmt_OffsetToLineCol(const char *p, int cb, int iPos, int *piLine, int *piCol);

/* 错误码 → 静态英文名（测试与日志用；界面走自己的资源串） */
const char *Fmt_ErrName(int iCode);

#endif // FORMAT_H

// End of Format.h
