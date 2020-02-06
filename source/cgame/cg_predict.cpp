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

#include "cgame/cg_local.h"

static int cg_numSolids;
static SyncEntityState *cg_solidList[MAX_PARSE_ENTITIES];

static int cg_numTriggers;
static SyncEntityState *cg_triggersList[MAX_PARSE_ENTITIES];
static bool cg_triggersListTriggered[MAX_PARSE_ENTITIES];

static bool ucmdReady = false;

/*
* CG_PredictedEvent - shared code can fire events during prediction
*/
void CG_PredictedEvent( int entNum, int ev, int parm ) {
	if( ev >= PREDICTABLE_EVENTS_MAX ) {
		return;
	}

	// ignore this action if it has already been predicted (the unclosed ucmd has timestamp zero)
	if( ucmdReady && cg.predictingTimeStamp > cg.predictedEventTimes[ev] ) {
		cg.predictedEventTimes[ev] = cg.predictingTimeStamp;
		CG_EntityEvent( &cg_entities[entNum].current, ev, parm, true );
	}
}

void CG_PredictedFireWeapon( int entNum, WeaponType weapon ) {
	CG_PredictedEvent( entNum, EV_FIREWEAPON, weapon );
}

/*
* CG_CheckPredictionError
*/
void CG_CheckPredictionError( void ) {
	int delta[3];
	int frame;
	vec3_t origin;

	if( !cg.view.playerPrediction ) {
		return;
	}

	// calculate the last usercmd_t we sent that the server has processed
	frame = cg.frame.ucmdExecuted & CMD_MASK;

	// compare what the server returned with what we had predicted it to be
	VectorCopy( cg.predictedOrigins[frame], origin );

	if( cg.predictedGroundEntity != -1 ) {
		SyncEntityState *ent = &cg_entities[cg.predictedGroundEntity].current;
		if( ent->solid == SOLID_BMODEL ) {
			if( ent->linearMovement ) {
				vec3_t move;
				GS_LinearMovementDelta( ent, cg.oldFrame.serverTime, cg.frame.serverTime, move );
				VectorAdd( cg.predictedOrigins[frame], move, origin );
			}
		}
	}

	VectorSubtract( cg.frame.playerState.pmove.origin, origin, delta );

	// save the prediction error for interpolation
	if( Abs( delta[0] ) > 128 || Abs( delta[1] ) > 128 || Abs( delta[2] ) > 128 ) {
		if( cg_showMiss->integer ) {
			Com_Printf( "prediction miss on %" PRIi64 ": %i\n", cg.frame.serverFrame, Abs( delta[0] ) + Abs( delta[1] ) + Abs( delta[2] ) );
		}
		VectorClear( cg.predictionError );          // a teleport or something
	} else {
		if( cg_showMiss->integer && ( delta[0] || delta[1] || delta[2] ) ) {
			Com_Printf( "prediction miss on %" PRIi64" : %i\n", cg.frame.serverFrame, Abs( delta[0] ) + Abs( delta[1] ) + Abs( delta[2] ) );
		}
		VectorCopy( cg.frame.playerState.pmove.origin, cg.predictedOrigins[frame] );
		VectorCopy( delta, cg.predictionError ); // save for error interpolation
	}
}

/*
* CG_BuildSolidList
*/
void CG_BuildSolidList( void ) {
	cg_numSolids = 0;
	cg_numTriggers = 0;

	for( int i = 0; i < cg.frame.numEntities; i++ ) {
		const SyncEntityState * ent = &cg.frame.parsedEntities[i & ( MAX_PARSE_ENTITIES - 1 )];
		if( ISEVENTENTITY( ent ) ) {
			continue;
		}

		if( ent->solid ) {
			switch( ent->type ) {
				// the following entities can never be solid
				case ET_ROCKET:
				case ET_GRENADE:
				case ET_PLASMA:
				case ET_LASERBEAM:
				case ET_HUD:
				case ET_LASER:
				case ET_SPIKES:
					break;

				case ET_PUSH_TRIGGER:
					cg_triggersList[cg_numTriggers++] = &cg_entities[ ent->number ].current;
					break;

				default:
					cg_solidList[cg_numSolids++] = &cg_entities[ ent->number ].current;
					break;
			}
		}
	}
}

/*
* CG_ClipEntityContact
*/
static bool CG_ClipEntityContact( const vec3_t origin, const vec3_t mins, const vec3_t maxs, int entNum ) {
	centity_t *cent;
	struct cmodel_s *cmodel;
	trace_t tr;
	vec3_t absmins, absmaxs;
	vec3_t entorigin, entangles;
	int64_t serverTime = cg.frame.serverTime;

	if( !mins ) {
		mins = vec3_origin;
	}
	if( !maxs ) {
		maxs = vec3_origin;
	}

	// find the cmodel
	cmodel = CG_CModelForEntity( entNum );
	if( !cmodel ) {
		return false;
	}

	cent = &cg_entities[entNum];

	// find the origin
	if( cent->current.solid == SOLID_BMODEL ) { // special value for bmodel
		if( cent->current.linearMovement ) {
			GS_LinearMovement( &cent->current, serverTime, entorigin );
		} else {
			VectorCopy( cent->current.origin, entorigin );
		}
		VectorCopy( cent->current.angles, entangles );
	} else {   // encoded bbox
		VectorCopy( cent->current.origin, entorigin );
		VectorClear( entangles ); // boxes don't rotate
	}

	// convert the box to compare to absolute coordinates
	VectorAdd( origin, mins, absmins );
	VectorAdd( origin, maxs, absmaxs );
	CM_TransformedBoxTrace( cl.cms, &tr, vec3_origin, vec3_origin, absmins, absmaxs, cmodel, MASK_ALL, entorigin, entangles );
	return tr.startsolid == true || tr.allsolid == true;
}

/*
* CG_Predict_TouchTriggers
*/
void CG_Predict_TouchTriggers( pmove_t *pm, vec3_t previous_origin ) {
	int i;
	SyncEntityState *state;

	// fixme: more accurate check for being able to touch or not
	if( pm->playerState->pmove.pm_type != PM_NORMAL ) {
		return;
	}

	for( i = 0; i < cg_numTriggers; i++ ) {
		state = cg_triggersList[i];

		if( state->type == ET_PUSH_TRIGGER ) {
			if( !cg_triggersListTriggered[i] ) {
				if( CG_ClipEntityContact( pm->playerState->pmove.origin, pm->mins, pm->maxs, state->number ) ) {
					GS_TouchPushTrigger( &client_gs, pm->playerState, state );
					cg_triggersListTriggered[i] = true;
				}
			}
		}
	}
}

/*
* CG_ClipMoveToEntities
*/
static void CG_ClipMoveToEntities( const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int ignore, int contentmask, trace_t *tr ) {
	int i, x, zd, zu;
	trace_t trace;
	vec3_t origin, angles;
	SyncEntityState *ent;
	struct cmodel_s *cmodel;
	vec3_t bmins, bmaxs;
	int64_t serverTime = cg.frame.serverTime;

	for( i = 0; i < cg_numSolids; i++ ) {
		ent = cg_solidList[i];

		if( ent->number == ignore ) {
			continue;
		}
		if( !( contentmask & CONTENTS_CORPSE ) && ( ( ent->type == ET_CORPSE ) || ( ent->type == ET_GIB ) ) ) {
			continue;
		}

		if( ent->type == ET_PLAYER ) {
			int teammask = contentmask & ( CONTENTS_TEAMALPHA | CONTENTS_TEAMBETA );
			if( teammask != 0 ) {
				int team = teammask == CONTENTS_TEAMALPHA ? TEAM_ALPHA : TEAM_BETA;
				if( ent->team != team )
					continue;
			}
		}

		if( ent->solid == SOLID_BMODEL ) { // special value for bmodel
			cmodel = CM_InlineModel( cl.cms, ent->modelindex );
			if( !cmodel ) {
				continue;
			}

			if( ent->linearMovement ) {
				GS_LinearMovement( ent, serverTime, origin );
			} else {
				VectorCopy( ent->origin, origin );
			}

			VectorCopy( ent->angles, angles );
		} else {   // encoded bbox
			x = 8 * ( ent->solid & 31 );
			zd = 8 * ( ( ent->solid >> 5 ) & 31 );
			zu = 8 * ( ( ent->solid >> 10 ) & 63 ) - 32;

			bmins[0] = bmins[1] = -x;
			bmaxs[0] = bmaxs[1] = x;
			bmins[2] = -zd;
			bmaxs[2] = zu;

			VectorCopy( ent->origin, origin );
			VectorClear( angles ); // boxes don't rotate

			if( ent->type == ET_PLAYER || ent->type == ET_CORPSE ) {
				cmodel = CM_OctagonModelForBBox( cl.cms, bmins, bmaxs );
			} else {
				cmodel = CM_ModelForBBox( cl.cms, bmins, bmaxs );
			}
		}

		CM_TransformedBoxTrace( cl.cms, &trace, (float *)start, (float *)end, (float *)mins, (float *)maxs, cmodel, contentmask, origin, angles );
		if( trace.allsolid || trace.fraction < tr->fraction ) {
			trace.ent = ent->number;
			*tr = trace;
		} else if( trace.startsolid ) {
			tr->startsolid = true;
		}

		if( tr->allsolid ) {
			return;
		}
	}
}

/*
* CG_Trace
*/
void CG_Trace( trace_t *t, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int ignore, int contentmask ) {
	// check against world
	CM_TransformedBoxTrace( cl.cms, t, start, end, mins, maxs, NULL, contentmask, NULL, NULL );
	t->ent = t->fraction < 1.0 ? 0 : -1; // world entity is 0
	if( t->fraction == 0 ) {
		return; // blocked by the world
	}

	// check all other solid models
	CG_ClipMoveToEntities( start, mins, maxs, end, ignore, contentmask, t );
}

/*
* CG_PointContents
*/
int CG_PointContents( const vec3_t point ) {
	int i;
	SyncEntityState *ent;
	struct cmodel_s *cmodel;
	int contents;

	contents = CM_TransformedPointContents( cl.cms, (float *)point, NULL, NULL, NULL );

	for( i = 0; i < cg_numSolids; i++ ) {
		ent = cg_solidList[i];
		if( ent->solid != SOLID_BMODEL ) { // special value for bmodel
			continue;
		}

		cmodel = CM_InlineModel( cl.cms, ent->modelindex );
		if( cmodel ) {
			contents |= CM_TransformedPointContents( cl.cms, (float *)point, cmodel, ent->origin, ent->angles );
		}
	}

	return contents;
}


static float predictedSteps[CMD_BACKUP]; // for step smoothing
/*
* CG_PredictAddStep
*/
static void CG_PredictAddStep( int virtualtime, int predictiontime, float stepSize ) {

	float oldStep;
	int delta;

	// check for stepping up before a previous step is completed
	delta = cls.realtime - cg.predictedStepTime;
	if( delta < PREDICTED_STEP_TIME ) {
		oldStep = cg.predictedStep * ( (float)( PREDICTED_STEP_TIME - delta ) / (float)PREDICTED_STEP_TIME );
	} else {
		oldStep = 0;
	}

	cg.predictedStep = oldStep + stepSize;
	cg.predictedStepTime = cls.realtime - ( predictiontime - virtualtime );
}

/*
* CG_PredictSmoothSteps
*/
static void CG_PredictSmoothSteps( void ) {
	int64_t outgoing;
	int64_t frame;
	usercmd_t cmd;
	int i;
	int virtualtime = 0, predictiontime = 0;

	cg.predictedStepTime = 0;
	cg.predictedStep = 0;

	trap_NET_GetCurrentState( NULL, &outgoing, NULL );

	i = outgoing;
	while( predictiontime < PREDICTED_STEP_TIME ) {
		if( outgoing - i >= CMD_BACKUP ) {
			break;
		}

		frame = i & CMD_MASK;
		trap_NET_GetUserCmd( frame, &cmd );
		predictiontime += cmd.msec;
		i--;
	}

	// run frames
	while( ++i <= outgoing ) {
		frame = i & CMD_MASK;
		trap_NET_GetUserCmd( frame, &cmd );
		virtualtime += cmd.msec;

		if( predictedSteps[frame] ) {
			CG_PredictAddStep( virtualtime, predictiontime, predictedSteps[frame] );
		}
	}
}

/*
* CG_PredictMovement
*
* Sets cg.predictedVelocty, cg.predictedOrigin and cg.predictedAngles
*/
void CG_PredictMovement( void ) {
	ZoneScoped;

	int64_t ucmdExecuted, ucmdHead;
	int64_t frame;
	pmove_t pm;

	trap_NET_GetCurrentState( NULL, &ucmdHead, NULL );
	ucmdExecuted = cg.frame.ucmdExecuted;

	if( ucmdHead - cg.predictFrom >= CMD_BACKUP ) {
		cg.predictFrom = 0;
	}

	if( cg.predictFrom > 0 ) {
		ucmdExecuted = cg.predictFrom;
		cg.predictedPlayerState = cg.predictFromPlayerState;
		cg_entities[cg.frame.playerState.POVnum].current = cg.predictFromEntityState;
	} else {
		cg.predictedPlayerState = cg.frame.playerState; // start from the final position
	}

	cg.predictedPlayerState.POVnum = cgs.playerNum + 1;

	// if we are too far out of date, just freeze
	if( ucmdHead - ucmdExecuted >= CMD_BACKUP ) {
		if( cg_showMiss->integer ) {
			Com_Printf( "exceeded CMD_BACKUP\n" );
		}

		cg.predictingTimeStamp = cl.serverTime;
		return;
	}

	// copy current state to pmove
	memset( &pm, 0, sizeof( pm ) );
	pm.playerState = &cg.predictedPlayerState;

	// clear the triggered toggles for this prediction round
	memset( &cg_triggersListTriggered, false, sizeof( cg_triggersListTriggered ) );

	// run frames
	while( ++ucmdExecuted <= ucmdHead ) {
		frame = ucmdExecuted & CMD_MASK;
		trap_NET_GetUserCmd( frame, &pm.cmd );

		ucmdReady = ( pm.cmd.serverTimeStamp != 0 );
		if( ucmdReady ) {
			cg.predictingTimeStamp = pm.cmd.serverTimeStamp;
		}

		Pmove( &client_gs, &pm );

		// copy for stair smoothing
		predictedSteps[frame] = pm.step;

		if( ucmdReady ) { // hmm fixme: the wip command may not be run enough time to get proper key presses
			cg_entities[cg.predictedPlayerState.POVnum].current.weapon = GS_ThinkPlayerWeapon( &client_gs, &cg.predictedPlayerState, pm.cmd.buttons, pm.cmd.msec, 0 );
		}

		// save for debug checking
		VectorCopy( cg.predictedPlayerState.pmove.origin, cg.predictedOrigins[frame] ); // store for prediction error checks

		// backup the last predicted ucmd which has a timestamp (it's closed)
		if( ucmdExecuted == ucmdHead - 1 ) {
			if( ucmdExecuted != cg.predictFrom ) {
				cg.predictFrom = ucmdExecuted;
				cg.predictFromPlayerState = cg.predictedPlayerState;
				cg.predictFromEntityState = cg_entities[cg.frame.playerState.POVnum].current;
			}
		}
	}

	cg.predictedGroundEntity = pm.groundentity;

	// compensate for ground entity movement
	if( pm.groundentity != -1 ) {
		SyncEntityState *ent = &cg_entities[pm.groundentity].current;

		if( ent->solid == SOLID_BMODEL ) {
			if( ent->linearMovement ) {
				vec3_t move;
				int64_t serverTime;

				serverTime = GS_MatchPaused( &client_gs ) ? cg.frame.serverTime : cl.serverTime + cgs.extrapolationTime;
				GS_LinearMovementDelta( ent, cg.frame.serverTime, serverTime, move );
				VectorAdd( cg.predictedPlayerState.pmove.origin, move, cg.predictedPlayerState.pmove.origin );
			}
		}
	}

	CG_PredictSmoothSteps();
}
