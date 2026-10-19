// Minimal-Bindings für PDFium (dynamisch geladen aus pdfium.dll)
#pragma once
#include <windows.h>

namespace pdf {
typedef void* DOC;
typedef void* PAGE;
typedef void* BMP;
typedef void* LINK;
typedef void* DEST;
typedef void* ACTION;
typedef void* TEXTPG;
typedef void* SCH;

struct FILEACCESS {
    unsigned long len;
    int (*getBlock)(void* param, unsigned long pos, unsigned char* buf, unsigned long size);
    void* param;
};
struct SIZEF { float w, h; };
struct RECTF { float l, t, r, b; };

enum { BITMAP_BGRx = 3 };
enum { F_ANNOT = 0x01, F_LCD_TEXT = 0x02, F_PRINTING = 0x800 };
enum { ERR_PASSWORD = 4 };
enum { ACTION_GOTO = 1, ACTION_URI = 3 };
}

struct Pdfium {
    HMODULE dll = nullptr;
    void (*InitLibrary)();
    pdf::DOC (*LoadCustomDocument)(pdf::FILEACCESS*, const char*);
    unsigned long (*GetLastError)();
    void (*CloseDocument)(pdf::DOC);
    int (*GetPageCount)(pdf::DOC);
    int (*GetPageSizeByIndexF)(pdf::DOC, int, pdf::SIZEF*);
    pdf::PAGE (*LoadPage)(pdf::DOC, int);
    void (*ClosePage)(pdf::PAGE);
    pdf::BMP (*BitmapCreateEx)(int, int, int, void*, int);
    int (*BitmapFillRect)(pdf::BMP, int, int, int, int, unsigned long);
    void (*BitmapDestroy)(pdf::BMP);
    void (*RenderPageBitmap)(pdf::BMP, pdf::PAGE, int, int, int, int, int, int);
    void (*RenderPage)(HDC, pdf::PAGE, int, int, int, int, int, int);
    int (*PageToDevice)(pdf::PAGE, int, int, int, int, int, double, double, int*, int*);
    int (*LinkEnumerate)(pdf::PAGE, int*, pdf::LINK*);
    int (*LinkGetAnnotRect)(pdf::LINK, pdf::RECTF*);
    pdf::DEST (*LinkGetDest)(pdf::DOC, pdf::LINK);
    pdf::ACTION (*LinkGetAction)(pdf::LINK);
    unsigned long (*ActionGetType)(pdf::ACTION);
    pdf::DEST (*ActionGetDest)(pdf::DOC, pdf::ACTION);
    unsigned long (*ActionGetURIPath)(pdf::DOC, pdf::ACTION, void*, unsigned long);
    int (*DestGetDestPageIndex)(pdf::DOC, pdf::DEST);
    int (*DestGetLocationInPage)(pdf::DEST, int*, int*, int*, float*, float*, float*);
    pdf::TEXTPG (*TextLoadPage)(pdf::PAGE);
    void (*TextClosePage)(pdf::TEXTPG);
    pdf::SCH (*TextFindStart)(pdf::TEXTPG, const unsigned short*, unsigned long, int);
    int (*TextFindNext)(pdf::SCH);
    int (*TextGetSchResultIndex)(pdf::SCH);
    int (*TextGetSchCount)(pdf::SCH);
    void (*TextFindClose)(pdf::SCH);
    int (*TextCountRects)(pdf::TEXTPG, int, int);
    int (*TextGetRect)(pdf::TEXTPG, int, double*, double*, double*, double*);

    template <class F> bool Get(F& fn, const char* name) {
        fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
        return fn != nullptr;
    }

    bool Load(const wchar_t* path) {
        dll = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) return false;
        bool ok = true;
        ok &= Get(InitLibrary, "FPDF_InitLibrary");
        ok &= Get(LoadCustomDocument, "FPDF_LoadCustomDocument");
        ok &= Get(GetLastError, "FPDF_GetLastError");
        ok &= Get(CloseDocument, "FPDF_CloseDocument");
        ok &= Get(GetPageCount, "FPDF_GetPageCount");
        ok &= Get(GetPageSizeByIndexF, "FPDF_GetPageSizeByIndexF");
        ok &= Get(LoadPage, "FPDF_LoadPage");
        ok &= Get(ClosePage, "FPDF_ClosePage");
        ok &= Get(BitmapCreateEx, "FPDFBitmap_CreateEx");
        ok &= Get(BitmapFillRect, "FPDFBitmap_FillRect");
        ok &= Get(BitmapDestroy, "FPDFBitmap_Destroy");
        ok &= Get(RenderPageBitmap, "FPDF_RenderPageBitmap");
        ok &= Get(RenderPage, "FPDF_RenderPage");
        ok &= Get(PageToDevice, "FPDF_PageToDevice");
        ok &= Get(LinkEnumerate, "FPDFLink_Enumerate");
        ok &= Get(LinkGetAnnotRect, "FPDFLink_GetAnnotRect");
        ok &= Get(LinkGetDest, "FPDFLink_GetDest");
        ok &= Get(LinkGetAction, "FPDFLink_GetAction");
        ok &= Get(ActionGetType, "FPDFAction_GetType");
        ok &= Get(ActionGetDest, "FPDFAction_GetDest");
        ok &= Get(ActionGetURIPath, "FPDFAction_GetURIPath");
        ok &= Get(DestGetDestPageIndex, "FPDFDest_GetDestPageIndex");
        ok &= Get(DestGetLocationInPage, "FPDFDest_GetLocationInPage");
        ok &= Get(TextLoadPage, "FPDFText_LoadPage");
        ok &= Get(TextClosePage, "FPDFText_ClosePage");
        ok &= Get(TextFindStart, "FPDFText_FindStart");
        ok &= Get(TextFindNext, "FPDFText_FindNext");
        ok &= Get(TextGetSchResultIndex, "FPDFText_GetSchResultIndex");
        ok &= Get(TextGetSchCount, "FPDFText_GetSchCount");
        ok &= Get(TextFindClose, "FPDFText_FindClose");
        ok &= Get(TextCountRects, "FPDFText_CountRects");
        ok &= Get(TextGetRect, "FPDFText_GetRect");
        return ok;
    }
};
