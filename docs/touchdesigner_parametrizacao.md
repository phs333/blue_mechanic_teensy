# Guia de parametrização do TouchDesigner — Blue Mechanic

Referência para montar a rede do TouchDesigner (TD) que controla os 10 nós ESP32 pelo bridge
Teensy 4.1. Cobre conexão, comandos, sintaxe, respostas, tempos de resposta e valores
recomendados.

```text
TouchDesigner ──USB (COM virtual, texto)──► Teensy 4.1 ──CAN 1 Mbps──► Node 1..10 (ESP32-S3)
              ◄── respostas em texto ─────            ◄── eventos ───
```

> Firmware de referência: Teensy com `USYNC`/`CANBR` e nós com seguidor em tempo real
> (feedforward + curva S). Grave **os dois** firmwares juntos.

---

## 1. Conexão serial no TD

| Item | Valor |
|---|---|
| Operador | **Serial DAT** |
| Porta | COM do dispositivo USB `1209:0001`, produto "Azul Mecanico USB" (o número da COM muda entre PCs: localize por VID/PID) |
| Baud rate | Qualquer (USB CDC ignora o baud) — use 115200 |
| Formato | Texto ASCII, **uma linha por comando**, terminada em `\n` (aceita `\r`, `\r\n`) |
| Respostas | Uma linha por evento, terminadas em `\r\n` |
| DTR | Ativo, se disponível (não obrigatório) |
| Após abrir a porta | Aguarde **500 ms** antes do primeiro comando |
| Tamanho máximo da linha | 127 caracteres (acima disso: `TEENSY_ERROR LINE_TOO_LONG`) |

Regras gerais:

- A letra do comando aceita maiúsculas ou minúsculas; espaços no início são ignorados.
- Números decimais usam **ponto** (`12.5`, nunca `12,5`).
- `node_id`: `1..10` para um nó; `0` = **broadcast** (todos os nós) nos comandos de atuação.
- `P` (ping) e `R` (status) exigem um nó específico (`1..10`).
- Para confirmar o canal após abrir, envie `P 1` e espere `PONG 1 0 0`. O banner `TEENSY_READY 1`
  só aparece se o host propagar DTR (no Windows não é garantido).

---

## 2. Eixos, unidades e faixas

| Eixo | Função | Unidade no `U`/`MS` | Faixa no comando | Limite padrão do nó |
|---|---|---|---|---|
| **C** | Base rotativa | graus | ±2621,43° (U) / ±3276,7° (MS) | **−360° … +360°** |
| **A** | Pivô dos lasers | graus | ±2621,43° (U) / ±3276,7° (MS) | **−360° … +360°** |
| **Z** | Linear | mm | ±655,34 mm (resolução 0,02 mm) | 0 … 480 mm (38400 passos) |

- Coordenadas são **absolutas** em relação ao zero (home) de cada eixo.
- Alvos fora do limite do nó são **saturados** no limite (não geram erro).
- Resolução mecânica padrão: C/A = 200 passos × 16 micropassos → **8,889 passos/°** (0,1125°/passo);
  Z = polia GT2 20 dentes → **80 passos/mm**.
- O campo Z do `U`/`MS` vai no CAN em unidades de 0,02 mm (int16, ±655,34 mm): cobre todo o curso
  de 480 mm. Valores com mais casas são arredondados para o múltiplo de 0,02 mm mais próximo
  (1 passo do Z = 0,0125 mm). Fora de ±655,34 mm: `TEENSY_ERROR INVALID_ARGUMENT`.
- `X` e `Y` são aceitos como aliases antigos de `C` e `A` nos comandos de eixo.

---

## 3. Resumo dos comandos

| Comando | Uso principal | Resposta de sucesso | Opcode CAN |
|---|---|---|---|
| `U` / `UF` | **Streaming de posição + lasers (uso principal do TD)** | *silencioso* | `0x25` (+`0x32`, `0x26`) |
| `MS` / `MSF` | Movimento coordenado único C+A+Z | `TEENSY_OK MS…`, `ACK`, `DONE` | `0x23` |
| `M` / `MF` | Mover um eixo para posição em passos | `ACK`, `DONE` | `0x20` / `0x22` |
| `H` | Home de um eixo | `ACK`, `DONE` | `0x21` |
| `STOP` / `ESTOP` / `X` | Parada imediata | `TEENSY_OK STOP`, `ACK` | `0x24` |
| `E` | Liga/desliga drivers | `ACK` | `0x10` |
| `S` | Nível de velocidade 1..5 (M/MS) | `ACK` | `0x11` |
| `L` | Laser individual | `ACK` | `0x30` |
| `F` | Ventoinha | `ACK` | `0x31` |
| `P` | Ping | `PONG` | `0x01` |
| `R` | Status e posição | `STATUS` + `POS` | `0x02` |
| `USYNC` | Commit sincronizado do `U` | `TEENSY_OK USYNC` | — |
| `CANBR` / `CANBR_LOCAL` | Bitrate do CAN | `TEENSY_OK CANBR` | `0x15` |
| `RAW` | Espelho de frames CAN crus (diagnóstico) | `TEENSY_OK CAN_RAW` | — |

`OTA_START`/`OTA_DATA`/`OTA_END`/`OTA_ABORT` existem para atualização de firmware pelo app Python e
não devem ser usados pelo TD.

---

## 4. Comandos em detalhe

### 4.1. `U` / `UF` — streaming de posição e lasers (recomendado)

```text
U  <node> <graus_c> <graus_a> <mm_z> <laser1> <laser2>
UF <node> <graus_c> <graus_a> <mm_z> <laser1> <laser2>
```

| Campo | Faixa | Observação |
|---|---|---|
| `node` | `0..10` | `0` = broadcast (mesmo alvo para todos) |
| `graus_c`, `graus_a` | ±2621,43 | resolução 0,01° |
| `mm_z` | `0 .. 480` (curso padrão) | resolução 0,02 mm; aceita até ±655,34, o nó satura no curso |
| `laser1`, `laser2` | `0..4095` | PWM 12 bits |

Exemplos:

```text
U 1 45.25 -12.50 150.00 2048 0
U 2 0 0 0 0 0
UF 3 370.0 10.0 20.0 4095 4095
```

Aliases aceitos: `TD` e `SYNC` (equivalentes a `U`).

**Respostas:** nenhuma em caso de sucesso. Em falha:

```text
TEENSY_ERROR INVALID_ARGUMENT UNIFIED     (faltam campos, laser > 4095, Z fora de ±655.34, etc.)
TEENSY_ERROR INVALID_NODE_ID 11
ERROR 1 25 08                             (nó: encoder indisponível — use UF)
ERROR 1 25 03                             (nó: sessão OTA ativa)
```

**Como o nó executa:**

- Não há fila: cada `U` só substitui o alvo de um seguidor em tempo real (segmentos de 5 ms).
- O nó estima a velocidade do alvo pela diferença entre frames (**feedforward**) e extrapola o alvo
  entre eles. Numa trajetória contínua o movimento acompanha o TD praticamente sem atraso.
- Aceleração e jerk do nó dão a curva S; a velocidade máxima é o `SPEED_MAX` do nó (720°/s padrão).
  O nível `S` **não** afeta o `U`.
- Alvos fora dos limites do nó são saturados no limite (a velocidade de feedforward é zerada ali).
- Se o TD parar de mandar frames, o alvo fica onde estava; 0,5 s depois o seguidor encerra
  (o próximo `U` o reinicia automaticamente).
- `UF` (malha aberta): ignora encoder e limites de C/A. As proteções do Z continuam ativas.
- Com o Z bloqueado (alarme de fim de curso), o Z é ignorado e C/A continuam.

**Lasers:** o frame de laser (`0x32`) só vai ao barramento quando o nível muda, ou a cada 250 ms
para se corrigir após um reboot do nó ou um `ESTOP`. É aplicado no instante em que chega ao nó.

**Commit sincronizado (`USYNC 1`, padrão):** o Teensy marca cada `U` como "aguardar". Quando a COM
fica ~0,3 ms sem bytes (fim da rajada do frame do TD), envia um commit em broadcast e **todos os nós
aplicam o alvo no mesmo instante**. Para isso funcionar, mande os `U` de todos os nós de um frame
juntos, em sequência (ver seção 7). Um commit perdido é aplicado pelo próprio nó após 10 ms.

### 4.2. `USYNC` — modo de aplicação do `U`

```text
USYNC        → TEENSY_OK USYNC 1        (consulta)
USYNC 1      → TEENSY_OK USYNC 1        (padrão: nós aplicam juntos no fim da rajada)
USYNC 0      → TEENSY_OK USYNC 0        (cada nó aplica o U ao recebê-lo)
```

O modo não é salvo: o Teensy volta a `USYNC 1` ao reiniciar. Use `USYNC 0` se o TD mandar os `U`
espaçados (um nó por vez, em ticks diferentes) ou se algum nó tiver firmware antigo.

### 4.3. `MS` / `MSF` — movimento coordenado único

```text
MS  <node> <graus_c> <graus_a> <mm_z> [laser1 laser2]
MSF <node> <graus_c> <graus_a> <mm_z> [laser1 laser2]
```

- Posição **absoluta**; C/A com resolução 0,1°, Z 0,02 mm (todo o curso de 480 mm).
- Os três eixos partem e chegam juntos (perfil S-curve com velocidade/aceleração do nó).
- Comandos `MS` consecutivos enfileirados são encadeados sem parar entre eles; um `MS` novo
  descarta `MS` ainda não iniciados na fila.
- Lasers opcionais: se informados, vão logo após o frame de movimento e são aplicados na hora em
  que chegam (no início do movimento, não ao fim).

```text
→ MS 1 90.0 -45.0 10.00
← TEENSY_OK MS 1 90.0 -45.0 10.00
← ACK 1 23
← DONE 1 23            (ao terminar o movimento)
```

Use `MS` para poses isoladas ("vá para este ponto e avise quando chegar"). Para animação contínua,
use `U`.

### 4.4. `M` / `MF` — um eixo, posição em passos

```text
M  <node> <eixo> <passos>      eixo = C | A | Z
MF <node> <eixo> <passos>      malha aberta em C/A
```

- `<passos>` é a **posição absoluta em passos** (não um deslocamento). C/A: passos × 0,1125° → graus;
  Z: passos / 80 → mm.

```text
→ M 1 C 800            (C vai para 90°)
← ACK 1 20
← DONE 1 20
```

### 4.5. `H` — home

```text
H <node> <eixo>        eixo = C | A | Z
```

- C/A: posiciona em malha fechada no zero gravado. Z: busca o fim de curso (3 testes) e sobe 5 mm.
- Um `H` descarta os movimentos pendentes daquele nó.

```text
→ H 2 Z
← ACK 2 21
← DONE 2 21            (Z: vários segundos)
```

### 4.6. `STOP` / `ESTOP` / `X` — parada

```text
STOP <node>            para os motores
ESTOP <node>           para os motores e apaga os lasers
X <node> [flags]       flags bit0 = apagar lasers
```

- `node 0` = todos os nós. O broadcast tem a maior prioridade do barramento.
- Descarta toda a fila e o alvo do `U`. É uma **parada brusca** (sem rampa): use em emergência.
  Em alta velocidade o eixo pode perder passos; refaça o home se necessário.
- Para pausar de forma suave, mande `U` com o alvo atual repetido.

```text
→ ESTOP 0
← TEENSY_OK STOP 0 1
← ACK 1 24 … ACK 10 24
← ERROR 1 20 0C         (se havia movimento M/MS em curso: interrompido)
```

Após um `ESTOP`, o próximo `U` volta a mover e reacende os lasers no nível pedido.

### 4.7. `E` — drivers

```text
E <node> 1      liga os drivers (torque)
E <node> 0      desliga (eixos livres)
← ACK <node> 10
```

### 4.8. `S` — nível de velocidade (M, MF, MS)

```text
S <node> <1..5>
← ACK <node> 11
```

| Nível | C/A (°/s) |
|---|---|
| 1 | 28 |
| 2 | 70 |
| 3 | 141 |
| 4 | 375 |
| 5 | até o `SPEED_MAX` (720 padrão) |

Não é salvo e **não afeta o `U`**.

### 4.9. `L` — laser individual

```text
L <node> <1|2> <0..4095>
← ACK <node> 30
```

Se usar `L` junto com `U`, o próximo `U` reenvia os níveis do `U`. Escolha um dos dois por cena.

### 4.10. `F` — ventoinha

```text
F <node> <0|1|2>       0 = desligada, 1 = ligada, 2 = automática (liga a 45 °C, desliga a 40 °C)
← ACK <node> 31
```

### 4.11. `P` — ping

```text
→ P 1 12 34
← PONG 1 12 34
```

`arg0` e `arg1` (`0..255`, opcionais) voltam iguais: servem para casar pergunta e resposta.

### 4.12. `R` — status e posição

```text
→ R 1
← STATUS 1 121 2048 0 1 2 3
← POS 1 45.2 -12.5 12000 31.5
```

`STATUS <node> <flags> <laser1> <laser2> <fan_on> <fan_mode> <nivel_velocidade>`

| Bit de `flags` | Valor | Significado |
|---|---|---|
| 0 | 1 | Drivers ligados |
| 1 | 2 | Z bloqueado (fim de curso acionado fora do home) |
| 2 | 4 | Alarme do Z ativo |
| 3 | 8 | Temperatura válida |
| 4 | 16 | TMC2209 (UART) pronto |
| 5 | 32 | CAN online |
| 6 | 64 | Formato de posição v2 (sempre presente) |

Exemplo: `121 = 64 + 32 + 16 + 8 + 1` → drivers ligados, TMC pronto, temperatura válida, CAN online.

`POS <node> <graus_c> <graus_a> <passos_z> <temp_c>`

- C/A em graus com 1 decimal, lidos do encoder **no momento do `R`** (valem durante o movimento);
  `nan` = encoder indisponível.
- Z em passos (÷ 80 = mm). Temperatura em °C; `-99.9` = sensor indisponível.

### 4.13. `CANBR` — bitrate do barramento

```text
CANBR                  → TEENSY_OK CANBR 1000000
CANBR 1000000          → broadcast para os nós trocarem e o Teensy troca junto
CANBR_LOCAL 500000     → troca só o Teensy
```

Valores: `125000`, `250000`, `500000`, `1000000`. O Teensy sempre inicia em 1 Mbps. Para migrar nós
que ainda estão em 500 kbps: `CANBR_LOCAL 500000` e depois `CANBR 1000000`. Não é algo do dia a dia
do TD: faça uma vez, na instalação.

### 4.14. `RAW` — diagnóstico

```text
RAW 1   → TEENSY_OK CAN_RAW 1     (cada frame CAN recebido também sai como CAN_RAW <id> <dlc> <bytes>)
RAW 0   → TEENSY_OK CAN_RAW 0
```

Só para depuração: aumenta muito o tráfego na COM.

---

## 5. Mensagens que chegam ao TD

| Mensagem | Origem | Quando |
|---|---|---|
| `HEARTBEAT <node>` | nó | A cada **1 s** por nó (10 nós = 10 linhas/s) |
| `ACK <node> <op>` | nó | Comando aceito (movimento: enfileirado, ainda não executado) |
| `DONE <node> <op>` | nó | Movimento ou home terminado |
| `ERROR <node> <op> <cod>` | nó | Falha (códigos abaixo) |
| `PONG`, `STATUS`, `POS` | nó | Respostas a `P` e `R` |
| `TEENSY_OK …` | Teensy | Confirmação local (`STOP`, `MS`, `USYNC`, `CANBR`, `RAW`) |
| `TEENSY_ERROR …` | Teensy | Erro local de sintaxe ou de envio no CAN |
| `TEENSY_READY 1` | Teensy | Ao abrir a porta (se o host propagar DTR) |

`<op>` e `<cod>` vêm em hexadecimal, sem `0x`.

### Códigos de `ERROR` do nó

| Código | Significado | O que fazer |
|---|---|---|
| `02` | Argumento inválido | Revise os valores do comando |
| `03` | Estado inválido (Z bloqueado ou OTA em curso) | `H <node> Z` ou aguarde o OTA |
| `04` | Tamanho de frame inválido | Firmware Teensy/nó incompatível |
| `06` | Comando não suportado pelo nó | Atualize o firmware do nó |
| `07` | Tempo esgotado (fila cheia) | Reduza a taxa de `M`/`MS`; use `U` |
| `08` | Encoder sem resposta | Verifique o cabo; `UF`/`MSF`/`MF` para malha aberta |
| `0C` | Interrompido por `STOP` (ou por um `H`) | Esperado após uma parada |

### `TEENSY_ERROR`

| Mensagem | Causa |
|---|---|
| `INVALID_ARGUMENT <campo>` | Sintaxe ou faixa inválida (`UNIFIED`, `MOVE_SYNC`, `AXIS`, `LASER`, `SPEED`, `CANBR`…) |
| `INVALID_NODE_ID <n>` | Nó fora de `0..10` (ou de `1..10` em `P`/`R`) |
| `UNKNOWN_COMMAND` | Comando não reconhecido |
| `LINE_TOO_LONG` | Linha com mais de 127 caracteres |
| `CAN_TX_BUSY <node>` | Barramento travado (sem transceiver, cabo ou terminação) |
| `CAN_TX_FAILED <node> <err>` | Frame sem ACK de nenhum nó no barramento |
| `CAN_SEND_FAILED <node> <err>` | Falha do controlador CAN |
| `CANBR_FAILED <err>` | Não foi possível trocar o bitrate |

---

## 6. Tempos de resposta

Valores **calculados ou simulados** a partir do firmware e do protocolo, a 1 Mbps. Ainda não foram
medidos no hardware.

### 6.1. Transporte

| Etapa | Tempo típico |
|---|---|
| TD → Teensy (USB, driver do Windows) | ~0,2–1 ms |
| Processamento de uma linha no Teensy | < 0,05 ms |
| Frame `U` no CAN (8 bytes) | 0,11–0,14 ms |
| Frame de laser (5 bytes) | 0,09–0,11 ms |
| Rajada de 10 nós (10 `U` + 10 lasers + commit) | ~2,5 ms no pior caso; ~1,5 ms com lasers fixos |
| Espera do commit (`USYNC 1`) | 0,3 ms após o último byte |
| Recepção no nó até o alvo atualizado | < 0,1 ms |

### 6.2. Movimento (`U`)

| Situação | Comportamento |
|---|---|
| Trajetória contínua (TD a 60–120 Hz) | Acompanha com erro de ~1° em trajetórias rápidas (senoide de ±90° a 0,5 Hz, pico de 283°/s); atraso efetivo próximo de zero |
| Mesma trajetória a 30 Hz | Erro de ~2,6° (extrapolação entre frames mais longos) |
| Início de reação a um salto de alvo | ~5–10 ms |
| Salto de 9° (alvo parado) | ~140 ms até parar no alvo |
| Salto de 90° | ~355 ms |
| Salto de 360° | ~740 ms |
| Diferença entre nós (`USYNC 1`) | Todos aplicam no mesmo frame de commit (< 0,1 ms) |
| Diferença entre nós (`USYNC 0`) | Até ~2,5 ms entre o primeiro e o último nó da rajada |

Os saltos usam os padrões do nó: C/A com 720°/s, 3600°/s² e jerk de 180.000°/s³. Saltos grandes
são limitados pela física (aceleração/jerk), não pela comunicação.

### 6.3. Outros comandos

| Comando | Tempo até a resposta |
|---|---|
| `P` → `PONG` | ~1–2 ms |
| `R` → `STATUS` + `POS` | ~2–3 ms (inclui a leitura dos dois encoders I²C) |
| `M`/`MS`/`H` → `ACK` | ~1–2 ms; o `DONE` chega ao fim do movimento |
| `E`/`S`/`L`/`F` → `ACK` | ~1–2 ms |
| `STOP`/`ESTOP` → motores parados | ≤ ~5 ms (mais até 0,5 ms se havia frames na fila do Teensy) |
| Laser (`U`/`L`) → PWM aplicado | ~1–2 ms |

---

## 7. Parametrização recomendada no TD

### 7.1. Taxa de envio

| Taxa do `U` | Uso | Carga do CAN (10 nós, lasers mudando todo frame) |
|---|---|---|
| 30 Hz | Mínimo aceitável | ~8% |
| **60 Hz** | **Recomendado** (taxa de cena do TD) | ~15% |
| 120 Hz | Movimentos muito rápidos | ~30% |
| > 200 Hz | Sem ganho perceptível | > 50% |

- Mantenha a taxa **constante** (envie a cada frame do TD, mesmo com o eixo parado). A taxa pode mudar
  durante o uso: o nó mede o intervalo sozinho.
- Mande sempre **posições**, nunca velocidades; o nó deriva a velocidade.
- Prefira trajetórias contínuas (Filter CHOP / Lag CHOP no TD). Saltos são suavizados pelo nó, mas
  chegam com o tempo físico da seção 6.2.

### 7.2. Ordem dentro de um frame

1. Monte as linhas `U` de todos os nós ativos.
2. Envie todas **de uma vez** (uma única chamada `send` com as linhas juntas, ou chamadas seguidas no
   mesmo callback).
3. Não intercale `R` dentro da rajada: faça o polling em outro tick.

### 7.3. Polling de status

- `R` por nó a no máximo **10 Hz** (10 nós = 100 frames de pedido + 200 de resposta por segundo).
- Para saber se o nó está vivo, use o `HEARTBEAT` (1 Hz): considere o nó offline após ~2,5 s sem ele.

### 7.4. Exemplo de script (Python no TD)

Exemplo de partida. Ajuste os nomes dos operadores ao seu projeto.

```python
# Execute DAT -> onFrameStart: envia a pose de todos os nós a cada frame
def onFrameStart(frame):
    serial = op('serial1')            # Serial DAT conectado à COM do Teensy
    pose = op('pose_nodes')           # Table DAT: node, c, a, z, l1, l2
    lines = []
    for r in range(1, pose.numRows):
        n = int(pose[r, 'node'])
        c = float(pose[r, 'c'])
        a = float(pose[r, 'a'])
        z = min(max(float(pose[r, 'z']), 0.0), 480.0)    # curso do Z
        l1 = int(pose[r, 'l1'])
        l2 = int(pose[r, 'l2'])
        lines.append(f"U {n} {c:.2f} {a:.2f} {z:.2f} {l1} {l2}")
    if lines:
        serial.send('\n'.join(lines), terminator='\n')   # rajada única -> um commit
    return
```

```python
# Callbacks do Serial DAT -> trata as respostas
def onReceive(dat, rowIndex, message, byteData):
    parts = message.split()
    if not parts:
        return
    kind = parts[0]
    if kind == 'POS':
        node, c, a, z_steps, temp = parts[1:6]
        op('status_nodes')[int(node), 'c'] = c
        op('status_nodes')[int(node), 'a'] = a
        op('status_nodes')[int(node), 'z_mm'] = int(z_steps) / 80.0
    elif kind == 'HEARTBEAT':
        op('status_nodes')[int(parts[1]), 'last_seen'] = absTime.seconds
    elif kind in ('ERROR', 'TEENSY_ERROR'):
        debug(message)
    return
```

### 7.5. Sequência de inicialização sugerida

```text
(abrir a COM, aguardar 500 ms)
P 1 … P 10            confirmar quais nós respondem (PONG)
E 0 1                 ligar os drivers de todos os nós
H 0 Z                 home do Z em todos (aguardar DONE <n> 21 de cada nó)
H 0 C  /  H 0 A       se necessário (aguardar DONE)
U … (a cada frame)    iniciar o streaming
```

E para encerrar:

```text
U … (pose de repouso, até chegar)
ESTOP 0               apaga os lasers e para tudo
E 0 0                 desliga os drivers (opcional)
```

---

## 8. Parâmetros dos nós que afetam o TD

Não são alterados pelo TD: ficam na NVS de cada nó e são ajustados pelo app Python (aba Parâmetros)
ou pelo console serial do nó.

| Parâmetro | Padrão | Efeito no `U` | Comando no console do nó |
|---|---|---|---|
| Limites C/A | −360° … +360° | Alvo saturado no limite | `LIMIT C -360 360` |
| `SPEED_MAX` C/A / Z | 720°/s / 300 mm/s | Velocidade máxima do seguidor | `SPEED_MAX C 720` |
| `ACCEL` C/A / Z | 3600°/s² / 2000 mm/s² | Aceleração do seguidor | `ACCEL C 3600` |
| Jerk do seguidor C/A / Z | 180.000°/s³ / 100.000 mm/s³ | Suavidade da curva S (maior = mais rápido e mais seco) | `MOTION TJERK C 180000` |
| `SPEED` (velocidade em uso) | 360°/s / 250 mm/s | Só `M`/`MS` (não afeta o `U`) | `SPEED C 360` |
| Bitrate CAN | 1 Mbps | — | via `CANBR` no Teensy |

Ajustes típicos:

- **Movimento "seco" demais ou vibração nas partidas:** reduza o jerk do seguidor (ex.: 90.000°/s³).
- **Não acompanha trajetórias rápidas:** aumente `ACCEL` (até o `ACCEL_MAX`, 7200°/s² padrão) e
  verifique se a velocidade pedida pelo TD não passa do `SPEED_MAX`.
- **Perda de passos:** reduza `ACCEL` e jerk; confira a corrente do TMC2209.

---

## 9. Problemas comuns

| Sintoma | Causa provável | Solução |
|---|---|---|
| Nenhuma resposta | COM errada ou aberta por outro programa | Feche o app Python; localize a COM por VID/PID `1209:0001` |
| `CAN_TX_BUSY` / `CAN_TX_FAILED` | Barramento sem nós, cabo ou terminação 120 Ω | Verifique CANH/CANL, GND comum e terminação nas pontas |
| Nó não responde, os outros sim | Nó em outro bitrate | `CANBR_LOCAL 500000`, `P <n>`; se responder, `CANBR 1000000` |
| `ERROR <n> 25 08` | Encoder do nó sem sinal | Cabo/ímã do AS5600; `UF` como contorno |
| Movimento "aos trancos" | Taxa do `U` irregular ou baixa | Envie a cada frame, ≥ 60 Hz |
| Nós fora de sincronia | `U` espaçados entre nós | Envie a rajada inteira de uma vez (seção 7.2) ou use `USYNC 0` |
| Laser não reacende após `ESTOP` | Usando só `L` | Reenvie `L`, ou use `U` (reenvia sozinho) |
