# pg_stat_login

PostgreSQL extension that tracks login statistics (successful and failed
authentication attempts) per user and database. This can be useful for observing login behavior in the database and for generating audit and security reports.

**Requires PostgreSQL 18 or later.** Uses the
[Custom Cumulative Statistics API](https://wiki.postgresql.org/wiki/CustomCumulativeStats) (CCS)
introduced in PG18, which means statistics **persist across clean server restarts**
automatically 

This extension is the evolution of [pg_login_stat](https://github.com/asotolongo/pg_login_stat).
Use pg_login_stat for a PG15–PG17 compatibility

## How it works

1. Installs a `ClientAuthentication_hook` that is invoked after every authentication attempt, following the example of core extension auth_delay.
2. Registers a custom stats kind (ID 30) via `pgstat_register_kind()`.
3. Maintains a fixed-size open-addressing hash table inside the CCS shared
   memory block, keyed by `(username, database_name)`.
4. Reporting: The pg_stat_logins view  has the statistics of logins, under the hood call the funcion  pg_stat_login()

## Requirements

- PostgreSQL 18+
- Must be loaded via `shared_preload_libraries`

## Installation

```bash
make
make install
```

You must make sure you can see the binary `pg_config`,
maybe setting PostgreSQL binary path in the OS  or setting `PG_CONFIG=/path_to_pg_config/`  in the makefile or run:

```bash
make PG_CONFIG=/path_to_pg_config/
sudo make install PG_CONFIG=/path_to_pg_config/
```

Add to `postgresql.conf`:

```
shared_preload_libraries = 'pg_stat_login'
```

Restart PostgreSQL and create the extension:

```sql
CREATE EXTENSION pg_stat_login;
```

## Configuration

| Parameter | Type | Default | Context | Description |
|---|---|---|---|---|
| `pg_stat_login.enable` | bool | `on` | SIGHUP | Enable/disable collection; changeable at runtime via `pg_reload_conf()` |

The extension tracks up to **10000** `(user, database)` pairs. This limit is fixed
and cannot be configured.

## Usage

### View accumulated statistics

```sql
SELECT * FROM pg_stat_logins;
username | datname  | login_ok | login_fail |         last_login_ok         |        last_login_fail        
----------+----------+----------+------------+-------------------------------+-------------------------------
 postgres | postgres |        2 |          0 | 2026-06-04 02:43:30.906027+00 | 
 testuser | testdb   |        2 |          3 | 2026-06-04 02:43:41.120841+00 | 2026-06-04 02:43:39.364863+00
(2 rows)
```

| Column | Type | Description |
|---|---|---|
| `username` | text | PostgreSQL role name |
| `datname` | text | Database name |
| `login_ok` | bigint | Successful login count |
| `login_fail` | bigint | Failed login count |
| `last_login_ok` | timestamptz | Timestamp of last successful login (NULL if none) |
| `last_login_fail` | timestamptz | Timestamp of last failed login (NULL if none) |

### Examples of use

#### Find users with failed logins

```sql
SELECT username, datname, login_ok, login_fail
FROM pg_login_stats
WHERE login_fail > 0
ORDER BY login_fail DESC;
```

#### Find users who have never successfully logged in

```sql
SELECT username, datname, login_fail
FROM pg_login_stats
WHERE login_ok = 0
ORDER BY login_fail DESC;
```

#### Checking the metadata 
```sql
SELECT * FROM pg_stat_login_info();
```

| Column | Type | Description |
|---|---|---|
| `max_entries` | int | Maximum pairs tracked (always 10000) |
| `num_entries` | int | Distinct `(user, database)` pairs currently tracked |
| `n_dropped` | bigint | Login events dropped because the table was full, since the last reset |
| `stats_reset` | timestamptz | Timestamp of the last reset; it is also set when the statistics start empty, for example on first start or after crash recovery |

If `n_dropped` grows, the table is full: new `(user, database)` pairs are not
tracked until `pg_stat_login_reset()` is called, while pairs already in the table
keep being counted. Failed logins also take up entries, so `n_dropped` growing
quickly can indicate login attempts with many different user or database names.

### Reset statistics

```sql
SELECT pg_stat_login_reset();
```

### Enable / disable at runtime

```sql
ALTER SYSTEM SET pg_stat_login.enable = off;--on
SELECT pg_reload_conf();
```

## Permissions

- Superusers can call all functions and the reset.
- Members of `pg_read_all_stats` can read statistics (view and `pg_stat_login_info()`).

## Persistence

Statistics survive **clean** PostgreSQL restarts (the CCS API serializes the
stats block to `$PGDATA/pg_stat/` on shutdown). They are discarded after crash
recovery, which follows standard PostgreSQL cumulative stats semantics.

## Stats kind ID

This extension uses custom stats kind ID **30**, registered at
https://wiki.postgresql.org/wiki/CustomCumulativeStats.


## Notes

- Failed logins include wrong passwords and other errors reported by the authentication method, and some time can count double due to [The SSL mode behavior in authentication-hooked extensions](https://ongres.com/blog/ssl_mode_behavior_in_authentication_hooked_extensions/)
- A client that closes the connection when asked for a password is **not counted**. `psql` and other libpq clients do this before prompting for the password and reconnecting, so counting it would report a false failure.
- Names are stored as the client sends them. A name that is not valid in the encoding of the database being queried is shown with its bytes escaped as `\xHH`, and in `SQL_ASCII` databases every non-ASCII byte is escaped. A valid name that the client encoding cannot represent still makes the query fail with `has no equivalent in encoding`, until `pg_stat_login_reset()` is called.
- Connections rejected before the `ClientAuthentication_hook` runs are **not counted** in `login_ok`, `login_fail` or `n_dropped`:
  - `sorry, too many clients already`, when all connection slots are in use. These rejections can be found in the server log.
  - `pg_hba.conf` rejections: a `reject` rule or no matching entry.
- Connections rejected after a successful authentication are **counted as `login_ok`**, because the hook has already run:
  - `remaining connection slots are reserved`, when a role without the required privileges gets only reserved slots.
  - A database that does not exist, when the credentials are valid.
  - A role that does not exist, with a method that does not verify credentials (for example `trust`).
- `login_ok` therefore means that authentication passed, not that a session was opened. Names are stored as received, without checking the catalog, so a role or database that does not exist gets its own row and takes up an entry. To find them, compare the rows with `pg_roles` and `pg_database`.

  | Attempt | `scram-sha-256` | `trust` |
  |---|---|---|
  | Role does not exist | `login_fail` | `login_ok` |
  | Database does not exist, valid password | `login_ok` | `login_ok` |
  | Database does not exist, wrong password | `login_fail` | `login_ok` |
  | Neither role nor database exist | `login_fail` | `login_ok` |

## Docker test environment

See [`test_docker/`](test_docker/) for a ready-to-use PostgreSQL 18 environment.
See [`test_docker19rc1/`](test_docker19rc1/) for the same environment on PostgreSQL 19 RC1.


## AI help
I received some help from Claude Code for coding some parts of the extension following my instructions

## Special thanks
To Fabrízio de Royes Mello for his suggestion about to follow the custom cumulative statistics API and his extension [pg_stat_log](https://github.com/fabriziomello/pg_stat_log/), I tried to copy some of his ideas.


## License

Released under the [PostgreSQL License](https://opensource.org/licenses/PostgreSQL).

## Authors:
This extension is an open project. Feel free to join us and improve this module. To find out how you can get involved, please contact us or write us:

Anthony Sotolongo: asotolongo@gmail.com