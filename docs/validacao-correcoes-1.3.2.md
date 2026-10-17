# Validação das correções — 1.3.2

Data: 17/10/2026. Plataforma: Windows x64, OBS Studio 32.2.2. O relatório original de diagnóstico é [revisao-sistema-2026-10-17.md](revisao-sistema-2026-10-17.md).

## Resultado

**Cinco grupos CTest aprovados, com 233 verificações internas PASS e nenhuma FAIL.** O teste adicional por WebSocket autenticado também passou, usando a DLL 1.3.2 em uma instância portátil separada. As duas câmeras chegaram a SAVED com áudio presente e emitiram ReplaySaved.

| Ponto da revisão | Correção e regressão coberta |
| --- | --- |
| Filtro consultado durante criação | Teste chama o callback Vendor real dentro de `create`, antes de o libobs preencher `context.data`; obtém OFFLINE com o UUID esperado, sem queda. Snapshot nulo também é testado. |
| Flag residual de arquivo | Testes de fonte ativa/inativa, consumo único da proteção, desativação e nova ativação com tentativa de captura. |
| Timestamps de áudio sobrepostos | Saída instrumentada verifica a duração em amostras de cada bloco e garante que o próximo timestamp não se sobreponha ao anterior. Pausa/retomada reancoram o relógio. |
| Áudio integral em snapshot curto | Duração e cutoff reduzem os blocos copiados. Pedido sem áudio cabe em orçamento menor; pedido com áudio é rejeitado antes da alocação quando não cabe. |
| Encoder sem recuperação | Injeção controlada de buffering temporário, erro fatal e ausência prolongada de pacotes no código real do filtro; teste verifica descarte, espera e recriação. |
| Relógio congelado | Mil frames repetidos não geram novas codificações. Buffer descarta pacotes/áudio repetidos, respeita limite de memória e mantém início decodificável por keyframe. |
| Dock e grupos | Widgets Qt e cenas/grupos reais do libobs: busca de fonte agrupada, recusa de lançamento sem cena/arquivo e marcação somente após carregamento e envio. |
| Corte no início do replay | Primeiro tick mostra frame zero e emite o bloco inicial. Reprodução completa preserva a cauda de áudio. Reinício, pausa, mute, slow motion, reverso, loop e intro/replay/outro também são exercitados. |
| Histórico entre pastas | Persistência de marcador em A, troca para B sem perder o marcador e remoção de marcador de arquivo excluído ao voltar à pasta correspondente. |
| Testes fora do CTest/CI | CTest descobre cinco grupos. Script local executado com sucesso; workflow Windows passa a executar os testes antes de empacotar e arquiva evidências. |

Os testes anteriores de remux MP4, AAC com sinal não silencioso, duração, publicação sem sobrescrita, snapshot após limpeza do buffer, captura multicâmera, renomeação, UUIDs distintos, conflitos de idempotência, falha isolada de câmera, vídeo sem áudio, migração de hotkey e recuperação após reinício continuam aprovados.

## Evidências e reprodução

No workspace, os resultados estão em:

- `audit/fixes-final-test.log`: configuração, build e resumo CTest finais.
- `audit/fixes-ctest-details.log`: saída detalhada com 233 PASS.
- `audit/fixes-live.log` e `audit/fixes-live.json`: conexão real autenticada, versão, capturas, deduplicação e eventos.
- `audit/fixes-binary.json`: checagem dos imports/ABI contra o runtime OBS instalado.
- `build_integration/test-results/RelWithDebInfo`: arquivos de mídia e journals isolados de cada execução.

Com um build configurado e o runtime do OBS disponível:

```powershell
./scripts/test-windows.ps1 -BuildDirectory build_integration -ObsRoot 'C:/Program Files/obs-studio'
```

O teste de inicialização usa o callback Vendor e o filtro reais. O teste de recuperação carrega a DLL real em dois processos sucessivos. As saídas de áudio/vídeo do teste de playback são instrumentadas; o teste de encoder injeta falhas controladas. O teste do dock utiliza Qt/libobs reais e intercepta somente o carregamento de arquivo e o comando final de troca de cena. O teste WebSocket complementar usa duas Media Sources reais em OBS completo.

## Limites

Não foram reproduzidos reset físico de GPU, RTSP de câmera física, disco cheio/desconectado, falta de energia nem operação de longa duração. Não há garantia absoluta para esses ambientes sem testes em campo. O workflow foi atualizado, mas sua execução remota no GitHub não ocorreu nesta sessão. O teste portátil foi encerrado após a validação; a instalação e o processo OBS de produção não foram substituídos.

DLL validada: SHA-256 `016e2b7c8837bfdc30bc2abf817facc7a4f58ed486ab3df73cbabc5bd7844144`.
