# Logos, patrocinadores e textos sobre o replay

As propriedades Vídeo de abertura e Vídeo de encerramento controlam a sequência de vinhetas. Para uma logo permanecer sobre o replay, use uma camada acima da fonte Sports Replay na cena do OBS. Esses recursos de composição são do OBS, não novos controles do plugin.

## Logo fixa

1. Na cena Replay, adicione uma fonte Imagem com uma logo PNG transparente.
2. Coloque essa fonte acima da fonte Sports Replay na lista de fontes.
3. Posicione e redimensione a logo; depois bloqueie a posição com o cadeado.
4. Adicione outras fontes Imagem para outros patrocinadores ou crie um grupo para organizá-las.

Para reduzir a opacidade, adicione à logo o filtro Correção de cor e ajuste Opacidade. Isso define transparência constante; não programa um fade automático.

## Texto em movimento

1. Adicione uma fonte Texto (GDI+) com a mensagem de patrocínio.
2. Adicione o filtro Rolagem e ajuste a velocidade horizontal ou vertical.
3. Use o limite de largura/altura do filtro para delimitar a faixa. Posicione-a acima do replay, na região desejada.

Rolagem é um movimento contínuo. Ela também pode ser usada em outras fontes visuais, mas não representa uma animação de entrada, pausa e saída com pontos de posição.

## Anúncio animado e esmaecimento

Para uma animação pronta, use uma Fonte de mídia com o anúncio animado. Transparência depende do formato e do codec do arquivo; PNG é apropriado para logo estática. Para entrada, saída, alternância de patrocinadores e movimentos programados, use uma Fonte de navegador com um overlay que implemente esses efeitos, ou um recurso de animação previamente validado em seu OBS. O filtro Correção de cor sozinho não agenda esses movimentos.

## Máscara e vinhetas

Máscara de imagem/Mistura serve para recortar ou combinar a imagem de uma fonte. Uma logo patrocinadora sobre o vídeo costuma ser melhor representada por uma fonte independente. A camada na cena Replay pode aparecer também durante a abertura e o encerramento; ocultá-la somente nessas fases exige controle adicional.

## Transmissão e arquivo salvo são diferentes

As camadas da cena aparecem na saída composta do OBS. O MP4 salvo pelo Sports Replay contém o buffer capturado da câmera e o áudio solicitado; ele não incorpora as logos, textos ou vinhetas da cena de playback. Para exportar a composição, grave a saída composta do OBS ou implemente uma etapa dedicada de renderização. A versão 1.3.1 não adiciona renderização de patrocinadores ao arquivo salvo.

Referência oficial dos filtros: https://obsproject.com/kb/filters-guide
