# SCD30-CO2-monitoring-system

## Descripcion

Este proyecto usa un sensor SCD30 conectado a un microcontrolador compatible con Arduino para medir variables ambientales y monitorearlas en Arduino Cloud.

El sistema toma lecturas de:

- CO2 (ppm)
- Temperatura (C)
- Humedad relativa (%)

Con esas lecturas tambien calcula estimaciones de composicion de gas para seguimiento del proceso:

- CO2 corregido por factor de dilucion
- Porcentaje estimado de CO2
- Porcentaje estimado de CH4

Ademas, evalua el estado general del reactor y genera niveles de alerta para visualizar en la nube.

## Variables que se monitorean en Arduino Cloud

- `co2SensorPpm`: lectura directa del SCD30.
- `co2CorrectedPpm`: CO2 ajustado por dilucion.
- `co2Percent`: porcentaje estimado de CO2.
- `ch4Percent`: porcentaje estimado de CH4.
- `temperatureC`: temperatura ambiente.
- `humidityPercent`: humedad relativa.
- `reactorState`: estado calculado del sistema.
- `alertLevel`: nivel de alerta.
- `sensorOk`: estado de salud del sensor.
- `alarmActive`: bandera de alarma activa.

## Flujo general

- Una tarea de lectura obtiene datos del SCD30 de forma periodica.
- Los datos se procesan y se colocan en una cola interna.
- El `loop()` principal sincroniza esas variables con Arduino Cloud.

## Requisitos basicos

- Placa compatible con Arduino y Arduino Cloud.
- Sensor SCD30.
- Librerias:
	- `SparkFun_SCD30_Arduino_Library`
	- `ArduinoIoTCloud` (segun configuracion de Arduino Cloud)
- Archivo `thingProperties.h` generado por Arduino Cloud.

## Uso rapido

1. Conectar el SCD30 por I2C.
2. Configurar el Thing en Arduino Cloud y sus variables.
3. Cargar el codigo en la placa.
4. Abrir el dashboard de Arduino Cloud para ver las variables en tiempo real.

## Nota

El calculo de CO2 corregido y el porcentaje de CH4 son estimaciones usadas para monitoreo del proceso.

