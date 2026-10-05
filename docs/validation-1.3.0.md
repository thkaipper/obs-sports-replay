# Validação e limites — 1.3.0

Testes executados em Windows x64 com OBS 32.2.2, obs-websocket 5.7.4 e dependências fixadas em buildspec.json. O binário final foi compilado em RelWithDebInfo com avisos tratados como erros. A instalação de produção não foi alterada.

## Cenários automatizados

- Mídia: timestamps do encoder, corte por instante/duração e keyframe, validade do snapshot após limpar o buffer, MP4 com AAC decodificável e sinal não nulo, vídeo sem áudio, recusa de sobrescrita, falha de escrita, alinhamento audiovisual e primeira imagem do clipe.
- Host libobs isolado: duas câmeras sem playback, UUIDs distintos, renomeação e duplicação, captura multicâmera, repetição idempotente, conflito de parâmetros, evento inválido, publicação sem partial, câmera ausente sem cancelar a câmera disponível, hotkeys independentes e migração, shutdown e desregistro.
- Reinício: nova session_id, recuperação de arquivo publicado, deduplicação sem câmera ativa e falha INTERRUPTED com limpeza de partial abandonado.
- OBS portátil real: WebSocket autenticado, descoberta de duas Media Sources com filtros, captura, repetição sem novo arquivo e dois eventos ReplaySaved. A instância portátil usa configuração independente.
- Inspeção PE: arquitetura x64, versão de API OBS 32.2.2 e resolução de imports de bibliotecas OBS/Qt/FFmpeg instaladas.

Os MP4 foram inspecionados e decodificados pelos testes com libavformat/libavcodec. Não foi utilizado ffprobe. Os logs e relatórios de aceitação acompanham a pasta evidence da distribuição.

## Ainda exige validação no ambiente operacional

RTSP real e sua latência/reconexão, NVENC/QSV/AMF nas placas do usuário, quatro ou mais câmeras, carga contínua de 24 horas, interrupção de energia, disco cheio real, armazenamento de rede e regressão visual completa dos controles manuais do dock/playback. Nenhum desses cenários é declarado aprovado nesta entrega.

O corte comum é por chegada monotônica ao processo, não por relógio físico da câmera. Streams com latências diferentes podem representar instantes físicos diferentes. Duração real pode exceder a solicitada até o keyframe anterior. Áudio só é incluído quando há áudio disponível no buffer; a API informa audio_present.

Fila: até 32 trabalhos e 512 MiB de snapshots. O buffer de cada filtro também consome memória fora desse limite. HealthChanged pode ter atraso do worker. O thumbnail inicial ainda é decodificado na interface. Shutdown aguarda as gravações aceitas e pode aguardar o armazenamento. Arquivos e journals não são removidos automaticamente por retenção; mantenha espaço livre e uma política externa que preserve a idempotência desejada.

Não há promessa de durabilidade absoluta contra perda de energia nem de compatibilidade com Linux/macOS nesta versão.
