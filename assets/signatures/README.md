# Packs de signatures

Velyx ne code en dur **aucune adresse mémoire du jeu**. Tout ce que le client
lit dans Minecraft passe par un nom symbolique (`Actor::position`,
`ClientInstance::instance`, …) résolu au démarrage à partir d'un fichier JSON.

Conséquence directe : quand Mojang publie une mise à jour, il n'y a **rien à
recompiler**. Il suffit de déposer un nouveau fichier ici.

## Où le fichier est cherché

Dans cet ordre, les trois sources se cumulent (la dernière gagne) :

1. `<dossier de Velyx.dll>/assets/signatures/<majeure.mineure>.json` — le pack
   livré avec la build ;
2. `%APPDATA%/Velyx/assets/signatures/<majeure.mineure>.json` — un pack déposé
   par l'utilisateur ;
3. `%APPDATA%/Velyx/config/signatures.json` — surcharges locales, pratiques
   pour corriger une seule entrée sans réécrire le pack.

`<majeure.mineure>` vient de la version du `Minecraft.Windows.exe` en cours
d'exécution : pour `1.21.44.1`, Velyx cherche `1.21.json`.

## Format

```jsonc
{
  "signatures": {
    // Forme courte : motif IDA, l'adresse trouvée est la cible.
    "LocalPlayer::sendChatMessage": "48 89 5C 24 ? 57 48 83 EC ?",

    // Forme longue : le motif tombe sur une instruction qui *référence* la
    // cible (call rel32, lea rip+disp32). Velyx suit le déplacement.
    "ClientInstance::instance": {
      "pattern": "48 8B 0D ? ? ? ? 48 85 C9 74 ?",
      "kind": "relative",
      "operand": 3,   // distance du début de l'instruction au disp32
      "length": 7,    // longueur totale de l'instruction
      "addend": 0     // ajouté à l'adresse finale
    }
  },

  "offsets": {
    // Décalages de champs, en octets, depuis le début de l'objet.
    "ClientInstance::localPlayer": 168,
    "Actor::position": 88
  }
}
```

`?` et `??` sont des jokers. La casse n'a pas d'importance.

## Ce que Velyx fait quand une entrée manque

Rien de dramatique, et c'est volontaire :

- une signature **non requise** manquante désactive seulement la fonctionnalité
  qui en dépend ;
- une signature **requise** manquante fait démarrer le client en mode dégradé :
  le menu, les thèmes, les profils et tous les modules purement côté client
  (FPS, CPS, horloge, keystrokes, graphique de performance…) fonctionnent, mais
  ceux qui lisent l'état du jeu affichent `--` ;
- la page **Diagnostic** du menu liste chaque entrée, son propriétaire et
  l'adresse résolue. C'est le point de départ pour compléter un pack.

Un client qui refuse de démarrer parce que le jeu a bougé de 4 octets est un
client cassé ; celui-ci vous le dit et continue.

## Écrire un pack

`template.json` liste tous les noms attendus avec des motifs vides. Remplissez
ceux dont vous avez besoin — les entrées vides sont ignorées sans bruit.

Un pack se valide en lançant le jeu et en ouvrant **Menu → Diagnostic** : chaque
ligne verte est résolue, chaque ligne rouge est une signature requise qui ne
correspond plus.
