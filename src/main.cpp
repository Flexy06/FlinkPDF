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

enum { TIMER_ANIM = 1, TIMER_ZOOM = 2, TIMER_SEARCH = 3, TIMER_TOAST = 4, TIMER_CARET = 5 };

enum Cmd {
    CMD_NONE = 0, CMD_OPEN, CMD_PREV, CMD_NEXT, CMD_PAGEBOX, CMD_ZOOMOUT, CMD_ZOOMIN, CMD_ZOOMBOX, CMD_FIT,
    CMD_SEARCH, CMD_PRINT, CMD_MENU, CMD_SETDEFAULT, CMD_SEARCHFIELD, CMD_SEARCH_PREV, CMD_SEARCH_NEXT,
    CMD_SEARCH_CLOSE, CMD_PWFIELD, CMD_PWOK, CMD_PWCANCEL,
    CMD_RECENT = 100,  // + Index
    // Menübefehle
    MNU_DEFAULT = 1000, MNU_FULLSCREEN, MNU_LOCATION, MNU_UNREGISTER, MNU_ABOUT, MNU_OPENWITH,
    MNU_ZOOM = 1100,  // + Prozent
    MNU_FITWIDTH = 1099, MNU_FITPAGE = 1098,
};

enum ZoomMode { ZM_CUSTOM = 0, ZM_WIDTH = 1, ZM_PAGE = 2 };
enum Field { FLD_NONE, FLD_PAGE, FLD_SEARCH, FLD_PASSWORD };

static const float GAP = 14.0f;     // Abstand zwischen Seiten (DIP)
static const float TOOLBAR = 48.0f;  // Höhe der Werkzeugleiste (DIP)
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

struct PageInfo {
    float w = 612, h = 792;  // in Punkt
    ID2D1Bitmap* bmp = nullptr;
    float bmpScale = 0;
    int bmpW = 0, bmpH = 0;
    bool linksLoaded = false;
    std::vector<LinkInfo> links;
};

struct Job {
    int gen = -1, page = -1;
    float scale = 0;
    int w = 0, h = 0;
    bool links = false;
};

struct RenderResult {
    int gen, page;
    float scale;
    int w, h;
    std::vector<unsigned char> px;
    bool hasLinks = false;
    std::vector<LinkInfo> links;
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
static IDWriteTextFormat *fUi, *fUiC, *fUiR, *fUiEll, *fBold, *fSmall, *fSmallC, *fIcon, *fIconBig, *fTitle;
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

class Worker {
    std::mutex mx;
    std::condition_variable cv;
    std::deque<Job> jobs;
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
                if (j.page == inflight.page && j.gen == inflight.gen && j.scale == inflight.scale) continue;
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

  private:
    void Run() {
        for (;;) {
            Job job;
            bool doSearch = false, last = false;
            int page = 0, gen = 0, id = 0;
            std::wstring q;
            {
                std::unique_lock<std::mutex> lk(mx);
                cv.wait(lk, [&] { return quit || !jobs.empty() || sActive; });
                if (quit) return;
                if (!jobs.empty()) {
                    job = jobs.front();
                    jobs.pop_front();
                    inflight = job;
                } else {
                    doSearch = true;
                    page = (sStart + sIdx) % sN;
                    sIdx++;
                    last = sIdx >= sN;
                    if (last) sActive = false;
                    gen = sGen; id = sId; q = sQuery;
                }
            }
            if (!doSearch) {
                Render(job);
                std::lock_guard<std::mutex> lk(mx);
                inflight = Job();
            } else {
                Search(gen, id, page, q, last);
            }
        }
    }

    void Render(const Job& job) {
        RenderResult* res = new RenderResult{job.gen, job.page, job.scale, job.w, job.h, {}, false, {}};
        try {
            res->px.resize((size_t)job.w * job.h * 4);
        } catch (...) {
            delete res;
            return;
        }
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
            if (pg && job.links) {
                pdf::SIZEF sz{612, 792};
                P.GetPageSizeByIndexF(g_doc, job.page, &sz);
                ExtractLinks(pg, job, res->links, sz.w, sz.h);
            }
            res->hasLinks = job.links;
            if (pg) P.ClosePage(pg);
        }
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
static float ViewW() { return g_cw; }
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
    float x = Snap((ContentW() - w) / 2 - g_sx);
    float y = Snap(TB() + PageTop(i) - g_sy);
    return D2D1::RectF(x, y, x + (float)w, y + (float)h);
}

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
        jobs.push_back(j);
    }
    g_worker.SetJobs(jobs);

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
    bool anim = std::fabs(t - g_sy) < ViewH() * 2.5;
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
    double cx = g_sx + ax - ContentW() / 2;
    double rel = cx / oldK;
    g_zoom = z;
    double k = K();
    g_sy = GAP * (i + 1) + g_prefix[i] * k + off * k - vy;
    g_sx = rel * k + ContentW() / 2 - ax;
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
    SetZoom(FitZoom(mode), ViewW() / 2, TB(), mode);
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
    SetZoom(nz, ViewW() / 2, TB() + ViewH() / 2);
    sys::WriteDw(L"ZoomMode", ZM_CUSTOM);
    sys::WriteDw(L"Zoom", (DWORD)std::lround(g_zoom * 100));
}

// ============================================================================
//  Dokument öffnen/schließen
// ============================================================================
static void ReleaseBitmaps() {
    for (auto& p : g_pages) { SafeRelease(p.bmp); p.bmpScale = 0; }
}

static void CloseDocument() {
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
    SetWindowTextW(g_hwnd, L"FlinkPDF");
}

static void AddRecent(const std::wstring& path) {
    g_recent.erase(std::remove_if(g_recent.begin(), g_recent.end(),
                                  [&](const std::wstring& s) { return _wcsicmp(s.c_str(), path.c_str()) == 0; }),
                   g_recent.end());
    g_recent.insert(g_recent.begin(), path);
    if (g_recent.size() > 8) g_recent.resize(8);
    sys::SaveRecent(g_recent);
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

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
    g_zoom = g_zoomMode == ZM_CUSTOM ? sys::ReadDw(L"Zoom", 100) / 100.0 : FitZoom(g_zoomMode);
    g_zoom = std::clamp(g_zoom, 0.1, 6.0);
    g_sx = g_tx = MaxSX() / 2;
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
    ScrollTo(tx, ty, std::fabs(ty - g_sy) < ViewH() * 3);
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
static bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

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
    Fill(D2D1::RectF(0, 0, W, H), T.bar);
    Fill(D2D1::RectF(0, H - 1, W, H), T.barLine);
    const float bt = 6, bb = 42, bw = 36;

    IconBtn(D2D1::RectF(8, bt, 8 + bw, bb), CMD_OPEN, 0xE8E5, L"Öffnen (Strg+O)");
    float leftEnd = 8 + bw + 8;

    // Rechte Gruppe (von rechts nach links)
    float x = W - 8;
    auto place = [&](float w) { x -= w; return D2D1::RectF(x, bt, x + w, bb); };
    IconBtn(place(bw), CMD_MENU, 0xE712, L"Weitere Optionen");
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

static void DrawPages() {
    int a, b;
    VisibleRange(a, b);
    D2D1_RECT_F clip = D2D1::RectF(0, TB(), g_cw, g_ch);
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
            Fill(r, 0xFFFFFF);
        }
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
        float tx = (float)((ViewW() - tw) * (g_sx / MaxSX()));
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
    int nRecent = (int)g_recent.size();
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

static void Paint() {
    if (!EnsureRT()) return;
    g_zones.clear();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(Col(T.bg));
    if (g_hasDoc) DrawPages();
    else if (!g_pwPath.empty()) DrawPassword();
    else DrawEmpty();
    if (g_hasDoc) DrawScrollbars();
    if (g_hasDoc && g_searchOpen) DrawSearchPanel();
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

static void ApplyTheme() {
    g_dark = sys::SystemUsesDarkTheme();
    T = g_dark ? kDark : kLight;
    BOOL d = g_dark;
    if (FAILED(DwmSetWindowAttribute(g_hwnd, 20, &d, sizeof d))) DwmSetWindowAttribute(g_hwnd, 19, &d, sizeof d);
    COLORREF cap = ToColorRef(T.bar), txt = ToColorRef(T.text);
    DwmSetWindowAttribute(g_hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof cap);
    DwmSetWindowAttribute(g_hwnd, 36 /*DWMWA_TEXT_COLOR*/, &txt, sizeof txt);
    sys::AllowDarkMenus(g_dark);
    if (g_tip) SetWindowTheme(g_tip, g_dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    Invalidate();
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
    AppendMenuW(m, MF_STRING | (sys::IsRegistered() ? 0 : MF_GRAYED), MNU_UNREGISTER, L"PDF-Registrierung entfernen");
    AppendMenuW(m, MF_STRING, MNU_ABOUT, L"Über FlinkPDF");
    POINT pt{DipToPx(anchor.right), DipToPx(anchor.bottom)};
    ClientToScreen(g_hwnd, &pt);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (c) {
        case MNU_DEFAULT: SetDefaultApp(); break;
        case MNU_FULLSCREEN: ToggleFullscreen(); break;
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
                        L"FlinkPDF 1.0\nEin schneller, schlanker PDF-Viewer.\n\n"
                        L"Tastenkürzel:\n"
                        L"Strg+O  Öffnen\t\tStrg+F  Suchen\n"
                        L"Strg+P  Drucken\t\tStrg+G  Gehe zu Seite\n"
                        L"Strg +/−  Zoom\t\tStrg+0  Seitenbreite\n"
                        L"Strg+1  100 %\t\tStrg+2  Ganze Seite\n"
                        L"F11  Vollbild\t\tStrg+W  Schließen\n\n"
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
        SetZoom((c - MNU_ZOOM) / 100.0, ViewW() / 2, TB() + ViewH() / 2);
        sys::WriteDw(L"ZoomMode", ZM_CUSTOM);
        sys::WriteDw(L"Zoom", c - MNU_ZOOM);
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
        default:
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
            case VK_OEM_PLUS: case VK_ADD: ZoomStep(+1); return;
            case VK_OEM_MINUS: case VK_SUBTRACT: ZoomStep(-1); return;
            case '0': case VK_NUMPAD0: ApplyFit(ZM_WIDTH); return;
            case '1': case VK_NUMPAD1:
                SetZoom(1.0, ViewW() / 2, TB() + ViewH() / 2);
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
            if (g_fullscreen) ToggleFullscreen();
            else if (g_searchOpen) CloseSearch();
            return;
    }
    if (!g_hasDoc) return;
    double page = ViewH() - 48;
    switch (vk) {
        case VK_DOWN: ScrollBy(0, 70, true); break;
        case VK_UP: ScrollBy(0, -70, true); break;
        case VK_NEXT: ScrollBy(0, page, true); break;
        case VK_PRIOR: ScrollBy(0, -page, true); break;
        case VK_SPACE: ScrollBy(0, shift ? -page : page, true); break;
        case VK_HOME: ScrollTo(g_sx, 0, false); break;
        case VK_END: ScrollTo(g_sx, MaxSY(), false); break;
        case VK_LEFT:
            if (MaxSX() > 0) ScrollBy(-70, 0, true); else GoToPage(CurrentPage() - 1);
            break;
        case VK_RIGHT:
            if (MaxSX() > 0) ScrollBy(70, 0, true); else GoToPage(CurrentPage() + 1);
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
            ScrollBy(0, y < t.top ? -(ViewH() - 48) : (ViewH() - 48), true);
        }
        Invalidate();
        return;
    }
    if (g_hasDoc && y >= TB()) {
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
    bool sbDrag = g_sbDrag, panning = g_panning;
    g_pressed = CMD_NONE;
    g_sbDrag = g_panning = false;
    if (GetCapture() == g_hwnd) ReleaseCapture();
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

static void OnWheel(int delta, bool horizontal, float x, float y) {
    if (!g_hasDoc) return;
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
    double d = delta / 120.0 * step;
    bool smooth = (delta % 120) == 0;
    if (horizontal) ScrollBy(d, 0, smooth);
    else if (shift) ScrollBy(-d, 0, smooth);
    else ScrollBy(0, -d, smooth);
}

// ============================================================================
//  Fensterprozedur
// ============================================================================
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
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
            if (g_hasDoc) {
                if (g_zoomMode != ZM_CUSTOM && std::fabs(FitZoom(g_zoomMode) - g_zoom) > 1e-4) {
                    int m = g_zoomMode;
                    SetZoom(FitZoom(m), ViewW() / 2, TB(), m);
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
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: OnLButtonDown(PxToDip(GET_X_LPARAM(lp)), PxToDip(GET_Y_LPARAM(lp))); return 0;
        case WM_LBUTTONUP: OnLButtonUp(PxToDip(GET_X_LPARAM(lp)), PxToDip(GET_Y_LPARAM(lp))); return 0;
        case WM_CAPTURECHANGED:
            g_panning = false;
            g_sbDrag = false;
            if (g_pressed != CMD_NONE) { g_pressed = CMD_NONE; Invalidate(); }
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            OnWheel(msg == WM_MOUSEHWHEEL ? -d : d, msg == WM_MOUSEHWHEEL, PxToDip(p.x), PxToDip(p.y));
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
                case TIMER_CARET:
                    if (g_focus == FLD_NONE) KillTimer(h, TIMER_CARET);
                    g_caretOn = !g_caretOn;
                    Invalidate();
                    break;
            }
            return 0;
        case WM_APP_RENDERED: {
            RenderResult* r = (RenderResult*)lp;
            if (r->gen == g_docGen && g_hasDoc && r->page < N()) {
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

    sys::RefreshRegistration();
    g_isDefault = sys::IsDefaultPdfApp();
    g_installed = sys::IsInstalledCopy();
    g_recent = sys::LoadRecent();
    g_zoomMode = (int)sys::ReadDw(L"ZoomMode", ZM_WIDTH);
    if (g_zoomMode < 0 || g_zoomMode > 2) g_zoomMode = ZM_WIDTH;

    bool dark = sys::SystemUsesDarkTheme();
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
