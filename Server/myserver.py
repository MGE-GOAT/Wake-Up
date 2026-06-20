from flask import Flask, request, jsonify
import sqlite3
from datetime import datetime, timedelta
import json
import os
import random
import smtplib
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
import threading
import base64
import requests
import time
import threading
import google.auth
from google.auth.transport.requests import Request
from google.oauth2 import service_account
SCOPES = ['https://www.googleapis.com/auth/firebase.messaging']

# Wake-command STT + intent classifier (faster-whisper + Qwen-14B with
# grammar-constrained output). Imported lazily so the rest of the server can
# still start without the model files / CUDA — useful for dev boxes.
try:
    import wake_command
except Exception as _e:
    wake_command = None
    print(f"⚠️  wake_command module unavailable ({_e!r}); /Wake_Command will return 503")

# Phase 5.1 — MQTT publisher. Additive: every state-change route also pushes
# the new state to the broker. Device + app subscribe; broker fans out. HTTP
# routes still work during the transition so older clients don't break.
import mqtt_pub

app = Flask(__name__)

DATABASE = 'server_database.db'
UPLOAD_FOLDER = 'uploads'  # Directory to save uploaded files
os.makedirs(UPLOAD_FOLDER, exist_ok=True)  # Ensure the directory exists

image_reqs = {}

validation_codes = {}

def add_member_to_dictionary(username, validation_code):
    """add a member to dictionary with removing it after a minute"""
    validation_codes[username] = validation_code
    print(f"Added: {username} -> {validation_code}")

    # set a timer to remove the member after a minute
    timer = threading.Timer(180, remove_validation_code, args=(username,))
    timer.start()

def remove_validation_code(username):
    """remove a member from dictionary"""
    if username in validation_codes:
        del validation_codes[username]
        print(f"Removed: {username}")

def send_validation_code_to_user_email(email, validation_code):
    """Send a validation code via Gmail SMTP.

    Iran network: smtp.gmail.com is reachable only via the xray HTTP CONNECT
    proxy at 127.0.0.1:10808. We route smtplib through it using PySocks
    when SMTP_PROXY_HOST/SMTP_PROXY_PORT env vars are set.

    Args:
        email (str): The user's email address.
        validation_code (int): The validation code to be sent.
    """
    # Iran reality: STARTTLS handshake on 587 breaks through xray's HTTP
    # CONNECT tunnel ("Connection unexpectedly closed" after EHLO). Port 465
    # (SMTPS — SSL from byte 1) works cleanly through SOCKS5. xray's listener
    # at 127.0.0.1:10808 is SOCKS5.
    SMTP_SERVER     = "smtp.gmail.com"
    SMTP_PORT       = 465
    SENDER_EMAIL    = os.environ.get("SMTP_SENDER_EMAIL", "noorijustfortest1@gmail.com")
    SENDER_PASSWORD = os.environ.get("SMTP_SENDER_PASSWORD", "")

    subject = "Your Validation Code"
    body = f"Hello,\n\nYour validation code is: {validation_code}\n\nThank you."
    msg = MIMEMultipart()
    msg["From"]    = SENDER_EMAIL
    msg["To"]      = email
    msg["Subject"] = subject
    msg.attach(MIMEText(body, "plain"))

    # If SMTP_PROXY_HOST is set, monkey-patch smtplib's socket through xray.
    # Otherwise direct (works if running outside Iran or via a country-bypass).
    proxy_host = os.environ.get("SMTP_PROXY_HOST")
    proxy_port = int(os.environ.get("SMTP_PROXY_PORT", "0"))
    proxy_type = os.environ.get("SMTP_PROXY_TYPE", "socks5").lower()  # http | socks5
    if proxy_host and proxy_port:
        try:
            import socks
            kind = socks.HTTP if proxy_type == "http" else socks.SOCKS5
            socks.set_default_proxy(kind, proxy_host, proxy_port)
            socks.wrap_module(smtplib)
        except Exception as e:
            print(f"[smtp] proxy setup failed ({e}) — falling back to direct")

    smtp = None
    try:
        # SMTP_SSL because the proxied STARTTLS handshake on 587 breaks. 465
        # is SSL from the start so no upgrade dance — works through SOCKS5.
        smtp = smtplib.SMTP_SSL(SMTP_SERVER, SMTP_PORT, timeout=30)
        smtp.login(SENDER_EMAIL, SENDER_PASSWORD)
        smtp.sendmail(SENDER_EMAIL, email, msg.as_string())
        # flush=True so the daemon-thread print actually reaches the log file
        # instead of sitting in stdio buffer until process exit.
        print(f"[smtp] sent code {validation_code} to {email}", flush=True)

    except Exception as e:
        print(f"[smtp] FAILED to send to {email}: {type(e).__name__}: {e}", flush=True)

    finally:
        # Close the SMTP connection. smtp may be None if the SMTP() constructor
        # itself threw (the original bug — UnboundLocalError masked the real
        # network error).
        if smtp is not None:
            try: smtp.quit()
            except Exception: pass

def read_file_as_base64(file_path):
    """Read a file from disk and return its content as a Base64-encoded string."""
    try:
        with open(file_path, "rb") as file:
            return base64.b64encode(file.read()).decode("utf-8")
    except FileNotFoundError:
        return None

_WAL_INITIALIZED = False

def get_db_connection():
    """Connect to the SQLite database.

    On first connection per process: enable WAL journal mode (concurrent
    reads + one writer, fixes the "database is locked" 500 we hit under
    polling load) and run one-shot index creation. Idempotent — IF NOT
    EXISTS makes both safe to re-run."""
    global _WAL_INITIALIZED
    conn = sqlite3.connect(DATABASE, timeout=15)
    conn.row_factory = sqlite3.Row
    if not _WAL_INITIALIZED:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA synchronous=NORMAL")  # WAL + NORMAL = safe + fast
        # Hot-path indexes. `events` is app-side only — not in this DB.
        conn.executescript("""
            CREATE INDEX IF NOT EXISTS idx_subscriptions_device ON subscriptions(device_id);
            CREATE INDEX IF NOT EXISTS idx_subscriptions_user   ON subscriptions(username);
            CREATE INDEX IF NOT EXISTS idx_messages_device      ON messages(device_id);
            CREATE INDEX IF NOT EXISTS idx_pill_schedules_dev   ON pill_schedules(device_id);
            CREATE INDEX IF NOT EXISTS idx_unread_msg_user      ON unread_messages(user_username);
        """)
        conn.commit()
        _WAL_INITIALIZED = True
    return conn


# ── Device auth ──────────────────────────────────────────────────────────────
# Single source of truth for device-side request auth. Endpoints used to do
# this inline against `produced_devices`; after Phase 2 migration they all go
# through here.
#
# Dual auth: accepts the new `Key-Hash` header (64-hex SHA-256, what the app
# sends after the user has subscribed) OR the legacy `Authentication-Code`
# header (numeric, what the laptop currently sends with its hardcoded
# AUTH_CODE). For the legacy case we hash the value on-the-fly and compare.
# Remove the legacy branch once every device is on Key-Hash.
import hashlib  # used by verify_device_auth + the new subscription endpoints

def verify_device_auth(req):
    """Look up the device by `Device-Id`, validate the credentials header.

    Returns (device_row, err_response, err_status). On success err_* are None.
    On failure device_row is None and the caller should `return jsonify(...),
    status` with the returned values."""
    device_id = req.headers.get("Device-Id") or req.headers.get("Device-ID")
    key_hash  = req.headers.get("Key-Hash")
    legacy    = req.headers.get("Authentication-Code")
    if not device_id or (not key_hash and not legacy):
        return None, {"error": "Device-Id or credentials missing"}, 400

    conn = get_db_connection()
    dev  = conn.execute(
        "SELECT device_id, key_hash FROM devices WHERE device_id = ?",
        (device_id,),
    ).fetchone()
    conn.close()

    if dev is None:
        return None, {"error": "Invalid Device"}, 403

    # New-style first; fall back to legacy hash-on-the-fly for compatibility.
    presented = key_hash or hashlib.sha256(legacy.encode("utf-8")).hexdigest()
    if presented != dev["key_hash"]:
        return None, {"error": "Invalid credentials"}, 403
    return dev, None, None


def sha256_hex(s: str) -> str:
    return hashlib.sha256(s.encode("utf-8")).hexdigest()


def _get_access_token():
    """
    دریافت توکن دسترسی معتبر برای احراز هویت درخواست‌ها.

    :return: توکن دسترسی
    """
    # مسیر فایل JSON کلید سرور
    SERVICE_ACCOUNT_FILE = 'elderly-care-assistant-1bab66e453cb.json'

    # ایجاد Credentials از فایل JSON
    credentials = service_account.Credentials.from_service_account_file(
        SERVICE_ACCOUNT_FILE, scopes=SCOPES)

    # رفرش کردن توکن در صورت نیاز
    credentials.refresh(Request())

    # بازگرداندن توکن دسترسی
    return credentials.token

def send_notification(firebase_token, title, body, color="#33FF00"):
    """Send a notification to the user using Firebase Cloud Messaging (FCM)."""
    access_token = _get_access_token()
    url = 'https://fcm.googleapis.com/v1/projects/elderly-care-assistant/messages:send'
    headers = {
        'Authorization': f'Bearer {access_token}',
        'Content-Type': 'application/json'
    }
    payload = {
        "message": {
            "token": firebase_token,
            "notification": {
                "title": title,
                "body": body,

            },
            "android": {
                "notification": {
                    "color": color,  
                }
            },
            "data": {
                "color": color
}

        }
    }    
    response = requests.post(url, headers=headers, json=payload)
    return response.json()

def check_and_send_notification(message_id, username, delay=1.1):
    """
    Check if the message is still unread after a delay.
    If it is, send a notification to the user.
    """
    # time.sleep(delay)  # Wait for the specified delay

    conn = get_db_connection()
    cursor = conn.cursor()

    # Check if the message is still in unread_messages
    cursor.execute(
        "SELECT * FROM unread_messages WHERE user_username = ? AND message_id = ?",
        (username, message_id)
    )
    result = cursor.fetchone()

    if result:
        # If the message is still unread, send a notification
        cursor.execute("SELECT firebase_token FROM users WHERE username = ?", (username,))
        user_data = cursor.fetchone()

        if user_data and user_data["firebase_token"]:
            firebase_token = user_data["firebase_token"]
            # Retrieve message details for the notification
            cursor.execute("SELECT message_text FROM messages WHERE id = ?", (message_id,))
            message_data = cursor.fetchone()
            message_text = json.loads(message_data["message_text"])
            Message_type = message_text.get("Message Type", "_")
            color = "#33FF00"  # Default color  
            if Message_type == 'Event':
                event_title = message_text.get("Message Title", "New Event")
                danger_level_number = message_text.get("Danger Level", "Unknown")
                
                if danger_level_number == 0:
                    danger_level = "بدون خطر جدی"
                    color = "#33FF00"
                elif danger_level_number == 1:
                    danger_level = "وضعیت نامشخص"
                    color = "#FFA500"  # Orange 
                elif danger_level_number == 2:
                    danger_level = "خطر جدی"
                    color = "#FF0000"  # Red
                notification_title = f"{event_title}"
                notification_body = f"میزان خطر: {danger_level}"
                send_notification(firebase_token, notification_title, notification_body, color)
            elif Message_type == 'Wake up word':
                event_title = message_text.get("Message Title", "New Event")
                message_text_text = message_text.get("Message Text", "Unknown")
                notification_title = f"{event_title}"
                notification_body = f"{message_text_text}"

                # Send the notification
                send_notification(firebase_token, notification_title, notification_body, color)

    conn.close()

@app.route("/Live_Device", methods=["GET"])
def live_device():
    """Handle Live packet of devices"""
    dev, err, status = verify_device_auth(request)
    if err: return jsonify(err), status
    device_id = dev["device_id"]

    conn = get_db_connection()
    cursor = conn.cursor()

    # Mark last_seen on the devices row (replaces the active_devices logic).
    cursor.execute("UPDATE devices SET last_seen = ? WHERE device_id = ?",
                   (datetime.now().strftime('%Y-%m-%d %H:%M:%S'), device_id))
    conn.commit()

    # Keep going with the rest of the live_device flow — image_req, pill
    # alarms, call_pending — which historically wrote to active_devices.
    # We need that row to exist for backwards compat with the snapshot poll.
    cursor.execute("SELECT * FROM active_devices WHERE device_id = ?", (device_id,))
    device = cursor.fetchone()

    if device is None:
        # Auto-create the active_devices row on first /Live_Device call so we
        # don't need a separate /Setup_Active_Device endpoint anymore.
        cursor.execute(
            "INSERT INTO active_devices (device_id, key_hash, create_time, "
            "last_message_time, last_live_time, config_file) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            (device_id, dev["key_hash"], datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
             None, datetime.now().strftime('%Y-%m-%d %H:%M:%S'), ""),
        )
        conn.commit()
        device = cursor.execute(
            "SELECT * FROM active_devices WHERE device_id = ?", (device_id,)
        ).fetchone()
    cursor.execute('''
        UPDATE active_devices
        SET last_live_time = ?
        WHERE device_id = ?
    ''', (
        datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
        device_id
    ))
    message = "Device live time updated"
    
    # save changes and close db
    conn.commit()
    conn.close()

 # بررسی دیکشنری image_reqs
    image_req = False
    image_req_id = None

    for req_id, (req_device_id, _) in image_reqs.items():
        if req_device_id == device_id:
            image_req = True
            image_req_id = req_id
            break

    # Active pill alarms (computed from schedule + last_ack_at). Device plays
    # the audio + replays every 5 min until ack.
    active_pill_alarms = _active_pill_alarms_for(device_id)

    # Is there a Call_Offer waiting to be answered by this device, and is the
    # device currently "in a call" with a specific subscriber? in_call_by lets
    # the other subscribers' apps grey their call button so only one user can
    # be in a call at a time (Phase 4 single-call lock).
    call_pending = False
    in_call_by   = None
    with _call_lock:
        box = _call_mailbox.get(device_id)
        if box:
            _autoclear_stale_call(box)
            if box.get("offer") and not box.get("answer"):
                call_pending = True
            in_call_by = box.get("in_call_by")

    return jsonify({
        "message": "Device live time updated",
        "image_req":          image_req,
        "image_req_id":       image_req_id,
        "active_pill_alarms": active_pill_alarms,
        "call_pending":       call_pending,
        "in_call_by":         in_call_by,
    }), 200

@app.route("/Config_Device", methods=["POST"])
def config_device():
    """Handle Config_Device packet."""
    dev, err, status = verify_device_auth(request)
    if err: return jsonify(err), status
    device_id = dev["device_id"]

    conn = get_db_connection()
    cursor = conn.cursor()

    # Extract JSON body
    try:
        config_data = request.get_json()
    except Exception:
        return jsonify({"error": "Invalid JSON body"}), 400

    new_key_hash = config_data.get("key-Hash")
    if not new_key_hash:
        return jsonify({"error": "New key-Hash missing in body"}), 400


    cursor.execute("SELECT * FROM active_devices WHERE device_id = ?", (device_id,))
    device = cursor.fetchone()


    if device is None:
        # add new device
        cursor.execute('''
            INSERT INTO active_devices (device_id, key_hash, last_message_time, last_live_time, config_file)
            VALUES (?, ?, ?, ?, ?)
        ''', (
            device_id,
            new_key_hash,
            None,  # Last message time
            datetime.now().strftime('%Y-%m-%d %H:%M:%S'),  # Last live time
            json.dumps(config_data)  # Default config file
        ))
        message = "New device added"
    else:
    # Update the device config
        cursor.execute('''
            UPDATE active_devices
            SET key_hash = ?, config_file = ?, last_live_time = ?
            WHERE device_id = ?
        ''', (
            new_key_hash,  # Update the key
            json.dumps(config_data),  # Store the entire config as JSON
            datetime.now().strftime('%Y-%m-%d %H:%M:%S'),  # Update live time
            device_id
        ))
        message = "Device config updated"
    
    # save changes and close db
    conn.commit()
    conn.close()

    return jsonify({"message": message}), 200

@app.route("/Message_Device", methods=["POST"])
def message_device():
    """Handle Message_Device packet."""

    def add_message_to_unread_for_user(message_id, user_username):
        """Add the message to the unread_messages table for a specific user."""
        conn = get_db_connection()
        cursor = conn.cursor()

        cursor.execute(
            "INSERT INTO unread_messages (user_username, message_id) VALUES (?, ?)",
            (user_username, message_id)
        )

        conn.commit()
        conn.close()


    def add_message_to_unread_for_subscribers(message_id):
        """Add the message to the unread_messages table for all subscribed users and check if notifications are needed."""
        conn = get_db_connection()
        cursor = conn.cursor()

        # Retrieve the device ID and message details associated with the message
        cursor.execute("SELECT device_id, message_text FROM messages WHERE id = ?", (message_id,))
        result = cursor.fetchone()

        if result is None:
            conn.close()
            raise ValueError(f"No message found with ID {message_id}")

        device_id = result["device_id"]

        # Find all users subscribed to the device
        cursor.execute(
            "SELECT username FROM subscriptions WHERE device_id = ?", (device_id,)
        )
        subscribed_users = cursor.fetchall()

        # Add the message to unread_messages for each user
        for user in subscribed_users:
            username = user["username"]
            cursor.execute(
                "INSERT INTO unread_messages (user_username, message_id) VALUES (?, ?)",
                (username, message_id)
            )

            # Start a thread to check if the message is still unread after a delay
            threading.Thread(target=check_and_send_notification, args=(message_id, username)).start()

        conn.commit()
        conn.close()

    dev, err, status = verify_device_auth(request)
    if err: return jsonify(err), status
    device_id = dev["device_id"]

    conn = get_db_connection()
    cursor = conn.cursor()

    cursor.execute("SELECT * FROM active_devices WHERE device_id = ?", (device_id,))
    device = cursor.fetchone()

    if device is None:
        conn.close()
        return jsonify({"error": "No active device found"}), 404

    # Process multipart/form-data
    message_text = request.form.get("text")
    message_image = request.files.get("image file")
    message_video = request.files.get("video file")

    if not message_text or not message_image:
        conn.close()
        return jsonify({"error": "Missing message text or image file"}), 400

    try:
        # Parse the JSON text
        message_data = json.loads(message_text)
        message_time = message_data.get("Message Time")
        message_type = message_data.get("Message Type")

        if not message_time or not message_type:
            raise ValueError("Message Time or Message Type missing")
    except Exception as e:
        conn.close()
        return jsonify({"error": f"Invalid message text: {str(e)}"}), 400

    # Save the image file
    image_file_path = os.path.join(UPLOAD_FOLDER, f"{device_id}_{datetime.now().strftime('%Y%m%d%H%M%S')}_image.bin")
    video_file_path = os.path.join(UPLOAD_FOLDER, f"{device_id}_{datetime.now().strftime('%Y%m%d%H%M%S')}_video.bin")
    message_image.save(image_file_path)

    if message_video is not None:
        message_video.save(video_file_path)

    # Insert message into database
    cursor.execute('''
        INSERT INTO messages (device_id, message_type, message_text, message_image, message_video, message_occur_time)
        VALUES (?, ?, ?, ?, ?, ?)
    ''', (
        device_id,
        message_type,
        message_text,
        image_file_path,
        video_file_path,
        message_time
    ))
    conn.commit()

    message_id = cursor.lastrowid

    # Handle unread messages based on Message Type
    if message_type == "image captured":
        image_req_id = message_data.get("image_req_id")
        if image_req_id is None:
            conn.close()
            return jsonify({"error": "image_req_id missing for image captured message"}), 400

        # Find the user associated with image_req_id from the dictionary
        req_data = image_reqs.get(str(image_req_id))

        if req_data:
            device_id, username = req_data
            add_message_to_unread_for_user(message_id, username)
            # Remove the used image_req_id from the dictionary
            del image_reqs[str(image_req_id)]
        else:
            conn.close()
            return jsonify({"error": "No user found for image_req_id"}), 404
        
    elif message_type == "Event":
        try:
            add_message_to_unread_for_subscribers(message_id)
        except ValueError as e:
            conn.close()
            return jsonify({"error": str(e)}), 500
        # Phase 5.1 — push to all subscribers via MQTT. App's MqttService
        # opens the fall details directly, no /Message_Check_App poll needed.
        mqtt_pub.event_fall(device_id, message_id, message_time,
                            image_url=f"/Media/{os.path.basename(image_file_path)}",
                            video_url=f"/Media/{os.path.basename(video_file_path)}"
                                      if message_video is not None else "")

    elif message_type == "Wake up word":
        try:
            add_message_to_unread_for_subscribers(message_id)
        except ValueError as e:
            conn.close()
            return jsonify({"error": str(e)}), 500
        # Wake-word events also fan out to subscribers.
        mqtt_pub.event_wake_command(device_id, "wake", message_text or "", message_id)

    conn.commit()
    conn.close()

    return jsonify({"message": "Message recorded successfully"}), 200

@app.route("/Registeration_App", methods=["POST"])
def registeration_app():
    """Handle user registration packets."""
    packet_type = request.headers.get("Packet")
    username = request.headers.get("Username")
    data = request.get_json()

    if not packet_type or not username:
        return jsonify({"error": "Packet or Username missing"}), 400

    if packet_type == "Create_Account":
        # Sign-up is device-agnostic: anyone with a working email can create
        # an account. Device subscription happens AFTER login via the
        # Phase 2 /Subscribe_Device / /Register_Device endpoints.
        #
        # We only validate that the username (email) isn't already taken and
        # that we haven't already sent a code we're still waiting on.
        conn = get_db_connection()
        cursor = conn.cursor()
        cursor.execute('SELECT 1 FROM users WHERE username = ?', (username,))
        if cursor.fetchone() is not None:
            conn.close()
            return jsonify({"error": "Username already exists"}), 409
        conn.close()

        if username in validation_codes:
            return jsonify({
                "error": "You have to wait for the time-out to pass then you can send another request"
            }), 403

        # Generate the code synchronously (stored in `validation_codes` dict
        # for the next /Registeration_App[Validation_Code] step). Fire the
        # actual SMTP send in a background thread — Gmail's TLS handshake
        # from Iran takes 10-30s and would otherwise block the HTTP response.
        validation_code = random.randint(100000, 999999)
        add_member_to_dictionary(username, validation_code)
        threading.Thread(
            target=send_validation_code_to_user_email,
            args=(username, validation_code),
            daemon=True,
        ).start()

        return jsonify({"message": "Validation code sent to email"}), 200

    elif packet_type == "Validation_Code":
        # Validate the code
        try:
            user_code = data.get("Validation_Code")
            user_password = data.get("Set_Password")
        except Exception:
            return jsonify({"error": "Invalid JSON"}), 400
        
        if username not in validation_codes:
            remove_validation_code(username)
            return jsonify({"error": "Invalid validation code"}), 403

        if validation_codes[username] != int(user_code):
            remove_validation_code(username)
            return jsonify({"error": "Invalid validation code"}), 403

        # Store user data in the database
        conn = get_db_connection()
        cursor = conn.cursor()
        try:
            cursor.execute('''
                INSERT INTO users (username, password, last_login_time, last_message_time, last_message_check_time, config_file)
                VALUES (?, ?, ?, ?, ?, ?)
            ''', (
                username,
                user_password,  # Password stored directly; consider hashing for security
                None,  # last login time
                None,  # Last message time
                None,  # Last message check time
                '{}',  # Default config as an empty JSON
            ))
            conn.commit()
        except sqlite3.IntegrityError:
            conn.close()
            return jsonify({"error": "Username already exists"}), 409
        conn.close()


        remove_validation_code(username)
        return jsonify({"message": "User registered successfully"}), 200

    else:
        return jsonify({"error": "Invalid packet type"}), 400
    
@app.route("/Login_App", methods=["POST"])
def login_app():
    """Handle Login_App packet."""
    username = request.headers.get("Username")
    password = request.headers.get("Password")

    if not username or not password:
        return jsonify({"error": "Username or Password missing"}), 400

    # Connect to the database
    conn = get_db_connection()
    cursor = conn.cursor()

    # Check if the user exists
    cursor.execute("SELECT * FROM users WHERE username = ?", (username,))
    user = cursor.fetchone()

    if user is None:
        conn.close()
        return jsonify({"error": "Invalid username or password"}), 403

    # Check if the password is correct
    if user["password"] != password:
        conn.close()
        return jsonify({"error": "Invalid username or password"}), 403
    
    

    # Fetch user's subscribed devices
    cursor.execute("SELECT device_id FROM subscriptions WHERE username = ?", (username,))
    devices = cursor.fetchall()

    # Fetch messages for each subscribed device
    device_data = {}
    for device in devices:
        
        device_id = device["device_id"]
        cursor.execute("SELECT * FROM messages WHERE device_id = ? AND message_type = ?", (device_id, "Event"))
        messages = cursor.fetchall()

        # Collect message data with Base64 encoded files
        device_data[device_id] = []
        
        for message in messages:
            
            message_image_content = read_file_as_base64(message["message_image"])
            message_video_content = read_file_as_base64(message["message_video"])
            
            device_data[device_id].append({
                "device_id":message["device_id"],
                "message_id": message["message_id"],
                "message_type": message["message_type"],
                "message_text": message["message_text"],
                "message_image": message_image_content,  
                "message_video": message_video_content,  
                "message_occur_time": message["message_occur_time"]
            })
        

    # Update user's last_login_time
    cursor.execute("UPDATE users SET last_login_time = ? WHERE username = ?", (datetime.now(), username))
    conn.commit()
    

    # Close database connection
    conn.close()

    has_any_messages = any(len(messages) > 0 for messages in device_data.values())

    # Prepare the response
    if has_any_messages:
        
        response_data = {
            "message":"Login to account successfully completed.",
            "config_file": json.loads(user["config_file"]),  # Convert config_file to JSON
            "subscribed_devices": device_data
        }
        
        final_response = jsonify(response_data)
    else:
        final_response = {}

    
    return final_response, 200

@app.route("/Subscribe_App", methods=["POST"])
def subscribe_app():
    """Handle Subscribe_App packet."""
    username = request.headers.get("Username")
    password = request.headers.get("Password")
    data = request.get_json()

    if not username or not password or not data:
        return jsonify({"error": "Missing username, password, or request body"}), 400

    device_id = data.get("device_ID")
    key_hash = data.get("Key_Hash")

    if not device_id or not key_hash:
        return jsonify({"error": "Missing device_ID or Key_Hash"}), 400

    # Connect to the database
    conn = get_db_connection()
    cursor = conn.cursor()

    # Check if the user exists and password is correct
    cursor.execute("SELECT * FROM users WHERE username = ?", (username,))
    user = cursor.fetchone()

    if user is None or user["password"] != password:
        conn.close()
        return jsonify({"error": "Invalid username or password"}), 403

    # Check if the device exists and key_hash matches
    cursor.execute("SELECT * FROM active_devices WHERE device_id = ?", (device_id,))
    device = cursor.fetchone()

    if device is None or device["key_hash"] != key_hash:
        conn.close()
        return jsonify({"error": "Invalid device ID or Key_Hash"}), 403

    # Check if subscription already exists
    cursor.execute(
        "SELECT * FROM subscriptions WHERE username = ? AND device_id = ?",
        (username, device_id)
    )
    existing_subscription = cursor.fetchone()

    if existing_subscription:
        conn.close()
        return jsonify({"message": "Subscription already exists"}), 200

    # Add subscription
    cursor.execute(
        "INSERT INTO subscriptions (username, device_id) VALUES (?, ?)",
        (username, device_id)
    )
    conn.commit()

    # Fetch messages for the device
    cursor.execute("SELECT * FROM messages WHERE device_id = ?", (device_id,))
    messages = cursor.fetchall()

    # Collect message data with Base64 encoded files
    message_archive = []
    for message in messages:
        if message["message_type"] == "Event":
            message_file_content = read_file_as_base64(message["message_file"])
            message_archive.append({
                "device_id":message["device_id"],
                "message_id": message["id"],
                "message_type": message["message_type"],
                "message_text": message["message_text"],
                "message_file": message_file_content,  # File content as Base64
                "message_occur_time": message["message_occur_time"]
            })

    conn.close()

    # Return archived messages
    return jsonify({"message": "Subscription added successfully" ,"device_ID": device_id, "message_archive": message_archive}), 200

@app.route("/Config_App", methods=["POST"])
def config_app():
    """Handle Config_App packet."""
    username = request.headers.get("Username")
    password = request.headers.get("Password")
    config_data = request.get_json()

    if not username or not password or not config_data:
        return jsonify({"error": "Missing username, password, or configuration data"}), 400

    # Connect to the database
    conn = get_db_connection()
    cursor = conn.cursor()
    
    # Check if the user exists and password is correct
    cursor.execute("SELECT * FROM users WHERE username = ?", (username,))
    user = cursor.fetchone()

    if user is None or user["password"] != password:
        conn.close()
        return jsonify({"error": "Invalid username or password"}), 403

    # Extract firebase_token from config_data
    firebase_token = config_data.get("firebase_token")
    
    # Update the user's config_file and firebase_token fields
    config_json = json.dumps(config_data)  # Convert config_data to a JSON string
    cursor.execute(
        "UPDATE users SET config_file = ?, firebase_token = ? WHERE username = ?",
        (config_json, firebase_token, username)
    )
    conn.commit()
    conn.close()

    return jsonify({"message": "Configuration updated successfully"}), 200

@app.route("/Message_Check_App", methods=["POST"])
def message_check_app():
    """Handle Message_Check_App packet."""
    username = request.headers.get("Username")
    password = request.headers.get("Password")

    if not username or not password:
        return jsonify({"error": "Missing username or password"}), 400

    # Connect to the database
    conn = get_db_connection()
    cursor = conn.cursor()

    # Check if the user exists and password is correct
    cursor.execute("SELECT * FROM users WHERE username = ?", (username,))
    user = cursor.fetchone()

    if user is None or user["password"] != password:
        conn.close()
        return jsonify({"error": "Invalid username or password"}), 403

    # Fetch unread messages for the user
    cursor.execute(
        """
        SELECT e.id AS message_id, e.message_type, e.message_text, e.message_image, e.message_video, e.message_occur_time, e.device_id
        FROM unread_messages ue
        JOIN messages e ON ue.message_id = e.id
        WHERE ue.user_username = ?
        """,
        (username,)
    )
    unread_messages = cursor.fetchall()

    # If no unread messages, return an appropriate message
    if not unread_messages:
        conn.close()
        return jsonify({"message": "No new messages found"}), 200

    # Prepare the messages for response
    message_list = []
    for message in unread_messages:
        message_image_content = read_file_as_base64(message["message_image"])
        if message["message_video"] is not None:
            message_video_content = read_file_as_base64(message["message_video"])
        else:
            message_video_content = None
        message_list.append({
                "device_id":message["device_id"],
                "message_id": message["message_id"],
                "message_type": message["message_type"],
                "message_text": message["message_text"],
                "message_image": message_image_content,  
                "message_video": message_video_content,  
                "message_occur_time": message["message_occur_time"]
        })

    # Remove the messages from unread_messages after sending
    message_ids = [message["message_id"] for message in unread_messages]
    cursor.executemany(
        "DELETE FROM unread_messages WHERE user_username = ? AND message_id = ?",
        [(username, message_id) for message_id in message_ids]
    )

    file_pathes = [message["message_image"] for message in unread_messages if message["message_type"] == "image captured"]

    for file_path in file_pathes:
        try:
            os.remove(file_path)
            print(f"Removed: {file_path}")
        except FileNotFoundError:
            print(f"File not found: {file_path}")
        except PermissionError:
            print(f"Permission denied: {file_path}")
        except Exception as e:
            print(f"Error removing {file_path}: {e}")

    conn.commit()
    conn.close()

    return jsonify({"new_messages": message_list}), 200

@app.route("/Image_Request_App", methods=["POST"])
def image_request_app():
    username = request.headers.get("Username")
    password = request.headers.get("Password")

    if not username or not password:
        return jsonify({"error": "Missing username or password"}), 400

    data = request.get_json()
    device_id = data.get("device_id")
    req_id = data.get("req_id")

    if not device_id or req_id is None:
        return jsonify({"error": "Missing device_id or req_id"}), 400

    conn = get_db_connection()
    cursor = conn.cursor()

    # بررسی کاربر
    cursor.execute("SELECT * FROM users WHERE username = ?", (username,))
    user = cursor.fetchone()

    if user is None or user["password"] != password:
        conn.close()
        return jsonify({"error": "Invalid username or password"}), 403

    # بررسی سابسکرایب بودن کاربر به دستگاه
    cursor.execute("SELECT * FROM subscriptions WHERE username = ? AND device_id = ?", (username, device_id))
    subscription = cursor.fetchone()

    if subscription is None:
        conn.close()
        return jsonify({"error": "User is not subscribed to the device"}), 403

    # افزودن به دیکشنری
    image_reqs[str(req_id)] = [device_id, username]

    conn.close()
    return jsonify({"message": "Image request added successfully"}), 200


# ---------------------------------------------------------------------------
# Voice messages: device records a short clip and uploads it; app fetches
# the list of voice messages per device. Files live under UPLOAD_FOLDER and
# rows go into the existing `messages` table with message_type='Voice'.
# ---------------------------------------------------------------------------
@app.route("/Voice_Message_Device", methods=["POST"])
def voice_message_device():
    dev, err, status = verify_device_auth(request)
    if err: return jsonify(err), status
    device_id = dev["device_id"]

    conn = get_db_connection()
    cursor = conn.cursor()

    audio = request.files.get("audio")
    if audio is None:
        conn.close()
        return jsonify({"error": "Missing audio file"}), 400

    ts = datetime.now().strftime('%Y%m%d%H%M%S')
    audio_path = os.path.join(UPLOAD_FOLDER, f"{device_id}_{ts}_voice.m4a")
    audio.save(audio_path)

    occurred = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    cursor.execute('''
        INSERT INTO messages (device_id, message_type, message_text, message_image, message_video, message_occur_time)
        VALUES (?, ?, ?, ?, ?, ?)
    ''', (device_id, "Voice", json.dumps({"Message Time": occurred, "Message Type": "Voice"}), "", audio_path, occurred))
    msg_id = cursor.lastrowid

    cursor.execute("SELECT username FROM subscriptions WHERE device_id = ?", (device_id,))
    subscriber_rows = cursor.fetchall()
    for row in subscriber_rows:
        cursor.execute(
            "INSERT INTO unread_messages (user_username, message_id) VALUES (?, ?)",
            (row["username"], msg_id),
        )

    conn.commit()
    conn.close()

    # MQTT push — every subscribed user's app gets an inbox refresh.
    mqtt_pub.event_voice(device_id, msg_id, occurred,
                         audio_url=f"/Media/{os.path.basename(audio_path)}")
    for row in subscriber_rows:
        mqtt_pub.notify_user(row["username"], {
            "type": "voice", "device_id": device_id, "message_id": msg_id,
        })
    return jsonify({"message": "voice stored", "message_id": msg_id}), 200


@app.route("/Voice_Messages_App", methods=["POST"])
def voice_messages_app():
    username = request.headers.get("Username")
    password = request.headers.get("Password")
    body = request.get_json(silent=True) or {}
    device_id = body.get("device_id")
    if not username or not password or not device_id:
        return jsonify({"error": "username, password, device_id required"}), 400

    conn = get_db_connection()
    cursor = conn.cursor()
    cursor.execute("SELECT * FROM users WHERE username = ?", (username,))
    user = cursor.fetchone()
    if user is None or user["password"] != password:
        conn.close()
        return jsonify({"error": "Invalid username or password"}), 403

    cursor.execute(
        "SELECT * FROM subscriptions WHERE username = ? AND device_id = ?",
        (username, device_id),
    )
    if cursor.fetchone() is None:
        conn.close()
        return jsonify({"error": "Not subscribed to this device"}), 403

    cursor.execute('''
        SELECT id, message_occur_time, message_video FROM messages
        WHERE device_id = ? AND message_type = 'Voice'
        ORDER BY id DESC LIMIT 100
    ''', (device_id,))

    out = []
    for row in cursor.fetchall():
        path = row["message_video"]
        b64 = None
        if path and os.path.isfile(path):
            with open(path, "rb") as f:
                b64 = base64.b64encode(f.read()).decode("ascii")
        out.append({
            "id": row["id"],
            "occurred": row["message_occur_time"],
            "audio_b64": b64,
        })
    conn.close()
    return jsonify({"messages": out}), 200


# ---------------------------------------------------------------------------
# Pill schedules — the device plays the recorded pill-reminder audio every
# 5 min until the elderly user says "قرص" (wake-word → /Wake_Command → PILL
# intent). The schedule is owned by the app (local SQLite per pill); the app
# pushes the schedule + audio file here so the device can fetch them.
# ---------------------------------------------------------------------------

PILL_AUDIO_DIR = os.path.join(UPLOAD_FOLDER, "pill_audio")
os.makedirs(PILL_AUDIO_DIR, exist_ok=True)

# How long an alarm stays "active" once its scheduled time arrives, before
# we give up and stop trying (assume missed dose). 1 hour is generous.
_PILL_ACTIVE_WINDOW_SEC = 60 * 60


def _today_at(hhmm: str) -> "datetime":
    """Parse 'HH:MM' string into today's datetime."""
    h, m = hhmm.strip().split(":")
    now = datetime.now()
    return now.replace(hour=int(h), minute=int(m), second=0, microsecond=0)


def _active_pill_alarms_for(device_id: str) -> list:
    """Phase 2.5 scheduler — replaces daily-fixed-times with course-of-treatment.

    For each pill, the next dose time is `start_at_utc + done * interval`. We
    advance the `consumptions_done` counter past any doses whose 1-hour active
    window has fully elapsed (missed-dose policy = skip; pharmacologically
    correct, no double-dose risk). Once `done >= total_count`, the course is
    finished and the row is never active again.

    A pill is "active right now" iff `next_due <= now <= next_due + WINDOW`."""
    from datetime import timezone
    conn = get_db_connection()
    cur  = conn.cursor()
    cur.execute(
        "SELECT id, pill_id_local, name, start_at_utc, interval_seconds, "
        "       total_count, consumptions_done "
        "FROM pill_schedules WHERE device_id = ?", (device_id,)
    )
    rows = cur.fetchall()

    now = datetime.now(timezone.utc).replace(tzinfo=None)
    active = []
    dirty_rows = []  # (id, new_done) pairs to persist after skip-forward
    for r in rows:
        try:
            start_at = datetime.strptime(r["start_at_utc"], "%Y-%m-%d %H:%M:%S")
        except Exception:
            continue
        interval = int(r["interval_seconds"])
        total    = int(r["total_count"])
        done     = int(r["consumptions_done"])
        if interval <= 0 or total <= 0:
            continue

        # Skip-forward: advance past any doses whose active window has fully
        # passed. Persist if we skipped any.
        while done < total:
            next_due = start_at + timedelta(seconds=done * interval)
            if (now - next_due).total_seconds() > _PILL_ACTIVE_WINDOW_SEC:
                done += 1
                continue
            break
        if done != r["consumptions_done"]:
            dirty_rows.append((r["id"], done))

        if done >= total:
            continue   # course finished

        next_due = start_at + timedelta(seconds=done * interval)
        if next_due <= now <= next_due + timedelta(seconds=_PILL_ACTIVE_WINDOW_SEC):
            active.append({
                "id":        r["id"],
                "pill_id":   r["pill_id_local"],
                "name":      r["name"],
                "due_at":    next_due.strftime("%Y-%m-%d %H:%M:%S"),
                "audio_url": f"/Pill_Audio/{r['id']}",
                "dose":      done + 1,        # 1-indexed for UI
                "of":        total,
            })

    # Persist any skip-forward updates so we don't recompute them on every poll.
    if dirty_rows:
        cur.executemany(
            "UPDATE pill_schedules SET consumptions_done = ? WHERE id = ?",
            [(done, pid) for (pid, done) in dirty_rows],
        )
        conn.commit()
    conn.close()
    return active


@app.route("/Pill_Sync_App", methods=["POST"])
def pill_sync_app():
    """App pushes the pill schedule (+ recorded audio) to the server.
    Multipart form: text=<json>, audio file=<m4a or wav>.

    JSON: {
      "device_id":        "...",
      "pill_id_local":    N,                       # app's local pill_id
      "name":             "...",
      "start_at_utc":     "YYYY-MM-DD HH:MM:SS",   # course starts here
      "interval_seconds": 28800,                   # e.g. 8h = 28800
      "total_count":      21                       # total doses in course
    }

    The app converts the user's Jalali date+time → Gregorian → UTC before
    POSTing, so the server only deals with ISO UTC. Re-POSTing the same
    `device_id + pill_id_local` upserts (replaces) the row but RESETS the
    `consumptions_done` counter — the app should only re-sync on schedule
    edits, not on every UI refresh."""
    text  = request.form.get("text")
    audio = request.files.get("audio file")
    if not text:
        return jsonify({"error": "text field required"}), 400
    try:
        data = json.loads(text)
    except Exception:
        return jsonify({"error": "invalid JSON in text field"}), 400

    device_id        = data.get("device_id")
    pill_id_local    = data.get("pill_id_local")
    name             = data.get("name", "")
    start_at_utc     = data.get("start_at_utc")
    interval_seconds = data.get("interval_seconds")
    total_count      = data.get("total_count")
    if (not device_id or pill_id_local is None or not start_at_utc
            or not interval_seconds or not total_count):
        return jsonify({
            "error": "device_id, pill_id_local, start_at_utc, "
                     "interval_seconds, total_count required"
        }), 400
    # Validate the ISO timestamp parses.
    try:
        datetime.strptime(start_at_utc, "%Y-%m-%d %H:%M:%S")
    except Exception:
        return jsonify({"error": "start_at_utc must be 'YYYY-MM-DD HH:MM:SS' UTC"}), 400

    # Audio transcode (unchanged from before — m4a → WAV so device can aplay).
    audio_path = ""
    if audio is not None:
        ts = datetime.now().strftime("%Y%m%d%H%M%S")
        raw_path = os.path.join(PILL_AUDIO_DIR,
                                f"{device_id}_{pill_id_local}_{ts}.raw")
        wav_path = os.path.join(PILL_AUDIO_DIR,
                                f"{device_id}_{pill_id_local}.wav")
        audio.save(raw_path)
        rc = os.system(
            f'ffmpeg -y -i "{raw_path}" -acodec pcm_s16le -ar 16000 '
            f'-ac 1 "{wav_path}" > /dev/null 2>&1'
        )
        try: os.remove(raw_path)
        except OSError: pass
        if rc == 0:
            audio_path = wav_path
        else:
            return jsonify({"error": "ffmpeg transcode failed"}), 500

    conn = get_db_connection()
    cur  = conn.cursor()
    cur.execute(
        """INSERT INTO pill_schedules
              (device_id, pill_id_local, name, audio_path,
               start_at_utc, interval_seconds, total_count, consumptions_done)
           VALUES (?, ?, ?, ?, ?, ?, ?, 0)
           ON CONFLICT(device_id, pill_id_local) DO UPDATE SET
              name              = excluded.name,
              audio_path        = COALESCE(NULLIF(excluded.audio_path, ''), audio_path),
              start_at_utc      = excluded.start_at_utc,
              interval_seconds  = excluded.interval_seconds,
              total_count       = excluded.total_count,
              consumptions_done = 0,
              last_ack_at       = NULL
        """,
        (device_id, pill_id_local, name, audio_path,
         start_at_utc, int(interval_seconds), int(total_count))
    )
    conn.commit()
    conn.close()
    # Push the now-current active-alarm list so the device sees the new
    # schedule without waiting for the next /Live_Device poll.
    mqtt_pub.state_pill(device_id, _active_pill_alarms_for(device_id))
    return jsonify({"message": "pill synced"}), 200


@app.route("/Media/<path:filename>", methods=["GET"])
def serve_media(filename: str):
    """Binary media file served by name. URLs published in MQTT event
    payloads point at this endpoint so subscribers can fetch the image/video
    on demand instead of receiving giant base64 in every poll response.
    Path is restricted to UPLOAD_FOLDER to prevent directory traversal."""
    from flask import send_from_directory
    safe = os.path.basename(filename)
    full = os.path.join(UPLOAD_FOLDER, safe)
    if not os.path.exists(full):
        return jsonify({"error": "not_found"}), 404
    return send_from_directory(UPLOAD_FOLDER, safe)


@app.route("/Pill_Audio/<int:pill_db_id>", methods=["GET"])
def pill_audio(pill_db_id: int):
    """Stream the transcoded WAV for a pill so the device can `aplay` it."""
    from flask import send_file
    conn = get_db_connection()
    cur  = conn.cursor()
    cur.execute("SELECT audio_path FROM pill_schedules WHERE id = ?", (pill_db_id,))
    row = cur.fetchone()
    conn.close()
    if row is None or not row["audio_path"] or not os.path.exists(row["audio_path"]):
        return jsonify({"error": "audio not found"}), 404
    return send_file(row["audio_path"], mimetype="audio/wav")


@app.route("/Pill_Ack_Device", methods=["POST"])
def pill_ack_device():
    """Device tells us the user just acked their pill (said "قرص").

    Two modes:
      - If `pill_schedule_id` is provided in the body, ack that exact row.
      - Otherwise, ack ALL currently-active alarms for the device. One wake-
        fire clears every due reminder so the elder doesn't have to repeat
        the wake word per pill. (Decision: 2026-05-24 — see
        feedback_pill_ack_scope memory.)"""
    device_id = request.headers.get("Device-ID") or request.headers.get("Device-Id")
    if not device_id:
        return jsonify({"error": "Device-ID required"}), 400

    body = request.get_json(silent=True) or {}
    target_id = body.get("pill_schedule_id")
    now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    conn = get_db_connection()
    cur  = conn.cursor()

    if target_id is not None:
        # Single-pill ack — increment consumptions_done by 1 (course advances).
        cur.execute(
            "UPDATE pill_schedules SET last_ack_at = ?, "
            "       consumptions_done = consumptions_done + 1 "
            "WHERE id = ? AND device_id = ? AND consumptions_done < total_count",
            (now_str, target_id, device_id),
        )
        acked_ids = [target_id] if cur.rowcount else []
    else:
        # Bulk ack — every currently-active alarm advances by one dose.
        active = _active_pill_alarms_for(device_id)
        acked_ids = [a["id"] for a in active]
        if acked_ids:
            placeholders = ",".join("?" for _ in acked_ids)
            cur.execute(
                f"UPDATE pill_schedules SET last_ack_at = ?, "
                f"       consumptions_done = consumptions_done + 1 "
                f"WHERE device_id = ? AND id IN ({placeholders}) "
                f"  AND consumptions_done < total_count",
                (now_str, device_id, *acked_ids),
            )
    conn.commit()
    conn.close()
    # The just-acked alarms have advanced consumptions_done. Push the
    # remaining active list (probably empty until the next interval) so the
    # device stops replaying and subscribers' apps clear the badge.
    mqtt_pub.state_pill(device_id, _active_pill_alarms_for(device_id))
    return jsonify({
        "acked": len(acked_ids),
        "pill_schedule_ids": acked_ids,
    }), 200


@app.route("/Pill_Delete_App", methods=["POST"])
def pill_delete_app():
    """App removed a pill — purge the schedule + audio file."""
    body = request.get_json(silent=True) or {}
    device_id     = body.get("device_id")
    pill_id_local = body.get("pill_id_local")
    if not device_id or pill_id_local is None:
        return jsonify({"error": "device_id and pill_id_local required"}), 400
    conn = get_db_connection()
    cur  = conn.cursor()
    cur.execute(
        "SELECT audio_path FROM pill_schedules "
        "WHERE device_id = ? AND pill_id_local = ?",
        (device_id, pill_id_local)
    )
    row = cur.fetchone()
    if row and row["audio_path"] and os.path.exists(row["audio_path"]):
        try: os.remove(row["audio_path"])
        except OSError: pass
    cur.execute(
        "DELETE FROM pill_schedules WHERE device_id = ? AND pill_id_local = ?",
        (device_id, pill_id_local)
    )
    conn.commit()
    conn.close()
    mqtt_pub.state_pill(device_id, _active_pill_alarms_for(device_id))
    return jsonify({"message": "pill deleted"}), 200


# ---------------------------------------------------------------------------
# WebRTC signaling for the in-app video call (app <-> device).
# These endpoints just shuttle SDP offers/answers and ICE candidates between
# the two peers without inspecting them. Keyed by device_id.
# ---------------------------------------------------------------------------
_call_mailbox = {
    # device_id: {
    #   "offer": {...sdp...} or None,
    #   "answer": {...sdp...} or None,
    #   "app_ice": [candidate, ...],
    #   "device_ice": [candidate, ...],
    #   "in_call_by": "username" or None,   # Phase 4 single-call lock
    #   "started_at": datetime or None,      # auto-clear after 30 min stale
    # }
}
_call_lock = threading.Lock()
_CALL_MAX_DURATION_SEC = 30 * 60   # safety net for crashed clients


def _mailbox(device_id):
    box = _call_mailbox.get(device_id)
    if box is None:
        box = {"offer": None, "answer": None, "app_ice": [], "device_ice": [],
               "in_call_by": None, "started_at": None}
        _call_mailbox[device_id] = box
    return box


def _autoclear_stale_call(box):
    """If a previous call's `started_at` is older than 30 min, treat the lock
    as abandoned (likely a crashed client that never POSTed /Call_End)."""
    if box["in_call_by"] and box["started_at"]:
        if (datetime.now() - box["started_at"]).total_seconds() > _CALL_MAX_DURATION_SEC:
            box["in_call_by"] = None
            box["started_at"] = None


def call_in_progress_user(device_id):
    """Public helper used by /Live_Device to embed in_call_by in the heartbeat
    response so all 3 subscribers can render the "in call by X" badge."""
    with _call_lock:
        box = _call_mailbox.get(device_id)
        if not box:
            return None
        _autoclear_stale_call(box)
        return box["in_call_by"]


@app.route("/Call_Offer", methods=["POST"])
def call_offer():
    """App posts offer for a device, or polls (GET-via-POST with no body).

    Single-call lock (Phase 4): if another subscriber's already in a call
    with this device, posting a new offer returns 409 with `in_call_by`.
    First poster wins and sets the lock. The lock is cleared by /Call_End
    or auto-expires after 30 min (crashed-client safety net)."""
    body = request.get_json(silent=True) or {}
    device_id = body.get("device_id") or request.headers.get("Device-ID")
    username  = request.headers.get("Username")  # caller identity for the lock
    if not device_id:
        return jsonify({"error": "device_id required"}), 400

    with _call_lock:
        box = _mailbox(device_id)
        _autoclear_stale_call(box)

        if "sdp" in body:
            # Posting an offer = claim the lock. Reject if someone else holds it.
            if box["in_call_by"] and box["in_call_by"] != username:
                return jsonify({
                    "error":      "in_call_by_other",
                    "in_call_by": box["in_call_by"],
                }), 409
            box["in_call_by"] = username or "anonymous"
            box["started_at"] = datetime.now()
            box["offer"] = {"sdp": body["sdp"], "type": body.get("type", "offer")}
            box["answer"] = None
            box["app_ice"] = []
            box["device_ice"] = []
            # Push call state — device starts wake_call_helper, other subs
            # see "in call by X" badge.
            mqtt_pub.state_call_pending(device_id, True, box["in_call_by"])
            return jsonify({"message": "offer stored"}), 200
        # Poll path — the device side. No lock interaction.
        return jsonify({"offer": box["offer"]}), 200


@app.route("/Call_Answer", methods=["POST"])
def call_answer():
    """Device posts answer for an offer, or app polls for the answer."""
    body = request.get_json(silent=True) or {}
    device_id = body.get("device_id") or request.headers.get("Device-ID")
    if not device_id:
        return jsonify({"error": "device_id required"}), 400

    with _call_lock:
        box = _mailbox(device_id)
        if "sdp" in body:
            box["answer"] = {"sdp": body["sdp"], "type": body.get("type", "answer")}
            return jsonify({"message": "answer stored"}), 200
        return jsonify({"answer": box["answer"]}), 200


@app.route("/Call_Ice", methods=["POST"])
def call_ice():
    """Both sides push their local ICE candidates and pull the peer's."""
    body = request.get_json(silent=True) or {}
    device_id = body.get("device_id") or request.headers.get("Device-ID")
    side = body.get("side")  # "app" or "device"
    if not device_id or side not in ("app", "device"):
        return jsonify({"error": "device_id and side=app|device required"}), 400

    with _call_lock:
        box = _mailbox(device_id)
        if "candidate" in body and body["candidate"] is not None:
            box[f"{side}_ice"].append(body["candidate"])
            return jsonify({"message": "candidate stored"}), 200
        # Pull from the other side
        other = "device_ice" if side == "app" else "app_ice"
        pending = box[other][:]
        box[other] = []
        return jsonify({"candidates": pending}), 200


@app.route("/Call_End", methods=["POST"])
def call_end():
    """Clear the single-call lock. Either the user who claimed it (via
    Username header) or the device side (no Username) may end. Other
    subscribers cannot force-end."""
    body = request.get_json(silent=True) or {}
    device_id = body.get("device_id") or request.headers.get("Device-ID")
    username  = request.headers.get("Username")
    if not device_id:
        return jsonify({"error": "device_id required"}), 400
    with _call_lock:
        box = _call_mailbox.get(device_id)
        if not box:
            return jsonify({"message": "call cleared"}), 200
        # No Username = device side (wake_call_helper.py); allow.
        # Username present = must match the holder of the lock.
        if username and box["in_call_by"] and box["in_call_by"] != username:
            return jsonify({
                "error": "not_call_owner",
                "in_call_by": box["in_call_by"],
            }), 403
        # Drop the whole mailbox so the next call starts clean.
        del _call_mailbox[device_id]
    # Push the cleared state so other subscribers re-enable their call button.
    mqtt_pub.state_call_pending(device_id, False, None)
    return jsonify({"message": "call cleared"}), 200


# =========================================================================
# /Wake_Command — Pi POSTs a 5-s WAV after wake fires. Server runs STT +
# intent classification and routes:
#
#   PILL    → add a "PillAck" message to the device's stream; subscribers'
#             apps see it and silence the active pill reminder locally.
#   CALL    → add a "CallRequest" message + push an FCM call-request notif
#             to every subscriber's phone.
#   MESSAGE → save the WAV under uploads/ as a voice message; same fan-out
#             as /Voice_Message_Device, app inbox tab picks it up.
#
# Headers: Device-ID, Authentication-Code
# Body:    audio/wav  (16-bit PCM mono 16 kHz)
# Reply:   {"intent": "...", "transcript": "...", "message_id": N}
# =========================================================================
def _fanout_message(cursor, device_id, msg_id,
                    fcm_title, fcm_body, fcm_color):
    """Add the message_id to every subscriber's unread queue + push FCM."""
    cursor.execute("SELECT username FROM subscriptions WHERE device_id = ?",
                   (device_id,))
    subscribers = [row["username"] for row in cursor.fetchall()]
    for username in subscribers:
        cursor.execute(
            "INSERT INTO unread_messages (user_username, message_id) VALUES (?, ?)",
            (username, msg_id),
        )
        cursor.execute(
            "SELECT firebase_token FROM users WHERE username = ?", (username,)
        )
        urow = cursor.fetchone()
        if urow and urow["firebase_token"]:
            try:
                send_notification(urow["firebase_token"],
                                  fcm_title, fcm_body, fcm_color)
            except Exception as e:
                print(f"  FCM send to {username} failed: {e}")


@app.route("/Wake_Command", methods=["POST"])
def wake_command_route():
    if wake_command is None:
        return jsonify({"error": "wake_command module not loaded"}), 503

    # ── device auth ──
    dev, err, status = verify_device_auth(request)
    if err: return jsonify(err), status
    device_id = dev["device_id"]

    conn = get_db_connection()
    cursor = conn.cursor()

    # ── pull raw audio from request body (Pi sets Content-Type: audio/wav) ──
    wav_bytes = request.get_data() or b""
    if len(wav_bytes) < 64:
        conn.close()
        return jsonify({"error": "Audio body too small / missing"}), 400

    ts        = datetime.now().strftime("%Y%m%d%H%M%S")
    occurred  = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    wav_path  = os.path.join(UPLOAD_FOLDER, f"{device_id}_{ts}_wake.wav")
    with open(wav_path, "wb") as f:
        f.write(wav_bytes)

    # ── STT + intent ──
    try:
        intent, transcript = wake_command.classify_wav(wav_path)
    except Exception as e:
        conn.close()
        # keep the clip for postmortem
        return jsonify({"error": f"classify failed: {e}",
                        "wav": wav_path}), 500

    text_payload = json.dumps({
        "Message Time":  occurred,
        "Message Type":  intent,                     # PILL | CALL | MESSAGE
        "transcript":    transcript,
    }, ensure_ascii=False)

    if intent == "PILL":
        # Pill acknowledgement — no audio needed downstream (app handles
        # locally by silencing the active reminder for this device).
        cursor.execute(
            """INSERT INTO messages
                   (device_id, message_type, message_text, message_image,
                    message_video, message_occur_time)
               VALUES (?, ?, ?, ?, ?, ?)""",
            (device_id, "PillAck", text_payload, "", "", occurred),
        )
        msg_id = cursor.lastrowid
        _fanout_message(cursor, device_id, msg_id,
                        "یادآور قرص", "بیمار قرص خود را خورد", "#33FF00")
        # Audio file isn't needed — drop it to save disk.
        try: os.remove(wav_path)
        except OSError: pass

    elif intent == "CALL":
        # Call request — caregivers get an urgent FCM. Audio is preserved so
        # the family can play back what was said when they open the request,
        # which gives context for whether to call back urgently.
        cursor.execute(
            """INSERT INTO messages
                   (device_id, message_type, message_text, message_image,
                    message_video, message_occur_time)
               VALUES (?, ?, ?, ?, ?, ?)""",
            (device_id, "CallRequest", text_payload, "", wav_path, occurred),
        )
        msg_id = cursor.lastrowid
        _fanout_message(cursor, device_id, msg_id,
                        "درخواست تماس", "بیمار درخواست تماس کرده است",
                        "#FF8800")

    else:  # MESSAGE — store the WAV in the inbox
        cursor.execute(
            """INSERT INTO messages
                   (device_id, message_type, message_text, message_image,
                    message_video, message_occur_time)
               VALUES (?, ?, ?, ?, ?, ?)""",
            (device_id, "Voice", text_payload, "", wav_path, occurred),
        )
        msg_id = cursor.lastrowid
        _fanout_message(cursor, device_id, msg_id,
                        "پیام صوتی جدید", "بیمار پیام صوتی فرستاد", "#33AAFF")

    conn.commit()
    conn.close()
    # Push the classified intent so every subscriber's app inbox refreshes
    # without polling /Message_Check_App. PILL is also a state change (pill
    # was acked), so re-push the device's active-alarm list too.
    mqtt_pub.event_wake_command(device_id, intent, transcript, msg_id)
    if intent == "PILL":
        mqtt_pub.state_pill(device_id, _active_pill_alarms_for(device_id))

    return jsonify({
        "intent":     intent,
        "transcript": transcript,
        "message_id": msg_id,
    }), 200


# ───────────────────────────────────────────────────────────────────────────
# Phase 2 subscription endpoints
#
# `devices` rows are created by /Register_Device (the BLE-onboarding flow's
# final step). Three users may subscribe to one device — enforced by the
# `subscriptions_max3` trigger.
#
# Auth model:
#   • app-side requests carry the user's server-account creds in headers
#     (`Username` + `Password`) and the device-level shared secret in the
#     body (`key_hash` = sha256(32-char device password)).
#   • Server only sees the hash, never the raw device password.
#   • Body-level `key_hash` proves the user actually knows the device pw;
#     it's the device-level auth wrapped in the existing user-account auth.
#
# Audit: every subscribe / unsubscribe / kick writes a row to
# `subscription_history` (action ∈ {joined, left, kicked}).
# ───────────────────────────────────────────────────────────────────────────

def _check_user_creds(req):
    """User-side auth via Username + Password headers. Returns (username, err, status)."""
    username = req.headers.get("Username")
    password = req.headers.get("Password")
    if not username or not password:
        return None, {"error": "Username/Password missing"}, 400
    conn = get_db_connection()
    row = conn.execute(
        "SELECT password FROM users WHERE username = ?", (username,)
    ).fetchone()
    conn.close()
    if row is None or row["password"] != password:
        return None, {"error": "Invalid Username/Password"}, 403
    return username, None, None


@app.route("/Register_Device", methods=["POST"])
def register_device():
    """Create a brand-new device row + first subscription. Called once per
    device, at the end of the BLE onboarding flow when the user has set
    the device-id + 32-char password.

    Body: {device_id, key_hash, username (optional — defaults to header)}
    """
    username, err, status = _check_user_creds(request)
    if err: return jsonify(err), status

    body      = request.get_json(silent=True) or {}
    device_id = body.get("device_id")
    key_hash  = body.get("key_hash")
    if not device_id or not key_hash:
        return jsonify({"error": "device_id and key_hash required"}), 400
    if len(key_hash) != 64:
        return jsonify({"error": "key_hash must be 64-hex SHA-256"}), 400

    conn = get_db_connection()
    try:
        with conn:
            conn.execute(
                "INSERT INTO devices (device_id, key_hash) VALUES (?, ?)",
                (device_id, key_hash),
            )
            conn.execute(
                "INSERT INTO subscriptions (device_id, username) VALUES (?, ?)",
                (device_id, username),
            )
            conn.execute(
                "INSERT INTO subscription_history "
                "(device_id, username, action, actor) VALUES (?, ?, 'joined', NULL)",
                (device_id, username),
            )
    except sqlite3.IntegrityError as e:
        msg = str(e)
        if "UNIQUE" in msg or "device_id" in msg:
            return jsonify({"error": "device_id_taken"}), 409
        return jsonify({"error": str(e)}), 500
    finally:
        conn.close()
    return jsonify({"registered": device_id, "owner": username}), 201


@app.route("/Subscribe_Device", methods=["POST"])
def subscribe_device():
    """Add the calling user as a subscriber of an existing device. Body has
    the device_id + key_hash (proof of knowing the device password). If the
    device already has 3 subscribers, returns 409 with the list so the app
    can prompt the user to kick someone via /Replace_Subscriber.

    Body: {device_id, key_hash}
    """
    username, err, status = _check_user_creds(request)
    if err: return jsonify(err), status

    body      = request.get_json(silent=True) or {}
    device_id = body.get("device_id")
    key_hash  = body.get("key_hash")
    if not device_id or not key_hash:
        return jsonify({"error": "device_id and key_hash required"}), 400

    conn = get_db_connection()
    try:
        dev = conn.execute(
            "SELECT key_hash FROM devices WHERE device_id = ?", (device_id,)
        ).fetchone()
        if dev is None:
            return jsonify({"error": "device_not_found"}), 404
        if dev["key_hash"] != key_hash:
            return jsonify({"error": "wrong_password"}), 403

        # Already subscribed?
        already = conn.execute(
            "SELECT 1 FROM subscriptions WHERE device_id = ? AND username = ?",
            (device_id, username),
        ).fetchone()
        if already:
            return jsonify({"message": "already_subscribed"}), 200

        # Try to insert — the BEFORE INSERT trigger raises 'device_full'.
        try:
            with conn:
                conn.execute(
                    "INSERT INTO subscriptions (device_id, username) VALUES (?, ?)",
                    (device_id, username),
                )
                conn.execute(
                    "INSERT INTO subscription_history "
                    "(device_id, username, action, actor) VALUES (?, ?, 'joined', NULL)",
                    (device_id, username),
                )
            return jsonify({"subscribed": device_id, "username": username}), 201
        except sqlite3.IntegrityError as e:
            if "device_full" not in str(e):
                return jsonify({"error": str(e)}), 500
            # Surface the existing 3 subscribers so the app can let the user
            # pick one to kick via /Replace_Subscriber.
            subs = conn.execute(
                "SELECT username, joined_at FROM subscriptions "
                "WHERE device_id = ? ORDER BY joined_at",
                (device_id,),
            ).fetchall()
            return jsonify({
                "error":       "device_full",
                "subscribers": [dict(s) for s in subs],
            }), 409
    finally:
        conn.close()


@app.route("/Replace_Subscriber", methods=["POST"])
def replace_subscriber():
    """Atomic kick-and-add: removes `replace_target` from the device's
    subscribers and adds the caller in their place. Caller must prove
    knowledge of the device password (key_hash); their server-account creds
    establish identity.

    Body: {device_id, key_hash, replace_target}
    """
    username, err, status = _check_user_creds(request)
    if err: return jsonify(err), status

    body           = request.get_json(silent=True) or {}
    device_id      = body.get("device_id")
    key_hash       = body.get("key_hash")
    replace_target = body.get("replace_target")
    if not device_id or not key_hash or not replace_target:
        return jsonify({"error": "device_id, key_hash, replace_target required"}), 400
    if replace_target == username:
        return jsonify({"error": "cannot_replace_self"}), 400

    conn = get_db_connection()
    try:
        dev = conn.execute(
            "SELECT key_hash FROM devices WHERE device_id = ?", (device_id,)
        ).fetchone()
        if dev is None:
            return jsonify({"error": "device_not_found"}), 404
        if dev["key_hash"] != key_hash:
            return jsonify({"error": "wrong_password"}), 403

        target_sub = conn.execute(
            "SELECT 1 FROM subscriptions WHERE device_id = ? AND username = ?",
            (device_id, replace_target),
        ).fetchone()
        if target_sub is None:
            return jsonify({"error": "target_not_subscribed"}), 404

        # The new user must not already be subscribed (otherwise we'd reduce
        # the count to 2 instead of swapping). Caller may be either a
        # current subscriber (rare — they wouldn't need to kick) or new.
        already = conn.execute(
            "SELECT 1 FROM subscriptions WHERE device_id = ? AND username = ?",
            (device_id, username),
        ).fetchone()
        if already:
            return jsonify({"error": "already_subscribed"}), 409

        with conn:
            conn.execute(
                "DELETE FROM subscriptions WHERE device_id = ? AND username = ?",
                (device_id, replace_target),
            )
            conn.execute(
                "INSERT INTO subscriptions (device_id, username) VALUES (?, ?)",
                (device_id, username),
            )
            conn.execute(
                "INSERT INTO subscription_history "
                "(device_id, username, action, actor) VALUES (?, ?, 'kicked', ?)",
                (device_id, replace_target, username),
            )
            conn.execute(
                "INSERT INTO subscription_history "
                "(device_id, username, action, actor) VALUES (?, ?, 'joined', ?)",
                (device_id, username, username),
            )
    finally:
        conn.close()

    # TODO: send FCM push to `replace_target` ("you were removed from
    # device X by username Y"). Wire when push side is plumbed.
    return jsonify({
        "replaced": replace_target,
        "added":    username,
    }), 200


@app.route("/Unsubscribe_Device", methods=["POST"])
def unsubscribe_device():
    """Caller leaves a device they're subscribed to.

    Body: {device_id, key_hash}
    """
    username, err, status = _check_user_creds(request)
    if err: return jsonify(err), status

    body      = request.get_json(silent=True) or {}
    device_id = body.get("device_id")
    key_hash  = body.get("key_hash")
    if not device_id or not key_hash:
        return jsonify({"error": "device_id and key_hash required"}), 400

    conn = get_db_connection()
    try:
        dev = conn.execute(
            "SELECT key_hash FROM devices WHERE device_id = ?", (device_id,)
        ).fetchone()
        if dev is None:
            return jsonify({"error": "device_not_found"}), 404
        # Require password proof even to leave — prevents an attacker who
        # learned a victim's app login from kicking them off devices.
        if dev["key_hash"] != key_hash:
            return jsonify({"error": "wrong_password"}), 403
        with conn:
            cur = conn.execute(
                "DELETE FROM subscriptions WHERE device_id = ? AND username = ?",
                (device_id, username),
            )
            if cur.rowcount == 0:
                return jsonify({"error": "not_subscribed"}), 404
            conn.execute(
                "INSERT INTO subscription_history "
                "(device_id, username, action, actor) VALUES (?, ?, 'left', NULL)",
                (device_id, username),
            )
    finally:
        conn.close()
    return jsonify({"unsubscribed": device_id, "username": username}), 200


@app.route("/Device_Subscribers", methods=["GET"])
def device_subscribers():
    """List current subscribers of a device the caller belongs to. Powers
    the subscribers-management screen in the app.

    Query: ?device_id=...
    """
    username, err, status = _check_user_creds(request)
    if err: return jsonify(err), status

    device_id = request.args.get("device_id")
    if not device_id:
        return jsonify({"error": "device_id query param required"}), 400

    conn = get_db_connection()
    try:
        is_member = conn.execute(
            "SELECT 1 FROM subscriptions WHERE device_id = ? AND username = ?",
            (device_id, username),
        ).fetchone()
        if not is_member:
            return jsonify({"error": "not_subscribed"}), 403
        subs = conn.execute(
            "SELECT username, joined_at FROM subscriptions "
            "WHERE device_id = ? ORDER BY joined_at",
            (device_id,),
        ).fetchall()
    finally:
        conn.close()
    return jsonify({
        "device_id":   device_id,
        "subscribers": [dict(s) for s in subs],
    }), 200


if __name__ == '__main__':
    # Pre-warm both heavy models so the first /Wake_Command request doesn't
    # pay the cold-start penalty (whisper + Qwen each take ~5 s to load).
    if wake_command is not None:
        try:
            wake_command.warm_up()
        except Exception as e:
            print(f"⚠️  wake_command warm-up failed: {e!r}")
    # debug=False — debug=True enables the Werkzeug debugger which leaks file
    # paths + a SECRET in stack traces. Keep off for anything beyond loopback.
    # Flask reloader and detailed errors aren't worth that surface area on a
    # LAN-exposed server.
    app.run(host='0.0.0.0', port=5000, debug=False)
