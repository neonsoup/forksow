/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "client/client.h"
#include "qcommon/threads.h"
#include "qcommon/version.h"

//#define UNSAFE_EXIT
#define MAX_MASTER_SERVERS                  4

#define SERVERBROWSER_PROTOCOL_VERSION      APP_PROTOCOL_VERSION

//=========================================================

typedef struct serverlist_s {
	char address[48];
	int64_t pingTimeStamp;
	int64_t lastUpdatedByMasterServer;
	int64_t masterServerUpdateSeq;
	bool isLocal;
	struct serverlist_s *pnext;
} serverlist_t;

serverlist_t *masterList;

static bool filter_allow_full = false;
static bool filter_allow_empty = false;

static int64_t masterServerUpdateSeq;

static int64_t localQueryTimeStamp = 0;

typedef struct masterserver_s {
	char addressString[MAX_TOKEN_CHARS];
	netadr_t address;
	Thread *resolverThread;
	volatile bool resolverActive;
	char delayedRequestModName[MAX_TOKEN_CHARS];
} masterserver_t;

static masterserver_t masterServers[MAX_MASTER_SERVERS];
int numMasterServers;

//=========================================================

/*
* CL_FreeServerlist
*/
static void CL_FreeServerlist( serverlist_t **serversList ) {
	serverlist_t *ptr;

	while( *serversList ) {
		ptr = *serversList;
		*serversList = ptr->pnext;
		Mem_ZoneFree( ptr );
	}
}

/*
* CL_ServerIsInList
*/
static serverlist_t *CL_ServerFindInList( serverlist_t *serversList, const char *adr ) {

	serverlist_t *server;

	server = serversList;
	while( server ) {
		if( !Q_stricmp( server->address, adr ) ) {
			return server;
		}
		server = server->pnext;
	}

	return NULL;
}

/*
* CL_AddServerToList
*/
static bool CL_AddServerToList( serverlist_t **serversList, const char *adr ) {
	serverlist_t *newserv;
	netadr_t nadr;

	if( !adr || !strlen( adr ) ) {
		return false;
	}

	if( !NET_StringToAddress( adr, &nadr ) ) {
		return false;
	}

	newserv = CL_ServerFindInList( *serversList, adr );
	if( newserv ) {
		// ignore excessive updates for about a second or so, which may happen
		// when we're querying multiple master servers at once
		if( !newserv->masterServerUpdateSeq ||
			newserv->lastUpdatedByMasterServer + 1000 < Sys_Milliseconds() ) {
			newserv->lastUpdatedByMasterServer = Sys_Milliseconds();
			newserv->masterServerUpdateSeq = masterServerUpdateSeq;
		}
		return false;
	}

	newserv = (serverlist_t *)Mem_ZoneMalloc( sizeof( serverlist_t ) );
	Q_strncpyz( newserv->address, adr, sizeof( newserv->address ) );
	newserv->pingTimeStamp = 0;
	newserv->lastUpdatedByMasterServer = Sys_Milliseconds();
	newserv->masterServerUpdateSeq = masterServerUpdateSeq;
	newserv->pnext = *serversList;
	newserv->isLocal = NET_IsLocalAddress( &nadr );
	*serversList = newserv;

	return true;
}

/*
* CL_ParseGetInfoResponse
*
* Handle a server responding to a detailed info broadcast
*/
void CL_ParseGetInfoResponse( const socket_t *socket, const netadr_t *address, msg_t *msg ) {
	char *s = MSG_ReadString( msg );
	Com_DPrintf( "%s\n", s );
}

/*
* CL_ParseGetStatusResponse
*
* Handle a server responding to a detailed info broadcast
*/
void CL_ParseGetStatusResponse( const socket_t *socket, const netadr_t *address, msg_t *msg ) {
	char *s = MSG_ReadString( msg );
	Com_DPrintf( "%s\n", s );
}


/*
* CL_QueryGetInfoMessage
*/
static void CL_QueryGetInfoMessage( const char *cmdname ) {
	netadr_t adr;
	const char *server;

	//get what master
	server = Cmd_Argv( 1 );
	if( !server || !( *server ) ) {
		Com_Printf( "%s: no address provided %s...\n", Cmd_Argv( 0 ), server ? server : "" );
		return;
	}

	// send a broadcast packet
	Com_DPrintf( "querying %s...\n", server );

	if( NET_StringToAddress( server, &adr ) ) {
		socket_t *socket;

		if( NET_GetAddressPort( &adr ) == 0 ) {
			NET_SetAddressPort( &adr, PORT_SERVER );
		}

		socket = ( adr.type == NA_IP6 ? &cls.socket_udp6 : &cls.socket_udp );
		Netchan_OutOfBandPrint( socket, &adr, "%s", cmdname );
	} else {
		Com_Printf( "Bad address: %s\n", server );
	}
}


/*
* CL_QueryGetInfoMessage_f - getinfo 83.97.146.17:27911
*/
void CL_QueryGetInfoMessage_f( void ) {
	CL_QueryGetInfoMessage( "getinfo" );
}


/*
* CL_QueryGetStatusMessage_f - getstatus 83.97.146.17:27911
*/
void CL_QueryGetStatusMessage_f( void ) {
	CL_QueryGetInfoMessage( "getstatus" );
}

/*
* CL_PingServer_f
*/
void CL_PingServer_f( void ) {
	const char *address_string;
	char requestString[64];
	netadr_t adr;
	serverlist_t *pingserver;
	socket_t *socket;

	if( Cmd_Argc() < 2 ) {
		Com_Printf( "Usage: pingserver [ip:port]\n" );
	}

	address_string = Cmd_Argv( 1 );

	if( !NET_StringToAddress( address_string, &adr ) ) {
		return;
	}

	pingserver = CL_ServerFindInList( masterList, address_string );
	if( !pingserver ) {
		return;
	}

	// never request a second ping while awaiting for a ping reply
	if( pingserver->pingTimeStamp + SERVER_PINGING_TIMEOUT > Sys_Milliseconds() ) {
		return;
	}

	pingserver->pingTimeStamp = Sys_Milliseconds();

	snprintf( requestString, sizeof( requestString ), "info %i %s %s", SERVERBROWSER_PROTOCOL_VERSION,
				 filter_allow_full ? "full" : "",
				 filter_allow_empty ? "empty" : "" );

	socket = ( adr.type == NA_IP6 ? &cls.socket_udp6 : &cls.socket_udp );
	Netchan_OutOfBandPrint( socket, &adr, "%s", requestString );
}

/*
* CL_ParseStatusMessage
* Handle a reply from a ping
*/
void CL_ParseStatusMessage( const socket_t *socket, const netadr_t *address, msg_t *msg ) {
	char *s = MSG_ReadString( msg );
	serverlist_t *pingserver;
	char adrString[64];

	Com_DPrintf( "%s\n", s );

	Q_strncpyz( adrString, NET_AddressToString( address ), sizeof( adrString ) );

	// ping response
	pingserver = CL_ServerFindInList( masterList, adrString );

	if( pingserver && pingserver->pingTimeStamp ) { // valid ping
		int ping = (int)(Sys_Milliseconds() - pingserver->pingTimeStamp);
		UI_AddToServerList( adrString, va( "\\\\ping\\\\%i%s", ping, s ) );
		pingserver->pingTimeStamp = 0;
		return;
	}

	// assume LAN response
	if( NET_IsLANAddress( address ) && ( localQueryTimeStamp + LAN_SERVER_PINGING_TIMEOUT > Sys_Milliseconds() ) ) {
		int ping = (int)(Sys_Milliseconds() - localQueryTimeStamp);
		UI_AddToServerList( adrString, va( "\\\\ping\\\\%i%s", ping, s ) );
		return;
	}

	// add the server info, but ignore the ping, cause it's not valid
	UI_AddToServerList( adrString, s );
}

/*
* CL_ParseGetServersResponseMessage
* Handle a reply from getservers message to master server
*/
static void CL_ParseGetServersResponseMessage( msg_t *msg, bool extended ) {
	const char *header;
	char adrString[64];
	uint8_t addr[16];
	unsigned short port;
	netadr_t adr;

	MSG_BeginReading( msg );
	MSG_ReadInt32( msg ); // skip the -1

	//jump over the command name
	header = ( extended ? "getserversExtResponse" : "getserversResponse" );
	if( !MSG_SkipData( msg, strlen( header ) ) ) {
		Com_Printf( "Invalid master packet ( missing %s )\n", header );
		return;
	}

	while( msg->readcount + 7 <= msg->cursize ) {
		char prefix = MSG_ReadInt8( msg );

		switch( prefix ) {
			case '\\':
				MSG_ReadData( msg, addr, 4 );
				port = ShortSwap( MSG_ReadInt16( msg ) ); // both endians need this swapped.
				snprintf( adrString, sizeof( adrString ), "%u.%u.%u.%u:%u", addr[0], addr[1], addr[2], addr[3], port );
				break;

			case '/':
				if( extended ) {
					MSG_ReadData( msg, addr, 16 );
					port = ShortSwap( MSG_ReadInt16( msg ) ); // both endians need this swapped.
					snprintf( adrString, sizeof( adrString ), "[%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x]:%hu",
								 addr[ 0], addr[ 1], addr[ 2], addr[ 3], addr[ 4], addr[ 5], addr[ 6], addr[ 7],
								 addr[ 8], addr[ 9], addr[10], addr[11], addr[12], addr[13], addr[14], addr[15],
								 port );
				} else {
					Com_Printf( "Invalid master packet ( IPv6 prefix in a non-extended response )\n" );
					return;
				}

				break;

			default:
				Com_Printf( "Invalid master packet ( missing separator )\n" );
				return;
		}

		if( port == 0 ) { // last server seen
			return;
		}

		Com_DPrintf( "%s\n", adrString );
		if( !NET_StringToAddress( adrString, &adr ) ) {
			Com_Printf( "Bad address: %s\n", adrString );
			continue;
		}

		CL_AddServerToList( &masterList, adrString );
	}
}

/*
* CL_ParseGetServersResponse
* Handle a reply from getservers message to master server
*/
void CL_ParseGetServersResponse( const socket_t *socket, const netadr_t *address, msg_t *msg, bool extended ) {
	serverlist_t *server;
	netadr_t adr;

	// add the new server addresses to the local addresses list
	masterServerUpdateSeq++;

	CL_ParseGetServersResponseMessage( msg, extended );

	// dump servers we just received an update on from the master server
	server = masterList;
	while( server ) {
		if( server->masterServerUpdateSeq == masterServerUpdateSeq
			&& !( server->isLocal && Com_ServerState() )
			&& NET_StringToAddress( server->address, &adr ) ) {
			UI_AddToServerList( server->address, "\\\\EOT" );
		}

		server = server->pnext;
	}
}

/*
* CL_MasterResolverThreadFunc
*/
static void CL_MasterResolverThreadFunc( void *param ) {
	masterserver_t *master = ( masterserver_t * ) param;

	NET_StringToAddress( master->addressString, &master->address );
	if( master->address.type == NA_IP || master->address.type == NA_IP6 ) {
		if( NET_GetAddressPort( &master->address ) == 0 ) {
			NET_SetAddressPort( &master->address, PORT_MASTER );
		}
	} else {
		Com_Printf( "Failed to resolve master server address: %s\n", master->addressString );
	}

	master->resolverActive = false;
}

/*
* CL_MasterAddressCache_Init
*/
static void CL_MasterAddressCache_Init( void ) {
	int numMasters;
	const char *ptr;
	const char *masterAddress;
	const char *masterServersStr;
	masterserver_t *master;
	int i;

	masterServersStr = Cvar_String( "masterservers" );
	if( !*masterServersStr ) {
		return;
	}

	// count the number of master servers
	numMasters = 0;
	for( ptr = masterServersStr; ptr; ) {
		masterAddress = COM_Parse( &ptr );
		if( !*masterAddress ) {
			break;
		}
		numMasters++;
	}

	// don't allow too many as each will spawn its own resolver thread
	if( numMasters > MAX_MASTER_SERVERS ) {
		numMasters = MAX_MASTER_SERVERS;
	}

	numMasterServers = 0;
	for( i = 0, ptr = masterServersStr, master = masterServers; i < numMasters && ptr; i++, master++ ) {
		masterAddress = COM_Parse( &ptr );
		if( !*masterAddress ) {
			break;
		}

		numMasterServers++;
		Q_strncpyz( master->addressString, masterAddress, sizeof( master->addressString ) );
		master->address.type = NA_NOTRANSMIT;
		master->resolverActive = true;
		master->resolverThread = NewThread( CL_MasterResolverThreadFunc, master );
	}
}

/*
* CL_MasterAddressCache_Shutdown
*/
static void CL_MasterAddressCache_Shutdown( void ) {
	// here we leak the mutex and resources allocated for resolving threads,
	// but at least we're not calling cancel on them, which is possibly dangerous

	// we're going to kill the main thread anyway, so keep the lock and let the threads die

	numMasterServers = 0;
}

/*
* CL_SendMasterServerQuery
*/
static void CL_SendMasterServerQuery( netadr_t *adr, const char *modname ) {
	const char *cmdname;
	socket_t *socket;
	const char *requeststring;

	if( adr->type == NA_IP ) {
		cmdname = "getservers";
		socket = &cls.socket_udp;
	} else {
		cmdname = "getserversExt";
		socket = &cls.socket_udp6;
	}

	// create the message
	requeststring = va( "%s %c%s %i %s %s", cmdname, toupper( modname[0] ), modname + 1, SERVERBROWSER_PROTOCOL_VERSION,
						filter_allow_full ? "full" : "",
						filter_allow_empty ? "empty" : "" );

	Com_DPrintf( "Querying %s: %s\n", NET_AddressToString( adr ), requeststring );

	Netchan_OutOfBandPrint( socket, adr, "%s", requeststring );
}

/*
* CL_GetServers_f
*/
void CL_GetServers_f( void ) {
	const char *requeststring;
	int i;
	const char *modname, *masterAddress;
	masterserver_t *master = NULL;

	filter_allow_full = false;
	filter_allow_empty = false;
	for( i = 2; i < Cmd_Argc(); i++ ) {
		if( !Q_stricmp( "full", Cmd_Argv( i ) ) ) {
			filter_allow_full = true;
		}

		if( !Q_stricmp( "empty", Cmd_Argv( i ) ) ) {
			filter_allow_empty = true;
		}
	}

	if( !Q_stricmp( Cmd_Argv( 1 ), "local" ) ) {
		if( localQueryTimeStamp + LAN_SERVER_PINGING_TIMEOUT > Sys_Milliseconds() ) {
			return;
		}

		localQueryTimeStamp = Sys_Milliseconds();

		// send a broadcast packet
		Com_DPrintf( "Pinging broadcast...\n" );

		// erm... modname isn't sent in local queries?

		requeststring = va( "info %i %s %s", SERVERBROWSER_PROTOCOL_VERSION,
							filter_allow_full ? "full" : "",
							filter_allow_empty ? "empty" : "" );

		for( i = 0; i < NUM_BROADCAST_PORTS; i++ ) {
			netadr_t broadcastAddress;
			NET_BroadcastAddress( &broadcastAddress, PORT_SERVER + i );
			Netchan_OutOfBandPrint( &cls.socket_udp, &broadcastAddress, "%s", requeststring );
		}
		return;
	}

	//get what master
	masterAddress = Cmd_Argv( 2 );
	if( !masterAddress || !( *masterAddress ) ) {
		return;
	}

	modname = Cmd_Argv( 3 );
	// never allow anyone to use DEFAULT_BASEGAME as mod name
	if( !modname || !modname[0] || !Q_stricmp( modname, DEFAULT_BASEGAME ) ) {
		modname = APPLICATION_NOSPACES;
	}
	assert( modname[0] );

	// check memory cache
	for( i = 0; i < numMasterServers; i++ ) {
		if( !Q_stricmp( masterServers[i].addressString, masterAddress ) ) {
			master = &masterServers[i];
			break;
		}
	}

	if( !master ) {
		Com_Printf( "Address is not in master servers list: %s\n", masterAddress );
		return;
	}

	if( !master->resolverActive ) {
		if( master->address.type == NA_IP || master->address.type == NA_IP6 ) {
			CL_SendMasterServerQuery( &master->address, modname );
			return;
		}

		Com_DPrintf( "Resolving master server address: %s\n", master->addressString );

		if( master->resolverThread ) {
			JoinThread( master->resolverThread );
		}

		master->resolverActive = true;
		master->resolverThread = NewThread( CL_MasterResolverThreadFunc, master );

	}

	Q_strncpyz( master->delayedRequestModName, modname, sizeof( master->delayedRequestModName ) );
}

/*
* CL_ServerListFrame
*/
void CL_ServerListFrame( void ) {
	int i;
	masterserver_t *master;

	for( i = 0, master = masterServers; i < numMasterServers; i++, master++ ) {
		if( !master->delayedRequestModName[0] || master->resolverActive ) {
			continue;
		}

		if( master->address.type == NA_IP || master->address.type == NA_IP6 ) {
			CL_SendMasterServerQuery( &master->address, master->delayedRequestModName );
		}
		master->delayedRequestModName[0] = '\0';
	}
}

/*
* CL_InitServerList
*/
void CL_InitServerList( void ) {
	CL_FreeServerlist( &masterList );

	CL_MasterAddressCache_Init();
}

/*
* CL_ShutDownServerList
*/
void CL_ShutDownServerList( void ) {
	CL_FreeServerlist( &masterList );

	CL_MasterAddressCache_Shutdown();
}
