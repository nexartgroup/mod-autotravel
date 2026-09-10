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
#include "Player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

    std::string const& db = ATNodeDb;

    std::string sql = "SELECT id, map_id, x, y, z, name FROM `" + db + "`.`playerbots_travelnode`";
    QueryResult res = WorldDatabase.Query(sql.c_str());
    if (!res)
    {
        LOG_INFO("server.loading",
                 "mod-autotravel: Keine Reiseknoten gefunden (Datenbank '{}'). "
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

    sql = "SELECT node_id, to_node_id, type, distance, extra_cost FROM `" + db +
          "`.`playerbots_travelnode_link`";
    QueryResult lres = WorldDatabase.Query(sql.c_str());

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

            if (l.type != 1)
            {
                if (!ATConf.useSpecialLinks)
                    continue;

                // Sonderverbindungen kosten extra, damit sie nur benutzt
                // werden, wenn sie wirklich viel Strecke sparen.
                extra += ATConf.specialLinkCost;
            }

            l.cost = distance + extra;
            if (l.cost <= 0.0f)
                l.cost = 1.0f;

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
    uint32 NearestNode(uint32 mapId, float x, float y, float radius, float* outDist)
    {
        uint32 best = 0;
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
            *outDist = best ? bestDist : -1.0f;
        return best;
    }
}

// ---------------------------------------------------------------------------
// A* mit gesperrten Kanten
// ---------------------------------------------------------------------------
//
// Dijkstra breitet sich gleichmaessig in alle Richtungen aus und besucht dabei
// Zehntausende Knoten. A* mit Luftlinien-Schaetzung laeuft auf das Ziel zu.
//
// Die Schaetzung ist zulaessig (unterschaetzt nie), weil die Kantenkosten aus
// Weglaengen stammen und ein Weg nie kuerzer als die Luftlinie ist. Bei Knoten
// auf ANDEREN Karten ist eine Luftlinie bedeutungslos -- dort ist die
// Schaetzung 0 und A* verhaelt sich wie Dijkstra.
//
// Wichtig: die Warteschlange enthaelt f = g + h, verglichen werden muss aber
// gegen g. Ohne diese Trennung waehlt A* falsche Wege.
//
// Neu ist die Sperrliste. Der Graph kennt Flugverbindungen, die ein konkreter
// Charakter gar nicht benutzen kann, weil ihm der Flugpunkt fehlt. Solche
// Kanten fallen erst auf, wenn die Route steht -- dann werden sie gesperrt und
// die Suche laeuft noch einmal.

namespace
{
    inline uint64 EdgeId(uint32 a, uint32 b)
    {
        return (uint64(a) << 32) | uint64(b);
    }

    bool SearchChain(uint32 startNode, uint32 endNode,
                     std::unordered_set<uint64> const& banned,
                     std::vector<uint32>& chain,
                     std::unordered_map<uint32, uint8>& prevType,
                     std::string& note)
    {
        chain.clear();
        prevType.clear();

        std::unordered_map<uint32, float> dist;
        std::unordered_set<uint32> closed;
        std::unordered_map<uint32, uint32> prev;

        typedef std::pair<float, uint32> QE;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;

        auto goalIt = sNodes.find(endNode);
        if (goalIt == sNodes.end())
        {
            note = "Zielknoten fehlt";
            return false;
        }
        ATNode const& goalNode = goalIt->second;

        auto heuristic = [&](uint32 n) -> float
        {
            auto it = sNodes.find(n);
            if (it == sNodes.end())
                return 0.0f;
            if (it->second.mapId != goalNode.mapId)
                return 0.0f;
            return AT::Dist2D(it->second.x, it->second.y, goalNode.x, goalNode.y);
        };

        dist[startNode] = 0.0f;
        pq.push(QE(heuristic(startNode), startNode));

        uint32 visited = 0;
        bool found = false;

        while (!pq.empty())
        {
            QE cur = pq.top();
            pq.pop();

            if (cur.second == endNode)
            {
                found = true;
                break;
            }

            if (closed.find(cur.second) != closed.end())
                continue;
            closed.insert(cur.second);

            auto dIt = dist.find(cur.second);
            if (dIt == dist.end())
                continue;

            if (++visited > 40000)
                break;

            auto lIt = sLinks.find(cur.second);
            if (lIt == sLinks.end())
                continue;

            float g = dIt->second;

            for (ATNodeLink const& l : lIt->second)
            {
                if (banned.find(EdgeId(cur.second, l.to)) != banned.end())
                    continue;

                float nd = g + l.cost;
                auto old = dist.find(l.to);
                if (old == dist.end() || nd < old->second)
                {
                    dist[l.to] = nd;
                    prev[l.to] = cur.second;
                    prevType[l.to] = l.type;
                    pq.push(QE(nd + heuristic(l.to), l.to));
                }
            }
        }

        if (!found)
        {
            note = "kein Weg im Knotengraphen";
            return false;
        }

        uint32 at = endNode;
        while (true)
        {
            chain.push_back(at);
            if (at == startNode)
                break;

            auto p = prev.find(at);
            if (p == prev.end())
            {
                note = "Rueckverfolgung unterbrochen";
                return false;
            }
            at = p->second;

            if (chain.size() > 400)
            {
                note = "Route unplausibel lang";
                return false;
            }
        }
        std::reverse(chain.begin(), chain.end());
        return true;
    }
}

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
    if (!destMap)
        destMap = startMap;

    float dStart = 0.0f, dEnd = 0.0f;

    uint32 startNode = NearestNode(startMap, player->GetPositionX(), player->GetPositionY(),
                                   ATConf.nodeSearchRadius, &dStart);
    uint32 endNode = NearestNode(destMap, dx, dy, ATConf.nodeSearchRadius, &dEnd);

    if (!startNode || !endNode)
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

    for (uint8 attempt = 0; attempt < 5; ++attempt)
    {
        if (!SearchChain(startNode, endNode, banned, chain, prevType, note))
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
        uint64 banEdge = 0;

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

                    if (ResolveTaxiHop(player,
                                       n.mapId, n.x, n.y,
                                       nn.mapId, nn.x, nn.y,
                                       fromNode, toNode, cost,
                                       boardMap, boardX, boardY, boardZ,
                                       landMap, landX, landY, landZ))
                    {
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
                        // Diese Verbindung kann der Charakter nicht nehmen.
                        banEdge = EdgeId(chain[i], chain[i + 1]);
                        retry = true;
                        ++flightsRejected;
                        break;
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

        banned.insert(banEdge);
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
    if (!n)
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
