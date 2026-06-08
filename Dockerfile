FROM postgres:18

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        postgresql-server-dev-18 \
        gcc \
        make \
        libkrb5-dev \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /etc/postgresql

COPY . /tmp/pg_stat_login/
RUN cd /tmp/pg_stat_login && \
    make && \
    make install && \
    rm -rf /tmp/pg_stat_login