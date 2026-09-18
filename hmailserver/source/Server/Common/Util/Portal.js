// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

(function () {
  'use strict';
  var el = function (id) { return document.getElementById(id); };
  // ---- Language. English is the key; a catalogue maps it to the reader's
  // language. The one the server chose from Accept-Language rides in the
  // page (lang-data, parsed below); a choice kept in this browser or in the
  // account's preferences fetches another (/portal-lang/<code>.json). The
  // script's own texts go through t() and tf(); the markup is walked once
  // the catalogue is known, and again on a change, each text node and
  // attribute keeping its English so it can be walked back.
  var LANGUAGES = [['en', 'English'], ['cs', '\u010ce\u0161tina'], ['da', 'Dansk'], ['de', 'Deutsch'], ['el', '\u0395\u03bb\u03bb\u03b7\u03bd\u03b9\u03ba\u03ac'], ['es', 'Espa\u00f1ol'], ['fi', 'Suomi'], ['fr', 'Fran\u00e7ais'], ['it', 'Italiano'], ['ja', '\u65e5\u672c\u8a9e'], ['ko', '\ud55c\uad6d\uc5b4'], ['nb', 'Norsk bokm\u00e5l'], ['nl', 'Nederlands'], ['pl', 'Polski'], ['pt-BR', 'Portugu\u00eas (Brasil)'], ['pt-PT', 'Portugu\u00eas (Portugal)'], ['ru', '\u0420\u0443\u0441\u0441\u043a\u0438\u0439'], ['sv', 'Svenska'], ['tr', 'T\u00fcrk\u00e7e'], ['uk', '\u0423\u043a\u0440\u0430\u0457\u043d\u0441\u044c\u043a\u0430'], ['zh-Hans', '\u7b80\u4f53\u4e2d\u6587'], ['ar', '\u0627\u0644\u0639\u0631\u0628\u064a\u0629'], ['he', '\u05e2\u05d1\u05e8\u05d9\u05ea'], ['fa', '\u0641\u0627\u0631\u0633\u06cc']];
  // Written right to left: the document is mirrored for these - dir on the
  // root, which the stylesheet's logical properties follow. A message keeps
  // its own direction: the body panes and the compose fields are dir=auto.
  var RTL = { ar: true, he: true, fa: true };
  var catalogue = {};
  var languageActive = 'en';
  var catalogueHas = function (key) { return Object.prototype.hasOwnProperty.call(catalogue, key) && typeof catalogue[key] === 'string' && catalogue[key] !== ''; };
  var t = function (key) { return catalogueHas(key) ? catalogue[key] : key; };
  var tf = function (key) { var s = t(key); for (var i = 1; i < arguments.length; i++) { s = s.split('{' + (i - 1) + '}').join(String(arguments[i])); } return s; };
  // Marks a text a table holds for a later t(); the identity. The checker
  // reads this page's keys out of t(), tf() and K().
  var K = function (key) { return key; };
  var LOCALISED_ATTRIBUTES = ['placeholder', 'aria-label', 'title', 'alt'];
  var localiseTree = function (root) {
    if (!root || !root.childNodes) { return; }
    var kids = root.childNodes;
    for (var i = 0; i < kids.length; i++) {
      var n = kids[i];
      if (n.nodeType === 3) {
        var original = n.__en !== undefined ? n.__en : String(n.nodeValue || '');
        var trimmed = original.trim();
        if (trimmed && (catalogueHas(trimmed) || n.__en !== undefined)) {
          if (n.__en === undefined) { n.__en = original; }
          n.nodeValue = original.replace(trimmed, function () { return t(trimmed); });
        }
      } else if (n.nodeType === 1) {
        if (n.tagName === 'SCRIPT' || n.tagName === 'STYLE') { continue; }
        for (var a = 0; a < LOCALISED_ATTRIBUTES.length; a++) {
          var name = LOCALISED_ATTRIBUTES[a];
          if (!n.hasAttribute || !n.hasAttribute(name)) { continue; }
          var key = '__en_' + name;
          var given = n[key] !== undefined ? n[key] : n.getAttribute(name);
          if (catalogueHas(given) || n[key] !== undefined) { n[key] = given; n.setAttribute(name, t(given)); }
        }
        localiseTree(n);
      }
    }
  };
  var knownLanguage = function (code) {
    var lower = String(code || '').toLowerCase();
    for (var i = 0; i < LANGUAGES.length; i++) { if (LANGUAGES[i][0].toLowerCase() === lower) { return LANGUAGES[i][0]; } }
    return null;
  };
  var applyCatalogue = function (code, data) {
    catalogue = data || {};
    languageActive = code;
    try { if (code === 'en') { localStorage.removeItem('hmPortalLang'); } else { localStorage.setItem('hmPortalLang', code); } } catch (e) { /* no storage */ }
    if (document.documentElement && document.documentElement.setAttribute) {
      document.documentElement.setAttribute('lang', code);
      document.documentElement.setAttribute('dir', RTL[code] ? 'rtl' : 'ltr');
    }
    if (document.body) { localiseTree(document.body); }
    var select = el('pref-language');
    if (select) { select.value = code; }
  };
  var loadLanguage = function (code) {
    if (!code || code === languageActive) { return Promise.resolve(true); }
    if (code === 'en') { applyCatalogue('en', {}); return Promise.resolve(true); }
    return fetch('/portal-lang/' + encodeURIComponent(code) + '.json', { credentials: 'same-origin' }).then(function (response) {
      return response.ok ? response.json() : null;
    }).then(function (data) { if (data) { applyCatalogue(code, data); return true; } return false; }, function () { return false; });
  };
  var browserLanguage = function () {
    if (typeof navigator === 'undefined') { return 'en'; }
    var wanted = navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language || 'en'];
    for (var i = 0; i < wanted.length; i++) {
      var w = String(wanted[i] || '');
      var exact = knownLanguage(w);
      if (exact) { return exact; }
      var primary = w.split('-')[0].toLowerCase();
      if (primary === 'pt') { return 'pt-BR'; }
      if (primary === 'zh') { return 'zh-Hans'; }
      if (primary === 'no' || primary === 'nn') { return 'nb'; }
      var byPrimary = knownLanguage(primary);
      if (byPrimary) { return byPrimary; }
    }
    return 'en';
  };
  (function () {
    var select = el('pref-language');
    if (select) {
      LANGUAGES.forEach(function (pair) { var o = document.createElement('option'); o.value = pair[0]; o.textContent = pair[1]; select.appendChild(o); });
    }
    var data = null;
    try { data = el('lang-data'); } catch (e) { data = null; }
    var code = null;
    var parsed = null;
    if (data) { try { parsed = JSON.parse(data.textContent || '{}'); code = knownLanguage(data.getAttribute('data-lang')); } catch (e) { parsed = null; } }
    var remembered = null;
    try { remembered = knownLanguage(localStorage.getItem('hmPortalLang')); } catch (e) { remembered = null; }
    if (remembered && remembered !== code) { loadLanguage(remembered); }
    else if (parsed && code && code !== 'en') { applyCatalogue(code, parsed); }
    else if (!remembered && !code) { var guess = browserLanguage(); if (guess !== 'en') { loadLanguage(guess); } }
  })();
  var applyLanguagePref = function () { var code = knownLanguage(pref('language') || ''); if (code) { loadLanguage(code); } };
  var say = function (id, text, ok) { var s = el(id); s.textContent = text || ''; s.className = 'status' + (text ? (ok ? ' ok' : ' error') : ''); };
  // The password is sent exactly once, to start the session; from then on
  // the browser's cookie is the credential and nothing is kept in memory -
  // with one exception the reader asks for: a second account added beside
  // this one has a session token of its own, held in memory by the block
  // "A second account in the same page" below, and a request made while that
  // account is the active one carries it as Authorization: Bearer. `who`
  // names the account a request is for when it is not the active one:
  // 'first' or 'second'.
  // The second factor a self-service form asks for, in BOTH of the places it
  // can come from: the one-time code from an authenticator app, and - for
  // somebody whose authenticator is gone - one of the recovery codes they
  // saved. The server takes either at every one of these routes
  // (AccountRecovery.h), so the page offers either at every one of them; it
  // used to offer the recovery code at the sign-in alone, which left the way
  // back in ending at a Security page with a box no lost authenticator could
  // fill. An empty field sends no header, because call() drops a header with
  // no value.
  var secondFactorHeaders = function (otpId, codeId) {
    return { 'X-hMailServer-OTP': el(otpId).value.trim(), 'X-hMailServer-Recovery-Code': el(codeId).value.trim() };
  };
  // What a 401 asking for the code does to the form: both fields appear, the
  // authenticator's takes the focus because most people have one, and the
  // sentence names both so that somebody who has lost theirs knows the other
  // field is for them.
  var askSecondFactor = function (otpId, codeId, statusId) {
    el(otpId).hidden = false;
    el(codeId).hidden = false;
    el(otpId).focus();
    say(statusId, t('Enter the code from your authenticator app, or one of your recovery codes.'), false);
  };
  var call = function (method, path, body, extra, who) {
    var h = {};
    if (extra) { for (var k in extra) { if (extra[k]) { h[k] = extra[k]; } } }
    if (method !== 'GET') { h['X-Requested-With'] = 'hMailServer'; }
    var asSecond = bearerFor(h, who);
    var options = { method: method, headers: h, cache: 'no-store', credentials: 'same-origin' };
    if (body !== undefined) { h['Content-Type'] = 'application/json'; options.body = JSON.stringify(body); }
    return fetch(path, options).then(function (response) {
      return response.text().then(function (text) {
        var data = null;
        try { data = text ? JSON.parse(text) : null; } catch (e) { data = null; }
        // A 401 that asks for the one-time code is not the session ending:
        // the password was right, and the code is what the form asks for next.
        // A 401 on the second account's token is that session ending, and
        // that session alone: the page goes back to the first account and
        // says so, rather than putting the gate up over a session that is
        // still live.
        if (response.status === 401 && asSecond && response.headers.get('X-hMailServer-OTP') !== 'required') {
          secondEnded(t('The second account\'s session has ended. Add it again to go on using it.'));
        } else if (response.status === 401 && !asSecond && path !== '/api/v1/me' && path.indexOf('/api/v1/session') !== 0 && !el('account').hidden && response.headers.get('X-hMailServer-OTP') !== 'required') {
          showSignIn();
          say('signin-status', t('Your session has ended. Sign in again.'), false);
        }
        return { status: response.status, data: data, otp: response.headers.get('X-hMailServer-OTP') };
      });
    }, function () {
      // The request never reached the server: the network went, or the
      // listener did. Without this arm the rejection is unhandled and the
      // caller's .then never runs, so the button that was pressed does
      // nothing at all and says nothing - and the probe, which schedules
      // its next tick inside its own .then, stops for good while the page
      // goes on saying it is watching. Status 0 is not a status any
      // server sends, and every check here is written as 'is it the one
      // I wanted', so it reports as the failure it is.
      return { status: 0, data: null, otp: null };
    });
  };
  // The same credential rule for the few requests that are not JSON - a part
  // fetched as bytes, a chunk of a file uploaded, a message the browser
  // sends after the page has closed: fetch, with the second account's token
  // added when the request is that account's.
  var apiFetch = function (url, options, who) {
    options = options || {};
    var h = {};
    if (options.headers) { for (var k in options.headers) { h[k] = options.headers[k]; } }
    bearerFor(h, who);
    options.headers = h;
    if (!options.credentials) { options.credentials = 'same-origin'; }
    return fetch(url, options);
  };
  var describe = function (result, fallback) {
    // The suppression list's refusal, said in the reader's language rather than
    // the server's English: the address is the one part of it that varies.
    // An entry an administrator added by hand is not an address that bounced,
    // and the route says which kind it is.
    if (result.data && result.data.code === 'recipient_suppressed' && result.data.address) {
      if (result.data.source === 'manual') {
        return tf('Not sent to {0}: your administrator has put this address on the suppression list of your domain, and can take it off again.', result.data.address);
      }
      return tf('Not sent to {0}: mail to this address has bounced, and it is on the suppression list of your domain. Your administrator can remove it.', result.data.address);
    }
    if (result.data && result.data.error) { return result.data.error; }
    if (result.status === 401) { return 'The address or password is not right, or the session has ended.'; }
    if (result.status === 429) { return 'Too many attempts; wait a minute and try again.'; }
    return fallback + ' (' + result.status + ')';
  };
  // A refusal the server gives a reason word for, shown in the reader's language
  // rather than in the server's English: the password breach check's two. Any
  // other refusal is describe()'s, in the server's own words as before.
  var REFUSALS = {
    breached: K('This password appears in a published data breach, so it would be among the first tried against this account. Choose a different one.'),
    breach_check_unavailable: K('The password could not be checked against published data breaches just now, and this server is set to refuse a change it cannot check. Try again later.')
  };
  var refusal = function (result, fallback) {
    var reason = result.data && result.data.reason;
    if (reason && Object.prototype.hasOwnProperty.call(REFUSALS, reason)) { return t(REFUSALS[reason]); }
    return describe(result, fallback);
  };
  var format = function (bytes) {
    if (bytes >= 1073741824) { return (bytes / 1073741824).toFixed(1) + ' GB'; }
    if (bytes >= 1048576) { return (bytes / 1048576).toFixed(1) + ' MB'; }
    if (bytes >= 1024) { return (bytes / 1024).toFixed(0) + ' KB'; }
    return bytes + ' bytes';
  };
  // Every value from the server is written as text, never as markup.
  var node = function (tag, text, className) {
    var n = document.createElement(tag);
    if (text !== undefined) { n.textContent = text; }
    if (className) { n.className = className; }
    return n;
  };
  var clear = function (n) { while (n.firstChild) { n.removeChild(n.firstChild); } };
  var button = function (text, className) { var b = node('button', text, className || 'btn ghost sm'); b.type = 'button'; return b; };

  // ---- Where the reader is: the address bar -------------------------------
  //
  // Routes live in the fragment, not the path. The page is served by the
  // server at exactly one address - RestApiServer answers GET /portal and
  // nothing below it - so a path route would be a 404 on the first reload,
  // and making the server serve one page under a whole subtree is a change
  // to the routing table for the sake of a cosmetic URL. The fragment needs
  // no server route at all, is never sent to the server (so no folder name
  // or search term reaches a log), and gives the history API everything it
  // needs: assigning location.hash pushes an entry and fires hashchange, so
  // Back and Forward work, and a reload lands where the reader was.
  //
  //   #/f/<id>[?q=text][&before=<uid>]   a folder, or a search inside it
  //   #/search?q=text                    a search across every folder
  //   #/m/<id>                           one message
  //   #/compose[?reply|replyall|forward|draft=<id>]
  //   #/held  #/folders  #/settings  #/filters  #/password
  var parseHash = function (raw) {
    var text = String(raw === undefined ? (location.hash || '') : raw).replace(/^#/, '');
    var cut = text.indexOf('?');
    var query = cut >= 0 ? text.slice(cut + 1) : '';
    var path = (cut >= 0 ? text.slice(0, cut) : text).replace(/^\/+/, '').replace(/\/+$/, '');
    var params = {};
    query.split('&').forEach(function (pair) {
      if (!pair) { return; }
      var eq = pair.indexOf('=');
      var key = decodeURIComponent(eq < 0 ? pair : pair.slice(0, eq));
      params[key] = eq < 0 ? '' : decodeURIComponent(pair.slice(eq + 1).replace(/\+/g, ' '));
    });
    return { parts: path ? path.split('/') : [], params: params, hash: text };
  };
  var go = function (hash) {
    rememberPlace();
    if (('#' + hash) === location.hash) { route(); return; }
    location.hash = hash;
  };
  var replaceWith = function (hash) {
    rememberPlace();
    if (history.replaceState) { history.replaceState(null, '', '#' + hash); route(); return; }
    location.replace('#' + hash);
  };
  var listHash = function () {
    if (state.everywhere && state.query) { return '/search?q=' + encodeURIComponent(state.query); }
    return '/f/' + (state.folderId || inboxId) + (state.query ? '?q=' + encodeURIComponent(state.query) : '') +
      (state.before ? (state.query ? '&' : '?') + 'before=' + state.before : '');
  };

  var state = { folderId: 0, query: '', everywhere: false, before: 0 };
  var me = '';
  var directoryLinked = false;
  var quarantineOn = true;

  // ---- The app shell ------------------------------------------------------
  var panels = ['mail-section', 'quarantine-section', 'folders-section', 'settings-section', 'filter-section', 'contacts-section', 'directory-section', 'away-section', 'storage-section', 'security-section', 'password-section', 'scheduled-section', 'senders-section', 'calendar-section', 'tasks-section', 'subscriptions-section', 'accounts-section', 'masked-section', 'recover-section', 'confidential-section'];
  var SETTINGS_PANELS = ['settings-section', 'away-section', 'filter-section', 'folders-section', 'storage-section', 'senders-section', 'subscriptions-section', 'accounts-section', 'masked-section', 'security-section', 'password-section'];
  var showPanel = function (id, title) {
    panels.forEach(function (p) { var e = el(p); if (e) { e.hidden = p !== id; } });
    el('settings-head').hidden = SETTINGS_PANELS.indexOf(id) < 0;
    el('view-title').textContent = title;
    var section = el(id);
    if (section && section.scrollTop) { section.scrollTop = 0; }
    if (id === 'mail-section') { applyPane(); }
    closeMenus();
    el('account').classList.remove('nav-open');
  };
  var markNav = function (hash) {
    [el('folder-nav'), el('view-nav'), el('side-nav')].forEach(function (nav) {
      if (!nav) { return; }
      var children = nav.children;
      for (var i = 0; i < children.length; i++) {
        children[i].classList.toggle('on', !!hash && children[i].getAttribute('data-route') === hash);
      }
    });
  };

  // ---- Folders ------------------------------------------------------------
  var allFolders = [];
  var foldersById = {};
  var inboxId = 0;
  // The hierarchy delimiter is the server's, and it is in the folder listing;
  // a folder's name is a path in it, so making or moving a folder means
  // sending the whole path and letting the server make what is missing.
  var delimiter = '.';
  var flatten = function (list, into, depth) {
    list.forEach(function (f) { f.depth = depth; into.push(f); flatten(f.subfolders || [], into, depth + 1); });
    return into;
  };
  var folderName = function (id) { var f = foldersById[id]; return f ? (f.path.toUpperCase() === 'INBOX' ? t('Inbox') : f.path) : 'Mail'; };
  // The folders a message may be moved to, as options; an option given
  // first (such as "leave it") goes before them.
  var fillFolderSelect = function (select, homeId, first) {
    clear(select);
    if (first) { select.appendChild(first); }
    var home = foldersById[homeId];
    allFolders.forEach(function (f) {
      if (f.id === homeId || !f.writable || (home && f.account_id !== home.account_id)) { return; }
      var option = node('option', f.path); option.value = f.id; select.appendChild(option);
    });
  };
  var renderFolders = function (tree) {
    delimiter = tree.delimiter || delimiter;
    var folders = flatten(tree.folders, [], 0);
    (tree.shared || []).forEach(function (share) {
      var start = folders.length;
      flatten(share.folders, folders, 0);
      for (var s = start; s < folders.length; s++) { folders[s].owner = share.owner || ''; }
    });
    allFolders = folders;
    foldersById = {};
    folders.forEach(function (f) { foldersById[f.id] = f; });
    var inbox = folders.filter(function (f) { return f.path.toUpperCase() === 'INBOX'; })[0];
    inboxId = inbox ? inbox.id : (folders.length ? folders[0].id : 0);
    var nav = el('folder-nav');
    clear(nav);
    var special = function (f, use) { return String(f.special_use || '').indexOf(String.fromCharCode(92) + use) >= 0; };
    // One entry: an icon, the name, the count - the third child, which the
    // change probe updates in place.
    var entry = function (f, iconName, route, label, count, cls) {
      var b = node('button', undefined, cls || '');
      b.type = 'button';
      b.setAttribute('data-route', route);
      var ico = node('span', undefined, 'ico'); ico.appendChild(icon(iconName)); b.appendChild(ico);
      b.appendChild(node('span', label, 'nm'));
      b.appendChild(node('span', count ? String(count) : '', 'ct'));
      if (count && !(f && special(f, 'Drafts'))) { b.classList.add('unread'); }
      b.addEventListener('click', function () { go(route); });
      if (f && route.indexOf('/f/') === 0) { dropTarget(b, f.id); }
      nav.appendChild(b);
      return b;
    };
    // The system folders first, in the order every mail client uses, then
    // the reader's own, then what others share. The Snoozed folder is the
    // server's waiting room, not a place to file things: it stays out.
    var order = { Sent: 2, Drafts: 3, Archive: 4, Junk: 5, Trash: 6 };
    var icons = { inbox: 'inbox', sent: 'send', drafts: 'draft', archive: 'archive', junk: 'junk', trash: 'trash' };
    var system = [], rest = [];
    folders.filter(function (f) { return !f.owner && f.path !== 'Snoozed'; }).forEach(function (f) {
      if (f.id === inboxId) { system.push([f, 'inbox', 0]); return; }
      var found = null;
      Object.keys(order).forEach(function (use) { if (!found && special(f, use)) { found = [f, use.toLowerCase(), order[use]]; } });
      if (found) { system.push(found); } else { rest.push(f); }
    });
    system.sort(function (a, b) { return a[2] - b[2]; });
    system.forEach(function (s) {
      var f = s[0];
      entry(f, icons[s[1]], '/f/' + f.id, s[1] === 'inbox' ? t('Inbox') : (f.name || f.path), s[1] === 'drafts' ? f.count : f.unseen, '');
      if (s[2] === 0) {
        entry(null, 'star', '/starred', t('Starred'), 0, '');
        if (markerShown()) { entry(null, 'important', '/important', t('Important'), 0, ''); }
      }
    });
    var saved = savedSearches();
    if (saved.length) {
      nav.appendChild(node('div', t('Saved searches'), 'navhead'));
      saved.forEach(function (s, i) { entry(null, 'search', savedSearchRoute(s, i), s.name, 0, ''); });
    }
    if (rest.length) {
      var head = node('div', t('Folders'), 'navhead');
      head.appendChild(node('span', undefined, 'grow'));
      var add = iconButton('plus', t('New folder'));
      add.addEventListener('click', function () { go('/folders'); });
      head.appendChild(add);
      nav.appendChild(head);
      // A folder under INBOX - the shape some clients make every folder in -
      // is listed as one of the reader's own, not as the inbox's child.
      rest.forEach(function (f) {
        var indent = f.path.toUpperCase().indexOf('INBOX' + delimiter.toUpperCase()) === 0 ? f.depth - 1 : f.depth;
        entry(f, 'folder', '/f/' + f.id, f.name || f.path, f.unseen, indent > 0 ? (indent > 1 ? 'child2' : 'child') : '');
      });
    }
    var lastOwner = '';
    folders.filter(function (f) { return f.owner; }).forEach(function (f) {
      if (f.owner !== lastOwner) {
        nav.appendChild(node('div', f.owner.charAt(0) === '#' ? t('Public folders') : t('Shared by ') + f.owner, 'navhead'));
        lastOwner = f.owner;
      }
      entry(f, 'folder', '/f/' + f.id, f.name || f.path, f.unseen, f.depth ? 'child' : '');
    });
    markNav(currentNavHash());
    updateTitle();
    renderAccountNav();
    renderNotifyFolders();
    fillFolderSelect(el('rule-folder'), 0);
    fillFolderSelect(el('folder-export-target'), 0);
    fillFolderSelect(el('folder-import-target'), 0);
    setExportLink();
  };
  var loadFolders = function () {
    return call('GET', '/api/v1/me/folders').then(function (result) {
      if (result.status === 200 && result.data) {
        renderFolders(result.data);
        renderQuickSteps();
        // The tree goes into the offline store too, so a page opened with no
        // connection has folders to show the kept messages under.
        if (offlineMine()) {
          folderTreeLast = result.data;
          if (offline.days) { metaPut('folders', result.data); }
        }
      }
      return result;
    });
  };

  // ---- The listing --------------------------------------------------------
  var listRows = [];
  var cursor = -1;
  var selected = {};
  var wholeFolder = false;
  var lastListing = null;
  // What "where the reader was" means, kept whenever the listing is left:
  // the scroll offset, the message the cursor was on, and the ticked boxes.
  var rememberPlace = function () {
    if (!lastListing || el('mail-section').hidden || el('message-list').hidden) { return; }
    lastListing.scroll = el('message-list').scrollTop;
    lastListing.cursorId = cursor >= 0 && listRows[cursor] ? listRows[cursor].id : 0;
    lastListing.selected = selected;
  };
  // The list and one message share a panel, so what belongs to the list -
  // the mailbox card above it and the keyboard hint - goes with it.
  var showList = function () {
    el('message-list').hidden = false;
    el('list-tools').hidden = false;
    if (!current) {
      el('message-view').hidden = true;
      el('pane-empty').hidden = false;
      el('mail-section').classList.remove('has-message');
    }
  };
  var setCursor = function (index, silent) {
    if (!listRows.length) { cursor = -1; return; }
    if (typeof prefetchNext === 'function') { setTimeout0(prefetchNext); }
    cursor = Math.max(0, Math.min(listRows.length - 1, index));
    listRows.forEach(function (r, i) { r.row.classList.toggle('msg-cursor', i === cursor); });
    if (!silent) { listRows[cursor].row.scrollIntoView({ block: 'nearest' }); }
  };
  var selectedIds = function () { return Object.keys(selected).filter(function (k) { return selected[k]; }).map(Number); };
  var renderBulk = function () {
    var ids = selectedIds();
    fillFolderSelect(el('bulk-move'), state.folderId);
    var all = el('select-all');
    if (all) {
      all.checked = listRows.length > 0 && listRows.every(function (r) { return !!selected[r.id]; });
      all.indeterminate = ids.length > 0 && !all.checked;
    }
    if (!all || !all.checked) { wholeFolder = false; }
    renderSelectNote();
    el('bulk-bar').hidden = ids.length === 0;
    el('bulk-count').textContent = wholeFolder ? tf('All of {0} selected', selectionScope()) : ids.length ? tf('{0} selected', ids.length) : '';
    listRows.forEach(function (r) { r.row.classList.toggle('selected', !!selected[r.id]); });
  };
  var clearSelection = function () { selected = {}; wholeFolder = false; renderBulk(); };
  // Shift-click and Ctrl-click in the list, as the file manager has them: a
  // shift-click ticks every row from the last row clicked to this one, a
  // ctrl-click (Cmd on a Mac) ticks or unticks this one without opening it.
  // The anchor is the row last clicked in any way, or ticked with x; when it
  // is no longer in the listing, the cursor row stands in for it.
  var anchorId = 0;
  var tickEntry = function (entry, on) {
    (entry.ids || [entry.id]).forEach(function (id) { selected[id] = on; });
    if (entry.box) { entry.box.checked = on; }
  };
  var tickRange = function (entry, on) {
    var to = listRows.indexOf(entry);
    if (to < 0) { return; }
    var from = -1;
    listRows.forEach(function (r, i) { if (r.id === anchorId) { from = i; } });
    if (from < 0) { from = cursor >= 0 ? cursor : to; }
    for (var i = Math.min(from, to); i <= Math.max(from, to); i++) { tickEntry(listRows[i], on); }
    renderBulk();
  };
  // True when the click selected rather than opened, and has been dealt with.
  var selectingClick = function (event, entry) {
    if (!event || !(event.shiftKey || event.ctrlKey || event.metaKey)) { anchorId = entry.id; return false; }
    if (event.preventDefault) { event.preventDefault(); }
    if (event.shiftKey) { tickRange(entry, true); return true; }
    tickEntry(entry, !selected[entry.id]);
    anchorId = entry.id;
    renderBulk();
    return true;
  };
  var renderSearchNote = function (list, page) {
    if (!page.query) { return; }
    var note = 'Searched ' + page.scanned + ' message' + (page.scanned === 1 ? '' : 's') + ' for \'' + page.query + '\'.';
    if (page.more) { note += ' Only the newest are shown.'; }
    var row = node('div', note, 'listfoot');
    if (!page.complete && page.next_before_uid) {
      var older = button(t('Search older messages'));
      older.addEventListener('click', function () { state.before = page.next_before_uid; go(listHash()); });
      row.appendChild(older);
    }
    list.appendChild(row);
  };
  // ---- Archive, Junk and Trash, and conversations ---------------------------
  var folderIs = function (folderId, use) {
    var f = foldersById[folderId];
    return !!(f && f.special_use && f.special_use.indexOf(String.fromCharCode(92) + use) >= 0);
  };
  var junkTargetFor = function (folderId) { return folderIs(folderId, 'Junk') ? 'inbox' : 'junk'; };
  // What a filing did: a delete says where the message went, since the
  // route moves it to Trash when the account has one.
  var filingLabel = function (to, result) {
    if (to === 'delete') { return result && result.data && result.data.deleted ? 'Deleted.' : 'Moved to the trash folder.'; }
    return to === 'archive' ? 'Archived.' : to === 'junk' ? 'Filed as junk.' : to === 'inbox' ? 'Moved to the inbox.' : to === 'trash' ? 'Moved to the trash folder.' : 'Deleted.';
  };
  var fileMessage = function (id, to) {
    if (to === 'delete') { return call('DELETE', '/api/v1/me/messages/' + id); }
    if (typeof to === 'number') { return call('POST', '/api/v1/me/messages/' + id + '/move', { folder_id: to }); }
    return call('POST', '/api/v1/me/messages/' + id + '/move', { to: to });
  };
  var afterFiling = function (result, what) {
    if (result.status === 200) { say('mail-status', what, true); lastListing = null; loadFolders(); return true; }
    say('mail-status', describe(result, t('Could not file the message')), false);
    return false;
  };
  // A collapsed conversation is filed whole, one message after another,
  // as its tick box selects it whole.
  // A swipe's action, from the settings: archive, delete, junk, mark as read, or nothing.
  var swipeAction = function (what, entry) {
    if (what === 'archive' || what === 'delete') { fileRow(entry, what); }
    else if (what === 'junk') { fileRow(entry, junkTargetFor(state.folderId)); }
    else if (what === 'read') { markRows([entry], true); }
  };
  var fileRow = function (row, to) {
    if (!row) { return; }
    var entries = (row.ids || [row.id]).map(function (id) { return { id: id, folderId: row.folderId || state.folderId }; });
    var at = cursor;
    fileMany(entries, to).then(function (ok) {
      if (!ok) { return; }
      if (current && entries.some(function (e) { return e.id === current.id; })) { closeMessage(true); }
      var reloaded = loadMessages();
      if (reloaded && reloaded.then) { reloaded.then(function () { setCursor(Math.min(at, listRows.length - 1), true); }); }
    });
  };
  var expandedThreads = {};
  // The conversation a message belongs to: the first reference names the
  // root, then what it answers, then itself.
  var threadKey = function (m) {
    var refs = (m.references || '').trim().split(/\s+/).filter(Boolean);
    return refs.length ? refs[0] : ((m.in_reply_to || '').trim() || m.message_id || ('id:' + m.id));
  };
  // ---- Labels: IMAP keywords on the message, a colour of the reader's own.
  var labelColours = {};
  var labelNames = function () { return Object.keys(labelColours).sort(function (a, b) { return a.toLowerCase() < b.toLowerCase() ? -1 : a.toLowerCase() > b.toLowerCase() ? 1 : 0; }); };
  var colourKeyOf = function (name) { var lower = name.toLowerCase(); for (var k in labelColours) { if (k.toLowerCase() === lower) { return k; } } return null; };
  var colourOf = function (name) { var k = colourKeyOf(name); return k ? labelColours[k] : '#5b6875'; };
  var chip = function (name) { var c = node('span', name, 'chip'); if (c.style) { c.style.backgroundColor = colourOf(name); } return c; };
  // A label is a word of the reader's own. The keywords this page keeps for
  // itself begin with $ - the second and third stars, the follow-up and its
  // day, pinned, muted - and are not labels: they are shown by the thing
  // that owns them (the star, the follow-up button, the row's marks) and
  // changed only through it, so that a message never keeps one of them
  // under a star, a flag or a mark that no longer shows it.
  var ownKeyword = function (k) { return String(k).charAt(0) === '$'; };
  var chipsFor = function (keywords) { var box = node('span', undefined, 'chips'); (keywords || []).filter(function (k) { return !ownKeyword(k); }).forEach(function (k) { box.appendChild(chip(k)); }); return box; };
  // A $ is refused with the brackets and the quotes: the page's own keywords
  // begin with one, so a label named $Star3 would put a third star's keyword
  // on a message without the star, where nothing in this page would show it.
  var validLabel = function (name) { return /^[\x21-\x7e]+$/.test(name) && !/[()\{\}\[\]%*"\\$]/.test(name) && name.length <= 60; };
  var saveLabelColours = function () { return savePrefs({ labels: JSON.stringify(labelColours) }); };
  var renderLabelNav = function () {
    var nav = el('label-nav');
    clear(nav);
    var names = labelNames();
    if (!names.length) { return; }
    nav.appendChild(node('div', t('Labels'), 'nav-heading'));
    names.forEach(function (name) {
      var b = document.createElement('button');
      b.type = 'button';
      b.appendChild(chip(name));
      b.addEventListener('click', function () { go('/search?q=' + encodeURIComponent('label:' + name)); });
      nav.appendChild(b);
    });
  };
  var renderLabelSettings = function () {
    var rows = el('label-rows');
    clear(rows);
    var names = labelNames();
    names.forEach(function (name) {
      var row = node('div', undefined, 'frow');
      var left = node('div', undefined, 'fname');
      left.appendChild(chip(name));
      row.appendChild(left);
      var colour = document.createElement('input');
      colour.type = 'color';
      colour.value = labelColours[name];
      colour.setAttribute('aria-label', t('Colour of ') + name);
      colour.addEventListener('change', function () {
        labelColours[name] = colour.value;
        saveLabelColours().then(function (ok) { if (ok) { say('label-status', t('Colour saved.'), true); } });
      });
      var remove = button(t('Remove'), 'btn ghost sm danger');
      remove.addEventListener('click', function () {
        delete labelColours[name];
        saveLabelColours().then(function (ok) { if (ok) { say('label-status', t('Taken off the list; messages keep the keyword until it is taken off them.'), true); } });
      });
      row.appendChild(colour);
      row.appendChild(remove);
      rows.appendChild(row);
    });
    el('label-empty').hidden = names.length > 0;
  };
  var refreshLabelColours = function () {
    labelColours = {};
    try { var parsed = JSON.parse(pref('labels') || '{}'); if (parsed && typeof parsed === 'object') { labelColours = parsed; } } catch (e) { labelColours = {}; }
    renderLabelNav();
    renderLabelSettings();
  };
  el('label-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var name = el('label-name').value.trim();
    if (!validLabel(name)) { say('label-status', t('A label is one word: letters, digits and punctuation, no spaces, quotes or brackets.'), false); return; }
    labelColours[colourKeyOf(name) || name] = el('label-colour').value || '#36c2ff';
    saveLabelColours().then(function (ok) { if (ok) { el('label-name').value = ''; say('label-status', t('Added.'), true); } });
  });
  var renderMessageLabels = function () {
    var box = el('message-labels');
    clear(box);
    if (!current) { return; }
    (current.flags.keywords || []).filter(function (k) { return !ownKeyword(k); }).forEach(function (k) { box.appendChild(chip(k)); });
  };
  var setKeywords = function (add, remove) {
    if (!current) { return Promise.resolve(false); }
    return call('PUT', '/api/v1/me/messages/' + current.id + '/flags', { keywords_add: add, keywords_remove: remove }).then(function (result) {
      if (result.status === 200 && result.data && result.data.flags) {
        current.flags = result.data.flags;
        renderMessageLabels();
        renderLabelMenu();
        lastListing = null;
        if (remove.length && !add.length) {
          // Undo puts the label back on the same message, whatever is open by then.
          var was = current;
          toast(t('Label removed.'), function () {
            call('PUT', '/api/v1/me/messages/' + was.id + '/flags', { keywords_add: remove, keywords_remove: [] }).then(function (back) {
              if (back.status === 200 && back.data && back.data.flags && current && current.id === was.id) { current.flags = back.data.flags; renderMessageLabels(); renderLabelMenu(); }
              lastListing = null;
            });
          });
        }
        return true;
      }
      say('mail-status', describe(result, t('Could not change the labels')), false);
      return false;
    });
  };
  var renderLabelMenu = function () {
    var menu = el('label-menu');
    clear(menu);
    if (menu.hidden || !current) { return; }
    var have = {};
    (current.flags.keywords || []).forEach(function (k) { have[k.toLowerCase()] = true; });
    // The menu offers the reader's labels and any label already on the
    // message; the page's own $ keywords are left out of both, as the chips
    // and the message's labels leave them out. Offered here, the second and
    // third stars were a tick box beside the star itself: unticking $Star3
    // took the keyword off and left the flag, which drops the message to the
    // first star while the page goes on painting the third - the very state
    // this row exists to remove.
    var names = labelNames().filter(function (k) { return !ownKeyword(k); });
    (current.flags.keywords || []).forEach(function (k) { if (!ownKeyword(k) && !colourKeyOf(k)) { names.push(k); } });
    if (!names.length) { menu.appendChild(node('span', t('No labels yet - name one:'), 'muted')); }
    names.forEach(function (name) {
      var label = document.createElement('label');
      label.className = 'inline';
      var box = document.createElement('input');
      box.type = 'checkbox';
      box.checked = !!have[name.toLowerCase()];
      box.setAttribute('aria-label', name);
      box.addEventListener('change', function () { if (box.checked) { setKeywords([name], []); } else { setKeywords([], [name]); } });
      label.appendChild(box);
      label.appendChild(chip(name));
      menu.appendChild(label);
    });
    var input = document.createElement('input');
    input.type = 'text';
    input.id = 'label-new';
    input.setAttribute('maxlength', '60');
    input.setAttribute('placeholder', t('New label'));
    input.setAttribute('aria-label', t('New label'));
    var add = button(t('Add'), 'btn ghost sm');
    add.id = 'label-new-add';
    add.addEventListener('click', function () {
      var name = input.value.trim();
      if (!validLabel(name)) { say('mail-status', t('A label is one word: letters, digits and punctuation, no spaces, quotes or brackets.'), false); return; }
      if (!colourKeyOf(name)) { labelColours[name] = '#36c2ff'; saveLabelColours(); }
      setKeywords([name], []);
    });
    menu.appendChild(input);
    menu.appendChild(add);
  };
  var toggleLabelMenu = function () { var menu = el('label-menu'); menu.hidden = !menu.hidden; renderLabelMenu(); };
  el('message-label').addEventListener('click', toggleLabelMenu);

  var messageRow = function (m, inThread) {
    var row = node('div', undefined, 'msg' + (m.flags.seen ? '' : ' unseen') + (inThread ? ' in-thread' : ''));
    row.setAttribute('role', 'listitem');
    paintRow(row, colourFor(m));
    var folderId = m.folder_id || state.folderId;
    var entry = { id: m.id, row: row, m: m, folderId: folderId };
    if (isPinned(m)) { row.classList.add('pinned'); }
    rowGestures(row, entry);
    var cb = node('span', undefined, 'cb');
    var box = document.createElement('input'); box.type = 'checkbox'; box.checked = !!selected[m.id]; box.setAttribute('aria-label', t('Select'));
    box.addEventListener('click', function (event) { event.stopPropagation(); if (event.shiftKey) { tickRange(entry, box.checked); return; } selected[m.id] = box.checked; anchorId = m.id; renderBulk(); });
    cb.appendChild(box);
    row.appendChild(cb);
    entry.box = box;
    if (box.checked) { row.classList.add('selected'); }
    entry.star = starButton(m);
    row.appendChild(entry.star);
    row.appendChild(avatarFor(m.from, false, m.brand));
    var outgoing = folderIs(folderId, 'Sent') || folderIs(folderId, 'Drafts');
    var who = node('div', undefined, 'who');
    // The marker stands at the head of the sender, and the sender's column is
    // widened for it, so that the name is no shorter than on a row without one.
    if (markable(folderId)) { entry.imp = importanceButton([m]); who.appendChild(entry.imp); row.classList.add('has-imp'); }
    who.appendChild(node('span', outgoing ? t('To: ') + (recipientsOf(m.to).slice(0, 2).join(', ') || '?') : nameOf(m.from)));
    who.setAttribute('title', (outgoing ? m.to : m.from) || '');
    row.appendChild(who);
    var what = node('div', undefined, 'what');
    what.appendChild(node('span', m.subject || t('(no subject)'), 'subject'));
    if (m.flags && m.flags.keywords && m.flags.keywords.length) { what.appendChild(chipsFor(m.flags.keywords)); }
    if (m.folder && state.everywhere) { what.appendChild(node('span', m.folder, 'badge')); }
    var nudge = nudgeFor(m, folderId);
    if (nudge) { entry.nudge = node('span', nudge, 'nudge'); what.appendChild(entry.nudge); }
    if (mentionsReader(m)) { var at = node('span', '@', 'mention'); at.setAttribute('title', t('Mentions you')); what.appendChild(at); }
    what.appendChild(node('span', m.snippet || '', 'snip'));
    row.appendChild(what);
    var meta = node('div', undefined, 'meta');
    if (m.has_attachments) { var clip = node('span', undefined, 'clip'); clip.setAttribute('title', t('Has attachments')); clip.appendChild(icon('clip', true)); meta.appendChild(clip); }
    if (isMuted(m)) { meta.appendChild(muteMark()); }
    var due = dueMark(m);
    if (due) { meta.appendChild(due); }
    var when = node('span', whenText(m), 'when'); when.setAttribute('title', fullDate(m)); meta.appendChild(when);
    row.appendChild(meta);
    var acts = node('div', undefined, 'acts');
    var act = function (name, title, fn) { var b = iconButton(name, title); b.addEventListener('click', function (e) { e.stopPropagation(); fn(); }); acts.appendChild(b); };
    act('archive', t('Archive'), function () { fileRow(entry, 'archive'); });
    act('trash', t('Delete'), function () { fileRow(entry, 'delete'); });
    act(m.flags.seen ? 'mail' : 'mail-open', m.flags.seen ? t('Mark as unread') : t('Mark as read'), function () { markRows([entry], !m.flags.seen); });
    row.appendChild(acts);
    row.addEventListener('click', function (event) { if (selectingClick(event, entry)) { return; } go('/m/' + m.id); });
    // On a touch screen: a swipe to the right archives, to the left deletes.
    var touchX = null;
    row.addEventListener('touchstart', function (e) { touchX = e.touches && e.touches.length === 1 ? e.touches[0].clientX : null; }, { passive: true });
    row.addEventListener('touchend', function (e) {
      if (touchX === null || !e.changedTouches || !e.changedTouches.length) { return; }
      var dx = e.changedTouches[0].clientX - touchX;
      touchX = null;
      if (dx > 90) { swipeAction(pref('swipe_right'), entry); } else if (dx < -90) { swipeAction(pref('swipe_left'), entry); }
    }, { passive: true });
    listRows.push(entry);
    return row;
  };
  // A folder of thousands: the first chunk is on the screen at once, the rest
  // arrives frame by frame while the reader is already reading.
  var CHUNK = 150;
  var chunkedAppend = function (items, list, make) {
    var at = 0;
    var frame = typeof window.requestAnimationFrame === 'function' ? window.requestAnimationFrame : null;
    var step = function () {
      var end = frame ? Math.min(items.length, at + CHUNK) : items.length;
      for (; at < end; at++) { list.appendChild(make(items[at])); }
      if (at < items.length) { frame(step); }
    };
    step();
  };
  var renderRows = function (page, list, shown) {
    // The Starred view puts the dated follow-ups first, the soonest at the
    // top; elsewhere the pinned messages come first.
    var ordered = state.everywhere && state.query === 'is:flagged' ? byDue(page.messages) : page.messages.filter(isPinned).concat(page.messages.filter(function (m) { return !isPinned(m); }));
    if (pref('view') !== 'threads' || page.query) {
      ordered.forEach(function (m) { shown[m.id] = true; });
      chunkedAppend(ordered, list, function (m) { return messageRow(m, false); });
      return;
    }
    // A null prototype: a References header spelling 'constructor' is a key, not a property.
    var groups = []; var byKey = Object.create(null);
    ordered.forEach(function (m) {
      shown[m.id] = true;
      var key = threadKey(m);
      if (!byKey[key]) { byKey[key] = { key: key, messages: [] }; groups.push(byKey[key]); }
      byKey[key].messages.push(m);
    });
    groups.forEach(function (g) {
      if (g.messages.length === 1) { list.appendChild(messageRow(g.messages[0], false)); return; }
      var newest = g.messages[0];
      var unseen = g.messages.some(function (m) { return !m.flags.seen; });
      var row = node('div', undefined, 'msg thread' + (unseen ? ' unseen' : ''));
      row.setAttribute('role', 'listitem');
      paintRow(row, g.messages.map(colourFor).filter(function (c) { return c; })[0] || '');
      var folderId = newest.folder_id || state.folderId;
      var entry = { id: newest.id, ids: g.messages.map(function (m) { return m.id; }), ms: g.messages, row: row, m: newest, folderId: folderId };
      if (g.messages.some(isPinned)) { row.classList.add('pinned'); }
      rowGestures(row, entry);
      var cb = node('span', undefined, 'cb');
      var box = document.createElement('input'); box.type = 'checkbox'; box.setAttribute('aria-label', t('Select the conversation'));
      box.checked = g.messages.every(function (m) { return !!selected[m.id]; });
      box.addEventListener('click', function (event) { event.stopPropagation(); if (event.shiftKey) { tickRange(entry, box.checked); return; } g.messages.forEach(function (m) { selected[m.id] = box.checked; }); anchorId = newest.id; renderBulk(); });
      cb.appendChild(box); row.appendChild(cb);
      entry.box = box;
      entry.star = starButton(newest);
      row.appendChild(entry.star);
      row.appendChild(avatarFor(newest.from, false, newest.brand));
      var senders = [];
      g.messages.forEach(function (m) { var s = nameOf(m.from); if (senders.indexOf(s) < 0) { senders.push(s); } });
      var who = node('div', undefined, 'who');
      if (markable(folderId)) { entry.imp = importanceButton(g.messages); who.appendChild(entry.imp); row.classList.add('has-imp'); }
      who.appendChild(node('span', senders.slice(0, 3).join(', ') + (senders.length > 3 ? ', ...' : '')));
      var count = node('span', String(g.messages.length), 'tc');
      count.setAttribute('title', t('Show every message of the conversation'));
      who.appendChild(count);
      row.appendChild(who);
      var what = node('div', undefined, 'what');
      what.appendChild(node('span', newest.subject || t('(no subject)'), 'subject'));
      var keywords = [];
      g.messages.forEach(function (m) { (m.flags.keywords || []).forEach(function (k) { if (keywords.indexOf(k) < 0) { keywords.push(k); } }); });
      if (keywords.length) { what.appendChild(chipsFor(keywords)); }
      var nudge = nudgeFor(newest, folderId);
      if (nudge) { entry.nudge = node('span', nudge, 'nudge'); what.appendChild(entry.nudge); }
      what.appendChild(node('span', newest.snippet || '', 'snip'));
      row.appendChild(what);
      var meta = node('div', undefined, 'meta');
      if (g.messages.some(function (m) { return m.has_attachments; })) { var clip = node('span', undefined, 'clip'); clip.appendChild(icon('clip', true)); meta.appendChild(clip); }
      if (g.messages.some(isMuted)) { meta.appendChild(muteMark()); }
      var soonest = soonestDue(g.messages);
      if (soonest) { meta.appendChild(dueMark(soonest)); }
      var when = node('span', whenText(newest), 'when'); when.setAttribute('title', fullDate(newest)); meta.appendChild(when);
      row.appendChild(meta);
      var acts = node('div', undefined, 'acts');
      var act = function (name, title, fn) { var b = iconButton(name, title); b.addEventListener('click', function (e) { e.stopPropagation(); fn(); }); acts.appendChild(b); };
      act('archive', t('Archive'), function () { fileRow(entry, 'archive'); });
      act('trash', t('Delete'), function () { fileRow(entry, 'delete'); });
      act(unseen ? 'mail-open' : 'mail', unseen ? t('Mark as read') : t('Mark as unread'), function () { markRows([entry], unseen); });
      row.appendChild(acts);
      var toggle = function () {
        expandedThreads[g.key] = !expandedThreads[g.key];
        if (!lastListing) { return; }
        renderMessages(lastListing.page);
        listRows.forEach(function (r, i) { if (r.toggle && r.id === newest.id) { setCursor(i, true); } });
      };
      count.addEventListener('click', function (event) { event.stopPropagation(); toggle(); });
      row.addEventListener('click', function (event) { if (selectingClick(event, entry)) { return; } go('/m/' + newest.id); });
      entry.toggle = toggle;
      listRows.push(entry);
      list.appendChild(row);
      if (expandedThreads[g.key]) {
        // Oldest first inside the conversation, newest last, as it is read.
        g.messages.slice().reverse().forEach(function (m) { list.appendChild(messageRow(m, true)); });
      }
    });
  };
  var lastPage = null;
  var renderMessages = function (page) {
    var list = el('message-list');
    clear(list);
    list.classList.remove('skeleton');
    listRows = []; cursor = -1;
    lastPage = page;
    judgeListing(page);
    renderTabs(page);
    var tabbed = tabsActive(page);
    var visible = tabbed ? page.messages.filter(function (m) { return tabOf(m) === activeTab; }) : page.messages;
    if (!visible.length) {
      selected = {}; renderBulk(); renderCount(page);
      var empty = node('div', undefined, 'empty');
      empty.appendChild(icon('empty'));
      empty.appendChild(node('div', page.query ? t('Nothing matched.') : tabbed && page.messages.length ? tf('Nothing under {0}.', tabName(activeTab)) : t('This folder is empty.')));
      list.appendChild(empty);
      renderSearchNote(list, page);
      return;
    }
    var shown = {};
    (sectionsActive(page) ? renderSections : renderRows)(tabbed ? { messages: visible, query: page.query } : page, list, shown);
    Object.keys(selected).forEach(function (k) { if (!shown[k]) { delete selected[k]; } });
    setCursor(0, true);
    renderBulk();
    renderCount(page);
    renderSearchNote(list, page);
    if (current) { renderConversation(current, page); }
    if (current) { listRows.forEach(function (r) { r.row.classList.toggle('open', r.id === current.id || (r.ids || []).indexOf(current.id) >= 0); }); }
  };
  var listingKey = function () { return state.everywhere + '|' + state.folderId + '|' + state.query + '|' + state.before; };
  // No connection: the inbox listing the last visit saved, else whatever the
  // offline store holds for this folder - or, for a search, the kept messages
  // whose subject, sender or text carry the words.
  var loadMessagesOffline = function (result) {
    if (result.status !== 0) { return false; }
    if (!state.everywhere && !state.query && !state.before && state.folderId === inboxId) {
      var saved = offlineRead('list');
      // Drawn from this browser's own store, not from the server: what reads
      // a listing must be able to tell, because nothing can be written back.
      if (saved) { saved.offline = true; offlineNote(true); renderMessages(saved); return true; }
    }
    if (state.before) { return false; }
    offlineListing().then(function (page) {
      if (page) { page.offline = true; offlineNote(true); renderMessages(page); }
      else { say('mail-status', describe(result, t('Could not read the folder')), false); }
    });
    return true;
  };
  var loadMessages = function () {
    var url;
    if (state.query && state.everywhere) {
      url = '/api/v1/me/search?q=' + encodeURIComponent(state.query) + '&limit=200';
    } else {
      if (!state.folderId) { renderMessages({ total: 0, messages: [] }); return Promise.resolve(); }
      url = '/api/v1/me/folders/' + state.folderId + '/messages';
      if (state.query) { url += '?q=' + encodeURIComponent(state.query) + (state.before ? '&before_uid=' + state.before : ''); }
      else if (state.before) { url += '?before_uid=' + state.before; }
    }
    return call('GET', url).then(function (result) {
      if (result.status === 200 && result.data) {
        renderMessages(result.data);
        lastListing = { key: listingKey(), page: result.data };
        askReplies(result.data);
        if (!state.everywhere && !state.query && !state.before && state.folderId === inboxId) { offlineKeep('list', result.data); }
        return;
      }
      if (loadMessagesOffline(result)) { return; }
      say('mail-status', describe(result, t('Could not read the folder')), false);
    });
  };
  // A change the probe saw, folded into what is on screen without moving the
  // reader: the same listing is fetched again, then the cursor is put back on
  // the message it was on (by id, not by index - rows may have arrived above
  // it), the ticked boxes are put back, and the scroll position is restored.
  var reloadKeepingPlace = function () {
    var onId = cursor >= 0 && listRows[cursor] ? listRows[cursor].id : 0;
    var ticked = selected;
    var scroll = el('message-list').scrollTop;
    return loadMessages().then(function () {
      selected = {};
      Object.keys(ticked).forEach(function (k) { if (ticked[k] && listRows.filter(function (r) { return String(r.id) === String(k); }).length) { selected[k] = true; } });
      listRows.forEach(function (r) { r.box.checked = !!selected[r.id]; });
      renderBulk();
      var at = -1;
      listRows.forEach(function (r, i) { if (r.id === onId) { at = i; } });
      if (at >= 0) { setCursor(at, true); }
      el('message-list').scrollTop = scroll;
    });
  };

  // ---- One message --------------------------------------------------------
  var current = null;
  var showHtml = false;
  var cameFromList = false;
  // An image the sender embedded: the HTML part points at it as cid:<id>, and
  // the attachment carrying that Content-ID is somewhere in the same message.
  //
  // It cannot be pointed at the attachment's download route, which is the
  // obvious answer and the wrong one. The frame is sandboxed without
  // allow-same-origin, so its document has an origin of its own; a browser
  // therefore counts a request it makes as cross-site, and the session cookie
  // is SameSite=Strict, so the cookie does not go and the route answers 401.
  // Measured in Chrome on 9 September 2026 through a logging proxy: the same
  // frame without the sandbox attribute sent the cookie and got 200 image/png,
  // and with it sent no cookie and got 401. Weakening the sandbox to make the
  // URL work would give the sender's markup this account's own origin, which
  // is the whole thing the frame exists to prevent.
  //
  // So the page fetches the attachment itself, with its own credentials, and
  // hands the frame the bytes as a data: URL - which its policy already allows
  // and which travels no further. The type is the one the SERVER declared on
  // the download, not the one the message claimed: the download route serves
  // HTML, SVG, XML and script as application/octet-stream on purpose, and
  // anything that does not come back as an image is left alone, so the
  // reference stays a cid: that renders as nothing and the attachment is still
  // in the list below, named. Remote images are untouched and the policy still
  // refuses them, so opening a message still tells its sender nothing.
  var RENDERS = { 'image/png': 1, 'image/jpeg': 1, 'image/gif': 1, 'image/webp': 1, 'image/bmp': 1, 'image/x-icon': 1, 'image/vnd.microsoft.icon': 1 };
  var INLINE_EACH = 4 * 1024 * 1024;
  var INLINE_TOGETHER = 12 * 1024 * 1024;
  var INLINE_MOST = 12;
  var inlineFor = { id: 0, images: null };
  var contentIdOf = function (a) { return String(a.content_id || '').replace(/^</, '').replace(/>$/, '').trim(); };
  var fetchInline = function (message) {
    if (inlineFor.id === message.id && inlineFor.images) { return Promise.resolve(inlineFor.images); }
    var budget = INLINE_TOGETHER;
    var wanted = (message.attachments || []).filter(function (a) {
      var type = String(a.content_type || '').toLowerCase();
      if (!contentIdOf(a) || (type && !RENDERS[type]) || a.size > INLINE_EACH || budget < a.size) { return false; }
      budget -= a.size;
      return true;
    }).slice(0, INLINE_MOST);
    if (!wanted.length) { inlineFor = { id: message.id, images: {} }; return Promise.resolve(inlineFor.images); }
    return Promise.all(wanted.map(function (a) {
      return apiFetch('/api/v1/me/messages/' + message.id + '/attachments/' + a.index, { cache: 'no-store', credentials: 'same-origin' }).then(function (response) {
        if (response.status !== 200) { return null; }
        return response.blob();
      }).then(function (blob) {
        if (!blob || String(blob.type || '').indexOf('image/') !== 0) { return null; }
        return new Promise(function (resolve) {
          var reader = new FileReader();
          reader.onload = function () { resolve({ id: contentIdOf(a).toLowerCase(), url: String(reader.result) }); };
          reader.onerror = function () { resolve(null); };
          reader.readAsDataURL(blob);
        });
      }, function () { return null; });
    })).then(function (found) {
      var images = {};
      found.forEach(function (one) { if (one && one.url.indexOf('data:image/') === 0) { images[one.id] = one.url; } });
      inlineFor = { id: message.id, images: images };
      return images;
    });
  };
  var resolveInline = function (html, images) {
    if (!Object.keys(images).length) { return html; }
    return html.replace(/(["'(=])\s*cid:([^"'()<>\s]+)/gi, function (whole, lead, reference) {
      var key = '';
      try { key = decodeURIComponent(reference).toLowerCase(); } catch (e) { key = reference.toLowerCase(); }
      if (!(key in images)) { key = reference.toLowerCase(); }
      if (!(key in images)) { return whole; }
      return lead + images[key];
    });
  };
  // HTML as it was sent: in a frame with an origin of its own, where
  // nothing runs, no form is submitted and no address is rewritten - a
  // document the server serves under a policy of its own (a srcdoc frame
  // would inherit this page's, which allows no remote image at all).
  // Remote images, fonts and styles stay blocked unless the reader asked
  // for this message, or for this sender, to show them; the server says
  // whether the part names any (html_remote), so the note is right.
  // What the remote-image block kept out, counted from the HTML as sent: the
  // images that point at a host, the hosts, and the ones that look like
  // tracking pixels - a pixel or less in size, hidden, or named as one. The
  // sender's habit is the number of their messages seen carrying pixels, kept
  // in this browser, each message counted once.
  var countRemote = function (html) {
    var out = { images: 0, hosts: {}, pixels: 0 };
    var tags = String(html || '').match(/<img\b[^>]*>/gi) || [];
    tags.forEach(function (tag) {
      var src = (tag.match(/\bsrc\s*=\s*["']?\s*(https?:\/\/[^"'\s>]+)/i) || [])[1];
      if (!src) { return; }
      out.images++;
      var host = (src.match(/^https?:\/\/([^\/?#]+)/i) || [])[1];
      if (host) { out.hosts[host.toLowerCase()] = true; }
      var w = (tag.match(/\bwidth\s*=\s*["']?\s*(\d+)/i) || [])[1];
      var h = (tag.match(/\bheight\s*=\s*["']?\s*(\d+)/i) || [])[1];
      var tiny = (w !== undefined && Number(w) <= 1) || (h !== undefined && Number(h) <= 1) || /width\s*:\s*[01]px|height\s*:\s*[01]px/i.test(tag);
      var hidden = /display\s*:\s*none|visibility\s*:\s*hidden/i.test(tag);
      var named = /pixel|track|beacon|open\.gif|\/o\.gif|\/open\b|\/img\/open|spacer/i.test(src);
      if (tiny || hidden || named) { out.pixels++; }
    });
    (String(html || '').match(/url\(\s*["']?https?:\/\/[^)"']+/gi) || []).forEach(function (u) {
      out.images++;
      var host = (u.match(/https?:\/\/([^\/?#"')]+)/i) || [])[1];
      if (host) { out.hosts[host.toLowerCase()] = true; }
    });
    out.hostCount = Object.keys(out.hosts).length;
    return out;
  };
  var TRACKERS = 'hmPortalTrackers';
  var trackerHabit = function (sender, messageId, pixels) {
    // What this browser learns about senders is the first account's, as
    // everything it keeps is: nothing is read or written for the second
    // account, whose messages are shown without the remote-image bar anyway.
    if (!offlineMine()) { return 0; }
    var record = {};
    try { record = JSON.parse(localStorage.getItem(TRACKERS) || '{}') || {}; } catch (e) { record = {}; }
    var entry = record[sender] || { count: 0, seen: [] };
    if (pixels && entry.seen.indexOf(messageId) < 0) {
      entry.count++; entry.seen.push(messageId);
      if (entry.seen.length > 50) { entry.seen = entry.seen.slice(-50); }
      record[sender] = entry;
      try { localStorage.setItem(TRACKERS, JSON.stringify(record)); } catch (e) { /* a browser without storage forgets */ }
    }
    return entry.count;
  };
  var describeRemote = function (showing, sender) {
    var counted = countRemote(showing.html);
    el('message-remote-count').textContent = counted.images ? tf('{0} remote image(s) from {1} host(s) were not loaded; {2} look like tracking pixels.', counted.images, counted.hostCount, counted.pixels) : '';
    var habit = trackerHabit(sender, showing.id, counted.pixels);
    el('message-remote-habit').textContent = habit ? tf('This sender has used tracking pixels in {0} message(s).', habit) : '';
  };
  // The one blob URL the reading frame may be showing - a second account's
  // document, fetched with its token - held so that it is released when the
  // frame moves on: to the next document, to the route, or to nothing. A
  // blob URL not revoked keeps its bytes for the life of the page.
  var htmlBlobUrl = '';
  var htmlBlobRevoke = function () {
    if (!htmlBlobUrl) { return; }
    try { URL.revokeObjectURL(htmlBlobUrl); } catch (e) { /* gone already */ }
    htmlBlobUrl = '';
  };
  var renderHtml = function () {
    var frame = el('message-html');
    if (!current || !current.html || !showHtml) { frame.hidden = true; frame.removeAttribute('src'); htmlBlobRevoke(); el('message-text').hidden = false; el('message-remote-second').hidden = true; return Promise.resolve(); }
    var showing = current;
    var sender = addressOf(showing.from).toLowerCase();
    var allowed = remoteAllowed.id === showing.id || remoteSenders().indexOf(sender) >= 0;
    if (onSecond()) {
      // A frame's own request carries the cookie, which is the first account's,
      // so the second account's document is fetched here with its token and
      // shown from a blob. A document shown that way is under this page's own
      // policy as well as its own (the server writes the policy into the
      // document too), and this page allows no remote image - so none is
      // offered: the bar says so instead of the two buttons.
      el('message-remote').hidden = true;
      el('message-remote-second').hidden = !showing.html_remote;
      frame.hidden = false;
      el('message-text').hidden = true;
      return apiFetch('/api/v1/me/messages/' + showing.id + '/html', { cache: 'no-store', credentials: 'same-origin' }).then(function (response) {
        if (response.status !== 200) { return null; }
        return response.text();
      }, function () { return null; }).then(function (text) {
        if (current !== showing || !showHtml) { return; }
        if (text === null) { frame.hidden = true; frame.removeAttribute('src'); htmlBlobRevoke(); el('message-text').hidden = false; return; }
        htmlBlobRevoke();
        htmlBlobUrl = URL.createObjectURL(new Blob([text], { type: 'text/html' }));
        frame.setAttribute('src', htmlBlobUrl);
      });
    }
    el('message-remote-second').hidden = true;
    el('message-remote').hidden = !(showing.html_remote && !allowed);
    if (!el('message-remote').hidden) { describeRemote(showing, sender); }
    frame.setAttribute('src', '/api/v1/me/messages/' + showing.id + '/html' + (allowed ? '?remote=1' : ''));
    htmlBlobRevoke();
    frame.hidden = false;
    el('message-text').hidden = true;
    return Promise.resolve();
  };
  var renderActions = function () {
    if (!current) { return; }
    var seen = current.flags.seen;
    var unread = el('message-unread');
    unread.setAttribute('title', seen ? t('Mark as unread') : t('Mark as read'));
    unread.setAttribute('aria-label', seen ? t('Mark as unread') : t('Mark as read'));
    setIcon(unread, seen ? 'mail' : 'mail-open');
    paintStar(el('message-flag'), starStage(current));
    var follow = el('message-followup');
    var dueDay = dueOf(current);
    follow.classList.toggle('on', hasFollowUp(current));
    follow.setAttribute('title', dueDay ? tf('Due {0}', dueText(dueDay)) : t('Follow up'));
    follow.setAttribute('aria-label', dueDay ? tf('Due {0}', dueText(dueDay)) : t('Follow up'));
    el('followup-clear').hidden = !hasFollowUp(current);
    el('followup-menu').hidden = true;
    el('label-menu').hidden = true;
    renderMessageLabels();
    el('message-edit').hidden = !current.flags.draft;
    el('message-pin').textContent = isPinned(current) ? t('Unpin') : t('Pin');
    el('message-mute').textContent = isMuted(current) ? t('Unmute') : t('Mute');
    el('message-important').textContent = isImportant(current) ? t('Not important') : t('Mark as important');
    el('message-important').hidden = !markable(current.folder_id);
    var junk = el('message-junk');
    var isJunk = folderIs(current.folder_id, 'Junk');
    junk.setAttribute('title', isJunk ? t('Not junk') : t('Junk'));
    junk.setAttribute('aria-label', isJunk ? t('Not junk') : t('Junk'));
    fillFolderSelect(el('message-move'), current.folder_id);
    listRows.forEach(function (r) { r.row.classList.toggle('open', r.id === current.id || (r.ids || []).indexOf(current.id) >= 0); });
  };
  var renderMessage = function (m) {
    el('mail-section').classList.add('has-message');
    el('message-view').hidden = false;
    el('pane-empty').hidden = true;
    closeMenus();
    el('message-subject').textContent = m.subject || t('(no subject)');
    var avatar = el('message-avatar');
    clear(avatar);
    avatar.removeAttribute('title');
    avatar.textContent = initialsOf(m.from);
    if (avatar.style) { avatar.style.background = avatarColour(addressOf(m.from)); }
    brandInto(avatar, m.from, m.brand);
    el('message-from-name').textContent = nameOf(m.from);
    var address = addressOf(m.from);
    el('message-from-address').textContent = address && address !== nameOf(m.from) ? '<' + address + '>' : '';
    var to = recipientsOf(m.to);
    el('message-to-line').textContent = t('to ') + (to.length ? to.slice(0, 3).join(', ') + (to.length > 3 ? ', ...' : '') : t('me'));
    el('message-date').textContent = fullDate(m);
    el('message-meta').textContent = 'From ' + (m.from || '?') + (m.to ? ' to ' + m.to : '') + (m.cc ? ', cc ' + m.cc : '') + ' - ' + (m.date || m.received);
    var details = el('message-details');
    clear(details);
    details.hidden = true;
    el('message-details-toggle').setAttribute('aria-expanded', 'false');
    [['From', m.from], ['To', m.to], ['Cc', m.cc], ['Date', m.date || m.received], ['Message-ID', m.message_id]].forEach(function (pair) {
      if (!pair[1]) { return; }
      details.appendChild(node('span', t(pair[0])));
      details.appendChild(node('span', pair[1]));
    });
    // The verdicts a reader learns to look at, the sender's domain against
    // the account's, the headers on request, and the file itself.
    var badges = el('message-badges');
    clear(badges);
    var auth = m.authentication || {};
    ['spf', 'dkim', 'dmarc'].forEach(function (k) {
      var v = auth[k] || '';
      if (!v) { return; }
      var cls = v === 'pass' ? 'good' : (v === 'none' || v === 'neutral' || v === 'temperror') ? 'muted' : 'bad';
      badges.appendChild(node('span', k.toUpperCase() + ' ' + v, 'badge ' + cls));
    });
    if (m.external) { badges.appendChild(node('span', t('External sender'), 'badge warn')); }
    el('message-remote').hidden = true;
    warnAbout(m);
    previewAttachments(m);
    el('message-headers').textContent = m.headers || '';
    el('message-headers').hidden = true;
    el('message-headers-toggle').textContent = t('Headers');
    el('message-headers-toggle').hidden = !m.headers;
    el('message-receipt').hidden = !(m.receipt_requested_by && pref('receipts') !== 'never');
    el('snooze-menu').hidden = true;
    el('followup-menu').hidden = true;
    el('message-unsubscribe').hidden = !m.list_unsubscribe;
    el('message-source').setAttribute('href', '/api/v1/me/messages/' + m.id + '/source');
    el('message-source').setAttribute('download', 'message-' + m.id + '.eml');
    var text = m.text || '';
    if (!text && m.html) {
      // An HTML-only message is read as a document and only its text is
      // shown: nothing in it runs, loads or renders.
      text = new DOMParser().parseFromString(m.html, 'text/html').body.textContent || '';
    }
    if (m.truncated) { text = t('This message is too large to show here; open it in your mail program.') + ((m.attachments || []).length ? t(' Its attachments can be downloaded below.') : ''); }
    el('message-text').textContent = text || t('(no text)');
    var attachments = el('message-attachments');
    clear(attachments);
    (m.attachments || []).forEach(function (a) {
      var link = node('a', undefined, 'att');
      link.href = '/api/v1/me/messages/' + m.id + '/attachments/' + a.index;
      link.setAttribute('download', a.name);
      guardDownload(link);
      link.setAttribute('title', a.name + ' (' + (a.content_type ? a.content_type + ', ' : '') + format(a.size) + ')');
      link.appendChild(icon('file'));
      var words = node('div');
      words.appendChild(node('div', a.name, 'n'));
      words.appendChild(node('div', format(a.size) + (a.content_type ? ' \u00b7 ' + a.content_type : '') + (a.content_id ? ' \u00b7 ' + t('in the message') : ''), 's'));
      link.appendChild(words);
      attachments.appendChild(link);
    });
    current = m;
    renderConversation(m);
    renderActions();
    smimeInspect(m);
    pgpInspect(m);
    el('message-html-toggle').hidden = !m.html || m.truncated;
    renderHtml();
    renderInvitation(m);
    if (el('message-body').scrollTop) { el('message-body').scrollTop = 0; }
  };
  // The message after the cursor is fetched while the reader is still on
  // this one, when the browser has an idle moment for it; opening it then
  // costs nothing. Never in the small DOM the page is tested in, which has
  // no idle moments to offer.
  var prefetched = {};
  var prefetchNext = function () {
    var idle = window.requestIdleCallback || window.requestAnimationFrame;
    if (typeof idle !== 'function' || cursor < 0 || !listRows[cursor + 1]) { return; }
    var id = listRows[cursor + 1].id;
    if (prefetched[id] || (current && current.id === id)) { return; }
    idle(function () {
      call('GET', '/api/v1/me/messages/' + id).then(function (result) {
        if (result.status === 200 && result.data) {
          prefetched[id] = result.data;
          var keys = Object.keys(prefetched);
          if (keys.length > 6) { delete prefetched[keys[0]]; }
        }
      });
    });
  };
  var openMessage = function (id) {
    say('mail-status', '', true);
    if (current && current.id === id) { renderMessage(current); return Promise.resolve(current); }
    var show = function (data) {
      showHtml = false;
      el('message-html-toggle').textContent = 'Show as sent';
      renderMessage(data);
      learnOpened(data);
      if (!data.flags.seen) { setFlags({ seen: true }, true); }
      offlineKeep('m' + data.id, data);
      offlineKeepMessage(data);
      return data;
    };
    if (prefetched[id]) { var ready = prefetched[id]; delete prefetched[id]; return Promise.resolve(show(ready)); }
    return call('GET', '/api/v1/me/messages/' + id).then(function (result) {
      if (result.status === 200 && result.data) { return show(result.data); }
      if (result.status !== 0) { say('mail-status', describe(result, t('Could not open the message')), false); return null; }
      // No connection: one of the last opened, else one the offline store holds.
      var saved = offlineRead('m' + id);
      if (saved) { offlineNote(true); return show(saved); }
      return offlineMessage(id).then(function (kept) {
        if (kept) { offlineNote(true); return show(kept); }
        say('mail-status', describe(result, t('Could not open the message')), false);
        return null;
      });
    });
  };
  var setFlags = function (flags, quiet) {
    if (!current) { return; }
    var id = current.id;
    return call('PUT', '/api/v1/me/messages/' + id + '/flags', flags).then(function (result) {
      if (result.status === 200 && result.data) {
        if (current && current.id === id) { current.flags = result.data.flags; renderActions(); }
        if (offline.days) { offlineFlagsSync({ id: id, flags: result.data.flags, folder_id: 0 }); }
        loadFolders();
        return;
      }
      if (!quiet) { say('mail-status', describe(result, t('Could not change the flags')), false); }
    });
  };

  // ---- Compose ------------------------------------------------------------
  var replyTo = null;
  var draftId = 0;
  var carried = [];
  var composeKey = '';
  var identities = [];
  var signature = { enabled: false, text: '' };
  var addressOf = function (header) {
    var m = /<([^>]+)>/.exec(header || '');
    return m ? m[1] : (header || '').trim();
  };
  var textOf = function (message) {
    var text = message.text || '';
    if (!text && message.html) { text = new DOMParser().parseFromString(message.html, 'text/html').body.textContent || ''; }
    return text;
  };
  var quoted = function (message) {
    return textOf(message).split('\n').map(function (line) { return '> ' + line; }).join('\n');
  };
  // A new message's key is its own, so a send queued from one form is
  // never mistaken for the next message written in it.
  var newComposeKey = function () { return 'new:' + Date.now(); };
  var blankCompose = function () {
    // A send waiting on the undo delay goes now rather than being lost
    // with the form it was written in.
    flushPending();
    // Formatting off before the fields are emptied, not after: switching it
    // off writes the editor's words into the text box, which would otherwise
    // be what the next message started with.
    if (richOn) { setRich(false); }
    ['compose-to', 'compose-cc', 'compose-bcc', 'compose-subject', 'compose-text'].forEach(function (id) { el(id).value = ''; });
    carried = []; replyTo = null; draftId = 0; dropped = [];
    el('compose-receipt').checked = false;
    el('compose-expires').value = '0'; el('compose-passcode').value = ''; syncExpiry();
    el('compose-sign').checked = false; el('compose-encrypt').checked = false; smimeComposeHints();
    el('compose-pgp-sign').checked = false; el('compose-pgp-encrypt').checked = false; pgpComposeHints();
    selectIdentity('');
    el('compose-files').value = ''; el('compose-files-note').textContent = '';
    resetLinked();
    say('compose-status', '', true);
  };
  // The Content-IDs this webmail's own images are given when a message is
  // written: img<n>.<32 hex>@<host> by the server (InlineImageParts.cpp) and
  // img<n>.<16 hex>@webmail.invalid by the S/MIME module (PortalSmime.js).
  var OWN_IMAGE_ID = /^img\d+\.(?:[0-9a-f]{16}@webmail\.invalid|[0-9a-f]{32}@)/i;
  var carryAttachments = function (message, embeddedStay) {
    carried = [];
    // A draft reopens as its text, so the images this webmail put into its
    // HTML - a signature's logo, an image pasted in, sent as cid: parts - are
    // not files to attach again: before they were parts they were data: URLs
    // in that HTML, which a draft never brought back either, and the logo
    // comes back with the formatted signature when formatting is switched on.
    // An image another mail client embedded in a draft it saved is still
    // carried as a file, as it always was.
    var list = (message.attachments || []).filter(function (a) {
      var cid = contentIdOf(a).toLowerCase();
      return !(embeddedStay && OWN_IMAGE_ID.test(cid) && String(message.html || '').toLowerCase().indexOf('cid:' + cid) >= 0);
    });
    if (!list.length) { return Promise.resolve(); }
    el('compose-files-note').textContent = 'Fetching ' + list.length + ' attachment(s)...';
    return Promise.all(list.map(function (a) {
      return apiFetch('/api/v1/me/messages/' + message.id + '/attachments/' + a.index, { cache: 'no-store', credentials: 'same-origin' }).then(function (response) {
        if (response.status !== 200) { throw new Error(a.name); }
        return response.blob();
      }).then(function (blob) {
        return new Promise(function (resolve, reject) {
          var reader = new FileReader();
          reader.onload = function () { resolve({ name: a.name, type: a.content_type || blob.type || 'application/octet-stream', data: String(reader.result).split(',')[1] || '' }); };
          reader.onerror = function () { reject(new Error(a.name)); };
          reader.readAsDataURL(blob);
        });
      });
    })).then(function (files) {
      carried = files;
      el('compose-files-note').textContent = 'Attached from the original: ' + files.map(function (f) { return f.name; }).join(', ');
    }, function (why) {
      carried = [];
      el('compose-files-note').textContent = 'The attachment ' + why.message + ' could not be fetched, so it is not attached.';
    });
  };
  // Reply, Reply all, Forward and Edit draft are addresses of their own, so
  // that a reload keeps the half-written reply's subject and quoting, and Back
  // out of the form and in again does not silently start a different message.
  var prime = function (mode, message) {
    blankCompose();
    var subject = message.subject || '';
    if (mode === 'draft') {
      draftId = message.id;
      el('compose-to').value = message.to || '';
      el('compose-cc').value = message.cc || '';
      el('compose-subject').value = subject;
      el('compose-text').value = message.text || '';
      // Its files first, then its formatting (draftFormatting, with the editor).
      return carryAttachments(message, true).then(function () { return draftFormatting(message); });
    }
    if (mode === 'forward') {
      if (!/^fwd?:/i.test(subject)) { subject = 'Fwd: ' + subject; }
      el('compose-subject').value = subject;
      el('compose-text').value = '\n\n---------- Forwarded message ----------\nFrom: ' + (message.from || '') + '\nDate: ' + (message.date || message.received) + '\nSubject: ' + (message.subject || '') + '\nTo: ' + (message.to || '') + '\n\n' + textOf(message);
      return carryAttachments(message);
    }
    if (!/^re:/i.test(subject)) { subject = 'Re: ' + subject; }
    replyTo = message.message_id ? { message_id: message.message_id, references: ((message.references || '') + ' ' + message.message_id).trim(), id: message.id, from: addressOf(message.from || '') } : null;
    el('compose-to').value = addressOf(message.from);
    el('compose-subject').value = subject;
    el('compose-text').value = '\n\nOn ' + (message.date || message.received) + ', ' + (message.from || '') + ' wrote:\n' + quoted(message);
    if (mode === 'replyall') {
      // Everyone on the message but this account, once each, sender first.
      var seen = {}; var others = [];
      ((message.to || '') + ',' + (message.cc || '')).split(/[,;]/).forEach(function (part) {
        var a = addressOf(part); var key = a.toLowerCase();
        if (!a || key === me || key === addressOf(message.from).toLowerCase() || seen[key]) { return; }
        seen[key] = true; others.push(a);
      });
      el('compose-cc').value = others.join(', ');
    }
    return Promise.resolve();
  };
  var composeBody = function () {
    var body = { to: el('compose-to').value, cc: el('compose-cc').value, bcc: el('compose-bcc').value, subject: el('compose-subject').value, text: el('compose-text').value };
    if (!el('compose-from').hidden) { body.from = el('compose-from').value; }
    if (el('compose-receipt').checked) { body.receipt = true; }
    if (confidentialOffered && Number(el('compose-expires').value) > 0) {
      body.expires_in = Number(el('compose-expires').value);
      var passcode = el('compose-passcode').value.trim();
      if (passcode) { body.passcode = passcode; }
    }
    if (el('compose-sign').checked) { body.sign = true; }
    if (el('compose-encrypt').checked) { body.encrypt = true; }
    if (el('compose-pgp-sign').checked) { body.pgp_sign = true; }
    if (el('compose-pgp-encrypt').checked) { body.pgp_encrypt = true; }
    if (richOn) { body.html = cleanHtml(el('compose-editor')); body.text = plainOf(el('compose-editor')).replace(/\n+$/, ''); }
    if (replyTo) { body.in_reply_to = replyTo.message_id; body.references = replyTo.references; body.answered_id = replyTo.id; }
    return body;
  };
  // ---- Files sent as links: above the domain's threshold a file goes to the
  // server's store in chunks and the message carries a link to it.
  var filesPolicy = { link_above_kb: 8192, days: 14, max_mb: 100, quota_mb: 1024 };
  var loadFilesPolicy = function () {
    return call('GET', '/api/v1/me/files').then(function (result) {
      if (result.status === 200 && result.data && result.data.policy) { filesPolicy = result.data.policy; }
    });
  };
  var linked = [];
  var linkedByKey = {};
  var fileKey = function (f) { return f.name + ':' + f.size + ':' + (f.lastModified || 0); };
  var htmlText = function (s) { return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;'); };
  var whenExpires = function (seconds) { return new Date(seconds * 1000).toLocaleDateString(); };
  var renderLinked = function () {
    var box = el('compose-links');
    clear(box);
    box.hidden = !linked.length;
    if (!linked.length) { return; }
    box.appendChild(node('span', t('Sent as links, until ') + whenExpires(linked[0].expires) + ': '));
    linked.forEach(function (f, i) {
      if (i > 0) { box.appendChild(node('span', ', ')); }
      box.appendChild(node('span', f.name + ' (' + format(f.size) + ')'));
    });
  };
  var CHUNK = 4 * 1024 * 1024;
  var uploadLinked = function (file) {
    var record = null;
    var offset = 0;
    var next = function () {
      if (offset >= file.size) { return record; }
      el('compose-files-note').textContent = 'Sending ' + file.name + ' as a link: ' + Math.round(100 * offset / file.size) + '%';
      var slice = file.slice(offset, Math.min(file.size, offset + CHUNK));
      return apiFetch('/api/v1/me/files/' + record.id + '/content?offset=' + offset, { method: 'PUT', body: slice, headers: { 'X-Requested-With': 'hMailServer', 'Content-Type': 'application/octet-stream' }, cache: 'no-store', credentials: 'same-origin' }).then(function (response) {
        return response.text().then(function (text) {
          var data = null;
          try { data = text ? JSON.parse(text) : null; } catch (e) { data = null; }
          if (response.status === 409 && data && typeof data.stored === 'number' && data.stored !== offset) { offset = data.stored; return next(); }
          if (response.status !== 200 || !data) { throw (data && data.error) || (t('Could not send ') + file.name + ' (' + response.status + ')'); }
          offset = data.stored;
          return next();
        });
      });
    };
    return call('POST', '/api/v1/me/files', { name: file.name, type: file.type || 'application/octet-stream', size: file.size }).then(function (result) {
      if (result.status !== 201 || !result.data) { throw describe(result, t('Could not send ') + file.name + t(' as a link')); }
      record = result.data;
      return next();
    }).then(function (done) {
      var entry = { name: file.name, size: file.size, link: done.link || record.link, expires: record.expires };
      linked.push(entry);
      linkedByKey[fileKey(file)] = entry;
      renderLinked();
      return entry;
    });
  };
  // The links go at the end of what is sent - the text, and the HTML when
  // there is one - never into the form, so a draft saved and then sent
  // carries them once.
  var appendLinks = function (body) {
    if (!linked.length) { return; }
    var until = whenExpires(linked[0].expires);
    var lines = linked.map(function (f) { return '- ' + f.name + ' (' + format(f.size) + '): ' + location.origin + f.link; });
    body.text = (body.text || '') + '\n\nFiles sent as links (until ' + until + '):\n' + lines.join('\n') + '\n';
    if (body.html) {
      var items = linked.map(function (f) { var url = location.origin + f.link; return '<li><a href="' + htmlText(url) + '">' + htmlText(f.name) + '</a> (' + htmlText(format(f.size)) + ')</li>'; });
      body.html += '<p>Files sent as links (until ' + htmlText(until) + '):</p><ul>' + items.join('') + '</ul>';
    }
  };
  var resetLinked = function () { linked = []; linkedByKey = {}; renderLinked(); };

  // Files: those under the domain's threshold are read in the browser and
  // sent as base64 in the same call, twelve megabytes together at most;
  // those above it go to the store first, one after another, and become
  // links.
  var dropped = [];
  var readFiles = function () {
    var files = Array.prototype.slice.call(el('compose-files').files || []).concat(dropped);
    var threshold = Number(filesPolicy.link_above_kb) * 1024;
    var asLink = function (f) { return threshold > 0 && f.size > threshold; };
    var small = files.filter(function (f) { return !asLink(f); });
    var large = files.filter(asLink).filter(function (f) { return !linkedByKey[fileKey(f)]; });
    var total = small.reduce(function (n, f) { return n + f.size; }, 0);
    // The pictures in the message count against the same twelve megabytes,
    // because they travel in the same request: a data: URL is base64, so its
    // characters stand for three quarters of their number in bytes, which is
    // what a file's size already is. Without this the two bounds could ask
    // the route for more than the sixteen megabytes it takes, and the reader
    // would see the connection fail rather than a sentence saying why.
    var pictures = richOn ? Math.round(imagesHeld() * 3 / 4) : 0;
    // And the attachments carried over from the message being answered,
    // forwarded or reopened: they are already base64 here and the callers put
    // them at the front of this same list, so they are bytes in the same
    // request. Counting only what was freshly picked let a forward of eleven
    // megabytes take four more of pictures and ask the route for eighteen.
    var brought = carried.reduce(function (n, f) { return n + Math.round(String(f.data || '').length * 3 / 4); }, 0);
    if (small.length > 20) { return Promise.reject(t('At most 20 files.')); }
    if (total + brought + pictures > 12 * 1024 * 1024) { return Promise.reject(pictures ? t('The pictures in the message and the files are larger than 12 MB together.') : t('The files are larger than 12 MB together.')); }
    var uploads = large.reduce(function (chain, f) { return chain.then(function () { return uploadLinked(f); }); }, Promise.resolve());
    return uploads.then(function () {
      if (large.length) { el('compose-files-note').textContent = large.length + ' file' + (large.length === 1 ? '' : 's') + ' sent as ' + (large.length === 1 ? 'a link' : 'links') + '.'; }
      return Promise.all(small.map(function (file) {
        return new Promise(function (resolve, reject) {
          var reader = new FileReader();
          reader.onload = function () { resolve({ name: file.name, type: file.type || 'application/octet-stream', data: String(reader.result).split(',')[1] || '' }); };
          reader.onerror = function () { reject('Could not read ' + file.name); };
          reader.readAsDataURL(file);
        });
      }));
    }, function (why) { return Promise.reject(String(why)); });
  };
  // The attachment reminder: a body that speaks of an attachment - attached,
  // attachment, enclosed - in the reader's own lines. A quoted line is what
  // the other person wrote, and so is everything under a forwarded message's
  // rule, so neither counts.
  var mentionsAttachment = function (text) {
    var own = String(text || '').split('---------- Forwarded message ----------')[0];
    own = own.split('\n').filter(function (line) { return line.replace(/^\s+/, '').charAt(0) !== '>'; }).join('\n');
    return /\b(attached|attachments?|enclosed)\b/i.test(own);
  };
  var anythingAttached = function () {
    var picked = el('compose-files').files;
    // A picture put into the message is something attached, as far as the
    // reminder is concerned: "the screenshot is attached" with a screenshot
    // pasted in is not a message that forgot anything.
    return !!((picked && picked.length) || dropped.length || carried.length || linked.length || (richOn && imagesHeld() > 0));
  };

  // ---- Held mail ----------------------------------------------------------
  var renderQuarantine = function (held) {
    var list = el('quarantine-list');
    clear(list);
    quarantineOn = !!held.enabled;
    el('held-count').textContent = held.enabled && held.messages.length ? String(held.messages.length) : '';
    if (!held.enabled) { el('quarantine-note').textContent = 'The server does not hold suspected spam for review; it refuses it.'; return; }
    if (!held.messages.length) { el('quarantine-note').textContent = 'Nothing is being held for you.'; return; }
    el('quarantine-note').textContent = held.messages.length + ' message' + (held.messages.length === 1 ? '' : 's') + ' held as suspected spam. Releasing one delivers it to you; deleting one is final.';
    held.messages.forEach(function (m) {
      var row = node('div', undefined, 'frow');
      var body = node('div', undefined, 'fname');
      body.appendChild(node('div', m.subject || t('(no subject)')));
      body.appendChild(node('div', t('From ') + m.sender + ' - ' + m.created + ' - ' + m.reason + t(' (score ') + m.score + ')', 'fmeta'));
      row.appendChild(body);
      var release = button(t('Release to my inbox'), 'btn sm');
      var discard = button(t('Delete'), 'btn ghost sm danger');
      release.addEventListener('click', function () {
        call('POST', '/api/v1/me/quarantine/' + m.id + '/release').then(function (result) {
          say('quarantine-status', result.status === 200 ? t('Released. It will arrive in your inbox shortly.') : describe(result, t('Could not release')), result.status === 200);
          loadQuarantine();
        });
      });
      discard.addEventListener('click', function () {
        call('DELETE', '/api/v1/me/quarantine/' + m.id).then(function (result) {
          say('quarantine-status', result.status === 200 ? t('Deleted.') : describe(result, t('Could not delete')), result.status === 200);
          loadQuarantine();
        });
      });
      row.appendChild(release); row.appendChild(discard);
      list.appendChild(row);
    });
  };
  var loadQuarantine = function () {
    return call('GET', '/api/v1/me/quarantine').then(function (result) {
      if (result.status === 200 && result.data) { renderQuarantine(result.data); }
    });
  };

  // ---- Making, renaming and deleting a folder -----------------------------
  //
  // The server decides what a folder may be called and what may be deleted;
  // this shows the sentence it answers with rather than inventing one.
  var renderFolderAdmin = function () {
    var list = el('folder-admin-list');
    clear(list);
    var parent = el('folder-new-parent');
    clear(parent);
    var top = node('option', t('At the top level')); top.value = ''; parent.appendChild(top);
    allFolders.forEach(function (f) {
      // Compared, not tested for truth. A public folder's account_id is 0,
      // and a falsy account_id walked straight through the truthiness form
      // of this guard - so the card drew Rename and Delete for folders the
      // routes always refuse. The folder picker above compares directly.
      if (foldersById[inboxId] && f.account_id !== foldersById[inboxId].account_id) { return; }
      var option = node('option', f.path); option.value = f.id; parent.appendChild(option);
      var row = node('div', undefined, 'frow');
      var name = node('div', f.path, 'fname');
      row.appendChild(name);
      var held = f.messages === undefined ? (f.count || 0) : f.messages;
      row.appendChild(node('span', held + ' message' + (held === 1 ? '' : 's'), 'fmeta'));
      if (f.writable === false) { row.appendChild(node('span', 'read-only', 'badge info')); list.appendChild(row); return; }
      var isInbox = f.path.toUpperCase() === 'INBOX';
      if (f.special_use) { row.appendChild(node('span', t('kept for ') + f.special_use, 'badge info')); }
      if (isInbox) { list.appendChild(row); return; }
      var rename = button(t('Rename'));
      rename.addEventListener('click', function () {
        clear(row);
        // The whole path, because that is what the server renames by: editing
        // the part before the delimiter moves the folder, and its subfolders
        // go with it. Showing only the leaf would make a move look like a
        // rename, and a rename to the top level look like nothing.
        var input = document.createElement('input'); input.type = 'text'; input.value = f.path; input.setAttribute('maxlength', '255');
        input.setAttribute('aria-label', t('The full name of this folder, parts separated by ') + delimiter);
        var save = button(t('Save'), 'btn sm');
        var cancel = button(t('Cancel'));
        save.addEventListener('click', function () {
          call('PUT', '/api/v1/me/folders/' + f.id, { name: input.value.trim() }).then(function (result) {
            if (result.status === 200) { say('folder-admin-status', t('Renamed.'), true); loadFolders().then(renderFolderAdmin); return; }
            say('folder-admin-status', describe(result, t('Could not rename the folder')), false);
            renderFolderAdmin();
          });
        });
        cancel.addEventListener('click', renderFolderAdmin);
        row.appendChild(input); row.appendChild(save); row.appendChild(cancel);
      });
      row.appendChild(rename);
      // The server refuses to delete a folder it has designated for a special
      // use, and the inbox, and it is right to: there is no undo here. Not
      // offering it is better than a refusal the reader did not ask for.
      if (f.special_use) { list.appendChild(row); return; }
      var remove = button(t('Delete'), 'btn ghost sm danger');
      remove.addEventListener('click', function () {
        if (remove.getAttribute('data-armed') !== 'yes') {
          remove.setAttribute('data-armed', 'yes');
          remove.textContent = t('Really delete?');
          return;
        }
        call('DELETE', '/api/v1/me/folders/' + f.id).then(function (result) {
          if (result.status === 200 || result.status === 204) {
            say('folder-admin-status', t('Deleted.'), true);
            if (state.folderId === f.id) { state.folderId = inboxId; }
            loadFolders().then(renderFolderAdmin);
            return;
          }
          say('folder-admin-status', describe(result, t('Could not delete the folder')), false);
          renderFolderAdmin();
        });
      });
      row.appendChild(remove);
      list.appendChild(row);
    });
  };

  // ---- The change probe ---------------------------------------------------
  //
  // One small GET every few seconds, carrying the token the last answer gave.
  // The server says whether anything changed and what each folder now holds;
  // when something did, the folder list and the open listing are read again
  // and the reader is put back where they were. Nothing is polled while the
  // tab is in the background - the browser tells us, and we stop until it
  // tells us otherwise, so a portal left open all day costs nothing.
  var probe = { token: '', timer: 0, paused: false, off: false };
  var probeEvery = 6000;
  var probeSlowly = 30000;
  var probeIn = probeEvery;
  var setProbeState = function (text, stale) {
    el('probe-state').textContent = text;
    el('probe-dot').classList.toggle('stale', !!stale);
  };
  var applyCounts = function (folders) {
    var fresh = false;
    var grew = [];
    folders.forEach(function (f) {
      var known = foldersById[f.id];
      if (!known) { fresh = true; return; }
      if (f.unseen > known.unseen && notifyWanted(f.id)) { grew.push({ folder: known, added: f.unseen - known.unseen, unseen: f.unseen }); }
      known.unseen = f.unseen;
      known.count = f.count;
    });
    notifyNewMail(grew);
    updateTitle();
    renderAccountNav();
    if (fresh) { return true; }
    var nav = el('folder-nav');
    var children = nav.children;
    for (var i = 0; i < children.length; i++) {
      var route = children[i].getAttribute('data-route') || '';
      var id = Number(route.replace('/f/', ''));
      var folder = foldersById[id];
      var count = children[i].children[2];
      if (folder && count) {
        var shown = folderIs(id, 'Drafts') ? folder.count : folder.unseen;
        count.textContent = shown ? String(shown) : '';
        children[i].classList.toggle('unread', !!folder.unseen && !folderIs(id, 'Drafts'));
      }
    }
    return false;
  };
  var probeStop = function () { if (probe.timer) { clearTimeout(probe.timer); probe.timer = 0; } };
  var probeLater = function () {
    probeStop();
    if (el('account').hidden || probe.off) { return; }
    probe.timer = setTimeout(probeNow, probeIn);
  };
  var probeNow = function () {
    probeStop();
    if (el('account').hidden || probe.off) { return Promise.resolve(); }
    // A background tab stops asking - unless notifications are on, in
    // which case it keeps asking, slowly, since that is what they are for.
    if (document.hidden && pref('notify') !== '1') { probe.paused = true; setProbeState('Paused - this tab is in the background', true); return Promise.resolve(); }
    probe.paused = false;
    return call('GET', '/api/v1/me/changes' + (probe.token ? '?since=' + encodeURIComponent(probe.token) : '')).then(function (result) {
      if (result.status === 200 && result.data) {
        var had = !!probe.token;
        probe.token = result.data.token || probe.token;
        var unknown = applyCounts(result.data.folders || []);
        probeIn = document.hidden ? probeSlowly : probeEvery;
        setProbeState('Watching for new mail', false);
        // A page left open overnight: the follow-ups due today are asked for once the day turns.
        if (followUps.day && followUps.day !== dueToday()) { followUpsCheck(); }
        if (had && (result.data.changed || unknown)) {
          loadFolders().then(function () {
            if (!el('mail-section').hidden && !el('message-list').hidden) { reloadKeepingPlace(); }
          });
          // What this browser keeps for reading offline follows the change too.
          offlineFillSoon();
        }
      } else if (result.status === 404 || result.status === 403) {
        // An older server, serving this page with no change route behind it -
        // 404 if it does not know the path, 403 if it does not know it is one
        // of the account's own. Asking it again every six seconds for the rest
        // of the day would be a defect of this page, so it is asked once.
        probe.off = true;
        setProbeState('This server does not report changes', true);
        return;
      } else if (result.status !== 401) {
        // Something transient - a restart, a rate limit. Keep watching, but
        // stop leaning on it.
        probeIn = probeSlowly;
        setProbeState('Not watching (' + result.status + ')', true);
      }
      probeLater();
    });
  };

  // ---- Routing ------------------------------------------------------------
  var pending = '';
  var showMail = function (folderId, params) {
    state.folderId = folderId;
    state.query = params.q || '';
    state.everywhere = false;
    state.before = Number(params.before || 0);
    el('mail-search').value = state.query;
    el('mail-search-everywhere').checked = false;
    dropMessage();
    showPanel('mail-section', folderName(folderId) + (state.query ? ' - \'' + state.query + '\'' : ''));
    markNav('/f/' + folderId);
    showList();
    renderListTools();
    if (lastListing && lastListing.key === listingKey()) {
      selected = lastListing.selected || {};
      renderMessages(lastListing.page);
      var back = -1;
      listRows.forEach(function (r, i) { if (r.id === lastListing.cursorId) { back = i; } });
      if (back >= 0) { setCursor(back, true); }
      el('message-list').scrollTop = lastListing.scroll || 0;
      reloadKeepingPlace();
      return;
    }
    loadMessages();
  };
  var showSearch = function (params) {
    state.query = params.q || '';
    state.everywhere = true;
    state.before = 0;
    el('mail-search').value = state.query;
    el('mail-search-everywhere').checked = true;
    dropMessage();
    var saved = params.saved !== undefined ? savedSearches()[Number(params.saved)] : null;
    showPanel('mail-section', saved && saved.q === state.query ? saved.name : 'Search - \'' + state.query + '\'');
    markNav(saved && saved.q === state.query ? savedSearchRoute(saved, Number(params.saved)) : '');
    showList();
    renderListTools();
    loadMessages();
  };
  var showMessage = function (id) {
    showPanel('mail-section', folderName(state.folderId));
    markNav(currentNavHash());
    showList();
    if (!lastListing && state.folderId && !state.query) { loadMessages(); }
    openMessage(id).then(function (m) {
      if (!m) { return; }
      if (!state.folderId && m.folder_id) {
        state.folderId = m.folder_id;
        el('view-title').textContent = folderName(m.folder_id);
        markNav('/f/' + m.folder_id);
        loadMessages();
      }
      var index = -1;
      listRows.forEach(function (r, i) { if (r.id === m.id || (r.ids || []).indexOf(m.id) >= 0) { index = i; } });
      if (index >= 0) { setCursor(index, true); }
    });
  };
  // ---- The installed app ------------------------------------------------
  // The unread count on the installed app's icon - the App Badging API, the
  // same total the title carries, which is the folders the reader asked to
  // be told about. Nothing without the API, nothing said when it refuses.
  var updateBadge = function (total) {
    try {
      if (typeof navigator === 'undefined' || typeof navigator.setAppBadge !== 'function') { return; }
      var p = total ? navigator.setAppBadge(total) : navigator.clearAppBadge();
      if (p && p.catch) { p.catch(function () { /* the badge is a courtesy */ }); }
    } catch (why) { /* the badge is a courtesy */ }
  };
  // A mailto: URL (RFC 6068) into the fields of a new message: the addresses
  // before the question mark, then subject, body, cc and bcc as query keys,
  // each percent-decoded; anything else the URL carries is ignored.
  var parseMailto = function (raw) {
    var text = String(raw || '');
    if (!/^mailto:/i.test(text)) { return null; }
    text = text.slice(7);
    var cut = text.indexOf('?');
    var out = { to: decodeURIComponent(cut >= 0 ? text.slice(0, cut) : text) };
    (cut >= 0 ? text.slice(cut + 1) : '').split('&').forEach(function (pair) {
      if (!pair) { return; }
      var eq = pair.indexOf('=');
      var key = decodeURIComponent(eq < 0 ? pair : pair.slice(0, eq)).toLowerCase();
      var value = eq < 0 ? '' : decodeURIComponent(pair.slice(eq + 1).replace(/\+/g, ' '));
      if (key === 'to') { out.to = out.to ? out.to + ', ' + value : value; }
      else if (key === 'subject' || key === 'body' || key === 'cc' || key === 'bcc') { out[key] = value; }
    });
    return out;
  };
  // What another app shared to the installed webmail: the service worker
  // answered the share's POST by putting the title, text, link and files into
  // a cache of their own and sending the page here. Read once, then gone.
  var readShare = function () {
    if (typeof caches === 'undefined' || !caches.open) { return Promise.resolve(null); }
    var name = 'hm-portal-share';
    return caches.open(name).then(function (c) {
      return c.match('/portal/share-text').then(function (r) { return r ? r.json() : null; }).then(function (meta) {
        if (!meta) { return null; }
        var files = [];
        var chain = Promise.resolve();
        (meta.files || []).forEach(function (fileName, i) {
          chain = chain.then(function () {
            return c.match('/portal/share-file/' + i).then(function (r) {
              if (!r) { return null; }
              return r.blob().then(function (b) { files.push(new File([b], fileName, { type: b.type || 'application/octet-stream' })); });
            });
          });
        });
        return chain.then(function () { return caches.delete(name); }).then(function () {
          return { title: meta.title || '', text: meta.text || '', url: meta.url || '', files: files };
        });
      });
    }).catch(function () { return null; });
  };
  var fillCompose = function (fields) {
    if (fields.to) { el('compose-to').value = fields.to; }
    if (fields.cc) { el('compose-cc').value = fields.cc; }
    if (fields.bcc) { el('compose-bcc').value = fields.bcc; }
    if (fields.subject) { el('compose-subject').value = fields.subject; }
    if (fields.body) {
      var text = el('compose-text');
      text.value = fields.body + (text.value ? '\n\n' + text.value : '');
    }
    syncComposeRows();
  };
  // A new message opened with something already in it: a mailto: link the
  // browser handed over, the fields as query keys, or what another app
  // shared. Runs after the form has been blanked and signed.
  var prefillCompose = function (params) {
    var fields = params.mailto !== undefined ? (parseMailto(params.mailto) || {}) : {};
    ['to', 'cc', 'bcc', 'subject', 'body'].forEach(function (key) { if (params[key] !== undefined) { fields[key] = params[key]; } });
    fillCompose(fields);
    if (params.share === undefined) { return Promise.resolve(); }
    return readShare().then(function (shared) {
      if (!shared) { return; }
      var body = [shared.text, shared.url].filter(function (s) { return s; }).join('\n');
      fillCompose({ subject: shared.title, body: body });
      if (shared.files.length) {
        shared.files.forEach(function (f) { dropped.push(f); });
        el('compose-files-note').textContent = 'Attached from the share: ' + shared.files.map(function (f) { return f.name + ' (' + format(f.size) + ')'; }).join(', ');
      }
    });
  };
  var composePrefilled = function (params) {
    return ['mailto', 'share', 'to', 'cc', 'bcc', 'subject', 'body'].some(function (key) { return params[key] !== undefined; });
  };
  var showCompose = function (params) {
    learnConfidential();
    var mode = params.reply !== undefined ? 'reply' : params.replyall !== undefined ? 'replyall' :
      params.forward !== undefined ? 'forward' : params.draft !== undefined ? 'draft' : 'new';
    var id = Number(params[mode] || 0);
    var key = mode + ':' + id;
    if (composeKey === key && !composePrefilled(params)) { openCompose(mode, id); el('compose-to').focus(); return; }
    composeKey = key;
    if (mode === 'new' || !id) { blankCompose(); addSignature(); syncComposeRows(); prefillCompose(params); openCompose('new', 0); el('compose-to').focus(); return; }
    if (current && current.id === id) { var was = current; prime(mode, was).then(function () { afterPrime(mode, was); openCompose(mode, id); }); return; }
    call('GET', '/api/v1/me/messages/' + id).then(function (result) {
      if (result.status === 200 && result.data) { prime(mode, result.data).then(function () { afterPrime(mode, result.data); openCompose(mode, id); }); return; }
      openCompose(mode, id);
      say('compose-status', describe(result, t('Could not read the message being answered')), false);
    });
  };
  // The one-time secret is for the moment it is made: it leaves the page
  // with the view that showed it, and with the account.
  var forgetSecret = function () { el('apppassword-new').hidden = true; el('apppassword-secret').textContent = ''; };
  var route = function () {
    if (el('account').hidden) { return; }
    var r = parseHash();
    var head = r.parts[0] || '';
    if (head !== 'security') { forgetSecret(); }
    if (head === 'f' && r.parts[1]) { cameFromList = true; showMail(Number(r.parts[1]), r.params); return; }
    if (head === 'search') { cameFromList = true; showSearch(r.params); return; }
    if (head === 'm' && r.parts[1]) { showMessage(Number(r.parts[1])); return; }
    if (head === 'starred') { cameFromList = true; showSearch({ q: 'is:flagged' }); el('view-title').textContent = t('Starred'); markNav('/starred'); return; }
    if (head === 'important') { cameFromList = true; showSearch({ q: IMPORTANT_QUERY }); el('view-title').textContent = t('Important'); markNav('/important'); return; }
    if (head === 'recover') { showPanel('recover-section', t('Recover deleted items')); markNav('/recover'); loadRecoverable(); return; }
    if (head === 'confidential') { showPanel('confidential-section', t('Messages that expire')); markNav('/confidential'); loadConfidential(); return; }
    if (head === 'scheduled') { showPanel('scheduled-section', t('Scheduled')); markNav('/scheduled'); loadScheduled(); return; }
    if (head === 'compose') {
      // The window opens over whatever page is shown; on a reload, over the list.
      if (!panels.some(function (p) { return el(p) && !el(p).hidden; })) { showMail(state.folderId || inboxId, {}); }
      showCompose(r.params);
      return;
    }
    if (head === 'held') { showPanel('quarantine-section', 'Held as suspected spam'); markNav('/held'); loadQuarantine(); return; }
    if (head === 'folders') { showPanel('folders-section', 'Manage folders'); markNav('/folders'); renderFolderAdmin(); return; }
    if (head === 'settings') { showPanel('settings-section', 'Settings'); markNav('/settings'); loadPush(); loadOffline(); loadDirectoryCard(); return; }
    if (head === 'filters') { showPanel('filter-section', 'Filters'); markNav('/filters'); renderColourRules(); return; }
    if (head === 'contacts') { showPanel('contacts-section', 'Contacts'); markNav('/contacts'); loadBooks().then(loadContacts); return; }
    if (head === 'directory') { showPanel('directory-section', t('Directory')); markNav('/directory'); loadDirectory(); return; }
    if (head === 'calendar') { showPanel('calendar-section', t('Calendar')); markNav('/calendar'); openCalendar(r.params); return; }
    if (head === 'tasks') { showPanel('tasks-section', t('Tasks')); markNav('/tasks'); loadTasks(); return; }
    if (head === 'away') { showPanel('away-section', 'Away and forwarding'); markNav('/away'); renderAwayAddresses(); loadScheduled(); return; }
    if (head === 'storage') { showPanel('storage-section', 'Storage'); markNav('/storage'); loadStorage(); loadFiles(); return; }
    if (head === 'senders') { showPanel('senders-section', t('Safe senders')); markNav('/senders'); loadSenderList(); loadDigestSetting(); return; }
    if (head === 'subscriptions') { showPanel('subscriptions-section', t('Subscriptions')); markNav('/subscriptions'); loadSubscriptions(); return; }
    if (head === 'masked') { showPanel('masked-section', t('Masked addresses')); markNav('/masked'); loadMaskedAddresses(); return; }
    if (head === 'accounts') { showPanel('accounts-section', t('Other accounts')); markNav('/accounts'); renderSecondCard(); loadFetchAccounts().then(function () { importOutcome(r.params); }); loadSenderAddresses(); return; }
    if (head === 'security') { showPanel('security-section', 'Security'); markNav('/security'); loadSecurity(); return; }
    if (head === 'password') {
      if (directoryLinked) { replaceWith('/f/' + inboxId); return; }
      showPanel('password-section', 'Change password'); markNav('/password'); return;
    }
    replaceWith('/f/' + (state.folderId || inboxId));
  };

  // ---- Sign in and sign out -----------------------------------------------
  // Everything the signed-in account put in this page. Called when the
  // gate goes back up, because the next person to use this browser must
  // not be shown any of it: openMessage and showMail both render from
  // these caches before they ask the server, and the fragment in the
  // address bar survives a sign-out, so a stale #/m/<id> would repaint
  // the previous account's message to the new one without a request.
  // Two halves. forgetView is what the page holds in memory and on the
  // screen for the account it is showing - the caches, the open message, the
  // list, the folders, the draft, the keys - and goes at a sign-out and at a
  // switch to the second account alike, since the other account must not be
  // shown any of it. forgetAccount is that and what this browser keeps for
  // the first account between visits - the offline store, the searches, the
  // tallies - which a switch keeps and only a sign-out removes.
  var forgetView = function () {
    // A send still waiting cannot go on a session that has ended.
    cancelUndo();
    me = '';
    identities = [];
    // The directory is the account's own domain's, so none of it survives a
    // switch to the other account.
    directoryEntries = null;
    directoryOwn = null;
    directoryEnabled = false;
    directoryEditable = false;
    directoryPhotoPending = undefined;
    if (el('nav-directory')) { el('nav-directory').hidden = true; }
    signature = { enabled: false, text: '' };
    prefs = {};
    document.title = baseTitle;
    hideSearchHistory();
    hideSuggestions();
    clear(el('search-suggest-list'));
    followUps.day = '';
    // The events the reminders watch are the account's that was shown: the
    // next tick reads the next account's own.
    remindState = { events: [], fetched: 0 };
    replied = {};
    askedReplies = {};
    clear(el('quick-steps'));
    prefetched = {};
    judged = {}; judgedByPage = {}; openedIds = {}; judgeWaiting = false;
    contactsCache = null;
    bookState = { books: [], current: 0, detail: false };
    current = null;
    lastListing = null;
    listRows = [];
    selected = {};
    allFolders = [];
    foldersById = {};
    inboxId = 0;
    // Where the reader was is a folder of the account being left: the next
    // account's inbox is where the router lands from nothing.
    state.folderId = 0; state.query = ''; state.everywhere = false; state.before = 0;
    inlineFor = { id: 0, images: null };
    showHtml = false;
    composeKey = '';
    blankCompose();
    hideCompose();
    forgetSecret();
    smimeForget();
    pgpForget();
    var frame = el('message-html');
    if (frame) { frame.hidden = true; frame.removeAttribute('src'); }
    htmlBlobRevoke();
    ['message-list', 'folder-nav', 'message-text', 'message-attachments'].forEach(function (id) {
      var node = el(id);
      if (node) { node.innerHTML = ''; }
    });
    // The address bar too, so the router has nothing to re-open. Put to
    // the root route rather than rebuilt from the location: replaceState
    // leaves no history entry and fires no hashchange, and '#/' is the one
    // address that names nothing - the gate is up now, and a later sign-in
    // lands on the inbox from it.
    if (location.hash && location.hash !== '#/') {
      try { history.replaceState(null, '', '#/'); } catch (e) { location.hash = '#/'; }
    }
  };
  var forgetAccount = function () {
    // The second account goes with the first: its token was this page's and
    // the page is signing out. First, so that what is forgotten below is
    // forgotten as the first account, whose it all is - the stores answer
    // nothing while the second account is the one shown.
    secondForget();
    accounts.active = 'first';
    forgetView();
    // What this browser learned about whose mail matters to this person goes
    // with the account, whichever way the session ended - and it is filed
    // under the address, so every tally goes rather than one named by it.
    tallyForget();
    offlineForget();
    searchesWrite([]);
    dueShownForget();
    emojiForget();
    accounts.first = { address: '', unseen: 0, inboxId: 0, changes: '' };
    renderAccountNav();
  };
  var showSignIn = function () {
    probeStop();
    // The page is signing out, or the first account's session has ended
    // under it: the second account's session is ended on the server with
    // its token, not merely forgotten here to stand until it times out. A
    // sign-out has ended it already and there is nothing to do.
    secondEndQuietly();
    forgetAccount();
    el('account').hidden = true;
    el('signin').hidden = false;
    // The gate holds two forms - signing in, and asking for a reset link -
    // and the one that shows is always the first of them when the gate goes
    // up, so a session that ended does not put somebody back at a form they
    // had opened once.
    el('signin-form').hidden = false;
    el('reset-form').hidden = true;
    el('signin-code-row').hidden = true;
    el('signin-code').value = '';
    say('reset-status', '', true);
    el('address').focus();
  };
  var render = function (account) {
    el('who').textContent = account.address;
    el('account-avatar').textContent = initialsOf(account.address);
    if (el('account-avatar').style) { el('account-avatar').style.background = avatarColour(account.address); }
    el('password-user').value = account.address;
    var used = account.quota.used_bytes, limit = account.quota.limit_mb * 1048576;
    if (limit > 0) {
      el('quota').textContent = format(used) + ' of ' + account.quota.limit_mb + ' MB used';
      el('quota-bar').style.width = Math.min(100, Math.round(100 * used / limit)) + '%';
    } else {
      el('quota').textContent = format(used) + ' used, no limit';
      el('quota-bar').style.width = '0';
    }
    el('password-changed').textContent = account.password_changed ? t('Password last changed ') + account.password_changed : '';
    el('vacation-enabled').checked = !!account.vacation.enabled;
    el('vacation-subject').value = account.vacation.subject || '';
    el('vacation-message').value = account.vacation.message || '';
    el('vacation-expires').checked = !!account.vacation.expires;
    el('vacation-expires-date').value = account.vacation.expires_date || '';
    el('otp-row').hidden = !account.second_factor;
    directoryLinked = !!account.directory_linked;
    var nav = el('view-nav').children;
    for (var i = 0; i < nav.length; i++) {
      if (nav[i].getAttribute('data-route') === '/password') { nav[i].hidden = directoryLinked; }
    }
    el('signin').hidden = true;
    el('account').hidden = false;
  };
  var renderSettings = function (s) {
    el('name-first').value = s.name.first || '';
    el('name-last').value = s.name.last || '';
    el('forward-enabled').checked = !!s.forwarding.enabled;
    el('forward-address').value = s.forwarding.address || '';
    el('forward-keep').checked = !!s.forwarding.keep_original;
    el('signature-enabled').checked = !!s.signature.enabled;
    el('signature-text').value = s.signature.text || '';
    el('signature-html').innerHTML = s.signature.html || '';
    signature = { enabled: !!s.signature.enabled, text: s.signature.text || '', html: s.signature.html || '' };
  };
  var loadSettings = function () {
    call('GET', '/api/v1/me/settings').then(function (result) {
      if (result.status === 200 && result.data) { renderSettings(result.data); }
    });
    call('GET', '/api/v1/me/filters').then(function (result) {
      if (result.status === 200 && result.data) { el('filter-script').value = result.data.active || ''; renderRules(result.data.active || ''); }
    });
  };
  // The account the page shows, read whole: the one the cookie stands for,
  // or the second one when that is the active one - every loader below asks
  // through call(), which carries the right credential for whichever it is.
  var load = function (quiet) {
    var asSecond = onSecond();
    return call('GET', '/api/v1/me').then(function (result) {
      if (result.status === 200 && result.data) {
        me = String(result.data.address || '').toLowerCase();
        if (asSecond) { accounts.second.address = me; } else { accounts.first.address = me; }
        smimeViaAppPassword = result.data.via_app_password === true;
        offline.only = false;
        offlineRetryStop();
        offlineNote(false);
        render(result.data);
        renderAccountNav();
        loadQuarantine();
        loadSettings();
        loadPrefs();
        loadIdentities();
        loadBranding();
        loadFilesPolicy();
        loadSmime();
        // The directory's switch and this person's own entry, once per sign-in:
        // the side nav offers the page only where the server answers one.
        loadDirectoryCard();
        loadPgpKeys();
        return loadFolders().then(function () {
          if (pending) { var wanted = pending; pending = ''; replaceWith(wanted); }
          else { route(); }
          probeNow();
          otherProbeLater();
          followUpsCheck();
          loadOffline();
          return true;
        });
      }
      // The second account could not be read: its session has ended, or the
      // server refused it. That account goes, and the page is the first
      // account's again - the first's session is not what failed here. No
      // connection at all is not that: the token is kept, and the page falls
      // back to the first account, whose kept mail is what it can show.
      if (asSecond && result.status !== 0) { secondEnded(t('The second account\'s session has ended. Add it again to go on using it.')); return false; }
      if (asSecond) { accounts.active = 'first'; forgetView(); renderAccountNav(); }
      // The server could not be reached at all, and this browser holds mail
      // kept for reading offline: show that, and ask again when the
      // connection is back. A refusal - a session that ended - is not this.
      if (result.status === 0 && offlineOwner()) { enterOffline(); return false; }
      if (!quiet) { say('signin-status', describe(result, t('Could not sign in')), false); }
      pending = parseHash().hash;
      showSignIn();
      return false;
    });
  };

  // ---- Wiring -------------------------------------------------------------
  el('signin-form').addEventListener('submit', function (event) {
    event.preventDefault();
    say('signin-status', '', true);
    var address = el('address').value.trim(), password = el('password').value;
    smimePassword = password;
    el('password').value = '';
    var basic = 'Basic ' + btoa(unescape(encodeURIComponent(address + ':' + password)));
    // A new session is a new server as far as this page knows: whatever the
    // last one answered about the change route, ask again.
    probe.token = ''; probe.off = false; probeIn = probeEvery;
    // A recovery code in place of the code from an authenticator app, for
    // somebody whose authenticator is gone: the server spends it, and says how
    // many are left so the page can say so too. The field is empty and hidden
    // unless the reader asked for it, and the header is then not sent at all.
    var recoveryCode = el('signin-code').value.trim();
    el('signin-code').value = '';
    call('POST', '/api/v1/session', undefined, { 'Authorization': basic, 'X-hMailServer-Recovery-Code': recoveryCode }).then(function (result) {
      if (result.status === 201) {
        var left = result.data && typeof result.data.recovery_codes_left === 'number' ? result.data.recovery_codes_left : -1;
        load(false).then(function () {
          if (left >= 0) { toast(tf('A recovery code was used to sign in. {0} of your codes are left.', String(left)), null, true); }
        });
        return;
      }
      say('signin-status', describe(result, t('Could not sign in')), false);
    });
  });
  el('signin-code-toggle').addEventListener('click', function () {
    el('signin-code-row').hidden = false;
    el('signin-code').focus();
  });
  el('signin-forgot').addEventListener('click', function () {
    say('signin-status', '', true);
    say('reset-status', '', true);
    el('reset-address').value = el('address').value.trim();
    el('signin-form').hidden = true;
    el('reset-form').hidden = false;
    el('reset-address').focus();
  });
  el('reset-cancel').addEventListener('click', function () {
    el('reset-form').hidden = true;
    el('signin-form').hidden = false;
    el('address').focus();
  });
  el('reset-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var address = el('reset-address').value.trim();
    if (!address) { return; }
    // One answer, whatever the server found: the route is written not to say
    // whether an address exists, and the page must not undo that by saying
    // something different when it does.
    call('POST', '/api/v1/password-reset', { address: address }).then(function (result) {
      if (result.status === 202) { say('reset-status', t('If that address can be sent a link, one is on its way. It works once and for half an hour.'), true); return; }
      say('reset-status', describe(result, t('This server does not offer that')), false);
    });
  });
  // What this server offers besides a password, asked before anybody has
  // signed in. Nothing here is a credential and nothing here says who exists.
  var passkeySignIn = function () {
    say('signin-status', '', true);
    return call('POST', '/api/v1/passkeys/challenge').then(function (result) {
      if (result.status !== 200 || !result.data) { say('signin-status', describe(result, t('That did not work. Sign in with your password.')), false); return null; }
      return navigator.credentials.get({ publicKey: {
        challenge: fromB64u(result.data.challenge),
        rpId: result.data.rp_id,
        timeout: result.data.timeout,
        // Required, not preferred: the server does not ask for the one-time
        // code on a passkey sign-in because the authenticator verified the
        // person, so an assertion made on a touch alone is one factor where
        // two were promised - and the server refuses it. Asking for it here
        // means the device asks for the PIN or the fingerprint rather than
        // the person being refused after they have already touched the key.
        userVerification: 'required',
        allowCredentials: []
      } });
    }).then(function (assertion) {
      if (!assertion) { return; }
      var r = assertion.response;
      var body = { passkey: { id: assertion.id, response: {
        clientDataJSON: toB64u(r.clientDataJSON),
        authenticatorData: toB64u(r.authenticatorData),
        signature: toB64u(r.signature),
        userHandle: r.userHandle ? toB64u(r.userHandle) : ''
      } } };
      probe.token = ''; probe.off = false; probeIn = probeEvery;
      return call('POST', '/api/v1/session', body).then(function (made) {
        if (made.status === 201) { load(false); return; }
        say('signin-status', t('That did not work. Sign in with your password.'), false);
      });
    }).catch(function () { say('signin-status', t('That did not work. Sign in with your password.'), false); });
  };
  el('signin-passkey').addEventListener('click', function () { passkeySignIn(); });
  el('signin-sso').addEventListener('click', function () { location.href = '/portal/oidc/start'; });
  var loadSignInMethods = function () {
    return call('GET', '/api/v1/signin-methods').then(function (result) {
      if (result.status !== 200 || !result.data) { return; }
      el('signin-passkey').hidden = !(result.data.passkeys && passkeysHere());
      if (result.data.oidc) {
        el('signin-sso').textContent = tf('Sign in with {0}', result.data.oidc_label || '');
        el('signin-sso').hidden = false;
      }
    });
  };
  // The identity provider sent the browser back with a one-use ticket in the
  // fragment, or with a sentence saying why it did not. The fragment is taken
  // out of the address before anything else reads it, so a reload does not
  // spend a ticket that is already gone.
  var singleSignOnReturn = function () {
    var hash = String(location.hash || '');
    var ticket = /^#sso=([A-Za-z0-9_-]+)$/.exec(hash);
    var failed = /^#sso_error=(.*)$/.exec(hash);
    if (!ticket && !failed) { return false; }
    try { history.replaceState(null, '', '#/'); } catch (e) { location.hash = '#/'; }
    // The sentence is percent-encoded by this server, but the fragment is
    // whatever the address bar holds, and a crafted link can put a broken
    // escape there. decodeURIComponent throws on one, and this runs while the
    // page is being built, so the throw would leave the reader an inert page
    // rather than a sign-in form: the raw text is shown instead.
    if (failed) {
      var sentence;
      try { sentence = decodeURIComponent(String(failed[1]).replace(/\+/g, ' ')); }
      catch (e) { sentence = String(failed[1]).replace(/\+/g, ' '); }
      say('signin-status', sentence, false);
      return true;
    }
    call('POST', '/api/v1/session', { sso_ticket: ticket[1] }).then(function (made) {
      if (made.status === 201) { load(false); return; }
      say('signin-status', describe(made, t('That did not work. Sign in with your password.')), false);
    });
    return true;
  };
  el('signout').addEventListener('click', function () {
    // The second account's session is ended first, with its own token, so
    // that nothing of this page's stays live on the server after the page
    // has signed out; then the first's, which clears the cookie. The page's
    // copy of the token goes before the request is made, so that a refusal
    // of it - the session already gone - is not read as the session ending
    // and answered with a reload of the first account under a sign-out.
    flushPending().then(function () {
      var token = accounts.second.token;
      secondForget();
      return token ? call('DELETE', '/api/v1/session', undefined, { 'Authorization': 'Bearer ' + token }, 'first') : null;
    }).then(function () { return call('DELETE', '/api/v1/session', undefined, undefined, 'first'); }).then(function () { probe.token = ''; probe.off = false; showSignIn(); });
  });
  el('theme-btn').addEventListener('click', function () {
    var next = document.body.getAttribute('data-theme') === 'light' ? 'dark' : 'light';
    applyTheme(next);
    if (!el('account').hidden) { savePrefs({ theme: next }); }
  });
  el('nav-compose').addEventListener('click', function () { go('/compose'); });
  (function () {
    var nav = el('view-nav').children;
    for (var i = 0; i < nav.length; i++) {
      (function (b) { b.addEventListener('click', function () { go(b.getAttribute('data-route')); }); })(nav[i]);
    }
  })();
  el('message-back').addEventListener('click', function () { closeMessage(false); });
  el('message-unread').addEventListener('click', function () { if (current) { setFlags({ seen: !current.flags.seen }, false); } });
  el('message-flag').addEventListener('click', function () { cycleStar(current); });
  el('message-delete').addEventListener('click', function () { fileCurrent('delete'); });
  var remoteAllowed = { id: 0 };
  var remoteSenders = function () { return String(pref('remote_senders') || '').toLowerCase().split(',').filter(Boolean); };
  el('remote-once').addEventListener('click', function () { if (current) { remoteAllowed = { id: current.id }; renderHtml(); } });
  el('remote-always').addEventListener('click', function () {
    if (!current) { return; }
    var sender = addressOf(current.from).toLowerCase();
    var list = remoteSenders();
    if (sender && list.indexOf(sender) < 0) { list.push(sender); }
    savePrefs({ remote_senders: list.join(',') }).then(function (ok) {
      if (!ok) { say('mail-status', 'Could not remember this sender: the list is full.', false); return; }
      renderHtml();
    });
  });
  // Links whose text names one site and go to another, and a sender whose
  // name is a contact's but whose address is not: the two shapes of a
  // forgery a reader can be shown without being asked to read headers.
  var hostOf = function (url) { try { return new URL(url, location.origin).hostname.toLowerCase().replace(/^www\./, ''); } catch (e) { return ''; } };
  var displayNameOf = function (header) { var m = /^\s*"?([^"<]*?)"?\s*</.exec(header || ''); return m ? m[1].trim() : ''; };
  // Read with the parser, which is inert and linear: a regex over a
  // megabyte of HTML a sender wrote could freeze the tab.
  var linkWarnings = function (html) {
    var out = [];
    var doc;
    try { doc = new DOMParser().parseFromString(html, 'text/html'); } catch (e) { return out; }
    if (!doc || typeof doc.getElementsByTagName !== 'function') { return out; }
    var anchors = doc.getElementsByTagName('a');
    for (var i = 0; i < anchors.length && out.length < 5; i++) {
      var text = (anchors[i].textContent || '').trim();
      if (!/^(https?:\/\/)?(www\.)?[a-z0-9-]+(\.[a-z0-9-]+)+(\/\S*)?$/i.test(text)) { continue; }
      var said = hostOf(/^https?:/i.test(text) ? text : 'https://' + text);
      var goes = hostOf(anchors[i].getAttribute('href') || '');
      if (said && goes && said !== goes) { out.push('A link that says ' + text + ' goes to ' + goes + '.'); }
    }
    return out;
  };
  var showWarnings = function (list) {
    var box = el('message-warnings');
    clear(box);
    list.forEach(function (w) { box.appendChild(node('div', w)); });
    box.hidden = !list.length;
  };
  var ensureContacts = function () {
    if (contactsCache) { return Promise.resolve(contactsCache); }
    return loadContacts();
  };
  var warnAbout = function (m) {
    // What the server decided, first, because it knows two things this page
    // cannot: whether the session that sent this authenticated, and whether
    // the sender has ever written to this mailbox before. Both lines appear
    // only when the recipient's domain asked for them.
    var warnings = [];
    if (m.external_stamped) { warnings.push(t('This message came from outside your organisation.')); }
    if (m.first_contact) { warnings.push(t('You do not usually get mail from this sender.')); }
    warnings = warnings.concat(linkWarnings(m.html || ''));
    showWarnings(warnings);
    ensureContacts().then(function (list) {
      if (current !== m) { return; }
      var name = displayNameOf(m.from);
      var address = addressOf(m.from).toLowerCase();
      if (!name) { return; }
      var same = (list || []).filter(function (c) { return c.kind !== 'group' && (c.name || '').toLowerCase() === name.toLowerCase() && c.address.toLowerCase() !== address; })[0];
      if (same) { warnings.push('The sender is named like your contact ' + same.name + ' <' + same.address + '>, but this came from ' + address + '.'); showWarnings(warnings); }
    });
  };
  // Images among the attachments are shown; a PDF is previewed on request,
  // in a frame of its own, from bytes the page fetched itself.
  var fetchAsDataUrl = function (id, index) {
    return apiFetch('/api/v1/me/messages/' + id + '/attachments/' + index, { cache: 'no-store', credentials: 'same-origin' }).then(function (response) {
      if (response.status !== 200) { return null; }
      return response.blob();
    }).then(function (blob) {
      if (!blob || String(blob.type || '').indexOf('image/') !== 0) { return null; }
      return new Promise(function (resolve) {
        var reader = new FileReader();
        reader.onload = function () { resolve(String(reader.result)); };
        reader.onerror = function () { resolve(null); };
        reader.readAsDataURL(blob);
      });
    }, function () { return null; });
  };
  // Previews keep to the inline budget - twelve parts, twelve megabytes -
  // and a part the HTML already shows in place is not fetched again.
  var previewAttachments = function (m) {
    var previews = el('message-previews');
    clear(previews);
    var budget = INLINE_TOGETHER, count = 0;
    (m.attachments || []).forEach(function (a) {
      var type = String(a.content_type || '').toLowerCase();
      var cid = contentIdOf(a);
      if (cid && m.html && m.html.toLowerCase().indexOf('cid:' + cid.toLowerCase()) >= 0) { return; }
      if (count >= INLINE_MOST || a.size > budget) { return; }
      if (RENDERS[type] && a.size <= INLINE_EACH) {
        count++; budget -= a.size;
        fetchAsDataUrl(m.id, a.index).then(function (url) {
          if (!url || current !== m) { return; }
          var img = document.createElement('img'); img.setAttribute('loading', 'lazy'); img.setAttribute('src', url); img.setAttribute('alt', a.name); img.setAttribute('title', a.name);
          previews.appendChild(img);
        });
      } else if (type === 'application/pdf' && a.size <= INLINE_TOGETHER) {
        var show = button(t('Preview ') + a.name);
        show.addEventListener('click', function () {
          apiFetch('/api/v1/me/messages/' + m.id + '/attachments/' + a.index, { cache: 'no-store', credentials: 'same-origin' }).then(function (response) { return response.status === 200 ? response.blob() : null; }).then(function (blob) {
            if (!blob || current !== m) { return; }
            var frame = document.createElement('iframe');
            frame.setAttribute('referrerpolicy', 'no-referrer'); frame.setAttribute('title', a.name);
            frame.setAttribute('src', URL.createObjectURL(new Blob([blob], { type: 'application/pdf' })));
            previews.appendChild(frame);
            show.hidden = true;
          });
        });
        previews.appendChild(show);
      }
    });
  };
  var fileCurrent = function (to) {
    if (!current) { return; }
    var was = current;
    fileMany([{ id: was.id, folderId: was.folder_id }], to).then(function (ok) {
      if (!ok) { return; }
      if (current === was) { closeMessage(true); }
      loadMessages();
    });
  };
  el('message-archive').addEventListener('click', function () { fileCurrent('archive'); });
  el('message-junk').addEventListener('click', function () { if (current) { fileCurrent(junkTargetFor(current.folder_id)); } });
  el('message-headers-toggle').addEventListener('click', function () {
    var pane = el('message-headers');
    pane.hidden = !pane.hidden;
    el('message-headers-toggle').textContent = pane.hidden ? 'Headers' : 'Hide headers';
  });
  document.addEventListener('keydown', function (event) {
    var tag = (event.target && event.target.tagName) || '';
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || event.ctrlKey || event.metaKey || event.altKey) { return; }
    if (el('account').hidden || el('mail-section').hidden || el('message-view').hidden || !current) { return; }
    if (event.key === 'e') { fileCurrent('archive'); event.preventDefault(); }
    else if (event.key === '!') { fileCurrent(junkTargetFor(current.folder_id)); event.preventDefault(); }
    else if (event.key === '#') { fileCurrent('delete'); event.preventDefault(); }
    else if (event.key === 'l') { toggleLabelMenu(); event.preventDefault(); }
    else if (event.key === 'm') { muteThread(entryOfCurrent(), !isMuted(current)); event.preventDefault(); }
  });
  el('message-move-go').addEventListener('click', function () {
    var target = el('message-move').value;
    if (!current || !target) { return; }
    var was = current;
    var back = was.folder_id;
    call('POST', '/api/v1/me/messages/' + was.id + '/move', { folder_id: Number(target) }).then(function (result) {
      if (result.status === 200) {
        lastListing = null;
        loadFolders();
        if (current === was) { closeMessage(true); }
        loadMessages();
        var moved = result.data && result.data.id;
        toast(t('Moved to ') + folderName(Number(target)), moved && back ? function () {
          call('POST', '/api/v1/me/messages/' + moved + '/move', { folder_id: back }).then(function () { lastListing = null; loadFolders(); loadMessages(); });
        } : null);
        return;
      }
      say('mail-status', describe(result, t('Could not move')), false);
    });
  });
  el('message-html-toggle').addEventListener('click', function () {
    showHtml = !showHtml;
    el('message-html-toggle').textContent = showHtml ? 'Show as text' : 'Show as sent';
    renderHtml();
  });
  el('message-reply').addEventListener('click', function () { if (current) { go('/compose?reply=' + current.id); } });
  el('message-reply-all').addEventListener('click', function () { if (current) { go('/compose?replyall=' + current.id); } });
  el('message-forward').addEventListener('click', function () { if (current) { go('/compose?forward=' + current.id); } });
  el('message-edit').addEventListener('click', function () { if (current) { go('/compose?draft=' + current.id); } });
  el('compose-discard').addEventListener('click', function () { composeKey = newComposeKey(); blankCompose(); closeCompose(); toast(t('Discarded.')); });
  // ---- Address books ---------------------------------------------------------
  // The page works in one address book at a time. The account's own come first,
  // the default before the rest, then the books other people share with it -
  // read-only or writable as the share says. Sharing is the owner's panel: an
  // address shares with one account, a domain name with every account in it.
  var bookState = { books: [], current: 0, detail: false };
  var BOOK_CHOSEN = 'hm-portal-book';
  var bookById = function (id) {
    var found = null;
    bookState.books.forEach(function (b) { if (b.id === id) { found = b; } });
    return found;
  };
  var currentBook = function () { return bookById(bookState.current); };
  var renderBookDetail = function () {
    var box = el('book-detail');
    clear(box);
    var b = currentBook();
    box.hidden = !bookState.detail || !b;
    if (box.hidden) { return; }
    if (!b.mine) {
      box.appendChild(node('p', tf('Shared by {0}', b.owner) + ' \u00b7 ' + (b.writable ? t('you can add and change contacts') : t('read-only')), 'hint'));
      var leave = button(t('Leave this address book'), 'btn ghost sm');
      leave.addEventListener('click', function () {
        call('DELETE', '/api/v1/me/address-books/' + b.id).then(function (result) {
          if (result.status !== 200) { say('book-status', describe(result, t('Could not leave the address book')), false); return; }
          bookState.current = 0; bookState.detail = false;
          loadBooks().then(loadContacts);
        });
      });
      box.appendChild(leave);
      return;
    }
    var form = node('form', undefined, 'row');
    var name = document.createElement('input');
    name.type = 'text'; name.value = b.name; name.id = 'book-detail-name';
    name.setAttribute('aria-label', t('Address book name')); name.setAttribute('maxlength', '255');
    var save = button(t('Save'), 'btn sm'); save.type = 'submit';
    form.appendChild(name); form.appendChild(save);
    form.addEventListener('submit', function (event) {
      event.preventDefault();
      call('PUT', '/api/v1/me/address-books/' + b.id, { name: name.value }).then(function (result) {
        if (result.status !== 200) { say('book-status', describe(result, t('Could not save the address book')), false); return; }
        loadBooks();
      });
    });
    box.appendChild(form);

    box.appendChild(node('h3', t('Sharing')));
    var shareForm = node('form', undefined, 'row');
    var target = document.createElement('input');
    target.type = 'text'; target.id = 'book-share-target';
    target.setAttribute('placeholder', 'someone@' + (String(me || '').split('@')[1] || 'example.com'));
    target.setAttribute('aria-label', t('Share with'));
    var writeLabel = node('label');
    var write = document.createElement('input'); write.type = 'checkbox'; write.id = 'book-share-write';
    writeLabel.appendChild(write); writeLabel.appendChild(node('span', ' ' + t('Can edit')));
    var share = button(t('Share'), 'btn sm'); share.type = 'submit';
    shareForm.appendChild(target); shareForm.appendChild(writeLabel); shareForm.appendChild(share);
    shareForm.addEventListener('submit', function (event) {
      event.preventDefault();
      var who = target.value.trim().toLowerCase();
      if (!who) { return; }
      call('PUT', '/api/v1/me/address-books/' + b.id + '/shares/' + encodeURIComponent(who), { write: write.checked }).then(function (result) {
        if (result.status !== 200) { say('book-status', describe(result, t('Could not share the address book')), false); return; }
        say('book-status', tf('Shared with {0}.', who), true);
        target.value = '';
        loadBooks();
      });
    });
    box.appendChild(shareForm);
    (b.shares || []).forEach(function (s) {
      var row = node('div', undefined, 'row book-share');
      row.appendChild(node('span', s.target + ' \u00b7 ' + (s.write ? t('can edit') : t('read-only')) +
        (s.kind === 'domain' ? ' \u00b7 ' + t('every account in the domain') : '')));
      var remove = button(t('Remove'), 'btn quiet sm');
      remove.addEventListener('click', function () {
        call('DELETE', '/api/v1/me/address-books/' + b.id + '/shares/' + encodeURIComponent(s.target)).then(function (result) {
          if (result.status !== 200) { say('book-status', describe(result, t('Could not remove the share')), false); return; }
          loadBooks();
        });
      });
      row.appendChild(remove);
      box.appendChild(row);
    });
    box.appendChild(node('p', t('Someone you share with sees these contacts in their webmail and on their phone, read-only unless they can edit.'), 'hint'));
    if (!b.is_default) {
      var del = button(t('Delete this address book'), 'btn quiet sm');
      del.addEventListener('click', function () {
        if (!window.confirm(tf('Delete the address book "{0}" and every contact in it?', b.name))) { return; }
        call('DELETE', '/api/v1/me/address-books/' + b.id).then(function (result) {
          if (result.status !== 200) { say('book-status', describe(result, t('Could not delete the address book')), false); return; }
          bookState.current = 0; bookState.detail = false;
          loadBooks().then(loadContacts);
        });
      });
      box.appendChild(del);
    }
  };
  var renderBooks = function () {
    var select = el('book-select');
    clear(select);
    bookState.books.forEach(function (b) {
      var o = document.createElement('option');
      o.value = String(b.id);
      o.textContent = (b.mine ? b.name : b.name + ' (' + b.owner + ')') + ' \u00b7 ' + b.contacts;
      select.appendChild(o);
    });
    select.value = String(bookState.current);
    renderBookDetail();
  };
  var loadBooks = function () {
    return call('GET', '/api/v1/me/address-books').then(function (result) {
      if (result.status !== 200 || !result.data) { say('book-status', describe(result, t('Could not read the address books')), false); return []; }
      bookState.books = result.data.books || [];
      if (!bookById(bookState.current)) {
        var remembered = 0;
        try { remembered = parseInt(localStorage.getItem(BOOK_CHOSEN) || '0', 10) || 0; } catch (e) { remembered = 0; }
        bookState.current = bookById(remembered) ? remembered : (bookState.books.length ? bookState.books[0].id : 0);
      }
      renderBooks();
      return bookState.books;
    });
  };
  el('book-select').addEventListener('change', function () {
    bookState.current = parseInt(el('book-select').value, 10) || 0;
    try { localStorage.setItem(BOOK_CHOSEN, String(bookState.current)); } catch (e) { /* no storage */ }
    renderBookDetail();
    renderContacts(contactsCache || []);
  });
  el('book-manage').addEventListener('click', function () { bookState.detail = !bookState.detail; renderBookDetail(); });
  el('book-form').addEventListener('submit', function (event) {
    event.preventDefault();
    say('book-status', '', true);
    call('POST', '/api/v1/me/address-books', { name: el('book-name').value }).then(function (result) {
      if (result.status !== 201) { say('book-status', describe(result, t('Could not make the address book')), false); return; }
      el('book-name').value = '';
      bookState.current = (result.data && result.data.id) || bookState.current;
      bookState.detail = true;
      loadBooks().then(loadContacts);
    });
  });

  // ---- Contacts ------------------------------------------------------------
  var contactsCache = null;
  // The card a phone stored, beside the name: the birthday (marked on the
  // day), the phones, the note - what the listing reads from it - and the
  // photo as the avatar, fetched as an image from the contact's own route.
  var contactDetails = function (c) {
    var bits = [];
    if (c.birthday) {
      var now = new Date();
      var monthDay = ('0' + (now.getMonth() + 1)).slice(-2) + '-' + ('0' + now.getDate()).slice(-2);
      bits.push(t('Birthday') + ' ' + c.birthday + (String(c.birthday).slice(-5) === monthDay ? ' \u00b7 ' + t('today') : ''));
    }
    (c.phones || []).forEach(function (p) { bits.push(p); });
    if (c.note) { bits.push(c.note.length > 120 ? c.note.slice(0, 117) + '...' : c.note); }
    return bits.join(' \u00b7 ');
  };
  var contactAvatar = function (c) {
    if (c.has_photo) {
      var img = document.createElement('img'); img.className = 'avatar'; img.alt = '';
      imageFrom(img, '/api/v1/me/contacts/' + c.id + '/photo');
      return img;
    }
    return node('span', initialsOf(c.name || c.address), 'avatar');
  };
  var contactButtons = function (c) {
    var write = document.createElement('button'); write.type = 'button'; write.className = 'btn ghost'; write.textContent = t('Write');
    write.addEventListener('click', function () { composeTo(c.name ? c.name + ' <' + c.address + '>' : c.address); });
    // A book shared read-only offers Write and nothing else: the server would
    // refuse the delete, and a button that cannot work is worse than none.
    if (c.book_writable === false) { return [write]; }
    var remove = document.createElement('button'); remove.type = 'button'; remove.className = 'btn ghost'; remove.textContent = t('Remove');
    remove.addEventListener('click', function () {
      call('DELETE', '/api/v1/me/contacts/' + c.id).then(function (result) {
        if (result.status !== 200) { say('contact-status', describe(result, t('Could not remove the contact')), false); return; }
        contactsCache = null; loadContacts();
        toast(t('Contact removed.'), function () {
          call('POST', '/api/v1/me/contacts', { name: c.name || '', address: c.address, book: c.book_id || bookState.current }).then(function (back) {
            if (back.status !== 201) { say('contact-status', describe(back, t('Could not add the contact')), false); }
            contactsCache = null; loadContacts();
          });
        });
      });
    });
    return [write, remove];
  };
  // What the person kept, and - apart - what they merely wrote to: a collected
  // address is a suggestion until it is kept, and only a kept one reaches
  // the phone over CardDAV.
  var renderContacts = function (all) {
    // The listing carries every book the reader can see (book=all) and each
    // contact says which it is in; this draws the one chosen above. Groups are
    // one account's own and are drawn for the reader's own books only.
    var list = all.filter(function (c) { return !bookState.current || c.book_id === bookState.current; });
    var book = currentBook();
    var writable = !book || book.writable !== false;
    el('contact-form').hidden = !writable;
    el('contact-readonly').hidden = writable;
    // Import goes with the add form: every card it posts would be refused 403
    // by a book shared read-only and the count would simply say none were
    // imported, which is the same reason the Remove button is not drawn there.
    // Export stays: reading a book shared with you is the whole point of it.
    el('contact-import').hidden = !writable;
    var kept = list.filter(function (c) { return c.kind !== 'group' && c.source !== 'collected'; });
    var other = list.filter(function (c) { return c.kind !== 'group' && c.source === 'collected'; });
    renderGroups(list.filter(function (c) { return c.kind === 'group' && c.book_mine !== false; }));
    var rows = el('contact-rows');
    rows.textContent = '';
    kept.forEach(function (c) {
      var tr = document.createElement('tr');
      var name = document.createElement('td'); name.appendChild(contactAvatar(c)); name.appendChild(node('span', c.name || ''));
      var address = document.createElement('td'); address.textContent = c.address;
      var details = document.createElement('td'); details.className = 'muted'; details.textContent = contactDetails(c);
      if (c.note) { details.title = c.note; }
      var actions = document.createElement('td');
      contactButtons(c).forEach(function (b) { actions.appendChild(b); });
      tr.appendChild(name); tr.appendChild(address); tr.appendChild(details); tr.appendChild(actions);
      rows.appendChild(tr);
    });
    el('contact-empty').hidden = kept.length > 0;
    el('contact-table').hidden = kept.length === 0;
    var otherRows = el('contact-other-rows');
    otherRows.textContent = '';
    other.forEach(function (c) {
      var tr = document.createElement('tr');
      var name = document.createElement('td'); name.textContent = c.name || '';
      var address = document.createElement('td'); address.textContent = c.address;
      var actions = document.createElement('td');
      var keep = document.createElement('button'); keep.type = 'button'; keep.className = 'btn ghost'; keep.textContent = t('Keep');
      keep.addEventListener('click', function () {
        call('PUT', '/api/v1/me/contacts/' + c.id, { source: 'manual' }).then(function (result) {
          if (result.status !== 200) { say('contact-status', describe(result, t('Could not keep the contact')), false); return; }
          contactsCache = null; loadContacts();
          toast(t('Kept as a contact.'));
        });
      });
      actions.appendChild(keep);
      contactButtons(c).forEach(function (b) { actions.appendChild(b); });
      tr.appendChild(name); tr.appendChild(address); tr.appendChild(actions);
      otherRows.appendChild(tr);
    });
    el('contact-other').hidden = other.length === 0;
  };
  var loadContacts = function () {
    return call('GET', '/api/v1/me/contacts?limit=1000&book=all').then(function (result) {
      if (result.status !== 200 || !result.data) { say('contact-status', describe(result, t('Could not read the contacts')), false); return []; }
      contactsCache = result.data.contacts || [];
      renderContacts(contactsCache);
      return contactsCache;
    });
  };
  el('contact-form').addEventListener('submit', function (event) {
    event.preventDefault();
    say('contact-status', '', true);
    call('POST', '/api/v1/me/contacts', { name: el('contact-name').value, address: el('contact-address').value, book: bookState.current }).then(function (result) {
      if (result.status === 201) { el('contact-name').value = ''; el('contact-address').value = ''; contactsCache = null; loadContacts(); return; }
      say('contact-status', describe(result, t('Could not add the contact')), false);
    });
  });

  // ---- Contact groups --------------------------------------------------------
  // A group is a row of the address book with members instead of an address.
  // The listing carries it with kind group, its members as its card names
  // them, and its recipients - every person reached, a group in a group
  // expanded - so the To field offers it by name and expands it with no
  // second request. Made, renamed, filled and emptied here; a phone that
  // syncs the address book makes and reads them too, as vCard groups.
  var recipientEntry = function (r) { return r.name ? r.name + ' <' + r.address + '>' : r.address; };
  var groupEntries = function (g) { return (g.recipients || []).map(recipientEntry).join(', '); };
  var groupCall = function (method, path, body, failure) {
    return call(method, path, body).then(function (result) {
      if (result.status !== 200 && result.status !== 201) { say('group-status', describe(result, failure), false); return null; }
      say('group-status', '', true);
      contactsCache = null;
      return loadContacts().then(function () { return result; });
    });
  };
  var renderGroups = function (groups) {
    var rows = el('group-rows');
    clear(rows);
    groups.forEach(function (g) {
      var tr = document.createElement('tr');
      var name = document.createElement('td');
      var label = node('span', g.name || '');
      name.appendChild(label);
      // The name as a field, shown in place of the label by Rename: Enter
      // saves, Escape puts the label back.
      var field = document.createElement('input'); field.type = 'text'; field.maxLength = 255; field.value = g.name || ''; field.hidden = true;
      field.setAttribute('aria-label', t('Group name'));
      field.addEventListener('keydown', function (e) {
        if (e.key === 'Enter') {
          e.preventDefault();
          var wanted = String(field.value || '').trim();
          if (!wanted || wanted === g.name) { field.hidden = true; label.hidden = false; return; }
          groupCall('PUT', '/api/v1/me/contacts/groups/' + g.id, { name: wanted }, t('Could not rename the group'));
        } else if (e.key === 'Escape') { e.preventDefault(); field.hidden = true; label.hidden = false; }
      });
      name.appendChild(field);
      var members = document.createElement('td');
      (g.members || []).forEach(function (m) {
        var line = node('div', undefined, 'row');
        line.appendChild(node('span', m.kind === 'group' ? tf('{0} (group)', m.name) : recipientEntry(m)));
        var remove = node('button', t('Remove'), 'btn ghost sm'); remove.type = 'button';
        remove.addEventListener('click', function () {
          groupCall('DELETE', '/api/v1/me/contacts/groups/' + g.id + '/members/' + encodeURIComponent(m.id ? String(m.id) : m.address), null, t('Could not remove the member'));
        });
        line.appendChild(remove);
        members.appendChild(line);
      });
      if (!(g.members || []).length) { members.appendChild(node('div', t('No members yet.'), 'muted')); }
      // One more: a contact by name, from the same completion the To field
      // has, an address that is not a contact, or another group of this book
      // by its name - the completion offers people alone, since picking a
      // group there would write out its people, but a name typed in that is
      // another group's goes in as that group.
      var add = document.createElement('form'); add.className = 'row';
      var wrap = node('span', undefined, 'complete-wrap');
      var input = document.createElement('input'); input.type = 'text'; input.id = 'group-member-' + g.id; input.maxLength = 255;
      input.placeholder = t('Name or address'); input.setAttribute('aria-label', t('Add a member'));
      wrap.appendChild(input); add.appendChild(wrap);
      var addButton = node('button', t('Add'), 'btn ghost sm'); addButton.type = 'submit'; add.appendChild(addButton);
      add.addEventListener('submit', function (event) {
        event.preventDefault();
        var entry = String(input.value || '').trim().replace(/,\s*$/, '').trim();
        if (!entry) { return; }
        var bracketed = /<([^>]+)>\s*$/.exec(entry);
        var address = bracketed ? bracketed[1].trim() : entry;
        var inner = address.indexOf('@') >= 0 ? null : (groups || []).filter(function (other) {
          return other.id !== g.id && String(other.name || '').toLowerCase() === address.toLowerCase();
        })[0];
        groupCall('POST', '/api/v1/me/contacts/groups/' + g.id + '/members',
          inner ? { id: inner.id } : { address: address }, t('Could not add the member'));
      });
      members.appendChild(add);
      var actions = document.createElement('td');
      var write = node('button', t('Write'), 'btn ghost sm'); write.type = 'button';
      write.addEventListener('click', function () { composeTo(groupEntries(g) + (g.recipients && g.recipients.length ? ', ' : '')); });
      var rename = node('button', t('Rename'), 'btn ghost sm'); rename.type = 'button';
      rename.addEventListener('click', function () { label.hidden = true; field.hidden = false; field.value = g.name || ''; field.focus(); });
      var remove = node('button', t('Delete'), 'btn ghost sm'); remove.type = 'button';
      remove.addEventListener('click', function () {
        var again = { name: g.name, members: (g.members || []).map(function (m) { return m.id ? { id: m.id } : { address: m.address }; }) };
        groupCall('DELETE', '/api/v1/me/contacts/groups/' + g.id, null, t('Could not remove the group')).then(function (result) {
          if (!result) { return; }
          toast(t('Group removed.'), function () { groupCall('POST', '/api/v1/me/contacts/groups', again, t('Could not create the group')); });
        });
      });
      actions.appendChild(write); actions.appendChild(rename); actions.appendChild(remove);
      tr.appendChild(name); tr.appendChild(members); tr.appendChild(actions);
      rows.appendChild(tr);
      complete(input, { persons: true });
    });
    el('group-table').hidden = groups.length === 0;
    el('group-empty').hidden = groups.length > 0;
  };
  el('group-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var wanted = String(el('group-name').value || '').trim();
    if (!wanted) { return; }
    groupCall('POST', '/api/v1/me/contacts/groups', { name: wanted, members: [] }, t('Could not create the group')).then(function (result) {
      if (result) { el('group-name').value = ''; }
    });
  });

  // ---- Contacts in and out ---------------------------------------------------
  // The address book as a file and back: vCard 3.0, one card per contact, is
  // what every phone and mail program reads; CSV with the header row Google
  // Contacts and Outlook write is what people already have. An import adds
  // what is not there yet, by address, and says how many it added.
  var vcardEscape = function (s) { return String(s || '').replace(/\\/g, '\\\\').replace(/\n/g, '\\n').replace(/,/g, '\\,').replace(/;/g, '\\;'); };
  var vcardUnescape = function (s) { return String(s || '').replace(/\\n/gi, '\n').replace(/\\([\\,;])/g, '$1'); };
  var contactsToVCard = function (list) {
    return list.map(function (c) {
      // A card a phone stored goes out whole; a row without one is made into a card.
      if (c.card) {
        var card = String(c.card).replace(/\r?\n/g, '\r\n');
        return card.slice(-2) === '\r\n' ? card : card + '\r\n';
      }
      // A group without its card goes out as the group card Apple's Contacts
      // and Thunderbird read, its members by address.
      if (c.kind === 'group') {
        return ['BEGIN:VCARD', 'VERSION:3.0', 'FN:' + vcardEscape(c.name), 'N:' + vcardEscape(c.name) + ';;;;', 'X-ADDRESSBOOKSERVER-KIND:group']
          .concat((c.recipients || []).map(function (r) { return 'X-ADDRESSBOOKSERVER-MEMBER:mailto:' + vcardEscape(r.address); }))
          .concat(['END:VCARD']).join('\r\n') + '\r\n';
      }
      var name = c.name || c.address;
      var words = String(c.name || '').trim().split(/\s+/).filter(function (w) { return w; });
      var family = words.length > 1 ? words[words.length - 1] : '';
      var given = words.length > 1 ? words.slice(0, -1).join(' ') : (words[0] || '');
      return ['BEGIN:VCARD', 'VERSION:3.0', 'FN:' + vcardEscape(name), 'N:' + vcardEscape(family) + ';' + vcardEscape(given) + ';;;',
        'EMAIL;TYPE=INTERNET:' + vcardEscape(c.address), 'END:VCARD'].join('\r\n') + '\r\n';
    }).join('');
  };
  var csvCell = function (s) { s = String(s || ''); return /[",\r\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s; };
  var contactsToCsv = function (list) {
    // A group has no address for the column; the vCard export carries it.
    return 'Name,E-mail Address\r\n' + list.filter(function (c) { return c.kind !== 'group'; }).map(function (c) { return csvCell(c.name) + ',' + csvCell(c.address) + '\r\n'; }).join('');
  };
  var parseVCards = function (text) {
    var lines = String(text).replace(/\r\n?/g, '\n').replace(/\n[ \t]/g, '').split('\n');
    var out = [];
    var card = null;
    lines.forEach(function (line) {
      var colon = line.indexOf(':');
      if (colon < 0) { return; }
      var head = line.slice(0, colon); var value = line.slice(colon + 1);
      var name = head.split(';')[0].toUpperCase();
      if (name === 'BEGIN' && value.toUpperCase() === 'VCARD') { card = { name: '', n: '', emails: [], group: false, members: [], raw: [line] }; return; }
      if (!card) { return; }
      card.raw.push(line);
      if (name === 'END') {
        var display = card.name || card.n;
        // A group card - KIND:group, or the X- spelling Apple writes - comes
        // in as a group, with the members it names by address. One naming a
        // member by urn:uuid means a card of the address book it came from,
        // which is not this one, so only the addresses are read.
        if (card.group) { if (display) { out.push({ group: true, name: display, members: card.members }); } card = null; return; }
        // The card itself rides along, so what a phone stored is kept as sent.
        var whole = card.raw.join('\r\n') + '\r\n';
        card.emails.forEach(function (address, i) { if (address.indexOf('@') > 0) { out.push(i === 0 ? { name: display, address: address, card: whole } : { name: display, address: address }); } });
        card = null; return;
      }
      if (/ENCODING=QUOTED-PRINTABLE/i.test(head)) { return; }
      if (name === 'FN') { card.name = vcardUnescape(value).trim(); }
      else if (name === 'N') { var parts = value.split(';'); card.n = [vcardUnescape(parts[1] || ''), vcardUnescape(parts[0] || '')].join(' ').trim(); }
      else if (name === 'EMAIL') { card.emails.push(vcardUnescape(value).trim()); }
      else if (name === 'KIND' || name === 'X-ADDRESSBOOKSERVER-KIND') { card.group = vcardUnescape(value).trim().toLowerCase() === 'group'; }
      else if (name === 'MEMBER' || name === 'X-ADDRESSBOOKSERVER-MEMBER') {
        var member = vcardUnescape(value).trim().replace(/^mailto:/i, '');
        if (member.indexOf('@') > 0 && member.indexOf(':') < 0) { card.members.push(member); }
      }
    });
    return out;
  };
  var parseCsvRows = function (text) {
    var rows = []; var row = []; var cell = ''; var quoted = false;
    var s = String(text).replace(/^\uFEFF/, '');
    for (var i = 0; i < s.length; i++) {
      var ch = s[i];
      if (quoted) {
        if (ch === '"') { if (s[i + 1] === '"') { cell += '"'; i++; } else { quoted = false; } }
        else { cell += ch; }
      } else if (ch === '"') { quoted = true; }
      else if (ch === ',') { row.push(cell); cell = ''; }
      else if (ch === '\n' || ch === '\r') { if (ch === '\r' && s[i + 1] === '\n') { i++; } row.push(cell); rows.push(row); row = []; cell = ''; }
      else { cell += ch; }
    }
    if (cell !== '' || row.length) { row.push(cell); rows.push(row); }
    return rows.filter(function (r) { return r.some(function (c) { return c.trim() !== ''; }); });
  };
  var parseContactsCsv = function (text) {
    var rows = parseCsvRows(text);
    if (rows.length < 2) { return []; }
    var header = rows[0].map(function (h) { return h.trim().toLowerCase(); });
    var emailCols = []; header.forEach(function (h, i) { if (/^e-?mail/.test(h) && !/type|label/.test(h)) { emailCols.push(i); } });
    var col = function (names) { for (var i = 0; i < header.length; i++) { if (names.indexOf(header[i]) >= 0) { return i; } } return -1; };
    var nameCol = col(['name', 'display name', 'full name']);
    var firstCol = col(['first name', 'given name']); var lastCol = col(['last name', 'family name', 'surname']);
    if (!emailCols.length) { return []; }
    var out = [];
    rows.slice(1).forEach(function (r) {
      var name = (nameCol >= 0 ? r[nameCol] : '') || [firstCol >= 0 ? r[firstCol] : '', lastCol >= 0 ? r[lastCol] : ''].join(' ');
      name = String(name || '').trim();
      emailCols.forEach(function (i) { var address = String(r[i] || '').trim(); if (address.indexOf('@') > 0) { out.push({ name: name, address: address }); } });
    });
    return out;
  };
  var downloadText = function (fileName, type, text) {
    // The text is left on the section as well: a page without object URLs
    // (a test harness) can still be asked what it would have sent.
    el('contacts-section').setAttribute('data-last-export', text);
    try {
      var link = document.createElement('a');
      link.href = URL.createObjectURL(new Blob([text], { type: type }));
      link.setAttribute('download', fileName);
      document.body.appendChild(link);
      if (typeof link.click === 'function') { link.click(); }
      document.body.removeChild(link);
    } catch (why) { /* nothing to download with */ }
  };
  var exportContacts = function (asCsv) {
    // What leaves in a file is the reader's own address books. A book somebody
    // shares with them is theirs, and exporting it here would take a copy of
    // somebody else's contacts out of the server without their knowing.
    var list = (contactsCache || []).filter(function (c) { return c.book_mine !== false; });
    if (!list.length) { say('contact-status', t('No contacts yet. Send a message, or add one above.'), false); return; }
    if (asCsv) {
      downloadText('contacts.csv', 'text/csv', contactsToCsv(list));
      say('contact-status', tf('Exported {0} contact(s).', list.length), true);
      return;
    }
    // The cards as the phones stored them, for the vCard export.
    call('GET', '/api/v1/me/contacts?limit=1000&fields=card').then(function (result) {
      var withCards = (result.status === 200 && result.data && result.data.contacts) ? result.data.contacts : list;
      downloadText('contacts.vcf', 'text/vcard', contactsToVCard(withCards));
      say('contact-status', tf('Exported {0} contact(s).', withCards.length), true);
    });
  };
  el('contact-export-vcf').addEventListener('click', function () { exportContacts(false); });
  el('contact-export-csv').addEventListener('click', function () { exportContacts(true); });
  el('contact-import-file').addEventListener('change', function () {
    var input = el('contact-import-file');
    var file = (input.files || [])[0];
    if (!file) { return; }
    say('contact-status', t('Importing...'), true);
    file.text().then(function (text) {
      var list = /BEGIN:VCARD/i.test(text) ? parseVCards(text) : parseContactsCsv(text);
      input.value = '';
      if (!list.length) { say('contact-status', t('Nothing in that file looked like a contact.'), false); return; }
      // A contact is known by its address and a group by its name, which is
      // what each is addressed by; neither is imported twice.
      var known = {};
      var knownGroups = {};
      (contactsCache || []).forEach(function (c) {
        if (c.kind === 'group') { knownGroups[String(c.name).toLowerCase()] = true; } else { known[String(c.address).toLowerCase()] = true; }
      });
      var fresh = [];
      list.forEach(function (c) {
        var key = String(c.group ? c.name : c.address).toLowerCase();
        var seen = c.group ? knownGroups : known;
        if (!seen[key]) { seen[key] = true; fresh.push(c); }
      });
      var imported = 0;
      var chain = Promise.resolve();
      fresh.forEach(function (c) {
        chain = chain.then(function () {
          if (c.group) {
            var members = c.members.map(function (address) { return { address: address }; });
            return call('POST', '/api/v1/me/contacts/groups', { name: c.name, members: members })
              .then(function (result) { if (result.status === 201) { imported++; } });
          }
          var body = { name: c.name, address: c.address, book: bookState.current };
          if (c.card) { body.card = c.card; }
          return call('POST', '/api/v1/me/contacts', body).then(function (result) { if (result.status === 201) { imported++; } });
        });
      });
      return chain.then(function () {
        contactsCache = null;
        say('contact-status', tf('{0} contact(s) imported, {1} already there.', imported, list.length - imported), true);
        return loadContacts();
      });
    }, function () { say('contact-status', t('Nothing in that file looked like a contact.'), false); });
  });

  // ---- To-field completion ---------------------------------------------------
  // The token being typed is the text after the last comma; on each keystroke the
  // address book is asked for it (small, so a fetch per key is fine) and a popup
  // under the field offers up to eight matches. Enter or Tab takes the marked one,
  // Escape closes, arrows move, a click takes.
  // A group is offered by its name with how many it reaches, and taking it
  // writes every recipient into the field, as Gmail and Outlook do; the
  // member field of a group asks for people only (options.persons).
  var complete = function (inputOrId, options) {
    var input = typeof inputOrId === 'string' ? el(inputOrId) : inputOrId;
    var persons = !!(options && options.persons);
    var list = document.createElement('ul'); list.className = 'complete'; list.hidden = true; list.setAttribute('role', 'listbox');
    input.parentNode.appendChild(list);
    var items = []; var marked = -1; var timer = null;
    var close = function () { list.hidden = true; list.textContent = ''; items = []; marked = -1; };
    var token = function () { var v = input.value; var i = v.lastIndexOf(','); return { head: i >= 0 ? v.slice(0, i + 1) : '', tail: (i >= 0 ? v.slice(i + 1) : v).replace(/^\s+/, '') }; };
    var entryOf = function (c) { return c.kind === 'group' ? groupEntries(c) : (c.name ? c.name + ' <' + c.address + '>' : c.address); };
    var take = function (c) { var t = token(); var entry = entryOf(c); if (!entry) { close(); return; } input.value = (t.head ? t.head + ' ' : '') + entry + ', '; close(); input.focus(); };
    var mark = function (n) { marked = n; for (var i = 0; i < list.children.length; i++) { list.children[i].classList.toggle('on', i === marked); } };
    var show = function (offered) {
      close();
      // A group is one account's own address book entry and expands against
      // that account's WHOLE address book, so the server lists none from a
      // book somebody shares with the reader. The page says the same thing,
      // so that a listing which did carry one would still not expand it into
      // the names and addresses of contacts nobody shared.
      var found = offered.filter(function (c) { return !(c.kind === 'group' && c.book_mine === false); });
      if (!found.length) { return; }
      items = found;
      found.forEach(function (c, i) {
        var li = document.createElement('li'); li.setAttribute('role', 'option');
        var nm = document.createElement('span'); nm.className = 'nm'; nm.textContent = c.name || c.address;
        var ad = document.createElement('span'); ad.className = 'ad';
        // A directory hit says where it came from, so that a name the reader
        // has never written to is not mistaken for one of their own contacts.
        ad.textContent = c.kind === 'group' ? tf('{0} member(s)', (c.recipients || []).length)
          : c.directory ? (c.name ? c.address + ' - ' : '') + (c.list ? t('Distribution list') : t('Directory'))
          : (c.name ? c.address : '');
        if (c.book_mine === false) { ad.textContent = (ad.textContent ? ad.textContent + ' \u00b7 ' : '') + c.book_name; }
        li.appendChild(nm); li.appendChild(ad);
        li.addEventListener('mousedown', function (e) { e.preventDefault(); take(c); });
        list.appendChild(li);
      });
      list.hidden = false; mark(0);
    };
    input.setAttribute('autocomplete', 'off');
    input.addEventListener('input', function () {
      var q = token().tail;
      if (timer) { clearTimeout(timer); }
      if (!q) { close(); return; }
      timer = setTimeout(function () {
        // The reader's own contacts first, then the organisation's directory -
        // the people already known before the people merely findable, which is
        // the order Outlook and Gmail offer them in. An address that is both is
        // offered once, as the contact, because that is the one carrying the
        // name the reader chose. The member field of a group asks for people
        // the reader keeps, so it does not ask the directory at all.
        // The member field of a group takes the account's own contacts; the To,
        // Cc and Bcc fields take every book the reader can see, the shared ones
        // labelled with whose they are.
        var mine = call('GET', '/api/v1/me/contacts?limit=8&q=' + encodeURIComponent(q) + (persons ? '&kind=person' : '&book=all'));
        var theirs = persons ? Promise.resolve([]) : directorySuggest(q);
        Promise.all([mine, theirs]).then(function (both) {
          var result = both[0];
          if (result.status !== 200 || !result.data || token().tail !== q) { return; }
          var contacts = result.data.contacts || [];
          var known = {};
          contacts.forEach(function (c) { if (c.address) { known[String(c.address).toLowerCase()] = true; } });
          var extra = (both[1] || []).filter(function (e) { return e.address && !known[String(e.address).toLowerCase()]; })
            .map(function (e) { return { name: e.name, address: e.address, kind: 'person', directory: true, title: e.title, department: e.department, list: e.kind === 'list' }; });
          // Up to eight of the reader's own contacts, from every book they can
          // see, and then up to five people or lists from the directory who
          // are not among them: the known before the findable, and the
          // findable never pushed off the end by a full address book.
          show(contacts.slice(0, 8).concat(extra.slice(0, 5)));
        });
      }, 120);
    });
    input.addEventListener('keydown', function (e) {
      if (list.hidden) { return; }
      if (e.key === 'ArrowDown') { e.preventDefault(); mark((marked + 1) % items.length); }
      else if (e.key === 'ArrowUp') { e.preventDefault(); mark((marked - 1 + items.length) % items.length); }
      else if (e.key === 'Enter' || e.key === 'Tab') { if (marked >= 0) { e.preventDefault(); take(items[marked]); } }
      else if (e.key === 'Escape') { close(); }
    });
    input.addEventListener('blur', function () { setTimeout(close, 150); });
  };
  complete('compose-to'); complete('compose-cc'); complete('compose-bcc');

  // ---- The organisation's directory ------------------------------------------
  // Outlook's Global Address List and Google's directory: every active account
  // and every enabled distribution list of the reader's OWN domain, with the
  // title, department, telephone numbers and photo the administrator or the
  // person set. The page never names a domain - the server answers the domain
  // of the account that signed in and no other - and a person who has opted out
  // is not in what it answers, so there is nothing here to filter out.
  var directoryEntries = null;
  var directoryEnabled = false;
  var directoryEditable = false;
  var directoryOwn = null;
  var directoryPhotoPending = undefined;   // undefined: unchanged. null: remove. string: new base64.

  var directoryName = function (entry) { return entry.name || entry.address; };

  var directoryDetail = function (entry) {
    var parts = [];
    if (entry.title) { parts.push(entry.title); }
    if (entry.department) { parts.push(entry.department); }
    return parts.join(' - ');
  };

  var directoryAvatar = function (entry) {
    if (entry.has_photo && entry.photo_url) {
      var img = document.createElement('img');
      img.className = 'av';
      img.setAttribute('alt', '');
      img.width = 44; img.height = 44;
      imageFrom(img, entry.photo_url);
      return img;
    }
    var box = document.createElement('span');
    box.className = 'av';
    box.setAttribute('aria-hidden', 'true');
    box.textContent = (directoryName(entry).trim().charAt(0) || '?').toUpperCase();
    return box;
  };

  var renderDirectory = function (entries) {
    var list = el('directory-list');
    clear(list);
    el('directory-off').hidden = directoryEnabled;
    el('directory-empty').hidden = !directoryEnabled || entries.length > 0;
    el('directory-count').textContent = entries.length ? tf('{0} listed', entries.length) : '';

    entries.forEach(function (entry) {
      var card = document.createElement('div');
      card.className = 'dircard';
      card.appendChild(directoryAvatar(entry));

      var who = document.createElement('div');
      who.className = 'who';
      var name = document.createElement('b');
      name.textContent = directoryName(entry);
      who.appendChild(name);

      var address = document.createElement('span');
      address.textContent = entry.address;
      who.appendChild(address);

      var detail = directoryDetail(entry);
      if (detail) { var d = document.createElement('span'); d.textContent = detail; who.appendChild(d); }
      if (entry.kind === 'list') { var k = document.createElement('span'); k.textContent = t('Distribution list'); who.appendChild(k); }
      if (entry.phone) { var p = document.createElement('span'); p.textContent = entry.phone; who.appendChild(p); }
      if (entry.mobile) { var m = document.createElement('span'); m.textContent = entry.mobile; who.appendChild(m); }
      if (entry.office) { var o = document.createElement('span'); o.textContent = entry.office; who.appendChild(o); }

      var acts = document.createElement('div');
      acts.className = 'acts';

      var write = document.createElement('button');
      write.type = 'button';
      write.className = 'btn quiet sm';
      write.textContent = t('Write');
      write.addEventListener('click', function () {
        go('/compose?to=' + encodeURIComponent(entry.name ? entry.name + ' <' + entry.address + '>' : entry.address));
      });
      acts.appendChild(write);

      // A list has no person behind it, so there is nothing to keep as a contact.
      if (entry.kind !== 'list') {
        var keep = document.createElement('button');
        keep.type = 'button';
        keep.className = 'btn quiet sm';
        keep.textContent = t('Add to contacts');
        keep.addEventListener('click', function () {
          say('directory-status', '', true);
          call('POST', '/api/v1/me/contacts', { name: entry.name || '', address: entry.address }).then(function (result) {
            if (result.status === 201) { contactsCache = null; say('directory-status', tf('{0} is in your contacts.', directoryName(entry)), true); return; }
            if (result.status === 409) { say('directory-status', t('That address is already one of your contacts.'), false); return; }
            say('directory-status', describe(result, t('Could not add the contact')), false);
          });
        });
        acts.appendChild(keep);
      }

      who.appendChild(acts);
      card.appendChild(who);
      list.appendChild(card);
    });
  };

  var directoryFilter = function () {
    if (!directoryEntries) { return; }
    var q = (el('directory-search').value || '').trim().toLowerCase();
    if (!q) { renderDirectory(directoryEntries); return; }
    renderDirectory(directoryEntries.filter(function (entry) {
      return [entry.name, entry.address, entry.title, entry.department].some(function (field) {
        return String(field || '').toLowerCase().indexOf(q) >= 0;
      });
    }));
  };

  var loadDirectory = function () {
    return call('GET', '/api/v1/me/directory?limit=1000').then(function (result) {
      if (result.status !== 200 || !result.data) { say('directory-status', describe(result, t('Could not read the directory')), false); return []; }
      directoryEnabled = !!result.data.enabled;
      directoryEditable = !!result.data.editable;
      directoryEntries = result.data.entries || [];
      directoryFilter();
      return directoryEntries;
    });
  };

  // The completion asks the server rather than this cache, so that a page left
  // open overnight offers the person who joined this morning.
  var directorySuggest = function (q) {
    if (!directoryEnabled) { return Promise.resolve([]); }
    return call('GET', '/api/v1/me/directory?limit=8&q=' + encodeURIComponent(q)).then(function (result) {
      if (result.status !== 200 || !result.data) { return []; }
      directoryEnabled = !!result.data.enabled;
      return result.data.entries || [];
    });
  };

  // The card under Settings: the person's own entry, and the opt-out, which is
  // theirs whether or not the server lets them set the rest.
  var directoryFields = ['title', 'department', 'phone', 'mobile', 'office'];

  var showDirectoryEntry = function () {
    var entry = directoryOwn || {};
    directoryFields.forEach(function (field) { el('directory-' + field).value = entry[field] || ''; });
    el('directory-optout').checked = !!entry.opt_out;
    el('directory-form').hidden = !directoryEditable;
    el('directory-locked').hidden = directoryEditable;
    var photo = el('directory-photo');
    var remove = el('directory-photo-remove');
    var has = directoryPhotoPending !== undefined ? directoryPhotoPending !== null : !!entry.has_photo;
    if (directoryPhotoPending) {
      photo.src = 'data:' + (entry.photo_type || 'image/jpeg') + ';base64,' + directoryPhotoPending;
    } else if (has) {
      imageFrom(photo, '/api/v1/me/directory/me/photo?v=' + encodeURIComponent(entry.updated || ''));
    }
    photo.hidden = !has;
    remove.hidden = !has;
  };

  var loadDirectoryCard = function () {
    return call('GET', '/api/v1/me/directory/me').then(function (result) {
      if (result.status !== 200 || !result.data) { el('directory-card').hidden = true; return; }
      directoryEnabled = !!result.data.enabled;
      directoryEditable = !!result.data.editable;
      directoryOwn = result.data.entry || {};
      directoryPhotoPending = undefined;
      el('directory-card').hidden = !directoryEnabled;
      el('nav-directory').hidden = !directoryEnabled;
      showDirectoryEntry();
    });
  };

  var saveDirectoryEntry = function (body, statusId, done) {
    say(statusId, '', true);
    return call('PUT', '/api/v1/me/directory/me', body).then(function (result) {
      if (result.status === 200 && result.data) {
        directoryOwn = result.data.entry || {};
        directoryEditable = !!result.data.editable;
        directoryPhotoPending = undefined;
        directoryEntries = null;
        showDirectoryEntry();
        say(statusId, done, true);
        return true;
      }
      say(statusId, describe(result, t('Could not save your directory entry')), false);
      // The server is the truth about what was stored; a refused save must not
      // leave the form showing what it refused.
      loadDirectoryCard();
      return false;
    });
  };

  el('directory-search').addEventListener('input', directoryFilter);

  el('directory-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var body = {};
    directoryFields.forEach(function (field) { body[field] = el('directory-' + field).value; });
    if (directoryPhotoPending !== undefined) { body.photo = directoryPhotoPending; }
    saveDirectoryEntry(body, 'directory-entry-status', t('Saved.'));
  });

  el('directory-optout').addEventListener('change', function () {
    saveDirectoryEntry({ opt_out: el('directory-optout').checked }, 'directory-optout-status',
      el('directory-optout').checked ? t('You are out of the directory.') : t('You are in the directory.'));
  });

  el('directory-photo-remove').addEventListener('click', function () {
    directoryPhotoPending = null;
    showDirectoryEntry();
    say('directory-entry-status', t('The photo goes when you save.'), true);
  });

  el('directory-photo-file').addEventListener('change', function () {
    var file = (el('directory-photo-file').files || [])[0];
    el('directory-photo-file').value = '';
    if (!file) { return; }
    if (!/^image\/(png|jpeg|gif)$/i.test(file.type || '')) { say('directory-entry-status', t('A directory photo is a JPEG, a PNG or a GIF.'), false); return; }
    if (file.size > 100 * 1024) { say('directory-entry-status', t('A directory photo is at most 100 KB.'), false); return; }
    var reader = new FileReader();
    reader.onload = function () {
      var url = String(reader.result || '');
      var at = url.indexOf(';base64,');
      if (at < 0) { say('directory-entry-status', t('A directory photo is a JPEG, a PNG or a GIF.'), false); return; }
      directoryPhotoPending = url.slice(at + 8);
      directoryOwn = directoryOwn || {};
      directoryOwn.photo_type = url.slice(5, at);
      showDirectoryEntry();
      say('directory-entry-status', t('The photo goes up when you save.'), true);
    };
    reader.onerror = function () { say('directory-entry-status', t('That file could not be read.'), false); };
    reader.readAsDataURL(file);
  });
  // ---- @mentions in the text -------------------------------------------------
  // An @ followed by letters, at the caret, asks the address book as the To
  // field does; the person chosen is written as @Name where the @ was and
  // added to To if not there. Outlook's shape, in the plain text editor.
  var mentionComplete = function () {
    var input = el('compose-text');
    var list = document.createElement('ul'); list.className = 'complete'; list.hidden = true; list.setAttribute('role', 'listbox');
    list.setAttribute('style', 'top:auto;bottom:100%');
    input.parentNode.appendChild(list);
    var items = []; var marked = -1; var timer = null;
    var close = function () { list.hidden = true; list.textContent = ''; items = []; marked = -1; };
    var mention = function () {
      var caret = typeof input.selectionStart === 'number' ? input.selectionStart : input.value.length;
      var head = input.value.slice(0, caret);
      var m = head.match(/(^|\s)@([^\s@]{1,40})$/);
      return m ? { start: caret - m[2].length - 1, q: m[2], caret: caret } : null;
    };
    var take = function (c) {
      var at = mention();
      if (!at) { close(); return; }
      var name = c.name || c.address.split('@')[0];
      input.value = input.value.slice(0, at.start) + '@' + name + ' ' + input.value.slice(at.caret);
      var to = el('compose-to').value;
      if (to.toLowerCase().indexOf(c.address.toLowerCase()) < 0) {
        el('compose-to').value = (to.trim() ? to.replace(/,?\s*$/, ', ') : '') + (c.name ? c.name + ' <' + c.address + '>' : c.address) + ', ';
      }
      close();
      input.focus();
    };
    var mark = function (n) { marked = n; for (var i = 0; i < list.children.length; i++) { list.children[i].classList.toggle('on', i === marked); } };
    var show = function (found) {
      close();
      if (!found.length) { return; }
      items = found;
      found.forEach(function (c) {
        var li = document.createElement('li'); li.setAttribute('role', 'option');
        var nm = document.createElement('span'); nm.className = 'nm'; nm.textContent = '@' + (c.name || c.address);
        var ad = document.createElement('span'); ad.className = 'ad'; ad.textContent = c.name ? c.address : '';
        li.appendChild(nm); li.appendChild(ad);
        li.addEventListener('mousedown', function (e) { e.preventDefault(); take(c); });
        list.appendChild(li);
      });
      list.hidden = false; mark(0);
    };
    input.addEventListener('input', function () {
      var at = mention();
      if (timer) { clearTimeout(timer); }
      if (!at) { close(); return; }
      timer = setTimeout(function () {
        call('GET', '/api/v1/me/contacts?limit=8&q=' + encodeURIComponent(at.q)).then(function (result) {
          var now = mention();
          if (result.status !== 200 || !result.data || !now || now.q !== at.q) { return; }
          // A mention is one person; a group has no address to mention.
          show((result.data.contacts || []).filter(function (c) { return c.kind !== 'group'; }));
        });
      }, 120);
    });
    input.addEventListener('keydown', function (e) {
      if (list.hidden) { return; }
      if (e.key === 'ArrowDown') { e.preventDefault(); mark((marked + 1) % items.length); }
      else if (e.key === 'ArrowUp') { e.preventDefault(); mark((marked - 1 + items.length) % items.length); }
      else if (e.key === 'Enter' || e.key === 'Tab') { if (marked >= 0) { e.preventDefault(); take(items[marked]); } }
      else if (e.key === 'Escape') { close(); }
    });
    input.addEventListener('blur', function () { setTimeout(close, 150); });
  };
  mentionComplete();
  // A message that mentions the reader by name, or by the mailbox's name,
  // in what the listing shows of it, carries an @ mark in the list.
  var readerNames = function () {
    var names = [];
    var first = el('name-first') ? String(el('name-first').value || '').trim() : '';
    if (first) { names.push(first); }
    if (me && me.indexOf('@') > 0) { names.push(me.split('@')[0]); }
    return names;
  };
  var mentionsReader = function (m) {
    var text = (String(m.subject || '') + ' ' + String(m.snippet || '')).toLowerCase();
    return readerNames().some(function (n) { return n && text.indexOf('@' + n.toLowerCase()) >= 0; });
  };

  // The request itself, once the undo delay has passed (or at once when
  // there is none). The form is emptied only when it still holds the
  // message that was sent: during the delay the reader may have gone on to
  // write another.
  var submitMessage = function (queued) {
    // A signed or an encrypted message is built here first (smimePrepare);
    // a plain one goes as it is.
    return smimePrepare(queued.body).then(pgpPrepare).then(pgpStamp).then(function (prepared) {
      queued.body = prepared;
      return call('POST', '/api/v1/me/messages', prepared);
    }, function (why) {
      say('compose-status', String(why && why.message ? why.message : why), false);
      el('compose-send').disabled = false;
      return { status: -1 };
    }).then(function (result) {
      if (result.status === -1) { return; }
      if (result.status === 0) {
        // No connection: the message waits in this browser and goes when
        // the connection is back. The form is left as it is. The outbox is
        // the first account's; a second account's message stays in the form,
        // and the form says so.
        if (outboxAdd(queued.body)) { say('compose-status', t('No connection. The message waits here and will be sent when the connection is back.'), false); }
        else { say('compose-status', t('No connection. The message is still here; send it again when the connection is back.'), false); }
        return;
      }
      if (result.status === 201) {
        if (composeKey === queued.key) {
          blankCompose();
          composeKey = newComposeKey();
          closeCompose();
        }
        toast(t('Sent.'));
        learnSent(queued);
        if (queued.draft) { call('DELETE', '/api/v1/me/messages/' + queued.draft + '?permanent=1').then(function () { loadFolders(); }); return; }
        loadFolders();
        return;
      }
      say('compose-status', describe(result, t('Could not send')), false);
    });
  };
  // Undo send: for the delay the account chose, the message waits here, in
  // the page, and Undo keeps it in the form untouched. Nothing has reached
  // the server until the bar runs out.
  var pendingSend = null;
  var cancelUndo = function () {
    if (!pendingSend) { return; }
    clearInterval(pendingSend.ticker); clearTimeout(pendingSend.timer);
    pendingSend = null;
    el('undo-bar').hidden = true;
    el('compose-send').disabled = false;
    if (el('toasts') && el('toasts').textContent.indexOf(t('Sending in {0} s').split('{0}')[0]) >= 0) { dismissToast(); }
  };
  el('compose-form').addEventListener('submit', function (event) {
    event.preventDefault();
    if (pendingSend) { return; }
    say('compose-status', '', true);
    var body = composeBody();
    // The message promises an attachment and carries none, not even as a
    // link: one question. Cancel leaves the message here, as written.
    if (mentionsAttachment(body.text) && !anythingAttached() && !window.confirm(t('Your message mentions an attachment, but nothing is attached. Send it anyway?'))) { return; }
    readFiles().then(function (files) {
      files = carried.concat(files);
      if (files.length) { body.attachments = files; }
      appendLinks(body);
      var queued = { body: body, key: composeKey, draft: draftId, answered: replyTo ? replyTo.from : '' };
      var delay = undoSeconds();
      if (!delay) { return submitMessage(queued); }
      var left = delay;
      el('undo-text').textContent = tf('Sending in {0} s', left);
      el('compose-send').disabled = true;
      pendingSend = queued;
      pendingSend.mode = composeMode; pendingSend.id = composeId;
      hideCompose();
      var countdown = toast(tf('Sending in {0} s', left), function () { el('undo-send').click(); }, true);
      pendingSend.ticker = setInterval(function () { left--; var text = tf('Sending in {0} s', Math.max(left, 0)); el('undo-text').textContent = text; if (countdown && countdown.firstChild) { countdown.firstChild.textContent = text; } }, 1000);
      pendingSend.timer = setTimeout(function () { var q = pendingSend; cancelUndo(); if (q) { submitMessage(q); } }, delay * 1000);
      return null;
    }, function (why) { say('compose-status', why, false); return null; });
  });
  el('undo-send').addEventListener('click', function () { var q = pendingSend; cancelUndo(); dismissToast(); if (q) { openCompose(q.mode || 'new', q.id || 0); } say('compose-status', t('Not sent. The message is still here.'), true); });
  // The send that is waiting goes now: before the form is cleared or
  // written to, and before the account signs out.
  var flushPending = function () {
    if (!pendingSend) { return Promise.resolve(); }
    var q = pendingSend;
    cancelUndo();
    return submitMessage(q);
  };
  // The page closing with a send waiting: the message goes in a request
  // the browser keeps alive after the page, when it fits in one (64 KB);
  // a larger one - attachments - is worth a warning instead. So is one that
  // is to be signed or encrypted: that is done in smimePrepare on the way
  // through submitMessage, and it cannot be done here, because beforeunload
  // does not wait for the cryptography - so the body sent from here would
  // be the plaintext, with an encrypt flag the server does not act on. The
  // one right thing is not to send it, and to warn.
  window.addEventListener('beforeunload', function (event) {
    if (!pendingSend) { return; }
    var body = JSON.stringify(pendingSend.body);
    if (body.length < 60000 && !pendingSend.body.sign && !pendingSend.body.encrypt && !pendingSend.body.pgp_sign && !pendingSend.body.pgp_encrypt) {
      cancelUndo();
      apiFetch('/api/v1/me/messages', { method: 'POST', keepalive: true, credentials: 'same-origin', headers: { 'Content-Type': 'application/json', 'X-Requested-With': 'hMailServer' }, body: body });
      return;
    }
    event.preventDefault();
    event.returnValue = '';
  });
  // The draft route stores what it is given, and it is given the plaintext:
  // signing and encryption happen on the way through submitMessage, not
  // here. A draft that was to be sealed is therefore refused rather than
  // stored in the clear with a flag the server ignores - and a scheduled
  // send, which is a draft with an hour, would otherwise go in the clear at
  // that hour. True when the body was refused, having said why.
  var smimeSealedDraft = function (body) {
    if (!body.sign && !body.encrypt && !body.pgp_sign && !body.pgp_encrypt) { return false; }
    say('compose-status', t('A signed or encrypted message cannot yet be saved as a draft or scheduled: untick Sign and Encrypt, or send it now.'), false);
    return true;
  };
  // The draft, saved: what the form holds and the files with it, over the
  // draft it came from. Resolves to the draft's id, or 0 when it could not
  // be saved, having said why.
  var saveDraft = function () {
    var body = composeBody();
    if (smimeSealedDraft(body)) { return Promise.resolve(0); }
    if (draftId) { body.replace_id = draftId; }
    return readFiles().then(function (files) {
      files = carried.concat(files);
      if (files.length) { body.attachments = files; }
      appendLinks(body);
      return call('POST', '/api/v1/me/drafts', body);
    }, function (why) { say('compose-status', why, false); return null; }).then(function (result) {
      if (!result) { return 0; }
      if (result.status === 201 && result.data) { draftId = result.data.id; composeKey = 'draft:' + draftId; say('compose-status', t('Draft saved.'), true); loadFolders(); return draftId; }
      say('compose-status', describe(result, t('Could not save the draft')), false);
      return 0;
    });
  };
  el('compose-save').addEventListener('click', function () { saveDraft(); });
  // Files dropped anywhere on the form join the ones picked.
  el('compose-form').addEventListener('dragover', function (event) { event.preventDefault(); el('compose-form').classList.add('dropping'); });
  el('compose-form').addEventListener('dragleave', function () { el('compose-form').classList.remove('dropping'); });
  el('compose-form').addEventListener('drop', function (event) {
    el('compose-form').classList.remove('dropping');
    var files = Array.prototype.slice.call((event.dataTransfer && event.dataTransfer.files) || []);
    // Text dropped into a field is the field's; only files are taken here.
    if (!files.length) { return; }
    event.preventDefault();
    files.forEach(function (f) { dropped.push(f); });
    if (files.length) { el('compose-files-note').textContent = 'Dropped: ' + dropped.map(function (f) { return f.name + ' (' + format(f.size) + ')'; }).join(', '); }
  });
  el('compose-files').addEventListener('change', function () {
    var files = Array.prototype.slice.call(el('compose-files').files || []);
    el('compose-files-note').textContent = files.length ? files.map(function (f) { return f.name + ' (' + format(f.size) + ')'; }).join(', ') : '';
  });
  // ---- Search history: the last ten searches, kept in this browser --------
  // Offered under the box while it is focused and empty; an entry runs that
  // search again, Clear the list empties it. Nothing of it reaches the
  // server, and it goes with the account at sign-out.
  // The list is the first account's, as everything this browser keeps is:
  // while the second account is shown, nothing is offered and nothing is
  // remembered, so that what was looked for in one mailbox is neither
  // written under the other nor read back to it.
  var SEARCHES = 'hmPortalSearches';
  var searchesRead = function () {
    if (!offlineMine()) { return []; }
    try {
      var list = JSON.parse(localStorage.getItem(SEARCHES) || '[]');
      return Array.isArray(list) ? list.filter(function (s) { return typeof s === 'string' && s; }).slice(0, 10) : [];
    } catch (e) { return []; }
  };
  var searchesWrite = function (list) {
    if (!offlineMine()) { return; }
    try { if (list.length) { localStorage.setItem(SEARCHES, JSON.stringify(list)); } else { localStorage.removeItem(SEARCHES); } } catch (e) { /* a browser that keeps nothing */ }
  };
  var rememberSearch = function (text) {
    if (!offlineMine()) { return; }
    var list = searchesRead().filter(function (s) { return s !== text; });
    list.unshift(text);
    searchesWrite(list.slice(0, 10));
  };
  var hideSearchHistory = function () { el('search-history').hidden = true; };
  var historyButtons = function () { return el('search-history-list').children; };
  // The arrow keys through one of the two lists under the box - the history
  // or the suggestions - and Escape back to the box.
  var listKeys = function (buttons, hide) {
    return function (e) {
      var items = buttons(); var at = -1;
      for (var i = 0; i < items.length; i++) { if (items[i] === e.target) { at = i; } }
      if (e.key === 'ArrowDown') { e.preventDefault(); if (at + 1 < items.length) { items[at + 1].focus(); } }
      else if (e.key === 'ArrowUp') { e.preventDefault(); if (at > 0) { items[at - 1].focus(); } else { el('mail-search').focus(); } }
      else if (e.key === 'Escape') { e.preventDefault(); hide(); el('mail-search').focus(); }
    };
  };
  var historyKeys = listKeys(historyButtons, hideSearchHistory);
  // Focus leaving the box or an entry closes the lists, unless it went to
  // the other of the two: a moment later, so a click on an entry lands first.
  var leaveHistory = function () {
    setTimeout(function () {
      var focus = document.activeElement;
      if (focus === el('mail-search') || within(focus, el('search-history')) || within(focus, el('search-suggest'))) { return; }
      hideSearchHistory();
      hideSuggestions();
    }, 150);
  };
  // ---- Search suggestions: a contact's name completing to from:<address> --
  // The token being typed - the text after the last space, with the from:
  // or to: it may already carry - is asked of the address book the way the
  // To field asks it, and the contacts it matches are offered under the box
  // in place of the history. One taken stands in the box as from:<address>
  // (to: when the token began with to:) in place of the name, beside
  // whatever else was typed, and the search runs. An operator's value
  // (is:unread, subject:x) and a quoted phrase are not names, and one
  // letter asks nothing.
  var suggestTimer = null;
  var suggestToken = function () {
    var v = el('mail-search').value;
    var cut = Math.max(v.lastIndexOf(' '), v.lastIndexOf('\t')) + 1;
    var tail = v.slice(cut);
    var m = /^(from|to):(.*)$/i.exec(tail);
    return { head: v.slice(0, cut), key: m ? m[1].toLowerCase() : 'from', q: m ? m[2] : tail };
  };
  var suggestButtons = function () { return el('search-suggest-list').children; };
  var hideSuggestions = function () {
    el('search-suggest').hidden = true;
    if (suggestTimer) { clearTimeout(suggestTimer); suggestTimer = null; }
  };
  var suggestKeys = listKeys(suggestButtons, hideSuggestions);
  var showSuggestions = function (found, token) {
    var list = el('search-suggest-list');
    clear(list);
    if (!found.length) { hideSuggestions(); return; }
    el('search-suggest-head').textContent = token.key === 'to' ? t('Search by recipient') : t('Search by sender');
    found.forEach(function (c) {
      var b = node('button', undefined, 'q'); b.type = 'button';
      b.appendChild(icon('person', true));
      b.appendChild(node('span', c.name || c.address, 'nm'));
      b.appendChild(node('span', token.key + ':' + c.address, 'ad'));
      b.addEventListener('mousedown', function (e) { e.preventDefault(); });
      b.addEventListener('click', function () {
        var text = token.head + token.key + ':' + c.address;
        el('mail-search').value = text;
        hideSuggestions();
        runSearch(text);
      });
      b.addEventListener('keydown', suggestKeys);
      b.addEventListener('blur', leaveHistory);
      list.appendChild(b);
    });
    el('search-suggest').hidden = false;
  };
  var suggestSearch = function () {
    var token = suggestToken();
    if (suggestTimer) { clearTimeout(suggestTimer); suggestTimer = null; }
    if (token.q.length < 2 || /[:"]/.test(token.q)) { hideSuggestions(); return; }
    suggestTimer = setTimeout(function () {
      suggestTimer = null;
      call('GET', '/api/v1/me/contacts?limit=8&q=' + encodeURIComponent(token.q)).then(function (result) {
        // The answer to what is still being typed, and no other.
        var now = suggestToken();
        if (result.status !== 200 || !result.data || now.q !== token.q || now.key !== token.key) { return; }
        // from: names one sender; a group has no address to search by.
        showSuggestions((result.data.contacts || []).filter(function (c) { return c.kind !== 'group'; }), token);
      });
    }, 120);
  };
  var showSearchHistory = function () {
    var list = el('search-history-list');
    clear(list);
    var items = searchesRead();
    if (!items.length || el('mail-search').value) { hideSearchHistory(); return; }
    items.forEach(function (q) {
      var b = node('button', undefined, 'q'); b.type = 'button';
      b.appendChild(icon('clock', true));
      b.appendChild(node('span', q));
      b.addEventListener('mousedown', function (e) { e.preventDefault(); });
      b.addEventListener('click', function () { el('mail-search').value = q; hideSearchHistory(); runSearch(q); });
      b.addEventListener('keydown', historyKeys);
      b.addEventListener('blur', leaveHistory);
      list.appendChild(b);
    });
    el('search-history').hidden = false;
  };
  // The one way a search is run, from the box, its options panel or the
  // history: an empty search is the folder itself, and a real one is kept.
  var runSearch = function (text) {
    text = String(text || '').trim();
    state.before = 0;
    lastListing = null;
    if (!text) { go('/f/' + (state.folderId || inboxId)); return; }
    rememberSearch(text);
    if (el('mail-search-everywhere').checked) { go('/search?q=' + encodeURIComponent(text)); return; }
    go('/f/' + (state.folderId || inboxId) + '?q=' + encodeURIComponent(text));
  };
  el('mail-search').addEventListener('focus', showSearchHistory);
  el('mail-search').addEventListener('blur', leaveHistory);
  el('mail-search').addEventListener('input', function () {
    if (el('mail-search').value) { hideSearchHistory(); suggestSearch(); } else { hideSuggestions(); showSearchHistory(); }
  });
  el('mail-search').addEventListener('keydown', function (e) {
    if (e.key === 'Escape') { hideSearchHistory(); hideSuggestions(); return; }
    if (e.key === 'ArrowDown') {
      var items = !el('search-history').hidden ? historyButtons() : !el('search-suggest').hidden ? suggestButtons() : [];
      if (items.length) { e.preventDefault(); items[0].focus(); }
    }
  });
  el('search-history-clear').addEventListener('mousedown', function (e) { e.preventDefault(); });
  el('search-history-clear').addEventListener('click', function () { searchesWrite([]); hideSearchHistory(); el('mail-search').focus(); });
  el('mail-search-form').addEventListener('submit', function (event) {
    event.preventDefault();
    hideSearchHistory();
    hideSuggestions();
    runSearch(el('mail-search').value);
  });
  el('mail-search-clear').addEventListener('click', function () {
    el('mail-search').value = '';
    el('mail-search-everywhere').checked = false;
    state.before = 0;
    lastListing = null;
    go('/f/' + (state.folderId || inboxId));
  });
  el('folder-new-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var name = el('folder-new-name').value.trim();
    var parent = el('folder-new-parent').value;
    var inside = parent && foldersById[Number(parent)] ? foldersById[Number(parent)].path : '';
    // One field, and it is a path: the server makes every part of it that is
    // not there yet, the way IMAP CREATE does.
    var body = { name: inside ? inside + delimiter + name : name };
    call('POST', '/api/v1/me/folders', body).then(function (result) {
      if (result.status === 201 || result.status === 200) {
        el('folder-new-name').value = '';
        say('folder-admin-status', t('Created.'), true);
        loadFolders().then(renderFolderAdmin);
        return;
      }
      say('folder-admin-status', describe(result, t('Could not create the folder')), false);
    });
  });
  el('vacation-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var body = {
      enabled: el('vacation-enabled').checked,
      subject: el('vacation-subject').value,
      message: el('vacation-message').value,
      expires: el('vacation-expires').checked,
      expires_date: el('vacation-expires-date').value
    };
    call('PUT', '/api/v1/me/vacation', body).then(function (result) {
      say('vacation-status', result.status === 200 ? t('Saved.') : describe(result, t('Could not save')), result.status === 200);
    });
  });
  // ---- The formatted signature ---------------------------------------------
  // Beside the plain signature, one with layout: bold, italic, underline, a
  // link, an image from a file kept as a data URI (the cleaner keeps those and
  // nothing remote). Saved as signature.html, which the server has stored all
  // along; put on formatted messages, the plain one on plain messages. The
  // server sends each image as a cid: part beside the HTML, which is what the
  // recipient's client shows (InlineImageParts.cpp).
  //
  // 64 KB, images included, counted as the server counts it - bytes of UTF-8 -
  // and refused by it above that (RestApiServer.cpp, MaxSignatureHtmlBytes):
  // MySQL's column holds 65,535 bytes, and a signature goes on every message,
  // so what it can carry is a logo rather than a photograph. An image is drawn
  // at most 600 by 200 pixels when it has to be made smaller - twice what a
  // signature shows a logo at, so it stays sharp on a high-density screen.
  var SIGNATURE_HTML_BYTES = 64000;
  var SIGNATURE_IMG_TAG_BYTES = '<img src="" alt="">'.length;
  var SIGNATURE_IMAGE_BOX = { width: 600, height: 200 };
  var utf8Bytes = function (s) {
    var n = 0;
    for (var i = 0; i < s.length; i++) {
      var c = s.charCodeAt(i);
      if (c < 0x80) { n += 1; } else if (c < 0x800) { n += 2; } else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.length) { n += 4; i++; } else { n += 3; }
    }
    return n;
  };
  // What the file's first bytes say it is, which is what the server checks
  // before it sends an image as a part: a PNG saved with a .jpg name is sent
  // as the PNG it is, and a file that is none of the four is refused here.
  var sniffImageType = function (url) {
    var m = /^data:[^,]*;base64,([A-Za-z0-9+\/]{16})/.exec(url || '');
    var head = '';
    try { head = m ? atob(m[1]) : ''; } catch (e) { head = ''; }
    if (head.slice(0, 8) === '\x89PNG\r\n\x1a\n') { return 'image/png'; }
    if (head.slice(0, 3) === '\xff\xd8\xff') { return 'image/jpeg'; }
    if (head.slice(0, 6) === 'GIF87a' || head.slice(0, 6) === 'GIF89a') { return 'image/gif'; }
    if (head.slice(0, 4) === 'RIFF' && head.slice(8, 12) === 'WEBP') { return 'image/webp'; }
    return '';
  };
  // A smaller copy of the image as a data: URL no longer than room, or null:
  // no canvas in this browser, an image it cannot read, or one that does not
  // fit even at a size that is no longer a logo. A JPEG stays a JPEG; anything
  // else becomes a PNG, which keeps transparency (and loses a GIF's animation).
  var shrinkSignatureImage = function (url, type, room) {
    var canvas = url ? document.createElement('canvas') : null;
    if (!canvas || typeof canvas.getContext !== 'function' || typeof Image !== 'function' || room <= 0) { return Promise.resolve(null); }
    return new Promise(function (resolve) {
      var img = new Image();
      img.onerror = function () { resolve(null); };
      img.onload = function () {
        var w = img.naturalWidth || img.width, h = img.naturalHeight || img.height;
        var out = type === 'image/jpeg' ? 'image/jpeg' : 'image/png';
        var scale = w && h ? Math.min(1, SIGNATURE_IMAGE_BOX.width / w, SIGNATURE_IMAGE_BOX.height / h) : 0;
        for (var attempt = 0; scale > 0 && attempt < 8; attempt++) {
          var cw = Math.max(1, Math.round(w * scale)), ch = Math.max(1, Math.round(h * scale));
          canvas.width = cw; canvas.height = ch;
          var context = canvas.getContext('2d');
          if (!context) { break; }
          context.clearRect(0, 0, cw, ch);
          context.drawImage(img, 0, 0, cw, ch);
          var smaller = String(canvas.toDataURL(out, 0.85) || '');
          if (smaller.indexOf('data:' + out + ';base64,') === 0 && smaller.length <= room) { resolve(smaller); return; }
          if (cw <= 48 || ch <= 16) { break; }
          scale *= 0.75;
        }
        resolve(null);
      };
      img.src = url;
    });
  };
  (function () {
    var editor = el('signature-html');
    var tools = el('signature-tools').children;
    for (var i = 0; i < tools.length; i++) {
      (function (b) {
        if (!b.getAttribute('data-cmd')) { return; }
        b.addEventListener('mousedown', function (event) { event.preventDefault(); });
        b.addEventListener('click', function () {
          var cmd = b.getAttribute('data-cmd');
          var arg = null;
          if (cmd === 'createLink') { arg = window.prompt ? window.prompt(t('Link address')) : ''; if (!arg) { return; } }
          try { document.execCommand(cmd, false, arg); } catch (e) { /* a browser without the command keeps the text */ }
        });
      })(tools[i]);
    }
    // An image goes into the signature as a data: URL, so it counts against
    // the signature's 64 KB; one that does not fit in what is left is drawn
    // smaller, when the browser can, and refused when even that does not fit.
    el('signature-image').addEventListener('change', function () {
      var file = (el('signature-image').files || [])[0];
      el('signature-image').value = '';
      if (!file) { return; }
      if (!/^image\/(png|jpeg|gif|webp)$/i.test(file.type || '')) { say('settings-status', t('An image for the signature: PNG, JPEG, GIF or WebP.'), false); return; }
      var room = SIGNATURE_HTML_BYTES - utf8Bytes(cleanHtml(editor)) - SIGNATURE_IMG_TAG_BYTES;
      new Promise(function (resolve) {
        var reader = new FileReader();
        reader.onload = function () { resolve(String(reader.result || '')); };
        reader.onerror = function () { resolve(''); };
        reader.readAsDataURL(file);
      }).then(function (url) {
        var type = sniffImageType(url);
        if (!type) { return { refused: true }; }
        url = 'data:' + type + url.slice(url.indexOf(';base64,'));
        if (url.length <= room) { return { url: url, smaller: false }; }
        return shrinkSignatureImage(url, type, room).then(function (smaller) { return { url: smaller, smaller: true }; });
      }).then(function (result) {
        if (result.refused) { say('settings-status', t('An image for the signature: PNG, JPEG, GIF or WebP.'), false); return; }
        if (!result.url) {
          say('settings-status', tf('That image does not fit: the formatted signature holds 64 KB, images included, and {0} KB of it is free.', Math.max(0, Math.floor(room / 1000))), false);
          return;
        }
        var img = document.createElement('img');
        img.setAttribute('src', result.url);
        img.setAttribute('alt', '');
        editor.appendChild(img);
        say('settings-status', result.smaller ? t('The image was made smaller to fit the signature.') : '', true);
      });
    });
  })();
  el('settings-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var signatureHtml = cleanHtml(el('signature-html'));
    if (utf8Bytes(signatureHtml) > SIGNATURE_HTML_BYTES) { say('settings-status', t('The formatted signature is over 64 KB with its images; take an image or some text out.'), false); return; }
    var body = {
      name: { first: el('name-first').value, last: el('name-last').value },
      signature: { enabled: el('signature-enabled').checked, text: el('signature-text').value, html: signatureHtml }
    };
    call('PUT', '/api/v1/me/settings', body).then(function (result) {
      if (result.status === 200 && result.data) { renderSettings(result.data); say('settings-status', t('Saved.'), true); return; }
      say('settings-status', describe(result, t('Could not save')), false);
    });
  });
  el('forward-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var body = { forwarding: { enabled: el('forward-enabled').checked, address: el('forward-address').value, keep_original: el('forward-keep').checked } };
    call('PUT', '/api/v1/me/settings', body).then(function (result) {
      if (result.status === 200 && result.data) { renderSettings(result.data); say('forward-status', t('Saved.'), true); return; }
      say('forward-status', describe(result, t('Could not save')), false);
    });
  });

  // ---- Storage ----------------------------------------------------------------
  var loadFiles = function () {
    return call('GET', '/api/v1/me/files').then(function (result) {
      if (result.status !== 200 || !result.data) { say('files-status', describe(result, t('Could not read the files')), false); return; }
      if (result.data.policy) { filesPolicy = result.data.policy; }
      var files = result.data.files || [];
      var rows = el('files-rows');
      clear(rows);
      el('files-empty').hidden = files.length > 0;
      el('files-note').textContent = (Number(filesPolicy.link_above_kb) > 0
        ? 'A file above ' + format(Number(filesPolicy.link_above_kb) * 1024) + ' on a message goes here and the message carries a link that lives ' + filesPolicy.days + ' days. '
        : 'Files are attached to messages here, never sent as links. ')
        + format(result.data.used_bytes || 0) + ' of ' + filesPolicy.quota_mb + ' MB used.';
      files.forEach(function (f) {
        var tr = document.createElement('tr');
        var name = document.createElement('td');
        name.appendChild(node('span', f.name));
        if (f.protected) { name.appendChild(node('span', ' ')); name.appendChild(node('span', t('Password'), 'badge info')); }
        if (!f.complete) { name.appendChild(node('span', ' ')); name.appendChild(node('span', t('Unfinished'), 'badge warn')); }
        tr.appendChild(name);
        tr.appendChild(node('td', format(f.size)));
        tr.appendChild(node('td', String(f.downloads || 0)));
        tr.appendChild(node('td', f.expired ? t('Expired') : whenExpires(f.expires)));
        var actions = document.createElement('td');
        var copy = button(t('Copy link'), 'btn ghost sm');
        copy.addEventListener('click', function () {
          var url = location.origin + f.link;
          if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(url).then(function () { say('files-status', t('Link copied: ') + url, true); }, function () { say('files-status', url, true); });
          } else { say('files-status', url, true); }
        });
        var protect = button(f.protected ? t('Change password') : t('Set password'), 'btn ghost sm');
        protect.addEventListener('click', function () {
          var given = window.prompt('A password for ' + f.name + ' (empty removes it):', '');
          if (given === null) { return; }
          call('PUT', '/api/v1/me/files/' + f.id, { password: given }).then(function (r) {
            say('files-status', r.status === 200 ? (given ? t('Password set.') : t('Password removed.')) : describe(r, t('Could not change it')), r.status === 200);
            loadFiles();
          });
        });
        var extend = button(t('Extend'), 'btn ghost sm');
        extend.addEventListener('click', function () {
          call('PUT', '/api/v1/me/files/' + f.id, { days: filesPolicy.days }).then(function (r) {
            say('files-status', r.status === 200 ? t('Kept for another ') + filesPolicy.days + t(' days.') : describe(r, t('Could not extend it')), r.status === 200);
            loadFiles();
          });
        });
        var remove = button(t('Remove'), 'btn ghost sm danger');
        remove.addEventListener('click', function () {
          call('DELETE', '/api/v1/me/files/' + f.id).then(function (r) {
            say('files-status', r.status === 200 ? t('Removed; the link is dead.') : describe(r, t('Could not remove it')), r.status === 200);
            loadFiles();
          });
        });
        actions.appendChild(copy); actions.appendChild(node('span', ' ')); actions.appendChild(protect); actions.appendChild(node('span', ' ')); actions.appendChild(extend); actions.appendChild(node('span', ' ')); actions.appendChild(remove);
        tr.appendChild(actions);
        rows.appendChild(tr);
      });
    });
  };
  var loadStorage = function () {
    el('storage-clean').textContent = t('Delete'); el('storage-clean').removeAttribute('data-armed');
    return call('GET', '/api/v1/me/storage').then(function (result) {
      if (result.status !== 200 || !result.data) { say('storage-status', describe(result, t('Could not read the storage')), false); return; }
      var s = result.data;
      el('storage-quota').textContent = s.limit_mb > 0
        ? format(s.used_bytes) + ' of ' + s.limit_mb + ' MB used (' + Math.min(100, Math.round(100 * s.used_bytes / (s.limit_mb * 1048576))) + '%)'
        : format(s.used_bytes) + ' used, no limit';
      var rows = el('storage-folder-rows');
      clear(rows);
      (s.folders || []).slice().sort(function (a, b) { return b.bytes - a.bytes; }).forEach(function (f) {
        var tr = document.createElement('tr');
        tr.appendChild(node('td', f.path));
        tr.appendChild(node('td', String(f.messages), 'num'));
        tr.appendChild(node('td', format(f.bytes), 'num'));
        tr.addEventListener('click', function () { go('/f/' + f.id); });
        tr.className = 'clickable';
        rows.appendChild(tr);
      });
      var largest = el('storage-largest-rows');
      clear(largest);
      (s.largest || []).forEach(function (m) {
        var tr = document.createElement('tr');
        tr.appendChild(node('td', m.subject || t('(no subject)')));
        tr.appendChild(node('td', m.from || ''));
        tr.appendChild(node('td', m.folder || ''));
        tr.appendChild(node('td', format(m.size), 'num'));
        tr.className = 'clickable';
        tr.addEventListener('click', function () { state.folderId = m.folder_id; go('/m/' + m.id); });
        largest.appendChild(tr);
      });
      el('storage-largest-empty').hidden = (s.largest || []).length > 0;
      el('storage-largest').hidden = (s.largest || []).length === 0;
    });
  };
  // Asked twice, as Empty this folder is: the first press arms the button,
  // the second deletes. A folder that could not be emptied is named, and
  // the count is of what actually went.
  el('storage-clean-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var b = el('storage-clean');
    if (b.getAttribute('data-armed') !== '1') { b.setAttribute('data-armed', '1'); b.textContent = t('Really delete?'); say('storage-status', t('Press again to delete these for good.'), true); return; }
    b.removeAttribute('data-armed'); b.textContent = t('Delete');
    var days = el('storage-days').value;
    var targets = allFolders.filter(function (f) { return folderIs(f.id, 'Junk') || folderIs(f.id, 'Trash'); });
    if (!targets.length) { say('storage-status', t('There is no Junk or Trash folder to clean.'), false); return; }
    var total = 0, failed = [];
    var chain = Promise.resolve();
    targets.forEach(function (f) {
      chain = chain.then(function () {
        return emptyFolder(f.id, days).then(function (r) { total += r.deleted; if (r.failed) { failed.push(folderName(f.id)); } });
      });
    });
    chain.then(function () {
      if (failed.length) { say('storage-status', tf('{0} deleted; {1} could not be emptied.', total, failed.join(', ')), false); }
      else { say('storage-status', total + t(' deleted.'), true); }
      loadFolders(); loadStorage();
    });
  });

  // ---- Security: app passwords and sessions ----------------------------------
  var ago = function (seconds) {
    if (seconds < 60) { return 'just now'; }
    if (seconds < 3600) { return Math.round(seconds / 60) + ' min ago'; }
    if (seconds < 86400) { return Math.round(seconds / 3600) + ' h ago'; }
    return Math.round(seconds / 86400) + ' d ago';
  };
  var loadAppPasswords = function () {
    return call('GET', '/api/v1/me/app-passwords').then(function (result) {
      if (result.status !== 200 || !result.data) { say('apppassword-status', describe(result, t('Could not read the app passwords')), false); return; }
      var rows = el('apppassword-rows');
      clear(rows);
      var list = result.data.app_passwords || [];
      list.forEach(function (a) {
        var tr = document.createElement('tr');
        tr.appendChild(node('td', a.name));
        tr.appendChild(node('td', a.created || ''));
        tr.appendChild(node('td', a.last_used || 'never'));
        var actions = document.createElement('td');
        var remove = button(t('Remove'));
        remove.addEventListener('click', function () {
          call('DELETE', '/api/v1/me/app-passwords/' + a.id).then(function (r) {
            if (r.status === 200) { say('apppassword-status', t('Removed. Anything still using it will stop signing in.'), true); loadAppPasswords(); }
            else { say('apppassword-status', describe(r, t('Could not remove it')), false); }
          });
        });
        actions.appendChild(remove);
        tr.appendChild(actions);
        rows.appendChild(tr);
      });
      el('apppassword-empty').hidden = list.length > 0;
      el('apppassword-table').hidden = list.length === 0;
    });
  };
  // The account's own password goes with the request, and the one-time
  // code when the server asks for it: minting a credential is proven by
  // the person, not by the session.
  el('sender-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var body = {
      value: el('sender-value').value,
      blocked: el('sender-kind').value === 'block',
      reason: el('sender-reason').value
    };
    call('POST', '/api/v1/me/sender-list', body).then(function (result) {
      if (result.status !== 201 && result.status !== 200) { say('sender-status', describe(result, t('Could not add it')), false); return; }
      el('sender-value').value = '';
      el('sender-reason').value = '';
      say('sender-status', result.status === 200 ? t('That address was already on your list; it has been changed.') : t('Added.'), true);
      loadSenderList();
    });
  });
  el('digest-enabled').addEventListener('change', function () {
    var wanted = el('digest-enabled').checked;
    call('PUT', '/api/v1/me/quarantine-digest', { enabled: wanted }).then(function (result) {
      if (result.status !== 200 || !result.data) { say('digest-status', describe(result, t('Could not save that')), false); loadDigestSetting(); return; }
      renderDigestSetting(result.data);
      say('digest-status', wanted ? t('You will get the digest.') : t('You will not get the digest. Held mail is still held, and still listed here.'), true);
    });
  });
  el('apppassword-form').addEventListener('submit', function (event) {
    event.preventDefault();
    forgetSecret();
    var body = { name: el('apppassword-name').value, password: el('apppassword-password').value };
    call('POST', '/api/v1/me/app-passwords', body, secondFactorHeaders('apppassword-otp', 'apppassword-code')).then(function (result) {
      if (result.status === 201 && result.data) {
        el('apppassword-secret').textContent = result.data.password || '';
        el('apppassword-new').hidden = false;
        el('apppassword-name').value = ''; el('apppassword-password').value = ''; el('apppassword-otp').value = ''; el('apppassword-code').value = '';
        say('apppassword-status', '', true);
        loadAppPasswords();
        return;
      }
      if (result.otp === 'required') { askSecondFactor('apppassword-otp', 'apppassword-code', 'apppassword-status'); return; }
      say('apppassword-status', describe(result, t('Could not make the password')), false);
    });
  });
  var loadSessions = function () {
    return call('GET', '/api/v1/me/sessions').then(function (result) {
      if (result.status !== 200 || !result.data) { say('session-status', describe(result, t('Could not read the sessions')), false); return; }
      var rows = el('session-rows');
      clear(rows);
      (result.data.sessions || []).forEach(function (s) {
        var tr = document.createElement('tr');
        tr.appendChild(node('td', ago(s.created_seconds_ago)));
        tr.appendChild(node('td', ago(s.idle_seconds)));
        var which = document.createElement('td');
        if (s.current) { which.appendChild(node('span', t('This browser'), 'badge info')); }
        if (s.support) { which.appendChild(node('span', t('Support (an administrator)'), 'badge warn')); }
        if (s.bearer) { which.appendChild(node('span', t('A second account in a webmail page'), 'badge')); }
        tr.appendChild(which);
        var actions = document.createElement('td');
        if (!s.current) {
          var end = button(t('End'));
          end.addEventListener('click', function () {
            call('DELETE', '/api/v1/me/sessions/' + s.id).then(function (r) {
              if (r.status === 200) { say('session-status', t('Ended.'), true); loadSessions(); } else { say('session-status', describe(r, t('Could not end it')), false); }
            });
          });
          actions.appendChild(end);
        }
        tr.appendChild(actions);
        rows.appendChild(tr);
      });
    });
  };
  el('sessions-end-others').addEventListener('click', function () {
    call('DELETE', '/api/v1/me/sessions').then(function (r) {
      if (r.status === 200 && r.data) { say('session-status', (r.data.ended || 0) + t(' ended.'), true); loadSessions(); } else { say('session-status', describe(r, t('Could not end them')), false); }
    });
  });
  // Everything that has signed in as this person, whatever protocol it spoke.
  // The list the server keeps is one row per client - the protocol, the address
  // and the program where there is one - so a phone fetching mail and the same
  // person's browser are two rows, which is what makes it possible to point at
  // one and say 'not me'.
  var loadDevices = function () {
    return call('GET', '/api/v1/me/devices').then(function (result) {
      if (result.status !== 200 || !result.data) { say('device-status', describe(result, t('Could not read the devices')), false); return; }
      var rows = el('device-rows');
      clear(rows);
      var list = result.data.devices || [];
      list.forEach(function (d) {
        var tr = document.createElement('tr');
        var what = document.createElement('td');
        what.appendChild(node('span', String(d.protocol || '')));
        if (d.current) { what.appendChild(node('span', t('This browser'), 'badge info')); }
        if (d.revoked) { what.appendChild(node('span', t('Revoked'), 'badge warn')); }
        tr.appendChild(what);
        tr.appendChild(node('td', d.address || ''));
        tr.appendChild(node('td', d.agent || '-'));
        tr.appendChild(node('td', (d.credential === 'app_password' && d.credential_name) ? d.credential_name : String(d.credential || '')));
        tr.appendChild(node('td', d.last_seen || ''));
        var actions = document.createElement('td');
        var revoke = button(t('Revoke'));
        revoke.addEventListener('click', function () {
          var also = el('device-credential').checked ? '?credential=revoke' : '';
          call('DELETE', '/api/v1/me/devices/' + encodeURIComponent(d.id) + also).then(function (r) {
            if (r.status === 200) { say('device-status', t('Revoked. Anything it had open has been closed.'), true); loadDevices(); }
            else { say('device-status', describe(r, t('Could not revoke it')), false); }
          });
        });
        actions.appendChild(revoke);
        tr.appendChild(actions);
        rows.appendChild(tr);
      });
      el('device-empty').hidden = list.length > 0;
      el('device-table').hidden = list.length === 0;
    });
  };
  // The recovery address and the one-time codes. The address is only ever
  // shown masked, here as everywhere: the page never holds the whole of it
  // after it has been set, because the server never answers it.
  var renderRecovery = function (state) {
    var card = el('recovery-card');
    card.hidden = !state.enabled;
    if (!state.enabled) { return; }
    var address = state.address || '';
    if (!state.address_set) { el('recovery-state').textContent = t('No recovery address yet.'); }
    else if (state.address_verified) { el('recovery-state').textContent = tf('Confirmed: {0}', address); }
    else { el('recovery-state').textContent = tf('Waiting to be confirmed: {0}', address); }
    el('recovery-resend').hidden = !state.address_set || !!state.address_verified;
    el('recovery-clear').hidden = !state.address_set;
    if (!state.available) { el('recovery-state').textContent += ' ' + t('This server has no public address of its own, so it cannot send the link.'); }
    var codes = state.codes || { total: 0, unused: 0 };
    el('recovery-codes-state').textContent = codes.total
      ? tf('{0} of {1} codes are unused.', String(codes.unused), String(codes.total))
      : t('No codes yet.');
  };
  var loadRecovery = function () {
    return call('GET', '/api/v1/me/recovery').then(function (result) {
      if (result.status !== 200 || !result.data) { el('recovery-card').hidden = true; return; }
      renderRecovery(result.data);
    });
  };
  el('recovery-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var body = { address: el('recovery-address').value.trim(), password: el('recovery-password').value };
    call('PUT', '/api/v1/me/recovery', body, secondFactorHeaders('recovery-otp', 'recovery-code')).then(function (result) {
      if (result.status === 200) {
        el('recovery-address').value = ''; el('recovery-password').value = ''; el('recovery-otp').value = ''; el('recovery-code').value = '';
        say('recovery-status', t('Saved. Open the link in the message just sent there to confirm it.'), true);
        loadRecovery();
        return;
      }
      if (result.otp === 'required') { askSecondFactor('recovery-otp', 'recovery-code', 'recovery-status'); return; }
      say('recovery-status', describe(result, t('Could not save that address')), false);
    });
  });
  el('recovery-resend').addEventListener('click', function () {
    call('POST', '/api/v1/me/recovery/resend', {}).then(function (result) {
      if (result.status === 200) { say('recovery-status', t('Sent. Open the link in the message to confirm it.'), true); return; }
      say('recovery-status', describe(result, t('Could not send it')), false);
    });
  });
  el('recovery-clear').addEventListener('click', function () {
    if (!window.confirm(t('Remove the recovery address? A forgotten password would then need an administrator.'))) { return; }
    call('DELETE', '/api/v1/me/recovery').then(function (result) {
      if (result.status === 200) { say('recovery-status', t('Removed.'), true); loadRecovery(); return; }
      say('recovery-status', describe(result, t('Could not remove it')), false);
    });
  });
  el('recovery-codes-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var body = { password: el('recovery-codes-password').value };
    call('POST', '/api/v1/me/recovery/codes', body, secondFactorHeaders('recovery-codes-otp', 'recovery-codes-code')).then(function (result) {
      if (result.status === 201 && result.data) {
        el('recovery-codes-list').textContent = (result.data.codes || []).join('  ');
        el('recovery-codes-new').hidden = false;
        el('recovery-codes-password').value = ''; el('recovery-codes-otp').value = ''; el('recovery-codes-code').value = '';
        say('recovery-codes-status', '', true);
        loadRecovery();
        return;
      }
      if (result.otp === 'required') { askSecondFactor('recovery-codes-otp', 'recovery-codes-code', 'recovery-codes-status'); return; }
      say('recovery-codes-status', describe(result, t('Could not make the codes')), false);
    });
  });
  // ---- Passkeys -------------------------------------------------------------
  // The browser does the cryptography; this page only carries base64url both
  // ways. A browser without WebAuthn, or a page not served over HTTPS, says so
  // and offers nothing, because navigator.credentials is not there to call.
  var passkeysHere = function () {
    try { return !!(window.PublicKeyCredential && navigator.credentials && navigator.credentials.create); }
    catch (e) { return false; }
  };
  var fromB64u = function (text) {
    var padded = String(text || '').replace(/-/g, '+').replace(/_/g, '/');
    while (padded.length % 4) { padded += '='; }
    var raw = atob(padded), bytes = new Uint8Array(raw.length);
    for (var i = 0; i < raw.length; i++) { bytes[i] = raw.charCodeAt(i); }
    return bytes;
  };
  var toB64u = function (buffer) {
    var bytes = new Uint8Array(buffer), text = '';
    for (var i = 0; i < bytes.length; i++) { text += String.fromCharCode(bytes[i]); }
    return btoa(text).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  };
  var loadPasskeys = function () {
    if (!passkeysHere()) { el('passkey-unavailable').hidden = false; el('passkey-form').hidden = true; }
    return call('GET', '/api/v1/passkeys').then(function (result) {
      if (result.status !== 200 || !result.data) { say('passkey-status', describe(result, t('Could not read the passkeys')), false); return; }
      el('passkey-off').hidden = result.data.enabled !== false;
      if (result.data.enabled === false) { el('passkey-form').hidden = true; }
      var rows = el('passkey-rows');
      clear(rows);
      var list = result.data.passkeys || [];
      list.forEach(function (p) {
        var tr = document.createElement('tr');
        tr.appendChild(node('td', p.name));
        tr.appendChild(node('td', p.created ? new Date(Number(p.created) * 1000).toLocaleString() : ''));
        tr.appendChild(node('td', p.last_used ? new Date(Number(p.last_used) * 1000).toLocaleString() : t('never')));
        var actions = document.createElement('td');
        var remove = button(t('Remove'));
        remove.addEventListener('click', function () {
          call('DELETE', '/api/v1/passkeys/' + p.id).then(function (r) {
            if (r.status === 200) { say('passkey-status', t('Removed. That device no longer signs you in.'), true); loadPasskeys(); }
            else { say('passkey-status', describe(r, t('Could not remove it')), false); }
          });
        });
        actions.appendChild(remove);
        tr.appendChild(actions);
        rows.appendChild(tr);
      });
      el('passkey-empty').hidden = list.length > 0;
      el('passkey-table').hidden = list.length === 0;
    });
  };
  el('passkey-form').addEventListener('submit', function (event) {
    event.preventDefault();
    if (!passkeysHere()) { return; }
    var name = el('passkey-name').value, password = el('passkey-password').value;
    say('passkey-status', '', true);
    call('POST', '/api/v1/passkeys/options', {}).then(function (result) {
      if (result.status !== 200 || !result.data) { say('passkey-status', describe(result, t('Could not add the passkey')), false); return null; }
      var options = result.data;
      return navigator.credentials.create({ publicKey: {
        challenge: fromB64u(options.challenge),
        rp: options.rp,
        user: { id: fromB64u(options.user.id), name: options.user.name, displayName: options.user.displayName },
        pubKeyCredParams: options.pubKeyCredParams,
        timeout: options.timeout,
        attestation: options.attestation,
        authenticatorSelection: options.authenticatorSelection,
        excludeCredentials: (options.excludeCredentials || []).map(function (c) { return { type: c.type, id: fromB64u(c.id) }; })
      } });
    }).then(function (made) {
      if (!made) { return; }
      var response = made.response;
      var body = { name: name, password: password, credential: { id: made.id, response: {
        clientDataJSON: toB64u(response.clientDataJSON),
        attestationObject: toB64u(response.attestationObject),
        transports: (response.getTransports ? response.getTransports() : []) || []
      } } };
      return call('POST', '/api/v1/passkeys', body, secondFactorHeaders('passkey-otp', 'passkey-code')).then(function (r) {
        if (r.status === 201) {
          el('passkey-name').value = ''; el('passkey-password').value = ''; el('passkey-otp').value = ''; el('passkey-code').value = '';
          say('passkey-status', t('Added. You can sign in with this device from now on.'), true);
          loadPasskeys();
          return;
        }
        if (r.otp === 'required') { askSecondFactor('passkey-otp', 'passkey-code', 'passkey-status'); return; }
        say('passkey-status', describe(r, t('Could not add the passkey')), false);
      });
    }).catch(function () {
      // The person pressed cancel, or the authenticator refused. Neither is a
      // failure to report as one; the challenge is spent and a new one is
      // asked for next time.
      say('passkey-status', t('Could not add the passkey'), false);
    });
  });

  var loadSecurity = function () { loadPasskeys(); loadAppPasswords(); loadRecovery(); loadSessions(); loadDevices(); loadSmime(); loadPgpPage(); };

  // Focus follows a change of view to its title, unless the view put the
  // focus somewhere itself, so that a screen reader announces where it is.
  // ---- Rules: a form that writes Sieve, and reads its own rules back from the
  // script's first line, so a script written by hand is never touched.
  var RULES_MARK = '# hmailserver-webmail-rules v1 ';
  var rulesOf = function (script) {
    var text = String(script || '').trim();
    if (!text) { return []; }
    if (text.indexOf(RULES_MARK) !== 0) { return null; }
    try { var list = JSON.parse(text.split('\n')[0].slice(RULES_MARK.length)); return Array.isArray(list) ? list : null; } catch (e) { return null; }
  };
  var sieveString = function (s) { return '"' + String(s).replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"'; };
  var conditionOf = function (r) {
    if (r.field === 'thread') {
      var ids = Array.isArray(r.text) ? r.text : [r.text];
      return 'anyof (' + ids.map(function (id) { return 'header :contains ["references", "in-reply-to"] ' + sieveString(id); }).join(', ') + ')';
    }
    var text = sieveString(r.text);
    if (r.field === 'any') { return 'anyof (header :contains "from" ' + text + ', header :contains ["to", "cc"] ' + text + ', header :contains "subject" ' + text + ')'; }
    if (r.field === 'to') { return 'header :contains ["to", "cc"] ' + text; }
    return 'header :contains ' + sieveString(r.field) + ' ' + text;
  };
  var actionOf = function (r) {
    if (r.action === 'move') { return 'fileinto ' + sieveString(r.folder) + '; stop;'; }
    if (r.action === 'flag') { return 'addflag "\\\\Flagged";'; }
    if (r.action === 'read') { return 'addflag "\\\\Seen";'; }
    if (r.action === 'label') { return 'addflag ' + sieveString(r.label) + ';'; }
    return 'discard; stop;';
  };
  var scriptOf = function (rules) {
    if (!rules.length) { return ''; }
    return RULES_MARK + JSON.stringify(rules) + '\n' +
      'require ["fileinto", "imap4flags"];\n' +
      rules.map(function (r) { return 'if ' + conditionOf(r) + ' {\n  ' + actionOf(r) + '\n}'; }).join('\n') + '\n';
  };
  var describeRule = function (r) {
    if (r.field === 'thread') { return { when: tf('A reply in the conversation "{0}"', r.subject || ''), then: 'move to ' + r.folder }; }
    var when = (r.field === 'any' ? 'From, To or Subject' : r.field === 'to' ? 'To or Cc' : r.field.charAt(0).toUpperCase() + r.field.slice(1)) + ' contains "' + r.text + '"';
    var then = r.action === 'move' ? 'move to ' + r.folder : r.action === 'label' ? 'label ' + r.label : r.action === 'flag' ? 'flag' : r.action === 'read' ? 'mark as read' : 'delete';
    return { when: when, then: then };
  };
  var currentRules = [];
  var renderRules = function (script) {
    var rules = rulesOf(script);
    el('rules-handwritten').hidden = rules !== null;
    el('rule-form').hidden = rules === null;
    currentRules = rules || [];
    var rows = el('rule-rows');
    clear(rows);
    currentRules.forEach(function (r, i) {
      var d = describeRule(r);
      var tr = document.createElement('tr');
      tr.appendChild(node('td', d.when));
      tr.appendChild(node('td', d.then));
      var actions = document.createElement('td');
      var remove = button(t('Remove'));
      remove.addEventListener('click', function () { var next = currentRules.slice(); next.splice(i, 1); saveRules(next); });
      actions.appendChild(remove);
      tr.appendChild(actions);
      rows.appendChild(tr);
    });
    el('rule-empty').hidden = rules === null || currentRules.length > 0;
    el('rule-table').hidden = rules === null || currentRules.length === 0;
    fillFolderSelect(el('rule-folder'), 0);
  };
  // ---- Rows coloured by a rule ----------------------------------------------
  // Outlook's conditional formatting: a field, a text and a colour, kept with
  // the account's preferences, edited on the Filters page beside the rules.
  // The first rule that matches paints the row's edge; the labels keep their
  // own colours, this is a rule on the row.
  var rowColours = function () {
    try {
      var list = JSON.parse(pref('row_colours') || '[]');
      return Array.isArray(list) ? list.filter(function (r) { return r && r.text && /^#[0-9a-f]{6}$/i.test(r.colour || ''); }) : [];
    } catch (why) { return []; }
  };
  var colourFor = function (m) {
    var rules = rowColours();
    if (!rules.length || !m) { return ''; }
    var from = String(m.from || '').toLowerCase();
    var to = (String(m.to || '') + ' ' + String(m.cc || '')).toLowerCase();
    var subject = String(m.subject || '').toLowerCase();
    for (var i = 0; i < rules.length; i++) {
      var needle = String(rules[i].text).toLowerCase();
      var field = rules[i].field;
      var hay = field === 'from' ? from : field === 'to' ? to : field === 'subject' ? subject : from + ' ' + to + ' ' + subject;
      if (hay.indexOf(needle) >= 0) { return rules[i].colour; }
    }
    return '';
  };
  var paintRow = function (row, colour) {
    // On the leading edge: --lead is 1, or -1 when the page is written right to left.
    if (colour) { row.setAttribute('style', 'box-shadow:inset calc(4px * var(--lead, 1)) 0 0 ' + colour); }
  };
  var writeRowColours = function (list) {
    return savePrefs({ row_colours: JSON.stringify(list.slice(0, 20)) }).then(function (ok) { if (ok) { renderColourRules(); lastListing = null; } return ok; });
  };
  var renderColourRules = function () {
    var box = el('colour-rows');
    clear(box);
    rowColours().forEach(function (r, i) {
      var line = node('div', undefined, 'row');
      var swatch = node('span', undefined, 'swatch'); swatch.setAttribute('style', 'display:inline-block;width:14px;height:14px;border-radius:3px;background:' + r.colour);
      line.appendChild(swatch);
      var fieldWord = r.field === 'from' ? t('From') : r.field === 'to' ? t('To or Cc') : r.field === 'subject' ? t('Subject') : t('From, To or Subject');
      line.appendChild(node('span', fieldWord + ' ' + t('contains') + ' ' + r.text));
      var remove = button(t('Remove'), 'btn ghost sm');
      remove.addEventListener('click', function () { writeRowColours(rowColours().filter(function (x, j) { return j !== i; })); });
      line.appendChild(remove);
      box.appendChild(line);
    });
  };
  el('colour-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var text = el('colour-text').value.trim();
    var colour = String(el('colour-value').value || '').toLowerCase();
    if (!text || !/^#[0-9a-f]{6}$/.test(colour)) { return; }
    writeRowColours(rowColours().concat([{ field: el('colour-field').value, text: text, colour: colour }])).then(function (ok) { if (ok) { el('colour-text').value = ''; } });
  });
  var saveRules = function (rules) {
    var script = scriptOf(rules);
    var before = el('filter-script').value;
    return call('PUT', '/api/v1/me/filters', { script: script }).then(function (result) {
      if (result.status === 200) {
        el('filter-script').value = script; renderRules(script); say('rule-status', t('Saved.'), true);
        if (before !== script) {
          toast(t('Rules saved.'), function () {
            call('PUT', '/api/v1/me/filters', { script: before }).then(function (back) {
              if (back.status === 200) { el('filter-script').value = before; renderRules(before); say('rule-status', t('Saved.'), true); }
              else { say('rule-status', describe(back, t('Could not save the rules')), false); }
            });
          });
        }
        return true;
      }
      say('rule-status', describe(result, t('Could not save the rules')), false);
      return false;
    });
  };
  el('rule-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var rule = { field: el('rule-field').value, text: el('rule-text').value.trim(), action: el('rule-action').value };
    if (!rule.text) { return; }
    if (rule.action === 'label') {
      rule.label = el('rule-label').value.trim();
      if (!validLabel(rule.label)) { say('rule-status', t('Give the label: one word, no spaces, quotes or brackets.'), false); return; }
      if (!colourKeyOf(rule.label)) { labelColours[rule.label] = '#36c2ff'; saveLabelColours(); }
    }
    if (rule.action === 'move') {
      var folderId = Number(el('rule-folder').value);
      var folder = foldersById[folderId];
      if (!folder) { say('rule-status', t('Choose a folder to move to.'), false); return; }
      rule.folder = folder.path;
    }
    saveRules(currentRules.concat([rule])).then(function (ok) { if (ok) { el('rule-text').value = ''; } });
  });
  el('rule-action').addEventListener('change', function () { el('rule-folder').hidden = el('rule-action').value !== 'move'; el('rule-label').hidden = el('rule-action').value !== 'label'; });

  // ---- A receipt when asked, an unsubscribe when offered
  el('receipt-send').addEventListener('click', function () {
    if (!current) { return; }
    call('POST', '/api/v1/me/messages/' + current.id + '/receipt').then(function (result) {
      el('message-receipt').hidden = true;
      say('mail-status', result.status === 201 ? t('A read receipt was sent.') : describe(result, t('The receipt could not be sent')), result.status === 201);
    });
  });
  el('receipt-never').addEventListener('click', function () { el('message-receipt').hidden = true; savePrefs({ receipts: 'never' }); });
  // ---- S/MIME ---------------------------------------------------------------
  // The cryptography is /portal-smime.js (window.hmSmime): DER, X.509, CMS and
  // the MIME around them, on the Web Crypto API. This block keeps the account's
  // key store in step with the server (GET/PUT/DELETE /api/v1/me/smime...),
  // unlocks the private keys with the password - which never leaves the
  // browser - and puts what the module finds on the page.
  var smime = function () { return window.hmSmime; };
  // Whether the page can do any of this: the module loaded, and the browser
  // gave it its cryptography. A page opened over plain HTTP - behind a
  // reverse proxy that speaks HTTPS to the world and HTTP to this server,
  // say - is not a secure context, and the browser withholds crypto.subtle
  // from it. Before this was asked, such a page offered Sign and Encrypt and
  // failed at send time with the name of a missing property; now the
  // controls stay away and the Security page says why.
  var smimeUsable = function () { return !!(smime() && smime().available !== false && window.isSecureContext !== false); };
  // current: the account's current password, and only while the keys are not
  // wrapped under it - see loadSmime. It is in this page and nowhere else,
  // and it goes the moment it has been used or the account signs out.
  var smimeState = { own: [], recipients: [], unlocked: {}, loaded: false, limits: {}, current: '' };
  // True when the session was started with an app password: a mailbox
  // credential, never the account's own password, so it is not what the
  // keys are wrapped under and must never become what they are wrapped under.
  var smimeViaAppPassword = false;
  var smimePassword = '';
  var smimeCerts = {};
  // Sign-out, for this block. The unlocked map holds the account's private
  // keys themselves - an extractable CryptoKey, the PKCS#8 it was made from
  // and, for an RSA key, its secret numbers - so leaving it standing hands
  // the previous account's keys to whoever signs in next in this tab, and
  // reads their sealed mail with them. Everything here goes back to what it
  // was before the first sign-in: the keys, the certificates the page parsed,
  // the password typed at the gate, the two tables of the store, and both
  // password boxes. A dialogue still waiting for a password is refused
  // rather than left open over the next account's session.
  var smimeForget = function () {
    smimeSettle(false, '');
    smimeState = { own: [], recipients: [], unlocked: {}, loaded: false, limits: {}, current: '' };
    smimeViaAppPassword = false;
    smimePassword = '';
    smimeCerts = {};
    clear(el('smime-own-rows'));
    clear(el('smime-recipient-rows'));
    el('smime-own-table').hidden = true;
    el('smime-recipient-table').hidden = true;
    el('smime-own-empty').hidden = false;
    el('smime-recipient-empty').hidden = false;
    el('smime-unlock-password').value = '';
    el('smime-passphrase').value = '';
    el('smime-file').value = '';
    say('smime-status', '', true);
    el('compose-sign-label').hidden = true;
    el('compose-encrypt-label').hidden = true;
    el('compose-smime-note').hidden = true;
    el('message-smime').hidden = true;
    el('message-smime-unlock').hidden = true;
    el('message-smime-text').textContent = '';
  };
  var smimeAddress = function (text) { var m = String(text || '').match(/<([^>]+)>/); return (m ? m[1] : String(text || '')).trim().toLowerCase(); };
  var smimeAddresses = function (text) { return String(text || '').split(/[,;]/).map(smimeAddress).filter(function (a) { return a.indexOf('@') > 0; }); };
  var smimeFromAddress = function () { var f = el('compose-from'); return (!f.hidden && f.value) ? smimeAddress(f.value) : me; };
  var smimeOwnFor = function (address) { return smimeState.own.filter(function (o) { return o.address === address; })[0] || null; };
  var smimeRecipientsFor = function (address) { return smimeState.recipients.filter(function (r) { return r.address === address; }); };
  var smimeParse = function (entry) {
    if (!smimeCerts[entry.fingerprint]) { try { smimeCerts[entry.fingerprint] = smime().parseCertificate(smime().b64decode(entry.certificate)); } catch (e) { return null; } }
    return smimeCerts[entry.fingerprint];
  };
  var smimeDate = function (seconds) { return seconds ? new Date(seconds * 1000).toLocaleDateString() : ''; };
  var smimeSeconds = function (date) { return Math.floor(date.getTime() / 1000); };
  var smimeRenderRows = function (tbody, list, kind) {
    clear(tbody);
    list.forEach(function (entry) {
      var tr = document.createElement('tr');
      tr.appendChild(node('td', entry.address));
      tr.appendChild(node('td', (entry.name ? entry.name + ' - ' : '') + entry.fingerprint.slice(0, 16)));
      tr.appendChild(node('td', smimeDate(entry.not_after)));
      var td = document.createElement('td');
      var remove = node('button', t('Remove'), 'btn ghost sm');
      remove.type = 'button';
      remove.addEventListener('click', function () {
        call('DELETE', '/api/v1/me/smime/' + kind + '/' + entry.fingerprint).then(function (result) {
          if (result.status === 200) { delete smimeState.unlocked[entry.fingerprint]; loadSmime(); return; }
          say('smime-status', describe(result, t('Could not remove')), false);
        });
      });
      td.appendChild(remove);
      tr.appendChild(td);
      tbody.appendChild(tr);
    });
  };
  // The compose form: Sign is offered when the From address has a certificate
  // of the account's own; Encrypt when there is any certificate to encrypt for;
  // and a note names what is missing for what is ticked.
  var smimeComposeHints = function () {
    if (!smimeState.loaded || !smimeUsable()) { return; }
    var own = smimeOwnFor(smimeFromAddress());
    el('compose-sign-label').hidden = !own;
    el('compose-encrypt-label').hidden = !own && !smimeState.recipients.length;
    var hints = [];
    var missing = [];
    smimeAddresses(el('compose-to').value + ',' + el('compose-cc').value + ',' + el('compose-bcc').value).forEach(function (a) {
      if (!smimeRecipientsFor(a).length && !smimeOwnFor(a) && missing.indexOf(a) < 0) { missing.push(a); }
    });
    if (el('compose-encrypt').checked && missing.length) { hints.push(tf('Encrypt: no certificate for {0}.', missing.join(', '))); }
    if (el('compose-sign').checked && !own) { hints.push(t('Sign: no certificate of yours for this From address.')); }
    el('compose-smime-note').textContent = hints.join(' ');
    el('compose-smime-note').hidden = !hints.length;
  };
  ['compose-to', 'compose-cc', 'compose-bcc'].forEach(function (id) { el(id).addEventListener('input', smimeComposeHints); });
  ['compose-from', 'compose-sign', 'compose-encrypt'].forEach(function (id) { el(id).addEventListener('change', smimeComposeHints); });
  var loadSmime = function () {
    if (!smime()) { return Promise.resolve(); }
    el('smime-unavailable').hidden = smimeUsable();
    el('smime-body').hidden = !smimeUsable();
    if (!smimeUsable()) { return Promise.resolve(); }
    // The state this load is for. A sign-out replaces smimeState while the
    // request is in the air; what comes back then belongs to an account that
    // has gone, and is not rendered or kept.
    var state = smimeState;
    return call('GET', '/api/v1/me/smime').then(function (result) {
      if (result.status !== 200 || !result.data || state !== smimeState) { return; }
      state.own = result.data.own || [];
      state.recipients = result.data.recipients || [];
      state.limits = result.data.limits || {};
      state.loaded = true;
      smimeRenderRows(el('smime-own-rows'), state.own, 'own');
      el('smime-own-table').hidden = !state.own.length;
      el('smime-own-empty').hidden = !!state.own.length;
      smimeRenderRows(el('smime-recipient-rows'), state.recipients, 'recipients');
      el('smime-recipient-table').hidden = !state.recipients.length;
      el('smime-recipient-empty').hidden = !!state.recipients.length;
      smimeComposeHints();
      // The password typed to sign in opens the keys once, then is forgotten.
      // When it will not open them - an administrator changed the password,
      // and the keys are wrapped under the old one - the dialogue asks for
      // the old one now, while the page still has the current one the server
      // has just accepted; the current one is held for exactly as long as
      // that dialogue is open, and goes with it whether it was answered or
      // cancelled. The submit handler wraps the keys again under it, which is
      // what the dialogue says. An app password is a mailbox credential and
      // not the account's password: it neither opens the keys nor wraps them.
      var typed = smimePassword;
      smimePassword = '';
      state.current = '';
      if (typed && state.own.length && !smimeViaAppPassword) {
        smimeUnlockWith(typed).then(null, function (e) {
          if (!e || e.message !== 'wrong password' || state !== smimeState) { return; }
          state.current = typed;
          var drop = function () { state.current = ''; };
          smimeAsk(t('Unlock your keys'), t('The password you signed in with does not open your keys. If an administrator changed your password, enter the one you had before; the keys are then re-wrapped with the current one.'), 'unlock').then(drop, drop);
        });
      }
    });
  };
  // Unwraps every own key that is still wrapped, with the password given;
  // rejects with the reason when one would not open.
  var smimeUnlockWith = function (password) {
    var chain = Promise.resolve();
    var failed = '';
    // Bound to the account's state at the start: the derivation takes long
    // enough that a sign-out can happen under it, and a key that opens after
    // that belongs to nobody the page is showing - it is not kept anywhere.
    var state = smimeState;
    state.own.forEach(function (entry) {
      chain = chain.then(function () {
        if (state !== smimeState || state.unlocked[entry.fingerprint] || !entry.key) { return null; }
        return smime().unwrapPrivateKey(entry.key, password).then(function (pkcs8) {
          if (state !== smimeState) { return null; }
          return smime().importPrivateKey(pkcs8).then(function (key) {
            if (state !== smimeState) { return; }
            var chainCerts = (entry.chain || []).map(function (c) { try { return smime().parseCertificate(smime().b64decode(c)); } catch (e) { return null; } }).filter(Boolean);
            state.unlocked[entry.fingerprint] = { key: key, certificate: smimeParse(entry), chain: chainCerts, address: entry.address, entry: entry };
          });
        }, function (e) { failed = e && e.message ? e.message : String(e); });
      });
    });
    return chain.then(function () { if (failed) { throw new Error(failed); } });
  };
  var smimeUnlockedList = function () { return Object.keys(smimeState.unlocked).map(function (k) { return smimeState.unlocked[k]; }); };
  var smimeAllUnlocked = function () { return smimeState.own.every(function (e) { return !!smimeState.unlocked[e.fingerprint]; }); };
  // The dialog: in 'unlock' mode it unwraps the keys with what is typed and
  // resolves; in 'password' mode it just hands the text back. Cancel rejects.
  var smimeWaiters = [];
  var smimeAsk = function (title, note, mode) {
    el('smime-unlock-title').textContent = title;
    el('smime-unlock-note').textContent = note;
    el('smime-unlock-form').setAttribute('data-ask', mode);
    say('smime-unlock-status', '', true);
    el('smime-unlock-password').value = '';
    el('smime-unlock').hidden = false;
    el('smime-unlock-password').focus();
    return new Promise(function (resolve, reject) { smimeWaiters.push({ resolve: resolve, reject: reject }); });
  };
  var smimeSettle = function (ok, value) { el('smime-unlock').hidden = true; smimeWaiters.splice(0).forEach(function (w) { if (ok) { w.resolve(value); } else { w.reject(value); } }); };
  var smimeUnlock = function () {
    if (smimeAllUnlocked()) { return Promise.resolve(''); }
    return smimeAsk(t('Unlock your keys'), t('Your password unwraps the private key in this browser; nothing is sent to the server.'), 'unlock');
  };
  el('smime-unlock-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var password = el('smime-unlock-password').value;
    if (!password) { say('smime-unlock-status', t('Enter your password.'), false); return; }
    if (el('smime-unlock-form').getAttribute('data-ask') === 'password') { smimeSettle(true, password); return; }
    smimeUnlockWith(password).then(function () {
      // The keys opened with a password this session did not sign in with:
      // an administrator changed it, and the wrapping is still the old
      // password's. Both are here at this moment - the current one because
      // loadSmime opened this dialogue while it still had it - so wrap them
      // again with the current one now, which is what the dialogue promises,
      // and forget it either way.
      var current = smimeState.current;
      smimeState.current = '';
      if (!current || current === password) { smimeSettle(true, password); return; }
      smimeRewrapUnlocked(current).then(function (count) {
        smimeSettle(true, current);
        if (count) { say('smime-status', t('Your keys were re-wrapped with the new password.'), true); loadSmime(); }
      }, function () { smimeSettle(true, password); });
    }, function (e) {
      if (!e || e.message !== 'wrong password') { say('smime-unlock-status', String(e && e.message || e), false); return; }
      // Two sentences, because only one of them is true at a time: the page
      // can wrap the keys again only while it holds the current password,
      // which is in the dialogue opened at sign-in and nowhere after it.
      say('smime-unlock-status', smimeState.current
        ? t('Wrong password. If an administrator changed your password, enter the one you had before; the keys are then re-wrapped with the current one.')
        : t('Wrong password. If an administrator changed your password, enter the one you had before; to have the keys re-wrapped with the current one, sign out, sign in again and unlock them once.'), false);
    });
  });
  el('smime-unlock-cancel').addEventListener('click', function () { smimeSettle(false, t('Not sent. The message is still here.')); });
  // Every unlocked key wrapped again under the password given and stored.
  var smimeRewrapUnlocked = function (password) {
    var chain = Promise.resolve();
    var count = 0;
    smimeUnlockedList().forEach(function (u) {
      if (!u.entry) { return; }
      chain = chain.then(function () {
        return smime().wrapPrivateKey(u.key.pkcs8, password).then(function (wrapped) {
          var e = u.entry;
          return call('PUT', '/api/v1/me/smime/own', { address: e.address, name: e.name, fingerprint: e.fingerprint, certificate: e.certificate, chain: e.chain || [], key: wrapped, not_after: e.not_after }).then(function (result) { if (result.status === 200 || result.status === 201) { count++; } });
        });
      });
    });
    return chain.then(function () { return count; });
  };
  // A password change on this page: the keys open with the old password and
  // are wrapped again with the new. Answers a note for the status line.
  var smimeRewrap = function (oldPassword, newPassword) {
    if (!smime() || !smimeState.own.length) { return Promise.resolve(''); }
    return smimeUnlockWith(oldPassword).then(null, function () {}).then(function () { return smimeRewrapUnlocked(newPassword); }).then(function (count) {
      return count ? t('Your keys were re-wrapped with the new password.') : '';
    }, function () { return ''; });
  };
  // ---- reading a message ----
  var smimeContentType = function (headers) {
    var m = String(headers || '').match(/^content-type:\s*([^\r\n]*(?:\r?\n[ \t][^\r\n]*)*)/im);
    return m ? m[1].replace(/\r?\n[ \t]+/g, ' ').trim().toLowerCase() : '';
  };
  var smimeRun = 0;
  var smimeInspect = function (m) {
    el('message-smime').hidden = true;
    el('message-smime-unlock').hidden = true;
    if (!smime()) { return; }
    var type = smimeContentType(m.headers);
    if (type.indexOf('multipart/signed') !== 0 && type.indexOf('application/pkcs7-mime') !== 0 && type.indexOf('application/x-pkcs7-mime') !== 0) { return; }
    if (!smimeUsable()) {
      el('message-smime-text').textContent = t('Signing and encryption are unavailable: the browser withholds its cryptography from a page that was not opened over HTTPS.');
      el('message-smime').hidden = false;
      return;
    }
    var run = ++smimeRun;
    el('message-smime-text').textContent = t('Checking the signature...');
    el('message-smime').hidden = false;
    var ready = smimeState.loaded ? Promise.resolve() : loadSmime();
    ready.then(function () {
      return apiFetch('/api/v1/me/messages/' + m.id + '/source', { cache: 'no-store', credentials: 'same-origin' }).then(function (r) { return r.ok ? r.arrayBuffer() : Promise.reject(new Error('HTTP ' + r.status)); });
    }).then(function (buffer) {
      return smime().readMessage(new Uint8Array(buffer), smimeUnlockedList());
    }).then(function (result) {
      if (run !== smimeRun || !current || current.id !== m.id) { return; }
      smimeShow(m, result);
    }, function (e) {
      if (run !== smimeRun) { return; }
      el('message-smime-text').textContent = tf('Could not check: {0}', e && e.message ? e.message : String(e));
    });
  };
  var smimeBadge = function (text, cls) { el('message-badges').appendChild(node('span', text, 'badge ' + cls)); };
  // A verified signer's certificate is kept, so a reply can be encrypted.
  var smimeRemember = function (signer, who) {
    smime().fingerprint(signer).then(function (fp) {
      if (smimeState.recipients.some(function (r) { return r.fingerprint === fp; }) || smimeState.own.some(function (o) { return o.fingerprint === fp; })) { return; }
      return call('PUT', '/api/v1/me/smime/recipients', { address: who, name: signer.name, fingerprint: fp, certificate: smime().b64encode(signer.raw), not_after: smimeSeconds(signer.notAfter) }).then(function (result) {
        if (result.status !== 201) { return; }
        smimeCerts[fp] = signer;
        el('message-smime-text').textContent = (el('message-smime-text').textContent + ' ' + tf('The certificate of {0} was kept for encrypting replies.', who)).trim();
        el('message-smime').hidden = false;
        loadSmime();
      });
    }, function () {});
  };
  var smimeShow = function (m, result) {
    var lines = [];
    if (result.encrypted) {
      if (result.encrypted.ok) { lines.push(t('Encrypted; decrypted in this browser. It can be searched by its headers only.')); smimeBadge(t('Encrypted'), 'good'); }
      else if (smimeState.own.length && !smimeAllUnlocked()) { lines.push(t('Encrypted. Unlock your keys to read it.')); el('message-smime-unlock').hidden = false; smimeBadge(t('Encrypted'), 'muted'); }
      else { lines.push(tf('Encrypted, and cannot be read here: {0}', result.encrypted.error)); smimeBadge(t('Encrypted'), 'muted'); }
    }
    if (result.signed) {
      var s = result.signed;
      var who = s.signer ? (s.signer.emails[0] || s.signer.name) : '';
      if (!s.valid) { smimeBadge(tf('Signature invalid: {0}', s.error), 'bad'); }
      else if (s.signer.emails.indexOf(smimeAddress(m.from)) < 0) { smimeBadge(tf('Signed by {0}, not the sender', who), 'warn'); }
      else {
        var certs = [s.signer].concat(s.certificates.filter(function (c) { return c !== s.signer; })).map(function (c) { return smime().b64encode(c.raw); });
        call('POST', '/api/v1/me/smime/chain', { certificates: certs, purpose: 'sign' }).then(function (verdict) {
          if (!current || current.id !== m.id) { return; }
          var d = verdict.data || {};
          if (verdict.status === 200 && d.trusted) { smimeBadge(tf('Signed by {0}', who), 'good'); }
          else { smimeBadge(tf('Signed by {0} (certificate not trusted: {1})', who, d.error || ('HTTP ' + verdict.status)), 'warn'); }
          smimeRemember(s.signer, who);
        });
      }
    }
    if (result.content && result.encrypted && result.encrypted.ok) {
      var c = result.content;
      var text = c.text || (c.html ? (new DOMParser().parseFromString(c.html, 'text/html').body.textContent || '') : '');
      el('message-text').textContent = text || '(no text)';
      var attachments = el('message-attachments');
      clear(attachments);
      if (c.attachments.length) {
        attachments.appendChild(node('span', t('Attachments: ')));
        c.attachments.forEach(function (a, i) {
          var link = node('a', a.name + ' (' + a.type + ', ' + format(a.bytes.length) + ')');
          link.href = URL.createObjectURL(new Blob([a.bytes], { type: a.type }));
          link.setAttribute('download', a.name);
          if (i > 0) { attachments.appendChild(node('span', ', ')); }
          attachments.appendChild(link);
        });
      }
    }
    el('message-smime-text').textContent = lines.join(' ');
    el('message-smime').hidden = !lines.length;
  };
  el('message-smime-unlock').addEventListener('click', function () { smimeUnlock().then(function () { if (current) { smimeInspect(current); } }, function () {}); });
  // ---- writing a message ----
  // The body the form made, turned into the MIME entity the server sends as
  // it is: signed with the From address's key (unlocked first), encrypted
  // for every recipient's certificate and the sender's own.
  var smimePrepare = function (body) {
    if (!body.sign && !body.encrypt) { return Promise.resolve(body); }
    if (!smime()) { return Promise.reject(t('The S/MIME module did not load.')); }
    if (!smimeUsable()) { return Promise.reject(t('Signing and encryption are unavailable: the browser withholds its cryptography from a page that was not opened over HTTPS.')); }
    var own = smimeOwnFor(body.from ? smimeAddress(body.from) : me);
    if (body.sign && !own) { return Promise.reject(t('Sign: no certificate of yours for this From address.')); }
    var recipients = [];
    if (body.encrypt) {
      var missing = [];
      smimeAddresses(body.to + ',' + body.cc + ',' + body.bcc).forEach(function (a) {
        var found = smimeRecipientsFor(a).map(smimeParse).filter(Boolean);
        var mine = smimeOwnFor(a);
        if (mine) { var pm = smimeParse(mine); if (pm) { found.push(pm); } }
        if (!found.length) { if (missing.indexOf(a) < 0) { missing.push(a); } return; }
        found.forEach(function (c) { if (recipients.indexOf(c) < 0) { recipients.push(c); } });
      });
      if (missing.length) { return Promise.reject(tf('Encrypt: no certificate for {0}.', missing.join(', '))); }
      if (own) { var mineCert = smimeParse(own); if (mineCert && recipients.indexOf(mineCert) < 0) { recipients.push(mineCert); } }
    }
    return (body.sign ? smimeUnlock() : Promise.resolve('')).then(function () {
      var signer = body.sign ? smimeState.unlocked[own.fingerprint] : null;
      if (body.sign && !signer) { throw new Error(t('Sign: the key could not be unlocked.')); }
      return smime().composeMessage({ headers: {}, text: body.text || '', html: body.html || '', attachments: body.attachments || [], sign: signer, encrypt: body.encrypt ? recipients : null });
    }).then(function (message) {
      var out = {};
      ['to', 'cc', 'bcc', 'from', 'subject', 'receipt', 'in_reply_to', 'references', 'answered_id'].forEach(function (k) { if (body[k] !== undefined && body[k] !== '') { out[k] = body[k]; } });
      // The module writes only MIME-Version above the entity; the server puts its own on.
      out.mime = message.replace(/^MIME-Version: 1\.0\r\n/, '');
      return out;
    });
  };
  // ---- the Security page ----
  el('smime-import-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var file = (el('smime-file').files || [])[0];
    if (!file) { say('smime-status', t('Choose a file first.'), false); return; }
    if (!smime()) { say('smime-status', t('The S/MIME module did not load.'), false); return; }
    if (!smimeUsable()) { say('smime-status', t('Signing and encryption are unavailable: the browser withholds its cryptography from a page that was not opened over HTTPS.'), false); return; }
    var passphrase = el('smime-passphrase').value;
    say('smime-status', t('Importing...'), true);
    file.arrayBuffer().then(function (buffer) {
      return smime().importAny(new Uint8Array(buffer), passphrase);
    }).then(function (found) {
      var keyed = found.keys.filter(function (k) { return k.certificate; });
      var others = found.certificates.filter(function (c) { return !keyed.some(function (k) { return k.certificate === c; }) && !c.isCA && c.emails.length; });
      if (!keyed.length && !others.length) { throw new Error(t('The file holds no certificate.')); }
      var ask = keyed.length ? smimeAsk(t('Your password protects the key'), t('It is wrapped with your password in this browser before it is stored; the server cannot open it.'), 'password') : Promise.resolve('');
      var state = smimeState;
      return ask.then(function (password) {
        var chain = Promise.resolve();
        var imported = [];
        keyed.forEach(function (k) {
          chain = chain.then(function () {
            var cert = k.certificate;
            var chainCerts = found.certificates.filter(function (c) { return c !== cert; });
            return smime().fingerprint(cert).then(function (fp) {
              return smime().wrapPrivateKey(k.key.pkcs8, password).then(function (wrapped) {
                var address = cert.emails[0] || me;
                var entry = { address: address, name: cert.name, fingerprint: fp, certificate: smime().b64encode(cert.raw), chain: chainCerts.map(function (c) { return smime().b64encode(c.raw); }), key: wrapped, not_after: smimeSeconds(cert.notAfter) };
                return call('PUT', '/api/v1/me/smime/own', entry).then(function (result) {
                  if (result.status !== 200 && result.status !== 201) { throw new Error(describe(result, t('Could not store the key'))); }
                  if (state !== smimeState) { return; }
                  smimeCerts[fp] = cert;
                  state.unlocked[fp] = { key: k.key, certificate: cert, chain: chainCerts, address: address, entry: entry };
                  imported.push(address);
                });
              });
            });
          });
        });
        others.forEach(function (cert) {
          chain = chain.then(function () {
            return smime().fingerprint(cert).then(function (fp) {
              return call('PUT', '/api/v1/me/smime/recipients', { address: cert.emails[0], name: cert.name, fingerprint: fp, certificate: smime().b64encode(cert.raw), not_after: smimeSeconds(cert.notAfter) }).then(function (result) {
                if (result.status === 200 || result.status === 201) { smimeCerts[fp] = cert; imported.push(cert.emails[0]); }
              });
            });
          });
        });
        return chain.then(function () { return imported; });
      });
    }).then(function (imported) {
      el('smime-file').value = ''; el('smime-passphrase').value = '';
      say('smime-status', tf('Imported {0}.', imported.join(', ')), true);
      loadSmime();
    }, function (e) {
      say('smime-status', tf('Could not import: {0}', e && e.message ? e.message : String(e)), false);
    });
  });
  // ---- OpenPGP ---------------------------------------------------------------
  // The cryptography is /portal-openpgp.js (window.hmOpenPgp): RFC 4880 version
  // 4 keys, signatures and messages, PGP/MIME and Autocrypt, on the Web Crypto
  // API. Unlike the S/MIME module, which the shell always carries, this one is
  // FETCHED ONLY WHEN SOMETHING NEEDS IT - a message that carries OpenPGP, the
  // Security page, a send that is to be sealed, or an account that has a key
  // and therefore wants an Autocrypt header on what it sends. It is the largest
  // script this page has, and most sessions never meet an OpenPGP message.
  //
  // This block keeps the key ring in step with the server (GET/PUT/DELETE
  // /api/v1/me/openpgp/keys...), opens the secret key with a passphrase that
  // never leaves the browser, and puts what the module finds on the page.
  var pgpModule = null;
  var pgpLoading = null;
  var pgpLoad = function () {
    if (window.hmOpenPgp) { pgpModule = window.hmOpenPgp; return Promise.resolve(pgpModule); }
    if (!pgpLoading) {
      pgpLoading = new Promise(function (resolve, reject) {
        var tag = document.createElement('script');
        tag.src = '/portal-openpgp.js';
        tag.addEventListener('load', function () {
          pgpModule = window.hmOpenPgp || null;
          if (pgpModule) { resolve(pgpModule); return; }
          pgpLoading = null;
          reject(new Error(t('The OpenPGP module did not load.')));
        });
        tag.addEventListener('error', function () { pgpLoading = null; reject(new Error(t('The OpenPGP module did not load.'))); });
        (document.head || document.body).appendChild(tag);
      });
    }
    return pgpLoading;
  };
  // Whether the page can do any of this: the module loaded, and the browser
  // gave it its cryptography. A page opened over plain HTTP is not a secure
  // context and the browser withholds crypto.subtle from it.
  var pgpUsable = function () { return !!(pgpModule && pgpModule.available !== false && window.isSecureContext !== false); };
  // parsed: fingerprint -> the public key the module read, so a key is read
  // once. unlocked: fingerprint -> the SECRET key, open, for as long as this
  // session lasts. autocrypt: address -> the header value for a message sent
  // from it. All three go at sign-out, with everything else.
  var pgpState = { own: [], peers: [], loaded: false, offered: false, limits: {}, lookup: {}, parsed: {}, unlocked: {}, autocrypt: {} };
  var pgpForget = function () {
    pgpSettle(false, '');
    pgpState = { own: [], peers: [], loaded: false, offered: false, limits: {}, lookup: {}, parsed: {}, unlocked: {}, autocrypt: {} };
    clear(el('pgp-own-rows'));
    clear(el('pgp-peer-rows'));
    el('pgp-own-table').hidden = true;
    el('pgp-peer-table').hidden = true;
    el('pgp-own-empty').hidden = false;
    el('pgp-peer-empty').hidden = false;
    el('pgp-unlock-passphrase').value = '';
    el('pgp-import-passphrase').value = '';
    el('pgp-import-file').value = '';
    el('pgp-make-passphrase').value = '';
    el('pgp-make-passphrase2').value = '';
    say('pgp-status', '', true);
    el('compose-pgp-sign-label').hidden = true;
    el('compose-pgp-encrypt-label').hidden = true;
    el('compose-pgp-note').hidden = true;
    el('message-pgp').hidden = true;
    el('message-pgp-unlock').hidden = true;
    el('message-pgp-keep').hidden = true;
    el('message-pgp-accept').hidden = true;
    el('message-pgp-text').textContent = '';
  };
  var pgpGroups = function (fingerprint) { return String(fingerprint || '').toUpperCase().replace(/(.{4})/g, '$1 ').trim(); };
  var pgpWhen = function (seconds) { return seconds ? new Date(seconds * 1000).toLocaleDateString() : t('Never'); };
  var pgpOwnFor = function (address) { return pgpState.own.filter(function (o) { return o.address === address && !o.revoked; })[0] || null; };
  // TRUST, as far as this page can see it. A correspondent's key that arrived
  // for an address already holding a DIFFERENT key is stored by the server as
  // a change (changed_from names the key it replaced) and is PENDING until the
  // person accepts it: not encrypted to, and its signatures not shown as good,
  // because the one thing a man in the middle needs is for a key to change in
  // silence. A key the person marked verified - its fingerprint read back to
  // its owner - shows the mark on every good signature from then on. pgpRowsFor
  // is every row an address has, in every state; pgpPeersFor is the ones a
  // message may be encrypted to.
  var pgpPending = function (row) { return !!(row && row.changed_from && !row.accepted); };
  var pgpRowsFor = function (address) { return pgpState.peers.filter(function (p) { return p.address === address; }); };
  var pgpPeersFor = function (address) { return pgpState.peers.filter(function (p) { return p.address === address && !p.revoked && !pgpPending(p); }); };
  var pgpPendingFor = function (address) { return pgpRowsFor(address).filter(function (p) { return !p.revoked && pgpPending(p); }); };
  // The ring's row for a key, by its PRIMARY fingerprint - the one the ring
  // files it under and the one a person reads back to its owner.
  var pgpRowOf = function (fingerprint) { return pgpState.peers.concat(pgpState.own).filter(function (r) { return r.fingerprint === fingerprint; })[0] || null; };
  var pgpChangeSentence = function (row) { return tf('This correspondent\'s key changed on {0}. It was {1}; it is now {2}.', pgpWhen(row.changed_at), pgpGroups(row.changed_from), pgpGroups(row.fingerprint)); };
  var pgpTrustCall = function (row, what, value) { return call('PUT', '/api/v1/me/openpgp/keys/' + row.id + '/' + what, what === 'verified' ? { verified: value } : { accepted: value }); };
  var pgpAllUnlocked = function () { return pgpState.own.length > 0 && pgpState.own.every(function (o) { return !!pgpState.unlocked[o.fingerprint]; }); };
  var pgpSave = function (text, name) {
    var link = document.createElement('a');
    link.href = URL.createObjectURL(new Blob([text], { type: 'application/pgp-keys' }));
    link.setAttribute('download', name);
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    setTimeout(function () { URL.revokeObjectURL(link.href); }, 10000);
  };
  // The unlock dialogue: in 'unlock' mode it opens every locked key of the
  // account with what is typed and resolves; in 'passphrase' mode it just hands
  // the text back, which is how an import asks what to store the key under.
  var pgpWaiters = [];
  var pgpAsk = function (title, note, mode) {
    el('pgp-unlock-title').textContent = title;
    el('pgp-unlock-note').textContent = note;
    el('pgp-unlock-form').setAttribute('data-ask', mode);
    say('pgp-unlock-status', '', true);
    el('pgp-unlock-passphrase').value = '';
    el('pgp-unlock').hidden = false;
    el('pgp-unlock-passphrase').focus();
    return new Promise(function (resolve, reject) { pgpWaiters.push({ resolve: resolve, reject: reject }); });
  };
  var pgpSettle = function (ok, value) {
    el('pgp-unlock').hidden = true;
    el('pgp-unlock-passphrase').value = '';
    pgpWaiters.splice(0).forEach(function (w) { if (ok) { w.resolve(value); } else { w.reject(value); } });
  };
  // Opens every own key that is still locked. Rejects with the reason when one
  // would not open; bound to the account's state at the start, since the
  // derivation takes long enough that a sign-out can happen under it.
  var pgpUnlockWith = function (passphrase) {
    var state = pgpState;
    var chain = Promise.resolve();
    var failed = '';
    state.own.forEach(function (row) {
      chain = chain.then(function () {
        if (state !== pgpState || state.unlocked[row.fingerprint] || !row.secret_key) { return null; }
        return pgpModule.readKey(row.secret_key, passphrase).then(function (key) {
          if (state !== pgpState) { return; }
          state.unlocked[row.fingerprint] = key;
        }, function (e) { failed = e && e.message ? e.message : String(e); });
      });
    });
    return chain.then(function () { if (failed) { throw new Error(failed); } });
  };
  var pgpUnlock = function () {
    if (pgpAllUnlocked()) { return Promise.resolve(''); }
    return pgpLoad().then(function () {
      return pgpAsk(t('Unlock your OpenPGP key'), t('The passphrase opens the key in this browser; nothing is sent to the server.'), 'unlock');
    });
  };
  el('pgp-unlock-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var passphrase = el('pgp-unlock-passphrase').value;
    if (!passphrase) { say('pgp-unlock-status', t('Enter the passphrase.'), false); return; }
    if (el('pgp-unlock-form').getAttribute('data-ask') === 'passphrase') { pgpSettle(true, passphrase); return; }
    pgpUnlockWith(passphrase).then(function () { pgpSettle(true, passphrase); }, function (e) {
      say('pgp-unlock-status', (e && e.message === 'wrong passphrase') ? t('Wrong passphrase. Nobody here can recover it: the key is encrypted where this server cannot see it.') : String(e && e.message || e), false);
    });
  });
  el('pgp-unlock-cancel').addEventListener('click', function () { pgpSettle(false, t('Not sent. The message is still here.')); });
  // One public key read once. A row whose key will not parse is left out of
  // what is handed to the module rather than breaking the whole read.
  var pgpParse = function (row) {
    if (pgpState.parsed[row.fingerprint]) { return Promise.resolve(pgpState.parsed[row.fingerprint]); }
    return pgpModule.readKey(pgpModule.b64decode(row.public_key)).then(function (key) {
      pgpState.parsed[row.fingerprint] = key;
      return key;
    }, function () { return null; });
  };
  // Every key this account has, the opened secret ones FIRST: a message
  // encrypted to a key is opened by whichever copy of that key the module
  // meets first, and only the opened one can open it.
  var pgpKeysInHand = function () {
    var chain = Promise.resolve();
    var out = [];
    pgpState.own.concat(pgpState.peers).forEach(function (row) {
      chain = chain.then(function () { return pgpParse(row); }).then(function (key) { if (key) { out.push(key); } });
    });
    return chain.then(function () {
      return Object.keys(pgpState.unlocked).map(function (k) { return pgpState.unlocked[k]; }).concat(out);
    });
  };
  // The Autocrypt header for each address the account has a key for, built once
  // the module is in hand. A message sent from an address with no key carries
  // no header, which is what Autocrypt means by having nothing to say.
  var pgpBuildAutocrypt = function () {
    var state = pgpState;
    var chain = Promise.resolve();
    state.own.forEach(function (row) {
      if (row.revoked) { return; }
      chain = chain.then(function () { return pgpParse(row); }).then(function (key) {
        if (!key || state !== pgpState) { return; }
        try { state.autocrypt[row.address] = pgpModule.autocryptHeader(row.address, key, true); } catch (e) { /* a key that will not describe itself carries no header */ }
      });
    });
    return chain;
  };
  var pgpAutocryptFor = function (address) { return pgpState.autocrypt[String(address || '').toLowerCase()] || ''; };
  var pgpRows = function (tbody, list, own) {
    clear(tbody);
    list.forEach(function (row) {
      var tr = document.createElement('tr');
      tr.appendChild(node('td', row.address + (row.name ? ' - ' + row.name : '')));
      var fingerprint = node('td', pgpGroups(row.fingerprint));
      if (!own) {
        // The three states a correspondent's key can be in - it replaced
        // another and waits to be accepted, it was verified in person, or
        // neither - named on the row as well as shown, so that a check can
        // read them whatever language the page is in.
        var trust = pgpPending(row) ? 'changed' : (row.verified ? 'verified' : 'plain');
        tr.setAttribute('data-pgp-trust', trust);
        if (trust === 'changed') { fingerprint.appendChild(node('div', pgpChangeSentence(row), 'badge warn')); }
        else if (trust === 'verified') { fingerprint.appendChild(node('div', '✓ ' + t('Verified in person'), 'badge good pgp-verified')); }
      }
      tr.appendChild(fingerprint);
      tr.appendChild(node('td', own ? (row.revoked ? t('Revoked') : pgpWhen(row.expires)) : (row.revoked ? t('Revoked') : row.source)));
      var td = document.createElement('td');
      var add = function (label, run) {
        var button = node('button', label, 'btn ghost sm');
        button.type = 'button';
        button.addEventListener('click', run);
        td.appendChild(button);
      };
      add(t('Copy the public key'), function () {
        pgpLoad().then(function () { return pgpParse(row); }).then(function (key) {
          if (!key) { say('pgp-status', t('That key could not be read.'), false); return; }
          return navigator.clipboard.writeText(pgpModule.exportPublic(key)).then(function () { say('pgp-status', t('The public key is on the clipboard.'), true); });
        }).catch(function (e) { say('pgp-status', String(e && e.message || e), false); });
      });
      if (!own && !row.revoked) {
        if (pgpPending(row)) {
          add(t('Accept the new key'), function () {
            pgpTrustCall(row, 'accepted', true).then(function (result) {
              if (result.status === 200) { loadPgpKeys(); return; }
              say('pgp-status', describe(result, t('Could not accept the key')), false);
            });
          });
        }
        add(row.verified ? t('Remove the verified mark') : t('Mark verified in person'), function () {
          pgpTrustCall(row, 'verified', !row.verified).then(function (result) {
            if (result.status === 200) { loadPgpKeys(); return; }
            say('pgp-status', describe(result, t('Could not mark the key')), false);
          });
        });
      }
      if (own) {
        add(t('Save the secret key'), function () {
          pgpSave(row.secret_key, row.address + '-secret.asc');
          say('pgp-status', t('The secret key was saved. It is still encrypted under its passphrase; keep the file and the passphrase apart.'), true);
        });
        add(t('Publish'), function () {
          call('PUT', '/api/v1/me/openpgp', { key: row.public_key, publish: true }).then(function (result) {
            say('pgp-status', (result.status === 200 || result.status === 201) ? t('The public key is published in this server\'s Web Key Directory.') : describe(result, t('Could not publish the key')), result.status === 200 || result.status === 201);
          });
        });
        if (!row.revoked) {
          add(t('Revoke'), function () {
            if (!window.confirm(t('Revoke this key? A revoked key signs and encrypts nothing, and every correspondent who fetches it is told so. It cannot be undone.'))) { return; }
            pgpRevoke(row);
          });
        }
      }
      add(t('Remove'), function () {
        if (own && !window.confirm(t('Remove this key from this server? If you have no copy of the secret key elsewhere, every message encrypted to it becomes unreadable for ever.'))) { return; }
        call('DELETE', '/api/v1/me/openpgp/keys/' + (own ? 'own' : 'correspondents') + '/' + row.fingerprint).then(function (result) {
          if (result.status === 200) { delete pgpState.unlocked[row.fingerprint]; delete pgpState.parsed[row.fingerprint]; loadPgpKeys(); return; }
          say('pgp-status', describe(result, t('Could not remove')), false);
        });
      });
      tr.appendChild(td);
      tbody.appendChild(tr);
    });
  };
  var pgpRenderPage = function () {
    el('pgp-off').hidden = pgpState.offered;
    el('pgp-body').hidden = !pgpState.offered;
    el('pgp-unavailable').hidden = window.isSecureContext !== false;
    pgpRows(el('pgp-own-rows'), pgpState.own, true);
    el('pgp-own-table').hidden = !pgpState.own.length;
    el('pgp-own-empty').hidden = !!pgpState.own.length;
    pgpRows(el('pgp-peer-rows'), pgpState.peers, false);
    el('pgp-peer-table').hidden = !pgpState.peers.length;
    el('pgp-peer-empty').hidden = !!pgpState.peers.length;
  };
  // The key ring, which is a plain JSON listing and costs nothing: asked for at
  // sign-in so the compose form knows what it may offer. The module itself is
  // fetched only when the account HAS a key, because only then is there
  // anything for it to do before a message arrives.
  var loadPgpKeys = function () {
    var state = pgpState;
    return call('GET', '/api/v1/me/openpgp/keys').then(function (result) {
      if (state !== pgpState) { return; }
      state.loaded = true;
      if (result.status === 403) { state.offered = false; pgpRenderPage(); return; }
      if (result.status !== 200 || !result.data) { return; }
      state.offered = true;
      state.own = result.data.own || [];
      state.peers = result.data.correspondents || [];
      state.limits = result.data.limits || {};
      state.lookup = result.data.lookup || {};
      pgpRenderPage();
      pgpComposeHints();
      if (!state.own.length) { return; }
      return pgpLoad().then(function () {
        if (state !== pgpState) { return; }
        el('pgp-curve-note').hidden = true;
        pgpModule.curve25519Available().then(function (have) { if (state === pgpState) { el('pgp-curve-note').hidden = !!have; } });
        return pgpBuildAutocrypt();
      }, function () { /* the page still works without it; a message that needs it says so */ });
    });
  };
  // The Security page asks for the module as well, since making or importing a
  // key is what that page is for.
  var loadPgpPage = function () {
    return loadPgpKeys().then(function () {
      if (!pgpState.offered) { return null; }
      return pgpLoad().then(function () {
        pgpModule.curve25519Available().then(function (have) { el('pgp-curve-note').hidden = !!have; });
      }, function (e) { say('pgp-status', String(e && e.message || e), false); });
    });
  };
  // Storing one key. own carries its secret half; a correspondent's does not.
  // The address is the key's own, or the one the caller names (an Autocrypt
  // header says whose key it carries; a lookup was for an address). A key of
  // the account's own falls back to the account; a correspondent's NEVER
  // does - a stranger's key with no address in its user id, filed under the
  // account's own address, would have every message the person sends sealed
  // to the stranger - so it goes to the server with none, and the server
  // refuses a correspondent's key it cannot file under an address.
  var pgpStore = function (key, secretArmored, source, address) {
    var body = {
      public_key: pgpModule.b64encode(pgpModule.publicBytes(key)),
      address: address || key.address || (secretArmored ? me : ''),
      name: key.name || '',
      source: source,
      revoked: !!key.revoked
    };
    if (secretArmored) { body.secret_key = secretArmored; }
    return call('PUT', '/api/v1/me/openpgp/keys/' + (secretArmored ? 'own' : 'correspondents'), body);
  };
  el('pgp-make-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var passphrase = el('pgp-make-passphrase').value;
    if (passphrase.length < 8) { say('pgp-status', t('The passphrase is at least eight characters.'), false); return; }
    if (passphrase !== el('pgp-make-passphrase2').value) { say('pgp-status', t('The two passphrases are not the same.'), false); return; }
    var address = String(el('pgp-make-address').value || me).trim().toLowerCase();
    var kind = el('pgp-make-kind').value;
    var spec = {
      type: kind === 'curve25519' ? 'curve25519' : 'rsa',
      bits: kind === 'rsa4096' ? 4096 : 3072,
      name: String(el('pgp-make-name').value || '').trim(),
      email: address,
      expiresIn: Number(el('pgp-make-expiry').value) || 0,
      passphrase: passphrase
    };
    say('pgp-status', t('Making the key in this browser. An RSA key takes a moment.'), true);
    pgpLoad().then(function () { return pgpModule.generateKey(spec); }).then(function (made) {
      return pgpStore(made.key, made.secretArmored, 'generated').then(function (result) {
        if (result.status !== 200 && result.status !== 201) { throw new Error(describe(result, t('Could not store the key'))); }
        pgpState.unlocked[made.fingerprint] = made.key;
        pgpSave(made.revocation, address + '-revocation.asc');
        el('pgp-make-passphrase').value = '';
        el('pgp-make-passphrase2').value = '';
        say('pgp-status', tf('The key {0} was made. Its revocation certificate has been saved to this device: keep it, because it is the only way to withdraw the key if the passphrase is ever lost.', pgpGroups(made.fingerprint)), true);
        return loadPgpKeys();
      });
    }).catch(function (e) { say('pgp-status', tf('The key could not be made: {0}', String(e && e.message || e)), false); });
  });
  el('pgp-import-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var file = (el('pgp-import-file').files || [])[0];
    if (!file) { say('pgp-status', t('Choose a file first.'), false); return; }
    var filePassphrase = el('pgp-import-passphrase').value;
    say('pgp-status', t('Importing...'), true);
    pgpLoad().then(function () { return file.text(); }).then(function (text) {
      if (/-----BEGIN PGP PUBLIC KEY BLOCK-----[\s\S]*-----BEGIN PGP SIGNATURE-----/.test(text) || /^\s*-----BEGIN PGP SIGNATURE-----/.test(text)) {
        return pgpImportRevocation(text);
      }
      return pgpModule.readKeys(text, filePassphrase).then(function (keys) {
        var secret = keys.filter(function (k) { return k.isSecret; });
        var pub = keys.filter(function (k) { return !k.isSecret; });
        var ask = secret.length
          ? pgpAsk(t('A passphrase for the key here'), t('The secret key is stored again under this passphrase, which never leaves the browser. Use the one your other keys here already use.'), 'passphrase')
          : Promise.resolve('');
        return ask.then(function (passphrase) {
          var chain = Promise.resolve();
          var done = [];
          var changes = [];
          secret.forEach(function (key) {
            chain = chain.then(function () {
              return pgpModule.exportSecret(key, passphrase).then(function (armoured) {
                return pgpStore(key, armoured, 'imported').then(function (result) {
                  if (result.status !== 200 && result.status !== 201) { throw new Error(describe(result, t('Could not store the key'))); }
                  pgpState.unlocked[key.fingerprint] = key;
                  done.push(key.address);
                });
              });
            });
          });
          pub.forEach(function (key) {
            chain = chain.then(function () {
              return pgpStore(key, '', 'imported').then(function (result) {
                if (result.status === 200 || result.status === 201) { done.push(key.address); }
                // A public key the server would not file - one with no address
                // in its user id, which this page never fills in with the
                // account's own - is said rather than silently left out.
                else if (result.status === 400) { changes.push(describe(result, t('Could not store the key'))); }
                // Imported over a different key already held for the address:
                // the server recorded the change, and the person is told here
                // rather than finding out when a send refuses.
                if (result.data && pgpPending(result.data)) { changes.push(pgpChangeSentence(result.data)); }
              });
            });
          });
          return chain.then(function () { return { done: done, changes: changes }; });
        });
      });
    }).then(function (imported) {
      el('pgp-import-file').value = '';
      el('pgp-import-passphrase').value = '';
      if (Array.isArray(imported)) { imported = { done: imported, changes: [] }; }
      if (imported) { say('pgp-status', tf('Imported {0}.', imported.done.join(', ')) + (imported.changes.length ? ' ' + imported.changes.join(' ') : ''), true); }
      return loadPgpKeys();
    }).catch(function (e) { say('pgp-status', tf('Could not import: {0}', String(e && e.message || e)), false); });
  });
  // A revocation certificate, applied to the key it revokes and stored again,
  // so that every correspondent who fetches the key is told.
  var pgpImportRevocation = function (text) {
    var chain = Promise.resolve();
    var done = [];
    pgpState.own.concat(pgpState.peers).forEach(function (row) {
      if (row.revoked) { return; }
      chain = chain.then(function () { return pgpParse(row); }).then(function (key) {
        if (!key) { return null; }
        return pgpModule.applyRevocation(key, text).then(function () {
          return pgpStore(key, row.secret_key || '', row.source).then(function () { done.push(row.address); });
        }, function () { /* not this key's revocation */ });
      });
    });
    return chain.then(function () {
      if (!done.length) { throw new Error(t('That revocation certificate does not belong to any key here.')); }
      return done;
    });
  };
  // Revoking: the certificate is made by the key itself, applied to it, and the
  // key stored again with the revocation on it, so that every correspondent who
  // fetches it afterwards is told. The certificate is saved to the device as
  // well, because whoever already has the key has to be given it.
  var pgpRevoke = function (row) {
    pgpLoad().then(pgpUnlock).then(function (passphrase) {
      var key = pgpState.unlocked[row.fingerprint];
      if (!key) { throw new Error(t('The key could not be opened.')); }
      return pgpModule.revocationCertificate(key, 1, 'Revoked in the webmail.').then(function (certificate) {
        return pgpModule.applyRevocation(key, certificate).then(function () {
          // The secret half carries the revocation too when the passphrase was
          // just typed; when the key was already open in this session there is
          // no passphrase to write it under, and the public half - which is
          // what anybody else ever fetches - carries it either way.
          return (passphrase ? pgpModule.exportSecret(key, passphrase) : Promise.resolve(row.secret_key)).then(function (secret) {
            return pgpStore(key, secret, row.source).then(function (result) {
              if (result.status !== 200 && result.status !== 201) { throw new Error(describe(result, t('Could not store the key'))); }
              pgpSave(certificate, row.address + '-revocation.asc');
              say('pgp-status', t('The key is revoked. Send the revocation certificate to anyone who has the key, and publish it if the key was published.'), true);
              delete pgpState.parsed[row.fingerprint];
              delete pgpState.unlocked[row.fingerprint];
              return loadPgpKeys();
            });
          });
        });
      });
    }).catch(function (e) { say('pgp-status', String(e && e.message || e), false); });
  };
  el('pgp-lookup-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var address = String(el('pgp-lookup-address').value || '').trim().toLowerCase();
    if (address.indexOf('@') < 1) { say('pgp-status', t('Type an e-mail address to look a key up for.'), false); return; }
    say('pgp-status', t('Looking...'), true);
    call('GET', '/api/v1/me/openpgp/lookup?address=' + encodeURIComponent(address)).then(function (result) {
      var d = result.data || {};
      if (result.status !== 200) { say('pgp-status', describe(result, t('Could not look the key up')), false); return; }
      if (!d.found) { say('pgp-status', tf('No key was found for {0}: {1}', address, d.error || ''), false); return; }
      if (!window.confirm(tf('A key for {0} was found at {1}. Its fingerprint is {2}. Read it back to its owner before you rely on it. Keep it?', address, d.source, pgpGroups(d.key.fingerprint)))) { say('pgp-status', '', true); return; }
      call('PUT', '/api/v1/me/openpgp/keys/correspondents', { public_key: d.key.public_key, address: address, name: d.key.name || '', source: d.source.indexOf('wkd') === 0 ? 'wkd' : 'keyserver' }).then(function (stored) {
        if (stored.status !== 200 && stored.status !== 201) { say('pgp-status', describe(stored, t('Could not store the key')), false); return; }
        el('pgp-lookup-address').value = '';
        // A lookup that answered a key other than the one held is a change,
        // which the server recorded and the list below shows with Accept.
        say('pgp-status', tf('The key of {0} was kept.', address) + (stored.data && pgpPending(stored.data) ? ' ' + pgpChangeSentence(stored.data) : ''), true);
        loadPgpKeys();
      });
    });
  });
  // ---- reading a message ----
  // Whether this message has anything for the module to do, asked without the
  // module: the content type, an Autocrypt header, or an inline block.
  var pgpNeeded = function (m) {
    var headers = String(m.headers || '');
    if (/application\/pgp-(encrypted|signature|keys)/i.test(headers)) { return true; }
    if (/^autocrypt:/im.test(headers)) { return true; }
    return /-----BEGIN PGP (MESSAGE|SIGNED MESSAGE|PUBLIC KEY BLOCK)-----/.test(String(m.text || ''));
  };
  // trust, when given, names the state the badge shows - changed, verified or
  // plain - on the element, for the same reason the key rows carry it.
  var pgpBadge = function (text, cls, trust) {
    var badge = node('span', text, 'badge pgp ' + cls);
    if (trust) { badge.setAttribute('data-pgp-trust', trust); }
    el('message-badges').appendChild(badge);
  };
  var pgpRun = 0;
  var pgpInspect = function (m) {
    el('message-pgp').hidden = true;
    el('message-pgp-unlock').hidden = true;
    el('message-pgp-keep').hidden = true;
    el('message-pgp-accept').hidden = true;
    pgpAcceptOffer = null;
    // Read again for the same message - after an unlock, after a key was
    // kept or accepted - the badges this put there last time come off first,
    // or a signature would be reported twice with two verdicts.
    var badges = el('message-badges');
    Array.prototype.slice.call(badges.children).forEach(function (badge) { if (/(^| )pgp( |$)/.test(String(badge.className))) { badges.removeChild(badge); } });
    if (!pgpNeeded(m)) { return; }
    if (window.isSecureContext === false) {
      el('message-pgp-text').textContent = t('OpenPGP is unavailable: the browser withholds its cryptography from a page that was not opened over HTTPS.');
      el('message-pgp').hidden = false;
      return;
    }
    var run = ++pgpRun;
    el('message-pgp-text').textContent = t('Reading the OpenPGP message...');
    el('message-pgp').hidden = false;
    var ready = pgpState.loaded ? Promise.resolve() : loadPgpKeys();
    ready.then(pgpLoad).then(pgpKeysInHand).then(function (keys) {
      return apiFetch('/api/v1/me/messages/' + m.id + '/source', { cache: 'no-store', credentials: 'same-origin' }).then(function (r) {
        return r.ok ? r.arrayBuffer() : Promise.reject(new Error('HTTP ' + r.status));
      }).then(function (buffer) { return pgpModule.readMessage(new Uint8Array(buffer), keys); });
    }).then(function (result) {
      if (run !== pgpRun || !current || current.id !== m.id) { return; }
      pgpShow(m, result);
    }, function (e) {
      if (run !== pgpRun) { return; }
      el('message-pgp-text').textContent = tf('Could not read it: {0}', String(e && e.message || e));
    });
  };
  // An Autocrypt header on a message from an address this account has no key
  // for is kept at once, which is what Autocrypt is: the key arrives with the
  // mail and nobody is asked anything. A header that would REPLACE a different
  // key already held for that address is not kept silently - a change of key is
  // the one thing a man in the middle needs to go unnoticed - so it waits
  // behind a button and says what it would do.
  var pgpAutocryptKeep = null;
  // The pending key the reader is being offered to accept, when the message
  // was signed by one or carries one.
  var pgpAcceptOffer = null;
  var pgpLearn = function (m, autocrypt, reported) {
    pgpAutocryptKeep = null;
    if (!autocrypt || autocrypt.address !== smimeAddress(m.from)) { return ''; }
    return pgpModule.readKey(autocrypt.keydata).then(function (key) {
      var held = pgpRowsFor(autocrypt.address).concat(pgpState.own.filter(function (o) { return o.address === autocrypt.address; }));
      var same = held.filter(function (row) { return row.fingerprint === key.fingerprint; })[0];
      if (same) {
        // Held already. A key kept but not yet accepted is said again here,
        // unless the signature on this message already said it.
        if (pgpPending(same) && same.fingerprint !== reported) { pgpAcceptOffer = same; el('message-pgp-accept').hidden = false; return pgpChangeSentence(same); }
        return '';
      }
      if (held.length) {
        pgpAutocryptKeep = { key: key, address: autocrypt.address };
        el('message-pgp-keep').hidden = false;
        return tf('This message carries a DIFFERENT OpenPGP key for {0}, fingerprint {1}. A key that changes without being asked about is what a man in the middle needs; check it with its owner before keeping it.', autocrypt.address, pgpGroups(key.fingerprint));
      }
      return pgpStore(key, '', 'autocrypt', autocrypt.address).then(function (result) {
        if (result.status !== 200 && result.status !== 201) { return ''; }
        loadPgpKeys();
        var learned = tf('The OpenPGP key of {0} was learned from this message.', autocrypt.address);
        // Held nothing for the address as far as this page knew, but the ring
        // knew better - a key stored in another tab, or the account's own -
        // and filed it as a change: said here, with Accept, as every other
        // path says it.
        if (result.data && pgpPending(result.data)) {
          pgpAcceptOffer = result.data;
          el('message-pgp-accept').hidden = false;
          return learned + ' ' + pgpChangeSentence(result.data);
        }
        return learned;
      });
    }, function () { return ''; });
  };
  el('message-pgp-keep').addEventListener('click', function () {
    if (!pgpAutocryptKeep) { return; }
    var keep = pgpAutocryptKeep;
    pgpAutocryptKeep = null;
    el('message-pgp-keep').hidden = true;
    pgpStore(keep.key, '', 'autocrypt', keep.address).then(function (result) {
      say('mail-status', (result.status === 200 || result.status === 201) ? tf('The key of {0} was kept.', keep.address) : describe(result, t('Could not store the key')), result.status === 200 || result.status === 201);
      return loadPgpKeys();
    }).then(function () {
      // Kept is not accepted: the server filed it as a change of the address's
      // key, and reading the message again shows both fingerprints and Accept.
      if (current) { pgpInspect(current); }
    });
  });
  el('message-pgp-accept').addEventListener('click', function () {
    if (!pgpAcceptOffer) { return; }
    var row = pgpAcceptOffer;
    pgpAcceptOffer = null;
    el('message-pgp-accept').hidden = true;
    pgpTrustCall(row, 'accepted', true).then(function (result) {
      say('mail-status', result.status === 200 ? tf('The key of {0} was accepted.', row.address) : describe(result, t('Could not accept the key')), result.status === 200);
      return loadPgpKeys();
    }).then(function () { if (current) { pgpInspect(current); } });
  });
  el('message-pgp-unlock').addEventListener('click', function () {
    pgpUnlock().then(function () { if (current) { pgpInspect(current); } }, function () {});
  });
  var pgpShow = function (m, result) {
    var lines = [];
    if (result.encrypted) {
      if (result.encrypted.ok) {
        lines.push(t('Encrypted with OpenPGP; opened in this browser. It can be searched by its headers only.'));
        pgpBadge(t('Encrypted'), 'good');
        if (result.integrityProtected === false) { lines.push(t('It carries no integrity check, which an OpenPGP message written since 2002 does: treat what it says with care.')); }
      }
      else if (pgpState.own.length && !pgpAllUnlocked()) {
        lines.push(t('Encrypted with OpenPGP. Unlock your key to read it.'));
        el('message-pgp-unlock').hidden = false;
        pgpBadge(t('Encrypted'), 'muted');
      }
      else {
        lines.push(tf('Encrypted with OpenPGP, and cannot be read here: {0}', result.encrypted.error));
        pgpBadge(t('Encrypted'), 'muted');
      }
    }
    if (result.signed) {
      var s = result.signed;
      var who = (s.key && s.key.address) || s.signerUserId || (s.keyId ? s.keyId.toUpperCase() : '');
      var from = smimeAddress(m.from);
      var carries = ((s.key && s.key.addresses) || []).indexOf(from) >= 0 || who === from;
      // The ring's row for the signing key, by its primary fingerprint;
      // s.fingerprint is the packet's, which may be a subkey's. Three states
      // for a signature that is good and the sender's: by a key that changed
      // and waits to be accepted, the warning that names both fingerprints
      // and nothing green; by a key verified in person, the mark; by any other
      // key, the plain verdict, with the fingerprint below it either way.
      var signer = s.key && s.key.fingerprint ? pgpRowOf(s.key.fingerprint) : null;
      if (!s.valid) { pgpBadge(tf('OpenPGP signature not verified: {0}', s.error), s.error && s.error.indexOf('no key here is') === 0 ? 'warn' : 'bad'); }
      else if (!carries) { pgpBadge(tf('Signed by {0}, not the sender', who), 'warn'); }
      else if (pgpPending(signer)) {
        pgpBadge(tf('Signed by {0}, but the key changed', who), 'warn', 'changed');
        lines.push(pgpChangeSentence(signer));
        pgpAcceptOffer = signer;
        el('message-pgp-accept').hidden = false;
      }
      else if (s.problem) { pgpBadge(tf('Signed by {0}, but {1}', who, s.problem), 'warn'); }
      else if (signer && signer.verified) { pgpBadge('✓ ' + tf('Signed by {0}, verified in person', who), 'good pgp-verified', 'verified'); }
      else { pgpBadge(tf('Signed by {0}', who), 'good', 'plain'); }
      if (s.valid && ((s.key && s.key.fingerprint) || s.fingerprint)) { lines.push(tf('The signing key is {0}.', pgpGroups((s.key && s.key.fingerprint) || s.fingerprint))); }
    }
    if (result.content && ((result.encrypted && result.encrypted.ok) || result.inline)) {
      var c = result.content;
      var text = c.text || (c.html ? (new DOMParser().parseFromString(c.html, 'text/html').body.textContent || '') : '');
      el('message-text').textContent = text || '(no text)';
      var attachments = el('message-attachments');
      clear(attachments);
      if (c.attachments.length) {
        attachments.appendChild(node('span', t('Attachments: ')));
        c.attachments.forEach(function (a, i) {
          var link = node('a', a.name + ' (' + a.type + ', ' + format(a.bytes.length) + ')');
          link.href = URL.createObjectURL(new Blob([a.bytes], { type: a.type }));
          link.setAttribute('download', a.name);
          if (i > 0) { attachments.appendChild(node('span', ', ')); }
          attachments.appendChild(link);
        });
      }
    }
    el('message-pgp-text').textContent = lines.join(' ');
    el('message-pgp').hidden = !lines.length;
    Promise.resolve(pgpLearn(m, result.autocrypt, pgpAcceptOffer ? pgpAcceptOffer.fingerprint : '')).then(function (learned) {
      if (!learned || !current || current.id !== m.id) { return; }
      el('message-pgp-text').textContent = (el('message-pgp-text').textContent + ' ' + learned).trim();
      el('message-pgp').hidden = false;
    });
  };
  // ---- writing a message ----
  var pgpComposeHints = function () {
    if (!pgpState.loaded || !pgpState.offered) { return; }
    var own = pgpOwnFor(smimeFromAddress());
    el('compose-pgp-sign-label').hidden = !own;
    el('compose-pgp-encrypt-label').hidden = !own && !pgpState.peers.length;
    var missing = [];
    var pending = [];
    var newer = [];
    smimeAddresses(el('compose-to').value + ',' + el('compose-cc').value + ',' + el('compose-bcc').value).forEach(function (a) {
      var usable = pgpPeersFor(a).length || pgpOwnFor(a);
      // A newer key waiting beside a usable one: the message goes to the
      // older key, and the person is told so rather than left to assume the
      // newest key is the one in use.
      if (pgpPendingFor(a).length && usable) { if (newer.indexOf(a) < 0) { newer.push(a); } return; }
      if (usable) { return; }
      var list = pgpPendingFor(a).length ? pending : missing;
      if (list.indexOf(a) < 0) { list.push(a); }
    });
    var hints = [];
    if (el('compose-pgp-encrypt').checked && missing.length) { hints.push(tf('Encrypt: no OpenPGP key for {0}.', missing.join(', '))); }
    if (el('compose-pgp-encrypt').checked && pending.length) { hints.push(tf('Encrypt: the key of {0} changed and has not been accepted; accept it under Security first.', pending.join(', '))); }
    if (el('compose-pgp-encrypt').checked && newer.length) { hints.push(tf('A newer key for {0} is waiting to be accepted; this is encrypted to the older one.', newer.join(', '))); }
    if (el('compose-pgp-sign').checked && !own) { hints.push(t('Sign: no OpenPGP key of yours for this From address.')); }
    el('compose-pgp-note').textContent = hints.join(' ');
    el('compose-pgp-note').hidden = !hints.length;
  };
  ['compose-to', 'compose-cc', 'compose-bcc'].forEach(function (id) { el(id).addEventListener('input', pgpComposeHints); });
  ['compose-from', 'compose-pgp-sign', 'compose-pgp-encrypt'].forEach(function (id) { el(id).addEventListener('change', pgpComposeHints); });
  // The body the form made, turned into the PGP/MIME entity the server sends as
  // it is: signed with the From address's key, encrypted to every recipient's
  // key and to the sender's own so the copy in Sent can still be read.
  var pgpPrepare = function (body) {
    if (!body.pgp_sign && !body.pgp_encrypt) { return Promise.resolve(body); }
    if (window.isSecureContext === false) { return Promise.reject(new Error(t('OpenPGP is unavailable: the browser withholds its cryptography from a page that was not opened over HTTPS.'))); }
    var from = body.from ? smimeAddress(body.from) : me;
    var own = pgpOwnFor(from);
    if (body.pgp_sign && !own) { return Promise.reject(new Error(t('Sign: no OpenPGP key of yours for this From address.'))); }
    return pgpLoad().then(function () {
      var recipients = [];
      var chain = Promise.resolve();
      if (body.pgp_encrypt) {
        var missing = [];
        var pending = [];
        smimeAddresses(body.to + ',' + body.cc + ',' + body.bcc).concat(own ? [from] : []).forEach(function (a) {
          // pgpPeersFor leaves a pending key out, so a key that changed and
          // was not accepted is never encrypted to - the send stops and says
          // why, rather than sealing the message to whoever sent the new key.
          var rows = pgpPeersFor(a).concat(pgpState.own.filter(function (o) { return o.address === a && !o.revoked; }));
          if (!rows.length) {
            if (a !== from) {
              var list = pgpPendingFor(a).length ? pending : missing;
              if (list.indexOf(a) < 0) { list.push(a); }
            }
            return;
          }
          rows.forEach(function (row) {
            chain = chain.then(function () { return pgpParse(row); }).then(function (key) { if (key && recipients.indexOf(key) < 0) { recipients.push(key); } });
          });
        });
        if (pending.length) { return Promise.reject(new Error(tf('Encrypt: the key of {0} changed and has not been accepted; accept it under Security first.', pending.join(', ')))); }
        if (missing.length) { return Promise.reject(new Error(tf('Encrypt: no OpenPGP key for {0}.', missing.join(', ')))); }
      }
      return chain.then(function () { return body.pgp_sign ? pgpUnlock() : null; }).then(function () {
        var signer = body.pgp_sign ? pgpState.unlocked[own.fingerprint] : null;
        if (body.pgp_sign && !signer) { throw new Error(t('Sign: the key could not be opened.')); }
        return pgpModule.composeMessage({ headers: {}, text: body.text || '', html: body.html || '', attachments: body.attachments || [], sign: signer, encrypt: body.pgp_encrypt ? recipients : null });
      });
    }).then(function (message) {
      var out = {};
      ['to', 'cc', 'bcc', 'from', 'subject', 'receipt', 'in_reply_to', 'references', 'answered_id'].forEach(function (k) { if (body[k] !== undefined && body[k] !== '') { out[k] = body[k]; } });
      // The module writes only MIME-Version above the entity; the server puts its own on.
      out.mime = message.replace(/^MIME-Version: 1\.0\r\n/, '');
      return out;
    });
  };
  // The last thing before a send: the Autocrypt header, on every message from
  // an address that has a key, sealed or not. The two flags this page used to
  // decide with are not the server's business and go here.
  var pgpStamp = function (body) {
    delete body.pgp_sign;
    delete body.pgp_encrypt;
    var value = pgpAutocryptFor(body.from ? smimeAddress(body.from) : me);
    if (value) { body.autocrypt = value; }
    return body;
  };
  el('message-unsubscribe').addEventListener('click', function () {
    if (!current) { return; }
    say('mail-status', t('Asking the list...'), true);
    call('POST', '/api/v1/me/messages/' + current.id + '/unsubscribe').then(function (result) {
      var d = result.data || {};
      if (result.status === 200 && d.method === 'one-click') { say('mail-status', d.ok ? t('Unsubscribed: the list answered ') + d.status + '.' : t('The list answered ') + d.status + t('; it may not have taken.'), !!d.ok); return; }
      if (result.status === 201 && d.method === 'mail') { say('mail-status', t('An unsubscribe request was sent to ') + d.to + '.', true); return; }
      say('mail-status', describe(result, t('Could not unsubscribe')), false);
    });
  });

  // ---- Rich text: a formatting bar over an editor whose HTML is rebuilt from
  // an allowed subset before it is sent, and a plain text made from the same
  // content, so the message goes as multipart/alternative.
  //
  // The subset: bold, italic, underline, breaks, paragraphs, lists, quotes,
  // links to http(s) and mailto, images the editor holds as data: URLs (the
  // send route moves each into a cid: part beside the HTML, InlineImageParts.cpp)
  // and tables - table, tr, td and th. Nothing is taken from an element but
  // its tag, a link's address and an image's data, so a table pasted in with
  // its styles, widths and handlers is sent as a table and nothing else. The
  // rebuild gives every table the border, padding and spacing that the
  // reader saw, as the HTML attributes every mail client honours - Outlook's
  // Word engine reads those where it ignores a stylesheet - and no cell may
  // span another: that is the simple table the roadmap asked for.
  var richOn = false;
  var ALLOWED = { B: 'b', STRONG: 'b', I: 'i', EM: 'i', U: 'u', BR: 'br', P: 'p', DIV: 'p', UL: 'ul', OL: 'ol', LI: 'li', BLOCKQUOTE: 'blockquote', A: 'a', IMG: 'img', H1: 'b', H2: 'b', H3: 'b', TABLE: 'table', THEAD: 'tbody', TBODY: 'tbody', TFOOT: 'tbody', TR: 'tr', TD: 'td', TH: 'th' };
  var TABLE_OPEN = '<table border="1" cellpadding="6" cellspacing="0" style="border-collapse:collapse">';
  var escapeHtml = function (s) { return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;'); };
  // The whole value, not just its beginning. This src is written into the
  // HTML the message carries and - since a draft reopens formatted - back
  // into this page's own document, so a value with a quote in it would carry
  // markup with it. What is admitted is what the server's own reader admits
  // (InlineImageParts::ParseDataUrl_): the base64 alphabet, and the spaces a
  // wrapped attribute has. The value is escaped again where it is written, so
  // that the encoder is right whatever the test admits.
  var isDataImage = function (src) { return /^data:image\/(png|jpeg|gif|webp);base64,[A-Za-z0-9+/=\s]*$/i.test(src) && src.length < 2000000; };
  var cleanNode = function (node) {
    if (node.nodeType === 3) { return escapeHtml(node.nodeValue); }
    if (node.nodeType !== 1) { return ''; }
    var tag = ALLOWED[node.tagName];
    var inner = '';
    for (var i = 0; i < node.childNodes.length; i++) { inner += cleanNode(node.childNodes[i]); }
    if (!tag) { return inner; }
    if (tag === 'br') { return '<br>'; }
    if (tag === 'img') {
      var src = String(node.getAttribute('src') || '');
      return isDataImage(src) ? '<img src="' + escapeHtml(src) + '" alt="' + escapeHtml(node.getAttribute('alt') || '') + '">' : '';
    }
    if (tag === 'a') {
      var href = String(node.getAttribute('href') || '');
      return /^(https?:|mailto:)/i.test(href) ? '<a href="' + escapeHtml(href) + '">' + inner + '</a>' : inner;
    }
    if (tag === 'p' && !inner) { return '<br>'; }
    if (tag === 'table') { return TABLE_OPEN + inner + '</table>'; }
    return '<' + tag + '>' + inner + '</' + tag + '>';
  };
  var cleanHtml = function (root) { var out = ''; for (var i = 0; i < root.childNodes.length; i++) { out += cleanNode(root.childNodes[i]); } return out; };
  // The text part: a table as lines of cells separated by tabs, an image as
  // its name in brackets, so a reader of the plain part knows what stood there.
  var plainOf = function (node) {
    if (node.nodeType === 3) { return node.nodeValue; }
    if (node.nodeType !== 1) { return ''; }
    if (node.tagName === 'BR') { return '\n'; }
    if (node.tagName === 'IMG') { var alt = String(node.getAttribute('alt') || '').trim(); return '[' + (alt || 'image') + ']'; }
    var text = '';
    for (var i = 0; i < node.childNodes.length; i++) { text += plainOf(node.childNodes[i]); }
    // A cell is one field of one row: a break inside it - a reader pressing
    // Enter in a cell - would otherwise split the row across two lines of the
    // plain part, where the tabs no longer line up with anything.
    if (node.tagName === 'TD' || node.tagName === 'TH') { return text.replace(/\n+$/, '').replace(/\n+/g, ' ') + '\t'; }
    if (node.tagName === 'TR') { return text.replace(/\t$/, '') + '\n'; }
    // A table begins on a line of its own, whatever stood before it, and ends
    // one, so that its rows read as rows in the plain part.
    if (node.tagName === 'TABLE') { return '\n' + text + '\n'; }
    if (/^(P|DIV|LI|BLOCKQUOTE|H1|H2|H3|UL|OL)$/.test(node.tagName)) { text = (node.tagName === 'BLOCKQUOTE' ? text.split('\n').map(function (l) { return '> ' + l; }).join('\n') : text) + '\n'; }
    return text;
  };
  var textToEditor = function (text) {
    var editor = el('compose-editor');
    while (editor.firstChild) { editor.removeChild(editor.firstChild); }
    String(text || '').split('\n').forEach(function (line, i) {
      if (i > 0) { editor.appendChild(document.createElement('br')); }
      editor.appendChild(node('span', line));
    });
  };
  // The elements of a kind under a node, in document order.
  var findAll = function (root, pred) { var out = []; (function walk(n) { for (var i = 0; i < n.childNodes.length; i++) { var c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
  var signatureBlock = function (editor) { return findAll(editor, function (n) { return n.getAttribute('data-sig') === '1'; })[0] || null; };
  var showRich = function (on) {
    richOn = on;
    el('compose-editor').hidden = !on; el('compose-text').hidden = on; el('rich-tools').hidden = !on;
    el('rich-toggle').textContent = on ? 'Plain text' : 'Formatting';
    if (!on) { el('emoji-panel').hidden = true; el('table-panel').hidden = true; el('emoji-open').setAttribute('aria-expanded', 'false'); el('table-open').setAttribute('aria-expanded', 'false'); }
  };
  var setRich = function (on) {
    var editor = el('compose-editor'), box = el('compose-text');
    if (on) {
      var text = box.value;
      if (signature.enabled && signature.html && signature.text) {
        var plainBlock = '\n\n-- \n' + signature.text;
        var at = text.lastIndexOf(plainBlock);
        if (at >= 0 && text.slice(at + plainBlock.length).trim() === '') { text = text.slice(0, at); }
      }
      textToEditor(text);
      if (signature.enabled && signature.html && !signatureBlock(editor)) {
        var sig = document.createElement('div'); sig.setAttribute('data-sig', '1'); sig.innerHTML = '<br>-- <br>' + signature.html;
        editor.appendChild(sig);
      }
      showRich(true);
    }
    else { box.value = plainOf(editor).replace(/\n+$/, ''); showRich(false); clear(editor); }
  };
  // Plain text has no pictures and no cells: the words of a table stay, as
  // lines, and an image becomes its name in brackets. Said before it happens.
  var holdsPictures = function (editor) { return findAll(editor, function (n) { return n.tagName === 'IMG' || n.tagName === 'TABLE'; }).length > 0; };
  el('rich-toggle').addEventListener('click', function () {
    if (richOn && holdsPictures(el('compose-editor')) && !window.confirm(t('Plain text keeps the words and drops the images and tables. Continue?'))) { return; }
    setRich(!richOn);
    if (richOn) { el('compose-editor').focus(); }
  });
  (function () {
    var tools = el('rich-tools').children;
    for (var i = 0; i < tools.length; i++) {
      (function (b) {
        // The button must not take the focus: Firefox collapses the editor's
        // selection when it does, and the command then has nothing to apply to.
        b.addEventListener('mousedown', function (event) { event.preventDefault(); });
        // Image and Table have handlers of their own, below.
        if (!b.getAttribute('data-cmd')) { return; }
        b.addEventListener('click', function () {
          var cmd = b.getAttribute('data-cmd'), arg = b.getAttribute('data-arg') || null;
          if (cmd === 'createLink') { arg = window.prompt ? window.prompt('The address to link to') : ''; if (!arg) { return; } }
          el('compose-editor').focus();
          try { document.execCommand(cmd, false, arg); } catch (e) { /* a browser without the command keeps the text */ }
        });
      })(tools[i]);
    }
  })();

  // ---- Where the reader is in the editor ------------------------------------
  // An emoji, an image or a table goes where the cursor was. The selection is
  // remembered whenever it is inside the editor, because a click on the
  // picker's search box or on a toolbar button takes it away in some
  // browsers, and the insertion has to go where the reader was writing, not
  // where the page happens to be focused. With no selection in the editor at
  // all - a window just opened, or a browser without the API - the thing
  // goes at the end of the text, above the signature.
  var savedRange = null;
  var currentCell = null;
  var cellOf = function (n) {
    var editor = el('compose-editor');
    while (n && n !== editor) {
      if (n.nodeType === 1 && (n.tagName === 'TD' || n.tagName === 'TH')) { return within(n, editor) ? n : null; }
      n = n.parentNode;
    }
    return null;
  };
  var liveRange = function () {
    var sel = window.getSelection ? window.getSelection() : null;
    if (!sel || !sel.rangeCount) { return null; }
    var range = sel.getRangeAt(0);
    return within(range.startContainer, el('compose-editor')) ? range : null;
  };
  var rememberRange = function () {
    var r = liveRange();
    if (r) { savedRange = r.cloneRange(); currentCell = cellOf(r.startContainer); }
  };
  var editorRange = function () {
    var live = liveRange();
    if (live) { return live; }
    return savedRange && within(savedRange.startContainer, el('compose-editor')) ? savedRange : null;
  };
  var selectRange = function (range) {
    var sel = window.getSelection ? window.getSelection() : null;
    if (!sel || !range) { return; }
    try { sel.removeAllRanges(); sel.addRange(range); savedRange = range.cloneRange(); } catch (e) { /* a range the document no longer holds */ }
  };
  var caretAfter = function (n) {
    if (!document.createRange) { return; }
    var after = document.createRange();
    after.setStartAfter(n); after.collapse(true);
    selectRange(after);
  };
  // Puts a node at the range - what the range covers goes - or, given none,
  // at the end of the text above the signature. Either way the cursor lands
  // after it.
  var insertInEditor = function (n, range) {
    var editor = el('compose-editor');
    if (range) {
      range.deleteContents();
      range.insertNode(n);
    } else {
      editor.insertBefore(n, signatureBlock(editor));
    }
    caretAfter(n);
  };
  // Into the plain box at the cursor, replacing what is selected.
  var insertInBox = function (text) {
    var box = el('compose-text');
    var start = typeof box.selectionStart === 'number' ? box.selectionStart : box.value.length;
    var end = typeof box.selectionEnd === 'number' ? box.selectionEnd : start;
    box.value = box.value.slice(0, start) + text + box.value.slice(end);
    var at = start + text.length;
    box.focus();
    if (box.setSelectionRange) { box.setSelectionRange(at, at); }
  };
  document.addEventListener('selectionchange', rememberRange);
  // A key typed into the editor is the message's. The page's shortcuts on the
  // document - j, k, e, Enter, ? and the rest - know a text field by its tag,
  // and this editor is a div; so a plain key stops here, and a key with a
  // modifier (Ctrl+K, the palette) or Escape goes on up as it always did.
  el('compose-editor').addEventListener('keydown', function (event) {
    if (!event.ctrlKey && !event.metaKey && !event.altKey && event.key !== 'Escape') { event.stopPropagation(); }
  });
  ['keyup', 'mouseup', 'click'].forEach(function (type) {
    el('compose-editor').addEventListener(type, function (event) {
      rememberRange();
      // A click names the cell it landed in, whether or not the browser has
      // moved the selection there yet.
      var hit = cellOf(event.target);
      if (hit) { currentCell = hit; }
    });
  });

  // ---- Emoji ------------------------------------------------------------------
  // A picker from a table in the page: no library, since the page's policy
  // runs no script but its own, and no service, since a message written here
  // reaches nobody before it is sent. Searched by name, in English - the
  // names are not in the catalogues, which would be a thousand more texts in
  // twenty-three languages for a picker that is also browsed - the last
  // thirty used kept in this browser first, and one picked goes where the
  // reader is in either editor. No skin tones and no flags: the table is what
  // people reach for in a message, not the standard.
  var EMOJI = [
    [K('Smileys'), '😀 grinning face|😃 grinning face big eyes|😄 grinning face smiling eyes|😁 beaming face|😆 grinning squinting laughing|😅 grinning face with sweat|🤣 rolling on the floor laughing|😂 face with tears of joy laughing|🙂 slightly smiling face|🙃 upside down face|😉 winking face|😊 smiling face blushing|😇 smiling face with halo innocent|🥰 smiling face with hearts love|😍 smiling face with heart eyes love|🤩 star struck|😘 face blowing a kiss|😗 kissing face|😚 kissing face closed eyes|😙 kissing face smiling eyes|😋 face savouring food yum|😛 face with tongue|😜 winking face with tongue|🤪 zany face crazy|😝 squinting face with tongue|🤑 money mouth face|🤗 hugging face|🤭 face with hand over mouth|🤫 shushing face quiet|🤔 thinking face|🤐 zipper mouth face|🤨 face with raised eyebrow|😐 neutral face|😑 expressionless face|😶 face without mouth|😏 smirking face|😒 unamused face|🙄 face with rolling eyes|😬 grimacing face|🤥 lying face|😌 relieved face|😔 pensive face sad|😪 sleepy face|🤤 drooling face|😴 sleeping face|😷 face with medical mask sick|🤒 face with thermometer sick|🤕 face with head bandage hurt|🤢 nauseated face|🤮 face vomiting|🤧 sneezing face|🥵 hot face|🥶 cold face freezing|🥴 woozy face|😵 dizzy face|🤯 exploding head mind blown|🤠 cowboy hat face|🥳 partying face celebration|😎 smiling face with sunglasses cool|🤓 nerd face|🧐 face with monocle|😕 confused face|😟 worried face|🙁 slightly frowning face|😮 face with open mouth surprised|😯 hushed face|😲 astonished face|😳 flushed face|🥺 pleading face|😦 frowning face with open mouth|😧 anguished face|😨 fearful face|😰 anxious face with sweat|😥 sad but relieved face|😢 crying face|😭 loudly crying face|😱 face screaming in fear|😖 confounded face|😣 persevering face|😞 disappointed face|😓 downcast face with sweat|😩 weary face|😫 tired face|🥱 yawning face|😤 face with steam from nose huffing|😡 pouting face angry red|😠 angry face|🤬 face with symbols on mouth swearing|😈 smiling face with horns devil|👿 angry face with horns devil|💀 skull|💩 pile of poo|🤡 clown face|👹 ogre|👺 goblin|👻 ghost|👽 alien|🤖 robot|😺 grinning cat|😸 grinning cat with smiling eyes|😹 cat with tears of joy|😻 smiling cat with heart eyes|😼 cat with wry smile|😽 kissing cat|🙀 weary cat|😿 crying cat|😾 pouting cat|🙈 see no evil monkey|🙉 hear no evil monkey|🙊 speak no evil monkey|💋 kiss mark|💌 love letter|💘 heart with arrow|💝 heart with ribbon|💖 sparkling heart|💗 growing heart|💓 beating heart|💞 revolving hearts|💕 two hearts|💟 heart decoration|💔 broken heart|❤️ red heart love|🧡 orange heart|💛 yellow heart|💚 green heart|💙 blue heart|💜 purple heart|🖤 black heart|🤍 white heart|🤎 brown heart|💯 hundred points|💢 anger symbol|💥 collision boom|💫 dizzy star|💦 sweat droplets|💨 dashing away|💣 bomb|💬 speech balloon|💭 thought balloon|💤 zzz sleeping'],
    [K('People'), '👋 waving hand hello bye|🤚 raised back of hand|✋ raised hand stop|🖖 vulcan salute|👌 ok hand|🤏 pinching hand small|✌️ victory hand peace|🤞 crossed fingers luck|🤟 love you gesture|🤘 sign of the horns rock|🤙 call me hand|👈 backhand index pointing left|👉 backhand index pointing right|👆 backhand index pointing up|👇 backhand index pointing down|☝️ index pointing up|👍 thumbs up yes like|👎 thumbs down no dislike|✊ raised fist|👊 oncoming fist bump|🤛 left facing fist|🤜 right facing fist|👏 clapping hands applause|🙌 raising hands hooray|👐 open hands|🤲 palms up together|🤝 handshake deal|🙏 folded hands please thanks pray|✍️ writing hand|💅 nail polish|🤳 selfie|💪 flexed biceps strong|🦵 leg|🦶 foot|👂 ear|👃 nose|🧠 brain|🦷 tooth|🦴 bone|👀 eyes|👅 tongue|👄 mouth lips|👶 baby|🧒 child|👦 boy|👧 girl|🧑 person|👱 person blond hair|👨 man|🧔 person beard|👩 woman|🧓 older person|👴 old man|👵 old woman|🙍 person frowning|🙎 person pouting|🙅 person gesturing no|🙆 person gesturing ok|💁 person tipping hand|🙋 person raising hand|🧏 deaf person|🙇 person bowing|🤦 person facepalming|🤷 person shrugging|👮 police officer|💂 guard|👷 construction worker|🤴 prince|👸 princess|👳 person wearing turban|🧕 woman with headscarf|🤵 person in tuxedo|👰 person with veil bride|🤰 pregnant woman|🤱 breast feeding|👼 baby angel|🎅 santa claus christmas|🤶 mrs claus christmas|🦸 superhero|🦹 supervillain|🧙 mage wizard|🧚 fairy|🧛 vampire|🧜 merperson|🧝 elf|🧞 genie|🧟 zombie|💆 person getting massage|💇 person getting haircut|🚶 person walking|🏃 person running|💃 woman dancing|🕺 man dancing|👯 people with bunny ears|🧖 person in steamy room sauna|🧗 person climbing|🏇 horse racing|🏂 snowboarder|🏄 person surfing|🚣 person rowing boat|🏊 person swimming|🚴 person biking cycling|🚵 person mountain biking|🤸 person cartwheeling|🤼 people wrestling|🤽 person playing water polo|🤾 person playing handball|🤹 person juggling|🧘 person in lotus position yoga|🛀 person taking bath|🛌 person in bed|👭 women holding hands|👫 woman and man holding hands couple|👬 men holding hands|💏 kiss couple|💑 couple with heart|👪 family|👤 bust in silhouette|👥 busts in silhouette|👣 footprints'],
    [K('Animals and nature'), '🐵 monkey face|🐒 monkey|🦍 gorilla|🐶 dog face|🐕 dog|🐩 poodle|🐺 wolf|🦊 fox|🦝 raccoon|🐱 cat face|🐈 cat|🦁 lion|🐯 tiger face|🐅 tiger|🐆 leopard|🐴 horse face|🐎 horse|🦄 unicorn|🦓 zebra|🦌 deer|🐮 cow face|🐂 ox|🐃 water buffalo|🐄 cow|🐷 pig face|🐖 pig|🐗 boar|🐽 pig nose|🐏 ram|🐑 ewe sheep|🐐 goat|🐪 camel|🐫 two hump camel|🦙 llama|🦒 giraffe|🐘 elephant|🦏 rhinoceros|🦛 hippopotamus|🐭 mouse face|🐁 mouse|🐀 rat|🐹 hamster|🐰 rabbit face|🐇 rabbit bunny|🦔 hedgehog|🦇 bat|🐻 bear|🐨 koala|🐼 panda|🦘 kangaroo|🦡 badger|🐾 paw prints|🦃 turkey|🐔 chicken|🐓 rooster|🐣 hatching chick|🐤 baby chick|🐥 front facing baby chick|🐦 bird|🐧 penguin|🦅 eagle|🦆 duck|🦢 swan|🦉 owl|🦚 peacock|🦜 parrot|🐸 frog|🐊 crocodile|🐢 turtle|🦎 lizard|🐍 snake|🐲 dragon face|🐉 dragon|🦕 sauropod dinosaur|🦖 t rex dinosaur|🐳 spouting whale|🐋 whale|🐬 dolphin|🐟 fish|🐠 tropical fish|🐡 blowfish|🦈 shark|🐙 octopus|🐚 spiral shell|🐌 snail|🦋 butterfly|🐛 bug caterpillar|🐜 ant|🐝 honeybee bee|🐞 lady beetle ladybird|🦗 cricket|🦂 scorpion|🦟 mosquito|🦠 microbe germ virus|💐 bouquet flowers|🌸 cherry blossom|💮 white flower|🌹 rose|🥀 wilted flower|🌺 hibiscus|🌻 sunflower|🌼 blossom|🌷 tulip|🌱 seedling|🌲 evergreen tree|🌳 deciduous tree|🌴 palm tree|🌵 cactus|🌾 sheaf of rice|🌿 herb|☘️ shamrock|🍀 four leaf clover luck|🍁 maple leaf|🍂 fallen leaf autumn|🍃 leaf fluttering in wind|🌍 globe europe africa earth|🌎 globe americas earth|🌏 globe asia australia earth|🌐 globe with meridians|🌑 new moon|🌓 first quarter moon|🌕 full moon|🌙 crescent moon|🌚 new moon face|🌝 full moon face|🌞 sun with face|⭐ star|🌟 glowing star|🌠 shooting star|☀️ sun|⛅ sun behind cloud|☁️ cloud|❄️ snowflake|⛄ snowman|🌀 cyclone|🌈 rainbow|🌂 closed umbrella|☔ umbrella with rain drops|⚡ high voltage lightning|🔥 fire|💧 droplet|🌊 water wave ocean'],
    [K('Food and drink'), '🍇 grapes|🍈 melon|🍉 watermelon|🍊 tangerine orange|🍋 lemon|🍌 banana|🍍 pineapple|🥭 mango|🍎 red apple|🍏 green apple|🍐 pear|🍑 peach|🍒 cherries|🍓 strawberry|🥝 kiwi fruit|🍅 tomato|🥥 coconut|🥑 avocado|🍆 aubergine eggplant|🥔 potato|🥕 carrot|🌽 ear of corn|🥒 cucumber|🥬 leafy green|🥦 broccoli|🍄 mushroom|🥜 peanuts|🌰 chestnut|🍞 bread|🥐 croissant|🥖 baguette bread|🥨 pretzel|🥯 bagel|🥞 pancakes|🧀 cheese wedge|🍖 meat on bone|🍗 poultry leg chicken|🥩 cut of meat steak|🥓 bacon|🍔 hamburger burger|🍟 french fries chips|🍕 pizza|🌭 hot dog|🥪 sandwich|🌮 taco|🌯 burrito|🥙 stuffed flatbread|🧆 falafel|🥚 egg|🍳 cooking fried egg|🥘 shallow pan of food paella|🍲 pot of food stew|🥣 bowl with spoon|🥗 green salad|🍿 popcorn|🧈 butter|🧂 salt|🥫 canned food|🍱 bento box|🍘 rice cracker|🍙 rice ball|🍚 cooked rice|🍛 curry rice|🍜 steaming bowl noodles ramen|🍝 spaghetti pasta|🍠 roasted sweet potato|🍢 oden|🍣 sushi|🍤 fried shrimp|🍥 fish cake with swirl|🥮 moon cake|🍡 dango|🥟 dumpling|🥠 fortune cookie|🥡 takeout box|🦀 crab|🦞 lobster|🦐 shrimp|🦑 squid|🦪 oyster|🍦 soft ice cream|🍧 shaved ice|🍨 ice cream|🍩 doughnut donut|🍪 cookie biscuit|🎂 birthday cake|🍰 shortcake cake slice|🧁 cupcake|🥧 pie|🍫 chocolate bar|🍬 candy sweet|🍭 lollipop|🍮 custard|🍯 honey pot|🍼 baby bottle|🥛 glass of milk|☕ hot beverage coffee tea|🍵 teacup without handle green tea|🍶 sake|🍾 bottle with popping cork champagne|🍷 wine glass|🍸 cocktail glass|🍹 tropical drink|🍺 beer mug|🍻 clinking beer mugs cheers|🥂 clinking glasses cheers|🥃 tumbler glass whisky|🥤 cup with straw|🧃 beverage box juice|🧉 mate|🧊 ice cube|🥢 chopsticks|🍴 fork and knife|🥄 spoon'],
    [K('Activities'), '🎃 jack o lantern halloween pumpkin|🎄 christmas tree|🎆 fireworks|🎇 sparkler|🧨 firecracker|✨ sparkles|🎈 balloon|🎉 party popper celebration congratulations|🎊 confetti ball|🎋 tanabata tree|🎍 pine decoration|🎎 japanese dolls|🎏 carp streamer|🎐 wind chime|🎑 moon viewing ceremony|🧧 red envelope|🎀 ribbon bow|🎁 wrapped gift present|🎫 ticket|🏆 trophy winner|🏅 sports medal|🥇 first place gold medal|🥈 second place silver medal|🥉 third place bronze medal|⚽ soccer ball football|⚾ baseball|🥎 softball|🏀 basketball|🏐 volleyball|🏈 american football|🏉 rugby football|🎾 tennis|🥏 flying disc frisbee|🎳 bowling|🏏 cricket game|🏑 field hockey|🏒 ice hockey|🥍 lacrosse|🏓 ping pong table tennis|🏸 badminton|🥊 boxing glove|🥋 martial arts uniform|🥅 goal net|⛳ flag in hole golf|🎣 fishing pole|🤿 diving mask|🎽 running shirt|🎿 skis|🛷 sled|🥌 curling stone|🎯 bullseye direct hit target|🪀 yo yo|🪁 kite|🎱 pool 8 ball billiards|🔮 crystal ball|🧿 nazar amulet|🎮 video game controller|🎰 slot machine|🎲 game die dice|🧩 puzzle piece|🧸 teddy bear|🎭 performing arts theatre masks|🎨 artist palette art|🧵 thread|🧶 yarn|🎼 musical score|🎵 musical note|🎶 musical notes music|🎤 microphone|🎧 headphone|🎷 saxophone|🎸 guitar|🎹 musical keyboard piano|🎺 trumpet|🎻 violin|🥁 drum|🎬 clapper board movie film'],
    [K('Travel and places'), '🚗 automobile car|🚕 taxi|🚙 sport utility vehicle|🚌 bus|🚎 trolleybus|🚓 police car|🚑 ambulance|🚒 fire engine|🚐 minibus|🚚 delivery truck|🚛 articulated lorry|🚜 tractor|🛵 motor scooter|🛺 auto rickshaw|🚲 bicycle bike|🛴 kick scooter|🛹 skateboard|🚏 bus stop|⛽ fuel pump petrol|🚨 police car light siren|🚥 horizontal traffic light|🚧 construction|⚓ anchor|⛵ sailboat|🛶 canoe|🚤 speedboat|🚢 ship|✈️ airplane flight|🛫 airplane departure|🛬 airplane arrival|🪂 parachute|💺 seat|🚁 helicopter|🚟 suspension railway|🚠 mountain cableway|🚡 aerial tramway|🚀 rocket launch|🛸 flying saucer ufo|🚂 locomotive steam train|🚃 railway car|🚄 high speed train|🚅 bullet train|🚆 train|🚇 metro underground subway|🚈 light rail|🚉 station|🚊 tram|🚝 monorail|🚞 mountain railway|🚋 tram car|🌋 volcano|🗻 mount fuji|🧱 brick|🏠 house home|🏡 house with garden|🏢 office building|🏣 japanese post office|🏤 post office|🏥 hospital|🏦 bank|🏨 hotel|🏩 love hotel|🏪 convenience store|🏫 school|🏬 department store|🏭 factory|🏯 japanese castle|🏰 castle|💒 wedding|🗼 tokyo tower|🗽 statue of liberty|⛪ church|🕌 mosque|🛕 hindu temple|🕍 synagogue|🕋 kaaba|⛲ fountain|⛺ tent camping|🌁 foggy|🌃 night with stars|🌄 sunrise over mountains|🌅 sunrise|🌆 cityscape at dusk|🌇 sunset|🌉 bridge at night|🎠 carousel horse|🎡 ferris wheel|🎢 roller coaster|🎪 circus tent|🧭 compass|🧳 luggage suitcase travel|⌛ hourglass done|⏳ hourglass not done|⌚ watch|⏰ alarm clock|🕐 one o clock|🕑 two o clock|🕒 three o clock|🕓 four o clock|🕔 five o clock|🕕 six o clock|🕖 seven o clock|🕗 eight o clock|🕘 nine o clock|🕙 ten o clock|🕚 eleven o clock|🕛 twelve o clock'],
    [K('Objects'), '📱 mobile phone|📲 mobile phone with arrow|☎️ telephone|📞 telephone receiver|📟 pager|📠 fax machine|🔋 battery|🔌 electric plug|💻 laptop computer|⌨️ keyboard|💽 computer disk|💾 floppy disk save|💿 optical disk cd|📀 dvd|🧮 abacus|🎥 movie camera|📺 television|📷 camera|📸 camera with flash|📹 video camera|📼 videocassette|🔍 magnifying glass tilted left search|🔎 magnifying glass tilted right search|💡 light bulb idea|🔦 flashlight torch|🏮 red paper lantern|🪔 diya lamp|📔 notebook with decorative cover|📕 closed book|📖 open book|📗 green book|📘 blue book|📙 orange book|📚 books|📓 notebook|📒 ledger|📃 page with curl|📜 scroll|📄 page facing up document|📰 newspaper|📑 bookmark tabs|🔖 bookmark|💰 money bag|💴 yen banknote|💵 dollar banknote|💶 euro banknote|💷 pound banknote|💸 money with wings|💳 credit card|🧾 receipt|💹 chart increasing with yen|✉️ envelope mail|📧 e mail|📨 incoming envelope|📩 envelope with arrow|📤 outbox tray|📥 inbox tray|📦 package parcel|📫 closed mailbox with raised flag|📮 postbox|✏️ pencil|✒️ black nib pen|📝 memo note|💼 briefcase|📁 file folder|📂 open file folder|📅 calendar|📆 tear off calendar|📇 card index|📈 chart increasing|📉 chart decreasing|📊 bar chart|📋 clipboard|📌 pushpin|📍 round pushpin location|📎 paperclip|📏 straight ruler|📐 triangular ruler|✂️ scissors|🔒 locked|🔓 unlocked|🔏 locked with pen|🔐 locked with key|🔑 key|🔨 hammer|🪓 axe|🏹 bow and arrow|🔧 wrench spanner|🔩 nut and bolt|⚙️ gear settings|⚖️ balance scale|🔗 link|🧰 toolbox|🧲 magnet|🧪 test tube|🧫 petri dish|🧬 dna|🔬 microscope|🔭 telescope|📡 satellite antenna|💉 syringe|🩸 drop of blood|💊 pill|🩹 adhesive bandage plaster|🩺 stethoscope|🚪 door|🪑 chair|🚽 toilet|🚿 shower|🛁 bathtub|🪒 razor|🧴 lotion bottle|🧷 safety pin|🧹 broom|🧺 basket|🧻 roll of paper|🧼 soap|🧽 sponge|🧯 fire extinguisher|🛒 shopping cart trolley|🗿 moai statue|🎓 graduation cap|👑 crown|🎩 top hat|👒 woman s hat|🧢 billed cap|📿 prayer beads|💄 lipstick|💍 ring|💎 gem stone diamond|👓 glasses|🥽 goggles|🥼 lab coat|🦺 safety vest|👔 necktie|👕 t shirt|👖 jeans|🧣 scarf|🧤 gloves|🧥 coat|🧦 socks|👗 dress|👘 kimono|🥻 sari|🩱 one piece swimsuit|🩲 briefs|🩳 shorts|👙 bikini|👚 woman s clothes|👛 purse|👜 handbag|👝 clutch bag|🎒 backpack|👞 man s shoe|👟 running shoe trainer|🥾 hiking boot|🥿 flat shoe|👠 high heeled shoe|👡 woman s sandal|🩰 ballet shoes|👢 woman s boot|🔔 bell|🔕 bell with slash muted|📢 loudspeaker|📣 megaphone|📯 postal horn|🔈 speaker low volume|🔉 speaker medium volume|🔊 speaker high volume|🔇 muted speaker'],
    [K('Symbols'), '✅ check mark button done|☑️ check box with check|✔️ check mark tick|❌ cross mark|❎ cross mark button|➕ plus|➖ minus|➗ divide|✖️ multiply|❓ question mark|❔ white question mark|❕ white exclamation mark|❗ exclamation mark|⚠️ warning|🚸 children crossing|⛔ no entry|🚫 prohibited|🚭 no smoking|🚯 no littering|🚱 non potable water|🚳 no bicycles|🚷 no pedestrians|📵 no mobile phones|🔞 no one under eighteen|☢️ radioactive|☣️ biohazard|⬆️ up arrow|↗️ up right arrow|➡️ right arrow|↘️ down right arrow|⬇️ down arrow|↙️ down left arrow|⬅️ left arrow|↖️ up left arrow|↕️ up down arrow|↔️ left right arrow|↩️ right arrow curving left|↪️ left arrow curving right|⤴️ right arrow curving up|⤵️ right arrow curving down|🔃 clockwise arrows|🔄 counterclockwise arrows refresh|🔙 back arrow|🔚 end arrow|🔛 on arrow|🔜 soon arrow|🔝 top arrow|🛐 place of worship|⚛️ atom symbol|✡️ star of david|☸️ wheel of dharma|☯️ yin yang|✝️ latin cross|☦️ orthodox cross|☪️ star and crescent|☮️ peace symbol|🕎 menorah|🔯 dotted six pointed star|♈ aries|♉ taurus|♊ gemini|♋ cancer|♌ leo|♍ virgo|♎ libra|♏ scorpio|♐ sagittarius|♑ capricorn|♒ aquarius|♓ pisces|⛎ ophiuchus|🔀 shuffle tracks|🔁 repeat|🔂 repeat single|▶️ play|⏩ fast forward|◀️ reverse|⏪ fast reverse rewind|⏫ fast up|⏬ fast down|🎦 cinema|🔅 dim|🔆 bright|📶 antenna bars signal|📳 vibration mode|📴 mobile phone off|♀️ female sign|♂️ male sign|♻️ recycling symbol|⚜️ fleur de lis|🔱 trident emblem|📛 name badge|🔰 japanese symbol for beginner|⭕ hollow red circle|✳️ eight spoked asterisk|✴️ eight pointed star|❇️ sparkle|©️ copyright|®️ registered|™️ trade mark|🔟 keycap ten|🔠 input latin uppercase|🔡 input latin lowercase|🔢 input numbers|🔣 input symbols|🔤 input latin letters|🆎 ab button blood type|🆑 cl button|🆒 cool button|🆓 free button|ℹ️ information|🆔 id button|🆕 new button|🆖 ng button|🆗 ok button|🆘 sos button help|🆙 up button|🆚 vs button|🔴 red circle|🟠 orange circle|🟡 yellow circle|🟢 green circle|🔵 blue circle|🟣 purple circle|🟤 brown circle|⚫ black circle|⚪ white circle|🟥 red square|🟧 orange square|🟨 yellow square|🟩 green square|🟦 blue square|🟪 purple square|🟫 brown square|⬛ black large square|⬜ white large square|🔶 large orange diamond|🔷 large blue diamond|🔸 small orange diamond|🔹 small blue diamond|🔺 red triangle pointed up|🔻 red triangle pointed down|💠 diamond with a dot|🔘 radio button|🔳 white square button|🔲 black square button|🏁 chequered flag finish|🚩 triangular flag|🎌 crossed flags|🏴 black flag|🏳️ white flag']
  ];
  var EMOJI_RECENT = 'hmPortalEmoji';
  var EMOJI_RECENT_MOST = 30;
  var emojiRecentRead = function () {
    try {
      var list = JSON.parse(localStorage.getItem(EMOJI_RECENT) || '[]');
      return Array.isArray(list) ? list.filter(function (s) { return typeof s === 'string' && s; }).slice(0, EMOJI_RECENT_MOST) : [];
    } catch (e) { return []; }
  };
  var emojiRecentWrite = function (list) {
    try { if (list.length) { localStorage.setItem(EMOJI_RECENT, JSON.stringify(list)); } else { localStorage.removeItem(EMOJI_RECENT); } } catch (e) { /* a browser that keeps nothing */ }
  };
  var emojiForget = function () { emojiRecentWrite([]); };
  var emojiEntries = null;
  var emojiAll = function () {
    if (emojiEntries) { return emojiEntries; }
    emojiEntries = [];
    EMOJI.forEach(function (group) {
      group[1].split('|').forEach(function (entry) {
        var at = entry.indexOf(' ');
        emojiEntries.push({ ch: entry.slice(0, at), name: entry.slice(at + 1), group: group[0] });
      });
    });
    return emojiEntries;
  };
  var emojiNameOf = function (ch) { var hit = emojiAll().filter(function (e) { return e.ch === ch; })[0]; return hit ? hit.name : ''; };
  var emojiRender = function (query) {
    var grid = el('emoji-grid');
    clear(grid);
    var words = String(query || '').trim().toLowerCase().split(/\s+/).filter(Boolean);
    var section = function (title, list) {
      if (!list.length) { return; }
      if (title) { grid.appendChild(node('div', title, 'emoji-head')); }
      var row = node('div', undefined, 'emoji-row');
      list.forEach(function (e) {
        var b = node('button', e.ch, 'emoji');
        b.type = 'button';
        b.setAttribute('title', e.name);
        b.setAttribute('aria-label', e.name);
        b.setAttribute('data-emoji', e.ch);
        row.appendChild(b);
      });
      grid.appendChild(row);
    };
    var all = emojiAll();
    if (!words.length) {
      section(t('Recently used'), emojiRecentRead().map(function (ch) { return { ch: ch, name: emojiNameOf(ch) }; }));
      EMOJI.forEach(function (group) { section(t(group[0]), all.filter(function (e) { return e.group === group[0]; })); });
      return;
    }
    var found = all.filter(function (e) { return words.every(function (w) { return e.name.indexOf(w) >= 0; }); });
    if (found.length) { section('', found); } else { grid.appendChild(node('div', t('No emoji matches.'), 'emoji-none')); }
  };
  var insertEmoji = function (ch) {
    var recent = emojiRecentRead().filter(function (s) { return s !== ch; });
    recent.unshift(ch);
    emojiRecentWrite(recent.slice(0, EMOJI_RECENT_MOST));
    if (!richOn) { insertInBox(ch); return; }
    // Through the editing command where the reader has a place in the
    // editor, so that the browser's own undo takes it back; a text node
    // otherwise.
    var editor = el('compose-editor');
    var range = editorRange();
    editor.focus();
    var done = false;
    if (range) {
      selectRange(range);
      try { done = !!document.execCommand('insertText', false, ch); } catch (e) { done = false; }
    }
    if (!done) { insertInEditor(document.createTextNode(ch), range); }
  };
  var emojiClose = function () { el('emoji-panel').hidden = true; el('emoji-open').setAttribute('aria-expanded', 'false'); };
  var emojiToggle = function () {
    var panel = el('emoji-panel');
    var open = panel.hidden;
    tableClose();
    if (!open) { emojiClose(); return; }
    el('emoji-search').value = '';
    emojiRender('');
    panel.hidden = false;
    el('emoji-open').setAttribute('aria-expanded', 'true');
    el('emoji-search').focus();
  };
  el('emoji-open').addEventListener('mousedown', function (event) { event.preventDefault(); });
  el('emoji-open').addEventListener('click', function (event) { event.stopPropagation(); emojiToggle(); });
  el('emoji-search').addEventListener('input', function () { emojiRender(el('emoji-search').value); });
  el('emoji-search').addEventListener('keydown', function (event) {
    if (event.key === 'Escape') { event.preventDefault(); emojiClose(); (richOn ? el('compose-editor') : el('compose-text')).focus(); return; }
    if (event.key !== 'Enter') { return; }
    event.preventDefault();
    var first = findAll(el('emoji-grid'), function (n) { return n.tagName === 'BUTTON' && n.getAttribute('data-emoji'); })[0];
    if (first) { insertEmoji(first.getAttribute('data-emoji')); }
  });
  // The grid must not take the focus either, or the editor's selection goes.
  el('emoji-grid').addEventListener('mousedown', function (event) { event.preventDefault(); });
  el('emoji-grid').addEventListener('click', function (event) {
    var b = event.target;
    while (b && b !== el('emoji-grid') && !(b.getAttribute && b.getAttribute('data-emoji'))) { b = b.parentNode; }
    if (!b || b === el('emoji-grid')) { return; }
    insertEmoji(b.getAttribute('data-emoji'));
  });

  // ---- Images in the message --------------------------------------------------
  // Pasted, dropped on the editor, or chosen from a file with the Image
  // button. Held as a data: URL - the one form a page can hold an image in
  // with no server to put it on - drawn no larger than 1200 pixels on its
  // longer side when the browser can draw (a screenshot of a whole screen is
  // three times that), and moved into a cid: part beside the HTML by the send
  // route, so that Gmail and Outlook show it (InlineImageParts.cpp). What is
  // checked is what the server checks: the file's first bytes, not its name.
  // A message's pictures are at most 6 MB of characters together, and one
  // image at most the 2,000,000 the rebuild admits; they then count for what
  // they are worth in bytes against the same twelve megabytes the files have
  // (readFiles), because the pictures and the files travel in the one request
  // the route bounds at sixteen.
  var IMAGE_BOX = { width: 1200, height: 1200 };
  var IMAGE_EACH = 2000000;
  var IMAGES_TOGETHER = 6 * 1024 * 1024;
  // And how many of them: what the send route puts back in place
  // (InlineImageParts::MostImages). A picture past that number is left in the
  // HTML as the data: URL it is, and a data: image is the one thing Gmail and
  // Outlook do not show - which is the whole reason the pictures become parts
  // at all. A walkthrough with a small screen grab at every step reaches
  // sixty-five long before it reaches six megabytes, so the count is a bound
  // of its own and not a consequence of the bytes.
  var MOST_IMAGES = 64;
  var picturesIn = function () { return findAll(el('compose-editor'), function (n) { return n.tagName === 'IMG' && String(n.getAttribute('src') || '').indexOf('data:') === 0; }); };
  var imagesCount = function () { return picturesIn().length; };
  var imagesHeld = function () {
    return picturesIn().reduce(function (n, img) { return n + String(img.getAttribute('src') || '').length; }, 0);
  };
  var isImageFile = function (f) { return /^image\/(png|jpeg|gif|webp)$/i.test(String((f && f.type) || '')); };
  var readAsDataUrl = function (file) {
    return new Promise(function (resolve) {
      var reader = new FileReader();
      reader.onload = function () { resolve(String(reader.result || '')); };
      reader.onerror = function () { resolve(''); };
      reader.readAsDataURL(file);
    });
  };
  // The image within the box and within room, or null. One already within
  // the box that fits is kept byte for byte - a JPEG is not encoded twice
  // for nothing; a larger one is drawn smaller, a JPEG as a JPEG and anything
  // else as a PNG (which keeps transparency and loses a GIF's animation), and
  // smaller again until it fits.
  var fitImage = function (url, type, room) {
    var canvas = document.createElement('canvas');
    if (typeof canvas.getContext !== 'function' || typeof Image !== 'function') { return Promise.resolve(url.length <= room ? url : null); }
    return new Promise(function (resolve) {
      var img = new Image();
      img.onerror = function () { resolve(url.length <= room ? url : null); };
      img.onload = function () {
        var w = img.naturalWidth || img.width, h = img.naturalHeight || img.height;
        var scale = w && h ? Math.min(1, IMAGE_BOX.width / w, IMAGE_BOX.height / h) : 1;
        if (scale >= 1 && url.length <= room) { resolve(url); return; }
        var out = type === 'image/jpeg' ? 'image/jpeg' : 'image/png';
        for (var attempt = 0; attempt < 8; attempt++) {
          var cw = Math.max(1, Math.round(w * scale)), ch = Math.max(1, Math.round(h * scale));
          canvas.width = cw; canvas.height = ch;
          var context = canvas.getContext('2d');
          if (!context) { break; }
          context.clearRect(0, 0, cw, ch);
          context.drawImage(img, 0, 0, cw, ch);
          var smaller = String(canvas.toDataURL(out, 0.85) || '');
          if (smaller.indexOf('data:' + out + ';base64,') === 0 && smaller.length <= room) { resolve(smaller); return; }
          if (cw <= 64 || ch <= 64) { break; }
          scale *= 0.75;
        }
        resolve(null);
      };
      img.src = url;
    });
  };
  // One file into the editor at the range (or at the end); the plain box
  // cannot hold a picture, so formatting is switched on first. Resolves to
  // what became of it - the sentence that refused it, or whether it had to be
  // made smaller - and says nothing itself: addImages writes the one line that
  // answers for the whole paste or drop.
  var addImage = function (file, range) {
    var refuse = function (why) { return { refused: why, smaller: false }; };
    if (!isImageFile(file)) { return Promise.resolve(refuse(t('An image to put in the message: PNG, JPEG, GIF or WebP.'))); }
    return readAsDataUrl(file).then(function (url) {
      var type = sniffImageType(url);
      if (!type) { return refuse(t('An image to put in the message: PNG, JPEG, GIF or WebP.')); }
      if ((richOn ? imagesCount() : 0) >= MOST_IMAGES) { return refuse(t('This message holds as many images as it can carry: 64 of them.')); }
      var left = IMAGES_TOGETHER - (richOn ? imagesHeld() : 0);
      var room = Math.min(IMAGE_EACH - 1, left);
      if (room <= 0) { return refuse(t('This message holds as many images as it can carry: 6 MB of them together.')); }
      var typed = 'data:' + type + ';base64,' + (url.split(',')[1] || '');
      return fitImage(typed, type, room).then(function (fitted) {
        // Which bound it was that the image could not be brought under.
        if (!fitted) { return refuse(left < IMAGE_EACH - 1 ? t('This message holds as many images as it can carry: 6 MB of them together.') : t('The image is too large for a message even when made smaller.')); }
        if (!richOn) { setRich(true); range = null; }
        var img = document.createElement('img');
        img.setAttribute('src', fitted);
        img.setAttribute('alt', String(file.name || '').replace(/[\r\n"<>]/g, ''));
        insertInEditor(img, range);
        return { refused: '', smaller: fitted !== typed };
      });
    });
  };
  // A paste or a drop may carry several files. Each goes in turn, and the
  // status line is written once, at the end, naming every file that did not
  // go in: a refusal used to be wiped off it by the next file of the same
  // batch going in, so a reader who dropped three files and had one refused
  // was told nothing at all.
  var addImages = function (files, range) {
    var refused = [], smaller = 0, went = 0;
    return files.reduce(function (chain, f) {
      return chain.then(function () {
        return addImage(f, range).then(function (what) {
          if (what.refused) { refused.push({ name: String((f && f.name) || ''), why: what.refused }); return; }
          went += 1;
          if (what.smaller) { smaller += 1; }
        });
      });
    }, Promise.resolve()).then(function () {
      // One file on its own needs no name - the reader has just chosen it -
      // and in a batch each refusal carries the name of the file it is about.
      if (refused.length) { say('compose-status', files.length === 1 ? refused[0].why : refused.map(function (one) { return (one.name || '?') + ': ' + one.why; }).join(' '), false); return went > 0; }
      say('compose-status', smaller ? t('The image was made smaller to fit.') : '', true);
      return went > 0;
    });
  };
  var pastedImages = function (event) {
    var data = event.clipboardData || window.clipboardData;
    if (!data) { return []; }
    var files = Array.prototype.slice.call(data.files || []).filter(function (f) { return /^image\//i.test(String(f.type || '')); });
    if (!files.length && data.items) {
      for (var i = 0; i < data.items.length; i++) {
        var item = data.items[i];
        if (item.kind === 'file' && /^image\//i.test(String(item.type || ''))) { var f = item.getAsFile ? item.getAsFile() : null; if (f) { files.push(f); } }
      }
    }
    return files;
  };
  // The point a drop landed on, as a range, where the browser says.
  var pointRange = function (event) {
    var editor = el('compose-editor');
    try {
      if (document.caretRangeFromPoint) { var r = document.caretRangeFromPoint(event.clientX, event.clientY); return r && within(r.startContainer, editor) ? r : null; }
      if (document.caretPositionFromPoint && document.createRange) {
        var p = document.caretPositionFromPoint(event.clientX, event.clientY);
        if (p && within(p.offsetNode, editor)) { var r2 = document.createRange(); r2.setStart(p.offsetNode, p.offset); r2.collapse(true); return r2; }
      }
    } catch (e) { /* a point outside any text */ }
    return null;
  };
  ['compose-editor', 'compose-text'].forEach(function (id) {
    // A paste that carries an image is the image; a paste of text is the
    // browser's, as it was.
    el(id).addEventListener('paste', function (event) {
      var files = pastedImages(event);
      if (!files.length) { return; }
      event.preventDefault();
      addImages(files, id === 'compose-editor' ? editorRange() : null);
    });
    // An image dropped on the text goes into it; any other file dropped
    // there is attached, as a file dropped anywhere on the form is.
    el(id).addEventListener('drop', function (event) {
      var files = Array.prototype.slice.call((event.dataTransfer && event.dataTransfer.files) || []);
      var images = files.filter(isImageFile);
      if (!images.length) { return; }
      event.preventDefault();
      event.stopPropagation();
      el('compose-form').classList.remove('dropping');
      var others = files.filter(function (f) { return !isImageFile(f); });
      others.forEach(function (f) { dropped.push(f); });
      if (others.length) { el('compose-files-note').textContent = 'Dropped: ' + dropped.map(function (f) { return f.name + ' (' + format(f.size) + ')'; }).join(', '); }
      addImages(images, id === 'compose-editor' ? (pointRange(event) || editorRange()) : null);
    });
  });
  var imageRange = null;
  el('image-open').addEventListener('click', function () { imageRange = editorRange(); el('compose-image').click(); });
  el('compose-image').addEventListener('change', function () {
    var files = Array.prototype.slice.call(el('compose-image').files || []);
    el('compose-image').value = '';
    if (files.length) { addImages(files, imageRange); }
    imageRange = null;
  });

  // ---- Tables ---------------------------------------------------------------------
  // Rows by columns, with a header row or not, inserted where the reader is;
  // with the cursor in a cell the same button offers a row above or below,
  // a column left or right, and the removal of the row, the column or the
  // table. Tab moves to the next cell and adds a row after the last, as a
  // word processor does. A cell holds a break so that it has a height to
  // click into while it is empty.
  var newCell = function (header) { var c = document.createElement(header ? 'th' : 'td'); c.appendChild(document.createElement('br')); return c; };
  var newRow = function (columns, header) { var tr = document.createElement('tr'); for (var i = 0; i < columns; i++) { tr.appendChild(newCell(header)); } return tr; };
  var rowOf = function (cell) { var n = cell; while (n && n.tagName !== 'TR') { n = n.parentNode; } return n; };
  var tableOf = function (cell) { var n = cell; while (n && n.tagName !== 'TABLE') { n = n.parentNode; } return n; };
  var cellsOf = function (row) { return Array.prototype.filter.call(row.childNodes, function (c) { return c.nodeType === 1 && (c.tagName === 'TD' || c.tagName === 'TH'); }); };
  // This table's rows, not those of a table pasted into one of its cells: a
  // column added to the one must not be added to the other.
  var rowsOf = function (table) { return findAll(table, function (n) { return n.tagName === 'TR' && tableOf(n) === table; }); };
  var siblingAfter = function (n) { var kids = n.parentNode.childNodes; var i = Array.prototype.indexOf.call(kids, n); return kids[i + 1] || null; };
  var placeCaret = function (cell) {
    currentCell = cell || null;
    if (!cell || !document.createRange) { return; }
    var r = document.createRange();
    r.setStart(cell, 0); r.collapse(true);
    selectRange(r);
  };
  var tableRange = null;
  var insertTable = function (rows, columns, header) {
    rows = Math.min(50, Math.max(1, Math.floor(Number(rows) || 0)));
    columns = Math.min(20, Math.max(1, Math.floor(Number(columns) || 0)));
    var table = document.createElement('table'), body = document.createElement('tbody');
    for (var r = 0; r < rows; r++) { body.appendChild(newRow(columns, header && r === 0)); }
    table.appendChild(body);
    var range = tableRange;
    if (!richOn) { setRich(true); range = null; }
    insertInEditor(table, range);
    // A line after the table, so that the reader can write below it.
    var after = document.createElement('p'); after.appendChild(document.createElement('br'));
    table.parentNode.insertBefore(after, siblingAfter(table));
    placeCaret(cellsOf(rowsOf(table)[0])[0]);
  };
  var TABLE_OPS = {
    'row-above': function (cell) { var row = rowOf(cell); var added = newRow(cellsOf(row).length, false); row.parentNode.insertBefore(added, row); placeCaret(cellsOf(added)[0]); },
    'row-below': function (cell) { var row = rowOf(cell); var added = newRow(cellsOf(row).length, false); row.parentNode.insertBefore(added, siblingAfter(row)); placeCaret(cellsOf(added)[0]); },
    'column-left': function (cell) {
      var at = cellsOf(rowOf(cell)).indexOf(cell), made = null;
      rowsOf(tableOf(cell)).forEach(function (r) { var cells = cellsOf(r); var c = newCell(cells[0] && cells[0].tagName === 'TH'); r.insertBefore(c, cells[at] || null); if (r === rowOf(cell)) { made = c; } });
      placeCaret(made);
    },
    'column-right': function (cell) {
      var at = cellsOf(rowOf(cell)).indexOf(cell), made = null;
      rowsOf(tableOf(cell)).forEach(function (r) { var cells = cellsOf(r); var c = newCell(cells[0] && cells[0].tagName === 'TH'); r.insertBefore(c, cells[at] ? siblingAfter(cells[at]) : null); if (r === rowOf(cell)) { made = c; } });
      placeCaret(made);
    },
    'delete-row': function (cell) {
      var row = rowOf(cell), table = tableOf(cell), rows = rowsOf(table);
      var at = rows.indexOf(row), column = cellsOf(row).indexOf(cell);
      row.parentNode.removeChild(row);
      rows = rowsOf(table);
      if (!rows.length) { table.parentNode.removeChild(table); placeCaret(null); return; }
      var next = rows[Math.min(at, rows.length - 1)];
      placeCaret(cellsOf(next)[Math.min(column, cellsOf(next).length - 1)]);
    },
    'delete-column': function (cell) {
      var row = rowOf(cell), table = tableOf(cell), at = cellsOf(row).indexOf(cell);
      rowsOf(table).forEach(function (r) { var c = cellsOf(r)[at]; if (c) { r.removeChild(c); } });
      var left = cellsOf(row);
      if (!left.length) { table.parentNode.removeChild(table); placeCaret(null); return; }
      placeCaret(left[Math.min(at, left.length - 1)]);
    },
    'delete-table': function (cell) { var table = tableOf(cell); table.parentNode.removeChild(table); placeCaret(null); }
  };
  var tableClose = function () { el('table-panel').hidden = true; el('table-open').setAttribute('aria-expanded', 'false'); };
  var tableToggle = function () {
    var panel = el('table-panel');
    var open = panel.hidden;
    emojiClose();
    if (!open) { tableClose(); return; }
    var editor = el('compose-editor');
    var inCell = currentCell && within(currentCell, editor) ? currentCell : null;
    currentCell = inCell;
    tableRange = editorRange();
    el('table-insert').hidden = !!inCell;
    el('table-edit').hidden = !inCell;
    panel.hidden = false;
    el('table-open').setAttribute('aria-expanded', 'true');
    if (!inCell) { el('table-rows').focus(); }
  };
  el('table-open').addEventListener('click', function (event) { event.stopPropagation(); tableToggle(); });
  el('table-insert-go').addEventListener('click', function () {
    insertTable(el('table-rows').value, el('table-columns').value, el('table-header').checked);
    tableClose();
    el('compose-editor').focus();
  });
  Array.prototype.forEach.call(el('table-edit').children, function (b) {
    b.addEventListener('mousedown', function (event) { event.preventDefault(); });
    b.addEventListener('click', function () {
      var op = TABLE_OPS[b.getAttribute('data-op')];
      var cell = currentCell && within(currentCell, el('compose-editor')) ? currentCell : null;
      if (!op || !cell) { tableClose(); return; }
      op(cell);
      el('compose-editor').focus();
      if (currentCell) { placeCaret(currentCell); } else { tableClose(); }
    });
  });
  el('compose-editor').addEventListener('keydown', function (event) {
    if (event.key !== 'Tab' || event.ctrlKey || event.metaKey || event.altKey) { return; }
    var live = liveRange();
    var cell = (live && cellOf(live.startContainer)) || cellOf(event.target) || null;
    if (!cell) { return; }
    event.preventDefault();
    var row = rowOf(cell), cells = cellsOf(row), at = cells.indexOf(cell);
    var rows = rowsOf(tableOf(cell)), ri = rows.indexOf(row);
    if (event.shiftKey) {
      if (at > 0) { placeCaret(cells[at - 1]); }
      else if (ri > 0) { var before = cellsOf(rows[ri - 1]); placeCaret(before[before.length - 1]); }
      return;
    }
    if (at < cells.length - 1) { placeCaret(cells[at + 1]); return; }
    if (ri < rows.length - 1) { placeCaret(cellsOf(rows[ri + 1])[0]); return; }
    var added = newRow(cells.length, false);
    row.parentNode.insertBefore(added, siblingAfter(row));
    placeCaret(cellsOf(added)[0]);
  });

  // ---- A draft's formatting, brought back ----------------------------------------
  // A draft reopens as its text; one with formatting - a table, an image
  // pasted in - comes back into the formatted editor instead, through the
  // same subset the send admits, so that nothing a draft carries can reach
  // the page but what the page would have sent. The images this webmail made
  // parts of it (OWN_IMAGE_ID) are put back as the data: URLs they were, from
  // the parts, so that the send makes parts of them again; an image another
  // mail client embedded stays a file, as carryAttachments keeps it.
  var draftFormatting = function (message) {
    var html = String(message.html || '');
    if (!html) { return Promise.resolve(); }
    // Only a message this account saved as a draft. #compose?draft=<id> names
    // a message by its number and nothing stops that number being a message
    // that arrived from outside, whose HTML is a stranger's; and this is the
    // one place the page puts message HTML into its own document rather than
    // into the sandboxed frame an open message is shown in. A message without
    // the Draft flag reopens as its text, as every draft did before this.
    if (!(message.flags && message.flags.draft)) { return Promise.resolve(); }
    var lower = html.toLowerCase();
    var own = (message.attachments || []).filter(function (a) {
      var cid = contentIdOf(a).toLowerCase();
      return cid && OWN_IMAGE_ID.test(cid) && lower.indexOf('cid:' + cid) >= 0 && RENDERS[String(a.content_type || '').toLowerCase()];
    });
    return Promise.all(own.map(function (a) {
      return apiFetch('/api/v1/me/messages/' + message.id + '/attachments/' + a.index, { cache: 'no-store', credentials: 'same-origin' }).then(function (response) {
        return response.status === 200 ? response.blob() : null;
      }).then(function (blob) {
        if (!blob || String(blob.type || '').indexOf('image/') !== 0) { return null; }
        return readAsDataUrl(blob).then(function (url) { return url.indexOf('data:image/') === 0 ? { id: contentIdOf(a).toLowerCase(), url: url } : null; });
      }, function () { return null; });
    })).then(function (found) {
      var images = {};
      found.forEach(function (one) { if (one) { images[one.id] = one.url; } });
      var resolved = resolveInline(html, images);
      var editor = el('compose-editor');
      var parsed = null;
      try { parsed = new DOMParser().parseFromString(cleanHtml(new DOMParser().parseFromString(resolved, 'text/html').body), 'text/html').body; } catch (e) { parsed = null; }
      if (!parsed) { return; }
      clear(editor);
      while (parsed.firstChild) { editor.appendChild(parsed.firstChild); }
      showRich(true);
    });
  };

  // ---- Offline: the inbox and opened messages kept in this browser, sends
  // queued, all of it forgotten at sign-out.
  var offlineData = function () { try { return JSON.parse(localStorage.getItem('hmPortalOffline') || '{}') || {}; } catch (e) { return {}; } };
  var offlineWrite = function (d) { try { localStorage.setItem('hmPortalOffline', JSON.stringify(d)); } catch (e) { /* storage full or refused: nothing kept */ } };
  // Every write to and read from either store is the first account's: the
  // second account's mail is never mixed into what this browser keeps, and a
  // page opened with no connection shows the first account's, whose it is.
  var offlineKeep = function (key, value) {
    if (!offlineMine()) { return; }
    if (key !== 'list' && value && value.html && value.html.length > 100000) { return; }
    var d = offlineData();
    d.items = d.items || {};
    d.order = d.order || [];
    d.items[key] = value;
    var at = d.order.indexOf(key);
    if (at >= 0) { d.order.splice(at, 1); }
    d.order.push(key);
    while (d.order.length > 31) { var old = d.order.shift(); if (old !== 'list') { delete d.items[old]; } else { d.order.push(old); } }
    offlineWrite(d);
  };
  var offlineRead = function (key) { if (!offlineMine()) { return null; } var d = offlineData(); return d.items && d.items[key] ? d.items[key] : null; };
  var offlineForget = function () { try { localStorage.removeItem('hmPortalOffline'); } catch (e) { /* nothing to forget */ } offlineForgetAll(); };
  var offlineNote = function (on) { el('offline-note').hidden = !on; };
  var outboxAdd = function (body) { if (!offlineMine()) { return false; } var d = offlineData(); d.outbox = d.outbox || []; d.outbox.push(body); offlineWrite(d); return true; };
  var outboxFlush = function () {
    if (!offlineMine()) { return; }
    var d = offlineData();
    if (!d.outbox || !d.outbox.length || el('account').hidden) { return; }
    var queued = d.outbox.slice();
    d.outbox = [];
    offlineWrite(d);
    var chain = Promise.resolve();
    queued.forEach(function (body) {
      chain = chain.then(function () {
        return call('POST', '/api/v1/me/messages', body).then(function (result) {
          if (result.status === 0) { outboxAdd(body); }
          else if (result.status === 201) { say('mail-status', t('A message written without a connection has been sent.'), true); loadFolders(); }
          else { say('mail-status', describe(result, t('A queued message could not be sent')), false); }
        });
      });
    });
  };
  window.addEventListener('online', function () {
    offlineNote(false);
    // A page that was opened with no connection now has one: the account is
    // asked for as it is at every start, and the kept mail gives way to the real thing.
    if (offline.only) { offlineRetryStop(); load(true); return; }
    outboxFlush();
    offlineFill();
  });
  window.addEventListener('offline', function () { offlineNote(true); });
  if (typeof navigator !== 'undefined' && navigator.serviceWorker && navigator.serviceWorker.register) {
    navigator.serviceWorker.register('/portal-sw.js').then(function () { pushWords(); }).catch(function () { /* the page works without it */ });
  }
  var setTimeout0 = function (fn) { if (typeof window.requestAnimationFrame === 'function') { window.requestAnimationFrame(fn); } };

  // ---- Offline for the last N days, Gmail's offline mail: the bodies and
  // attachments of the messages received within the window the reader chose -
  // under the ceiling the administrator set - kept in the browser's IndexedDB
  // as they are opened and in the background, read and searched without a
  // connection, forgotten at sign-out. The server's part is GET/PUT
  // /api/v1/me/offline (the ceiling, the choice, the window's start) and
  // /api/v1/me/offline/messages, the listing the store is filled from and
  // pruned by; the service worker answers a kept message's HTML body and
  // attachments out of the same store when the network fails. Four stores:
  // rows (what a listing and a search need, read whole), messages (the body
  // JSON and the keys of its parts), parts (the HTML document and the
  // attachments as blobs) and meta (the folder tree). It lives in this
  // browser: the Settings card says so, counts it, and clears it.
  var OFFLINE_DB = 'hmPortalOffline';
  var OFFLINE_PART_EACH = 8 * 1024 * 1024;       // an attachment above this is not kept
  var OFFLINE_MESSAGE_MOST = 32 * 1024 * 1024;   // and a message's parts together stop here
  var OFFLINE_SLICE = 25;                        // bodies fetched per background run
  var OFFLINE_STEPS = [7, 14, 30, 60, 90, 180, 365];
  var offline = { ceiling: 0, days: 0, since: '', count: 0, bytes: 0, kept: 0, keptBytes: 0, walked: false, filling: false, again: false, full: false, only: false, retry: 0, quota: null };
  var offlineRows = null;   // id -> the rows record, read once from the store and kept in step with it
  var folderTreeLast = null;   // the tree as the server last listed it, for the store
  var offlineDbPromise = null;
  var offlineSchema = function (db) {
    ['rows', 'messages', 'parts', 'meta'].forEach(function (name) {
      if (!db.objectStoreNames.contains(name)) { db.createObjectStore(name, { keyPath: name === 'parts' || name === 'meta' ? 'key' : 'id' }); }
    });
  };
  var offlineDb = function () {
    if (offlineDbPromise) { return offlineDbPromise; }
    offlineDbPromise = new Promise(function (resolve) {
      if (typeof indexedDB === 'undefined' || !indexedDB || !indexedDB.open) { resolve(null); return; }
      var request;
      try { request = indexedDB.open(OFFLINE_DB, 1); } catch (e) { resolve(null); return; }
      request.onupgradeneeded = function () { offlineSchema(request.result); };
      request.onsuccess = function () { resolve(request.result); };
      request.onerror = function () { resolve(null); };
      request.onblocked = function () { resolve(null); };
    });
    return offlineDbPromise;
  };
  // One request on one store, as a promise; a store that cannot be reached
  // answers null, and nothing here throws at a browser without the API.
  var idb = function (store, mode, act) {
    return offlineDb().then(function (db) {
      if (!db) { return null; }
      return new Promise(function (resolve) {
        var request;
        try { request = act(db.transaction(store, mode).objectStore(store)); } catch (e) { resolve(null); return; }
        request.onsuccess = function () { resolve(request.result === undefined ? null : request.result); };
        request.onerror = function () { resolve(null); };
      });
    });
  };
  var metaGet = function (key) { return idb('meta', 'readonly', function (s) { return s.get(key); }).then(function (r) { return r ? r.value : null; }); };
  var metaPut = function (key, value) { return idb('meta', 'readwrite', function (s) { return s.put({ key: key, value: value }); }); };
  var offlineOwner = function () { try { return localStorage.getItem('hmPortalOfflineOwner') || ''; } catch (e) { return ''; } };
  var offlineOwnerWrite = function (address) { try { if (address) { localStorage.setItem('hmPortalOfflineOwner', address); } else { localStorage.removeItem('hmPortalOfflineOwner'); } } catch (e) { /* a browser that keeps nothing */ } };
  var offlineTally = function () {
    var n = 0, b = 0;
    Object.keys(offlineRows || {}).forEach(function (k) { n++; b += offlineRows[k].bytes || 0; });
    offline.kept = n; offline.keptBytes = b;
  };
  var offlineRowsRead = function () {
    if (offlineRows) { return Promise.resolve(offlineRows); }
    return idb('rows', 'readonly', function (s) { return s.getAll(); }).then(function (all) {
      offlineRows = {};
      (all || []).forEach(function (r) { if (r && r.id) { offlineRows[r.id] = r; } });
      offlineTally();
      return offlineRows;
    });
  };
  var offlineWithin = function (received) { return !!offline.since && String(received || '') >= offline.since; };
  // The text a search looks through: subject, sender, recipients and the
  // body - the plain part, or the words of the HTML one.
  var offlineWords = function (m) {
    var text = m.text || '';
    if (!text && m.html) { try { text = new DOMParser().parseFromString(m.html, 'text/html').body.textContent || ''; } catch (e) { text = ''; } }
    return ((m.subject || '') + '\n' + (m.from || '') + '\n' + (m.to || '') + '\n' + (m.cc || '') + '\n' + text).toLowerCase();
  };
  // The flags, copied. The kept row must not share the flags object with the
  // message the page holds, or a change the page makes to its own copy is
  // already in the row before anything is written to the store: the sync
  // below then finds nothing to do, and the browser keeps the old flags.
  var flagsCopy = function (flags) {
    var out = {};
    Object.keys(flags || {}).forEach(function (k) { out[k] = k === 'keywords' ? (flags[k] || []).slice() : flags[k]; });
    return out;
  };
  // The listing row the folder view draws, from the message's own JSON.
  var offlineRowOf = function (m) {
    var text = m.text || '';
    if (!text && m.html) { try { text = new DOMParser().parseFromString(m.html, 'text/html').body.textContent || ''; } catch (e) { text = ''; } }
    return { id: m.id, uid: m.uid, folder_id: m.folder_id, size: m.size, received: m.received, subject: m.subject || '', from: m.from || '', to: m.to || '', date: m.date || '',
             flags: flagsCopy(m.flags), message_id: m.message_id || '', references: m.references || '', in_reply_to: m.in_reply_to || '', has_attachments: !!(m.attachments || []).length,
             snippet: text.replace(/\s+/g, ' ').trim().slice(0, 120), brand: m.brand || '', category: m.category || '',
             // The attachments' names, for filename: asked with no connection.
             names: (m.attachments || []).map(function (a) { return String(a.name || '').toLowerCase(); }).join('\n') };
  };
  var offlineFetchPart = function (url) {
    return apiFetch(url, { cache: 'no-store', credentials: 'same-origin' }, 'first').then(function (response) {
      if (response.status !== 200) { return null; }
      return response.blob().then(function (blob) { return { blob: blob, type: response.headers.get('Content-Type') || blob.type || 'application/octet-stream' }; });
    }, function () { return null; });
  };
  // The HTML document the server renders for the message (its cid: images
  // inlined) and the attachments, each within a size and all within a budget.
  var offlineParts = function (m) {
    var wanted = [];
    if (m.html) { wanted.push({ key: m.id + '/html', url: '/api/v1/me/messages/' + m.id + '/html', size: m.html.length, name: '' }); }
    var budget = OFFLINE_MESSAGE_MOST;
    (m.attachments || []).forEach(function (a) {
      if (a.size > OFFLINE_PART_EACH || a.size > budget) { return; }
      budget -= a.size;
      wanted.push({ key: m.id + '/' + a.index, url: '/api/v1/me/messages/' + m.id + '/attachments/' + a.index, size: a.size, name: a.name || '' });
    });
    var keys = [], bytes = 0;
    var chain = Promise.resolve();
    wanted.forEach(function (w) {
      chain = chain.then(function () {
        return offlineFetchPart(w.url).then(function (part) {
          if (!part) { return; }
          return idb('parts', 'readwrite', function (s) { return s.put({ key: w.key, id: m.id, type: part.type, name: w.name, blob: part.blob }); }).then(function (ok) {
            if (ok !== null) { keys.push(w.key); bytes += (part.blob && part.blob.size) || w.size || 0; }
          });
        });
      });
    });
    return chain.then(function () { return { keys: keys, bytes: bytes }; });
  };
  // A message the reader opened, or the background run fetched: kept when the
  // window covers it. One already kept has only its flags brought up to date.
  var offlineKeepMessage = function (m) {
    if (!offlineMine() || !offline.days || !m || !m.id || m.truncated || !offlineWithin(m.received)) { return Promise.resolve(false); }
    return offlineRowsRead().then(function (rows) {
      if (rows[m.id]) { return offlineFlagsSync(m); }
      return offlineParts(m).then(function (parts) {
        var row = { id: m.id, folder_id: m.folder_id, received: m.received, bytes: (m.size || 0) + parts.bytes, row: offlineRowOf(m), words: offlineWords(m) };
        return idb('messages', 'readwrite', function (s) { return s.put({ id: m.id, data: m, parts: parts.keys }); }).then(function (ok) {
          if (ok === null) { return false; }
          return idb('rows', 'readwrite', function (s) { return s.put(row); }).then(function (ok2) {
            if (ok2 === null) { return false; }
            rows[m.id] = row;
            if (offline.quota) { offline.quota.usage += row.bytes; }
            offlineTally();
            renderOfflineCard();
            return true;
          });
        });
      });
    });
  };
  // A flag or a folder that moved on the server, or was moved from this page.
  var offlineFlagsSync = function (m) {
    if (!offlineMine()) { return Promise.resolve(false); }
    return offlineRowsRead().then(function (rows) {
      var row = rows[m.id];
      if (!row) { return false; }
      var seen = !!(m.flags && m.flags.seen), flagged = !!(m.flags && m.flags.flagged);
      var folderId = m.folder_id || row.folder_id;
      // The keywords count as a change too: $Important, a label, $Muted. A
      // change of nothing but those used to stop here, so the kept copy kept
      // the old keywords and the message read offline was in the wrong
      // section with no marker on it.
      var words = function (flags) { return ((flags && flags.keywords) || []).slice().sort().join(','); };
      if (row.row.flags && !!row.row.flags.seen === seen && !!row.row.flags.flagged === flagged &&
          words(row.row.flags) === words(m.flags) && row.folder_id === folderId) { return true; }
      row.row.flags = m.flags ? flagsCopy(m.flags) : row.row.flags;
      row.folder_id = folderId; row.row.folder_id = folderId;
      return idb('rows', 'readwrite', function (s) { return s.put(row); }).then(function () {
        return idb('messages', 'readonly', function (s) { return s.get(m.id); });
      }).then(function (record) {
        if (!record || !record.data) { return true; }
        record.data.flags = row.row.flags; record.data.folder_id = folderId;
        return idb('messages', 'readwrite', function (s) { return s.put(record); }).then(function () { return true; });
      });
    });
  };
  var offlineDrop = function (id) {
    return idb('messages', 'readonly', function (s) { return s.get(id); }).then(function (record) {
      var chain = Promise.resolve();
      ((record && record.parts) || []).forEach(function (key) { chain = chain.then(function () { return idb('parts', 'readwrite', function (s) { return s.delete(key); }); }); });
      return chain.then(function () { return idb('messages', 'readwrite', function (s) { return s.delete(id); }); })
        .then(function () { return idb('rows', 'readwrite', function (s) { return s.delete(id); }); })
        .then(function () { if (offlineRows) { delete offlineRows[id]; } offlineTally(); });
    });
  };
  var offlineClear = function () {
    offlineRows = {};
    offlineTally();
    return idb('parts', 'readwrite', function (s) { return s.clear(); })
      .then(function () { return idb('messages', 'readwrite', function (s) { return s.clear(); }); })
      .then(function () { return idb('rows', 'readwrite', function (s) { return s.clear(); }); });
  };
  // Sign-out: the database goes, and the name of whose mail it held.
  var offlineForgetAll = function () {
    offlineRows = null;
    offline.kept = 0; offline.keptBytes = 0; offline.walked = false; offline.only = false;
    offlineRetryStop();
    offlineFillStop();
    offlineOwnerWrite('');
    var opened = offlineDbPromise;
    offlineDbPromise = null;
    Promise.resolve(opened).then(function (db) {
      if (db && db.close) { try { db.close(); } catch (e) { /* closed already */ } }
      try { if (typeof indexedDB !== 'undefined' && indexedDB && indexedDB.deleteDatabase) { indexedDB.deleteDatabase(OFFLINE_DB); } } catch (e) { /* nothing to delete */ }
    });
  };
  var offlineMessage = function (id) {
    if (!offlineMine()) { return Promise.resolve(null); }
    return idb('messages', 'readonly', function (s) { return s.get(id); }).then(function (record) { return record && record.data ? record.data : null; });
  };
  // What the browser allows this site, asked once per run and kept in step
  // as messages are kept, so a slice that will not fit stops before it fails.
  var offlineEstimate = function () {
    if (typeof navigator === 'undefined' || !navigator.storage || !navigator.storage.estimate) { offline.quota = null; return Promise.resolve(null); }
    return Promise.resolve().then(function () { return navigator.storage.estimate(); }).then(function (e) {
      offline.quota = e && e.quota ? { usage: Number(e.usage) || 0, quota: Number(e.quota) } : null;
      return offline.quota;
    }, function () { offline.quota = null; return null; });
  };
  var offlineRoom = function (size) { return !offline.quota || offline.quota.usage + size * 2 < offline.quota.quota * 0.9; };
  // The reader's window, then the store brought into step with it.
  var loadOffline = function () {
    if (offline.only) { return Promise.resolve(); }
    // The second account: the card says whose the store is, and asks nothing.
    if (!offlineMine()) { renderOfflineCard(); return Promise.resolve(); }
    return call('GET', '/api/v1/me/offline').then(function (result) {
      // Only an answer that really carries the choice is acted on. A request
      // that never arrived, a server that could not read the choice (500),
      // an older server without the route: none of these is a reader who
      // chose nothing, and what is kept stays as it is until one answers.
      // Emptying the store on a failed read would throw away a window of
      // mail over a database that was unreachable for a moment.
      if (result.status !== 200 || !result.data || typeof result.data.days !== 'number' || typeof result.data.days_max !== 'number') { return; }
      offline.ceiling = Number(result.data.days_max) || 0;
      offline.days = Number(result.data.days) || 0;
      offline.since = offline.days ? String(result.data.since || '') : '';
      renderOfflineCard();
      if (offline.days) { offlineOwnerWrite(me); return offlineFill(); }
      // Nothing chosen - here, or from another browser: whatever was kept goes.
      offlineOwnerWrite('');
      return offlineRowsRead().then(function (rows) { if (Object.keys(rows).length) { return offlineClear().then(renderOfflineCard); } });
    });
  };
  var offlineWalk = function (afterId, listed) {
    return call('GET', '/api/v1/me/offline/messages?after_id=' + afterId + '&limit=500').then(function (result) {
      if (result.status !== 200 || !result.data) { return null; }
      var d = result.data;
      // A listing that says nothing is chosen while this page holds a choice
      // is not a listing to prune by: a walk that answered nothing would
      // drop everything. The next GET /api/v1/me/offline settles it.
      if (!Number(d.days) && offline.days) { return null; }
      offline.count = Number(d.count) || 0; offline.bytes = Number(d.bytes) || 0;
      if (d.since) { offline.since = String(d.since); }
      (d.messages || []).forEach(function (row) { listed[row.id] = row; });
      if (d.more && Number(d.next_after_id) > afterId) { return offlineWalk(Number(d.next_after_id), listed); }
      return listed;
    });
  };
  // The background run: what the window no longer holds goes, a flag or a
  // folder that moved is moved here, then the bodies still missing come in
  // newest first, a slice at a time, while the browser has room for them.
  var offlineFill = function () {
    if (!offlineMine() || !offline.days || el('account').hidden || offline.only) { return Promise.resolve(); }
    if (offline.filling) { offline.again = true; return Promise.resolve(); }
    offline.filling = true;
    var listed = {};
    if (folderTreeLast) { metaPut('folders', folderTreeLast); }
    return Promise.all([offlineRowsRead(), offlineEstimate(), offlineWalk(0, listed)]).then(function (got) {
      var rows = got[0];
      if (!got[2]) { return; }
      var chain = Promise.resolve();
      Object.keys(rows).forEach(function (id) { if (!listed[id]) { chain = chain.then(function () { return offlineDrop(Number(id)); }); } });
      Object.keys(listed).forEach(function (id) { if (rows[id]) { chain = chain.then(function () { return offlineFlagsSync(listed[id]); }); } });
      var missing = Object.keys(listed).filter(function (id) { return !rows[id]; }).map(Number).sort(function (a, b) { return b - a; });
      offline.full = false;
      var taken = 0;
      missing.forEach(function (id) {
        chain = chain.then(function () {
          if (taken >= OFFLINE_SLICE || offline.full || el('account').hidden || !offline.days) { return; }
          if (!offlineRoom(listed[id].size || 0)) { offline.full = true; return; }
          taken++;
          return call('GET', '/api/v1/me/messages/' + id).then(function (result) {
            if (result.status === 200 && result.data) { return offlineKeepMessage(result.data); }
          });
        });
      });
      return chain.then(function () {
        offline.walked = true;
        renderOfflineCard();
        if (taken >= OFFLINE_SLICE && !offline.full && missing.length > taken) { offline.again = true; }
      });
    }).then(function () { offlineFillDone(); }, function () { offlineFillDone(); });
  };
  var offlineFillDone = function () {
    offline.filling = false;
    if (offline.again) { offline.again = false; offlineAgainTimer = setTimeout(function () { offlineAgainTimer = 0; offlineFill(); }, 1500); }
  };
  // A change the probe reports - mail that arrived, a move or a delete made
  // here or elsewhere - brings the store into step a few seconds later: one
  // walk for a burst of changes, none while nothing is kept. Both timers are
  // dropped at sign-out, so no run armed for one account fires for the next.
  var offlineFillTimer = 0, offlineAgainTimer = 0;
  var offlineFillStop = function () {
    if (offlineFillTimer) { clearTimeout(offlineFillTimer); offlineFillTimer = 0; }
    if (offlineAgainTimer) { clearTimeout(offlineAgainTimer); offlineAgainTimer = 0; }
    offline.again = false;
  };
  var offlineFillSoon = function () {
    if (!offlineMine() || !offline.days || offline.only) { return; }
    if (offlineFillTimer) { clearTimeout(offlineFillTimer); }
    offlineFillTimer = setTimeout(function () { offlineFillTimer = 0; offlineFill(); }, 5000);
  };
  // A size as the reader writes one - 750, 10k, 2M, 1G, with or without a B -
  // and a span - 7d, 2w, 3m, 1y - as the moment that long ago, in the
  // received time's own form so the two compare as text; read the way the
  // server reads them, but on this browser's clock, which is the one there
  // is with no connection. -1 and '' when the value is not one.
  var offlineSizeOf = function (value) {
    var m = /^(\d{1,13})(k|kb|m|mb|g|gb)?$/.exec(value);
    if (!m) { return -1; }
    var unit = (m[2] || '').charAt(0);
    return Number(m[1]) * (unit === 'k' ? 1024 : unit === 'm' ? 1048576 : unit === 'g' ? 1073741824 : 1);
  };
  var offlineMomentAgo = function (value) {
    var m = /^(\d{1,5})([dwmy])$/.exec(value);
    if (!m || !Number(m[1])) { return ''; }
    var n = Number(m[1]), d = new Date();
    if (m[2] === 'd') { d.setDate(d.getDate() - n); } else if (m[2] === 'w') { d.setDate(d.getDate() - 7 * n); } else if (m[2] === 'm') { d.setMonth(d.getMonth() - n); } else { d.setFullYear(d.getFullYear() - n); }
    var two = function (x) { return (x < 10 ? '0' : '') + x; };
    return d.getFullYear() + '-' + two(d.getMonth() + 1) + '-' + two(d.getDate()) + ' ' + two(d.getHours()) + ':' + two(d.getMinutes()) + ':' + two(d.getSeconds());
  };
  // No connection: the kept messages of this folder, or a search over all of
  // them - the operators this store can answer (from:, to:, subject:, in:,
  // label:, category:, has:attachment, is:unread and the rest of is:, a
  // quoted phrase, -word; larger: and smaller: against the size, before:,
  // after:, older_than: and newer_than: against the received time, filename:
  // against the kept attachments' names); an operator it cannot (has:link,
  // in_reply_to:) and a value that is not one let a message through rather
  // than hide it. OR is ignored.
  var offlineMatches = function (r, query) {
    var terms = String(query || '').toLowerCase().match(/"[^"]*"|\S+/g) || [];
    var row = r.row, words = r.words || '';
    var keywords = (row.flags && row.flags.keywords) || [];
    var received = String(row.received || r.received || '');
    return terms.every(function (term) {
      if (term === 'or') { return true; }
      var negative = term.charAt(0) === '-' && term.length > 1;
      if (negative) { term = term.slice(1); }
      var at = term.indexOf(':');
      var key = at > 0 ? term.slice(0, at) : '';
      var value = (at > 0 ? term.slice(at + 1) : term).replace(/^"|"$/g, '');
      var hit, size, moment;
      if (key === 'from') { hit = String(row.from).toLowerCase().indexOf(value) >= 0; }
      else if (key === 'to' || key === 'cc' || key === 'bcc') { hit = String(row.to).toLowerCase().indexOf(value) >= 0; }
      else if (key === 'subject') { hit = String(row.subject).toLowerCase().indexOf(value) >= 0; }
      else if (key === 'in') { hit = folderName(r.folder_id).toLowerCase().indexOf(value) >= 0; }
      else if (key === 'label') { hit = keywords.some(function (k) { return String(k).toLowerCase() === value; }); }
      else if (key === 'category') { hit = String(row.category || '').toLowerCase() === value; }
      else if (key === 'has') { hit = value === 'attachment' ? !!row.has_attachments : true; }
      else if (key === 'is') {
        hit = value === 'unread' ? !row.flags.seen : value === 'read' ? !!row.flags.seen : value === 'flagged' ? !!row.flags.flagged : value === 'unflagged' ? !row.flags.flagged
            : value === 'answered' ? !!row.flags.answered : true;
      }
      else if (key === 'larger' || key === 'smaller') { size = offlineSizeOf(value); hit = size < 0 ? true : key === 'larger' ? (row.size || 0) > size : (row.size || 0) < size; }
      else if (key === 'before' || key === 'after') { hit = !/^\d{4}-\d{2}-\d{2}$/.test(value) ? true : key === 'before' ? received.slice(0, 10) < value : received.slice(0, 10) > value; }
      else if (key === 'older_than' || key === 'newer_than') { moment = offlineMomentAgo(value); hit = !moment ? true : key === 'older_than' ? received < moment : received >= moment; }
      else if (key === 'filename') { hit = typeof row.names !== 'string' ? true : !!value && row.names.indexOf(value) >= 0; }
      else if (key) { hit = true; }
      else { hit = words.indexOf(value) >= 0; }
      return negative ? !hit : hit;
    });
  };
  var offlineListing = function () {
    if (!offlineMine()) { return Promise.resolve(null); }
    return offlineRowsRead().then(function (rows) {
      var ids = Object.keys(rows).map(Number);
      if (!ids.length) { return null; }
      if (!state.query) { ids = ids.filter(function (id) { return rows[id].folder_id === state.folderId; }); }
      ids.sort(function (a, b) { return b - a; });
      var hits = ids.map(function (id) { return rows[id]; }).filter(function (r) { return !state.query || offlineMatches(r, state.query); });
      var page = { total: hits.length, scanned: ids.length, complete: true, more: false, query: state.query, offline: true,
                   messages: hits.map(function (r) { var row = r.row; row.folder = folderName(r.folder_id); return row; }) };
      if (!state.query) { page.folder_id = state.folderId; }
      return page;
    });
  };
  // A page opened with no connection, in a browser that holds mail kept for
  // exactly that: the account view with the kept folders and messages, no
  // session asked for, and load() again the moment the connection is back.
  // "Back" is the browser's online event when the network went, and a timer
  // when it did not: a listener that is restarting, or a proxy that is down,
  // fails the request the same way with the network up all along, and no
  // online event will ever come for that. So the account is asked for again
  // at the probe's slow pace until it answers - with the real thing, or with
  // a refusal that puts the sign-in gate up - and the probe itself rests
  // meanwhile, since its answer would be the same.
  var offlineRetryStop = function () { if (offline.retry) { clearTimeout(offline.retry); offline.retry = 0; } };
  var offlineRetryArm = function () {
    offlineRetryStop();
    offline.retry = setTimeout(function () { offline.retry = 0; if (offline.only) { load(true); } }, probeSlowly);
  };
  var enterOffline = function () {
    var owner = offlineOwner();
    if (!owner) { return; }
    probeStop();
    offlineRetryArm();
    // Already showing the kept mail: the retry is re-armed and nothing is
    // drawn again over what the reader is looking at.
    if (offline.only) { return; }
    offline.only = true;
    me = owner;
    el('who').textContent = owner;
    el('account-avatar').textContent = initialsOf(owner);
    el('signin').hidden = true;
    el('account').hidden = false;
    offlineNote(true);
    setProbeState(t('No connection'), true);
    metaGet('folders').then(function (tree) {
      if (tree) { renderFolders(tree); }
      if (pending) { var wanted = pending; pending = ''; replaceWith(wanted); } else { route(); }
    });
  };
  // The Settings card: the choice under the ceiling, what is kept and how
  // much, whether the window will fit, and a way to clear it.
  var renderOfflineCard = function () {
    var second = !offlineMine();
    el('offline-second').hidden = !second;
    if (second) {
      el('offline-second').textContent = tf('What is kept for reading offline belongs to {0}, the account you signed in as. Switch to it to change what is kept.', accounts.first.address);
      el('offline-form').hidden = true; el('offline-off').hidden = true; el('offline-clear').hidden = true; el('offline-room').hidden = true;
      el('offline-kept').textContent = '';
      return;
    }
    var select = el('pref-offline-days');
    clear(select);
    var steps = OFFLINE_STEPS.filter(function (n) { return n <= offline.ceiling; });
    if (offline.ceiling && steps.indexOf(offline.ceiling) < 0) { steps.push(offline.ceiling); }
    if (offline.days && steps.indexOf(offline.days) < 0) { steps.push(offline.days); }
    steps.sort(function (a, b) { return a - b; });
    var off = document.createElement('option'); off.value = '0'; off.textContent = t('Nothing beyond the inbox listing'); select.appendChild(off);
    steps.forEach(function (n) { var o = document.createElement('option'); o.value = String(n); o.textContent = tf('The last {0} days', n); select.appendChild(o); });
    select.value = String(offline.days);
    el('offline-form').hidden = !offline.ceiling;
    el('offline-off').hidden = !!offline.ceiling;
    el('offline-clear').hidden = !offline.kept;
    var kept = el('offline-kept');
    if (!offline.days) { kept.textContent = offline.kept ? tf('{0} messages are still kept here.', offline.kept) : ''; }
    else if (!offline.walked) { kept.textContent = tf('{0} messages are kept here, {1}.', offline.kept, format(offline.keptBytes)); }
    else { kept.textContent = tf('{0} of the {1} messages of the last {2} days are kept here, {3}.', offline.kept, offline.count, offline.days, format(offline.keptBytes)); }
    if (offline.quota && offline.days) { kept.textContent += ' ' + tf('This browser allows this site about {0}.', format(offline.quota.quota)); }
    var room = el('offline-room');
    var free = offline.quota ? Math.max(0, offline.quota.quota * 0.9 - offline.quota.usage + offline.keptBytes) : 0;
    var fits = !offline.quota || !offline.walked || offline.bytes * 1.5 <= free;
    room.hidden = !offline.days || fits;
    room.textContent = room.hidden ? '' : tf('The last {0} days are about {1} and this browser has room for about {2}: the newest are kept and the rest are not.', offline.days, format(offline.bytes), format(free));
  };
  el('offline-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var days = Number(el('pref-offline-days').value) || 0;
    call('PUT', '/api/v1/me/offline', { days: days }).then(function (result) {
      if (result.status !== 200 || !result.data) { say('offline-status', describe(result, t('Could not save the choice')), false); return; }
      offline.ceiling = Number(result.data.days_max) || 0;
      offline.days = Number(result.data.days) || 0;
      offline.since = offline.days ? String(result.data.since || '') : '';
      offline.walked = false;
      say('offline-status', offline.days ? t('Saved. The mail is being kept as you read, and in the background.') : t('Saved. Nothing more is kept in this browser.'), true);
      renderOfflineCard();
      if (offline.days) { offlineOwnerWrite(me); offlineFill(); }
      else { offlineOwnerWrite(''); offlineClear().then(renderOfflineCard); }
    });
  });
  // Clear: what is kept goes now, and the choice goes to nothing, so it does
  // not come straight back.
  el('offline-clear').addEventListener('click', function () {
    call('PUT', '/api/v1/me/offline', { days: 0 }).then(function (result) {
      if (result.status !== 200) { say('offline-status', describe(result, t('Could not save the choice')), false); return; }
      offline.days = 0; offline.since = ''; offline.walked = false;
      offlineOwnerWrite('');
      offlineClear().then(function () { renderOfflineCard(); say('offline-status', t('Cleared. Nothing more is kept in this browser until you choose again.'), true); });
    });
  });

  // ---- Later: send later, snooze, what is scheduled ----------------------------
  var pad2 = function (n) { return (n < 10 ? '0' : '') + n; };
  var stampOf = function (d) { return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()) + ' ' + pad2(d.getHours()) + ':' + pad2(d.getMinutes()); };
  var fromPicker = function (value) { return String(value || '').replace('T', ' ').slice(0, 16); };
  var setExportLink = function () {
    var id = Number(el('folder-export-target').value);
    el('folder-export-link').setAttribute('href', id ? '/api/v1/me/folders/' + id + '/export' : '#');
    el('folder-export-link').setAttribute('download', 'folder-' + (id || 0) + '.mbox');
  };
  el('folder-export-target').addEventListener('change', setExportLink);
  el('folder-import-go').addEventListener('click', function () {
    var id = Number(el('folder-import-target').value);
    var file = (el('folder-import-file').files || [])[0];
    if (!id || !file) { say('folder-import-status', t('Choose a folder and a .eml file.'), false); return; }
    var reader = new FileReader();
    reader.onload = function () {
      apiFetch('/api/v1/me/folders/' + id + '/messages', { method: 'POST', credentials: 'same-origin', cache: 'no-store',
        headers: { 'Content-Type': 'message/rfc822', 'X-Requested-With': 'hMailServer' }, body: String(reader.result) }).then(function (response) {
        if (response.status === 201) { say('folder-import-status', t('Imported into ') + folderName(id) + '.', true); el('folder-import-file').value = ''; lastListing = null; loadFolders(); return; }
        return response.text().then(function (text) { var why = ''; try { why = JSON.parse(text).error || ''; } catch (e) { why = ''; } say('folder-import-status', why || (t('Could not import (') + response.status + ')'), false); });
      });
    };
    reader.onerror = function () { say('folder-import-status', t('The file could not be read.'), false); };
    reader.readAsText(file);
  });
  el('compose-send-later').addEventListener('click', function () {
    if (expiryChosen()) { say('compose-status', t('A message that expires cannot be scheduled: send it now, or set Expires to Never.'), false); return; }
    var at = fromPicker(el('compose-later').value);
    if (at.length !== 16) { say('compose-status', t('Choose when to send it.'), false); return; }
    if (!el('compose-to').value.trim()) { say('compose-status', t('Say who it is to.'), false); return; }
    var body = composeBody();
    if (smimeSealedDraft(body)) { return; }
    if (draftId) { body.replace_id = draftId; }
    readFiles().then(function (files) {
      files = carried.concat(files);
      if (files.length) { body.attachments = files; }
      appendLinks(body);
      return call('POST', '/api/v1/me/drafts', body);
    }, function (why) { say('compose-status', why, false); return null; }).then(function (result) {
      if (!result) { return; }
      if (result.status !== 201 || !result.data) { say('compose-status', describe(result, t('Could not save the draft')), false); return; }
      var id = result.data.id;
      return call('POST', '/api/v1/me/drafts/' + id + '/schedule', { send_at: at }).then(function (scheduled) {
        if (scheduled.status === 201) {
          blankCompose(); composeKey = 'new:0';
          say('compose-status', t('Saved as a draft; it will be sent at ') + at + '.', true);
          loadFolders();
          return;
        }
        draftId = id; composeKey = 'draft:' + id;
        say('compose-status', describe(scheduled, t('Saved as a draft, but it could not be scheduled')), false);
      });
    });
  });
  var snoozeUntil = function (at) {
    if (!current) { return; }
    call('POST', '/api/v1/me/messages/' + current.id + '/snooze', { until: at }).then(function (result) {
      if (result.status === 200) { say('mail-status', t('Snoozed until ') + at + '.', true); current = null; lastListing = null; loadFolders(); go(listHash()); return; }
      say('mail-status', describe(result, t('Could not snooze')), false);
    });
  };
  el('message-snooze').addEventListener('click', function () { var m = el('snooze-menu'); m.hidden = !m.hidden; });
  var snoozeHours = function () { var n = Number(pref('snooze_hours')); return n > 0 && n <= 72 ? n : 3; };
  var snoozeHour = function () { var n = Number(pref('snooze_hour')); return n >= 0 && n <= 23 ? n : 9; };
  var renderSnoozePresets = function () {
    el('snooze-3h').textContent = tf('in {0} hours', snoozeHours());
    el('snooze-tomorrow').textContent = tf('tomorrow at {0}', snoozeHour());
    el('snooze-week').textContent = tf('next week at {0}', snoozeHour());
  };
  el('snooze-3h').addEventListener('click', function () { snoozeUntil(stampOf(new Date(Date.now() + snoozeHours() * 3600 * 1000))); });
  el('snooze-tomorrow').addEventListener('click', function () { var d = new Date(); d.setDate(d.getDate() + 1); d.setHours(snoozeHour(), 0, 0, 0); snoozeUntil(stampOf(d)); });
  el('snooze-week').addEventListener('click', function () { var d = new Date(); d.setDate(d.getDate() + 7); d.setHours(snoozeHour(), 0, 0, 0); snoozeUntil(stampOf(d)); });
  el('snooze-go').addEventListener('click', function () { var at = fromPicker(el('snooze-at').value); if (at.length === 16) { snoozeUntil(at); } else { say('mail-status', t('Choose when.'), false); } });
  // ---- Clean up conversation, as Outlook has it -----------------------------
  // The messages of a conversation whose whole text a later message of it
  // quotes are redundant: each is read, oldest first, and one whose every
  // line of three characters or more is found in a later one - the quote
  // markers and the spacing taken out of both, so a quote re-wrapped or
  // nested still counts - goes to Trash with the others, with Undo. The
  // newest message stays, as does one unread, starred or carrying an
  // attachment, since a reader may not have seen it and a quote carries no
  // file. Fifty messages at most are read for it. From a conversation row's
  // menu, or the open message's, whose conversation is the listing's.
  var cleanLines = function (text) {
    return String(text || '').split(/\r?\n/).map(function (line) { return line.replace(/^[\s>]+/, '').replace(/\s+/g, ' ').trim(); }).filter(function (line) { return line.length >= 3; });
  };
  var quotedIn = function (earlier, later) {
    var lines = cleanLines(earlier);
    if (!lines.length) { return false; }
    var haystack = cleanLines(later).join('\n');
    return lines.every(function (line) { return haystack.indexOf(line) >= 0; });
  };
  var hasAttachments = function (m) { return !!(m.has_attachments || (m.attachments && m.attachments.length)); };
  var cleanUpConversation = function (members) {
    var list = (members || []).filter(function (m) { return m && m.id; }).slice(0, 50);
    var nothing = function () { toast(t('Nothing to clean up: every message says something the later ones do not.')); };
    if (list.length < 2) { nothing(); return; }
    list.sort(function (a, b) { var da = dateOf(a), db = dateOf(b); return (da ? da.getTime() : 0) - (db ? db.getTime() : 0); });
    Promise.all(list.map(function (m) {
      return call('GET', '/api/v1/me/messages/' + m.id).then(function (result) { return result.status === 200 && result.data ? textOf(result.data) : null; });
    })).then(function (texts) {
      var redundant = [];
      list.forEach(function (m, i) {
        if (i === list.length - 1 || texts[i] === null || !m.flags || !m.flags.seen || m.flags.flagged || hasAttachments(m)) { return; }
        for (var j = i + 1; j < list.length; j++) {
          if (texts[j] !== null && quotedIn(texts[i], texts[j])) { redundant.push({ id: m.id, folderId: m.folder_id || state.folderId }); break; }
        }
      });
      if (!redundant.length) { nothing(); return; }
      var ids = redundant.map(function (e) { return e.id; });
      fileMany(redundant, 'delete').then(function (ok) {
        if (!ok) { return; }
        if (current && ids.indexOf(current.id) >= 0) { closeMessage(true); }
        lastListing = null;
        loadMessages();
      });
    });
  };
  // The open message's conversation: the listing's messages with its key, and itself.
  var conversationOfCurrent = function () {
    if (!current) { return []; }
    var page = lastListing && lastListing.page;
    var key = threadKey(current);
    var members = page ? page.messages.filter(function (x) { return threadKey(x) === key; }) : [];
    if (!members.some(function (x) { return x.id === current.id; })) { members.push(current); }
    return members;
  };
  el('message-cleanup').addEventListener('click', function () { closeMenus(); cleanUpConversation(conversationOfCurrent()); });
  // ---- Nudges: a reply or a follow-up that seems owed, as Gmail suggests ----
  // Computed from what a row already carries. A message received three to
  // thirty days ago, not answered, not from the reader, not muted, that
  // asked a question - a question mark in its subject or its first line -
  // is nudged to be replied to. A message in the Sent folder that old is
  // nudged to be followed up when nothing answered it, which the listing
  // cannot say: the page asks it of the server once, in_reply_to: for each
  // such message on the page joined by OR, and keeps the answer - a reply
  // once seen stays seen, the rest are asked about again after a while. A
  // preference turns nudges off. Nothing is nudged in a search.
  var NUDGE_DAYS_MIN = 3;
  var NUDGE_DAYS_MAX = 30;
  var nudgesOn = function () { return pref('nudges') !== '0'; };
  var daysOld = function (m) { var d = dateOf(m); return d ? Math.floor((Date.now() - d.getTime()) / 86400000) : -1; };
  var oldEnough = function (m) { var days = daysOld(m); return days >= NUDGE_DAYS_MIN && days <= NUDGE_DAYS_MAX; };
  var fromMe = function (m) { return !!me && addressOf(m.from || '').toLowerCase() === me; };
  var asksAQuestion = function (m) { return /\?/.test(m.subject || '') || /\?/.test(m.snippet || ''); };
  var messageIdOf = function (s) { return String(s || '').replace(/[<>]/g, '').trim().toLowerCase(); };
  var replied = {};
  var askedReplies = {};
  var nudgeFor = function (m, folderId) {
    if (!nudgesOn() || state.query || state.everywhere || !m.flags || m.flags.draft || !oldEnough(m)) { return ''; }
    if (folderIs(folderId, 'Sent')) {
      var id = messageIdOf(m.message_id);
      return id && !replied[id] ? tf('Sent {0} days ago. Follow up?', daysOld(m)) : '';
    }
    if (folderIs(folderId, 'Drafts') || folderIs(folderId, 'Junk') || folderIs(folderId, 'Trash')) { return ''; }
    if (m.flags.answered || fromMe(m) || isMuted(m) || !asksAQuestion(m)) { return ''; }
    return tf('Received {0} days ago. Reply?', daysOld(m));
  };
  var askReplies = function (page) {
    if (!nudgesOn() || state.query || state.everywhere || !folderIs(state.folderId, 'Sent')) { return; }
    var wanted = [];
    page.messages.forEach(function (m) {
      var id = messageIdOf(m.message_id);
      if (id && !replied[id] && oldEnough(m) && wanted.length < 16 && !(askedReplies[id] && Date.now() - askedReplies[id] < 5 * 60 * 1000)) { wanted.push(m.message_id); askedReplies[id] = Date.now(); }
    });
    if (!wanted.length) { return; }
    call('GET', '/api/v1/me/search?q=' + encodeURIComponent(wanted.map(function (id) { return 'in_reply_to:' + id; }).join(' OR ')) + '&limit=200').then(function (result) {
      if (result.status !== 200 || !result.data) { return; }
      var learned = false;
      (result.data.messages || []).forEach(function (r) {
        String((r.in_reply_to || '') + ' ' + (r.references || '')).split(/\s+/).forEach(function (ref) { var id = messageIdOf(ref); if (id && !replied[id]) { replied[id] = true; learned = true; } });
      });
      // The rows nudged a moment ago that turn out to have been answered lose the nudge, in place.
      if (!learned) { return; }
      listRows.forEach(function (r) { if (r.nudge && !nudgeFor(r.m, r.folderId)) { r.nudge.parentNode.removeChild(r.nudge); r.nudge = null; } });
    });
  };
  // ---- Follow-up flags: a flag with a date, as Outlook has them -------------
  // A follow-up is the star, the $FollowUp keyword and a $Due-YYYY-MM-DD
  // keyword beside it, all on the message: it travels with the message
  // wherever it is filed and whatever client moves it, a mail program that
  // does not know the two keywords shows the star and leaves them be, and
  // label:$FollowUp finds them. The Starred view lists the dated ones
  // first, the soonest at the top; a row says when one is due; and what is
  // due today or before is announced once a day.
  var FOLLOWUP = '$FollowUp';
  var DUE = '$Due-';
  var isFollowUpKeyword = function (k) { return String(k).toLowerCase() === FOLLOWUP.toLowerCase(); };
  var isDueKeyword = function (k) { return String(k).slice(0, DUE.length).toLowerCase() === DUE.toLowerCase() && /^\d{4}-\d{2}-\d{2}$/.test(String(k).slice(DUE.length)); };
  var keywordsOf = function (m) { return (m && m.flags && m.flags.keywords) || []; };
  var hasFollowUp = function (m) { return keywordsOf(m).some(isFollowUpKeyword); };
  var dueOf = function (m) { var found = keywordsOf(m).filter(isDueKeyword); return found.length ? found[0].slice(DUE.length) : ''; };
  var dueDayOf = function (d) { return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()); };
  var dueToday = function () { return dueDayOf(new Date()); };
  var dueText = function (day) {
    var d = new Date(day + 'T00:00:00');
    if (isNaN(d.getTime())) { return day; }
    try { return d.toLocaleDateString(languageActive, d.getFullYear() === new Date().getFullYear() ? { day: 'numeric', month: 'short' } : { year: 'numeric', month: 'short', day: 'numeric' }); } catch (e) { return day; }
  };
  // The badge a row carries: overdue, due today, or the day.
  var dueMark = function (m) {
    var day = dueOf(m);
    if (!day) { return null; }
    var now = dueToday();
    var s = node('span', day < now ? t('Overdue') : day === now ? t('Due today') : tf('Due {0}', dueText(day)), 'due' + (day < now ? ' late' : day === now ? ' now' : ''));
    s.setAttribute('title', day);
    return s;
  };
  var soonestDue = function (messages) {
    var best = null;
    messages.forEach(function (m) { var d = dueOf(m); if (d && (!best || d < dueOf(best))) { best = m; } });
    return best;
  };
  var byDue = function (messages) {
    var dated = messages.filter(function (m) { return !!dueOf(m); });
    dated.sort(function (a, b) { var x = dueOf(a), y = dueOf(b); return x < y ? -1 : x > y ? 1 : 0; });
    return dated.concat(messages.filter(function (m) { return !dueOf(m); }));
  };
  // The star, the keyword and the day in one change; a day already there
  // is taken off first. An empty day clears the flag, star and all - the
  // second and third stars' keywords with it, as taking the star off does -
  // and a follow-up on a message that shows no star gives it the first star,
  // never a third one left behind under a cleared flag.
  var setFollowUp = function (m, day) {
    var stale = keywordsOf(m).filter(isDueKeyword);
    var body = day ? { flagged: true, keywords_add: [FOLLOWUP, DUE + day], keywords_remove: stale.concat(starStage(m) === 0 ? STAR_KEYWORDS : []) } : { flagged: false, keywords_remove: stale.concat([FOLLOWUP], STAR_KEYWORDS) };
    return call('PUT', '/api/v1/me/messages/' + m.id + '/flags', body).then(function (result) {
      if (result.status !== 200 || !result.data || !result.data.flags) { say('mail-status', describe(result, t('Could not change the flags')), false); return false; }
      m.flags = result.data.flags;
      if (current && current.id === m.id) { current.flags = result.data.flags; renderActions(); }
      toast(day ? tf('Follow up by {0}.', dueText(day)) : t('Follow-up flag cleared.'));
      lastListing = null;
      loadFolders();
      if (!el('mail-section').hidden) { reloadKeepingPlace(); }
      return true;
    });
  };
  var followUpIn = function (days) { var d = new Date(); d.setDate(d.getDate() + days); return dueDayOf(d); };
  el('message-followup').addEventListener('click', function () { var menu = el('followup-menu'); menu.hidden = !menu.hidden; el('snooze-menu').hidden = true; });
  el('followup-today').addEventListener('click', function () { if (current) { setFollowUp(current, followUpIn(0)); } });
  el('followup-tomorrow').addEventListener('click', function () { if (current) { setFollowUp(current, followUpIn(1)); } });
  el('followup-week').addEventListener('click', function () { if (current) { setFollowUp(current, followUpIn(7)); } });
  el('followup-go').addEventListener('click', function () {
    var day = String(el('followup-at').value || '').slice(0, 10);
    if (current && /^\d{4}-\d{2}-\d{2}$/.test(day)) { setFollowUp(current, day); } else { say('mail-status', t('Choose when.'), false); }
  });
  el('followup-clear').addEventListener('click', function () { if (current) { setFollowUp(current, ''); } });
  // What is due today or before, asked of the server once a day - at
  // sign-in, and again when the probe sees the day turn - and announced
  // once per message: a toast that opens it, or the Starred view when there
  // are several, and a browser notification where those are on. Which were
  // announced is kept in this browser for the day, and forgotten at
  // sign-out.
  var DUE_SHOWN = 'hmPortalDueShown';
  var followUps = { day: '' };
  // Kept in this browser for the first account, whose everything it keeps
  // is; for the second account, in the page alone, with its token - so a
  // switch does not announce again what was announced, and nothing of the
  // second account is written where it would outlive the token.
  var dueShownRead = function () {
    if (!offlineMine()) { var marks = accounts.second.marks.due; return marks.day === dueToday() ? marks.ids : []; }
    try { var v = JSON.parse(localStorage.getItem(DUE_SHOWN) || 'null'); return v && v.day === dueToday() && Array.isArray(v.ids) ? v.ids : []; } catch (e) { return []; }
  };
  var dueShownWrite = function (ids) {
    if (!offlineMine()) { accounts.second.marks.due = { day: dueToday(), ids: ids }; return; }
    try { localStorage.setItem(DUE_SHOWN, JSON.stringify({ day: dueToday(), ids: ids })); } catch (e) { /* a browser that keeps nothing */ }
  };
  var dueShownForget = function () { try { localStorage.removeItem(DUE_SHOWN); } catch (e) { /* nothing to forget */ } };
  var followUpsCheck = function () {
    followUps.day = dueToday();
    return call('GET', '/api/v1/me/search?q=' + encodeURIComponent('label:' + FOLLOWUP) + '&limit=200').then(function (result) {
      if (result.status !== 200 || !result.data || el('account').hidden) { return; }
      var shown = dueShownRead();
      var due = (result.data.messages || []).filter(function (m) { var d = dueOf(m); return d && d <= dueToday() && shown.indexOf(m.id) < 0; });
      if (!due.length) { return; }
      dueShownWrite(shown.concat(due.map(function (m) { return m.id; })));
      var text = due.length === 1 ? tf('Due for follow-up: {0}', due[0].subject || t('(no subject)')) : tf('{0} messages are due for follow-up', due.length);
      if (due.length === 1) { toast(text, function () { go('/m/' + due[0].id); }, true, t('Open')); }
      else { toast(text, function () { go('/starred'); }, true, t('Show them')); }
      if (pref('notify') === '1' && 'Notification' in window && Notification.permission === 'granted') {
        try { new Notification(text, { tag: 'hm-followup' }); } catch (e) { /* a browser without notifications is still a browser */ }
      }
    });
  };
  var loadScheduled = function () {
    return call('GET', '/api/v1/me/scheduled').then(function (result) {
      if (result.status !== 200 || !result.data) { say('scheduled-status', describe(result, t('Could not read what is scheduled')), false); return; }
      var rows = el('scheduled-rows');
      clear(rows);
      var list = result.data.scheduled || [];
      list.forEach(function (s) {
        var tr = document.createElement('tr');
        tr.appendChild(node('td', s.action === 'send' ? t('Send') : t('Bring back')));
        tr.appendChild(node('td', s.subject || t('(no subject)')));
        tr.appendChild(node('td', s.at));
        var actions = document.createElement('td');
        var cancel = button(t('Cancel'));
        cancel.addEventListener('click', function () {
          call('DELETE', '/api/v1/me/scheduled/' + s.id).then(function (r) {
            if (r.status === 200) { say('scheduled-status', s.action === 'send' ? t('Cancelled; the draft stays.') : t('Brought back.'), true); loadScheduled(); lastListing = null; loadFolders(); }
            else { say('scheduled-status', describe(r, t('Could not cancel')), false); }
          });
        });
        actions.appendChild(cancel);
        tr.appendChild(actions);
        rows.appendChild(tr);
      });
      el('scheduled-empty').hidden = list.length > 0;
      el('scheduled-table').hidden = list.length === 0;
    });
  };

  el('filter-form').addEventListener('submit', function (event) {
    event.preventDefault();
    call('PUT', '/api/v1/me/filters', { script: el('filter-script').value }).then(function (result) {
      if (result.status === 200) { say('filter-status', el('filter-script').value.trim() ? t('Saved. The filter runs on every message that arrives from now on.') : t('Removed.'), true); return; }
      say('filter-status', describe(result, t('Could not save the filter')), false);
    });
  });
  el('password-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var now = el('current').value, fresh = el('new').value, again = el('confirm').value;
    if (fresh !== again) { say('password-status', t('The two new passwords differ.'), false); return; }
    call('POST', '/api/v1/me/password', { current: now, 'new': fresh }, secondFactorHeaders('otp', 'password-code')).then(function (result) {
      if (result.status === 200) {
        el('current').value = ''; el('new').value = ''; el('confirm').value = ''; el('otp').value = ''; el('password-code').value = '';
        smimeRewrap(now, fresh).then(function (note) {
          say('password-status', t('Password changed. Other browsers signed in to this account have been signed out.') + (note ? ' ' + note : ''), true);
          load(true);
        });
        return;
      }
      if (result.otp === 'required') { el('otp-row').hidden = false; askSecondFactor('otp', 'password-code', 'password-status'); return; }
      say('password-status', refusal(result, t('Could not change the password')), false);
    });
  });
  el('bulk-read').addEventListener('click', function () { eachSelected(function (id) { return call('PUT', '/api/v1/me/messages/' + id + '/flags', { seen: true }); }); });
  el('bulk-unread').addEventListener('click', function () { eachSelected(function (id) { return call('PUT', '/api/v1/me/messages/' + id + '/flags', { seen: false }); }); });
  el('bulk-archive').addEventListener('click', function () { fileSelected('archive'); });
  el('bulk-junk').addEventListener('click', function () { fileSelected(junkTargetFor(state.folderId)); });
  el('bulk-delete').addEventListener('click', function () { fileSelected('delete'); });
  el('bulk-move-go').addEventListener('click', function () {
    var target = Number(el('bulk-move').value);
    if (!target) { return; }
    eachSelected(function (id) { return call('POST', '/api/v1/me/messages/' + id + '/move', { folder_id: target }); });
  });
  el('bulk-clear').addEventListener('click', function () { clearSelection(); listRows.forEach(function (r) { r.box.checked = false; }); });
  // One call per message, in order, then one reload.
  var eachSelected = function (act) {
    return selectedEntries().then(function (entries) {
      var chain = Promise.resolve();
      entries.forEach(function (e) { chain = chain.then(function () { return act(e.id); }); });
      return chain.then(function () { clearSelection(); lastListing = null; loadFolders(); return loadMessages(); });
    }, function (r) { say('mail-status', describe(r, t('Could not read the folder')), false); });
  };
  document.addEventListener('keydown', function (event) {
    var tag = (event.target && event.target.tagName) || '';
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || event.ctrlKey || event.metaKey || event.altKey) { return; }
    if (el('account').hidden || el('mail-section').hidden || el('message-list').hidden) { return; }
    if (current && !el('message-view').hidden && (event.key === 'e' || event.key === '!' || event.key === '#' || event.key === 'm')) { return; }
    if (event.key === 'ArrowDown' || event.key === 'j') { setCursor(cursor + 1); event.preventDefault(); }
    else if (event.key === 'ArrowUp' || event.key === 'k') { setCursor(cursor - 1); event.preventDefault(); }
    else if (event.key === 'Enter' && cursor >= 0) { var hit = listRows[cursor]; if (hit.toggle) { hit.toggle(); } else { go('/m/' + hit.id); } event.preventDefault(); }
    else if (event.key === 'e' && cursor >= 0) { fileRow(listRows[cursor], 'archive'); event.preventDefault(); }
    else if (event.key === '!' && cursor >= 0) { fileRow(listRows[cursor], junkTargetFor(state.folderId)); event.preventDefault(); }
    else if (event.key === '#' && cursor >= 0) { fileRow(listRows[cursor], 'delete'); event.preventDefault(); }
    else if (event.key === 'm' && cursor >= 0) { muteThread(listRows[cursor], !isMuted(listRows[cursor].m)); event.preventDefault(); }
    else if (event.key === 'x' && cursor >= 0) { var r = listRows[cursor]; r.box.checked = !r.box.checked; (r.ids || [r.id]).forEach(function (id) { selected[id] = r.box.checked; }); anchorId = r.id; renderBulk(); event.preventDefault(); }
  });
  window.addEventListener('hashchange', route);
  // Registered after the router's own listener - listeners run in the
  // order they were added - so it runs once the view has been shown and
  // any field the view focused has the focus; otherwise the title takes it.
  window.addEventListener('hashchange', function () {
    var title = el('view-title');
    var active = document.activeElement;
    if (title && !el('account').hidden && (!active || active === document.body || active.tagName === 'BUTTON')) { title.focus(); }
  });
  // The skip link goes to the content itself, not through the router:
  // #content is no route, and the router would send it to the folder.
  el('skip').addEventListener('click', function (event) {
    event.preventDefault();
    var content = el('content');
    content.focus();
    if (content.scrollIntoView) { content.scrollIntoView(); }
  });
  document.addEventListener('visibilitychange', function () {
    if (!document.hidden) { probeNow(); return; }
    if (document.hidden && pref('notify') !== '1') { probeStop(); probe.paused = true; setProbeState('Paused - this tab is in the background', true); }
  });

  // A session from an earlier visit is still good until it has been idle
  // too long: try it first, and only ask for the password when it is not.
  // ---- Preferences ----------------------------------------------------------
  // Kept with the account (GET/PUT /api/v1/me/preferences); the browser's
  // storage holds only the theme, for the sign-in page before there is one.
  var prefs = {};
  var basePrefs = { theme: 'system', density: 'comfortable', undo_seconds: '5', notify: '0', notify_folders: '', view: 'threads', pane: 'right', inbox: 'tabs', tabs_by_sender: '', important_marker: 'auto', inbox_sections: '0', nudges: '1', timezone: '', swipe_right: 'archive', swipe_left: 'delete', snooze_hours: '3', snooze_hour: '9', reply_place: 'below', retention_trash_days: '0', retention_junk_days: '0', retention_archive_days: '0' };
  // The server's recovery window, learned the first time the list tools are
  // drawn: 0 means it keeps nothing, and the way in stays hidden.
  var recoverDays = 0;
  var recoverAsked = false;
  // Whether the server offers a message that expires, learned the same way.
  var confidentialOffered = false;
  var confidentialAsked = false;
  var renderListTools = function () {
    learnConfidential();
    if (!recoverAsked) {
      recoverAsked = true;
      call('GET', '/api/v1/me/recoverable').then(function (result) {
        recoverDays = result.status === 200 && result.data ? Number(result.data.days) || 0 : 0;
        el('folder-recover').hidden = !(!state.everywhere && folderIs(state.folderId, 'Trash') && recoverDays > 0);
      });
    }
    el('view-threads').textContent = pref('view') === 'threads' ? t('Show messages one by one') : t('Show conversations');
    var emptyable = !state.everywhere && (folderIs(state.folderId, 'Junk') || folderIs(state.folderId, 'Trash'));
    el('folder-empty').hidden = !emptyable;
    el('folder-recover').hidden = !(!state.everywhere && folderIs(state.folderId, 'Trash') && recoverDays > 0);
    el('folder-confidential').hidden = !(!state.everywhere && folderIs(state.folderId, 'Sent') && confidentialOffered);
    el('folder-empty').textContent = t('Empty this folder');
    ['right', 'bottom', 'off'].forEach(function (mode) { el('pane-' + mode).classList.toggle('on', paneMode() === mode); });
    el('mail-search-clear').hidden = !state.query;
  };
  el('view-threads').addEventListener('click', function () {
    savePrefs({ view: pref('view') === 'threads' ? 'messages' : 'threads' }).then(function () {
      renderListTools();
      if (lastListing && !el('mail-section').hidden && el('message-view').hidden) { renderMessages(lastListing.page); }
    });
  });
  // Two presses, as deleting a folder takes two: the second one empties.
  // Emptying goes in batches the server sizes: its answer's remaining says
  // whether to ask again. Bounded, so a folder that fills as fast as it
  // empties cannot keep this going for ever. Resolves to what was deleted
  // and whether a call failed, with the failing answer.
  var emptyFolder = function (folderId, days) {
    var deleted = 0;
    var path = '/api/v1/me/folders/' + folderId + '/empty' + (days === undefined ? '' : '?older_than_days=' + encodeURIComponent(days));
    var round = function (left) {
      return call('POST', path).then(function (result) {
        if (result.status !== 200 || !result.data) { return { deleted: deleted, failed: true, result: result }; }
        deleted += result.data.deleted || 0;
        if (result.data.remaining > 0 && left > 0) { return round(left - 1); }
        return { deleted: deleted, failed: false, result: result };
      });
    };
    return round(200);
  };
  el('folder-empty').addEventListener('click', function () {
    var b = el('folder-empty');
    if (b.getAttribute('data-armed') !== '1') { b.setAttribute('data-armed', '1'); b.textContent = t('Really empty?'); return; }
    b.removeAttribute('data-armed');
    emptyFolder(state.folderId).then(function (r) {
      if (!r.failed) { say('mail-status', r.deleted + t(' deleted.'), true); lastListing = null; loadFolders(); loadMessages(); }
      else { say('mail-status', describe(r.result, t('Could not empty the folder')) + (r.deleted ? ' ' + tf('{0} had gone first.', r.deleted) : ''), false); }
      b.textContent = t('Empty this folder');
    });
  });
  var pref = function (name) { return prefs[name] !== undefined ? prefs[name] : basePrefs[name]; };
  var undoSeconds = function () { var n = Number(pref('undo_seconds')); return isNaN(n) ? 5 : Math.min(30, Math.max(0, Math.round(n))); };
  var systemDark = window.matchMedia ? window.matchMedia('(prefers-color-scheme: dark)') : null;
  var applyTheme = function (choice) {
    var actual = choice === 'light' || choice === 'dark' ? choice : (systemDark && !systemDark.matches ? 'light' : 'dark');
    document.body.setAttribute('data-theme', actual);
    try { localStorage.setItem('hmPortalTheme', choice); } catch (e) { /* a browser that keeps nothing is still a browser */ }
  };
  var storedTheme = function () { try { return localStorage.getItem('hmPortalTheme') || 'system'; } catch (e) { return 'system'; } };
  if (systemDark && systemDark.addEventListener) { systemDark.addEventListener('change', function () { if (pref('theme') === 'system') { applyTheme('system'); } }); }
  var notifyFolderIds = function () {
    var raw = pref('notify_folders');
    if (!raw) { return inboxId ? [inboxId] : []; }
    return raw.split(',').map(Number).filter(function (n) { return n > 0; });
  };
  var notifyWanted = function (folderId) { return pref('notify') === '1' && notifyFolderIds().indexOf(folderId) >= 0; };
  var notifyNewMail = function (grew) {
    if (!grew.length || !('Notification' in window) || Notification.permission !== 'granted') { return; }
    grew.forEach(function (g) {
      try { new Notification('New mail in ' + (g.folder.name || g.folder.path), { body: g.added + ' new, ' + g.unseen + ' unread', tag: 'hm-folder-' + g.folder.id }); } catch (e) { /* a browser without notifications is still a browser */ }
    });
  };
  // ---- Web Push: a notification with the page closed --------------------------
  //
  // The browser makes the subscription and this page hands it to the server; the
  // server never gives it back, because the endpoint and the two keys together
  // are the capability to notify this device. So which row is THIS device is
  // remembered here, by id, and recovered by subscribing again - which the server
  // treats as the same device, since it is the same endpoint.
  var PUSH_ID = 'hmPortalPushId';
  var pushState = { enabled: false, key: '', list: [] };
  var pushIdRead = function () { try { return Number(localStorage.getItem(PUSH_ID)) || 0; } catch (e) { return 0; } };
  var pushIdWrite = function (id) { try { localStorage.setItem(PUSH_ID, String(id)); } catch (e) { /* a browser that keeps nothing */ } };
  var pushIdForget = function () { try { localStorage.removeItem(PUSH_ID); } catch (e) { /* nothing to forget */ } };
  var pushKeyBytes = function (value) {
    var padded = String(value || '').replace(/-/g, '+').replace(/_/g, '/');
    while (padded.length % 4) { padded += '='; }
    var raw = atob(padded);
    var bytes = new Uint8Array(raw.length);
    for (var i = 0; i < raw.length; i++) { bytes[i] = raw.charCodeAt(i); }
    return bytes;
  };
  var pushB64 = function (buffer) {
    var bytes = new Uint8Array(buffer);
    var text = '';
    for (var i = 0; i < bytes.length; i++) { text += String.fromCharCode(bytes[i]); }
    return btoa(text).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  };
  // "22:30" -> 1350, and back. -1 is "no quiet hours", which is an empty field.
  var pushMinutes = function (value) {
    var parts = /^(\d{1,2}):(\d{2})$/.exec(String(value || ''));
    if (!parts) { return -1; }
    var minutes = Number(parts[1]) * 60 + Number(parts[2]);
    return minutes >= 0 && minutes < 1440 ? minutes : -1;
  };
  var pushClock = function (minutes) {
    var n = Number(minutes);
    if (isNaN(n) || n < 0) { return ''; }
    return pad2(Math.floor(n / 60)) + ':' + pad2(n % 60);
  };
  // The three words the service worker shows, in the reader's language, left
  // where a worker can read them: a worker has no localStorage and cannot ask the
  // page anything while the page is closed, and the Cache Storage it already
  // uses for a share is shared with it.
  var pushWords = function () {
    if (typeof caches === 'undefined' || !caches.open) { return Promise.resolve(); }
    var words = { mail: t('New mail'), messages: tf('{0} new messages', '{0}'), test: t('A test notification') };
    return caches.open('hm-portal-push').then(function (c) {
      return c.put('/portal/push-words', new Response(JSON.stringify(words), { headers: { 'Content-Type': 'application/json' } }));
    }).catch(function () { /* a browser without Cache Storage still shows English */ });
  };
  var pushMine = function () {
    var id = pushIdRead();
    for (var i = 0; i < pushState.list.length; i++) { if (pushState.list[i].id === id) { return pushState.list[i]; } }
    return null;
  };
  // A name the person will recognise without being asked for one. The platform
  // and the browser family, which is what distinguishes a phone from a desktop.
  var pushDeviceName = function () {
    var agent = (navigator && navigator.userAgent) || '';
    var platform = /Android/i.test(agent) ? 'Android' : /iPhone|iPad|iPod/i.test(agent) ? 'iOS' :
      /Mac OS X/i.test(agent) ? 'Mac' : /Windows/i.test(agent) ? 'Windows' : /Linux/i.test(agent) ? 'Linux' : '';
    var browser = /Edg\//i.test(agent) ? 'Edge' : /OPR\//i.test(agent) ? 'Opera' : /Firefox\//i.test(agent) ? 'Firefox' :
      /Chrome\//i.test(agent) ? 'Chrome' : /Safari\//i.test(agent) ? 'Safari' : 'Browser';
    return (platform ? platform + ' ' : '') + browser;
  };
  var renderPush = function () {
    var rows = el('push-rows');
    if (!rows) { return; }
    clear(rows);
    pushState.list.forEach(function (s) {
      var tr = document.createElement('tr');
      tr.appendChild(node('td', s.device || ''));
      tr.appendChild(node('td', s.created ? new Date(s.created * 1000).toLocaleString() : ''));
      var last = document.createElement('td');
      last.textContent = s.last_sent ? new Date(s.last_sent * 1000).toLocaleString() : '';
      // A device nothing is reaching says so where a person will look, with the
      // push service's own reason: a phone that went quiet is otherwise a
      // mystery, and the row is kept for exactly that.
      if (s.state === 'dead-lettered') { last.appendChild(node('div', tf('Not being delivered: {0}', s.last_detail || ''), 'hint')); }
      tr.appendChild(last);
      var actions = document.createElement('td');
      var test = button(t('Test'));
      test.addEventListener('click', function () {
        call('POST', '/api/v1/me/push/test', { id: s.id }).then(function (r) {
          if (r.status === 200) { say('push-status', t('A test notification has been sent.'), true); }
          else { say('push-status', describe(r, t('Could not send a test notification')), false); }
          loadPush();
        });
      });
      var remove = button(t('Remove'));
      remove.addEventListener('click', function () { pushRemove(s.id); });
      actions.appendChild(test);
      actions.appendChild(remove);
      tr.appendChild(actions);
      rows.appendChild(tr);
    });
    el('push-table').hidden = pushState.list.length === 0;
    el('push-empty').hidden = pushState.list.length > 0;

    var mine = pushMine();
    el('push-form').hidden = !mine;
    el('push-subscribe').hidden = !!mine;
    if (mine) {
      el('push-detail').value = String(mine.detail);
      el('push-senders').value = mine.senders || '';
      el('push-labels').value = mine.labels || '';
      el('push-quiet-from').value = pushClock(mine.quiet_from);
      el('push-quiet-to').value = pushClock(mine.quiet_to);
    }
  };
  var loadPush = function () {
    if (!el('push-rows') || el('account').hidden) { return Promise.resolve(); }
    // The subscription this browser holds is the first account's: it is
    // never handed to the second account's server side, whose push routes
    // are not asked at all. The card says whose it is.
    el('push-second').hidden = !onSecond();
    if (onSecond()) {
      el('push-second').textContent = tf('Notifications on this device belong to {0}, the account you signed in as. Switch to it to change them.', accounts.first.address);
      el('push-form').hidden = true; el('push-subscribe').hidden = true; el('push-table').hidden = true; el('push-empty').hidden = true;
      return Promise.resolve();
    }
    return call('GET', '/api/v1/me/push').then(function (result) {
      if (result.status !== 200 || !result.data) { return; }
      pushState.enabled = !!result.data.enabled;
      pushState.key = result.data.vapid_public_key || '';
      pushState.list = result.data.subscriptions || [];
      renderPush();
      // A subscription this browser still holds whose row this page has lost
      // track of - a different browser profile, cleared storage - is handed back
      // to the server, which recognises the endpoint and answers with its id.
      if (!pushMine() && pushState.enabled && pushState.key) { pushRecover(); }
    });
  };
  var pushRegistration = function () {
    if (typeof navigator === 'undefined' || !navigator.serviceWorker || !navigator.serviceWorker.ready) { return Promise.resolve(null); }
    return navigator.serviceWorker.ready.catch(function () { return null; });
  };
  var pushSend = function (subscription, quiet) {
    var body = {
      endpoint: subscription.endpoint,
      keys: { p256dh: pushB64(subscription.getKey('p256dh')), auth: pushB64(subscription.getKey('auth')) },
      device: pushDeviceName(),
      quiet_offset: -new Date().getTimezoneOffset()
    };
    return call('POST', '/api/v1/me/push/subscriptions', body).then(function (r) {
      if (r.status === 201 && r.data && r.data.id) {
        pushIdWrite(r.data.id);
        if (!quiet) { say('push-status', t('Notifications are on for this device.'), true); }
        return loadPush();
      }
      if (!quiet) { say('push-status', describe(r, t('Could not turn notifications on')), false); }
      return null;
    });
  };
  var pushRecover = function () {
    return pushRegistration().then(function (registration) {
      if (!registration || !registration.pushManager) { return null; }
      return registration.pushManager.getSubscription();
    }).then(function (subscription) {
      return subscription ? pushSend(subscription, true) : null;
    }).catch(function () { /* nothing to recover */ });
  };
  var pushSubscribe = function () {
    say('push-status', '', true);
    if (typeof Notification === 'undefined' || typeof navigator === 'undefined' || !navigator.serviceWorker) {
      say('push-status', t('This browser cannot show notifications.'), false);
      return Promise.resolve();
    }
    if (!pushState.enabled) { say('push-status', t('Notifications are switched off on this server.'), false); return Promise.resolve(); }
    if (!pushState.key) { say('push-status', t('This server has no notification key yet. Ask your administrator to make one.'), false); return Promise.resolve(); }
    return Promise.resolve(Notification.requestPermission()).then(function (permission) {
      if (permission !== 'granted') {
        say('push-status', t('Notifications were refused for this site. Allow them in the browser and try again.'), false);
        return null;
      }
      return pushRegistration().then(function (registration) {
        if (!registration || !registration.pushManager) {
          say('push-status', t('This browser cannot show notifications.'), false);
          return null;
        }
        return registration.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: pushKeyBytes(pushState.key) });
      });
    }).then(function (subscription) {
      if (!subscription) { return null; }
      return pushWords().then(function () { return pushSend(subscription, false); });
    }).catch(function () {
      say('push-status', t('Could not turn notifications on'), false);
    });
  };
  var pushRemove = function (id) {
    return call('DELETE', '/api/v1/me/push/subscriptions/' + id).then(function (r) {
      if (r.status !== 200) { say('push-status', describe(r, t('Could not remove it')), false); return null; }
      if (id === pushIdRead()) {
        pushIdForget();
        say('push-status', t('This device will not be notified any more.'), true);
        // And the browser's own subscription, so that the next "turn it on"
        // makes a fresh one rather than handing back the one just removed.
        return pushRegistration().then(function (registration) {
          if (!registration || !registration.pushManager) { return null; }
          return registration.pushManager.getSubscription();
        }).then(function (subscription) { return subscription ? subscription.unsubscribe() : null; })
          .catch(function () { /* the row is gone either way */ })
          .then(function () { return loadPush(); });
      }
      return loadPush();
    });
  };
  if (el('push-subscribe')) {
    el('push-subscribe').addEventListener('click', function () { pushSubscribe(); });
    el('push-form').addEventListener('submit', function (e) {
      e.preventDefault();
      var mine = pushMine();
      if (!mine) { return; }
      var body = {
        detail: Number(el('push-detail').value),
        senders: el('push-senders').value,
        labels: el('push-labels').value,
        quiet_from: pushMinutes(el('push-quiet-from').value),
        quiet_to: pushMinutes(el('push-quiet-to').value),
        quiet_offset: -new Date().getTimezoneOffset()
      };
      call('PUT', '/api/v1/me/push/subscriptions/' + mine.id, body).then(function (r) {
        if (r.status === 200) { say('push-status', t('Saved.'), true); loadPush(); }
        else { say('push-status', describe(r, t('Could not save what this device is told')), false); }
      });
    });
  }

  var baseTitle = document.title || 'hMailServer - my account';
  var updateTitle = function () {
    var total = 0;
    notifyFolderIds().forEach(function (id) { var f = foldersById[id]; if (f) { total += f.unseen || 0; } });
    document.title = (total ? '(' + total + ') ' : '') + baseTitle;
    // The installed app's badge is the first account's, as the app is; the
    // tab's title follows whichever account is shown.
    if (offlineMine()) { updateBadge(total); }
  };
  var renderNotifyFolders = function () {
    var box = el('notify-folders');
    clear(box);
    var chosen = notifyFolderIds();
    allFolders.forEach(function (f) {
      var label = document.createElement('label'); label.className = 'inline';
      var tick = document.createElement('input'); tick.type = 'checkbox'; tick.value = String(f.id); tick.checked = chosen.indexOf(f.id) >= 0;
      label.appendChild(tick); label.appendChild(node('span', ' ' + f.path));
      box.appendChild(label);
    });
  };
  var applyPrefs = function () {
    applyTheme(pref('theme'));
    document.body.setAttribute('data-density', pref('density') === 'compact' ? 'compact' : 'comfortable');
    el('pref-theme').value = pref('theme') === 'light' || pref('theme') === 'dark' ? pref('theme') : 'system';
    el('pref-density').value = pref('density') === 'compact' ? 'compact' : 'comfortable';
    el('pref-pane').value = paneMode();
    applyPane();
    el('pref-reply-place').value = pref('reply_place') === 'above' ? 'above' : 'below';
    // The reader's own retention: a choice the page does not offer reads as Never.
    ['trash', 'junk', 'archive'].forEach(function (which) {
      var v = String(pref('retention_' + which + '_days') || '0');
      el('pref-retention-' + which).value = ['7', '30', '90', '365'].indexOf(v) >= 0 ? v : '0';
    });
    el('pref-undo').value = String(undoSeconds());
    if (!el('pref-undo').value) { el('pref-undo').value = '5'; }
    el('pref-notify').checked = pref('notify') === '1';
    el('pref-nudges').checked = nudgesOn();
    el('pref-inbox').value = inboxMode();
    el('pref-important').value = markerMode();
    el('pref-sections').value = sectionsOn() ? '1' : '0';
    el('pref-language').value = knownLanguage(pref('language') || '') || languageActive;
    fillZones();
    el('pref-timezone').value = pref('timezone') || '';
    el('pref-swipe-right').value = pref('swipe_right');
    el('pref-swipe-left').value = pref('swipe_left');
    el('pref-snooze-hours').value = String(snoozeHours());
    el('pref-snooze-hour').value = String(snoozeHour());
    renderSnoozePresets();
    renderNotifyFolders();
    renderTemplates();
    renderQuickSteps();
    renderSupport();
    refreshLabelColours();
    applyLanguagePref();
    updateTitle();
  };
  // ---- Quick steps: a button of the reader's own, as Outlook has them -------
  // Kept in the preferences as qs.<slug> -> {name, key, read, move, label,
  // forward}, so they follow the account between browsers. One runs on the
  // ticked messages, failing those on the open message, failing that on the
  // row under the cursor: mark as read, the label, the forward - the compose
  // window primed from the message, attachments and all, before anything
  // moves it - and then the move, whose toast has Undo as any filing's has.
  // A key from 1 to 9 runs it from the list.
  var quickSteps = function () {
    var list = [];
    Object.keys(prefs).forEach(function (k) {
      if (k.indexOf('qs.') !== 0) { return; }
      try {
        var v = JSON.parse(prefs[k]);
        if (v && v.name) {
          list.push({ key: k, name: String(v.name), shortcut: /^[1-9]$/.test(String(v.key || '')) ? String(v.key) : '', read: !!v.read, move: Number(v.move) || 0, label: String(v.label || ''), forward: String(v.forward || '') });
        }
      } catch (e) { /* a value that is not a quick step is left alone */ }
    });
    return list.sort(function (a, b) { return a.name.localeCompare(b.name); });
  };
  var quickStepByKey = function (digit) { return quickSteps().filter(function (s) { return s.shortcut === digit; })[0] || null; };
  var describeQuickStep = function (s) {
    var parts = [];
    if (s.read) { parts.push(t('Mark as read')); }
    if (s.label) { parts.push(t('Label') + ' ' + s.label); }
    if (s.forward) { parts.push(t('Forward to') + ' ' + s.forward); }
    if (s.move) { parts.push(t('Move to') + ' ' + folderName(s.move)); }
    return parts.join(', ');
  };
  var quickStepTargets = function () {
    if (selectedIds().length) { return selectedEntries(); }
    if (current && !el('message-view').hidden) { return Promise.resolve([{ id: current.id, folderId: current.folder_id }]); }
    var r = cursor >= 0 ? listRows[cursor] : null;
    if (r) { return Promise.resolve((r.ids || [r.id]).map(function (id) { return { id: id, folderId: r.folderId || state.folderId }; })); }
    return Promise.resolve([]);
  };
  // The compose window primed as Forward would prime it, addressed, and the
  // address bar told; the route then finds the form already holds it.
  var forwardTo = function (id, address) {
    composeKey = 'forward:' + id;
    return call('GET', '/api/v1/me/messages/' + id).then(function (result) {
      if (result.status !== 200 || !result.data) { say('mail-status', describe(result, t('Could not read the message being answered')), false); return null; }
      return prime('forward', result.data).then(function () {
        afterPrime('forward', result.data);
        el('compose-to').value = address;
        openCompose('forward', id);
        go('/compose?forward=' + id);
      });
    });
  };
  var runQuickStep = function (step) {
    quickStepTargets().then(function (entries) {
      if (!entries.length) { say('mail-status', t('Nothing to run it on.'), false); return; }
      var ids = entries.map(function (e) { return e.id; });
      var chain = Promise.resolve();
      if (step.read) { ids.forEach(function (id) { chain = chain.then(function () { return call('PUT', '/api/v1/me/messages/' + id + '/flags', { seen: true }); }); }); }
      if (step.label) { chain = chain.then(function () { return setKeywordOn(ids, step.label, true); }); }
      if (step.forward) { chain = chain.then(function () { return forwardTo(entries[0].id, step.forward); }); }
      chain.then(function () {
        if (step.move) {
          return fileMany(entries, step.move).then(function (ok) { if (ok && current && ids.indexOf(current.id) >= 0) { closeMessage(true); } });
        }
        toast(tf('Quick step {0} done.', step.name));
        return null;
      }).then(function () {
        clearSelection();
        lastListing = null;
        loadFolders();
        if (!el('mail-section').hidden) { reloadKeepingPlace(); }
      });
    }, function (r) { say('mail-status', describe(r, t('Could not read the folder')), false); });
  };
  var renderQuickSteps = function () {
    var list = quickSteps();
    var bar = el('quick-steps');
    clear(bar);
    list.forEach(function (s) {
      var b = node('button', s.name, 'textbtn'); b.type = 'button';
      b.setAttribute('title', describeQuickStep(s) + (s.shortcut ? ' (' + s.shortcut + ')' : ''));
      b.addEventListener('click', function () { runQuickStep(s); });
      bar.appendChild(b);
    });
    var rows = el('quickstep-rows');
    clear(rows);
    list.forEach(function (s) {
      var tr = document.createElement('tr');
      tr.appendChild(node('td', s.name));
      tr.appendChild(node('td', s.shortcut || t('None')));
      tr.appendChild(node('td', describeQuickStep(s)));
      var actions = document.createElement('td');
      var remove = button(t('Remove'));
      remove.addEventListener('click', function () { var change = {}; change[s.key] = null; savePrefs(change).then(function (ok) { if (ok) { say('quickstep-status', t('Removed.'), true); } }); });
      actions.appendChild(remove);
      tr.appendChild(actions);
      rows.appendChild(tr);
    });
    el('quickstep-empty').hidden = list.length > 0;
    el('quickstep-table').hidden = list.length === 0;
    // The folders a step may move to, the choice kept while they are redrawn.
    var move = el('quickstep-move');
    var chosen = move.value;
    var leave = node('option', t('Leave it where it is')); leave.value = '';
    fillFolderSelect(move, 0, leave);
    move.value = chosen || '';
  };
  el('quickstep-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var name = el('quickstep-name').value.trim().slice(0, 40);
    var label = el('quickstep-label').value.trim();
    var forward = el('quickstep-forward').value.trim();
    var move = Number(el('quickstep-move').value) || 0;
    var read = el('quickstep-read').checked;
    if (!name) { say('quickstep-status', t('Give the quick step a name.'), false); return; }
    if (label && !validLabel(label)) { say('quickstep-status', t('Give the label: one word, no spaces, quotes or brackets.'), false); return; }
    if (forward && forward.indexOf('@') < 1) { say('quickstep-status', t('Give an address to forward to.'), false); return; }
    if (!read && !label && !forward && !move) { say('quickstep-status', t('Give the quick step something to do.'), false); return; }
    var slug = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, 40) || 'step';
    var change = {};
    change['qs.' + slug] = JSON.stringify({ name: name, key: el('quickstep-key').value, read: read, move: move, label: label, forward: forward });
    savePrefs(change).then(function (ok) {
      if (!ok) { return; }
      say('quickstep-status', t('Saved.'), true);
      el('quickstep-name').value = ''; el('quickstep-key').value = ''; el('quickstep-read').checked = false; el('quickstep-move').value = ''; el('quickstep-label').value = ''; el('quickstep-forward').value = '';
    });
  });
  // ---- Templates: kept in the preferences as tpl.<slug> -> {name, subject, text}
  var templates = function () {
    var list = [];
    Object.keys(prefs).forEach(function (k) {
      if (k.indexOf('tpl.') !== 0) { return; }
      try { var v = JSON.parse(prefs[k]); if (v && v.name) { list.push({ key: k, name: v.name, subject: v.subject || '', text: v.text || '' }); } } catch (e) { /* a value that is not a template is left alone */ }
    });
    return list.sort(function (a, b) { return a.name.localeCompare(b.name); });
  };
  var renderTemplates = function () {
    var list = templates();
    var pick = el('template-pick');
    clear(pick);
    var none = node('option', t('Templates')); none.value = ''; pick.appendChild(none);
    list.forEach(function (tp) { var o = node('option', tp.name); o.value = tp.key; pick.appendChild(o); });
    var rows = el('template-rows');
    clear(rows);
    list.forEach(function (tp) {
      var tr = document.createElement('tr');
      tr.appendChild(node('td', tp.name));
      tr.appendChild(node('td', tp.subject));
      var actions = document.createElement('td');
      var remove = button(t('Remove'));
      remove.addEventListener('click', function () { var change = {}; change[tp.key] = null; savePrefs(change).then(function (ok) { if (ok) { say('template-status', t('Removed.'), true); } }); });
      actions.appendChild(remove);
      tr.appendChild(actions);
      rows.appendChild(tr);
    });
    el('template-empty').hidden = list.length > 0;
    el('template-table').hidden = list.length === 0;
  };
  var fillTemplate = function (text) {
    var first = (el('compose-to').value.split(',')[0] || '').replace(/<.*$/, '').replace(/"/g, '').trim().split(/\s+/)[0] || '';
    return text.replace(/\{first_name\}/g, first).replace(/\{subject\}/g, el('compose-subject').value).replace(/\{date\}/g, new Date().toLocaleDateString());
  };
  el('template-use').addEventListener('click', function () {
    var key = el('template-pick').value;
    var tp = templates().filter(function (x) { return x.key === key; })[0];
    if (!tp) { return; }
    if (!el('compose-subject').value && tp.subject) { el('compose-subject').value = fillTemplate(tp.subject); }
    var box = el('compose-text');
    box.value = fillTemplate(tp.text) + (box.value ? '\n' + box.value : '');
    box.focus();
  });
  el('template-save').addEventListener('click', function () {
    var name = window.prompt ? window.prompt('A name for this template') : '';
    if (!name) { return; }
    name = name.trim().slice(0, 60);
    var slug = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, 40) || 'template';
    var value = JSON.stringify({ name: name, subject: el('compose-subject').value.slice(0, 500), text: el('compose-text').value.slice(0, 3000) });
    var change = {};
    change['tpl.' + slug] = value;
    savePrefs(change).then(function (ok) { say('compose-status', ok ? t('Saved as a template.') : t('The template could not be saved.'), ok); });
  });
  var loadPrefs = function () {
    return call('GET', '/api/v1/me/preferences').then(function (result) {
      if (result.status === 200 && result.data) { prefs = result.data.preferences || {}; }
      applyPrefs();
    });
  };
  var savePrefs = function (changes) {
    return call('PUT', '/api/v1/me/preferences', changes).then(function (result) {
      if (result.status === 200 && result.data) { prefs = result.data.preferences || {}; applyPrefs(); return true; }
      say('prefs-status', describe(result, t('Could not save the preferences')), false);
      return false;
    });
  };
  // ---- Branding: the server's from the page itself, the domain's own after
  // sign-in; the announcement above everything; the title takes the name.
  var branding = { name: '', logo: '', announcement: '' };
  var applyBranding = function (b) {
    branding = { name: (b && b.name) || '', logo: (b && b.logo) || '', announcement: (b && b.announcement) || '' };
    if (branding.name) {
      el('signin-title').textContent = branding.name;
      document.title = branding.name + t(' - my account');
      baseTitle = document.title;
      updateTitle();
    }
    var logo = el('signin-logo');
    clear(logo);
    if (branding.logo && branding.logo.indexOf('data:image/') === 0) {
      var img = document.createElement('img'); img.setAttribute('src', branding.logo); img.setAttribute('alt', branding.name || 'Logo');
      logo.appendChild(img);
    } else {
      logo.appendChild(node('span', 'hM'));
    }
    el('signin-announcement').textContent = branding.announcement;
    el('signin-announcement').hidden = !branding.announcement;
    el('announcement').textContent = branding.announcement;
    el('announcement').hidden = !branding.announcement;
  };
  (function () {
    var data = null;
    try { data = el('branding-data'); } catch (e) { data = null; }
    if (data) { try { applyBranding(JSON.parse(data.textContent || '{}')); } catch (e) { /* not JSON: the defaults stay */ } }
  })();
  var loadBranding = function () {
    var domain = me.indexOf('@') > 0 ? me.slice(me.indexOf('@') + 1) : '';
    if (!domain) { return; }
    call('GET', '/api/v1/portal/branding?domain=' + encodeURIComponent(domain)).then(function (result) {
      if (result.status === 200 && result.data) { applyBranding(result.data); }
    });
  };

  // ---- Support access: the consent switch, the record of the last access.
  el('support-allowed').addEventListener('change', function () {
    savePrefs({ support_allowed: el('support-allowed').checked ? '1' : null }).then(function (ok) {
      if (ok) { say('support-status', el('support-allowed').checked ? t('An administrator may now open your mailbox for support.') : t('Support access is off.'), true); }
    });
  });
  el('support-note-dismiss').addEventListener('click', function () { savePrefs({ support_last: null }); });
  var renderSupport = function () {
    el('support-allowed').checked = pref('support_allowed') === '1';
    var last = pref('support_last') || '';
    el('support-last').textContent = last ? 'An administrator last opened this mailbox for support on ' + last + '.' : '';
    el('support-last').hidden = !last;
    el('support-note-text').textContent = last ? 'An administrator opened this mailbox for support on ' + last + '.' : '';
    el('support-note').hidden = !last;
  };

  // The browser is asked to send mailto: links here: registerProtocolHandler,
  // from a click as browsers require, the %s the link the browser hands
  // over, which the compose route reads. The browser asks the reader once.
  el('pref-mailto').addEventListener('click', function () {
    try {
      navigator.registerProtocolHandler('mailto', (location.origin || '') + '/portal#/compose?mailto=%s');
      say('prefs-status', t('This webmail will open mailto: links from now on.'), true);
    } catch (why) {
      say('prefs-status', t('This browser did not take the mailto: handler.'), false);
    }
  });
  el('prefs-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var ticked = [];
    var labels = el('notify-folders').children;
    for (var i = 0; i < labels.length; i++) { var tick = labels[i].children[0]; if (tick && tick.checked) { ticked.push(tick.value); } }
    var markerBefore = markerMode();
    savePrefs({ language: el('pref-language').value, timezone: el('pref-timezone').value, swipe_right: el('pref-swipe-right').value, swipe_left: el('pref-swipe-left').value, reply_place: el('pref-reply-place').value, retention_trash_days: el('pref-retention-trash').value, retention_junk_days: el('pref-retention-junk').value, retention_archive_days: el('pref-retention-archive').value, snooze_hours: el('pref-snooze-hours').value, snooze_hour: el('pref-snooze-hour').value, theme: el('pref-theme').value, density: el('pref-density').value, pane: el('pref-pane').value, undo_seconds: el('pref-undo').value,
                notify: el('pref-notify').checked ? '1' : '0', notify_folders: ticked.join(','), inbox: el('pref-inbox').value, important_marker: el('pref-important').value, inbox_sections: el('pref-sections').value, nudges: el('pref-nudges').checked ? '1' : '0' }).then(function (ok) {
      if (ok) {
        say('prefs-status', t('Saved.'), true);
        if (lastPage) { renderMessages(lastPage); }
        // The Important entry in the sidebar comes and goes with the marker.
        if (markerBefore !== markerMode()) { loadFolders(); }
        // A reply already open moves to the slot the reader just chose.
        if (!el('compose-section').hidden && el('compose-section').classList.contains('inline')) { placeCompose('inline'); }
      }
    });
  });
  el('pref-notify').addEventListener('change', function () {
    if (el('pref-notify').checked && 'Notification' in window && Notification.permission === 'default') { Notification.requestPermission(); }
  });

  // ---- Identities and the signature -----------------------------------------
  var loadIdentities = function () {
    return call('GET', '/api/v1/me/identities').then(function (result) {
      identities = (result.status === 200 && result.data && result.data.identities) || [];
      var select = el('compose-from');
      clear(select);
      identities.forEach(function (i) { var o = document.createElement('option'); o.value = i.address; o.textContent = i.header || i.address; select.appendChild(o); });
      var one = identities.length < 2;
      select.hidden = one; el('compose-from-label').hidden = one; el('compose-from-row').hidden = one;
      renderAwayAddresses();
    });
  };
  var renderAwayAddresses = function () {
    var list = el('away-addresses');
    clear(list);
    var kinds = { account: 'your address', alias: 'an alias that points here', granted: 'a mailbox that lets you write as it' };
    identities.forEach(function (i) {
      var li = document.createElement('li');
      li.appendChild(node('span', i.address));
      li.appendChild(node('span', kinds[i.kind] || i.kind, 'kind'));
      list.appendChild(li);
    });
    if (!identities.length) { list.appendChild(node('li', t('Not known yet.'), 'muted')); }
  };
  var selectIdentity = function (address) {
    var wanted = (address || '').toLowerCase();
    var found = identities.filter(function (i) { return i.address === wanted; })[0];
    el('compose-from').value = found ? found.address : (identities[0] ? identities[0].address : '');
  };
  // A reply goes out from the address the message came to, when that is one
  // of the account's identities; otherwise from the account itself.
  // The identity a message was addressed to: whole addresses, compared
  // whole, and the account's own wins when it is among them.
  var identityAddressedIn = function (message) {
    var addresses = ((message.to || '') + ',' + (message.cc || '')).split(/[,;]/).map(function (entry) {
      var m = /<([^>]+)>/.exec(entry);
      return (m ? m[1] : entry).trim().toLowerCase();
    }).filter(Boolean);
    if (addresses.indexOf(me) >= 0) { return ''; }
    var hit = identities.filter(function (i) { return i.kind !== 'account' && addresses.indexOf(i.address) >= 0; })[0];
    return hit ? hit.address : '';
  };
  var addSignature = function () {
    if (!signature.enabled || !signature.text) { return; }
    var box = el('compose-text');
    box.value = '\n\n-- \n' + signature.text + (box.value ? '\n' + box.value : '');
    if (box.setSelectionRange) { box.setSelectionRange(0, 0); }
  };
  var afterPrime = function (mode, message) {
    if (mode === 'draft') { selectIdentity(addressOf(message.from)); }
    else { selectIdentity(identityAddressedIn(message)); addSignature(); }
    syncComposeRows();
    el(richOn ? 'compose-editor' : 'compose-text').focus();
  };
  var composeTo = function (entry) {
    blankCompose();
    composeKey = newComposeKey();
    addSignature();
    el('compose-to').value = entry;
    go('/compose');
    el('compose-subject').focus();
  };

  // ---- Print ----------------------------------------------------------------
  el('message-print').addEventListener('click', function () { window.print(); });
  el('message-pin').addEventListener('click', function () { closeMenus(); if (current) { pinMessages([current.id], !isPinned(current)); } });
  el('message-mute').addEventListener('click', function () { closeMenus(); if (current) { muteThread(entryOfCurrent(), !isMuted(current)); } });
  el('message-block').addEventListener('click', function () { closeMenus(); if (current) { blockSender(addressOf(current.from)); } });
  el('message-sweep').addEventListener('click', function () { closeMenus(); if (current) { sweepSender(addressOf(current.from), current.folder_id || state.folderId); } });

  // ---- The shortcut list and the palette -------------------------------------
  var closeOverlays = function () { el('keys-overlay').hidden = true; el('palette').hidden = true; el('event-overlay').hidden = true; el('task-overlay').hidden = true; };
  var openKeys = function () { el('palette').hidden = true; el('keys-overlay').hidden = false; el('keys-close').focus(); };
  el('keys-close').addEventListener('click', closeOverlays);
  ['keys-overlay', 'palette'].forEach(function (id) { el(id).addEventListener('click', function (e) { if (e.target === el(id)) { closeOverlays(); } }); });
  // Every entry is something the page already has a button for; the palette
  // is the fast path to it, not a second feature set.
  var paletteItems = [];
  var paletteMarked = 0;
  var paletteActions = function () {
    var items = [];
    var add = function (label, kind, run) { items.push({ label: label, kind: kind, run: run }); };
    add('Write a message', 'action', function () { go('/compose'); });
    add('Held mail', 'page', function () { go('/held'); });
    add('Manage folders', 'page', function () { go('/folders'); });
    add('Settings', 'page', function () { go('/settings'); });
    add('Filters', 'page', function () { go('/filters'); });
    add('Contacts', 'page', function () { go('/contacts'); });
    add('Calendar', 'page', function () { go('/calendar'); });
    add('Tasks', 'page', function () { go('/tasks'); });
    add('New event', 'action', function () { go('/calendar'); openEvent(null, { day: wallKey(new Date()) }); });
    add('Away and forwarding', 'page', function () { go('/away'); });
    add('Storage', 'page', function () { go('/storage'); });
    add('Recover deleted items', 'page', function () { go('/recover'); });
    add('Messages that expire', 'page', function () { go('/confidential'); });
    add('Safe senders', 'page', function () { go('/senders'); });
    add('Subscriptions', 'page', function () { go('/subscriptions'); });
    add('Other accounts', 'page', function () { go('/accounts'); });
    add('Masked addresses', 'page', function () { go('/masked'); });
    add('Security', 'page', function () { go('/security'); });
    if (!directoryLinked) { add('Change password', 'page', function () { go('/password'); }); }
    add('Light or dark', 'action', function () { el('theme-btn').click(); });
    add('Keyboard shortcuts', 'action', openKeys);
    add('Sign out', 'action', function () { el('signout').click(); });
    var open = current && !el('mail-section').hidden && !el('message-view').hidden;
    if (open) {
      add('Reply', 'message', function () { el('message-reply').click(); });
      add('Reply all', 'message', function () { el('message-reply-all').click(); });
      add('Forward', 'message', function () { el('message-forward').click(); });
      add(current.flags.seen ? 'Mark as unread' : 'Mark as read', 'message', function () { el('message-unread').click(); });
      // Named for the star this entry will give; it presses the star over the
      // open message, as the entries around it press their own buttons, so it
      // acts on whatever is open when it is chosen rather than on the message
      // the palette was built for.
      add(['Star', 'Second star', 'Third star', 'Unstar'][starStage(current)], 'message', function () { el('message-flag').click(); });
      if (markable(current.folder_id)) { add(isImportant(current) ? 'Not important' : 'Mark as important', 'message', function () { el('message-important').click(); }); }
      add('Delete', 'message', function () { el('message-delete').click(); });
      add('Print', 'message', function () { window.print(); });
      allFolders.forEach(function (f) {
        if (f.id !== current.folder_id) { add('Move to ' + f.path, 'message', function () { el('message-move').value = String(f.id); el('message-move-go').click(); }); }
      });
    }
    allFolders.forEach(function (f) { add('Go to ' + f.path, 'folder', function () { go('/f/' + f.id); }); });
    return items;
  };
  var runPalette = function (item) { closeOverlays(); item.run(); };
  var markPalette = function (n) {
    if (!paletteItems.length) { return; }
    paletteMarked = (n + paletteItems.length) % paletteItems.length;
    var kids = el('palette-list').children;
    for (var i = 0; i < kids.length; i++) { kids[i].classList.toggle('on', i === paletteMarked); }
  };
  var renderPalette = function (items) {
    var list = el('palette-list');
    clear(list);
    paletteItems = items.slice(0, 12);
    paletteMarked = 0;
    paletteItems.forEach(function (item, i) {
      var li = document.createElement('li'); li.setAttribute('role', 'option');
      li.appendChild(node('span', item.label)); li.appendChild(node('span', item.kind, 'kind'));
      li.classList.toggle('on', i === 0);
      li.addEventListener('mousedown', function (e) { e.preventDefault(); runPalette(item); });
      list.appendChild(li);
    });
  };
  var paletteTimer = null;
  var filterPalette = function () {
    var q = el('palette-input').value.trim().toLowerCase();
    var words = q ? q.split(/\s+/) : [];
    var matches = function (label) { var l = label.toLowerCase(); return words.every(function (w) { return l.indexOf(w) >= 0; }); };
    var items = paletteActions().filter(function (i) { return matches(i.label); });
    renderPalette(items);
    if (paletteTimer) { clearTimeout(paletteTimer); paletteTimer = null; }
    if (q.length < 2) { return; }
    // "write to anna": the address book, as the To field completes from it.
    paletteTimer = setTimeout(function () {
      var wanted = q.replace(/^(write|compose|mail|to)\s+(to\s+)?/, '');
      call('GET', '/api/v1/me/contacts?limit=6&q=' + encodeURIComponent(wanted)).then(function (result) {
        if (el('palette').hidden || el('palette-input').value.trim().toLowerCase() !== q) { return; }
        var found = (result.status === 200 && result.data && result.data.contacts) || [];
        if (!found.length) { return; }
        var extra = found.filter(function (c) { return c.kind !== 'group' || (c.recipients || []).length; }).map(function (c) {
          var entry = c.kind === 'group' ? groupEntries(c) : (c.name ? c.name + ' <' + c.address + '>' : c.address);
          return { label: 'Write to ' + (c.kind === 'group' ? c.name : entry), kind: 'contact', run: function () { composeTo(entry + ', '); } };
        });
        renderPalette(items.concat(extra));
      });
    }, 150);
  };
  var openPalette = function () {
    el('keys-overlay').hidden = true;
    el('palette').hidden = false;
    el('palette-input').value = '';
    filterPalette();
    el('palette-input').focus();
  };
  el('palette-input').addEventListener('input', filterPalette);
  el('palette-input').addEventListener('keydown', function (e) {
    if (e.key === 'ArrowDown') { e.preventDefault(); markPalette(paletteMarked + 1); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); markPalette(paletteMarked - 1); }
    else if (e.key === 'Enter') { e.preventDefault(); if (paletteItems[paletteMarked]) { runPalette(paletteItems[paletteMarked]); } }
    else if (e.key === 'Escape') { e.preventDefault(); closeOverlays(); }
  });
  document.addEventListener('keydown', function (event) {
    if (el('account').hidden) { return; }
    if ((event.ctrlKey || event.metaKey) && !event.altKey && (event.key === 'k' || event.key === 'K')) { event.preventDefault(); openPalette(); return; }
    if (event.key === 'Escape' && (!el('keys-overlay').hidden || !el('palette').hidden)) { closeOverlays(); return; }
    var tag = (event.target && event.target.tagName) || '';
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || event.ctrlKey || event.metaKey || event.altKey) { return; }
    if (event.key === '?') { event.preventDefault(); if (el('keys-overlay').hidden) { openKeys(); } else { closeOverlays(); } }
  });


  // ---- The look: icons, avatars, names and times ---------------------------
  var SVG_NS = 'http://www.w3.org/2000/svg';
  var svgNode = function (tag) { return document.createElementNS ? document.createElementNS(SVG_NS, tag) : document.createElement(tag); };
  var icon = function (name, small) {
    var svg = svgNode('svg');
    svg.setAttribute('class', 'i' + (small ? ' sm' : ''));
    svg.setAttribute('aria-hidden', 'true');
    var use = svgNode('use');
    use.setAttribute('href', '#i-' + name);
    svg.appendChild(use);
    return svg;
  };
  var iconButton = function (name, title, className) {
    var b = node('button', undefined, 'iconbtn sm' + (className ? ' ' + className : ''));
    b.type = 'button';
    b.setAttribute('title', title);
    b.setAttribute('aria-label', title);
    b.appendChild(icon(name));
    return b;
  };
  // The <use> inside a button, by walking: the page's test harness has no
  // querySelector, on purpose.
  var findUse = function (n) {
    if (!n || !n.childNodes) { return null; }
    for (var i = 0; i < n.childNodes.length; i++) {
      var c = n.childNodes[i];
      if (String(c.tagName || '').toLowerCase() === 'use') { return c; }
      var deeper = findUse(c);
      if (deeper) { return deeper; }
    }
    return null;
  };
  var setIcon = function (holder, name) {
    var use = findUse(holder);
    if (use) { use.setAttribute('href', '#i-' + name); }
  };
  var nameOf = function (header) {
    var name = displayNameOf(header);
    if (name) { return name.replace(/^"|"$/g, ''); }
    var address = addressOf(header);
    return address || String(header || '').trim() || '?';
  };
  var initialsOf = function (header) {
    var name = nameOf(header).replace(/[<>"]/g, '').trim();
    if (!name || name === '?') { return '?'; }
    if (name.indexOf('@') >= 0) { name = name.split('@')[0]; }
    var parts = name.split(/[\s._-]+/).filter(Boolean);
    if (!parts.length) { return '?'; }
    return ((parts[0].charAt(0)) + (parts.length > 1 ? parts[parts.length - 1].charAt(0) : '')).toUpperCase();
  };
  var avatarColour = function (address) {
    var h = 0, s = String(address || '').toLowerCase();
    for (var i = 0; i < s.length; i++) { h = (h * 31 + s.charCodeAt(i)) >>> 0; }
    return 'var(--a' + ((h % 10) + 1) + ')';
  };
  // A brand's logo (BIMI) in the avatar, for mail the server judged dmarc=pass
  // for that brand and says so in brand; the initials stay underneath and come
  // back if the logo cannot be had - the server answers 404 when the domain
  // publishes none, its policy does not enforce, or the logo is not SVG Tiny PS.
  var brandInto = function (a, header, brand) {
    if (!brand) { return a; }
    var img = document.createElement('img');
    img.className = 'brand';
    img.setAttribute('alt', '');
    img.setAttribute('src', '/api/v1/me/brand-logo?domain=' + encodeURIComponent(brand));
    img.addEventListener('error', function () { if (img.parentNode === a) { a.removeChild(img); a.textContent = initialsOf(header); } });
    a.textContent = '';
    a.appendChild(img);
    a.setAttribute('title', brand);
    return a;
  };
  var avatarFor = function (header, large, brand) {
    var a = node('span', initialsOf(header), 'avatar' + (large ? ' lg' : ''));
    if (a.style) { a.style.background = avatarColour(addressOf(header)); }
    a.setAttribute('aria-hidden', 'true');
    return brandInto(a, header, brand);
  };
  var recipientsOf = function (header) {
    return String(header || '').split(',').map(function (s) { return s.trim() ? nameOf(s.trim()) : ''; }).filter(Boolean);
  };
  var dateOf = function (m) {
    var d = new Date(m.date || '');
    if (isNaN(d.getTime())) { d = new Date(String(m.received || '').replace(' ', 'T')); }
    return isNaN(d.getTime()) ? null : d;
  };
  // The reader's own zone, when one is chosen in Settings: every date and
  // time the page shows is rendered in it, and today and this year are
  // decided in it, so a reader away from home, or reading a server on
  // another continent, sees their own clock. Empty means the browser's.
  var zoneOptions = function (options) {
    var zone = pref('timezone');
    if (zone) { options.timeZone = zone; }
    return options;
  };
  var dayKey = function (d) { try { return d.toLocaleDateString('en-CA', zoneOptions({ year: 'numeric', month: '2-digit', day: '2-digit' })); } catch (e) { return d.toDateString(); } };
  // The time today, the day this year, the date otherwise - what a list shows.
  var whenText = function (m) {
    var d = dateOf(m);
    if (!d) { return m.date || m.received || ''; }
    var now = new Date();
    try {
      var day = dayKey(d), today = dayKey(now);
      if (day === today) { return d.toLocaleTimeString(languageActive, zoneOptions({ hour: '2-digit', minute: '2-digit', hour12: false })); }
      if (day.slice(0, 4) === today.slice(0, 4)) { return d.toLocaleDateString(languageActive, zoneOptions({ day: 'numeric', month: 'short' })); }
      return d.toLocaleDateString(languageActive, zoneOptions({ year: 'numeric', month: 'short', day: 'numeric' }));
    } catch (e) { return d.toDateString(); }
  };
  var fullDate = function (m) {
    var d = dateOf(m);
    if (!d) { return m.date || m.received || ''; }
    try { return d.toLocaleString(languageActive, zoneOptions({ weekday: 'short', year: 'numeric', month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' })); } catch (e) { return d.toString(); }
  };
  // The zones a browser knows, or a short list when it cannot say.
  var fillZones = function () {
    var select = el('pref-timezone');
    if (select.children.length > 1) { return; }
    var zones = [];
    try { zones = (typeof Intl !== 'undefined' && Intl.supportedValuesOf) ? Intl.supportedValuesOf('timeZone') : []; } catch (e) { zones = []; }
    if (!zones.length) { zones = ['UTC', 'Europe/London', 'Europe/Paris', 'Europe/Berlin', 'Europe/Madrid', 'Europe/Rome', 'Europe/Amsterdam', 'Europe/Stockholm', 'Europe/Warsaw', 'Europe/Athens', 'Europe/Kyiv', 'Europe/Moscow', 'America/New_York', 'America/Chicago', 'America/Denver', 'America/Los_Angeles', 'America/Sao_Paulo', 'Asia/Tokyo', 'Asia/Seoul', 'Asia/Shanghai', 'Asia/Kolkata', 'Australia/Sydney']; }
    var chosen = pref('timezone');
    if (chosen && zones.indexOf(chosen) < 0) { zones.push(chosen); }
    zones.forEach(function (z) { var o = document.createElement('option'); o.value = z; o.textContent = z; select.appendChild(o); });
  };

  // ---- Toasts, with undo ----------------------------------------------------
  var toastTimer = 0;
  var dismissToast = function () {
    var box = el('toasts');
    if (box) { clear(box); }
    if (toastTimer && typeof clearTimeout === 'function') { clearTimeout(toastTimer); }
    toastTimer = 0;
  };
  var toast = function (text, undo, sticky, label) {
    var box = el('toasts');
    if (!box) { say('mail-status', text, true); return null; }
    clear(box);
    var one = node('div', undefined, 'toast');
    one.setAttribute('role', 'status');
    one.appendChild(node('span', text));
    one.appendChild(node('span', undefined, 'sp'));
    if (undo) {
      var u = node('button', label || t('Undo')); u.type = 'button';
      u.addEventListener('click', function () { dismissToast(); undo(); });
      one.appendChild(u);
    }
    var x = node('button', undefined, 'x'); x.type = 'button'; x.setAttribute('aria-label', t('Dismiss'));
    x.appendChild(icon('close', true));
    x.addEventListener('click', dismissToast);
    one.appendChild(x);
    box.appendChild(one);
    if (toastTimer && typeof clearTimeout === 'function') { clearTimeout(toastTimer); }
    toastTimer = sticky ? 0 : setTimeout(function () { clear(box); toastTimer = 0; }, 8000);
    return one;
  };
  var filingText = function (to, result, count) {
    var what;
    if (typeof to === 'number') { what = t('moved to ') + folderName(to); }
    else if (to === 'delete') { what = result && result.data && result.data.deleted ? t('deleted') : t('moved to Trash'); }
    else { what = to === 'archive' ? t('archived') : to === 'junk' ? t('moved to Junk') : to === 'inbox' ? t('moved to Inbox') : to === 'trash' ? t('moved to Trash') : t('deleted'); }
    if (count > 1) { return tf('{0} {1}.', count, what); }
    return what.charAt(0).toUpperCase() + what.slice(1) + '.';
  };
  // One call per message, in order; then a toast whose Undo moves each one
  // back where it was, by the id the move gave it.
  var fileMany = function (entries, to) {
    var moved = [];
    var last = null;
    var chain = Promise.resolve();
    entries.forEach(function (e) {
      chain = chain.then(function () {
        return fileMessage(e.id, to).then(function (r) {
          last = r;
          if (r.status === 200 && r.data && r.data.id && e.folderId) { moved.push({ id: r.data.id, back: e.folderId }); }
        });
      });
    });
    return chain.then(function () {
      if (!last || last.status !== 200) { say('mail-status', describe(last || { status: 0 }, t('Could not file the message')), false); return false; }
      say('mail-status', '', true);
      lastListing = null;
      loadFolders();
      var undo = moved.length ? function () {
        var c = Promise.resolve();
        moved.forEach(function (x) { c = c.then(function () { return call('POST', '/api/v1/me/messages/' + x.id + '/move', { folder_id: x.back }); }); });
        c.then(function () { lastListing = null; loadFolders(); if (!el('mail-section').hidden) { loadMessages(); } });
      } : null;
      toast(filingText(to, last, entries.length), undo);
      return true;
    });
  };
  var fileSelected = function (to) {
    selectedEntries().then(function (entries) {
      var ids = entries.map(function (e) { return e.id; });
      return fileMany(entries, to).then(function (ok) {
        clearSelection();
        if (ok && current && ids.indexOf(current.id) >= 0) { closeMessage(true); }
        loadMessages();
      });
    }, function (r) { say('mail-status', describe(r, t('Could not read the folder')), false); });
  };
  // ---- Every message of the folder, not only the page ----------------------
  // Ticking the page's box offers the rest of the folder, as both webmails
  // do; taken up, the bulk actions read the folder page by page - at most
  // five thousand messages - and act on all of it, or on the open tab of it.
  var selectionScope = function () {
    return lastPage && tabsActive(lastPage) ? tabName(activeTab) : folderName(state.folderId);
  };
  var renderSelectNote = function () {
    var note = el('select-note');
    var page = lastListing && lastListing.key === listingKey() ? lastListing.page : null;
    var all = el('select-all');
    if (!page || page.query || state.everywhere || !all || !all.checked || !listRows.length || !(page.total > page.messages.length)) { wholeFolder = false; note.hidden = true; return; }
    if (wholeFolder) {
      el('select-note-text').textContent = tf('Every message in {0} is selected.', selectionScope());
      el('select-folder').textContent = t('Clear selection');
    } else {
      el('select-note-text').textContent = tf('All {0} messages on this page are selected.', selectedIds().length);
      el('select-folder').textContent = tf('Select every message in {0}', selectionScope());
    }
    note.hidden = false;
  };
  el('select-folder').addEventListener('click', function () {
    if (wholeFolder) { clearSelection(); listRows.forEach(function (r) { r.box.checked = false; }); return; }
    wholeFolder = true;
    renderBulk();
  });
  var selectedEntries = function () {
    if (!wholeFolder) {
      return Promise.resolve(selectedIds().map(function (id) {
        var folderId = state.folderId;
        listRows.forEach(function (r) { if (r.id === id || (r.ids || []).indexOf(id) >= 0) { folderId = r.folderId || folderId; } });
        return { id: id, folderId: folderId };
      }));
    }
    var folderId = state.folderId;
    var tab = lastPage && tabsActive(lastPage) ? activeTab : '';
    var entries = [], seen = 0, before = 0;
    var step = function () {
      return call('GET', '/api/v1/me/folders/' + folderId + '/messages?limit=200' + (before ? '&before_uid=' + before : '')).then(function (r) {
        if (r.status !== 200 || !r.data) { return Promise.reject(r); }
        var ms = r.data.messages || [];
        seen += ms.length;
        ms.forEach(function (m) { if (!tab || tabOf(m) === tab) { entries.push({ id: m.id, folderId: folderId }); } });
        if (!ms.length || seen >= (r.data.total || 0) || entries.length >= 5000 || !ms[ms.length - 1].uid) { return entries; }
        before = ms[ms.length - 1].uid;
        return step();
      });
    };
    return step();
  };
  var markRows = function (entries, seen) {
    var chain = Promise.resolve();
    entries.forEach(function (e) {
      (e.ids || [e.id]).forEach(function (id) {
        chain = chain.then(function () { return call('PUT', '/api/v1/me/messages/' + id + '/flags', { seen: seen }); });
      });
    });
    chain.then(function () { lastListing = null; loadFolders(); reloadKeepingPlace(); });
  };
  // Three stars, as Gmail has them: the star, then amber, then red, then
  // none again. The first is the IMAP flag every client shows; the second
  // and third are the keywords $Star2 and $Star3 on top of it, which other
  // clients ignore and the Starred view includes all the same.
  //
  // One function gives a message its next star, and every way this page has
  // of starring goes through it: the star on a row and the star over the
  // open message, s on either, the row's menu and the palette. Each press is
  // one request carrying the flag and both keywords - the stage's keyword
  // added and the other taken off, or both taken off with the flag - so a
  // message never keeps a star keyword its star no longer shows. Nothing
  // else may edit these two keywords: the label menu offers the reader's
  // labels alone (ownKeyword, above), so the star is the only way to a star.
  var STAR_KEYWORDS = ['$Star2', '$Star3'];
  var isStarKeyword = function (k) { return /^\$star[23]$/i.test(String(k)); };
  var starStage = function (m) {
    if (!m.flags.flagged) { return 0; }
    var keys = (m.flags.keywords || []).map(function (k) { return k.toLowerCase(); });
    return keys.indexOf('$star3') >= 0 ? 3 : keys.indexOf('$star2') >= 0 ? 2 : 1;
  };
  var starTitle = function (stage) { return stage === 0 ? t('Star') : stage === 1 ? t('Second star') : stage === 2 ? t('Third star') : t('Unstar'); };
  var paintStar = function (button, stage) {
    button.classList.toggle('on', stage > 0);
    button.classList.toggle('s2', stage === 2);
    button.classList.toggle('s3', stage === 3);
    button.setAttribute('title', starTitle(stage));
    // The name read out is the star the press will give, on a row as over
    // the open message. aria-label wins over title, so a row left at "Star"
    // announced the wrong thing for every star but the first.
    button.setAttribute('aria-label', starTitle(stage));
  };
  // The change that puts a message at a stage: 0 none, 1 the flag, 2 and 3
  // the flag and that stage's keyword.
  var starChange = function (stage) {
    var keep = stage >= 2 ? STAR_KEYWORDS[stage - 2] : '';
    var change = { flagged: stage > 0, keywords_remove: STAR_KEYWORDS.filter(function (k) { return k !== keep; }) };
    if (keep) { change.keywords_add = [keep]; }
    return change;
  };
  // The flag and the keywords a change left, put on every copy of the
  // message the page holds - its row, the conversation it heads, the open
  // message - and painted there; the other flags (seen, answered) stay as
  // each copy had them.
  var showStar = function (m, flagged, keywords, button) {
    var take = function (copy) { if (copy && copy.flags && copy.id === m.id) { copy.flags.flagged = flagged; copy.flags.keywords = keywords.slice(); } };
    take(m);
    listRows.forEach(function (r) {
      (r.ms || [r.m]).forEach(take);
      if (r.star && r.m && r.m.id === m.id) { paintStar(r.star, starStage(r.m)); }
    });
    if (button) { paintStar(button, starStage(m)); }
    if (current && current.id === m.id) { take(current); renderActions(); }
  };
  // The presses for one message are counted and sent in turn. Counting each
  // from the star the message SHOWS would lose the second of two quick
  // presses - the key repeating while it is held, or a double press - since
  // the page's copy of the message changes only when the answer arrives: both
  // would ask for the first star and the message would stop there, the second
  // press swallowed. So each press counts from the star the press before it
  // asked for, and a press whose predecessor was refused is dropped rather
  // than sent for a star the message never reached.
  var starPending = {};
  var cycleStar = function (m, button) {
    if (!m || !m.flags) { return Promise.resolve(); }
    var id = m.id;
    var pending = starPending[id] || { stage: starStage(m), queue: Promise.resolve(), waiting: 0, failed: false };
    starPending[id] = pending;
    pending.stage = (pending.stage + 1) % 4;
    pending.waiting += 1;
    var stage = pending.stage;
    var sent = pending.queue.then(function () {
      if (pending.failed) { return; }
      var change = starChange(stage);
      return call('PUT', '/api/v1/me/messages/' + id + '/flags', change).then(function (result) {
        if (result.status !== 200) {
          pending.failed = true;
          say('mail-status', describe(result, t('Could not change the flags')), false);
          return;
        }
        // The server's answer when it gave one; otherwise what was asked for.
        var flags = result.data && result.data.flags;
        var keywords = flags ? (flags.keywords || []) : (m.flags.keywords || []).filter(function (k) { return !isStarKeyword(k); }).concat(change.keywords_add || []);
        showStar(m, flags ? !!flags.flagged : change.flagged, keywords, button);
        if (stage === 1) { learnStarred(m); }
        lastListing = null;
      });
    });
    // However a press ended, it gives its place in the queue up. A press
    // whose work threw stops the presses already behind it - they would ask
    // for a star the message never reached - but the queue itself settles,
    // because a queue left rejected would swallow every later press of that
    // message for the life of the page and the star would simply stop
    // working. The count is kept the same way, so the next press starts
    // again from the star the message shows.
    var done = function () {
      pending.waiting -= 1;
      if (!pending.waiting && starPending[id] === pending) { delete starPending[id]; }
    };
    pending.queue = sent.then(null, function () { pending.failed = true; });
    return sent.then(done, done);
  };
  var starButton = function (m) {
    var stage = starStage(m);
    var star = node('button', undefined, 'star' + (stage > 0 ? ' on' : '') + (stage === 2 ? ' s2' : '') + (stage === 3 ? ' s3' : ''));
    star.type = 'button';
    star.setAttribute('title', starTitle(stage));
    star.setAttribute('aria-label', starTitle(stage));
    star.appendChild(icon('star'));
    star.addEventListener('click', function (event) { event.stopPropagation(); cycleStar(m, star); });
    return star;
  };

  // ---- The reading pane -----------------------------------------------------
  var paneMode = function () { var p = pref('pane'); return p === 'bottom' || p === 'off' ? p : 'right'; };
  var applyPane = function () {
    var s = el('mail-section');
    var mode = paneMode();
    s.classList.toggle('has-pane', mode !== 'off');
    s.classList.toggle('pane-bottom', mode === 'bottom');
    s.classList.toggle('no-pane', mode === 'off');
  };
  // The message leaves the pane; quietly, the address bar is put back to the
  // list without a history entry (the message is gone), otherwise it goes
  // back the way the reader came.
  var dropMessage = function () {
    if (!el('compose-section').hidden && el('compose-section').classList.contains('inline')) { placeCompose('dock'); }
    current = null;
    el('message-view').hidden = true;
    el('pane-empty').hidden = false;
    el('mail-section').classList.remove('has-message');
  };
  var closeMessage = function (quiet) {
    current = null;
    el('message-view').hidden = true;
    el('pane-empty').hidden = false;
    el('mail-section').classList.remove('has-message');
    listRows.forEach(function (r) { r.row.classList.remove('open'); });
    if (parseHash().parts[0] !== 'm') { return; }
    if (!quiet && cameFromList && history.length > 1) { history.back(); return; }
    replaceWith(listHash());
  };
  var currentNavHash = function () {
    if (state.everywhere) { return state.query === 'is:flagged' ? '/starred' : state.query === IMPORTANT_QUERY ? '/important' : ''; }
    return state.folderId ? '/f/' + state.folderId : '';
  };
  var renderCount = function (page) {
    var count = el('list-count');
    var older = el('page-older');
    var newer = el('page-newer');
    var n = page.messages.length;
    if (page.query) {
      count.textContent = n ? tf('{0} results', n) : '';
      older.hidden = true; newer.hidden = true;
      return;
    }
    count.textContent = n ? (state.before ? tf('{0} older, of {1}', n, page.total) : tf('1-{0} of {1}', n, page.total)) : '';
    older.hidden = !(n && page.total > n);
    newer.hidden = !state.before;
    pagerOlderUid = n ? page.messages[n - 1].uid : 0;
  };
  var pagerOlderUid = 0;
  el('page-older').addEventListener('click', function () { if (pagerOlderUid) { state.before = pagerOlderUid; go(listHash()); } });
  el('page-newer').addEventListener('click', function () { state.before = 0; go(listHash()); });
  el('list-refresh').addEventListener('click', function () { lastListing = null; loadFolders(); loadMessages(); });
  el('list-mark-all-read').addEventListener('click', function () { closeMenus(); markRows(listRows.filter(function (r) { return !r.m || !r.m.flags.seen || r.ids; }), true); });
  el('message-details-toggle').addEventListener('click', function () {
    var d = el('message-details');
    d.hidden = !d.hidden;
    el('message-details-toggle').setAttribute('aria-expanded', d.hidden ? 'false' : 'true');
  });
  ['right', 'bottom', 'off'].forEach(function (mode) {
    el('pane-' + mode).addEventListener('click', function () {
      closeMenus();
      savePrefs({ pane: mode }).then(function () { applyPane(); renderListTools(); if (current) { el('message-view').hidden = false; el('pane-empty').hidden = true; } });
    });
  });

  // ---- Menus that open under a button and close on a click elsewhere --------
  var within = function (target, ancestor) { var n = target; while (n) { if (n === ancestor) { return true; } n = n.parentNode; } return false; };
  var menus = [['list-more-btn', 'list-more'], ['message-more-btn', 'message-more'], ['select-menu-btn', 'select-menu'], ['account-btn', 'account-menu'], ['search-tune', 'search-advanced']];
  var closeMenus = function () {
    menus.forEach(function (pair) { var m = el(pair[1]); if (m && !m.hidden) { m.hidden = true; el(pair[0]).setAttribute('aria-expanded', 'false'); } });
  };
  menus.forEach(function (pair) {
    el(pair[0]).addEventListener('click', function (event) {
      event.stopPropagation();
      var menu = el(pair[1]);
      var open = menu.hidden;
      closeMenus();
      menu.hidden = !open;
      el(pair[0]).setAttribute('aria-expanded', open ? 'true' : 'false');
      if (open && pair[1] === 'search-advanced') { renderSavedSearches(); el('adv-from').focus(); }
    });
    el(pair[1]).addEventListener('click', function (event) { event.stopPropagation(); });
  });
  document.addEventListener('click', function (event) {
    var open = menus.filter(function (pair) { return !el(pair[1]).hidden; });
    if (!open.length) { return; }
    var inside = open.some(function (pair) { return within(event.target, el(pair[1])) || within(event.target, el(pair[0])); });
    if (!inside) { closeMenus(); }
  });
  el('select-menu').addEventListener('click', function (event) {
    var pick = event.target && event.target.getAttribute ? event.target.getAttribute('data-pick') : '';
    if (!pick) { return; }
    closeMenus();
    listRows.forEach(function (r) {
      var messages = r.ms || [r.m];
      var take = pick === 'all' ? true : pick === 'none' ? false :
        pick === 'read' ? messages.every(function (m) { return m.flags.seen; }) :
        pick === 'unread' ? messages.some(function (m) { return !m.flags.seen; }) :
        pick === 'starred' ? messages.some(function (m) { return m.flags.flagged; }) :
        messages.every(function (m) { return !m.flags.flagged; });
      (r.ids || [r.id]).forEach(function (id) { selected[id] = take; });
      r.box.checked = take;
    });
    renderBulk();
  });
  el('select-all').addEventListener('click', function () {
    var take = el('select-all').checked;
    listRows.forEach(function (r) { (r.ids || [r.id]).forEach(function (id) { selected[id] = take; }); r.box.checked = take; });
    renderBulk();
  });
  el('menu-settings').addEventListener('click', function () { closeMenus(); go('/settings'); });
  el('menu-security').addEventListener('click', function () { closeMenus(); go('/security'); });
  el('settings-btn').addEventListener('click', function () { go('/settings'); });
  // ---- The tour ----------------------------------------------------------
  // Teaching the webmail while somebody uses it. One mechanism, the same shape
  // as the desktop Control Panel's and the Control Deck's: a step is an element
  // (by its id), one sentence, an optional condition that ends the step when
  // the thing has happened, and what to do when the element is not on the
  // screen - skip it, or end the tour saying so. A tour is an ordered list of
  // those with a name and a version.
  //
  // Four properties of it are the feature rather than decoration, and each is
  // checked by build/portal-script-test.js:
  //
  //   * it is never modal - the ring takes no pointer events and the card is
  //     the only thing on its layer that does, so everything underneath, the
  //     message being pointed at included, stays usable;
  //   * it never takes the keyboard - nothing is focused when a step opens, and
  //     the card's three buttons are ordinary tab stops;
  //   * a step whose element is not on the screen is skipped rather than waited
  //     on, because this page hides half of itself depending on what the reader
  //     is doing;
  //   * where it got to is kept with the ACCOUNT rather than with the browser,
  //     so a reader who starts the walk on a laptop finishes it on a phone.
  //
  // Every sentence goes through the catalogues like every other text here.
  var TOURS = [{
    id: 'firstrun',
    name: K('Show me around'),
    version: 1,
    steps: [
      { target: 'pane-empty', mail: true, until: 'message-open',
        text: K('Choose a message in the list and it opens here beside it. The More menu moves this pane, or turns it off.') },
      { target: 'nav-compose', mail: true, until: 'compose-open',
        text: K('Compose opens a window you can keep writing in while you read something else.') },
      { target: 'label-nav', mail: true,
        text: K('Labels are your own way of marking mail. A message can carry several, and it stays in its folder.') },
      { target: 'mail-search', missing: 'end',
        text: K('Search takes words, and from:, is:unread and has:attachment. The magnifier beside it builds one for you.') }
    ]
  }];
  // What a step can wait for: the thing the reader is being asked to try.
  var TOUR_CONDITIONS = {
    'message-open': function () { return !el('message-view').hidden; },
    'compose-open': function () { return !el('compose-section').hidden; }
  };
  var tourFind = function (id) {
    for (var i = 0; i < TOURS.length; i++) { if (TOURS[i].id === id) { return TOURS[i]; } }
    return null;
  };
  // An element counts as on the screen when neither it nor anything between it
  // and the body is hidden. This page hides by the hidden attribute
  // throughout, so that is the whole question. The walk stops at the body on
  // purpose: document.hidden is the Page Visibility API - true whenever the
  // tab is in the background - and a tour that decided its own steps were
  // missing because the reader had switched tabs would skip half of itself.
  var tourVisible = function (id) {
    var found = el(id);
    var node = found;
    while (node && node !== document.body) {
      if (node.hidden) { return null; }
      node = node.parentNode;
    }
    return found;
  };
  // Which tours this account has finished, and where it stopped in one it left.
  // Kept in the account's preferences rather than in this browser, so it
  // follows the reader; the value is one line - "firstrun=1|firstrun@2v1" -
  // which is the form the Control Panel keeps in the registry and the Deck in
  // localStorage, so all three can be read with one explanation. Every kind of
  // damage is survivable and silent: the worst it costs is being offered a walk
  // twice.
  var tourProgress = function () {
    var out = { finished: {}, stopped: {} };
    var halves = String(pref('tours') || '').split('|');
    halves[0].split(',').forEach(function (pair) {
      var at = pair.indexOf('=');
      if (at <= 0) { return; }
      var version = parseInt(pair.slice(at + 1), 10);
      if (version > 0) { out.finished[pair.slice(0, at)] = version; }
    });
    (halves[1] || '').split(',').forEach(function (pair) {
      var at = pair.indexOf('@');
      if (at <= 0) { return; }
      var rest = pair.slice(at + 1), mark = rest.indexOf('v');
      if (mark <= 0) { return; }
      var step = parseInt(rest.slice(0, mark), 10), version = parseInt(rest.slice(mark + 1), 10);
      if (step > 0 && version > 0) { out.stopped[pair.slice(0, at)] = { step: step, version: version }; }
    });
    return out;
  };
  var tourSaveProgress = function (progress) {
    var finished = Object.keys(progress.finished).sort().map(function (id) { return id + '=' + progress.finished[id]; }).join(',');
    var stopped = Object.keys(progress.stopped).sort().map(function (id) { return id + '@' + progress.stopped[id].step + 'v' + progress.stopped[id].version; }).join(',');
    return savePrefs({ tours: finished + '|' + stopped });
  };
  var tourResumeAt = function (walk) {
    var stop = tourProgress().stopped[walk.id];
    return stop && stop.version === walk.version && stop.step < walk.steps.length ? stop.step : 0;
  };
  // The tour running now: the tour, the step it is on, and what that step's
  // condition said when it opened. The last one is why a step does not vanish
  // in front of a reader who already had a message open - it is the CHANGE
  // that ends a step, not the state.
  var tourNow = null;
  var tourTimer = null;
  var tourCondition = function (step) {
    var test = step && step.until ? TOUR_CONDITIONS[step.until] : null;
    try { return !!test && !!test(); } catch (e) { return false; }
  };
  // The element a step points at, with the mail view opened first where the
  // step lives there: an element on a panel nobody has opened is hidden, and a
  // hidden element is one there is nothing to point at.
  var tourElement = function (step) {
    // replaceWith rather than go: it applies the route at once, where setting
    // the address applies it on a later turn, and a step asked about one turn
    // too early would be skipped for living on a panel that is merely not
    // shown YET. It also keeps the tour out of the back button, which is right
    // - opening the list to say something about it is not somewhere the reader
    // asked to go.
    if (step.mail && el('mail-section').hidden) { replaceWith(listHash()); }
    return tourVisible(step.target);
  };
  // From this step onwards, the first that can be shown. A step whose element
  // is missing is skipped; one marked missing 'end' stops the tour instead,
  // which is for the step whose absence makes the rest meaningless.
  var tourSettle = function (from) {
    for (var at = from; at < tourNow.walk.steps.length; at++) {
      var step = tourNow.walk.steps[at];
      if (tourElement(step)) { tourNow.at = at; tourNow.entry = tourCondition(step); return true; }
      if (step.missing === 'end') { tourNow.at = at; tourEnd('missing'); return false; }
    }
    tourEnd('finished');
    return false;
  };
  var tourPlace = function () {
    if (!tourNow) { return; }
    var step = tourNow.walk.steps[tourNow.at];
    var node = tourVisible(step.target);
    var ring = el('tour-ring'), card = el('tour-card');
    if (!node || !node.getBoundingClientRect) { ring.hidden = true; return; }
    var at = node.getBoundingClientRect();
    if (!at.width && !at.height) { ring.hidden = true; return; }
    ring.hidden = false;
    ring.style.left = at.left + 'px'; ring.style.top = at.top + 'px';
    ring.style.width = at.width + 'px'; ring.style.height = at.height + 'px';
    var height = card.offsetHeight || 190;
    var room = typeof window === 'undefined' || typeof window.innerHeight !== 'number' ? 900 : window.innerHeight;
    var wide = typeof window === 'undefined' || typeof window.innerWidth !== 'number' ? 1280 : window.innerWidth;
    var top = at.bottom + 12;
    if (top + height > room) { top = Math.max(8, at.top - 12 - height); }
    card.style.top = Math.max(8, Math.min(top, Math.max(8, room - height - 8))) + 'px';
    card.style.left = Math.max(8, Math.min(at.left, Math.max(8, wide - 346))) + 'px';
  };
  var tourDraw = function () {
    var step = tourNow.walk.steps[tourNow.at];
    el('tour-name').textContent = t(tourNow.walk.name);
    el('tour-step').textContent = tf('Step {0} of {1}', tourNow.at + 1, tourNow.walk.steps.length);
    el('tour-text').textContent = t(step.text);
    el('tour-back').hidden = tourNow.at === 0;
    el('tour-next').textContent = tourNow.at === tourNow.walk.steps.length - 1 ? t('Finish') : t('Next');
    el('tour-card').hidden = false;
    tourPlace();
  };
  var tourTick = function () {
    if (!tourNow) { if (tourTimer) { clearInterval(tourTimer); tourTimer = null; } return; }
    tourPlace();
    var step = tourNow.walk.steps[tourNow.at];
    if (step.until && !tourNow.entry && tourCondition(step)) { tourNext(); }
  };
  var tourStart = function (id, at) {
    var walk = tourFind(id);
    if (!walk || el('account').hidden) { return; }
    tourNow = { walk: walk, at: 0, entry: false };
    var from = typeof at === 'number' && at >= 0 ? Math.min(at, walk.steps.length - 1) : tourResumeAt(walk);
    if (!tourSettle(from)) { return; }
    tourDraw();
    if (!tourTimer) { tourTimer = setInterval(tourTick, 400); }
  };
  var tourNext = function () {
    if (!tourNow) { return; }
    if (tourNow.at + 1 >= tourNow.walk.steps.length) { tourEnd('finished'); return; }
    if (tourSettle(tourNow.at + 1)) { tourDraw(); }
  };
  var tourBack = function () {
    if (!tourNow) { return; }
    for (var at = tourNow.at - 1; at >= 0; at--) {
      if (tourElement(tourNow.walk.steps[at])) {
        tourNow.at = at; tourNow.entry = tourCondition(tourNow.walk.steps[at]); tourDraw(); return;
      }
    }
    // Nowhere to go back to is standing still, never closing the tour: Back is
    // how somebody re-reads a sentence, and a Back that ended it would be a trap.
    tourDraw();
  };
  // Every ending says something. Silence after a tour that stopped on its own
  // is how a reader decides the feature is broken.
  var tourEnd = function (why) {
    if (!tourNow) { return; }
    var walk = tourNow.walk, at = tourNow.at;
    var progress = tourProgress();
    if (why === 'finished') {
      progress.finished[walk.id] = Math.max(progress.finished[walk.id] || 0, walk.version);
      delete progress.stopped[walk.id];
    } else if (why === 'left' && at > 0) {
      progress.stopped[walk.id] = { step: at, version: walk.version };
    } else {
      delete progress.stopped[walk.id];
    }
    tourSaveProgress(progress);
    tourNow = null;
    if (tourTimer) { clearInterval(tourTimer); tourTimer = null; }
    el('tour-card').hidden = true;
    el('tour-ring').hidden = true;
    if (why === 'finished') { toast(tf('That is the end of {0}. You can start it again whenever you like.', t(walk.name))); }
    else if (why === 'missing') { toast(tf('{0} stops here: the next thing it points at is not on this screen.', t(walk.name))); }
    else { toast(tf('{0} closed. It picks up where you left off.', t(walk.name))); }
  };
  el('menu-tour').addEventListener('click', function () { closeMenus(); tourStart('firstrun', -1); });
  el('tour-next').addEventListener('click', function () { tourNext(); });
  el('tour-back').addEventListener('click', function () { tourBack(); });
  el('tour-skip').addEventListener('click', function () { tourEnd('left'); });
  // Escape leaves the tour only when the keyboard is already inside its card.
  // Everywhere else Escape still belongs to the page - it closes a menu, it
  // goes back to the list - and a tour that ate it would be exactly the "in the
  // way of the keyboard" this promises not to be.
  document.addEventListener('keydown', function (event) {
    if (!tourNow || event.key !== 'Escape') { return; }
    var active = document.activeElement;
    if (active === el('tour-next') || active === el('tour-back') || active === el('tour-skip')) { tourEnd('left'); }
  });

  el('keys-btn').addEventListener('click', function () { openKeys(); });
  el('storage-link').addEventListener('click', function () { go('/storage'); });
  (function () {
    var nav = el('side-nav').children;
    for (var i = 0; i < nav.length; i++) {
      (function (b) { b.addEventListener('click', function () { go(b.getAttribute('data-route')); }); })(nav[i]);
    }
  })();
  el('nav-toggle').addEventListener('click', function () { el('account').classList.toggle('nav-open'); });
  el('nav-scrim').addEventListener('click', function () { el('account').classList.remove('nav-open'); });
  el('side').addEventListener('click', function (event) {
    var n = event.target;
    while (n && n !== el('side')) { if (n.tagName === 'BUTTON') { el('account').classList.remove('nav-open'); return; } n = n.parentNode; }
  });

  // ---- The search box's options ---------------------------------------------
  var quoteTerm = function (value) { value = value.trim(); return /\s/.test(value) ? '"' + value.replace(/"/g, '') + '"' : value; };
  var advancedQuery = function () {
    var parts = [];
    var words = el('adv-words').value.trim();
    if (words) { parts.push(words); }
    if (el('adv-from').value.trim()) { parts.push('from:' + quoteTerm(el('adv-from').value)); }
    if (el('adv-to').value.trim()) { parts.push('to:' + quoteTerm(el('adv-to').value)); }
    if (el('adv-subject').value.trim()) { parts.push('subject:' + quoteTerm(el('adv-subject').value)); }
    if (el('adv-label').value.trim()) { parts.push('label:' + el('adv-label').value.trim()); }
    if (el('adv-after').value) { parts.push('after:' + el('adv-after').value); }
    if (el('adv-before').value) { parts.push('before:' + el('adv-before').value); }
    var size = el('adv-size').value.replace(/\s+/g, '');
    if (size) { parts.push((el('adv-size-kind').value === 'smaller' ? 'smaller:' : 'larger:') + size); }
    if (el('adv-age').value) { parts.push('newer_than:' + el('adv-age').value); }
    if (el('adv-attachment').checked) { parts.push('has:attachment'); }
    if (el('adv-unread').checked) { parts.push('is:unread'); }
    if (el('adv-flagged').checked) { parts.push('is:flagged'); }
    return parts.join(' ');
  };
  // ---- Saved searches -------------------------------------------------------
  // A search kept under a name, in the account's preferences so it follows
  // the reader between browsers, and listed among the folders: opening one
  // runs the search again, so it is always current, which is what Outlook's
  // search folders are. Twenty at most, as many as a navigation can carry.
  var savedSearches = function () {
    try {
      var list = JSON.parse(pref('saved_searches') || '[]');
      return Array.isArray(list) ? list.filter(function (s) { return s && s.name && s.q; }) : [];
    } catch (why) { return []; }
  };
  var savedSearchRoute = function (s, i) { return '/search?q=' + encodeURIComponent(s.q) + '&saved=' + i; };
  var writeSavedSearches = function (list) {
    return savePrefs({ saved_searches: JSON.stringify(list.slice(0, 20)) }).then(function (ok) { if (ok) { return loadFolders(); } });
  };
  var renderSavedSearches = function () {
    var box = el('adv-saved');
    clear(box);
    var list = savedSearches();
    box.hidden = !list.length;
    list.forEach(function (s, i) {
      var open = node('button', s.name, 'btn ghost sm'); open.type = 'button';
      open.addEventListener('click', function () { closeMenus(); go(savedSearchRoute(s, i)); });
      var remove = iconButton('close', t('Remove') + ': ' + s.name);
      remove.addEventListener('click', function () { writeSavedSearches(list.filter(function (x, j) { return j !== i; })).then(renderSavedSearches); });
      var row = node('span', undefined, 'saved-search'); row.appendChild(open); row.appendChild(remove);
      box.appendChild(row);
    });
  };
  el('adv-save').addEventListener('click', function () {
    var q = advancedQuery() || el('mail-search').value.trim();
    var name = el('adv-save-name').value.trim();
    if (!q || !name) { el('adv-save-name').focus(); return; }
    var list = savedSearches().filter(function (s) { return s.name !== name; });
    list.push({ name: name, q: q });
    el('adv-save-name').value = '';
    writeSavedSearches(list).then(renderSavedSearches);
  });
  el('adv-search').addEventListener('click', function () {
    var q = advancedQuery();
    el('mail-search').value = q;
    el('mail-search-everywhere').checked = el('adv-where').value === 'all';
    closeMenus();
    if (!q) { return; }
    runSearch(q);
  });
  el('adv-reset').addEventListener('click', function () {
    ['adv-from', 'adv-to', 'adv-subject', 'adv-words', 'adv-label', 'adv-after', 'adv-before', 'adv-size', 'adv-age'].forEach(function (id) { el(id).value = ''; });
    ['adv-attachment', 'adv-unread', 'adv-flagged'].forEach(function (id) { el(id).checked = false; });
    el('adv-where').value = 'folder';
    el('adv-size-kind').value = 'larger';
  });
  el('mail-search').addEventListener('input', function () { el('mail-search-clear').hidden = !el('mail-search').value && !state.query; });
  window.addEventListener('hashchange', function () { el('mail-search-clear').hidden = !state.query && !el('mail-search').value; });

  // ---- Compose: Cc and Bcc appear when asked for, or when they hold something
  var syncComposeRows = function () {
    el('compose-cc-row').hidden = !el('compose-cc').value;
    el('compose-bcc-row').hidden = !el('compose-bcc').value;
    el('show-cc').hidden = !el('compose-cc-row').hidden;
    el('show-bcc').hidden = !el('compose-bcc-row').hidden;
  };
  el('show-cc').addEventListener('click', function () { el('compose-cc-row').hidden = false; el('show-cc').hidden = true; el('compose-cc').focus(); });
  el('show-bcc').addEventListener('click', function () { el('compose-bcc-row').hidden = false; el('show-bcc').hidden = true; el('compose-bcc').focus(); });

  // ---- The keys a mail client has, on top of the list's own ----------------
  document.addEventListener('keydown', function (event) {
    var tag = (event.target && event.target.tagName) || '';
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || event.ctrlKey || event.metaKey || event.altKey) { return; }
    if (el('account').hidden || !el('keys-overlay').hidden || !el('palette').hidden) { return; }
    if (event.key === '/') { event.preventDefault(); el('mail-search').focus(); return; }
    if (event.key === 'c') { event.preventDefault(); go('/compose'); return; }
    if (el('mail-section').hidden) { return; }
    // A digit runs the quick step given that key.
    if (/^[1-9]$/.test(event.key)) { var step = quickStepByKey(event.key); if (step) { event.preventDefault(); runQuickStep(step); } return; }
    var open = current && !el('message-view').hidden;
    if (event.key === 'u' || (event.key === 'Escape' && open)) { if (open) { event.preventDefault(); closeMessage(false); } return; }
    if (open && event.key === 'r') { event.preventDefault(); go('/compose?reply=' + current.id); return; }
    if (open && event.key === 'a') { event.preventDefault(); go('/compose?replyall=' + current.id); return; }
    if (open && event.key === 'f') { event.preventDefault(); go('/compose?forward=' + current.id); return; }
    // One key, one meaning: the next star, for the open message as for the
    // row under the cursor - what the star beside either of them does.
    if (event.key === 's') {
      event.preventDefault();
      var row = cursor >= 0 ? listRows[cursor] : null;
      if (open) { cycleStar(current); } else if (row) { cycleStar(row.m, row.star); }
      return;
    }
    // The marker's keys, Gmail's: + or = marks the open message, or else the
    // row under the cursor, important; - marks it not important.
    if (event.key === '+' || event.key === '=' || event.key === '-') {
      var wanted = event.key !== '-';
      var under = cursor >= 0 ? listRows[cursor] : null;
      if (open && markable(current.folder_id)) { event.preventDefault(); markImportant([current], wanted); }
      else if (!open && under && markable(under.folderId)) { event.preventDefault(); markImportant(under.ms || [under.m], wanted); }
      return;
    }
    if (event.key === 'o' && cursor >= 0 && listRows[cursor] && !el('message-list').hidden) { event.preventDefault(); go('/m/' + listRows[cursor].id); return; }
    // With a reading pane, moving the cursor reads the message it lands on.
    if ((event.key === 'j' || event.key === 'k' || event.key === 'ArrowDown' || event.key === 'ArrowUp') && paneMode() !== 'off' && open && cursor >= 0 && listRows[cursor] && listRows[cursor].id !== current.id) {
      replaceWith('/m/' + listRows[cursor].id);
    }
  });


  // ---- The compose window: docked over the mail, or under the message it answers
  var composeMode = 'new';
  var composeId = 0;
  // Where an inline reply sits is the reader's choice (discussion #266): under
  // the message it answers, which is Gmail's shape and the default, or above the
  // whole conversation, which is Outlook's. Nothing about the sent message
  // changes - the reply is top-posted either way.
  var replySlot = function () { return el(pref('reply_place') === 'above' ? 'reply-slot-top' : 'reply-slot'); };
  var placeCompose = function (where) {
    var dock = el('compose-section');
    if (where === 'inline') {
      var slot = replySlot();
      if (dock.parentNode !== slot) { slot.appendChild(dock); }
      dock.classList.add('inline');
      dock.classList.remove('min'); dock.classList.remove('full');
    } else {
      if (dock.parentNode !== document.body) { document.body.appendChild(dock); }
      dock.classList.remove('inline');
    }
  };
  var composeTitle = function (mode) {
    if (mode === 'reply') { return t('Reply'); }
    if (mode === 'replyall') { return t('Reply all'); }
    if (mode === 'forward') { return t('Forward'); }
    if (mode === 'draft') { return t('Draft'); }
    return t('New message');
  };
  var openCompose = function (mode, id) {
    composeMode = mode; composeId = id;
    var answering = (mode === 'reply' || mode === 'replyall' || mode === 'forward') && current && current.id === id && !el('message-view').hidden;
    placeCompose(answering ? 'inline' : 'dock');
    el('compose-title').textContent = composeTitle(mode) + (answering && current && current.subject ? ': ' + current.subject : '');
    el('compose-section').hidden = false;
    el('compose-section').classList.remove('min');
  };
  var hideCompose = function () { el('compose-section').hidden = true; };
  // Closing keeps what was written: a draft is saved when there is anything
  // to save, and the address bar goes back to the list when it named the window.
  var closeCompose = function () {
    hideCompose();
    el('compose-section').classList.remove('min'); el('compose-section').classList.remove('full');
    if (parseHash().parts[0] === 'compose') { replaceWith(listHash()); }
  };
  el('compose-close').addEventListener('click', function () {
    var written = el('compose-to').value.trim() || el('compose-subject').value.trim() || el('compose-text').value.trim();
    if (written && !pendingSend) { el('compose-save').click(); }
    closeCompose();
  });
  el('compose-attach').addEventListener('click', function () { el('compose-files').click(); });
  el('compose-min').addEventListener('click', function () { el('compose-section').classList.toggle('min'); el('compose-section').classList.remove('full'); });
  el('compose-expand').addEventListener('click', function () { el('compose-section').classList.toggle('full'); el('compose-section').classList.remove('min'); });
  el('compose-title').addEventListener('click', function () { if (el('compose-section').classList.contains('min')) { el('compose-section').classList.remove('min'); } });

  // ---- Pop-out: the message, or the message being written, in a window of its own
  // The page at that address, in a new window the size of a reading pane, so
  // a person reads one message while writing another. Nothing is handed to
  // the new window but the address: it loads as a reload of this page would.
  var popOut = function (hash) {
    var url = location.href.split('#')[0] + '#' + hash;
    try { window.open(url, '_blank', 'popup,width=900,height=720,noopener'); } catch (e) { /* a browser that refuses windows */ }
  };
  el('message-popout').addEventListener('click', function () { if (current) { popOut('/m/' + current.id); } });
  // The message being written is saved as a draft first, so the new window
  // opens on what was typed, and the form here closes: two windows editing
  // one draft would each save over the other. A form with nothing in it
  // opens its own address in the new window.
  el('compose-popout').addEventListener('click', function () {
    var written = el('compose-to').value.trim() || el('compose-subject').value.trim() || el('compose-text').value.trim();
    if (!written) { popOut('/compose' + (composeMode !== 'new' && composeId ? '?' + composeMode + '=' + composeId : '')); closeCompose(); return; }
    saveDraft().then(function (id) {
      if (!id) { return; }
      popOut('/compose?draft=' + id);
      blankCompose();
      composeKey = newComposeKey();
      closeCompose();
    });
  });

  // ---- The rest of a conversation, above the message opened ---------------
  var conversationOpen = {};
  var renderConversation = function (m, page) {
    var box = el('conversation');
    clear(box);
    page = page || (lastListing && lastListing.page);
    if (!page || pref('view') !== 'threads' || page.query) { return; }
    var key = threadKey(m);
    var others = page.messages.filter(function (x) { return x.id !== m.id && threadKey(x) === key; });
    if (!others.length) { return; }
    others.sort(function (a, b) { var da = dateOf(a), db = dateOf(b); return (da ? da.getTime() : 0) - (db ? db.getTime() : 0); });
    box.appendChild(node('div', tf('{0} earlier messages in this conversation', others.length), 'count'));
    others.forEach(function (x) {
      var card = node('div', undefined, 'msg-card collapsed');
      var head = node('div', undefined, 'msg-head');
      head.appendChild(avatarFor(x.from, true, x.brand));
      var from = node('div', undefined, 'from');
      var line = node('div'); line.appendChild(node('b', nameOf(x.from))); from.appendChild(line);
      from.appendChild(node('div', x.snippet || '', 'snip'));
      head.appendChild(from);
      var when = node('div', whenText(x), 'date'); when.setAttribute('title', fullDate(x)); head.appendChild(when);
      card.appendChild(head);
      var body = node('div', undefined, 'body'); body.hidden = true; body.setAttribute('dir', 'auto'); card.appendChild(body);
      var atts = node('div', undefined, 'atts'); atts.hidden = true; card.appendChild(atts);
      var loaded = null;
      card.addEventListener('click', function () {
        var open = card.classList.contains('collapsed');
        card.classList.toggle('collapsed', !open);
        body.hidden = !open; atts.hidden = !open;
        from.childNodes[1].hidden = open;
        if (!open || loaded) { return; }
        loaded = true;
        body.textContent = t('Loading...');
        call('GET', '/api/v1/me/messages/' + x.id).then(function (result) {
          if (result.status !== 200 || !result.data) { body.textContent = t('Could not open the message'); return; }
          var d = result.data;
          var text = d.text || '';
          if (!text && d.html) { text = new DOMParser().parseFromString(d.html, 'text/html').body.textContent || ''; }
          body.textContent = text || t('(no text)');
          clear(atts);
          (d.attachments || []).forEach(function (a) {
            var link = node('a', undefined, 'att');
            link.href = '/api/v1/me/messages/' + d.id + '/attachments/' + a.index;
            link.setAttribute('download', a.name);
            guardDownload(link);
            link.appendChild(icon('file'));
            var words = node('div'); words.appendChild(node('div', a.name, 'n')); words.appendChild(node('div', format(a.size), 's'));
            link.appendChild(words);
            atts.appendChild(link);
          });
          if (!d.flags.seen) { call('PUT', '/api/v1/me/messages/' + d.id + '/flags', { seen: true }).then(function () { x.flags.seen = true; loadFolders(); }); }
        });
      });
      box.appendChild(card);
    });
  };


  // ---- Pin, block, sweep; drag to a folder; the right-click menu -----------
  // ---- The inbox in tabs ----------------------------------------------------
  // Primary, Social, Promotions, Updates and Forums, the way Gmail sorts an
  // inbox, or Focused and Other, the way Outlook does; or one list. The
  // server says which tab a message is from its header (category in the
  // listing); the reader's own word for a sender, kept with the account's
  // preferences, overrules it. A tab is a filter over the page loaded, so
  // switching costs no request.
  var TABS = ['primary', 'social', 'promotions', 'updates', 'forums'];
  var TAB_ICONS = { primary: 'person', social: 'people', promotions: 'tag', updates: 'info', forums: 'chat', focused: 'person', other: 'inbox' };
  var tabName = function (tab) {
    return tab === 'primary' ? t('Primary') : tab === 'social' ? t('Social') : tab === 'promotions' ? t('Promotions') : tab === 'updates' ? t('Updates') :
      tab === 'forums' ? t('Forums') : tab === 'focused' ? t('Focused') : t('Other');
  };
  var inboxMode = function () { var m = pref('inbox'); return m === 'focused' || m === 'all' ? m : 'tabs'; };
  var tabList = function () { return inboxMode() === 'focused' ? ['focused', 'other'] : TABS; };
  var activeTab = 'primary';
  var senderTabs = function () { try { var v = JSON.parse(pref('tabs_by_sender') || '{}'); return v && typeof v === 'object' ? v : {}; } catch (e) { return {}; } };
  var tabOf = function (m) {
    var own = senderTabs()[addressOf(m.from || '').toLowerCase()] || '';
    var focused = inboxMode() === 'focused';
    if (focused && (own === 'focused' || own === 'other')) { return own; }
    var category = TABS.indexOf(own) >= 0 ? own : TABS.indexOf(m.category) >= 0 ? m.category : 'primary';
    if (focused) { return category === 'primary' ? 'focused' : 'other'; }
    return category;
  };
  var tabsActive = function (page) { return inboxMode() !== 'all' && !state.everywhere && !page.query && !!inboxId && state.folderId === inboxId; };
  var renderTabs = function (page) {
    var bar = el('inbox-tabs');
    clear(bar);
    if (!tabsActive(page)) { bar.hidden = true; return; }
    var tabs = tabList();
    if (tabs.indexOf(activeTab) < 0) { activeTab = tabs[0]; }
    var unseen = {};
    page.messages.forEach(function (m) { if (!m.flags.seen) { var tab = tabOf(m); unseen[tab] = (unseen[tab] || 0) + 1; } });
    tabs.forEach(function (tab) {
      var b = document.createElement('button');
      b.type = 'button';
      b.setAttribute('role', 'tab');
      b.setAttribute('data-tab', tab);
      b.setAttribute('aria-selected', tab === activeTab ? 'true' : 'false');
      if (tab === activeTab) { b.classList.add('on'); }
      b.appendChild(icon(TAB_ICONS[tab]));
      b.setAttribute('title', tabName(tab));
      b.appendChild(node('span', tabName(tab), 'lbl'));
      if (unseen[tab]) { var n = node('span', String(unseen[tab]), 'n'); n.setAttribute('title', tf('{0} unread', unseen[tab])); b.appendChild(n); }
      b.addEventListener('click', function () { activeTab = tab; if (lastPage) { renderMessages(lastPage); } });
      bar.appendChild(b);
    });
    bar.hidden = false;
  };
  // The row's menu offers the other tabs for the sender; the choice is kept
  // in the preferences, so it holds on every device and for every message
  // from that sender, and Undo takes it back.
  var renderContextTabs = function (m) {
    var box = el('context-tabs');
    clear(box);
    if (lastPage && tabsActive(lastPage) && m) {
      var mine = tabOf(m);
      box.appendChild(node('div', undefined, 'sep'));
      tabList().forEach(function (tab) {
        if (tab === mine) { return; }
        var b = document.createElement('button');
        b.type = 'button';
        b.setAttribute('data-act', 'tab');
        b.setAttribute('data-tab', tab);
        b.textContent = tf('Show under {0}', tabName(tab));
        box.appendChild(b);
      });
    }
    box.hidden = !box.children.length;
  };
  var moveSenderToTab = function (address, tab) {
    address = String(address || '').trim().toLowerCase();
    if (!address || !tab) { return; }
    var map = senderTabs();
    var before = JSON.stringify(map);
    map[address] = tab;
    savePrefs({ tabs_by_sender: JSON.stringify(map) }).then(function (ok) {
      if (!ok) { return; }
      if (lastPage) { renderMessages(lastPage); }
      toast(tf('Messages from {0} will show under {1}.', address, tabName(tab)), function () {
        savePrefs({ tabs_by_sender: before }).then(function () { if (lastPage) { renderMessages(lastPage); } });
      });
    });
  };

  // ---- Mute -----------------------------------------------------------------
  // The conversation leaves the inbox, and its replies will too: every
  // message of it carries $Muted (so a row shows it, and Unmute knows), the
  // messages are archived, and a rule files whatever answers any of them
  // into the archive folder. Unmute takes the keyword and the rule away.
  var MUTE = '$Muted';
  var isMuted = function (m) { return !!(m && m.flags && (m.flags.keywords || []).indexOf(MUTE) >= 0); };
  var muteMark = function () { var s = node('span', undefined, 'mute'); s.setAttribute('title', t('Muted')); s.appendChild(icon('bell-off', true)); return s; };
  var entryOfCurrent = function () {
    var hit = null;
    listRows.forEach(function (r) { if (r.id === current.id || (r.ids || []).indexOf(current.id) >= 0) { hit = r; } });
    return hit || { id: current.id, m: current, folderId: current.folder_id || state.folderId };
  };
  var threadIdsOf = function (messages) {
    var ids = [];
    messages.forEach(function (m) {
      [m.message_id, m.in_reply_to].concat(String(m.references || '').split(/\s+/)).forEach(function (id) {
        id = String(id || '').trim();
        if (id && ids.indexOf(id) < 0) { ids.push(id); }
      });
    });
    return ids;
  };
  var setKeywordOn = function (ids, keyword, on) {
    var chain = Promise.resolve();
    ids.forEach(function (id) { chain = chain.then(function () { return call('PUT', '/api/v1/me/messages/' + id + '/flags', on ? { keywords_add: [keyword] } : { keywords_remove: [keyword] }); }); });
    return chain;
  };
  var muteThread = function (entry, on) {
    if (!entry || !entry.m) { return; }
    var messages = entry.ms || [entry.m];
    var ids = messages.map(function (m) { return m.id; });
    var key = threadKey(entry.m);
    withRules(function (rules) {
      var before = rules.slice();
      var kept = rules.filter(function (r) { return !(r.field === 'thread' && r.key === key); });
      var next = kept;
      if (on) {
        var archive = allFolders.filter(function (f) { return !f.owner && folderIs(f.id, 'Archive'); })[0];
        next = kept.concat([{ field: 'thread', key: key, text: threadIdsOf(messages), subject: entry.m.subject || '', action: 'move', folder: archive ? archive.path : 'Archive' }]);
      }
      return setKeywordOn(ids, MUTE, on).then(function () { return saveRules(next); }).then(function (ok) {
        if (!ok) { return; }
        var moved = [];
        var moves = Promise.resolve();
        if (on && !folderIs(entry.folderId, 'Archive')) {
          ids.forEach(function (id) {
            moves = moves.then(function () {
              return fileMessage(id, 'archive').then(function (r) { if (r.status === 200 && r.data && r.data.id) { moved.push({ id: r.data.id, back: entry.folderId }); } });
            });
          });
        }
        return moves.then(function () {
          if (current && ids.indexOf(current.id) >= 0) {
            if (moved.length) { closeMessage(true); }
            else { current.flags.keywords = (current.flags.keywords || []).filter(function (k) { return k !== MUTE; }).concat(on ? [MUTE] : []); renderActions(); }
          }
          lastListing = null;
          loadFolders();
          if (moved.length) { loadMessages(); } else { reloadKeepingPlace(); }
          toast(on ? t('Muted. Replies will skip the inbox.') : t('Unmuted.'), on ? function () {
            var back = [];
            var undo = saveRules(before);
            moved.forEach(function (x) {
              undo = undo.then(function () {
                return call('POST', '/api/v1/me/messages/' + x.id + '/move', { folder_id: x.back }).then(function (r) { if (r.status === 200 && r.data && r.data.id) { back.push(r.data.id); } });
              });
            });
            undo.then(function () { return setKeywordOn(moved.length ? back : ids, MUTE, false); }).then(function () {
              lastListing = null;
              loadFolders();
              if (!el('mail-section').hidden) { loadMessages(); }
            });
          } : null);
        });
      });
    });
  };
  var PIN = '$Pinned';
  var isPinned = function (m) { return !!(m && m.flags && (m.flags.keywords || []).indexOf(PIN) >= 0); };
  var pinMessages = function (ids, on) {
    var chain = Promise.resolve();
    ids.forEach(function (id) {
      chain = chain.then(function () { return call('PUT', '/api/v1/me/messages/' + id + '/flags', on ? { keywords_add: [PIN] } : { keywords_remove: [PIN] }); });
    });
    chain.then(function () {
      if (current && ids.indexOf(current.id) >= 0) {
        var list = (current.flags.keywords || []).filter(function (k) { return k !== PIN; });
        if (on) { list.push(PIN); }
        current.flags.keywords = list;
        renderActions();
      }
      lastListing = null;
      reloadKeepingPlace();
      toast(on ? t('Pinned to the top of the list.') : t('Unpinned.'));
    });
  };
  // ---- Important, and sections of the inbox ---------------------------------
  // Gmail's importance marker, decided in this browser from what the page
  // holds already, and written to the message as the $Important keyword
  // through the flags route, so any other client sees it and label:$Important
  // finds it. The reader's own mark wins: Mark as important puts $Important
  // on and $NotImportant off, Not important the reverse, and a message that
  // carries either keyword is never judged again. The page judges a message
  // once, the first time it lists it in the inbox, and only while the marker
  // is set to decide on its own. What it counts is a tally in this browser's
  // storage, one per account: whom the reader wrote to from here, whose mail
  // they opened, answered, starred, marked or unmarked; it is kept until they
  // sign out. The server learns nothing: it holds the keyword, and the
  // contacts it collected from the mail the account sent, both of which it
  // held already.
  var IMPORTANT = '$Important';
  var NOT_IMPORTANT = '$NotImportant';
  var IMPORTANT_QUERY = 'label:' + IMPORTANT;
  var IMPORTANT_THRESHOLD = 2;
  var IMPORTANT_TALLY = 'hmPortalImportant';
  var hasKeyword = function (m, keyword) { return keywordsOf(m).some(function (k) { return String(k).toLowerCase() === keyword.toLowerCase(); }); };
  var isImportant = function (m) { return hasKeyword(m, IMPORTANT) && !hasKeyword(m, NOT_IMPORTANT); };
  var markerMode = function () { var v = pref('important_marker'); return v === 'manual' || v === 'off' ? v : 'auto'; };
  var markerShown = function () { return markerMode() !== 'off'; };
  // Mail the reader sent is not mail the reader is being asked to read, so
  // Sent and Drafts have no marker at all: no button on the row, no item in
  // either menu, and the keys do nothing there.
  var markable = function (folderId) { return markerShown() && !folderIs(folderId, 'Sent') && !folderIs(folderId, 'Drafts'); };
  var sectionsOn = function () { return pref('inbox_sections') === '1'; };
  var sectionsActive = function (page) { return sectionsOn() && !state.everywhere && !page.query && !!inboxId && state.folderId === inboxId; };
  // An address is a name a stranger chose, so every map one is looked up in
  // has no prototype and every lookup asks for an own property: a sender who
  // writes From: <constructor> must not be found in a map that is empty.
  var ownProperty = function (o, k) { return !!o && Object.prototype.hasOwnProperty.call(o, k); };
  // The tally, read once per change rather than once per row.
  var tallyKey = function () { return IMPORTANT_TALLY + ':' + (me || ''); };
  var tallyMemo = { key: '', tally: null };
  var tallyRead = function () {
    if (tallyMemo.tally && tallyMemo.key === tallyKey()) { return tallyMemo.tally; }
    var tally = {};
    try { var v = JSON.parse(localStorage.getItem(tallyKey()) || '{}'); tally = v && typeof v === 'object' ? v : {}; } catch (e) { tally = {}; }
    tallyMemo = { key: tallyKey(), tally: tally };
    return tally;
  };
  var tallyWrite = function (tally) { tallyMemo = { key: tallyKey(), tally: tally }; try { localStorage.setItem(tallyKey(), JSON.stringify(tally)); } catch (e) { /* a browser that keeps nothing */ } };
  // Every tally this browser holds goes, not only the one filed under the
  // address that is signed in now: a session that ended while the page was
  // closed leaves no address to name it by, and the promise is that what
  // this browser learned goes when the session does, however it ended.
  var tallyForget = function () {
    tallyMemo = { key: '', tally: null };
    try {
      if (typeof localStorage.key !== 'function') { localStorage.removeItem(tallyKey()); return; }
      var gone = [];
      for (var i = 0; i < localStorage.length; i++) {
        var name = localStorage.key(i);
        if (name && String(name).indexOf(IMPORTANT_TALLY + ':') === 0) { gone.push(name); }
      }
      gone.forEach(function (name) { localStorage.removeItem(name); });
    } catch (e) { /* nothing to forget */ }
  };
  var tallyCount = function (tally, heading, address) { var counts = tally[heading]; return ownProperty(counts, address) ? (Number(counts[address]) || 0) : 0; };
  // One count per sender under a heading, held between -9 and 99; at most
  // five hundred senders under a heading, the least counted dropped first.
  // The counts are kept with no prototype, so that a sender calling itself
  // __proto__ is a count like any other rather than a write that vanishes.
  var tallyAdd = function (heading, address, by) {
    address = String(address || '').trim().toLowerCase();
    if (!address || !me || address === me || markerMode() !== 'auto') { return; }
    // The tally is filed under the address, but it is kept in this browser
    // and would outlive the second account's token: nothing is learned
    // while that account is shown, and its marker goes by the other reasons
    // alone, the ones read off the message itself.
    if (!offlineMine()) { return; }
    var tally = tallyRead();
    var counts = Object.create(null), was = tally[heading];
    if (was && typeof was === 'object') { Object.keys(was).forEach(function (k) { counts[k] = Number(was[k]) || 0; }); }
    counts[address] = Math.max(-9, Math.min(99, (Number(counts[address]) || 0) + by));
    var names = Object.keys(counts);
    if (names.length > 500) {
      names.sort(function (a, b) { return Math.abs(counts[a]) - Math.abs(counts[b]); });
      names.slice(0, names.length - 500).forEach(function (k) { delete counts[k]; });
    }
    tally[heading] = counts;
    tallyWrite(tally);
  };
  // Whom the reader writes to: the contacts the server collected from the
  // mail this account sent and the ones kept by hand, and whoever was
  // written to from this page since.
  var contactAddresses = { of: null, set: Object.create(null) };
  var writtenTo = function (address, tally) {
    if (contactAddresses.of !== contactsCache) {
      contactAddresses = { of: contactsCache, set: Object.create(null) };
      (contactsCache || []).forEach(function (c) { var a = String(c.address || '').toLowerCase(); if (a) { contactAddresses.set[a] = true; } });
    }
    return ownProperty(contactAddresses.set, address) || tallyCount(tally, 'wrote', address) > 0;
  };
  var ownAddresses = function () {
    var list = me ? [me] : [];
    (identities || []).forEach(function (i) { var a = addressOf(String(i.address || '')).toLowerCase(); if (a && list.indexOf(a) < 0) { list.push(a); } });
    return list;
  };
  // Sent to the reader by name: named in a To of a few, not in a list of
  // many. The folder listing carries To and not Cc - so this asks the To
  // line alone, and a message that names the reader only in Cc does not earn
  // this reason. It is worth one point of the two, so nothing is marked on
  // it by itself; a mailing that hides its list in Cc gets no further.
  var sentDirectly = function (m) {
    var own = ownAddresses();
    var named = String(m.to || '') + ',' + String(m.cc || '');
    var to = named.split(/[,;]/).map(function (p) { return addressOf(p).toLowerCase(); }).filter(function (a) { return !!a; });
    return to.length > 0 && to.length <= 5 && to.some(function (a) { return own.indexOf(a) >= 0; });
  };
  // Mail the server sorted into one of the other tabs - Social, Promotions,
  // Updates, Forums - is mail somebody sent to a list, whatever the reader's
  // inbox layout is; the reader's own word for a sender overrules the
  // server's here as it does in the tab bar.
  var bulkSender = function (m) {
    var words = senderTabs(), sender = addressOf(m.from || '').toLowerCase();
    var own = ownProperty(words, sender) ? String(words[sender]) : '';
    var category = TABS.indexOf(own) >= 0 ? own : TABS.indexOf(m.category) >= 0 ? m.category : 'primary';
    return TABS.indexOf(category) > 0;
  };
  // Every reason a message is important, with its weight; two points make it
  // so. Each is a thing the reader can be told, and the docs carry the table.
  // The reader's own mark is worth two reasons, so that it outweighs the one
  // thing that argues the other way.
  var importantReasons = function (m) {
    var sender = addressOf(m.from || '').toLowerCase();
    if (!sender || sender === me || !m.flags || m.flags.draft) { return []; }
    var tally = tallyRead();
    var reasons = [];
    var marked = tallyCount(tally, 'marked', sender);
    if (marked > 0) { reasons.push(['marked', 4]); }
    if (marked < 0) { reasons.push(['unmarked', -4]); }
    if (writtenTo(sender, tally)) { reasons.push(['wrote', 3]); }
    if (tallyCount(tally, 'answered', sender) > 0) { reasons.push(['answered', 2]); }
    if (tallyCount(tally, 'opened', sender) >= 2) { reasons.push(['opened', 1]); }
    if (tallyCount(tally, 'starred', sender) > 0) { reasons.push(['starred', 1]); }
    if (sentDirectly(m)) { reasons.push(['direct', 1]); }
    if (bulkSender(m)) { reasons.push(['bulk', -2]); }
    return reasons;
  };
  var importantScore = function (m) { return importantReasons(m).reduce(function (sum, r) { return sum + r[1]; }, 0); };
  var reasonText = function (reasons) {
    var best = null;
    reasons.forEach(function (r) { if (r[1] > 0 && (!best || r[1] > best[1])) { best = r; } });
    if (!best) { return ''; }
    return best[0] === 'marked' ? t('you marked mail from this sender important') : best[0] === 'wrote' ? t('you write to this sender') : best[0] === 'answered' ? t('you answered this sender') :
      best[0] === 'opened' ? t('you open mail from this sender') : best[0] === 'starred' ? t('you starred mail from this sender') : t('it was sent to you by name');
  };
  // The marker on a row: lit when the message (any of the conversation) is
  // important, saying why when this page decided it, and a press to change it.
  var judgedByPage = {};
  var importanceTitle = function (ms) {
    if (!ms.some(isImportant)) { return t('Mark as important'); }
    var why = markerMode() === 'auto' && ms.some(function (m) { return judgedByPage[m.id]; }) ? reasonText(importantReasons(ms.filter(isImportant)[0])) : '';
    return why ? tf('Important because {0}. Click to mark as not important.', why) : t('Important. Click to mark as not important.');
  };
  var paintImportance = function (b, ms) {
    var on = ms.some(isImportant);
    b.classList.toggle('on', on);
    b.setAttribute('aria-pressed', on ? 'true' : 'false');
    b.setAttribute('title', importanceTitle(ms));
    b.setAttribute('aria-label', importanceTitle(ms));
  };
  var importanceButton = function (ms) {
    var b = node('button', undefined, 'imp');
    b.type = 'button';
    b.appendChild(icon('important', true));
    paintImportance(b, ms);
    b.addEventListener('click', function (e) { e.stopPropagation(); markImportant(ms, !ms.some(isImportant)); });
    return b;
  };
  // The keywords a change left, put on every copy of the message the page
  // holds - the listing, its rows, the open message - and painted there.
  var putKeywords = function (id, keywords) {
    var take = function (copy) { if (copy && copy.flags && copy.id === id) { copy.flags.keywords = keywords.slice(); } };
    if (lastPage) { lastPage.messages.forEach(take); }
    listRows.forEach(function (r) {
      (r.ms || [r.m]).forEach(take);
      if (r.imp && (r.ids || [r.id]).indexOf(id) >= 0) { paintImportance(r.imp, r.ms || [r.m]); }
    });
    if (current) { take(current); }
  };
  var rerenderKeeping = function (id) {
    if (!lastPage) { return; }
    renderMessages(lastPage);
    if (!id) { return; }
    listRows.forEach(function (r, i) { if (r.id === id || (r.ids || []).indexOf(id) >= 0) { setCursor(i, true); } });
  };
  var judged = {};
  // The inbox listing this browser keeps for reading with no connection was
  // saved before the mark was made; it is saved again after one, or the
  // keyword is missing exactly when the connection is.
  var offlineListKeep = function () {
    if (!offline.days || !lastPage || state.everywhere || state.query || state.before || !inboxId || state.folderId !== inboxId) { return; }
    offlineKeep('list', lastPage);
  };
  // One PUT per message, in turn; the answer is the messages that were
  // changed rather than a yes or a no. A conversation the server refuses
  // half way - a folder the ACL will not let this account write to, a
  // message already carrying as many keywords as it may - leaves the rest
  // changed, and the caller is told which those were so that the way back
  // is offered over exactly them.
  var sendMarks = function (ms, changeFor) {
    var chain = Promise.resolve(), done = [];
    ms.forEach(function (m) {
      chain = chain.then(function () {
        return call('PUT', '/api/v1/me/messages/' + m.id + '/flags', changeFor(m)).then(function (result) {
          if (result.status !== 200 || !result.data || !result.data.flags) { return; }
          done.push(m);
          judged[m.id] = true;
          putKeywords(m.id, result.data.flags.keywords || []);
          if (offline.days) { offlineFlagsSync({ id: m.id, flags: result.data.flags, folder_id: 0 }); }
        });
      });
    });
    return chain.then(function () { if (done.length) { offlineListKeep(); } return done; });
  };
  var afterMarks = function (ms) {
    if (current && ms.some(function (m) { return m.id === current.id; })) { renderActions(); }
    if (lastPage && sectionsActive(lastPage)) { rerenderKeeping(ms[0].id); }
  };
  // A mark over a conversation is one word about that sender, not one word
  // for each of its messages: raising a thread of twenty must not take
  // twenty corrections before "not important" about the same sender counts.
  var tallySenders = function (ms, by) {
    var seen = Object.create(null);
    ms.forEach(function (m) {
      var address = addressOf(m.from || '').trim().toLowerCase();
      if (!address || ownProperty(seen, address)) { return; }
      seen[address] = true;
      tallyAdd('marked', address, by);
    });
  };
  // The reader's mark: the one keyword on, the other off, on every message
  // given, and the sender tallied so the next from them is judged with it.
  // Undo puts back the keywords each message had, which may be neither.
  var importantOwn = function (k) { var lower = String(k).toLowerCase(); return lower === IMPORTANT.toLowerCase() || lower === NOT_IMPORTANT.toLowerCase(); };
  var markImportant = function (ms, on) {
    if (!markerShown() || !ms.length) { return Promise.resolve(); }
    var had = ms.map(function (m) { return keywordsOf(m).filter(importantOwn); });
    var change = on ? { keywords_add: [IMPORTANT], keywords_remove: [NOT_IMPORTANT] } : { keywords_add: [NOT_IMPORTANT], keywords_remove: [IMPORTANT] };
    return sendMarks(ms, function () { return change; }).then(function (done) {
      // Nothing changed is a failure and is said so. Some of it changed is
      // still a change the reader made and may want back, so it is said and
      // the Undo is offered over the messages that did change.
      if (!done.length) { say('mail-status', t('Could not change the flags'), false); return; }
      if (done.length < ms.length) { say('mail-status', t('Could not change the flags'), false); }
      done.forEach(function (m) { delete judgedByPage[m.id]; });
      tallySenders(done, on ? 1 : -1);
      afterMarks(done);
      toast(on ? t('Marked as important.') : t('Marked as not important.'), function () {
        sendMarks(done, function (m) {
          var keep = had[ms.indexOf(m)];
          return { keywords_add: keep, keywords_remove: [IMPORTANT, NOT_IMPORTANT].filter(function (k) { return keep.indexOf(k) < 0; }) };
        }).then(function (back) {
          if (!back.length) { say('mail-status', t('Could not change the flags'), false); return; }
          if (back.length < done.length) { say('mail-status', t('Could not change the flags'), false); }
          tallySenders(back, on ? -1 : 1);
          afterMarks(back);
        });
      });
    });
  };
  // The listing judged: once per message per session, in the inbox, while
  // the marker decides on its own and the server can be reached; a message
  // carrying either keyword is left as it is. A verdict goes on the page's
  // copy at once, so a section does not move when the answer comes, then to
  // the server in turn, at most twenty-five in one pass - the rest wait for
  // the next render. The contacts weigh most, so nothing is judged before
  // they have been read once.
  var judgeWaiting = false;
  var judgeListing = function (page) {
    // page.offline is the listing this browser kept, drawn because the
    // server could not be reached. offline.only is the session that started
    // that way; a session that merely lost its connection is not that, and
    // judging what it draws would paint markers no PUT could ever carry.
    if (markerMode() !== 'auto' || offline.only || !page || page.offline || page.query || state.everywhere || !inboxId || state.folderId !== inboxId) { return; }
    if (!contactsCache) {
      if (judgeWaiting) { return; }
      judgeWaiting = true;
      ensureContacts().then(function () {
        judgeWaiting = false;
        if (lastPage !== page || !contactsCache) { return; }
        judgeListing(page);
        if (sectionsActive(page)) { rerenderKeeping(cursor >= 0 && listRows[cursor] ? listRows[cursor].id : 0); }
      });
      return;
    }
    var todo = [], had = {};
    page.messages.forEach(function (m) {
      if (todo.length >= 25 || judged[m.id] || !m.flags || m.flags.draft || hasKeyword(m, IMPORTANT) || hasKeyword(m, NOT_IMPORTANT)) { return; }
      judged[m.id] = true;
      if (importantScore(m) >= IMPORTANT_THRESHOLD) { todo.push(m); }
    });
    todo.forEach(function (m) { had[m.id] = keywordsOf(m).slice(); judgedByPage[m.id] = true; putKeywords(m.id, keywordsOf(m).concat([IMPORTANT])); });
    if (!todo.length) { return; }
    sendMarks(todo, function () { return { keywords_add: [IMPORTANT] }; }).then(function (done) {
      // A verdict the server did not take is not a verdict. The message goes
      // back to the keywords it had and is judged again on the next listing,
      // rather than wearing a marker no other client will ever see.
      var lost = todo.filter(function (m) { return done.indexOf(m) < 0; });
      lost.forEach(function (m) { delete judged[m.id]; delete judgedByPage[m.id]; putKeywords(m.id, had[m.id]); });
      if (lost.length && lastPage === page && sectionsActive(page)) { rerenderKeeping(cursor >= 0 && listRows[cursor] ? listRows[cursor].id : 0); }
    });
  };
  // What the tally counts: an opening, the first star, a send and whom it
  // answered. Each is one count for the sender, or the recipient, and none
  // is sent anywhere.
  var openedIds = {};
  var learnOpened = function (m) {
    if (!m || !m.id || openedIds[m.id] || fromMe(m)) { return; }
    openedIds[m.id] = true;
    tallyAdd('opened', addressOf(m.from || ''), 1);
  };
  var learnStarred = function (m) { if (m && !fromMe(m)) { tallyAdd('starred', addressOf(m.from || ''), 1); } };
  var learnSent = function (queued) {
    var body = (queued && queued.body) || {};
    var seen = Object.create(null);
    [body.to, body.cc].forEach(function (field) {
      String(field || '').split(/[,;]/).forEach(function (part) { var a = addressOf(part).toLowerCase(); if (a && !seen[a]) { seen[a] = true; tallyAdd('wrote', a, 1); } });
    });
    if (queued && queued.answered) { tallyAdd('answered', queued.answered, 1); }
  };
  el('message-important').addEventListener('click', function () { closeMenus(); if (current) { markImportant([current], !isImportant(current)); } });
  // ---- Sections of the inbox: Important, Starred, Everything else -----------
  // A choice in Settings. The tabs sort first and the sections divide what
  // the open tab shows, so under Primary the sections hold Primary's mail
  // and under Promotions its own; with the inbox in one list they divide the
  // whole of it. A conversation goes where its messages send it: Important
  // if any is, else Starred if any is, else Everything else. A section folds
  // on a click, and this browser remembers which are folded.
  var SECTIONS = ['important', 'starred', 'rest'];
  var SECTIONS_CLOSED = 'hmPortalSectionsClosed';
  var sectionName = function (s) { return s === 'important' ? t('Important') : s === 'starred' ? t('Starred') : t('Everything else'); };
  var sectionOf = function (ms) {
    if (markerShown() && ms.some(isImportant)) { return 'important'; }
    if (ms.some(function (m) { return !!(m.flags && m.flags.flagged); })) { return 'starred'; }
    return 'rest';
  };
  var closedSections = function () { try { var v = JSON.parse(localStorage.getItem(SECTIONS_CLOSED) || '{}'); return v && typeof v === 'object' ? v : {}; } catch (e) { return {}; } };
  var toggleSection = function (s) {
    var closed = closedSections();
    if (closed[s]) { delete closed[s]; } else { closed[s] = 1; }
    try { if (Object.keys(closed).length) { localStorage.setItem(SECTIONS_CLOSED, JSON.stringify(closed)); } else { localStorage.removeItem(SECTIONS_CLOSED); } } catch (e) { /* a browser that keeps nothing */ }
    if (lastPage) { renderMessages(lastPage); }
  };
  var renderSections = function (page, list, shown) {
    var threads = pref('view') === 'threads' && !page.query;
    var byKey = Object.create(null), keyOf = {};
    if (threads) { page.messages.forEach(function (m) { var k = threadKey(m); keyOf[m.id] = k; if (!byKey[k]) { byKey[k] = []; } byKey[k].push(m); }); }
    var buckets = { important: [], starred: [], rest: [] };
    page.messages.forEach(function (m) { buckets[sectionOf(threads ? byKey[keyOf[m.id]] : [m])].push(m); });
    var closed = closedSections();
    SECTIONS.forEach(function (s) {
      var ms = buckets[s];
      if (!ms.length) { return; }
      // The list says it is a list and each row says it is an item of one, so
      // a section must not come between them: the box around a section is
      // presentational, its heading is a heading, and its rows are a group
      // named by that heading - which is what the toggle folds away.
      var box = node('div', undefined, 'list-sect' + (closed[s] ? ' shut' : ''));
      box.setAttribute('role', 'presentation');
      var head = node('div', undefined, 'list-sect-head');
      head.setAttribute('role', 'heading');
      head.setAttribute('aria-level', '3');
      var toggle = node('button', undefined, 'list-sect-toggle');
      toggle.type = 'button';
      var name = node('span', sectionName(s), 'nm');
      name.id = 'list-sect-name-' + s;
      toggle.setAttribute('aria-expanded', closed[s] ? 'false' : 'true');
      toggle.setAttribute('aria-controls', 'list-sect-rows-' + s);
      toggle.appendChild(icon('down', true));
      toggle.appendChild(name);
      var unseen = ms.filter(function (m) { return !(m.flags && m.flags.seen); }).length;
      toggle.appendChild(node('span', unseen ? tf('{0}, {1} unread', ms.length, unseen) : String(ms.length), 'ct'));
      toggle.addEventListener('click', function () { toggleSection(s); });
      head.appendChild(toggle);
      box.appendChild(head);
      var rows = node('div', undefined, 'list-sect-rows');
      rows.id = 'list-sect-rows-' + s;
      rows.setAttribute('role', 'group');
      rows.setAttribute('aria-labelledby', name.id);
      box.appendChild(rows);
      list.appendChild(box);
      if (!closed[s]) { renderRows({ messages: ms, query: page.query }, rows, shown); }
    });
  };
  // A rule that files what the sender sends into Junk - or discards it where
  // the account has no Junk folder - added to the account's own rules.
  // ---- Report phishing ------------------------------------------------------
  // Beside Junk: the message's source goes to the domain's postmaster as an
  // attachment (with what the page knows of it in the text when the source
  // cannot be fetched), the message is filed as junk, which teaches the
  // filter, and the sender is blocked - the block's toast carries its Undo.
  var reportPhishing = function (m) {
    if (!m) { return Promise.resolve(); }
    var domain = me && me.indexOf('@') > 0 ? me.split('@')[1] : '';
    if (!domain) { say('mail-status', t('Could not report the message'), false); return Promise.resolve(); }
    var sender = addressOf(m.from);
    var report = {
      to: 'postmaster@' + domain,
      subject: t('Phishing report: ') + String(m.subject || ''),
      text: tf('{0} reported this message as phishing. From: {1}. Subject: {2}. Received: {3}. Its source is attached.', me, m.from || '', m.subject || '', m.date || m.received || '')
    };
    var source = apiFetch('/api/v1/me/messages/' + m.id + '/source', { cache: 'no-store', credentials: 'same-origin' }).then(function (response) {
      if (response.status !== 200) { return null; }
      return response.blob().then(function (blob) {
        return new Promise(function (resolve) {
          var reader = new FileReader();
          reader.onload = function () { resolve({ name: 'reported-' + m.id + '.eml', type: 'message/rfc822', data: String(reader.result).split(',')[1] || '' }); };
          reader.onerror = function () { resolve(null); };
          reader.readAsDataURL(blob);
        });
      });
    }).catch(function () { return null; });
    return source.then(function (attachment) {
      if (attachment) { report.attachments = [attachment]; }
      return call('POST', '/api/v1/me/messages', report);
    }).then(function (result) {
      if (result.status !== 201 && result.status !== 200) { say('mail-status', describe(result, t('Could not report the message')), false); return; }
      var entry = { id: m.id, folderId: m.folder_id || state.folderId, m: m };
      if (!folderIs(entry.folderId, 'Junk')) { fileRow(entry, 'junk'); }
      if (sender) { blockSender(sender); }
      say('mail-status', t('Reported to the postmaster, filed as junk and the sender blocked.'), true);
    });
  };
  el('message-phish').addEventListener('click', function () { if (current) { closeMenus(); reportPhishing(current); } });
  // Block this sender. It used to write a rule into the account's own Sieve
  // script - a move-to-Junk, or a discard when the account had no Junk folder.
  // It writes to the blocked-sender list instead, and the reasons are worth
  // keeping: a rule could not say "and everything under that domain" without a
  // second rule, it could not expire, it was invisible to the administrator
  // looking at why a sender was not getting through, and the discard branch
  // destroyed mail - which is the one thing "block" must never mean. An
  // existing rule somebody already has is left exactly where it is; nothing
  // migrates and nothing is deleted.
  var blockSender = function (address) {
    address = String(address || '').trim();
    if (!address) { return; }
    call('POST', '/api/v1/me/sender-list', { value: address, blocked: true }).then(function (result) {
      if (result.status !== 201 && result.status !== 200) { toast(describe(result, t('Could not add it'))); return; }
      var id = result.data && result.data.id;
      toast(tf('Mail from {0} will be held as junk.', address), id ? function () {
        call('DELETE', '/api/v1/me/sender-list/' + id).then(function () { loadSenderList(); });
      } : null);
      loadSenderList();
    });
  };
  // Safe senders and blocked senders: one list, one row per address or domain,
  // and a POST of a value already on it rewrites that row rather than adding a
  // second - so moving an address from safe to blocked is one action and never
  // leaves the two halves arguing.
  var renderSenderList = function (items) {
    var rows = el('sender-rows');
    clear(rows);
    items.forEach(function (entry) {
      var tr = document.createElement('tr');
      tr.appendChild(node('td', entry.value));
      tr.appendChild(node('td', entry.blocked ? t('Blocked') : t('Safe')));
      tr.appendChild(node('td', entry.reason || ''));
      var actions = document.createElement('td');
      var flip = button(entry.blocked ? t('Make safe') : t('Block'));
      flip.addEventListener('click', function () {
        call('PUT', '/api/v1/me/sender-list/' + entry.id, { blocked: !entry.blocked }).then(function (result) {
          if (result.status !== 200) { say('sender-status', describe(result, t('Could not change it')), false); return; }
          loadSenderList();
        });
      });
      var remove = button(t('Remove'));
      remove.addEventListener('click', function () {
        call('DELETE', '/api/v1/me/sender-list/' + entry.id).then(function (result) {
          if (result.status !== 200) { say('sender-status', describe(result, t('Could not remove it')), false); return; }
          say('sender-status', t('Removed.'), true);
          loadSenderList();
        });
      });
      actions.appendChild(flip);
      actions.appendChild(remove);
      tr.appendChild(actions);
      rows.appendChild(tr);
    });
    el('sender-empty').hidden = items.length > 0;
    el('sender-table').hidden = items.length === 0;
  };
  var loadSenderList = function () {
    return call('GET', '/api/v1/me/sender-list').then(function (result) {
      if (result.status !== 200 || !result.data) { say('sender-status', describe(result, t('Could not read your lists')), false); return; }
      renderSenderList(result.data.items || []);
    });
  };
  var renderDigestSetting = function (state) {
    el('digest-enabled').checked = !!state.enabled;
    el('digest-enabled').disabled = !state.available;
    el('digest-note').textContent = state.available
      ? (state.last_sent ? tf('The last one was sent {0}.', state.last_sent) : t('None has been sent to you yet.'))
      : t('Your server does not send a digest, so there is nothing to switch on here.');
  };
  // ---- Subscriptions -------------------------------------------------------
  // Every sender whose mail offers a way out, how much it sent in the window,
  // and one button to leave - Gmail's Manage subscriptions. The unsubscribe is
  // the message route's own, asked of the sender's newest message, which names
  // the list's current way out. What was left is remembered with the account,
  // so a list that keeps writing shows that it was asked to stop, and when.
  var unsubscribedOf = function () {
    try { var map = JSON.parse(pref('unsubscribed') || '{}'); return map && typeof map === 'object' ? map : {}; } catch (why) { return {}; }
  };
  var rememberUnsubscribed = function (address) {
    var map = unsubscribedOf();
    map[address] = dayKey(new Date());
    // The newest forty: a preference is at most 4,000 characters.
    var keys = Object.keys(map).sort(function (a, b) { return String(map[b]).localeCompare(String(map[a])); }).slice(0, 40);
    var kept = {};
    keys.forEach(function (k) { kept[k] = map[k]; });
    return savePrefs({ unsubscribed: JSON.stringify(kept) });
  };
  var deleteMailFrom = function (address) {
    return call('GET', '/api/v1/me/search?q=' + encodeURIComponent('from:' + address) + '&limit=200').then(function (result) {
      if (result.status !== 200 || !result.data) { say('subs-status', describe(result, t('Could not find their mail')), false); return false; }
      var hits = (result.data.messages || []).filter(function (m) { return addressOf(m.from).toLowerCase() === address.toLowerCase(); });
      if (!hits.length) { toast(tf('Nothing from {0} to delete.', address)); return false; }
      return fileMany(hits.map(function (m) { return { id: m.id, folderId: m.folder_id }; }), 'delete');
    });
  };
  var renderSubscriptions = function (data) {
    var rows = el('subs-rows');
    clear(rows);
    var list = data.subscriptions || [];
    var left = unsubscribedOf();
    list.forEach(function (s) {
      var tr = document.createElement('tr');
      var who = document.createElement('td');
      who.appendChild(node('div', s.name || s.address));
      if (s.name) { who.appendChild(node('div', s.address, 'muted')); }
      if (s.list_id) { who.appendChild(node('div', s.list_id, 'muted')); }
      tr.appendChild(who);
      var much = document.createElement('td');
      much.appendChild(node('div', tf('{0} messages', s.count)));
      if (s.unread) { much.appendChild(node('div', tf('{0} unread', s.unread), 'muted')); }
      tr.appendChild(much);
      tr.appendChild(node('td', whenText({ received: s.last })));
      var actions = document.createElement('td');
      if (left[s.address]) {
        actions.appendChild(node('span', tf('Unsubscribed {0}', left[s.address]), 'badge good'));
      }
      if (s.method === 'one-click' || s.method === 'mail') {
        var leave = button(left[s.address] ? t('Unsubscribe again') : t('Unsubscribe'), 'btn sm');
        leave.addEventListener('click', function () {
          leave.disabled = true;
          call('POST', '/api/v1/me/messages/' + s.message_id + '/unsubscribe').then(function (result) {
            var d = result.data || {};
            if (result.status === 200 && d.method === 'one-click' && d.ok) { say('subs-status', tf('Unsubscribed from {0}: the list answered {1}.', s.address, d.status), true); }
            else if (result.status === 201 && d.method === 'mail') { say('subs-status', tf('An unsubscribe request for {0} was sent to {1}.', s.address, d.to), true); }
            else { leave.disabled = false; say('subs-status', describe(result, t('Could not unsubscribe')), false); return; }
            rememberUnsubscribed(s.address).then(function () { renderSubscriptions(data); });
          });
        });
        actions.appendChild(leave);
      } else if (s.method === 'page' && /^https:\/\//i.test(s.url || '')) {
        // A list that offers only a page: the person has to go there, as they
        // would from the link at the bottom of the message.
        var open = node('a', t('Open their page'), 'btn ghost sm');
        open.setAttribute('href', s.url);
        open.setAttribute('target', '_blank');
        open.setAttribute('rel', 'noopener noreferrer');
        open.addEventListener('click', function () { rememberUnsubscribed(s.address).then(function () { renderSubscriptions(data); }); });
        actions.appendChild(open);
      }
      var block = button(t('Block'));
      block.addEventListener('click', function () { blockSender(s.address); });
      actions.appendChild(block);
      var sweep = button(t('Delete their mail'));
      sweep.addEventListener('click', function () {
        if (!window.confirm(tf('Delete the mail from {0}? It goes to Trash.', s.address))) { return; }
        deleteMailFrom(s.address).then(function (ok) { if (ok) { loadSubscriptions(); } });
      });
      actions.appendChild(sweep);
      tr.appendChild(actions);
      rows.appendChild(tr);
    });
    el('subs-table').hidden = !list.length;
    el('subs-empty').hidden = list.length > 0;
    el('subs-partial').hidden = data.complete !== false;
    el('subs-partial').textContent = data.complete === false ? tf('Only the newest {0} messages were read; there is more mail in that time than that.', data.scanned) : '';
  };
  var loadSubscriptions = function () {
    say('subs-status', t('Reading your mail...'), true);
    return call('GET', '/api/v1/me/subscriptions?days=' + encodeURIComponent(el('subs-days').value || '90')).then(function (result) {
      if (result.status !== 200 || !result.data) { say('subs-status', describe(result, t('Could not read your subscriptions')), false); return; }
      say('subs-status', '', true);
      renderSubscriptions(result.data);
    });
  };
  el('subs-days').addEventListener('change', function () { loadSubscriptions(); });
  // ---- Other accounts ------------------------------------------------------
  // Another mailbox collected into this one - Gmail's "check mail from other
  // accounts" - through the reader's own fetch-account routes. The server keeps
  // the filters on and reads no recipients out of the collected mail whatever
  // the page sends; the page's part is the form, the four providers' settings,
  // and saying plainly what each of them asks for.
  var FETCH_PRESETS = {
    gmail: { name: 'Gmail', host: 'imap.gmail.com', port: 993, type: 'imap', security: 'tls', hint: K('Gmail refuses your usual password here. Turn on 2-Step Verification in your Google Account, create an app password under Security, and use that.') },
    outlook: { name: 'Outlook.com', host: 'outlook.office365.com', port: 993, type: 'imap', security: 'tls', hint: K('Outlook.com no longer accepts a password from another mail server; it asks for a Microsoft sign-in. Until that is offered here, forward your mail from Outlook.com instead: Settings, Mail, Forwarding.'),
      signIn: 'microsoft', signInHint: K('Outlook.com asks for a Microsoft sign-in: use Sign in to Outlook.com above.') },
    yahoo: { name: 'Yahoo Mail', host: 'imap.mail.yahoo.com', port: 993, type: 'imap', security: 'tls', hint: K('Yahoo asks for an app password: Account Info, Account security, Generate app password.') },
    icloud: { name: 'iCloud Mail', host: 'imap.mail.me.com', port: 993, type: 'imap', security: 'tls', hint: K('iCloud asks for an app-specific password, made at account.apple.com under Sign-In and Security.') }
  };
  var fetchEditing = null;
  var fetchPoll = null;
  var importOffered = { google: false, microsoft: false };
  var fetchChoosing = null;
  var fetchFormReset = function () {
    fetchEditing = null;
    ['fetch-name', 'fetch-host', 'fetch-user', 'fetch-password'].forEach(function (id) { el(id).value = ''; });
    el('fetch-preset').value = '';
    el('fetch-port').value = '993';
    el('fetch-type').value = 'imap';
    el('fetch-security').value = 'tls';
    el('fetch-keep').value = '-1';
    el('fetch-every').value = '30';
    el('fetch-mirror').checked = false;
    el('fetch-hint').hidden = true;
    el('fetch-save').textContent = t('Add the account');
    el('fetch-cancel').hidden = true;
  };
  // Set up by an administrator: something a reader cannot set, which the
  // server keeps out of a reader's change, so the page offers none.
  var fetchIsAdministrators = function (f) {
    return f.process_mime_recipients || f.enable_route_recipients || f.use_antispam === false || f.use_antivirus === false || !!f.mime_recipient_headers;
  };
  var renderFetchAccounts = function (list) {
    var rows = el('fetch-rows');
    clear(rows);
    var anyLocked = false;
    list.forEach(function (f) {
      var tr = document.createElement('tr');
      var who = document.createElement('td');
      who.appendChild(node('div', f.name));
      if (f.username) { who.appendChild(node('div', f.username, 'muted')); }
      if (f.oauth_provider) { who.appendChild(node('div', f.oauth_provider === 'google' ? t('Signed in with Google') : t('Signed in with Microsoft'), 'muted')); }
      tr.appendChild(who);
      tr.appendChild(node('td', String(f.server_type).toUpperCase() + ' ' + f.server_address + ':' + f.port + (f.mirror_folders ? ' \u00b7 ' + t('with its folders') : '')));
      var seen = f.mirror_seen || [];
      var state = f.locked ? t('Collecting now') : !f.enabled ? t('Paused') :
        f.mirror_pending ? (seen.length ? t('Waiting for you to choose the folders') : t('Reading the folders there')) :
        f.next_download_time ? tf('Next collection {0}', whenText({ received: f.next_download_time })) : t('Waiting');
      if (f.collected_messages) { state += ' \u00b7 ' + tf('{0} messages', f.collected_messages); }
      // Still to list its folders: look again, as for a collection running.
      anyLocked = anyLocked || !!f.locked || (!!f.mirror_pending && !seen.length && !!f.enabled);
      tr.appendChild(node('td', state));
      var actions = document.createElement('td');
      var now = button(t('Collect now'));
      now.disabled = !f.enabled || !!f.locked;
      now.addEventListener('click', function () {
        call('POST', '/api/v1/me/fetch-accounts/' + f.id + '/download').then(function (result) {
          if (result.status !== 202) { say('fetch-status', describe(result, t('Could not start the collection')), false); return; }
          say('fetch-status', tf('Collecting from {0}.', f.name), true);
          loadFetchAccounts();
        });
      });
      actions.appendChild(now);
      if (f.mirror_folders && seen.length && !fetchIsAdministrators(f)) {
        var folders = button(f.mirror_pending ? t('Choose the folders') : t('Folders'), f.mirror_pending ? 'btn sm' : 'btn ghost sm');
        folders.addEventListener('click', function () { openFolderChoice(f); });
        actions.appendChild(folders);
      }
      if (fetchIsAdministrators(f)) {
        actions.appendChild(node('span', t('Set up by your administrator'), 'muted'));
      } else {
        var pause = button(f.enabled ? t('Pause') : t('Resume'));
        pause.addEventListener('click', function () {
          call('PUT', '/api/v1/me/fetch-accounts/' + f.id, { enabled: !f.enabled }).then(function (result) {
            if (result.status !== 200) { say('fetch-status', describe(result, t('Could not change it')), false); return; }
            loadFetchAccounts();
          });
        });
        actions.appendChild(pause);
        var edit = button(t('Edit'));
        // Signed in with a provider: its server and address are the grant's.
        edit.hidden = !!f.oauth_provider;
        edit.addEventListener('click', function () {
          fetchEditing = f.id;
          el('fetch-preset').value = '';
          el('fetch-hint').hidden = true;
          el('fetch-name').value = f.name;
          el('fetch-host').value = f.server_address;
          el('fetch-port').value = String(f.port);
          el('fetch-type').value = f.server_type;
          el('fetch-security').value = f.connection_security === 'starttls_optional' ? 'starttls_required' : f.connection_security;
          el('fetch-user').value = f.username || '';
          el('fetch-password').value = '';
          el('fetch-keep').value = String(f.days_to_keep_messages === 0 || f.days_to_keep_messages === -1 ? f.days_to_keep_messages : 30);
          el('fetch-every').value = String(f.minutes_between_fetch >= 360 ? 360 : f.minutes_between_fetch >= 60 ? 60 : f.minutes_between_fetch >= 30 ? 30 : 15);
          el('fetch-mirror').checked = !!f.mirror_folders;
          el('fetch-save').textContent = t('Save the account');
          el('fetch-cancel').hidden = false;
          el('fetch-name').focus();
        });
        actions.appendChild(edit);
        var remove = button(t('Remove'));
        remove.addEventListener('click', function () {
          if (!window.confirm(tf('Stop collecting from {0}? The mail already collected stays.', f.name))) { return; }
          call('DELETE', '/api/v1/me/fetch-accounts/' + f.id).then(function (result) {
            if (result.status !== 200) { say('fetch-status', describe(result, t('Could not remove it')), false); return; }
            say('fetch-status', t('Removed.'), true);
            if (fetchEditing === f.id) { fetchFormReset(); }
            loadFetchAccounts();
          });
        });
        actions.appendChild(remove);
      }
      tr.appendChild(actions);
      rows.appendChild(tr);
    });
    el('fetch-table').hidden = !list.length;
    el('fetch-empty').hidden = list.length > 0;
    // While a collection runs, look again in a few seconds, as the list of a
    // folder does for new mail.
    if (fetchPoll) { clearTimeout(fetchPoll); fetchPoll = null; }
    if (anyLocked && !el('accounts-section').hidden) { fetchPoll = setTimeout(function () { fetchPoll = null; loadFetchAccounts(); }, 4000); }
  };
  var loadFetchAccounts = function () {
    return call('GET', '/api/v1/me/fetch-accounts').then(function (result) {
      if (result.status === 403) {
        el('fetch-off').hidden = false;
        el('fetch-form').hidden = true;
        el('fetch-table').hidden = true;
        el('fetch-empty').hidden = true;
        el('import-signin').hidden = true;
        return;
      }
      el('fetch-off').hidden = true;
      el('fetch-form').hidden = false;
      if (result.status !== 200 || !Array.isArray(result.data)) { say('fetch-status', describe(result, t('Could not read your other accounts')), false); return; }
      renderFetchAccounts(result.data);
      return call('GET', '/api/v1/me/fetch-accounts/import').then(function (offered) {
        importOffered = offered.status === 200 && offered.data ? offered.data : { google: false, microsoft: false };
        el('import-google').hidden = !importOffered.google;
        el('import-microsoft').hidden = !importOffered.microsoft;
        el('import-signin').hidden = !importOffered.google && !importOffered.microsoft;
        // The sign-in at the provider leaves this page and comes back to it
        // reloaded, and a reload forgets the second account: the page would
        // be the first account's again with the new row out of sight. So
        // the two buttons wait while the second account is shown, and the
        // note says where to do it.
        var second = onSecond();
        el('import-google').disabled = second;
        el('import-microsoft').disabled = second;
        el('import-second').hidden = !second;
        if (second) { el('import-second').textContent = tf('Signing in at Gmail or Outlook.com leaves this page and comes back to it reloaded, which forgets the second account. To move mail into {0}, sign in to it on its own.', accounts.second.address); }
      });
    });
  };
  // Back from the provider: what the callback said, once, and the address
  // bar put back to the page itself so a reload does not say it again.
  var importOutcome = function (params) {
    if (!params || (params.imported === undefined && params.import_error === undefined)) { return; }
    if (params.import_error !== undefined) {
      say('fetch-status', tf('The sign-in did not finish: {0}', params.import_error), false);
    } else {
      say('fetch-status', t('Signed in. Your server is reading what is there; when the folders are listed, choose which to bring.'), true);
    }
    try { history.replaceState(null, '', '#/accounts'); } catch (e) { /* the message is shown; the address stays */ }
  };
  var startImport = function (provider) {
    var body = { provider: provider, mirror_folders: !!el('import-mirror').checked, days_to_keep_messages: el('import-keep').value === '0' ? 0 : -1 };
    el('import-google').disabled = true;
    el('import-microsoft').disabled = true;
    call('POST', '/api/v1/me/fetch-accounts/import', body).then(function (result) {
      el('import-google').disabled = false;
      el('import-microsoft').disabled = false;
      if (result.status !== 200 || !result.data || !result.data.url) { say('fetch-status', describe(result, t('Could not start the sign-in')), false); return; }
      say('fetch-status', t('Taking you to sign in...'), true);
      location.href = result.data.url;
    });
  };
  el('import-google').addEventListener('click', function () { startImport('google'); });
  el('import-microsoft').addEventListener('click', function () { startImport('microsoft'); });
  var openFolderChoice = function (f) {
    fetchChoosing = f;
    var list = el('fetch-folder-list');
    clear(list);
    var skipped = f.mirror_pending ? null : (f.mirror_skip || []);
    // The views of the other folders (\\All, \\Flagged, \\Important) start unticked.
    var copies = f.mirror_copies || [];
    (f.mirror_seen || []).forEach(function (name) {
      var label = document.createElement('label');
      label.className = 'inline';
      var box = document.createElement('input');
      box.type = 'checkbox';
      box.setAttribute('data-folder', name);
      box.checked = skipped === null ? copies.indexOf(name) < 0 : skipped.indexOf(name) < 0;
      label.appendChild(box);
      label.appendChild(node('span', ' ' + name));
      list.appendChild(label);
    });
    el('fetch-folders-title').textContent = tf('Folders to bring from {0}', f.name);
    el('fetch-folders').hidden = false;
  };
  el('fetch-folders-cancel').addEventListener('click', function () { fetchChoosing = null; el('fetch-folders').hidden = true; });
  el('fetch-folders-save').addEventListener('click', function () {
    var f = fetchChoosing;
    if (!f) { return; }
    var skip = [];
    var labels = el('fetch-folder-list').childNodes;
    for (var i = 0; i < labels.length; i++) {
      var box = labels[i].firstChild;
      if (box && box.getAttribute && box.getAttribute('data-folder') !== null && !box.checked) { skip.push(box.getAttribute('data-folder')); }
    }
    call('PUT', '/api/v1/me/fetch-accounts/' + f.id, { mirror_skip: skip }).then(function (result) {
      if (result.status !== 200) { say('fetch-status', describe(result, t('Could not save the choice')), false); return; }
      fetchChoosing = null;
      el('fetch-folders').hidden = true;
      return call('POST', '/api/v1/me/fetch-accounts/' + f.id + '/download').then(function () {
        say('fetch-status', tf('Bringing the folders over from {0}.', f.name), true);
        loadFetchAccounts();
      });
    });
  });
  el('fetch-preset').addEventListener('change', function () {
    var p = FETCH_PRESETS[el('fetch-preset').value];
    el('fetch-hint').hidden = !p;
    if (!p) { return; }
    if (!el('fetch-name').value.trim()) { el('fetch-name').value = p.name; }
    el('fetch-host').value = p.host;
    el('fetch-port').value = String(p.port);
    el('fetch-type').value = p.type;
    el('fetch-security').value = p.security;
    el('fetch-hint').textContent = p.signIn && importOffered[p.signIn] ? t(p.signInHint) : t(p.hint);
  });
  el('fetch-type').addEventListener('change', function () {
    var imap = el('fetch-type').value === 'imap';
    var port = el('fetch-port').value;
    if (imap && port === '995') { el('fetch-port').value = '993'; }
    if (!imap && port === '993') { el('fetch-port').value = '995'; }
    el('fetch-mirror').disabled = !imap;
    if (!imap) { el('fetch-mirror').checked = false; }
  });
  el('fetch-cancel').addEventListener('click', function () { fetchFormReset(); say('fetch-status', '', true); });
  el('fetch-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var body = {
      name: el('fetch-name').value.trim(),
      server_address: el('fetch-host').value.trim(),
      port: Number(el('fetch-port').value) || 0,
      server_type: el('fetch-type').value,
      connection_security: el('fetch-security').value,
      username: el('fetch-user').value.trim(),
      days_to_keep_messages: el('fetch-keep').value === '' ? -1 : Number(el('fetch-keep').value),
      minutes_between_fetch: Number(el('fetch-every').value) || 30,
      mirror_folders: el('fetch-type').value === 'imap' && el('fetch-mirror').checked
    };
    if (!body.name || !body.server_address || !body.port) { say('fetch-status', t('Give the account a name, a server and a port.'), false); return; }
    var password = el('fetch-password').value;
    if (password || fetchEditing === null) { body.password = password; }
    var editing = fetchEditing;
    el('fetch-save').disabled = true;
    call(editing === null ? 'POST' : 'PUT', editing === null ? '/api/v1/me/fetch-accounts' : '/api/v1/me/fetch-accounts/' + editing, body).then(function (result) {
      el('fetch-save').disabled = false;
      if (result.status !== 201 && result.status !== 200) { say('fetch-status', describe(result, t('Could not save the account')), false); return; }
      el('fetch-password').value = '';
      say('fetch-status', editing === null ? tf('{0} added. The first collection starts now.', body.name) : t('Saved.'), true);
      fetchFormReset();
      loadFetchAccounts();
    });
  });
  // ---- A second account in the same page ------------------------------------
  //
  // Two mailboxes side by side, which Outlook does and Gmail does not: the
  // account the cookie stands for - the first, the one the page signed in as -
  // and one more, added under Settings with its address and password. The
  // server answers that sign-in in its bearer form (POST /api/v1/session?bearer=1):
  // the session token in the body and no cookie, because a cookie is one per
  // origin and the first account's is already there. The token lives in this
  // closure and nowhere else - not in localStorage, not in sessionStorage, not
  // in a cookie - so a reload forgets it, and the Settings card says so; its
  // ceilings are the server's, the same as any session of that account. Every
  // request made while the second account is the active one carries it as
  // Authorization: Bearer (call and apiFetch, at the top of this file); a
  // frame or a link whose request the browser makes on its own would carry the
  // cookie instead, so for the second account those are fetched here with the
  // token and shown or saved from a blob (renderHtml, guardDownload, imageFrom).
  //
  // A switch is a sign-out of the view and a sign-in of the other account:
  // forgetView drops everything the page holds for the account shown, and load
  // reads the other whole, so the folders, the list, the reading pane, the
  // From of a new message, the contacts and the settings are all the active
  // account's because nothing else is left. What this browser keeps between
  // visits stays the first account's: the offline store (offlineMine), the
  // push subscription and the service worker (loadPush), the app badge. A
  // message opened in one account is answered from that account: the switch
  // closes the message and the compose window, saving a draft of what was
  // written to the account it was written in, so a reply from the other
  // account to it is not a thing.
  //
  // What it costs, said plainly: the token is a credential a script in this
  // page can read, which the cookie (HttpOnly) is not. The page is served
  // under a policy that allows no inline script and no eval and loads nothing
  // from anywhere else (RestApiPortal.cpp), which is what makes that
  // acceptable; the token is never written to storage or into a URL, never
  // logged by the server, and never minted for the administrator.
  // The second account's record, empty: no token, and the two lists of what
  // was announced to it - follow-ups due today, reminders shown - which the
  // first account keeps in the browser and the second keeps here, with the
  // token, so that nothing of it outlives the token.
  var secondBlank = function () {
    return { address: '', token: '', unseen: 0, inboxId: 0, changes: '', idle: 0, lifetime: 0, marks: { due: { day: '', ids: [] }, remind: [] } };
  };
  var accounts = {
    first: { address: '', unseen: 0, inboxId: 0, changes: '' },
    second: secondBlank(),
    active: 'first'
  };
  var onSecond = function () { return accounts.active === 'second' && !!accounts.second.token; };
  // Whether what this browser keeps - the offline store, the outbox, the app
  // badge - is the account shown: it is the first account's, always.
  var offlineMine = function () { return !onSecond(); };
  var otherOf = function () { return accounts.active === 'second' ? 'first' : 'second'; };
  // The token onto a request's headers when the request is the second
  // account's: named by who ('first' or 'second'), or the active account when
  // it is not named. True when it was added.
  var bearerFor = function (h, who) {
    var second = who === 'second' ? true : who === 'first' ? false : onSecond();
    if (!second || !accounts.second.token || h.Authorization) { return false; }
    h.Authorization = 'Bearer ' + accounts.second.token;
    return true;
  };
  var activeUnseen = function () { var inbox = foldersById[inboxId]; return inbox ? (inbox.unseen || 0) : 0; };
  // The two accounts at the top of the folder column, each with its unread
  // count; the one shown is marked, the other is a button that switches.
  var renderAccountNav = function () {
    // What the page cannot offer for the second account is hidden while it
    // is shown rather than left to fail: a window opened on an address
    // loads as a reload of this page would, which is as the first account,
    // and would answer 404 for the second account's message or draft.
    ['message-popout', 'compose-popout'].forEach(function (id) { if (el(id)) { el(id).hidden = onSecond(); } });
    var nav = el('account-nav');
    if (!nav) { return; }
    var has = !!accounts.second.token;
    nav.hidden = !has;
    clear(nav);
    if (!has) { return; }
    ['first', 'second'].forEach(function (which) {
      var rec = accounts[which];
      var shown = which === accounts.active;
      var b = node('button', undefined, shown ? 'on' : '');
      b.type = 'button';
      b.setAttribute('data-account', which);
      b.setAttribute('aria-pressed', shown ? 'true' : 'false');
      b.setAttribute('title', shown ? t('The account shown') : t('Switch to this account'));
      var ico = node('span', undefined, 'ico');
      var av = node('span', initialsOf(rec.address), 'avatar');
      if (av.style) { av.style.background = avatarColour(rec.address); }
      ico.appendChild(av);
      b.appendChild(ico);
      var nm = node('span', rec.address, 'nm');
      nm.appendChild(node('small', which === 'first' ? t('Signed in') : t('Second account')));
      b.appendChild(nm);
      var unseen = shown ? activeUnseen() : (rec.unseen || 0);
      b.appendChild(node('span', unseen ? String(unseen) : '', 'ct'));
      if (unseen) { b.classList.add('unread'); }
      b.addEventListener('click', function () { switchTo(which); });
      nav.appendChild(b);
    });
  };
  // The inbox of the account not shown, read from a folder listing made as
  // it: which folder is its inbox, and how much of it is unread.
  var learnOther = function (which, result) {
    if (result.status !== 200 || !result.data) { return; }
    var list = flatten(result.data.folders || [], [], 0);
    var inbox = list.filter(function (f) { return String(f.path || '').toUpperCase() === 'INBOX'; })[0] || list[0];
    accounts[which].inboxId = inbox ? inbox.id : 0;
    accounts[which].unseen = inbox ? (inbox.unseen || 0) : 0;
    renderAccountNav();
  };
  // The count of the account not shown, kept fresh at the probe's slow pace
  // with the change route, as that account: one small request every half
  // minute while the tab is in front, none while there is no second account.
  var otherProbe = { timer: 0 };
  var otherProbeStop = function () { if (otherProbe.timer) { clearTimeout(otherProbe.timer); otherProbe.timer = 0; } };
  var otherProbeLater = function () {
    otherProbeStop();
    if (!accounts.second.token || el('account').hidden) { return; }
    otherProbe.timer = setTimeout(otherProbeNow, probeSlowly);
  };
  var otherProbeNow = function () {
    otherProbeStop();
    if (!accounts.second.token || el('account').hidden) { return Promise.resolve(); }
    if (document.hidden) { otherProbeLater(); return Promise.resolve(); }
    var which = otherOf();
    var rec = accounts[which];
    return call('GET', '/api/v1/me/changes' + (rec.changes ? '?since=' + encodeURIComponent(rec.changes) : ''), undefined, undefined, which).then(function (result) {
      if (result.status === 200 && result.data) {
        rec.changes = result.data.token || rec.changes;
        (result.data.folders || []).forEach(function (f) { if (f.id === rec.inboxId) { rec.unseen = f.unseen || 0; } });
        renderAccountNav();
      }
      otherProbeLater();
    });
  };
  // The switch. What was being written is kept as a draft of the account it
  // was written in, as closing the compose window keeps it, and a send still
  // waiting on the undo delay goes now - both before the credential changes.
  // Then the view is forgotten and the other account read whole.
  var switchTo = function (which) {
    if (which === accounts.active || (which === 'second' && !accounts.second.token)) { return Promise.resolve(false); }
    var written = !el('compose-section').hidden && (el('compose-to').value.trim() || el('compose-subject').value.trim() || el('compose-text').value.trim());
    var kept = written && !pendingSend ? saveDraft() : Promise.resolve(0);
    return kept.then(function () { return flushPending(); }).then(function () {
      var leaving = accounts[accounts.active];
      leaving.unseen = activeUnseen();
      leaving.inboxId = inboxId;
      leaving.changes = probe.token;
      probeStop();
      otherProbeStop();
      forgetView();
      accounts.active = which;
      probe.token = accounts[which].changes || ''; probe.off = false; probeIn = probeEvery;
      pending = '';
      renderAccountNav();
      return load(false);
    });
  };
  // Adding the second account: its password goes to the server once, in the
  // request that starts its session, and is not kept. The same account twice
  // is refused here, before any request, when the address is the one the
  // page knows: the second sign-in from one device would end the first's
  // session under the one-per-device policy. A spelling the page cannot know
  // to be the same account - the address at an alias domain - is the
  // server's to refuse, which it does with 409, said here in the same words.
  var secondAdd = function (address, password) {
    address = String(address || '').trim().toLowerCase();
    if (!address || !password) { say('second-status', t('Enter the address and the password.'), false); return Promise.resolve(false); }
    if (address === String(accounts.first.address || me || '').toLowerCase()) { say('second-status', t('That is the account you are signed in as.'), false); return Promise.resolve(false); }
    var basic = 'Basic ' + btoa(unescape(encodeURIComponent(address + ':' + password)));
    return call('POST', '/api/v1/session?bearer=1', undefined, { 'Authorization': basic }, 'first').then(function (result) {
      if (result.status === 409) { say('second-status', t('That is the account you are signed in as.'), false); return false; }
      if (result.status !== 201 || !result.data || !result.data.token) { say('second-status', describe(result, t('Could not sign in to the second account')), false); return false; }
      // One second account at a time: another replaces it, and the earlier
      // session is ended rather than left to time out.
      var earlier = accounts.second.token;
      if (earlier) { call('DELETE', '/api/v1/session', undefined, { 'Authorization': 'Bearer ' + earlier }, 'first'); }
      accounts.second = secondBlank();
      accounts.second.address = String(result.data.address || address).toLowerCase();
      accounts.second.token = String(result.data.token);
      accounts.second.idle = Number(result.data.idle_seconds) || 0;
      accounts.second.lifetime = Number(result.data.lifetime_seconds) || 0;
      el('second-address').value = '';
      say('second-status', tf('{0} has been added. Both accounts are at the top of the folder column.', accounts.second.address), true);
      renderSecondCard();
      renderAccountNav();
      return call('GET', '/api/v1/me/folders', undefined, undefined, 'second').then(function (folders) { learnOther('second', folders); otherProbeLater(); return true; });
    });
  };
  var secondEndSession = function () {
    if (!accounts.second.token) { return Promise.resolve(); }
    return call('DELETE', '/api/v1/session', undefined, undefined, 'second').then(function () { /* ended, or gone already */ });
  };
  var secondForget = function () {
    otherProbeStop();
    accounts.second = secondBlank();
  };
  // The page is leaving the second account behind for good - signing out,
  // or the first account's session has ended under it - and the second's
  // session is ended on the server rather than left to stand until it idles
  // out. The page's copy of the token goes first, so that the answer,
  // whatever it is, is not read as that session ending now; the request is
  // fired and not waited for, with keepalive, since the gate is what comes
  // next and the page may be going with it.
  var secondEndQuietly = function () {
    var token = accounts.second.token;
    if (!token) { return; }
    secondForget();
    try {
      apiFetch('/api/v1/session', { method: 'DELETE', keepalive: true, cache: 'no-store', credentials: 'same-origin', headers: { 'X-Requested-With': 'hMailServer', 'Authorization': 'Bearer ' + token } }, 'first')
        .then(null, function () { /* gone already, or no connection */ });
    } catch (e) { /* a browser that refuses the request */ }
  };
  // The second account's session is over - ended by the server, or removed
  // here: the token is forgotten, and when it was the account shown the page
  // is the first account's again. Once: every request in flight on the token
  // answers 401 together, and only the first of them is an event.
  var secondEnded = function (message) {
    if (!accounts.second.token) { return; }
    var wasActive = onSecond();
    secondForget();
    if (wasActive) {
      probeStop();
      // A send still waiting on the undo delay was the second account's, and
      // its session is gone: forgetView drops it (its first act is cancelUndo,
      // so nothing here can send it under the first account's cookie), and
      // the reader is told rather than left to find the message missing.
      if (pendingSend) { toast(t('A message waiting to be sent was not sent: the second account\'s session ended first.')); }
      forgetView();
      accounts.active = 'first';
      probe.token = accounts.first.changes || ''; probe.off = false; probeIn = probeEvery;
      pending = '';
      renderAccountNav();
      load(true).then(function () { if (message) { toast(message); } });
    } else {
      renderAccountNav();
      if (message) { toast(message); }
    }
    renderSecondCard();
  };
  var secondRemove = function () {
    var address = accounts.second.address;
    secondEndSession().then(function () { secondEnded(tf('{0} has been removed from this page.', address)); });
  };
  // The Settings card: the form while there is no second account, and while
  // there is one, what it is and where its session lives.
  var renderSecondCard = function () {
    if (!el('second-current')) { return; }
    var has = !!accounts.second.token;
    el('second-current').hidden = !has;
    el('second-form').hidden = has;
    if (!has) { return; }
    var minutes = Math.round((accounts.second.idle || 0) / 60);
    el('second-current-text').textContent = onSecond()
      ? tf('You are looking at {0}; {1} is the account you signed in as. This session lives in this page only: reloading the page forgets it, and it ends after {2} minutes without use.', accounts.second.address, accounts.first.address, minutes)
      : tf('{0} is added beside {1}. Its session lives in this page only: reloading the page forgets it, and it ends after {2} minutes without use.', accounts.second.address, accounts.first.address, minutes);
    el('second-switch').textContent = onSecond() ? t('Switch back') : t('Switch to it');
  };
  // A link the browser would follow with the cookie - an attachment, the
  // message source, a folder or the whole account as a download - is fetched
  // here with the token when the second account is the one shown, and saved
  // from a blob. The first account's links are untouched: the browser follows
  // them, with the cookie, as before.
  var guardDownload = function (link) {
    link.addEventListener('click', function (event) {
      if (!onSecond()) { return; }
      event.preventDefault();
      var url = link.getAttribute('href') || link.href || '';
      if (!url || url === '#' || url.indexOf('/api/v1/me/') < 0) { return; }
      apiFetch(url, { cache: 'no-store', credentials: 'same-origin' }).then(function (response) {
        return response.status === 200 ? response.blob() : null;
      }, function () { return null; }).then(function (blob) {
        if (!blob) { toast(t('Could not download it.')); return; }
        var a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.setAttribute('download', link.getAttribute('download') || 'download');
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        setTimeout(function () { try { URL.revokeObjectURL(a.href); } catch (e) { /* gone already */ } }, 60000);
      });
    });
  };
  // An image the browser would fetch with the cookie, likewise. The blob URL
  // is released once the image has it, loaded or failed: an image keeps its
  // pixels, not the URL they came from.
  var imageFrom = function (img, url) {
    if (!onSecond()) { img.src = url; return; }
    apiFetch(url, { cache: 'no-store', credentials: 'same-origin' }).then(function (response) {
      return response.status === 200 ? response.blob() : null;
    }, function () { return null; }).then(function (blob) {
      if (!blob) { return; }
      var blobUrl = URL.createObjectURL(blob);
      var release = function () { try { URL.revokeObjectURL(blobUrl); } catch (e) { /* gone already */ } };
      img.addEventListener('load', release);
      img.addEventListener('error', release);
      img.src = blobUrl;
    });
  };
  el('second-form').addEventListener('submit', function (event) {
    event.preventDefault();
    say('second-status', '', true);
    var password = el('second-password').value;
    el('second-password').value = '';
    secondAdd(el('second-address').value, password);
  });
  el('second-end').addEventListener('click', function () { secondRemove(); });
  el('second-switch').addEventListener('click', function () { switchTo(onSecond() ? 'first' : 'second'); });
  ['message-source', 'export-everything', 'folder-export-link'].forEach(function (id) { if (el(id)) { guardDownload(el(id)); } });

  // ---- Recover deleted items -------------------------------------------------
  // What a person deleted within the server's window, put back into the folder
  // it came from or deleted for good. The server keeps the copies and decides
  // where a restored message goes; the page lists and asks.
  el('folder-recover').addEventListener('click', function () { el('list-more').hidden = true; go('/recover'); });
  var renderRecoverable = function (data) {
    var rows = el('recover-rows');
    clear(rows);
    var items = data.items || [];
    recoverDays = Number(data.days) || 0;
    el('recover-note').textContent = recoverDays > 0
      ? tf('Messages you deleted in the last {0} days: put one back and it returns to the folder it was deleted from.', recoverDays)
      : t('Your server keeps nothing that is deleted.');
    items.forEach(function (item) {
      var tr = document.createElement('tr');
      tr.appendChild(node('td', item.subject || t('(no subject)')));
      tr.appendChild(node('td', item.from || ''));
      tr.appendChild(node('td', item.folder || ''));
      tr.appendChild(node('td', new Date(Number(item.deleted) * 1000).toLocaleString() + ' \u00b7 ' + format(item.size || 0)));
      var actions = document.createElement('td');
      var restore = button(t('Put back'), 'btn sm');
      restore.addEventListener('click', function () {
        call('POST', '/api/v1/me/recoverable/' + item.id + '/restore').then(function (result) {
          if (result.status !== 200) { say('recover-status', describe(result, t('Could not put it back')), false); return; }
          say('recover-status', tf('{0} is back where it was.', item.subject || t('(no subject)')), true);
          loadRecoverable();
          loadFolders();
        });
      });
      actions.appendChild(restore);
      var purge = button(t('Delete for good'));
      purge.addEventListener('click', function () {
        if (!window.confirm(tf('Delete {0} for good? It cannot be recovered after this.', item.subject || t('(no subject)')))) { return; }
        call('DELETE', '/api/v1/me/recoverable/' + item.id).then(function (result) {
          if (result.status !== 200) { say('recover-status', describe(result, t('Could not delete it')), false); return; }
          say('recover-status', t('Deleted for good.'), true);
          loadRecoverable();
        });
      });
      actions.appendChild(purge);
      tr.appendChild(actions);
      rows.appendChild(tr);
    });
    el('recover-table').hidden = !items.length;
    el('recover-empty').hidden = items.length > 0 || recoverDays === 0;
  };
  var loadRecoverable = function () {
    return call('GET', '/api/v1/me/recoverable').then(function (result) {
      if (result.status !== 200 || !result.data) { say('recover-status', describe(result, t('Could not read your deleted items')), false); return; }
      renderRecoverable(result.data);
    });
  };
  // ---- Messages that expire ---------------------------------------------------
  // Gmail's confidential mode: an expiry on the compose form keeps the message on
  // this server and sends the recipients a link; this page lists what was sent,
  // whether and by whom it was opened, and withdraws one. Whether the server
  // offers it at all is learned once (it needs a public address for the links).
  el('folder-confidential').addEventListener('click', function () { el('list-more').hidden = true; go('/confidential'); });
  // A message that expires is sent now: the schedule route sends a draft as
  // it is, with no expiry, so Send at goes away while one is chosen and the
  // Schedule button refuses it if reached anyway.
  var expiryChosen = function () { return confidentialOffered && Number(el('compose-expires').value) > 0; };
  var syncExpiry = function () {
    var on = Number(el('compose-expires').value) > 0;
    el('compose-passcode').hidden = !on;
    el('compose-expires-note').hidden = !on;
    el('later-row').hidden = on;
  };
  el('compose-expires').addEventListener('change', syncExpiry);
  var learnConfidential = function () {
    if (confidentialAsked) { return; }
    confidentialAsked = true;
    call('GET', '/api/v1/me/confidential').then(function (result) {
      confidentialOffered = !!(result.status === 200 && result.data && result.data.available);
      el('expires-row').hidden = !confidentialOffered;
      el('folder-confidential').hidden = !(!state.everywhere && folderIs(state.folderId, 'Sent') && confidentialOffered);
    });
  };
  var renderConfidential = function (data) {
    var rows = el('confidential-rows');
    clear(rows);
    var items = data.items || [];
    confidentialOffered = !!data.available;
    el('confidential-note').textContent = confidentialOffered
      ? t('A message sent with an expiry stays here and its recipients read it through a link until then. What was sent, and whether it was opened. Withdrawing one ends it now.')
      : t('Your server does not offer this: it has no public address for the links. Ask whoever runs it.');
    items.forEach(function (item) {
      var tr = document.createElement('tr');
      var subject = document.createElement('td');
      subject.appendChild(node('span', item.subject || t('(no subject)')));
      if (item.passcode) { subject.appendChild(node('span', ' ')); subject.appendChild(node('span', t('Passcode'), 'badge info')); }
      if (item.revoked) { subject.appendChild(node('span', ' ')); subject.appendChild(node('span', t('Withdrawn'), 'badge warn')); }
      else if (item.expired) { subject.appendChild(node('span', ' ')); subject.appendChild(node('span', t('Expired'), 'badge')); }
      tr.appendChild(subject);
      tr.appendChild(node('td', (item.recipients || []).join(', ')));
      tr.appendChild(node('td', new Date(Number(item.expires) * 1000).toLocaleString()));
      tr.appendChild(node('td', Number(item.opened) > 0
        ? new Date(Number(item.opened) * 1000).toLocaleString() + (item.opened_by === 'passcode' ? ' · ' + t('with the passcode') : ' · ' + item.opened_by)
        : t('Not yet')));
      var actions = document.createElement('td');
      if (!item.revoked && !item.expired) {
        var revoke = button(t('Withdraw'), 'btn ghost sm danger');
        revoke.addEventListener('click', function () {
          if (!window.confirm(tf('Withdraw {0}? Nobody can read it after this, and the copy kept here is deleted.', item.subject || t('(no subject)')))) { return; }
          call('POST', '/api/v1/me/confidential/' + item.id + '/revoke').then(function (result) {
            if (result.status !== 200) { say('confidential-status', describe(result, t('Could not withdraw it')), false); return; }
            say('confidential-status', t('Withdrawn; the link no longer works.'), true);
            loadConfidential();
          });
        });
        actions.appendChild(revoke);
      }
      tr.appendChild(actions);
      rows.appendChild(tr);
    });
    el('confidential-table').hidden = !items.length;
    el('confidential-empty').hidden = items.length > 0 || !confidentialOffered;
  };
  var loadConfidential = function () {
    return call('GET', '/api/v1/me/confidential').then(function (result) {
      if (result.status !== 200 || !result.data) { say('confidential-status', describe(result, t('Could not read your messages that expire')), false); return; }
      renderConfidential(result.data);
    });
  };
  // ---- Masked addresses -------------------------------------------------------
  // An address of the reader's own for one sign-up: made in a click, copied,
  // switched off when it turns to spam. The server makes the address and
  // refuses mail to it once it is off; the page lists, copies and switches.
  var renderMaskedAddresses = function (list) {
    var rows = el('masked-rows');
    clear(rows);
    list.forEach(function (m) {
      var tr = document.createElement('tr');
      var cell = document.createElement('td');
      cell.appendChild(node('div', m.address));
      if (!m.active) { cell.appendChild(node('div', t('Switched off: mail to it is refused'), 'muted')); }
      tr.appendChild(cell);
      tr.appendChild(node('td', m.label || ''));
      tr.appendChild(node('td', tf('{0} messages', m.received || 0)));
      var actions = document.createElement('td');
      var copy = button(t('Copy'));
      copy.addEventListener('click', function () {
        if (navigator.clipboard && navigator.clipboard.writeText) {
          navigator.clipboard.writeText(m.address).then(function () { say('masked-status', tf('{0} is copied.', m.address), true); }, function () { say('masked-status', m.address, true); });
        } else {
          say('masked-status', m.address, true);
        }
      });
      actions.appendChild(copy);
      var toggle = button(m.active ? t('Switch off') : t('Switch on'));
      toggle.addEventListener('click', function () {
        call('PUT', '/api/v1/me/masked-addresses/' + m.id, { active: !m.active }).then(function (result) {
          if (result.status !== 200) { say('masked-status', describe(result, t('Could not change it')), false); return; }
          say('masked-status', m.active ? tf('{0} is off: mail to it is refused from now on.', m.address) : tf('{0} is on again.', m.address), true);
          loadMaskedAddresses();
          loadIdentities();
        });
      });
      actions.appendChild(toggle);
      var remove = button(t('Remove'));
      remove.addEventListener('click', function () {
        if (!window.confirm(tf('Remove {0}? Mail to it will be refused, and it cannot be made again.', m.address))) { return; }
        call('DELETE', '/api/v1/me/masked-addresses/' + m.id).then(function (result) {
          if (result.status !== 200) { say('masked-status', describe(result, t('Could not remove it')), false); return; }
          say('masked-status', t('Removed.'), true);
          loadMaskedAddresses();
          loadIdentities();
        });
      });
      actions.appendChild(remove);
      tr.appendChild(actions);
      rows.appendChild(tr);
    });
    el('masked-table').hidden = !list.length;
    el('masked-empty').hidden = list.length > 0;
  };
  var loadMaskedAddresses = function () {
    return call('GET', '/api/v1/me/masked-addresses').then(function (result) {
      var off = result.status === 403;
      el('masked-off').hidden = !off;
      el('masked-form').hidden = off;
      if (off) { el('masked-table').hidden = true; el('masked-empty').hidden = true; return; }
      if (result.status !== 200 || !result.data) { say('masked-status', describe(result, t('Could not read your masked addresses')), false); return; }
      renderMaskedAddresses(result.data.addresses || []);
    });
  };
  el('masked-form').addEventListener('submit', function (event) {
    event.preventDefault();
    el('masked-make').disabled = true;
    call('POST', '/api/v1/me/masked-addresses', { label: el('masked-label').value.trim() }).then(function (result) {
      el('masked-make').disabled = false;
      if (result.status !== 201 || !result.data) { say('masked-status', describe(result, t('Could not make the address')), false); return; }
      el('masked-label').value = '';
      say('masked-status', tf('{0} is yours: give it out, and mail to it comes here.', result.data.address), true);
      if (navigator.clipboard && navigator.clipboard.writeText) { navigator.clipboard.writeText(result.data.address).then(function () {}, function () {}); }
      loadMaskedAddresses();
      loadIdentities();
    });
  });
  // ---- Send mail as ----------------------------------------------------------
  // An address at another provider, proved with a code mailed to it, and then
  // one of the addresses the compose From offers. The server keeps the code and
  // counts the tries; the page shows what it answers, and says plainly when the
  // other domain's policy means mail sent as it from here may not arrive.
  var renderSenderAddresses = function (list) {
    var ul = el('sendas-list');
    clear(ul);
    list.forEach(function (a) {
      var li = document.createElement('li');
      li.appendChild(node('strong', a.address));
      if (a.name) { li.appendChild(node('span', ' ' + a.name, 'muted')); }
      li.appendChild(node('span', ' \u00b7 ' + (a.verified ? t('Verified') : t('Waiting for the code')), a.verified ? 'ok' : 'muted'));
      if (a.dmarc_policy === 'reject' || a.dmarc_policy === 'quarantine') {
        li.appendChild(node('div', tf('{0} asks other servers to refuse mail from it that it did not send itself, so mail sent as this address from here may not arrive.', a.address.split('@')[1] || a.address), 'hint'));
      }
      if (!a.verified) {
        var code = document.createElement('input');
        code.type = 'text';
        code.setAttribute('inputmode', 'numeric');
        code.setAttribute('maxlength', '6');
        code.setAttribute('aria-label', t('Code'));
        code.setAttribute('data-sendas-code', String(a.id));
        li.appendChild(code);
        var verify = button(t('Verify'), 'btn sm');
        verify.addEventListener('click', function () {
          call('POST', '/api/v1/me/sender-addresses/' + a.id + '/verify', { code: String(code.value || '').trim() }).then(function (result) {
            if (result.status === 200) {
              say('sendas-status', tf('{0} is verified: you can write as it now.', a.address), true);
              loadSenderAddresses();
              loadIdentities();
              return;
            }
            if (result.status === 400 && result.data && result.data.attempts_left !== undefined) { say('sendas-status', tf('That is not the code. {0} tries left.', result.data.attempts_left), false); return; }
            if (result.status === 410) { say('sendas-status', t('That code no longer works: send a new one.'), false); return; }
            say('sendas-status', describe(result, t('Could not check the code')), false);
          });
        });
        li.appendChild(verify);
        var resend = button(t('Send a new code'));
        resend.addEventListener('click', function () {
          call('POST', '/api/v1/me/sender-addresses/' + a.id + '/resend').then(function (result) {
            if (result.status === 202) { say('sendas-status', tf('A new code is on its way to {0}.', a.address), true); return; }
            if (result.status === 429 && result.data) { say('sendas-status', tf('Wait {0} seconds before sending another.', result.data.retry_after), false); return; }
            say('sendas-status', describe(result, t('Could not send a new code')), false);
          });
        });
        li.appendChild(resend);
      }
      var remove = button(t('Remove'));
      remove.addEventListener('click', function () {
        if (!window.confirm(tf('Stop sending as {0}?', a.address))) { return; }
        call('DELETE', '/api/v1/me/sender-addresses/' + a.id).then(function (result) {
          if (result.status !== 200) { say('sendas-status', describe(result, t('Could not remove it')), false); return; }
          say('sendas-status', t('Removed.'), true);
          loadSenderAddresses();
          loadIdentities();
        });
      });
      li.appendChild(remove);
      ul.appendChild(li);
    });
  };
  var loadSenderAddresses = function () {
    return call('GET', '/api/v1/me/sender-addresses').then(function (result) {
      if (result.status === 403) { el('sendas').hidden = true; return; }
      el('sendas').hidden = false;
      if (result.status !== 200 || !result.data) { say('sendas-status', describe(result, t('Could not read your addresses')), false); return; }
      renderSenderAddresses(result.data.addresses || []);
    });
  };
  el('sendas-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var body = { address: el('sendas-address').value.trim(), name: el('sendas-name').value.trim() };
    if (!body.address) { say('sendas-status', t('Give the address.'), false); return; }
    el('sendas-add').disabled = true;
    call('POST', '/api/v1/me/sender-addresses', body).then(function (result) {
      el('sendas-add').disabled = false;
      if (result.status !== 201) { say('sendas-status', describe(result, t('Could not add the address')), false); return; }
      el('sendas-address').value = '';
      el('sendas-name').value = '';
      say('sendas-status', result.data && result.data.code_sent ? tf('A code is on its way to {0}. Type it below.', body.address) : t('The address is added, but the code could not be sent: send a new one.'), !!(result.data && result.data.code_sent));
      loadSenderAddresses();
    });
  });
  var loadDigestSetting = function () {
    return call('GET', '/api/v1/me/quarantine-digest').then(function (result) {
      if (result.status !== 200 || !result.data) { return; }
      renderDigestSetting(result.data);
    });
  };
  // The account's rules as the server has them now - the page's copy is only
  // as fresh as the last visit to the Rules page - or a word when the script
  // was written by hand, in which case nothing is written over it.
  var withRules = function (fn) {
    return call('GET', '/api/v1/me/filters').then(function (result) {
      if (result.status === 200 && result.data) { el('filter-script').value = result.data.active || ''; }
      var rules = rulesOf(el('filter-script').value);
      if (rules === null) { toast(t('The filter script was written by hand: add the rule there.')); return null; }
      return fn(rules);
    });
  };
  // Every message from the sender in the folder, deleted in one go, with undo.
  var sweepSender = function (address, folderId) {
    address = String(address || '').trim();
    if (!address || !folderId) { return; }
    call('GET', '/api/v1/me/folders/' + folderId + '/messages?q=' + encodeURIComponent('from:' + address) + '&limit=200').then(function (result) {
      if (result.status !== 200 || !result.data) { say('mail-status', describe(result, t('Could not read the folder')), false); return; }
      var hits = (result.data.messages || []).filter(function (m) { return addressOf(m.from).toLowerCase() === address.toLowerCase(); });
      if (!hits.length) { toast(tf('Nothing from {0} in this folder.', address)); return; }
      fileMany(hits.map(function (m) { return { id: m.id, folderId: folderId }; }), 'delete').then(function (ok) {
        if (ok && current && hits.some(function (m) { return m.id === current.id; })) { closeMessage(true); }
        loadMessages();
      });
    });
  };
  var dragged = null;
  var rowGestures = function (row, entry) {
    row.setAttribute('draggable', 'true');
    row.addEventListener('dragstart', function (event) {
      var ids = selected[entry.id] ? selectedIds() : (entry.ids || [entry.id]);
      dragged = { ids: ids, folderId: entry.folderId };
      row.classList.add('dragging');
      if (event.dataTransfer) { try { event.dataTransfer.setData('text/plain', ids.join(',')); event.dataTransfer.effectAllowed = 'move'; } catch (e) { /* the harness's event has none */ } }
    });
    row.addEventListener('dragend', function () { row.classList.remove('dragging'); dragged = null; });
    row.addEventListener('contextmenu', function (event) {
      event.preventDefault();
      openContextMenu(entry, event.clientX || 0, event.clientY || 0);
    });
  };
  var dropTarget = function (button, folderId) {
    button.addEventListener('dragover', function (event) { if (!dragged || dragged.folderId === folderId) { return; } event.preventDefault(); button.classList.add('drop'); });
    button.addEventListener('dragleave', function () { button.classList.remove('drop'); });
    button.addEventListener('drop', function (event) {
      event.preventDefault();
      button.classList.remove('drop');
      if (!dragged) { return; }
      var moving = dragged;
      dragged = null;
      fileMany(moving.ids.map(function (id) { return { id: id, folderId: moving.folderId }; }), folderId).then(function (ok) {
        if (ok && current && moving.ids.indexOf(current.id) >= 0) { closeMessage(true); }
        clearSelection();
        loadMessages();
      });
    });
  };
  var contextRow = null;
  var openContextMenu = function (entry, x, y) {
    closeMenus();
    contextRow = entry;
    var menu = el('context-menu');
    var m = entry.m;
    var items = menu.children;
    for (var i = 0; i < items.length; i++) {
      var act = items[i].getAttribute ? items[i].getAttribute('data-act') : '';
      if (act === 'read') { items[i].textContent = m.flags.seen ? t('Mark as unread') : t('Mark as read'); }
      if (act === 'star') { items[i].textContent = starTitle(starStage(m)); }
      if (act === 'pin') { items[i].textContent = isPinned(m) ? t('Unpin') : t('Pin'); }
      if (act === 'mute') { items[i].textContent = isMuted(m) ? t('Unmute') : t('Mute'); }
      if (act === 'important') { items[i].textContent = isImportant(m) ? t('Not important') : t('Mark as important'); items[i].hidden = !markable(entry.folderId); }
      // Only a conversation has anything to clean up.
      if (act === 'cleanup') { items[i].hidden = !(entry.ms && entry.ms.length > 1); }
      if (act === 'junk') { items[i].textContent = folderIs(entry.folderId, 'Junk') ? t('Not junk') : t('Junk'); }
    }
    renderContextTabs(m);
    menu.hidden = false;
    if (menu.style) {
      var w = window.innerWidth || 1200, h = window.innerHeight || 800;
      // At the pointer, opening away from it: to the right of it normally, to
      // the left of it when the page is written right to left.
      var rtl = document.documentElement && document.documentElement.getAttribute && document.documentElement.getAttribute('dir') === 'rtl';
      menu.style.left = Math.max(4, Math.min(rtl ? x - 240 : x, w - 240)) + 'px';
      menu.style.top = Math.max(4, Math.min(y, h - 320)) + 'px';
    }
  };
  var closeContextMenu = function () { el('context-menu').hidden = true; contextRow = null; };
  el('context-menu').addEventListener('click', function (event) {
    var act = event.target && event.target.getAttribute ? event.target.getAttribute('data-act') : '';
    var entry = contextRow;
    closeContextMenu();
    if (!act || !entry) { return; }
    var m = entry.m;
    if (act === 'open') { go('/m/' + entry.id); }
    else if (act === 'archive') { fileRow(entry, 'archive'); }
    else if (act === 'junk') { fileRow(entry, junkTargetFor(entry.folderId)); }
    else if (act === 'delete') { fileRow(entry, 'delete'); }
    else if (act === 'read') { markRows([entry], !m.flags.seen); }
    else if (act === 'star') { cycleStar(m, entry.star); }
    else if (act === 'pin') { pinMessages(entry.ids || [entry.id], !isPinned(m)); }
    else if (act === 'important') { markImportant(entry.ms || [m], !isImportant(m)); }
    else if (act === 'block') { blockSender(addressOf(m.from)); }
    else if (act === 'sweep') { sweepSender(addressOf(m.from), entry.folderId); }
    else if (act === 'mute') { muteThread(entry, !isMuted(m)); }
    else if (act === 'cleanup') { cleanUpConversation(entry.ms || [m]); }
    else if (act === 'tab') { moveSenderToTab(addressOf(m.from), event.target.getAttribute('data-tab')); }
  });
  document.addEventListener('click', function (event) {
    if (el('context-menu').hidden) { return; }
    if (!within(event.target, el('context-menu'))) { closeContextMenu(); }
  });
  document.addEventListener('keydown', function (event) { if (event.key === 'Escape' && !el('context-menu').hidden) { closeContextMenu(); } });

  // ---- The calendar --------------------------------------------------------
  //
  // Month, week, day and agenda views over /api/v1/me/calendar/events, which
  // the server answers occurrence by occurrence across every calendar the
  // account can see; the event editor, with a recurrence, a reminder and the
  // attendees; drag to move and drag to resize; the calendars themselves with
  // their colours and shares; and reminders while the page is open. Every
  // time is shown in the reader's zone (the one chosen in Settings, else the
  // browser's), and written back with that zone, so a weekly 09:00 stays
  // 09:00 across a clock change.
  var calState = { view: 'month', anchor: new Date(), calendars: [], events: [], window: '', loaded: false, editing: null, detail: 0 };
  var CAL_HIDDEN = 'hmPortalCalendarsHidden';
  var CAL_VIEW = 'hmPortalCalendarView';
  var CAL_HOUR = 48;
  var calZone = function () {
    var chosen = pref('timezone');
    if (chosen) { return chosen; }
    try { return Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC'; } catch (e) { return 'UTC'; }
  };
  var zoneList = function () {
    var zones = [];
    try { zones = (typeof Intl !== 'undefined' && Intl.supportedValuesOf) ? Intl.supportedValuesOf('timeZone') : []; } catch (e) { zones = []; }
    if (!zones.length) { zones = ['UTC', 'Europe/London', 'Europe/Paris', 'Europe/Berlin', 'Europe/Madrid', 'Europe/Rome', 'Europe/Amsterdam', 'Europe/Stockholm', 'Europe/Warsaw', 'Europe/Athens', 'Europe/Kiev', 'Europe/Moscow', 'America/New_York', 'America/Chicago', 'America/Denver', 'America/Los_Angeles', 'America/Sao_Paulo', 'Asia/Tokyo', 'Asia/Shanghai', 'Asia/Kolkata', 'Asia/Dubai', 'Australia/Sydney']; }
    return zones;
  };
  // The wall clock of an instant in a zone, and back.
  var WEEKDAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
  var wallOf = function (date, zone) {
    try {
      var parts = new Intl.DateTimeFormat('en-US', { timeZone: zone, hourCycle: 'h23', year: 'numeric', month: 'numeric', day: 'numeric', hour: 'numeric', minute: 'numeric', weekday: 'short' }).formatToParts(date);
      var o = {};
      parts.forEach(function (p) { o[p.type] = p.value; });
      return { y: +o.year, m: +o.month, d: +o.day, hh: (+o.hour) % 24, mm: +o.minute, wd: Math.max(0, WEEKDAYS.indexOf(o.weekday)) };
    } catch (e) {
      return { y: date.getUTCFullYear(), m: date.getUTCMonth() + 1, d: date.getUTCDate(), hh: date.getUTCHours(), mm: date.getUTCMinutes(), wd: date.getUTCDay() };
    }
  };
  var instantOf = function (y, m, d, hh, mm, zone) {
    var wanted = Date.UTC(y, m - 1, d, hh, mm);
    var guess = wanted;
    for (var i = 0; i < 2; i++) {
      var w = wallOf(new Date(guess), zone);
      guess += wanted - Date.UTC(w.y, w.m - 1, w.d, w.hh, w.mm);
    }
    return new Date(guess);
  };
  var parseUtc = function (text) {
    var m = /^(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})Z$/.exec(String(text || ''));
    if (!m) { return null; }
    return new Date(Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]));
  };
  var utcText = function (date) {
    return date.getUTCFullYear() + pad2(date.getUTCMonth() + 1) + pad2(date.getUTCDate()) + 'T' + pad2(date.getUTCHours()) + pad2(date.getUTCMinutes()) + pad2(date.getUTCSeconds()) + 'Z';
  };
  var dayKeyOf = function (y, m, d) { return y + '-' + pad2(m) + '-' + pad2(d); };
  var wallKey = function (date) { var w = wallOf(date, calZone()); return dayKeyOf(w.y, w.m, w.d); };
  var wallInput = function (date) { var w = wallOf(date, calZone()); return dayKeyOf(w.y, w.m, w.d) + 'T' + pad2(w.hh) + ':' + pad2(w.mm); };
  var dayOffset = function (y, m, d, days) { var t = new Date(Date.UTC(y, m - 1, d + days)); return { y: t.getUTCFullYear(), m: t.getUTCMonth() + 1, d: t.getUTCDate(), wd: t.getUTCDay() }; };
  var keyOffset = function (key, days) { var p = key.split('-'); var o = dayOffset(+p[0], +p[1], +p[2], days); return dayKeyOf(o.y, o.m, o.d); };
  var monthNames = function (y, m) {
    try { return new Date(Date.UTC(y, m - 1, 1, 12)).toLocaleDateString(languageActive, { timeZone: 'UTC', month: 'long', year: 'numeric' }); } catch (e) { return y + '-' + pad2(m); }
  };
  var dayName = function (key, style) {
    var p = key.split('-');
    try { return new Date(Date.UTC(+p[0], +p[1] - 1, +p[2], 12)).toLocaleDateString(languageActive, style === 'long' ? { timeZone: 'UTC', weekday: 'long', day: 'numeric', month: 'long' } : { timeZone: 'UTC', weekday: 'short', day: 'numeric' }); } catch (e) { return key; }
  };
  var timeText = function (date) {
    try { return date.toLocaleTimeString(languageActive, zoneOptions({ hour: '2-digit', minute: '2-digit', hour12: false })); } catch (e) { var w = wallOf(date, calZone()); return pad2(w.hh) + ':' + pad2(w.mm); }
  };
  var spanText = function (ev) {
    var s = parseUtc(ev.start), e = parseUtc(ev.end);
    if (!s) { return ''; }
    if (ev.all_day) {
      var last = keyOffset(ev.local_end, -1);
      return ev.local_start === last || !ev.local_end ? dayName(ev.local_start, 'long') : dayName(ev.local_start, 'long') + ' – ' + dayName(last, 'long');
    }
    var sameDay = e && wallKey(s) === wallKey(e);
    return dayName(wallKey(s), 'long') + ' ' + timeText(s) + (e && e > s ? ' – ' + (sameDay ? '' : dayName(wallKey(e), 'long') + ' ') + timeText(e) : '');
  };
  var hiddenCalendars = function () { try { return JSON.parse(localStorage.getItem(CAL_HIDDEN) || '[]') || []; } catch (e) { return []; } };
  var calendarShown = function (id) { return hiddenCalendars().indexOf(Number(id)) < 0; };
  var calendarById = function (id) { return calState.calendars.filter(function (c) { return c.id === Number(id); })[0] || null; };
  var calendarColor = function (id) { var c = calendarById(id); return (c && c.color) || 'var(--accent)'; };
  var writableCalendars = function () { return calState.calendars.filter(function (c) { return c.writable; }); };

  var calWindow = function () {
    var zone = calZone();
    var w = wallOf(calState.anchor, zone);
    var start, end;
    if (calState.view === 'month') {
      var first = dayOffset(w.y, w.m, 1, 0);
      var offset = (first.wd + 6) % 7;
      var s = dayOffset(first.y, first.m, first.d, -offset);
      start = instantOf(s.y, s.m, s.d, 0, 0, zone);
      var e = dayOffset(s.y, s.m, s.d, 42);
      end = instantOf(e.y, e.m, e.d, 0, 0, zone);
    } else if (calState.view === 'week') {
      var monday = dayOffset(w.y, w.m, w.d, -((w.wd + 6) % 7));
      start = instantOf(monday.y, monday.m, monday.d, 0, 0, zone);
      var sunday = dayOffset(monday.y, monday.m, monday.d, 7);
      end = instantOf(sunday.y, sunday.m, sunday.d, 0, 0, zone);
    } else if (calState.view === 'day') {
      start = instantOf(w.y, w.m, w.d, 0, 0, zone);
      var next = dayOffset(w.y, w.m, w.d, 1);
      end = instantOf(next.y, next.m, next.d, 0, 0, zone);
    } else {
      start = instantOf(w.y, w.m, w.d, 0, 0, zone);
      var later = dayOffset(w.y, w.m, w.d, 31);
      end = instantOf(later.y, later.m, later.d, 0, 0, zone);
    }
    return { start: start, end: end };
  };
  var calTitle = function () {
    var w = wallOf(calState.anchor, calZone());
    if (calState.view === 'month') { return monthNames(w.y, w.m); }
    if (calState.view === 'day') { return dayName(dayKeyOf(w.y, w.m, w.d), 'long'); }
    var range = calWindow();
    var a = wallOf(range.start, calZone()), b = wallOf(new Date(range.end.getTime() - 60000), calZone());
    return dayName(dayKeyOf(a.y, a.m, a.d)) + ' – ' + dayName(dayKeyOf(b.y, b.m, b.d)) + ', ' + b.y;
  };

  var loadCalendars = function (force) {
    if (calState.calendars.length && !force) { return Promise.resolve(calState.calendars); }
    return call('GET', '/api/v1/me/calendars').then(function (result) {
      if (result.status !== 200 || !result.data) { say('cal-status', describe(result, t('Could not read the calendars')), false); return calState.calendars; }
      calState.calendars = result.data.calendars || [];
      renderCalendarList();
      return calState.calendars;
    });
  };
  var loadEvents = function () {
    var range = calWindow();
    var key = utcText(range.start) + utcText(range.end);
    return call('GET', '/api/v1/me/calendar/events?start=' + utcText(range.start) + '&end=' + utcText(range.end)).then(function (result) {
      if (result.status !== 200 || !result.data) { say('cal-status', describe(result, t('Could not read the events')), false); return; }
      calState.events = result.data.events || [];
      calState.window = key;
      calState.loaded = true;
      renderCalendar();
    });
  };
  var openCalendar = function (params) {
    try { var kept = localStorage.getItem(CAL_VIEW); if (kept && ['month', 'week', 'day', 'agenda'].indexOf(kept) >= 0 && !(params && params.view)) { calState.view = kept; } } catch (e) { /* no storage */ }
    if (params && params.view && ['month', 'week', 'day', 'agenda'].indexOf(params.view) >= 0) { calState.view = params.view; }
    el('cal-zone').textContent = tf('Times are shown in {0}.', calZone());
    fillZoneSelect(el('event-tz'));
    loadCalendars().then(loadEvents);
  };
  var fillZoneSelect = function (select) {
    if (select.children.length) { return; }
    zoneList().forEach(function (z) { var o = document.createElement('option'); o.value = z; o.textContent = z; select.appendChild(o); });
  };

  // ---- the calendars beside the grid
  var renderCalendarList = function () {
    var list = el('cal-list');
    clear(list);
    var addList = el('cal-add-form');
    addList.hidden = false;
    calState.calendars.forEach(function (c) {
      var row = node('div', undefined, 'cal-row');
      var box = document.createElement('input');
      box.type = 'checkbox';
      box.checked = calendarShown(c.id);
      box.setAttribute('aria-label', c.display_name);
      box.addEventListener('change', function () {
        var hidden = hiddenCalendars().filter(function (id) { return id !== c.id; });
        if (!box.checked) { hidden.push(c.id); }
        try { localStorage.setItem(CAL_HIDDEN, JSON.stringify(hidden)); } catch (e) { /* no storage */ }
        renderCalendar();
      });
      row.appendChild(box);
      var dot = node('span', undefined, 'cal-dot');
      dot.style.background = c.color || 'var(--accent)';
      row.appendChild(dot);
      var name = node('span', c.display_name + (c.mine ? '' : ' (' + c.owner + ')'), 'cal-name');
      name.setAttribute('title', c.mine ? c.display_name : tf('Shared by {0}', c.owner) + (c.writable ? '' : ' · ' + t('read-only')));
      row.appendChild(name);
      var edit = iconButton('pen', c.mine ? t('Edit calendar') : t('Calendar details'));
      edit.addEventListener('click', function () { calState.detail = calState.detail === c.id ? 0 : c.id; renderCalendarDetail(); });
      row.appendChild(edit);
      list.appendChild(row);
    });
    renderCalendarDetail();
    var choice = el('event-calendar');
    clear(choice);
    writableCalendars().forEach(function (c) { var o = document.createElement('option'); o.value = String(c.id); o.textContent = c.mine ? c.display_name : c.display_name + ' (' + c.owner + ')'; choice.appendChild(o); });
    var taskChoice = el('task-add-list');
    if (taskChoice) { clear(taskChoice); writableCalendars().forEach(function (c) { var o = document.createElement('option'); o.value = String(c.id); o.textContent = c.display_name; taskChoice.appendChild(o); }); }
    var taskEdit = el('task-list-select');
    if (taskEdit) { clear(taskEdit); writableCalendars().forEach(function (c) { var o = document.createElement('option'); o.value = String(c.id); o.textContent = c.display_name; taskEdit.appendChild(o); }); }
  };
  var renderCalendarDetail = function () {
    var box = el('cal-detail');
    clear(box);
    var c = calendarById(calState.detail);
    box.hidden = !c;
    if (!c) { return; }
    box.appendChild(node('h3', c.display_name));
    if (!c.mine) {
      box.appendChild(node('p', tf('Shared by {0}', c.owner) + ' · ' + (c.writable ? t('you can add and change events') : t('read-only')), 'hint'));
      var leave = button(t('Leave this calendar'), 'btn ghost sm');
      leave.addEventListener('click', function () {
        call('DELETE', '/api/v1/me/calendars/' + c.id).then(function (result) {
          if (result.status !== 200) { say('cal-status', describe(result, t('Could not leave the calendar')), false); return; }
          calState.detail = 0;
          loadCalendars(true).then(loadEvents);
        });
      });
      box.appendChild(leave);
      return;
    }
    var form = node('form', undefined, 'row');
    var name = document.createElement('input'); name.type = 'text'; name.value = c.display_name; name.setAttribute('aria-label', t('Calendar name')); name.setAttribute('maxlength', '255'); name.id = 'cal-detail-name';
    var color = document.createElement('input'); color.type = 'color'; color.value = c.color || '#1a73e8'; color.setAttribute('aria-label', t('Colour')); color.id = 'cal-detail-color';
    var save = button(t('Save'), 'btn sm'); save.type = 'submit';
    form.appendChild(name); form.appendChild(color); form.appendChild(save);
    form.addEventListener('submit', function (event) {
      event.preventDefault();
      call('PUT', '/api/v1/me/calendars/' + c.id, { display_name: name.value, color: color.value }).then(function (result) {
        if (result.status !== 200) { say('cal-status', describe(result, t('Could not save the calendar')), false); return; }
        say('cal-status', t('Calendar saved.'), true);
        loadCalendars(true).then(renderCalendar);
      });
    });
    box.appendChild(form);

    box.appendChild(node('h3', t('Sharing')));
    var shareForm = node('form', undefined, 'row');
    var address = document.createElement('input'); address.type = 'text'; address.setAttribute('placeholder', 'someone@' + (me.split('@')[1] || 'example.com')); address.setAttribute('aria-label', t('Share with')); address.id = 'cal-share-address';
    var writeLabel = node('label'); var write = document.createElement('input'); write.type = 'checkbox'; write.id = 'cal-share-write'; writeLabel.appendChild(write); writeLabel.appendChild(node('span', ' ' + t('Can edit')));
    var share = button(t('Share'), 'btn sm'); share.type = 'submit';
    shareForm.appendChild(address); shareForm.appendChild(writeLabel); shareForm.appendChild(share);
    shareForm.addEventListener('submit', function (event) {
      event.preventDefault();
      var who = address.value.trim().toLowerCase();
      if (!who) { return; }
      call('PUT', '/api/v1/me/calendars/' + c.id + '/shares/' + encodeURIComponent(who), { write: write.checked }).then(function (result) {
        if (result.status !== 200) { say('cal-status', describe(result, t('Could not share the calendar')), false); return; }
        say('cal-status', tf('Shared with {0}.', who), true);
        loadCalendars(true);
      });
    });
    box.appendChild(shareForm);
    (c.shares || []).forEach(function (s) {
      var row = node('div', undefined, 'row cal-share');
      row.appendChild(node('span', s.address + ' · ' + (s.write ? t('can edit') : t('read-only'))));
      var remove = button(t('Remove'), 'btn quiet sm');
      remove.addEventListener('click', function () {
        call('DELETE', '/api/v1/me/calendars/' + c.id + '/shares/' + encodeURIComponent(s.address)).then(function (result) {
          if (result.status !== 200) { say('cal-status', describe(result, t('Could not remove the share')), false); return; }
          loadCalendars(true);
        });
      });
      row.appendChild(remove);
      box.appendChild(row);
    });
    box.appendChild(node('p', t('Someone you share with sees this calendar in their webmail and on their phone, read-only unless they can edit.'), 'hint'));
    if (!c.is_default) {
      var del = button(t('Delete this calendar'), 'btn quiet sm');
      del.addEventListener('click', function () {
        if (!window.confirm(tf('Delete the calendar "{0}" and every event in it?', c.display_name))) { return; }
        call('DELETE', '/api/v1/me/calendars/' + c.id).then(function (result) {
          if (result.status !== 200) { say('cal-status', describe(result, t('Could not delete the calendar')), false); return; }
          calState.detail = 0;
          loadCalendars(true).then(loadEvents);
        });
      });
      box.appendChild(del);
    }
  };
  el('cal-add-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var name = el('cal-add-name').value.trim();
    if (!name) { return; }
    call('POST', '/api/v1/me/calendars', { display_name: name, color: el('cal-add-color').value }).then(function (result) {
      if (result.status !== 201) { say('cal-status', describe(result, t('Could not add the calendar')), false); return; }
      el('cal-add-name').value = '';
      say('cal-status', tf('Calendar "{0}" added.', name), true);
      loadCalendars(true);
    });
  });

  // ---- the views
  var visibleEvents = function () { return calState.events.filter(function (ev) { return calendarShown(ev.calendar); }); };
  var eventDays = function (ev) {
    // The day keys, in the reader's zone, an occurrence touches.
    var keys = [];
    if (ev.all_day) {
      var k = ev.local_start, last = keyOffset(ev.local_end || ev.local_start, -1);
      if (last < k) { last = k; }
      for (var i = 0; i < 60 && k <= last; i++) { keys.push(k); k = keyOffset(k, 1); }
      return keys;
    }
    var s = parseUtc(ev.start), e = parseUtc(ev.end) || s;
    if (!s) { return keys; }
    var from = wallKey(s), to = wallKey(new Date(Math.max(s.getTime(), e.getTime() - 60000)));
    var key = from;
    for (var j = 0; j < 60 && key <= to; j++) { keys.push(key); key = keyOffset(key, 1); }
    return keys;
  };
  var chipFor = function (ev, withTime) {
    var chip = node('button', undefined, 'cal-chip' + (ev.writable ? '' : ' ro') + (ev.status === 'CANCELLED' ? ' cancelled' : '') + (ev.my_partstat === 'DECLINED' ? ' declined' : ''));
    chip.type = 'button';
    chip.style.background = calendarColor(ev.calendar);
    var s = parseUtc(ev.start);
    if (withTime && !ev.all_day && s) { chip.appendChild(node('span', timeText(s), 'tm')); }
    chip.appendChild(node('span', ev.summary || t('(no title)'), 'nm'));
    chip.setAttribute('title', (ev.summary || '') + '\n' + spanText(ev) + (ev.location ? '\n' + ev.location : ''));
    chip.setAttribute('data-event', String(ev.id));
    chip.setAttribute('data-instance', ev.recurrence_id || '');
    chip.addEventListener('click', function (event) { event.stopPropagation(); openEvent(ev, null); });
    if (ev.writable) {
      chip.draggable = true;
      chip.setAttribute('draggable', 'true');
      chip.addEventListener('dragstart', function (event) { calDrag = ev; try { event.dataTransfer.setData('text/plain', String(ev.id)); event.dataTransfer.effectAllowed = 'move'; } catch (e) { /* the test DOM has no transfer */ } });
      chip.addEventListener('dragend', function () { calDrag = null; });
    }
    return chip;
  };
  var calDrag = null;
  var calDropTarget = function (cell, onDrop) {
    cell.addEventListener('dragover', function (event) { if (calDrag) { event.preventDefault(); cell.classList.add('over'); } });
    cell.addEventListener('dragleave', function () { cell.classList.remove('over'); });
    cell.addEventListener('drop', function (event) { event.preventDefault(); cell.classList.remove('over'); if (calDrag) { var ev = calDrag; calDrag = null; onDrop(ev); } });
  };
  var renderCalendar = function () {
    if (!calState.loaded) { return; }
    el('cal-title').textContent = calTitle();
    var tabs = el('cal-views').children;
    for (var i = 0; i < tabs.length; i++) { tabs[i].classList.toggle('on', tabs[i].getAttribute('data-cal-view') === calState.view); }
    var grid = el('cal-grid');
    clear(grid);
    if (calState.view === 'month') { renderMonth(grid); }
    else if (calState.view === 'agenda') { renderAgenda(grid); }
    else { renderWeek(grid, calState.view === 'day' ? 1 : 7); }
  };
  var byDay = function () {
    var map = {};
    visibleEvents().forEach(function (ev) { eventDays(ev).forEach(function (k) { (map[k] = map[k] || []).push(ev); }); });
    Object.keys(map).forEach(function (k) { map[k].sort(function (a, b) { return (a.all_day === b.all_day ? 0 : a.all_day ? -1 : 1) || (a.start < b.start ? -1 : a.start > b.start ? 1 : 0); }); });
    return map;
  };
  var renderMonth = function (grid) {
    var zone = calZone();
    var w = wallOf(calState.anchor, zone);
    var first = dayOffset(w.y, w.m, 1, 0);
    var start = dayOffset(first.y, first.m, first.d, -((first.wd + 6) % 7));
    var today = wallKey(new Date());
    var map = byDay();
    var table = node('div', undefined, 'cal-month');
    var head = node('div', undefined, 'cal-month-head');
    for (var h = 0; h < 7; h++) { var d0 = dayOffset(start.y, start.m, start.d, h); head.appendChild(node('span', dayName(dayKeyOf(d0.y, d0.m, d0.d)).replace(/[\d\s.,]+$/, '').replace(/^[\d\s.,]+/, ''))); }
    table.appendChild(head);
    for (var i = 0; i < 42; i++) {
      var d = dayOffset(start.y, start.m, start.d, i);
      var key = dayKeyOf(d.y, d.m, d.d);
      var cell = node('div', undefined, 'cal-cell' + (d.m !== w.m ? ' other' : '') + (key === today ? ' today' : ''));
      cell.setAttribute('data-day', key);
      var num = node('button', String(d.d), 'cal-daynum');
      num.type = 'button';
      num.setAttribute('aria-label', dayName(key, 'long'));
      (function (k) {
        num.addEventListener('click', function (event) { event.stopPropagation(); calState.anchor = instantOf(+k.slice(0, 4), +k.slice(5, 7), +k.slice(8, 10), 12, 0, calZone()); setCalView('day'); });
        cell.addEventListener('click', function () { openEvent(null, { day: k }); });
        calDropTarget(cell, function (ev) { moveEventToDay(ev, k); });
      })(key);
      cell.appendChild(num);
      (map[key] || []).forEach(function (ev) { cell.appendChild(chipFor(ev, true)); });
      table.appendChild(cell);
    }
    grid.appendChild(table);
  };
  var renderWeek = function (grid, days) {
    var zone = calZone();
    var w = wallOf(calState.anchor, zone);
    var start = days === 1 ? dayOffset(w.y, w.m, w.d, 0) : dayOffset(w.y, w.m, w.d, -((w.wd + 6) % 7));
    var today = wallKey(new Date());
    var map = byDay();
    var keys = [];
    for (var i = 0; i < days; i++) { var d = dayOffset(start.y, start.m, start.d, i); keys.push(dayKeyOf(d.y, d.m, d.d)); }
    var wrap = node('div', undefined, 'cal-week' + (days === 1 ? ' one' : ''));
    var head = node('div', undefined, 'cal-week-head');
    head.appendChild(node('span', '', 'gutter'));
    keys.forEach(function (k) { head.appendChild(node('span', dayName(k), k === today ? 'today' : '')); });
    wrap.appendChild(head);
    var allDay = node('div', undefined, 'cal-allday');
    allDay.appendChild(node('span', t('all day'), 'gutter'));
    keys.forEach(function (k) {
      var cell = node('div', undefined, 'cal-allday-cell');
      cell.setAttribute('data-day', k);
      (map[k] || []).filter(function (ev) { return ev.all_day; }).forEach(function (ev) { cell.appendChild(chipFor(ev, false)); });
      cell.addEventListener('click', function () { openEvent(null, { day: k, allDay: true }); });
      calDropTarget(cell, function (ev) { moveEventToDay(ev, k); });
      allDay.appendChild(cell);
    });
    wrap.appendChild(allDay);
    var body = node('div', undefined, 'cal-hours');
    var gutter = node('div', undefined, 'cal-gutter');
    for (var h = 0; h < 24; h++) { gutter.appendChild(node('span', pad2(h) + ':00')); }
    body.appendChild(gutter);
    keys.forEach(function (k) {
      var col = node('div', undefined, 'cal-col' + (k === today ? ' today' : ''));
      col.setAttribute('data-day', k);
      for (var hh = 0; hh < 24; hh++) {
        var slot = node('div', undefined, 'cal-slot');
        slot.setAttribute('data-day', k);
        slot.setAttribute('data-hour', String(hh));
        (function (day, hour) {
          slot.addEventListener('click', function () { openEvent(null, { day: day, hour: hour }); });
          calDropTarget(slot, function (ev) { moveEventToHour(ev, day, hour); });
        })(k, hh);
        col.appendChild(slot);
      }
      (map[k] || []).filter(function (ev) { return !ev.all_day; }).forEach(function (ev) {
        var s = parseUtc(ev.start), e = parseUtc(ev.end) || s;
        var ws = wallOf(s, zone);
        var startMin = wallKey(s) === k ? ws.hh * 60 + ws.mm : 0;
        var minutes = Math.max(15, Math.round((e.getTime() - s.getTime()) / 60000) - (wallKey(s) === k ? 0 : startMin));
        if (wallKey(e) !== k && e > s) { minutes = Math.min(minutes, 24 * 60 - startMin); }
        var block = chipFor(ev, true);
        block.className = block.className.replace('cal-chip', 'cal-ev');
        block.style.top = (startMin / 60 * CAL_HOUR) + 'px';
        block.style.height = Math.max(22, minutes / 60 * CAL_HOUR) + 'px';
        if (ev.writable) {
          var handle = node('span', undefined, 'cal-ev-resize');
          handle.setAttribute('title', t('Drag to change the end'));
          (function (occurrence, element) {
            handle.addEventListener('mousedown', function (event) {
              event.preventDefault();
              event.stopPropagation();
              calResize = { ev: occurrence, startY: event.clientY, block: element, height: parseFloat(element.style.height) || 22, delta: 0 };
            });
          })(ev, block);
          block.appendChild(handle);
        }
        col.appendChild(block);
      });
      body.appendChild(col);
    });
    wrap.appendChild(body);
    grid.appendChild(wrap);
  };
  var renderAgenda = function (grid) {
    var map = byDay();
    var keys = Object.keys(map).sort();
    var range = calWindow();
    var from = wallKey(range.start);
    var list = node('div', undefined, 'cal-agenda');
    var any = false;
    keys.forEach(function (k) {
      if (k < from) { return; }
      any = true;
      list.appendChild(node('h3', dayName(k, 'long')));
      map[k].forEach(function (ev) {
        var row = node('div', undefined, 'cal-agenda-row');
        var s = parseUtc(ev.start);
        row.appendChild(node('span', ev.all_day ? t('all day') : timeText(s), 'tm'));
        var dot = node('span', undefined, 'cal-dot');
        dot.style.background = calendarColor(ev.calendar);
        row.appendChild(dot);
        var open = node('button', ev.summary || t('(no title)'), 'textbtn');
        open.type = 'button';
        open.addEventListener('click', function () { openEvent(ev, null); });
        row.appendChild(open);
        if (ev.location) { row.appendChild(node('span', ev.location, 'muted')); }
        list.appendChild(row);
      });
    });
    if (!any) { list.appendChild(node('p', t('Nothing in the next month.'), 'hint')); }
    grid.appendChild(list);
  };

  // ---- moving and resizing
  var calResize = null;
  document.addEventListener('mousemove', function (event) {
    if (!calResize) { return; }
    calResize.delta = event.clientY - calResize.startY;
    calResize.block.style.height = Math.max(22, calResize.height + calResize.delta) + 'px';
  });
  document.addEventListener('mouseup', function (event) {
    if (!calResize) { return; }
    var r = calResize;
    calResize = null;
    var delta = (event.clientY !== undefined ? event.clientY - r.startY : r.delta);
    var minutes = Math.round(delta / CAL_HOUR * 60 / 15) * 15;
    if (!minutes) { return; }
    var s = parseUtc(r.ev.start), e = parseUtc(r.ev.end) || s;
    var newEnd = new Date(Math.max(s.getTime() + 15 * 60000, e.getTime() + minutes * 60000));
    writeMove(r.ev, s, newEnd);
  });
  var scopeOf = function (ev) {
    // A recurring occurrence: this one, or the series. Cancel in the question means the series.
    if (!ev.recurring) { return ''; }
    return window.confirm(t('Change only this occurrence? Cancel changes the whole series.')) ? ev.recurrence_id : '';
  };
  var writeMove = function (ev, start, end) {
    var body = { start: wallInput(start), end: wallInput(end), tz: calZone() };
    if (ev.all_day) { body.start = wallKey(start); body.end = wallKey(end); body.all_day = true; }
    var scope = scopeOf(ev);
    if (scope) { body.instance = scope; }
    return call('PUT', '/api/v1/me/calendar/events/' + ev.id, body).then(function (result) {
      if (result.status !== 200) { say('cal-status', describe(result, t('Could not move the event')), false); return; }
      toast(t('Event moved.'));
      loadEvents();
    });
  };
  var moveEventToDay = function (ev, key) {
    var s = parseUtc(ev.start), e = parseUtc(ev.end) || s;
    var duration = Math.max(0, e.getTime() - s.getTime());
    if (ev.all_day) {
      var days = Math.max(1, Math.round(duration / 86400000));
      var p = key.split('-');
      var ns = instantOf(+p[0], +p[1], +p[2], 0, 0, 'UTC');
      writeMove(ev, ns, new Date(ns.getTime() + days * 86400000));
      return;
    }
    var w = wallOf(s, calZone());
    var q = key.split('-');
    var newStart = instantOf(+q[0], +q[1], +q[2], w.hh, w.mm, calZone());
    writeMove(ev, newStart, new Date(newStart.getTime() + duration));
  };
  var moveEventToHour = function (ev, key, hour) {
    var s = parseUtc(ev.start), e = parseUtc(ev.end) || s;
    var duration = Math.max(15 * 60000, e.getTime() - s.getTime());
    var w = wallOf(s, calZone());
    var q = key.split('-');
    var newStart = instantOf(+q[0], +q[1], +q[2], hour, ev.all_day ? 0 : w.mm, calZone());
    var moved = Object.assign({}, ev, { all_day: false });
    writeMove(moved, newStart, new Date(newStart.getTime() + (ev.all_day ? 3600000 : duration)));
  };

  // ---- the editor
  var rruleParts = function (text) {
    var out = {};
    String(text || '').split(';').forEach(function (part) { var eq = part.indexOf('='); if (eq > 0) { out[part.slice(0, eq).toUpperCase()] = part.slice(eq + 1); } });
    return out;
  };
  var DAY_CODES = ['MO', 'TU', 'WE', 'TH', 'FR', 'SA', 'SU'];
  var openEvent = function (ev, defaults) {
    calState.editing = ev;
    var zone = calZone();
    el('event-title-h').textContent = ev ? t('Event') : t('New event');
    el('event-summary').value = ev ? ev.summary || '' : '';
    el('event-location').value = ev ? ev.location || '' : '';
    el('event-description').value = ev ? ev.description || '' : '';
    var allDay = ev ? !!ev.all_day : !!(defaults && defaults.allDay);
    el('event-allday').checked = allDay;
    var start, end;
    if (ev) {
      start = parseUtc(ev.start);
      end = parseUtc(ev.end) || start;
    } else {
      var day = (defaults && defaults.day) || wallKey(new Date());
      var p = day.split('-');
      var hour = defaults && defaults.hour !== undefined ? defaults.hour : 9;
      start = instantOf(+p[0], +p[1], +p[2], hour, 0, zone);
      end = new Date(start.getTime() + 3600000);
    }
    if (allDay) {
      el('event-start-date').value = ev ? ev.local_start : wallKey(start);
      el('event-end-date').value = ev ? keyOffset(ev.local_end || ev.local_start, -1) : wallKey(start);
    } else {
      el('event-start').value = wallInput(start);
      el('event-end').value = wallInput(end);
    }
    fillZoneSelect(el('event-tz'));
    el('event-tz').value = zone;
    var choice = el('event-calendar');
    if (ev) { choice.value = String(ev.calendar); } else if (defaults && defaults.calendar) { choice.value = String(defaults.calendar); } else if (choice.children.length) { choice.value = choice.children[0].value; }
    var rule = rruleParts(ev ? ev.rrule : '');
    el('event-repeat').value = (rule.FREQ || '').toLowerCase() || 'none';
    el('event-interval').value = rule.INTERVAL || '1';
    var days = (rule.BYDAY || '').split(',');
    var boxes = el('event-days').children;
    for (var i = 0; i < boxes.length; i++) { var cb = boxes[i].children[0]; cb.checked = days.indexOf(cb.value) >= 0; }
    el('event-ends').value = rule.UNTIL ? 'on' : rule.COUNT ? 'after' : 'never';
    el('event-until').value = rule.UNTIL ? rule.UNTIL.slice(0, 4) + '-' + rule.UNTIL.slice(4, 6) + '-' + rule.UNTIL.slice(6, 8) : '';
    el('event-count').value = rule.COUNT || '10';
    el('event-alarm').value = ev && ev.alarm_minutes >= 0 ? String(ev.alarm_minutes) : '-1';
    el('event-attendees').value = ev ? (ev.attendees || []).map(function (a) { return a.name ? a.name + ' <' + a.address + '>' : a.address; }).join(', ') : '';
    el('event-invite').checked = true;
    var recurringOccurrence = !!(ev && ev.recurring);
    el('event-scope-row').hidden = !recurringOccurrence;
    el('event-scope').value = 'occurrence';
    el('event-delete').hidden = !ev;
    var readOnly = !!(ev && !ev.writable);
    el('event-save').hidden = readOnly;
    el('event-delete').hidden = !ev || readOnly;
    el('event-readonly').hidden = !readOnly;
    var answered = ev && ev.my_partstat ? ev.my_partstat : '';
    el('event-partstat').hidden = !answered;
    el('event-partstat').textContent = answered ? partstatWords(answered) : '';
    el('event-attendee-list').hidden = !(ev && ev.attendees && ev.attendees.length);
    clear(el('event-attendee-list'));
    (ev && ev.attendees || []).forEach(function (a) { el('event-attendee-list').appendChild(node('span', (a.name || a.address) + ' · ' + partstatWords(a.partstat), 'badge ' + (a.partstat === 'ACCEPTED' ? 'good' : a.partstat === 'DECLINED' ? 'bad' : ''))); });
    say('event-status', '', true);
    updateEventForm();
    el('event-overlay').hidden = false;
    el('event-summary').focus();
  };
  var partstatWords = function (partstat) {
    if (partstat === 'ACCEPTED') { return t('accepted'); }
    if (partstat === 'DECLINED') { return t('declined'); }
    if (partstat === 'TENTATIVE') { return t('tentative'); }
    return t('not answered');
  };
  var updateEventForm = function () {
    var allDay = el('event-allday').checked;
    el('event-timed').hidden = allDay;
    el('event-dated').hidden = !allDay;
    var repeat = el('event-repeat').value;
    var scope = !el('event-scope-row').hidden && el('event-scope').value === 'occurrence';
    el('event-repeat-row').hidden = scope;
    el('event-repeat-more').hidden = repeat === 'none' || scope;
    el('event-days').hidden = repeat !== 'weekly';
    el('event-until').hidden = el('event-ends').value !== 'on';
    el('event-count').hidden = el('event-ends').value !== 'after';
  };
  ['event-allday', 'event-repeat', 'event-ends', 'event-scope'].forEach(function (id) { el(id).addEventListener('change', updateEventForm); });
  var closeEvent = function () { el('event-overlay').hidden = true; calState.editing = null; };
  el('event-cancel').addEventListener('click', closeEvent);
  el('event-overlay').addEventListener('click', function (event) { if (event.target === el('event-overlay')) { closeEvent(); } });
  var eventBody = function () {
    var body = { summary: el('event-summary').value.trim(), location: el('event-location').value.trim(), description: el('event-description').value, tz: el('event-tz').value || calZone() };
    body.calendar = Number(el('event-calendar').value) || 0;
    body.all_day = el('event-allday').checked;
    if (body.all_day) {
      body.start = el('event-start-date').value;
      body.end = el('event-end-date').value ? keyOffset(el('event-end-date').value, 1) : '';
    } else {
      body.start = el('event-start').value;
      body.end = el('event-end').value;
    }
    var scope = !el('event-scope-row').hidden && el('event-scope').value === 'occurrence';
    if (scope && calState.editing) { body.instance = calState.editing.recurrence_id; }
    else {
      var repeat = el('event-repeat').value;
      if (repeat === 'none') { body.rrule = ''; }
      else {
        var parts = ['FREQ=' + repeat.toUpperCase()];
        var interval = Math.max(1, Number(el('event-interval').value) || 1);
        if (interval > 1) { parts.push('INTERVAL=' + interval); }
        if (repeat === 'weekly') {
          var chosen = [];
          var boxes = el('event-days').children;
          for (var i = 0; i < boxes.length; i++) { if (boxes[i].children[0].checked) { chosen.push(boxes[i].children[0].value); } }
          if (chosen.length) { parts.push('BYDAY=' + chosen.join(',')); }
        }
        if (el('event-ends').value === 'on' && el('event-until').value) { parts.push('UNTIL=' + el('event-until').value.replace(/-/g, '') + (body.all_day ? '' : 'T235959Z')); }
        else if (el('event-ends').value === 'after') { parts.push('COUNT=' + Math.max(1, Number(el('event-count').value) || 1)); }
        body.rrule = parts.join(';');
      }
    }
    body.alarm_minutes = Number(el('event-alarm').value);
    var attendees = el('event-attendees').value.split(/[,;]/).map(function (s) { return s.trim(); }).filter(function (s) { return s; });
    body.attendees = attendees;
    body.invite = attendees.length > 0 && el('event-invite').checked;
    return body;
  };
  el('event-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var body = eventBody();
    if (!body.summary) { say('event-status', t('Give the event a title.'), false); return; }
    if (!body.start) { say('event-status', t('Give the event a start.'), false); return; }
    var editing = calState.editing;
    var request = editing ? call('PUT', '/api/v1/me/calendar/events/' + editing.id, body) : call('POST', '/api/v1/me/calendar/events', body);
    request.then(function (result) {
      if (result.status !== 200 && result.status !== 201) { say('event-status', describe(result, t('Could not save the event')), false); return; }
      closeEvent();
      toast(editing ? t('Event saved.') : (body.invite ? t('Event saved and invitations sent.') : t('Event added.')));
      loadEvents();
    });
  });
  el('event-delete').addEventListener('click', function () {
    var ev = calState.editing;
    if (!ev) { return; }
    var scope = !el('event-scope-row').hidden && el('event-scope').value === 'occurrence' ? ev.recurrence_id : '';
    if (!scope && !window.confirm(tf('Delete "{0}"?', ev.summary || t('(no title)')))) { return; }
    var notify = ev.attendees && ev.attendees.length && ev.organizer === me.toLowerCase();
    call('DELETE', '/api/v1/me/calendar/events/' + ev.id + (scope ? '?instance=' + encodeURIComponent(scope) : (notify ? '?notify=1' : ''))).then(function (result) {
      if (result.status !== 200) { say('event-status', describe(result, t('Could not delete the event')), false); return; }
      closeEvent();
      toast(scope ? t('Occurrence removed.') : t('Event deleted.'));
      loadEvents();
    });
  });
  var setCalView = function (view) {
    calState.view = view;
    try { localStorage.setItem(CAL_VIEW, view); } catch (e) { /* no storage */ }
    loadEvents();
  };
  el('cal-views').addEventListener('click', function (event) {
    var view = event.target && event.target.getAttribute ? event.target.getAttribute('data-cal-view') : '';
    if (view) { setCalView(view); }
  });
  var calStep = function (direction) {
    var zone = calZone();
    var w = wallOf(calState.anchor, zone);
    var next;
    if (calState.view === 'month') { next = dayOffset(w.y, w.m + direction, 1, 0); }
    else if (calState.view === 'week') { next = dayOffset(w.y, w.m, w.d, 7 * direction); }
    else { next = dayOffset(w.y, w.m, w.d, direction); }
    calState.anchor = instantOf(next.y, next.m, next.d, 12, 0, zone);
    loadEvents();
  };
  el('cal-prev').addEventListener('click', function () { calStep(-1); });
  el('cal-next').addEventListener('click', function () { calStep(1); });
  el('cal-today').addEventListener('click', function () { calState.anchor = new Date(); loadEvents(); });
  el('cal-new').addEventListener('click', function () { openEvent(null, { day: wallKey(calState.anchor) }); });

  // ---- reminders while the page is open
  var REMIND_SHOWN = 'hmPortalRemindShown';
  var remindState = { events: [], fetched: 0 };
  // Which reminders were shown: in this browser for the first account, and
  // in the page alone for the second, as the follow-up marks are.
  var remindShown = function () {
    if (!offlineMine()) { return accounts.second.marks.remind.slice(); }
    try { return JSON.parse(localStorage.getItem(REMIND_SHOWN) || '[]') || []; } catch (e) { return []; }
  };
  var remindShownWrite = function (list) {
    if (!offlineMine()) { accounts.second.marks.remind = list; return; }
    try { localStorage.setItem(REMIND_SHOWN, JSON.stringify(list)); } catch (e) { /* no storage */ }
  };
  var remindTick = function () {
    if (el('account').hidden) { return; }
    var now = Date.now();
    var refresh = function () {
      if (now - remindState.fetched < 10 * 60000) { return Promise.resolve(); }
      remindState.fetched = now;
      var start = new Date(now - 3600000), end = new Date(now + 36 * 3600000);
      return call('GET', '/api/v1/me/calendar/events?start=' + utcText(start) + '&end=' + utcText(end)).then(function (result) {
        if (result.status === 200 && result.data) { remindState.events = result.data.events || []; }
      });
    };
    refresh().then(function () {
      var shown = remindShown();
      var changed = false;
      remindState.events.forEach(function (ev) {
        if (typeof ev.alarm_minutes !== 'number' || ev.alarm_minutes < 0 || ev.status === 'CANCELLED') { return; }
        var s = parseUtc(ev.start);
        if (!s) { return; }
        var fire = s.getTime() - ev.alarm_minutes * 60000;
        var key = ev.id + '|' + (ev.recurrence_id || ev.start);
        if (fire > now || now - fire > 3600000 || shown.indexOf(key) >= 0) { return; }
        shown.push(key);
        changed = true;
        var words = tf('Reminder: {0} at {1}', ev.summary || t('(no title)'), ev.all_day ? dayName(ev.local_start) : timeText(s));
        toast(words, null, true);
        if (pref('notify') === '1' && 'Notification' in window && Notification.permission === 'granted') {
          try { new Notification(words, { body: spanText(ev), tag: 'hm-event-' + key }); } catch (e) { /* a browser without notifications is still a browser */ }
        }
      });
      if (changed) { remindShownWrite(shown.slice(-200)); }
    });
  };
  setInterval(remindTick, 60000);

  // ---- Tasks -----------------------------------------------------------------
  var taskState = { tasks: [], editing: null };
  var TASKS_DONE = 'hmPortalTasksDone';
  var loadTasks = function () {
    el('task-show-done').checked = (function () { try { return localStorage.getItem(TASKS_DONE) === '1'; } catch (e) { return false; } })();
    return loadCalendars().then(function () {
      return call('GET', '/api/v1/me/tasks').then(function (result) {
        if (result.status !== 200 || !result.data) { say('task-status', describe(result, t('Could not read the tasks')), false); return; }
        taskState.tasks = result.data.tasks || [];
        renderTasks();
      });
    });
  };
  var dueWords = function (task) {
    if (!task.due) { return ''; }
    if (task.all_day) { return dayName(task.local_due, 'long'); }
    var d = parseUtc(task.due);
    return d ? dayName(wallKey(d), 'long') + ' ' + timeText(d) : task.local_due;
  };
  var overdue = function (task) {
    if (!task.due || task.completed) { return false; }
    var d = parseUtc(task.due);
    if (!d) { return false; }
    if (task.all_day) { return task.local_due < wallKey(new Date()); }
    return d.getTime() < Date.now();
  };
  var renderTasks = function () {
    var list = el('task-list');
    clear(list);
    var showDone = el('task-show-done').checked;
    var shown = taskState.tasks.filter(function (task) { return showDone || !task.completed; });
    el('task-empty').hidden = shown.length > 0;
    shown.forEach(function (task) {
      var row = node('div', undefined, 'task-row' + (task.completed ? ' done' : '') + (overdue(task) ? ' overdue' : ''));
      var box = document.createElement('input');
      box.type = 'checkbox';
      box.checked = !!task.completed;
      box.disabled = !task.writable;
      box.setAttribute('aria-label', task.summary);
      box.addEventListener('change', function () {
        call('PUT', '/api/v1/me/tasks/' + task.id, { completed: box.checked }).then(function (result) {
          if (result.status !== 200) { say('task-status', describe(result, t('Could not change the task')), false); box.checked = !box.checked; return; }
          toast(box.checked ? t('Task done.') : t('Task reopened.'));
          loadTasks();
        });
      });
      row.appendChild(box);
      var words = node('div', undefined, 'task-words');
      var title = node('button', task.summary || t('(no title)'), 'task-title');
      title.type = 'button';
      title.addEventListener('click', function () { openTask(task); });
      words.appendChild(title);
      var meta = node('div', undefined, 'task-meta');
      if (task.priority && task.priority <= 4) { meta.appendChild(node('span', t('High priority'), 'badge warn')); }
      if (task.due) { meta.appendChild(node('span', dueWords(task), overdue(task) ? 'due late' : 'due')); }
      var c = calendarById(task.calendar);
      if (c && calState.calendars.length > 1) { var chip = node('span', c.display_name, 'badge'); chip.style.background = c.color || ''; chip.style.color = c.color ? '#fff' : ''; meta.appendChild(chip); }
      if (task.description) { meta.appendChild(node('span', task.description.split('\n')[0], 'muted')); }
      words.appendChild(meta);
      row.appendChild(words);
      list.appendChild(row);
    });
  };
  el('task-show-done').addEventListener('change', function () {
    try { localStorage.setItem(TASKS_DONE, el('task-show-done').checked ? '1' : '0'); } catch (e) { /* no storage */ }
    renderTasks();
  });
  el('task-add-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var summary = el('task-add-summary').value.trim();
    if (!summary) { return; }
    var body = { summary: summary, calendar: Number(el('task-add-list').value) || 0 };
    if (el('task-add-due').value) { body.due = el('task-add-due').value; }
    call('POST', '/api/v1/me/tasks', body).then(function (result) {
      if (result.status !== 201) { say('task-status', describe(result, t('Could not add the task')), false); return; }
      el('task-add-summary').value = '';
      el('task-add-due').value = '';
      say('task-status', '', true);
      loadTasks();
    });
  });
  var openTask = function (task) {
    taskState.editing = task;
    el('task-summary').value = task.summary || '';
    el('task-notes').value = task.description || '';
    var d = task.due ? parseUtc(task.due) : null;
    el('task-due').value = task.due ? (task.all_day ? task.local_due : wallKey(d)) : '';
    el('task-due-time').value = task.due && !task.all_day && d ? pad2(wallOf(d, calZone()).hh) + ':' + pad2(wallOf(d, calZone()).mm) : '';
    el('task-priority').value = task.priority ? (task.priority <= 4 ? '1' : task.priority <= 5 ? '5' : '9') : '0';
    el('task-list-select').value = String(task.calendar);
    el('task-completed').checked = !!task.completed;
    el('task-save').hidden = !task.writable;
    el('task-delete').hidden = !task.writable;
    say('task-edit-status', '', true);
    el('task-overlay').hidden = false;
    el('task-summary').focus();
  };
  var closeTask = function () { el('task-overlay').hidden = true; taskState.editing = null; };
  el('task-cancel').addEventListener('click', closeTask);
  el('task-overlay').addEventListener('click', function (event) { if (event.target === el('task-overlay')) { closeTask(); } });
  el('task-form').addEventListener('submit', function (event) {
    event.preventDefault();
    var task = taskState.editing;
    if (!task) { return; }
    var body = { summary: el('task-summary').value.trim(), description: el('task-notes').value, priority: Number(el('task-priority').value) || 0, completed: el('task-completed').checked, calendar: Number(el('task-list-select').value) || task.calendar };
    if (!body.summary) { say('task-edit-status', t('Give the task a title.'), false); return; }
    var due = el('task-due').value;
    if (due && el('task-due-time').value) { body.due = due + 'T' + el('task-due-time').value; body.tz = calZone(); body.all_day = false; }
    else if (due) { body.due = due; body.all_day = true; }
    else { body.due = ''; }
    call('PUT', '/api/v1/me/tasks/' + task.id, body).then(function (result) {
      if (result.status !== 200) { say('task-edit-status', describe(result, t('Could not save the task')), false); return; }
      closeTask();
      toast(t('Task saved.'));
      loadTasks();
    });
  });
  el('task-delete').addEventListener('click', function () {
    var task = taskState.editing;
    if (!task || !window.confirm(tf('Delete "{0}"?', task.summary || t('(no title)')))) { return; }
    call('DELETE', '/api/v1/me/tasks/' + task.id).then(function (result) {
      if (result.status !== 200) { say('task-edit-status', describe(result, t('Could not delete the task')), false); return; }
      closeTask();
      toast(t('Task deleted.'));
      loadTasks();
    });
  });

  // ---- The invitation a message carries ------------------------------------
  var renderInvitation = function (m) {
    var box = el('message-invitation');
    clear(box);
    box.hidden = !m.invitation;
    if (!m.invitation) { return; }
    box.appendChild(node('p', t('Reading the invitation...'), 'hint'));
    call('GET', '/api/v1/me/messages/' + m.id + '/invitation').then(function (result) {
      if (!current || current.id !== m.id) { return; }
      clear(box);
      if (result.status !== 200 || !result.data) { box.appendChild(node('p', describe(result, t('The invitation could not be read')), 'hint')); return; }
      var inv = result.data;
      var method = inv.method || 'REQUEST';
      var heading = method === 'CANCEL' ? t('Cancelled event') : method === 'REPLY' ? t('Reply to your invitation') : method === 'COUNTER' ? t('A new time is proposed') : t('Invitation');
      box.appendChild(node('h3', heading));
      box.appendChild(node('div', inv.summary || t('(no title)'), 'inv-title'));
      var when = node('div', undefined, 'inv-line');
      when.appendChild(node('span', t('When'), 'k'));
      when.appendChild(node('span', spanText({ start: inv.start, end: inv.end, all_day: inv.all_day, local_start: inv.local_start, local_end: inv.local_end }) + (inv.recurring ? ' · ' + t('repeats') : '')));
      box.appendChild(when);
      if (inv.location) { var where = node('div', undefined, 'inv-line'); where.appendChild(node('span', t('Where'), 'k')); where.appendChild(node('span', inv.location)); box.appendChild(where); }
      if (inv.organizer) { var who = node('div', undefined, 'inv-line'); who.appendChild(node('span', t('Organiser'), 'k')); who.appendChild(node('span', inv.organizer_name ? inv.organizer_name + ' <' + inv.organizer + '>' : inv.organizer)); box.appendChild(who); }
      if (inv.attendees && inv.attendees.length) {
        var guests = node('div', undefined, 'inv-line');
        guests.appendChild(node('span', t('Guests'), 'k'));
        var badges = node('span', undefined, 'chips');
        inv.attendees.forEach(function (a) { badges.appendChild(node('span', (a.name || a.address) + ' · ' + partstatWords(a.partstat), 'badge ' + (a.partstat === 'ACCEPTED' ? 'good' : a.partstat === 'DECLINED' ? 'bad' : ''))); });
        guests.appendChild(badges);
        box.appendChild(guests);
      }
      var status = node('div', undefined, 'inv-status');
      var actions = node('div', undefined, 'row');
      var reply = function (action, extra) {
        var body = Object.assign({ action: action }, extra || {});
        return call('POST', '/api/v1/me/messages/' + m.id + '/invitation/reply', body).then(function (result) {
          if (result.status !== 200) { status.textContent = describe(result, t('Could not answer the invitation')); status.className = 'inv-status error'; return; }
          toast(action === 'remove' ? t('Removed from your calendar.') : action === 'counter' ? t('Your proposal was sent to the organiser.') : t('Your answer was sent and the event is in your calendar.'));
          calState.loaded = false;
          renderInvitation(m);
        });
      };
      if (method === 'REPLY') {
        var who2 = inv.attendees && inv.attendees[0];
        status.textContent = who2 ? tf('{0} has {1}.', who2.name || who2.address, partstatWords(who2.partstat)) + (inv.i_am_organizer && inv.in_calendar ? ' ' + t('Your event has been updated.') : '') : '';
      } else if (method === 'CANCEL') {
        status.textContent = inv.in_calendar ? t('The organiser cancelled this event. It is marked cancelled in your calendar.') : t('The organiser cancelled this event.');
        if (inv.in_calendar) { var remove = button(t('Remove from calendar'), 'btn ghost sm'); remove.addEventListener('click', function () { reply('remove'); }); actions.appendChild(remove); }
      } else if (method === 'COUNTER') {
        status.textContent = t('A guest proposes this time instead. Change the event in your calendar to accept it.');
      } else if (inv.i_am_organizer) {
        status.textContent = t('You organise this event.');
      } else {
        var answered = inv.stored_partstat || '';
        status.textContent = answered ? (inv.stored_status === 'CANCELLED' ? t('This event was cancelled.') : tf('You {0}.', partstatWords(answered)) + ' ' + t('It is in your calendar.')) : t('Not answered yet.');
        [['accept', t('Accept'), 'btn sm'], ['tentative', t('Maybe'), 'btn ghost sm'], ['decline', t('Decline'), 'btn ghost sm']].forEach(function (pair) {
          var b = button(pair[1], pair[2]);
          b.setAttribute('data-invite', pair[0]);
          b.addEventListener('click', function () { reply(pair[0]); });
          actions.appendChild(b);
        });
        var propose = button(t('Propose a new time'), 'btn quiet sm');
        var proposeBox = node('div', undefined, 'row');
        proposeBox.hidden = true;
        var ps = document.createElement('input'); ps.type = 'datetime-local'; ps.setAttribute('aria-label', t('Proposed start')); ps.value = inv.start ? wallInput(parseUtc(inv.start)) : ''; ps.id = 'invite-propose-start';
        var pe = document.createElement('input'); pe.type = 'datetime-local'; pe.setAttribute('aria-label', t('Proposed end')); pe.value = inv.end ? wallInput(parseUtc(inv.end)) : ''; pe.id = 'invite-propose-end';
        var send = button(t('Send proposal'), 'btn sm');
        send.addEventListener('click', function () { reply('counter', { start: ps.value, end: pe.value, tz: calZone() }); });
        proposeBox.appendChild(ps); proposeBox.appendChild(pe); proposeBox.appendChild(send);
        propose.addEventListener('click', function () { proposeBox.hidden = !proposeBox.hidden; });
        actions.appendChild(propose);
        box.appendChild(status);
        box.appendChild(actions);
        box.appendChild(proposeBox);
        return;
      }
      box.appendChild(status);
      if (actions.children.length) { box.appendChild(actions); }
    });
  };

  // The theme the last visit chose, for the sign-in page; the account's own
  // choice replaces it once there is an account.
  applyTheme(storedTheme());
  // Before parseHash: the fragment the identity provider sent the browser back
  // with is an answer to a sign-in, not a route, and it is taken out of the
  // address here so that a reload cannot try to spend a ticket twice.
  // In a try: nothing in an address somebody was sent may stop the page being
  // built. A fragment this does not understand is not a sign-in, and the page
  // then loads as it loads on any other visit.
  var redeeming = false;
  try { redeeming = singleSignOnReturn(); } catch (e) { redeeming = false; }
  pending = parseHash().hash;
  loadSignInMethods();
  if (!redeeming) { load(true); }
})();
