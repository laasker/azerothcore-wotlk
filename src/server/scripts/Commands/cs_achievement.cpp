/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "AchievementMgr.h"
#include "ChatCommandTags.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Language.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"

using namespace Acore::ChatCommands;

class achievement_commandscript : public CommandScript
{
public:
    achievement_commandscript() : CommandScript("achievement_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable achievementCommandTable =
        {
            { "add",      HandleAchievementAddCommand,      rbac::RBAC_PERM_COMMAND_ACHIEVEMENT_ADD,      Console::Yes },
            { "checkall", HandleAchievementCheckAllCommand, rbac::RBAC_PERM_COMMAND_ACHIEVEMENT_CHECKALL, Console::Yes }
        };
        static ChatCommandTable commandTable =
        {
            { "achievement", achievementCommandTable }
        };
        return commandTable;
    }

    static bool HandleAchievementAddCommand(ChatHandler* handler, AchievementEntry const* achievementEntry, Tail playerNameArg)
    {
        Player* targetPlayer = nullptr;
        ObjectGuid targetGuid;
        std::string targetName;

        if (!playerNameArg.empty())
        {
            std::string nameArgument(playerNameArg);
            targetName = nameArgument.find("|Hplayer:") != std::string::npos
                ? handler->extractPlayerNameFromLink(nameArgument.data())
                : nameArgument;

            if (targetName.empty() || !normalizePlayerName(targetName))
            {
                handler->SendErrorMessage(LANG_PLAYER_NOT_FOUND);
                return false;
            }

            targetPlayer = ObjectAccessor::FindPlayerByName(targetName, false);
            targetGuid = targetPlayer ? targetPlayer->GetGUID() : sCharacterCache->GetCharacterGuidByName(targetName);

            if (!targetGuid)
            {
                handler->SendErrorMessage(LANG_PLAYER_NOT_FOUND);
                return false;
            }
        }
        else
        {
            Optional<PlayerIdentifier> player = PlayerIdentifier::FromTargetOrSelf(handler);
            if (!player)
            {
                handler->SendErrorMessage(LANG_PLAYER_NOT_FOUND);
                return false;
            }

            targetName = player->GetName();
            targetGuid = player->GetGUID();
            if (player->IsConnected())
                targetPlayer = player->GetConnectedPlayer();
        }

        if (targetPlayer)
        {
            targetPlayer->CompletedAchievement(achievementEntry);
            handler->PSendSysMessage(LANG_ACHIEVEMENT_ADD_ONLINE, achievementEntry->ID, achievementEntry->name[0], targetName);
        }
        else
        {
            sAchievementMgr->CompletedAchievementForOfflinePlayer(targetGuid.GetCounter(), achievementEntry);
            handler->PSendSysMessage(LANG_ACHIEVEMENT_ADD_OFFLINE, achievementEntry->ID, achievementEntry->name[0], targetName);
        }

        return true;
    }

    static bool HandleAchievementCheckAllCommand(ChatHandler* handler, Optional<PlayerIdentifier> player)
    {
        if (!player)
        {
            player = PlayerIdentifier::FromTarget(handler);
        }

        if (!player)
        {
            handler->SendErrorMessage(LANG_PLAYER_NOT_FOUND);
            return false;
        }

        if (player->IsConnected())
        {
            if (Player* target = player->GetConnectedPlayer())
                target->CheckAllAchievementCriteria();
        }
        else
        {
            auto* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_ADD_AT_LOGIN_FLAG);
            stmt->SetData(0, uint16(AT_LOGIN_CHECK_ACHIEVS));
            stmt->SetData(1, player->GetGUID().GetCounter());
            CharacterDatabase.Execute(stmt);
        }

        return true;
    }
};

void AddSC_achievement_commandscript()
{
    new achievement_commandscript();
}
