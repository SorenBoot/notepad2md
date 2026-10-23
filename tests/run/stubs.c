/* MSHTML/IE 的 IID 只在链接进 mshtml 的 GUID 表里；单测只连 RunCmd.o
   里的纯逻辑函数，这里给出最小定义以便链接。 */
#include <windows.h>
#include <ole2.h>
#include <initguid.h>
#include <mshtml.h>
#include <exdisp.h>
