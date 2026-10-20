# Qualidade da água medida com luz — ESP32 + AS7341 + lanterna

Baseado no *Guia do projeto para a equipe do hackathon*. Uma lanterna atravessa o pote com água; o sensor de cor AS7341 mede quanto de cada cor passou e o sensor de turbidez mede as partículas. O ESP32 mede a cada 1 minuto, mostra um relatório em palavras e envia para a API.

```
lanterna → pote com água → AS7341 + sensor de turbidez → ESP32 ──Wi-Fi──▶ API ─▶ banco ─▶ dashboard
```

## Arquivos (`src/`)

| Arquivo | O que faz |
|---|---|
| `main.cpp` | Medição automática, comandos, ensaio e relatório |
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
constexpr char API_URL[] = "https://SUA-API.onrender.com";  // sem "/" no final
constexpr char API_KEY[] = "CHAVE_COMBINADA_COM_O_BACK_END";
}
```

## Ligações

| Sensor | ESP32 |
|---|---|
| AS7341 VIN / GND / SDA / SCL | 3V3 / GND / GPIO 21 / GPIO 22 |
| Turbidez VCC / GND | VIN (5 V) / GND |
| Turbidez OUT | divisor `OUT ─[10 kΩ]─┬─ GPIO 34`, `└─[20 kΩ]─ GND` (**obrigatório**) |

Chave D/A da plaquinha de turbidez em **A**. Se o sensor de turbidez não estiver ligado, use `INSTALADO = false` em `turbidez_config.h`.

## Uso

1. **Upload** (com o monitor serial fechado) e depois abra o monitor.
2. Ao ligar aparece o **ensaio**. Aperte Enter e siga os passos (3 medições cada):
   - **água limpa:** vira o padrão (100% de luz) e o 0% de contaminação;
   - **água podre:** 100% de contaminação;
   - **água da lagoa:** o teste, avaliado entre 0% e 100%.

   Se ninguém apertar Enter em 2 minutos, o ensaio é pulado e o aparelho segue medindo.
3. Depois, o aparelho mede **sozinho a cada 1 minuto**.

| Comando | O que faz |
|---|---|
| `ensaio` | Roda o ensaio de novo |
| `n NOME` | Nome da água no pote (ex.: `n lagoa`) |
| `m` | Medir agora |
| `p` | Pausar / continuar |
| `c` | Novo padrão de água limpa (a próxima medição) |
| `v` | Ver o sensor ao vivo (alinhar a lanterna) |
| `g N` | Ganho do sensor (1, 2, 4 … 512). Apaga o padrão |
| `e` | Exportar o histórico CSV |
| `apagar sim` | Apagar o histórico (baixe antes) |

Mexeu na lanterna (posição, brilho, pilha)? Coloque água limpa e digite `c`.

## Dados

- **API:** cada medição vai por `POST {API_URL}/api/leituras/` com o header `X-API-Key`. O formato do JSON e as respostas esperadas estão no documento de comunicação com a API, entregue à equipe de back-end.
- **Sem internet:** as medições ficam numa fila na placa e são reenviadas depois.
- **Histórico:** toda medição também fica em `historico.csv` na placa (~4,5 dias). Exporte com `e`.
- **Monitor:** tudo que aparece no monitor serial também fica salvo em `logs/`.

## Limites

Os índices (turbidez, algas, cor, contaminação) são **relativos**: comparam a água com o padrão e com as referências. Não substituem análise de laboratório.
