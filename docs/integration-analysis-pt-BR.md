# Análise e arquitetura proposta para integração

Data: 04/10/2026. Este documento é uma proposta anterior à implementação; não documenta recursos já disponíveis.

## Base verificada

- Repositório: https://github.com/Voodoo25/obs-sports-replay
- `main` consultado: `3ed1c2f158dee9ee3d6bd8a4aad7f461498590c2`.
- Comparei os 115 arquivos rastreados do upstream com esta pasta, normalizando CRLF/LF: nenhuma diferença. Esta pasta não contém `.git`; arquivos adicionais, inclusive binários, não foram considerados nessa equivalência.
- O histórico posterior à atualização de versão inclui `c57a1aa` (intervalo de keyframes e FPS) e `3ed1c2f` (reinício do buffer após regressão do clock). Essas correções devem permanecer.
- O buildspec desta pasta ainda usa OBS 31.1.1 e pacotes de dependências de 2025-07-11.
- A compilação local anterior em `C:/Users/User/sports-replay-build/obs-sports-replay` usa OBS 32.2.2, obs-deps e pacote Qt de 2026-07-15. Cache: Visual Studio 17 2022; Qt dos pacotes: 6.11.1. A DLL adaptada importa avcodec-62, avformat-62, avutil-60 e swscale-9.
- Os hashes Windows já constam nesse buildspec local e podem ser reaproveitados após verificar os respectivos arquivos. Os hashes macOS permaneceram antigos: não copiar essa configuração inteira e alegar suporte multiplataforma.
- A versão do plugin permanece 1.2.0 nos dois binários; ela não identifica por si só a base de código nem a compatibilidade. A próxima distribuição precisa de versão própria e manifesto com commit e dependências.

## Funcionamento atual confirmado no código

1. `capture-filter.c` recebe vídeo assíncrono, converte/codifica via `sr-codec.c` e alimenta `sr-buffer.c`. O buffer contém vídeo comprimido e áudio float planar.
2. `sr_capture_get_buffer()` expõe o buffer a partir dos dados de uma instância do filtro. A fonte playback procura uma fonte pelo nome, enumera seus filtros e seleciona o primeiro Sports Replay Capture.
3. `sr_buffer_snapshot()` clona referências dos pacotes e duplica áudio sob o mutex do buffer. O snapshot preserva o vídeo mesmo quando o ring buffer avança.
4. `sr_playback_capture_replay_ex()` salva o snapshot sincronicamente antes de instalar a reprodução. A captura somente para salvar também depende de uma fonte playback.
5. `sr-save.c` remuxa vídeo em MP4 sem recodificá-lo. Cria apenas uma faixa de vídeo; não salva áudio. Escreve diretamente no nome final e ignora o resultado do trailer e do fechamento.
6. O nome atual é fonte + horário até segundos. Há colisões e falta sanitização. `sr-dock.cpp` interpreta o nome para decidir qual fonte playback usar.
7. As hotkeys são registradas por fonte com nomes como `SportsReplay.Capture`. O OBS tem contexto da fonte; nomes repetidos não provam que os atalhos manuais se confundam. Porém, a identificação desses atalhos por software externo é inadequada. Mudar os nomes exige preservar/migrar os bindings existentes.
8. Não há Vendor API, fila de save, identidade persistente de filtro, catálogo de jobs ou confirmação estruturada.

## Relação com os problemas anteriores

- Gravação síncrona, nomes com colisão, áudio ausente e resultado de finalização ignorado são parte da correção P0.
- Corrigir `sr_clip_advance()`: o AVFrame escolhido para saída é reutilizado ao decodificar o próximo quadro antes do retorno. Usar referências/quadro separado preservando a vida útil prometida.
- Tornar Qt e frontend coerentes no CMake. O preset habilita ambos, mas as opções podem desativar dependências usadas incondicionalmente.
- Atualizar README: o padrão de keyframe é 15, não all-intra.
- Miniaturas são abertas/decodificadas na thread da interface em cada atualização. Implementar cache e considerar trabalho em background para evitar pausas com disco lento.

## Arquitetura proposta

`Vendor API → coordenador de captura → snapshots por filtro → fila limitada → worker → MP4 temporário → publicação atômica → catálogo de status → VendorEvent`.

A captura manual e a API compartilham o serviço de save. Playback, cenas e controles continuam funcionando. O worker recebe apenas snapshot e metadata com ownership definido; nunca depende de ponteiros crus para filtros, fontes ou encoder.

### Identidade

Persistir UUID `capture_id` nas configurações do filtro, gerar uma vez e preservar após rename/restart. Ao duplicar/importar um filtro, detectar colisão de UUID e gerar identidade nova apenas para a cópia. Informar também identidade da coleção de cenas para distinguir configurações.

Uma fonte playback possui UUID próprio: ela pode não ter câmera configurada ou mudar de câmera. As hotkeys usam esse UUID e descrição amigável. A câmera selecionada deve passar a ser referenciada por `capture_id`, mantendo migração da seleção antiga por nome.

### Idempotência e recuperação

Chave: `(event_id, capture_id)`. Reservar a chave sob lock antes do snapshot; pedidos concorrentes recebem o estado existente. Guardar os parâmetros originais: uma repetição com duração/áudio divergentes retorna conflito, sem nova captura.

Para resistir ao restart, manter journal/catálogo persistente do plugin, sem banco do sistema externo. O registro durável de aceitação deve existir antes de confirmar `ACCEPTED`. Após crash, um job aceito sem arquivo concluído fica `FAILED/INTERRUPTED`; não recapturar o buffer novo para o mesmo lance. Se o MP4 já foi publicado e apenas a atualização do status faltou, recuperar pelo manifesto e arquivo correspondente.

`session_id` muda a cada inicialização; isso não apaga a deduplicação persistente. Se houver retenção limitada, documentar explicitamente a janela de idempotência. Limpeza de memória não pode silenciosamente permitir repetir um evento ainda dentro dessa garantia.

Estados públicos mínimos: `ACCEPTED`, `SAVE_QUEUED`, `SAVING`, `SAVED`, `FAILED`. `SNAPSHOT_CREATED` pode ser transição interna. Falha anterior à aceitação pode permitir retry explícito; falha posterior não autoriza uma captura nova automática.

### Multicâmera e duração

Definir um cutoff monotônico único por requisição e selecionar dados até esse instante. Guardar tempo de chegada monotônico além do timestamp da mídia; timestamps de fontes diferentes podem não ter a mesma origem. Informar desvio entre câmeras e duração efetiva.

Cutoff comum não garante sincronização física de câmeras RTSP com latências diferentes. A API deve expor essa limitação; sincronização precisa exige origem/relógio comum nas câmeras ou calibração externa.

Manter keyframe inicial decodificável ao recortar 10/15/30 segundos. Sem recodificar vídeo, o início pode precisar incluir até um GOP extra. Retornar duração solicitada, duração real e política de alinhamento; não prometer corte exato arbitrário. A falha de uma câmera não cancela as outras.

### Fila, memória e shutdown

Fila limitada por jobs e bytes estimados, incluindo áudio duplicado. Reservar capacidade antes de aceitar/snapshotar. Retornar `QUEUE_FULL`/`MEMORY_LIMIT` sem bloquear as threads do OBS. Começar com um worker de save e ampliar concorrência apenas após medir.

Definir ordem de locks; nenhum acesso ao frontend Qt ou escrita em disco enquanto o mutex do buffer está retido. Proteger duração e demais configurações usadas nas threads; hoje `sr_capture_update()` altera campos que callbacks consultam sem a mesma proteção.

No shutdown: fechar admissão; desregistrar requests; aguardar callbacks ativos; finalizar ou cancelar fila de forma definida; juntar workers; liberar catálogo/configuração por último. Cancelamentos devem ter status recuperável. Testar especificamente callbacks/eventos concorrentes ao unload.

### Save atômico e áudio

Nome final baseado em IDs, sem dependência do nome amigável. Preferir diretório por evento com nome codificado ou hash determinístico para evitar colisões decorrentes da sanitização e caminhos excessivos no Windows.

Abrir formato `mp4` explicitamente para arquivos `.mp4.partial`; a inferência pelo sufixo `.partial` falha. Gravar no mesmo volume/diretório do destino; validar header, pacotes, trailer, fechamento e flush necessário; publicar por rename sem sobrescrever replay existente. Somente depois marcar `SAVED` e emitir evento. Garantias em compartilhamentos de rede precisam de teste próprio.

Limpar apenas partials identificados como pertencentes ao plugin e abandonados, sem afetar outro processo ou job ativo. Persistir metadata suficiente para recuperar uma publicação interrompida entre rename e atualização do catálogo.

`save_audio=true` exige AAC, conversão adequada, alinhamento ao intervalo do vídeo e tratamento de lacunas. Acrescentar libswresample quando necessário e carregar áudio de MP4 para preservar reprodução pelo dock. Não anunciar `audio_save=true` antes de validar o caminho completo. Se a primeira etapa não entregar áudio, responder claramente como recurso indisponível, sem ignorar `save_audio=true`.

### Saúde

Separar último frame recebido de último pacote codificado: câmera recebendo dados em formato não suportado não está apta a gerar replay. `last_frame_age_ms` usa relógio monotônico de chegada, sem comparar diretamente timestamp de câmera com relógio do sistema.

Expor encoder solicitado/real, fallback, erro, resets de clock, buffer disponível, memória estimada e fila. Fallback explícito de hardware para software implica `DEGRADED`; seleção `auto` deve ter política documentada. Encoder com falha hoje pode ficar desativado até mudança de settings: avaliar recuperação controlada com backoff para uso 24x7.

Health global inclui `session_id`, integração disponível, diretório, espaço livre, fila e último erro. Disco livre é observação, não garantia de que um save futuro terá sucesso.

## Vendor API pública

O header público oficial fornece `obs_websocket_register_vendor`, `obs_websocket_vendor_register_request`, `obs_websocket_vendor_unregister_request` e `obs_websocket_vendor_emit_event`. Registrar o vendor em `obs_module_post_load()`, conforme exigência explícita do header. O mecanismo usa proc handlers do OBS: a integração pode ficar indisponível sem impedir uso manual. Fixar a revisão do header compatível com o obs-websocket instalado.

Referências: https://github.com/obsproject/obs-websocket/blob/master/lib/obs-websocket-api.h e https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md .

Vendor proposto: `sports-replay`. Requests: `GetPluginInfo`, `ListCaptureSources`, `GetCaptureSource`, `GetHealth`, `CaptureEvent`, `GetReplayStatus`. Events: `ReplayAccepted`, `ReplaySaveStarted`, `ReplaySaved`, `ReplayFailed`, `CaptureSourceHealthChanged`.

O cliente deve assinar eventos Vendors e consultar `GetReplayStatus` ao reconectar: eventos WebSocket não substituem um catálogo persistente. Ausência de resposta por timeout pode ser desconexão; distinguir isso de uma resposta válida indicando vendor/request indisponível antes de concluir `PLUGIN_NOT_LOADED`.

O header usa `obs_data_t`, cuja representação de arrays não é equivalente a JSON arbitrário. Preferir e testar `cameras: [{"capture_id":"..."}]` em vez de prometer que o array de strings do exemplo será preservado pelo callback da versão instalada.

Exemplo proposto, ainda não implementado:

```json
{
  "requestType": "CallVendorRequest",
  "requestData": {
    "vendorName": "sports-replay",
    "requestType": "CaptureEvent",
    "requestData": {
      "event_id": "SR01-20261004-194215-000127",
      "duration_ms": 30000,
      "save_audio": true,
      "cameras": [{"capture_id": "550e8400-e29b-41d4-a716-446655440001"}]
    }
  }
}
```

Publicar códigos estáveis, incluindo os solicitados e `QUEUE_FULL`, `MEMORY_LIMIT`, `REQUEST_CONFLICT`, `INTERRUPTED`, `AUDIO_UNAVAILABLE`. Distinguir erro de transporte, rejeição e estado terminal por câmera. `save_only=true` é o comportamento inicial da automação; não adicionar controle de playback implícito nessa etapa.

## Arquivos previstos

| Arquivos existentes | Alteração prevista |
| --- | --- |
| `buildspec.json`, `CMakePresets.json`, `CMakeLists.txt`, `cmake/FindFFmpeg.cmake` | Dependências Windows fixadas, integração opcional, áudio e configuração coerente |
| `.github/workflows/build-project.yaml`, `windows-installer.yaml`, scripts de build/package, `installer/sports-replay.iss` | Build reproduzível, verificação de imports, ZIP/PDB/installer da mesma revisão |
| `src/plugin-main.c` | Inicialização ordenada, registro da API e shutdown |
| `src/capture-filter.c`, `sr-capture.h` | UUID, registro de filtros, captura direta, health e sincronização |
| `src/sr-buffer.c/h`, `sr-codec.c/h` | Snapshot com cutoff/duração, métricas e timestamps corretos |
| `src/sr-save.c/h` | Resultado detalhado, publicação atômica, AAC opcional |
| `src/playback-source.c`, `sr-load.c/h` | Serviço de save compartilhado, hotkeys/migração, seleção por ID e áudio de arquivo |
| `src/sr-dock.cpp`, `sr-thumb.c` | Metadata para roteamento, arquivos prontos, cache de miniaturas |
| `src/sr-config.c/h`, `sr-clip.c` | Opções/persistência e correção de quadro de vinheta |
| `data/locale/*.ini`, `README.md`, `docs/*` | API, instalação, changelog, pt-BR e limites |

Novos módulos sugeridos: `sr-integration` (Vendor API), `sr-capture-registry` (identidade/consulta segura), `sr-jobs` (admissão/fila/worker), `sr-job-store` (idempotência durável/recuperação). Evitar reescrever codecs, playback ou gerenciamento de cenas sem necessidade.

## Etapas de entrega e critérios

1. Fixar commit upstream e build Windows 32.2.2 reproduzível; preservar GPL e autoria; produzir artefatos em pasta separada sem instalar no OBS em uso.
2. Identidade/registro/health, migração de hotkeys e API de consulta.
3. Captura direta multicâmera, idempotência persistente, fila limitada e save atômico; integrar captura manual ao serviço.
4. Duração e metadata, áudio AAC/sincronização, recuperação de partials e dock.
5. Testes automatizados de jobs/duplicação/erros/shutdown e teste de integração Vendor API. Validar arquivos com ffprobe, áudio e duração; executar matriz de 1/2/4+ câmeras, reconexão RTSP, fallback, disco cheio e crash controlado.
6. Entregar fonte completo, diff contra commit fixado, DLL/PDB/ZIP, preferencialmente installer, API JSON, changelog e relatório de evidências.

Idempotência, fila limitada, session_id e timestamps de health são requisitos de confiabilidade da primeira versão mesmo quando listados como P1. Áudio pode ser etapa posterior apenas com capability/erro honestos. Não distribuir build com status de produção antes de validar os cenários essenciais no OBS real.

## Evidências e limites

- VALIDADO ESTATICAMENTE nesta análise: equivalência da pasta com upstream, funcionamento do save/snapshot/hotkeys no código, configurações do build anterior, existência da API pública e proposta de alterações.
- EVIDÊNCIA DE EXECUÇÃO ANTERIOR: logs locais da DLL adaptada registraram load, encoder NVENC em duas fontes e save. Isso não é teste da integração proposta.
- NÃO TESTADO: Vendor Requests/Eventos neste plugin, idempotência, queue/worker, save atômico, áudio AAC, comportamento em crash/shutdown, stress 24x7. Nenhum desses recursos foi implementado nesta etapa.
- OBS 32.2.2: alvo e DLL adaptada com evidência local parcial. OBS 31.x e 32.0.x: não validados para o novo build; não prometer compatibilidade da mesma DLL com ABI antiga de FFmpeg.
- Nenhum código-fonte, buildspec ou binário existente foi alterado nesta etapa; apenas este relatório foi acrescentado.
