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

// CALIBRACAO: com AGUA LIMPA no pote, a medicao deve dar perto de 0 NTU.
// Se der mais, aumente este valor (em volts) ate chegar perto de 0. Ex.: 0.15
constexpr float AJUSTE_V = 0.0f;

// Tensao -> NTU (unidade padrao de turbidez), curva do fabricante desse modelo de
// sensor alimentado em 5 V. Vale de 2,5 V (~3000 NTU) a 4,2 V (~0 NTU).
inline float ntuDaTensao(float mv) {
  const float v = constrain(mv / 1000.0f + AJUSTE_V, 2.5f, 4.2f);
  return max(0.0f, -1120.4f * v * v + 5742.3f * v - 4352.9f);
}

// Classificacao em palavras, com referencias de legislacao brasileira:
//   ate 5 NTU = limite da agua potavel (Portaria GM/MS 888/2021)
//   ate 40 NTU = agua doce classe 1; ate 100 NTU = classes 2 e 3 (CONAMA 357/2005)
inline const char* classificar(float ntu) {
  return ntu < 5 ? "LIMPA" : ntu < 40 ? "LEVEMENTE TURVA" : ntu < 100 ? "TURVA" : "MUITO TURVA";
}

struct Resultado {
  bool valido = false;  // false = sensor desativado (INSTALADO = false)
  float mediaMv = 0;    // tensao media na saida do sensor
  float desvioMv = 0;
  float ntu = 0;        // turbidez em NTU

  // Ligado mas sem sinal (tensao quase zero): nao e "agua turva", e defeito.
  bool semSinal() const { return valido && mediaMv < MINIMO_MV; }
  // Leitura confiavel para mostrar/enviar em NTU.
  bool temNtu() const { return valido && !semSinal(); }
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
  r.ntu = ntuDaTensao(r.mediaMv);
  r.valido = true;
  return r;
}

}  // namespace turbidez_config

#endif
