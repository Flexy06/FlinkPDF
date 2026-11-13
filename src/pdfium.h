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
typedef void* OBJ;
typedef void* BOOKMARK;
typedef void* ANNOT;

struct FILEACCESS {
    unsigned long len;
    int (*getBlock)(void* param, unsigned long pos, unsigned char* buf, unsigned long size);
    void* param;
};
struct SIZEF { float w, h; };
struct RECTF { float l, t, r, b; };
struct MATRIX { float a, b, c, d, e, f; };
struct POINTF { float x, y; };
struct FILEWRITE {
    int version;
    int (*WriteBlock)(FILEWRITE* self, const void* data, unsigned long size);
};

enum { BITMAP_BGRx = 3 };
enum { F_ANNOT = 0x01, F_LCD_TEXT = 0x02, F_PRINTING = 0x800 };
enum { ERR_PASSWORD = 4 };
enum { ACTION_GOTO = 1, ACTION_URI = 3 };
enum { OBJ_IMAGE = 3, OBJ_FORM = 5 };
enum { ANNOT_INK = 15, ANNOT_FLAG_PRINT = 4 };
enum { SAVE_INCREMENTAL = 1, SAVE_NO_INCREMENTAL = 2 };
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
    int (*TextCountChars)(pdf::TEXTPG);
    unsigned int (*TextGetUnicode)(pdf::TEXTPG, int);
    int (*TextGetLooseCharBox)(pdf::TEXTPG, int, pdf::RECTF*);
    int (*PageCountObjects)(pdf::PAGE);
    pdf::OBJ (*PageGetObject)(pdf::PAGE, int);
    int (*ObjGetType)(pdf::OBJ);
    int (*ObjGetBounds)(pdf::OBJ, float*, float*, float*, float*);
    int (*ObjGetMatrix)(pdf::OBJ, pdf::MATRIX*);
    int (*FormCountObjects)(pdf::OBJ);
    pdf::OBJ (*FormGetObject)(pdf::OBJ, unsigned long);
    pdf::BOOKMARK (*BookmarkFirstChild)(pdf::DOC, pdf::BOOKMARK);
    pdf::BOOKMARK (*BookmarkNextSibling)(pdf::DOC, pdf::BOOKMARK);
    unsigned long (*BookmarkGetTitle)(pdf::BOOKMARK, void*, unsigned long);
    pdf::DEST (*BookmarkGetDest)(pdf::DOC, pdf::BOOKMARK);
    pdf::ACTION (*BookmarkGetAction)(pdf::BOOKMARK);
    pdf::ANNOT (*PageCreateAnnot)(pdf::PAGE, int);
    int (*AnnotAddInkStroke)(pdf::ANNOT, const pdf::POINTF*, size_t);
    int (*AnnotSetColor)(pdf::ANNOT, int, unsigned int, unsigned int, unsigned int, unsigned int);
    int (*AnnotSetBorder)(pdf::ANNOT, float, float, float);
    int (*AnnotSetRect)(pdf::ANNOT, const pdf::RECTF*);
    int (*AnnotSetFlags)(pdf::ANNOT, int);
    void (*PageCloseAnnot)(pdf::ANNOT);
    int (*PageGetAnnotCount)(pdf::PAGE);
    pdf::ANNOT (*PageGetAnnot)(pdf::PAGE, int);
    int (*AnnotGetSubtype)(pdf::ANNOT);
    unsigned long (*AnnotGetInkListCount)(pdf::ANNOT);
    unsigned long (*AnnotGetInkListPath)(pdf::ANNOT, unsigned long, pdf::POINTF*, unsigned long);
    int (*PageRemoveAnnot)(pdf::PAGE, int);
    int (*DeviceToPage)(pdf::PAGE, int, int, int, int, int, int, int, double*, double*);
    int (*SaveAsCopy)(pdf::DOC, pdf::FILEWRITE*, unsigned long);

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
        ok &= Get(TextCountChars, "FPDFText_CountChars");
        ok &= Get(TextGetUnicode, "FPDFText_GetUnicode");
        ok &= Get(TextGetLooseCharBox, "FPDFText_GetLooseCharBox");
        ok &= Get(PageCountObjects, "FPDFPage_CountObjects");
        ok &= Get(PageGetObject, "FPDFPage_GetObject");
        ok &= Get(ObjGetType, "FPDFPageObj_GetType");
        ok &= Get(ObjGetBounds, "FPDFPageObj_GetBounds");
        ok &= Get(ObjGetMatrix, "FPDFPageObj_GetMatrix");
        ok &= Get(FormCountObjects, "FPDFFormObj_CountObjects");
        ok &= Get(FormGetObject, "FPDFFormObj_GetObject");
        ok &= Get(BookmarkFirstChild, "FPDFBookmark_GetFirstChild");
        ok &= Get(BookmarkNextSibling, "FPDFBookmark_GetNextSibling");
        ok &= Get(BookmarkGetTitle, "FPDFBookmark_GetTitle");
        ok &= Get(BookmarkGetDest, "FPDFBookmark_GetDest");
        ok &= Get(BookmarkGetAction, "FPDFBookmark_GetAction");
        ok &= Get(PageCreateAnnot, "FPDFPage_CreateAnnot");
        ok &= Get(AnnotAddInkStroke, "FPDFAnnot_AddInkStroke");
        ok &= Get(AnnotSetColor, "FPDFAnnot_SetColor");
        ok &= Get(AnnotSetBorder, "FPDFAnnot_SetBorder");
        ok &= Get(AnnotSetRect, "FPDFAnnot_SetRect");
        ok &= Get(AnnotSetFlags, "FPDFAnnot_SetFlags");
        ok &= Get(PageCloseAnnot, "FPDFPage_CloseAnnot");
        ok &= Get(PageGetAnnotCount, "FPDFPage_GetAnnotCount");
        ok &= Get(PageGetAnnot, "FPDFPage_GetAnnot");
        ok &= Get(AnnotGetSubtype, "FPDFAnnot_GetSubtype");
        ok &= Get(AnnotGetInkListCount, "FPDFAnnot_GetInkListCount");
        ok &= Get(AnnotGetInkListPath, "FPDFAnnot_GetInkListPath");
        ok &= Get(PageRemoveAnnot, "FPDFPage_RemoveAnnot");
        ok &= Get(DeviceToPage, "FPDF_DeviceToPage");
        ok &= Get(SaveAsCopy, "FPDF_SaveAsCopy");
        return ok;
    }
};
