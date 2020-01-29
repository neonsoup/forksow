#include "qcommon/base.h"
#include "qcommon/qcommon.h"
#include "qcommon/assets.h"
#include "qcommon/compression.h"
#include "qcommon/hashtable.h"
#include "client/maps.h"
#include "client/renderer/model.h"

constexpr u32 MAX_MAPS = 128;

static Map maps[ MAX_MAPS ];
static u32 num_maps;
static Hashtable< MAX_MAPS * 2 > maps_hashtable;

void InitMaps() {
	ZoneScoped;

	num_maps = 0;

	for( const char * path : AssetPaths() ) {
		Span< const char > ext = FileExtension( path );
		if( ext != ".bsp" )
			continue;

		Span< const u8 > compressed = AssetBinary( path );
		if( compressed.n < 4 ) {
			Com_Printf( S_COLOR_RED "BSP too small %s\n", path );
			continue;
		}

		Span< u8 > decompressed;
		defer { FREE( sys_allocator, decompressed.ptr ); };
		bool ok = Decompress( path, sys_allocator, compressed, &decompressed );
		if( !ok )
			continue;

		Span< const u8 > data = decompressed.ptr == NULL ? compressed : decompressed;

		if( !LoadBSPRenderData( &maps[ num_maps ], path, data ) )
			continue;

		maps[ num_maps ].cms = CM_LoadMap( data );
		if( maps[ num_maps ].cms == NULL )
			// TODO: free render data
			continue;

		maps_hashtable.add( Hash64( path, strlen( path ) - ext.n ), num_maps );
		num_maps++;
	}

}

void ShutdownMaps() {
	for( u32 i = 0; i < num_maps; i++ ) {
		CM_Free( maps[ i ].cms );
	}
}

const Map * FindMap( StringHash name ) {
	u64 idx;
	if( !maps_hashtable.get( name.hash, &idx ) )
		return NULL;
	return &maps[ idx ];
}

const Map * FindMap( const char * name ) {
	return FindMap( StringHash( name ) );
}
