/*
 * AutoTravel_Route.cpp
 * ---------------------------------------------------------------------------
 * Kartenkoordinaten des Clients in Weltkoordinaten des Servers umrechnen,
 * Routen aus Etappen aufbauen, Start, Stop und Diagnose.
 *
 * Der schwierige Teil ist die Umrechnung. Carbonite kennt Positionen als
 * Zonenkoordinaten 0..1. Um daraus Weltkoordinaten zu machen, braucht es den
 * Kartenausschnitt aus WorldMapArea.dbc. AzerothCore legt fuer diese Datei
 * keinen DBCStorage an, obwohl der Extractor sie erzeugt -- deshalb liest
 * dieses Modul sie selbst.
 */

#include "AutoTravel.h"

#include "Chat.h"
#include "Config.h"
#include "GameTime.h"
#include "GridDefines.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

// ---------------------------------------------------------------------------
// Sitzungszugriff
// ---------------------------------------------------------------------------

ATSession* AutoTravelMgr::Find(Player* player)
{
    if (!player)
        return nullptr;
    auto it = _sessions.find(player->GetGUID());
    return (it == _sessions.end()) ? nullptr : &it->second;
}

ATSession const* AutoTravelMgr::Find(Player* player) const
{
    if (!player)
        return nullptr;
    auto it = _sessions.find(player->GetGUID());
    return (it == _sessions.end()) ? nullptr : &it->second;
}

bool AutoTravelMgr::IsActive(Player* player) const
{
    ATSession const* s = Find(player);
    return s && s->state != AT_IDLE;
}

// NodeCount() steht in AutoTravel_Nodes.cpp, weil dort die Knotentabelle liegt.

// ---------------------------------------------------------------------------
// WorldMapArea.dbc -- eigener Loader
// ---------------------------------------------------------------------------
//
// Das DBC-Format ist trivial und seit Vanilla unveraendert:
//
//   char   magic[4] = "WDBC"
//   uint32 recordCount, fieldCount, recordSize, stringBlockSize
//   danach recordCount * recordSize Bytes
//
// WorldMapArea.dbc (3.3.5a), die ersten acht Felder:
//   0 ID   1 mapID   2 areaID   3 areaName(Stringoffset)
//   4 locLeft   5 locRight   6 locTop   7 locBottom      (alle float)
//
// Nur diese acht werden gelesen. Ob dahinter zehn oder elf Felder stehen, ist
// egal -- der Offset der ersten acht ist stabil.

namespace
{
    struct ATMapArea
    {
        uint32 mapId = 0;
        uint32 areaId = 0;
        float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
    };

    std::unordered_map<uint32, ATMapArea> sATMapAreas;
    bool sATMapAreasLoaded = false;

    // Die Zuordnung Client-Karten-ID -> WorldMapArea-ID steht NICHT hier,
    // sondern je Spieler in AutoTravelMgr::_calib: die vom Client gelieferte ID
    // (GetCurrentMapAreaID) ist nicht zwangslaeufig die aus dem DBC, und die
    // Angaben, aus denen sie gelernt wird, stammen vom Client.

    void ApplyArea(ATMapArea const& e, bool swapped, float mx, float my, float& wx, float& wy)
    {
        if (!swapped)
        {
            // waagerechte Kartenachse -> Welt-Y, senkrechte -> Welt-X
            wy = e.left + mx * (e.right - e.left);
            wx = e.top  + my * (e.bottom - e.top);
        }
        else
        {
            wx = e.left + mx * (e.right - e.left);
            wy = e.top  + my * (e.bottom - e.top);
        }
    }

    bool LoadWorldMapAreaFile(std::string const& path, std::string& note)
    {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in.is_open())
            return false;

        char magic[4];
        in.read(magic, 4);
        if (!in || magic[0] != 'W' || magic[1] != 'D' || magic[2] != 'B' || magic[3] != 'C')
        {
            note = path + " ist keine DBC-Datei.";
            return false;
        }

        uint32 recordCount = 0, fieldCount = 0, recordSize = 0, stringSize = 0;
        in.read(reinterpret_cast<char*>(&recordCount), 4);
        in.read(reinterpret_cast<char*>(&fieldCount), 4);
        in.read(reinterpret_cast<char*>(&recordSize), 4);
        in.read(reinterpret_cast<char*>(&stringSize), 4);

        if (!in || fieldCount < 8 || recordSize < fieldCount * 4 || recordCount == 0)
        {
            note = path + ": unerwarteter Aufbau (Felder " + std::to_string(fieldCount) + ").";
            return false;
        }

        // Schutz gegen eine beschaedigte Datei mit absurden Kopfdaten.
        if (uint64(recordCount) * uint64(recordSize) > 64ull * 1024ull * 1024ull)
        {
            note = path + ": Kopfdaten unplausibel gross.";
            return false;
        }

        std::vector<char> buf(size_t(recordCount) * recordSize);
        in.read(buf.data(), std::streamsize(buf.size()));
        if (!in)
        {
            note = path + ": Datei unvollstaendig.";
            return false;
        }

        for (uint32 i = 0; i < recordCount; ++i)
        {
            char const* rec = buf.data() + size_t(i) * recordSize;
            uint32 u[8];
            float  f[8];
            std::memcpy(u, rec, sizeof(u));
            std::memcpy(f, rec, sizeof(f));

            ATMapArea a;
            a.mapId  = u[1];
            a.areaId = u[2];
            a.left   = f[4];
            a.right  = f[5];
            a.top    = f[6];
            a.bottom = f[7];

            // Eintraege ohne Ausdehnung sind unbrauchbar.
            if (a.left == a.right || a.top == a.bottom)
                continue;

            sATMapAreas[u[0]] = a;
        }

        note = path + ": " + std::to_string(sATMapAreas.size()) + " Kartenausschnitte geladen.";
        return !sATMapAreas.empty();
    }
}

void AutoTravelMgr::LoadMapAreas()
{
    sATMapAreas.clear();
    sATMapAreasLoaded = false;
    _calib.clear();

    std::string dataDir = sConfigMgr->GetOption<std::string>("DataDir", ".");
    if (!dataDir.empty() && dataDir[dataDir.size() - 1] != '/' && dataDir[dataDir.size() - 1] != '\\')
        dataDir += "/";

    std::string const candidates[] =
    {
        dataDir + "dbc/WorldMapArea.dbc",
        dataDir + "dbc/enUS/WorldMapArea.dbc",
        dataDir + "dbc/enGB/WorldMapArea.dbc",
        dataDir + "dbc/deDE/WorldMapArea.dbc",
        dataDir + "dbc/frFR/WorldMapArea.dbc",
        dataDir + "dbc/ruRU/WorldMapArea.dbc",
        "dbc/WorldMapArea.dbc",
    };

    std::string note;
    for (auto const& p : candidates)
    {
        if (LoadWorldMapAreaFile(p, note))
        {
            sATMapAreasLoaded = true;
            LOG_INFO("server.loading", "mod-autotravel: {}", note);
            return;
        }
    }

    LOG_ERROR("server.loading",
              "mod-autotravel: WorldMapArea.dbc nicht gefunden oder unlesbar (DataDir='{}'). "
              "Die Zielumrechnung ist damit deaktiviert. {}", dataDir, note);
}

// ---------------------------------------------------------------------------
// Karten-ID lernen
// ---------------------------------------------------------------------------
//
// Schickt das Addon zusaetzlich seine eigene normalisierte Position mit, dann
// kennt der Server fuer denselben Punkt beide Darstellungen. Er probiert alle
// Kartenausschnitte der aktuellen Map durch und nimmt den, der die bekannte
// echte Position reproduziert. Das ist eindeutig: nur der richtige Ausschnitt
// trifft auf wenige Yards genau.

void AutoTravelMgr::LearnMapId(Player* player, uint32 clientMapId, float pnx, float pny)
{
    if (!player || !sATMapAreasLoaded || !clientMapId || pnx <= 0.0f || pny <= 0.0f)
        return;

    ATCalibration& cal = _calib[player->GetGUID()];
    if (cal.fix.find(clientMapId) != cal.fix.end())
        return;
    if (cal.fix.size() >= MAX_CALIB_ENTRIES)
        return;                            // ein Client darf nicht unbegrenzt Karten melden

    float bestErr = 1.0e9f;
    uint32 bestId = 0;

    for (auto const& kv : sATMapAreas)
    {
        if (kv.second.mapId != player->GetMapId())
            continue;
        for (int sw = 0; sw < 2; ++sw)
        {
            float wx, wy;
            ApplyArea(kv.second, sw != 0, pnx, pny, wx, wy);
            float e = AT::Dist2D(wx, wy, player->GetPositionX(), player->GetPositionY());
            if (e < bestErr)
            {
                bestErr = e;
                bestId = kv.first;
            }
        }
    }

    if (bestId && bestErr <= 250.0f)
    {
        cal.fix[clientMapId] = bestId;
        cal.delta = int32(int64(bestId) - int64(clientMapId));
        cal.deltaKnown = true;
        if (bestId != clientMapId)
            LOG_DEBUG("module", "mod-autotravel: {}: Karten-ID {} des Clients entspricht "
                                "WorldMapArea {} (Abweichung {:.1f} yd).",
                      player->GetName(), clientMapId, bestId, bestErr);
    }
}

// ---------------------------------------------------------------------------
// Kartenkoordinaten -> Weltkoordinaten
// ---------------------------------------------------------------------------

bool AutoTravelMgr::MapToWorld(Player* player, uint32 uiMapId, float nx, float ny,
                               bool hasCalib, float pnx, float pny,
                               float& outX, float& outY, std::string& err,
                               uint32* targetMap) const
{
    if (!sATMapAreasLoaded)
    {
        err = "Serverseitig konnte WorldMapArea.dbc nicht geladen werden - siehe Serverlog.";
        return false;
    }

    // Gelernte Zuordnung dieses Spielers merken. Begrenzt, und nur fuer ihn:
    // die Angaben, aus denen sie entsteht, stammen vom Client.
    auto remember = [&](uint32 clientId, uint32 areaId)
    {
        ATCalibration& cal = _calib[player->GetGUID()];
        if (cal.fix.find(clientId) == cal.fix.end() && cal.fix.size() >= MAX_CALIB_ENTRIES)
            return;
        cal.fix[clientId] = areaId;
        cal.delta = int32(int64(areaId) - int64(clientId));
        cal.deltaKnown = true;
    };

    uint32 chosen = 0;
    bool   swapped = false;

    // --- Fall 1: Gegenprobe moeglich ---------------------------------------
    if (hasCalib && pnx > 0.0f && pny > 0.0f)
    {
        float bestErr = 1.0e9f;
        uint32 bestId = 0;
        bool   bestSwapped = false;

        for (auto const& kv : sATMapAreas)
        {
            if (kv.second.mapId != player->GetMapId())
                continue;

            for (int sw = 0; sw < 2; ++sw)
            {
                float wx, wy;
                ApplyArea(kv.second, sw != 0, pnx, pny, wx, wy);
                float e = AT::Dist2D(wx, wy, player->GetPositionX(), player->GetPositionY());
                if (e < bestErr)
                {
                    bestErr = e;
                    bestId = kv.first;
                    bestSwapped = (sw != 0);
                }
            }
        }

        if (!bestId || bestErr > 250.0f)
        {
            char b[288];
            std::snprintf(b, sizeof(b),
                "Kein passender Kartenausschnitt gefunden (bester Treffer ID %u, %.0f yd daneben). "
                "Das Ziel wird nicht angefahren. Pruefe die erkannte Zone mit '/at ziel'.",
                bestId, bestErr);
            err = b;
            return false;
        }

        if (bestId != uiMapId)
            LOG_DEBUG("module", "mod-autotravel: {}: Karten-ID {} des Clients entspricht "
                                "WorldMapArea {} (Abweichung {:.1f} yd).",
                      player->GetName(), uiMapId, bestId, bestErr);

        remember(uiMapId, bestId);

        chosen = bestId;
        swapped = bestSwapped;
    }
    // --- Fall 2: keine Gegenprobe -> gelernte Zuordnung benutzen -----------
    else
    {
        uint32 tryId = uiMapId;

        auto cit = _calib.find(player->GetGUID());
        if (cit != _calib.end())
        {
            auto fix = cit->second.fix.find(uiMapId);
            if (fix != cit->second.fix.end())
                tryId = fix->second;
            else if (cit->second.deltaKnown)
            {
                int64 shifted = int64(uiMapId) + int64(cit->second.delta);
                if (shifted > 0 && shifted <= int64(0xFFFFFFFFu)
                    && sATMapAreas.find(uint32(shifted)) != sATMapAreas.end())
                    tryId = uint32(shifted);
            }
        }

        auto it = sATMapAreas.find(tryId);
        if (it == sATMapAreas.end())
        {
            err = "Unbekannte Karten-ID " + std::to_string(uiMapId) + ".";
            return false;
        }
        // Ein Ziel auf einer anderen Karte (anderer Kontinent, Instanz) ist nur
        // dann ein Fehler, wenn der Aufrufer damit nicht umgehen kann.
        if (it->second.mapId != player->GetMapId() && !targetMap)
        {
            err = "Das Ziel liegt auf einer anderen Karte (Map " +
                  std::to_string(it->second.mapId) + ").";
            return false;
        }
        chosen = tryId;
    }

    ATMapArea const& e = sATMapAreas.find(chosen)->second;
    ApplyArea(e, swapped, nx, ny, outX, outY);

    if (targetMap)
        *targetMap = e.mapId;

    // Auf einer fremden Karte gibt es fuer diesen Spieler kein Gelaende, gegen das
    // sich die Zone pruefen liesse. Das holt ResolveWorld nach, sobald der
    // Spieler dort ist (SetLegTarget).
    if (e.mapId != player->GetMapId())
        return true;

    // --- Gegenprobe ueber die Zone -----------------------------------------
    // Der aufgeloeste Punkt muss in der Zone liegen, die zu diesem
    // Kartenausschnitt gehoert. Damit faellt eine um eins verschobene Karten-ID
    // auch dann auf, wenn der Spieler ganz woanders steht -- genau der Fall bei
    // Zwischenstuetzpunkten einer Route.
    if (!hasCalib && e.areaId)
    {
        float probeZ = BestGroundZ(player, outX, outY);
        if (probeZ > INVALID_HEIGHT)
        {
            uint32 zoneAt = player->GetMap()->GetZoneId(player->GetPhaseMask(), outX, outY, probeZ);
            if (zoneAt && zoneAt != e.areaId)
            {
                for (int32 d = -1; d <= 1; d += 2)
                {
                    auto alt = sATMapAreas.find(uint32(int32(chosen) + d));
                    if (alt == sATMapAreas.end() || alt->second.mapId != player->GetMapId())
                        continue;

                    float ax, ay;
                    ApplyArea(alt->second, swapped, nx, ny, ax, ay);
                    float az = BestGroundZ(player, ax, ay);
                    if (az <= INVALID_HEIGHT)
                        continue;

                    if (player->GetMap()->GetZoneId(player->GetPhaseMask(), ax, ay, az) == alt->second.areaId)
                    {
                        remember(uiMapId, alt->first);
                        LOG_DEBUG("module", "mod-autotravel: {}: Karten-ID {} ueber die Zonenpruefung "
                                            "auf WorldMapArea {} korrigiert.",
                                  player->GetName(), uiMapId, alt->first);
                        outX = ax;
                        outY = ay;
                        return true;
                    }
                }
            }
        }
    }

    return true;
}

bool AutoTravelMgr::ResolveWorld(Player* player, uint32 uiMapId, float nx, float ny,
                                 bool hasCalib, float pnx, float pny,
                                 float& x, float& y, float& z, uint32& mapId,
                                 std::string& err, bool allowOtherMap) const
{
    if (nx < 0.0f || nx > 1.0f || ny < 0.0f || ny > 1.0f)
    {
        err = "Ungueltige Zielkoordinaten vom Addon erhalten.";
        return false;
    }

    uint32 areaMap = AT_NO_MAP;
    if (!MapToWorld(player, uiMapId, nx, ny, hasCalib, pnx, pny, x, y, err,
                    allowOtherMap ? &areaMap : nullptr))
        return false;

    mapId = player->GetMapId();

    // Ziel auf einer anderen Karte: x/y stehen fest, die Hoehe laesst sich dort
    // nicht abfragen (das Gelaende ist nur fuer die Karte des Spielers geladen).
    if (allowOtherMap && areaMap != AT_NO_MAP && areaMap != player->GetMapId())
    {
        mapId = areaMap;
        z = 0.0f;
        return true;
    }

    z = BestGroundZ(player, x, y);
    if (z <= INVALID_HEIGHT)
    {
        err = "Fuer diese Position sind keine Hoehendaten verfuegbar (fehlende vmaps?).";
        return false;
    }
    return true;
}

void AutoTravelMgr::Resolve(Player* player, uint32 uiMapId, float nx, float ny,
                            bool hasCalib, float pnx, float pny)
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint32 mapId = 0;
    std::string err;

    if (!ResolveWorld(player, uiMapId, nx, ny, hasCalib, pnx, pny, x, y, z, mapId, err))
    {
        Msg(player, err);
        return;
    }

    char buf[192];
    std::snprintf(buf, sizeof(buf), "[AT]W|%u|%.3f|%.3f|%.3f", mapId, x, y, z);
    Raw(player, buf);
}

// ---------------------------------------------------------------------------
// Etappen
// ---------------------------------------------------------------------------
//
// Format eines Stuetzpunkts vom Addon:  <kartenId>:<nx>:<ny>:<art>

void AutoTravelMgr::RouteAdd(Player* player, bool clearFirst, std::string const& packed)
{
    std::vector<ATLeg>& r = _pendingRoutes[player->GetGUID()];
    if (clearFirst)
        r.clear();

    std::istringstream iss(packed);
    std::string tok;

    while (iss >> tok)
    {
        size_t a = tok.find(':');
        size_t b = (a == std::string::npos) ? a : tok.find(':', a + 1);
        size_t c = (b == std::string::npos) ? b : tok.find(':', b + 1);
        if (a == std::string::npos || b == std::string::npos || c == std::string::npos)
            continue;

        ATLeg leg;
        uint32 kind = 0;

        if (!AT::ParseUInt(tok.substr(0, a), leg.uiMapId) || !leg.uiMapId)
            continue;
        if (!AT::ParseNorm(tok.substr(a + 1, b - a - 1), leg.nx))
            continue;
        if (!AT::ParseNorm(tok.substr(b + 1, c - b - 1), leg.ny))
            continue;
        if (!AT::ParseUInt(tok.substr(c + 1), kind))
            kind = 0;

        leg.kind = (kind == 1) ? AT_LEG_TAXI : AT_LEG_WALK;

        if (r.size() >= 32)
            break;
        r.push_back(leg);
    }
}

// Aktuelle Etappe in Weltkoordinaten aufloesen. Nicht aufloesbare Etappen
// werden uebersprungen statt die ganze Reise zu kippen.
bool AutoTravelMgr::SetLegTarget(Player* player, ATSession& s)
{
    while (s.legIdx < s.route.size())
    {
        ATLeg& leg = s.route[s.legIdx];

        if (!leg.resolved)
        {
            uint32 mapId = 0;
            std::string err;
            if (!ResolveWorld(player, leg.uiMapId, leg.nx, leg.ny, false, 0.0f, 0.0f,
                              leg.wx, leg.wy, leg.wz, mapId, err))
            {
                char b[256];
                std::snprintf(b, sizeof(b), "Stuetzpunkt %u uebersprungen: %s",
                              uint32(s.legIdx + 1), err.c_str());
                Dbg(player, s, b);
                ++s.legIdx;
                continue;
            }
            leg.mapId = mapId;
            leg.resolved = true;
        }

        // Ziel auf einer anderen Karte: jetzt, auf der Zielkarte, laesst sich der
        // Punkt vollstaendig aufloesen -- mit Zonenpruefung und Hoehe.
        if (leg.groundPending && leg.mapId == player->GetMapId())
        {
            uint32 mapId = 0;
            std::string err;
            float x = leg.wx, y = leg.wy, z = 0.0f;
            if (leg.uiMapId
                && ResolveWorld(player, leg.uiMapId, leg.nx, leg.ny, false, 0.0f, 0.0f,
                                x, y, z, mapId, err))
            {
                leg.wx = x;
                leg.wy = y;
                leg.wz = z;
            }
            else
            {
                float const gz = BestGroundZ(player, leg.wx, leg.wy);
                leg.wz = (gz > INVALID_HEIGHT) ? gz : player->GetPositionZ();
                Dbg(player, s, "Zielpunkt auf der neuen Karte nicht neu aufloesbar (" + err +
                               ") - bleibt bei den bisherigen Koordinaten.");
            }
            leg.groundPending = false;
        }

        // Etappen auf einer anderen Karte kann der Laeufer nicht anfahren. Sie
        // gehoeren zu einem Transport oder Portal und werden dort behandelt.
        if (leg.mapId != AT_NO_MAP && leg.mapId != player->GetMapId() && leg.kind == AT_LEG_WALK)
        {
            Dbg(player, s, "Stuetzpunkt liegt auf einer anderen Karte - uebersprungen.");
            ++s.legIdx;
            continue;
        }

        s.mapId = (leg.mapId != AT_NO_MAP) ? leg.mapId : player->GetMapId();
        s.destX = leg.wx;
        s.destY = leg.wy;
        s.destZ = leg.wz;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Routenplanung
// ---------------------------------------------------------------------------
//
// Reihenfolge der Quellen, jeweils nur wenn die vorige nichts liefert:
//
//   1. Flugmeister      -- schnellste Verbindung ueber weite Strecken
//   2. Playerbot-Knoten -- kennt Portale, Schiffe und Zeppeline
//   3. Carbonite        -- die vom Addon gelieferten Stuetzpunkte
//
// Das eigentliche Ziel bleibt in jedem Fall der letzte Carbonite-Punkt.

bool AutoTravelMgr::ApplyPlannedRoute(Player* player, ATSession& s, std::string& err)
{
    if (s.route.empty())
        return true;

    ATLeg last = s.route.back();
    if (!last.resolved)
    {
        uint32 m = 0;
        std::string resolveErr;
        if (!ResolveWorld(player, last.uiMapId, last.nx, last.ny, false, 0.0f, 0.0f,
                          last.wx, last.wy, last.wz, m, resolveErr, true))
            return true;                   // die Etappen laufen einzeln weiter
        last.mapId = m;
        last.resolved = true;
        last.groundPending = (m != player->GetMapId());
    }

    // Liegt das Ziel auf einer anderen Karte, sind Koordinaten nicht vergleichbar:
    // keine Luftlinie, kein Flug dorthin, und die Carbonite-Stuetzpunkte helfen
    // nicht -- ein Kontinent ist nur ueber Schiff, Zeppelin oder Portal erreichbar.
    bool const crossMap = (last.mapId != AT_NO_MAP && last.mapId != player->GetMapId());

    s.finalMapId = last.mapId;
    s.finalX = last.wx;
    s.finalY = last.wy;
    s.finalZ = last.wz;

    float direct = crossMap ? 0.0f : player->GetExactDist2d(last.wx, last.wy);
    s.startDistance = direct;

    // --- 1. Flugmeister ----------------------------------------------------
    // Der Knotengraph kennt Flugverbindungen ebenfalls; ein direkter Flugplan
    // gibt es nur innerhalb einer Karte.
    if (!crossMap && ATConf.useTaxi && direct >= ATConf.taxiMinDistance)
    {
        std::vector<ATLeg> plan;
        std::string note;
        if (BuildTaxiPlan(player, last.mapId, last.wx, last.wy, last.wz, plan, note))
        {
            plan.push_back(last);
            s.route = plan;
            s.legIdx = 0;
            Msg(player, "Flugroute geplant: " + note);
            return true;
        }
        Dbg(player, s, "Kein sinnvoller Flug (" + note + ").");
    }

    // --- 2. Knotengraph ----------------------------------------------------
    std::string nodeNote;
    if (!ATConf.useTravelNodes)
        nodeNote = "AutoTravel.UseTravelNodes ist abgeschaltet";
    else if (!crossMap && direct < ATConf.nodeMinDistance)
        nodeNote = "die Strecke ist zu kurz";
    else
    {
        std::vector<ATLeg> nodeLegs;
        if (BuildNodeRoute(player, last.mapId, last.wx, last.wy, last.wz, nodeLegs, nodeNote))
        {
            nodeLegs.push_back(last);
            s.route = nodeLegs;
            s.legIdx = 0;

            uint32 special = 0;
            for (ATLeg const& l : s.route)
                if (l.kind != AT_LEG_WALK)
                    ++special;

            char b[256];
            std::snprintf(b, sizeof(b), "Knotenroute: %s%s", nodeNote.c_str(),
                          special ? " (enthaelt Sonderverbindungen)" : "");
            Dbg(player, s, b);

            if (crossMap)
            {
                std::snprintf(b, sizeof(b),
                              "Das Ziel liegt auf einer anderen Karte (Map %u). AutoTravel plant den "
                              "Weg ueber %u Sonderverbindung(en) -- Schiff, Zeppelin oder Portal "
                              "musst du am Anleger selbst betreten.", last.mapId, special);
                Msg(player, b);
            }
            else if (special)
            {
                std::snprintf(b, sizeof(b),
                              "Die Route benutzt %u Sonderverbindung(en). AutoTravel bringt dich "
                              "hin und macht danach von selbst weiter.", special);
                Msg(player, b);
            }
            return true;
        }
        if (!crossMap)
            Dbg(player, s, "Knotenroute nicht nutzbar (" + nodeNote + ") - benutze die Carbonite-Stuetzpunkte.");
    }

    // --- 3. Carbonite ------------------------------------------------------
    // s.route bleibt, wie es ist -- ausser bei einem Ziel auf einer anderen Karte:
    // dort gibt es ohne Knotenroute keinen Weg.
    if (crossMap)
    {
        err = "Das Ziel liegt auf einer anderen Karte (Map " + std::to_string(last.mapId) +
              "), und AutoTravel findet keine Verbindung dorthin: " + nodeNote + ". Ein Kontinent "
              "ist nur ueber Schiff, Zeppelin oder Portal erreichbar; dafuer braucht das Modul die "
              "Reiseknoten von mod-playerbots (siehe '.at nodes').";
        return false;
    }

    return true;
}

bool AutoTravelMgr::BeginTravel(Player* player, ATSession& s)
{
    s.startMapId = player->GetMapId();

    std::string planErr;
    if (!ApplyPlannedRoute(player, s, planErr))
    {
        Msg(player, planErr);
        _sessions.erase(player->GetGUID());
        SyncStatus(player);
        return false;
    }

    if (!SetLegTarget(player, s))
    {
        Msg(player, "Kein brauchbarer Stuetzpunkt in der Route.");
        _sessions.erase(player->GetGUID());
        SyncStatus(player);
        return false;
    }

    s.state = AT_CALCULATE_PATH;
    s.lastX = player->GetPositionX();
    s.lastY = player->GetPositionY();
    s.lastZ = player->GetPositionZ();
    ++_statTravelsStarted;

    // Bei einem Kartenwechsel gibt es keine vergleichbare Luftlinie; der
    // Fortschritt kommt dann aus der Etappenzahl (RemainingForStatus).
    if (s.startDistance <= 0.0f && (s.finalMapId == AT_NO_MAP || s.finalMapId == s.startMapId))
        s.startDistance = player->GetExactDist2d(s.finalX, s.finalY);

    Msg(player, "Reise gestartet: " + s.destName);
    PushStatus(player, s);
    return true;
}

// Naechste Etappe aktivieren. false = Route zu Ende.
bool AutoTravelMgr::AdvanceLeg(Player* player, ATSession& s)
{
    ++s.legIdx;
    s.repathAttempts = 0;
    s.stuckStrikes = 0;
    s.mountTried = false;
    s.path.clear();
    s.idx = 0;
    s.flying = false;

    if (s.legIdx >= s.route.size())
        return false;

    if (!SetLegTarget(player, s))
        return false;

    char b[192];
    std::snprintf(b, sizeof(b), "Stuetzpunkt %u/%u erreicht, weiter zum naechsten.",
                  uint32(s.legIdx), uint32(s.route.size()));
    Dbg(player, s, b);

    s.state = AT_CALCULATE_PATH;
    return true;
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

namespace
{
    // Gemeinsame Vorpruefung fuer alle Startwege.
    bool CanStart(AutoTravelMgr* mgr, Player* player)
    {
        if (!ATConf.enable)
        {
            mgr->Msg(player, "AutoTravel ist auf diesem Server deaktiviert.");
            return false;
        }
        if (!sWorld->getBoolConfig(CONFIG_ENABLE_MMAPS))
        {
            mgr->Msg(player, "Serverseitiges Pathfinding (mmaps) ist deaktiviert. "
                             "Ohne mmaps kann kein Weg berechnet werden.");
            return false;
        }
        if (!player->IsAlive())
        {
            mgr->Msg(player, "Du bist tot.");
            return false;
        }
        if (player->IsInFlight() || player->GetVehicle() || player->IsBeingTeleported())
        {
            mgr->Msg(player, "Jetzt gerade nicht moeglich (Flug, Fahrzeug oder Teleport laeuft).");
            return false;
        }
        return true;
    }
}

bool AutoTravelMgr::Start(Player* player, uint32 uiMapId, float nx, float ny,
                          bool hasCalib, float pnx, float pny, std::string const& name)
{
    // Jede Absage endet mit SyncStatus(): das Addon wechselt beim Absenden in
    // "Startet" und wartet auf eine Statuszeile. Eine Absage kommt aber nur als
    // Text; ohne die Statuszeile bliebe es dort haengen.
    if (!CanStart(this, player))
    {
        SyncStatus(player);
        return false;
    }

    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    uint32 tmpMap = 0;
    std::string err;
    // Das Ziel darf auf einer anderen Karte liegen (anderer Kontinent): ob und wie
    // man dorthin kommt, entscheidet die Routenplanung (Knotengraph).
    if (!ResolveWorld(player, uiMapId, nx, ny, hasCalib, pnx, pny, wx, wy, wz, tmpMap, err, true))
    {
        Msg(player, err);
        SyncStatus(player);
        return false;
    }

    ATSession& s = _sessions[player->GetGUID()];
    bool wasDebug = s.debug;
    uint32 keepGrace = s.graceOverride;
    float keepArrival = s.arrivalOverride;

    if (s.controlTaken)
        HaltMovement(player, s);

    s = ATSession();
    s.debug = wasDebug;
    s.graceOverride = keepGrace;
    s.arrivalOverride = keepArrival;
    s.mapId = player->GetMapId();
    s.destName = AT::SanitizeText(name, 48);
    if (s.destName.empty())
        s.destName = "Ziel";

    ATLeg leg;
    leg.uiMapId = uiMapId;
    leg.nx = nx;
    leg.ny = ny;
    leg.mapId = tmpMap;
    leg.wx = wx;
    leg.wy = wy;
    leg.wz = wz;
    leg.resolved = true;
    leg.groundPending = (tmpMap != player->GetMapId());
    leg.name = s.destName;
    s.route.push_back(leg);
    s.legIdx = 0;

    char buf[224];
    if (leg.groundPending)
        std::snprintf(buf, sizeof(buf), "Ziel: Map %u (andere Karte) | X %.2f Y %.2f",
                      tmpMap, wx, wy);
    else
        std::snprintf(buf, sizeof(buf), "Ziel: Map %u | X %.2f Y %.2f Z %.2f | %.0f yd",
                      s.mapId, wx, wy, wz, player->GetExactDist2d(wx, wy));
    Dbg(player, s, buf);

    return BeginTravel(player, s);
}

bool AutoTravelMgr::RouteStart(Player* player, std::string const& name)
{
    auto it = _pendingRoutes.find(player->GetGUID());
    if (it == _pendingRoutes.end() || it->second.empty())
    {
        Msg(player, "Keine Route empfangen.");
        SyncStatus(player);
        return false;
    }

    if (!CanStart(this, player))
    {
        SyncStatus(player);
        return false;
    }

    ATSession& s = _sessions[player->GetGUID()];
    bool wasDebug = s.debug;
    uint32 keepGrace = s.graceOverride;
    float keepArrival = s.arrivalOverride;

    if (s.controlTaken)
        HaltMovement(player, s);

    s = ATSession();
    s.debug = wasDebug;
    s.graceOverride = keepGrace;
    s.arrivalOverride = keepArrival;
    s.mapId = player->GetMapId();
    s.destName = AT::SanitizeText(name, 48);
    if (s.destName.empty())
        s.destName = "Ziel";

    // Die empfangene Route gehoert jetzt der Sitzung. Sie bleibt nicht als
    // Kopie in _pendingRoutes zurueck: das Addon sendet sie bei jedem Start neu,
    // und eine liegengebliebene Route koennte sonst von einem spaeteren
    // ".at rstart" ohne neue Uebertragung unbemerkt wiederverwendet werden.
    s.route = std::move(it->second);
    _pendingRoutes.erase(it);
    s.legIdx = 0;

    char buf[160];
    std::snprintf(buf, sizeof(buf), "Route mit %u Stuetzpunkten uebernommen.",
                  uint32(s.route.size()));
    Dbg(player, s, buf);

    return BeginTravel(player, s);
}

void AutoTravelMgr::Stop(Player* player, std::string const& reason, bool silent)
{
    auto it = _sessions.find(player->GetGUID());
    if (it == _sessions.end() || it->second.state == AT_IDLE)
    {
        if (!silent)
            Msg(player, "AutoTravel ist nicht aktiv.");
        return;
    }

    ATSession& s = it->second;
    HaltMovement(player, s);
    s.state = AT_IDLE;
    PushStatus(player, s);

    if (!silent)
        Msg(player, reason.empty() ? "Reise gestoppt." : reason);

    // Debug- und Optionswerte behalten, den Rest wegwerfen.
    bool keepDebug = s.debug;
    float keepArrival = s.arrivalOverride;
    uint32 keepGrace = s.graceOverride;

    if (keepDebug || keepArrival > 0.0f || keepGrace > 0)
    {
        s = ATSession();
        s.debug = keepDebug;
        s.arrivalOverride = keepArrival;
        s.graceOverride = keepGrace;
    }
    else
        _sessions.erase(it);
}

void AutoTravelMgr::Repath(Player* player)
{
    ATSession* s = Find(player);
    if (!s || s->state == AT_IDLE)
    {
        Msg(player, "AutoTravel ist nicht aktiv.");
        return;
    }

    if (s->IsHandedOver())
    {
        Msg(player, "Der Pfad wird neu berechnet, sobald der Autopilot wieder uebernimmt.");
        return;
    }

    HaltMovement(player, *s);
    s->path.clear();
    s->idx = 0;
    s->flying = false;
    s->repathAttempts = 0;                 // ein ausdruecklicher Neubeginn: frische Zaehler
    s->stuckStrikes = 0;
    s->state = AT_CALCULATE_PATH;
    Msg(player, "Pfad wird neu berechnet.");
    PushStatus(player, *s);
}

// ---------------------------------------------------------------------------
// Aufraeumen
// ---------------------------------------------------------------------------
//
// Das Modul registriert bewusst kein PlayerScript (siehe AutoTravel_SC.cpp),
// bekommt also kein Ausloggen gemeldet. Die Sitzungen raeumt Update() selbst ab;
// alle uebrigen GUID-Tabellen raeumt dieser Durchlauf alle 30 Sekunden ab.
// Ohne ihn wuerde jeder Spieler, der je eine Route oder einen Handschlag
// geschickt hat, bis zum Neustart des Servers einen Eintrag hinterlassen.

void AutoTravelMgr::SweepOrphans()
{
    auto gone = [](ObjectGuid const& guid)
    {
        return ObjectAccessor::FindConnectedPlayer(guid) == nullptr;
    };

    std::erase_if(_pendingRoutes, [&](auto const& kv) { return gone(kv.first); });
    std::erase_if(_calib,         [&](auto const& kv) { return gone(kv.first); });
    std::erase_if(_addonPlayers,  [&](ObjectGuid const& guid) { return gone(guid); });

    // Zeitstempel zaehlen nur, solange sie wirken. Das gilt auch fuer Spieler,
    // die zwischendurch ausgeloggt haben -- die Abklingzeit soll sich nicht
    // durch Neuanmelden umgehen lassen.
    uint32 const nowSec = uint32(time(nullptr));
    std::erase_if(_tpCooldown, [&](auto const& kv) { return nowSec >= kv.second; });

    // Ein Eintrag der Befehlsbremse ist erst entbehrlich, wenn BEIDE Klassen auch
    // beim groessten Gewicht abgelaufen waeren.
    uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
    uint32 const longest = ATConf.commandCooldownMs * AT_CMD_MAX_WEIGHT;
    std::erase_if(_cmdStamp, [&](auto const& kv)
    {
        for (uint32 stamp : kv.second)
            if (stamp != 0 && uint32(nowMs - stamp) < longest)
                return false;
        return true;
    });
}

void AutoTravelMgr::AbortAllSessions(std::string const& why)
{
    for (auto& kv : _sessions)
    {
        ATSession& s = kv.second;

        // Nur Spieler in der Welt: Wer gerade die Karte wechselt, bekommt die
        // Kontrolle vom Client beim Betreten der neuen Welt zurueck.
        Player* player = ObjectAccessor::FindPlayer(kv.first);
        if (!player)
            continue;

        if (s.state != AT_IDLE || s.controlTaken)
        {
            HaltMovement(player, s);
            s.state = AT_IDLE;
            PushStatus(player, s);
            Msg(player, why);
        }
    }
    _sessions.clear();
}

// ---------------------------------------------------------------------------
// Statusmeldung an das Addon
// ---------------------------------------------------------------------------
//
//   [AT]S|<zustand>|<restdistanz>|<ziel>|<flags>|<pfadpunkte>|<versuche>|<etappe>|<etappen>|<fortschritt>
//
// flags: 1 = beritten, 2 = fliegt, 4 = schwimmt, 8 = Server steuert,
//        16 = vom Spieler pausiert

// Restentfernung und Fortschritt fuer die Statuszeile.
//
// Auf einer Karte ist beides die Luftlinie zum Endziel. Ueber eine Kartengrenze
// hinweg sind Koordinaten nicht vergleichbar: dann zaehlt die Strecke bis zur
// aktuellen Etappe (oder bis zum Endziel, sobald der Spieler auf dessen Karte
// ist), und der Fortschritt kommt aus der Zahl der Etappen.
void AutoTravelMgr::RemainingForStatus(Player* player, ATSession const& s,
                                       float& dist, uint32& progress) const
{
    dist = 0.0f;
    progress = 0;

    bool const crossMap = s.finalMapId != AT_NO_MAP && s.startMapId != AT_NO_MAP
                          && s.finalMapId != s.startMapId;

    if (!crossMap)
    {
        dist = player->GetExactDist2d(s.finalX ? s.finalX : s.destX,
                                      s.finalX ? s.finalY : s.destY);
        if (s.startDistance > 1.0f)
        {
            float frac = 1.0f - (dist / s.startDistance);
            progress = uint32(std::max(0.0f, std::min(1.0f, frac)) * 100.0f);
        }
        return;
    }

    if (s.finalMapId == player->GetMapId())
        dist = player->GetExactDist2d(s.finalX, s.finalY);
    else if (s.mapId == player->GetMapId() && (s.destX != 0.0f || s.destY != 0.0f))
        dist = player->GetExactDist2d(s.destX, s.destY);

    if (!s.route.empty())
        progress = uint32(std::min<size_t>(99, (s.legIdx * 100) / s.route.size()));
}

void AutoTravelMgr::PushStatus(Player* player, ATSession& s)
{
    if (!player || !player->GetSession())
        return;

    float dist = 0.0f;
    uint32 progress = 0;
    RemainingForStatus(player, s, dist, progress);

    uint32 flags = 0;
    if (player->IsMounted())  flags |= 1;
    if (s.flying)             flags |= 2;
    if (s.swimming)           flags |= 4;
    if (s.controlTaken)       flags |= 8;
    if (s.pausedByPlayer)     flags |= 16;

    char buf[512];
    std::snprintf(buf, sizeof(buf), "[AT]S|%s|%.0f|%s|%u|%u|%u|%u|%u|%u",
                  ATStateName(s.state),
                  dist,
                  s.destName.empty() ? "-" : s.destName.c_str(),
                  flags,
                  uint32(s.path.size()),
                  std::max(s.repathAttempts, s.stuckStrikes),
                  uint32(s.legIdx + 1),
                  uint32(s.route.size()),
                  progress);

    Raw(player, buf);
}

void AutoTravelMgr::SyncStatus(Player* player)
{
    ATSession* s = Find(player);
    if (s && s->state != AT_IDLE)
        PushStatus(player, *s);
    else
        Raw(player, "[AT]S|IDLE|0|-|0|0|0|0|0|0");
}

void AutoTravelMgr::PrintStatus(Player* player)
{
    ATSession* s = Find(player);
    if (!s || s->state == AT_IDLE)
    {
        SyncStatus(player);
        Msg(player, "Status: bereit.");
        return;
    }

    char buf[288];
    std::snprintf(buf, sizeof(buf),
                  "Status: %s | Ziel: %s | Etappe %u/%u | Pfadpunkte %u/%u | Versuche %u",
                  ATStateName(s->state), s->destName.c_str(),
                  uint32(s->legIdx + 1), uint32(s->route.size()),
                  uint32(s->idx), uint32(s->path.size()),
                  std::max(s->repathAttempts, s->stuckStrikes));
    Msg(player, buf);
    PushStatus(player, *s);
}

// ---------------------------------------------------------------------------
// Teleport
// ---------------------------------------------------------------------------
//
// Bewusst getrennt vom Reisebetrieb: der Teleport umgeht jede Wegfindung und
// nimmt beliebige Zielkoordinaten entgegen. Die Rechtepruefung dafuer sitzt im
// Befehlsteil (AutoTravel_SC.cpp), nicht hier.

void AutoTravelMgr::Teleport(Player* player, uint32 uiMapId, float nx, float ny,
                             bool hasCalib, float pnx, float pny, std::string const& name)
{
    if (!ATConf.enable)
    {
        Msg(player, "AutoTravel ist auf diesem Server deaktiviert.");
        return;
    }
    if (!ATConf.allowTeleport)
    {
        Msg(player, "Teleport ist auf diesem Server deaktiviert.");
        return;
    }
    if (player->IsInCombat())
    {
        Msg(player, "Im Kampf ist kein Teleport moeglich.");
        return;
    }
    if (!player->IsAlive())
    {
        Msg(player, "Du bist tot.");
        return;
    }
    if (player->IsInFlight() || player->GetVehicle() || player->IsBeingTeleported())
    {
        Msg(player, "Jetzt gerade nicht moeglich (Flug, Fahrzeug oder Teleport laeuft).");
        return;
    }
    if (player->GetMap()->IsBattlegroundOrArena() || player->InBattleground())
    {
        Msg(player, "In Schlachtfeldern und Arenen nicht erlaubt.");
        return;
    }

    uint32 nowSec = uint32(time(nullptr));
    auto cd = _tpCooldown.find(player->GetGUID());
    if (cd != _tpCooldown.end() && nowSec < cd->second)
    {
        char b[96];
        std::snprintf(b, sizeof(b), "Noch %u Sekunden Abklingzeit.", cd->second - nowSec);
        Msg(player, b);
        return;
    }

    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint32 mapId = 0;
    std::string err;
    if (!ResolveWorld(player, uiMapId, nx, ny, hasCalib, pnx, pny, x, y, z, mapId, err))
    {
        Msg(player, err);
        return;
    }

    float dist = player->GetExactDist2d(x, y);
    if (ATConf.teleportMinDist > 0.0f && dist < ATConf.teleportMinDist)
    {
        char b[160];
        std::snprintf(b, sizeof(b), "Das Ziel ist nur %.0f yd entfernt - lauf hin.", dist);
        Msg(player, b);
        return;
    }

    // Laufende Reise sauber beenden, damit Spline und Teleport sich nicht
    // gegenseitig ins Gehege kommen.
    auto it = _sessions.find(player->GetGUID());
    if (it != _sessions.end())
    {
        HaltMovement(player, it->second);
        _sessions.erase(it);
    }

    // Abklingzeit und Erfolgsmeldung nur, wenn der Core den Teleport annimmt. Er
    // lehnt ihn z. B. fuer ungueltige Koordinaten oder einen Spieler im Ladevorgang
    // ab; wer dann trotzdem warten muss und "Teleport zu ..." liest, wurde nicht
    // teleportiert.
    if (!player->TeleportTo(mapId, x, y, z + 0.5f, player->GetOrientation()))
    {
        Msg(player, "Der Teleport wurde vom Server abgelehnt.");
        SyncStatus(player);
        return;
    }
    _tpCooldown[player->GetGUID()] = nowSec + ATConf.teleportCooldown;
    SyncStatus(player);                    // die laufende Reise (falls eine lief) ist beendet

    std::string shown = AT::SanitizeText(name, 48);
    if (shown.empty())
        shown = "Ziel";

    char buf[224];
    std::snprintf(buf, sizeof(buf), "Teleport zu %s (%.1f / %.1f / %.1f), %.0f yd.",
                  shown.c_str(), x, y, z, dist);
    Msg(player, buf);
}

// ---------------------------------------------------------------------------
// Diagnose
// ---------------------------------------------------------------------------

void AutoTravelMgr::Diagnose(Player* player, uint32 uiMapId, float nx, float ny,
                             bool hasCalib, float pnx, float pny)
{
    char b[288];

    Msg(player, "--- AutoTravel Diagnose ---");

    std::snprintf(b, sizeof(b), "mmaps aktiv: %s | WorldMapArea geladen: %s | Reiseknoten: %u",
                  sWorld->getBoolConfig(CONFIG_ENABLE_MMAPS) ? "ja" : "NEIN",
                  sATMapAreasLoaded ? "ja" : "NEIN",
                  uint32(NodeCount()));
    Msg(player, b);

    std::snprintf(b, sizeof(b), "Spieler: Map %u Zone %u | %.1f / %.1f / %.1f | %s%s",
                  player->GetMapId(), player->GetZoneId(),
                  player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                  player->IsOutdoors() ? "draussen" : "drinnen",
                  player->IsMounted() ? (player->CanFly() ? ", flugfaehig" : ", beritten") : "");
    Msg(player, b);

    std::string fixTxt = "noch nicht gelernt";
    auto cit = _calib.find(player->GetGUID());
    if (cit != _calib.end())
    {
        auto fix = cit->second.fix.find(uiMapId);
        if (fix != cit->second.fix.end())
            fixTxt = std::to_string(fix->second);
    }
    std::snprintf(b, sizeof(b), "Karten-ID %u -> WorldMapArea %s | Gegenprobe: %s",
                  uiMapId, fixTxt.c_str(), hasCalib ? "vorhanden" : "fehlt");
    Msg(player, b);

    float x, y, z;
    uint32 mapId;
    std::string err;
    if (!ResolveWorld(player, uiMapId, nx, ny, hasCalib, pnx, pny, x, y, z, mapId, err))
    {
        Msg(player, "Zielaufloesung fehlgeschlagen: " + err);
        return;
    }

    std::snprintf(b, sizeof(b), "Ziel: %.1f / %.1f / %.1f | Entfernung %.0f yd",
                  x, y, z, player->GetExactDist2d(x, y));
    Msg(player, b);

    Map* map = player->GetMap();
    uint32 phase = player->GetPhaseMask();
    float terrain = map->GetHeight(phase, x, y, MAX_HEIGHT);
    if (terrain > INVALID_HEIGHT)
        std::snprintf(b, sizeof(b), "Rohgelaende dort: %.2f (Ziel-Z %.2f)", terrain, z);
    else
        std::snprintf(b, sizeof(b), "Rohgelaende dort: keine Angabe - an dieser Stelle ist kein Boden.");
    Msg(player, b);

    std::vector<float> planes;
    FindGroundPlanes(player, x, y, z, planes);
    if (planes.empty())
        Msg(player, "Keine begehbare Flaeche in der Naehe der Zielhoehe.");
    else
    {
        std::string list;
        for (float p : planes)
        {
            char t[32];
            std::snprintf(t, sizeof(t), "%.1f ", p);
            list += t;
        }
        Msg(player, "Gefundene Etagen: " + list);
    }

    float zc[6];
    uint8 zn = 0;
    auto addZ = [&](float v)
    {
        if (v <= INVALID_HEIGHT || zn >= 6)
            return;
        for (uint8 i = 0; i < zn; ++i)
            if (std::fabs(zc[i] - v) < 1.5f)
                return;
        zc[zn++] = v;
    };

    float pz = player->GetPositionZ();
    addZ(z);
    addZ(terrain);
    addZ(map->GetHeight(phase, x, y, pz + 5.0f,   true, 400.0f));
    addZ(map->GetHeight(phase, x, y, pz + 40.0f,  true, 400.0f));
    addZ(map->GetHeight(phase, x, y, pz + 120.0f, true, 400.0f));
    addZ(map->GetHeight(phase, x, y, pz + 300.0f, true, 400.0f));

    if (zn == 0)
    {
        Msg(player, "Keine gueltige Hoehe an dieser Stelle - dort ist kein Boden.");
        return;
    }

    bool done = false;
    for (uint8 pass = 0; pass < 2 && !done; ++pass)
    {
        for (uint8 i = 0; i < zn; ++i)
        {
            ATPathResult r;
            bool ok = TryPathBetween(player,
                                     player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                                     x, y, zc[i], pass == 1, r);
            std::snprintf(b, sizeof(b), "  %-10s Z %8.2f -> 0x%X (%s), %u Punkte %s",
                          pass == 1 ? "Eckpunkte" : "geglaettet",
                          zc[i], r.type, AT::PathTypeName(r.type).c_str(),
                          uint32(r.points.size()), ok ? "AKZEPTIERT" : "verworfen");
            Msg(player, b);
            if (ok)
            {
                done = true;
                break;
            }
        }
    }

    if (!done)
        Msg(player, "Kein Kandidat war brauchbar. Fehlen fuer diese Kartenkachel die mmaps?");
}
