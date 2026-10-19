<p align="center"><img src="docs/logo.svg" width="96" alt="FlinkPDF-Logo"></p>
<h1 align="center">FlinkPDF</h1>
<p align="center">Ein schneller, schlanker PDF-Viewer für Windows 10/11 – startet sofort, sieht modern aus und lässt sich als Standard-App für PDFs festlegen.</p>

## Warum?

Acrobat und Co. brauchen gefühlt ewig zum Starten. FlinkPDF ist eine native Win32-App (~450 KB) mit Direct2D-Oberfläche. Gerendert wird mit [PDFium](https://pdfium.googlesource.com/pdfium/) – der PDF-Engine aus Chrome – in einem Hintergrund-Thread, sodass Scrollen auch bei Dokumenten mit Hunderten Seiten flüssig bleibt. Keine Laufzeitumgebung, kein Installer nötig.

## Funktionen

- Fortlaufendes Scrollen mit sanfter Animation, Ziehen mit der Maus, Touch- und Touchpad-Gesten
- Zoom (Strg+Mausrad, Pinch, Menü), „Seitenbreite“ und „Ganze Seite“ – die Einstellung wird gemerkt
- Suche im Dokument mit Trefferhervorhebung (Strg+F, F3)
- Klickbare Links und Inhaltsverzeichnisse
- Seitensprung (Seitenzahl einfach eintippen), Vollbild (F11), Drucken (Strg+P)
- Passwortgeschützte PDFs
- Heller und dunkler Modus passend zu Windows
- „Zuletzt geöffnet“, Drag & Drop
- Ein-Klick-Installation nach `%LOCALAPPDATA%\Programs` (ohne Adminrechte) inkl. Startmenü-Eintrag, Registrierung als PDF-App und Deinstallation über die Windows-Einstellungen

## Installation

1. [Neuestes Release](../../releases/latest) herunterladen und entpacken.
2. `FlinkPDF.exe` starten (bei SmartScreen-Warnung: „Weitere Informationen“ → „Trotzdem ausführen“ – die EXE ist nicht signiert).
3. **„Installieren & als Standard festlegen“** klicken und in den Windows-Einstellungen bei `.pdf` FlinkPDF auswählen.

Kommandozeile: `FlinkPDF.exe --install`, `--uninstall`, `--register`, `--unregister`.

## Tastenkürzel

| Kürzel | Aktion |
|---|---|
| Strg+O | Öffnen |
| Strg+F / F3 / Umschalt+F3 | Suchen / nächster / vorheriger Treffer |
| Strg+G oder Ziffern tippen | Gehe zu Seite |
| Strg + / Strg − | Zoom |
| Strg+0 / Strg+1 / Strg+2 | Seitenbreite / 100 % / Ganze Seite |
| Strg+P | Drucken |
| F11 | Vollbild |
| Strg+W | Fenster schließen |

## Selbst bauen

Gebaut wird per Cross-Compile mit MinGW-w64 (z. B. unter Ubuntu/WSL):

```sh
sudo apt install g++-mingw-w64-x86-64-posix mingw-w64-tools python3-pil
sh build.sh      # -> build/FlinkPDF.exe
sh package.sh    # -> dist/FlinkPDF.zip inkl. pdfium.dll
```

`pdfium.dll` stammt aus dem [pypdfium2](https://pypi.org/project/pypdfium2/)-Wheel (Builds von [bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries)) und muss neben der EXE liegen. Die GitHub Action baut bei jedem Push automatisch und hängt bei Tags (`v*`) das ZIP an ein Release.

### Aufbau

- `src/main.cpp` – Fenster, Direct2D-Zeichnung, Eingabe, Render-/Such-Thread
- `src/pdfium.h` – dynamisch geladene PDFium-Funktionen
- `src/system.h` – Registrierung als PDF-App, Installation, Einstellungen (Registry)
- `res/` – Manifest (Per-Monitor-DPI v2), Versionsinfo; das Icon erzeugt `res/mkicon.py` beim Bauen

## Lizenzen

PDFium und seine Abhängigkeiten stehen unter BSD-3-Clause/Apache-2.0 u. a.; die Lizenztexte liegen dem Release im Ordner `Lizenzen` bei.
