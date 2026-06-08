MODULE_big = pg_stat_login
OBJS       = pg_stat_login.o

EXTENSION = pg_stat_login
DATA      = pg_stat_login--1.0.sql
PGFILEDESC = "pg_stat_login - login statistics via Custom Cumulative Stats API (PG18+)"

REGRESS      = pg_stat_login
REGRESS_OPTS = --temp-config=./pg_stat_login.conf \
               --temp-instance=./tmp_check

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)