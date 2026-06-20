"""
Phase 2 schema migration.

Drops the factory-list `produced_devices` table (no longer needed — factory
devices ship with hardcoded id="Noban" and no password; user sets both
during BLE onboarding).

Renames `active_devices` → `devices`, converts `key_hash` INTEGER → TEXT
(it's a SHA-256 hex string).

Renames `subscription` → `subscriptions` (plural) and adds enforcement of
max 3 subscribers per device via a BEFORE INSERT trigger.

Adds `subscription_history` audit table.

Seeds the laptop's existing test device so the running pipeline doesn't
404 between this migration and the device-side main.cpp update.

Run once: `python migrate_phase2.py`. Idempotent — safe to re-run; will
skip tables that already match the target schema.
"""
from __future__ import annotations

import hashlib
import os
import sqlite3
import sys

DB_PATH = os.path.join(os.path.dirname(__file__), "server_database.db")


def sha256_hex(s: str) -> str:
    return hashlib.sha256(s.encode("utf-8")).hexdigest()


def main() -> int:
    if not os.path.exists(DB_PATH):
        print(f"no DB at {DB_PATH}", file=sys.stderr); return 1

    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()

    # Snapshot existing data before destructive ops.
    existing_devices = cur.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='active_devices'"
    ).fetchone()
    existing_subs    = cur.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='subscription'"
    ).fetchone()

    legacy_devices: list[sqlite3.Row] = []
    legacy_subs:    list[sqlite3.Row] = []
    if existing_devices:
        legacy_devices = cur.execute("SELECT * FROM active_devices").fetchall()
    if existing_subs:
        legacy_subs = cur.execute("SELECT * FROM subscription").fetchall()

    # The current pipeline running on the laptop uses these creds — preserve
    # them as a row in the new `devices` table so it keeps working between
    # this migration and the device-side main.cpp update that will switch
    # to user-chosen creds in id.txt.
    LAPTOP_ID       = "ID_12233945"
    LAPTOP_AUTH_RAW = "1223344556677889"     # historical hardcoded AUTH_CODE
    LAPTOP_KEY_HASH = sha256_hex(LAPTOP_AUTH_RAW)

    # Also preserve the existing single subscription if it points at the
    # laptop device — that's the noorih1414@gmail.com -> ID_12233945 row.
    preserved_subs = [
        (s["username"], s["device_id"])
        for s in legacy_subs
        if s["device_id"] == LAPTOP_ID
    ]

    cur.executescript("""
        BEGIN;

        DROP TABLE IF EXISTS produced_devices;
        DROP TABLE IF EXISTS active_devices;
        DROP TABLE IF EXISTS subscription;

        CREATE TABLE devices (
            device_id   TEXT PRIMARY KEY,
            key_hash    TEXT NOT NULL,
            created_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
            last_seen   DATETIME
        );

        CREATE TABLE subscriptions (
            device_id   TEXT NOT NULL,
            username    TEXT NOT NULL,
            joined_at   DATETIME DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (device_id, username),
            FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE,
            FOREIGN KEY (username)  REFERENCES users(username)    ON DELETE CASCADE
        );

        -- Hard cap: max 3 subscribers per device, enforced at write time.
        CREATE TRIGGER subscriptions_max3
        BEFORE INSERT ON subscriptions
        WHEN (SELECT COUNT(*) FROM subscriptions WHERE device_id = NEW.device_id) >= 3
        BEGIN
            SELECT RAISE(ABORT, 'device_full');
        END;

        -- Audit log. Every subscription event (join / leave / kick / replace)
        -- writes a row here so disputes can be reconstructed.
        --   action  = 'joined' (Subscribe_Device)
        --           | 'left'   (Unsubscribe_Device, self-initiated)
        --           | 'kicked' (Replace_Subscriber, performed by `actor`)
        --   actor   = username that performed the action (NULL if self)
        CREATE TABLE subscription_history (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id   TEXT NOT NULL,
            username    TEXT NOT NULL,
            action      TEXT NOT NULL,
            actor       TEXT,
            at          DATETIME DEFAULT CURRENT_TIMESTAMP
        );

        COMMIT;
    """)

    # Seed laptop test device.
    cur.execute(
        "INSERT INTO devices (device_id, key_hash) VALUES (?, ?)",
        (LAPTOP_ID, LAPTOP_KEY_HASH),
    )

    # Restore preserved subscriptions. The trigger enforces max 3, but the
    # legacy data only had 1 row so this won't fail.
    for username, device_id in preserved_subs:
        cur.execute(
            "INSERT INTO subscriptions (device_id, username) VALUES (?, ?)",
            (device_id, username),
        )
        cur.execute(
            "INSERT INTO subscription_history (device_id, username, action, actor) "
            "VALUES (?, ?, 'joined', NULL)",
            (device_id, username),
        )

    conn.commit()
    conn.close()

    # Verification pass.
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    tables = [r["name"] for r in conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"
    )]
    print("tables after migration:", sorted(tables))
    print()
    print("devices:")
    for r in conn.execute("SELECT device_id, key_hash, created_at FROM devices"):
        print(f"  {r['device_id']:15s}  {r['key_hash']:24s}...  {r['created_at']}")
    print()
    print("subscriptions:")
    for r in conn.execute("SELECT device_id, username, joined_at FROM subscriptions"):
        print(f"  {r['device_id']:15s}  {r['username']}  {r['joined_at']}")
    print()
    print("subscription_history:")
    for r in conn.execute("SELECT device_id, username, action, actor, at FROM subscription_history"):
        print(f"  {r['device_id']:15s}  {r['username']}  {r['action']}  actor={r['actor']}  {r['at']}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
