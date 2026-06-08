# test_docker — Docker test environment for pg_stat_login (PostgreSQL 18)

Starts a PostgreSQL 18 instance with `pg_stat_login` installed and ready to test.
Statistics persist across container restarts (PG18 Custom Cumulative Stats API).

## Files

| File | Description |
|---|---|
| `../Dockerfile` | Image based on `postgres:18` that compiles and installs the extension |
| `docker-compose.yml` | Service with `shared_preload_libraries=pg_stat_login` |
| `pg_hba.conf` | Requires password for all connections (enables failed-login testing) |
| `init.sql` | Runs on first startup: creates the extension and the `tester` user |

## Requirements

- Docker and Docker Compose
- Port 5432 free on the host

## Usage

All commands must be run **from this directory** (`test_docker/`).

### Start

```bash
docker compose up -d
```

### Check it is ready

```bash
docker compose logs --tail 5
# Should end with: database system is ready to accept connections
```

### Stop

```bash
docker compose down        # keeps the data volume (stats are persisted)
docker compose down -v     # deletes the volume (fresh start)
```

### Rebuild after changing source code

```bash
docker compose build
docker compose up -d
```

---

## Credentials

| User | Password | Notes |
|---|---|---|
| `postgres` | `postgres` | Superuser |
| `tester` | `secret123` | Unprivileged test user |

---

## Tests

### Connect

```bash
psql -h localhost -p 5432 -U postgres -d testdb
psql -h localhost -p 5432 -U tester   -d testdb
```

### View accumulated statistics

```sql
SELECT * FROM pg_stat_logins;
```

```
 username | datname | login_ok | login_fail | last_login_ok | last_login_fail
----------+---------+----------+------------+---------------+-----------------
 postgres | testdb  |        2 |          0 | 2026-06-03... |
 tester   | testdb  |        1 |          1 | 2026-06-03... | 2026-06-03...
```

### Generate a failed login

```bash
PGPASSWORD=wrong psql -h localhost -p 5432 -U tester -d testdb
```

### Generate multiple logins

```bash
# 3 successful
for i in 1 2 3; do
  PGPASSWORD=postgres psql -h localhost -p 5432 -U postgres -d testdb -c "SELECT 1;" > /dev/null
done

# 2 failed
for i in 1 2; do
  PGPASSWORD=bad psql -h localhost -p 5432 -U tester -d testdb -c "SELECT 1;" 2>/dev/null || true
done

# View result
PGPASSWORD=postgres psql -h localhost -p 5432 -U postgres -d testdb -c "SELECT * FROM pg_stat_logins;"
```

### Capacity and drop monitoring

```sql
SELECT * FROM pg_stat_login_info();
```

```
 max_entries | num_entries | n_dropped |          stats_reset
-------------+-------------+-----------+-------------------------------
        1000 |           3 |         0 | 2026-06-03 12:00:00.000000+00
```

`n_dropped > 0` means `max_entries` is too small — increase it and restart.

### Reset statistics

```sql
SELECT pg_stat_login_reset();
```

### Persistence test

```bash
# Generate some stats
PGPASSWORD=postgres psql -h localhost -p 5432 -U postgres -d testdb -c "SELECT 1;" > /dev/null
PGPASSWORD=postgres psql -h localhost -p 5432 -U postgres -d testdb -c "SELECT * FROM pg_stat_logins;"

# Stop and restart (WITHOUT -v, to keep the volume)
docker compose down
docker compose up -d

# Stats are still there (CCS API persists across clean restarts)
PGPASSWORD=postgres psql -h localhost -p 5432 -U postgres -d testdb -c "SELECT * FROM pg_stat_logins;"
```

### Enable / disable collection at runtime

```sql
ALTER SYSTEM SET pg_stat_login.enable = off;
SELECT pg_reload_conf();

-- Logins from this point are not recorded

ALTER SYSTEM SET pg_stat_login.enable = on;
SELECT pg_reload_conf();
```

### Watch server logs

```bash
docker compose logs -f
```