import sqlite3
import json

# Path to database file
DATABASE = 'server_database.db'

# Create Tables
conn = sqlite3.connect(DATABASE)
cursor = conn.cursor()

# Enable foreign key constraints
cursor.execute("PRAGMA foreign_keys = ON;")


# Create produces devices table
cursor.execute('''
    CREATE TABLE IF NOT EXISTS produced_devices (
        id TEXT PRIMARY KEY,
        Authentication_code INTEGER
    )
''')


# Create active devices table
cursor.execute('''
    CREATE TABLE IF NOT EXISTS active_devices (
        device_id PRIMARY KEY,
        key_hash INTEGER,
        create_time DATETIME DEFAULT CURRENT_TIMESTAMP,
        last_message_time DATETIME, 
        last_live_time DATETIME,
        config_file TEXT, -- Use TEXT to store JSON data
        FOREIGN KEY (device_id) REFERENCES produced_devices (id)
    )
''')



# Create users table
cursor.execute('''
    CREATE TABLE IF NOT EXISTS users (
        username TEXT PRIMARY KEY,
        password TEXT,
        firebase_token TEXT,
        last_login_time DATETIME,
        last_message_time DATETIME, 
        last_message_check_time DATETIME,
        config_file TEXT -- Use TEXT to store JSON data
    )
''')

# Create messages table
cursor.execute('''
    CREATE TABLE IF NOT EXISTS messages (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        device_id TEXT,
        message_type TEXT,
        message_text TEXT,
        message_image TEXT, -- Use TEXT to store file paths
        message_video TEXT,
        message_occur_time DATETIME,
        FOREIGN KEY (device_id) REFERENCES produced_devices (id)
    )
''')

# Create subscription table
cursor.execute('''
    CREATE TABLE IF NOT EXISTS subscription (
        username TEXT,
        device_id TEXT,
        subscription_time DATETIME DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY (username) REFERENCES users (username),
        FOREIGN KEY (device_id) REFERENCES produced_devices (id)
    )
''')


cursor.execute('''
CREATE TABLE IF NOT EXISTS unread_messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_username TEXT,
    message_id INTEGER,
    FOREIGN KEY (user_username) REFERENCES users (username),
    FOREIGN KEY (message_id) REFERENCES messages (id)
);
''')


# Pill schedules. Each row is one pill assigned to one device. The app's
# add-pill page POSTs to /Pill_Sync_App which inserts/updates rows here.
# `times_json` is a JSON list of "HH:MM" strings. `audio_path` points to the
# server-stored copy of the recorded pill-reminder audio (transcoded to WAV
# on upload so the device can play it directly via aplay/mpv without a
# decoder dep). `last_ack_at` is updated when the device wake-fires PILL.
cursor.execute('''
CREATE TABLE IF NOT EXISTS pill_schedules (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    pill_id_local INTEGER NOT NULL,
    name TEXT,
    times_json TEXT,
    count INTEGER,
    audio_path TEXT,
    last_ack_at DATETIME,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(device_id, pill_id_local),
    FOREIGN KEY (device_id) REFERENCES produced_devices (id)
);
''')


print("Database and tables created successfully.")

# conn.close()


























# conn = sqlite3.connect('server_database.db')
# cursor = conn.cursor()

cursor.execute('''
    INSERT INTO produced_devices (id, Authentication_code) 
    VALUES (?, ?)
''', (
    'ID_12233945',            # Device ID
     1223344556677889         # Authentication_code
))

cursor.execute('''
    INSERT INTO produced_devices (id, Authentication_code) 
    VALUES (?, ?)
''', (
    'ID_12233946',            # Device ID
     1223344556677880         # Authentication_code
))


cursor.execute('''
    INSERT INTO produced_devices (id, Authentication_code) 
    VALUES (?, ?)
''', (
    'ID_12233947',            # Device ID
     1223344556677881         # Authentication_code
))


cursor.execute('''
    INSERT INTO produced_devices (id, Authentication_code) 
    VALUES (?, ?)
''', (
    'ID_12233948',            # Device ID
     1223344556677882         # Authentication_code
))

cursor.execute('''
    INSERT INTO users (username, password) 
    VALUES (?, ?)
''', (
    'noorih1414@gmail.com',            
     1234,
))




data = {
    "key-Hash": 5889849602967952,
    "config1": "val1",
    "config2": "val2"
}


cursor.execute('''
    INSERT INTO active_devices (device_id, key_hash, config_file) 
    VALUES (?, ?, ?)
''', (
    'ID_12233945',            
     5889849602967952,
     json.dumps(data)         
))



cursor.execute('''
    INSERT INTO subscription (username, device_id) 
    VALUES (?, ?)
''', (
    'noorih1414@gmail.com',            
    'ID_12233945',
))




conn.commit()
# cursor.execute("SELECT * FROM config")
# print("Config Table:", cursor.fetchall())
conn.close()


