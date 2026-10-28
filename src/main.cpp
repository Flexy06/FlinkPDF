// FlinkPDF – ein schneller, schlanker PDF-Viewer für Windows
// Win32 + Direct2D/DirectWrite, Rendering über PDFium in einem Hintergrund-Thread.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pdfium.h"
#include "system.h"

// ============================================================================
//  Konstanten
// ============================================================================
#define WM_APP_RENDERED (WM_APP + 1)
#define WM_APP_SEARCH (WM_APP + 2)
#define WM_APP_TOAST (WM_APP + 3)
#define WM_APP_TEXT (WM_APP + 4)

enum { TIMER_ANIM = 1, TIMER_ZOOM = 2, TIMER_SEARCH = 3, TIMER_TOAST = 4, TIMER_CARET = 5, TIMER_PANEL = 6, TIMER_AUTOSCROLL = 7 };

enum Cmd {
    CMD_NONE = 0, CMD_OPEN, CMD_PREV, CMD_NEXT, CMD_PAGEBOX, CMD_ZOOMOUT, CMD_ZOOMIN, CMD_ZOOMBOX, CMD_FIT,
    CMD_SEARCH, CMD_PRINT, CMD_MENU, CMD_SETDEFAULT, CMD_SEARCHFIELD, CMD_SEARCH_PREV, CMD_SEARCH_NEXT,
    CMD_SEARCH_CLOSE, CMD_PWFIELD, CMD_PWOK, CMD_PWCANCEL, CMD_SETTINGS, CMD_TOC, CMD_TOC_CLOSE, CMD_TOC_PANEL,
    CMD_RECENT = 100,  // + Index
    // Einstellungen
    CMD_SET_PANEL = 200, CMD_SET_CLOSE, CMD_SET_MICA, CMD_SET_NIGHT, CMD_SET_SMOOTH, CMD_SET_INVH, CMD_SET_INVV,
    CMD_SET_REMEMBER, CMD_SET_CLEARRECENT, CMD_SET_UNREG, CMD_SET_DIM, CMD_SET_TOCAUTO,
    CMD_SET_THEME = 230,  // + 0..2
    CMD_SET_SPEED = 240,  // + 0..2
    CMD_SET_OPEN = 250,   // + 0..2
    CMD_SET_PCOLOR = 260, // + 0..2
    CMD_SET_MOUSE = 270,  // + 0..1
    CMD_TOC_ROW = 20000,  // + sichtbare Zeile
    CMD_TOC_CHEV = 60000, // + sichtbare Zeile (Auf-/Zuklappen)
    // Menübefehle
    MNU_DEFAULT = 1000, MNU_FULLSCREEN, MNU_LOCATION, MNU_UNREGISTER, MNU_ABOUT, MNU_OPENWITH, MNU_SETTINGS, MNU_COPY,
    MNU_SELALL, MNU_SEARCHSEL, MNU_TOC,
    MNU_ZOOM = 1100,  // + Prozent
    MNU_FITWIDTH = 1099, MNU_FITPAGE = 1098,
};

enum ZoomMode { ZM_CUSTOM = 0, ZM_WIDTH = 1, ZM_PAGE = 2 };
enum Field { FLD_NONE, FLD_PAGE, FLD_SEARCH, FLD_PASSWORD };

static const float GAP = 14.0f;     // Abstand zwischen Seiten (DIP)
static const float TOOLBAR = 48.0f;  // Höhe der Werkzeugleiste (DIP)
static const float PANEL_W = 380.0f;  // Breite des Einstellungs-Panels
static const float TOC_W = 280.0f;    // Breite der Inhaltsverzeichnis-Seitenleiste
static const float SB_W = 14.0f;     // Breite der Scrollbar-Zone
static const double kZoomSteps[] = {0.1, 0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25,
                                    1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0};

// ============================================================================
//  Theme
// ============================================================================
struct Theme {
    UINT32 bg, bar, barLine, text, dim, accent, accentText, field, fieldLine, card, cardLine, toast, toastText;
    float hover, press, shadow;
};
static const Theme kDark = {0x1C1C1C, 0x2B2B2B, 0x1A1A1A, 0xF3F3F3, 0x9E9E9E, 0x4CC2FF, 0x002A40, 0x1F1F1F, 0x4A4A4A,
                            0x2C2C2C, 0x3F3F3F, 0x3A3A3A, 0xFFFFFF, 0.08f, 0.05f, 0.55f};
static const Theme kLight = {0xE4E4E4, 0xF7F7F7, 0xDADADA, 0x1B1B1B, 0x5E5E5E, 0x005FB8, 0xFFFFFF, 0xFFFFFF, 0xBDBDBD,
                             0xFFFFFF, 0xD6D6D6, 0x2B2B2B, 0xFFFFFF, 0.06f, 0.10f, 0.22f};

// ============================================================================
//  Datenstrukturen
// ============================================================================
struct LinkInfo {
    D2D1_RECT_F r;  // normiert 0..1
    int page = -1;  // Sprungziel (oder -1)
    float y = -1;   // normierte Position im Ziel (oder -1)
    std::wstring uri;
};

// Ein Zeichen der Textebene; r normiert (0..1), bei erzeugten Zeichen (Zeilenumbruch, Leerzeichen) leer
struct CharBox {
    D2D1_RECT_F r;
    unsigned int ch;
};

struct PageInfo {
    float w = 612, h = 792;  // in Punkt
    ID2D1Bitmap* bmp = nullptr;
    float bmpScale = 0;
    int bmpW = 0, bmpH = 0;
    bool linksLoaded = false;
    std::vector<LinkInfo> links;
    bool textLoaded = false;
    std::vector<CharBox> chars;  // Zeichen für die Textauswahl (erst bei Bedarf geladen)
};

struct Job {
    int gen = -1, page = -1;
    float scale = 0;
    int w = 0, h = 0;
    bool links = false;
    int color = 0;  // Seitenfarbe: 0 normal, 1 Sepia, 2 dunkel
};

struct TextResult {
    int gen, page;
    std::vector<CharBox> chars;
};

struct RenderResult {
    int gen, page;
    float scale;
    int w, h;
    std::vector<unsigned char> px;
    bool hasLinks = false;
    std::vector<LinkInfo> links;
    int color = 0;
};

struct SearchHit {
    int page;
    std::vector<D2D1_RECT_F> rects;  // normiert
};

struct SearchResult {
    int gen, id, page;
    bool done;
    std::vector<SearchHit> hits;
};

struct Zone {
    D2D1_RECT_F r;
    int cmd;
    const wchar_t* tip;
};

// Einstellungen (in HKCU\Software\FlinkPDF gespeichert)
struct Settings {
    int theme = 0;           // 0 = wie Windows, 1 = hell, 2 = dunkel
    bool mica = true;        // Windows 11: Mica + integrierte Titelleiste
    int pageColor = 0;       // Seitenfarbe: 0 normal, 1 Sepia, 2 dunkel (Bilder bleiben unverändert)
    int mouseMode = 0;       // linke Maustaste: 0 = Text auswählen (auf Text), 1 = immer verschieben
    bool tocAuto = false;    // Inhaltsverzeichnis beim Öffnen anzeigen
    bool smooth = true;      // sanftes Scrollen
    int speed = 1;           // 0 langsam, 1 normal, 2 schnell
    bool invH = false;       // horizontale Scrollrichtung umkehren
    bool invV = false;       // vertikale Scrollrichtung umkehren
    int openZoom = 0;        // 0 = zuletzt verwendet, 1 = Seitenbreite, 2 = ganze Seite
    bool remember = true;    // Leseposition je Datei merken

    void Load() {
        theme = (int)std::min<DWORD>(2, sys::ReadDw(L"Theme", 0));
        mica = sys::ReadDw(L"Mica", 1) != 0;
        pageColor = (int)std::min<DWORD>(2, sys::ReadDw(L"PageColor", sys::ReadDw(L"Night", 0) ? 2 : 0));
        mouseMode = (int)std::min<DWORD>(1, sys::ReadDw(L"MouseMode", 0));
        tocAuto = sys::ReadDw(L"TocAuto", 0) != 0;
        smooth = sys::ReadDw(L"Smooth", 1) != 0;
        speed = (int)std::min<DWORD>(2, sys::ReadDw(L"Speed", 1));
        invH = sys::ReadDw(L"InvertH", 0) != 0;
        invV = sys::ReadDw(L"InvertV", 0) != 0;
        openZoom = (int)std::min<DWORD>(2, sys::ReadDw(L"OpenZoom", 0));
        remember = sys::ReadDw(L"Remember", 1) != 0;
    }
    void Save() const {
        sys::WriteDw(L"Theme", theme);
        sys::WriteDw(L"Mica", mica);
        sys::WriteDw(L"PageColor", pageColor);
        sys::WriteDw(L"MouseMode", mouseMode);
        sys::WriteDw(L"TocAuto", tocAuto);
        sys::WriteDw(L"Smooth", smooth);
        sys::WriteDw(L"Speed", speed);
        sys::WriteDw(L"InvertH", invH);
        sys::WriteDw(L"InvertV", invV);
        sys::WriteDw(L"OpenZoom", openZoom);
        sys::WriteDw(L"Remember", remember);
    }
};

// ============================================================================
//  Globaler Zustand
// ============================================================================
static Pdfium P;
static std::mutex g_pdfMx;  // schützt alle PDFium-Aufrufe sowie g_doc/g_docGen
static pdf::DOC g_doc = nullptr;
static int g_docGen = 0;

struct FileReader {
    HANDLE h = INVALID_HANDLE_VALUE;
    unsigned long size = 0;
};
static FileReader g_reader;
static pdf::FILEACCESS g_access;

static int GetBlock(void* param, unsigned long pos, unsigned char* buf, unsigned long size) {
    FileReader* r = (FileReader*)param;
    unsigned long done = 0;
    while (done < size) {
        OVERLAPPED ov{};
        ov.Offset = pos + done;
        DWORD got = 0;
        if (!ReadFile(r->h, buf + done, size - done, &got, &ov) || got == 0) return 0;
        done += got;
    }
    return 1;
}

static HWND g_hwnd;
static HWND g_tip;
static float g_dpi = 96;
static bool g_dark = true;
static Theme T = kDark;
static bool g_isDefault = false;
static bool g_installed = false;

// Direct2D
static ID2D1Factory* g_fac;
static IDWriteFactory* g_dw;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_br;
static ID2D1LinearGradientBrush* g_grad;
static ID2D1PathGeometry *g_logoPage, *g_logoFold, *g_logoBolt;
static IDWriteTextFormat *fUi, *fUiC, *fUiR, *fUiEll, *fBold, *fSmall, *fSmallC, *fIcon, *fIconBig, *fTitle, *fHead, *fSection,
    *fSeg, *fItem, *fIconSm;
static UINT32 g_maxBmp = 8192;

// Dokument
static std::wstring g_path;
static std::vector<PageInfo> g_pages;
static std::vector<double> g_prefix;  // kumulierte Seitenhöhen in Punkt
static float g_maxW = 612;
static float g_fitW = 612;  // typische Seitenbreite (Median) für "Seitenbreite"
static bool g_hasDoc = false;

// Ansicht
static double g_zoom = 1.0;
static int g_zoomMode = ZM_WIDTH;
static double g_sx = 0, g_sy = 0, g_tx = 0, g_ty = 0;  // Scroll (aktuell/Ziel)
static bool g_animating = false;
static LARGE_INTEGER g_animLast;
static bool g_zoomSettling = false;
static float g_cw = 800, g_ch = 600;  // Clientgröße in DIP
static bool g_fullscreen = false;
static WINDOWPLACEMENT g_fsPrev{sizeof(WINDOWPLACEMENT)};

// Interaktion
static std::vector<Zone> g_zones;
static int g_hot = CMD_NONE, g_pressed = CMD_NONE;
static bool g_tracking = false;
static bool g_panning = false, g_panMoved = false;
static POINT g_panStart;
static double g_panSx, g_panSy;
static bool g_sbHover = false, g_sbDrag = false;
static float g_sbGrab = 0;
static float g_gestureDist = 0;
static POINT g_gestureLast;
static const wchar_t* g_curTip = nullptr;

// Textfelder
static int g_focus = FLD_NONE;
static std::wstring g_pageText, g_searchText, g_pwText;
static bool g_selAll = false, g_caretOn = true;

// Suche
static bool g_searchOpen = false;
static int g_searchId = 0;
static bool g_searchDone = true;
static std::wstring g_searchQuery;
static std::vector<SearchHit> g_hits;
static int g_curHit = -1;

// Passwort
static std::wstring g_pwPath;
static bool g_pwWrong = false;

// Sonstiges
static std::vector<std::wstring> g_recent;
static std::wstring g_toast;

// Einstellungen & Fensterrahmen
static Settings S;
static bool g_isWin11 = false;
static bool g_customFrame = false;   // Mica + Werkzeugleiste in der Titelleiste
static float g_captionLeft = 1e9f;   // linke Kante der System-Fensterknöpfe (Client-DIP)
static D2D1_RECT_F g_capRect{0, 0, 0, 0};  // Bereich der System-Fensterknöpfe (Client-DIP)
static int g_ncPressed = 0;          // gedrückter Fensterknopf (HTCLOSE/HTMINBUTTON/HTMAXBUTTON)
static bool g_panelOpen = false;     // Einstellungs-Panel
static float g_panelT = 0;           // Animation 0..1
static bool g_panelWant = false;     // Ziel der Animation (offen/zu)
static LARGE_INTEGER g_panelLast;
static float g_panelScroll = 0, g_panelContentH = 0;
static const size_t kRecentMax = 30;   // gespeichert (für Lesepositionen); angezeigt werden max. 8

// Textauswahl
struct TextPos {
    int page = 0, idx = 0;  // Einfügeposition vor Zeichen idx
};
static bool operator<(const TextPos& a, const TextPos& b) { return a.page < b.page || (a.page == b.page && a.idx < b.idx); }
static TextPos g_selA, g_selB;
static bool g_selActive = false, g_selecting = false;
static float g_selMx = 0, g_selMy = 0;

// Inhaltsverzeichnis (Lesezeichen des PDFs)
struct TocItem {
    std::wstring title;
    int page = -1;
    float y = -1;
    int depth = 0;
    bool kids = false, open = false;
};
static std::vector<TocItem> g_toc;
static std::vector<int> g_tocVis;  // sichtbare Einträge (Indizes in g_toc)
static bool g_tocLoaded = false, g_tocOpen = false;
static float g_tocScroll = 0;

template <class T> static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// ============================================================================
//  Render-Worker (Hintergrund-Thread)
// ============================================================================
static void ExtractLinks(pdf::PAGE pg, const Job& job, std::vector<LinkInfo>& out, float ptW, float ptH) {
    const int SX = std::max(1, (int)lround(ptW * 8)), SY = std::max(1, (int)lround(ptH * 8));
    int pos = 0;
    pdf::LINK link = nullptr;
    while (P.LinkEnumerate(pg, &pos, &link)) {
        pdf::RECTF rc;
        if (!P.LinkGetAnnotRect(link, &rc)) continue;
        int x1, y1, x2, y2;
        P.PageToDevice(pg, 0, 0, SX, SY, 0, rc.l, rc.t, &x1, &y1);
        P.PageToDevice(pg, 0, 0, SX, SY, 0, rc.r, rc.b, &x2, &y2);
        LinkInfo li;
        li.r = D2D1::RectF(std::min(x1, x2) / (float)SX, std::min(y1, y2) / (float)SY, std::max(x1, x2) / (float)SX,
                           std::max(y1, y2) / (float)SY);
        pdf::DEST d = P.LinkGetDest(g_doc, link);
        if (!d) {
            pdf::ACTION a = P.LinkGetAction(link);
            if (a) {
                unsigned long t = P.ActionGetType(a);
                if (t == pdf::ACTION_GOTO) d = P.ActionGetDest(g_doc, a);
                else if (t == pdf::ACTION_URI) {
                    unsigned long n = P.ActionGetURIPath(g_doc, a, nullptr, 0);
                    if (n > 1 && n < 8192) {
                        std::string s(n, '\0');
                        P.ActionGetURIPath(g_doc, a, &s[0], n);
                        s.resize(strlen(s.c_str()));
                        li.uri.assign(s.begin(), s.end());
                    }
                }
            }
        }
        if (d) {
            li.page = P.DestGetDestPageIndex(g_doc, d);
            int hasX = 0, hasY = 0, hasZ = 0;
            float x = 0, y = 0, z = 0;
            if (li.page >= 0 && P.DestGetLocationInPage(d, &hasX, &hasY, &hasZ, &x, &y, &z) && hasY) {
                pdf::SIZEF sz{};
                if (P.GetPageSizeByIndexF(g_doc, li.page, &sz) && sz.h > 0) li.y = std::clamp(1.0f - y / sz.h, 0.0f, 1.0f);
            }
        }
        if (li.page >= 0 || !li.uri.empty()) out.push_back(li);
    }
    (void)job;
}

// --- Seitenfarben: Bildbereiche ermitteln (inkl. Bilder in Form-XObjects) -------------------------
static pdf::MATRIX MulMatrix(const pdf::MATRIX& m, const pdf::MATRIX& f) {  // erst f, dann m
    return {m.a * f.a + m.c * f.b, m.b * f.a + m.d * f.b, m.a * f.c + m.c * f.d, m.b * f.c + m.d * f.d,
            m.a * f.e + m.c * f.f + m.e, m.b * f.e + m.d * f.f + m.f};
}

static void WalkImages(pdf::PAGE pg, pdf::OBJ o, const pdf::MATRIX& m, int w, int h, std::vector<RECT>& out, int depth) {
    int type = P.ObjGetType(o);
    if (type == pdf::OBJ_IMAGE) {
        float l, b, r, t;
        if (!P.ObjGetBounds(o, &l, &b, &r, &t)) return;
        float xs[4] = {l, r, l, r}, ys[4] = {b, b, t, t};
        int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
        for (int k = 0; k < 4; k++) {
            double px = m.a * xs[k] + m.c * ys[k] + m.e, py = m.b * xs[k] + m.d * ys[k] + m.f;
            int dx, dy;
            P.PageToDevice(pg, 0, 0, w, h, 0, px, py, &dx, &dy);
            x0 = std::min(x0, dx); x1 = std::max(x1, dx);
            y0 = std::min(y0, dy); y1 = std::max(y1, dy);
        }
        RECT rc{std::max(0, x0), std::max(0, y0), std::min(w, x1), std::min(h, y1)};
        if (rc.right - rc.left >= 2 && rc.bottom - rc.top >= 2) out.push_back(rc);
    } else if (type == pdf::OBJ_FORM && depth < 4) {
        pdf::MATRIX fm{1, 0, 0, 1, 0, 0};
        P.ObjGetMatrix(o, &fm);
        pdf::MATRIX cm = MulMatrix(m, fm);
        int n = P.FormCountObjects(o);
        for (int i = 0; i < n && out.size() < 512; i++) WalkImages(pg, P.FormGetObject(o, (unsigned long)i), cm, w, h, out, depth + 1);
    }
}

static void CollectImageRects(pdf::PAGE pg, int w, int h, std::vector<RECT>& out) {
    const pdf::MATRIX id{1, 0, 0, 1, 0, 0};
    int n = P.PageCountObjects(pg);
    for (int i = 0; i < n && out.size() < 512; i++) WalkImages(pg, P.PageGetObject(pg, i), id, w, h, out, 0);
}

// Färbt Text und Grafik um (Tabelle pro Kanal) – Bilder werden vorher gesichert und danach zurückkopiert
static void ApplyPageColor(RenderResult& res, const std::vector<RECT>& imgs) {
    static unsigned char lut[3][3][256];  // [Modus][Kanal B,G,R][Wert]
    static bool init = false;
    if (!init) {
        const int ink[2][3] = {{0x22, 0x2F, 0x3B}, {0xE4, 0xE4, 0xE4}};    // Sepia: Tinte, Dunkel: Schrift (B,G,R)
        const int paper[2][3] = {{0xD8, 0xEC, 0xF4}, {0x1E, 0x1E, 0x1E}};  // Sepia: Papier, Dunkel: Hintergrund
        for (int mode = 1; mode <= 2; mode++)
            for (int c = 0; c < 3; c++)
                for (int v = 0; v < 256; v++)
                    lut[mode][c][v] = (unsigned char)(ink[mode - 1][c] + (paper[mode - 1][c] - ink[mode - 1][c]) * v / 255);
        init = true;
    }
    const int mode = std::clamp(res.color, 1, 2);
    const int stride = res.w * 4;
    std::vector<std::vector<unsigned char>> saved;
    for (const RECT& r : imgs) {
        std::vector<unsigned char> buf((size_t)(r.right - r.left) * 4 * (r.bottom - r.top));
        for (int y = r.top; y < r.bottom; y++)
            memcpy(&buf[(size_t)(y - r.top) * (r.right - r.left) * 4], &res.px[(size_t)y * stride + r.left * 4], (r.right - r.left) * 4);
        saved.push_back(std::move(buf));
    }
    unsigned char* d = res.px.data();
    const unsigned char *lb = lut[mode][0], *lg = lut[mode][1], *lr = lut[mode][2];
    for (size_t k = 0, n = res.px.size(); k < n; k += 4) {
        d[k] = lb[d[k]];
        d[k + 1] = lg[d[k + 1]];
        d[k + 2] = lr[d[k + 2]];
    }
    for (size_t i = 0; i < imgs.size(); i++) {
        const RECT& r = imgs[i];
        for (int y = r.top; y < r.bottom; y++)
            memcpy(&res.px[(size_t)y * stride + r.left * 4], &saved[i][(size_t)(y - r.top) * (r.right - r.left) * 4], (r.right - r.left) * 4);
    }
}

// --- Textebene einer Seite laden (Aufrufer hält g_pdfMx) ------------------------------------------
static bool ExtractText(int page, std::vector<CharBox>& out) {
    pdf::PAGE pg = P.LoadPage(g_doc, page);
    if (!pg) return false;
    pdf::SIZEF sz{612, 792};
    P.GetPageSizeByIndexF(g_doc, page, &sz);
    const int SX = std::max(1, (int)lround(sz.w * 8)), SY = std::max(1, (int)lround(sz.h * 8));
    pdf::TEXTPG tp = P.TextLoadPage(pg);
    if (tp) {
        int n = P.TextCountChars(tp);
        out.reserve(std::max(0, n));
        for (int i = 0; i < n; i++) {
            CharBox c{D2D1::RectF(0, 0, 0, 0), P.TextGetUnicode(tp, i)};
            pdf::RECTF rc;
            if (P.TextGetLooseCharBox(tp, i, &rc)) {
                int x1, y1, x2, y2;
                P.PageToDevice(pg, 0, 0, SX, SY, 0, rc.l, rc.t, &x1, &y1);
                P.PageToDevice(pg, 0, 0, SX, SY, 0, rc.r, rc.b, &x2, &y2);
                c.r = D2D1::RectF(std::min(x1, x2) / (float)SX, std::min(y1, y2) / (float)SY, std::max(x1, x2) / (float)SX,
                                  std::max(y1, y2) / (float)SY);
            }
            out.push_back(c);
        }
        P.TextClosePage(tp);
    }
    P.ClosePage(pg);
    return true;
}

class Worker {
    std::mutex mx;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::deque<std::pair<int, int>> textJobs;  // (gen, page)
    std::pair<int, int> textInflight{-1, -1};
    Job inflight;
    bool quit = false;
    // Suche
    bool sActive = false;
    int sGen = 0, sId = 0, sStart = 0, sN = 0, sIdx = 0;
    std::wstring sQuery;
    std::vector<float> pw, ph;
    std::thread th;
    HWND hwnd = nullptr;

  public:
    void Start(HWND h) {
        hwnd = h;
        th = std::thread([this] { Run(); });
    }
    void Stop() {
        {
            std::lock_guard<std::mutex> lk(mx);
            quit = true;
        }
        cv.notify_all();
        if (th.joinable()) th.join();
    }
    void SetJobs(const std::vector<Job>& v) {
        {
            std::lock_guard<std::mutex> lk(mx);
            jobs.clear();
            for (auto& j : v) {
                if (j.page == inflight.page && j.gen == inflight.gen && j.scale == inflight.scale && j.color == inflight.color) continue;
                jobs.push_back(j);
            }
        }
        cv.notify_all();
    }
    void StartSearch(int gen, int id, const std::wstring& q, int start, int n) {
        {
            std::lock_guard<std::mutex> lk(mx);
            sActive = n > 0 && !q.empty();
            sGen = gen; sId = id; sQuery = q; sStart = start; sN = n; sIdx = 0;
        }
        cv.notify_all();
    }
    void StopSearch() {
        std::lock_guard<std::mutex> lk(mx);
        sActive = false;
    }
    // Textebene im Leerlauf laden (niedrigere Priorität als das Rendern)
    void SetTextJobs(int gen, const std::vector<int>& pages) {
        {
            std::lock_guard<std::mutex> lk(mx);
            textJobs.clear();
            for (int p : pages)
                if (!(gen == textInflight.first && p == textInflight.second)) textJobs.push_back({gen, p});
        }
        cv.notify_all();
    }

  private:
    void Run() {
        for (;;) {
            Job job;
            bool doSearch = false, doText = false, last = false;
            int page = 0, gen = 0, id = 0;
            std::wstring q;
            {
                std::unique_lock<std::mutex> lk(mx);
                cv.wait(lk, [&] { return quit || !jobs.empty() || !textJobs.empty() || sActive; });
                if (quit) return;
                if (!jobs.empty()) {
                    job = jobs.front();
                    jobs.pop_front();
                    inflight = job;
                } else if (!textJobs.empty()) {
                    doText = true;
                    gen = textJobs.front().first;
                    page = textJobs.front().second;
                    textJobs.pop_front();
                    textInflight = {gen, page};
                } else {
                    doSearch = true;
                    page = (sStart + sIdx) % sN;
                    sIdx++;
                    last = sIdx >= sN;
                    if (last) sActive = false;
                    gen = sGen; id = sId; q = sQuery;
                }
            }
            if (doText) {
                auto* res = new TextResult{gen, page, {}};
                bool ok;
                {
                    std::lock_guard<std::mutex> lk(g_pdfMx);
                    ok = g_doc && g_docGen == gen && ExtractText(page, res->chars);
                }
                if (!ok || !PostMessageW(hwnd, WM_APP_TEXT, 0, (LPARAM)res)) delete res;
                std::lock_guard<std::mutex> lk(mx);
                textInflight = {-1, -1};
            } else if (!doSearch) {
                Render(job);
                std::lock_guard<std::mutex> lk(mx);
                inflight = Job();
            } else {
                Search(gen, id, page, q, last);
            }
        }
    }

    void Render(const Job& job) {
        RenderResult* res = new RenderResult{job.gen, job.page, job.scale, job.w, job.h, {}, false, {}, job.color};
        try {
            res->px.resize((size_t)job.w * job.h * 4);
        } catch (...) {
            delete res;
            return;
        }
        std::vector<RECT> imgs;  // Bildbereiche in Pixeln (bleiben bei Sepia/dunkel unverändert)
        {
            std::lock_guard<std::mutex> lk(g_pdfMx);
            if (!g_doc || g_docGen != job.gen) { delete res; return; }
            pdf::PAGE pg = P.LoadPage(g_doc, job.page);
            pdf::BMP b = P.BitmapCreateEx(job.w, job.h, pdf::BITMAP_BGRx, res->px.data(), job.w * 4);
            if (b) {
                P.BitmapFillRect(b, 0, 0, job.w, job.h, 0xFFFFFFFF);
                if (pg) P.RenderPageBitmap(b, pg, 0, 0, job.w, job.h, 0, pdf::F_ANNOT);
                P.BitmapDestroy(b);
            }
            if (pg && job.color) CollectImageRects(pg, job.w, job.h, imgs);
            if (pg && job.links) {
                pdf::SIZEF sz{612, 792};
                P.GetPageSizeByIndexF(g_doc, job.page, &sz);
                ExtractLinks(pg, job, res->links, sz.w, sz.h);
            }
            res->hasLinks = job.links;
            if (pg) P.ClosePage(pg);
        }
        if (job.color) ApplyPageColor(*res, imgs);  // außerhalb der PDFium-Sperre
        if (!PostMessageW(hwnd, WM_APP_RENDERED, 0, (LPARAM)res)) delete res;
    }

    void Search(int gen, int id, int page, const std::wstring& q, bool last) {
        SearchResult* res = new SearchResult{gen, id, page, last, {}};
        {
            std::lock_guard<std::mutex> lk(g_pdfMx);
            if (!g_doc || g_docGen != gen) { delete res; return; }
            pdf::PAGE pg = P.LoadPage(g_doc, page);
            if (pg) {
                pdf::SIZEF sz{612, 792};
                P.GetPageSizeByIndexF(g_doc, page, &sz);
                const int SX = std::max(1, (int)lround(sz.w * 8)), SY = std::max(1, (int)lround(sz.h * 8));
                pdf::TEXTPG tp = P.TextLoadPage(pg);
                if (tp) {
                    pdf::SCH sch = P.TextFindStart(tp, reinterpret_cast<const unsigned short*>(q.c_str()), 0, 0);
                    if (sch) {
                        while (P.TextFindNext(sch)) {
                            int idx = P.TextGetSchResultIndex(sch), cnt = P.TextGetSchCount(sch);
                            int nr = P.TextCountRects(tp, idx, cnt);
                            SearchHit hit{page, {}};
                            for (int r = 0; r < nr; r++) {
                                double l, t, rr, b;
                                if (!P.TextGetRect(tp, r, &l, &t, &rr, &b)) continue;
                                int x1, y1, x2, y2;
                                P.PageToDevice(pg, 0, 0, SX, SY, 0, l, t, &x1, &y1);
                                P.PageToDevice(pg, 0, 0, SX, SY, 0, rr, b, &x2, &y2);
                                hit.rects.push_back(D2D1::RectF(std::min(x1, x2) / (float)SX, std::min(y1, y2) / (float)SY,
                                                                std::max(x1, x2) / (float)SX, std::max(y1, y2) / (float)SY));
                            }
                            if (!hit.rects.empty()) res->hits.push_back(std::move(hit));
                        }
                        P.TextFindClose(sch);
                    }
                    P.TextClosePage(tp);
                }
                P.ClosePage(pg);
            }
        }
        if (!PostMessageW(hwnd, WM_APP_SEARCH, 0, (LPARAM)res)) delete res;
    }
};
static Worker g_worker;

// ============================================================================
//  Hilfsfunktionen
// ============================================================================
static float TB() { return g_fullscreen ? 0.0f : TOOLBAR; }
static float ContentX0() { return (g_hasDoc && g_tocOpen && !g_fullscreen) ? std::min(TOC_W, g_cw * 0.45f) : 0.0f; }
static float ViewW() { return std::max(1.0f, g_cw - ContentX0()); }
static float CenterX() { return ContentX0() + ViewW() / 2; }
static float ViewH() { return std::max(1.0f, g_ch - TB()); }
static double K() { return g_zoom * 96.0 / 72.0; }  // DIP pro Punkt
static int N() { return (int)g_pages.size(); }
static double PageTop(int i) { return GAP * (i + 1) + g_prefix[i] * K(); }
static double TotalH() { return GAP * (N() + 1) + g_prefix[N()] * K(); }
static double ContentW() { return std::max<double>(ViewW(), g_maxW * K() + 2 * GAP); }
static double MaxSY() { return std::max(0.0, TotalH() - ViewH()); }
static double MaxSX() { return std::max(0.0, ContentW() - ViewW()); }
static float Snap(double v) { double s = g_dpi / 96.0; return (float)(std::round(v * s) / s); }

static D2D1_RECT_F PageRect(int i) {
    double w = g_pages[i].w * K(), h = g_pages[i].h * K();
    float x = Snap(ContentX0() + (ContentW() - w) / 2 - g_sx);
    float y = Snap(TB() + PageTop(i) - g_sy);
    return D2D1::RectF(x, y, x + (float)w, y + (float)h);
}

static bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

static int PageAtDocY(double y) {
    int lo = 0, hi = N() - 1, ans = 0;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (PageTop(m) <= y) { ans = m; lo = m + 1; } else hi = m - 1;
    }
    return ans;
}

static void VisibleRange(int& a, int& b) {
    a = PageAtDocY(g_sy);
    b = PageAtDocY(g_sy + ViewH());
}

static int CurrentPage() {
    if (!g_hasDoc) return 0;
    if (g_sy >= MaxSY() - 1 && MaxSY() > 0) {
        int a, b;
        VisibleRange(a, b);
        return b;
    }
    return PageAtDocY(g_sy + ViewH() * 0.3);
}

static void Invalidate() { InvalidateRect(g_hwnd, nullptr, FALSE); }

static std::wstring Fmt(const wchar_t* f, ...) {
    wchar_t b[512];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf(b, 511, f, ap);
    va_end(ap);
    b[511] = 0;
    return b;
}

static void Toast(const std::wstring& s, UINT ms = 3200) {
    g_toast = s;
    SetTimer(g_hwnd, TIMER_TOAST, ms, nullptr);
    Invalidate();
}

static float PxToDip(int v) { return v * 96.0f / g_dpi; }
static int DipToPx(float v) { return (int)std::lround(v * g_dpi / 96.0f); }

static void UpdateClientSize() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_cw = PxToDip(rc.right);
    g_ch = PxToDip(rc.bottom);
}

// ============================================================================
//  Rendering-Aufträge
// ============================================================================
static float CapScale(int i, double s) {
    const PageInfo& p = g_pages[i];
    double lim = std::min<double>(8192, g_maxBmp);
    double c = std::min({s, lim / p.w, lim / p.h, std::sqrt(24e6 / (p.w * (double)p.h))});
    return (float)c;
}

static void UpdateJobs() {
    if (!g_hasDoc) return;
    int a, b;
    VisibleRange(a, b);
    double s = K() * g_dpi / 96.0;
    double mid = g_sy + ViewH() / 2;
    std::vector<int> order;
    for (int i = a; i <= b; i++) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int x, int y) {
        double cx = std::fabs(PageTop(x) + g_pages[x].h * K() / 2 - mid);
        double cy = std::fabs(PageTop(y) + g_pages[y].h * K() / 2 - mid);
        return cx < cy;
    });
    if (b + 1 < N()) order.push_back(b + 1);
    if (a - 1 >= 0) order.push_back(a - 1);
    if (b + 2 < N()) order.push_back(b + 2);

    std::vector<Job> jobs;
    for (int i : order) {
        PageInfo& p = g_pages[i];
        float sc = CapScale(i, s);
        bool need = !p.bmp || (!g_zoomSettling && std::fabs(p.bmpScale - sc) > 1e-4f);
        if (!need) continue;
        Job j;
        j.gen = g_docGen;
        j.page = i;
        j.scale = sc;
        j.w = std::max(1, (int)std::lround(p.w * sc));
        j.h = std::max(1, (int)std::lround(p.h * sc));
        j.links = !p.linksLoaded;
        j.color = S.pageColor;
        jobs.push_back(j);
    }
    g_worker.SetJobs(jobs);
    if (S.mouseMode == 0) {  // Textebene der sichtbaren Seiten im Leerlauf vorladen (für Cursor & Auswahl)
        std::vector<int> tj;
        for (int i = a; i <= b; i++)
            if (!g_pages[i].textLoaded) tj.push_back(i);
        g_worker.SetTextJobs(g_docGen, tj);
    }

    // Speicher freigeben: alles außerhalb des Sichtbereichs ±3 Seiten
    for (int i = 0; i < N(); i++) {
        if ((i < a - 3 || i > b + 3) && g_pages[i].bmp) {
            SafeRelease(g_pages[i].bmp);
            g_pages[i].bmpScale = 0;
        }
    }
}

// ============================================================================
//  Scrollen & Zoomen
// ============================================================================
static void ClampScroll() {
    g_sy = std::clamp(g_sy, 0.0, MaxSY());
    g_sx = std::clamp(g_sx, 0.0, MaxSX());
    g_ty = std::clamp(g_ty, 0.0, MaxSY());
    g_tx = std::clamp(g_tx, 0.0, MaxSX());
}

static void StopAnim() {
    g_animating = false;
    KillTimer(g_hwnd, TIMER_ANIM);
    g_tx = g_sx;
    g_ty = g_sy;
}

static void StartAnim() {
    if (!g_animating) {
        g_animating = true;
        QueryPerformanceCounter(&g_animLast);
        SetTimer(g_hwnd, TIMER_ANIM, 8, nullptr);
    }
}

static void ScrollTo(double x, double y, bool anim) {
    if (anim) {
        g_tx = x; g_ty = y;
        ClampScroll();
        StartAnim();
    } else {
        StopAnim();
        g_sx = g_tx = x;
        g_sy = g_ty = y;
        ClampScroll();
    }
    UpdateJobs();
    Invalidate();
}

static void ScrollBy(double dx, double dy, bool anim) {
    double bx = g_animating ? g_tx : g_sx, by = g_animating ? g_ty : g_sy;
    ScrollTo(bx + dx, by + dy, anim);
}

static void GoToPage(int i, float yNorm = -1) {
    if (!g_hasDoc) return;
    i = std::clamp(i, 0, N() - 1);
    double t = PageTop(i) - GAP;
    if (yNorm >= 0) t = PageTop(i) + yNorm * g_pages[i].h * K() - 24;
    bool anim = S.smooth && std::fabs(t - g_sy) < ViewH() * 2.5;
    ScrollTo(g_sx, t, anim);
}

static double FitZoom(int mode) {
    double aw = std::max(50.0, (double)ViewW() - 2 * GAP - 6);
    double zw = aw / (g_fitW * 96.0 / 72.0);
    if (mode == ZM_WIDTH) return std::clamp(zw, 0.1, 6.0);
    int c = g_hasDoc ? CurrentPage() : 0;
    double ph = g_hasDoc ? g_pages[c].h : 792;
    double zh = std::max(50.0, (double)ViewH() - 2 * GAP) / (ph * 96.0 / 72.0);
    return std::clamp(std::min(zw, zh), 0.1, 6.0);
}

static void SetZoom(double z, float ax, float ay, int mode = ZM_CUSTOM) {
    if (!g_hasDoc) return;
    z = std::clamp(z, 0.1, 6.0);
    g_zoomMode = mode;
    StopAnim();
    double oldK = K();
    double vy = ay - TB();
    double docY = g_sy + vy;
    int i = PageAtDocY(docY);
    double off = (docY - GAP * (i + 1) - g_prefix[i] * oldK) / oldK;
    double cx = g_sx + (ax - ContentX0()) - ContentW() / 2;
    double rel = cx / oldK;
    g_zoom = z;
    double k = K();
    g_sy = GAP * (i + 1) + g_prefix[i] * k + off * k - vy;
    g_sx = rel * k + ContentW() / 2 - (ax - ContentX0());
    g_tx = g_sx; g_ty = g_sy;
    ClampScroll();
    g_zoomSettling = true;
    SetTimer(g_hwnd, TIMER_ZOOM, 140, nullptr);
    UpdateJobs();
    Invalidate();
}

static void ApplyFit(int mode) {
    if (!g_hasDoc) { g_zoomMode = mode; return; }
    int c = CurrentPage();
    SetZoom(FitZoom(mode), CenterX(), TB(), mode);
    if (mode == ZM_PAGE) ScrollTo(g_sx, PageTop(c) - GAP, false);
    sys::WriteDw(L"ZoomMode", mode);
}

static void ZoomStep(int dir) {
    double z = g_zoom, nz = z;
    if (dir > 0) {
        for (double s : kZoomSteps) if (s > z + 0.001) { nz = s; break; }
    } else {
        for (int i = (int)(sizeof kZoomSteps / sizeof kZoomSteps[0]) - 1; i >= 0; i--)
            if (kZoomSteps[i] < z - 0.001) { nz = kZoomSteps[i]; break; }
    }
    SetZoom(nz, CenterX(), TB() + ViewH() / 2);
    sys::WriteDw(L"ZoomMode", ZM_CUSTOM);
    sys::WriteDw(L"Zoom", (DWORD)std::lround(g_zoom * 100));
}

// ============================================================================
//  Dokument öffnen/schließen
// ============================================================================
static void ReleaseBitmaps() {
    for (auto& p : g_pages) { SafeRelease(p.bmp); p.bmpScale = 0; }
}

static const wchar_t* kPosKey = L"Software\\FlinkPDF\\Positions";
struct SavedPos {
    int page;
    float frac;
};

static void SavePosition() {
    if (!g_hasDoc || !S.remember || g_path.empty()) return;
    int i = PageAtDocY(g_sy);
    SavedPos v{i, (float)((g_sy - PageTop(i)) / std::max(1.0, g_pages[i].h * K()))};
    RegSetKeyValueW(HKEY_CURRENT_USER, kPosKey, g_path.c_str(), REG_BINARY, &v, sizeof v);
}

static bool LoadPosition(const std::wstring& path, SavedPos& v) {
    DWORD sz = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, kPosKey, path.c_str(), RRF_RT_REG_BINARY, nullptr, &v, &sz) == ERROR_SUCCESS &&
           sz == sizeof v;
}

static void CloseDocument() {
    SavePosition();
    g_worker.SetJobs({});
    g_worker.StopSearch();
    {
        std::lock_guard<std::mutex> lk(g_pdfMx);
        if (g_doc) P.CloseDocument(g_doc);
        g_doc = nullptr;
        g_docGen++;
        if (g_reader.h != INVALID_HANDLE_VALUE) CloseHandle(g_reader.h);
        g_reader.h = INVALID_HANDLE_VALUE;
    }
    ReleaseBitmaps();
    g_pages.clear();
    g_prefix.assign(1, 0.0);
    g_hasDoc = false;
    g_hits.clear();
    g_curHit = -1;
    g_searchDone = true;
    g_path.clear();
    g_selActive = g_selecting = false;
    g_toc.clear();
    g_tocVis.clear();
    g_tocLoaded = false;
    g_tocScroll = 0;
    SetWindowTextW(g_hwnd, L"FlinkPDF");
}

static void AddRecent(const std::wstring& path) {
    g_recent.erase(std::remove_if(g_recent.begin(), g_recent.end(),
                                  [&](const std::wstring& s) { return _wcsicmp(s.c_str(), path.c_str()) == 0; }),
                   g_recent.end());
    g_recent.insert(g_recent.begin(), path);
    while (g_recent.size() > kRecentMax) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kPosKey, g_recent.back().c_str());
        g_recent.pop_back();
    }
    sys::SaveRecent(g_recent);
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static void LoadToc();

static bool OpenDocument(const std::wstring& pathIn, const std::wstring* password = nullptr) {
    std::wstring path = pathIn;
    {
        DWORD n = GetFullPathNameW(pathIn.c_str(), 0, nullptr, nullptr);
        if (n) {
            std::wstring full(n, L'\0');
            DWORD m = GetFullPathNameW(pathIn.c_str(), n, &full[0], nullptr);
            if (m && m < n) { full.resize(m); path = full; }
        }
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Toast(L"Die Datei konnte nicht geöffnet werden.");
        return false;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 0xFFFFFFF0LL) {
        CloseHandle(h);
        Toast(L"Die Datei ist zu groß.");
        return false;
    }
    CloseDocument();
    g_pwPath.clear();
    std::string pw = password ? ToUtf8(*password) : std::string();
    unsigned long err = 0;
    std::vector<pdf::SIZEF> sizes;
    {
        std::lock_guard<std::mutex> lk(g_pdfMx);
        g_reader.h = h;
        g_reader.size = (unsigned long)sz.QuadPart;
        g_access.len = g_reader.size;
        g_access.getBlock = GetBlock;
        g_access.param = &g_reader;
        g_doc = P.LoadCustomDocument(&g_access, password ? pw.c_str() : nullptr);
        if (!g_doc) {
            err = P.GetLastError();
            CloseHandle(h);
            g_reader.h = INVALID_HANDLE_VALUE;
        } else {
            int n = P.GetPageCount(g_doc);
            sizes.resize(std::max(0, n));
            for (int i = 0; i < n; i++) {
                if (!P.GetPageSizeByIndexF(g_doc, i, &sizes[i]) || sizes[i].w < 1 || sizes[i].h < 1) sizes[i] = {612, 792};
            }
            g_docGen++;
        }
    }
    if (!g_doc) {
        if (err == pdf::ERR_PASSWORD) {
            g_pwPath = path;
            g_pwWrong = password != nullptr;
            g_pwText.clear();
            g_focus = FLD_PASSWORD;
            g_selAll = false;
            SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
            Invalidate();
        } else {
            Toast(L"Keine gültige oder eine beschädigte PDF-Datei.");
        }
        return false;
    }
    if (sizes.empty()) sizes.push_back({612, 792});  // sollte nicht vorkommen
    g_pages.assign(sizes.size(), PageInfo());
    g_prefix.assign(sizes.size() + 1, 0.0);
    g_maxW = 1;
    for (size_t i = 0; i < sizes.size(); i++) {
        g_pages[i].w = sizes[i].w;
        g_pages[i].h = sizes[i].h;
        g_prefix[i + 1] = g_prefix[i] + sizes[i].h;
        g_maxW = std::max(g_maxW, sizes[i].w);
    }
    {
        std::vector<float> ws;
        for (auto& sz : sizes) ws.push_back(sz.w);
        std::nth_element(ws.begin(), ws.begin() + ws.size() / 2, ws.end());
        g_fitW = ws[ws.size() / 2];
    }
    g_hasDoc = true;
    g_path = path;
    if (g_focus == FLD_PASSWORD) g_focus = FLD_NONE;
    UpdateClientSize();
    StopAnim();
    g_sx = g_sy = g_tx = g_ty = 0;
    if (S.tocAuto || g_tocOpen) {
        LoadToc();
        if (S.tocAuto) g_tocOpen = !g_toc.empty();
    }
    if (S.openZoom == 1) g_zoomMode = ZM_WIDTH;
    else if (S.openZoom == 2) g_zoomMode = ZM_PAGE;
    g_zoom = g_zoomMode == ZM_CUSTOM ? sys::ReadDw(L"Zoom", 100) / 100.0 : FitZoom(g_zoomMode);
    g_zoom = std::clamp(g_zoom, 0.1, 6.0);
    g_sx = g_tx = MaxSX() / 2;
    SavedPos pos;
    if (S.remember && LoadPosition(path, pos) && pos.page >= 0 && pos.page < N()) {
        g_sy = g_ty = PageTop(pos.page) + std::clamp(pos.frac, -0.2f, 1.0f) * g_pages[pos.page].h * K();
        ClampScroll();
    }
    std::wstring title = sys::FileOf(path) + L" – FlinkPDF";
    SetWindowTextW(g_hwnd, title.c_str());
    AddRecent(path);
    if (g_searchOpen && !g_searchText.empty()) SetTimer(g_hwnd, TIMER_SEARCH, 10, nullptr);
    UpdateJobs();
    Invalidate();
    return true;
}

static void ShowOpenDialog() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg)))
        return;
    COMDLG_FILTERSPEC spec[] = {{L"PDF-Dokumente", L"*.pdf"}, {L"Alle Dateien", L"*.*"}};
    dlg->SetFileTypes(2, spec);
    dlg->SetTitle(L"PDF öffnen");
    if (!g_path.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(sys::DirOf(g_path).c_str(), nullptr, IID_IShellItem, (void**)&folder))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    if (dlg->Show(g_hwnd) == S_OK) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                OpenDocument(p);
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    dlg->Release();
}

// ============================================================================
//  Suche
// ============================================================================
static void StartSearch() {
    g_searchId++;
    g_hits.clear();
    g_curHit = -1;
    g_searchQuery = g_searchText;
    if (!g_hasDoc || g_searchQuery.empty()) {
        g_worker.StopSearch();
        g_searchDone = true;
        Invalidate();
        return;
    }
    g_searchDone = false;
    g_worker.StartSearch(g_docGen, g_searchId, g_searchQuery, CurrentPage(), N());
    Invalidate();
}

static void RevealHit(int idx) {
    if (idx < 0 || idx >= (int)g_hits.size()) return;
    const SearchHit& h = g_hits[idx];
    D2D1_RECT_F r = h.rects[0];
    const PageInfo& p = g_pages[h.page];
    double top = PageTop(h.page) + r.top * p.h * K();
    double bot = PageTop(h.page) + r.bottom * p.h * K();
    double ty = g_sy, tx = g_sx;
    if (top < g_sy + 8 || bot > g_sy + ViewH() - 8) ty = top - ViewH() * 0.35;
    double pageLeft = (ContentW() - p.w * K()) / 2;
    double l = pageLeft + r.left * p.w * K(), rr = pageLeft + r.right * p.w * K();
    if (l < g_sx || rr > g_sx + ViewW()) tx = l - ViewW() * 0.3;
    ScrollTo(tx, ty, S.smooth && std::fabs(ty - g_sy) < ViewH() * 3);
}

static void NextHit(int dir) {
    if (g_hits.empty()) return;
    int n = (int)g_hits.size();
    g_curHit = g_curHit < 0 ? 0 : (g_curHit + dir + n) % n;
    RevealHit(g_curHit);
    Invalidate();
}

static void OpenSearch() {
    if (!g_hasDoc) return;
    g_searchOpen = true;
    g_focus = FLD_SEARCH;
    g_selAll = !g_searchText.empty();
    g_caretOn = true;
    SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
    Invalidate();
}

static void CloseSearch() {
    g_searchOpen = false;
    if (g_focus == FLD_SEARCH) g_focus = FLD_NONE;
    g_worker.StopSearch();
    g_hits.clear();
    g_curHit = -1;
    g_searchId++;
    g_searchDone = true;
    Invalidate();
}

// ============================================================================
//  Textauswahl
// ============================================================================
static void EnsureText(int page) {
    PageInfo& p = g_pages[page];
    if (p.textLoaded) return;
    std::vector<CharBox> chars;
    {
        std::lock_guard<std::mutex> lk(g_pdfMx);
        if (g_doc) ExtractText(page, chars);
    }
    p.chars = std::move(chars);
    p.textLoaded = true;
}

static int PageAtPoint(float x, float y) {
    if (!g_hasDoc || y < TB() || x < ContentX0()) return -1;
    int a, b;
    VisibleRange(a, b);
    for (int i = a; i <= b; i++)
        if (In(PageRect(i), x, y)) return i;
    return -1;
}

// Einfügeposition (vor Zeichen i) für einen Punkt; onText = Punkt liegt auf einem Zeichen
static int CaretAt(int page, float x, float y, bool* onText, int* charIdx = nullptr) {
    PageInfo& p = g_pages[page];
    if (onText) *onText = false;
    if (charIdx) *charIdx = -1;
    if (!p.textLoaded) return 0;
    D2D1_RECT_F r = PageRect(page);
    float nx = (x - r.left) / std::max(1.0f, r.right - r.left), ny = (y - r.top) / std::max(1.0f, r.bottom - r.top);
    int best = -1;
    float bestScore = 1e30f;
    for (int i = 0; i < (int)p.chars.size(); i++) {
        const D2D1_RECT_F& c = p.chars[i].r;
        if (c.right <= c.left) continue;
        float dx = (nx < c.left ? c.left - nx : nx > c.right ? nx - c.right : 0) * p.w;
        float dy = (ny < c.top ? c.top - ny : ny > c.bottom ? ny - c.bottom : 0) * p.h;
        float score = dy * 8 + dx;  // gleiche Zeile bevorzugen
        if (score < bestScore) {
            bestScore = score;
            best = i;
            if (score == 0) break;
        }
    }
    if (best < 0) return 0;
    if (onText) *onText = bestScore < 1.5f;
    if (charIdx) *charIdx = best;
    const D2D1_RECT_F& c = p.chars[best].r;
    return nx > (c.left + c.right) / 2 ? best + 1 : best;
}

static void SelOrdered(TextPos& a, TextPos& b) {
    a = g_selA;
    b = g_selB;
    if (b < a) std::swap(a, b);
}

static void UpdateSelectionFromMouse() {
    float x = g_selMx, y = std::clamp(g_selMy, TB() + 1, g_ch - 1);
    int pg = PageAtPoint(std::max(x, ContentX0() + 1), y);
    if (pg < 0) pg = std::clamp(PageAtDocY(g_sy + (y - TB())), 0, N() - 1);
    EnsureText(pg);
    TextPos np{pg, CaretAt(pg, x, y, nullptr)};
    if (np.page != g_selB.page || np.idx != g_selB.idx) {
        g_selB = np;
        Invalidate();
    }
    g_selActive = g_selA.page != g_selB.page || g_selA.idx != g_selB.idx;
}

static bool IsWordChar(unsigned int cp) {
    return cp == '_' || (cp < 0x10000 && IsCharAlphaNumericW((WCHAR)cp)) || (cp >= 0x10000);
}

static void SelectWordAt(int page, float x, float y) {
    EnsureText(page);
    int ci = -1;
    bool on = false;
    CaretAt(page, x, y, &on, &ci);
    if (!on || ci < 0) return;
    const auto& ch = g_pages[page].chars;
    int a = ci, b = ci + 1;
    if (IsWordChar(ch[ci].ch)) {
        while (a > 0 && IsWordChar(ch[a - 1].ch)) a--;
        while (b < (int)ch.size() && IsWordChar(ch[b].ch)) b++;
    }
    g_selA = {page, a};
    g_selB = {page, b};
    g_selActive = true;
    Invalidate();
}

static void SelectAll() {
    if (!g_hasDoc) return;
    g_selA = {0, 0};
    g_selB = {N() - 1, INT_MAX / 2};
    g_selActive = true;
    Invalidate();
}

static std::wstring SelectionText(size_t limit = SIZE_MAX) {
    std::wstring out;
    if (!g_selActive) return out;
    TextPos a, b;
    SelOrdered(a, b);
    for (int p = a.page; p <= b.page && out.size() < limit; p++) {
        EnsureText(p);
        const auto& ch = g_pages[p].chars;
        int n = (int)ch.size();
        int s = p == a.page ? std::min(a.idx, n) : 0, e = p == b.page ? std::min(b.idx, n) : n;
        for (int k = s; k < e; k++) {
            unsigned int cp = ch[k].ch;
            if (cp == 0x2) cp = '-';  // weiches Trennzeichen am Zeilenende
            if (cp == 0 || cp == 0xFFFE || cp == 0xFFFF || (cp < 32 && cp != '\r' && cp != '\n' && cp != '\t')) continue;
            if (cp < 0x10000) out.push_back((wchar_t)cp);
            else {
                cp -= 0x10000;
                out.push_back((wchar_t)(0xD800 + (cp >> 10)));
                out.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
            }
        }
        if (p != b.page) out += L"\r\n";
    }
    return out;
}

static void CopySelection() {
    std::wstring t = SelectionText();
    if (t.empty() || !OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (hg) {
        memcpy(GlobalLock(hg), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(hg);
        if (!SetClipboardData(CF_UNICODETEXT, hg)) GlobalFree(hg);
    }
    CloseClipboard();
    Toast(L"Text kopiert", 1400);
}

// ============================================================================
//  Inhaltsverzeichnis
// ============================================================================
static void RebuildTocVis() {
    g_tocVis.clear();
    int skipDepth = INT_MAX;
    for (int i = 0; i < (int)g_toc.size(); i++) {
        const TocItem& t = g_toc[i];
        if (t.depth > skipDepth) continue;
        skipDepth = INT_MAX;
        g_tocVis.push_back(i);
        if (t.kids && !t.open) skipDepth = t.depth;
    }
}

static void LoadToc() {
    g_toc.clear();
    g_tocLoaded = true;
    if (!g_hasDoc) return;
    std::lock_guard<std::mutex> lk(g_pdfMx);
    if (!g_doc) return;
    struct Frame {
        pdf::BOOKMARK bm;
        int depth;
    };
    std::vector<Frame> stack;
    std::vector<void*> seen;
    pdf::BOOKMARK first = P.BookmarkFirstChild(g_doc, nullptr);
    if (first) stack.push_back({first, 0});
    while (!stack.empty() && g_toc.size() < 5000) {
        Frame f = stack.back();
        stack.pop_back();
        if (std::find(seen.begin(), seen.end(), f.bm) != seen.end()) continue;  // Schutz vor Zyklen
        seen.push_back(f.bm);
        TocItem it;
        it.depth = f.depth;
        unsigned long n = P.BookmarkGetTitle(f.bm, nullptr, 0);
        if (n > 2 && n < 4096) {
            std::wstring t(n / 2, L'\0');
            P.BookmarkGetTitle(f.bm, &t[0], n);
            t.resize(wcslen(t.c_str()));
            for (auto& c : t)
                if (c < 32) c = L' ';
            it.title = t;
        }
        pdf::DEST d = P.BookmarkGetDest(g_doc, f.bm);
        if (!d) {
            pdf::ACTION a = P.BookmarkGetAction(f.bm);
            if (a && P.ActionGetType(a) == pdf::ACTION_GOTO) d = P.ActionGetDest(g_doc, a);
        }
        if (d) {
            it.page = P.DestGetDestPageIndex(g_doc, d);
            int hx = 0, hy = 0, hz = 0;
            float x = 0, y = 0, z = 0;
            if (it.page >= 0 && it.page < N() && P.DestGetLocationInPage(d, &hx, &hy, &hz, &x, &y, &z) && hy)
                it.y = std::clamp(1.0f - y / g_pages[it.page].h, 0.0f, 1.0f);
        }
        pdf::BOOKMARK child = f.depth < 12 ? P.BookmarkFirstChild(g_doc, f.bm) : nullptr;
        it.kids = child != nullptr;
        g_toc.push_back(it);
        pdf::BOOKMARK next = P.BookmarkNextSibling(g_doc, f.bm);
        if (next) stack.push_back({next, f.depth});
        if (child) stack.push_back({child, f.depth + 1});  // Kind zuerst (Tiefensuche)
    }
    int top = 0;
    for (auto& t : g_toc) top += t.depth == 0;
    if (top == 1)
        for (auto& t : g_toc)
            if (t.depth == 0) t.open = true;  // einzelnes Wurzelelement direkt aufklappen
    RebuildTocVis();
}

static void SetTocOpen(bool open) {
    if (!g_hasDoc) return;
    if (open && !g_tocLoaded) LoadToc();
    g_tocOpen = open;
    if (g_zoomMode != ZM_CUSTOM) SetZoom(FitZoom(g_zoomMode), CenterX(), TB(), g_zoomMode);
    else {
        ClampScroll();
        UpdateJobs();
    }
    Invalidate();
}

// ============================================================================
//  Drucken
// ============================================================================
static void PrintDocument() {
    if (!g_hasDoc) return;
    PRINTPAGERANGE ranges[16] = {};
    PRINTDLGEXW pd{};
    pd.lStructSize = sizeof pd;
    pd.hwndOwner = g_hwnd;
    pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOCURRENTPAGE | PD_USEDEVMODECOPIESANDCOLLATE;
    pd.nPageRanges = 0;
    pd.nMaxPageRanges = 16;
    pd.lpPageRanges = ranges;
    pd.nMinPage = 1;
    pd.nMaxPage = (DWORD)N();
    pd.nCopies = 1;
    pd.nStartPage = START_PAGE_GENERAL;
    HRESULT hr = PrintDlgExW(&pd);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
    if (FAILED(hr) || pd.dwResultAction != PD_RESULT_PRINT || !pd.hDC) {
        if (pd.hDC) DeleteDC(pd.hDC);
        return;
    }
    std::vector<int> pages;
    if (pd.Flags & PD_PAGENUMS) {
        for (DWORD r = 0; r < pd.nPageRanges; r++)
            for (DWORD p = ranges[r].nFromPage; p <= ranges[r].nToPage && p <= (DWORD)N(); p++)
                if (p >= 1) pages.push_back((int)p - 1);
    } else {
        for (int i = 0; i < N(); i++) pages.push_back(i);
    }
    HDC dc = pd.hDC;
    int gen = g_docGen;
    std::wstring name = sys::FileOf(g_path);
    HWND hwnd = g_hwnd;
    Toast(L"Wird gedruckt …");
    std::thread([dc, pages, gen, name, hwnd] {
        DOCINFOW di{sizeof di};
        di.lpszDocName = name.c_str();
        bool ok = StartDocW(dc, &di) > 0;
        int pw = GetDeviceCaps(dc, HORZRES), ph = GetDeviceCaps(dc, VERTRES);
        for (size_t k = 0; ok && k < pages.size(); k++) {
            if (StartPage(dc) <= 0) { ok = false; break; }
            {
                std::lock_guard<std::mutex> lk(g_pdfMx);
                if (!g_doc || g_docGen != gen) { ok = false; }
                else {
                    pdf::PAGE pg = P.LoadPage(g_doc, pages[k]);
                    pdf::SIZEF sz{612, 792};
                    P.GetPageSizeByIndexF(g_doc, pages[k], &sz);
                    if (pg) {
                        bool rot = (sz.w > sz.h) != (pw > ph);
                        double w = rot ? sz.h : sz.w, h = rot ? sz.w : sz.h;
                        double s = std::min(pw / w, ph / h);
                        int dw = (int)(w * s), dh = (int)(h * s);
                        P.RenderPage(dc, pg, (pw - dw) / 2, (ph - dh) / 2, dw, dh, rot ? 1 : 0, pdf::F_ANNOT | pdf::F_PRINTING);
                        P.ClosePage(pg);
                    }
                }
            }
            if (EndPage(dc) <= 0) ok = false;
        }
        if (ok) EndDoc(dc); else AbortDoc(dc);
        DeleteDC(dc);
        auto* msg = new std::wstring(ok ? L"An den Drucker gesendet." : L"Drucken abgebrochen.");
        if (!PostMessageW(hwnd, WM_APP_TOAST, 0, (LPARAM)msg)) delete msg;
    }).detach();
}

// ============================================================================
//  Direct2D-Ressourcen
// ============================================================================
static ID2D1PathGeometry* Poly(std::initializer_list<D2D1_POINT_2F> pts) {
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(g_fac->CreatePathGeometry(&g))) return nullptr;
    ID2D1GeometrySink* s = nullptr;
    g->Open(&s);
    auto it = pts.begin();
    s->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
    for (++it; it != pts.end(); ++it) s->AddLine(*it);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    s->Release();
    return g;
}

static bool HasFont(const wchar_t* name) {
    IDWriteFontCollection* c = nullptr;
    if (FAILED(g_dw->GetSystemFontCollection(&c, FALSE))) return false;
    UINT32 i = 0;
    BOOL ex = FALSE;
    c->FindFamilyName(name, &i, &ex);
    c->Release();
    return ex;
}

static IDWriteTextFormat* MkFmt(const wchar_t* fam, float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a,
                                bool ellipsis = false) {
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"de-de", &f);
    if (!f) return nullptr;
    f->SetTextAlignment(a);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis) {
        DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        IDWriteInlineObject* sign = nullptr;
        g_dw->CreateEllipsisTrimmingSign(f, &sign);
        f->SetTrimming(&t, sign);
        SafeRelease(sign);
    }
    return f;
}

static void CreateDeviceIndependent() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)&g_fac);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    const wchar_t* ui = HasFont(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* disp = HasFont(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : ui;
    const wchar_t* icon = HasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    fUi = MkFmt(ui, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    fUiC = MkFmt(ui, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fUiR = MkFmt(ui, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fUiEll = MkFmt(ui, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    fBold = MkFmt(ui, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    fSmall = MkFmt(ui, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    fSmallC = MkFmt(ui, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fIcon = MkFmt(icon, 15.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fIconBig = MkFmt(icon, 18.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fTitle = MkFmt(disp, 30.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    fHead = MkFmt(disp, 20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    fSection = MkFmt(ui, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    fSeg = MkFmt(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fItem = MkFmt(ui, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    fIconSm = MkFmt(icon, 10.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    // Logo (100×100-Box)
    g_logoPage = Poly({{29, 17}, {59, 17}, {74, 32}, {74, 83}, {29, 83}});
    g_logoFold = Poly({{59, 17}, {74, 32}, {59, 32}});
    g_logoBolt = Poly({{56, 28}, {39, 55}, {50.5f, 55}, {45, 74}, {63, 45}, {51.5f, 45}, {58.5f, 28}});
}

static void DiscardDevice() {
    ReleaseBitmaps();
    SafeRelease(g_grad);
    SafeRelease(g_br);
    SafeRelease(g_rt);
}

static bool EnsureRT() {
    if (g_rt) return true;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties();
    rp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);  // für Mica-Transparenz
    rp.dpiX = rp.dpiY = g_dpi;
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp =
        D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)));
    if (FAILED(g_fac->CreateHwndRenderTarget(rp, hp, &g_rt))) return false;
    g_rt->SetDpi(g_dpi, g_dpi);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &g_br);
    D2D1_GRADIENT_STOP st[2] = {{0.0f, D2D1::ColorF(1.0f, 0.43f, 0.24f, 1)}, {1.0f, D2D1::ColorF(0.88f, 0.12f, 0.29f, 1)}};
    ID2D1GradientStopCollection* coll = nullptr;
    g_rt->CreateGradientStopCollection(st, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &coll);
    if (coll) {
        g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(100, 100)),
                                        D2D1::BrushProperties(), coll, &g_grad);
        coll->Release();
    }
    g_maxBmp = g_rt->GetMaximumBitmapSize();
    UpdateJobs();
    return true;
}

// ============================================================================
//  Zeichnen
// ============================================================================
static D2D1_COLOR_F Col(UINT32 rgb, float a = 1) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}
static void SetC(UINT32 rgb, float a = 1) { g_br->SetColor(Col(rgb, a)); }
static void Fill(const D2D1_RECT_F& r, UINT32 c, float a = 1) { SetC(c, a); g_rt->FillRectangle(r, g_br); }
static void FillR(const D2D1_RECT_F& r, float rad, UINT32 c, float a = 1) {
    SetC(c, a);
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_br);
}
static void StrokeR(const D2D1_RECT_F& r, float rad, UINT32 c, float a = 1, float w = 1) {
    SetC(c, a);
    D2D1_RECT_F rr = D2D1::RectF(r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(rr, rad, rad), g_br, w);
}
static void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, UINT32 c, float a = 1) {
    if (!f || s.empty()) return;
    SetC(c, a);
    g_rt->DrawText(s.c_str(), (UINT32)s.size(), f, &r, g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}
static float Measure(const std::wstring& s, IDWriteTextFormat* f) {
    IDWriteTextLayout* l = nullptr;
    if (!f || FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000, 100, &l))) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}

static void AddZone(const D2D1_RECT_F& r, int cmd, const wchar_t* tip) { g_zones.push_back({r, cmd, tip}); }

static void IconBtn(const D2D1_RECT_F& r, int cmd, wchar_t glyph, const wchar_t* tip, bool enabled = true,
                    bool active = false) {
    if (enabled) {
        AddZone(r, cmd, tip);
        if (active) FillR(r, 5, T.text, T.hover * 1.4f);
        if (g_hot == cmd) FillR(r, 5, T.text, g_pressed == cmd ? T.press : T.hover);
    }
    wchar_t s[2] = {glyph, 0};
    Text(s, fIcon, r, T.text, enabled ? 1.0f : 0.35f);
}

static void DrawShadow(const D2D1_RECT_F& r, float rad, float strength) {
    for (int i = 1; i <= 4; i++) {
        D2D1_RECT_F s = D2D1::RectF(r.left - i, r.top - i + 1.5f, r.right + i, r.bottom + i + 1.5f);
        FillR(s, rad + i, 0x000000, strength * 0.22f / i);
    }
}

static void DrawLogo(float x, float y, float size) {
    float s = size / 100.0f;
    g_rt->SetTransform(D2D1::Matrix3x2F(s, 0, 0, s, x, y));
    if (g_grad) g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, 100, 100), 23, 23), g_grad);
    SetC(0xFFFFFF);
    if (g_logoPage) g_rt->FillGeometry(g_logoPage, g_br);
    SetC(0xFFD2C6);
    if (g_logoFold) g_rt->FillGeometry(g_logoFold, g_br);
    if (g_logoBolt && g_grad) g_rt->FillGeometry(g_logoBolt, g_grad);
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

static void DrawField(const D2D1_RECT_F& r, const std::wstring& text, const wchar_t* placeholder, bool focused,
                      IDWriteTextFormat* f, bool center, bool mask = false) {
    std::wstring shown = mask ? std::wstring(text.size(), L'\x2022') : text;
    float tw = Measure(shown, f);
    float x0 = center ? (r.left + r.right) / 2 - tw / 2 : r.left;
    D2D1_RECT_F tr = D2D1::RectF(x0, r.top, std::max(x0 + tw + 2, r.right), r.bottom);
    if (focused && g_selAll && !shown.empty())
        FillR(D2D1::RectF(x0 - 1, r.top + 4, x0 + tw + 1, r.bottom - 4), 2, T.accent, 0.35f);
    if (shown.empty() && placeholder) {
        Text(placeholder, center ? fUiC : fUi, r, T.dim);
    } else {
        g_rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        Text(shown, center ? fUi : f, tr, T.text);
        g_rt->PopAxisAlignedClip();
    }
    if (focused && g_caretOn && !g_selAll) {
        float cx = std::min(x0 + tw + 1, r.right - 1);
        if (shown.empty() && center) cx = (r.left + r.right) / 2;
        if (shown.empty() && !center) cx = r.left + 1;
        Fill(D2D1::RectF(cx, r.top + 7, cx + 1.2f, r.bottom - 7), T.text);
    }
}

static void DrawToolbar() {
    float W = g_cw, H = TOOLBAR;
    if (!g_customFrame) Fill(D2D1::RectF(0, 0, W, H), T.bar);
    Fill(D2D1::RectF(0, H - 1, W, H), T.barLine, g_customFrame ? 0.7f : 1.0f);
    const float bt = 6, bb = 42, bw = 36;

    IconBtn(D2D1::RectF(8, bt, 8 + bw, bb), CMD_OPEN, 0xE8E5, L"Öffnen (Strg+O)");
    float leftEnd = 8 + bw + 8;
    if (g_hasDoc) {
        IconBtn(D2D1::RectF(leftEnd - 6, bt, leftEnd - 6 + bw, bb), CMD_TOC, 0xE8FD, L"Inhaltsverzeichnis (Strg+B)", true, g_tocOpen);
        leftEnd += bw + 2;
    }

    // Rechte Gruppe (von rechts nach links)
    float x = std::min(W - 8, g_captionLeft - 10);  // Platz für die System-Fensterknöpfe (Mica-Modus)
    auto place = [&](float w) { x -= w; return D2D1::RectF(x, bt, x + w, bb); };
    IconBtn(place(bw), CMD_MENU, 0xE712, L"Weitere Optionen");
    IconBtn(place(bw), CMD_SETTINGS, 0xE713, L"Einstellungen (Strg+,)", true, g_panelOpen);
    x -= 2;
    if (!g_hasDoc) {
        Text(L"FlinkPDF", fUiEll, D2D1::RectF(leftEnd, 0, x - 8, H), T.text);
        return;
    }
    IconBtn(place(bw), CMD_PRINT, 0xE749, L"Drucken (Strg+P)");
    x -= 6;
    Fill(D2D1::RectF(x + 2, 14, x + 3, 34), T.text, 0.12f);
    x -= 6;
    IconBtn(place(bw), CMD_FIT, g_zoomMode == ZM_WIDTH ? 0xE9A6 : 0xE740,
            g_zoomMode == ZM_WIDTH ? L"Ganze Seite anzeigen (Strg+2)" : L"An Seitenbreite anpassen (Strg+0)");
    IconBtn(place(bw), CMD_ZOOMIN, 0xE8A3, L"Vergrößern (Strg +)");
    D2D1_RECT_F zb = place(58);
    AddZone(zb, CMD_ZOOMBOX, L"Zoomstufe wählen");
    if (g_hot == CMD_ZOOMBOX) FillR(zb, 5, T.text, g_pressed == CMD_ZOOMBOX ? T.press : T.hover);
    Text(Fmt(L"%d %%", (int)std::lround(g_zoom * 100)), fUiC, zb, T.text);
    IconBtn(place(bw), CMD_ZOOMOUT, 0xE71F, L"Verkleinern (Strg −)");
    x -= 6;
    Fill(D2D1::RectF(x + 2, 14, x + 3, 34), T.text, 0.12f);
    x -= 6;
    IconBtn(place(bw), CMD_SEARCH, 0xE721, L"Suchen (Strg+F)", true, g_searchOpen);
    float rightStart = x - 8;

    // Mittlere Gruppe: Seite x / n
    std::wstring total = Fmt(L"/ %d", N());
    float totalW = Measure(total, fUi) + 4;
    int digits = (int)std::to_wstring(N()).size();
    float boxW = std::max(40.0f, 18.0f + digits * 8.5f);
    float cw = bw + 4 + boxW + 8 + totalW + 4 + bw;
    float cx = W / 2 - cw / 2;
    if (cx + cw > rightStart) cx = rightStart - cw;
    if (cx < leftEnd) cx = leftEnd;
    int cur = CurrentPage();
    float gx = cx;
    IconBtn(D2D1::RectF(gx, bt, gx + bw, bb), CMD_PREV, 0xE70E, L"Vorherige Seite", cur > 0);
    gx += bw + 4;
    D2D1_RECT_F pb = D2D1::RectF(gx, 10, gx + boxW, 38);
    AddZone(pb, CMD_PAGEBOX, L"Gehe zu Seite (Strg+G)");
    bool pf = g_focus == FLD_PAGE;
    FillR(pb, 4, T.field);
    StrokeR(pb, 4, pf ? T.accent : T.fieldLine, 1, pf ? 1.5f : 1);
    if (pf) DrawField(D2D1::RectF(pb.left + 4, pb.top, pb.right - 4, pb.bottom), g_pageText, nullptr, true, fUi, true);
    else Text(std::to_wstring(cur + 1), fUiC, pb, T.text);
    gx += boxW + 8;
    Text(total, fUi, D2D1::RectF(gx, 0, gx + totalW, H), T.dim);
    gx += totalW + 4;
    IconBtn(D2D1::RectF(gx, bt, gx + bw, bb), CMD_NEXT, 0xE70D, L"Nächste Seite", cur < N() - 1);

    // Dateiname links
    if (cx - 10 - leftEnd > 60) Text(sys::FileOf(g_path), fUiEll, D2D1::RectF(leftEnd, 0, cx - 12, H), T.text);
}

static void DrawSearchPanel() {
    float W = g_cw;
    float w = std::min(420.0f, W - 32);
    D2D1_RECT_F c = D2D1::RectF(W - 16 - w, TB() + 12, W - 16, TB() + 12 + 46);
    DrawShadow(c, 8, T.shadow);
    FillR(c, 8, T.card);
    StrokeR(c, 8, T.cardLine);
    AddZone(c, CMD_SEARCHFIELD, nullptr);
    wchar_t ic[2] = {0xE721, 0};
    Text(ic, fIcon, D2D1::RectF(c.left + 6, c.top, c.left + 36, c.bottom), T.dim);
    float bx = c.right - 6;
    auto btn = [&](int cmd, wchar_t g, const wchar_t* tip, bool en) {
        bx -= 32;
        IconBtn(D2D1::RectF(bx, c.top + 7, bx + 32, c.bottom - 7), cmd, g, tip, en);
    };
    btn(CMD_SEARCH_CLOSE, 0xE711, L"Schließen (Esc)", true);
    btn(CMD_SEARCH_NEXT, 0xE70D, L"Nächster Treffer (Enter)", !g_hits.empty());
    btn(CMD_SEARCH_PREV, 0xE70E, L"Vorheriger Treffer (Umschalt+Enter)", !g_hits.empty());
    std::wstring cnt;
    if (!g_searchQuery.empty()) {
        if (g_hits.empty()) cnt = g_searchDone ? L"Keine Treffer" : L"Suche …";
        else cnt = Fmt(L"%d / %d%s", g_curHit + 1, (int)g_hits.size(), g_searchDone ? L"" : L"+");
    }
    float cntW = cnt.empty() ? 0 : Measure(cnt, fSmall) + 10;
    Text(cnt, fSmallC, D2D1::RectF(bx - cntW, c.top, bx, c.bottom), T.dim);
    D2D1_RECT_F fr = D2D1::RectF(c.left + 38, c.top + 6, bx - cntW - 4, c.bottom - 6);
    DrawField(fr, g_searchText, L"Im Dokument suchen", g_focus == FLD_SEARCH, fUi, false);
    // Platzhalter linksbündig zeichnen (DrawField nutzt fUi für nicht zentriert)
}

static void DrawSelection(int i, const D2D1_RECT_F& r) {
    TextPos a, b;
    SelOrdered(a, b);
    if (i < a.page || i > b.page) return;
    const PageInfo& p = g_pages[i];
    if (!p.textLoaded) return;
    int n = (int)p.chars.size();
    int s0 = i == a.page ? std::min(a.idx, n) : 0, e = i == b.page ? std::min(b.idx, n) : n;
    float pw = r.right - r.left, ph = r.bottom - r.top;
    SetC(T.accent, 0.34f);
    D2D1_RECT_F cur{0, 0, 0, 0};
    bool has = false;
    auto flush = [&] {
        if (has)
            g_rt->FillRectangle(D2D1::RectF(r.left + cur.left * pw, r.top + cur.top * ph, r.left + cur.right * pw, r.top + cur.bottom * ph),
                                g_br);
    };
    for (int k = s0; k < e; k++) {
        const D2D1_RECT_F& c = p.chars[k].r;
        if (c.right <= c.left) continue;
        if (has) {
            float ov = std::min(cur.bottom, c.bottom) - std::max(cur.top, c.top);
            float mh = std::min(cur.bottom - cur.top, c.bottom - c.top);
            if (ov > mh * 0.5f && c.left >= cur.left - 0.002f) {  // gleiche Zeile: zu einem Rechteck zusammenfassen
                cur.right = std::max(cur.right, c.right);
                cur.top = std::min(cur.top, c.top);
                cur.bottom = std::max(cur.bottom, c.bottom);
                continue;
            }
            flush();
        }
        cur = c;
        has = true;
    }
    flush();
}

static void DrawToc() {
    float W = ContentX0();
    if (W <= 0) return;
    float top = TB(), H = g_ch;
    D2D1_RECT_F panel = D2D1::RectF(0, top, W, H);
    Fill(panel, T.card);
    Fill(D2D1::RectF(W - 1, top, W, H), T.cardLine);
    AddZone(panel, CMD_TOC_PANEL, nullptr);
    Text(L"INHALT", fSection, D2D1::RectF(18, top, W - 50, top + 44), T.accent);
    IconBtn(D2D1::RectF(W - 44, top + 6, W - 10, top + 38), CMD_TOC_CLOSE, 0xE711, L"Schließen (Strg+B)");
    float y0 = top + 44;
    Fill(D2D1::RectF(12, y0 - 1, W - 12, y0), T.cardLine);
    g_rt->PushAxisAlignedClip(D2D1::RectF(0, y0, W - 1, H), D2D1_ANTIALIAS_MODE_ALIASED);
    if (g_toc.empty()) {
        Text(L"Kein Inhaltsverzeichnis vorhanden", fSmall, D2D1::RectF(18, y0 + 10, W - 16, y0 + 34), T.dim);
    } else {
        const float RH = 30;
        int visN = (int)g_tocVis.size();
        float maxScroll = std::max(0.0f, visN * RH + 12 - (H - y0));
        g_tocScroll = std::clamp(g_tocScroll, 0.0f, maxScroll);
        int cur = CurrentPage(), curRow = -1;
        for (int v = 0; v < visN; v++) {
            int pg = g_toc[g_tocVis[v]].page;
            if (pg >= 0 && pg <= cur) curRow = v;
        }
        for (int v = std::max(0, (int)(g_tocScroll / RH) - 1); v < visN; v++) {
            float y = y0 + 6 + v * RH - g_tocScroll;
            if (y > H) break;
            const TocItem& t = g_toc[g_tocVis[v]];
            D2D1_RECT_F row = D2D1::RectF(6, y, W - 8, y + RH - 2);
            AddZone(row, CMD_TOC_ROW + v, nullptr);
            if (v == curRow) {
                FillR(row, 5, T.accent, 0.16f);
                FillR(D2D1::RectF(6, y + 8, 9, y + RH - 10), 1.5f, T.accent);
            }
            if (g_hot == CMD_TOC_ROW + v) FillR(row, 5, T.text, g_pressed == CMD_TOC_ROW + v ? T.press : T.hover);
            float x = 12 + std::min(t.depth, 8) * 14.0f;
            if (t.kids) {
                D2D1_RECT_F cr = D2D1::RectF(x, y + 3, x + 22, y + RH - 5);
                AddZone(cr, CMD_TOC_CHEV + v, nullptr);
                if (g_hot == CMD_TOC_CHEV + v) FillR(cr, 4, T.text, T.hover);
                wchar_t g[2] = {(wchar_t)(t.open ? 0xE70D : 0xE76C), 0};
                Text(g, fIconSm, cr, T.dim);
            }
            x += 24;
            Text(t.title.empty() ? std::wstring(L"(ohne Titel)") : t.title, fItem, D2D1::RectF(x, y, W - 14, y + RH - 2),
                 v == curRow ? T.text : T.text, v == curRow ? 1.0f : 0.85f);
        }
    }
    g_rt->PopAxisAlignedClip();
}

static void DrawPages() {
    int a, b;
    VisibleRange(a, b);
    D2D1_RECT_F clip = D2D1::RectF(ContentX0(), TB(), g_cw, g_ch);
    g_rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    double s = K() * g_dpi / 96.0;
    // Treffer-Index pro Seite (g_hits ist nach Seite sortiert)
    for (int i = a; i <= b; i++) {
        PageInfo& p = g_pages[i];
        D2D1_RECT_F r = PageRect(i);
        // Schatten
        for (int k = 1; k <= 3; k++) {
            D2D1_RECT_F sr = D2D1::RectF(r.left - k, r.top - k + 1, r.right + k, r.bottom + k + 1);
            Fill(sr, 0x000000, T.shadow * 0.16f / k);
        }
        if (p.bmp) {
            D2D1_RECT_F dst = r;
            int ew = std::max(1, (int)std::lround(p.w * s)), eh = std::max(1, (int)std::lround(p.h * s));
            if (p.bmpW == ew && p.bmpH == eh) {
                dst.right = r.left + p.bmpW * 96.0f / g_dpi;
                dst.bottom = r.top + p.bmpH * 96.0f / g_dpi;
            }
            g_rt->DrawBitmap(p.bmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
        } else {
            Fill(r, S.pageColor == 2 ? 0x1E1E1E : S.pageColor == 1 ? 0xF4ECD8 : 0xFFFFFF);
        }
        if (g_selActive) DrawSelection(i, r);
        // Suchtreffer
        if (!g_hits.empty()) {
            auto it = std::lower_bound(g_hits.begin(), g_hits.end(), i, [](const SearchHit& h, int pg) { return h.page < pg; });
            float pw = r.right - r.left, ph = r.bottom - r.top;
            for (; it != g_hits.end() && it->page == i; ++it) {
                bool cur = (int)(it - g_hits.begin()) == g_curHit;
                for (auto& q : it->rects) {
                    D2D1_RECT_F hr = D2D1::RectF(r.left + q.left * pw - 1, r.top + q.top * ph - 1, r.left + q.right * pw + 1,
                                                 r.top + q.bottom * ph + 1);
                    if (cur) {
                        Fill(hr, 0xFF8C00, 0.45f);
                        SetC(0xE07000, 0.9f);
                        g_rt->DrawRectangle(hr, g_br, 1.2f);
                    } else {
                        Fill(hr, 0xFFD400, 0.38f);
                    }
                }
            }
        }
    }
    g_rt->PopAxisAlignedClip();
}

static bool ThumbRect(D2D1_RECT_F& out) {
    double tot = TotalH(), vh = ViewH();
    if (!g_hasDoc || tot <= vh + 0.5) return false;
    float th = (float)std::max(40.0, vh * vh / tot);
    float ty = TB() + (float)((vh - th) * (g_sy / MaxSY()));
    out = D2D1::RectF(g_cw - SB_W, ty, g_cw, ty + th);
    return true;
}

static void DrawScrollbars() {
    D2D1_RECT_F t;
    if (ThumbRect(t)) {
        bool wide = g_sbHover || g_sbDrag;
        if (wide) Fill(D2D1::RectF(g_cw - SB_W, TB(), g_cw, g_ch), T.bar, 0.85f);
        float w = wide ? 8.0f : 4.0f;
        D2D1_RECT_F r = D2D1::RectF(g_cw - 3 - w, t.top + 2, g_cw - 3, t.bottom - 2);
        FillR(r, w / 2, T.text, wide ? 0.55f : 0.35f);
        if (g_sbDrag) {
            std::wstring s = Fmt(L"Seite %d", CurrentPage() + 1);
            float tw = Measure(s, fUi) + 22;
            float cy = (t.top + t.bottom) / 2;
            D2D1_RECT_F bub = D2D1::RectF(g_cw - SB_W - 10 - tw, cy - 16, g_cw - SB_W - 10, cy + 16);
            DrawShadow(bub, 6, T.shadow);
            FillR(bub, 6, T.card);
            StrokeR(bub, 6, T.cardLine);
            Text(s, fUiC, bub, T.text);
        }
    }
    double cwid = ContentW();
    if (g_hasDoc && cwid > ViewW() + 0.5) {
        float tw = (float)std::max(40.0, ViewW() * ViewW() / cwid);
        float tx = ContentX0() + (float)((ViewW() - tw) * (g_sx / MaxSX()));
        FillR(D2D1::RectF(tx + 2, g_ch - 7, tx + tw - 2, g_ch - 3), 2, T.text, 0.35f);
    }
}

static D2D1_RECT_F Btn(float cx, float y, float w, float h) { return D2D1::RectF(cx - w / 2, y, cx + w / 2, y + h); }

static void PrimaryButton(const D2D1_RECT_F& r, int cmd, const std::wstring& label) {
    AddZone(r, cmd, nullptr);
    FillR(r, 6, T.accent, g_hot == cmd ? (g_pressed == cmd ? 0.8f : 0.9f) : 1.0f);
    Text(label, fBold, r, T.accentText);
}
static void SecondaryButton(const D2D1_RECT_F& r, int cmd, const std::wstring& label) {
    AddZone(r, cmd, nullptr);
    FillR(r, 6, T.card);
    if (g_hot == cmd) FillR(r, 6, T.text, g_pressed == cmd ? T.press : T.hover);
    StrokeR(r, 6, T.cardLine);
    Text(label, fUiC, r, T.text);
}

static void DrawPassword() {
    float cx = g_cw / 2, top = TB() + std::max(24.0f, (ViewH() - 260) / 2);
    D2D1_RECT_F card = D2D1::RectF(cx - 210, top, cx + 210, top + 230);
    DrawShadow(card, 10, T.shadow);
    FillR(card, 10, T.card);
    StrokeR(card, 10, T.cardLine);
    wchar_t lock[2] = {0xE72E, 0};
    Text(lock, fIconBig, D2D1::RectF(cx - 20, top + 18, cx + 20, top + 50), T.accent);
    Text(L"Passwortgeschütztes PDF", fBold, D2D1::RectF(card.left, top + 54, card.right, top + 80), T.text);
    Text(g_pwWrong ? L"Falsches Passwort – bitte erneut versuchen." : sys::FileOf(g_pwPath), fSmallC,
         D2D1::RectF(card.left + 16, top + 80, card.right - 16, top + 100), g_pwWrong ? 0xE81123 : T.dim);
    D2D1_RECT_F f = D2D1::RectF(card.left + 24, top + 116, card.right - 24, top + 150);
    AddZone(f, CMD_PWFIELD, nullptr);
    FillR(f, 5, T.field);
    StrokeR(f, 5, g_focus == FLD_PASSWORD ? T.accent : T.fieldLine, 1, g_focus == FLD_PASSWORD ? 1.5f : 1);
    DrawField(D2D1::RectF(f.left + 10, f.top, f.right - 10, f.bottom), g_pwText, L"Passwort", g_focus == FLD_PASSWORD, fUi,
              false, true);
    PrimaryButton(D2D1::RectF(cx + 4, top + 170, card.right - 24, top + 206), CMD_PWOK, L"Öffnen");
    SecondaryButton(D2D1::RectF(card.left + 24, top + 170, cx - 4, top + 206), CMD_PWCANCEL, L"Abbrechen");
}

static void DrawEmpty() {
    float cx = g_cw / 2;
    int nRecent = std::min<int>(8, (int)g_recent.size());
    bool showDefault = !g_isDefault;
    float blockH = 96 + 18 + 40 + 26 + 28 + 40 + (showDefault ? 50 : 0);
    float avail = ViewH() - 48;
    int fit = std::max(0, (int)((avail - blockH - 44) / 44));
    nRecent = std::min(nRecent, fit);
    if (nRecent > 0) blockH += 44 + nRecent * 44.0f;
    float y = TB() + std::max(24.0f, (ViewH() - blockH) / 2);
    DrawLogo(cx - 48, y, 96);
    y += 96 + 18;
    Text(L"FlinkPDF", fTitle, D2D1::RectF(0, y, g_cw, y + 40), T.text);
    y += 40;
    Text(L"PDF hierher ziehen oder öffnen", fUiC, D2D1::RectF(0, y, g_cw, y + 26), T.dim);
    y += 26 + 28;
    PrimaryButton(Btn(cx, y, 240, 40), CMD_OPEN, L"Datei öffnen");
    y += 40;
    if (showDefault) {
        y += 10;
        SecondaryButton(Btn(cx, y, 300, 40), CMD_SETDEFAULT,
                        g_installed ? L"Als Standard-App für PDFs festlegen" : L"Installieren & als Standard festlegen");
        y += 40;
    }
    if (nRecent > 0) {
        y += 28;
        float w = std::min(520.0f, g_cw - 48);
        Text(L"Zuletzt geöffnet", fSmall, D2D1::RectF(cx - w / 2 + 12, y, cx + w / 2, y + 16), T.dim);
        y += 20;
        for (int i = 0; i < nRecent; i++) {
            D2D1_RECT_F r = D2D1::RectF(cx - w / 2, y, cx + w / 2, y + 42);
            AddZone(r, CMD_RECENT + i, g_recent[i].c_str());
            if (g_hot == CMD_RECENT + i) FillR(r, 6, T.text, g_pressed == CMD_RECENT + i ? T.press : T.hover);
            wchar_t ic[2] = {0xE8A5, 0};
            Text(ic, fIcon, D2D1::RectF(r.left + 6, r.top, r.left + 38, r.bottom), T.accent);
            std::wstring name = sys::FileOf(g_recent[i]);
            float nw = std::min(Measure(name, fUiEll) + 2, (r.right - r.left) * 0.6f);
            Text(name, fUiEll, D2D1::RectF(r.left + 42, r.top, r.left + 42 + nw, r.bottom), T.text);
            Text(sys::DirOf(g_recent[i]), fSmall, D2D1::RectF(r.left + 42 + nw + 10, r.top, r.right - 10, r.bottom), T.dim);
            y += 44;
        }
    }
}

static void DrawToast() {
    float tw = std::min(Measure(g_toast, fUi) + 36, g_cw - 32);
    D2D1_RECT_F r = D2D1::RectF(g_cw / 2 - tw / 2, g_ch - 64, g_cw / 2 + tw / 2, g_ch - 26);
    DrawShadow(r, 8, 0.6f);
    FillR(r, 8, T.toast);
    Text(g_toast, fUiC, r, T.toastText);
}

// ---------------------------------------------------------------------------
//  Einstellungs-Panel
// ---------------------------------------------------------------------------
static float PanelEase() { float t = 1 - g_panelT; return 1 - t * t * t; }
static float PanelLeft() { return g_cw - std::min(PANEL_W, g_cw) + (1 - PanelEase()) * 36; }

static void Toggle(float x, float cy, bool on, int cmd, bool enabled) {
    D2D1_RECT_F r = D2D1::RectF(x, cy - 10, x + 40, cy + 10);
    float a = enabled ? 1.0f : 0.4f;
    if (enabled) AddZone(D2D1::RectF(x - 6, cy - 16, x + 46, cy + 16), cmd, nullptr);
    if (on) {
        FillR(r, 10, T.accent, a);
    } else {
        FillR(r, 10, T.field, a);
        StrokeR(r, 10, T.dim, 0.9f * a, 1);
    }
    if (enabled && g_hot == cmd) FillR(r, 10, on ? 0x000000 : T.text, 0.08f);
    float kx = on ? x + 30 : x + 10, rad = (g_pressed == cmd) ? 7.5f : 6.0f;
    SetC(on ? T.accentText : T.dim, a);
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), rad, rad), g_br);
}

static void Segmented(const D2D1_RECT_F& r, std::initializer_list<const wchar_t*> opts, int sel, int cmdBase) {
    FillR(r, 6, T.field);
    StrokeR(r, 6, T.cardLine);
    float w = (r.right - r.left - 4) / (float)opts.size();
    int i = 0;
    for (const wchar_t* o : opts) {
        D2D1_RECT_F sr = D2D1::RectF(r.left + 2 + i * w, r.top + 2, r.left + 2 + (i + 1) * w, r.bottom - 2);
        AddZone(sr, cmdBase + i, nullptr);
        if (i == sel) {
            FillR(sr, 5, T.accent);
            Text(o, fSeg, sr, T.accentText);
        } else {
            if (g_hot == cmdBase + i) FillR(sr, 5, T.text, g_pressed == cmdBase + i ? T.press : T.hover);
            Text(o, fSeg, sr, T.text);
        }
        i++;
    }
}

static void SmallButton(const D2D1_RECT_F& r, int cmd, const wchar_t* label, bool enabled = true) {
    if (enabled) AddZone(r, cmd, nullptr);
    FillR(r, 5, T.field, enabled ? 1.0f : 0.5f);
    if (enabled && g_hot == cmd) FillR(r, 5, T.text, g_pressed == cmd ? T.press : T.hover);
    StrokeR(r, 5, T.cardLine);
    Text(label, fSeg, r, T.text, enabled ? 1.0f : 0.4f);
}

static void DrawSettings() {
    float e = PanelEase();
    float top = TB(), H = g_ch;
    // abgedunkelter Hintergrund – Klick schließt das Panel
    Fill(D2D1::RectF(0, top, g_cw, H), 0x000000, 0.28f * e);
    AddZone(D2D1::RectF(0, top, g_cw, H), CMD_SET_DIM, nullptr);
    float L = PanelLeft(), R = L + std::min(PANEL_W, g_cw);
    D2D1_RECT_F panel = D2D1::RectF(L, top, R, H);
    for (int k = 1; k <= 6; k++) Fill(D2D1::RectF(L - k * 2.0f, top, L, H), 0x000000, 0.035f * e);
    Fill(panel, T.card, e);
    Fill(D2D1::RectF(L, top, L + 1, H), T.cardLine, e);
    AddZone(panel, CMD_SET_PANEL, nullptr);
    g_rt->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_ALIASED);

    const float x0 = L + 24, x1 = R - 24;
    float y = top + 18 - g_panelScroll;
    Text(L"Einstellungen", fHead, D2D1::RectF(x0, y, x1 - 40, y + 36), T.text);
    IconBtn(D2D1::RectF(x1 - 34, y + 1, x1 + 2, y + 35), CMD_SET_CLOSE, 0xE711, L"Schließen (Esc)");
    y += 52;

    auto section = [&](const wchar_t* name) {
        Text(name, fSection, D2D1::RectF(x0, y, x1, y + 20), T.accent);
        y += 28;
    };
    auto divider = [&]() {
        y += 8;
        Fill(D2D1::RectF(x0, y, x1, y + 1), T.cardLine);
        y += 18;
    };
    // Zeile mit Schalter; desc optional
    auto toggleRow = [&](const wchar_t* label, const wchar_t* desc, bool on, int cmd, bool enabled = true) {
        float h = desc ? 48.0f : 36.0f;
        float tr = x1 - 52;
        Text(label, fUi, D2D1::RectF(x0, y, tr, y + (desc ? 26 : h)), T.text, enabled ? 1.0f : 0.5f);
        if (desc) Text(desc, fSmall, D2D1::RectF(x0, y + 24, tr, y + 44), T.dim);
        Toggle(x1 - 40, y + h / 2, on, cmd, enabled);
        y += h + 6;
    };
    auto segRow = [&](const wchar_t* label, std::initializer_list<const wchar_t*> opts, int sel, int base) {
        Text(label, fUi, D2D1::RectF(x0, y, x1, y + 22), T.text);
        y += 28;
        Segmented(D2D1::RectF(x0, y, x1, y + 34), opts, sel, base);
        y += 34 + 14;
    };
    auto buttonRow = [&](const wchar_t* label, const wchar_t* desc, const wchar_t* btn, int cmd, bool enabled) {
        float h = desc ? 48.0f : 36.0f;
        float bw = Measure(btn, fSeg) + 28;
        Text(label, fUi, D2D1::RectF(x0, y, x1 - bw - 12, y + (desc ? 26 : h)), T.text);
        if (desc) Text(desc, fSmall, D2D1::RectF(x0, y + 24, x1 - bw - 12, y + 44), T.dim);
        SmallButton(D2D1::RectF(x1 - bw, y + h / 2 - 15, x1, y + h / 2 + 15), cmd, btn, enabled);
        y += h + 6;
    };

    section(L"DARSTELLUNG");
    segRow(L"Farbschema", {L"Wie Windows", L"Hell", L"Dunkel"}, S.theme, CMD_SET_THEME);
    toggleRow(L"Mica & integrierte Titelleiste", g_isWin11 ? L"Moderner Fensterstil von Windows 11" : L"Nur unter Windows 11 verfügbar",
              S.mica && g_isWin11, CMD_SET_MICA, g_isWin11);
    segRow(L"Seitenfarbe  (Bilder bleiben unverändert)", {L"Normal", L"Sepia", L"Dunkel"}, S.pageColor, CMD_SET_PCOLOR);
    divider();
    section(L"BEDIENUNG");
    segRow(L"Ziehen mit der linken Maustaste", {L"Text auswählen", L"Verschieben"}, S.mouseMode, CMD_SET_MOUSE);
    toggleRow(L"Sanftes Scrollen", nullptr, S.smooth, CMD_SET_SMOOTH);
    segRow(L"Scrollgeschwindigkeit", {L"Langsam", L"Normal", L"Schnell"}, S.speed, CMD_SET_SPEED);
    toggleRow(L"Horizontal umkehren", L"Richtung links/rechts tauschen", S.invH, CMD_SET_INVH);
    toggleRow(L"Vertikal umkehren", L"Richtung oben/unten tauschen", S.invV, CMD_SET_INVV);
    divider();
    section(L"DOKUMENTE");
    segRow(L"Zoom beim Öffnen", {L"Zuletzt", L"Seitenbreite", L"Ganze Seite"}, S.openZoom, CMD_SET_OPEN);
    toggleRow(L"Inhaltsverzeichnis anzeigen", L"Beim Öffnen, falls das PDF eines hat", S.tocAuto, CMD_SET_TOCAUTO);
    toggleRow(L"Leseposition merken", L"Öffnet Dateien an der letzten Stelle", S.remember, CMD_SET_REMEMBER);
    buttonRow(L"Verlauf", L"„Zuletzt geöffnet“ und Positionen", L"Leeren", CMD_SET_CLEARRECENT, !g_recent.empty());
    divider();
    section(L"WINDOWS");
    if (g_isDefault) {
        Text(L"Standard-App für PDFs", fUi, D2D1::RectF(x0, y, x1 - 90, y + 36), T.text);
        wchar_t ok[2] = {0xE73E, 0};
        Text(ok, fIcon, D2D1::RectF(x1 - 80, y, x1 - 58, y + 36), T.accent);
        Text(L"Aktiv", fUiR, D2D1::RectF(x1 - 60, y, x1, y + 36), T.accent);
        y += 42;
    } else {
        buttonRow(L"Standard-App für PDFs", g_installed ? L"Öffnet die Windows-Einstellungen" : L"Installiert FlinkPDF für deinen Benutzer",
                  L"Festlegen", CMD_SETDEFAULT, true);
    }
    buttonRow(L"PDF-Registrierung", L"FlinkPDF aus „Öffnen mit“ entfernen", L"Entfernen", CMD_SET_UNREG, sys::IsRegistered());
    y += 10;
    Text(L"FlinkPDF 1.2  ·  Rendering: PDFium", fSmall, D2D1::RectF(x0, y, x1, y + 20), T.dim);
    y += 32;
    g_panelContentH = y + g_panelScroll - top;
    g_rt->PopAxisAlignedClip();
}

static void OpenPanel(bool open) {
    if (open && !g_panelOpen) {
        g_panelOpen = true;
        g_panelT = 0;
        g_panelScroll = 0;
        g_isDefault = sys::IsDefaultPdfApp();
        if (g_focus != FLD_NONE && g_focus != FLD_PASSWORD) g_focus = FLD_NONE;
    }
    if (!g_panelOpen) return;
    QueryPerformanceCounter(&g_panelLast);
    SetTimer(g_hwnd, TIMER_PANEL, 8, nullptr);
    g_panelWant = open;
    Invalidate();
}

static void Paint() {
    if (!EnsureRT()) return;
    g_zones.clear();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->SetTextAntialiasMode(g_customFrame ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
    if (g_customFrame) {
        g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));  // Werkzeugleiste: Mica scheint durch
        Fill(D2D1::RectF(0, TB(), g_cw, g_ch), T.bg);
    } else {
        g_rt->Clear(Col(T.bg));
    }
    if (g_hasDoc) DrawPages();
    else if (!g_pwPath.empty()) DrawPassword();
    else DrawEmpty();
    if (g_hasDoc) DrawScrollbars();
    if (g_hasDoc && ContentX0() > 0) DrawToc();
    if (g_hasDoc && g_searchOpen) DrawSearchPanel();
    if (g_panelOpen) DrawSettings();
    if (!g_fullscreen) DrawToolbar();
    if (!g_toast.empty()) DrawToast();
    HRESULT hr = g_rt->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        DiscardDevice();
        Invalidate();
    }
}

// ============================================================================
//  Theme / Fenster
// ============================================================================
static COLORREF ToColorRef(UINT32 rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

static UINT32 MixColor(UINT32 a, UINT32 b, float t) {
    auto ch = [&](int sh) { return (UINT32)std::lround(((a >> sh) & 0xFF) * (1 - t) + ((b >> sh) & 0xFF) * t) << sh; };
    return ch(16) | ch(8) | ch(0);
}

static bool SystemAccent(UINT32& rgb) {
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &v,
                     &sz) != ERROR_SUCCESS)
        return false;
    rgb = ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);  // ABGR -> RGB
    return true;
}

static bool DetectWin11() {
    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW*);
    auto fn = (RtlGetVersion_t)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW vi{sizeof vi};
    if (!fn || fn(&vi) != 0) return false;
    // Wine meldet sich ggf. als Windows 10; echte Win11-Builds beginnen bei 22000
    return vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22000;
}

static int SysMetric(int idx) {
    typedef int(WINAPI * Fn)(int, UINT);
    static Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    return fn ? fn(idx, (UINT)g_dpi) : GetSystemMetrics(idx);
}
static int FrameY() { return SysMetric(SM_CYFRAME) + SysMetric(SM_CXPADDEDBORDER); }

static void UpdateCaptionBounds() {
    g_captionLeft = 1e9f;
    g_capRect = D2D1::RectF(0, 0, 0, 0);
    if (!g_customFrame) return;
    RECT r{}, wr{};
    if (SUCCEEDED(DwmGetWindowAttribute(g_hwnd, 5 /*DWMWA_CAPTION_BUTTON_BOUNDS*/, &r, sizeof r)) && r.right > r.left) {
        GetWindowRect(g_hwnd, &wr);
        POINT o{0, 0};
        ClientToScreen(g_hwnd, &o);
        g_capRect = D2D1::RectF(PxToDip(r.left + wr.left - o.x), PxToDip(r.top + wr.top - o.y), PxToDip(r.right + wr.left - o.x),
                                PxToDip(r.bottom + wr.top - o.y));
    } else {
        g_capRect = D2D1::RectF(g_cw - 138, 0, g_cw, 32);  // Standardmaße der Windows-11-Fensterknöpfe
    }
    g_capRect.top = std::min(g_capRect.top, 0.0f);
    g_captionLeft = g_capRect.left;
}

// Welcher Fensterknopf liegt unter dem Punkt? (Minimieren | Maximieren | Schließen)
static int CaptionHit(float x, float y) {
    if (!g_customFrame || g_capRect.right <= g_capRect.left) return 0;
    if (x < g_capRect.left || x >= g_capRect.right || y < g_capRect.top || y >= g_capRect.bottom) return 0;
    int i = (int)((x - g_capRect.left) / ((g_capRect.right - g_capRect.left) / 3.0f));
    return i <= 0 ? HTMINBUTTON : i == 1 ? HTMAXBUTTON : HTCLOSE;
}

// Windows 11: Fensterrahmen in den Clientbereich erweitern und Mica als Hintergrund der Werkzeugleiste nutzen.
// Das Compositing übernimmt DWM auf der GPU – für die App entstehen keine zusätzlichen Kosten.
static void ApplyFrame() {
    bool want = g_isWin11 && S.mica && !g_fullscreen;
    if (want != g_customFrame) {
        g_customFrame = want;
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    MARGINS m = g_customFrame ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(g_hwnd, &m);
    if (g_isWin11) {
        int backdrop = g_customFrame ? 2 /*DWMSBT_MAINWINDOW = Mica*/ : 1 /*DWMSBT_NONE*/;
        if (FAILED(DwmSetWindowAttribute(g_hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof backdrop))) {
            BOOL mica = g_customFrame;  // Windows 11 21H2
            DwmSetWindowAttribute(g_hwnd, 1029 /*DWMWA_MICA_EFFECT*/, &mica, sizeof mica);
        }
    }
    COLORREF cap = g_customFrame ? 0xFFFFFFFF /*DWMWA_COLOR_DEFAULT*/ : ToColorRef(T.bar);
    COLORREF txt = g_customFrame ? 0xFFFFFFFF : ToColorRef(T.text);
    DwmSetWindowAttribute(g_hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof cap);
    DwmSetWindowAttribute(g_hwnd, 36 /*DWMWA_TEXT_COLOR*/, &txt, sizeof txt);
    UpdateCaptionBounds();
    Invalidate();
}

static void ApplyTheme() {
    g_dark = S.theme == 0 ? sys::SystemUsesDarkTheme() : S.theme == 2;
    T = g_dark ? kDark : kLight;
    UINT32 acc;
    if (SystemAccent(acc)) {
        T.accent = g_dark ? MixColor(acc, 0xFFFFFF, 0.42f) : MixColor(acc, 0x000000, 0.08f);
        float lum = (0.299f * ((T.accent >> 16) & 0xFF) + 0.587f * ((T.accent >> 8) & 0xFF) + 0.114f * (T.accent & 0xFF)) / 255;
        T.accentText = lum > 0.6f ? 0x101010 : 0xFFFFFF;
    }
    BOOL d = g_dark;
    if (FAILED(DwmSetWindowAttribute(g_hwnd, 20, &d, sizeof d))) DwmSetWindowAttribute(g_hwnd, 19, &d, sizeof d);
    sys::AllowDarkMenus(g_dark);
    if (g_tip) SetWindowTheme(g_tip, g_dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    ApplyFrame();
}

static UINT GetWindowDpi(HWND h) {
    typedef UINT(WINAPI * Fn)(HWND);
    static Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn) return fn(h);
    HDC dc = GetDC(h);
    UINT d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(h, dc);
    return d;
}

static void ToggleFullscreen() {
    DWORD style = GetWindowLongW(g_hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof mi};
        if (GetWindowPlacement(g_hwnd, &g_fsPrev) && GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            g_fullscreen = true;
            ApplyFrame();
            SetWindowLongW(g_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            Toast(L"Vollbild – mit Esc oder F11 beenden", 2200);
        }
    } else {
        g_fullscreen = false;
        SetWindowLongW(g_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_fsPrev);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        ApplyFrame();
    }
    Invalidate();
}

static void SavePlacement() {
    WINDOWPLACEMENT wp{sizeof wp};
    if (g_fullscreen) wp = g_fsPrev;
    else if (!GetWindowPlacement(g_hwnd, &wp)) return;
    RegSetKeyValueW(HKEY_CURRENT_USER, sys::kSettingsKey, L"Placement", REG_BINARY, &wp, sizeof wp);
}

static void UpdateTooltip(const Zone* z) {
    const wchar_t* tip = z ? z->tip : nullptr;
    if (!g_tip || tip == g_curTip) return;
    g_curTip = tip;
    TOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.hwnd = g_hwnd;
    ti.uId = 1;
    SendMessageW(g_tip, TTM_POP, 0, 0);
    if (tip) {
        ti.rect = {DipToPx(z->r.left), DipToPx(z->r.top), DipToPx(z->r.right), DipToPx(z->r.bottom)};
        ti.lpszText = (LPWSTR)tip;
    } else {
        ti.rect = {0, 0, 0, 0};
        ti.lpszText = (LPWSTR)L"";
    }
    SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

// ============================================================================
//  Befehle
// ============================================================================
static void FocusField(int f) {
    g_focus = f;
    g_caretOn = true;
    if (f == FLD_PAGE) {
        g_pageText = std::to_wstring(CurrentPage() + 1);
        g_selAll = true;
    }
    if (f != FLD_NONE) SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
    else KillTimer(g_hwnd, TIMER_CARET);
    Invalidate();
}

static void SetDefaultApp() {
    std::wstring exe = sys::Install();
    g_installed = sys::IsInstalledCopy() || _wcsicmp(sys::DirOf(exe).c_str(), sys::InstallDir().c_str()) == 0;
    sys::OpenDefaultAppsSettings();
    Toast(L"Wähle in den Einstellungen bei „.pdf“ FlinkPDF aus.", 7000);
}

static void ShowMenu(const D2D1_RECT_F& anchor) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, MNU_DEFAULT, g_isDefault ? L"Standard-App-Einstellungen öffnen" : L"Als Standard-App für PDFs festlegen…");
    AppendMenuW(m, MF_STRING | (g_hasDoc ? 0 : MF_GRAYED), MNU_FULLSCREEN, L"Vollbild\tF11");
    AppendMenuW(m, MF_STRING | (g_hasDoc ? 0 : MF_GRAYED), MNU_LOCATION, L"Im Explorer anzeigen");
    AppendMenuW(m, MF_STRING | (g_hasDoc ? 0 : MF_GRAYED), MNU_OPENWITH, L"Mit anderer App öffnen…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, MNU_SETTINGS, L"Einstellungen\tStrg+,");
    AppendMenuW(m, MF_STRING | (sys::IsRegistered() ? 0 : MF_GRAYED), MNU_UNREGISTER, L"PDF-Registrierung entfernen");
    AppendMenuW(m, MF_STRING, MNU_ABOUT, L"Über FlinkPDF");
    POINT pt{DipToPx(anchor.right), DipToPx(anchor.bottom)};
    ClientToScreen(g_hwnd, &pt);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (c) {
        case MNU_DEFAULT: SetDefaultApp(); break;
        case MNU_FULLSCREEN: ToggleFullscreen(); break;
        case MNU_SETTINGS: OpenPanel(true); break;
        case MNU_LOCATION: {
            std::wstring a = L"/select,\"" + g_path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", a.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }
        case MNU_OPENWITH: {
            OPENASINFO oi{};
            oi.pcszFile = g_path.c_str();
            oi.oaifInFlags = OAIF_EXEC;
            SHOpenWithDialog(g_hwnd, &oi);
            break;
        }
        case MNU_UNREGISTER:
            sys::UnregisterApp();
            g_isDefault = sys::IsDefaultPdfApp();
            Toast(L"FlinkPDF ist nicht mehr als PDF-Programm registriert.");
            break;
        case MNU_ABOUT:
            MessageBoxW(g_hwnd,
                        L"FlinkPDF 1.2\nEin schneller, schlanker PDF-Viewer.\n\n"
                        L"Tastenkürzel:\n"
                        L"Strg+O  Öffnen\t\tStrg+F  Suchen\n"
                        L"Strg+P  Drucken\t\tStrg+G  Gehe zu Seite\n"
                        L"Strg +/−  Zoom\t\tStrg+0  Seitenbreite\n"
                        L"Strg+1  100 %\t\tStrg+2  Ganze Seite\n"
                        L"F11  Vollbild\t\tStrg+W  Schließen\n"
                        L"Strg+,  Einstellungen\t\tStrg+B  Inhaltsverzeichnis\n"
                        L"Strg+C  Kopieren\t\tStrg+A  Alles auswählen\n\n"
                        L"Rendering: PDFium (BSD-3-Clause / Apache-2.0)",
                        L"Über FlinkPDF", MB_OK | MB_ICONINFORMATION);
            break;
    }
}

static void ShowZoomMenu(const D2D1_RECT_F& anchor) {
    HMENU m = CreatePopupMenu();
    int pct[] = {50, 75, 100, 125, 150, 200, 300, 400};
    for (int p : pct) AppendMenuW(m, MF_STRING, MNU_ZOOM + p, Fmt(L"%d %%", p).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_zoomMode == ZM_WIDTH ? MF_CHECKED : 0), MNU_FITWIDTH, L"Seitenbreite\tStrg+0");
    AppendMenuW(m, MF_STRING | (g_zoomMode == ZM_PAGE ? MF_CHECKED : 0), MNU_FITPAGE, L"Ganze Seite\tStrg+2");
    POINT pt{DipToPx(anchor.left), DipToPx(anchor.bottom)};
    ClientToScreen(g_hwnd, &pt);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (c == MNU_FITWIDTH) ApplyFit(ZM_WIDTH);
    else if (c == MNU_FITPAGE) ApplyFit(ZM_PAGE);
    else if (c > MNU_ZOOM) {
        SetZoom((c - MNU_ZOOM) / 100.0, CenterX(), TB() + ViewH() / 2);
        sys::WriteDw(L"ZoomMode", ZM_CUSTOM);
        sys::WriteDw(L"Zoom", c - MNU_ZOOM);
    }
}

static void ShowContextMenu(float x, float y) {
    HMENU m = CreatePopupMenu();
    std::wstring sel = SelectionText(200);
    AppendMenuW(m, MF_STRING | (g_selActive ? 0 : MF_GRAYED), MNU_COPY, L"Kopieren\tStrg+C");
    AppendMenuW(m, MF_STRING, MNU_SELALL, L"Alles auswählen\tStrg+A");
    AppendMenuW(m, MF_STRING | (g_selActive && sel.size() < 120 ? 0 : MF_GRAYED), MNU_SEARCHSEL, L"Auswahl im Dokument suchen");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_tocOpen ? MF_CHECKED : 0), MNU_TOC, L"Inhaltsverzeichnis\tStrg+B");
    POINT pt{DipToPx(x), DipToPx(y)};
    ClientToScreen(g_hwnd, &pt);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (c) {
        case MNU_COPY: CopySelection(); break;
        case MNU_SELALL: SelectAll(); break;
        case MNU_TOC: SetTocOpen(!g_tocOpen); break;
        case MNU_SEARCHSEL: {
            std::wstring q;
            for (wchar_t ch : sel) q.push_back(ch == L'\r' || ch == L'\n' || ch == L'\t' ? L' ' : ch);
            while (!q.empty() && q.back() == L' ') q.pop_back();
            while (!q.empty() && q.front() == L' ') q.erase(q.begin());
            if (q.empty()) break;
            g_searchText = q;
            OpenSearch();
            StartSearch();
            break;
        }
    }
}

static void SubmitPassword() {
    if (g_pwPath.empty()) return;
    std::wstring p = g_pwPath, pw = g_pwText;
    OpenDocument(p, &pw);
}

static void Execute(int cmd, const D2D1_RECT_F& r) {
    switch (cmd) {
        case CMD_OPEN: ShowOpenDialog(); break;
        case CMD_PREV: GoToPage(CurrentPage() - 1); break;
        case CMD_NEXT: GoToPage(CurrentPage() + 1); break;
        case CMD_PAGEBOX: FocusField(FLD_PAGE); break;
        case CMD_ZOOMIN: ZoomStep(+1); break;
        case CMD_ZOOMOUT: ZoomStep(-1); break;
        case CMD_ZOOMBOX: ShowZoomMenu(r); break;
        case CMD_FIT: ApplyFit(g_zoomMode == ZM_WIDTH ? ZM_PAGE : ZM_WIDTH); break;
        case CMD_SEARCH: if (g_searchOpen) CloseSearch(); else OpenSearch(); break;
        case CMD_PRINT: PrintDocument(); break;
        case CMD_MENU: ShowMenu(r); break;
        case CMD_SETDEFAULT: SetDefaultApp(); break;
        case CMD_SEARCHFIELD: FocusField(FLD_SEARCH); g_selAll = false; break;
        case CMD_SEARCH_NEXT: NextHit(+1); break;
        case CMD_SEARCH_PREV: NextHit(-1); break;
        case CMD_SEARCH_CLOSE: CloseSearch(); break;
        case CMD_PWFIELD: FocusField(FLD_PASSWORD); break;
        case CMD_PWOK: SubmitPassword(); break;
        case CMD_PWCANCEL: g_pwPath.clear(); FocusField(FLD_NONE); break;
        case CMD_SETTINGS: OpenPanel(!(g_panelOpen && g_panelWant)); break;
        case CMD_SET_CLOSE:
        case CMD_SET_DIM: OpenPanel(false); break;
        case CMD_SET_PANEL: break;
        case CMD_SET_MICA: S.mica = !S.mica; S.Save(); ApplyFrame(); break;
        case CMD_TOC: SetTocOpen(!g_tocOpen); break;
        case CMD_TOC_CLOSE: SetTocOpen(false); break;
        case CMD_TOC_PANEL: break;
        case CMD_SET_TOCAUTO: S.tocAuto = !S.tocAuto; S.Save(); break;
        case CMD_SET_SMOOTH: S.smooth = !S.smooth; S.Save(); break;
        case CMD_SET_INVH: S.invH = !S.invH; S.Save(); break;
        case CMD_SET_INVV: S.invV = !S.invV; S.Save(); break;
        case CMD_SET_REMEMBER:
            S.remember = !S.remember;
            S.Save();
            if (!S.remember) RegDeleteTreeW(HKEY_CURRENT_USER, kPosKey);
            break;
        case CMD_SET_CLEARRECENT:
            g_recent.clear();
            sys::SaveRecent(g_recent);
            RegDeleteTreeW(HKEY_CURRENT_USER, kPosKey);
            Toast(L"Verlauf geleert.");
            break;
        case CMD_SET_UNREG:
            sys::UnregisterApp();
            g_isDefault = sys::IsDefaultPdfApp();
            Toast(L"FlinkPDF ist nicht mehr als PDF-Programm registriert.");
            break;
        default:
            if (cmd >= CMD_TOC_CHEV && cmd < CMD_TOC_CHEV + (int)g_tocVis.size()) {
                TocItem& t = g_toc[g_tocVis[cmd - CMD_TOC_CHEV]];
                t.open = !t.open;
                RebuildTocVis();
            } else if (cmd >= CMD_TOC_ROW && cmd < CMD_TOC_ROW + (int)g_tocVis.size()) {
                const TocItem& t = g_toc[g_tocVis[cmd - CMD_TOC_ROW]];
                if (t.page >= 0 && t.page < N()) GoToPage(t.page, t.y);
            } else if (cmd >= CMD_SET_PCOLOR && cmd <= CMD_SET_PCOLOR + 2) {
                S.pageColor = cmd - CMD_SET_PCOLOR;
                S.Save();
                ReleaseBitmaps();
                UpdateJobs();
            } else if (cmd >= CMD_SET_MOUSE && cmd <= CMD_SET_MOUSE + 1) {
                S.mouseMode = cmd - CMD_SET_MOUSE;
                S.Save();
                UpdateJobs();
            } else if (cmd >= CMD_SET_THEME && cmd <= CMD_SET_THEME + 2) {
                S.theme = cmd - CMD_SET_THEME;
                S.Save();
                ApplyTheme();
            } else if (cmd >= CMD_SET_SPEED && cmd <= CMD_SET_SPEED + 2) {
                S.speed = cmd - CMD_SET_SPEED;
                S.Save();
            } else if (cmd >= CMD_SET_OPEN && cmd <= CMD_SET_OPEN + 2) {
                S.openZoom = cmd - CMD_SET_OPEN;
                S.Save();
            }
            if (cmd >= CMD_RECENT && cmd < CMD_RECENT + (int)g_recent.size()) {
                std::wstring p = g_recent[cmd - CMD_RECENT];
                if (!OpenDocument(p) && GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    g_recent.erase(g_recent.begin() + (cmd - CMD_RECENT));
                    sys::SaveRecent(g_recent);
                    Toast(L"Die Datei existiert nicht mehr.");
                }
            }
    }
}

// ============================================================================
//  Eingabe
// ============================================================================
static const Zone* ZoneAt(float x, float y) {
    for (auto it = g_zones.rbegin(); it != g_zones.rend(); ++it)
        if (In(it->r, x, y)) return &*it;
    return nullptr;
}

static const LinkInfo* LinkAt(float x, float y) {
    if (!g_hasDoc || y < TB()) return nullptr;
    int a, b;
    VisibleRange(a, b);
    for (int i = a; i <= b; i++) {
        D2D1_RECT_F r = PageRect(i);
        if (!In(r, x, y)) continue;
        float nx = (x - r.left) / (r.right - r.left), ny = (y - r.top) / (r.bottom - r.top);
        for (auto& l : g_pages[i].links)
            if (In(l.r, nx, ny)) return &l;
    }
    return nullptr;
}

static void FollowLink(const LinkInfo& l) {
    if (l.page >= 0) {
        GoToPage(l.page, l.y);
    } else if (!l.uri.empty()) {
        std::wstring u = l.uri, low = u;
        for (auto& c : low) c = towlower(c);
        if (low.rfind(L"http://", 0) == 0 || low.rfind(L"https://", 0) == 0 || low.rfind(L"mailto:", 0) == 0)
            ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

static bool InScrollbar(float x, float y) {
    D2D1_RECT_F t;
    return ThumbRect(t) && x >= g_cw - SB_W && y >= TB();
}

static void FieldChanged() {
    g_caretOn = true;
    if (g_focus == FLD_SEARCH) SetTimer(g_hwnd, TIMER_SEARCH, 200, nullptr);
    Invalidate();
}

static std::wstring* FieldText() {
    switch (g_focus) {
        case FLD_PAGE: return &g_pageText;
        case FLD_SEARCH: return &g_searchText;
        case FLD_PASSWORD: return &g_pwText;
    }
    return nullptr;
}

static void FieldChar(wchar_t c) {
    std::wstring* s = FieldText();
    if (!s) return;
    if (g_focus == FLD_PAGE && !(c >= L'0' && c <= L'9')) return;
    if (g_selAll) { s->clear(); g_selAll = false; }
    if (s->size() < 256) s->push_back(c);
    FieldChanged();
}

static void Paste() {
    std::wstring* s = FieldText();
    if (!s || !OpenClipboard(g_hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            if (g_selAll) { s->clear(); g_selAll = false; }
            for (; *p && s->size() < 256; p++) {
                if (*p == L'\r' || *p == L'\n' || *p == L'\t') s->push_back(L' ');
                else if (g_focus != FLD_PAGE || (*p >= L'0' && *p <= L'9')) s->push_back(*p);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    FieldChanged();
}

static bool FieldKey(WPARAM vk) {
    std::wstring* s = FieldText();
    if (!s) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    switch (vk) {
        case VK_BACK:
            if (g_selAll || ctrl) s->clear();
            else if (!s->empty()) {
                s->pop_back();
                if (!s->empty() && IS_HIGH_SURROGATE(s->back())) s->pop_back();
            }
            g_selAll = false;
            FieldChanged();
            return true;
        case VK_RETURN:
            if (g_focus == FLD_PAGE) {
                int p = _wtoi(g_pageText.c_str());
                FocusField(FLD_NONE);
                if (p >= 1 && p <= N()) GoToPage(p - 1);
            } else if (g_focus == FLD_SEARCH) {
                KillTimer(g_hwnd, TIMER_SEARCH);
                if (g_searchText != g_searchQuery) StartSearch();
                else NextHit(shift ? -1 : +1);
            } else if (g_focus == FLD_PASSWORD) {
                SubmitPassword();
            }
            return true;
        case VK_ESCAPE:
            if (g_focus == FLD_SEARCH) CloseSearch();
            else if (g_focus == FLD_PASSWORD) { g_pwPath.clear(); FocusField(FLD_NONE); }
            else FocusField(FLD_NONE);
            return true;
        case 'A':
            if (ctrl) { g_selAll = true; Invalidate(); return true; }
            return false;
        case 'V':
            if (ctrl) { Paste(); return true; }
            return false;
        case VK_F3:
            NextHit(shift ? -1 : +1);
            return true;
    }
    return false;
}

static void OnKey(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (ctrl) {
        switch (vk) {
            case 'O': ShowOpenDialog(); return;
            case 'W': PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return;
            case 'P': PrintDocument(); return;
            case 'F': OpenSearch(); return;
            case 'G': if (g_hasDoc) FocusField(FLD_PAGE); return;
            case VK_OEM_COMMA: OpenPanel(!(g_panelOpen && g_panelWant)); return;
            case 'C': if (g_selActive) CopySelection(); return;
            case 'A': SelectAll(); return;
            case 'B': if (g_hasDoc) SetTocOpen(!g_tocOpen); return;
            case VK_OEM_PLUS: case VK_ADD: ZoomStep(+1); return;
            case VK_OEM_MINUS: case VK_SUBTRACT: ZoomStep(-1); return;
            case '0': case VK_NUMPAD0: ApplyFit(ZM_WIDTH); return;
            case '1': case VK_NUMPAD1:
                SetZoom(1.0, CenterX(), TB() + ViewH() / 2);
                sys::WriteDw(L"ZoomMode", ZM_CUSTOM); sys::WriteDw(L"Zoom", 100);
                return;
            case '2': case VK_NUMPAD2: ApplyFit(ZM_PAGE); return;
            case VK_HOME: if (g_hasDoc) ScrollTo(g_sx, 0, false); return;
            case VK_END: if (g_hasDoc) ScrollTo(g_sx, MaxSY(), false); return;
        }
    }
    switch (vk) {
        case VK_F11: if (g_hasDoc || g_fullscreen) ToggleFullscreen(); return;
        case VK_F3: NextHit(shift ? -1 : +1); return;
        case VK_ESCAPE:
            if (g_panelOpen) OpenPanel(false);
            else if (g_selActive) { g_selActive = false; Invalidate(); }
            else if (g_fullscreen) ToggleFullscreen();
            else if (g_searchOpen) CloseSearch();
            return;
    }
    if (!g_hasDoc) return;
    double page = ViewH() - 48;
    switch (vk) {
        case VK_DOWN: ScrollBy(0, 70, S.smooth); break;
        case VK_UP: ScrollBy(0, -70, S.smooth); break;
        case VK_NEXT: ScrollBy(0, page, S.smooth); break;
        case VK_PRIOR: ScrollBy(0, -page, S.smooth); break;
        case VK_SPACE: ScrollBy(0, shift ? -page : page, S.smooth); break;
        case VK_HOME: ScrollTo(g_sx, 0, false); break;
        case VK_END: ScrollTo(g_sx, MaxSY(), false); break;
        case VK_LEFT:
            if (MaxSX() > 0) ScrollBy(-70, 0, S.smooth); else GoToPage(CurrentPage() - 1);
            break;
        case VK_RIGHT:
            if (MaxSX() > 0) ScrollBy(70, 0, S.smooth); else GoToPage(CurrentPage() + 1);
            break;
    }
}

static void OnMouseMove(float x, float y) {
    if (!g_tracking) {
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, g_hwnd, 0};
        TrackMouseEvent(&tme);
        g_tracking = true;
    }
    if (g_sbDrag) {
        D2D1_RECT_F t;
        if (ThumbRect(t)) {
            float th = t.bottom - t.top;
            double frac = (y - g_sbGrab - TB()) / std::max(1.0f, ViewH() - th);
            ScrollTo(g_sx, frac * MaxSY(), false);
        }
        return;
    }
    if (g_selecting) {
        g_selMx = x;
        g_selMy = y;
        UpdateSelectionFromMouse();
        bool edge = y < TB() + 24 || y > g_ch - 24;  // am Rand automatisch weiterscrollen
        if (edge) SetTimer(g_hwnd, TIMER_AUTOSCROLL, 16, nullptr);
        else KillTimer(g_hwnd, TIMER_AUTOSCROLL);
        SetCursor(LoadCursor(nullptr, IDC_IBEAM));
        return;
    }
    if (g_panning) {
        POINT p;
        GetCursorPos(&p);
        float dx = PxToDip(p.x - g_panStart.x), dy = PxToDip(p.y - g_panStart.y);
        if (std::fabs(dx) + std::fabs(dy) > 4) g_panMoved = true;
        if (g_panMoved) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            ScrollTo(g_panSx - dx, g_panSy - dy, false);
        }
        return;
    }
    const Zone* z = ZoneAt(x, y);
    int hot = z ? z->cmd : CMD_NONE;
    bool sb = !z && InScrollbar(x, y);
    if (hot != g_hot || sb != g_sbHover) {
        g_hot = hot;
        g_sbHover = sb;
        Invalidate();
    }
    UpdateTooltip(z);
    LPCWSTR cur = IDC_ARROW;
    if (z && (z->cmd == CMD_SEARCHFIELD || z->cmd == CMD_PWFIELD)) cur = IDC_IBEAM;
    else if (z) cur = IDC_ARROW;
    else if (!sb && LinkAt(x, y)) cur = IDC_HAND;
    else if (!sb && S.mouseMode == 0) {
        int pg = PageAtPoint(x, y);
        bool on = false;
        if (pg >= 0 && g_pages[pg].textLoaded) CaretAt(pg, x, y, &on);
        if (on) cur = IDC_IBEAM;
    }
    SetCursor(LoadCursor(nullptr, cur));
}

static void OnLButtonDown(float x, float y) {
    SetFocus(g_hwnd);
    const Zone* z = ZoneAt(x, y);
    if (g_focus != FLD_NONE) {
        bool keep = z && ((g_focus == FLD_SEARCH && z->cmd >= CMD_SEARCHFIELD && z->cmd <= CMD_SEARCH_CLOSE) ||
                          (g_focus == FLD_PAGE && z->cmd == CMD_PAGEBOX) ||
                          (g_focus == FLD_PASSWORD && z->cmd >= CMD_PWFIELD && z->cmd <= CMD_PWCANCEL));
        if (!keep) FocusField(FLD_NONE);
    }
    if (z) {
        g_pressed = z->cmd;
        SetCapture(g_hwnd);
        Invalidate();
        return;
    }
    D2D1_RECT_F t;
    if (InScrollbar(x, y) && ThumbRect(t)) {
        if (y >= t.top && y < t.bottom) {
            g_sbDrag = true;
            g_sbGrab = y - t.top;
            SetCapture(g_hwnd);
        } else {
            ScrollBy(0, y < t.top ? -(ViewH() - 48) : (ViewH() - 48), S.smooth);
        }
        Invalidate();
        return;
    }
    if (g_hasDoc && y >= TB() && x >= ContentX0()) {
        int pg = PageAtPoint(x, y);
        bool shift = GetKeyState(VK_SHIFT) < 0;
        if (pg >= 0 && (S.mouseMode == 0 || shift)) {
            EnsureText(pg);
            bool on = false;
            int c = CaretAt(pg, x, y, &on);
            if ((shift && g_selActive) || on || (shift && S.mouseMode == 1)) {
                if (!(shift && g_selActive)) g_selA = {pg, c};
                g_selB = {pg, c};
                g_selActive = g_selA.page != g_selB.page || g_selA.idx != g_selB.idx;
                g_selecting = true;
                g_selMx = x;
                g_selMy = y;
                StopAnim();
                SetCapture(g_hwnd);
                Invalidate();
                return;
            }
        }
        if (g_selActive) {
            g_selActive = false;
            Invalidate();
        }
        g_panning = true;
        g_panMoved = false;
        GetCursorPos(&g_panStart);
        StopAnim();
        g_panSx = g_sx;
        g_panSy = g_sy;
        SetCapture(g_hwnd);
    }
}

static void OnLButtonUp(float x, float y) {
    // Zustand sichern – ReleaseCapture löst WM_CAPTURECHANGED aus, das ihn zurücksetzt
    int pressed = g_pressed;
    bool sbDrag = g_sbDrag, panning = g_panning, selecting = g_selecting;
    g_pressed = CMD_NONE;
    g_sbDrag = g_panning = g_selecting = false;
    if (GetCapture() == g_hwnd) ReleaseCapture();
    if (selecting) {
        KillTimer(g_hwnd, TIMER_AUTOSCROLL);
        if (!g_selActive) {  // nur geklickt: ggf. Link öffnen
            const LinkInfo* l = LinkAt(x, y);
            if (l) { LinkInfo copy = *l; FollowLink(copy); }
        }
        return;
    }
    if (pressed != CMD_NONE) {
        const Zone* z = ZoneAt(x, y);
        if (z && z->cmd == pressed) {
            D2D1_RECT_F r = z->r;
            Execute(pressed, r);
        }
        Invalidate();
        return;
    }
    if (sbDrag) {
        Invalidate();
        return;
    }
    if (panning) {
        if (!g_panMoved) {
            const LinkInfo* l = LinkAt(x, y);
            if (l) { LinkInfo copy = *l; FollowLink(copy); }
        }
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
    }
}

static float PanelLeft();

static void OnWheel(int delta, bool horizontal, float x, float y) {
    if (g_panelOpen) {
        if (!horizontal && x >= PanelLeft()) {
            float maxS = std::max(0.0f, g_panelContentH - (g_ch - TB()));
            g_panelScroll = std::clamp(g_panelScroll - delta / 120.0f * 60.0f, 0.0f, maxS);
            Invalidate();
        }
        return;
    }
    if (!g_hasDoc) return;
    if (x < ContentX0() && y >= TB()) {  // Inhaltsverzeichnis scrollen
        if (!horizontal) {
            g_tocScroll -= delta / 120.0f * 90.0f;
            Invalidate();
        }
        return;
    }
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (ctrl && !horizontal) {
        SetZoom(g_zoom * std::pow(1.12, delta / 120.0), x, y);
        sys::WriteDw(L"ZoomMode", ZM_CUSTOM);
        sys::WriteDw(L"Zoom", (DWORD)std::lround(g_zoom * 100));
        return;
    }
    UINT lines = 3;
    SystemParametersInfoW(horizontal ? SPI_GETWHEELSCROLLCHARS : SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    double step = lines == WHEEL_PAGESCROLL ? ViewH() - 48 : lines * 40.0;
    static const double kSpeed[] = {0.6, 1.0, 1.6};
    double d = delta / 120.0 * step * kSpeed[S.speed];
    bool smooth = S.smooth && (delta % 120) == 0;  // Präzisions-Touchpads liefern feine Schritte -> direkt folgen
    if (horizontal || shift) {
        // WM_MOUSEHWHEEL: positiv = nach rechts; Umschalt+Rad: Rad nach unten = nach rechts
        double dx = horizontal ? d : -d;
        if (S.invH) dx = -dx;
        ScrollBy(dx, 0, smooth);
    } else {
        double dy = -d;
        if (S.invV) dy = -dy;
        ScrollBy(0, dy, smooth);
    }
}

// ============================================================================
//  Fensterprozedur
// ============================================================================
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_customFrame) {
        // Fensterknöpfe selbst auslösen: DefWindowProc kennt ohne klassische Titelleiste deren Position nicht
        // und ignoriert sonst den Klick (u. a. auf das X).
        bool capBtn = wp == HTCLOSE || wp == HTMINBUTTON || wp == HTMAXBUTTON;
        if ((msg == WM_NCLBUTTONDOWN || msg == WM_NCLBUTTONDBLCLK) && capBtn) {
            g_ncPressed = (int)wp;
            return 0;
        }
        if (msg == WM_NCLBUTTONUP && capBtn) {
            int pressed = g_ncPressed;
            g_ncPressed = 0;
            if (pressed == (int)wp) {
                if (wp == HTCLOSE) PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0);
                else if (wp == HTMINBUTTON) PostMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                else PostMessageW(h, WM_SYSCOMMAND, IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE, 0);
            }
            return 0;
        }
        // Darstellung/Hover der Knöpfe (inkl. Snap-Layouts) übernimmt DWM
        LRESULT lr = 0;
        if (DwmDefWindowProc(h, msg, wp, lp, &lr)) return lr;
    }
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp && g_customFrame) {
                NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
                LONG top = p->rgrc[0].top;
                DefWindowProcW(h, msg, wp, lp);  // Standardränder links/rechts/unten
                p->rgrc[0].top = top;              // Titelleiste entfernen – die Werkzeugleiste übernimmt
                if (IsZoomed(h)) p->rgrc[0].top += FrameY();
                return 0;
            }
            break;
        case WM_NCHITTEST:
            if (g_customFrame) {
                LRESULT r = DefWindowProcW(h, msg, wp, lp);
                if (r != HTCLIENT) return r;
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(h, &pt);
                float x = PxToDip(pt.x), y = PxToDip(pt.y);
                if (int c = CaptionHit(x, y)) return c;
                if (!IsZoomed(h) && pt.y < DipToPx(5)) return HTTOP;
                if (y < TB() && !ZoneAt(x, y)) return HTCAPTION;  // freie Fläche der Leiste: Fenster ziehen
                return HTCLIENT;
            }
            break;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            ApplyTheme();
            break;
        case WM_CREATE: {
            g_hwnd = h;
            g_dpi = (float)GetWindowDpi(h);
            g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                                    h, nullptr, nullptr, nullptr);
            if (g_tip) {
                TOOLINFOW ti{};
                ti.cbSize = sizeof ti;
                ti.uFlags = TTF_SUBCLASS;
                ti.hwnd = h;
                ti.uId = 1;
                ti.lpszText = (LPWSTR)L"";
                SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
                SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_INITIAL, 500);
            }
            ApplyTheme();
            DragAcceptFiles(h, TRUE);
            GESTURECONFIG gc[] = {{GID_ZOOM, GC_ZOOM, 0},
                                  {GID_PAN,
                                   GC_PAN | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY | GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY |
                                       GC_PAN_WITH_INERTIA,
                                   GC_PAN_WITH_GUTTER}};
            SetGestureConfig(h, 0, 2, gc, sizeof(GESTURECONFIG));
            g_worker.Start(h);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            Paint();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: {
            if (g_rt) g_rt->Resize(D2D1::SizeU(std::max(1, (int)LOWORD(lp)), std::max(1, (int)HIWORD(lp))));
            UpdateClientSize();
            UpdateCaptionBounds();
            if (g_hasDoc) {
                if (g_zoomMode != ZM_CUSTOM && std::fabs(FitZoom(g_zoomMode) - g_zoom) > 1e-4) {
                    int m = g_zoomMode;
                    SetZoom(FitZoom(m), CenterX(), TB(), m);
                } else {
                    ClampScroll();
                    UpdateJobs();
                }
            }
            Invalidate();
            return 0;
        }
        case WM_DPICHANGED: {
            g_dpi = (float)HIWORD(wp);
            RECT* r = (RECT*)lp;
            if (g_rt) g_rt->SetDpi(g_dpi, g_dpi);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateClientSize();
            UpdateCaptionBounds();
            UpdateJobs();
            Invalidate();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {DipToPx(420), DipToPx(320)};
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) ApplyTheme();
            break;
        case WM_ACTIVATEAPP:
            if (wp) {
                bool d = sys::IsDefaultPdfApp();
                if (d != g_isDefault) { g_isDefault = d; Invalidate(); }
            }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;  // wird in WM_MOUSEMOVE gesetzt
            break;
        case WM_MOUSEMOVE: OnMouseMove(PxToDip(GET_X_LPARAM(lp)), PxToDip(GET_Y_LPARAM(lp))); return 0;
        case WM_MOUSELEAVE:
            g_tracking = false;
            if (g_hot != CMD_NONE || g_sbHover) { g_hot = CMD_NONE; g_sbHover = false; Invalidate(); }
            UpdateTooltip(nullptr);
            return 0;
        case WM_LBUTTONDOWN: OnLButtonDown(PxToDip(GET_X_LPARAM(lp)), PxToDip(GET_Y_LPARAM(lp))); return 0;
        case WM_LBUTTONDBLCLK: {
            float x = PxToDip(GET_X_LPARAM(lp)), y = PxToDip(GET_Y_LPARAM(lp));
            int pg = ZoneAt(x, y) ? -1 : PageAtPoint(x, y);
            if (pg >= 0) SelectWordAt(pg, x, y);  // Doppelklick markiert ein Wort
            else OnLButtonDown(x, y);
            return 0;
        }
        case WM_RBUTTONUP: {
            float x = PxToDip(GET_X_LPARAM(lp)), y = PxToDip(GET_Y_LPARAM(lp));
            if (g_hasDoc && !ZoneAt(x, y) && PageAtPoint(x, y) >= 0) {
                ShowContextMenu(x, y);
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP: OnLButtonUp(PxToDip(GET_X_LPARAM(lp)), PxToDip(GET_Y_LPARAM(lp))); return 0;
        case WM_CAPTURECHANGED:
            g_selecting = false;
            KillTimer(h, TIMER_AUTOSCROLL);
            g_panning = false;
            g_sbDrag = false;
            if (g_pressed != CMD_NONE) { g_pressed = CMD_NONE; Invalidate(); }
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            OnWheel(d, msg == WM_MOUSEHWHEEL, PxToDip(p.x), PxToDip(p.y));
            return 0;
        }
        case WM_GESTURE: {
            GESTUREINFO gi{};
            gi.cbSize = sizeof gi;
            if (GetGestureInfo((HGESTUREINFO)lp, &gi) && (gi.dwID == GID_ZOOM || gi.dwID == GID_PAN)) {
                POINT pt{gi.ptsLocation.x, gi.ptsLocation.y};
                ScreenToClient(h, &pt);
                if (gi.dwID == GID_ZOOM) {
                    float d = (float)(DWORD)gi.ullArguments;
                    if (!(gi.dwFlags & GF_BEGIN) && g_gestureDist > 0 && d > 0)
                        SetZoom(g_zoom * d / g_gestureDist, PxToDip(pt.x), PxToDip(pt.y));
                    g_gestureDist = d;
                    if (gi.dwFlags & GF_END) {
                        sys::WriteDw(L"ZoomMode", ZM_CUSTOM);
                        sys::WriteDw(L"Zoom", (DWORD)std::lround(g_zoom * 100));
                    }
                } else {
                    if (!(gi.dwFlags & GF_BEGIN))
                        ScrollBy(-PxToDip(pt.x - g_gestureLast.x), -PxToDip(pt.y - g_gestureLast.y), false);
                    g_gestureLast = pt;
                }
                CloseGestureInfoHandle((HGESTUREINFO)lp);
                return 0;
            }
            break;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (msg == WM_SYSKEYDOWN && wp != VK_F10) break;
            if (g_focus != FLD_NONE && FieldKey(wp)) return 0;
            OnKey(wp);
            return 0;
        case WM_CHAR:
            if (wp < 32 || wp == 127) return 0;
            if (g_focus != FLD_NONE) FieldChar((wchar_t)wp);
            else if (g_hasDoc && wp >= L'0' && wp <= L'9') {
                FocusField(FLD_PAGE);
                g_pageText = std::wstring(1, (wchar_t)wp);
                g_selAll = false;
                Invalidate();
            }
            return 0;
        case WM_TIMER:
            switch (wp) {
                case TIMER_ANIM: {
                    LARGE_INTEGER now, fq;
                    QueryPerformanceCounter(&now);
                    QueryPerformanceFrequency(&fq);
                    double dt = (double)(now.QuadPart - g_animLast.QuadPart) / fq.QuadPart;
                    g_animLast = now;
                    double f = 1.0 - std::exp(-std::min(dt, 0.1) / 0.055);
                    double dy = g_ty - g_sy, dx = g_tx - g_sx;
                    if (std::fabs(dy) < 0.5 && std::fabs(dx) < 0.5) {
                        g_sy = g_ty;
                        g_sx = g_tx;
                        g_animating = false;
                        KillTimer(h, TIMER_ANIM);
                    } else {
                        g_sy += dy * f;
                        g_sx += dx * f;
                    }
                    UpdateJobs();
                    Invalidate();
                    break;
                }
                case TIMER_ZOOM:
                    KillTimer(h, TIMER_ZOOM);
                    g_zoomSettling = false;
                    UpdateJobs();
                    break;
                case TIMER_SEARCH:
                    KillTimer(h, TIMER_SEARCH);
                    StartSearch();
                    break;
                case TIMER_TOAST:
                    KillTimer(h, TIMER_TOAST);
                    g_toast.clear();
                    Invalidate();
                    break;
                case TIMER_AUTOSCROLL: {
                    if (!g_selecting) {
                        KillTimer(h, TIMER_AUTOSCROLL);
                        break;
                    }
                    float y = g_selMy;
                    double dy = y < TB() + 24 ? -(TB() + 24 - y) * 0.6 - 4 : y > g_ch - 24 ? (y - (g_ch - 24)) * 0.6 + 4 : 0;
                    if (dy == 0) KillTimer(h, TIMER_AUTOSCROLL);
                    else {
                        ScrollBy(0, dy, false);
                        UpdateSelectionFromMouse();
                    }
                    break;
                }
                case TIMER_PANEL: {
                    LARGE_INTEGER now, fq;
                    QueryPerformanceCounter(&now);
                    QueryPerformanceFrequency(&fq);
                    float dt = (float)(now.QuadPart - g_panelLast.QuadPart) / fq.QuadPart;
                    g_panelLast = now;
                    g_panelT = std::clamp(g_panelT + (g_panelWant ? dt : -dt) / 0.16f, 0.0f, 1.0f);
                    if ((g_panelWant && g_panelT >= 1) || (!g_panelWant && g_panelT <= 0)) {
                        KillTimer(h, TIMER_PANEL);
                        if (!g_panelWant) g_panelOpen = false;
                    }
                    Invalidate();
                    break;
                }
                case TIMER_CARET:
                    if (g_focus == FLD_NONE) KillTimer(h, TIMER_CARET);
                    g_caretOn = !g_caretOn;
                    Invalidate();
                    break;
            }
            return 0;
        case WM_APP_RENDERED: {
            RenderResult* r = (RenderResult*)lp;
            if (r->gen == g_docGen && g_hasDoc && r->page < N() && r->color == S.pageColor) {
                PageInfo& p = g_pages[r->page];
                if (r->hasLinks && !p.linksLoaded) {
                    p.links = std::move(r->links);
                    p.linksLoaded = true;
                }
                if (EnsureRT()) {
                    ID2D1Bitmap* bmp = nullptr;
                    D2D1_BITMAP_PROPERTIES bp =
                        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
                    if (SUCCEEDED(g_rt->CreateBitmap(D2D1::SizeU(r->w, r->h), r->px.data(), r->w * 4, bp, &bmp))) {
                        SafeRelease(p.bmp);
                        p.bmp = bmp;
                        p.bmpScale = r->scale;
                        p.bmpW = r->w;
                        p.bmpH = r->h;
                    }
                }
                int a, b;
                VisibleRange(a, b);
                if (r->page >= a && r->page <= b) Invalidate();
            }
            delete r;
            return 0;
        }
        case WM_APP_SEARCH: {
            SearchResult* r = (SearchResult*)lp;
            if (r->id == g_searchId && r->gen == g_docGen && g_hasDoc) {
                if (!r->hits.empty()) {
                    auto pos = std::lower_bound(g_hits.begin(), g_hits.end(), r->page,
                                                [](const SearchHit& hh, int pg) { return hh.page < pg; });
                    int at = (int)(pos - g_hits.begin());
                    int cnt = (int)r->hits.size();
                    g_hits.insert(pos, std::make_move_iterator(r->hits.begin()), std::make_move_iterator(r->hits.end()));
                    if (g_curHit >= at) g_curHit += cnt;
                    if (g_curHit < 0) {
                        g_curHit = at;
                        RevealHit(g_curHit);
                    }
                }
                if (r->done) g_searchDone = true;
                Invalidate();
            }
            delete r;
            return 0;
        }
        case WM_APP_TEXT: {
            TextResult* r = (TextResult*)lp;
            if (r->gen == g_docGen && g_hasDoc && r->page < N() && !g_pages[r->page].textLoaded) {
                g_pages[r->page].chars = std::move(r->chars);
                g_pages[r->page].textLoaded = true;
                if (g_selActive) Invalidate();
            }
            delete r;
            return 0;
        }
        case WM_APP_TOAST: {
            std::wstring* s = (std::wstring*)lp;
            Toast(*s);
            delete s;
            return 0;
        }
        case WM_DROPFILES: {
            HDROP d = (HDROP)wp;
            UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; i++) {
                UINT len = DragQueryFileW(d, i, nullptr, 0);
                std::wstring p(len + 1, L'\0');
                DragQueryFileW(d, i, &p[0], len + 1);
                p.resize(len);
                if (i == 0) OpenDocument(p);
                else {
                    std::wstring a = L"\"" + p + L"\"";
                    ShellExecuteW(nullptr, L"open", sys::ExePath().c_str(), a.c_str(), nullptr, SW_SHOWNORMAL);
                }
            }
            DragFinish(d);
            SetForegroundWindow(h);
            return 0;
        }
        case WM_CLOSE:
            SavePlacement();
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            g_worker.Stop();
            CloseDocument();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ============================================================================
//  Einstieg
// ============================================================================
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring file;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--register") { sys::RegisterApp(sys::ExePath()); return 0; }
        if (a == L"--unregister") { sys::UnregisterApp(); return 0; }
        if (a == L"--install") { sys::Install(); sys::OpenDefaultAppsSettings(); return 0; }
        if (a == L"--uninstall") {
            sys::Uninstall();
            MessageBoxW(nullptr, L"FlinkPDF wurde entfernt.", L"FlinkPDF", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (file.empty() && !a.empty() && a[0] != L'-') file = a;
    }
    LocalFree(argv);

    std::wstring dll = sys::DirOf(sys::ExePath()) + L"\\pdfium.dll";
    if (!P.Load(dll.c_str())) {
        MessageBoxW(nullptr, L"pdfium.dll wurde nicht gefunden oder ist ungültig.\nSie muss im selben Ordner wie FlinkPDF.exe liegen.",
                    L"FlinkPDF", MB_OK | MB_ICONERROR);
        return 1;
    }
    P.InitLibrary();
    g_prefix.assign(1, 0.0);

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    CreateDeviceIndependent();

    S.Load();
    g_isWin11 = DetectWin11();
    sys::RefreshRegistration();
    g_isDefault = sys::IsDefaultPdfApp();
    g_installed = sys::IsInstalledCopy();
    g_recent = sys::LoadRecent();
    g_zoomMode = (int)sys::ReadDw(L"ZoomMode", ZM_WIDTH);
    if (g_zoomMode < 0 || g_zoomMode > 2) g_zoomMode = ZM_WIDTH;

    bool dark = S.theme == 0 ? sys::SystemUsesDarkTheme() : S.theme == 2;
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(ToColorRef(dark ? kDark.bg : kLight.bg));
    wc.lpszClassName = L"FlinkPDFWindow";
    RegisterClassExW(&wc);

    HDC sdc = GetDC(nullptr);
    float sysDpi = (float)GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);
    int w = (int)(1100 * sysDpi / 96), hgt = (int)(820 * sysDpi / 96);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"FlinkPDF", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, hgt,
                                nullptr, nullptr, hi, nullptr);
    if (!hwnd) return 1;

    // Fensterposition wiederherstellen
    WINDOWPLACEMENT wp{};
    DWORD sz = sizeof wp;
    bool maximize = false;
    if (RegGetValueW(HKEY_CURRENT_USER, sys::kSettingsKey, L"Placement", RRF_RT_REG_BINARY, nullptr, &wp, &sz) == ERROR_SUCCESS &&
        sz == sizeof wp && wp.length == sizeof wp) {
        maximize = wp.showCmd == SW_SHOWMAXIMIZED;
        wp.showCmd = SW_HIDE;
        wp.flags = 0;
        // Nur übernehmen, wenn das Fenster auf einem vorhandenen Monitor liegt
        if (MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONULL)) SetWindowPlacement(hwnd, &wp);
    }
    if (!file.empty()) OpenDocument(file);
    ShowWindow(hwnd, maximize ? SW_SHOWMAXIMIZED : (show == SW_SHOWMINIMIZED ? SW_SHOWNORMAL : show));
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
