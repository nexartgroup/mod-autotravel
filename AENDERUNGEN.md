# Prüfbericht und Änderungen

Der hochgeladene Stand wurde geprüft, nicht neu geschrieben. Gefunden wurden
**ein schwerer Bewertungsfehler**, eine **Sicherheitslücke** und drei kleinere
Mängel.

---

## 1. Die Bergstrafe hing an der Punktdichte, nicht am Gelände

`ScoreNaturalPath()` startete für **jeden Punkt** ein neues Fenster. Damit
zählte dieselbe Steigung so oft, wie Punkte darauf lagen — und die Punktzahl
hängt vom Pfadmodus ab, nicht vom Berg.

Gemessen an einem Hang von 200 yd Länge und 30 yd Anstieg:

```
Abtastung                     Punkte  Fensterstrafe
alle   2.0 yd ein Punkt          101              0
alle   4.0 yd ein Punkt           51              0
alle   8.0 yd ein Punkt           26            768
alle  20.0 yd ein Punkt           11           1000
```

Zwei Fehler zugleich:

* **Punktdichte**: gleicher Berg, Strafen zwischen 0 und 1000.
* **Fenster zu kurz**: bei 10 yd Fenster steigt auch ein langer Berg nur ein
  bis zwei Yards und blieb unter der Schwelle von 2 yd. Auf geglätteten
  Pfaden (etwa 4 yd Schrittweite) feuerte die Bergstrafe deshalb **nie** —
  ausgerechnet auf denen, die normalerweise benutzt werden.

Die Einheit war zusätzlich falsch: `travelled * gain * penalty` ergibt
Yards² und explodiert gegenüber der Wegstrecke, die in Yards zählt. Das ist
die Ursache für Umwege, die in keinem Verhältnis zum vermiedenen Hügel stehen.

**Neu:** Fenster fester Länge (40 yd), gesetzt in festen Abständen entlang der
**Strecke** statt je Punkt. Die Strafe wird in Yards gerechnet und ist damit
mit der Wegstrecke vergleichbar.

```
Punktdichte-Test (200 yd Hang, 30 yd Anstieg)
  alle   2.0 yd: 101 Punkte -> Strafe     54
  alle   4.0 yd:  51 Punkte -> Strafe     54
  alle   8.0 yd:  26 Punkte -> Strafe     54
  alle  20.0 yd:  11 Punkte -> Strafe     54
```

Und gegen die Beispiele der Vorlage:

```
  100 yd eben                Strecke   100 + Berg     0 =    100
  110 yd sanft ansteigend    Strecke   108 + Berg     0 =    108
  125 yd um den Berg herum   Strecke   124 + Berg     0 =    124
  105 yd steil hinauf        Strecke   104 + Berg   302 =    406
```

Der Weg um den Berg gewinnt deutlich, ohne dass ein 3-km-Umweg attraktiv wird.

Neu kalibriert: `ElevationWindow 10 → 40`, `GainStart 2 → 4`,
`GainStrong 6 → 10`, `GainExtreme 8 → 20`, Strafen `5/20/80 → 3/8/20`.

---

## 1b. Rettung setzte auf Treppen unter die Stadt

Auf Treppen und Rampen rutscht der Charakter durch die Stufe. Die Rettung
suchte die Bodenhöhe **von `pz + 2` abwärts über 200 Yards**. Sobald er mehr
als zwei Yards durchgerutscht war, lag `pz + 2` unter der Stufe — und die
nächste gefundene Fläche war das Rohgelände unter der Stadt:

```
Stufe 95.0, Rohgelaende unter der Stadt 59.0
pz           ALT (pz+2)   NEU (Pfad-Z)   Ergebnis
94.5               95.0           95.0   ALT ok    / NEU ok
93.0               95.0           95.0   ALT ok    / NEU ok
92.0               59.0           95.0   ALT STURZ / NEU ok
88.0               59.0           95.0   ALT STURZ / NEU ok
80.0               59.0           95.0   ALT STURZ / NEU ok
```

**Neu:** Bezug ist der **Pfad selbst**. Seine Punkte stammen aus dem NavMesh
und liegen damit auf der richtigen begehbaren Ebene. Gesucht wird um den
nächstgelegenen Pfadpunkt herum und nur in einem engen Fenster
(`RescueSearchRange`, Standard 14 yd), damit keine andere Etage gewinnen kann.

Zusätzlich zwei Sicherungen:

* Liegt die gefundene Fläche mehr als das Suchfenster **unter** der Pfadhöhe,
  ist es eine andere Etage — dann wird **gar nicht** zurückgesetzt. Lieber
  einmal nicht helfen als in den Keller versetzen.
* Die zuletzt bestätigt gute Höhe wird laufend mitgeführt und dient als
  Rückfallebene, wenn gerade kein Pfad vorliegt.

---

## 2. `.at tp` war für jeden Spieler offen

```cpp
{ "at", HandleAt, SEC_PLAYER, Console::No }
```

Der Teleport lag unter demselben `SEC_PLAYER` wie alles andere und nimmt
beliebige Zielkoordinaten entgegen. Jeder Spieler konnte sich überallhin
versetzen.

**Neu:** eigene Rechteprüfung nur für `tp`, über
`AutoTravel.TeleportSecurity` (Standard 2 = GM). Die übrigen Unterbefehle
bleiben offen — sie können nichts, was der Spieler nicht ohnehin darf.

---

## 3. Ungeprüfte Umwandlung von Spielereingaben

14 Aufrufe von `atoi()`/`atof()` auf Werten aus Chatnachrichten. `atoi("abc")`
ist still `0`, `atof("nan")` ergibt NaN — und NaN pflanzt sich durch die
gesamte Wegfindung fort.

**Neu:** `ParseUInt`, `ParseFloat`, `ParseBool`, `ParseNorm` mit `strtoul`/
`strtod`, Prüfung auf vollständigen Verbrauch, `ERANGE`, `isfinite` und
Wertebereich 0..1 für normalisierte Koordinaten. Ungültige Eingabe wird
abgewiesen statt in eine 0 verwandelt.

---

## 4. Knotensuche war Dijkstra

Dijkstra breitet sich gleichmäßig in alle Richtungen aus. **Neu:** A* mit
Luftlinien-Schätzung.

Die Schätzung ist zulässig, weil die Kantenkosten aus Weglängen stammen und
ein Weg nie kürzer als die Luftlinie ist. Bei Knoten auf **anderen Karten**
ist eine Luftlinie bedeutungslos — dort ist die Schätzung 0 und A* verhält
sich wie Dijkstra.

Dabei berichtigt: die Warteschlange enthält jetzt `f = g + h`, der Vergleich
läuft aber gegen `g`. Ohne diese Trennung hätte A* falsche Wege gewählt.

---

## 5. Umweg am Routenanfang

Der nächstgelegene Knoten liegt häufig **hinter** dem Spieler; die alte Prüfung
verglich nur zwei Entfernungen und entfernte höchstens einen Knoten.

**Neu:** Vergleich des tatsächlichen Umwegs
(`|Spieler→n0| + |n0→n1|` gegen `|Spieler→n1|`), in einer Schleife für bis zu
vier Knoten.

```
Knoten 80 yd hinter dem Spieler    über 360  direkt 200  -> ÜBERSPRINGEN
Knoten seitlich, leichter Umweg    über 219  direkt 200  -> behalten
Knoten genau auf dem Weg           über 200  direkt 200  -> behalten
```

---

## 6. Schwebeschwelle zu scharf

`AboveMeshHeight` stand auf 6 yd. Auf hügeligem Gelände überspannt der Spline
regelmäßig eine Senke, ohne dass etwas kaputt ist. **Neu:** 12 yd.

---

## Geprüft, aber nicht geändert

* **Kandidatenauswahl** — der Stand wählt bereits den besten Kandidaten statt
  des ersten gültigen. Richtig so.
* **Hang- und Kurvenstrafe** — dimensionsmäßig sauber (`horizontal × Überschuss
  × Faktor` ergibt Yards) und skaliert mit der Strecke. Keine Änderung nötig.
* **Contour-Probing** — greift nur bei erkanntem Anstieg, ist also nicht der
  Normalfall. Bleibt.
* **`ChunkPoints`** — die Vorlage schlägt 12 → 6 vor. Das ist eine reine
  Geschmacksfrage ohne messbares Kriterium; ohne Test im Spiel würde ich einen
  funktionierenden Wert nicht ändern.

## Automatische Gegenprüfungen

Beim Bauen laufen jetzt zwölf Plausibilitätsprüfungen der Schwellwerte
gegeneinander (Zielradius gegen Etappenradius, Fenster gegen Anstiegsschwelle,
unvollständiger Pfad teurer als jeder Berg …). Alle zwölf sind derzeit in
Ordnung.


---

## Zur Frage: Navigation auf den Client verlagern?

Kurz: **nein**, und es würde das Problem auch nicht lösen.

**Bewegung ist clientseitig nicht auslösbar.** `MoveForwardStart()`,
`TurnLeftStart()` und alle verwandten Funktionen sind in 3.3.5a *protected* —
ein Addon darf sie nicht aufrufen. Ein Client-Router müsste seine Ergebnisse
also doch wieder an den Server schicken, damit der bewegt. Damit hätte man
dieselbe Architektur wie jetzt, nur mit der Wegfindung auf der schwächeren
Seite.

**Der Client hat die nötigen Daten nicht.** Lua kennt weder Terrainhöhen noch
Kollisionsgeometrie noch das NavMesh. Es gibt keine Möglichkeit, aus einem
Addon heraus zu erfahren, ob ein Punkt begehbar ist. Genau diese Frage ist der
Kern der Wegfindung.

**Das Clipping käme davon nicht weg.** Es entsteht, weil die Z-Werte des
serverseitigen Splines nicht exakt zur Kollisionsgeometrie passen, die der
Client rendert. Wer die Route auf dem Client rechnet, ändert daran nichts —
bewegt wird weiterhin per Spline vom Server.

**Was tatsächlich hilft**, ist genau das oben Umgesetzte: die Rettung an der
Pfadgeometrie ausrichten statt an einer Blindsuche, und im Zweifel nichts tun.
Für mehrstöckige Bereiche wie Sturmwind ist die Etagenprüfung der
entscheidende Teil.


---

# Nachtrag: Durchfallen auf Treppen

## Die Ursache war eine Verankerung an der Spielerposition

`SelectGroundPlane()` bewertete die gefundenen Flächen relativ zur **echten
Position des Charakters**. Solange er sauber auf der Stufe steht, geht das gut.
Rutscht er einmal durch, liegt seine Position näher am Hallenboden als an der
Treppe — und ab da gewinnt der Hallenboden **jede weitere Wahl**:

```
Der Charakter ist bereits durch die Stufe gerutscht.
steht bei 95.0   Ebenen {89.0, 97.0}  ->  97.0  ok
steht bei 92.0   Ebenen {89.0, 97.0}  ->  89.0  <-- bleibt unter der Treppe
steht bei 90.0   Ebenen {89.0, 97.0}  ->  89.0  <-- bleibt unter der Treppe
```

Genau das beschriebene Bild: er bleibt bis zum Ende der Treppe darunter und
wird erst oben wieder hinaufgesetzt.

Dazu kam eine **Asymmetrie** in der Bewertung: Sprünge nach oben wurden hart
bestraft (Schutz gegen Torbögen), ein Sturz auf eine tiefere Fläche war
kostenlos.

## Umsetzung deines Vorschlags

**1. Bezug ist der Routenverlauf, nicht die Spielerposition.**
Die Sollhöhe wird zwischen den benachbarten NavMesh-Punkten interpoliert. Deren
Höhen stammen aus dem Mesh und liegen auf der begehbaren Fläche — unabhängig
davon, wo der Charakter gerade steckt.

**2. Bewertung symmetrisch**, plus eine Neigungsregel: Der Übergang von der
zuletzt akzeptierten Fläche darf nicht steiler als `MaxWalkSlope` (1.2) sein.
Was steiler wäre, ist keine Lauffläche, sondern eine andere Etage.

**3. Ausreißerkorrektur im Pfad** (`FixPathZOutliers`). Weicht ein einzelner
Punkt um mehr als `ZOutlierTolerance` (2,5 yd) von der aus seinen Nachbarn
interpolierten Höhe ab, wird geprüft, ob dort eine Fläche auf Sollhöhe liegt —
und diese genommen. Steigen die Nachbarn mit, wie bei Treppe, Rampe und Hang,
bleibt der Punkt unangetastet.

**4. Kein Mitschleppen des Fehlers.** Der Segmentanfang für das nächste Stück
ist die **Routenhöhe** des erreichten Punkts, nicht die erzeugte Terrainhöhe.
Ein einmaliger Fehlgriff wandert damit nicht durch die ganze Treppe.

Gegenüberstellung mit einem einmaligen Durchrutschen bei Schritt 1:

```
i    Soll    ALT (Spieler)  NEU (Route)
0    95.0    95.0           95.0
1    97.0    97.0           97.0
2    99.0    89.0           99.0   <- ALT unter der Treppe
3   101.0    89.0          101.0   <- ALT unter der Treppe
4   103.0    89.0          103.0   <- ALT unter der Treppe
```

## Was das nicht löst

Das verhindert, dass der Charakter **unter der Treppe bleibt**. Ob er beim
ersten Schritt kurz durch die Stufe rutscht, hängt an der Kollisionsprüfung des
Clients gegen die Spline-Interpolation — dagegen hilft nur, die Punkte auf
Treppen enger zu setzen (`AutoTravel.TerrainStep` verkleinern, etwa auf 1.5).
Das kostet mehr Höhenabfragen und sollte deshalb erst gemessen werden.


---

# Nachtrag 2: "Kein begehbarer Weg" zum Hafen

## Die Diagnose enthielt die Antwort

```
Ziel: -8643.9 / 1333.4 / 5.6 | Entfernung 687 yd
Gelaendehoehe dort: -56.52 (Ziel-Z 5.64)
  Eckpunkte  Z  5.64 -> 0x4 (INCOMPLETE), 20 Punkte verworfen
```

Der Pathfinder hatte einen Pfad mit 20 Punkten gefunden — **verworfen hat ihn
AutoTravel selbst.** Zwischen Steg (5.6) und Rohgelände darunter (−56.5) liegen
62 Yards.

## Ursache: die punktweise Bodenprüfung

`TryPath()` verglich jeden Pfadpunkt gegen **eine** Bodenhöhe und verwarf die
**gesamte** Route, sobald ein einziger Punkt um mehr als 5 bzw. 8 Yards abwich.
In Sturmwind kippt damit jede Brücke und jeder Steg den ganzen Weg:

```
Punkt                Pfad-Z   Gelaende   alte Pruefung
Handelsdistrikt       104.9      104.5   ok
Kanalbruecke           98.0       60.0   VERWIRFT DEN GANZEN PFAD
Parkviertel            90.0       89.6   ok
Hafenrampe             40.0       12.0   VERWIRFT DEN GANZEN PFAD
Steg am Wasser          5.6      -56.5   VERWIRFT DEN GANZEN PFAD
```

Im Code stand dazu ausdrücklich, an dieser Stelle **keine**
Mehrflächen-Erkennung zu verwenden. Genau die wird aber gebraucht, um eine
Brücke von einem Fehlgriff zu unterscheiden — eine Brücke *ist* eine gültige
Fläche über dem Gelände.

## Behoben

* Geprüft wird, ob **irgendeine** Fläche an dieser Stelle zur Pfadhöhe passt
  (`FindGroundPlanes`) — Brücke, Steg, Rampe oder Gelände.
* Ein einzelner unpassender Punkt kippt die Route nicht mehr. Verworfen wird
  erst, wenn **ein Viertel** der Punkte nirgends aufliegt.

```
Weg zum Hafen:     0 von 5 Punkten ohne Flaeche -> akzeptiert
Kaputter Pfad:     3 von 5 Punkten ohne Flaeche -> VERWORFEN
```

Die Schutzwirkung bleibt also erhalten, ohne legitime Bauwerke auszuschließen.

## Was noch offen ist

Die geglättete Variante meldete `NOPATH` mit 2 Punkten — dort findet Detour
kein Zielpolygon. Das ist eine andere Baustelle als die Verwerfung: Bei 687
Yards greift ohnehin die 296-Yard-Grenze der Glättung, und die Eckpunkt-Variante
übernimmt. Sie liefert `INCOMPLETE`, also einen Teilweg — der reicht, weil
AutoTravel am Ende jedes Teilstücks neu rechnet.
