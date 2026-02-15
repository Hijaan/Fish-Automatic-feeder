#include <WiFiUdp.h>
#include <NTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <EEPROM.h>
#include <RTClib.h>

// ---------------- WiFi & Telegram ----------------
const char* ssid = "Picasso";
const char* password = "22222222";
const char* botToken = "6635161265:AAHRFolKRZPURqUJcBD-5axpY1yEG6HF0II";

WiFiClientSecure client;
UniversalTelegramBot bot(botToken, client);

//------------------NTP SYNC-----------
WiFiUDP ntpUDP;

NTPClient timeClient(
  ntpUDP,
  "pool.ntp.org",   // server NTP
  8 * 3600,         // offset zona waktu WITA (UTC+7)
  60000             // interval update (1 menit)
);

// ___ WAKTU DELAY ___ (motor utama & motor dispenser)
int waktuDelay = 12000;   // main motor run delay in ms (persisted)
int motor2Delay = 12000;  // dispenser motor run delay in ms (persisted)
int bukaanPakan = 2000;

// increment step and interval (now configurable & persisted)
int incrementStep = 20;                 // ms added every interval (persisted)
uint16_t incrementIntervalDays = 3;     // interval in days (persisted)
// Helper to compute seconds from days when used

// --- Admin chat id (only this user can add users) ---
const String adminChatId = "5627733104"; // <-- replace with your admin chat_id

// --- Allowed users (initial) ---
String allowedUsersDefault[] = {
  "5627733104",   // admin (example)
  "6886222138"
};
#define MAX_ALLOWED_USERS 5
String allowedUsers[MAX_ALLOWED_USERS];
int allowedUsersCount = 0;

// Telegram request pacing
int botRequestDelay = 1000;
unsigned long lastTimeBotRan = 0;

// ---------------- Hardware ----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;

// Motor driver 1 pins (main feeder motor)
#define IN1 12
#define IN2 13

// Motor driver 2 pins (dispenser motor, menggantikan servo)
#define IN3 25 
#define IN4 33

// HC-SR04 pins (level sensor)
#define TRIG 26
#define ECHO 27
 
#define USE_PWM 0           // 0 = simple digital on/off, 1 = use ESP32 PWM (ledc)

// Keypad 4x4
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {19, 18, 5, 17};
byte colPins[COLS] = {16, 4, 2, 15};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ---------------- Scheduling & EEPROM ----------------
#define EEPROM_SIZE 128
// jadwal addresses
#define ADDR_JAM1   0
#define ADDR_MENIT1 1
#define ADDR_JAM2   2
#define ADDR_MENIT2 3
// allowed users storage start
#define ADDR_ALLOWED_BASE 8

// additional EEPROM addresses (must not overlap)
#define ADDR_WAKTU_DELAY   40    // 4 bytes (uint32)
#define ADDR_SERVO_DELAY   44    // 2 bytes (uint16) -- used for motor2Delay
#define ADDR_LAST_INC      48    // 4 bytes (uint32) unix timestamp of last increment
#define ADDR_INCREMENT_STEP 52   // 2 bytes (uint16)
#define ADDR_INCREMENT_DAYS 54   // 2 bytes (uint16)

int jadwalJam1 = 8;
int jadwalMenit1 = 0;
int jadwalJam2 = 15;
int jadwalMenit2 = 0;
bool sudahPakan1 = false;
bool sudahPakan2 = false;

// State for Telegram interactive setting
int awaitingJadwal = 0;       // 0=none, 1=jadwal1, 2=jadwal2
String awaitingChatId = "";  // chat id that bot expects time from
bool awaitingAddUser = false; // waiting admin to send new user id
String awaitingAddUserChat = "";

// Menu & misc
int menuIndex = 0;
String menuList[3] = {"1.Status", "2.Jadwal", "3.Manual"};

// ***************************************************************
// *** EEPROM helpers for multi-byte values
// ***************************************************************
void eepromWriteUint32(int addr, uint32_t val) {
  EEPROM.write(addr, val & 0xFF);
  EEPROM.write(addr + 1, (val >> 8) & 0xFF);
  EEPROM.write(addr + 2, (val >> 16) & 0xFF);
  EEPROM.write(addr + 3, (val >> 24) & 0xFF);
}
uint32_t eepromReadUint32(int addr) {
  uint32_t b0 = EEPROM.read(addr);
  uint32_t b1 = EEPROM.read(addr + 1);
  uint32_t b2 = EEPROM.read(addr + 2);
  uint32_t b3 = EEPROM.read(addr + 3);
  uint32_t val = (b0) | (b1 << 8) | (b2 << 16) | (b3 << 24);
  return val;
}
void eepromWriteUint16(int addr, uint16_t val) {
  EEPROM.write(addr, val & 0xFF);
  EEPROM.write(addr + 1, (val >> 8) & 0xFF);
}
uint16_t eepromReadUint16(int addr) {
  uint16_t b0 = EEPROM.read(addr);
  uint16_t b1 = EEPROM.read(addr + 1);
  return (b0) | (b1 << 8);
}

// ***************************************************************
// *** PROTOTYPES
// ***************************************************************
void tampilMenu();
void menuStatusPakan();
void menuJadwalPakan();
void menuPakanManual();
void setJadwal(int nomor);
void beriPakan();
void maybeIncrementDelaysIfNeeded();

// ---------------- Helpers ----------------
String formatTime(int jam, int menit) {
  char b[6];
  sprintf(b, "%02d:%02d", jam, menit);
  return String(b);
}
String formatTime(DateTime now) {
  return formatTime(now.hour(), now.minute());
}

void saveJadwalToEEPROM() {
  EEPROM.write(ADDR_JAM1, jadwalJam1);
  EEPROM.write(ADDR_MENIT1, jadwalMenit1);
  EEPROM.write(ADDR_JAM2, jadwalJam2);
  EEPROM.write(ADDR_MENIT2, jadwalMenit2);
  EEPROM.commit();
}

void loadJadwalFromEEPROM() {
  int a = EEPROM.read(ADDR_JAM1);
  int b = EEPROM.read(ADDR_MENIT1);
  int c = EEPROM.read(ADDR_JAM2);
  int d = EEPROM.read(ADDR_MENIT2);

  if (a == 255 || b == 255 || c == 255 || d == 255) {
    // initialize defaults
    jadwalJam1 = 8; jadwalMenit1 = 0;
    jadwalJam2 = 15; jadwalMenit2 = 0;
    saveJadwalToEEPROM();
  } else {
    jadwalJam1 = a;
    jadwalMenit1 = b;
    jadwalJam2 = c;
    jadwalMenit2 = d;
  }
}

// Allowed users persistency: simple format
void saveAllowedUsersToEEPROM() {
  int addr = ADDR_ALLOWED_BASE;
  EEPROM.write(addr++, allowedUsersCount);
  for (int i = 0; i < allowedUsersCount; i++) {
    String s = allowedUsers[i];
    int len = s.length();
    if (len > 30) len = 30; // cap
    EEPROM.write(addr++, len);
    for (int j = 0; j < len; j++) {
      EEPROM.write(addr++, s[j]);
    }
  }
  EEPROM.commit();
}

void loadAllowedUsersFromEEPROM() {
  int addr = ADDR_ALLOWED_BASE;
  int count = EEPROM.read(addr++);
  if (count <= 0 || count > MAX_ALLOWED_USERS) {
    // initialize from default list
    allowedUsersCount = sizeof(allowedUsersDefault)/sizeof(allowedUsersDefault[0]);
    if (allowedUsersCount > MAX_ALLOWED_USERS) allowedUsersCount = MAX_ALLOWED_USERS;
    for (int i = 0; i < allowedUsersCount; i++) allowedUsers[i] = allowedUsersDefault[i];
    saveAllowedUsersToEEPROM();
    return;
  }
  allowedUsersCount = count;
  for (int i = 0; i < allowedUsersCount; i++) {
    int len = EEPROM.read(addr++);
    String s = "";
    for (int j = 0; j < len; j++) {
      char c = (char)EEPROM.read(addr++);
      s += c;
    }
    allowedUsers[i] = s;
  }
}

// ---------------- Allowed users management ----------------
bool isUserAllowed(String user_id) {
  for (int i = 0; i < allowedUsersCount; i++) {
    if (allowedUsers[i] == user_id) return true;
  }
  return false;
}

void addAllowedUser(String newUserId) {
  // check duplicate
  for (int i = 0; i < allowedUsersCount; i++) {
    if (allowedUsers[i] == newUserId) {
      Serial.println("User exists: " + newUserId);
      return;
    }
  }
  if (allowedUsersCount < MAX_ALLOWED_USERS) {
    allowedUsers[allowedUsersCount++] = newUserId;
    saveAllowedUsersToEEPROM();
    Serial.println("Added allowed user: " + newUserId);
  } else {
    Serial.println("Allowed user list full");
  }
}

void sendToAllowedUsers(String message) {
  for (int i = 0; i < allowedUsersCount; i++) {
    bot.sendMessage(allowedUsers[i], message, "Markdown");
  }
}

// ---------------- Hardware functions ----------------
void beriPakan() {
   // Safety: do not block forever. max timeout factor (in case limit switches missing)
  const unsigned long TIMEOUT_FACTOR = 2UL; // allow up to 2x requested delay

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Memberi Pakan...");

  // --- Move actuator FORWARD ---
//Linear Aktuator Buka Maju & Motor mutar
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  delay(200);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
  delay(waktuDelay);

//Linear aktuator mundur & motor mutar
  digitalWrite(IN3, LOW); 
  digitalWrite(IN4, HIGH);
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  delay(waktuDelay);

//Berhenti
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Pakan Selesai!");
  delay(1000);

  tampilMenu();
}

long bacaJarak() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long durasi = pulseIn(ECHO, HIGH, 30000); // timeout 30ms
  if (durasi == 0) return 999; // no echo
  long jarak = durasi * 0.034 / 2;
  return jarak;
}

// ---------------- Display & menu ----------------
void tampilMenu() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Menu:");
  lcd.setCursor(0,1);
  lcd.print(menuList[menuIndex]);
}

void tampilStatusLCD() {
  DateTime now = rtc.now();
  long jarak = bacaJarak();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Lvl:");
  if (jarak < 10) lcd.print("Penuh");
  else if (jarak < 30) lcd.print("Sedang");
  else if (jarak == 999) lcd.print("Err");
  else lcd.print("Habis");

  lcd.setCursor(11,0);
  lcd.print(formatTime(now.hour(), now.minute()));
  lcd.setCursor(0,1);
  lcd.print("J1:");
  lcd.print(formatTime(jadwalJam1, jadwalMenit1));
  lcd.setCursor(9,1);
  lcd.print("J2:");
  lcd.print(formatTime(jadwalJam2, jadwalMenit2));
}

// ---------------- Telegram menus ----------------
void showMainMenu(String chat_id, String from_name) {
  String welcome = "🐟 *Pakan Ikan ITK* 🐟\n";
  welcome += "Halo, " + from_name + "!\n";
  welcome += "Pilih opsi:";
  String keyboardJson =
    "{\"keyboard\": [[\"/status\", \"/pakan\"], [\"/jadwal\", \"/allowed\"], [\"/help\"]],"
    "\"resize_keyboard\": true, \"one_time_keyboard\": false}";
  bot.sendMessageWithReplyKeyboard(chat_id, welcome, "Markdown", keyboardJson, true);
}

void showJadwalMenu(String chat_id) {
  String jadwalMsg = "⏰ *Jadwal Pakan Saat Ini* ⏰\n\n";
  jadwalMsg += "J1: " + formatTime(jadwalJam1, jadwalMenit1) + "\n";
  jadwalMsg += "J2: " + formatTime(jadwalJam2, jadwalMenit2) + "\n\n";
  jadwalMsg += "Pilih aksi:";
  String keyboardJson =
    "{\"keyboard\":[[\"/set_jadwal1\", \"/set_jadwal2\"],[\"/status\",\"/help\"]],"
    "\"resize_keyboard\":true,\"one_time_keyboard\":false}";
  bot.sendMessageWithReplyKeyboard(chat_id, jadwalMsg, "Markdown", keyboardJson, true);
}

void sendStatus(String chat_id) {
  DateTime now = rtc.now();
  long jarak = bacaJarak();

  String statusMsg = "📊 *Status Sistem* 📊\n\n";
  statusMsg += "🕐 Waktu: " + formatTime(now) + "\n";
  statusMsg += "📦 Level Pakan: ";
  statusMsg += (jarak == 999) ? "Err" : ((jarak < 10) ? "Penuh" : (jarak < 20 ? "Sedang" : "Habis"));
  statusMsg += "\n";
  statusMsg += "📅 Jadwal:\n";
  statusMsg += "• J1: " + formatTime(jadwalJam1, jadwalMenit1) + " " + (sudahPakan1 ? "✅" : "❌") + "\n";
  statusMsg += "• J2: " + formatTime(jadwalJam2, jadwalMenit2) + " " + (sudahPakan2 ? "✅" : "❌") + "\n";
  statusMsg += "\n⏱️ Delays:\n";
  statusMsg += "• Motor utama: " + String(waktuDelay) + " ms\n";
  statusMsg += "• Motor dispenser: " + String(motor2Delay) + " ms\n";
  statusMsg += "\n🔁 Increment every " + String(incrementIntervalDays) + " day(s): +" + String(incrementStep) + " ms\n";

  bot.sendMessage(chat_id, statusMsg, "Markdown");
}

// ---------------- Set jadwal from telegram ----------------
void setJadwalFromTelegram(int nomor, String timeStr, String chat_id) {
  int colonIndex = timeStr.indexOf(':');
  if (colonIndex == -1) {
    bot.sendMessage(chat_id, "❌ Format salah! Gunakan HH:MM", "");
    return;
  }

  String jamStr = timeStr.substring(0, colonIndex);
  String menitStr = timeStr.substring(colonIndex + 1);
  jamStr.trim(); menitStr.trim();
  int jam = jamStr.toInt();
  int menit = menitStr.toInt();

  if (jam < 0 || jam > 23 || menit < 0 || menit > 59) {
    bot.sendMessage(chat_id, "❌ Format waktu tidak valid! Gunakan 00–23:00–59", "");
    return;
  }

  if (nomor == 1) {
    jadwalJam1 = jam; jadwalMenit1 = menit; sudahPakan1 = false;
  } else {
    jadwalJam2 = jam; jadwalMenit2 = menit; sudahPakan2 = false;
  }

  saveJadwalToEEPROM();
  String msg = "✅ *Jadwal " + String(nomor) + " Disimpan!* ✅\nWaktu: " + formatTime(jam, menit);
  bot.sendMessage(chat_id, msg, "Markdown");
}

// ---------------- Telegram incoming handler ----------------
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    Serial.println("Msg from: " + chat_id + " -> " + text);

    // security: only allowed users
    if (!isUserAllowed(chat_id)) {
      bot.sendMessage(chat_id, "⛔ Akses ditolak. Anda tidak terdaftar.", "");
      continue;
    }

    // --- Normalize and parse command/argument (robust) ---
    text.trim();
    if (text.length() == 0) continue;

    String cmd = text;
    String arg = "";
    int sp = cmd.indexOf(' ');
    if (sp != -1) {
      arg = cmd.substring(sp + 1);
      arg.trim();
      cmd = cmd.substring(0, sp);
      cmd.trim();
    }

    // remove CR/LF and any stray control chars
    cmd.replace("\r", "");
    cmd.replace("\n", "");

    // remove botname suffix if present (e.g. "/help@MyBot" -> "/help")
    int atPos = cmd.indexOf('@');
    if (atPos != -1) {
      cmd = cmd.substring(0, atPos);
    }

    // make command comparisons case-insensitive
    cmd.toLowerCase();

    // debug log for troubleshooting
    Serial.println("Parsed cmd: [" + cmd + "] arg: [" + arg + "]");

    // If we're awaiting a time from this chat, handle it first
    if (awaitingJadwal != 0 && awaitingChatId == chat_id) {
      if (text.indexOf(':') != -1) {
        setJadwalFromTelegram(awaitingJadwal, text, chat_id);
        awaitingJadwal = 0;
        awaitingChatId = "";
      } else {
        bot.sendMessage(chat_id, "⚠️ Harap kirim waktu dengan format HH:MM. Contoh: 13:38", "");
      }
      continue;
    }

    // If we're awaiting an add_user value from admin
    if (awaitingAddUser && awaitingAddUserChat == chat_id) {
      // expect a chat_id string of new user
      String newId = text;
      newId.trim();
      if (newId.length() > 0) {
        addAllowedUser(newId);
        bot.sendMessage(chat_id, "✅ User ditambahkan: " + newId, "");
      } else {
        bot.sendMessage(chat_id, "❌ ID tidak valid.", "");
      }
      awaitingAddUser = false;
      awaitingAddUserChat = "";
      continue;
    }

    // Normal commands - use parsed cmd and arg
    if (cmd == "/status") {
      sendStatus(chat_id);
    }
    else if (cmd == "/pakan") {
      bot.sendMessage(chat_id, "🔄 Memberi pakan manual...", "");
      beriPakan();
      DateTime now = rtc.now();
      bot.sendMessage(chat_id, "✅ Pakan manual diberikan pada " + formatTime(now), "Markdown");
    }
    else if (cmd == "/jadwal") {
      showJadwalMenu(chat_id);
    }
    else if (cmd == "/set_jadwal1" || cmd == "/setjadwal1") {
      bot.sendMessage(chat_id, "🕐 Silakan kirim waktu untuk Jadwal 1 (format HH:MM)", "");
      awaitingJadwal = 1;
      awaitingChatId = chat_id;
    }
    else if (cmd == "/set_jadwal2" || cmd == "/setjadwal2") {
      bot.sendMessage(chat_id, "🕐 Silakan kirim waktu untuk Jadwal 2 (format HH:MM)", "");
      awaitingJadwal = 2;
      awaitingChatId = chat_id;
    }
    else if (cmd == "/add_user") {
      // only admin allowed to add
      if (chat_id == adminChatId) {
        bot.sendMessage(chat_id, "Kirim chat_id user baru yang ingin ditambahkan:", "");
        awaitingAddUser = true;
        awaitingAddUserChat = chat_id;
      } else {
        bot.sendMessage(chat_id, "❌ Hanya admin yang dapat menambah user.", "");
      }
    }
    else if (cmd == "/allowed") {
      String msg = "👥 Allowed Users:\n";
      for (int k = 0; k < allowedUsersCount; k++) {
        msg += String(k+1) + ". " + allowedUsers[k] + "\n";
      }
      bot.sendMessage(chat_id, msg, "");
    }
    // Accept both /set_motor2 and /setmotor2 (and handle if arg missing)
    else if (cmd == "/set_motor2" || cmd == "/setmotor2") {
      if (arg.length() == 0) {
        bot.sendMessage(chat_id, "❗ Kirim nilai delay dalam ms. Contoh: /set_motor2 12000", "");
      } else {
        String val = arg;
        val.trim();
        int v = val.toInt();
        if (v > 0 && v <= 60000) {
          motor2Delay = v;
          eepromWriteUint16(ADDR_SERVO_DELAY, (uint16_t)motor2Delay);
          EEPROM.commit();
          bot.sendMessage(chat_id, "✅ Dispenser delay disimpan: " + String(motor2Delay) + " ms", "");
        } else {
          bot.sendMessage(chat_id, "❌ Nilai tidak valid. Gunakan ms (1..60000). Contoh: /set_motor2 12000", "");
        }
      }
    }
    // Accept both /get_motor2 and /getmotor2 for convenience
    else if (cmd == "/get_motor2" || cmd == "/getmotor2") {
      bot.sendMessage(chat_id, "Dispenser delay saat ini: " + String(motor2Delay) + " ms", "");
    }
    // New: set/get motor1 (main motor) delay
    else if (cmd == "/set_motor1" || cmd == "/setmotor1") {
      if (arg.length() == 0) {
        bot.sendMessage(chat_id, "❗ Kirim nilai delay dalam ms. Contoh: /set_motor1 12000", "");
      } else {
        String val = arg;
        val.trim();
        long v = val.toInt();
        if (v > 0 && v <= 60000L) {
          waktuDelay = (int)v;
          eepromWriteUint32(ADDR_WAKTU_DELAY, (uint32_t)waktuDelay);
          EEPROM.commit();
          bot.sendMessage(chat_id, "✅ Motor utama delay disimpan: " + String(waktuDelay) + " ms", "");
        } else {
          bot.sendMessage(chat_id, "❌ Nilai tidak valid. Gunakan ms (1..60000). Contoh: /set_motor1 12000", "");
        }
      }
    }
    else if (cmd == "/get_motor1" || cmd == "/getmotor1") {
      bot.sendMessage(chat_id, "Motor utama delay saat ini: " + String(waktuDelay) + " ms", "");
    }
    // Increment settings
    else if (cmd == "/get_increment") {
      bot.sendMessage(chat_id, "🔁 Increment every " + String(incrementIntervalDays) + " day(s): +" + String(incrementStep) + " ms each", "");
    }
    else if (cmd == "/atur_jarak_hari") {
      if (arg.length() == 0) {
        bot.sendMessage(chat_id, "❗ Kirim jumlah hari. Contoh: /atur_jarak_hari 3", "");
      } else {
        int days = arg.toInt();
        if (days >= 1 && days <= 30) {
          incrementIntervalDays = (uint16_t)days;
          eepromWriteUint16(ADDR_INCREMENT_DAYS, incrementIntervalDays);
          EEPROM.commit();
          bot.sendMessage(chat_id, "✅ Interval increment disimpan: " + String(incrementIntervalDays) + " hari", "");
        } else {
          bot.sendMessage(chat_id, "❌ Nilai tidak valid. Gunakan 1..30 hari.", "");
        }
      }
    }
    else if (cmd == "/atur_tambah_pakan") {
      if (arg.length() == 0) {
        bot.sendMessage(chat_id, "❗ Kirim ms per increment. Contoh: /atur_tambah_pakan 20", "");
      } else {
        int step = arg.toInt();
        if (step >= 1 && step <= 5000) {
          incrementStep = step;
          eepromWriteUint16(ADDR_INCREMENT_STEP, (uint16_t)incrementStep);
          EEPROM.commit();
          bot.sendMessage(chat_id, "✅ Increment step disimpan: " + String(incrementStep) + " ms", "");
        } else {
          bot.sendMessage(chat_id, "❌ Nilai tidak valid. Gunakan 1..5000 ms.", "");
        }
      }
    }
    else if (cmd == "/help" || cmd == "/menu") {
      String helpMsg = "📘 Daftar Perintah 📘\n\n";
      helpMsg += "/menu - Tampilkan menu utama\n";
      helpMsg += "/status - Lihat status & jadwal\n";
      helpMsg += "/pakan - Pakan manual\n";
      helpMsg += "/set_jadwal1 - Atur Jadwal 1\n";
      helpMsg += "/set_jadwal2 - Atur Jadwal 2\n";
      helpMsg += "/add_user - Tambah user (admin only)\n";
      helpMsg += "/allowed - Lihat daftar allowed users\n";
      helpMsg += "/get_motor1 - Tampilkan motor utama delay saat ini\n";
      helpMsg += "/set_motor1 (ms) - Set dan simpan motor utama delay (ms)\n";
      helpMsg += "/get_motor2 - Tampilkan dispenser delay saat ini\n";
      helpMsg += "/set_motor2 (ms) - Set dan simpan dispenser delay (ms)\n";
      helpMsg += "/get_increment - Tampilkan pengaturan increment\n";
      helpMsg += "/atur_jarak_hari (n) - Set interval increment (hari)\n";
      helpMsg += "/atur_tambah_pakan (ms) - Set step increment (ms)\n";
      helpMsg += "/sync_rtc - Sinkronisasi RTC dengan NTP\n";
  // send as plain text (no Markdown)
    bot.sendMessage(chat_id, helpMsg, "");
    }
    else if (cmd == "/sync_rtc" || cmd == "/syncrtc") {
      syncRTCfromNTP(chat_id);
    }

    else {
      bot.sendMessage(chat_id, "Perintah tidak dikenali. Ketik /menu atau /help", "");
    }
  }
}


// ---------------- Sync RTC dengan NTP ------------
void syncRTCfromNTP(String chat_id) {
  if (WiFi.status() != WL_CONNECTED) {
    if (chat_id.length()) bot.sendMessage(chat_id, "❌ WiFi belum terhubung!", "");
    return;
  }

  timeClient.update();

  unsigned long epochTime = timeClient.getEpochTime();

  if (epochTime < 100000) {
    if (chat_id.length()) bot.sendMessage(chat_id, "❌ Gagal ambil waktu dari NTP!", "");
    return;
  }

  rtc.adjust(DateTime(epochTime));

  String msg = "✅ *RTC Berhasil Disinkronkan Dengan NTP!* \nWaktu sekarang: ";
  msg += String(timeClient.getFormattedTime());

  if (chat_id.length()) bot.sendMessage(chat_id, msg, "Markdown");
  
  Serial.println(msg);
}

// ---------------- Setup & Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(10);

  // EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadJadwalFromEEPROM();
  loadAllowedUsersFromEEPROM();

  // load persisted delays and last increment
  // read waktuDelay (4 bytes)
  uint32_t wd = eepromReadUint32(ADDR_WAKTU_DELAY);
  uint16_t sd = eepromReadUint16(ADDR_SERVO_DELAY);
  uint32_t lastInc = eepromReadUint32(ADDR_LAST_INC);
  uint16_t savedStep = eepromReadUint16(ADDR_INCREMENT_STEP);
  uint16_t savedDays = eepromReadUint16(ADDR_INCREMENT_DAYS);

  // If EEPROM empty (all 0xFF), initialize default values
  if (wd == 0xFFFFFFFFUL) {
    waktuDelay = 12000;
    eepromWriteUint32(ADDR_WAKTU_DELAY, (uint32_t)waktuDelay);
  } else {
    waktuDelay = (int)wd;
  }

  // Use 12000 as default dispenser delay if EEPROM empty (previous code used 900)
  if (sd == 0xFFFF) {
    motor2Delay = 12000; // default yang diinginkan
    eepromWriteUint16(ADDR_SERVO_DELAY, (uint16_t)motor2Delay);
  } else {
    motor2Delay = (int)sd;
  }

  // increment step & days defaults
  if (savedStep == 0xFFFF) {
    incrementStep = 20;
    eepromWriteUint16(ADDR_INCREMENT_STEP, (uint16_t)incrementStep);
  } else {
    incrementStep = (int)savedStep;
  }

  if (savedDays == 0xFFFF) {
    incrementIntervalDays = 3;
    eepromWriteUint16(ADDR_INCREMENT_DAYS, (uint16_t)incrementIntervalDays);
  } else {
    incrementIntervalDays = (uint16_t)savedDays;
  }

  if (lastInc == 0xFFFFFFFFUL) {
    // not set yet: initialize to current time after RTC starts
    eepromWriteUint32(ADDR_LAST_INC, 0); // will set real value after rtc init
  }

  EEPROM.commit();

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Pakan Ikan ITK");
  delay(700);

  // RTC
  if (!rtc.begin()) {
    lcd.clear();
    lcd.print("RTC Error!");
    Serial.println("RTC not found");
    while (1);
  }
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // if lastInc was zero/uninitialized, set it now to current day boundary
  uint32_t savedLastInc = eepromReadUint32(ADDR_LAST_INC);
  if (savedLastInc == 0) {
    uint32_t nowTs = rtc.now().unixtime();
    eepromWriteUint32(ADDR_LAST_INC, nowTs);
    EEPROM.commit();
  }

  // Motor pins
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);

  // WiFi connect
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  lcd.setCursor(0,1);
  lcd.print("Connecting...");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi: " + WiFi.localIP().toString());
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("WiFi OK");
    lcd.setCursor(0,1);
    lcd.print(WiFi.localIP().toString());
  } else {
    lcd.clear();
    lcd.print("WiFi Failed");
  }
  delay(800);
  
  // Telegram secure client: for ESP32 simple testing we can set insecure
  client.setInsecure();
  
  // CALLL NTP
  timeClient.begin();
  delay(500);
  syncRTCfromNTP(""); // Hapus spasi dan tambahkan tanda kutip kosong

  // Notify allowed users
  String welcomeMsg = "🤖 *Pakan Ikan ITK Started!* \n";
  welcomeMsg += "IP: " + WiFi.localIP().toString() + "\n";
  welcomeMsg += "Motor delay: " + String(waktuDelay) + " ms\n";
  welcomeMsg += "Dispenser delay: " + String(motor2Delay) + " ms\n";
  welcomeMsg += "Banyak pakan ditambahkan tiap" + String(incrementIntervalDays) + " day(s): +" + String(incrementStep) + " ms\n";
  welcomeMsg += "Gunakan /pakan untuk Beri Pakan.";
  sendToAllowedUsers(welcomeMsg);

  tampilMenu();
}

void loop() {
  static unsigned long lastNTP = 0;
  if (millis() - lastNTP > 6UL * 3600UL * 1000UL) { // 6 jam
    lastNTP = millis();
    syncRTCfromNTP(""); // Tambahkan tanda kutip kosong
  }

  // first, check and apply any increments if needed
  maybeIncrementDelaysIfNeeded();

  // keypad handling (non-blocking)
  char key = keypad.getKey();
  if (key) {
    if (key == 'A') {
      menuIndex--;
      if (menuIndex < 0) menuIndex = 2;
      tampilMenu();
    } else if (key == 'B') {
      menuIndex++;
      if (menuIndex > 2) menuIndex = 0;
      tampilMenu();
    } else if (key == 'D') {
      if (menuIndex == 0) menuStatusPakan();
      else if (menuIndex == 1) menuJadwalPakan();
      else if (menuIndex == 2) menuPakanManual();
    } else if (key == '#') {
      // manual quick feed from keypad
      beriPakan();
    }
  }

  // Telegram polling (rate-limited)
  if (millis() > lastTimeBotRan + botRequestDelay) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = millis();
  }

  // Automatic feeding check
  DateTime now = rtc.now();
  if (now.hour() == jadwalJam1 && now.minute() == jadwalMenit1 && !sudahPakan1) {
    beriPakan();
    sudahPakan1 = true;
    String msg = "⏰ *Pakan Otomatis Jadwal 1* ⏰\nWaktu: " + formatTime(now);
    sendToAllowedUsers(msg);
  }
  if (now.hour() == jadwalJam2 && now.minute() == jadwalMenit2 && !sudahPakan2) {
    beriPakan();
    sudahPakan2 = true;
    String msg = "⏰ *Pakan Otomatis Jadwal 2* ⏰\nWaktu: " + formatTime(now);
    sendToAllowedUsers(msg);
  }
  // reset daily flags at midnight (00:00)
  static int lastCheckedDay = -1;
  if (now.day() != lastCheckedDay) {
    sudahPakan1 = false;
    sudahPakan2 = false;
    lastCheckedDay = now.day();
  }

  delay(120);
}

// check RTC and EEPROM to increment delays every N days (configurable)
void maybeIncrementDelaysIfNeeded() {
  DateTime now = rtc.now();
  uint32_t nowTs = now.unixtime();
  uint32_t lastInc = eepromReadUint32(ADDR_LAST_INC);
  // if lastInc is zero (or uninitialized) set it
  if (lastInc == 0 || lastInc == 0xFFFFFFFFUL) {
    eepromWriteUint32(ADDR_LAST_INC, nowTs);
    EEPROM.commit();
    return;
  }
  if (nowTs <= lastInc) return;

  uint32_t incrementIntervalS = (uint32_t)incrementIntervalDays * 24UL * 3600UL;
  if (incrementIntervalS == 0) return; // guard

  uint32_t diff = nowTs - lastInc;
  if (diff < incrementIntervalS) return;

  uint32_t increments = diff / incrementIntervalS; // how many periods passed
  if (increments == 0) return;

  // apply increments
  long totalMotorAdd = (long)increments * (long)incrementStep;
  long totalMotor2Add = (long)increments * (long)incrementStep;
  waktuDelay += totalMotorAdd;
  motor2Delay += totalMotor2Add;

  // update lastInc to skip the applied periods (avoid rounding issues)
  uint32_t advanced = increments * incrementIntervalS;
  lastInc += advanced;

  // save to EEPROM
  eepromWriteUint32(ADDR_WAKTU_DELAY, (uint32_t)waktuDelay);
  eepromWriteUint16(ADDR_SERVO_DELAY, (uint16_t)motor2Delay); // kept same address
  eepromWriteUint32(ADDR_LAST_INC, lastInc);
  eepromWriteUint16(ADDR_INCREMENT_STEP, (uint16_t)incrementStep);
  eepromWriteUint16(ADDR_INCREMENT_DAYS, (uint16_t)incrementIntervalDays);
  EEPROM.commit();

  // notify allowed users
  String msg = "🔧 *Delay Auto-Increment*\n";
  msg += "Setiap " + String(incrementIntervalDays) + " hari ditambah " + String(incrementStep) + " ms.\n";
  msg += "Periode yg terlewat: " + String(increments) + "\n";
  msg += "Motor utama sekarang: " + String(waktuDelay) + " ms\n";
  msg += "Motor dispenser sekarang: " + String(motor2Delay) + " ms\n";
  sendToAllowedUsers(msg);

  Serial.println("Delays incremented by " + String(increments) + " * " + String(incrementStep));
}

// ---------------- Existing menu functions for keypad (unchanged) ----------------
void menuStatusPakan() {
  while (true) {
    long jarak = bacaJarak();
    DateTime now = rtc.now();
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Lvl:");
    if (jarak < 10) lcd.print("Penuh");
    else if (jarak < 20) lcd.print("Sedang");
    else if (jarak == 999) lcd.print("Err");
    else lcd.print("Habis");

    lcd.setCursor(9,0);
    lcd.print(formatTime(now.hour(), now.minute()));

    lcd.setCursor(0,1);
    lcd.print("Jrk:");
    lcd.print(jarak);
    lcd.print("cm");

    // non-blocking check for keypad back
    unsigned long start = millis();
    while (millis() - start < 600) {
      char k = keypad.getKey();
      if (k == '*') {
        tampilMenu();
        return;
      }
      delay(10);
    }
  }
}

void menuJadwalPakan() {
  while (true) {
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Pilih Jadwal:");
    lcd.setCursor(0,1);
    lcd.print("A:1  B:2  *:Back");
    unsigned long start = millis();
    while (millis() - start < 1000) {
      char k = keypad.getKey();
      if (k == 'A') { setJadwal(1); return; }
      if (k == 'B') { setJadwal(2); return; }
      if (k == '*') { tampilMenu(); return; }
      delay(10);
    }
  }
}

void setJadwal(int nomor) {
  int jam = (nomor==1)? jadwalJam1 : jadwalJam2;
  int menit = (nomor==1)? jadwalMenit1 : jadwalMenit2;
  String input = "";
  while (true) {
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Set Jadwal ");
    lcd.print(nomor);
    lcd.setCursor(0,1);
    if (jam < 10) lcd.print("0");
    lcd.print(jam);
    lcd.print(":");
    if (menit < 10) lcd.print("0");
    lcd.print(menit);
    lcd.print(" ");

    char key = keypad.getKey();
    if (key && key >= '0' && key <= '9') {
      input += key;
      if (input.length() == 4) {
        int newJam = (input.substring(0,2)).toInt();
        int newMenit = (input.substring(2,4)).toInt();
        if (newJam >=0 && newJam <=23 && newMenit >=0 && newMenit <=59) {
          if (nomor==1) { jadwalJam1=newJam; jadwalMenit1=newMenit; sudahPakan1=false; }
          else { jadwalJam2=newJam; jadwalMenit2=newMenit; sudahPakan2=false; }
          saveJadwalToEEPROM();
          lcd.clear();
          lcd.setCursor(0,0);
          lcd.print("Disimpan:");
          lcd.setCursor(0,1);
          lcd.print(formatTime(newJam, newMenit));
          delay(1000);
          tampilMenu();
          return;
        } else {
          lcd.clear(); lcd.print("Format Salah"); delay(1000); input="";
        }
      }
    }
    if (key == '*') { tampilMenu(); return; }
    if (key == 'D') { // allow D to accept if partially entered
      if (input.length() >= 3) {
        while (input.length() < 4) input += '0';
        int newJam = (input.substring(0,2)).toInt();
        int newMenit = (input.substring(2,4)).toInt();
        if (newJam >=0 && newJam <=23 && newMenit >=0 && newMenit <=59) {
          if (nomor==1) { jadwalJam1=newJam; jadwalMenit1=newMenit; sudahPakan1=false; }
          else { jadwalJam2=newJam; jadwalMenit2=newMenit; sudahPakan2=false; }
          saveJadwalToEEPROM();
          lcd.clear();
          lcd.setCursor(0,0);
          lcd.print("Disimpan:");
          lcd.setCursor(0,1);
          lcd.print(formatTime(newJam, newMenit));
          delay(1000);
          tampilMenu();
          return;
        } else {
          lcd.clear(); lcd.print("Format Salah"); delay(1000); input="";
        }
      } else {
        tampilMenu(); return;
      }
    }
    delay(80);
  }
}

void menuPakanManual() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Pakan Manual");
  lcd.setCursor(0,1);
  lcd.print("D:Mulai  *:Back");
  while (true) {
    char key = keypad.getKey();
    if (key == 'D') {
      beriPakan();
      return;
    } else if (key == '*') {
      tampilMenu();
      return;
    }
    delay(20);
  }
}