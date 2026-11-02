#pragma once

// ==========================================
// TEMPLATE CREDENTIALS
// Copy this file to "secrets.h" and insert your Firebase credentials.
// secrets.h is listed in .gitignore — NEVER commit it to version control.
// ==========================================
#define FIREBASE_PROJECT_ID   "your-firebase-project-id"
#define FIREBASE_API_KEY      "your-firebase-api-key"
#define FIREBASE_RTDB_HOST    "your-project-default-rtdb.region.firebasedatabase.app"

// Captive Portal SoftAP Security (WPA2 password).
// Use a strong random password — do NOT use "12345678".
#define AP_PASSWORD           "YourStrongApPassword!"

// Timezone GMT offset in seconds (e.g., 19800 for India UTC+5:30)
#define TIMEZONE_OFFSET_SEC   19800

// Optional: Firebase Email/Password credentials for the ESP32 service account.
// When set, the scanner authenticates with Firebase before reading/writing Firestore.
// This is REQUIRED to enable authenticated scanner writes (PONG, COMPLETED status).
// Create a dedicated Firebase Auth user for the scanner (not the owner account).
#define FIREBASE_AUTH_EMAIL    ""
#define FIREBASE_AUTH_PASSWORD ""
