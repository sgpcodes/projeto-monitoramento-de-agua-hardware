// =============================================================================
// sensor_config.h — sensor de cor AS7341
// -----------------------------------------------------------------------------
// Ligacao:  VIN -> 3V3   GND -> GND   SDA -> GPIO 21   SCL -> GPIO 22
//
// O AS7341 tem 10 "olhinhos" (canais), cada um sensivel a uma cor. Cada leitura
// devolve 10 numeros de 0 a 65535: quanto maior, mais luz daquela cor chegou ao
// sensor depois de atravessar a agua. medir() faz 20 leituras e calcula a MEDIA
// (vai para a API) e o DESVIO PADRAO (o quanto variou).
// =============================================================================

#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

#include <Adafruit_AS7341.h>
#include <Arduino.h>
#include <Wire.h>

namespace sensor_config {

constexpr uint8_t PINO_SDA = 21;
constexpr uint8_t PINO_SCL = 22;

// GANHO: amplificacao. Alto enxerga luz fraca, mas estoura (65535) com luz forte.
constexpr as7341_gain_t GANHO_PADRAO = AS7341_GAIN_64X;
// Tempo de cada leitura = (ATIME + 1) x (ASTEP + 1) x 2,78 us = ~281 ms.
// Depois que o estudo comecar, NAO mude ATIME/ASTEP/ganho (guia, "Regras de ouro").
constexpr uint8_t ATIME = 100;
constexpr uint16_t ASTEP = 999;
constexpr uint8_t LEITURAS = 20;             // leituras por medicao
constexpr uint16_t ESPERA_MS = 3000;         // tempo para fechar a caixa
constexpr uint16_t SATURACAO = 65535;        // valor maximo = canal estourado
constexpr uint16_t LUZ_FRACA = 1000;         // CLEAR abaixo disso = lanterna fraca/torta
constexpr uint8_t CANAIS = 10;
constexpr uint8_t CORES = 8;                 // F1 a F8 (as visiveis)
constexpr uint8_t CLEAR = 8;                 // posicao do canal CLEAR (luz total)

// As duas listas estao na MESMA ordem: canal da biblioteca e nome no JSON.
const as7341_color_channel_t CANAL[CANAIS] = {
  AS7341_CHANNEL_415nm_F1,  // violeta
  AS7341_CHANNEL_445nm_F2,  // azul
  AS7341_CHANNEL_480nm_F3,  // ciano
  AS7341_CHANNEL_515nm_F4,  // verde
  AS7341_CHANNEL_555nm_F5,  // verde-amarelo
  AS7341_CHANNEL_590nm_F6,  // amarelo
  AS7341_CHANNEL_630nm_F7,  // laranja
  AS7341_CHANNEL_680nm_F8,  // vermelho (pico da clorofila -> algas)
  AS7341_CHANNEL_CLEAR,     // luz total
  AS7341_CHANNEL_NIR,       // infravermelho (~910 nm)
};
// Nome e comprimento de onda (nm) das 8 cores visiveis, como vao no JSON.
const char* const NOME_COR[CORES] = {
  "Violeta", "Azul", "Ciano", "Verde", "Verde-amarelo", "Amarelo", "Laranja", "Vermelho",
};
const uint16_t ONDA_NM[CORES] = {415, 445, 480, 515, 555, 590, 630, 680};

struct Resultado {
  float media[CANAIS];
  float desvio[CANAIS];
  uint8_t leituras = 0;   // quantas deram certo (normalmente 20)
  bool saturado = false;  // algum canal bateu 65535?
};

// Codigo do ganho na biblioteca -> valor "humano" (codigo 7 -> 64X).
inline uint16_t ganhoEmX(as7341_gain_t ganho) {
  const uint8_t codigo = static_cast<uint8_t>(ganho);
  return codigo == 0 ? 0 : static_cast<uint16_t>(1U << (codigo - 1));
}

// O contrario: 1, 2, 4 ... 512 -> codigo. False se o numero nao for um ganho valido.
inline bool ganhoDeX(int x, as7341_gain_t& ganho) {
  for (uint8_t codigo = 1; codigo <= static_cast<uint8_t>(AS7341_GAIN_512X); ++codigo) {
    if ((1 << (codigo - 1)) == x) {
      ganho = static_cast<as7341_gain_t>(codigo);
      return true;
    }
  }
  return false;
}

inline bool iniciar(Adafruit_AS7341& as7341) {
  Wire.begin(PINO_SDA, PINO_SCL);
  if (!as7341.begin()) {
    return false;
  }
  as7341.setATIME(ATIME);
  as7341.setASTEP(ASTEP);
  as7341.setGain(GANHO_PADRAO);
  as7341.enableLED(false);  // a luz vem da lanterna; o LED da plaquinha fica apagado
  return true;
}

// UMA leitura dos 10 canais.
inline bool lerCanais(Adafruit_AS7341& as7341, uint16_t valores[CANAIS]) {
  if (!as7341.readAllChannels()) {
    return false;
  }
  for (uint8_t c = 0; c < CANAIS; ++c) {
    valores[c] = as7341.getChannel(CANAL[c]);
  }
  return true;
}

// 20 leituras -> media e desvio padrao (metodo de Welford: sem guardar as 20).
inline bool medir(Adafruit_AS7341& as7341, Stream& serial, Resultado& r, uint16_t esperaMs) {
  delay(esperaMs);
  r = Resultado();
  float variancia[CANAIS] = {0};
  for (uint8_t c = 0; c < CANAIS; ++c) r.media[c] = 0;

  for (uint8_t i = 0; i < LEITURAS; ++i) {
    uint16_t valores[CANAIS];
    if (!lerCanais(as7341, valores)) continue;  // falhou: as outras continuam valendo
    const uint8_t n = ++r.leituras;
    for (uint8_t c = 0; c < CANAIS; ++c) {
      r.saturado = r.saturado || valores[c] >= SATURACAO;
      const float delta = valores[c] - r.media[c];
      r.media[c] += delta / n;
      variancia[c] += delta * (valores[c] - r.media[c]);
    }
    serial.print('.');  // um ponto por leitura, para ver o progresso
  }
  serial.println();

  for (uint8_t c = 0; c < CANAIS; ++c) {
    r.desvio[c] = r.leituras > 1 ? sqrt(variancia[c] / (r.leituras - 1)) : 0;
  }
  return r.leituras > 0;
}

}  // namespace sensor_config

#endif
