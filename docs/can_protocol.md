# Protocolo CAN — Blue Mechanic V1

## Visão Geral

A comunicação CAN utiliza o periférico TWAI on-chip do ESP32-S3. O Teensy 4.1 com Zephyr atua como bridge/controlador da aplicação e os ESP32-S3 atuam como nós de execução. Fisicamente, a topologia é um barramento multidrop com 1 Teensy e até 10 ESP32-S3.

O `node_id` válido é **1–10** tanto no ESP32 quanto no Teensy. O limite faz parte do protocolo: com as bases padrão e filtros `0x7F0`, valores maiores podem fazer status, posição e eventos compartilharem o mesmo CAN ID.

O protocolo usa frames CAN padrão de 11 bits (standard identifiers) com DLC máximo de 8 bytes.

---

## Configuração de Hardware

| Parâmetro     | Valor Padrão | Pino ESP32-S3 |
|---------------|-------------|---------------|
| CAN TX        | GPIO 39     | GPIO_NUM_39   |
| CAN RX        | GPIO 40     | GPIO_NUM_40   |
| Bitrate       | 500 kbps    | —             |
| Node ID       | 1           | —             |

No Teensy 4.1, o bridge usa FlexCAN1: CTX1 no pino 22 e CRX1 no pino 23. Os dois controladores exigem transceivers CAN externos compatíveis com lógica de 3,3 V, terra comum e terminação de 120 Ω nas duas extremidades físicas do barramento.

### IDs CAN

Cada nó ESP32 possui três faixas de IDs base configuráveis. A telemetria de posição/temperatura usa uma subfaixa derivada do status:

| Base            | Padrão  | Finalidade                        |
|-----------------|---------|-----------------------------------|
| `can_command_base_id` | `0x200` | Comandos do Teensy para o ESP32   |
| `can_status_base_id`  | `0x280` | Status do ESP32 para o Teensy     |
| `can_event_base_id`   | `0x300` | Eventos do ESP32 para o Teensy    |
| `can_status_base_id + 0x10` | `0x290` | Posição e temperatura |

O ID efetivo de cada ESP32 é calculado somando o `node_id` à base:

```
own_command_id  = can_command_base_id + node_id
own_status_id   = can_status_base_id  + node_id
own_position_id = can_status_base_id  + 0x10 + node_id
own_event_id    = can_event_base_id   + node_id
```

**Exemplo para node_id = 1:**
- Comando: `0x201`
- Status: `0x281`
- Posição/temperatura: `0x291`
- Evento: `0x301`

### Filtro de Hardware

Com as bases padrão, o filtro TWAI usa ID `0x200` e máscara `0x780`, aceitando em hardware o grupo `0x200..0x27F`. O firmware faz uma segunda validação em software e processa somente:

- `can_command_base_id + node_id`: comando unicast para o próprio nó;
- `can_command_base_id`: comando broadcast para todos os nós.

O broadcast é indicado apenas para comandos de atuação, como enable, velocidade, movimento, home, laser e fan. Não use broadcast para `PING` ou `STATUS_REQUEST`, pois todos os nós responderiam ao mesmo tempo. O broadcast não oferece sincronização temporal rígida: cada ESP32 agenda a operação localmente depois de receber o frame.

Se `can_command_base_id` for alterado, o Teensy deve ser reconfigurado junto. A base e os IDs `base + node_id` também devem permanecer no mesmo bloco de 128 IDs coberto pela máscara `0x780`.

---

## Formato do Frame CAN

Todos os frames usam o formato padrão de 11 bits:

| Campo       | Bits          | Valor                          |
|-------------|---------------|--------------------------------|
| Standard ID | 11 bits       | ID efetivo do nó ou base de comando para broadcast |
| DLC         | 4 bits        | 1–8 bytes de dados             |
| Data        | 0–64 bits     | Payload do frame               |
| RTR         | 1 bit         | Data frame (0)                 |
| IDE         | 1 bit         | Standard frame (0)             |

---

## Direção da Comunicação

| Direção     | ID de Origem     | ID de Destino      | Descrição                              |
|-------------|------------------|--------------------|----------------------------------------|
| Teensy → ESP32 | `own_command_id` | —                  | Comando unicast                         |
| Teensy → ESP32 | `can_command_base_id` | —             | Comando broadcast                       |
| ESP32 → Teensy | —                | `own_status_id`    | Resposta compacta de estado            |
| ESP32 → Teensy | —                | `own_position_id`  | Posições e temperatura                 |
| ESP32 → Teensy | —                | `own_event_id`     | Eventos assíncronos (heartbeat, ACK)   |

---

## DLC e correlação de comandos

O firmware ESP32-S3 atual **não implementa byte de sequência**. Ele não lê, armazena, devolve nem usa sequência para eliminar duplicatas. Alguns handlers toleram bytes excedentes, mas esses bytes não fazem parte do contrato e não devem ser enviados.

O Teensy deve sempre transmitir o DLC exato indicado para cada opcode. Isso é especialmente importante no comando de laser, em que o quarto byte é a parte alta do nível, e em `MOVE_PROFILE`/`MOVE_SYNC`, que ocupam os 8 bytes do frame.

Os eventos `ACK`, `DONE` e `ERROR` identificam somente o nó e o opcode. Portanto, não envie vários movimentos do mesmo opcode ao mesmo nó se for necessário correlacionar individualmente cada conclusão.

---

## OpCodes de Comando (Teensy → ESP32)

### `0x01` — `CAN_OP_PING`

**DLC esperado:** 3 bytes

| Byte | Conteúdo          |
|------|-------------------|
| 0    | Opcode (`0x01`)   |
| 1    | Arg0 (dado arbitrário) |
| 2    | Arg1 (dado arbitrário) |

**Resposta:** `CAN_EVT_PONG` com os mesmos Arg0 e Arg1.

---

### `0x02` — `CAN_OP_STATUS_REQUEST`

**DLC esperado:** 1 byte

| Byte | Conteúdo          |
|------|-------------------|
| 0    | Opcode (`0x02`)   |

**Resposta:** dois frames de 8 bytes: `CAN_EVT_STATUS` em `own_status_id` e a telemetria de posição/temperatura em `own_position_id`.

---

### `0x10` — `CAN_OP_ENABLE`

**DLC esperado:** 2 bytes

| Byte | Conteúdo                              |
|------|---------------------------------------|
| 0    | Opcode (`0x10`)                       |
| 1    | Enable flag (`0x01` = ligar, `0x00` = desligar) |

**Ação:** Liga/desliga os drivers de passo via `hardware_set_driver_enable()`.

**Resposta:** `CAN_EVT_ACK` ou `CAN_EVT_ERROR`.

---

### `0x11` — `CAN_OP_SPEED`

**DLC esperado:** 2 bytes

| Byte | Conteúdo                              |
|------|---------------------------------------|
| 0    | Opcode (`0x11`)                       |
| 1    | Nível de velocade (1–5)               |

**Níveis de velocidade:**

| Level | `move_delay_us` | Velocidade relativa |
|-------|-----------------|---------------------|
| 1     | 2000 µs         | Mais lenta          |
| 2     | 800 µs          | Lenta               |
| 3     | 400 µs          | Média               |
| 4     | 150 µs          | Rápida              |
| 5     | 50 µs           | Mais rápida         |

**Resposta:** `CAN_EVT_ACK` ou `CAN_EVT_ERROR`.

---

### `0x12` — `CAN_OP_AXIS_SPEED`

Define e persiste a velocidade solicitada de um eixo. O firmware a converte para o período de passos disponível, portanto pode haver quantização.

**DLC esperado:** 6 bytes

| Byte | Conteúdo |
|------|----------|
| 0 | Opcode (`0x12`) |
| 1 | Eixo (`C`, `A` ou `Z`; aliases `X`/`Y` aceitos) |
| 2–5 | Velocidade `float32`, little-endian (`deg/s` para C/A, `mm/s` para Z) |

### `0x13` — `CAN_OP_AXIS_ACCEL`

Mesmo formato de `CAN_OP_AXIS_SPEED`, com aceleração `float32` em `deg/s²` ou `mm/s²`.

**DLC esperado:** 6 bytes

### `0x14` — `CAN_OP_MOVE_PROFILE`

Carrega um perfil de uso único para o próximo `MOVE`/`MOVE_FORCE` do mesmo eixo ou para o próximo `MOVE_SYNC`. Esse comando não grava NVS.

**DLC esperado:** 8 bytes

| Byte | Conteúdo |
|------|----------|
| 0 | Opcode (`0x14`) |
| 1 | Eixo (`C`, `A` ou `Z`; aliases legados `X`/`Y` aceitos) |
| 2–5 | Velocidade `float32` little-endian; `0` usa o padrão |
| 6–7 | Aceleração `uint16` little-endian; `0` usa o padrão |

O Teensy deve enviar o perfil imediatamente antes do movimento correspondente.

---

### `0x20` — `CAN_OP_MOVE`

**DLC esperado:** 6 bytes

| Byte | Conteúdo                              |
|------|---------------------------------------|
| 0    | Opcode (`0x20`)                       |
| 1    | Eixo (`'C'`, `'A'` ou `'Z'`; aliases `'X'`/`'Y'`) |
| 2    | Steps byte 0 (LSB)                    |
| 3    | Steps byte 1                        |
| 4    | Steps byte 2                        |
| 5    | Steps byte 3 (MSB)                    |

**Ação:** Enfileira comando de movimento relativo em `motion_queue`. O ESP32 executa o movimento de forma assíncrona.

**Resposta:** `CAN_EVT_ACK` (enfileirado) ou `CAN_EVT_ERROR`.

---

### `0x21` — `CAN_OP_HOME`

**DLC esperado:** 2 bytes

| Byte | Conteúdo                              |
|------|---------------------------------------|
| 0    | Opcode (`0x21`)                       |
| 1    | Eixo (`'C'`, `'A'`, `'Z'`; aliases `'X'`/`'Y'`, sem distinção de caixa) |

**Ação:** Enfileira comando de homing. C/A ajustam para o home salvo. Z pode levar dezenas de segundos, pois busca o fim de curso por até 30000 passos, libera o sensor e aplica o alívio configurado.

**Resposta:** `CAN_EVT_ACK` (enfileirado) ou `CAN_EVT_ERROR`.

---

### `0x22` — `CAN_OP_MOVE_FORCE`

Mesmo payload de `CAN_OP_MOVE`, mas executa em malha aberta, sem correção/limites dos encoders C/A. Os limites físicos do eixo Z continuam ativos.

**DLC esperado:** 6 bytes

---

### `0x23` — `CAN_OP_MOVE_SYNC`

Dispara um movimento relativo sincronizado de C, A e Z no próprio ESP32. O payload usa unidades físicas compactas e ocupa um único frame CAN Classic.

**DLC esperado:** 8 bytes

| Byte | Conteúdo |
|------|----------|
| 0 | Opcode (`0x23`) |
| 1–2 | ΔC `int16` little-endian em décimos de grau (`0,1°`) |
| 3–4 | ΔA `int16` little-endian em décimos de grau (`0,1°`) |
| 5–6 | ΔZ `int16` little-endian em centésimos de milímetro (`0,01 mm`) |
| 7 | Flags: bit 0 = forçar C/A sem encoder; bits 1–7 reservados e obrigatoriamente zero |

O ESP32 converte as unidades físicas para passos usando `steps_per_rev`, microsteps e polia Z gravados localmente, enfileira um único `MOTION_CMD_MOVE_SYNC` e inicia os três canais RMT pelo mesmo planejador. A faixa representável é ±3276,7° para C/A e aproximadamente ±327,67 mm para Z.

Perfis `CAN_OP_MOVE_PROFILE` opcionais podem ser enviados para C, A e Z imediatamente antes do trigger. As velocidades são consumidas por eixo; a aceleração compartilhada deve ser igual nos perfis usados. Sem perfil, o ESP32 usa a configuração persistida.

**Resposta:** `CAN_EVT_ACK` quando o movimento sincronizado é enfileirado, seguido de `CAN_EVT_DONE` ou `CAN_EVT_ERROR` ao terminar.

---

### `0x24` — `CAN_OP_STOP`

Parada imediata. Interrompe o movimento em curso (RMT ou bit-bang, latência ≤ 5 ms), descarta todos os comandos na fila e é aceito em qualquer estado, inclusive durante OTA. Envie no ID de broadcast para parar todos os nós.

**DLC esperado:** 2 bytes

| Byte | Conteúdo |
|------|----------|
| 0 | Opcode (`0x24`) |
| 1 | Flags: bit 0 = também apagar os dois lasers (E-STOP); bits 1–7 reservados |

**Resposta:** `CAN_EVT_ACK`. O movimento interrompido termina com `CAN_EVT_ERROR` (`0x0C`, `ESP_ERR_NOT_FINISHED`), e cada comando descartado da fila também recebe `CAN_EVT_ERROR` com `0x0C`. Se o STOP interromper um `MOVE_SYNC` com Z, a posição Z fica incerta e é preciso `HOME Z`.

---

### `0x30` — `CAN_OP_LASER`

**DLC esperado:** 4 bytes no formato atual de 16 bits. O ESP32 ainda aceita o formato legado de 3 bytes.

| Byte | Conteúdo                              |
|------|---------------------------------------|
| 0    | Opcode (`0x30`)                       |
| 1    | Índice do laser (1 ou 2)              |
| 2–3  | Nível lógico do laser (`uint16`, little-endian, 0–4095) |

**Ação:** Configura o nível lógico do laser via `hardware_set_laser_level()`. O nível é aplicado diretamente ao duty PWM; o emissor deve limitá-lo a 0–4095.

**Resposta:** `CAN_EVT_ACK` ou `CAN_EVT_ERROR`.

---

### `0x31` — `CAN_OP_FAN`

**DLC esperado:** 2 bytes

| Byte | Conteúdo                              |
|------|---------------------------------------|
| 0    | Opcode (`0x31`)                       |
| 1    | Modo do fan                           |

**Modos do fan:**

| Valor | Modo                  | Ação                          |
|-------|-----------------------|-------------------------------|
| 0     | `FAN_MODE_MANUAL_OFF` | Fan desligado                 |
| 1     | `FAN_MODE_MANUAL_ON`  | Fan ligado                    |
| 2     | `FAN_MODE_AUTO`       | Fan controlado por temperatura|

**Resposta:** `CAN_EVT_ACK` ou `CAN_EVT_ERROR`.

---

## Eventos e Respostas (ESP32 → Teensy)

### `0x80` — `CAN_EVT_HEARTBEAT`

Enviado periodicamente pelo ESP32 a cada 1000 ms quando o barramento CAN está online.

| Byte | Conteúdo          |
|------|-------------------|
| 0    | Evento (`0x80`)   |
| 1    | Node ID do ESP32  |
| 2    | Node ID repetido pelo firmware atual |
| 3–7  | Reservado (0)     |

---

### `0x81` — `CAN_EVT_PONG`

Resposta ao `CAN_OP_PING`.

| Byte | Conteúdo          |
|------|-------------------|
| 0    | Evento (`0x81`)   |
| 1    | Node ID           |
| 2    | Arg0 do ping      |
| 3    | Arg1 do ping      |
| 4–7  | Reservado (0)     |

---

### `0x82` — `CAN_EVT_STATUS`

Resposta ao `CAN_OP_STATUS_REQUEST`. Payload de 8 bytes.

| Byte | Conteúdo                              |
|------|---------------------------------------|
| 0    | Evento (`0x82`)                       |
| 1    | Node ID do ESP32                      |
| 2    | Flags de estado (bitmask)             |
| 3–4  | Nível lógico do laser 1 (`uint16`, little-endian) |
| 5–6  | Nível lógico do laser 2 (`uint16`, little-endian) |
| 7    | Fan: bit 0 saída, bits 1–3 modo, bits 4–7 nível de velocidade |

### Byte 2 — Flags de estado (bitmask):

| Bit | Máscara  | Significado                    |
|-----|----------|--------------------------------|
| 0   | `0x01`   | `drivers_enabled`              |
| 1   | `0x02`   | `z_bloqueado`                  |
| 2   | `0x04`   | `alarme_z_ativo`               |
| 3   | `0x08`   | `temp_valid`                   |
| 4   | `0x10`   | `tmc_uart_ready`               |
| 5   | `0x20`   | `can_online`                   |
| 6   | `0x40`   | `pos_v2`: o frame de posição seguinte usa o formato v2 (ângulos com sinal) |

### Telemetria de posição e temperatura (`own_position_id`)

| Byte | Conteúdo |
|------|----------|
| 0–1 | C: v2 = décimos de grau, `int16` com sinal (`INT16_MIN` = inválido); v1 = centigraus `uint16` 0..36000 (`0xFFFF` = inválido) |
| 2–3 | A: mesmo formato de C |
| 4–5 | Z em passos, `uint16` little-endian (saturado em `65534`) |
| 6–7 | Temperatura em décimos de °C, `int16` little-endian (`INT16_MIN` = inválida) |

---

### `0x83` — `CAN_EVT_ACK`

Confirmação de que um comando síncrono foi processado ou de que um movimento/homing foi aceito na fila.

| Byte | Conteúdo          |
|------|-------------------|
| 0    | Evento (`0x83`)   |
| 1    | Node ID           |
| 2    | Opcode do comando |
| 3    | 0 (sucesso)       |
| 4–7  | Reservado (0)     |

---

### `0x84` — `CAN_EVT_DONE`

Indica que o processamento de um comando de movimento ou homing terminou. Para movimentos limitados, `DONE` pode significar que o firmware executou apenas a parcela permitida ou não gerou passos por já estar no limite.

| Byte | Conteúdo          |
|------|-------------------|
| 0    | Evento (`0x84`)   |
| 1    | Node ID           |
| 2    | Opcode do comando |
| 3    | 0 (sucesso)       |
| 4–7  | Reservado (0)     |

---

### `0xE0` — `CAN_EVT_ERROR`

Indica que um comando falhou.

| Byte | Conteúdo          |
|------|-------------------|
| 0    | Evento (`0xE0`)   |
| 1    | Node ID           |
| 2    | Opcode do comando |
| 3    | Byte baixo do código `esp_err_t` |
| 4–7  | Reservado (0)     |

**Bytes baixos de erro comuns:**

| Valor              | Significado                    |
|--------------------|--------------------------------|
| `0x00` | `ESP_OK` |
| `0x01` | `ESP_ERR_NO_MEM` |
| `0x02` | `ESP_ERR_INVALID_ARG` |
| `0x03` | `ESP_ERR_INVALID_STATE` |
| `0x04` | `ESP_ERR_INVALID_SIZE` |
| `0x05` | `ESP_ERR_NOT_FOUND` |
| `0x06` | `ESP_ERR_NOT_SUPPORTED` |
| `0x07` | `ESP_ERR_TIMEOUT` |
| `0xFF` | `ESP_FAIL` |

---

## Validação de Configuração CAN

O ESP32 valida as configurações CAN antes de iniciar o periférico TWAI:

| Parâmetro     | Restrição                          |
|---------------|------------------------------------|
| `node_id`     | 1–10                               |
| `can_bitrate` | 125000, 250000, 500000, 1000000   |
| `can_command_base_id + 10` | ≤ `0x7FF` (limite de 11 bits) |
| `can_status_base_id + 10`  | ≤ `0x7FF`                       |
| `can_status_base_id + 0x10 + 10` | ≤ `0x7FF`                 |
| `can_event_base_id + 10`   | ≤ `0x7FF`                       |

Se a configuração for inválida, o ESP32 não inicia o TWAI e reporta `ESP_ERR_INVALID_ARG`.

---

## Implementação do bridge (Teensy 4.1 + Zephyr)

### Inicialização

1. Configurar o controlador **FlexCAN** do Teensy 4.1 com o mesmo bitrate dos ESP32 (padrão: 500 kbps).
2. Transmitir comandos unicast em `0x200 + node_id` ou broadcast em `0x200`.
3. Configurar filtros de recepção para status `0x281..0x28A`, posição `0x291..0x29A` e eventos `0x301..0x30A`.
4. Usar somente IDs padrão de 11 bits e frames Classic CAN de até 8 bytes.

### Envio de Comandos

Para enviar um comando a um ESP32:

```c
// Exemplo: enviar CAN_OP_MOVE para node_id = 1
uint16_t target_id = can_command_base_id + node_id;  // 0x201
uint8_t payload[6] = {0x20, 'C', 0x00, 0x10, 0x00, 0x00};
// payload[0] = opcode CAN_OP_MOVE (0x20)
// payload[1] = eixo canônico 'C'
// payload[2-5] = steps (little-endian, int32_t)

can_send(target_id, payload, 6);
```

### Recebimento de Respostas

O Teensy deve configurar filtros separados para:
- IDs de status (`can_status_base_id + node_id`)
- IDs de posição/temperatura (`can_status_base_id + 0x10 + node_id`)
- IDs de evento (`can_event_base_id + node_id`)

Ou usar filtros por grupo que cubram as faixas de todos os ESP32.

### Tratamento de Respostas

| Evento recebido | Ação esperada pelo Teensy                              |
|-----------------|-------------------------------------------------------|
| `CAN_EVT_ACK`   | Comando rápido processado ou movimento enfileirado     |
| `CAN_EVT_DONE`  | Processamento do movimento/homing encerrado            |
| `CAN_EVT_ERROR` | Comando falhou — byte 2 é o opcode e byte 3 é o código de erro |
| `CAN_EVT_PONG`  | Resposta ao ping — verificar Arg0 e Arg1              |
| `CAN_EVT_STATUS`| Atualizar estado do ESP32                             |
| `CAN_EVT_HEARTBEAT` | Manter conexão ativa                               |

### Timeout e retransmissão

- Um timeout de 500 ms pode ser usado para esperar a resposta inicial `ACK`, `PONG`, `STATUS` ou `ERROR`.
- O timeout de `DONE` deve ser específico da operação; movimentos e homing podem durar vários segundos.
- Não retransmita automaticamente movimentos após timeout. Como não existe deduplicação nem sequência, uma repetição pode executar o movimento duas vezes.
- Antes de uma repetição deliberada, consulte `STATUS` e `POS`.

### Exemplo de máquina de estados do Teensy

```
ESTADO_IDLE → envia comando → ESPERA_RESPOSTA
    │                                    │
    │                              recebe ACK/DONE/ERROR
    │                                    │
    └────────────────────────────────────┘
        │
        ├── ACK de comando rápido → volta a IDLE
        ├── ACK de movimento → espera DONE/ERROR
        ├── ERROR → reporta erro, volta a IDLE
        └── timeout → consulta estado antes de decidir
```

---

## Considerações de Timing

| Operação                  | Regra de timing | Observação                              |
|---------------------------|-----------------|-----------------------------------------|
| Comando síncrono          | Esperar até 500 ms | Responde com PONG, STATUS, ACK ou ERROR |
| MOVE/MOVE_FORCE/HOME      | ACK inicial; DONE variável | A conclusão depende da fila e do percurso |
| HOME Z                    | Pode durar dezenas de segundos | Busca até 30000 passos, seguida da liberação |
| Heartbeat                 | A cada 1000 ms  | Assíncrono                              |

Os valores acima são políticas do bridge, não garantias rígidas de latência do ESP32.

---

## Resumo do Protocolo

| Aspecto              | Detalhe                                                        |
|----------------------|----------------------------------------------------------------|
| Modo                 | Standard 11-bit identifiers                                    |
| Bitrate              | 500 kbps (configurável: 125k, 250k, 500k, 1000k)             |
| Topologia            | 1 bridge Teensy + até 10 nós ESP32                            |
| Filtragem            | Hardware por grupo `0x780`; software aceita ID próprio ou broadcast |
| Broadcast            | Suportado em `can_command_base_id`; evitar em consultas       |
| Sequenciamento       | Não implementado                                                |
| Confirmação          | ACK para aceitação; DONE/ERROR posterior em movimentos          |
| Heartbeat            | 1000 ms                                                        |
| Payload máximo       | 8 bytes                                                        |
| Endianness           | Little-endian para multi-bytes                                 |
