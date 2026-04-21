# FairPlay SAP — provisioning

Ce document décrit comment remplacer le stub `third_party/fairplay_blobs_stub.cpp`
par les données FairPlay nécessaires au handshake `POST /fp-setup`. Sans ces
données iOS refuse de passer à `ANNOUNCE` / `SETUP`.

## 1. Ce que UxPlay fait vraiment (et ce qu'on fait aussi)

FairPlay SAP se décompose en **deux sous-étapes bien distinctes** :

| Étape | Rôle | Approche UxPlay |
|-------|------|-----------------|
| **setup + handshake** (`/fp-setup` msg1↔msg2, msg3↔msg4) | Dialogue d'authentification DRM | **Replay** : 4 réponses msg2 pré-enregistrées + header fixe pour msg4. Aucune crypto côté serveur. |
| **decrypt** (au moment de `ANNOUNCE`) | Transformer le `rsaaeskey` (72 bytes) en clé AES-128 qui déchiffre le stream | **Algorithme obfusqué** : `lib/playfair/` + table `omg_hax.h` (~500 Ko) |

Pour "setup + handshake" il suffit donc de ~600 bytes de données, pas 16 Ko.
C'est ce que le niveau A provisionne ici. Le niveau B (decrypt) est une étape
distincte pour plus tard.

## 2. Niveau A — handshake (suffisant pour atteindre ANNOUNCE/SETUP/RECORD)

### Étape 1 — cloner UxPlay à côté (pas dans ce repo)

```powershell
git clone https://github.com/FDH2/UxPlay.git ../UxPlay-reference
```

### Étape 2 — repérer les données

Le fichier qui compte est `lib/fairplay_playfair.c`. Il contient :

- `char reply_message[4][142]` — les 4 réponses msg2 (indexées par msg1[14])
- `char fp_header[]` — 12 bytes utilisés dans msg4

### Étape 3 — créer `third_party/fairplay_blobs_real.cpp`

Structure :

```cpp
// GPL-3.0 — dérivé de UxPlay/lib/fairplay_playfair.c
#include "crypto/fairplay_blobs.h"

namespace ap::crypto::fairplay_blobs {

const bool kPresent = true;

const unsigned char kReplyMessage[kReplyCount][kReplySize] = {
    // Mode 0 : reply_message[0] copié depuis UxPlay
    { 0x46,0x50,0x4c,0x59, /* 142 bytes au total */ },
    // Mode 1 : reply_message[1]
    { 0x46,0x50,0x4c,0x59, /* … */ },
    // Mode 2
    { 0x46,0x50,0x4c,0x59, /* … */ },
    // Mode 3
    { 0x46,0x50,0x4c,0x59, /* … */ },
};

const unsigned char kFpHeader[kFpHeaderSize] = {
    // fp_header[] depuis UxPlay
    0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x14,
};

} // namespace
```

### Étape 4 — reconfigurer et rebuild

```powershell
rmdir /s /q build
cmake -S . -B build <toolchain flags>
cmake --build build --config Debug
```

À la configuration tu dois voir :

```
-- FairPlay: using provisioned blob (fairplay_blobs_real.cpp)
```

### Étape 5 — vérifier

Au runtime, les logs fp-setup doivent passer de `STUB msg2` à :

```
fp-setup msg1 (mode N) → 142B replay
fp-setup msg3 → 32B response (fp_header + msg3[144..164])
```

Et iPhone doit enchaîner sur `ANNOUNCE` / `SETUP` / `RECORD`.

## 3. Niveau B — decrypt (stream déchiffré, affichage)

Pour décoder la vidéo, il faut aller plus loin : transformer le 72-byte
`rsaaeskey` en une clé AES-128 de 16 bytes qui déchiffrera le H.264 qui arrive
sur les sockets UDP.

Cette étape appelle `playfair_decrypt(msg3, rsaaeskey, key_out)` qui vit dans
`UxPlay/lib/playfair/playfair.c` et dépend de :

| Fichier | Taille | Contenu |
|---------|--------|---------|
| `hand_garble.c` | 20 Ko | Cycle AES-like + XOR |
| `omg_hax.c` | 22 Ko | Key schedule + session key |
| `omg_hax.h` | **483 Ko** | **Table `default_sap[]` Apple** |
| `modified_md5.c` | 3.5 Ko | Hash dérivé MD5 |
| `sap_hash.c` | 3.5 Ko | Autre hash interne |
| `playfair.c` / `.h` | 1.2 Ko | Façade |
| `LICENSE.md` | 35 Ko | **Licence à lire avant de redistribuer** |

Pour intégrer ce niveau plus tard :

1. Copier tout `lib/playfair/` dans `third_party/playfair/`
2. Lire `LICENSE.md` (licence dérivée BSD-ish mais avec restrictions Apple)
3. Ajouter `third_party/playfair/*.c` au `target_sources` du CMake quand le
   blob réel est détecté
4. Ajouter dans `fairplay.h` une méthode `decrypt(rsaaeskey, out)` qui appelle
   `playfair_decrypt(keymsg_.data(), rsaaeskey, out)`
5. L'utiliser dans `handle_announce` pour déchiffrer le `rsaaeskey` du SDP

## 4. Licence

Dès que `fairplay_blobs_real.cpp` (ou `lib/playfair/*`) est linké, **l'exécutable
devient GPL-3.0 par dérivation d'UxPlay**. Mettre à jour `README.md` et
`LICENSE` en conséquence.

## 5. Garde-fous

- **`third_party/fairplay_blobs_real.cpp` est gitignored** — ne jamais le
  commit dans ce repo.
- Le stub (`fairplay_blobs_stub.cpp`) reste compilable seul et garde la forme
  RTSP correcte ; les tests unitaires passent dans les deux configurations.
- Diagnostic runtime : la ligne de log `STUB msg2` vs `142B replay` au premier
  `/fp-setup` indique quelle variante est linkée.
