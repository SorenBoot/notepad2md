/*===========================================================================

  notepad2md — Markdown 渲染模块接口

  ===========================================================================*/

#ifndef MD_PREVIEW_MARKDOWN_H
#define MD_PREVIEW_MARKDOWN_H

#ifdef __cplusplus
extern "C" {
#endif

/*
  将 Markdown 文本转换为完整 HTML 文档字符串。

  mdtext  —— 以 NUL 结尾的 Markdown 源文本
  返回    —— malloc 分配的 HTML 字符串（含 DOCTYPE/style/body），
              调用方必须用 Markdown_FreeHtml() 释放。
              输入为 NULL 或解析失败时返回 NULL。

  注意：md4c 内部按 UTF-8 处理。若 mdtext 不是 UTF-8 编码，
        需由调用方先转换为 UTF-8，否则非 ASCII 字符会乱码。
*/
char* Markdown_Render(const char *mdtext);

/*
  仅将 Markdown 文本转为 body 片段（不含 html/head/style 外壳）。

  用于预览面板的增量刷新：拿到片段后替换内容容器的 innerHTML，
  而不是重载整个文档，这样才能避免闪烁。

  返回值同样用 Markdown_FreeHtml() 释放。
*/
char* Markdown_RenderFragment(const char *mdtext);

/*
  带本地化文案的版本。

  copyLabelUtf8   —— 代码块复制按钮的初始文字（UTF-8，如 "复制"/"Copy"）
  copiedLabelUtf8 —— 点击后短暂显示的文字（UTF-8，如 "已复制"/"Copied"）

  传 NULL 时按英文 "Copy"/"Copied" 处理。旧的 Markdown_Render() /
  Markdown_RenderFragment() 就是调用这两个函数、传入 NULL 的薄包装，
  这样 Markdown 模块本身不依赖任何 Windows 资源。
*/
char* Markdown_RenderEx(const char *mdtext,
                        const char *copyLabelUtf8,
                        const char *copiedLabelUtf8);

char* Markdown_RenderFragmentEx(const char *mdtext,
                                const char *copyLabelUtf8,
                                const char *copiedLabelUtf8);

/* 释放 Markdown_Render() / Markdown_RenderFragment() 返回的字符串 */
void Markdown_FreeHtml(char *html);

#ifdef __cplusplus
}
#endif

#endif  /* MD_PREVIEW_MARKDOWN_H */
