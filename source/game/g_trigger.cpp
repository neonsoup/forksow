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

#include "game/g_local.h"

/*
* G_TriggerWait
*
* Called always when using a trigger that supports wait flag
* Returns true if the trigger shouldn't be activated
*/
static bool G_TriggerWait( edict_t *ent, edict_t *other ) {
	if( GS_RaceGametype( &server_gs ) ) {
		if( other->trigger_entity == ent && other->trigger_timeout && other->trigger_timeout >= level.time ) {
			return true;
		}

		other->trigger_entity = ent;
		other->trigger_timeout = level.time + 1000 * ent->wait;
		return false;
	}

	if( ent->timeStamp >= level.time ) {
		return true;
	}

	// the wait time has passed, so set back up for another activation
	ent->timeStamp = level.time + ( ent->wait * 1000 );
	return false;
}

static void InitTrigger( edict_t *self ) {
	self->r.solid = SOLID_TRIGGER;
	self->movetype = MOVETYPE_NONE;
	GClip_SetBrushModel( self );
	self->r.svflags = SVF_NOCLIENT;
}


// the trigger was just activated
// ent->activator should be set to the activator so it can be held through a delay
// so wait for the delay time before firing
static void multi_trigger( edict_t *ent ) {
	if( G_TriggerWait( ent, ent->activator ) ) {
		return;     // already been triggered

	}
	G_UseTargets( ent, ent->activator );

	if( ent->wait <= 0 ) {
		// we can't just remove (self) here, because this is a touch function
		// called while looping through area links...
		ent->touch = NULL;
		ent->nextThink = level.time + 1;
		ent->think = G_FreeEdict;
	}
}

static void Use_Multi( edict_t *ent, edict_t *other, edict_t *activator ) {
	ent->activator = activator;
	multi_trigger( ent );
}

static void Touch_Multi( edict_t *self, edict_t *other, cplane_t *plane, int surfFlags ) {
	if( other->r.client ) {
		if( self->spawnflags & 2 ) {
			return;
		}
	} else {
		return;
	}

	if( self->s.team && self->s.team != other->s.team ) {
		return;
	}

	self->activator = other;
	multi_trigger( self );
}

static void trigger_enable( edict_t *self, edict_t *other, edict_t *activator ) {
	self->r.solid = SOLID_TRIGGER;
	self->use = Use_Multi;
	GClip_LinkEntity( self );
}

void SP_trigger_multiple( edict_t *ent ) {
	GClip_SetBrushModel( ent );

	if( st.noise != EMPTY_HASH ) {
		ent->sound = st.noise;
	}

	// gameteam field from editor
	if( st.gameteam >= TEAM_SPECTATOR && st.gameteam < GS_MAX_TEAMS ) {
		ent->s.team = st.gameteam;
	} else {
		ent->s.team = TEAM_SPECTATOR;
	}

	if( !ent->wait ) {
		ent->wait = 0.2f;
	}

	ent->touch = Touch_Multi;
	ent->movetype = MOVETYPE_NONE;
	ent->r.svflags |= SVF_NOCLIENT;

	if( ent->spawnflags & 4 ) {
		ent->r.solid = SOLID_NOT;
		ent->use = trigger_enable;
	} else {
		ent->r.solid = SOLID_TRIGGER;
		ent->use = Use_Multi;
	}

	GClip_LinkEntity( ent );
}

void SP_trigger_once( edict_t *ent ) {
	ent->wait = -1;
	SP_trigger_multiple( ent );
}

//==============================================================================
//
//trigger_always
//
//==============================================================================

static void trigger_always_think( edict_t *ent ) {
	G_UseTargets( ent, ent );
	G_FreeEdict( ent );
}

void SP_trigger_always( edict_t *ent ) {
	// we must have some delay to make sure our use targets are present
	if( ent->delay < 0.3f ) {
		ent->delay = 0.3f;
	}

	ent->think = trigger_always_think;
	ent->nextThink = level.time + 1000 * ent->delay;
}

//==============================================================================
//
//trigger_push
//
//==============================================================================

static void G_JumpPadSound( edict_t *ent ) {
	vec3_t org;
	edict_t *sound;

	if( ent->moveinfo.sound_start == EMPTY_HASH ) {
		return;
	}

	org[0] = ent->s.origin[0] + 0.5 * ( ent->r.mins[0] + ent->r.maxs[0] );
	org[1] = ent->s.origin[1] + 0.5 * ( ent->r.mins[1] + ent->r.maxs[1] );
	org[2] = ent->s.origin[2] + 0.5 * ( ent->r.mins[2] + ent->r.maxs[2] );

	sound = G_PositionedSound( org, CHAN_AUTO, ent->moveinfo.sound_start );
	if( sound && sound->r.areanum < 0 ) {
		// HACK: jumppad sounds may get trapped inside solid or go outside level bounds and get culled
		// so forcefully place them into legal space
		sound->r.areanum = ent->r.areanum < 0 ? ent->r.areanum2 : ent->r.areanum;
	}
}

#define PUSH_ONCE   1
#define MIN_TRIGGER_PUSH_REBOUNCE_TIME 100

static void trigger_push_touch( edict_t *self, edict_t *other, cplane_t *plane, int surfFlags ) {
	if( self->s.team && self->s.team != other->s.team ) {
		return;
	}

	if( G_TriggerWait( self, other ) ) {
		return;
	}

	// add an event
	if( other->r.client ) {
		GS_TouchPushTrigger( &server_gs, &other->r.client->ps, &self->s );
	} else {
		// pushing of non-clients
		if( other->movetype != MOVETYPE_BOUNCEGRENADE ) {
			return;
		}

		VectorCopy( self->s.origin2, other->velocity );
	}

	// game timers for fall damage
	G_JumpPadSound( self ); // play jump pad sound

	// self removal
	if( self->spawnflags & PUSH_ONCE ) {
		self->touch = NULL;
		self->nextThink = level.time + 1;
		self->think = G_FreeEdict;
	}
}

static void trigger_push_setup( edict_t *self ) {
	vec3_t origin, velocity;
	float height, time;
	float dist;
	edict_t *target;

	if( !self->target ) {
		vec3_t movedir;

		G_SetMovedir( self->s.angles, movedir );
		VectorScale( movedir, ( self->speed ? self->speed : 1000 ) * 10, self->s.origin2 );
		return;
	}

	target = G_PickTarget( self->target );
	if( !target ) {
		G_FreeEdict( self );
		return;
	}
	self->target_ent = target;

	VectorAdd( self->r.absmin, self->r.absmax, origin );
	VectorScale( origin, 0.5, origin );

	height = target->s.origin[2] - origin[2];
	time = sqrtf( height / ( 0.5f * level.gravity ) );
	if( !time ) {
		G_FreeEdict( self );
		return;
	}

	VectorSubtract( target->s.origin, origin, velocity );
	velocity[2] = 0;
	dist = VectorNormalize( velocity );
	VectorScale( velocity, dist / time, velocity );
	velocity[2] = time * level.gravity;
	VectorCopy( velocity, self->s.origin2 );
}

void SP_trigger_push( edict_t *self ) {
	InitTrigger( self );

	self->moveinfo.sound_start = st.noise != EMPTY_HASH ? st.noise : S_JUMPPAD;

	// gameteam field from editor
	if( st.gameteam >= TEAM_SPECTATOR && st.gameteam < GS_MAX_TEAMS ) {
		self->s.team = st.gameteam;
	} else {
		self->s.team = TEAM_SPECTATOR;
	}

	self->touch = trigger_push_touch;
	self->think = trigger_push_setup;
	self->nextThink = level.time + 1;
	self->r.svflags &= ~SVF_NOCLIENT;
	self->s.type = ET_PUSH_TRIGGER;
	GClip_LinkEntity( self ); // ET_PUSH_TRIGGER gets exceptions at linking so it's added for prediction
	self->timeStamp = level.time;
	if( !self->wait ) {
		self->wait = MIN_TRIGGER_PUSH_REBOUNCE_TIME * 0.001f;
	}
}

//==============================================================================
//
//trigger_hurt
//
//==============================================================================

static void hurt_use( edict_t *self, edict_t *other, edict_t *activator ) {
	if( self->r.solid == SOLID_NOT ) {
		self->r.solid = SOLID_TRIGGER;
	} else {
		self->r.solid = SOLID_NOT;
	}
	GClip_LinkEntity( self );

	if( !( self->spawnflags & 2 ) ) {
		self->use = NULL;
	}
}

static void hurt_touch( edict_t *self, edict_t *other, cplane_t *plane, int surfFlags ) {
	int dflags;
	int damage;

	if( !other->takedamage || G_IsDead( other ) ) {
		return;
	}

	if( self->s.team && self->s.team != other->s.team ) {
		return;
	}

	if( G_TriggerWait( self, other ) ) {
		return;
	}

	damage = self->dmg;
	if( self->spawnflags & ( 32 | 64 ) ) {
		damage = other->health + 1;
	}

	if( self->spawnflags & 8 ) {
		dflags = DAMAGE_NO_PROTECTION;
	} else {
		dflags = 0;
	}

	if( self->spawnflags & ( 32 | 64 ) ) { // KILL, FALL
		// play the death sound
		if( self->sound != EMPTY_HASH ) {
			G_Sound( other, CHAN_AUTO | CHAN_FIXED, self->sound );
			other->pain_debounce_time = level.time + 25;
		}
	} else if( !( self->spawnflags & 4 ) && self->sound != EMPTY_HASH ) {
		if( (int)( level.time * 0.001 ) & 1 ) {
			G_Sound( other, CHAN_AUTO | CHAN_FIXED, self->sound );
		}
	}

	G_Damage( other, self, world, vec3_origin, vec3_origin, other->s.origin, damage, damage, dflags, MOD_TRIGGER_HURT );
}

void SP_trigger_hurt( edict_t *self ) {
	InitTrigger( self );

	if( self->dmg > 300 ) { // HACK: force KILL spawnflag for big damages
		self->spawnflags |= 32;
	}

	self->sound = st.noise;

	// gameteam field from editor
	if( st.gameteam >= TEAM_SPECTATOR && st.gameteam < GS_MAX_TEAMS ) {
		self->s.team = st.gameteam;
	} else {
		self->s.team = TEAM_SPECTATOR;
	}

	self->touch = hurt_touch;

	if( !self->dmg ) {
		self->dmg = 5;
	}

	if( self->spawnflags & 16 || !self->wait ) {
		self->wait = 0.1f;
	}

	if( self->spawnflags & 1 ) {
		self->r.solid = SOLID_NOT;
	} else {
		self->r.solid = SOLID_TRIGGER;
	}

	if( self->spawnflags & 2 ) {
		self->use = hurt_use;
	}
}

//==============================================================================
//
//trigger_gravity
//
//==============================================================================

static void trigger_gravity_touch( edict_t *self, edict_t *other, cplane_t *plane, int surfFlags ) {
	if( self->s.team && self->s.team != other->s.team ) {
		return;
	}

	other->gravity = self->gravity;
}

void SP_trigger_gravity( edict_t *self ) {
	if( st.gravity == 0 ) {
		if( developer->integer ) {
			Com_Printf( "trigger_gravity without gravity set at %s\n", vtos( self->s.origin ) );
		}
		G_FreeEdict( self );
		return;
	}

	// gameteam field from editor
	if( st.gameteam >= TEAM_SPECTATOR && st.gameteam < GS_MAX_TEAMS ) {
		self->s.team = st.gameteam;
	} else {
		self->s.team = TEAM_SPECTATOR;
	}

	InitTrigger( self );
	self->gravity = atof( st.gravity );
	self->touch = trigger_gravity_touch;
}

static void TeleporterTouch( edict_t *self, edict_t *other, cplane_t *plane, int surfFlags ) {
	edict_t *dest;

	if( !G_PlayerCanTeleport( other ) ) {
		return;
	}

	if( ( self->s.team != TEAM_SPECTATOR ) && ( self->s.team != other->s.team ) ) {
		return;
	}
	if( self->spawnflags & 1 && other->r.client->ps.pmove.pm_type != PM_SPECTATOR ) {
		return;
	}

	// wait delay
	if( self->timeStamp > level.time ) {
		return;
	}

	self->timeStamp = level.time + ( self->wait * 1000 );

	dest = G_Find( NULL, FOFS( targetname ), self->target );
	if( !dest ) {
		if( developer->integer ) {
			Com_Printf( "Couldn't find destination.\n" );
		}
		return;
	}

	// play custom sound if any (played from the teleporter entrance)
	if( self->sound != EMPTY_HASH ) {
		vec3_t org;

		if( self->s.model != EMPTY_HASH ) {
			org[0] = self->s.origin[0] + 0.5 * ( self->r.mins[0] + self->r.maxs[0] );
			org[1] = self->s.origin[1] + 0.5 * ( self->r.mins[1] + self->r.maxs[1] );
			org[2] = self->s.origin[2] + 0.5 * ( self->r.mins[2] + self->r.maxs[2] );
		} else {
			VectorCopy( self->s.origin, org );
		}

		G_PositionedSound( org, CHAN_AUTO, self->sound );
	}

	G_TeleportPlayer( other, dest );
}

void SP_trigger_teleport( edict_t *ent ) {
	if( !ent->target ) {
		if( developer->integer ) {
			Com_Printf( "teleporter without a target.\n" );
		}
		G_FreeEdict( ent );
		return;
	}

	ent->sound = st.noise;

	// gameteam field from editor
	if( st.gameteam >= TEAM_SPECTATOR && st.gameteam < GS_MAX_TEAMS ) {
		ent->s.team = st.gameteam;
	} else {
		ent->s.team = TEAM_SPECTATOR;
	}

	InitTrigger( ent );
	GClip_LinkEntity( ent );
	ent->touch = TeleporterTouch;
}
