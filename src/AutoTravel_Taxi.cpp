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
 * ---------------------------------------------------------------------------
 * WAS "BEKANNT" HEISST
 * ---------------------------------------------------------------------------
 *
 * Ein Flugpunkt gilt als bekannt, wenn er in der Flugpunktmaske des Charakters
 * steht (`m_taxi.IsTaximaskNodeKnown`). Das ist dieselbe Maske, die der
 * GM-Befehl `.cheat taxi` vollstaendig setzt -- ein Spielleiter mit
 * eingeschaltetem Taxi-Cheat hat damit automatisch alle Punkte, und die
 * Automatik benutzt sie auch. Das ist gewollt und braucht keine Sonderbehandlung:
 * die Maske ist die einzige Wahrheit, und der Core prueft sie beim Abflug noch
 * einmal selbst.
 *
 * Was dagegen NICHT passieren darf: eine Flugverbindung einplanen, die der
 * Charakter gar nicht nehmen kann. Genau dafuer gibt es ResolveTaxiHop() --
 * es beantwortet die Frage "koennte dieser Charakter hier wirklich fliegen"
 * VOR der Reise, nicht erst, wenn er am Flugmeister steht.
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
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // -----------------------------------------------------------------------
    // Preis einer Flugverbindung
    // -----------------------------------------------------------------------
    //
    // AzerothCore hat den Wertetyp von sTaxiPathSetBySource im Lauf der Zeit
    // geaendert:
    //
    //   frueher   std::unordered_map<uint32, TaxiPathBySourceAndDestination>
    //   heute     std::unordered_map<uint32, TaxiPathEntry const*>
    //
    // Beide tragen ein Feld 'price'; einmal wird es mit '.', einmal mit '->'
    // erreicht. Damit das Modul auf beiden Staenden baut, wird der Zugriff
    // hier einmal gekapselt statt an der Aufrufstelle geraten.
    //
    // 'if constexpr' statt zweier Ueberladungen, weil eine Ueberladung auf
    // 'T const&' und eine auf 'T const*' fuer ein Zeigerargument beide exakt
    // passen und die Aufloesung dann von der partiellen Ordnung abhaengt --
    // unnoetig heikel fuer eine so kleine Sache.
    template<class T>
    inline uint32 TaxiPriceOf(T const& v)
    {
        if constexpr (std::is_pointer<T>::value)
            return v ? uint32(v->price) : 0u;
        else
            return uint32(v.price);
    }

    // Fraktionspruefung: ein Flugpunkt gehoert zu einer Seite, wenn dort ein
    // Reittier fuer diese Seite steht. Index 0 ist Horde, 1 ist Allianz.
    bool NodeUsableBy(TaxiNodesEntry const* node, Player* player)
    {
        if (!node)
            return false;
        uint32 idx = (player->GetTeamId() == TEAM_ALLIANCE) ? 1 : 0;
        return node->MountCreatureID[idx] != 0;
    }

    bool NodeKnownBy(uint32 nodeId, Player* player)
    {
        return player->m_taxi.IsTaximaskNodeKnown(nodeId);
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

            if (!NodeKnownBy(to, player))
                continue;

            TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(to);
            if (!NodeUsableBy(node, player))
                continue;

            TaxiHop h;
            h.to = to;
            h.price = TaxiPriceOf(kv.second);
            out.push_back(h);
        }
    }

    // Guenstigste Kette zwischen zwei Flugpunkten. Rueckgabe false, wenn es
    // keine gibt oder sie ueber der Preisgrenze liegt.
    bool FindCheapestChain(Player* player, uint32 from, uint32 to,
                           std::vector<uint32>& chain, uint32& totalCost)
    {
        chain.clear();
        totalCost = 0;

        if (!from || !to || from == to)
            return false;
        if (!NodeKnownBy(from, player) || !NodeKnownBy(to, player))
            return false;

        std::unordered_map<uint32, uint32> cost;
        std::unordered_map<uint32, uint32> prev;
        std::unordered_set<uint32> closed;

        typedef std::pair<uint32, uint32> QE;      // (Preis, Knoten)
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;

        cost[from] = 0;
        pq.push(QE(0, from));

        std::vector<TaxiHop> hops;
        bool found = false;
        uint32 guard = 0;

        while (!pq.empty())
        {
            QE cur = pq.top();
            pq.pop();

            if (cur.second == to)
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
            return false;

        totalCost = cost[to];
        if (totalCost > ATConf.taxiMaxCostCopper)
            return false;

        uint32 at = to;
        while (true)
        {
            chain.push_back(at);
            if (at == from)
                break;
            auto p = prev.find(at);
            if (p == prev.end())
                return false;
            at = p->second;
            if (chain.size() > 60)
                return false;
        }
        std::reverse(chain.begin(), chain.end());
        return chain.size() >= 2;
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
            if (!NodeKnownBy(i, player))
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
// Zaehlwerk fuer die Auskunft
// ---------------------------------------------------------------------------

void AutoTravelMgr::TaxiStats(Player* player, uint32& total, uint32& known,
                              uint32& usable) const
{
    total = 0;
    known = 0;
    usable = 0;

    for (uint32 i = 1; i < sTaxiNodesStore.GetNumRows(); ++i)
    {
        TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
        if (!node)
            continue;
        ++total;

        if (!NodeKnownBy(i, player))
            continue;
        ++known;

        if (NodeUsableBy(node, player))
            ++usable;
    }
}

// ---------------------------------------------------------------------------
// Kann dieser Charakter hier wirklich fliegen?
// ---------------------------------------------------------------------------
//
// Beantwortet die Frage fuer EINE Verbindung, vor der Reise. Gebraucht wird
// das an zwei Stellen:
//
//   * der Knotengraph von mod-playerbots kennt Flugverbindungen, die ein
//     konkreter Charakter nicht nehmen kann -- ein Charakter der Stufe 1 kennt
//     genau einen Flugpunkt
//   * die eigene Flugplanung, die dieselben Pruefungen braucht
//
// Geprueft wird alles, was der Core beim Abflug auch prueft: Punkt bekannt,
// Fraktion passt, Verbindung existiert, Preis unter der Grenze, Geld reicht.
// Zusaetzlich muss der gefundene Flugmeister nah genug am gefragten Ort liegen
// -- sonst beschreibt die Verbindung einen ganz anderen Flug.

bool AutoTravelMgr::ResolveTaxiHop(Player* player,
                                   uint32 mapA, float ax, float ay,
                                   uint32 mapB, float bx, float by,
                                   uint32& fromNode, uint32& toNode, uint32& cost,
                                   uint32& boardMap, float& boardX, float& boardY, float& boardZ,
                                   uint32& landMap, float& landX, float& landY, float& landZ) const
{
    fromNode = 0;
    toNode = 0;
    cost = 0;

    if (!ATConf.useTaxi || !player)
        return false;

    // Wie weit ein Flugmeister vom gefragten Ort entfernt sein darf, damit es
    // noch dieselbe Verbindung ist. Die Knoten des Playerbot-Graphen liegen
    // direkt beim Flugmeister ("Dun Morogh flightMaster"), also reicht ein
    // enges Fenster.
    constexpr float MATCH_RANGE = 80.0f;

    float dA = 0.0f, dB = 0.0f;
    uint32 a = NearestKnownNode(player, mapA, ax, ay, MATCH_RANGE, &dA);
    if (!a)
        return false;

    uint32 b = NearestKnownNode(player, mapB, bx, by, MATCH_RANGE, &dB);
    if (!b || a == b)
        return false;

    std::vector<uint32> chain;
    uint32 total = 0;
    if (!FindCheapestChain(player, a, b, chain, total))
        return false;

    if (player->GetMoney() < total)
        return false;

    TaxiNodesEntry const* ea = sTaxiNodesStore.LookupEntry(a);
    TaxiNodesEntry const* eb = sTaxiNodesStore.LookupEntry(b);
    if (!ea || !eb)
        return false;

    fromNode = a;
    toNode = b;
    cost = total;

    boardMap = ea->map_id;
    boardX = ea->x;
    boardY = ea->y;
    boardZ = ea->z;

    landMap = eb->map_id;
    landX = eb->x;
    landY = eb->y;
    landZ = eb->z;

    return true;
}

// ---------------------------------------------------------------------------
// Eigene Flugplanung
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

    // --- Hat der Charakter ueberhaupt genug Flugpunkte? --------------------
    //
    // Diese Pruefung steht bewusst ganz vorn. Ein frischer Charakter kennt
    // einen einzigen Flugpunkt; ohne sie liefe die ganze Suche los, um am Ende
    // dasselbe festzustellen.
    {
        uint32 total = 0, known = 0, usable = 0;
        TaxiStats(player, total, known, usable);
        if (usable < 2)
        {
            char b[160];
            std::snprintf(b, sizeof(b),
                          "du kennst erst %u nutzbare Flugpunkte (von %u)", usable, total);
            note = b;
            return false;
        }
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

    // --- Guenstigste Verbindung suchen ------------------------------------
    std::vector<uint32> chain;
    uint32 total = 0;

    if (!FindCheapestChain(player, startNode, endNode, chain, total))
    {
        note = "keine bezahlbare Flugverbindung zwischen den beiden Punkten";
        return false;
    }

    if (player->GetMoney() < total)
    {
        note = "das Gold reicht nicht (" + MoneyText(total) + ")";
        return false;
    }

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

    char b[320];
    std::snprintf(b, sizeof(b),
                  "%u Stationen, %s, %.0f yd zum Flugmeister, danach noch %.0f yd zu Fuss "
                  "(spart %.0f%%).",
                  uint32(chain.size()), MoneyText(total).c_str(), toStart, fromEnd,
                  saving * 100.0f);
    note = b;
    return true;
}

// ---------------------------------------------------------------------------
// Abflug
// ---------------------------------------------------------------------------

bool AutoTravelMgr::StartTaxi(Player* player, ATSession& s, ATLeg const& leg)
{
    if (!ATConf.useTaxi || leg.kind != AT_LEG_TAXI)
        return false;

    if (!leg.taxiFrom || !leg.taxiTo)
    {
        // Darf nicht mehr vorkommen: Etappen aus dem Knotengraphen werden
        // beim Bauen gegen die echten Flugpunkte geprueft. Bleibt als
        // Sicherung stehen, damit ein Fehler hier nicht in einem
        // ActivateTaxiPathTo mit leerer Liste endet.
        Dbg(player, s, "Flugetappe ohne aufgeloeste Flugpunkte - wird uebersprungen.");
        return false;
    }

    if (player->IsInCombat())
    {
        Dbg(player, s, "Flug nicht moeglich: im Kampf.");
        return false;
    }

    // Der Core verlangt: nicht beritten, Kontrolle beim Client, nah genug am
    // Flugpunkt. Die ersten beiden Punkte werden hier hergestellt.
    ReleaseControl(player, s);

    if (player->IsMounted())
    {
        player->Dismount();
        player->RemoveAurasByType(SPELL_AURA_MOUNTED);
    }

    // Kette neu bestimmen: zwischen Planung und Abflug koennen Minuten
    // vergehen, und in der Zeit kann der Charakter Flugpunkte dazugelernt oder
    // Gold ausgegeben haben.
    std::vector<uint32> nodes;
    uint32 price = 0;

    if (!FindCheapestChain(player, leg.taxiFrom, leg.taxiTo, nodes, price))
    {
        Dbg(player, s, "Flug nicht moeglich: keine bezahlbare Verbindung mehr vorhanden.");
        return false;
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

    char b[288];
    std::snprintf(b, sizeof(b),
                  "Abflug fuer %s%s. AutoTravel wartet auf die Landung.",
                  MoneyText(price).c_str(),
                  (nodes.size() > 2) ? " ueber Zwischenstationen" : "");
    Msg(player, b);

    s.wasInFlight = false;
    return true;
}

// ---------------------------------------------------------------------------
// Auskunft
// ---------------------------------------------------------------------------

void AutoTravelMgr::TaxiInfo(Player* player)
{
    char b[320];

    uint32 total = 0, known = 0, usable = 0;
    TaxiStats(player, total, known, usable);

    std::snprintf(b, sizeof(b),
                  "Flugpunkte: %u von %u bekannt, davon %u fuer deine Fraktion nutzbar.",
                  known, total, usable);
    Msg(player, b);

    std::snprintf(b, sizeof(b), "Automatik: %s | Preisgrenze %s | ab %.0f yd Restweg | "
                                "muss %.0f%% der Strecke sparen",
                  ATConf.useTaxi ? "an" : "aus",
                  MoneyText(ATConf.taxiMaxCostCopper).c_str(),
                  ATConf.taxiMinDistance,
                  ATConf.taxiMinSaving * 100.0f);
    Msg(player, b);

    if (usable < 2)
    {
        Msg(player, "Mit weniger als zwei nutzbaren Flugpunkten kommt kein Flug zustande - "
                    "AutoTravel laeuft dann. Ein Spielleiter kann mit '.cheat taxi on' alle "
                    "Flugpunkte freischalten.");
        return;
    }

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

    std::snprintf(b, sizeof(b),
                  "Naechster Flugpunkt: #%u, %.0f yd entfernt, %u direkt erreichbare Ziele.",
                  n, d, uint32(hops.size()));
    Msg(player, b);

    if (node && node->map_id != player->GetMapId())
        Msg(player, "Hinweis: der Flugpunkt liegt auf einer anderen Karte.");
}
