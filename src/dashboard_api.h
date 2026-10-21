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
constexpr char FIRMWARE_VERSAO[] = "2026.10.21-turbidez-v2";  // vai no JSON (firmware_version)
constexpr size_t MAX_LINHA = 2048;                   // uma medicao em JSON tem ~1,3 KB
constexpr size_t LIMITE_RESTANTES = 60UL * 1024UL;  // o que cabe na RAM ao reescrever a fila

enum class Envio { ENVIADO, GUARDADO, FILA_CHEIA, RECUSADO, SEM_DATA };

// Diagnostico calculado no main.cpp (comparando com o padrao de agua limpa).
struct Diagnostico {
  float porcentagem[sensor_config::CORES];  // % de luz que passou em cada cor
  const char* turbidez;                     // LIMPA | LEVEMENTE TURVA | TURVA | MUITO TURVA
  float luzBloqueadaPct;                    // turbidez pelo AS7341 (590-630 nm)
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
  if (!LittleFS.exists(caminho)) return 0;  // evita o aviso "does not exist" no monitor
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
  doc["firmware_version"] = FIRMWARE_VERSAO;
  doc["tipo"] = m.tipo;
  doc["amostra"] = m.amostra;

  // Canais crus e desvio padrao (barras de erro no dashboard), ganho e saturacao.
  doc["ganho"] = m.ganho;
  doc["saturado"] = m.cor->saturado;
  JsonObject media = doc["media"].to<JsonObject>();
  JsonObject desvio = doc["desvio"].to<JsonObject>();
  for (uint8_t c = 0; c < sensor_config::CANAIS; ++c) {
    char chave[8];
    if (c < sensor_config::CORES) {
      snprintf(chave, sizeof(chave), "f%u_%u", c + 1, sensor_config::ONDA_NM[c]);
    } else {
      strcpy(chave, c == sensor_config::CLEAR ? "clear" : "nir");
    }
    media[chave] = serialized(String(m.cor->media[c], 1));
    desvio[chave] = serialized(String(m.cor->desvio[c], 1));
  }

  // Sensor de turbidez: dados crus sempre (quando ligado) + estado + calibracao.
  //   ntu              = NTU VALIDADO (null enquanto a calibracao nao for validada)
  //   ntu_experimental = estimativa pela curva do fabricante (nao validada)
  const turbidez_config::Resultado& t = *m.turbidez;
  JsonObject turbidez = doc["turbidez"].to<JsonObject>();
  turbidez["status"] = turbidez_config::nomeEstado(t.estado());
  turbidez["calibracao_id"] = turbidez_config::CALIBRACAO.id;
  turbidez["calibracao_status"] = turbidez_config::CALIBRACAO.validada ? "validada" : "pendente";
  if (t.valido) {
    turbidez["adc_raw"] = serialized(String(t.adcBruto, 0));
    turbidez["tensao_v"] = serialized(String(t.mediaMv / 1000.0f, 3));
    turbidez["desvio_mv"] = serialized(String(t.desvioMv, 0));
  } else {
    turbidez["adc_raw"] = nullptr;
    turbidez["tensao_v"] = nullptr;
    turbidez["desvio_mv"] = nullptr;
  }
  if (t.temNtu()) {
    turbidez["ntu_experimental"] = serialized(String(t.ntu, 1));
  } else {
    turbidez["ntu_experimental"] = nullptr;
  }
  if (t.temNtuValidado()) {
    turbidez["ntu"] = serialized(String(t.ntu, 1));
  } else {
    turbidez["ntu"] = nullptr;
  }
  if (isnan(d.luzBloqueadaPct)) {
    turbidez["luz_bloqueada_pct"] = nullptr;  // pouca luz no sensor: nao deu para medir
  } else {
    turbidez["luz_bloqueada_pct"] = serialized(String(d.luzBloqueadaPct, 1));  // pelo AS7341
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
    ultimoErro() = "sem internet (Wi-Fi desconectado)";
    return -1;  // vai para a fila
  }
  const String url = String(secrets::API_URL) + "/api/leituras/";
  WiFiClientSecure seguro;  // https:// (Render)
  WiFiClient simples;       // http:// (API rodando num computador, para testar)
  seguro.setInsecure();     // criptografa, mas nao confere o certificado
  HTTPClient http;
  if (!(url.startsWith("https://") ? http.begin(seguro, url) : http.begin(simples, url))) {
    ultimoErro() = "endereco da API invalido (confira o secrets.h)";
    return -1;
  }
  http.setTimeout(TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", secrets::API_KEY);
  const int codigo = http.POST(json);
  // Motivo da falha em palavras simples.
  if (codigo < 0) {
    ultimoErro() = "o dashboard nao respondeu (pode estar ligando, tenta de novo depois)";
  } else if (codigo == 401 || codigo == 403) {
    ultimoErro() = "a chave da API esta errada (confira o secrets.h e o Render)";
  } else if (codigo >= 500) {
    ultimoErro() = "o dashboard esta com problema no momento";
  } else if (codigo >= 300) {
    // A API explica o motivo em {"erro": "..."}; mostra so a frase.
    const String corpo = http.getString();
    JsonDocument resposta;
    const bool lido = !deserializeJson(resposta, corpo);
    const char* motivo = lido ? (resposta["erro"] | resposta["detail"] | "") : "";
    ultimoErro() = String("o dashboard recusou os dados: ") + (*motivo ? motivo : corpo.substring(0, 150).c_str());
  }
  http.end();
  return codigo;
}

inline bool tentarDeNovo(int codigo) { return codigo < 0 || codigo >= 500; }

inline bool temPendentes() { return tamanho(PENDENTES) > 0; }

struct Reenvio {
  uint16_t enviadas = 0;
  uint16_t recusadas = 0;
  uint16_t esperando = 0;
};

// Manda ate 10 medicoes guardadas sem internet. Para na primeira falha de rede;
// as outras ficam para a proxima vez. Linhas recusadas (4xx) sao descartadas.
inline Reenvio reenviarPendentes() {
  Reenvio r;
  if (!temPendentes() || !wifi_config::conectado()) return r;
  static char linha[MAX_LINHA];  // tamanho fixo: arquivo estragado nao trava nem estoura a memoria
  File f = LittleFS.open(PENDENTES, FILE_READ);
  String restantes;
  uint8_t tentativas = 0;
  bool falhou = false;
  while (f.available()) {
    esp_task_wdt_reset();  // cada envio pode levar ate 30 s
    const size_t antes = f.position();
    const size_t n = f.readBytesUntil('\n', linha, sizeof(linha) - 1);
    if (f.position() == antes) break;  // arquivo corrompido nao avanca: descarta o resto
    linha[n] = '\0';
    String json(linha);
    json.trim();
    if (json.isEmpty()) continue;
    if (falhou || tentativas >= REENVIOS_POR_VEZ) {
      if (restantes.length() + json.length() < LIMITE_RESTANTES) {
        restantes += json + '\n';
        ++r.esperando;
      }
      continue;
    }
    ++tentativas;
    const int codigo = post(json);
    if (tentarDeNovo(codigo)) {
      restantes += json + '\n';
      ++r.esperando;
      falhou = true;
    } else if (codigo < 300) {
      ++r.enviadas;
    } else {
      ++r.recusadas;
    }
  }
  f.close();
  LittleFS.remove(PENDENTES);
  if (restantes.length() > 0) {
    File novo = LittleFS.open(PENDENTES, FILE_WRITE);
    novo.print(restantes);
    novo.close();
  }
  return r;
}

inline Envio enviar(const Medicao& m) {
  char data[11];
  if (!wifi_config::agora(data, sizeof(data), "%Y-%m-%d", 2000)) {
    return Envio::SEM_DATA;  // sem data a API nao sabe a qual coleta pertence
  }
  const String json = montarJson(m, data);
  const int codigo = post(json);
  if (codigo >= 200 && codigo < 300) return Envio::ENVIADO;
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
