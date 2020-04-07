#pragma once

typedef void ( *JobCallback )( void * data );

void InitThreadPool();
void ShutdownThreadPool();

void ThreadPoolDo( JobCallback callback, void * data );
void ThreadPoolFinish();
