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
}
