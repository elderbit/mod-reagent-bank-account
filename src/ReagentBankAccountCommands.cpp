#include "ScriptMgr.h"
#include "Chat.h"
#include "Player.h"
#include "AccountMgr.h"
#include "ObjectMgr.h"

class reagentbank_commands : public CommandScript
{
public:
    reagentbank_commands() : CommandScript("reagentbank_commands") {}

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> auditSub = {
            {"audit", SEC_GAMEMASTER, false, &HandleAuditSummary, "Syntax: .reagentbank audit <accountId> [guid=<guid>] [top=<N>] [page=<p>] [pageSize=<s>]"}};
        static std::vector<ChatCommand> purgeSub = {
            {"purge", SEC_GAMEMASTER, false, &HandlePurge, "Syntax: .reagentbank purge <accountId> [guid=<guid>] [olderThan=<seconds>]"}};
        static std::vector<ChatCommand> root = {
            {"reagentbank", SEC_GAMEMASTER, false, nullptr, "Reagent bank commands", auditSub},
            {"reagentbank", SEC_GAMEMASTER, false, nullptr, "Reagent bank commands", purgeSub}};
        return root;
    }

    // Parse key=value style optional arguments
    static std::map<std::string, std::string> ParseArgs(char const *args)
    {
        std::map<std::string, std::string> out;
        if (!args)
            return out;
        std::string s(args);
        std::istringstream iss(s);
        std::string token;
        while (iss >> token)
        {
            auto pos = token.find('=');
            if (pos == std::string::npos)
                continue;
            out[token.substr(0, pos)] = token.substr(pos + 1);
        }
        return out;
    }

    static bool HandleAuditSummary(ChatHandler *handler, char const *args)
    {
        if (!*args)
        {
            handler->PSendSysMessage("Usage: .reagentbank audit <accountId> [guid=<guid>] [top=<N>] [page=<p>] [pageSize=<s>]");
            return false;
        }
        // First token is accountId
        std::string working(args);
        std::istringstream iss(working);
        std::string acctToken;
        iss >> acctToken;
        uint32 accountId = strtoul(acctToken.c_str(), nullptr, 10);
        std::string rest;
        std::getline(iss, rest);
        auto kv = ParseArgs(rest.c_str());

        uint32 guid = kv.count("guid") ? strtoul(kv["guid"].c_str(), nullptr, 10) : 0;
        uint32 topN = kv.count("top") ? strtoul(kv["top"].c_str(), nullptr, 10) : 5;
        if (topN == 0)
            topN = 5;
        if (topN > 50)
            topN = 50;
        uint32 page = kv.count("page") ? strtoul(kv["page"].c_str(), nullptr, 10) : 1;
        if (page == 0)
            page = 1;
        uint32 pageSize = kv.count("pageSize") ? strtoul(kv["pageSize"].c_str(), nullptr, 10) : 20;
        if (pageSize == 0)
            pageSize = 20;
        if (pageSize > 200)
            pageSize = 200;
        uint32 offset = (page - 1) * pageSize;

        std::string where = "WHERE account_id = " + std::to_string(accountId);
        if (guid)
            where += " AND guid = " + std::to_string(guid);

        QueryResult countRes = CharacterDatabase.Query("SELECT COUNT(*) FROM mod_reagent_bank_audit " + where);
        uint64 totalRows = countRes ? (*countRes)[0].Get<uint64>() : 0;
        QueryResult summary = CharacterDatabase.Query("SELECT action, COUNT(*), SUM(delta) FROM mod_reagent_bank_audit " + where + " GROUP BY action");

        handler->PSendSysMessage("ReagentBank Audit Summary for account %u%s:", accountId, guid ? (" guid=" + std::to_string(guid)).c_str() : "");
        if (summary)
        {
            do
            {
                std::string action = (*summary)[0].Get<std::string>();
                uint64 rows = (*summary)[1].Get<uint64>();
                int64 totalDelta = (*summary)[2].Get<int64>();
                handler->PSendSysMessage("  %s: events=%u total=%lld", action.c_str(), (uint32)rows, (long long)totalDelta);
            } while (summary->NextRow());
        }
        else
            handler->PSendSysMessage("  (no events)");

        // Top items with human-readable names
        std::string topWhere = where;
        QueryResult topItems = CharacterDatabase.PQuery(
            "SELECT item_entry, item_subclass, SUM(CASE WHEN action='DEPOSIT' THEN delta ELSE -delta END) AS net "
            "FROM mod_reagent_bank_audit %s GROUP BY item_entry, item_subclass ORDER BY ABS(net) DESC LIMIT %u",
            topWhere.c_str(), topN);
        handler->PSendSysMessage("Top %u net movement items:", topN);
        if (topItems)
        {
            do
            {
                uint32 entry = (*topItems)[0].Get<uint32>();
                uint32 subclass = (*topItems)[1].Get<uint32>();
                int64 net = (*topItems)[2].Get<int64>();
                if (ItemTemplate const *proto = sObjectMgr->GetItemTemplate(entry))
                    handler->PSendSysMessage("  %s (entry %u subclass %u): net %lld", proto->Name1.c_str(), entry, subclass, (long long)net);
                else
                    handler->PSendSysMessage("  Item %u (subclass %u): net %lld", entry, subclass, (long long)net);
            } while (topItems->NextRow());
        }
        else
            handler->PSendSysMessage("  (no items)");

        // Paginated rows (most recent first)
        QueryResult pageRows = CharacterDatabase.PQuery(
            "SELECT ts, action, item_entry, item_subclass, delta FROM mod_reagent_bank_audit %s ORDER BY id DESC LIMIT %u OFFSET %u", where.c_str(), pageSize, offset);
        handler->PSendSysMessage("Events page %u size %u (total %u rows):", page, pageSize, (uint32)totalRows);
        if (pageRows)
        {
            do
            {
                uint32 ts = (*pageRows)[0].Get<uint32>();
                std::string action = (*pageRows)[1].Get<std::string>();
                uint32 entry = (*pageRows)[2].Get<uint32>();
                uint32 subclass = (*pageRows)[3].Get<uint32>();
                int32 delta = (*pageRows)[4].Get<int32>();
                if (ItemTemplate const *proto = sObjectMgr->GetItemTemplate(entry))
                    handler->PSendSysMessage("  [%u] %s %s (entry %u subclass %u) delta %d", ts, action.c_str(), proto->Name1.c_str(), entry, subclass, delta);
                else
                    handler->PSendSysMessage("  [%u] %s item %u subclass %u delta %d", ts, action.c_str(), entry, subclass, delta);
            } while (pageRows->NextRow());
        }
        else
            handler->PSendSysMessage("  (no rows for this page)");
        return true;
    }

    static bool HandlePurge(ChatHandler *handler, char const *args)
    {
        if (!*args)
        {
            handler->PSendSysMessage("Usage: .reagentbank purge <accountId> [guid=<guid>] [olderThan=<seconds>]");
            return false;
        }
        std::string working(args);
        std::istringstream iss(working);
        std::string acctToken;
        iss >> acctToken;
        uint32 accountId = strtoul(acctToken.c_str(), nullptr, 10);
        std::string rest;
        std::getline(iss, rest);
        auto kv = ParseArgs(rest.c_str());
        uint32 guid = kv.count("guid") ? strtoul(kv["guid"].c_str(), nullptr, 10) : 0;
        uint32 olderThan = kv.count("olderThan") ? strtoul(kv["olderThan"].c_str(), nullptr, 10) : 0;
        uint32 cutoff = olderThan ? (uint32)time(nullptr) - olderThan : 0;

        std::string where = "account_id = " + std::to_string(accountId);
        if (guid)
            where += " AND guid = " + std::to_string(guid);
        if (cutoff)
            where += " AND ts < " + std::to_string(cutoff);

        QueryResult countBefore = CharacterDatabase.PQuery("SELECT COUNT(*) FROM mod_reagent_bank_audit WHERE %s", where.c_str());
        uint64 rows = countBefore ? (*countBefore)[0].Get<uint64>() : 0;
        CharacterDatabase.DirectPExecute("DELETE FROM mod_reagent_bank_audit WHERE %s", where.c_str());
        handler->PSendSysMessage("Purged %u audit rows for account %u%s%s", (uint32)rows, accountId,
                                 guid ? (" guid=" + std::to_string(guid)).c_str() : "",
                                 cutoff ? (" olderThanSeconds=" + std::to_string(olderThan)).c_str() : "");
        return true;
    }
};

void AddSC_reagentbank_commands()
{
    new reagentbank_commands();
}
