# FairPlay SAP — provisioning des blobs

Ce document décrit comment remplacer le stub `third_party/fairplay_blobs_stub.cpp`
par les blobs réels nécessaires à la phase `POST /fp-setup` du handshake
AirPlay. Sans ces blobs, iOS ira jusqu'à `pair-verify` mais refusera de
poursuivre vers `ANNOUNCE` / `SETUP`.

## 1. Contexte

FairPlay SAP v1 est la phase DRM propriétaire d'Apple. L'algorithme a été
rétro-ingénieré il y a ~8 ans (projet `playfair` d'EstebanKubata) et intégré
à RPiPlay puis UxPlay. Il dépend de **deux données binaires extraites du
framework `AirPlayReceiver`** d'Apple :

1. Une **table de lookup** de ~16 Ko utilisée pour construire le corps de
   `msg2` (réponse à `msg1`).
2. Une **clé AES-128** utilisée pour déchiffrer `msg3` et produire `msg4`.

Ces données sont copyrightées Apple. Aucune licence publique ne les autorise
formellement, mais elles sont embarquées de fait dans tous les récepteurs
AirPlay open-source (RPiPlay, UxPlay, shairport-sync avec mirroring, …)
depuis plusieurs années sans action juridique.

**Ce dépôt ne les inclut pas.** La compilation par défaut link contre un
stub rempli de zéros. L'utilisateur les provisionne lui-même.

## 2. Procédure

### Étape 1 — récupérer le source UxPlay

```bash
git clone https://github.com/FDH2/UxPlay.git uxplay-src
```

Le fichier pertinent est `uxplay-src/lib/fairplay_playfair.c`. Il contient
les trois blobs dont nous avons besoin, sous forme de tableaux `static const`.

### Étape 2 — créer `third_party/fairplay_blobs_real.cpp`

Créer un fichier `third_party/fairplay_blobs_real.cpp` dans notre dépôt,
avec la structure suivante :

```cpp
// GPL-3.0 — dérivé de UxPlay/lib/fairplay_playfair.c
// Données binaires Apple, usage d'interopérabilité uniquement.
#include "crypto/fairplay_blobs.h"

namespace ap::crypto::fairplay_blobs {

const bool kPresent = true;

const unsigned char kTable[kTableSize] = {
    // … copier ici le contenu de fplay_hex[] depuis UxPlay …
};

const unsigned char kAesKey[kAesKeySize] = {
    // … copier ici le contenu de fplay_aes_key[] depuis UxPlay …
};

const unsigned char kReplyHeaderMode1[kReplyHeaderSize] = {
    // … copier ici le header mode 1 depuis fairplay_reply_msg_header_mode_1[] …
};
const unsigned char kReplyHeaderMode2[kReplyHeaderSize] = {
    // … et le header mode 2 …
};

} // namespace ap::crypto::fairplay_blobs
```

### Étape 3 — rebuild

Le CMake détecte automatiquement `fairplay_blobs_real.cpp` s'il existe et
substitue le stub par celui-ci. Vérifier à la configuration :

```
-- FairPlay: using provisioned blob (fairplay_blobs_real.cpp)
```

### Étape 4 — port du calcul

Le stub court-circuite les deux fonctions `compute_msg2_stub` et
`compute_msg4_stub` dans `src/crypto/fairplay.cpp`. Une fois les blobs en
place, porter le **calcul réel** depuis UxPlay — deux fonctions d'environ
40 lignes chacune :

- `uxplay-src/lib/fairplay.c::fairplay_setup_msg2` → adapter en
  `real_compute_msg2(mode, msg1, out)` dans notre `fairplay.cpp`
- `uxplay-src/lib/fairplay.c::fairplay_setup_msg4` → `real_compute_msg4`

Basculer les deux branches `if (!fairplay_blobs::kPresent)` pour appeler
la version réelle quand le vrai blob est présent.

## 3. Licence

Dès que `third_party/fairplay_blobs_real.cpp` est en place, **l'exécutable
devient redistribuable uniquement en GPL-3.0** (contamination par UxPlay).
Mettre à jour `README.md` et `LICENSE` en conséquence.

## 4. Vérification

Après provisioning complet :

1. `cmake --build build` : pas d'erreur, log "using provisioned blob"
2. `python3 tools/test_pair_verify.py` : T5 passe toujours (forme RTSP)
3. Capture Wireshark d'une session iPhone : la paire `msg1/msg2` doit
   correspondre octet-pour-octet à ce que UxPlay produit avec la même
   entrée (tester avec le même iPhone successivement sur UxPlay Linux et
   notre récepteur Windows, comparer les dumps).

## 5. Garde-fous

- **Ne jamais commiter `fairplay_blobs_real.cpp`** dans le dépôt public.
  Ajouter `third_party/fairplay_blobs_real.cpp` à `.gitignore` dès la
  création du fichier.
- Si le dépôt devient public, réfléchir à la stratégie de distribution :
  UxPlay fournit les blobs dans son propre repo, on peut documenter un
  script qui les récupère chez eux à la demande.
