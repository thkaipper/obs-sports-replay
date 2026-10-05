# Compilação Windows — integração 1.3.0

Base: main de Voodoo25/obs-sports-replay, commit 3ed1c2f158dee9ee3d6bd8a4aad7f461498590c2. A versão 1.2.0 não foi usada como base. As correções posteriores de encoder, keyframe e relógio foram preservadas.

Alvo validado: Windows x64, OBS Studio 32.2.2, obs-websocket 5.7.4, Qt 6.11.1 e FFmpeg 8.1.2 (avcodec/avformat 62, avutil 60, swscale 9). Não distribuir esta DLL para OBS 31 ou arquiteturas diferentes.

Ferramentas usadas: Visual Studio 2022 Build Tools, MSVC 19.44.35229, Windows SDK e CMake com gerador Visual Studio 17 2022. As dependências e os SHA-256 estão fixados em buildspec.json. O fluxo normal baixa e verifica os SDKs oficiais; requer internet e espaço para compilar o SDK do OBS.

```powershell
./scripts/build-windows.ps1 -Tests
./scripts/package-windows.ps1 -InnoCompiler 'C:/caminho/ISCC.exe'
```

O instalador foi compilado com Inno Setup 6.7.3. Sem InnoCompiler, o segundo comando produz somente o ZIP. Para usar um SDK previamente extraído, passe -SdkRoot ao primeiro script e -BuildDirectory build_sdk ao segundo. Essa alternativa exige os mesmos pacotes fixados em buildspec.json.

## Validação

Feche a instância portátil de teste antes de trocar sua DLL. Os testes não exigem substituir a instalação de produção.

```powershell
$env:PATH = 'C:/Program Files/obs-studio/bin/64bit;' + $env:PATH
$env:QT_PLUGIN_PATH = 'C:/Program Files/obs-studio/bin/64bit'
./build_x64/RelWithDebInfo/sr-media-test.exe ./test-media
./build_x64/RelWithDebInfo/sr-integration-test.exe ./build_x64/RelWithDebInfo/sports-replay.dll ./data ./test-host
./build_x64/RelWithDebInfo/sr-integration-test.exe ./build_x64/RelWithDebInfo/sports-replay.dll ./data ./test-host recover
python ./scripts/verify-binary.py ./build_x64/RelWithDebInfo/sports-replay.dll 'C:/Program Files/obs-studio/bin/64bit'
```

Use diretórios novos para os testes de mídia e host: publicação não sobrescreve arquivos existentes. O teste live-websocket.py requer Python websockets e uma instância OBS portátil dedicada com autenticação habilitada. Ele remove os inputs dessa instância de teste; não aponte para o OBS de produção. Consulte o cabeçalho do script para os argumentos.

## Instalação

Feche o OBS, faça backup do plugin atual e instale no diretório do OBS. No ZIP, copie sports-replay/bin/64bit/sports-replay.dll para obs-plugins/64bit e sports-replay/data para data/obs-plugins/sports-replay. O instalador faz esse mapeamento e permite escolher o diretório. Esta entrega não substituiu o plugin da instalação de produção.

Adicione o filtro Sports Replay Capture a cada câmera. O sistema externo deve descobrir capture_id por ListCaptureSources e usar CallVendorRequest; nomes de fontes não são chaves persistentes. Veja vendor-api.md. Teste as câmeras reais antes de colocar em operação contínua.

GPL e autoria do projeto original permanecem nos arquivos. Esta versão é uma extensão local; não é uma release oficial do upstream. O instalador gerado não possui assinatura de publicação.
