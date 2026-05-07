// ===================== KUTUPHANE VE PIN TANIMLARI =====================
#include <Arduino_FreeRTOS.h>   // FreeRTOS gorev yoneticisi
#include <semphr.h>             // Mutex / semafor API
#include <Wire.h>               // I2C haberlesme kutuphanesi
#include <Adafruit_GFX.h>       // OLED grafik katmani
#include <Adafruit_SSD1306.h>   // SSD1306 OLED surucu
#include <Adafruit_MPU6050.h>   // MPU6050 IMU / sicaklik sensoru
#include <Adafruit_Sensor.h>    // Adafruit unified sensor katmani
#include <Keypad.h>             // Membran keypad kutuphanesi
#include <Servo.h>              // Servo motor kutuphanesi

#define SCREEN_WIDTH  128       // OLED genisligi (piksel)
#define SCREEN_HEIGHT  64       // OLED yuksekligi (piksel)
#define OLED_RESET     -1       // Reset pini kullanilmiyor
#define OLED_ADDRESS  0x3C      // SSD1306 I2C adresi

#define TRIG_PIN  11            // HC-SR04 tetikleme pini
#define ECHO_PIN  10            // HC-SR04 yanki pini
#define SERVO_PIN 12            // Servo PWM pini

// ===================== KEYPAD TANIMLARI =====================
const byte ROWS = 4;            // Keypad satir sayisi
const byte COLS = 4;            // Keypad sutun sayisi

char keys[ROWS][COLS] = {       // Keypad tus haritasi
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {30, 31, 32, 33};  // Satir pinleri
byte colPins[COLS] = {34, 35, 36, 37};  // Sutun pinleri

// ===================== KISISEL PARAMETRELER =====================
// Ogrenci No son 4 hane: 1053
const char RESET_PASSWORD[]  = "1053";  // Sistemi acma sifresi

// Ogrenci No son 2 hane: 53 => 53 + 15 = 68 cm
const int  DISTANCE_THRESHOLD = 68;     // Guvenlik mesafe esigi (cm)

// Ogrenci No son hane: 3 => 3 + 50 = 53 derece
const float TEMP_LIMIT        = 53.0;   // Sicaklik siniri (C)

// ===================== NESNE OLUSTURMA =====================
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET); // OLED nesnesi
Adafruit_MPU6050 mpu;                                                   // MPU6050 nesnesi
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS); // Keypad nesnesi
Servo  servo;                                                           // Servo nesnesi

// ===================== GLOBAL DEGISKENLER =====================
volatile bool  isLocked    = false;     // Guvenlik kilidi bayragi (HC-SR04 tetikler)
volatile bool  isEmergency = false;     // Ariza modu bayragi (MPU6050 tetikler)

volatile float currentDistance = 999.0; // Son okunan mesafe (cm)
volatile float currentTemp     = 25.0;  // Son okunan sicaklik (C)

char passwordBuffer[10];                // Girilen sifre tamponu
int  passwordIndex = 0;                 // Buffer yazma indeksi

int  servoAngle = 0;                    // Mevcut servo acisi (derece)
int  servoDir   = 1;                    // Yon: +1 ileri, -1 geri

SemaphoreHandle_t i2cMutex;             // I2C hat paylasimi icin mutex

// ===================== YARDIMCI FONKSIYONLAR =====================
float readDistance() {
  digitalWrite(TRIG_PIN, LOW);           // Tetikleme pini sifirla
  delayMicroseconds(2);                  // 2 us bekle
  digitalWrite(TRIG_PIN, HIGH);          // 10 us yuksek darbe gonder
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);           // Darbe sona erdirildi

  long duration = pulseIn(ECHO_PIN, HIGH, 30000UL); // Yanki suresi okunuyor (max ~5m)
  if (duration == 0) return 999.0;       // Zaman asimi: nesne yok

  return (duration * 0.034) / 2.0;      // ses hizi ile cm hesabi (gidis-donus /2)
}

void clearPasswordBuffer() {
  memset(passwordBuffer, 0, sizeof(passwordBuffer)); // Tamponu sifirla
  passwordIndex = 0;                                 // Indeksi sifirla
}

// ===================== TASK_SAFETY (Oncelik: 4 - Kritik) =====================
void Task_Safety(void *pvParameters) {
  (void)pvParameters;                    // Kullanilmayan parametre uyarisi engellenir

  for (;;) {
    float dist = readDistance();         // HC-SR04 ile mesafe oku
    currentDistance = dist;              // Global degiskeni guncelle

    if (dist < DISTANCE_THRESHOLD) {    // Esik alti: guvenlik ihlali
      isLocked = true;                  // Kilidi aktif et (sadece sifre ile kapanir)
      Serial.print("[SAFETY] IHLAL! Mesafe: ");
      Serial.print(dist);
      Serial.println(" cm");
    } else {
      Serial.print("[SAFETY] OK - Mesafe: ");
      Serial.print(dist);
      Serial.println(" cm");
    }

    vTaskDelay(pdMS_TO_TICKS(50));       // 50ms bekle
  }
}

// ===================== TASK_HEALTH (Oncelik: 3 - Yuksek) =====================
void Task_Health(void *pvParameters) {
  (void)pvParameters;

  for (;;) {
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) { // I2C mutex al
      sensors_event_t accel, gyro, temp;      // Sensor olay yapilari
      mpu.getEvent(&accel, &gyro, &temp);     // MPU6050'den tum veri oku
      float t = temp.temperature;             // Sicaklik degerini al
      xSemaphoreGive(i2cMutex);               // I2C mutex'i birak

      currentTemp = t;                        // Global degiskeni guncelle

      if (t >= TEMP_LIMIT) {                 // Limit asildi: ariza moduna gec
        isEmergency = true;
        Serial.print("[HEALTH] ARIZA! Sicaklik: ");
        Serial.print(t);
        Serial.println(" C");
      } else {
        // isEmergency burada false YAPILMIYOR
        // Ariza modu: sicaklik dusuk + dogru sifre birlikte gerekli (Task_HMI yonetir)
        Serial.print("[HEALTH] OK - Sicaklik: ");
        Serial.print(t);
        Serial.println(" C");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));           // 200ms bekle
  }
}

// ===================== TASK_MOTION (Oncelik: 2 - Orta) =====================
void Task_Motion(void *pvParameters) {
  (void)pvParameters;

  for (;;) {
    if (!isLocked && !isEmergency) {          // Her iki bayrak da pasifse hareket et
      servo.write(servoAngle);                // Servo mevcut aciya git

      servoAngle += (5 * servoDir);           // 5 derecelik adimla ilerle

      if (servoAngle >= 180) {               // Maksimum aciya ulasinca geri don
        servoAngle = 180;
        servoDir = -1;
      } else if (servoAngle <= 0) {          // Minimum aciya ulasinca ileri git
        servoAngle = 0;
        servoDir = 1;
      }
    } else {
      servo.write(servoAngle);               // Kilitli modda servo mevcut acida tutulur (titreme engellenir)
    }

    vTaskDelay(pdMS_TO_TICKS(30));           // 30ms bekle (yumusak gecis)
  }
}

// ===================== TASK_HMI (Oncelik: 1 - Dusuk) =====================
void Task_HMI(void *pvParameters) {
  (void)pvParameters;

  static bool          blinkState     = false;
  static unsigned long blinkTimer     = 0;
  static bool          showWrongPass  = false;
  static unsigned long wrongPassTimer = 0;

  for (;;) {
    char key = keypad.getKey();

    if (key) {

      // Normal modda keypad girisi sadece algilanir, sifre kontrolu yapilmaz
      if (!isLocked && !isEmergency) {
        clearPasswordBuffer();
        Serial.print("[HMI] Normal modda keypad girisi algilandi: ");
        Serial.println(key);
      }

      // Kilit veya ariza modunda sifre kontrolu yapilir
      else {
        if (key == '*') {
          clearPasswordBuffer();
          Serial.println("[HMI] Buffer temizlendi (*)");

        } else if (key == '#') {
          passwordBuffer[passwordIndex] = '\0';

          if (strcmp(passwordBuffer, RESET_PASSWORD) == 0) {

            if (currentTemp < TEMP_LIMIT) {
              isLocked = false;
              isEmergency = false;
              Serial.println("[HMI] Sifre DOGRU + sicaklik normal - sistem acildi.");
            } else {
              Serial.println("[HMI] Sifre DOGRU ama sicaklik hala yuksek - sistem acilmadi.");
            }

            clearPasswordBuffer();

          } else {
            showWrongPass  = true;
            wrongPassTimer = millis();
            clearPasswordBuffer();
            Serial.println("[HMI] Sifre YANLIS!");
          }

        } else if (passwordIndex < 9) {
          passwordBuffer[passwordIndex++] = key;
          passwordBuffer[passwordIndex] = '\0';

          Serial.print("[HMI] Buffer: ");
          Serial.println(passwordBuffer);
        }
      }
    }

    if (showWrongPass && (millis() - wrongPassTimer >= 2000)) {
      showWrongPass = false;
    }

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      oled.clearDisplay();
      oled.setTextSize(1);
      oled.setTextColor(SSD1306_WHITE);

      if (showWrongPass) {
        oled.setCursor(20, 20);
        oled.println("HATALI SIFRE!");
        oled.setCursor(10, 38);
        oled.println("Tekrar Deneyiniz...");

      } else if (isEmergency) {
        oled.setCursor(0, 0);
        oled.println("!!! ARIZA: MOTOR SICAK !!!");
        oled.setCursor(0, 22);
        oled.println("Soguma Bekleniyor...");
        oled.setCursor(0, 44);
        oled.print("Isi: ");
        oled.print(currentTemp, 1);
        oled.println(" C");

      } else if (isLocked) {
        if (millis() - blinkTimer >= 500) {
          blinkState = !blinkState;
          blinkTimer = millis();
        }

        if (blinkState) {
          oled.setCursor(0, 0);
          oled.println("!!! GUVENLIK IHLAL !!!");
        }

        oled.setCursor(0, 22);
        oled.println("Sifre Giriniz:");
        oled.setCursor(0, 44);
        oled.print("> ");
        oled.println(passwordBuffer);

      } else {
        oled.setCursor(0, 0);
        oled.println("DURUM: AKTIF");
        oled.setCursor(0, 22);
        oled.print("Mesafe: ");
        oled.print((currentDistance >= 999.0) ? 0 : (int)currentDistance);
        oled.println("cm");
        oled.setCursor(0, 44);
        oled.print("Isi: ");
        oled.print(currentTemp, 1);
        oled.println(" C");
      }

      oled.display();
      xSemaphoreGive(i2cMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
// ayame's code
// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);              // Seri monitor 115200 baud baslatildi
  Wire.begin();                      // I2C baslatildi

  pinMode(TRIG_PIN, OUTPUT);         // HC-SR04 tetikleme pini cikis
  pinMode(ECHO_PIN, INPUT);          // HC-SR04 yanki pini giris

  servo.attach(SERVO_PIN);          // Servo pini baglandi
  servo.write(0);                   // Baslangic konumu: 0 derece

  if (!mpu.begin()) {               // MPU6050 baslatildi
    Serial.println("MPU6050 bulunamadi!");
    while (1);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);  // Ivmemetre araligi: +/-8G
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);        // Jiroskop araligi: 500 deg/s
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);     // Dijital filtre bant genisligi

  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) { // OLED baslatildi
    Serial.println("OLED bulunamadi!");
    while (1);
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(20, 25);
  oled.println("Sistem Basliyor..."); // Baslangic mesaji gosterildi
  oled.display();

  i2cMutex = xSemaphoreCreateMutex(); // I2C mutex olusturuldu
  if (i2cMutex == NULL) {
    Serial.println("Mutex olusturulamadi!");
    while (1);
  }

  clearPasswordBuffer();             // Sifre tamponu temizlendi

  Serial.println("=== Sistem Parametreleri ===");
  Serial.print("Sifre            : "); Serial.println(RESET_PASSWORD);
  Serial.print("Mesafe Esigi (cm): "); Serial.println(DISTANCE_THRESHOLD);
  Serial.print("Sicaklik Siniri  : "); Serial.println(TEMP_LIMIT);
  Serial.println("============================");

  // ===================== FREERTOS GOREV OLUSTURMA =====================
  // xTaskCreate(fonksiyon, isim, yigin_bayt, parametre, oncelik, handle)
  xTaskCreate(Task_Safety, "Safety", 256, NULL, 4, NULL); // Oncelik 4: kritik
  xTaskCreate(Task_Health, "Health", 512, NULL, 3, NULL); // Oncelik 3: yuksek
  xTaskCreate(Task_Motion, "Motion", 256, NULL, 2, NULL); // Oncelik 2: orta
  xTaskCreate(Task_HMI,    "HMI",    512, NULL, 1, NULL); // Oncelik 1: dusuk

  // Arduino FreeRTOS kutuphanesi vTaskStartScheduler()'i otomatik cagiriyor
}

// ===================== ANA DONGU =====================
void loop() {
  // FreeRTOS aktif: tum isler task'lar tarafindan yurutulur, loop bos birakilir
}