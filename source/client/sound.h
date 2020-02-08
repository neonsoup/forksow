#pragma once

#include "qcommon/types.h"

struct SoundEffect;

struct ImmediateSoundHandle {
	u64 x;
};

bool S_Init();
void S_Shutdown();

const SoundEffect * FindSoundEffect( StringHash name );
const SoundEffect * FindSoundEffect( const char * name );

void S_Update( Vec3 origin, Vec3 velocity, const mat3_t axis );
void S_UpdateEntity( int ent_num, Vec3 origin, Vec3 velocity );

void S_StartFixedSound( const SoundEffect * sfx, Vec3 origin, int channel, float volume );
void S_StartEntitySound( const SoundEffect * sfx, int ent_num, int channel, float volume );
void S_StartGlobalSound( const SoundEffect * sfx, int channel, float volume );
void S_StartLocalSound( const SoundEffect * sfx, int channel, float volume );
ImmediateSoundHandle S_ImmediateEntitySound( const SoundEffect * sfx, int ent_num, float volume, ImmediateSoundHandle handle );
ImmediateSoundHandle S_ImmediateLineSound( const SoundEffect * sfx, Vec3 start, Vec3 end, float volume, ImmediateSoundHandle handle );
void S_StopAllSounds( bool stopMusic );

void S_StartMenuMusic();
void S_StopBackgroundTrack();
