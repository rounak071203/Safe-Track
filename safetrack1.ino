#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <MPU6050.h>
#include <HardwareSerial.h>
#include <TinyGPS++.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// =====================
// CREDENTIALS
// =====================
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define API_KEY "YOUR_FIREBASE_API_KEY"
#define DATABASE_URL "YOUR_FIREBASE_DATABASE_URL"
#define USER_EMAIL "YOUR_FIREBASE_EMAIL"
#define USER_PASSWORD "YOUR_FIREBASE_PASSWORD"

#define BOT_TOKEN "YOUR_TELEGRAM_BOT_TOKEN"
#define CHAT_ID "YOUR_TELEGRAM_CHAT_ID"

#define TWILIO_SID "YOUR_TWILIO_ACCOUNT_SID"
#define TWILIO_TOKEN "YOUR_TWILIO_AUTH_TOKEN"

// Family WhatsApp numbers
String familyNumbers[] = {
  "whatsapp%3A%2B918767415098"
};
int familyCount = 1;

// Home location for geofencing
#define HOME_LAT 16.684283
#define HOME_LNG 74.272944
#define GEOFENCE_RADIUS 500

// Motion threshold
#define MOTION_THRESHOLD 15000

// Pins
#define BUZZER_PIN 25
#define SDA_PIN 15
#define SCL_PIN 4

// =====================
// OBJECTS
// =====================
MPU6050 mpu;
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// =====================
// FLAGS
// =====================
bool armed = true;
bool alertSent = false;
double currentLat = 0;
double currentLng = 0;

// =====================
// FUNCTIONS
// =====================

void sendTelegramMessage(String message) {
  HTTPClient http;
  message.replace(" ", "%20");
  message.replace("!", "%21");
  String url = "https://api.telegram.org/bot";
  url += BOT_TOKEN;
  url += "/sendMessage?chat_id=";
  url += CHAT_ID;
  url += "&text=";
  url += message;
  http.begin(url);
  http.GET();
  http.end();
  Serial.println("Telegram sent!");
}

void sendWhatsApp(String toNumber, String message) {
  HTTPClient http;
  String url = "https://api.twilio.com/2010-04-01/Accounts/";
  url += TWILIO_SID;
  url += "/Messages.json";
  http.begin(url);
  http.setAuthorization(TWILIO_SID, TWILIO_TOKEN);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String payload = "From=whatsapp%3A%2B14155238886";
  payload += "&To=" + toNumber;
  payload += "&Body=" + message;
  http.POST(payload);
  http.end();
  Serial.println("WhatsApp sent!");
}

void makeCall(String toNumber) {
  HTTPClient http;
  String url = "https://api.twilio.com/2010-04-01/Accounts/";
  url += TWILIO_SID;
  url += "/Calls.json";
  http.begin(url);
  http.setAuthorization(TWILIO_SID, TWILIO_TOKEN);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String payload = "From=%2B19166448776";
  payload += "&To=" + toNumber;
  payload += "&Url=http%3A%2F%2Fdemo.twilio.com%2Fdocs%2Fvoice.xml";
  http.POST(payload);
  http.end();
  Serial.println("Call made!");
}

String findNearestHospital() {
  HTTPClient http;
  String query = "[out:json];(node[amenity=hospital](around:10000,";
  query += String(currentLat, 6);
  query += ",";
  query += String(currentLng, 6);
  query += ");node[amenity=clinic](around:10000,";
  query += String(currentLat, 6);
  query += ",";
  query += String(currentLng, 6);
  query += "););out body 1;";
  http.begin("https://overpass-api.de/api/interpreter");
  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int httpCode = http.POST("data=" + query);
  String hospLocation = "";
  if (httpCode == 200) {
    String response = http.getString();
    int latIndex = response.indexOf("\"lat\":");
    int lonIndex = response.indexOf("\"lon\":");
    if (latIndex != -1 && lonIndex != -1) {
      String hospLat = response.substring(latIndex + 6, response.indexOf(",", latIndex + 6));
      String hospLon = response.substring(lonIndex + 6, response.indexOf(",", lonIndex + 6));
      hospLocation = "https://maps.google.com/?q=" + hospLat + "," + hospLon;
    }
  }
  http.end();
  return hospLocation;
}

void pushToFirebase(String alertType) {
  if (Firebase.ready()) {
    Firebase.RTDB.setDouble(&fbdo, "/status/last_lat", currentLat);
    Firebase.RTDB.setDouble(&fbdo, "/status/last_lng", currentLng);
    Firebase.RTDB.setString(&fbdo, "/status/last_updated", String(millis()));
    Firebase.RTDB.setString(&fbdo, "/alerts/latest/type", alertType);
    Firebase.RTDB.setDouble(&fbdo, "/alerts/latest/lat", currentLat);
    Firebase.RTDB.setDouble(&fbdo, "/alerts/latest/lng", currentLng);
    Serial.println("Firebase updated!");
  }
}

void triggerAlert(bool outsideGeofence) {
  // Buzzer ON
  digitalWrite(BUZZER_PIN, HIGH);

  // Get location link
  String victimLocation = "https://maps.google.com/?q=";
  victimLocation += String(currentLat, 6);
  victimLocation += ",";
  victimLocation += String(currentLng, 6);

  // Send family alerts
  for (int i = 0; i < familyCount; i++) {
    String waMessage = "Alert!%20Accident%20detected!%20Location:%20" + victimLocation;
    sendWhatsApp(familyNumbers[i], waMessage);
    String callNumber = familyNumbers[i];
    callNumber.replace("whatsapp%3A%2B", "%2B");
    makeCall(callNumber);
  }

  // Telegram alert
  sendTelegramMessage("Alert! Accident detected! Location: " + victimLocation);

  if (outsideGeofence) {
    // Find and alert nearest hospital
    String hospLocation = findNearestHospital();
    if (hospLocation != "") {
      Serial.println("Nearest Hospital: " + hospLocation);
    }
  }

  // Push to Firebase
  pushToFirebase(outsideGeofence ? "geofence_breach" : "motion");

  alertSent = true;
}

void checkTelegramCommands() {
  HTTPClient http;
  String url = "https://api.telegram.org/bot";
  url += BOT_TOKEN;
  url += "/getUpdates";
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String response = http.getString();
    if (response.indexOf("/arm") != -1) {
      armed = true;
      alertSent = false;
      sendTelegramMessage("SafeTrack ARMED!");
      Serial.println("Armed!");
    }
    if (response.indexOf("/disarm") != -1) {
      armed = false;
      digitalWrite(BUZZER_PIN, LOW);
      sendTelegramMessage("SafeTrack DISARMED!");
      Serial.println("Disarmed!");
    }
    if (response.indexOf("/location") != -1) {
      String loc = "Current location: https://maps.google.com/?q=";
      loc += String(currentLat, 6);
      loc += ",";
      loc += String(currentLng, 6);
      sendTelegramMessage(loc);
    }
    if (response.indexOf("/status") != -1) {
      sendTelegramMessage(armed ? "Status: ARMED" : "Status: DISARMED");
    }
  }
  http.end();
}

// =====================
// SETUP
// =====================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Test buzzer
  digitalWrite(BUZZER_PIN, HIGH);
  delay(200);
  digitalWrite(BUZZER_PIN, LOW);

  // Init MPU6050
  Wire.begin(15, 4);
  mpu.initialize();
  if (mpu.testConnection()) {
    Serial.println("MPU6050 Connected!");
  } else {
    Serial.println("MPU6050 Failed!");
  }

  // Init GPS
  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);
  Serial.println("GPS Started!");

  // Connect WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nWiFi Connected!");

  // Connect Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  while (!Firebase.ready()) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("Firebase Connected!");

  sendTelegramMessage("SafeTrack System Online!");
  Serial.println("System Ready!");
}

// =====================
// LOOP
// =====================
void loop() {
  // Read GPS
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }
  if (gps.location.isValid()) {
    currentLat = gps.location.lat();
    currentLng = gps.location.lng();
  }

  // Check Telegram commands every 3 seconds
  static unsigned long lastTelegramCheck = 0;
  if (millis() - lastTelegramCheck > 3000) {
    checkTelegramCommands();
    lastTelegramCheck = millis();
  }

  // Push GPS to Firebase every 10 seconds
  static unsigned long lastFirebasePush = 0;
  if (millis() - lastFirebasePush > 10000 && currentLat != 0) {
    Firebase.RTDB.setDouble(&fbdo, "/status/last_lat", currentLat);
    Firebase.RTDB.setDouble(&fbdo, "/status/last_lng", currentLng);
    lastFirebasePush = millis();
  }

  if (armed && !alertSent) {
    // Read MPU6050
    int16_t ax, ay, az, gx, gy, gz;
    mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
    long totalAccel = abs(ax) + abs(ay) + abs(az);

    if (totalAccel > MOTION_THRESHOLD) {
      Serial.println("Motion Detected!");

      // Check geofence
      bool outsideGeofence = false;
      if (currentLat != 0) {
        double distance = TinyGPSPlus::distanceBetween(
          currentLat, currentLng,
          HOME_LAT, HOME_LNG
        );
        Serial.print("Distance: ");
        Serial.println(distance);
        outsideGeofence = distance > GEOFENCE_RADIUS;
      }

      triggerAlert(outsideGeofence);
    }
  }

  delay(500);
}