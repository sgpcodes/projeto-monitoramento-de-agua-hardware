// =============================================================================
// wifi_config.h — conexao Wi-Fi e hora certa pela internet
// -----------------------------------------------------------------------------
// Nome e senha das redes ficam em secrets.h. O ESP32 so conecta em Wi-Fi 2,4 GHz.
// O ESP32 nao tem relogio com bateria: ao ligar, ele pega a hora em servidores
// NTP na internet (horario de Brasilia) e dai o relogio interno anda sozinho.
// =============================================================================

#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <time.h>

#include "secrets.h"

namespace wifi_config {

constexpr uint32_t TIMEOUT_MS = 20000;      // desiste de conectar depois de 20 s
constexpr char FUSO_BRASILIA[] = "<-03>3";  // UTC-3, sem horario de verao

// As redes do secrets.h; o ESP32 escolhe a que estiver com sinal.
inline WiFiMulti& redes() {
  static WiFiMulti multi;
  static bool cadastradas = false;
  if (!cadastradas) {
    multi.addAP(secrets::WIFI_SSID, secrets::WIFI_PASSWORD);
    multi.addAP(secrets::WIFI_SSID_2, secrets::WIFI_PASSWORD_2);
    multi.addAP(secrets::WIFI_SSID_3, secrets::WIFI_PASSWORD_3);
    multi.addAP(secrets::WIFI_SSID_4, secrets::WIFI_PASSWORD_4);
    multi.addAP(secrets::WIFI_SSID_5, secrets::WIFI_PASSWORD_5);
    cadastradas = true;
  }
  return multi;
}

inline bool conectado() { return WiFi.status() == WL_CONNECTED; }

// Escreve a hora atual no formato pedido (strftime). False se ainda nao pegou a hora.
inline bool agora(char* destino, size_t tamanho, const char* formato, uint32_t esperaMs = 100) {
  tm t;
  if (!getLocalTime(&t, esperaMs)) {
    destino[0] = '\0';
    return false;
  }
  strftime(destino, tamanho, formato, &t);
  return true;
}

// Conecta na rede disponivel (das do secrets.h) e espera a hora chegar.
// mostrar = false: tenta em silencio (antes de cada envio).
// Motivo da ultima recusa do roteador (para mostrar no monitor).
inline uint8_t& ultimoMotivo() {
  static uint8_t motivo = 0;
  return motivo;
}
inline const char* textoMotivo(uint8_t m) {
  switch (m) {
    case 2: case 15: case 202: case 204: return "SENHA ERRADA ou seguranca incompativel (use WPA2, nao WPA3)";
    case 201: return "rede nao encontrada";
    case 203: return "roteador recusou a associacao (limite de aparelhos ou filtro)";
    case 8: return "desconectado pelo proprio ESP32";
    default: return "outro motivo";
  }
}

inline bool conectar(Stream& serial, bool mostrar = true) {
  if (conectado()) return true;
  static bool eventoRegistrado = false;
  if (!eventoRegistrado) {
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) { ultimoMotivo() = info.wifi_sta_disconnected.reason; },
                 ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    eventoRegistrado = true;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);  // se o sinal cair depois, reconecta sozinho
  // Sem economia de energia no radio: com ela o ESP32 "cochila" entre pacotes e,
  // principalmente no roteador do celular, os envios falham um sim, outro nao.
  WiFi.setSleep(false);
  if (mostrar) serial.print(F("# Conectando Wi-Fi..."));
  const uint32_t inicio = millis();
  while (redes().run() != WL_CONNECTED && millis() - inicio < TIMEOUT_MS) {
    if (mostrar) serial.print('.');
  }
  if (!mostrar) {
    tm t;
    if (conectado()) getLocalTime(&t, 10000);  // espera a hora, sem imprimir
    return conectado();
  }
  serial.println();
  if (!conectado()) {
    serial.println(F("# Wi-Fi indisponivel (nenhuma das redes). Tento de novo na proxima medicao."));
    if (ultimoMotivo()) serial.printf("# Motivo da recusa: %u = %s\n", ultimoMotivo(), textoMotivo(ultimoMotivo()));
    // Mostra o que o ESP32 enxerga: se a rede nao aparece, esta longe ou em 5 GHz;
    // se aparece e mesmo assim nao conecta, a senha (ou o nome) esta diferente.
    WiFi.disconnect();  // para a tentativa de conexao antes de procurar redes
    delay(100);
    const int16_t total = WiFi.scanNetworks();
    serial.print(F("# Redes que o ESP32 encontrou (so 2,4 GHz): "));
    for (int16_t i = 0; i < total; ++i) {
      serial.print(i ? ", " : "");
      serial.print(WiFi.SSID(i));
      serial.printf(" (%d dBm)", WiFi.RSSI(i));
    }
    serial.println(total > 0 ? "" : total == 0 ? "nenhuma" : "falha ao procurar redes");
    WiFi.scanDelete();
    return false;
  }
  serial.printf("# Wi-Fi conectado na rede %s (%s)\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

  char hora[20];
  if (agora(hora, sizeof(hora), "%d/%m/%Y %H:%M:%S", 10000)) {
    serial.printf("# Hora certa: %s\n", hora);
  } else {
    serial.println(F("# A hora da internet ainda nao chegou. Tento de novo na proxima medicao."));
  }
  return true;
}

inline void iniciar(Stream& serial) {
  configTzTime(FUSO_BRASILIA, "pool.ntp.org", "time.google.com");
  conectar(serial);
}

}  // namespace wifi_config

#endif
