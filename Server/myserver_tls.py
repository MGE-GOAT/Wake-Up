from flask import Flask

app = Flask(__name__)

@app.route("/DeviceLive")
def hello_world():
    # return jsonify({"status": "success", "message": "Event saved successfully."}), 200
    return "<p>OK you are live!</p>"

if __name__ == '__main__':
    # Replace 'cert.pem' and 'key.pem' with the paths to your SSL certificate and key files
    app.run(host='0.0.0.0', port=443, debug=True, ssl_context=('cert.pem', 'key.pem'))
