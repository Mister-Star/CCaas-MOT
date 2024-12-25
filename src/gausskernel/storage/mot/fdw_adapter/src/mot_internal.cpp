/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * mot_internal.cpp
 *    MOT Foreign Data Wrapper internal interfaces to the MOT engine.
 *
 * IDENTIFICATION
 *    src/gausskernel/storage/mot/fdw_adapter/src/mot_internal.cpp
 *
 * -------------------------------------------------------------------------
 */
#include <google/protobuf/io/gzip_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>

#include <ostream>
#include <istream>
#include <iomanip>
#include <pthread.h>
#include <cstring>
#include "transaction.pb.h"
#include "node.pb.h"
#include "client.pb.h"
#include "server.pb.h"
#include "storage.pb.h"
#include "message.pb.h"
#include "postgres.h"
#include "access/dfs/dfs_query.h"
#include "access/sysattr.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/nodeFuncs.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "utils/syscache.h"
#include "executor/executor.h"
#include "storage/ipc.h"
#include "commands/dbcommands.h"
#include "knl/knl_session.h"

#include "mot_internal.h"
#include "row.h"
#include "log_statistics.h"
#include "spin_lock.h"
#include "txn.h"
#include "table.h"
#include "utilities.h"
#include "mot_engine.h"
#include "sentinel.h"
#include "txn.h"
#include "txn_access.h"
#include "index_factory.h"
#include "column.h"
#include "mm_raw_chunk_store.h"
#include "ext_config_loader.h"
#include "config_manager.h"
#include "mot_error.h"
#include "utilities.h"
#include "jit_context.h"
#include "mm_cfg.h"
#include "jit_statistics.h"
#include "gaussdb_config_loader.h"
#include <cstdio>
#include <stdio.h>
#include <iostream>
#include "string"
#include "sstream"
#include <fstream>
#include <atomic>
#include <sys/time.h>
// #include "neu_concurrency_tools/blockingconcurrentqueue.h"
// #include "neu_concurrency_tools/blocking_mpmc_queue.h"
// #include "neu_concurrency_tools/ThreadPool.h"
// #include "epoch_merge.h"
#include "zmq.hpp"
#include "zmq.h"
#include <sched.h>
#include "utils/timestamp.h"
#include "postmaster/postmaster.h"

#include "postmaster/tinyxml2.h"

#include <lz4.h>

int TaaS_Start();

/** @define masks for CSN word   */
#define CSN_BITS 0x1FFFFFFFFFFFFFFFUL

#define STATUS_BITS 0xE000000000000000UL

#define IS_CHAR_TYPE(oid) (oid == VARCHAROID || oid == BPCHAROID || oid == TEXTOID || oid == CLOBOID || oid == BYTEAOID)
#define IS_INT_TYPE(oid)                                                                                           \
    (oid == BOOLOID || oid == CHAROID || oid == INT8OID || oid == INT2OID || oid == INT4OID || oid == FLOAT4OID || \
        oid == FLOAT8OID || oid == INT1OID || oid == DATEOID || oid == TIMEOID || oid == TIMESTAMPOID ||           \
        oid == TIMESTAMPTZOID)

MOT::MOTEngine* MOTAdaptor::m_engine = nullptr;
static XLOGLogger xlogger;

// enable MOT Engine logging facilities
DECLARE_LOGGER(InternalExecutor, FDW)

/** @brief on_proc_exit() callback for cleaning up current thread - only when thread pool is ENABLED. */
static void MOTCleanupThread(int status, Datum ptr);

/** @brief Helper for cleaning up all JIT context objects stored in all CachedPlanSource of the current session. */
static void DestroySessionJitContexts();

// in a thread-pooled environment we need to ensure thread-locals are initialized properly
static inline void EnsureSafeThreadAccessInline()
{
    if (MOTCurrThreadId == INVALID_THREAD_ID) {
        MOT_LOG_DEBUG("Initializing safe thread access for current thread");
        MOT::AllocThreadId();
        // register for cleanup only once - not having a current thread id is the safe indicator we never registered
        // proc-exit callback for this thread
        if (g_instance.attr.attr_common.enable_thread_pool) {
            on_proc_exit(MOTCleanupThread, PointerGetDatum(nullptr));
            MOT_LOG_DEBUG("Registered current thread for proc-exit callback for thread %p", (void*)pthread_self());
        }
    }
    if (MOTCurrentNumaNodeId == MEM_INVALID_NODE) {
        MOT::InitCurrentNumaNodeId();
    }
    MOT::InitMasstreeThreadinfo();
}

extern void EnsureSafeThreadAccess()
{
    EnsureSafeThreadAccessInline();
}

static void DestroySession(MOT::SessionContext* sessionContext)
{
    MOT_ASSERT(MOTAdaptor::m_engine);
    MOT_LOG_DEBUG("Destroying session context %p, connection_id %u", sessionContext, sessionContext->GetConnectionId());

    if (u_sess->mot_cxt.jit_session_context_pool) {
        JitExec::FreeSessionJitContextPool(u_sess->mot_cxt.jit_session_context_pool);
    }
    MOT::GetSessionManager()->DestroySessionContext(sessionContext);
}

// Global map of PG session identification (required for session statistics)
// This approach is safer than saving information in the session context
static pthread_spinlock_t sessionDetailsLock;
typedef std::map<MOT::SessionId, pair<::ThreadId, pg_time_t>> SessionDetailsMap;
static SessionDetailsMap sessionDetailsMap;

static void InitSessionDetailsMap()
{
    pthread_spin_init(&sessionDetailsLock, 0);
}

static void DestroySessionDetailsMap()
{
    pthread_spin_destroy(&sessionDetailsLock);
}

static void RecordSessionDetails()
{
    MOT::SessionId sessionId = u_sess->mot_cxt.session_id;
    if (sessionId != INVALID_SESSION_ID) {
        pthread_spin_lock(&sessionDetailsLock);
        sessionDetailsMap.emplace(sessionId, std::make_pair(t_thrd.proc->pid, t_thrd.proc->myStartTime));
        pthread_spin_unlock(&sessionDetailsLock);
    }
}

static void ClearSessionDetails(MOT::SessionId sessionId)
{
    if (sessionId != INVALID_SESSION_ID) {
        pthread_spin_lock(&sessionDetailsLock);
        SessionDetailsMap::iterator itr = sessionDetailsMap.find(sessionId);
        if (itr != sessionDetailsMap.end()) {
            sessionDetailsMap.erase(itr);
        }
        pthread_spin_unlock(&sessionDetailsLock);
    }
}

inline void ClearCurrentSessionDetails()
{
    ClearSessionDetails(u_sess->mot_cxt.session_id);
}

static void GetSessionDetails(MOT::SessionId sessionId, ::ThreadId* gaussSessionId, pg_time_t* sessionStartTime)
{
    // although we have the PGPROC in the user data of the session context, we prefer not to use
    // it due to safety (in some unknown constellation we might hold an invalid pointer)
    // it is much safer to save a copy of the two required fields
    pthread_spin_lock(&sessionDetailsLock);
    SessionDetailsMap::iterator itr = sessionDetailsMap.find(sessionId);
    if (itr != sessionDetailsMap.end()) {
        *gaussSessionId = itr->second.first;
        *sessionStartTime = itr->second.second;
    }
    pthread_spin_unlock(&sessionDetailsLock);
}

// provide safe session auto-cleanup in case of missing session closure
// This mechanism relies on the fact that when a session ends, eventually its thread is terminated
// ATTENTION: in thread-pooled envelopes this assumption no longer holds true, since the container thread keeps
// running after the session ends, and a session might run each time on a different thread, so we
// disable this feature, instead we use this mechanism to generate thread-ended event into the MOT Engine
static pthread_key_t sessionCleanupKey;

static void SessionCleanup(void* key)
{
    MOT_ASSERT(!g_instance.attr.attr_common.enable_thread_pool);

    // in order to ensure session-id cleanup for session 0 we use positive values
    MOT::SessionId sessionId = (MOT::SessionId)(((uint64_t)key) - 1);
    if (sessionId != INVALID_SESSION_ID) {
        MOT_LOG_WARN("Encountered unclosed session %u (missing call to DestroyTxn()?)", (unsigned)sessionId);
        ClearSessionDetails(sessionId);
        MOT_LOG_DEBUG("SessionCleanup(): Calling DestroySessionJitContext()");
        DestroySessionJitContexts();
        if (MOTAdaptor::m_engine) {
            MOT::SessionContext* sessionContext = MOT::GetSessionManager()->GetSessionContext(sessionId);
            if (sessionContext != nullptr) {
                DestroySession(sessionContext);
            }
            // since a call to on_proc_exit(destroyTxn) was probably missing, we should also cleanup thread-locals
            // pay attention that if we got here it means the thread pool is disabled, so we must ensure thread-locals
            // are cleaned up right now. Due to these complexities, onCurrentThreadEnding() was designed to be proof
            // for repeated calls.
            MOTAdaptor::m_engine->OnCurrentThreadEnding();
        }
    }
}

static void InitSessionCleanup()
{
    pthread_key_create(&sessionCleanupKey, SessionCleanup);
}

static void DestroySessionCleanup()
{
    pthread_key_delete(sessionCleanupKey);
}

static void ScheduleSessionCleanup()
{
    pthread_setspecific(sessionCleanupKey, (const void*)(uint64_t)(u_sess->mot_cxt.session_id + 1));
}

static void CancelSessionCleanup()
{
    pthread_setspecific(sessionCleanupKey, nullptr);
}

static GaussdbConfigLoader* gaussdbConfigLoader = nullptr;

bool MOTAdaptor::m_initialized = false;
bool MOTAdaptor::m_callbacks_initialized = false;

static void WakeupWalWriter()
{
    if (g_instance.proc_base->walwriterLatch != nullptr) {
        SetLatch(g_instance.proc_base->walwriterLatch);
    }
}

void MOTAdaptor::Init()
{
    if (m_initialized) {
        // This is highly unexpected, and should especially be guarded in scenario of switch-over to standby.
        elog(FATAL, "Double attempt to initialize MOT engine, it is already initialized");
    }

    m_engine = MOT::MOTEngine::CreateInstanceNoInit(g_instance.attr.attr_common.MOTConfigFileName, 0, nullptr);
    if (m_engine == nullptr) {
        elog(FATAL, "Failed to create MOT engine");
    }

    MOT::MOTConfiguration& motCfg = MOT::GetGlobalConfiguration();
    motCfg.SetTotalMemoryMb(g_instance.attr.attr_memory.max_process_memory / KILO_BYTE);

    gaussdbConfigLoader = new (std::nothrow) GaussdbConfigLoader();
    if (gaussdbConfigLoader == nullptr) {
        MOT::MOTEngine::DestroyInstance();
        elog(FATAL, "Failed to allocate memory for GaussDB/MOTEngine configuration loader.");
    }
    MOT_LOG_TRACE("Adding external configuration loader for GaussDB");
    if (!m_engine->AddConfigLoader(gaussdbConfigLoader)) {
        delete gaussdbConfigLoader;
        gaussdbConfigLoader = nullptr;
        MOT::MOTEngine::DestroyInstance();
        elog(FATAL, "Failed to add GaussDB/MOTEngine configuration loader");
    }

    if (!m_engine->LoadConfig()) {
        m_engine->RemoveConfigLoader(gaussdbConfigLoader);
        delete gaussdbConfigLoader;
        gaussdbConfigLoader = nullptr;
        MOT::MOTEngine::DestroyInstance();
        elog(FATAL, "Failed to load configuration for MOT engine.");
    }

    // Check max process memory here - we do it anyway to protect ourselves from miscalculations.
    // Attention: the following values are configured during the call to MOTEngine::LoadConfig() just above
    uint64_t globalMemoryKb = MOT::g_memGlobalCfg.m_maxGlobalMemoryMb * KILO_BYTE;
    uint64_t localMemoryKb = MOT::g_memGlobalCfg.m_maxLocalMemoryMb * KILO_BYTE;
    uint64_t maxReserveMemoryKb = globalMemoryKb + localMemoryKb;

    // check whether the 2GB gap between MOT and envelope is still kept
    if ((g_instance.attr.attr_memory.max_process_memory < (int32)maxReserveMemoryKb) ||
        ((g_instance.attr.attr_memory.max_process_memory - maxReserveMemoryKb) < MIN_DYNAMIC_PROCESS_MEMORY)) {
        // we allow one extreme case: GaussDB is configured to its limit, and zero memory is left for us
        if (maxReserveMemoryKb <= motCfg.MOT_MIN_MEMORY_USAGE_MB * KILO_BYTE) {
            MOT_LOG_WARN("Allowing MOT to work in minimal memory mode");
        } else {
            m_engine->RemoveConfigLoader(gaussdbConfigLoader);
            delete gaussdbConfigLoader;
            gaussdbConfigLoader = nullptr;
            MOT::MOTEngine::DestroyInstance();
            elog(FATAL,
                "The value of pre-reserved memory for MOT engine is not reasonable: "
                "Request for a maximum of %" PRIu64 " KB global memory, and %" PRIu64
                " KB session memory (total of %" PRIu64 " KB) is invalid since max_process_memory is %u KB",
                globalMemoryKb,
                localMemoryKb,
                maxReserveMemoryKb,
                g_instance.attr.attr_memory.max_process_memory);
        }
    }

    if (!m_engine->Initialize()) {
        m_engine->RemoveConfigLoader(gaussdbConfigLoader);
        delete gaussdbConfigLoader;
        gaussdbConfigLoader = nullptr;
        MOT::MOTEngine::DestroyInstance();
        elog(FATAL, "Failed to initialize MOT engine.");
    }

    if (!JitExec::JitStatisticsProvider::CreateInstance()) {
        m_engine->RemoveConfigLoader(gaussdbConfigLoader);
        delete gaussdbConfigLoader;
        gaussdbConfigLoader = nullptr;
        MOT::MOTEngine::DestroyInstance();
        elog(FATAL, "Failed to initialize JIT statistics.");
    }

    // make sure current thread is cleaned up properly when thread pool is enabled
    EnsureSafeThreadAccessInline();

    if (motCfg.m_enableRedoLog && motCfg.m_loggerType == MOT::LoggerType::EXTERNAL_LOGGER) {
        m_engine->GetRedoLogHandler()->SetLogger(&xlogger);
        m_engine->GetRedoLogHandler()->SetWalWakeupFunc(WakeupWalWriter);
    }

    InitSessionDetailsMap();
    if (!g_instance.attr.attr_common.enable_thread_pool) {
        InitSessionCleanup();
    }
    InitDataNodeId();
    InitKeyOperStateMachine();
    TaaS_Start();
    m_initialized = true;
}

void MOTAdaptor::NotifyConfigChange()
{
    if (gaussdbConfigLoader != nullptr) {
        gaussdbConfigLoader->MarkChanged();
    }
}

void MOTAdaptor::InitDataNodeId()
{
    MOT::GetGlobalConfiguration().SetPgNodes(1, 1);
}

void MOTAdaptor::Destroy()
{
    if (!m_initialized) {
        return;
    }

    JitExec::JitStatisticsProvider::DestroyInstance();
    if (!g_instance.attr.attr_common.enable_thread_pool) {
        DestroySessionCleanup();
    }
    DestroySessionDetailsMap();
    if (gaussdbConfigLoader != nullptr) {
        m_engine->RemoveConfigLoader(gaussdbConfigLoader);
        delete gaussdbConfigLoader;
        gaussdbConfigLoader = nullptr;
    }

    EnsureSafeThreadAccessInline();
    MOT::MOTEngine::DestroyInstance();
    m_engine = nullptr;
    knl_thread_mot_init();  // reset all thread-locals, mandatory for standby switch-over
    m_initialized = false;
}

MOT::TxnManager* MOTAdaptor::InitTxnManager(
    const char* callerSrc, MOT::ConnectionId connection_id /* = INVALID_CONNECTION_ID */)
{
    if (!u_sess->mot_cxt.txn_manager) {
        bool attachCleanFunc =
            (MOTCurrThreadId == INVALID_THREAD_ID ? true : !g_instance.attr.attr_common.enable_thread_pool);

        // First time we handle this connection
        if (m_engine == nullptr) {
            elog(ERROR, "initTxnManager: MOT engine is not initialized");
            return nullptr;
        }

        // create new session context
        MOT::SessionContext* session_ctx =
            MOT::GetSessionManager()->CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, connection_id);
        if (session_ctx == nullptr) {
            MOT_REPORT_ERROR(
                MOT_ERROR_INTERNAL, "Session Initialization", "Failed to create session context in %s", callerSrc);
            ereport(ERROR, (errmsg("Session startup: failed to create session context.")));
            return nullptr;
        }
        MOT_ASSERT(u_sess->mot_cxt.session_context == session_ctx);
        MOT_ASSERT(u_sess->mot_cxt.session_id == session_ctx->GetSessionId());
        MOT_ASSERT(u_sess->mot_cxt.connection_id == session_ctx->GetConnectionId());

        // make sure we cleanup leftovers from other session
        u_sess->mot_cxt.jit_context_count = 0;

        // record session details for statistics report
        RecordSessionDetails();

        if (attachCleanFunc) {
            // schedule session cleanup when thread pool is not used
            if (!g_instance.attr.attr_common.enable_thread_pool) {
                on_proc_exit(DestroyTxn, PointerGetDatum(session_ctx));
                ScheduleSessionCleanup();
            } else {
                on_proc_exit(MOTCleanupThread, PointerGetDatum(nullptr));
                MOT_LOG_DEBUG("Registered current thread for proc-exit callback for thread %p", (void*)pthread_self());
            }
        }

        u_sess->mot_cxt.txn_manager = session_ctx->GetTxnManager();
        elog(DEBUG1, "Init TXN_MAN for thread %u", MOTCurrThreadId);
    }

    return u_sess->mot_cxt.txn_manager;
}

static void DestroySessionJitContexts()
{
    // we must release all JIT context objects associated with this session now.
    // it seems that when thread pool is disabled, all cached plan sources for the session are not
    // released explicitly, but rather implicitly as part of the release of the memory context of the session.
    // in any case, we guard against repeated destruction of the JIT context by nullifying it
    MOT_LOG_DEBUG("Cleaning up all JIT context objects for current session");
    CachedPlanSource* psrc = u_sess->pcache_cxt.first_saved_plan;
    while (psrc != nullptr) {
        if (psrc->mot_jit_context != nullptr) {
            MOT_LOG_DEBUG("DestroySessionJitContexts(): Calling DestroyJitContext(%p)", psrc->mot_jit_context);
            JitExec::DestroyJitContext(psrc->mot_jit_context);
            psrc->mot_jit_context = nullptr;
        }
        psrc = psrc->next_saved;
    }
    MOT_LOG_DEBUG("DONE Cleaning up all JIT context objects for current session");
}

/** @brief Notification from thread pool that a session ended (only when thread pool is ENABLED). */
extern void MOTOnSessionClose()
{
    MOT_LOG_TRACE("Received session close notification (current session id: %u, current connection id: %u)",
        u_sess->mot_cxt.session_id,
        u_sess->mot_cxt.connection_id);
    if (u_sess->mot_cxt.session_id != INVALID_SESSION_ID) {
        ClearCurrentSessionDetails();
        MOT_LOG_DEBUG("MOTOnSessionClose(): Calling DestroySessionJitContexts()");
        DestroySessionJitContexts();
        if (!MOTAdaptor::m_engine) {
            MOT_LOG_ERROR("MOTOnSessionClose(): MOT engine is not initialized");
        } else {
            EnsureSafeThreadAccessInline();  // this is ok, it wil be cleaned up when thread exits
            MOT::SessionContext* sessionContext = u_sess->mot_cxt.session_context;
            if (sessionContext == nullptr) {
                MOT_LOG_WARN("Received session close notification, but no current session is found. Current session id "
                             "is %u. Request ignored.",
                    u_sess->mot_cxt.session_id);
            } else {
                DestroySession(sessionContext);
                MOT_ASSERT(u_sess->mot_cxt.session_id == INVALID_SESSION_ID);
            }
        }
    }
}

/** @brief Notification from thread pool that a pooled thread ended (only when thread pool is ENABLED). */
static void MOTOnThreadShutdown()
{
    if (!MOTAdaptor::m_initialized) {
        return;
    }

    MOT_LOG_TRACE("Received thread shutdown notification");
    if (!MOTAdaptor::m_engine) {
        MOT_LOG_ERROR("MOTOnThreadShutdown(): MOT engine is not initialized");
    } else {
        MOTAdaptor::m_engine->OnCurrentThreadEnding();
    }
    knl_thread_mot_init();  // reset all thread locals
}

/**
 * @brief on_proc_exit() callback to handle thread-cleanup - regardless of whether thread pool is enabled or not.
 * registration to on_proc_exit() is triggered by first call to EnsureSafeThreadAccessInline().
 */
static void MOTCleanupThread(int status, Datum ptr)
{
    MOT_ASSERT(g_instance.attr.attr_common.enable_thread_pool);
    MOT_LOG_TRACE("Received thread cleanup notification (thread-pool ON)");

    // when thread pool is used we just cleanup current thread
    // this might be a duplicate because thread pool also calls MOTOnThreadShutdown() - this is still ok
    // because we guard against repeated calls in MOTEngine::onCurrentThreadEnding()
    MOTOnThreadShutdown();
}

void MOTAdaptor::DestroyTxn(int status, Datum ptr)
{
    MOT_ASSERT(!g_instance.attr.attr_common.enable_thread_pool);

    // cleanup session
    if (!g_instance.attr.attr_common.enable_thread_pool) {
        CancelSessionCleanup();
    }
    ClearCurrentSessionDetails();
    MOT_LOG_DEBUG("DestroyTxn(): Calling DestroySessionJitContexts()");
    DestroySessionJitContexts();
    MOT::SessionContext* session = (MOT::SessionContext*)DatumGetPointer(ptr);
    if (m_engine == nullptr) {
        elog(ERROR, "destroyTxn: MOT engine is not initialized");
    }

    if (session != MOT_GET_CURRENT_SESSION_CONTEXT()) {
        MOT_LOG_WARN("Ignoring request to delete session context: already deleted");
    } else if (session != nullptr) {
        elog(DEBUG1, "Destroy SessionContext, connection_id = %u \n", session->GetConnectionId());
        EnsureSafeThreadAccessInline();  // may be accessed from new thread pool worker
        MOT::GcManager* gc = MOT_GET_CURRENT_SESSION_CONTEXT()->GetTxnManager()->GetGcSession();
        if (gc != nullptr) {
            gc->GcEndTxn();
        }
        DestroySession(session);
    }

    // clean up thread
    MOTOnThreadShutdown();
}

MOT::RC MOTAdaptor::ValidateCommit()
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    if (!IS_PGXC_COORDINATOR) {
        return txn->ValidateCommit();
    } else {
        // Nothing to do in coordinator
        return MOT::RC_OK;
    }
}

void MOTAdaptor::RecordCommit(uint64_t csn)
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetCommitSequenceNumber(csn);
    if (!IS_PGXC_COORDINATOR) {
        txn->RecordCommit();
    } else {
        txn->LiteCommit();
    }
}

MOT::RC MOTAdaptor::Commit(uint64_t csn)
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetCommitSequenceNumber(csn);
    if (!IS_PGXC_COORDINATOR) {
        return txn->Commit();
    } else {
        txn->LiteCommit();
        return MOT::RC_OK;
    }
}

void MOTAdaptor::EndTransaction()
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    // Nothing to do in coordinator
    if (!IS_PGXC_COORDINATOR) {
        txn->EndTransaction();
    }
}

void MOTAdaptor::Rollback()
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    if (!IS_PGXC_COORDINATOR) {
        txn->Rollback();
    } else {
        txn->LiteRollback();
    }
}

MOT::RC MOTAdaptor::Prepare()
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    if (!IS_PGXC_COORDINATOR) {
        return txn->Prepare();
    } else {
        txn->LitePrepare();
        return MOT::RC_OK;
    }
}

void MOTAdaptor::CommitPrepared(uint64_t csn)
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetCommitSequenceNumber(csn);
    if (!IS_PGXC_COORDINATOR) {
        txn->CommitPrepared();
    } else {
        txn->LiteCommitPrepared();
    }
}

void MOTAdaptor::RollbackPrepared()
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    if (!IS_PGXC_COORDINATOR) {
        txn->RollbackPrepared();
    } else {
        txn->LiteRollbackPrepared();
    }
}

MOT::RC MOTAdaptor::InsertRow(MOTFdwStateSt* fdwState, TupleTableSlot* slot)
{
    EnsureSafeThreadAccessInline();
    uint8_t* newRowData = nullptr;
    fdwState->m_currTxn->SetTransactionId(fdwState->m_txnId);
    MOT::Table* table = fdwState->m_table;
    MOT::Row* row = table->CreateNewRow();
    if (row == nullptr) {
        MOT_REPORT_ERROR(
            MOT_ERROR_OOM, "Insert Row", "Failed to create new row for table %s", table->GetLongTableName().c_str());
        return MOT::RC_MEMORY_ALLOCATION_ERROR;
    }
    newRowData = const_cast<uint8_t*>(row->GetData());
    PackRow(slot, table, fdwState->m_attrsUsed, newRowData);

    MOT::RC res = table->InsertRow(row, fdwState->m_currTxn);
    if ((res != MOT::RC_OK) && (res != MOT::RC_UNIQUE_VIOLATION)) {
        MOT_REPORT_ERROR(
            MOT_ERROR_OOM, "Insert Row", "Failed to insert new row for table %s", table->GetLongTableName().c_str());
    }
    return res;
}

MOT::RC MOTAdaptor::UpdateRow(MOTFdwStateSt* fdwState, TupleTableSlot* slot, MOT::Row* currRow)
{
    EnsureSafeThreadAccessInline();
    MOT::RC rc;

    do {
        fdwState->m_currTxn->SetTransactionId(fdwState->m_txnId);
        rc = fdwState->m_currTxn->UpdateLastRowState(MOT::AccessType::WR);
        if (rc != MOT::RC::RC_OK) {
            break;
        }
        uint8_t* rowData = const_cast<uint8_t*>(currRow->GetData());
        PackUpdateRow(slot, fdwState->m_table, fdwState->m_attrsModified, rowData);
        MOT::BitmapSet modified_columns(fdwState->m_attrsModified, fdwState->m_table->GetFieldCount() - 1);

        rc = fdwState->m_currTxn->OverwriteRow(currRow, modified_columns);
    } while (0);

    return rc;
}

MOT::RC MOTAdaptor::DeleteRow(MOTFdwStateSt* fdwState, TupleTableSlot* slot)
{
    EnsureSafeThreadAccessInline();
    fdwState->m_currTxn->SetTransactionId(fdwState->m_txnId);
    MOT::RC rc = fdwState->m_currTxn->DeleteLastRow();
    return rc;
}

// NOTE: colId starts from 1
bool MOTAdaptor::SetMatchingExpr(
    MOTFdwStateSt* state, MatchIndexArr* marr, int16_t colId, KEY_OPER op, Expr* expr, Expr* parent, bool set_local)
{
    bool res = false;
    uint16_t numIx = state->m_table->GetNumIndexes();

    for (uint16_t i = 0; i < numIx; i++) {
        MOT::Index* ix = state->m_table->GetIndex(i);
        if (ix != nullptr && ix->IsFieldPresent(colId)) {
            if (marr->m_idx[i] == nullptr) {
                marr->m_idx[i] = (MatchIndex*)palloc0(sizeof(MatchIndex));
                marr->m_idx[i]->Init();
                marr->m_idx[i]->m_ix = ix;
            }

            res |= marr->m_idx[i]->SetIndexColumn(state, colId, op, expr, parent, set_local);
        }
    }

    return res;
}

MatchIndex* MOTAdaptor::GetBestMatchIndex(MOTFdwStateSt* festate, MatchIndexArr* marr, int numClauses, bool setLocal)
{
    MatchIndex* best = nullptr;
    double bestCost = INT_MAX;
    uint16_t numIx = festate->m_table->GetNumIndexes();
    uint16_t bestI = (uint16_t)-1;

    for (uint16_t i = 0; i < numIx; i++) {
        if (marr->m_idx[i] != nullptr && marr->m_idx[i]->IsUsable()) {
            double cost = marr->m_idx[i]->GetCost(numClauses);
            if (cost < bestCost) {
                if (bestI < MAX_NUM_INDEXES) {
                    if (marr->m_idx[i]->GetNumMatchedCols() < marr->m_idx[bestI]->GetNumMatchedCols())
                        continue;
                }
                bestCost = cost;
                bestI = i;
            }
        }
    }

    if (bestI < MAX_NUM_INDEXES) {
        best = marr->m_idx[bestI];
        for (int k = 0; k < 2; k++) {
            for (int j = 0; j < best->m_ix->GetNumFields(); j++) {
                if (best->m_colMatch[k][j]) {
                    if (best->m_opers[k][j] < KEY_OPER::READ_INVALID) {
                        best->m_params[k][j] = AddParam(&best->m_remoteConds, best->m_colMatch[k][j]);
                        if (!list_member(best->m_remoteCondsOrig, best->m_parentColMatch[k][j])) {
                            best->m_remoteCondsOrig = lappend(best->m_remoteCondsOrig, best->m_parentColMatch[k][j]);
                        }

                        if (j > 0 && best->m_opers[k][j - 1] != KEY_OPER::READ_KEY_EXACT &&
                            !list_member(festate->m_localConds, best->m_parentColMatch[k][j])) {
                            if (setLocal)
                                festate->m_localConds = lappend(festate->m_localConds, best->m_parentColMatch[k][j]);
                        }
                    } else if (!list_member(festate->m_localConds, best->m_parentColMatch[k][j]) &&
                               !list_member(best->m_remoteCondsOrig, best->m_parentColMatch[k][j])) {
                        if (setLocal)
                            festate->m_localConds = lappend(festate->m_localConds, best->m_parentColMatch[k][j]);
                        best->m_colMatch[k][j] = nullptr;
                        best->m_parentColMatch[k][j] = nullptr;
                    }
                }
            }
        }
    }

    for (uint16_t i = 0; i < numIx; i++) {
        if (marr->m_idx[i] != nullptr) {
            MatchIndex* mix = marr->m_idx[i];
            if (i != bestI) {
                if (setLocal) {
                    for (int k = 0; k < 2; k++) {
                        for (int j = 0; j < mix->m_ix->GetNumFields(); j++) {
                            if (mix->m_colMatch[k][j] &&
                                !list_member(festate->m_localConds, mix->m_parentColMatch[k][j]) &&
                                !(best != nullptr &&
                                    list_member(best->m_remoteCondsOrig, mix->m_parentColMatch[k][j]))) {
                                festate->m_localConds = lappend(festate->m_localConds, mix->m_parentColMatch[k][j]);
                            }
                        }
                    }
                }
                pfree(mix);
                marr->m_idx[i] = nullptr;
            }
        }
    }
    if (best != nullptr && best->m_ix != nullptr) {
        for (uint16_t i = 0; i < numIx; i++) {
            if (best->m_ix == festate->m_table->GetIndex(i)) {
                best->m_ixPosition = i;
                break;
            }
        }
    }

    return best;
}

void MOTAdaptor::OpenCursor(Relation rel, MOTFdwStateSt* festate)
{
    bool matchKey = true;
    bool forwardDirection = true;
    bool found = false;

    EnsureSafeThreadAccessInline();

    // GetTableByExternalId cannot return nullptr at this stage, because it is protected by envelope's table lock.
    festate->m_table = festate->m_currTxn->GetTableByExternalId(rel->rd_id);

    do {
        // this scan all keys case
        // we need to open both cursors on start and end to prevent
        // infinite scan in case "insert into table A ... as select * from table A ...
        if (festate->m_bestIx == nullptr) {
            int fIx, bIx;
            uint8_t* buf = nullptr;
            // assumption that primary index cannot be changed, can take it from
            // table and not look on ddl_access
            MOT::Index* ix = festate->m_table->GetPrimaryIndex();
            uint16_t keyLength = ix->GetKeyLength();

            if (festate->m_order == SORTDIR_ENUM::SORTDIR_ASC) {
                fIx = 0;
                bIx = 1;
                festate->m_forwardDirectionScan = true;
            } else {
                fIx = 1;
                bIx = 0;
                festate->m_forwardDirectionScan = false;
            }

            festate->m_cursor[fIx] = festate->m_table->Begin(festate->m_currTxn->GetThdId());

            festate->m_stateKey[bIx].InitKey(keyLength);
            buf = festate->m_stateKey[bIx].GetKeyBuf();
            errno_t erc = memset_s(buf, keyLength, 0xff, keyLength);
            securec_check(erc, "\0", "\0");
            festate->m_cursor[bIx] =
                ix->Search(&festate->m_stateKey[bIx], false, false, festate->m_currTxn->GetThdId(), found);
            break;
        }

        for (int i = 0; i < 2; i++) {
            if (i == 1 && festate->m_bestIx->m_end < 0) {
                if (festate->m_forwardDirectionScan) {
                    uint8_t* buf = nullptr;
                    MOT::Index* ix = festate->m_bestIx->m_ix;
                    uint16_t keyLength = ix->GetKeyLength();

                    festate->m_stateKey[1].InitKey(keyLength);
                    buf = festate->m_stateKey[1].GetKeyBuf();
                    errno_t erc = memset_s(buf, keyLength, 0xff, keyLength);
                    securec_check(erc, "\0", "\0");
                    festate->m_cursor[1] =
                        ix->Search(&festate->m_stateKey[1], false, false, festate->m_currTxn->GetThdId(), found);
                } else {
                    festate->m_cursor[1] = festate->m_bestIx->m_ix->Begin(festate->m_currTxn->GetThdId());
                }
                break;
            }

            KEY_OPER oper = (i == 0 ? festate->m_bestIx->m_ixOpers[0] : festate->m_bestIx->m_ixOpers[1]);

            forwardDirection = ((oper & ~KEY_OPER_PREFIX_BITMASK) < KEY_OPER::READ_KEY_OR_PREV);

            CreateKeyBuffer(rel, festate, i);

            if (i == 0) {
                festate->m_forwardDirectionScan = forwardDirection;
            }

            switch (oper) {
                case KEY_OPER::READ_KEY_EXACT:
                case KEY_OPER::READ_KEY_OR_NEXT:
                case KEY_OPER::READ_KEY_LIKE:
                case KEY_OPER::READ_PREFIX_LIKE:
                case KEY_OPER::READ_PREFIX:
                case KEY_OPER::READ_PREFIX_OR_NEXT:
                    matchKey = true;
                    forwardDirection = true;
                    break;

                case KEY_OPER::READ_KEY_AFTER:
                case KEY_OPER::READ_PREFIX_AFTER:
                    matchKey = false;
                    forwardDirection = true;
                    break;

                case KEY_OPER::READ_KEY_OR_PREV:
                case KEY_OPER::READ_PREFIX_OR_PREV:
                    matchKey = true;
                    forwardDirection = false;
                    break;

                case KEY_OPER::READ_KEY_BEFORE:
                case KEY_OPER::READ_PREFIX_BEFORE:
                    matchKey = false;
                    forwardDirection = false;
                    break;

                default:
                    elog(INFO, "Invalid key operation: %u", oper);
                    break;
            }

            festate->m_cursor[i] = festate->m_bestIx->m_ix->Search(
                &festate->m_stateKey[i], matchKey, forwardDirection, festate->m_currTxn->GetThdId(), found);

            if (!found && oper == KEY_OPER::READ_KEY_EXACT && festate->m_bestIx->m_ix->GetUnique()) {
                festate->m_cursor[i]->Invalidate();
                festate->m_cursor[i]->Destroy();
                delete festate->m_cursor[i];
                festate->m_cursor[i] = nullptr;
            }
        }
    } while (0);
}

static void VarLenFieldType(
    Form_pg_type typeDesc, Oid typoid, int32_t colLen, int16* typeLen, bool& isBlob, MOT::RC& res)
{
    isBlob = false;
    res = MOT::RC_OK;
    if (typeDesc->typlen < 0) {
        *typeLen = colLen;
        switch (typeDesc->typstorage) {
            case 'p':
                break;
            case 'x':
            case 'm':
                if (typoid == NUMERICOID) {
                    *typeLen = DECIMAL_MAX_SIZE;
                    break;
                }
                /* fall through */
            case 'e':
#ifdef USE_ASSERT_CHECKING
                if (typoid == TEXTOID)
                    *typeLen = colLen = MAX_VARCHAR_LEN;
#endif
                if (colLen > MAX_VARCHAR_LEN || colLen < 0) {
                    res = MOT::RC_COL_SIZE_INVALID;
                } else {
                    isBlob = true;
                }
                break;
            default:
                break;
        }
    }
}

static MOT::RC TableFieldType(
    const ColumnDef* colDef, MOT::MOT_CATALOG_FIELD_TYPES& type, int16* typeLen, Oid& typoid, bool& isBlob)
{
    MOT::RC res = MOT::RC_OK;
    Type tup;
    Form_pg_type typeDesc;
    int32_t colLen;

    if (colDef->typname->arrayBounds != nullptr) {
        return MOT::RC_UNSUPPORTED_COL_TYPE_ARR;
    }

    tup = typenameType(nullptr, colDef->typname, &colLen);
    typeDesc = ((Form_pg_type)GETSTRUCT(tup));
    typoid = HeapTupleGetOid(tup);
    *typeLen = typeDesc->typlen;

    // Get variable-length field length.
    VarLenFieldType(typeDesc, typoid, colLen, typeLen, isBlob, res);

    switch (typoid) {
        case CHAROID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_CHAR;
            break;
        case INT1OID:
        case BOOLOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_TINY;
            break;
        case INT2OID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_SHORT;
            break;
        case INT4OID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_INT;
            break;
        case INT8OID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_LONG;
            break;
        case DATEOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_DATE;
            break;
        case TIMEOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_TIME;
            break;
        case TIMESTAMPOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_TIMESTAMP;
            break;
        case TIMESTAMPTZOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_TIMESTAMPTZ;
            break;
        case INTERVALOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_INTERVAL;
            break;
        case TINTERVALOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_TINTERVAL;
            break;
        case TIMETZOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_TIMETZ;
            break;
        case FLOAT4OID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_FLOAT;
            break;
        case FLOAT8OID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_DOUBLE;
            break;
        case NUMERICOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_DECIMAL;
            break;
        case VARCHAROID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_VARCHAR;
            break;
        case BPCHAROID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_VARCHAR;
            break;
        case TEXTOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_VARCHAR;
            break;
        case CLOBOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_BLOB;
            break;
        case BYTEAOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_VARCHAR;
            break;
        default:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_UNKNOWN;
            res = MOT::RC_UNSUPPORTED_COL_TYPE;
    }

    if (tup) {
        ReleaseSysCache(tup);
    }

    return res;
}

void MOTAdaptor::ValidateCreateIndex(IndexStmt* stmt, MOT::Table* table, MOT::TxnManager* txn)
{
    if (stmt->primary) {
        if (!table->IsTableEmpty(txn->GetThdId())) {
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_FDW_ERROR),
                    errmsg(
                        "Table %s is not empty, create primary index is not allowed", table->GetTableName().c_str())));
            return;
        }
    } else if (table->GetNumIndexes() == MAX_NUM_INDEXES) {
        ereport(ERROR,
            (errmodule(MOD_MOT),
                errcode(ERRCODE_FDW_TOO_MANY_INDEXES),
                errmsg("Can not create index, max number of indexes %u reached", MAX_NUM_INDEXES)));
        return;
    }

    if (strcmp(stmt->accessMethod, "btree") != 0) {
        ereport(ERROR, (errmodule(MOD_MOT), errmsg("MOT supports indexes of type BTREE only (btree or btree_art)")));
        return;
    }

    if (list_length(stmt->indexParams) > (int)MAX_KEY_COLUMNS) {
        ereport(ERROR,
            (errmodule(MOD_MOT),
                errcode(ERRCODE_FDW_TOO_MANY_INDEX_COLUMNS),
                errmsg("Can't create index"),
                errdetail(
                    "Number of columns exceeds %d max allowed %u", list_length(stmt->indexParams), MAX_KEY_COLUMNS)));
        return;
    }
}

MOT::RC MOTAdaptor::CreateIndex(IndexStmt* stmt, ::TransactionId tid)
{
    MOT::RC res;
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetTransactionId(tid);
    MOT::Table* table = txn->GetTableByExternalId(stmt->relation->foreignOid);

    if (table == nullptr) {
        ereport(ERROR,
            (errmodule(MOD_MOT),
                errcode(ERRCODE_UNDEFINED_TABLE),
                errmsg("Table not found for oid %u", stmt->relation->foreignOid)));
        return MOT::RC_ERROR;
    }

    ValidateCreateIndex(stmt, table, txn);

    elog(LOG,
        "creating %s index %s (OID: %u), for table: %s",
        (stmt->primary ? "PRIMARY" : "SECONDARY"),
        stmt->idxname,
        stmt->indexOid,
        stmt->relation->relname);
    uint64_t keyLength = 0;
    MOT::Index* index = nullptr;
    MOT::IndexOrder index_order = MOT::IndexOrder::INDEX_ORDER_SECONDARY;

    // Use the default index tree flavor from configuration file
    MOT::IndexingMethod indexing_method = MOT::IndexingMethod::INDEXING_METHOD_TREE;
    MOT::IndexTreeFlavor flavor = MOT::GetGlobalConfiguration().m_indexTreeFlavor;

    // check if we have primary and delete previous definition
    if (stmt->primary) {
        index_order = MOT::IndexOrder::INDEX_ORDER_PRIMARY;
    }

    index = MOT::IndexFactory::CreateIndex(index_order, indexing_method, flavor);
    if (index == nullptr) {
        report_pg_error(MOT::RC_ABORT);
        return MOT::RC_ABORT;
    }
    index->SetExtId(stmt->indexOid);
    index->SetNumTableFields((uint32_t)table->GetFieldCount());
    int count = 0;

    ListCell* lc = nullptr;
    foreach (lc, stmt->indexParams) {
        IndexElem* ielem = (IndexElem*)lfirst(lc);

        uint64_t colid = table->GetFieldId((ielem->name != nullptr ? ielem->name : ielem->indexcolname));
        if (colid == (uint64_t)-1) {  // invalid column
            delete index;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
                    errmsg("Can't create index on field"),
                    errdetail("Specified column not found in table definition")));
            return MOT::RC_ERROR;
        }

        MOT::Column* col = table->GetField(colid);

        // Temp solution for NULLs, do not allow index creation on column that does not carry not null flag
        if (!MOT::GetGlobalConfiguration().m_allowIndexOnNullableColumn && !col->m_isNotNull) {
            delete index;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_FDW_INDEX_ON_NULLABLE_COLUMN_NOT_ALLOWED),
                    errmsg("Can't create index on nullable columns"),
                    errdetail("Column %s is nullable", col->m_name)));
            return MOT::RC_ERROR;
        }

        // Temp solution, we have to support DECIMAL and NUMERIC indexes as well
        if (col->m_type == MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_DECIMAL) {
            delete index;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("Can't create index on field"),
                    errdetail("INDEX on NUMERIC or DECIMAL fields not supported yet")));
            return MOT::RC_ERROR;
        }
        if (col->m_keySize > MAX_KEY_SIZE) {
            delete index;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
                    errmsg("Can't create index on field"),
                    errdetail("Column size is greater than maximum index size")));
            return MOT::RC_ERROR;
        }
        keyLength += col->m_keySize;

        index->SetLenghtKeyFields(count, colid, col->m_keySize);
        count++;
    }

    index->SetNumIndexFields(count);

    if ((res = index->IndexInit(keyLength, stmt->unique, stmt->idxname, nullptr)) != MOT::RC_OK) {
        delete index;
        report_pg_error(res);
        return res;
    }

    res = txn->CreateIndex(table, index, stmt->primary);
    if (res != MOT::RC_OK) {
        delete index;
        if (res == MOT::RC_TABLE_EXCEEDS_MAX_INDEXES) {
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_FDW_TOO_MANY_INDEXES),
                    errmsg("Can not create index, max number of indexes %u reached", MAX_NUM_INDEXES)));
            return MOT::RC_TABLE_EXCEEDS_MAX_INDEXES;
        } else {
            report_pg_error(txn->m_err, stmt->idxname, txn->m_errMsgBuf);
            return MOT::RC_UNIQUE_VIOLATION;
        }
    }

    return MOT::RC_OK;
}

void MOTAdaptor::AddTableColumns(MOT::Table* table, List *tableElts, bool& hasBlob)
{
    hasBlob = false;
    ListCell* cell = nullptr;
    foreach (cell, tableElts) {
        int16 typeLen = 0;
        bool isBlob = false;
        MOT::MOT_CATALOG_FIELD_TYPES colType;
        ColumnDef* colDef = (ColumnDef*)lfirst(cell);

        if (colDef == nullptr || colDef->typname == nullptr) {
            delete table;
            table = nullptr;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
                    errmsg("Column definition is not complete"),
                    errdetail("target table is a foreign table")));
            break;
        }

        Oid typoid = InvalidOid;
        MOT::RC res = TableFieldType(colDef, colType, &typeLen, typoid, isBlob);
        if (res != MOT::RC_OK) {
            delete table;
            table = nullptr;
            report_pg_error(res, colDef, (void*)(int64)typeLen);
            break;
        }
        hasBlob |= isBlob;

        if (colType == MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_DECIMAL) {
            if (list_length(colDef->typname->typmods) > 0) {
                bool canMakeShort = true;
                int precision = 0;
                int scale = 0;
                int count = 0;

                ListCell* c = nullptr;
                foreach (c, colDef->typname->typmods) {
                    Node* d = (Node*)lfirst(c);
                    if (!IsA(d, A_Const)) {
                        canMakeShort = false;
                        break;
                    }
                    A_Const* ac = (A_Const*)d;

                    if (ac->val.type != T_Integer) {
                        canMakeShort = false;
                        break;
                    }

                    if (count == 0) {
                        precision = ac->val.val.ival;
                    } else {
                        scale = ac->val.val.ival;
                    }

                    count++;
                }

                if (canMakeShort) {
                    int len = 0;

                    len += scale / DEC_DIGITS;
                    len += (scale % DEC_DIGITS > 0 ? 1 : 0);

                    precision -= scale;

                    len += precision / DEC_DIGITS;
                    len += (precision % DEC_DIGITS > 0 ? 1 : 0);

                    typeLen = sizeof(MOT::DecimalSt) + len * sizeof(NumericDigit);
                }
            }
        }

        res = table->AddColumn(colDef->colname, typeLen, colType, colDef->is_not_null, typoid);
        if (res != MOT::RC_OK) {
            delete table;
            table = nullptr;
            report_pg_error(res, colDef, (void*)(int64)typeLen);
            break;
        }
    }
}

MOT::RC MOTAdaptor::CreateTable(CreateForeignTableStmt* stmt, ::TransactionId tid)
{
    bool hasBlob = false;
    MOT::Index* primaryIdx = nullptr;
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__, tid);
    MOT::Table* table = nullptr;
    MOT::RC res = MOT::RC_ERROR;
    std::string tname("");
    char* dbname = NULL;

    do {
        table = new (std::nothrow) MOT::Table();
        if (table == nullptr) {
            ereport(ERROR,
                (errmodule(MOD_MOT), errcode(ERRCODE_OUT_OF_MEMORY), errmsg("Allocation of table metadata failed")));
            break;
        }

        uint32_t columnCount = list_length(stmt->base.tableElts);

        // once the columns have been counted, we add one more for the nullable columns
        ++columnCount;

        // prepare table name
        dbname = get_database_name(u_sess->proc_cxt.MyDatabaseId);
        if (dbname == nullptr) {
            delete table;
            table = nullptr;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_UNDEFINED_DATABASE),
                    errmsg("database with OID %u does not exist", u_sess->proc_cxt.MyDatabaseId)));
            break;
        }
        tname.append(dbname);
        tname.append("_");
        if (stmt->base.relation->schemaname != nullptr) {
            tname.append(stmt->base.relation->schemaname);
        } else {
            tname.append("#");
        }

        tname.append("_");
        tname.append(stmt->base.relation->relname);

        if (!table->Init(
                stmt->base.relation->relname, tname.c_str(), columnCount, stmt->base.relation->foreignOid)) {
            delete table;
            table = nullptr;
            report_pg_error(MOT::RC_MEMORY_ALLOCATION_ERROR);
            break;
        }

        // the null fields are copied verbatim because we have to give them back at some point
        res = table->AddColumn(
            "null_bytes", BITMAPLEN(columnCount - 1), MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_NULLBYTES);
        if (res != MOT::RC_OK) {
            delete table;
            table = nullptr;
            report_pg_error(MOT::RC_MEMORY_ALLOCATION_ERROR);
            break;
        }

        /*
         * Add all the columns.
         * NOTE: On failure, table object will be deleted and ereport will be done in AddTableColumns.
         */
        AddTableColumns(table, stmt->base.tableElts, hasBlob);

        table->SetFixedLengthRow(!hasBlob);

        uint32_t tupleSize = table->GetTupleSize();
        if (tupleSize > (unsigned int)MAX_TUPLE_SIZE) {
            delete table;
            table = nullptr;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("Un-support feature"),
                    errdetail("MOT: Table %s tuple size %u exceeds MAX_TUPLE_SIZE=%u !!!",
                        stmt->base.relation->relname,
                        tupleSize,
                        (unsigned int)MAX_TUPLE_SIZE)));
        }

        if (!table->InitRowPool()) {
            delete table;
            table = nullptr;
            report_pg_error(MOT::RC_MEMORY_ALLOCATION_ERROR);
            break;
        }

        elog(LOG,
            "creating table %s (OID: %u), num columns: %u, tuple: %u",
            table->GetLongTableName().c_str(),
            stmt->base.relation->foreignOid,
            columnCount,
            tupleSize);

        res = txn->CreateTable(table);
        if (res != MOT::RC_OK) {
            delete table;
            table = nullptr;
            report_pg_error(res);
            break;
        }

        // add default PK index
        primaryIdx = MOT::IndexFactory::CreatePrimaryIndexEx(MOT::IndexingMethod::INDEXING_METHOD_TREE,
            DEFAULT_TREE_FLAVOR,
            8,
            table->GetLongTableName(),
            res,
            nullptr);
        if (res != MOT::RC_OK) {
            txn->DropTable(table);
            report_pg_error(res);
            break;
        }
        primaryIdx->SetExtId(stmt->base.relation->foreignOid + 1);
        primaryIdx->SetNumTableFields(columnCount);
        primaryIdx->SetNumIndexFields(1);
        primaryIdx->SetLenghtKeyFields(0, -1, 8);
        primaryIdx->SetFakePrimary(true);

        // Add default primary index
        res = txn->CreateIndex(table, primaryIdx, true);
    } while (0);

    if (res != MOT::RC_OK) {
        if (table != nullptr) {
            txn->DropTable(table);
        }
        if (primaryIdx != nullptr) {
            delete primaryIdx;
        }
    }

    return res;
}

MOT::RC MOTAdaptor::DropIndex(DropForeignStmt* stmt, ::TransactionId tid)
{
    MOT::RC res = MOT::RC_OK;
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetTransactionId(tid);

    elog(LOG, "dropping index %s, ixoid: %u, taboid: %u", stmt->name, stmt->indexoid, stmt->reloid);

    // get table
    do {
        MOT::Index* index = txn->GetIndexByExternalId(stmt->reloid, stmt->indexoid);
        if (index == nullptr) {
            elog(LOG,
                "Drop index %s error, index oid %u of table oid %u not found.",
                stmt->name,
                stmt->indexoid,
                stmt->reloid);
            res = MOT::RC_INDEX_NOT_FOUND;
        } else if (index->IsPrimaryKey()) {
            elog(LOG, "Drop primary index is not supported, failed to drop index: %s", stmt->name);
        } else {
            MOT::Table* table = index->GetTable();
            uint64_t table_relid = table->GetTableExId();
            JitExec::PurgeJitSourceCache(table_relid, false);
            table->WrLock();
            res = txn->DropIndex(index);
            table->Unlock();
        }
    } while (0);

    return res;
}

MOT::RC MOTAdaptor::DropTable(DropForeignStmt* stmt, ::TransactionId tid)
{
    MOT::RC res = MOT::RC_OK;
    MOT::Table* tab = nullptr;
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetTransactionId(tid);

    elog(LOG, "dropping table %s, oid: %u", stmt->name, stmt->reloid);
    do {
        tab = txn->GetTableByExternalId(stmt->reloid);
        if (tab == nullptr) {
            res = MOT::RC_TABLE_NOT_FOUND;
            elog(LOG, "Drop table %s error, table oid %u not found.", stmt->name, stmt->reloid);
        } else {
            uint64_t table_relid = tab->GetTableExId();
            JitExec::PurgeJitSourceCache(table_relid, false);
            res = txn->DropTable(tab);
        }
    } while (0);

    return res;
}

MOT::RC MOTAdaptor::TruncateTable(Relation rel, ::TransactionId tid)
{
    MOT::RC res = MOT::RC_OK;
    MOT::Table* tab = nullptr;

    EnsureSafeThreadAccessInline();

    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetTransactionId(tid);

    elog(LOG, "truncating table %s, oid: %u", NameStr(rel->rd_rel->relname), rel->rd_id);
    do {
        tab = txn->GetTableByExternalId(rel->rd_id);
        if (tab == nullptr) {
            elog(LOG, "Truncate table %s error, table oid %u not found.", NameStr(rel->rd_rel->relname), rel->rd_id);
            break;
        }

        JitExec::PurgeJitSourceCache(rel->rd_id, true);
        tab->WrLock();
        res = txn->TruncateTable(tab);
        tab->Unlock();
    } while (0);

    return res;
}

MOT::RC MOTAdaptor::VacuumTable(Relation rel, ::TransactionId tid)
{
    MOT::RC res = MOT::RC_OK;
    MOT::Table* tab = nullptr;
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetTransactionId(tid);

    elog(LOG, "vacuuming table %s, oid: %u", NameStr(rel->rd_rel->relname), rel->rd_id);
    do {
        tab = MOT::GetTableManager()->GetTableSafeByExId(rel->rd_id);
        if (tab == nullptr) {
            elog(LOG, "Vacuum table %s error, table oid %u not found.", NameStr(rel->rd_rel->relname), rel->rd_id);
            break;
        }

        tab->Compact(txn);
        tab->Unlock();
    } while (0);
    return res;
}

uint64_t MOTAdaptor::GetTableIndexSize(uint64_t tabId, uint64_t ixId)
{
    uint64_t res = 0;
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    MOT::Table* tab = nullptr;
    MOT::Index* ix = nullptr;

    do {
        tab = txn->GetTableByExternalId(tabId);
        if (tab == nullptr) {
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_FDW_TABLE_NOT_FOUND),
                    errmsg("Get table size error, table oid %lu not found.", tabId)));
            break;
        }

        if (ixId > 0) {
            ix = tab->GetIndexByExtId(ixId);
            if (ix == nullptr) {
                ereport(ERROR,
                    (errmodule(MOD_MOT),
                        errcode(ERRCODE_FDW_TABLE_NOT_FOUND),
                        errmsg("Get index size error, index oid %lu for table oid %lu not found.", ixId, tabId)));
                break;
            }
            res = ix->GetIndexSize();
        } else
            res = tab->GetTableSize();
    } while (0);

    return res;
}

MotMemoryDetail* MOTAdaptor::GetMemSize(uint32_t* nodeCount, bool isGlobal)
{
    EnsureSafeThreadAccessInline();
    MotMemoryDetail* result = nullptr;
    *nodeCount = 0;

    /* We allocate an array of size (m_nodeCount + 1) to accommodate one aggregated entry of all global pools. */
    uint32_t statsArraySize = MOT::g_memGlobalCfg.m_nodeCount + 1;
    MOT::MemRawChunkPoolStats* chunkPoolStatsArray =
        (MOT::MemRawChunkPoolStats*)palloc(statsArraySize * sizeof(MOT::MemRawChunkPoolStats));
    if (chunkPoolStatsArray != nullptr) {
        errno_t erc = memset_s(chunkPoolStatsArray,
            statsArraySize * sizeof(MOT::MemRawChunkPoolStats),
            0,
            statsArraySize * sizeof(MOT::MemRawChunkPoolStats));
        securec_check(erc, "\0", "\0");

        uint32_t realStatsEntries;
        if (isGlobal) {
            realStatsEntries = MOT::MemRawChunkStoreGetGlobalStats(chunkPoolStatsArray, statsArraySize);
        } else {
            realStatsEntries = MOT::MemRawChunkStoreGetLocalStats(chunkPoolStatsArray, statsArraySize);
        }

        MOT_ASSERT(realStatsEntries <= statsArraySize);
        if (realStatsEntries > 0) {
            result = (MotMemoryDetail*)palloc(realStatsEntries * sizeof(MotMemoryDetail));
            if (result != nullptr) {
                for (uint32_t node = 0; node < realStatsEntries; ++node) {
                    result[node].numaNode = chunkPoolStatsArray[node].m_node;
                    result[node].reservedMemory = chunkPoolStatsArray[node].m_reservedBytes;
                    result[node].usedMemory = chunkPoolStatsArray[node].m_usedBytes;
                }
                *nodeCount = realStatsEntries;
            }
        }
        pfree(chunkPoolStatsArray);
    }

    return result;
}

MotSessionMemoryDetail* MOTAdaptor::GetSessionMemSize(uint32_t* sessionCount)
{
    EnsureSafeThreadAccessInline();
    MotSessionMemoryDetail* result = nullptr;
    *sessionCount = 0;

    uint32_t session_count = MOT::g_memGlobalCfg.m_maxThreadCount;
    MOT::MemSessionAllocatorStats* session_stats_array =
        (MOT::MemSessionAllocatorStats*)palloc(session_count * sizeof(MOT::MemSessionAllocatorStats));
    if (session_stats_array != nullptr) {
        uint32_t real_session_count = MOT::MemSessionGetAllStats(session_stats_array, session_count);
        if (real_session_count > 0) {
            result = (MotSessionMemoryDetail*)palloc(real_session_count * sizeof(MotSessionMemoryDetail));
            if (result != nullptr) {
                for (uint32_t session_index = 0; session_index < real_session_count; ++session_index) {
                    GetSessionDetails(session_stats_array[session_index].m_sessionId,
                        &result[session_index].threadid,
                        &result[session_index].threadStartTime);
                    result[session_index].totalSize = session_stats_array[session_index].m_reservedSize;
                    result[session_index].usedSize = session_stats_array[session_index].m_usedSize;
                    result[session_index].freeSize = result[session_index].totalSize - result[session_index].usedSize;
                }
                *sessionCount = real_session_count;
            }
        }
        pfree(session_stats_array);
    }

    return result;
}

void MOTAdaptor::CreateKeyBuffer(Relation rel, MOTFdwStateSt* festate, int start)
{
    uint8_t* buf = nullptr;
    uint8_t pattern = 0x00;
    EnsureSafeThreadAccessInline();
    int16_t num = festate->m_bestIx->m_ix->GetNumFields();
    const uint16_t* fieldLengths = festate->m_bestIx->m_ix->GetLengthKeyFields();
    const int16_t* orgCols = festate->m_bestIx->m_ix->GetColumnKeyFields();
    TupleDesc desc = rel->rd_att;
    uint16_t offset = 0;
    int32_t* exprs = nullptr;
    KEY_OPER* opers = nullptr;
    uint16_t keyLength;
    KEY_OPER oper;

    if (start == 0) {
        exprs = festate->m_bestIx->m_params[festate->m_bestIx->m_start];
        opers = festate->m_bestIx->m_opers[festate->m_bestIx->m_start];
        oper = festate->m_bestIx->m_ixOpers[0];
    } else {
        exprs = festate->m_bestIx->m_params[festate->m_bestIx->m_end];
        opers = festate->m_bestIx->m_opers[festate->m_bestIx->m_end];
        // end may be equal start but the operation maybe different, look at getCost
        oper = festate->m_bestIx->m_ixOpers[1];
    }

    keyLength = festate->m_bestIx->m_ix->GetKeyLength();
    festate->m_stateKey[start].InitKey(keyLength);
    buf = festate->m_stateKey[start].GetKeyBuf();

    switch (oper) {
        case KEY_OPER::READ_KEY_EXACT:
        case KEY_OPER::READ_KEY_OR_NEXT:
        case KEY_OPER::READ_KEY_BEFORE:
        case KEY_OPER::READ_KEY_LIKE:
        case KEY_OPER::READ_PREFIX:
        case KEY_OPER::READ_PREFIX_LIKE:
        case KEY_OPER::READ_PREFIX_OR_NEXT:
        case KEY_OPER::READ_PREFIX_BEFORE:
            pattern = 0x00;
            break;

        case KEY_OPER::READ_KEY_OR_PREV:
        case KEY_OPER::READ_PREFIX_AFTER:
        case KEY_OPER::READ_PREFIX_OR_PREV:
        case KEY_OPER::READ_KEY_AFTER:
            pattern = 0xff;
            break;

        default:
            elog(LOG, "Invalid key operation: %u", oper);
            break;
    }

    for (int i = 0; i < num; i++) {
        if (opers[i] < KEY_OPER::READ_INVALID) {
            bool is_null = false;
            ExprState* expr = (ExprState*)list_nth(festate->m_execExprs, exprs[i] - 1);
            Datum val = ExecEvalExpr((ExprState*)(expr), festate->m_econtext, &is_null, nullptr);
            if (is_null) {
                MOT_ASSERT((offset + fieldLengths[i]) <= keyLength);
                errno_t erc = memset_s(buf + offset, fieldLengths[i], 0x00, fieldLengths[i]);
                securec_check(erc, "\0", "\0");
            } else {
                MOT::Column* col = festate->m_table->GetField(orgCols[i]);
                uint8_t fill = 0x00;

                // in case of like fill rest of the key with appropriate to direction values
                if (opers[i] == KEY_OPER::READ_KEY_LIKE) {
                    switch (oper) {
                        case KEY_OPER::READ_KEY_LIKE:
                        case KEY_OPER::READ_KEY_OR_NEXT:
                        case KEY_OPER::READ_KEY_AFTER:
                        case KEY_OPER::READ_PREFIX:
                        case KEY_OPER::READ_PREFIX_LIKE:
                        case KEY_OPER::READ_PREFIX_OR_NEXT:
                        case KEY_OPER::READ_PREFIX_AFTER:
                            break;

                        case KEY_OPER::READ_PREFIX_BEFORE:
                        case KEY_OPER::READ_PREFIX_OR_PREV:
                        case KEY_OPER::READ_KEY_BEFORE:
                        case KEY_OPER::READ_KEY_OR_PREV:
                            fill = 0xff;
                            break;

                        case KEY_OPER::READ_KEY_EXACT:
                        default:
                            elog(LOG, "Invalid key operation: %u", oper);
                            break;
                    }
                }

                DatumToMOTKey(col,
                    expr->resultType,
                    val,
                    desc->attrs[orgCols[i] - 1]->atttypid,
                    buf + offset,
                    fieldLengths[i],
                    opers[i],
                    fill);
            }
        } else {
            MOT_ASSERT((offset + fieldLengths[i]) <= keyLength);
            festate->m_stateKey[start].FillPattern(pattern, fieldLengths[i], offset);
        }

        offset += fieldLengths[i];
    }

    festate->m_bestIx->m_ix->AdjustKey(&festate->m_stateKey[start], pattern);
}

bool MOTAdaptor::IsScanEnd(MOTFdwStateSt* festate)
{
    bool res = false;
    EnsureSafeThreadAccessInline();

    // festate->cursor[1] (end iterator) might be NULL (in case it is not in use). If this is the case, return false
    // (which means we have not reached the end yet)
    if (festate->m_cursor[1] == nullptr) {
        return false;
    }

    if (!festate->m_cursor[1]->IsValid()) {
        return true;
    } else {
        const MOT::Key* startKey = nullptr;
        const MOT::Key* endKey = nullptr;
        MOT::Index* ix = (festate->m_bestIx != nullptr ? festate->m_bestIx->m_ix : festate->m_table->GetPrimaryIndex());

        startKey = reinterpret_cast<const MOT::Key*>(festate->m_cursor[0]->GetKey());
        endKey = reinterpret_cast<const MOT::Key*>(festate->m_cursor[1]->GetKey());
        if (startKey != nullptr && endKey != nullptr) {
            int cmpRes = memcmp(startKey->GetKeyBuf(), endKey->GetKeyBuf(), ix->GetKeySizeNoSuffix());

            if (festate->m_forwardDirectionScan) {
                if (cmpRes > 0)
                    res = true;
            } else {
                if (cmpRes < 0)
                    res = true;
            }
        }
    }

    return res;
}

void MOTAdaptor::PackRow(TupleTableSlot* slot, MOT::Table* table, uint8_t* attrs_used, uint8_t* destRow)
{
    errno_t erc;
    EnsureSafeThreadAccessInline();
    HeapTuple srcData = (HeapTuple)slot->tts_tuple;
    TupleDesc tupdesc = slot->tts_tupleDescriptor;
    bool hasnulls = HeapTupleHasNulls(srcData);
    uint64_t i = 0;
    uint64_t j = 1;
    uint64_t cols = table->GetFieldCount() - 1;  // column count includes null bits field

    // the null bytes are necessary and have to give them back
    if (!hasnulls) {
        erc = memset_s(destRow + table->GetFieldOffset(i), table->GetFieldSize(i), 0xff, table->GetFieldSize(i));
        securec_check(erc, "\0", "\0");
    } else {
        erc = memcpy_s(destRow + table->GetFieldOffset(i),
            table->GetFieldSize(i),
            &srcData->t_data->t_bits[0],
            table->GetFieldSize(i));
        securec_check(erc, "\0", "\0");
    }

    // we now copy the fields, for the time being the null ones will be copied as well
    for (; i < cols; i++, j++) {
        bool isnull = false;
        Datum value = heap_slot_getattr(slot, j, &isnull);

        if (!isnull) {
            DatumToMOT(table->GetField(j), value, tupdesc->attrs[i]->atttypid, destRow);
        }
    }
}

void MOTAdaptor::PackUpdateRow(TupleTableSlot* slot, MOT::Table* table, const uint8_t* attrs_used, uint8_t* destRow)
{
    EnsureSafeThreadAccessInline();
    TupleDesc tupdesc = slot->tts_tupleDescriptor;
    uint8_t* bits;
    uint64_t i = 0;
    uint64_t j = 1;

    // column count includes null bits field
    uint64_t cols = table->GetFieldCount() - 1;
    bits = destRow + table->GetFieldOffset(i);

    for (; i < cols; i++, j++) {
        if (BITMAP_GET(attrs_used, i)) {
            bool isnull = false;
            Datum value = heap_slot_getattr(slot, j, &isnull);

            if (!isnull) {
                DatumToMOT(table->GetField(j), value, tupdesc->attrs[i]->atttypid, destRow);
                BITMAP_SET(bits, i);
            } else {
                BITMAP_CLEAR(bits, i);
            }
        }
    }
}

void MOTAdaptor::UnpackRow(TupleTableSlot* slot, MOT::Table* table, const uint8_t* attrs_used, uint8_t* srcRow)
{
    EnsureSafeThreadAccessInline();
    TupleDesc tupdesc = slot->tts_tupleDescriptor;
    uint64_t i = 0;

    // column count includes null bits field
    uint64_t cols = table->GetFieldCount() - 1;

    for (; i < cols; i++) {
        if (BITMAP_GET(attrs_used, i))
            MOTToDatum(table, tupdesc->attrs[i], srcRow, &(slot->tts_values[i]), &(slot->tts_isnull[i]));
        else {
            slot->tts_isnull[i] = true;
            slot->tts_values[i] = PointerGetDatum(nullptr);
        }
    }
}

// useful functions for data conversion: utils/fmgr/gmgr.cpp
void MOTAdaptor::MOTToDatum(MOT::Table* table, const Form_pg_attribute attr, uint8_t* data, Datum* value, bool* is_null)
{
    EnsureSafeThreadAccessInline();
    if (!BITMAP_GET(data, (attr->attnum - 1))) {
        *is_null = true;
        *value = PointerGetDatum(nullptr);

        return;
    }

    size_t len = 0;
    MOT::Column* col = table->GetField(attr->attnum);

    *is_null = false;
    switch (attr->atttypid) {
        case VARCHAROID:
        case BPCHAROID:
        case TEXTOID:
        case CLOBOID:
        case BYTEAOID: {
            uintptr_t tmp;
            col->Unpack(data, &tmp, len);

            bytea* result = (bytea*)palloc(len + VARHDRSZ);
            errno_t erc = memcpy_s(VARDATA(result), len, (uint8_t*)tmp, len);
            securec_check(erc, "\0", "\0");
            SET_VARSIZE(result, len + VARHDRSZ);

            *value = PointerGetDatum(result);
            break;
        }
        case NUMERICOID: {
            MOT::DecimalSt* d;
            col->Unpack(data, (uintptr_t*)&d, len);

            *value = NumericGetDatum(MOTNumericToPG(d));
            break;
        }
        default:
            col->Unpack(data, value, len);
            break;
    }
}

void MOTAdaptor::DatumToMOT(MOT::Column* col, Datum datum, Oid type, uint8_t* data)
{
    EnsureSafeThreadAccessInline();
    switch (type) {
        case BYTEAOID:
        case TEXTOID:
        case VARCHAROID:
        case CLOBOID:
        case BPCHAROID: {
            bytea* txt = DatumGetByteaP(datum);
            size_t size = VARSIZE(txt);  // includes header len VARHDRSZ
            char* src = VARDATA(txt);
            col->Pack(data, (uintptr_t)src, size - VARHDRSZ);

            if ((char*)datum != (char*)txt) {
                pfree(txt);
            }

            break;
        }
        case NUMERICOID: {
            Numeric n = DatumGetNumeric(datum);
            char buf[DECIMAL_MAX_SIZE];
            MOT::DecimalSt* d = (MOT::DecimalSt*)buf;

            if (NUMERIC_NDIGITS(n) > DECIMAL_MAX_DIGITS) {
                ereport(ERROR,
                    (errmodule(MOD_MOT),
                        errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                        errmsg("Value exceeds maximum precision: %d", NUMERIC_MAX_PRECISION)));
                break;
            }
            PGNumericToMOT(n, *d);
            col->Pack(data, (uintptr_t)d, DECIMAL_SIZE(d));

            break;
        }
        default:
            col->Pack(data, datum, col->m_size);
            break;
    }
}

inline void MOTAdaptor::VarcharToMOTKey(
    MOT::Column* col, Oid datumType, Datum datum, Oid colType, uint8_t* data, size_t len, KEY_OPER oper, uint8_t fill)
{
    bool noValue = false;
    switch (datumType) {
        case BYTEAOID:
        case TEXTOID:
        case VARCHAROID:
        case CLOBOID:
        case BPCHAROID:
            break;
        default:
            noValue = true;
            errno_t erc = memset_s(data, len, 0x00, len);
            securec_check(erc, "\0", "\0");
            break;
    }

    if (noValue) {
        return;
    }

    bytea* txt = DatumGetByteaP(datum);
    size_t size = VARSIZE(txt);  // includes header len VARHDRSZ
    char* src = VARDATA(txt);

    if (size > len) {
        size = len;
    }

    size -= VARHDRSZ;
    if (oper == KEY_OPER::READ_KEY_LIKE) {
        if (src[size - 1] == '%') {
            size -= 1;
        } else {
            // switch to equal
            if (colType == BPCHAROID) {
                fill = 0x20;  // space ' ' == 0x20
            } else {
                fill = 0x00;
            }
        }
    } else if (colType == BPCHAROID) {  // handle padding for blank-padded type
        fill = 0x20;
    }
    col->PackKey(data, (uintptr_t)src, size, fill);

    if ((char*)datum != (char*)txt) {
        pfree(txt);
    }
}

inline void MOTAdaptor::FloatToMOTKey(MOT::Column* col, Oid datumType, Datum datum, uint8_t* data)
{
    if (datumType == FLOAT8OID) {
        MOT::DoubleConvT dc;
        MOT::FloatConvT fc;
        dc.m_r = (uint64_t)datum;
        fc.m_v = (float)dc.m_v;
        uint64_t u = (uint64_t)fc.m_r;
        col->PackKey(data, u, col->m_size);
    } else {
        col->PackKey(data, datum, col->m_size);
    }
}

inline void MOTAdaptor::NumericToMOTKey(MOT::Column* col, Oid datumType, Datum datum, uint8_t* data)
{
    Numeric n = DatumGetNumeric(datum);
    char buf[DECIMAL_MAX_SIZE];
    MOT::DecimalSt* d = (MOT::DecimalSt*)buf;
    PGNumericToMOT(n, *d);
    col->PackKey(data, (uintptr_t)d, DECIMAL_SIZE(d));
}

inline void MOTAdaptor::TimestampToMOTKey(MOT::Column* col, Oid datumType, Datum datum, uint8_t* data)
{
    if (datumType == TIMESTAMPTZOID) {
        Timestamp result = DatumGetTimestamp(DirectFunctionCall1(timestamptz_timestamp, datum));
        col->PackKey(data, result, col->m_size);
    } else if (datumType == DATEOID) {
        Timestamp result = DatumGetTimestamp(DirectFunctionCall1(date_timestamp, datum));
        col->PackKey(data, result, col->m_size);
    } else {
        col->PackKey(data, datum, col->m_size);
    }
}

inline void MOTAdaptor::TimestampTzToMOTKey(MOT::Column* col, Oid datumType, Datum datum, uint8_t* data)
{
    if (datumType == TIMESTAMPOID) {
        TimestampTz result = DatumGetTimestampTz(DirectFunctionCall1(timestamp_timestamptz, datum));
        col->PackKey(data, result, col->m_size);
    } else if (datumType == DATEOID) {
        TimestampTz result = DatumGetTimestampTz(DirectFunctionCall1(date_timestamptz, datum));
        col->PackKey(data, result, col->m_size);
    } else {
        col->PackKey(data, datum, col->m_size);
    }
}

inline void MOTAdaptor::DateToMOTKey(MOT::Column* col, Oid datumType, Datum datum, uint8_t* data)
{
    if (datumType == TIMESTAMPOID) {
        DateADT result = DatumGetDateADT(DirectFunctionCall1(timestamp_date, datum));
        col->PackKey(data, result, col->m_size);
    } else if (datumType == TIMESTAMPTZOID) {
        DateADT result = DatumGetDateADT(DirectFunctionCall1(timestamptz_date, datum));
        col->PackKey(data, result, col->m_size);
    } else {
        col->PackKey(data, datum, col->m_size);
    }
}

void MOTAdaptor::DatumToMOTKey(
    MOT::Column* col, Oid datumType, Datum datum, Oid colType, uint8_t* data, size_t len, KEY_OPER oper, uint8_t fill)
{
    EnsureSafeThreadAccessInline();
    switch (colType) {
        case BYTEAOID:
        case TEXTOID:
        case VARCHAROID:
        case CLOBOID:
        case BPCHAROID:
            VarcharToMOTKey(col, datumType, datum, colType, data, len, oper, fill);
            break;
        case FLOAT4OID:
            FloatToMOTKey(col, datumType, datum, data);
            break;
        case NUMERICOID:
            NumericToMOTKey(col, datumType, datum, data);
            break;
        case TIMESTAMPOID:
            TimestampToMOTKey(col, datumType, datum, data);
            break;
        case TIMESTAMPTZOID:
            TimestampTzToMOTKey(col, datumType, datum, data);
            break;
        case DATEOID:
            DateToMOTKey(col, datumType, datum, data);
            break;
        default:
            col->PackKey(data, datum, col->m_size);
            break;
    }
}

MOTFdwStateSt* InitializeFdwState(void* fdwState, List** fdwExpr, uint64_t exTableID)
{
    MOTFdwStateSt* state = (MOTFdwStateSt*)palloc0(sizeof(MOTFdwStateSt));
    List* values = (List*)fdwState;

    state->m_allocInScan = true;
    state->m_foreignTableId = exTableID;
    if (list_length(values) > 0) {
        ListCell* cell = list_head(values);
        int type = ((Const*)lfirst(cell))->constvalue;
        if (type != FDW_LIST_STATE) {
            return state;
        }
        cell = lnext(cell);
        state->m_cmdOper = (CmdType)((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);
        state->m_order = (SORTDIR_ENUM)((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);
        state->m_hasForUpdate = (bool)((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);
        state->m_foreignTableId = ((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);
        state->m_numAttrs = ((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);
        state->m_ctidNum = ((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);
        state->m_numExpr = ((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);

        int len = BITMAP_GETLEN(state->m_numAttrs);
        state->m_attrsUsed = (uint8_t*)palloc0(len);
        state->m_attrsModified = (uint8_t*)palloc0(len);
        BitmapDeSerialize(state->m_attrsUsed, len, &cell);

        if (cell != NULL) {
            state->m_bestIx = &state->m_bestIxBuf;
            state->m_bestIx->Deserialize(cell, exTableID);
        }

        if (fdwExpr != NULL && *fdwExpr != NULL) {
            ListCell* c = NULL;
            int i = 0;

            // divide fdw expr to param list and original expr
            state->m_remoteCondsOrig = NULL;

            foreach (c, *fdwExpr) {
                if (i < state->m_numExpr) {
                    i++;
                    continue;
                } else {
                    state->m_remoteCondsOrig = lappend(state->m_remoteCondsOrig, lfirst(c));
                }
            }

            *fdwExpr = list_truncate(*fdwExpr, state->m_numExpr);
        }
    }
    return state;
}

void* SerializeFdwState(MOTFdwStateSt* state)
{
    List* result = NULL;

    // set list type to FDW_LIST_STATE
    result = lappend(result, makeConst(INT4OID, -1, InvalidOid, 4, FDW_LIST_STATE, false, true));
    result = lappend(result, makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(state->m_cmdOper), false, true));
    result = lappend(result, makeConst(INT1OID, -1, InvalidOid, 4, Int8GetDatum(state->m_order), false, true));
    result = lappend(result, makeConst(BOOLOID, -1, InvalidOid, 1, BoolGetDatum(state->m_hasForUpdate), false, true));
    result =
        lappend(result, makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(state->m_foreignTableId), false, true));
    result = lappend(result, makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(state->m_numAttrs), false, true));
    result = lappend(result, makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(state->m_ctidNum), false, true));
    result = lappend(result, makeConst(INT2OID, -1, InvalidOid, 2, Int16GetDatum(state->m_numExpr), false, true));
    int len = BITMAP_GETLEN(state->m_numAttrs);
    result = BitmapSerialize(result, state->m_attrsUsed, len);

    if (state->m_bestIx != nullptr) {
        state->m_bestIx->Serialize(&result);
    }
    ReleaseFdwState(state);
    return result;
}

void ReleaseFdwState(MOTFdwStateSt* state)
{
    CleanCursors(state);

    if (state->m_currTxn) {
        state->m_currTxn->m_queryState.erase((uint64_t)state);
    }

    if (state->m_bestIx && state->m_bestIx != &state->m_bestIxBuf)
        pfree(state->m_bestIx);

    if (state->m_remoteCondsOrig != nullptr)
        list_free(state->m_remoteCondsOrig);

    if (state->m_attrsUsed != NULL)
        pfree(state->m_attrsUsed);

    if (state->m_attrsModified != NULL)
        pfree(state->m_attrsModified);

    state->m_table = NULL;
    pfree(state);
}




























































std::atomic<int> cpu_index(1);
void SetCPU(){
    cpu_set_t logicalEpochSet;
    CPU_ZERO(&logicalEpochSet);
    CPU_SET(cpu_index.fetch_add(1), &logicalEpochSet); //2就是核心号
    int rc = sched_setaffinity(0, sizeof(cpu_set_t), &logicalEpochSet);
    if (rc == -1) {
//        ereport(FATAL, (errmsg("绑核失败")));
    }
}

void string_free(void *data, void *hint){
    delete static_cast<std::string*>(hint);
}

uint64_t now_to_us(){
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

template<typename key, typename value>
class ConcurrentHashMap {
public:
    typedef typename std::unordered_map<key, value>::iterator map_iterator;
    typedef typename std::unordered_map<key, value>::size_type size_type;

    bool insert(key &k, value &v, value *p) {
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if (iter == _map_temp.end()) {
            _map_temp[k] = v;
            *p = nullptr;
        } else {
            *p = _map_temp[k];
            _map_temp[k] = v;
        }
        return true;
    }

    void insert(key &k, value &v) {
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        _map_temp[k] = v;
    }


    void remove(key &k, value &v) {
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if (iter != _map_temp.end()) {
            if (iter->second == v) {
                _map_temp.erase(iter);
            }
        }
    }

    void remove(key &k) {
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if (iter != _map_temp.end()) {
            _map_temp.erase(iter);
        }
    }

    void clear() {
        for(uint64_t i = 0; i < _N; i ++){
            std::unique_lock<std::mutex> lock(_mutex[i]);
            _map[i].clear();
        }
    }

    void unsafe_clear() {
        for(uint64_t i = 0; i < _N; i ++){
            _map[i].clear();
        }
    }

    bool contain(key &k, value &v){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            if(iter->second == v){
                return true;
            }
        }
        return false;
    }

    bool contain(key &k){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            return true;
        }
        return false;
    }

    bool unsafe_contain(key &k, value &v){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            if(iter->second == v){
                return true;
            }
        }
        return false;
    }

    bool get_value(key &k, value &v) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()) {
            v = iter->second;
            return true;
        }
        else {
            return false;
        }
    }

    bool unsafe_get_value(key &k, value &v) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()) {
            v = iter->second;
            return true;
        }
        else {
            return false;
        }
    }

    size_type size() {
        size_type ans = 0;
        for(uint64_t i = 0; i < _N; i ++){
            std::unique_lock<std::mutex> lock(_mutex[i]);
            ans += _map[i].size();
        }
        return ans;
    }

protected:
    inline std::unordered_map<key, value>& GetMapRef(const key k){ return _map[(_hash(k) % _N)]; }
    inline std::unordered_map<key, value>& GetMapRef(const key k) const { return _map[(_hash(k) % _N)]; }
    inline std::mutex& GetMutexRef(const key k) { return _mutex[(_hash(k) % _N)]; }
    inline std::mutex& GetMutexRef(const key k) const {return _mutex[(_hash(k) % _N)]; }

private:
    const static uint64_t _N = 101;//521 997 1217 12281 122777 prime
    std::hash<key> _hash;
    std::unordered_map<key, value> _map[_N];
    std::mutex _mutex[_N];
};




template<typename T>
using BlockingConcurrentQueue =  moodycamel::BlockingConcurrentQueue<T>;

struct send_thread_params{
    uint64_t current_epoch;
    uint64_t tot;
    std::string* merge_request_ptr;
    send_thread_params(uint64_t ce, uint64_t tot_temp, std::string* ptr1):
        current_epoch(ce), tot(tot_temp), merge_request_ptr(ptr1){}
    send_thread_params(){}
};

bool Gzip(google::protobuf::MessageLite* ptr, std::string* serialized_str_ptr) {
    //        google::protobuf::io::GzipOutputStream::Options options;
    //        options.format = google::protobuf::io::GzipOutputStream::GZIP;
    //        options.compression_level = 9;
    //        google::protobuf::io::StringOutputStream outputStream(serialized_str_ptr);
    //        google::protobuf::io::GzipOutputStream gzipStream(&outputStream, options);
    //        auto res = ptr->SerializeToZeroCopyStream(&gzipStream);
    //        gzipStream.Close();

    google::protobuf::io::StringOutputStream outputStream(serialized_str_ptr);
    auto res = ptr->SerializeToZeroCopyStream(&outputStream);
    return res;
}

bool UnGzip(google::protobuf::MessageLite* ptr, const std::string* str) {
    //    auto message_string_ptr = std::make_unique<std::string>(static_cast<const char*>(message_ptr->data()), message_ptr->size());
    //        google::protobuf::io::ArrayInputStream inputStream(str->data(), (int)str->size());
    //        google::protobuf::io::GzipInputStream gzipStream(&inputStream);
    //        return ptr->ParseFromZeroCopyStream(&gzipStream);
    google::protobuf::io::ArrayInputStream inputStream(str->data(), (int)str->size());
    return ptr->ParseFromZeroCopyStream(&inputStream);
}




































//Storage
BlockingConcurrentQueue<std::unique_ptr<proto::Transaction>> storage_update_queue;


//**************************************************************************************************************************************************************************************************
//*****                                                                                                                                                                                        *****
//*****                                                                                                                                                                                        *****
//*****                                                                                      TAAS    Client                                                                                    *****
//*****                                                                                                                                                                                        *****
//*****                                                                                                                                                                                        *****
//**************************************************************************************************************************************************************************************************

BlockingConcurrentQueue<std::unique_ptr<zmq::message_t>> client_listen_message_queue;
BlockingConcurrentQueue<std::unique_ptr<send_thread_params>> client_send_message_queue;
BlockingConcurrentQueue<std::unique_ptr<proto::Message>> client_other_message_queue;

// std::unique_ptr<std::mutex> client_send_mutex, client_listen_mutex;
// std::shared_ptr<zmq::context_t> client_send_context, client_listen_context;
// std::shared_ptr<zmq::socket_t> client_send_socket, client_listen_socket;

std::atomic<bool> client_init_ok_flag(false), client_start_flag(false);
ConcurrentHashMap<uint64_t, MOT::TxnManager*> txn_map;
std::atomic<uint64_t> local_csn(5);

bool MOTAdaptor::InsertTxntoLocalChangeSet(MOT::TxnManager* txMan){
    auto msg = std::make_unique<proto::Message>();
    auto* txn = msg->mutable_txn();
    proto::Row *row;
    proto::Column* col;
    proto::OpType op_type;
    MOT::Row* local_row = nullptr;
    MOT::Key* key = nullptr;
    void* buf = nullptr;
    uint64_t fieldCnt;
    const MOT::Access* access = nullptr;
    MOT::BitmapSet* bmp;
    MOT::TxnOrderedSet_t &orderedSet = txMan->m_accessMgr->GetOrderedRowSet();
    int num = 0;
    int read_op_num = 0, write_op_num = 0;
    for (const auto &raPair : orderedSet){
        num ++;
        access = raPair.second;
        row = txn->add_row();
        if (access->m_type == MOT::RD) {
            op_type = proto::OpType::Read;
            local_row = access->m_localRow;
            read_op_num += 1;
        }
        else if (access->m_type == MOT::WR){
            op_type = proto::OpType::Update;
            local_row = access->m_localRow;
            write_op_num += 1;
        }
        else if (access->m_type == MOT::INS){
            op_type = proto::OpType::Insert;
            local_row = access->m_localInsertRow;
            write_op_num += 1;
        }
        else if (access->m_type == MOT::DEL){
            op_type = proto::OpType::Delete;
            local_row = access->m_localRow;
            write_op_num += 1;
        }

        if(local_row == nullptr || local_row->GetTable() == nullptr){
            return false;
        }
        key = local_row->GetTable()->BuildKeyByRow(local_row, txMan, buf);
        if (key == nullptr) {
            return false;
        }
        row->set_key(std::move(std::string(key->GetKeyBuf(), key->GetKeyBuf() + key->GetKeyLength())));
        row->set_table_name(local_row->GetTable()->GetLongTableName());
//        if(op_type == proto::OpType::Update || op_type == proto::OpType::Insert) {
//            row->set_data(local_row->GetData(), local_row->GetTable()->GetTupleSize());
//        }
        if (access->m_type == MOT::RD) {
            auto csn_s = std::to_string(local_row->GetCommitSequenceNumber());
            row->set_data(csn_s.c_str(), csn_s.size());
        }
        else {
            std::string str = std::string(reinterpret_cast<const char*>(local_row->GetData()), local_row->GetTable()->GetTupleSize());
            int maxCompressedSize = LZ4_compressBound(str.size());
            std::string compressed(maxCompressedSize + sizeof(int), '\0');
            // 压缩数据
            int compressedSize = LZ4_compress_default(str.data(), &compressed[sizeof(int)], str.size(), maxCompressedSize);
            if (compressedSize <= 0) {
                throw std::runtime_error("Compression failed");
            }
            // 存储原始大小
            *reinterpret_cast<int*>(&compressed[0]) = str.size();
            // 调整大小以匹配实际压缩后的大小
            compressed.resize(compressedSize + sizeof(int));
            row->set_data(compressed.c_str(), compressed.size());
        }
        row->set_op_type(op_type);
    }
    txn->set_client_ip(kLocalIp);
    txMan->SetCommitSequenceNumber(local_csn.fetch_add(1));
    txn->set_client_txn_id(txMan->GetCommitSequenceNumber());
    txn->set_csn(txMan->GetCommitSequenceNumber());
    txn->set_storage_type("mot");

    MOT::TxnManager* txnMan_ptr = nullptr;
    auto csn = txMan->GetCommitSequenceNumber();
    txn_map.insert(csn, txMan, &txnMan_ptr);
    if(txnMan_ptr != nullptr && txnMan_ptr->commit_state == MOT::RC::RC_WAIT) {
        //抢占了别人的 hash map 出现冲突 abort 前一个，然后存储当前的
        txnMan_ptr->commit_state = MOT::RC::RC_ABORT;
        txnMan_ptr->cv.notify_all();
    }

    {
        string* serialized_txn_str_ptr = new string();
//        Gzip(msg.get(), serialized_txn_str_ptr);

        google::protobuf::io::StringOutputStream outputStream(serialized_txn_str_ptr);
        auto res = msg->SerializeToZeroCopyStream(&outputStream);
        MOT_LOG_INFO("send a message to CCaaS, size = %lu, read op num = %ld, write op num = %ld", serialized_txn_str_ptr->size(), read_op_num, write_op_num);

        client_send_message_queue.enqueue(std::move(std::make_unique<send_thread_params>(0, 0, serialized_txn_str_ptr)));
        client_send_message_queue.enqueue(std::move(std::make_unique<send_thread_params>(0, 0, nullptr)));
    }
    return true;
}




void ClientSendThreadMain(uint64_t id) {
    SetCPU();
    MOT_LOG_INFO("线程 ClientSendThreadMain 开始工作 %llu", id);
    std::vector<std::shared_ptr<zmq::socket_t>> client_send_sockets;
    auto client_send_context = std::make_shared<zmq::context_t>(1);
    for(int i = 0; i < kTxnNodeIp.size(); i ++) {
        auto client_send_socket = std::make_shared<zmq::socket_t>(*client_send_context, ZMQ_PUSH);
        client_send_socket->connect("tcp://" + kTxnNodeIp[i] + ":5551");
        client_send_sockets.emplace_back(client_send_socket);
    }

    std::unique_ptr<send_thread_params> params;
    std::unique_ptr<zmq::message_t> msg;
    auto cnt = 0;
    while(!client_init_ok_flag.load()) usleep(200);
    while(true) {
        client_send_message_queue.wait_dequeue(params);
        if(params != nullptr && params->merge_request_ptr != nullptr) {
            msg = std::make_unique<zmq::message_t>(static_cast<void*>(const_cast<char*>(params->merge_request_ptr->data())),
                    params->merge_request_ptr->size(), string_free, static_cast<void*>(params->merge_request_ptr));
            client_send_sockets[cnt]->send(*(msg));
            cnt = (cnt + 1) % kTxnNodeIp.size();
            // MOT_LOG_INFO("ClientSendThreadMain 发送一个事务");
        }
    }
}

void ClientListenThreadMain(uint64_t id) {
    SetCPU();
    MOT_LOG_INFO("线程 ClientListenThreadMain 开始工作 %llu", id);

    //================PULL==================
    zmq::context_t listen_context(1);
    zmq::socket_t socket_listen(listen_context, ZMQ_PULL);
    int queue_length = 0;
    socket_listen.setsockopt(ZMQ_RCVHWM, &queue_length, sizeof(queue_length));
    socket_listen.bind("tcp://*:5552");
    MOT_LOG_INFO("线程开始工作 ClientListenThreadMain Client PULL tcp://*:5552");
    std::unique_ptr<zmq::message_t> message_ptr;
    while(true) {
        message_ptr = std::make_unique<zmq::message_t>();
        socket_listen.recv(&(*message_ptr));
//        MOT_LOG_INFO("Client PULL Receive a Txn");
        client_listen_message_queue.enqueue(std::move(message_ptr));
        client_listen_message_queue.enqueue(std::move(nullptr));
    }
}

void ClientWorker1ThreadMain(uint64_t id) { // handle result return from Txn node/ Storage Node
    SetCPU();
    MOT_LOG_INFO("线程 ClientWorker1ThreadMain 开始工作 %llu", id);
    std::unique_ptr<zmq::message_t> message_ptr;
    std::unique_ptr<std::string> message_string_ptr;
    std::unique_ptr<proto::Message> msg_ptr;
    MOT::TxnManager* txnMan;
    uint64_t csn = 0;

    while(true) {
        client_listen_message_queue.wait_dequeue(message_ptr);
//        MOT_LOG_INFO("Client 收到一个事务");
        if(message_ptr != nullptr && message_ptr->size() > 0) {
//            message_string_ptr = std::make_unique<std::string>(static_cast<const char *>(message_ptr->data()),message_ptr->size());
//            msg_ptr = std::make_unique<proto::Message>();
//            UnGzip(msg_ptr.get(), message_string_ptr.get());

            msg_ptr = std::make_unique<proto::Message>();
            message_string_ptr = std::make_unique<std::string>(static_cast<const char*>(message_ptr->data()), message_ptr->size());
            google::protobuf::io::ArrayInputStream inputStream(message_string_ptr->data(), message_string_ptr->size());
            msg_ptr->ParseFromZeroCopyStream(&inputStream);

            if(msg_ptr->type_case() == proto::Message::TypeCase::kReplyTxnResultToClient) {
                //wake up local thread and return the commit result
                auto& txn = msg_ptr->reply_txn_result_to_client();
                csn = txn.client_txn_id();
//                 MOT_LOG_INFO("唤醒2 csn %llu %llu", csn, now_to_us());
                if(txn_map.get_value(csn, txnMan)) {
                    if(txn.txn_state() == proto::TxnState::Commit) {
                        txnMan->commit_state = MOT::RC::RC_OK;
                    }
                    else{
                        txnMan->commit_state = MOT::RC::RC_ABORT;
                    }
                    txnMan->cv.notify_all();
                    // MOT_LOG_INFO("唤醒3 csn %llu, txn txnid %llu, txn_state %llu %llu", csn, txnMan->GetCommitSequenceNumber(), txnMan->commit_state, now_to_us());
                    txn_map.remove(csn);
                }
                else {
                    // MOT_LOG_INFO("未找到 csn %llu", csn);
                }
            }
            else if(msg_ptr->type_case() == proto::Message::TypeCase::kClientReadResponse) {
                //wake up local thread and return the read result
            }
            else {
//                client_other_message_queue.enqueue(std::move(msg_ptr));
//                client_other_message_queue.enqueue(std::move(std::make_unique<proto::Message>()));
            }
        }
    }
}

void ClientManagerThreadMain(uint64_t id) { //handle other status
    MOT_LOG_INFO("线程 ClientManagerThreadMain 开始工作 %llu", id);
    client_init_ok_flag.store(true);
    usleep(1000000);
    client_start_flag.store(true);
    auto msg_ptr = std::make_unique<proto::Message>();
    while(true) {
        client_other_message_queue.wait_dequeue(msg_ptr);
    }
}



























//**************************************************************************************************************************************************************************************************
//*****                                                                                                                                                                                        *****
//*****                                                                                                                                                                                        *****
//*****                                                                                      TAAS    Storage                                                                                   *****
//*****                                                                                                                                                                                        *****
//*****                                                                                                                                                                                        *****
//**************************************************************************************************************************************************************************************************

BlockingConcurrentQueue<std::unique_ptr<zmq::message_t>> storage_listen_message_queue;
BlockingConcurrentQueue<std::unique_ptr<send_thread_params>> storage_send_message_queue;
BlockingConcurrentQueue<std::unique_ptr<proto::Message>> storage_other_message_queue;
BlockingConcurrentQueue<std::unique_ptr<proto::Message>> storage_read_queue;

std::atomic<bool> storage_init_ok_flag(false), storage_start_flag(false);
std::atomic<uint64_t> update_epoch(0), current_epoch(5), total_commit_txn_num(0);

uint64_t start_time_ll, start_physical_epoch = 1, cache_size = 10000;
struct timeval start_time;

proto::Node dest_node, src_node;

void SendPullRequest(uint64_t epoch_id) {
    auto msg = std::make_unique<proto::Message>();
    auto* request = msg->mutable_storage_pull_request();
    request->mutable_send_node()->CopyFrom(src_node);
    request->mutable_recv_node()->CopyFrom(dest_node);
    request->set_epoch_id(epoch_id);
    auto serialized_txn_str_ptr = std::make_unique<std::string>();
//    Gzip(msg.get(), serialized_txn_str_ptr.get());
    google::protobuf::io::StringOutputStream outputStream(serialized_txn_str_ptr.get());
    auto res = msg->SerializeToZeroCopyStream(&outputStream);
    storage_send_message_queue.enqueue(std::move(std::make_unique<send_thread_params>(0, 0, serialized_txn_str_ptr.release())));
    storage_send_message_queue.enqueue(std::move(std::make_unique<send_thread_params>(0, 0, nullptr)));
}

bool HandlePackTxnx(proto::Message* msg) {
    if(msg->type_case() == proto::Message::TypeCase::kStoragePullResponse) {
        auto* response = &(msg->storage_pull_response());
        if(response->result() == proto::Result::Fail) {
            //Send pull request to another server or wait a few seconds
            return true;
        }
        for(int i = 0; i < (int) response->txns_size(); i ++) {
            auto txn = std::make_unique<proto::Transaction>(std::move(std::move(response->txns(i))));
            storage_update_queue.enqueue(std::move(txn));
        }
        storage_update_queue.enqueue(std::move(std::make_unique<proto::Transaction>()));
    }
    else {
        auto* response = &(msg->storage_push_response());
        for(int i = 0; i < (int) response->txns_size(); i ++) {
            auto txn = std::make_unique<proto::Transaction>(std::move(std::move(response->txns(i))));
            storage_update_queue.enqueue(std::move(txn));
        }
        storage_update_queue.enqueue(std::move(std::make_unique<proto::Transaction>()));
    }
}

void StorageMessageManagerThreadMain(uint64_t id) { // handle result return from Txn node/ Storage Node
    SetCPU();
    MOT_LOG_INFO("线程 StorageMessageManagerThreadMain 开始工作 %llu", id);
    MOT::SessionContext* session_context = MOT::GetSessionManager()->
                                           CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, INVALID_CONNECTION_ID);
    MOT::TxnManager* txn_manager = session_context->GetTxnManager();
    std::unique_ptr<zmq::message_t> message_ptr;
    std::unique_ptr<proto::Message> msg_ptr;
    std::unique_ptr<std::string> message_string_ptr;
    while(true) {
        storage_listen_message_queue.wait_dequeue(message_ptr);
        if(message_ptr != nullptr && message_ptr->size() > 0) {
//            message_string_ptr = std::make_unique<std::string>(static_cast<const char*>(message_ptr->data()), message_ptr->size());
//            msg_ptr = std::make_unique<proto::Message>();
//            UnGzip(msg_ptr.get(), message_string_ptr.get());

            msg_ptr = std::make_unique<proto::Message>();
            assert(msg_ptr != nullptr);
//            message_string_ptr = std::make_unique<std::string>(static_cast<const char*>(message_ptr->data()), message_ptr->size());
            google::protobuf::io::ArrayInputStream inputStream(message_ptr->data(), message_ptr->size());
//            google::protobuf::io::ArrayInputStream inputStream(message_string_ptr->data(), message_string_ptr->size());
            msg_ptr->ParseFromZeroCopyStream(&inputStream);

            if(msg_ptr->type_case() == proto::Message::TypeCase::kStoragePullResponse || msg_ptr->type_case() == proto::Message::TypeCase::kStoragePushResponse) {
                if(msg_ptr->type_case() == proto::Message::TypeCase::kStoragePullResponse) {
                    auto* response = &(msg_ptr->storage_pull_response());
                    if(response->result() == proto::Result::Fail) {
                        //Send pull request to another server or wait a few seconds
                        continue;
                    }
                    for(int i = 0; i < (int) response->txns_size(); i ++) {
                        auto txn = std::make_unique<proto::Transaction>(response->txns(i));
                        storage_update_queue.enqueue(std::move(txn));
                    }
                    storage_update_queue.enqueue(std::move(std::make_unique<proto::Transaction>()));
                }
                else {
                    auto* response = &(msg_ptr->storage_push_response());
                    for(int i = 0; i < (int) response->txns_size(); i ++) {
                        auto txn = std::make_unique<proto::Transaction>(response->txns(i));
                        storage_update_queue.enqueue(std::move(txn));
                    }
                    storage_update_queue.enqueue(std::move(std::make_unique<proto::Transaction>()));
                }
            }
            else {
//                storage_other_message_queue.enqueue(std::move(msg_ptr));
//                storage_other_message_queue.enqueue(std::move(std::make_unique<proto::Message>()));
            }
        }
    }
}

void StorageUpdaterThreadMain(uint64_t id) {
    MOT_LOG_INFO("线程 StorageUpdaterThreadMain 开始工作 %llu", id);
    MOT::SessionContext* session_context = MOT::GetSessionManager()->
        CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, INVALID_CONNECTION_ID);
    MOT::TxnManager* txn_manager = session_context->GetTxnManager();
    std::map<MOT::Row*, bool> lock_map, should_lock_map;
    std::vector<MOT::Table*> vec_table;
    std::vector<MOT::Row*> vec_row;
    size_t key_length, key_id;
    std::string key_str;
    MOT::Table* table;
    MOT::Row* row;
    MOT::Key* key;
    std::shared_ptr<MOT::Key> key_ptr;
    void* buf;
    MOT::RC res;
    bool commit_res = false;
    uint64_t commit_txn_num = 0;

    auto txn_ptr = std::make_unique<proto::Transaction>();

    uint64_t cnt = 0;
    while(true) {
        txn_ptr.reset(nullptr);
//        while(!storage_update_queue.try_dequeue(txn_ptr)) usleep(100);
        storage_update_queue.wait_dequeue(txn_ptr);

        // handle txn read request
        if (txn_ptr->row_size() > 0) {
            cnt ++;
            if(cnt % 100 == 0) {
                MOT_LOG_INFO("Storage process %llu 个事务", cnt);
            }
            //             MOT_LOG_INFO("Storage 收到一个事务");
            txn_manager->CleanTxn();
            vec_row.clear();
            vec_table.clear();
            lock_map.clear();
            should_lock_map.clear();
            commit_res = true;
            key_id = 0;


            txn_manager->SetCommitSequenceNumber(txn_ptr->csn());
            for (int i = 0; i < txn_ptr->row_size(); i++) {
                auto *row_it = &(txn_ptr->row(i));
                MOT::Table* table = MOTAdaptor::m_engine->GetTableManager()->GetTable(row_it->table_name());
                if (table != nullptr) {
                    key_length = row_it->key().length();
                    buf = MOT::MemSessionAlloc(key_length);
                    if (buf == nullptr)
                        Assert(false);
                    key = new (buf) MOT::Key(key_length);
                    key->CpKey((uint8_t*)row_it->key().c_str(), key_length);
                    key_str = key->GetKeyStr();
                    res = table->FindRow(key, row, 0);
                    MOT::MemSessionFree(buf);
                    if (res != MOT::RC_OK) {  // an error
                        // look up fail!~
                        row = nullptr;
                    }
                    if (row != nullptr && (row_it->op_type() == proto::OpType::Update ||
                                              row_it->op_type() == proto::OpType::Delete)) {
                        should_lock_map[row] = true;
                    }
                }
                else {
                    table = nullptr, row = nullptr;
                    MOT_LOG_INFO("Taas Storage Updater 3010 table is null %s", row_it->table_name().c_str());
                }
                vec_row.push_back(row);
                vec_table.push_back(table);
            }

            for (auto row_it = should_lock_map.begin(); row_it != should_lock_map.end(); ++row_it) {
                auto row_temp = row_it->first;
                if (!lock_map[row_temp]) {
                    row_temp->LockRow();
                    lock_map[row_temp] = true;
                }
            }

            // process operation
            key_id = 0;
            //            for (auto row_it = txn_ptr->row().begin(); row_it != txn_ptr->row().end(); ++row_it) {
            for (int i = 0; i < txn_ptr->row_size(); i++) {
                auto* row_it = &(txn_ptr->row(i));
                row = vec_row[key_id];
                table = vec_table[key_id];
                key_id++;
                if (table == nullptr) {
                    continue;
                }
                if (row == nullptr) {  // insert or delete by others before this epoch
                    if (row_it->op_type() == proto::OpType::Insert) {
                        row = table->CreateNewRow();
                        row->CopyData((uint8_t*)row_it->data().c_str(), table->GetTupleSize());
                        res = table->InsertRow(row, txn_manager);
//                        if ((res != MOT::RC_OK) && (res != MOT::RC_UNIQUE_VIOLATION)) {
//                            MOT_REPORT_ERROR(MOT_ERROR_OOM,
//                                "Insert Row ",
//                                "Failed to insert new row for table %s",
//                                table->GetLongTableName().c_str());
//                        }
//                        if (res != MOT::RC_OK) {
//                            MOT_REPORT_ERROR(MOT_ERROR_OOM,
//                                "Taas Insert Row ",
//                                "Failed to insert new row for table %s",
//                                table->GetLongTableName().c_str());
//                        }
                    } else {
                        MOT_LOG_INFO("Storage Error Update/Delete a NULL row txn %llu key_id %llu op_type %llu",
                            txn_ptr->client_txn_id(),
                            key_id,
                            row_it->op_type());
                    }
                    /* code */
                } else {
                    if (row_it->op_type() == proto::OpType::Delete) {
                        row->GetPrimarySentinel()->SetDirty();
                        row->SetCSN_Delete(txn_ptr->csn());
                        // MOT_LOG_INFO("Storage Delete txn %llu key_id %llu op_type %llu", txn_ptr->client_txn_id(), key_id++, row_it->op_type());
                    } else if (row_it->op_type() == proto::OpType::Update) {
                        std::string compressed = row_it->data();
                        int originalSize = *reinterpret_cast<const int*>(compressed.data());
                        // 解压缩
                        std::string decompressed(originalSize, '\0');
                        int decompressedSize = LZ4_decompress_safe(&compressed[sizeof(int)], &decompressed[0],
                            compressed.size() - sizeof(int), originalSize);
                        if (decompressedSize < 0) {
                            throw std::runtime_error("Decompression failed");
                        }
                        row->CopyData((uint8_t*)decompressed.c_str(), table->GetTupleSize());
                        // for (auto col_it = row_it->column().begin(); col_it != row_it->column().end(); ++col_it) {
                        //     row->SetValueVariable(col_it->id(), col_it->value().c_str(), col_it->value().length()); //
                        // }
                        row->SetCSN_Update(txn_ptr->csn());
                        // MOT_LOG_INFO("Storage Update txn %llu key_id %llu op_type %llu", txn_ptr->client_txn_id(), key_id++, row_it->op_type());
                    } else if (row_it->op_type() == proto::OpType::Insert) {  /// never should be happen
//                        row = table->CreateNewRow();
//                        std::string compressed = row_it->data();
//                        int originalSize = *reinterpret_cast<const int*>(compressed.data());
//                        // 解压缩
//                        std::string decompressed(originalSize, '\0');
//                        int decompressedSize = LZ4_decompress_safe(&compressed[sizeof(int)], &decompressed[0],
//                            compressed.size() - sizeof(int), originalSize);
//                        if (decompressedSize < 0) {
//                            throw std::runtime_error("Decompression failed");
//                        }
//                        row->CopyData((uint8_t*)decompressed.c_str(), table->GetTupleSize());
////                        row->CopyData((uint8_t*)row_it->data().c_str(), table->GetTupleSize());
//                        res = table->InsertRow(row, txn_manager);
//                        if ((res != MOT::RC_OK) && (res != MOT::RC_UNIQUE_VIOLATION)) {
//                            MOT_REPORT_ERROR(MOT_ERROR_OOM,
//                                "Insert Row ",
//                                "Failed to insert new row for table %s",
//                                table->GetLongTableName().c_str());
//                        }
//                        if (res != MOT::RC_OK) {
//                            MOT_REPORT_ERROR(MOT_ERROR_OOM,
//                                "Taas Insert Row ",
//                                "Failed to insert new row for table %s",
//                                table->GetLongTableName().c_str());
//                        }
                        // MOT_LOG_INFO("Storage Insert %llu key_id %llu op_type %llu", txn_ptr->client_txn_id(), key_id++, row_it->op_type());
                    }
                }
            }
             txn_manager->SetCommitSequenceNumber(txn_ptr->csn());
            if (txn_manager->TaasLogCommit() != MOT::RC::RC_OK) {
                // MOT_LOG_INFO("TaasLogCommit Failed");
                commit_res = false;
            }  // write change时会写入csn

            // release
            key_id = 0;
            for (auto row_it = lock_map.begin(); row_it != lock_map.end(); ++row_it) {
                auto row_temp = row_it->first;
                if (lock_map[row_temp]) {
                    row_temp->ReleaseRow();
                    lock_map[row_temp] = false;
                }
            }
//            for (int i = 0; i < txn_ptr->row_size(); i++) {
//                auto* row_it = &(txn_ptr->row(i));
//                //            for (auto row_it = txn_ptr->row().begin(); row_it != txn_ptr->row().end(); ++row_it) {
//                row = vec_row[key_id];
//                table = vec_table[key_id];
//                key_id++;
//                if (table == nullptr) {
//                    continue;
//                }
//                if (row == nullptr) {  // an error
//                    /* code */
//                }
//                if (row != nullptr &&
//                    (row_it->op_type() == proto::OpType::Update || row_it->op_type() == proto::OpType::Delete)) {
//                    if (lock_map[row]) {
//                        row->ReleaseRow();
//                        lock_map[row] = false;
//                        // MOT_LOG_INFO("txn ReleaseRow %llu key_id %llu op_type %llu", txn_ptr->client_txn_id(), key_id++, row_it->op_type());
//                    } else {
//                        // MOT_LOG_INFO("txn ReleaseRow else");
//                    }
//                }
//            }

            vec_row.clear();
            vec_table.clear();
            lock_map.clear();
            //            MOT_LOG_INFO("Storage 提交 txn num %llu", commit_txn_num);
            if (!commit_res) {
                ///
            } else {
                commit_txn_num++;
                if (commit_txn_num % 100 == 0) {
                    auto num = total_commit_txn_num.fetch_add(commit_txn_num);
                    commit_txn_num = 0;
                    MOT_LOG_INFO("共提交 txn num %llu", num);
                }
            }
            txn_ptr.release();
        }
    }
}

void StorageReaderThreadMain(uint64_t id) {
    MOT::SessionContext* session_context = MOT::GetSessionManager()->
                                           CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, INVALID_CONNECTION_ID);
    MOT::TxnManager* txn_manager = session_context->GetTxnManager();
    MOT_LOG_INFO("线程 StorageReaderThreadMain 开始工作 %llu", id);
    auto msg_ptr = std::make_unique<proto::Message>();
    while(true) {
        storage_read_queue.wait_dequeue(msg_ptr);
        //handle client read request

        //send client read response
    }
}

void StorageInit() {

}

uint64_t GetSleeptime(){
    uint64_t sleep_time;
    struct timeval current_time;
    uint64_t current_time_ll;
    gettimeofday(&current_time, NULL);
    current_time_ll = current_time.tv_sec * 1000000 + current_time.tv_usec;
    sleep_time = current_time_ll - (start_time_ll + (long)(current_epoch.load() - start_physical_epoch) * kSleepTime);
    if(sleep_time >= kSleepTime){
        MOT_LOG_INFO("start time : %llu, current time : %llu, 差值 %llu ,sleep time : %d", start_time_ll, current_time_ll, sleep_time, 0);
        return 0;
    }
    else{
        // MOT_LOG_INFO("start time : %llu, current time : %llu, 差值 %llu, sleep time : %llu", start_time_ll, current_time_ll, sleep_time, ksleeptime - sleep_time);
        return kSleepTime - sleep_time;
    }
}

void StorageManagerThreadMain(uint64_t id) { //handle other status
    MOT_LOG_INFO("线程 StorageManagerThreadMain 开始工作 %llu", id);
    StorageInit();
    storage_init_ok_flag.store(true);
    usleep(1000000);
    storage_start_flag.store(true);
//    auto msg_ptr = std::make_unique<proto::Message>();

    zmq::message_t message;
    zmq::context_t context(1);
    zmq::socket_t request_puller(context, ZMQ_PULL);
    request_puller.bind("tcp://*:5546");
    MOT_LOG_INFO("Storage 等待接受同步消息");
    request_puller.recv(&message);
    while(true) usleep(1000000000);
//    uint64_t sleep_time = static_cast<uint64_t>((((start_time.tv_sec / 60) + 1) * 60) * 1000000);
//    usleep(sleep_time - start_time_ll);
//    gettimeofday(&start_time, NULL);
//    start_time_ll = start_time.tv_sec * 1000000 + start_time.tv_usec;
//    usleep(50000);
//    auto epoch_mod = current_epoch.load() % cache_size;
//    while(true) {
//        usleep(GetSleeptime());
//        current_epoch.fetch_add(1);
//        epoch_mod = epoch_mod + 1 % cache_size;
//        shoule_update_txn_num[epoch_mod] = std::make_unique<std::atomic<uint64_t>>(0);
//        updated_txn_num[epoch_mod] = std::make_unique<std::atomic<uint64_t>>(0);
//        // SendPullRequest(current_epoch.load() - 5); //delay 5 epoch
//    }
}

void StorageWorker1ThreadMain(uint64_t id) { // Listen SUB
    SetCPU();
    MOT_LOG_INFO("线程 StorageWorker1ThreadMain Listener Subscribe Mode 开始工作 %llu", id);
    MOT::SessionContext* session_context = MOT::GetSessionManager()->
                                           CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, INVALID_CONNECTION_ID);
    MOT::TxnManager* txn_manager = session_context->GetTxnManager();
    while(storage_init_ok_flag.load() == false) usleep(200);
    zmq::context_t listen_context(1);
    zmq::socket_t socket_listen(listen_context, ZMQ_SUB);
    int queue_length = 0;
    socket_listen.setsockopt(ZMQ_SUBSCRIBE, "", 0);
    socket_listen.setsockopt(ZMQ_RCVHWM, &queue_length, sizeof(queue_length));
    for(int i = 0; i < kTxnNodeIp.size(); i ++) {
        socket_listen.connect("tcp://" + kTxnNodeIp[i] + ":5556");
        MOT_LOG_INFO("线程开始工作 ListenThread %s", ("tcp://" + kTxnNodeIp[i] + ":5556").c_str());
    }

    std::unique_ptr<zmq::message_t> message_ptr;
    for(;;) {
        message_ptr = std::make_unique<zmq::message_t>();
        socket_listen.recv(&(*message_ptr));
//        MOT_LOG_INFO("Listen SUB receive a message");
        if(!storage_listen_message_queue.enqueue(std::move(message_ptr))) assert(false);
        if(!storage_listen_message_queue.enqueue(std::move(std::make_unique<zmq::message_t>()))) assert(false);
    }
}





void StorageSendThreadMain(uint64_t id) { // PULL PUSH
    MOT::SessionContext* session_context = MOT::GetSessionManager()->
                                           CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, INVALID_CONNECTION_ID);
    MOT::TxnManager* txn_manager = session_context->GetTxnManager();
    MOT_LOG_INFO("线程 StorageSendThreadMain 开始工作 %llu", id);
    auto storage_send_context = std::make_shared<zmq::context_t>(1);
    auto storage_send_socket = std::make_shared<zmq::socket_t>(*storage_send_context, ZMQ_PUSH);
    storage_send_socket->connect("tcp://" + kTxnNodeIp[0] + ":5553");
    std::unique_ptr<send_thread_params> params;
    std::unique_ptr<zmq::message_t> msg;
    while(storage_init_ok_flag.load() == false) usleep(200);
    while(true) {
        storage_send_message_queue.wait_dequeue(params);
        if(params != nullptr && params->merge_request_ptr != nullptr) {
            msg = std::make_unique<zmq::message_t>(static_cast<void*>(const_cast<char*>(params->merge_request_ptr->data())),
                    params->merge_request_ptr->size(), string_free, static_cast<void*>(params->merge_request_ptr));
            storage_send_socket->send(*(msg));
        }
    }
}

void StorageListenThreadMain(uint64_t id) {// PULL PUSH
    MOT::SessionContext* session_context = MOT::GetSessionManager()->
                                           CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, INVALID_CONNECTION_ID);
    MOT::TxnManager* txn_manager = session_context->GetTxnManager();
    MOT_LOG_INFO("线程 StorageListenThreadMain 开始工作 %llu", id);
    auto storage_listen_context = std::make_shared<zmq::context_t>(2);
    auto storage_listen_socket = std::make_shared<zmq::socket_t>(*storage_listen_context, ZMQ_PULL);
    storage_listen_socket->bind("tcp://*:5554");
    while(storage_init_ok_flag.load() == false) usleep(200);
    std::unique_ptr<zmq::message_t> message_ptr;
    while(true) {
        message_ptr = std::make_unique<zmq::message_t>();
        storage_listen_socket->recv(&(*message_ptr));
        storage_listen_message_queue.enqueue(std::move(message_ptr));
        storage_listen_message_queue.enqueue(std::move(std::make_unique<zmq::message_t>()));
    }
}




















///************************************************************************************************************************************************************************//
///************************************************************************************************************************************************************************//
///******************************************************************************TaaS--Local-Implementation****************************************************************//
///************************************************************************************************************************************************************************//
///************************************************************************************************************************************************************************//

class AtomicCounters {
private:
    std::vector<std::unique_ptr<std::atomic<uint64_t> > > vec;
    uint64_t _size{};
public:

    AtomicCounters() = default;

    explicit AtomicCounters(uint64_t size = 8);

    void Init(uint64_t size = 8);

    uint64_t IncCount(const uint64_t &index, const uint64_t &value) {
        return vec[index % _size]->fetch_add(value);
    }

    uint64_t DecCount(const uint64_t &index, const uint64_t &value) {
        return vec[index % _size]->fetch_sub(value);
    }

    void SetCount(const uint64_t &value) {
        for (auto &i: vec) {
            i->store(value);
        }
    }

    void SetCount(const uint64_t &index, const uint64_t &value) {
        vec[index % _size]->store(value);
    }

    uint64_t GetCount() {
        uint64_t ans = 0;
        for (auto &i: vec) {
            ans += i->load();
        }
        return ans;
    }

    uint64_t GetCount(const uint64_t &index) {
        return vec[index % _size]->load();
    }

    void Clear(const uint64_t value = 0) {
        (void) value;
        for (auto &i: vec) {
            i->store(0);
        }
    }

    void Resize(const uint64_t &size) {
        if (size <= _size) return;
        vec.resize(size);
        for (uint64_t i = _size; i < size; i++) {
            vec.emplace_back(std::make_unique<std::atomic<uint64_t>>(0));
        }
    }
};


class AtomicCounters_Cache {
private:
    std::vector<std::unique_ptr<std::vector<std::unique_ptr<std::atomic<uint64_t>>>>> vec;
    uint64_t _size{}, epoch_mod{}, _length{};
public:

    AtomicCounters_Cache() = default;

    explicit AtomicCounters_Cache(uint64_t length = 1000, uint64_t size = 2);

    void Init(uint64_t length = 1000, uint64_t size = 2, uint64_t value = 0);

    uint64_t IncCount(const uint64_t &epoch, const uint64_t &index, const uint64_t &value) {
        return (*vec[epoch % _length])[index % _size]->fetch_add(value);
    }

    uint64_t DecCount(const uint64_t &epoch, const uint64_t &index, const uint64_t &value) {
        return (*vec[epoch % _length])[index % _size]->fetch_sub(value);
    }

    void SetCount(const uint64_t &epoch, const uint64_t &index, const uint64_t &value) {
        (*vec[epoch % _length])[index % _size]->store(value);
    }

    void SetCount(const uint64_t &epoch, const uint64_t &value) {
        auto &v = (*vec[epoch % _length]);
        for (auto &i: v) {
            i->store(value);
        }
    }

    uint64_t GetCount(const uint64_t &epoch, const uint64_t &index) {
        return (*vec[epoch % _length])[index % _size]->load();
    }

    uint64_t GetCount(const uint64_t &epoch) {
        auto &v = (*vec[epoch % _length]);
        uint64_t ans = 0;
        for (auto &i: v) {
            ans += i->load();
        }
        return ans;
    }

    void Clear(const uint64_t &epoch, const uint64_t &value = 0) {
        (void) value;
        auto &v = (*vec[epoch % _length]);
        for (auto &i: v) {
            i->store(value);
        }
    }

    void Clear() {
        for (auto &v : vec) {
            for (auto &i: *v) {
                i->store(0);
            }
        }
    }

};

AtomicCounters::AtomicCounters(uint64_t size){
    _size = size;
    for(int i = 0; i < (int)size; i ++) {
        vec.emplace_back(std::make_unique<std::atomic<uint64_t>>(0));
    }
}

void AtomicCounters::Init(uint64_t size){
    if(size < _size) return;
    _size = size;
    vec.resize(size);
    for(int i = 0; i < (int)size; i ++) {
        vec[i] = std::make_unique<std::atomic<uint64_t>>(0);
    }
}

AtomicCounters_Cache::AtomicCounters_Cache(uint64_t length, uint64_t size){
    _size = size;
    _length = length;
    vec.resize(length);
    for(int i = 0; i < (int)length; i ++) {
        vec[i] = std::make_unique<std::vector<std::unique_ptr<std::atomic<uint64_t>>>>();
        auto &v = (*vec[i]);
        v.resize(size);
        for(uint64_t j = 0; j < size; j ++) {
            v[j] = std::make_unique<std::atomic<uint64_t>>(0);
        }
    }
}

void AtomicCounters_Cache::Init(uint64_t length, uint64_t size, uint64_t value) {
    if(size < _size && length < _length) return ;
    _size = size;
    _length = length;
    vec.resize(length);
    for(unsigned int i = 0; i < length; i ++) {
        vec[i] = std::make_unique<std::vector<std::unique_ptr<std::atomic<uint64_t>>>>();
        auto p = &(*vec[i]);
        assert((uint64_t)p != 0x1);
        auto &v = (*(vec[i]));
        v.resize(size);
        for(unsigned int j = 0; j < size; j ++) {
            v[j] = std::make_unique<std::atomic<uint64_t>>(value);
        }
    }
}


template<typename key, typename value, typename pointer>
class concurrent_crdt_unordered_map {
public:
    typedef typename std::unordered_map<key, value>::iterator map_iterator;
    typedef typename std::unordered_map<key, value>::size_type size_type;

    bool insert(const key &k, const value &v, pointer &ptr) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        bool result = true;
        std::unique_lock<std::mutex> lock(GetMutexRef(k));
        map_iterator iter = _map_temp.find(k);
        if (iter == _map_temp.end()) {
            _map_temp[k] = v;
            ptr = "0";
            result = true;
        } else {
            if (iter->second > v) {
                ptr = _map_temp[k];
                _map_temp[k] = v;
                result = true;
            }
            else if(iter->second == v){
                ptr = "0";
                result = true;
            }
            else{
                ptr = v;
                result = false;
            }
        }
        lock.unlock();
        return result;
    }

    void insert(const key &k, const value &v){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(GetMutexRef(k));
        _map_temp[k] = v;
    }


    void remove(const key &k, const value &v) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(GetMutexRef(k));
        map_iterator iter = _map_temp.find(k);
        if (iter != _map_temp.end()) {
            if (iter->second == v) {
                //if the abort txn has insert row and has not been modifid by ohters
                //then remove it from map;or keep it
                _map_temp.erase(iter);
            }
        }
        lock.unlock();
    }

    void remove(const key &k) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(GetMutexRef(k));
        map_iterator iter = _map_temp.find(k);
        if (iter != _map_temp.end()) {
            _map_temp.erase(iter);
        }
        lock.unlock();
    }

    void clear() {
        for(uint64_t i = 0; i < _N; i ++){
            std::unique_lock<std::mutex> lock(_mutex[i]);
        }
        for(uint64_t i = 0; i < _N; i ++){
            _map[i].clear();
        }
    }

    void unsafe_clear() {
        for(uint64_t i = 0; i < _N; i ++){
            _map[i].clear();
        }
    }

    bool contain(const key &k, const value &v){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(GetMutexRef(k));
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            if(iter->second == v){
                return true;
            }
        }
        return false;
    }

    bool contain(const key &k){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(GetMutexRef(k));
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            return true;
        }
        return false;
    }

    bool unsafe_contain(const key &k, value &v){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            if(iter->second == v){
                return true;
            }
        }
        return false;
    }

    size_type size() {
        size_type ans = 0;
        for(uint64_t i = 0; i < _N; i ++){
            std::unique_lock<std::mutex> lock(_mutex[i]);
            ans += _map[i].size();
        }
        return ans;
    }

    bool getValue(std::vector<key> &keys, std::vector<value> &values) {
        for(uint64_t i = 0; i < _N; i ++){
            std::unique_lock<std::mutex> lock(_mutex[i]);
        }
        for(uint64_t i = 0; i < _N; i ++){
            for(auto p : _map[i]) {
                keys.push_back(p.first);
                values.push_back(p.second);
            }
        }
        return true;
    }

protected:
    inline std::unordered_map<key, value>& GetMapRef(const key k){ return _map[(_hash(k) % _N)]; }
    inline std::unordered_map<key, value>& GetMapRef(const key k) const { return _map[(_hash(k) % _N)]; }
    inline std::mutex& GetMutexRef(const key k) { return _mutex[(_hash(k) % _N)]; }
    inline std::mutex& GetMutexRef(const key k) const {return _mutex[(_hash(k) % _N)]; }

private:
    const static uint64_t _N = 101;//101 337 599 733 911 1217 12281 122777 prime
    std::hash<key> _hash;
    std::unordered_map<key, value> _map[_N];
    std::mutex _mutex[_N];
};

template<typename key, typename value>
class concurrent_unordered_map {
public:
    typedef typename std::unordered_map<key, value>::iterator map_iterator;
    typedef typename std::unordered_map<key, value>::size_type size_type;

    void insert(const key &k, const value &v) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(GetMutexRef(k));
        _map_temp[k] = v;
    }

    bool insertState(const key &k, const value &v) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(GetMutexRef(k));
        map_iterator iter = _map_temp.find(k);
        if (iter != _map_temp.end()) {
            if(iter->second == v){
                return true;
            } else {
                return false;
            }
        }
        _map_temp[k] = v;
        return true;
    }

    void remove(const key &k, const value &v) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock( GetMutexRef(k));
        map_iterator iter = _map_temp.find(k);
        if (iter != _map_temp.end()) {
            if (iter->second == v) {
                //if the abort txn has insert row and has not been modifid by ohters
                //then remove it from map;or keep it
                _map_temp.erase(iter);
            }
        }
        lock.unlock();
    }

    void remove(const key &k) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock( GetMutexRef(k));
        map_iterator iter = _map_temp.find(k);
        if (iter != _map_temp.end()) {
            _map_temp.erase(iter);
        }
        lock.unlock();
    }

    void clear() {
        for(uint64_t i = 0; i < _N; i ++){
            std::unique_lock<std::mutex> lock(_mutex[i]);
        }
        for(uint64_t i = 0; i < _N; i ++){
            _map[i].clear();
        }
    }

    void unsafe_clear() {
        for(uint64_t i = 0; i < _N; i ++){
            _map[i].clear();
        }
    }

    bool contain(key &k, value &v){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(GetMutexRef(k));
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            if(iter->second == v){
                return true;
            }
        }
        return false;
    }

    bool getValue(const key &k, value &v){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(GetMutexRef(k));
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            v = _map_temp[k];
            return true;
        }
        v = value();
        return false;
    }

    bool try_lock(const key &k, value &v) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(GetMutexRef(k));
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            if(_map_temp[k] == v) { /// locked already by itself
                return true;
            }
            else if(_map_temp[k] == "" || _map_temp[k] == "0"){
                _map_temp[k] = v;
                return true;
            }
            else { /// locked already by others
                return false;
            }
        }
        _map_temp[k] = v;
        return true;
    }


    value unlock(const key &k, value &v) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(GetMutexRef(k));
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            if(_map_temp[k] == v) { /// locked already by itself
                _map_temp[k] = "";
            }
            else if(_map_temp[k] == "" || _map_temp[k] == "0" || _map_temp[k] == "-1"){
                _map_temp[k] = "";
            }
            else { /// locked already by others
                /// do nothing
            }
        }
        value tmp = _map_temp[k];
        return tmp;
    }

    uint64_t countLock(){
        uint64_t count = 0;
        for(uint64_t i = 0; i < _N; i ++){
            std::unique_lock<std::mutex> lock(_mutex[i]);
            for (const auto& pair : _map[i]) {
                if (!pair.second.empty() && pair.second != "" && pair.second != "-1" && pair.second != "0") {
                    count++;
                }
            }
        }
        return count;
    }



    bool contain(const key &k){
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            return true;
        }
        return false;
    }

    bool unsafe_contain(const key &k, value &v){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            if(iter->second == v){
                return true;
            }
        }
        return false;
    }

    size_type size() {
        size_type ans = 0;
        for(uint64_t i = 0; i < _N; i ++){
            std::unique_lock<std::mutex> lock(_mutex[i]);
            ans += _map[i].size();
        }
        return ans;
    }

    bool getValue(std::basic_string<char> keys, std::vector<value> &values) {
        for(uint64_t i = 0; i < _N; i ++){
            std::unique_lock<std::mutex> lock(_mutex[i]);
        }
        for(uint64_t i = 0; i < _N; i ++){
            for(auto p : _map[i]) {
                keys.push_back(p.first);
                values.push_back(p.second);
            }
        }
        return true;
    }

    bool getValue(std::vector<key> &keys, std::vector<value> &values) {
        for(uint64_t i = 0; i < _N; i ++){
            std::unique_lock<std::mutex> lock(_mutex[i]);
        }
        for(uint64_t i = 0; i < _N; i ++){
            for(auto p : _map[i]) {
                keys.push_back(p.first);
                values.push_back(p.second);
            }
        }
        return true;
    }

protected:
    inline std::unordered_map<key, value>& GetMapRef(const key k){ return _map[(_hash(k) % _N)]; }
    inline std::unordered_map<key, value>& GetMapRef(const key k) const { return _map[(_hash(k) % _N)]; }
    inline std::mutex& GetMutexRef(const key k) { return _mutex[(_hash(k) % _N)]; }
    inline std::mutex& GetMutexRef(const key k) const {return _mutex[(_hash(k) % _N)]; }

private:
    const static uint64_t _N = 101;//1217 12281 122777 prime
    std::hash<key> _hash;
    std::unordered_map<key, value> _map[_N];
    std::mutex _mutex[_N];
};












///TaaSContext

template<typename T>
using  BlockingConcurrentQueue = moodycamel::BlockingConcurrentQueue<T>;
//using  BlockingConcurrentQueue = BlockingMPMCQueue<T>;

template<typename T>
using  MessageBlockingConcurrentQueue = moodycamel::BlockingConcurrentQueue<T>;

namespace Taas {
enum ServerMode {
    Taas = 1,
    LevelDB = 2,
    HBase = 3,
    MultiModelClient = 4,
};
enum TaasMode {
    MultiMaster = 1,
    Shard = 2,
    TwoPC = 3,
    MultiModel = 4
};

class TaasContext {
public:
    explicit TaasContext() {
        //            GetTaaSServerInfo("../TaaS_config.xml");
    }
    //        explicit TaasContext(const std::string& TaaS_config_file_path, const std::string& Storage_config_file_path) {
    //            GetTaaSServerInfo(TaaS_config_file_path);
    //        }
    /// 1: TaaS server, 2: leveldb server, 3:hbase server
    static ServerMode server_type;

    ///TaaS server config
    static TaasMode taasMode;
    static std::vector<std::string> kServerIp;
    static uint64_t kTxnNodeNum, kBackUpNum ;
    static uint64_t kIndexNum, kEpochSize_us, txn_node_ip_index,
        kShardNum, kReplicaNum,
        kDurationTime_us,
        kCacheMaxLength, kDelayEpochNum, print_mode_size;
    static uint64_t kMergeThreadNum, kEpochTxnThreadNum, kEpochMessageThreadNum;
    static uint64_t kTestClientNum, kTestKeyRange, kTestTxnOpNum;
    static uint64_t kHandleEpochMessageNumOfEachTraversal, kHandleTxnMessageNumOfEachTraversal, kSafeEpochDistance;

    static bool is_read_repeatable, is_snap_isolation,
        is_breakdown, is_sync_start,
        is_cache_server_available;
    static std::string glog_path;

    static void GetTaaSServerInfo(const std::string &config_file_path = "/tmp/zwx/TaaS_config.xml");

    static std::string Print();
};

class StorageContext {
public:
    explicit StorageContext() {
        //            GetStorageInfo("../Storage_config.xml");
    }
    //        explicit StorageContext(const std::string& Storage_config_file_path) {
    //            GetStorageInfo(Storage_config_file_path);
    //        }

    /// storage info
    //        static bool is_tikv_enable = false, is_leveldb_enable = false, is_hbase_enable = false, is_mot_enable = true, is_nebula_enable = false;
    //        static std::string kMasterIp, kPrivateIp, kTiKVIP, kLevelDBIP, kHbaseIP;
    //        static uint64_t kTikvThreadNum = 10, kLeveldbThreadNum = 10, kHbaseThreadNum = 10, kMOTThreadNum = 10;

    static bool is_tikv_enable, is_leveldb_enable, is_hbase_enable, is_mot_enable, is_nebula_enable;
    static std::string kMasterIp, kPrivateIp, kTiKVIP, kLevelDBIP, kHbaseIP;
    static uint64_t kTikvThreadNum, kLeveldbThreadNum, kHbaseThreadNum, kMOTThreadNum;

    static void GetStorageInfo(const std::string &config_file_path = "/tmp/zwx/Storage_config.xml");

};

enum TestMode {
    MultiModelTest = 1,
    KV = 2,
    SQL = 3,
    GQL = 4
};

class MultiModelContext {
public:

    explicit MultiModelContext() {
        //            GetMultiModelInfo("../MultiModel_config.xml");
    }
    //        explicit MultiModelContext(const std::string& MultiModel_config_file_path) {
    //            GetMultiModelInfo(MultiModel_config_file_path);
    //        }

    static std::string  kMultiModelClientIP, kTaasIP,
        kNebulaIP, kNebulaSpace, kNebulaUser, kNebulaPwd,
        kMOTIP, kMOTDsnName, kMOTDsnUid, kMOTDsnPwd;
    static TestMode kTestMode;
    static bool isLoadData, isUseMot, isUseNebula;

    static uint64_t kRecordCount, kTxnNum, kWriteNum, kReadNum, kOpNum, kClientNum;
    static std::string kDistribution;

    void GetMultiModelInfo(const std::string &config_file_path = "/tmp/zwx/MultiModel_config.xml");
};

class Context {
public:
    TaasContext taasContext;
    StorageContext storageContext;
    MultiModelContext multiModelContext;

    void Init() {
        taasContext.GetTaaSServerInfo("/tmp/zwx/TaaS_config.xml");
        storageContext.GetStorageInfo("/tmp/zwx/Storage_config.xml");
        multiModelContext.GetMultiModelInfo("/tmp/zwx/MultiModelConfig.xml");
    }
};




ServerMode TaasContext::server_type = ServerMode::Taas;
TaasMode TaasContext::taasMode = TaasMode::MultiMaster;
std::vector<std::string> TaasContext::kServerIp;
uint64_t TaasContext::kTxnNodeNum = 1, TaasContext::kBackUpNum = 1;
uint64_t TaasContext::kIndexNum = 1, TaasContext::kEpochSize_us = 10000/** us */, TaasContext::txn_node_ip_index = 0,
         TaasContext::kShardNum = 1, TaasContext::kReplicaNum = 1,
         TaasContext::kDurationTime_us = 0,
         TaasContext::kCacheMaxLength = 200000, TaasContext::kDelayEpochNum = 0, TaasContext::print_mode_size = 1000;
uint64_t TaasContext::kMergeThreadNum = 0, TaasContext::kEpochTxnThreadNum = 0, TaasContext::kEpochMessageThreadNum = 0;
uint64_t TaasContext::kTestClientNum = 0, TaasContext::kTestKeyRange = 1000000, TaasContext::kTestTxnOpNum = 10;
uint64_t TaasContext::kHandleEpochMessageNumOfEachTraversal = 1, TaasContext::kHandleTxnMessageNumOfEachTraversal = 1, TaasContext::kSafeEpochDistance = 10;

bool TaasContext::is_read_repeatable = false, TaasContext::is_snap_isolation = false,
     TaasContext::is_breakdown = false, TaasContext::is_sync_start = false,
     TaasContext::is_cache_server_available = false;
std::string TaasContext::glog_path = "/tmp";

bool StorageContext::is_tikv_enable = false, StorageContext::is_leveldb_enable = false, StorageContext::is_hbase_enable = false,
     StorageContext::is_mot_enable = true, StorageContext::is_nebula_enable = false;
std::string StorageContext::kMasterIp, StorageContext::kPrivateIp, StorageContext::kTiKVIP, StorageContext::kLevelDBIP, StorageContext::kHbaseIP;
uint64_t StorageContext::kTikvThreadNum = 10, StorageContext::kLeveldbThreadNum = 10, StorageContext::kHbaseThreadNum = 10, StorageContext::kMOTThreadNum = 10;


std::string  MultiModelContext::kMultiModelClientIP, MultiModelContext::kTaasIP,
    MultiModelContext::kNebulaIP, MultiModelContext::kNebulaSpace, MultiModelContext::kNebulaUser, MultiModelContext::kNebulaPwd,
    MultiModelContext::kMOTIP, MultiModelContext::kMOTDsnName, MultiModelContext::kMOTDsnUid, MultiModelContext::kMOTDsnPwd;
TestMode MultiModelContext::kTestMode = MultiModelTest;
bool MultiModelContext::isLoadData = true , MultiModelContext::isUseMot = true, MultiModelContext::isUseNebula = true;

uint64_t MultiModelContext::kRecordCount = 1000000, MultiModelContext::kTxnNum = 10000, MultiModelContext::kWriteNum = 100,
         MultiModelContext::kReadNum = 0, MultiModelContext::kOpNum = 10, MultiModelContext::kClientNum = 10;
std::string MultiModelContext::kDistribution = "zipfian";



void TaasContext::GetTaaSServerInfo(const std::string& config_file_path){
    tinyxml2::XMLDocument doc;
    doc.LoadFile(config_file_path.c_str());
    auto* root=doc.RootElement();

    tinyxml2::XMLElement* server = root->FirstChildElement("server_type");
    server_type = static_cast<ServerMode>(std::stoull(server->GetText()));

    tinyxml2::XMLElement* server_mode = root->FirstChildElement("taas_server_mode");
    taasMode = static_cast<TaasMode>(std::stoull(server_mode->GetText()));

    tinyxml2::XMLElement* server_num = root->FirstChildElement("txn_node_num");
    kTxnNodeNum= std::stoull(server_num->GetText());
    tinyxml2::XMLElement* txn_node_ip_index_xml = root->FirstChildElement("txn_node_ip_index");
    txn_node_ip_index=std::stoull(txn_node_ip_index_xml->GetText()) ;
    tinyxml2::XMLElement *index_element = root->FirstChildElement("txn_node_ip");
    while (index_element){
        tinyxml2::XMLElement *ip_port = index_element->FirstChildElement("txn_ip");
        const char* content;
        while(ip_port){
            content = ip_port->GetText();
            std::string temp(content);
            kServerIp.push_back(temp);
            ip_port=ip_port->NextSiblingElement();

        }
        index_element = index_element->NextSiblingElement();
    }

    tinyxml2::XMLElement* sync_start = root->FirstChildElement("sync_start");
    is_sync_start = std::stoull(sync_start->GetText());
    tinyxml2::XMLElement* epoch_size_us = root->FirstChildElement("epoch_size_us");
    kEpochSize_us= std::stoull(epoch_size_us->GetText());
    tinyxml2::XMLElement* cachemaxlength = root->FirstChildElement("cache_max_length");
    kCacheMaxLength = std::stoull(cachemaxlength->GetText());

    tinyxml2::XMLElement* shard_num = root->FirstChildElement("shard_num");
    kShardNum= std::stoull(shard_num->GetText());
    tinyxml2::XMLElement* replica_num = root->FirstChildElement("replica_num");
    kReplicaNum = std::stoull(replica_num->GetText());

    if(kReplicaNum > kTxnNodeNum) kReplicaNum = kTxnNodeNum;
    if(kShardNum > kTxnNodeNum) kShardNum = kTxnNodeNum;
    kBackUpNum = 2; /// send to another 2 server

    tinyxml2::XMLElement* merge_thread_num = root->FirstChildElement("merge_thread_num");
    kMergeThreadNum = std::stoull(merge_thread_num->GetText());
    tinyxml2::XMLElement* epoch_txn_thread_num = root->FirstChildElement("epoch_txn_thread_num");
    kEpochTxnThreadNum = std::stoull(epoch_txn_thread_num->GetText());
    tinyxml2::XMLElement* epoch_message_thread_num = root->FirstChildElement("epoch_message_thread_num");
    kEpochMessageThreadNum = std::stoull(epoch_message_thread_num->GetText());

    tinyxml2::XMLElement* duration_time = root->FirstChildElement("duration_time_us");
    kDurationTime_us = std::stoull(duration_time->GetText());
    tinyxml2::XMLElement* client_num = root->FirstChildElement("test_client_num");
    kTestClientNum = std::stoull(client_num->GetText());
    tinyxml2::XMLElement* key_range = root->FirstChildElement("test_key_range");
    kTestKeyRange = std::stoull(key_range->GetText());
    tinyxml2::XMLElement* test_txn_op_num = root->FirstChildElement("test_txn_op_num");
    kTestTxnOpNum = std::stoull(test_txn_op_num->GetText());

    tinyxml2::XMLElement* handle_epoch_message_num_each_traversal = root->FirstChildElement("handle_epoch_message_num_each_traversal");
    kHandleEpochMessageNumOfEachTraversal = std::stoull(handle_epoch_message_num_each_traversal->GetText());
    tinyxml2::XMLElement* handle_txn_message_num_each_traversal = root->FirstChildElement("handle_txn_message_num_each_traversal");
    kHandleTxnMessageNumOfEachTraversal = std::stoull(handle_txn_message_num_each_traversal->GetText());
    tinyxml2::XMLElement* safe_epoch_distance = root->FirstChildElement("safe_epoch_distance");
    kSafeEpochDistance = std::stoull(safe_epoch_distance->GetText());


    /** Get glog path */
    //        tinyxml2::XMLElement* glog_path_ = root->FirstChildElement("glog_path");
    //        glog_path = std::string(glog_path_->GetText());

    tinyxml2::XMLElement* mode_size_t = root->FirstChildElement("print_mode_size");
    print_mode_size = std::stoull(mode_size_t->GetText());

    //        kBackUpNum = kTxnNodeNum - 1;
}

std::string TaasContext::Print() {
    std::string res = "";
    res += "Config Info:\n \tServerIp:\n";
    int cnt = 0;
    for(const auto& i : kServerIp) {
        res += "\t \t ID: " + std::to_string(cnt++) + ", IP: " + i.c_str() + "\n";
    }
    res += "\t ServerNum: "+ std::to_string(kTxnNodeNum) + "\n\t txn_node_ip_index: "
           + std::to_string(txn_node_ip_index) + "\n\t EpochSize_us: " + std::to_string(kEpochSize_us) + "\n";
    res += "\t CacheLength: " + std::to_string(kCacheMaxLength) + "\n";
    res += "\t MergeThreadNum: " + std::to_string(kMergeThreadNum) + "\n\t DurationTime_us: " + std::to_string(kDurationTime_us) + "\n";
    res += "\t TestClientNum: " + std::to_string(kTestClientNum) + "\n\t TestKeyRange: "
           + std::to_string(kTestKeyRange) + "\n\t TestTxnOpNum: " + std::to_string(kTestTxnOpNum) + "\n";
    res += "\t SycnStart: " + std::to_string(is_sync_start) + "\n";
    return res;
}

void StorageContext::GetStorageInfo(const std::string& config_file_path){
    tinyxml2::XMLDocument doc;
    doc.LoadFile(config_file_path.c_str());
    auto* root=doc.RootElement();

    tinyxml2::XMLElement* mot = root->FirstChildElement("is_mot_enable");
    is_mot_enable = std::stoull(mot->GetText());
    tinyxml2::XMLElement* mot_thread_num = root->FirstChildElement("mot_thread_num");
    kMOTThreadNum = std::stoull(mot_thread_num->GetText());
    tinyxml2::XMLElement* nebula = root->FirstChildElement("is_nebula_enable");
    is_nebula_enable = std::stoull(nebula->GetText());


    tinyxml2::XMLElement* tikv = root->FirstChildElement("is_tikv_enable");
    is_tikv_enable = std::stoull(tikv->GetText());
    tinyxml2::XMLElement *ip_port= root->FirstChildElement("tikv_ip");
    auto tikv_ip=ip_port->GetText();
    kTiKVIP = std::string(tikv_ip);
    tinyxml2::XMLElement* tikv_thread_num = root->FirstChildElement("tikv_thread_num");
    kTikvThreadNum = std::stoull(tikv_thread_num->GetText());

    tinyxml2::XMLElement* leveldb = root->FirstChildElement("is_leveldb_enable");
    is_leveldb_enable = std::stoull(leveldb->GetText());
    tinyxml2::XMLElement *leveldb_ip_port= root->FirstChildElement("leveldb_ip");
    auto leveldb_ip = leveldb_ip_port->GetText();
    kLevelDBIP = std::string(leveldb_ip);
    tinyxml2::XMLElement* leveldb_thread_num = root->FirstChildElement("leveldb_thread_num");
    kLeveldbThreadNum = std::stoull(leveldb_thread_num->GetText());

    tinyxml2::XMLElement* hbase = root->FirstChildElement("is_hbase_enable");
    is_hbase_enable = std::stoull(hbase->GetText());
    tinyxml2::XMLElement *hbase_ip_port= root->FirstChildElement("hbase_ip");
    auto hbase_ip=hbase_ip_port->GetText();
    kHbaseIP = std::string(hbase_ip);
    tinyxml2::XMLElement* hbase_thread_num = root->FirstChildElement("hbase_thread_num");
    kHbaseThreadNum = std::stoull(hbase_thread_num->GetText());

}

void MultiModelContext::GetMultiModelInfo(const std::string &config_file_path) {
    tinyxml2::XMLDocument doc;
    doc.LoadFile("../MultiModelConfig.xml");
    tinyxml2::XMLElement *root=doc.RootElement();

    tinyxml2::XMLElement *multimodel_client = root->FirstChildElement("multimodel_client");
    auto multimodel_clients = multimodel_client->GetText();
    kMultiModelClientIP = std::string(multimodel_clients);

    tinyxml2::XMLElement *taas_ip_port = root->FirstChildElement("taas_ip");
    auto taas_ip = taas_ip_port->GetText();
    kTaasIP = std::string(taas_ip);

    tinyxml2::XMLElement* use_nebula = root->FirstChildElement("use_nebula");
    isUseNebula = std::stoull(use_nebula->GetText());

    tinyxml2::XMLElement *nebula_ip_port = root->FirstChildElement("nebula_ip");
    auto nebula_ip = nebula_ip_port->GetText();
    kNebulaIP = std::string(nebula_ip);

    tinyxml2::XMLElement *nebula_user = root->FirstChildElement("nebula_user");
    auto nebula_users = nebula_user->GetText();
    kNebulaUser = std::string(nebula_users);

    tinyxml2::XMLElement *nebula_pwd = root->FirstChildElement("nebula_pwd");
    auto nebula_pwds = nebula_pwd->GetText();
    kNebulaPwd = std::string(nebula_pwds);

    tinyxml2::XMLElement *nebula_space = root->FirstChildElement("nebula_space");
    auto nebula_spaces = nebula_space->GetText();
    kNebulaSpace = std::string(nebula_spaces);


    tinyxml2::XMLElement* use_mot = root->FirstChildElement("use_mot");
    isUseMot = std::stoull(use_mot->GetText());

    tinyxml2::XMLElement *mot_ip_port = root->FirstChildElement("mot_ip");
    auto mot_ip = mot_ip_port->GetText();
    kMOTIP = std::string(mot_ip);

    tinyxml2::XMLElement *mot_dsnnames = root->FirstChildElement("mot_dsnname");
    auto mot_dsnname = mot_dsnnames->GetText();
    kMOTDsnName = std::string(mot_dsnname);

    tinyxml2::XMLElement *mot_dsnuids = root->FirstChildElement("mot_dsnuid");
    auto mot_dsnuid = mot_dsnuids->GetText();
    kMOTDsnUid = std::string(mot_dsnuid);

    tinyxml2::XMLElement *mot_dsnpwd = root->FirstChildElement("mot_dsnpwd");
    auto mot_dsnpwds = mot_dsnpwd->GetText();
    kMOTDsnPwd = std::string(mot_dsnpwds);


    tinyxml2::XMLElement* test_mode = root->FirstChildElement("test_mode");
    kTestMode = static_cast<TestMode>(std::stoull(test_mode->GetText()));

    tinyxml2::XMLElement* is_generate_txn = root->FirstChildElement("is_load_data");
    isLoadData = std::stoull(is_generate_txn->GetText());

    tinyxml2::XMLElement* record_count = root->FirstChildElement("record_count");
    kRecordCount = std::stoull(record_count->GetText());

    tinyxml2::XMLElement* server_num = root->FirstChildElement("txn_num");
    kTxnNum =  std::stoull(server_num->GetText());

    tinyxml2::XMLElement* write = root->FirstChildElement("write");
    kWriteNum =  std::stoull(write->GetText());

    tinyxml2::XMLElement* read = root->FirstChildElement("read");
    kReadNum =  std::stoull(read->GetText());

    tinyxml2::XMLElement* opnum = root->FirstChildElement("op_num");
    kOpNum =  std::stoull(opnum->GetText());

    tinyxml2::XMLElement *distribution = root->FirstChildElement("distribution");
    auto distribution_s = distribution->GetText();
    kDistribution = std::string(distribution_s);

    tinyxml2::XMLElement* client_threads = root->FirstChildElement("client_threads");
    kClientNum =  std::stoull(client_threads->GetText());


}


///ThreadCount
class ThreadCounters{
public:
    uint64_t thread_id = 0, max_length = 0, shard_num = 0, local_server_id, replica_num = 1, server_num = 1;

    static Context ctx;
    static std::atomic<uint64_t> inc_id;
    static std::vector<std::vector<bool>> is_local_shard;

    ///message handling
public:
    std::shared_ptr<AtomicCounters_Cache>
        shard_should_send_txn_num_local,
        shard_send_txn_num_local,
        shard_should_handle_local_txn_num_local,
        shard_handled_local_txn_num_local,
        shard_should_handle_remote_txn_num_local,
        shard_handled_remote_txn_num_local,
        shard_received_txn_num_local,

        remote_server_should_send_txn_num_local,
        remote_server_send_txn_num_local,
        remote_server_should_handle_txn_num_local,
        remote_server_handled_txn_num_local,
        remote_server_received_txn_num_local,

        backup_should_send_txn_num_local,
        backup_send_txn_num_local,
        backup_received_txn_num_local;

    static std::vector<std::shared_ptr<AtomicCounters_Cache>>
        shard_should_send_txn_num_local_vec,
        shard_send_txn_num_local_vec,
        shard_should_handle_local_txn_num_local_vec,
        shard_handled_local_txn_num_local_vec,
        shard_should_handle_remote_txn_num_local_vec,
        shard_handled_remote_txn_num_local_vec,
        shard_received_txn_num_local_vec,


        remote_server_should_send_txn_num_local_vec,
        remote_server_send_txn_num_local_vec,
        remote_server_should_handle_txn_num_local_vec,
        remote_server_handled_txn_num_local_vec,
        remote_server_received_txn_num_local_vec,


        backup_should_send_txn_num_local_vec,
        backup_send_txn_num_local_vec,
        backup_received_txn_num_local_vec;

    static std::vector<uint64_t>
        shard_send_ack_epoch_num,
        remote_server_send_ack_epoch_num,
        backup_send_ack_epoch_num,
        backup_insert_set_send_ack_epoch_num,
        abort_set_send_ack_epoch_num; /// check and reply ack

    static std::vector<std::unique_ptr<std::atomic<bool>>>
        epoch_shard_handle_complete,
        epoch_shard_send_complete,
        epoch_shard_receive_complete,
        epoch_remote_server_handle_complete,
        epoch_remote_server_send_complete,
        epoch_remote_server_receive_complete,
        epoch_back_up_complete,
        epoch_abort_set_merge_complete,
        epoch_insert_set_complete;

    static AtomicCounters_Cache
        shard_should_receive_pack_num,
        shard_received_pack_num,
        shard_should_receive_txn_num,
        shard_received_ack_num,

        remote_server_should_receive_pack_num,
        remote_server_received_pack_num,
        remote_server_should_receive_txn_num,
        remote_server_received_ack_num,

        backup_should_receive_pack_num,
        backup_received_pack_num,
        backup_should_receive_txn_num,
        backup_received_ack_num,

        insert_set_should_receive_num,
        insert_set_received_num,
        insert_set_received_ack_num,

        abort_set_should_receive_num,
        abort_set_received_num,
        abort_set_received_ack_num,

        meta_info_received_num,

        redo_log_push_down_ack_num,
        redo_log_push_down_local_epoch;

    static bool CheckEpochShardSendComplete(const uint64_t& epoch) ;
    static bool CheckEpochShardReceiveComplete(const uint64_t& epoch) ;

    static bool IsShardSendFinish(const uint64_t &epoch, const uint64_t &shard_id) ;
    static bool IsShardSendFinish(const uint64_t &epoch) ;
    static bool IsShardTxnReceiveComplete(const uint64_t &epoch) ;
    static bool IsShardTxnReceiveComplete(const uint64_t &epoch, const uint64_t &id) ;
    static bool IsShardPackReceiveComplete(const uint64_t &epoch) ;
    static bool IsShardPackReceiveComplete(const uint64_t &epoch, const uint64_t &id) ;




    static bool CheckEpochRemoteServerSendComplete(const uint64_t& epoch) ;
    static bool CheckEpochRemoteServerReceiveComplete(const uint64_t& epoch) ;

    static bool IsRemoteServerSendFinish(const uint64_t &epoch, const uint64_t &shard_id) ;
    static bool IsRemoteServerSendFinish(const uint64_t &epoch) ;
    static bool IsRemoteServerTxnReceiveComplete(const uint64_t &epoch) ;
    static bool IsRemoteServerTxnReceiveComplete(const uint64_t &epoch, const uint64_t &id) ;
    static bool IsRemoteServerPackReceiveComplete(const uint64_t &epoch) ;
    static bool IsRemoteServerPackReceiveComplete(const uint64_t &epoch, const uint64_t &id) ;




    static bool CheckEpochBackUpComplete(const uint64_t& epoch) ;

    static bool IsBackUpSendFinish(const uint64_t &epoch) ;
    static bool IsBackUpTxnReceiveComplete(const uint64_t &epoch) ;
    static bool IsBackUpTxnReceiveComplete(const uint64_t &epoch, const uint64_t &id) ;
    static bool IsBackUpPackReceiveComplete(const uint64_t &epoch) ;
    static bool IsBackUpPackReceiveComplete(const uint64_t &epoch, const uint64_t &id) ;



    static bool CheckEpochAbortSetMergeComplete(const uint64_t& epoch) ;
    static bool CheckEpochInsertSetMergeComplete(const uint64_t& epoch) ;

    static bool IsAbortSetReceiveComplete(const uint64_t &epoch, const uint64_t &id) ;
    static bool IsAbortSetReceiveComplete(const uint64_t &epoch);
    static bool IsInsertSetReceiveComplete(const uint64_t &epoch, const uint64_t &id) ;
    static bool IsInsertSetReceiveComplete(const uint64_t &epoch) ;


    static bool IsShardACKReceiveComplete(const uint64_t &epoch) ;
    static bool IsRemoteServerACKReceiveComplete(const uint64_t &epoch) ;
    static bool IsBackUpACKReceiveComplete(const uint64_t &epoch) ;
    static bool IsAbortSetACKReceiveComplete(const uint64_t &epoch) ;
    static bool IsInsertSetACKReceiveComplete(const uint64_t &epoch) ;
    static bool IsRedoLogPushDownACKReceiveComplete(const uint64_t &epoch) ;



    static bool CheckEpochClientTxnHandleComplete(const uint64_t &epoch) ;
    static bool CheckEpochShardTxnHandleComplete(const uint64_t &epoch) ;




    ///Merge
public:
    std::shared_ptr<AtomicCounters_Cache>
        epoch_should_read_validate_txn_num_local,
        epoch_read_validated_txn_num_local,
        epoch_should_merge_txn_num_local,
        epoch_merged_txn_num_local,
        epoch_should_commit_txn_num_local,
        epoch_committed_txn_num_local,
        epoch_record_commit_txn_num_local,
        epoch_record_committed_txn_num_local,
        epoch_result_return_txn_num_local,
        epoch_result_returned_txn_num_local;

    std::atomic<uint64_t>
        total_merge_txn_num_local,
        total_merge_latency_local,
        total_commit_txn_num_local,
        total_commit_latency_local,
        success_commit_txn_num_local,
        success_commit_latency_local,
        total_read_version_check_failed_txn_num_local,
        total_failed_txn_num_local;
public:
    static std::vector<std::shared_ptr<AtomicCounters_Cache>>
        epoch_should_read_validate_txn_num_local_vec,
        epoch_read_validated_txn_num_local_vec,
        epoch_should_merge_txn_num_local_vec,
        epoch_merged_txn_num_local_vec,
        epoch_should_commit_txn_num_local_vec,
        epoch_committed_txn_num_local_vec,
        epoch_record_commit_txn_num_local_vec,
        epoch_record_committed_txn_num_local_vec,
        epoch_result_return_txn_num_local_vec,
        epoch_result_returned_txn_num_local_vec;

    static std::vector<std::unique_ptr<std::atomic<bool>>>
        epoch_read_validate_complete,
        epoch_merge_complete,
        epoch_commit_complete,
        epoch_record_committed,
        epoch_result_returned;

    static std::atomic<uint64_t>
        total_merge_txn_num,
        total_merge_latency,
        total_commit_txn_num,
        total_commit_latency,
        success_commit_txn_num,
        success_commit_latency,
        total_read_version_check_failed_txn_num,
        total_failed_txn_num;


    static bool CheckEpochReadValidateComplete(const uint64_t& epoch);
    static bool CheckEpochMergeComplete(const uint64_t& epoch) ;
    static bool CheckEpochCommitComplete(const uint64_t& epoch) ;
    static bool CheckEpochRecordCommitted(const uint64_t& epoch) ;
    static bool CheckEpochResultReturned(const uint64_t& epoch) ;

    static bool IsReadValidateComplete(const uint64_t& epoch) ;
    static bool IsMergeComplete(const uint64_t& epoch) ;
    static bool IsCommitComplete(const uint64_t & epoch) ;
    static bool IsRecordCommitted(const uint64_t & epoch) ;
    static bool IsResultReturned(const uint64_t & epoch) ;




public:

    void ThreadCountersInit(const Context& context);
    static bool StaticInit();
    static bool StaticClear(uint64_t& epoch);

    static void ClearAllThreadLocalCountNum(const uint64_t &epoch, const std::vector<std::shared_ptr<AtomicCounters_Cache>> &vec) ;
    static uint64_t GetAllThreadLocalCountNum(const uint64_t &epoch, const std::vector<std::shared_ptr<AtomicCounters_Cache>> &vec) ;
    static uint64_t GetAllThreadLocalCountNum(const uint64_t &epoch, const uint64_t &shard_id, const std::vector<std::shared_ptr<AtomicCounters_Cache>> &vec);

};



std::atomic<uint64_t> ThreadCounters::inc_id(0);
Context ThreadCounters::ctx;
std::vector<std::vector<bool>> ThreadCounters::is_local_shard;

std::vector<std::shared_ptr<AtomicCounters_Cache>>
    ThreadCounters::shard_should_send_txn_num_local_vec,
    ThreadCounters::shard_send_txn_num_local_vec,
    ThreadCounters::shard_should_handle_local_txn_num_local_vec,
    ThreadCounters::shard_handled_local_txn_num_local_vec,
    ThreadCounters::shard_should_handle_remote_txn_num_local_vec,
    ThreadCounters::shard_handled_remote_txn_num_local_vec,
    ThreadCounters::shard_received_txn_num_local_vec,

    ThreadCounters::remote_server_should_send_txn_num_local_vec,
    ThreadCounters::remote_server_send_txn_num_local_vec,
    ThreadCounters::remote_server_should_handle_txn_num_local_vec,
    ThreadCounters::remote_server_handled_txn_num_local_vec,
    ThreadCounters::remote_server_received_txn_num_local_vec,


    ThreadCounters::backup_should_send_txn_num_local_vec,
    ThreadCounters::backup_send_txn_num_local_vec,
    ThreadCounters::backup_received_txn_num_local_vec;

std::vector<uint64_t>
    ThreadCounters::shard_send_ack_epoch_num,
    ThreadCounters::remote_server_send_ack_epoch_num,
    ThreadCounters::backup_send_ack_epoch_num,
    ThreadCounters::backup_insert_set_send_ack_epoch_num,
    ThreadCounters::abort_set_send_ack_epoch_num;

std::vector<std::unique_ptr<std::atomic<bool>>>
    ThreadCounters::epoch_shard_handle_complete,
    ThreadCounters::epoch_shard_send_complete,
    ThreadCounters::epoch_shard_receive_complete,
    ThreadCounters::epoch_remote_server_handle_complete,
    ThreadCounters::epoch_remote_server_send_complete,
    ThreadCounters::epoch_remote_server_receive_complete,
    ThreadCounters::epoch_back_up_complete,
    ThreadCounters::epoch_abort_set_merge_complete,
    ThreadCounters::epoch_insert_set_complete;

AtomicCounters_Cache
    ThreadCounters::shard_should_receive_pack_num(10, 1),
    ThreadCounters::shard_received_pack_num(10, 1),
    ThreadCounters::shard_should_receive_txn_num(10, 1),
    ThreadCounters::shard_received_ack_num(10, 1),

    ThreadCounters::remote_server_should_receive_pack_num(10, 1),
    ThreadCounters::remote_server_received_pack_num(10, 1),
    ThreadCounters::remote_server_should_receive_txn_num(10, 1),
    ThreadCounters::remote_server_received_ack_num(10, 1),

    ThreadCounters::backup_should_receive_pack_num(10, 1),
    ThreadCounters::backup_received_pack_num(10, 1),
    ThreadCounters::backup_should_receive_txn_num(10, 1),
    ThreadCounters::backup_received_ack_num(10, 1),

    ThreadCounters::insert_set_should_receive_num(10, 1),
    ThreadCounters::insert_set_received_num(10, 1),
    ThreadCounters::insert_set_received_ack_num(10, 1),

    ThreadCounters::abort_set_should_receive_num(10, 1),
    ThreadCounters::abort_set_received_num(10, 1),
    ThreadCounters::abort_set_received_ack_num(10, 1),

    ThreadCounters::meta_info_received_num(10, 1),

    ThreadCounters::redo_log_push_down_ack_num(10, 1),
    ThreadCounters::redo_log_push_down_local_epoch(10, 1);






std::vector<std::shared_ptr<AtomicCounters_Cache>>
    ThreadCounters::epoch_should_read_validate_txn_num_local_vec,
    ThreadCounters::epoch_read_validated_txn_num_local_vec,
    ThreadCounters::epoch_should_merge_txn_num_local_vec,
    ThreadCounters::epoch_merged_txn_num_local_vec,
    ThreadCounters::epoch_should_commit_txn_num_local_vec,
    ThreadCounters::epoch_committed_txn_num_local_vec,
    ThreadCounters::epoch_record_commit_txn_num_local_vec,
    ThreadCounters::epoch_record_committed_txn_num_local_vec,
    ThreadCounters::epoch_result_return_txn_num_local_vec,
    ThreadCounters::epoch_result_returned_txn_num_local_vec;

std::vector<std::unique_ptr<std::atomic<bool>>>
    ThreadCounters::epoch_read_validate_complete,
    ThreadCounters::epoch_merge_complete,
    ThreadCounters::epoch_commit_complete,
    ThreadCounters::epoch_record_committed,
    ThreadCounters::epoch_result_returned;

std::atomic<uint64_t>
    ThreadCounters::total_merge_txn_num(0),
    ThreadCounters::total_merge_latency(0),
    ThreadCounters::total_commit_txn_num(0),
    ThreadCounters::total_commit_latency(0),
    ThreadCounters::success_commit_txn_num(0),
    ThreadCounters::success_commit_latency(0),
    ThreadCounters::total_read_version_check_failed_txn_num(0),
    ThreadCounters::total_failed_txn_num(0);







void ThreadCounters::ThreadCountersInit(const Context& context) {
    thread_id = inc_id.fetch_add(1);
    shard_num = TaasContext::kShardNum;
    replica_num = TaasContext::kReplicaNum;
    server_num = TaasContext::kTxnNodeNum;
    max_length = TaasContext::kCacheMaxLength;
    local_server_id = TaasContext::txn_node_ip_index;

    shard_should_send_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    shard_send_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num);
    shard_should_handle_local_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    shard_handled_local_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    shard_should_handle_remote_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    shard_handled_remote_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    shard_received_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),


    remote_server_should_send_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    remote_server_send_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    remote_server_should_handle_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    remote_server_handled_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    remote_server_received_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),

    backup_should_send_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    backup_send_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    backup_received_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num);

    shard_should_send_txn_num_local_vec[thread_id] = shard_should_send_txn_num_local;
    shard_send_txn_num_local_vec[thread_id] = shard_send_txn_num_local;
    shard_should_handle_local_txn_num_local_vec[thread_id] = shard_should_handle_local_txn_num_local;
    shard_handled_local_txn_num_local_vec[thread_id] = shard_handled_local_txn_num_local;
    shard_should_handle_remote_txn_num_local_vec[thread_id] = shard_should_handle_remote_txn_num_local;
    shard_handled_remote_txn_num_local_vec[thread_id] = shard_handled_remote_txn_num_local;
    shard_received_txn_num_local_vec[thread_id] = shard_received_txn_num_local;


    remote_server_should_send_txn_num_local_vec[thread_id] = remote_server_should_send_txn_num_local;
    remote_server_send_txn_num_local_vec[thread_id] = remote_server_send_txn_num_local;
    remote_server_should_handle_txn_num_local_vec[thread_id] = remote_server_should_handle_txn_num_local;
    remote_server_handled_txn_num_local_vec[thread_id] = remote_server_handled_txn_num_local;
    remote_server_received_txn_num_local_vec[thread_id] = remote_server_received_txn_num_local;


    backup_should_send_txn_num_local_vec[thread_id] = backup_should_send_txn_num_local;
    backup_send_txn_num_local_vec[thread_id] = backup_send_txn_num_local;
    backup_received_txn_num_local_vec[thread_id] = backup_received_txn_num_local;




    epoch_should_read_validate_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    epoch_read_validated_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num);
    epoch_should_merge_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    epoch_merged_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    epoch_should_commit_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    epoch_committed_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    epoch_record_commit_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num),
    epoch_record_committed_txn_num_local = std::make_shared<AtomicCounters_Cache>(max_length, server_num);
    epoch_result_return_txn_num_local  = std::make_shared<AtomicCounters_Cache>(max_length, server_num);
    epoch_result_returned_txn_num_local  = std::make_shared<AtomicCounters_Cache>(max_length, server_num);


    epoch_should_read_validate_txn_num_local_vec[thread_id] = epoch_should_read_validate_txn_num_local;
    epoch_read_validated_txn_num_local_vec[thread_id] = epoch_read_validated_txn_num_local;
    epoch_should_merge_txn_num_local_vec[thread_id] = epoch_should_merge_txn_num_local;
    epoch_merged_txn_num_local_vec[thread_id] = epoch_merged_txn_num_local;
    epoch_should_commit_txn_num_local_vec[thread_id] = epoch_should_commit_txn_num_local;
    epoch_committed_txn_num_local_vec[thread_id] = epoch_committed_txn_num_local;
    epoch_record_commit_txn_num_local_vec[thread_id] = epoch_record_commit_txn_num_local;
    epoch_record_committed_txn_num_local_vec[thread_id] = epoch_record_committed_txn_num_local;
    epoch_result_return_txn_num_local_vec[thread_id] = epoch_result_return_txn_num_local;
    epoch_result_returned_txn_num_local_vec[thread_id] = epoch_result_returned_txn_num_local;

}

bool ThreadCounters::StaticInit() {
    auto thread_total_num = TaasContext::kMergeThreadNum * 2
                            + TaasContext::kEpochMessageThreadNum + TaasContext::kEpochTxnThreadNum;
    auto max_length = TaasContext::kCacheMaxLength;
    auto shard_num = TaasContext::kShardNum;
    auto replica_num = TaasContext::kReplicaNum;
    auto server_num = TaasContext::kTxnNodeNum;

    is_local_shard.resize(TaasContext::kTxnNodeNum);
    for(auto &i : is_local_shard) {
        i.resize(TaasContext::kShardNum);
    }
    for(uint64_t server_id = 0; server_id < TaasContext::kTxnNodeNum; server_id ++) {
        for(uint64_t i = 0; i < TaasContext::kShardNum; i ++) {
            for(uint64_t j = 0; j < TaasContext::kReplicaNum; j ++ ) {
                if((i + TaasContext::kTxnNodeNum - j) % TaasContext::kTxnNodeNum == server_id) {
                    is_local_shard[server_id][i] = true;
                }
            }
        }
    }

    shard_should_send_txn_num_local_vec.resize(thread_total_num);
    shard_send_txn_num_local_vec.resize(thread_total_num);
    shard_should_handle_local_txn_num_local_vec.resize(thread_total_num);
    shard_handled_local_txn_num_local_vec.resize(thread_total_num);
    shard_should_handle_remote_txn_num_local_vec.resize(thread_total_num);
    shard_handled_remote_txn_num_local_vec.resize(thread_total_num);
    shard_received_txn_num_local_vec.resize(thread_total_num);

    remote_server_should_send_txn_num_local_vec.resize(thread_total_num);
    remote_server_send_txn_num_local_vec.resize(thread_total_num);
    remote_server_should_handle_txn_num_local_vec.resize(thread_total_num);
    remote_server_handled_txn_num_local_vec.resize(thread_total_num);
    remote_server_received_txn_num_local_vec.resize(thread_total_num);


    backup_should_send_txn_num_local_vec.resize(thread_total_num);
    backup_send_txn_num_local_vec.resize(thread_total_num);
    backup_received_txn_num_local_vec.resize(thread_total_num);

    shard_send_ack_epoch_num.resize(server_num + 1);
    remote_server_send_ack_epoch_num.resize(server_num + 1);
    backup_send_ack_epoch_num.resize(server_num + 1);
    backup_insert_set_send_ack_epoch_num.resize(server_num + 1);
    abort_set_send_ack_epoch_num.resize(server_num + 1);
    for(int i = 0; i <= (int) server_num; i ++ ) { /// start at 1, not 0
        shard_send_ack_epoch_num[i] = 1;
        remote_server_send_ack_epoch_num[i] = 1;
        backup_send_ack_epoch_num[i] = 1;
        backup_insert_set_send_ack_epoch_num[i] = 1;
        abort_set_send_ack_epoch_num[i] = 1;
    }

    epoch_shard_handle_complete.resize(max_length);
    epoch_shard_send_complete.resize(max_length);
    epoch_shard_receive_complete.resize(max_length);
    epoch_remote_server_handle_complete.resize(max_length);
    epoch_remote_server_send_complete.resize(max_length);
    epoch_remote_server_receive_complete.resize(max_length);
    epoch_back_up_complete.resize(max_length);
    epoch_abort_set_merge_complete.resize(max_length);
    epoch_insert_set_complete.resize(max_length);
    for(int i = 0; i < static_cast<int>(max_length); i ++) {
        epoch_shard_handle_complete[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_shard_send_complete[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_shard_receive_complete[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_remote_server_handle_complete[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_remote_server_send_complete[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_remote_server_receive_complete[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_back_up_complete[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_abort_set_merge_complete[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_insert_set_complete[i] = std::make_unique<std::atomic<bool>>(false);
    }

    shard_should_receive_pack_num.Init(max_length, server_num, 1),
        shard_received_pack_num.Init(max_length, server_num),
        shard_should_receive_txn_num.Init(max_length, server_num, 0),
        shard_received_ack_num.Init(max_length, server_num),

        remote_server_should_receive_pack_num.Init(max_length, server_num, 1),
        remote_server_received_pack_num.Init(max_length, server_num),
        remote_server_should_receive_txn_num.Init(max_length, server_num, 0),
        remote_server_received_ack_num.Init(max_length, server_num, 0),

        backup_should_receive_pack_num.Init(max_length, server_num, 1),
        backup_received_pack_num.Init(max_length, server_num),
        backup_should_receive_txn_num.Init(max_length, server_num, 0),
        backup_received_ack_num.Init(max_length, server_num),

        insert_set_should_receive_num.Init(max_length, server_num, 1),
        insert_set_received_num.Init(max_length, server_num),
        insert_set_received_ack_num.Init(max_length, server_num),

        abort_set_should_receive_num.Init(max_length, server_num, 1),
        abort_set_received_num.Init(max_length, server_num);
    abort_set_received_ack_num.Init(max_length, server_num);

    redo_log_push_down_ack_num.Init(max_length, server_num);
    redo_log_push_down_local_epoch.Init(max_length, server_num);





    ///Merge
    epoch_should_read_validate_txn_num_local_vec.resize(thread_total_num);
    epoch_read_validated_txn_num_local_vec.resize(thread_total_num);
    epoch_should_merge_txn_num_local_vec.resize(thread_total_num);
    epoch_merged_txn_num_local_vec.resize(thread_total_num);
    epoch_should_commit_txn_num_local_vec.resize(thread_total_num);
    epoch_committed_txn_num_local_vec.resize(thread_total_num);
    epoch_record_commit_txn_num_local_vec.resize(thread_total_num);
    epoch_record_committed_txn_num_local_vec.resize(thread_total_num);
    epoch_result_return_txn_num_local_vec.resize(thread_total_num);
    epoch_result_returned_txn_num_local_vec.resize(thread_total_num);

    ///epoch merge state
    epoch_read_validate_complete.resize(TaasContext::kCacheMaxLength);
    epoch_merge_complete.resize(TaasContext::kCacheMaxLength);
    epoch_commit_complete.resize(TaasContext::kCacheMaxLength);
    epoch_record_committed.resize(TaasContext::kCacheMaxLength);
    epoch_result_returned.resize(TaasContext::kCacheMaxLength);
    for(int i = 0; i < static_cast<int>(TaasContext::kCacheMaxLength); i ++) {
        epoch_read_validate_complete[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_merge_complete[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_commit_complete[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_record_committed[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_result_returned[i] = std::make_unique<std::atomic<bool>>(false);
    }
    return true;
}

bool ThreadCounters::StaticClear(uint64_t& epoch) {
    auto epoch_mod_temp = epoch % TaasContext::kCacheMaxLength;
    auto cache_clear_epoch_num_mod = epoch % TaasContext::kCacheMaxLength;

    ///Message handle
    shard_should_receive_pack_num.Clear(cache_clear_epoch_num_mod, 1),///relate to server state
        shard_received_pack_num.Clear(cache_clear_epoch_num_mod, 0),
        shard_should_receive_txn_num.Clear(cache_clear_epoch_num_mod, 0),
        shard_received_ack_num.Clear(cache_clear_epoch_num_mod, 0),

        remote_server_should_receive_pack_num.Clear(cache_clear_epoch_num_mod, 1),///relate to server state
        remote_server_received_pack_num.Clear(cache_clear_epoch_num_mod, 0),
        remote_server_should_receive_txn_num.Clear(cache_clear_epoch_num_mod, 0),
        remote_server_received_ack_num.Clear(cache_clear_epoch_num_mod, 0),

        backup_should_receive_pack_num.Clear(cache_clear_epoch_num_mod, 1),///relate to server state
        backup_received_pack_num.Clear(cache_clear_epoch_num_mod, 0),
        backup_should_receive_txn_num.Clear(cache_clear_epoch_num_mod, 0),
        backup_received_ack_num.Clear(cache_clear_epoch_num_mod, 0),

        insert_set_should_receive_num.Clear(cache_clear_epoch_num_mod, 1),///relate to server state
        insert_set_received_num.Clear(cache_clear_epoch_num_mod, 0),
        insert_set_received_ack_num.Clear(cache_clear_epoch_num_mod, 0),
        abort_set_should_receive_num.Clear(cache_clear_epoch_num_mod, 1),///relate to server state
        abort_set_received_num.Clear(cache_clear_epoch_num_mod, 0);
    abort_set_received_ack_num.Clear(cache_clear_epoch_num_mod, 0);
    redo_log_push_down_ack_num.Clear(cache_clear_epoch_num_mod, 0);
    redo_log_push_down_local_epoch.Clear(cache_clear_epoch_num_mod, 0);

    epoch_shard_handle_complete[cache_clear_epoch_num_mod]->store(false);
    epoch_shard_send_complete[cache_clear_epoch_num_mod]->store(false);
    epoch_shard_receive_complete[cache_clear_epoch_num_mod]->store(false);
    epoch_remote_server_handle_complete[cache_clear_epoch_num_mod]->store(false);
    epoch_remote_server_send_complete[cache_clear_epoch_num_mod]->store(false);
    epoch_remote_server_receive_complete[cache_clear_epoch_num_mod]->store(false);
    epoch_back_up_complete[cache_clear_epoch_num_mod]->store(false);
    epoch_abort_set_merge_complete[cache_clear_epoch_num_mod]->store(false);
    epoch_insert_set_complete[cache_clear_epoch_num_mod]->store(false);


    ///Merge
    epoch_read_validate_complete[epoch_mod_temp]->store(false);
    epoch_merge_complete[epoch_mod_temp]->store(false);
    epoch_commit_complete[epoch_mod_temp]->store(false);
    epoch_record_committed[epoch_mod_temp]->store(false);
    epoch_result_returned[epoch_mod_temp]->store(false);


    ClearAllThreadLocalCountNum(epoch, shard_should_send_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, shard_send_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, shard_should_handle_local_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, shard_handled_local_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, shard_should_handle_remote_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, shard_handled_remote_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, shard_received_txn_num_local_vec);


    ClearAllThreadLocalCountNum(epoch, remote_server_should_send_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, remote_server_send_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, remote_server_should_handle_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, remote_server_handled_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, remote_server_received_txn_num_local_vec);


    ClearAllThreadLocalCountNum(epoch, backup_should_send_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, backup_send_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, backup_received_txn_num_local_vec);


    ClearAllThreadLocalCountNum(epoch, epoch_should_read_validate_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, epoch_read_validated_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, epoch_should_merge_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, epoch_merged_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, epoch_should_commit_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, epoch_committed_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, epoch_record_commit_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, epoch_record_committed_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, epoch_result_return_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, epoch_result_returned_txn_num_local_vec);

    return true;
}




bool ThreadCounters::CheckEpochShardSendComplete(const uint64_t& epoch) {
    auto epoch_mod = epoch % TaasContext::kCacheMaxLength;
    if(epoch_shard_send_complete[epoch_mod]->load()) {
        return true;
    }
    if (epoch < EpochManager::GetPhysicalEpoch() &&
        IsShardACKReceiveComplete(epoch) &&
        IsShardSendFinish(epoch)
    ) {
        epoch_shard_send_complete[epoch_mod]->store(true);
        return true;
    }
    return false;
}
bool ThreadCounters::CheckEpochShardReceiveComplete(const uint64_t& epoch) {
    auto epoch_mod = epoch % TaasContext::kCacheMaxLength;
    if (epoch_shard_receive_complete[epoch_mod]->load()) return true;
    if (epoch < EpochManager::GetPhysicalEpoch() &&
        IsShardPackReceiveComplete(epoch) &&
        IsShardTxnReceiveComplete(epoch)) {
        epoch_shard_receive_complete[epoch_mod]->store(true);
        return true;
    }
    return false;
}

bool ThreadCounters::IsShardSendFinish(const uint64_t &epoch, const uint64_t &shard_id) {
    return epoch < EpochManager::GetPhysicalEpoch() &&

           GetAllThreadLocalCountNum(epoch, shard_id, shard_send_txn_num_local_vec) >=
               GetAllThreadLocalCountNum(epoch, shard_id, shard_should_send_txn_num_local_vec) &&

           GetAllThreadLocalCountNum(epoch, shard_id, shard_handled_local_txn_num_local_vec) >=
               GetAllThreadLocalCountNum(epoch, shard_id, shard_should_handle_local_txn_num_local_vec);
}
bool ThreadCounters::IsShardSendFinish(const uint64_t &epoch) {
    return epoch < EpochManager::GetPhysicalEpoch() &&

           GetAllThreadLocalCountNum(epoch, shard_send_txn_num_local_vec) >=
               GetAllThreadLocalCountNum(epoch, shard_should_send_txn_num_local_vec) &&

           GetAllThreadLocalCountNum(epoch, shard_handled_local_txn_num_local_vec) >=
               GetAllThreadLocalCountNum(epoch, shard_should_handle_local_txn_num_local_vec)
        ;
}
bool ThreadCounters::IsShardTxnReceiveComplete(const uint64_t &epoch) {
    if(GetAllThreadLocalCountNum(epoch, shard_received_txn_num_local_vec) < shard_should_receive_txn_num.GetCount(epoch))
        return false;
    return true;
}
bool ThreadCounters::IsShardTxnReceiveComplete(const uint64_t &epoch, const uint64_t &id) {
    return GetAllThreadLocalCountNum(epoch, shard_received_txn_num_local_vec) >= shard_should_receive_txn_num.GetCount(epoch, id);
}
bool ThreadCounters::IsShardPackReceiveComplete(const uint64_t &epoch) {
    for(uint64_t i = 0; i < TaasContext::kTxnNodeNum; i ++) {
        if(i == TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, i) == 0) continue;
        if(shard_received_pack_num.GetCount(epoch, i) < shard_should_receive_pack_num.GetCount(epoch, i)) return false;
    }
    return true;
}
bool ThreadCounters::IsShardPackReceiveComplete(const uint64_t &epoch, const uint64_t &id) {
    return shard_received_pack_num.GetCount(epoch, id) >= shard_should_receive_pack_num.GetCount(epoch, id);
}








bool ThreadCounters::CheckEpochRemoteServerSendComplete(const uint64_t& epoch) {
    auto epoch_mod = epoch % TaasContext::kCacheMaxLength;
    if(epoch_remote_server_send_complete[epoch_mod]->load()) {
        return true;
    }
    if (epoch < EpochManager::GetPhysicalEpoch() &&
        IsRemoteServerACKReceiveComplete(epoch) &&
        IsRemoteServerSendFinish(epoch)
    ) {
        epoch_remote_server_send_complete[epoch_mod]->store(true);
        return true;
    }
    return false;
}
bool ThreadCounters::CheckEpochRemoteServerReceiveComplete(const uint64_t& epoch) {
    auto epoch_mod = epoch % TaasContext::kCacheMaxLength;
    if (epoch_remote_server_receive_complete[epoch_mod]->load()) return true;
    if (epoch < EpochManager::GetPhysicalEpoch() &&
        IsRemoteServerPackReceiveComplete(epoch) &&
        IsRemoteServerTxnReceiveComplete(epoch)) {
        epoch_remote_server_receive_complete[epoch_mod]->store(true);
        return true;
    }
    return false;
}

bool ThreadCounters::IsRemoteServerSendFinish(const uint64_t &epoch, const uint64_t &shard_id) {
    return epoch < EpochManager::GetPhysicalEpoch() &&

           GetAllThreadLocalCountNum(epoch, shard_id, remote_server_send_txn_num_local_vec) >=
               GetAllThreadLocalCountNum(epoch, shard_id, remote_server_should_send_txn_num_local_vec) &&

           GetAllThreadLocalCountNum(epoch, shard_id, remote_server_handled_txn_num_local_vec) >=
               GetAllThreadLocalCountNum(epoch, shard_id, remote_server_should_handle_txn_num_local_vec);
}
bool ThreadCounters::IsRemoteServerSendFinish(const uint64_t &epoch) {
    return epoch < EpochManager::GetPhysicalEpoch() &&

           GetAllThreadLocalCountNum(epoch, remote_server_send_txn_num_local_vec) >=
               GetAllThreadLocalCountNum(epoch, remote_server_should_send_txn_num_local_vec) &&

           GetAllThreadLocalCountNum(epoch, remote_server_handled_txn_num_local_vec) >=
               GetAllThreadLocalCountNum(epoch, remote_server_should_handle_txn_num_local_vec)
        ;
}
bool ThreadCounters::IsRemoteServerTxnReceiveComplete(const uint64_t &epoch) {
    if(GetAllThreadLocalCountNum(epoch, remote_server_received_txn_num_local_vec) < remote_server_should_receive_txn_num.GetCount(epoch))
        return false;
    return true;
}
bool ThreadCounters::IsRemoteServerTxnReceiveComplete(const uint64_t &epoch, const uint64_t &id) {
    return GetAllThreadLocalCountNum(epoch, remote_server_received_txn_num_local_vec) >= remote_server_should_receive_txn_num.GetCount(epoch, id);
}
bool ThreadCounters::IsRemoteServerPackReceiveComplete(const uint64_t &epoch) {
    for(uint64_t i = 0; i < TaasContext::kTxnNodeNum; i ++) {
        if(i == TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, i) == 0) continue;
        if(remote_server_received_pack_num.GetCount(epoch, i) < remote_server_should_receive_pack_num.GetCount(epoch, i)) return false;
    }
    return true;
}
bool ThreadCounters::IsRemoteServerPackReceiveComplete(const uint64_t &epoch, const uint64_t &id) {
    return remote_server_received_pack_num.GetCount(epoch, id) >= remote_server_should_receive_pack_num.GetCount(epoch, id);
}








bool ThreadCounters::CheckEpochBackUpComplete(const uint64_t& epoch) {
    auto epoch_mod = epoch % TaasContext::kCacheMaxLength;
    if (epoch_back_up_complete[epoch_mod]->load()) return true;
    if(epoch < EpochManager::GetPhysicalEpoch() && IsBackUpACKReceiveComplete(epoch)
        &&IsBackUpSendFinish(epoch)) {
        epoch_back_up_complete[epoch_mod]->store(true);
        return true;
    }
    return false;
}

bool ThreadCounters::IsBackUpSendFinish(const uint64_t &epoch) {
    return epoch < EpochManager::GetPhysicalEpoch() &&
           GetAllThreadLocalCountNum(epoch, backup_send_txn_num_local_vec) >=
               GetAllThreadLocalCountNum(epoch, backup_should_send_txn_num_local_vec) &&

           GetAllThreadLocalCountNum(epoch, shard_handled_local_txn_num_local_vec) >=
               GetAllThreadLocalCountNum(epoch, shard_should_handle_local_txn_num_local_vec)
        ;
}
bool ThreadCounters::IsBackUpTxnReceiveComplete(const uint64_t &epoch) {
    for(uint64_t i = 0; i < TaasContext::kTxnNodeNum; i ++) {
        if(i == TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, i) == 0) continue;
        if(GetAllThreadLocalCountNum(epoch, i, backup_received_txn_num_local_vec) < backup_should_receive_txn_num.GetCount(epoch, i)) return false;
    }
    return true;
}
bool ThreadCounters::IsBackUpTxnReceiveComplete(const uint64_t &epoch, const uint64_t &id) {
    return GetAllThreadLocalCountNum(epoch, id, backup_received_txn_num_local_vec) >= backup_should_receive_txn_num.GetCount(epoch, id);
}
bool ThreadCounters::IsBackUpPackReceiveComplete(const uint64_t &epoch) {
    for(uint64_t i = 0; i < TaasContext::kTxnNodeNum; i ++) {
        if(i == TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, i) == 0) continue;
        if(backup_received_pack_num.GetCount(epoch, i) < backup_should_receive_pack_num.GetCount(epoch, i)) return false;
    }
    return true;
}
bool ThreadCounters::IsBackUpPackReceiveComplete(const uint64_t &epoch, const uint64_t &id) {
    return backup_received_pack_num.GetCount(epoch, id) >= backup_should_receive_pack_num.GetCount(epoch, id);
}



bool ThreadCounters::CheckEpochAbortSetMergeComplete(const uint64_t& epoch) {
    auto epoch_mod = epoch % TaasContext::kCacheMaxLength;
    if(epoch_abort_set_merge_complete[epoch_mod]->load()) return true;
    if(epoch < EpochManager::GetPhysicalEpoch() &&
        IsAbortSetACKReceiveComplete(epoch) &&
        IsAbortSetReceiveComplete(epoch)
    ) {
        epoch_abort_set_merge_complete[epoch_mod]->store(true);
        return true;
    }
    return false;
}
bool ThreadCounters::CheckEpochInsertSetMergeComplete(const uint64_t& epoch) {
    auto epoch_mod = epoch % TaasContext::kCacheMaxLength;
    if(epoch_insert_set_complete[epoch_mod]->load()) return true;
    if(epoch < EpochManager::GetPhysicalEpoch() &&
        IsInsertSetACKReceiveComplete(epoch) &&
        IsInsertSetReceiveComplete(epoch)
    ) {
        epoch_insert_set_complete[epoch_mod]->store(true);
        return true;
    }
    return false;
}

bool ThreadCounters::IsAbortSetReceiveComplete(const uint64_t &epoch, const uint64_t &id) {
    return abort_set_received_num.GetCount(epoch, id) >= abort_set_should_receive_num.GetCount(epoch, id);
}
bool ThreadCounters::IsAbortSetReceiveComplete(const uint64_t &epoch) {
    for(uint64_t i = 0; i < TaasContext::kTxnNodeNum; i ++) {
        if(i == TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, i) == 0) continue;
        if(abort_set_received_num.GetCount(epoch, i) < abort_set_should_receive_num.GetCount(epoch, i)) return false;
    }
    return true;
}
bool ThreadCounters::IsInsertSetReceiveComplete(const uint64_t &epoch, const uint64_t &id) {
    return insert_set_received_num.GetCount(epoch, id) >= insert_set_should_receive_num.GetCount(epoch, id);
}
bool ThreadCounters::IsInsertSetReceiveComplete(const uint64_t &epoch) {
    for(uint64_t i = 0; i < TaasContext::kTxnNodeNum; i ++) {
        if(i == TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, i) == 0) continue;
        if(insert_set_received_num.GetCount(epoch, i) < insert_set_should_receive_num.GetCount(epoch, i)) return false;
    }
    return true;
}





bool ThreadCounters::IsShardACKReceiveComplete(const uint64_t &epoch) {
    for(uint64_t i = 0; i < TaasContext::kTxnNodeNum; i ++) {
        if(i == TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, i) == 0) continue;
        if(shard_received_ack_num.GetCount(epoch, i) < shard_should_receive_pack_num.GetCount(epoch, i)) return false;
    }
    return true;
}
bool ThreadCounters::IsRemoteServerACKReceiveComplete(const uint64_t &epoch) {
    for(uint64_t i = 0; i < TaasContext::kTxnNodeNum; i ++) {
        if(i == TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, i) == 0) continue;
        if(remote_server_received_ack_num.GetCount(epoch, i) < remote_server_should_receive_pack_num.GetCount(epoch, i)) return false;
    }
    return true;
}
bool ThreadCounters::IsBackUpACKReceiveComplete(const uint64_t &epoch) {
    uint64_t to_id ;
    for(uint64_t i = 0; i < TaasContext::kBackUpNum; i ++) { /// send to i+1, i+2...i+kBackNum-1
        to_id = (TaasContext::txn_node_ip_index + i + 1) % TaasContext::kTxnNodeNum;
        if(to_id == (uint64_t)TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, to_id) == 0) continue;
        if(backup_received_ack_num.GetCount(epoch, to_id) < backup_should_receive_pack_num.GetCount(epoch, to_id)) return false;
    }
    return true;
}
bool ThreadCounters::IsAbortSetACKReceiveComplete(const uint64_t &epoch) {
    for(uint64_t i = 0; i < TaasContext::kTxnNodeNum; i ++) {
        if(i == TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, i) == 0) continue;
        if(abort_set_received_ack_num.GetCount(epoch, i) < abort_set_should_receive_num.GetCount(epoch, i)) return false;
    }
    return true;
}
bool ThreadCounters::IsInsertSetACKReceiveComplete(const uint64_t &epoch) {
    for(uint64_t i = 0; i < TaasContext::kTxnNodeNum; i ++) {
        if(i == TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, i) == 0) continue;
        if(insert_set_received_ack_num.GetCount(epoch, i) < insert_set_should_receive_num.GetCount(epoch, i)) return false;
    }
    return true;
}
bool ThreadCounters::IsRedoLogPushDownACKReceiveComplete(const uint64_t &epoch) {
    for(uint64_t i = 0; i < TaasContext::kTxnNodeNum; i ++) {
        if(i == TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, i) == 0) continue;
        if(redo_log_push_down_ack_num.GetCount(epoch, i) < EpochManager::server_state.GetCount(epoch, i)) return false;
    }
    return true;
}



bool ThreadCounters::CheckEpochClientTxnHandleComplete(const uint64_t &epoch) {
    auto epoch_mod = epoch % TaasContext::kCacheMaxLength;
    if(epoch_shard_handle_complete[epoch_mod]->load()) {
        return true;
    }
    else {
        if(epoch < EpochManager::GetPhysicalEpoch() &&
            GetAllThreadLocalCountNum(epoch, shard_handled_local_txn_num_local_vec) >=
                GetAllThreadLocalCountNum(epoch, shard_should_handle_local_txn_num_local_vec)) {
            epoch_shard_handle_complete[epoch_mod]->store(true);
            return true;
        }
        return false;
    }
}

bool ThreadCounters::CheckEpochShardTxnHandleComplete(const uint64_t &epoch) {
    auto epoch_mod = epoch % TaasContext::kCacheMaxLength;
    if(epoch_remote_server_handle_complete[epoch_mod]->load()) {
        return true;
    }
    else {
        if(epoch < EpochManager::GetPhysicalEpoch() &&
            GetAllThreadLocalCountNum(epoch, remote_server_handled_txn_num_local_vec) >=
                GetAllThreadLocalCountNum(epoch, remote_server_should_handle_txn_num_local_vec)) {
            epoch_remote_server_handle_complete[epoch_mod]->store(true);
            return true;
        }
        return false;
    }
}






















bool ThreadCounters::CheckEpochReadValidateComplete(const uint64_t& epoch) {
    if(epoch_read_validate_complete[epoch % TaasContext::kCacheMaxLength]->load()) {
        return true;
    }
    if (epoch < EpochManager::GetPhysicalEpoch() && IsReadValidateComplete(epoch)) {
        epoch_read_validate_complete[epoch % TaasContext::kCacheMaxLength]->store(true);
        return true;
    }
    return false;
}
bool ThreadCounters::CheckEpochMergeComplete(const uint64_t& epoch) {
    if(epoch_merge_complete[epoch % TaasContext::kCacheMaxLength]->load()) {
        return true;
    }
    if (epoch < EpochManager::GetPhysicalEpoch() && IsMergeComplete(epoch)) {
        epoch_merge_complete[epoch % TaasContext::kCacheMaxLength]->store(true);
        return true;
    }
    return false;
}
bool ThreadCounters::CheckEpochCommitComplete(const uint64_t& epoch) {
    if (epoch_commit_complete[epoch % TaasContext::kCacheMaxLength]->load()) return true;
    if (epoch < EpochManager::GetPhysicalEpoch() && IsCommitComplete(epoch)) {
        epoch_commit_complete[epoch % TaasContext::kCacheMaxLength]->store(true);
        return true;
    }
    return false;
}
bool ThreadCounters::CheckEpochRecordCommitted(const uint64_t& epoch) {
    if (epoch_record_committed[epoch % TaasContext::kCacheMaxLength]->load()) return true;
    if (epoch < EpochManager::GetPhysicalEpoch() && IsCommitComplete(epoch) && IsRecordCommitted(epoch)) {
        epoch_record_committed[epoch % TaasContext::kCacheMaxLength]->store(true);
        return true;
    }
    return false;
}

bool ThreadCounters::CheckEpochResultReturned(const uint64_t& epoch) {
    if (epoch_result_returned[epoch % TaasContext::kCacheMaxLength]->load()) return true;
    if (epoch < EpochManager::GetPhysicalEpoch() && IsRecordCommitted(epoch) && IsResultReturned(epoch)) {
        epoch_result_returned[epoch % TaasContext::kCacheMaxLength]->store(true);
        return true;
    }
    return false;
}





bool ThreadCounters::IsReadValidateComplete(const uint64_t& epoch) {
    if(GetAllThreadLocalCountNum(epoch, epoch_should_read_validate_txn_num_local_vec) >
        GetAllThreadLocalCountNum(epoch, epoch_read_validated_txn_num_local_vec))
        return false;
    return true;
}
bool ThreadCounters::IsMergeComplete(const uint64_t& epoch) {
    if(GetAllThreadLocalCountNum(epoch,epoch_should_read_validate_txn_num_local_vec) >
        GetAllThreadLocalCountNum(epoch,epoch_read_validated_txn_num_local_vec))
        return false;
    if(GetAllThreadLocalCountNum(epoch, epoch_should_merge_txn_num_local_vec) >
        GetAllThreadLocalCountNum(epoch, epoch_merged_txn_num_local_vec))
        return false;
    return true;
}
bool ThreadCounters::IsCommitComplete(const uint64_t & epoch) {
    if(GetAllThreadLocalCountNum(epoch, epoch_should_commit_txn_num_local_vec) >
        GetAllThreadLocalCountNum(epoch, epoch_committed_txn_num_local_vec))
        return false;
    return true;
}
bool ThreadCounters::IsRecordCommitted(const uint64_t & epoch) {
    if(GetAllThreadLocalCountNum(epoch, epoch_record_commit_txn_num_local_vec) >
        GetAllThreadLocalCountNum(epoch, epoch_record_committed_txn_num_local_vec))
        return false;
    return true;
}

bool ThreadCounters::IsResultReturned(const uint64_t & epoch) {
    if(GetAllThreadLocalCountNum(epoch, epoch_result_return_txn_num_local_vec) >
        GetAllThreadLocalCountNum(epoch, epoch_result_returned_txn_num_local_vec))
        return false;
    return true;
}

void ThreadCounters::ClearAllThreadLocalCountNum(const uint64_t &epoch, const std::vector<std::shared_ptr<AtomicCounters_Cache>> &vec) {
    for(const auto& i : vec) {
        if(i != nullptr)
            i->Clear(epoch);
    }
}

uint64_t ThreadCounters::GetAllThreadLocalCountNum(const uint64_t &epoch, const std::vector<std::shared_ptr<AtomicCounters_Cache>> &vec) {
    uint64_t ans = 0;
    for(const auto& i : vec) {
        if(i != nullptr)
            ans += i->GetCount(epoch);
    }
    return ans;
}
uint64_t ThreadCounters::GetAllThreadLocalCountNum(const uint64_t &epoch, const uint64_t &shard_id, const std::vector<std::shared_ptr<AtomicCounters_Cache>> &vec) {
    uint64_t ans = 0;
    for(const auto& i : vec) {
        if(i != nullptr)
            ans += i->GetCount(epoch, shard_id);
    }
    return ans;
}







///Message

struct pack_params {
    uint64_t id{};/// send to whom
    uint64_t time{};
    std::string ip; /// send to whom
    uint64_t epoch{};
    proto::TxnType type{};
    std::unique_ptr<std::string> str;
    std::shared_ptr<proto::Transaction> txn;
    explicit pack_params(uint64_t id_, uint64_t time_, std::string ip_, uint64_t e = 0, proto::TxnType ty = proto::TxnType::NullMark,
        std::unique_ptr<std::string> && s = nullptr, std::shared_ptr<proto::Transaction> &&t = nullptr):
          id(id_), time(time_), ip(std::move(ip_)), epoch(e), type(ty), str(std::move(s)), txn(std::move(t)){}
    pack_params()= default;
};

struct send_params {
    uint64_t id{}; /// send to whom
    uint64_t time{};
    std::string ip; /// send to whom
    uint64_t epoch{};
    proto::TxnType type{};
    std::unique_ptr<std::string> str;
    std::shared_ptr<proto::Transaction> txn;
    bool send_to_all;
    //        send_params(uint64_t id_, uint64_t time_, std::string ip_, uint64_t e = 0, proto::TxnType ty = proto::TxnType::NullMark,
    //                    std::unique_ptr<std::string> && s = nullptr, std::shared_ptr<proto::Transaction> &&t = nullptr):
    //                id(id_), time(time_), ip(std::move(ip_)), epoch(e), type(ty), str(std::move(s)), txn(std::move(t)), send_to_all(false){}
    send_params(uint64_t id_, uint64_t time_, std::string ip_, uint64_t e = 0, proto::TxnType ty = proto::TxnType::NullMark,
        std::unique_ptr<std::string> && s = nullptr, std::shared_ptr<proto::Transaction> &&t = nullptr, bool send_to_all_t = false):
          id(id_), time(time_), ip(std::move(ip_)), epoch(e), type(ty), str(std::move(s)), txn(std::move(t)), send_to_all(send_to_all_t){}
    send_params()= default;
};

class MessageQueue{
public:
    static std::unique_ptr<MessageBlockingConcurrentQueue<std::unique_ptr<zmq::message_t>>> listen_message_queue, listen_message_txn_queue, listen_message_epoch_queue;
    static std::unique_ptr<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>> send_to_server_queue, send_to_client_queue,
        send_to_storage_queue, send_to_mot_storage_queue, send_to_nebula_storage_queue, send_to_server_pub_queue;
    static std::unique_ptr<MessageBlockingConcurrentQueue<std::unique_ptr<proto::Message>>> request_queue, raft_message_queue;
    static void StaticInitMessageQueue();
    static std::atomic<uint64_t> client_receive_message_num, client_send_message_num;
};


// 接受client和peer txn node发来的写集，都放在listen_message_queue中
std::unique_ptr<MessageBlockingConcurrentQueue<std::unique_ptr<zmq::message_t>>>
    MessageQueue::listen_message_queue, MessageQueue::listen_message_txn_queue, MessageQueue::listen_message_epoch_queue;
//    std::unique_ptr<MessageBlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>> MessageQueue::listen_message_txn_queue, MessageQueue::listen_message_epoch_queue;
std::unique_ptr<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>> MessageQueue::send_to_server_queue,
    MessageQueue::send_to_client_queue, MessageQueue::send_to_storage_queue, MessageQueue::send_to_mot_storage_queue,
    MessageQueue::send_to_nebula_storage_queue, MessageQueue::send_to_server_pub_queue;
std::unique_ptr<MessageBlockingConcurrentQueue<std::unique_ptr<proto::Message>>> MessageQueue::request_queue,
    MessageQueue::raft_message_queue;
std::atomic<uint64_t> MessageQueue::client_receive_message_num, MessageQueue::client_send_message_num;

void MessageQueue::StaticInitMessageQueue() {
    listen_message_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<zmq::message_t>>>();
    listen_message_txn_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<zmq::message_t>>>();
    listen_message_epoch_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<zmq::message_t>>>();
    send_to_server_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>>();
    send_to_server_pub_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>>();
    send_to_client_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>>();
    send_to_storage_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>>();
    send_to_mot_storage_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>>();
    send_to_nebula_storage_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>>();
    request_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<proto::Message>>>();
    raft_message_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<proto::Message>>>();
}

// 接受client和peer txn node发来的写集，都放在listen_message_queue中
std::unique_ptr<MessageBlockingConcurrentQueue<std::unique_ptr<zmq::message_t>>>
    MessageQueue::listen_message_queue, MessageQueue::listen_message_txn_queue, MessageQueue::listen_message_epoch_queue;
//    std::unique_ptr<MessageBlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>> MessageQueue::listen_message_txn_queue, MessageQueue::listen_message_epoch_queue;
std::unique_ptr<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>> MessageQueue::send_to_server_queue,
    MessageQueue::send_to_client_queue, MessageQueue::send_to_storage_queue, MessageQueue::send_to_mot_storage_queue,
    MessageQueue::send_to_nebula_storage_queue, MessageQueue::send_to_server_pub_queue;
std::unique_ptr<MessageBlockingConcurrentQueue<std::unique_ptr<proto::Message>>> MessageQueue::request_queue,
    MessageQueue::raft_message_queue;
std::atomic<uint64_t> MessageQueue::client_receive_message_num, MessageQueue::client_send_message_num;

void MessageQueue::StaticInitMessageQueue() {
    listen_message_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<zmq::message_t>>>();
    listen_message_txn_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<zmq::message_t>>>();
    listen_message_epoch_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<zmq::message_t>>>();
    send_to_server_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>>();
    send_to_server_pub_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>>();
    send_to_client_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>>();
    send_to_storage_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>>();
    send_to_mot_storage_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>>();
    send_to_nebula_storage_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<send_params>>>();
    request_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<proto::Message>>>();
    raft_message_queue = std::make_unique<MessageBlockingConcurrentQueue<std::unique_ptr<proto::Message>>>();
}



class EpochMessageReceiveHandler : public ThreadCounters {
public:
    bool Init(const uint64_t &id);

    void HandleReceivedMessage();
    void TryHandleReceivedMessage();
    void HandleReceivedControlMessage();
    void TryHandleReceivedControlMessage();
    bool SetMessageRelatedCountersInfo();
    bool HandleReceivedTxn();
    void HandleMultiModelClientSubTxn(const uint64_t& txn_id);
    uint64_t getMultiModelTxnId();
    bool HandleMultiModelClientTxn();
    bool UpdateEpochAbortSet();


    [[nodiscard]] uint64_t GetHashValue(const std::string& key) const {
        return _hash(key) % shard_num;
    }

    void ReadValidateQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction> &txn_ptr_);
    void MergeQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_);
    void CommitQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_);
    void RedoLogQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_);
    void ResultReturnQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_);

    static bool StaticInit();
    static bool StaticClear([[maybe_unused]] uint64_t& epoch);

private:
    std::unique_ptr<zmq::message_t> message_ptr;
    std::unique_ptr<std::string> message_string_ptr;
    std::unique_ptr<proto::Message> msg_ptr;
    std::shared_ptr<proto::Transaction> txn_ptr;
    std::unique_ptr<pack_params> pack_param;
    std::string csn_temp, key_temp, key_str, table_name, csn_result;
    uint64_t total_single_shard_time = 0, total_single_remote_handle_time = 0, total_single_shard_num = 0, total_single_remote_handle_num = 0;
    uint64_t thread_id = 0, local_server_id = 0, epoch_mod = 0, epoch = 0, max_length = 0, server_num = 1, shard_num = 0, replica_num = 1,
             round_robin = 0, sent_to = 0,///cache check
        message_epoch = 0, message_epoch_mod = 0, message_server_id = 0, txn_server_id = 0,shard_id = 0, shard_server_id =0, ///message epoch info
        server_reply_ack_id = 0,
             cache_clear_epoch_num = 0, cache_clear_epoch_num_mod = 0,
             redo_log_push_down_reply = 1;
    std::vector<std::vector<bool>> is_local_shard;
public:
    bool res, sleep_flag;
    std::shared_ptr<proto::Transaction> empty_txn_ptr;
    std::hash<std::string> _hash;


public:
    void Shard();

    bool UpdateMetaInfo();
};


bool EpochMessageReceiveHandler::Init(const uint64_t &id) {
    message_ptr = nullptr;
    txn_ptr.reset();
    thread_id = id;
    server_num = TaasContext::kTxnNodeNum;
    shard_num = TaasContext::kShardNum;
    replica_num = TaasContext::kReplicaNum;
    local_server_id = TaasContext::txn_node_ip_index;
    max_length = TaasContext::kCacheMaxLength;
    ThreadCountersInit(ctx);

    server_num = TaasContext::kTxnNodeNum,
    shard_num = TaasContext::kShardNum,
    replica_num = TaasContext::kReplicaNum,
    local_server_id = TaasContext::txn_node_ip_index,
    max_length = TaasContext::kCacheMaxLength;

    is_local_shard.resize(server_num);
    for(auto &i : is_local_shard) {
        i.resize(shard_num);
    }
    for(uint64_t server_id = 0; server_id < server_num; server_id ++) {
        for(uint64_t i = 0; i < shard_num; i ++) {
            for(uint64_t j = 0; j < replica_num; j ++ ) {
                if((i + server_num + j) % server_num == server_id) {
                    is_local_shard[server_id][i] = true;
                }
            }
        }
    }

    return true;
}

bool EpochMessageReceiveHandler::StaticInit() {
    return true;
}

bool EpochMessageReceiveHandler::StaticClear([[maybe_unused]] uint64_t& epoch) {
    return true;
}

void EpochMessageReceiveHandler::ReadValidateQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_) {
    auto epoch_mod_temp = epoch_ % TaasContext::kCacheMaxLength;
    epoch_should_read_validate_txn_num_local->IncCount(epoch_mod_temp, txn_ptr_->txn_server_id(), 1);
    TransactionCache::epoch_read_validate_queue[epoch_mod_temp]->enqueue(txn_ptr_);
    TransactionCache::epoch_read_validate_queue[epoch_mod_temp]->enqueue(nullptr);
}
void EpochMessageReceiveHandler::MergeQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_) {
    auto epoch_mod_temp = epoch_ % TaasContext::kCacheMaxLength;
    epoch_should_merge_txn_num_local->IncCount(epoch_mod_temp, txn_ptr->txn_server_id(), 1);
    TransactionCache::epoch_merge_queue[epoch_mod_temp]->enqueue(txn_ptr_);
    TransactionCache::epoch_merge_queue[epoch_mod_temp]->enqueue(nullptr);
}
void EpochMessageReceiveHandler::CommitQueueEnqueue(uint64_t& epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_) {
    auto epoch_mod_temp = epoch_ % TaasContext::kCacheMaxLength;
    epoch_should_commit_txn_num_local->IncCount(epoch_mod_temp, txn_ptr_->txn_server_id(), 1);
    TransactionCache::epoch_commit_queue[epoch_mod_temp]->enqueue(txn_ptr_);
    TransactionCache::epoch_commit_queue[epoch_mod_temp]->enqueue(nullptr);
}
void EpochMessageReceiveHandler::RedoLogQueueEnqueue(uint64_t& epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_) {
    auto epoch_mod_temp = epoch_ % TaasContext::kCacheMaxLength;
    epoch_record_commit_txn_num_local->IncCount(epoch_mod_temp, txn_ptr_->txn_server_id(), 1);
    TransactionCache::epoch_redo_log_queue[epoch_mod_temp]->enqueue(txn_ptr_);
    TransactionCache::epoch_redo_log_queue[epoch_mod_temp]->enqueue(nullptr);
}
void EpochMessageReceiveHandler::ResultReturnQueueEnqueue(uint64_t& epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_) {
    auto epoch_mod_temp = epoch_ % TaasContext::kCacheMaxLength;
    epoch_result_return_txn_num_local->IncCount(epoch_mod_temp, txn_ptr_->txn_server_id(), 1);
    TransactionCache::epoch_result_return_queue[epoch_mod_temp]->enqueue(txn_ptr_);
    TransactionCache::epoch_result_return_queue[epoch_mod_temp]->enqueue(nullptr);
}

void EpochMessageReceiveHandler::HandleReceivedMessage() {
    auto safe_length = TaasContext::kSafeEpochDistance;
    while(!EpochManager::IsTimerStop()) {
        //            while( EpochManager::GetLogicalEpoch() + safe_length > EpochManager::GetPhysicalEpoch() ) {
        //                usleep(TaasContext::kEpochSize_us);
        //            }
        MessageQueue::listen_message_txn_queue->wait_dequeue(message_ptr);
        if (message_ptr == nullptr || message_ptr->empty()) continue;
        message_string_ptr = std::make_unique<std::string>(static_cast<const char *>(message_ptr->data()), message_ptr->size());
        msg_ptr = std::make_unique<proto::Message>();
        res = UnGzip(msg_ptr.get(), message_string_ptr.get());
        assert(res);
        txn_ptr = std::make_shared<proto::Transaction>(msg_ptr->txn());
        HandleReceivedTxn();
        txn_ptr.reset();
    }
}

void EpochMessageReceiveHandler::TryHandleReceivedMessage() {
    sleep_flag = true;
    //        if(MessageQueue::listen_message_txn_queue->try_dequeue(message_ptr)) {
    //            sleep_flag = false;
    //            if (message_ptr == nullptr || message_ptr->empty()) return;
    //            message_string_ptr = std::make_unique<std::string>(static_cast<const char *>(message_ptr->data()),message_ptr->size());
    //            msg_ptr = std::make_unique<proto::Message>();
    //            res = UnGzip(msg_ptr.get(), message_string_ptr.get());
    //            assert(res);
    //            txn_ptr = std::make_shared<proto::Transaction>(msg_ptr->txn());
    //            HandleReceivedTxn();
    //            txn_ptr.reset();
    //        }
    for(int i = 0; i < (int)TaasContext::kHandleTxnMessageNumOfEachTraversal; i ++) {
        if(MessageQueue::listen_message_txn_queue->try_dequeue(message_ptr)) {
            sleep_flag = false;
            if (message_ptr == nullptr) return;
            if(message_ptr->empty()) {
                txn_ptr.reset();
                continue;
            }
            message_string_ptr = std::make_unique<std::string>(static_cast<const char *>(message_ptr->data()),message_ptr->size());
            msg_ptr = std::make_unique<proto::Message>();
            res = UnGzip(msg_ptr.get(), message_string_ptr.get());
            assert(res);
            txn_ptr = std::make_shared<proto::Transaction>(msg_ptr->txn());
            HandleReceivedTxn();
            //            if(txn_ptr->commit_epoch() > EpochManager::GetLogicalEpoch()) return ;
            txn_ptr.reset();
        }
    }
}

void EpochMessageReceiveHandler::HandleReceivedControlMessage() {
    while(!EpochManager::IsTimerStop()) {
        MessageQueue::listen_message_epoch_queue->wait_dequeue(message_ptr);
        if (message_ptr == nullptr || message_ptr->empty()) continue;
        message_string_ptr = std::make_unique<std::string>(static_cast<const char *>(message_ptr->data()),message_ptr->size());
        msg_ptr = std::make_unique<proto::Message>();
        res = UnGzip(msg_ptr.get(), message_string_ptr.get());
        assert(res);
        txn_ptr = std::make_shared<proto::Transaction>(msg_ptr->txn());
        HandleReceivedTxn();
        txn_ptr.reset();
        if(total_single_shard_num > 0 && total_single_shard_num % TaasContext::print_mode_size == 0) {
//            LOG(INFO) << "ClientTxnHandle Time Cost : " << total_single_shard_time  << " ClientTxnHandle Time count : " << total_single_shard_num << " ClientTxnHandle avg: " << total_single_shard_time/total_single_shard_num
//                      << "ShardedClientTxn Time Cost : " << total_single_remote_handle_time << " ShardedClientTxn Time count : " << total_single_remote_handle_num << " ShardedClientTxn Tavg: " << total_single_remote_handle_time/total_single_remote_handle_num
//                      << " end";
        }
    }
}

void EpochMessageReceiveHandler::TryHandleReceivedControlMessage() {
    sleep_flag = true;
    //        if(MessageQueue::listen_message_epoch_queue->try_dequeue(message_ptr)) {
    //            sleep_flag = false;
    //            if (message_ptr == nullptr || message_ptr->empty()) return;
    //            message_string_ptr = std::make_unique<std::string>(static_cast<const char *>(message_ptr->data()),message_ptr->size());
    //            msg_ptr = std::make_unique<proto::Message>();
    //            res = UnGzip(msg_ptr.get(), message_string_ptr.get());
    //            assert(res);
    //            txn_ptr = std::make_shared<proto::Transaction>(msg_ptr->txn());
    //            HandleReceivedTxn();
    ////            if(txn_ptr->commit_epoch() > EpochManager::GetLogicalEpoch()) return ;
    //            txn_ptr.reset();
    //        }
    for(int i = 0; i < (int)TaasContext::kHandleEpochMessageNumOfEachTraversal; i ++) {
        if(MessageQueue::listen_message_epoch_queue->try_dequeue(message_ptr)) {
            sleep_flag = false;
            if (message_ptr == nullptr) return;
            if(message_ptr->empty()) {
                txn_ptr.reset();
                continue;
            }
            message_string_ptr = std::make_unique<std::string>(static_cast<const char *>(message_ptr->data()),message_ptr->size());
            msg_ptr = std::make_unique<proto::Message>();
            res = UnGzip(msg_ptr.get(), message_string_ptr.get());
            assert(res);
            txn_ptr = std::make_shared<proto::Transaction>(msg_ptr->txn());
            HandleReceivedTxn();
            //            if(txn_ptr->commit_epoch() > EpochManager::GetLogicalEpoch()) return ;
            txn_ptr.reset();
        }
    }
}








void EpochMessageReceiveHandler::Shard() {
    auto shard_row_vector = std::make_shared<std::vector<std::shared_ptr<proto::Transaction>>>() ;
    for(uint64_t i = 0; i < shard_num; i ++) {
        shard_row_vector->emplace_back(std::make_shared<proto::Transaction>());
        auto vector_i = &(*((*shard_row_vector)[i]));
        vector_i->set_csn(txn_ptr->csn());
        vector_i->set_commit_epoch(txn_ptr->commit_epoch());
        vector_i->set_txn_server_id(txn_ptr->txn_server_id());
        vector_i->set_client_ip(txn_ptr->client_ip());
        vector_i->set_client_txn_id(txn_ptr->client_txn_id());


        vector_i->set_message_server_id(local_server_id);
        vector_i->set_shard_id(i);
        vector_i->set_txn_type(proto::ShardedClientTxn);
    }
    for(auto i = 0; i < txn_ptr->row_size(); i ++) {
        const auto& row = txn_ptr->row(i);
        auto row_ptr = (*shard_row_vector)[GetHashValue(row.key())]->add_row();
        (*row_ptr) = row;
    }
    for(uint64_t i = 0; i < shard_num; i ++) {
        if(is_local_shard[local_server_id][i]) {
            ReadValidateQueueEnqueue(message_epoch, (*shard_row_vector)[i]);
            MergeQueueEnqueue(message_epoch, (*shard_row_vector)[i]);
            CommitQueueEnqueue(message_epoch, (*shard_row_vector)[i]);

            shard_id = i;
            for(uint64_t j = 0; j < TaasContext::kReplicaNum; j ++ ) { /// use the network for reducing the merge time
                auto to_id = (shard_id + TaasContext::kTxnNodeNum + j) % TaasContext::kTxnNodeNum;
                if (to_id == TaasContext::txn_node_ip_index) continue;
                remote_server_should_send_txn_num_local->IncCount(message_epoch, to_id, 1);
            }
            EpochMessageSendHandler::SendTxnToServer(message_epoch, shard_id, txn_ptr, proto::TxnType::RemoteServerTxn);
            for(uint64_t j = 0; j < TaasContext::kReplicaNum; j ++ ) {
                auto to_id = (shard_id + TaasContext::kTxnNodeNum + j) % TaasContext::kTxnNodeNum;
                if (to_id == TaasContext::txn_node_ip_index) continue;
                remote_server_send_txn_num_local->IncCount(message_epoch, to_id, 1);
            }
        } else {
            if((*shard_row_vector)[i]->row_size() > 0) {
                round_robin = (round_robin + 1) % replica_num;
                sent_to = (i + round_robin) % server_num;
                shard_should_send_txn_num_local->IncCount(message_epoch, sent_to, 1); //use server_id to send EpochShardEndMessage
                (*shard_row_vector)[i]->set_shard_server_id(sent_to);
                EpochMessageSendHandler::SendTxnToServer(message_epoch, sent_to, (*shard_row_vector)[i], proto::TxnType::ShardedClientTxn);
                shard_send_txn_num_local->IncCount(message_epoch, sent_to, 1);
            }
        }
    }
    backup_should_send_txn_num_local->IncCount(message_epoch, txn_server_id, 1);
    EpochMessageSendHandler::SendTxnToServer(message_epoch, txn_server_id, txn_ptr, proto::TxnType::BackUpTxn);
    RedoLogQueueEnqueue(message_epoch, txn_ptr); /// full txn for redo log
    ResultReturnQueueEnqueue(message_epoch, txn_ptr); /// return result to users
    backup_send_txn_num_local->IncCount(message_epoch, txn_server_id, 1);
}

bool EpochMessageReceiveHandler::SetMessageRelatedCountersInfo() {
    message_epoch = txn_ptr->commit_epoch();
    message_epoch_mod = message_epoch % TaasContext::kCacheMaxLength;
    txn_server_id = txn_ptr->txn_server_id();
    shard_id = txn_ptr->shard_id();
    shard_server_id = txn_ptr->shard_server_id();
    message_server_id = txn_ptr->message_server_id();
    csn_temp = std::to_string(txn_ptr->csn()) + ":" + std::to_string(txn_ptr->txn_server_id());
    return true;
}

bool EpochMessageReceiveHandler::HandleReceivedTxn() {
    SetMessageRelatedCountersInfo();
    switch (txn_ptr->txn_type()) {
        ///这里需要注意 这几个计数器是以server_id为粒度增加的，不是线程id ！！！
        case proto::TxnType::ClientTxn : {/// sql node --> txn node
            if(TaasContext::taasMode == TaasMode::MultiModel) {
                HandleMultiModelClientTxn();
            }
            else {
                auto time1 = now_to_us();
                message_epoch = EpochManager::GetPhysicalEpoch();
                shard_should_handle_local_txn_num_local->IncCount(message_epoch, local_server_id, 1);
                txn_ptr->set_commit_epoch(message_epoch);
                txn_ptr->set_csn(now_to_us());
                txn_ptr->set_txn_server_id(local_server_id);
                txn_ptr->set_txn_type(proto::RemoteServerTxn);
                SetMessageRelatedCountersInfo();
                Shard();
                shard_handled_local_txn_num_local->IncCount(message_epoch, local_server_id, 1);
                //                    LOG(INFO) << "ClientTxnHandle Time Cost " << now_to_us() - time1 << " us";
                total_single_shard_time += now_to_us() - time1 ;
                total_single_shard_num  ++;
            }
            break;
        }
        case proto::TxnType::ShardedClientTxn : {
            auto time1 = now_to_us();
            shard_should_handle_remote_txn_num_local->IncCount(message_epoch, message_server_id, 1);

            ReadValidateQueueEnqueue(message_epoch, txn_ptr);
            MergeQueueEnqueue(message_epoch, txn_ptr);
            CommitQueueEnqueue(message_epoch, txn_ptr);
            shard_received_txn_num_local->IncCount(message_epoch, message_server_id, 1);

            shard_id = txn_ptr->shard_id();
            assert(is_local_shard[local_server_id][shard_id]);
            for(uint64_t j = 0; j < TaasContext::kReplicaNum; j ++ ) { /// use the network for reducing the merge time
                auto to_id = (shard_id + TaasContext::kTxnNodeNum + j) % TaasContext::kTxnNodeNum;
                if (to_id == TaasContext::txn_node_ip_index) continue;
                remote_server_should_send_txn_num_local->IncCount(message_epoch, to_id, 1);
            }
            EpochMessageSendHandler::SendTxnToServer(message_epoch, shard_id, txn_ptr, proto::TxnType::RemoteServerTxn);
            for(uint64_t j = 0; j < TaasContext::kReplicaNum; j ++ ) {
                auto to_id = (shard_id + TaasContext::kTxnNodeNum + j) % TaasContext::kTxnNodeNum;
                if (to_id == TaasContext::txn_node_ip_index) continue;
                remote_server_send_txn_num_local->IncCount(message_epoch, to_id, 1);
            }

            //                TransactionCache::epoch_txn_map[message_epoch_mod]->insert(csn_temp, txn_ptr);

            shard_handled_remote_txn_num_local->IncCount(message_epoch, message_server_id, 1);
            //                LOG(INFO) << "ShardedClientTxn Time Cost " << now_to_us() - time1 << " us";
            total_single_remote_handle_time += now_to_us() - time1 ;
            total_single_remote_handle_num ++;
            break;
        }
        case proto::TxnType::RemoteServerTxn : {
            remote_server_should_handle_txn_num_local->IncCount(message_epoch, message_server_id, 1);
            //                TransactionCache::epoch_txn_map[message_epoch_mod]->insert(csn_temp, txn_ptr);
            MergeQueueEnqueue(message_epoch, txn_ptr);
            CommitQueueEnqueue(message_epoch, txn_ptr);
            remote_server_received_txn_num_local->IncCount(message_epoch, message_server_id, 1);
            remote_server_handled_txn_num_local->IncCount(message_epoch, message_server_id, 1);
            break;
        }
        case proto::TxnType::BackUpTxn : {
            //                TransactionCache::epoch_back_txn_map[message_epoch_mod]->insert(csn_temp, txn_ptr);
            backup_received_txn_num_local->IncCount(message_epoch, message_server_id, 1);
            break;
        }
        case proto::TxnType::EpochShardEndFlag : {
            shard_should_receive_txn_num.IncCount(message_epoch, message_server_id,txn_ptr->csn());
            shard_received_pack_num.IncCount(message_epoch, message_server_id, 1);
            CheckEpochShardReceiveComplete(message_epoch);
            EpochMessageSendHandler::SendTxnToServer(message_epoch, message_server_id, empty_txn_ptr, proto::TxnType::EpochShardACK);
            break;
        }
        case proto::EpochRemoteServerEndFlag : {
            remote_server_should_receive_txn_num.IncCount(message_epoch, message_server_id,txn_ptr->csn());
            remote_server_received_pack_num.IncCount(message_epoch, message_server_id, 1);
            CheckEpochRemoteServerReceiveComplete(message_epoch);
            EpochMessageSendHandler::SendTxnToServer(message_epoch, message_server_id, empty_txn_ptr, proto::TxnType::EpochRemoteServerACK);
            break;
        }
        case proto::TxnType::EpochBackUpEndFlag : {
            backup_should_receive_txn_num.IncCount(message_epoch, message_server_id, txn_ptr->csn());
            backup_received_pack_num.IncCount(message_epoch, message_server_id, 1);
            EpochMessageSendHandler::SendTxnToServer(message_epoch, message_server_id, empty_txn_ptr, proto::TxnType::BackUpACK);
            break;
        }
        case proto::EpochCommittedTxnEndFlag : {
            /// do nothing
            break;
        }
        case proto::TxnType::AbortSet : {
            UpdateEpochAbortSet();
            abort_set_received_num.IncCount(message_epoch,message_server_id, 1);
            EpochMessageSendHandler::SendTxnToServer(message_epoch, message_server_id, empty_txn_ptr, proto::TxnType::AbortSetACK);
            break;
        }
        case proto::TxnType::InsertSet : {
            insert_set_received_num.IncCount(message_epoch, message_server_id, 1);
            EpochMessageSendHandler::SendTxnToServer(message_epoch, message_server_id, empty_txn_ptr, proto::TxnType::InsertSetACK);
            break;
        }
        case proto::TxnType::EpochShardACK : {
            shard_received_ack_num.IncCount(message_epoch, message_server_id, 1);
            break;
        }
        case proto::EpochRemoteServerACK : {
            remote_server_received_ack_num.IncCount(message_epoch, message_server_id, 1);
            break;
        }
        case proto::TxnType::BackUpACK : {
            backup_received_ack_num.IncCount(message_epoch, message_server_id, 1);
            break;
        }
        case proto::TxnType::AbortSetACK : {
            abort_set_received_ack_num.IncCount(message_epoch, message_server_id, 1);
            break;
        }
        case proto::TxnType::InsertSetACK : {
            insert_set_received_ack_num.IncCount(message_epoch, message_server_id, 1);
            break;
        }
        case proto::TxnType::EpochLogPushDownComplete : {
            redo_log_push_down_ack_num.IncCount(message_epoch, message_server_id, 1);
            break;
        }
        case proto::TxnType::ViewChange : {
            message_epoch = txn_ptr->commit_epoch();
            message_epoch_mod = txn_ptr->commit_epoch() % TaasContext::kCacheMaxLength;
            for(int i = 0; i < txn_ptr->row_size(); i ++) {
                TransactionCache::read_version_map.insert(txn_ptr->row(i).key(), txn_ptr->row(i).data());
            }
            break;
        }
        case proto::TxnType::MetaInfo : {
            UpdateMetaInfo();
            meta_info_received_num.IncCount(message_epoch,message_server_id, 1);
            //          EpochMessageSendHandler::SendTxnToServer(message_epoch, message_server_id, empty_txn_ptr, proto::TxnType::AbortSetACK);
            break;
        }

        case proto::NullMark:
        case proto::TxnType_INT_MIN_SENTINEL_DO_NOT_USE_:
        case proto::TxnType_INT_MAX_SENTINEL_DO_NOT_USE_:
        case proto::CommittedTxn:
        case proto::Lock_ok:
        case proto::Lock_abort:
        case proto::Prepare_req:
        case proto::Prepare_ok:
        case proto::Prepare_abort:
        case proto::Commit_req:
        case proto::Commit_ok:
        case proto::Commit_abort:
        case proto::Abort_txn:
            break;
    }
    return true;
}

bool EpochMessageReceiveHandler::UpdateEpochAbortSet() {
    message_epoch = txn_ptr->commit_epoch();
    message_epoch_mod = txn_ptr->commit_epoch() % TaasContext::kCacheMaxLength;
    for(int i = 0; i < txn_ptr->row_size(); i ++) {
        TransactionCache::epoch_abort_txn_set[message_epoch_mod]->insert(txn_ptr->row(i).key(), txn_ptr->row(i).data());
    }
    return true;
}

bool EpochMessageReceiveHandler::UpdateMetaInfo() {
    message_epoch = txn_ptr->commit_epoch();
    message_epoch_mod = txn_ptr->commit_epoch() % TaasContext::kCacheMaxLength;
    for(int i = 0; i < txn_ptr->row_size(); i ++) {
        TransactionCache::read_version_map.insert(txn_ptr->row(i).key(), txn_ptr->row(i).data());
    }
    return true;
}






uint64_t EpochMessageReceiveHandler::getMultiModelTxnId() {
    for(auto i = 0; i < txn_ptr->row_size(); i ++) {
        const auto &row = txn_ptr->row(i);
        if (row.op_type() == proto::OpType::Read) {
            continue;
        }
        std::string tempData = txn_ptr->row(0).data();
        std::string tempKey = txn_ptr->row(0).key();
        uint64_t index;
        if (!tempData.empty()) {
            index = tempData.find("tid:");
            if (index < tempData.length()) {
                auto tid = std::strtoull(&tempData.at(index), nullptr, 10);
                return tid;
            }
        }
    }
    return 0;
}

void EpochMessageReceiveHandler::HandleMultiModelClientSubTxn(const uint64_t& txn_id) {
    shard_should_handle_local_txn_num_local->IncCount(message_epoch, local_server_id, 1);
    txn_ptr->set_commit_epoch(message_epoch);
    txn_ptr->set_csn(txn_id);
    txn_ptr->set_txn_server_id(local_server_id);
    SetMessageRelatedCountersInfo();
    ReadValidateQueueEnqueue(message_epoch, txn_ptr);
    CommitQueueEnqueue(message_epoch, txn_ptr);
    shard_handled_local_txn_num_local->IncCount(message_epoch, local_server_id, 1);
}

bool EpochMessageReceiveHandler::HandleMultiModelClientTxn() {
    std::shared_ptr<MultiModelTxn> multiModelTxn;
    uint64_t txn_id;
    if(txn_ptr->storage_type() == "mot" || txn_ptr->storage_type() == "nebula") {
        txn_id = getMultiModelTxnId();
    }
    else {
        txn_id = txn_ptr->client_txn_id();
    }
    TransactionCache::MultiModelTxnMap.getValue(std::to_string(txn_id), multiModelTxn);
    if(txn_ptr->storage_type() == "kv") {
        multiModelTxn->total_txn_num = txn_ptr->csn(); // total sub txn num
    }
    multiModelTxn->received_txn_num += 1;
    if(multiModelTxn->total_txn_num == multiModelTxn->received_txn_num) {
        message_epoch = EpochManager::GetPhysicalEpoch();
        shard_should_handle_local_txn_num_local->IncCount(message_epoch, local_server_id, 1);
        txn_ptr = multiModelTxn->kv;
        HandleMultiModelClientSubTxn(txn_id);
        if(multiModelTxn->sql != nullptr) {
            txn_ptr = multiModelTxn->sql;
            HandleMultiModelClientSubTxn(txn_id);
        }
        if(multiModelTxn->gql != nullptr) {
            txn_ptr = multiModelTxn->gql;
            HandleMultiModelClientSubTxn(txn_id);
        }
        shard_handled_local_txn_num_local->IncCount(message_epoch, local_server_id, 1);
    }
    return true;
}







class EpochMessageSendHandler {
public:
    static std::atomic<uint64_t> TotalLatency, TotalTxnNum, TotalSuccessTxnNUm, TotalSuccessLatency;
    static bool SendTxnCommitResultToClient(const std::shared_ptr<proto::Transaction>& txn_ptr, proto::TxnState txn_state);
    static bool SendTxnToServer(uint64_t& epoch, uint64_t& to_whom, const std::shared_ptr<proto::Transaction>& txn_ptr, proto::TxnType txn_type);
    static bool SendRemoteServerTxn(uint64_t& epoch, uint64_t& to_whom, const std::shared_ptr<proto::Transaction>& txn_ptr, proto::TxnType txn_type);
    static bool SendBackUpTxn(uint64_t& epoch, const std::shared_ptr<proto::Transaction>& txn_ptr, proto::TxnType txn_type);
    static bool SendACK(uint64_t &epoch, uint64_t &to_whom, proto::TxnType txn_type);
    static bool SendMessageToAll(uint64_t& epoch, proto::TxnType txn_type);

    ///一下函数都由single one线程执行
    static void StaticInit();
    static void StaticClear();
    static std::vector<std::unique_ptr<std::atomic<uint64_t>>> shard_send_epoch, backup_send_epoch, abort_set_send_epoch, insert_set_send_epoch;
    //        static uint64_t shard_sent_epoch, backup_sent_epoch, abort_sent_epoch, insert_set_sent_epoch, abort_set_sent_epoch;

    static void CheckAndSendEpochMessage();
    static void CheckAndSendEpochShardEndMessage();
    static bool SendEpochShardEndMessage(const uint64_t &txn_node_ip_index, const uint64_t &epoch, const uint64_t &kTxnNodeNum);
    static void CheckAndSendEpochRemoteServerEndMessage();
    static bool SendEpochRemoteServerEndMessage(const uint64_t &txn_node_ip_index, const uint64_t &epoch, const uint64_t &kTxnNodeNum);
    static void CheckAndSendAbortSet();
    static bool SendAbortSet(const uint64_t &txn_node_ip_index, const uint64_t &epoch);

private:
    bool sleep_flag = false;
    std::unique_ptr<pack_params> pack_param;
    bool SendMetaInfo(const uint64_t& txn_node_ip_index, const uint64_t& epoch);
};

std::atomic<uint64_t> EpochMessageSendHandler::TotalLatency(0), EpochMessageSendHandler::TotalTxnNum(0),
    EpochMessageSendHandler::TotalSuccessTxnNUm(0), EpochMessageSendHandler::TotalSuccessLatency(0);
std::vector<std::unique_ptr<std::atomic<uint64_t>>> EpochMessageSendHandler::shard_send_epoch,
    EpochMessageSendHandler::backup_send_epoch,
    EpochMessageSendHandler::abort_set_send_epoch,
    EpochMessageSendHandler::insert_set_send_epoch;

//    uint64_t EpochMessageSendHandler::shard_sent_epoch = 1, EpochMessageSendHandler::backup_sent_epoch = 1,
//            EpochMessageSendHandler::abort_sent_epoch = 1,
//            EpochMessageSendHandler::insert_set_sent_epoch = 1, EpochMessageSendHandler::abort_set_sent_epoch = 1;




void EpochMessageSendHandler::StaticInit() {
    shard_send_epoch.resize(TaasContext::kTxnNodeNum);
    backup_send_epoch.resize(TaasContext::kTxnNodeNum);
    abort_set_send_epoch.resize(TaasContext::kTxnNodeNum);
    insert_set_send_epoch.resize(TaasContext::kTxnNodeNum);
    for(uint64_t i = 0; i < TaasContext::kTxnNodeNum; i ++) {
        backup_send_epoch [i] = std::make_unique<std::atomic<uint64_t>>(1);
        abort_set_send_epoch [i] = std::make_unique<std::atomic<uint64_t>>(1);
        shard_send_epoch[i] = std::make_unique<std::atomic<uint64_t>>(1);
        insert_set_send_epoch[i] = std::make_unique<std::atomic<uint64_t>>(1);
    }

}

void EpochMessageSendHandler::StaticClear() {
}

/**
 * @brief 将txn设置事务状态，并通过protobuf将Reply序列化，将序列化的结果放到send_to_client_queue中，等待发送给客户端
 *
 * @param ctx XML文件的配置信息
 * @param txn 等待回复给client的事务
 * @param txn_state 告诉client此txn的状态(Success or Abort)
 */
bool EpochMessageSendHandler::SendTxnCommitResultToClient(const std::shared_ptr<proto::Transaction>& txn_ptr, proto::TxnState txn_state) {
    if(txn_ptr->txn_server_id() != TaasContext::txn_node_ip_index) return true;
    txn_ptr->set_txn_state(txn_state);
    auto msg = std::make_unique<proto::Message>();
    auto rep = msg->mutable_reply_txn_result_to_client();
    rep->set_txn_state(txn_state);
    rep->set_client_txn_id(txn_ptr->client_txn_id());
    auto serialized_txn_str_ptr = std::make_unique<std::string>();
    Gzip(msg.get(), serialized_txn_str_ptr.get());
    auto tim = now_to_us() - txn_ptr->csn();
    TotalLatency.fetch_add(tim);
    TotalTxnNum.fetch_add(1);
    if(txn_state == proto::TxnState::Commit) {
        TotalSuccessLatency.fetch_add(tim);
        TotalSuccessTxnNUm.fetch_add(1);
    }
    MessageQueue::send_to_client_queue->enqueue(
        std::make_unique<send_params>(txn_ptr->client_txn_id(), txn_ptr->csn(), txn_ptr->client_ip(), txn_ptr->commit_epoch(), proto::TxnType::CommittedTxn, std::move(serialized_txn_str_ptr), nullptr));
    return MessageQueue::send_to_client_queue->enqueue(
        std::make_unique<send_params>(0, 0, "", 0, proto::TxnType::NullMark, nullptr, nullptr, false));
}

bool EpochMessageSendHandler::SendTxnToServer(uint64_t &epoch, uint64_t &to_whom, const std::shared_ptr<proto::Transaction>& txn_ptr, proto::TxnType txn_type) {
    if(TaasContext::kTxnNodeNum > 1) {
        auto pack_param = std::make_unique<pack_params>(to_whom, 0, "", epoch, txn_type, nullptr);
        switch (txn_type) {
            case proto::TxnType::ShardedClientTxn :
            case proto::TxnType::RemoteServerTxn : {
                return SendRemoteServerTxn(epoch, to_whom, txn_ptr, txn_type);
            }
            case proto::TxnType::BackUpTxn : {
                return EpochMessageSendHandler::SendBackUpTxn(epoch, txn_ptr, txn_type);
            }
            case proto::TxnType::BackUpACK :
            case proto::TxnType::AbortSetACK :
            case proto::TxnType::InsertSetACK :
            case proto::TxnType::EpochShardACK :
            case proto::EpochRemoteServerACK : {
                return SendACK(epoch, to_whom, txn_type);
            }
            case proto::TxnType::EpochLogPushDownComplete : {
                return SendMessageToAll(epoch, txn_type);
            }
            case proto::NullMark:
            case proto::TxnType_INT_MIN_SENTINEL_DO_NOT_USE_:
            case proto::TxnType_INT_MAX_SENTINEL_DO_NOT_USE_:
            case proto::ClientTxn:
            case proto::EpochShardEndFlag:
            case proto::EpochRemoteServerEndFlag:
            case proto::EpochBackUpEndFlag:
            case proto::EpochCommittedTxnEndFlag:
            case proto::CommittedTxn:
            case proto::AbortSet:
            case proto::InsertSet:
            case proto::Lock_ok:
            case proto::Lock_abort:
            case proto::Prepare_req:
            case proto::Prepare_ok:
            case proto::Prepare_abort:
            case proto::Commit_req:
            case proto::Commit_ok:
            case proto::Commit_abort:
            case proto::Abort_txn:
                break;

        }
    }
    return true;
}

bool EpochMessageSendHandler::SendRemoteServerTxn(uint64_t& epoch, uint64_t& to_whom, const std::shared_ptr<proto::Transaction>& txn_ptr, proto::TxnType txn_type) {
    auto msg = std::make_unique<proto::Message>();
    auto* txn_temp = msg->mutable_txn();
    *(txn_temp) = *txn_ptr;
    txn_temp->set_txn_type(txn_type);
    auto serialized_txn_str_ptr = std::make_unique<std::string>();
    Gzip(msg.get(), serialized_txn_str_ptr.get());
    assert(!serialized_txn_str_ptr->empty());
    if (txn_type == proto::TxnType::ShardedClientTxn) {
        assert(to_whom != TaasContext::txn_node_ip_index);
        MessageQueue::send_to_server_queue->enqueue(
            std::make_unique<send_params>(to_whom, 0, "", epoch,
                txn_type, std::move(serialized_txn_str_ptr), nullptr));
        return MessageQueue::send_to_server_queue->enqueue(
            std::make_unique<send_params>(0, 0, "", 0, proto::TxnType::NullMark,
                nullptr, nullptr, false));
    } else {///RemoteServerTxn
        MessageQueue::send_to_server_pub_queue->enqueue(
            std::make_unique<send_params>(to_whom, 0, "", epoch, txn_type,
                std::move(serialized_txn_str_ptr), nullptr));
        return MessageQueue::send_to_server_pub_queue->enqueue(
            std::make_unique<send_params>(0, 0, "", 0, proto::TxnType::NullMark,
                nullptr, nullptr, false));
    }
}

bool EpochMessageSendHandler::SendBackUpTxn(uint64_t& epoch, const std::shared_ptr<proto::Transaction>& txn_ptr, proto::TxnType txn_type) {
    auto msg = std::make_unique<proto::Message>();
    auto* txn_temp = msg->mutable_txn();
    *(txn_temp) = *txn_ptr;
    txn_temp->set_txn_type(txn_type);
    txn_temp->set_message_server_id(TaasContext::txn_node_ip_index);
    auto serialized_txn_str_ptr = std::make_unique<std::string>();
    Gzip(msg.get(), serialized_txn_str_ptr.get());
    assert(!serialized_txn_str_ptr->empty());
    MessageQueue::send_to_server_pub_queue->enqueue(
        std::make_unique<send_params>(0, 0, "", epoch, txn_type,
            std::move(serialized_txn_str_ptr), nullptr, false));
    return MessageQueue::send_to_server_pub_queue->enqueue(
        std::make_unique<send_params>(0, 0, "", 0, proto::TxnType::NullMark,
            nullptr, nullptr, false));
}

bool EpochMessageSendHandler::SendACK(uint64_t &epoch, uint64_t &to_whom, proto::TxnType txn_type) {
    if(to_whom == TaasContext::txn_node_ip_index) return true;
    auto msg = std::make_unique<proto::Message>();
    auto* txn_end = msg->mutable_txn();
    txn_end->set_txn_server_id(TaasContext::txn_node_ip_index);
    txn_end->set_txn_type(txn_type);
    txn_end->set_commit_epoch(epoch);
    txn_end->set_message_server_id(TaasContext::txn_node_ip_index);
    std::vector<std::string> keys, values;
    auto serialized_txn_str_ptr = std::make_unique<std::string>();
    Gzip(msg.get(), serialized_txn_str_ptr.get());
    assert(to_whom != TaasContext::txn_node_ip_index);
    MessageQueue::send_to_server_queue->enqueue(
        std::make_unique<send_params>(to_whom, 0, "", epoch, txn_type,
            std::move(serialized_txn_str_ptr),nullptr, false));
    return MessageQueue::send_to_server_queue->enqueue(
        std::make_unique<send_params>(0, 0, "", 0, proto::TxnType::NullMark,
            nullptr, nullptr, false));
}

bool EpochMessageSendHandler::SendMessageToAll(uint64_t& epoch, proto::TxnType txn_type) {
    auto msg = std::make_unique<proto::Message>();
    auto* txn_end = msg->mutable_txn();
    txn_end->set_txn_server_id(TaasContext::txn_node_ip_index);
    txn_end->set_txn_type(txn_type);
    txn_end->set_commit_epoch(epoch);
    txn_end->set_shard_id(0);
    txn_end->set_message_server_id(TaasContext::txn_node_ip_index);
    auto serialized_txn_str_ptr = std::make_unique<std::string>();
    Gzip(msg.get(), serialized_txn_str_ptr.get());
    MessageQueue::send_to_server_pub_queue->enqueue(
        std::make_unique<send_params>(0, 0, "", epoch, txn_type,
            std::move(serialized_txn_str_ptr),nullptr, true));
    return MessageQueue::send_to_server_pub_queue->enqueue(
        std::make_unique<send_params>(0, 0, "", 0, proto::TxnType::NullMark,
            nullptr, nullptr, false));
}

void EpochMessageSendHandler::CheckAndSendEpochShardEndMessage() {
}

bool EpochMessageSendHandler::SendEpochShardEndMessage(const uint64_t &txn_node_ip_index, const uint64_t &epoch, const uint64_t &kTxnNodeNum) {
    for(uint64_t server_id = 0; server_id < kTxnNodeNum; server_id ++) {
        if (server_id == txn_node_ip_index) continue;
        auto msg = std::make_unique<proto::Message>();
        auto *txn_end = msg->mutable_txn();
        txn_end->set_txn_server_id(txn_node_ip_index);
        txn_end->set_txn_type(proto::TxnType::EpochShardEndFlag);
        txn_end->set_commit_epoch(epoch);
        txn_end->set_message_server_id(TaasContext::txn_node_ip_index);
        txn_end->set_csn(EpochMessageReceiveHandler::GetAllThreadLocalCountNum(epoch, server_id,
            EpochMessageReceiveHandler::shard_should_send_txn_num_local_vec)); /// 不同server由不同的数量
        auto serialized_txn_str_ptr = std::make_unique<std::string>();
        Gzip(msg.get(), serialized_txn_str_ptr.get());
        assert(server_id != TaasContext::txn_node_ip_index);
        MessageQueue::send_to_server_queue->enqueue(
            std::make_unique<send_params>(server_id, 0, "", epoch,proto::TxnType::EpochShardEndFlag,
                std::move(serialized_txn_str_ptr),nullptr, false));
        MessageQueue::send_to_server_queue->enqueue(
            std::make_unique<send_params>(0, 0, "", 0, proto::TxnType::NullMark,
                nullptr, nullptr, false));
    }
    {
        auto msg = std::make_unique<proto::Message>();
        auto* txn_end = msg->mutable_txn();
        txn_end->set_txn_server_id(txn_node_ip_index);
        txn_end->set_txn_type(proto::TxnType::EpochBackUpEndFlag);
        txn_end->set_commit_epoch(epoch);
        txn_end->set_message_server_id(TaasContext::txn_node_ip_index);
        txn_end->set_csn(static_cast<uint64_t>(EpochMessageReceiveHandler::GetAllThreadLocalCountNum(epoch, EpochMessageReceiveHandler::backup_should_send_txn_num_local_vec)));
        auto serialized_txn_str_ptr = std::make_unique<std::string>();
        Gzip(msg.get(), serialized_txn_str_ptr.get());

        auto msg_0 = std::make_unique<proto::Message>();
        auto* txn_end_0 = msg_0->mutable_txn();
        txn_end_0->set_txn_server_id(txn_node_ip_index);
        txn_end_0->set_txn_type(proto::TxnType::EpochBackUpEndFlag);
        txn_end_0->set_commit_epoch(epoch);
        txn_end_0->set_shard_id(0);
        txn_end_0->set_message_server_id(TaasContext::txn_node_ip_index);
        txn_end_0->set_csn(0);
        auto serialized_txn_str_ptr_0 = std::make_unique<std::string>();
        Gzip(msg_0.get(), serialized_txn_str_ptr_0.get());

        uint64_t to_id;
        for(uint64_t i = 0; i < kTxnNodeNum; i ++) {
            to_id = (TaasContext::txn_node_ip_index + i + 1) % TaasContext::kTxnNodeNum;
            if(to_id == (uint64_t)TaasContext::txn_node_ip_index || EpochManager::server_state.GetCount(epoch, to_id) == 0) continue;
            if(i < TaasContext::kBackUpNum) {
                auto s = std::make_unique<std::string>(*serialized_txn_str_ptr);
                assert(to_id != TaasContext::txn_node_ip_index);
                MessageQueue::send_to_server_queue->enqueue(std::make_unique<send_params>(to_id, 0, "", epoch,proto::TxnType::EpochBackUpEndFlag, std::move(s),nullptr));
            }
            else {
                auto s = std::make_unique<std::string>(*serialized_txn_str_ptr_0);
                assert(to_id != TaasContext::txn_node_ip_index);
                MessageQueue::send_to_server_queue->enqueue(std::make_unique<send_params>(to_id, 0, "", epoch,proto::TxnType::EpochBackUpEndFlag, std::move(s),nullptr));
            }
            MessageQueue::send_to_server_queue->enqueue(
                std::make_unique<send_params>(0, 0, "", 0, proto::TxnType::NullMark, nullptr, nullptr, false));
        }
    }
    return true;
}

void EpochMessageSendHandler::CheckAndSendEpochRemoteServerEndMessage() {
}

bool EpochMessageSendHandler::SendEpochRemoteServerEndMessage(const uint64_t &txn_node_ip_index, const uint64_t &epoch, const uint64_t &kTxnNodeNum) {
    for(uint64_t server_id = 0; server_id < kTxnNodeNum; server_id ++) {
        if (server_id == txn_node_ip_index) continue;
        auto msg = std::make_unique<proto::Message>();
        auto *txn_end = msg->mutable_txn();
        txn_end->set_txn_server_id(txn_node_ip_index);
        txn_end->set_txn_type(proto::TxnType::EpochRemoteServerEndFlag);
        txn_end->set_commit_epoch(epoch);
        txn_end->set_message_server_id(TaasContext::txn_node_ip_index);
        txn_end->set_csn(EpochMessageReceiveHandler::GetAllThreadLocalCountNum(epoch, server_id,
            EpochMessageReceiveHandler::remote_server_should_send_txn_num_local_vec));
        auto serialized_txn_str_ptr = std::make_unique<std::string>();
        Gzip(msg.get(), serialized_txn_str_ptr.get());
        assert(server_id != TaasContext::txn_node_ip_index);
        MessageQueue::send_to_server_queue->enqueue(
            std::make_unique<send_params>(server_id, 0, "", epoch,proto::TxnType::EpochRemoteServerEndFlag, std::move(serialized_txn_str_ptr),nullptr));
        MessageQueue::send_to_server_queue->enqueue(
            std::make_unique<send_params>(0, 0, "", 0, proto::TxnType::NullMark, nullptr, nullptr, false));
    }
    return true;
}

void EpochMessageSendHandler::CheckAndSendAbortSet() {
}
bool EpochMessageSendHandler::SendAbortSet(const uint64_t &txn_node_ip_index, const uint64_t &epoch) {
    auto msg = std::make_unique<proto::Message>();
    auto *txn_end = msg->mutable_txn();
    txn_end->set_txn_server_id(txn_node_ip_index);
    txn_end->set_message_server_id(TaasContext::txn_node_ip_index);
    txn_end->set_txn_type(proto::TxnType::AbortSet);
    txn_end->set_commit_epoch(epoch);
    txn_end->set_shard_id(0);
    std::vector<std::string> keys, values;
    TransactionCache::local_epoch_abort_txn_set[epoch % TaasContext::kCacheMaxLength]->getValue(keys, values);
    for (uint64_t i = 0; i < keys.size(); i++) {
        auto row = txn_end->add_row();
        row->set_key(keys[i]);
        row->set_data(values[i]);
    }
    auto serialized_txn_str_ptr = std::make_unique<std::string>();
    Gzip(msg.get(), serialized_txn_str_ptr.get());
    MessageQueue::send_to_server_pub_queue->enqueue(
        std::make_unique<send_params>(0, 0, "", epoch, proto::TxnType::AbortSet,
            std::move(serialized_txn_str_ptr), nullptr, true));
    return MessageQueue::send_to_server_pub_queue->enqueue(
        std::make_unique<send_params>(0, 0, "", 0, proto::TxnType::NullMark,
            nullptr, nullptr, false));
}

bool EpochMessageSendHandler::SendMetaInfo(const uint64_t &txn_node_ip_index, const uint64_t &epoch) {
    auto msg = std::make_unique<proto::Message>();
    auto *txn_end = msg->mutable_txn();
    txn_end->set_txn_server_id(txn_node_ip_index);
    txn_end->set_message_server_id(TaasContext::txn_node_ip_index);
    txn_end->set_txn_type(proto::TxnType::MetaInfo);
    txn_end->set_commit_epoch(epoch);
    txn_end->set_shard_id(0);
    std::vector<std::string> keys, values;
    TransactionCache::read_version_map.getValue(keys, values);
    for (uint64_t i = 0; i < keys.size(); i++) {
        auto row = txn_end->add_row();
        row->set_key(keys[i]);
        row->set_data(values[i]);
    }
    auto serialized_txn_str_ptr = std::make_unique<std::string>();
    Gzip(msg.get(), serialized_txn_str_ptr.get());
    MessageQueue::send_to_server_pub_queue->enqueue(
        std::make_unique<send_params>(0, 0, "", epoch, proto::TxnType::MetaInfo,
            std::move(serialized_txn_str_ptr), nullptr, true));
    return MessageQueue::send_to_server_pub_queue->enqueue(
        std::make_unique<send_params>(0, 0, "", 0, proto::TxnType::NullMark,
            nullptr, nullptr, false));
}



///MessageSend&Receive
void ListenClientThreadMain() {///监听client 写集
    // 设置ZeroMQ的相关变量，并监听5555端口，接受client发来的写集
    int queue_length = 1000000000;
    zmq::context_t listen_context(1);
    zmq::socket_t socket_listen(listen_context, ZMQ_PULL);
    zmq::recv_flags recvFlags = zmq::recv_flags::none;
    zmq::recv_result_t recvResult;
    socket_listen.set(zmq::sockopt::sndhwm, queue_length);
    socket_listen.set(zmq::sockopt::rcvhwm, queue_length);
    socket_listen.bind("tcp://*:5551");
    bool res;
    printf("线程开始工作 ListenClientThread ZMQ_PULL tcp://*:5551\n");
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    while (!EpochManager::IsTimerStop()) {
        std::unique_ptr<zmq::message_t> message_ptr = std::make_unique<zmq::message_t>();
        recvResult = socket_listen.recv((*message_ptr), recvFlags);//防止上次遗留消息造成message cache出现问题
        assert(recvResult != -1);
        if (is_epoch_advance_started.load()) {
            MessageQueue::client_receive_message_num.fetch_add(1);
            res = MessageQueue::listen_message_txn_queue->enqueue(std::move(message_ptr));
            assert(res);
            res = MessageQueue::listen_message_txn_queue->enqueue(nullptr);
            assert(res); //防止moodycamel取不出
            break;
        }
    }

    while (!EpochManager::IsTimerStop()) {
        std::unique_ptr<zmq::message_t> message_ptr = std::make_unique<zmq::message_t>();
        recvResult = socket_listen.recv((*message_ptr), recvFlags);
        assert(recvResult != -1);
        MessageQueue::client_receive_message_num.fetch_add(1);
        res = MessageQueue::listen_message_txn_queue->enqueue(std::move(message_ptr));
        //            printf("线程开始工作 ListenClientThread receive a message\n");
        assert(res);
        res = MessageQueue::listen_message_txn_queue->enqueue(nullptr);
        assert(res); //防止moodycamel取不出
    }
}

/**
 * @brief 将send_to_client_queue中的Reply消息发送给client
 *
 * @param id
 * @param ctx
 */
void SendClientThreadMain() {
    // 设置ZeroMQ的相关变量，通过5556端口发送Reply给client
    zmq::context_t context(1);
    zmq::send_flags sendFlags = zmq::send_flags::none;
    zmq::send_result_t sendResult;
    int queue_length = 1000000000;
    std::unique_ptr<send_params> params;
    std::unique_ptr<zmq::message_t> msg;
    printf("线程开始工作 SendClientThread ZMQ_PUSH tcp://ip+:5552 \n");
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    std::unordered_map<std::string, std::unique_ptr<zmq::socket_t>> socket_map;
    if (TaasContext::kTestClientNum > 0) {
        while (!EpochManager::IsTimerStop()) {
            MessageQueue::send_to_client_queue->wait_dequeue(params);
        }
    } else {
        //         使用ZeroMQ发送Reply给client
        while(!EpochManager::IsTimerStop()) {
            MessageQueue::send_to_client_queue->wait_dequeue(params);
            if (params == nullptr || params->type == proto::TxnType::NullMark) continue;
            MessageQueue::client_send_message_num.fetch_add(1);
            msg = std::make_unique<zmq::message_t>(*(params->str));
            auto key = "tcp://" + params->ip;
            if (socket_map.find(key) != socket_map.end()) {
                //                    printf("send to client %s\n", key.c_str());
                socket_map[key]->send(*(msg), sendFlags);
            } else {
                auto socket = std::make_unique<zmq::socket_t>(context, ZMQ_PUSH);
                socket->set(zmq::sockopt::sndhwm, queue_length);
                socket->set(zmq::sockopt::rcvhwm, queue_length);
                socket->connect("tcp://" + params->ip + ":5552");
                //                    printf("send to client %s\n", key.c_str());
                socket_map[key] = std::move(socket);
                socket_map[key]->send(*(msg), sendFlags);
            }
        }
    }
}

void SendServerThreadMain() {
    auto server_num = TaasContext::kTxnNodeNum,
         shard_num = TaasContext::kShardNum,
         replica_num = TaasContext::kReplicaNum,
         local_server_id = TaasContext::txn_node_ip_index,
         max_length = TaasContext::kCacheMaxLength;
    zmq::context_t context(1);
    zmq::message_t reply(5);
    zmq::send_flags sendFlags = zmq::send_flags::none;
    int queue_length = 1000000000;
    std::unordered_map<std::uint64_t, std::unique_ptr<zmq::socket_t>> socket_map;
    std::unique_ptr<send_params> params;
    std::unique_ptr<zmq::message_t> msg;
    assert(TaasContext::kServerIp.size() >= TaasContext::kTxnNodeNum);
    for (uint64_t i = 0; i < server_num; i++) {
        if(i == local_server_id) continue;
        auto socket = std::make_unique<zmq::socket_t>(context, ZMQ_PUSH);
        socket->set(zmq::sockopt::sndhwm, queue_length);
        socket->set(zmq::sockopt::rcvhwm, queue_length);
        socket->connect("tcp://" + TaasContext::kServerIp[i] + ":" + std::to_string(20000+i));
        socket_map[i] = std::move(socket);
        printf("Send Server connect ZMQ_PUSH %s", ("tcp://" + TaasContext::kServerIp[i] + ":" + std::to_string(20000+i) + "\n").c_str());
    }
    printf("线程开始工作 SendServerThread\n");


    init_ok_num.fetch_add(1);
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    while (!EpochManager::IsTimerStop()) {
        MessageQueue::send_to_server_queue->wait_dequeue(params);
        if (params == nullptr || params->type == proto::TxnType::NullMark || params->str == nullptr) continue;
        assert(params->id != TaasContext::txn_node_ip_index);
        assert(params->id < TaasContext::kTxnNodeNum);
        msg = std::make_unique<zmq::message_t>(*(params->str));
        socket_map[params->id]->send(*msg, sendFlags);
    }
    socket_map[0]->send((zmq::message_t &) "end", sendFlags);
}

void SendServerPUBThreadMain() {

    auto server_num = TaasContext::kTxnNodeNum,
         shard_num = TaasContext::kShardNum,
         replica_num = TaasContext::kReplicaNum,
         local_server_id = TaasContext::txn_node_ip_index,
         max_length = TaasContext::kCacheMaxLength;

    zmq::context_t context(1);
    zmq::message_t reply(5);
    zmq::send_flags sendFlags = zmq::send_flags::none;

    int queue_length = 1000000000;
    std::unordered_map<std::uint64_t, std::unique_ptr<zmq::socket_t>> socket_map;
    std::unique_ptr<send_params> params;
    std::unique_ptr<zmq::message_t> msg;
    assert(TaasContext::kServerIp.size() >= TaasContext::kTxnNodeNum);
    for (uint64_t i = 0; i < shard_num; i++) {
        auto socket = std::make_unique<zmq::socket_t>(context, ZMQ_PUB);
        socket->set(zmq::sockopt::sndhwm, queue_length);
        socket->set(zmq::sockopt::rcvhwm, queue_length);
        socket->bind("tcp://*:" + std::to_string(21000 + i));
        socket_map[i] = std::move(socket);
        printf("Send Server connect ZMQ_PUB %s", (std::to_string(21000+i) + "\n").c_str());///Shard Replica
    }

    std::unique_ptr<zmq::socket_t> socket_to_all;
    socket_to_all= std::make_unique<zmq::socket_t>(context, ZMQ_PUB);
    socket_to_all->set(zmq::sockopt::sndhwm, queue_length);
    socket_to_all->set(zmq::sockopt::rcvhwm, queue_length);
    socket_to_all->bind("tcp://*:" + std::to_string(22000+TaasContext::txn_node_ip_index));
    printf("Send Server bind ZMQ_PUB %s", ("tcp://*:" + std::to_string(22000+TaasContext::txn_node_ip_index) + "\n").c_str());///ACK

    std::unique_ptr<zmq::socket_t> socket_back_up;
    socket_back_up= std::make_unique<zmq::socket_t>(context, ZMQ_PUB);
    socket_back_up->set(zmq::sockopt::sndhwm, queue_length);
    socket_back_up->set(zmq::sockopt::rcvhwm, queue_length);
    socket_back_up->bind("tcp://*:" + std::to_string(23000+TaasContext::txn_node_ip_index));
    printf("Send Server bind ZMQ_PUB %s", ("tcp://*:" + std::to_string(23000+TaasContext::txn_node_ip_index) + "\n").c_str());///BackUp

    printf("线程开始工作 SendServerThread\n");

    init_ok_num.fetch_add(1);
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    while (!EpochManager::IsTimerStop()) {
        MessageQueue::send_to_server_pub_queue->wait_dequeue(params);
        if (params == nullptr || params->type == proto::TxnType::NullMark || params->str == nullptr) continue;
        msg = std::make_unique<zmq::message_t>(*(params->str));
        if(params->type == proto::TxnType::BackUpTxn) {
            socket_back_up->send(*msg, sendFlags);
            continue;
        }
        if(params->send_to_all) {
            socket_to_all->send(*msg, sendFlags);
            continue;
        }
        socket_map[params->id]->send(*msg, sendFlags);
    }
    socket_map[0]->send((zmq::message_t &) "end", sendFlags);
}

/**
 * @brief 监听其他txn node发来的写集，并放在listen_message_queue中
 *
 * @param id 暂时未使用
 * @param ctx XML的配置信息
 */
void ListenServerThreadMain() {///监听远端txn node写集
    // 设置ZeroMQ的相关变量，监听其他txn node是否有写集发来
    zmq::context_t listen_context(1);
    zmq::recv_flags recvFlags = zmq::recv_flags::none;
    zmq::recv_result_t  recvResult;
    int queue_length = 1000000000;
    zmq::socket_t socket_listen(listen_context, ZMQ_PULL);
    socket_listen.bind("tcp://*:" + std::to_string(20000+TaasContext::txn_node_ip_index));//to server
    socket_listen.set(zmq::sockopt::sndhwm, queue_length);
    socket_listen.set(zmq::sockopt::rcvhwm, queue_length);
    printf("线程开始工作 ListenServerThread ZMQ_PULL tcp://*:%s\n", std::to_string(20000+TaasContext::txn_node_ip_index).c_str());

    init_ok_num.fetch_add(1);
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    while (!EpochManager::IsTimerStop()) {
        std::unique_ptr<zmq::message_t> message_ptr = std::make_unique<zmq::message_t>();
        recvResult = socket_listen.recv((*message_ptr), recvFlags);//防止上次遗留消息造成message cache出现问题
        assert(recvResult >= 0);
        if (is_epoch_advance_started.load()) {
            auto res = MessageQueue::listen_message_epoch_queue->enqueue(std::move(message_ptr));
            assert(res);
            res = MessageQueue::listen_message_epoch_queue->enqueue(nullptr);
            assert(res); //防止moodycamel取不出
            break;
        }
    }
    while (!EpochManager::IsTimerStop()) {
        std::unique_ptr<zmq::message_t> message_ptr = std::make_unique<zmq::message_t>();
        recvResult = socket_listen.recv((*message_ptr), recvFlags);
        assert(recvResult >= 0);
        auto res = MessageQueue::listen_message_epoch_queue->enqueue(std::move(message_ptr));
        assert(res);
        res = MessageQueue::listen_message_epoch_queue->enqueue(nullptr);
        assert(res); //防止moodycamel取不出
    }
}

void ListenServerThreadMain_Sub() {///监听远端txn node写集
    auto server_num = TaasContext::kTxnNodeNum,
         shard_num = TaasContext::kShardNum,
         replica_num = TaasContext::kReplicaNum,
         local_server_id = TaasContext::txn_node_ip_index,
         max_length = TaasContext::kCacheMaxLength;
    std::vector<std::vector<bool>> is_local_shard;
    is_local_shard.resize(server_num);
    for(auto &i : is_local_shard) {
        i.resize(shard_num);
    }

    zmq::context_t listen_context(1);
    zmq::recv_flags recvFlags = zmq::recv_flags::none;
    zmq::recv_result_t  recvResult;
    int queue_length = 1000000000;
    zmq::socket_t socket_listen(listen_context, ZMQ_SUB);

    for(uint64_t i = 0; i < shard_num; i ++) {
        if(i % server_num == local_server_id) {
            for(uint64_t j = 1; j < replica_num; j ++ ) {
                socket_listen.connect("tcp://" + TaasContext::kServerIp[(i + j) % server_num] + ":"
                                      + std::to_string(21000 + i));  /// shard replica
//                LOG(INFO) << "Server:" << (i + j) % server_num << "Shard: " << i;
            }
        }
        else {
            for(uint64_t j = 0; j < replica_num; j ++ ) {
                if((i + j) % server_num == local_server_id) {
                    for(uint64_t k = 0; k < replica_num; k ++) {
                        if((i + k) % server_num == local_server_id) continue;
                        socket_listen.connect("tcp://" + TaasContext::kServerIp[(i + k) % server_num] + ":"
                                              + std::to_string(21000 + i));  /// shard replica
//                        LOG(INFO) << "Server:" << (i + k) % server_num << "Shard: " << i;
                    }
                }
            }
        }
    }

    //        for (uint64_t i = 0; i < server_num; i++) {
    //            if (i == TaasContext::txn_node_ip_index) continue;
    //            for(uint64_t j = 0; j < shard_num; j++) {
    //                if(is_local_shard[i][j]) {///shard j send from i is a local shard of current server, then receive the replica
    //                    socket_listen.connect("tcp://" + TaasContext::kServerIp[i] + ":" + std::to_string(21000+j));///shard replica
    //                    printf("Listen Server connect ZMQ_SUB %s", ("tcp://" + TaasContext::kServerIp[i] + ":" + std::to_string(21000+j) + "\n").c_str());
    //                }
    //            }
    //        }

    for (uint64_t i = 0; i < server_num; i++) {
        if (i == TaasContext::txn_node_ip_index) continue;
        socket_listen.connect("tcp://" + TaasContext::kServerIp[i] + ":" + std::to_string(22000+i));///ACK to sall erver
        printf("Listen Server connect ZMQ_SUB %s", ("tcp://" + TaasContext::kServerIp[i] + ":" + std::to_string(22000+i) + "\n").c_str());
    }

    uint64_t to_id;
    for(uint64_t i = 0; i < TaasContext::kBackUpNum; i++) {
        to_id = (TaasContext::txn_node_ip_index + TaasContext::kTxnNodeNum - i - 1) % TaasContext::kTxnNodeNum;
        if(to_id == TaasContext::txn_node_ip_index) continue;
        socket_listen.connect("tcp://" + TaasContext::kServerIp[to_id] + ":" + std::to_string(23000+to_id));///BackUp
        printf("Listen Server connect ZMQ_SUB %s", ("tcp://" + TaasContext::kServerIp[i] + ":" + std::to_string(23000+to_id) + "\n").c_str());
    }

    socket_listen.set(zmq::sockopt::subscribe,"");
    socket_listen.set(zmq::sockopt::sndhwm, queue_length);
    socket_listen.set(zmq::sockopt::rcvhwm, queue_length);
    printf("线程开始工作 ListenServerThread ZMQ_SUB\n");

    init_ok_num.fetch_add(1);
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    while (!EpochManager::IsTimerStop()) {
        std::unique_ptr<zmq::message_t> message_ptr = std::make_unique<zmq::message_t>();
        recvResult = socket_listen.recv((*message_ptr), recvFlags);//防止上次遗留消息造成message cache出现问题
        //            LOG(INFO) << "receive a message";
        assert(recvResult >= 0);
        if (is_epoch_advance_started.load()) {
            auto res = MessageQueue::listen_message_epoch_queue->enqueue(std::move(message_ptr));
            assert(res);
            res = MessageQueue::listen_message_epoch_queue->enqueue(nullptr);
            assert(res); //防止moodycamel取不出
            break;
        }
    }

    while (!EpochManager::IsTimerStop()) {
        std::unique_ptr<zmq::message_t> message_ptr = std::make_unique<zmq::message_t>();
        recvResult = socket_listen.recv((*message_ptr), recvFlags);
        //            LOG(INFO) << "receive a message";
        assert(recvResult >= 0);
        auto res = MessageQueue::listen_message_epoch_queue->enqueue(std::move(message_ptr));
        assert(res);
        res = MessageQueue::listen_message_epoch_queue->enqueue(nullptr);
        assert(res); //防止moodycamel取不出
    }
}


void SendToMOTStorageThreadMain() { //PUB Txn
    int queue_length = 1000000000;
    zmq::context_t context(1);
    zmq::message_t reply(5);
    zmq::send_flags sendFlags = zmq::send_flags::none;
    zmq::socket_t socket_send(context, ZMQ_PUB);
    socket_send.set(zmq::sockopt::sndhwm, queue_length);
    socket_send.set(zmq::sockopt::rcvhwm, queue_length);
    socket_send.bind("tcp://*:5556");//to server
    printf("线程开始工作 SendStoragePUBServerThread ZMQ_PUB tcp:// ip + :5556\n");
    std::unique_ptr<send_params> params;
    std::unique_ptr<zmq::message_t> msg;
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    while (!EpochManager::IsTimerStop()) {
        //            if(MessageQueue::send_to_mot_storage_queue->try_dequeue(params)) {
        //                if (params == nullptr || params->type == proto::TxnType::NullMark) continue;
        //                msg = std::make_unique<zmq::message_t>(*(params->str));
        //                socket_send.send(*msg, sendFlags);
        //            }
        //            else {
        //                usleep(50);
        //            }
        MessageQueue::send_to_mot_storage_queue->wait_dequeue(params);
        if (params == nullptr || params->type == proto::TxnType::NullMark) continue;
        msg = std::make_unique<zmq::message_t>(*(params->str));
        socket_send.send(*msg, sendFlags);
        //            LOG(INFO) << "MOT PUB a txn";
    }
    socket_send.send((zmq::message_t &) "end", sendFlags);
}



class MOT {
public:
    static std::unique_ptr<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>> task_queue, redo_log_queue;
    static std::vector<std::unique_ptr<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>>
        epoch_redo_log_queue; ///store transactions receive from clients, wait to push down

    static std::atomic<uint64_t> pushed_down_epoch;
    static std::atomic<uint64_t> total_commit_txn_num, success_commit_txn_num, failed_commit_txn_num;
    static std::vector<std::unique_ptr<std::atomic<bool>>> epoch_redo_log_complete;

    static std::condition_variable commit_cv;

    static std::atomic<uint64_t> inc_id;
    uint64_t thread_id = 0, max_length = 0, sharding_num = 0, local_server_id;
    std::shared_ptr<AtomicCounters_Cache>
        epoch_should_push_down_txn_num_local,
        epoch_pushed_down_txn_num_local;

    static std::vector<std::shared_ptr<AtomicCounters_Cache>>
        epoch_should_push_down_txn_num_local_vec,
        epoch_pushed_down_txn_num_local_vec;

    void Init();
    static void StaticInit();
    static void StaticClear(const uint64_t &epoch);

    static void ClearAllThreadLocalCountNum(const uint64_t &epoch, const std::vector<std::shared_ptr<AtomicCounters_Cache>> &vec) ;
    static uint64_t GetAllThreadLocalCountNum(const uint64_t &epoch, const std::vector<std::shared_ptr<AtomicCounters_Cache>> &vec) ;
    static uint64_t GetAllThreadLocalCountNum(const uint64_t &epoch, const uint64_t &sharding_id, const std::vector<std::shared_ptr<AtomicCounters_Cache>> &vec);

    static bool CheckEpochPushDownComplete(const uint64_t &epoch);
    static void DBRedoLogQueueEnqueue(const uint64_t& thread_id, const uint64_t &epoch, std::shared_ptr<proto::Transaction> txn_ptr);
    static bool DBRedoLogQueueTryDequeue(const uint64_t &epoch, std::shared_ptr<proto::Transaction> txn_ptr);

    static bool GeneratePushDownTask(const uint64_t &epoch);
    void SendTransactionToDB_Usleep();
    void SendTransactionToDB_Block();
};

std::unique_ptr<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>  MOT::task_queue, MOT::redo_log_queue;
std::vector<std::unique_ptr<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>> MOT::epoch_redo_log_queue;
std::atomic<uint64_t> MOT::pushed_down_epoch(1);
std::atomic<uint64_t> MOT::total_commit_txn_num(0), MOT::success_commit_txn_num(0), MOT::failed_commit_txn_num(0);
std::vector<std::unique_ptr<std::atomic<bool>>> MOT::epoch_redo_log_complete;
std::condition_variable MOT::commit_cv;

std::atomic<uint64_t> MOT::inc_id;

std::vector<std::shared_ptr<AtomicCounters_Cache>>
    MOT::epoch_should_push_down_txn_num_local_vec,
    MOT::epoch_pushed_down_txn_num_local_vec;

void MOT::Init() {
    thread_id = inc_id.fetch_add(1);
    sharding_num = TaasContext::kTxnNodeNum;
    max_length = TaasContext::kCacheMaxLength;
    local_server_id = TaasContext::txn_node_ip_index;
    epoch_should_push_down_txn_num_local= std::make_shared<AtomicCounters_Cache>(max_length, sharding_num);
    epoch_pushed_down_txn_num_local= std::make_shared<AtomicCounters_Cache>(max_length, sharding_num);
    epoch_should_push_down_txn_num_local_vec[thread_id] = epoch_should_push_down_txn_num_local;
    epoch_pushed_down_txn_num_local_vec[thread_id] = epoch_pushed_down_txn_num_local;
}

void MOT::StaticInit() {
    task_queue = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
    redo_log_queue = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
    epoch_redo_log_complete.resize(TaasContext::kCacheMaxLength);
    epoch_redo_log_queue.resize(TaasContext::kCacheMaxLength);
    for(int i = 0; i < static_cast<int>(TaasContext::kCacheMaxLength); i ++) {
        epoch_redo_log_complete[i] = std::make_unique<std::atomic<bool>>(false);
        epoch_redo_log_queue[i] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
    }
    epoch_should_push_down_txn_num_local_vec.resize(StorageContext::kMOTThreadNum);
    epoch_pushed_down_txn_num_local_vec.resize(StorageContext::kMOTThreadNum);
}

void MOT::StaticClear(const uint64_t &epoch) {
    epoch_redo_log_complete[epoch % TaasContext::kCacheMaxLength]->store(false);
    //        epoch_redo_log_queue[epoch % TaasContext::kCacheMaxLength] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
    ClearAllThreadLocalCountNum(epoch, epoch_should_push_down_txn_num_local_vec);
    ClearAllThreadLocalCountNum(epoch, epoch_pushed_down_txn_num_local_vec);
}

void MOT::ClearAllThreadLocalCountNum(const uint64_t &epoch, const std::vector<std::shared_ptr<AtomicCounters_Cache>> &vec) {
    for(const auto& i : vec) {
        if(i != nullptr)
            i->Clear(epoch);
    }
}
uint64_t MOT::GetAllThreadLocalCountNum(const uint64_t &epoch, const std::vector<std::shared_ptr<AtomicCounters_Cache>> &vec) {
    uint64_t ans = 0;
    for(const auto& i : vec) {
        if(i != nullptr)
            ans += i->GetCount(epoch);
    }
    return ans;
}
uint64_t MOT::GetAllThreadLocalCountNum(const uint64_t &epoch, const uint64_t &sharding_id, const std::vector<std::shared_ptr<AtomicCounters_Cache>> &vec) {
    uint64_t ans = 0;
    for(const auto& i : vec) {
        if(i != nullptr)
            ans += i->GetCount(epoch, sharding_id);
    }
    return ans;
}

bool MOT::CheckEpochPushDownComplete(const uint64_t &epoch) {
    if(epoch_redo_log_complete[epoch % TaasContext::kCacheMaxLength]->load()) return true;
    //        if(epoch < EpochManager::GetLogicalEpoch() &&
    //           epoch_pushed_down_txn_num_local->GetCount(epoch) >= epoch_should_push_down_txn_num.GetCount(epoch)) {
    //            epoch_redo_log_complete[epoch % TaasContext::kCacheMaxLength]->store(true);
    //            return true;
    //        }
    if(epoch < EpochManager::GetLogicalEpoch() &&
        GetAllThreadLocalCountNum(epoch, epoch_pushed_down_txn_num_local_vec) >=
            GetAllThreadLocalCountNum(epoch, epoch_should_push_down_txn_num_local_vec)
    ) {
        epoch_redo_log_complete[epoch % TaasContext::kCacheMaxLength]->store(true);
        return true;
    }
    return false;
}
void MOT::DBRedoLogQueueEnqueue(const uint64_t& thread_id, const uint64_t &epoch, std::shared_ptr<proto::Transaction> txn_ptr) {
    epoch_should_push_down_txn_num_local_vec[thread_id % inc_id.load() ]->IncCount(epoch, txn_ptr->txn_server_id(), 1);
    auto epoch_mod = epoch % TaasContext::kCacheMaxLength;
    epoch_redo_log_queue[epoch_mod]->enqueue(txn_ptr);
    epoch_redo_log_queue[epoch_mod]->enqueue(nullptr);
    txn_ptr.reset();
}

bool MOT::DBRedoLogQueueTryDequeue(const uint64_t &epoch, std::shared_ptr<proto::Transaction> txn_ptr) {
    auto epoch_mod = epoch % TaasContext::kCacheMaxLength;
    return epoch_redo_log_queue[epoch_mod]->try_dequeue(txn_ptr);
}

bool MOT::GeneratePushDownTask(const uint64_t &epoch) {
    auto txn_ptr = std::make_shared<proto::Transaction>();
    txn_ptr->set_commit_epoch(epoch);
    task_queue->enqueue(txn_ptr);
    task_queue->enqueue(nullptr);
    return true;
}

void MOT::SendTransactionToDB_Usleep() {
    bool sleep_flag;
    std::shared_ptr<proto::Transaction> txn_ptr;
    uint64_t epoch, epoch_mod;
    proto::Transaction* ptr;
    epoch = EpochManager::GetPushDownEpoch();
    sleep_flag = true;
    while (!EpochManager::IsTimerStop()) {
        epoch = EpochManager::GetPushDownEpoch();
        while(!EpochManager::IsRecordCommitted(epoch)) {
            usleep(storage_sleep_time);
            epoch = EpochManager::GetPushDownEpoch();
        }
        epoch_mod = epoch % TaasContext::kCacheMaxLength;
        sleep_flag = true;
        //            uint64_t cnt = 0;
        while(epoch_redo_log_queue[epoch_mod]->try_dequeue(txn_ptr)) {
            //                cnt ++;
            //                LOG(INFO) << "Try Dequeue MOT, epoch : " <<  epoch_mod << " cnt: " << cnt;
            if(txn_ptr == nullptr || txn_ptr->txn_type() == proto::TxnType::NullMark) {
                continue;
            }
            //                commit_cv.notify_all();
            //                LOG(INFO) << "Send a txn to MOT, epoch : " <<  txn_ptr->commit_epoch();
            epoch = txn_ptr->commit_epoch();
            auto push_msg = std::make_unique<proto::Message>();
            auto push_response = push_msg->mutable_storage_push_response();
            push_response->set_result(proto::Success);
            push_response->set_epoch_id(epoch);
            push_response->set_txn_num(1);
            ptr = push_response->add_txns();
            *ptr = *txn_ptr;
            /// *(ptr) = (*txn_ptr);
            auto serialized_pull_resp_str = std::make_unique<std::string>();
            Gzip(push_msg.get(), serialized_pull_resp_str.get());
            MessageQueue::send_to_mot_storage_queue->enqueue(std::make_unique<send_params>(0, 0,
                "", epoch, proto::TxnType::CommittedTxn, std::move(serialized_pull_resp_str), nullptr));
            epoch_pushed_down_txn_num_local->IncCount(epoch, epoch, 1);
            txn_ptr.reset();
            sleep_flag = false;
        }
        if(sleep_flag)
            usleep(sleep_time);
    }
    //        if(sleep_flag)
    //            usleep(storage_sleep_time);
}



class RedoLoger {
public:
    static AtomicCounters epoch_log_lsn;///epoch, value        for epoch log (each epoch has single one counter)
    static std::vector<std::unique_ptr<concurrent_unordered_map<std::string, std::shared_ptr<proto::Transaction>>>> committed_txn_cache;
    static void StaticInit();
    static void ClearRedoLog(const uint64_t& epoch_mod);
    static bool RedoLog(const uint64_t& thread_id, std::shared_ptr<proto::Transaction> txn_ptr);
    static bool GeneratePushDownTask(const uint64_t& epoch);
    static bool CheckPushDownComplete(const uint64_t& epoch);
};

AtomicCounters RedoLoger::epoch_log_lsn(10);
std::vector<std::unique_ptr<concurrent_unordered_map<std::string, std::shared_ptr<proto::Transaction>>>> RedoLoger::committed_txn_cache;
void RedoLoger::StaticInit() {
    auto max_length = TaasContext::kCacheMaxLength;
    epoch_log_lsn.Init(max_length);
    committed_txn_cache.resize(max_length);

    for(int i = 0; i < static_cast<int>(max_length); i ++) {
        committed_txn_cache[i] = std::make_unique<concurrent_unordered_map<std::string, std::shared_ptr<proto::Transaction>>>();
    }
    if(StorageContext::is_tikv_enable) {
        TiKV::StaticInit();
    }
    if(StorageContext::is_leveldb_enable) {
        LevelDB::StaticInit();
    }
    if(StorageContext::is_hbase_enable) {
        HBase::StaticInit();
    }
    if(StorageContext::is_mot_enable) {
        MOT::StaticInit();
    }
    if(StorageContext::is_nebula_enable) {
        Nebula::StaticInit();
    }
}

void RedoLoger::ClearRedoLog(const uint64_t& epoch) {
    auto epoch_mod = epoch % TaasContext::kCacheMaxLength;
    committed_txn_cache[epoch_mod]->clear();
    epoch_log_lsn.SetCount(epoch_mod, 0);
    if(StorageContext::is_mot_enable) {
        MOT::StaticClear(epoch);
    }
    if(StorageContext::is_nebula_enable) {
        Nebula::StaticClear(epoch);
    }
    if(StorageContext::is_tikv_enable) {
        TiKV::StaticClear(epoch);
    }
    if(StorageContext::is_leveldb_enable) {
        LevelDB::StaticClear(epoch);
    }
    if(StorageContext::is_hbase_enable) {
        HBase::StaticClear(epoch);
    }
}


bool RedoLoger::RedoLog(const uint64_t& thread_id, std::shared_ptr<proto::Transaction> txn_ptr) {
    uint64_t epoch_id = txn_ptr->commit_epoch();
    auto lsn = epoch_log_lsn.IncCount(epoch_id, 1);
    auto key = std::to_string(epoch_id) + ":" + std::to_string(lsn);
    committed_txn_cache[epoch_id % TaasContext::kCacheMaxLength]->insert(key, txn_ptr);
    if(StorageContext::is_mot_enable) {
        if (txn_ptr->storage_type() == "mot")
            MOT::DBRedoLogQueueEnqueue(thread_id, epoch_id, txn_ptr);
    }
    txn_ptr.reset();
    return true;
}

bool RedoLoger::GeneratePushDownTask(const uint64_t &epoch) {
    MOT::GeneratePushDownTask(epoch);
    return true;
}

bool RedoLoger::CheckPushDownComplete(const uint64_t &epoch) {
    return (StorageContext::is_mot_enable == 0 || MOT::CheckEpochPushDownComplete(epoch));
}












///Transaction

class TransactionCache {
public:
    ///message handler
    static std::vector<std::unique_ptr<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>>
        epoch_backup_txn,
        epoch_insert_set,
        epoch_abort_set;

    static concurrent_unordered_map<std::string, std::shared_ptr<MultiModelTxn>> MultiModelTxnMap;

    ///merge
    static std::vector<std::unique_ptr<concurrent_crdt_unordered_map<std::string, std::string, std::string>>>
        epoch_merge_map, ///epoch merge   row_header
        local_epoch_abort_txn_set,
        epoch_abort_txn_set; /// for epoch final check

    static std::vector<std::unique_ptr<concurrent_unordered_map<std::string, std::shared_ptr<proto::Transaction>>>>
        epoch_txn_map, epoch_write_set_map, epoch_back_txn_map;

    static concurrent_unordered_map<std::string, std::string>
        read_version_map,
        read_version_map_data, ///read validate for higher isolation
        read_version_map_csn, ///read validate for higher isolation
        insert_set;   ///插入集合，用于判断插入是否可以执行成功 check key exits?

    static std::vector<std::unique_ptr<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>>
        epoch_read_validate_queue,
        epoch_merge_queue,///存放要进行merge的事务，分片
        epoch_commit_queue,
        epoch_redo_log_queue,
        epoch_result_return_queue;///存放每个epoch要进行写日志的事务，分片写日志

    static void CacheInit();
    static void EpochCacheClear(uint64_t& epoch);
};

std::vector<std::unique_ptr<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>>
    TransactionCache::epoch_backup_txn,
    TransactionCache::epoch_insert_set,
    TransactionCache::epoch_abort_set;

concurrent_unordered_map<std::string, std::shared_ptr<MultiModelTxn>> TransactionCache::MultiModelTxnMap;

std::vector<std::unique_ptr<concurrent_crdt_unordered_map<std::string, std::string, std::string>>>
    TransactionCache::epoch_merge_map,
    TransactionCache::local_epoch_abort_txn_set,
    TransactionCache::epoch_abort_txn_set;

std::vector<std::unique_ptr<concurrent_unordered_map<std::string, std::shared_ptr<proto::Transaction>>>>
    TransactionCache::epoch_txn_map,
    TransactionCache::epoch_write_set_map,
    TransactionCache::epoch_back_txn_map;

concurrent_unordered_map<std::string, std::string>
    TransactionCache::read_version_map,
    TransactionCache::read_version_map_data,
    TransactionCache::read_version_map_csn,
    TransactionCache::insert_set;

std::vector<std::unique_ptr<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>>
    TransactionCache::epoch_read_validate_queue,
    TransactionCache::epoch_merge_queue,
    TransactionCache::epoch_commit_queue,
    TransactionCache::epoch_redo_log_queue,
    TransactionCache::epoch_result_return_queue;


void TransactionCache::CacheInit() {
    auto max_length = TaasContext::kCacheMaxLength;

    ///Message handle
    epoch_backup_txn.resize(max_length);
    epoch_insert_set.resize(max_length);
    epoch_abort_set.resize(max_length);

    for(int i = 0; i < static_cast<int>(max_length); i ++) {
        epoch_backup_txn[i] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
        epoch_insert_set[i] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
        epoch_abort_set[i] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
    }

    ///Merge
    epoch_merge_map.resize(max_length);
    local_epoch_abort_txn_set.resize(max_length);
    epoch_abort_txn_set.resize(max_length);
    epoch_txn_map.resize(max_length);
    epoch_back_txn_map.resize(max_length);
    epoch_write_set_map.resize(max_length);

    epoch_read_validate_queue.resize(max_length);
    epoch_merge_queue.resize(max_length);
    epoch_commit_queue.resize(max_length);
    epoch_redo_log_queue.resize(max_length);
    epoch_result_return_queue.resize(max_length);

    for(int i = 0; i < static_cast<int>(max_length); i ++) {
        epoch_merge_map[i] = std::make_unique<concurrent_crdt_unordered_map<std::string, std::string, std::string>>();
        local_epoch_abort_txn_set[i] = std::make_unique<concurrent_crdt_unordered_map<std::string, std::string, std::string>>();
        epoch_abort_txn_set[i] = std::make_unique<concurrent_crdt_unordered_map<std::string, std::string, std::string>>();
        epoch_txn_map[i] = std::make_unique<concurrent_unordered_map<std::string, std::shared_ptr<proto::Transaction>>>();
        epoch_back_txn_map[i] = std::make_unique<concurrent_unordered_map<std::string, std::shared_ptr<proto::Transaction>>>();
        epoch_write_set_map[i] = std::make_unique<concurrent_unordered_map<std::string, std::shared_ptr<proto::Transaction>>>();

        epoch_read_validate_queue[i] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
        epoch_merge_queue[i] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
        epoch_commit_queue[i] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
        epoch_redo_log_queue[i] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
        epoch_result_return_queue[i] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
    }
}

void TransactionCache::EpochCacheClear(uint64_t &epoch) {
    auto epoch_mod_temp = epoch % TaasContext::kCacheMaxLength;

    ///Message handle
    //        epoch_backup_txn[cache_clear_epoch_num_mod] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
    //        epoch_insert_set[cache_clear_epoch_num_mod] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();
    //        epoch_abort_set[cache_clear_epoch_num_mod] = std::make_unique<BlockingConcurrentQueue<std::shared_ptr<proto::Transaction>>>();

    ///Merge

    epoch_merge_map[epoch_mod_temp]->clear();
    epoch_txn_map[epoch_mod_temp]->clear();
    epoch_write_set_map[epoch_mod_temp]->clear();
    epoch_back_txn_map[epoch_mod_temp]->clear();
    epoch_abort_txn_set[epoch_mod_temp]->clear();
    local_epoch_abort_txn_set[epoch_mod_temp]->clear();
}

class CRDTMerge{
public:
    static bool ValidateReadSet(std::shared_ptr<proto::Transaction> txn_ptr);
    static bool ValidateWriteSet(std::shared_ptr<proto::Transaction> txn_ptr);
    static bool MultiMasterCRDTMerge(std::shared_ptr<proto::Transaction> txn_ptr);
    static bool Commit(std::shared_ptr<proto::Transaction> txn_ptr);

};



bool CRDTMerge::ValidateReadSet(std::shared_ptr<proto::Transaction> txn_ptr) {
    ///RC & RR & SI
    //RC do not check read data
    auto epoch_mod = txn_ptr->commit_epoch() % TaasContext::kCacheMaxLength;
    std::string version;
    uint64_t csn = 0;
    for(auto i = 0; i < txn_ptr->row_size(); i ++) {
        const auto& row = txn_ptr->row(i);
        auto key = txn_ptr->storage_type() + ":" + row.key();
        if(row.op_type() != proto::OpType::Read) {
            continue;
        }
        /// indeed, we should use the csn to check the read version,
        /// but there are some bugs in updating the csn to the storage(tikv).
        if (!TransactionCache::read_version_map.getValue(key, version)) {
            /// should be abort, but Taas do not connect load data,
            /// so read the init snap will get empty in read_version_map
            continue;
        }
        if (version != row.data()) { /// row.data == data item.version_csn
                                      //                continue; ///only for debug
            auto csn_temp = std::to_string(txn_ptr->csn()) + ":" + std::to_string(txn_ptr->txn_server_id());
            TransactionCache::epoch_abort_txn_set[epoch_mod]->insert(csn_temp, csn_temp);
            //                LOG(INFO) <<"Txn read version check failed";
            //                LOG(INFO) <<"read version check failed version : " << version << ", row.data() : " << row.data();
            txn_ptr.reset();
            return false;
        }
    }
    txn_ptr.reset();
    return true;
}

bool CRDTMerge::ValidateWriteSet(std::shared_ptr<proto::Transaction> txn_ptr) {
    auto epoch_mod = txn_ptr->commit_epoch() % TaasContext::kCacheMaxLength;
    auto csn_temp = std::to_string(txn_ptr->csn()) + ":" + std::to_string(txn_ptr->txn_server_id());
    if(TransactionCache::epoch_abort_txn_set[epoch_mod]->contain(csn_temp, csn_temp)) {
        txn_ptr.reset();
        return false;
    }
    txn_ptr.reset();
    return true;
}

bool CRDTMerge::MultiMasterCRDTMerge(std::shared_ptr<proto::Transaction> txn_ptr) {
    auto epoch_mod = txn_ptr->commit_epoch() % TaasContext::kCacheMaxLength;
    auto csn_temp = std::to_string(txn_ptr->csn()) + ":" + std::to_string(txn_ptr->txn_server_id());
    std::string csn_result;
    bool result = true;
    for(auto i = 0; i < txn_ptr->row_size(); i ++) {
        const auto& row = txn_ptr->row(i);
        if(row.op_type() == proto::OpType::Read) {
            continue;
        }
        auto key = txn_ptr->storage_type() + ":" + row.key();
        if (!TransactionCache::epoch_merge_map[epoch_mod]->insert(key, csn_temp, csn_result)) {
            TransactionCache::epoch_abort_txn_set[epoch_mod]->insert(csn_result, csn_result);
            result = false;
        }
    }
    txn_ptr.reset();
    return result;
}

bool CRDTMerge::Commit(std::shared_ptr<proto::Transaction> txn_ptr) {
    auto csn_temp = std::to_string(txn_ptr->csn()) + ":" + std::to_string(txn_ptr->txn_server_id());
    for(auto i = 0; i < txn_ptr->row_size(); i ++) {
        const auto& row = txn_ptr->row(i);
        auto key = txn_ptr->storage_type() + ":" + row.key();
        if(row.op_type() == proto::OpType::Read) {
            continue;
        }
        else if(row.op_type() == proto::OpType::Insert) {
            TransactionCache::insert_set.insert(key, csn_temp);
        }
        else if(row.op_type() == proto::OpType::Delete) {
            TransactionCache::insert_set.remove(key, csn_temp);
        }
        else {
            //nothing to do
        }
        TransactionCache::read_version_map.insert(key, csn_temp);
    }
    txn_ptr.reset();
    return true;
}


class Merger : public ThreadCounters {

public:
    std::unique_ptr<zmq::message_t> message_ptr;
    std::unique_ptr<std::string> message_string_ptr;
    std::unique_ptr<proto::Message> msg_ptr;
    std::shared_ptr<proto::Transaction> txn_ptr, write_set, backup_txn, full_txn;
    std::shared_ptr<std::vector<std::shared_ptr<proto::Transaction>>> shard_row_vector;
    std::unique_ptr<pack_params> pack_param;
    std::string csn_temp, key_temp, key_str, table_name, csn_result;
    uint64_t local_server_id, epoch_mod = 0, epoch = 0, max_length = 0, server_num = 1, shard_id = 0, shard_server_id =0, replica_num = 1,
                              round_robin = 0, sent_to = 0,///cache check
        message_epoch = 0, message_epoch_mod = 0, message_server_id = 0, ///message epoch info
        txn_server_id = 0;

    bool res, sleep_flag;
    std::shared_ptr<proto::Transaction> empty_txn_ptr;
    std::hash<std::string> _hash;

    uint64_t total_single_shard_time = 0,
             total_single_remote_handle_time = 0,
             total_single_validate_time = 0,
             total_single_merge_time = 0,
             total_single_abort_set_time = 0,
             total_single_commit_time = 0,
             total_single_log_time = 0,
             total_single_result_time = 0,
             total_single_time = 0,

             total_single_shard_num = 0,
             total_single_remote_handle_num = 0,
             total_single_validate_num = 0,
             total_single_merge_num = 0,
             total_single_abort_set_num = 0,
             total_single_commit_num = 0,
             total_single_log_num = 0,
             total_single_result_num = 0,
             total_single_num = 0;

    [[nodiscard]] uint64_t GetHashValue(const std::string& key) const {
        return _hash(key) % shard_num;
    }

public:
    bool MergeQueueTryDequeue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_);
    bool CommitQueueTryDequeue(uint64_t &epoch_, std::shared_ptr<proto::Transaction> txn_ptr_);

    void MergeInit(const uint64_t &id);
    void ReadValidate();
    void Send();
    void Merge();
    void Commit();
    void RedoLog();
    void ResultReturn();
    void EpochMerge();

    void ReadValidateQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction> &txn_ptr_);
    void MergeQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_);
    void CommitQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_);
    void ResultReturnQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_);

};


void Merger::MergeInit(const uint64_t &id) {

    txn_ptr.reset();
    message_ptr = nullptr;
    shard_num = TaasContext::kTxnNodeNum;
    local_server_id = TaasContext::txn_node_ip_index;
    ThreadCountersInit(ctx);
}


void Merger::ReadValidateQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_) {
    auto epoch_mod_temp = epoch_ % TaasContext::kCacheMaxLength;
    epoch_should_read_validate_txn_num_local->IncCount(epoch_mod_temp, txn_ptr_->txn_server_id(), 1);
    TransactionCache::epoch_read_validate_queue[epoch_mod_temp]->enqueue(txn_ptr_);
    TransactionCache::epoch_read_validate_queue[epoch_mod_temp]->enqueue(nullptr);
}
void Merger::MergeQueueEnqueue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_) {
    auto epoch_mod_temp = epoch_ % TaasContext::kCacheMaxLength;
    epoch_should_merge_txn_num_local->IncCount(epoch_mod_temp, txn_ptr->txn_server_id(), 1);
    TransactionCache::epoch_merge_queue[epoch_mod_temp]->enqueue(txn_ptr_);
    TransactionCache::epoch_merge_queue[epoch_mod_temp]->enqueue(nullptr);
}
void Merger::CommitQueueEnqueue(uint64_t& epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_) {
    auto epoch_mod_temp = epoch_ % TaasContext::kCacheMaxLength;
    epoch_should_commit_txn_num_local->IncCount(epoch_mod_temp, txn_ptr_->txn_server_id(), 1);
    TransactionCache::epoch_commit_queue[epoch_mod_temp]->enqueue(txn_ptr_);
    TransactionCache::epoch_commit_queue[epoch_mod_temp]->enqueue(nullptr);
}
void Merger::ResultReturnQueueEnqueue(uint64_t& epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_) {
    auto epoch_mod_temp = epoch_ % TaasContext::kCacheMaxLength;
    epoch_result_return_txn_num_local->IncCount(epoch_mod_temp, txn_ptr_->txn_server_id(), 1);
    TransactionCache::epoch_result_return_queue[epoch_mod_temp]->enqueue(txn_ptr_);
    TransactionCache::epoch_result_return_queue[epoch_mod_temp]->enqueue(nullptr);
}

bool Merger::MergeQueueTryDequeue(uint64_t &epoch_, const std::shared_ptr<proto::Transaction>& txn_ptr_) {
    ///not use for now
    return false;
}
bool Merger::CommitQueueTryDequeue(uint64_t& epoch_, std::shared_ptr<proto::Transaction> txn_ptr_) {
    auto epoch_mod_temp = epoch_ % TaasContext::kCacheMaxLength;
    return TransactionCache::epoch_commit_queue[epoch_mod_temp]->try_dequeue(txn_ptr_);
}




void Merger::Send() {
}

void Merger::ReadValidate() {
    message_epoch = txn_ptr->commit_epoch();
    message_epoch_mod = message_epoch % TaasContext::kCacheMaxLength;
    message_server_id = txn_ptr->txn_server_id();
    shard_id = txn_ptr->shard_id();
    shard_server_id = txn_ptr->shard_server_id();
    auto time1 = now_to_us();
    if (CRDTMerge::ValidateReadSet(txn_ptr)) {
        /// already enqueue merge_queue, commit_queue, redo_log_queue, result_return_queue
    }
    else {
        total_read_version_check_failed_txn_num_local.fetch_add(1);
        csn_temp = std::to_string(txn_ptr->csn()) + ":" + std::to_string(txn_ptr->txn_server_id());
        TransactionCache::epoch_abort_txn_set[message_epoch_mod]->insert(csn_temp, csn_temp);
    }
    epoch_read_validated_txn_num_local->IncCount(message_epoch, message_server_id, 1);
    //        LOG(INFO) << "Validate Time Cost " << now_to_us() - time1 << " us";
    total_single_validate_time += now_to_us() - time1;
    total_single_validate_num ++;
}

void Merger::Merge() {
    auto time1 = now_to_us();
    epoch = txn_ptr->commit_epoch();
    CRDTMerge::MultiMasterCRDTMerge(txn_ptr);
    total_merge_txn_num_local.fetch_add(1);
    total_merge_latency_local.fetch_add(now_to_us() - time1);
    epoch_merged_txn_num_local->IncCount(epoch, txn_server_id, 1);
    //        LOG(INFO) << "Merge Time Cost " << now_to_us() - time1 << " us";
    total_single_merge_time += now_to_us() - time1;
    total_single_merge_num ++;
}

void Merger::Commit() {
    auto time1 = now_to_us();
    if (CRDTMerge::ValidateWriteSet(txn_ptr)) {
        CRDTMerge::Commit(txn_ptr);
    }
    epoch_committed_txn_num_local->IncCount(epoch, txn_ptr->txn_server_id(), 1);
    //        LOG(INFO) << "Commit Time Cost " << now_to_us() - time1 << " us";
    total_single_commit_time += now_to_us() - time1;
    total_single_commit_num ++;
}

void Merger::RedoLog() {
    auto time1 = now_to_us();
    if (!CRDTMerge::ValidateWriteSet(txn_ptr)) {
        total_failed_txn_num_local.fetch_add(1);
        //            EpochMessageSendHandler::SendTxnCommitResultToClient(txn_ptr, proto::TxnState::Abort);
    } else {
        RedoLoger::RedoLog(thread_id, txn_ptr);
        //            EpochMessageSendHandler::SendTxnCommitResultToClient(txn_ptr, proto::TxnState::Commit);
        success_commit_txn_num_local.fetch_add(1);
        success_commit_latency_local.fetch_add(now_to_us() - time1);
    }
    total_commit_txn_num_local.fetch_add(1);
    total_commit_latency_local.fetch_add(now_to_us() - time1);
    //        LOG(INFO) << "******* Merge RedoLog Epoch : " << epoch << "txn_server_id" << txn_ptr->txn_server_id() << "********\n";
    epoch_record_committed_txn_num_local->IncCount(epoch, txn_ptr->txn_server_id(), 1);
    //        LOG(INFO) << "RedoLog Time Cost " << now_to_us() - time1 << " us";
    total_single_log_time += now_to_us() - time1;
    total_single_log_num ++;
}

void Merger::ResultReturn() {
    auto time1 = now_to_us();
    if (!CRDTMerge::ValidateWriteSet(txn_ptr)) {
        EpochMessageSendHandler::SendTxnCommitResultToClient(txn_ptr, proto::TxnState::Abort);
    } else {
        EpochMessageSendHandler::SendTxnCommitResultToClient(txn_ptr, proto::TxnState::Commit);
    }
    epoch_result_returned_txn_num_local->IncCount(epoch, txn_ptr->txn_server_id(), 1);
    total_single_result_time += now_to_us() - time1;
    total_single_result_num ++;
    total_single_time += now_to_us() - txn_ptr->csn();
    total_single_num ++;
    if(total_single_num > 0 &&  total_single_num % TaasContext::print_mode_size == 0) {
//        LOG(INFO) << " Validate Time Cost : " << total_single_validate_time  << " Validate Time count : " << total_single_validate_num << " Validate avg : " << total_single_validate_time/total_single_validate_num
//                  << " Merge Time Cost : " << total_single_merge_time << " Merge Time count : " << total_single_merge_num << " Validate avg : " << total_single_merge_time/total_single_merge_num
//                  << " Commit Time Cost : " << total_single_commit_time << " Commit Time count : " << total_single_commit_num << " Validate avg : " << total_single_commit_time/total_single_commit_num
//                  << " RedoLog Time Cost : " << total_single_log_time << " RedoLog Time count : " << total_single_log_num << " Validate avg : " << total_single_log_time/total_single_log_num
//                  << " ResultReturn Time Cost : " << total_single_result_time << " ResultReturn Time count : " << total_single_result_num << " Validate avg : " << total_single_result_time/total_single_result_num
//                  << " Total Time Cost : " << total_single_time << " Total Time count : " << total_single_num << "Validate avg : " << total_single_time/total_single_num
//                  << " end";
    }
    //        LOG(INFO) << "ResultReturn Time Cost " << now_to_us() - time1 << " us";
    //        LOG(INFO) << "Total Cost " << now_to_us() - txn_ptr->csn() << " us";
}

void Merger::EpochMerge() {
    epoch = EpochManager::GetLogicalEpoch();
    while (!EpochManager::IsTimerStop()) {
        sleep_flag = true;
        epoch = EpochManager::GetLogicalEpoch();
        epoch_mod = epoch % TaasContext::kCacheMaxLength;

        while(TransactionCache::epoch_read_validate_queue[epoch_mod]->try_dequeue(txn_ptr)) { /// only local txn do this procedure
            if (txn_ptr != nullptr && txn_ptr->txn_type() != proto::TxnType::NullMark) {
                ReadValidate();
                txn_ptr.reset();
                sleep_flag = false;
            }
        }

        if(!EpochManager::IsEpochMergeComplete(epoch)) {
            while (TransactionCache::epoch_merge_queue[epoch_mod]->try_dequeue(txn_ptr)) {
                if (txn_ptr != nullptr && txn_ptr->txn_type() != proto::TxnType::NullMark) {
                    Merge();
                    txn_ptr.reset();
                    sleep_flag = false;
                }
            }
        }

        if(EpochManager::IsAbortSetMergeComplete(epoch) && !EpochManager::IsCommitComplete(epoch)) {
            while (!EpochManager::IsCommitComplete(epoch) &&
                   TransactionCache::epoch_commit_queue[epoch_mod]->try_dequeue(txn_ptr)) {
                if (txn_ptr != nullptr && txn_ptr->txn_type() != proto::TxnType::NullMark) {
                    Commit();
                    txn_ptr.reset();
                    sleep_flag = false;
                }
            }
        }

        if(EpochManager::IsAbortSetMergeComplete(epoch) && !EpochManager::IsRecordCommitted(epoch)) {
            //                LOG(INFO) << "******* Merge RedoLog 1 : " << epoch << "********\n";
            while (!EpochManager::IsRecordCommitted(epoch) && TransactionCache::epoch_redo_log_queue[epoch_mod]->try_dequeue(txn_ptr)) {
                if (txn_ptr != nullptr && txn_ptr->txn_type() != proto::TxnType::NullMark) { /// only local txn do redo log
                                                                                              //                        LOG(INFO) << "******* Merge RedoLog 2 : " << epoch << "txn_server_id" << txn_ptr->txn_server_id() << "********\n";
                    RedoLog();
                    txn_ptr.reset();
                    sleep_flag = false;
                }
            }
        }

        if(sleep_flag)
            usleep(merge_sleep_time);
    }
}





///Epoch


const uint64_t sleep_time, logical_sleep_timme, storage_sleep_time, merge_sleep_time, message_sleep_time;
uint64_t cache_server_available, total_commit_txn_num;
std::atomic<uint64_t> merge_epoch , abort_set_epoch ,
    commit_epoch , redo_log_epoch , clear_epoch ;
std::atomic<int> init_ok_num;
std::atomic<bool> is_epoch_advance_started, test_start;
void InitEpochTimerManager();
bool CheckRedoLogPushDownState();
void EpochLogicalTimerManagerThreadMain();
void EpochPhysicalTimerManagerThreadMain();
std::string PrintfToString(const char* format, ...);
void OUTPUTLOG(const std::string& s, uint64_t& epoch);

class EpochManager {
private:
    static bool timerStop;
    static std::atomic<uint64_t> logical_epoch, physical_epoch, push_down_epoch;
public:

    static uint64_t max_length;
    static std::vector<std::unique_ptr<std::atomic<bool>>>
        merge_complete, abort_set_merge_complete,
        commit_complete, record_committed, result_returned,
        is_current_epoch_abort;

    ///epoch, index, value  for 集群状态
    static AtomicCounters_Cache server_state; /// epoch, csn(atomic increase)
    static  std::vector<std::unique_ptr<std::atomic<uint64_t>>> online_server_num;
    ///fault tolerance: cache server mod
    static std::vector<std::unique_ptr<std::atomic<uint64_t>>>  cache_server_received_epoch;

    static std::atomic<uint64_t> view_change_epoch, view_change_server_num;

    static void SetTimerStop(bool value) {timerStop = value;}
    static bool IsTimerStop() {return timerStop;}

    static bool IsEpochMergeComplete(uint64_t epoch) {return merge_complete[epoch % max_length]->load();}
    static void SetEpochMergeComplete(uint64_t epoch, bool value) {merge_complete[epoch % max_length]->store(value);}

    static bool IsAbortSetMergeComplete(uint64_t epoch) {return abort_set_merge_complete[epoch % max_length]->load();}
    static void SetAbortSetMergeComplete(uint64_t epoch, bool value) {abort_set_merge_complete[epoch % max_length]->store(value);}

    static bool IsCommitComplete(uint64_t epoch) {return commit_complete[epoch % max_length]->load();}
    static void SetCommitComplete(uint64_t epoch, bool value) {commit_complete[epoch % max_length]->store(value);}

    static bool IsRecordCommitted(uint64_t epoch){ return record_committed[epoch % max_length]->load();}
    static void SetRecordCommitted(uint64_t epoch, bool value){ record_committed[epoch % max_length]->store(value);}

    static bool IsResultReturned(uint64_t epoch){ return result_returned[epoch % max_length]->load();}
    static void SetResultReturned(uint64_t epoch, bool value){ result_returned[epoch % max_length]->store(value);}

    static bool IsCurrentEpochAbort(uint64_t epoch){ return is_current_epoch_abort[epoch % max_length]->load();}
    static void SetCurrentEpochAbort(uint64_t epoch, bool value){ is_current_epoch_abort[epoch % max_length]->store(value);}

    static void SetPhysicalEpoch(uint64_t value){ physical_epoch.store(value);}
    static uint64_t AddPhysicalEpoch(){
        return physical_epoch.fetch_add(1);
    }
    static uint64_t GetPhysicalEpoch(){ return physical_epoch.load();}

    static void SetLogicalEpoch(uint64_t value){ logical_epoch.store(value);}
    static uint64_t AddLogicalEpoch(){
        return logical_epoch.fetch_add(1);
    }
    static uint64_t GetLogicalEpoch(){ return logical_epoch.load();}

    static void SetPushDownEpoch(uint64_t value){ push_down_epoch.store(value);}
    static uint64_t AddPushDownEpoch(){
        return push_down_epoch.fetch_add(1);
    }
    static uint64_t GetPushDownEpoch(){ return push_down_epoch.load();}


    static void ClearMergeEpochState(uint64_t& epoch) {
        auto epoch_mod = epoch %  max_length;
        merge_complete[epoch_mod]->store(false);
        abort_set_merge_complete[epoch_mod]->store(false);
        commit_complete[epoch_mod]->store(false);
        record_committed[epoch_mod]->store(false);
        result_returned[epoch_mod]->store(false);
        is_current_epoch_abort[epoch_mod]->store(false);
    }

    static void EpochCacheSafeCheck() {
        if(((GetLogicalEpoch() % TaasContext::kCacheMaxLength) ==  ((GetPhysicalEpoch() + 55) % TaasContext::kCacheMaxLength)) ||
            ((GetPushDownEpoch() % TaasContext::kCacheMaxLength) ==  ((GetPhysicalEpoch() + 55) % TaasContext::kCacheMaxLength))) {
            uint64_t i = 0;
            OUTPUTLOG("Assert", reinterpret_cast<uint64_t &>(i));
            printf("++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");
            printf("+++++++++++++++Fata : Cache Size exceeded!!! +++++++++++++++++++++\n");
            printf("++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");
            SetTimerStop(true);
            assert(false);
        }
    }

    static uint64_t AddOnLineServerNum(uint64_t& epoch, uint64_t value) {
        return online_server_num[epoch % max_length]->fetch_add(value);
    }
    static uint64_t SubOnLineServerNum(uint64_t& epoch, uint64_t value) {
        return online_server_num[epoch % max_length]->fetch_sub(value);
    }
    static void StoreOnLineServerNum(uint64_t& epoch, uint64_t value) {
        online_server_num[epoch % max_length]->store(value);
    }
    static uint64_t GetOnLineServerNum(uint64_t& epoch) {
        return online_server_num[epoch % max_length]->load();
    }
    static void SetServerOnLine(uint64_t& epoch, const std::string& ip);
    static void SetServerOffLine(uint64_t& epoch, const std::string& ip);

    static void SetCacheServerStored(uint64_t& epoch, uint64_t value) {
        cache_server_received_epoch[epoch % max_length]->store(value);
    }

    static bool IsInitOK() {
        return init_ok_num.load() >= (int)(TaasContext::kEpochMessageThreadNum +
                                           TaasContext::kEpochTxnThreadNum + TaasContext::kMergeThreadNum + 1 + 4);
    }
};

using namespace std;
const uint64_t sleep_time = 100, logical_sleep_timme = 100, storage_sleep_time = 100, merge_sleep_time = 100, message_sleep_time = 50;
uint64_t cache_server_available = 1, total_commit_txn_num = 0;
std::atomic<uint64_t> merge_epoch = 1, abort_set_epoch = 1,
                      commit_epoch = 1, redo_log_epoch = 1, clear_epoch = 1;

bool EpochManager::timerStop = false;
std::atomic<uint64_t> EpochManager::logical_epoch(1), EpochManager::physical_epoch(0), EpochManager::push_down_epoch(1);
uint64_t EpochManager::max_length = 10000;
//epoch merge state
std::vector<std::unique_ptr<std::atomic<bool>>> EpochManager::merge_complete, EpochManager::abort_set_merge_complete,
    EpochManager::commit_complete, EpochManager::record_committed, EpochManager::result_returned, EpochManager::is_current_epoch_abort;
//cluster state
std::vector<std::unique_ptr<std::atomic<uint64_t>>> EpochManager::online_server_num;
AtomicCounters_Cache EpochManager::server_state(10, 2);
//cache server
std::vector<std::unique_ptr<std::atomic<uint64_t>>> EpochManager::cache_server_received_epoch;


std::atomic<uint64_t> EpochManager::view_change_epoch(0), EpochManager::view_change_server_num(0);
// EpochPhysicalTimerManagerThreadMain中得到的当前微秒级别的时间戳
uint64_t start_time_ll, start_physical_epoch = 1;
struct timeval start_time;

// EpochManager是否初始化完成
std::atomic<int> init_ok_num(0);
std::atomic<bool> is_epoch_advance_started(false), test_start(false);

void InitEpochTimerManager(){
    Merger::StaticInit();
    TransactionCache::CacheInit();
    ThreadCounters::StaticInit();
    MessageQueue::StaticInitMessageQueue();
    EpochMessageSendHandler::StaticInit();
    EpochMessageReceiveHandler::StaticInit();
    RedoLoger::StaticInit();

    EpochManager::max_length = TaasContext::kCacheMaxLength;
    //==========Logical Epoch Merge State=============
    EpochManager::merge_complete.resize(EpochManager::max_length);
    EpochManager::abort_set_merge_complete.resize(EpochManager::max_length);
    EpochManager::commit_complete.resize(EpochManager::max_length);
    EpochManager::record_committed.resize(EpochManager::max_length);
    EpochManager::result_returned.resize(EpochManager::max_length);
    EpochManager::is_current_epoch_abort.resize(EpochManager::max_length);
    //cluster state
    EpochManager::online_server_num.resize(EpochManager::max_length + 1);
    //        EpochManager::should_receive_pack_num.resize(EpochManager::max_length + 1);
    EpochManager::server_state.Init(EpochManager::max_length,TaasContext::kTxnNodeNum + 5, 1);
    //cache server
    EpochManager::cache_server_received_epoch.resize(EpochManager::max_length + 1);
    uint64_t val = 1;
    if(TaasContext::is_cache_server_available) {
        val = 0;
    }

    for(int i = 0; i < static_cast<int>(EpochManager::max_length); i ++) {
        EpochManager::merge_complete[i] = std::make_unique<std::atomic<bool>>(false);
        EpochManager::abort_set_merge_complete[i] = std::make_unique<std::atomic<bool>>(false);
        EpochManager::commit_complete[i] = std::make_unique<std::atomic<bool>>(false);
        EpochManager::record_committed[i] = std::make_unique<std::atomic<bool>>(false);
        EpochManager::result_returned[i] = std::make_unique<std::atomic<bool>>(false);
        EpochManager::is_current_epoch_abort[i] = std::make_unique<std::atomic<bool>>(false);
        //cluster state
        EpochManager::online_server_num[i] = std::make_unique<std::atomic<uint64_t>>();
        EpochManager::online_server_num[i]->store(TaasContext::kTxnNodeNum + 5);
        //cache server
        EpochManager::cache_server_received_epoch[i] =std::make_unique<std::atomic<uint64_t>>(val);

    }
    init_ok_num.fetch_add(1);
}


/**
 * @brief 根据配置信息计算得到间隔多长时间增加epoch号
 *
 * @param ctx XML中的配置信息
 * @return uint64_t 微妙级的时间戳
 */
uint64_t GetSleeptime(){
    uint64_t sleep_time_temp;
    // current_time由两部分组成，tv_sec + tv_usec，代表秒和毫秒数，合起来就是总的时间戳
    struct timeval current_time{};
    uint64_t current_time_ll;
    gettimeofday(&current_time, nullptr);
    // 得到目前的微秒级时间戳
    current_time_ll = current_time.tv_sec * 1000000 + current_time.tv_usec;
    sleep_time_temp = current_time_ll - (start_time_ll + (long)(EpochManager::GetPhysicalEpoch() - start_physical_epoch) * TaasContext::kEpochSize_us);
    if(sleep_time_temp >= TaasContext::kEpochSize_us){
        return 0;
    }
    else{
        return TaasContext::kEpochSize_us - sleep_time_temp;
    }
}

std::string PrintfToString(const char* format, ...) {
    char buffer[5120];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    return std::string(buffer);
}

void OUTPUTLOG(const string& s, uint64_t& epoch_){
    auto epoch_mod = epoch_ % EpochManager::max_length;
//    LOG(INFO) << PrintfToString("%60s \n\
//        physical                     %6lu, logical                      %6lu,   \
//        pushdown_mot                 %6lu, pushdownepoch                %6lu  \n\
//        merge_epoch                  %6lu, abort_set_epoch              %6lu    \
//        commit_epoch                 %6lu, redo_log_epoch               %6lu  \n\
//        clear_epoch                  %6lu,                                        \
//        epoch_mod                    %6lu, disstance                    %6lu  \n\
//\
//        handlelocaltxnNum            %6lu, shouldhandlelocaltxnNum      %6lu,   \
//        handleremotetxnNum           %6lu, shouldhandleremotetxnNum     %6lu, \n\
//        ReadValidatedTxnNum          %6lu, ShouldReadValidateTxnNum     %6lu,   \
//        MergedTxnNum                 %6lu, ShouldMergeTxnNum            %6lu, \n\
//        CommittedTxnNum              %6lu, ShouldCommitTxnNum           %6lu,   \
//        RecordCommit                 %6lu, RecordCommitted              %6lu, \n\
//        ShouldReceiveShardPackNum    %6lu, ReceivedShardPackNum         %6lu    \
//        ShouldReceiveShardTxnNum     %6lu, ReceivedShardTxnNum          %6lu  \n\
//        ShouldReceiveRemotePackNum   %6lu, ReceivedRemotePackNum        %6lu    \
//        ShouldReceiveRemoteTxnNum    %6lu, ReceivedRemoteTxnNum         %6lu  \n\
//        ShouldReceiveBackUpPackNum   %6lu, ReceivedBackUpPackNum        %6lu    \
//        ShouldReceiveBackUpTxnNum    %6lu, ReceivedBackUpTxnNum         %6lu  \n\
//        ShouldReceiveInsertsetNum    %6lu, ReceivedInsertSetNum         %6lu    \
//        ShouldReceiveAbortSetNum     %6lu, ReceivedAbortSetNum          %6lu  \n\
//        ReceivedShardACKNum          %6lu, ReceivedBackupACKNum         %6lu    \
//        ReceivedInsertSetACKNum      %6lu, ReceivedAbortSetACKNum       %6lu  \n\
//        merge_num                    %6lu, time          %lu \n\
//====\
//        message send client num      %6lu, message receive client num   %6lu    \
//        handled client txn num       %6lu\n",
//                     s.c_str(),
//                     EpochManager::GetPhysicalEpoch(),                                                  EpochManager::GetLogicalEpoch(),
//                     MOT::pushed_down_epoch.load(),                                                EpochManager::GetPushDownEpoch(),
//                     merge_epoch.load(), abort_set_epoch.load(), commit_epoch.load(), redo_log_epoch.load(),clear_epoch.load(),
//                     epoch_mod,                                                                         EpochManager::GetPhysicalEpoch() - EpochManager::GetLogicalEpoch(),
//
//                     EpochMessageReceiveHandler::GetAllThreadLocalCountNum(epoch_mod, EpochMessageReceiveHandler::shard_handled_local_txn_num_local_vec),
//                     EpochMessageReceiveHandler::GetAllThreadLocalCountNum(epoch_mod, EpochMessageReceiveHandler::shard_should_handle_local_txn_num_local_vec),
//                     EpochMessageReceiveHandler::GetAllThreadLocalCountNum(epoch_mod, EpochMessageReceiveHandler::shard_handled_remote_txn_num_local_vec),
//                     EpochMessageReceiveHandler::GetAllThreadLocalCountNum(epoch_mod, EpochMessageReceiveHandler::shard_should_handle_remote_txn_num_local_vec),
//                     Merger::GetAllThreadLocalCountNum(epoch_mod, Merger::epoch_read_validated_txn_num_local_vec),
//                     Merger::GetAllThreadLocalCountNum(epoch_mod, Merger::epoch_should_read_validate_txn_num_local_vec),
//                     Merger::GetAllThreadLocalCountNum(epoch_mod, Merger::epoch_merged_txn_num_local_vec),
//                     Merger::GetAllThreadLocalCountNum(epoch_mod, Merger::epoch_should_merge_txn_num_local_vec),
//                     Merger::GetAllThreadLocalCountNum(epoch_mod, Merger::epoch_committed_txn_num_local_vec),
//                     Merger::GetAllThreadLocalCountNum(epoch_mod, Merger::epoch_should_commit_txn_num_local_vec),
//                     Merger::GetAllThreadLocalCountNum(epoch_mod, Merger::epoch_record_commit_txn_num_local_vec),
//                     Merger::GetAllThreadLocalCountNum(epoch_mod, Merger::epoch_record_committed_txn_num_local_vec),
//                     EpochMessageReceiveHandler::shard_should_receive_pack_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::shard_received_pack_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::shard_should_receive_txn_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::GetAllThreadLocalCountNum(epoch_mod, EpochMessageReceiveHandler::shard_received_txn_num_local_vec),
//                     EpochMessageReceiveHandler::remote_server_should_receive_pack_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::remote_server_received_pack_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::remote_server_should_receive_txn_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::GetAllThreadLocalCountNum(epoch_mod, EpochMessageReceiveHandler::remote_server_received_txn_num_local_vec),
//                     EpochMessageReceiveHandler::backup_should_receive_pack_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::backup_received_pack_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::backup_should_receive_txn_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::GetAllThreadLocalCountNum(epoch_mod, EpochMessageReceiveHandler::backup_received_txn_num_local_vec),
//                     EpochMessageReceiveHandler::insert_set_should_receive_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::insert_set_received_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::abort_set_should_receive_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::abort_set_received_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::shard_received_ack_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::backup_received_ack_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::insert_set_received_ack_num.GetCount(epoch_mod),
//                     EpochMessageReceiveHandler::abort_set_received_ack_num.GetCount(epoch_mod),
//                     (uint64_t)0, now_to_us(),
//                     MessageQueue::client_send_message_num.load(), MessageQueue::client_receive_message_num.load(),
//                     EpochMessageSendHandler::TotalTxnNum.load()
//                         )
//
//              << PrintfToString("\n Epoch: %lu ClearEpoch: %lu, SuccessTxnNumber %lu, ToTalSuccessLatency %lu, SuccessAvgLatency %lf, TotalCommitTxnNum %lu, TotalCommitlatency %lu, TotalCommitAvglatency %lf \n",
//                     epoch_, clear_epoch.load(),
//                     EpochMessageSendHandler::TotalSuccessTxnNUm.load(), EpochMessageSendHandler::TotalSuccessLatency.load(),
//                     (((double)EpochMessageSendHandler::TotalSuccessLatency.load()) / ((double)EpochMessageSendHandler::TotalSuccessTxnNUm.load())),
//                     EpochMessageSendHandler::TotalTxnNum.load(),///receive from client
//                     EpochMessageSendHandler::TotalLatency.load(),
//                     (((double)EpochMessageSendHandler::TotalLatency.load()) / ((double)EpochMessageSendHandler::TotalTxnNum.load())))
//              << PrintfToString("EpochMerge MergeTxnNumber %lu, ToTalMergeLatency %lu, FailedReadCheckTxnNum %lu, MergeAvgLatency %lf \n",
//                     Merger::total_merge_txn_num.load(), Merger::total_merge_latency.load(),
//                     Merger::total_read_version_check_failed_txn_num.load(),
//                     (((double)Merger::total_merge_latency.load()) / ((double)Merger::total_merge_txn_num.load())))
//              << PrintfToString("EpochCommit CommitTxnNumber %lu, ToTalMCommitLatency %lu, FailedTxnNUm %lu, SuccessTxnNum %lu, TotalCommitAvgLatency %lf SuccessCommitLatency %lf\n",
//                     Merger::total_commit_txn_num.load(), Merger::total_commit_latency.load(),
//                     Merger::total_failed_txn_num.load(), Merger::success_commit_txn_num.load(),
//                     (((double)Merger::total_commit_latency.load()) / ((double)Merger::total_commit_txn_num.load())),
//                     (((double)Merger::success_commit_txn_num.load()) / ((double)Merger::success_commit_latency.load())))
//              << PrintfToString("Storage Push Down TiKVTotalTxnNumber %lu ,TiKVSuccessTxnNum %lu, TiKVFailedTxnNum %lu \n",
//                     TiKV::total_commit_txn_num.load(), TiKV::total_commit_txn_num.load() - TiKV::failed_commit_txn_num.load(), TiKV::failed_commit_txn_num.load())
//              << "**************************************************************************************************************************************************************************************\n";
}

bool CheckRedoLogPushDownState() {
    auto i = redo_log_epoch.load();
    shared_ptr<proto::Transaction> empty_txn_ptr;
    while(!EpochManager::IsTimerStop()) {
        while(i >= commit_epoch.load()) usleep(logical_sleep_timme);
        while(!EpochManager::IsRecordCommitted(i)) usleep(logical_sleep_timme);
        while(!RedoLoger::CheckPushDownComplete(i)) usleep(logical_sleep_timme);
        EpochMessageSendHandler::SendTxnToServer(i,i, empty_txn_ptr, proto::TxnType::EpochLogPushDownComplete);
        while(!EpochMessageReceiveHandler::IsRedoLogPushDownACKReceiveComplete(i)) usleep(logical_sleep_timme);
        {
            if(i % TaasContext::print_mode_size == 0)
//                LOG(INFO) << PrintfToString("=-=-=-=-=-=-= 完成一个Epoch的 Log Push Down Epoch: %8lu ClearEpoch: %8lu =-=-=-=-=-=-=\n", commit_epoch.load(), i);

            EpochManager::ClearMergeEpochState(i); //清空当前epoch的merge信息
            EpochMessageReceiveHandler::StaticClear(i);//清空current epoch的receive cache num信息
            TransactionCache::EpochCacheClear(i);
            ThreadCounters::StaticClear(i);
            RedoLoger::ClearRedoLog(i);
            redo_log_epoch.fetch_add(1);
            clear_epoch.fetch_add(1);
            EpochManager::AddPushDownEpoch();
            i ++;
        }
    }
    return true;
}

void EpochLogicalTimerManagerThreadMain() {
}

void EpochPhysicalTimerManagerThreadMain() {
    InitEpochTimerManager();
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    //==========同步============
    zmq::message_t message;
    zmq::context_t context(1);
    zmq::socket_t request_puller(context, ZMQ_PULL);
    request_puller.bind("tcp://*:5546");
    request_puller.recv(&message);
    gettimeofday(&start_time, nullptr);
    start_time_ll = start_time.tv_sec * 1000000 + start_time.tv_usec;
//    if(TaasContext::is_sync_start && TaasContext::taasMode != TaasMode::TwoPC) {
//        auto sleep_time_temp = static_cast<uint64_t>((((start_time.tv_sec / 60) + 1) * 60) * 1000000);
//        usleep(sleep_time_temp - start_time_ll);
//        gettimeofday(&start_time, nullptr);
//        start_time_ll = start_time.tv_sec * 1000000 + start_time.tv_usec;
//    }
    EpochManager::SetPhysicalEpoch(1);
    EpochManager::SetLogicalEpoch(1);
    auto epoch_ = EpochManager::GetPhysicalEpoch();
    auto logical = EpochManager::GetLogicalEpoch();
    test_start.store(true);
    is_epoch_advance_started.store(true);

    printf("=============  EpochTimerManager 同步完成，数据库开始正常运行 ============= \n");

    auto startTime = now_to_us();
    if(TaasContext::taasMode == TaasMode::TwoPC) {
        while(!EpochManager::IsTimerStop()){
            usleep(10000);
        }
    }
    else {
        while(!EpochManager::IsTimerStop()){
            usleep(GetSleeptime());
            EpochManager::AddPhysicalEpoch();
            epoch_ ++;
            logical = EpochManager::GetLogicalEpoch();
            if(epoch_ % TaasContext::print_mode_size == 0) {
//                LOG(INFO) << "============= Start Physical Epoch : " << epoch_ << ", logical : " << logical << "Time : " << now_to_us() - startTime << "=============\n";
                OUTPUTLOG("============= Epoch INFO ============= ", logical);
            }
            EpochManager::EpochCacheSafeCheck();
        }
        OUTPUTLOG("============= Epoch INFO ============= ", logical);
//        LOG(INFO) << "Start Physical epoch : " << epoch_ << ", logical : " << logical << "Time : " << now_to_us() - startTime;
    }
    printf("EpochTimerManager End!!!\n");
}


void EpochManager::SetServerOnLine(uint64_t& epoch_, const std::string& ip) {
    for(int i = 0; i < (int)TaasContext::kServerIp.size(); i++) {
        if(ip == TaasContext::kServerIp[i]) {
            server_state.SetCount(epoch_, i, 1);
            EpochMessageReceiveHandler::shard_should_receive_pack_num.Clear(epoch_, 1);///relate to server state
            EpochMessageReceiveHandler::backup_should_receive_pack_num.Clear(epoch_, 1);///relate to server state
            EpochMessageReceiveHandler::insert_set_should_receive_num.Clear(epoch_, 1);///relate to server state
            EpochMessageReceiveHandler::abort_set_should_receive_num.Clear(epoch_, 1);///relate to server state
        }
    }
}

void EpochManager::SetServerOffLine(uint64_t& epoch_, const std::string& ip) {
    for(int i = 0; i < (int)TaasContext::kServerIp.size(); i++) {
        if(ip == TaasContext::kServerIp[i]) {
            server_state.SetCount(epoch_, i, 0);
            EpochMessageReceiveHandler::shard_should_receive_pack_num.Clear(epoch_, 0);///relate to server state
            EpochMessageReceiveHandler::backup_should_receive_pack_num.Clear(epoch_, 0);///relate to server state
            EpochMessageReceiveHandler::insert_set_should_receive_num.Clear(epoch_, 0);///relate to server state
            EpochMessageReceiveHandler::abort_set_should_receive_num.Clear(epoch_, 0);///relate to server state
        }
    }
}


class ShardEpochManager {
public:
    static bool CheckEpochMergeState();
    static bool CheckEpochAbortMergeState();
    static bool CheckEpochCommitState();

    static void EpochLogicalTimerManagerThreadMain();
};

static uint64_t last_total_commit_txn_num = 0;

void ShardEpochManager::EpochLogicalTimerManagerThreadMain() {

    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    uint64_t epoch = 1;
    OUTPUTLOG("===== Start Epoch的合并 ===== ", epoch);
    util::thread_pool_light workers(5);
    //        while(!EpochManager::IsInitOK() || EpochManager::GetPhysicalEpoch() < 10) usleep(sleep_time);
    while(!EpochManager::IsTimerStop()){

        while(epoch >= EpochManager::GetPhysicalEpoch()) std::this_thread::yield();;
        auto time1 = now_to_us();
        //                LOG(INFO) << "**** Start Epoch Merge Epoch : " << epoch << "****\n";
        while(!EpochMessageReceiveHandler::CheckEpochClientTxnHandleComplete(epoch)) std::this_thread::yield();;

        while(!EpochMessageReceiveHandler::CheckEpochShardReceiveComplete(epoch)) std::this_thread::yield();;

        while(!EpochMessageReceiveHandler::CheckEpochShardTxnHandleComplete(epoch)) std::this_thread::yield();;
        auto time2 = now_to_us();
        while(!EpochMessageReceiveHandler::CheckEpochBackUpComplete(epoch)) std::this_thread::yield();;

        while(!EpochMessageReceiveHandler::CheckEpochRemoteServerReceiveComplete(epoch)) std::this_thread::yield();;
        auto time3 = now_to_us();

        while(!Merger::CheckEpochMergeComplete(epoch)) std::this_thread::yield();;
        EpochManager::SetEpochMergeComplete(epoch, true);
        merge_epoch.fetch_add(1);
        auto time4 = now_to_us();

        while(!EpochMessageReceiveHandler::CheckEpochAbortSetMergeComplete(epoch)) std::this_thread::yield();;
        EpochManager::SetAbortSetMergeComplete(epoch, true);
        abort_set_epoch.fetch_add(1);
        auto time5 = now_to_us();

        while(!Merger::CheckEpochCommitComplete(epoch)) std::this_thread::yield();;
        EpochManager::SetCommitComplete(epoch, true);
        auto time6 = now_to_us();
        while(!Merger::CheckEpochRecordCommitted(epoch)) std::this_thread::yield();;
        EpochManager::SetRecordCommitted(epoch, true);
        auto time7 = now_to_us();

        ///change to sync
        //            while(redo_log_epoch.load() < epoch) std::this_thread::yield();;

        while(!Merger::CheckEpochResultReturned(epoch)) std::this_thread::yield();;
        EpochManager::SetResultReturned(epoch, true);
        auto time8 = now_to_us();
        commit_epoch.fetch_add(1);
        EpochManager::AddLogicalEpoch();
        auto epoch_commit_success_txn_num = ThreadCounters::GetAllThreadLocalCountNum(epoch,
            ThreadCounters::epoch_record_committed_txn_num_local_vec);
        total_commit_txn_num += epoch_commit_success_txn_num;///success
        if(epoch % TaasContext::print_mode_size == 0) {
//            LOG(INFO) << PrintfToString(
//                             "************ 完成一个Epoch的合并 Physical Epoch %lu, Logical Epoch: %lu, Local EpochSuccessCommitTxnNum: %lu,TotalSuccessTxnNum: %lu, EpochCommitTxnNum: %lu ",
//                             EpochManager::GetPhysicalEpoch(), epoch, epoch_commit_success_txn_num, total_commit_txn_num,
//                             EpochMessageSendHandler::TotalTxnNum.load() - last_total_commit_txn_num)
//                      << "\n Time Cost  Epoch: " << epoch
//                      << " ,Shard Transmit Txn cost: " << time2 - time1
//                      << " ,Validate and Transmit write set Txn cost: " << time3 - time2
//                      << " ,Merge time cost : " << time4 - time3
//                      << " ,Abort Set Merge time cost : " << time5 - time4
//                      << " ,Commit time cost : " << time6 - time5
//                      << " ,Log time cost : " << time7 - time6
//                      << " ,Result time cost : " << time8 - time7
//                      << " Total Time Cost **** " << time8 - time1
//                      << " ****end\n";
            OUTPUTLOG("===== Logical Start Epoch的合并 ===== ", epoch);
            MOT_LOG_INFO("TaaS Logical Start Epoch的合并 Physical %lu, Logical %lu, Log %lu", EpochManager::GetPhysicalEpoch(), epoch, EpochManager::GetPushDownEpoch());
        }
        epoch ++;
        last_total_commit_txn_num = EpochMessageSendHandler::TotalTxnNum.load();
    }
}



///Worker

void WorkerFroMOTStorageThreadMain(uint64_t id) {
    std::string name = "EpochMOT";
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    MOT_LOG_INFO("TaaS Thread" + name);
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    MOT mot;
    mot.Init();
    while (!EpochManager::IsTimerStop()) {
        mot.SendTransactionToDB_Usleep();
    }
}

void WorkerForStorageSendMOTThreadMain() {
    std::string name = "EpochMOTStorage";
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    MOT_LOG_INFO("TaaS Thread" + name);
    SendToMOTStorageThreadMain();
}

void WorkerFroMessageThreadMain(uint64_t id) {/// handle client txn
    std::string name = "TxnMessage-" + std::to_string(id);
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    EpochMessageReceiveHandler receiveHandler;
    MOT_LOG_INFO("TaaS Thread" + name);
    class TwoPC twoPC;
    while(init_ok_num.load() < 5) usleep(sleep_time);
    receiveHandler.Init(id);
    Taas::TwoPC::Init(id);
    init_ok_num.fetch_add(1);
    //        bool sleep_flag;
    //        auto safe_length = TaasContext::kCacheMaxLength / 10;
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    while(!EpochManager::IsTimerStop()){
        switch(TaasContext::taasMode) {
            case TaasMode::MultiModel :
            case TaasMode::MultiMaster :
            case TaasMode::Shard : {
                while(!EpochManager::IsTimerStop()) {
                    //                        sleep_flag = true;
                    //                        receiveHandler.TryHandleReceivedControlMessage();
                    //                        if( EpochManager::GetLogicalEpoch() + safe_length > EpochManager::GetPhysicalEpoch() ) /// avoid task backlogs, stop handling txn comes from the client
                    //                            receiveHandler.TryHandleReceivedMessage();
                    //
                    //                        sleep_flag = sleep_flag & receiveHandler.sleep_flag;
                    //
                    //                        if(sleep_flag) usleep(merge_sleep_time);
                    while(!EpochManager::IsTimerStop())
                        receiveHandler.HandleReceivedMessage();
                }
                break;
            }
            case TaasMode::TwoPC : {
                while(!EpochManager::IsTimerStop()) {
                    twoPC.HandleClientMessage();        // test
                                                  //                        twoPC.HandleReceivedMessage();

                }
                break;
            }
        }
    }
}

void WorkerFroMessageEpochThreadMain(uint64_t id) {/// handle message
    std::string name = "EpochMessage-" + std::to_string(id);
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    MOT_LOG_INFO("TaaS Thread" + name);
    EpochMessageReceiveHandler receiveHandler;
    class TwoPC twoPC;
    while(init_ok_num.load() < 5) usleep(sleep_time);
    receiveHandler.Init(id);
    Taas::TwoPC::Init(id);
    init_ok_num.fetch_add(1);
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    while(!EpochManager::IsTimerStop()){
        switch(TaasContext::taasMode) {
            case TaasMode::MultiModel :
            case TaasMode::MultiMaster :
            case TaasMode::Shard : {
                while(!EpochManager::IsTimerStop()) {
                    receiveHandler.HandleReceivedControlMessage();
                }
                break;
            }
            case TaasMode::TwoPC : {
                while(!EpochManager::IsTimerStop()) {
                    twoPC.HandleReceivedMessage();      // test
                }
                break;
            }
        }
    }
}

void WorkerForClientListenThreadMain() {
    std::string name = "EpochClientListen";
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    MOT_LOG_INFO("TaaS Thread" + name);
    SetCPU();
    ListenClientThreadMain();
}

void WorkerForClientSendThreadMain() {
    std::string name = "EpochClientSend";
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    MOT_LOG_INFO("TaaS Thread" + name);
    SetCPU();
    SendClientThreadMain();
}

void WorkerForServerListenThreadMain() {
    std::string name = "EpochServerListen";
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    MOT_LOG_INFO("TaaS Thread" + name);
    SetCPU();
    ListenServerThreadMain();
}

void WorkerForServerListenThreadMain_Epoch() {
    std::string name = "EpochServerListen";
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    MOT_LOG_INFO("TaaS Thread" + name);
    SetCPU();
    ListenServerThreadMain_Sub();
}

void WorkerForServerSendThreadMain() {
    std::string name = "EpochServerSend";
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    MOT_LOG_INFO("TaaS Thread" + name);
    SetCPU();
    SendServerThreadMain();
}

void WorkerForServerSendPUBThreadMain() {
    std::string name = "EpochClientSend";
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    MOT_LOG_INFO("TaaS Thread" + name);
    SetCPU();
    SendServerPUBThreadMain();
}

void EpochWorkerThreadMain(uint64_t id) {
    std::string name = "TaaSMerger-" + std::to_string(id);
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    MOT_LOG_INFO("TaaS Thread" + name);
    Merger merger;
    EpochMessageReceiveHandler receiveHandler;
    class TwoPC two_pc;
    while(init_ok_num.load() < 5) usleep(sleep_time);
    //        LOG(INFO) << "start worker init" << id;
    merger.MergeInit(id);
    receiveHandler.Init(id);
    Taas::TwoPC::Init(id);
    bool sleep_flag;
    init_ok_num.fetch_add(1);
    //        LOG(INFO) << "finish worker init" << id;
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    SetCPU();
    switch(TaasContext::taasMode) {
        case TaasMode::MultiModel :
        case TaasMode::MultiMaster :
        case TaasMode::Shard : {
            while(!EpochManager::IsTimerStop()) {
                sleep_flag = true;

                merger.epoch = EpochManager::GetLogicalEpoch();
                merger.epoch_mod = merger.epoch % TaasContext::kCacheMaxLength;
                while (TransactionCache::epoch_read_validate_queue[merger.epoch_mod]->try_dequeue(
                    merger.txn_ptr)) {
                    if (merger.txn_ptr != nullptr && merger.txn_ptr->txn_type() != proto::TxnType::NullMark) {
                        merger.ReadValidate();
                        merger.txn_ptr.reset();
                        sleep_flag = false;
                    }
                }

                while (!EpochManager::IsEpochMergeComplete(merger.epoch) && TransactionCache::epoch_merge_queue[merger.epoch_mod]->try_dequeue(merger.txn_ptr)) {
                    if (merger.txn_ptr != nullptr &&
                        merger.txn_ptr->txn_type() != proto::TxnType::NullMark) {
                        merger.Merge();
                        merger.txn_ptr.reset();
                        sleep_flag = false;
                    }
                }

                while (EpochManager::IsAbortSetMergeComplete(merger.epoch) &&
                       !EpochManager::IsCommitComplete(merger.epoch) &&
                       TransactionCache::epoch_commit_queue[merger.epoch_mod]->try_dequeue(
                           merger.txn_ptr)) {
                    if (merger.txn_ptr != nullptr &&
                        merger.txn_ptr->txn_type() != proto::TxnType::NullMark) {
                        merger.Commit();
                        merger.txn_ptr.reset();
                        sleep_flag = false;
                    }
                }

                while (EpochManager::IsAbortSetMergeComplete(merger.epoch) &&
                       !EpochManager::IsRecordCommitted(merger.epoch) &&
                       TransactionCache::epoch_redo_log_queue[merger.epoch_mod]->try_dequeue(
                           merger.txn_ptr)) {
                    if (merger.txn_ptr != nullptr && merger.txn_ptr->txn_type() !=
                                                         proto::TxnType::NullMark) { /// only local txn do redo log
                        merger.RedoLog();
                        merger.txn_ptr.reset();
                        sleep_flag = false;
                    }
                }

                while (EpochManager::IsRecordCommitted(merger.epoch) &&
                       !EpochManager::IsResultReturned(merger.epoch) &&
                       TransactionCache::epoch_result_return_queue[merger.epoch_mod]->try_dequeue(
                           merger.txn_ptr)) {
                    if (merger.txn_ptr != nullptr && merger.txn_ptr->txn_type() !=
                                                         proto::TxnType::NullMark) { /// only local txn do redo log
                        merger.ResultReturn();
                        merger.txn_ptr.reset();
                        sleep_flag = false;
                    }
                }

                if (sleep_flag) usleep(merge_sleep_time);
            }
            break;
        }
        case TaasMode::TwoPC : {
            two_pc.HandleClientMessage();
            break;
        }
    }
}


void WorkerForPhysicalThreadMain() {
    std::string name = "EpochPhysical";
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    SetCPU();
    MOT_LOG_INFO("TaaS Thread" + name);
    EpochPhysicalTimerManagerThreadMain();
}

void WorkerForLogicalThreadMain() {
    std::string name = "EpochLogical";
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    SetCPU();
    MOT_LOG_INFO("TaaS Thread" + name);
    ShardEpochManager::EpochLogicalTimerManagerThreadMain();
}

void WorkerForEpochControlMessageThreadMain() {
    SetCPU();
    MOT_LOG_INFO("WorkerForEpochControlMessageThreadMain TaaS");
    while(!EpochManager::IsInitOK() || EpochManager::GetPhysicalEpoch() < 10) usleep(sleep_time);
    while(!EpochManager::IsTimerStop()){
        switch(TaasContext::taasMode) {
            case TaasMode::MultiModel :
            case TaasMode::MultiMaster :
            case TaasMode::Shard : {
                uint64_t local_server_id = TaasContext::txn_node_ip_index;
                uint64_t shard_epoch = 1, remote_server_epoch = 1, abort_send_epoch = 1, server_num = TaasContext::kTxnNodeNum;
                bool sleep_flag;
                while(!EpochManager::IsInitOK()) usleep(sleep_time);
                while(!EpochManager::IsTimerStop()) {
                    sleep_flag = true;
                    //                        while(shard_epoch >= EpochManager::GetPhysicalEpoch()) {
                    //                            usleep(100);
                    //                        }
                    if (shard_epoch < EpochManager::GetPhysicalEpoch() && EpochMessageReceiveHandler::CheckEpochClientTxnHandleComplete(shard_epoch)) {
                        EpochMessageSendHandler::SendEpochShardEndMessage(local_server_id, shard_epoch, server_num);
                        //                            LOG(INFO) << "Send EpochShardEndFlag epoch " << shard_epoch;
                        shard_epoch ++;
                        sleep_flag = false;
                    }

                    if(remote_server_epoch < shard_epoch &&
                        EpochMessageReceiveHandler::CheckEpochShardReceiveComplete(remote_server_epoch) &&
                        EpochMessageReceiveHandler::CheckEpochShardTxnHandleComplete(remote_server_epoch)) {
                        EpochMessageSendHandler::SendEpochRemoteServerEndMessage(local_server_id, remote_server_epoch, server_num);
                        //                            LOG(INFO) << "Send SendEpochRemoteServerEndMessage epoch " << remote_server_epoch;
                        remote_server_epoch ++;
                        sleep_flag = false;
                    }

                    if(abort_send_epoch < remote_server_epoch && EpochManager::IsEpochMergeComplete(abort_send_epoch)) {
                        EpochMessageSendHandler::SendAbortSet(local_server_id, abort_send_epoch);
                        //                            LOG(INFO) << "Send SendAbortSet epoch " << abort_send_epoch;
                        abort_send_epoch ++;
                        sleep_flag = false;
                    }

                    //
                    //                        if(EpochManager::IsEpochMergeComplete(abort_send_epoch)) {
                    //                            EpochMessageSendHandler::SendAbortSet(local_server_id, abort_send_epoch, TaasContext::kCacheMaxLength);
                    //                            abort_send_epoch ++;
                    //                            sleep_flag = false;
                    //                        }
                    //                        if(sleep_flag) usleep(100);
                    if(sleep_flag) std::this_thread::yield();
                }
                break;
            }
            case TaasMode::TwoPC : {
                //
                break;
            }
        }
    }
}

void WorkerForLogicalRedoLogPushDownCheckThreadMain() {
    SetCPU();
    MOT_LOG_INFO("TaaS Thread" + name);
    while(!EpochManager::IsInitOK()) usleep(sleep_time);
    while(!EpochManager::IsTimerStop()){
        switch(TaasContext::taasMode) {
            case TaasMode::MultiModel :
            case TaasMode::MultiMaster :
            case TaasMode::Shard : {
                CheckRedoLogPushDownState();
                break;
            }
            case TaasMode::TwoPC : {
                //                    TwoPhaseCommitManager::TwoPhaseCommitManagerThreadMain(ctx);
            }
        }
        //            CheckRedoLogPushDownState(ctx);
    }
}


int TaaSmain() {
    auto res = TaasContext::Print();
    printf("%s\n", res.c_str());
    std::vector<std::unique_ptr<std::thread>> threads;
    int cnt = 0;

    auto server_num = TaasContext::kTxnNodeNum,
         shard_num = TaasContext::kShardNum,
         replica_num = TaasContext::kReplicaNum,
         local_server_id = TaasContext::txn_node_ip_index,
         max_length = TaasContext::kCacheMaxLength;
    std::vector<std::vector<bool>> is_local_shard;
    is_local_shard.resize(server_num);
    for(auto &i : is_local_shard) {
        i.resize(shard_num);
    }
    for(uint64_t server_id = 0; server_id < server_num; server_id ++) {
        for(uint64_t i = 0; i < shard_num; i ++) {
            for(uint64_t j = 0; j < replica_num; j ++ ) {
                if((i + server_num + j) % server_num == server_id) {
                    is_local_shard[server_id][i] = true;
                }
            }
        }
    }
    std::string s = "";
    for(uint64_t server_id = 0; server_id < server_num; server_id ++) {
        for(uint64_t i = 0; i < shard_num; i ++) {
            if(is_local_shard[server_id][i]) {
                s += "1";
            }
            else {
                s += "0";
            }
        }
        s += "\n";
    }


    if(TaasContext::server_type == ServerMode::Taas) {  /// TaaS servers
        EpochManager epochManager;
        threads.push_back(std::make_unique<std::thread>(WorkerForPhysicalThreadMain));
        cnt++;
        threads.push_back(std::make_unique<std::thread>(WorkerForLogicalThreadMain));
        cnt++;
        threads.push_back(std::make_unique<std::thread>(WorkerForLogicalRedoLogPushDownCheckThreadMain));
        cnt++;
        threads.push_back(std::make_unique<std::thread>(WorkerForEpochControlMessageThreadMain));
        cnt++;

        for (int i = 0; i < (int)TaasContext::kEpochTxnThreadNum; i++) {  /// handle client txn
            threads.push_back(std::make_unique<std::thread>(WorkerFroMessageThreadMain, i));
            cnt++;  /// client txn message
        }
        for (int i = 0; i < (int)TaasContext::kEpochMessageThreadNum; i++) {  /// handle remote server message
            threads.push_back(std::make_unique<std::thread>(WorkerFroMessageEpochThreadMain, i));
            cnt++;  /// epoch message
        }
        for (int i = 0; i < (int)TaasContext::kMergeThreadNum; i++) {
            //                threads.push_back(std::make_unique<std::thread>(WorkerFroMergeThreadMain, i));  cnt++;///merge & commit
            threads.push_back(std::make_unique<std::thread>(EpochWorkerThreadMain, i));
            cnt++;
        }

        threads.push_back(std::make_unique<std::thread>(WorkerForClientListenThreadMain));
        cnt++;  /// client
        threads.push_back(std::make_unique<std::thread>(WorkerForClientSendThreadMain));
        cnt++;

        threads.push_back(std::make_unique<std::thread>(WorkerForServerListenThreadMain));
        cnt++;  /// Server
        threads.push_back(std::make_unique<std::thread>(WorkerForServerListenThreadMain_Epoch));
        cnt++;
        threads.push_back(std::make_unique<std::thread>(WorkerForServerSendThreadMain));
        cnt++;
        threads.push_back(std::make_unique<std::thread>(WorkerForServerSendPUBThreadMain));
        cnt++;

        /// Storage
        if (StorageContext::is_mot_enable) {
            threads.push_back(std::make_unique<std::thread>(WorkerForStorageSendMOTThreadMain));
            cnt++;
            for (int i = 0; i < (int)StorageContext::kMOTThreadNum; i++) {
                threads.push_back(std::make_unique<std::thread>(WorkerFroMOTStorageThreadMain, i));
                cnt++;  /// mot push down
            }
        }
    }

    MOT_LOG_INFO("Start TaaS");

    if(TaasContext::kDurationTime_us != 0) {
        while(!test_start.load()) usleep(sleep_time);
        usleep(TaasContext::kDurationTime_us);
        EpochManager::SetTimerStop(true);
    }
    //        else {
    //            std::signal(SIGINT, signalHandler);
    //        }
    for(auto &i : threads) {
        i->join();
    }
    google::ShutdownGoogleLogging();
    std::cout << "============================================================================" << std::endl;
    std::cout << "=====================              END                 =====================" << std::endl;
    std::cout << "============================================================================" << std::endl;
    return 0;
}


int TaaS_Start(){

    Context ctx;
    ctx.Init();

    auto server_num = TaasContext::kTxnNodeNum,
         shard_num = TaasContext::kShardNum,
         replica_num = TaasContext::kReplicaNum,
         local_server_id = TTaasContext::txn_node_ip_index,
         max_length = TaasContext::kCacheMaxLength;
    std::vector<std::vector<bool>> is_local_shard;
    is_local_shard.resize(server_num);
    for(auto &i : is_local_shard) {
        i.resize(shard_num);
    }
    for(uint64_t server_id = 0; server_id < server_num; server_id ++) {
        for(uint64_t i = 0; i < shard_num; i ++) {
            for(uint64_t j = 0; j < replica_num; j ++ ) {
                if((i + server_num + j) % server_num == server_id) {
                    is_local_shard[server_id][i] = true;
                }
            }
        }
    }
    std::string s = "";
    for(uint64_t server_id = 0; server_id < server_num; server_id ++) {
        for(uint64_t i = 0; i < shard_num; i ++) {
            if(is_local_shard[server_id][i]) {
                s += "1";
            }
            else {
                s += "0";
            }
        }
        s += "\n";
    }

    printf("============================\n");
    printf("shard replication statues:\n%s", s.c_str());
    printf("============================\n");
    Taas::main();
}