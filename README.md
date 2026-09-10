# mod-autotravel

Serverseitige Wegfindung und Fortbewegung fuer AzerothCore 3.3.5a (Client 3.3.5a,
Build 12340). Gegenstueck zum Client-Addon **AutoTravel**, das mit **Carbonite
3.34** zusammenarbeitet.

```
Carbonite (Client)  ->  wohin        gesetztes Ziel und die Wegpunkte dorthin
Addon               ->  bedienen     auslesen, umrechnen, Panel, Ruhe erkennen
mod-autotravel      ->  wie          NavMesh, Gelaende, Spline, Reittier,
                                     Flugmount, Flugmeister, Transporte
```

---

## Warum die Bewegung auf dem Server liegt

Ein 3.3.5a-Addon kann den Charakter nicht bewegen. `MoveForwardStart()`,
`TurnLeftStart()` und alle verwandten Funktionen sind protected; aus Addoncode
gerufen werden sie als "tainted" abgewiesen. Lua kennt ausserdem weder
Gelaendehoehen noch Kollisionsgeometrie noch das NavMesh -- es gibt keine
Moeglichkeit, aus einem Addon heraus zu erfahren, ob ein Punkt begehbar ist.
Genau das ist aber der Kern der Wegfindung.

Deshalb liefert der Client das Ziel, und der Server faehrt.

---

## Voraussetzungen

| Was | Warum |
|---|---|
| `MoveMaps.Enable = 1` in der worldserver.conf, mmaps erzeugt | ohne NavMesh kein Weg |
| `vmap.enableHeight = 1`, vmaps erzeugt | ohne VMaps keine Bruecken, Stege und Etagen |
| `dbc/WorldMapArea.dbc` unter `DataDir` | AzerothCore legt dafuer keinen Speicher an, das Modul liest die Datei selbst |
| Carbonite 3.34 im Client | Zielquelle |
| mod-playerbots | **optional**: liefert den Reiseknotengraphen |

Fehlt eine dieser Voraussetzungen, sagt das Modul beim Start im Log, was es
nicht gefunden hat. `/at diag` im Spiel zeigt dasselbe fuer ein konkretes Ziel.

---

## Einbau

```bash
cd /pfad/zu/azerothcore/modules
git clone <dieses-repo> mod-autotravel

cd /pfad/zu/azerothcore/build
cmake ..            # das Modul wird automatisch mit eingesammelt
make -j$(nproc)
make install
```

Danach:

```bash
cp /pfad/zu/etc/modules/autotravel.conf.dist \
   /pfad/zu/etc/modules/autotravel.conf
```

Die `.dist` wird bei einem Update ueberschrieben, die `.conf` nicht.

Das Modul braucht **keine SQL-Datei** und legt keine eigenen Tabellen an. Ist
mod-playerbots vorhanden, liest es dessen Tabellen `playerbots_travelnode` und
`playerbots_travelnode_link` nur lesend; der Weltserver braucht dafuer
Leserecht auf diese Datenbank (Name in `AutoTravel.NodeDatabase`).

---

## Was das Modul kann

### Wegfindung

* Mehrere Kandidaten je Ziel: verschiedene Zielhoehen (Gelaende, Steg, Bruecke,
  Galerie), geglaettet und als Eckpunkte. Bewertet wird alles, gewaehlt der
  beste -- nicht der erste gueltige.
* Bewertung in Yards: Wegstrecke plus Aufschlaege fuer Steigung, anhaltenden
  Anstieg, scharfe Kurven, Klippen und Schwimmstrecke. Dadurch gewinnt ein
  Umweg von 30 Yards um einen Hang herum, ein Umweg von 3 Kilometern nicht.
* Sieht der beste Weg nach einem Berganstieg aus, werden zusaetzlich vier Wege
  seitlich daran vorbei geprueft (links/rechts, nah/weit).

### Gelaende, Etagen und Wasser

* An jeder Stelle werden bis zu vier begehbare Ebenen gesucht statt nur einer.
  Das ist der Unterschied zwischen "auf der Kanalbruecke" und "im Kanal".
* Die Auswahl richtet sich nach dem **Routenverlauf**, nicht nach der
  Spielerposition. Ist der Charakter einmal durch eine Treppenstufe gerutscht,
  bliebe er sonst bis zum Ende der Treppe darunter.
* Wasserpunkte werden an die Oberflaeche gelegt, damit der Charakter schwimmt
  statt ueber den Seeboden zu laufen und zu ertrinken.
* Verliert er trotzdem den Bodenkontakt, wird er zurueckgesetzt -- aber nur
  innerhalb eines engen Fensters um die Pfadhoehe. Liegt die naechste Flaeche
  eine Etage tiefer, geschieht ausdruecklich **nichts**.

### Reisemittel

* **Reittier**: das schnellste passende aus dem Zauberbuch, nur draussen, nicht
  im Wasser, nicht im Kampf. In Gebaeuden wird abgesessen.
* **Eigenes Flugmount**: wo Fliegen erlaubt ist, wird eine Luftroute mit
  Hoehenprofil geflogen -- Steigflug, Reiseflug ueber dem hoechsten Hindernis,
  Sinkflug. Ob Fliegen erlaubt ist, entscheidet der Core beim Aufsitzen
  (`CanFly()`); in Azeroth gewaehrt dasselbe Mount nur Bodentempo, dann wird
  gelaufen.
* **Flugmeister**: das Modul sucht die guenstigste Verbindung zwischen zwei
  **bekannten** Flugpunkten (Dijkstra ueber `sTaxiPathSetBySource`), laeuft hin
  und startet den Flug selbst. Nur, wenn er einen einstellbaren Anteil der
  Laufstrecke spart und unter der Preisgrenze bleibt.

  Bekannt heisst: er steht in der Flugpunktmaske des Charakters. Das ist
  dieselbe Maske, die `.cheat taxi on` vollstaendig setzt -- ein Spielleiter mit
  Taxi-Cheat hat damit alle Punkte, und die Automatik benutzt sie. Das braucht
  keinen Sonderfall im Code: die Maske ist die einzige Wahrheit, und der Core
  prueft sie beim Abflug noch einmal selbst.

  Auch Flugverbindungen aus dem Knotengraphen von mod-playerbots werden gegen
  diese Maske geprueft, **bevor** die Reise beginnt. Der Graph beschreibt, was
  es an Verbindungen gibt, nicht, was ein bestimmter Charakter nehmen kann.
  Faellt eine durch, wird sie gesperrt und die Route neu gesucht.
* **Zeppelin, Schiff, Tiefenbahn**: das Modul bringt den Charakter zum Anleger
  und pausiert. Steht er auf einem Transport, wird gewartet; steigt er aus,
  wird von der neuen Position aus weitergerechnet. Einsteigen muss der Spieler
  selbst -- ein Transport ist ein bewegtes Objekt, auf das keine Wegfindung
  fuehrt.
* **Portale**: ueber den Knotengraphen von mod-playerbots als Sonderverbindung.

### Uebergabe an den Spieler

Der Autopilot haelt die Clientkontrolle **nur, solange er faehrt**. Sie geht
sofort zurueck bei:

* Pausenknopf im Addon (`.at pause`)
* Kampfbeginn
* Tod, Kartenwechsel, Fahrzeug, Teleport
* Taxiflug und Transport
* Verlassen der Welt

Der Playerbot wird dabei **nicht** angefasst. Das ist Absicht: pausiert der
Autopilot wegen eines Kampfes, soll der Selbstmodus weiterlaufen und sich
wehren koennen.

Solange der Autopilot faehrt, wird ausserdem das **AFK-Kennzeichen** geloescht.
Der Client setzt es nach ein paar Minuten ohne Tastendruck von selbst; waehrend
einer Reise ist das falsch, und im Schlachtfeld fuehrt es zum Hinauswurf.
Waehrend `AT_PLAYER_CONTROL` bleibt es dagegen erhalten -- dann ist der Spieler
tatsaechlich weg. Abschaltbar mit `AutoTravel.SuppressAfk = 0`.

Zurueck uebernimmt der Autopilot nur auf Ansage. Ist das Addon vorhanden,
wartet er auch nach einem Kampf auf dessen Ruhemeldung, statt dem Spieler die
Steuerung nach zwei Sekunden wieder wegzunehmen. Ohne Addon faehrt er nach der
eingestellten Wartezeit von selbst weiter -- es gaebe sonst niemanden, der die
Meldung schicken koennte.

---

## Befehle

Alle Unterbefehle liegen unter `.at` und sind fuer Spieler offen. Zwei
Ausnahmen:

* `.at tp` verlangt die Rechtestufe aus `AutoTravel.TeleportSecurity`
  (Standard 2 = Spielleiter). Der Befehl umgeht jede Wegfindung und nimmt
  beliebige Zielkoordinaten entgegen.
* `.at set` verlangt Spielleiterrechte, ausser fuer `arrival` und `grace` --
  die gelten nur fuer die eigene Reise.

| Befehl | Wirkung |
|---|---|
| `.at` | Status |
| `.at hello` | Handschlag; das Addon fragt damit ab, ob das Modul da ist |
| `.at pause` / `.at resume` | Uebergabe an den Spieler und zurueck |
| `.at stop` / `.at repath` | Reise beenden / Pfad neu rechnen |
| `.at diag ...` | warum scheitert der Pfad zu diesem Ziel? |
| `.at nodes` | Zustand des Reiseknotengraphen |
| `.at taxi` | bekannte Flugpunkte, Preisgrenze |
| `.at options [suchwort]` | alle Einstellungen mit aktuellem Stand |
| `.at set <schluessel> <wert>` | Einstellung im laufenden Betrieb aendern |

Die Befehle `start`, `route`, `rstart`, `resolve` und `tp` erwarten Parameter,
die das Addon erzeugt; von Hand sind sie unpraktisch.

---

## Konfiguration

`conf/autotravel.conf.dist` enthaelt **alle** 90 Werte mit Bereich,
Standardwert und einer Zeile Erklaerung. Jeder Wert ist zusaetzlich zur
Laufzeit erreichbar:

```
.at options fly          # alle Werte, die zum Fliegen gehoeren
.at set fly 0            # Fliegen abschalten
.at set taxicost 20000   # Preisgrenze auf 2 Gold
```

Laufzeitaenderungen gelten bis zum Serverneustart.

Beim Laden pruefen zwoelf Plausibilitaetstests die Schwellwerte gegeneinander
(Zielradius gegen Etappenradius, Fenster gegen Anstiegsschwelle, unvollstaendiger
Pfad teurer als jeder Berg ...). Auffaelligkeiten stehen als Warnung im Log,
verhindern den Start aber nicht.

---

## Aufbau der Quellen

| Datei | Inhalt |
|---|---|
| `AutoTravel.h` | Typen, Zustaende, Konfiguration, Sitzung, Manager |
| `AutoTravel_Config.cpp` | Optionsregistry, Laden, Meldungen, Parser |
| `AutoTravel_Terrain.cpp` | Bodenhoehen, Etagen, Wasser, Hoehenprofil |
| `AutoTravel_Path.cpp` | PathGenerator, Bewertung, Contour-Suche |
| `AutoTravel_Move.cpp` | Spline, Kontrolle, Reittier, Luftroute |
| `AutoTravel_Route.cpp` | WorldMapArea.dbc, Etappen, Start/Stop, Diagnose |
| `AutoTravel_Session.cpp` | Takt, Zustandsmaschine, Uebergabe |
| `AutoTravel_Nodes.cpp` | Reiseknoten von mod-playerbots, A* |
| `AutoTravel_Taxi.cpp` | Flugpunkte, Dijkstra, Abflug |
| `AutoTravel_SC.cpp` | Befehle und Anbindung an den Core |

---

## Benutzte Core-Schnittstellen

Falls das Modul bei dir nicht baut, liegt es mit hoher Wahrscheinlichkeit an
einer dieser Stellen -- AzerothCore aendert sie gelegentlich:

| Verwendet | Datei |
|---|---|
| `Movement::MoveSplineInit` mit `MovebyPath`, `SetWalk`, `SetVelocity`, `SetFly`, `SetFacing(float)`, `Launch` | AutoTravel_Move.cpp |
| `PathGenerator::CalculatePath / GetPathType / GetPath / SetUseStraightPath` | AutoTravel_Path.cpp |
| `Map::GetHeight`, `Map::GetWaterOrGroundLevel`, `Map::GetZoneId` | AutoTravel_Terrain.cpp |
| `Player::SetClientControl`, `StopMoving`, `NearTeleportTo`, `movespline` | AutoTravel_Move.cpp, _Session.cpp |
| `Player::m_taxi.IsTaximaskNodeKnown`, `Player::ActivateTaxiPathTo` | AutoTravel_Taxi.cpp |
| `sTaxiNodesStore`, `sTaxiPathSetBySource` aus `DBCStores.h` | AutoTravel_Taxi.cpp |
| `Player::m_movementInfo.transport.guid` | AutoTravel_Session.cpp |
| `Player::isAFK`, `Player::ToggleAFK` | AutoTravel_Session.cpp |
| `WorldScript::OnUpdate`, `OnAfterConfigLoad` | AutoTravel_SC.cpp |

### Ein Unterschied zwischen den Corestaenden ist bereits abgefangen

`sTaxiPathSetBySource` hat im Lauf der Zeit den Wertetyp gewechselt:

```
frueher   std::unordered_map<uint32, TaxiPathBySourceAndDestination>
heute     std::unordered_map<uint32, TaxiPathEntry const*>
```

Beide tragen ein Feld `price`, einmal ueber `.` und einmal ueber `->`
erreichbar. `AutoTravel_Taxi.cpp` kapselt den Zugriff in `TaxiPriceOf()` und
baut deshalb auf beiden Staenden.

Bewusst **nicht** benutzt wird `PlayerScript`. AzerothCore hat dessen Hooks
zwischenzeitlich von `OnLogout` auf `OnPlayerLogout` umbenannt; ein Modul, das
sie verwendet, baut je nach Corestand nicht mehr. Noetig sind sie nicht: die
Clientkontrolle wird nicht gespeichert und steht nach jedem Login wieder beim
Client, und verwaiste Sitzungen raeumt der Takt selbst ab.

---

## Pruefen ohne Server

Neben dem Modul liegt `autotravel-tools/`. Damit lassen sich Modul und Addon
pruefen, ohne AzerothCore zu bauen und ohne den Client zu starten:

```
./pipeline.sh              nur pruefen
./pipeline.sh --fix        autotravel.conf.dist neu erzeugen, dann pruefen
./pipeline.sh --package    nach bestandener Pruefung die Zips bauen
```

Fuenf Schritte: Uebersetzen gegen Attrappen der Core-Schnittstelle (beide
Auspraegungen von `sTaxiPathSetBySource`), Lua-Syntax, rund fuenfzig
Durchlaufpruefungen in einer nachgebauten Oberflaeche, Modul und Addon
gegeneinander halten, Konfigurationsdatei gegen die Registry.

`conf/autotravel.conf.dist` wird dabei nicht gepflegt, sondern **erzeugt**.
Wer einen Wert hinzufuegt, traegt ihn in `struct ATConfig`, in `sOptions[]` und
in die Gruppierung in `gen/gen_conf.py` ein und ruft `./pipeline.sh --fix`.

Einzelheiten stehen in `autotravel-tools/README.md`.

---

## Bekannte Grenzen

* **Kontinentwechsel zu Fuss** gibt es nicht. Ein Ziel auf einer anderen Karte
  wird ueber den Knotengraphen oder einen Transport erreicht, sonst gar nicht.
* **Einsteigen** in Zeppelin und Schiff macht der Spieler selbst. Der Autopilot
  bringt ihn zum Anleger und wartet.
* **Clipping auf Treppen** laesst sich nicht restlos beseitigen. Es entsteht,
  weil die Z-Werte des serverseitigen Splines nicht exakt zur
  Kollisionsgeometrie passen, die der Client rendert. Das Modul verhindert, dass
  der Charakter *unter der Treppe bleibt*; ein kurzes Durchrutschen auf der
  ersten Stufe bleibt moeglich. Wer es ganz weg haben will, verkleinert
  `AutoTravel.TerrainStep` auf etwa 1.5 -- das kostet deutlich mehr
  Hoehenabfragen.
* **Instanzen und Schlachtfelder** sind ausgenommen.
