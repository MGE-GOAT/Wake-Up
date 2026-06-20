"""One-shot data migration: SQLite (server_database.db) → PostgreSQL.

Copies users, devices, active_devices, subscriptions, subscription_history,
messages, unread_messages, pill_schedules. Idempotent-ish: uses ON CONFLICT
DO NOTHING so re-running won't duplicate. Resets Postgres sequences after
load so new BIGSERIAL ids continue past the migrated max.

Run:  python migrate_sqlite_to_pg.py
Env:  DATABASE_URL (default postgresql://elderly:elderly_local_pw@127.0.0.1:5432/elderly_care)
      SQLITE_DB    (default server_database.db)
"""
import asyncio
import os
import sqlite3
from datetime import datetime

import asyncpg

# Columns that map to Postgres TIMESTAMP — SQLite stored them as TEXT, asyncpg
# needs real datetime objects. Parse on the way in.
TS_COLS = {
    "last_login_time", "last_message_time", "last_message_check_time",
    "created_at", "last_seen", "create_time", "last_live_time",
    "message_occur_time", "joined_at", "at", "start_at_utc", "last_ack_at",
}

_TS_FORMATS = ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S",
               "%Y-%m-%d %H:%M", "%Y-%m-%dT%H:%M:%S")


def _coerce(col, val):
    if col in TS_COLS and isinstance(val, str) and val.strip():
        for fmt in _TS_FORMATS:
            try:
                return datetime.strptime(val.strip(), fmt)
            except ValueError:
                continue
        return None  # unparseable → NULL rather than crash
    return val

SQLITE_DB = os.environ.get("SQLITE_DB", "server_database.db")
DATABASE_URL = os.environ.get(
    "DATABASE_URL", "postgresql://elderly:elderly_local_pw@127.0.0.1:5432/elderly_care")

# (sqlite_table, columns, pg_conflict_target). Order respects FK deps.
TABLES = [
    ("users",
     ["username", "password", "firebase_token", "last_login_time",
      "last_message_time", "last_message_check_time", "config_file"],
     "(username)"),
    ("devices",
     ["device_id", "key_hash", "created_at", "last_seen"], "(device_id)"),
    ("active_devices",
     ["device_id", "key_hash", "create_time", "last_message_time",
      "last_live_time", "config_file"], "(device_id)"),
    ("messages",
     ["id", "device_id", "message_type", "message_text", "message_image",
      "message_video", "message_occur_time"], "(id)"),
    ("unread_messages",
     ["id", "user_username", "message_id"], "(id)"),
    ("subscriptions",
     ["device_id", "username", "joined_at"], "(device_id, username)"),
    ("subscription_history",
     ["id", "device_id", "username", "action", "actor", "at"], "(id)"),
    ("pill_schedules",
     ["id", "device_id", "pill_id_local", "name", "audio_path", "start_at_utc",
      "interval_seconds", "total_count", "consumptions_done", "last_ack_at",
      "created_at"], "(id)"),
]

# Sequences to bump after load: (table, id_column)
SEQUENCES = [("messages", "id"), ("unread_messages", "id"),
             ("subscription_history", "id"), ("pill_schedules", "id")]


def _sqlite_rows(table, cols):
    if not os.path.exists(SQLITE_DB):
        print(f"  (no {SQLITE_DB} — skipping {table})")
        return []
    conn = sqlite3.connect(SQLITE_DB)
    conn.row_factory = sqlite3.Row
    try:
        # Only select columns that exist in this SQLite table.
        existing = {r[1] for r in conn.execute(f"PRAGMA table_info({table})")}
        if not existing:
            return []
        use_cols = [c for c in cols if c in existing]
        rows = conn.execute(f"SELECT {', '.join(use_cols)} FROM {table}").fetchall()
        return use_cols, [tuple(_coerce(c, r[c]) for c in use_cols) for r in rows]
    except sqlite3.OperationalError as e:
        print(f"  (skip {table}: {e})")
        return [], []
    finally:
        conn.close()


async def main():
    pg = await asyncpg.connect(DATABASE_URL)
    print(f"Migrating {SQLITE_DB} → {DATABASE_URL.split('@')[-1]}")
    try:
        for table, cols, conflict in TABLES:
            res = _sqlite_rows(table, cols)
            if not res or not res[1]:
                print(f"  {table}: 0 rows")
                continue
            use_cols, rows = res
            placeholders = ", ".join(f"${i+1}" for i in range(len(use_cols)))
            sql = (f"INSERT INTO {table} ({', '.join(use_cols)}) "
                   f"VALUES ({placeholders}) "
                   f"ON CONFLICT {conflict} DO NOTHING")
            inserted = 0
            for row in rows:
                try:
                    await pg.execute(sql, *row)
                    inserted += 1
                except Exception as e:
                    print(f"    row skip ({table}): {e}")
            print(f"  {table}: {inserted}/{len(rows)} rows")

        # Bump sequences so future inserts don't collide with migrated ids.
        for table, idcol in SEQUENCES:
            await pg.execute(
                f"SELECT setval(pg_get_serial_sequence('{table}', '{idcol}'), "
                f"COALESCE((SELECT MAX({idcol}) FROM {table}), 1))")
        print("Sequences reset. Migration complete.")
    finally:
        await pg.close()


if __name__ == "__main__":
    asyncio.run(main())
