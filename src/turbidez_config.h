// =============================================================================
// turbidez_config.h — sensor de turbidez analogico (sonda infravermelha)
// -----------------------------------------------------------------------------
// Numa perna da sonda ha um LED infravermelho e na outra um sensor de luz. Quanto
// mais particulas (lama, sedimento), menos luz chega e MENOR a tensao na saida:
//   agua limpa -> ~4,1 V        agua turva -> a tensao cai (ex.: 2,5 V)
//
// Ligacao (a saida chega a ~4,5 V e o ESP32 aguenta no maximo 3,3 V!):
//   VCC -> VIN (5 V)    GND -> GND    chave D/A da plaquinha em "A"
//   OUT ──[ 10 kΩ ]──┬── GPIO 34
//                    └──[ 20 kΩ ]── GND        (divisor: 4,5 V vira 3,0 V)
// =============================================================================

#ifndef TURBIDEZ_CONFIG_H
#define TURBIDEZ_CONFIG_H

#include <Arduino.h>

namespace turbidez_config {

// Mude para false se o sensor ainda nao estiver ligado: o JSON envia null.
constexpr bool INSTALADO = true;

// Pino do ADC1 (os do ADC2 param de funcionar com o Wi-Fi ligado).
constexpr uint8_t PINO = 34;
// Resistores do divisor. Se usar outros valores, ajuste aqui.
constexpr float R1_OHMS = 10000.0f;  // entre o OUT e o GPIO 34
constexpr float R2_OHMS = 20000.0f;  // entre o GPIO 34 e o GND
constexpr float FATOR_DIVISOR = (R1_OHMS + R2_OHMS) / R2_OHMS;  // = 1,5

constexpr uint8_t LEITURAS = 20;
constexpr uint8_t OVERSAMPLE = 16;     // o ADC do ESP32 e ruidoso: media de 16 conversoes
constexpr float MINIMO_MV = 500.0f;    // abaixo disso: fio solto / sem energia

struct Resultado {
  bool valido = false;  // false = sensor desativado (INSTALADO = false)
  float mediaMv = 0;    // tensao media na saida do sensor
  float desvioMv = 0;

  // Ligado mas sem sinal (tensao quase zero): nao e "agua turva", e defeito.
  bool semSinal() const { return valido && mediaMv < MINIMO_MV; }
};

inline void iniciar() {
  if (INSTALADO) {
    analogSetPinAttenuation(PINO, ADC_11db);  // faixa de 0 a ~3,1 V no pino
  }
}

// UMA leitura, ja na tensao real do sensor (desfaz o divisor).
inline float lerMv() {
  uint32_t soma = 0;
  for (uint8_t i = 0; i < OVERSAMPLE; ++i) {
    soma += analogReadMilliVolts(PINO);
  }
  return static_cast<float>(soma) / OVERSAMPLE * FATOR_DIVISOR;
}

// 20 leituras (~1 s) -> media e desvio padrao.
inline Resultado medir() {
  Resultado r;
  if (!INSTALADO) return r;
  float variancia = 0;
  for (uint8_t n = 1; n <= LEITURAS; ++n) {
    const float valor = lerMv();
    const float delta = valor - r.mediaMv;
    r.mediaMv += delta / n;
    variancia += delta * (valor - r.mediaMv);
    delay(50);
  }
  r.desvioMv = sqrt(variancia / (LEITURAS - 1));
  r.valido = true;
  return r;
}

}  // namespace turbidez_config

#endif
