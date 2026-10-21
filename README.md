# Qualidade da água medida com luz — ESP32 + AS7341 + lanterna

Baseado no *Guia do projeto para a equipe do hackathon*. Uma lanterna atravessa o pote com água; o sensor de cor AS7341 mede quanto de cada cor passou e o sensor de turbidez mede as partículas. O ESP32 mede a cada 1 minuto, mostra um relatório em palavras e envia para a API.

```
lanterna → pote com água → AS7341 + sensor de turbidez → ESP32 ──Wi-Fi──▶ API ─▶ banco ─▶ dashboard
```

## Arquivos (`src/`)

| Arquivo | O que faz |
|---|---|
| `main.cpp` | Medição automática, botão, comandos e relatório |
| `sensor_config.h` | Sensor de cor AS7341 (pinos e leitura) |
| `turbidez_config.h` | Sensor de turbidez (pino, divisor de tensão e leitura) |
| `wifi_config.h` | Wi-Fi e hora da internet |
| `dashboard_api.h` | JSON, envio para a API, fila sem internet e histórico CSV |
| `secrets.h` | Wi-Fi, link e chave da API. **Não vai para o git** |

O `secrets.h` tem este formato:
```cpp
namespace secrets {
constexpr char WIFI_SSID[] = "NOME_DA_REDE";        // Wi-Fi 2,4 GHz
constexpr char WIFI_PASSWORD[] = "SENHA_DA_REDE";
constexpr char WIFI_SSID_2[] = "OUTRA_REDE";        // ex.: Wi-Fi de casa
constexpr char WIFI_PASSWORD_2[] = "SENHA_DA_OUTRA_REDE";
constexpr char WIFI_SSID_3[] = "TERCEIRA_REDE";      // ex.: celular
constexpr char WIFI_PASSWORD_3[] = "SENHA_DA_TERCEIRA_REDE";
constexpr char WIFI_SSID_4[] = "QUARTA_REDE";
constexpr char WIFI_PASSWORD_4[] = "SENHA_DA_QUARTA_REDE";
constexpr char WIFI_SSID_5[] = "QUINTA_REDE";
constexpr char WIFI_PASSWORD_5[] = "SENHA_DA_QUINTA_REDE";
constexpr char API_URL[] = "https://SUA-API.onrender.com";  // sem "/" no final
constexpr char API_KEY[] = "CHAVE_COMBINADA_COM_O_BACK_END";
}
```

## Ligações

| Sensor | ESP32 |
|---|---|
| AS7341 VIN / GND / SDA / SCL | 3V3 / GND / GPIO 21 / GPIO 22 |
| Turbidez VCC / GND | VIN (5 V) / GND |
| Turbidez OUT | divisor `OUT ─[10 kΩ]─┬─ GPIO 34`, `└─[10 kΩ]─ GND` (**obrigatório**; 5 V vira 2,5 V) |

Chave D/A da plaquinha de turbidez em **A**. Se o sensor de turbidez não estiver ligado, use `INSTALADO = false` em `turbidez_config.h`.

**Turbidez (sensor analógico):** o ESP32 lê o ADC (GPIO 34, 12 bits, atenuação 11 dB, `analogReadMilliVolts`), desfaz o divisor e manda a **tensão e o ADC crus**. A conversão para NTU usa a curva publicada pelo fabricante (DFRobot SEN0189) e está **marcada como experimental**: o modelo do sensor não foi confirmado e a curva nunca foi comparada com padrões de turbidez. Por isso `turbidez.ntu` vai `null` e a estimativa vai em `turbidez.ntu_experimental`; o dashboard usa a luz bloqueada pelo sensor de cor como índice relativo de turbidez. **Para calibrar:** medir padrões de formazina (ou comparar com um turbidímetro de referência), ajustar os coeficientes da `CALIBRACAO` em `turbidez_config.h`, trocar o `id` e marcar `validada = true`. Leituras sem sinal, saturadas, instáveis ou fora da faixa da curva são enviadas com o `status` correspondente, nunca como 0 NTU.

## Uso

1. **Upload** (com o monitor serial fechado) e depois abra o monitor.
2. Aperte o botão **BOOT** da placa (ou digite `gravar`) e grave as duas referências. Cada uma mede 3 vezes e fica salva mesmo desligando:
   - `1` **água limpa:** vira o padrão (100% de luz) e o 0% de contaminação;
   - `2` **água suja:** vira o 100% de contaminação.
3. Coloque a água da lagoa. O aparelho mede **sozinho a cada 1 minuto, sem parar**, e envia cada medição para o dashboard. Se a placa reiniciar, ele volta a medir sozinho com as referências salvas.

| Comando | O que faz |
|---|---|
| `gravar` ou botão **BOOT** | Gravar: 1 = água limpa, 2 = água suja, 3 = medir agora |
| `n NOME` | Nome da água no pote (padrão: `lagoa`) |
| `m` | Medir agora |
| `p` | Pausar / continuar |
| `v` | Ver o sensor ao vivo (alinhar a lanterna) |
| `g N` | Ganho do sensor (1, 2, 4 … 512). Apaga o padrão |
| `e` | Exportar o histórico CSV |
| `apagar sim` | Apagar o histórico (baixe antes) |

**Nomes das amostras** (comando `n NOME`, o dashboard junta as 3 últimas medições de cada nome):

| Amostra | Nome |
|---|---|
| Água boa (controle) | `agua_boa` |
| Água podre | gravada com BOOT → 2 (`agua_podre`) |
| Pontos da lagoa | `lagoa_p1`, `lagoa_p2`, `lagoa_p3` |
| Referências | `ref_cha`, `ref_terra`, `ref_algas` |
| Série de diluição | `dil_<corante>_<concentração>`, ex.: `dil_amarelo_25`, `dil_amarelo_50`, `dil_amarelo_100` |

Mudou alguma coisa (lanterna, pote, água de referência)? Aperte o botão **BOOT** da placa (ou digite `gravar`) e escolha o que regravar. A nova referência também vai para o dashboard. Botão externo: ligue entre o GPIO 4 e o GND e mude `PINO_BOTAO` para 4 no `main.cpp`.

## Dados

- **Wi-Fi:** o ESP32 conecta em qualquer uma das redes do `secrets.h` (até 5) e pega a hora da internet. Se cair, ele tenta de novo a cada medição. Embaixo de cada relatório aparece `✓ Enviado com sucesso` ou `✗ Erro:` com o motivo.
- **API:** cada medição vai por `POST {API_URL}/api/leituras/` com o header `X-API-Key` (formato abaixo). A API deve responder 201; com 401/400 o ESP32 mostra o erro, e sem resposta ou com 5xx ele guarda e reenvia depois.
- **Sem internet:** as medições ficam numa fila na placa e são reenviadas depois.
- **Histórico:** toda medição também fica em `historico.csv` na placa (~4,5 dias). Exporte com `e`.
- **Monitor:** tudo que aparece no monitor serial também fica salvo em `logs/`.

## JSON enviado para a API

```json
{
  "data_coleta": "2026-10-20",
  "medido_em": "2026-10-20T14:02:10-03:00",
  "tipo": "amostra",
  "amostra": "lagoa",
  "ganho": 64,
  "saturado": false,
  "media": { "f1_415": 2394.0, "f2_445": 4802.0, "...": "...", "clear": 31200.0, "nir": 1650.0 },
  "desvio": { "f1_415": 8.1, "f2_445": 11.3, "...": "...", "clear": 40.2, "nir": 6.0 },
  "firmware_version": "2026.10.21-turbidez-v2",
  "turbidez": {
    "status": "ok", "calibracao_id": "dfrobot-sen0189-fabricante-v1", "calibracao_status": "pendente",
    "adc_raw": 2290, "tensao_v": 3.692, "desvio_mv": 8,
    "ntu_experimental": 1593.0, "ntu": null,
    "luz_bloqueada_pct": 12.5, "classificacao": "LEVEMENTE TURVA"
  },
  "cores": [
    { "nome": "Violeta",       "comprimento_onda_nm": 415, "valor": 2394,  "porcentagem": 57.0 },
    { "nome": "Azul",          "comprimento_onda_nm": 445, "valor": 4802,  "porcentagem": 49.0 },
    { "nome": "Ciano",         "comprimento_onda_nm": 480, "valor": 4576,  "porcentagem": 52.0 },
    { "nome": "Verde",         "comprimento_onda_nm": 515, "valor": 4860,  "porcentagem": 54.0 },
    { "nome": "Verde-amarelo", "comprimento_onda_nm": 555, "valor": 5225,  "porcentagem": 55.0 },
    { "nome": "Amarelo",       "comprimento_onda_nm": 590, "valor": 4920,  "porcentagem": 60.0 },
    { "nome": "Laranja",       "comprimento_onda_nm": 630, "valor": 3965,  "porcentagem": 65.0 },
    { "nome": "Vermelho",      "comprimento_onda_nm": 680, "valor": 1404,  "porcentagem": 54.0 }
  ],
  "diagnostico": {
    "turbidez": "LEVEMENTE TURVA",
    "algas": "SINAL FRACO",
    "cor": "NORMAL",
    "contaminacao_pct": 20,
    "contaminacao": "LEVEMENTE CONTAMINADA",
    "resumo": "Agua levemente turva, com sinal fraco de algas, cor normal."
  }
}
```

- `tipo`: `branco` (água limpa de referência, cores ≈ 100%) ou `amostra`.
- `media` / `desvio`: os 10 canais crus do sensor e o quanto variaram nas 20 leituras (barras de erro do dashboard). `ganho`/`saturado`: configuração e alerta de luz forte demais.
- `valor`: número captado pelo sensor naquela cor. `porcentagem`: quanto de luz passou comparado à água limpa (100% = igual à água limpa).
- `turbidez.luz_bloqueada_pct`: turbidez pelo sensor de cor (AS7341): % da luz bloqueada em 590–630 nm, faixa onde a cor da água e a clorofila quase não absorvem (mesma ideia da ISO 7027). Vem sempre.
- `turbidez.status`: `ok`, `sem_sinal`, `saturado`, `instavel`, `fora_da_faixa_da_curva` ou `desativado`. `turbidez.ntu` = NTU **validado** (null enquanto `calibracao_status` = `pendente`); `ntu_experimental` = estimativa pela curva do fabricante.
- `turbidez.classificacao`: palavra pela luz bloqueada do sensor de cor; pelas faixas da lei (NTU) só com calibração validada.
- `contaminacao_pct`: 0 = água limpa, 100 = água podre de referência. Vem `null` até gravar a água suja.

## Limites

Os índices (turbidez, algas, cor, contaminação) são **relativos**: comparam a água com o padrão e com as referências. Não substituem análise de laboratório.
