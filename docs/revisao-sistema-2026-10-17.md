# Revisão do Sports Replay 1.3.1 — 17/10/2026

**Atualização:** este documento preserva o diagnóstico anterior às correções. Os dez pontos foram tratados na versão 1.3.2; consulte a [validação das correções](validacao-correcoes-1.3.2.md).

Foram encontrados **10 pontos acionáveis**: 2 de prioridade alta, 7 de prioridade média e 1 de prioridade baixa. Nove dizem respeito ao comportamento do sistema; um é uma lacuna na execução automática dos testes. Os resultados abaixo distinguem reprodução em teste de constatação por leitura do código.

O código de produção não foi corrigido nesta revisão. Foram adicionados este relatório e um programa de diagnóstico isolado em `audit/harness`. A compilação existente foi atualizada para testar os fontes atuais; a instalação de produção do OBS não foi substituída.

## Escopo e verificações

Revisados: captura, buffers, encoder/decoder, playback e hotkeys, intro/outro, gravação MP4/AAC, carregamento, thumbnails, dock, rastreamento de cenas, configuração, API Vendor, journal/recuperação, scripts de build/pacote, instalador e configuração de CI. A cópia em `fork-publicacao` tem os mesmos fontes, testes e scripts da raiz após normalizar as quebras de linha.

Verificações executadas no Windows x64:

| Verificação | Resultado |
| --- | --- |
| Compilação dos fontes e testes em `build_integration`, RelWithDebInfo | Aprovada |
| Teste de mídia | 95 verificações PASS; saída 0 |
| Host isolado com o plugin DLL real e libobs | 60 verificações PASS; saída 0 |
| Recuperação em um segundo processo | 18 verificações PASS; saída 0 |
| Imports/ABI da DLL recompilada e da DLL do staging da release | Aprovados contra o OBS instalado |
| Quatro hashes de `release/SHA256SUMS-1.3.1.txt` | Todos conferem |
| DLL no ZIP 1.3.1 versus DLL de staging | Idênticas |
| Chaves de tradução en-US, es-ES e pt-BR | 61 chaves em cada idioma, sem chaves ausentes em relação ao inglês |
| Descoberta de testes pelo CTest | **Zero testes registrados** |
| Diagnóstico adicional de playback/buffer | Reproduziu os itens 2, 3, 4, 6 e 8 abaixo |

Os 173 PASS são verificações internas dos três programas executados, não 173 cenários independentes. Os testes existentes não cobrem toda a interface e os cenários de falha abaixo.

Evidências desta execução: `audit/20261017-172659/{media.log,host.log,recover.log,probes.log,ctest.log,shipped-binary.json}`. Programa adicional: `audit/harness/probe.c`.

## Achados

### 1. Alta — consulta de saúde pode acessar um filtro ainda em criação

**Local:** `src/capture-filter.c:143`, `src/sr-integration.cpp:498`, `src/capture-filter.c:341`.

O filtro entra no registro de capturas dentro de seu callback `create`, antes de esse callback retornar. O libobs só atribui `source->context.data` depois do retorno de `create` — confirmado na dependência local em `.deps/obs-studio-32.2.2/libobs/obs-source.c:492`.

Uma chamada concorrente a `GetCaptureSource` para um UUID persistente já conhecido pode encontrar o filtro nessa janela e passar `obs_obj_get_data(source) == NULL` diretamente a `sr_capture_health`. Essa função desreferencia `c` ao acessar `state_mutex`, podendo encerrar o processo do OBS. Diferentemente de `capture_list`, essa rota não verifica `data`.

**Evidência:** sequência de inicialização e caminho de ponteiro nulo identificados por leitura dos fontes do plugin e do libobs. Não foi reproduzida uma queda concorrente nesta execução.

**Correção sugerida:** verificar a disponibilidade de `context.data` em todas as rotas e retornar um estado de inicialização; impedir que o registro exponha instâncias incompletas como prontas.

### 2. Alta — reprodução de arquivo deixa uma flag que pode bloquear a próxima captura automática

**Local:** `src/playback-source.c:319`, `src/playback-source.c:927`, `src/playback-source.c:963`.

`sr_playback_play_file` sempre define `skip_next_autocapture = true`. Quando o arquivo é iniciado em uma fonte já ativa, não há uma nova ativação imediata para consumir a flag. Ao sair da cena, `deactivate` para a reprodução, mas mantém a flag. Na próxima entrada, `activate` consome a flag e retorna sem capturar e sem reiniciar o replay parado.

**Impacto:** o operador volta à cena esperando um replay atualizado e pode colocar no ar uma imagem parada. O mecanismo de retorno por captura vazia também é ignorado nesse caminho.

**Evidência reproduzida:** após carregar arquivo e executar desativação/ativação, o diagnóstico registrou `skip_autocapture_after_deactivate=1`, seguido de `next_activation_playing=0` e `empty_bounce_countdown=0.0`.

**Correção sugerida:** associar a flag à ativação pendente do lançamento e limpá-la ao desativar ou cancelar esse lançamento.

### 3. Média — blocos de áudio recebem timestamps sobrepostos durante playback

**Local:** `src/playback-source.c:489`, especialmente a linha 507.

O loop pode emitir vários blocos de áudio em um único tick de vídeo. Cada bloco recebe `os_gettime_ns()`, sem preservar a distância temporal dos blocos originais. Dois blocos de aproximadamente 21,3 ms podem ser enviados com apenas nanossegundos de diferença.

**Impacto:** o áudio entregue ao OBS tem intervalos sobrepostos, com risco de cortes, correções de sincronismo e descontinuidade. Os testes existentes verificam áudio gravado/carregado, mas não os timestamps emitidos na reprodução.

**Evidência reproduzida:** com ticks de 30 fps e pausas de 33 ms entre chamadas, 17 blocos emitidos apresentaram 6 intervalos menores que metade da duração do bloco anterior; o menor intervalo foi 100 ns. A instrumentação observou as chamadas de saída; não foi feita avaliação auditiva no mixer real.

**Correção sugerida:** manter uma origem temporal da reprodução e calcular o timestamp de saída a partir da posição de cada amostra/bloco.

### 4. Média — snapshot curto copia todo o áudio do buffer

**Local:** `src/sr-buffer.c:205`, `src/sr-integration.cpp:298`.

O recorte por duração/cutoff é aplicado aos pacotes de vídeo, mas o snapshot duplica todos os blocos de áudio. Isso também acontece quando `save_audio=false`, pois a opção não chega à função de snapshot. A admissão ainda estima memória usando o buffer inteiro, antes de recortar.

**Impacto:** consumo desnecessário de RAM e maior tempo segurando os locks da captura. Pedidos curtos ou sem áudio podem atingir `MEMORY_LIMIT` desnecessariamente. Somente o áudio estéreo float de um buffer de 120 s a 48 kHz ocupa cerca de 43,9 MiB por cópia.

**Evidência reproduzida:** um pedido de 1 s, alinhado ao GOP para 1466,7 ms de vídeo, manteve os mesmos 118 blocos de áudio do arquivo original de aproximadamente 2,5 s.

**Correção sugerida:** copiar apenas o áudio que intersecta o intervalo efetivo do vídeo e permitir snapshot sem áudio; estimar/reservar a memória do recorte solicitado.

### 5. Média — falha permanente de um encoder já aberto não dispara recuperação

**Local:** `src/sr-codec.c:257`, `src/capture-filter.c:232`, `src/capture-filter.c:283`.

`sr_encoder_encode` devolve `NULL` tanto para indisponibilidade temporária de pacote quanto para erros definitivos. O filtro simplesmente continua quando recebe `NULL`. O retry de dez segundos é ativado somente quando a criação do encoder falha; ele não cobre um encoder que abriu e depois deixou de funcionar.

**Impacto:** uma falha persistente de hardware/driver pode deixar a câmera em `ERROR` indefinidamente, ainda recebendo frames, até uma mudança de configuração, resolução ou relógio provocar recriação.

**Evidência:** fluxo de erros confirmado por leitura; não foi induzido reset de GPU nem falha real de encoder nesta revisão.

**Correção sugerida:** distinguir EAGAIN de erro definitivo e implementar recriação com espera progressiva ou um watchdog para ausência prolongada de pacotes.

### 6. Média — relógio congelado pode fazer o buffer crescer sem limite

**Local:** `src/capture-filter.c:190`, `src/sr-buffer.c:105`, `src/sr-buffer.c:117`.

A proteção do relógio detecta timestamps menores que o anterior, mas não timestamps repetidos. A expiração usa apenas o relógio da fonte. Com captura automática de fps (`capture_fps=0`), frames recebidos continuamente com o mesmo timestamp podem continuar sendo codificados e armazenados sem expiração.

**Impacto:** em uma fonte defeituosa com relógio congelado, o buffer deixa de ser limitado pela duração configurada. A gravação também pode falhar porque pacotes consecutivos têm duração zero. O limite de 512 MiB da fila não protege os buffers vivos.

**Evidência reproduzida no buffer:** 1000 pacotes com timestamp idêntico permaneceram armazenados em um buffer configurado para 1 s. O crescimento indefinido decorre da condição de expiração; não foi realizado ensaio prolongado com uma câmera física defeituosa.

**Correção sugerida:** tratar repetição persistente de timestamps e impor um limite de memória ou de pacotes independente do relógio da fonte.

### 7. Média — dock não encontra replay colocado dentro de um grupo

**Local:** `src/sr-dock.cpp:133`, especialmente a linha 145.

O dock procura a cena com `obs_scene_find_source`, que só verifica itens diretamente dentro dela. Uma fonte Sports Replay dentro de um grupo não é encontrada. O caminho da hotkey utiliza outra busca, `sr_find_scene_with_source`, que considera grupos.

**Impacto:** o duplo clique pode carregar o arquivo e marcar o replay como exibido, mas não enviar a cena ao programa.

**Evidência:** chamada e implementação da busca conferidas no código do plugin e no libobs local. Não foi realizado teste de clique na interface.

**Correção sugerida:** compartilhar a busca de cena com o caminho das hotkeys, incluindo grupos, e marcar como exibido somente depois de um lançamento bem-sucedido.

### 8. Média — início da reprodução perde o primeiro frame e o primeiro bloco de áudio

**Local:** `src/playback-source.c:499`, `src/playback-source.c:579`.

O primeiro tick avança o playhead antes de mostrar uma imagem. A 30 fps, um replay de 30 fps começa no índice 1. No áudio, a condição `chunk->ts > from` descarta o bloco que começa exatamente no início da reprodução.

**Evidência reproduzida:** o primeiro frame mostrado teve luminância 18, correspondente ao índice 1, enquanto o frame inicial tinha luminância 16. Depois do primeiro tick, dois blocos de áudio haviam sido consumidos, mas apenas um emitido; o primeiro tinha timestamp zero.

**Impacto:** início cortado, particularmente perceptível em clipes curtos e sons com ataque imediato.

**Correção sugerida:** exibir a posição inicial antes do avanço e selecionar/recortar áudio pela interseção dos intervalos, sem excluir a igualdade no início.

### 9. Baixa — trocar a pasta apaga marcações de replays exibidos de outras pastas

**Local:** `src/sr-dock.cpp:388`, especialmente a linha 399.

`refreshList` monta `existing` somente com arquivos da pasta atual e remove de `playedPaths` tudo o que não está nesse conjunto. Ao trocar de pasta A para B, marcações de arquivos ainda existentes em A são removidas e a remoção é persistida.

**Impacto:** ao voltar à pasta anterior, os replays perdem a indicação de que já foram exibidos.

**Evidência:** algoritmo de limpeza confirmado por leitura. Não depende de exclusão real dos arquivos.

**Correção sugerida:** remover marcações somente quando o arquivo correspondente deixar de existir, ou manter o histórico separado por pasta.

### 10. Média — testes não são registrados no CTest nem executados no CI

**Local:** `CMakeLists.txt:69`, `.github/workflows/build-project.yaml:284`, `.github/scripts/Build-Windows.ps1`.

`SR_BUILD_TESTS` cria dois executáveis e habilita testing, mas não há chamadas a `add_test`. O workflow revisado compila e empacota sem habilitar/executar esses testes.

**Evidência executada:** `ctest --test-dir build_integration -C RelWithDebInfo -N` retornou `Total Tests: 0`, mesmo com `SR_BUILD_TESTS=ON`.

**Impacto:** uma execução de CTest ou pipeline bem-sucedida não demonstra que captura, AAC, idempotência ou recuperação continuam funcionando.

**Correção sugerida:** registrar testes com diretórios temporários exclusivos, ordenar o teste de recuperação depois do host e executar os testes no CI com as dependências de runtime necessárias. Adicionar cobertura das falhas reproduzidas nesta revisão.

## Limites e ordem de correção

Não foi iniciada uma sessão WebSocket real nesta revisão, nem operado o OBS de produção. O teste host usa a DLL real, com libobs e um adaptador de proc-handler para a API Vendor; ele não substitui validação da interface, transições e mixer em uma instância completa. Também não foram realizados testes de RTSP físico, encoders de hardware, disco cheio/desconectado, falta de energia ou carga prolongada. A checagem do instalador foi estática; ele não foi executado.

Prioridade recomendada: primeiro eliminar o acesso nulo durante criação e o estado residual de autoplay; depois corrigir a linha temporal do áudio e a recuperação/limitação da captura. Em seguida, resolver recorte, navegação do dock e início do playback, incorporando essas verificações à execução automática dos testes.

Para repetir os diagnósticos adicionais, após os testes de mídia terem produzido um `audio.mp4`:

```powershell
cmake -S audit/harness -B audit/harness/build -G 'Visual Studio 17 2022' -A x64
cmake --build audit/harness/build --config RelWithDebInfo --parallel 4
$env:PATH = 'C:/Program Files/obs-studio/bin/64bit;' + $env:PATH
./audit/harness/build/RelWithDebInfo/replay-audit.exe ./audit/20261017-172659/media/audio.mp4
```

O programa intercepta apenas a saída de áudio/vídeo para observar os dados enviados pelo código de playback. Seus serviços auxiliares têm stubs explícitos; ele não comprova sozinho comportamento audível/visual na interface do OBS.
