#include "AutoTravel.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
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
    //   .at start   <uiMapId> <nx> <ny> <hasCalib> <pnx> <pny> <Zielname...>
    //   .at tp      <uiMapId> <nx> <ny> <hasCalib> <pnx> <pny> <Zielname...>
    //   .at resolve <uiMapId> <nx> <ny> <hasCalib> <pnx> <pny>
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

        if (cmd == "start" || cmd == "tp" || cmd == "resolve")
        {
            if (a.size() < 7)
            {
                handler->SendSysMessage("[AT]M|Ungueltige Parameter.");
                return true;
            }
            uint32 uiMapId  = uint32(atoi(a[1].c_str()));
            float  nx       = float(atof(a[2].c_str()));
            float  ny       = float(atof(a[3].c_str()));
            bool   hasCalib = atoi(a[4].c_str()) != 0;
            float  pnx      = float(atof(a[5].c_str()));
            float  pny      = float(atof(a[6].c_str()));
            std::string name = JoinFrom(a, 7);

            if (cmd == "start")
                sAutoTravel->Start(player, uiMapId, nx, ny, hasCalib, pnx, pny, name);
            else if (cmd == "tp")
                sAutoTravel->Teleport(player, uiMapId, nx, ny, hasCalib, pnx, pny, name);
            else
                sAutoTravel->Resolve(player, uiMapId, nx, ny, hasCalib, pnx, pny);
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
            bool on = a.size() > 1 ? atoi(a[1].c_str()) != 0 : true;
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
