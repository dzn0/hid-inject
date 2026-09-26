# hid-inject: teclado e mouse na fila compartilhada

Esta variante parte do fork de mouse já testado. Os arquivos `hid-inject-shared.cpp` e `hid-inject-shared.sys` permanecem como versão anterior para comparação.

## Mudanças

- O teclado usa a mesma seção e o mesmo evento do mouse. A fila mantém o layout V1 e os nomes `Global\HidInjectMouseQueueV1` e `Global\HidInjectMouseWakeV1`, então o emissor existente continua compatível.
- O comando de teclado é `MODE=20`, `X=scan code`, `Y=0` para pressionar e soltar, `1` para pressionar ou `2` para soltar. `MODE=-1` encerra o worker.
- O worker não abre mais `kbd_cmd.txt` nem acorda a cada 10 ms para procurá-lo. Quando a fila está vazia, espera pelo evento sem timeout.
- Os cliques de mouse nos modos 2 e 3 enviam `DOWN` e `UP` sem a antiga pausa de 80 ms. **Isto é experimental**: o teste de movimento anterior não verificou o registro de cliques sem pausa.
- O toque de teclado em `Y=0` preserva o intervalo de 10 ms entre pressionar e soltar. Esse intervalo não é polling de IPC.

## Teste

Depois de reiniciar e carregar `hid-inject-shared-input.sys`, abra PowerShell como administrador. Para um toque de W:

```powershell
& 'D:\Opencode\projects\hid-inject\combined\x64\hid-inject-shared-send.exe' 20 17 0
```

Para aguardar 3 segundos e repetir `wdsawdsawdsa` durante 5 segundos:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'D:\Opencode\projects\hid-inject\combined\x64\test-wdsawdsawdsa.ps1'
```

O script envia um comando a cada 50 ms e imprime o número de comandos enviados. Selecione a janela alvo durante os 3 segundos iniciais. No DebugView, procure `[hid][shm] key #`.

Para verificar os cliques sem pausa, teste separadamente os modos 2 (esquerdo) e 3 (direito) em uma janela que indique claramente o clique. Os modos 4/5 e 6/7 continuam disponíveis para `DOWN`/`UP` explícitos.

## Build

`hid-inject-shared-input.sys` foi compilado com EWDK 28000 em x64, `/kernel /GS-`, entrada `DriverMain`, link com `ntoskrnl.lib` e `hal.lib`, sem avisos. O emissor Win32 também compilou. Esta variante **não foi carregada nem testada em execução**. Não use `kdmapper --free`: o worker continua executando após `DriverMain` retornar.
