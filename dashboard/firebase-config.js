// Firebase web config for the sub-meter dashboard.
//
// Paste the values from Firebase Console > Project settings > General >
// Your apps > Web app > SDK setup and configuration ("Config").
// These values are not secret: the database security rules protect the data.

export const firebaseConfig = {
  apiKey: "AIzaSyBhujmeXC6gYgHddEksb90b9za9YdPY1U0",
  authDomain: "submeter-44f13.firebaseapp.com",
  databaseURL: "https://submeter-44f13-default-rtdb.europe-west1.firebasedatabase.app",
  projectId: "submeter-44f13",
  storageBucket: "submeter-44f13.firebasestorage.app",
  messagingSenderId: "529259129320",
  appId: "1:529259129320:web:2e42a52022afcfae43dc5d"
};

// Timing used by the staleness checks. The spec says: set STALE_MS to
// 3 x the firmware's sync interval once that interval is fixed.
export const SYNC_INTERVAL_MS = 10000;   // ESP32 step 6 sync period
export const STALE_MS = 30000;           // device / channel counts as offline after this

// History keys (history/YYYY-MM-DD/HHMM) are written in building local time.
// Port Harcourt is West Africa Time, UTC+1 with no daylight saving.
export const HISTORY_UTC_OFFSET_MIN = 60;
