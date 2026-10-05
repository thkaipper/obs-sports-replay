# Alterações da integração 1.3.0

- Base no main 3ed1c2f, preservando as correções posteriores à release 1.2.0.
- Compilação Windows x64 para OBS 32.2.2 e bibliotecas atuais, com hashes de dependências fixados.
- API Vendor sports-replay: informações, descoberta de filtros, saúde, captura multicâmera e consulta de status; eventos de aceitação, início, sucesso, falha e saúde.
- Identidade UUID por filtro e identificação da fonte proprietária para evitar reutilizar UUID ao duplicar filtros. Hotkeys por UUID da fonte, com migração de vínculos antigos.
- Captura direta dos buffers sem depender de uma fonte de playback. Corte monotônico comum entre câmeras e seleção da duração respeitando keyframes.
- Salvamento em worker com fila limitada, idempotência persistente por event_id/capture_id e recuperação após reinício.
- MP4 com remux de vídeo, áudio AAC opcional, fechamento e publicação de .partial sem sobrescrever destinos. ReplaySaved ocorre depois da publicação.
- Leitura de áudio dos arquivos salvos, proteção de estado compartilhado, correção da primeira imagem do clipe e mapeamento de timestamps do encoder.
- Identificação por capture_id na integração com playback/dock; cache de thumbnails limitado; tradução pt-BR.
- Testes de mídia, host isolado e WebSocket real; scripts de compilação, verificação e empacotamento.

Não implementa pós-roll, sincronização física das câmeras, servidor externo ou aplicativo comercial. A captura usa o conteúdo já disponível nos buffers locais.
