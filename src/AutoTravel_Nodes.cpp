/*
 * AutoTravel_Nodes.cpp
 * ---------------------------------------------------------------------------
 * Der Reiseknotengraph von mod-playerbots.
 *
 * mod-playerbots pflegt einen Graphen aus mehreren tausend Knoten samt
 * Verbindungen. Genau dieser Graph ist der Grund, warum ein Bot von Sturmwind
 * aus zum Questgebiet FLIEGT statt zu laufen: er kennt Verbindungen, die das
 * NavMesh gar nicht kennen kann -- Flugrouten, Portale, Schiffe.
 *
 * AutoTravel liest ihn per SQL. Bewusst NICHT ueber die C++-Schnittstelle von
 * mod-playerbots: das wuerde eine Kompilierabhaengigkeit zwischen zwei Modulen
 * erzeugen. Ueber die Datenbank bleibt die Kopplung an den Daten, und ohne
 * mod-playerbots faellt AutoTravel einfach auf die Carbonite-Route zurueck.
 *
 * Tabellen:
 *   playerbots_travelnode        id, name, map_id, x, y, z, linked
 *   playerbots_travelnode_link   node_id, to_node_id, type, object, distance,
 *                                swim_distance, extra_cost, calculated
 *
 * Die Koordinaten sind bereits Weltkoordinaten. Fuer diese Etappen entfaellt
 * die gesamte Karten-ID-Umrechnung samt ihrer Fehlerquellen.
 */

#include "AutoTravel.h"

#include "Chat.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    std::unordered_map<uint32, ATNode> sNodes;
    std::unordered_map<uint32, std::vector<ATNodeLink>> sLinks;
    bool sNodesLoaded = false;

    ATLegKind KindFromLinkType(uint8 t)
    {
        switch (t)
        {
            case 1:  return AT_LEG_WALK;
            case 2:  return AT_LEG_PORTAL;
            case 3:  return AT_LEG_TRANSPORT;
            case 4:  return AT_LEG_TAXI;
            default: return AT_LEG_MANUAL;
        }
    }
}

size_t AutoTravelMgr::NodeCount() const
{
    return sNodes.size();
}

// ---------------------------------------------------------------------------
// Laden
// ---------------------------------------------------------------------------
//
// ACHTUNG, warum hier erst im information_schema nachgesehen wird:
//
// mod-playerbots ist laut README optional. Fehlt es, fehlen auch seine Tabellen.
// Eine einfache SELECT-Abfrage auf eine fehlende Tabelle liefert aber KEIN
// leeres Ergebnis: MySQLConnection::_HandleMySQLErrno beendet den Core bei
// ER_NO_SUCH_TABLE und ER_BAD_FIELD_ERROR mit ABORT, nach zehn Sekunden
// Wartezeit ("Your database structure is not up to date"). Ein Server ohne
// mod-playerbots waere beim Start abgestuerzt -- genau der Fall, den das Modul
// angeblich auffaengt.
//
// Deshalb: Tabellen UND benoetigte Spalten vorab ueber information_schema
// pruefen (das nie an einer fehlenden Nutzertabelle scheitert) und nur dann
// lesen, wenn alles da ist. Ein anderes Spaltenlayout in einer kuenftigen
// mod-playerbots-Fassung fuehrt so zu einer Warnung statt zu einem Absturz.

namespace
{
    // Liefert die (kleingeschriebenen) Spaltennamen einer Tabelle, leer wenn die
    // Tabelle oder Datenbank nicht existiert. db und table sind geprueft.
    std::unordered_set<std::string> ColumnsOf(std::string const& db, char const* table)
    {
        std::unordered_set<std::string> cols;

        std::string sql =
            "SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = '" + db +
            "' AND TABLE_NAME = '" + table + "'";

        QueryResult res = WorldDatabase.Query(sql);
        if (!res)
            return cols;

        do
        {
            std::string c = res->Fetch()[0].Get<std::string>();
            std::transform(c.begin(), c.end(), c.begin(),
                           [](unsigned char ch) { return char(std::tolower(ch)); });
            cols.insert(std::move(c));
        } while (res->NextRow());

        return cols;
    }

    // true, wenn die Tabelle existiert und alle Spalten hat. Sonst steht in
    // 'why', was fehlt.
    bool TableUsable(std::string const& db, char const* table,
                     std::initializer_list<char const*> required, std::string& why)
    {
        std::unordered_set<std::string> have = ColumnsOf(db, table);
        if (have.empty())
        {
            why = std::string("Tabelle '") + db + "." + table + "' nicht gefunden";
            return false;
        }

        for (char const* c : required)
        {
            if (have.find(c) == have.end())
            {
                why = std::string("Tabelle '") + db + "." + table + "' hat keine Spalte '" + c + "'";
                return false;
            }
        }
        return true;
    }
}

void AutoTravelMgr::LoadTravelNodes()
{
    sNodes.clear();
    sLinks.clear();
    sNodesLoaded = false;

    if (!ATConf.useTravelNodes)
    {
        LOG_INFO("server.loading", "mod-autotravel: Reiseknoten sind per Konfiguration aus.");
        return;
    }

    std::string const& db = ATNodeDb;      // in LoadConfig auf Namenszeichen geprueft

    std::string why;
    if (!TableUsable(db, "playerbots_travelnode",
                     { "id", "map_id", "x", "y", "z", "name" }, why)
        || !TableUsable(db, "playerbots_travelnode_link",
                        { "node_id", "to_node_id", "type", "distance", "extra_cost" }, why))
    {
        LOG_INFO("server.loading",
                 "mod-autotravel: Keine Reiseknoten benutzbar ({}). Ohne mod-playerbots ist das "
                 "normal; AutoTravel benutzt dann die Carbonite-Route. Liegen die Tabellen in einer "
                 "anderen Datenbank, AutoTravel.NodeDatabase anpassen.", why);
        return;
    }

    std::string sql = "SELECT id, map_id, x, y, z, name FROM `" + db + "`.`playerbots_travelnode`";
    QueryResult res = WorldDatabase.Query(sql);
    if (!res)
    {
        LOG_INFO("server.loading",
                 "mod-autotravel: Die Tabelle der Reiseknoten ist leer (Datenbank '{}'). "
                 "AutoTravel benutzt weiterhin die Carbonite-Route.", db);
        return;
    }

    do
    {
        Field* f = res->Fetch();
        ATNode n;
        n.id    = f[0].Get<uint32>();
        n.mapId = f[1].Get<uint32>();
        n.x     = f[2].Get<float>();
        n.y     = f[3].Get<float>();
        n.z     = f[4].Get<float>();
        n.name  = f[5].Get<std::string>();
        sNodes[n.id] = n;
    } while (res->NextRow());

    // Die Spalte "object" (Areatrigger eines Portals, Eintrag eines Transports) ist
    // nicht Pflicht: fehlt sie, bleiben Portale ungeprueft.
    std::string whyObject;
    bool const haveObject = TableUsable(db, "playerbots_travelnode_link",
                                        { "node_id", "to_node_id", "type", "distance",
                                          "extra_cost", "object" }, whyObject);

    sql = std::string("SELECT node_id, to_node_id, type, distance, extra_cost") +
          (haveObject ? ", object" : "") + " FROM `" + db + "`.`playerbots_travelnode_link`";
    QueryResult lres = WorldDatabase.Query(sql);

    uint32 linkCount = 0;
    std::unordered_map<uint32, uint32> typeCount;

    if (lres)
    {
        do
        {
            Field* f = lres->Fetch();
            uint32 from = f[0].Get<uint32>();

            ATNodeLink l;
            l.to   = f[1].Get<uint32>();
            l.type = f[2].Get<uint8>();

            float distance = f[3].Get<float>();
            float extra    = f[4].Get<float>();

            if (sNodes.find(from) == sNodes.end() || sNodes.find(l.to) == sNodes.end())
                continue;

            // Der Aufschlag fuer Sonderverbindungen (SpecialLinkCost) und der
            // Schalter UseSpecialLinks wirken erst bei der Suche. So bleibt der
            // Graph vollstaendig im Speicher, und .at set speciallinks /
            // specialcost kosten keine Datenbankabfrage mehr.
            l.baseCost = distance + extra;
            if (!(l.baseCost > 0.0f))          // auch NaN und negative Werte
                l.baseCost = 1.0f;
            if (haveObject)
                l.object = f[5].Get<uint32>();

            sLinks[from].push_back(l);
            ++linkCount;
            ++typeCount[l.type];
        } while (lres->NextRow());
    }

    sNodesLoaded = !sNodes.empty() && linkCount > 0;

    LOG_INFO("server.loading", "mod-autotravel: {} Reiseknoten, {} Verbindungen geladen.",
             uint32(sNodes.size()), linkCount);

    for (auto const& kv : typeCount)
        LOG_INFO("server.loading", "mod-autotravel:   Verbindungstyp {} ({}): {}",
                 uint32(kv.first), ATLinkTypeName(uint8(kv.first)), kv.second);
}

// ---------------------------------------------------------------------------
// Naechster Knoten
// ---------------------------------------------------------------------------

namespace
{
    // Liefert AT_NO_NODE, wenn kein Knoten im Umkreis liegt. (Die Knoten-ID 0 ist
    // ein echter Knoten -- siehe AT_NO_NODE.)
    uint32 NearestNode(uint32 mapId, float x, float y, float radius, float* outDist)
    {
        uint32 best = AT_NO_NODE;
        float bestDist = radius;

        for (auto const& kv : sNodes)
        {
            if (kv.second.mapId != mapId)
                continue;
            float d = AT::Dist2D(x, y, kv.second.x, kv.second.y);
            if (d < bestDist)
            {
                bestDist = d;
                best = kv.first;
            }
        }

        if (outDist)
            *outDist = (best != AT_NO_NODE) ? bestDist : -1.0f;
        return best;
    }
}

// ---------------------------------------------------------------------------
// Kuerzester Weg
// ---------------------------------------------------------------------------
//
// Die Suche selbst steht als reine Funktion in AutoTravel_Util.cpp
// (AT::ShortestChain) und ist dort als eigenes Programm getestet. Hier kommen nur
// die Regeln dazu, die den Spieler und den Core brauchen: welche Karten durchquert
// werden duerfen, ob ein Flug oder Portal fuer diesen Charakter in Frage kommt.

// ---------------------------------------------------------------------------
// Route aus dem Knotengraphen
// ---------------------------------------------------------------------------

bool AutoTravelMgr::BuildNodeRoute(Player* player, uint32 destMap,
                                   float dx, float dy, float /*dz*/,
                                   std::vector<ATLeg>& out, std::string& note) const
{
    out.clear();

    if (!sNodesLoaded)
    {
        note = "keine Reiseknoten geladen";
        return false;
    }

    uint32 startMap = player->GetMapId();
    if (destMap == AT_NO_MAP)
        destMap = startMap;

    float dStart = 0.0f, dEnd = 0.0f;

    uint32 startNode = NearestNode(startMap, player->GetPositionX(), player->GetPositionY(),
                                   ATConf.nodeSearchRadius, &dStart);
    uint32 endNode = NearestNode(destMap, dx, dy, ATConf.nodeSearchRadius, &dEnd);

    if (startNode == AT_NO_NODE || endNode == AT_NO_NODE)
    {
        note = "kein Knoten in Reichweite";
        return false;
    }
    if (startNode == endNode)
    {
        note = "Start und Ziel liegen am selben Knoten";
        return false;
    }

    // --- Suchen, pruefen, notfalls sperren und erneut suchen ---------------
    //
    // Der Knotengraph von mod-playerbots beschreibt, was es an Verbindungen
    // GIBT -- nicht, was DIESER Charakter benutzen kann. Eine Flugverbindung
    // nuetzt nichts, wenn ihm der Flugpunkt fehlt; ein Charakter der Stufe 1
    // kennt genau einen.
    //
    // Frueher landete so eine Verbindung ungeprueft in der Route. Die Etappe
    // wurde brav angelaufen, der Abflug scheiterte, und AutoTravel meldete
    // "Der Flug kam nicht zustande" -- nachdem es den Charakter zum
    // Flugmeister geschickt hatte.
    //
    // Jetzt wird jede Sonderverbindung der fertigen Kette geprueft. Was der
    // Charakter nicht nehmen kann, wird gesperrt und die Suche laeuft erneut.

    std::unordered_set<uint64> banned;
    std::vector<uint32> chain;
    std::unordered_map<uint32, uint8> prevType;

    uint32 flightsPlanned = 0;
    uint32 flightsRejected = 0;

    // --- Regeln fuer die Suche ---------------------------------------------
    //
    // * Durchquert werden duerfen nur Ostkontinente, Kalimdor und die Tiefenbahn;
    //   Outland, Nordend und Instanzen nur, wenn Start oder Ziel dort liegen (siehe
    //   AT::TRANSIT_MAPS). Ohne das fuehrte Brachland -> Sturmwind ueber den Zeppelin
    //   nach Nordend und ein Schiff zurueck: billig im Graphen, unbrauchbar im Spiel.
    // * Flugverbindungen, deren Enden dieser Charakter nicht kennt, scheiden schon in
    //   der Suche aus. Frueher wurde nur die erste unbrauchbare Verbindung je Suche
    //   gesperrt und nach fuenf Suchen aufgegeben: bei wenigen bekannten Flugpunkten
    //   (am mitgelieferten Graphen oft 15 bis 60 Sperren noetig) scheiterte die
    //   Planung, vor allem ueber eine Kartengrenze, wo es keinen Ausweichweg gibt.
    // * Zeppeline fahren nur die Horde, Schiffe nur die Allianz mit (AT::TransportFaction).
    // * Ein Portal zaehlt nur, wenn sein Areatrigger in dieser Datenbank ein
    //   Teleportziel hat. Der Graph stammt aus mod-playerbots; ein Trigger, den der
    //   Server nicht kennt, liesse den Charakter vor einem toten Portal warten.
    AT::ChainRules rules;
    rules.specialLinkCost = ATConf.specialLinkCost;
    rules.useSpecialLinks = ATConf.useSpecialLinks;
    for (uint32 m : AT::TRANSIT_MAPS)
        rules.allowedMaps.insert(m);
    rules.allowedMaps.insert(startMap);
    rules.allowedMaps.insert(destMap);
    rules.allowedMaps.insert(player->GetMapId());

    rules.linkUsable = [&](uint32 from, ATNodeLink const& l) -> bool
    {
        if (l.type == 4)
        {
            auto a = sNodes.find(from);
            auto b = sNodes.find(l.to);
            if (a == sNodes.end() || b == sNodes.end())
                return false;
            return TaxiHopPlausible(player,
                                    a->second.mapId, a->second.x, a->second.y,
                                    b->second.mapId, b->second.x, b->second.y);
        }
        if (l.type == 2 && l.object)
            return sObjectMgr->GetAreaTriggerTeleport(l.object) != nullptr;
        if (l.type == 3 && l.object)
        {
            // Zeppeline der Horde, Schiffe der Allianz: nur fuer die eigene Fraktion
            uint8 const f = AT::TransportFaction(l.object);
            if (f == 1 && player->GetTeamId() != TEAM_ALLIANCE)
                return false;
            if (f == 2 && player->GetTeamId() != TEAM_HORDE)
                return false;
        }
        return true;
    };

    // Was die Vorpruefung nicht erkennt (Geld, keine Flugkette zwischen den
    // bekannten Punkten), faellt erst bei der Umwandlung auf. Alle solchen
    // Verbindungen einer Kette werden zusammen gesperrt; die Suche selbst ist billig.
    for (uint8 attempt = 0; attempt < 24; ++attempt)
    {
        if (!AT::ShortestChain(sNodes, sLinks, startNode, endNode, banned, rules,
                               chain, prevType, note))
            return false;

        // --- Umweg am Routenanfang abschneiden -----------------------------
        //
        // Der naechstgelegene Knoten liegt haeufig HINTER dem Spieler. Wird er
        // stur angelaufen, rennt der Charakter erst in die Gegenrichtung und
        // dreht dann um. Verglichen wird deshalb der tatsaechliche Umweg:
        //
        //     ueber n0:  |Spieler->n0| + |n0->n1|
        //     direkt:    |Spieler->n1|
        {
            float px = player->GetPositionX();
            float py = player->GetPositionY();
            uint32 skipped = 0;

            while (chain.size() > 2 && skipped < 4)
            {
                auto i0 = sNodes.find(chain[0]);
                auto i1 = sNodes.find(chain[1]);
                if (i0 == sNodes.end() || i1 == sNodes.end())
                    break;

                ATNode const& n0 = i0->second;
                ATNode const& n1 = i1->second;

                // Nur vergleichbar, solange beide auf derselben Karte liegen.
                if (n0.mapId != startMap || n1.mapId != startMap)
                    break;

                float viaN0  = AT::Dist2D(px, py, n0.x, n0.y) + AT::Dist2D(n0.x, n0.y, n1.x, n1.y);
                float direct = AT::Dist2D(px, py, n1.x, n1.y);

                if (direct > ATConf.nodeSearchRadius * 1.5f)
                    break;
                if (viaN0 <= direct * ATConf.skipDetourFactor)
                    break;

                chain.erase(chain.begin());
                ++skipped;
            }
        }

        // --- In Etappen umwandeln, Sonderverbindungen dabei pruefen --------
        out.clear();
        flightsPlanned = 0;

        bool retry = false;
        std::vector<uint64> banEdges;
        uint64 moneyLeft = player->GetMoney();     // Fluege der Kette zusammen, nicht einzeln

        for (size_t i = 0; i < chain.size(); ++i)
        {
            auto ni = sNodes.find(chain[i]);
            if (ni == sNodes.end())
                continue;
            ATNode const& n = ni->second;

            ATLeg leg;
            leg.mapId = n.mapId;
            leg.wx = n.x;
            leg.wy = n.y;
            leg.wz = n.z;
            leg.resolved = true;
            leg.name = n.name;
            leg.kind = AT_LEG_WALK;

            if (i + 1 < chain.size())
            {
                uint8 t = 1;
                auto pt = prevType.find(chain[i + 1]);
                if (pt != prevType.end())
                    t = pt->second;

                leg.kind = KindFromLinkType(t);

                auto nx = sNodes.find(chain[i + 1]);
                if (nx != sNodes.end())
                    leg.nextName = nx->second.name;

                // --- Flugverbindung: gegen die echten Flugpunkte pruefen ----
                if (leg.kind == AT_LEG_TAXI && nx != sNodes.end())
                {
                    ATNode const& nn = nx->second;

                    uint32 fromNode = 0, toNode = 0, cost = 0;
                    float boardX = 0.0f, boardY = 0.0f, boardZ = 0.0f;
                    float landX = 0.0f, landY = 0.0f, landZ = 0.0f;
                    uint32 boardMap = 0, landMap = 0;

                    // ResolveTaxiHop prueft das Geld nur fuer diesen einen Flug; hier
                    // zaehlen alle Fluege der Kette zusammen.
                    if (ResolveTaxiHop(player,
                                       n.mapId, n.x, n.y,
                                       nn.mapId, nn.x, nn.y,
                                       fromNode, toNode, cost,
                                       boardMap, boardX, boardY, boardZ,
                                       landMap, landX, landY, landZ)
                        && uint64(cost) <= moneyLeft)
                    {
                        moneyLeft -= cost;
                        leg.taxiFrom = fromNode;
                        leg.taxiTo   = toNode;
                        leg.taxiCost = cost;

                        // Auf die Position des echten Flugmeisters ruecken.
                        // Der Core laesst den Abflug nur innerhalb von zwei
                        // Interaktionsdistanzen zu -- der Graphknoten liegt
                        // meist daneben.
                        leg.mapId = boardMap;
                        leg.wx = boardX;
                        leg.wy = boardY;
                        leg.wz = boardZ;
                        ++flightsPlanned;
                    }
                    else
                    {
                        // Diese Verbindung kann der Charakter nicht nehmen. Weiter
                        // pruefen: so werden alle schlechten Fluege der Kette auf
                        // einmal gesperrt.
                        banEdges.push_back(AT::EdgeId(chain[i], chain[i + 1]));
                        retry = true;
                        ++flightsRejected;
                    }
                }
            }

            out.push_back(leg);
        }

        if (!retry)
        {
            char b[288];
            std::snprintf(b, sizeof(b),
                          "%u Knoten, Start %.0f yd entfernt, Ziel %.0f yd vom letzten Knoten"
                          "%s%u Flug(e) geplant%s",
                          uint32(out.size()), dStart, dEnd,
                          flightsPlanned ? ", " : "",
                          flightsPlanned,
                          flightsRejected ? " (nicht nutzbare Fluege uebersprungen)" : "");
            if (!flightsPlanned)
                std::snprintf(b, sizeof(b),
                              "%u Knoten, Start %.0f yd entfernt, Ziel %.0f yd vom letzten Knoten%s",
                              uint32(out.size()), dStart, dEnd,
                              flightsRejected ? " (nicht nutzbare Fluege uebersprungen)" : "");
            note = b;
            return true;
        }

        for (uint64 e : banEdges)
            banned.insert(e);
    }

    note = "zu viele nicht nutzbare Verbindungen im Knotengraphen";
    out.clear();
    return false;
}

// ---------------------------------------------------------------------------
// Diagnose
// ---------------------------------------------------------------------------

void AutoTravelMgr::NodeInfo(Player* player)
{
    char b[288];
    std::snprintf(b, sizeof(b), "Reiseknoten: %u geladen, Datenbank '%s'.",
                  uint32(sNodes.size()), ATNodeDb.c_str());
    Msg(player, b);

    if (sNodes.empty())
    {
        Msg(player, "Nichts geladen - Tabellenname oder Datenbankrechte pruefen (siehe Serverlog).");
        return;
    }

    float d = 0.0f;
    uint32 n = NearestNode(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                           ATConf.nodeSearchRadius, &d);
    if (n == AT_NO_NODE)
    {
        std::snprintf(b, sizeof(b), "Kein Knoten innerhalb von %.0f yd.", ATConf.nodeSearchRadius);
        Msg(player, b);
        return;
    }

    ATNode const& node = sNodes.find(n)->second;
    size_t links = sLinks.count(n) ? sLinks.find(n)->second.size() : 0;

    std::snprintf(b, sizeof(b), "Naechster Knoten: #%u '%s', %.0f yd entfernt, %u Verbindungen.",
                  node.id, node.name.c_str(), d, uint32(links));
    Msg(player, b);
}

// ---------------------------------------------------------------------------
// Gesundheitsbericht (.at stats, nur Spielleiter)
// ---------------------------------------------------------------------------

void AutoTravelMgr::PrintStats(Player* player)
{
    char b[320];

    Msg(player, "--- AutoTravel Gesundheitsbericht ---");

    // Sitzungen nach Zustand
    std::unordered_map<uint32, uint32> byState;
    for (auto const& kv : _sessions)
        ++byState[uint32(kv.second.state)];

    std::string states;
    for (auto const& kv : byState)
    {
        if (kv.first == AT_IDLE)
            continue;
        states += std::string(ATStateName(ATState(kv.first))) + "=" + std::to_string(kv.second) + " ";
    }

    std::snprintf(b, sizeof(b), "Sitzungen: %u gesamt | aktiv: %s",
                  uint32(_sessions.size()), states.empty() ? "keine" : states.c_str());
    Msg(player, b);

    std::snprintf(b, sizeof(b), "Reisen seit Start: %llu begonnen, %llu angekommen, %llu fehlgeschlagen",
                  (unsigned long long)_statTravelsStarted, (unsigned long long)_statTravelsArrived,
                  (unsigned long long)_statTravelsFailed);
    Msg(player, b);

    std::snprintf(b, sizeof(b), "Wegberechnungen: %llu, davon %llu Mal wegen des Budgets (%u je Takt) verschoben",
                  (unsigned long long)_statPathCalcs, (unsigned long long)_statPathDeferred,
                  ATConf.maxPathsPerTick);
    Msg(player, b);

    std::snprintf(b, sizeof(b), "Befehlsbremse: %llu Befehle abgewiesen (Abstand %u ms)",
                  (unsigned long long)_statCmdThrottled, ATConf.commandCooldownMs);
    Msg(player, b);

    std::snprintf(b, sizeof(b),
                  "Speicher: %u Spieler mit Addon, %u gemerkte Routen, %u Kartenzuordnungen, "
                  "%u Teleport-Abklingzeiten",
                  uint32(_addonPlayers.size()), uint32(_pendingRoutes.size()),
                  uint32(_calib.size()), uint32(_tpCooldown.size()));
    Msg(player, b);

    size_t links = 0;
    for (auto const& kv : sLinks)
        links += kv.second.size();
    std::snprintf(b, sizeof(b), "Reiseknoten: %u Knoten, %u Verbindungen (Datenbank '%s')",
                  uint32(sNodes.size()), uint32(links), ATNodeDb.c_str());
    Msg(player, b);
}

