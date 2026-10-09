#pragma once

// ==========================================
// TEMPLATE CREDENTIALS
// Copy this file to "secrets.h" and insert your Firebase credentials.
// ==========================================
#define FIREBASE_PROJECT_ID "your-firebase-project-id"
#define FIREBASE_API_KEY "your-firebase-api-key"

// Captive Portal SoftAP Security (WPA2 password to prevent unauthorized takeover)
#define AP_PASSWORD "FitnessBox99!"

// Timezone GMT offset in seconds (e.g., 19800 for India UTC+5:30)
#define TIMEZONE_OFFSET_SEC 19800

// Optional Device Authentication credentials (for locked-down Firestore rules)
#define FIREBASE_AUTH_EMAIL ""
#define FIREBASE_AUTH_PASSWORD ""
