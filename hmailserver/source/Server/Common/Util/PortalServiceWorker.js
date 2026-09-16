// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

'use strict';
var CACHE = 'hm-portal-shell-v1';
var SHELL = ['/portal', '/portal.js', '/portal-smime.js', '/portal.webmanifest'];
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
  if (e.request.method !== 'GET' || SHELL.indexOf(url.pathname) < 0) { return; }
  e.respondWith(fetch(e.request).then(function (r) {
    if (r.ok) { var copy = r.clone(); caches.open(CACHE).then(function (c) { c.put(e.request, copy); }); }
    return r;
  }).catch(function () { return caches.match(e.request); }));
});
