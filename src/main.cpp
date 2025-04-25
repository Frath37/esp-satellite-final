// === esp32_mqtt_ota.cpp ===
// Version avec OTA Web + MQTT + mDNS + fallback IP + JSON sondes + bouton reset. configure ini pour ota ou serie 

#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>

#define BUS1 21
#define BUS2 4
#define BUS3 17
#define BUS4 18
#define LED_PIN 8

const char* ssid1 = "Geoclimair wifi";
const char* pass1 = "0607292493";
const char* ssid2 = "Livebox-d3b8";
const char* pass2 = "CwfAlb0607@!";
IPAddress fallbackMQTT(192, 168, 2, 22);
IPAddress mqttIP;

WiFiClient espClient;
PubSubClient client(espClient);
WebServer server(80);

OneWire oneWire1(BUS1), oneWire2(BUS2), oneWire3(BUS3), oneWire4(BUS4);
DallasTemperature sensors1(&oneWire1), sensors2(&oneWire2), sensors3(&oneWire3), sensors4(&oneWire4);
DallasTemperature* allSensors[4] = { &sensors1, &sensors2, &sensors3, &sensors4 };

struct Sonde {
  String name;
  DeviceAddress address;
};

#define MAX_SONDES 10
Sonde sondesConnues[MAX_SONDES];
int nbSondes = 0;
unsigned long startMillis;

void blink(int n) {
  for (int i = 0; i < n; i++) {
    digitalWrite(LED_PIN, HIGH); delay(200);
    digitalWrite(LED_PIN, LOW); delay(200);
  }
  delay(500);
}

void reverseAddress(DeviceAddress in, DeviceAddress out) {
  out[0] = in[0];
  for (int i = 1; i <= 6; i++) out[i] = in[7 - i];
  out[7] = 0x00;
}

bool compareAddress(DeviceAddress a, DeviceAddress b) {
  for (int i = 0; i < 7; i++) if (a[i] != b[i]) return false;
  return true;
}

bool parseAddress(const char* addrStr, DeviceAddress &deviceAddr) {
  for (int i = 0; i < 8; i++) {
    char byteStr[3] = { addrStr[i * 2], addrStr[i * 2 + 1], 0 };
    deviceAddr[i] = strtoul(byteStr, NULL, 16);
  }
  return true;
}

void connect_wifi() {
  Serial.printf("📶 Connexion à %s...\n", ssid1);
  WiFi.begin(ssid1, pass1);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 5000) { delay(500); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) { Serial.printf("\n✅ Connecté à %s\n", ssid1); return; }

  Serial.printf("\n❌ Échec %s → essai %s...\n", ssid1, ssid2);
  WiFi.begin(ssid2, pass2); t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 5000) { delay(500); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) { Serial.printf("\n✅ Connecté à %s\n", ssid2); return; }

  Serial.println("\n❌ Aucun Wi-Fi trouvé → redémarrage");
  ESP.restart();
}

void chargerSondesConnues() {
  HTTPClient http;
  String url = "http://" + mqttIP.toString() + "/sondes.json";
  Serial.print("🔄 Chargement de : "); Serial.println(url);

  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) { Serial.println("❌ Erreur JSON"); nbSondes = 0; return; }
    nbSondes = 0;
    for (JsonObject obj : doc.as<JsonArray>()) {
      if (nbSondes >= MAX_SONDES) break;
      sondesConnues[nbSondes].name = obj["name"].as<String>();
      parseAddress(obj["address"], sondesConnues[nbSondes].address);
      Serial.printf("✅ Sonde %d : %s\n", nbSondes, sondesConnues[nbSondes].name.c_str());
      nbSondes++;
    }
  } else {
    Serial.printf("❌ HTTP %d\n", httpCode); nbSondes = 0;
  }
  http.end();
}

void handleRoot() {
  String html = "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='10'>";
  html += "<style>body{font-family:sans-serif;}table{border-collapse:collapse;}td,th{border:1px solid #ccc;padding:8px;}</style>";
  html += "<title>Sondes</title></head><body><h2>Sondes</h2><table><tr><th>Nom</th><th>Temp</th></tr>";
  for (int i = 0; i < nbSondes; i++) {
    float temp = -127;
    for (int b = 0; b < 4; b++) {
      DallasTemperature* sensors = allSensors[b];
      DeviceAddress addr; int idx = 0;
      while (sensors->getAddress(addr, idx++)) {
        DeviceAddress rev; reverseAddress(addr, rev);
        if (compareAddress(rev, sondesConnues[i].address)) {
          temp = sensors->getTempC(addr); break;
        }
      }
    }
    html += "<tr><td>" + sondesConnues[i].name + "</td><td>" + String(temp, 2) + " °C</td></tr>";
  }
  html += "</table><br><a href='/reset'><button>Reboot ESP</button></a></body></html>";
  server.send(200, "text/html", html);
}

void handleUploadForm() {
  server.send(200, "text/html",
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware'>"
    "<input type='submit' value='Mettre à jour'>"
    "</form>");
}

void handleUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("🔄 OTA : %s\n", upload.filename.c_str());
    Update.begin();
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.println("✅ OTA terminée");
    } else {
      Serial.println("❌ OTA erreur");
    }
  }
}

void setupOtaWeb() {
  server.on("/update", HTTP_GET, handleUploadForm);
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", Update.hasError() ? "❌ Échec" : "✅ OK, reboot...");
    if (!Update.hasError()) ESP.restart();
  }, handleUpload);

  server.on("/reset", HTTP_GET, []() {
    server.send(200, "text/plain", "⏳ Redémarrage...");
    delay(100); ESP.restart();
  });
}

void reconnect() {
  while (!client.connected()) {
    if (client.connect("ESP32_TEMP")) break;
    delay(5000);
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(115200);
  blink(1);

  connect_wifi(); blink(2);

  if (!MDNS.begin("espmini")) Serial.println("⚠️ Erreur mDNS");

  bool found = false;
  for (int i = 0; i < 10; i++) {
    mqttIP = MDNS.queryHost("mqtt.local");
    if (mqttIP) { found = true; break; }
    Serial.printf("⏳ Tentative mDNS %d/10...\n", i + 1);
    delay(1000);
  }
  if (!found) {
    Serial.println("⚠️ mqtt.local introuvable → fallback");
    mqttIP = fallbackMQTT;
  }
  client.setServer(mqttIP, 1883);
  Serial.print("🔗 MQTT via : "); Serial.println(mqttIP);

  reconnect(); blink(3);

  for (int i = 0; i < 4; i++) allSensors[i]->begin();
  chargerSondesConnues(); blink(4);

  server.on("/", handleRoot);
  setupOtaWeb();
  server.begin();

  startMillis = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connect_wifi();
  if (!client.connected()) reconnect();
  client.loop();
  server.handleClient();

  if (millis() - startMillis >= 6000) {
    int totalSondes = 0;
    bool aPublie = false;
    for (int b = 0; b < 4; b++) {
      allSensors[b]->requestTemperatures();
      delay(750);
      DeviceAddress addr;
      int idx = 0;
      while (allSensors[b]->getAddress(addr, idx++)) {
        float temp = allSensors[b]->getTempC(addr);
        DeviceAddress revAddr;
        reverseAddress(addr, revAddr);
        String topic = "";
        for (int j = 0; j < nbSondes; j++) {
          if (compareAddress(revAddr, sondesConnues[j].address)) {
            String nom = sondesConnues[j].name;
            nom.replace("_", "/");
            topic = "homeauto/temp/pac/" + nom;
            break;
          }
        }
        if (topic != "") {
          Serial.printf("📡 %s → %.2f°C\n", topic.c_str(), temp);
          if (client.publish(topic.c_str(), String(temp).c_str(), true)) aPublie = true;
        }
        totalSondes++;
      }
    }
    if (aPublie) blink(1);
    Serial.printf("🧬 %d sonde(s) traitée(s)\n", totalSondes);
    startMillis = millis();
  }
}
