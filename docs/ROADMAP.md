# Feuille de route

Votre liste de fonctionnalités, reprise ligne par ligne, avec son état réel et
l'endroit où le code doit atterrir.

**Légende**

| | Signification |
| --- | --- |
| ✅ | Écrit, compilé, utilisable |
| 🟡 | Le socle existe ; il reste le module ou l'écran à écrire (généralement < 150 lignes) |
| ⬜ | À écrire entièrement |

---

## Configuration et profils

| Fonctionnalité | État | Où |
| --- | --- | --- |
| Profile Manager | ✅ | `dll/config/ProfileManager.*` + page Profils |
| Auto Profile Switch | ✅ | `ProfileManager::profileForServer`, plus longue correspondance |
| Config Sharing (code + fichier) | ✅ | `exportCode` / `importCode`, format `VELYX1:` |
| Config Versioning | ✅ | `snapshot` / `restore`, 20 points conservés |
| Settings Search | ✅ | `ModuleManager::search`, cherche modules **et** réglages |
| Favorites Modules | ✅ | `Module::setFavourite`, page Favoris |
| Keybind Manager | ✅ | Page Raccourcis, avec mode bascule/maintien/impulsion |
| Command Palette (Ctrl+K) | ✅ | `dll/ui/CommandPalette.*`, extensible via `registerCommand` |
| Module Permissions | ✅ | `ModulePermissions`, affichées avant activation |
| Safe Mode | ✅ | Deux plantages consécutifs → tout sauf l'essentiel désactivé |

## Apparence

| Fonctionnalité | État | Où |
| --- | --- | --- |
| HUD Editor avancé | ✅ | `dll/ui/HudEditor.*` — grille, aimantation, guides, groupes, flèches |
| Placement libre, rotation, transparence | ✅ | `HudModule`, réglages partagés par tous les éléments |
| Groupes d'éléments | ✅ | Réglage `group` ; déplacement solidaire dans l'éditeur |
| Theme Creator | ✅ | `dll/ui/Theme.*` + page Thèmes, édition en direct |
| Accessibility Mode | ✅ | Module `accessibility` : thème Contrast, échelle du texte, bordures épaisses, animations coupées |
| Screen Filters (nuit, contraste, saturation, daltonisme) | ✅ | Module `screen_filters`, matrices de couleur D2D (protanopie, deutéranopie, tritanopie) |
| Custom Sky | ⬜ | Nécessite un hook `SkyRenderer` (signature à fournir) |
| Crosshair Designer | ⬜ | Module `HudModule` ; tout le dessin nécessaire existe dans `Renderer` |
| Custom Hit Color | 🟡 | Module `custom_hit_color` écrit ; inerte tant que le hook `HurtColor` n'a pas de signature |
| Custom Damage Tint | 🟡 | Module `damage_tint` écrit ; même dépendance |

## Performance

| Fonctionnalité | État | Où |
| --- | --- | --- |
| FPS Graph (frametimes, freezes, 1 % lows) | ✅ | Module `fps_graph` + `FrameStats` |
| Session Stats | ✅ | Module `session_stats` + service `SessionStats` |
| Server Performance Monitor | 🟡 | Ping ✅. TPS estimé et pertes de paquets demandent le hook réseau (`PacketEvent` est déjà défini) |
| Performance Mode | ✅ | Module `performance_mode` : seuils haut/bas, coupe flou, ombres et animations, rétablit au-dessus |
| Battery Mode | ✅ | Module `battery_mode` : détection secteur/batterie, limiteur de framerate, effets coupés |
| Benchmark intégré | ✅ | Module `benchmark` : mesure chronométrée, verdict et réglages proposés |
| Playtime Tracker | ✅ | Service `Playtime` (agrégation jour/semaine/total), élément de HUD et graphique 14 jours |

## Capture et rejeu

| Fonctionnalité | État | Où |
| --- | --- | --- |
| Screenshot Mode | ✅ | Module `screenshot_mode` : masque les éléments marqués, capture, notifie |
| Screenshot Manager | 🟡 | Encodage PNG, rangement `<serveur>/<date>/` et énumération faits ; reste la galerie avec vignettes |
| Replay System | ⬜ | Gros morceau : capture de frames encodée, à faire dans un thread dédié |
| Instant Replay (30/60 s) | ⬜ | Tampon circulaire sur le back buffer ; raccourci déjà réservé (`ClientConfig::instantReplayKey`) |
| Clip Markers | ⬜ | Raccourci déjà réservé (`clipMarkerKey`) ; écrit un horodatage dans le journal de session |
| Match History | ✅ | Page **Historique** : serveur, durée, K/D, FPS moyen, blocs |

## Monde et navigation

| Fonctionnalité | État | Où |
| --- | --- | --- |
| Advanced Waypoints | ⬜ | `Paths::waypoints()` réservé ; demande la projection monde → écran (`Render3DEvent`) |
| World Notes | ⬜ | `Paths::notes()` réservé |
| Friend Notes | ⬜ | Idem, stockage local par pseudo |
| Favorite Servers / Quick Join | ⬜ | Demande le hook de l'écran multijoueur |
| Resource Pack Manager | ⬜ | Demande les signatures `ResourcePackRepository` |
| Shader Presets | ⬜ | Dépend du chargeur de shaders |

## Chat et son

| Fonctionnalité | État | Où |
| --- | --- | --- |
| Chat Tabs | ⬜ | `ChatReceiveEvent` est défini et annulable ; reste le rendu du chat côté client |
| Chat Search | ⬜ | Même socle |
| Chat Mentions | ⬜ | Détection sur `ChatReceiveEvent` + `Notifications::push` — le plus court des quatre |
| Chat Translator | ⬜ | Demande un accès réseau ; à déclarer dans les permissions du module |
| Sound Visualizer | ⬜ | `SoundEvent` défini ; demande la signature `SoundEngine::play` |
| Sound Mixer | ⬜ | Même hook, avec réécriture du volume |

## Vie privée

| Fonctionnalité | État | Où |
| --- | --- | --- |
| Privacy Mode | ✅ | Module `privacy_mode` : masque serveur, pseudo et coordonnées |
| Streamer Mode | ✅ | Module `streamer_mode` : confidentialité complète + filtrage du chat (adresses, invitations, mots interdits) |

## Extensibilité

| Fonctionnalité | État | Où |
| --- | --- | --- |
| Plugin API | ⬜ | La catégorie `ModuleCategory::Script` et le modèle de permissions sont en place |
| Scripting Lua / JS | ⬜ | Prévu en Lua (`external/` accueillerait la bibliothèque) ; l'API sûre s'expose via `Settings` et `EventBus` |
| Marketplace | ⬜ | Dépend d'une infrastructure serveur, à traiter en dernier |

## Cycle de vie

| Fonctionnalité | État | Où |
| --- | --- | --- |
| Notification Center | ✅ | `dll/ui/Notifications.*`, avec historique |
| Crash Reporter | ✅ | Filtre d'exception, rapport horodaté nommant le module suspect, panneau dans **Diagnostic** |
| Update Manager (stable/beta/nightly) | ⬜ | Le canal est déjà dans `ClientConfig` et dans la version compilée |
| Changelog intégré | ⬜ | Une page de plus dans le menu |
| Onboarding | ⬜ | `ClientConfig::onboardingCompleted` est déjà là et faux au premier lancement |

## Modules du catalogue

Sur les ~104 modules listés pour la version PC :

- **42 sont écrits** (voir README) ;
- **la grande majorité du reste sont des éléments de HUD**, c'est-à-dire des
  sous-classes de `TextHud` d'une vingtaine de lignes chacune. Ce n'est pas de
  la conception, c'est du remplissage — la partie difficile est faite ;
- **les modules de rendu** (Fullbright, Nametag, View Model, Motion Blur, Fog
  Color, Time Changer, Weather Changer, Block Outline, Chunk Border…) demandent
  chacun une signature ou un hook spécifique. Ils sont donc bloqués sur le pack
  de signatures, pas sur le code du client ;
- **les modules serveur** (Hive Stats, Hive Utils, Zeqa Utils, Auto GG) demandent
  le parsing du chat et du scoreboard, donc les hooks correspondants.

## Ce qui vient ensuite, dans l'ordre

1. **Un pack de signatures pour la version de Bedrock visée.** C'est ce qui
   débloque le plus de fonctionnalités par heure passée : sans lui, un tiers du
   catalogue reste inerte quelle que soit la quantité de code écrite.
2. **Le reste des éléments de HUD**, en série, sur `TextHud`.
3. **Le chat côté client**, qui débloque d'un coup Chat Tabs, Search, Mentions,
   Translator et Compact Chat.
4. **Les modules de rendu**, au fur et à mesure que les signatures arrivent.
5. **La galerie de captures** avec vignettes, au-dessus de `screenshot::gallery()`.
6. **Replay et Instant Replay**, le plus gros morceau, à traiter isolément.
