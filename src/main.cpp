// =============================================================================
// main.cpp — o aparelho: mede a agua, mostra o relatorio e envia para a API
// -----------------------------------------------------------------------------
// Arquivos:
//   sensor_config.h    sensor de cor AS7341
//   turbidez_config.h  sensor de turbidez (em NTU)
//   ph_config.h        sensor de pH (e a calibracao com solucoes padrao)
//   wifi_config.h      Wi-Fi e hora da internet
//   dashboard_api.h    JSON, envio para a API, fila sem internet e historico CSV
//   secrets.h          senha do Wi-Fi, link e chave da API (fora do git)
//
// Uso: aperte o botao BOOT (ou digite gravar) para gravar a agua limpa (0%) e a
// agua suja (100%); ficam salvas mesmo desligando. Fora isso, o aparelho mede a
// lagoa sozinho a cada 10 segundos, sem parar, e envia para o dashboard.
//
// Os dois nucleos do ESP32 trabalham ao mesmo tempo:
//   nucleo 1 (loop)        mede os 3 sensores, mostra o relatorio, grava o historico
//                          e poe a medicao na FILA. Nunca espera a internet.
//   nucleo 0 (tarefaEnvio) tira da fila e envia para a API. E o nucleo do Wi-Fi:
//                          a espera pela internet (ate 30 s) nao atrasa as medicoes.
// A fila guarda ate 10 medicoes na RAM; sem internet elas vao para a fila da
// memoria flash (/pendentes.jsonl) e sao reenviadas depois.
//
// Contas, com o padrao de agua limpa como 100% de luz:
//   % de luz que passou = amostra / padrao      absorbancia A = -log10(fracao)
//   turbidez = NTU do sensor de turbidez; pelo AS7341 = luz bloqueada em 590-630 nm
//   (faixa onde a cor da agua e a clorofila quase nao absorvem: sobra o efeito das
//   particulas, mesma ideia da norma ISO 7027)
//   algas = A680 - A590      cor (materia organica) = A415 - A555
//   contaminacao = (A . A_podre) / (A_podre . A_podre)   0% = limpa, 100% = podre
// As palavras (LIMPA, TURVA...) ajudam a leitura; nao sao laudo de laboratorio.
// =============================================================================

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>

#include "dashboard_api.h"
#include "ph_config.h"
#include "sensor_config.h"
#include "turbidez_config.h"
#include "wifi_config.h"

namespace {

using sensor_config::CANAIS;
using sensor_config::CORES;

constexpr uint32_t INTERVALO_MS = 10UL * 1000UL;       // medicao automatica: 10 s (mudanca aparece quase na hora)
constexpr uint32_t REENVIO_MS = 5UL * 60UL * 1000UL;   // tenta a fila a cada 5 min
constexpr uint32_t WATCHDOG_S = 180;                   // travou 3 min? reinicia sozinho
constexpr uint8_t REPLICAS = 3;                        // medicoes ao gravar limpa/suja
constexpr char ARQUIVO_REFERENCIA[] = "/referencia.bin";
// Botao BOOT da placa. Para um botao externo: ligue entre GPIO 4 e GND e mude para 4.
constexpr uint8_t PINO_BOTAO = 0;
// Fila entre os nucleos e a tarefa de envio.
constexpr uint8_t TAMANHO_FILA = 10;                   // medicoes esperando na RAM
constexpr uint32_t RETENTAR_WIFI_MS = 60UL * 1000UL;   // sem Wi-Fi: tenta reconectar a cada 1 min
constexpr BaseType_t NUCLEO_ENVIO = 0;                 // o mesmo do Wi-Fi
QueueHandle_t fila = nullptr;

Adafruit_AS7341 as7341;

// ---- Referencias: padrao de agua limpa (100% de luz) e agua podre (100% sujeira)
struct Referencia {
  uint8_t versao = 3;  // muda se a struct mudar: arquivo antigo e ignorado
  as7341_gain_t ganho = sensor_config::GANHO_PADRAO;
  bool temPadrao = false;
  float padrao[CANAIS] = {0};
  bool temPodre = false;
  float podre[CORES] = {0};  // assinatura (absorbancia) da agua podre
};

// O que o aparelho lembra entre um comando e outro.
struct Estado {
  as7341_gain_t ganho = sensor_config::GANHO_PADRAO;
  bool aoVivo = false;     // comando v
  bool automatico = true;  // comando p
  uint32_t proximaMs = INTERVALO_MS;
  char amostra[21] = "lagoa";  // comando n
  Referencia ref;
} estado;

void salvarReferencia() {
  File f = LittleFS.open(ARQUIVO_REFERENCIA, FILE_WRITE);
  if (f) {
    f.write(reinterpret_cast<const uint8_t*>(&estado.ref), sizeof(Referencia));
    f.close();
  }
}

void carregarReferencia() {
  File f = LittleFS.open(ARQUIVO_REFERENCIA, FILE_READ);
  if (!f) return;
  Referencia lida;
  if (f.size() == sizeof(lida) && f.read(reinterpret_cast<uint8_t*>(&lida), sizeof(lida)) == sizeof(lida) &&
      lida.versao == Referencia().versao) {
    estado.ref = lida;
  }
  f.close();
}

bool temPadrao() { return estado.ref.temPadrao && estado.ref.ganho == estado.ganho; }

// ---- Contas e palavras ----------------------------------------------------------
struct Indices {
  float luz[CORES];     // fracao de luz que passou (1 = 100%)
  float absorb[CORES];  // absorbancia
  float turbidezCor;    // 0 a 1: fracao da luz bloqueada pelas particulas (AS7341)
  float algas, cor;
  bool temEscala = false;
  bool poucaLuz = false;  // sinal fraco demais: cor e turbidez pelo AS7341 nao valem
  float contaminacao = 0;  // 0 = limpa, 1 = igual a podre
};

Indices calcular(const sensor_config::Resultado& r) {
  const Referencia& ref = estado.ref;
  Indices x;
  for (uint8_t c = 0; c < CORES; ++c) {
    x.luz[c] = ref.padrao[c] > 0 ? constrain(r.media[c] / ref.padrao[c], 0.0f, 1.5f) : 0;
    x.absorb[c] = -log10(max(x.luz[c], 0.001f));
  }
  // Media (geometrica) do amarelo 590 nm e do laranja 630 nm.
  x.turbidezCor = constrain(1.0f - sqrt(x.luz[5] * x.luz[6]), 0.0f, 1.0f);
  x.algas = x.absorb[7] - x.absorb[5];  // 680 - 590 (clorofila)
  x.cor = x.absorb[0] - x.absorb[4];    // 415 - 555 (materia organica)
  // Com menos de 1000 contagens no canal CLEAR o sensor mede ruido: tudo parece
  // "agua turvissima". Melhor dizer que nao deu para medir.
  x.poucaLuz = r.media[sensor_config::CLEAR] < sensor_config::LUZ_FRACA;

  // Escala limpa -> podre (Beer-Lambert: x% de agua podre gera x% da absorbancia dela).
  if (ref.temPodre) {
    float produto = 0, norma = 0;
    for (uint8_t c = 0; c < CORES; ++c) {
      produto += x.absorb[c] * ref.podre[c];
      norma += ref.podre[c] * ref.podre[c];
    }
    x.temEscala = norma > 1e-4f;
    x.contaminacao = x.temEscala ? max(0.0f, produto / norma) : 0;
  }
  return x;
}

const char* palavraTurbidez(const Indices& x, const turbidez_config::Resultado& t) {
  // Palavra pela lei (NTU) so com calibracao validada; senao, pelo sensor de cor.
  if (t.temNtuValidado()) return turbidez_config::classificar(t.ntu);
  if (x.poucaLuz) return "SEM LEITURA";
  const float v = x.turbidezCor;
  return v < 0.05f ? "LIMPA" : v < 0.15f ? "LEVEMENTE TURVA" : v < 0.35f ? "TURVA" : "MUITO TURVA";
}
// Mesmos limites do dashboard: abaixo de 0,05 baixo; ate 0,20 moderado; acima alto.
constexpr float INDICE_MODERADO = 0.05f;
constexpr float INDICE_ALTO = 0.20f;
const char* palavraAlgas(float v) { return v < INDICE_MODERADO ? "NAO DETECTADAS" : v <= INDICE_ALTO ? "SINAL FRACO" : "PRESENTES"; }
const char* palavraCor(float v) { return v < INDICE_MODERADO ? "NORMAL" : v <= INDICE_ALTO ? "LEVEMENTE AMARELADA" : "COR DE CHA"; }
const char* palavraContaminacao(float v) {
  return v < 0.10f ? "MUITO PROXIMA DA AGUA LIMPA" : v < 0.30f ? "LEVEMENTE CONTAMINADA"
         : v < 0.60f ? "CONTAMINADA" : v <= 1.0f ? "MUITO CONTAMINADA" : "PIOR QUE A AGUA PODRE";
}

// Tudo que vai no JSON (e no relatorio) em palavras e porcentagens.
dashboard_api::Diagnostico diagnosticar(const Indices& x, const turbidez_config::Resultado& t,
                                        const ph_config::Resultado& p) {
  dashboard_api::Diagnostico d;
  for (uint8_t c = 0; c < CORES; ++c) d.porcentagem[c] = x.luz[c] * 100.0f;
  d.turbidez = palavraTurbidez(x, t);
  d.luzBloqueadaPct = x.poucaLuz ? NAN : x.turbidezCor * 100.0f;  // NAN vira null no JSON
  d.algas = palavraAlgas(x.algas);
  d.cor = palavraCor(x.cor);
  d.temContaminacao = x.temEscala;
  d.contaminacaoPct = x.contaminacao * 100.0f;
  d.contaminacao = palavraContaminacao(x.contaminacao);
  d.ph = !p.valido ? nullptr : p.temPh() ? ph_config::classificar(p.ph) : "SEM LEITURA";
  String turb = d.turbidez;
  turb.toLowerCase();
  String resumo = "Agua " + turb + ", " +
                  (x.algas < INDICE_MODERADO ? "sem sinal de algas" : x.algas <= INDICE_ALTO ? "com sinal fraco de algas" : "com sinal de algas") +
                  ", " + (x.cor < INDICE_MODERADO ? "cor normal" : x.cor <= INDICE_ALTO ? "levemente amarelada" : "cor de cha");
  if (p.temPh()) {
    String palavra = ph_config::classificar(p.ph);
    palavra.toLowerCase();
    resumo += ", pH " + String(p.ph, 1) + " (" + palavra + ")";
  }
  strlcpy(d.resumo, (resumo + ".").c_str(), sizeof(d.resumo));
  return d;
}

// ---- Relatorio no monitor ---------------------------------------------------------
void linha() { Serial.println(F("────────────────────────────────────────────────")); }

void barra(float fracao) {
  const int cheios = round(constrain(fracao, 0.0f, 1.0f) * 20);
  for (int i = 0; i < 20; ++i) Serial.print(i < cheios ? "█" : "░");
}

// Mensagem das medicoes antigas (guardadas sem internet) que foram reenviadas.
void imprimirReenvio(const dashboard_api::Reenvio& r) {
  if (r.enviadas > 0) Serial.printf(" ✓ %u medicoes guardadas tambem foram enviadas.\n", r.enviadas);
  if (r.recusadas > 0) Serial.printf(" ✗ %u medicoes guardadas estavam com defeito e foram descartadas.\n", r.recusadas);
  if (r.esperando > 0) {
    Serial.printf(" ⏳ Ainda faltam %u medicoes guardadas; envio nas proximas.\n", r.esperando);
  } else {
    Serial.println(F(" Tudo certo, nada pendente."));
  }
}

// Resultado do envio em palavras simples.
void imprimirEnvio(dashboard_api::Envio envio) {
  const char* motivo = dashboard_api::ultimoErro().c_str();
  switch (envio) {
    case dashboard_api::Envio::ENVIADO:
      if (!dashboard_api::temPendentes()) {
        Serial.println(F(" ✓ Enviado com sucesso. Tudo certo, nada pendente."));
      } else {
        Serial.println(F(" ✓ Enviado com sucesso. Enviando tambem as medicoes guardadas..."));
        imprimirReenvio(dashboard_api::reenviarPendentes());
      }
      break;
    case dashboard_api::Envio::GUARDADO:
      Serial.printf(" ✗ Nao enviado: %s.\n   A medicao ficou guardada e sera enviada sozinha depois.\n", motivo);
      break;
    case dashboard_api::Envio::FILA_CHEIA:
      Serial.printf(" ✗ Nao enviado: %s.\n   A memoria de espera encheu: a medicao ficou so no historico da placa.\n", motivo);
      break;
    case dashboard_api::Envio::RECUSADO: Serial.printf(" ✗ Nao enviado: %s.\n", motivo); break;
    case dashboard_api::Envio::SEM_DATA:
      Serial.println(F(" ✗ Nao enviado: o relogio ainda nao pegou a hora da internet.\n"
                       "   A medicao ficou no historico da placa."));
      break;
  }
}

void imprimirRelatorio(const char* titulo, const sensor_config::Resultado& r, const turbidez_config::Resultado& t,
                       const ph_config::Resultado& p, const Indices& x, const dashboard_api::Diagnostico& d) {
  char quando[17];
  if (!wifi_config::agora(quando, sizeof(quando), "%d/%m/%Y %H:%M")) strcpy(quando, "sem hora");

  Serial.println();
  linha();
  Serial.printf(" %s  ·  %s\n", titulo, quando);
  linha();
  if (d.temContaminacao) {
    Serial.printf(" Contaminacao  %.0f%%  %s\n LIMPA ", d.contaminacaoPct, d.contaminacao);
    barra(x.contaminacao);
    Serial.println(F(" SUJA\n"));
  }
  if (t.temNtuValidado()) {
    Serial.printf(" Turbidez  %s (%.0f NTU, sensor em %.2f V)\n", d.turbidez, t.ntu, t.mediaMv / 1000.0f);
  } else if (x.poucaLuz) {
    Serial.println(F(" Turbidez  SEM LEITURA (pouca luz no sensor de cor)"));
  } else {
    Serial.printf(" Turbidez  %s (sensor de cor: %.0f%% da luz bloqueada)\n", d.turbidez, x.turbidezCor * 100.0f);
  }
  // Sensor de turbidez: tensao e ADC crus sempre; NTU so como estimativa experimental.
  if (t.valido) {
    Serial.printf(" Sensor de turbidez  %.2f V (ADC %.0f, variacao %.0f mV) · %s", t.mediaMv / 1000.0f, t.adcBruto,
                  t.desvioMv, turbidez_config::nomeEstado(t.estado()));
    if (t.temNtu() && !turbidez_config::CALIBRACAO.validada) {
      Serial.printf(" · ~%.0f NTU experimental (calibracao pendente)", t.ntu);
    }
    Serial.println();
  }
  if (p.valido) {
    if (p.temPh()) {
      Serial.printf(" pH        %.2f  %s", p.ph, d.ph);
    } else {
      Serial.printf(" pH        SEM LEITURA (%s)", ph_config::nomeEstado(p.estado()));
    }
    Serial.printf("  (sensor %.3f V, variacao %.0f mV, calibracao %s)\n", p.mediaMv / 1000.0f, p.desvioMv,
                  ph_config::nomeCalibracao());
  }
  Serial.printf(" Algas     %s\n Cor       %s\n\n", d.algas, d.cor);

  Serial.println(F(" Cor            Onda    Valor   Luz que passou"));
  for (uint8_t c = 0; c < CORES; ++c) {
    Serial.printf(" %-13s %3u nm %6.0f  ", sensor_config::NOME_COR[c], sensor_config::ONDA_NM[c], r.media[c]);
    barra(x.luz[c]);
    Serial.printf(" %3.0f%%\n", d.porcentagem[c]);
  }
  if (r.saturado) Serial.println(F(" ⚠ Luz forte demais (sensor estourou): diminua o ganho, ex.: g 16"));
  if (x.poucaLuz) {
    Serial.printf(" ⚠ POUCA LUZ NO SENSOR (total %.0f, o normal sao milhares): cor, algas e turbidez nao valem.\n"
                  "   Confira se a lanterna esta acesa e alinhada (comando v) ou aumente o ganho (ex.: g 128).\n",
                  r.media[sensor_config::CLEAR]);
  }
  if (p.estado() == ph_config::Estado::SEM_SINAL) Serial.println(F(" ⚠ Sensor de pH SEM SINAL: confira os fios do Po e o divisor."));
  if (p.estado() == ph_config::Estado::INSTAVEL) Serial.println(F(" ⚠ pH ainda mudando: o eletrodo leva ate 2 min para estabilizar."));
  if (p.valido && ph_config::calibracao().pontos == 0) Serial.println(F(" ⚠ pH sem calibrar: use as solucoes padrao (comando ph 6.86, depois ph 4.01)."));
  if (t.semSinal()) Serial.println(F(" ⚠ Sensor de turbidez SEM SINAL: confira os fios e a chave D/A em \"A\"."));
  if (t.estado() == turbidez_config::Estado::INSTAVEL) {
    Serial.printf(" ⚠ Sensor de turbidez INSTAVEL (variou %.0f mV na medicao): bolhas na sonda ou mau contato.\n", t.desvioMv);
  }
  if (t.estado() == turbidez_config::Estado::FORA_DA_CURVA) {
    // Nao e defeito: o valor passou do inicio ou do fim da curva (2,5 V a 4,2 V).
    if (turbidez_config::tensaoCorrigidaV(t.mediaMv) > turbidez_config::CALIBRACAO.maxV) {
      Serial.printf(" Sensor de turbidez acima de %.1f V: agua mais clara que o inicio da curva (<= ~1 NTU, experimental).\n",
                    turbidez_config::CALIBRACAO.maxV);
    } else {
      Serial.printf(" Sensor de turbidez abaixo de %.1f V: alem do fim da curva (>= ~3000 NTU, experimental).\n",
                    turbidez_config::CALIBRACAO.minV);
    }
  }
  if (t.noMaximo()) {
    Serial.printf(" ⚠ Sensor de turbidez travado no maximo (%.2f V): turbidez estimada pelo sensor de cor.\n"
                  "   Confira a chave D/A em \"A\" e os resistores do divisor.\n", t.mediaMv / 1000.0f);
  }
  float maiorLuz = 0;
  for (uint8_t c = 0; c < CORES; ++c) maiorLuz = max(maiorLuz, x.luz[c]);
  if (temPadrao() && maiorLuz > 1.10f) {
    Serial.printf(" ⚠ LEITURA SUSPEITA: uma cor passou %.0f%% (mais luz que na agua limpa).\n"
                  "   Entrou luz de fora ou a lanterna mudou: feche a caixa ou regrave a agua limpa (BOOT -> 1).\n",
                  maiorLuz * 100.0f);
  }
  Serial.println();
}

// ---- Medir e enviar -------------------------------------------------------------
bool medirSensores(uint16_t esperaMs, sensor_config::Resultado& r, turbidez_config::Resultado& t,
                   ph_config::Resultado& p) {
  Serial.print(F("# Medindo"));
  if (!sensor_config::medir(as7341, Serial, r, esperaMs)) {
    Serial.println(F("# ERRO: o sensor de cor nao respondeu. Confira os fios."));
    return false;
  }
  t = turbidez_config::medir();
  p = ph_config::medir();
  return true;
}

// Calcula, mostra o relatorio, grava no historico e poe na fila de envio (nucleo 0).
void relatarEEnviar(const char* tipo, const char* nome, const char* titulo, const sensor_config::Resultado& r,
                    const turbidez_config::Resultado& t, const ph_config::Resultado& p) {
  const Indices x = calcular(r);
  dashboard_api::Medicao m;
  m.diagnostico = diagnosticar(x, t, p);
  imprimirRelatorio(titulo, r, t, p, x, m.diagnostico);
  m.tipo = tipo;
  strlcpy(m.amostra, nome, sizeof(m.amostra));
  m.ganho = sensor_config::ganhoEmX(estado.ganho);
  m.temData = wifi_config::agora(m.data, sizeof(m.data), "%Y-%m-%d", 50) &&
              wifi_config::agora(m.medidoEm, sizeof(m.medidoEm), "%Y-%m-%dT%H:%M:%S-03:00", 50);
  m.cor = r;
  m.turbidez = t;
  m.ph = p;
  if (!dashboard_api::registrar(m)) Serial.println(F(" ✗ Historico CHEIO: baixe com e e digite apagar sim"));
  if (xQueueSend(fila, &m, 0) != pdTRUE) {
    Serial.println(F(" ✗ Fila de envio cheia (internet muito lenta): esta medicao ficou so no historico da placa."));
  }

  // Hora da proxima medicao automatica (pela hora da internet, se ja tiver).
  time_t proxima = time(nullptr) + INTERVALO_MS / 1000;
  char hora[6];
  if (estado.automatico && proxima > 1700000000) {
    strftime(hora, sizeof(hora), "%H:%M", localtime(&proxima));
    Serial.printf(" Proxima medicao: %s\n", hora);
  }
  linha();
}

// Guarda a medicao como padrao de agua limpa, se a luz estiver boa.
bool salvarPadrao(const sensor_config::Resultado& r) {
  if (r.media[sensor_config::CLEAR] < sensor_config::LUZ_FRACA) {
    Serial.println(F(" ✗ Chegou pouca luz: a lanterna esta LIGADA e apontada? Padrao NAO salvo."));
    return false;
  }
  if (r.saturado) {
    Serial.println(F(" ✗ Luz forte demais: diminua o ganho (ex.: g 16). Padrao NAO salvo."));
    return false;
  }
  memcpy(estado.ref.padrao, r.media, sizeof(estado.ref.padrao));
  estado.ref.temPadrao = true;
  estado.ref.ganho = estado.ganho;
  salvarReferencia();
  Serial.println(F(" ✓ PADRAO DE AGUA LIMPA SALVO (100%)"));
  return true;
}

// Medicao automatica (ou comando m) da agua no pote (lagoa).
void medirAmostra() {
  if (!temPadrao()) {
    Serial.println(F("\n# Falta gravar a AGUA LIMPA: aperte o botao BOOT e escolha 1."));
    return;
  }
  sensor_config::Resultado r;
  turbidez_config::Resultado t;
  ph_config::Resultado p;
  if (!medirSensores(0, r, t, p)) return;
  char titulo[21];
  strcpy(titulo, estado.amostra);
  for (char* c = titulo; *c; ++c) *c = toupper(*c);
  relatarEEnviar("amostra", estado.amostra, titulo, r, t, p);
}

// ---- Botao e teclado -------------------------------------------------------------
// True uma vez por aperto (espera soltar).
bool botaoApertado() {
  if (digitalRead(PINO_BOTAO) == HIGH) return false;
  delay(30);  // ignora o "tremido" do contato
  if (digitalRead(PINO_BOTAO) == HIGH) return false;
  while (digitalRead(PINO_BOTAO) == LOW) delay(10);
  return true;
}

// Espera uma linha digitada (o botao vale como Enter). tempoMs = 0: espera para
// sempre. Tempo esgotado devolve "x" (= cancelar).
String lerLinha(uint32_t tempoMs) {
  while (Serial.available()) Serial.read();
  const uint32_t inicio = millis();
  String texto;
  while (tempoMs == 0 || millis() - inicio < tempoMs) {
    esp_task_wdt_reset();  // esperando a pessoa: nao e travamento
    if (botaoApertado()) return "";
    while (Serial.available()) {
      const char ch = Serial.read();
      if (ch == '\n') {
        texto.trim();
        texto.toLowerCase();
        return texto;
      }
      if (ch != '\r') texto += ch;
    }
    delay(20);
  }
  return "x";
}

bool esperarEnter(uint32_t tempoMs) { return lerLinha(tempoMs) != "x"; }

// ---- Gravar referencias (botao BOOT): agua limpa (0%) e agua suja (100%) --------
// Mostra o cabecalho, espera Enter e mede 3 vezes; devolve a media.
bool passo(const char* cabecalho, const char* instrucao, sensor_config::Resultado& r,
           turbidez_config::Resultado& t, ph_config::Resultado& p) {
  Serial.printf("\n==================================================\n %s\n"
                "==================================================\n %s\n"
                " Lanterna ACESA, mesma posicao, caixa fechada.\n"
                " Aperte Enter ou o botao quando estiver pronto (x + Enter = cancelar).\n",
                cabecalho, instrucao);
  if (!esperarEnter(0)) {
    Serial.println(F("# Cancelado."));
    return false;
  }
  r = sensor_config::Resultado();
  for (uint8_t c = 0; c < CANAIS; ++c) r.media[c] = r.desvio[c] = 0;
  t = turbidez_config::Resultado();
  p = ph_config::Resultado();
  for (uint8_t i = 1; i <= REPLICAS; ++i) {
    Serial.printf("   Medicao %u/3 ", i);
    sensor_config::Resultado ri;
    turbidez_config::Resultado ti;
    ph_config::Resultado pi;
    if (!medirSensores(i == 1 ? sensor_config::ESPERA_MS : 0, ri, ti, pi)) return false;
    for (uint8_t c = 0; c < CANAIS; ++c) {
      r.media[c] += ri.media[c] / REPLICAS;
      r.desvio[c] += ri.desvio[c] / REPLICAS;
    }
    r.saturado = r.saturado || ri.saturado;
    r.leituras += ri.leituras;
    t.valido = ti.valido;
    t.mediaMv += ti.mediaMv / REPLICAS;
    t.adcBruto += ti.adcBruto / REPLICAS;
    t.desvioMv = max(t.desvioMv, ti.desvioMv);  // o pior dos 3: instabilidade nao some na media
    p.valido = pi.valido;
    p.mediaMv += pi.mediaMv / REPLICAS;
    p.adcBruto += pi.adcBruto / REPLICAS;
    p.desvioMv = max(p.desvioMv, pi.desvioMv);
  }
  t.ntu = turbidez_config::ntuDaTensao(t.mediaMv);
  p.ph = ph_config::phDaTensao(p.mediaMv);
  return true;
}

// Agua limpa: vira o padrao (100% de luz) e o 0% de contaminacao.
bool gravarLimpa(const char* cabecalho) {
  sensor_config::Resultado r;
  turbidez_config::Resultado t;
  ph_config::Resultado p;
  if (!passo(cabecalho, "Coloque AGUA LIMPA no pote, ate a marca.", r, t, p) || !salvarPadrao(r)) return false;
  relatarEEnviar("branco", "agua_limpa", "AGUA LIMPA (referencia 0%)", r, t, p);
  return true;
}

// Agua suja/podre: vira o 100% de contaminacao. Precisa do padrao de agua limpa.
bool gravarSuja(const char* cabecalho) {
  if (!temPadrao()) {
    Serial.println(F(" ✗ Grave a agua limpa primeiro."));
    return false;
  }
  sensor_config::Resultado r;
  turbidez_config::Resultado t;
  ph_config::Resultado p;
  if (!passo(cabecalho, "Lave o pote e coloque a AGUA SUJA. Mexa bem.", r, t, p)) return false;
  memcpy(estado.ref.podre, calcular(r).absorb, sizeof(estado.ref.podre));
  estado.ref.temPodre = true;
  salvarReferencia();
  Serial.println(F(" ✓ REFERENCIA DE AGUA SUJA SALVA (100%)"));
  relatarEEnviar("amostra", "agua_podre", "AGUA SUJA (referencia 100%)", r, t, p);
  return true;
}

// Botao ou comando gravar: escolher o que regravar (mudou a luz, o pote, a agua...).
void menuGravar() {
  estado.aoVivo = false;
  Serial.println(F("\n================ REGRAVAR ================\n"
                   "   1   agua limpa  (padrao, 0%)\n"
                   "   2   agua suja   (referencia 100%)\n"
                   "   3   medir a amostra agora\n"
                   "   x   cancelar\n"
                   " Digite o numero e aperte Enter:"));
  const String opcao = lerLinha(60000);
  if (opcao == "1") {
    gravarLimpa("REGRAVAR: AGUA LIMPA (referencia 0%)");
  } else if (opcao == "2") {
    gravarSuja("REGRAVAR: AGUA SUJA (referencia 100%)");
  } else if (opcao == "3") {
    medirAmostra();
  } else {
    Serial.println(F("# Cancelado."));
  }
  estado.proximaMs = millis() + INTERVALO_MS;
}

// ---- Comandos -----------------------------------------------------------------
void ajuda() {
  Serial.println(F("\n=============== COMO USAR ===============\n"
                   " O aparelho mede sozinho a cada 10 segundos. Digite e aperte Enter:\n"
                   "   gravar     gravar agua limpa ou agua suja (ou aperte o botao BOOT)\n"
                   "   n NOME     nome da agua no pote (ex.: n lagoa)\n"
                   "   m          medir agora\n"
                   "   p          pausar / continuar as medicoes automaticas\n"
                   "   v          ver o sensor ao vivo (alinhar a lanterna)\n"
                   "   g N        ganho do sensor (1, 2, 4 ... 512)\n"
                   "   ph N       calibrar o pH na solucao padrao N (ex.: ph 6.86, ph 4.01)\n"
                   "   ph apagar  voltar o pH aos valores tipicos (sem calibracao)\n"
                   "   e          exportar o historico (CSV)\n"
                   "   apagar sim apagar o historico (baixe antes!)\n"
                   "========================================="));
}

void comando(char* texto) {
  const char* cmd = strtok(texto, " ");
  const char* arg = strtok(nullptr, " ");
  if (cmd == nullptr) return;

  if (!strcmp(cmd, "n")) {
    if (arg) strncpy(estado.amostra, arg, sizeof(estado.amostra) - 1);
    Serial.printf("# Agora medindo: %s\n", estado.amostra);
  } else if (!strcmp(cmd, "m")) {
    estado.aoVivo = false;
    medirAmostra();
    estado.proximaMs = millis() + INTERVALO_MS;
  } else if (!strcmp(cmd, "p")) {
    estado.automatico = !estado.automatico;
    estado.proximaMs = millis();
    Serial.println(estado.automatico ? F("# Medicoes automaticas: LIGADAS.") : F("# Medicoes automaticas: PAUSADAS."));
  } else if (!strcmp(cmd, "gravar")) {
    menuGravar();
  } else if (!strcmp(cmd, "v")) {
    estado.aoVivo = !estado.aoVivo;
    Serial.println(estado.aoVivo ? F("# Ao vivo: LIGADO (v para parar).") : F("# Ao vivo: DESLIGADO."));
  } else if (!strcmp(cmd, "g")) {
    as7341_gain_t novo;
    if (!arg || !sensor_config::ganhoDeX(atoi(arg), novo)) {
      Serial.println(F("# Use: g N, com N = 1, 2, 4, 8, 16, 32, 64, 128, 256 ou 512."));
      return;
    }
    estado.ganho = novo;
    as7341.setGain(novo);
    estado.ref.ganho = novo;
    estado.ref.temPadrao = false;  // o padrao foi medido com outro ganho
    salvarReferencia();
    Serial.printf("# Ganho: %uX. A proxima medicao (com agua limpa) vira o novo padrao.\n",
                  sensor_config::ganhoEmX(novo));
  } else if (!strcmp(cmd, "ph")) {
    estado.aoVivo = false;
    if (arg && !strcmp(arg, "apagar")) {
      ph_config::apagarCalibracao();
      Serial.println(F("# Calibracao do pH apagada (valores tipicos: pH 7 = 2,50 V)."));
    } else if (!arg || !ph_config::INSTALADO || !ph_config::calibrar(atof(arg), Serial)) {
      Serial.printf("# Use: ph N (N = pH da solucao padrao, de 1 a 13, ex.: ph 6.86). Calibracao atual: %s\n",
                    ph_config::nomeCalibracao());
    }
    estado.proximaMs = millis() + INTERVALO_MS;
  } else if (!strcmp(cmd, "e")) {
    dashboard_api::exportar(Serial);
  } else if (!strcmp(cmd, "apagar")) {
    if (arg && !strcmp(arg, "sim")) {
      dashboard_api::apagarHistorico(Serial);
      Serial.println(F("# Historico apagado."));
    } else {
      Serial.println(F("# Isso apaga o historico. Baixe antes (e)! Para confirmar: apagar sim"));
    }
  } else {
    ajuda();
  }
}

// Le o que foi digitado no monitor serial; ao chegar o Enter, executa o comando.
void lerTeclado() {
  static char texto[40];
  static uint8_t tamanho = 0;
  while (Serial.available()) {
    const char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      texto[tamanho] = '\0';
      if (tamanho > 0) comando(texto);
      tamanho = 0;
    } else if (tamanho < sizeof(texto) - 1) {
      texto[tamanho++] = ch;
    }
  }
}

// Modo ao vivo: uma leitura rapida por linha, para alinhar a lanterna (nao vai para a API).
void aoVivo() {
  static const char* const ROTULO[CANAIS] = {"Vio", "Azu", "Cia", "Ver", "VeA", "Ama", "Lar", "Vrm", "Total", "IR"};
  uint16_t v[CANAIS];
  if (!sensor_config::lerCanais(as7341, v)) return;
  bool saturado = false;
  for (uint8_t c = 0; c < CANAIS; ++c) {
    Serial.printf("%s %u | ", ROTULO[c], v[c]);
    saturado = saturado || v[c] >= sensor_config::SATURACAO;
  }
  if (turbidez_config::INSTALADO) {
    float bruto = 0;
    const float mv = turbidez_config::lerMv(&bruto);
    const float ntu = turbidez_config::ntuDaTensao(mv);
    Serial.printf("Turbidez %.2f V (ADC %.0f)", mv / 1000.0f, bruto);
    if (!isnan(ntu)) Serial.printf(" ~%.0f NTU experimental", ntu);
  }
  if (ph_config::INSTALADO) {
    const float mv = ph_config::lerMv();
    Serial.printf(" | pH %.2f (%.3f V)", ph_config::phDaTensao(mv), mv / 1000.0f);
  }
  Serial.println(saturado ? F("  <- LUZ FORTE DEMAIS") : F(""));
}

// ---- Nucleo 0: tarefa que envia para a API ------------------------------------------
// Fica esperando a fila. Cada medicao que chega e enviada (ou guardada na flash se
// estiver sem internet); a cada 5 min tenta de novo as guardadas.
void tarefaEnvio(void*) {
  esp_task_wdt_add(nullptr);  // o vigia tambem cuida desta tarefa
  uint32_t ultimaConexao = millis();
  uint32_t reenvioMs = millis();
  dashboard_api::Medicao m;
  while (true) {
    esp_task_wdt_reset();
    const bool chegou = xQueueReceive(fila, &m, pdMS_TO_TICKS(1000)) == pdTRUE;

    // Sem Wi-Fi: tenta reconectar no maximo 1 vez por minuto (leva ate 20 s);
    // enquanto isso as medicoes vao direto para a fila da flash, sem esperar.
    if (!wifi_config::conectado() && millis() - ultimaConexao >= RETENTAR_WIFI_MS) {
      ultimaConexao = millis();
      wifi_config::conectar(Serial, false);
      esp_task_wdt_reset();
    }
    if (chegou) {
      // Envia primeiro e so depois imprime: o envio leva segundos e nao corta o relatorio.
      const dashboard_api::Envio envio = dashboard_api::enviar(m);
      Serial.printf("\n# Envio %s (%s %.8s):", m.amostra, m.tipo, m.temData ? m.medidoEm + 11 : "sem hora");
      imprimirEnvio(envio);
    }
    // Medicoes guardadas sem internet: tenta de novo de tempos em tempos.
    if (millis() - reenvioMs >= REENVIO_MS) {
      reenvioMs = millis();
      if (dashboard_api::temPendentes() && wifi_config::conectado()) {
        Serial.println(F("\n# Enviando as medicoes guardadas sem internet..."));
        imprimirReenvio(dashboard_api::reenviarPendentes());
      }
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  esp_task_wdt_init(WATCHDOG_S, true);  // vigia: se o programa travar, a placa reinicia
  esp_task_wdt_add(nullptr);
  Serial.println(F("\n# Ligacoes: AS7341 VIN=3V3 SDA=21 SCL=22 · Turbidez VCC=VIN(5V) OUT=divisor->GPIO 34"
                   " · pH V+=VIN(5V) Po=divisor->GPIO 35"));
  turbidez_config::iniciar();
  if (!sensor_config::iniciar(as7341)) {
    Serial.println(F("# ERRO: AS7341 nao encontrado. Confira os fios."));
    while (true) delay(1000);
  }
  Serial.println(F("# Sensores prontos."));

  dashboard_api::iniciar(Serial);  // memoria flash: fila e historico
  ph_config::iniciar();            // depois da flash: le a calibracao salva
  wifi_config::iniciar(Serial);    // Wi-Fi e hora

  // Fila entre os nucleos e a tarefa de envio no nucleo 0 (pilha grande: HTTPS).
  fila = xQueueCreate(TAMANHO_FILA, sizeof(dashboard_api::Medicao));
  if (fila == nullptr || xTaskCreatePinnedToCore(tarefaEnvio, "envio", 12288, nullptr, 1, nullptr, NUCLEO_ENVIO) != pdPASS) {
    Serial.println(F("# ERRO: sem memoria para a fila de envio."));
    while (true) delay(1000);
  }
  carregarReferencia();
  if (estado.ref.temPadrao) {  // volta o ganho usado no padrao salvo
    estado.ganho = estado.ref.ganho;
    as7341.setGain(estado.ganho);
  }
  Serial.printf("# Ganho: %uX  ·  Padrao de agua limpa: %s  ·  Historico: %u medicoes\n",
                sensor_config::ganhoEmX(estado.ganho), temPadrao() ? "OK" : "NAO EXISTE",
                dashboard_api::linhasHistorico());
  ajuda();
  if (!temPadrao()) Serial.println(F("# Falta gravar a AGUA LIMPA: aperte o botao BOOT e escolha 1."));
  if (!estado.ref.temPodre) Serial.println(F("# Falta gravar a AGUA SUJA: aperte o botao BOOT e escolha 2."));
  if (ph_config::INSTALADO) Serial.printf("# Calibracao do pH: %s\n", ph_config::nomeCalibracao());
}

// loop() roda no nucleo 1: so mede, mostra e poe na fila (o envio e no nucleo 0).
void loop() {
  esp_task_wdt_reset();  // "estou vivo"
  lerTeclado();
  if (botaoApertado()) menuGravar();

  if (estado.aoVivo) {
    aoVivo();
    return;
  }

  // Medicao automatica (a subtracao funciona mesmo quando millis() da a volta).
  if (estado.automatico && static_cast<int32_t>(millis() - estado.proximaMs) >= 0) {
    estado.proximaMs = millis() + INTERVALO_MS;
    medirAmostra();
  }
}
