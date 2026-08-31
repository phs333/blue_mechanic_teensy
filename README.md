# Blue Mechanic — Teensy 4.1 CAN/USB bridge

Firmware Zephyr para transformar comandos ASCII recebidos por USB CDC ACM
(porta COM no Windows) em frames CAN e devolver status/eventos dos nodes.

```text
TouchDesigner/host <-> USB CDC ACM <-> Teensy 4.1 <-> FlexCAN1 <-> nodes ESP32
```

O termo UART, neste projeto, refere-se à API UART usada pelo CDC ACM. O UART
físico do Teensy fica reservado para logs de diagnóstico.

## Hardware

| Função | Teensy 4.1 |
|---|---|
| FlexCAN1 TX / CTX1 | pino 22 |
| FlexCAN1 RX / CRX1 | pino 23 |
| Console TX1/RX1, 115200 8N1 | pinos 1/0 |
| WS2812, 12 LEDs | pino 14 |
| USB CDC ACM | conector USB nativo |

O i.MX RT1062 possui o controlador FlexCAN, mas o Teensy 4.1 **não possui
transceiver CAN na placa**. Ligue os pinos 22/23 a um transceiver com I/O de
3,3 V, una os GNDs e termine o barramento com 120 ohms em cada extremidade.

## Build e gravação

Requisitos: PlatformIO Core 6.1 e a plataforma Teensy instalada.

```powershell
pio run --environment teensy41
pio run --environment teensy41 --target upload
```

O upload usa o Teensy Loader CLI com o modelo `TEENSY41`. Pressione uma vez o
botão Program quando solicitado. O script repete automaticamente a transferência
se o HalfKay recusar o primeiro bloco logo depois de entrar no bootloader.
Depois que o firmware iniciar, a interface CDC passa a aparecer como COM.

Para localizar a COM correta e abrir o monitor sem número fixo:

```powershell
.\tools\monitor_bridge.ps1
```

Consulte [detecção COM no Windows](docs/windows_com.md),
[protocolo USB/TouchDesigner](docs/teensy_usb_touchdesigner.md) e
[protocolo CAN](docs/can_protocol.md).

## Versão do Zephyr

A plataforma PlatformIO Teensy 5.2.0 usada aqui fornece Zephyr 2.7.1 e GCC
8.2.1. O firmware permanece compatível com essa combinação e o build é
reprodutível, mas Zephyr 2.7 já chegou ao fim de suporte. Para produto novo ou
conectado a ambientes não confiáveis, planeje uma migração separada para
Zephyr 3.7 LTS ou para uma release estável atual usando `west`; as APIs CAN,
USB e devicetree mudaram bastante e essa migração deve ser validada em
hardware.

## Identidade USB de desenvolvimento

O firmware usa `1209:0001` e um serial hexadecimal único derivado do hardware
do i.MX RT1062. Esse PID é público apenas para protótipos/testes privados e
não pode ser usado em unidades distribuídas ou vendidas. As instruções de troca estão em
[docs/windows_com.md](docs/windows_com.md).
