#!/usr/bin/env bash
#
# Prueft das Modul, ohne AzerothCore zu bauen und ohne einen Server zu starten.
#
#   tools/check.sh <pfad-zu-azerothcore>      (oder AC_DIR=... tools/check.sh)
#
# Drei Schritte:
#   1. conf/autotravel.conf.dist stimmt mit der Optionsregistry ueberein
#   2. jede Quelldatei uebersetzt (nur Syntax und Typen, kein Linken) gegen die
#      ECHTEN Header des angegebenen AzerothCore-Standes
#   3. tools/tests/util_test.cpp laeuft als eigenes Programm
#
# Schritt 2 ist der eigentliche Wert: Aendert AzerothCore eine Schnittstelle, die
# das Modul benutzt, faellt es hier auf -- nicht erst beim Bauen des Servers.
# Es ersetzt aber keinen echten Bau und keinen Test im Spiel.
#
# Benoetigt: g++ (C++20), python3, die Entwicklungspakete, die AzerothCore selbst
# braucht (Boost, OpenSSL, MySQL-Client).

set -u

MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AC_DIR="${1:-${AC_DIR:-}}"

if [ -z "$AC_DIR" ] || [ ! -d "$AC_DIR/src/server/game" ]; then
    echo "Verwendung: tools/check.sh <pfad-zu-azerothcore>" >&2
    echo "(ein Checkout von azerothcore-wotlk, ein flacher Klon genuegt)" >&2
    exit 2
fi

CXX="${CXX:-g++}"
FAILED=0

# Include-Verzeichnisse wie sie AzerothCores CMake zusammenstellt.
INC=()
while IFS= read -r d; do INC+=("-I$d"); done < <(
    find "$AC_DIR/src/common" "$AC_DIR/src/server/game" "$AC_DIR/src/server/shared" \
         "$AC_DIR/src/server/database" -type d)
for d in g3dlite/include fmt/include recastnavigation/Detour/Include \
         recastnavigation/Recast/Include utf8cpp threads/include; do
    [ -d "$AC_DIR/deps/$d" ] && INC+=("-I$AC_DIR/deps/$d")
done
[ -d /usr/include/mysql ] && INC+=("-I/usr/include/mysql")
INC+=("-I$MODULE_DIR/src")

# gnu++20, nicht c++20: die Fehlermakros des Cores brauchen ##__VA_ARGS__.
CXXFLAGS=(-std=gnu++20 -Wall -Wextra -Wno-unused-parameter)

echo "== 1/3 Konfigurationsdatei"
python3 "$MODULE_DIR/tools/gen_conf.py" --check || FAILED=1

echo "== 2/3 Uebersetzen gegen die Header von $AC_DIR"
for f in "$MODULE_DIR"/src/*.cpp; do
    out=$("$CXX" "${CXXFLAGS[@]}" -fsyntax-only "${INC[@]}" "$f" 2>&1)
    rc=$?
    # Warnungen in fremden Headern sind nicht Sache des Moduls: nur die eigenen zaehlen.
    own=$(printf '%s\n' "$out" | grep -E "^$MODULE_DIR/.*(warning|error)" || true)
    if [ $rc -ne 0 ]; then
        echo "   FEHLER  $(basename "$f")"
        printf '%s\n' "$out" | grep -E "error" | head -20
        FAILED=1
    elif [ -n "$own" ]; then
        echo "   WARNUNG $(basename "$f")"
        printf '%s\n' "$own" | head -10
        FAILED=1
    else
        echo "   ok      $(basename "$f")"
    fi
done

echo "== 3/3 Unit-Tests"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT
if "$CXX" "${CXXFLAGS[@]}" "${INC[@]}" \
        "$MODULE_DIR/tools/tests/util_test.cpp" "$MODULE_DIR/src/AutoTravel_Util.cpp" \
        -o "$BUILD_DIR/util_test" 2> "$BUILD_DIR/build.log"; then
    "$BUILD_DIR/util_test" || FAILED=1
else
    echo "   Bauen des Tests fehlgeschlagen:"
    grep -E "error|undefined" "$BUILD_DIR/build.log" | head -20
    FAILED=1
fi

echo
if [ $FAILED -ne 0 ]; then
    echo "FEHLGESCHLAGEN"
    exit 1
fi
echo "alles in Ordnung"
