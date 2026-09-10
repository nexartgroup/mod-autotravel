/*
 * AutoTravel_Config.cpp
 * ---------------------------------------------------------------------------
 * Konfiguration, Optionsregistry, Meldungen, Hilfsfunktionen.
 *
 * Die Optionsregistry ist der Grund, warum diese Datei ganz vorne steht: in der
 * Vorfassung kannte ".at set" genau zwei Schluessel (arrival, grace), waehrend
 * die Optionsseite des Addons ein Dutzend weitere schickte. Alle liefen ins
 * Leere und meldeten "Unbekannte Option".
 *
 * Jetzt gibt es GENAU EINE Tabelle, aus der sich sowohl das Laden aus der
 * autotravel.conf als auch ".at set" und ".at options" bedienen. Ein neuer Wert
 * wird an einer Stelle eingetragen und ist damit ueberall verfuegbar.
 */

#include "AutoTravel.h"

#include "Chat.h"
#include "Config.h"
#include "Log.h"
#include "Player.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>   // offsetof
#include <cstdio>
#include <cstdlib>
#include <cstring>

ATConfig ATConf;
std::string ATNodeDb = "acore_playerbots";

AutoTravelMgr* AutoTravelMgr::instance()
{
    static AutoTravelMgr inst;
    return &inst;
}

// ---------------------------------------------------------------------------
// Namen
// ---------------------------------------------------------------------------

char const* ATStateName(ATState s)
{
    switch (s)
    {
        case AT_IDLE:            return "IDLE";
        case AT_CALCULATE_PATH:  return "REPATHING";
        case AT_TRAVELING:       return "TRAVELING";
        case AT_COMBAT_PAUSED:   return "COMBAT";
        case AT_PLAYER_CONTROL:  return "PLAYER";
        case AT_MOUNTING:        return "MOUNTING";
        case AT_TAKEOFF:         return "TAKEOFF";
        case AT_WAIT_TAXI:       return "TAXI";
        case AT_WAIT_TRANSPORT:  return "TRANSPORT";
        case AT_WAIT_MANUAL:     return "MANUAL";
        case AT_ARRIVED:         return "ARRIVED";
        case AT_FAILED:          return "FAILED";
    }
    return "UNKNOWN";
}

char const* ATLegKindName(ATLegKind k)
{
    switch (k)
    {
        case AT_LEG_WALK:      return "Laufweg";
        case AT_LEG_TAXI:      return "Flugroute";
        case AT_LEG_TRANSPORT: return "Transport";
        case AT_LEG_PORTAL:    return "Portal";
        case AT_LEG_MANUAL:    return "Sonderverbindung";
    }
    return "Verbindung";
}

char const* ATLinkTypeName(uint8 t)
{
    switch (t)
    {
        case 1:  return "zu Fuss";
        case 2:  return "Portal";
        case 3:  return "Transport";
        case 4:  return "Flugroute";
        default: return "Sonderverbindung";
    }
}

// ---------------------------------------------------------------------------
// Kleine Helfer
// ---------------------------------------------------------------------------

namespace AT
{
    float Dist2D(float ax, float ay, float bx, float by)
    {
        float dx = ax - bx;
        float dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

    std::string PathTypeName(uint32 t)
    {
        std::string out;
        if (t & 0x01) out += "NORMAL ";
        if (t & 0x02) out += "SHORTCUT ";
        if (t & 0x04) out += "INCOMPLETE ";
        if (t & 0x08) out += "NOPATH ";
        if (t & 0x10) out += "NOT_USING_PATH ";
        if (t & 0x20) out += "SHORT ";
        if (t & 0x40) out += "FARFROMPOLY ";
        if (out.empty()) out = "BLANK";
        return out;
    }

    bool ParseUInt(std::string const& in, uint32& out)
    {
        if (in.empty())
            return false;
        char* end = nullptr;
        errno = 0;
        unsigned long v = std::strtoul(in.c_str(), &end, 10);
        if (errno == ERANGE || end == in.c_str() || *end != '\0')
            return false;
        if (v > 0xFFFFFFFFul)
            return false;
        out = uint32(v);
        return true;
    }

    bool ParseInt(std::string const& in, int32& out)
    {
        if (in.empty())
            return false;
        char* end = nullptr;
        errno = 0;
        long v = std::strtol(in.c_str(), &end, 10);
        if (errno == ERANGE || end == in.c_str() || *end != '\0')
            return false;
        out = int32(v);
        return true;
    }

    bool ParseFloat(std::string const& in, float& out)
    {
        if (in.empty())
            return false;
        char* end = nullptr;
        errno = 0;
        double v = std::strtod(in.c_str(), &end);
        if (errno == ERANGE || end == in.c_str() || *end != '\0')
            return false;
        if (!std::isfinite(v))
            return false;
        out = float(v);
        return true;
    }

    bool ParseBool(std::string const& in, bool& out)
    {
        if (in == "on" || in == "true" || in == "yes" || in == "an")  { out = true;  return true; }
        if (in == "off" || in == "false" || in == "no" || in == "aus") { out = false; return true; }
        uint32 v = 0;
        if (!ParseUInt(in, v) || v > 1)
            return false;
        out = (v != 0);
        return true;
    }

    bool ParseNorm(std::string const& in, float& out)
    {
        if (!ParseFloat(in, out))
            return false;
        return out >= 0.0f && out <= 1.0f;
    }
}

// ---------------------------------------------------------------------------
// Optionsregistry
// ---------------------------------------------------------------------------
//
// Jede Zeile beschreibt einen Wert vollstaendig:
//
//   key       kurzer Schluessel fuer ".at set" und die Optionsseite des Addons
//   confName  Name in autotravel.conf.dist
//   type      B = bool, F = float, U = uint32
//   ptr       Zeiger in ATConf
//   min/max   erlaubter Bereich (nur F und U)
//   help      einzeilige Beschreibung fuer ".at options"
//
// Der Schluessel ist bewusst kurz und klein geschrieben: das Addon schickt ihn
// woertlich, und im Chat tippt man ihn von Hand.

namespace
{
    enum ATOptType : uint8 { OPT_BOOL, OPT_FLOAT, OPT_UINT };

    struct ATOption
    {
        char const* key;
        char const* confName;
        ATOptType   type;
        size_t      offset;
        double      minV;
        double      maxV;
        double      defV;
        char const* help;
    };

    #define AT_OFF(field) offsetof(ATConfig, field)

    ATOption const sOptions[] =
    {
        // Schluessel            conf-Name                             Typ       Feld                        min      max        Standard  Hilfe
        { "enable",              "AutoTravel.Enable",                  OPT_BOOL,  AT_OFF(enable),              0,       1,         1,       "Modul aktiv" },
        { "debug",               "AutoTravel.Debug",                   OPT_BOOL,  AT_OFF(debug),               0,       1,         0,       "ausfuehrliche Serverausgabe" },
        { "arrival",             "AutoTravel.ArrivalDistance",         OPT_FLOAT, AT_OFF(arrivalDistance),     0.5,     100,       8,       "Zielradius der letzten Etappe (yd)" },
        { "leg",                 "AutoTravel.LegDistance",             OPT_FLOAT, AT_OFF(legDistance),         1,       120,       15,      "Radius der Zwischenetappen (yd)" },
        { "chunk",               "AutoTravel.ChunkPoints",             OPT_UINT,  AT_OFF(chunkPoints),         2,       60,        12,      "NavMesh-Punkte je Spline-Abschnitt" },
        { "step",                "AutoTravel.TerrainStep",             OPT_FLOAT, AT_OFF(terrainStep),         0.5,     10,        3,       "Abtastweite entlang eines Abschnitts (yd)" },
        { "tick",                "AutoTravel.UpdateIntervalMs",        OPT_UINT,  AT_OFF(updateIntervalMs),    50,      1000,      200,     "Taktweite des Moduls (ms)" },

        { "control",             "AutoTravel.TakeClientControl",       OPT_BOOL,  AT_OFF(takeClientControl),   0,       1,         1,       "Clientkontrolle waehrend der Fahrt uebernehmen" },
        { "combatpause",         "AutoTravel.PauseInCombat",           OPT_BOOL,  AT_OFF(pauseInCombat),       0,       1,         1,       "im Kampf pausieren und Kontrolle abgeben" },
        { "combatresume",        "AutoTravel.ResumeAfterCombat",       OPT_BOOL,  AT_OFF(resumeAfterCombat),   0,       1,         1,       "nach dem Kampf weiterfahren" },
        { "grace",               "AutoTravel.CombatGraceMs",           OPT_UINT,  AT_OFF(combatGraceMs),       0,       30000,     2000,    "Wartezeit nach dem Kampf (ms)" },
        { "deathresume",         "AutoTravel.ResumeAfterDeath",        OPT_BOOL,  AT_OFF(resumeAfterDeath),    0,       1,         0,       "Ziel nach dem Tod behalten" },
        { "handover",            "AutoTravel.HandoverTimeoutMs",       OPT_UINT,  AT_OFF(handoverTimeoutMs),   10000,   3600000,   900000,  "Abbruch, wenn der Spieler dauerhaft uebernimmt (ms)" },

        { "stuck",               "AutoTravel.StuckDetection",          OPT_BOOL,  AT_OFF(stuckDetection),      0,       1,         1,       "Feststecken erkennen" },
        { "stucktime",           "AutoTravel.StuckTimeoutMs",          OPT_UINT,  AT_OFF(stuckTimeoutMs),      1000,    60000,     5000,    "Messfenster fuer Feststecken (ms)" },
        { "stuckdist",           "AutoTravel.StuckMinDistance",        OPT_FLOAT, AT_OFF(stuckMinDistance),    0.5,     50,        3,       "Mindestfortschritt im Messfenster (yd)" },
        { "repath",              "AutoTravel.MaxRepathAttempts",       OPT_UINT,  AT_OFF(maxRepathAttempts),   1,       50,        8,       "Versuche, bevor aufgegeben wird" },

        { "mount",               "AutoTravel.AutoMount",               OPT_BOOL,  AT_OFF(autoMount),           0,       1,         1,       "Reittier automatisch rufen" },
        { "mountdist",           "AutoTravel.MountMinDistance",        OPT_FLOAT, AT_OFF(mountMinDistance),    0,       5000,      150,     "ab dieser Restentfernung aufsitzen (yd)" },
        { "dismount",            "AutoTravel.DismountIndoors",         OPT_BOOL,  AT_OFF(dismountIndoors),     0,       1,         1,       "in Gebaeuden absteigen" },

        { "fly",                 "AutoTravel.AllowFlying",             OPT_BOOL,  AT_OFF(allowFlying),         0,       1,         1,       "eigenes Flugmount benutzen, wo erlaubt" },
        { "flydist",             "AutoTravel.FlyMinDistance",          OPT_FLOAT, AT_OFF(flyMinDistance),      100,     10000,     400,     "ab dieser Entfernung fliegen (yd)" },
        { "flyclear",            "AutoTravel.FlyClearance",            OPT_FLOAT, AT_OFF(flyClearance),        5,       200,       25,      "Sicherheitsabstand ueber Gelaende (yd)" },
        { "flysample",           "AutoTravel.FlySampleStep",           OPT_FLOAT, AT_OFF(flySampleStep),       5,       100,       25,      "Abtastweite des Hoehenprofils (yd)" },
        { "flymax",              "AutoTravel.FlyMaxHeight",            OPT_FLOAT, AT_OFF(flyMaxHeight),        30,      1000,      250,     "hoechste Flughoehe ueber Grund (yd)" },
        { "flyland",             "AutoTravel.FlyDescendDistance",      OPT_FLOAT, AT_OFF(flyDescendDistance),  10,      400,       60,      "Beginn des Sinkflugs vor dem Ziel (yd)" },

        { "taxi",                "AutoTravel.UseTaxi",                 OPT_BOOL,  AT_OFF(useTaxi),             0,       1,         1,       "Flugmeister selbstaendig benutzen" },
        { "taxidist",            "AutoTravel.TaxiMinDistance",         OPT_FLOAT, AT_OFF(taxiMinDistance),     200,     50000,     1500,    "ab dieser Restentfernung Flug pruefen (yd)" },
        { "taxisaving",          "AutoTravel.TaxiMinSaving",           OPT_FLOAT, AT_OFF(taxiMinSaving),       0,       0.95,      0.60,    "Anteil der Strecke, den der Flug sparen muss" },
        { "taxiwalk",            "AutoTravel.TaxiMaxWalkToNode",       OPT_FLOAT, AT_OFF(taxiMaxWalkToNode),   50,      10000,     1200,    "hoechster Fussweg zum Flugmeister (yd)" },
        { "taxicost",            "AutoTravel.TaxiMaxCostCopper",       OPT_UINT,  AT_OFF(taxiMaxCostCopper),   0,       100000000, 50000,   "Preisgrenze je Flug in Kupfer" },
        { "taxiboard",           "AutoTravel.TaxiBoardDistance",       OPT_FLOAT, AT_OFF(taxiBoardDistance),   2,       9.5,       8,       "Naehe zum Flugmeister zum Abheben (yd)" },

        { "transport",           "AutoTravel.UseTransports",           OPT_BOOL,  AT_OFF(useTransports),       0,       1,         1,       "Zeppelin, Schiff und Bahn begleiten" },
        { "transportwait",       "AutoTravel.TransportWaitMs",         OPT_UINT,  AT_OFF(transportWaitMs),     30000,   1800000,   300000,  "Wartezeit am Anleger (ms)" },

        { "swim",                "AutoTravel.Swim",                    OPT_BOOL,  AT_OFF(swim),                0,       1,         1,       "schwimmen erlaubt" },
        { "swimoffset",          "AutoTravel.SwimSurfaceOffset",       OPT_FLOAT, AT_OFF(swimSurfaceOffset),   0,       5,         1.2,     "Tiefe unter der Wasseroberflaeche (yd)" },
        { "swimdepth",           "AutoTravel.MinSwimDepth",            OPT_FLOAT, AT_OFF(minSwimDepth),        0.5,     20,        2,       "ab dieser Wassertiefe wird geschwommen (yd)" },
        { "swimair",             "AutoTravel.MaxUnderwaterMs",         OPT_UINT,  AT_OFF(maxUnderwaterMs),     5000,    300000,    45000,   "Abbruch nach dieser Zeit unter Wasser (ms)" },
        { "waterpenalty",        "AutoTravel.WaterPenalty",            OPT_FLOAT, AT_OFF(waterPenalty),        0,       20,        1.5,     "Zusatzkosten je Yard Schwimmstrecke" },

        { "rescue",              "AutoTravel.RescueUnderMesh",         OPT_BOOL,  AT_OFF(rescueUnderMesh),     0,       1,         1,       "bei fehlendem Bodenkontakt zuruecksetzen" },
        { "under",               "AutoTravel.UnderMeshDepth",          OPT_FLOAT, AT_OFF(underMeshDepth),      0.5,     20,        2.5,     "Tiefe, ab der es als durchgefallen gilt (yd)" },
        { "above",               "AutoTravel.AboveMeshHeight",         OPT_FLOAT, AT_OFF(aboveMeshHeight),     2,       60,        12,      "Hoehe, ab der es als schwebend gilt (yd)" },
        { "rescuerange",         "AutoTravel.RescueSearchRange",       OPT_FLOAT, AT_OFF(rescueSearchRange),   4,       60,        14,      "Suchfenster um die Pfadhoehe (yd)" },
        { "maxrescue",           "AutoTravel.MaxRescues",              OPT_UINT,  AT_OFF(maxRescues),          1,       50,        6,       "Rettungen, bevor abgebrochen wird" },
        { "walkslope",           "AutoTravel.MaxWalkSlope",            OPT_FLOAT, AT_OFF(maxWalkSlope),        0.3,     3,         1.2,     "groesste Neigung, die als Laufflaeche gilt" },
        { "zoutlier",            "AutoTravel.ZOutlierTolerance",       OPT_FLOAT, AT_OFF(zOutlierTolerance),   0.5,     20,        2.5,     "Abweichung, ab der ein Punkt Ausreisser ist (yd)" },
        { "stepup",              "AutoTravel.MaxStepUp",               OPT_FLOAT, AT_OFF(maxStepUp),           0.5,     10,        2.5,     "groesste Stufe nach oben (yd)" },
        { "stepdown",            "AutoTravel.MaxStepDown",             OPT_FLOAT, AT_OFF(maxStepDown),         1,       40,        6,       "groesster Absatz nach unten (yd)" },
        { "planes",              "AutoTravel.GroundPlanes",            OPT_UINT,  AT_OFF(groundPlanes),        1,       8,         4,       "wie viele Etagen je Punkt gesucht werden" },

        { "nodes",               "AutoTravel.UseTravelNodes",          OPT_BOOL,  AT_OFF(useTravelNodes),      0,       1,         1,       "Knotengraph von mod-playerbots benutzen" },
        { "noderadius",          "AutoTravel.NodeSearchRadius",        OPT_FLOAT, AT_OFF(nodeSearchRadius),    50,      10000,     800,     "Suchradius fuer den naechsten Knoten (yd)" },
        { "nodemin",             "AutoTravel.NodeMinDistance",         OPT_FLOAT, AT_OFF(nodeMinDistance),     0,       10000,     300,     "ab dieser Entfernung Knoten benutzen (yd)" },
        { "nodeskip",            "AutoTravel.SkipDetourFactor",        OPT_FLOAT, AT_OFF(skipDetourFactor),    1,       5,         1.25,    "Umwegfaktor, ab dem der Startknoten entfaellt" },
        { "speciallinks",        "AutoTravel.UseSpecialLinks",         OPT_BOOL,  AT_OFF(useSpecialLinks),     0,       1,         1,       "Portale und Transporte im Graphen erlauben" },
        { "specialcost",         "AutoTravel.SpecialLinkCost",         OPT_FLOAT, AT_OFF(specialLinkCost),     0,       100000,    400,     "Aufschlag fuer Sonderverbindungen" },

        { "teleport",            "AutoTravel.AllowTeleport",           OPT_BOOL,  AT_OFF(allowTeleport),       0,       1,         1,       "Teleportbefehl erlaubt" },
        { "teleportsec",         "AutoTravel.TeleportSecurity",        OPT_UINT,  AT_OFF(teleportSecurity),    0,       3,         2,       "Rechtestufe fuer den Teleport" },
        { "teleportmin",         "AutoTravel.TeleportMinDistance",     OPT_FLOAT, AT_OFF(teleportMinDist),     0,       50000,     0,       "Mindestentfernung fuer den Teleport (yd)" },
        { "teleportcd",          "AutoTravel.TeleportCooldownSec",     OPT_UINT,  AT_OFF(teleportCooldown),    0,       3600,      5,       "Abklingzeit des Teleports (s)" },

        { "natural",             "AutoTravel.NaturalPathing",          OPT_BOOL,  AT_OFF(naturalPathing),      0,       1,         1,       "natuerliche Wege bevorzugen" },
        { "slopestart",          "AutoTravel.SlopeStart",              OPT_FLOAT, AT_OFF(slopeStart),          0,       3,         0.15,    "Neigung, ab der Strafe faellt" },
        { "slopestrong",         "AutoTravel.SlopeStrong",             OPT_FLOAT, AT_OFF(slopeStrong),         0,       3,         0.25,    "Neigung fuer die zweite Stufe" },
        { "slopeextreme",        "AutoTravel.SlopeExtreme",            OPT_FLOAT, AT_OFF(slopeExtreme),        0,       3,         0.35,    "Neigung fuer die dritte Stufe" },
        { "slopecost",           "AutoTravel.SlopePenalty",            OPT_FLOAT, AT_OFF(slopePenalty),        0,       1000,      5,       "Strafe erste Stufe" },
        { "slopecost2",          "AutoTravel.SteepSlopePenalty",       OPT_FLOAT, AT_OFF(steepSlopePenalty),   0,       1000,      10,      "Strafe zweite Stufe" },
        { "slopecost3",          "AutoTravel.ExtremeSlopePenalty",     OPT_FLOAT, AT_OFF(extremeSlopePenalty), 0,       1000,      30,      "Strafe dritte Stufe" },
        { "elevwindow",          "AutoTravel.ElevationWindow",         OPT_FLOAT, AT_OFF(elevationWindow),     10,      400,       40,      "Fensterlaenge fuer den Hoehengewinn (yd)" },
        { "elevstart",           "AutoTravel.ElevationGainStart",      OPT_FLOAT, AT_OFF(elevationGainStart),  0,       200,       4,       "Hoehengewinn erste Stufe (yd)" },
        { "elevstrong",          "AutoTravel.ElevationGainStrong",     OPT_FLOAT, AT_OFF(elevationGainStrong), 0,       200,       10,      "Hoehengewinn zweite Stufe (yd)" },
        { "elevextreme",         "AutoTravel.ElevationGainExtreme",    OPT_FLOAT, AT_OFF(elevationGainExtreme),0,       200,       20,      "Hoehengewinn dritte Stufe (yd)" },
        { "elevcost",            "AutoTravel.ElevationPenalty",        OPT_FLOAT, AT_OFF(elevationPenalty),    0,       1000,      3,       "Strafe erste Stufe" },
        { "elevcost2",           "AutoTravel.StrongElevationPenalty",  OPT_FLOAT, AT_OFF(strongElevationPenalty),0,     1000,      8,       "Strafe zweite Stufe" },
        { "elevcost3",           "AutoTravel.ExtremeElevationPenalty", OPT_FLOAT, AT_OFF(extremeElevationPenalty),0,    1000,      20,      "Strafe dritte Stufe" },
        { "turnstart",           "AutoTravel.TurnPenaltyStart",        OPT_FLOAT, AT_OFF(turnPenaltyStart),    0,       180,       35,      "Winkel, ab dem Strafe faellt (Grad)" },
        { "turnstrong",          "AutoTravel.TurnPenaltyStrong",       OPT_FLOAT, AT_OFF(turnPenaltyStrong),   0,       180,       70,      "Winkel zweite Stufe (Grad)" },
        { "turnextreme",         "AutoTravel.TurnPenaltyExtreme",      OPT_FLOAT, AT_OFF(turnPenaltyExtreme),  0,       180,       110,     "Winkel dritte Stufe (Grad)" },
        { "turncost",            "AutoTravel.TurnPenalty",             OPT_FLOAT, AT_OFF(turnPenalty),         0,       1000,      2,       "Strafe erste Stufe" },
        { "turncost2",           "AutoTravel.StrongTurnPenalty",       OPT_FLOAT, AT_OFF(strongTurnPenalty),   0,       1000,      5,       "Strafe zweite Stufe" },
        { "turncost3",           "AutoTravel.ExtremeTurnPenalty",      OPT_FLOAT, AT_OFF(extremeTurnPenalty),  0,       1000,      12,      "Strafe dritte Stufe" },
        { "incomplete",          "AutoTravel.IncompletePathPenalty",   OPT_FLOAT, AT_OFF(incompletePathPenalty),0,      1000000,   10000,   "Aufschlag fuer Teilwege" },
        { "cliffcost",           "AutoTravel.CliffPenalty",            OPT_FLOAT, AT_OFF(cliffPenalty),        0,       100000,    400,     "Aufschlag je erkanntem Absturz" },

        { "contour",             "AutoTravel.ContourProbing",          OPT_BOOL,  AT_OFF(contourProbing),      0,       1,         1,       "Weg um den Berg herum suchen" },
        { "contour_elevation",   "AutoTravel.ContourTriggerElevation", OPT_FLOAT, AT_OFF(contourTriggerElevation),1,    200,       15,      "Hoehengewinn, der die Suche ausloest (yd)" },
        { "contour_slope",       "AutoTravel.ContourTriggerSlope",     OPT_FLOAT, AT_OFF(contourTriggerSlope), 0.01,    3,         0.20,    "Steigung, die die Suche ausloest" },
        { "contour_narrow",      "AutoTravel.ContourNarrowOffset",     OPT_FLOAT, AT_OFF(contourNarrowOffset), 10,      1000,      100,     "erster Suchabstand seitlich (yd)" },
        { "contour_wide",        "AutoTravel.ContourWideOffset",       OPT_FLOAT, AT_OFF(contourWideOffset),   10,      2000,      180,     "zweiter Suchabstand seitlich (yd)" },
        { "contour_first",       "AutoTravel.ContourFirstProgress",    OPT_FLOAT, AT_OFF(contourFirstProgress),0.05,    0.90,      0.35,    "Lage des ersten Suchpunkts auf der Strecke" },
        { "contour_second",      "AutoTravel.ContourSecondProgress",   OPT_FLOAT, AT_OFF(contourSecondProgress),0.10,   0.95,      0.65,    "Lage des zweiten Suchpunkts" },
        { "contour_factor",      "AutoTravel.ContourMaxDistanceFactor",OPT_FLOAT, AT_OFF(contourMaxDistanceFactor),1,   10,        2.5,     "hoechster Umwegfaktor der Contour-Route" },
    };

    constexpr size_t OPTION_COUNT = sizeof(sOptions) / sizeof(sOptions[0]);

    ATOption const* FindOption(std::string const& key)
    {
        for (size_t i = 0; i < OPTION_COUNT; ++i)
            if (key == sOptions[i].key)
                return &sOptions[i];
        return nullptr;
    }

    void* FieldPtr(ATOption const& o)
    {
        return reinterpret_cast<char*>(&ATConf) + o.offset;
    }

    double ReadOption(ATOption const& o)
    {
        void* p = FieldPtr(o);
        switch (o.type)
        {
            case OPT_BOOL:  return *reinterpret_cast<bool*>(p) ? 1.0 : 0.0;
            case OPT_FLOAT: return double(*reinterpret_cast<float*>(p));
            case OPT_UINT:  return double(*reinterpret_cast<uint32*>(p));
        }
        return 0.0;
    }

    // Schreibt mit Bereichsbegrenzung. Gibt zurueck, ob der Wert unveraendert
    // uebernommen werden konnte; ein begrenzter Wert meldet false, damit der
    // Aufrufer den Spieler darauf hinweisen kann.
    bool WriteOption(ATOption const& o, double v, double& applied)
    {
        bool clamped = false;
        if (o.type != OPT_BOOL)
        {
            if (v < o.minV) { v = o.minV; clamped = true; }
            if (v > o.maxV) { v = o.maxV; clamped = true; }
        }

        void* p = FieldPtr(o);
        switch (o.type)
        {
            case OPT_BOOL:  *reinterpret_cast<bool*>(p)   = (v != 0.0); break;
            case OPT_FLOAT: *reinterpret_cast<float*>(p)  = float(v);   break;
            case OPT_UINT:  *reinterpret_cast<uint32*>(p) = uint32(v < 0.0 ? 0.0 : v); break;
        }
        applied = v;
        return !clamped;
    }

    std::string Format(double v, ATOptType t)
    {
        char b[64];
        if (t == OPT_BOOL)
            std::snprintf(b, sizeof(b), "%s", v != 0.0 ? "an" : "aus");
        else if (t == OPT_UINT)
            std::snprintf(b, sizeof(b), "%u", uint32(v));
        else
            std::snprintf(b, sizeof(b), "%.3f", v);
        return b;
    }
}

// ---------------------------------------------------------------------------
// Laden
// ---------------------------------------------------------------------------

void AutoTravelMgr::LoadConfig()
{
    for (size_t i = 0; i < OPTION_COUNT; ++i)
    {
        ATOption const& o = sOptions[i];
        double v = o.defV;

        switch (o.type)
        {
            case OPT_BOOL:
                v = sConfigMgr->GetOption<bool>(o.confName, o.defV != 0.0) ? 1.0 : 0.0;
                break;
            case OPT_FLOAT:
                v = double(sConfigMgr->GetOption<float>(o.confName, float(o.defV)));
                break;
            case OPT_UINT:
                v = double(sConfigMgr->GetOption<uint32>(o.confName, uint32(o.defV)));
                break;
        }

        double applied = v;
        if (!WriteOption(o, v, applied))
        {
            LOG_WARN("server.loading",
                     "mod-autotravel: '{}' lag ausserhalb des erlaubten Bereichs und wurde auf {} gesetzt.",
                     o.confName, Format(applied, o.type));
        }
    }

    // Werte, die keine Zahl sind, liegen ausserhalb der Registry.
    ATNodeDb = sConfigMgr->GetOption<std::string>("AutoTravel.NodeDatabase", "acore_playerbots");

    // --- Gegenpruefungen ---------------------------------------------------
    // Zwoelf Plausibilitaetspruefungen der Schwellwerte gegeneinander. Sie
    // fangen Einstellungen ab, die sich gegenseitig aufheben oder das Modul in
    // eine Endlosschleife treiben.
    struct Check { bool bad; char const* text; };
    Check const checks[] =
    {
        { ATConf.arrivalDistance >= ATConf.legDistance,
          "ArrivalDistance sollte kleiner als LegDistance sein, sonst gilt die letzte Etappe frueher als erreicht als jede Zwischenetappe." },
        { ATConf.elevationWindow <= ATConf.elevationGainStart,
          "ElevationWindow muss deutlich groesser als ElevationGainStart sein, sonst greift die Bergstrafe nie." },
        { ATConf.elevationGainStart >= ATConf.elevationGainStrong ||
          ATConf.elevationGainStrong >= ATConf.elevationGainExtreme,
          "Die drei ElevationGain-Stufen muessen aufsteigend sein." },
        { ATConf.slopeStart >= ATConf.slopeStrong || ATConf.slopeStrong >= ATConf.slopeExtreme,
          "Die drei Slope-Stufen muessen aufsteigend sein." },
        { ATConf.turnPenaltyStart >= ATConf.turnPenaltyStrong ||
          ATConf.turnPenaltyStrong >= ATConf.turnPenaltyExtreme,
          "Die drei TurnPenalty-Stufen muessen aufsteigend sein." },
        { ATConf.incompletePathPenalty < ATConf.extremeElevationPenalty * 100.0f,
          "IncompletePathPenalty sollte teurer als jeder Berg sein, sonst gewinnt ein Teilweg gegen einen vollstaendigen." },
        { ATConf.underMeshDepth >= ATConf.rescueSearchRange,
          "UnderMeshDepth muss kleiner als RescueSearchRange sein, sonst kann die Rettung nichts finden." },
        { ATConf.aboveMeshHeight <= ATConf.maxStepUp,
          "AboveMeshHeight muss groesser als MaxStepUp sein, sonst gilt jede Stufe als Schweben." },
        { ATConf.contourFirstProgress >= ATConf.contourSecondProgress,
          "ContourFirstProgress muss vor ContourSecondProgress liegen." },
        { ATConf.contourNarrowOffset >= ATConf.contourWideOffset,
          "ContourNarrowOffset muss kleiner als ContourWideOffset sein." },
        { ATConf.terrainStep > ATConf.legDistance,
          "TerrainStep groesser als LegDistance erzeugt Etappen ohne Zwischenpunkte." },
        { ATConf.flyClearance >= ATConf.flyMaxHeight,
          "FlyClearance muss kleiner als FlyMaxHeight sein, sonst ist keine Flughoehe gueltig." },
    };

    uint32 problems = 0;
    for (Check const& c : checks)
    {
        if (c.bad)
        {
            ++problems;
            LOG_WARN("server.loading", "mod-autotravel: {}", c.text);
        }
    }

    LOG_INFO("server.loading",
             "mod-autotravel: Konfiguration geladen ({} Werte, {} Hinweise).",
             uint32(OPTION_COUNT), problems);

    LoadMapAreas();
    LoadTravelNodes();
}

// ---------------------------------------------------------------------------
// Laufzeitaenderung
// ---------------------------------------------------------------------------

bool AutoTravelMgr::SetOption(Player* player, std::string const& key, std::string const& value)
{
    std::string k = key;
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    // Zwei Schluessel gelten nur fuer die eigene Sitzung, nicht global.
    // Ohne diese Trennung wuerde ein Spieler mit "/at ziel 20" den Zielradius
    // fuer alle anderen mitverstellen.
    if (k == "arrival" || k == "grace")
    {
        ATSession& s = _sessions[player->GetGUID()];
        float v = 0.0f;
        if (!AT::ParseFloat(value, v))
        {
            Msg(player, "Ungueltiger Wert.");
            return false;
        }

        if (k == "arrival")
        {
            if (v < 0.5f || v > 100.0f)
            {
                Msg(player, "Zielradius muss zwischen 0.5 und 100 Yards liegen.");
                return false;
            }
            s.arrivalOverride = v;
            Dbg(player, s, "Zielradius fuer diese Sitzung: " + value + " yd");
        }
        else
        {
            if (v < 0.0f || v > 30.0f)
            {
                Msg(player, "Wartezeit muss zwischen 0 und 30 Sekunden liegen.");
                return false;
            }
            s.graceOverride = uint32(v * 1000.0f);
            Dbg(player, s, "Wartezeit nach dem Kampf: " + value + " s");
        }

        // Reine Optionssitzungen nicht dauerhaft mitschleppen.
        if (s.state == AT_IDLE && s.arrivalOverride <= 0.0f && s.graceOverride == 0 && !s.debug)
            _sessions.erase(player->GetGUID());
        return true;
    }

    ATOption const* o = FindOption(k);
    if (!o)
    {
        Msg(player, "Unbekannte Option '" + key + "'. '.at options' zeigt alle.");
        return false;
    }

    double v = 0.0;
    if (o->type == OPT_BOOL)
    {
        bool b = false;
        if (!AT::ParseBool(value, b))
        {
            Msg(player, "Fuer '" + k + "' wird 0 oder 1 erwartet.");
            return false;
        }
        v = b ? 1.0 : 0.0;
    }
    else
    {
        float f = 0.0f;
        if (!AT::ParseFloat(value, f))
        {
            Msg(player, "Fuer '" + k + "' wird eine Zahl erwartet.");
            return false;
        }
        v = double(f);
    }

    double applied = v;
    bool exact = WriteOption(*o, v, applied);

    char b[256];
    if (exact)
        std::snprintf(b, sizeof(b), "%s = %s", k.c_str(), Format(applied, o->type).c_str());
    else
        std::snprintf(b, sizeof(b), "%s = %s (auf den erlaubten Bereich begrenzt)",
                      k.c_str(), Format(applied, o->type).c_str());
    Msg(player, b);

    // Werte, die die Ladezeit betreffen, sofort wirksam machen.
    if (k == "nodes" || k == "speciallinks" || k == "specialcost")
        LoadTravelNodes();

    return true;
}

void AutoTravelMgr::ListOptions(Player* player, std::string const& filter)
{
    std::string f = filter;
    std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    Msg(player, "--- AutoTravel Optionen ---");

    uint32 shown = 0;
    char b[300];
    for (size_t i = 0; i < OPTION_COUNT; ++i)
    {
        ATOption const& o = sOptions[i];
        if (!f.empty() && std::string(o.key).find(f) == std::string::npos
                       && std::string(o.help).find(f) == std::string::npos)
            continue;

        std::snprintf(b, sizeof(b), "%-18s %-10s  %s",
                      o.key, Format(ReadOption(o), o.type).c_str(), o.help);
        Msg(player, b);
        ++shown;
        if (shown >= 40)
        {
            Msg(player, "... weitere Werte mit '.at options <suchwort>'.");
            break;
        }
    }

    if (!shown)
        Msg(player, "Kein Treffer.");
    else
        Msg(player, "Aendern mit '.at set <schluessel> <wert>'.");
}

// ---------------------------------------------------------------------------
// Meldungen
// ---------------------------------------------------------------------------
//
// Das Protokoll zum Addon laeuft ueber Systemnachrichten mit [AT]-Prefix. Das
// Addon filtert sie aus dem Chat heraus, sofern es geladen ist. Wer ohne Addon
// spielt, sieht die Meldungen im Klartext -- das ist gewollt.

void AutoTravelMgr::Raw(Player* player, std::string const& line) const
{
    if (!player || !player->GetSession())
        return;
    ChatHandler(player->GetSession()).SendSysMessage(line.c_str());
}

void AutoTravelMgr::Msg(Player* player, std::string const& text) const
{
    Raw(player, "[AT]M|" + text);
}

void AutoTravelMgr::Dbg(Player* player, ATSession const& s, std::string const& text) const
{
    if (!ATConf.debug && !s.debug)
        return;
    Raw(player, "[AT]D|" + text);
}

void AutoTravelMgr::SetDebug(Player* player, bool on)
{
    ATSession& s = _sessions[player->GetGUID()];
    s.debug = on;
    Msg(player, on ? "Debug-Modus aktiv." : "Debug-Modus aus.");
    if (!on && s.state == AT_IDLE && s.arrivalOverride <= 0.0f && s.graceOverride == 0)
        _sessions.erase(player->GetGUID());
}

// Handshake: das Addon schickt beim Login ".at hello" und wartet auf diese
// Antwort, bevor es weitere Befehle sendet. Ohne den Handshake wuerde ein
// fehlendes Servermodul dazu fuehren, dass der Charakter ".at start ..." laut
// im Chat sagt.
void AutoTravelMgr::SendHello(Player* player)
{
    _addonPlayers.insert(player->GetGUID());

    char b[192];
    std::snprintf(b, sizeof(b), "[AT]H|%s|%u|%u|%u",
                  "3.0",
                  ATConf.enable ? 1u : 0u,
                  uint32(NodeCount()),
                  ATConf.useTaxi ? 1u : 0u);
    Raw(player, b);
}
