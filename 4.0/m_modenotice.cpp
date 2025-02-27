/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2018 Sadie Powell <sadie@witchery.services>
 *   Copyright (C) 2013-2014 Attila Molnar <attilamolnar@hush.com>
 *   Copyright (C) 2012, 2019 Robby <robby@chatbelgie.be>
 *   Copyright (C) 2009-2010 Daniel De Graaf <danieldg@inspircd.org>
 *
 * This file is part of InspIRCd.  InspIRCd is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/// $ModAuthor: InspIRCd Developers
/// $ModDepends: core 4
/// $ModDesc: Adds the /MODENOTICE command which sends a message to all users with the specified user modes set.


#include "inspircd.h"

class CommandModeNotice : public Command
{
 public:
	CommandModeNotice(Module* parent) : Command(parent,"MODENOTICE",2,2)
	{
		syntax = { "<modeletters> :<message>" };
		access_needed = CmdAccess::OPERATOR;
	}

	CmdResult Handle(User* src, const Params& parameters) override
	{
		std::string msg = "*** From " + src->nick + ": " + parameters[1];
		int mlen = parameters[0].length();
		const UserManager::LocalList& list = ServerInstance->Users.GetLocalUsers();
		for (UserManager::LocalList::const_iterator i = list.begin(); i != list.end(); ++i)
		{
			User* user = *i;
			for (int n = 0; n < mlen; n++)
			{
				if (!user->IsModeSet(parameters[0][n]))
					goto next_user;
			}
			user->WriteNotice(msg);
next_user:	;
		}
		return CmdResult::SUCCESS;
	}

	RouteDescriptor GetRouting(User* user, const Params& parameters) override
	{
		return ROUTE_OPT_BCAST;
	}
};

class ModuleModeNotice : public Module
{
 private:
	CommandModeNotice cmd;

 public:
	ModuleModeNotice()
		: Module(VF_NONE, "Adds the /MODENOTICE command which sends a message to all users with the specified user modes set.")
		, cmd(this)
	{
	}
};

MODULE_INIT(ModuleModeNotice)
