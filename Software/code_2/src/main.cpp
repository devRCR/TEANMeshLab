#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <esp_bt.h>

// Configuración de la pantalla OLED
#define I2C_ADDRESS 0x3C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// Configuración del sensor y muestras
#define SHT31_ADDRESS 0x44
#define UPDATE_INTERVAL 2000
#define NUM_SAMPLES 2

// Compensación de temperatura
#define TEMP_OFFSET 0.0  // Ajuste según la diferencia observada

// Objetos de pantalla y sensor
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_SHT31 sht31 = Adafruit_SHT31();

// Variables globales
unsigned long lastUpdateTime = 0;
float tempReadings[NUM_SAMPLES] = {0};
float humReadings[NUM_SAMPLES] = {0};
int sampleIndex = 0;
int validSamples = 0;

// Configuración del módulo XBee
uint8_t destAddress[8] = {0x00, 0x13, 0xA2, 0x00, 0x41, 0x4F, 0xED, 0xDD};
HardwareSerial xbeeSerial(2);  // UART2 para XBee

// Funciones del XBee
void sendXBeeData(String data) {
  uint8_t dataLength = data.length();
  uint8_t frameLength = dataLength + 15;
  uint8_t frameDataLength = frameLength - 4;
  uint8_t frame[frameLength];

  frame[0] = 0x7E;  // Byte de inicio de la trama API
  frame[1] = (frameDataLength >> 8) & 0xFF;  // Longitud alta del frame data
  frame[2] = frameDataLength & 0xFF;  // Longitud baja del frame data
  frame[3] = 0x00;  // Tipo de trama (0x00 para trama de transmisión)
  frame[4] = 0x01;  // Frame ID (puede ser cualquier valor)
  memcpy(&frame[5], destAddress, 8);  // Dirección de destino
  frame[13] = 0x00;  // Opciones de transmisión (0x00 para transmisión estándar)

  // Copiar los datos a enviar en la trama
  data.getBytes(&frame[14], dataLength + 1);

  // Calcular el checksum
  uint8_t checksum = 0;
  for (int i = 3; i < frameLength - 1; i++) {
    checksum += frame[i];
  }
  frame[frameLength - 1] = 0xFF - (checksum & 0xFF);  // Añadir el checksum al final de la trama

  // Enviar la trama al XBee
  xbeeSerial.write(frame, frameLength);
}

void setup() {
  Serial.begin(115200);

  // Deshabilitar WiFi y Bluetooth
  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true);
  btStop();

  // Inicializar pantalla OLED
  if (!display.begin(I2C_ADDRESS, true)) {
    Serial.println(F("Fallo en la pantalla OLED"));
    while (true);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println(F("Iniciando..."));
  display.display();
  delay(2000);

  // Inicializar sensor SHT31
  if (!sht31.begin(SHT31_ADDRESS)) {
    Serial.println(F("Error iniciando SHT31"));
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("Error SHT31"));
    display.display();
    while (true);
  }

  display.clearDisplay();

  // Inicializar XBee (UART2)
  xbeeSerial.begin(9600);  // Configuración del puerto UART2
}

void drawSignalBars(int level) {
  for (int i = 0; i < 5; i++) {
    int height = (i * 2) + 2;
    int x = 100 + (i * 4);
    int y = 10 - (i * 2);
    if (i < level) {
      display.fillRect(x, y, 3, height, SH110X_WHITE);
    } else {
      display.drawRect(x, y, 3, height, SH110X_WHITE);
    }
  }
}

float calculateAverage(float *readings, int numReadings) {
  float sum = 0;
  for (int i = 0; i < numReadings; i++) {
    sum += readings[i];
  }
  return sum / numReadings;
}

void displayData(float avgTemp, float avgHum, int signalLevel) {
  display.clearDisplay();

  // Mostrar texto principal
  display.setTextSize(2);
  display.setCursor(35, 0);
  display.print(F("TEAN"));

  // Dibujar barras de señal
  drawSignalBars(signalLevel);

  // Dibujar línea separadora
  display.drawLine(0, 16, SCREEN_WIDTH, 16, SH110X_WHITE);

  // Mostrar temperatura promedio
  display.setCursor(0, 25);
  display.print(F("T: "));
  display.print(avgTemp);
  display.print(F(" C"));

  // Mostrar humedad promedio
  display.setCursor(0, 45);
  display.print(F("H: "));
  display.print(avgHum);
  display.print(F(" %"));

  // Actualizar pantalla
  display.display();
}

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastUpdateTime >= UPDATE_INTERVAL) {
    lastUpdateTime = currentMillis;

    // Leer datos del sensor con corrección de temperatura
    float temperature = sht31.readTemperature() + TEMP_OFFSET;
    float humidity = sht31.readHumidity();

    if (isnan(temperature) || isnan(humidity)) {
      Serial.println(F("Error leyendo datos del sensor"));
      return;
    }

    // Actualizar lecturas
    tempReadings[sampleIndex] = temperature;
    humReadings[sampleIndex] = humidity;
    sampleIndex = (sampleIndex + 1) % NUM_SAMPLES;

    if (validSamples < NUM_SAMPLES) validSamples++;

    // Calcular y mostrar promedios si hay suficientes datos
    if (validSamples == NUM_SAMPLES) {
      float avgTemp = calculateAverage(tempReadings, NUM_SAMPLES);
      float avgHum = calculateAverage(humReadings, NUM_SAMPLES);

      // Cambiar nivel de señal cada 2 segundos
      int signalLevel = (currentMillis / UPDATE_INTERVAL) % 5;

      // Mostrar datos en pantalla
      displayData(avgTemp, avgHum, signalLevel);

      // Enviar datos de temperatura y humedad al XBee
      String dataToSend = "T:" + String(avgTemp) + " H:" + String(avgHum);
      sendXBeeData(dataToSend);
    }
  }
}
