#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Wi-Fi credentials
const char* ssid = "axia_R&D";
const char* password = "AXIA2023..";

// Firebase configuration
const String FIREBASE_HOST = "https://rfid-35b0a-default-rtdb.firebaseio.com/";
const String FIREBASE_AUTH = "your-firebase-auth-token";

// RFID pins
#define SS_PIN 21
#define RST_PIN 22
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Relay pin
#define RELAY_PIN 2

// Terminal info
const String terminalId = "BORNE_A1";
const String defaultGameId = "GAME_ZOMBIE_001";

struct Card {
  String id;
  String uid;
  String customerName;
  float balance;
  String status;
};

struct Game {
  String id;
  String name;
  int duration;
  float price;
  String status;
};

struct GameSession {
  String sessionId;
  String cardId;
  String cardUid;
  String customerName;
  float originalBalance;
  float remainingBalance;
  String gameId;
  String gameName;
  int gameDuration;
  float gamePrice;
  unsigned long startTime;
  bool isActive;
};

Card localCards[10];
Game localGames[5];
int cardCount = 0;
int gameCount = 0;
GameSession currentSession;

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  connectToWiFi();
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("RFID Ready");

  // Fetch data from Firebase
  fetchCardsFromFirebase();
  fetchGamesFromFirebase();

  sendTerminalInfoToFirebase();
  Serial.println("Borne prête !");
}

void loop() {
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String cardUid = getCardUid();
    Serial.println("Carte détectée : " + cardUid);

    // Find card and get current balance from Firebase
    Card* foundCard = findCard(cardUid);
    if (foundCard != nullptr) {
      // Get fresh balance from Firebase
      float currentBalance = getCurrentBalanceFromFirebase(foundCard->id);
      if (currentBalance >= 0) { // Valid balance retrieved
        foundCard->balance = currentBalance; // Update local copy
        
        Game* foundGame = findGame(defaultGameId);
        if (foundGame != nullptr) {
          if (foundCard->balance >= foundGame->price) {
            if (currentSession.isActive) {
              endSession();
              delay(1000);
            }
            startSession(foundCard, foundGame);
          } else {
            Serial.println("⚠️ Solde insuffisant !");
            Serial.println("   Solde actuel: " + String(foundCard->balance));
            Serial.println("   Prix du jeu: " + String(foundGame->price));
          }
        } else {
          Serial.println("❌ Jeu non trouvé: " + defaultGameId);
        }
      } else {
        Serial.println("❌ Erreur lors de la récupération du solde");
      }
    } else {
      Serial.println("❌ Carte inconnue");
    }

    mfrc522.PICC_HaltA();
  }

  if (currentSession.isActive) checkTimeout();
  delay(100);
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connexion Wi-Fi");

  unsigned long startAttemptTime = millis();
  const unsigned long timeout = 15000;

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < timeout) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Connecté ! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n❌ Échec de connexion Wi-Fi. Vérifie le SSID/mot de passe ou la bande 2.4 GHz.");
    Serial.print("Code état WiFi : ");
    Serial.println(WiFi.status());
  }
}

String getCardUid() {
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void fetchCardsFromFirebase() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ Pas de connexion Wi-Fi pour récupérer les cartes");
    return;
  }

  HTTPClient http;
  String url = FIREBASE_HOST + "cards.json";
  if (!FIREBASE_AUTH.isEmpty() && FIREBASE_AUTH != "your-firebase-auth-token") {
    url += "?auth=" + FIREBASE_AUTH;
  }

  http.begin(url);
  http.setTimeout(10000); // 10 second timeout
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096); // Increased size
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
      Serial.println("❌ Erreur parsing JSON cartes: " + String(error.c_str()));
      http.end();
      return;
    }

    cardCount = 0;
    
    // Handle both object and array formats
    if (doc.is<JsonObject>()) {
      JsonObject cards = doc.as<JsonObject>();
      for (JsonPair card : cards) {
        if (cardCount < 10) {
          JsonObject cardData = card.value().as<JsonObject>();
          localCards[cardCount].id = cardData["id"] | card.key().c_str();
          localCards[cardCount].uid = cardData["uid"] | "";
          localCards[cardCount].customerName = cardData["customerName"] | "Unknown";
          localCards[cardCount].balance = cardData["balance"] | 0.0;
          localCards[cardCount].status = cardData["status"] | "active";
          cardCount++;
        }
      }
    } else if (doc.is<JsonArray>()) {
      JsonArray cards = doc.as<JsonArray>();
      for (JsonObject cardData : cards) {
        if (cardCount < 10) {
          localCards[cardCount].id = cardData["id"] | String("CARD_" + String(cardCount));
          localCards[cardCount].uid = cardData["uid"] | "";
          localCards[cardCount].customerName = cardData["customerName"] | "Unknown";
          localCards[cardCount].balance = cardData["balance"] | 0.0;
          localCards[cardCount].status = cardData["status"] | "active";
          cardCount++;
        }
      }
    }
    
    Serial.println("✅ " + String(cardCount) + " cartes récupérées depuis Firebase");
  } else {
    Serial.println("❌ Erreur lors de la récupération des cartes: " + String(httpCode));
    if (httpCode > 0) {
      Serial.println("Response: " + http.getString());
    }
  }
  
  http.end();
}

void fetchGamesFromFirebase() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ Pas de connexion Wi-Fi pour récupérer les jeux");
    return;
  }

  HTTPClient http;
  String url = FIREBASE_HOST + "games.json";
  if (!FIREBASE_AUTH.isEmpty() && FIREBASE_AUTH != "your-firebase-auth-token") {
    url += "?auth=" + FIREBASE_AUTH;
  }

  http.begin(url);
  http.setTimeout(10000);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
      Serial.println("❌ Erreur parsing JSON jeux: " + String(error.c_str()));
      http.end();
      return;
    }

    gameCount = 0;
    
    // Handle both object and array formats
    if (doc.is<JsonObject>()) {
      JsonObject games = doc.as<JsonObject>();
      for (JsonPair game : games) {
        if (gameCount < 5) {
          JsonObject gameData = game.value().as<JsonObject>();
          localGames[gameCount].id = gameData["id"] | game.key().c_str();
          localGames[gameCount].name = gameData["name"] | "Unknown Game";
          localGames[gameCount].duration = gameData["duration"] | 60;
          localGames[gameCount].price = gameData["price"] | 1.0;
          localGames[gameCount].status = gameData["status"] | "active";
          gameCount++;
        }
      }
    } else if (doc.is<JsonArray>()) {
      JsonArray games = doc.as<JsonArray>();
      for (JsonObject gameData : games) {
        if (gameCount < 5) {
          localGames[gameCount].id = gameData["id"] | String("GAME_" + String(gameCount));
          localGames[gameCount].name = gameData["name"] | "Unknown Game";
          localGames[gameCount].duration = gameData["duration"] | 60;
          localGames[gameCount].price = gameData["price"] | 1.0;
          localGames[gameCount].status = gameData["status"] | "active";
          gameCount++;
        }
      }
    }
    
    Serial.println("✅ " + String(gameCount) + " jeux récupérés depuis Firebase");
  } else {
    Serial.println("❌ Erreur lors de la récupération des jeux: " + String(httpCode));
    if (httpCode > 0) {
      Serial.println("Response: " + http.getString());
    }
  }
  
  http.end();
}

// Get current balance from Firebase for accurate calculation
float getCurrentBalanceFromFirebase(String cardId) {
  if (WiFi.status() != WL_CONNECTED) return -1;

  HTTPClient http;
  String url = FIREBASE_HOST + "cards/" + cardId + "/balance.json";
  if (!FIREBASE_AUTH.isEmpty() && FIREBASE_AUTH != "your-firebase-auth-token") {
    url += "?auth=" + FIREBASE_AUTH;
  }

  http.begin(url);
  http.setTimeout(5000);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    float balance = payload.toFloat();
    Serial.println("Balance récupérée: " + String(balance));
    http.end();
    return balance;
  } else {
    Serial.println("❌ Erreur récupération balance: " + String(httpCode));
    http.end();
    return -1;
  }
}

Card* findCard(String uid) {
  for (int i = 0; i < cardCount; i++) {
    if (localCards[i].uid == uid && localCards[i].status == "active") {
      return &localCards[i];
    }
  }
  return nullptr;
}

Game* findGame(String id) {
  for (int i = 0; i < gameCount; i++) {
    if (localGames[i].id == id && localGames[i].status == "active") {
      return &localGames[i];
    }
  }
  return nullptr;
}

void startSession(Card* card, Game* game) {
  float newBalance = card->balance - game->price;

  currentSession.sessionId = "SESS_" + terminalId + "_" + String(millis());
  currentSession.isActive = true;
  currentSession.startTime = millis();
  currentSession.cardId = card->id;
  currentSession.cardUid = card->uid;
  currentSession.customerName = card->customerName;
  currentSession.originalBalance = card->balance;
  currentSession.remainingBalance = newBalance;
  currentSession.gameId = game->id;
  currentSession.gameName = game->name;
  currentSession.gameDuration = game->duration;
  currentSession.gamePrice = game->price;

  // Mise à jour du solde local
  card->balance = newBalance;

  // 🔁 Activation du relais uniquement ici
  digitalWrite(RELAY_PIN, HIGH);

  Serial.println("🎮 Session démarrée : " + currentSession.gameName);
  
  updateCardInFirebase();
  sendSessionToFirebase();
}
void endSession() {
  if (!currentSession.isActive) return;

  // 🔁 Désactivation du relais
  digitalWrite(RELAY_PIN, LOW);
  currentSession.isActive = false;

  Serial.println("🛑 Session terminée pour : " + currentSession.customerName);
  
  updateSessionEndInFirebase();
}


void checkTimeout() {
  if ((millis() - currentSession.startTime) / 1000 >= currentSession.gameDuration) {
    Serial.println("⏰ Timeout atteint");
    endSession();
  }
}

void sendTerminalInfoToFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = FIREBASE_HOST + "terminals/" + terminalId + ".json";
  if (!FIREBASE_AUTH.isEmpty() && FIREBASE_AUTH != "your-firebase-auth-token") {
    url += "?auth=" + FIREBASE_AUTH;
  }

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(512);
  doc["terminalId"] = terminalId;
  doc["status"] = "online";
  doc["defaultGameId"] = defaultGameId;
  doc["lastUpdate"] = millis();
  doc["ipAddress"] = WiFi.localIP().toString();

  String jsonString;
  serializeJson(doc, jsonString);
  int httpCode = http.PUT(jsonString);
  Serial.println("Terminal update: " + String(httpCode));
  http.end();
}

void sendSessionToFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = FIREBASE_HOST + "sessions/" + currentSession.sessionId + ".json";
  if (!FIREBASE_AUTH.isEmpty() && FIREBASE_AUTH != "your-firebase-auth-token") {
    url += "?auth=" + FIREBASE_AUTH;
  }

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(1024);
  doc["sessionId"] = currentSession.sessionId;
  doc["cardId"] = currentSession.cardId;
  doc["cardUid"] = currentSession.cardUid;
  doc["customerName"] = currentSession.customerName;
  doc["gameName"] = currentSession.gameName;
  doc["gamePrice"] = currentSession.gamePrice;
  doc["gameDuration"] = currentSession.gameDuration;
  doc["originalBalance"] = currentSession.originalBalance;
  doc["remainingBalance"] = currentSession.remainingBalance;
  doc["startTime"] = currentSession.startTime;
  doc["terminal"] = terminalId;
  doc["status"] = currentSession.isActive ? "active" : "completed";

  String jsonString;
  serializeJson(doc, jsonString);
  int httpCode = http.PUT(jsonString);
  Serial.println("Session create: " + String(httpCode));
  http.end();
}

void updateSessionEndInFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = FIREBASE_HOST + "sessions/" + currentSession.sessionId + ".json";
  if (!FIREBASE_AUTH.isEmpty() && FIREBASE_AUTH != "your-firebase-auth-token") {
    url += "?auth=" + FIREBASE_AUTH;
  }

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(512);
  doc["endTime"] = millis();
  doc["status"] = "completed";
  doc["finalBalance"] = currentSession.remainingBalance;

  String jsonString;
  serializeJson(doc, jsonString);
  int httpCode = http.PATCH(jsonString);
  Serial.println("Session end update: " + String(httpCode));
  http.end();
}

void updateCardInFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = FIREBASE_HOST + "cards/" + currentSession.cardId + ".json";
  if (!FIREBASE_AUTH.isEmpty() && FIREBASE_AUTH != "your-firebase-auth-token") {
    url += "?auth=" + FIREBASE_AUTH;
  }

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(512);
  doc["balance"] = currentSession.remainingBalance;
  doc["lastUsed"] = millis();
  doc["lastTerminal"] = terminalId;

  String jsonString;
  serializeJson(doc, jsonString);
  int httpCode = http.PATCH(jsonString);
  Serial.println("Card balance update: " + String(httpCode));
  http.end();
}