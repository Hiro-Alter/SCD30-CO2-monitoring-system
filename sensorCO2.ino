#include "arduino_secrets.h"
#include "thingProperties.h"
#include <Arduino.h>
#include <Wire.h>
#include "SparkFun_SCD30_Arduino_Library.h"

// =====================================================
// CONFIGURACIÓN
// =====================================================

#define I2C_SDA           4       // D2 en NodeMCU v3
#define I2C_SCL           5       // D1 en NodeMCU v3
#define I2C_FREQ_HZ       50000   // 10k con pull-ups externos de 10kΩ

#define SCD30_INTERVAL_S  5
#define SENSOR_POLL_MS    2000
#define SENSOR_TIMEOUT_MS 30000
#define CLOUD_UPDATE_MS   5000

#define DILUTION_FACTOR       50.0
#define SCD30_SATURATION_PPM  9800.0

SCD30 airSensor;
bool     scd30Ready    = false;
uint32_t lastValidRead = 0;

// =====================================================
// ESTRUCTURA LOCAL (cálculos internos)
// =====================================================

struct ReactorData {
  float co2SensorPpm;
  float co2CorrectedPpm;
  float co2Percent;
  float ch4Percent;
  float temperatureC;
  float humidityPercent;

  bool  sensorOk;
  bool  alarmActive;
  int   alertLevel;
  char  state[60];
};

ReactorData latestData;

// =====================================================
// FUNCIONES AUXILIARES
// =====================================================

float clampFloat(float value, float minVal, float maxVal) {
  if (value < minVal) return minVal;
  if (value > maxVal) return maxVal;
  return value;
}

float estimateCO2CorrectedPpm(float ppm) {
  return ppm * DILUTION_FACTOR;
}

float estimateCO2Percent(float ppm) {
  return clampFloat((ppm * DILUTION_FACTOR) / 10000.0, 0.0, 100.0);
}

float estimateCH4Percent(float co2Pct) {
  return clampFloat(100.0 - co2Pct, 0.0, 100.0);
}

// =====================================================
// EVALUACIÓN DEL ESTADO DEL REACTOR
// =====================================================

void evaluateReactorState(ReactorData &data) {
  data.alertLevel  = 0;
  data.alarmActive = false;

  if (!data.sensorOk) {
    strcpy(data.state, "ERROR_SENSOR");
    data.alertLevel  = 3;
    data.alarmActive = true;
    return;
  }

  bool tempOptimal  = false;
  bool tempWarning  = false;
  bool tempCritical = false;

  if (data.temperatureC < 20.0) {
    strcpy(data.state, "PSICROFILO_BAJA_EFICIENCIA");
    tempWarning = true;
  } else if (data.temperatureC < 30.0) {
    strcpy(data.state, "BAJA_TEMPERATURA_FUERA_MESOFILICO");
    tempWarning = true;
  } else if (data.temperatureC <= 40.0) {
    tempOptimal = true;
  } else if (data.temperatureC < 50.0) {
    strcpy(data.state, "ALTA_TEMPERATURA_FUERA_MESOFILICO");
    tempWarning = true;
  } else if (data.temperatureC <= 60.0) {
    strcpy(data.state, "TERMOFILO_NO_OBJETIVO_DEL_PROYECTO");
    tempWarning = true;
  } else {
    strcpy(data.state, "CRITICO_SOBRETEMPERATURA");
    tempCritical = true;
  }

  if (tempCritical) {
    data.alertLevel  = 3;
    data.alarmActive = true;
    return;
  }

  bool gasNormal      = false;
  bool gasWarning     = false;
  bool gasObservation = false;

  if (data.co2Percent >= 30.0 && data.co2Percent <= 50.0 &&
      data.ch4Percent >= 50.0 && data.ch4Percent <= 70.0) {
    gasNormal = true;
  } else if (data.co2Percent < 30.0 && data.ch4Percent > 70.0) {
    gasObservation = true;
  } else if (data.co2Percent > 50.0 && data.ch4Percent < 50.0) {
    gasWarning = true;
  }

  if (data.co2SensorPpm >= SCD30_SATURATION_PPM) gasObservation = true;

  if (tempOptimal && gasNormal) {
    strcpy(data.state, data.co2SensorPpm >= SCD30_SATURATION_PPM
      ? "ESTABLE_CO2_CERCA_LIMITE_SENSOR"
      : "ESTABLE_MESOFILICO");
    data.alertLevel  = data.co2SensorPpm >= SCD30_SATURATION_PPM ? 1 : 0;
    data.alarmActive = false;
    return;
  }
  if (tempOptimal && gasObservation) {
    strcpy(data.state, "MESOFILICO_GAS_FUERA_RANGO_NORMAL");
    data.alertLevel = 1; data.alarmActive = false; return;
  }
  if (tempOptimal && gasWarning) {
    strcpy(data.state, "MESOFILICO_BAJO_METANO");
    data.alertLevel = 2; data.alarmActive = true; return;
  }
  if (tempWarning && gasNormal) {
    data.alertLevel = 2; data.alarmActive = true; return;
  }
  if (tempWarning && gasObservation) {
    data.alertLevel = 2; data.alarmActive = true; return;
  }
  if (tempWarning && gasWarning) {
    strcpy(data.state, "ADVERTENCIA_TEMPERATURA_Y_GAS");
    data.alertLevel = 3; data.alarmActive = true; return;
  }

  strcpy(data.state, "ESTADO_NO_CLASIFICADO");
  data.alertLevel = 1; data.alarmActive = false;
}

// =====================================================
// INICIALIZACIÓN SCD30
// =====================================================

bool initSCD30() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_FREQ_HZ);
  delay(100);

  if (!airSensor.begin()) {
    Serial.println("SCD30 no detectado por I2C.");
    return false;
  }

  airSensor.setMeasurementInterval(SCD30_INTERVAL_S);
  airSensor.setAutoSelfCalibration(true);
  Serial.println("SCD30 inicializado con autocalibracion activada.");
  return true;
}

// =====================================================
// ACTUALIZACIÓN CLOUD (5 variables)
// =====================================================

void updateCloudVariables(const ReactorData &data) {
  co2SensorPpm    = data.co2SensorPpm;
  temperatureC    = data.temperatureC;
  humidityPercent = data.humidityPercent;
  reactorState    = String(data.state);
  alarmActive     = data.alarmActive;
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

  scd30Ready    = initSCD30();
  lastValidRead = millis();
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  ArduinoCloud.update();

  // Reintento si el sensor falló
  if (!scd30Ready) {
    static uint32_t lastRetry = 0;
    if (millis() - lastRetry >= 5000) {
      lastRetry     = millis();
      scd30Ready    = initSCD30();
      lastValidRead = millis();

      if (!scd30Ready) {
        ReactorData errData = {};
        errData.temperatureC = -127.0;
        errData.sensorOk     = false;
        evaluateReactorState(errData);
        updateCloudVariables(errData);
      }
    }
    delay(100);
    return;
  }

  // Lectura del sensor
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll >= SENSOR_POLL_MS) {
    lastPoll = millis();

    if (airSensor.dataAvailable()) {
      latestData.co2SensorPpm    = airSensor.getCO2();
      latestData.temperatureC    = airSensor.getTemperature();
      latestData.humidityPercent = airSensor.getHumidity();
      latestData.co2CorrectedPpm = estimateCO2CorrectedPpm(latestData.co2SensorPpm);
      latestData.co2Percent      = estimateCO2Percent(latestData.co2SensorPpm);
      latestData.ch4Percent      = estimateCH4Percent(latestData.co2Percent);
      latestData.sensorOk        = true;

      evaluateReactorState(latestData);
      lastValidRead = millis();

      Serial.printf("CO2: %.0f ppm | Corr: %.0f ppm | CO2%%: %.1f | CH4%%: %.1f | T: %.1f°C | HR: %.1f%% | Estado: %s | Alarma: %s\n",
        latestData.co2SensorPpm,    latestData.co2CorrectedPpm,
        latestData.co2Percent,      latestData.ch4Percent,
        latestData.temperatureC,    latestData.humidityPercent,
        latestData.state,           latestData.alarmActive ? "SI" : "NO");

    } else {
      Serial.printf("[SCD30] Sin datos. Esperando... (%lu ms desde ultima lectura)\n",
        millis() - lastValidRead);
    }

    // Timeout
    if (millis() - lastValidRead > SENSOR_TIMEOUT_MS) {
      Serial.println("Timeout SCD30. Reinicializando...");
      scd30Ready = false;
    }
  }

  // Envío a Cloud cada 5 segundos
  static uint32_t lastCloudUpdate = 0;
  if (millis() - lastCloudUpdate >= CLOUD_UPDATE_MS) {
    updateCloudVariables(latestData);
    lastCloudUpdate = millis();
  }

  delay(10);
}