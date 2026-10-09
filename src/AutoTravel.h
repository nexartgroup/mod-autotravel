/*
 * mod-autotravel  --  AzerothCore 3.3.5a
 * ---------------------------------------------------------------------------
 * Serverseitige Wegfindung und Fortbewegung fuer das Client-Addon AutoTravel.
 *
 * Aufgabenteilung:
 *
 *   Carbonite (Client)  ->  wohin       (Zielpunkt und grobe Stuetzpunkte)
 *   Addon               ->  auslesen, umrechnen, Bedienung, Ruhe-Erkennung
 *   dieses Modul        ->  wie         (NavMesh, Gelaende, Spline, Taxi,
 *                                        Flugmount, Transporte, Uebergabe)
 *
 * Warum serverseitig: ein 3.3.5a-Addon kann den Charakter nicht bewegen --
 * saemtliche Bewegungsfunktionen sind protected. Es kennt ausserdem weder
 * Gelaendehoehen noch Kollisionsgeometrie noch das NavMesh.
 *
 * Uebergabemodell:
 *   Der Autopilot haelt die Clientkontrolle nur, solange er faehrt. Ein Klick
 *   auf den Pausenknopf, ein Kampf oder jeder harte Zustandswechsel gibt sie
 *   sofort zurueck. Nach einer einstellbaren Ruhezeit ohne Eingabe meldet das
 *   Addon "bereit" und der Autopilot setzt die Reise fort.
 * ---------------------------------------------------------------------------
 */

#ifndef MOD_AUTOTRAVEL_H
#define MOD_AUTOTRAVEL_H

#include "Common.h"
#include "ObjectGuid.h"
#include "MoveSplineInitArgs.h"

#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
class Map;

// ---------------------------------------------------------------------------
// Protokoll zum Addon
// ---------------------------------------------------------------------------
//
// Das Addon fragt beim Login mit ".at hello" an und bekommt
//
//   [AT]H|<modulversion>|<aktiv>|<knoten>|<flug>|<afk>|<protokoll>|<rechte>|<faehigkeiten>
//
// Die ersten fuenf Felder stammen aus aelteren Fassungen und behalten Stelle und
// Bedeutung. <protokoll> zaehlt hoch, wenn sich Nachrichten unvertraeglich
// aendern; ein Addon mit kleinerer Nummer als der Server (oder umgekehrt) soll
// das melden, statt Nachrichten falsch zu lesen.

constexpr uint32 AT_PROTOCOL_VERSION = 4;
constexpr char const* AT_MODULE_VERSION = "4.0";

// Was dieser Server fuer genau diesen Spieler anbietet. Das Addon blendet damit
// Bedienelemente aus, die ohnehin abgewiesen wuerden.
enum ATCapability : uint32
{
    AT_CAP_HANDOVER  = 1,    // .at pause / .at resume
    AT_CAP_ROUTE     = 2,    // .at route / .at rstart
    AT_CAP_TAXI      = 4,    // Flugmeister werden selbstaendig benutzt
    AT_CAP_TELEPORT  = 8,    // .at tp ist fuer diesen Spieler erlaubt
    AT_CAP_SETTINGS  = 16,   // serverweite Einstellungen mit .at set aenderbar
    AT_CAP_TRANSPORT = 32    // Zeppelin, Schiff, Bahn werden begleitet
};

// Klassen der Befehlsbremse (siehe AutoTravelMgr::AllowCommand)
enum ATCmdClass : uint8
{
    AT_CMD_HEAVY = 0,     // loest Wegfindung aus
    AT_CMD_INFO  = 1      // Auskunft, schickt mehrere Zeilen zurueck
};

constexpr uint32 AT_CMD_MAX_WEIGHT = 10;   // groesster Faktor, den ein Befehl verlangen darf

// ---------------------------------------------------------------------------
// Zustaende
// ---------------------------------------------------------------------------

enum ATState : uint8
{
    AT_IDLE = 0,          // keine Reise
    AT_CALCULATE_PATH,    // naechstes Teilstueck wird gesucht
    AT_TRAVELING,         // Spline laeuft, Server steuert
    AT_COMBAT_PAUSED,     // Kampf: Kontrolle liegt beim Spieler
    AT_PLAYER_CONTROL,    // Spieler hat bewusst uebernommen (Pausenknopf)
    AT_MOUNTING,          // Reittier wird gerufen
    AT_TAKEOFF,           // Flugmount: Steigflug
    AT_WAIT_TAXI,         // Taxiflug laeuft oder wird erwartet
    AT_WAIT_TRANSPORT,    // Zeppelin / Schiff / Bahn
    AT_WAIT_MANUAL,       // Sonderverbindung, die der Spieler selbst nimmt
    AT_ARRIVED,
    AT_FAILED
};

char const* ATStateName(ATState s);

// ---------------------------------------------------------------------------
// Etappen
// ---------------------------------------------------------------------------
//
// Eine Route besteht aus Etappen. Zwischen zwei Etappen sucht das NavMesh den
// echten Weg. Etappen koennen aus drei Quellen stammen:
//
//   * Carbonite            (Kartenkoordinaten, muessen aufgeloest werden)
//   * Playerbot-Knoten     (bereits Weltkoordinaten)
//   * Taxi-/Transportplan  (Weltkoordinaten aus DBC)
//

enum ATLegKind : uint8
{
    AT_LEG_WALK = 0,      // hinlaufen
    AT_LEG_TAXI,          // ab hier per Flugmeister weiter
    AT_LEG_TRANSPORT,     // ab hier per Zeppelin / Schiff / Bahn weiter
    AT_LEG_PORTAL,        // ab hier per Portal weiter
    AT_LEG_MANUAL         // Sonderverbindung ohne Automatik
};

// "Karte nicht bekannt". NICHT 0: die Karte 0 sind die Oestlichen Koenigreiche
// (Sturmwind, Eisenschmiede ...). Mit 0 als Platzhalter galt jedes Ziel dort als
// "gleiche Karte wie der Spieler" -- ein Charakter in Kalimdor lief dann zu den
// Koordinaten von Sturmwind, die dort irgendwo im Sueden liegen.
constexpr uint32 AT_NO_MAP = 0xFFFFFFFFu;

struct ATLeg
{
    // Quelle A: Kartenkoordinaten vom Addon
    uint32 uiMapId = 0;
    float  nx = 0.0f, ny = 0.0f;

    // Quelle B: fertige Weltkoordinaten
    uint32 mapId = AT_NO_MAP;
    float  wx = 0.0f, wy = 0.0f, wz = 0.0f;
    bool   resolved = false;

    // Das Ziel liegt auf einer anderen Karte als der Spieler: x/y sind bekannt,
    // die Hoehe nicht (das Gelaende dort ist fuer diesen Spieler nicht geladen).
    // Sie wird nachgetragen, sobald die Etappe an der Reihe ist.
    bool   groundPending = false;

    ATLegKind kind = AT_LEG_WALK;

    // Nur fuer AT_LEG_TAXI belegt
    uint32 taxiFrom = 0;
    uint32 taxiTo   = 0;
    uint32 taxiCost = 0;

    std::string name;
    std::string nextName;
};

char const* ATLegKindName(ATLegKind k);

// ---------------------------------------------------------------------------
// Knotengraph aus mod-playerbots
// ---------------------------------------------------------------------------

struct ATNode
{
    uint32 id = 0;
    uint32 mapId = 0;
    float  x = 0.0f, y = 0.0f, z = 0.0f;
    std::string name;
};

struct ATNodeLink
{
    uint32 to = 0;
    uint8  type = 0;      // 1 = zu Fuss, alles andere = Sonderverbindung

    // Kosten aus der Datenbank (Strecke + Zusatzkosten). Der Aufschlag fuer
    // Sonderverbindungen wird erst bei der Suche addiert, damit sich
    // SpecialLinkCost und UseSpecialLinks ohne erneutes Laden aus der
    // Datenbank aendern lassen.
    float  baseCost = 1.0f;
};

char const* ATLinkTypeName(uint8 t);

// ---------------------------------------------------------------------------
// Konfiguration
// ---------------------------------------------------------------------------
//
// Jeder Wert ist zusaetzlich zur Laufzeit ueber ".at set <schluessel> <wert>"
// erreichbar. Die Zuordnung Schluessel -> Feld steht in AutoTravel_Config.cpp
// in genau einer Tabelle, damit Addon und Server nicht auseinanderlaufen.
//

// ATConfig enthaelt bewusst NUR skalare Felder. Die Optionsregistry in
// AutoTravel_Config.cpp adressiert sie ueber offsetof, und das ist nur fuer
// Strukturen mit Standardlayout zulaessig. Ein std::string darin wuerde das
// Layout brechen -- deshalb steht der Datenbankname als eigene Variable
// darunter.
struct ATConfig
{
    // --- Grundbetrieb ------------------------------------------------------
    bool   enable             = true;
    bool   debug              = false;
    float  arrivalDistance    = 8.0f;    // Zielradius der letzten Etappe
    float  legDistance        = 15.0f;   // Radius der Zwischenetappen
    uint32 chunkPoints        = 12;      // NavMesh-Punkte je Spline-Abschnitt
    float  terrainStep        = 3.0f;    // Abtastung entlang eines Abschnitts
    uint32 updateIntervalMs   = 200;     // Taktweite des Moduls

    // --- Schutz des Weltservers --------------------------------------------
    // Mindestabstand zwischen zwei aufwaendigen Befehlen EINES Spielers
    // (start, rstart, tp, resolve, diag, repath, nodes, taxi). Jeder davon
    // loest Wegfindung auf dem Weltserver-Thread aus.
    uint32 commandCooldownMs  = 400;
    // Hoechstzahl neuer Wegberechnungen je Takt, serverweit. Ueberzaehlige
    // Sitzungen warten einen Takt; wer am laengsten wartet, kommt zuerst dran.
    // Verhindert, dass viele gleichzeitig startende Reisen den Weltserver fuer
    // Sekunden anhalten.
    uint32 maxPathsPerTick    = 3;
    // Zeitbudget je Takt fuer Wegberechnungen (ms). Eine einzelne Berechnung
    // laeuft immer; weitere nur, solange die bisherigen zusammen unter dem Budget
    // blieben. Das Zaehlbudget oben zaehlt Aufrufe, dieses die Kosten: eine
    // Wegsuche ohne Ergebnis kostet ein Vielfaches einer erfolgreichen.
    uint32 pathBudgetMs       = 40;

    // --- Uebergabe Spieler / Autopilot -------------------------------------
    bool   takeClientControl  = true;
    bool   pauseInCombat      = true;
    bool   resumeAfterCombat  = true;
    uint32 combatGraceMs      = 2000;
    bool   resumeAfterDeath   = false;
    // Sicherheitsnetz: meldet sich das Addon nach einer Pause gar nicht mehr
    // (Absturz, Reload, /console reloadui), endet die Reise nach dieser Zeit.
    uint32 handoverTimeoutMs  = 900000;  // 15 Minuten
    // Solange der Autopilot faehrt, wird das AFK-Kennzeichen geloescht. Der
    // Charakter legt ja Strecke zurueck -- er ist nicht abwesend.
    bool   suppressAfk        = true;

    // --- Feststecken -------------------------------------------------------
    bool   stuckDetection     = true;
    uint32 stuckTimeoutMs     = 5000;
    float  stuckMinDistance   = 3.0f;
    uint32 maxRepathAttempts  = 8;

    // --- Reittiere ---------------------------------------------------------
    bool   autoMount          = true;
    float  mountMinDistance   = 150.0f;
    bool   dismountIndoors    = true;

    // --- Fliegen mit eigenem Flugmount -------------------------------------
    bool   allowFlying        = true;
    float  flyMinDistance     = 400.0f;  // darunter lohnt der Steigflug nicht
    float  flyClearance       = 25.0f;   // Sicherheitsabstand ueber Gelaende
    float  flySampleStep      = 25.0f;   // Abtastung des Hoehenprofils
    float  flyMaxHeight       = 250.0f;  // maximale Hoehe ueber Grund
    float  flyDescendDistance = 60.0f;   // ab hier Sinkflug zum Ziel

    // --- Flugmeister -------------------------------------------------------
    bool   useTaxi            = true;
    float  taxiMinDistance    = 1500.0f; // erst ab dieser Restentfernung
    float  taxiMinSaving      = 0.60f;   // Flug muss so viel Strecke sparen
    float  taxiMaxWalkToNode  = 1200.0f; // Fussweg bis zum Flugmeister
    uint32 taxiMaxCostCopper  = 50000;   // 5 Gold je Flug
    // Wie nah der Charakter am Flugpunkt stehen muss, bevor das Modul den Flug
    // startet. Diese Grenze setzt das MODUL: ActivateTaxiPathTo prueft die
    // Entfernung bei einem Aufruf ohne Flugmeister-NPC nicht (im aktuellen
    // AzerothCore ist die frueher dort stehende Pruefung nur noch ein Kommentar).
    float  taxiBoardDistance  = 8.0f;

    // --- Transporte --------------------------------------------------------
    bool   useTransports      = true;
    uint32 transportWaitMs    = 300000;  // 5 Minuten warten, dann aufgeben

    // --- Schwimmen ---------------------------------------------------------
    bool   swim               = true;
    float  swimSurfaceOffset  = 1.2f;
    float  minSwimDepth       = 2.0f;
    uint32 maxUnderwaterMs    = 45000;
    float  waterPenalty       = 1.5f;    // Kosten je Yard Schwimmstrecke

    // --- Bodenkontakt und Rettung ------------------------------------------
    bool   rescueUnderMesh    = true;
    float  underMeshDepth     = 2.5f;
    float  aboveMeshHeight    = 12.0f;
    float  rescueSearchRange  = 14.0f;
    uint32 maxRescues         = 6;
    float  maxWalkSlope       = 1.20f;   // groesste Neigung einer Laufflaeche
    float  zOutlierTolerance  = 2.5f;
    float  maxStepUp          = 2.5f;    // groesste Stufe nach oben
    float  maxStepDown        = 6.0f;    // groesster Absatz nach unten
    // uint32, nicht uint8: die Optionsregistry schreibt ueber einen
    // uint32-Zeiger. Ein schmaleres Feld wuerde die Nachbarfelder mit
    // ueberschreiben.
    uint32 groundPlanes       = 4;       // wie viele Etagen gesucht werden

    // --- Knotengraph -------------------------------------------------------
    bool   useTravelNodes     = true;
    float  nodeSearchRadius   = 800.0f;
    float  nodeMinDistance    = 300.0f;
    float  skipDetourFactor   = 1.25f;
    bool   useSpecialLinks    = true;
    float  specialLinkCost    = 400.0f;

    // --- Teleport ----------------------------------------------------------
    bool   allowTeleport      = true;
    uint32 teleportSecurity   = 2;       // 0 Spieler 1 Mod 2 GM 3 Admin
    float  teleportMinDist    = 0.0f;
    uint32 teleportCooldown   = 5;

    // --- Bewertung: natuerliche Wege ---------------------------------------
    bool   naturalPathing     = true;

    float  slopeStart         = 0.15f;
    float  slopeStrong        = 0.25f;
    float  slopeExtreme       = 0.35f;
    float  slopePenalty       = 5.0f;
    float  steepSlopePenalty  = 10.0f;
    float  extremeSlopePenalty = 30.0f;

    float  elevationWindow    = 40.0f;
    float  elevationGainStart = 4.0f;
    float  elevationGainStrong = 10.0f;
    float  elevationGainExtreme = 20.0f;
    float  elevationPenalty   = 3.0f;
    float  strongElevationPenalty = 8.0f;
    float  extremeElevationPenalty = 20.0f;

    float  turnPenaltyStart   = 35.0f;
    float  turnPenaltyStrong  = 70.0f;
    float  turnPenaltyExtreme = 110.0f;
    float  turnPenalty        = 2.0f;
    float  strongTurnPenalty  = 5.0f;
    float  extremeTurnPenalty = 12.0f;

    float  incompletePathPenalty = 10000.0f;
    float  cliffPenalty       = 400.0f;  // je erkanntem Absturz im Pfad

    // --- Contour-Suche -----------------------------------------------------
    bool   contourProbing     = true;
    float  contourTriggerElevation = 15.0f;
    float  contourTriggerSlope     = 0.20f;
    float  contourNarrowOffset     = 100.0f;
    float  contourWideOffset       = 180.0f;
    float  contourFirstProgress    = 0.35f;
    float  contourSecondProgress   = 0.65f;
    float  contourMaxDistanceFactor = 2.5f;
};

extern ATConfig ATConf;

// Name der Datenbank, in der mod-playerbots seine Reiseknoten haelt.
extern std::string ATNodeDb;

// ---------------------------------------------------------------------------
// Sitzung
// ---------------------------------------------------------------------------

struct ATSession
{
    ATState state = AT_IDLE;

    // Reiseziel (Endpunkt der Route)
    uint32  finalMapId = AT_NO_MAP;
    float   finalX = 0.0f, finalY = 0.0f, finalZ = 0.0f;
    std::string destName;

    // Aktive Etappe
    std::vector<ATLeg> route;
    size_t  legIdx = 0;
    uint32  mapId  = 0;                 // Karte, auf der die Etappe laeuft
    float   destX = 0.0f, destY = 0.0f, destZ = 0.0f;

    // Aktuelles Teilstueck
    Movement::PointsArray path;
    size_t  idx = 0;
    bool    pathIncomplete = false;
    uint32  lastPathType = 0;

    // Bewegung
    bool    controlTaken = false;
    bool    swimming = false;
    bool    flying = false;             // Luftroute mit eigenem Flugmount
    uint32  underwaterTimer = 0;

    // Uebergabe
    bool    pausedByPlayer = false;
    uint32  handoverTimer = 0;
    uint32  combatTimer = 0;

    // Bodenkontakt
    float   lastGoodZ = 0.0f;
    bool    hasGoodZ = false;
    uint8   offMeshHits = 0;
    uint32  rescueCount = 0;

    // Feststecken
    float   lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f;
    uint32  stuckTimer = 0;
    uint32  repathAttempts = 0;         // aufeinanderfolgende Fehlschlaege der Wegsuche
    // Aufeinanderfolgende Messfenster ohne Fortschritt. Bewusst EIGENER Zaehler:
    // repathAttempts wird nach jeder erfolgreichen Wegsuche auf 0 gesetzt, und
    // eine Wegsuche gelingt auch dann, wenn der Charakter an derselben Stelle
    // festhaengt. Mit einem gemeinsamen Zaehler wuerde das Aufgeben nie eintreten.
    uint32  stuckStrikes = 0;

    // Wegberechnung: wartet die Sitzung auf ihr Budget? Je Takt vergibt Update()
    // die Berechnungen zuerst an die, die am laengsten gewartet haben. Ohne diese
    // Alterung koennten wenige Sitzungen mit unerreichbarem Ziel das Budget
    // jedes Takts aufbrauchen und alle anderen dauerhaft aussperren.
    uint32  pathWaitTicks = 0;
    bool    pathGranted = false;

    // Reittier
    uint32  mountTimer = 0;
    bool    mountTried = false;

    // Taxi / Transport
    bool    wasInFlight = false;
    uint32  waitTimer = 0;
    ObjectGuid transportGuid;

    // Anzeige
    uint32  statusTimer = 0;
    float   startDistance = 0.0f;
    uint32  startMapId = AT_NO_MAP;     // Karte beim Start; ungleich finalMapId = Kartenwechsel

    // Sitzungsbezogene Uebersteuerungen
    float   arrivalOverride = 0.0f;
    uint32  graceOverride = 0;
    bool    debug = false;

    bool IsDriving() const
    {
        return state == AT_CALCULATE_PATH || state == AT_TRAVELING
            || state == AT_MOUNTING || state == AT_TAKEOFF;
    }

    bool IsHandedOver() const
    {
        return state == AT_PLAYER_CONTROL || state == AT_COMBAT_PAUSED;
    }
};

// ---------------------------------------------------------------------------
// Ergebnis einer Pfadsuche
// ---------------------------------------------------------------------------

struct ATPathResult
{
    Movement::PointsArray points;
    uint32 type = 0;
    bool   incomplete = false;
    float  score = 0.0f;
    bool   valid = false;
};

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

class AutoTravelMgr
{
public:
    static AutoTravelMgr* instance();

    // --- Start / Laden -----------------------------------------------------
    void LoadConfig();
    void LoadMapAreas();
    void LoadTravelNodes();

    // --- Takt --------------------------------------------------------------
    void Update(uint32 diff);

    // --- Befehle vom Addon -------------------------------------------------
    bool Start(Player* player, uint32 uiMapId, float nx, float ny,
               bool hasCalib, float pnx, float pny, std::string const& name);
    void RouteAdd(Player* player, bool clearFirst, std::string const& packed);
    bool RouteStart(Player* player, std::string const& name);

    void Stop(Player* player, std::string const& reason, bool silent = false);
    void Repath(Player* player);

    // Uebergabe
    void PauseByPlayer(Player* player, std::string const& why);
    void ResumeByPlayer(Player* player);

    void Teleport(Player* player, uint32 uiMapId, float nx, float ny,
                  bool hasCalib, float pnx, float pny, std::string const& name);
    void Resolve(Player* player, uint32 uiMapId, float nx, float ny,
                 bool hasCalib, float pnx, float pny);
    void Diagnose(Player* player, uint32 uiMapId, float nx, float ny,
                  bool hasCalib, float pnx, float pny);

    void LearnMapId(Player* player, uint32 clientMapId, float pnx, float pny);

    void PrintStatus(Player* player);
    void SendHello(Player* player);
    void SetDebug(Player* player, bool on);
    bool SetOption(Player* player, std::string const& key, std::string const& value);
    void ListOptions(Player* player, std::string const& filter);

    void NodeInfo(Player* player);
    void TaxiInfo(Player* player);
    void PrintStats(Player* player);      // Gesundheitsbericht fuer Spielleiter
    size_t NodeCount() const;

    bool IsActive(Player* player) const;

    // Rate-Limit fuer Befehle. Liefert false (und meldet es dem Spieler), wenn der
    // Befehl zu dicht auf den vorigen DERSELBEN Klasse folgt. Die Klassen haben je
    // einen eigenen Zeitgeber, damit ein Auskunftsbefehl nicht eine Reise
    // blockiert; 'weight' vervielfacht den Mindestabstand fuer besonders teure
    // Befehle (".at diag" rechnet bis zu zwoelf Wege).
    bool AllowCommand(Player* player, ATCmdClass cls = AT_CMD_HEAVY, uint32 weight = 1);

    // Schickt dem Addon den tatsaechlichen Zustand: die laufende Reise oder IDLE.
    // Gebraucht nach einem abgewiesenen Start. Das Addon wechselt beim Absenden
    // in "Startet" und wartet auf eine Statuszeile; ohne diese bliebe es dort
    // haengen, weil eine Absage nur als Text kommt.
    void SyncStatus(Player* player);

    // Rechte des Spielers als Faehigkeitsmaske fuer die Hello-Antwort.
    uint32 CapabilitiesFor(Player* player) const;

    // --- Aufraeumen --------------------------------------------------------
    // Beendet alle Reisen und gibt die Clientkontrolle zurueck. Wird gebraucht,
    // wenn das Modul zur Laufzeit abgeschaltet wird: ohne das bliebe jeder
    // fahrende Spieler ohne Steuerung zurueck.
    void AbortAllSessions(std::string const& why);

    // --- Knotengraph (in AutoTravel_Nodes.cpp) -----------------------------
    bool BuildNodeRoute(Player* player, uint32 destMap, float dx, float dy, float dz,
                        std::vector<ATLeg>& out, std::string& note) const;

    // --- Taxi (in AutoTravel_Taxi.cpp) -------------------------------------
    bool BuildTaxiPlan(Player* player, uint32 destMap, float dx, float dy, float dz,
                       std::vector<ATLeg>& out, std::string& note) const;
    bool StartTaxi(Player* player, ATSession& s, ATLeg const& leg);

    // Beantwortet VOR der Reise, ob dieser Charakter zwischen zwei Orten
    // wirklich fliegen koennte: Flugpunkt bekannt, Fraktion passt, Verbindung
    // vorhanden, Preis unter der Grenze, Geld reicht. Liefert dazu die
    // Positionen der beiden Flugmeister, damit die Etappe genau dort landet --
    // der Core laesst den Abflug nur aus naechster Naehe zu.
    bool ResolveTaxiHop(Player* player,
                        uint32 mapA, float ax, float ay,
                        uint32 mapB, float bx, float by,
                        uint32& fromNode, uint32& toNode, uint32& cost,
                        uint32& boardMap, float& boardX, float& boardY, float& boardZ,
                        uint32& landMap, float& landX, float& landY, float& landZ) const;

    void TaxiStats(Player* player, uint32& total, uint32& known, uint32& usable) const;

private:
    // --- Sitzungsablauf (AutoTravel_Session.cpp) ---------------------------
    void UpdateSession(Player* player, ATSession& s, uint32 diff);
    bool BeginTravel(Player* player, ATSession& s);
    bool SetLegTarget(Player* player, ATSession& s);
    bool AdvanceLeg(Player* player, ATSession& s);
    bool ApplyPlannedRoute(Player* player, ATSession& s, std::string& err);
    void RemainingForStatus(Player* player, ATSession const& s, float& dist, uint32& progress) const;
    void Finish(Player* player, ATSession& s, std::string const& text, bool ok);

    bool CheckHandover(Player* player, ATSession& s, uint32 diff);
    bool CheckGroundContact(Player* player, ATSession& s);
    bool CheckTransport(Player* player, ATSession& s, uint32 diff);

    // --- Bewegung (AutoTravel_Move.cpp) ------------------------------------
    void LaunchChunk(Player* player, ATSession& s);
    void HaltMovement(Player* player, ATSession& s);
    void TakeControl(Player* player, ATSession& s);
    void ReleaseControl(Player* player, ATSession& s);
    bool TryMount(Player* player, ATSession& s);
    uint32 PickMount(Player* player, bool wantFlying) const;
    bool ShouldFly(Player* player, ATSession& s) const;
    bool BuildAirPath(Player* player, ATSession& s);

    // --- Gelaende (AutoTravel_Terrain.cpp) ---------------------------------
    float BestGroundZ(Player* player, float x, float y) const;
    void  FindGroundPlanes(Player* player, float x, float y, float probeZ,
                           std::vector<float>& out) const;
    float SelectGroundPlaneOnRoute(std::vector<float> const& planes,
                                   float expectedZ, float prevPlane,
                                   float horizontalStep) const;
    bool  WaterSurface(Player* player, float x, float y, float probeZ,
                       float& level, float& ground) const;
    float TravelZ(Player* player, float x, float y, float groundZ) const;
    float TerrainCeilingBetween(Player* player, float ax, float ay,
                                float bx, float by, float step) const;

    // --- Pfad (AutoTravel_Path.cpp) ----------------------------------------
    bool TryPathBetween(Player* player,
                        float sx, float sy, float sz,
                        float dx, float dy, float dz,
                        bool straight, ATPathResult& out) const;
    float PathDistance(Movement::PointsArray const& path) const;
    float ScoreNaturalPath(Player* player, Movement::PointsArray const& path,
                           bool incomplete) const;
    bool  HasMountainClimb(Movement::PointsArray const& path) const;
    uint32 CountCliffs(Player* player, Movement::PointsArray const& path) const;
    bool  BuildContourCandidate(Player* player, ATSession const& s,
                                float offset, bool left, ATPathResult& out) const;
    void  FixPathZOutliers(Player* player, Movement::PointsArray& path) const;
    bool  CalculatePath(Player* player, ATSession& s);

    // --- Karten (AutoTravel_Route.cpp) -------------------------------------
    // Mit 'targetMap' darf das Ziel auf einer anderen Karte als der des Spielers
    // liegen; die Karte des Ziels kommt dann dort heraus. Ohne den Zeiger ist es
    // ein Fehler, wie bisher.
    bool MapToWorld(Player* player, uint32 uiMapId, float nx, float ny,
                    bool hasCalib, float pnx, float pny,
                    float& outX, float& outY, std::string& err,
                    uint32* targetMap = nullptr) const;
    // Mit allowOtherMap liefert eine Zielkarte != Karte des Spielers Erfolg mit
    // z = 0: die Hoehe laesst sich nur auf der eigenen Karte abfragen.
    bool ResolveWorld(Player* player, uint32 uiMapId, float nx, float ny,
                      bool hasCalib, float pnx, float pny,
                      float& x, float& y, float& z, uint32& mapId,
                      std::string& err, bool allowOtherMap = false) const;

    // --- Meldungen (AutoTravel_Config.cpp) ---------------------------------
public:
    void Msg(Player* player, std::string const& text) const;
    void Dbg(Player* player, ATSession const& s, std::string const& text) const;
    void Raw(Player* player, std::string const& line) const;
private:
    void PushStatus(Player* player, ATSession& s);

    float ArrivalDist(ATSession const& s) const
    {
        return s.arrivalOverride > 0.0f ? s.arrivalOverride : ATConf.arrivalDistance;
    }

    ATSession* Find(Player* player);
    ATSession const* Find(Player* player) const;

    // Debugausgabe ist global (AutoTravel.Debug) oder je Sitzung (.at debug)
    // einschaltbar. Aufrufer mit teurer Textaufbereitung pruefen das VORHER,
    // statt Zeilen zu formatieren, die dann verworfen werden.
    static bool DebugEnabled(ATSession const& s) { return ATConf.debug || s.debug; }

    // Spieler, deren Addon sich gemeldet hat. Nur bei ihnen wartet der
    // Autopilot nach einem Kampf auf die Ruhemeldung des Clients, statt von
    // sich aus wieder zu uebernehmen -- ohne Addon gaebe es niemanden, der
    // diese Meldung schicken koennte.
    std::unordered_set<ObjectGuid> _addonPlayers;

    std::unordered_map<ObjectGuid, ATSession> _sessions;
    std::unordered_map<ObjectGuid, uint32> _tpCooldown;
    std::unordered_map<ObjectGuid, std::vector<ATLeg>> _pendingRoutes;
    // Rate-Limit: je Spieler und Befehlsklasse der Zeitpunkt des letzten Befehls
    std::unordered_map<ObjectGuid, std::array<uint32, 2>> _cmdStamp;

    // Zuordnung "Karten-ID des Clients -> WorldMapArea-ID", die der Server aus
    // der echten Spielerposition gelernt hat. Pro SPIELER gehalten: die Angaben
    // stammen vom Client, und ein gemeinsamer Speicher haette es jedem Spieler
    // erlaubt, die Aufloesung fuer alle anderen zu verstellen (und ihn mit
    // beliebigen Karten-IDs unbegrenzt wachsen zu lassen).
    struct ATCalibration
    {
        std::unordered_map<uint32, uint32> fix;
        int32 delta = 0;
        bool  deltaKnown = false;
    };
    mutable std::unordered_map<ObjectGuid, ATCalibration> _calib;
    static constexpr size_t MAX_CALIB_ENTRIES = 64;

    // Raeumt Eintraege von Spielern ab, die nicht mehr verbunden sind. Ohne
    // PlayerScript (siehe AutoTravel_SC.cpp) ist das der einzige Weg, die
    // GUID-Tabellen am Wachsen zu hindern.
    void SweepOrphans();

    uint32 _tick = 0;
    uint32 _sweepTimer = 0;
    uint32 _pathSpentUs = 0;     // bisher in diesem Takt fuer Wegberechnungen verbraucht
    uint32 _pathCalls = 0;       // Wegberechnungen in diesem Takt

    // Zaehler seit dem Start, nur fuer ".at stats"
    uint64 _statTravelsStarted = 0;
    uint64 _statTravelsArrived = 0;
    uint64 _statTravelsFailed  = 0;
    uint64 _statPathCalcs      = 0;
    uint64 _statPathDeferred   = 0;   // Takte, in denen das Budget eine Sitzung warten liess
    uint64 _statCmdThrottled   = 0;

};

#define sAutoTravel AutoTravelMgr::instance()

// ---------------------------------------------------------------------------
// Kleine Helfer, die mehrere Uebersetzungseinheiten brauchen
// ---------------------------------------------------------------------------

namespace AT
{
    float Dist2D(float ax, float ay, float bx, float by);
    std::string PathTypeName(uint32 t);

    bool ParseUInt(std::string const& in, uint32& out);
    bool ParseFloat(std::string const& in, float& out);
    bool ParseBool(std::string const& in, bool& out);
    bool ParseNorm(std::string const& in, float& out);

    // Macht Text vom Client fuer das Zeilenprotokoll unschaedlich: keine
    // Steuerzeichen, kein '|' (Feldtrenner im Protokoll und Escape-Zeichen im
    // Chat), hoechstens maxBytes Bytes, nie mitten in einem UTF-8-Zeichen
    // abgeschnitten.
    std::string SanitizeText(std::string const& in, size_t maxBytes);

    // Datenbankname fuer die Knotentabellen: nur Buchstaben, Ziffern, '_' und
    // '$', hoechstens 64 Zeichen. Der Name wird in SQL eingesetzt.
    bool IsSafeIdentifier(std::string const& in);

    // --- Grenzen der Spline-Pakete ------------------------------------------
    //
    // SMSG_MONSTER_MOVE kodiert die Zwischenpunkte eines gewoehnlichen (nicht
    // glatten) Splines als Abstand zur MITTE von erstem und letztem Punkt, in
    // 11 / 11 / 10 Bit zu je 0,25 yd: x und y erreichen +-256 yd, z +-128 yd.
    // Was darueber liegt, wird abgeschnitten, und der Client zeichnet einen
    // voellig anderen Weg -- er "fliegt" mit dem Mehrfachen des Tempos ueber die
    // Karte, weil die Dauer aus dem WAHREN Weg berechnet wurde. Der Core prueft
    // das nicht (MoveSplineInitArgs::_checkPathBounds ist auskommentiert).
    //
    // Die Grenzen hier liegen mit Absicht deutlich unter den harten Werten.
    constexpr float SPLINE_PACK_LIMIT_XY = 220.0f;
    constexpr float SPLINE_PACK_LIMIT_Z  = 100.0f;

    // Laengstes Wegsegment (horizontal, yd), das als EIN Stueck in einen Abschnitt
    // geht. Laengere werden vorher geteilt: ein Segment ueber den ganzen See
    // waere sonst nicht zerlegbar, und Wasser/Land liesse sich nicht trennen.
    constexpr float SPLINE_MAX_SEGMENT = 40.0f;

    // Passen alle Zwischenpunkte in die Paketkodierung (siehe oben)?
    bool SplineFitsPacket(Movement::PointsArray const& pts);

    // Teilt jedes Segment ab Index 'from', das horizontal laenger als maxLen ist,
    // in gleich lange Stuecke. Das erste Segment beginnt bei 'origin' (der
    // Spielerposition), die folgenden beim vorigen Punkt. Hoehen werden linear
    // zwischen den Segmentenden verteilt -- wie es LaunchChunk fuer die Sollhoehe
    // ohnehin tut, die Auswahl der Bodenflaeche aendert sich dadurch nicht.
    // Rueckgabe: true, wenn etwas geteilt wurde. 'from' bleibt gueltig.
    bool SplitLongSegments(Movement::PointsArray& path, size_t from,
                           G3D::Vector3 const& origin, float maxLen);
}

#endif // MOD_AUTOTRAVEL_H
