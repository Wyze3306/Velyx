# Architecture

Ce document explique comment Velyx est structuré et **pourquoi**. Les décisions
qui ont l'air arbitraires ont presque toutes une raison liée à la nature de
Bedrock : une application MSIX, en D3D12, qui change d'adresses toutes les six
semaines.

---

## Vue d'ensemble

```
                    ┌─────────────────────────────┐
   Velyx.exe ──────▶│  Minecraft.Windows.exe      │
   (launcher)       │                             │
   crée l'instance  │   ┌─────────────────────┐   │
   injecte la DLL   │   │      Velyx.dll      │   │
                    │   └─────────────────────┘   │
                    └─────────────────────────────┘
```

Le launcher et la DLL partagent `velyx_core` (journalisation, chemins, couleurs,
chaînes, processus). Rien dans `core` ne connaît le jeu : c'est ce qui permet au
launcher de le lier sans traîner tout le client.

---

## Démarrage de la DLL

`DllMain` ne fait qu'une chose : créer un thread. Tout le reste — création d'un
device D3D, scan mémoire, accès disque — se ferait sous le *loader lock* et
figerait le jeu.

```
DllMain
  └─ thread → Velyx::start()
       ├─ Paths::ensureLayout()          arborescence %APPDATA%/Velyx
       ├─ Log::init()                    fichier + console optionnelle
       ├─ ClientConfig::load()           + détection de plantage précédent
       ├─ sdk::bindGame()                déclare les signatures nécessaires
       ├─ Signatures::resolveAll()       scan (ou cache disque)
       ├─ ThemeManager::load()
       ├─ bindServices()                 CPS, frametimes, stats, vie privée
       ├─ ModuleManager::initialize()    construit le catalogue
       ├─ ProfileManager::load()         + applique le profil actif
       └─ HookManager::installAll()      SwapChain + fenêtre
```

À partir de là, le client vit **sur le thread de rendu du jeu**, dans le détour
de `Present`.

---

## La boucle de frame

```
Present (détour)
  └─ Velyx::onPresent
       ├─ GraphicsContext::attach()      idempotent
       ├─ WindowHook::attach()           première frame seulement
       ├─ calcul du delta, lissage du FPS
       ├─ emit FrameEvent                → SDK, services, animations
       ├─ GraphicsContext::beginFrame()  acquiert le back buffer
       │    ├─ emit RenderEvent          → éléments de HUD
       │    └─ emit RenderTopEvent       → menu, notifications, palette
       └─ GraphicsContext::endFrame()    rend le buffer et flush
```

Le découpage `RenderEvent` / `RenderTopEvent` n'est pas cosmétique : les
notifications et le menu doivent passer **au-dessus** du HUD, et rien d'autre ne
garantit cet ordre.

---

## L'overlay : pourquoi D3D11On12

Bedrock rend en D3D12. Direct2D ne sait pas dessiner sur une ressource D3D12.
La chaîne est donc :

```
ID3D12Resource (back buffer)
      │  D3D11On12Device::CreateWrappedResource
      ▼
ID3D11Resource ──QueryInterface──▶ IDXGISurface
      │  ID2D1DeviceContext::CreateBitmapFromDxgiSurface
      ▼
ID2D1Bitmap1  ← la cible de rendu de Velyx
```

Deux détails coûtent cher si on les rate :

- **la file de commandes doit être celle du jeu**. D3D11On12 en exige une ;
  en créer une nous-mêmes provoque un blocage au présent. D'où le hook sur
  `ID3D12CommandQueue::ExecuteCommandLists`, qui est le seul endroit où le jeu
  nous montre la sienne.
- **les ressources enveloppées doivent être relâchées avant `ResizeBuffers`**,
  sinon DXGI refuse le redimensionnement. Le détour de `ResizeBuffers` appelle
  `releaseTargets()` avant de passer la main.

Une ressource est acquise pour la durée de la passe overlay et rendue
immédiatement après : les listes de commandes du jeu ne voient rien.

Le chemin D3D11 pur existe aussi et sert au cas où la fenêtre soit un swapchain
D3D11 classique.

---

## Adresses du jeu

Rien n'est codé en dur. `Signatures` est un registre : les fonctionnalités
déclarent ce dont elles ont besoin (`require("Actor::position")`), les motifs
viennent d'un JSON, et le résultat est mis en cache sur disque, indexé par
`version du jeu + empreinte du pack`. Un scan complet du `.text` prend quelques
millisecondes grâce à l'ancrage `memchr` sur le premier octet concret ; les
lancements suivants ne scannent pas du tout.

L'échec est *toujours* local :

- signature non requise absente → sa fonctionnalité se tait ;
- signature requise absente → mode dégradé, message clair, le client démarre ;
- pointeur invalide au runtime → `memory::readable()` renvoie une valeur par
  défaut plutôt que de déréférencer.

`sdk::Game` est la seule façade qui lit le jeu. Un module ne fait jamais de
lecture mémoire : il lit `game().player()` ou `game().world()`, rafraîchis une
fois par frame.

---

## Événements

`EventBus` est typé, synchrone, trié par priorité, et supporte l'annulation.

Deux propriétés méritent d'être connues :

1. **les handlers tournent sans le verrou**. Un module peut réagir en
   s'abonnant, en se désabonnant, ou en émettant un autre événement ; les
   mutations sont mises en file et appliquées à la fin de l'émission.
2. **un module désactivé ne coûte rien**. `Module::on()` enregistre une
   *fabrique* d'abonnement, pas l'abonnement lui-même. Il est créé à
   l'activation et détruit à la désactivation. C'est ce qui rend un catalogue de
   150 modules viable.

---

## Modules

```
Module                      base : identité, réglages, keybind, permissions
 ├─ HudModule               + placement, ancrage, rotation, opacité, groupe
 │   └─ TextHud             + lignes libellé/valeur, alignement, mesure
 │       ├─ FpsHud
 │       ├─ CpsHud
 │       └─ …
 ├─ Zoom, FreeLook, …       modules de déplacement
 └─ ClickGui, HudEditor, …  surfaces du client (essentielles)
```

`HudModule` implémente le placement une fois pour toutes : une sous-classe
répond à « quelle taille fais-tu ? » et « dessine-toi ici », et hérite du reste.
`TextHud` va plus loin pour le cas le plus fréquent. C'est la raison pour
laquelle ajouter un compteur au HUD coûte vingt lignes et que tous s'alignent
au pixel près quand on les empile.

Les réglages sont des `std::variant` décrits (libellé, plage, unité, condition
de visibilité, mots-clés). Cette description unique alimente **à la fois**
l'affichage dans le menu, la sérialisation JSON, la recherche et la palette de
commandes — il n'y a pas de liste à tenir à jour ailleurs.

---

## Profils

Un profil contient l'état de tous les modules, leurs réglages, la disposition du
HUD et le thème. Changer de profil désactive tout, puis recharge — aucun état ne
fuit d'un profil à l'autre.

- **Auto Profile Switch** : chaque profil déclare des sous-chaînes à comparer à
  l'adresse et au nom du serveur rejoint ; la correspondance la plus longue
  gagne, sinon le profil par défaut s'applique.
- **Versionnage** : chaque bascule, import ou réinitialisation écrit d'abord un
  point de restauration dans `profiles/<nom>/versions/`. Vingt sont conservés.
- **Partage** : `VELYX1:<base64 du JSON>`, une seule ligne collable dans un chat.

---

## Interface

`Ui` est une couche *immediate mode* : le menu est reconstruit à chaque frame.
Il n'y a donc pas d'arbre de widgets à garder synchronisé avec la liste des
modules — un module ajouté par un plugin apparaît sans inscription.

Ce qui est conservé entre les frames est uniquement l'état d'interaction :
quel widget est survolé, lequel est en cours de glissement, où en est chaque
zone de défilement, et une valeur animée par widget. C'est cette dernière qui
fait que l'interface *bouge* au lieu d'être simplement redessinée.

Les identifiants de widget sont des hachages `nom + index`, ce qui permet à une
ligne dans une boucle de garder son survol d'une frame à l'autre.

---

## Thèmes

`Theme` est une structure sérialisable qui contient **toutes** les décisions
visuelles : treize couleurs, quatre valeurs de forme, la typographie, les effets
et la vitesse d'animation. Aucun code de rendu n'invente une couleur ou un
arrondi ; tout passe par `theme()`.

C'est ce qui fait du créateur de thèmes une vraie fonctionnalité plutôt qu'un
sélecteur de couleur d'accent : un thème peut supprimer le flou, arrondir
autrement, agrandir le texte et désactiver les animations — ce dernier point
étant exactement ce dont le mode accessibilité a besoin.

---

## Le launcher

Voir le README pour le principe. Côté code :

- `InstanceManager` — découverte du jeu, clonage par liens durs, réécriture du
  manifeste, enregistrement, activation, injection ;
- `AccountStore` — étiquettes de comptes et liaison à une instance. Aucun jeton,
  aucun identifiant, jamais ;
- `main.cpp` — fenêtre Win32 + `ID2D1HwndRenderTarget`, sans dépendance à
  installer.

Les deux seules opérations qui passent par PowerShell sont `Get-AppxPackage` et
`Add-AppxPackage -Register` : il n'existe pas d'équivalent C++ raisonnable sans
tirer tout WinRT dans le binaire.

---

## Conventions

- C++23, `namespace velyx`, `PascalCase` pour les types, `camelCase` pour les
  fonctions et variables, `membre_` pour les champs privés.
- Un fichier d'en-tête explique *pourquoi* la classe existe ; les commentaires
  en ligne expliquent les décisions non évidentes, pas ce que le code dit déjà.
- Les chaînes visibles par l'utilisateur sont en français, celles des journaux
  aussi. Les identifiants de code, les clés JSON et les noms de signatures sont
  en anglais et stables — ce sont des données, pas de l'affichage.
