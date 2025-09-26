// ============================= INCLUDES =============================
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <esp_pm.h>
#include <esp_sleep.h>

// ============================= CONFIGURACIÓN =============================
// Hardware
#define I2C_ADDRESS 0x3C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SHT31_ADDRESS 0x44
#define XBEE_UART 2
#define I2C_CLOCK 100000

// Muestreo
#define NUM_SAMPLES 10
#define TEMP_OFFSET 0.0
#define SAMPLE_BURST_INTERVAL_MS 100    // 100ms entre muestras durante el burst
#define TRANSMISSION_INTERVAL_MS 5000   // Cada 5 segundos tomar las 10 muestras
#define DISPLAY_UPDATE_INTERVAL_MS 5000 // Actualizar display cada 5 segundos
#define SENSOR_TIMEOUT_MS 100

// Power Management
#define LIGHT_SLEEP_DURATION_US 500000
#define CPU_FREQ_MHZ 80
#define CPU_MIN_FREQ_MHZ 10

// Debug condicional
#define DEBUG_ENABLED 0
#if DEBUG_ENABLED
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(fmt, ...)
#endif

// ============================= ESTRUCTURAS DE DATOS =============================
struct SensorReading {
  float temperature;
  float humidity;
  bool isValid;
  
  SensorReading() : temperature(0.0), humidity(0.0), isValid(false) {}
  SensorReading(float temp, float hum) : temperature(temp), humidity(hum), isValid(true) {}
};

class CircularBuffer {
private:
  float tempBuffer[NUM_SAMPLES];
  float humBuffer[NUM_SAMPLES];
  uint8_t count;

public:
  CircularBuffer() : count(0) {
    memset(tempBuffer, 0, sizeof(tempBuffer));
    memset(humBuffer, 0, sizeof(humBuffer));
  }

  void addReading(const SensorReading& reading) {
    if (count < NUM_SAMPLES) {
      tempBuffer[count] = reading.temperature;
      humBuffer[count] = reading.humidity;
      count++;
    }
  }

  bool isFull() const { return count == NUM_SAMPLES; }
  uint8_t getCount() const { return count; }
  
  void clear() { count = 0; }

  SensorReading getAverage() const {
    if (count == 0) return SensorReading();
    
    float tempSum = 0, humSum = 0;
    for (uint8_t i = 0; i < count; i++) {
      tempSum += tempBuffer[i];
      humSum += humBuffer[i];
    }
    
    return SensorReading(tempSum / count, humSum / count);
  }
};

struct SystemTimers {
  unsigned long lastTransmission;
  uint16_t cycleCount;
  
  SystemTimers() : lastTransmission(0), cycleCount(0) {}
  
  void init() {
    lastTransmission = millis();
  }
};

// ============================= OBJETOS GLOBALES =============================
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_SHT31 sht31;
HardwareSerial xbeeSerial(XBEE_UART);

const uint8_t xbeeDestAddress[8] = {0x00, 0x13, 0xA2, 0x00, 0x41, 0x4F, 0xED, 0xDD};
uint8_t xbeeFrameBuffer[64];

CircularBuffer sensorBuffer;
SystemTimers timers;

// ============================= FUNCIONES AUXILIARES =============================
void initializePowerManagement() {
  Serial.println("Configurando power management...");
  
  // CPU frequency
  setCpuFrequencyMhz(CPU_FREQ_MHZ);
  Serial.printf("CPU configurado a %dMHz\n", CPU_FREQ_MHZ);
  
  // Disable radios
  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true);
  btStop();
  esp_bt_controller_disable();
  esp_bt_mem_release(ESP_BT_MODE_BTDM);
  Serial.println("WiFi y Bluetooth deshabilitados");
  
  // Power management config
  esp_pm_config_esp32_t pm_config = {
    .max_freq_mhz = CPU_FREQ_MHZ,
    .min_freq_mhz = CPU_MIN_FREQ_MHZ,
    .light_sleep_enable = true
  };
  
  esp_err_t result = esp_pm_configure(&pm_config);
  Serial.printf("Power Management: %s\n", result == ESP_OK ? "OK" : "ERROR");
  
  // I2C optimization
  Wire.setClock(I2C_CLOCK);
  Serial.printf("I2C configurado a %dHz\n", I2C_CLOCK);
}

bool readSensorData(SensorReading& reading) {
  unsigned long startTime = millis();
  
  float temp, hum;
  if (!sht31.readBoth(&temp, &hum)) {
    return false;
  }
  
  if (millis() - startTime > SENSOR_TIMEOUT_MS) {
    DEBUG_PRINTLN("Sensor timeout");
    return false;
  }
  
  // Apply offset and validate ranges
  temp += TEMP_OFFSET;
  if (temp < -40 || temp > 85 || hum < 0 || hum > 100) {
    DEBUG_PRINTLN("Valores fuera de rango");
    return false;
  }
  
  reading = SensorReading(temp, hum);
  return true;
}

void drawSignalBars(uint8_t level) {
  for (uint8_t i = 0; i < 5; i++) {
    uint8_t height = (i * 2) + 2;
    uint8_t x = 100 + (i * 4);
    uint8_t y = 10 - (i * 2);
    
    if (i < level) {
      display.fillRect(x, y, 3, height, SH110X_WHITE);
    } else {
      display.drawRect(x, y, 3, height, SH110X_WHITE);
    }
  }
}

void updateDisplay(const SensorReading& data, uint8_t signalLevel) {
  display.clearDisplay();
  
  // Header
  display.setTextSize(2);
  display.setCursor(35, 0);
  display.print(F("TEAN"));
  
  drawSignalBars(signalLevel);
  display.drawLine(0, 16, SCREEN_WIDTH, 16, SH110X_WHITE);
  
  // Temperature
  display.setCursor(0, 25);
  display.print(F("T: "));
  display.print(data.temperature, 1);
  display.print(F(" C"));
  
  // Humidity
  display.setCursor(0, 45);
  display.print(F("H: "));
  display.print(data.humidity, 1);
  display.print(F(" %"));
  
  display.display();
}

void sendXBeeData(const SensorReading& data) {
  char dataStr[24];
  snprintf(dataStr, sizeof(dataStr), "T:%.1f H:%.1f", data.temperature, data.humidity);
  
  const uint8_t dataLen = strlen(dataStr);
  const uint8_t frameLen = dataLen + 15;
  const uint8_t frameDataLen = frameLen - 4;
  
  if (frameLen > sizeof(xbeeFrameBuffer)) {
    Serial.println("Error: Datos muy largos para XBee");
    return;
  }
  
  // Build XBee API frame
  xbeeFrameBuffer[0] = 0x7E;
  xbeeFrameBuffer[1] = (frameDataLen >> 8) & 0xFF;
  xbeeFrameBuffer[2] = frameDataLen & 0xFF;
  xbeeFrameBuffer[3] = 0x00;
  xbeeFrameBuffer[4] = 0x01;
  memcpy(&xbeeFrameBuffer[5], xbeeDestAddress, 8);
  xbeeFrameBuffer[13] = 0x00;
  memcpy(&xbeeFrameBuffer[14], dataStr, dataLen);
  
  // Calculate checksum
  uint8_t checksum = 0;
  for (uint8_t i = 3; i < frameLen - 1; i++) {
    checksum += xbeeFrameBuffer[i];
  }
  xbeeFrameBuffer[frameLen - 1] = 0xFF - (checksum & 0xFF);
  
  xbeeSerial.write(xbeeFrameBuffer, frameLen);
  xbeeSerial.flush();
  
  DEBUG_PRINTF("XBee enviado: %s\n", dataStr);
  Serial.printf("Transmisión: %s\n", dataStr);
}

void enterLightSleep() {
  DEBUG_PRINTF("Light Sleep: %lums\n", LIGHT_SLEEP_DURATION_US / 1000);
  
  Serial.flush();
  esp_sleep_enable_timer_wakeup(LIGHT_SLEEP_DURATION_US);
  esp_light_sleep_start();
  
  DEBUG_PRINTLN("Despertar de Light Sleep");
}

// ============================= SETUP =============================
void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("===================");
  Serial.println("TEAN - Sistema Optimizado");
  Serial.println("Iniciando...");
  
  // Initialize power management
  initializePowerManagement();
  
  // Initialize display
  Serial.println("Inicializando display...");
  if (!display.begin(I2C_ADDRESS, true)) {
    Serial.println("Error: Display OLED falló");
    delay(2000);
  } else {
    Serial.println("Display OLED: OK");
    display.setContrast(255);
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0);
    display.println(F("TEAN - Optimizado"));
    display.println(F("Iniciando..."));
    display.display();
    delay(1000);
  }
  
  // Initialize sensor
  Serial.println("Inicializando sensor...");
  if (!sht31.begin(SHT31_ADDRESS)) {
    Serial.println("Error: Sensor SHT31 falló");
    delay(2000);
  } else {
    Serial.println("Sensor SHT31: OK");
  }
  
  // Initialize XBee
  Serial.println("Inicializando XBee...");
  xbeeSerial.begin(9600);
  delay(250);
  Serial.println("XBee: OK");
  
  timers.init();
  Serial.println("Sistema listo!");
  Serial.flush();
}

// ============================= LOOP PRINCIPAL =============================
void loop() {
  unsigned long now = millis();
  
  // Realizar muestreo cada 5 segundos
  if (now - timers.lastTransmission >= TRANSMISSION_INTERVAL_MS) {
    timers.lastTransmission = now;
    timers.cycleCount++;
    
    Serial.printf("Iniciando ciclo %d - Tomando %d muestras...\n", timers.cycleCount, NUM_SAMPLES);
    
    // Limpiar buffer y tomar 10 muestras consecutivas
    sensorBuffer.clear();
    
    for (uint8_t sample = 0; sample < NUM_SAMPLES; sample++) {
      SensorReading reading;
      if (readSensorData(reading)) {
        sensorBuffer.addReading(reading);
        DEBUG_PRINTF("Muestra %d/%d - T:%.1f H:%.1f\n", 
                     sample + 1, NUM_SAMPLES,
                     reading.temperature, reading.humidity);
      } else {
        DEBUG_PRINTF("Error en muestra %d\n", sample + 1);
      }
      
      // Pausa corta entre muestras
      if (sample < NUM_SAMPLES - 1) {
        delay(SAMPLE_BURST_INTERVAL_MS);
      }
    }
    
    // Calcular promedio y transmitir
    if (sensorBuffer.getCount() > 0) {
      SensorReading average = sensorBuffer.getAverage();
      
      // Enviar por XBee
      sendXBeeData(average);
      
      // Actualizar display
      uint8_t signalLevel = (timers.cycleCount % 5) + 1;
      updateDisplay(average, signalLevel);
      
      Serial.printf("Ciclo %d completado - Promedio de %d muestras: T:%.1f°C H:%.1f%%\n", 
                    timers.cycleCount, sensorBuffer.getCount(), 
                    average.temperature, average.humidity);
    } else {
      Serial.println("Error: No se pudieron obtener muestras válidas");
      
      // Mostrar error en display
      display.setTextSize(1);
      display.setCursor(0, 55);
      display.print(F("ERR:SENSOR"));
      display.display();
    }
  }
  
  // Light sleep entre ciclos de muestreo
  delay(50);
  enterLightSleep();
  delay(100);
}