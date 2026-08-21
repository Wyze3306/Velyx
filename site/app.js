/* Velyx — site behaviour
   Three jobs: swap the language, run the menu in the hero, list the
   45 modules. The module list below is the real one, taken from the
   client's registry. */

(function () {
  'use strict';

  var API  = 'https://api.github.com/repos/Wyze3306/Velyx/releases/latest';

  /* ── the inventory ───────────────────────────────────────────── */

  var CATS = [
    { id: 'client',   fr: 'Client',      en: 'Client' },
    { id: 'movement', fr: 'Déplacement', en: 'Movement' },
    { id: 'hud',      fr: 'HUD',         en: 'HUD' },
    { id: 'render',   fr: 'Rendu',       en: 'Render' },
    { id: 'utility',  fr: 'Utilitaires', en: 'Utilities' }
  ];

  function m(cat, fr, en, dfr, den) {
    return { cat: cat, fr: fr, en: en, dfr: dfr || '', den: den || '' };
  }

  var MODULES = [
    /* client */
    m('client', 'Menu Velyx', 'Velyx menu', 'Le menu principal. Ctrl+K l’ouvre sur la recherche.', 'The main menu. Ctrl+K opens it on the search.'),
    m('client', 'Éditeur de HUD', 'HUD editor', 'Place les éléments à l’écran.', 'Arranges the elements on screen.'),
    m('client', 'Centre de notifications', 'Notification centre', 'Ce que le client a fait, et quand.', 'What the client did, and when.'),
    m('client', 'Rappel du menu', 'Menu reminder', 'Un badge dans le coin, avec la touche qui ouvre le client.', 'A corner badge naming the key that opens the client.'),
    m('client', 'Assistant de démarrage', 'Setup assistant', 'Cinq écrans au premier lancement.', 'Five screens on the first run.'),

    /* movement */
    m('movement', 'Zoom', 'Zoom', 'Réduit le champ de vision tant que la touche est maintenue.', 'Narrows the field of view while the key is held.'),
    m('movement', 'FOV personnalisé', 'Custom FOV', 'Permet un champ de vision au-delà des limites du jeu.', 'Allows a field of view past the game’s own limits.'),
    m('movement', 'FOV dynamique (Java)', 'Dynamic FOV (Java)', 'Élargit le champ de vision en sprint, comme sur Java.', 'Widens the field of view while sprinting, like Java.'),
    m('movement', 'Multiplicateur de sensibilité', 'Sensitivity multiplier', 'Ajuste la sensibilité au-delà du curseur du jeu.', 'Tunes sensitivity beyond the game’s own slider.'),
    m('movement', 'Sprint automatique', 'Auto sprint', 'Garde le sprint sans tenir la touche.', 'Keeps sprinting without holding the key.'),
    m('movement', 'Accroupissement à bascule', 'Toggle sneak', 'Une pression pour s’accroupir, une pour se relever.', 'One press to sneak, one to stand.'),
    m('movement', 'FreeLook', 'FreeLook', 'Regarde autour de soi sans changer de direction.', 'Look around without changing where you go.'),
    m('movement', 'SnapLook', 'SnapLook', 'Fait pivoter la caméra d’un angle fixe en une pression.', 'Turns the camera by a fixed angle in one press.'),
    m('movement', 'Caméra cinématique', 'Cinematic camera', 'Lisse les mouvements de caméra pour l’enregistrement.', 'Smooths camera movement for recording.'),
    m('movement', 'Perspective automatique', 'Auto perspective', 'Change de vue selon la situation.', 'Switches view to suit the situation.'),
    m('movement', 'Null Movement', 'Null movement', 'Donne la priorité à la dernière touche de direction.', 'Gives priority to the last direction key pressed.'),

    /* hud */
    m('hud', 'FPS', 'FPS', 'Images par seconde, à l’endroit que vous choisissez.', 'Frames per second, wherever you put it.'),
    m('hud', 'CPS', 'CPS', 'Clics par seconde, clic gauche et clic droit.', 'Clicks per second, left and right.'),
    m('hud', 'Horloge', 'Clock', 'L’heure réelle, sans quitter le jeu.', 'The real time, without leaving the game.'),
    m('hud', 'Coordonnées', 'Coordinates', 'X, Y, Z, et la dimension.', 'X, Y, Z, and the dimension.'),
    m('hud', 'Direction', 'Direction', 'Le point cardinal que vous regardez.', 'The compass direction you are facing.'),
    m('hud', 'Vitesse', 'Speed', 'Blocs par seconde.', 'Blocks per second.'),
    m('hud', 'Ping', 'Ping', 'La latence vers le serveur.', 'Latency to the server.'),
    m('hud', 'Mémoire', 'Memory', 'La mémoire prise par le jeu.', 'Memory the game is using.'),
    m('hud', 'Adresse du serveur', 'Server address', 'Le serveur où vous êtes. Masqué en mode streamer.', 'The server you are on. Hidden in streamer mode.'),
    m('hud', 'Minuteur AFK', 'AFK timer', 'Le temps depuis votre dernière action.', 'Time since you last did anything.'),
    m('hud', 'Stats de session', 'Session stats', 'Ce que vous avez fait depuis le lancement.', 'What you have done since launch.'),
    m('hud', 'Chronomètre', 'Stopwatch', 'Départ, arrêt, tours.', 'Start, stop, laps.'),
    m('hud', 'Keystrokes', 'Keystrokes', 'Les touches et les clics, affichés à l’écran.', 'Your keys and clicks, drawn on screen.'),
    m('hud', 'Graphique FPS', 'FPS graph', 'Temps par image, gels et 1 % les plus bas.', 'Frame times, freezes and 1% lows.'),
    m('hud', 'Moniteur serveur', 'Server monitor', 'L’état du serveur pendant que vous jouez.', 'How the server is holding up while you play.'),
    m('hud', 'Armure', 'Armour', 'Vos pièces d’armure et leur usure.', 'Your armour pieces and how worn they are.'),
    m('hud', 'Temps de jeu', 'Playtime', 'Aujourd’hui, cette semaine, au total.', 'Today, this week, all time.'),

    /* render */
    m('render', 'Viseur', 'Crosshair', 'Six formes, contour, éclair au coup, écart dynamique.', 'Six shapes, outline, hit flash, dynamic spread.'),
    m('render', 'Filtres d’écran', 'Screen filters', 'Nuit, contraste, saturation et aides pour daltoniens.', 'Night, contrast, saturation and colour-blindness aids.'),
    m('render', 'Couleur de coup', 'Hit colour', 'Change la couleur d’un joueur touché. Purement visuel.', 'Changes the colour of a player you hit. Purely visual.'),
    m('render', 'Teinte de dégâts', 'Damage tint', 'Remplace le voile rouge quand vous prenez des dégâts.', 'Replaces the red flash when you take damage.'),

    /* utility */
    m('utility', 'Mode confidentialité', 'Privacy mode', 'Masque ce qui vous identifie à l’écran.', 'Hides anything that identifies you on screen.'),
    m('utility', 'Mode streamer', 'Streamer mode', 'Masque le personnel et filtre le chat pendant un live.', 'Hides personal details and filters chat during a stream.'),
    m('utility', 'Mode performance', 'Performance mode', 'Coupe les effets coûteux quand le framerate chute.', 'Drops expensive effects when the frame rate falls.'),
    m('utility', 'Mode batterie', 'Battery mode', 'Limite le framerate quand le portable est débranché.', 'Caps the frame rate when the laptop is unplugged.'),
    m('utility', 'Mode accessibilité', 'Accessibility mode', 'Texte plus grand, contours marqués, animations réduites.', 'Larger text, thicker borders, less motion.'),
    m('utility', 'Mode capture', 'Screenshot mode', 'Masque le HUD marqué, capture, range par serveur et par date.', 'Hides marked HUD, captures, files it by server and date.'),
    m('utility', 'Benchmark', 'Benchmark', 'Mesure quelques secondes et propose des réglages.', 'Measures for a few seconds and suggests settings.'),
    m('utility', 'Marqueurs', 'Clip markers', 'Pose un repère horodaté pour retrouver un moment.', 'Drops a timestamped marker to find a moment again.')
  ];

  /* the launcher's rows: one instance per account, and they run together */

  var INSTANCES = [
    { av: 'M', fr: 'Principal', en: 'Main',    account: 'MainAccount',  ver: '1.26.44', on: true },
    { av: 'S', fr: 'Second',    en: 'Second',  account: 'AltAccount',   ver: '1.26.44', on: true },
    { av: 'B', fr: 'Build',     en: 'Build',   account: 'BuildAccount', ver: '1.26.44', on: false },
    { av: 'T', fr: 'Tests',     en: 'Testing', account: null,           ver: '1.25.60', on: false }
  ];

  /* the twelve the menu shows here, and the three that start on */
  var SHOWN = ['FPS', 'Viseur', 'Keystrokes', 'Zoom', 'Mode performance', 'Filtres d’écran',
              'Coordonnées', 'FOV dynamique (Java)', 'Mode capture', 'Graphique FPS',
              'Mode confidentialité', 'FreeLook'];
  var ON = { 'FPS': 1, 'Viseur': 1, 'Keystrokes': 1 };

  /* ── language ────────────────────────────────────────────────── */

  var META = {
    fr: {
      title: 'Velyx — client utilitaire pour Minecraft Bedrock',
      all: 'Tous',
      search: 'Rechercher un module',
      none: 'Aucun module ne correspond.',
      on1: 'module actif', onN: 'modules actifs',
      swOn: 'Désactiver', swOff: 'Activer',
      noAcc: 'aucun compte', live: 'En cours', start: 'Lancer',
      inst1: 'instance lancée', instN: 'instances lancées',
      acc1: 'compte Microsoft', accN: 'comptes Microsoft',
      stopA: 'Arrêter', startA: 'Lancer'
    },
    en: {
      title: 'Velyx — utility client for Minecraft Bedrock',
      all: 'All',
      search: 'Search for a module',
      none: 'No module matches that.',
      on1: 'module on', onN: 'modules on',
      swOn: 'Turn off', swOff: 'Turn on',
      noAcc: 'no account', live: 'Running', start: 'Launch',
      inst1: 'instance running', instN: 'instances running',
      acc1: 'Microsoft account', accN: 'Microsoft accounts',
      stopA: 'Stop', startA: 'Launch'
    }
  };

  var lang = 'fr';

  function applyLang(next) {
    lang = next;
    document.documentElement.lang = next;
    document.title = META[next].title;

    var nodes = document.querySelectorAll('[data-en]');
    for (var i = 0; i < nodes.length; i++) {
      var el = nodes[i];
      if (el.dataset.fr === undefined) el.dataset.fr = el.innerHTML;
      el.innerHTML = next === 'en' ? el.dataset.en : el.dataset.fr;
    }

    var holders = document.querySelectorAll('[data-en-placeholder]');
    for (var j = 0; j < holders.length; j++) {
      var h = holders[j];
      if (h.dataset.frPlaceholder === undefined) h.dataset.frPlaceholder = h.placeholder;
      h.placeholder = next === 'en' ? h.dataset.enPlaceholder : h.dataset.frPlaceholder;
      h.setAttribute('aria-label', h.placeholder);
    }

    var segs = document.querySelectorAll('.seg-btn');
    for (var k = 0; k < segs.length; k++) {
      var on = segs[k].dataset.lang === next;
      segs[k].classList.toggle('is-on', on);
      segs[k].setAttribute('aria-pressed', on ? 'true' : 'false');
    }

    try { localStorage.setItem('velyx-lang', next); } catch (e) {}

    drawLauncher();
    drawHudChips();
    drawChips();
    drawGrid();
    drawInventory();
    paintRelease();
  }

  /* ── the menu in the hero ────────────────────────────────────── */

  var grid  = document.getElementById('grid');
  var chips = document.getElementById('chips');
  var find  = document.getElementById('find');
  var count = document.getElementById('count');
  var pick  = 'all';

  function menuList() {
    var out = [];
    for (var i = 0; i < MODULES.length; i++) {
      if (SHOWN.indexOf(MODULES[i].fr) !== -1) out.push(MODULES[i]);
    }
    return out;
  }

  function label(item) { return lang === 'en' ? item.en : item.fr; }
  function desc(item)  { return lang === 'en' ? item.den : item.dfr; }

  function fold(s) {
    return s.toLowerCase().normalize('NFD').replace(/[\u0300-\u036f]/g, '');
  }

  function drawChips() {
    if (!chips) return;
    var used = {};
    var list = menuList();
    for (var i = 0; i < list.length; i++) used[list[i].cat] = 1;

    var html = '<button type="button" class="chip' + (pick === 'all' ? ' is-on' : '') +
               '" data-cat="all">' + META[lang].all + '</button>';
    for (var c = 0; c < CATS.length; c++) {
      if (!used[CATS[c].id]) continue;
      html += '<button type="button" class="chip' + (pick === CATS[c].id ? ' is-on' : '') +
              '" data-cat="' + CATS[c].id + '">' + CATS[c][lang] + '</button>';
    }
    chips.innerHTML = html;
  }

  function drawGrid() {
    if (!grid) return;
    var q = fold(find && find.value ? find.value : '');
    var list = menuList();
    var html = '';
    var shown = 0;

    for (var i = 0; i < list.length; i++) {
      var it = list[i];
      if (pick !== 'all' && it.cat !== pick) continue;
      if (q && fold(label(it) + ' ' + desc(it)).indexOf(q) === -1) continue;

      var on = !!ON[it.fr];
      html += '<button type="button" role="switch" aria-checked="' + (on ? 'true' : 'false') +
              '" class="mcard' + (on ? ' is-on' : '') + '"' +
              ' data-id="' + it.fr.replace(/"/g, '&quot;') + '"' +
              ' aria-label="' + (on ? META[lang].swOn : META[lang].swOff) + ' — ' + label(it) + '">' +
                '<span class="bar"></span>' +
                '<span class="txt">' +
                  '<span class="nm">' + label(it) + '</span>' +
                  '<span class="ds">' + desc(it) + '</span>' +
                '</span>' +
                '<span class="sw"></span>' +
              '</button>';
      shown++;
    }

    grid.innerHTML = shown ? html : '<p class="menu-empty">' + META[lang].none + '</p>';
    drawCount();
  }

  function drawCount() {
    if (!count) return;
    var n = 0, list = menuList();
    for (var i = 0; i < list.length; i++) if (ON[list[i].fr]) n++;
    count.textContent = n + ' ' + (n === 1 ? META[lang].on1 : META[lang].onN);
  }

  if (grid) {
    grid.addEventListener('click', function (e) {
      var card = e.target.closest ? e.target.closest('.mcard') : null;
      if (!card) return;
      var id = card.dataset.id;
      if (ON[id]) delete ON[id]; else ON[id] = 1;
      var on = !!ON[id];
      card.classList.toggle('is-on', on);
      card.setAttribute('aria-checked', on ? 'true' : 'false');
      var name = card.querySelector('.nm').textContent;
      card.setAttribute('aria-label', (on ? META[lang].swOn : META[lang].swOff) + ' — ' + name);
      drawCount();
    });
  }

  if (chips) {
    chips.addEventListener('click', function (e) {
      var b = e.target.closest ? e.target.closest('.chip') : null;
      if (!b) return;
      pick = b.dataset.cat;
      drawChips();
      drawGrid();
    });
  }

  if (find) {
    find.addEventListener('input', drawGrid);
  }

  /* ── the launcher, running in the hero ────────────────────────────────── */

  var lcRows = document.getElementById('lc-rows');

  function drawLauncher() {
    if (!lcRows) return;
    var html = '';
    for (var i = 0; i < INSTANCES.length; i++) {
      var it = INSTANCES[i];
      var name = lang === 'en' ? it.en : it.fr;
      var acc = it.account || META[lang].noAcc;
      html += '<li>' +
                '<i class="av av-' + (i + 1) + '">' + it.av + '</i>' +
                '<span class="lc-name"><b>' + name + '</b><em>' + acc + '</em></span>' +
                '<span class="mono lc-ver">' + it.ver + '</span>' +
                '<button type="button" class="lc-go' + (it.on ? ' is-live' : '') + '" data-i="' + i +
                '" aria-label="' + (it.on ? META[lang].stopA : META[lang].startA) + ' — ' + name + '">' +
                  (it.on ? META[lang].live : META[lang].start) +
                '</button>' +
              '</li>';
    }
    lcRows.innerHTML = html;
    drawLauncherCount();
  }

  function drawLauncherCount() {
    var running = 0, accounts = 0;
    for (var i = 0; i < INSTANCES.length; i++) {
      if (!INSTANCES[i].on) continue;
      running++;
      if (INSTANCES[i].account) accounts++;
    }
    var c = document.getElementById('lc-count');
    var a = document.getElementById('lc-acc');
    if (c) c.textContent = running + ' ' + (running === 1 ? META[lang].inst1 : META[lang].instN);
    if (a) a.textContent = accounts + ' ' + (accounts === 1 ? META[lang].acc1 : META[lang].accN);
  }

  if (lcRows) {
    lcRows.addEventListener('click', function (e) {
      var b = e.target.closest ? e.target.closest('.lc-go') : null;
      if (!b) return;
      var it = INSTANCES[+b.dataset.i];
      it.on = !it.on;
      drawLauncher();
    });
  }

  /* ── the seventeen HUD elements ──────────────────────────────── */

  function drawHudChips() {
    var host = document.getElementById('hud-chips');
    if (!host) return;
    var html = '';
    for (var i = 0; i < MODULES.length; i++) {
      if (MODULES[i].cat !== 'hud') continue;
      html += '<li>' + label(MODULES[i]) + '</li>';
    }
    host.innerHTML = html;
  }

  /* ── the full list ───────────────────────────────────────────── */

  function drawInventory() {
    var host = document.getElementById('mod-cols');
    if (!host) return;
    var html = '';
    for (var c = 0; c < CATS.length; c++) {
      var cat = CATS[c], items = '', n = 0;
      for (var i = 0; i < MODULES.length; i++) {
        if (MODULES[i].cat !== cat.id) continue;
        items += '<li>' + label(MODULES[i]) + '</li>';
        n++;
      }
      html += '<div class="mod-group">' +
                '<div class="mod-head">' +
                  '<span class="mod-cat">' + cat[lang] + '</span>' +
                  '<span class="mod-n">' + n + '</span>' +
                '</div>' +
                '<ul class="mod-list">' + items + '</ul>' +
              '</div>';
    }
    host.innerHTML = html;
  }

  /* ── the download button, once there is something to download ── */

  var release = null;

  function paintRelease() {
    var meta = document.getElementById('dl-meta');
    if (!release || !meta || !release.tag) return;
    var size = release.exeSize ? ' · ' + Math.round(release.exeSize / 1048576) + ' ' + (lang === 'en' ? 'MB' : 'Mo') : '';
    var base = lang === 'en'
      ? ' · Windows 10 and 11 · 64-bit · free and open source'
      : ' · Windows 10 et 11 · 64 bits · gratuit et open source';
    meta.removeAttribute('data-en');
    meta.textContent = release.tag + size + base;
  }

  /* Every download link says which file it wants; until a release exists they
     all lead to the releases page, which is the honest answer. */
  function pointDownloads(exe, dll) {
    var links = document.querySelectorAll('[data-dl]');
    for (var i = 0; i < links.length; i++) {
      var want = links[i].dataset.dl === 'dll' ? dll : exe;
      if (want) links[i].href = want.browser_download_url;
    }
  }

  function askRelease() {
    if (!window.fetch) return;
    fetch(API, { headers: { 'Accept': 'application/vnd.github+json' } })
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (rel) {
        if (!rel || !rel.assets) return;
        var exe = null, dll = null;
        for (var i = 0; i < rel.assets.length; i++) {
          var name = rel.assets[i].name;
          if (!exe && /\.exe$/i.test(name)) exe = rel.assets[i];
          if (!dll && /\.dll$/i.test(name)) dll = rel.assets[i];
        }
        release = { tag: rel.tag_name, exeSize: exe ? exe.size : 0 };
        pointDownloads(exe, dll);
        paintRelease();
      })
      .catch(function () {});
  }

  /* ── go ──────────────────────────────────────────────────────── */

  var saved = null;
  try { saved = localStorage.getItem('velyx-lang'); } catch (e) {}
  var start = saved || ((navigator.language || 'fr').toLowerCase().indexOf('fr') === 0 ? 'fr' : 'en');

  document.addEventListener('click', function (e) {
    var b = e.target.closest ? e.target.closest('.seg-btn') : null;
    if (!b) return;
    applyLang(b.dataset.lang);
  });

  applyLang(start);
  askRelease();
})();
