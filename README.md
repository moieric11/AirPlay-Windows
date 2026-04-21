# AirPlay Windows Receiver

Récepteur AirPlay 2 (mirroring) pour Windows, porté depuis
[UxPlay](https://github.com/FDH2/UxPlay) (GPL-3.0) avec un stack 100 %
natif Windows (pas de Bonjour SDK ni de dépendance Apple au runtime).

> **État actuel — validé contre un iPhone 16 (iOS 18) en production.**
>
> Un iPhone du même réseau Wi-Fi voit `AirPlay-Windows` dans son Centre
> de contrôle, complète tout le handshake de sécurité, et pousse
> effectivement son flux H.264 chiffré vers le PC. Le récepteur reçoit
> ~3 Mo/session, parse chaque frame (SPS_PPS, IDR, NON_IDR, stream
> reports) et **identifie correctement le codec et la résolution** en
> temps réel :
>
> ```
> mirror video: H.264 High level 3.1, 498x1080
> ```
>
> Ce qui reste pour afficher la vidéo à l'écran est documenté plus bas.

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
| FairPlay decrypt (ekey → AES key)           | ❌   | `lib/playfair/` — cf. `docs/FAIRPLAY.md`     |
| Décryption AES-CTR des NAL H.264            | ❌   | en aval de FairPlay decrypt                  |
| Décodeur H.264 (FFmpeg)                     | ❌   | à intégrer                                   |
| Renderer (SDL2 / DirectX)                   | ❌   | à intégrer                                   |
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

1. **FairPlay decrypt niveau B** — porter `lib/playfair/` d'UxPlay
   (`hand_garble.c`, `omg_hax.c`, `modified_md5.c`, `sap_hash.c` +
   la table `omg_hax.h` de 483 Ko). Dès que `fairplay_decrypt(ekey)`
   renvoie une clé AES valide, toutes les frames chiffrées peuvent
   être déchiffrées en AES-CTR.
2. **H.264 decoder** via FFmpeg (vcpkg).
3. **Renderer** SDL2 ou DirectX 11.
4. **Audio RAOP** sur le stream type 96 (ALAC/AAC).

## Références

- **[UxPlay](https://github.com/FDH2/UxPlay)** — référence primaire, fork actif de RPiPlay, maintenu iOS 17/18 (GPL-3.0)
- [RPiPlay](https://github.com/FD-/RPiPlay) — ancêtre, non maintenu depuis 2021 (GPL-3.0)
- [OpenAirplay](https://openairplay.github.io/airplay-spec/) — notes de protocole
- [playfair](https://github.com/EstebanKubata/playfair) — décrypteur FairPlay
- [shairport-sync](https://github.com/mikebrady/shairport-sync) — pour l'audio RAOP

## Licence

Porté depuis UxPlay (GPL-3.0) — le binaire final est GPL-3.0.
