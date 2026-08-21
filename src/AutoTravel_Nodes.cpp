#include "AutoTravel.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Reiseknoten von mod-playerbots
// ---------------------------------------------------------------------------
// mod-playerbots pflegt einen Graphen aus 3781 Knoten mit Verbindungen. Genau
// dieser Graph ist der Grund, warum ein Bot mit "nc +new rpg" von Sturmwind
// aus zum Questgebiet FLIEGT: der Graph kennt Verbindungen, die das NavMesh
// nicht kennen kann -- Flugrouten, Portale, Schiffe.
//
// AutoTravel liest ihn per SQL aus der Playerbot-Datenbank. Bewusst NICHT
// ueber die C++-Schnittstelle von mod-playerbots: das wuerde eine
// Kompilierabhaengigkeit zwischen zwei Modulen erzeugen. Ueber die Datenbank
// bleibt die Kopplung an den Daten, und ohne mod-playerbots faellt AutoTravel
// einfach auf die Carbonite-Route zurueck.
//
// Tabellen (in dieser Reihenfolge geprueft):
//   playerbots_travelnode        id, name, map_id, x, y, z, linked
//   playerbots_travelnode_link   node_id, to_node_id, type, object, distance,
//                                swim_distance, extra_cost, calculated
//
// Die Koordinaten sind bereits Weltkoordinaten. Fuer diese Etappen entfaellt
// die gesamte Karten-ID-Umrechnung samt ihrer Fehlerquellen.

namespace
{
    std::unordered_map<uint32, ATNode> sNodes;
    std::unordered_map<uint32, std::vector<ATNodeLink>> sLinks;
    bool sNodesLoaded = false;

    inline float Dist2D(float ax, float ay, float bx, float by)
    {
        float dx = ax - bx, dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

    char const* LinkTypeName(uint8 t)
    {
        // Typ 1 ist in der gelieferten Datenbank die Laufverbindung. Die
        // uebrigen Werte werden beim Laden gezaehlt und protokolliert, statt
        // sie hier zu erraten.
        switch (t)
        {
            case 1:  return "zu Fuss";
            case 2:  return "Portal";
            case 3:  return "Transport";
            case 4:  return "Flugroute";
            default: return "Sonderverbindung";
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
        LOG_INFO("server.loading", "mod-autotravel: TravelNodes sind per Konfiguration aus.");
        return;
    }

    std::string const db = ATConf.nodeDb;

    std::string sql = "SELECT id, map_id, x, y, z, name FROM `" + db + "`.`playerbots_travelnode`";
    QueryResult res = WorldDatabase.Query(sql.c_str());
    if (!res)
    {
        LOG_INFO("server.loading",
                 "mod-autotravel: Keine TravelNodes gefunden (Datenbank '{}'). "
                 "AutoTravel benutzt weiterhin die Carbonite-Route.", db);
        return;
    }

    do
    {
        Field* f = res->Fetch();
        ATNode n;
        n.id    = f[0].Get<uint32>();
        n.mapId = f[1].Get<uint32>();
        n.x = f[2].Get<float>();
        n.y = f[3].Get<float>();
        n.z = f[4].Get<float>();

        if (!std::isfinite(n.x) ||
            !std::isfinite(n.y) ||
            !std::isfinite(n.z))
        {
            LOG_ERROR(
                "server.loading",
                "mod-autotravel: TravelNode {} verworfen - ungueltige Koordinaten.",
                n.id);

            continue;
        }
        n.name  = f[5].Get<std::string>();
        auto existing = sNodes.find(n.id);

        if (existing != sNodes.end())
        {
            LOG_ERROR(
                "server.loading",
                "mod-autotravel: Doppelte TravelNode-ID {} - "
                "zweiter Eintrag verworfen.",
                n.id);

            continue;
        }

        sNodes.emplace(n.id, std::move(n));
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
            float distance   = f[3].Get<float>();
            float extra      = f[4].Get<float>();
            if (!std::isfinite(distance) ||
                !std::isfinite(extra) ||
                distance < 0.0f ||
                extra < 0.0f)
            {
                LOG_ERROR(
                    "server.loading",
                    "mod-autotravel: Verbindung {} -> {} verworfen - "
                    "ungueltige Kosten.",
                    from,
                    l.to);

                continue;
            }
            if (from == l.to)
{
                LOG_ERROR(
                    "server.loading",
                    "mod-autotravel: Selbstverbindung {} -> {} verworfen.",
                    from,
                    l.to);

                continue;
            }
            if (sNodes.find(from) == sNodes.end() || sNodes.find(l.to) == sNodes.end())
                continue;

            if (l.type != 1)
            {
                if (!ATConf.useSpecialLinks)
                    continue;
                // Sonderverbindungen kosten extra, damit sie nur benutzt
                // werden, wenn sie wirklich viel Strecke sparen -- der Spieler
                // muss dort schliesslich selbst taetig werden.
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

    LOG_INFO("server.loading", "mod-autotravel: {} TravelNodes, {} Verbindungen geladen.",
             uint32(sNodes.size()), linkCount);

    uint32 invalidLinks = 0;

    for (auto const& kv : sLinks)
    {
        if (sNodes.find(kv.first) == sNodes.end())
        {
            ++invalidLinks;
            continue;
        }

        for (ATNodeLink const& link : kv.second)
        {
            if (sNodes.find(link.to) == sNodes.end())
                ++invalidLinks;
        }
    }

    if (invalidLinks)
    {
        LOG_ERROR(
            "server.loading",
            "mod-autotravel: {} ungueltige TravelNode-Verbindungen gefunden.",
            invalidLinks);
    }

    for (auto const& kv : typeCount)
        LOG_INFO("server.loading", "mod-autotravel:   Verbindungstyp {} ({}): {}",
                 uint32(kv.first), LinkTypeName(uint8(kv.first)), kv.second);
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
            float d = Dist2D(x, y, kv.second.x, kv.second.y);
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
// Dijkstra
// ---------------------------------------------------------------------------

bool AutoTravelMgr::BuildNodeRoute(
    Player* player,
    uint32 destinationMapId,
    float dx,
    float dy,
    float dz,
    std::vector<ATLeg>& out,
    std::string& note) const
{
    out.clear();

    if (!sNodesLoaded)
    {
        note = "keine TravelNodes geladen";
        return false;
    }

    uint32 startMapId = player->GetMapId();

    float dStart = 0.0f;
    float dEnd = 0.0f;

    uint32 startNode =
        NearestNode(
            startMapId,
            player->GetPositionX(),
            player->GetPositionY(),
            ATConf.nodeSearchRadius,
            &dStart);

    uint32 endNode =
        NearestNode(
            destinationMapId,
            dx,
            dy,
            ATConf.nodeSearchRadius,
            &dEnd);

    if (!startNode || !endNode)
    {
        note = "kein Knoten in Reichweite";
        return false;
    }
    if (startNode == endNode)
    {
        note = "Start und Ziel am selben Knoten";
        return false;
    }

    // --- Suche -------------------------------------------------------------
    std::unordered_map<uint32, float> dist;
    std::unordered_map<uint32, bool> closed;
    std::unordered_map<uint32, uint32> prev;
    std::unordered_map<uint32, uint8> prevType;

    typedef std::pair<float, uint32> QE;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;

    // A* statt reinem Dijkstra: die geschaetzte Restentfernung lenkt die Suche
    // auf das Ziel zu. Die Schaetzung ist zulaessig (unterschaetzt nie), weil
    // die Kantenkosten aus Weglaengen stammen und ein Weg nie kuerzer als die
    // Luftlinie ist. Bei Knoten auf ANDEREN Karten ist eine Luftlinie
    // bedeutungslos -- dort ist die Schaetzung 0, A* verhaelt sich wie Dijkstra.
    ATNode const& goalNode = sNodes[endNode];
    auto heuristic = [&](uint32 n) -> float
    {
        ATNode const& x = sNodes[n];
        if (x.mapId != goalNode.mapId)
            return 0.0f;
        return Dist2D(x.x, x.y, goalNode.x, goalNode.y);
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

        // cur.first ist f = g + h. Verglichen werden muss gegen g.
        auto dIt = dist.find(cur.second);
        if (dIt == dist.end())
            continue;
        if (closed[cur.second])
            continue;
        closed[cur.second] = true;

        if (++visited > 40000)
            break;

        auto lIt = sLinks.find(cur.second);
        if (lIt == sLinks.end())
            continue;

        for (ATNodeLink const& l : lIt->second)
        {
            float nd = dist[cur.second] + l.cost;
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

    // --- Zurueckverfolgen --------------------------------------------------
    std::vector<uint32> chain;
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

    // --- Umweg am Routenanfang abschneiden ---------------------------------
    // Der naechstgelegene Knoten liegt haeufig HINTER dem Spieler. Wird er
    // stur angelaufen, rennt der Charakter erst in die Gegenrichtung und dreht
    // dann um. Verglichen wird deshalb der Umweg:
    //
    //     ueber n0:  |Spieler->n0| + |n0->n1|
    //     direkt:    |Spieler->n1|
    //
    // Ist der Umweg groesser als SkipDetourFactor, faellt n0 weg. In einer
    // Schleife, weil manchmal mehrere Knoten hinter dem Spieler liegen.
    {
        float px = player->GetPositionX();
        float py = player->GetPositionY();
        uint32 skipped = 0;

        while (chain.size() > 1 && skipped < 4)
        {
            ATNode const& n0 = sNodes[chain[0]];
            ATNode const& n1 = sNodes[chain[1]];

            float viaN0  = Dist2D(px, py, n0.x, n0.y) + Dist2D(n0.x, n0.y, n1.x, n1.y);
            float direct = Dist2D(px, py, n1.x, n1.y);

            if (direct > ATConf.nodeSearchRadius * 1.5f)
                break;
            if (viaN0 <= direct * ATConf.skipDetourFactor)
                break;

            chain.erase(chain.begin());
            ++skipped;
        }
    }

    // --- In Etappen umwandeln ---------------------------------------------
    for (size_t i = 0; i < chain.size(); ++i)
    {
        ATNode const& n = sNodes[chain[i]];

        ATLeg leg;

        leg.mapId = n.mapId;
        leg.wx = n.x;
        leg.wy = n.y;
        leg.wz = n.z;

        leg.resolved = true;
        leg.needsMapArrival = (n.mapId != player->GetMapId());

        leg.name = n.name;
        leg.resolved = true;
        leg.name = n.name;

        // Art der Verbindung zum naechsten Knoten
        if (i + 1 < chain.size())
        {
            uint8 t = 1;

            auto pt = prevType.find(chain[i + 1]);
            if (pt != prevType.end())
                t = pt->second;

            leg.linkType = t;
            leg.nextName = sNodes[chain[i + 1]].name;

            if (t != 1)
            {
                leg.flags |= AT_LEG_SPECIAL;
                leg.policy = AT_LEG_CONNECTION;
            }
            else
            {
                leg.policy = AT_LEG_OPTIONAL;
            }
        }

        out.push_back(leg);
    }

    char b[192];
    std::snprintf(b, sizeof(b), "%u Knoten, Start %.0f yd entfernt, Ziel %.0f yd vom letzten Knoten",
                  uint32(out.size()), dStart, dEnd);
    note = b;
    return true;
}

// ---------------------------------------------------------------------------
// Diagnose
// ---------------------------------------------------------------------------

void AutoTravelMgr::NodeInfo(Player* player)
{
    char b[256];
    std::snprintf(b, sizeof(b), "TravelNodes: %u Knoten geladen, Datenbank '%s'.",
                  uint32(sNodes.size()), ATConf.nodeDb.c_str());
    Msg(player, b);

    if (sNodes.empty())
    {
        Msg(player, "Nichts geladen - Tabellenname oder Datenbankrechte pruefen (Serverlog).");
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

    ATNode const& node = sNodes[n];
    std::snprintf(b, sizeof(b), "Naechster Knoten: #%u '%s', %.0f yd entfernt, %u Verbindungen.",
                  node.id, node.name.c_str(), d,
                  uint32(sLinks.count(n) ? sLinks[n].size() : 0));
    Msg(player, b);
}
