# AirPlay Windows Receiver

Récepteur AirPlay 2 (mirroring) pour Windows, basé sur le protocole rétro-ingénieré
par la communauté (RPiPlay, UxPlay, OpenAirplay).

> **Statut : squelette fonctionnel — itération 1.**
> Le projet s'annonce en mDNS comme un récepteur AirPlay, accepte les connexions
> TCP sur le port 7000 et loggue les requêtes RTSP-like. La partie cryptographique
> (FairPlay SAP, Curve25519) et le décodage vidéo ne sont pas encore implémentés.

## Pourquoi ce projet

Apple n'a jamais publié la spécification d'AirPlay 2 mirroring. Les seules
implémentations open-source fiables (RPiPlay, UxPlay) ciblent Linux/macOS.
Ce projet porte la même architecture sur Windows.

## Stratégie

Copier ce qui fonctionne déjà, pas réinventer :

1. **Découverte mDNS** — `_airplay._tcp` et `_raop._tcp` via Bonjour SDK (Apple officiel pour Windows)
2. **Serveur RTSP-like** — mêmes routes que RPiPlay (`GET /info`, `POST /pair-setup`, `POST /fp-setup`, `SETUP`, `RECORD`, `TEARDOWN`)
3. **Pairing / FairPlay** — portage direct des clés SRP + playfair de RPiPlay
4. **Streams** — H.264 sur TCP (mirroring), ALAC/AAC sur UDP (audio)
5. **Rendu** — FFmpeg + SDL2 (aligné sur UxPlay)

Chaque étape est validée avant de passer à la suivante.

## Prérequis (Windows)

- **Visual Studio 2022** avec composants C++ (ou Build Tools équivalents)
- **CMake 3.20+**
- **vcpkg** (pour OpenSSL, plog)
- **Bonjour SDK for Windows** (DNS-SD) — <https://developer.apple.com/bonjour/>
  Fournit `dns_sd.h` et `dnssd.lib`. Installé aussi avec iTunes.
- Le service **Bonjour Service** (mDNSResponder) doit tourner.

## Build

```bat
git clone https://github.com/moieric11/airplay-windows.git
cd airplay-windows
cmake --preset default
cmake --build --preset default
```

Ajuster `BONJOUR_SDK_HOME` si le SDK n'est pas dans `C:\Program Files\Bonjour SDK`.

## Lancement

```bat
build\airplay-windows.exe
```

Sur un iPhone / iPad / Mac du même réseau, ouvrir le Centre de contrôle
→ Recopie d'écran : le récepteur `AirPlay-Windows` devrait apparaître.
La connexion échouera au handshake FairPlay (non implémenté) mais la découverte
et les premières requêtes RTSP doivent être visibles dans les logs.

## Feuille de route

- [x] Structure CMake, dépendances, logger
- [x] Serveur TCP (port 7000) bloquant mono-thread
- [x] Annonce mDNS `_airplay._tcp` via Bonjour
- [x] Parser RTSP-like (requête / méthode / en-têtes / body)
- [x] Dispatcher vers des handlers stubbés
- [ ] `GET /info` — renvoyer le plist `deviceinfo` correctement (champ `features`, `pi`, `vv`, …)
- [ ] `POST /pair-setup` et `POST /pair-verify` — SRP-6a + Curve25519 (port depuis RPiPlay `lib/pairing.c`)
- [ ] `POST /fp-setup` — FairPlay SAP v1 (port depuis RPiPlay `lib/fairplay.c` + table `playfair`)
- [ ] `SETUP` / `RECORD` — ouverture des streams RTP
- [ ] Décrypt H.264 (AES-CTR avec clé dérivée du handshake)
- [ ] Décodage H.264 via FFmpeg
- [ ] Rendu via SDL2
- [ ] Audio (ALAC/AAC, RTCP feedback)
- [ ] Gestion multi-clients, TEARDOWN propre

## Références

- [RPiPlay](https://github.com/FD-/RPiPlay) — implémentation de référence (GPL-3.0)
- [UxPlay](https://github.com/FDH2/UxPlay) — fork maintenu, desktop
- [OpenAirplay](https://openairplay.github.io/airplay-spec/) — specification
- [playfair](https://github.com/EstebanKubata/playfair) — décrypteur FairPlay
- [shairport-sync](https://github.com/mikebrady/shairport-sync) — pour l'audio RAOP

## Licence

Projet de recherche. Le portage du code issu de RPiPlay sera GPL-3.0.
