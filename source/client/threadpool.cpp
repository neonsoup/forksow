#include "qcommon/base.h"
#include "qcommon/qcommon.h" // TODO: delete min/max macros and add tracy to base
#include "qcommon/threads.h"
#include "client/threadpool.h"

struct Job {
	JobCallback callback;
	void * data;
};

static Job jobs[ 4096 ];
static Mutex * jobs_mutex;
static Semaphore * jobs_sem;
static bool shutting_down;
static size_t jobs_head;
static size_t jobs_length;

static Thread * workers[ 32 ];
static u32 num_workers;

static void ThreadPoolWorker( void * ) {
	while( true ) {
		Wait( jobs_sem );
		Lock( jobs_mutex );

		if( shutting_down ) {
			Unlock( jobs_mutex );
			break;
		}

		if( jobs_length == 0 ) {
			Unlock( jobs_mutex );
			continue;
		}

		Job * job = &jobs[ jobs_head % ARRAY_COUNT( jobs ) ];
		jobs_head++;
		jobs_length--;

		Unlock( jobs_mutex );

		{
			ZoneScopedN( "Do a job" );
			job->callback( job->data );
		}
	}
}

void InitThreadPool() {
	shutting_down = false;
	jobs_head = 0;
	jobs_length = 0;
	jobs_mutex = NewMutex();
	jobs_sem = NewSemaphore();

	num_workers = Min2( GetCoreCount() - 1, u32( ARRAY_COUNT( workers ) ) );

	for( u32 i = 0; i < num_workers; i++ ) {
		workers[ i ] = NewThread( ThreadPoolWorker );
	}
}

void ShutdownThreadPool() {
	shutting_down = true;

	for( u32 i = 0; i < num_workers; i++ ) {
		Signal( jobs_sem );
	}

	for( u32 i = 0; i < num_workers; i++ ) {
		JoinThread( workers[ i ] );
	}

	DeleteSemaphore( jobs_sem );
	DeleteMutex( jobs_mutex );
}

void ThreadPoolDo( JobCallback callback, void * data ) {
	ZoneScoped;

	Lock( jobs_mutex );

	assert( jobs_length < ARRAY_COUNT( jobs ) );

	Job * job = &jobs[ ( jobs_head + jobs_length ) % ARRAY_COUNT( jobs ) ];
	job->callback = callback;
	job->data = data;

	jobs_length++;

	Unlock( jobs_mutex );
	Signal( jobs_sem );
}

void ThreadPoolFinish() {
	while( true ) {
		Lock( jobs_mutex );

		if( jobs_length == 0 ) {
			Unlock( jobs_mutex );
			break;
		}

		Job * job = &jobs[ jobs_head % ARRAY_COUNT( jobs ) ];
		jobs_head++;
		jobs_length--;

		Unlock( jobs_mutex );

		{
			ZoneScopedN( "Do a job" );
			job->callback( job->data );
		}
	}
}
