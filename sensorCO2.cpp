#include "thingProperties.h"
#include <Arduino.h>
#include <Wire.h>
#include "SparkFun_SCD30_Arduino_Library.h"

// =====================================================
// CONFIGURACIÓN GENERAL
// =====================================================

#define I2C_SDA 21
#define I2C_SCL 22
#define I2C_FREQ_HZ 50000

#define SCD30_INTERVAL_S 5
#define SENSOR_POLL_MS 1000
#define SENSOR_TIMEOUT_MS 15000

#define CLOUD_UPDATE_MS 5000

// El gas se diluye 1:50 antes de llegar al SCD30.
#define DILUTION_FACTOR 50.0

// Límite superior aproximado del SCD30.
// Si la lectura se acerca a este valor, el CO2 real puede estar en el límite superior.
#define SCD30_SATURATION_PPM 9800.0

SCD30 airSensor;

QueueHandle_t sensorQueue;
volatile bool scd30Ready = false;

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

  // -------------------------------
  // Evaluación por temperatura
  // -------------------------------

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

  // -------------------------------
  // Evaluación por gases
  // -------------------------------

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

  // El sensor está cerca del límite máximo.
  // Con factor 50, 9800 ppm equivalen a 49 % de CO2 estimado.
  if (data.co2SensorPpm >= SCD30_SATURATION_PPM) {
    gasObservation = true;
  }

  // -------------------------------
  // Estado combinado del reactor
  // -------------------------------

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

  if (!airSensor.begin()) {
    Serial.println("SCD30 no detectado por I2C.");
    scd30Ready = false;
    return false;
  }

  airSensor.setMeasurementInterval(SCD30_INTERVAL_S);

  // Autocalibración activada.
  airSensor.setAutoSelfCalibration(true);

  scd30Ready = true;

  Serial.println("SCD30 inicializado con autocalibracion activada.");
  return true;
}

// =====================================================
// ACTUALIZACIÓN DE VARIABLES DE ARDUINO CLOUD
// =====================================================

void updateCloudVariables(const ReactorData &data) {
  co2SensorPpm = data.co2SensorPpm;
  co2CorrectedPpm = data.co2CorrectedPpm;
  co2Percent = data.co2Percent;
  ch4Percent = data.ch4Percent;

  temperatureC = data.temperatureC;
  humidityPercent = data.humidityPercent;

  reactorState = String(data.state);
  alertLevel = data.alertLevel;
  sensorOk = data.sensorOk;
  alarmActive = data.alarmActive;
}

// =====================================================
// TAREA FREERTOS: LECTURA DEL SENSOR
// =====================================================

void taskSensor(void *parameter) {
  uint32_t lastValidRead = millis();
  uint32_t lastErrorReport = 0;

  for (;;) {
    if (!scd30Ready) {
      initSCD30();

      if (!scd30Ready && millis() - lastErrorReport >= 5000) {
        ReactorData errorData = {};

        errorData.co2SensorPpm = 0.0;
        errorData.co2CorrectedPpm = 0.0;
        errorData.co2Percent = 0.0;
        errorData.ch4Percent = 0.0;
        errorData.temperatureC = -127.0;
        errorData.humidityPercent = 0.0;
        errorData.sensorOk = false;

        evaluateReactorState(errorData);
        xQueueOverwrite(sensorQueue, &errorData);

        lastErrorReport = millis();
      }

      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    if (airSensor.dataAvailable()) {
      ReactorData data = {};

      data.co2SensorPpm = airSensor.getCO2();
      data.temperatureC = airSensor.getTemperature();
      data.humidityPercent = airSensor.getHumidity();

      data.co2CorrectedPpm = estimateCO2CorrectedPpm(data.co2SensorPpm);
      data.co2Percent = estimateCO2Percent(data.co2SensorPpm);
      data.ch4Percent = estimateCH4Percent(data.co2Percent);

      data.sensorOk = true;

      evaluateReactorState(data);

      xQueueOverwrite(sensorQueue, &data);

      lastValidRead = millis();

      Serial.print("CO2 sensor ppm: ");
      Serial.print(data.co2SensorPpm);

      Serial.print(" | CO2 corregido ppm: ");
      Serial.print(data.co2CorrectedPpm);

      Serial.print(" | CO2 %: ");
      Serial.print(data.co2Percent, 2);

      Serial.print(" | CH4 %: ");
      Serial.print(data.ch4Percent, 2);

      Serial.print(" | Temp C: ");
      Serial.print(data.temperatureC, 2);

      Serial.print(" | HR %: ");
      Serial.print(data.humidityPercent, 2);

      Serial.print(" | Estado: ");
      Serial.print(data.state);

      Serial.print(" | Alerta: ");
      Serial.println(data.alertLevel);
    }

    if (millis() - lastValidRead > SENSOR_TIMEOUT_MS) {
      scd30Ready = false;
      Serial.println("Timeout del SCD30. Se intentara reinicializar.");
    }

    vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_MS));
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  sensorQueue = xQueueCreate(1, sizeof(ReactorData));

  if (sensorQueue == NULL) {
    Serial.println("Error creando cola de datos.");
    while (true) {
      delay(1000);
    }
  }

  initProperties();

  ArduinoCloud.begin(ArduinoIoTPreferredConnection);

  setDebugMessageLevel(2);
  ArduinoCloud.printDebugInfo();

  initSCD30();

  xTaskCreatePinnedToCore(
    taskSensor,
    "TaskSensor",
    4096,
    NULL,
    2,
    NULL,
    1
  );
}

// =====================================================
// LOOP PRINCIPAL
// =====================================================

void loop() {
  ArduinoCloud.update();

  static uint32_t lastCloudUpdate = 0;
  static ReactorData latestData;

  if (millis() - lastCloudUpdate >= CLOUD_UPDATE_MS) {
    if (xQueuePeek(sensorQueue, &latestData, 0) == pdTRUE) {
      updateCloudVariables(latestData);
    }

    lastCloudUpdate = millis();
  }

  delay(10);
}