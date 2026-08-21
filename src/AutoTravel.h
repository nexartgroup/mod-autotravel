#ifndef MOD_AUTOTRAVEL_H
#define MOD_AUTOTRAVEL_H

#include "Common.h"
#include "ObjectGuid.h"
#include "MoveSplineInitArgs.h"

#include <string>
#include <cstddef>
#include <unordered_map>
#include <vector>

class Player;

enum ATState : uint8
{
    AT_IDLE = 0,
    AT_CALCULATE_PATH,
    AT_TRAVELING,
    AT_COMBAT_PAUSED,
    AT_MOUNTING,
    AT_WAIT_FLIGHT,
    AT_PLAYER_PAUSED,
    AT_ARRIVED,
    AT_FAILED
};

enum ATControlOwner : uint8
{
    AT_OWNER_TRAVEL = 0,   // AutoTravel bewegt den Charakter
    AT_OWNER_PLAYER = 1,   // der Mensch hat die Steuerung
};

char const* ATOwnerName(ATControlOwner o);

enum ATLegFlags : uint8
{
    AT_LEG_NORMAL  = 0,
    AT_LEG_FLIGHT  = 1,    // Carbonite-Abschnitt "F": Flugmeister
    AT_LEG_SPECIAL = 2,    // TravelNode-Verbindung, die nicht gelaufen wird
};

// Reiseknoten aus der Playerbot-Datenbank (playerbots_travelnode).
// Die Koordinaten sind bereits Weltkoordinaten - keine Umrechnung noetig.
struct ATNode
{
    uint32 id = 0;
    uint32 mapId = 0;
    float  x = 0.0f, y = 0.0f, z = 0.0f;
    std::string name;
};

// Rohdaten der Kante. Die Kosten entstehen erst bei der Suche, weil sie vom
// gewaehlten Routenprofil und von Laufzeitaufschlaegen abhaengen.
struct ATNodeLink
{
    uint32 to = 0;
    uint8  type = 0;
    float  distance = 0.0f;
    float  extra = 0.0f;
    float  swim = 0.0f;
};

// Routenprofil: bestimmt, wie stark Korridore bevorzugt und Sonderwege
// bestraft werden. Kein "ROADS", weil es in 3.3.5a keine Strassendaten gibt --
// bevorzugt wird der Korridor des Reisegraphen.
enum ATRouteProfile : uint8
{
    AT_PROFILE_KORRIDOR = 0,   // Standard: Korridor bevorzugen, Abkuerzung erlaubt
    AT_PROFILE_KURZ     = 1,   // moeglichst kurz, Korridor egal
    AT_PROFILE_SCHNELL  = 2,   // Flug und Portal bevorzugt
    AT_PROFILE_SICHER   = 3,   // Korridor stark bevorzugt, Wasser meiden
    AT_PROFILE_ZU_FUSS  = 4,   // keine Sonderverbindungen
};

char const* ATProfileName(ATRouteProfile p);
bool ParseProfile(std::string const& in, uint32& out);

struct ATProfileWeights
{
    float walk;        // Faktor auf Laufkanten
    float special;     // Faktor auf Flug/Portal/Transport
    float specialAdd;  // fester Aufschlag je Sonderverbindung
    float swim;        // Faktor auf Schwimmanteile
    float offroad;     // ab welchem Vielfachen der Luftlinie der Korridor
                       // zugunsten des Direktwegs aufgegeben wird
};

ATProfileWeights const& ATWeights(ATRouteProfile p);

// Ein Stuetzpunkt der Carbonite-Route. Zwischen zwei Stuetzpunkten sucht das
// NavMesh den tatsaechlichen Weg -- Carbonite gibt nur die grobe Abfolge vor
// (Zonenuebergang, Torbogen, Bruecke, Flugpunkt, Ziel).
struct ATLeg
{
    uint32 uiMapId = 0;
    float  nx = 0.0f, ny = 0.0f;
    uint8  flags = AT_LEG_NORMAL;

    float  wx = 0.0f, wy = 0.0f, wz = 0.0f;
    bool   resolved = false;
    std::string name;

    uint32 nodeId   = 0;       // Knoten im Reisegraphen, 0 = kein Knoten
    uint8  linkType = 0;       // Art der Verbindung zum NAECHSTEN Punkt
    std::string nextName;      // Name des naechsten Punktes
};

char const* ATStateName(ATState s);
char const* LinkTypeNameFor(uint8 t);

struct ATConfig
{
    bool  enable            = true;
    float arrivalDistance   = 8.0f;
    float legDistance       = 15.0f;   // Radius fuer Zwischenstuetzpunkte
    uint32 finalApproachTries = 2;     // Anlaeufe auf den exakten Zielpunkt
    float heuristicWeight   = 1.0f;    // A*: 1.0 = kuerzester Weg garantiert
    uint32 routeProfile     = 0;       // Standardprofil
    uint32 routeCacheSec    = 120;     // Haltbarkeit zwischengespeicherter Routen
    float stuckPenalty      = 300.0f;  // Aufschlag je Stuck auf einer Kante
    float penaltyDecaySec   = 900.0f;  // Halbwertszeit dieses Aufschlags
    float simplifyTolerance = 0.7f;    // Douglas-Peucker, Yards
    bool  autoMount         = true;
    float mountMinDistance  = 150.0f;
    bool  pauseInCombat     = true;
    bool  resumeAfterCombat = true;
    uint32 combatGraceMs    = 1500;
    bool  stuckDetection    = true;
    uint32 stuckTimeoutMs   = 5000;
    float stuckMinDistance  = 3.0f;
    uint32 maxRepathAttempts = 8;
    bool  resumeAfterDeath  = false;
    bool  takeClientControl = true;
    uint32 chunkPoints      = 12;
    bool  swim              = true;
    float swimSurfaceOffset = 1.2f;    // wie tief unter der Oberflaeche
    float minSwimDepth      = 2.0f;    // darunter ist es eine Pfuetze
    uint32 maxUnderwaterMs  = 45000;   // Notbremse, falls doch getaucht wird
    bool  rescueUnderMesh   = true;
    float underMeshDepth    = 2.5f;    // ab so viel unter dem Boden gilt es als durchgefallen
    float aboveMeshHeight   = 12.0f;    // ab so viel darueber gilt er als haengend/fallend
    bool  useTravelNodes    = true;
    std::string nodeDb      = "acore_playerbots";
    float nodeSearchRadius  = 800.0f;
    float nodeMinDistance   = 300.0f;
    float skipDetourFactor  = 1.25f;   // ab diesem Umwegfaktor Startknoten ueberspringen
    bool  useSpecialLinks   = true;
    float specialLinkCost   = 400.0f;
    bool  allowTeleport     = true;
    uint32 teleportSecurity = 2;       // 0=Spieler 1=Moderator 2=GM 3=Admin
    float teleportMinDist   = 0.0f;
    uint32 teleportCooldown = 5;
    bool  debug             = false;
};

extern ATConfig ATConf;

struct ATSession
{
    ATState state       = AT_IDLE;

    uint32  mapId       = 0;

    // Angefordertes Ziel: bleibt unveraendert und entscheidet ueber ANKUNFT.
    float   reqX        = 0.0f;
    float   reqY        = 0.0f;
    float   reqZ        = 0.0f;

    // Punkt, den die Wegfindung tatsaechlich anlaeuft. Weicht ab, wenn die
    // gewuenschte Stelle nicht begehbar ist. Darf NIE die Ankunft definieren.
    float   destX       = 0.0f;
    float   destY       = 0.0f;
    float   destZ       = 0.0f;
    float   approachOff = 0.0f;    // Abstand approach <-> requested
    uint8   finalTries  = 0;       // Versuche fuer den Schlussanflug
    std::string destName;

    std::vector<ATLeg> route;
    size_t  legIdx      = 0;
    bool    wasInFlight = false;

    Movement::PointsArray path;
    size_t  idx         = 0;
    bool    pathIncomplete = false;
    uint32  lastPathType   = 0;

    bool    controlTaken   = false;
    bool    debug          = false;

    // arrival tolerance override (0 = use global)
    float   arrivalOverride = 0.0f;
    uint32  graceOverride   = 0;      // ms, 0 = Konfigurationswert
    int8    controlOverride = -1;     // -1 = Konfiguration, 0 = aus, 1 = an
    ATControlOwner owner    = AT_OWNER_TRAVEL;
    ATRouteProfile profile  = AT_PROFILE_KORRIDOR;
    uint32  lastNodeA       = 0;      // zuletzt benutzte Kante, fuer Stuck-Aufschlag
    uint32  lastNodeB       = 0;
    bool    playerPaused    = false;  // ausdruecklich angefordert (.at pause 1)

    uint32  underwaterTimer = 0;
    bool    swimming        = false;
    uint8   offMeshHits     = 0;
    float   lastOffMeshZ    = 0.0f;
    uint32  rescueCount     = 0;

    // stuck tracking
    float   lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f;
    uint32  stuckTimer = 0;

    // misc timers
    uint32  repathAttempts = 0;
    uint32  combatTimer    = 0;
    uint32  statusTimer    = 0;
    uint32  mountTimer     = 0;
    bool    mountTried     = false;
};

class AutoTravelMgr
{
public:
    static AutoTravelMgr* instance();

    void LoadConfig();
    void LoadMapAreas();
    void LoadTravelNodes();

    // Route ueber den Playerbot-Knotengraphen. false = kein Weg gefunden.
    bool BuildNodeRoute(Player* player, float dx, float dy, float dz,
                        std::vector<ATLeg>& out, std::string& note) const;
    void NodeInfo(Player* player);

    // Laufzeitaufschlag fuer eine Kante, auf der es haengengeblieben ist.
    // Bewusst NICHT in die Datenbank: er verfaellt von selbst.
    void PenalizeEdge(uint32 from, uint32 to);
    void ClearPenalties();
    size_t NodeCount() const;
    void Update(uint32 diff);

    bool Start(Player* player, uint32 uiMapId, float nx, float ny,
               bool hasCalib, float pnx, float pny, std::string const& name);

    // Routenaufbau: erst RouteAdd (ggf. mehrfach), dann RouteStart.
    void RouteAdd(Player* player, bool clearFirst, std::string const& packed);
    bool RouteStart(Player* player, std::string const& name);
    void Teleport(Player* player, uint32 uiMapId, float nx, float ny,
                  bool hasCalib, float pnx, float pny, std::string const& name);
    void Resolve(Player* player, uint32 uiMapId, float nx, float ny,
                 bool hasCalib, float pnx, float pny);

    // Lernt die Zuordnung Client-Karten-ID -> WorldMapArea-ID anhand der
    // eigenen Position. Funktioniert auch, wenn das Ziel woanders liegt.
    void LearnMapId(Player* player, uint32 clientMapId, float pnx, float pny);

    // Ausfuehrliche Diagnose zu einem Ziel, ohne loszulaufen.
    void Diagnose(Player* player, uint32 uiMapId, float nx, float ny,
                  bool hasCalib, float pnx, float pny);
    void Stop(Player* player, std::string const& reason, bool silent = false);
    void Repath(Player* player);
    void PrintStatus(Player* player);
    void SetOption(Player* player, std::string const& key, std::string const& value);
    void SetPlayerPause(Player* player, bool on);
    void SetDebug(Player* player, bool on);

    bool IsActive(Player* player) const;

private:
    void UpdateSession(Player* player, ATSession& s, uint32 diff);

    bool CalculatePath(Player* player, ATSession& s);
    bool BeginTravel(Player* player, ATSession& s);
    void ApplyNodeRouting(Player* player, ATSession& s);
    bool SetLegTarget(Player* player, ATSession& s);
    bool AdvanceLeg(Player* player, ATSession& s);
    bool TryPath(Player* player, float x, float y, float z, bool straight,
                 Movement::PointsArray& out, uint32& typeOut, bool& incomplete) const;

    // Beste plausible Oberflaeche an x/y -- beruecksichtigt auch Gebaeude,
    // Bruecken und Stadtboeden, nicht nur das Rohgelaende.
    float BestGroundZ(Player* player, float x, float y) const;

    // Wasseroberflaeche an x/y. Rueckgabe false = kein nennenswertes Wasser.
    bool  WaterSurface(Player* player, float x, float y, float probeZ, float& level) const;

    // Hoehe, auf der sich der Charakter dort bewegen soll: Boden, oder knapp
    // unter der Wasseroberflaeche, wenn dort geschwommen wird.
    float TravelZ(Player* player, float x, float y, float groundZ) const;
    void LaunchChunk(Player* player, ATSession& s);
    void HaltMovement(Player* player, ATSession& s);
    void ReleaseControl(Player* player, ATSession& s);

    bool TryMount(Player* player, ATSession& s);
    uint32 PickGroundMount(Player* player) const;

    bool MapToWorld(Player* player, uint32 uiMapId, float nx, float ny,
                    bool hasCalib, float pnx, float pny,
                    float& outX, float& outY, std::string& err) const;

    bool ResolveWorld(Player* player, uint32 uiMapId, float nx, float ny,
                      bool hasCalib, float pnx, float pny,
                      float& x, float& y, float& z, uint32& mapId,
                      std::string& err) const;

    void PushStatus(Player* player, ATSession const& s);
    void Msg(Player* player, std::string const& text) const;
    void Dbg(Player* player, ATSession const& s, std::string const& text) const;

    float ArrivalDist(ATSession const& s) const
    {
        return s.arrivalOverride > 0.0f ? s.arrivalOverride : ATConf.arrivalDistance;
    }

    std::unordered_map<ObjectGuid, ATSession> _sessions;
    std::unordered_map<ObjectGuid, uint32> _tpCooldown;   // Unix-Zeit
    std::unordered_map<ObjectGuid, std::vector<ATLeg>> _pendingRoutes;
    float _lastRouteCost = 0.0f;
    uint32 _tick = 0;
};

#define sAutoTravel AutoTravelMgr::instance()

#endif // MOD_AUTOTRAVEL_H
