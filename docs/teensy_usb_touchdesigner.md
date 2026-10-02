# Interface Teensy USB <-> TouchDesigner

Este documento descreve a integração do TouchDesigner com o bridge. Os nomes canônicos dos eixos são **C** (base), **A** (pivot) e **Z** (linear). `X` e `Y` existem somente como aliases legados de `C` e `A`.

> Guia prático para montar a rede do TD (comandos, respostas, tempos e valores recomendados): [touchdesigner_parametrizacao.md](./touchdesigner_parametrizacao.md).

## Visao geral

O Teensy 4.1 atua como uma ponte entre o TouchDesigner e a rede CAN dos nodes ESP32.

Fluxo de comunicacao:

```text
TouchDesigner <-> USB CDC ACM (porta COM virtual) <-> Teensy 4.1 <-> CAN <-> ESP32 nodes
```

O TouchDesigner conversa com o Teensy por texto ASCII, uma linha por comando. O Teensy converte cada comando USB em um frame CAN e repassa de volta, tambem em texto, os eventos recebidos dos ESP32.

Este documento descreve apenas o lado `TouchDesigner <-> USB`.

---

## Regras de transporte USB

- A conexao aparece no computador como uma porta COM virtual USB CDC ACM.
- No Windows, prefira localizar a COM por VID/PID e número de série em vez de fixar o número da porta.
- DTR deve ficar ativo quando disponivel, mas as respostas nao dependem dele.
- Aguarde cerca de 500 ms entre abrir a COM e enviar o primeiro comando.
- Cada comando deve ser enviado como uma linha ASCII terminada por `\n`, `\r` ou `\r\n`.
- O Teensy ignora espacos em branco no inicio da linha.
- A letra do comando nao diferencia maiusculas de minusculas.
- As respostas do Teensy sao sempre enviadas como linhas terminadas em `\r\n`.
- Se o host propagar DTR, o Teensy envia `TEENSY_READY 1`; no Windows esse
  banner nao e garantido. Use uma resposta a comando para confirmar o canal.
- Nao existe frame binario no lado USB: tudo e texto.

---

## Regras gerais do protocolo USB

- `node_id` válido para unicast: `1..10`.
- `node_id = 0` representa broadcast CAN e pode ser usado nos comandos de atuação `M`, `MF`, `MS`, `MSF`, `H`, `E`, `S`, `L` e `F`.
- `P` e `R` exigem um node de `1..10` para evitar respostas simultâneas de todos os nós.
- O broadcast entrega um único frame a todos os ESP32, mas não garante que os motores iniciem no mesmo instante.
- O bridge envia o DLC exato, sem byte de sequência. O firmware ESP32 atual não implementa correlação nem deduplicação por sequência.
- O TouchDesigner nao precisa montar CAN ID, DLC ou payload binario.
- As respostas CAN sao assincronas. Um comando enviado agora pode gerar resposta alguns milissegundos depois, ou muito mais tarde no caso de movimentos.

---

## Comandos enviados pelo TouchDesigner

Formato geral:

```text
<COMANDO> <arg1> <arg2> ...
```

### 1. Mover eixo

Sintaxe:

```text
M <node_id> <axis> <steps>
```

- `node_id`: `0..10`
- `axis`: `C`, `A` ou `Z`
- `steps`: inteiro com sinal

Exemplo:

```text
M 1 C 4096
M 2 Z -1200
```

Comportamento esperado:

- O ESP32 deve responder com `ACK`.
- Quando o movimento terminar, o ESP32 deve responder com `DONE`.

O ESP32 ainda aceita `X` como alias de `C` e `Y` como alias de `A`, mas o TouchDesigner deve emitir os nomes canônicos.

---

### 2. Mover eixo em malha aberta

Sintaxe:

```text
MF <node_id> <axis> <steps>
```

- Em C/A, ignora encoder e limites angulares.
- Em Z, o bloqueio, o fim de curso, o zero e o curso máximo continuam ativos.
- Usa o opcode CAN `0x22`.

Exemplo:

```text
MF 2 A -200
```

---

### 2.1. Mover C, A e Z sincronizados

Sintaxe normal e forçada:

```text
MS <node_id> <graus_c> <graus_a> <mm_z>
MSF <node_id> <graus_c> <graus_a> <mm_z>
```

- C/A são deslocamentos relativos com resolução CAN de `0,1°`.
- Z é posição absoluta com resolução CAN de `0,02 mm` (±655,34 mm: cobre o curso de 480 mm).
- O Teensy monta um único frame CAN de opcode `0x23` e DLC 8.
- O ESP32 converte as unidades físicas usando sua configuração e inicia os três canais RMT juntos.
- `MSF` ignora encoder e limites angulares de C/A; as proteções físicas de Z continuam ativas.
- Velocidade e aceleração vêm da configuração persistida no ESP32.

Exemplos:

```text
MS 1 90.0 -45.0 10.00
MSF 2 5.0 5.0 -2.50
```

---

### 2.2. Streaming de posição absoluta (U / UF) — recomendado para o TouchDesigner

Sintaxe:

```text
U  <node_id> <graus_c> <graus_a> <mm_z> <laser1> <laser2>
UF <node_id> <graus_c> <graus_a> <mm_z> <laser1> <laser2>
```

- Coordenadas **absolutas**: C/A em graus (resolução 0,01°, faixa ±2621,43°), Z em mm (0,02 mm, até 480 mm de curso).
- `laser1`/`laser2`: PWM 12 bits (`0..4095`). O frame de laser só é reenviado quando o nível muda
  (ou a cada 250 ms, para se corrigir após um reboot do node ou um `ESTOP`).
- `UF` ignora encoder e limites de C/A (malha aberta); as proteções de Z continuam ativas.
- Sucesso é **silencioso** (sem `ACK`/`DONE`/`TEENSY_OK`); só falhas geram `ERROR`/`TEENSY_ERROR`.
- **Commit sincronizado (`USYNC 1`, padrão):** o Teensy marca cada `U` como "aguardar" e, quando a COM
  fica ~0,3 ms sem bytes (fim da rajada do frame do TD), envia um broadcast de commit (`0x26`). Todos
  os nodes aplicam o alvo no mesmo instante, sem a diferença de ~1–2 ms entre o node 1 e o node 10.
  Mande os `U` de todos os nodes de um frame juntos, em sequência. Se um commit se perder, o node
  aplica sozinho após 10 ms. `USYNC 0` volta a aplicar cada `U` ao chegar; `USYNC` informa o modo.
- O Teensy mantém até 4 frames CAN em voo (no máximo 1 por node, preservando a ordem de cada node).
  Broadcasts (`STOP`/`ESTOP`, commit, `CANBR`) esperam os frames pendentes saírem antes de serem enviados.
- Envie um `U` por node a cada frame do TD, em taxa constante (30–120 Hz; 60 Hz ou mais recomendado).
  A taxa pode mudar a qualquer momento: o node mede o intervalo entre frames.

Como o node executa:

- Não há fila: cada `U` só atualiza o alvo de um seguidor em tempo real (segmentos de 5 ms).
- O node estima a velocidade do alvo pela diferença entre frames (feedforward) e extrapola o alvo
  entre eles, compensando o atraso interno. Numa trajetória contínua o erro de acompanhamento
  fica em ~1° a 60–120 Hz (contra ~20° de um seguidor só de posição).
- Aceleração (`ACCEL`) e jerk (`MOTION TJERK`) limitam a curva S; a velocidade máxima é `SPEED_MAX`.
- Se o TD parar de mandar frames, o alvo para onde estava; 0,5 s depois o seguidor encerra.
- Trajetórias com mudanças bruscas (degraus) são suavizadas pelos limites de aceleração/jerk.

---

### 3. Home de eixo

Sintaxe:

```text
H <node_id> <axis>
```

- `node_id`: `0..10`
- `axis`: `C`, `A` ou `Z`

Exemplo:

```text
H 1 C
H 3 Z
```

Comportamento esperado:

- O ESP32 responde com `ACK` quando enfileira o homing.
- Depois responde com `DONE` quando termina.

---

### 4. Habilitar ou desabilitar drivers

Sintaxe:

```text
E <node_id> <enable>
```

- `enable = 1`: habilita drivers
- `enable = 0`: desabilita drivers

Exemplo:

```text
E 1 1
E 1 0
```

Resposta esperada:

- `ACK` em caso de sucesso
- `ERROR` em caso de falha

---

### 5. Ajustar velocidade

Sintaxe:

```text
S <node_id> <level>
```

- `level`: `1..5`

Exemplo:

```text
S 1 3
S 4 5
```

Resposta esperada:

- `ACK` ou `ERROR`

---

### 6. Ajustar laser

Sintaxe:

```text
L <node_id> <laser_index> <pwm>
```

- `laser_index`: normalmente `1` ou `2`
- `pwm`: `0..4095`, transmitido como `uint16` little-endian

Exemplo:

```text
L 1 1 2048
L 1 2 4095
```

Resposta esperada:

- `ACK` ou `ERROR`

---

### 7. Ajustar modo do fan

Sintaxe:

```text
F <node_id> <mode>
```

- `mode = 0`: manual off
- `mode = 1`: manual on
- `mode = 2`: auto

Exemplo:

```text
F 1 0
F 1 2
```

Resposta esperada:

- `ACK` ou `ERROR`

Observacao:

- Se uma linha contiver argumentos extras depois do `mode`, o Teensy ignora o excedente e usa apenas os dois primeiros argumentos numericos.

---

### 8. Ping

Sintaxe:

```text
P <node_id> [arg0] [arg1]
```

- `arg0` e `arg1` sao opcionais no lado USB.
- Se omitidos, o Teensy envia ambos como `0`.

Exemplo:

```text
P 1
P 1 12 34
```

Resposta esperada:

```text
PONG <node_id> <arg0> <arg1>
```

---

### 9. Solicitar status

Sintaxe:

```text
R <node_id>
```

Exemplo:

```text
R 1
R 7
```

Resposta esperada:

```text
STATUS <node_id> <flags> <laser1> <laser2> <fan_on> <fan_mode> <speed>
POS <node_id> <pos_c> <pos_a> <z_steps> <temp_c>
```

Para diagnosticar o payload sem mudar as respostas usadas pelo app, habilite
temporariamente o espelho dos frames crus:

```text
RAW 1
R 1
RAW 0
```

Enquanto ativo, o Teensy emite antes da linha decodificada:

```text
CAN_RAW 291 8 D2 04 7B 00 00 00 FA 00
```

---

### 10. Bitrate do barramento CAN (CANBR)

```text
CANBR                 -> TEENSY_OK CANBR <bitrate atual>
CANBR <bps>           -> broadcast para todos os nodes trocarem + troca o Teensy
CANBR_LOCAL <bps>     -> troca só o Teensy
```

- Valores aceitos: `125000`, `250000`, `500000`, `1000000`. O Teensy inicia sempre em **1 Mbps**.
- Os nodes gravam o novo bitrate na NVS e trocam ~30 ms depois do `ACK` (que ainda sai no bitrate antigo).
- Migração de nodes que estão em 500 kbps: `CANBR_LOCAL 500000` e depois `CANBR 1000000`.
- Nodes gravados de fábrica com o firmware novo já nascem em 1 Mbps.

---

## Respostas enviadas pelo Teensy ao TouchDesigner

Todas as respostas chegam como linhas de texto independentes.

### HEARTBEAT

Formato:

```text
HEARTBEAT <node_id>
```

Uso:

- Indica que o node esta vivo e online no barramento CAN.

Exemplo:

```text
HEARTBEAT 1
```

---

### PONG

Formato:

```text
PONG <node_id> <arg0> <arg1>
```

Uso:

- Resposta ao comando `P`.
- `arg0` e `arg1` repetem os valores enviados no ping.

Exemplo:

```text
PONG 1 12 34
```

---

### STATUS

Formato:

```text
STATUS <node_id> <flags> <laser1> <laser2> <fan_on> <fan_mode> <speed>
```

Campos:

- `node_id`: node que respondeu
- `flags`: bitmask de estado
- `laser1`: nível lógico do laser 1, `0..4095`
- `laser2`: nível lógico do laser 2, `0..4095`
- `fan_on`: `0` ou `1`
- `fan_mode`: valor do modo do fan extraido do status
- `speed`: nivel de velocidade atual, normalmente `1..5`

Exemplo:

```text
STATUS 1 41 2048 0 1 2 3
```

Interpretacao do exemplo:

- `node_id = 1`
- `flags = 41`
- `laser1 = 2048`
- `laser2 = 0`
- `fan_on = 1`
- `fan_mode = 2`
- `speed = 3`

Bits de `flags`:

- bit 0 / `0x01`: `drivers_enabled`
- bit 1 / `0x02`: `z_bloqueado`
- bit 2 / `0x04`: `alarme_z_ativo`
- bit 3 / `0x08`: `temp_valid`
- bit 4 / `0x10`: `tmc_uart_ready`
- bit 5 / `0x20`: `can_online`

---

### ACK

Formato:

```text
ACK <node_id> <opcode_hex>
```

Uso:

- Um comando rápido foi processado ou um movimento/homing foi aceito na fila.
- `ACK` não significa que o eixo já chegou ao destino.

Exemplo:

```text
ACK 1 20
ACK 1 31
```

---

### DONE

Formato:

```text
DONE <node_id> <opcode_hex>
```

Uso:

- Processamento de movimento ou homing encerrado.
- Um movimento limitado pode terminar com `DONE` mesmo executando menos passos que o solicitado.

Exemplo:

```text
DONE 1 20
DONE 1 21
```

---

### ERROR

Formato:

```text
ERROR <node_id> <opcode_hex> <error_hex>
```

Uso:

- O node respondeu com falha para o comando recebido.

Exemplo:

```text
ERROR 1 31 02
```

Observacao:

- `error_hex` e um codigo retornado pelo node ESP32.
- A interpretacao detalhada do erro deve seguir `docs/can_protocol.md`.

---

### POS

Formato:

```text
POS <node_id> <pos_c> <pos_a> <z_steps> <temp_c>
```

Exemplo:

```text
POS 1 12.34 98.70 2048 26.5
```

Posicoes podem ser negativas (limites de +-540 graus) e posicoes invalidas
aparecem como `nan`; temperatura invalida aparece como
`-99.9`. A formatacao e feita com inteiros no firmware e nao depende de
suporte a `printf` de ponto flutuante.

`POS` não é um broadcast periódico do ESP32. Ele é enviado junto com `STATUS` em resposta a `R`, salvo se o Teensy fizer polling automático.

---

### TEENSY_ERROR

Essas mensagens sao geradas localmente pelo Teensy antes mesmo de haver resposta CAN.

Formatos atuais:

```text
TEENSY_READY 1
TEENSY_ERROR INVALID_NODE_ID <node_id>
TEENSY_ERROR INVALID_PAYLOAD_LENGTH
TEENSY_ERROR INVALID_ARGUMENT <campo>
TEENSY_ERROR LINE_TOO_LONG
TEENSY_ERROR CAN_SEND_FAILED <node_id> <err>
TEENSY_ERROR CAN_TX_FAILED <node_id> <err>
TEENSY_ERROR CAN_TX_BUSY <node_id>
TEENSY_ERROR CAN_FILTER_ATTACH_FAILED <status_id> <pos_id> <event_id>
TEENSY_ERROR UNKNOWN_COMMAND
```

Uso:

- Indicam erro local de parsing, validacao ou envio para o barramento CAN.
- `CAN_TX_FAILED` indica que o controlador iniciou a transmissao, mas recebeu
  erro do barramento, como ausencia de ACK.
- `CAN_TX_BUSY` indica que ainda existe uma transmissao pendente. Isso mantem
  a COM responsiva mesmo sem transceiver ou sem outro node no CAN.

Exemplos:

```text
TEENSY_ERROR INVALID_NODE_ID 200
TEENSY_ERROR UNKNOWN_COMMAND
```

---

## Relacao entre comandos USB e opcodes CAN

| USB | Significado | Opcode CAN |
|-----|-------------|------------|
| `P` | Ping | `0x01` |
| `R` | Status request | `0x02` |
| `E` | Enable | `0x10` |
| `S` | Speed | `0x11` |
| `M` | Move | `0x20` |
| `H` | Home | `0x21` |
| `MF` | Move em malha aberta | `0x22` |
| `MS` / `MSF` | Move C+A+Z sincronizado | `0x23` |
| `U` / `UF` | Alvo absoluto em streaming (+ lasers `0x32` quando mudam) | `0x25` |
| `CANBR` | Troca de bitrate (broadcast) | `0x15` |
| (automático, `USYNC 1`) | Commit sincronizado dos `U` | `0x26` |
| `L` | Laser | `0x30` |
| `F` | Fan | `0x31` |

O TouchDesigner normalmente nao precisa usar esses opcodes diretamente, mas eles aparecem em `ACK`, `DONE` e `ERROR`.

---

## Fluxos tipicos

### Fluxo 1: mover eixo

TouchDesigner envia:

```text
M 1 C 1000
```

Resposta esperada:

```text
ACK 1 20
DONE 1 20
```

`ACK` pode chegar quase imediatamente. `DONE` chega apenas quando o movimento termina.

---

### Fluxo 2: requisitar status

TouchDesigner envia:

```text
R 1
```

Resposta esperada:

```text
STATUS 1 41 0 0 1 2 3
POS 1 12.34 98.70 2048 25.0
```

---

### Fluxo 3: teste de conectividade

TouchDesigner envia:

```text
P 1 7 99
```

Resposta esperada:

```text
PONG 1 7 99
```

---

## Recomendacoes para uso no TouchDesigner

- Tratar a interface como protocolo orientado a linha.
- Enviar exatamente um comando por linha.
- Fazer o parsing das respostas com `split()` por espaco.
- Correlacionar respostas por `node_id`.
- Para `MOVE`, `MF`, `MS`, `MSF` e `HOME`, esperar primeiro `ACK` e depois `DONE` ou `ERROR`.
- Não manter vários comandos do mesmo opcode pendentes para o mesmo node: os eventos não carregam eixo nem sequência.
- Após timeout, consultar `R` antes de repetir um movimento, evitando execução dupla.
- Tratar `HEARTBEAT` como evento assincrono.
- Tratar `TEENSY_ERROR` separado de `ERROR`, porque `TEENSY_ERROR` vem do bridge e `ERROR` vem do ESP32.

---

## Contrato de implementação do bridge

- O protocolo CAN atual não possui sequência funcional; o bridge envia sempre o DLC exato.
- O `node_id` é obtido subtraindo a base correspondente do CAN ID recebido; somente resultados de `1..10` são aceitos.
- Os filtros de recepção do sistema de 10 nodes cobrem status `0x281..0x28A`, posição `0x291..0x29A` e eventos `0x301..0x30A`.
- Somente `node_id` de `1..10` e aceito, evitando sobreposicao entre essas faixas.
- A COM CDC transporta apenas o protocolo. Logs do Zephyr saem no UART fisico dos pinos 0/1 a 115200 8N1.

---

## Referencias

- Protocolo CAN dos ESP32: [can_protocol.md](./can_protocol.md)
- Protocolo ASCII completo do bridge: [teensy_serial_protocol.md](./teensy_serial_protocol.md)
- Detecção automática da COM no Windows: [windows_com.md](./windows_com.md)
- Firmware do bridge USB/CAN: [src/main.c](../src/main.c)
