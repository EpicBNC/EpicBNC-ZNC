#include "main.h"
#include "User.h"
#include "Nick.h"
#include "Modules.h"
#include "Chan.h"
#include "Utils.h"
#include "Csocket.h"
#include "md5.h"
#include <pwd.h>
#include <sstream>

/*
 * Secure chat system
 * Author: imaginos <imaginos@imaginos.net>
 * 
 * $Log$
 * Revision 1.1  2004/08/24 00:08:52  prozacx
 * Initial revision
 *
 *
 */     
class CSChat;

class CRemMarkerJob : public CTimer 
{
public:
	CRemMarkerJob( CModule* pModule, unsigned int uInterval, unsigned int uCycles, const string& sLabel, const string& sDescription ) 
		: CTimer( pModule, uInterval, uCycles, sLabel, sDescription) {}

	virtual ~CRemMarkerJob() {}
	void SetNick( const string & sNick )
	{
		m_sNick = sNick;
	}

protected:
	virtual void RunJob();
	string		m_sNick;
};

class CSChatSock : public Csock
{
public:
	CSChatSock( CSChat *pMod ) : Csock()
	{
		m_pModule = pMod;
	}
	CSChatSock( int itimeout = 60 ) : Csock( itimeout ) 
	{
		m_pModule = NULL;	
		EnableReadLine();
	}
	CSChatSock( const CS_STRING & sHost, int iPort, int iTimeout = 60 )
		: Csock( sHost, iPort, iTimeout ) 
	{
		m_pModule = NULL;
		EnableReadLine();
	}

	virtual Csock *GetSockObj( const CS_STRING & sHostname, int iPort )
	{
		CSChatSock *p = new CSChatSock( sHostname, iPort );
		return( p );
	}

	virtual bool ConnectionFrom( const CS_STRING & sHost, int iPort ) 
	{
		Close();	// close the listener after the first connection	
		return( true ); 
	}

	virtual void Connected();
	virtual bool CreatedChild( Csock *pSock )
	{
		CSChatSock *p = (CSChatSock *)pSock;
		p->SetModule( m_pModule );
		p->SetChatNick( m_sChatNick );
		p->SetSockName( GetSockName() + "::" + m_sChatNick );
		return( true );
	}
	
	virtual void Timeout();

	void SetModule( CSChat *p )
	{
		m_pModule = p;
	}
	void SetChatNick( const string & sNick )
	{
		m_sChatNick = sNick;
	}

	const string & GetChatNick() const { return( m_sChatNick ); }

	virtual void ReadLine( const CS_STRING & sLine );
	virtual void Disconnected();

	virtual void AddLine( const string & sLine )
	{
		m_vBuffer.insert( m_vBuffer.begin(), sLine );
		if ( m_vBuffer.size() > 200 )
			m_vBuffer.pop_back();
	}

	virtual void DumpBuffer()
	{
		for( vector<CS_STRING>::reverse_iterator it = m_vBuffer.rbegin(); it != m_vBuffer.rend(); it++ )
			ReadLine( *it );

		m_vBuffer.clear();
	}

private:
	CSChat 					*m_pModule;	
	string					m_sChatNick;
	vector<CS_STRING>		m_vBuffer;	
};



class CSChat : public CModule 
{
public:
	MODCONSTRUCTOR(CSChat) {}
	virtual ~CSChat() { CleanSocks(); }

	virtual bool OnLoad( const string & sArgs )
	{
		m_sPemFile = sArgs;

		if ( m_sPemFile.empty() )
		{
			m_sPemFile = m_pUser->GetBinPath() + "/znc.pem";
		}

		if (!CFile::Exists(m_sPemFile)) {
			PutModule("Unable to load pem file [" + m_sPemFile + "]");
			return false;
		}

		return true;
	}

	virtual void OnUserAttached() 
	{
		string sName = "SCHAT::" + m_pUser->GetUserName();
		for( u_int a = 0; a < m_pManager->size(); a++ )
		{
			if ( ( strncmp( (*m_pManager)[a]->GetSockName().c_str(), sName.c_str(), sName.length() ) != 0 ) || ( (*m_pManager)[a]->GetType() == CSChatSock::LISTENER ) )
				continue;
			
			CSChatSock *p = (CSChatSock *)(*m_pManager)[a];
			p->DumpBuffer();
		}
	}
	virtual void OnUserDetached() {}

	void CleanSocks()
	{
		string sName = "SCHAT::" + m_pUser->GetUserName();
		for( u_int a= 0; a < m_pManager->size(); a++ )
		{
			if ( strncmp( (*m_pManager)[a]->GetSockName().c_str(), sName.c_str(), sName.length() ) == 0 )
				m_pManager->DelSock( a-- );
		}
	}
	
	virtual string GetDescription() 
	{
		return ( "Secure cross platform (:P) chat system" );
	}

	virtual bool OnUserRaw( string & sLine )
	{
		if ( strncasecmp( sLine.c_str(), "schat ", 6 ) == 0 )
		{
			OnModCommand( "chat " + sLine.substr( 6, string::npos ) );
			return( true );

		} else if ( strcasecmp( sLine.c_str(), "schat" ) == 0 )
		{
			PutModule( "SChat User Area ..." );
			OnModCommand( "help" );
			return( true );
		
		}
		
		return( false );
	}	
	virtual void OnModCommand( const string& sCommand ) 
	{
		u_int iPos = sCommand.find( " " );
		string sCom, sArgs;
		if ( iPos == string::npos )
			sCom = sCommand;
		else
		{
			sCom = sCommand.substr( 0, iPos );
			sArgs = sCommand.substr( iPos + 1, string::npos );
		}

		if ( ( strcasecmp( sCom.c_str(), "chat" ) == 0 ) && ( !sArgs.empty() ) )
		{
			string sSockName = "SCHAT::" + m_pUser->GetUserName();
			string sNick = "(s)" + sArgs;
			for( u_int a= 0; a < m_pManager->size(); a++ )
			{
				if ( strncmp( (*m_pManager)[a]->GetSockName().c_str(), sSockName.c_str(), sSockName.length() ) != 0 )
					continue;

				CSChatSock *pSock = (CSChatSock *)(*m_pManager)[a];
				if ( strcasecmp( pSock->GetChatNick().c_str(), sNick.c_str() ) == 0 )
				{
					PutModule( "Already Connected to [" + sArgs + "]" );
					return;
				}
			}

			CSChatSock *pSock = new CSChatSock;
			pSock->SetCipher( "HIGH" );
			pSock->SetPemLocation( m_sPemFile );
			pSock->SetModule( this );
			pSock->SetChatNick( sNick );

			u_short iPort = m_pManager->ListenRand( sSockName, m_pUser->GetLocalIP(), true, SOMAXCONN, pSock, 60 );

			if ( iPort == 0 )
			{
				PutModule( "Failed to start chat!" );
				return;
			}

			stringstream s;	
			s << "PRIVMSG " << sArgs << " :\001";
			s << "DCC SCHAT chat ";
			s << CUtils::GetLongIP( m_pUser->GetLocalIP() );
			s << " " << iPort << "\001";
		
			PutIRC( s.str() );

		} else if ( strcasecmp( sCom.c_str(), "list" ) == 0 )
		{
			string sName = "SCHAT::" + m_pUser->GetUserName();
			CTable Table;
			Table.AddColumn( "Nick" );
			Table.AddColumn( "Created" );
			Table.AddColumn( "Host" );
			Table.AddColumn( "Port" );
			Table.AddColumn( "Status" );
			Table.AddColumn( "Cipher" );
			for( u_int a= 0; a < m_pManager->size(); a++ )
			{
				if ( strncmp( (*m_pManager)[a]->GetSockName().c_str(), sName.c_str(), sName.length() ) != 0 )
					continue;

				Table.AddRow();

				CSChatSock *pSock = (CSChatSock *)(*m_pManager)[a];
				Table.SetCell( "Nick", pSock->GetChatNick() );
				unsigned long long iStartTime = pSock->GetStartTime();
				time_t iTime = iStartTime / 1000;
				char *pTime = ctime( &iTime );
				if ( pTime )
				{
					string sTime = pTime;
					CUtils::Trim( sTime );
					Table.SetCell( "Created", sTime );
				}
				
				if ( pSock->GetType() != CSChatSock::LISTENER )
				{
					Table.SetCell( "Status", "Established" );	
					Table.SetCell( "Host", pSock->GetRemoteIP() );
					Table.SetCell( "Port", CUtils::ToString( pSock->GetRemotePort() ) );
					SSL_SESSION *pSession = pSock->GetSSLSession();
					if ( ( pSession ) && ( pSession->cipher ) && ( pSession->cipher->name ) )
						Table.SetCell( "Cipher", pSession->cipher->name );

				} else
				{
					Table.SetCell( "Status", "Waiting" );
					Table.SetCell( "Port", CUtils::ToString( pSock->GetLocalPort() ) );
				}
			}
			if ( Table.size() ) 
			{
				unsigned int uTableIdx = 0;
				string sLine;
				while ( Table.GetLine( uTableIdx++, sLine ) )
					PutModule( sLine );
			} else
				PutModule( "No SDCCs currently in session" );

		} else if ( strcasecmp( sCom.c_str(), "close" ) == 0 )
		{
			string sName = "SCHAT::" + m_pUser->GetUserName();
			for( u_int a= 0; a < m_pManager->size(); a++ )
			{
				if ( strncmp( (*m_pManager)[a]->GetSockName().c_str(), sName.c_str(), sName.length() ) != 0 )
					continue;

				CSChatSock *pSock = (CSChatSock *)(*m_pManager)[a];
				if ( strncasecmp( sArgs.c_str(), "(s)", 3 ) != 0 )
					sArgs = "(s)" + sArgs;

				if ( strcasecmp( sArgs.c_str(), pSock->GetChatNick().c_str() ) == 0 )
				{
					pSock->Close();
					return;
				}
				PutModule( "No Such Chat [" + sArgs + "]" );
			}

		} else if ( strcasecmp( sCom.c_str(), "help" ) == 0 )
		{
			PutModule( "Commands are: " );
			PutModule( "    help           - This text." );
			PutModule( "    chat <nick>    - Chat a nick." );
			PutModule( "    list           - List current chats." );
			PutModule( "    close <nick>   - Close a chat to a nick." );
			PutModule( "    timers         - Shows related timers." );
		} else if ( strcasecmp( sCom.c_str(), "timers" ) == 0 )
			ListTimers();
		else
			PutModule( "Unknown command [" + sCom + "] [" + sArgs + "]" );
	}

	virtual bool OnPrivCTCP( const CNick& Nick, string& sMessage )
	{
		if ( strncasecmp( sMessage.c_str(), "DCC SCHAT ", 10 ) == 0 )
		{
			// chat ip port
			unsigned long iIP = strtoul( CUtils::Token( sMessage, 3 ).c_str(), NULL, 10 );
			unsigned short iPort = strtoul( CUtils::Token( sMessage, 4 ).c_str(), NULL, 10 );

			if ( ( iIP > 0 ) && ( iPort > 0 ) )
			{
				pair<u_long, u_short> pTmp;
				pTmp.first = iIP;
				pTmp.second = iPort;
				m_siiWaitingChats["(s)" + Nick.GetNick()] = pTmp;
				SendToUser( "(s)" + Nick.GetNick() + "!" + "(s)" + Nick.GetNick() + "@" + CUtils::GetIP( iIP ), "*** Incoming DCC SCHAT, Accept ? (yes/no)" );
				CRemMarkerJob *p = new CRemMarkerJob( this, 60, 1, "Remove (s)" + Nick.GetNick(), "Removes this nicks entry for waiting DCC." );
				p->SetNick( "(s)" + Nick.GetNick() );
				AddTimer( p );
				return( true );
			}
		}
		
		return( false );
	}

	void AcceptSDCC( const string & sNick, u_long iIP, u_short iPort )
	{
		CSChatSock *p = new CSChatSock( CUtils::GetIP( iIP ), iPort, 60 );
		p->SetModule( this );
		p->SetChatNick( sNick );
		string sSockName = "SCHAT::" + m_pUser->GetUserName() +  "::" + sNick;
		m_pManager->Connect( CUtils::GetIP( iIP ), iPort, sSockName, 60, true, m_pUser->GetLocalIP(), p );
		RemTimer( "Remove " + sNick ); // delete any associated timer to this nick
	}
	virtual bool OnUserMsg( const string& sTarget, string& sMessage )
	{
		if ( strncmp( sTarget.c_str(), "(s)", 3 ) == 0 )
		{
			string sSockName = "SCHAT::" + m_pUser->GetUserName() + "::" + sTarget;
			CSChatSock *p = (CSChatSock *)m_pManager->FindSockByName( sSockName );
			if ( !p )
			{
				map< string,pair< u_long,u_short > >::iterator it = m_siiWaitingChats.find( sTarget );
				if ( it != m_siiWaitingChats.end() )
				{
					if ( strcasecmp( sMessage.c_str(), "yes" ) != 0 )
						SendToUser( sTarget + "!" + sTarget + "@" + CUtils::GetIP( it->second.first ), "Refusing to accept DCC SCHAT!" );
					else
						AcceptSDCC( sTarget, it->second.first, it->second.second );

					m_siiWaitingChats.erase( it );
					return( true );
				}
				PutModule( "No such SCHAT to [" + sTarget + "]" );
			} else
				p->Write( sMessage + "\n" );

			return( true );
		}
		return( false );
	}

	virtual void RemoveMarker( const string & sNick )
	{
		map< string,pair< u_long,u_short > >::iterator it = m_siiWaitingChats.find( sNick );
		if ( it != m_siiWaitingChats.end() )
			m_siiWaitingChats.erase( it );
	}

	void SendToUser( const string & sFrom, const string & sText )
	{
		//:*schat!znc@znc.com PRIVMSG Jim :
		string sSend = ":" + sFrom + " PRIVMSG " + m_pUser->GetCurNick() + " :" + sText;
		PutUser( sSend );
	}

	bool IsAttached()
	{
		return( m_pUser->IsUserAttached() );
	}
	
private:
	map< string,pair< u_long,u_short > >		m_siiWaitingChats;
	string		m_sPemFile;
};


//////////////////// methods ////////////////

void CSChatSock::ReadLine( const CS_STRING & sLine )
{
	if ( m_pModule )
	{
		string sText = sLine;
		CUtils::Trim( sText );
		if ( m_pModule->IsAttached() )
			m_pModule->SendToUser( m_sChatNick + "!" + m_sChatNick + "@" + GetRemoteIP(), sText );
		else
			AddLine( sText );
	}
}

void CSChatSock::Disconnected()
{
	if ( m_pModule )
		m_pModule->SendToUser( m_sChatNick + "!" + m_sChatNick + "@" + GetRemoteIP(), "*** Disconnected." );
}

void CSChatSock::Connected()
{
	SetTimeout( 0 );
	if ( m_pModule )
		m_pModule->SendToUser( m_sChatNick + "!" + m_sChatNick + "@" + GetRemoteIP(), "*** Connected." );
}

void CSChatSock::Timeout()
{
	if ( m_pModule )
	{
		if ( GetType() == LISTENER )
			m_pModule->PutModule( "Timeout while waiting for [" + m_sChatNick + "]" );
		else
			m_pModule->SendToUser( m_sChatNick + "!" + m_sChatNick + "@" + GetRemoteIP(), "*** Connection Timed out." );
	}
}

void CRemMarkerJob::RunJob()
{
	CSChat *p = (CSChat *)m_pModule;
	p->RemoveMarker( m_sNick );

	// store buffer
}
MODULEDEFS(CSChat)

