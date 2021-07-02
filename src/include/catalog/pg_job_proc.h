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
 * ---------------------------------------------------------------------------------------
 * 
 * pg_job_proc.h
 *        definition of the system "job task" relation (pg_job_proc)
 *        along with the relation's initial contents.
 * 
 * 
 * IDENTIFICATION
 *        src/include/catalog/pg_job_proc.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef PG_JOB_PROC_H
#define PG_JOB_PROC_H

#include "catalog/genbki.h"

/*-------------------------------------------------------------------------
 *        pg_job_proc definition.  cpp turns this into
 *        typedef struct FormData_pg_job_proc
 *-------------------------------------------------------------------------
 */
#define PgJobProcRelationId 9023
#define PgJobProcRelation_Rowtype_Id 11659

CATALOG(pg_job_proc,9023) BKI_SHARED_RELATION  BKI_SCHEMA_MACRO
{
    int4 job_id;   /* foreign key, reference to pg_job.job_id */
    text what;     /* Body of the anonymous PL/pgSQL block that the job executes */
} FormData_pg_job_proc;

/*-------------------------------------------------------------------------
 *        Form_pg_job_proc corresponds to a pointer to a tuple with
 *        the format of pg_job_proc relation.
 *-------------------------------------------------------------------------
 */
typedef FormData_pg_job_proc* Form_pg_job_proc;

/*-------------------------------------------------------------------------
 *        compiler constants for pg_job_schedule
 *-------------------------------------------------------------------------
 */
#define Natts_pg_job_proc         2
#define Anum_pg_job_proc_job_id   1
#define Anum_pg_job_proc_what     2

#endif /* PG_JOB_PROC_H */

