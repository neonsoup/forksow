#include "qcommon/base.h"
#include "qcommon/qcommon.h"
#include "qcommon/assets.h"
#include "qcommon/hashtable.h"
#include "client/maps.h"
#include "client/renderer/model.h"

#include "zstd/zstd.h"

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

		u32 zstd_magic = ZSTD_MAGICNUMBER;
		if( memcmp( compressed.ptr, &zstd_magic, sizeof( zstd_magic ) ) == 0 ) {
			unsigned long long const decompressed_size = ZSTD_getDecompressedSize( compressed.ptr, compressed.n );
			if( decompressed_size == ZSTD_CONTENTSIZE_ERROR || decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN ) {
				Com_Printf( S_COLOR_RED "Corrupt BSP %s\n", path );
				continue;
			}

			decompressed = ALLOC_SPAN( sys_allocator, u8, decompressed_size );
			{
				ZoneScopedN( "ZSTD_decompress" );
				size_t r = ZSTD_decompress( decompressed.ptr, decompressed.n, compressed.ptr, compressed.n );
				if( r != decompressed_size ) {
					Com_Printf( S_COLOR_RED "Failed to decompress BSP: %s", ZSTD_getErrorName( r ) );
					continue;
				}
			}
		}

		Span< const u8 > data = decompressed.ptr == NULL ? compressed : decompressed;

		if( !LoadBSPRenderData( &maps[ num_maps ], path, data ) )
			continue;

		StringHash hash = StringHash( path );
		maps[ num_maps ].cms = CM_LoadMap( path, data, hash.hash );
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
