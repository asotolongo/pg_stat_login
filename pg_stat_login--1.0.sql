\echo Use "CREATE EXTENSION pg_stat_login" to load this file. \quit

-- Returns all accumulated login statistics from the CCS shared memory snapshot.
CREATE FUNCTION pg_stat_login(
    OUT username        text,
    OUT datname         text,
    OUT login_ok        bigint,
    OUT login_fail      bigint,
    OUT last_login_ok   timestamptz,
    OUT last_login_fail timestamptz
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_login'
LANGUAGE C STRICT PARALLEL UNSAFE;

-- Returns metadata about the shared memory area.
CREATE FUNCTION pg_stat_login_info(
    OUT max_entries  int,
    OUT num_entries  int,
    OUT n_dropped    bigint,
    OUT stats_reset  timestamptz
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_login_info'
LANGUAGE C STRICT PARALLEL UNSAFE;

-- Resets all accumulated statistics and reclaims all slots.
CREATE FUNCTION pg_stat_login_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_stat_login_reset'
LANGUAGE C STRICT PARALLEL UNSAFE;

-- Convenience view over pg_stat_login().
CREATE VIEW pg_stat_logins AS
    SELECT * FROM pg_stat_login();

-- Permissions: superuser can reset; pg_read_all_stats can read.
REVOKE ALL ON FUNCTION pg_stat_login_reset() FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_stat_login()       FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_stat_login_info()  FROM PUBLIC;
REVOKE ALL ON pg_stat_logins                 FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pg_stat_login()      TO pg_read_all_stats;
GRANT EXECUTE ON FUNCTION pg_stat_login_info() TO pg_read_all_stats;
GRANT SELECT  ON pg_stat_logins                TO pg_read_all_stats;