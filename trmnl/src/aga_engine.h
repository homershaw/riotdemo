#pragma once
#include <Arduino.h>

struct AGAEngineConfig {
  float pipeID_mm;
  float orifice_mm;
  float basePressure_kPaAbs;
  float baseTemperature_C;
  float atmosphericPressure_kPa;
  bool staticPressureIsAbsolute;
  uint8_t tapType;
};

struct AGAEngineResult {
  bool valid;
  float flow_E3m3_day;
  float massFlow_kg_s;
  float Cd;
  float Y1;
  float Ev;
  float beta;
  float ReD;
  float rhoFlow_kg_m3;
  float rhoBase_kg_m3;
  float Zf;
  float Zb;
  float kappa;
  float viscosity_cP;
  int flowError;
  int baseError;
  uint32_t calcTime_ms;
};

void agaEngineBegin();
AGAEngineResult agaEngineCalculate(
  const AGAEngineConfig& cfg,
  const float molPct[21],
  float dp_kPa,
  float staticPressureEU,
  float temp_C
);
