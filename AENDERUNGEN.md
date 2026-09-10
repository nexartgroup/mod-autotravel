# Was sich geaendert hat

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

Eine Feinheit, die leicht uebersehen wird: der Core laesst den Abflug nur
innerhalb von `2 * INTERACTION_DISTANCE`, also 10 Yards, zu. Der normale
Etappenradius von 15 Yards haette jeden Flug mit "zu weit weg" abgelehnt.
Deshalb gilt vor einem Flugmeister ein eigener, kleinerer Radius.

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

## 11. Nachtrag: Baufehler auf aktuellem AzerothCore

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

## 12. Kleinigkeiten

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
