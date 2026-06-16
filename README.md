# SCD30-CO2-monitoring-system

## Descripcion

Este proyecto usa un sensor SCD30 conectado a un microcontrolador compatible con Arduino para medir variables ambientales y monitorearlas en Arduino Cloud.

El archivo principal del proyecto es `sensorCO2.ino`.

El sistema toma lecturas de:

- CO2 (ppm)
- Temperatura (C)
- Humedad relativa (%)

Con esas lecturas tambien calcula estimaciones internas de composicion de gas para seguimiento del proceso:

- CO2 corregido por factor de dilucion
- Porcentaje estimado de CO2
- Porcentaje estimado de CH4

Ademas, evalua el estado general del reactor y genera niveles de alerta para visualizar en la nube.

## Variables publicadas en Arduino Cloud

- `co2SensorPpm`: lectura directa del SCD30.
- `temperatureC`: temperatura ambiente.
- `humidityPercent`: humedad relativa.
- `reactorState`: estado calculado del sistema.
- `alarmActive`: bandera de alarma activa.

## Variables internas calculadas

- `co2CorrectedPpm`: CO2 ajustado por dilucion.
- `co2Percent`: porcentaje estimado de CO2.
- `ch4Percent`: porcentaje estimado de CH4.
- `alertLevel`: nivel de alerta interno.
- `sensorOk`: estado interno del sensor.

## Flujo general

- En `setup()` se inicializa Arduino Cloud y el SCD30 por I2C.
- En `loop()` se leen datos del sensor de forma periodica.
- Se calculan variables derivadas y se evalua el estado del reactor.
- Se actualizan las variables de Arduino Cloud cada 5 segundos.
- Si el sensor falla o entra en timeout, el sistema intenta reinicializarlo automaticamente.

## Requisitos basicos

- Placa compatible con Arduino y Arduino Cloud.
- Sensor SCD30.
- Librerias:
	- `SparkFun_SCD30_Arduino_Library`
	- `ArduinoIoTCloud` (segun configuracion de Arduino Cloud)
- Archivo `thingProperties.h` generado por Arduino Cloud.
- Archivo `arduino_secrets.h` para credenciales de conexion (si aplica segun placa).

## Uso rapido

1. Conectar el SCD30 por I2C.
2. Configurar el Thing en Arduino Cloud y sus variables.
3. Cargar el codigo en la placa.
4. Abrir el dashboard de Arduino Cloud para ver las variables en tiempo real.

## Nota

El calculo de CO2 corregido y el porcentaje de CH4 son estimaciones usadas para monitoreo del proceso, pero no se publican directamente en Arduino Cloud en esta version.

