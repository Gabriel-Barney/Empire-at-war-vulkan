#ifndef STUB_WINDOWS_H
#define STUB_WINDOWS_H
#include <stdint.h>
#include <wchar.h>
typedef uint16_t WCHAR;
typedef struct _GUID { uint32_t Data1; uint16_t Data2; uint16_t Data3; unsigned char Data4[8]; } GUID;
typedef GUID IID;
typedef int32_t HRESULT;
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_POINTER ((HRESULT)0x80004003)
#define E_ABORT ((HRESULT)0x80004004)
#define E_FAIL ((HRESULT)0x80004005)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002)
#define DXGI_ERROR_MORE_DATA ((HRESULT)0x887A0003)
#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004)
#define SUCCEEDED(x) ((HRESULT)(x)>=0)
#define FAILED(x) ((HRESULT)(x)<0)
#define DXGI_ERROR_ALREADY_EXISTS ((HRESULT)0x887A0036)
#define D3DCOLOR_ARGB(a,r,g,b) ((uint32_t)((((a)&0xff)<<24)|(((r)&0xff)<<16)|(((g)&0xff)<<8)|((b)&0xff)))
#include <stddef.h>
#define CONTAINING_RECORD(address, type, field) ((type *)((char *)(address) - offsetof(type, field)))
#define D3DPS_VERSION(major, minor) (0xFFFF0000 | ((major) << 8) | (minor))
#define D3DVS_VERSION(major, minor) (0xFFFE0000 | ((major) << 8) | (minor))
#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif
typedef int32_t INT; typedef float FLOAT;
#endif
