/*-----------------------------------------------------------------------
  Copyright (c) 2014-2016, NVIDIA. All rights reserved.
  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
   * Neither the name of its contributors may be used to endorse 
     or promote products derived from this software without specific
     prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
  PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
  OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
-----------------------------------------------------------------------*/

#include <GL/glew.h>

#include "threadpool.hpp"
#include <assert.h>

#define THREADPOOL_TERMINATE_FUNC  ((ThreadPool::WorkerFunc)1)

#if _WIN32

typedef BOOL (WINAPI *LPFN_GLPI)(
  PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, 
  PDWORD);


// Helper function to count set bits in the processor mask.
static DWORD CountSetBits(ULONG_PTR bitMask)
{
  DWORD LSHIFT = sizeof(ULONG_PTR)*8 - 1;
  DWORD bitSetCount = 0;
  ULONG_PTR bitTest = (ULONG_PTR)1 << LSHIFT;    
  DWORD i;

  for (i = 0; i <= LSHIFT; ++i)
  {
    bitSetCount += ((bitMask & bitTest)?1:0);
    bitTest/=2;
  }

  return bitSetCount;
}

unsigned int ThreadPool::sysGetNumCores()
{
  LPFN_GLPI glpi;
  BOOL done = FALSE;
  PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = NULL;
  PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = NULL;
  DWORD returnLength = 0;
  DWORD logicalProcessorCount = 0;
  DWORD numaNodeCount = 0;
  DWORD processorCoreCount = 0;
  DWORD processorL1CacheCount = 0;
  DWORD processorL2CacheCount = 0;
  DWORD processorL3CacheCount = 0;
  DWORD processorPackageCount = 0;
  DWORD byteOffset = 0;
  PCACHE_DESCRIPTOR Cache;

  glpi = (LPFN_GLPI) GetProcAddress(
    GetModuleHandleA("kernel32"),
    "GetLogicalProcessorInformation");
  if (NULL == glpi) 
  {
    return tthread::thread::hardware_concurrency();
  }

  while (!done)
  {
    DWORD rc = glpi(buffer, &returnLength);

    if (FALSE == rc) 
    {
      if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) 
      {
        if (buffer) 
          free(buffer);

        buffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(
          returnLength);

        if (NULL == buffer) 
        {
          return tthread::thread::hardware_concurrency();
        }
      } 
      else 
      {
        return tthread::thread::hardware_concurrency();
      }
    } 
    else
    {
      done = TRUE;
    }
  }

  ptr = buffer;

  while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength) 
  {
    switch (ptr->Relationship) 
    {
    case RelationNumaNode:
      // Non-NUMA systems report a single record of this type.
      numaNodeCount++;
      break;

    case RelationProcessorCore:
      processorCoreCount++;

      // A hyperthreaded core supplies more than one logical processor.
      logicalProcessorCount += CountSetBits(ptr->ProcessorMask);
      break;

    case RelationCache:
      // Cache data is in ptr->Cache, one CACHE_DESCRIPTOR structure for each cache. 
      Cache = &ptr->Cache;
      if (Cache->Level == 1)
      {
        processorL1CacheCount++;
      }
      else if (Cache->Level == 2)
      {
        processorL2CacheCount++;
      }
      else if (Cache->Level == 3)
      {
        processorL3CacheCount++;
      }
      break;

    case RelationProcessorPackage:
      // Logical processors share a physical package.
      processorPackageCount++;
      break;

    default:
      break;
    }
    byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    ptr++;
  }

#if 0
  printf(TEXT("\nGetLogicalProcessorInformation results:\n"));
  printf(TEXT("Number of NUMA nodes: %d\n"), 
    numaNodeCount);
  printf(TEXT("Number of physical processor packages: %d\n"), 
    processorPackageCount);
  printf(TEXT("Number of processor cores: %d\n"), 
    processorCoreCount);
  printf(TEXT("Number of logical processors: %d\n"), 
    logicalProcessorCount);
  printf(TEXT("Number of processor L1/L2/L3 caches: %d/%d/%d\n"), 
    processorL1CacheCount,
    processorL2CacheCount,
    processorL3CacheCount);
#endif

  free(buffer);

  return processorCoreCount;
}

#else

unsigned int ThreadPool::sysGetNumCores()
{
  return tthread::thread::hardware_concurrency();
}

#endif


void ThreadPool::threadKicker( void* arg )
{
  ThreadEntry* thread = (ThreadEntry*) arg;
  thread->m_origin->threadProcess(*thread);
}

void ThreadPool::threadProcess( ThreadEntry& entry )
{
  // create window
  NVPWindow::ContextFlags flags;
  flags.core = 0;
  flags.debug = 0;
  flags.major = 4;
  flags.minor = 2;
  flags.robust = 0;
  flags.share = entry.m_share;

  bool haswindow = flags.share != THREADPOOL_NO_CONTEXT;

  {
    tthread::lock_guard<tthread::mutex> lock(m_globalMutex);

    if (haswindow){
      entry.m_window.activate(16,16,"_dummy",&flags,1);
    }

    printf("%d created...\n", entry.m_id);

    m_globalInit++;
    m_globalCond.notify_all();
  }

#if _WIN32
  // assume hyperthreading, move to n physical cores
  unsigned int cpuCore = entry.m_id*2 + 1;
  SetThreadAffinityMask(GetCurrentThread(), 1 << cpuCore);
#endif

  while (true)
  {
    {
      tthread::lock_guard<tthread::mutex> lock(entry.m_commMutex);
      while(!entry.m_fn){
        entry.m_commCond.wait(entry.m_commMutex);
      }
    }

    if (entry.m_fn == THREADPOOL_TERMINATE_FUNC) break;

    NVP_BARRIER();

    if (haswindow){
      glPushAttrib(GL_ALL_ATTRIB_BITS);
      glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    }

    printf("%d started job\n", entry.m_id);

    entry.m_fn(entry.m_window,entry.m_fnArg);
    entry.m_fn = 0;
    
    if (haswindow){
      glFinish();
    }

    printf("%d finished job\n", entry.m_id);

    if (haswindow){
      glPopAttrib();
      glPopClientAttrib();
    }
  }

  printf("%d exiting...\n", entry.m_id);

  {
    tthread::lock_guard<tthread::mutex> lock(m_globalMutex);
    if (haswindow){
      entry.m_window.deactivate();
    }
    printf("%d shutdown\n", entry.m_id);
  }
  
}

void ThreadPool::init( unsigned int numThreads, NVPWindow* share)
{
  m_numThreads  = numThreads;
  m_globalInit = 0;
  m_hasSharedContext = share != THREADPOOL_NO_CONTEXT;

  m_pool = new ThreadEntry[numThreads];

  for (unsigned int i = 0; i < numThreads; i++){
    ThreadEntry& entry = m_pool[i];
    entry.m_share = share;
    entry.m_id = numThreads - i - 1;
    entry.m_origin = this;
    entry.m_fn = 0;
    entry.m_fnArg = 0;
  }

  NVP_BARRIER();

  for (unsigned int i = 0; i < numThreads; i++){
    ThreadEntry& entry = m_pool[i];
    entry.m_thread = new tthread::thread( threadKicker, &m_pool[i]);
  }

  {
    tthread::lock_guard<tthread::mutex> lock(m_globalMutex);
    while (m_globalInit < numThreads){
      m_globalCond.wait(m_globalMutex);
    }
  }

#if _WIN32
  // pin the main thread to core 0
  SetThreadAffinityMask(GetCurrentThread(), 1);
#endif
}

void ThreadPool::deinit()
{
  NVP_BARRIER();

  for (unsigned int i = 0; i < m_numThreads; i++){
    ThreadEntry& entry = m_pool[i];

    {
      tthread::lock_guard<tthread::mutex> lock(entry.m_commMutex);
      entry.m_fn = THREADPOOL_TERMINATE_FUNC;
      entry.m_fnArg = 0;
      entry.m_commCond.notify_all();
    }

    tthread::this_thread::yield();

    entry.m_thread->join();
    delete entry.m_thread;
    entry.m_thread = 0;
  }

  delete [] m_pool;
  m_pool = 0;
  m_numThreads = 0;
}

void ThreadPool::activateJob( unsigned int tid, WorkerFunc fn, void* arg )
{
  assert( tid < m_numThreads);

  ThreadEntry& entry = m_pool[tid];

  assert( entry.m_fn == 0 );

  {
    tthread::lock_guard<tthread::mutex> lock(entry.m_commMutex);
    entry.m_fn = fn;
    entry.m_fnArg = arg;
    entry.m_commCond.notify_all();
  }

}

