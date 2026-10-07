# Version 4.0.1 -- Der Charakter "fliegt" ueber die Karte

Gemeldet: Bei laengeren Strecken gleitet der Charakter sehr schnell durch die
Luft, schiesst am Ziel vorbei, manchmal ueber die halbe Karte und wieder
zurueck, bis er schliesslich landet. Nach dem Wasser sind die Schritte klein
und schnell, oder der Charakter "huepft" durch die Luft.

## Ursache (Hauptfehler)

Das Modul baut je Abschnitt EINEN `MoveSpline` und begrenzte ihn nur nach der
Zahl der NavMesh-Punkte (`ChunkPoints`, Standard 12), nicht nach seiner Laenge.
Im offenen Gelaende bleiben vom NavMesh-Pfad nur wenige Eckpunkte uebrig, jeder
Abstand betraegt dann leicht 80 bis 100 yd: aus 9 Punkten wurde ein Abschnitt
von rund 750 yd (im Log: "251 Punkte, Wasser").

`SMSG_MONSTER_MOVE` kann so einen Abschnitt nicht tragen. Die Zwischenpunkte
eines gewoehnlichen Splines werden als Abstand zur Mitte von Start und Ende in
11/11/10 Bit zu je 0,25 yd gepackt (`ByteBuffer::appendPackXYZ`,
`WriteLinearPath`): x und y erreichen +-256 yd, z +-128 yd. Was darueber liegt,
wird abgeschnitten. Der Core merkt es nicht -- `MoveSplineInitArgs::
_checkPathBounds()` ist in `Validate()` auskommentiert und waere mit
`MAX_OFFSET = 1024` selbst um den Faktor vier zu grosszuegig. Der Client baut aus
den verstuemmelten Punkten einen anderen, laengeren Weg und laeuft ihn in der
Zeit ab, die der Server fuer den WAHREN Weg berechnet hat: also mit dem
Mehrfachen des Tempos, durch die Luft, an falschen Stellen. Am Ende landet er
beim richtigen Zielpunkt (der Server hat den Charakter die ganze Zeit korrekt
gefuehrt).

Nachgerechnet mit den Zahlen aus dem Log: der 746-yd-Abschnitt wuerde vom Client
als 2780 yd langer Weg gezeichnet (3,7-faches Tempo, Punkte bis 512 yd
verschoben). Der 279-yd-Abschnitt davor liegt innerhalb der Grenzen und lief
normal -- das erklaert das "teilweise".

## Aenderungen

* `LaunchChunk` begrenzt den Abschnitt jetzt nach der Paketkodierung
  (`AT::SplineFitsPacket`, Grenzen 220 yd in x/y und 100 yd in z, mit Reserve
  zu den harten 256/128 yd). Ein Segment, das darueber hinausfuehrt, beginnt den
  naechsten Abschnitt.
* Lange NavMesh-Segmente werden vorher in Stuecke von hoechstens 40 yd geteilt
  (`AT::SplitLongSegments`, Hoehen linear verteilt). Sonst liesse sich ein
  einzelnes Segment ueber den See weder begrenzen noch an der Wassergrenze
  trennen.
* **Wasser und Land gehen nicht mehr in einen Abschnitt.** Bisher bekam der
  ganze Abschnitt Schwimmtempo und Schwimmkennzeichen, sobald ein einziger Punkt
  im Wasser lag: das Landstueck davor und danach wurde mit 4,7 statt 7 (im Log
  10,5) yd/s gelaufen, die Laufanimation passte nicht zur Geschwindigkeit
  ("Minischritte"). Ein Segment gilt als Wasser, wenn die Mehrheit seiner Punkte
  im Wasser liegt.
* `ReleaseControl` und `TryMount` fragen den Wasserzustand an der echten
  Position (`GetLiquidData`) statt `Player::IsInWater()`. Der Core fuehrt
  `IsInWater()` nur aus Bewegungspaketen des Clients nach und verwirft diese,
  solange ein Spline laeuft; der Wert blieb waehrend der ganzen Fahrt auf dem
  Stand vor dem Start. Folge: Schwimmkennzeichen, das nach einer im Wasser
  begonnenen Fahrt auf dem Land stehenblieb, und ein Aufsitzen, das nach einer
  Schwimmstrecke verweigert wurde. Der Wert wird jetzt auch nachgefuehrt
  (`SetInWater`).
* Bodenabschnitte werden immer als linearer Bodenspline gesendet. Der Konstruktor
  von `MoveSplineInit` schaltet sonst den Flugmodus ein (glatter Spline,
  Fluganimation, ungepackt), sobald der Spieler `CAN_FLY` oder `DISABLE_GRAVITY`
  traegt -- GM-Flug, Flugaura, Flugmount.
* Die Debugzeile "Abschnitt gestartet" nennt jetzt die Laenge in yd und die
  Bewegungskennzeichen des Spielers.

## Was getestet wurde

* Die Paketkodierung des Cores ist in `tools/tests/util_test.cpp` nachgebildet
  (11/11/10 Bit, Vorzeichenerweiterung wie beim Client). Geprueft wird, dass
  `SplineFitsPacket` genau die Abschnitte durchlaesst, die unversehrt ankommen,
  und den 746-yd-Abschnitt aus dem Log abweist; mit absichtlich falscher Grenze
  schlagen die Tests an. 109 Pruefungen, 0 Fehler.
* Uebersetzt gegen die Header von AzerothCore `master`.

Nicht getestet (hier nicht moeglich): Verhalten im Spiel. Dass der 3.3.5a-Client
die gepackten Felder mit Vorzeichen liest, ist aus dem Kodierer und seiner
Verwendung durch normale NPC-Pfade geschlossen, nicht am Client geprueft. Die
Erklaerung der "Minischritte" ist eine Annahme.

---

# Version 4.0 -- Pruefung und Haertung

Gepruefte Grundlage: AzerothCore `master` (Oktober 2026), Modul und beide Addons.
Alle Aussagen ueber den Core stammen aus dessen Quelltext, nicht aus dem
Gedaechtnis. Was getestet wurde und was nicht, steht am Ende.

## Fehler, die den Weltserver beenden konnten

**Fehlende mod-playerbots-Tabellen beendeten den Server beim Start.** Die README
versprach, das Modul falle ohne mod-playerbots auf die Carbonite-Route zurueck.
Tatsaechlich fragte `LoadTravelNodes()` die Tabelle `playerbots_travelnode` mit
einem einfachen SELECT ab, und `MySQLConnection::_HandleMySQLErrno` ruft bei
`ER_NO_SUCH_TABLE` und `ER_BAD_FIELD_ERROR` `ABORT` auf. Jetzt wird zuerst
`information_schema` nach Tabellen und Spalten gefragt; fehlt etwas, steht der
Grund im Log. Der Datenbankname wird vor dem Einsetzen in SQL geprueft.

**`IsTaximaskNodeKnown` ohne Bereichspruefung.** Der Core indiziert die
Flugpunktmaske ohne Grenze: Knoten 0 landet auf Feld 255, jeder Knoten ueber 448
hinter dem Array. Die Knotennummern stammen aus DBC-Daten. `NodeKnownBy()` prueft
jetzt den Bereich.

## Fehler im Ablauf

* **Der Autopilot gab nie auf, wenn der Charakter festhing.** Der Zaehler fuer
  "festgefahren" war derselbe wie der fuer "Wegsuche fehlgeschlagen", und der
  wurde nach jeder erfolgreichen Wegsuche auf 0 gesetzt -- die gelingt auch an
  derselben Stelle. Ergebnis: Endlosschleife aus Festhaengen und Neuberechnen.
  Jetzt eigener Zaehler (`stuckStrikes`), der nur durch echten Fortschritt endet.
* **Reisen ueber einen Kartenwechsel rissen ab.** `Update()` warf jede Sitzung
  weg, fuer die `ObjectAccessor::FindPlayer` nichts lieferte -- und das liefert
  nur Spieler *in der Welt*. Waehrend des Ladebildschirms eines Portals oder
  Zeppelins ist der Spieler verbunden, aber kurz nicht in der Welt. Die Sitzung
  verschwand genau dann, wenn sie weitergehen sollte. Jetzt
  `FindConnectedPlayer`; solange der Spieler laedt, ruht die Sitzung.
* **`.at set enable 0` liess fahrende Spieler ohne Steuerung zurueck.** Der Takt
  kehrte bei abgeschaltetem Modul sofort zurueck und gab die Kontrolle nie
  frei. Jetzt beendet `AbortAllSessions()` alle Reisen sauber.
* **Flug starten.** `StartTaxi()` rief `ActivateTaxiPathTo(nodes, nullptr, 0)`.
  Mit `spellid == 0` und `InstantFlightPaths = 1` bucht der Core den vollen
  Fahrpreis ab, versetzt den Spieler ans Ziel und gibt trotzdem `false` zurueck;
  das Modul meldete "Flug kam nicht zustande" und rechnete von der falschen
  Stelle weiter. Jetzt `spellid = 1` (die Konvention des Cores fuer eigene
  Aufrufer). Die dabei entfallende Pruefung des Reittiermodells
  (`GetTaxiMountDisplayId`) macht das Modul vorher selbst, damit kein
  unsichtbarer Flug entsteht.
* `Pause` wird serverseitig geprueft: waehrend Flug oder Transport steuert
  niemand, das Umschalten haette die Sitzung im naechsten Takt nur
  zurueckgesetzt.
* Sicherung gegen eine ungueltige Etappe (`legIdx` ausserhalb der Route).

## Sicherheit

* **Clientangaben lagen in gemeinsamem Speicher.** Die gelernte Zuordnung
  "Karten-ID des Clients -> WorldMapArea-ID" war global und wurde aus Angaben des
  Clients gespeist. Jeder Spieler konnte die Zielaufloesung fuer alle anderen
  verstellen und die Tabelle mit beliebigen IDs unbegrenzt wachsen lassen. Jetzt
  je Spieler und auf 64 Eintraege begrenzt.
* **Keine Bremse fuer aufwaendige Befehle.** `.at diag` berechnet bis zu zwoelf
  Wege auf dem Weltserver-Thread. Jetzt Mindestabstand je Spieler
  (`CommandCooldownMs`, Standard 400 ms), getrennt nach zwei Klassen: Auskunft
  (`nodes`, `taxi`, `options`) und aufwaendige Befehle. Ein Befehl kostet ein
  Mehrfaches des Abstands (`diag` zehnfach, `start`/`rstart` dreifach, sonst
  einfach), sodass ein Spieler mit `.at diag` nicht schneller sein kann als mit
  einem Statusbefehl. Dazu ein serverweites Wegberechnungs-Budget je Takt
  (`MaxPathsPerTick` Wege und `PathBudgetMs` Rechenzeit); wartende Sitzungen
  kommen nach Wartezeit zuerst dran, damit keine verhungert.
* **`teleportsec` ueber `.at set`.** Ein Spielleiter konnte die Rechtestufe fuer
  `.at tp` auf 0 setzen und den Teleport damit fuer alle freischalten. Jetzt
  nur Administratoren. Der Schluessel wird ausserdem vor der Rechtepruefung
  kleingeschrieben.
* Texte vom Client (Zielname) werden vor dem Einsetzen ins Zeilenprotokoll
  bereinigt; die Zahlenparser lehnen Vorzeichen, Leerraum, Hexschreibweise und
  Werte ausserhalb des Bereichs ab (`strtoul("-18446744073709551615")` ergab
  still 1).
* GUID-Tabellen (`_pendingRoutes`, `_addonPlayers`, Kartenzuordnungen) wuchsen
  fuer jeden Spieler, der je etwas geschickt hatte, bis zum Neustart. Jetzt
  Aufraeumdurchlauf alle 30 Sekunden; eine verwendete Route wird sofort
  entfernt.

## Leistung

* Die Plausibilitaetspruefung der NavMesh-Punkte kostete bis zu vier
  VMap-Abfragen je Punkt und Kandidat -- bei zwoelf bis vierzehn Kandidaten
  Zehntausende Strahlabfragen je Wegberechnung. Jetzt hoechstens 24 gleichmaessig
  verteilte Stichproben mit vorzeitigem Abbruch; kurze Wege werden weiter
  vollstaendig geprueft. Die Wasserauswertung ebenso (64). Zur Einordnung: der
  `PathGenerator` des Cores liefert ohnehin hoechstens 74 Punkte je Weg
  (`MAX_POINT_PATH_LENGTH`); die Zahl der Punkte ist also nach oben begrenzt, die
  Kosten je Punkt waren das Problem.
* Debugzeilen wurden auch bei ausgeschaltetem Debug formatiert (samt erneuter
  Streckenberechnung je Kandidat). Jetzt nur noch bei Bedarf.
* `.at set speciallinks/specialcost` las frueher den gesamten Knotengraphen neu
  aus der Datenbank. Der Aufschlag wird jetzt erst bei der Suche addiert.

## Protokoll

* Der Handschlag meldet jetzt Protokollnummer, Rechtestufe und eine
  Faehigkeitsmaske (siehe README, "Protokoll"). Die ersten fuenf Felder
  behalten Stelle und Bedeutung; das Statusformat ist unveraendert.
* Der Server schickt nach dem Handschlag, nach jeder abgewiesenen oder
  gedrosselten Startanfrage (`start`, `rstart`) und nach einem Teleport eine
  Statuszeile (`SyncStatus`). Ein Addon, das auf einen Start wartet, erfaehrt so
  die Absage, statt in "Startet" zu haengen.
* Neu: `.at stats` (Spielleiter) -- Sitzungen je Zustand, Zaehler,
  Speicherstand. Fehlgeschlagene Reisen stehen mit Grund im Log
  (`LOG_DEBUG`, Kategorie `module`).

## Werkzeuge

Die README verwies auf ein Verzeichnis `autotravel-tools/`, das nie eingecheckt
wurde. Jetzt gibt es `tools/check.sh` (Konfigurationsabgleich, Uebersetzen
gegen die Header des echten Cores, Unit-Tests) und `tools/gen_conf.py`. Die
Konfigurationsdatei ist damit tatsaechlich erzeugt statt von Hand gepflegt.

## Was getestet wurde

Getestet (ausgefuehrt):
* alle Quelldateien gegen die Header von AzerothCore `master` uebersetzt
  (`-fsyntax-only`, gnu++20), ohne Warnungen in den eigenen Dateien
* die Unit-Tests der Eingabefunktionen (80 Pruefungen), dazu mit absichtlich
  eingebauten Fehlern gegengeprueft, dass sie auch anschlagen
* die Konfigurationsdatei gegen die Registry

Nicht getestet (hier nicht moeglich): Linken, Starten eines Servers,
tatsaechliches Laufen auf einer Karte, Zusammenspiel mit einem echten Client.
Die Aenderungen am Zustandsautomaten (Festhaengen, Kartenwechsel) sind
Codepruefung gegen den Core, kein Laufzeitnachweis.

---

# Was sich geaendert hat

> **Aelterer Abschnitt.** Er stammt aus der vorangegangenen Ueberarbeitung und
> ist unveraendert uebernommen. Zahlen darin (etwa "89 Eintraege") und einzelne
> Dateien sind inzwischen ueberholt: die Registry hat 93 Werte, und das dort
> beschriebene Bedienfenster `AT_GUI.lua` (`/at gui`) liegt nicht im
> Addon-Repository. Dort gibt es das Panel (`AT_UI.lua`) und die Optionsseite
> (`AT_Options.lua`). Massgeblich fuer den heutigen Stand ist der Abschnitt
> "Version 4.0" oben.

Modul und Addon wurden neu geschrieben. Die im Spiel erprobten Loesungen der
Vorfassung sind erhalten geblieben -- die streckenbasierte Bergstrafe, die
Etagenwahl am Routenverlauf, die Mehrflaechen-Pruefung fuer Bruecken, A* im
Knotengraphen. Neu ist alles, was darum herum fehlte.

---

## 1. Die Uebergabe an den Spieler gab es gar nicht

Das war die Hauptforderung, und sie war nicht umgesetzt. Die Vorfassung nahm
mit `SetClientControl(player, false)` die Steuerung und gab sie erst am Ende
der Reise, bei Kampfbeginn oder beim Abbruch zurueck. Dazwischen konnte der
Spieler **nichts** tun: bei entzogener Kontrolle erzeugt ein Tastendruck nicht
einmal ein Bewegungspaket, das der Server sehen koennte.

### Was jetzt passiert

**Uebernehmen** geschieht bewusst -- ueber den grossen Knopf im Panel, den
mittleren Mausknopf auf dem Minimap-Symbol oder eine frei belegbare Taste unter
*Tastaturbelegung -> AutoTravel*. Der Server gibt die Kontrolle im selben
Serverakt zurueck; der Befehl laeuft in einer eigenen Warteschlange mit 0,1 s
Abstand statt der 0,35 s, mit denen Routen uebertragen werden.

**Zurueckgeben** geschieht von selbst. Das Addon beobachtet, ob der Spieler
etwas tut, und meldet Ruhe:

```
Bewegung   Mausblick   Mausbewegung   Maustasten   Modifikatortasten
Fallen     Kampf       Zaubern        offener Chat
offene Fenster (Beute, Haendler, Quest, Post, Bank, Flugmeister, Karte ...)
```

Nach der eingestellten Ruhezeit (Standard 8 s) laeuft ein sichtbarer Countdown
(Standard 3 s), den jede Eingabe abbricht. Erst danach uebernimmt der Autopilot.

### Warum kein Haken auf WASD

Naheliegend waere, die Bewegungstasten mitzulesen. Das geht in 3.3.5a nicht
verlustfrei:

* `SetPropagateKeyboardInput` gibt es erst ab Cataclysm. Ein Frame mit
  `EnableKeyboard(true)` schluckt jede Taste, auch Enter und Screenshot.
* `SetOverrideBindingClick` leitet eine Taste auf einen Addon-Knopf um -- dann
  bewegt sie nicht mehr.
* Weiterreichen ist nicht moeglich: `MoveForwardStart()` ist protected und wird
  aus Addoncode als "tainted" abgewiesen.

Der Tastendruck ginge also in jedem Fall verloren. Deshalb: **uebernommen wird
per Knopf, zurueckgegeben wird beobachtend.** Fuers Beobachten reicht die
Auswertung oben vollstaendig aus, und sie kostet keinen einzigen Tastendruck.

### Der Playerbot bleibt an

Beim Pausieren wird der Selbstmodus ausdruecklich **nicht** abgeschaltet -- weder
von Hand noch bei Kampfbeginn. Sonst koennte sich der Charakter genau in dem
Moment nicht wehren, in dem er es braucht. `AutoDisableBot` steht neu auf
**aus**, betrifft ohnehin nur das Ende der ganzen Reise.

### Nach dem Kampf

Die Vorfassung fuhr zwei Sekunden nach Kampfende einfach weiter und nahm dem
Spieler die Steuerung mitten im Pluendern wieder weg. Jetzt geht der Server
nach dem Kampf in dieselbe Uebergabe wie nach einer Handpause und wartet auf
die Ruhemeldung des Addons. Ist kein Addon da, faehrt er wie bisher direkt
weiter -- es gaebe sonst niemanden, der die Meldung schicken koennte.

---

## 2. Ein Absturz des Weltservers durch einen Diagnosebefehl

```cpp
Dbg(player, _sessions.at(player->GetGUID()), b);
```

`std::map::at()` wirft `std::out_of_range`, wenn der Schluessel fehlt. Diese
Zeile lief in `TryPathBetween()` bei eingeschaltetem Debug -- und `.at diag`
ruft `TryPathBetween()` fuer einen Spieler auf, der gar keine Sitzung hat. Mit
`AutoTravel.Debug = 1` beendete ein harmloser Diagnosebefehl den Weltserver.

Behoben durch `Find()`, das `nullptr` liefert statt zu werfen.

---

## 3. Die halbe Optionsseite war wirkungslos

Die Optionsseite des Addons schickte `natural`, `contour`, `contour_elevation`,
`contour_slope`, `contour_narrow`, `contour_wide` und `contour_factor`. Der
Server kannte in `SetOption()` genau zwei Schluessel:

```cpp
if (key == "arrival") { ... }
else if (key == "grace") { ... }
else Msg(player, "Unbekannte Option: " + key);
```

Alle sieben liefen ins Leere. Jeder Haken auf der Seite tat nichts.

Neu gibt es **eine** Tabelle in `AutoTravel_Config.cpp` mit 89 Eintraegen, aus
der sich das Laden aus der Konfigurationsdatei, `.at set` und `.at options`
gemeinsam bedienen. Ein neuer Wert wird an einer Stelle eingetragen und ist
ueberall verfuegbar. `conf/autotravel.conf.dist` wird aus derselben Tabelle
erzeugt und kann deshalb nicht davon abweichen.

Zusaetzlich pruefen die Auslieferungstests, dass jeder Schluessel, den das Addon
sendet, im Server existiert.

---

## 4. Das Addon war beim ersten Optionsklick kaputt

```lua
function AT.SetServerOption(key, value)
   ...
   Send("at set " .. tostring(key) .. " " .. v)   -- Zeile 102
end
...
local function Send(cmd)                          -- Zeile 126
```

`Send` ist eine **lokale** Funktion, die erst 24 Zeilen spaeter deklariert
wird. Der Aufruf in Zeile 102 sucht deshalb ein globales `Send`, findet nil und
wirft "attempt to call global 'Send' (a nil value)". Jeder Haken auf der
Optionsseite loeste diesen Fehler aus.

---

## 5. Die Optionsseite war nur zur Haelfte erreichbar

Die Seite reichte bis y = -840. Das Optionsfenster von 3.3.5a ist rund 600
Pixel hoch. Alles ab dem Abschnitt "Teleport" war schlicht nicht sichtbar und
nicht anklickbar.

Neu: ein `ScrollFrame`. Die Positionen werden ausserdem mitgezaehlt statt von
Hand gesetzt -- eine neue Zeile in der Mitte verschiebt jetzt alles Folgende
von selbst, statt zwanzig Zahlen ungueltig zu machen.

---

## 6. Es gab keine Flugmeister, keine Flugmounts, keine Transporte

Die Vorfassung meldete bei einer Sonderverbindung nur "dort musst du selbst
fliegen oder das Portal nehmen" und wartete.

### Flugmeister

Neu sucht der Server die guenstigste Verbindung zwischen zwei **bekannten**
Flugpunkten -- Dijkstra ueber `sTaxiPathSetBySource`, mit Fraktionspruefung
ueber `MountCreatureID` und der Flugpunktmaske des Charakters. Er laeuft hin und
startet den Flug mit `ActivateTaxiPathTo`. Zwischenstationen sind erlaubt.

Zwei Bedingungen, beide einstellbar: der Flug muss mindestens 60 Prozent der
verbleibenden **Laufstrecke** sparen (die Flugstrecke selbst zaehlt nicht als
Laufweg), und er darf hoechstens 5 Gold kosten.

Eine Feinheit: vor einem Flugmeister gilt ein eigener, kleinerer Radius
(`TaxiBoardDistance`, 8 Yards) statt des Etappenradius von 15. Die Fassung 3.x
begruendete das damit, der Core lasse den Abflug nur innerhalb von
`2 * INTERACTION_DISTANCE` zu. **Das stimmt im aktuellen AzerothCore nicht
mehr**: `Player::ActivateTaxiPathTo` prueft die Entfernung bei einem Aufruf ohne
Flugmeister-NPC nicht. Der kleinere Radius bleibt trotzdem sinnvoll -- er stellt
sicher, dass der Charakter wirklich am Flugpunkt steht, bevor der Flug
beginnt -- ist aber eine Entscheidung des Moduls, keine Vorgabe des Cores.

### Eigenes Flugmount

Wo Fliegen erlaubt ist, wird eine Luftroute gebaut: Steigflug, Reiseflug entlang
eines Hoehenprofils, Sinkflug.

```
              ____________
             /            \.
        ____/    Berg      \____
       /                        \.
      A                          B
```

Das Profil wird zweimal mit dem Maximum der Nachbarschaft geglaettet, damit die
Route **vor** dem Berg steigt statt hineinzufliegen. Damit sind Klippen,
Wasser und Berge auf einen Schlag erledigt.

Ob Fliegen ueberhaupt erlaubt ist, wird nicht geraten: nach dem Aufsitzen sagt
`CanFly()`, ob der Core das Flugtempo gewaehrt hat. In Azeroth gewaehrt dasselbe
Mount nur Bodentempo -- dann wird gelaufen, ohne dass jemand eine Zonenliste
pflegen muss.

### Zeppelin, Schiff, Tiefenbahn

Steht der Charakter auf einem Transport, uebergibt der Autopilot vollstaendig
und wartet. Weder Wegfindung noch Bodenrettung noch Feststeck-Erkennung ergeben
dort einen Sinn -- alle drei wuerden sofort und dauernd ausloesen, weil sich der
Boden unter dem Charakter wegbewegt. Steigt er aus, wird von der neuen Position
aus weitergerechnet. Das deckt alle drei Fahrzeugarten ab, ohne fuer jedes
Sonderwissen zu brauchen.

---

## 7. Nur zwei Etagen waren zu wenig

`FindGroundPlanes()` suchte genau zwei Ebenen. Das reicht fuer einen Torbogen,
nicht fuer die Stellen, an denen es darauf ankommt: die Bank von Sturmwind, das
Wirtshaus mit Galerie, die Rampen der Tiefenbahn, Eisenschmiede mit drei
uebereinanderliegenden Ringen.

Neu wird von oben nach unten durchgetastet, bis die eingestellte Zahl an Etagen
gefunden ist (Standard 4, einstellbar bis 8) oder das Suchfenster verlassen
wurde.

Die Bewertung kennt jetzt zusaetzlich `MaxStepUp` und `MaxStepDown`: nach oben
ist eine Treppenstufe normal, nach unten ist ein Absatz von mehreren Yards
verdaechtig. Vorher galt dieselbe Grenze in beide Richtungen.

---

## 8. Klippen und Wasser flossen nicht in die Bewertung ein

Zwei Aufschlaege sind dazugekommen, beide in Yards und damit mit der Wegstrecke
vergleichbar:

* **Klippen**: ein Hoehensturz ueber eine sehr kurze horizontale Strecke. Ein
  Hang faellt ueber viele Yards, eine Klippe auf einmal. Faellt es ins Wasser,
  zaehlt es nicht.
* **Schwimmstrecke**: jeder Yard im Wasser kostet extra. Damit gewinnt der Weg
  am Ufer entlang gegen den Weg quer durch den See, solange der Umweg im
  Rahmen bleibt.

---

## 9. Bewegung ohne Tempoangabe

`MoveSplineInit` bekam nur `MovebyPath()` und `SetWalk(false)`. Die
Geschwindigkeit blieb dem Core ueberlassen, der sie aus den Bewegungsflags
ableitet -- und die werden im selben Atemzug von Hand gesetzt.

Neu wird sie ausdruecklich gesetzt: `MOVE_FLIGHT` beim Fliegen, `MOVE_SWIM` im
Wasser, sonst `MOVE_RUN`. Ausserdem dreht sich der Charakter am Ende jedes
Abschnitts in Fahrtrichtung, statt in die zuletzt vom Client gemeldete Richtung
zu springen.

---

## 10. Ein uint8, in den ein uint32 geschrieben wurde

Die Optionsregistry adressiert die Konfigurationsfelder ueber `offsetof` und
schreibt sie ueber einen typgerechten Zeiger. `groundPlanes` war als `uint8`
deklariert, in der Registry aber als `OPT_UINT` eingetragen -- ein Schreibzugriff
haette vier Bytes in ein Ein-Byte-Feld geschrieben und die Nachbarfelder
mitgenommen.

Gefunden hat das ein Pruefskript, das die Typen in Kopfdatei und Registry
gegeneinander haelt. Es laeuft jetzt bei jeder Auslieferung mit, zusammen mit:

* jede deklarierte Methode hat genau eine Definition
* jeder Optionsschluessel des Addons existiert im Server
* jeder Chatbefehl des Addons wird vom Server behandelt
* jeder Zustandsname des Servers ist im Panel hinterlegt
* jede `AT.*`-Funktion, die aufgerufen wird, ist auch definiert

Zusaetzlich wurde das Modul gegen Attrappen der Core-Schnittstelle uebersetzt
und gebunden, und das Addon in einer nachgebauten Oberflaeche geladen und
durchgespielt: Handschlag, Statuspaket, Pause, Countdown, Abbruch durch
Bewegung, Wiederaufnahme, Ende, alle Slash-Befehle, Optionsseite.

---

## 11. Nachtrag: AFK-Kennzeichen, Bedienfenster, Botpad

Vier Punkte aus der Rueckmeldung.

### Das AFK-Kennzeichen ruht waehrend der Fahrt

Der Client setzt es nach ein paar Minuten ohne Tastendruck von selbst und
schickt es als `CHAT_MSG_AFK` an den Server. Waehrend der Autopilot faehrt, ist
das schlicht falsch -- der Charakter legt Strecke zurueck. Sichtbar wird der
Unterschied spaetestens im Schlachtfeld, wo ein AFK-Kennzeichen zum Hinauswurf
fuehrt.

Verhindern laesst sich das Setzen nicht: die Entscheidung faellt im Client, und
ein Addon kann dessen Leerlaufzaehler nicht zuruecksetzen. Loeschen laesst es
sich aber. `UpdateSession()` prueft im Takt (Standard 200 ms) und ruft
`ToggleAFK()`, solange die Reise laeuft.

Bewusst NICHT waehrend `AT_PLAYER_CONTROL`: dort steuert der Spieler, und wenn
er wirklich weggeht, soll das auch so angezeigt werden. Sobald der Autopilot
uebernimmt, faellt das Kennzeichen im naechsten Takt weg.

Abschaltbar mit `AutoTravel.SuppressAfk = 0` oder `.at set afk 0`. Der
Handschlag `[AT]H` traegt den Zustand jetzt als fuenftes Feld, damit das Addon
ihn anzeigen kann; das Addon liest die Felder gezaehlt statt gemustert und
kommt deshalb auch mit einem aelteren Modul zurecht, das nur vier schickt.

### Ein Bedienfenster

`AT_GUI.lua`, sechs Reiter: Reise, Uebergabe, Bot, Wege, Anzeige, Info. Zu
oeffnen mit `/at gui`, ueber den Stern im Panel oder mit Umschalt+Linksklick
auf das Minimap-Symbol. Escape schliesst es, die Position wird gemerkt.

Das kleine Panel bleibt, was es war: der schmale Statusstreifen am Rand. Die
Seite unter *Interface -> AddOns* bleibt die vollstaendige Werteliste. Alle
drei lesen und schreiben dieselben Werte und rufen sich gegenseitig zum
Auffrischen -- es gibt keinen zweiten Zustand.

Dabei ist eine Unschoenheit aufgefallen, die es vorher schon gab: nach dem
Handschlag schickte das Addon **alle** Serveroptionen auf einmal. Die sind aber
serverweit und verlangen Spielleiterrechte, und der Handschlag laeuft nach
jedem Ladebildschirm. Ein normaler Spieler haette bei jedem Zonenwechsel ein
Dutzend Absagen im Chat bekommen, ein Spielleiter serverweite Werte auf seinen
persoenlichen Stand zurueckgedreht. Jetzt geht nur noch `arrival` automatisch
raus -- der Zielradius, den jeder fuer sich setzen darf. Die uebrigen wandern
nur beim ausdruecklichen Aendern zum Server, oder ueber einen eigenen Knopf auf
dem Reiter *Wege*.

### Bot-Einstellungen bleiben, wenn der Selbstmodus schon laeuft

Die alte Fassung rief beim Reisestart bedingungslos `Enable()` und damit
`ApplyProfile()` -- also `co !`, `nc !` und danach das eigene Profil. Wer den
Bot vorher eingerichtet hatte, verlor diese Einrichtung mit dem ersten
Reisestart.

Jetzt entscheidet `B.PrepareForTravel()` anhand eines **dreiwertigen** Zustands:

```
laeuft   -> nichts anfassen, kein nc !, kein co !, kein Profil
aus      -> einschalten und das Profil setzen
unbekannt-> erst fragen, dann eins von beiden
```

Das nil ist der springende Punkt. "Ich weiss es nicht" und "der Bot ist aus"
fuehren zu gegensaetzlichem Verhalten; eine Funktion, die beides als false
zurueckgibt, wuerde bei jedem ersten Reisestart nach dem Login die
Einstellungen ueberschreiben. Deshalb gibt es `B.KnownState()` neben
`B.IsRunning()`.

Gefragt wird mit `nc ?` -- einer reinen Abfrage, die nichts aendert. Antwortet
der Bot innerhalb von vier Sekunden mit einer Fluesternachricht an den eigenen
Charakter, laeuft er. Kommt nichts, ist er aus. Die Reise wartet darauf nicht;
sie haengt nicht am Bot.

Ausgeschaltet wird am Ende nur, was AutoTravel selbst eingeschaltet hat
(`B.turnedOnByUs`). War der Bot vorher an, bleibt er an.

### Botpad laeuft daneben

Drei Beruehrungspunkte, alle in `AT_Compat.lua` behandelt, ohne eine Zeile in
Botpad zu aendern:

* **Doppelte Meldungen.** Botpad hoert ebenfalls auf `CHAT_MSG_SYSTEM` und gibt
  `[AT]M`-Zeilen selbst aus. Ein Chatfilter hilft dagegen nicht --
  `ChatFrame_AddMessageEventFilter` greift in den Anzeigeweg ein, nicht in
  fremde Ereignisbehandlungen. Deshalb wird `Botpad.Print` umschlossen. Die
  Huelle laesst alles durch und unterdrueckt genau eine Sache: einen Text, den
  AutoTravel im selben Moment schon ausgegeben hat.
* **Wer richtet den Bot ein.** Die Regel oben gilt unabhaengig von Botpad: wer
  den Selbstmodus einschaltet, behaelt seine Einstellungen. `B.KnownState()`
  liest zusaetzlich `Botpad.Bot.running`, wenn AutoTravel selbst noch nichts
  gesehen hat. Wer ausschliesslich mit Botpad arbeiten will, nimmt den Haken
  bei "Playerbot mitsteuern" heraus.
* **Chatfilter.** Beide verbergen ihre eigenen Botbefehle. Die Filterkette ruft
  beide auf und es genuegt, wenn einer true liefert -- hier war nichts zu tun.

---

## 12. Nachtrag: Baufehler auf aktuellem AzerothCore

```
AutoTravel_Taxi.cpp:100: member reference type 'const TaxiPathEntry *const'
                         is a pointer; did you mean to use '->'?
```

`sTaxiPathSetBySource` hat den Wertetyp gewechselt: frueher stand die Struktur
`TaxiPathBySourceAndDestination` direkt im Container, heute ein Zeiger auf
`TaxiPathEntry`. Beide tragen ein Feld `price`, einmal ueber `.` und einmal
ueber `->` erreichbar.

Der Zugriff sitzt jetzt in einer Stelle:

```cpp
template<class T>
inline uint32 TaxiPriceOf(T const& v)
{
    if constexpr (std::is_pointer<T>::value)
        return v ? uint32(v->price) : 0u;
    else
        return uint32(v.price);
}
```

`if constexpr` statt zweier Ueberladungen: eine Ueberladung auf `T const&` und
eine auf `T const*` passen fuer ein Zeigerargument beide exakt, und die
Aufloesung haengt dann an der partiellen Ordnung von Funktionstemplates --
unnoetig heikel fuer eine so kleine Sache.

Gegengeprueft wurde gegen Attrappen **beider** Corestaende; die Datei
uebersetzt in beiden Faellen.

Bei der Gelegenheit zwei Includes nachgezogen, die bisher nur zufaellig ueber
`Player.h` hereinkamen: `SpellAuraDefines.h` in `AutoTravel_Session.cpp` (fuer
`SPELL_AURA_MOUNTED`) und `<cstddef>` in `AutoTravel_Config.cpp` (fuer
`offsetof`).

---

## 13. Nachtrag: Fluege, die es gar nicht gibt

Aus einem Protokoll im Spiel:

```
Flugpunkte: 3 bekannt, davon 3 fuer deine Fraktion nutzbar.
Die Route benutzt 1 Sonderverbindung(en).
Reise gestartet: Goto 32, 41
Der Flug kam nicht zustande - es geht zu Fuss weiter.
```

Ein Charakter der Stufe 1 kennt genau einen Flugpunkt. Der Knotengraph von
mod-playerbots kennt trotzdem Flugverbindungen zwischen seinen Knoten -- er
beschreibt ja, was es an Verbindungen GIBT, nicht, was DIESER Charakter
benutzen kann.

### Was schieflief

`BuildNodeRoute()` uebernahm den Verbindungstyp ungeprueft: Typ 4 wurde zu
`AT_LEG_TAXI`. Die Etappe landete in der Route, der Charakter lief brav zum
Flugmeister, und dort scheiterte `StartTaxi()` -- weil die Etappe aus dem
Graphen gar keine Flugpunkte trug. `leg.taxiFrom` und `leg.taxiTo` waren
schlicht 0.

Das war doppelt aergerlich: der Weg zum Flugmeister war umsonst, und die
Meldung kam erst, nachdem er gelaufen war.

### Was jetzt passiert

Jede Sonderverbindung der fertigen Kette wird gegen die echten Flugpunkte
geprueft, BEVOR die Reise beginnt. Dafuer gibt es `ResolveTaxiHop()`: es
beantwortet fuer eine Verbindung alles, was der Core beim Abflug auch prueft --
Punkt bekannt, Fraktion passt, Verbindung vorhanden, Preis unter der Grenze,
Geld reicht.

Faellt eine Verbindung durch, wird die Kante **gesperrt und die Suche laeuft
erneut** (bis zu fuenfmal). Findet sich kein Weg mehr, faellt AutoTravel auf
die Carbonite-Route und damit aufs Laufen zurueck -- also genau auf das, was
vorher auch passiert ist, nur ohne den Umweg zum Flugmeister.

Haelt eine Verbindung stand, wandern die aufgeloesten Flugpunkte in die Etappe,
**und die Etappe rueckt auf die Position des echten Flugmeisters**. Der
Graphknoten liegt meist ein paar Yards daneben, und der Core laesst den Abflug
nur innerhalb von zwei Interaktionsdistanzen zu.

### Frueher abbrechen

`BuildTaxiPlan()` zaehlt jetzt zuerst die nutzbaren Flugpunkte. Unter zwei
davon hat kein Flug eine Chance, und die ganze Suche kann entfallen. `.at taxi`
zeigt die Zahl deshalb auch im Verhaeltnis:

```
Flugpunkte: 3 von 254 bekannt, davon 3 fuer deine Fraktion nutzbar.
```

Das haette die Lage sofort erklaert. Vorher stand dort nur "3 bekannt" -- ohne
Bezugsgroesse sieht das nach viel aus.

### Zum Taxi-Cheat

`.cheat taxi on` setzt die komplette Flugpunktmaske des Charakters. Da
AutoTravel ausschliesslich diese Maske liest (`m_taxi.IsTaximaskNodeKnown`),
funktioniert die Automatik damit sofort und vollstaendig -- ohne Sonderfall im
Code. Das ist die richtige Kopplung: die Maske ist die einzige Wahrheit, und
der Core prueft sie beim Abflug noch einmal selbst.

`.at taxi` weist bei zu wenigen Flugpunkten ausdruecklich darauf hin.

---

## 14. Nachtrag: Rohzeilen im Chat

Ebenfalls aus dem Protokoll:

```
AutoTravel: Rohgelaende dort: 338.49 (Ziel-Z 338.49)
[AT]Mohgelaende dort: 338.49 (Ziel-Z 338.49)
```

Zwei Fehler auf einmal.

Erstens stand die Rohzeile ueberhaupt da. Der Chatfilter blendete sie nur aus,
wenn "HideProtocol" gesetzt war -- ein Schalter, den man versehentlich umlegen
kann. Rohzeilen sind aber unter keinen Umstaenden nuetzlich.

Zweitens ist sie verstuemmelt: aus `[AT]M|Rohgelaende` wurde `[AT]Mohgelaende`.
Das Trennzeichen `|` leitet im Client eine Escapefolge ein, und `|R` wird beim
Zeichnen geschluckt. Die Auswertung im Addon ist davon nicht betroffen -- sie
sieht den Text vor dem Rendern -- angezeigt werden darf so etwas trotzdem nicht.

Neu: der Filter blendet `[AT]`-Zeilen **immer** aus. Der Schalter heisst jetzt
"ChatMessages" und entscheidet ueber die LESBARE Fassung.

---

## 15. Nachtrag: Das Fenster schliesst sich nicht mehr von selbst

Drei Ursachen, alle behoben:

* Das Fenster stand in `UISpecialFrames`. Bequem, wenn man es kurz aufmacht --
  laestig, wenn es offen stehen soll: jede Flucht aus einem anderen Fenster,
  jeder Griff zum Spielmenue macht es zu. Der Eintrag ist jetzt abschaltbar und
  standardmaessig **aus**.
* Nach `/reload` oder einem Neuanmelden war es weg. Der offene Zustand wird
  jetzt gemerkt und wiederhergestellt.
* Der Schliessknopf ging an `frame:Hide()` vorbei am gemerkten Zustand. Jetzt
  laeuft alles ueber `G.Close()`.

---

## 16. Nachtrag: Eine Pruefstrecke

Bisher liefen die Pruefungen von Hand. Sie liegen jetzt als
`autotravel-tools/` bei und laufen mit einem Aufruf:

```
./pipeline.sh              nur pruefen
./pipeline.sh --fix        autotravel.conf.dist neu erzeugen, dann pruefen
./pipeline.sh --package    nach bestandener Pruefung die Zips bauen
```

Fuenf Schritte: Servermodul gegen Attrappen der Core-Schnittstelle uebersetzen
(beide Auspraegungen von `sTaxiPathSetBySource`), Lua-Syntax mit `luac5.1`,
rund fuenfzig Durchlaufpruefungen in einer nachgebauten Oberflaeche, Servermodul
und Addon gegeneinander halten, und die Konfigurationsdatei gegen die Registry.

Die Konfigurationsdatei wird dabei nicht mehr gepflegt, sondern **erzeugt**.
Beim ersten Lauf hat die Strecke prompt gemeldet, dass die ausgelieferte
`.dist` nicht die erzeugte war -- genau der Fall, fuer den sie da ist.

Was die Strecke nicht kann, steht in ihrer README: sie ersetzt weder den Build
gegen den echten AzerothCore noch eine Fahrt im Spiel.

---

## 17. Kleinigkeiten

* `RouteAdd()` benutzte `atoi`/`atof` auf Spielereingaben; jetzt die gepruefte
  Umwandlung, die im Rest des Moduls schon verwendet wurde.
* `ATLeg` kennt jetzt eine `mapId`. Ohne sie liessen sich Knoten- und
  Taxietappen auf anderen Karten nicht sauber behandeln.
* Der Fortschrittsbalken wird vom Server berechnet statt aus der schwankenden
  Restdistanz im Client geschaetzt; bei einem Zwischenziel sprang er sonst
  zurueck.
* `WorldMapArea.dbc` wird jetzt auch unter `frFR` und `ruRU` gesucht, und ein
  unplausibel grosser Dateikopf wird abgewiesen, statt Speicher dafuer
  anzufordern.
* `.at set` fuer serverweite Werte verlangt jetzt Spielleiterrechte. Vorher
  haette `/at ziel 20` den Zielradius fuer alle Spieler mitverstellt.
* Der Handschlag `.at hello` sagt dem Addon, ob das Modul da, aktiv und wie
  bestueckt ist. Fehlermeldungen koennen dadurch unterscheiden, ob das Addon
  oder das Modul schuld ist.

---

## Geprueft und bewusst nicht geaendert

* **Streckenbasierte Bergstrafe.** Fenster fester Laenge in festen Abstaenden
  entlang der Strecke -- damit haengt die Strafe an der Geometrie und nicht an
  der Punktdichte. Bleibt genau so.
* **Etagenwahl am Routenverlauf** statt an der Spielerposition. Das ist der
  Kern gegen "bleibt unter der Treppe". Bleibt.
* **Mehrflaechen-Pruefung in `TryPath`**, damit eine Kanalbruecke nicht die
  ganze Route kippt. Bleibt, mit der Toleranz von einem Viertel.
* **A\* statt Dijkstra** im Knotengraphen, mit `f = g + h` in der
  Warteschlange und Vergleich gegen `g`. Bleibt.
* **Umweg am Routenanfang abschneiden.** Bleibt, jetzt mit zusaetzlicher
  Pruefung, dass beide Knoten auf derselben Karte liegen -- eine Luftlinie
  ueber Kartengrenzen hinweg ist bedeutungslos.
* **`ChunkPoints = 12`.** Ohne Messung im Spiel wird ein funktionierender Wert
  nicht geaendert.

---

## Zur Frage: Navigation auf den Client verlagern?

Kurz: nein, und es wuerde das Problem auch nicht loesen.

**Bewegung ist clientseitig nicht ausloesbar.** `MoveForwardStart()`,
`TurnLeftStart()` und alle verwandten Funktionen sind in 3.3.5a protected. Ein
Client-Router muesste seine Ergebnisse also doch wieder an den Server schicken,
damit der bewegt -- dieselbe Architektur wie jetzt, nur mit der Wegfindung auf
der schwaecheren Seite.

**Der Client hat die noetigen Daten nicht.** Lua kennt weder Terrainhoehen noch
Kollisionsgeometrie noch das NavMesh. Es gibt keine Moeglichkeit, aus einem
Addon heraus zu erfahren, ob ein Punkt begehbar ist. Genau diese Frage ist der
Kern der Wegfindung.

**Das Clipping kaeme davon nicht weg.** Es entsteht, weil die Z-Werte des
serverseitigen Splines nicht exakt zur Kollisionsgeometrie passen, die der
Client rendert. Wer die Route auf dem Client rechnet, aendert daran nichts --
bewegt wird weiterhin per Spline vom Server.
