// =============================================================================
// main.cpp — o aparelho: mede a agua, mostra o relatorio e envia para a API
// -----------------------------------------------------------------------------
// Arquivos:
//   sensor_config.h    sensor de cor AS7341
//   turbidez_config.h  sensor de turbidez
//   wifi_config.h      Wi-Fi e hora da internet
//   dashboard_api.h    JSON, envio para a API, fila sem internet e historico CSV
//   secrets.h          senha do Wi-Fi, link e chave da API (fora do git)
//
// Uso: ao ligar aparece o ENSAIO (agua limpa = 0%, agua podre = 100%, lagoa =
// teste). Depois o aparelho mede sozinho a cada 1 minuto. Comandos: digite ?.
//
// Contas (guia, Parte B · 3), com o padrao de agua limpa como 100% de luz:
//   luz que passou T = amostra / padrao        absorbancia A = -log10(T)
//   turbidez = 1 - mV/mV_padrao (sensor de turbidez) ou 1 - T(590 nm)
//   algas = A680 - A590      cor (materia organica) = A415 - A555
//   contaminacao = (A . A_podre) / (A_podre . A_podre)   0 = limpa, 1 = podre
// As palavras (LIMPA, TURVA...) sao estimativas, nao normas oficiais.
// =============================================================================

#include <Arduino.h>
#include <LittleFS.h>

#include "dashboard_api.h"
#include "sensor_config.h"
#include "turbidez_config.h"
#include "wifi_config.h"

namespace {

using sensor_config::CANAIS;
using sensor_config::CORES;

constexpr uint32_t INTERVALO_MS = 60UL * 1000UL;           // medicao automatica: 1 minuto
constexpr uint32_t ESPERA_ENSAIO_MS = 120UL * 1000UL;      // 2 min para apertar Enter ao ligar
constexpr uint32_t REENVIO_MS = 5UL * 60UL * 1000UL;       // tenta a fila a cada 5 min
constexpr uint8_t REPLICAS = 3;                            // medicoes por passo do ensaio
constexpr char ARQUIVO_REFERENCIA[] = "/referencia.bin";

Adafruit_AS7341 as7341;

// ---- Referencias: padrao de agua limpa (100% de luz) e agua podre (100% sujeira)
struct Referencia {
  uint8_t versao = 2;  // muda se a struct mudar: arquivo antigo e ignorado
  as7341_gain_t ganho = sensor_config::GANHO_PADRAO;
  bool temPadrao = false;
  float padrao[CANAIS] = {0};
  bool temTurbidez = false;
  float turbidezMv = 0;
  bool temPodre = false;
  float podre[CORES] = {0};  // assinatura (absorbancia) da agua podre
};

// O que o aparelho lembra entre um comando e outro.
struct Estado {
  as7341_gain_t ganho = sensor_config::GANHO_PADRAO;
  bool aoVivo = false;      // comando v
  bool automatico = true;   // comando p
  uint16_t contador = 0;
  uint32_t proximaMs = INTERVALO_MS;
  uint32_t reenvioMs = 0;
  char amostra[21] = "sem_nome";  // comando n
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

// ---- Contas -------------------------------------------------------------------
struct Indices {
  float luz[CORES];     // fracao de luz que passou (1 = 100%)
  float absorb[CORES];  // absorbancia
  float turbidez, algas, cor;
  bool turbidezPeloSensor = false;
  bool temEscala = false;
  float contaminacao = 0;  // 0 = limpa, 1 = igual a podre
};

Indices calcular(const sensor_config::Resultado& cor, const turbidez_config::Resultado& turb) {
  const Referencia& ref = estado.ref;
  Indices x;
  for (uint8_t c = 0; c < CORES; ++c) {
    x.luz[c] = ref.padrao[c] > 0 ? constrain(cor.media[c] / ref.padrao[c], 0.0f, 1.5f) : 0;
    x.absorb[c] = -log10(max(x.luz[c], 0.001f));
  }
  if (turb.valido && !turb.semSinal() && ref.temTurbidez && ref.turbidezMv > 0) {
    x.turbidez = 1.0f - turb.mediaMv / ref.turbidezMv;
    x.turbidezPeloSensor = true;
  } else {
    x.turbidez = 1.0f - x.luz[5];  // amarelo (590 nm): pouco afetado por algas e cor
  }
  x.algas = x.absorb[7] - x.absorb[5];  // 680 - 590
  x.cor = x.absorb[0] - x.absorb[4];    // 415 - 555

  // Escala limpa -> podre: quanto da "assinatura" da agua podre cabe nesta agua.
  // (Beer-Lambert: misturar x% de agua podre gera x% da absorbancia dela.)
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

const char* palavraTurbidez(float v) {
  return v < 0.05f ? "LIMPA" : v < 0.15f ? "LEVEMENTE TURVA" : v < 0.35f ? "TURVA" : "MUITO TURVA";
}
const char* palavraAlgas(float v) { return v < 0.02f ? "NAO DETECTADAS" : v < 0.08f ? "SINAL FRACO" : "PRESENTES"; }
const char* palavraCor(float v) { return v < 0.03f ? "NORMAL" : v < 0.10f ? "LEVEMENTE AMARELADA" : "COR DE CHA"; }
const char* palavraContaminacao(float v) {
  return v < 0.10f ? "MUITO PROXIMA DA AGUA LIMPA" : v < 0.30f ? "LEVEMENTE CONTAMINADA"
         : v < 0.60f ? "CONTAMINADA" : v <= 1.0f ? "MUITO CONTAMINADA" : "PIOR QUE A AGUA PODRE";
}

// ---- Relatorio ----------------------------------------------------------------
void linha() { Serial.println(F("────────────────────────────────────────────────")); }

void barra(float fracao) {
  const int cheios = round(constrain(fracao, 0.0f, 1.0f) * 20);
  for (int i = 0; i < 20; ++i) Serial.print(i < cheios ? "█" : "░");
}

void imprimirEnvio(dashboard_api::Envio envio) {
  switch (envio) {
    case dashboard_api::Envio::ENVIADO: Serial.println(F(" ✓ Enviado para o dashboard")); break;
    case dashboard_api::Envio::GUARDADO:
      Serial.print(F(" ⏳ Sem internet: guardado ("));
      Serial.print(dashboard_api::contarPendentes());
      Serial.println(F(" esperando). Sera enviado quando a internet voltar."));
      break;
    case dashboard_api::Envio::FILA_CHEIA: Serial.println(F(" ✗ Sem internet e fila cheia (continua no historico).")); break;
    case dashboard_api::Envio::RECUSADO: Serial.println(F(" ✗ O servidor recusou os dados (veja a mensagem acima).")); break;
    case dashboard_api::Envio::SEM_DATA: Serial.println(F(" ✗ Nao enviado: o relogio ainda nao pegou a hora.")); break;
  }
}

void imprimirRelatorio(uint16_t numero, const char* nome, const sensor_config::Resultado& cor,
                       const turbidez_config::Resultado& turb, const Indices& x) {
  static const char* const NOME_COR[CORES] = {"Violeta ", "Azul    ", "Ciano   ", "Verde   ",
                                              "Verde-am", "Amarelo ", "Laranja ", "Vermelho"};
  char quando[20];
  if (!wifi_config::agora(quando, sizeof(quando), "%d/%m/%Y %H:%M:%S")) strcpy(quando, "sem hora");

  Serial.println();
  linha();
  Serial.printf(" MEDICAO %u  ·  %s  ·  %s\n", numero, nome, quando);
  linha();
  Serial.printf(" Turbidez (barro, particulas) ... %s   (%.2f%s)\n", palavraTurbidez(x.turbidez),
                x.turbidez, x.turbidezPeloSensor ? "" : ", pelo sensor de cor");
  Serial.printf(" Algas (agua verde) ............. %s   (%.2f)\n", palavraAlgas(x.algas), x.algas);
  Serial.printf(" Cor (agua cor de cha) .......... %s   (%.2f)\n", palavraCor(x.cor), x.cor);

  Serial.println();
  Serial.println(F(" Luz que atravessou a agua (100% = padrao de agua limpa):"));
  bool passouDemais = false;
  for (uint8_t c = 0; c < CORES; ++c) {
    passouDemais = passouDemais || x.luz[c] > 1.10f;
    Serial.printf("   %s  ", NOME_COR[c]);
    barra(x.luz[c]);
    Serial.printf("  %.0f%%\n", x.luz[c] * 100.0f);
  }
  if (passouDemais) Serial.println(F(" ⚠ Passou MAIS luz que no padrao: a lanterna mudou. Novo padrao: c"));

  if (x.temEscala) {
    Serial.println();
    Serial.println(F(" AVALIACAO DA AGUA (escala entre a agua limpa e a agua podre):"));
    Serial.print(F("   LIMPA |"));
    barra(x.contaminacao);
    Serial.println(F("| PODRE"));
    Serial.printf("   Nivel de contaminacao: %.0f%%  ->  %s\n", x.contaminacao * 100.0f,
                  palavraContaminacao(x.contaminacao));
  }

  if (cor.saturado) Serial.println(F(" ⚠ Luz forte demais (sensor estourou): diminua o ganho, ex.: g 16"));
  if (turb.semSinal()) Serial.println(F(" ⚠ Sensor de turbidez SEM SINAL: confira os fios e a chave D/A em \"A\"."));

  String t = palavraTurbidez(x.turbidez);
  t.toLowerCase();
  Serial.printf("\n → Agua %s, %s, %s.\n", t.c_str(),
                x.algas < 0.02f ? "sem sinal de algas" : x.algas < 0.08f ? "com sinal fraco de algas" : "com sinal de algas",
                x.cor < 0.03f ? "cor normal" : x.cor < 0.10f ? "levemente amarelada" : "cor de cha");
}

// ---- Medir --------------------------------------------------------------------
// Mede os dois sensores, envia para a API e grava no historico.
bool medir(const char* tipo, const char* nome, uint8_t replica, uint16_t esperaMs,
           sensor_config::Resultado& cor, turbidez_config::Resultado& turb, dashboard_api::Envio& envio) {
  Serial.print(F("# Medindo"));
  if (!sensor_config::medir(as7341, Serial, cor, esperaMs)) {
    Serial.println(F("# ERRO: o sensor de cor nao respondeu. Confira os fios."));
    return false;
  }
  turb = turbidez_config::medir();
  const dashboard_api::Medicao m = {tipo, nome, replica, sensor_config::ganhoEmX(estado.ganho), &cor, &turb};
  envio = dashboard_api::enviar(Serial, m);
  if (!dashboard_api::registrar(m)) Serial.println(F("# ✗ Historico CHEIO: baixe com e e digite apagar sim"));
  return true;
}

// Guarda a medicao como padrao de agua limpa, se a luz estiver boa.
bool salvarPadrao(const sensor_config::Resultado& cor, const turbidez_config::Resultado& turb) {
  if (cor.media[sensor_config::CLEAR] < sensor_config::LUZ_FRACA) {
    Serial.println(F(" ✗ Chegou pouca luz: a lanterna esta LIGADA e apontada? Padrao NAO salvo."));
    return false;
  }
  if (cor.saturado) {
    Serial.println(F(" ✗ Luz forte demais: diminua o ganho (ex.: g 16). Padrao NAO salvo."));
    return false;
  }
  memcpy(estado.ref.padrao, cor.media, sizeof(estado.ref.padrao));
  estado.ref.temPadrao = true;
  estado.ref.temTurbidez = turb.valido && !turb.semSinal();
  estado.ref.turbidezMv = turb.mediaMv;
  estado.ref.ganho = estado.ganho;
  salvarReferencia();
  Serial.println(F(" ✓ PADRAO DE AGUA LIMPA SALVO (100%)"));
  return true;
}

// Medicao automatica (ou comando m). Sem padrao, ela vira o padrao.
void medirAmostra() {
  sensor_config::Resultado cor;
  turbidez_config::Resultado turb;
  dashboard_api::Envio envio;
  if (!temPadrao()) {
    Serial.println(F("\n# Sem padrao: esta medicao sera o PADRAO DE AGUA LIMPA (pote com agua limpa)."));
    if (medir("branco", "agua_limpa", 1, 0, cor, turb, envio)) {
      salvarPadrao(cor, turb);
      imprimirEnvio(envio);
    }
    return;
  }
  ++estado.contador;
  const uint8_t replica = (estado.contador - 1) % 255 + 1;  // a API aceita 1 a 255
  if (!medir("amostra", estado.amostra, replica, 0, cor, turb, envio)) return;
  imprimirRelatorio(estado.contador, estado.amostra, cor, turb, calcular(cor, turb));
  imprimirEnvio(envio);
  Serial.printf(" Historico: %u medicoes na placa.\n", dashboard_api::linhasHistorico());
  linha();
  if (estado.automatico) Serial.println(F(" Proxima medicao em 1 minuto.  (p = pausar, ? = ajuda)"));
}

// ---- Ensaio: agua limpa (0%), agua podre (100%), lagoa (teste) ------------------
// Espera Enter (x = cancelar). tempoMs = 0: espera para sempre. False = cancelou/tempo.
bool esperarEnter(uint32_t tempoMs) {
  while (Serial.available()) Serial.read();
  const uint32_t inicio = millis();
  String texto;
  while (tempoMs == 0 || millis() - inicio < tempoMs) {
    while (Serial.available()) {
      const char ch = Serial.read();
      if (ch == '\n') {
        texto.trim();
        return !texto.equalsIgnoreCase("x");
      }
      if (ch != '\r') texto += ch;
    }
    delay(20);
  }
  return false;
}

// Mostra o passo, espera Enter e faz 3 medicoes; devolve a media delas.
bool passo(uint8_t numero, const char* titulo, const char* instrucao, const char* tipo, const char* nome,
           sensor_config::Resultado& cor, turbidez_config::Resultado& turb) {
  Serial.printf("\n==================================================\n PASSO %u de 3:  %s\n"
                "==================================================\n %s\n"
                " Lanterna ACESA, mesma posicao, caixa fechada.\n"
                " Aperte Enter quando estiver pronto (x + Enter = cancelar).\n",
                numero, titulo, instrucao);
  if (!esperarEnter(0)) {
    Serial.println(F("# Ensaio cancelado."));
    return false;
  }
  cor = sensor_config::Resultado();
  for (uint8_t c = 0; c < CANAIS; ++c) cor.media[c] = cor.desvio[c] = 0;
  turb = turbidez_config::Resultado();
  for (uint8_t i = 1; i <= REPLICAS; ++i) {
    Serial.printf("   Replica %u/3 ", i);
    sensor_config::Resultado r;
    turbidez_config::Resultado t;
    dashboard_api::Envio envio;
    if (!medir(tipo, nome, i, i == 1 ? sensor_config::ESPERA_MS : 0, r, t, envio)) return false;
    for (uint8_t c = 0; c < CANAIS; ++c) {
      cor.media[c] += r.media[c] / REPLICAS;
      cor.desvio[c] += r.desvio[c] / REPLICAS;
    }
    cor.saturado = cor.saturado || r.saturado;
    cor.leituras += r.leituras;
    turb.valido = t.valido;
    turb.mediaMv += t.mediaMv / REPLICAS;
  }
  return true;
}

void ensaio(bool aoLigar) {
  Serial.println(F("\n##################################################\n"
                   "#            ENSAIO DE 3 AMOSTRAS                #\n"
                   "##################################################\n"
                   " 1) agua limpa    = referencia 0%  (o que a lagoa deveria ser)\n"
                   " 2) agua podre    = referencia 100% (o que ninguem quer na lagoa)\n"
                   " 3) agua da lagoa = TESTE: onde ela fica entre 0% e 100%?\n"
                   " Cada passo mede 3 vezes (~40 s)."));
  if (aoLigar) {
    Serial.println(F(" Aperte Enter para COMECAR (x ou esperar 2 min = pular)."));
    if (!esperarEnter(ESPERA_ENSAIO_MS)) {
      Serial.println(F("# Ensaio pulado. Medicoes automaticas normais (digite ensaio para rodar)."));
      return;
    }
  }
  estado.aoVivo = false;
  sensor_config::Resultado cor;
  turbidez_config::Resultado turb;

  if (!passo(1, "AGUA LIMPA (referencia 0%)", "Coloque AGUA LIMPA no pote, ate a marca.",
             "branco", "agua_limpa", cor, turb)) return;
  if (!salvarPadrao(cor, turb)) return;

  if (!passo(2, "AGUA PODRE (referencia 100%)", "Lave o pote e coloque a AGUA PODRE. Mexa bem.",
             "amostra", "agua_podre", cor, turb)) return;
  Indices x = calcular(cor, turb);
  memcpy(estado.ref.podre, x.absorb, sizeof(estado.ref.podre));
  estado.ref.temPodre = true;
  salvarReferencia();
  x = calcular(cor, turb);
  imprimirRelatorio(2, "agua_podre", cor, turb, x);
  float luzMedia = 0;
  for (uint8_t c = 0; c < CORES; ++c) luzMedia += x.luz[c] / CORES;
  if (luzMedia < 0.03f) Serial.println(F(" ⚠ Agua quase opaca (< 3% de luz): dilua 1 parte em 9 de agua limpa."));
  Serial.println(F(" ✓ Referencia de agua podre salva (100%)."));

  strcpy(estado.amostra, "lagoa");
  if (!passo(3, "AGUA DA LAGOA (TESTE)", "Lave o pote e coloque a AGUA DA LAGOA. Mexa de leve.",
             "amostra", "lagoa", cor, turb)) return;
  x = calcular(cor, turb);
  imprimirRelatorio(3, "lagoa", cor, turb, x);
  linha();
  Serial.printf("\n ✓ ENSAIO CONCLUIDO. A agua da lagoa ficou em %.0f%%: %s\n"
                "   Daqui em diante, cada medicao mostra o nivel de contaminacao.\n",
                x.contaminacao * 100.0f, palavraContaminacao(x.contaminacao));
  estado.automatico = true;
  estado.proximaMs = millis() + INTERVALO_MS;
}

// ---- Comandos -----------------------------------------------------------------
void ajuda() {
  Serial.println(F("\n=============== COMO USAR ===============\n"
                   " O aparelho mede sozinho a cada 1 minuto. Digite e aperte Enter:\n"
                   "   ensaio     agua limpa (0%), agua podre (100%), lagoa (teste)\n"
                   "   n NOME     nome da agua no pote (ex.: n lagoa)\n"
                   "   m          medir agora\n"
                   "   p          pausar / continuar as medicoes automaticas\n"
                   "   c          novo padrao de agua limpa (a proxima medicao)\n"
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

  if (!strcmp(cmd, "ensaio")) {
    ensaio(false);
  } else if (!strcmp(cmd, "n")) {
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
  } else if (!strcmp(cmd, "c")) {
    estado.ref.temPadrao = false;
    salvarReferencia();
    estado.proximaMs = millis() + INTERVALO_MS;
    Serial.println(F("# Padrao apagado. Coloque AGUA LIMPA: a proxima medicao (ou m) vira o padrao."));
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
  if (turbidez_config::INSTALADO) Serial.printf("Turbidez %.0f mV", turbidez_config::lerMv());
  Serial.println(saturado ? F("  <- LUZ FORTE DEMAIS") : F(""));
}

}  // namespace

void setup() {
  Serial.begin(115200);
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

  // Ensaio ao ligar. Se ninguem apertar Enter em 2 min (ex.: a placa reiniciou
  // sozinha), pula e segue medindo normalmente.
  ensaio(true);
}

void loop() {
  lerTeclado();

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
