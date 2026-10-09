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

    std::printf("%d Pruefungen, %d Fehler\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
