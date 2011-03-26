#include "profile.h"
#include "profile-trace.h"
#include "sym-cache.h"
#include "atomic.h"
#include "hook.h"
#include "walk-syms.h"
#include <algorithm>
#include <sys/types.h>
#include <sys/time.h>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <list>
#include <unistd.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <cxxabi.h>

#ifdef __APPLE__
# include <crt_externs.h>
# define program_invocation_name **_NSGetArgv()
#endif

// -------------------------------------------------------------------
// Used to capture real user start arguments in our custom thread wrapper
struct HIDDEN IgProfWrappedArg { void *(*start_routine)(void *); void *arg; };
struct HIDDEN IgProfTraceAlloc { IgProfTrace *buf; bool perthread; };
struct HIDDEN IgProfDumpInfo { int depth; int nsyms; int nlibs; int nctrs;
                               const char *tofile; FILE *output;
                               IgProfSymCache *symcache; int blocksig;
			       IgProfTrace::PerfStat perf; };
class HIDDEN IgProfExitDump { public: ~IgProfExitDump(void); };
typedef std::list<void (*) (void)> IgProfCallList;
typedef std::list<IgProfTraceAlloc *> IgProfBufList;

static void igprof_init_thread(void);
static void igprof_exit_thread(bool final);

// -------------------------------------------------------------------
// Traps for this profiling module
DUAL_HOOK(1, void, doexit, _main, _libc,
          (int code), (code),
          "exit", 0, "libc.so.6")
DUAL_HOOK(1, void, doexit, _main2, _libc2,
          (int code), (code),
          "_exit", 0, "libc.so.6")
DUAL_HOOK(2, int,  dokill, _main, _libc,
          (pid_t pid, int sig), (pid, sig),
          "kill", 0, "libc.so.6")

LIBHOOK(4, int, dopthread_create, _main,
        (pthread_t *thread, const pthread_attr_t *attr,
         void * (*start_routine)(void *), void *arg),
        (thread, attr, start_routine, arg),
        "pthread_create", 0, 0)

LIBHOOK(4, int, dopthread_create, _pthread20,
        (pthread_t *thread, const pthread_attr_t *attr,
         void * (*start_routine)(void *), void *arg),
        (thread, attr, start_routine, arg),
        "pthread_create", "GLIBC_2.0", 0)

LIBHOOK(4, int, dopthread_create, _pthread21,
        (pthread_t *thread, const pthread_attr_t *attr,
         void * (*start_routine)(void *), void *arg),
        (thread, attr, start_routine, arg),
        "pthread_create", "GLIBC_2.1", 0)

// Data for this profiler module
static const size_t     N_TRACE_CACHE   = 2000000;
static const int        N_MODULES       = 8;
static const int        MAX_FNAME       = 1024;
static IgProfAtomic     s_enabled       = 0;
static bool             s_initialized   = false;
static bool             s_activated     = false;
static bool             s_pthreads      = false;
static volatile int     s_quitting      = 0;
static double           s_clockres      = 0;
static pthread_mutex_t  s_buflock       = PTHREAD_MUTEX_INITIALIZER;
static IgProfBufList    *s_buflist      = 0;
static IgProfTraceAlloc *s_bufs         = 0;
static IgProfTrace      *s_masterbuf    = 0;
static IgProfCallList   *s_threadinits  = 0;
static const char       *s_options      = 0;
static char             s_masterbufdata[sizeof(IgProfTrace)];
static pthread_t        s_mainthread;
static pthread_t        s_dumpthread;
static pthread_key_t    s_bufkey;
static pthread_key_t    s_flagkey;
static char             s_outname[MAX_FNAME];
static char             s_dumpflag[MAX_FNAME];

// Static data that needs to be constructed lazily on demand
static IgProfCallList &
threadinits(void)
{
  if (! s_threadinits)
    s_threadinits = new IgProfCallList;
  return *s_threadinits;
}

static IgProfBufList &
buflist(void)
{
  if (! s_buflist)
    s_buflist = new IgProfBufList;
  return *s_buflist;
}

static void
initBuf(IgProfTraceAlloc &info, bool perthread)
{
  info.perthread = perthread;
  info.buf = new IgProfTrace;
}

/** Dump out the profile data.  */
static void
dumpOneProfile(IgProfDumpInfo &info, IgProfTrace::Stack *frame)
{
  if (info.depth) // No address at root
  {
    IgProfSymCache::Symbol *sym = info.symcache->get(frame->address);

    if (sym->id >= 0)
      fprintf (info.output, "C%d FN%d+%ld", info.depth, sym->id, sym->symoffset);
    else
    {
      const char        *symname = sym->name;
      char              symgen[32];

      sym->id = info.nsyms++;

      if (! symname || ! *symname)
      {
        sprintf(symgen, "@?%p", sym->address);
        symname = symgen;
      }

      if (sym->binary->id >= 0)
        fprintf(info.output, "C%d FN%d=(F%d+%ld N=(%s))+%ld",
                info.depth, sym->id, sym->binary->id, sym->binoffset,
                symname, sym->symoffset);
      else
        fprintf(info.output, "C%d FN%d=(F%d=(%s)+%ld N=(%s))+%ld",
                info.depth, sym->id, (sym->binary->id = info.nlibs++),
                sym->binary->name ? sym->binary->name : "",
                sym->binoffset, symname, sym->symoffset);
    }

    for (IgProfTrace::Counter *ctr = frame->counters; ctr; ctr = ctr->next)
    {
      if (ctr->ticks || ctr->peak)
      {
        if (ctr->def->id >= 0)
          __extension__
          fprintf(info.output, " V%d:(%ju,%ju,%ju)",
                  ctr->def->id, ctr->ticks, ctr->value, ctr->peak);
        else
          __extension__
          fprintf(info.output, " V%d=(%s):(%ju,%ju,%ju)",
                  (ctr->def->id = info.nctrs++), ctr->def->name,
                  ctr->ticks, ctr->value, ctr->peak);

        for (IgProfTrace::Resource *res = ctr->resources; res; res = res->nextlive)
          __extension__
          fprintf(info.output, ";LK=(%p,%ju)", (void *) res->resource, res->size);
      }
    }
    fputc('\n', info.output);
  }

  info.depth++;
  for (frame = frame->children; frame; frame = frame->sibling)
    dumpOneProfile(info, frame);
  info.depth--;
}

/** Reset IDs used in dumping out profile data.  */
static void
dumpResetIDs(IgProfTrace::Stack *frame)
{
  for (IgProfTrace::Counter *ctr = frame->counters; ctr; ctr = ctr->next)
    ctr->def->id = -1;

  for (frame = frame->children; frame; frame = frame->sibling)
    dumpResetIDs(frame);
}

/** Utility function to dump out the profiler data from all current
    profile buffers: trace tree and live maps.  The strange calling
    convention is so this can be launched as a thread.  */
static void *
dumpAllProfiles(void *arg)
{
  IgProfDumpInfo *info = (IgProfDumpInfo *) arg;
  IgProfTrace::PerfStat &perf = info->perf;
  itimerval stopped = { { 0, 0 }, { 0, 0 } };
  itimerval prof, virt, real;
  sigset_t  sigmask;

  if (info->blocksig)
  {
    setitimer(ITIMER_PROF, &stopped, &prof);
    setitimer(ITIMER_VIRTUAL, &stopped, &virt);
    setitimer(ITIMER_REAL, &stopped, &real);

    sigset_t everything;
    sigfillset(&everything);
    pthread_sigmask(SIG_BLOCK, &everything, &sigmask);
  }

  char outname[MAX_FNAME];
  const char *tofile = info->tofile;
  if (! tofile || ! tofile[0])
  {
    const char *progname = program_invocation_name;
    const char *slash = strrchr(progname, '/');
    if (slash && slash[1])
      progname = slash+1;
    else if (slash)
      progname = "unnamed";

    timeval tv;
    gettimeofday(&tv, 0);
    sprintf(outname, "|gzip -c>igprof.%.100s.%ld.%f.gz",
            progname, (long) getpid(), tv.tv_sec + 1e-6*tv.tv_usec);
    tofile = outname;
  }

  igprof_debug("dumping state to %s\n", tofile);
  info->output = (tofile[0] == '|'
                  ? (unsetenv("LD_PRELOAD"), popen(tofile+1, "w"))
                  : fopen (tofile, "w+"));
  if (! info->output)
    igprof_debug("can't write to output %s: %s (error %d)\n",
                 tofile, strerror(errno), errno);
  else
  {
    fprintf(info->output, "P=(ID=%lu N=(%s) T=%f)\n",
            (unsigned long) getpid(), program_invocation_name, s_clockres);

    pthread_mutex_lock(&s_buflock);
    IgProfSymCache symcache;
    info->symcache = &symcache;
    IgProfBufList &bl = buflist();
    for (IgProfBufList::iterator i = bl.begin(), e = bl.end(); i != e; ++i)
      if (IgProfTraceAlloc *bufs = *i)
        for (int ib = 0; ib < N_MODULES; ++ib)
          if (IgProfTrace *buf = bufs[ib].buf)
          {
            buf->lock();
            dumpOneProfile(*info, buf->stackRoot());
            dumpResetIDs(buf->stackRoot());
	    perf += buf->perfStats();
            buf->unlock();
          }

    s_masterbuf->lock();
    dumpOneProfile(*info, s_masterbuf->stackRoot());
    dumpResetIDs(s_masterbuf->stackRoot());
    perf += s_masterbuf->perfStats();
    s_masterbuf->unlock();

    if (tofile[0] == '|')
      pclose(info->output);
    else
      fclose(info->output);
    pthread_mutex_unlock(&s_buflock);
  }

  if (info->blocksig)
  {
    setitimer(ITIMER_PROF, &prof, 0);
    setitimer(ITIMER_VIRTUAL, &virt, 0);
    setitimer(ITIMER_REAL, &real, 0);
    pthread_sigmask(SIG_SETMASK, &sigmask, 0);
  }

  double depthAvg = (1. * perf.sumDepth) / perf.ntraces;
  double ticksAvg = (1. * perf.sumTicks) / perf.ntraces;
  double tperdAvg = (1./16 * perf.sumTPerD) / perf.ntraces;
  igprof_debug("trace perf: ntraces=%.0f"
	       " depth=[av %.1f, rms %.1f]"
	       " ticks=[av %.1f, rms %.1f]"
	       " ticks-per-depth=[av %.1f, rms %.1f]\n",
               1. * perf.ntraces,
	       depthAvg, sqrt((1. * perf.sum2Depth) / perf.ntraces - depthAvg * depthAvg),
	       ticksAvg, sqrt((1. * perf.sum2Ticks) / perf.ntraces - ticksAvg * ticksAvg),
	       tperdAvg, sqrt((1./16/16 * perf.sum2TPerD) / perf.ntraces - tperdAvg * tperdAvg));
  return 0;
}

/** Thread generating in-flight profile data dumps.  Handles both
    external asynchronous and in-program synchronous dump requests.

    The dumps are generated from this separate, non-profiled thread,
    so that we can guarantee we will never attempt to lock the profile
    pool lock recursively in the same thread.  */
static void *
asyncDumpThread(void *)
{
  int dodump = 0;
  struct stat st;
  while (true)
  {
    // If we are done processing, quit.  Give threads max ~1s to quit.
    if (s_quitting && ++s_quitting > 100)
      break;

    // Check every once in a while if a dump has been requested.
    if (! (++dodump % 32) && ! stat(s_dumpflag, &st))
    {
      unlink(s_dumpflag);
      IgProfDumpInfo info = { 0, 0, 0, 0, s_outname, 0, 0, 1,
                              { 0, 0, 0, 0, 0, 0, 0 } };
      dumpAllProfiles(&info);
      dodump = 0;
    }

    // Have a nap.
    usleep (10000);
  }

  return 0;
}

extern "C" VISIBLE void
igprof_dump_now(const char *tofile)
{
  pthread_t tid;
  IgProfDumpInfo info = { 0, 0, 0, 0, tofile, 0, 0, 1,
                          { 0, 0, 0, 0, 0, 0, 0 } };
  pthread_create(&tid, 0, &dumpAllProfiles, &info);
  pthread_join(tid, 0);
}

/** Dump out profile data when application is about to exit. */
static void
exitDump(void *)
{
  if (! s_activated) return;
  igprof_debug("final exit in thread 0x%lx, saving profile data\n",
               (unsigned long) pthread_self());
  igprof_disable(true);
  s_activated = false;
  s_enabled = 0;
  s_quitting = 1;
  igprof_exit_thread(true);
  IgProfDumpInfo info = { 0, 0, 0, 0, s_outname, 0, 0, 0,
                          { 0, 0, 0, 0, 0, 0, 0 } };
  dumpAllProfiles(&info);
  igprof_debug("igprof quitting\n");
  s_initialized = false; // signal local data is unsafe to use
  s_pthreads = false; // make sure we no longer use threads stuff
}

// -------------------------------------------------------------------
/** Initialise the profiler core itself.  Prepares the the program
    for profiling.  Captures various exit points so we generate a
    dump before the program goes "out".  Automatically triggered
    to run on library load.  All profiler modules should invoke
    this method before doing their own initialisation.

    Returns @c true if profiling is activated in this process.  */
bool
igprof_init(int *moduleid, void (*threadinit)(void), bool perthread, double clockres)
{
  if (! s_initialized)
  {
    s_initialized = true;

    const char *options = igprof_options();
    if (! options || ! *options)
    {
      igprof_debug("$IGPROF not set, not profiling this process\n");
      return s_activated = false;
    }

    for (const char *opts = options; *opts; )
    {
      while (*opts == ' ' || *opts == ',')
        ++opts;

      if (! strncmp(opts, "igprof:out='", 12))
      {
        int i = 0;
        opts += 12;
        while (i < MAX_FNAME-1 && *opts && *opts != '\'')
          s_outname[i++] = *opts++;
        s_outname[i] = 0;
      }
      else if (! strncmp(opts, "igprof:dump='", 13))
      {
        int i = 0;
        opts += 13;
        while (i < MAX_FNAME-1 && *opts && *opts != '\'')
          s_dumpflag[i++] = *opts++;
        s_dumpflag[i] = 0;
      }
      else
        opts++;

      while (*opts && *opts != ',' && *opts != ' ')
        opts++;
    }

    // Install exit handler to generate actual dump.
    abi::__cxa_atexit(&exitDump, 0, 0);

    s_bufs = new IgProfTraceAlloc[N_MODULES];
    memset(s_bufs, 0, N_MODULES * sizeof(*s_bufs));
    buflist().push_back(s_bufs);

    void *program = dlopen(0, RTLD_NOW);
    if (program && dlsym(program, "pthread_create"))
    {
      s_pthreads = true;
      pthread_key_create(&s_bufkey, 0);
      pthread_key_create(&s_flagkey, 0);
      pthread_setspecific(s_flagkey, new IgProfAtomic(1));
    }
    dlclose(program);

    const char *target = getenv("IGPROF_TARGET");
    s_mainthread = pthread_self();
    if (target && ! strstr(program_invocation_name, target))
    {
      igprof_debug("Current process not selected for profiling:"
                   " process '%s' does not match '%s'\n",
                   program_invocation_name, target);
      return s_activated = false;
    }

    igprof_debug("Activated in %s, %s, main thread id 0x%lx\n",
                 program_invocation_name,
                 s_pthreads ? "multi-threaded" : "no threads",
                 s_mainthread);
    igprof_debug("Options: %s\n", options);

    IgHook::hook(doexit_hook_main.raw);
    IgHook::hook(doexit_hook_main2.raw);
    IgHook::hook(dokill_hook_main.raw);
#if __linux
    if (doexit_hook_main.raw.chain)  IgHook::hook(doexit_hook_libc.raw);
    if (doexit_hook_main2.raw.chain) IgHook::hook(doexit_hook_libc2.raw);
    if (dokill_hook_main.raw.chain)  IgHook::hook(dokill_hook_libc.raw);
#endif
    if (s_pthreads)
    {
      if (s_dumpflag[0])
        pthread_create (&s_dumpthread, 0, &asyncDumpThread, 0);

      IgHook::hook(dopthread_create_hook_main.raw);
#if __linux
      IgHook::hook(dopthread_create_hook_pthread20.raw);
      IgHook::hook(dopthread_create_hook_pthread21.raw);
#endif
    }
    s_activated = true;
    s_enabled = 1;
  }

  if (! s_activated)
    return false;

  if (! moduleid)
    return true;

  igprof_disable(true);

  if (clockres > 0)
  {
    igprof_debug("Timing resolution set to %f\n", clockres);
    s_clockres = clockres;
  }

  if (! s_masterbuf)
    s_masterbuf = new (s_masterbufdata) IgProfTrace;

  int modid;
  for (modid = 0; modid < N_MODULES; ++modid)
    if (! s_bufs[modid].buf)
    {
      initBuf(s_bufs[modid], perthread);
      *moduleid = modid;
      break;
    }

  if (modid == N_MODULES)
  {
    igprof_debug("Too many profilers enabled (%d), please"
                  " rebuild IgProf with larger N_MODULES\n",
                  N_MODULES);
    abort ();
  }

  if (threadinit)
    threadinits().push_back(threadinit);

  igprof_enable(true);
  return true;
}

/** Return @c true if the process was linked against threading package.  */
bool
igprof_is_multi_threaded(void)
{ return s_pthreads; }

/** Setup a thread so it can be used in profiling.  This should be
    called for every thread that will participate in profiling.  */
static void
igprof_init_thread(void)
{
  IgProfTraceAlloc *bufs = new IgProfTraceAlloc[N_MODULES];
  memset(bufs, 0, N_MODULES * sizeof(*bufs));
  pthread_setspecific(s_bufkey, bufs);

  IgProfAtomic *enabled = new IgProfAtomic(1);
  pthread_setspecific(s_flagkey, enabled);

  for (int i = 0; i < N_MODULES && s_bufs[i].buf; ++i)
    if (s_bufs[i].perthread)
      initBuf(bufs[i], true);

  pthread_mutex_lock(&s_buflock);
  buflist().push_back(bufs);
  pthread_mutex_unlock(&s_buflock);
}

/** Finalise a thread.  */
static void
igprof_exit_thread(bool final)
{
  if (! s_pthreads && ! final)
    return;

  itimerval stopped = { { 0, 0 }, { 0, 0 } };
  setitimer(ITIMER_PROF, &stopped, 0);
  setitimer(ITIMER_VIRTUAL, &stopped, 0);
  setitimer(ITIMER_REAL, &stopped, 0);

  pthread_t thread = pthread_self ();
  IgProfTraceAlloc *bufs
    = (thread == s_mainthread ? s_bufs
       : (IgProfTraceAlloc *) pthread_getspecific(s_bufkey));

  pthread_mutex_lock(&s_buflock);
  IgProfBufList &bl = buflist();
  IgProfBufList::iterator ibuf = std::find(bl.begin(), bl.end(), bufs);
  if (ibuf != bl.end())
    bl.erase(ibuf);
  pthread_mutex_unlock(&s_buflock);

  for (int i = 0; i < N_MODULES && bufs; ++i)
  {
    IgProfTrace *p = bufs[i].buf;
    if (p)
    {
      s_masterbuf->mergeFrom(*p);
      bufs[i].buf = 0;
      delete p;
    }
  }

  if (thread == s_mainthread)
    s_bufs = 0;
  else
    pthread_setspecific(s_bufkey, 0);

  delete [] bufs;
}

/** Return a profile buffer for a profiler in the current thread.  It
    is safe to call this function from any thread and in asynchronous
    signal handlers at any time.  Returns the buffer to use or a null
    to indicate no data should be gathered in the calling context, for
    example if the profile core itself has already been destroyed.  */
IgProfTrace *
igprof_buffer(int moduleid)
{
  // Check which pool to return.  We return the one from s_bufs in
  // non-threaded applications and always in the main thread.  In
  // other threads we return the main thread buffer if a single buffer
  // was requested, otherwise a per-thread buffer.
  pthread_t thread = pthread_self();
  IgProfTraceAlloc *bufs = s_bufs;
  if (! s_activated)
    bufs = 0;
  else if (thread != s_mainthread && s_bufs[moduleid].perthread)
    bufs = (IgProfTraceAlloc *) pthread_getspecific(s_bufkey);

  return bufs ? bufs[moduleid].buf : 0;
}

/** Check if the profiler is currently enabled.  This function should
    be called by asynchronous signal handlers to check whether they
    should record profile data -- not for the actual running where
    the value of the flag has little useful value, but to make sure
    no data is gathered after the system has started to close down.  */
bool
igprof_is_enabled(bool globally)
{
  if (! globally && s_pthreads)
  {
    IgProfAtomic *flag = (IgProfAtomic *) pthread_getspecific(s_flagkey);
    return flag ? (*flag > 0 && s_enabled > 0) : false;
  }
  else
    return s_enabled > 0;
}

/** Enable the profiling system, either globally or just in this
    thread.  This is safe to call from anywhere.  Returns @c true if
    the profiler is enabled after the call. */
bool
igprof_enable(bool globally)
{
  if (! globally && s_pthreads)
  {
    IgProfAtomic *flag = (IgProfAtomic *) pthread_getspecific(s_flagkey);
    return flag ? (IgProfAtomicInc(flag) > 0 && s_enabled > 0) : false;
  }
  else
  {
    IgProfAtomic newval = IgProfAtomicInc(&s_enabled);
    return newval > 0;
  }
}

/** Disable the profiling system, either globally or just in this
    thread.  This is safe to call from anywhere.  Returns @c true if
    the profiler was enabled before the call.  */
bool
igprof_disable(bool globally)
{
  if (! globally && s_pthreads)
  {
    IgProfAtomic *flag = (IgProfAtomic *) pthread_getspecific(s_flagkey);
    return flag ? (IgProfAtomicDec(flag) >= 0 && s_enabled > 0) : false;
  }
  else
  {
    IgProfAtomic newval = IgProfAtomicDec(&s_enabled);
    return newval >= 0;
  }
}

/** Get user-provided profiling options.  */
const char *
igprof_options(void)
{
  if (! s_options)
    s_options = getenv("IGPROF");
  return s_options;
}

/** Internal assertion helper routine.  */
int
igprof_panic(const char *file, int line, const char *func, const char *expr)
{
  igprof_disable(true);

  fprintf (stderr, "%s: %s:%d: %s: assertion failure: %s\n",
           program_invocation_name, file, line, func, expr);

  void *trace [128];
  int levels = IgHookTrace::stacktrace(trace, 128);
  for (int i = 2; i < levels; ++i)
  {
    const char  *sym = 0;
    const char  *lib = 0;
    long        offset = 0;
    long        liboffset = 0;

    IgHookTrace::symbol(trace[i], sym, lib, offset, liboffset);
    fprintf(stderr, "  %p %s %s %ld [%s %s %ld]\n",
            trace[i], sym ? sym : "?",
            (offset < 0 ? "-" : "+"), labs(offset), lib ? lib : "?",
            (liboffset < 0 ? "-" : "+"), labs(liboffset));
  }

  // abort();
  return 1;
}

/** Internal printf()-like debugging utility.  Produces output if
    $IGPROF_DEBUGGING environment variable is set to any value.  */
void
igprof_debug(const char *format, ...)
{
  static const char *debugging = getenv("IGPROF_DEBUGGING");
  if (debugging)
  {
    timeval tv;
    gettimeofday(&tv, 0);
    fprintf(stderr, "*** IgProf(%lu, %.3f): ",
            (unsigned long) getpid(),
            tv.tv_sec + 1e-6*tv.tv_usec);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
  }
}

// -------------------------------------------------------------------
/** A wrapper for starting user threads to enable profiling.  */
static void *
threadWrapper(void *arg)
{
  // Get arguments.
  IgProfWrappedArg *wrapped = (IgProfWrappedArg *) arg;
  void *(*start_routine)(void*) = wrapped->start_routine;
  void *start_arg = wrapped->arg;
  delete wrapped;

  // Report the thread and enable per-thread profiling pools.
  if (s_activated)
  {
    // Removing the __extension__ gives a warning which
    // is acknowledged as a language problem in the C++ Standard Core
    // Language Defect Report
    //
    // http://www.open-std.org/jtc1/sc22/wg21/docs/cwg_defects.html#195
    //
    // since the suggested decision seems to be that the syntax should
    // actually be "Conditionally-Supported Behavior" in some
    // future C++ standard I simply silence the warning.
    __extension__
    igprof_debug("captured thread id 0x%lx for profiling (%p, %p)\n",
                 (unsigned long) pthread_self(),
                 (void *)(start_routine),
                 start_arg);

    igprof_init_thread();
  }

  // Make sure we've called stack trace code at least once in
  // this thread before the profile signal hits.
  void *dummy = 0; IgHookTrace::stacktrace(&dummy, 1);

  // Run per-profiler initialisation.
  if (s_activated)
  {
    IgProfCallList                      &l = threadinits();
    IgProfCallList::reverse_iterator    i = l.rbegin();
    IgProfCallList::reverse_iterator    end = l.rend();
    for ( ; i != end; ++i)
      (*i)();
  }

  // Run the user thread.
  void *ret = (*start_routine)(start_arg);

  // Harvest thread profile result.
  if (s_activated)
  {
    // Removing the __extension__ gives a warning which
    // is acknowledged as a language problem in the C++ Standard Core
    // Language Defect Report
    //
    // http://www.open-std.org/jtc1/sc22/wg21/docs/cwg_defects.html#195
    //
    // since the suggested decision seems to be that the syntax should
    // actually be "Conditionally-Supported Behavior" in some
    // future C++ standard I simply silence the warning.
    __extension__
    igprof_debug("leaving thread id 0x%lx from profiling (%p, %p)\n",
                 (unsigned long) pthread_self(),
                 (void *) start_routine, start_arg);
    igprof_exit_thread(false);
  }
  return ret;
}

/** Trap thread creation to run per-profiler initialisation.  */
static int
dopthread_create(IgHook::SafeData<igprof_dopthread_create_t> &hook,
                 pthread_t *thread,
                 const pthread_attr_t *attr,
                 void * (*start_routine)(void *),
                 void *arg)
{
  size_t stack = 0;
  if (attr && pthread_attr_getstacksize(attr, &stack) == 0 && stack < 64*1024)
  {
    igprof_debug("pthread_create requests too small stack %lu -> use 64kB\n",
		 (unsigned long) stack);
    pthread_attr_setstacksize((pthread_attr_t *) attr, 64*1024);
  }

  if (start_routine == dumpAllProfiles)
    return hook.chain(thread, attr, start_routine, arg);
  else
  {
    // Pass the actual arguments to our wrapper in a temporary memory
    // structure.  We need to hide the creation from memory profiler
    // in case it's running concurrently with this profiler.
    igprof_disable(false);
    IgProfWrappedArg *wrapped = new IgProfWrappedArg;
    wrapped->start_routine = start_routine;
    wrapped->arg = arg;
    igprof_enable(false);
    return hook.chain(thread, attr, &threadWrapper, wrapped);
  }
}

/** Trapped calls to exit() and _exit().  */
static void
doexit(IgHook::SafeData<igprof_doexit_t> &hook, int code)
{
  // Force the merge of per-thread profile tree into the main tree
  // if a thread calls exit().  Then forward the call.
  pthread_t thread = pthread_self();
  igprof_debug("%s(%d) called in thread id 0x%lx\n",
               hook.function, code, (unsigned long) thread);
#if 0
  if (s_pthreads && thread != s_mainthread)
  {
    igprof_disable(true);
    igprof_exit_thread(false);
    igprof_enable(true);
  }
#endif
  hook.chain (code);
}

/** Trapped calls to kill().  Dump out profiler data if the signal
    looks dangerous.  Mostly really to trap calls to abort().  */
static int
dokill(IgHook::SafeData<igprof_dokill_t> &hook, pid_t pid, int sig)
{
  if ((pid == 0 || pid == getpid())
      && (sig == SIGHUP || sig == SIGINT || sig == SIGQUIT
          || sig == SIGILL || sig == SIGABRT || sig == SIGFPE
          || sig == SIGKILL || sig == SIGSEGV || sig == SIGPIPE
          || sig == SIGALRM || sig == SIGTERM || sig == SIGUSR1
          || sig == SIGUSR2 || sig == SIGBUS || sig == SIGIOT))
  {
    bool enabled = igprof_disable(true);
    if (enabled)
    {
      igprof_debug("kill(%d,%d) called, dumping state\n", (int) pid, sig);
      IgProfDumpInfo info = { 0, 0, 0, 0, s_outname, 0, 0, 0,
                              { 0, 0, 0, 0, 0, 0, 0 } };
      dumpAllProfiles(&info);
    }
    igprof_enable(true);
  }
  return hook.chain (pid, sig);
}

// -------------------------------------------------------------------
static bool autoboot = igprof_init(0, 0, false);