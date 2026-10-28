// Windows-Integration: Registrierung als PDF-App, Installation, Einstellungen
#pragma once
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <vector>

namespace sys {

static const wchar_t* kProgId = L"FlinkPDF.Document";
static const wchar_t* kAppName = L"FlinkPDF";
static const wchar_t* kSettingsKey = L"Software\\FlinkPDF";
static const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FlinkPDF";

inline std::wstring ExePath() {
    std::wstring b(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &b[0], (DWORD)b.size());
    b.resize(n);
    return b;
}
inline std::wstring DirOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"." : p.substr(0, i);
}
inline std::wstring FileOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}
inline std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}
inline std::wstring InstallDir() {
    std::wstring base = KnownFolder(FOLDERID_UserProgramFiles);  // %LOCALAPPDATA%\Programs
    if (base.empty()) {
        std::wstring la = KnownFolder(FOLDERID_LocalAppData);
        if (!la.empty()) base = la + L"\\Programs";
    }
    if (base.empty()) return DirOf(ExePath());  // Notfall: am aktuellen Ort bleiben
    return base + L"\\FlinkPDF";
}
inline bool IsInstalledCopy() { return _wcsicmp(DirOf(ExePath()).c_str(), InstallDir().c_str()) == 0; }

inline void RegStr(const wchar_t* key, const wchar_t* name, const std::wstring& val) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
}
inline void RegDw(const wchar_t* key, const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_DWORD, &v, sizeof v);
}
inline bool ReadStr(HKEY root, const wchar_t* key, const wchar_t* name, std::wstring& out) {
    DWORD sz = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &sz) != ERROR_SUCCESS) return false;
    std::wstring b(sz / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, &b[0], &sz) != ERROR_SUCCESS) return false;
    b.resize(wcslen(b.c_str()));
    out = b;
    return true;
}
inline DWORD ReadDw(const wchar_t* name, DWORD def) {
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) return v;
    return def;
}
inline void WriteDw(const wchar_t* name, DWORD v) { RegDw(kSettingsKey, name, v); }

// ---- Registrierung als PDF-Programm (nur aktueller Benutzer, keine Adminrechte nötig)
inline void RegisterApp(const std::wstring& exe) {
    std::wstring q = L"\"" + exe + L"\"";
    std::wstring cmd = q + L" \"%1\"";
    RegStr(L"Software\\Classes\\FlinkPDF.Document", nullptr, L"PDF-Dokument");
    RegStr(L"Software\\Classes\\FlinkPDF.Document\\DefaultIcon", nullptr, q + L",0");
    RegStr(L"Software\\Classes\\FlinkPDF.Document\\shell", nullptr, L"open");
    RegStr(L"Software\\Classes\\FlinkPDF.Document\\shell\\open", L"FriendlyAppName", kAppName);
    RegStr(L"Software\\Classes\\FlinkPDF.Document\\shell\\open\\command", nullptr, cmd);
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Classes\\.pdf\\OpenWithProgids", kProgId, REG_NONE, nullptr, 0);
    RegStr(L"Software\\Classes\\Applications\\FlinkPDF.exe", L"FriendlyAppName", kAppName);
    RegStr(L"Software\\Classes\\Applications\\FlinkPDF.exe\\DefaultIcon", nullptr, q + L",0");
    RegStr(L"Software\\Classes\\Applications\\FlinkPDF.exe\\shell\\open\\command", nullptr, cmd);
    RegStr(L"Software\\Classes\\Applications\\FlinkPDF.exe\\SupportedTypes", L".pdf", L"");
    RegStr(L"Software\\FlinkPDF\\Capabilities", L"ApplicationName", kAppName);
    RegStr(L"Software\\FlinkPDF\\Capabilities", L"ApplicationDescription", L"Schneller, schlanker PDF-Viewer");
    RegStr(L"Software\\FlinkPDF\\Capabilities", L"ApplicationIcon", q + L",0");
    RegStr(L"Software\\FlinkPDF\\Capabilities\\FileAssociations", L".pdf", kProgId);
    RegStr(L"Software\\RegisteredApplications", kAppName, L"Software\\FlinkPDF\\Capabilities");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

inline void UnregisterApp() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\FlinkPDF.Document");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\FlinkPDF.exe");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Classes\\.pdf\\OpenWithProgids", kProgId);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\FlinkPDF\\Capabilities");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kAppName);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

inline bool IsRegistered(std::wstring* cmdOut = nullptr) {
    std::wstring c;
    bool ok = ReadStr(HKEY_CURRENT_USER, L"Software\\Classes\\FlinkPDF.Document\\shell\\open\\command", nullptr, c);
    if (cmdOut) *cmdOut = c;
    return ok;
}

// Falls die EXE verschoben wurde: Registrierung still aktualisieren
inline void RefreshRegistration() {
    std::wstring cmd;
    if (!IsRegistered(&cmd)) return;
    std::wstring want = L"\"" + ExePath() + L"\" \"%1\"";
    if (_wcsicmp(cmd.c_str(), want.c_str()) != 0) {
        // Nur aktualisieren, wenn die registrierte EXE nicht mehr existiert oder wir die installierte Kopie sind
        std::wstring regExe = cmd.size() > 2 && cmd[0] == L'"' ? cmd.substr(1, cmd.find(L'"', 1) - 1) : cmd;
        if (IsInstalledCopy() || GetFileAttributesW(regExe.c_str()) == INVALID_FILE_ATTRIBUTES) RegisterApp(ExePath());
    }
}

inline bool IsDefaultPdfApp() {
    const wchar_t* keys[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoiceLatest\\ProgId",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoiceLatest",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoice",
    };
    for (auto k : keys) {
        std::wstring v;
        if (ReadStr(HKEY_CURRENT_USER, k, L"ProgId", v)) {
            for (auto& ch : v) ch = towlower(ch);
            return v.find(L"flinkpdf") != std::wstring::npos;
        }
    }
    return false;
}

inline void OpenDefaultAppsSettings() {
    // Windows 11: springt direkt zur Seite von FlinkPDF; Windows 10: allgemeine Standard-Apps-Seite
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=FlinkPDF", nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
}

inline bool CreateShortcut(const std::wstring& exe, const std::wstring& lnk) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) return false;
    sl->SetPath(exe.c_str());
    sl->SetWorkingDirectory(DirOf(exe).c_str());
    sl->SetDescription(L"FlinkPDF – schneller PDF-Viewer");
    sl->SetIconLocation(exe.c_str(), 0);
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
        pf->Release();
    }
    sl->Release();
    return ok;
}

inline std::wstring ShortcutPath() { return KnownFolder(FOLDERID_Programs) + L"\\FlinkPDF.lnk"; }

// Kopiert FlinkPDF nach %LOCALAPPDATA%\Programs\FlinkPDF, registriert es und legt Startmenü-Eintrag an.
// Rückgabe: Pfad der installierten EXE (oder aktuelle EXE, falls Kopieren nicht möglich war)
inline std::wstring Install() {
    std::wstring src = ExePath(), srcDir = DirOf(src);
    std::wstring dir = InstallDir();
    std::wstring exe = dir + L"\\FlinkPDF.exe";
    if (!IsInstalledCopy()) {
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        bool ok = CopyFileW(src.c_str(), exe.c_str(), FALSE) != 0;
        ok = (CopyFileW((srcDir + L"\\pdfium.dll").c_str(), (dir + L"\\pdfium.dll").c_str(), FALSE) != 0 ||
              GetFileAttributesW((dir + L"\\pdfium.dll").c_str()) != INVALID_FILE_ATTRIBUTES) && ok;
        if (!ok && GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) exe = src;  // Fallback: Ort beibehalten
    } else {
        exe = src;
    }
    RegisterApp(exe);
    CreateShortcut(exe, ShortcutPath());
    std::wstring q = L"\"" + exe + L"\"";
    RegStr(kUninstKey, L"DisplayName", L"FlinkPDF");
    RegStr(kUninstKey, L"DisplayIcon", q + L",0");
    RegStr(kUninstKey, L"DisplayVersion", L"1.2.0");
    RegStr(kUninstKey, L"Publisher", L"Flexy06");
    RegStr(kUninstKey, L"InstallLocation", DirOf(exe));
    RegStr(kUninstKey, L"UninstallString", q + L" --uninstall");
    RegDw(kUninstKey, L"NoModify", 1);
    RegDw(kUninstKey, L"NoRepair", 1);
    RegDw(kUninstKey, L"EstimatedSize", 9000);
    return exe;
}

inline void Uninstall() {
    UnregisterApp();
    DeleteFileW(ShortcutPath().c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kSettingsKey);
    if (IsInstalledCopy()) {
        // Ordner nach Programmende löschen
        std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + InstallDir() + L"\"";
        STARTUPINFOW si{sizeof si};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring sysdir(MAX_PATH, L'\0');
        sysdir.resize(GetSystemDirectoryW(&sysdir[0], MAX_PATH));
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sysdir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
}

// ---- Zuletzt geöffnet
inline std::vector<std::wstring> LoadRecent() {
    std::vector<std::wstring> r;
    DWORD sz = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Recent", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &sz) != ERROR_SUCCESS) return r;
    std::vector<wchar_t> b(sz / sizeof(wchar_t) + 2, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Recent", RRF_RT_REG_MULTI_SZ, nullptr, b.data(), &sz) != ERROR_SUCCESS) return r;
    for (const wchar_t* p = b.data(); *p; p += wcslen(p) + 1) r.push_back(p);
    return r;
}
inline void SaveRecent(const std::vector<std::wstring>& r) {
    std::wstring m;
    for (auto& s : r) { m += s; m.push_back(0); }
    m.push_back(0);
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"Recent", REG_MULTI_SZ, m.data(), (DWORD)(m.size() * sizeof(wchar_t)));
}

inline bool SystemUsesDarkTheme() {
    DWORD v = 1, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

// Undokumentierte uxtheme-Funktionen für dunkle Kontextmenüs (Win10 1903+ / Win11)
inline void AllowDarkMenus(bool dark) {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    typedef int(WINAPI * SetPreferredAppMode_t)(int);
    typedef void(WINAPI * FlushMenuThemes_t)();
    auto setMode = (SetPreferredAppMode_t)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (FlushMenuThemes_t)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (flush) flush();
}

}  // namespace sys
