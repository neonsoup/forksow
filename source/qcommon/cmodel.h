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

#define MAX_CM_LEAFS        ( MAX_MAP_LEAFS )

typedef struct {
	int contents;
	int flags;
	char *name;
} cshaderref_t;

typedef struct {
	int children[2];            // negative numbers are leafs
	cplane_t *plane;
} cnode_t;

typedef struct {
	int surfFlags;
	cplane_t plane;
} cbrushside_t;

typedef struct {
	int contents;
	int numsides;

	vec3_t mins, maxs;

	cbrushside_t *brushsides;
} cbrush_t;

typedef struct {
	int contents;
	int numfacets;

	vec3_t mins, maxs;

	cbrush_t *facets;
} cface_t;

typedef struct {
	int contents;
	int cluster;

	int area;

	int nummarkbrushes;
	int nummarkfaces;

	int *markbrushes;
	int *markfaces;
} cleaf_t;

typedef struct cmodel_s {
	u64 hash;

	bool builtin;

	int nummarkfaces;
	int nummarkbrushes;

	vec3_t cyl_offset;

	vec3_t mins, maxs;

	cbrush_t *brushes;
	cface_t *faces;

	// dummy iterators for the tracing code
	// which treats brush models as leafs
	int *markfaces;
	int *markbrushes;
} cmodel_t;

typedef struct {
	int floodnum;               // if two areas have equal floodnums, they are connected
	int floodvalid;
} carea_t;

struct CollisionModel {
	u64 base_hash;
	u64 world_hash;

	int checkcount;
	int floodvalid;

	u32 checksum;

	int numbrushsides;
	cbrushside_t *map_brushsides;

	int numshaderrefs;
	cshaderref_t *map_shaderrefs;

	int numplanes;
	cplane_t *map_planes;

	int numnodes;
	cnode_t *map_nodes;

	int numleafs;                   // = 1
	cleaf_t map_leaf_empty;         // allow leaf funcs to be called without a map
	cleaf_t *map_leafs;             // = &map_leaf_empty;

	int nummarkbrushes;
	int *map_markbrushes;

	u32 num_models;
	vec3_t world_mins, world_maxs;

	int numbrushes;
	cbrush_t *map_brushes;

	int numfaces;
	cface_t *map_faces;

	int nummarkfaces;
	int *map_markfaces;

	vec3_t *map_verts;              // this will be freed
	int numvertexes;

	// each area has a list of portals that lead into other areas
	// when portals are closed, other areas may not be visible or
	// hearable even if the vis info says that it should be
	int numareas;                   // = 1
	carea_t map_area_empty;
	carea_t *map_areas;             // = &map_area_empty;
	int *map_areaportals;

	dvis_t *map_pvs;
	int map_visdatasize;

	uint8_t nullrow[MAX_CM_LEAFS / 8];

	int numentitychars;
	char map_entitystring_empty;
	char *map_entitystring;         // = &map_entitystring_empty;

	const u8 *cmod_base;

	// cm_trace.c
	cbrushside_t box_brushsides[6];
	cbrush_t box_brush[1];
	int box_markbrushes[1];
	cmodel_t box_cmodel[1];
	int box_checkcount;

	cbrushside_t oct_brushsides[10];
	cbrush_t oct_brush[1];
	int oct_markbrushes[1];
	cmodel_t oct_cmodel[1];
	int oct_checkcount;

	int *map_brush_checkcheckouts;
	int *map_face_checkcheckouts;
};

CollisionModel *CM_LoadMap( Span< const u8 > data, u64 base_hash );
void CM_Free( CollisionModel *cms );

struct cmodel_s * CM_FindCModel( StringHash hash );
struct cmodel_s * CM_TryFindCModel( StringHash hash );

bool CM_IsBrushModel( StringHash hash );

int CM_NumClusters( CollisionModel *cms );
int CM_NumAreas( CollisionModel *cms );
char *CM_EntityString( CollisionModel *cms );
int CM_EntityStringLen( CollisionModel *cms );
const char *CM_ShaderrefName( CollisionModel *cms, int ref );

// creates a clipping hull for an arbitrary bounding box
struct cmodel_s *CM_ModelForBBox( CollisionModel *cms, vec3_t mins, vec3_t maxs );
struct cmodel_s *CM_OctagonModelForBBox( CollisionModel *cms, vec3_t mins, vec3_t maxs );
void CM_InlineModelBounds( const CollisionModel *cms, const struct cmodel_s *cmodel, vec3_t mins, vec3_t maxs );

// returns an ORed contents mask
int CM_TransformedPointContents( CollisionModel *cms, const vec3_t p, struct cmodel_s *cmodel, const vec3_t origin, const vec3_t angles );

void CM_TransformedBoxTrace( CollisionModel *cms, trace_t *tr, const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs,
							 struct cmodel_s *cmodel, int brushmask, const vec3_t origin, const vec3_t angles );

int CM_ClusterRowSize( CollisionModel *cms );
int CM_AreaRowSize( CollisionModel *cms );
int CM_PointLeafnum( CollisionModel *cms, const vec3_t p );

// call with topnode set to the headnode, returns with topnode
// set to the first node that splits the box
int CM_BoxLeafnums( CollisionModel *cms, vec3_t mins, vec3_t maxs, int *list, int listsize, int *topnode );

int CM_LeafCluster( CollisionModel *cms, int leafnum );
int CM_LeafArea( CollisionModel *cms, int leafnum );

void CM_SetAreaPortalState( CollisionModel *cms, int area1, int area2, bool open );
bool CM_AreasConnected( CollisionModel *cms, int area1, int area2 );

int CM_WriteAreaBits( CollisionModel *cms, uint8_t *buffer );
void CM_ReadAreaBits( CollisionModel *cms, uint8_t *buffer );
bool CM_HeadnodeVisible( CollisionModel *cms, int headnode, uint8_t *visbits );

void CM_WritePortalState( CollisionModel *cms, int file );
void CM_ReadPortalState( CollisionModel *cms, int file );

void CM_MergePVS( CollisionModel *cms, const vec3_t org, uint8_t *out );
int CM_MergeVisSets( CollisionModel *cms, const vec3_t org, uint8_t *pvs, uint8_t *areabits );

bool CM_InPVS( CollisionModel *cms, const vec3_t p1, const vec3_t p2 );

bool CM_LeafsInPVS( CollisionModel *cms, int leafnum1, int leafnum2 );

void CM_Init( void );
void CM_Shutdown( void );
