// =============================================================================
// dashboard_api.h — envia as medicoes para a API (back-end / dashboard)
// -----------------------------------------------------------------------------
// Cada medicao:
//   1. vira um JSON no formato combinado com o back-end (docs/comunicacao_api_esp32.pdf);
//   2. vai por POST para {API_URL}/api/leituras/ com o header X-API-Key:
//        2xx -> ENVIADO
//        sem internet / erro 5xx -> GUARDADO em /pendentes.jsonl e reenviado depois
//        4xx (chave errada, JSON invalido) -> RECUSADO (reenviar nao resolve)
//   3. tambem vira uma linha em /historico.csv na placa (backup de TUDO,
//      exportado com o comando e e lido por tools/gerar_relatorio.py).
// Os dois arquivos ficam na memoria flash (LittleFS): nao somem ao desligar.
// =============================================================================

#ifndef DASHBOARD_API_H
#define DASHBOARD_API_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include "secrets.h"
#include "sensor_config.h"
#include "turbidez_config.h"
#include "wifi_config.h"

namespace dashboard_api {

constexpr char PENDENTES[] = "/pendentes.jsonl";
constexpr char HISTORICO[] = "/historico.csv";
constexpr uint16_t TIMEOUT_MS = 30000;               // Render gratis pode levar ~30 s
constexpr size_t LIMITE_PENDENTES = 300UL * 1024UL;  // ~500 medicoes esperando envio
constexpr size_t LIMITE_HISTORICO = 900UL * 1024UL;  // ~6.500 medicoes = ~4,5 dias
constexpr uint8_t REENVIOS_POR_VEZ = 10;

enum class Envio { ENVIADO, GUARDADO, FILA_CHEIA, RECUSADO, SEM_DATA };

// Tudo que vai no JSON de uma medicao.
struct Medicao {
  const char* tipo;     // escuro | branco (padrao de agua limpa) | amostra
  const char* amostra;  // nome da agua no pote (comando n), ex.: lagoa
  uint8_t replica;      // 1 a 255
  uint16_t ganho;
  const sensor_config::Resultado* cor;
  const turbidez_config::Resultado* turbidez;
};

inline size_t& linhasHistorico() {
  static size_t total = 0;
  return total;
}

inline size_t tamanho(const char* caminho) {
  File f = LittleFS.open(caminho, FILE_READ);
  const size_t t = f ? f.size() : 0;
  if (f) f.close();
  return t;
}

// Abre a memoria flash, cria o historico (se nao existir) e conta as linhas.
inline void iniciar(Stream& serial) {
  if (!LittleFS.begin(true)) {
    serial.println(F("# ERRO: memoria flash indisponivel (sem fila e sem historico)."));
    return;
  }
  if (!LittleFS.exists(HISTORICO)) {
    File f = LittleFS.open(HISTORICO, FILE_WRITE);
    f.println(F("tipo,amostra,data_hora,uptime_s,ganho,f1_415,f2_445,f3_480,f4_515,f5_555,"
                "f6_590,f7_630,f8_680,clear,nir,turbidez_mv,saturado,cv_max"));
    f.close();
  }
  File f = LittleFS.open(HISTORICO, FILE_READ);
  size_t quebras = 0;
  uint8_t bloco[256];
  while (f.available()) {
    const size_t lidos = f.read(bloco, sizeof(bloco));
    for (size_t i = 0; i < lidos; ++i) quebras += bloco[i] == '\n';
  }
  f.close();
  linhasHistorico() = quebras > 0 ? quebras - 1 : 0;  // tira o cabecalho
}

// ---- JSON ---------------------------------------------------------------------
inline String montarJson(const Medicao& m, const char* data) {
  char medidoEm[26];
  wifi_config::agora(medidoEm, sizeof(medidoEm), "%Y-%m-%dT%H:%M:%S-03:00");

  JsonDocument doc;
  doc["data_coleta"] = data;
  if (medidoEm[0] != '\0') doc["medido_em"] = medidoEm;  // hora real da medicao
  doc["tipo"] = m.tipo;
  doc["amostra"] = m.amostra;
  doc["led"] = strcmp(m.tipo, "escuro") == 0 ? "nenhum" : "frente";
  doc["replica"] = m.replica;
  doc["ganho"] = m.ganho;
  doc["atime"] = sensor_config::ATIME;
  doc["astep"] = sensor_config::ASTEP;
  doc["n"] = m.cor->leituras;
  doc["saturado"] = m.cor->saturado;
  JsonObject media = doc["media"].to<JsonObject>();
  JsonObject desvio = doc["desvio"].to<JsonObject>();
  for (uint8_t c = 0; c < sensor_config::CANAIS; ++c) {
    media[sensor_config::NOME_CANAL[c]] = serialized(String(m.cor->media[c], 1));
    desvio[sensor_config::NOME_CANAL[c]] = serialized(String(m.cor->desvio[c], 1));
  }
  if (m.turbidez->valido) {
    doc["turbidez_mv"] = serialized(String(m.turbidez->mediaMv, 1));
    doc["dp_turbidez_mv"] = serialized(String(m.turbidez->desvioMv, 1));
  } else {
    doc["turbidez_mv"] = nullptr;
    doc["dp_turbidez_mv"] = nullptr;
  }
  String json;
  serializeJson(doc, json);
  return json;
}

// ---- Envio e fila ---------------------------------------------------------------
// Codigo HTTP da resposta, ou negativo se nem chegou na API.
inline int post(Stream& serial, const String& json) {
  if (!wifi_config::conectado()) return -1;  // sem Wi-Fi: vai para a fila
  const String url = String(secrets::API_URL) + "/api/leituras/";
  WiFiClientSecure seguro;  // https:// (Render)
  WiFiClient simples;       // http:// (API rodando num computador, para testar)
  seguro.setInsecure();     // criptografa, mas nao confere o certificado
  HTTPClient http;
  if (!(url.startsWith("https://") ? http.begin(seguro, url) : http.begin(simples, url))) return -1;
  http.setTimeout(TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", secrets::API_KEY);
  const int codigo = http.POST(json);
  if (codigo >= 400 && codigo < 500) {
    serial.print(F("# O servidor respondeu: "));
    serial.println(http.getString().substring(0, 300));
  }
  http.end();
  return codigo;
}

inline bool tentarDeNovo(int codigo) { return codigo < 0 || codigo >= 500; }

inline size_t contarPendentes() {
  File f = LittleFS.open(PENDENTES, FILE_READ);
  if (!f) return 0;
  size_t total = 0;
  while (f.available()) total += f.readStringUntil('\n').length() > 1;
  f.close();
  return total;
}

// Tenta mandar ate 10 pendentes. Para na primeira falha de rede; o resto fica.
inline void reenviarPendentes(Stream& serial) {
  if (!LittleFS.exists(PENDENTES) || !wifi_config::conectado()) return;
  File f = LittleFS.open(PENDENTES, FILE_READ);
  String restantes;
  uint8_t tentativas = 0;
  uint16_t enviadas = 0;
  bool falhou = false;
  while (f.available()) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.isEmpty()) continue;
    if (falhou || tentativas >= REENVIOS_POR_VEZ) {
      restantes += linha + '\n';
      continue;
    }
    ++tentativas;
    const int codigo = post(serial, linha);
    if (tentarDeNovo(codigo)) {
      restantes += linha + '\n';
      falhou = true;
    } else if (codigo < 300) {
      ++enviadas;
    }
  }
  f.close();
  LittleFS.remove(PENDENTES);
  if (restantes.length() > 0) {
    File novo = LittleFS.open(PENDENTES, FILE_WRITE);
    novo.print(restantes);
    novo.close();
  }
  if (enviadas > 0) {
    serial.print(F("# "));
    serial.print(enviadas);
    serial.println(F(" medicoes antigas (guardadas sem internet) foram enviadas."));
  }
}

inline Envio enviar(Stream& serial, const Medicao& m) {
  char data[11];
  if (!wifi_config::agora(data, sizeof(data), "%Y-%m-%d", 2000)) {
    return Envio::SEM_DATA;  // sem data a API nao sabe a qual coleta pertence
  }
  const String json = montarJson(m, data);
  const int codigo = post(serial, json);
  if (codigo >= 200 && codigo < 300) {
    reenviarPendentes(serial);  // internet ok: aproveita e manda as antigas
    return Envio::ENVIADO;
  }
  if (tentarDeNovo(codigo)) {
    if (tamanho(PENDENTES) + json.length() > LIMITE_PENDENTES) return Envio::FILA_CHEIA;
    File f = LittleFS.open(PENDENTES, FILE_APPEND);
    f.println(json);
    f.close();
    return Envio::GUARDADO;
  }
  return Envio::RECUSADO;
}

// ---- Historico CSV (backup de tudo, independe da internet) -----------------------
inline bool registrar(const Medicao& m) {
  char quando[20];
  wifi_config::agora(quando, sizeof(quando), "%Y-%m-%d %H:%M:%S", 50);
  float cvMax = 0;  // maior variacao (%) entre as 20 leituras, nas 8 cores
  for (uint8_t c = 0; c < sensor_config::CORES; ++c) {
    if (m.cor->media[c] > 0) cvMax = max(cvMax, m.cor->desvio[c] / m.cor->media[c] * 100.0f);
  }
  String nome = m.amostra;
  nome.replace(',', '_');  // virgula quebraria as colunas

  String linha = strcmp(m.tipo, "branco") == 0 ? "padrao" : m.tipo;
  linha += ","; linha += nome;
  linha += ","; linha += quando;
  linha += ","; linha += millis() / 1000UL;
  linha += ","; linha += m.ganho;
  for (uint8_t c = 0; c < sensor_config::CANAIS; ++c) {
    linha += ",";
    linha += String(m.cor->media[c], 1);
  }
  linha += ",";
  if (m.turbidez->valido) linha += String(m.turbidez->mediaMv, 0);
  linha += m.cor->saturado ? ",1," : ",0,";
  linha += String(cvMax, 2);

  if (tamanho(HISTORICO) + linha.length() + 2 > LIMITE_HISTORICO) return false;
  File f = LittleFS.open(HISTORICO, FILE_APPEND);
  if (!f) return false;
  f.println(linha);
  f.close();
  ++linhasHistorico();
  return true;
}

// Comando e: imprime o CSV inteiro para copiar do monitor (ou de logs/).
inline void exportar(Stream& serial) {
  File f = LittleFS.open(HISTORICO, FILE_READ);
  if (!f) return;
  serial.println(F("===== INICIO DO HISTORICO (copie daqui ate o FIM) ====="));
  uint8_t bloco[256];
  while (f.available()) serial.write(bloco, f.read(bloco, sizeof(bloco)));
  f.close();
  serial.println(F("===== FIM DO HISTORICO ====="));
}

inline void apagarHistorico(Stream& serial) {
  LittleFS.remove(HISTORICO);
  iniciar(serial);
}

}  // namespace dashboard_api

#endif
