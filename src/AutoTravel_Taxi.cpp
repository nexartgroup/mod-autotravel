/*
 * AutoTravel_Taxi.cpp
 * ---------------------------------------------------------------------------
 * Flugmeister: Planung und Start eines Taxifluges durch den Server selbst.
 *
 * Warum serverseitig: ein Addon darf in 3.3.5a den Flugmeister nicht anklicken
 * (InteractUnit ist protected), und TakeTaxiNode() braucht ein bereits offenes
 * Flugmeisterfenster. Der Server dagegen kann den Flug direkt starten -- mit
 * allen normalen Pruefungen: Flugpunkt bekannt, Fraktion passt, Geld reicht.
 *
 * Datenquellen aus dem Core:
 *
 *   sTaxiNodesStore        alle Flugpunkte mit Karte, Position und Fraktion
 *   sTaxiPathSetBySource   welche Punkte direkt verbunden sind, und zu welchem
 *                          Preis
 *   player->m_taxi         welche Punkte dieser Charakter kennt
 *
 * Aus diesen drei Angaben wird ein kleiner Graph gebaut und mit Dijkstra
 * durchsucht. Mehrere Zwischenstationen sind ausdruecklich erlaubt -- genau so
 * funktioniert Fliegen im Spiel auch, wenn man es von Hand macht.
 *
 * Der Flug wird nur vorgeschlagen, wenn er sich lohnt: er muss einen
 * einstellbaren Anteil der Laufstrecke sparen und darf eine Preisgrenze nicht
 * ueberschreiten. Beides steht in der Konfiguration.
 */

#include "AutoTravel.h"

#include "Chat.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "SpellAuraDefines.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Fraktionspruefung: ein Flugpunkt gehoert zu einer Seite, wenn dort ein
    // Reittier fuer diese Seite steht. Index 0 ist Horde, 1 ist Allianz.
    bool NodeUsableBy(TaxiNodesEntry const* node, Player* player)
    {
        if (!node)
            return false;
        uint32 idx = (player->GetTeamId() == TEAM_ALLIANCE) ? 1 : 0;
        return node->MountCreatureID[idx] != 0;
    }

    std::string MoneyText(uint32 copper)
    {
        char b[64];
        uint32 g = copper / 10000;
        uint32 s = (copper % 10000) / 100;
        uint32 c = copper % 100;
        if (g)
            std::snprintf(b, sizeof(b), "%ug %us %uk", g, s, c);
        else if (s)
            std::snprintf(b, sizeof(b), "%us %uk", s, c);
        else
            std::snprintf(b, sizeof(b), "%uk", c);
        return b;
    }

    struct TaxiHop
    {
        uint32 to = 0;
        uint32 price = 0;
    };

    // Nachbarn eines Flugpunkts, gefiltert auf das, was dieser Charakter
    // ueberhaupt benutzen darf.
    void CollectHops(uint32 from, Player* player, std::vector<TaxiHop>& out)
    {
        out.clear();

        auto src = sTaxiPathSetBySource.find(from);
        if (src == sTaxiPathSetBySource.end())
            return;

        for (auto const& kv : src->second)
        {
            uint32 to = kv.first;

            if (!player->m_taxi.IsTaximaskNodeKnown(to))
                continue;

            TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(to);
            if (!NodeUsableBy(node, player))
                continue;

            TaxiHop h;
            h.to = to;
            h.price = kv.second.price;
            out.push_back(h);
        }
    }

    // Naechster bekannter, benutzbarer Flugpunkt zu einer Stelle.
    uint32 NearestKnownNode(Player* player, uint32 mapId, float x, float y,
                            float maxDist, float* outDist)
    {
        uint32 best = 0;
        float bestDist = maxDist;

        for (uint32 i = 1; i < sTaxiNodesStore.GetNumRows(); ++i)
        {
            TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
            if (!node || node->map_id != mapId)
                continue;
            if (!NodeUsableBy(node, player))
                continue;
            if (!player->m_taxi.IsTaximaskNodeKnown(i))
                continue;

            float d = AT::Dist2D(x, y, node->x, node->y);
            if (d < bestDist)
            {
                bestDist = d;
                best = i;
            }
        }

        if (outDist)
            *outDist = best ? bestDist : -1.0f;
        return best;
    }
}

// ---------------------------------------------------------------------------
// Planung
// ---------------------------------------------------------------------------

bool AutoTravelMgr::BuildTaxiPlan(Player* player, uint32 destMap,
                                  float dx, float dy, float dz,
                                  std::vector<ATLeg>& out, std::string& note) const
{
    out.clear();

    if (!ATConf.useTaxi)
    {
        note = "Flugmeister sind abgeschaltet";
        return false;
    }
    if (player->GetMap()->IsBattlegroundOrArena() || player->GetMap()->IsDungeon())
    {
        note = "hier gibt es keine Flugrouten";
        return false;
    }
    if (!destMap)
        destMap = player->GetMapId();

    float const walkDirect = player->GetExactDist2d(dx, dy);
    if (walkDirect < ATConf.taxiMinDistance)
    {
        note = "die Strecke ist zu kurz";
        return false;
    }

    // --- Start- und Zielflugpunkt -----------------------------------------
    float toStart = 0.0f, fromEnd = 0.0f;

    uint32 startNode = NearestKnownNode(player, player->GetMapId(),
                                        player->GetPositionX(), player->GetPositionY(),
                                        ATConf.taxiMaxWalkToNode, &toStart);
    if (!startNode)
    {
        note = "kein bekannter Flugpunkt in der Naehe";
        return false;
    }

    uint32 endNode = NearestKnownNode(player, destMap, dx, dy,
                                      ATConf.taxiMaxWalkToNode, &fromEnd);
    if (!endNode)
    {
        note = "kein bekannter Flugpunkt beim Ziel";
        return false;
    }
    if (startNode == endNode)
    {
        note = "Start und Ziel gehoeren zum selben Flugpunkt";
        return false;
    }

    // --- Lohnt sich der Flug ueberhaupt? ----------------------------------
    //
    // Verglichen wird die Laufstrecke, die uebrig bleibt:
    //
    //     zu Fuss:  |Spieler -> Ziel|
    //     mit Flug: |Spieler -> Flugpunkt A| + |Flugpunkt B -> Ziel|
    //
    // Die Flugstrecke selbst kostet keine Laufzeit und zaehlt deshalb nicht mit.
    float const walkWithTaxi = toStart + fromEnd;
    float const saving = 1.0f - (walkWithTaxi / std::max(1.0f, walkDirect));

    if (saving < ATConf.taxiMinSaving)
    {
        char b[192];
        std::snprintf(b, sizeof(b), "der Flug spart nur %.0f%% der Strecke", saving * 100.0f);
        note = b;
        return false;
    }

    // --- Guenstigste Flugverbindung suchen (Dijkstra) ----------------------
    std::unordered_map<uint32, uint32> cost;      // Knoten -> Preis in Kupfer
    std::unordered_map<uint32, uint32> prev;
    std::unordered_set<uint32> closed;

    typedef std::pair<uint32, uint32> QE;         // (Preis, Knoten)
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;

    cost[startNode] = 0;
    pq.push(QE(0, startNode));

    std::vector<TaxiHop> hops;
    bool found = false;
    uint32 guard = 0;

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

        if (++guard > 5000)
            break;

        CollectHops(cur.second, player, hops);
        for (TaxiHop const& h : hops)
        {
            uint32 nc = cur.first + h.price;
            if (nc > ATConf.taxiMaxCostCopper)
                continue;

            auto old = cost.find(h.to);
            if (old == cost.end() || nc < old->second)
            {
                cost[h.to] = nc;
                prev[h.to] = cur.second;
                pq.push(QE(nc, h.to));
            }
        }
    }

    if (!found)
    {
        note = "keine Flugverbindung zwischen den beiden Punkten";
        return false;
    }

    uint32 const total = cost[endNode];

    if (total > ATConf.taxiMaxCostCopper)
    {
        note = "der Flug ist teurer als erlaubt (" + MoneyText(total) + ")";
        return false;
    }
    if (player->GetMoney() < total)
    {
        note = "das Gold reicht nicht (" + MoneyText(total) + ")";
        return false;
    }

    // --- Kette zurueckverfolgen -------------------------------------------
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
            note = "Rueckverfolgung der Flugroute unterbrochen";
            return false;
        }
        at = p->second;
        if (chain.size() > 60)
        {
            note = "Flugroute unplausibel lang";
            return false;
        }
    }
    std::reverse(chain.begin(), chain.end());

    TaxiNodesEntry const* startEntry = sTaxiNodesStore.LookupEntry(startNode);
    TaxiNodesEntry const* endEntry = sTaxiNodesStore.LookupEntry(endNode);
    if (!startEntry || !endEntry)
    {
        note = "Flugpunktdaten unvollstaendig";
        return false;
    }

    // --- Etappen bauen ----------------------------------------------------
    //
    //   1. zum Flugmeister laufen (dort startet der Flug)
    //   2. am Zielflugpunkt landen und zu Fuss weiter
    //
    // Die Zwischenstationen der Kette werden nicht als Etappen gefuehrt: den
    // ganzen Flug erledigt der Core in einem Zug.
    ATLeg board;
    board.mapId = startEntry->map_id;
    board.wx = startEntry->x;
    board.wy = startEntry->y;
    board.wz = startEntry->z;
    board.resolved = true;
    board.kind = AT_LEG_TAXI;
    board.taxiFrom = startNode;
    board.taxiTo = endNode;
    board.taxiCost = total;
    board.name = "Flugmeister";
    board.nextName = "Flugpunkt";

    // Die vollstaendige Kette in der Etappe mitfuehren, damit StartTaxi sie
    // unveraendert an den Core geben kann. Sie steht als Namensliste im
    // nextName-Feld nicht gut aufgehoben -- deshalb wird sie beim Start
    // einfach erneut berechnet, siehe StartTaxi().

    ATLeg land;
    land.mapId = endEntry->map_id;
    land.wx = endEntry->x;
    land.wy = endEntry->y;
    land.wz = endEntry->z;
    land.resolved = true;
    land.kind = AT_LEG_WALK;
    land.name = "Flugpunkt";

    out.push_back(board);
    out.push_back(land);

    char b[288];
    std::snprintf(b, sizeof(b),
                  "%u Stationen, %s, %.0f yd zum Flugmeister, danach noch %.0f yd zu Fuss "
                  "(spart %.0f%%).",
                  uint32(chain.size()), MoneyText(total).c_str(), toStart, fromEnd, saving * 100.0f);
    note = b;
    return true;
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

bool AutoTravelMgr::StartTaxi(Player* player, ATSession& s, ATLeg const& leg)
{
    if (!ATConf.useTaxi || leg.kind != AT_LEG_TAXI || !leg.taxiFrom || !leg.taxiTo)
        return false;

    if (player->IsInCombat())
    {
        Dbg(player, s, "Flug nicht moeglich: im Kampf.");
        return false;
    }

    // Der Core verlangt: nicht beritten, Kontrolle beim Client, nah genug am
    // Flugpunkt. Alle drei Punkte werden hier hergestellt.
    ReleaseControl(player, s);

    if (player->IsMounted())
    {
        player->Dismount();
        player->RemoveAurasByType(SPELL_AURA_MOUNTED);
    }

    // Kette neu bestimmen: zwischen Planung und Abflug koennen Minuten
    // vergehen, und in der Zeit kann der Charakter Flugpunkte dazugelernt
    // haben -- oder Gold ausgegeben haben.
    std::vector<uint32> nodes;
    {
        std::unordered_map<uint32, uint32> cost;
        std::unordered_map<uint32, uint32> prev;
        std::unordered_set<uint32> closed;

        typedef std::pair<uint32, uint32> QE;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;

        cost[leg.taxiFrom] = 0;
        pq.push(QE(0, leg.taxiFrom));

        std::vector<TaxiHop> hops;
        bool found = false;
        uint32 guard = 0;

        while (!pq.empty())
        {
            QE cur = pq.top();
            pq.pop();

            if (cur.second == leg.taxiTo)
            {
                found = true;
                break;
            }
            if (closed.find(cur.second) != closed.end())
                continue;
            closed.insert(cur.second);
            if (++guard > 5000)
                break;

            CollectHops(cur.second, player, hops);
            for (TaxiHop const& h : hops)
            {
                uint32 nc = cur.first + h.price;
                if (nc > ATConf.taxiMaxCostCopper)
                    continue;
                auto old = cost.find(h.to);
                if (old == cost.end() || nc < old->second)
                {
                    cost[h.to] = nc;
                    prev[h.to] = cur.second;
                    pq.push(QE(nc, h.to));
                }
            }
        }

        if (!found)
        {
            Dbg(player, s, "Flug nicht moeglich: keine Verbindung mehr vorhanden.");
            return false;
        }

        uint32 at = leg.taxiTo;
        while (true)
        {
            nodes.push_back(at);
            if (at == leg.taxiFrom)
                break;
            auto p = prev.find(at);
            if (p == prev.end())
                return false;
            at = p->second;
            if (nodes.size() > 60)
                return false;
        }
        std::reverse(nodes.begin(), nodes.end());
    }

    if (nodes.size() < 2)
        return false;

    // Preis aufsummieren, um ihn melden zu koennen.
    uint32 price = 0;
    {
        std::vector<TaxiHop> hops;
        for (size_t i = 0; i + 1 < nodes.size(); ++i)
        {
            CollectHops(nodes[i], player, hops);
            for (TaxiHop const& h : hops)
                if (h.to == nodes[i + 1])
                {
                    price += h.price;
                    break;
                }
        }
    }

    if (player->GetMoney() < price)
    {
        Msg(player, "Der Flug kostet " + MoneyText(price) + " - so viel hast du nicht dabei.");
        return false;
    }

    if (!player->ActivateTaxiPathTo(nodes, nullptr, 0))
    {
        Dbg(player, s, "ActivateTaxiPathTo wurde vom Core abgelehnt.");
        return false;
    }

    TaxiNodesEntry const* endEntry = sTaxiNodesStore.LookupEntry(leg.taxiTo);

    char b[288];
    std::snprintf(b, sizeof(b), "Abflug fuer %s%s%s. AutoTravel wartet auf die Landung.",
                  MoneyText(price).c_str(),
                  (endEntry && nodes.size() > 2) ? " ueber " : "",
                  (endEntry && nodes.size() > 2) ? "Zwischenstationen" : "");
    Msg(player, b);

    s.wasInFlight = false;
    return true;
}

// ---------------------------------------------------------------------------
// Diagnose
// ---------------------------------------------------------------------------

void AutoTravelMgr::TaxiInfo(Player* player)
{
    char b[288];

    uint32 known = 0;
    uint32 usable = 0;
    for (uint32 i = 1; i < sTaxiNodesStore.GetNumRows(); ++i)
    {
        TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
        if (!node)
            continue;
        if (!player->m_taxi.IsTaximaskNodeKnown(i))
            continue;
        ++known;
        if (NodeUsableBy(node, player))
            ++usable;
    }

    std::snprintf(b, sizeof(b),
                  "Flugpunkte: %u bekannt, davon %u fuer deine Fraktion nutzbar. "
                  "Automatik: %s, Preisgrenze %s.",
                  known, usable,
                  ATConf.useTaxi ? "an" : "aus",
                  MoneyText(ATConf.taxiMaxCostCopper).c_str());
    Msg(player, b);

    float d = 0.0f;
    uint32 n = NearestKnownNode(player, player->GetMapId(),
                                player->GetPositionX(), player->GetPositionY(),
                                ATConf.taxiMaxWalkToNode, &d);
    if (!n)
    {
        std::snprintf(b, sizeof(b), "Kein bekannter Flugpunkt innerhalb von %.0f yd.",
                      ATConf.taxiMaxWalkToNode);
        Msg(player, b);
        return;
    }

    TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(n);
    std::vector<TaxiHop> hops;
    CollectHops(n, player, hops);

    std::snprintf(b, sizeof(b), "Naechster Flugpunkt: #%u, %.0f yd entfernt, %u Ziele erreichbar.",
                  n, d, uint32(hops.size()));
    Msg(player, b);

    if (node && node->map_id != player->GetMapId())
        Msg(player, "Hinweis: der Flugpunkt liegt auf einer anderen Karte.");
}
