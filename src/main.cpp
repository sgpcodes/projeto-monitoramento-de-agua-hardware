// =============================================================================
// main.cpp — o aparelho: mede a agua, mostra o relatorio e envia para a API
// -----------------------------------------------------------------------------
// Arquivos:
//   sensor_config.h    sensor de cor AS7341
//   turbidez_config.h  sensor de turbidez (em NTU)
//   wifi_config.h      Wi-Fi e hora da internet
//   dashboard_api.h    JSON, envio para a API, fila sem internet e historico CSV
//   secrets.h          senha do Wi-Fi, link e chave da API (fora do git)
//
// Uso: aperte o botao BOOT (ou digite gravar) para gravar a agua limpa (0%) e a
// agua suja (100%); ficam salvas mesmo desligando. Fora isso, o aparelho mede a
// lagoa sozinho a cada 1 minuto, sem parar, e envia para o dashboard.
//
// Contas, com o padrao de agua limpa como 100% de luz:
//   % de luz que passou = amostra / padrao      absorbancia A = -log10(fracao)
//   turbidez = NTU do sensor de turbidez (ou, sem ele, 1 - luz no amarelo 590 nm)
//   algas = A680 - A590      cor (materia organica) = A415 - A555
//   contaminacao = (A . A_podre) / (A_podre . A_podre)   0% = limpa, 100% = podre
// As palavras (LIMPA, TURVA...) ajudam a leitura; nao sao laudo de laboratorio.
// =============================================================================

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>

#include "dashboard_api.h"
#include "sensor_config.h"
#include "turbidez_config.h"
#include "wifi_config.h"

namespace {

using sensor_config::CANAIS;
using sensor_config::CORES;

constexpr uint32_t INTERVALO_MS = 60UL * 1000UL;       // medicao automatica: 1 minuto
constexpr uint32_t REENVIO_MS = 5UL * 60UL * 1000UL;   // tenta a fila a cada 5 min
constexpr uint32_t WATCHDOG_S = 180;                   // travou 3 min? reinicia sozinho
constexpr uint8_t REPLICAS = 3;                        // medicoes ao gravar limpa/suja
constexpr char ARQUIVO_REFERENCIA[] = "/referencia.bin";
// Botao BOOT da placa. Para um botao externo: ligue entre GPIO 4 e GND e mude para 4.
constexpr uint8_t PINO_BOTAO = 0;

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
  uint32_t reenvioMs = 0;
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
  float turbidezCor;    // 0 a 1, usado so se o sensor de turbidez nao estiver ligado
  float algas, cor;
  bool temEscala = false;
  float contaminacao = 0;  // 0 = limpa, 1 = igual a podre
};

Indices calcular(const sensor_config::Resultado& r) {
  const Referencia& ref = estado.ref;
  Indices x;
  for (uint8_t c = 0; c < CORES; ++c) {
    x.luz[c] = ref.padrao[c] > 0 ? constrain(r.media[c] / ref.padrao[c], 0.0f, 1.5f) : 0;
    x.absorb[c] = -log10(max(x.luz[c], 0.001f));
  }
  x.turbidezCor = 1.0f - x.luz[5];      // amarelo 590 nm: pouco afetado por algas e cor
  x.algas = x.absorb[7] - x.absorb[5];  // 680 - 590 (clorofila)
  x.cor = x.absorb[0] - x.absorb[4];    // 415 - 555 (materia organica)

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
  if (t.temNtu()) return turbidez_config::classificar(t.ntu);
  const float v = x.turbidezCor;
  return v < 0.05f ? "LIMPA" : v < 0.15f ? "LEVEMENTE TURVA" : v < 0.35f ? "TURVA" : "MUITO TURVA";
}
const char* palavraAlgas(float v) { return v < 0.02f ? "NAO DETECTADAS" : v < 0.08f ? "SINAL FRACO" : "PRESENTES"; }
const char* palavraCor(float v) { return v < 0.03f ? "NORMAL" : v < 0.10f ? "LEVEMENTE AMARELADA" : "COR DE CHA"; }
const char* palavraContaminacao(float v) {
  return v < 0.10f ? "MUITO PROXIMA DA AGUA LIMPA" : v < 0.30f ? "LEVEMENTE CONTAMINADA"
         : v < 0.60f ? "CONTAMINADA" : v <= 1.0f ? "MUITO CONTAMINADA" : "PIOR QUE A AGUA PODRE";
}

// Tudo que vai no JSON (e no relatorio) em palavras e porcentagens.
dashboard_api::Diagnostico diagnosticar(const Indices& x, const turbidez_config::Resultado& t) {
  dashboard_api::Diagnostico d;
  for (uint8_t c = 0; c < CORES; ++c) d.porcentagem[c] = x.luz[c] * 100.0f;
  d.turbidez = palavraTurbidez(x, t);
  d.algas = palavraAlgas(x.algas);
  d.cor = palavraCor(x.cor);
  d.temContaminacao = x.temEscala;
  d.contaminacaoPct = x.contaminacao * 100.0f;
  d.contaminacao = palavraContaminacao(x.contaminacao);
  String turb = d.turbidez;
  turb.toLowerCase();
  d.resumo = "Agua " + turb + ", " +
             (x.algas < 0.02f ? "sem sinal de algas" : x.algas < 0.08f ? "com sinal fraco de algas" : "com sinal de algas") +
             ", " + (x.cor < 0.03f ? "cor normal" : x.cor < 0.10f ? "levemente amarelada" : "cor de cha") + ".";
  return d;
}

// ---- Relatorio no monitor ---------------------------------------------------------
void linha() { Serial.println(F("────────────────────────────────────────────────")); }

void barra(float fracao) {
  const int cheios = round(constrain(fracao, 0.0f, 1.0f) * 20);
  for (int i = 0; i < 20; ++i) Serial.print(i < cheios ? "█" : "░");
}

// Uma linha so: deu certo ou o motivo do erro.
void imprimirEnvio(dashboard_api::Envio envio) {
  const char* erro = dashboard_api::ultimoErro().c_str();
  switch (envio) {
    case dashboard_api::Envio::ENVIADO: Serial.println(F(" ✓ Enviado com sucesso")); break;
    case dashboard_api::Envio::GUARDADO: Serial.printf(" ✗ Erro: %s. Guardado, sera reenviado.\n", erro); break;
    case dashboard_api::Envio::FILA_CHEIA: Serial.printf(" ✗ Erro: %s. Fila cheia, ficou so no historico.\n", erro); break;
    case dashboard_api::Envio::RECUSADO: Serial.printf(" ✗ Erro: o servidor recusou (%s)\n", erro); break;
    case dashboard_api::Envio::SEM_DATA: Serial.println(F(" ✗ Erro: sem a hora da internet. Ficou so no historico.")); break;
  }
}

void imprimirRelatorio(const char* titulo, const sensor_config::Resultado& r, const turbidez_config::Resultado& t,
                       const Indices& x, const dashboard_api::Diagnostico& d) {
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
  if (t.temNtu()) {
    Serial.printf(" Turbidez  %s (%.0f NTU)\n", d.turbidez, t.ntu);
  } else {
    Serial.printf(" Turbidez  %s\n", d.turbidez);
  }
  Serial.printf(" Algas     %s\n Cor       %s\n\n", d.algas, d.cor);

  for (uint8_t c = 0; c < CORES; ++c) {
    Serial.printf(" %-13s %3u nm ", sensor_config::NOME_COR[c], sensor_config::ONDA_NM[c]);
    barra(x.luz[c]);
    Serial.printf(" %3.0f%%\n", d.porcentagem[c]);
  }
  if (r.saturado) Serial.println(F(" ⚠ Luz forte demais (sensor estourou): diminua o ganho, ex.: g 16"));
  if (t.semSinal()) Serial.println(F(" ⚠ Sensor de turbidez SEM SINAL: confira os fios e a chave D/A em \"A\"."));
  Serial.println();
}

// ---- Medir e enviar -------------------------------------------------------------
bool medirSensores(uint16_t esperaMs, sensor_config::Resultado& r, turbidez_config::Resultado& t) {
  Serial.print(F("# Medindo"));
  if (!sensor_config::medir(as7341, Serial, r, esperaMs)) {
    Serial.println(F("# ERRO: o sensor de cor nao respondeu. Confira os fios."));
    return false;
  }
  t = turbidez_config::medir();
  return true;
}

// Calcula, mostra o relatorio, envia para a API e grava no historico.
void relatarEEnviar(const char* tipo, const char* nome, const char* titulo, const sensor_config::Resultado& r,
                    const turbidez_config::Resultado& t) {
  const Indices x = calcular(r);
  const dashboard_api::Diagnostico d = diagnosticar(x, t);
  imprimirRelatorio(titulo, r, t, x, d);
  const dashboard_api::Medicao m = {tipo, nome, sensor_config::ganhoEmX(estado.ganho), &r, &t, &d};
  wifi_config::conectar(Serial, false);  // se caiu ou nunca conectou, tenta de novo
  imprimirEnvio(dashboard_api::enviar(Serial, m));
  if (!dashboard_api::registrar(m)) Serial.println(F(" ✗ Historico CHEIO: baixe com e e digite apagar sim"));
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
  if (!medirSensores(0, r, t)) return;
  char titulo[21];
  strcpy(titulo, estado.amostra);
  for (char* c = titulo; *c; ++c) *c = toupper(*c);
  relatarEEnviar("amostra", estado.amostra, titulo, r, t);
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
           turbidez_config::Resultado& t) {
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
  for (uint8_t i = 1; i <= REPLICAS; ++i) {
    Serial.printf("   Medicao %u/3 ", i);
    sensor_config::Resultado ri;
    turbidez_config::Resultado ti;
    if (!medirSensores(i == 1 ? sensor_config::ESPERA_MS : 0, ri, ti)) return false;
    for (uint8_t c = 0; c < CANAIS; ++c) {
      r.media[c] += ri.media[c] / REPLICAS;
      r.desvio[c] += ri.desvio[c] / REPLICAS;
    }
    r.saturado = r.saturado || ri.saturado;
    r.leituras += ri.leituras;
    t.valido = ti.valido;
    t.mediaMv += ti.mediaMv / REPLICAS;
  }
  t.ntu = turbidez_config::ntuDaTensao(t.mediaMv);
  return true;
}

// Agua limpa: vira o padrao (100% de luz) e o 0% de contaminacao.
bool gravarLimpa(const char* cabecalho) {
  sensor_config::Resultado r;
  turbidez_config::Resultado t;
  if (!passo(cabecalho, "Coloque AGUA LIMPA no pote, ate a marca.", r, t) || !salvarPadrao(r)) return false;
  relatarEEnviar("branco", "agua_limpa", "AGUA LIMPA (referencia 0%)", r, t);
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
  if (!passo(cabecalho, "Lave o pote e coloque a AGUA SUJA. Mexa bem.", r, t)) return false;
  memcpy(estado.ref.podre, calcular(r).absorb, sizeof(estado.ref.podre));
  estado.ref.temPodre = true;
  salvarReferencia();
  Serial.println(F(" ✓ REFERENCIA DE AGUA SUJA SALVA (100%)"));
  relatarEEnviar("amostra", "agua_podre", "AGUA SUJA (referencia 100%)", r, t);
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
                   " O aparelho mede sozinho a cada 1 minuto. Digite e aperte Enter:\n"
                   "   gravar     gravar agua limpa ou agua suja (ou aperte o botao BOOT)\n"
                   "   n NOME     nome da agua no pote (ex.: n lagoa)\n"
                   "   m          medir agora\n"
                   "   p          pausar / continuar as medicoes automaticas\n"
                   "   v          ver o sensor ao vivo (alinhar a lanterna)\n"
                   "   g N        ganho do sensor (1, 2, 4 ... 512)\n"
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
    const float mv = turbidez_config::lerMv();
    Serial.printf("Turbidez %.1f NTU (%.2f V)", turbidez_config::ntuDaTensao(mv), mv / 1000.0f);
  }
  Serial.println(saturado ? F("  <- LUZ FORTE DEMAIS") : F(""));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  esp_task_wdt_init(WATCHDOG_S, true);  // vigia: se o programa travar, a placa reinicia
  esp_task_wdt_add(nullptr);
  Serial.println(F("\n# Ligacoes: AS7341 VIN=3V3 SDA=21 SCL=22 · Turbidez VCC=VIN(5V) OUT=divisor->GPIO 34"));
  turbidez_config::iniciar();
  if (!sensor_config::iniciar(as7341)) {
    Serial.println(F("# ERRO: AS7341 nao encontrado. Confira os fios."));
    while (true) delay(1000);
  }
  Serial.println(F("# Sensores prontos."));

  dashboard_api::iniciar(Serial);  // memoria flash: fila e historico
  wifi_config::iniciar(Serial);    // Wi-Fi e hora
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
}

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

  // Leituras guardadas sem internet: tenta de novo de tempos em tempos.
  if (millis() - estado.reenvioMs >= REENVIO_MS) {
    estado.reenvioMs = millis();
    dashboard_api::reenviarPendentes(Serial);
  }
}
