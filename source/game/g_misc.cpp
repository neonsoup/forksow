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

void ThrowSmallPileOfGibs( edict_t *self, const vec3_t knockback, int damage ) {
	int contents = G_PointContents( self->s.origin );
	if( contents & CONTENTS_NODROP ) {
		return;
	}

	vec3_t origin;
	VectorCopy( self->s.origin, origin );
	self->s.origin[2] += 4;

	// clamp the damage value since events do bitwise & 0xFF on the passed param
	damage = Clamp( 0, damage, 255 );

	edict_t * event = G_SpawnEvent( EV_SPOG, damage, origin );
	event->s.team = self->s.team;
	VectorAdd( self->velocity, knockback, event->s.origin2 );
}

void BecomeExplosion1( edict_t *self ) {
	int radius;

	// turn entity into event
	if( self->projectileInfo.radius > 255 * 8 ) {
		radius = ( self->projectileInfo.radius * 1 / 16 ) & 0xFF;
		if( radius < 1 ) {
			radius = 1;
		}

		G_MorphEntityIntoEvent( self, EV_EXPLOSION2, radius );
	} else {
		radius = ( self->projectileInfo.radius * 1 / 8 ) & 0xFF;
		if( radius < 1 ) {
			radius = 1;
		}

		G_MorphEntityIntoEvent( self, EV_EXPLOSION1, radius );
	}

	self->r.svflags &= ~SVF_NOCLIENT;
}

static void path_corner_touch( edict_t *self, edict_t *other, cplane_t *plane, int surfFlags ) {
	vec3_t v;
	edict_t *next;

	if( other->movetarget != self ) {
		return;
	}
	if( other->enemy ) {
		return;
	}

	if( self->pathtarget ) {
		const char *savetarget;

		savetarget = self->target;
		self->target = self->pathtarget;
		G_UseTargets( self, other );
		self->target = savetarget;
	}

	if( self->target ) {
		next = G_PickTarget( self->target );
	} else {
		next = NULL;
	}

	if( next && ( next->spawnflags & 1 ) ) {
		VectorCopy( next->s.origin, v );
		v[2] += next->r.mins[2];
		v[2] -= other->r.mins[2];
		VectorCopy( v, other->s.origin );
		next = G_PickTarget( next->target );
		other->s.teleported = true;
	}

	other->movetarget = next;

	VectorSubtract( other->movetarget->s.origin, other->s.origin, v );
}

void SP_path_corner( edict_t *self ) {
	if( !self->targetname ) {
		if( developer->integer ) {
			Com_Printf( "path_corner with no targetname at %s\n", vtos( self->s.origin ) );
		}
		G_FreeEdict( self );
		return;
	}

	self->r.solid = SOLID_TRIGGER;
	self->touch = path_corner_touch;
	VectorSet( self->r.mins, -8, -8, -8 );
	VectorSet( self->r.maxs, 8, 8, 8 );
	self->r.svflags |= SVF_NOCLIENT;
	GClip_LinkEntity( self );
}

#define START_OFF   64

//========================================================
//
//	FUNC_*
//
//========================================================

static void func_wall_use( edict_t *self, edict_t *other, edict_t *activator ) {
	if( self->r.solid == SOLID_NOT ) {
		self->r.solid = SOLID_YES;
		self->r.svflags &= ~SVF_NOCLIENT;
		KillBox( self, MOD_CRUSH, vec3_origin );
	} else {
		self->r.solid = SOLID_NOT;
		self->r.svflags |= SVF_NOCLIENT;
	}
	GClip_LinkEntity( self );

	if( !( self->spawnflags & 2 ) ) {
		self->use = NULL;
	}
}

void SP_func_wall( edict_t *self ) {
	G_InitMover( self );
	self->s.solid = SOLID_NOT;

	// just a wall
	if( ( self->spawnflags & 7 ) == 0 ) {
		self->r.solid = SOLID_YES;
		GClip_LinkEntity( self );
		return;
	}

	// it must be TRIGGER_SPAWN
	if( !( self->spawnflags & 1 ) ) {
		//		Com_Printf ("func_wall missing TRIGGER_SPAWN\n");
		self->spawnflags |= 1;
	}

	// yell if the spawnflags are odd
	if( self->spawnflags & 4 ) {
		if( !( self->spawnflags & 2 ) ) {
			if( developer->integer ) {
				Com_Printf( "func_wall START_ON without TOGGLE\n" );
			}
			self->spawnflags |= 2;
		}
	}

	self->use = func_wall_use;
	if( self->spawnflags & 4 ) {
		self->r.solid = SOLID_YES;
	} else {
		self->r.solid = SOLID_NOT;
		self->r.svflags |= SVF_NOCLIENT;
	}
	GClip_LinkEntity( self );
}

//===========================================================

void SP_func_static( edict_t *ent ) {
	G_InitMover( ent );
	ent->movetype = MOVETYPE_NONE;
	ent->r.svflags = SVF_BROADCAST;
	GClip_LinkEntity( ent );
}

//===========================================================

static void func_explosive_explode( edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, const vec3_t point ) {
	vec3_t origin, bakorigin;
	vec3_t size;
	int mass;

	// do not explode unless visible
	if( self->r.svflags & SVF_NOCLIENT ) {
		return;
	}

	self->takedamage = DAMAGE_NO;

	// bmodel origins are (0 0 0), we need to adjust that here
	VectorCopy( self->s.origin, bakorigin );
	VectorScale( self->r.size, 0.5, size );
	VectorAdd( self->r.absmin, size, origin );
	VectorCopy( origin, self->s.origin );

	if( self->projectileInfo.maxDamage ) {
		G_RadiusDamage( self, attacker, NULL, NULL, MOD_EXPLOSIVE );
	}

	VectorSubtract( self->s.origin, inflictor->s.origin, self->velocity );
	VectorNormalize( self->velocity );
	VectorScale( self->velocity, 150, self->velocity );

	// start chunks towards the center
	VectorScale( size, 0.5, size );
	mass = self->projectileInfo.radius * 0.75;
	if( !mass ) {
		mass = 75;
	}

	G_UseTargets( self, attacker );

	if( self->projectileInfo.maxDamage ) {
		edict_t *explosion;

		explosion = G_Spawn();
		VectorCopy( self->s.origin, explosion->s.origin );
		explosion->projectileInfo = self->projectileInfo;
		BecomeExplosion1( explosion );
	}

	if( self->use == NULL ) {
		G_FreeEdict( self );
		return;
	}

	self->health = self->max_health;
	self->r.solid = SOLID_NOT;
	self->r.svflags |= SVF_NOCLIENT;
	VectorCopy( bakorigin, self->s.origin );
	VectorClear( self->velocity );
	GClip_LinkEntity( self );
}

static void func_explosive_think( edict_t *self ) {
	func_explosive_explode( self, self, self->enemy, self->count, vec3_origin );
}

static void func_explosive_use( edict_t *self, edict_t *other, edict_t *activator ) {
	self->enemy = other;
	self->count = ceilf( self->health );

	if( self->delay ) {
		self->think = func_explosive_think;
		self->nextThink = level.time + self->delay * 1000;
		return;
	}

	func_explosive_explode( self, self, other, self->count, vec3_origin );
}

static void func_explosive_spawn( edict_t *self, edict_t *other, edict_t *activator ) {
	self->r.solid = SOLID_YES;
	self->r.svflags &= ~SVF_NOCLIENT;
	self->use = NULL;
	KillBox( self, MOD_CRUSH, vec3_origin );
	GClip_LinkEntity( self );
}

void SP_func_explosive( edict_t *self ) {
	G_InitMover( self );

	self->projectileInfo.maxDamage = max( self->dmg, 1 );
	self->projectileInfo.minDamage = min( self->dmg, 1 );
	self->projectileInfo.maxKnockback = self->projectileInfo.maxDamage;
	self->projectileInfo.minKnockback = self->projectileInfo.minDamage;
	self->projectileInfo.radius = st.radius;
	if( !self->projectileInfo.radius ) {
		self->projectileInfo.radius = self->dmg + 100;
	}

	if( self->spawnflags & 1 ) {
		self->r.svflags |= SVF_NOCLIENT;
		self->r.solid = SOLID_NOT;
		self->use = func_explosive_spawn;
	} else {
		if( self->targetname ) {
			self->use = func_explosive_use;
		}
	}

	if( self->use != func_explosive_use ) {
		if( !self->health ) {
			self->health = 100;
		}
		self->die = func_explosive_explode;
		self->takedamage = DAMAGE_YES;
	}
	self->max_health = self->health;
	self->s.effects = EF_WORLD_MODEL;

	GClip_LinkEntity( self );
}

//========================================================
//
//	MISC_*
//
//========================================================

void SP_misc_model( edict_t *ent ) {
	G_FreeEdict( ent );
}

void SP_model( edict_t *ent ) {
	ent->r.svflags &= ~SVF_NOCLIENT;
	ent->s.model = StringHash( ent->model );
	GClip_LinkEntity( ent );
}
