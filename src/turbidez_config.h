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
//                    └──[ 10 kΩ ]── GND        (divisor: 5 V vira 2,5 V)
// O divisor e pela METADE de proposito: o ADC do ESP32 so mede bem ate ~2,45 V.
// Com 10k/20k a agua limpa ficava em ~3 V no pino e a leitura travava no maximo.
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
constexpr float R2_OHMS = 10000.0f;  // entre o GPIO 34 e o GND
constexpr float FATOR_DIVISOR = (R1_OHMS + R2_OHMS) / R2_OHMS;  // = 2,0

constexpr uint8_t LEITURAS = 20;
constexpr uint8_t OVERSAMPLE = 16;     // o ADC do ESP32 e ruidoso: media de 16 conversoes
constexpr float MINIMO_MV = 500.0f;    // abaixo disso: fio solto / sem energia
// Acima disso o pino passou de ~3,0 V: o ADC do ESP32 travou no maximo e o valor
// nao vale (chave D/A em "D", ou divisor ligado errado). Agua limpa da ~4,1 V.
constexpr float MAXIMO_MV = 3000.0f * FATOR_DIVISOR;

// Tensao REAL entre VIN e GND, medida com o multimetro com tudo ligado (pela USB
// costuma dar 4,5 a 4,8 V, nao 5 V). A curva do fabricante e para 5 V: o Arduino
// Uno compensa isso sozinho (o ADC dele usa a propria alimentacao como regua), o
// ESP32 nao. Sem esse ajuste, 0,3 V a menos ja faz agua limpa parecer muito turva.
constexpr float ALIMENTACAO_V = 5.0f;

// ---- Calibracao (tensao -> NTU) -----------------------------------------------
// O MODELO DO SENSOR NAO FOI CONFIRMADO por documentacao do projeto. A curva abaixo
// e a publicada pela DFRobot para o SEN0189 alimentado em 5 V
// (NTU = -1120,4 V^2 + 5742,3 V - 4352,9, valida de ~2,5 V a ~4,2 V). Ela NUNCA foi
// comparada com padroes de turbidez neste aparelho: o NTU calculado e so uma
// ESTIMATIVA EXPERIMENTAL e vai no JSON como "ntu_experimental"; o campo "ntu"
// (valor validado) fica null ate existir uma calibracao validada.
// Para validar no futuro: medir padroes de formazina (ou comparar com um turbidimetro
// de referencia), ajustar os coeficientes, trocar o ID e marcar VALIDADA = true.
struct Calibracao {
  const char* id;     // identificacao e versao da curva usada
  bool validada;      // so true com comparacao contra padroes/turbidimetro
  float a, b, c;      // NTU = a*V^2 + b*V + c (V = tensao na saida do sensor, em 5 V)
  float minV, maxV;   // faixa de tensao em que a curva vale
};
constexpr Calibracao CALIBRACAO = {"dfrobot-sen0189-fabricante-v1", false,
                                   -1120.4f, 5742.3f, -4352.9f, 2.5f, 4.2f};

// Ajuste fino da curva, em volts (so faz sentido depois de calibrar).
constexpr float AJUSTE_V = 0.0f;

// Filtragem: 20 leituras de 16 conversoes cada (~1 s). Desvio acima disso = instavel.
constexpr float INSTAVEL_MV = 100.0f;

// Estado da medicao, enviado no JSON (turbidez.status).
enum class Estado : uint8_t { DESATIVADO, OK, SEM_SINAL, SATURADO, INSTAVEL, FORA_DA_CURVA };
inline const char* nomeEstado(Estado e) {
  switch (e) {
    case Estado::OK: return "ok";
    case Estado::SEM_SINAL: return "sem_sinal";
    case Estado::SATURADO: return "saturado";
    case Estado::INSTAVEL: return "instavel";
    case Estado::FORA_DA_CURVA: return "fora_da_faixa_da_curva";
    default: return "desativado";
  }
}

// Etapa 3: tensao na saida do sensor (mV) -> tensao equivalente em 5 V (a curva e
// para 5 V; ALIMENTACAO_V compensa a alimentacao real).
inline float tensaoCorrigidaV(float mv) { return mv / 1000.0f * (5.0f / ALIMENTACAO_V) + AJUSTE_V; }

// Etapa 3: curva -> NTU. Fora da faixa da curva devolve NAN (nada de "cortar" o valor).
inline float ntuDaTensao(float mv) {
  const float v = tensaoCorrigidaV(mv);
  if (v < CALIBRACAO.minV || v > CALIBRACAO.maxV) return NAN;
  return max(0.0f, CALIBRACAO.a * v * v + CALIBRACAO.b * v + CALIBRACAO.c);
}

// Classificacao em palavras, com referencias de legislacao brasileira. So e usada
// com NTU de calibracao VALIDADA (ate 5 NTU = Portaria GM/MS 888/2021; ate 40 e
// 100 NTU = CONAMA 357/2005, classes 1 e 2/3 de agua doce).
inline const char* classificar(float ntu) {
  return ntu < 5 ? "LIMPA" : ntu < 40 ? "LEVEMENTE TURVA" : ntu < 100 ? "TURVA" : "MUITO TURVA";
}

struct Resultado {
  bool valido = false;  // false = sensor desativado (INSTALADO = false)
  float adcBruto = 0;   // etapa 1: media da leitura crua do ADC (0 a 4095)
  float mediaMv = 0;    // etapa 2: tensao media na SAIDA do sensor (divisor desfeito)
  float desvioMv = 0;
  float ntu = NAN;      // etapa 3: NTU pela curva (experimental enquanto nao validada)

  // Etapa 4: validacao. Leitura invalida NUNCA vira 0 NTU.
  Estado estado() const {
    if (!valido) return Estado::DESATIVADO;
    if (mediaMv < MINIMO_MV) return Estado::SEM_SINAL;
    if (mediaMv >= MAXIMO_MV) return Estado::SATURADO;
    if (desvioMv > INSTAVEL_MV) return Estado::INSTAVEL;
    if (isnan(ntu)) return Estado::FORA_DA_CURVA;
    return Estado::OK;
  }
  bool semSinal() const { return estado() == Estado::SEM_SINAL; }
  bool noMaximo() const { return estado() == Estado::SATURADO; }
  // Tem estimativa pela curva (experimental).
  bool temNtu() const { return estado() == Estado::OK; }
  // NTU de verdade: so com calibracao validada.
  bool temNtuValidado() const { return temNtu() && CALIBRACAO.validada; }
};

inline void iniciar() {
  if (INSTALADO) {
    analogReadResolution(12);                 // 0 a 4095
    analogSetPinAttenuation(PINO, ADC_11db);  // faixa de 0 a ~3,1 V no pino (boa ate ~2,45 V)
  }
}

// Etapas 1 e 2: UMA leitura (media de 16 conversoes) -> ADC cru e tensao real do
// sensor. analogReadMilliVolts usa a calibracao de fabrica do ADC do ESP32.
inline float lerMv(float* adcBruto = nullptr) {
  uint32_t soma = 0, somaBruto = 0;
  for (uint8_t i = 0; i < OVERSAMPLE; ++i) {
    soma += analogReadMilliVolts(PINO);
    somaBruto += analogRead(PINO);
  }
  if (adcBruto) *adcBruto = static_cast<float>(somaBruto) / OVERSAMPLE;
  return static_cast<float>(soma) / OVERSAMPLE * FATOR_DIVISOR;
}

// 20 leituras (~1 s) -> media e desvio padrao (Welford).
inline Resultado medir() {
  Resultado r;
  if (!INSTALADO) return r;
  float variancia = 0;
  for (uint8_t n = 1; n <= LEITURAS; ++n) {
    float bruto = 0;
    const float valor = lerMv(&bruto);
    const float delta = valor - r.mediaMv;
    r.mediaMv += delta / n;
    variancia += delta * (valor - r.mediaMv);
    r.adcBruto += (bruto - r.adcBruto) / n;
    delay(50);
  }
  r.desvioMv = sqrt(variancia / (LEITURAS - 1));
  r.ntu = ntuDaTensao(r.mediaMv);
  r.valido = true;
  return r;
}

}  // namespace turbidez_config

#endif
