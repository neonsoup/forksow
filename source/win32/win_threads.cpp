#include "qcommon/base.h"
#include "qcommon/qcommon.h"

#include <windows.h>
#include <process.h>

struct Thread { HANDLE handle; };
struct Mutex { SRWLOCK lock; };
struct Semaphore { HANDLE handle; };

struct ThreadStartData {
	void ( *callback )( void * );
	void * data;
};

static DWORD WINAPI ThreadWrapper( void * data ) {
	ThreadStartData * tsd = ( ThreadStartData * ) data;
	tsd->callback( tsd->data );
	FREE( sys_allocator, tsd );

	return 0;
}

Thread * NewThread( void ( *callback )( void * ), void * data ) {
	ThreadStartData * tsd = ALLOC( sys_allocator, ThreadStartData );
	tsd->callback = callback;
	tsd->data = data;

	DWORD id;
	HANDLE handle = CreateThread( 0, 0, ThreadWrapper, tsd, 0, &id );
	if( handle == NULL ) {
		Com_Error( ERR_FATAL, "CreateThread" );
	}

	// can't use sys_allocator because serverlist leaks its threads
	Thread * thread = new Thread;
	thread->handle = handle;
	return thread;
}

void JoinThread( Thread * thread ) {
	WaitForSingleObject( thread->handle, INFINITE );
	CloseHandle( thread->handle );
	delete thread;
}

Mutex * NewMutex() {
	// can't use sys_allocator because sys_allocator itself calls NewMutex
	Mutex * mutex = new Mutex;
	InitializeSRWLock( &mutex->lock );
	return mutex;
}

void DeleteMutex( Mutex * mutex ) {
	delete mutex;
}

void Lock( Mutex * mutex ) {
	AcquireSRWLockExclusive( &mutex->lock );
}

void Unlock( Mutex * mutex ) {
	ReleaseSRWLockExclusive( &mutex->lock );
}

Semaphore * NewSemaphore() {
	LONG max = 8192; // needs to be at least as big as thread pool size TODO
	HANDLE handle = CreateSemaphoreA( NULL, 0, max, NULL );
	if( handle == NULL ) {
		Com_Error( ERR_FATAL, "CreateSemaphoreA" );
	}

	Semaphore * sem = ALLOC( sys_allocator, Semaphore );
	sem->handle = handle;
	return sem;
}

void DeleteSemaphore( Semaphore * sem ) {
	CloseHandle( sem->handle );
	FREE( sys_allocator, sem );
}

void Signal( Semaphore * sem ) {
	if( ReleaseSemaphore( sem->handle, 1, NULL ) == 0 ) {
		Com_Error( ERR_FATAL, "ReleaseSemaphore" );
	}
}

void Wait( Semaphore * sem ) {
	WaitForSingleObject( sem->handle, INFINITE );
}

u32 GetCoreCount() {
	SYSTEM_INFO info;
	GetSystemInfo( &info );
	return info.dwNumberOfProcessors;
}

int Sys_Atomic_FetchAdd( volatile int *value, int add ) {
	return InterlockedExchangeAdd( (volatile LONG*)value, add );
}
