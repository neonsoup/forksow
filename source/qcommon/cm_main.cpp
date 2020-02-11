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

#include "qcommon/qcommon.h"
#include "qcommon/cm_local.h"
#include "qcommon/hashtable.h"
#include "qcommon/string.h"

constexpr u32 MAX_CMODELS = 1024;

static cmodel_t cmodels[ MAX_CMODELS ];
static u32 num_cmodels;
static Hashtable< MAX_CMODELS * 2 > cmodels_hashtable;

static bool cm_initialized = false;

mempool_t *cmap_mempool;

static cvar_t *cm_noAreas;

/*
* CM_AllocateCheckCounts
*/
static void CM_AllocateCheckCounts( CollisionModel *cms ) {
	cms->checkcount = 0;
	cms->map_brush_checkcheckouts = ( int * ) Mem_Alloc( cmap_mempool, cms->numbrushes * sizeof( int ) );
	cms->map_face_checkcheckouts = ( int * ) Mem_Alloc( cmap_mempool, cms->numfaces * sizeof( int ) );
}

/*
* CM_FreeCheckCounts
*/
static void CM_FreeCheckCounts( CollisionModel *cms ) {
	cms->checkcount = 0;

	if( cms->map_brush_checkcheckouts ) {
		Mem_Free( cms->map_brush_checkcheckouts );
		cms->map_brush_checkcheckouts = NULL;
	}

	if( cms->map_face_checkcheckouts ) {
		Mem_Free( cms->map_face_checkcheckouts );
		cms->map_face_checkcheckouts = NULL;
	}
}

/*
* CM_Clear
*/
static void CM_Clear( CollisionModel *cms ) {
	if( cms->map_shaderrefs ) {
		Mem_Free( cms->map_shaderrefs[0].name );
		Mem_Free( cms->map_shaderrefs );
		cms->map_shaderrefs = NULL;
		cms->numshaderrefs = 0;
	}

	if( cms->map_faces ) {
		for( int i = 0; i < cms->numfaces; i++ )
			Mem_Free( cms->map_faces[i].facets );
		Mem_Free( cms->map_faces );
		cms->map_faces = NULL;
		cms->numfaces = 0;
	}

	for( u32 i = 0; i < cms->num_models; i++ ) {
		String< 16 > suffix( "*{}", i );
		u64 hash = Hash64( suffix.c_str(), suffix.len(), cms->base_hash );

		u64 idx;
		bool ok = cmodels_hashtable.get( hash, &idx );
		assert( ok );

		Mem_Free( cmodels[ idx ].markfaces );
		Mem_Free( cmodels[ idx ].markbrushes );

		num_cmodels--;
		Swap2( &cmodels[ idx ], &cmodels[ num_cmodels ] );

		cmodels_hashtable.update( cmodels[ idx ].hash, idx );
		cmodels_hashtable.remove( cmodels[ num_cmodels ].hash );
	}

	if( cms->map_nodes ) {
		Mem_Free( cms->map_nodes );
		cms->map_nodes = NULL;
		cms->numnodes = 0;
	}

	if( cms->map_markfaces ) {
		Mem_Free( cms->map_markfaces );
		cms->map_markfaces = NULL;
		cms->nummarkfaces = 0;
	}

	if( cms->map_leafs != &cms->map_leaf_empty ) {
		Mem_Free( cms->map_leafs );
		cms->map_leafs = &cms->map_leaf_empty;
		cms->numleafs = 0;
	}

	if( cms->map_areas != &cms->map_area_empty ) {
		Mem_Free( cms->map_areas );
		cms->map_areas = &cms->map_area_empty;
		cms->numareas = 0;
	}

	if( cms->map_areaportals ) {
		Mem_Free( cms->map_areaportals );
		cms->map_areaportals = NULL;
	}

	if( cms->map_planes ) {
		Mem_Free( cms->map_planes );
		cms->map_planes = NULL;
		cms->numplanes = 0;
	}

	if( cms->map_markbrushes ) {
		Mem_Free( cms->map_markbrushes );
		cms->map_markbrushes = NULL;
		cms->nummarkbrushes = 0;
	}

	if( cms->map_brushsides ) {
		Mem_Free( cms->map_brushsides );
		cms->map_brushsides = NULL;
		cms->numbrushsides = 0;
	}

	if( cms->map_brushes ) {
		Mem_Free( cms->map_brushes );
		cms->map_brushes = NULL;
		cms->numbrushes = 0;
	}

	if( cms->map_pvs ) {
		Mem_Free( cms->map_pvs );
		cms->map_pvs = NULL;
	}

	if( cms->map_entitystring != &cms->map_entitystring_empty ) {
		Mem_Free( cms->map_entitystring );
		cms->map_entitystring = &cms->map_entitystring_empty;
	}

	CM_FreeCheckCounts( cms );

	ClearBounds( cms->world_mins, cms->world_maxs );
}

/*
===============================================================================

MAP LOADING

===============================================================================
*/

/*
* CM_LoadMap
* Loads in the map and all submodels
*/
CollisionModel * CM_LoadMap( Span< const u8 > data, u64 base_hash ) {
	ZoneScoped;

	CollisionModel * cms = ( CollisionModel * ) Mem_Alloc( cmap_mempool, sizeof( CollisionModel ) );
	cms->base_hash = base_hash;

	const char * suffix = "*0";
	cms->world_hash = Hash64( suffix, strlen( suffix ), base_hash );

	CM_InitBoxHull( cms );
	CM_InitOctagonHull( cms );
	CM_Clear( cms );

	CM_LoadQ3BrushModel( cms, data );

	if( cms->numareas ) {
		cms->map_areas = ( carea_t * ) Mem_Alloc( cmap_mempool, cms->numareas * sizeof( *cms->map_areas ) );
		cms->map_areaportals = ( int * ) Mem_Alloc( cmap_mempool, cms->numareas * cms->numareas * sizeof( *cms->map_areaportals ) );

		memset( cms->map_areaportals, 0, cms->numareas * cms->numareas * sizeof( *cms->map_areaportals ) );
		CM_FloodAreaConnections( cms );
	}

	CM_AllocateCheckCounts( cms );

	memset( cms->nullrow, 255, MAX_CM_LEAFS / 8 );

	return cms;
}

/*
* CM_Free
*/
void CM_Free( CollisionModel *cms ) {
	CM_Clear( cms );
	Mem_Free( cms );
}

cmodel_t * CM_NewCModel( u64 hash ) {
	// TODO: bounds checking
	cmodel_t * model = &cmodels[ num_cmodels ];
	model->hash = hash;
	cmodels_hashtable.add( hash, num_cmodels );
	num_cmodels++;
	return model;
}

cmodel_t * CM_TryFindCModel( StringHash hash ) {
	u64 idx;
	return cmodels_hashtable.get( hash.hash, &idx ) ? &cmodels[ idx ] : NULL;
}

cmodel_t * CM_FindCModel( StringHash hash ) {
	cmodel_t * cmodel = CM_TryFindCModel( hash );
	if( cmodel == NULL ) {
		Com_Error( ERR_DROP, "FindCModel failed" );
	}
	return cmodel;
}

bool CM_IsBrushModel( StringHash hash ) {
	u64 idx;
	return cmodels_hashtable.get( hash.hash, &idx );
}

/*
* CM_InlineModelBounds
*/
void CM_InlineModelBounds( const CollisionModel *cms, const cmodel_t *cmodel, vec3_t mins, vec3_t maxs ) {
	if( cmodel->hash == cms->world_hash ) {
		VectorCopy( cms->world_mins, mins );
		VectorCopy( cms->world_maxs, maxs );
	} else {
		VectorCopy( cmodel->mins, mins );
		VectorCopy( cmodel->maxs, maxs );
	}
}

/*
* CM_ShaderrefName
*/
const char *CM_ShaderrefName( CollisionModel *cms, int ref ) {
	if( ref < 0 || ref >= cms->numshaderrefs ) {
		return NULL;
	}
	return cms->map_shaderrefs[ref].name;
}

/*
* CM_EntityStringLen
*/
int CM_EntityStringLen( CollisionModel *cms ) {
	return cms->numentitychars;
}

/*
* CM_EntityString
*/
char *CM_EntityString( CollisionModel *cms ) {
	return cms->map_entitystring;
}

/*
* CM_LeafCluster
*/
int CM_LeafCluster( CollisionModel *cms, int leafnum ) {
	if( leafnum < 0 || leafnum >= cms->numleafs ) {
		Com_Error( ERR_DROP, "CM_LeafCluster: bad number" );
	}
	return cms->map_leafs[leafnum].cluster;
}

/*
* CM_LeafArea
*/
int CM_LeafArea( CollisionModel *cms, int leafnum ) {
	if( leafnum < 0 || leafnum >= cms->numleafs ) {
		Com_Error( ERR_DROP, "CM_LeafArea: bad number" );
	}
	return cms->map_leafs[leafnum].area;
}

/*
* CM_BoundBrush
*/
void CM_BoundBrush( cbrush_t *brush ) {
	int i;

	for( i = 0; i < 3; i++ ) {
		brush->mins[i] = -brush->brushsides[i * 2 + 0].plane.dist;
		brush->maxs[i] = +brush->brushsides[i * 2 + 1].plane.dist;
	}
}

/*
===============================================================================

PVS

===============================================================================
*/

/*
* CM_ClusterRowSize
*/
int CM_ClusterRowSize( CollisionModel *cms ) {
	return cms->map_pvs ? cms->map_pvs->rowsize : MAX_CM_LEAFS / 8;
}

/*
* CM_ClusterRowLongs
*/
static int CM_ClusterRowLongs( CollisionModel *cms ) {
	return cms->map_pvs ? ( cms->map_pvs->rowsize + 3 ) / 4 : MAX_CM_LEAFS / 32;
}

/*
* CM_NumClusters
*/
int CM_NumClusters( CollisionModel *cms ) {
	return cms->map_pvs ? cms->map_pvs->numclusters : 0;
}

/*
* CM_ClusterPVS
*/
static inline uint8_t *CM_ClusterPVS( CollisionModel *cms, int cluster ) {
	dvis_t *vis = cms->map_pvs;

	if( cluster == -1 || !vis ) {
		return cms->nullrow;
	}

	return ( uint8_t * )vis->data + cluster * vis->rowsize;
}

/*
* CM_NumAreas
*/
int CM_NumAreas( CollisionModel *cms ) {
	return cms->numareas;
}

/*
* CM_AreaRowSize
*/
int CM_AreaRowSize( CollisionModel *cms ) {
	return ( cms->numareas + 7 ) / 8;
}

/*
===============================================================================

AREAPORTALS

===============================================================================
*/

/*
* CM_FloodArea_r
*/
static void CM_FloodArea_r( CollisionModel *cms, int areanum, int floodnum ) {
	int i;
	carea_t *area;
	int *p;

	area = &cms->map_areas[areanum];
	if( area->floodvalid == cms->floodvalid ) {
		if( area->floodnum == floodnum ) {
			return;
		}
		Com_Error( ERR_DROP, "FloodArea_r: reflooded" );
	}

	area->floodnum = floodnum;
	area->floodvalid = cms->floodvalid;
	p = cms->map_areaportals + areanum * cms->numareas;
	for( i = 0; i < cms->numareas; i++ ) {
		if( p[i] > 0 ) {
			CM_FloodArea_r( cms, i, floodnum );
		}
	}
}

/*
* CM_FloodAreaConnections
*/
void CM_FloodAreaConnections( CollisionModel *cms ) {
	int i;
	int floodnum;

	// all current floods are now invalid
	cms->floodvalid++;
	floodnum = 0;
	for( i = 0; i < cms->numareas; i++ ) {
		if( cms->map_areas[i].floodvalid == cms->floodvalid ) {
			continue; // already flooded into
		}
		floodnum++;
		CM_FloodArea_r( cms, i, floodnum );
	}
}

/*
* CM_SetAreaPortalState
*/
void CM_SetAreaPortalState( CollisionModel *cms, int area1, int area2, bool open ) {
	int row1, row2;

	if( area1 == area2 ) {
		return;
	}
	if( area1 < 0 || area2 < 0 ) {
		return;
	}

	row1 = area1 * cms->numareas + area2;
	row2 = area2 * cms->numareas + area1;
	if( open ) {
		cms->map_areaportals[row1]++;
		cms->map_areaportals[row2]++;
	} else {
		if( cms->map_areaportals[row1] > 0 ) {
			cms->map_areaportals[row1]--;
		}
		if( cms->map_areaportals[row2] > 0 ) {
			cms->map_areaportals[row2]--;
		}
	}

	CM_FloodAreaConnections( cms );
}

/*
* CM_AreasConnected
*/
bool CM_AreasConnected( CollisionModel *cms, int area1, int area2 ) {
	if( cm_noAreas->integer ) {
		return true;
	}

	if( area1 == area2 ) {
		return true;
	}
	if( area1 < 0 || area2 < 0 ) {
		return true;
	}

	if( area1 >= cms->numareas || area2 >= cms->numareas ) {
		Com_Error( ERR_DROP, "CM_AreasConnected: area >= numareas" );
	}

	if( cms->map_areas[area1].floodnum == cms->map_areas[area2].floodnum ) {
		return true;
	}
	return false;
}

/*
* CM_MergeAreaBits
*/
static int CM_MergeAreaBits( CollisionModel *cms, uint8_t *buffer, int area ) {
	int i;

	if( area < 0 ) {
		return CM_AreaRowSize( cms );
	}

	for( i = 0; i < cms->numareas; i++ ) {
		if( CM_AreasConnected( cms, i, area ) ) {
			buffer[i >> 3] |= 1 << ( i & 7 );
		}
	}

	return CM_AreaRowSize( cms );
}

/*
* CM_WriteAreaBits
*/
int CM_WriteAreaBits( CollisionModel *cms, uint8_t *buffer ) {
	int i;
	int rowsize, bytes;

	rowsize = CM_AreaRowSize( cms );
	bytes = rowsize * cms->numareas;

	if( cm_noAreas->integer ) {
		// for debugging, send everything
		memset( buffer, 255, bytes );
	} else {
		uint8_t *row;

		memset( buffer, 0, bytes );

		for( i = 0; i < cms->numareas; i++ ) {
			row = buffer + i * rowsize;
			CM_MergeAreaBits( cms, row, i );
		}
	}

	return bytes;
}

/*
* CM_ReadAreaBits
*/
void CM_ReadAreaBits( CollisionModel *cms, uint8_t *buffer ) {
	int i, j;
	int rowsize;

	memset( cms->map_areaportals, 0, cms->numareas * cms->numareas * sizeof( *cms->map_areaportals ) );

	rowsize = CM_AreaRowSize( cms );
	for( i = 0; i < cms->numareas; i++ ) {
		uint8_t *row;

		row = buffer + i * rowsize;
		for( j = 0; j < cms->numareas; j++ ) {
			if( row[j >> 3] & ( 1 << ( j & 7 ) ) ) {
				cms->map_areaportals[i * cms->numareas + j] = 1;
			}
		}
	}

	CM_FloodAreaConnections( cms );
}

/*
* CM_WritePortalState
* Writes the portal state to a savegame file
*/
void CM_WritePortalState( CollisionModel *cms, int file ) {
	int i, j, t;

	for( i = 0; i < cms->numareas; i++ ) {
		for( j = 0; j < cms->numareas; j++ ) {
			t = LittleLong( cms->map_areaportals[i * cms->numareas + j] );
			FS_Write( &t, sizeof( t ), file );
		}
	}
}

/*
* CM_ReadPortalState
* Reads the portal state from a savegame file
* and recalculates the area connections
*/
void CM_ReadPortalState( CollisionModel *cms, int file ) {
	int i;

	FS_Read( &cms->map_areaportals, cms->numareas * cms->numareas * sizeof( *cms->map_areaportals ), file );

	for( i = 0; i < cms->numareas * cms->numareas; i++ )
		cms->map_areaportals[i] = LittleLong( cms->map_areaportals[i] );

	CM_FloodAreaConnections( cms );
}

/*
* CM_HeadnodeVisible
* Returns true if any leaf under headnode has a cluster that
* is potentially visible
*/
bool CM_HeadnodeVisible( CollisionModel *cms, int nodenum, uint8_t *visbits ) {
	int cluster;
	cnode_t *node;

	while( nodenum >= 0 ) {
		node = &cms->map_nodes[nodenum];
		if( CM_HeadnodeVisible( cms, node->children[0], visbits ) ) {
			return true;
		}
		nodenum = node->children[1];
	}

	cluster = cms->map_leafs[-1 - nodenum].cluster;
	if( cluster == -1 ) {
		return false;
	}
	if( visbits[cluster >> 3] & ( 1 << ( cluster & 7 ) ) ) {
		return true;
	}
	return false;
}


/*
* CM_MergePVS
* Merge PVS at origin into out
*/
void CM_MergePVS( CollisionModel *cms, const vec3_t org, uint8_t *out ) {
	int leafs[128];
	int i, j, count;
	int longs;
	uint8_t *src;
	vec3_t mins, maxs;

	for( i = 0; i < 3; i++ ) {
		mins[i] = org[i] - 9;
		maxs[i] = org[i] + 9;
	}

	count = CM_BoxLeafnums( cms, mins, maxs, leafs, sizeof( leafs ) / sizeof( int ), NULL );
	if( count < 1 ) {
		Com_Error( ERR_FATAL, "CM_MergePVS: count < 1" );
	}
	longs = CM_ClusterRowLongs( cms );

	// convert leafs to clusters
	for( i = 0; i < count; i++ )
		leafs[i] = CM_LeafCluster( cms, leafs[i] );

	// or in all the other leaf bits
	for( i = 0; i < count; i++ ) {
		for( j = 0; j < i; j++ )
			if( leafs[i] == leafs[j] ) {
				break;
			}
		if( j != i ) {
			continue; // already have the cluster we want
		}
		src = CM_ClusterPVS( cms, leafs[i] );
		for( j = 0; j < longs; j++ )
			( (int *)out )[j] |= ( (int *)src )[j];
	}
}

/*
* CM_MergeVisSets
*/
int CM_MergeVisSets( CollisionModel *cms, const vec3_t org, uint8_t *pvs, uint8_t *areabits ) {
	int area;

	assert( pvs || areabits );

	if( pvs ) {
		CM_MergePVS( cms, org, pvs );
	}

	area = CM_PointLeafnum( cms, org );

	area = CM_LeafArea( cms, area );
	if( areabits && area > -1 ) {
		CM_MergeAreaBits( cms, areabits, area );
	}

	return CM_AreaRowSize( cms ); // areabytes
}

/*
* CM_InPVS
*
* Also checks portalareas so that doors block sight
*/
bool CM_InPVS( CollisionModel *cms, const vec3_t p1, const vec3_t p2 ) {
	int leafnum1, leafnum2;

	leafnum1 = CM_PointLeafnum( cms, p1 );
	leafnum2 = CM_PointLeafnum( cms, p2 );

	return CM_LeafsInPVS( cms, leafnum1, leafnum2 );
}

bool CM_LeafsInPVS( CollisionModel *cms, int leafnum1, int leafnum2 ) {
	int cluster;
	int area1, area2;
	uint8_t *mask;

	cluster = CM_LeafCluster( cms, leafnum1 );
	area1 = CM_LeafArea( cms, leafnum1 );
	mask = CM_ClusterPVS( cms, cluster );

	cluster = CM_LeafCluster( cms, leafnum2 );
	area2 = CM_LeafArea( cms, leafnum2 );

	if( ( !( mask[cluster >> 3] & ( 1 << ( cluster & 7 ) ) ) ) ) {
		return false;
	}

	if( !CM_AreasConnected( cms, area1, area2 ) ) {
		return false; // a door blocks sight

	}
	return true;
}

/*
* CM_Init
*/
void CM_Init( void ) {
	assert( !cm_initialized );

	cmap_mempool = Mem_AllocPool( NULL, "Collision Map" );

	cm_noAreas = Cvar_Get( "cm_noAreas", "0", CVAR_CHEAT );

	cm_initialized = true;
}

/*
* CM_Shutdown
*/
void CM_Shutdown( void ) {
	if( !cm_initialized ) {
		return;
	}

	Mem_FreePool( &cmap_mempool );

	cm_initialized = false;
}
