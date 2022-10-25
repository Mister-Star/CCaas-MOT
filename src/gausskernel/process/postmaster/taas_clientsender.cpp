#include "postgres.h"
#include "knl/knl_variable.h"

#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
// #include "postmaster/walwriter.h"
#include "postmaster/client.h"
#include "storage/buf/bufmgr.h"
#include "storage/ipc.h"
#include "storage/lock/lwlock.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

#include "gssignal/gs_signal.h"

#include "storage/mot/mot_fdw.h"
#include <thread>
#include <sstream>
#include <iostream>
#include "postmaster/postmaster.h"
std::vector<uint64_t> client_sender_thread_ids;
#define NANOSECONDS_PER_MILLISECOND 1000000L
#define NANOSECONDS_PER_SECOND 1000000000L

/* Signal handlers */
static void _quickdie(SIGNAL_ARGS);
static void _SigHupHandler(SIGNAL_ARGS);
static void _ShutdownHandler(SIGNAL_ARGS);
static void _sigusr1_handler(SIGNAL_ARGS);

THR_LOCAL const int g_sleep_timeout_ms = 300; 

// overwrite base on walwriter

void ClientSenderMain(void)
{
    sigjmp_buf local_sigjmp_buf;
    MemoryContext clientsender_context;
    sigset_t old_sig_mask;

    (void)gspqsignal(SIGHUP, _SigHupHandler);    /* set flag to read config file */
    (void)gspqsignal(SIGINT, _ShutdownHandler);  /* request shutdown */
    (void)gspqsignal(SIGTERM, _ShutdownHandler); /* request shutdown */
    (void)gspqsignal(SIGQUIT, _quickdie);       /* hard crash time */
    (void)gspqsignal(SIGALRM, SIG_IGN);
    (void)gspqsignal(SIGPIPE, SIG_IGN);
    (void)gspqsignal(SIGUSR1, _sigusr1_handler);
    (void)gspqsignal(SIGUSR2, SIG_IGN); /* not used */
    (void)gspqsignal(SIGCHLD, SIG_DFL);
    (void)gspqsignal(SIGTTIN, SIG_DFL);
    (void)gspqsignal(SIGTTOU, SIG_DFL);
    (void)gspqsignal(SIGCONT, SIG_DFL);
    (void)gspqsignal(SIGWINCH, SIG_DFL);

    sigdelset(&t_thrd.libpq_cxt.BlockSig, SIGQUIT);

    t_thrd.utils_cxt.CurrentResourceOwner = ResourceOwnerCreate(NULL, "client sender", MEMORY_CONTEXT_STORAGE);

    clientsender_context = AllocSetContextCreate(t_thrd.top_mem_cxt,
        "client sender",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE);
    (void)MemoryContextSwitchTo(clientsender_context);

    int curTryCounter;
    int* oldTryCounter = NULL;
    if (sigsetjmp(local_sigjmp_buf, 1) != 0) {
        gstrace_tryblock_exit(true, oldTryCounter);
        pthread_sigmask(SIG_SETMASK, &old_sig_mask, NULL);
        t_thrd.log_cxt.error_context_stack = NULL;
        HOLD_INTERRUPTS();
        EmitErrorReport();
        AbortAsyncListIO();
        LWLockReleaseAll();
        pgstat_report_waitevent(WAIT_EVENT_END);
        AbortBufferIO();
        UnlockBuffers();
        ResourceOwnerRelease(t_thrd.utils_cxt.CurrentResourceOwner, RESOURCE_RELEASE_BEFORE_LOCKS, false, true);
        AtEOXact_Buffers(false);
        AtEOXact_SMgr();
        AtEOXact_Files();
        AtEOXact_HashTables(false);
        (void)MemoryContextSwitchTo(clientsender_context);
        FlushErrorState();
        MemoryContextResetAndDeleteChildren(clientsender_context);
        RESUME_INTERRUPTS();
        pg_usleep(1000000L);
        smgrcloseall();
    }
    oldTryCounter = gstrace_tryblock_entry(&curTryCounter);

    t_thrd.log_cxt.PG_exception_stack = &local_sigjmp_buf;

    gs_signal_setmask(&t_thrd.libpq_cxt.UnBlockSig, NULL);
    (void)gs_signal_unblock_sigusr2();

    SetWalWriterSleeping(false);

    pgstat_report_appname("client sender");
    pgstat_report_activity(STATE_IDLE, NULL);

    uint64_t id = 0;
    std::stringstream thread_id_stream;
    thread_id_stream << std::this_thread::get_id();
    uint64_t thread_id = std::stoull(thread_id_stream.str());
    for(auto &i : client_sender_thread_ids ){
        if(i == thread_id){
            break;
        }
        id++;
    }
    for (;;) {
        pgstat_report_activity(STATE_RUNNING, NULL);

        if (t_thrd.clientsender_cxt.got_SIGHUP) {
            t_thrd.clientsender_cxt.got_SIGHUP = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        if (t_thrd.clientsender_cxt.shutdown_requested) {
            proc_exit(0);
        }
        FDWClientSenderThreadMain(id);

        pgstat_report_activity(STATE_IDLE, NULL);
    }
}

/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */
static void _quickdie(SIGNAL_ARGS)
{
    gs_signal_setmask(&t_thrd.libpq_cxt.BlockSig, NULL);
    on_exit_reset();
    exit(2);
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void _SigHupHandler(SIGNAL_ARGS)
{
    int save_errno = errno;
    t_thrd.clientsender_cxt.got_SIGHUP = true;
    if (t_thrd.proc)
        SetLatch(&t_thrd.proc->procLatch);
    errno = save_errno;
}

/* SIGTERM: set flag to exit normally */
static void _ShutdownHandler(SIGNAL_ARGS)
{
    int save_errno = errno;
    t_thrd.clientsender_cxt.shutdown_requested = true;
    if (t_thrd.proc)
        SetLatch(&t_thrd.proc->procLatch);
    errno = save_errno;
}

/* SIGUSR1: used for latch wakeups */
static void _sigusr1_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    latch_sigusr1_handler();
    errno = save_errno;
}
