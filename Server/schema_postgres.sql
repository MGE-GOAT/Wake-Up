-- Phase 5.4 — PostgreSQL schema for the async FastAPI server.
-- Translated 1:1 from the SQLite schema (server_database.db) with these
-- mechanical changes:
--   INTEGER PRIMARY KEY AUTOINCREMENT  → BIGSERIAL / GENERATED IDENTITY
--   DATETIME                           → TIMESTAMP (naive; app stores UTC strings)
--   TEXT PRIMARY KEY                   → TEXT PRIMARY KEY (unchanged)
--   sqlite trigger RAISE(ABORT,...)    → PL/pgSQL trigger function
--   ON CONFLICT(...) DO UPDATE         → identical syntax in PG
-- Safe to re-run: every object uses IF NOT EXISTS / CREATE OR REPLACE.

CREATE TABLE IF NOT EXISTS users (
    username                TEXT PRIMARY KEY,
    password                TEXT,
    firebase_token          TEXT,
    last_login_time         TIMESTAMP,
    last_message_time       TIMESTAMP,
    last_message_check_time TIMESTAMP,
    config_file             TEXT DEFAULT '{}'    -- JSON as text, matches SQLite
);

CREATE TABLE IF NOT EXISTS devices (
    device_id   TEXT PRIMARY KEY,
    key_hash    TEXT NOT NULL,
    created_at  TIMESTAMP DEFAULT now(),
    last_seen   TIMESTAMP
);

CREATE TABLE IF NOT EXISTS active_devices (
    device_id          TEXT PRIMARY KEY,
    key_hash           TEXT,
    create_time        TIMESTAMP,
    last_message_time  TIMESTAMP,
    last_live_time     TIMESTAMP,
    config_file        TEXT,
    mac                TEXT,
    call_requested     TIMESTAMP,
    guest              BOOLEAN DEFAULT FALSE,   -- "do not disturb" — pauses ALL detection/pills/calls
    guest_requested    BOOLEAN,                 -- one-shot app/voice guest command (consumed on heartbeat)
    sleep              BOOLEAN DEFAULT FALSE,    -- sleep mode — pauses ONLY fall+camera; wake double-gated
    sleep_requested    BOOLEAN                  -- one-shot SLEEP/WAKE command (consumed on heartbeat)
);

CREATE TABLE IF NOT EXISTS messages (
    id                  BIGSERIAL PRIMARY KEY,
    device_id           TEXT,
    message_type        TEXT,
    message_text        TEXT,
    message_image       TEXT,      -- file path
    message_video       TEXT,      -- file path
    message_occur_time  TIMESTAMP
);

CREATE TABLE IF NOT EXISTS unread_messages (
    id            BIGSERIAL PRIMARY KEY,
    user_username TEXT,
    message_id    BIGINT
);

CREATE TABLE IF NOT EXISTS subscriptions (
    device_id   TEXT NOT NULL,
    username    TEXT NOT NULL,
    joined_at   TIMESTAMP DEFAULT now(),
    PRIMARY KEY (device_id, username),
    FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE,
    FOREIGN KEY (username)  REFERENCES users(username)    ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS subscription_history (
    id          BIGSERIAL PRIMARY KEY,
    device_id   TEXT NOT NULL,
    username    TEXT NOT NULL,
    action      TEXT NOT NULL,            -- joined | left | kicked
    actor       TEXT,
    at          TIMESTAMP DEFAULT now()
);

CREATE TABLE IF NOT EXISTS pill_schedules (
    id                BIGSERIAL PRIMARY KEY,
    device_id         TEXT NOT NULL,
    pill_id_local     INTEGER NOT NULL,
    name              TEXT,
    audio_path        TEXT,
    start_at_utc      TIMESTAMP NOT NULL,
    interval_seconds  INTEGER NOT NULL,
    total_count       INTEGER NOT NULL,
    consumptions_done INTEGER NOT NULL DEFAULT 0,
    last_ack_at       TIMESTAMP,
    created_at        TIMESTAMP DEFAULT now(),
    UNIQUE (device_id, pill_id_local)
);

-- ── 3-subscriber cap (was a SQLite BEFORE INSERT trigger raising 'device_full')
CREATE OR REPLACE FUNCTION enforce_max3_subscribers() RETURNS TRIGGER AS $$
BEGIN
    IF (SELECT COUNT(*) FROM subscriptions WHERE device_id = NEW.device_id) >= 3 THEN
        RAISE EXCEPTION 'device_full';
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS subscriptions_max3 ON subscriptions;
CREATE TRIGGER subscriptions_max3
    BEFORE INSERT ON subscriptions
    FOR EACH ROW EXECUTE FUNCTION enforce_max3_subscribers();

-- ── Hot-path indexes (same set as the SQLite WAL init block)
CREATE INDEX IF NOT EXISTS idx_subscriptions_device ON subscriptions(device_id);
CREATE INDEX IF NOT EXISTS idx_subscriptions_user   ON subscriptions(username);
CREATE INDEX IF NOT EXISTS idx_messages_device      ON messages(device_id);
CREATE INDEX IF NOT EXISTS idx_pill_schedules_dev   ON pill_schedules(device_id);
CREATE INDEX IF NOT EXISTS idx_unread_msg_user      ON unread_messages(user_username);

-- Ownership so the app role can write everything.
GRANT ALL ON ALL TABLES IN SCHEMA public TO elderly;
GRANT ALL ON ALL SEQUENCES IN SCHEMA public TO elderly;
