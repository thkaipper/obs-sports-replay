# Sports Replay for OBS Studio

**Manutenção deste fork: [ITSync](https://itsync.xyz).** Versão 1.3.1: crédito da manutenção adicionado ao rodapé, preservando a autoria original da Systec. Consulte [o guia de logos e patrocinadores](docs/patrocinadores-pt-BR.md) e [as notas desta atualização](docs/release-notes-1.3.1-pt-BR.md).

## Atualização deste fork — 1.3.0

Este fork adiciona integração com sistemas externos ao **OBS Studio 32.2.2 no Windows x64**, usando o `main` upstream como base (commit `3ed1c2f`). A autoria original e a licença GPL foram preservadas.

- **API pelo obs-websocket:** descoberta das câmeras, consulta de saúde, captura e acompanhamento dos replays pelo vendor `sports-replay`.
- **Captura multicâmera sem playback:** filtros capturam diretamente seus buffers usando um instante de corte comum.
- **UUID persistente por filtro:** renomear uma fonte não muda a identificação usada pelo sistema externo; filtros duplicados recebem identidade própria.
- **Eventos idempotentes:** repetir `event_id` + `capture_id` consulta o mesmo trabalho, inclusive após reiniciar o OBS, enquanto o journal for preservado.
- **Gravação assíncrona com fila limitada:** MP4 com áudio AAC opcional, publicação por `.partial` e evento `ReplaySaved` após a conclusão.
- **Recuperação e diagnóstico:** status por câmera, identificação de sessão e tratamento de trabalhos interrompidos.
- **Controles manuais preservados:** hotkeys independentes com migração dos vínculos antigos, integração com playback/dock e tradução pt-BR.

Consulte [as novidades e os limites desta versão](docs/release-notes-1.3.0-pt-BR.md), [a API para integração](docs/vendor-api.md), [instalação e compilação](docs/build-windows.md) e [testes realizados](docs/validation-1.3.0.md).

**Validação:** mídia com AAC, captura de duas câmeras, deduplicação, recuperação após reinício e WebSocket autenticado em OBS 32.2.2 real. RTSP real, encoders de hardware e operação prolongada ainda precisam de validação no ambiente do usuário. Esta versão é uma extensão deste fork; não é uma release oficial do projeto original.

**Low-memory instant replay for live sports broadcasts.**

Sports Replay captures the last seconds of any camera into a **compressed**
in-memory buffer (hardware H.264 with configurable keyframes via NVENC / AMF / QSV, with an
x264 software fallback) instead of holding raw frames in RAM. A 15-second
1080p60 buffer uses on the order of **tens of megabytes instead of several
gigabytes**, so multi-camera replay setups run comfortably on ordinary
streaming PCs.

![Sports Replay running in a live off-road race broadcast](docs/screenshot-obs.png)

## Why

The popular raw-buffer replay plugins keep every frame uncompressed in memory
(~8 MB per 1080p frame). With several cameras and a long buffer that easily
reaches many gigabytes and pushes a PC to its limit mid-broadcast. Sports
Replay solves that by encoding the buffer with your GPU, keeping quality high
while cutting memory use by roughly **50–100×**.

## Features

- **Compressed replay buffer** — per-camera capture filter, buffer duration
  and quality configurable.
- **Auto-play on scene switch** — cut to the replay scene and it plays the
  last N seconds automatically.
- **Slow motion & reverse** — playback speed from 10% to 400%, hotkeys for
  speed presets and direction.
- **Configurable end action** — freeze on the last frame, **return to the
  previous scene**, **cut to the replay camera's scene**, or loop.
- **Sponsor bumpers** — optional intro and outro video clips played around
  the replay (e.g. a "REPLAY" sting with a sponsor).
- **Automatic save to disk** — every replay is saved as an `.mp4` (muxed
  from the already-encoded frames, no re-encode).
- **Replay bin dock** — a dockable panel shows the last replays as
  thumbnails; **double-click sends a saved replay to program** and returns
  to the previous scene when it ends. Perfect for re-showing a play minutes
  later.
- **Multi-camera** — one capture filter per camera; replay any of them.

## Integration update 1.3.0

This fork targets **OBS Studio 32.2.2 Windows x64** and adds a versioned
`sports-replay` Vendor API, direct filter capture, persistent event/camera
deduplication, bounded asynchronous saves, atomic MP4 publication and AAC audio.
See [Vendor API](docs/vendor-api.md), [build and installation](docs/build-windows.md),
[validation](docs/validation-1.3.0.md), and [changelog](docs/changelog-1.3.0.md).

## Requirements

- OBS Studio 32.2.2 Windows x64 for this fork. Other versions are not validated.
- A GPU with a hardware H.264 encoder recommended (NVIDIA/AMD/Intel); falls
  back to x264 software encoding otherwise.

## Installation

1. Download the installer from the [latest release](../../releases/latest).
2. Run it (it installs into your OBS plugins folder).
3. Restart OBS.

## How to use

1. **Add the capture filter to each camera.** Right-click a camera source →
   *Filters* → add **Sports Replay Capture**. Set the buffer duration and
   quality there.
2. **Add the playback source.** In your replay scene, add a **Sports Replay**
   source and pick which camera to replay from in its properties. Set the
   playback speed and, under *When the replay ends*, choose *Return to the
   previous scene*. Optionally set intro/outro sponsor clips.
3. **Live replay.** Cut to the replay scene → the last N seconds play
   automatically, then it returns to your main camera.
4. **Replay bin.** Open the **Replays (Sports Replay)** dock. It lists your
   saved replays with thumbnails; double-click any of them to send it to
   program. The save folder is set with the ⚙ button (defaults to
   `Videos/Sports Replay`).

### The capture filter (per camera)

![Sports Replay Capture filter](docs/screenshot-capture-filter.png)

### The playback source

![Sports Replay source properties](docs/screenshot-source-properties.png)

### The replay bin dock

![Replay bin dock with thumbnails](docs/screenshot-dock.png)

Assign hotkeys under *Settings → Hotkeys* (capture, play/pause, speed
presets, reverse, play last saved replay).

## Questions, ideas & feedback

Come say hi in [**Discussions**](../../discussions) — that is the best place to
reach me:

- ❓ **[Q&A](../../discussions/categories/q-a)** — something not working, or not
  sure how to set it up.
- 💡 **[Ideas](../../discussions/categories/ideas)** — a feature you need for
  your workflow. This is what drives the roadmap.
- 📺 **[Show and tell](../../discussions/categories/show-and-tell)** — using it
  on a real broadcast? I would love to see it.

For confirmed bugs, open an [issue](../../issues).
*Se habla español* — escribí en el idioma que prefieras.

## Building from source

Uses the standard [OBS plugin template](https://github.com/obsproject/obs-plugintemplate)
build system.

```sh
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Support development

Plugins like this take a lot of time, testing and late nights to build and keep
working across OBS updates. Sports Replay is free and open source, and it will
stay that way. If it has been useful for your broadcasts, a small contribution
helps me keep maintaining it, adding new features, and building more free tools
for the community. Every bit is genuinely appreciated — thank you for your
support! 🙏

- 💵 **Payoneer** — `systecinformatica@gmail.com`
- 🇦🇷 **Mercado Pago** (Argentina only) — alias `systecinformatica`
- ₿ **USDT (TRC-20)** — `TTHh4B9k9nbp3DB1DKN2XcPrVurPZFvPpz`

## Author

Developed by **Systec** — [www.systecinformatica.com.ar](https://www.systecinformatica.com.ar)

## License

GPL-2.0-or-later — Copyright (C) 2026 Systec (https://www.systecinformatica.com.ar)
