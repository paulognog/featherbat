# FeatherBat

Monitor ultraleve de bateria para dispositivos sem fio na barra de tarefas do Windows 11, escrito em **C puro sem CRT (`-nostdlib`)** e orientado a eventos.

## Destaques técnicos

- **Ultraleve (~80 KB, sem CRT e sem WinRT):** executável único sem dependências externas (`kernel32`, `user32`, `gdi32`, `advapi32`, `setupapi`, `cfgmgr32`, `hid`, `shell32`, `comctl32`). `BluetoothApis.dll` e `xinput1_4.dll` só são carregadas se houver dispositivo compatível conectado.
- **Zero timer de 1 s no widget:** o posicionamento na barra de tarefas usa `SetWinEventHook` (`EVENT_OBJECT_LOCATIONCHANGE` e `EVENT_OBJECT_REORDER` filtrados na thread da taskbar) + `TaskbarCreated`, `WM_SETTINGCHANGE` e `WM_DISPLAYCHANGE`.
- **Orientado a eventos onde o hardware permite:**
  - **Logitech HID++:** leitura assíncrona pendente (`OVERLAPPED`) para eventos `0x41` (acordar/dormir) e broadcasts de bateria (`0x1004`/`0x1000`/`0x1001`/`0x1F20`), além de curvas XML do G HUB (quando instalado) e suporte ao **Logitech G435** (`046D:0ADF`).
  - **Bluetooth LE (GATT):** `BluetoothGATTRegisterEvent` na característica `0x2A19` (push em tempo real, sem polling).
  - **PnP / energia:** `CM_Register_Notification` (HID, BLE, GATT, BthPort, Audio Render) + `WM_POWERBROADCAST` para detectar conexão/desconexão e retorno de suspensão na hora.
  - **Pull lento apenas onde não há push:**
    - **HyperX** (`Cloud III`, `Cloud III S`, `Cloud II`, `Cloud II Core`, `Cloud Alpha`, `Cloud Alpha 2`, `Cloud Flight`, `Cloud Stinger Core`) a cada 60 s quando conectado.
    - **Attack Shark / Delux / chineses:** receptores **Rongyuan** (81 modelos), **Xenta `1D57`** (assíncrono para mostrar `⚡` em ~2 s ao carregar) e família **Kysona / ATK / VXE / Pulsar / Hitscan / Redragon**.
    - **DualSense / DualSense Edge / DualShock 4:** lê 1 relatório a cada 60 s apenas quando conectado (evita acordar 250×/s).
    - **Xbox (XInput):** 60 s apenas quando conectado.
    - **Bluetooth clássico** via `CfgMgr32`/`SetupAPI` (5 min), **HID genérico** (`0x06:0x20` / `0x85:0x65`) e ponte opcional para **HeadsetControl** (`headsetcontrol.exe` no mesmo diretório ou no `PATH`).
- **Interface nativa:** ícones `Segoe Fluent Icons`, tema claro/escuro, DPI dinâmico, realce no hover, tooltip com nome/status completo, menu de clique direito (**Atualizar agora**, **Iniciar com o Windows** e **Fechar**) e alerta de bateria baixa (`≤ 15%`, rearmado acima de `20%` ou ao carregar).

## Instalação e uso

1. Baixe `featherbat.exe` na página de [Releases](../../releases/latest) — é um executável único, então você também pode enviar apenas esse `.exe` para amigos.
2. Dê dois cliques em `featherbat.exe`.
   - Ele se copia automaticamente para `%LOCALAPPDATA%\Programs\featherbat\featherbat.exe`, configura a inicialização com o Windows e passa a rodar de lá — você pode até apagar o arquivo baixado em *Downloads* depois!
   - Para atualizar no futuro, basta baixar a nova versão e dar dois cliques nela.
   - Para ligar ou desligar a inicialização automática a qualquer momento, clique com o botão direito no widget → **Iniciar com o Windows**.

> **Aviso do Windows SmartScreen?** Como o executável não possui assinatura digital paga, na primeira abertura o Windows pode exibir *"O Windows protegeu o computador"*. Clique em **Mais informações → Executar assim mesmo**, ou compile do código-fonte abaixo.

### Linha de comando (opcional)

- `featherbat.exe --no-autostart` — executa sem instalar nem registrar no início do Windows.
- `featherbat.exe --once` — lista os dispositivos detectados no terminal e sai.
- `featherbat.exe --dump` — exibe coleções HID, nós Bluetooth e frames Xenta para adicionar novos dispositivos.
- `featherbat.exe --quit` — encerra a instância em execução (também disponível via clique direito no widget → **Fechar**).
- `featherbat.exe --uninstall` — encerra, remove do início do Windows, limpa o registro e apaga a pasta instalada.

## Compilação (MSYS2 UCRT64)

```bat
build.bat
```

Ou com `gcc` (testes unitários):

```sh
gcc -std=c11 -Os -nostdlib -ffreestanding -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -ffunction-sections -mno-stack-arg-probe -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Wall -Wextra -mconsole -s -Wl,--gc-sections -Wl,-e,mainCRTStartup tests/unit_test.c build/mini.o build/hid.o build/drv_logi.o build/drv_ps.o build/drv_kysona.o -o build/unit_test.exe -lkernel32 -luser32 -lsetupapi -lcfgmgr32 -lhid
./build/unit_test.exe
```

## Licença

[GPL-3.0](LICENSE) — lógica de protocolos de dispositivos baseada em [Wireless-Device-Battery-Monitor](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor) e [windows-taskbar-battery](https://github.com/renanleonel/windows-taskbar-battery).
