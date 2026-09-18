# Diretrizes do Projeto Blue Mechanic - Teensy 4.1 CAN Bridge

## Execução Autônoma e Auto-Aprovação de Comandos
- **Auto-Aprovação de Comandos**: Execute comandos de terminal, compilações (`pio run`), uploads, scripts de verificação e testes de forma direta e autônoma, sem solicitar confirmação ou aprovação prévia para comandos comuns.
- **Modo Direto**: Não entre em Planning Mode nem crie planos intermediários bloqueantes para tarefas de código, correções de bugs, refatorações ou diagnósticos. Aplique as modificações diretamente no código.
- **Sem Interrupções Desnecessárias**: Só interrompa para solicitar confirmação caso haja risco real de perda irreversível de dados ou conflito arquitetural crítico sem resolução óbvia.
- **Validação Imediata**: Ao modificar o firmware ou scripts, valide imediatamente com o build do PlatformIO (`pio run -e teensy41`) ou comandos pertinentes antes de finalizar a resposta.

## Nomenclatura e Terminologia
- **Termo "Nodes" / "Nós"**: Refira-se aos dispositivos ESP32-S3 conectados via barramento CAN exclusivamente como **"nodes"** ou **"nós"** (ex.: "Node 1..10", "Status dos Nós").
- **Nunca use o termo "frota" ou "fleet"**.

## Ambiente e Build
- **Plataforma**: PlatformIO com framework Zephyr para Teensy 4.1 (`board = teensy41_bridge`).
- **Compilação**: `pio run -e teensy41`.
- **Scripts de Suporte**: Localizados no diretório `tools/`.
