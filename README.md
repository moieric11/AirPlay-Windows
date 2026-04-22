# AirPlay Windows Receiver

Récepteur AirPlay 2 (mirroring) pour Windows, porté depuis
[UxPlay](https://github.com/FDH2/UxPlay) (GPL-3.0) avec un stack 100 %
natif Windows (pas de Bonjour SDK ni de dépendance Apple au runtime).

> **État actuel — récepteur AirPlay 2 fonctionnel sur Windows.**
>
> Un iPhone / iPad / Mac du même Wi-Fi voit `AirPlay-Windows` dans son
> Centre de contrôle. La recopie d'écran s'affiche **en temps réel dans
> une fenêtre SDL2** (498×1080 pour un iPhone 16 portrait, resizable,
> conserve l'aspect). L'audio d'Apple Music, des vidéos natives et des
> notifications iOS **sort du périphérique audio Windows par défaut**
> via WASAPI/SDL.
>
> Ce qui ne marche pas : les apps qui forcent l'**AirPlay Streaming
> mode** avec FairPlay DRM (YouTube / Netflix / Apple TV+) restent
> hors scope — elles demandent un protocole et une DRM distincts, à
> plusieurs jours de travail.

## Ce qui fonctionne

| Étape                                       | État | Source UxPlay portée                         |
|---------------------------------------------|------|----------------------------------------------|
| mDNS `_airplay._tcp` + `_raop._tcp`         | ✅   | Windows `dnsapi.dll` (natif, zéro SDK Apple) |
| `GET /info` — bplist00 complet              | ✅   | `plist/info.plist` (via libplist)            |
| `POST /pair-setup` (no-SRP)                 | ✅   | `lib/pairing.c`                              |
| `POST /pair-verify` (X25519+AES-CTR+Ed25519)| ✅   | `lib/pairing.c`                              |
| `POST /fp-setup` (4×142 replay)             | ✅   | `lib/fairplay_playfair.c` (blobs isolés)     |
| AirPlay 2 `SETUP` (session + streams plist) | ✅   | `raop_handler_setup`                         |
| `GET_PARAMETER volume`                      | ✅   | `raop_handler_get_parameter`                 |
| `RECORD`, `TEARDOWN` partiel + session      | ✅   | `raop_handler_teardown`                      |
| Client NTP (poll iOS timing server)         | ✅   | `lib/raop_ntp.c`                             |
| Mirror TCP listener + frame parser          | ✅   | `lib/raop_rtp_mirror.c`                      |
| Parse SPS (profile/level/résolution)        | ✅   | H.264 spec, standalone                       |
| **FairPlay decrypt (ekey → AES key)**       | ✅   | `lib/playfair/*` (provisioned locally)       |
| **AES post-hash avec ECDH secret**          | ✅   | `raop_handler_setup` (modern-client path)    |
| **AES-CTR des NAL H.264 (in-place decrypt)**| ✅   | `lib/mirror_buffer.c`                        |
| **Split NAL + conversion Annex-B**          | ✅   | `raop_rtp_mirror_thread` (port-by-port)      |
| **Décodeur H.264 (libavcodec)**             | ✅   | FFmpeg — SPS/PPS extraits de l'avcC          |
| **Renderer vidéo temps réel (SDL2)**        | ✅   | SDL2 IYUV texture, GPU YUV→RGB               |
| **Audio UDP RTP + AES-CBC decrypt**         | ✅   | `lib/raop_buffer.c`                          |
| **RAOP RTP seq dedup**                      | ✅   | bitset 65k, sliding window                   |
| **Décodeur AAC-ELD (libavcodec)**           | ✅   | ASC `F8 E8 50 00` construit from-scratch     |
| **Sortie audio SDL2 / WASAPI**              | ✅   | int16 stéréo 44.1 kHz, push mode             |
| AirPlay streaming mode (FairPlay Streaming) | ❌   | YouTube / Netflix / ATV+ — hors scope        |
| Volume control (SET_PARAMETER → mixer)      | ❌   | loggé mais pas câblé                         |
| Seeking / pause / métadonnées DAAP          | ❌   | logs only pour l'instant                     |

## Stratégie

Copier UxPlay au plus près ligne par ligne, valider à chaque étape
contre un iPhone réel. Pas d'improvisation : chaque fichier cite sa
source dans UxPlay et conserve la licence GPL-3.0 compatible.

## Prérequis (Windows)

- **Visual Studio 2022** (ou Build Tools) avec composants C++
- **CMake 3.20+**
- **vcpkg** (pour OpenSSL + libplist, déclarés dans `vcpkg.json`)
- **Windows 10 1809+ ou Windows 11** (API mDNS native `DnsServiceRegister`)

Aucune dépendance Apple requise : le récepteur s'annonce via `dnsapi.dll`
de Windows, pas via Bonjour SDK.

## Build

```powershell
git clone <url-du-repo>
cd airplay-windows

# Si ton VS embarque vcpkg (typique 2022+) :
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="C:\Program Files (x86)\Microsoft Visual Studio\17\BuildTools\VC\vcpkg\scripts\buildsystems\vcpkg.cmake" `
  -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

Première config : vcpkg télécharge et compile `libplist` + `openssl`
(~5 min). Ensuite c'est caché.

## FairPlay — provisioning local

Le handshake `/fp-setup` utilise 4 réponses pré-enregistrées d'Apple TV
(568 bytes au total), embarquées dans UxPlay sous licence GPL-3.0. Elles
ne sont pas commitées dans ce repo. Procédure de provisioning locale
dans `docs/FAIRPLAY.md` — il suffit de créer un fichier
`third_party/fairplay_blobs_real.cpp` (gitignored) avec les bytes
correspondants, CMake le détecte automatiquement.

Sans ces blobs, iOS décroche à `/fp-setup`. Avec, la session s'ouvre.

## Lancement

```powershell
build\Debug\airplay-windows.exe
```

Logs attendus au démarrage :

```
ip=192.168.x.x
TCP server listening on 0.0.0.0:7000
mDNS registered: AirPlay-Windows._airplay._tcp.local
mDNS registered: XXXXXXXXXXXX@AirPlay-Windows._raop._tcp.local
```

Puis sur iPhone / iPad / Mac du même Wi-Fi → Centre de contrôle →
Recopie d'écran → `AirPlay-Windows`. Au tap, les logs montrent tout le
handshake + la réception du flux H.264.

## Tests

Suite Python qui joue le rôle d'iOS pour 6 scénarios (pair-verify
aller-retour, URI absolue + headers iOS réels, signature corrompue,
sessions concurrentes, fp-setup framing, ANNOUNCE/SETUP/RECORD/TEARDOWN
legacy) :

```bash
pip install cryptography
python3 tools/test_pair_verify.py
# === 26/26 checks passed ===
```

## Prochaines étapes

La chaîne bout-en-bout marche pour mirror + audio RAOP. Les prolongements
naturels, par priorité décroissante :

1. **Câbler le volume** — iPhone envoie `SET_PARAMETER "volume: -X.Y"`
   que l'on logge déjà ; il suffit de traduire la valeur dB (0 dB = max,
   -144 dB = muet) vers un gain linéaire appliqué au buffer PCM avant
   `SdlAudioOutput::push`.
2. **DAAP metadata** — parser les `mlit` (titre, artiste, album, cover
   art) envoyés en `SET_PARAMETER` pour les afficher dans la fenêtre
   SDL quand l'écran mirror n'est pas actif.
3. **ALAC support** — quand `ct=2` (iTunes legacy, AirPlay 1), le flux
   est en ALAC au lieu d'AAC-ELD. `libavcodec` a un décodeur `alac`
   disponible via `AV_CODEC_ID_ALAC` ; c'est ~30 lignes pour dispatcher.
4. **AirPlay Streaming mode** pour YouTube / Netflix / Apple TV+.
   Protocole distinct (`/reverse`, `/play` avec URL, FairPlay Streaming
   DRM). Plusieurs jours de travail, licence DRM sensible.

## Références

- **[UxPlay](https://github.com/FDH2/UxPlay)** — référence primaire, fork actif de RPiPlay, maintenu iOS 17/18 (GPL-3.0)
- [RPiPlay](https://github.com/FD-/RPiPlay) — ancêtre, non maintenu depuis 2021 (GPL-3.0)
- [OpenAirplay](https://openairplay.github.io/airplay-spec/) — notes de protocole
- [playfair](https://github.com/EstebanKubata/playfair) — décrypteur FairPlay
- [shairport-sync](https://github.com/mikebrady/shairport-sync) — pour l'audio RAOP

## Licence

Porté depuis UxPlay (GPL-3.0) — le binaire final est GPL-3.0.
