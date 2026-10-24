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
| mod-playerbots | **optional**: liefert den Reiseknotengraphen. Fehlt es, benutzt das Modul die Carbonite-Route (siehe unten, warum das keine Selbstverstaendlichkeit ist) |

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

**Warum vorher nachgesehen wird, ob die Tabellen da sind:** eine einfache
SELECT-Abfrage auf eine fehlende Tabelle liefert in AzerothCore kein leeres
Ergebnis. `MySQLConnection::_HandleMySQLErrno` beendet den Weltserver bei
`ER_NO_SUCH_TABLE` und `ER_BAD_FIELD_ERROR` mit `ABORT` ("Your database
structure is not up to date"). Ein Server ohne mod-playerbots waere beim Start
abgestuerzt. Das Modul fragt deshalb zuerst `information_schema` nach den
Tabellen **und** den benoetigten Spalten und liest nur, wenn alles vorhanden
ist. Fehlt etwas, steht der Grund als Hinweis im Log, und die Carbonite-Route
gilt. Der Datenbankname wird auf Buchstaben, Ziffern, `_` und `$` geprueft, bevor
er in SQL eingesetzt wird.

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

### Zielanker: NPCs, Objekte und Questziele

Das Addon liefert nur Kartenkoordinaten, keine Hoehe. Wo Flaechen uebereinanderliegen,
suchte die Hoehensuche die Etage bisher nach der Position des **Spielers** -- und die
Exodar liegt rund 130 yd *unter* dem Gelaende darueber. Wer draussen stand (oder auf einer
anderen Etage), bekam die Oberflaeche als Ziel; der Weg zu Ganaar fuehrte aus der Stadt
hinaus, und die Reise endete 139 yd ueber ihm.

Ist das Ziel ein NPC, ein Objekt oder ein Questziel, kennt der Server seine Position genau:
sie steht in den Spawndaten. Das Modul sucht zum Kartenpunkt den passenden Spawn und nimmt
dessen Etage (und bei NPCs auch x/y). Ein Spawn gilt als Anker, wenn er mindestens eines
dieser Merkmale hat:

| Merkmal | Bedeutung | Toleranz |
|---|---|---|
| Dienst | NPC mit Haendler-, Lehrer-, Questgeber-, Flug-, Gasthaus-, Bank-, Auktions-, Stall-, Kampfmeister-Flag oder ein nutzbares Objekt (Briefkasten, Amboss, Schmiede, Gildentresor, Besprechungsstein). Reines Gespraech zaehlt nicht: Wachen und Passanten sind keine Ziele | `AnchorRadius`, 12 yd |
| Name | der **englische** Name in `creature_template.name` steht im Label, das das Addon aus Carbonite mitschickt | `AnchorNameRadius`, 25 yd |
| Quest | Ziel einer offenen Quest des Spielers (Kill- und Interaktionsziele, auch ueber Kill-Credit-Eintraege) oder der Abgabe-NPC einer fertigen Quest | `AnchorQuestRadius`, 40 yd |

* **Sprache.** Carbonite zeigt die Namen englisch, auch bei deutschem Client ("High Chief
  Stillpine" statt "Oberhaeuptling der Tannenruhfeste"). Verglichen wird deshalb gegen die
  Namen der Basistabelle, nicht gegen `creature_template_locale`. Der Name muss als ganze
  Woerter im Label stehen (mindestens vier Zeichen); ein Label, das nur die Zone nennt,
  trifft keinen NPC.
* **Autotracking.** Carbonite schickt keine Quest-ID mit, und der Server braucht sie auch
  nicht: er kennt das Questlog des Spielers und prueft die Spawns in der Naehe gegen dessen
  offene Ziele und fertige Quests. Gegenstandsziele und Gebietsziele haben keinen eigenen
  Spawn und fallen weg -- dort gilt das alte Verfahren.
* **Auswahl.** Bewertet wird der Abstand im Verhaeltnis zur Toleranz; Name und Quest
  verbessern die Wertung (der Name stark, die Quest weniger), wer Wegpunkte abgeht, verschlechtert
  sie. Ein NPC direkt am Kartenpunkt schlaegt ein 35 yd entferntes Questziel. Liegt **kein**
  Kandidat innerhalb seiner Toleranz -- anderes Phasing, ein verschobener Spawn, ein Punkt mitten
  im Gelaende --, gilt das alte Verfahren, als gaebe es den Anker nicht.
* **Etagen.** Spawns innerhalb von `AnchorFloorTolerance` (6 yd) in der Hoehe gehoeren zu einer
  Etage. Gibt es mehrere gleich gut belegte Etagen (zwei Haendler ohne Namensbezug, 9 yd
  auseinander, auf verschiedenen Stockwerken), werden bis zu zwei weitere als Rueckfall
  mitgerechnet; der Weg entscheidet, welche gewinnt. Ein Anker mit Namens- oder Questbeleg
  bekommt keine Ersatzetage.
* **Nur das Endziel.** Zwischenpunkte einer Route bleiben Wegmarken: ein Stadttor ueber der Exodar
  darf nicht an einen NPC in der Stadt darunter heften.
* **Welche Spawns.** Die Quelle sind die Spawn-Mengen des Cores je Karte und Gitter
  (`ObjectMgr::GetGridObjectGuids`): sie kennen Spielereignisse (31 % der Dienst-NPCs gibt es nur
  waehrend eines Ereignisses), und das Modul pflegt keinen eigenen Index, der veralten koennte.
  Auf der Karte des Spielers gelten ausserdem Phase, Spawngruppen und Pools; auf einer fremden
  Karte lassen sich die beiden letzten nicht pruefen. Spawns ohne Datenbankeintrag (`.npc add`
  ohne Speichern) zaehlen nicht.
* **Verdeckte Ziele.** Liegt das Gelaende mehr als 15 yd ueber dem Anker (Stadt unter der Erde,
  Gebaeude), gilt er als verdeckt. Die Reise laeuft dann nur zu seiner Etage; die Oberflaeche
  darueber ist kein Ersatz, es gibt keinen Luftweg, keine Contour-Suche -- und fuehrt kein Weg hin,
  sagt das Modul es, statt zur Oberflaeche zu laufen.
* **Ankunft.** Ein verankertes Ziel gilt erst als erreicht, wenn auch die Etage stimmt
  (`AnchorArrivalZ`, 10 yd). Endet ein Teilstueck am Ziel auf der falschen Etage, wird neu gerechnet; nach
  zwei solchen Teilstuecken gilt das Ziel als erreicht (ein NPC auf einem Podest ohne Weg darf die Reise
  nicht endlos beschaeftigen).
* **Auch auf anderen Karten.** Die Spawndaten gelten fuer jede Karte: ein Ziel in Sturmwind bekommt
  seine Hoehe schon bei der Planung, nicht erst nach der Ankunft.

`.at diag` im Spiel zeigt die Suche: wie viele Spawns geprueft und warum welche verworfen wurden, den
gewaehlten Anker mit Belegen, Abstand und Etage, die naechsten Kandidaten und was die alte Hoehensuche
gewaehlt haette. `AutoTravel.UseTargetAnchor = 0` schaltet alles ab.

### Erweiterte Wegsuche: Sackgassen in Staedten mit mehreren Etagen

Der Wegfinder des Cores (`PathGenerator`) sucht mit hoechstens **1024 Knoten** und liefert
hoechstens 74 Polygone. Ist der Weg laenger oder verwinkelter, als der Pool hergibt -- eine Stadt wie
die Exodar, deren Ausgang hunderte Yards entfernt liegt --, ist die Suche erschoepft, bevor sie das
Ziel erreicht. Detour gibt dann nicht "keinen Weg" zurueck, sondern den Knoten mit der kleinsten
**Luftlinie** zum Ziel. Liegt das Ziel eine Etage hoeher (der NPC an der Oberflaeche, der Spieler
in der Stadt), ist das die Stelle im Innenraum direkt darunter: eine Wand.

Dazu kommt eine Eigenheit von Detour: liegt x/y des Ziels im letzten Polygon dieses Teilwegs, setzt
`findStraightPath` den **letzten Punkt auf die Zielposition selbst**, mit ihrer Hoehe (die Funktion
`closestPointOnPolyBoundary` gibt die Position dann unveraendert zurueck). Der Teilweg endet scheinbar
am NPC, 139 yd ueber dem Boden. Im geglaetteten Weg (dem ersten Kandidaten) laeuft der Weg in
4-yd-Schritten am Boden bis unter das Ziel und haengt dann die Zielposition an: ein rein senkrechtes
Stueck; im Eckpunkt-Weg, in einem grossen Polygon, ist es eine lange Schraege. Der Charakter lief dorthin, "kletterte" in kleinen Spruengen die Wand hinauf, und die
Bodenrettung setzte ihn zurueck -- oder er wurde durch die Decke an die Oberflaeche versetzt.
Spiegelbildlich trifft es ein Ziel im Innenraum, wenn der Spieler draussen steht.

Mit einem Modell (`tools/tests/search_test.cpp`: Innenraum mit 2300 Polygonen, 280 yd lange Rampe,
Ziel 140 yd hoeher senkrecht darueber) und der echten Ausgabe von Detour laesst sich das nachstellen: der
1024-Knoten-Wegfinder endet unter dem Ziel, `PARTIAL | OUT_OF_NODES`, sein letzter Punkt ist meist die
Zielposition in 140 yd Hoehe; mit grossem Pool findet dieselbe Suche den Weg ueber die Rampe -- in
unter einer Millisekunde.

* **Endstueck abschneiden.** Bei einem Teilweg wird das letzte Wegstueck entfernt, wenn man es nicht
  ablaufen kann (`AT::TrimDeadTail`): es steigt oder faellt um mindestens 12 yd und geht dabei weniger als
  ein Zweieinhalbtel davon zur Seite (Treppen und Rampen sind flacher), oder es endet genau auf der
  Zielposition und ueberwindet dabei mindestens 20 yd. Erst danach wird beurteilt, wo der Teilweg endet.
* **Ausloeser.** Fiel ein solches Stueck weg, oder sieht das Ende verdaechtig aus
  (`AT::PartialLooksStuck`: es liegt fast senkrecht unter oder ueber dem Ziel, auf anderer Hoehe in der
  Naehe, oder das Ziel ist in 3D kaum naeher als vom Start), laeuft eine **eigene Suche**
  (`AutoTravel_Search.cpp`) mit `ExtendedSearchNodes` Knoten (65535, Detours Hoechstwert), einer
  Zeitgrenze von `ExtendedSearchMs` (30 ms) und den Kosten des PathGenerators, **Steigung eingerechnet**:
  sonst ersetzte sie seinen Weg durch einen steileren. Ein langer Weg ueber freies Gelaende, den der
  PathGenerator nur wegen seiner Laengengrenze abschneidet, loest sie nicht aus.
* **Ergebnis.** Die Suche liefert nur einen **durchgehenden** Weg, der das Zielpolygon erreicht; er
  ersetzt den Teilweg. Ein erschoepfter Suchraum liefert keinen Weg (die Suche gibt den
  "naechsten Knoten" nicht als Weg aus).
* **Ziel weit neben dem NavMesh.** Liegen Start oder Ziel mehr als 7 yd neben dem NavMesh (ein NPC auf einem
  Dach oder einer Plattform ohne NavMesh), setzt die Suche das Ziel wie der PathGenerator auf den naechsten Punkt des
  Zielpolygons und markiert den Weg als unvollstaendig (`FARFROMPOLY`).
* **Keine Verbindung.** Findet auch die erweiterte Suche keinen Weg, bleibt vom Teilweg der begehbare
  Teil -- ohne das senkrechte oder festgenagelte Endstueck. Bleibt davon nichts uebrig, oder endet er
  fast senkrecht unter oder ueber dem Ziel (Etagenunterschied ab 20 yd, in der Ebene hoechstens 30 yd,
  bei weiten Wegen bis 60 yd), erfaehrt der Spieler es einmal je Etappe: "endet vermutlich in einer
  Sackgasse ..." mit dem Grund -- *keine Verbindung im NavMesh* (der Suchraum ist erschoepft; ein
  Datenproblem der mmaps, das das Modul nicht beheben kann), oder *Suchpool erschoepft / Zeitgrenze
  erreicht / Weg zu lang* (Werte erhoehen: `.at set extms 100`). Bei einem blossen Verdacht
  (kaum Fortschritt) bleibt es bei der Debug-Zeile. Fuehrt der Weg zu einem verankerten Ziel in eine Sackgasse, probiert
  das Modul danach **nicht** die Hoehensuche: sie fuehrte auf den Boden unter dem Ziel und endete mit "Ziel
  erreicht" auf der falschen Etage.
* **Bremsen.** Je Kandidat laufen zwei Fassungen (glatt, Eckpunkte); die zweite nimmt das Ergebnis der
  ersten (vier Zwischenspeicher). Nach einem Fehlschlag pausiert die Suche `ExtendedSearchRetryMs`
  (15 s; nach Zeitgrenze oder Fehler 2 s) **fuer diesen Start und dieses Ziel**, und je Wegberechnung
  laufen hoechstens drei Suchen -- der Ersatzring um ein unerreichbares Ziel probiert 24 Punkte.
* **`.at diag`** zeigt je Kandidat, ob der Weg aus der erweiterten Suche stammt, wie viele Suchschritte
  sie brauchte und -- bei einer Sackgasse -- warum sie keinen Ausweg fand.
* `AutoTravel.ExtendedSearch = 0` schaltet sie ab.

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

### Wegpunktketten

Verkettet der Spieler in Carbonite mehrere Ziele (Strg + Umschalt + Linksklick auf die Karte), laeuft
AutoTravel sie **der Reihe nach** ab. (Bis 4.0.7 zaehlte nur der letzte Punkt: die Planung durch Flugmeister
oder Knotengraph ersetzte ab `AutoTravel.NodeMinDistance` die ganze Route und warf alles dazwischen weg.)

* Das Addon markiert die gesetzten Ziele im Befehl `.at route` mit der Art `2` (Bitmaske: `1` Flugpunkt,
  `2` Wegpunkt). Was Carbonite selbst dazwischen berechnet (Zonenuebergaenge, Torbogen, Flugpunkte), bleibt
  gewoehnlicher Stuetzpunkt.
* Die Planung arbeitet **abschnittsweise**: vom aktuellen Punkt bis zum naechsten Wegpunkt. Sie ersetzt nur die
  Stuetzpunkte dazwischen, nie den Wegpunkt. Der erste Abschnitt wird beim Start geplant, jeder weitere, sobald
  der Charakter einen Wegpunkt erreicht hat -- von seiner tatsaechlichen Position aus. Eine Kette kann deshalb
  je Abschnitt einen Flug, ein Schiff oder ein Portal enthalten.
* Ein Wegpunkt gilt mit dem Zielradius (`AutoTravel.ArrivalDistance`, 8 yd) als erreicht, die Stuetzpunkte
  dazwischen mit `AutoTravel.LegDistance` (15 yd). Das Modul meldet "Wegpunkt 2/4 erreicht."
* **Nicht erreichbar**: findet der Pfad zu einem Zwischen-Wegpunkt keinen Weg oder kommt der Charakter dort nicht
  voran, wird er mit Meldung uebersprungen und die Kette geht weiter. Das Endziel gibt dagegen auf. Liegt ein
  spaeterer Wegpunkt auf einer anderen Karte und gibt es keine Verbindung, endet die Reise mit dieser
  Begruendung.
* Statuszeile: bei einer Kette zaehlen `<etappe>` und `<etappen>` die **Wegpunkte** (2 von 4), nicht die Etappen,
  die die Planung dazwischen einfuegt; Restentfernung und Fortschritt gelten dem Weg zum naechsten Wegpunkt.
* Aeltere Module lesen die Art `2` als gewoehnlichen Laufpunkt: ein neues Addon mit einem Modul vor 4.0.8 faehrt
  wie frueher nur zum letzten Punkt (das Addon warnt, wenn die Faehigkeit `64` fehlt).
* Das Addon begrenzt Ketten auf 30 Punkte; der Server nimmt 32 Stuetzpunkte je Route.

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
  die gelten nur fuer die eigene Reise. `teleportsec` vergibt selbst Rechte und
  verlangt deshalb Administratorrechte: sonst koennte ein Spielleiter `.at tp`
  fuer alle freischalten.
* `.at stats` zeigt Spielleitern Zaehler und Speicherstand des Moduls.

| Befehl | Wirkung |
|---|---|
| `.at` | Status |
| `.at hello` | Handschlag; das Addon fragt damit ab, ob das Modul da ist, in welcher Version und was dieser Spieler darf (siehe "Protokoll") |
| `.at pause` / `.at resume` | Uebergabe an den Spieler und zurueck |
| `.at stop` / `.at repath` | Reise beenden / Pfad neu rechnen |
| `.at stats` | Gesundheitsbericht (nur Spielleiter): Sitzungen je Zustand, Reisen, Wegberechnungen, abgewiesene Befehle, Speicherstand |
| `.at diag ...` | warum scheitert der Pfad zu diesem Ziel? |
| `.at nodes` | Zustand des Reiseknotengraphen |
| `.at taxi` | bekannte Flugpunkte, Preisgrenze |
| `.at options [suchwort]` | alle Einstellungen mit aktuellem Stand |
| `.at set <schluessel> <wert>` | Einstellung im laufenden Betrieb aendern |

Die Befehle `start`, `route`, `rstart`, `resolve` und `tp` erwarten Parameter,
die das Addon erzeugt; von Hand sind sie unpraktisch. `route` nimmt Punkte der Form
`<karte>:<nx>:<ny>:<art>`; `art` ist eine Bitmaske (`1` Flugpunkt, `2` Wegpunkt, siehe
"Wegpunktketten"), hoechstens 32 Punkte je Route.

Alle Befehle, die Wegfindung oder viele Antwortzeilen ausloesen (`start`,
`rstart`, `tp`, `resolve`, `diag`, `repath`, `nodes`, `taxi`, `options`), laufen
durch eine **Befehlsbremse**: je Spieler gilt ein Mindestabstand
(`AutoTravel.CommandCooldownMs`, Standard 400 ms). Auskunftsbefehle (`nodes`,
`taxi`, `options`) haben eine eigene Spur, und aufwaendige Befehle kosten ein
Mehrfaches (`diag` das Zehnfache, `start`/`rstart` das Dreifache). `pause`,
`resume`, `stop`, `hello` und `status` sind davon ausgenommen -- ein Spieler soll
nie am Anhalten gehindert werden.

---

## Protokoll zum Addon

Das Addon schickt Befehle als Chatzeilen mit Punkt (`.at ...`). Ist das Modul
geladen, faengt der Server sie ab; sie werden nie an andere Spieler
weitergegeben. Antworten kommen
als Systemnachrichten mit dem Praefix `[AT]`, die das Addon aus dem Chat
filtert. Spieler ohne Addon sehen sie im Klartext.

**Warum erst ein Handschlag.** Ohne dieses Modul kennt der Server `.at` nicht.
Der Core meldet das einem normalen Spieler als "Es gibt keinen solchen Befehl" --
**es sei denn**, `AllowPlayerCommands` steht auf 0 (Standard 1): dann behandelt
`ChatHandler::_ParseCommands` die Zeile als gewoehnlichen Chattext, und der
Charakter riefe `.at start ...` in /sagen. Das Addon sendet deshalb zuerst
`.at hello` und alle weiteren Modulbefehle erst nach der Antwort. So weiss es
auch, welches Protokoll und welche Faehigkeiten der Server anbietet.

```
.at hello  ->  [AT]H|<modulversion>|<aktiv>|<knoten>|<flug>|<afk>|<protokoll>|<rechte>|<faehigkeiten>
```

| Feld | Bedeutung |
|---|---|
| modulversion | z. B. `4.0` |
| aktiv | 1, wenn `AutoTravel.Enable` an ist |
| knoten, flug, afk | Zahl der Reiseknoten; Flugmeister an; AFK-Unterdrueckung an |
| protokoll | zaehlt hoch, wenn sich Nachrichten unvertraeglich aendern (heute 4). Module vor 4.0 senden das Feld nicht; das Addon behandelt das als Protokoll 3, das dieselben Nachrichten benutzt |
| rechte | Kontostufe des Spielers (0 Spieler, 1 Moderator, 2 Spielleiter, 3 Administrator) |
| faehigkeiten | Bitmaske dessen, was dieser Server **diesem Spieler** anbietet |

Faehigkeiten: `1` Uebergabe (`pause`/`resume`), `2` Route (`route`/`rstart`),
`4` Flugmeister, `8` Teleport erlaubt (`allowTeleport` und Rechtestufe),
`16` serverweite Einstellungen aenderbar (Spielleiter), `32` Transporte,
`64` Wegpunktketten (ab 4.0.8: Stuetzpunkte der Art 2 werden einzeln angelaufen).
Das Addon blendet damit Knoepfe aus, die ohnehin abgewiesen wuerden, statt den
Spieler mit Absagen zu bedienen. **Die Pruefung bleibt trotzdem beim Server**: ein
Client kann jede Faehigkeit behaupten.

```
[AT]S|<zustand>|<restdistanz>|<ziel>|<flags>|<pfadpunkte>|<versuche>|<etappe>|<etappen>|<fortschritt>
```

Zustaende: `IDLE REPATHING TRAVELING COMBAT PLAYER MOUNTING TAKEOFF TAXI
TRANSPORT MANUAL ARRIVED FAILED`. Flags: `1` beritten, `2` fliegt, `4`
schwimmt, `8` Server steuert, `16` vom Spieler pausiert. Das Zielfeld ist
bereinigt (kein `|`, keine Steuerzeichen, hoechstens 48 Bytes). Weitere
Nachrichten: `[AT]M|<Text>` Meldung, `[AT]D|<Text>` Debug,
`[AT]W|<map>|<x>|<y>|<z>` Weltkoordinaten.

**Versionsregel:** neue Felder werden nur **hinten angehaengt**; Empfaenger
ignorieren zusaetzliche Felder und lesen fehlende als 0. Aendert sich Bedeutung
oder Reihenfolge eines bestehenden Feldes, steigt die Protokollnummer.

---

## Schutz des Weltservers

Das Modul rechnet auf dem Weltserver-Thread. Alles, was ein Client dort
anstossen kann, ist begrenzt:

* **Befehlsbremse** je Spieler: Mindestabstand `AutoTravel.CommandCooldownMs`
  (Standard 400 ms), getrennt fuer Auskunftsbefehle (`nodes`, `taxi`, `options`)
  und aufwaendige Befehle. Ein Befehl kostet ein Mehrfaches des Abstands: `diag`
  das Zehnfache, `start`/`rstart` das Dreifache, alle anderen das Einfache.
* **Wegberechnungs-Budget**: hoechstens `AutoTravel.MaxPathsPerTick` (Standard 3)
  neue Wegberechnungen je Takt und hoechstens `AutoTravel.PathBudgetMs`
  (Standard 40 ms) Rechenzeit, serverweit. Weitere Sitzungen warten; wer am
  laengsten wartet, kommt zuerst dran, sodass bei Dauerlast keine verhungert.
  Startet eine Gruppe gleichzeitig, verteilt sich die Last, statt den Server
  fuer Sekunden anzuhalten.
* **Stichproben statt Vollpruefung**: die Plausibilitaetspruefung der
  NavMesh-Punkte (bis zu vier VMap-Abfragen je Punkt) tastet lange Wege
  gleichmaessig mit hoechstens 24 Stichproben ab, die Wasserauswertung mit 64.
  Kurze Wege werden weiterhin vollstaendig geprueft. Der `PathGenerator` des
  Cores liefert ohnehin hoechstens 74 Punkte je Weg.
* **Keine Clientangaben in gemeinsamem Speicher.** Die gelernte Zuordnung
  "Karten-ID des Clients -> WorldMapArea-ID" liegt je Spieler (hoechstens 64
  Eintraege). Frueher war sie global: jeder Spieler konnte die Aufloesung fuer
  alle anderen verstellen und die Tabelle mit beliebigen IDs unbegrenzt wachsen
  lassen.
* **Aufraeumen**: alle 30 Sekunden werden gemerkte Routen, Kartenzuordnungen
  und Addon-Markierungen ausgeloggter Spieler entfernt. Das Modul registriert
  bewusst kein `PlayerScript` (die Hooks wurden zwischen Corestaenden
  umbenannt), bekommt also kein Ausloggen gemeldet.
* **Eingaben**: Zahlen werden streng geprueft (`nan`, `inf`, Ueberlauf,
  Vorzeichen, Hexschreibweise abgewiesen), Texte vom Client werden bereinigt,
  bevor sie in das Zeilenprotokoll gelangen.
* **Zustandsautomat**: der Zaehler fuer "festgefahren" ist von dem fuer
  "Wegsuche fehlgeschlagen" getrennt. Vorher setzte jede erfolgreiche Wegsuche
  beide zurueck, und ein Charakter, der an derselben Stelle haengt, wurde
  endlos neu berechnet statt nach `MaxRepathAttempts` aufzugeben.
* **Kartenwechsel**: Sitzungen ueberleben den Ladebildschirm eines Portals oder
  Zeppelins. `ObjectAccessor::FindPlayer` liefert nur Spieler *in der Welt*; die
  Vorfassung warf die Sitzung in genau dem Augenblick weg, an dem sie
  weitergehen sollte.
* **Abschalten zur Laufzeit** (`.at set enable 0`) gibt die Clientkontrolle aller
  fahrenden Spieler zurueck, statt sie ohne Steuerung stehen zu lassen.

---

## Konfiguration

`conf/autotravel.conf.dist` enthaelt **alle** 105 Werte mit Bereich,
Standardwert und einer Zeile Erklaerung. Jeder Wert ist zusaetzlich zur
Laufzeit erreichbar:

```
.at options fly          # alle Werte, die zum Fliegen gehoeren
.at set fly 0            # Fliegen abschalten
.at set taxicost 20000   # Preisgrenze auf 2 Gold
```

Laufzeitaenderungen gelten bis zum Serverneustart.

Die Datei ist **erzeugt** (`tools/gen_conf.py`) und kann deshalb nicht von der
Optionsregistry abweichen.

Beim Laden pruefen dreizehn Plausibilitaetstests die Schwellwerte gegeneinander
(Zielradius gegen Etappenradius, Fenster gegen Anstiegsschwelle, unvollstaendiger
Pfad teurer als jeder Berg ...). Auffaelligkeiten stehen als Warnung im Log,
verhindern den Start aber nicht.

---

## Aufbau der Quellen

| Datei | Inhalt |
|---|---|
| `AutoTravel.h` | Typen, Zustaende, Konfiguration, Sitzung, Manager |
| `AutoTravel_Config.cpp` | Optionsregistry, Laden, Meldungen, Handschlag, Befehlsbremse |
| `AutoTravel_Util.cpp` | reine Hilfsfunktionen (Zahlenparser, Textbereinigung); ohne Core testbar |
| `AutoTravel_Terrain.cpp` | Bodenhoehen, Etagen, Wasser, Hoehenprofil |
| `AutoTravel_Anchor.cpp` | Zielanker: Spawns, Questziele, Phase aus dem ObjectMgr |
| `AutoTravel_Path.cpp` | PathGenerator, Bewertung, Contour-Suche |
| `AutoTravel_Search.cpp` / `.h` | erweiterte Wegsuche auf dem NavMesh (reines Detour; mit eigenem NavMesh testbar) |
| `AutoTravel_Move.cpp` | Spline, Kontrolle, Reittier, Luftroute |
| `AutoTravel_Route.cpp` | WorldMapArea.dbc, Etappen, Start/Stop, Diagnose |
| `AutoTravel_Session.cpp` | Takt, Zustandsmaschine, Uebergabe |
| `AutoTravel_Nodes.cpp` | Reiseknoten von mod-playerbots, kuerzester Weg (Dijkstra) |
| `AutoTravel_Taxi.cpp` | Flugpunkte, Dijkstra, Abflug |
| `AutoTravel_SC.cpp` | Befehle und Anbindung an den Core |
| `tools/check.sh` | Pruefung gegen die Header von AzerothCore, Unit-Tests, Konfigurationsabgleich |
| `tools/gen_conf.py` | erzeugt `conf/autotravel.conf.dist` aus der Optionsregistry |

---

## Benutzte Core-Schnittstellen

Falls das Modul bei dir nicht baut, liegt es mit hoher Wahrscheinlichkeit an
einer dieser Stellen -- AzerothCore aendert sie gelegentlich:

| Verwendet | Datei |
|---|---|
| `Movement::MoveSplineInit` mit `MovebyPath`, `SetWalk`, `SetVelocity`, `SetFly`, `SetFacing(float)`, `Launch` | AutoTravel_Move.cpp |
| `PathGenerator::CalculatePath / GetPathType / GetPath / SetUseStraightPath` | AutoTravel_Path.cpp |
| `Map::GetMapCollisionData().GetMMapData().GetNavMesh()`; Detour: `dtNavMeshQuery::init`, `findNearestPoly`, `initSlicedFindPath`, `updateSlicedFindPath`, `finalizeSlicedFindPath`, `findStraightPath`; `NAV_GROUND`, `NAV_WATER`, `NAV_MAGMA` aus `MapDefines.h` | AutoTravel_Search.cpp, _Path.cpp |
| `Map::GetHeight`, `Map::GetWaterOrGroundLevel`, `Map::GetZoneId` | AutoTravel_Terrain.cpp |
| `ObjectMgr::GetGridObjectGuids`, `GetCreatureData`, `GetGameObjectData`, `GetCreatureTemplate`, `GetGameObjectTemplate`, `GetQuestTemplate`, `GetCreatureQuestInvolvedRelationBounds`, `ChooseCreatureFlags`; `Map::IsSpawnGroupActive`, `Map::GetPoolData`; `Player::getQuestStatusMap` | AutoTravel_Anchor.cpp |
| `Player::SetClientControl`, `StopMoving`, `NearTeleportTo`, `movespline` | AutoTravel_Move.cpp, _Session.cpp |
| `Player::m_taxi.IsTaximaskNodeKnown`, `Player::ActivateTaxiPathTo` | AutoTravel_Taxi.cpp |
| `sTaxiNodesStore`, `sTaxiPathSetBySource` aus `DBCStores.h` | AutoTravel_Taxi.cpp |
| `Player::m_movementInfo.transport.guid` | AutoTravel_Session.cpp |
| `Player::isAFK`, `Player::ToggleAFK` | AutoTravel_Session.cpp |
| `WorldScript::OnUpdate`, `OnAfterConfigLoad` | AutoTravel_SC.cpp |
| `ObjectAccessor::FindPlayer`, `FindConnectedPlayer` | AutoTravel_Session.cpp, _Route.cpp |
| `GameTime::GetGameTimeMS` | AutoTravel_Config.cpp, _Route.cpp |
| `WorldDatabase.Query(std::string_view)` auf `information_schema` und die Knotentabellen | AutoTravel_Nodes.cpp |
| `TaxiMaskSize` (Bereichsgrenze fuer `IsTaximaskNodeKnown`, das selbst nicht prueft) | AutoTravel_Taxi.cpp |

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

```
tools/check.sh <pfad-zu-azerothcore>
```

Ein flacher Klon von `azerothcore-wotlk` genuegt (`git clone --depth 1`), dazu
`g++` mit C++20 und die Entwicklungspakete, die AzerothCore selbst braucht
(Boost, OpenSSL, MySQL-Client). Vier Schritte:

1. **Konfigurationsdatei**: `conf/autotravel.conf.dist` stimmt mit der
   Optionsregistry ueberein (`tools/gen_conf.py --check`).
2. **Uebersetzen gegen die echten Header** des angegebenen Corestandes
   (`-fsyntax-only`, Warnungen als Fehler fuer die eigenen Dateien). Aendert
   AzerothCore eine Schnittstelle, die das Modul benutzt, faellt es hier auf --
   nicht erst beim Bau des Servers.
3. **Unit-Tests** (`tools/tests/util_test.cpp`) fuer alles, was Eingaben des
   Clients verarbeitet: Zahlenparser, Textbereinigung, Namenspruefung. Sie laufen
   als eigenes Programm und brauchen nichts von AzerothCore ausser den Headern.
4. **Wegsuche auf einem NavMesh** (`tools/tests/search_test.cpp`): ein selbstgebautes NavMesh
   (Innenraum, Rampe, Oberflaeche) zeigt mit der echten Ausgabe von Detour, dass der Wegfinder mit
   1024 Knoten in der Sackgasse endet, sein letzter Punkt auf die Zielposition festgenagelt ist,
   das Abschneiden und die Erkennung greifen und die erweiterte Suche den Ausgang findet; dazu
   die Steigungskosten (steiler Huegel gegen laengeren flachen Umweg) und die Grenzen der Suche (Pool,
   Zeit, Weglaenge, kein Start- oder Zielpolygon, getrennte Insel). Detour wird aus
   `deps/recastnavigation` des angegebenen Standes mitgebaut.

Das ersetzt **keinen echten Bau und keinen Test im Spiel**. Es prueft Syntax und
Typen gegen den Core, nicht das Linken, nicht das Verhalten auf einem Server.

Eine neue Option eintragen:

1. Feld in `struct ATConfig` (`src/AutoTravel.h`)
2. Zeile in `sOptions[]` (`src/AutoTravel_Config.cpp`), in der passenden Gruppe
   (Gruppen sind durch Leerzeilen getrennt)
3. `tools/gen_conf.py` ausfuehren

---

## Aktualisieren

**Von 3.x auf 4.0.** Es gibt **keine SQL-Aenderung** und keine neuen Tabellen.

1. Modul aktualisieren, **`cmake` neu ausfuehren** (es gibt eine neue Quelldatei,
   `src/AutoTravel_Util.cpp`; AzerothCore sammelt die Dateien beim
   Konfigurieren ein) und den Server neu bauen.
2. Die neue `autotravel.conf.dist` ansehen. Neu sind `AutoTravel.CommandCooldownMs`,
   `AutoTravel.MaxPathsPerTick` und `AutoTravel.PathBudgetMs`; fehlen sie in der eigenen
   `autotravel.conf`, gelten die Standardwerte. Eine bestehende `.conf` bleibt
   unveraendert gueltig.
3. Das Addon (`mod-autotravel_clientaddon`) auf Version 11 bringen, um
   Uebernehmen/Weiter, Berechtigungsanzeige und den abgesicherten Start zu
   bekommen. **Aeltere Addons arbeiten weiter**: das Statusformat ist unveraendert,
   und zusaetzliche Felder im Handschlag werden von aelteren Lesern ignoriert.
   Ein neues Addon arbeitet auch mit einem aelteren Modul (Protokoll 3) zusammen,
   dann ohne Berechtigungsanzeige.
4. Ein Spielleiter, der bisher `teleportsec` zur Laufzeit gesetzt hat, braucht
   dafuer jetzt Administratorrechte; in der `.conf` aendert sich nichts.

## Fehlersuche

| Beobachtung | Bedeutung / Abhilfe |
|---|---|
| Log: "Keine Reiseknoten benutzbar (Tabelle ... nicht gefunden)" | Ohne mod-playerbots normal; es gilt die Carbonite-Route. Liegen die Tabellen in einer anderen Datenbank: `AutoTravel.NodeDatabase` |
| Log: "... hat keine Spalte ..." | Die Tabellen von mod-playerbots haben ein anderes Layout als erwartet. Das Modul liest sie nicht, statt den Server zu beenden |
| Log: "WorldMapArea.dbc nicht gefunden" | `DataDir` in der worldserver.conf pruefen; die Datei muss unter `dbc/` liegen |
| Spieler: "Kein Weg gefunden" | `/at diag` im Spiel. Fehlen mmaps fuer die Kachel, steht es dort |
| Spieler: "Zu schnell - bitte einen Augenblick warten" | Die Befehlsbremse. Ein Addon sendet nicht so dicht; `AutoTravel.CommandCooldownMs` senken oder auf 0 stellen |
| Reisen starten spuerbar verzoegert, wenn viele gleichzeitig beginnen | Das Wegberechnungs-Budget. `.at stats` zeigt, wie oft es gegriffen hat; `AutoTravel.MaxPathsPerTick` erhoehen |
| Spieler bleibt nach `.at set enable 0` stehen | Ab 4.0 gibt das Abschalten die Steuerung zurueck. In aelteren Staenden: `.at stop` bzw. Neuanmeldung |
| "Das Ziel liegt auf einer anderen Karte ..., und AutoTravel findet keine Verbindung dorthin" | Kontinentwechsel braucht den Knotengraphen: `.at nodes` pruefen, `AutoTravel.UseTravelNodes = 1`. Enthaelt der Graph keine Schiffs-/Zeppelinverbindung zwischen den Karten, kann das Modul keine erfinden |
| Verkettete Carbonite-Ziele: AutoTravel faehrt nur zum letzten | Wegpunktketten gibt es ab Modul 4.0.8 **und** Addon 11.9 (`/at info` zeigt beide Versionen; die Faehigkeit `64` fehlt bei einem alten Modul). Mit `.at debug` steht beim Start "Route mit N Stuetzpunkten uebernommen, davon M Wegpunkte"; fehlt der Zusatz, hat das Addon die Ziele nicht als Wegpunkte erkannt: `/at route` im Addon zeigt `[Wegpunkt]` an den Punkten, `/at route roh` die Rohdaten von Carbonite, `/at wegpunkte alle` laeuft jeden Punkt der Route ab |
| `.at diag` meldet "Kein passender Kartenausschnitt" | Die Zonenzuordnung des Clients stimmt nicht; `/at karten` im Addon, sonst `/at karte <id>` |
| Der Charakter laeuft aus einer Stadt, die ueber- oder untereinanderliegende Etagen hat (Exodar, Dalaran, Unterstadt), oder die Reise endet ueber dem NPC | `.at diag` (mit Ziel): die Zeile "Anker:" zeigt, ob ein Spawn erkannt wurde, und "Ohne Anker waere die Zielhoehe" die alte Wahl. Steht dort "keiner", fehlt ein NPC mit Dienst oder Namen im Radius (`AutoTravel.AnchorRadius` hochsetzen, oder das Label des Carbonite-Ziels pruefen). Mit `.at debug` zeigen die Zeilen "Direktkandidat" und "Zielanker aktiv", welche Etage den Weg geliefert hat |
| Der Charakter laeuft in einer Stadt mit mehreren Etagen (Exodar) unten in Richtung des NPCs an der Oberflaeche, gegen eine Wand, statt die Treppe hinauf aus der Stadt zu laufen, oder umgekehrt | Das war der Teilweg in die Sackgasse (4.0.7, siehe "Erweiterte Wegsuche"). `.at diag` zeigt je Kandidat "(erweiterte Suche: durchgehender Weg)" oder "(Teilweg endet in einer Sackgasse)" und darunter die Zeile "Erweiterte Wegsuche: ...". Dort steht, ob es keine Verbindung im NavMesh gibt (ein Datenproblem der mmaps) oder die Zeitgrenze nicht reichte (`AutoTravel.ExtendedSearchMs` erhoehen) |
| Charakter gleitet schnell durch die Luft, schiesst am Ziel vorbei, "huepft", oder hat nach dem Wasser Minischritte | Bis 4.0 ein Abschnitt, der die Paketkodierung sprengte (siehe AENDERUNGEN.md, "4.0.1"). Ab 4.0.1 darf es ihn nicht mehr geben: mit Debug zeigen die Zeilen `Abschnitt gestartet` Laenge in yd (hoechstens etwa 440), Tempo, Wasser/Land und die Bewegungskennzeichen des Spielers. Tritt es trotzdem auf, diese Zeilen mitschicken |

`AutoTravel.Debug = 1` (oder `.at debug` je Spieler) schaltet ausfuehrliche
Ausgabe ein; fehlgeschlagene Reisen stehen mit Grund im Serverlog, sofern die
Kategorie `module` auf Debug steht.

---

## Bekannte Grenzen

* **Kontinentwechsel zu Fuss** gibt es nicht. Ein Ziel auf einer anderen Karte
  wird ueber den Knotengraphen von mod-playerbots erreicht (Schiff, Zeppelin,
  Portal als Sonderverbindung), sonst gar nicht: ohne geladene Knoten oder ohne
  Verbindung zwischen den Karten bricht die Reise mit einer Meldung ab, die sagt,
  was fehlt. Das Betreten des Transports ist Sache des Spielers. Ab 4.0.2.
  Seit 4.0.5 fuehrt die Route nur durch Oestliche Koenigreiche, Kalimdor und die
  Tiefenbahn (Outland, Nordend und Instanzen nur, wenn Start oder Ziel dort liegen);
  Zeppeline gelten als Horde, Schiffe als Allianz (ausser Ratchet - Booty Bay).
  **Nicht** beruecksichtigt sind Fusswege durch feindliche Hauptstaedte, Portale der
  anderen Fraktion und die Stufe des Gebiets.
* **Einsteigen** in Zeppelin und Schiff macht der Spieler selbst. Der Autopilot
  bringt ihn zum Anleger und wartet.
* **Clipping auf Treppen** laesst sich nicht restlos beseitigen. Es entsteht,
  weil die Z-Werte des serverseitigen Splines nicht exakt zur
  Kollisionsgeometrie passen, die der Client rendert. Das Modul verhindert, dass
  der Charakter *unter der Treppe bleibt*; ein kurzes Durchrutschen auf der
  ersten Stufe bleibt moeglich. Wer es ganz weg haben will, verkleinert
  `AutoTravel.TerrainStep` auf etwa 1.5 -- das kostet deutlich mehr
  Hoehenabfragen.
* **Zielanker kennen nur Spawns.** Ein Ziel, das kein NPC, Objekt oder Questziel ist (ein Punkt
  mitten auf einer Etage der Exodar), hat keinen Anker; dort entscheidet weiter die Hoehensuche
  nach der Position des Spielers. Ein Spawn ist ausserdem ein Ausgangspunkt: NPCs, die Wegpunkte
  ablaufen oder umherstreifen, stehen nicht immer dort (ihre Wertung ist schlechter), Aufzuege
  und Tueren kennt das Modul nicht -- ein Anker hinter einer Tuer oder einem Lift kann ein
  Teilweg bleiben. Dicht gestapelte Staedte (Dalaran, Darnassus, Eisenschmiede, Unterstadt) lassen
  sich aus x/y allein nicht immer eindeutig aufloesen; der Name im Label und die Quest verbessern
  das, und die Etagenpruefung bei der Ankunft faengt den Rest ab.
* **Die Sackgasse wird erkannt, wenn der Teilweg in Reichweite des Ziels endet** (in der Ebene bis 100 yd). In einer
  sehr grossen Halle, deren Teilweg weit vor dem Ziel abbricht, laeuft der Charakter ein Stueck in die falsche
  Richtung, bis er nah genug ist; eine groessere Reichweite loeste die Suche bei jedem Teilweg ueber Hanggelaende aus
  und umginge die Contour-Suche.
* **Die erweiterte Wegsuche kann nur finden, was das NavMesh enthaelt.** Fehlt in den mmaps die
  Verbindung zwischen zwei Etagen (eine Treppe, die der Generator nicht als begehbar erkannt hat) oder ist eine
  Kachel nicht geladen, gibt es keinen durchgehenden Weg, und das Modul meldet die Sackgasse, statt
  sie zu verschweigen. Aufzuege und Tueren gehoeren nicht zum NavMesh.
* **Instanzen und Schlachtfelder** sind ausgenommen.
* **Transport der Nachrichten**: Befehle und Antworten laufen als normale Chatzeilen.
  Neuere AzerothCore-Staende haben dafuer den `AddonChannelCommandHandler`
  (Befehle ueber `SendAddonMessage("AzerothCore", ...)` mit Quittung `a`/`o`/`f`
  je Befehl). Er wuerde jede Verwechslung mit normalem Chat ausschliessen und
  echte Quittungen liefern. Das Modul benutzt ihn bewusst noch nicht: er existiert nicht
  in allen Corestaenden, und ohne Test im Spiel ist die Umstellung zu riskant.
* **Der Handschlag laeuft selbst als Punktbefehl.** Fehlt das Modul, bekommt ein
  normaler Spieler einmal oder zweimal "Es gibt keinen solchen Befehl" zu sehen,
  bevor das Addon aufgibt -- bei `AllowPlayerCommands = 0` stuende die Zeile
  stattdessen in /sagen. Die Abfrage beim Anmelden laesst sich im Addon
  abschalten.
