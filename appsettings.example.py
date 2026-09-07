"""Copy to appsettings.py and configure locally. Never commit credentials."""
API_KEY = ""
SECRET_KEY = ""
COOKIE_SECURE = False
GOOGLE_CLIENT_SECRETS_FILE = "client_secret.json"
ALLOWED_EMAILS = ["you@example.com"]
# Exact authorized redirect URI from the Google Cloud web OAuth client.
OAUTH_REDIRECT_URI = "http://localhost:8000/oauth2callback"
