/*
Copyright (C) 2009 German Garcia Fernandez ("Jal")

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

#include "qcommon/base.h"
#include "qcommon/qcommon.h"
#include "cgame/cg_local.h"

// Thanks to Xavatar (xavatar2004@hotmail.com) for the path spline implementation

//===================================================================

#define DEFAULT_SUBTITLE_SECONDS 9

char *demoscriptname;
bool democam_editing_mode;
int64_t demo_initial_timestamp;
int64_t demo_time;

static bool CamIsFree;

#define CG_DemoCam_UpdateDemoTime() ( demo_time = cl.serverTime - demo_initial_timestamp )

//===================================================================

enum
{
	DEMOCAM_FIRSTPERSON,
	DEMOCAM_THIRDPERSON,
	DEMOCAM_POSITIONAL,
	DEMOCAM_PATH_LINEAR,
	DEMOCAM_PATH_SPLINE,
	DEMOCAM_ORBITAL,

	DEMOCAM_MAX_TYPES
};

static const char *cam_TypeNames[] = {
	"FirstPerson",
	"ThirdPerson",
	"Positional",
	"Path_linear",
	"Path_spline",
	"orbital",
	NULL
};

typedef struct cg_democam_s
{
	int type;
	int64_t timeStamp;
	int trackEnt;
	vec3_t origin;
	vec3_t angles;
	int fov;
	vec3_t tangent;
	vec3_t angles_tangent;
	float speed;
	struct cg_democam_s *next;
} cg_democam_t;

cg_democam_t *cg_cams_headnode = NULL;
cg_democam_t *currentcam, *nextcam;

static vec3_t cam_origin, cam_angles, cam_velocity;
static float cam_fov = 90;
static int cam_viewtype;
static int cam_POVent;
static bool cam_3dPerson;
static vec3_t cam_orbital_angles;
static float cam_orbital_radius;

/*
* CG_Democam_FindCurrent
*/
static cg_democam_t *CG_Democam_FindCurrent( int64_t time ) {
	int64_t higher_time = 0;
	cg_democam_t *cam, *curcam;

	cam = cg_cams_headnode;
	curcam = NULL;
	while( cam != NULL ) {
		if( curcam == NULL || ( cam->timeStamp <= time && cam->timeStamp > higher_time ) ) {
			higher_time = cam->timeStamp;
			curcam = cam;
		}
		cam = cam->next;
	}

	return curcam;
}

/*
* CG_Democam_FindNext
*/
static cg_democam_t *CG_Democam_FindNext( int64_t time ) {
	int64_t lower_time = INT64_MAX;
	cg_democam_t *cam, *ncam;

	cam = cg_cams_headnode;
	ncam = NULL;
	while( cam != NULL ) {
		if( cam->timeStamp > time && cam->timeStamp < lower_time ) {
			lower_time = cam->timeStamp;
			ncam = cam;
		}
		cam = cam->next;
	}

	return ncam;
}

/*
* CG_Democam_RegisterCam
*/
static cg_democam_t *CG_Democam_RegisterCam( int type ) {
	cg_democam_t *cam;

	CG_DemoCam_UpdateDemoTime();

	cam = cg_cams_headnode;
	while( cam != NULL ) {
		if( cam->timeStamp == demo_time ) { // a cam exists with the very same timestamp
			Com_Printf( "warning: There was a cam with the same timestamp, it's being replaced\n" );
			break;
		}
		cam = cam->next;
	}

	if( cam == NULL ) {
		cam = ( cg_democam_t * )CG_Malloc( sizeof( cg_democam_t ) );
		cam->next = cg_cams_headnode;
		cg_cams_headnode = cam;
	}

	cam->timeStamp = demo_time;
	cam->type = type;
	VectorCopy( cam_origin, cam->origin );
	VectorCopy( cam_angles, cam->angles );
	if( type == DEMOCAM_ORBITAL ) { // in orbital cams, the angles are the angular velocity
		VectorSet( cam->angles, 0, 96, 0 );
	}
	if( type == DEMOCAM_FIRSTPERSON || type == DEMOCAM_THIRDPERSON ) {
		cam->fov = 0;
	} else {
		cam->fov = 90;
	}

	return cam;
}

/*
* CG_Democam_UnregisterCam
*/
static void CG_Democam_UnregisterCam( cg_democam_t *cam ) {
	cg_democam_t *tcam;

	if( !cam ) {
		return;
	}

	// headnode shortcut
	if( cg_cams_headnode == cam ) {
		cg_cams_headnode = cg_cams_headnode->next;
		CG_Free( cam );
		return;
	}

	// find the camera which has this one as next;
	tcam = cg_cams_headnode;
	while( tcam != NULL ) {
		if( tcam->next == cam ) {
			tcam->next = cam->next;

			CG_Free( cam );
			break;
		}
		tcam = tcam->next;
	}
}

/*
* CG_Democam_FreeCams
*/
void CG_Democam_FreeCams( void ) {
	while( cg_cams_headnode )
		CG_Democam_UnregisterCam( cg_cams_headnode );

	cg_cams_headnode = NULL;
}

//===================================================================

typedef struct cg_subtitles_s
{
	int64_t timeStamp;
	int64_t maxDuration;
	bool highprint;
	char *text;

	struct cg_subtitles_s *next;
} cg_subtitle_t;

static cg_subtitle_t *cg_subs_headnode = NULL;

static cg_subtitle_t *CG_Democam_FindCurrentSubtitle( void ) {
	int64_t higher_time = 0;
	cg_subtitle_t *sub, *currentsub;

	sub = cg_subs_headnode;
	currentsub = NULL;
	while( sub != NULL ) {
		if( ( currentsub == NULL || sub->timeStamp > higher_time ) && sub->timeStamp <= demo_time &&
			( sub->timeStamp + sub->maxDuration > demo_time ) ) {
			higher_time = sub->timeStamp;
			currentsub = sub;
		}
		sub = sub->next;
	}

	return currentsub;
}

/*
* CG_Democam_RegisterSubtitle
*/
static cg_subtitle_t *CG_Democam_RegisterSubtitle( void ) {
	cg_subtitle_t *sub;

	CG_DemoCam_UpdateDemoTime();

	sub = cg_subs_headnode;
	while( sub != NULL ) {
		if( sub->timeStamp == demo_time ) { // a subtitle exists with the very same timestamp
			Com_Printf( "warning: There was a subtitle with the same timestamp, it's being replaced\n" );
			break;
		}
		sub = sub->next;
	}

	if( sub == NULL ) {
		sub = ( cg_subtitle_t * )CG_Malloc( sizeof( cg_subtitle_t ) );
		sub->next = cg_subs_headnode;
		cg_subs_headnode = sub;
	}

	sub->timeStamp = demo_time;
	sub->maxDuration = DEFAULT_SUBTITLE_SECONDS * 1000;
	sub->highprint = false;
	return sub;
}

/*
* CG_Democam_UnregisterSubtitle
*/
static void CG_Democam_UnregisterSubtitle( cg_subtitle_t *sub ) {
	cg_subtitle_t *tsub;

	if( !sub ) {
		return;
	}

	// headnode shortcut
	if( cg_subs_headnode == sub ) {
		cg_subs_headnode = cg_subs_headnode->next;
		if( sub->text ) {
			CG_Free( sub->text );
		}
		CG_Free( sub );
		return;
	}

	// find the camera which has this one as next;
	tsub = cg_subs_headnode;
	while( tsub != NULL ) {
		if( tsub->next == sub ) {
			tsub->next = sub->next;

			if( sub->text ) {
				CG_Free( sub->text );
			}
			CG_Free( sub );
			break;
		}
		tsub = tsub->next;
	}
}

/*
* CG_Democam_FreeSubtitles
*/
void CG_Democam_FreeSubtitles( void ) {
	while( cg_subs_headnode )
		CG_Democam_UnregisterSubtitle( cg_subs_headnode );

	cg_subs_headnode = NULL;
}

//===================================================================

/*
* CG_Democam_ExecutePathAnalisys
*/
static void CG_Democam_ExecutePathAnalysis( void ) {
	int64_t pathtime;
	cg_democam_t *ccam, *ncam, *pcam, *sncam;
	int count;

	pathtime = 0;

	count = 0;
	while( ( ncam = CG_Democam_FindNext( pathtime ) ) != NULL ) {
		ccam = CG_Democam_FindCurrent( pathtime );
		if( ccam ) {
			count++;
			if( ccam->type == DEMOCAM_PATH_SPLINE ) {
				pcam = NULL;
				sncam = CG_Democam_FindNext( ncam->timeStamp );
				if( ccam->timeStamp > 0 ) {
					pcam = CG_Democam_FindCurrent( ccam->timeStamp - 1 );
				}

				if( !pcam ) {
					VectorSubtract( ncam->origin, ccam->origin, ccam->tangent );
					VectorScale( ccam->tangent, 1.0 / 4.0, ccam->tangent );

					if( ncam->angles[1] - ccam->angles[1] > 180 ) {
						ncam->angles[1] -= 360;
					}
					if( ncam->angles[1] - ccam->angles[1] < -180 ) {
						ncam->angles[1] += 360;
					}

					if( ncam->angles[2] - ccam->angles[2] > 180 ) {
						ncam->angles[2] -= 360;
					}
					if( ncam->angles[2] - ccam->angles[2] < -180 ) {
						ncam->angles[2] += 360;
					}

					VectorSubtract( ncam->angles, ccam->angles, ccam->angles_tangent );
					VectorScale( ccam->angles_tangent, 1.0 / 4.0, ccam->angles_tangent );
				} else if( pcam ) {
					VectorSubtract( ncam->origin, pcam->origin, ccam->tangent );
					VectorScale( ccam->tangent, 1.0 / 4.0, ccam->tangent );

					if( pcam->angles[1] - ccam->angles[1] > 180 ) {
						pcam->angles[1] -= 360;
					}
					if( pcam->angles[1] - ccam->angles[1] < -180 ) {
						pcam->angles[1] += 360;
					}
					if( ncam->angles[1] - ccam->angles[1] > 180 ) {
						ncam->angles[1] -= 360;
					}
					if( ncam->angles[1] - ccam->angles[1] < -180 ) {
						ncam->angles[1] += 360;
					}

					if( pcam->angles[2] - ccam->angles[2] > 180 ) {
						pcam->angles[2] -= 360;
					}
					if( pcam->angles[2] - ccam->angles[2] < -180 ) {
						pcam->angles[2] += 360;
					}
					if( ncam->angles[2] - ccam->angles[2] > 180 ) {
						ncam->angles[2] -= 360;
					}
					if( ncam->angles[2] - ccam->angles[2] < -180 ) {
						ncam->angles[2] += 360;
					}

					VectorSubtract( ncam->angles, pcam->angles, ccam->angles_tangent );
					VectorScale( ccam->angles_tangent, 1.0 / 4.0, ccam->angles_tangent );
				}

				if( sncam ) {
					VectorSubtract( sncam->origin, ccam->origin, ncam->tangent );
					VectorScale( ncam->tangent, 1.0 / 4.0, ncam->tangent );

					if( ccam->angles[1] - ncam->angles[1] > 180 ) {
						ccam->angles[1] -= 360;
					}
					if( ccam->angles[1] - ncam->angles[1] < -180 ) {
						ccam->angles[1] += 360;
					}
					if( sncam->angles[1] - ncam->angles[1] > 180 ) {
						sncam->angles[1] -= 360;
					}
					if( sncam->angles[1] - ncam->angles[1] < -180 ) {
						sncam->angles[1] += 360;
					}

					if( ccam->angles[2] - ncam->angles[2] > 180 ) {
						ccam->angles[2] -= 360;
					}
					if( ccam->angles[2] - ncam->angles[2] < -180 ) {
						ccam->angles[2] += 360;
					}
					if( sncam->angles[2] - ncam->angles[2] > 180 ) {
						sncam->angles[2] -= 360;
					}
					if( sncam->angles[2] - ncam->angles[2] < -180 ) {
						sncam->angles[2] += 360;
					}

					VectorSubtract( sncam->angles, ccam->angles, ncam->angles_tangent );
					VectorScale( ncam->angles_tangent, 1.0 / 4.0, ncam->angles_tangent );
				} else if( !sncam ) {
					VectorSubtract( ncam->origin, ccam->origin, ncam->tangent );
					VectorScale( ncam->tangent, 1.0 / 4.0, ncam->tangent );

					if( ncam->angles[1] - ccam->angles[1] > 180 ) {
						ncam->angles[1] -= 360;
					}
					if( ncam->angles[1] - ccam->angles[1] < -180 ) {
						ncam->angles[1] += 360;
					}

					if( ncam->angles[2] - ccam->angles[2] > 180 ) {
						ncam->angles[2] -= 360;
					}
					if( ncam->angles[2] - ccam->angles[2] < -180 ) {
						ncam->angles[2] += 360;
					}

					VectorSubtract( ncam->angles, ccam->angles, ncam->angles_tangent );
					VectorScale( ncam->angles_tangent, 1.0 / 4.0, ncam->angles_tangent );
				}
			}
		}

		pathtime = ncam->timeStamp;
	}
}

/*
* CG_LoadRecamScriptFile
*/
bool CG_LoadRecamScriptFile( char *filename ) {
	int filelen, filehandle;
	uint8_t *buf = NULL;
	char *ptr, *token;
	int linecount;
	cg_democam_t *cam = NULL;

	if( !filename ) {
		Com_Printf( "CG_LoadRecamScriptFile: no filename\n" );
		return false;
	}

	filelen = FS_FOpenFile( filename, &filehandle, FS_READ );
	if( !filehandle || filelen < 1 ) {
		FS_FCloseFile( filehandle );
	} else {
		buf = ( uint8_t * )CG_Malloc( filelen + 1 );
		filelen = FS_Read( buf, filelen, filehandle );
		FS_FCloseFile( filehandle );
	}

	if( !buf ) {
		return false;
	}

	// parse the script
	linecount = 0;
	ptr = ( char * )buf;
	while( ptr ) {
		token = COM_ParseExt( &ptr, true );
		if( !token[0] ) {
			break;
		}

		if( !Q_stricmp( token, "subtitle" ) || !Q_stricmp( token, "print" ) ) {
			cg_subtitle_t *sub;

			sub = CG_Democam_RegisterSubtitle();
			sub->highprint = ( Q_stricmp( token, "print" ) == 0 );

			token = COM_ParseExt( &ptr, true );
			if( !token[0] ) {
				break;
			}
			sub->timeStamp = (unsigned int)atoi( token );
			token = COM_ParseExt( &ptr, true );
			if( !token[0] ) {
				break;
			}
			sub->maxDuration = (unsigned int)atoi( token );
			sub->text = CG_CopyString( COM_ParseExt( &ptr, true ) );

			linecount = 0;
		} else {
			switch( linecount ) {
				case 0:
					cam = CG_Democam_RegisterCam( atoi( token ) );
					break;
				case 1:
					cam->timeStamp = (unsigned int)atoi( token );
					break;
				case 2:
					cam->origin[0] = atof( token );
					break;
				case 3:
					cam->origin[1] = atof( token );
					break;
				case 4:
					cam->origin[2] = atof( token );
					break;
				case 5:
					cam->angles[0] = atof( token );
					break;
				case 6:
					cam->angles[1] = atof( token );
					break;
				case 7:
					cam->angles[2] = atof( token );
					break;
				case 8:
					cam->trackEnt = atoi( token );
					break;
				case 9:
					cam->fov = atoi( token );
					break;
				default:
					Com_Error( ERR_DROP, "CG_LoadRecamScriptFile: bad switch\n" );
			}

			linecount++;
			if( linecount == 10 ) {
				linecount = 0;
			}
		}
	}

	CG_Free( buf );
	if( linecount != 0 ) {
		Com_Printf( "CG_LoadRecamScriptFile: Invalid script. Ignored\n" );
		CG_Democam_FreeCams();
		CG_Democam_FreeSubtitles();
		return false;
	}

	CG_Democam_ExecutePathAnalysis();
	return true;
}

/*
* CG_SaveRecamScriptFile
*/
void CG_SaveRecamScriptFile( const char *filename ) {
	cg_democam_t *cam;
	cg_subtitle_t *sub;
	int filehandle;
	char str[256];

	if( !cg_cams_headnode && !cg_subs_headnode ) {
		Com_Printf( "CG_SaveRecamScriptFile: no cameras nor subtitles to save\n" );
		return;
	}

	if( !filename ) {
		filename = demoscriptname;
		if( !filename ) {
			return;
		}
	}

	if( FS_FOpenFile( filename, &filehandle, FS_WRITE ) == -1 ) {
		Com_Printf( "CG_SaveRecamScriptFile: Couldn't create the file %s\n", demoscriptname );
		return;
	}

	snprintf( str, sizeof( str ), "// cam script file generated by %s\n", Cvar_String( "gamename" ) );
	FS_Print( filehandle, str );

	snprintf( str, sizeof( str ), "// demo start time: %" PRIi64 "\n", demo_initial_timestamp );
	FS_Print( filehandle, str );

	cam = cg_cams_headnode;
	while( cam != NULL ) {
		snprintf( str, sizeof( str ), "%i %" PRIi64" %.2f %.2f %.2f %.2f %.2f %.2f %i %i\n",
					 cam->type,
					 cam->timeStamp,
					 cam->origin[0],
					 cam->origin[1],
					 cam->origin[2],
					 cam->angles[0],
					 cam->angles[1],
					 cam->angles[2],
					 cam->trackEnt,
					 cam->fov
					 );
		FS_Print( filehandle, str );
		cam = cam->next;
	}

	sub = cg_subs_headnode;
	while( sub != NULL ) {
		snprintf( str, sizeof( str ), "%s %" PRIi64 " %" PRIi64 " ",
					 sub->highprint ? "print" : "subtitle",
					 sub->timeStamp,
					 sub->maxDuration
					 );
		FS_Print( filehandle, str );
		FS_Print( filehandle, "\"" );
		FS_Print( filehandle, sub->text ? sub->text : "" );
		FS_Print( filehandle, "\"\n" );
		sub = sub->next;
	}

	FS_FCloseFile( filehandle );
	Com_Printf( "cam file saved\n" );
}

//===================================================================

/*
* CG_DrawEntityNumbers
*/
static void CG_DrawEntityNumbers( void ) {
	float zfar = 2048;
	int i, entnum;
	centity_t *cent;
	vec3_t dir;
	float dist;
	trace_t trace;
	vec3_t eorigin;
	int shadowOffset = max( 1, frame_static.viewport_height / 600 );

	for( i = 0; i < cg.frame.numEntities; i++ ) {
		entnum = cg.frame.parsedEntities[i & ( MAX_PARSE_ENTITIES - 1 )].number;
		if( entnum < 1 || entnum >= MAX_EDICTS ) {
			continue;
		}
		cent = &cg_entities[entnum];
		if( cent->serverFrame != cg.frame.serverFrame ) {
			continue;
		}

		if( cent->current.model == EMPTY_HASH ) {
			continue;
		}

		// Kill if behind the view
		VectorLerp( cent->prev.origin, cg.lerpfrac, cent->current.origin, eorigin );
		VectorSubtract( eorigin, cam_origin, dir );
		dist = VectorNormalize2( dir, dir ) * cg.view.fracDistFOV;
		if( dist > zfar ) {
			continue;
		}

		if( DotProduct( dir, &cg.view.axis[AXIS_FORWARD] ) < 0 ) {
			continue;
		}

		CG_Trace( &trace, cam_origin, vec3_origin, vec3_origin, eorigin, cent->current.number, MASK_OPAQUE );
		if( trace.fraction == 1.0f ) {
			Vec2 coords = WorldToScreen( FromQF3( eorigin ) );
			if( ( coords.x < 0 || coords.x > frame_static.viewport_width ) || ( coords.y < 0 || coords.y > frame_static.viewport_height ) ) {
				return;
			}

			// trap_SCR_DrawString( coords.x + shadowOffset, coords.y + shadowOffset,
			// 					 ALIGN_LEFT_MIDDLE, va( "%i", cent->current.number ), cgs.fontSystemSmall, colorBlack );
			// trap_SCR_DrawString( coords.x, coords.y,
			// 					 ALIGN_LEFT_MIDDLE, va( "%i", cent->current.number ), cgs.fontSystemSmall, colorWhite );
		}
	}
}

void CG_Democam_DrawCenterSubtitle( int y, unsigned int maxwidth, struct qfontface_s *font, char *text ) {
	char *ptr, *s, *t, c, d;
	int x = frame_static.viewport_width / 2;

	if( !text || !text[0] ) {
		return;
	}

	int shadowOffset = 2 * frame_static.viewport_height / 600;
	if( !shadowOffset ) {
		shadowOffset = 1;
	}

	// if( !maxwidth || trap_SCR_strWidth( text, font, 0 ) <= maxwidth ) {
	if( !maxwidth ) {
		// trap_SCR_DrawStringWidth( x + shadowOffset, y + shadowOffset, ALIGN_CENTER_TOP, COM_RemoveColorTokens( text ), maxwidth, font, colorBlack );
		// trap_SCR_DrawStringWidth( x, y, ALIGN_CENTER_TOP, text, maxwidth, font, colorWhite );
		return;
	}

	t = s = ptr = text;
	while( *s ) {
		while( *s && *s != ' ' && *s != '\n' )
			s++;

		// if( ( !*s || *s == '\n' ) && trap_SCR_strWidth( ptr, font, 0 ) < maxwidth ) { // new line or end of text, in both cases force write
		if( false ) { // new line or end of text, in both cases force write
			c = *s;
			*s = 0;
			// trap_SCR_DrawStringWidth( x + shadowOffset, y + shadowOffset, ALIGN_CENTER_TOP, COM_RemoveColorTokens( ptr ), maxwidth, font, colorBlack );
			// trap_SCR_DrawStringWidth( x, y, ALIGN_CENTER_TOP, ptr, maxwidth, font, colorWhite );
			*s = c;

			if( !*s ) {
				break;
			}

			t = s;
			s++;
			ptr = s;
		} else {
			c = *s;
			*s = 0;

			// if( trap_SCR_strWidth( ptr, font, 0 ) < maxwidth ) {
			// 	*s = c;
			// 	t = s;
			// 	s++;
			// 	continue;
			// }

			*s = c;
			d = *t;
			*t = 0;
			// trap_SCR_DrawStringWidth( x + shadowOffset, y + shadowOffset, ALIGN_CENTER_TOP, COM_RemoveColorTokens( ptr ), maxwidth, font, colorBlack );
			// trap_SCR_DrawStringWidth( x, y, ALIGN_CENTER_TOP, ptr, maxwidth, font, colorWhite );
			*t = d;
			s = t;
			s++;
			ptr = s;
		}

		// y += trap_SCR_FontHeight( font );
	}
}

/*
* CG_DrawDemocam2D
*/
void CG_DrawDemocam2D( void ) {
	int xpos, ypos;
	const char *cam_type_name;
	int64_t cam_timestamp;
	char sfov[8], strack[8];
	cg_subtitle_t *sub;

	if( !cgs.demoPlaying ) {
		return;
	}

	if( ( sub = CG_Democam_FindCurrentSubtitle() ) != NULL ) {
		if( sub->text && sub->text[0] ) {
			int y;

			if( sub->highprint ) {
				y = frame_static.viewport_height * 0.30f;
			} else {
				y = frame_static.viewport_height - ( frame_static.viewport_height * 0.30f );
			}

			// CG_Democam_DrawCenterSubtitle( y, frame_static.viewport_width * 0.75, cgs.fontSystemBig, sub->text );
		}
	}

	if( democam_editing_mode ) {
		// draw the numbers of every entity in the view
		CG_DrawEntityNumbers();

		// draw the cams info
		xpos = 8 * frame_static.viewport_height / 600;
		ypos = 100 * frame_static.viewport_height / 600;

		if( *cgs.demoName ) {
			// trap_SCR_DrawString( xpos, ypos, ALIGN_LEFT_TOP, va( "Demo: %s", cgs.demoName ), cgs.fontSystemSmall, colorWhite );
			// ypos += trap_SCR_FontHeight( cgs.fontSystemSmall );
		}

		// trap_SCR_DrawString( xpos, ypos, ALIGN_LEFT_TOP, va( "Play mode: %s%s%s", S_COLOR_ORANGE, CamIsFree ? "Free Fly" : "Preview", S_COLOR_WHITE ), cgs.fontSystemSmall, colorWhite );
		// ypos += trap_SCR_FontHeight( cgs.fontSystemSmall );

		// trap_SCR_DrawString( xpos, ypos, ALIGN_LEFT_TOP, va( "Time: %" PRIi64, demo_time ), cgs.fontSystemSmall, colorWhite );
		// ypos += trap_SCR_FontHeight( cgs.fontSystemSmall );

		cam_type_name = "none";
		cam_timestamp = 0;

		if( currentcam ) {
			cam_type_name = cam_TypeNames[currentcam->type];
			cam_timestamp = currentcam->timeStamp;
			snprintf( strack, sizeof( strack ), "%i", currentcam->trackEnt );
			snprintf( sfov, sizeof( sfov ), "%i", currentcam->fov );
		} else {
			Q_strncpyz( strack, "NO", sizeof( strack ) );
			Q_strncpyz( sfov, "NO", sizeof( sfov ) );
		}

		// trap_SCR_DrawString( xpos, ypos, ALIGN_LEFT_TOP, 
		// 	va( "Current cam: " S_COLOR_ORANGE "%s" S_COLOR_WHITE " Fov " S_COLOR_ORANGE "%s" S_COLOR_WHITE " Start %" PRIi64 " Tracking " S_COLOR_ORANGE "%s" S_COLOR_WHITE,
		// 													 cam_type_name, sfov, cam_timestamp, strack ),
		// 					 cgs.fontSystemSmall, colorWhite );
		// ypos += trap_SCR_FontHeight( cgs.fontSystemSmall );

		if( currentcam ) {
			// trap_SCR_DrawString( xpos, ypos, ALIGN_LEFT_TOP, 
			// 	va( "Pitch: " S_COLOR_ORANGE "%.2f" S_COLOR_WHITE " Yaw: " S_COLOR_ORANGE "%.2f" S_COLOR_WHITE " Roll: " S_COLOR_ORANGE "%.2f" S_COLOR_WHITE,
			// 													 currentcam->angles[PITCH], currentcam->angles[YAW], currentcam->angles[ROLL] ),
			// 					 cgs.fontSystemSmall, colorWhite );
		}
		// ypos += trap_SCR_FontHeight( cgs.fontSystemSmall );

		cam_type_name = "none";
		cam_timestamp = 0;
		Q_strncpyz( sfov, "NO", sizeof( sfov ) );
		if( nextcam ) {
			cam_type_name = cam_TypeNames[nextcam->type];
			cam_timestamp = nextcam->timeStamp;
			snprintf( strack, sizeof( strack ), "%i", nextcam->trackEnt );
			snprintf( sfov, sizeof( sfov ), "%i", nextcam->fov );
		} else {
			Q_strncpyz( strack, "NO", sizeof( strack ) );
			Q_strncpyz( sfov, "NO", sizeof( sfov ) );
		}

		// trap_SCR_DrawString( xpos, ypos, ALIGN_LEFT_TOP, 
		// 	va( "Next cam: " S_COLOR_ORANGE "%s" S_COLOR_WHITE " Fov " S_COLOR_ORANGE "%s" S_COLOR_WHITE " Start %" PRIi64 " Tracking " S_COLOR_ORANGE "%s" S_COLOR_WHITE,
		// 													 cam_type_name, sfov, cam_timestamp, strack ),
		// 					 cgs.fontSystemSmall, colorWhite );
		// ypos += trap_SCR_FontHeight( cgs.fontSystemSmall );

		if( nextcam ) {
			// trap_SCR_DrawString( xpos, ypos, ALIGN_LEFT_TOP, 
			// 	va( "Pitch: " S_COLOR_ORANGE "%.2f" S_COLOR_WHITE " Yaw: " S_COLOR_ORANGE "%.2f" S_COLOR_WHITE " Roll: " S_COLOR_ORANGE "%.2f" S_COLOR_WHITE,
			// 													 nextcam->angles[PITCH], nextcam->angles[YAW], nextcam->angles[ROLL] ),
			// 					 cgs.fontSystemSmall, colorWhite );
		}
		// ypos += trap_SCR_FontHeight( cgs.fontSystemSmall );
	}
}

//===================================================================

/*
* CG_DemoCam_LookAt
*/
bool CG_DemoCam_LookAt( int trackEnt, vec3_t vieworg, vec3_t viewangles ) {
	centity_t *cent;
	vec3_t dir;
	vec3_t origin;
	struct cmodel_s *cmodel;
	int i;

	if( trackEnt < 1 || trackEnt >= MAX_EDICTS ) {
		return false;
	}

	cent = &cg_entities[trackEnt];
	if( cent->serverFrame != cg.frame.serverFrame ) {
		return false;
	}

	// seems to be valid. Find the angles to look at this entity
	VectorLerp( cent->prev.origin, cg.lerpfrac, cent->current.origin, origin );

	// if having a bounding box, look to its center
	if( ( cmodel = CG_CModelForEntity( trackEnt ) ) != NULL ) {
		vec3_t mins, maxs;
		CM_InlineModelBounds( cl.cms, cmodel, mins, maxs );
		for( i = 0; i < 3; i++ )
			origin[i] += ( mins[i] + maxs[i] );
	}

	VectorSubtract( origin, vieworg, dir );
	VectorNormalize( dir );
	VecToAngles( dir, viewangles );
	return true;
}

/*
* CG_DemoCam_GetViewType
*/
int CG_DemoCam_GetViewType( void ) {
	return cam_viewtype;
}

/*
* CG_DemoCam_GetThirdPerson
*/
bool CG_DemoCam_GetThirdPerson( void ) {
	if( !currentcam ) {
		return ( chaseCam.mode == CAM_THIRDPERSON );
	}
	return ( cam_viewtype == VIEWDEF_PLAYERVIEW && cam_3dPerson );
}

/*
* CG_DemoCam_GetViewDef
*/
void CG_DemoCam_GetViewDef( cg_viewdef_t *view ) {
	view->POVent = cam_POVent;
	view->thirdperson = cam_3dPerson;
	view->playerPrediction = false;
	view->drawWeapon = false;
	view->draw2D = false;
}

/*
* CG_DemoCam_GetOrientation
*/
float CG_DemoCam_GetOrientation( vec3_t origin, vec3_t angles, vec3_t velocity ) {
	VectorCopy( cam_angles, angles );
	VectorCopy( cam_origin, origin );
	VectorCopy( cam_velocity, velocity );

	if( !currentcam || !currentcam->fov ) {
		return bound( MIN_FOV, cg_fov->value, MAX_FOV );
	}

	return cam_fov;
}

static short freecam_delta_angles[3];

/*
* CG_DemoCam_FreeFly
*/
int CG_DemoCam_FreeFly( void ) {
	usercmd_t cmd;
	const float SPEED = 500;

	if( cgs.demoPlaying && CamIsFree ) {
		vec3_t wishvel, wishdir, forward, right, up, moveangles;
		float fmove, smove, upmove, wishspeed, maxspeed;
		int i;

		maxspeed = 250;

		// run frame
		trap_NET_GetUserCmd( trap_NET_GetCurrentUserCmdNum() - 1, &cmd );
		cmd.msec = cls.realFrameTime;

		for( i = 0; i < 3; i++ )
			moveangles[i] = SHORT2ANGLE( cmd.angles[i] ) + SHORT2ANGLE( freecam_delta_angles[i] );

		AngleVectors( moveangles, forward, right, up );
		VectorCopy( moveangles, cam_angles );

		fmove = cmd.forwardmove * SPEED / 127.0f;
		smove = cmd.sidemove * SPEED / 127.0f;
		upmove = cmd.upmove * SPEED / 127.0f;
		if( cmd.buttons & BUTTON_SPECIAL ) {
			maxspeed *= 2;
		}

		for( i = 0; i < 3; i++ )
			wishvel[i] = forward[i] * fmove + right[i] * smove;
		wishvel[2] += upmove;

		wishspeed = VectorNormalize2( wishvel, wishdir );
		if( wishspeed > maxspeed ) {
			wishspeed = maxspeed / wishspeed;
			VectorScale( wishvel, wishspeed, wishvel );
			wishspeed = maxspeed;
		}

		VectorMA( cam_origin, (float)cls.realFrameTime * 0.001f, wishvel, cam_origin );

		cam_POVent = 0;
		cam_3dPerson = false;
		return VIEWDEF_DEMOCAM;
	}

	return VIEWDEF_PLAYERVIEW;
}

static void CG_Democam_SetCameraPositionFromView( void ) {
	if( cg.view.type == VIEWDEF_PLAYERVIEW ) {
		VectorCopy( cg.view.origin, cam_origin );
		VectorCopy( cg.view.angles, cam_angles );
		VectorCopy( cg.view.velocity, cam_velocity );
		cam_fov = cg.view.fov_y;
		cam_orbital_radius = 0;
	}

	if( !CamIsFree ) {
		int i;
		usercmd_t cmd;

		trap_NET_GetUserCmd( trap_NET_GetCurrentUserCmdNum() - 1, &cmd );

		for( i = 0; i < 3; i++ )
			freecam_delta_angles[i] = ANGLE2SHORT( cam_angles[i] ) - cmd.angles[i];
	} else {
		cam_orbital_radius = 0;
	}
}

/*
* CG_Democam_CalcView
*/
static int CG_Democam_CalcView( void ) {
	int i, viewType;
	float lerpfrac;
	vec3_t v;

	viewType = VIEWDEF_PLAYERVIEW;
	VectorClear( cam_velocity );

	if( currentcam ) {
		if( !nextcam ) {
			lerpfrac = 0;
		} else {
			lerpfrac = (float)( demo_time - currentcam->timeStamp ) / (float)( nextcam->timeStamp - currentcam->timeStamp );
		}

		switch( currentcam->type ) {
			case DEMOCAM_FIRSTPERSON:
				VectorCopy( cg.view.origin, cam_origin );
				VectorCopy( cg.view.angles, cam_angles );
				VectorCopy( cg.view.velocity, cam_velocity );
				cam_fov = cg.view.fov_y;
				break;

			case DEMOCAM_THIRDPERSON:
				VectorCopy( cg.view.origin, cam_origin );
				VectorCopy( cg.view.angles, cam_angles );
				VectorCopy( cg.view.velocity, cam_velocity );
				cam_fov = cg.view.fov_y;
				cam_3dPerson = true;
				break;

			case DEMOCAM_POSITIONAL:
				viewType = VIEWDEF_DEMOCAM;
				cam_POVent = 0;
				VectorCopy( currentcam->origin, cam_origin );
				if( !CG_DemoCam_LookAt( currentcam->trackEnt, cam_origin, cam_angles ) ) {
					VectorCopy( currentcam->angles, cam_angles );
				}
				cam_fov = currentcam->fov;
				break;

			case DEMOCAM_PATH_LINEAR:
				viewType = VIEWDEF_DEMOCAM;
				cam_POVent = 0;
				VectorCopy( cam_origin, v );

				if( !nextcam || nextcam->type == DEMOCAM_FIRSTPERSON || nextcam->type == DEMOCAM_THIRDPERSON ) {
					Com_Printf( "Warning: CG_DemoCam: path_linear cam without a valid next cam\n" );
					VectorCopy( currentcam->origin, cam_origin );
					if( !CG_DemoCam_LookAt( currentcam->trackEnt, cam_origin, cam_angles ) ) {
						VectorCopy( currentcam->angles, cam_angles );
					}
					cam_fov = currentcam->fov;
				} else {
					VectorLerp( currentcam->origin, lerpfrac, nextcam->origin, cam_origin );
					if( !CG_DemoCam_LookAt( currentcam->trackEnt, cam_origin, cam_angles ) ) {
						for( i = 0; i < 3; i++ ) cam_angles[i] = LerpAngle( currentcam->angles[i], nextcam->angles[i], lerpfrac );
					}
					cam_fov = (float)currentcam->fov + (float)( nextcam->fov - currentcam->fov ) * lerpfrac;
				}

				// set velocity
				VectorSubtract( cam_origin, v, cam_velocity );
				break;

			case DEMOCAM_PATH_SPLINE:
				viewType = VIEWDEF_DEMOCAM;
				cam_POVent = 0;
				lerpfrac = Clamp01( lerpfrac );
				VectorCopy( cam_origin, v );

				if( !nextcam || nextcam->type == DEMOCAM_FIRSTPERSON || nextcam->type == DEMOCAM_THIRDPERSON ) {
					Com_Printf( "Warning: CG_DemoCam: path_spline cam without a valid next cam\n" );
					VectorCopy( currentcam->origin, cam_origin );
					if( !CG_DemoCam_LookAt( currentcam->trackEnt, cam_origin, cam_angles ) ) {
						VectorCopy( currentcam->angles, cam_angles );
					}
					cam_fov = currentcam->fov;
				} else {  // valid spline path
#define VectorHermiteInterp( a, at, b, bt, c, v )  ( ( v )[0] = ( 2 * powf( c, 3 ) - 3 * powf( c, 2 ) + 1 ) * a[0] + ( powf( c, 3 ) - 2 * powf( c, 2 ) + c ) * 2 * at[0] + ( -2 * powf( c, 3 ) + 3 * powf( c, 2 ) ) * b[0] + ( powf( c, 3 ) - powf( c, 2 ) ) * 2 * bt[0], ( v )[1] = ( 2 * powf( c, 3 ) - 3 * powf( c, 2 ) + 1 ) * a[1] + ( powf( c, 3 ) - 2 * powf( c, 2 ) + c ) * 2 * at[1] + ( -2 * powf( c, 3 ) + 3 * powf( c, 2 ) ) * b[1] + ( powf( c, 3 ) - powf( c, 2 ) ) * 2 * bt[1], ( v )[2] = ( 2 * powf( c, 3 ) - 3 * powf( c, 2 ) + 1 ) * a[2] + ( powf( c, 3 ) - 2 * powf( c, 2 ) + c ) * 2 * at[2] + ( -2 * powf( c, 3 ) + 3 * powf( c, 2 ) ) * b[2] + ( powf( c, 3 ) - powf( c, 2 ) ) * 2 * bt[2] )

					float lerpspline, A, B, C, n1, n2, n3;
					cg_democam_t *previouscam = NULL;
					cg_democam_t *secondnextcam = NULL;

					if( nextcam ) {
						secondnextcam = CG_Democam_FindNext( nextcam->timeStamp );
					}
					if( currentcam->timeStamp > 0 ) {
						previouscam = CG_Democam_FindCurrent( currentcam->timeStamp - 1 );
					}

					if( !previouscam && nextcam && !secondnextcam ) {
						lerpfrac = (float)( demo_time - currentcam->timeStamp ) / (float)( nextcam->timeStamp - currentcam->timeStamp );
						lerpspline = lerpfrac;
					} else if( !previouscam && nextcam && secondnextcam ) {
						n1 = nextcam->timeStamp - currentcam->timeStamp;
						n2 = secondnextcam->timeStamp - nextcam->timeStamp;
						A = n1 * ( n1 - n2 ) / ( powf( n1, 2 ) + n1 * n2 - n1 - n2 );
						B = ( 2 * n1 * n2 - n1 - n2 ) / ( powf( n1, 2 ) + n1 * n2 - n1 - n2 );
						lerpfrac = (float)( demo_time - currentcam->timeStamp ) / (float)( nextcam->timeStamp - currentcam->timeStamp );
						lerpspline = A * powf( lerpfrac, 2 ) + B * lerpfrac;
					} else if( previouscam && nextcam && !secondnextcam ) {
						n2 = currentcam->timeStamp - previouscam->timeStamp;
						n3 = nextcam->timeStamp - currentcam->timeStamp;
						A = n3 * ( n2 - n3 ) / ( -n2 - n3 + n2 * n3 + powf( n3, 2 ) );
						B = -1 / ( -n2 - n3 + n2 * n3 + powf( n3, 2 ) ) * ( n2 + n3 - 2 * powf( n3, 2 ) );
						lerpfrac = (float)( demo_time - currentcam->timeStamp ) / (float)( nextcam->timeStamp - currentcam->timeStamp );
						lerpspline = A * powf( lerpfrac, 2 ) + B * lerpfrac;
					} else if( previouscam && nextcam && secondnextcam ) {
						n1 = currentcam->timeStamp - previouscam->timeStamp;
						n2 = nextcam->timeStamp - currentcam->timeStamp;
						n3 = secondnextcam->timeStamp - nextcam->timeStamp;
						A = -2 * powf( n2, 2 ) * ( -powf( n2, 2 ) + n1 * n3 ) / ( 2 * n2 * n3 + powf( n2, 3 ) * n3 - 3 * powf( n2, 2 ) * n1 + n1 * powf( n2, 3 ) + 2 * n1 * n2 - 3 * powf( n2, 2 ) * n3 - 3 * powf( n2, 3 ) + 2 * powf( n2, 2 ) + powf( n2, 4 ) + n1 * powf( n2, 2 ) * n3 - 3 * n1 * n2 * n3 + 2 * n1 * n3 );
						B = powf( n2, 2 ) * ( -2 * n1 - 3 * powf( n2, 2 ) - n2 * n3 + 2 * n3 + 3 * n1 * n3 + n1 * n2 ) / ( 2 * n2 * n3 + powf( n2, 3 ) * n3 - 3 * powf( n2, 2 ) * n1 + n1 * powf( n2, 3 ) + 2 * n1 * n2 - 3 * powf( n2, 2 ) * n3 - 3 * powf( n2, 3 ) + 2 * powf( n2, 2 ) + powf( n2, 4 ) + n1 * powf( n2, 2 ) * n3 - 3 * n1 * n2 * n3 + 2 * n1 * n3 );
						C = -( powf( n2, 2 ) * n1 - 2 * n1 * n2 + 3 * n1 * n2 * n3 - 2 * n1 * n3 - 2 * powf( n2, 4 ) + 3 * powf( n2, 3 ) - 2 * powf( n2, 3 ) * n3 + 5 * powf( n2, 2 ) * n3 - 2 * powf( n2, 2 ) - 2 * n2 * n3 ) / ( 2 * n2 * n3 + powf( n2, 3 ) * n3 - 3 * powf( n2, 2 ) * n1 + n1 * powf( n2, 3 ) + 2 * n1 * n2 - 3 * powf( n2, 2 ) * n3 - 3 * powf( n2, 3 ) + 2 * powf( n2, 2 ) + powf( n2, 4 ) + n1 * powf( n2, 2 ) * n3 - 3 * n1 * n2 * n3 + 2 * n1 * n3 );
						lerpfrac = (float)( demo_time - currentcam->timeStamp ) / (float)( nextcam->timeStamp - currentcam->timeStamp );
						lerpspline = A * powf( lerpfrac, 3 ) + B * powf( lerpfrac, 2 ) + C * lerpfrac;
					} else {
						lerpfrac = 0;
						lerpspline = 0;
					}


					VectorHermiteInterp( currentcam->origin, currentcam->tangent, nextcam->origin, nextcam->tangent, lerpspline, cam_origin );
					if( !CG_DemoCam_LookAt( currentcam->trackEnt, cam_origin, cam_angles ) ) {
						VectorHermiteInterp( currentcam->angles, currentcam->angles_tangent, nextcam->angles, nextcam->angles_tangent, lerpspline, cam_angles );
					}
					cam_fov = (float)currentcam->fov + (float)( nextcam->fov - currentcam->fov ) * lerpfrac;
#undef VectorHermiteInterp
				}

				// set velocity
				VectorSubtract( cam_origin, v, cam_velocity );
				break;

			case DEMOCAM_ORBITAL:
				viewType = VIEWDEF_DEMOCAM;
				cam_POVent = 0;
				cam_fov = currentcam->fov;
				VectorCopy( cam_origin, v );

				if( !currentcam->trackEnt || currentcam->trackEnt >= MAX_EDICTS ) {
					Com_Printf( "Warning: CG_DemoCam: orbital cam needs a track entity set\n" );
					VectorCopy( currentcam->origin, cam_origin );
					VectorClear( cam_angles );
					VectorClear( cam_velocity );
				} else {
					vec3_t center, forward;
					struct cmodel_s *cmodel;
					const float ft = (float)cls.frametime * 0.001f;

					// find the trackEnt origin
					VectorLerp( cg_entities[currentcam->trackEnt].prev.origin, cg.lerpfrac, cg_entities[currentcam->trackEnt].current.origin, center );

					// if having a bounding box, look to its center
					if( ( cmodel = CG_CModelForEntity( currentcam->trackEnt ) ) != NULL ) {
						vec3_t mins, maxs;
						CM_InlineModelBounds( cl.cms, cmodel, mins, maxs );
						for( i = 0; i < 3; i++ )
							center[i] += ( mins[i] + maxs[i] );
					}

					if( !cam_orbital_radius ) {
						// cam is just started, find distance from cam to trackEnt and keep it as radius
						VectorSubtract( currentcam->origin, center, forward );
						cam_orbital_radius = VectorNormalize( forward );
						VecToAngles( forward, cam_orbital_angles );
					}

					for( i = 0; i < 3; i++ ) {
						cam_orbital_angles[i] += currentcam->angles[i] * ft;
						cam_orbital_angles[i] = AngleNormalize360( cam_orbital_angles[i] );
					}

					AngleVectors( cam_orbital_angles, forward, NULL, NULL );
					VectorMA( center, cam_orbital_radius, forward, cam_origin );

					// lookat
					VectorInverse( forward );
					VecToAngles( forward, cam_angles );
				}

				// set velocity
				VectorSubtract( cam_origin, v, cam_velocity );
				break;

			default:
				break;
		}

		if( currentcam->type != DEMOCAM_ORBITAL ) {
			VectorClear( cam_orbital_angles );
			cam_orbital_radius = 0;
		}
	}

	return viewType;
}

/*
* CG_DemoCam_Update
*/
bool CG_DemoCam_Update( void ) {
	if( !cgs.demoPlaying ) {
		return false;
	}

	if( !demo_initial_timestamp && cg.frame.valid ) {
		demo_initial_timestamp = cl.serverTime;
	}

	CG_DemoCam_UpdateDemoTime();

	// see if we have any cams to be played
	currentcam = CG_Democam_FindCurrent( demo_time );
	nextcam = CG_Democam_FindNext( demo_time );

	cam_3dPerson = false;
	cam_viewtype = VIEWDEF_PLAYERVIEW;
	cam_POVent = cg.frame.playerState.POVnum;

	if( CamIsFree ) {
		cam_viewtype = CG_DemoCam_FreeFly();
	} else if( currentcam ) {
		cam_viewtype = CG_Democam_CalcView();
	}

	CG_Democam_SetCameraPositionFromView();

	return true;
}

/*
* CG_DemoCam_IsFree
*/
bool CG_DemoCam_IsFree( void ) {
	return CamIsFree;
}

/*
* CG_DemoFreeFly_Cmd_f
*/
static void CG_DemoFreeFly_Cmd_f( void ) {
	if( Cmd_Argc() > 1 ) {
		if( !Q_stricmp( Cmd_Argv( 1 ), "on" ) ) {
			CamIsFree = true;
		} else if( !Q_stricmp( Cmd_Argv( 1 ), "off" ) ) {
			CamIsFree = false;
		}
	} else {
		CamIsFree = !CamIsFree;
	}

	VectorClear( cam_velocity );
	Com_Printf( "demo cam mode %s\n", CamIsFree ? "Free Fly" : "Preview" );
}

/*
* CG_CamSwitch_Cmd_f
*/
static void CG_CamSwitch_Cmd_f( void ) {

}

/*
* CG_AddCam_Sub_f
*/
static void CG_AddSub_Cmd_f( void ) {
	cg_subtitle_t *sub;

	sub = CG_Democam_RegisterSubtitle();
	if( !sub ) {
		Com_Printf( "DemoCam Error: Failed to allocate the subtitle\n" );
		return;
	}

	if( Cmd_Argc() > 1 ) {
		char str[MAX_STRING_CHARS]; // one line of the console can't handle more than this
		int i;

		str[0] = 0;
		for( i = 1; i < Cmd_Argc(); i++ ) {
			Q_strncatz( str, Cmd_Argv( i ), sizeof( str ) );
			if( i < Cmd_Argc() - 1 ) {
				Q_strncatz( str, " ", sizeof( str ) );
			}
		}

		sub->text = CG_CopyString( str );
	} else {
		sub->text = CG_CopyString( "" );
	}
}

/*
* CG_AddPrint_Cmd_f
*/
static void CG_AddPrint_Cmd_f( void ) {
	cg_subtitle_t *sub;

	sub = CG_Democam_RegisterSubtitle();
	if( !sub ) {
		Com_Printf( "DemoCam Error: Failed to allocate the subtitle\n" );
		return;
	}

	if( Cmd_Argc() > 1 ) {
		char str[MAX_STRING_CHARS]; // one line of the console can't handle more than this
		int i;

		str[0] = 0;
		for( i = 1; i < Cmd_Argc(); i++ ) {
			Q_strncatz( str, Cmd_Argv( i ), sizeof( str ) );
			if( i < Cmd_Argc() - 1 ) {
				Q_strncatz( str, " ", sizeof( str ) );
			}
		}

		sub->text = CG_CopyString( str );
	} else {
		sub->text = CG_CopyString( "" );
	}

	sub->highprint = true;
}

/*
* CG_AddCam_Cmd_f
*/
static void CG_AddCam_Cmd_f( void ) {
	int type, i;

	CG_DemoCam_UpdateDemoTime();

	if( Cmd_Argc() == 2 ) {
		// type
		type = -1;
		for( i = 0; cam_TypeNames[i] != NULL; i++ ) {
			if( !Q_stricmp( cam_TypeNames[i], Cmd_Argv( 1 ) ) ) {
				type = i;
				break;
			}
		}

		if( type != -1 ) {
			// valid. Register and return
			if( CG_Democam_RegisterCam( type ) != NULL ) {
				Com_Printf( "cam added\n" );

				// update current cam
				CG_Democam_ExecutePathAnalysis();
				currentcam = CG_Democam_FindCurrent( demo_time );
				nextcam = CG_Democam_FindNext( demo_time );
				return;
			}
		}
	}

	// print help
	Com_Printf( " : Usage: AddCam <type>\n" );
	Com_Printf( " : Available types:\n" );
	for( i = 0; cam_TypeNames[i] != NULL; i++ )
		Com_Printf( " : %s\n", cam_TypeNames[i] );
}

/*
* CG_DeleteCam_Cmd_f
*/
static void CG_DeleteCam_Cmd_f( void ) {
	if( !currentcam ) {
		Com_Printf( "DeleteCam: No current cam to delete\n" );
		return;
	}

	CG_DemoCam_UpdateDemoTime();
	currentcam = CG_Democam_FindCurrent( demo_time );

	CG_Democam_UnregisterCam( currentcam );

	// update pointer to new current cam
	CG_Democam_ExecutePathAnalysis();
	currentcam = CG_Democam_FindCurrent( demo_time );
	nextcam = CG_Democam_FindNext( demo_time );
	Com_Printf( "cam deleted\n" );
}

/*
* CG_EditCam_Cmd_f
*/
static void CG_EditCam_Cmd_f( void ) {
	CG_DemoCam_UpdateDemoTime();

	currentcam = CG_Democam_FindCurrent( demo_time );
	if( !currentcam ) {
		Com_Printf( "Editcam: no current cam\n" );
		return;
	}

	if( Cmd_Argc() >= 2 && Q_stricmp( Cmd_Argv( 1 ), "help" ) ) {
		if( !Q_stricmp( Cmd_Argv( 1 ), "type" ) ) {
			int type, i;
			if( Cmd_Argc() < 3 ) { // not enough parameters, print help
				Com_Printf( "Usage: EditCam type <type name>\n" );
				return;
			}

			// type
			type = -1;
			for( i = 0; cam_TypeNames[i] != NULL; i++ ) {
				if( !Q_stricmp( cam_TypeNames[i], Cmd_Argv( 2 ) ) ) {
					type = i;
					break;
				}
			}

			if( type != -1 ) {
				// valid. Register and return
				currentcam->type = type;
				Com_Printf( "cam edited\n" );
				CG_Democam_ExecutePathAnalysis();
				return;
			} else {
				Com_Printf( "invalid type name\n" );
			}
		}
		if( !Q_stricmp( Cmd_Argv( 1 ), "track" ) ) {
			if( Cmd_Argc() < 3 ) {
				// not enough parameters, print help
				Com_Printf( "Usage: EditCam track <entity number> ( 0 for no tracking )\n" );
				return;
			}
			currentcam->trackEnt = atoi( Cmd_Argv( 2 ) );
			Com_Printf( "cam edited\n" );
			CG_Democam_ExecutePathAnalysis();
			return;
		} else if( !Q_stricmp( Cmd_Argv( 1 ), "fov" ) ) {
			if( Cmd_Argc() < 3 ) {
				// not enough parameters, print help
				Com_Printf( "Usage: EditCam fov <value>\n" );
				return;
			}
			currentcam->fov = atoi( Cmd_Argv( 2 ) );
			Com_Printf( "cam edited\n" );
			CG_Democam_ExecutePathAnalysis();
			return;
		} else if( !Q_stricmp( Cmd_Argv( 1 ), "timeOffset" ) ) {
			int64_t newtimestamp;
			if( Cmd_Argc() < 3 ) {
				// not enough parameters, print help
				Com_Printf( "Usage: EditCam timeOffset <value>\n" );
				return;
			}
			newtimestamp = currentcam->timeStamp += atoi( Cmd_Argv( 2 ) );
			if( newtimestamp + cl.serverTime <= demo_initial_timestamp ) {
				newtimestamp = 1;
			}
			currentcam->timeStamp = newtimestamp;
			currentcam = CG_Democam_FindCurrent( demo_time );
			nextcam = CG_Democam_FindNext( demo_time );
			Com_Printf( "cam edited\n" );
			CG_Democam_ExecutePathAnalysis();
			return;
		} else if( !Q_stricmp( Cmd_Argv( 1 ), "origin" ) ) {
			VectorCopy( cg.view.origin, currentcam->origin );
			cam_orbital_radius = 0;
			Com_Printf( "cam edited\n" );
			CG_Democam_ExecutePathAnalysis();
			return;
		} else if( !Q_stricmp( Cmd_Argv( 1 ), "angles" ) ) {
			VectorCopy( cg.view.angles, currentcam->angles );
			Com_Printf( "cam edited\n" );
			CG_Democam_ExecutePathAnalysis();
			return;
		} else if( !Q_stricmp( Cmd_Argv( 1 ), "pitch" ) ) {
			if( Cmd_Argc() < 3 ) {
				// not enough parameters, print help
				Com_Printf( "Usage: EditCam pitch <value>\n" );
				return;
			}
			currentcam->angles[PITCH] = atof( Cmd_Argv( 2 ) );
			Com_Printf( "cam edited\n" );
			CG_Democam_ExecutePathAnalysis();
			return;
		} else if( !Q_stricmp( Cmd_Argv( 1 ), "yaw" ) ) {
			if( Cmd_Argc() < 3 ) {
				// not enough parameters, print help
				Com_Printf( "Usage: EditCam yaw <value>\n" );
				return;
			}
			currentcam->angles[YAW] = atof( Cmd_Argv( 2 ) );
			Com_Printf( "cam edited\n" );
			CG_Democam_ExecutePathAnalysis();
			return;
		} else if( !Q_stricmp( Cmd_Argv( 1 ), "roll" ) ) {
			if( Cmd_Argc() < 3 ) {
				// not enough parameters, print help
				Com_Printf( "Usage: EditCam roll <value>\n" );
				return;
			}
			currentcam->angles[ROLL] = atof( Cmd_Argv( 2 ) );
			Com_Printf( "cam edited\n" );
			CG_Democam_ExecutePathAnalysis();
			return;
		}
	}

	// print help
	Com_Printf( " : Usage: EditCam <command>\n" );
	Com_Printf( " : Available commands:\n" );
	Com_Printf( " : type <type name>\n" );
	Com_Printf( " : track <entity number> ( 0 for no track )\n" );
	Com_Printf( " : fov <value> ( only for not player views )\n" );
	Com_Printf( " : timeOffset <value> ( + or - milliseconds to be added to camera timestamp )\n" );
	Com_Printf( " : origin ( changes cam to current origin )\n" );
	Com_Printf( " : angles ( changes cam to current angles )\n" );
	Com_Printf( " : pitch <value> ( assigns pitch angle to current cam )\n" );
	Com_Printf( " : yaw <value> ( assigns yaw angle to current cam )\n" );
	Com_Printf( " : roll <value> ( assigns roll angle to current cam )\n" );
}

/*
* CG_SaveCam_Cmd_f
*/
void CG_SaveCam_Cmd_f( void ) {
	if( !cgs.demoPlaying ) {
		return;
	}
	if( Cmd_Argc() > 1 ) {
		char *customName;
		int custom_name_size;

		custom_name_size = sizeof( char ) * ( strlen( "demos/" ) + strlen( Cmd_Argv( 1 ) ) + strlen( ".cam" ) + 1 );
		customName = ( char * )CG_Malloc( custom_name_size );
		snprintf( customName, custom_name_size, "demos/%s", Cmd_Argv( 1 ) );
		COM_ReplaceExtension( customName, ".cam", custom_name_size );
		CG_SaveRecamScriptFile( customName );
		CG_Free( customName );
		return;
	}

	CG_SaveRecamScriptFile( demoscriptname );
}

/*
* CG_Democam_ImportCams_f
*/
void CG_Democam_ImportCams_f( void ) {
	int name_size;
	char *customName;

	if( Cmd_Argc() < 2 ) {
		Com_Printf( "Usage: importcams <filename> (relative to demos directory)\n" );
		return;
	}

	// see if there is any script for this demo, and load it
	name_size = sizeof( char ) * ( strlen( "demos/" ) + strlen( Cmd_Argv( 1 ) ) + strlen( ".cam" ) + 1 );
	customName = ( char * )CG_Malloc( name_size );
	snprintf( customName, name_size, "demos/%s", Cmd_Argv( 1 ) );
	COM_ReplaceExtension( customName, ".cam", name_size );
	if( CG_LoadRecamScriptFile( customName ) ) {
		Com_Printf( "cam script imported\n" );
	} else {
		Com_Printf( "CG_Democam_ImportCams_f: no valid file found\n" );
	}
}

/*
* CG_DemoEditMode_RemoveCmds
*/
void CG_DemoEditMode_RemoveCmds( void ) {
	Cmd_RemoveCommand( "addcam" );
	Cmd_RemoveCommand( "deletecam" );
	Cmd_RemoveCommand( "editcam" );
	Cmd_RemoveCommand( "saverecam" );
	Cmd_RemoveCommand( "clearcams" );
	Cmd_RemoveCommand( "importcams" );
	Cmd_RemoveCommand( "subtitle" );
	Cmd_RemoveCommand( "addprint" );
}

/*
* CG_DemoEditMode_Cmd_f
*/
static void CG_DemoEditMode_Cmd_f( void ) {
	if( !cgs.demoPlaying ) {
		return;
	}

	if( Cmd_Argc() > 1 ) {
		if( !Q_stricmp( Cmd_Argv( 1 ), "on" ) ) {
			democam_editing_mode = true;
		} else if( !Q_stricmp( Cmd_Argv( 1 ), "off" ) ) {
			democam_editing_mode = false;
		}
	} else {
		democam_editing_mode = !democam_editing_mode;
	}

	Com_Printf( "demo cam editing mode %s\n", democam_editing_mode ? "on" : "off" );
	if( democam_editing_mode ) {
		Cmd_AddCommand( "addcam", CG_AddCam_Cmd_f );
		Cmd_AddCommand( "deletecam", CG_DeleteCam_Cmd_f );
		Cmd_AddCommand( "editcam", CG_EditCam_Cmd_f );
		Cmd_AddCommand( "saverecam", CG_SaveCam_Cmd_f );
		Cmd_AddCommand( "clearcams", CG_Democam_FreeCams );
		Cmd_AddCommand( "importcams", CG_Democam_ImportCams_f );
		Cmd_AddCommand( "subtitle", CG_AddSub_Cmd_f );
		Cmd_AddCommand( "addprint", CG_AddPrint_Cmd_f );
	} else {
		CG_DemoEditMode_RemoveCmds();
	}
}

/*
* CG_DemocamInit
*/
void CG_DemocamInit( void ) {
	int name_size;

	democam_editing_mode = false;
	demo_time = 0;
	demo_initial_timestamp = 0;

	if( !cgs.demoPlaying ) {
		return;
	}

	if( !*cgs.demoName ) {
		Com_Error( ERR_DROP, "CG_DemocamInit: no demo name string\n" );
	}

	// see if there is any script for this demo, and load it
	name_size = sizeof( char ) * ( strlen( cgs.demoName ) + strlen( ".cam" ) + 1 );
	demoscriptname = ( char * )CG_Malloc( name_size );
	snprintf( demoscriptname, name_size, "%s", cgs.demoName );
	COM_ReplaceExtension( demoscriptname, ".cam", name_size );

	Com_Printf( "cam: %s\n", demoscriptname );

	// add console commands
	Cmd_AddCommand( "demoEditMode", CG_DemoEditMode_Cmd_f );
	Cmd_AddCommand( "demoFreeFly", CG_DemoFreeFly_Cmd_f );
	Cmd_AddCommand( "camswitch", CG_CamSwitch_Cmd_f );

	if( CG_LoadRecamScriptFile( demoscriptname ) ) {
		Com_Printf( "Loaded demo cam script\n" );
	}
}

/*
* CG_DemocamShutdown
*/
void CG_DemocamShutdown( void ) {
	if( !cgs.demoPlaying ) {
		return;
	}

	// remove console commands
	Cmd_RemoveCommand( "demoEditMode" );
	Cmd_RemoveCommand( "demoFreeFly" );
	Cmd_RemoveCommand( "camswitch" );
	if( democam_editing_mode ) {
		CG_DemoEditMode_RemoveCmds();
	}

	CG_Democam_FreeCams();
	CG_Democam_FreeSubtitles();
	CG_Free( demoscriptname );
	demoscriptname = NULL;
}

/*
* CG_DemocamReset
*/
void CG_DemocamReset( void ) {
	demo_time = 0;
	demo_initial_timestamp = 0;
}
