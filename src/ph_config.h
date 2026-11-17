// =============================================================================
// ph_config.h — sensor de pH (eletrodo de vidro + plaquinha PH-4502C)
// -----------------------------------------------------------------------------
// O eletrodo (vidro + referencia de prata/cloreto de prata) gera alguns milivolts
// conforme o pH; a plaquinha amplifica e entrega no pino Po uma tensao de 0 a 5 V:
//   pH 7 -> ~2,5 V      agua acida (pH baixo) -> SOBE      alcalina -> DESCE
// (~0,18 V por unidade de pH).
//
// Ligacao (a saida Po pode passar de 3,3 V e o ESP32 aguenta no maximo 3,3 V!):
//   V+ -> VIN (5 V)    G -> GND    To e Do: nao usados
//   Po ──[ 10 kΩ ]──┬── GPIO 35
//                   └──[ 10 kΩ ]── GND        (divisor: 5 V vira 2,5 V)
//
// Cuidados do manual do eletrodo:
//   - Eletrodo novo ou parado ha muito tempo: deixar 8 horas na solucao de KCl 3 M.
//   - Responde em ate 2 minutos: espere a leitura parar de mudar antes de calibrar.
//   - Entre uma agua e outra, lave com agua destilada (sujeira de uma agua passa
//     para a outra). Guarde com a tampinha com KCl, nunca seco.
//   - O conector BNC tem que ficar limpo e seco.
//
// Calibracao sem solucao (comando ph zero): com o BNC em curto o eletrodo "da" 0 mV,
// que pelo manual e o pH 7 (+- 0,5); grava esse ponto e usa a inclinacao tipica.
// Com solucao padrao (comando ph N): coloque o eletrodo na solucao, espere 2 minutos
// e digite, por exemplo, ph 6.86 e depois ph 4.01 (ou 9.18).
// Agua mineral serve de solucao: o pH vem no rotulo (ex.: ph 7.4).
// Fica salva na placa. Sem calibrar, usa os valores tipicos da plaquinha
// (pH 7 = 2,50 V e -0,18 V/pH): o pH vai no JSON com calibracao "pendente".
// =============================================================================

#ifndef PH_CONFIG_H
#define PH_CONFIG_H

#include <Arduino.h>
#include <LittleFS.h>

namespace ph_config {

// Mude para false se o sensor ainda nao estiver ligado: o JSON envia null.
constexpr bool INSTALADO = true;

// Pino do ADC1 (os do ADC2 param de funcionar com o Wi-Fi ligado). O 34 e da turbidez.
constexpr uint8_t PINO = 35;
constexpr float R1_OHMS = 10000.0f;  // entre o Po e o GPIO 35
constexpr float R2_OHMS = 10000.0f;  // entre o GPIO 35 e o GND
constexpr float FATOR_DIVISOR = (R1_OHMS + R2_OHMS) / R2_OHMS;  // = 2,0

constexpr uint8_t LEITURAS = 20;
constexpr uint8_t OVERSAMPLE = 16;
constexpr float MINIMO_MV = 300.0f;                     // abaixo: fio solto / sem energia
constexpr float MAXIMO_MV = 3000.0f * FATOR_DIVISOR;    // ADC travado no maximo
constexpr float INSTAVEL_MV = 30.0f;                    // ~0,17 pH de variacao em 1 s

// Valores tipicos da plaquinha, usados ate calibrar.
constexpr float MV_PH7_PADRAO = 2500.0f;
constexpr float MV_POR_PH_PADRAO = -180.0f;
constexpr char ARQUIVO[] = "/ph_calibracao.bin";

// Faixa de pH para agua doce (CONAMA 357/2005, classes 1 a 3): 6 a 9.
inline const char* classificar(float ph) { return ph < 6.0f ? "ACIDA" : ph <= 9.0f ? "NORMAL" : "ALCALINA"; }

// Dois pontos de calibracao (pH da solucao padrao e tensao medida nela).
struct Calibracao {
  uint8_t versao = 1;
  uint8_t pontos = 0;  // 0 = valores tipicos, 1 = so desloca, 2 = reta completa
  float ph[2] = {0, 0};
  float mv[2] = {0, 0};
};

inline Calibracao& calibracao() {
  static Calibracao c;
  return c;
}

inline const char* nomeCalibracao() {
  const uint8_t p = calibracao().pontos;
  return p >= 2 ? "2_pontos" : p == 1 ? "1_ponto" : "pendente";
}

// Inclinacao (mV por unidade de pH) e tensao no pH 7, pela calibracao.
inline void reta(float& mvPorPh, float& mvPh7) {
  const Calibracao& c = calibracao();
  mvPorPh = MV_POR_PH_PADRAO;
  mvPh7 = MV_PH7_PADRAO;
  if (c.pontos >= 2) mvPorPh = (c.mv[1] - c.mv[0]) / (c.ph[1] - c.ph[0]);
  if (c.pontos >= 1) mvPh7 = c.mv[0] + (7.0f - c.ph[0]) * mvPorPh;
}

inline float phDaTensao(float mv) {
  float mvPorPh, mvPh7;
  reta(mvPorPh, mvPh7);
  return 7.0f + (mv - mvPh7) / mvPorPh;
}

enum class Estado : uint8_t { DESATIVADO, OK, SEM_SINAL, SATURADO, INSTAVEL, FORA_DA_FAIXA };
inline const char* nomeEstado(Estado e) {
  switch (e) {
    case Estado::OK: return "ok";
    case Estado::SEM_SINAL: return "sem_sinal";
    case Estado::SATURADO: return "saturado";
    case Estado::INSTAVEL: return "instavel";
    case Estado::FORA_DA_FAIXA: return "fora_da_faixa";
    default: return "desativado";
  }
}

struct Resultado {
  bool valido = false;  // false = sensor desativado
  float adcBruto = 0;   // media da leitura crua do ADC (0 a 4095)
  float mediaMv = 0;    // tensao media no Po (divisor desfeito)
  float desvioMv = 0;
  float ph = NAN;

  Estado estado() const {
    if (!valido) return Estado::DESATIVADO;
    if (mediaMv < MINIMO_MV) return Estado::SEM_SINAL;
    if (mediaMv >= MAXIMO_MV) return Estado::SATURADO;
    if (desvioMv > INSTAVEL_MV) return Estado::INSTAVEL;
    if (isnan(ph) || ph < 0.0f || ph > 14.0f) return Estado::FORA_DA_FAIXA;
    return Estado::OK;
  }
  bool temPh() const { return estado() == Estado::OK; }
};

// Chamar depois do LittleFS.begin (dashboard_api::iniciar).
inline void iniciar() {
  if (!INSTALADO) return;
  analogReadResolution(12);
  analogSetPinAttenuation(PINO, ADC_11db);
  File f = LittleFS.open(ARQUIVO, FILE_READ);
  if (!f) return;
  Calibracao lida;
  if (f.size() == sizeof(lida) && f.read(reinterpret_cast<uint8_t*>(&lida), sizeof(lida)) == sizeof(lida) &&
      lida.versao == Calibracao().versao) {
    calibracao() = lida;
  }
  f.close();
}

// UMA leitura (media de 16 conversoes): tensao no Po, em mV.
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
  r.ph = phDaTensao(r.mediaMv);
  r.valido = true;
  return r;
}

// Comando ph N: mede a solucao padrao de pH N e guarda o ponto. Um ponto novo
// perto (menos de 1 pH) de um ja salvo substitui esse; senao, vira o 2o ponto.
inline bool calibrar(float phPadrao, Stream& serial) {
  if (!INSTALADO || phPadrao < 1.0f || phPadrao > 13.0f) return false;
  const Resultado r = medir();
  if (r.estado() == Estado::SEM_SINAL || r.estado() == Estado::SATURADO) {
    serial.printf(" ✗ Sensor de pH %s (%.2f V): confira os fios. Calibracao NAO salva.\n", nomeEstado(r.estado()),
                  r.mediaMv / 1000.0f);
    return false;
  }
  if (r.estado() == Estado::INSTAVEL) {
    serial.println(F(" ✗ Leitura ainda mudando: espere 2 minutos com o eletrodo parado e tente de novo."));
    return false;
  }
  Calibracao& c = calibracao();
  uint8_t i = 0;
  if (c.pontos == 1) i = fabsf(c.ph[0] - phPadrao) < 1.0f ? 0 : 1;
  if (c.pontos == 2) i = fabsf(c.ph[0] - phPadrao) <= fabsf(c.ph[1] - phPadrao) ? 0 : 1;
  c.ph[i] = phPadrao;
  c.mv[i] = r.mediaMv;
  c.pontos = max<uint8_t>(c.pontos, i + 1);
  File f = LittleFS.open(ARQUIVO, FILE_WRITE);
  if (f) {
    f.write(reinterpret_cast<const uint8_t*>(&c), sizeof(c));
    f.close();
  }
  float mvPorPh, mvPh7;
  reta(mvPorPh, mvPh7);
  serial.printf(" ✓ pH %.2f gravado (%.3f V). Calibracao: %s · pH 7 = %.3f V · %.1f mV/pH\n", phPadrao,
                r.mediaMv / 1000.0f, nomeCalibracao(), mvPh7 / 1000.0f, mvPorPh);
  // Eletrodo bom: 160 a 200 mV/pH na saida da plaquinha (59 mV/pH no vidro x ganho).
  if (c.pontos >= 2 && (fabsf(mvPorPh) < 120.0f || fabsf(mvPorPh) > 240.0f)) {
    serial.println(F(" ⚠ Inclinacao estranha: confira as solucoes ou se o eletrodo precisa de KCl (8 h)."));
  }
  if (c.pontos < 2) serial.println(F("   (Opcional) Para mais precisao, grave outra solucao longe desta (ex.: ph 4.01)."));
  return true;
}

inline void apagarCalibracao() {
  calibracao() = Calibracao();
  LittleFS.remove(ARQUIVO);
}

}  // namespace ph_config

#endif
