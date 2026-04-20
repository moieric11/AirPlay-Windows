# Notes protocole AirPlay 2 (mirroring)

> Notes de travail consolidées à partir d'**UxPlay** (référence primaire,
> maintenu), de RPiPlay (ancêtre), d'OpenAirplay et de shairport-sync. Rien
> ici ne vient d'une documentation officielle Apple.
>
> **Pourquoi UxPlay plutôt que RPiPlay** : RPiPlay n'a plus de commit de fond
> depuis 2021. UxPlay en est le fork direct — mêmes fichiers `lib/pairing.c`,
> `lib/fairplay.c`, `lib/raop.c`, `plist/info.plist` — mais avec les
> correctifs iOS 17/18 (champs `osBuildVersion`, `sourceVersion`, ajustements
> de `pair-verify`, modèles acceptés élargis). On porte toujours depuis
> UxPlay ; RPiPlay n'est consulté qu'en cas de divergence inexpliquée.

## 1. Découverte (mDNS / DNS-SD)

Deux services sont annoncés conjointement pour qu'iOS propose le mirroring :

### `_airplay._tcp` — service principal

| Clé TXT   | Exemple                  | Rôle                                                   |
|-----------|--------------------------|--------------------------------------------------------|
| deviceid  | `AA:BB:CC:DD:EE:FF`      | MAC-like, doit matcher celui de `_raop`                |
| features  | `0x5A7FFFF7,0x1E`        | Bitmask (deux mots de 32 bits) — voir § 1.3            |
| flags     | `0x4`                    | `0x4` = password non requis                            |
| model     | `AppleTV3,2`             | iOS gate certaines features selon le modèle déclaré    |
| pi        | UUID stable              | Public identifier, stable entre sessions               |
| srcvers   | `220.68`                 | Version AirPlay (doit être ≥ 220 pour mirroring)       |
| vv        | `2`                      | Version vidéo                                          |
| pk        | 64 hex                   | Clé publique Ed25519 (à ajouter après pair-setup)      |

### `_raop._tcp` — audio (requis même pour mirroring pur)

Nom de service : `<12-hex-de-deviceid>@<DeviceName>` (ex. `AABBCCDDEEFF@AirPlay-Windows`).

Clés TXT principales : `cn` (codecs), `et` (encryption), `ft` (features bis),
`am` (modèle), `tp=UDP`, `vn=65537`, `vs=220.68`, `vv=2`, `pi`.

### 1.3 Feature bitmask

Le champ `features` est une bitmask de 64 bits exprimée en deux mots hex
séparés par une virgule. Bits critiques :

| Bit | Nom                  | Signification                             |
|-----|----------------------|-------------------------------------------|
|  0  | Video                | Stream vidéo AirPlay classique            |
|  1  | Photo                | Diaporama photo                           |
|  2  | VideoFairPlay        | Vidéo chiffrée FairPlay                   |
|  7  | Audio                | Audio RAOP                                |
|  9  | AudioRedundant       | Audio avec paquets redondants             |
| 14  | MFiSoftAirPlay       | Obligatoire pour AirPlay 2                |
| 19  | ScreenMirroring      | **Requis pour la recopie d'écran**        |
| 20  | ScreenRotate         | Rotation                                  |
| 22  | AudioFormats         | Extension audio format                    |

Valeur utilisée ici (`0x5A7FFFF7,0x1E`) reprise d'UxPlay — connue pour passer
la phase de découverte sur iOS 13–18.

## 2. Handshake (RTSP-like sur TCP 7000)

iOS ouvre une session texte sur le port déclaré par `_airplay._tcp`. Le format
est proche d'HTTP/1.1 mais la ligne de statut peut être soit `HTTP/1.1` soit
`RTSP/1.0` selon la méthode.

Séquence observée (simplifiée) :

```
1. GET  /info                     — description du récepteur (plist binaire)
2. POST /pair-setup                — SRP-6a étape 1..3
3. POST /pair-verify               — Curve25519 ECDH (dérive clés session)
4. POST /fp-setup                  — FairPlay SAP v1 (4 messages, décrypteur playfair)
5. OPTIONS                         — annonce méthodes
6. ANNOUNCE                        — SDP côté source (AES keys chiffrées avec clé FairPlay)
7. SETUP rtsp://.../stream         — négocie ports RTP (audio, vidéo, event, timing)
8. RECORD                          — démarre le flux
9. GET_PARAMETER / SET_PARAMETER   — ajustements volume, track info
10. TEARDOWN                       — fermeture
```

### Points de friction connus

- **`/info`** doit répondre en **binary plist**, pas en texte libre. Le fichier
  `plist/info.plist` d'UxPlay (version iOS 17-safe) est la référence. Stub
  actuel : texte brut — iOS est tolérant jusqu'au pair-setup.
- **`pair-setup`** : SRP-6a avec paramètres 3072-bit standards. La pair
  `(username, password)` est hardcodée (`Pair-Setup` / `3939`) dans UxPlay.
- **`fp-setup`** : 4 messages. Les deux premiers sont des challenges/réponses
  utilisant la table statique `playfair_table` (16 Ko) déchiffrable via l'algo
  AES-CTR dérivé dans le code de `playfair`. Sans cette table, impossible de
  passer l'étape.
- **Chiffrement des streams** : les clés AES sont transportées dans l'entête
  `RSAAESKey` de l'ANNOUNCE, chiffrées en RSA 2048-bit avec la clé publique
  FairPlay. UxPlay embarque la clé privée (c'est la raison principale de la
  licence GPL du projet).

## 3. Flux média

- **Vidéo** : H.264 en TCP sur un port négocié. Les NALUs sont préfixés par
  un header 128-bit (type, longueur, timestamp). AES-128-CTR, IV dérivé du
  timestamp.
- **Audio mirroring** : AAC-ELD @ 44.1 kHz, RTP sur UDP.
- **Timing** : RTCP-like NTP sur UDP, côté récepteur = répondeur.
- **Events** : canal bidirectionnel pour pause, statut réseau, etc.

## 4. État actuel de ce projet

| Composant              | État    | Fichier principal                 |
|------------------------|---------|-----------------------------------|
| mDNS `_airplay._tcp`   | OK      | `src/mdns/bonjour_service.cpp`    |
| mDNS `_raop._tcp`      | OK      | idem                              |
| Serveur TCP 7000       | OK      | `src/net/tcp_server.cpp`          |
| Parser RTSP-like       | OK      | `src/airplay/rtsp_parser.cpp`     |
| Dispatcher             | OK      | `src/airplay/routes.cpp`          |
| `GET /info` (plist)    | Stub    | à porter depuis UxPlay            |
| `pair-setup` (SRP)     | Stub    | porter `lib/pairing.c` d'UxPlay   |
| `pair-verify` (X25519) | Stub    | idem                              |
| `fp-setup` (FairPlay)  | Stub    | nécessite playfair + clé privée   |
| SETUP / RECORD         | Stub    | —                                 |
| Décrypt H.264          | Absent  | —                                 |
| Rendu                  | Absent  | FFmpeg + SDL2 à intégrer          |

## 5. Prochaine étape recommandée

Intégrer le plist binaire de `/info` (1 journée) puis porter `pair-setup` /
`pair-verify` d'UxPlay (2–3 journées) — c'est le premier jalon qui permet
d'observer un échange complet jusqu'à FairPlay dans les logs.
