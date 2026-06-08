-- Install extension pg_stat_login to track login statistics
CREATE EXTENSION pg_stat_login;
CREATE EXTENSION dblink;
show shared_preload_libraries;
\dx+ pg_stat_login


-- tester user to log in and generate statistics
CREATE USER tester WITH PASSWORD 'secret123' SUPERUSER;
GRANT CONNECT ON DATABASE postgres TO tester;

-- allow to user tester see the statistics
GRANT EXECUTE ON FUNCTION pg_stat_login() TO tester;
GRANT SELECT ON pg_stat_logins TO tester;

-- allow to user tester see reset statistics
GRANT EXECUTE ON FUNCTION pg_stat_login_reset() TO tester;
--
--connect to the database with the tester user to generate  OK login statistics
--
\c "dbname=contrib_regression user=tester password=secret123   sslmode=disable"
select current_database();

--
--connect to the database with the tester user to generate  OK login statistics
--
\c "dbname=contrib_regression user=tester password=secret123 sslmode=disable"


--
--connect to the database with the test user to generate  FAIL login statistics using dblink
--
DO $$
BEGIN
    PERFORM dblink_connect(
        'dbname=contrib_regression user=tester password=WRONG'
    );
EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'Login failed as expected: Message: %', SQLERRM;
END;
$$;

--
--get login statistics for the tester user
--
SELECT username, datname, login_ok, login_fail FROM pg_stat_logins where username='tester';

--
--  get the metadata
--
SELECT max_entries,num_entries,n_dropped FROM pg_stat_login_info();
--
--reset statistics
--
SELECT pg_stat_login_reset();

--
--get login statistics for the tester user after reset
--
SELECT username, datname, login_ok, login_fail FROM pg_stat_logins where username='tester';

--
--disable the extension that captures login statistics
--
ALTER SYSTEM SET pg_stat_login.enable = off;
select pg_reload_conf();
SELECT pg_stat_login_reset();

--
--connect to the database with the tester user to check login statistics with extension disabled
--
\c "dbname=contrib_regression user=tester password=secret123 sslmode=disable"

--
--get login statistics for the tester user with the extension disabled
--
SELECT username, datname, login_ok, login_fail FROM pg_stat_logins where username='tester';


--cleanup
DROP EXTENSION pg_stat_login;
DROP EXTENSION dblink;