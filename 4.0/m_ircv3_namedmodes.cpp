/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2021, Val Lorentz <progval+inspircd@progval.net>
 *   Copyright (C) 2017 B00mX0r <b00mx0r@aureus.pw>
 *   Copyright (C) 2013-2016 Attila Molnar <attilamolnar@hush.com>
 *   Copyright (C) 2013, 2017-2019 Sadie Powell <sadie@witchery.services>
 *   Copyright (C) 2012, 2019 Robby <robby@chatbelgie.be>
 *   Copyright (C) 2010 Craig Edwards <brain@inspircd.org>
 *   Copyright (C) 2009-2010 Daniel De Graaf <danieldg@inspircd.org>
 *
 * This file is a module for InspIRCd.  It is free software: you can
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

/******************************************************************************
 *                                 WARNING                                    *
 *                                                                            *
 * This module implementes a work-in-progress implementation.                 *
 * This module itself is a prototype, and should not be used in production,   *
 * as it may cause crashes, privilege escalation, data leaks, and various     *
 * other bugs.                                                                *
 * It is intended for testing client implementation of the draft/named-modes  *
 * specification and should not be used in production.                        *
 ******************************************************************************/

#include "inspircd.h"
#include "listmode.h"
#include "modules/cap.h"
#include "modules/ircv3.h"
#include "modules/ircv3_replies.h"
#include "modules/isupport.h"

/* $ModAuthor: Val Lorentz */
/* $ModAuthorMail: progval+inspircd@progval.net */
/* $ModDesc: Prototype implementation of the work-in-progress IRCv3 draft/named-modes: https://github.com/progval/ircv3-specifications/blob/named-modes/extensions/named-modes.md . Do not use this in production. */
/* $ModDepends: core 4.0 */
/* $ModConflicts: m_namedmodes.so */


enum
{
	// IRCv3 named-modes
	RPL_ENDOFPROPLIST = 960,
	RPL_PROPLIST = 961,
	RPL_ENDOFLISTPROPLIST = 962,
	RPL_LISTPROPLIST = 963,
	RPL_CHMODELIST = 964,
	RPL_UMODELIST = 965,
};

static void DisplayModeList(LocalUser* user, Channel* channel)
{
	Numeric::ParamBuilder<1> numeric(user, RPL_PROPLIST);
	numeric.AddStatic(channel->name);

	for (const auto& [_, mh] : ServerInstance->Modes.GetModes(MODETYPE_CHANNEL))
	{
		if (!channel->IsModeSet(mh))
			continue;

		numeric.Add(mh->name);
		ParamModeBase* pm = mh->IsParameterMode();
		if (pm)
		{
			if ((pm->IsParameterSecret()) && (!channel->HasUser(user)) && (!user->HasPrivPermission("channels/auspex")))
				numeric.Add("<" + mh->name + ">");
			else
				numeric.Add(channel->GetModeParameter(mh));
		}
	}
	numeric.Flush();
	user->WriteNumeric(RPL_ENDOFPROPLIST, channel->name, "End of mode list");
}

/* Handles PROP commands from clients */
class CommandProp final
	: public SplitCommand
{
 private:
	IRCv3::Replies::Fail fail;

 public:
	CommandProp(Module* parent)
		: SplitCommand(parent, "PROP", 1)
		, fail(parent)
	{
		syntax = { "<channel> (<mode>|(+|-)<mode>=[<value>])+" };
	}

	CmdResult HandleLocal(LocalUser* src, const Params& parameters) override
	{
		Channel* const chan = ServerInstance->Channels.Find(parameters[0]);
		if (!chan)
		{
			src->WriteNumeric(Numerics::NoSuchChannel(parameters[0]));
			return CmdResult::FAILURE;
		}

		if (parameters.size() == 1)
		{
			DisplayModeList(src, chan);
			return CmdResult::SUCCESS;
		}
		unsigned int i = 1;
		Modes::ChangeList modes;
		bool success = true;
		while (i < parameters.size() && success)
		{
			std::string prop = parameters[i++];
			if (prop.empty())
				continue;
			switch (prop[0]) {
				case '+':
					/* Request to set a mode */
					success = ChangeMode(src, prop.substr(1), true, modes);
					break;
				case '-':
					/* Request to unset a mode */
					success = ChangeMode(src, prop.substr(1), false, modes);
					break;
				default:
					/* Handle listmode list request */
					success = ListMode(src, chan, prop);
					break;
			}

		}
		if (success) {
			ServerInstance->Modes.ProcessSingle(src, chan, NULL, modes, ModeParser::MODE_CHECKACCESS);
			return CmdResult::SUCCESS;
		}
		else {
			fail.Send(src, this, "UNKNOWN", "unknown error");
			return CmdResult::FAILURE;
		}
	}

	/* Adds an item to the ChangeList.
	 *
	 * @param prop should be either 'name' or 'name=value' from the client
	 * @param plus: whether it's prefixed with a '+' or a '-'
	 * @param modes the ChangeList to update if possible.
	 * @return whether the prop was valid.
	 */
	bool ChangeMode(LocalUser *user, const std::string &prop, bool plus, Modes::ChangeList &modes) {
		std::size_t separator_position = prop.find("=");

		std::string mode_name = prop.substr(0, separator_position);

		ModeHandler* mh = ServerInstance->Modes.FindMode(mode_name, MODETYPE_CHANNEL);
		if (!mh) {
			/* This mode does not exist */
			fail.Send(user, this, "UNKNOWN_MODE", mode_name + " is not a valid mode name");
			return false;
		}

		if (mh->NeedsParam(plus))
		{
			if (separator_position == std::string::npos) {
				/* Expected a value but didn't get one */
				fail.Send(user, this, "MISSING_VALUE", prop + " requires a value");
				return false;
			}
			else {
				modes.push(mh, plus, prop.substr(separator_position+1));
				return true;
			}
		}
		else {
			if (separator_position == std::string::npos) {
				modes.push(mh, plus);
				return true;
			}
			else {
				/* Got a value but didn't expect it */
				fail.Send(user, this, "UNEXPECTED_VALUE", prop + " does not take a value");
				return false;
			}
		}
	}

	/* Replies to a PROP list request.
	 *
	 * @param user local user who requested the list
	 * @param channel channel the user requested the list for
	 * @param prop name of the mode
	 * @return whether the prop was valid.
	 */
	bool ListMode(LocalUser* user, Channel* channel, const std::string &prop) {
		if (prop.find("=") != std::string::npos) {
			fail.Send(user, this, "INVALID_SYNTAX", "PROP list request should not have a value");
			return false;
		}

		ModeHandler* mh = ServerInstance->Modes.FindMode(prop, MODETYPE_CHANNEL);

		if (!mh) {
			fail.Send(user, this, "UNKNOWN_MODE", prop + " is not a valid mode name");
			return false;
		}

		ListModeBase* listmode = mh->IsListModeBase();

		if (!listmode) {
			fail.Send(user, this, "NOT_LISTMODE", prop + " is not a list mode");
			return false;
		}

		ListModeBase::ModeList* modelist = listmode->GetList(channel);

		if (modelist) {
			/* NULL if the list is empty */
			for (const auto& item : *modelist)
				user->WriteNumeric(RPL_LISTPROPLIST, channel->name, prop, item.mask, item.setter, item.time);
		}
		user->WriteNumeric(RPL_ENDOFLISTPROPLIST, channel->name, prop, "End of mode list");

		return true;
	}
};

/* Handles MODE commands sent to clients, and rewrites them as a PROP command */
class ModeHook final
	: public ClientProtocol::EventHook
{
	std::vector<ClientProtocol::Message*> propmsgs;
	Cap::Capability cap;

 public:

	ModeHook(Module* mod)
		: ClientProtocol::EventHook(mod, "MODE", PRIORITY_LAST) /* Run last so that other modules (eg. m_hidemode) can do their thing, before we convert them to PROP, which they don't know how to handle */
		, cap(mod, "draft/named-modes")
	{
	}

	void OnEventInit(const ClientProtocol::Event& ev) override
	{
		const ClientProtocol::Events::Mode& modeev = static_cast<const ClientProtocol::Events::Mode&>(ev);

		propmsgs.clear();

		if (modeev.GetMessages().empty()) {
			/* This should never happen; other modules (eg. m_hidemode) should return MOD_RES_DENY when filtering so further events are not triggered with an empty list of messages. */
			ServerInstance->Logs.Log(MODNAME, LOG_DEFAULT, "Error: m_ircv3_namedmodes got MODE event with empty message list.");
			return;
		}

		const ClientProtocol::Messages::Mode& first_modemsg = modeev.GetMessages().front();
		const std::string* source = first_modemsg.GetSource(); /* Should be the same for all messages. */
		User* source_user = first_modemsg.GetSourceUser(); /* ditto */
		std::string target = first_modemsg.GetParams().front(); /* ditto */

		/* TODO: Here, we create one PROP for each change in the MODE. This is correct, but wasteful; so we should merge them into a minimal number of PROPs (while not exceeding the 512 byte limit) */

		for (auto &change : modeev.GetChangeList().getlist()) {

			if (!change.mh) {
				/* This should never happen. */
				ServerInstance->Logs.Log(MODNAME, LOG_DEFAULT, "Error: m_ircv3_namedmodes got MODE event with NULL handler.");
				return;
			}

			ClientProtocol::Message* propmsg;
			if (source) {
				propmsg = new ClientProtocol::Message("PROP", *source, source_user);
			}
			else {
				propmsg = new ClientProtocol::Message("PROP", source_user);
			}

			if (!change.mh) {
				/* This should never happen. */
				ServerInstance->Logs.Log(MODNAME, LOG_DEFAULT, "Error: m_ircv3_namedmodes got MODE event with NULL handler.");
				delete propmsg;
				continue;
			}

			propmsg->PushParam(target);

			char plus_or_minus = change.adding ? '+' : '-';
			if (change.param.empty()) {
				propmsg->PushParam(plus_or_minus + change.mh->name);
			}
			else {
				propmsg->PushParam(plus_or_minus + change.mh->name + "=" + change.param);
			}
			propmsgs.push_back(propmsg);
		}
	}

	ModResult OnPreEventSend(LocalUser* user, const ClientProtocol::Event& ev, ClientProtocol::MessageList& messagelist) override
	{
		const ClientProtocol::Events::Mode& modeev = static_cast<const ClientProtocol::Events::Mode&>(ev);
		size_t nb_modemsgs = modeev.GetMessages().size();

		if (cap.IsEnabled(user)) {
			/* FIXME: We should filter some PROPs here, or m_hidemode.cpp becomes useless. */

			/* FIXME: There are always at least as many PROPs as MODEs, right? */

			/* Overwrite the mode with the first PROP */
			/* FIXME: shouldn't we delete the first messages in messagelist? I tried doing that, but it causes a double-free, so it looks like it is taken care of elsewhere? But then, who deallocates the messages we are writing here? */
			std::copy(propmsgs.begin(), propmsgs.begin() + nb_modemsgs, messagelist.begin());

			/* Insert the other PROPs (if any) */
			messagelist.insert(messagelist.begin()+nb_modemsgs, propmsgs.begin()+nb_modemsgs, propmsgs.end());
		}

		return MOD_RES_PASSTHRU;
	}
};


/* Handles MODE +Z from clients */
class DummyZ final
	: public ModeHandler
{
 public:
	DummyZ(Module* parent) : ModeHandler(parent, "namebase", 'Z', PARAM_ALWAYS, MODETYPE_CHANNEL)
	{
		list = true;
	}

	// Handle /MODE #chan Z
	void DisplayList(User* user, Channel* chan) override
	{
		LocalUser* luser = IS_LOCAL(user);
		if (luser)
			::DisplayModeList(luser, chan);
	}
};

class ModuleIrcv3NamedModes final
	: public Module
	, public ISupport::EventListener
{
 private:
	CommandProp cmd;
	ModeHook modehook;
	DummyZ dummyZ;

 public:

	ModuleIrcv3NamedModes()
		: Module(VF_VENDOR, "Provides support for adding and removing modes via their long names.")
		, ISupport::EventListener(this)
		, cmd(this)
		, modehook(this)
		, dummyZ(this)
	{
	}

	void Prioritize() override
	{
		/* Convert MODE +Z before other modules start interpreting modes. */
		ServerInstance->Modules.SetPriority(this, I_OnPreMode, PRIORITY_FIRST);
	}

	void OnBuildISupport(ISupport::TokenMap& tokens) override
	{
		tokens["MAXMODES"] = "4"; /* TODO: this is an arbitrary number, check if we can safely increase it. */
	}

	void OnUserConnect(LocalUser* user) override
	{
		WriteModes(user, RPL_CHMODELIST, MODETYPE_CHANNEL);
		WriteModes(user, RPL_UMODELIST, MODETYPE_USER);
	}

	void WriteModes(LocalUser* user, unsigned int numeric, ModeType mt) {
		/* Inspired by CoreModMode::GenerateModeList */

		/* TODO: Here, we create one numeric for each mode type. This is correct, but wasteful; so we should merge them into a minimal number of numerics (while not exceeding the 512 byte limit) */
		auto modes = ServerInstance->Modes.GetModes(mt);
		for (auto i=modes.begin(); i!=modes.end(); /* incremented inside the loop */)
		{
			const auto& [_, mh] = *i;
			short unsigned int type;
			bool needs_param_when_setting = mh->NeedsParam(true);
			bool needs_param_when_unsetting = mh->NeedsParam(false);
			PrefixMode* pm = mh->IsPrefixMode();
			if (pm && pm->GetPrefix()) {
				type = 5; /* prefix mode */
			}
			else if (mh->IsListMode()) {
				type = 1; /* list mode */
			}
			else if (needs_param_when_setting && needs_param_when_unsetting)
			{
				type = 2; /* param mode */
			}
			else if (needs_param_when_setting)
			{
				type = 3; /* param mode */
			}
			else if (needs_param_when_unsetting) {
				/* wat? (param needed only when unsetting) */
				ServerInstance->Logs.Log(MODNAME, LOG_DEFAULT, "Error: MODE %s needs a parameter only when unsetting", mh->name.c_str());
				continue;
			}
			else {
				type = 4; /* flag */
			}

			/* TODO: if mt == MODETYPE_USER, make sure we don't output types 1, 2, or 5, as they are not allowed by the spec. */
			std::string mode_string = InspIRCd::Format("%d:%s=%c", type, mh->name.c_str(), mh->GetModeChar());
			i++;
			if (i != modes.end()) {
				/* "all but the last numeric MUST have a parameter containing only an asterisk (*) preceding the mode list." */
				user->WriteNumeric(numeric, "*", mode_string);
			}
			else {
				user->WriteNumeric(numeric, mode_string);
				break;
			}
		}
	}

	ModResult OnPreMode(User* source, User* dest, Channel* channel, Modes::ChangeList& modes) override
	{
		if (!channel)
			return MOD_RES_PASSTHRU;

		Modes::ChangeList::List& list = modes.getlist();
		for (Modes::ChangeList::List::iterator i = list.begin(); i != list.end(); )
		{
			Modes::Change& curr = *i;
			// Replace all namebase (dummyZ) modes being changed with the actual
			// mode handler and parameter. The parameter format of the namebase mode is
			// <modename>[=<parameter>].
			if (curr.mh == &dummyZ)
			{
				std::string name = curr.param;
				std::string value;
				std::string::size_type eq = name.find('=');
				if (eq != std::string::npos)
				{
					value.assign(name, eq + 1, std::string::npos);
					name.erase(eq);
				}

				ModeHandler* mh = ServerInstance->Modes.FindMode(name, MODETYPE_CHANNEL);
				if (!mh)
				{
					// Mode handler not found
					i = list.erase(i);
					continue;
				}

				curr.param.clear();
				if (mh->NeedsParam(curr.adding))
				{
					if (value.empty())
					{
						// Mode needs a parameter but there wasn't one
						i = list.erase(i);
						continue;
					}

					// Change parameter to the text after the '='
					curr.param = std::move(value);
				}

				// Put the actual ModeHandler in place of the namebase handler
				curr.mh = std::move(mh);
			}

			++i;
		}

		return MOD_RES_PASSTHRU;
	}
};

MODULE_INIT(ModuleIrcv3NamedModes)
