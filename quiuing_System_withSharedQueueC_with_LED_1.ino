#include <SPI.h>
#include <MFRC522.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <esp_now.h>
#include <WiFi.h>
#include <vector>
#include <algorithm>
#include "SharedQueue.h"
#include <Wire.h>
#include "RTClib.h"

#define RST_PIN  5
#define SS_PIN   4
#define GREEN_LED_PIN 15
#define RED_LED_PIN   2

MFRC522 mfrc522(SS_PIN, RST_PIN);
Preferences prefs;

SharedQueue sharedQueue("rfid-patients");
SharedQueue sharedQueueA("queue-A");
SharedQueue sharedQueueB("queue-B");
SharedQueue sharedQueueC("queue-C");

RTC_DS3231 rtc;
bool isArrivalNode = false, isDoctorNode = false;

const uint8_t arrivalMACs[][6] = {
    {0x08, 0xD1, 0xF9, 0xD7, 0x50, 0x98},
    {0x5C, 0x01, 0x3B, 0x97, 0x54, 0xB4},
    {0x78, 0x42, 0x1C, 0x6C, 0xE4, 0x9C},
    {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC},
    {0x9F, 0x8E, 0x7D, 0x6C, 0x5B, 0x4A}
};
const int numArrivalNodes = sizeof(arrivalMACs) / sizeof(arrivalMACs[0]);

const uint8_t doctorMACs[][6] = {
    {0x78, 0x42, 0x1C, 0x6C, 0xA8, 0x3C},
    {0x5C, 0x01, 0x3B, 0x98, 0x3C, 0xEC},
    {0x5C, 0x01, 0x3B, 0x98, 0xA6, 0x38},
    {0x5C, 0x01, 0x3B, 0x99, 0x07, 0xDC}
};

void onDataRecv(const esp_now_recv_info_t *recvInfo, const uint8_t *incomingData, int len) {
    QueueItem item;
    memcpy(&item, incomingData, sizeof(item));
    const uint8_t* mac = recvInfo->src_addr;
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.print("📩 Received from: "); Serial.println(macStr);

    isArrivalNode = isDoctorNode = false;
    for (int i = 0; i < numArrivalNodes; i++)
        if (memcmp(mac, arrivalMACs[i], 6) == 0) { isArrivalNode = true; break; }
    for (int i = 0; i < 4; i++)
        if (memcmp(mac, doctorMACs[i], 6) == 0) { isDoctorNode = true; break; }

    if (isArrivalNode) { Serial.println("🔄 Handling Arrival Node message...");
        if (item.addToQueue && !item.removeFromQueue) {
            sharedQueue.addIfNew(String(item.uid), String(item.timestamp), item.number);
        } else if (!item.addToQueue && item.removeFromQueue) {
            sharedQueue.removeByUID(String(item.uid));
            Serial.printf("🔁 Synchronized removal of UID: %s\n", item.uid);
        }
    } else if (isDoctorNode) {
         Serial.println("👨‍⚕️ Doctor Node message received...");
        if (strcmp(item.uid, "REQ_NEXT") == 0) {
             Serial.println("📬 Handling 'REQ_NEXT' from Doctor...");
            int doctorNodeID = -1;
            for (int i = 0; i < 4; i++){
                if (memcmp(mac, doctorMACs[i], 6) == 0) {
                doctorNodeID = i + 1;
                break;
                }
            }
               sharedQueue.print();sharedQueueA.print();sharedQueueB.print();sharedQueueC.print();
            if (doctorNodeID == -1){
                 Serial.println("❌ Unknown Doctor Node MAC!");
                  return;}
            if (!sharedQueueC.empty()) {
    QueueEntry entry = sharedQueueC.front();
    QueueItem sendItem;
    strncpy(sendItem.uid, entry.uid.c_str(), sizeof(sendItem.uid));
    strncpy(sendItem.timestamp, entry.timestamp.c_str(), sizeof(sendItem.timestamp));
    sendItem.number = entry.number;
    sendItem.node = doctorNodeID;
    sendItem.addToQueue = false;
    sendItem.removeFromQueue = false;

    // Send to doctor
    esp_now_send(mac, (uint8_t*)&sendItem, sizeof(sendItem));

    // Remove from MixedQueue
    // Move to bottom of the correct original queue
    if (sharedQueue.exists(entry.uid)) {
       // sharedQueue.removeByUID(entry.uid); 
        sharedQueue.pop();      // Remove original position
        sharedQueue.push(entry); 
         sharedQueueC.pop(); sharedQueueC.push(entry);           // Push to back
    } else if (sharedQueueB.exists(entry.uid)) {
        sharedQueueB.pop(); sharedQueueC.pop();
        sharedQueueB.push(entry);sharedQueueC.push(entry);
    } 
}
 else {
                QueueItem zeroItem = {};
                strncpy(zeroItem.uid, "NO_PATIENT", sizeof(zeroItem.uid));
                zeroItem.number = 0;
                zeroItem.node = doctorNodeID;
                zeroItem.addToQueue = false;
                zeroItem.removeFromQueue = false;
                esp_now_send(mac, (uint8_t*)&zeroItem, sizeof(zeroItem));
                Serial.printf("⚠️ Queue empty. Sent 'NO_PATIENT' to Doctor Node %d.\n", doctorNodeID);
          
            }
        } else if (item.removeFromQueue) {
            item.addToQueue = false;
            item.removeFromQueue = true;
            for (int i = 0; i < numArrivalNodes; i++) {
                if (memcmp(arrivalMACs[i], WiFi.macAddress().c_str(), 6) != 0)
                    esp_now_send(arrivalMACs[i], (uint8_t*)&item, sizeof(item));
            }
            Serial.println("📤 Broadcasted removal to Arrival Nodes.");

            if (sharedQueue.exists(String(item.uid))) {
                sharedQueue.removeByUID(String(item.uid));
                sharedQueueC.removeByUID(String(item.uid));
                sharedQueueA.add(String(item.uid), String(item.timestamp), item.number);
                Serial.printf("🗑️ UID %s removed from SharedQueue and added to SharedQueueA.\n", item.uid);
            } else {
                sharedQueueC.removeByUID(String(item.uid));
                sharedQueueB.removeByUID(String(item.uid));
                Serial.printf("🗑️ UID %s removed from SharedQueueC and SharedQueueB.\n", item.uid);
            }

            if (sharedQueueC.empty()) {
                Serial.println("⚠️ Queue is now empty. Sending NO_PATIENT to all Doctor Nodes...");
                for (int i = 0; i < 4; i++) {
                    QueueItem zeroItem = {};
                    strncpy(zeroItem.uid, "NO_PATIENT", sizeof(zeroItem.uid));
                    zeroItem.number = 0;
                    zeroItem.node = i + 1;
                    zeroItem.addToQueue = false;
                    zeroItem.removeFromQueue = false;
                    esp_now_send(doctorMACs[i], (uint8_t*)&zeroItem, sizeof(zeroItem));
                }
                Serial.println("📤 Sent NO_PATIENT to all doctors.");
            }
        }
    }
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Sent 🟢" : "Failed 🔴");
}

void processCard(String uid) {
    DateTime now = rtc.now();
    char timeBuffer[25];
    snprintf(timeBuffer, sizeof(timeBuffer), "%04d-%02d-%02d %02d:%02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
    String timeStr = String(timeBuffer);

    int pid = getPermanentNumber(uid);
    if (pid == -1) pid = 0;

    QueueItem item;
    strncpy(item.uid, uid.c_str(), sizeof(item.uid));
    strncpy(item.timestamp, timeStr.c_str(), sizeof(item.timestamp));
    item.number = pid;
    item.addToQueue = true; item.removeFromQueue = false;

    if (sharedQueueA.exists(uid)) {
        sharedQueueA.removeByUID(uid);
        sharedQueueB.add(uid, timeStr, pid);
        for (int i = 0; i < numArrivalNodes; i++) esp_now_send(arrivalMACs[i], (uint8_t*)&item, sizeof(item));
        Serial.println("🔄 UID moved from SharedQueueA to SharedQueueB.");
        blinkLED(GREEN_LED_PIN);  // Success indicator
    } else if (sharedQueue.exists(uid) || sharedQueueB.exists(uid)) {
        Serial.println("⚠️ UID already in queue.");
        blinkLED(RED_LED_PIN);  // Warning indicator
    } else {
        sharedQueue.add(uid, timeStr, pid);
        for (int i = 0; i < numArrivalNodes; i++) esp_now_send(arrivalMACs[i], (uint8_t*)&item, sizeof(item));
        Serial.println("✅ UID added to SharedQueue.");
        blinkLED(GREEN_LED_PIN);  // Success indicator
    }
    sharedQueue.print();sharedQueueA.print();sharedQueueB.print();sharedQueueC.print();
}

void createMixedQueue() {
    sharedQueueC.clear();
    std::vector<QueueEntry> entriesA = sharedQueue.getAll();
    std::vector<QueueEntry> entriesB = sharedQueueB.getAll();
    size_t indexA = 0, indexB = 0;
    while (indexA < entriesA.size() || indexB < entriesB.size()) {
        for (int i = 0; i < 5 && indexA < entriesA.size(); i++, indexA++)
            sharedQueueC.add(entriesA[indexA].uid, entriesA[indexA].timestamp, entriesA[indexA].number);
        for (int i = 0; i < 3 && indexB < entriesB.size(); i++, indexB++)
            sharedQueueC.add(entriesB[indexB].uid, entriesB[indexB].timestamp, entriesB[indexB].number);
    }
}

void printAllQueues() {
    Serial.println("📋 All Queues:");
    Serial.print("🔸 sharedQueue: "); sharedQueue.print();
    Serial.print("🔸 sharedQueueA: "); sharedQueueA.print();
    Serial.print("🔸 sharedQueueB: "); sharedQueueB.print();
    Serial.print("🔸 sharedQueueC: "); sharedQueueC.print();
}

void clearAllQueues() {
    sharedQueue.clear(); sharedQueueA.clear(); sharedQueueB.clear(); sharedQueueC.clear();
    Serial.println("🔄 All queues cleared.");
}


 void setup() {
    Serial.begin(115200);
    prefs.begin("rfidMap", false);

    // Hardcoded RFID UID to patient number mappings
    prefs.putUInt("13B6B1E3", 1);  prefs.putUInt("13D7ADE3", 2);  prefs.putUInt("A339D9E3", 3);
    prefs.putUInt("220C1805", 4);  prefs.putUInt("638348E3", 5);  prefs.putUInt("A3E9C7E3", 6);
    prefs.putUInt("5373BEE3", 7);  prefs.putUInt("62EDFF51", 8);  prefs.putUInt("131DABE3", 9);
    prefs.putUInt("B3D4B0E3", 10); prefs.putUInt("23805EE3", 11); prefs.putUInt("1310BAE3", 12);
    prefs.putUInt("D38A47E3", 13); prefs.putUInt("6307D8E3", 14); prefs.putUInt("D35FC4E3", 15);
    prefs.putUInt("C394B9E3", 16); prefs.putUInt("53D8E1E3", 17); prefs.putUInt("93DAE1E3", 18);
    prefs.putUInt("7367ABE3", 19); prefs.putUInt("7304BFE3", 20); prefs.putUInt("D374B6E3", 21);
    prefs.putUInt("33A7BDE3", 22); prefs.putUInt("9308C2E3", 23); prefs.putUInt("83CDC8E3", 24);
    prefs.putUInt("3337ECE3", 25); prefs.putUInt("C37ABFE3", 26); prefs.putUInt("D392ADE3", 27);
    prefs.putUInt("336AB9E3", 28); prefs.putUInt("B3B7AFE3", 29); prefs.putUInt("E396CBE3", 30);

    SPI.begin(); WiFi.mode(WIFI_STA);
    Serial.print("WiFi MAC: "); Serial.println(WiFi.macAddress());

    mfrc522.PCD_Init();
    pinMode(GREEN_LED_PIN, OUTPUT);
    pinMode(RED_LED_PIN, OUTPUT);
    digitalWrite(GREEN_LED_PIN, HIGH);
    digitalWrite(RED_LED_PIN, HIGH);

    if (!rtc.begin()) {
        Serial.println("❌ Couldn't find RTC module! Check wiring.");
        while (1);
    }

    if (rtc.lostPower()) {
        Serial.println("⚠️ RTC lost power, setting to compile time.");
        rtc.adjust(DateTime(__DATE__, __TIME__));
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW Init Failed");
        return;
    }

    for (int i = 0; i < numArrivalNodes; i++) {
        esp_now_peer_info_t p = {};
        memcpy(p.peer_addr, arrivalMACs[i], 6);
        p.channel = 1;
        esp_now_add_peer(&p);
    }

    for (int i = 0; i < 4; i++) {
        esp_now_peer_info_t p = {};
        memcpy(p.peer_addr, doctorMACs[i], 6);
        p.channel = 1;
        esp_now_add_peer(&p);
    }

    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    sharedQueue.load();
    sharedQueueA.load();
    sharedQueueB.load();
    sharedQueueC.load();

    createMixedQueue();
    printAllQueues();
}



void loop() {
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return;
    String uid = getUIDString(mfrc522.uid.uidByte, mfrc522.uid.size);
    processCard(uid);
    mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1(); delay(1200);
    createMixedQueue(); printAllQueues();
}

String getUIDString(byte *buf, byte size) {
    String uid=""; for (byte i=0;i<size;i++){if(buf[i]<0x10)uid+="0"; uid+=String(buf[i],HEX);} uid.toUpperCase(); return uid;
}

void blinkLED(int pin) {
    digitalWrite(pin, LOW); delay(500); digitalWrite(pin, HIGH);
}

int getPermanentNumber(String uid) {
    prefs.begin("rfidMap", true); int pid=-1;
    if (prefs.isKey(uid.c_str())) pid = prefs.getUInt(uid.c_str(),-1);
    prefs.end(); return pid;
}
