// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

'use strict';
var CACHE = 'hm-portal-shell-v1';
var SHELL = ['/portal', '/portal.js', '/portal-smime.js', '/portal-markdown.js', '/portal.webmanifest'];
self.addEventListener('install', function (e) {
  e.waitUntil(caches.open(CACHE).then(function (c) { return c.addAll(SHELL); }).then(function () { return self.skipWaiting(); }));
});
self.addEventListener('activate', function (e) {
  e.waitUntil(caches.keys().then(function (keys) {
    return Promise.all(keys.filter(function (k) { return k !== CACHE; }).map(function (k) { return caches.delete(k); }));
  }).then(function () { return self.clients.claim(); }));
});
// ---- Web Push ---------------------------------------------------------------
//
// The body is an encrypted JSON document the server built for THIS subscription
// and nothing more: the person's own choice decides whether it holds a count, a
// sender or a subject, and this worker shows what it was given rather than
// fetching anything. A push with no readable payload still shows a notification,
// because a browser that has granted permission expects one for every push and
// will otherwise show its own - and because "there is new mail" is true.
//
// The words come from a cache the page fills in the reader's language
// (pushWords in portal.js); English is the fallback, since a worker has no
// localStorage and cannot ask a page that is not open.
var PUSH_WORDS = 'hm-portal-push';
function pushWords() {
  return caches.open(PUSH_WORDS).then(function (c) { return c.match('/portal/push-words'); }).then(function (r) {
    return r ? r.json() : null;
  }).catch(function () { return null; }).then(function (words) {
    return words || { mail: 'New mail', messages: '{0} new messages', test: 'A test notification' };
  });
}
function pushBody(data, words) {
  if (data && data.test) { return words.test; }
  if (data && data.subject) { return data.subject; }
  var count = data && data.count > 1 ? data.count : 0;
  return count ? String(words.messages).replace('{0}', String(count)) : words.mail;
}
function pushTitle(data, words) {
  if (data && (data.name || data.from)) { return data.name || data.from; }
  return words.mail;
}
self.addEventListener('push', function (e) {
  var data = null;
  try { data = e.data ? e.data.json() : null; } catch (err) { data = null; }
  e.waitUntil(pushWords().then(function (words) {
    // One tag, so a burst of mail replaces itself rather than stacking up a
    // screen of notifications; renotify so the later one is still announced.
    return self.registration.showNotification(pushTitle(data, words), {
      body: pushBody(data, words),
      tag: 'hm-push',
      renotify: true,
      data: { url: '/portal' }
    });
  }));
});
self.addEventListener('notificationclick', function (e) {
  e.notification.close();
  var url = (e.notification.data && e.notification.data.url) || '/portal';
  e.waitUntil(self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then(function (windows) {
    for (var i = 0; i < windows.length; i++) {
      if (windows[i].url.indexOf('/portal') >= 0 && windows[i].focus) { return windows[i].focus(); }
    }
    return self.clients.openWindow ? self.clients.openWindow(url) : null;
  }));
});
// A push service may replace a subscription on its own, and the page cannot be
// asked to re-post it while it is closed. Nothing can be done from here without
// the account's session, so this is left for the page: the row the server still
// holds stops working, is dead-lettered after the configured attempts, and is
// shown with its reason in Settings - which is where a person is told to turn
// notifications on again.
self.addEventListener('pushsubscriptionchange', function () { /* the page recovers this */ });

// ---- Offline ------------------------------------------------------------------
//
// The page keeps the bodies and attachments of the last N days in IndexedDB
// (hmPortalOffline, the parts store: '<id>/html' is the HTML document the server
// rendered for the message, '<id>/<n>' its n-th attachment). A message's HTML is
// shown in a frame whose source is the server's route, and an attachment is a
// link to its route; when the network fails, this answers both out of that
// store, under the headers the server sends them with, so a kept message reads
// offline exactly as it read online. The network is always tried first: the
// store is a fallback, never a cache that could show a stale part.
var OFFLINE_DB = 'hmPortalOffline';
var OFFLINE_PART = /^\/api\/v1\/me\/messages\/(\d+)\/(html|attachments\/(\d+))$/;
var OFFLINE_HTML_POLICY = "default-src 'none'; style-src 'unsafe-inline'; img-src data:; sandbox allow-popups allow-popups-to-escape-sandbox; form-action 'none'; base-uri 'none'; frame-ancestors 'self'";
function offlineDb() {
  return new Promise(function (resolve) {
    if (typeof indexedDB === 'undefined' || !indexedDB || !indexedDB.open) { resolve(null); return; }
    var request;
    try { request = indexedDB.open(OFFLINE_DB, 1); } catch (e) { resolve(null); return; }
    // The same four stores the page makes, so whichever opens first leaves
    // the other nothing to upgrade.
    request.onupgradeneeded = function () {
      var db = request.result;
      ['rows', 'messages', 'parts', 'meta'].forEach(function (name) {
        if (!db.objectStoreNames.contains(name)) { db.createObjectStore(name, { keyPath: name === 'parts' || name === 'meta' ? 'key' : 'id' }); }
      });
    };
    request.onsuccess = function () { resolve(request.result); };
    request.onerror = function () { resolve(null); };
    request.onblocked = function () { resolve(null); };
  });
}
function offlinePart(key) {
  return offlineDb().then(function (db) {
    if (!db) { return null; }
    return new Promise(function (resolve) {
      var request;
      try { request = db.transaction('parts', 'readonly').objectStore('parts').get(key); } catch (e) { resolve(null); return; }
      request.onsuccess = function () { resolve(request.result || null); };
      request.onerror = function () { resolve(null); };
    }).then(function (part) { try { db.close(); } catch (e) { /* closed already */ } return part; });
  });
}
function asciiName(name) {
  var out = '';
  var text = String(name || 'attachment');
  for (var i = 0; i < text.length; i++) {
    var c = text.charCodeAt(i);
    out += c >= 32 && c < 127 && text.charAt(i) !== '"' && text.charAt(i) !== '\\' ? text.charAt(i) : '_';
  }
  return out || 'attachment';
}
function offlineAnswer(match) {
  var isHtml = match[3] === undefined;
  var key = match[1] + '/' + (isHtml ? 'html' : match[3]);
  return offlinePart(key).then(function (part) {
    if (!part || !part.blob) { return new Response('', { status: 503, headers: { 'Cache-Control': 'no-store' } }); }
    var headers = {
      'Content-Type': isHtml ? 'text/html; charset=utf-8' : (part.type || 'application/octet-stream'),
      'X-Content-Type-Options': 'nosniff',
      'Referrer-Policy': 'no-referrer',
      'Cache-Control': 'no-store'
    };
    if (isHtml) {
      headers['Content-Security-Policy'] = OFFLINE_HTML_POLICY;
    } else {
      headers['Content-Security-Policy'] = 'sandbox';
      headers['Content-Disposition'] = 'attachment; filename="' + asciiName(part.name) + '"; filename*=UTF-8\'\'' + encodeURIComponent(String(part.name || 'attachment'));
    }
    return new Response(part.blob, { status: 200, headers: headers });
  });
}

// A share from another app arrives as a POST to the share target. The
// title, text, link and files go into a cache of their own and the page is
// sent to a new message, which reads them once and deletes the cache.
var SHARE = 'hm-portal-share';
self.addEventListener('fetch', function (e) {
  var url = new URL(e.request.url);
  if (e.request.method === 'POST' && url.pathname === '/portal/share') {
    e.respondWith(e.request.formData().then(function (form) {
      var files = form.getAll('files').filter(function (f) { return f && typeof f.name === 'string'; });
      var meta = { title: form.get('title') || '', text: form.get('text') || '', url: form.get('url') || '', files: files.map(function (f) { return f.name; }) };
      return caches.open(SHARE).then(function (c) {
        var puts = [c.put('/portal/share-text', new Response(JSON.stringify(meta), { headers: { 'Content-Type': 'application/json' } }))];
        files.forEach(function (f, i) { puts.push(c.put('/portal/share-file/' + i, new Response(f, { headers: { 'Content-Type': f.type || 'application/octet-stream' } }))); });
        return Promise.all(puts);
      }).then(function () { return Response.redirect('/portal#/compose?share=1', 303); });
    }).catch(function () { return Response.redirect('/portal#/compose', 303); }));
    return;
  }
  var part = e.request.method === 'GET' ? OFFLINE_PART.exec(url.pathname) : null;
  if (part) {
    e.respondWith(fetch(e.request).catch(function () { return offlineAnswer(part); }));
    return;
  }
  if (e.request.method !== 'GET' || SHELL.indexOf(url.pathname) < 0) { return; }
  e.respondWith(fetch(e.request).then(function (r) {
    if (r.ok) { var copy = r.clone(); caches.open(CACHE).then(function (c) { c.put(e.request, copy); }); }
    return r;
  }).catch(function () { return caches.match(e.request); }));
});
