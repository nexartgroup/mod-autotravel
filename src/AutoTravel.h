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

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
class Map;

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

struct ATLeg
{
    // Quelle A: Kartenkoordinaten vom Addon
    uint32 uiMapId = 0;
    float  nx = 0.0f, ny = 0.0f;

    // Quelle B: fertige Weltkoordinaten
    uint32 mapId = 0;
    float  wx = 0.0f, wy = 0.0f, wz = 0.0f;
    bool   resolved = false;

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
    float  cost = 0.0f;
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
    // Der Core laesst den Abflug nur innerhalb von 2 * INTERACTION_DISTANCE
    // (10 Yards) zu. Der Wert bleibt bewusst darunter.
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
    uint32  finalMapId = 0;
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
    bool    flyChecked = false;
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
    uint32  repathAttempts = 0;

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
    size_t NodeCount() const;

    bool IsActive(Player* player) const;

    // --- Aufraeumen --------------------------------------------------------
    void OnPlayerLeave(Player* player);

    // --- Knotengraph (in AutoTravel_Nodes.cpp) -----------------------------
    bool BuildNodeRoute(Player* player, uint32 destMap, float dx, float dy, float dz,
                        std::vector<ATLeg>& out, std::string& note) const;

    // --- Taxi (in AutoTravel_Taxi.cpp) -------------------------------------
    bool BuildTaxiPlan(Player* player, uint32 destMap, float dx, float dy, float dz,
                       std::vector<ATLeg>& out, std::string& note) const;
    bool StartTaxi(Player* player, ATSession& s, ATLeg const& leg);

private:
    // --- Sitzungsablauf (AutoTravel_Session.cpp) ---------------------------
    void UpdateSession(Player* player, ATSession& s, uint32 diff);
    bool BeginTravel(Player* player, ATSession& s);
    bool SetLegTarget(Player* player, ATSession& s);
    bool AdvanceLeg(Player* player, ATSession& s);
    void ApplyPlannedRoute(Player* player, ATSession& s);
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
    bool MapToWorld(Player* player, uint32 uiMapId, float nx, float ny,
                    bool hasCalib, float pnx, float pny,
                    float& outX, float& outY, std::string& err) const;
    bool ResolveWorld(Player* player, uint32 uiMapId, float nx, float ny,
                      bool hasCalib, float pnx, float pny,
                      float& x, float& y, float& z, uint32& mapId,
                      std::string& err) const;

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

    // Spieler, deren Addon sich gemeldet hat. Nur bei ihnen wartet der
    // Autopilot nach einem Kampf auf die Ruhemeldung des Clients, statt von
    // sich aus wieder zu uebernehmen -- ohne Addon gaebe es niemanden, der
    // diese Meldung schicken koennte.
    std::unordered_set<ObjectGuid> _addonPlayers;

    std::unordered_map<ObjectGuid, ATSession> _sessions;
    std::unordered_map<ObjectGuid, uint32> _tpCooldown;
    std::unordered_map<ObjectGuid, std::vector<ATLeg>> _pendingRoutes;

    uint32 _tick = 0;

    friend struct ATOptionAccess;
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
    bool ParseInt(std::string const& in, int32& out);
    bool ParseFloat(std::string const& in, float& out);
    bool ParseBool(std::string const& in, bool& out);
    bool ParseNorm(std::string const& in, float& out);
}

#endif // MOD_AUTOTRAVEL_H
