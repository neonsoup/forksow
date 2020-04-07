#pragma once

struct Thread;
struct Mutex;
struct Semaphore;

Thread * NewThread( void ( *callback )( void * ), void * data = NULL );
void JoinThread( Thread * thread );

Mutex * NewMutex();
void DeleteMutex( Mutex * mutex );
void Lock( Mutex * mutex );
void Unlock( Mutex * mutex );

Semaphore * NewSemaphore();
void DeleteSemaphore( Semaphore * sem );
void Wait( Semaphore * sem );
void Signal( Semaphore * sem );

u32 GetCoreCount();

int Sys_Atomic_FetchAdd( volatile int *value, int add );
