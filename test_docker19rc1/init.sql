-- Install the extension in the test database
CREATE EXTENSION pg_stat_login;

-- Test user for simulating successful and failed logins
CREATE USER tester WITH PASSWORD 'secret123';
GRANT CONNECT ON DATABASE testdb TO tester;

-- Allow tester to read statistics
GRANT EXECUTE ON FUNCTION pg_stat_login()      TO tester;
GRANT EXECUTE ON FUNCTION pg_stat_login_info() TO tester;
GRANT SELECT  ON pg_stat_logins                TO tester;