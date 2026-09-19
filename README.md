# ESP32 OTA Blink

Projeto de teste da funcionalidade **OTA (Over-The-Air)** do ESP32 usando o framework **ESP-IDF**.

O firmware conecta o dispositivo ao Wi-Fi, pisca um LED e verifica periodicamente em um servidor HTTP local se há uma nova versão do firmware disponível. Caso exista, ele baixa, grava e reinicia com a nova versão automaticamente.

## Funcionalidades

- Pisca um LED em um GPIO configurável (padrão: `GPIO 15`)
- Conecta ao Wi-Fi em modo STA (SSID/senha fixos no código)
- Consulta um `manifest.json` em um servidor HTTP para verificar a versão mais recente
- Compara a versão atual com a versão do servidor (`strcmp` no `version` do manifest)
- Baixa o novo firmware via `esp_https_ota` e grava na próxima partição OTA
- Exibe informações do estado da partição, versão e data de compilação
- Confirma o boot da nova versão (`PENDING_VERIFY -> VALID`) ou executa rollback automático
- Layout de partições com `factory` + `ota_0` + `ota_1` (dois slots de OTA)

## Estrutura do Projeto

| Arquivo | Descrição |
| ------- | --------- |
| `main/blink_example_main.c` | Código-fonte principal (Wi-Fi, OTA e blink) |
| `main/CMakeLists.txt` | Registro do componente e dependências |
| `partitions_two_ota.csv` | Tabela de partições com 2 slots de OTA |
| `otadata/manifest.json` | Manifest JSON servido pelo servidor HTTP |
| `otadata/blink_ota.bin` | Firmware compilado pronto para ser servido |
| `sdkconfig.defaults` | Configurações padrão de build |
| `version.txt` | Versão do firmware |

## Arquitetura OTA

```
                    ┌──────────────────────┐
  ESP32 ──Wi-Fi─── ► │ Servidor HTTP local  │
                    │  (ex: 192.168.15.50) │
                    └──────────────────────┘
                             │
                             ├── GET /manifest.json  → versão mais recente + URL
                             └── GET /blink_ota.bin  → binário do firmware
```

Fluxo de atualização:

1. Inicializa NVS e configura Wi-Fi (STA)
2. Pisca o LED enquanto aguarda conexão
3. Lê `manifest.json` e compara a versão com a versão em execução
4. Se diferente, baixa o firmware apontado pela URL do manifest
5. Grava via `esp_https_ota` na próxima partição (`ota_0` ou `ota_1`)
6. Reinicia; se o novo firmware iniciar com `PENDING_VERIFY` e passar nos testes, marca como `VALID`; se falhar, faz rollback

## Configuração

### Pré-requisitos

- ESP-IDF (versão que suporte `esp_https_ota`)
- Um ESP32 com Wi-Fi (testado em placas DevKit)
- Servidor HTTP simples na rede local para servir o firmware (ex: `python3 -m http.server 8070` na pasta `otadata/`)

### Ajustes no código

No arquivo `main/blink_example_main.c`:

```c
#define MANIFEST_URL   "http://192.168.15.50:8070/manifest.json"
```

- Altere `MANIFEST_URL` para o IP do servidor na rede local
- A versão do firmware é definida em `version.txt` no build

### Servidor HTTP

Sirva a pasta `otadata/`:

```bash
cd otadata
python3 -m http.server 8070
```

O `manifest.json` segue o formato:

```json
{
    "version": "2.0.0",
    "url": "http://192.168.15.50:8070/blink_ota.bin",
    "changelog": "Initial version"
}
```

### Targets suportados

ESP32, ESP32-S2, ESP32-C3, ESP32-C6, ESP32-H2, entre outros (configurável via `idf.py set-target`).

## Build e Flash

```bash
idf.py set-target esp32      # ou esp32s3, esp32c3, etc.
idf.py build
idf.py -p PORT flash monitor
```

Para gerar o binário de OTA a ser servido:

```bash
idf.py build
# o binário fica em build/blink.bin → renomeado para otadata/blink_ota.bin
cp build/blink.bin otadata/blink_ota.bin
```

## Como Testar

1. Suba o servidor HTTP com `otadata/blink_ota.bin` (primeira versão)
2. Grave o firmware via USB (`idf.py flash`) com a mesma versão inicial
3. Modifique a versão no `manifest.json` (ex: `2.0.0` -> `3.0.0`) e compile um firmware novo
4. Copie o novo `build/blink.bin` para `otadata/blink_ota.bin`
5. Reinicie o ESP32: ele detectará a nova versão, baixará e reiniciará com o firmware atualizado

## Saída Esperada (monitor serial)

```text
I (1234) blink_ota: ===============================================
I (1235) blink_ota: Versão do projeto: 2.0.0
I (1236) blink_ota: Data de compilação: Sep 18 2026 17:22:33
I (1237) blink_ota: Partition Versão atual: 2.0.0
I (1238) blink_ota: Partição atual: ota_0 (endereço: 0x10000)
I (1239) blink_ota: Estado: VALID
I (1240) blink_ota: Próxima partição OTA: ota_1
I (1250) blink_ota: Wi-Fi inicializado.
I (2050) blink_ota: IP_EVENT_STA_GOT_IP
I (2051) blink_ota: Obteve IP: 192.168.15.x
I (4052) blink_ota: Verificando atualizações em: http://192.168.15.50:8070/manifest.json
I (4060) blink_ota: Versão atual: 2.0.0
I (4061) blink_ota: Versão no servidor: 3.0.0
I (4062) blink_ota: ✓ Nova versão disponível!
I (4063) blink_ota: Conectado ao Wi-Fi. Iniciando download de: http://192.168.15.50:8070/blink_ota.bin
I (7050) blink_ota: OTA Sucessivo! Reiniciando em 3 segundos...
```

## Rollback

O firmware implementa o fluxo de rollback do ESP-IDF:

- Após o OTA, o novo app inicia com estado `ESP_OTA_IMG_PENDING_VERIFY`
- Em `check_and_confirm_ota()`, se os testes passarem, chama `esp_ota_mark_app_valid_cancel_rollback()`
- Se falhar, chama `esp_ota_mark_app_invalid_rollback_and_reboot()` para voltar à versão anterior

## Observações

- As credenciais de Wi-Fi e URLs estão **hardcoded** no código; para produção, use `Kconfig` ou NVS
- O teste OTA é feito via **HTTP** (sem TLS); para produção, use HTTPS (`cert_pem`)
- A comparação de versão é feita por string (`strcmp`), não semântica (`major.minor.patch`)
