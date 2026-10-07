#!/usr/bin/env python3
"""
Erzeugt conf/autotravel.conf.dist aus der Optionsregistry in
src/AutoTravel_Config.cpp.

Die Registry (sOptions[]) ist die EINZIGE Stelle, an der ein Wert definiert wird.
Die .dist darf deshalb nicht von Hand gepflegt werden -- sie wuerde sonst von der
Registry abweichen, und ein Wert, der im Spiel per ".at set" erreichbar ist,
fehlte in der Konfigurationsdatei (oder umgekehrt).

Verwendung:
    tools/gen_conf.py            schreibt conf/autotravel.conf.dist neu
    tools/gen_conf.py --check    bricht mit Fehler ab, wenn die Datei veraltet ist

Neue Werte eintragen:
    1. Feld in "struct ATConfig" (src/AutoTravel.h)
    2. Zeile in sOptions[] (src/AutoTravel_Config.cpp), in der passenden Gruppe
       (Gruppen sind durch Leerzeilen getrennt)
    3. dieses Skript ausfuehren
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "src" / "AutoTravel_Config.cpp"
OUTPUT = ROOT / "conf" / "autotravel.conf.dist"

# Ueberschriften der Gruppen, in der Reihenfolge der Registry. Die Zahl der
# Gruppen muss stimmen; das Skript prueft es.
GROUP_TITLES = [
    "Grundbetrieb",
    "Uebergabe zwischen Spieler und Autopilot",
    "Feststecken",
    "Reittiere",
    "Fliegen mit eigenem Flugmount",
    "Flugmeister",
    "Transporte: Zeppelin, Schiff, Tiefenbahn",
    "Schwimmen",
    "Bodenkontakt und Etagen",
    "Reiseknoten aus mod-playerbots",
    "Teleport",
    "Bewertung natuerlicher Wege",
    "Suche um Hindernisse herum",
]

HEADER = """\
#
# mod-autotravel  --  Konfiguration
# ---------------------------------------------------------------------------
# ERZEUGTE DATEI. Nicht von Hand aendern -- sie entsteht aus der Optionsregistry
# in src/AutoTravel_Config.cpp (Werkzeug: tools/gen_conf.py).
#
# Diese Datei nach autotravel.conf kopieren und DORT aendern. Die .dist wird
# bei einem Update ueberschrieben, die .conf nicht.
#
# Jeder Wert laesst sich zusaetzlich im laufenden Betrieb setzen:
#
#     .at options            zeigt alle Werte mit ihrem aktuellen Stand
#     .at options fly        zeigt nur Werte, die zum Fliegen gehoeren
#     .at set fly 0          schaltet das Fliegen ab
#
# Aenderungen im laufenden Betrieb gelten bis zum naechsten Serverneustart und
# brauchen Spielleiterrechte. Ausgenommen sind 'arrival' und 'grace': die
# gelten nur fuer die eigene Reise und darf jeder Spieler setzen. 'teleportsec'
# vergibt Rechte und braucht Administratorrechte.
#
# VORAUSSETZUNGEN
#   * mmaps muessen erzeugt und in worldserver.conf aktiviert sein
#     (MoveMaps.Enable = 1). Ohne sie kann der Server keinen Weg berechnen.
#   * vmaps werden fuer die Hoehen gebraucht (Vmap.EnableHeight = 1).
#   * dbc/WorldMapArea.dbc muss unter DataDir liegen. AzerothCore legt dafuer
#     keinen eigenen Speicher an, deshalb liest dieses Modul die Datei selbst.
#   * mod-playerbots ist optional. Ist es vorhanden, benutzt AutoTravel seinen
#     Reiseknotengraphen; fehlt es, faellt es auf die Carbonite-Route zurueck.
#
# ---------------------------------------------------------------------------

[worldserver]

"""

FOOTER = """\

#--------------------------------------------------------------------------
#   Reiseknoten: Datenbankname
#--------------------------------------------------------------------------

#    Datenbank, in der mod-playerbots die Tabellen playerbots_travelnode
#    und playerbots_travelnode_link haelt. Der Weltserver braucht dort
#    Leserecht. Erlaubt sind Buchstaben, Ziffern, '_' und '$'; sonst gilt
#    der Standardwert.
AutoTravel.NodeDatabase = "acore_playerbots"
"""

ROW = re.compile(
    r'^\s*\{\s*"([^"]+)",\s*"([^"]+)",\s*(OPT_\w+),\s*AT_OFF\((\w+)\),\s*'
    r'([^,]+),\s*([^,]+),\s*([^,]+),\s*"([^"]*)"\s*\},\s*$'
)


def parse_registry():
    text = REGISTRY.read_text(encoding="utf-8")
    start = text.index("ATOption const sOptions[] =")
    body = text[start:]
    body = body[body.index("{") + 1:]
    body = body[: body.index("};")]

    groups, current = [], []
    for line in body.splitlines():
        stripped = line.strip()
        if not stripped:
            if current:
                groups.append(current)
                current = []
            continue
        if stripped.startswith("//"):
            continue
        m = ROW.match(line)
        if not m:
            raise SystemExit("Registryzeile nicht lesbar: " + line)
        key, conf, typ, field, lo, hi, default, helptext = m.groups()
        current.append(
            dict(key=key, conf=conf, type=typ, field=field,
                 lo=float(lo), hi=float(hi), default=float(default), help=helptext)
        )
    if current:
        groups.append(current)
    return groups


def fmt(value, typ):
    if typ in ("OPT_UINT", "OPT_BOOL"):
        return str(int(value))
    # %g macht aus 1000000 "1e+06" -- fuer eine Konfigurationsdatei unleserlich.
    # Ganze Zahlen werden ausgeschrieben.
    if value == int(value) and abs(value) < 1e12:
        return str(int(value))
    return "%g" % value


def render(groups):
    if len(groups) != len(GROUP_TITLES):
        raise SystemExit(
            "Die Registry hat %d Gruppen, GROUP_TITLES %d. Bitte tools/gen_conf.py anpassen."
            % (len(groups), len(GROUP_TITLES))
        )

    keys = [o["key"] for g in groups for o in g]
    confs = [o["conf"] for g in groups for o in g]
    if len(set(keys)) != len(keys):
        raise SystemExit("Doppelter Laufzeitschluessel in der Registry.")
    if len(set(confs)) != len(confs):
        raise SystemExit("Doppelter Konfigurationsname in der Registry.")

    out = [HEADER]
    for title, group in zip(GROUP_TITLES, groups):
        out.append("#--------------------------------------------------------------------------\n")
        out.append("#   %s\n" % title)
        out.append("#--------------------------------------------------------------------------\n\n")
        for o in group:
            out.append("#    %s\n" % o["help"])
            if o["type"] == "OPT_BOOL":
                out.append("#    0 = aus, 1 = an\n")
            else:
                out.append("#    erlaubt: %s bis %s\n" % (fmt(o["lo"], o["type"]), fmt(o["hi"], o["type"])))
            out.append("#    Laufzeitschluessel: %s\n" % o["key"])
            out.append("%s = %s\n\n" % (o["conf"], fmt(o["default"], o["type"])))
    out.append(FOOTER)
    text = "".join(out)

    # Zwischen letzter Option und Fusszeile genau eine Leerzeile.
    text = text.replace("\n\n\n#-----", "\n\n#-----")
    return text


def main():
    groups = parse_registry()
    text = render(groups)

    if "--check" in sys.argv:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != text:
            print("conf/autotravel.conf.dist ist nicht aktuell. "
                  "Bitte 'tools/gen_conf.py' ausfuehren.", file=sys.stderr)
            return 1
        n = sum(len(g) for g in groups)
        print("conf/autotravel.conf.dist ist aktuell (%d Werte)." % n)
        return 0

    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print("%s geschrieben (%d Werte)." % (OUTPUT.relative_to(ROOT), sum(len(g) for g in groups)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
