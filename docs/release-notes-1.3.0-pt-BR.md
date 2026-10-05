# Sports Replay 1.3.0 — integração com OBS 32.2.2

Esta atualização permite que sistemas externos solicitem e acompanhem replays de várias câmeras por obs-websocket, sem exigir uma fonte de playback para iniciar a captura. O plugin continua oferecendo seus controles manuais de replay.

Base upstream: `Voodoo25/obs-sports-replay`, `main` no commit `3ed1c2f158dee9ee3d6bd8a4aad7f461498590c2`. As correções de buffer, keyframe, encoder e relógio posteriores à release 1.2.0 foram preservadas. Alvo validado: Windows x64, OBS 32.2.2 e obs-websocket 5.7.4.

## Funcionalidades novas

- Vendor API `sports-replay`, versão 1: `GetPluginInfo`, `ListCaptureSources`, `GetCaptureSource`, `GetHealth`, `CaptureEvent` e `GetReplayStatus`.
- Eventos `ReplayAccepted`, `ReplayStarted`, `ReplaySaved`, `ReplayFailed` e `HealthChanged` para acompanhar o ciclo de captura e gravação.
- UUID persistente por filtro e identificação da fonte proprietária, protegendo a integração contra renomeações e duplicação de filtros.
- Captura multicâmera com instante monotônico comum, seleção de duração e falhas independentes por câmera.
- Deduplicação persistente por `event_id` + `capture_id`, com detecção de conflito quando os parâmetros mudam.
- Worker de gravação, limitado a 32 trabalhos e 512 MiB de snapshots, sem gravar o arquivo na callback de vídeo.
- Vídeo remuxado sem reencodificação e áudio AAC opcional. Arquivos publicados por `.partial`, sem sobrescrever destinos existentes; sucesso informado depois da publicação.
- Recuperação de arquivos já publicados e tratamento de trabalhos interrompidos após reiniciar o processo.
- Consulta de buffer, encoder, disponibilidade de câmera, armazenamento e sessão.
- Hotkeys por identidade de fonte com migração das teclas antigas, correções de timestamps e primeira imagem, reprodução de áudio salvo, cache de thumbnails limitado e tradução pt-BR.

## Integração

Use `CallVendorRequest` com `vendorName: "sports-replay"`. Descubra `capture_id` com `ListCaptureSources`; depois envie `CaptureEvent` com um `event_id` estável e `cameras` como uma lista de objetos `{ "capture_id": "UUID" }`. Consulte `GetReplayStatus` ou acompanhe os eventos Vendor. Nomes de fontes não devem ser usados como identidade persistente.

O contrato completo, exemplos e códigos de erro estão em [vendor-api.md](vendor-api.md). A implementação suporta captura do conteúdo já disponível no buffer; não implementa pós-roll.

## Validação e limites

Compilação RelWithDebInfo com avisos tratados como erros. Testes aprovados: snapshots e keyframes, MP4/AAC decodificável, captura de duas câmeras, deduplicação, câmera ausente sem cancelar outra, hotkeys, shutdown, recuperação após reinício e WebSocket autenticado em uma instância portátil real do OBS 32.2.2.

Ainda exigem testes operacionais: RTSP real, NVENC/QSV/AMF no hardware do usuário, quatro ou mais câmeras, 24 horas de carga, disco cheio, armazenamento de rede e interrupção de energia. O corte comum usa chegada ao processo; não sincroniza os relógios físicos das câmeras. Os limites completos estão em [validation-1.3.0.md](validation-1.3.0.md).

## Compilação e instalação

Consulte [build-windows.md](build-windows.md). Dependências e hashes estão fixados em `buildspec.json`; os scripts de compilação, empacotamento e verificação estão em `scripts/`. Feche o OBS e faça backup da DLL atual antes de instalar.

Código-fonte disponibilizado sob a licença original GPL. Autoria e créditos originais preservados. Esta atualização pertence ao fork `thkaipper/obs-sports-replay` e não representa uma release oficial do upstream.
