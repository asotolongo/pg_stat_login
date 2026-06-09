/*-------------------------------------------------------------------------
 *
 * pg_stat_login.c
 *   Track PostgreSQL login statistics (successes and failures)
 *   per user and database using the ClientAuthentication_hook.
 *
 * Uses the fixed-amount Custom Cumulative Statistics API introduced in
 * PostgreSQL 18, following the same pattern as pg_stat_log https://github.com/fabriziomello/pg_stat_log/ 
 *
 * Stats kind ID 30 — registered at:
 *   https://wiki.postgresql.org/wiki/CustomCumulativeStats
 *
 * Configuration GUCs:
 *   pg_stat_login.enable      - enable/disable collection (default: on)
 *   pg_stat_login.max_entries - max (user, db) pairs tracked (default: 1000)
 *
 * SQL interface:
 *   pg_stat_login()       - login statistics as a set of rows
 *   pg_stat_login_info()  - capacity and metadata
 *   pg_stat_login_reset() - clears all accumulated statistics
 *   pg_stat_logins        - view over pg_stat_login()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "libpq/auth.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC_EXT(
	.name = "pg_stat_login",
	.version = PG_VERSION
);

/*
 * Custom stats kind ID — registered at
 * https://wiki.postgresql.org/wiki/CustomCumulativeStats
 *
 */
#define PGSTAT_KIND_LOGIN          30
#define PGSTAT_LOGIN_MODULE_NAME   "pg_stat_login"
#define PGSTAT_LOGIN_TRANCHE_NAME  PGSTAT_LOGIN_MODULE_NAME
#define PGSTAT_LOGIN_MAX_DEFAULT   1000
#define PGSTAT_LOGIN_MIN_ENTRIES   100

/*
 * One slot in the open-addressing hash table.
 * Keys are zero-padded to NAMEDATALEN for consistent hashing.
 */
typedef struct PlsSlot
{
	bool        used;
	char        username[NAMEDATALEN];
	char        database[NAMEDATALEN];
	int64       login_ok;
	int64       login_fail;
	TimestampTz last_login_ok;
	TimestampTz last_login_fail;
} PlsSlot;

/*
 * Stats data block — variable-length header followed by the slot array.
 * This entire struct is what gets persisted to disk (shared_data_len).
 */
typedef struct PlsData
{
	int      max_entries;
	int      num_entries;
	PlsSlot  entries[FLEXIBLE_ARRAY_MEMBER];
} PlsData;

/*
 * Shared memory wrapper.
 * LWLock + changecount fields live outside PlsData so they are not
 * serialized to disk. Only the PlsData portion (starting at 'data')
 * is saved and restored across restarts.
 */
typedef struct PlsSharedBlock
{
	LWLock      lock;        /* protects all writes */
	uint32      changecount; /* changecount protocol for readers */
	TimestampTz stats_reset; /* timestamp of last reset */
	uint64      n_dropped;   /* events dropped due to full table */
	char        data[FLEXIBLE_ARRAY_MEMBER]; /* holds one PlsData */
} PlsSharedBlock;

/* Sizes computed in _PG_init based on max_entries GUC */
static Size stats_data_size;   /* size of PlsData + slot array */
static Size shared_block_size; /* size of PlsSharedBlock + PlsData */

/* GUC variables */
static bool pls_enabled = true;
static int  pls_max = PGSTAT_LOGIN_MAX_DEFAULT;

/* Saved hook values */
static ClientAuthentication_hook_type prev_client_auth_hook = NULL;
static shmem_startup_hook_type        prev_shmem_startup_hook = NULL;

/* KindInfo descriptor — filled by _PG_init */
static PgStat_KindInfo login_stats_kind;

/* Forward declarations */
void _PG_init(void);
PG_FUNCTION_INFO_V1(pg_stat_login);
PG_FUNCTION_INFO_V1(pg_stat_login_info);
PG_FUNCTION_INFO_V1(pg_stat_login_reset);

static void pls_init_shmem_cb(void *stats);
static void pls_init_backend_cb(void);
static void pls_reset_all_cb(TimestampTz ts);
static void pls_snapshot_cb(void);
static void pls_client_auth(Port *port, int status);
static void pls_shmem_startup(void);


/* ---- Helpers ---- */

static inline PlsData *
pls_get_data(PlsSharedBlock *shmem)
{
	return (PlsData *) shmem->data;
}

/*
 * pls_compute_hash
 *   Hash a (username, database) key by treating the zero-padded char arrays
 *   as a sequence of uint32 words and combining via murmurhash32.
 *   NAMEDATALEN (64) is always a multiple of 4.
 */
static uint32
pls_compute_hash(const char *username, const char *database)
{
	const uint32 *p;
	uint32        h = 0;
	int           i;

	p = (const uint32 *) username;
	for (i = 0; i < NAMEDATALEN / 4; i++)
		h = hash_combine(h, murmurhash32(p[i]));

	p = (const uint32 *) database;
	for (i = 0; i < NAMEDATALEN / 4; i++)
		h = hash_combine(h, murmurhash32(p[i]));

	return h;
}


/* ---- CCS callbacks ---- */

/*
 * pls_init_shmem_cb
 *   Runs once in the postmaster after shared memory is allocated.
 *   Initializes LWLock, changecount, and the PlsData header.
 */
static void
pls_init_shmem_cb(void *stats)
{
	PlsSharedBlock *shmem = (PlsSharedBlock *) stats;
	PlsData        *s;

#if PG_VERSION_NUM >= 190000
	LWLockInitialize(&shmem->lock, LWLockNewTrancheId(PGSTAT_LOGIN_TRANCHE_NAME));
#else
	LWLockInitialize(&shmem->lock, LWLockNewTrancheId());
#endif
	shmem->changecount = 0;
	shmem->stats_reset = GetCurrentTimestamp();
	shmem->n_dropped   = 0;

	s              = pls_get_data(shmem);
	s->max_entries = pls_max;
	s->num_entries = 0;
	/* slot array is zeroed at shared memory allocation */
}

/*
 * pls_init_backend_cb
 *   Runs in every backend after the stats file has been loaded.
 *   Validates that the persisted max_entries matches the current GUC.
 *   If max_entries changed across a clean restart, discards stale data
 *   to prevent out-of-bounds array access.
 */
static void
pls_init_backend_cb(void)
{
	PlsSharedBlock *shmem = (PlsSharedBlock *) pgstat_get_custom_shmem_data(PGSTAT_KIND_LOGIN);
	PlsData        *s     = pls_get_data(shmem);

	if (s->max_entries == pls_max)
		return;

	elog(LOG,
		 "pg_stat_login: discarding persisted stats "
		 "(max_entries changed from %d to %d)",
		 s->max_entries, pls_max);

	LWLockAcquire(&shmem->lock, LW_EXCLUSIVE);
	pgstat_begin_changecount_write(&shmem->changecount);
	s->max_entries = pls_max;
	s->num_entries = 0;
	MemSet(s->entries, 0, (Size) pls_max * sizeof(PlsSlot));
	pgstat_end_changecount_write(&shmem->changecount);
	shmem->stats_reset = GetCurrentTimestamp();
	shmem->n_dropped   = 0;
	LWLockRelease(&shmem->lock);
}

/*
 * pls_reset_all_cb
 *   Called by pgstat_reset_of_kind(). Zeroes the slot array so that
 *   slots are reclaimed for reuse after a reset.
 */
static void
pls_reset_all_cb(TimestampTz ts)
{
	PlsSharedBlock *shmem = (PlsSharedBlock *) pgstat_get_custom_shmem_data(PGSTAT_KIND_LOGIN);
	PlsData        *s     = pls_get_data(shmem);

	LWLockAcquire(&shmem->lock, LW_EXCLUSIVE);
	pgstat_begin_changecount_write(&shmem->changecount);
	MemSet(s->entries, 0, (Size) s->max_entries * sizeof(PlsSlot));
	s->num_entries = 0;
	pgstat_end_changecount_write(&shmem->changecount);
	shmem->stats_reset = ts;
	shmem->n_dropped   = 0;
	LWLockRelease(&shmem->lock);
}

/*
 * pls_snapshot_cb
 *   Copies current stats from shared memory into the per-backend snapshot
 *   buffer using the changecount protocol for atomic reads.
 */
static void
pls_snapshot_cb(void)
{
	PlsSharedBlock *shmem = (PlsSharedBlock *) pgstat_get_custom_shmem_data(PGSTAT_KIND_LOGIN);
	PlsData        *snap  = (PlsData *) pgstat_get_custom_snapshot_data(PGSTAT_KIND_LOGIN);

	pgstat_copy_changecounted_stats(snap,
									pls_get_data(shmem),
									stats_data_size,
									&shmem->changecount);
}


/* ---- Hooks ---- */

/*
 * pls_shmem_startup
 *   Registers the LWLock tranche name in every process.
 *   On fork-based platforms backends inherit the tranche ID from the
 *   postmaster; under EXEC_BACKEND each child must re-register.
 */
static void
pls_shmem_startup(void)
{
	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

#if PG_VERSION_NUM < 190000
	{
		PlsSharedBlock *shmem = (PlsSharedBlock *) pgstat_get_custom_shmem_data(PGSTAT_KIND_LOGIN);
		LWLockRegisterTranche(shmem->lock.tranche, PGSTAT_LOGIN_TRANCHE_NAME);
	}
#endif
}

/*
 * pls_client_auth
 *   ClientAuthentication_hook implementation.
 *   Records one login event (success or failure) into the shared hash table
 *   using open-addressing with linear probing.
 *
 *   Note: ClientAuthentication() is called before pgstat_beinit() in each
 *   backend, but pgstat_get_custom_shmem_data() accesses PgStat_ShmemControl
 *   (postmaster shared memory) which is valid at this point.
 */
static void
pls_client_auth(Port *port, int status)
{
	PlsSharedBlock *shmem;
	PlsData        *s;
	char            username[NAMEDATALEN];
	char            database[NAMEDATALEN];
	uint32          hash;
	uint32          idx;
	TimestampTz     now;
	int             probe;

	/* Always chain to the previous hook first */
	if (prev_client_auth_hook)
		prev_client_auth_hook(port, status);

	if (!pls_enabled)
		return;

	/* MyProc is set by InitProcess() before ClientAuthentication() fires */
	if (!MyProc)
		return;

	if (port->user_name == NULL || port->database_name == NULL)
		return;

	/* Zero-pad so NAMEDATALEN-wide comparison is unambiguous */
	memset(username, 0, sizeof(username));
	memset(database, 0, sizeof(database));
	strlcpy(username, port->user_name, NAMEDATALEN);
	strlcpy(database, port->database_name, NAMEDATALEN);

	now  = GetCurrentTimestamp();
	hash = pls_compute_hash(username, database);

	shmem = (PlsSharedBlock *) pgstat_get_custom_shmem_data(PGSTAT_KIND_LOGIN);

	LWLockAcquire(&shmem->lock, LW_EXCLUSIVE);

	s   = pls_get_data(shmem);
	idx = hash % (uint32) s->max_entries;

	for (probe = 0; probe < s->max_entries; probe++)
	{
		uint32   pos  = (idx + (uint32) probe) % (uint32) s->max_entries;
		PlsSlot *slot = &s->entries[pos];

		if (!slot->used)
		{
			/* Empty slot — create a new entry if there is still capacity */
			if (s->num_entries < s->max_entries)
			{
				pgstat_begin_changecount_write(&shmem->changecount);
				slot->used = true;
				memcpy(slot->username, username, NAMEDATALEN);
				memcpy(slot->database, database, NAMEDATALEN);
				if (status == STATUS_OK)
				{
					slot->login_ok       = 1;
					slot->last_login_ok  = now;
				}
				else
				{
					slot->login_fail      = 1;
					slot->last_login_fail = now;
				}
				s->num_entries++;
				pgstat_end_changecount_write(&shmem->changecount);
			}
			else
				shmem->n_dropped++;
			break;
		}

		/* Check if this slot matches our key */
		if (memcmp(slot->username, username, NAMEDATALEN) == 0 &&
			memcmp(slot->database, database, NAMEDATALEN) == 0)
		{
			pgstat_begin_changecount_write(&shmem->changecount);
			if (status == STATUS_OK)
			{
				slot->login_ok++;
				slot->last_login_ok = now;
			}
			else
			{
				slot->login_fail++;
				slot->last_login_fail = now;
			}
			pgstat_end_changecount_write(&shmem->changecount);
			break;
		}
	}

	/* All slots occupied by different keys */
	if (probe == s->max_entries)
		shmem->n_dropped++;

	LWLockRelease(&shmem->lock);
}


/* ---- Module load ---- */

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_login must be loaded via shared_preload_libraries")));

	DefineCustomBoolVariable("pg_stat_login.enable",
							 "Enable pg_stat_login statistics collection.",
							 NULL,
							 &pls_enabled,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * max_entries is PGC_POSTMASTER because it determines the shared memory
	 * block size which is fixed at postmaster startup.
	 */
	DefineCustomIntVariable("pg_stat_login.max_entries",
							"Maximum number of (user, database) pairs to track.",
							NULL,
							&pls_max,
							PGSTAT_LOGIN_MAX_DEFAULT,
							PGSTAT_LOGIN_MIN_ENTRIES,
							INT_MAX / 2,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	MarkGUCPrefixReserved("pg_stat_login");

	/* Compute sizes based on the max_entries GUC value */
	stats_data_size   = offsetof(PlsData, entries) +
						(Size) pls_max * sizeof(PlsSlot);
	shared_block_size = offsetof(PlsSharedBlock, data) + stats_data_size;

	/* Fill in the KindInfo descriptor */
	{
		PgStat_KindInfo tmp = {
			.name            = PGSTAT_LOGIN_MODULE_NAME,
			.fixed_amount    = true,
			.write_to_file   = true,
			.shared_size     = shared_block_size,
			.shared_data_off = offsetof(PlsSharedBlock, data),
			.shared_data_len = stats_data_size,
			.init_shmem_cb   = pls_init_shmem_cb,
			.init_backend_cb = pls_init_backend_cb,
			.reset_all_cb    = pls_reset_all_cb,
			.snapshot_cb     = pls_snapshot_cb,
		};
		memcpy(&login_stats_kind, &tmp, sizeof(PgStat_KindInfo));
	}

	pgstat_register_kind(PGSTAT_KIND_LOGIN, &login_stats_kind);

	/*
	 * Install shmem_startup_hook to register the LWLock tranche name
	 * in every process (needed for EXEC_BACKEND; harmless on fork).
	 */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook      = pls_shmem_startup;

	/* Install ClientAuthentication hook */
	prev_client_auth_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = pls_client_auth;
}


/* ---- SQL-callable functions ---- */

/*
 * pg_stat_login()
 *   Returns all accumulated login statistics as a set of rows.
 *   Reads from a per-backend snapshot (lock-free after snapshot).
 */
Datum
pg_stat_login(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	PlsData       *snap;
	int            i;

	pgstat_snapshot_fixed(PGSTAT_KIND_LOGIN);
	snap = (PlsData *) pgstat_get_custom_snapshot_data(PGSTAT_KIND_LOGIN);

	InitMaterializedSRF(fcinfo, 0);

	for (i = 0; i < snap->max_entries; i++)
	{
		PlsSlot *slot = &snap->entries[i];
		Datum    values[6];
		bool     nulls[6] = {false};

		if (!slot->used)
			continue;

		values[0] = CStringGetTextDatum(slot->username);
		values[1] = CStringGetTextDatum(slot->database);
		values[2] = Int64GetDatum(slot->login_ok);
		values[3] = Int64GetDatum(slot->login_fail);

		if (slot->login_ok > 0)
			values[4] = TimestampTzGetDatum(slot->last_login_ok);
		else
			nulls[4] = true;

		if (slot->login_fail > 0)
			values[5] = TimestampTzGetDatum(slot->last_login_fail);
		else
			nulls[5] = true;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/*
 * pg_stat_login_info()
 *   Returns metadata about the shared memory area:
 *   capacity, current occupancy, dropped events, and last reset timestamp.
 */
Datum
pg_stat_login_info(PG_FUNCTION_ARGS)
{
	ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	PlsSharedBlock *shmem;
	PlsData        *s;
	Datum           values[4];
	bool            nulls[4] = {false};
	int             max_entries;
	int             num_entries;
	uint64          n_dropped;
	TimestampTz     stats_reset;

	InitMaterializedSRF(fcinfo, 0);

	shmem = (PlsSharedBlock *) pgstat_get_custom_shmem_data(PGSTAT_KIND_LOGIN);
	s     = pls_get_data(shmem);

	LWLockAcquire(&shmem->lock, LW_SHARED);
	max_entries  = s->max_entries;
	num_entries  = s->num_entries;
	n_dropped    = shmem->n_dropped;
	stats_reset  = shmem->stats_reset;
	LWLockRelease(&shmem->lock);

	values[0] = Int32GetDatum(max_entries);
	values[1] = Int32GetDatum(num_entries);
	values[2] = Int64GetDatum((int64) n_dropped);
	values[3] = TimestampTzGetDatum(stats_reset);

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	return (Datum) 0;
}

/*
 * pg_stat_login_reset()
 *   Clears all accumulated statistics and reclaims all slots.
 */
Datum
pg_stat_login_reset(PG_FUNCTION_ARGS)
{
	pgstat_reset_of_kind(PGSTAT_KIND_LOGIN);
	PG_RETURN_VOID();
}
