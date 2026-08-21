#include "AutoTravel.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <cstdio>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // ---- Gepruefte Umwandlung -------------------------------------------
    // Diese Werte stammen aus einer Chatnachricht des Spielers. atoi/atof
    // liefern bei Unsinn still eine 0, atof("nan") sogar NaN -- und NaN
    // pflanzt sich durch die gesamte Wegfindung fort.

    bool ParseUInt(std::string const& in, uint32& out)
    {
        if (in.empty()) return false;
        char* end = nullptr;
        errno = 0;
        unsigned long v = std::strtoul(in.c_str(), &end, 10);
        if (errno == ERANGE || end == in.c_str() || *end != '\0') return false;
        if (v > 0xFFFFFFFFul) return false;
        out = uint32(v);
        return true;
    }

    bool ParseFloat(std::string const& in, float& out)
    {
        if (in.empty()) return false;
        char* end = nullptr;
        errno = 0;
        double v = std::strtod(in.c_str(), &end);
        if (errno == ERANGE || end == in.c_str() || *end != '\0') return false;
        if (!std::isfinite(v)) return false;
        out = float(v);
        return true;
    }

    bool ParseBool(std::string const& in, bool& out)
    {
        uint32 v = 0;
        if (!ParseUInt(in, v) || v > 1) return false;
        out = (v != 0);
        return true;
    }

    bool ParseNorm(std::string const& in, float& out)
    {
        if (!ParseFloat(in, out)) return false;
        return out >= 0.0f && out <= 1.0f;   // normalisierte Kartenkoordinate
    }

    std::vector<std::string> Split(std::string const& in)
    {
        std::vector<std::string> out;
        std::istringstream iss(in);
        std::string t;
        while (iss >> t)
            out.push_back(t);
        return out;
    }

    std::string JoinFrom(std::vector<std::string> const& v, size_t start)
    {
        std::string r;
        for (size_t i = start; i < v.size(); ++i)
        {
            if (!r.empty())
                r += " ";
            r += v[i];
        }
        return r;
    }
}

class autotravel_commandscript : public CommandScript
{
public:
    autotravel_commandscript() : CommandScript("autotravel_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table =
        {
            { "at", HandleAt, SEC_PLAYER, Console::No }
        };
        return table;
    }

    // Syntax (wird ausschliesslich vom Addon erzeugt):
    //   .at start   <uiMapId> <nx> <ny> <hasCalib> <pnx> <pny> <curMap> <cnx> <cny> <Name...>
    //   .at tp      <uiMapId> <nx> <ny> <hasCalib> <pnx> <pny> <curMap> <cnx> <cny> <Name...>
    //   .at resolve <uiMapId> <nx> <ny> <hasCalib> <pnx> <pny> <curMap> <cnx> <cny>
    //
    // curMap/cnx/cny sind die Karten-ID und die normalisierte Position der
    // Zone, in der der Spieler GERADE steht. Damit kann der Server die
    // Zuordnung Client-ID -> WorldMapArea-ID auch dann pruefen, wenn das Ziel
    // in einer anderen Zone liegt.
    //   .at stop
    //   .at repath
    //   .at status
    //   .at debug <0|1>
    //   .at set <key> <value>
    static bool HandleAt(ChatHandler* handler, Tail argsTail)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::vector<std::string> a = Split(std::string(argsTail));
        if (a.empty())
        {
            sAutoTravel->PrintStatus(player);
            return true;
        }

        std::string const& cmd = a[0];

        if (cmd == "start" || cmd == "tp" || cmd == "resolve" || cmd == "diag")
        {
            if (a.size() < 7)
            {
                handler->SendSysMessage("[AT]M|Ungueltige Parameter.");
                return true;
            }
            uint32 uiMapId = 0;
            float  nx = 0.0f, ny = 0.0f, pnx = 0.0f, pny = 0.0f;
            bool   hasCalib = false;

            if (!ParseUInt(a[1], uiMapId) || !uiMapId
                || !ParseNorm(a[2], nx) || !ParseNorm(a[3], ny)
                || !ParseBool(a[4], hasCalib)
                || !ParseNorm(a[5], pnx) || !ParseNorm(a[6], pny))
            {
                handler->SendSysMessage("[AT]M|Ungueltige Parameter - Befehl abgewiesen.");
                return true;
            }

            // Teleport umgeht jede Wegfindung und nimmt beliebige
            // Zielkoordinaten entgegen. Ohne diese Pruefung koennte sich jeder
            // Spieler ueberallhin versetzen -- der Befehl lag bisher unter
            // demselben SEC_PLAYER wie alles andere.
            if (cmd == "tp")
            {
                if (handler->GetSession()->GetSecurity() <
                    AccountTypes(ATConf.teleportSecurity))
                {
                    handler->SendSysMessage("[AT]M|Teleport ist dir nicht erlaubt.");
                    return true;
                }
            }
            if (a.size() >= 10)
            {
                uint32 curMap = 0;
                float  cnx = 0.0f, cny = 0.0f;
                if (ParseUInt(a[7], curMap) && curMap
                    && ParseNorm(a[8], cnx) && ParseNorm(a[9], cny))
                {
                    sAutoTravel->LearnMapId(player, curMap, cnx, cny);
                }
            }

            std::string name = JoinFrom(a, 10);

            if (cmd == "start")
                sAutoTravel->Start(player, uiMapId, nx, ny, hasCalib, pnx, pny, name);
            else if (cmd == "tp")
                sAutoTravel->Teleport(player, uiMapId, nx, ny, hasCalib, pnx, pny, name);
            else if (cmd == "diag")
                sAutoTravel->Diagnose(player, uiMapId, nx, ny, hasCalib, pnx, pny);
            else
                sAutoTravel->Resolve(player, uiMapId, nx, ny, hasCalib, pnx, pny);
            return true;
        }

        if (cmd == "route")
        {
            // .at route <0=neu|1=anhaengen> <map:nx:ny:flags> ...
            if (a.size() < 3) return true;
            uint32 mode = 0;
            if (!ParseUInt(a[1], mode)) return true;
            bool clearFirst = (mode == 0);
            sAutoTravel->RouteAdd(player, clearFirst, JoinFrom(a, 2));
            return true;
        }

        if (cmd == "rstart")
        {
            // .at rstart <curMap> <cnx> <cny> <Name...>
            if (a.size() >= 4)
            {
                uint32 curMap = 0;
                float  cnx = 0.0f, cny = 0.0f;
                if (ParseUInt(a[1], curMap) && curMap
                    && ParseNorm(a[2], cnx) && ParseNorm(a[3], cny))
                {
                    sAutoTravel->LearnMapId(player, curMap, cnx, cny);
                }
                sAutoTravel->RouteStart(player, JoinFrom(a, 4));
            }
            return true;
        }

        if (cmd == "nodes")
        {
            sAutoTravel->NodeInfo(player);
            return true;
        }

        if (cmd == "stop")
        {
            sAutoTravel->Stop(player, "Reise gestoppt.");
            return true;
        }

        if (cmd == "repath")
        {
            sAutoTravel->Repath(player);
            return true;
        }

        if (cmd == "status")
        {
            sAutoTravel->PrintStatus(player);
            return true;
        }

        if (cmd == "debug")
        {
            bool on = true;
            if (a.size() > 1 && !ParseBool(a[1], on))
            {
                handler->SendSysMessage("[AT]M|Ungueltiger Parameter.");
                return true;
            }
            sAutoTravel->SetDebug(player, on);
            return true;
        }

        if (cmd == "set" && a.size() >= 3)
        {
            sAutoTravel->SetOption(player, a[1], a[2]);
            return true;
        }

        handler->SendSysMessage("[AT]M|Unbekannter Unterbefehl.");
        return true;
    }
};

class autotravel_worldscript : public WorldScript
{
public:
    autotravel_worldscript() : WorldScript("autotravel_worldscript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sAutoTravel->LoadConfig();
    }

    void OnUpdate(uint32 diff) override
    {
        sAutoTravel->Update(diff);
    }
};

void AddSC_autotravel()
{
    new autotravel_commandscript();
    new autotravel_worldscript();
}
