/*
 * AutoTravel_Util.cpp
 * ---------------------------------------------------------------------------
 * Kleine, reine Hilfsfunktionen: Zahlenparser, Textbereinigung, Namenspruefung.
 *
 * Bewusst eigene Datei ohne jede Abhaengigkeit vom Core ausser den Basistypen.
 * Alles hier verarbeitet Eingaben, die der Client liefert -- und laesst sich
 * deshalb als eigenes Programm testen, ohne AzerothCore zu bauen
 * (tools/tests/util_test.cpp).
 */

#include "AutoTravel.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <queue>
#include <string>

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

    namespace
    {
        bool IsDigit(char c) { return c >= '0' && c <= '9'; }
    }

    // strtoul() akzeptiert fuehrenden Leerraum und ein Vorzeichen und rechnet
    // "-18446744073709551615" ohne Fehlermeldung zu 1 um. Das sind keine Zahlen,
    // die ein Addon je schickt -- also gelten nur reine Ziffernfolgen.
    bool ParseUInt(std::string const& in, uint32& out)
    {
        if (in.empty() || !IsDigit(in[0]))
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

    // Erlaubt sind nur Zeichen einer gewoehnlichen Dezimalzahl. strtod() liest
    // sonst auch "0x1p3", "infinity" und "nan(...)"; die beiden letzten fing
    // schon isfinite() ab, die Hexform soll gar nicht erst durchgehen.
    bool ParseFloat(std::string const& in, float& out)
    {
        if (in.empty())
            return false;
        for (char c : in)
        {
            if (!IsDigit(c) && c != '-' && c != '+' && c != '.' && c != 'e' && c != 'E')
                return false;
        }
        char* end = nullptr;
        errno = 0;
        double v = std::strtod(in.c_str(), &end);
        if (errno == ERANGE || end == in.c_str() || *end != '\0')
            return false;
        if (!std::isfinite(v))
            return false;

        // Ein Wert, der als double endlich ist, aber nicht in ein float passt
        // ("1e39"), wuerde beim Verengen zu +inf. Dann ist er auch abzuweisen.
        float f = float(v);
        if (!std::isfinite(f))
            return false;
        out = f;
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

    std::string SanitizeText(std::string const& in, size_t maxBytes)
    {
        std::string out;
        out.reserve(std::min(in.size(), maxBytes));

        for (unsigned char c : in)
        {
            // Steuerzeichen und DEL raus; '|' ist Feldtrenner im Protokoll und
            // leitet im Chat Farb- und Linkcodes ein.
            if (c < 0x20 || c == 0x7F || c == '|')
                continue;
            out.push_back(char(c));
        }

        // Nicht mitten in einem UTF-8-Zeichen abschneiden: erst auf die Grenze
        // zurueckgehen, dann pruefen, ob das letzte Zeichen vollstaendig ist.
        if (out.size() > maxBytes)
        {
            size_t cut = maxBytes;
            while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
                --cut;
            out.resize(cut);
        }

        // Fuehrende und folgende Leerzeichen entfernen.
        size_t b = out.find_first_not_of(' ');
        if (b == std::string::npos)
            return std::string();
        size_t e = out.find_last_not_of(' ');
        return out.substr(b, e - b + 1);
    }

    // Der Name steht in SQL nur zwischen Backticks bzw. in einem einfach
    // gequoteten Text. Gefaehrlich sind deshalb nur ` ' \ und Steuerzeichen;
    // ein Bindestrich ("acore-playerbots") ist ein gueltiger Datenbankname und
    // darf nicht stillschweigend durch den Standard ersetzt werden. Trotzdem
    // bleibt die Liste eng: Buchstaben, Ziffern, _ $ und -.
    bool IsSafeIdentifier(std::string const& in)
    {
        if (in.empty() || in.size() > 64)
            return false;
        for (unsigned char c : in)
        {
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                   || (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '-';
            if (!ok)
                return false;
        }
        return true;
    }
    bool SplineFitsPacket(Movement::PointsArray const& pts)
    {
        size_t const n = pts.size();

        for (size_t i = 0; i < n; ++i)
        {
            if (!std::isfinite(pts[i].x) || !std::isfinite(pts[i].y) || !std::isfinite(pts[i].z))
                return false;
        }

        if (n < 3)
            return true;                       // ohne Zwischenpunkte wird nichts gepackt

        G3D::Vector3 const mid = (pts.front() + pts.back()) * 0.5f;

        // Wie WriteLinearPath: nur die Zwischenpunkte werden kodiert.
        for (size_t i = 1; i + 1 < n; ++i)
        {
            if (std::fabs(pts[i].x - mid.x) > SPLINE_PACK_LIMIT_XY
                || std::fabs(pts[i].y - mid.y) > SPLINE_PACK_LIMIT_XY
                || std::fabs(pts[i].z - mid.z) > SPLINE_PACK_LIMIT_Z)
                return false;
        }
        return true;
    }

    bool SplitLongSegments(Movement::PointsArray& path, size_t from,
                           G3D::Vector3 const& origin, float maxLen)
    {
        if (!(maxLen > 1.0f) || from >= path.size())
            return false;

        // Erst feststellen, ob ueberhaupt etwas zu tun ist: der Normalfall
        // (kurze Segmente) soll nichts kopieren.
        bool needed = false;
        float px = origin.x;
        float py = origin.y;
        for (size_t i = from; i < path.size(); ++i)
        {
            if (Dist2D(path[i].x, path[i].y, px, py) > maxLen)
            {
                needed = true;
                break;
            }
            px = path[i].x;
            py = path[i].y;
        }
        if (!needed)
            return false;

        Movement::PointsArray out(path.begin(), path.begin() + from);
        out.reserve(path.size() + 32);

        float startX = origin.x;
        float startY = origin.y;
        float startZ = (from > 0) ? path[from - 1].z : origin.z;

        for (size_t i = from; i < path.size(); ++i)
        {
            G3D::Vector3 const t = path[i];
            float const len = Dist2D(t.x, t.y, startX, startY);

            // Obergrenze gegen Ausreisser: 400 Stuecke sind 16 km bei 40 yd.
            uint32 pieces = std::max<uint32>(1, uint32(std::ceil(len / maxLen)));
            if (pieces > 400)
                pieces = 400;

            for (uint32 k = 1; k < pieces; ++k)
            {
                float const f = float(k) / float(pieces);
                out.push_back(G3D::Vector3(startX + (t.x - startX) * f,
                                           startY + (t.y - startY) * f,
                                           startZ + (t.z - startZ) * f));
            }
            out.push_back(t);

            startX = t.x;
            startY = t.y;
            startZ = t.z;
        }

        path.swap(out);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Kuerzester Weg im Knotengraphen
// ---------------------------------------------------------------------------
//
// Dijkstra, kein A*: die Luftlinie als Schaetzung ist nicht zulaessig, weil
// Flug-, Schiffs- und Portalkanten im Graphen nur wenige Punkte kosten, egal wie
// weit sie tragen -- A* mit geschlossener Menge lieferte damit oft Ketten, die
// teurer waren als das Optimum. Der Graph hat rund 3,8 Tsd. Knoten und 15 Tsd.
// Kanten; die vollstaendige Suche ist billig.

bool AT::ShortestChain(std::unordered_map<uint32, ATNode> const& nodes,
                       std::unordered_map<uint32, std::vector<ATNodeLink>> const& links,
                       uint32 startNode, uint32 endNode,
                       std::unordered_set<uint64> const& banned,
                       ChainRules const& rules,
                       std::vector<uint32>& chain,
                       std::unordered_map<uint32, uint8>& prevType,
                       std::string& note)
{
    chain.clear();
    prevType.clear();

    if (nodes.find(startNode) == nodes.end() || nodes.find(endNode) == nodes.end())
    {
        note = "Start- oder Zielknoten fehlt";
        return false;
    }

    std::unordered_map<uint32, float> dist;
    std::unordered_set<uint32> closed;
    std::unordered_map<uint32, uint32> prev;

    typedef std::pair<float, uint32> QE;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;

    dist[startNode] = 0.0f;
    pq.push(QE(0.0f, startNode));

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

        if (++visited > rules.maxVisited)
            break;

        auto lIt = links.find(cur.second);
        if (lIt == links.end())
            continue;

        float const g = dIt->second;

        for (ATNodeLink const& l : lIt->second)
        {
            if (l.type < 1 || l.type > 4)
                continue;                    // unbekannter Typ: nicht benutzbar

            if (banned.find(EdgeId(cur.second, l.to)) != banned.end())
                continue;

            auto toIt = nodes.find(l.to);
            if (toIt == nodes.end())
                continue;

            if (!rules.allowedMaps.empty()
                && rules.allowedMaps.find(toIt->second.mapId) == rules.allowedMaps.end())
                continue;

            float cost = l.baseCost;
            if (l.type != 1)
            {
                if (!rules.useSpecialLinks)
                    continue;
                if (rules.linkUsable && !rules.linkUsable(cur.second, l))
                    continue;

                // Sonderverbindungen kosten extra, damit sie nur benutzt werden,
                // wenn sie wirklich viel Strecke sparen.
                cost += rules.specialLinkCost;
            }

            float const nd = g + cost;
            auto old = dist.find(l.to);
            if (old == dist.end() || nd < old->second)
            {
                dist[l.to] = nd;
                prev[l.to] = cur.second;
                prevType[l.to] = l.type;
                pq.push(QE(nd, l.to));
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
            chain.clear();
            return false;
        }
        at = p->second;

        if (chain.size() > 400)
        {
            note = "Route unplausibel lang";
            chain.clear();
            return false;
        }
    }
    std::reverse(chain.begin(), chain.end());
    return true;
}

uint8 AT::TransportFaction(uint32 entry)
{
    switch (entry)
    {
        // Horde: Zeppeline
        case 164871:    // Zeppelin (The Thundercaller): Orgrimmar - Grom'gol
        case 175080:    // Zeppelin (The Iron Eagle): Orgrimmar - Unterstadt
        case 176495:    // Zeppelin (The Purple Princess): Unterstadt - Grom'gol
        case 181689:    // Zeppelin, Horde (Cloudkisser): Unterstadt - Vengeance Landing
        case 186238:    // Zeppelin, Horde (The Mighty Wind): Orgrimmar - Warsong Hold
            return 2;

        // Allianz: Schiffe
        case 176231:    // Ship (The Lady Mehley)
        case 176244:    // Ship, Night Elf (Moonspray)
        case 176310:    // Ship (The Bravery)
        case 177233:    // Ship, Night Elf (Feathermoon Ferry)
        case 181646:    // Ship, Night Elf (Elune's Blessing)
        case 181688:    // Ship, Icebreaker (Northspear): Menethil - Valgarde
        case 190536:    // Ship, Icebreaker (Stormwind's Pride): Sturmwind - Valiance Keep
            return 1;

        default:
            return 0;   // u. a. 20808 (The Maiden's Fancy), die Schildkroeten
    }
}
