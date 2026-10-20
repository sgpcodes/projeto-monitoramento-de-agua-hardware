// =============================================================================
// dashboard_api.h — envia as medicoes para a API (back-end / dashboard)
// -----------------------------------------------------------------------------
// Cada medicao:
//   1. vira um JSON: as 8 cores (valor e %), a turbidez em NTU e o diagnostico;
//   2. vai por POST para {API_URL}/api/leituras/ com o header X-API-Key:
//        2xx -> ENVIADO
//        sem internet / erro 5xx -> GUARDADO em /pendentes.jsonl e reenviado depois
//        4xx (chave errada, JSON invalido) -> RECUSADO (reenviar nao resolve)
//   3. tambem vira uma linha em /historico.csv na placa (backup de TUDO,
//      exportado com o comando e).
// Os dois arquivos ficam na memoria flash (LittleFS): nao somem ao desligar.
// =============================================================================

#ifndef DASHBOARD_API_H
#define DASHBOARD_API_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>

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

// Diagnostico calculado no main.cpp (comparando com o padrao de agua limpa).
struct Diagnostico {
  float porcentagem[sensor_config::CORES];  // % de luz que passou em cada cor
  const char* turbidez;                     // LIMPA | LEVEMENTE TURVA | TURVA | MUITO TURVA
  const char* algas;                        // NAO DETECTADAS | SINAL FRACO | PRESENTES
  const char* cor;                          // NORMAL | LEVEMENTE AMARELADA | COR DE CHA
  bool temContaminacao;                     // so depois do ensaio (agua podre medida)
  float contaminacaoPct;                    // 0 = agua limpa, 100 = agua podre
  const char* contaminacao;                 // em palavras
  String resumo;                            // ex.: "Agua turva, sem sinal de algas, cor normal."
};

// Tudo que vai no JSON de uma medicao.
struct Medicao {
  const char* tipo;     // branco (agua limpa de referencia) | amostra
  const char* amostra;  // nome da agua no pote (comando n), ex.: lagoa
  uint16_t ganho;       // so para o historico CSV
  const sensor_config::Resultado* cor;
  const turbidez_config::Resultado* turbidez;
  const Diagnostico* diagnostico;
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

  const Diagnostico& d = *m.diagnostico;

  JsonDocument doc;
  doc["data_coleta"] = data;
  doc["medido_em"] = medidoEm;  // hora real da medicao
  doc["tipo"] = m.tipo;
  doc["amostra"] = m.amostra;

  // Canais crus (o dashboard calcula os graficos a partir deles), ganho e saturacao.
  doc["ganho"] = m.ganho;
  doc["saturado"] = m.cor->saturado;
  JsonObject media = doc["media"].to<JsonObject>();
  for (uint8_t c = 0; c < sensor_config::CANAIS; ++c) {
    char chave[8];
    if (c < sensor_config::CORES) {
      snprintf(chave, sizeof(chave), "f%u_%u", c + 1, sensor_config::ONDA_NM[c]);
    } else {
      strcpy(chave, c == sensor_config::CLEAR ? "clear" : "nir");
    }
    media[chave] = serialized(String(m.cor->media[c], 1));
  }

  // Turbidez em NTU (null se o sensor de turbidez nao estiver ligado).
  JsonObject turbidez = doc["turbidez"].to<JsonObject>();
  if (m.turbidez->temNtu()) {
    turbidez["ntu"] = serialized(String(m.turbidez->ntu, 1));
    turbidez["tensao_v"] = serialized(String(m.turbidez->mediaMv / 1000.0f, 2));
  } else {
    turbidez["ntu"] = nullptr;
    turbidez["tensao_v"] = nullptr;
  }
  turbidez["classificacao"] = d.turbidez;

  // As 8 cores: nome, comprimento de onda, numero captado e % de luz que passou.
  JsonArray cores = doc["cores"].to<JsonArray>();
  for (uint8_t c = 0; c < sensor_config::CORES; ++c) {
    JsonObject cor = cores.add<JsonObject>();
    cor["nome"] = sensor_config::NOME_COR[c];
    cor["comprimento_onda_nm"] = sensor_config::ONDA_NM[c];
    cor["valor"] = static_cast<uint32_t>(round(m.cor->media[c]));
    cor["porcentagem"] = serialized(String(d.porcentagem[c], 1));
  }

  // Diagnostico em palavras.
  JsonObject diag = doc["diagnostico"].to<JsonObject>();
  diag["turbidez"] = d.turbidez;
  diag["algas"] = d.algas;
  diag["cor"] = d.cor;
  if (d.temContaminacao) {
    diag["contaminacao_pct"] = static_cast<int>(round(d.contaminacaoPct));
    diag["contaminacao"] = d.contaminacao;
  } else {
    diag["contaminacao_pct"] = nullptr;
    diag["contaminacao"] = nullptr;
  }
  diag["resumo"] = d.resumo;

  String json;
  serializeJson(doc, json);
  return json;
}

// ---- Envio e fila ---------------------------------------------------------------
// Motivo da ultima falha de envio, para mostrar no monitor.
inline String& ultimoErro() {
  static String erro;
  return erro;
}

// Codigo HTTP da resposta, ou negativo se nem chegou na API.
inline int post(const String& json) {
  if (!wifi_config::conectado()) {
    ultimoErro() = "sem Wi-Fi";
    return -1;  // vai para a fila
  }
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
  if (codigo < 0) {
    ultimoErro() = "o servidor nao respondeu (" + HTTPClient::errorToString(codigo) + ")";
  } else if (codigo >= 300) {
    ultimoErro() = "HTTP " + String(codigo) + ": " + http.getString().substring(0, 200);
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
    esp_task_wdt_reset();  // cada envio pode levar ate 30 s
    const size_t antes = f.position();
    String linha = f.readStringUntil('\n');
    if (f.position() == antes) break;  // arquivo corrompido nao avanca: descarta o resto
    linha.trim();
    if (linha.isEmpty()) continue;
    if (falhou || tentativas >= REENVIOS_POR_VEZ) {
      restantes += linha + '\n';
      continue;
    }
    ++tentativas;
    const int codigo = post(linha);
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
  const int codigo = post(json);
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

  String linha = m.tipo;
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
