#include "main.h"
#include "UserSock.h"
#include "User.h"
#include "znc.h"
#include "IRCSock.h"
#include "DCCBounce.h"
#include "DCCSock.h"

void CUserSock::ReadLine(const string& sData) {
	string sLine = sData;

	while ((CUtils::Right(sLine, 1) == "\r") || (CUtils::Right(sLine, 1) == "\n")) {
		CUtils::RightChomp(sLine);
	}

	DEBUG_ONLY(cout << GetSockName() << " <- [" << sLine << "]" << endl);

#ifdef _MODULES
	if (m_bAuthed) {
		if ((m_pUser) && (m_pUser->GetModules().OnUserRaw(sLine))) {
			return;
		}
	}
#endif

	string sCommand = CUtils::Token(sLine, 0);

	if (strcasecmp(sCommand.c_str(), "ZNC") == 0) {
		PutStatus("Hello.  How may I help you?");
		return;
	} else if (strcasecmp(sCommand.c_str(), "PONG") == 0) {
		return;	// Block pong replies, we already responded to the pings
	} else if (strcasecmp(sCommand.c_str(), "PASS") == 0) {
		m_sPass = CUtils::Token(sLine, 1);

		if (m_sPass.find(":") != string::npos) {
			m_sUser = CUtils::Token(m_sPass, 0, false, ':');
			m_sPass = CUtils::Token(m_sPass, 1, true, ':');
		}

		m_bGotUser = (!m_sUser.empty());

		if ((m_bGotNick) && (m_bGotUser)) {
			AuthUser();
		}

		return;		// Don't forward this msg.  ZNC has already registered us.
	} else if (strcasecmp(sCommand.c_str(), "NICK") == 0) {
		string sNick = CUtils::Token(sLine, 1);
		if (CUtils::Left(sNick, 1) == ":") {
			CUtils::LeftChomp(sNick);
		}

		if (!m_bAuthed) {
			m_sNick = sNick;
			m_bGotNick = true;

			if (m_bGotUser) {
				AuthUser();
			}
			return;		// Don't forward this msg.  The bnc will handle nick changes until auth is complete 
		}

		if ((m_pUser) && (strcasecmp(sNick.c_str(), m_pUser->GetNick().c_str()) == 0)) {
			m_uKeepNickCounter++;
		}
	} else if (strcasecmp(sCommand.c_str(), "USER") == 0) {
		if ((!m_bGotUser) && (!m_bAuthed)) {
			m_sUser = CUtils::Token(sLine, 1);
			m_bGotUser = true;

			if (m_bGotNick) {
				AuthUser();
			}
		}

		return;		// Don't forward this msg.  ZNC has already registered us.
	} else if (strcasecmp(sCommand.c_str(), "JOIN") == 0) {
		string sChan = CUtils::Token(sLine, 1);
		if (CUtils::Left(sChan, 1) == ":") {
			CUtils::LeftChomp(sChan);
		}

		if (m_pUser) {
			CChan* pChan = m_pUser->FindChan(sChan);

			if ((pChan) && (!pChan->IsOn())) {
				pChan->IncClientRequests();
			}
		}
	} else if (strcasecmp(sCommand.c_str(), "QUIT") == 0) {
		if (m_pIRCSock) {
			m_pIRCSock->UserDisconnected();
		}

		Close();	// Treat a client quit as a detach
		return;		// Don't forward this msg.  We don't want the client getting us disconnected.
	} else if (strcasecmp(sCommand.c_str(), "NOTICE") == 0) {
		string sTarget = CUtils::Token(sLine, 1);
		string sMsg = CUtils::Token(sLine, 2, true);

		if (CUtils::Left(sMsg, 1) == ":") {
			CUtils::LeftChomp(sMsg);
		}

		if ((!m_pUser) || (strcasecmp(sTarget.c_str(), string(m_pUser->GetStatusPrefix() + "status").c_str())) == 0) {
			return;
		}

		if (strncasecmp(sTarget.c_str(), m_pUser->GetStatusPrefix().c_str(), m_pUser->GetStatusPrefix().length()) == 0) {
#ifdef _MODULES
			if (m_pUser) {
				string sModule = sTarget;
				CUtils::LeftChomp(sModule, m_pUser->GetStatusPrefix().length());

				CModule* pModule = m_pUser->GetModules().FindModule(sModule);
				if (pModule) {
					pModule->OnModNotice(sMsg);
				} else {
					PutStatus("No such module [" + sModule + "]");
				}
			}
#endif
			return;
		}

		if (CUtils::WildCmp("DCC * (*)", sMsg.c_str())) {
			sMsg = "DCC " + CUtils::Token(sLine, 3) + " (" + ((m_pIRCSock) ? m_pIRCSock->GetLocalIP() : GetLocalIP()) + ")";
		}

		CChan* pChan = m_pUser->FindChan(sTarget);

		if ((pChan) && (pChan->KeepBuffer())) {
			pChan->AddBuffer(":" + GetNickMask() + " NOTICE " + sTarget + " :" + sMsg);
		}

#ifdef _MODULES
		if (CUtils::WildCmp("\001*\001", sMsg.c_str())) {
			string sCTCP = sMsg;
			CUtils::LeftChomp(sCTCP);
			CUtils::RightChomp(sCTCP);

			if ((m_pUser) && (m_pUser->GetModules().OnUserCTCPReply(sTarget, sCTCP))) {
				return;
			}

			sMsg = "\001" + sCTCP + "\001";
		} else {
			if ((m_pUser) && (m_pUser->GetModules().OnUserNotice(sTarget, sMsg))) {
				return;
			}
		}
#endif

		PutIRC("NOTICE " + sTarget + " :" + sMsg);
		return;
	} else if (strcasecmp(sCommand.c_str(), "PRIVMSG") == 0) {
		string sTarget = CUtils::Token(sLine, 1);
		string sMsg = CUtils::Token(sLine, 2, true);

		if (CUtils::Left(sMsg, 1) == ":") {
			CUtils::LeftChomp(sMsg);
		}

		if (CUtils::WildCmp("\001*\001", sMsg.c_str())) {
			string sCTCP = sMsg;
			CUtils::LeftChomp(sCTCP);
			CUtils::RightChomp(sCTCP);

			if (strncasecmp(sCTCP.c_str(), "DCC ", 4) == 0) {
				string sType = CUtils::Token(sCTCP, 1);
				string sFile = CUtils::Token(sCTCP, 2);
				unsigned long uLongIP = strtoul(CUtils::Token(sCTCP, 3).c_str(), NULL, 10);
				unsigned short uPort = strtoul(CUtils::Token(sCTCP, 4).c_str(), NULL, 10);
				unsigned long uFileSize = strtoul(CUtils::Token(sCTCP, 5).c_str(), NULL, 10);
				string sIP = (m_pIRCSock) ? m_pIRCSock->GetLocalIP() : GetLocalIP();

				if (!m_pUser->UseClientIP()) {
					uLongIP = CUtils::GetLongIP(GetRemoteIP());
				}

				if (strcasecmp(sType.c_str(), "CHAT") == 0) {
					if (strncasecmp(sTarget.c_str(), m_pUser->GetStatusPrefix().c_str(), m_pUser->GetStatusPrefix().length()) == 0) {
					} else {
						unsigned short uBNCPort = CDCCBounce::DCCRequest(sTarget, uLongIP, uPort, "", true, m_pUser, (m_pIRCSock) ? m_pIRCSock->GetLocalIP() : GetLocalIP(), "");
						if (uBNCPort) {
							PutIRC("PRIVMSG " + sTarget + " :\001DCC CHAT chat " + CUtils::ToString(CUtils::GetLongIP(sIP)) + " " + CUtils::ToString(uBNCPort) + "\001");
						}
					}
				} else if (strcasecmp(sType.c_str(), "SEND") == 0) {
					// DCC SEND readme.txt 403120438 5550 1104

					if (strncasecmp(sTarget.c_str(), m_pUser->GetStatusPrefix().c_str(), m_pUser->GetStatusPrefix().length()) == 0) {
						if ((!m_pUser) || (strcasecmp(sTarget.c_str(), string(m_pUser->GetStatusPrefix() + "status").c_str()) == 0)) {
							if (!m_pUser) {
								return;
							}

							string sPath = m_pUser->GetDLPath();
							if (!CFile::Exists(sPath)) {
								PutStatus("Directory [" + sPath + "] not found, creating.");
								if (mkdir(sPath.c_str(), 0700)) {
									PutStatus("Could not create [" + sPath + "] directory.");
									return;
								}
							} else if (!CFile::IsDir(sPath)) {
								PutStatus("Error: [" + sPath + "] is not a directory.");
								return;
							}

							string sLocalFile = sPath + "/" + sFile;

							if (m_pUser) {
								m_pUser->GetFile(GetNick(), CUtils::GetIP(uLongIP), uPort, sLocalFile, uFileSize);
							}
						} else {
#ifdef _MODULES
							if ((m_pUser) && (m_pUser->GetModules().OnDCCUserSend(sTarget, uLongIP, uPort, sFile, uFileSize))) {
								return;
							}
#endif
						}
					} else {
						unsigned short uBNCPort = CDCCBounce::DCCRequest(sTarget, uLongIP, uPort, sFile, false, m_pUser, (m_pIRCSock) ? m_pIRCSock->GetLocalIP() : GetLocalIP(), "");
						if (uBNCPort) {
							PutIRC("PRIVMSG " + sTarget + " :\001DCC SEND " + sFile + " " + CUtils::ToString(CUtils::GetLongIP(sIP)) + " " + CUtils::ToString(uBNCPort) + " " + CUtils::ToString(uFileSize) + "\001");
						}
					}
				} else if (strcasecmp(sType.c_str(), "RESUME") == 0) {
					// PRIVMSG user :DCC RESUME "znc.o" 58810 151552
					unsigned short uResumePort = atoi(CUtils::Token(sCTCP, 3).c_str());
					unsigned long uResumeSize = strtoul(CUtils::Token(sCTCP, 4).c_str(), NULL, 10);

					// Need to lookup the connection by port, filter the port, and forward to the user
					if (strncasecmp(sTarget.c_str(), m_pUser->GetStatusPrefix().c_str(), m_pUser->GetStatusPrefix().length()) == 0) {
						if ((m_pUser) && (m_pUser->ResumeFile(sTarget, uResumePort, uResumeSize))) {
							PutServ(":" + sTarget + "!znc@znc.com PRIVMSG " + GetNick() + " :\001DCC ACCEPT " + sFile + " " + CUtils::ToString(uResumePort) + " " + CUtils::ToString(uResumeSize) + "\001");
						} else {
							PutStatus("DCC -> [" + GetNick() + "][" + sFile + "] Unable to find send to initiate resume.");
						}
					} else {
						CDCCBounce* pSock = (CDCCBounce*) m_pZNC->GetManager().FindSockByLocalPort(uResumePort);
						if ((pSock) && (strncasecmp(pSock->GetSockName().c_str(), "DCC::", 5) == 0)) {
							PutIRC("PRIVMSG " + sTarget + " :\001DCC " + sType + " " + sFile + " " + CUtils::ToString(pSock->GetUserPort()) + " " + CUtils::Token(sCTCP, 4) + "\001");
						}
					}
				} else if (strcasecmp(sType.c_str(), "ACCEPT") == 0) {
					if (strncasecmp(sTarget.c_str(), m_pUser->GetStatusPrefix().c_str(), m_pUser->GetStatusPrefix().length()) == 0) {
					} else {
						// Need to lookup the connection by port, filter the port, and forward to the user
						TSocketManager<Csock>& Manager = m_pZNC->GetManager();

						for (unsigned int a = 0; a < Manager.size(); a++) {
							CDCCBounce* pSock = (CDCCBounce*) Manager[a];

							if ((pSock) && (strncasecmp(pSock->GetSockName().c_str(), "DCC::", 5) == 0)) {
								if (pSock->GetUserPort() == atoi(CUtils::Token(sCTCP, 3).c_str())) {
									PutIRC("PRIVMSG " + sTarget + " :\001DCC " + sType + " " + sFile + " " + CUtils::ToString(pSock->GetLocalPort()) + " " + CUtils::Token(sCTCP, 4) + "\001");
								}
							}
						}
					}
				}

				return;
			}

			if (strncasecmp(sTarget.c_str(), m_pUser->GetStatusPrefix().c_str(), m_pUser->GetStatusPrefix().length()) == 0) {
#ifdef _MODULES
				string sModule = sTarget;
				CUtils::LeftChomp(sModule, m_pUser->GetStatusPrefix().length());

				CModule* pModule = m_pUser->GetModules().FindModule(sModule);
				if (pModule) {
					pModule->OnModCTCP(sCTCP);
				} else {
					PutStatus("No such module [" + sModule + "]");
				}
#endif
				return;
			}

#ifdef _MODULES
			if ((m_pUser) && (m_pUser->GetModules().OnUserCTCP(sTarget, sCTCP))) {
				return;
			}
#endif

			PutIRC("PRIVMSG " + sTarget + " :\001" + sCTCP + "\001");
			return;
		}

		if ((m_pUser) && (strcasecmp(sTarget.c_str(), string(m_pUser->GetStatusPrefix() + "status").c_str()) == 0)) {
#ifdef _MODULES
			if ((m_pUser) && (m_pUser->GetModules().OnStatusCommand(sMsg))) {
				return;
			}
#endif
			UserCommand(sMsg);
			return;
		}

		if (strncasecmp(sTarget.c_str(), m_pUser->GetStatusPrefix().c_str(), m_pUser->GetStatusPrefix().length()) == 0) {
#ifdef _MODULES
			if (m_pUser) {
				string sModule = sTarget;
				CUtils::LeftChomp(sModule, m_pUser->GetStatusPrefix().length());

				CModule* pModule = m_pUser->GetModules().FindModule(sModule);
				if (pModule) {
					pModule->OnModCommand(sMsg);
				} else {
					PutStatus("No such module [" + sModule + "]");
				}
			}
#endif
			return;
		}

		CChan* pChan = m_pUser->FindChan(sTarget);

		if ((pChan) && (pChan->KeepBuffer())) {
			pChan->AddBuffer(":" + GetNickMask() + " PRIVMSG " + sTarget + " :" + sMsg);
		}

#ifdef _MODULES
		if ((m_pUser) && (m_pUser->GetModules().OnUserMsg(sTarget, sMsg))) {
			return;
		}
#endif
		PutIRC("PRIVMSG " + sTarget + " :" + sMsg);
		return;
	}

	PutIRC(sLine);
}

void CUserSock::SetNick(const string& s) {
   m_sNick = s;

	if (m_pUser) {
		if (strcasecmp(m_sNick.c_str(), m_pUser->GetNick().c_str()) == 0) {
			m_uKeepNickCounter = 0;
		}
	}
}

bool CUserSock::DecKeepNickCounter() {
	if (!m_uKeepNickCounter) {
		return false;
	}

	m_uKeepNickCounter--;
	return true;
}

void CUserSock::UserCommand(const string& sLine) {
	if (!m_pUser) {
		return;
	}

	if (sLine.empty()) {
		return;
	}

	string sCommand = CUtils::Token(sLine, 0);

	if (strcasecmp(sCommand.c_str(), "LISTNICKS") == 0) {
		string sChan = CUtils::Token(sLine, 1);

		if (sChan.empty()) {
			PutStatus("Usage: ListNicks <#chan>");
			return;
		}

		CChan* pChan = m_pUser->FindChan(sChan);

		if (!pChan) {
			PutStatus("You are not on [" + sChan + "]");
			return;
		}

		if (!pChan->IsOn()) {
			PutStatus("You are not on [" + sChan + "] [trying]");
			return;
		}

		const map<string,CNick*>& msNicks = pChan->GetNicks();

		if (!msNicks.size()) {
			PutStatus("No nicks on [" + sChan + "]");
			return;
		}

		CTable Table;
		Table.AddColumn("@");
		Table.AddColumn("+");
		Table.AddColumn("Nick");
		Table.AddColumn("Ident");
		Table.AddColumn("Host");

		for (map<string,CNick*>::const_iterator a = msNicks.begin(); a != msNicks.end(); a++) {
			Table.AddRow();
			if (a->second->IsOp()) { Table.SetCell("@", "@"); }
			if (a->second->IsVoice()) { Table.SetCell("+", "+"); }
			Table.SetCell("Nick", a->second->GetNick());
			Table.SetCell("Ident", a->second->GetIdent());
			Table.SetCell("Host", a->second->GetHost());
		}

		unsigned int uTableIdx = 0;
		string sLine;

		while (Table.GetLine(uTableIdx++, sLine)) {
			PutStatus(sLine);
		}
	} else if (strcasecmp(sCommand.c_str(), "JUMP") == 0) {
		if (m_pUser) {
			if (m_pIRCSock) {
				m_pIRCSock->Close();
				PutStatus("Jumping to the next server in the list...");
				return;
			}
		}
	} else if (strcasecmp(sCommand.c_str(), "LISTCHANS") == 0) {
		if (m_pUser) {
			const vector<CChan*>& vChans = m_pUser->GetChans();

			CTable Table;
			Table.AddColumn("Name");
			Table.AddColumn("Status");
			Table.AddColumn("Buf");
			Table.AddColumn("Modes");
			Table.AddColumn("Users");
			Table.AddColumn("+o");
			Table.AddColumn("+v");

			for (unsigned int a = 0; a < vChans.size(); a++) {
				CChan* pChan = vChans[a];
				Table.AddRow();
				Table.SetCell("Name", pChan->GetName());
				Table.SetCell("Status", ((vChans[a]->IsOn()) ? "Joined" : "Trying"));
				Table.SetCell("Buf", CUtils::ToString(pChan->GetBufferCount()));

				string sModes = pChan->GetModeString();
				unsigned int uLimit = pChan->GetLimit();
				const string& sKey = pChan->GetKey();

				if (uLimit) { sModes += " " + CUtils::ToString(uLimit); }
				if (!sKey.empty()) { sModes += " " + sKey; }

				Table.SetCell("Modes", sModes);
				Table.SetCell("Users", CUtils::ToString(pChan->GetNickCount()));
				Table.SetCell("+o", CUtils::ToString(pChan->GetOpCount()));
				Table.SetCell("+v", CUtils::ToString(pChan->GetVoiceCount()));
			}

			if (Table.size()) {
				unsigned int uTableIdx = 0;
				string sLine;

				while (Table.GetLine(uTableIdx++, sLine)) {
					PutStatus(sLine);
				}
			}
		}
	} else if (strcasecmp(sCommand.c_str(), "SEND") == 0) {
		string sToNick = CUtils::Token(sLine, 1);
		string sFile = CUtils::Token(sLine, 2);

		if ((sToNick.empty()) || (sFile.empty())) {
			PutStatus("Usage: Send <nick> <file>");
			return;
		}

		if (m_pUser) {
			m_pUser->SendFile(sToNick, sFile);
		}
	} else if (strcasecmp(sCommand.c_str(), "GET") == 0) {
		string sFile = CUtils::Token(sLine, 1);

		if (sFile.empty()) {
			PutStatus("Usage: Get <file>");
			return;
		}

		if (m_pUser) {
			m_pUser->SendFile(GetNick(), sFile);
		}
	} else if (strcasecmp(sCommand.c_str(), "LISTDCCS") == 0) {
		TSocketManager<Csock>& Manager = m_pZNC->GetManager();

		CTable Table;
		Table.AddColumn("Type");
		Table.AddColumn("State");
		Table.AddColumn("Speed");
		Table.AddColumn("Nick");
		Table.AddColumn("IP");
		Table.AddColumn("File");

		for (unsigned int a = 0; a < Manager.size(); a++) {
			const string& sSockName = Manager[a]->GetSockName();

			if (strncasecmp(sSockName.c_str(), "DCC::", 5) == 0) {
				if (strncasecmp(sSockName.c_str() +5, "XFER::REMOTE::", 14) == 0) {
					continue;
				}

				if (strncasecmp(sSockName.c_str() +5, "CHAT::REMOTE::", 14) == 0) {
					continue;
				}

				if (strncasecmp(sSockName.c_str() +5, "SEND", 4) == 0) {
					CDCCSock* pSock = (CDCCSock*) Manager[a];

					Table.AddRow();
					Table.SetCell("Type", "Sending");
					Table.SetCell("State", CUtils::ToPercent(pSock->GetProgress()));
					Table.SetCell("Speed", CUtils::ToKBytes(pSock->GetAvgWrite() / 1000.0));
					Table.SetCell("Nick", pSock->GetRemoteNick());
					Table.SetCell("IP", pSock->GetRemoteIP());
					Table.SetCell("File", pSock->GetFileName());
				} else if (strncasecmp(sSockName.c_str() +5, "GET", 3) == 0) {
					CDCCSock* pSock = (CDCCSock*) Manager[a];

					Table.AddRow();
					Table.SetCell("Type", "Getting");
					Table.SetCell("State", CUtils::ToPercent(pSock->GetProgress()));
					Table.SetCell("Speed", CUtils::ToKBytes(pSock->GetAvgRead() / 1000.0));
					Table.SetCell("Nick", pSock->GetRemoteNick());
					Table.SetCell("IP", pSock->GetRemoteIP());
					Table.SetCell("File", pSock->GetFileName());
				} else if (strncasecmp(sSockName.c_str() +5, "LISTEN", 6) == 0) {
					CDCCSock* pSock = (CDCCSock*) Manager[a];

					Table.AddRow();
					Table.SetCell("Type", "Sending");
					Table.SetCell("State", "Waiting");
					Table.SetCell("Nick", pSock->GetRemoteNick());
					Table.SetCell("IP", pSock->GetRemoteIP());
					Table.SetCell("File", pSock->GetFileName());
				} else if (strncasecmp(sSockName.c_str() +5, "XFER::LOCAL", 11) == 0) {
					CDCCBounce* pSock = (CDCCBounce*) Manager[a];

					string sState = "Waiting";
					if ((pSock->IsConnected()) || (pSock->IsPeerConnected())) {
						sState = "Halfway";
						if ((pSock->IsPeerConnected()) && (pSock->IsPeerConnected())) {
							sState = "Connected";
						}
					}

					Table.AddRow();
					Table.SetCell("Type", "Xfer");
					Table.SetCell("State", sState);
					Table.SetCell("Nick", pSock->GetRemoteNick());
					Table.SetCell("IP", pSock->GetRemoteIP());
					Table.SetCell("File", pSock->GetFileName());
				} else if (strncasecmp(sSockName.c_str() +5, "CHAT::LOCAL", 11) == 0) {
					CDCCBounce* pSock = (CDCCBounce*) Manager[a];

					string sState = "Waiting";
					if ((pSock->IsConnected()) || (pSock->IsPeerConnected())) {
						sState = "Halfway";
						if ((pSock->IsPeerConnected()) && (pSock->IsPeerConnected())) {
							sState = "Connected";
						}
					}

					Table.AddRow();
					Table.SetCell("Type", "Chat");
					Table.SetCell("State", sState);
					Table.SetCell("Nick", pSock->GetRemoteNick());
					Table.SetCell("IP", pSock->GetRemoteIP());
				}
			}
		}

		if (Table.size()) {
			unsigned int uTableIdx = 0;
			string sLine;

			while (Table.GetLine(uTableIdx++, sLine)) {
				PutStatus(sLine);
			}
		} else {
			PutStatus("You have no active DCCs.");
		}
	} else if ((strcasecmp(sCommand.c_str(), "LISTMODS") == 0) || (strcasecmp(sCommand.c_str(), "LISTMODULES") == 0)) {
#ifdef _MODULES
		if (m_pUser) {
			CModules& Modules = m_pUser->GetModules();

			if (!Modules.size()) {
				PutStatus("You have no modules loaded.");
				return;
			}

			CTable Table;
			Table.AddColumn("Name");
			Table.AddColumn("Description");

			for (unsigned int b = 0; b < Modules.size(); b++) {
				Table.AddRow();
				Table.SetCell("Name", Modules[b]->GetModName());
				Table.SetCell("Description", CUtils::Ellipsize(Modules[b]->GetDescription(), 128));
			}

			unsigned int uTableIdx = 0; string sLine;
			while (Table.GetLine(uTableIdx++, sLine)) {
				PutStatus(sLine);
			}
		}
#else
		PutStatus("Modules are not enabled.");
#endif
		return;
	} else if ((strcasecmp(sCommand.c_str(), "LOADMOD") == 0) || (strcasecmp(sCommand.c_str(), "LOADMODULE") == 0)) {
		string sMod = CUtils::Token(sLine, 1);
		string sArgs = CUtils::Token(sLine, 2, true);

		if (m_pUser->DenyLoadMod()) {
			PutStatus("Unable to load [" + sMod + "] Access Denied.");
			return;
		}
#ifdef _MODULES
		if (sMod.empty()) {
			PutStatus("Usage: LoadMod <module> [args]");
			return;
		}

		string sModRet;
		m_pUser->GetModules().LoadModule(sMod, sArgs, m_pUser, m_pZNC->GetBinPath(), sModRet);
		PutStatus(sModRet);
#else
		PutStatus("Unable to load [" + sMod + "] Modules are not enabled.");
#endif
		return;
	} else if ((strcasecmp(sCommand.c_str(), "UNLOADMOD") == 0) || (strcasecmp(sCommand.c_str(), "UNLOADMODULE") == 0)) {
		string sMod = CUtils::Token(sLine, 1);

		if (m_pUser->DenyLoadMod()) {
			PutStatus("Unable to unload [" + sMod + "] Access Denied.");
			return;
		}
#ifdef _MODULES
		if (sMod.empty()) {
			PutStatus("Usage: UnloadMod <module>");
			return;
		}

		string sModRet;
		m_pUser->GetModules().UnloadModule(sMod, sModRet);
		PutStatus(sModRet);
#else
		PutStatus("Unable to unload [" + sMod + "] Modules are not enabled.");
#endif
		return;
	} else if ((strcasecmp(sCommand.c_str(), "RELOADMOD") == 0) || (strcasecmp(sCommand.c_str(), "RELOADMODULE") == 0)) {
		string sMod = CUtils::Token(sLine, 1);
		string sArgs = CUtils::Token(sLine, 2, true);

		if (m_pUser->DenyLoadMod()) {
			PutStatus("Unable to reload [" + sMod + "] Access Denied.");
			return;
		}
#ifdef _MODULES
		if (sMod.empty()) {
			PutStatus("Usage: ReloadMod <module> [args]");
			return;
		}

		string sModRet;
		m_pUser->GetModules().ReloadModule(sMod, sArgs, m_pUser, m_pZNC->GetBinPath(), sModRet);
		PutStatus(sModRet);
#else
		PutStatus("Unable to unload [" + sMod + "] Modules are not enabled.");
#endif
		return;
	} else if (strcasecmp(sCommand.c_str(), "SETBUFFER") == 0) {
		string sChan = CUtils::Token(sLine, 1);

		if (sChan.empty()) {
			PutStatus("Usage: SetBuffer <#chan> [linecount]");
			return;
		}

		CChan* pChan = m_pUser->FindChan(sChan);

		if (!pChan) {
			PutStatus("You are not on [" + sChan + "]");
			return;
		}

		if (!pChan->IsOn()) {
			PutStatus("You are not on [" + sChan + "] [trying]");
			return;
		}

		unsigned int uLineCount = strtoul(CUtils::Token(sLine, 2).c_str(), NULL, 10);

		if (uLineCount > 500) {
			PutStatus("Max linecount is 500.");
			return;
		}

		pChan->SetBufferCount(uLineCount);

		PutStatus("BufferCount for [" + sChan + "] set to [" + CUtils::ToString(pChan->GetBufferCount()) + "]");
	} else {
		PutStatus("I'm too stupid to help you with [" + sCommand + "]");
	}
}

bool CUserSock::ConnectionFrom(const string& sHost, int iPort) {
	DEBUG_ONLY(cout << GetSockName() << " == ConnectionFrom(" << sHost << ", " << iPort << ")" << endl);
	return m_pZNC->IsHostAllowed(sHost);
}

void CUserSock::AuthUser() {
	if (!m_pZNC) {
		DEBUG_ONLY(cout << "znc not set!" << endl);
		Close();
		return;
	}

	CUser* pUser = m_pZNC->GetUser(m_sUser);

	if ((!pUser) || (!pUser->CheckPass(m_sPass))) {
		if (pUser) {
			pUser->PutStatus("Another user attempted to login as you, with a bad password.");
		}

		PutStatus("Bad username and/or password.");
		PutServ(":irc.znc.com 464 " + GetNick() + " :Password Incorrect");
		Close();
	} else {
		m_sPass = "";
		m_pUser = pUser;

		if (!m_pUser->IsHostAllowed(GetRemoteIP())) {
			Close();
			return;
		}

		m_bAuthed = true;
		SetSockName("USR::" + pUser->GetUserName());

		CIRCSock* pIRCSock = (CIRCSock*) m_pZNC->FindSockByName("IRC::" + pUser->GetUserName());

		if (pIRCSock) {
			m_pIRCSock = pIRCSock;
			pIRCSock->UserConnected(this);
		}

#ifdef _MODULES
		m_pUser->GetModules().OnUserAttached();
#endif
	}
}

void CUserSock::Connected() {
	DEBUG_ONLY(cout << GetSockName() << " == Connected();" << endl);
	SetTimeout(900);	// Now that we are connected, let nature take its course
}

void CUserSock::ConnectionRefused() {
	DEBUG_ONLY(cout << GetSockName() << " == ConnectionRefused()" << endl);
}

void CUserSock::Disconnected() {
	if (m_pIRCSock) {
		m_pIRCSock->UserDisconnected();
		m_pIRCSock = NULL;
	}

#ifdef _MODULES
	if (m_pUser) {
		m_pUser->GetModules().OnUserDetached();
	}
#endif
}

void CUserSock::IRCConnected(CIRCSock* pIRCSock) {
	m_pIRCSock = pIRCSock;
}

void CUserSock::BouncedOff() {
	PutStatus("You are being disconnected because another user just authenticated as you.");
	m_pIRCSock = NULL;
	Close();
}

void CUserSock::IRCDisconnected() {
	m_pIRCSock = NULL;
}

Csock* CUserSock::GetSockObj(const string& sHost, int iPort) {
	CUserSock* pSock = new CUserSock(sHost, iPort);
	pSock->SetZNC(m_pZNC);

	return pSock;
}

void CUserSock::PutIRC(const string& sLine) {
	if (m_pIRCSock) {
		m_pIRCSock->PutServ(sLine);
	}
}

void CUserSock::PutServ(const string& sLine) {
	DEBUG_ONLY(cout << GetSockName() << " -> [" << sLine << "]" << endl);
	Write(sLine + "\r\n");   
}

void CUserSock::PutStatus(const string& sLine) {
	PutModule("status", sLine);
}

void CUserSock::PutModule(const string& sModule, const string& sLine) {
	if (!m_pUser) {
		return;
	}

	DEBUG_ONLY(cout << GetSockName() << " -> [:" + m_pUser->GetStatusPrefix() + ((sModule.empty()) ? "status" : sModule) + "!znc@znc.com PRIVMSG " << GetNick() << " :" << sLine << "]" << endl);
	Write(":" + m_pUser->GetStatusPrefix() + ((sModule.empty()) ? "status" : sModule) + "!znc@znc.com PRIVMSG " + GetNick() + " :" + sLine + "\r\n");
}

string CUserSock::GetNick() const {
	string sRet;

	if ((m_bAuthed) && (m_pIRCSock)) {
		sRet = m_pIRCSock->GetNick();
	}

	return (sRet.empty()) ? m_sNick : sRet;
}

string CUserSock::GetNickMask() const {
	if (m_pIRCSock) {
		return m_pIRCSock->GetNickMask();
	}

	return GetNick() + "!" + m_pUser->GetIdent() + "@" + m_pUser->GetVHost();
}

