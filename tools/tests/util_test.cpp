/*
 * util_test.cpp -- Tests fuer AutoTravel_Util.cpp
 * ---------------------------------------------------------------------------
 * Alles, was der Client schickt, laeuft zuerst durch diese Funktionen. Die
 * Tests decken deshalb vor allem Ausreisser ab: NaN, Unendlich, Ueberlauf,
 * Reste hinter der Zahl, Steuerzeichen, abgeschnittene UTF-8-Zeichen.
 *
 * Bauen und starten: tools/check.sh <pfad-zu-azerothcore>
 * Braucht von AzerothCore nur die Header (Basistypen), nichts wird gelinkt.
 */

#include "AutoTravel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond) \
    do { \
        ++sChecks; \
        if (!(cond)) { ++sFailures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static void TestParseUInt()
{
    uint32 v = 123;
    CHECK(AT::ParseUInt("0", v) && v == 0);
    CHECK(AT::ParseUInt("4294967295", v) && v == 4294967295u);
    CHECK(!AT::ParseUInt("4294967296", v));            // eins zu gross
    CHECK(!AT::ParseUInt("99999999999999999999", v));  // ERANGE
    CHECK(!AT::ParseUInt("", v));
    CHECK(!AT::ParseUInt("abc", v));                   // atoi haette still 0 geliefert
    CHECK(!AT::ParseUInt("12abc", v));                 // Rest hinter der Zahl
    CHECK(!AT::ParseUInt("1.5", v));
    CHECK(!AT::ParseUInt(" 7", v));                    // kein fuehrender Leerraum
    CHECK(!AT::ParseUInt("+7", v));
    CHECK(!AT::ParseUInt("-1", v));
    CHECK(!AT::ParseUInt("-18446744073709551615", v)); // strtoul rechnet das still zu 1 um
    CHECK(!AT::ParseUInt("0x10", v));
}

static void TestParseFloat()
{
    float v = 0.0f;
    CHECK(AT::ParseFloat("1.5", v) && v == 1.5f);
    CHECK(AT::ParseFloat("-0.25", v) && v == -0.25f);
    CHECK(!AT::ParseFloat("nan", v));                  // NaN pflanzt sich durch die Wegfindung fort
    CHECK(!AT::ParseFloat("NaN", v));
    CHECK(!AT::ParseFloat("inf", v));
    CHECK(!AT::ParseFloat("-inf", v));
    CHECK(!AT::ParseFloat("1e999", v));                // ERANGE
    CHECK(!AT::ParseFloat("1e39", v));                 // endlich als double, +inf als float
    CHECK(!AT::ParseFloat("-1e39", v));
    CHECK(AT::ParseFloat("3.0e38", v) && std::isfinite(v));   // knapp unter FLT_MAX
    CHECK(!AT::ParseFloat("", v));
    CHECK(!AT::ParseFloat("1.5x", v));
    CHECK(!AT::ParseFloat(".", v));
    CHECK(!AT::ParseFloat("0x1p3", v));                // Hexfloat
    CHECK(!AT::ParseFloat("infinity", v));
    CHECK(!AT::ParseFloat(" 1", v));
    CHECK(AT::ParseFloat("1e3", v) && v == 1000.0f);
    CHECK(AT::ParseFloat("+2", v) && v == 2.0f);
}

static void TestParseBool()
{
    bool b = false;
    CHECK(AT::ParseBool("1", b) && b);
    CHECK(AT::ParseBool("0", b) && !b);
    CHECK(AT::ParseBool("on", b) && b);
    CHECK(AT::ParseBool("aus", b) && !b);
    CHECK(AT::ParseBool("true", b) && b);
    CHECK(!AT::ParseBool("2", b));
    CHECK(!AT::ParseBool("yes please", b));
    CHECK(!AT::ParseBool("", b));
    CHECK(!AT::ParseBool("-1", b));
}

static void TestParseNorm()
{
    float v = -1.0f;
    CHECK(AT::ParseNorm("0", v) && v == 0.0f);
    CHECK(AT::ParseNorm("1", v) && v == 1.0f);
    CHECK(AT::ParseNorm("0.5", v) && v == 0.5f);
    CHECK(!AT::ParseNorm("1.0001", v));
    CHECK(!AT::ParseNorm("-0.0001", v));
    CHECK(!AT::ParseNorm("nan", v));
}

static void TestSanitize()
{
    // Trenner und Steuerzeichen
    CHECK(AT::SanitizeText("a|b", 48) == "ab");
    CHECK(AT::SanitizeText("|cffff0000rot|r", 48) == "cffff0000rotr");
    CHECK(AT::SanitizeText(std::string("a\nb\tc\x01" "d\x7f" "e"), 48) == "abcde");

    // Leerraum an den Raendern, innen bleibt er
    CHECK(AT::SanitizeText("  Sturmwind Bank  ", 48) == "Sturmwind Bank");
    CHECK(AT::SanitizeText("   ", 48).empty());
    CHECK(AT::SanitizeText("", 48).empty());
    CHECK(AT::SanitizeText("|||", 48).empty());

    // Laengenbegrenzung in Bytes
    CHECK(AT::SanitizeText(std::string(200, 'x'), 48).size() == 48);
    CHECK(AT::SanitizeText("abcdef", 6) == "abcdef");
    CHECK(AT::SanitizeText("abcdefg", 6) == "abcdef");

    // UTF-8: "Ä" = C3 84. Bei maxBytes 3 darf nach "ab" nichts halbes stehen bleiben.
    std::string r = AT::SanitizeText("ab\xC3\x84" "cd", 3);
    CHECK(r == "ab");
    // Passt das Zeichen ganz hinein, bleibt es.
    CHECK(AT::SanitizeText("ab\xC3\x84" "cd", 4) == "ab\xC3\x84");
    // Dreibytezeichen "€" = E2 82 AC an der Grenze
    CHECK(AT::SanitizeText("a\xE2\x82\xAC" "b", 3) == "a");
    CHECK(AT::SanitizeText("a\xE2\x82\xAC" "b", 4) == "a\xE2\x82\xAC");

    // Ergebnis enthaelt nie ein '|' oder Steuerzeichen, egal was hereinkommt
    std::string nasty;
    for (int i = 0; i < 256; ++i)
        nasty.push_back(char(i));
    std::string clean = AT::SanitizeText(nasty, 300);
    bool ok = true;
    for (unsigned char c : clean)
        if (c < 0x20 || c == 0x7F || c == '|')
            ok = false;
    CHECK(ok);
}

static void TestIdentifier()
{
    CHECK(AT::IsSafeIdentifier("acore_playerbots"));
    CHECK(AT::IsSafeIdentifier("db$1"));
    CHECK(AT::IsSafeIdentifier("A"));
    CHECK(!AT::IsSafeIdentifier(""));
    CHECK(!AT::IsSafeIdentifier("a b"));
    CHECK(!AT::IsSafeIdentifier("a`b"));
    CHECK(!AT::IsSafeIdentifier("a'b"));
    CHECK(!AT::IsSafeIdentifier("a;drop table x"));
    CHECK(!AT::IsSafeIdentifier("a.b"));
    CHECK(AT::IsSafeIdentifier("acore-playerbots"));   // gueltiger Datenbankname mit Bindestrich
    CHECK(!AT::IsSafeIdentifier("a\\b"));
    CHECK(!AT::IsSafeIdentifier("a\nb"));
    CHECK(!AT::IsSafeIdentifier("a\"b"));
    CHECK(!AT::IsSafeIdentifier(std::string(65, 'a')));
    CHECK(AT::IsSafeIdentifier(std::string(64, 'a')));
}

static void TestMisc()
{
    CHECK(std::fabs(AT::Dist2D(0, 0, 3, 4) - 5.0f) < 1e-5f);
    CHECK(AT::PathTypeName(0) == "BLANK");
    CHECK(AT::PathTypeName(0x01 | 0x04).find("NORMAL") != std::string::npos);
    CHECK(AT::PathTypeName(0x01 | 0x04).find("INCOMPLETE") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Spline-Pakete
// ---------------------------------------------------------------------------
//
// Bildet WriteLinearPath / ByteBuffer::appendPackXYZ des Cores nach (11/11/10
// Bit, 0,25 yd, Abstand zur Mitte von erstem und letztem Punkt) und liest die
// Werte so zurueck, wie es ein Client mit vorzeichenbehafteten Bitfeldern tut.
// Damit pruefen die Tests nicht nur die Grenzen, sondern, dass alles, was
// SplineFitsPacket durchlaesst, wirklich unversehrt ankommt -- und was es
// abweist, tatsaechlich zerstoert wuerde (der Fehler, der Charaktere ueber die
// Karte "fliegen" liess).

static float UnpackSigned(uint32 raw, int bits)
{
    int v = int(raw & ((1u << bits) - 1));
    if (v & (1 << (bits - 1)))
        v -= (1 << bits);
    return float(v) * 0.25f;
}

static Movement::PointsArray PacketRoundTrip(Movement::PointsArray const& pts)
{
    Movement::PointsArray out;
    if (pts.size() < 3)
        return pts;

    G3D::Vector3 const mid = (pts.front() + pts.back()) * 0.5f;
    out.push_back(pts.front());
    for (size_t i = 1; i + 1 < pts.size(); ++i)
    {
        G3D::Vector3 const off = mid - pts[i];
        uint32 const px = uint32(int(off.x / 0.25f)) & 0x7FFu;
        uint32 const py = uint32(int(off.y / 0.25f)) & 0x7FFu;
        uint32 const pz = uint32(int(off.z / 0.25f)) & 0x3FFu;
        out.push_back(G3D::Vector3(mid.x - UnpackSigned(px, 11),
                                   mid.y - UnpackSigned(py, 11),
                                   mid.z - UnpackSigned(pz, 10)));
    }
    out.push_back(pts.back());
    return out;
}

static float MaxDeviation(Movement::PointsArray const& a, Movement::PointsArray const& b)
{
    float worst = 0.0f;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        worst = std::max(worst, (a[i] - b[i]).length());
    return worst;
}

// Gerade Strecke mit einem Punkt je 'step' yd.
static Movement::PointsArray Line(G3D::Vector3 const& a, G3D::Vector3 const& b, float step)
{
    Movement::PointsArray out;
    float const len = (b - a).length();
    uint32 const n = std::max<uint32>(1, uint32(std::ceil(len / step)));
    for (uint32 i = 0; i <= n; ++i)
        out.push_back(a + (b - a) * (float(i) / float(n)));
    return out;
}

static void TestSplinePacket()
{
    using G3D::Vector3;

    // Der gemeldete Abschnitt: rund 750 yd Luftlinie ueber den See.
    Movement::PointsArray lake = Line(Vector3(-5702.6f, -3125.3f, 315.9f),
                                      Vector3(-5700.3f, -3871.3f, 331.6f), 3.0f);
    CHECK(!AT::SplineFitsPacket(lake));
    CHECK(MaxDeviation(lake, PacketRoundTrip(lake)) > 100.0f);       // der Client sah einen anderen Weg

    // Der ebenfalls gemeldete 279-yd-Abschnitt war in Ordnung.
    Movement::PointsArray ok = Line(Vector3(-5447.6f, -3121.6f, 348.1f),
                                    Vector3(-5714.6f, -3118.5f, 315.8f), 3.0f);
    CHECK(AT::SplineFitsPacket(ok));
    CHECK(MaxDeviation(ok, PacketRoundTrip(ok)) < 0.5f);             // 0,25-yd-Raster je Achse

    // Grenzfaelle: knapp innerhalb und knapp ausserhalb.
    Movement::PointsArray edgeIn = Line(Vector3(0, 0, 0), Vector3(2 * 215.0f, 0, 0), 3.0f);
    CHECK(AT::SplineFitsPacket(edgeIn));
    CHECK(MaxDeviation(edgeIn, PacketRoundTrip(edgeIn)) < 0.5f);
    Movement::PointsArray edgeOut = Line(Vector3(0, 0, 0), Vector3(2 * 230.0f, 0, 0), 3.0f);
    CHECK(!AT::SplineFitsPacket(edgeOut));

    // Hoehe: +-128 yd sind hart, die Grenze liegt bei 100.
    Movement::PointsArray tall = Line(Vector3(0, 0, 0), Vector3(30, 0, 260), 3.0f);
    CHECK(!AT::SplineFitsPacket(tall));
    Movement::PointsArray low = Line(Vector3(0, 0, 0), Vector3(30, 0, 180), 3.0f);
    CHECK(AT::SplineFitsPacket(low));
    CHECK(MaxDeviation(low, PacketRoundTrip(low)) < 0.5f);

    // Ein Wegepunkt, der weit abseits liegt (Hin- und Rueckweg), zaehlt auch.
    Movement::PointsArray uturn = Line(Vector3(0, 0, 0), Vector3(400, 0, 0), 3.0f);
    Movement::PointsArray back = Line(Vector3(400, 0, 0), Vector3(2, 0, 0), 3.0f);
    uturn.insert(uturn.end(), back.begin() + 1, back.end());
    CHECK(!AT::SplineFitsPacket(uturn));

    // Ausreisser und Sonderfaelle.
    Movement::PointsArray nan2 = ok;
    nan2[5].z = std::numeric_limits<float>::quiet_NaN();
    CHECK(!AT::SplineFitsPacket(nan2));
    Movement::PointsArray two;
    two.push_back(Vector3(0, 0, 0));
    two.push_back(Vector3(5000, 0, 0));
    CHECK(AT::SplineFitsPacket(two));                                // nichts zu packen
    CHECK(AT::SplineFitsPacket(Movement::PointsArray()));
}

static void TestSplitLongSegments()
{
    using G3D::Vector3;

    // Ein 746-yd-Segment wird in Stuecke von hoechstens 40 yd geteilt, die
    // Hoehe verteilt sich linear, Endpunkt bleibt erhalten.
    Movement::PointsArray path;
    path.push_back(Vector3(0, 0, 100));                 // Index 0: Start
    path.push_back(Vector3(0, -746, 130));              // Index 1: Ziel
    Vector3 const origin(0, 0, 100);

    CHECK(AT::SplitLongSegments(path, 1, origin, AT::SPLINE_MAX_SEGMENT));
    CHECK(path.size() == 20);                           // 746 / 40 -> 19 Stuecke, 18 neue Punkte + 2 alte
    CHECK(path.front().z == 100.0f);                    // Davorliegendes unberuehrt
    CHECK(path.back().y == -746.0f && path.back().z == 130.0f);

    float longest = 0.0f;
    bool monotone = true;
    for (size_t i = 2; i < path.size(); ++i)
    {
        longest = std::max(longest, AT::Dist2D(path[i].x, path[i].y, path[i - 1].x, path[i - 1].y));
        if (path[i].z + 1e-4f < path[i - 1].z)
            monotone = false;
    }
    CHECK(longest <= AT::SPLINE_MAX_SEGMENT + 0.01f);
    CHECK(monotone);

    // Ein zweiter Durchgang aendert nichts mehr.
    size_t const before = path.size();
    CHECK(!AT::SplitLongSegments(path, 1, origin, AT::SPLINE_MAX_SEGMENT));
    CHECK(path.size() == before);

    // Teilen ab der Mitte: das erste verbleibende Segment beginnt bei 'origin'
    // (der Spielerposition), nicht beim vorigen Pfadpunkt.
    Movement::PointsArray p2;
    p2.push_back(Vector3(0, 0, 0));
    p2.push_back(Vector3(10, 0, 0));
    p2.push_back(Vector3(10, 200, 0));
    CHECK(AT::SplitLongSegments(p2, 2, Vector3(10, 0, 0), 40.0f));
    CHECK(p2.size() == 3 + 4);                          // 200 / 40 -> 5 Stuecke, 4 neue Punkte
    CHECK(p2[0].x == 0.0f && p2[1].x == 10.0f);

    // Kurze Wege und Randfaelle.
    Movement::PointsArray shortPath;
    shortPath.push_back(Vector3(0, 0, 0));
    shortPath.push_back(Vector3(30, 0, 0));
    CHECK(!AT::SplitLongSegments(shortPath, 1, Vector3(0, 0, 0), 40.0f));
    CHECK(!AT::SplitLongSegments(shortPath, 5, Vector3(0, 0, 0), 40.0f));   // from hinter dem Ende
    CHECK(!AT::SplitLongSegments(shortPath, 1, Vector3(0, 0, 0), 0.0f));    // unsinnige Laenge

    // Nach dem Teilen passt jede Gruppe aufeinanderfolgender Punkte, die
    // LaunchChunk nehmen darf (hier: 12 Punkte), in die Paketkodierung.
    Movement::PointsArray big;
    big.push_back(Vector3(0, 0, 0));
    big.push_back(Vector3(0, -5000, 0));
    AT::SplitLongSegments(big, 1, Vector3(0, 0, 0), AT::SPLINE_MAX_SEGMENT);
    bool fitsEverywhere = true;
    for (size_t i = 1; i + 12 <= big.size(); ++i)
    {
        Movement::PointsArray window(big.begin() + i, big.begin() + i + 12);
        if (!AT::SplineFitsPacket(window))
            fitsEverywhere = false;
    }
    CHECK(fitsEverywhere);
}

// ---------------------------------------------------------------------------
// Kuerzester Weg im Knotengraphen
// ---------------------------------------------------------------------------
//
// Ein kleiner Graph nach dem Fall aus einem Spielerbericht: Brachland (Karte 1) nach
// dem Hafen von Sturmwind (Karte 0).
//
//   Karte 1:   0 Brachland (!)  - 1 Zeppelinturm  - 2 Ratchet
//   Karte 571: 10 Kriegshymnenfeste - 11 Valianzfeste
//   Karte 0:   21 Booty Bay - 22 Dunkelwald - 20 Hafen Sturmwind
//   Karte 369: 30 Tiefenbahn Ost - 31 Tiefenbahn West
//   Karte 429: 40 Dire Maul (Instanz)
//
// Der Knoten 0 ist mit Absicht der Start: die Knoten-ID 0 ist ein echter Knoten.
//   Nordend:  0 -> 1 (100) -> Zeppelin -> 10 -> 11 (1300) -> Schiff -> 20     ~2200
//   Ratchet:  0 -> 2 (500) -> Schiff -> 21 -> 22 (4000) -> 20 (4000)          ~9300

namespace
{
    struct TestGraph
    {
        std::unordered_map<uint32, ATNode> nodes;
        std::unordered_map<uint32, std::vector<ATNodeLink>> links;

        void Node(uint32 id, uint32 map)
        {
            ATNode n;
            n.id = id;
            n.mapId = map;
            n.name = "n" + std::to_string(id);
            nodes[id] = n;
        }
        void Link(uint32 a, uint32 b, uint8 type, float cost, uint32 object = 0, bool both = true)
        {
            ATNodeLink l;
            l.to = b;
            l.type = type;
            l.baseCost = cost;
            l.object = object;
            links[a].push_back(l);
            if (both)
            {
                l.to = a;
                links[b].push_back(l);
            }
        }
    };

    TestGraph MakeGraph()
    {
        TestGraph g;
        g.Node(0, 1); g.Node(1, 1); g.Node(2, 1);
        g.Node(10, 571); g.Node(11, 571);
        g.Node(21, 0); g.Node(22, 0); g.Node(20, 0);
        g.Node(30, 369); g.Node(31, 369);
        g.Node(40, 429);

        g.Link(0, 1, 1, 100);
        g.Link(0, 2, 1, 500);
        g.Link(1, 10, 3, 1);                // Zeppelin nach Nordend
        g.Link(10, 11, 1, 1300);
        g.Link(11, 20, 3, 1);               // Schiff nach Sturmwind
        g.Link(2, 21, 3, 1);                // Schiff Ratchet -> Booty Bay
        g.Link(21, 22, 1, 4000);
        g.Link(22, 20, 1, 4000);
        return g;
    }

    bool Has(std::vector<uint32> const& chain, uint32 id)
    {
        return std::find(chain.begin(), chain.end(), id) != chain.end();
    }

    AT::ChainRules Rules(std::initializer_list<uint32> maps)
    {
        AT::ChainRules r;
        r.allowedMaps.insert(maps.begin(), maps.end());
        return r;
    }
}

static void TestShortestChain()
{
    std::vector<uint32> chain;
    std::unordered_map<uint32, uint8> types;
    std::unordered_set<uint64> none;
    std::string note;

    // --- ohne Kartenfilter nimmt die Suche den billigsten Weg: ueber Nordend ------
    {
        TestGraph g = MakeGraph();
        AT::ChainRules open;                                  // leer = alle Karten
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 20, none, open, chain, types, note));
        CHECK(!chain.empty() && chain.front() == 0 && chain.back() == 20);   // Knoten 0 als Start
        CHECK(Has(chain, 10) && Has(chain, 11));
        CHECK(types[10] == 3 && types[20] == 3 && types[1] == 1);
    }

    // --- mit dem Filter: Ratchet, nicht Nordend -----------------------------------
    {
        TestGraph g = MakeGraph();
        AT::ChainRules r = Rules({ 0, 1, 369 });
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 20, none, r, chain, types, note));
        CHECK(!Has(chain, 10) && !Has(chain, 11));
        CHECK(Has(chain, 2) && Has(chain, 21) && Has(chain, 22));
        CHECK(chain.size() == 5);                             // 0, 2, 21, 22, 20
        CHECK(types[21] == 3);

        // Start oder Ziel in Nordend: dann ist es erlaubt
        AT::ChainRules north = Rules({ 0, 1, 369, 571 });
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 11, none, north, chain, types, note));
        CHECK(Has(chain, 10));
        AT::ChainRules noNorth = Rules({ 0, 1, 369 });
        CHECK(!AT::ShortestChain(g.nodes, g.links, 0, 11, none, noNorth, chain, types, note));
        CHECK(chain.empty());
    }

    // --- Instanzen sind nur Ziel, nie Abkuerzung ----------------------------------
    {
        TestGraph g = MakeGraph();
        g.Link(1, 40, 2, 1);                 // Eingang ...
        g.Link(40, 22, 2, 1);                // ... und ein "Ausgang" mitten in Sturmwinds Hinterland
        AT::ChainRules r = Rules({ 0, 1, 369 });
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 20, none, r, chain, types, note));
        CHECK(!Has(chain, 40));
        AT::ChainRules inInstance = Rules({ 0, 1, 369, 429 });
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 40, none, inInstance, chain, types, note));
        CHECK(chain.back() == 40);
    }

    // --- Tiefenbahn (369) bleibt als Durchgang erlaubt ----------------------------
    {
        TestGraph g = MakeGraph();
        g.Link(22, 30, 2, 1);                // Eingang
        g.Link(30, 31, 3, 1);                // Bahn
        g.Link(31, 20, 2, 1);                // Ausgang direkt am Hafen
        AT::ChainRules r = Rules({ 0, 1, 369 });
        CHECK(AT::ShortestChain(g.nodes, g.links, 21, 20, none, r, chain, types, note));
        CHECK(Has(chain, 30) && Has(chain, 31));
    }

    // --- gesperrte Kanten ----------------------------------------------------------
    {
        TestGraph g = MakeGraph();
        AT::ChainRules open;
        std::unordered_set<uint64> banned;
        banned.insert(AT::EdgeId(1, 10));                     // der Zeppelin faellt weg
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 20, banned, open, chain, types, note));
        CHECK(!Has(chain, 10) && Has(chain, 21));
        banned.insert(AT::EdgeId(2, 21));
        CHECK(!AT::ShortestChain(g.nodes, g.links, 0, 20, banned, open, chain, types, note));
        CHECK(!note.empty());
    }

    // --- Pruefung je Sonderverbindung (Flug, Portal) --------------------------------
    {
        TestGraph g = MakeGraph();
        g.Link(0, 22, 4, 1);                 // ein "Flug" direkt hin
        AT::ChainRules r = Rules({ 0, 1, 369 });
        int asked = 0;
        r.linkUsable = [&](uint32, ATNodeLink const& l) { ++asked; return l.type != 4; };
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 20, none, r, chain, types, note));
        CHECK(asked > 0);                                     // die Flugkante wurde geprueft
        CHECK(types[22] == 1);                                // aber nicht ueber die Flugkante

        AT::ChainRules ok = Rules({ 0, 1, 369 });
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 20, none, ok, chain, types, note));
        CHECK(types[22] == 4);                                // erlaubt: der Flug ist billiger
    }

    // --- Portale mit "object" ----------------------------------------------------
    {
        TestGraph g = MakeGraph();
        g.Link(0, 21, 2, 1, 1103);           // "Portal" direkt nach Booty Bay
        AT::ChainRules r = Rules({ 0, 1, 369 });
        r.linkUsable = [](uint32, ATNodeLink const& l) { return !(l.type == 2 && l.object == 1103); };
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 20, none, r, chain, types, note));
        CHECK(types[21] == 3);                                // ueber das Schiff, nicht das tote Portal
    }

    // --- unbekannter Verbindungstyp, nur zu Fuss ---------------------------------
    {
        TestGraph g = MakeGraph();
        g.Link(0, 20, 5, 1);                 // Typ 5: nicht benutzbar
        g.Link(0, 20, 0, 1);                 // Typ 0: ebenso
        AT::ChainRules r = Rules({ 0, 1, 369 });
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 20, none, r, chain, types, note));
        CHECK(chain.size() == 5);

        AT::ChainRules walk = Rules({ 0, 1, 369 });
        walk.useSpecialLinks = false;
        CHECK(!AT::ShortestChain(g.nodes, g.links, 0, 20, none, walk, chain, types, note));   // ohne Schiff kein Weg

        // Sonderverbindungen kosten Aufschlag: ein langer Fussweg schlaegt eine teure Verbindung
        TestGraph h;
        h.Node(1, 0); h.Node(2, 0); h.Node(3, 0);
        h.Link(1, 2, 3, 10);                 // Schiff: 10 + 400
        h.Link(2, 3, 1, 10);
        h.Link(1, 3, 1, 300);                // zu Fuss: 300
        AT::ChainRules cost;
        CHECK(AT::ShortestChain(h.nodes, h.links, 1, 3, none, cost, chain, types, note));
        CHECK(chain.size() == 2);            // direkt
        cost.specialLinkCost = 0.0f;
        CHECK(AT::ShortestChain(h.nodes, h.links, 1, 3, none, cost, chain, types, note));
        CHECK(chain.size() == 3);            // ohne Aufschlag wird das Schiff genommen
    }

    // --- Fraktion der Transporte ----------------------------------------------------
    {
        CHECK(AT::TransportFaction(175080) == 2);             // Iron Eagle: Horde
        CHECK(AT::TransportFaction(186238) == 2);             // Mighty Wind: Horde
        CHECK(AT::TransportFaction(190536) == 1);             // Stormwind's Pride: Allianz
        CHECK(AT::TransportFaction(176310) == 1);             // The Bravery: Allianz
        CHECK(AT::TransportFaction(20808) == 0);              // The Maiden's Fancy: beide
        CHECK(AT::TransportFaction(0) == 0);
        CHECK(AT::TransportFaction(999999) == 0);

        // Allianzcharakter im Brachland: kein Zeppelin, der Weg geht ueber Ratchet
        TestGraph g = MakeGraph();
        g.links.clear();
        g.Link(0, 1, 1, 100);
        g.Link(0, 2, 1, 500);
        g.Link(1, 22, 3, 1, 175080);          // Horde-Zeppelin nach Sturmwind (billig)
        g.Link(2, 21, 3, 1, 20808);           // Schiff fuer beide
        g.Link(21, 22, 1, 4000);
        g.Link(22, 20, 1, 100);
        AT::ChainRules ally = Rules({ 0, 1, 369 });
        ally.linkUsable = [](uint32, ATNodeLink const& l)
        {
            if (l.type != 3)
                return true;
            uint8 const f = AT::TransportFaction(l.object);
            return f == 0 || f == 1;           // Allianz
        };
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 20, none, ally, chain, types, note));
        CHECK(Has(chain, 2) && Has(chain, 21) && !Has(chain, 1));

        AT::ChainRules horde = Rules({ 0, 1, 369 });
        horde.linkUsable = [](uint32, ATNodeLink const& l)
        {
            if (l.type != 3)
                return true;
            uint8 const f = AT::TransportFaction(l.object);
            return f == 0 || f == 2;           // Horde
        };
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 20, none, horde, chain, types, note));
        CHECK(Has(chain, 1) && !Has(chain, 2));
    }

    // --- Randfaelle ----------------------------------------------------------------
    {
        TestGraph g = MakeGraph();
        AT::ChainRules open;
        CHECK(!AT::ShortestChain(g.nodes, g.links, 0, 999, none, open, chain, types, note));   // Ziel fehlt
        CHECK(!AT::ShortestChain(g.nodes, g.links, 999, 20, none, open, chain, types, note));  // Start fehlt
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 0, none, open, chain, types, note));      // Start = Ziel
        CHECK(chain.size() == 1 && chain[0] == 0);

        AT::ChainRules tiny;
        tiny.maxVisited = 2;
        CHECK(!AT::ShortestChain(g.nodes, g.links, 0, 20, none, tiny, chain, types, note));    // Obergrenze greift

        // Kante auf einen Knoten ohne Eintrag
        g.Link(0, 777, 1, 1, 0, false);
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 20, none, open, chain, types, note));

        // Kette ueber 400 Knoten wird abgelehnt
        TestGraph longG;
        for (uint32 i = 0; i < 450; ++i)
            longG.Node(i, 0);
        for (uint32 i = 0; i + 1 < 450; ++i)
            longG.Link(i, i + 1, 1, 1);
        CHECK(!AT::ShortestChain(longG.nodes, longG.links, 0, 449, none, open, chain, types, note));
        CHECK(chain.empty());
        CHECK(AT::ShortestChain(longG.nodes, longG.links, 0, 300, none, open, chain, types, note));
        CHECK(chain.size() == 301);
    }

    // --- Ergebnis ist die billigste Kette, nicht die mit den wenigsten Knoten ------
    {
        TestGraph g;
        for (uint32 i = 0; i < 6; ++i)
            g.Node(i, 0);
        g.Link(0, 5, 1, 1000);               // direkt, teuer
        g.Link(0, 1, 1, 10);
        g.Link(1, 2, 1, 10);
        g.Link(2, 3, 1, 10);
        g.Link(3, 5, 1, 10);                 // vier Schritte, 40
        AT::ChainRules open;
        CHECK(AT::ShortestChain(g.nodes, g.links, 0, 5, none, open, chain, types, note));
        CHECK(chain.size() == 5);
    }
}

// ---------------------------------------------------------------------------
// Zielanker
// ---------------------------------------------------------------------------
//
// Die Spawndaten stammen aus der Basis-Datenbank von AzerothCore (creature.sql); der
// Fall "Ganaar" ist der gemeldete: die Exodar liegt rund 130 yd unter dem Gelaende
// darueber, und die Hoehensuche nach der Spielerposition fuehrte aus der Stadt hinaus.

static AT::AnchorCand Cand(uint64 guid, uint32 entry, uint32 flags, float x, float y, float z)
{
    AT::AnchorCand c;
    c.spawnId = guid;
    c.entry = entry;
    c.flags = flags;
    c.x = x;
    c.y = y;
    c.z = z;
    return c;
}

static void TestLabelMatchesName()
{
    // Das Label ist Carbonites englischer Text; verglichen wird gegen den englischen
    // Namen der Datenbank, nicht gegen den, den ein deutscher Client anzeigt.
    CHECK(AT::LabelMatchesName("High Chief Stillpine", "High Chief Stillpine"));
    CHECK(AT::LabelMatchesName("high chief stillpine", "High Chief Stillpine"));
    CHECK(AT::LabelMatchesName("HIGH CHIEF STILLPINE", "High Chief Stillpine"));
    CHECK(AT::LabelMatchesName("Ganaar", "Ganaar"));
    CHECK(AT::LabelMatchesName("Kill High Chief Stillpine", "High Chief Stillpine"));    // Name im Label
    CHECK(AT::LabelMatchesName("Speak with High Chief Stillpine (1/1)", "High Chief Stillpine"));
    // Nur der Name im Label zaehlt, nicht ein Label im laengeren Namen: ein Wegpunkt, der
    // nur "Stormwind City" heisst, trifft nicht jede "Stormwind City Guard".
    CHECK(!AT::LabelMatchesName("Stillpine", "High Chief Stillpine"));
    CHECK(!AT::LabelMatchesName("Stormwind City", "Stormwind City Guard"));
    CHECK(AT::LabelMatchesName("Stormwind City Guard", "Stormwind City Guard"));
    CHECK(AT::LabelMatchesName("  Ganaar  ", "Ganaar"));                                // Leerraum

    // Die deutsche Fassung ist NICHT der Name in der Datenbank.
    CHECK(!AT::LabelMatchesName("Oberhaeuptling der Tannenruhfeste", "High Chief Stillpine"));
    CHECK(!AT::LabelMatchesName("Tierausbilder", "Ganaar"));

    // Wortgrenzen: kein Teilwort.
    CHECK(!AT::LabelMatchesName("Boars", "Boar"));
    CHECK(!AT::LabelMatchesName("Banker", "Bank"));
    CHECK(!AT::LabelMatchesName("Grommash", "Grom"));
    CHECK(!AT::LabelMatchesName("Quest Ganaarx", "Ganaar"));

    // Zu kurz: stimmt mit zu vielem ueberein.
    CHECK(!AT::LabelMatchesName("Bob", "Bob"));
    CHECK(!AT::LabelMatchesName("Wache", "Die Wache"));
    CHECK(!AT::LabelMatchesName("Chief", "High Chief Stillpine"));
    CHECK(!AT::LabelMatchesName("Chief Stillpine", "High Chief Stillpine"));
    CHECK(!AT::LabelMatchesName("", "Ganaar"));
    CHECK(!AT::LabelMatchesName("Ganaar", ""));
    CHECK(!AT::LabelMatchesName("", ""));

    // Satzzeichen trennen Woerter, der Apostroph gehoert zum Wort; Umlaute sind Buchstaben.
    CHECK(AT::LabelMatchesName("Ar'kelos", "Ar'kelos"));
    CHECK(AT::LabelMatchesName("Quest: Ar'kelos, the Bold", "Ar'kelos"));
    CHECK(!AT::LabelMatchesName("Arkelos", "Ar'kelos"));
    CHECK(AT::LabelMatchesName("Stallmeister M\xC3\xBCller-L\xC3\xB6wenzahn", "M\xC3\xBCller-L\xC3\xB6wenzahn"));
    CHECK(!AT::LabelMatchesName("M\xC3\xBCller", "Mueller"));
}

static void TestAnchorFlagsText()
{
    CHECK(AT::AnchorFlagsText(0) == "-");
    CHECK(AT::AnchorFlagsText(AT::ANCHOR_INTERACTIVE) == "Dienst");
    CHECK(AT::AnchorFlagsText(AT::ANCHOR_INTERACTIVE | AT::ANCHOR_OBJECT) == "Objekt");
    CHECK(AT::AnchorFlagsText(AT::ANCHOR_INTERACTIVE | AT::ANCHOR_NAME | AT::ANCHOR_QUEST) == "Dienst, Name, Quest");
    CHECK(AT::AnchorFlagsText(AT::ANCHOR_QUEST | AT::ANCHOR_MOVER) == "Quest, laeuft umher");
}

static void TestPickAnchorGanaar()
{
    using namespace AT;
    AnchorRules rules;            // 12 / 25 / 40 yd, Etage 6 yd

    // Die Hunter-Terrasse der Exodar (Karte 530), echte Spawns. Das Reh auf dem Gelaende
    // darueber (z +13,6, 20 yd entfernt) hat kein NPC-Flag und kommt gar nicht erst vor.
    std::vector<AnchorCand> c;
    c.push_back(Cand(31750, 16712, ANCHOR_INTERACTIVE, -4229.28f, -11549.6f, -125.946f));   // Ganaar
    c.push_back(Cand(85592, 16735, ANCHOR_INTERACTIVE, -4230.67f, -11549.6f, -125.979f));   // Muhaa
    c.push_back(Cand(63006, 17505, ANCHOR_INTERACTIVE, -4233.06f, -11556.1f, -126.032f));   // Killac
    c.push_back(Cand(57737, 16738, ANCHOR_INTERACTIVE, -4222.30f, -11563.9f, -125.935f));   // Deremiis, 15,9 yd

    AnchorPick p;
    CHECK(PickAnchor(c, -4229.28f, -11549.6f, rules, p));
    CHECK(p.found && c[p.index].entry == 16712);
    CHECK(p.dist2D < 0.01f);
    CHECK(p.floors == 1 && p.altCount == 0);
    CHECK(std::fabs(c[p.index].z - (-125.946f)) < 0.001f);

    // Klick 2,5 yd daneben: weiter Ganaar.
    CHECK(PickAnchor(c, -4226.78f, -11549.6f, rules, p) && c[p.index].entry == 16712);
    CHECK(p.floors == 1);

    // Deremiis (15,9 yd) liegt ausserhalb der 12 yd; ein Klick direkt bei ihm trifft ihn trotzdem.
    CHECK(PickAnchor(c, -4222.30f, -11563.9f, rules, p) && c[p.index].entry == 16738);
    // ... aber ein Klick 15 yd von allen entfernt trifft keinen: altes Verfahren.
    CHECK(!PickAnchor(c, -4229.28f, -11530.0f, rules, p));

    // Klick neben Muhaa: ohne Label gewinnt die Naehe (dieselbe Etage), mit dem Namen
    // "Ganaar" gewinnt er.
    CHECK(PickAnchor(c, -4231.5f, -11549.6f, rules, p) && c[p.index].entry == 16735);
    c[0].flags |= ANCHOR_NAME;
    CHECK(PickAnchor(c, -4231.5f, -11549.6f, rules, p) && c[p.index].entry == 16712);
    CHECK(p.floors == 1);
}

static void TestPickAnchorFloors()
{
    using namespace AT;
    AnchorRules rules;

    // Exodar, Ruestungsanbau: Yil (z -94,0) und Gotaan (z -132,1), 9 yd auseinander.
    std::vector<AnchorCand> ex;
    ex.push_back(Cand(57743, 16750, ANCHOR_INTERACTIVE, -4280.97f, -11778.2f, -93.953f));   // Yil
    ex.push_back(Cand(57745, 16753, ANCHOR_INTERACTIVE, -4286.98f, -11771.5f, -132.132f));  // Gotaan
    AnchorPick p;
    CHECK(PickAnchor(ex, -4280.97f, -11778.2f, rules, p) && ex[p.index].entry == 16750);
    CHECK(p.floors == 2 && p.altCount == 1 && ex[p.alt[0]].entry == 16753);

    // Mit 8 yd Radius gehoert Gotaan nicht mehr dazu.
    AnchorRules narrow = rules;
    narrow.radiusInteractive = 8.0f;
    CHECK(PickAnchor(ex, -4280.97f, -11778.2f, narrow, p) && p.floors == 1 && p.altCount == 0);

    // Dalaran: der Bankier (z 620) steht 1,2 yd neben einer Haendlerin auf z 653,7.
    std::vector<AnchorCand> dal;
    dal.push_back(Cand(104104, 29530, ANCHOR_INTERACTIVE, 5766.59f, 734.04f, 620.134f));    // Binzik Goldbook
    dal.push_back(Cand(4761, 37942, ANCHOR_INTERACTIVE, 5765.45f, 734.38f, 653.748f));      // Arcanist Uovril
    dal.push_back(Cand(85238, 35790, ANCHOR_INTERACTIVE, 5770.65f, 734.66f, 618.66f));      // Usuri Brightcoin
    dal.push_back(Cand(100025, 28715, ANCHOR_INTERACTIVE, 5759.27f, 732.16f, 641.777f));    // Endora Moorehead
    dal.push_back(Cand(60320, 194147, ANCHOR_INTERACTIVE | ANCHOR_OBJECT, 5763.82f, 726.93f, 618.59f)); // Briefkasten
    dal.push_back(Cand(104206, 31579, ANCHOR_INTERACTIVE, 5764.79f, 744.0f, 653.748f));     // Arcanist Adurin
    CHECK(PickAnchor(dal, 5766.59f, 734.04f, rules, p) && dal[p.index].entry == 29530);
    CHECK(p.floors == 3 && p.altCount == 2);
    CHECK(dal[p.alt[0]].z > 600.0f && dal[p.alt[1]].z > 600.0f);
    CHECK(std::fabs(dal[p.alt[0]].z - dal[p.index].z) > 6.0f && std::fabs(dal[p.alt[1]].z - dal[p.index].z) > 6.0f);
    CHECK(std::fabs(dal[p.alt[0]].z - dal[p.alt[1]].z) > 6.0f);

    // Das Label "Binzik Goldbook" trifft den Bankier: dann gibt es keine Ersatzetagen,
    // denn der Name ist ein staerkerer Beleg als "irgendein NPC in der Naehe".
    dal[0].flags |= ANCHOR_NAME;
    CHECK(PickAnchor(dal, 5766.59f, 734.04f, rules, p) && dal[p.index].entry == 29530);
    CHECK(p.floors == 3 && p.altCount == 0);

    // Klickt der Spieler eher bei der Haendlerin (0,3 yd) und nennt den Bankier: Name gewinnt.
    CHECK(PickAnchor(dal, 5765.2f, 734.3f, rules, p) && dal[p.index].entry == 29530);

    // Die Etagentoleranz: 5 yd Unterschied sind eine Etage, 7 yd zwei.
    std::vector<AnchorCand> t;
    t.push_back(Cand(1, 1, ANCHOR_INTERACTIVE, 0, 0, 0.0f));
    t.push_back(Cand(2, 2, ANCHOR_INTERACTIVE, 1, 0, 5.0f));
    CHECK(PickAnchor(t, 0, 0, rules, p) && p.floors == 1);
    t[1].z = 7.0f;
    CHECK(PickAnchor(t, 0, 0, rules, p) && p.floors == 2 && p.altCount == 1);
}

static void TestPickAnchorQuest()
{
    using namespace AT;
    AnchorRules rules;
    AnchorPick p;

    // "High Chief Stillpine" (Eintrag 17440) auf Azuremyst: Questgeber, Abgabe und Ziel mehrerer Quests.
    // Carbonite zeigt ihn mit seinem englischen Namen; der Kartenpunkt liegt nicht auf dem Spawn.
    std::vector<AnchorCand> c;
    c.push_back(Cand(62915, 17440, ANCHOR_INTERACTIVE | ANCHOR_NAME | ANCHOR_QUEST, -3353.36f, -12400.5f, 26.0876f));
    CHECK(PickAnchor(c, -3383.36f, -12400.5f, rules, p) && c[p.index].entry == 17440);   // 30 yd daneben
    CHECK(p.dist2D > 29.9f && p.dist2D < 30.1f);
    CHECK(!PickAnchor(c, -3400.0f, -12400.5f, rules, p));                                 // 46,6 yd: zu weit, altes Verfahren

    // Ohne Name und Quest reicht derselbe Abstand nicht (12 yd).
    c[0].flags = ANCHOR_INTERACTIVE;
    CHECK(!PickAnchor(c, -3383.36f, -12400.5f, rules, p));
    CHECK(PickAnchor(c, -3355.36f, -12400.5f, rules, p));

    // Ein Questziel, das kein NPC mit Dienst ist (ein Tier, das getoetet werden soll),
    // taugt als Anker fuer die Etage, wenn der Kartenpunkt in der Naehe liegt.
    std::vector<AnchorCand> k;
    k.push_back(Cand(500, 17000, ANCHOR_QUEST, 100.0f, 100.0f, 42.0f));
    CHECK(PickAnchor(k, 130.0f, 100.0f, rules, p) && k[p.index].entry == 17000);
    CHECK(!PickAnchor(k, 145.0f, 100.0f, rules, p));

    // Steht ein NPC mit Dienst direkt am Kartenpunkt, schlaegt er ein 35 yd entferntes Questziel.
    k.push_back(Cand(501, 17001, ANCHOR_INTERACTIVE, 133.0f, 100.0f, 50.0f));
    CHECK(PickAnchor(k, 135.0f, 100.0f, rules, p) && k[p.index].entry == 17001);   // 2 yd gegen 35 yd
    // Liegt der Klick dagegen beim Questziel, gewinnt das.
    CHECK(PickAnchor(k, 101.0f, 100.0f, rules, p) && k[p.index].entry == 17000);
}

static void TestPickAnchorRules()
{
    using namespace AT;
    AnchorRules rules;
    AnchorPick p;

    // Kandidaten ohne Merkmal sind keine Anker (das Reh 1,5 yd neben dem Lehrer).
    std::vector<AnchorCand> c;
    c.push_back(Cand(61441, 17201, 0, -4227.92f, -11698.6f, 6.61f));                           // Moongraze Buck
    c.push_back(Cand(57744, 16752, ANCHOR_INTERACTIVE, -4228.17f, -11697.1f, -143.47f));       // Muaat
    c.push_back(Cand(85599, 16751, ANCHOR_INTERACTIVE, -4234.13f, -11699.4f, -143.54f));       // Merran
    CHECK(PickAnchor(c, -4227.92f, -11698.6f, rules, p) && c[p.index].entry == 16752);
    CHECK(p.floors == 1);
    c.erase(c.begin() + 1, c.end());
    CHECK(!PickAnchor(c, -4227.92f, -11698.6f, rules, p));                                      // nur das Reh: kein Anker

    // Leere Liste, NaN und Unendlich.
    std::vector<AnchorCand> none;
    CHECK(!PickAnchor(none, 0, 0, rules, p) && !p.found);
    std::vector<AnchorCand> one;
    one.push_back(Cand(1, 1, ANCHOR_INTERACTIVE, 0, 0, 0));
    float const nan = std::numeric_limits<float>::quiet_NaN();
    float const inf = std::numeric_limits<float>::infinity();
    CHECK(!PickAnchor(one, nan, 0, rules, p));
    CHECK(!PickAnchor(one, 0, inf, rules, p));
    one[0].x = nan;
    CHECK(!PickAnchor(one, 0, 0, rules, p));
    one[0].x = 0;
    one[0].z = inf;
    CHECK(!PickAnchor(one, 0, 0, rules, p));
    one[0].z = 0;
    CHECK(PickAnchor(one, 0, 0, rules, p));

    // Radius 0 schaltet die jeweilige Klasse aus.
    AnchorRules off;
    off.radiusInteractive = 0.0f;
    CHECK(!PickAnchor(one, 0, 0, off, p));

    // Wer herumlaeuft, verliert gegen einen Stehenden gleicher Entfernung.
    std::vector<AnchorCand> m;
    m.push_back(Cand(10, 1, ANCHOR_INTERACTIVE | ANCHOR_MOVER, 5.0f, 0.0f, 0.0f));
    m.push_back(Cand(11, 2, ANCHOR_INTERACTIVE, 0.0f, 5.0f, 0.0f));
    CHECK(PickAnchor(m, 0, 0, rules, p) && m[p.index].entry == 2);

    // Gleichstand: mehr Belege, dann Kreatur vor Objekt, dann kleinere Spawn-ID.
    std::vector<AnchorCand> g;
    g.push_back(Cand(30, 3, ANCHOR_INTERACTIVE | ANCHOR_OBJECT, 3.0f, 0.0f, 0.0f));
    g.push_back(Cand(20, 2, ANCHOR_INTERACTIVE, 0.0f, 3.0f, 0.0f));
    CHECK(PickAnchor(g, 0, 0, rules, p) && g[p.index].entry == 2);                   // Kreatur vor Objekt
    g.push_back(Cand(10, 1, ANCHOR_INTERACTIVE, -3.0f, 0.0f, 0.0f));
    CHECK(PickAnchor(g, 0, 0, rules, p) && g[p.index].entry == 1);                   // kleinere Spawn-ID

    // Stapel auf einem Punkt (Spawns an derselben Stelle): kein Absturz, eine Etage.
    std::vector<AnchorCand> st;
    for (uint64 i = 0; i < 6; ++i)
        st.push_back(Cand(100 + i, uint32(i + 1), ANCHOR_INTERACTIVE, 1.0f, 1.0f, 10.0f));
    CHECK(PickAnchor(st, 1.0f, 1.0f, rules, p) && p.floors == 1 && p.altCount == 0);

    // Mehr als zwei Ersatzetagen werden auf zwei begrenzt, die Etagenzahl bleibt wahr.
    std::vector<AnchorCand> many;
    for (int i = 0; i < 5; ++i)
        many.push_back(Cand(uint64(200 + i), uint32(i + 1), ANCHOR_INTERACTIVE, float(i) * 0.5f, 0.0f, float(i) * 20.0f));
    CHECK(PickAnchor(many, 0, 0, rules, p) && p.floors == 5 && p.altCount == 2);
}


// ---------------------------------------------------------------------------
// Teilweg in die Sackgasse? (AT::PartialLooksStuck)
// ---------------------------------------------------------------------------

static void TestPartialLooksStuck()
{
    // Der Fall aus dem Spielerbericht: Spieler in der Exodar (Z -130), Huntress Kella Nightbow an der
    // Oberflaeche 95 yd entfernt (Z 8.8). Der Teilweg endet an der Wand unter ihr.
    float const sx = -4203.5f, sy = -11415.1f, sz = -130.1f;
    float const dx = -4269.0f, dy = -11484.5f, dz = 8.8f;

    // endet im Innenraum, 6 yd neben der Stelle unter dem Ziel
    CHECK(AT::PartialLooksStuck(sx, sy, sz, dx, dy, dz, dx + 4.0f, dy - 4.5f, -130.0f));
    // endet 55 yd vom Ziel entfernt, auf derselben Etage wie der Start (Luftlinie kaum kleiner)
    CHECK(AT::PartialLooksStuck(sx, sy, sz, dx, dy, dz, dx + 40.0f, dy - 37.0f, -129.0f));
    // die Wand hinauf geklettert (Z -115): noch immer eine andere Etage
    CHECK(AT::PartialLooksStuck(sx, sy, sz, dx, dy, dz, dx + 3.0f, dy + 2.0f, -115.0f));

    // streng (Meldung an den Spieler): nur die Sackgasse unter dem Ziel
    CHECK(AT::PartialEndsUnderTarget(sx, sy, sz, dx, dy, dz, dx + 4.0f, dy - 4.5f, -130.0f));
    CHECK(AT::PartialEndsUnderTarget(sx, sy, sz, dx, dy, dz, dx + 3.0f, dy + 2.0f, -115.0f));
    // 55 yd neben dem Ziel, gleiche Etage wie der Start: Verdacht ja, Meldung nein
    CHECK(!AT::PartialEndsUnderTarget(sx, sy, sz, dx, dy, dz, dx + 40.0f, dy - 37.0f, -129.0f));
    // ein Ziel auf einer Anhoehe, 800 yd weit weg: der Teilweg endet 100 yd davor und 25 yd tiefer -- das ist
    // der Weg dorthin, keine Sackgasse unter dem Ziel
    CHECK(!AT::PartialEndsUnderTarget(0.0f, 0.0f, 0.0f, 800.0f, 0.0f, 50.0f, 700.0f, 0.0f, 25.0f));
    // dasselbe, 25 yd vor dem Ziel: unter dem Ziel
    CHECK(AT::PartialEndsUnderTarget(0.0f, 0.0f, 0.0f, 800.0f, 0.0f, 50.0f, 775.0f, 5.0f, 25.0f));
    // weiter als 60 yd ist es auch bei einem Weg von 4000 yd nicht mehr "darunter"
    CHECK(!AT::PartialEndsUnderTarget(0.0f, 0.0f, 0.0f, 4000.0f, 0.0f, 50.0f, 3900.0f, 0.0f, 20.0f));

    // Ein Weg ueber freies Gelaende, vom PathGenerator wegen seiner Laengengrenze abgeschnitten:
    // 1500 yd Luftlinie, der Teilweg kommt 450 yd weiter.
    CHECK(!AT::PartialLooksStuck(0.0f, 0.0f, 10.0f, 1500.0f, 0.0f, 12.0f, 450.0f, 20.0f, 11.0f));
    // dasselbe zu Beginn der Reise, aber nur 300 yd Fortschritt: 20 % -- noch kein Verdacht
    CHECK(!AT::PartialLooksStuck(0.0f, 0.0f, 10.0f, 1500.0f, 0.0f, 12.0f, 300.0f, 0.0f, 11.0f));
    // kaum Fortschritt: 5 % von 1500 yd
    CHECK(AT::PartialLooksStuck(0.0f, 0.0f, 10.0f, 1500.0f, 0.0f, 12.0f, 75.0f, 0.0f, 10.0f));

    // Ein Weg bergauf zum Ziel: Hoehenunterschied 60 yd, aber das Ende liegt weit vom Ziel
    // (200 yd) und schon deutlich naeher als der Start (600 yd).
    CHECK(!AT::PartialLooksStuck(0.0f, 0.0f, 0.0f, 600.0f, 0.0f, 60.0f, 400.0f, 0.0f, 40.0f));

    // Aus der Durchsicht: eine Sackgasse hinter einer Mauer, 40 yd seitlich vom Ziel und 30 yd tiefer,
    // 40 % Fortschritt -- weder "unter dem Ziel" noch "kaum Fortschritt", aber eine andere Hoehe in der Naehe.
    CHECK(AT::PartialLooksStuck(0.0f, 0.0f, 0.0f, 100.0f, 0.0f, 30.0f, 60.0f, 0.0f, 0.0f));
    CHECK(!AT::PartialEndsUnderTarget(0.0f, 0.0f, 0.0f, 100.0f, 0.0f, 30.0f, 60.0f, 0.0f, 0.0f));
    // dieselbe Hoehe, 10 yd Unterschied reichen aber nur in der Naehe: 300 yd entfernt kein Verdacht
    CHECK(!AT::PartialLooksStuck(0.0f, 0.0f, 0.0f, 1000.0f, 0.0f, 20.0f, 700.0f, 0.0f, 5.0f));

    // Der Teilweg endet praktisch am Ziel: es fehlt nur der letzte Schritt.
    CHECK(!AT::PartialLooksStuck(sx, sy, sz, dx, dy, dz, dx + 3.0f, dy, dz + 5.0f));
    CHECK(!AT::PartialLooksStuck(0.0f, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f, 95.0f, 0.0f, 0.0f));

    // Das Spiegelbild: der Spieler steht ueber der Stadt, das Ziel liegt eine Etage tiefer, der
    // Teilweg endet auf der Oberflaeche direkt darueber.
    CHECK(AT::PartialLooksStuck(dx, dy, dz, sx, sy, sz, sx - 5.0f, sy + 3.0f, 12.0f));

    // Unsinn hinein, kein Absturz und kein Verdacht heraus
    float const nan = std::numeric_limits<float>::quiet_NaN();
    float const inf = std::numeric_limits<float>::infinity();
    CHECK(!AT::PartialLooksStuck(nan, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f));
    CHECK(!AT::PartialLooksStuck(0.0f, 0.0f, 0.0f, inf, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f));
    CHECK(!AT::PartialLooksStuck(0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 1.0f, nan, 0.0f));
    // Start gleich Ziel
    CHECK(!AT::PartialLooksStuck(5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f));
}


// ---------------------------------------------------------------------------
// Nicht begehbares Endstueck eines Teilwegs (AT::IsVerticalJump / IsPinnedStep / TrimDeadTail)
// ---------------------------------------------------------------------------

struct TestPoint { float x, y, z; };

static void TestTrimDeadTail()
{
    // Treppen und Rampen sind keine Spruenge: Steigung 1,2 bis 2.
    CHECK(!AT::IsVerticalJump(0, 0, 0, 10, 0, 12));        // Steigung 1,2
    CHECK(!AT::IsVerticalJump(0, 0, 0, 5, 0, 10));         // Steigung 2
    CHECK(!AT::IsVerticalJump(0, 0, 0, 100, 0, 5));        // flach
    CHECK(!AT::IsVerticalJump(0, 0, 0, 0, 0, 11.9f));      // unter der Mindesthoehe
    // senkrecht und fast senkrecht
    CHECK(AT::IsVerticalJump(0, 0, 0, 0, 0, 139));
    CHECK(AT::IsVerticalJump(0, 0, 0, 3, 4, 139));         // 5 yd zur Seite, 139 hoch
    CHECK(AT::IsVerticalJump(0, 0, 0, 0, 0, -139));        // auch abwaerts
    CHECK(AT::IsVerticalJump(0, 0, 0, 4, 0, 12.5f));       // 12,5 > 2,5 * 4
    CHECK(!AT::IsVerticalJump(0, 0, 0, 5, 0, 12.5f));      // 12,5 = 2,5 * 5: gerade noch begehbar

    // Auf die Zielposition festgenagelt: das letzte Stueck endet genau auf ihr und ueberwindet
    // mindestens 20 yd. Auch eine lange Schraege zaehlt, die IsVerticalJump durchlaesst
    // (70 yd zur Seite, 139 hoch: Steigung 2).
    CHECK(!AT::IsVerticalJump(0, 0, 0, 70, 0, 139));
    CHECK(AT::IsPinnedStep(0, 0, 0, 70, 0, 139, 70, 0, 139));
    CHECK(AT::IsPinnedStep(0, 0, 139, 70, 0, 0, 70, 0, 0));          // auch abwaerts
    CHECK(AT::IsPinnedStep(0, 0, 0, 0, 0, 139, 0, 0, 139));          // senkrecht sowieso
    CHECK(!AT::IsPinnedStep(0, 0, 0, 70, 0, 139, 80, 0, 139));       // das Ziel liegt woanders
    CHECK(AT::IsPinnedStep(0, 0, 0, 70, 0, 139, 70.4f, 0, 139));     // Abweichung in der Ebene 0,4 yd
    CHECK(!AT::IsPinnedStep(0, 0, 0, 70, 0, 139, 70.6f, 0, 139));    // 0,6 yd
    CHECK(AT::IsPinnedStep(0, 0, 0, 70, 0, 139, 70, 0, 141.5f));     // Abweichung in der Hoehe 2,5 yd
    CHECK(!AT::IsPinnedStep(0, 0, 0, 70, 0, 139, 70, 0, 143.5f));    // 3,5 yd
    CHECK(!AT::IsPinnedStep(0, 0, 0, 10, 0, 15, 10, 0, 15));         // unter 20 yd Hoehe
    CHECK(AT::IsPinnedStep(0, 0, 0, 300, 0, 100, 300, 0, 100));      // Steigung 1:3
    CHECK(!AT::IsPinnedStep(0, 0, 0, 400, 0, 100, 400, 0, 100));     // 1:4 und flacher: eine Rampe

    // Der Teilweg aus dem Spielerbericht: dem NPC entgegen, der letzte Punkt ist seine Zielposition.
    std::vector<TestPoint> path = { { 0, 0, -130 }, { 20, 5, -130 }, { 38, 12, -130 }, { 40, 14, 8.8f } };
    CHECK(AT::TrimDeadTail(path, 40, 14, 8.8f) == 1);
    CHECK(path.size() == 3);
    CHECK(path.back().z == -130.0f);
    CHECK(AT::TrimDeadTail(path, 40, 14, 8.8f) == 0);      // das naechste Stueck ist flach: nichts mehr
    CHECK(path.size() == 3);

    // Dasselbe, wenn das Stueck senkrecht UND das Ziel ein anderes ist (der Core wurde mit einem
    // anderen Ziel gefragt als dem, auf dem der Weg endet): die Steilheit allein genuegt.
    {
        std::vector<TestPoint> p = { { 0, 0, -130 }, { 38, 12, -130 }, { 40, 14, 8.8f } };
        CHECK(AT::TrimDeadTail(p, 99, 99, 99) == 1);
    }

    // Nach dem Abschneiden sieht man die Sackgasse: das Ende liegt 5 yd neben der Stelle unter dem Ziel.
    {
        std::vector<TestPoint> p2 = { { 0, 0, -130 }, { 36, 10, -130 }, { 40, 14, 8.8f } };
        AT::TrimDeadTail(p2, 40, 14, 8.8f);
        CHECK(AT::PartialLooksStuck(0, 0, -130, 40, 14, 8.8f, p2.back().x, p2.back().y, p2.back().z));
        CHECK(AT::PartialEndsUnderTarget(0, 0, -130, 40, 14, 8.8f, p2.back().x, p2.back().y, p2.back().z));
    }
    // Ohne das Abschneiden wuerde der letzte Punkt "praktisch am Ziel" liegen und nichts melden.
    CHECK(!AT::PartialLooksStuck(0, 0, -130, 40, 14, 8.8f, 40, 14, 8.8f));

    // Eine lange Schraege in einem grossen Polygon: Start und festgenageltes Ziel, 70 yd auseinander.
    {
        std::vector<TestPoint> big = { { 0, 0, -130 }, { 70, 10, 8.8f } };
        CHECK(AT::TrimDeadTail(big, 70, 10, 8.8f) == 1);
        CHECK(big.size() == 1);
        // Der Rest ist der Start: kein Fortschritt, die Erkennung schlaegt an.
        CHECK(AT::PartialLooksStuck(0, 0, -130, 70, 10, 8.8f, big[0].x, big[0].y, big[0].z));
    }
    // Endet der Weg dagegen NICHT auf der Zielposition und ist nicht senkrecht, bleibt er.
    {
        std::vector<TestPoint> big = { { 0, 0, -130 }, { 70, 10, 8.8f } };
        CHECK(AT::TrimDeadTail(big, 90, 10, 8.8f) == 0);
        CHECK(big.size() == 2);
    }

    // allowPinned = false (der Weg ist nur unvollstaendig, weil der START weit neben dem NavMesh liegt):
    // [Start, Ziel] in einer langen Rampe ist ein ehrlicher Weg und bleibt; ein senkrechtes Stueck faellt trotzdem.
    {
        std::vector<TestPoint> ramp2 = { { 0, 0, 0 }, { 108, 0, 54 } };
        CHECK(AT::TrimDeadTail(ramp2, 108, 0, 54, false) == 0);
        CHECK(ramp2.size() == 2);
        CHECK(AT::TrimDeadTail(ramp2, 108, 0, 54, true) == 1);          // mit dem Standard wuerde es als festgenagelt abgeschnitten
        std::vector<TestPoint> vert = { { 0, 0, -130 }, { 0, 0, 8.8f } };
        CHECK(AT::TrimDeadTail(vert, 0, 0, 8.8f, false) == 1);
    }

    // Nur Start und Sprung: es bleibt ein Punkt, nichts zu laufen.
    std::vector<TestPoint> two = { { 0, 0, -130 }, { 0, 0, 8.8f } };
    CHECK(AT::TrimDeadTail(two, 0, 0, 8.8f) == 1);
    CHECK(two.size() == 1);
    CHECK(AT::TrimDeadTail(two, 0, 0, 8.8f) == 0);         // ein einzelner Punkt hat kein Stueck

    // leer und ein Punkt
    std::vector<TestPoint> none;
    CHECK(AT::TrimDeadTail(none, 0, 0, 0) == 0);

    // Ein steiles, aber begehbares Ende (Rampe) bleibt.
    std::vector<TestPoint> ramp = { { 0, 0, 0 }, { 10, 0, 6 }, { 20, 0, 18 } };
    CHECK(AT::TrimDeadTail(ramp, 20, 0, 18) == 0);         // 12 yd: unter der Grenze fuer festgenagelte Stuecke
    CHECK(ramp.size() == 3);
    CHECK(AT::TrimDeadTail(ramp, 99, 99, 99) == 0);
    CHECK(ramp.size() == 3);
}

int main()
{
    TestParseUInt();
    TestParseFloat();
    TestParseBool();
    TestParseNorm();
    TestSanitize();
    TestIdentifier();
    TestMisc();
    TestSplinePacket();
    TestSplitLongSegments();
    TestShortestChain();
    TestLabelMatchesName();
    TestAnchorFlagsText();
    TestPickAnchorGanaar();
    TestPickAnchorFloors();
    TestPickAnchorQuest();
    TestPickAnchorRules();
    TestPartialLooksStuck();
    TestTrimDeadTail();

    std::printf("%d Pruefungen, %d Fehler\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
