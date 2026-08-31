# Detecção automática da porta COM no Windows

## O que o overlay faz

O `zephyr/app.overlay` instancia uma interface USB CDC ACM em
`&zephyr_udc0`. Isso faz o Windows carregar o driver nativo `usbser.sys` e
criar uma porta `COMx`. O número é sempre atribuído pelo Windows; um overlay
do firmware não consegue escolher nem descobrir esse número.

O bridge usa uma identidade USB estável:

| Campo | Valor de desenvolvimento |
|---|---|
| VID:PID | `1209:0001` |
| Produto | `Blue Mechanic CAN USB Bridge` |
| Serial | hexadecimal único do i.MX RT1062, por exemplo `5001008B403151D700` |

`1209:0001` é o PID público de teste do projeto pid.codes. Ele é adequado
somente para protótipo/teste privado. Antes de fabricar, vender ou distribuir
o equipamento, obtenha um PID próprio e altere os três pontos que usam o ID:

- `zephyr/prj.conf`;
- `boards/teensy41_bridge.json`;
- `tools/find_bridge_port.py`.

## PlatformIO

O manifesto local `boards/teensy41_bridge.json` informa o VID/PID ao
PlatformIO. Por isso `platformio.ini` não fixa `COM3`, `COM4`, etc. Com o
firmware gravado e enumerado, o monitor seleciona o bridge pelo hardware ID:

```powershell
pio device monitor --project-dir . --environment teensy41
```

O atalho abaixo seleciona pelo VID/PID e aguarda até 15 segundos:

```powershell
.\tools\monitor_bridge.ps1
```

Para obter apenas o nome da porta:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" `
  .\tools\find_bridge_port.py --wait 15
```

A saída é uma única linha, por exemplo `COM7`. O helper aceita `--json` para
diagnóstico e `--serial ID` para escolher uma unidade específica. Se houver
mais de um bridge e nenhum serial for informado, ele informa a ambiguidade em
vez de escolher uma porta arbitrariamente. Não use `monitor_port = COM*`: em
máquinas com vários adaptadores seriais esse padrão pode selecionar o
dispositivo errado.

## TouchDesigner

O TouchDesigner possui o Serial Devices DAT para acompanhar adição e remoção
de portas. Quando a identificação precisa ser inequívoca, um Text DAT pode
chamar o helper e atribuir o resultado ao parâmetro Port do Serial DAT:

```python
import os
import subprocess

python = os.path.expandvars(
    r"%USERPROFILE%\.platformio\penv\Scripts\python.exe"
)
finder = os.path.join(project.folder, "tools", "find_bridge_port.py")
com_port = subprocess.check_output(
    [python, finder, "--wait", "5"], text=True
).strip()

serial_dat = op("serial1")
serial_dat.par.active = False
serial_dat.par.port = com_port
serial_dat.par.active = True
```

Adapte `finder` se o arquivo `.toe` não estiver na raiz deste projeto. Use o
modo de recepção **One Per Line** e envie cada comando terminado por `\n`.
O PlatformIO mantém DTR ativo por `monitor_dtr = 1`. O firmware não depende de
DTR para responder comandos, pois algumas combinações Windows/Zephyr 2.7 não
propagam essa linha de controle. Aguarde cerca de 500 ms depois de abrir a
porta antes do primeiro envio.

## Diagnóstico rápido

1. Confirme que o cabo USB transmite dados, não apenas alimentação.
2. No Gerenciador de Dispositivos, procure `Blue Mechanic CAN USB Bridge` em
   **Portas (COM e LPT)**.
3. Execute `pio device list` e confira `VID:PID=1209:0001` e o serial.
4. Se o Teensy estiver no bootloader, ele aparece como o dispositivo de carga
   da PJRC, não como a COM do firmware; grave/inicie o firmware primeiro.
5. Se não houver COM, confira no UART físico (pinos 0/1, 115200 8N1) os logs de
   inicialização do Zephyr. Os logs foram separados da COM do protocolo.
6. Quando o host propaga DTR, o firmware envia `TEENSY_READY 1`; não dependa
   desse banner no Windows. Para um teste que independe do CAN, envie `R 99`
   após 500 ms: a resposta deve ser
   `TEENSY_ERROR INVALID_NODE_ID 99`.
7. Sem transceiver ou outro equipamento capaz de confirmar o frame, `R 1` pode
   produzir `TEENSY_ERROR CAN_TX_FAILED 1 -2` ou deixar um envio pendente. Nesse
   segundo caso, outro comando CAN retorna `TEENSY_ERROR CAN_TX_BUSY 1`, mas a
   COM continua aceitando comandos locais como `R 99`.
