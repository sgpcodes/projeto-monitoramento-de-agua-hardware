// =============================================================================
// wifi_config.h — conexao Wi-Fi e hora certa pela internet
// -----------------------------------------------------------------------------
// Nome e senha das tres redes ficam em secrets.h. O ESP32 so conecta em Wi-Fi 2,4 GHz.
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

// As tres redes do secrets.h; o ESP32 escolhe a que estiver com sinal.
inline WiFiMulti& redes() {
  static WiFiMulti multi;
  static bool cadastradas = false;
  if (!cadastradas) {
    multi.addAP(secrets::WIFI_SSID, secrets::WIFI_PASSWORD);
    multi.addAP(secrets::WIFI_SSID_2, secrets::WIFI_PASSWORD_2);
    multi.addAP(secrets::WIFI_SSID_3, secrets::WIFI_PASSWORD_3);
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

// Conecta na rede disponivel (das tres do secrets.h) e espera a hora chegar.
// mostrar = false: tenta em silencio (antes de cada envio).
inline bool conectar(Stream& serial, bool mostrar = true) {
  if (conectado()) return true;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);  // se o sinal cair depois, reconecta sozinho
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
    // Mostra o que o ESP32 enxerga: se a rede nao aparece, esta longe ou em 5 GHz;
    // se aparece e mesmo assim nao conecta, a senha (ou o nome) esta diferente.
    const int16_t total = WiFi.scanNetworks();
    serial.print(F("# Redes que o ESP32 encontrou (so 2,4 GHz): "));
    for (int16_t i = 0; i < total; ++i) {
      serial.print(i ? ", " : "");
      serial.print(WiFi.SSID(i));
    }
    serial.println(total > 0 ? "" : "nenhuma");
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
