// =============================================================================
// wifi_config.h — conexao Wi-Fi e hora certa pela internet
// -----------------------------------------------------------------------------
// Nome e senha da rede ficam em secrets.h. O ESP32 so conecta em Wi-Fi 2,4 GHz.
// O ESP32 nao tem relogio com bateria: ao ligar, ele pega a hora em servidores
// NTP na internet (horario de Brasilia) e dai o relogio interno anda sozinho.
// =============================================================================

#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "secrets.h"

namespace wifi_config {

constexpr uint32_t TIMEOUT_MS = 20000;      // desiste de conectar depois de 20 s
constexpr char FUSO_BRASILIA[] = "<-03>3";  // UTC-3, sem horario de verao

inline bool conectar(Stream& serial) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);  // se o sinal cair depois, reconecta sozinho
  WiFi.begin(secrets::WIFI_SSID, secrets::WIFI_PASSWORD);
  serial.print(F("# Conectando Wi-Fi"));
  const uint32_t inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < TIMEOUT_MS) {
    delay(500);
    serial.print('.');
  }
  serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    serial.println(F("# Wi-Fi indisponivel. As medicoes ficam guardadas e sao enviadas depois."));
    return false;
  }
  serial.print(F("# Wi-Fi conectado: "));
  serial.println(WiFi.localIP());
  return true;
}

inline void iniciar(Stream& serial) {
  conectar(serial);
  configTzTime(FUSO_BRASILIA, "pool.ntp.org", "time.google.com");
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

}  // namespace wifi_config

#endif
