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
            float distance   = f[3].Get<float>();
            float extra      = f[4].Get<float>();

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
// A* auf dem Reisegraphen
// ---------------------------------------------------------------------------
// Vorher lief hier Dijkstra: der breitet sich gleichmaessig in ALLE Richtungen
// aus und sieht sich halb Azeroth an, bevor er ein Ziel 300 Yards weiter
// findet. A* schaetzt zusaetzlich die Restentfernung und arbeitet damit
// gerichtet auf das Ziel zu -- dasselbe Prinzip wie bei Kartendiensten.
//
//     f(n) = g(n)            + h(n)
//            bisherige Kosten  Luftlinie zum Ziel
//
// Damit A* denselben Weg findet wie Dijkstra, muss die Schaetzung
// ZULAESSIG sein: sie darf die echten Restkosten nie ueberschaetzen. Das ist
// hier erfuellt, weil die Kantenkosten aus Weglaengen stammen und ein Weg
// niemals kuerzer als die Luftlinie ist.
//
// Zwei Faelle brechen das und werden abgefangen:
//
//   1. Andere Karte -- eine Luftlinie zwischen zwei Karten ist bedeutungslos.
//      Dort ist h = 0, A* verhaelt sich wie Dijkstra.
//   2. Sonderverbindungen (Flug, Portal) tragen einen Aufschlag, sind also
//      teurer als ihre Laenge. Das macht die Schaetzung nur vorsichtiger,
//      nicht ungueltig.
//
// AutoTravel.HeuristicWeight > 1.0 sucht schneller, kann aber einen etwas
// laengeren Weg liefern. Standard 1.0 = kuerzester Weg garantiert.

bool AutoTravelMgr::BuildNodeRoute(Player* player, float dx, float dy, float /*dz*/,
                                   std::vector<ATLeg>& out, std::string& note) const
{
    out.clear();

    if (!sNodesLoaded)
    {
        note = "keine TravelNodes geladen";
        return false;
    }

    uint32 mapId = player->GetMapId();
    float dStart = 0.0f, dEnd = 0.0f;

    uint32 startNode = NearestNode(mapId, player->GetPositionX(), player->GetPositionY(),
                                   ATConf.nodeSearchRadius, &dStart);
    uint32 endNode   = NearestNode(mapId, dx, dy, ATConf.nodeSearchRadius, &dEnd);

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

    ATNode const& goal = sNodes[endNode];
    float const w = ATConf.heuristicWeight;

    // Zulaessige Schaetzung der Restkosten
    auto heuristic = [&](ATNode const& n) -> float
    {
        if (n.mapId != goal.mapId)
            return 0.0f;                       // andere Karte: keine Aussage moeglich
        return Dist2D(n.x, n.y, goal.x, goal.y) * w;
    };

    std::unordered_map<uint32, float> gScore;
    std::unordered_map<uint32, uint32> prev;
    std::unordered_map<uint32, uint8> prevType;
    std::unordered_map<uint32, bool> closed;

    typedef std::pair<float, uint32> QE;        // (f, Knoten)
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;

    gScore[startNode] = 0.0f;
    open.push(QE(heuristic(sNodes[startNode]), startNode));

    uint32 expanded = 0;
    bool found = false;

    while (!open.empty())
    {
        QE cur = open.top();
        open.pop();

        if (cur.second == endNode)
        {
            found = true;
            break;
        }

        if (closed[cur.second])
            continue;
        closed[cur.second] = true;

        if (++expanded > 40000)
            break;

        auto gIt = gScore.find(cur.second);
        if (gIt == gScore.end())
            continue;
        float g = gIt->second;

        auto lIt = sLinks.find(cur.second);
        if (lIt == sLinks.end())
            continue;

        for (ATNodeLink const& l : lIt->second)
        {
            if (closed[l.to])
                continue;

            float ng = g + l.cost;
            auto old = gScore.find(l.to);
            if (old == gScore.end() || ng < old->second)
            {
                gScore[l.to]   = ng;
                prev[l.to]     = cur.second;
                prevType[l.to] = l.type;
                open.push(QE(ng + heuristic(sNodes[l.to]), l.to));
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

    // Der Startknoten liegt oft hinter dem Spieler - dann ueberspringen.
    if (chain.size() > 1)
    {
        ATNode const& n0 = sNodes[chain[0]];
        ATNode const& n1 = sNodes[chain[1]];
        float toStart = Dist2D(player->GetPositionX(), player->GetPositionY(), n0.x, n0.y);
        float toNext  = Dist2D(player->GetPositionX(), player->GetPositionY(), n1.x, n1.y);
        if (toNext < toStart)
            chain.erase(chain.begin());
    }

    // --- In Etappen umwandeln ---------------------------------------------
    for (size_t i = 0; i < chain.size(); ++i)
    {
        ATNode const& n = sNodes[chain[i]];

        ATLeg leg;
        leg.wx = n.x;
        leg.wy = n.y;
        leg.wz = n.z;
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
                leg.flags |= AT_LEG_SPECIAL;
        }

        out.push_back(leg);
    }

    char b[224];
    std::snprintf(b, sizeof(b),
                  "%u Knoten, %u geprueft, Start %.0f yd entfernt, Ziel %.0f yd vom letzten Knoten",
                  uint32(out.size()), expanded, dStart, dEnd);
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
