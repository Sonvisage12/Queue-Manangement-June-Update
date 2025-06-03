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
SharedQueue sharedQueueC("queue-C");  // Mixed Queue

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

    if (isArrivalNode) {
        if (item.addToQueue && !item.removeFromQueue) {
            sharedQueue.addIfNew(String(item.uid), String(item.timestamp), item.number);
        } else if (!item.addToQueue && item.removeFromQueue) {
            sharedQueue.removeByUID(String(item.uid));
        }
    } else if (isDoctorNode) {
        if (strcmp(item.uid, "REQ_NEXT") == 0) {
            int doctorNodeID = -1;
            for (int i = 0; i < 4; i++) if (memcmp(mac, doctorMACs[i], 6) == 0) doctorNodeID = i + 1;
            if (doctorNodeID == -1) return;
            if (!sharedQueueC.empty()) {
                QueueEntry entry = sharedQueueC.front();
                QueueItem sendItem;
                strncpy(sendItem.uid, entry.uid.c_str(), sizeof(sendItem.uid));
                strncpy(sendItem.timestamp, entry.timestamp.c_str(), sizeof(sendItem.timestamp));
                sendItem.number = entry.number;
                sendItem.node = doctorNodeID;
                esp_now_send(mac, (uint8_t*)&sendItem, sizeof(sendItem));
                sharedQueueC.pop(); sharedQueueC.push(entry);
            } else {
                QueueItem zeroItem = {};
                strncpy(zeroItem.uid, "NO_PATIENT", sizeof(zeroItem.uid));
                esp_now_send(mac, (uint8_t*)&zeroItem, sizeof(zeroItem));
            }
        } else if (item.removeFromQueue) {
    if (sharedQueue.exists(String(item.uid))) {
        // If UID is in sharedQueue
        sharedQueue.removeByUID(String(item.uid));
        sharedQueueC.removeByUID(String(item.uid));
        sharedQueueA.add(String(item.uid), String(item.timestamp), item.number);
        Serial.printf("🗑️ UID %s removed from SharedQueue and added to SharedQueueA.\n", item.uid);
    } else {
        // Otherwise, remove from SharedQueueC and SharedQueueB
        sharedQueueC.removeByUID(String(item.uid));
        sharedQueueB.removeByUID(String(item.uid));
        Serial.printf("🗑️ UID %s removed from SharedQueueC and SharedQueueB.\n", item.uid);
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
    } else if (sharedQueue.exists(uid)) {
    } else if (sharedQueueB.exists(uid)) {
    } else {
        sharedQueue.add(uid, timeStr, pid);
        for (int i = 0; i < numArrivalNodes; i++) esp_now_send(arrivalMACs[i], (uint8_t*)&item, sizeof(item));
    }
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
    Serial.println("📋 Contents of All Queues:");
    Serial.print("🔸 sharedQueue: "); sharedQueue.print();
    Serial.print("🔸 sharedQueueA: "); sharedQueueA.print();
    Serial.print("🔸 sharedQueueB: "); sharedQueueB.print();
    Serial.print("🔸 sharedQueueC: "); sharedQueueC.print();
}

void clearAllQueues() {
    sharedQueue.clear();
    sharedQueueA.clear();
    sharedQueueB.clear();
    sharedQueueC.clear();
    Serial.println("🔄 All queues cleared.");
}

void setup() {
    Serial.begin(115200);
   //clearAllQueues();
    prefs.begin("rfidMap", false);
    SPI.begin(); WiFi.mode(WIFI_STA);
    mfrc522.PCD_Init(); pinMode(GREEN_LED_PIN, OUTPUT); pinMode(RED_LED_PIN, OUTPUT);
    digitalWrite(GREEN_LED_PIN, HIGH); digitalWrite(RED_LED_PIN, HIGH);
    if (!rtc.begin()) while (1);
    if (rtc.lostPower()) rtc.adjust(DateTime(__DATE__, __TIME__));
    if (esp_now_init() != ESP_OK) return;
    for (int i = 0; i < numArrivalNodes; i++) { esp_now_peer_info_t p={}; memcpy(p.peer_addr, arrivalMACs[i],6); p.channel=1; esp_now_add_peer(&p); }
    for (int i = 0; i < 4; i++) { esp_now_peer_info_t p={}; memcpy(p.peer_addr, doctorMACs[i],6); p.channel=1; esp_now_add_peer(&p); }
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);
    sharedQueue.load(); sharedQueueA.load(); sharedQueueB.load(); sharedQueueC.load();
    createMixedQueue();
    printAllQueues();  // 🔥 Print queues at startup
}

void loop() {
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return;
    String uid = getUIDString(mfrc522.uid.uidByte, mfrc522.uid.size);
    processCard(uid);
    mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1(); delay(1500);
    createMixedQueue();
    printAllQueues();  // 🔥 Print queues after each scan
}

String getUIDString(byte *buf, byte size) {
    String uid=""; for (byte i=0;i<size;i++){if(buf[i]<0x10)uid+="0"; uid+=String(buf[i],HEX);} uid.toUpperCase(); return uid;
}

void blinkLED(int pin) { digitalWrite(pin, LOW); delay(1000); digitalWrite(pin, HIGH); }

int getPermanentNumber(String uid) {
    prefs.begin("rfidMap", true); int pid=-1;
    if (prefs.isKey(uid.c_str())) pid = prefs.getUInt(uid.c_str(),-1);
    prefs.end(); return pid;
}
