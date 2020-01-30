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

#include "server/server.h"
#include "qcommon/csprng.h"
#include "qcommon/compression.h"
#include "qcommon/hash.h"

server_constant_t svc;              // constant server info (trully persistant since sv_init)
server_static_t svs;                // persistant server info
server_t sv;                 // local server

/*
* SV_CreateBaseline
*
* Entity baselines are used to compress the update messages
* to the clients -- only the fields that differ from the
* baseline will be transmitted
*/
static void SV_CreateBaseline( void ) {
	edict_t *svent;
	int entnum;

	for( entnum = 1; entnum < sv.gi.num_edicts; entnum++ ) {
		svent = EDICT_NUM( entnum );

		if( !svent->r.inuse ) {
			continue;
		}
		if( svent->s.model == EMPTY_HASH && svent->s.sound == EMPTY_HASH && !svent->s.effects ) {
			continue;
		}

		svent->s.number = entnum;

		//
		// take current state as baseline
		//
		sv.baselines[entnum] = svent->s;
	}
}

/*
* SV_SetServerConfigStrings
*/
void SV_SetServerConfigStrings( void ) {
	snprintf( sv.configstrings[CS_MAXCLIENTS], sizeof( sv.configstrings[0] ), "%i", sv_maxclients->integer );
	Q_strncpyz( sv.configstrings[CS_HOSTNAME], Cvar_String( "sv_hostname" ), sizeof( sv.configstrings[0] ) );
}

/*
* SV_SpawnServer
* Change the server to a new map, taking all connected clients along with it.
*/
static void SV_SpawnServer( const char *mapname, bool devmap ) {
	int i;

	if( devmap ) {
		Cvar_ForceSet( "sv_cheats", "1" );
	}
	Cvar_FixCheatVars();

	Com_Printf( "SpawnServer: %s\n", mapname );

	svs.spawncount++;   // any partially connected client will be restarted

	Com_SetServerState( ss_dead );

	// wipe the entire per-level structure
	memset( &sv, 0, sizeof( sv ) );

	SV_ResetClientFrameCounters();
	svs.realtime = Sys_Milliseconds();
	svs.gametime = 0;

	Q_strncpyz( sv.mapname, mapname, sizeof( sv.mapname ) );

	SV_SetServerConfigStrings();

	sv.nextSnapTime = 1000;

	snprintf( sv.configstrings[CS_WORLDMODEL], sizeof( sv.configstrings[CS_WORLDMODEL] ), "maps/%s.bsp", mapname );

	u8 * buf;
	int length = FS_LoadFile( sv.configstrings[ CS_WORLDMODEL ], ( void ** ) &buf, NULL, 0 );
	if( buf == NULL ) {
		Com_Error( ERR_FATAL, "Couldn't load %s", sv.configstrings[ CS_WORLDMODEL ] );
	}

	Span< const u8 > compressed = Span< const u8 >( buf, length );
	Span< u8 > decompressed;
	defer { FREE( sys_allocator, decompressed.ptr ); };
	bool ok = Decompress( sv.configstrings[ CS_WORLDMODEL ], sys_allocator, compressed, &decompressed );
	if( !ok ) {
		Com_Error( ERR_FATAL, "Couldn't decompress %s", sv.configstrings[ CS_WORLDMODEL ] );
	}

	Span< const u8 > data = decompressed.ptr == NULL ? compressed : decompressed;
	svs.cms = CM_LoadMap( data );

	snprintf( sv.configstrings[CS_MAPCHECKSUM], sizeof( sv.configstrings[CS_MAPCHECKSUM] ), "%u", svs.cms->checksum );

	// set serverinfo variable
	Cvar_FullSet( "mapname", sv.mapname, CVAR_SERVERINFO | CVAR_READONLY, true );

	//
	// spawn the rest of the entities on the map
	//

	// precache and static commands can be issued during
	// map initialization
	sv.state = ss_loading;
	Com_SetServerState( sv.state );

	// load and spawn all other entities
	ge->InitLevel( sv.mapname, CM_EntityString( svs.cms ), CM_EntityStringLen( svs.cms ), 0 );

	// run two frames to allow everything to settle
	ge->RunFrame( svc.snapFrameTime );
	ge->RunFrame( svc.snapFrameTime );

	SV_CreateBaseline(); // create a baseline for more efficient communications

	// all precaches are complete
	sv.state = ss_game;
	Com_SetServerState( sv.state );
}

/*
* SV_InitGame
* A brand new game has been started
*/
void SV_InitGame( void ) {
	int i;
	edict_t *ent;
	netadr_t address, ipv6_address;
	bool socket_opened = false;

	// make sure the client is down
	CL_Disconnect( NULL );

	if( svs.initialized ) {
		// cause any connected clients to reconnect
		SV_ShutdownGame( "Server restarted", true );

		// SV_ShutdownGame will also call Cvar_GetLatchedVars
	} else {
		// get any latched variable changes (sv_maxclients, etc)
		Cvar_GetLatchedVars( CVAR_LATCH );
	}

	u64 entropy[ 2 ];
	CSPRNG_Bytes( entropy, sizeof( entropy ) );
	svs.rng = new_rng( entropy[ 0 ], entropy[ 1 ] );

	svs.initialized = true;

	if( sv_skilllevel->integer > 2 ) {
		Cvar_ForceSet( "sv_skilllevel", "2" );
	}
	if( sv_skilllevel->integer < 0 ) {
		Cvar_ForceSet( "sv_skilllevel", "0" );
	}

	// init clients
	if( sv_maxclients->integer < 1 ) {
		Cvar_FullSet( "sv_maxclients", "8", CVAR_SERVERINFO | CVAR_LATCH, true );
	} else if( sv_maxclients->integer > MAX_CLIENTS ) {
		Cvar_FullSet( "sv_maxclients", va( "%i", MAX_CLIENTS ), CVAR_SERVERINFO | CVAR_LATCH, true );
	}

	svs.spawncount = random_uniform( &svs.rng, 0, S16_MAX );
	svs.clients = ( client_t * ) Mem_Alloc( sv_mempool, sizeof( client_t ) * sv_maxclients->integer );
	svs.client_entities.num_entities = sv_maxclients->integer * UPDATE_BACKUP * MAX_SNAP_ENTITIES;
	svs.client_entities.entities = ( SyncEntityState * ) Mem_Alloc( sv_mempool, sizeof( SyncEntityState ) * svs.client_entities.num_entities );

	// init network stuff

	address.type = NA_NOTRANSMIT;
	ipv6_address.type = NA_NOTRANSMIT;

	if( !is_dedicated_server ) {
		NET_InitAddress( &address, NA_LOOPBACK );
		if( !NET_OpenSocket( &svs.socket_loopback, SOCKET_LOOPBACK, &address, true ) ) {
			Com_Error( ERR_FATAL, "Couldn't open loopback socket: %s\n", NET_ErrorString() );
		}
	}

	if( is_dedicated_server || sv_maxclients->integer > 1 ) {
		// IPv4
		NET_StringToAddress( sv_ip->string, &address );
		NET_SetAddressPort( &address, sv_port->integer );
		if( !NET_OpenSocket( &svs.socket_udp, SOCKET_UDP, &address, true ) ) {
			Com_Printf( "Error: Couldn't open UDP socket: %s\n", NET_ErrorString() );
		} else {
			socket_opened = true;
		}

		// IPv6
		NET_StringToAddress( sv_ip6->string, &ipv6_address );
		if( ipv6_address.type == NA_IP6 ) {
			NET_SetAddressPort( &ipv6_address, sv_port6->integer );
			if( !NET_OpenSocket( &svs.socket_udp6, SOCKET_UDP, &ipv6_address, true ) ) {
				Com_Printf( "Error: Couldn't open UDP6 socket: %s\n", NET_ErrorString() );
			} else {
				socket_opened = true;
			}
		} else {
			Com_Printf( "Error: invalid IPv6 address: %s\n", sv_ip6->string );
		}
	}

	if( is_dedicated_server && !socket_opened ) {
		Com_Error( ERR_FATAL, "Couldn't open any socket\n" );
	}

	// init game
	SV_InitGameProgs();
	for( i = 0; i < sv_maxclients->integer; i++ ) {
		ent = EDICT_NUM( i + 1 );
		ent->s.number = i + 1;
		svs.clients[i].edict = ent;
	}
}

/*
* SV_FinalMessage
*
* Used by SV_ShutdownGame to send a final message to all
* connected clients before the server goes down.  The messages are sent immediately,
* not just stuck on the outgoing message list, because the server is going
* to totally exit after returning from this function.
*/
static void SV_FinalMessage( const char *message, bool reconnect ) {
	int i, j;
	client_t *cl;

	for( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		if( cl->edict && ( cl->edict->r.svflags & SVF_FAKECLIENT ) ) {
			continue;
		}
		if( cl->state >= CS_CONNECTING ) {
			if( reconnect ) {
				SV_SendServerCommand( cl, "forcereconnect \"%s\"", message );
			} else {
				SV_SendServerCommand( cl, "disconnect %i \"%s\"", DROP_TYPE_GENERAL, message );
			}

			SV_InitClientMessage( cl, &tmpMessage, NULL, 0 );
			SV_AddReliableCommandsToMessage( cl, &tmpMessage );

			// send it twice
			for( j = 0; j < 2; j++ )
				SV_SendMessageToClient( cl, &tmpMessage );
		}
	}
}

/*
* SV_ShutdownGame
*
* Called when each game quits
*/
void SV_ShutdownGame( const char *finalmsg, bool reconnect ) {
	if( !svs.initialized ) {
		return;
	}

	if( svs.demo.file ) {
		SV_Demo_Stop_f();
	}

	if( svs.clients ) {
		SV_FinalMessage( finalmsg, reconnect );
	}

	SV_ShutdownGameProgs();

	NET_CloseSocket( &svs.socket_loopback );
	NET_CloseSocket( &svs.socket_udp );
	NET_CloseSocket( &svs.socket_udp6 );

	// get any latched variable changes (sv_maxclients, etc)
	Cvar_GetLatchedVars( CVAR_LATCH );

	if( svs.clients ) {
		Mem_Free( svs.clients );
		svs.clients = NULL;
	}

	if( svs.client_entities.entities ) {
		Mem_Free( svs.client_entities.entities );
		memset( &svs.client_entities, 0, sizeof( svs.client_entities ) );
	}

	if( svs.cms ) {
		CM_Free( svs.cms );
		svs.cms = NULL;
	}

	memset( &sv, 0, sizeof( sv ) );
	Com_SetServerState( sv.state );

	if( sv_mempool ) {
		Mem_EmptyPool( sv_mempool );
	}

	memset( &svs, 0, sizeof( svs ) );
}

/*
* SV_Map
* command from the console or progs.
*/
void SV_Map( const char *level, bool devmap ) {
	client_t *cl;
	int i;

	if( svs.demo.file ) {
		SV_Demo_Stop_f();
	}

	if( sv.state == ss_dead ) {
		SV_InitGame(); // the game is just starting
	}

	// remove all bots before changing map
	for( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		if( cl->state && cl->edict && ( cl->edict->r.svflags & SVF_FAKECLIENT ) ) {
			SV_DropClient( cl, DROP_TYPE_GENERAL, NULL );
		}
	}

	// wsw : Medar : this used to be at SV_SpawnServer, but we need to do it before sending changing
	// so we don't send frames after sending changing command
	// leave slots at start for clients only
	for( i = 0; i < sv_maxclients->integer; i++ ) {
		// needs to reconnect
		if( svs.clients[i].state > CS_CONNECTING ) {
			svs.clients[i].state = CS_CONNECTING;
		}

		svs.clients[i].lastframe = -1;
		memset( svs.clients[i].gameCommands, 0, sizeof( svs.clients[i].gameCommands ) );
	}

	SV_BroadcastCommand( "changing\n" );
	SV_SendClientMessages();
	SV_SpawnServer( level, devmap );
	SV_BroadcastCommand( "reconnect\n" );
}
