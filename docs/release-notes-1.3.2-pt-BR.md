# Sports Replay 1.3.2

Atualização de correções para OBS Studio 32.2.2 no Windows x64. Autoria original da Systec e manutenção deste fork pela ITSync preservadas.

Os dez pontos da revisão de 17/10/2026 foram tratados:

- Consulta de saúde e snapshot seguros enquanto um filtro ainda está sendo inicializado.
- Limpeza da proteção de reprodução de arquivo ao sair da cena, permitindo a próxima captura automática.
- Timestamps de áudio baseados na posição das amostras, com espaçamento preservado entre blocos.
- Primeiro frame e primeiro bloco de áudio preservados ao iniciar ou reiniciar um replay.
- Snapshots copiam somente os blocos de áudio que intersectam o vídeo selecionado. Trabalhos sem áudio não duplicam o áudio do buffer; a verificação de memória considera o recorte antes da cópia.
- Recriação automática após erro fatal de encoder ou dez segundos sem produzir pacotes, com intervalo de dez segundos entre tentativas.
- Descarte de timestamps repetidos e limite independente de aproximadamente 512 MiB por buffer vivo, mantendo limites de GOP na remoção de vídeo. Em bitrates muito altos, a duração disponível pode ser menor que a configurada.
- Dock localiza fontes dentro de grupos e não marca um replay como exibido quando não consegue localizar a cena ou carregar o arquivo.
- Histórico de replays exibidos preservado ao trocar de pasta.
- Cinco grupos de testes registrados no CTest, executados antes do empacotamento no workflow Windows. Cada execução usa diretórios isolados.

A API Vendor permanece na versão 1, com os mesmos nomes de requests, identificação por UUID e deduplicação persistente. O build também acompanha mudanças de versão em `buildspec.json`, e o teste WebSocket cria seus próprios inputs/cena sem apagar os inputs existentes.

Validação local: compilação com warnings tratados como erros, cinco grupos CTest aprovados, 233 verificações internas PASS e teste WebSocket autenticado em OBS portátil separado, com duas fontes reais, AAC, deduplicação e eventos ReplaySaved. Detalhes e limites estão em [Validação das correções](validacao-correcoes-1.3.2.md).

Para instalar, feche o OBS e use o ZIP `sports-replay-1.3.2-windows-x64.zip`: copie a DLL de `sports-replay/bin/64bit` para `obs-plugins/64bit` e as traduções de `sports-replay/data` para `data/obs-plugins/sports-replay`, dentro da instalação do OBS.
