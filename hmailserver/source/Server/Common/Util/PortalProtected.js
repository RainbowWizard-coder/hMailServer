// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The reading page of a password-protected message (PortalProtected.html,
// served at /portal/protected/<token> with the webmail's own headers, this
// script at /portal-protected.js). No session, no account: the token in the
// address bar is the whole capability, and the password is the whole key.
//
// What happens here, in order: the ciphertext is fetched from
// /api/v1/protected/<token> - the hint, the dates and an armoured OpenPGP
// message - the password is asked for, the page's OpenPGP module opens the
// message with it, and the MIME message inside is shown: the text, and each
// attachment offered to save. A wrong password says so and nothing else,
// because a wrong password and a damaged message are not the reader's to
// tell apart. A reply is encrypted under the same password and posted to
// /api/v1/protected/<token>/reply; the sender is told.
//
// The password is held in this script's closure for as long as the page is
// open, so a reply can be sealed under it, and it goes into no request: the
// harness (build/portal-script-test.js) reads every request this page makes
// and fails if the password is in any of them.
(function () {
  'use strict';

  var el = function (id) { return document.getElementById(id); };
  var say = function (id, text, ok) {
    var box = el(id);
    box.textContent = text || '';
    box.className = 'status ' + (ok ? 'ok' : 'err');
  };
  var show = function (id, on) { el(id).hidden = !on; };
  var clear = function (node) { while (node.firstChild) { node.removeChild(node.firstChild); } };
  var node = function (tag, text) { var n = document.createElement(tag); if (text !== undefined) { n.textContent = text; } return n; };
  var when = function (seconds) { var d = new Date(Number(seconds) * 1000); return isNaN(d.getTime()) ? '' : d.toLocaleString(); };
  var format = function (n) {
    if (n < 1024) { return n + ' B'; }
    if (n < 1024 * 1024) { return Math.round(n / 1024) + ' KB'; }
    return (n / (1024 * 1024)).toFixed(1) + ' MB';
  };

  // The token: 22 characters of base64url at the end of the path, and nothing
  // else is asked about.
  var match = String(location.pathname || '').match(/\/portal\/protected\/([A-Za-z0-9_-]{22})$/);
  var token = match ? match[1] : '';

  var record = null;
  var password = '';

  var gone = function (text) {
    show('loading', false);
    show('unlock', false);
    show('message', false);
    el('gone-text').textContent = text;
    show('gone', true);
  };

  var pgp = function () { return window.hmOpenPgp || null; };
  var usable = function () { return !!(pgp() && pgp().available !== false && window.isSecureContext !== false && window.hmSmime); };

  var fetchJson = function (path, options) {
    return fetch(path, Object.assign({ cache: 'no-store', credentials: 'omit' }, options || {})).then(function (response) {
      return response.text().then(function (text) {
        var data = null;
        try { data = text ? JSON.parse(text) : null; } catch (e) { data = null; }
        return { status: response.status, data: data };
      });
    }, function () { return { status: 0, data: null }; });
  };

  var describe = function (result, fallback) {
    if (result.status === 0) { return 'No connection; try again in a moment.'; }
    return (result.data && result.data.error) ? result.data.error : fallback + ' (' + result.status + ')';
  };

  // ---- fetching the ciphertext ----
  var load = function () {
    if (!token) { gone('There is no password-protected message at this address.'); return; }
    if (!usable()) {
      gone(window.isSecureContext === false
        ? 'This page needs HTTPS: the browser withholds its cryptography from a page that was not opened over it, and the message can only be decrypted here.'
        : 'This page\'s cryptography did not load, so the message cannot be decrypted here. Reload the page, or try another browser.');
      return;
    }
    fetchJson('/api/v1/protected/' + token).then(function (result) {
      if (result.status === 200 && result.data && typeof result.data.message === 'string') {
        record = result.data;
        show('loading', false);
        if (record.hint) { el('hint').textContent = record.hint; show('hint-box', true); }
        el('title').textContent = record.reply ? 'A reply to your password-protected message' : 'A password-protected message';
        if (record.reply) { el('intro').textContent = 'This reply was written on the reading page of a message you sent, and encrypted there under the password you set. Only that password opens it.'; }
        el('until').textContent = 'It can be read here until ' + when(record.expires) + '.';
        show('unlock', true);
        el('password').focus();
        return;
      }
      if (result.status === 404) { gone('There is no password-protected message at this address, or it has been removed.'); return; }
      if (result.status === 410) { gone(describe(result, 'This message can no longer be read')); return; }
      if (result.status === 429) { gone(describe(result, 'Too many requests')); return; }
      if (result.status === 403) { gone(describe(result, 'This message cannot be read here')); return; }
      gone(describe(result, 'The message could not be fetched'));
    });
  };

  // ---- opening it ----
  var render = function (opened) {
    var meta = el('meta');
    clear(meta);
    var subject = opened.subject || '(no subject)';
    el('title').textContent = subject;
    if (opened.from) { meta.appendChild(node('div', 'From: ' + opened.from)); }
    if (opened.to) { meta.appendChild(node('div', 'To: ' + opened.to)); }
    if (opened.date) { meta.appendChild(node('div', 'Date: ' + opened.date)); }
    meta.appendChild(node('div', 'Decrypted in this browser with the password you typed; readable here until ' + when(record.expires) + '.'));

    var content = opened.content || {};
    var text = content.text || '';
    if (!text && content.html) {
      // A message written with only formatting: its text is what the markup
      // holds, with the blocks that end a line ending one. Nothing of the HTML
      // is rendered - this page shows text and runs nothing.
      text = String(content.html)
        .replace(/<(style|script)[\s\S]*?<\/\1>/gi, '')
        .replace(/<br\s*\/?>/gi, '\n').replace(/<\/(p|div|li|tr|h1|h2|h3|blockquote)>/gi, '\n')
        .replace(/<[^>]+>/g, '')
        .replace(/&nbsp;/g, ' ').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&#39;/g, '\'').replace(/&amp;/g, '&')
        .trim();
    }
    el('text').textContent = text || '(no text)';

    var list = el('attachments');
    clear(list);
    var attachments = (content.attachments || []).filter(function (a) { return a && a.bytes && !a.contentId; });
    attachments.forEach(function (a) {
      var li = node('li');
      li.appendChild(node('span', a.name + ' (' + format(a.bytes.length) + ') '));
      var save = node('button', 'Save');
      save.className = 'quiet';
      save.type = 'button';
      save.addEventListener('click', function () {
        // The bytes are in this page already; a link to them is how a browser
        // saves a file without anything leaving the page.
        var url = URL.createObjectURL(new Blob([a.bytes], { type: 'application/octet-stream' }));
        var link = document.createElement('a');
        link.href = url;
        link.setAttribute('download', a.name || 'attachment');
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
        setTimeout(function () { URL.revokeObjectURL(url); }, 10000);
      });
      li.appendChild(save);
      list.appendChild(li);
    });
    show('attachments-box', attachments.length > 0);
    show('reply-box', !!record.can_reply);
    show('unlock', false);
    show('message', true);
  };

  el('password-form').addEventListener('submit', function (event) {
    event.preventDefault();
    if (!record) { return; }
    var typed = el('password').value;
    if (!typed) { say('password-status', 'Type the password.', false); return; }
    el('open').disabled = true;
    say('password-status', 'Decrypting...', true);
    pgp().openProtected(record.message, typed).then(function (opened) {
      password = typed;
      el('password').value = '';
      say('password-status', '', true);
      el('open').disabled = false;
      render(opened);
    }, function () {
      // Whatever went wrong - a wrong password, a damaged message - the
      // reader is told one thing.
      el('open').disabled = false;
      say('password-status', 'That password is not right.', false);
      el('password').focus();
    });
  });

  // ---- replying ----
  el('reply-form').addEventListener('submit', function (event) {
    event.preventDefault();
    if (!record || !password) { return; }
    var text = el('reply-text').value;
    if (!text.trim()) { say('reply-status', 'Write something first.', false); return; }
    el('reply-send').disabled = true;
    say('reply-status', 'Encrypting and sending...', true);
    var subject = 'Re: ' + (el('title').textContent || '');
    pgp().protectedMessage({ headers: { subject: subject }, text: text, html: '', attachments: [], password: password }).then(function (armored) {
      return fetchJson('/api/v1/protected/' + token + '/reply', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ message: armored }) });
    }).then(function (result) {
      el('reply-send').disabled = false;
      if (result.status === 201) {
        el('reply-text').value = '';
        say('reply-status', 'Your reply is on its way to the sender, encrypted under the same password. It can be read until ' + when(result.data && result.data.expires) + '.', true);
        return;
      }
      say('reply-status', describe(result, 'The reply could not be sent'), false);
    }, function (e) {
      el('reply-send').disabled = false;
      say('reply-status', 'The reply could not be encrypted: ' + String(e && e.message ? e.message : e), false);
    });
  });

  load();
}());
