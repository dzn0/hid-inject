# hid-inject: variante de memória compartilhada

Esta é uma tentativa isolada. `hid-inject.cpp` e `combined.vcxproj` continuam apontando para a versão de arquivo. A variante está em `combined/src/hid-inject-shared.cpp`; o emissor de teste está em `combined/tools/hid-inject-shared-send.cpp`.

## Protocolo

- O worker em kernel cria a seção `Global\HidInjectMouseQueueV1` e o evento `Global\HidInjectMouseWakeV1`.
- O handle da seção permanece aberto durante a vida do worker para que o emissor consiga encontrá-la pelo nome.
- O emissor abre os dois objetos depois que o driver indica `ready`. O acesso é reservado a LocalSystem e processos elevados do grupo Administrators.
- A fila suporta 256 comandos de 16 bytes e **um único produtor**. `Mode`, `X` e `Y` preservam os valores de `ProcessMou`; `Mode = -1` encerra o worker.
- O produtor escreve o comando, publica `WriteIndex` com uma operação `Interlocked` e chama `SetEvent`. O worker copia o comando da seção antes de `InjectMouse`.
- O teclado ainda usa `kbd_cmd.txt` e sua espera de até 10 ms. Cliques completos ainda têm a pausa de 80 ms do código original.

## Logs no DebugView

Ative **Capture Kernel**. A variante usa `DbgPrintEx` com prefixo `[hid][shm]` para registrar a criação da fila/evento, os primeiros oito comandos consumidos, uma amostra a cada 1024 comandos e os totais ao encerrar. Falhas de criação, índices corrompidos e erros ao esperar o evento também são registrados. Movimento não é impresso a cada comando para evitar alterar a latência medida.

## Teste do emissor

Compile o emissor como aplicativo Win32 x64. Em um terminal elevado, após iniciar a variante do driver:

```powershell
hid-inject-shared-send.exe 0 50 -30
hid-inject-shared-send.exe 2 0 0
hid-inject-shared-send.exe --stdin
```

No modo `--stdin`, envie uma linha `MODE X Y` por comando. Mantenha esse processo aberto para testar taxas altas: iniciar um novo `.exe` para cada movimento acrescenta um custo maior que a comunicação em memória compartilhada.

## Build e estado

O código foi compilado com EWDK 28000 em x64, `/kernel /GS-`, entrada `DriverMain`, e link com `ntoskrnl.lib` e `hal.lib`. O emissor Win32 também compilou. O `.sys` **não foi carregado nem testado em execução**. O caminho existente de injeção em `mouclass` foi preservado; uma falha nele ainda pode causar BSOD independentemente do IPC.

Não use `kdmapper --free` com essa variante: o worker continua executando após o retorno de `DriverMain`.
