#include "thingProperties.h"
#include <Arduino.h>
#include <Wire.h>
#include "SparkFun_SCD30_Arduino_Library.h"

// =====================================================
// CONFIGURACIÓN GENERAL
// =====================================================

// ESP8266 NodeMCU / Wemos D1 mini:
// GPIO4 = D2 = SDA
// GPIO5 = D1 = SCL
#define I2C_SDA 4
#define I2C_SCL 5
#define I2C_FREQ_HZ 50000

#define SCD30_INTERVAL_S 5
#define SENSOR_POLL_MS 1000
#define SENSOR_TIMEOUT_MS 15000
#define CLOUD_UPDATE_MS 5000
#define SENSOR_RETRY_MS 5000

// El gas se diluye 1:50 antes de llegar al SCD30.
#define DILUTION_FACTOR 50.0

// El SCD30 mide aproximadamente hasta 10000 ppm.
// Se usa este umbral para detectar lectura cercana al límite.
#define SCD30_SATURATION_PPM 9800.0

SCD30 airSensor;

bool scd30Ready = false;
bool hasLatestData = false;

uint32_t lastSensorPoll = 0;
uint32_t lastValidRead = 0;
uint32_t lastCloudUpdate = 0;
uint32_t lastSensorRetry = 0;

// =====================================================
// ESTRUCTURA LOCAL DE DATOS
// =====================================================

struct ReactorData {
  float co2SensorPpm;
  float co2CorrectedPpm;
  float co2Percent;
  float ch4Percent;
  float temperatureC;
  float humidityPercent;

  bool sensorOk;
  bool alarmActive;
  int alertLevel;

  char state[60];
};

ReactorData latestData;

// =====================================================
// FUNCIONES AUXILIARES
// =====================================================

float clampFloat(float value, float minValue, float maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

float estimateCO2CorrectedPpm(float co2SensorPpm) {
  return co2SensorPpm * DILUTION_FACTOR;
}

float estimateCO2Percent(float co2SensorPpm) {
  float correctedPpm = estimateCO2CorrectedPpm(co2SensorPpm);

  // 10000 ppm = 1 %
  float percent = correctedPpm / 10000.0;

  return clampFloat(percent, 0.0, 100.0);
}

float estimateCH4Percent(float co2PercentValue) {
  // Aproximación usada en el proyecto:
  // CH4 % ≈ 100 % - CO2 %
  return clampFloat(100.0 - co2PercentValue, 0.0, 100.0);
}

// =====================================================
// EVALUACIÓN DEL ESTADO DEL REACTOR
// =====================================================

void evaluateReactorState(ReactorData &data) {
  data.alertLevel = 0;
  data.alarmActive = false;

  if (!data.sensorOk) {
    strcpy(data.state, "ERROR_SENSOR");
    data.alertLevel = 3;
    data.alarmActive = true;
    return;
  }

  bool tempOptimal = false;
  bool tempWarning = false;
  bool tempCritical = false;

  if (data.temperatureC < 20.0) {
    strcpy(data.state, "PSICROFILO_BAJA_EFICIENCIA");
    tempWarning = true;
  }
  else if (data.temperatureC >= 20.0 && data.temperatureC < 30.0) {
    strcpy(data.state, "BAJA_TEMPERATURA_FUERA_MESOFILICO");
    tempWarning = true;
  }
  else if (data.temperatureC >= 30.0 && data.temperatureC <= 40.0) {
    tempOptimal = true;
  }
  else if (data.temperatureC > 40.0 && data.temperatureC < 50.0) {
    strcpy(data.state, "ALTA_TEMPERATURA_FUERA_MESOFILICO");
    tempWarning = true;
  }
  else if (data.temperatureC >= 50.0 && data.temperatureC <= 60.0) {
    strcpy(data.state, "TERMOFILO_NO_OBJETIVO_DEL_PROYECTO");
    tempWarning = true;
  }
  else if (data.temperatureC > 60.0) {
    strcpy(data.state, "CRITICO_SOBRETEMPERATURA");
    tempCritical = true;
  }

  if (tempCritical) {
    data.alertLevel = 3;
    data.alarmActive = true;
    return;
  }

  bool gasNormal = false;
  bool gasWarning = false;
  bool gasObservation = false;

  if (data.co2Percent >= 30.0 && data.co2Percent <= 50.0 &&
      data.ch4Percent >= 50.0 && data.ch4Percent <= 70.0) {
    gasNormal = true;
  }
  else if (data.co2Percent < 30.0 && data.ch4Percent > 70.0) {
    gasObservation = true;
  }
  else if (data.co2Percent > 50.0 && data.ch4Percent < 50.0) {
    gasWarning = true;
  }

  if (data.co2SensorPpm >= SCD30_SATURATION_PPM) {
    gasObservation = true;
  }

  if (tempOptimal && gasNormal) {
    if (data.co2SensorPpm >= SCD30_SATURATION_PPM) {
      strcpy(data.state, "ESTABLE_CO2_CERCA_LIMITE_SENSOR");
      data.alertLevel = 1;
      data.alarmActive = false;
      return;
    }

    strcpy(data.state, "ESTABLE_MESOFILICO");
    data.alertLevel = 0;
    data.alarmActive = false;
    return;
  }

  if (tempOptimal && gasObservation) {
    strcpy(data.state, "MESOFILICO_GAS_FUERA_RANGO_NORMAL");
    data.alertLevel = 1;
    data.alarmActive = false;
    return;
  }

  if (tempOptimal && gasWarning) {
    strcpy(data.state, "MESOFILICO_BAJO_METANO");
    data.alertLevel = 2;
    data.alarmActive = true;
    return;
  }

  if (tempWarning && gasNormal) {
    data.alertLevel = 2;
    data.alarmActive = true;
    return;
  }

  if (tempWarning && gasObservation) {
    data.alertLevel = 2;
    data.alarmActive = true;
    return;
  }

  if (tempWarning && gasWarning) {
    strcpy(data.state, "ADVERTENCIA_TEMPERATURA_Y_GAS");
    data.alertLevel = 3;
    data.alarmActive = true;
    return;
  }

  strcpy(data.state, "ESTADO_NO_CLASIFICADO");
  data.alertLevel = 1;
  data.alarmActive = false;
}

// =====================================================
// INICIALIZACIÓN DEL SENSOR SCD30
// =====================================================

bool initSCD30() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_FREQ_HZ);

#ifdef ESP8266
  // Ayuda con sensores I2C que pueden requerir clock stretching.
  Wire.setClockStretchLimit(200000);
#endif

  if (!airSensor.begin()) {
    Serial.println("SCD30 no detectado por I2C.");
    scd30Ready = false;
    return false;
  }

  airSensor.setMeasurementInterval(SCD30_INTERVAL_S);

  // Autocalibración activada.
  airSensor.setAutoSelfCalibration(true);

  scd30Ready = true;
  lastValidRead = millis();

  Serial.println("SCD30 inicializado con autocalibracion activada.");
  return true;
}

// =====================================================
// ERROR DEL SENSOR
// =====================================================

void setSensorError() {
  latestData.co2SensorPpm = 0.0;
  latestData.co2CorrectedPpm = 0.0;
  latestData.co2Percent = 0.0;
  latestData.ch4Percent = 0.0;
  latestData.temperatureC = -127.0;
  latestData.humidityPercent = 0.0;
  latestData.sensorOk = false;

  evaluateReactorState(latestData);

  hasLatestData = true;
}

// =====================================================
// LECTURA DEL SENSOR
// =====================================================

void readSCD30IfAvailable() {
  if (!scd30Ready) {
    if (millis() - lastSensorRetry >= SENSOR_RETRY_MS) {
      lastSensorRetry = millis();

      if (!initSCD30()) {
        setSensorError();
      }
    }

    return;
  }

  if (millis() - lastSensorPoll < SENSOR_POLL_MS) {
    return;
  }

  lastSensorPoll = millis();

  if (airSensor.dataAvailable()) {
    latestData.co2SensorPpm = airSensor.getCO2();
    latestData.temperatureC = airSensor.getTemperature();
    latestData.humidityPercent = airSensor.getHumidity();

    latestData.co2CorrectedPpm = estimateCO2CorrectedPpm(latestData.co2SensorPpm);
    latestData.co2Percent = estimateCO2Percent(latestData.co2SensorPpm);
    latestData.ch4Percent = estimateCH4Percent(latestData.co2Percent);

    latestData.sensorOk = true;

    evaluateReactorState(latestData);

    hasLatestData = true;
    lastValidRead = millis();

    Serial.print("CO2 sensor ppm: ");
    Serial.print(latestData.co2SensorPpm);

    Serial.print(" | CO2 corregido ppm: ");
    Serial.print(latestData.co2CorrectedPpm);

    Serial.print(" | CO2 %: ");
    Serial.print(latestData.co2Percent, 2);

    Serial.print(" | CH4 %: ");
    Serial.print(latestData.ch4Percent, 2);

    Serial.print(" | Temp C: ");
    Serial.print(latestData.temperatureC, 2);

    Serial.print(" | HR %: ");
    Serial.print(latestData.humidityPercent, 2);

    Serial.print(" | Estado: ");
    Serial.print(latestData.state);

    Serial.print(" | Alerta: ");
    Serial.println(latestData.alertLevel);
  }

  if (millis() - lastValidRead > SENSOR_TIMEOUT_MS) {
    Serial.println("Timeout del SCD30. Se intentara reinicializar.");
    scd30Ready = false;
    setSensorError();
  }
}

// =====================================================
// ACTUALIZACIÓN DE VARIABLES DE ARDUINO CLOUD
// =====================================================

void updateCloudVariables() {
  if (!hasLatestData) {
    return;
  }

  if (millis() - lastCloudUpdate < CLOUD_UPDATE_MS) {
    return;
  }

  lastCloudUpdate = millis();

  co2SensorPpm = latestData.co2SensorPpm;
  co2CorrectedPpm = latestData.co2CorrectedPpm;
  co2Percent = latestData.co2Percent;
  ch4Percent = latestData.ch4Percent;

  temperatureC = latestData.temperatureC;
  humidityPercent = latestData.humidityPercent;

  reactorState = String(latestData.state);
  alertLevel = latestData.alertLevel;
  sensorOk = latestData.sensorOk;
  alarmActive = latestData.alarmActive;
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  initProperties();

  ArduinoCloud.begin(ArduinoIoTPreferredConnection);

  setDebugMessageLevel(2);
  ArduinoCloud.printDebugInfo();

  initSCD30();
}

// =====================================================
// LOOP PRINCIPAL
// =====================================================

void loop() {
  ArduinoCloud.update();

  readSCD30IfAvailable();
  updateCloudVariables();

  delay(10);
}