"""Copy to appsettings.py and configure locally. Never commit credentials."""
API_KEY = ""
SECRET_KEY = ""
COOKIE_SECURE = False
GOOGLE_CLIENT_SECRETS_FILE = "client_secret.json"
ALLOWED_EMAILS = ["you@example.com"]

# Gemini conversation gateway (port 8041); cloud key stays on the Pi.
GEMINI_API_KEY = ""
GEMINI_MODEL = "gemini-3.1-flash-live-preview"
GEMINI_MAX_SESSIONS = 2
RECOGNIZE_URL = "http://127.0.0.1:8000/recognize"
# Optional: GEMINI_SYSTEM_INSTRUCTION = "Your assistant instructions..."
