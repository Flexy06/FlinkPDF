#!/bin/sh
# Baut FlinkPDF und erstellt dist/FlinkPDF.zip (inkl. pdfium.dll aus dem pypdfium2-Wheel)
set -e
cd "$(dirname "$0")"
sh ./build.sh
rm -rf dist && mkdir -p dist/tmp dist/FlinkPDF/Lizenzen
pip download pypdfium2 --platform win_amd64 --only-binary=:all: --no-deps -d dist/tmp -q
python3 -m zipfile -e dist/tmp/pypdfium2-*.whl dist/tmp/wheel
cp dist/tmp/wheel/pypdfium2_raw/pdfium.dll dist/FlinkPDF/
cp dist/tmp/wheel/pypdfium2-*.dist-info/licenses/data/windows_x64/BUILD_LICENSES/*.txt dist/FlinkPDF/Lizenzen/
cp build/FlinkPDF.exe dist/FlinkPDF/
cp LIESMICH.txt dist/FlinkPDF/
cd dist && rm -rf tmp && python3 -m zipfile -c FlinkPDF.zip FlinkPDF
ls -la FlinkPDF.zip
