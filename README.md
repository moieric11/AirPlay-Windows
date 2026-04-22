# AirPlay Windows Receiver

Récepteur AirPlay 2 (mirroring) pour Windows, porté depuis
[UxPlay](https://github.com/FDH2/UxPlay) (GPL-3.0) avec un stack 100 %
natif Windows (pas de Bonjour SDK ni de dépendance Apple au runtime).

> **État actuel — flux H.264 déchiffré en direct depuis un iPhone 16 / iOS 18.**
>
> Un iPhone du même Wi-Fi voit `AirPlay-Windows` dans son Centre de
> contrôle, traverse l'intégralité du handshake (pair-verify + FairPlay
> + SETUP + RECORD), envoie son flux H.264 chiffré via TCP. Le récepteur
> récupère la clé AES de session (via FairPlay playfair + post-hash
> ECDH), déchiffre chaque frame à la volée, et **les NAL units qui en
> sortent sont conformes à la spec H.264** (forbidden_zero_bit=0, NAL
> types 1/5/6/7/8, longueurs exactes) :
>
> ```
> mirror video: H.264 High level 3.1, 498x1080
> mirror decrypted frame[2]: VIDEO_IDR    first NAL len=6575 type=5  ref_idc=1
> mirror decrypted frame[3]: VIDEO_NON_IDR first NAL len=406  type=1  ref_idc=1
> ```
>
> Tout le protocole et toute la cryptographie sont derrière nous. Ce
> qui reste est du travail applicatif pur (split NAL + décodeur +
> renderer), documenté plus bas.

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
| Split NAL + conversion Annex-B              | ❌   | `raop_rtp_mirror_thread` — cf. roadmap       |
| Décodeur H.264 (FFmpeg)                     | ❌   | à intégrer                                   |
| Renderer (SDL2 / Direct3D 11)               | ❌   | à intégrer                                   |
| Audio (RAOP, ALAC/AAC)                      | ❌   | `lib/raop.c`                                 |

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

Plus aucune étape n'implique de protocole AirPlay ou de cryptographie —
tout est à partir de H.264 en clair :

1. **Split NAL unités + conversion Annex-B** : chaque payload décrypté
   contient une ou plusieurs NAL units préfixées par leur longueur BE
   (format AVCC). Remplacer chaque length par le start-code
   `00 00 00 01` donne de l'Annex-B consommable par FFmpeg. Stocker
   les SPS/PPS clair (type 7/8) à part pour les prepender à chaque
   IDR à envoyer au décodeur.
2. **H.264 decoder** via FFmpeg (vcpkg → `libavcodec`). Alimenter
   `avcodec_send_packet` avec (SPS/PPS + IDR) au démarrage, puis
   les P-frames successifs via `avcodec_receive_frame`.
3. **Renderer** Direct3D 11 ou SDL2 pour afficher les `AVFrame`
   YUV → RGB.
4. **Audio RAOP** (stream type 96) sur le chemin UDP : AES-128-CBC
   (contrairement à CTR pour vidéo), décodage ALAC/AAC.

## Références

- **[UxPlay](https://github.com/FDH2/UxPlay)** — référence primaire, fork actif de RPiPlay, maintenu iOS 17/18 (GPL-3.0)
- [RPiPlay](https://github.com/FD-/RPiPlay) — ancêtre, non maintenu depuis 2021 (GPL-3.0)
- [OpenAirplay](https://openairplay.github.io/airplay-spec/) — notes de protocole
- [playfair](https://github.com/EstebanKubata/playfair) — décrypteur FairPlay
- [shairport-sync](https://github.com/mikebrady/shairport-sync) — pour l'audio RAOP

## Licence

Porté depuis UxPlay (GPL-3.0) — le binaire final est GPL-3.0.
