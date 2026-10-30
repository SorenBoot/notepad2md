// 「检查更新」纯逻辑单元测试（不依赖 GUI、不联网）
#include <stdio.h>
#include <string.h>

#include "UpdateCheck.h"

static int gFail = 0,gPass = 0;

static void Check(int bOk,const char *pszName)
{
  if (bOk) { gPass++; }
  else { gFail++; printf("  FAIL  %s\n",pszName); }
}

static void CheckStr(const char *pszGot,const char *pszWant,const char *pszName)
{
  if (strcmp(pszGot,pszWant) == 0) { gPass++; return; }
  gFail++;
  printf("  FAIL  %s\n        got  [%s]\n        want [%s]\n",pszName,pszGot,pszWant);
}

static void CheckInt(int iGot,int iWant,const char *pszName)
{
  if (iGot == iWant) { gPass++; return; }
  gFail++;
  printf("  FAIL  %s  got %d want %d\n",pszName,iGot,iWant);
}

int main(void)
{
  char szBuf[2048];
  int i;

  printf("== JSON 字段提取 ==\n");
  {
    const char *pJson =
      "{\"url\":\"https://api.github.com/x\",\"tag_name\":\"v0.0.3\","
      "\"name\":\"0.0.3\",\"body\":\"line1\\nline2\"}";

    CheckInt(UpdateExtractJsonString(pJson,(int)strlen(pJson),"tag_name",szBuf,sizeof(szBuf)),1,"找到 tag_name");
    CheckStr(szBuf,"v0.0.3","tag_name 值");
    CheckInt(UpdateExtractJsonString(pJson,(int)strlen(pJson),"body",szBuf,sizeof(szBuf)),1,"找到 body");
    CheckStr(szBuf,"line1\nline2","body 里的 \\n 还原");

    // 键不存在
    CheckInt(UpdateExtractJsonString(pJson,(int)strlen(pJson),"no_such_key",szBuf,sizeof(szBuf)),0,"键不存在返回 0");
    CheckStr(szBuf,"","键不存在时输出清空");

    // 只看键名，别被 URL 里的 "tag" 字样骗到
    CheckInt(UpdateExtractJsonString(pJson,(int)strlen(pJson),"tag",szBuf,sizeof(szBuf)),0,"不做前缀匹配");
  }

  printf("== JSON 转义 ==\n");
  {
    const char *pJson = "{\"body\":\"a\\\"b\\\\c\\/d\\te\\rf\\bg\"}";
    UpdateExtractJsonString(pJson,(int)strlen(pJson),"body",szBuf,sizeof(szBuf));
    CheckStr(szBuf,"a\"b\\c/d\te\rf\bg","各种单字符转义");
  }
  {
    const char *pJson = "{\"body\":\"\\u4e2d\\u6587\"}";
    UpdateExtractJsonString(pJson,(int)strlen(pJson),"body",szBuf,sizeof(szBuf));
    CheckStr(szBuf,"\xe4\xb8\xad\xe6\x96\x87","\\uXXXX 解成 UTF-8 中文");
  }
  {
    // U+1F600 = \uD83D\uDE00，代理对
    const char *pJson = "{\"body\":\"\\uD83D\\uDE00\"}";
    UpdateExtractJsonString(pJson,(int)strlen(pJson),"body",szBuf,sizeof(szBuf));
    CheckStr(szBuf,"\xf0\x9f\x98\x80","代理对拼成 4 字节");
  }
  {
    const char *pJson = "{\"body\":null,\"tag_name\":\"v9\"}";
    CheckInt(UpdateExtractJsonString(pJson,(int)strlen(pJson),"body",szBuf,sizeof(szBuf)),1,"body 为 null");
    CheckStr(szBuf,"","null 当空串");
    UpdateExtractJsonString(pJson,(int)strlen(pJson),"tag_name",szBuf,sizeof(szBuf));
    CheckStr(szBuf,"v9","null 之后的字段照常解析");
  }
  {
    // 截断的 JSON：不能越界读，也不能崩
    const char *pJson = "{\"tag_name\":\"v0.0";
    UpdateExtractJsonString(pJson,(int)strlen(pJson),"tag_name",szBuf,sizeof(szBuf));
    Check(1,"截断 JSON 不崩");
    const char *pJson2 = "{\"tag_name\":";
    UpdateExtractJsonString(pJson2,(int)strlen(pJson2),"tag_name",szBuf,sizeof(szBuf));
    Check(1,"只有键没有值不崩");
    const char *pJson3 = "";
    CheckInt(UpdateExtractJsonString(pJson3,0,"tag_name",szBuf,sizeof(szBuf)),0,"空输入返回 0");
  }
  {
    // 输出缓冲小：必须截断而不是溢出
    char szSmall[8];
    const char *pJson = "{\"tag_name\":\"v1.2.3.4.5.6\"}";
    UpdateExtractJsonString(pJson,(int)strlen(pJson),"tag_name",szSmall,sizeof(szSmall));
    Check(strlen(szSmall) < sizeof(szSmall),"小缓冲不溢出");
  }

  printf("== 版本比较 ==\n");
  {
    static const struct { const char *a,*b; int want; const char *name; } c[] = {
      { "0.0.3","0.0.2", 1, "0.0.3 > 0.0.2" },
      { "0.0.2","0.0.3",-1, "0.0.2 < 0.0.3" },
      { "0.0.2","0.0.2", 0, "相等" },
      { "v0.0.3","0.0.2", 1, "v 前缀" },
      { "V1.0.0","v1.0.0",0, "大小写 v 都忽略" },
      { "1.0","1.0.0", 0, "段数不等视为相同" },
      { "1.0.0","1.0", 0, "段数不等（反向）" },
      { "0.10","0.9", 1, "数字比大小不是字符串比" },
      { "1.2.3","1.2.10",-1, "多段各自比较" },
      { "10.0","9.99", 1, "每段独立进位" },
      { "0.0.0.1","0.0.0", 1, "第四段" },
      { "","0.0.1",-1, "空版本当 0" },
    };
    for (i = 0; i < (int)(sizeof(c)/sizeof(c[0])); i++) {
      int r = UpdateVersionCmp(c[i].a,c[i].b);
      int norm = (r > 0) ? 1 : ((r < 0) ? -1 : 0);
      CheckInt(norm,c[i].want,c[i].name);
    }
  }

  printf("== 从 URL 取 tag ==\n");
  {
    CheckInt(UpdateExtractTagFromUrl("https://github.com/SorenBoot/notepad2md/releases/tag/v0.0.2",szBuf,sizeof(szBuf)),1,"标准 Releases 地址");
    CheckStr(szBuf,"v0.0.2","取出 tag");
    UpdateExtractTagFromUrl("https://github.com/x/y/releases/tag/v1.2.3/",szBuf,sizeof(szBuf));
    CheckStr(szBuf,"v1.2.3","尾部斜杠去掉");
    UpdateExtractTagFromUrl("https://github.com/x/y/releases/tag/v1.2.3?x=1",szBuf,sizeof(szBuf));
    CheckStr(szBuf,"v1.2.3","查询串去掉");
    CheckInt(UpdateExtractTagFromUrl("https://github.com/x/y/releases",szBuf,sizeof(szBuf)),0,"没有 /tag/ 返回 0");
    CheckInt(UpdateExtractTagFromUrl("https://github.com/x/y/releases/tag/",szBuf,sizeof(szBuf)),0,"tag 为空返回 0");
    CheckInt(UpdateExtractTagFromUrl("",szBuf,sizeof(szBuf)),0,"空 URL 返回 0");
    CheckInt(UpdateExtractTagFromUrl(NULL,szBuf,sizeof(szBuf)),0,"NULL 返回 0");
    // 多个 /tag/ 时取最后一个（路径前缀里也可能出现）
    UpdateExtractTagFromUrl("https://h/tag/x/github.com/a/b/releases/tag/v3",szBuf,sizeof(szBuf));
    CheckStr(szBuf,"v3","多个 /tag/ 取最后一个");
  }

  printf("== UpdateIsNewer ==\n");
  CheckInt(UpdateIsNewer("0.0.2","v0.0.3"),1,"远端新");
  CheckInt(UpdateIsNewer("0.0.2","v0.0.2"),0,"同版本不算新");
  CheckInt(UpdateIsNewer("0.0.3","v0.0.2"),0,"远端旧不算新");
  CheckInt(UpdateIsNewer("0.0.2",""),0,"远端空串");
  CheckInt(UpdateIsNewer("","v0.0.3"),0,"本地空串");

  printf("== 发布说明压缩 ==\n");
  {
    const char *pNotes =
      "## 新增\n"
      "**检查更新** 功能\n"
      "\n"
      "\n"
      "\n"
      "## 修复\n"
      "- `INI` 段头背景\n";
    int n = UpdateFormatNotes(pNotes,szBuf,sizeof(szBuf),10);
    CheckInt(n,4,"4 行有效内容");
    CheckStr(szBuf,"新增\n检查更新 功能\n\n修复\n- INI 段头背景","去掉 # 与 ** 与 `，空行压缩");
  }
  {
    // 行数超限要截断并加 ...
    const char *pNotes = "l1\nl2\nl3\nl4\nl5\nl6";
    UpdateFormatNotes(pNotes,szBuf,sizeof(szBuf),3);
    Check(strstr(szBuf,"...") != NULL,"超行数加省略号");
    Check(strstr(szBuf,"l4") == NULL,"超出的行不出现");
    Check(strstr(szBuf,"l3") != NULL,"限内的行都在");
  }
  {
    CheckInt(UpdateFormatNotes("",szBuf,sizeof(szBuf),10),0,"空说明返回 0");
    CheckInt(UpdateFormatNotes(NULL,szBuf,sizeof(szBuf),10),0,"NULL 说明返回 0");
    CheckInt(UpdateFormatNotes("\n\n\n",szBuf,sizeof(szBuf),10),0,"只有空行返回 0");
  }
  {
    // 小缓冲：截断，不溢出
    char szSmall[16];
    UpdateFormatNotes("a long line that will not fit at all",szSmall,sizeof(szSmall),10);
    Check(strlen(szSmall) < sizeof(szSmall),"小缓冲不溢出");
  }
  {
    // 行内单独的 * 不该被吃掉
    UpdateFormatNotes("2 * 3 = 6",szBuf,sizeof(szBuf),10);
    CheckStr(szBuf,"2 * 3 = 6","单个星号保留");
  }

  printf("\n通过 %d, 失败 %d\n",gPass,gFail);
  return (gFail == 0) ? 0 : 1;
}
