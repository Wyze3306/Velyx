<div align="center">

# Velyx

**Client utilitaire pour Minecraft Bedrock Edition**

Une DLL injectée dans le jeu, plus un launcher capable de faire tourner plusieurs
instances en parallèle, chacune sur son propre compte Microsoft.

Vert forêt, vert menthe, blanc. Un seul menu. Pas de triche.

[![build](https://github.com/Wyze3306/Velyx/actions/workflows/build.yml/badge.svg)](https://github.com/Wyze3306/Velyx/actions/workflows/build.yml)
[![licence](https://img.shields.io/badge/licence-GPL--3.0-3DDC84)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-0B1F17)](CMakeLists.txt)

</div>

---

```
Velyx/
├── src/core/       code partagé DLL ↔ launcher (log, chemins, couleurs, process)
├── src/dll/        le client injecté
├── src/launcher/   Velyx.exe : instances, comptes, injection
├── assets/         polices, thèmes, packs de signatures
└── docs/           architecture, feuille de route
```

## Compiler

Velyx est un binaire Windows x64, mais il se compile **aussi bien depuis Linux**
que depuis Windows. Les deux chemins sont vérifiés par l'intégration continue.

**Depuis Linux (mingw-w64)**

```bash
sudo apt install mingw-w64 cmake ninja-build
./build.sh
```

**Depuis Windows (Visual Studio 2022)**

```bat
build.bat
```

| Sortie | Rôle |
| --- | --- |
| `Velyx.dll` | le client, à injecter dans `Minecraft.Windows.exe` |
| `Velyx.exe` | le launcher |
| `assets/` | polices, thèmes, packs de signatures |

Aucune dépendance à télécharger : MinHook et nlohmann/json sont dans `external/`.

---

## Ce qui tourne

### Socle

| Brique | |
| --- | --- |
| Build multi-plateforme (MSVC + mingw), CI sur les deux | ✅ |
| Scanner de signatures avec cache disque et packs JSON | ✅ |
| Gestionnaire de hooks (MinHook), installation par lot | ✅ |
| Hook DXGI : `Present`, `Present1`, `ResizeBuffers`, `ExecuteCommandLists` | ✅ |
| Overlay Direct2D via D3D11On12 (+ chemin D3D11 direct) | ✅ |
| Bus d'événements typé, priorités, annulation, souscriptions différées | ✅ |
| Renderer 2D : arrondis, dégradés, ombres, flou, matrices de couleur, rotation | ✅ |
| DirectWrite, cache LRU de layouts, polices embarquées | ✅ |
| Capture clavier/souris par sous-classement de fenêtre | ✅ |
| Modules : réglages typés, keybinds, permissions, favoris, recherche floue | ✅ |
| Profils : bascule, versionnage, import/export par code | ✅ |
| Auto Profile Switch selon le serveur rejoint | ✅ |
| Moteur de thèmes + 4 thèmes intégrés | ✅ |
| Mode sans échec, détection de plantage, rapport de crash nominatif | ✅ |
| Capture d'écran PNG, galerie rangée par serveur et par date | ✅ |
| Suivi du temps de jeu, historique des parties | ✅ |

### Interface

Menu principal (catégories, liste, réglages, recherche) · Éditeur de HUD (grille,
aimantation, guides, groupes, flèches) · Palette de commandes `Ctrl+K` · Centre de
notifications avec historique · Créateur de thèmes en direct · Gestionnaire de
profils avec partage par code · Gestionnaire de raccourcis · **Historique**
(graphique du temps de jeu + parties) · **Diagnostic** (santé des signatures +
dernier plantage).

Toute la bibliothèque de widgets — interrupteurs, curseurs, listes déroulantes,
champs texte, sélecteur TSV + alpha, capture de touche, zones défilantes — est
écrite pour Velyx, animée, et pilotée par le thème.

### 42 modules

**Déplacement (11)** — Zoom · FOV personnalisé · FOV dynamique Java ·
Multiplicateur de sensibilité · Sprint automatique · Accroupissement à bascule ·
FreeLook · SnapLook · Caméra cinématique · Perspective automatique · Null Movement

**HUD (17)** — FPS · CPS · Horloge · Coordonnées · Direction · Vitesse · Ping ·
Mémoire · Adresse du serveur · Minuteur AFK · Stats de session · Chronomètre ·
Keystrokes · Graphique FPS · Moniteur serveur · Armure · Temps de jeu

**Confort et vie privée (8)** — Mode confidentialité · Mode streamer ·
Mode performance · Mode batterie · Mode accessibilité · Mode capture ·
Benchmark · Filtres d'écran

**Rendu (2)** — Couleur de coup · Teinte de dégâts

**Client (4)** — Menu · Éditeur de HUD · Palette de commandes · Notifications

Une classe de base `TextHud` fait qu'un nouvel élément de HUD tient en une
vingtaine de lignes : il déclare son contenu, et hérite du placement libre, de la
rotation, de l'opacité, du fond thémé, de l'aimantation et du mode capture.

`docs/ROADMAP.md` reprend la liste de fonctionnalités ligne par ligne avec son
état et l'endroit exact où le code doit atterrir.

---

## Le point important : les signatures

Velyx ne contient **aucune adresse mémoire de Minecraft**, et c'est un choix
d'architecture.

Tout ce que le client lit dans le jeu (position, vie, ping, adresse du serveur…)
passe par un nom symbolique résolu au démarrage depuis
`assets/signatures/<version>.json`. Conséquences :

- une mise à jour de Bedrock ne demande **aucune recompilation**, juste un
  nouveau fichier JSON ;
- une signature manquante désactive proprement la fonctionnalité concernée au
  lieu de faire planter le jeu ;
- la page **Diagnostic** liste ce qui est résolu et ce qui ne l'est pas.

**Sans pack de signatures**, Velyx démarre et reste pleinement utilisable pour
tout ce qui ne dépend pas du jeu : menu, thèmes, profils, éditeur de HUD, FPS,
CPS, horloge, keystrokes, mémoire, graphique de performance, chronomètre,
captures d'écran, filtres, temps de jeu, benchmark. Les modules qui lisent l'état
du jeu affichent `--`.

Voir `assets/signatures/README.md` pour écrire et valider un pack.

---

## Le launcher : plusieurs jeux, plusieurs comptes

Bedrock est une application MSIX : Windows refuse d'en lancer deux copies. Le
launcher contourne cela de la seule manière propre qui existe — il crée de
**vraies identités de paquet distinctes** :

1. il localise le jeu installé (`Get-AppxPackage Microsoft.MinecraftUWP`) ;
2. il en fait une copie par **liens durs NTFS** — quasi instantané, quasi zéro
   octet supplémentaire ;
3. il réécrit `Identity/Name` et le nom affiché dans `AppxManifest.xml` ;
4. il enregistre le tout avec `Add-AppxPackage -Register`.

Windows voit alors une application différente, lui donne **son propre conteneur
de données** — et donc **sa propre connexion Xbox**. C'est ce qui fait qu'un
compte par instance fonctionne, sans jamais toucher à un identifiant.

Le lancement passe par `IApplicationActivationManager::ActivateApplication`, qui
rend le PID dont l'injecteur a besoin. La DLL reçoit au passage l'ACE
`ALL APPLICATION PACKAGES` sans laquelle `LoadLibraryW` échoue dans un
AppContainer.

**Velyx ne stocke aucun identifiant, aucun jeton, aucun mot de passe.** Un
« compte » dans le launcher est une étiquette que vous attachez à une instance.
La connexion se fait dans le jeu, comme d'habitude, et y reste.

**Prérequis** : le mode développeur de Windows doit être activé
(*Paramètres → Confidentialité et sécurité → Espace développeurs*). Le launcher
le vérifie et le dit avant que vous tentiez quoi que ce soit.

---

## Position sur la triche

Velyx est un client **utilitaire** : HUD, confort, performance, organisation.

Pas de killaura, pas de fly, pas de reach, pas d'ESP à travers les murs, pas
d'automatisation du gameplay. Les modules d'entraînement chronomètrent et
comptent, ils ne jouent pas à votre place. Les couleurs de dégâts et les nametags
sont purement visuels et côté client.

Cette ligne est tenue dans le code : chaque module déclare ses permissions
(`réseau`, `fichiers`, `entrées simulées`, `mémoire du jeu`), et le menu les
affiche **avant** que vous ne l'activiez.

---

## Licence et provenance

Velyx est sous **GPL-3.0** (voir [LICENSE](LICENSE)).

Le code est original. **Aucune ligne de Flarial n'a été copiée** — Flarial est
sous AGPL-3.0. Le dépôt `flarialmc/dll-oss` a servi de référence pour comprendre
*quelles* fonctions du jeu un client Bedrock doit atteindre ; l'architecture, le
rendu, l'interface et les modules sont écrits pour Velyx.

Dépendances embarquées : MinHook (BSD-2-Clause), nlohmann/json (MIT).

Minecraft est une marque de Mojang AB. Velyx n'est ni affilié ni approuvé par
Mojang ou Microsoft.

---

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — comment les briques s'emboîtent, et pourquoi
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — la liste de fonctionnalités, ligne par ligne, avec son état
- [`assets/signatures/README.md`](assets/signatures/README.md) — écrire et valider un pack de signatures
