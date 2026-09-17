// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The portal's script, executed. build/check-portal-script.py hands the page
// (Portal.html) and the script (Portal.js) here as the files they are; this
// builds a small DOM from the markup, answers fetch out of a table of
// recorded API responses, runs the script in that world and then asserts what
// the reader would see.
//
// The DOM is deliberately small and deliberately hand-written: no dependency,
// so this runs on a bare runner with nothing installed, and nothing in it is
// magic - if the script reaches for something that is not here, the test fails
// with the name of it rather than passing quietly. Two places where it differs
// from a browser, and both are in the direction of determinism: hashchange is
// fired on a microtask rather than a task, and setTimeout is a queue this file
// fires by hand, so the change probe is tested without waiting six seconds.

'use strict';

const fs = require('fs');
const vm = require('vm');

const [pagePath, scriptPath] = process.argv.slice(2);
if (!pagePath || !scriptPath) {
   console.error('usage: node build/portal-script-test.js <portal.html> <portal.js>');
   process.exit(2);
}
// The catalogues sit beside the page, one file per language; the server
// answers /portal-lang/<code>.json out of them and so does the fetch stub
// below, so a language the test chooses is the language that shipped.
const cataloguesDir = pagePath.slice(0, Math.max(pagePath.lastIndexOf('/'), pagePath.lastIndexOf('\\')) + 1) + 'PortalLanguages/';

/* ------------------------------------------------------------------ the DOM */

const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source', 'track', 'wbr']);
const RAW = new Set(['script', 'style']);

class TextNode {
   constructor(data) { this.data = data; this.parentNode = null; this.childNodes = []; this.nodeType = 3; }
   get textContent() { return this.data; }
   set textContent(v) { this.data = String(v); }
   get nodeValue() { return this.data; }
   set nodeValue(v) { this.data = String(v); }
}

class Element {
   constructor(tag) {
      this.tagName = String(tag).toUpperCase();
      this.nodeName = this.tagName;
      // An element is 1 and a text node is 3, as the DOM says: the localiser
      // walks the markup by them, so without them it would walk past every text.
      this.nodeType = 1;
      this.attributes = Object.create(null);
      this.childNodes = [];
      this.parentNode = null;
      this.listeners = Object.create(null);
      this.style = {};
      this.hidden = false;
      this.value = '';
      this.checked = false;
      this.scrollTop = 0;
      this.scrolledIntoView = 0;
      this.focused = 0;
   }
   get id() { return this.attributes.id || ''; }
   // The page names two elements it has just made - the label menu's new-label
   // field and its button - and a getter with no setter made that a TypeError
   // from inside renderLabelMenu, so the label menu had never been opened by
   // any check here. A browser puts a named element within reach of
   // getElementById; the newest of a name wins, as the newest render is the
   // menu on screen.
   set id(v) { this.attributes.id = String(v); if (parsed && parsed.byId) { parsed.byId[String(v)] = this; } }
   get children() { return this.childNodes.filter((n) => n instanceof Element); }
   get firstChild() { return this.childNodes.length ? this.childNodes[0] : null; }
   get className() { return this.attributes.class || ''; }
   set className(v) { this.attributes.class = String(v); }
   get classList() {
      const self = this;
      const parts = () => (self.className ? self.className.split(/\s+/).filter(Boolean) : []);
      const write = (list) => { self.className = list.join(' '); };
      return {
         contains: (c) => parts().indexOf(c) >= 0,
         add: (c) => { const p = parts(); if (p.indexOf(c) < 0) { p.push(c); write(p); } },
         remove: (c) => write(parts().filter((x) => x !== c)),
         toggle: (c, force) => {
            const on = force === undefined ? parts().indexOf(c) < 0 : !!force;
            if (on) { const p = parts(); if (p.indexOf(c) < 0) { p.push(c); write(p); } }
            else { write(parts().filter((x) => x !== c)); }
            return on;
         }
      };
   }
   get textContent() {
      return this.childNodes.map((n) => (n instanceof Element ? n.textContent : n.data)).join('');
   }
   set textContent(v) {
      this.childNodes.forEach((n) => { n.parentNode = null; });
      this.childNodes = [];
      if (v !== '' && v !== null && v !== undefined) { this.appendChild(new TextNode(String(v))); }
   }
   appendChild(child) {
      if (child.parentNode) { child.parentNode.removeChild(child); }
      child.parentNode = this;
      this.childNodes.push(child);
      return child;
   }
   removeChild(child) {
      const at = this.childNodes.indexOf(child);
      if (at >= 0) { this.childNodes.splice(at, 1); child.parentNode = null; }
      return child;
   }
   setAttribute(name, value) {
      this.attributes[name] = String(value);
      if (name === 'hidden') { this.hidden = true; }
      if (name === 'value') { this.value = String(value); }
      if (name === 'type') { this.type = String(value); }
   }
   getAttribute(name) { return name in this.attributes ? this.attributes[name] : null; }
   hasAttribute(name) { return name in this.attributes; }
   removeAttribute(name) { delete this.attributes[name]; if (name === 'hidden') { this.hidden = false; } }
   addEventListener(type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); }
   removeEventListener(type, fn) {
      const list = this.listeners[type] || [];
      const at = list.indexOf(fn);
      if (at >= 0) { list.splice(at, 1); }
   }
   dispatchEvent(event) {
      event.target = event.target || this;
      let node = this;
      while (node) {
         (node.listeners[event.type] || []).slice().forEach((fn) => fn.call(node, event));
         if (event.propagationStopped) { return !event.defaultPrevented; }
         node = node.parentNode;
      }
      return !event.defaultPrevented;
   }
   focus() { this.focused += 1; document.activeElement = this; }
   // The page presses its own buttons in a few places - Save before Close in
   // the compose window, the file picker behind Attach - and a DOM without
   // this reports it as "click is not a function" from inside a handler,
   // which names neither the button nor the reason.
   click() { this.dispatchEvent(makeEvent('click')); }
   scrollIntoView() { this.scrolledIntoView += 1; }
   /* There is no layout in this DOM, so an element has the rectangle a test
      gives it (el.rect = {left, top, width, height}) and otherwise none at all.
      That is enough for the tour, which asks only where the thing it points at
      is; an element with no rectangle is one it draws no ring around, which is
      what a browser reports for something that is not on the screen. */
   getBoundingClientRect() {
      const r = this.rect || { left: 0, top: 0, width: 0, height: 0 };
      return { left: r.left, top: r.top, width: r.width, height: r.height, x: r.left, y: r.top,
               right: r.left + r.width, bottom: r.top + r.height };
   }
   get offsetHeight() { return (this.rect || { height: 0 }).height; }
   querySelector() { throw new Error('the portal script must not need querySelector'); }
}

function makeEvent(type, extra) {
   const event = Object.assign({
      type,
      defaultPrevented: false,
      propagationStopped: false,
      preventDefault() { this.defaultPrevented = true; },
      stopPropagation() { this.propagationStopped = true; }
   }, extra || {});
   return event;
}

function parseHtml(text) {
   const root = new Element('#document-fragment');
   const stack = [root];
   const byId = Object.create(null);
   let i = 0;
   const top = () => stack[stack.length - 1];
   while (i < text.length) {
      const lt = text.indexOf('<', i);
      if (lt < 0) {
         if (i < text.length) { top().appendChild(new TextNode(text.slice(i))); }
         break;
      }
      if (lt > i) { top().appendChild(new TextNode(text.slice(i, lt))); }
      if (text.startsWith('<!--', lt)) { i = text.indexOf('-->', lt); i = i < 0 ? text.length : i + 3; continue; }
      if (text.startsWith('<!', lt)) { i = text.indexOf('>', lt); i = i < 0 ? text.length : i + 1; continue; }
      const gt = text.indexOf('>', lt);
      if (gt < 0) { break; }
      const inside = text.slice(lt + 1, gt);
      i = gt + 1;
      if (inside.startsWith('/')) {
         const name = inside.slice(1).trim().toLowerCase();
         for (let n = stack.length - 1; n > 0; n -= 1) {
            if (stack[n].tagName === name.toUpperCase()) { stack.length = n; break; }
         }
         continue;
      }
      const nameEnd = inside.search(/[\s/]/);
      const name = (nameEnd < 0 ? inside : inside.slice(0, nameEnd)).toLowerCase();
      const element = new Element(name);
      const attrText = nameEnd < 0 ? '' : inside.slice(nameEnd);
      const attr = /([:\w-]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'>]+)))?/g;
      let match;
      while ((match = attr.exec(attrText)) !== null) {
         const value = match[2] !== undefined ? match[2] : match[3] !== undefined ? match[3] : match[4] !== undefined ? match[4] : '';
         element.attributes[match[1]] = value;
      }
      if ('hidden' in element.attributes) { element.hidden = true; }
      if ('value' in element.attributes) { element.value = element.attributes.value; }
      if ('checked' in element.attributes) { element.checked = true; }
      if ('type' in element.attributes) { element.type = element.attributes.type; }
      if (element.attributes.id) { byId[element.attributes.id] = element; }
      top().appendChild(element);
      if (RAW.has(name)) {
         const close = text.toLowerCase().indexOf('</' + name, i);
         const end = close < 0 ? text.length : close;
         if (end > i) { element.appendChild(new TextNode(text.slice(i, end))); }
         i = close < 0 ? text.length : text.indexOf('>', close) + 1;
         continue;
      }
      if (!VOID.has(name) && !inside.trim().endsWith('/')) { stack.push(element); }
   }
   return { root, byId };
}

const parsed = parseHtml(fs.readFileSync(pagePath, 'utf8'));

const document = new Element('#document');
document.byId = parsed.byId;
document.hidden = false;
document.activeElement = null;
document.getElementById = function (id) {
   const found = parsed.byId[id];
   if (!found) { throw new Error('the script asked for an element the page does not have: #' + id); }
   return found;
};
document.createElement = function (tag) { return new Element(tag); };
document.body = (function find(node) {
   if (node instanceof Element && node.tagName === 'BODY') { return node; }
   for (const child of node.childNodes) { const hit = find(child); if (hit) { return hit; } }
   return null;
})(parsed.root);
if (!document.body) { throw new Error('the page has no <body>'); }
(function () {
   // Everything under <html> bubbles to the document, so a keydown on an input
   // reaches the handler the script puts on the document, as in a browser.
   let node = document.body;
   while (node.parentNode && node.parentNode !== parsed.root) { node = node.parentNode; }
   node.parentNode = document;
   // <html> itself: what the script writes lang and dir on.
   document.documentElement = node;
})();

/* -------------------------------------------------- window, location, timers */

const window = new Element('#window');
// A confirm box the test answers: what was asked is recorded, the answer is scripted.
const confirms = [];
let confirmAnswer = true;
window.confirm = (text) => { confirms.push(String(text)); return confirmAnswer; };
// A new window is recorded, not opened.
const opened = [];
window.open = (url, name, features) => { opened.push({ url: String(url), name: String(name || ''), features: String(features || '') }); return null; };
const historyStack = [''];
const navigatedTo = [];
let historyAt = 0;

function fireHashChange() {
   Promise.resolve().then(() => window.dispatchEvent(makeEvent('hashchange')));
}

const location = {
   origin: 'http://portal.test',
   get href() { return this.origin + '/portal' + this.hash; },
   set href(value) { navigatedTo.push(String(value)); },
   get hash() { return historyStack[historyAt] ? '#' + historyStack[historyAt] : ''; },
   set hash(value) {
      const wanted = String(value).replace(/^#/, '');
      if (wanted === historyStack[historyAt]) { return; }
      historyStack.length = historyAt + 1;
      historyStack.push(wanted);
      historyAt += 1;
      fireHashChange();
   },
   replace(value) { historyStack[historyAt] = String(value).replace(/^#/, ''); fireHashChange(); }
};

const history = {
   get length() { return historyStack.length; },
   replaceState(state, title, url) { historyStack[historyAt] = String(url).replace(/^#/, ''); },
   pushState(state, title, url) {
      historyStack.length = historyAt + 1;
      historyStack.push(String(url).replace(/^#/, ''));
      historyAt += 1;
   },
   back() { if (historyAt > 0) { historyAt -= 1; fireHashChange(); } },
   forward() { if (historyAt < historyStack.length - 1) { historyAt += 1; fireHashChange(); } }
};

const timers = [];
const intervals = [];
let timerId = 1;
function fireTimers() {
   const due = timers.splice(0, timers.length);
   due.forEach((t) => t.fn());
   return due.length;
}
// A repeating timer is a queue this file fires by hand rather than node's own:
// a real one would keep the process alive after the last check and would fire
// whenever the runner happened to be slow, which is how a test starts failing
// on a busy machine and nowhere else.
function fireIntervals() { intervals.slice().forEach((t) => t.fn()); return intervals.length; }

const store = new Map();
const localStorage = {
   getItem: (k) => (store.has(k) ? store.get(k) : null),
   setItem: (k, v) => store.set(k, String(v)),
   removeItem: (k) => store.delete(k),
   get length() { return store.size; }
};

class DOMParserStub {
   parseFromString(text) {
      const tree = parseHtml(text);
      const body = (function find(node) {
         if (node instanceof Element && node.tagName === 'BODY') { return node; }
         for (const child of node.childNodes) { const hit = find(child); if (hit) { return hit; } }
         return null;
      })(tree.root);
      return { body: body || tree.root };
   }
}

class FileReaderStub {
   readAsDataURL(blob) {
      const self = this;
      Promise.resolve().then(() => {
         if (!blob || blob.broken) { if (self.onerror) { self.onerror(); } return; }
         self.result = 'data:' + (blob.type || 'application/octet-stream') + ';base64,' + (blob.base64 || '');
         if (self.onload) { self.onload(); }
      });
   }
}

/* ------------------------------------------------------- the recorded server */

const requests = [];
let signedIn = false;
let changeStep = 0;
let renameRefusal = null;
// What POST /api/v1/me/password answers: a changed password, unless a test
// sets a refusal with its status and body.
let passwordAnswer = null;
// The account's own safe and blocked senders, and whether the digest reaches
// it. Both change as the page writes to them.
let senderList = [{ id: 401, scope: 'account', value: 'friend@example.org', blocked: false, reason: 'a customer', expires: false, expired: false }];
const digestState = { available: true, enabled: true, hour: 7, last_sent: '2026-09-14 07:05:00' };
// The next send is refused as the suppression list refuses one, once.
let sendRefusal = null;
// One message arrives while the reader is looking at the folder: the probe
// says so, and from then on the folder tree and the listing both show it.
let arrived = false;
// The preferences the page saves, answered back merged, as the server does.
let prefsStore = {};
// A server that does not answer the change route at all - an older one, or one
// built before that route existed.
let changesMissing = false;
// One more change reported by the next probe, whatever step it is at.
let changeAgain = false;
// Which account the recorded server answers as. The sign-out checks at the
// end sign in twice, as two different people in one browser, which is the
// only way to see what one of them left behind for the other.
let account = 'user@example.com';
// The S/MIME store, per account: the rows GET /api/v1/me/smime answers with.
// A row's wrapped key is the stub module's own shape (see smimeStub below).
const smimeStores = {};
const wrappedUnder = (password) => ({ v: 1, kdf: 'PBKDF2-SHA256', iterations: 600000, salt: 'AAAAAAAAAAAAAAAAAAAAAA==', iv: 'AAAAAAAAAAAAAAAA', data: String(password) });
const ownKeyOf = (address, password) => ({ id: 1, address, name: 'Key of ' + address, fingerprint: 'f'.repeat(64), certificate: 'MAMCAQE=', chain: [], key: wrappedUnder(password), not_after: 2000000000, created: 1700000000 });
// One message in the shape the S/MIME reader looks at: the headers say it is
// sealed, so opening it hands the page's unlocked keys to the module.
const SEALED = {
   id: 601, folder_id: 1, uid: 21, subject: 'Sealed', from: 'b@example.net', to: 'user@example.com', cc: '',
   date: '2026-09-09 14:00', size: 900, message_id: '<sealed@example.net>', references: '', in_reply_to: '',
   headers: 'Content-Type: application/pkcs7-mime; smime-type=enveloped-data; name="smime.p7m"\r\nFrom: b@example.net\r\n',
   text: '', html: '', html_remote: false, flags: { seen: true, flagged: false, draft: false }, attachments: []
};

const INBOX = { id: 1, account_id: 7, name: 'INBOX', path: 'INBOX', parent_id: 0, special_use: '', subscribed: true, writable: true, messages: 3, unseen: 2, uidvalidity: 1, subfolders: [] };
const SENT = { id: 2, account_id: 7, name: 'Sent', path: 'Sent', parent_id: 0, special_use: '\\Sent', subscribed: true, writable: true, messages: 1, unseen: 0, uidvalidity: 1, subfolders: [] };
const WORK = { id: 3, account_id: 7, name: 'Work', path: 'INBOX.Work', parent_id: 1, special_use: '', subscribed: true, writable: true, messages: 0, unseen: 0, uidvalidity: 1, subfolders: [] };

function folderTree() {
   const inbox = Object.assign({}, INBOX, { subfolders: [Object.assign({}, WORK)] });
   if (arrived) { inbox.messages = 4; inbox.unseen = 3; }
   return { delimiter: '.', folders: [inbox, Object.assign({}, SENT)], shared: [] };
}

// The flags the page changes, kept per message and answered back in every
// listing, search and message from then on: the star and the keywords. One
// message starts with a follow-up due today, so the reminder has something
// to say at sign-in.
const todayStamp = (() => { const d = new Date(); return d.getFullYear() + '-' + String(d.getMonth() + 1).padStart(2, '0') + '-' + String(d.getDate()).padStart(2, '0'); })();
// A listing date that many days ago, in the shape the server writes, so the
// nudges - which count days from today - read the same whenever this runs.
const stampDaysAgo = (days) => { const d = new Date(Date.now() - days * 86400000); return d.getFullYear() + '-' + String(d.getMonth() + 1).padStart(2, '0') + '-' + String(d.getDate()).padStart(2, '0') + ' ' + String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0'); };
const flaggedById = { 101: true };
const keywordsById = { 101: ['$FollowUp', '$Due-' + todayStamp] };
// Armed for the next request a star sends - the one carrying the flag and
// the star keywords together. The change is stored as usual, but the answer
// carries a keyword list the page cannot use, so the work the page does with
// the answer throws. What is being asked is not that this server would ever
// send such a thing: it is that one press that throws does not take that
// message's star down with it for the rest of the session.
let brokenFlagsAnswerOnce = false;
const isStarRequest = (body) => /"keywords_remove"/.test(body || '') && /\$Star2/i.test(body || '');
const withState = (m) => Object.assign({}, m, { flags: Object.assign({}, m.flags, { flagged: !!flaggedById[m.id], keywords: (keywordsById[m.id] || []).slice() }) });

const LISTING = {
   total: 5,
   messages: [
      // Received five days ago, asks a question, never answered: the shape a nudge is for.
      { id: 103, uid: 13, subject: 'Third', from: 'c@example.net', to: 'user@example.com', date: stampDaysAgo(5), size: 900, snippet: 'Can you send the figures?', flags: { seen: false, flagged: false, draft: false } },
      { id: 102, uid: 12, subject: 'Second', from: 'b@example.net', to: 'user@example.com', date: '2026-09-08 09:00', size: 800, flags: { seen: false, flagged: false, draft: false }, in_reply_to: '<one@example.net>', brand: 'example.net' },
      { id: 101, uid: 11, subject: 'First', from: 'a@example.net', to: 'user@example.com', date: '2026-09-07 09:00', size: 700, flags: { seen: true, flagged: false, draft: false } }
   ]
};

// The Sent folder: one message the inbox's 'Second' answers, one nothing
// answered, sent long enough ago to be nudged, and one sent yesterday.
const SENT_LISTING = [
   { id: 401, uid: 1, subject: 'Figures for the board', from: 'user@example.com', to: 'alice@example.net', date: stampDaysAgo(6), size: 500, message_id: '<one@example.net>', flags: { seen: true, flagged: false, draft: false } },
   { id: 402, uid: 2, subject: 'Draft agenda', from: 'user@example.com', to: 'bob@example.net', date: stampDaysAgo(5), size: 500, message_id: '<agenda@example.com>', flags: { seen: true, flagged: false, draft: false } },
   { id: 403, uid: 3, subject: 'Thanks', from: 'user@example.com', to: 'carol@example.net', date: stampDaysAgo(1), size: 300, message_id: '<thanks@example.com>', flags: { seen: true, flagged: false, draft: false } }
];

// The Work folder: one conversation of three. The first is quoted whole in
// the second; the second is not quoted in the third. Clean up should take
// the first alone.
const WORK_THREAD = [
   { id: 503, uid: 3, subject: 'Re: Plan', from: 'b@example.net', to: 'user@example.com', date: '2026-09-03 11:00', size: 300, message_id: '<plan-3@example.net>', in_reply_to: '<plan-2@example.net>', references: '<plan@example.net> <plan-2@example.net>', flags: { seen: true, flagged: false, draft: false }, text: 'Ten it is, with the figures.' },
   { id: 502, uid: 2, subject: 'Re: Plan', from: 'user@example.com', to: 'b@example.net', date: '2026-09-02 11:00', size: 300, message_id: '<plan-2@example.net>', in_reply_to: '<plan@example.net>', references: '<plan@example.net>', flags: { seen: true, flagged: false, draft: false }, text: 'Ten works.\n\nOn Tuesday, b@example.net wrote:\n> Shall we meet at ten?\n> Bring the  figures.' },
   { id: 501, uid: 1, subject: 'Plan', from: 'b@example.net', to: 'user@example.com', date: '2026-09-01 11:00', size: 300, message_id: '<plan@example.net>', in_reply_to: '', references: '', flags: { seen: true, flagged: false, draft: false }, text: 'Shall we meet at ten?\nBring the figures.\n-- \nB' }
];

// The rest of the folder, two pages deep: what the page asks for with before_uid.
const OLDER = [
   { id: 100, uid: 10, subject: 'Older', from: 'e@example.net', to: 'user@example.com', date: '2026-09-06 09:00', size: 500, flags: { seen: true, flagged: false, draft: false } },
   { id: 99, uid: 9, subject: 'Oldest', from: 'e@example.net', to: 'user@example.com', date: '2026-09-05 09:00', size: 500, flags: { seen: true, flagged: false, draft: false } }
];

const ARRIVAL = { id: 104, uid: 14, subject: 'Just in', from: 'd@example.net', to: 'user@example.com', date: '2026-09-09 12:00', size: 400, flags: { seen: false, flagged: false, draft: false } };

const WITH_IMAGE = {
   id: 102, folder_id: 1, uid: 12, subject: 'Second', from: 'Bee <b@example.net>', to: 'user@example.com', cc: '', brand: 'example.net',
   date: '2026-09-08 09:00', received: '2026-09-08 09:00:00', size: 800, message_id: '<two@example.net>', references: '', in_reply_to: '<one@example.net>',
   text: '', html: '<p>Hello</p><img src="cid:logo@example.net" alt="logo"><img src="cid:chart@example.net" alt="chart"><img src="https://tracker.example.org/pixel.gif">',
   html_remote: true,
   flags: { seen: false, flagged: false, draft: false },
   attachments: [
      { index: 0, name: 'notes.txt', size: 12, content_type: 'text/plain', content_id: '' },
      { index: 1, name: 'logo.png', size: 3400, content_type: 'image/png', content_id: '<logo@example.net>' },
      { index: 2, name: 'chart.svg', size: 900, content_type: 'image/svg+xml', content_id: 'chart@example.net' }
   ]
};

// A draft this webmail saved: a file, the signature's logo as the server makes
// an image a part (img<n>.<32 hex>@host), one the S/MIME module made
// (img<n>.<16 hex>@webmail.invalid), and a photo another mail client embedded
// under a Content-ID of its own.
const DRAFT_WITH_LOGO = {
   id: 105, folder_id: 1, uid: 15, subject: 'Unfinished', from: 'user@example.com', to: 'b@example.net', cc: '',
   date: '2026-09-09 13:00', size: 900, message_id: '<draft@example.com>', references: '', in_reply_to: '',
   text: 'Not yet', html: '<p>Not yet</p><img src="cid:img1.0123456789abcdef0123456789abcdef@mail.example.com" alt=""><img src="cid:img1.0123456789abcdef@webmail.invalid" alt=""><img src="cid:image001.jpg@01DC2A3B.4C5D6E70" alt="">',
   html_remote: false,
   flags: { seen: true, flagged: false, draft: true },
   attachments: [
      { index: 0, name: 'notes.txt', size: 12, content_type: 'text/plain', content_id: '' },
      { index: 1, name: 'image1.png', size: 3400, content_type: 'image/png', content_id: '<img1.0123456789abcdef0123456789abcdef@mail.example.com>' },
      { index: 2, name: 'image1.png', size: 3400, content_type: 'image/png', content_id: 'img1.0123456789abcdef@webmail.invalid' },
      { index: 3, name: 'photo.jpg', size: 5100, content_type: 'image/jpeg', content_id: '<image001.jpg@01DC2A3B.4C5D6E70>' }
   ]
};

function json(status, body, headers) {
   return { status, body: body === undefined ? '' : JSON.stringify(body), headers: headers || {} };
}

// The server's half of Web Push: the switch, the application server key and the
// devices. pushRow is the listing's shape, and the assertion that matters is
// what it does NOT carry - the endpoint and the two keys are the capability to
// notify the device, and the real route never gives them back.
let pushEnabled = true;
const VAPID_PUBLIC = 'BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8';
const pushDevices = [];
function pushRow(device) {
   return {
      id: device.id, device: device.device, created: device.created, last_sent: device.last_sent,
      state: device.state, failures: device.failures, next_attempt: 0, last_detail: device.last_detail,
      detail: device.detail, senders: device.senders, labels: device.labels,
      quiet_from: device.quiet_from, quiet_to: device.quiet_to, quiet_offset: device.quiet_offset,
      service: 'https://push.example.net'
   };
}

// The calendar, the tasks and an invitation: what the page's calendar and
// tasks pages and the invitation card read. The events are answered relative
// to the window asked for, so the month on the page - whatever month the run
// happens in - has one recurring standup and one all-day event in it; the
// wall clock of the standup (calWall) is what the drag and resize checks
// expect to see written back.
let calendarsState = [
   { id: 1, name: 'calendar', display_name: 'Calendar', color: '', owner: 'user@example.com', mine: true, writable: true, is_default: true, sync_token: 1, shares: [] },
   { id: 2, name: 'work', display_name: 'Work', color: '#1a73e8', owner: 'user@example.com', mine: true, writable: true, is_default: false, sync_token: 1, shares: [{ address: 'b@example.net', write: false }] },
   { id: 3, name: 'team', display_name: 'Team', color: '#d93025', owner: 'b@example.net', mine: false, writable: false, is_default: false, sync_token: 1, shares: [] }
];
const calWall = { start: null, end: null };
function utcStamp(d) { const p = (n) => (n < 10 ? '0' : '') + n; return d.getUTCFullYear() + p(d.getUTCMonth() + 1) + p(d.getUTCDate()) + 'T' + p(d.getUTCHours()) + p(d.getUTCMinutes()) + p(d.getUTCSeconds()) + 'Z'; }
function localStamp(d) { const p = (n) => (n < 10 ? '0' : '') + n; return d.getUTCFullYear() + '-' + p(d.getUTCMonth() + 1) + '-' + p(d.getUTCDate()) + 'T' + p(d.getUTCHours()) + ':' + p(d.getUTCMinutes()); }
function eventsIn(query) {
   const m = /start=(\d{8}T\d{6}Z)&end=(\d{8}T\d{6}Z)/.exec(query || '');
   if (!m) { return []; }
   const startOf = (s) => new Date(Date.UTC(+s.slice(0, 4), +s.slice(4, 6) - 1, +s.slice(6, 8), +s.slice(9, 11), +s.slice(11, 13), +s.slice(13, 15)));
   const from = startOf(m[1]), to = startOf(m[2]);
   const out = [];
   // The standup: 09:00-09:30 UTC on the tenth day of the window, or the first when the window is short.
   const day = new Date(from.getTime() + (to - from > 20 * 86400000 ? 10 : to - from > 2 * 86400000 ? 1 : 0) * 86400000);
   const s = new Date(Date.UTC(day.getUTCFullYear(), day.getUTCMonth(), day.getUTCDate(), 9, 0, 0));
   const e = new Date(s.getTime() + 30 * 60000);
   if (s >= from && s < to) {
      calWall.start = s;
      calWall.end = e;
      out.push({ id: 10, calendar: 2, uid: 'standup@example.com', start: utcStamp(s), end: utcStamp(e), local_start: localStamp(s), local_end: localStamp(e), recurring: true, recurrence_id: '20260921T090000Z',
         overridden: false, writable: true, mine: true, color: '#1a73e8', summary: 'Standup', location: '', description: '', tz: 'UTC', rrule: 'FREQ=WEEKLY;COUNT=3', alarm_minutes: 10,
         organizer: 'user@example.com', organizer_name: '', attendees: [{ address: 'guest@example.com', name: 'Guest Person', partstat: 'ACCEPTED' }], my_partstat: '', status: '', sequence: 1, all_day: false });
   }
   const allDay = new Date(from.getTime() + 12 * 86400000);
   const a = new Date(Date.UTC(allDay.getUTCFullYear(), allDay.getUTCMonth(), allDay.getUTCDate()));
   if (a >= from && a < to) {
      out.push({ id: 11, calendar: 1, uid: 'away@example.com', start: utcStamp(a), end: utcStamp(new Date(a.getTime() + 86400000)), local_start: localStamp(a).slice(0, 10), local_end: localStamp(new Date(a.getTime() + 86400000)).slice(0, 10),
         recurring: false, recurrence_id: '', overridden: false, writable: true, mine: true, color: '', summary: 'Away', location: '', description: '', tz: '', rrule: '', alarm_minutes: -1,
         organizer: '', organizer_name: '', attendees: [], my_partstat: '', status: '', sequence: 0, all_day: true });
   }
   return out;
}
let tasksState = [
   { id: 21, calendar: 1, uid: 'milk@example.com', summary: 'Buy milk', description: 'Two litres.', due: '20260920T000000Z', local_due: '2026-09-20', all_day: true, tz: '', completed: false, completed_at: '', priority: 0, status: 'NEEDS-ACTION', writable: true, mine: true, color: '', sequence: 0 },
   { id: 22, calendar: 2, uid: 'report@example.com', summary: 'Write the report', description: '', due: '20200101T090000Z', local_due: '2020-01-01T09:00', all_day: false, tz: 'UTC', completed: false, completed_at: '', priority: 1, status: 'NEEDS-ACTION', writable: true, mine: true, color: '#1a73e8', sequence: 0 },
   { id: 23, calendar: 1, uid: 'old@example.com', summary: 'Old chore', description: '', due: '', local_due: '', all_day: false, tz: '', completed: true, completed_at: '20260901T100000Z', priority: 0, status: 'COMPLETED', writable: true, mine: true, color: '', sequence: 1 }
];
const INVITED = {
   id: 106, folder_id: 1, uid: 16, subject: 'Invitation: Board meeting', from: 'Sam Self <self@example.com>', to: 'user@example.com', cc: '',
   date: '2026-09-10 09:00', size: 2600, message_id: '<invite@example.com>', references: '', in_reply_to: '',
   text: 'You are invited to: Board meeting', html: '', html_remote: false, invitation: true,
   flags: { seen: true, flagged: false, draft: false }, attachments: []
};
let invitationAnswer = '';
function invitationJson() {
   return { method: 'REQUEST', component: 'VEVENT', uid: 'board@example.com', start: '20261019T080000Z', end: '20261019T090000Z', local_start: '2026-10-19T09:00', local_end: '2026-10-19T10:00', recurring: false,
      in_calendar: invitationAnswer !== '', event_id: invitationAnswer ? 30 : 0, calendar: invitationAnswer ? 1 : 0, stored_partstat: invitationAnswer, stored_status: '', i_am_organizer: false,
      summary: 'Board meeting', location: 'Room 9', description: 'Bring numbers.', tz: 'Europe/London', rrule: '', alarm_minutes: -1, organizer: 'self@example.com', organizer_name: 'Sam Self',
      attendees: [{ address: 'user@example.com', name: '', partstat: invitationAnswer || 'NEEDS-ACTION' }], my_partstat: invitationAnswer || 'NEEDS-ACTION', status: '', sequence: 0, all_day: false };
}
function answerCalendar(method, path, body) {
   if (path === '/api/v1/me/calendars' && method === 'GET') { return json(200, { calendars: calendarsState }); }
   if (path === '/api/v1/me/calendars' && method === 'POST') {
      const wanted = JSON.parse(body || '{}');
      const made = { id: 4 + calendarsState.length, name: String(wanted.display_name || '').toLowerCase(), display_name: wanted.display_name, color: wanted.color || '', owner: 'user@example.com', mine: true, writable: true, is_default: false, sync_token: 1, shares: [] };
      calendarsState = calendarsState.concat([made]);
      return json(201, made);
   }
   let m = /^\/api\/v1\/me\/calendars\/(\d+)\/shares\/(.+)$/.exec(path);
   if (m && method === 'PUT') { const c = calendarsState.filter((x) => x.id === Number(m[1]))[0]; if (!c) { return json(404, { error: 'calendar not found' }); } c.shares = c.shares.concat([{ address: decodeURIComponent(m[2]), write: !!JSON.parse(body || '{}').write }]); return json(200, c); }
   if (m && method === 'DELETE') { const c = calendarsState.filter((x) => x.id === Number(m[1]))[0]; if (!c) { return json(404, { error: 'calendar not found' }); } c.shares = c.shares.filter((s) => s.address !== decodeURIComponent(m[2])); return json(200, c); }
   m = /^\/api\/v1\/me\/calendars\/(\d+)$/.exec(path);
   if (m && method === 'PUT') { const c = calendarsState.filter((x) => x.id === Number(m[1]))[0]; if (!c) { return json(404, { error: 'calendar not found' }); } Object.assign(c, JSON.parse(body || '{}')); return json(200, c); }
   if (m && method === 'DELETE') { calendarsState = calendarsState.filter((x) => x.id !== Number(m[1])); return json(200, { deleted: true }); }
   if (path.startsWith('/api/v1/me/calendar/events?') && method === 'GET') { return json(200, { events: eventsIn(path.split('?')[1]) }); }
   if (path === '/api/v1/me/calendar/events' && method === 'POST') { return json(201, { id: 40, uid: 'new@example.com', calendar: 1, invited: 2 }); }
   m = /^\/api\/v1\/me\/calendar\/events\/(\d+)(\?.*)?$/.exec(path);
   if (m && method === 'PUT') { return json(200, { id: Number(m[1]), uid: 'x', calendar: 2 }); }
   if (m && method === 'DELETE') { return json(200, { removed: 'event', id: Number(m[1]) }); }
   if (m && method === 'GET') { return json(200, { id: Number(m[1]) }); }
   if (path === '/api/v1/me/tasks' && method === 'GET') { return json(200, { tasks: tasksState }); }
   if (path === '/api/v1/me/tasks' && method === 'POST') {
      const wanted = JSON.parse(body || '{}');
      const made = { id: 24, calendar: wanted.calendar || 1, uid: 'bank@example.com', summary: wanted.summary, description: '', due: wanted.due ? wanted.due.replace(/-/g, '') + 'T000000Z' : '', local_due: wanted.due || '', all_day: true, tz: '', completed: false, completed_at: '', priority: 0, status: 'NEEDS-ACTION', writable: true, mine: true, color: '', sequence: 0 };
      tasksState = tasksState.concat([made]);
      return json(201, made);
   }
   m = /^\/api\/v1\/me\/tasks\/(\d+)$/.exec(path);
   if (m && method === 'PUT') { const task = tasksState.filter((x) => x.id === Number(m[1]))[0]; if (!task) { return json(404, { error: 'task not found' }); } const wanted = JSON.parse(body || '{}'); if (wanted.completed !== undefined) { task.completed = !!wanted.completed; } if (wanted.summary) { task.summary = wanted.summary; } return json(200, task); }
   if (m && method === 'DELETE') { tasksState = tasksState.filter((x) => x.id !== Number(m[1])); return json(200, { removed: true }); }
   if (path === '/api/v1/me/messages/106' && method === 'GET') { return json(200, withState(JSON.parse(JSON.stringify(INVITED)))); }
   if (path === '/api/v1/me/messages/106/invitation' && method === 'GET') { return json(200, invitationJson()); }
   if (path === '/api/v1/me/messages/106/invitation/reply' && method === 'POST') {
      const wanted = JSON.parse(body || '{}');
      if (wanted.action === 'counter') { return json(200, { countered: true, to: 'self@example.com' }); }
      if (wanted.action === 'remove') { invitationAnswer = ''; return json(200, { removed: true }); }
      invitationAnswer = wanted.action === 'accept' ? 'ACCEPTED' : wanted.action === 'decline' ? 'DECLINED' : 'TENTATIVE';
      return json(200, { stored: true, event_id: 30, calendar: 1, partstat: invitationAnswer, replied: true, to: 'self@example.com' });
   }
   return null;
}

// The Subscriptions page: three senders offering a way out - one-click, a
// mailto and a page only - and what the page does with each. The answer can
// be made partial or empty, or refused, by the checks.
let subscriptionsMode = 'full';
const subscriptionDeletes = [];
function answerSubscriptions(method, path, body) {
   if (method === 'GET' && /^\/api\/v1\/me\/subscriptions(\?|$)/.test(path)) {
      if (subscriptionsMode === 'refused') { return json(500, { error: 'the mailbox could not be read' }); }
      const days = Number((/days=(\d+)/.exec(path) || [0, 90])[1]);
      const all = [
         { address: 'news@example.com', name: 'Weekly News', list_id: 'Weekly <weekly.example.com>', count: 12, unread: 4, first: '2026-06-20 08:00:00', last: '2026-09-15 08:00:00', message_id: 902, method: 'one-click', url: '' },
         { address: 'shop@example.com', name: '', list_id: '', count: 5, unread: 0, first: '2026-08-01 10:00:00', last: '2026-09-10 10:00:00', message_id: 911, method: 'mail', url: '' },
         { address: 'blog@example.com', name: 'A Blog', list_id: '', count: 2, unread: 2, first: '2026-09-01 12:00:00', last: '2026-09-12 12:00:00', message_id: 931, method: 'page', url: 'https://blog.example.com/settings' }
      ];
      const list = subscriptionsMode === 'empty' ? [] : all;
      return json(200, { days: days, since: '2026-06-19 00:00:00', scanned: subscriptionsMode === 'partial' ? 5000 : 19, complete: subscriptionsMode !== 'partial', subscriptions: list });
   }
   if (method === 'POST' && path === '/api/v1/me/messages/902/unsubscribe') { return json(200, { method: 'one-click', ok: true, status: 200, error: '' }); }
   if (method === 'POST' && path === '/api/v1/me/messages/911/unsubscribe') { return json(201, { method: 'mail', queued: true, to: 'shop-leave@example.com' }); }
   if (method === 'GET' && path.startsWith('/api/v1/me/search?q=' + encodeURIComponent('from:blog@example.com'))) {
      return json(200, { query: 'from:blog@example.com', scanned: 40, complete: true, more: false, messages: [
         { folder_id: 1, folder: 'INBOX', id: 931, uid: 31, size: 900, received: '2026-09-12 12:00:00', subject: 'New post', from: 'A Blog <blog@example.com>', date: '', flags: { seen: false, flagged: false, answered: false, draft: false, deleted: false, keywords: [] } },
         { folder_id: 3, folder: 'Work', id: 932, uid: 7, size: 800, received: '2026-09-01 12:00:00', subject: 'First post', from: 'A Blog <blog@example.com>', date: '', flags: { seen: false, flagged: false, answered: false, draft: false, deleted: false, keywords: [] } },
         { folder_id: 1, folder: 'INBOX', id: 933, uid: 33, size: 700, received: '2026-09-02 12:00:00', subject: 'Not theirs', from: 'someone@blog.example.com.evil.test', date: '', flags: { seen: false, flagged: false, answered: false, draft: false, deleted: false, keywords: [] } }
      ] });
   }
   if (method === 'DELETE' && /^\/api\/v1\/me\/messages\/93\d$/.test(path)) {
      subscriptionDeletes.push(Number(path.split('/').pop()));
      return json(200, { deleted: true, id: 9000 + subscriptionDeletes.length });
   }
   return null;
}
// The Other accounts page: one account the reader added and one an
// administrator set up to read recipients from the message, which the page
// shows and does not offer to change.
let fetchMode = 'on';
let fetchAccountsState = [
   { id: 71, name: 'Old provider', server_address: 'pop.old-provider.example', port: 995, server_type: 'pop3', username: 'me@old-provider.example',
     minutes_between_fetch: 30, days_to_keep_messages: -1, connection_security: 'tls', mime_recipient_headers: '', enabled: true,
     process_mime_recipients: false, process_mime_date: false, use_antispam: true, use_antivirus: true, enable_route_recipients: false,
     mirror_folders: false, locked: false, next_download_time: '2026-09-17 14:00:00' },
   { id: 72, name: 'Catch-all', server_address: 'mail.example.com', port: 110, server_type: 'pop3', username: 'catchall',
     minutes_between_fetch: 5, days_to_keep_messages: 0, connection_security: 'none', mime_recipient_headers: 'To,CC', enabled: true,
     process_mime_recipients: true, process_mime_date: false, use_antispam: true, use_antivirus: true, enable_route_recipients: false,
     mirror_folders: false, locked: false, next_download_time: '2026-09-17 13:05:00' }
];
// Send mail as: one address waiting for its code, at a domain that rejects
// mail it did not send, and one verified.
let senderAddresses = [
   { id: 5, address: 'me@strict.example', name: 'Me Elsewhere', verified: false, created: '2026-09-17 10:00:00', dmarc_policy: 'reject' },
   { id: 6, address: 'old@relaxed.example', name: '', verified: true, created: '2026-09-16 10:00:00', dmarc_policy: 'none' }
];
function answerSenderAddresses(method, path, body) {
   if (!path.startsWith('/api/v1/me/sender-addresses')) { return null; }
   if (path === '/api/v1/me/sender-addresses' && method === 'GET') { return json(200, { addresses: senderAddresses.slice(), max: 5 }); }
   if (path === '/api/v1/me/sender-addresses' && method === 'POST') {
      const wanted = JSON.parse(body || '{}');
      const made = { id: 10 + senderAddresses.length, address: String(wanted.address).toLowerCase(), name: wanted.name || '', verified: false, created: '2026-09-17 12:00:00', dmarc_policy: '', code_sent: true };
      senderAddresses.push(made);
      return json(201, made);
   }
   const m = /^\/api\/v1\/me\/sender-addresses\/(\d+)(\/verify|\/resend)?$/.exec(path);
   if (!m) { return json(404, { error: 'not found' }); }
   const at = senderAddresses.findIndex((a) => a.id === Number(m[1]));
   if (at < 0) { return json(404, { error: 'not one of your addresses' }); }
   if (m[2] === '/verify') {
      const typed = JSON.parse(body || '{}').code;
      if (typed === '123456') { senderAddresses[at].verified = true; return json(200, senderAddresses[at]); }
      return json(400, { error: 'that is not the code', attempts_left: 4 });
   }
   if (m[2] === '/resend') { return json(429, { error: 'a code went out less than a minute ago', retry_after: 42 }); }
   if (method === 'DELETE') { senderAddresses.splice(at, 1); return json(200, { deleted: true }); }
   return json(404, { error: 'not found' });
}
// Masked addresses: one on, one switched off.
let maskedAddresses = [
   { id: 3, address: 'bookshop.k3v9qa@example.com', label: 'Bookshop', active: true, received: 12, created: '2026-09-17 10:00:00' },
   { id: 2, address: 'news.a2b3c4@example.com', label: 'Newsletter', active: false, received: 340, created: '2026-09-01 10:00:00' }
];
function answerMaskedAddresses(method, path, body) {
   if (!path.startsWith('/api/v1/me/masked-addresses')) { return null; }
   if (path === '/api/v1/me/masked-addresses' && method === 'GET') { return json(200, { addresses: maskedAddresses.slice(), max: 20 }); }
   if (path === '/api/v1/me/masked-addresses' && method === 'POST') {
      const wanted = JSON.parse(body || '{}');
      const made = { id: 10 + maskedAddresses.length, address: 'garden.p7q8r9@example.com', label: wanted.label || '', active: true, received: 0, created: '2026-09-17 12:00:00' };
      maskedAddresses.unshift(made);
      return json(201, made);
   }
   const m = /^\/api\/v1\/me\/masked-addresses\/(\d+)$/.exec(path);
   const at = m ? maskedAddresses.findIndex((a) => a.id === Number(m[1])) : -1;
   if (at < 0) { return json(404, { error: 'not one of your masked addresses' }); }
   if (method === 'PUT') { Object.assign(maskedAddresses[at], JSON.parse(body || '{}')); return json(200, maskedAddresses[at]); }
   if (method === 'DELETE') { maskedAddresses.splice(at, 1); return json(200, { deleted: true }); }
   return json(404, { error: 'not found' });
}
// Recover deleted items: a window of 14 days and two deleted messages.
let recoverable = { days: 14, items: [
   { id: 41, folder_id: 1, folder: 'INBOX', subject: 'Quarterly figures', from: 'Finance <finance@example.com>', size: 20480, seen: true, deleted: 1789600000 },
   { id: 40, folder_id: 7, folder: 'Projects.Garden', subject: 'Seed order', from: 'shop@example.com', size: 4096, seen: false, deleted: 1789500000 }
] };
function answerRecoverable(method, path) {
   if (!path.startsWith('/api/v1/me/recoverable')) { return null; }
   if (path === '/api/v1/me/recoverable' && method === 'GET') { return json(200, { days: recoverable.days, items: recoverable.items.slice() }); }
   const m = /^\/api\/v1\/me\/recoverable\/(\d+)(\/restore)?$/.exec(path);
   const at = m ? recoverable.items.findIndex((i) => i.id === Number(m[1])) : -1;
   if (at < 0) { return json(404, { error: 'not found' }); }
   if (m[2] && method === 'POST') { const item = recoverable.items.splice(at, 1)[0]; return json(200, { folder_id: item.folder_id, message_id: 900 }); }
   if (!m[2] && method === 'DELETE') { recoverable.items.splice(at, 1); return json(200, { deleted: true }); }
   return json(404, { error: 'not found' });
}
// Messages that expire: offered, one not yet opened and set with a passcode, one
// opened by its recipient.
let confidentialOffered = true;
let confidential = [
   { id: 12, subject: 'The board pack', recipients: ['ann@example.net'], created: 1789600000, expires: 1789686400, expired: false, opened: 0, opened_by: '', revoked: false, passcode: true, link: 'https://mail.example.com/confidential/abc' },
   { id: 11, subject: 'Salary review', recipients: ['bob@example.net', 'cy@example.org'], created: 1789500000, expires: 1789586400, expired: false, opened: 1789510000, opened_by: 'bob@example.net', revoked: false, passcode: false, link: 'https://mail.example.com/confidential/def' }
];
function answerConfidential(method, path) {
   if (!path.startsWith('/api/v1/me/confidential')) { return null; }
   if (path === '/api/v1/me/confidential' && method === 'GET') { return json(200, { available: confidentialOffered, max_seconds: 7776000, items: confidential.map((i) => Object.assign({}, i)) }); }
   const m = /^\/api\/v1\/me\/confidential\/(\d+)\/revoke$/.exec(path);
   const at = m ? confidential.findIndex((i) => i.id === Number(m[1])) : -1;
   if (at < 0) { return json(404, { error: 'not one of your messages that expire' }); }
   if (method === 'POST') { confidential[at].revoked = true; return json(200, { revoked: true }); }
   return json(404, { error: 'not found' });
}
// Offline in the browser: a ceiling of 30 days and a reader who has chosen
// nothing yet, so nothing is fetched in the background until a check says so.
// The window's listing carries the three inbox messages, by id from the oldest
// up, with only what the page needs to decide what to fetch and what to drop.
let offlineWindow = { days_max: 30, days: 0 };
const offlineSince = '2026-08-26 00:00:00';
// And the one that arrived, once a probe has reported it into the window.
let offlineArrival = false;
// A server that could not read the choice - the database gone for a moment -
// answers 500, never "nothing chosen"; and a listing that says days 0 while
// the page holds a choice, which the page must not prune by.
let offlineRefused = false;
let offlineListingBlank = false;
function offlineRows() {
   return [101, 102, 103].concat(offlineArrival ? [104] : []).map((id) => {
      const m = LISTING.messages.concat([ARRIVAL]).find((x) => x.id === id);
      return { id, folder_id: 1, uid: m.uid, size: m.size, received: m.date + ':00', flags: withState(m).flags };
   });
}
function answerOffline(method, path, body) {
   if (!path.startsWith('/api/v1/me/offline')) { return null; }
   const days = Math.min(offlineWindow.days, offlineWindow.days_max);
   const window = () => json(200, { days_max: offlineWindow.days_max, days, since: days ? offlineSince : '' });
   if (offlineRefused && method === 'GET') { return json(500, { error: 'the choice could not be read' }); }
   if (offlineListingBlank && path.startsWith('/api/v1/me/offline/messages')) { return json(200, { days: 0, since: '', count: 0, bytes: 0, messages: [], more: false, next_after_id: 0 }); }
   if (path === '/api/v1/me/offline' && method === 'GET') { return window(); }
   if (path === '/api/v1/me/offline' && method === 'PUT') {
      const wanted = JSON.parse(body || '{}');
      if (typeof wanted.days !== 'number' || wanted.days < 0 || wanted.days > offlineWindow.days_max || wanted.days !== Math.floor(wanted.days)) {
         return json(400, { error: 'days is a whole number between 0 and ' + offlineWindow.days_max, days_max: offlineWindow.days_max });
      }
      offlineWindow.days = wanted.days;
      return json(200, { days_max: offlineWindow.days_max, days: wanted.days, since: wanted.days ? offlineSince : '' });
   }
   if (path.startsWith('/api/v1/me/offline/messages') && method === 'GET') {
      if (!days) { return json(200, { days: 0, since: '', count: 0, bytes: 0, messages: [], more: false, next_after_id: 0 }); }
      const rows = offlineRows();
      return json(200, { days, since: offlineSince, count: rows.length, bytes: rows.reduce((n, r) => n + r.size, 0), messages: rows, more: false, next_after_id: 0 });
   }
   return json(404, { error: 'not found' });
}
let importOffer = { google: true, microsoft: false };
function answerFetchAccounts(method, path, body) {
   if (!path.startsWith('/api/v1/me/fetch-accounts')) { return null; }
   if (fetchMode === 'off') { return json(403, { error: 'adding other accounts is not offered on this server' }); }
   if (path === '/api/v1/me/fetch-accounts/import' && method === 'GET') { return json(200, importOffer); }
   if (path === '/api/v1/me/fetch-accounts/import' && method === 'POST') {
      const wanted = JSON.parse(body || '{}');
      if (!importOffer[wanted.provider]) { return json(403, { error: 'signing in with that provider is not set up on this server' }); }
      return json(200, { url: 'https://accounts.google.example/auth?state=abc&code_challenge=xyz' });
   }
   if (path === '/api/v1/me/fetch-accounts' && method === 'GET') { return json(200, fetchAccountsState.slice()); }
   if (path === '/api/v1/me/fetch-accounts' && method === 'POST') {
      const wanted = JSON.parse(body || '{}');
      const made = Object.assign({ id: 80 + fetchAccountsState.length, enabled: true, locked: false, next_download_time: '2026-09-17 13:30:00',
         use_antispam: true, use_antivirus: true, process_mime_recipients: false, enable_route_recipients: false, mime_recipient_headers: '' }, wanted);
      delete made.password;
      fetchAccountsState.push(made);
      return json(201, made);
   }
   const m = /^\/api\/v1\/me\/fetch-accounts\/(\d+)(\/download)?$/.exec(path);
   if (!m) { return json(404, { error: 'not found' }); }
   const at = fetchAccountsState.findIndex((f) => f.id === Number(m[1]));
   if (at < 0) { return json(404, { error: 'fetch account not found' }); }
   if (m[2] && method === 'POST') { return json(202, { queued: true }); }
   if (method === 'PUT') { const wanted = JSON.parse(body || '{}'); delete wanted.password; Object.assign(fetchAccountsState[at], wanted); return json(200, fetchAccountsState[at]); }
   if (method === 'DELETE') { fetchAccountsState.splice(at, 1); return json(200, { deleted: true }); }
   return json(404, { error: 'not found' });
}
function answer(method, path, body) {
   if (method === 'POST' && path === '/api/v1/session') { signedIn = true; return json(201, { address: account }); }
   if (method === 'DELETE' && path === '/api/v1/session') { signedIn = false; return json(200, { ended: true }); }
   if (!signedIn) { return json(401, { error: 'Not signed in.' }); }
   { const calendar = answerCalendar(method, path, body); if (calendar) { return calendar; } }
   { const subscriptions = answerSubscriptions(method, path, body); if (subscriptions) { return subscriptions; } }
   { const fetches = answerFetchAccounts(method, path, body); if (fetches) { return fetches; } }
   { const senders = answerSenderAddresses(method, path, body); if (senders) { return senders; } }
   { const masks = answerMaskedAddresses(method, path, body); if (masks) { return masks; } }
   { const kept = answerRecoverable(method, path); if (kept) { return kept; } }
   { const expiring = answerConfidential(method, path); if (expiring) { return expiring; } }
   { const window = answerOffline(method, path, body); if (window) { return window; } }
   if (path === '/api/v1/me') {
      return json(200, {
         address: account, quota: { used_bytes: 5242880, limit_mb: 100 },
         vacation: { enabled: false, subject: '', message: '', expires: false, expires_date: '' },
         second_factor: false, directory_linked: false, password_changed: '2026-09-01'
      });
   }
   if (path === '/api/v1/me/folders' && method === 'GET') { return json(200, folderTree()); }
   // Web Push. The devices are state, not a constant: the whole contract is that
   // a device subscribing twice under the same endpoint keeps ONE row, and that
   // the listing never carries the endpoint or either key back.
   if (path === '/api/v1/me/push' && method === 'GET') {
      return json(200, { enabled: pushEnabled, vapid_public_key: pushEnabled ? VAPID_PUBLIC : '', max_devices: 10, subscriptions: pushDevices.map(pushRow) });
   }
   if (path === '/api/v1/me/push/subscriptions' && method === 'POST') {
      const wanted = JSON.parse(body || '{}');
      if (!pushEnabled) { return json(409, { error: 'Web Push is switched off on this server' }); }
      const keys = wanted.keys || {};
      if (!wanted.endpoint || !keys.p256dh || !keys.auth) { return json(400, { error: 'a key is missing' }); }
      let device = pushDevices.find((d) => d.endpoint === wanted.endpoint);
      if (!device) {
         device = { id: 40 + pushDevices.length, endpoint: wanted.endpoint, created: 1789000000, last_sent: 0, state: 'active', failures: 0, last_detail: '' };
         pushDevices.push(device);
      }
      device.p256dh = keys.p256dh;
      device.auth = keys.auth;
      device.device = wanted.device || '';
      device.detail = wanted.detail === undefined ? 1 : Number(wanted.detail);
      device.senders = wanted.senders || '';
      device.labels = wanted.labels || '';
      device.quiet_from = wanted.quiet_from === undefined ? -1 : Number(wanted.quiet_from);
      device.quiet_to = wanted.quiet_to === undefined ? -1 : Number(wanted.quiet_to);
      device.quiet_offset = Number(wanted.quiet_offset) || 0;
      return json(201, pushRow(device));
   }
   if (/^\/api\/v1\/me\/push\/subscriptions\/\d+$/.test(path) && method === 'PUT') {
      const id = Number(path.split('/').pop());
      const device = pushDevices.find((d) => d.id === id);
      if (!device) { return json(404, { error: 'no such device of this account' }); }
      const wanted = JSON.parse(body || '{}');
      ['detail', 'quiet_from', 'quiet_to', 'quiet_offset'].forEach((k) => { if (wanted[k] !== undefined) { device[k] = Number(wanted[k]); } });
      ['senders', 'labels'].forEach((k) => { if (wanted[k] !== undefined) { device[k] = String(wanted[k]); } });
      return json(200, pushRow(device));
   }
   if (/^\/api\/v1\/me\/push\/subscriptions\/\d+$/.test(path) && method === 'DELETE') {
      const id = Number(path.split('/').pop());
      const at = pushDevices.findIndex((d) => d.id === id);
      if (at < 0) { return json(404, { error: 'no such device of this account' }); }
      pushDevices.splice(at, 1);
      return json(200, { removed: true });
   }
   if (path === '/api/v1/me/push/test' && method === 'POST') {
      const wanted = JSON.parse(body || '{}');
      return pushDevices.some((d) => d.id === Number(wanted.id)) ? json(200, { sent: true }) : json(404, { error: 'no such device of this account' });
   }
   if (path === '/api/v1/me/quarantine') { return json(200, { enabled: true, messages: [] }); }
   // The clients that have signed in as this person: a phone on an app password
   // and this browser. Two rows, because a client is the protocol, the address
   // and the program.
   if (path === '/api/v1/me/devices' && method === 'GET') {
      return json(200, { devices: [
         { id: '0123456789abcdef0123456789abcdef', protocol: 'imap', address: '203.0.113.9', agent: '',
           credential: 'app_password', credential_name: 'Phone', first_seen: '2026-09-01 08:00:00',
           last_seen: '2026-09-16 07:30:00', sign_ins: 412, revoked: false, revoked_at: '', current: false },
         { id: 'fedcba9876543210fedcba9876543210', protocol: 'web', address: '198.51.100.4',
           agent: 'Mozilla/5.0', credential: 'password', credential_name: '', first_seen: '2026-09-14 11:02:00',
           last_seen: '2026-09-16 09:15:00', sign_ins: 6, revoked: false, revoked_at: '', current: true }
      ] });
   }
   if (/^\/api\/v1\/me\/devices\//.test(path) && method === 'DELETE') {
      return json(200, { revoked: true, forgotten: false, sessions_ended: 1, connections_dropped: 2,
         credential_disabled: path.indexOf('credential=revoke') >= 0 });
   }
   // The reader's own safe and blocked senders. Kept as state, not a constant:
   // the page's whole contract is that a POST of a value already on the list
   // rewrites that row rather than adding a second, and a constant could not
   // show the difference.
   if (path === '/api/v1/me/sender-list' && method === 'GET') { return json(200, { scope: 'account', items: senderList.slice() }); }
   if (path === '/api/v1/me/sender-list' && method === 'POST') {
      const wanted = JSON.parse(body || '{}');
      const value = String(wanted.value || '').trim().toLowerCase().replace(/^@/, '');
      const existing = senderList.filter((e) => e.value === value)[0];
      if (existing) {
         existing.blocked = !!wanted.blocked;
         existing.reason = wanted.reason || '';
         return json(200, existing);
      }
      const entry = { id: 500 + senderList.length, scope: 'account', value: value, blocked: !!wanted.blocked, reason: wanted.reason || '', expires: false, expired: false };
      senderList.push(entry);
      return json(201, entry);
   }
   if (/^\/api\/v1\/me\/sender-list\/\d+$/.test(path) && method === 'PUT') {
      const id = Number(path.split('/').pop());
      const wanted = JSON.parse(body || '{}');
      const entry = senderList.filter((e) => e.id === id)[0];
      if (!entry) { return json(404, { error: 'no such entry' }); }
      if (wanted.blocked !== undefined) { entry.blocked = !!wanted.blocked; }
      return json(200, entry);
   }
   if (/^\/api\/v1\/me\/sender-list\/\d+$/.test(path) && method === 'DELETE') {
      const id = Number(path.split('/').pop());
      senderList = senderList.filter((e) => e.id !== id);
      return json(200, { deleted: true });
   }
   if (path === '/api/v1/me/quarantine-digest' && method === 'GET') { return json(200, digestState); }
   if (path === '/api/v1/me/quarantine-digest' && method === 'PUT') {
      const wanted = JSON.parse(body || '{}');
      digestState.enabled = digestState.available && !!wanted.enabled;
      return json(200, digestState);
   }
   if (path === '/api/v1/me/settings' && method === 'GET') {
      return json(200, { name: { first: 'A', last: 'B' }, forwarding: { enabled: false, address: '', keep_original: true }, signature: { enabled: false, text: '' } });
   }
   if (/^\/api\/v1\/me\/contacts\/\d+$/.test(path) && method === 'DELETE') { return json(200, { deleted: true }); }
   if (/^\/api\/v1\/me\/contacts\/\d+$/.test(path) && method === 'PUT') {
      const changed = JSON.parse(body || '{}');
      return json(200, { id: Number(path.split('/')[5]), name: 'Alan Hidden', address: 'al@example.org', source: changed.source || 'collected', birthday: '', note: '', phones: [], has_photo: false });
   }
   if (path === '/api/v1/me/contacts' && method === 'POST') {
      const added = JSON.parse(body || '{}');
      return json(201, { id: 100 + requests.length, name: added.name || '', address: added.address || '', source: 'manual' });
   }
   if (path.startsWith('/api/v1/me/contacts?') && method === 'GET') {
      // The address book, narrowed the way the server narrows it: names and
      // addresses containing the text.
      const q = decodeURIComponent((path.split('q=')[1] || '').split('&')[0]).toLowerCase();
      const book = [
         { id: 1, name: 'Alice Example', address: 'alice@example.net', source: 'manual', birthday: '', note: '', phones: [], has_photo: false },
         { id: 2, name: 'Alan Hidden', address: 'al@example.org', source: 'collected', birthday: '', note: '', phones: [], has_photo: false },
         { id: 3, name: 'Carol Card', address: 'carol@example.org', source: 'manual', birthday: '1985-04-12', note: 'Met at the conference.', phones: ['+1 555 0100'], has_photo: true,
           card: 'BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Carol Card\r\nN:Card;Carol;;;\r\nEMAIL;TYPE=INTERNET:carol@example.org\r\nTEL:+1 555 0100\r\nBDAY:1985-04-12\r\nNOTE:Met at the conference.\r\nEND:VCARD\r\n' }
      ];
      const withCards = path.indexOf('fields=card') >= 0;
      const found = book.filter((c) => c.name.toLowerCase().indexOf(q) >= 0 || c.address.indexOf(q) >= 0)
         .map((c) => { const copy = Object.assign({}, c); if (!withCards) { delete copy.card; } return copy; });
      return json(200, { contacts: found, count: found.length, total: book.length });
   }
   if (path === '/api/v1/me/filters' && method === 'GET') { return json(200, { active: '' }); }
   if (path === '/api/v1/me/filters' && method === 'PUT') { return json(200, { saved: true }); }
   if (path === '/api/v1/me/preferences' && method === 'GET') { return json(200, { preferences: prefsStore }); }
   if (path === '/api/v1/me/preferences' && method === 'PUT') { Object.assign(prefsStore, JSON.parse(body || '{}')); return json(200, { preferences: prefsStore }); }
   if (path.startsWith('/api/v1/me/folders/1/messages')) {
      if (path.indexOf('before_uid=') >= 0) { return json(200, { total: arrived ? 6 : 5, messages: OLDER.map(withState) }); }
      if (!arrived) { return json(200, { total: LISTING.total, messages: LISTING.messages.map(withState) }); }
      return json(200, { total: 6, messages: [ARRIVAL].concat(LISTING.messages).map(withState) });
   }
   if (path.startsWith('/api/v1/me/folders/2/messages')) { return json(200, { total: SENT_LISTING.length, messages: SENT_LISTING.map(withState) }); }
   if (path.startsWith('/api/v1/me/folders/3/messages')) { return json(200, { total: WORK_THREAD.length, messages: WORK_THREAD.map((m) => withState(Object.assign({}, m, { text: undefined }))) }); }
   if (path.startsWith('/api/v1/me/search?')) {
      // Every folder, for the three queries the page makes of it: the Starred
      // view's, the follow-up reminder's, and the Sent folder's nudges asking
      // what answered its messages (in_reply_to: terms joined by OR).
      const q = decodeURIComponent((path.split('q=')[1] || '').split('&')[0]);
      const all = LISTING.messages.concat(arrived ? [ARRIVAL] : [], OLDER, SENT_LISTING).map(withState);
      const asked = q.split(' OR ').filter((t) => t.startsWith('in_reply_to:')).map((t) => t.slice('in_reply_to:'.length).replace(/[<>]/g, '').toLowerCase());
      const answers = (m) => ((m.in_reply_to || '') + ' ' + (m.references || '')).toLowerCase().split(/\s+/).map((r) => r.replace(/[<>]/g, '')).some((r) => r && asked.indexOf(r) >= 0);
      const hits = asked.length ? all.filter(answers)
         : q === 'is:flagged' ? all.filter((m) => m.flags.flagged)
         : q === 'label:$FollowUp' ? all.filter((m) => m.flags.keywords.some((k) => k.toLowerCase() === '$followup')) : [];
      return json(200, { query: q, scanned: all.length, complete: true, more: false, messages: hits.map((m) => Object.assign({ folder_id: 1, folder: 'INBOX' }, m)) });
   }
   if (/^\/api\/v1\/me\/folders\/\d+\/messages/.test(path)) { return json(200, { total: 0, messages: [] }); }
   if (/^\/api\/v1\/me\/messages\/102\/attachments\/(\d+)$/.test(path)) {
      const index = Number(path.split('/').pop());
      // The download route's own content type, which is what decides whether
      // the page may show it: an SVG comes back as a file, on purpose.
      const types = { 0: 'text/plain', 1: 'image/png', 2: 'application/octet-stream' };
      return { status: 200, body: 'PNGBYTES', headers: { 'Content-Type': types[index] || 'application/octet-stream' } };
   }
   if (path === '/api/v1/me/messages/102' && method === 'GET') { return json(200, withState(JSON.parse(JSON.stringify(WITH_IMAGE)))); }
   if (path === '/api/v1/me/messages/104' && method === 'GET') {
      return json(200, withState(Object.assign({}, ARRIVAL, { folder_id: 1, received: ARRIVAL.date + ':00', cc: '', text: 'just in', html: '', attachments: [], message_id: '<four@example.net>', references: '', in_reply_to: '' })));
   }
   // The HTML document the server renders for the message, which the offline store keeps.
   if (path.startsWith('/api/v1/me/messages/102/html') && method === 'GET') {
      return { status: 200, body: '<!doctype html><html><head><meta charset="utf-8"></head><body><p>Hello</p></body></html>', headers: { 'Content-Type': 'text/html; charset=utf-8' } };
   }
   if (/^\/api\/v1\/me\/messages\/105\/attachments\/(\d+)$/.test(path)) {
      const types = { 0: 'text/plain', 1: 'image/png', 2: 'image/png', 3: 'image/jpeg' };
      return { status: 200, body: 'FILEBYTES', headers: { 'Content-Type': types[Number(path.split('/').pop())] || 'application/octet-stream' } };
   }
   if (path === '/api/v1/me/messages/105' && method === 'GET') { return json(200, withState(JSON.parse(JSON.stringify(DRAFT_WITH_LOGO)))); }
   if (path === '/api/v1/me/smime' && method === 'GET') {
      const store = smimeStores[account] || { own: [], recipients: [] };
      return json(200, { own: store.own, recipients: store.recipients, limits: { own: 20, recipients: 500 } });
   }
   if (path === '/api/v1/me/smime/own' && method === 'PUT') {
      const store = smimeStores[account] || (smimeStores[account] = { own: [], recipients: [] });
      const wanted = JSON.parse(body || '{}');
      const at = store.own.findIndex((o) => o.fingerprint === wanted.fingerprint);
      const row = Object.assign({ id: 1, created: 1700000000 }, wanted);
      if (at >= 0) { store.own[at] = row; return json(200, row); }
      store.own.push(row);
      return json(201, row);
   }
   if (path === '/api/v1/me/messages/601' && method === 'GET') { return json(200, withState(JSON.parse(JSON.stringify(SEALED)))); }
   if (path === '/api/v1/me/messages/601/source' && method === 'GET') {
      return { status: 200, headers: { 'Content-Type': 'message/rfc822' }, body: 'Content-Type: application/pkcs7-mime\r\n\r\nAAECAwQ=\r\n' };
   }
   if (/^\/api\/v1\/me\/messages\/5\d\d$/.test(path) && method === 'GET') {
      const one = WORK_THREAD.filter((m) => m.id === Number(path.split('/').pop()))[0];
      if (one) { return json(200, withState(Object.assign({ folder_id: 3, cc: '', html: '', attachments: [] }, one))); }
   }
   // A deletion moves the message to Trash and says where it went, as the server does.
   if (/^\/api\/v1\/me\/messages\/\d+$/.test(path) && method === 'DELETE') { return json(200, { id: 900 + Number(path.split('/').pop()), folder_id: 6, deleted: false }); }
   if (/^\/api\/v1\/me\/messages\/\d+$/.test(path) && method === 'GET') {
      return json(200, withState(Object.assign(JSON.parse(JSON.stringify(WITH_IMAGE)), { id: Number(path.split('/').pop()), html: '', text: 'plain' })));
   }
   if (path === '/api/v1/me/messages' && method === 'POST') {
      if (sendRefusal) { const refusal = sendRefusal; sendRefusal = null; return json(400, refusal); }
      return json(201, { id: 300 });
   }
   if (path === '/api/v1/me/drafts' && method === 'POST') { return json(201, { id: 55 }); }
   if (/\/messages\/\d+\/flags$/.test(path)) {
      // The star and the keywords are kept, removed first then added, as the server does.
      const id = Number(path.split('/')[5]);
      const change = JSON.parse(body || '{}');
      if (typeof change.flagged === 'boolean') { flaggedById[id] = change.flagged; }
      let keywords = (keywordsById[id] || []).slice();
      (change.keywords_remove || []).forEach((k) => { keywords = keywords.filter((have) => have.toLowerCase() !== k.toLowerCase()); });
      (change.keywords_add || []).forEach((k) => { if (!keywords.some((have) => have.toLowerCase() === k.toLowerCase())) { keywords.push(k); } });
      keywordsById[id] = keywords;
      if (brokenFlagsAnswerOnce && isStarRequest(body)) { brokenFlagsAnswerOnce = false; return json(200, { id, folder_id: 1, flags: { seen: true, flagged: !!flaggedById[id], draft: false, keywords: 7 } }); }
      return json(200, { id, folder_id: 1, flags: { seen: true, flagged: !!flaggedById[id], draft: false, keywords: keywords.slice() } });
   }
   if (path.startsWith('/api/v1/me/changes')) {
      if (changesMissing) { return json(404, { error: 'Not found.' }); }
      if (changeAgain) { changeAgain = false; return json(200, { token: 't3', changed: true, folders: [{ id: 1, count: 4, unseen: 3 }, { id: 2, count: 1, unseen: 0 }, { id: 3, count: 0, unseen: 0 }] }); }
      changeStep += 1;
      if (changeStep === 1) { return json(200, { token: 't1', folders: [{ id: 1, count: 3, unseen: 2 }, { id: 2, count: 1, unseen: 0 }, { id: 3, count: 0, unseen: 0 }] }); }
      if (changeStep === 2) { arrived = true; return json(200, { token: 't2', changed: true, folders: [{ id: 1, count: 4, unseen: 3 }, { id: 2, count: 1, unseen: 0 }, { id: 3, count: 0, unseen: 0 }] }); }
      return json(200, { token: 't2', changed: false, folders: [{ id: 1, count: 4, unseen: 3 }, { id: 2, count: 1, unseen: 0 }, { id: 3, count: 0, unseen: 0 }] });
   }
   // The reader's language, as the server answers it: the catalogue itself.
   const wantsCatalogue = /^\/portal-lang\/([A-Za-z-]+)\.json$/.exec(path);
   if (wantsCatalogue && method === 'GET') {
      try { return { status: 200, headers: { 'Content-Type': 'application/json' }, body: fs.readFileSync(cataloguesDir + wantsCatalogue[1] + '.json', 'utf8') }; }
      catch (e) { return json(404, { error: 'No catalogue: ' + wantsCatalogue[1] }); }
   }
   if (path === '/api/v1/me/password' && method === 'POST') {
      if (passwordAnswer) { return json(passwordAnswer.status, passwordAnswer.body); }
      return json(200, { changed: true });
   }
   if (path === '/api/v1/me/folders' && method === 'POST') { return json(201, { id: 9, name: 'Bills' }); }
   if (/^\/api\/v1\/me\/folders\/\d+$/.test(path) && method === 'PUT') {
      if (renameRefusal) { return json(400, { error: renameRefusal }); }
      return json(200, { id: 3, name: 'Renamed' });
   }
   if (/^\/api\/v1\/me\/folders\/\d+$/.test(path) && method === 'DELETE') { return json(200, { deleted: true }); }
   // A move answers with the id the message has where it went, and the folder.
   if (/\/messages\/\d+\/move$/.test(path) && method === 'POST') { return json(200, { id: 202, folder_id: 4 }); }
   return json(404, { error: 'No such route: ' + path });
}

// No connection: every request is recorded and then fails the way a browser
// fails it, with a rejection and no status, which is what the page reads as 0.
let networkDown = false;
// Or the connection goes the moment this one request ('METHOD /path') has
// been answered - a laptop leaving the Wi-Fi as the page re-asks for the account.
let networkDownAfter = '';
function fetchStub(path, options) {
   const method = (options && options.method) || 'GET';
   const headers = (options && options.headers) || {};
   requests.push({ method, path, headers, body: options && options.body });
   if (networkDown) { return Promise.reject(new TypeError('Failed to fetch')); }
   const reply = answer(method, path, options && options.body);
   if (networkDownAfter && networkDownAfter === method + ' ' + path) { networkDownAfter = ''; networkDown = true; }
   return Promise.resolve({
      status: reply.status,
      ok: reply.status >= 200 && reply.status < 300,
      headers: { get: (name) => (name in reply.headers ? reply.headers[name] : null) },
      text: () => Promise.resolve(reply.body),
      arrayBuffer: () => Promise.resolve(Uint8Array.from(Buffer.from(reply.body, 'binary')).buffer),
      json: () => Promise.resolve(JSON.parse(reply.body)),
      // A real Blob, so the worker can answer it as a Response, with the base64
      // the FileReader stub reads off it.
      blob: () => Promise.resolve(Object.assign(new Blob(['ABC'], { type: reply.headers['Content-Type'] || 'application/octet-stream' }), { base64: 'QUJD' }))
   });
}

/* --------------------------------------------- the page's S/MIME module */

// window.hmSmime, as far as the page uses it. The real module is held to
// OpenSSL in both directions by build/check-portal-smime.js; what matters
// here is which keys the page hands it, so this one records them. Its
// wrapped key is its own password - {"data": "<the password>"} - so it
// unwraps with the right one and throws 'wrong password' with any other,
// which is the message the real module throws and the page reads.
const readWith = [];
const smimeStub = {
   parseCertificate: (bytes) => ({ raw: bytes, emails: [], name: 'Certificate', notAfter: new Date('2030-01-01T00:00:00Z'), isCA: false, keyType: 'rsa' }),
   b64decode: (value) => Uint8Array.from(Buffer.from(String(value), 'base64')),
   b64encode: (bytes) => Buffer.from(bytes).toString('base64'),
   fingerprint: async () => 'f'.repeat(64),
   importPrivateKey: async (pkcs8) => ({ type: 'rsa', pkcs8 }),
   wrapPrivateKey: async (pkcs8, password) => wrappedUnder(password),
   unwrapPrivateKey: async (wrapped, password) => {
      if (!wrapped || String(wrapped.data) !== String(password)) { throw new Error('wrong password'); }
      return Uint8Array.from([0x30, 0x03, 0x02, 0x01, 0x01]);
   },
   readMessage: async (bytes, unlocked) => {
      readWith.push(unlocked.map((u) => u.address).sort());
      return { layers: ['encrypted'], encrypted: { ok: false, error: 'not encrypted for a key this page holds' }, signed: null, content: null };
   }
};

/* ------------------------------------------------------------ the assertions */

const failures = [];
let checks = 0;
function check(what, ok, detail) {
   checks += 1;
   if (!ok) { failures.push(what + (detail ? ' -- ' + detail : '')); }
}
function since(n) { return requests.slice(n); }
function called(from, method, pattern) {
   return since(from).some((r) => r.method === method && (typeof pattern === 'string' ? r.path === pattern : pattern.test(r.path)));
}
const flush = async () => { for (let i = 0; i < 60; i += 1) { await new Promise((r) => setImmediate(r)); } };

/* ------------------------------------------------------------------ the run */

// Node has globals of its own with these names (navigator, localStorage,
// fetch), some of them getter-only, so each one is defined over rather than
// assigned: the script must see this file's world and nothing of node's.
// The installed app's world: the App Badging API, the protocol-handler
// registration and the Cache Storage the service worker leaves a share in,
// recorded rather than real.
const badges = [];
const handlers = [];
// The browser's half of Web Push, recorded rather than real: a permission the
// test can set, a service worker registration with a push manager, and a
// subscription whose keys are the ones RFC 8291's example uses - so what the
// page sends is checked against a key of a known length rather than a blob.
const NotificationStub = function (title, options) { notifications.push({ title, options }); };
const notifications = [];
NotificationStub.permission = 'default';
NotificationStub.requestPermission = () => Promise.resolve(NotificationStub.permission);

function bytesOf(base64url) {
   const padded = base64url.replace(/-/g, '+').replace(/_/g, '/') + '==='.slice((base64url.length + 3) % 4);
   return Uint8Array.from(Buffer.from(padded, 'base64'));
}
const SUBSCRIPTION_P256DH = 'BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4';
const SUBSCRIPTION_AUTH = 'BTBZMqHH6r4Tts7J_aSIgg';

let liveSubscription = null;
let subscribeCalls = [];
const pushManagerStub = {
   subscribe: (options) => {
      subscribeCalls.push(options);
      liveSubscription = {
         endpoint: 'https://push.example.net/push/JzLQ3raZJfFBR0aqvOMsLrt54w4rJUsV',
         options: options,
         getKey: (name) => (name === 'p256dh' ? bytesOf(SUBSCRIPTION_P256DH) : bytesOf(SUBSCRIPTION_AUTH)).buffer,
         unsubscribe: () => { liveSubscription = null; return Promise.resolve(true); }
      };
      return Promise.resolve(liveSubscription);
   },
   getSubscription: () => Promise.resolve(liveSubscription)
};
const registrationStub = { pushManager: pushManagerStub, showNotification: () => Promise.resolve() };
const serviceWorkerStub = {
   register: () => Promise.resolve(registrationStub),
   ready: Promise.resolve(registrationStub)
};
let clipboardText = '';
const navigatorStub = {
   language: 'en', languages: ['en'],
   clipboard: { writeText: (text) => { clipboardText = String(text); return Promise.resolve(); } },
   userAgent: 'Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 Chrome/130.0 Mobile Safari/537.36',
   serviceWorker: serviceWorkerStub,
   setAppBadge: (n) => { badges.push(Number(n)); return Promise.resolve(); },
   clearAppBadge: () => { badges.push(0); return Promise.resolve(); },
   registerProtocolHandler: (scheme, url) => { handlers.push({ scheme: String(scheme), url: String(url) }); }
};
const cacheStores = new Map();
const cachesStub = {
   open: async (name) => {
      if (!cacheStores.has(name)) { cacheStores.set(name, new Map()); }
      const m = cacheStores.get(name);
      // A real Cache hands back a FRESH Response each time it is matched; a body
      // can be read only once, so returning the same object twice would let a
      // second reader see an empty one - which is not what a browser does, and
      // is how the service worker's words looked like a bug that was not there.
      return { match: async (k) => { const r = m.get(k); return r && r.clone ? r.clone() : r; }, put: async (k, r) => { m.set(k, r); }, delete: async (k) => m.delete(k) };
   },
   delete: async (name) => cacheStores.delete(name)
};
// An IndexedDB, in memory: enough of the API for the page's offline store and
// the worker's reading of it - open with an upgrade, object stores by keyPath,
// put/get/delete/clear/getAll, requests answering on a microtask - and no more,
// so that the page reaching for anything else fails here by name. One map
// serves the page and the worker, as one origin's storage serves both.
const idbDatabases = new Map();
function idbRequest(run) {
   const request = { result: undefined, error: null, onsuccess: null, onerror: null };
   Promise.resolve().then(() => {
      try { request.result = run(); if (request.onsuccess) { request.onsuccess({ target: request }); } }
      catch (e) { request.error = e; if (request.onerror) { request.onerror({ target: request }); } }
   });
   return request;
}
class IdbStore {
   constructor(keyPath) { this.keyPath = keyPath; this.records = new Map(); }
   put(value) { return idbRequest(() => { const key = value[this.keyPath]; if (key === undefined) { throw new Error('no key'); } this.records.set(key, value); return key; }); }
   get(key) { return idbRequest(() => this.records.get(key)); }
   delete(key) { return idbRequest(() => { this.records.delete(key); return undefined; }); }
   clear() { return idbRequest(() => { this.records.clear(); return undefined; }); }
   getAll() { return idbRequest(() => Array.from(this.records.values())); }
   getAllKeys() { return idbRequest(() => Array.from(this.records.keys())); }
   count() { return idbRequest(() => this.records.size); }
}
class IdbDatabase {
   constructor(name) { this.name = name; this.version = 0; this.stores = new Map(); this.closed = false; }
   get objectStoreNames() { const names = Array.from(this.stores.keys()); return { contains: (n) => names.indexOf(n) >= 0, length: names.length }; }
   createObjectStore(name, options) { const store = new IdbStore(options && options.keyPath); this.stores.set(name, store); return store; }
   transaction(names, mode) {
      const db = this;
      const wanted = Array.isArray(names) ? names : [names];
      wanted.forEach((n) => { if (!db.stores.has(n)) { throw new Error('no such store: ' + n); } });
      return { mode: mode || 'readonly', objectStore: (n) => { if (wanted.indexOf(n) < 0) { throw new Error('store not in transaction: ' + n); } return db.stores.get(n); } };
   }
   close() { this.closed = true; }
}
const indexedDBStub = {
   open: (name, version) => {
      const request = { result: undefined, error: null, onsuccess: null, onerror: null, onupgradeneeded: null, onblocked: null };
      Promise.resolve().then(() => {
         let db = idbDatabases.get(name);
         const fresh = !db;
         if (fresh) { db = new IdbDatabase(name); idbDatabases.set(name, db); }
         request.result = db;
         if (fresh || (version && version > db.version)) { db.version = version || 1; if (request.onupgradeneeded) { request.onupgradeneeded({ target: request }); } }
         if (request.onsuccess) { request.onsuccess({ target: request }); }
      });
      return request;
   },
   deleteDatabase: (name) => idbRequest(() => { idbDatabases.delete(name); return undefined; })
};
const idbStore = (name) => { const db = idbDatabases.get('hmPortalOffline'); return db && db.stores.get(name) ? db.stores.get(name).records : new Map(); };
// What the browser says it allows this site, as navigator.storage.estimate answers it.
let storageEstimate = { usage: 5 * 1024 * 1024, quota: 2 * 1024 * 1024 * 1024 };
navigatorStub.storage = { estimate: () => Promise.resolve(Object.assign({}, storageEstimate)) };
const world = {
   navigator: navigatorStub, caches: cachesStub, Notification: NotificationStub, indexedDB: indexedDBStub,
   document, window, location, history, localStorage,
   fetch: fetchStub,
   confirm: window.confirm,
   DOMParser: DOMParserStub,
   FileReader: FileReaderStub,
   setTimeout: (fn, ms) => { const id = timerId++; timers.push({ id, fn, ms }); return id; },
   clearTimeout: (id) => { const at = timers.findIndex((t) => t.id === id); if (at >= 0) { timers.splice(at, 1); } },
   setInterval: (fn, ms) => { const id = timerId++; intervals.push({ id, fn, ms }); return id; },
   clearInterval: (id) => { const at = intervals.findIndex((t) => t.id === id); if (at >= 0) { intervals.splice(at, 1); } }
};
Object.keys(world).forEach((name) => {
   Object.defineProperty(globalThis, name, { value: world[name], writable: true, configurable: true });
});

async function main() {
   // A reader who was on the settings page when the session lapsed: the page is
   // loaded at that address, signed out.
   location.hash = '#/settings';
   await flush();

   vm.runInThisContext(fs.readFileSync(scriptPath, 'utf8'), { filename: 'portal.js' });
   await flush();

   const gate = document.getElementById('signin');
   const app = document.getElementById('account');
   check('a page opened without a session shows the sign-in form', gate.hidden === false && app.hidden === true,
      'signin.hidden=' + gate.hidden + ' account.hidden=' + app.hidden);
   check('nothing but /api/v1/me was asked for before a session existed',
      requests.every((r) => r.path === '/api/v1/me'), JSON.stringify(requests.map((r) => r.path)));

   // ---- sign in
   const before = requests.length;
   document.getElementById('address').value = 'user@example.com';
   document.getElementById('password').value = 'a-real-password';
   document.getElementById('signin-form').dispatchEvent(makeEvent('submit'));
   await flush();

   const withAuth = requests.filter((r) => r.headers && r.headers.Authorization);
   check('the password is sent exactly once', withAuth.length === 1, withAuth.length + ' requests carried Authorization');
   check('and only to start a session', withAuth.length === 1 && withAuth[0].method === 'POST' && withAuth[0].path === '/api/v1/session',
      withAuth.length ? withAuth[0].method + ' ' + withAuth[0].path : 'none');
   check('the password field is emptied', document.getElementById('password').value === '');
   check('nothing secret is stored in the browser',
      [...store.values()].every((v) => v.indexOf('a-real-password') < 0) && [...store.keys()].every((k) => !/pass|token|secret|auth/i.test(k)),
      JSON.stringify([...store.entries()]));
   check('every write carries the header the server demands',
      requests.filter((r) => r.method !== 'GET' && r.path !== '/api/v1/session').every((r) => r.headers['X-Requested-With'] === 'hMailServer'));
   check('the account is shown', app.hidden === false && gate.hidden === true);
   check('the folder tree was read', called(before, 'GET', '/api/v1/me/folders'));

   // ---- a follow-up due today is announced once at sign-in, and the notice opens the message
   const toastBox = document.getElementById('toasts');
   check('the follow-ups are asked for once at sign-in', since(before).filter((r) => r.method === 'GET' && r.path === '/api/v1/me/search?q=label%3A%24FollowUp&limit=200').length === 1,
      JSON.stringify(since(before).map((r) => r.path)));
   check('and the one due today is announced, with a way to open it', toastBox.textContent.indexOf('Due for follow-up: First') >= 0 && toastBox.textContent.indexOf('Open') >= 0, toastBox.textContent);
   check('which was announced is kept for the day, in this browser', (() => { try { const v = JSON.parse(store.get('hmPortalDueShown')); return v.day === todayStamp && v.ids.length === 1 && v.ids[0] === 101; } catch (e) { return false; } })(),
      String(store.get('hmPortalDueShown')));
   // Dismissed for now; what Open does is checked at the end, on a second sign-in.
   { const buttons = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.tagName === 'BUTTON' && c.getAttribute('aria-label') === 'Dismiss') { buttons.push(c); } if (c.childNodes) { walk(c); } } })(toastBox); buttons.forEach((b) => b.dispatchEvent(makeEvent('click'))); }
   check('and the notice can be dismissed', toastBox.children.length === 0 && location.hash === '#/settings', toastBox.children.length + ' toasts, ' + location.hash);

   // ---- the address the reader arrived at is the one that is restored
   check('a reload lands where the reader was', document.getElementById('settings-section').hidden === false,
      'hash=' + location.hash + ' title=' + document.getElementById('view-title').textContent);
   check('the sign-in did not lose the address', location.hash === '#/settings', location.hash);
   check('the settings page offers everything the server holds as one download, at the reader\'s own export route',
      document.getElementById('export-everything').getAttribute('href') === '/api/v1/me/export.zip' && document.getElementById('export-everything').getAttribute('download') === 'my-mail.zip',
      document.getElementById('export-everything') ? document.getElementById('export-everything').getAttribute('href') : 'no link');

   // ---- the app shell: folders in the sidebar, with their unseen counts
   const nav = document.getElementById('folder-nav');
   // The navigation carries the folders, a Starred view and a heading; the folders are the /f/ entries.
   const folderRoutes = [];
   for (let i = 0; i < nav.children.length; i++) {
      const route = nav.children[i].getAttribute && nav.children[i].getAttribute('data-route');
      if (route && route.indexOf('/f/') === 0) { folderRoutes.push(route); }
   }
   check('every folder is in the sidebar', folderRoutes.length === 3, folderRoutes.length + ' entries');
   check('the inbox is first', folderRoutes[0] === '/f/1', folderRoutes[0]);
   check('the inbox\'s subfolder is listed with the folders', folderRoutes.indexOf('/f/3') > 0, folderRoutes.join(' '));
   check('the Starred view is in the sidebar', nav.children[1].getAttribute('data-route') === '/starred', nav.children[1].getAttribute('data-route'));
   check('the unseen count is shown', nav.children[0].children[2].textContent === '2', nav.children[0].children[2].textContent);

   // ---- a folder has an address, and clicking one goes to it
   nav.children[0].dispatchEvent(makeEvent('click'));
   await flush();
   check('a folder is an address', location.hash === '#/f/1', location.hash);
   check('the folder listing renders one row per message', document.getElementById('message-list').children.length === 3,
      document.getElementById('message-list').children.length + ' rows');
   check('an unseen message is marked', document.getElementById('message-list').children[0].className.indexOf('unseen') >= 0,
      document.getElementById('message-list').children[0].className);
   const rowNames = (row) => { const out = []; for (let i = 0; i < row.childNodes.length; i++) { const c = row.childNodes[i]; if (c.className === 'who') { out.push(c.textContent); } } return out; };
   check('a row names its sender', rowNames(document.getElementById('message-list').children[1]).length === 1 && rowNames(document.getElementById('message-list').children[1])[0].length > 0,
      JSON.stringify(rowNames(document.getElementById('message-list').children[1])));
   check('the view is titled by the folder', document.getElementById('view-title').textContent === 'Inbox',
      document.getElementById('view-title').textContent);

   // ---- the keyboard moves a cursor
   const rows = () => document.getElementById('message-list').children;
   const onCursor = () => { const list = rows(); let at = -1; for (let i = 0; i < list.length; i += 1) { if (list[i].className.indexOf('msg-cursor') >= 0) { at = i; } } return at; };
   check('the cursor starts on the first row', onCursor() === 0, 'at ' + onCursor());
   document.dispatchEvent(makeEvent('keydown', { key: 'j', target: document.body }));
   check('j moves the cursor down', onCursor() === 1, 'at ' + onCursor());
   document.dispatchEvent(makeEvent('keydown', { key: 'ArrowDown', target: document.body }));
   check('the arrow keys move it too', onCursor() === 2, 'at ' + onCursor());
   document.dispatchEvent(makeEvent('keydown', { key: 'k', target: document.body }));
   check('k moves it back up', onCursor() === 1, 'at ' + onCursor());
   document.dispatchEvent(makeEvent('keydown', { key: 'x', target: document.body }));
   check('x ticks the box under the cursor', document.getElementById('bulk-bar').hidden === false &&
      document.getElementById('bulk-count').textContent === '1 selected', document.getElementById('bulk-count').textContent);
   document.dispatchEvent(makeEvent('keydown', { key: 'x', target: document.body }));
   check('and unticks it', document.getElementById('bulk-bar').hidden === true);
   const typing = makeEvent('keydown', { key: 'j', target: document.getElementById('compose-text') });
   document.dispatchEvent(typing);
   check('a keystroke in a text box is not a shortcut', onCursor() === 1 && typing.defaultPrevented === false, 'at ' + onCursor());

   // ---- e archives the row under the cursor, and the toast's Undo moves it back
   const beforeArchive = requests.length;
   const timersBefore = timers.length;
   document.dispatchEvent(makeEvent('keydown', { key: 'e', target: document.body }));
   await flush();
   await flush();
   const archiveCall = since(beforeArchive).filter((r) => r.method === 'POST' && /\/messages\/102\/move$/.test(r.path))[0];
   check('e archives the message under the cursor', !!archiveCall && JSON.parse(archiveCall.body).to === 'archive', archiveCall ? archiveCall.body : 'no move');
   const toasts = document.getElementById('toasts');
   const clickUndo = () => { const buttons = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.tagName === 'BUTTON' && c.textContent === 'Undo') { buttons.push(c); } if (c.childNodes) { walk(c); } } })(toasts); if (!buttons.length) { throw new Error('no Undo button in the toast'); } buttons[0].dispatchEvent(makeEvent('click')); };
   const dismissAllToasts = () => { const buttons = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.tagName === 'BUTTON' && c.getAttribute('aria-label') === 'Dismiss') { buttons.push(c); } if (c.childNodes) { walk(c); } } })(toasts); buttons.forEach((b) => b.dispatchEvent(makeEvent('click'))); };
   check('a toast says so and offers Undo', toasts.children.length === 1 && toasts.textContent.indexOf('Archived') >= 0 && toasts.textContent.indexOf('Undo') >= 0,
      toasts.textContent);
   const undoButtons = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.tagName === 'BUTTON' && c.textContent === 'Undo') { undoButtons.push(c); } walk(c); } })(toasts);
   const beforeUndo = requests.length;
   if (undoButtons.length) { undoButtons[0].dispatchEvent(makeEvent('click')); }
   await flush();
   await flush();
   const undoCall = since(beforeUndo).filter((r) => r.method === 'POST' && /\/messages\/202\/move$/.test(r.path))[0];
   check('Undo moves it back where it was, by the id the move gave it', !!undoCall && JSON.parse(undoCall.body).folder_id === 1, undoCall ? undoCall.body : 'no move back');
   check('and the toast is gone with its timer', toasts.children.length === 0 && timers.length === timersBefore, toasts.children.length + ' toasts, ' + timers.length + ' timers (' + timersBefore + ' before)');
   check('the cursor is still on a row', onCursor() >= 0, 'at ' + onCursor());

   // ---- the star toggles the flag in place
   const starOf = (row) => { for (let i = 0; i < row.childNodes.length; i++) { if (row.childNodes[i].className && row.childNodes[i].className.indexOf('star') === 0) { return row.childNodes[i]; } } return null; };
   const beforeStar = requests.length;
   starOf(rows()[0]).dispatchEvent(makeEvent('click'));
   await flush();
   const starCall = since(beforeStar).filter((r) => r.method === 'PUT' && /\/flags$/.test(r.path))[0];
   check('the star sets the flag', !!starCall && JSON.parse(starCall.body).flagged === true, starCall ? starCall.body : 'no flags call');
   check('and shows it', starOf(rows()[0]).className.indexOf('on') >= 0, starOf(rows()[0]).className);

   // ---- the select menu picks by state
   document.getElementById('select-menu-btn').dispatchEvent(makeEvent('click'));
   check('the select menu opens', document.getElementById('select-menu').hidden === false);
   const picks = document.getElementById('select-menu').children;
   let unreadPick = null; for (let i = 0; i < picks.length; i++) { if (picks[i].getAttribute('data-pick') === 'unread') { unreadPick = picks[i]; } }
   unreadPick.dispatchEvent(makeEvent('click', { target: unreadPick }));
   check('Unread ticks the unread rows', document.getElementById('bulk-count').textContent === '2 selected', document.getElementById('bulk-count').textContent);
   document.getElementById('bulk-clear').dispatchEvent(makeEvent('click'));
   check('Clear unticks them', document.getElementById('bulk-bar').hidden === true);
   document.dispatchEvent(makeEvent('keydown', { key: 'j', target: document.body }));
   while (onCursor() < 1) { document.dispatchEvent(makeEvent('keydown', { key: 'j', target: document.body })); }
   while (onCursor() > 1) { document.dispatchEvent(makeEvent('keydown', { key: 'k', target: document.body })); }

   // ---- a right-click menu on a row: pin, and Escape closes it
   rows()[1].dispatchEvent(makeEvent('contextmenu', { clientX: 300, clientY: 200 }));
   check('a right-click opens the row menu', document.getElementById('context-menu').hidden === false);
   const menuItems = document.getElementById('context-menu').children;
   let pinItem = null; for (let i = 0; i < menuItems.length; i++) { if (menuItems[i].getAttribute && menuItems[i].getAttribute('data-act') === 'pin') { pinItem = menuItems[i]; } }
   check('and offers to pin it', !!pinItem && pinItem.textContent === 'Pin', pinItem ? pinItem.textContent : 'no pin item');
   const beforePin = requests.length;
   pinItem.dispatchEvent(makeEvent('click', { target: pinItem }));
   await flush();
   await flush();
   const pinCall = since(beforePin).filter((r) => r.method === 'PUT' && /\/messages\/102\/flags$/.test(r.path))[0];
   check('Pin adds the pin keyword to the message', !!pinCall && JSON.stringify(JSON.parse(pinCall.body).keywords_add) === '["$Pinned"]', pinCall ? pinCall.body : 'no flags call');
   check('and the menu is gone', document.getElementById('context-menu').hidden === true);
   // The pin is honoured: the pinned message now heads the list, and Unpin puts it back.
   const menuItem = (act) => { const menu = document.getElementById('context-menu'); for (let i = 0; i < menu.children.length; i++) { if (menu.children[i].getAttribute('data-act') === act) { return menu.children[i]; } } return null; };
   check('the pinned message heads the list', rows()[0].textContent.indexOf('Second') >= 0 && rows()[0].className.indexOf('pinned') >= 0, rows()[0].className + ' ' + rows()[0].textContent.slice(0, 30));
   rows()[0].dispatchEvent(makeEvent('contextmenu', { clientX: 300, clientY: 200 }));
   check('and its row menu offers Unpin', !!menuItem('pin') && menuItem('pin').textContent === 'Unpin', menuItem('pin') ? menuItem('pin').textContent : 'no pin item');
   const beforeUnpin = requests.length;
   menuItem('pin').dispatchEvent(makeEvent('click', { target: menuItem('pin') }));
   await flush();
   await flush();
   const unpinCall = since(beforeUnpin).filter((r) => r.method === 'PUT' && /\/messages\/102\/flags$/.test(r.path))[0];
   check('Unpin takes the keyword off and the row goes back to its place', !!unpinCall && JSON.stringify(JSON.parse(unpinCall.body).keywords_remove) === '["$Pinned"]' && rows()[1].textContent.indexOf('Second') >= 0 && rows()[0].className.indexOf('pinned') < 0,
      (unpinCall ? unpinCall.body : 'no flags call') + ' ' + rows().map((r) => r.textContent.slice(0, 20)).join(' | '));
   dismissAllToasts();
   rows()[1].dispatchEvent(makeEvent('contextmenu', { clientX: 300, clientY: 200 }));
   document.dispatchEvent(makeEvent('keydown', { key: 'Escape', target: document.body }));
   check('Escape closes the row menu', document.getElementById('context-menu').hidden === true);

   // ---- the inbox in tabs: every message here is primary, so one tab holds them all
   const tabBar = () => document.getElementById('inbox-tabs');
   const contextItems = (attribute, value) => { const out = []; const walk = (n) => { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.getAttribute && c.getAttribute(attribute) === value) { out.push(c); } if (c.childNodes) { walk(c); } } }; walk(document.getElementById('context-menu')); return out; };
   check('the inbox shows its tabs', tabBar().hidden === false && tabBar().children.length === 5, tabBar().hidden + ' ' + tabBar().children.length);
   check('Primary is the open tab', tabBar().children[0].className.indexOf('on') >= 0 && tabBar().children[0].textContent.indexOf('Primary') === 0, tabBar().children[0].textContent);
   check('and carries the unread count', tabBar().children[0].textContent === 'Primary2', tabBar().children[0].textContent);
   rows()[1].dispatchEvent(makeEvent('contextmenu', { clientX: 300, clientY: 200 }));
   const tabItems = contextItems('data-act', 'tab');
   const socialItem = tabItems.filter((b) => b.getAttribute('data-tab') === 'social')[0];
   check('the row menu offers the other four tabs', tabItems.length === 4 && !!socialItem, tabItems.length + ' items');
   const beforeTab = requests.length;
   socialItem.dispatchEvent(makeEvent('click', { target: socialItem }));
   await flush();
   await flush();
   const prefCall = since(beforeTab).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/preferences')[0];
   check('the sender\'s tab is kept with the account\'s preferences', !!prefCall && JSON.parse(JSON.parse(prefCall.body).tabs_by_sender)['b@example.net'] === 'social', prefCall ? prefCall.body : 'no preferences call');
   check('the row leaves Primary', rows().length === 2, rows().length + ' rows');
   check('and Social counts it', tabBar().children[1].textContent === 'Social1', tabBar().children[1].textContent);
   check('the toast offers Undo', toasts.textContent.indexOf('b@example.net') >= 0 && toasts.textContent.indexOf('Undo') >= 0, toasts.textContent);
   tabBar().children[1].dispatchEvent(makeEvent('click'));
   check('the Social tab shows it', rows().length === 1 && rows()[0].textContent.indexOf('Second') >= 0, rows().length + ' rows');
   rows()[0].dispatchEvent(makeEvent('contextmenu', { clientX: 300, clientY: 200 }));
   const primaryItem = contextItems('data-act', 'tab').filter((b) => b.getAttribute('data-tab') === 'primary')[0];
   primaryItem.dispatchEvent(makeEvent('click', { target: primaryItem }));
   await flush();
   await flush();
   check('sent back, the tab is empty and says so', rows().length === 1 && rows()[0].textContent.indexOf('Nothing under Social') >= 0, rows()[0].textContent);
   tabBar().children[0].dispatchEvent(makeEvent('click'));
   check('and Primary has its three rows again', rows().length === 3, rows().length + ' rows');
   dismissAllToasts();
   while (onCursor() < 1) { document.dispatchEvent(makeEvent('keydown', { key: 'j', target: document.body })); }
   while (onCursor() > 1) { document.dispatchEvent(makeEvent('keydown', { key: 'k', target: document.body })); }

   // ---- mute: the conversation is archived, its replies will be filed by a rule, and Undo takes it all back
   rows()[1].dispatchEvent(makeEvent('contextmenu', { clientX: 300, clientY: 200 }));
   const muteItem = contextItems('data-act', 'mute')[0];
   check('the row menu offers Mute', !!muteItem && muteItem.textContent === 'Mute', muteItem ? muteItem.textContent : 'no mute item');
   const beforeMute = requests.length;
   muteItem.dispatchEvent(makeEvent('click', { target: muteItem }));
   await flush();
   await flush();
   await flush();
   const muteFlag = since(beforeMute).filter((r) => r.method === 'PUT' && /\/messages\/102\/flags$/.test(r.path))[0];
   check('Mute marks the message', !!muteFlag && JSON.stringify(JSON.parse(muteFlag.body).keywords_add) === '["$Muted"]', muteFlag ? muteFlag.body : 'no flags call');
   check('after reading the rules the server has', called(beforeMute, 'GET', '/api/v1/me/filters'));
   const muteRule = since(beforeMute).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/filters')[0];
   const muteScript = muteRule ? JSON.parse(muteRule.body).script : '';
   check('and writes a rule that files its replies into the archive folder',
      muteScript.indexOf('header :contains ["references", "in-reply-to"] "<one@example.net>"') >= 0 && muteScript.indexOf('fileinto "Archive"') >= 0, muteScript.slice(0, 300));
   const muteMove = since(beforeMute).filter((r) => r.method === 'POST' && /\/messages\/102\/move$/.test(r.path))[0];
   check('and archives the conversation', !!muteMove && JSON.parse(muteMove.body).to === 'archive', muteMove ? muteMove.body : 'no move');
   check('the toast says so and offers Undo', toasts.textContent.indexOf('Muted') >= 0 && toasts.textContent.indexOf('Undo') >= 0, toasts.textContent);
   const beforeUnmute = requests.length;
   clickUndo();
   await flush();
   await flush();
   await flush();
   const rulesBack = since(beforeUnmute).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/filters')[0];
   check('Undo takes the rule away', !!rulesBack && JSON.parse(rulesBack.body).script === '', rulesBack ? rulesBack.body : 'no filters call');
   const moveBack = since(beforeUnmute).filter((r) => r.method === 'POST' && /\/messages\/202\/move$/.test(r.path))[0];
   check('moves the message back by the id the archive gave it', !!moveBack && JSON.parse(moveBack.body).folder_id === 1, moveBack ? moveBack.body : 'no move back');
   const unmark = since(beforeUnmute).filter((r) => r.method === 'PUT' && /\/messages\/202\/flags$/.test(r.path))[0];
   check('and takes the mark off it', !!unmark && JSON.stringify(JSON.parse(unmark.body).keywords_remove) === '["$Muted"]', unmark ? unmark.body : 'no flags call');
   dismissAllToasts();

   // ---- select all: the page first, then the whole folder, read page by page
   document.getElementById('select-all').checked = true;
   document.getElementById('select-all').dispatchEvent(makeEvent('click'));
   const note = document.getElementById('select-note');
   check('ticking every row on the page offers the rest of the folder', note.hidden === false && note.textContent.indexOf('Select every message in Primary') >= 0, note.hidden + ' ' + note.textContent);
   document.getElementById('select-folder').dispatchEvent(makeEvent('click'));
   check('and says the whole folder is selected', document.getElementById('bulk-count').textContent === 'All of Primary selected' && note.textContent.indexOf('Clear selection') >= 0,
      document.getElementById('bulk-count').textContent + ' / ' + note.textContent);
   const beforeSweep = requests.length;
   document.getElementById('bulk-read').dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   await flush();
   const pagedCalls = since(beforeSweep).filter((r) => r.method === 'GET' && /\/folders\/1\/messages\?limit=200/.test(r.path));
   check('the folder is read page by page, from the uid the first page ended on', pagedCalls.length === 2 && /before_uid=11$/.test(pagedCalls[1].path), pagedCalls.map((r) => r.path).join(' '));
   const flaggedIds = since(beforeSweep).filter((r) => r.method === 'PUT' && /\/flags$/.test(r.path)).map((r) => r.path.match(/messages\/(\d+)\//)[1]);
   check('and every message of it is acted on, not only the page', flaggedIds.join(',') === '103,102,101,100,99', flaggedIds.join(','));
   check('the selection is cleared after', document.getElementById('bulk-bar').hidden === true && note.hidden === true);
   while (onCursor() < 1) { document.dispatchEvent(makeEvent('keydown', { key: 'j', target: document.body })); }
   while (onCursor() > 1) { document.dispatchEvent(makeEvent('keydown', { key: 'k', target: document.body })); }

   // ---- a row dragged onto a folder moves there, with undo
   const sentButton = (() => { const n = document.getElementById('folder-nav').children; for (let i = 0; i < n.length; i++) { if (n[i].getAttribute && n[i].getAttribute('data-route') === '/f/2') { return n[i]; } } return null; })();
   const beforeDrop = requests.length;
   rows()[0].dispatchEvent(makeEvent('dragstart'));
   sentButton.dispatchEvent(makeEvent('dragover'));
   sentButton.dispatchEvent(makeEvent('drop'));
   await flush();
   await flush();
   const dropCall = since(beforeDrop).filter((r) => r.method === 'POST' && /\/messages\/103\/move$/.test(r.path))[0];
   check('a row dropped on a folder is moved there', !!dropCall && JSON.parse(dropCall.body).folder_id === 2, dropCall ? dropCall.body : 'no move');
   check('and the toast offers Undo', document.getElementById('toasts').textContent.indexOf('Undo') >= 0, document.getElementById('toasts').textContent);
   { const toastBox = document.getElementById('toasts'); const buttons = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.tagName === 'BUTTON' && c.textContent === 'Undo') { buttons.push(c); } walk(c); } })(toastBox); if (buttons.length) { buttons[0].dispatchEvent(makeEvent('click')); } }
   await flush();
   await flush();
   while (onCursor() < 1) { document.dispatchEvent(makeEvent('keydown', { key: 'j', target: document.body })); }
   while (onCursor() > 1) { document.dispatchEvent(makeEvent('keydown', { key: 'k', target: document.body })); }

   // ---- Enter opens the message under the cursor, and it has an address
   const beforeOpen = requests.length;
   document.dispatchEvent(makeEvent('keydown', { key: 'Enter', target: document.body }));
   await flush();
   check('Enter opens the message under the cursor', location.hash === '#/m/102', location.hash);
   check('which is read from the server', called(beforeOpen, 'GET', '/api/v1/me/messages/102'));
   check('the message is shown beside the list, in the reading pane', document.getElementById('message-view').hidden === false &&
      document.getElementById('message-list').hidden === false && document.getElementById('mail-section').className.indexOf('has-message') >= 0,
      document.getElementById('mail-section').className);
   check('an unread message is marked read on opening', called(beforeOpen, 'PUT', /\/messages\/102\/flags$/));
   check('the attachment names its type', document.getElementById('message-attachments').textContent.indexOf('image/png') >= 0,
      document.getElementById('message-attachments').textContent);

   // ---- the HTML part is the server's document, in a frame with a policy of its own
   document.getElementById('message-html-toggle').dispatchEvent(makeEvent('click'));
   await flush();
   const frame = document.getElementById('message-html');
   const src = String(frame.getAttribute('src') || '');
   check('the HTML part is shown in the frame', frame.hidden === false && src.length > 0, src);
   check('the frame points at the message\'s own document on this server, without remote content',
      src === '/api/v1/me/messages/102/html', src);
   check('nothing is built here: the frame carries no srcdoc', !frame.srcdoc, String(frame.srcdoc || ''));
   check('the page fetched no attachment for it - the server inlines what the document embeds',
      !called(beforeOpen, 'GET', '/api/v1/me/messages/102/attachments/1'));
   check('a message that names remote content shows the note while the reader has not allowed it',
      document.getElementById('message-remote').hidden === false);
   document.getElementById('remote-once').dispatchEvent(makeEvent('click'));
   await flush();
   check('allowing it for this message points the frame at the document with its remote content',
      String(frame.getAttribute('src') || '') === '/api/v1/me/messages/102/html?remote=1', String(frame.getAttribute('src') || ''));
   check('and the note goes', document.getElementById('message-remote').hidden === true);
   check('the frame is still sandboxed', frame.getAttribute('sandbox') === 'allow-popups allow-popups-to-escape-sandbox',
      frame.getAttribute('sandbox'));

   // ---- Reply is an address of its own
   document.getElementById('message-reply').dispatchEvent(makeEvent('click'));
   await flush();
   check('Reply is an address', location.hash === '#/compose?reply=102', location.hash);
   check('the reply is addressed to the sender', document.getElementById('compose-to').value === 'b@example.net',
      document.getElementById('compose-to').value);
   check('and its subject answers the original', document.getElementById('compose-subject').value === 'Re: Second',
      document.getElementById('compose-subject').value);
   check('the reply is written under the message, over the list', document.getElementById('compose-section').hidden === false &&
      document.getElementById('mail-section').hidden === false && document.getElementById('compose-section').className.indexOf('inline') >= 0,
      document.getElementById('compose-section').className);
   check('and, by default, in the slot under the message it answers', document.getElementById('compose-section').parentNode.id === 'reply-slot',
      document.getElementById('compose-section').parentNode.id);

   // ---- Back goes back, and lands on the row it left
   history.back();
   await flush();
   check('Back returns to the message', location.hash === '#/m/102', location.hash);
   history.back();
   await flush();
   check('Back again returns to the folder', location.hash === '#/f/1', location.hash);
   check('the listing is shown again', document.getElementById('message-list').hidden === false &&
      document.getElementById('message-view').hidden === true);
   check('and the cursor is back on the row it was on', onCursor() === 1, 'at ' + onCursor());

   // ---- the change probe
   check('the first probe asked without a token', called(0, 'GET', '/api/v1/me/changes'));
   const beforeProbe = requests.length;
   check('the probe is waiting on a timer', timers.length === 1, timers.length + ' timers');
   fireTimers();
   await flush();
   check('the next probe carries the token the last answer gave',
      called(beforeProbe, 'GET', /\/api\/v1\/me\/changes\?since=t1$/), JSON.stringify(since(beforeProbe).map((r) => r.path)));
   check('a change makes the folder list and the listing be read again',
      called(beforeProbe, 'GET', '/api/v1/me/folders') && called(beforeProbe, 'GET', /\/folders\/1\/messages/),
      JSON.stringify(since(beforeProbe).map((r) => r.path)));
   check('the unseen count on screen follows it', nav.children[0].children[2].textContent === '3',
      nav.children[0].children[2].textContent);
   check('the message that arrived is in the listing', rows().length === 4 &&
      rows()[0].textContent.indexOf('Just in') >= 0, rows().length + ' rows');
   check('and the cursor is still on the message it was on, which has moved down',
      onCursor() === 2 && rows()[2].textContent.indexOf('Second') >= 0,
      'at ' + onCursor() + ' which is ' + (rows()[onCursor()] ? rows()[onCursor()].textContent.slice(0, 20) : 'nothing'));

   // ---- and it stops while the tab is in the background
   const beforeHidden = requests.length;
   document.hidden = true;
   document.dispatchEvent(makeEvent('visibilitychange'));
   fireTimers();
   await flush();
   check('nothing is polled while the tab is hidden', !called(beforeHidden, 'GET', /changes/),
      JSON.stringify(since(beforeHidden).map((r) => r.path)));
   check('and the reader is told why', document.getElementById('probe-state').textContent.indexOf('background') >= 0,
      document.getElementById('probe-state').textContent);
   document.hidden = false;
   document.dispatchEvent(makeEvent('visibilitychange'));
   await flush();
   check('it starts again when the tab comes back', called(beforeHidden, 'GET', /changes/));

   // ---- a server with no change route at all is asked once, not for ever
   changesMissing = true;
   const beforeGone = requests.length;
   fireTimers();
   await flush();
   check('a server with no change route is asked once more', called(beforeGone, 'GET', /changes/));
   check('and then not again', timers.length === 0, timers.length + ' timers still armed');
   check('and the reader is told', document.getElementById('probe-state').textContent.indexOf('does not report') >= 0,
      document.getElementById('probe-state').textContent);
   changesMissing = false;

   // ---- folder management
   const viewNav = document.getElementById('view-nav').children;
   const foldersButton = viewNav.filter((b) => b.getAttribute('data-route') === '/folders')[0];
   foldersButton.dispatchEvent(makeEvent('click'));
   await flush();
   check('managing folders is an address', location.hash === '#/folders', location.hash);
   const admin = document.getElementById('folder-admin-list');
   check('every folder is listed to be managed', admin.children.length === 3, admin.children.length + ' rows');

   const beforeCreate = requests.length;
   document.getElementById('folder-new-name').value = 'Bills';
   document.getElementById('folder-new-parent').value = '1';
   document.getElementById('folder-new-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const create = since(beforeCreate).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/folders')[0];
   check('creating a folder inside another posts the whole path, in the delimiter the server named',
      !!create && JSON.parse(create.body).name === 'INBOX.Bills', create ? create.body : 'no POST');
   check('and the folder tree is read again', called(beforeCreate, 'GET', '/api/v1/me/folders'));

   // The inbox and a folder the server keeps for a purpose cannot be deleted,
   // so the button is not there to be pressed.
   const adminRows = document.getElementById('folder-admin-list').children;
   const labels = (row) => row.children.map((c) => c.textContent);
   check('the inbox offers neither rename nor delete',
      labels(adminRows[0]).indexOf('Delete') < 0 && labels(adminRows[0]).indexOf('Rename') < 0, JSON.stringify(labels(adminRows[0])));
   const sentRow = adminRows.filter((r) => r.textContent.indexOf('Sent') >= 0)[0];
   check('a folder the server keeps for a special use may be renamed but not deleted',
      !!sentRow && labels(sentRow).indexOf('Delete') < 0 && labels(sentRow).indexOf('Rename') >= 0,
      sentRow ? JSON.stringify(labels(sentRow)) : 'no Sent row');
   check('and it says what it is kept for', !!sentRow && sentRow.textContent.indexOf('kept for') >= 0,
      sentRow ? sentRow.textContent : '');

   // A rename the server refuses shows the server's own sentence.
   renameRefusal = 'A folder called Work is already there.';
   const workAt = () => { const rows = document.getElementById('folder-admin-list').children; let at = -1; rows.forEach((r, i) => { if (r.textContent.indexOf('Work') >= 0) { at = i; } }); return at; };
   const at = workAt();
   const row = document.getElementById('folder-admin-list').children[at];
   row.children.filter((c) => c.textContent === 'Rename')[0].dispatchEvent(makeEvent('click'));
   // The row is now the editor, so it no longer carries the folder name.
   const editing = document.getElementById('folder-admin-list').children[at];
   const field = editing.children.filter((c) => c.tagName === 'INPUT')[0];
   check('renaming shows the whole path, because a rename may move the folder', field.value === 'INBOX.Work', field.value);
   field.value = 'INBOX.Work.2026';
   const beforeRename = requests.length;
   editing.children.filter((c) => c.textContent === 'Save')[0].dispatchEvent(makeEvent('click'));
   await flush();
   const rename = since(beforeRename).filter((r) => r.method === 'PUT')[0];
   check('renaming a folder puts the whole path', !!rename && /\/api\/v1\/me\/folders\/\d+$/.test(rename.path) &&
      JSON.parse(rename.body).name === 'INBOX.Work.2026', rename ? rename.method + ' ' + rename.path + ' ' + rename.body : 'no PUT');
   check("the server's own refusal is what the reader is shown",
      document.getElementById('folder-admin-status').textContent === renameRefusal,
      document.getElementById('folder-admin-status').textContent);
   renameRefusal = null;

   // Deleting takes two presses, and the second one is the one that deletes.
   const doomed = document.getElementById('folder-admin-list').children[at];
   const remove = doomed.children.filter((c) => c.textContent === 'Delete')[0];
   const beforeDelete = requests.length;
   remove.dispatchEvent(makeEvent('click'));
   await flush();
   check('one press on Delete asks rather than deletes', !called(beforeDelete, 'DELETE', /folders/) &&
      remove.textContent === 'Really delete?', remove.textContent);
   remove.dispatchEvent(makeEvent('click'));
   await flush();
   check('the second press deletes it', called(beforeDelete, 'DELETE', /^\/api\/v1\/me\/folders\/\d+$/));

   // ---- searching has an address of its own
   document.getElementById('mail-search').value = 'invoice';
   document.getElementById('mail-search-everywhere').checked = true;
   document.getElementById('mail-search-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('a search across every folder is an address', location.hash === '#/search?q=invoice', location.hash);
   check('and it is what was asked of the server', called(requests.length - 3, 'GET', /\/api\/v1\/me\/search\?q=invoice/),
      JSON.stringify(requests.slice(-3).map((r) => r.path)));

   // ---- the attachment reminder: a message that promises an attachment and carries none is questioned once
   const compose = document.getElementById('compose-section');
   // The window opens on the navigation's microtask, and opening blanks the
   // form, so the fields are filled once that has happened.
   const writeMessage = async (text) => {
      document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
      await flush();
      document.getElementById('compose-to').value = 'a@example.net';
      document.getElementById('compose-subject').value = 'The report';
      document.getElementById('compose-text').value = text;
   };
   await writeMessage('Please see the attached report.\n> You said you would attach it.');
   check('c opens the compose window', location.hash === '#/compose' && compose.hidden === false, location.hash + ' hidden=' + compose.hidden);
   confirmAnswer = false;
   const beforeAsk = requests.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('a body that says "attached" with nothing attached is questioned', confirms.length === 1 && confirms[0].indexOf('nothing is attached') >= 0, JSON.stringify(confirms));
   check('Cancel sends nothing', !called(beforeAsk, 'POST', '/api/v1/me/messages') && timers.length === 0, timers.length + ' timers');
   check('and leaves the message in the open form', compose.hidden === false && document.getElementById('compose-text').value.indexOf('attached report') >= 0, 'hidden=' + compose.hidden);
   confirmAnswer = true;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('OK is asked once more and lets it go', confirms.length === 2 && timers.length > 0, confirms.length + ' questions, ' + timers.length + ' timers');
   fireTimers();
   await flush();
   await flush();
   check('and the message is sent after the undo delay', called(beforeAsk, 'POST', '/api/v1/me/messages'), JSON.stringify(since(beforeAsk).map((r) => r.method + ' ' + r.path)));
   check('and the form is closed', compose.hidden === true && document.getElementById('compose-text').value === '', 'hidden=' + compose.hidden);
   dismissAllToasts();
   await writeMessage('Thanks, received.\n> Please find the report attached.\n> It is enclosed as a PDF.');
   const beforeQuoted = requests.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   fireTimers();
   await flush();
   await flush();
   check('a mention only in the quoted lines is the other person\'s and is not questioned', confirms.length === 2 && called(beforeQuoted, 'POST', '/api/v1/me/messages'), confirms.length + ' questions');
   dismissAllToasts();

   // ---- a recipient on the suppression list: the reason in the reader's words, naming the address
   await writeMessage('See you on Monday.');
   sendRefusal = { error: 'gone@remote.test: gone@remote.test is on the suppression list of example.com: mail to it bounced permanently (5.1.1) on 2026-09-14. An administrator can remove it.',
      code: 'recipient_suppressed', address: 'gone@remote.test', source: 'bounce' };
   const beforeSuppressed = requests.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   fireTimers();
   await flush();
   await flush();
   const suppressedStatus = document.getElementById('compose-status').textContent;
   check('a send refused by the suppression list says which address and why, in the page\'s own sentence', called(beforeSuppressed, 'POST', '/api/v1/me/messages') &&
      suppressedStatus.indexOf('Not sent to gone@remote.test') === 0 && suppressedStatus.indexOf('suppression list of your domain') > 0 &&
      suppressedStatus.indexOf('has bounced') > 0, suppressedStatus);
   check('and the message stays in the form, to be sent without that address', document.getElementById('compose-text').value === 'See you on Monday.');

   // ---- the same refusal for an entry an administrator added: it does not say the address bounced
   sendRefusal = { error: 'left@remote.test: left@remote.test is on the suppression list of example.com: it was added by an administrator on 2026-09-14. An administrator can remove it.',
      code: 'recipient_suppressed', address: 'left@remote.test', source: 'manual' };
   const beforeManual = requests.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   fireTimers();
   await flush();
   await flush();
   const manualStatus = document.getElementById('compose-status').textContent;
   check('a hand-made entry is refused in its own sentence, which names the address and does not claim it bounced', called(beforeManual, 'POST', '/api/v1/me/messages') &&
      manualStatus.indexOf('Not sent to left@remote.test') === 0 && manualStatus.indexOf('your administrator has put this address') > 0 &&
      manualStatus.indexOf('bounced') < 0, manualStatus);
   document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
   await flush();
   dismissAllToasts();

   // ---- shift-click and ctrl-click in the list select; a plain click still opens
   nav.children[0].dispatchEvent(makeEvent('click'));
   await flush();
   check('the inbox is listed again, with the message that arrived', location.hash === '#/f/1' && rows().length === 4, location.hash + ' ' + rows().length + ' rows');
   const boxOf = (row) => row.childNodes[0].childNodes[0];
   const ticked = () => rows().filter((r) => r.className.indexOf('selected') >= 0).length;
   const beforeCtrl = requests.length;
   rows()[0].dispatchEvent(makeEvent('click', { ctrlKey: true }));
   check('a ctrl-click ticks the row without opening it', document.getElementById('bulk-count').textContent === '1 selected' && location.hash === '#/f/1' && boxOf(rows()[0]).checked === true,
      document.getElementById('bulk-count').textContent + ' ' + location.hash);
   rows()[2].dispatchEvent(makeEvent('click', { shiftKey: true }));
   check('a shift-click ticks every row from the last one clicked to this one', document.getElementById('bulk-count').textContent === '3 selected' && ticked() === 3 && boxOf(rows()[1]).checked === true,
      document.getElementById('bulk-count').textContent + ', ' + ticked() + ' rows marked');
   rows()[1].dispatchEvent(makeEvent('click', { ctrlKey: true }));
   check('a ctrl-click on a ticked row unticks it', document.getElementById('bulk-count').textContent === '2 selected' && boxOf(rows()[1]).checked === false, document.getElementById('bulk-count').textContent);
   boxOf(rows()[3]).checked = true;
   boxOf(rows()[3]).dispatchEvent(makeEvent('click', { shiftKey: true }));
   check('a shift-click on a box ticks the range from the last row clicked', document.getElementById('bulk-count').textContent === '4 selected' && ticked() === 4, document.getElementById('bulk-count').textContent);
   check('and none of it opened a message or asked the server', location.hash === '#/f/1' && !called(beforeCtrl, 'GET', /\/messages\/\d+$/), JSON.stringify(since(beforeCtrl).map((r) => r.path)));
   rows()[1].dispatchEvent(makeEvent('click'));
   await flush();
   check('a plain click still opens the message', location.hash === '#/m/103' && called(beforeCtrl, 'GET', '/api/v1/me/messages/103'), location.hash);
   document.getElementById('bulk-clear').dispatchEvent(makeEvent('click'));
   check('and the selection can be cleared', document.getElementById('bulk-bar').hidden === true && ticked() === 0, ticked() + ' rows marked');

   // ---- search history: the last ten searches, kept in this browser and offered under the empty box
   const search = document.getElementById('mail-search');
   const historyBox = document.getElementById('search-history');
   const historyList = document.getElementById('search-history-list');
   const searchFor = async (text) => { search.value = text; document.getElementById('mail-search-form').dispatchEvent(makeEvent('submit')); await flush(); };
   check('the search made earlier was kept in this browser', store.get('hmPortalSearches') === '["invoice"]', store.get('hmPortalSearches'));
   await searchFor('from:alice');
   check('a new search goes to the front', store.get('hmPortalSearches') === '["from:alice","invoice"]', store.get('hmPortalSearches'));
   await searchFor('invoice');
   check('a search made again moves to the front rather than doubling', store.get('hmPortalSearches') === '["invoice","from:alice"]', store.get('hmPortalSearches'));
   for (let i = 0; i < 12; i += 1) { await searchFor('word' + i); }
   const kept = JSON.parse(store.get('hmPortalSearches'));
   check('and the list holds the last ten', kept.length === 10 && kept[0] === 'word11' && kept[9] === 'word2', JSON.stringify(kept));
   search.value = 'x';
   search.dispatchEvent(makeEvent('focus'));
   check('nothing is offered under a box with something in it', historyBox.hidden === true);
   const beforeShown = requests.length;
   search.value = '';
   search.dispatchEvent(makeEvent('input'));
   check('emptying the box offers the recent searches, newest first', historyBox.hidden === false && historyList.children.length === 10 && historyList.children[0].textContent === 'word11',
      'hidden=' + historyBox.hidden + ' ' + historyList.children.length + ' entries');
   check('without asking the server', requests.length === beforeShown, JSON.stringify(since(beforeShown).map((r) => r.path)));
   historyList.children[1].dispatchEvent(makeEvent('click'));
   await flush();
   // The folder shown is the scope, as the box's own search would have it: the inbox, since the list before this one opened it.
   check('an entry runs that search again, in the folder shown', location.hash === '#/f/1?q=word10' && called(beforeShown, 'GET', /\/folders\/1\/messages\?.*q=word10/) && historyBox.hidden === true,
      location.hash + ' ' + JSON.stringify(since(beforeShown).map((r) => r.path)));
   search.value = '';
   search.dispatchEvent(makeEvent('focus'));
   check('the box focused and empty offers them', historyBox.hidden === false && historyList.children[0].textContent === 'word10', historyList.children[0].textContent);
   search.dispatchEvent(makeEvent('keydown', { key: 'ArrowDown', target: search }));
   check('the arrow key moves into the list', document.activeElement === historyList.children[0]);
   historyList.children[0].dispatchEvent(makeEvent('keydown', { key: 'ArrowDown', target: historyList.children[0] }));
   check('and down it', document.activeElement === historyList.children[1]);
   historyList.children[1].dispatchEvent(makeEvent('keydown', { key: 'Escape', target: historyList.children[1] }));
   check('Escape closes it and returns to the box', historyBox.hidden === true && document.activeElement === search);
   search.dispatchEvent(makeEvent('focus'));
   document.getElementById('search-history-clear').dispatchEvent(makeEvent('click'));
   check('Clear the list empties it', !store.has('hmPortalSearches') && historyBox.hidden === true, String(store.get('hmPortalSearches')));
   search.dispatchEvent(makeEvent('focus'));
   check('and nothing is offered after', historyBox.hidden === true);
   await searchFor('after:2026-09-01');
   check('a search after that starts the list again', store.get('hmPortalSearches') === '["after:2026-09-01"]', store.get('hmPortalSearches'));

   // ---- search suggestions: a contact's name, typed in the box, completes to from:<address>
   const suggestBox = document.getElementById('search-suggest');
   const suggestList = document.getElementById('search-suggest-list');
   const suggestHead = document.getElementById('search-suggest-head');
   const typeSearch = async (text) => { search.value = text; search.dispatchEvent(makeEvent('input')); fireTimers(); await flush(); };
   const beforeSuggest = requests.length;
   await typeSearch('a');
   check('one letter in the box asks nothing of the address book', !called(beforeSuggest, 'GET', /contacts/) && suggestBox.hidden === true,
      JSON.stringify(since(beforeSuggest).map((r) => r.path)));
   await typeSearch('ali');
   check('a name typed in the box asks the address book for it, as the To field does', called(beforeSuggest, 'GET', '/api/v1/me/contacts?limit=8&q=ali'),
      JSON.stringify(since(beforeSuggest).map((r) => r.path)));
   check('and the contact it matches is offered under the box as from:<address>',
      suggestBox.hidden === false && suggestList.children.length === 1 && suggestList.children[0].textContent === 'Alice Examplefrom:alice@example.net' && suggestHead.textContent === 'Search by sender',
      'hidden=' + suggestBox.hidden + ' ' + suggestList.children.length + ' entries, head=' + suggestHead.textContent);
   check('while the history stays closed', historyBox.hidden === true);
   await typeSearch('invoice to:al');
   check('a name after to: completes to to:<address>, and the head says so',
      suggestList.children.length === 2 && suggestList.children[0].textContent === 'Alice Exampleto:alice@example.net' && suggestHead.textContent === 'Search by recipient',
      suggestList.children.length + ' entries, head=' + suggestHead.textContent);
   const beforeOperator = requests.length;
   await typeSearch('is:unread');
   check('an operator is not a name and asks nothing', !called(beforeOperator, 'GET', /contacts/) && suggestBox.hidden === true,
      JSON.stringify(since(beforeOperator).map((r) => r.path)));
   await typeSearch('invoice ali');
   search.dispatchEvent(makeEvent('keydown', { key: 'ArrowDown', target: search }));
   check('the arrow key moves into the suggestions', document.activeElement === suggestList.children[0]);
   suggestList.children[0].dispatchEvent(makeEvent('keydown', { key: 'Escape', target: suggestList.children[0] }));
   check('Escape closes them and returns to the box', suggestBox.hidden === true && document.activeElement === search);
   await typeSearch('invoice ali');
   const beforeTake = requests.length;
   suggestList.children[0].dispatchEvent(makeEvent('click'));
   await flush();
   check('one taken stands in the box as from:<address> beside the words typed, and the search runs in the folder shown',
      search.value === 'invoice from:alice@example.net' && location.hash === '#/f/1?q=' + encodeURIComponent('invoice from:alice@example.net') && called(beforeTake, 'GET', /\/folders\/1\/messages\?.*q=invoice/) && suggestBox.hidden === true,
      'value=' + search.value + ' ' + location.hash + ' ' + JSON.stringify(since(beforeTake).map((r) => r.path)));
   check('and is kept with the recent searches', JSON.parse(store.get('hmPortalSearches'))[0] === 'invoice from:alice@example.net', store.get('hmPortalSearches'));

   // ---- pop-out: the message, or the message being written, in a window of its own
   location.hash = '#/m/102';
   await flush();
   check('the message is open', location.hash === '#/m/102' && document.getElementById('message-view').hidden === false, location.hash);
   document.getElementById('message-popout').dispatchEvent(makeEvent('click'));
   check('the pop-out opens the message at its own address in a new window of a given size',
      opened.length === 1 && opened[0].url === 'http://portal.test/portal#/m/102' && /width=\d+/.test(opened[0].features) && /height=\d+/.test(opened[0].features), JSON.stringify(opened));
   check('and this window stays where it was', location.hash === '#/m/102' && document.getElementById('message-view').hidden === false, location.hash);
   document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
   await flush();
   document.getElementById('compose-popout').dispatchEvent(makeEvent('click'));
   await flush();
   check('an empty compose form pops out at its own address', opened.length === 2 && opened[1].url === 'http://portal.test/portal#/compose', JSON.stringify(opened.slice(1)));
   check('and closes here', compose.hidden === true, 'hidden=' + compose.hidden);
   await writeMessage('Half a thought');
   const beforePop = requests.length;
   document.getElementById('compose-popout').dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   const draftCall = since(beforePop).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/drafts')[0];
   check('a form with something in it is saved as a draft first', !!draftCall && JSON.parse(draftCall.body).text === 'Half a thought', draftCall ? draftCall.body : 'no draft saved');
   check('and the new window opens on that draft', opened.length === 3 && opened[2].url === 'http://portal.test/portal#/compose?draft=55', JSON.stringify(opened.slice(2)));
   check('and the form here is closed and blank, so one window edits the draft', compose.hidden === true && document.getElementById('compose-text').value === '', 'hidden=' + compose.hidden + ' text=' + document.getElementById('compose-text').value);

   // ---- follow-up flags: a flag with a date, the badge on the row, the Starred view by due date
   location.hash = '#/f/1';
   await flush();
   const dueOfRow = (row) => { let found = null; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.className && c.className.indexOf('due') === 0) { found = c; } if (c.childNodes) { walk(c); } } })(row); return found; };
   const rowBySubject = (subject) => rows().filter((r) => r.textContent.indexOf(subject) >= 0)[0];
   check('a row due today says so', !!dueOfRow(rowBySubject('First')) && dueOfRow(rowBySubject('First')).textContent === 'Due today' && dueOfRow(rowBySubject('First')).className.indexOf('now') >= 0,
      dueOfRow(rowBySubject('First')) ? dueOfRow(rowBySubject('First')).textContent : 'no badge');
   check('and a row with no follow-up carries no badge', !dueOfRow(rowBySubject('Second')));
   location.hash = '#/m/102';
   await flush();
   const followBtn = document.getElementById('message-followup');
   const followMenu = document.getElementById('followup-menu');
   check('the open message offers Follow up, off', !followBtn.classList.contains('on') && followMenu.hidden === true && document.getElementById('followup-clear').hidden === true,
      followBtn.className + ' menu hidden=' + followMenu.hidden);
   followBtn.dispatchEvent(makeEvent('click'));
   check('the button opens the menu', followMenu.hidden === false);
   const beforeFollow = requests.length;
   document.getElementById('followup-tomorrow').dispatchEvent(makeEvent('click'));
   await flush();
   const tomorrowStamp = (() => { const d = new Date(); d.setDate(d.getDate() + 1); return d.getFullYear() + '-' + String(d.getMonth() + 1).padStart(2, '0') + '-' + String(d.getDate()).padStart(2, '0'); })();
   const followCall = since(beforeFollow).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/messages/102/flags')[0];
   check('tomorrow stars the message and adds the $FollowUp and $Due- keywords in one change',
      !!followCall && JSON.parse(followCall.body).flagged === true && JSON.stringify(JSON.parse(followCall.body).keywords_add) === JSON.stringify(['$FollowUp', '$Due-' + tomorrowStamp]),
      followCall ? followCall.body : 'no flags call');
   check('the button shows it on, the menu closes and Clear the flag is offered', followBtn.classList.contains('on') && followMenu.hidden === true && document.getElementById('followup-clear').hidden === false, followBtn.className);
   check('and the row carries the day', !!dueOfRow(rowBySubject('Second')) && dueOfRow(rowBySubject('Second')).getAttribute('title') === tomorrowStamp,
      dueOfRow(rowBySubject('Second')) ? dueOfRow(rowBySubject('Second')).textContent : 'no badge');
   dismissAllToasts();
   location.hash = '#/starred';
   await flush();
   // The starred messages, the dated ones first and the soonest at the top, the undated after them.
   check('the Starred view lists the follow-ups by due date, the soonest first', rows().length >= 2 && rows()[0].textContent.indexOf('First') >= 0 && rows()[1].textContent.indexOf('Second') >= 0 && rows().slice(2).every((r) => !dueOfRow(r)),
      rows().map((r) => r.textContent.slice(0, 40)).join(' | '));
   location.hash = '#/m/102';
   await flush();
   const beforeClear = requests.length;
   document.getElementById('followup-clear').dispatchEvent(makeEvent('click'));
   await flush();
   const clearCall = since(beforeClear).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/messages/102/flags')[0];
   check('Clear the flag unstars the message and takes both keywords off, and the second and third stars\' keywords with them',
      !!clearCall && JSON.parse(clearCall.body).flagged === false && JSON.stringify(JSON.parse(clearCall.body).keywords_remove) === JSON.stringify(['$Due-' + tomorrowStamp, '$FollowUp', '$Star2', '$Star3']),
      clearCall ? clearCall.body : 'no flags call');
   check('and the button is off again', !followBtn.classList.contains('on') && document.getElementById('followup-clear').hidden === true, followBtn.className + ' clear hidden=' + document.getElementById('followup-clear').hidden);
   dismissAllToasts();
   // The reminder's Open: announced again on a fresh sign-in, since what was announced went with the account.
   document.getElementById('signout').dispatchEvent(makeEvent('click'));
   await flush();
   check('sign-out forgets which follow-ups were announced', !store.has('hmPortalDueShown') && gate.hidden === false, String(store.get('hmPortalDueShown')));
   document.getElementById('address').value = 'user@example.com';
   document.getElementById('password').value = 'a-real-password';
   const beforeAgain = requests.length;
   document.getElementById('signin-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('and the next sign-in announces the one due today again', app.hidden === false && toastBox.textContent.indexOf('Due for follow-up: First') >= 0, toastBox.textContent);
   { const buttons = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.tagName === 'BUTTON' && c.textContent === 'Open') { buttons.push(c); } if (c.childNodes) { walk(c); } } })(toastBox); if (buttons.length) { buttons[0].dispatchEvent(makeEvent('click')); } }
   await flush();
   check('Open opens the message', location.hash === '#/m/101' && called(beforeAgain, 'GET', '/api/v1/me/messages/101') && toastBox.children.length === 0, location.hash + ' ' + toastBox.children.length + ' toasts');
   document.getElementById('message-back').dispatchEvent(makeEvent('click'));
   await flush();

   // ---- nudges: a reply or a follow-up that seems owed, from the row's own fields and one bounded search
   location.hash = '#/f/1';
   await flush();
   const nudgeOf = (row) => { let found = null; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.className === 'nudge') { found = c; } if (c.childNodes) { walk(c); } } })(row); return found; };
   check('a message received days ago that asked a question and was never answered is nudged', !!nudgeOf(rowBySubject('Third')) && nudgeOf(rowBySubject('Third')).textContent === 'Received 5 days ago. Reply?',
      nudgeOf(rowBySubject('Third')) ? nudgeOf(rowBySubject('Third')).textContent : 'no nudge');
   check('and one that asked nothing is not', !nudgeOf(rowBySubject('Second')) && !nudgeOf(rowBySubject('First')));
   const beforeSentView = requests.length;
   location.hash = '#/f/2';
   await flush();
   const askCall = since(beforeSentView).filter((r) => r.method === 'GET' && r.path.startsWith('/api/v1/me/search?q='))[0];
   check('the Sent folder asks once whether anything answered the messages old enough to nudge, in one search',
      since(beforeSentView).filter((r) => r.path.startsWith('/api/v1/me/search?q=')).length === 1 && !!askCall && decodeURIComponent(askCall.path.split('q=')[1].split('&')[0]) === 'in_reply_to:<one@example.net> OR in_reply_to:<agenda@example.com>',
      askCall ? askCall.path : 'no search');
   check('a sent message nothing answered is nudged to follow up', !!nudgeOf(rowBySubject('Draft agenda')) && nudgeOf(rowBySubject('Draft agenda')).textContent === 'Sent 5 days ago. Follow up?',
      nudgeOf(rowBySubject('Draft agenda')) ? nudgeOf(rowBySubject('Draft agenda')).textContent : 'no nudge');
   check('one that was answered is not, nor one sent yesterday', !nudgeOf(rowBySubject('Figures for the board')) && !nudgeOf(rowBySubject('Thanks')),
      rows().map((r) => r.textContent.slice(0, 60)).join(' | '));
   location.hash = '#/f/1';
   await flush();
   const beforeSentAgain = requests.length;
   location.hash = '#/f/2';
   await flush();
   check('listed again within a while, the folder asks nothing more', !since(beforeSentAgain).some((r) => r.path.startsWith('/api/v1/me/search?q=')) && !!nudgeOf(rowBySubject('Draft agenda')),
      JSON.stringify(since(beforeSentAgain).map((r) => r.path)));
   document.getElementById('pref-nudges').checked = false;
   const beforeNudgesOff = requests.length;
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const nudgePref = since(beforeNudgesOff).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/preferences')[0];
   check('the preference turns nudges off, with the account', !!nudgePref && JSON.parse(nudgePref.body).nudges === '0' && !nudgeOf(rowBySubject('Draft agenda')),
      (nudgePref ? nudgePref.body : 'no preferences call') + ' nudge=' + !!nudgeOf(rowBySubject('Draft agenda')));

   // ---- the reply card above the conversation, as a choice (discussion #266)
   document.getElementById('pref-reply-place').value = 'above';
   const beforeReplyPlace = requests.length;
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const replyPlacePref = since(beforeReplyPlace).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/preferences')[0];
   check('the reply card can be put above the conversation, with the account', !!replyPlacePref && JSON.parse(replyPlacePref.body).reply_place === 'above',
      replyPlacePref ? replyPlacePref.body : 'no preferences call');
   location.hash = '#/m/102';
   await flush();
   document.getElementById('message-reply').dispatchEvent(makeEvent('click'));
   await flush();
   check('and the next reply is written above the conversation, still inline', document.getElementById('compose-section').parentNode.id === 'reply-slot-top' &&
      document.getElementById('compose-section').className.indexOf('inline') >= 0, document.getElementById('compose-section').parentNode.id);
   check('the message sent is unchanged by the choice: still addressed to the sender', document.getElementById('compose-to').value === 'b@example.net',
      document.getElementById('compose-to').value);
   location.hash = '#/settings';
   await flush();
   document.getElementById('pref-reply-place').value = 'below';
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('and put back under the message', prefsStore.reply_place === 'below', String(prefsStore.reply_place));

   // ---- auto-delete and auto-archive, the reader's own choices, kept with the account
   check('the retention choices read as Never until the reader makes one',
      document.getElementById('pref-retention-trash').value === '0' && document.getElementById('pref-retention-archive').value === '0',
      document.getElementById('pref-retention-trash').value + '/' + document.getElementById('pref-retention-archive').value);
   document.getElementById('pref-retention-trash').value = '30';
   document.getElementById('pref-retention-junk').value = '7';
   document.getElementById('pref-retention-archive').value = '365';
   const beforeRetention = requests.length;
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const retentionPref = since(beforeRetention).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/preferences')[0];
   const retentionBody = retentionPref ? JSON.parse(retentionPref.body) : {};
   check('the three choices go to the account as days, for the server\'s sweep to read',
      retentionBody.retention_trash_days === '30' && retentionBody.retention_junk_days === '7' && retentionBody.retention_archive_days === '365',
      retentionPref ? retentionPref.body : 'no preferences call');
   document.getElementById('pref-retention-trash').value = '0';
   document.getElementById('pref-retention-junk').value = '0';
   document.getElementById('pref-retention-archive').value = '0';
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();

   // ---- quick steps: a button of the reader's own, kept with the account, run by a click or a key
   location.hash = '#/settings';
   await flush();
   const stepButtons = () => document.getElementById('quick-steps').children;
   const stepRows = () => document.getElementById('quickstep-rows').children;
   const defineStep = async (name, key, read, move, label, forward) => {
      document.getElementById('quickstep-name').value = name;
      document.getElementById('quickstep-key').value = key;
      document.getElementById('quickstep-read').checked = read;
      document.getElementById('quickstep-move').value = move;
      document.getElementById('quickstep-label').value = label;
      document.getElementById('quickstep-forward').value = forward;
      document.getElementById('quickstep-form').dispatchEvent(makeEvent('submit'));
      await flush();
   };
   const beforeStep = requests.length;
   await defineStep('File it', '1', true, '3', 'done', '');
   const stepPref = since(beforeStep).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/preferences')[0];
   const stepSaved = stepPref && JSON.parse(stepPref.body)['qs.file-it'] ? JSON.parse(JSON.parse(stepPref.body)['qs.file-it']) : null;
   check('a quick step is kept with the account\'s preferences', !!stepSaved && stepSaved.key === '1' && stepSaved.read === true && stepSaved.move === 3 && stepSaved.label === 'done',
      stepPref ? stepPref.body : 'no preferences call');
   check('and appears as a button over the list', stepButtons().length === 1 && stepButtons()[0].textContent === 'File it', stepButtons().length + ' buttons');
   check('and in the settings table with what it does', stepRows().length === 1 && stepRows()[0].textContent.indexOf('Mark as read') >= 0 && stepRows()[0].textContent.indexOf('Label done') >= 0 && stepRows()[0].textContent.indexOf('Move to INBOX.Work') >= 0,
      stepRows().length ? stepRows()[0].textContent : 'no rows');
   const beforeIdle = requests.length;
   await defineStep('Idle', '', false, '', '', '');
   check('a step with nothing to do is refused', requests.length === beforeIdle && document.getElementById('quickstep-status').textContent === 'Give the quick step something to do.',
      document.getElementById('quickstep-status').textContent);
   location.hash = '#/f/1';
   await flush();
   boxOf(rowBySubject('Second')).checked = true;
   boxOf(rowBySubject('Second')).dispatchEvent(makeEvent('click'));
   const beforeRun = requests.length;
   stepButtons()[0].dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   const ran = since(beforeRun).map((r) => ({ method: r.method, path: r.path, body: r.body ? JSON.parse(r.body) : {} }));
   const readAt = ran.findIndex((r) => r.method === 'PUT' && r.path === '/api/v1/me/messages/102/flags' && r.body.seen === true);
   const labelAt = ran.findIndex((r) => r.method === 'PUT' && r.path === '/api/v1/me/messages/102/flags' && JSON.stringify(r.body.keywords_add) === '["done"]');
   const moveAt = ran.findIndex((r) => r.method === 'POST' && r.path === '/api/v1/me/messages/102/move' && r.body.folder_id === 3);
   check('the button marks the ticked message read, labels it and moves it, in that order', readAt >= 0 && labelAt > readAt && moveAt > labelAt,
      JSON.stringify(ran.map((r) => r.method + ' ' + r.path)));
   check('with the filing\'s own toast and Undo, and the selection cleared', toasts.textContent.indexOf('Undo') >= 0 && ticked() === 0, toasts.textContent + ' ' + ticked() + ' ticked');
   dismissAllToasts();
   const beforeKey = requests.length;
   document.dispatchEvent(makeEvent('keydown', { key: '1', target: document.body }));
   await flush();
   await flush();
   const keyed = since(beforeKey);
   check('the key 1 runs it on the row under the cursor', keyed.some((r) => r.method === 'PUT' && /\/messages\/\d+\/flags$/.test(r.path) && JSON.parse(r.body).seen === true) && keyed.some((r) => r.method === 'POST' && /\/messages\/\d+\/move$/.test(r.path)),
      JSON.stringify(keyed.map((r) => r.method + ' ' + r.path)));
   dismissAllToasts();
   const beforeNoStep = requests.length;
   document.dispatchEvent(makeEvent('keydown', { key: '2', target: document.body }));
   await flush();
   check('a digit with no step does nothing', requests.length === beforeNoStep, JSON.stringify(since(beforeNoStep).map((r) => r.path)));
   // A step that forwards primes the compose window from the message before anything moves it.
   location.hash = '#/settings';
   await flush();
   await defineStep('Send on', '2', false, '3', '', 'boss@example.net');
   location.hash = '#/f/1';
   await flush();
   boxOf(rowBySubject('Second')).checked = true;
   boxOf(rowBySubject('Second')).dispatchEvent(makeEvent('click'));
   const beforeForward = requests.length;
   stepButtons().filter((b) => b.textContent === 'Send on')[0].dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   await flush();
   const fwd = since(beforeForward);
   const gotAt = fwd.findIndex((r) => r.method === 'GET' && r.path === '/api/v1/me/messages/102');
   const movedAt = fwd.findIndex((r) => r.method === 'POST' && r.path === '/api/v1/me/messages/102/move');
   check('a forwarding step reads the message into the compose window, addressed, before the move', gotAt >= 0 && movedAt > gotAt && compose.hidden === false && document.getElementById('compose-to').value === 'boss@example.net' && location.hash === '#/compose?forward=102',
      'got=' + gotAt + ' moved=' + movedAt + ' to=' + document.getElementById('compose-to').value + ' ' + location.hash);
   dismissAllToasts();
   location.hash = '#/settings';
   await flush();
   const beforeRemove = requests.length;
   stepRows()[0].children[3].children[0].dispatchEvent(makeEvent('click'));
   await flush();
   const removePref = since(beforeRemove).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/preferences')[0];
   check('Remove takes it out of the preferences and off the list', !!removePref && JSON.parse(removePref.body)['qs.file-it'] === null && stepButtons().length === 1 && stepButtons()[0].textContent === 'Send on',
      (removePref ? removePref.body : 'no preferences call') + ' ' + stepButtons().length + ' buttons');

   // ---- clean up conversation: a message whose whole text a later one quotes goes to Trash
   location.hash = '#/f/3';
   await flush();
   check('the Work folder lists its conversation as one row', rows().length === 1 && rows()[0].className.indexOf('thread') >= 0, rows().length + ' rows ' + (rows()[0] ? rows()[0].className : ''));
   rows()[0].dispatchEvent(makeEvent('contextmenu', { clientX: 300, clientY: 200 }));
   const cleanItem = menuItem('cleanup');
   check('the row menu offers Clean up conversation', !!cleanItem && cleanItem.hidden === false, cleanItem ? 'hidden=' + cleanItem.hidden : 'no item');
   const beforeClean = requests.length;
   cleanItem.dispatchEvent(makeEvent('click', { target: cleanItem }));
   await flush();
   await flush();
   await flush();
   const cleaned = since(beforeClean);
   check('every message of the conversation is read', [501, 502, 503].every((id) => cleaned.some((r) => r.method === 'GET' && r.path === '/api/v1/me/messages/' + id)),
      JSON.stringify(cleaned.map((r) => r.method + ' ' + r.path)));
   check('the one quoted whole by a later message goes to Trash, and only that one', cleaned.filter((r) => r.method === 'DELETE').map((r) => r.path).join(' ') === '/api/v1/me/messages/501',
      JSON.stringify(cleaned.filter((r) => r.method === 'DELETE').map((r) => r.path)));
   check('with a toast that offers Undo', toasts.textContent.indexOf('Undo') >= 0, toasts.textContent);
   dismissAllToasts();
   location.hash = '#/f/1';
   await flush();
   rows()[0].dispatchEvent(makeEvent('contextmenu', { clientX: 300, clientY: 200 }));
   check('a row that is one message offers no clean-up', menuItem('cleanup').hidden === true);
   document.dispatchEvent(makeEvent('keydown', { key: 'Escape', target: document.body }));
   location.hash = '#/m/103';
   await flush();
   const beforeLone = requests.length;
   document.getElementById('message-cleanup').dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   check('the open message, alone in its conversation, has nothing to clean up and nothing is deleted', !since(beforeLone).some((r) => r.method === 'DELETE') && toasts.textContent.indexOf('Nothing to clean up') >= 0,
      toasts.textContent + ' ' + JSON.stringify(since(beforeLone).map((r) => r.method + ' ' + r.path)));
   dismissAllToasts();

   // ---- the theme is a choice the browser keeps, and it is not a secret
   document.getElementById('theme-btn').dispatchEvent(makeEvent('click'));
   check('the theme can be turned over', document.body.getAttribute('data-theme') === 'light',
      document.body.getAttribute('data-theme'));
   check('and is remembered', store.get('hmPortalTheme') === 'light');
      // ---- report phishing: to the postmaster, into junk, the sender blocked
   location.hash = '#/m/102';
   await flush();
   const beforePhish = requests.length;
   document.getElementById('message-phish').dispatchEvent(makeEvent('click'));
   await flush();
   const phishReport = since(beforePhish).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/messages').map((r) => JSON.parse(r.body))[0];
   check('the report goes to the postmaster of the reader\'s domain with the message named', !!phishReport && phishReport.to === 'postmaster@example.com' && phishReport.subject === 'Phishing report: Second' && phishReport.text.indexOf('b@example.net') >= 0,
      JSON.stringify(phishReport).slice(0, 200));
   // The sender goes onto the reader's own blocked-sender list, not into their
   // Sieve script. It used to be a rule - a move-to-Junk, or a DISCARD when the
   // account had no Junk folder, which destroyed the evidence of a phishing
   // report - and a rule could not cover a domain, could not expire and was
   // invisible to the administrator asked why a sender was not getting through.
   check('the sender goes onto the blocked-sender list', since(beforePhish).some((r) => r.method === 'POST' && r.path === '/api/v1/me/sender-list' && /b@example\.net/.test(r.body || '') && /"blocked":true/.test(r.body || '')),
      JSON.stringify(since(beforePhish).map((r) => r.method + ' ' + r.path).slice(0, 6)));
   check('and nothing is written over the reader\'s filter script', !since(beforePhish).some((r) => r.method === 'PUT' && r.path === '/api/v1/me/filters'),
      JSON.stringify(since(beforePhish).map((r) => r.method + ' ' + r.path).slice(0, 6)));
   check('and the message is filed', since(beforePhish).some((r) => r.method !== 'GET' && /move|junk/.test(r.path + ' ' + (r.body || ''))),
      JSON.stringify(since(beforePhish).map((r) => r.method + ' ' + r.path).slice(0, 6)));

   // ---- safe senders and blocked senders, and the digest switch
   dismissAllToasts();
   location.hash = '#/senders';
   await flush();
   await flush();
   check('the safe-senders page is a settings page', !document.getElementById('senders-section').hidden && !document.getElementById('settings-head').hidden,
      document.getElementById('senders-section').hidden + ' ' + document.getElementById('settings-head').hidden);
   const senderRowsAtFirst = document.getElementById('sender-rows').children.length;
   check('the account\'s own list is listed', senderRowsAtFirst > 0 && document.getElementById('sender-rows').textContent.indexOf('friend@example.org') >= 0,
      document.getElementById('sender-rows').textContent);

   // Adding a domain: the whole point of the feature is that a person may name
   // one sender or every sender under a name, and the page must not insist on
   // an @.
   document.getElementById('sender-value').value = 'newsletters.example.net';
   document.getElementById('sender-kind').value = 'allow';
   document.getElementById('sender-reason').value = 'the invoices come from here';
   const beforeSenderAdd = requests.length;
   document.getElementById('sender-form').dispatchEvent(makeEvent('submit'));
   await flush();
   await flush();
   const senderAdded = since(beforeSenderAdd).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/sender-list').map((r) => JSON.parse(r.body))[0];
   check('a domain can be added as a safe sender, with the reason', !!senderAdded && senderAdded.value === 'newsletters.example.net' && senderAdded.blocked === false && senderAdded.reason === 'the invoices come from here',
      JSON.stringify(senderAdded));
   check('and the list is read back rather than guessed at', since(beforeSenderAdd).some((r) => r.method === 'GET' && r.path === '/api/v1/me/sender-list'),
      JSON.stringify(since(beforeSenderAdd).map((r) => r.method + ' ' + r.path)));
   check('so the new row is on the page', document.getElementById('sender-rows').children.length === senderRowsAtFirst + 1,
      String(document.getElementById('sender-rows').children.length));

   // The same address again, blocked this time. One row, changed - not two rows
   // arguing, which is what a list a person keeps by hand must never grow.
   document.getElementById('sender-value').value = 'newsletters.example.net';
   document.getElementById('sender-kind').value = 'block';
   document.getElementById('sender-reason').value = '';
   const beforeSenderAgain = requests.length;
   document.getElementById('sender-form').dispatchEvent(makeEvent('submit'));
   await flush();
   await flush();
   check('naming the same value again changes the row rather than adding a second', document.getElementById('sender-rows').children.length === senderRowsAtFirst + 1 && senderList.filter((e) => e.value === 'newsletters.example.net').length === 1,
      String(senderList.length));
   check('and it is now blocked', senderList.filter((e) => e.value === 'newsletters.example.net')[0].blocked === true,
      JSON.stringify(senderList));

   // Removing the row it had at the start, to prove the delete goes to the
   // entry's own id and not to the first row on the page.
   const beforeSenderRemove = requests.length;
   document.getElementById('sender-rows').children[0].children[3].children[1].dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   check('a row is removed by its own id', since(beforeSenderRemove).some((r) => r.method === 'DELETE' && r.path === '/api/v1/me/sender-list/401'),
      JSON.stringify(since(beforeSenderRemove).map((r) => r.method + ' ' + r.path)));

   // The digest switch is the reader's half of a decision the domain makes.
   check('the digest switch shows what the server said', document.getElementById('digest-enabled').checked === true && document.getElementById('digest-enabled').disabled === false,
      document.getElementById('digest-enabled').checked + ' ' + document.getElementById('digest-enabled').disabled);
   const beforeDigest = requests.length;
   document.getElementById('digest-enabled').checked = false;
   document.getElementById('digest-enabled').dispatchEvent(makeEvent('change'));
   await flush();
   await flush();
   const digestWrite = since(beforeDigest).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/quarantine-digest').map((r) => JSON.parse(r.body))[0];
   check('turning the digest off writes the reader\'s own opt-out', !!digestWrite && digestWrite.enabled === false,
      JSON.stringify(digestWrite));
   check('and the page says the mail is still held', document.getElementById('digest-status').textContent.toLowerCase().indexOf('still held') >= 0,
      document.getElementById('digest-status').textContent);

   // ---- a formatted signature: the editor, its tools, and the field the page saves
   location.hash = '#/settings';
   await flush();
   check('the settings have a formatted signature with tools and an image control', !!document.getElementById('signature-html') && document.getElementById('signature-tools').children.length >= 5 && !!document.getElementById('signature-image'),
      String(document.getElementById('signature-tools') && document.getElementById('signature-tools').children.length));
   const beforeSig = requests.length;
   document.getElementById('settings-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const sigPut = since(beforeSig).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/settings').map((r) => JSON.parse(r.body))[0];
   // The cleaner walks real DOM nodes (nodeType, childNodes), which this
   // harness's elements are not; what is checked here is that the page
   // sends the formatted signature as a field of its own.
   check('the formatted signature is saved as its own field', !!sigPut && typeof sigPut.signature.html === 'string', sigPut ? JSON.stringify(sigPut.signature) : 'no PUT');

   // ---- the signature's 64 KB, images included: the number the server refuses
   // above (RestApiServer.cpp) and MySQL's column holds, said before a file is picked
   const sigEditor = document.getElementById('signature-html');
   const sigImage = document.getElementById('signature-image');
   const sigStatus = document.getElementById('settings-status');
   const sigImages = () => sigEditor.children.filter((n) => n.tagName === 'IMG');
   const pageNotes = (function collect(node, out) {
      if (node instanceof Element && node.className === 'note') { out.push(node.textContent); }
      (node.childNodes || []).forEach((child) => collect(child, out));
      return out;
   })(document.body, []);
   check('the settings say how much the formatted signature holds before an image is chosen', pageNotes.some((n) => n.indexOf('64 KB, images included') >= 0), JSON.stringify(pageNotes.filter((n) => /signature/i.test(n))));
   // A real 1x1 PNG: the page reads a file's first bytes, as the server does, before it believes the type.
   const logo = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==';
   sigImage.files = [{ name: 'logo.svg', type: 'image/svg+xml', size: 40, base64: 'PHN2Zz4=' }];
   sigImage.dispatchEvent(makeEvent('change'));
   await flush();
   check('a picture that is not PNG, JPEG, GIF or WebP is refused', sigImages().length === 0 && /PNG, JPEG, GIF or WebP/.test(sigStatus.textContent), sigStatus.textContent);
   sigImage.files = [{ name: 'logo.png', type: 'image/png', size: 70, base64: logo }];
   sigImage.dispatchEvent(makeEvent('change'));
   await flush();
   check('a logo that fits goes into the signature as a data: URL', sigImages().length === 1 && sigImages()[0].getAttribute('src') === 'data:image/png;base64,' + logo && sigImage.value === '',
      sigImages().length + ' ' + sigStatus.textContent);
   sigImage.files = [{ name: 'logo.jpg', type: 'image/jpeg', size: 70, base64: logo }];
   sigImage.dispatchEvent(makeEvent('change'));
   await flush();
   check('a PNG named as a JPEG goes in as the PNG it is', sigImages().length === 2 && sigImages()[1].getAttribute('src') === 'data:image/png;base64,' + logo, sigImages().length + ' ' + (sigImages()[1] ? sigImages()[1].getAttribute('src').slice(0, 30) : ''));
   sigImage.files = [{ name: 'notes.png', type: 'image/png', size: 30, base64: 'VGhpcyBpcyBub3QgYSBwaWN0dXJlIGF0IGFsbA==' }];
   sigImage.dispatchEvent(makeEvent('change'));
   await flush();
   check('a file whose bytes are not an image is refused, whatever it is called', sigImages().length === 2 && /PNG, JPEG, GIF or WebP/.test(sigStatus.textContent), sigImages().length + ' ' + sigStatus.textContent);
   // No canvas in this DOM, so an image that does not fit cannot be drawn smaller: refused, with the room left.
   const photo = 'iVBORw0KGgoA' + 'A'.repeat(69988);
   sigImage.files = [{ name: 'photo.png', type: 'image/png', size: 52500, base64: photo }];
   sigImage.dispatchEvent(makeEvent('change'));
   await flush();
   check('an image that does not fit, and cannot be made smaller, is refused with how much is free', sigImages().length === 2 && /64 KB/.test(sigStatus.textContent) && /63 KB of it is free/.test(sigStatus.textContent) && sigStatus.className.indexOf('error') >= 0,
      sigImages().length + ' ' + sigStatus.textContent);
   // With a canvas: drawn at most 600 by 200, and smaller again until it fits.
   const drawn = [];
   const realCreateElement = document.createElement;
   document.createElement = function (tag) {
      const made = realCreateElement(tag);
      if (String(tag).toLowerCase() === 'canvas') {
         made.getContext = () => ({ clearRect() {}, drawImage() { drawn.push(made.width + 'x' + made.height); } });
         made.toDataURL = (type) => 'data:' + type + ';base64,' + 'A'.repeat(made.width * 150);
      }
      return made;
   };
   Object.defineProperty(globalThis, 'Image', { configurable: true, writable: true, value: class {
      set src(value) { this.given = value; this.naturalWidth = 1200; this.naturalHeight = 400; Promise.resolve().then(() => this.onload()); }
   } });
   sigImage.files = [{ name: 'banner.png', type: 'image/png', size: 52500, base64: photo }];
   sigImage.dispatchEvent(makeEvent('change'));
   await flush();
   document.createElement = realCreateElement;
   delete globalThis.Image;
   const shrunk = sigImages()[2];
   check('an image that does not fit is drawn at most 600 by 200 first, then smaller until it fits', drawn[0] === '600x200' && drawn.length === 3 && !!shrunk && shrunk.getAttribute('src').length <= 64000 - 19 && /made smaller/.test(sigStatus.textContent),
      JSON.stringify(drawn) + ' ' + (shrunk ? shrunk.getAttribute('src').length : 'none') + ' ' + sigStatus.textContent);
   // A signature over 64 KB - an image pasted in rather than chosen - is not sent at all.
   const pasted = document.createElement('img');
   pasted.setAttribute('src', 'data:image/png;base64,' + photo);
   sigEditor.appendChild(pasted);
   const beforeOver = requests.length;
   document.getElementById('settings-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('a formatted signature over 64 KB is not sent, and the page says why', !since(beforeOver).some((r) => r.path === '/api/v1/me/settings') && /over 64 KB/.test(sigStatus.textContent),
      sigStatus.textContent + ' ' + JSON.stringify(since(beforeOver).map((r) => r.method + ' ' + r.path)));
   sigEditor.removeChild(pasted);
   const beforeFits = requests.length;
   document.getElementById('settings-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const fitsPut = since(beforeFits).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/settings').map((r) => JSON.parse(r.body))[0];
   check('under it, the signature is sent with its images in it', !!fitsPut && fitsPut.signature.html.indexOf('<img src="data:image/png;base64,' + logo + '" alt="">') === 0 && fitsPut.signature.html.length <= 64000,
      fitsPut ? fitsPut.signature.html.length + ' ' + fitsPut.signature.html.slice(0, 80) : 'no PUT');
   sigImages().forEach((n) => sigEditor.removeChild(n));

   // ---- a draft reopens as its text, so the images this webmail made parts of
   // its HTML - a signature's logo - are not attached again; an image another
   // mail client embedded is, as it always was
   const beforeDraft = requests.length;
   location.hash = '#/compose?draft=105';
   await flush();
   await flush();
   const draftFetched = since(beforeDraft).filter((r) => /\/messages\/105\/attachments\/\d+$/.test(r.path)).map((r) => r.path.split('/').pop());
   const draftNote = document.getElementById('compose-files-note').textContent;
   check('a draft reopened carries its files, and not the images this webmail made parts of its HTML', draftFetched.join(',') === '0,3' && draftNote.indexOf('notes.txt') >= 0 && draftNote.indexOf('photo.jpg') >= 0 && draftNote.indexOf('image1.png') < 0,
      JSON.stringify(draftFetched) + ' ' + draftNote);
   document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
   await flush();
   const beforeOtherDraft = requests.length;
   location.hash = '#/compose?draft=102';
   await flush();
   await flush();
   const otherDraftFetched = since(beforeOtherDraft).filter((r) => /\/messages\/102\/attachments\/\d+$/.test(r.path)).map((r) => r.path.split('/').pop());
   check('a draft whose images another mail client embedded carries every one of them as a file', otherDraftFetched.join(',') === '0,1,2', JSON.stringify(otherDraftFetched));
   document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
   await flush();
   const beforeForwardImages = requests.length;
   location.hash = '#/compose?forward=105';
   await flush();
   await flush();
   const forwardFetched = since(beforeForwardImages).filter((r) => /\/messages\/105\/attachments\/\d+$/.test(r.path)).map((r) => r.path.split('/').pop());
   check('a forward, whose HTML does not go with it, still carries this webmail\'s images as files', forwardFetched.join(',') === '0,1,2,3', JSON.stringify(forwardFetched));
   document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
   await flush();
   dismissAllToasts();

   // ---- @mentions: the text asks the address book, the person goes to To
   location.hash = '#/compose';
   await flush();
   const mentionBox = document.getElementById('compose-text');
   mentionBox.value = 'Hello @ali';
   mentionBox.selectionStart = mentionBox.value.length;
   const beforeMention = requests.length;
   mentionBox.dispatchEvent(makeEvent('input'));
   fireTimers();
   await flush();
   check('an @ in the text asks the address book for the name', called(beforeMention, 'GET', '/api/v1/me/contacts?limit=8&q=ali'), JSON.stringify(since(beforeMention).map((r) => r.path).slice(0, 3)));
   mentionBox.dispatchEvent(makeEvent('keydown', { key: 'Enter' }));
   await flush();
   check('the person chosen is written as @Name and added to To', mentionBox.value === 'Hello @Alice Example ' && document.getElementById('compose-to').value.indexOf('alice@example.net') >= 0,
      mentionBox.value + ' | ' + document.getElementById('compose-to').value);
   document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
   await flush();

   // ---- rows coloured by a rule
   location.hash = '#/filters';
   await flush();
   document.getElementById('colour-field').value = 'from';
   document.getElementById('colour-text').value = 'b@example.net';
   document.getElementById('colour-value').value = '#ff0000';
   const beforeColour = requests.length;
   document.getElementById('colour-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const colourPut = since(beforeColour).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/preferences').map((r) => JSON.parse(r.body))[0];
   check('a colour rule is kept with the account', !!colourPut && /b@example\.net/.test(colourPut.row_colours) && /#ff0000/.test(colourPut.row_colours), JSON.stringify(colourPut));
   check('and listed on the Filters page', document.getElementById('colour-rows').children.length === 1 && document.getElementById('colour-rows').textContent.indexOf('b@example.net') >= 0,
      document.getElementById('colour-rows').textContent);
   location.hash = '#/f/1';
   await flush();
   const colouredRow = Array.from(document.getElementById('message-list').children).find((r) => (r.textContent || '').indexOf('Second') >= 0);
   check('the row the rule matches carries the colour', !!colouredRow && String(colouredRow.getAttribute('style') || '').indexOf('#ff0000') >= 0, colouredRow ? String(colouredRow.getAttribute('style')) : 'no row');
   const plainRow = Array.from(document.getElementById('message-list').children).find((r) => (r.textContent || '').indexOf('First') >= 0);
   check('and one it does not match does not', !plainRow || !String(plainRow.getAttribute('style') || '').length, plainRow ? String(plainRow.getAttribute('style')) : 'no row');

   // ---- multiple stars: the star pressed again cycles amber, red, none
   location.hash = '#/f/1';
   await flush();
   const starRow = Array.from(document.getElementById('message-list').children)[0];
   const starBtn = Array.from(starRow.children).find((c) => (c.className || '').indexOf('star') >= 0) || Array.from(starRow.children).flatMap((c) => Array.from(c.children || [])).find((c) => (c.className || '').indexOf('star') === 0);
   const stageBodies = [];
   for (let press = 0; press < 4; press += 1) {
      const before = requests.length;
      starBtn.dispatchEvent(makeEvent('click'));
      await flush();
      const call = since(before).filter((r) => r.method === 'PUT' && /\/flags$/.test(r.path))[0];
      stageBodies.push(call ? JSON.parse(call.body) : null);
   }
   const stagesRight = stageBodies.length === 4 && stageBodies.every(Boolean)
      && stageBodies[1].flagged === true && (stageBodies[1].keywords_add || []).join() === '$Star2'
      && stageBodies[2].flagged === true && (stageBodies[2].keywords_add || []).join() === '$Star3' && (stageBodies[2].keywords_remove || []).indexOf('$Star2') >= 0
      && stageBodies[3].flagged === false && (stageBodies[3].keywords_remove || []).length === 2;
   check('the star pressed again goes amber, then red, then off, as flag and keywords', stagesRight, JSON.stringify(stageBodies));

   // ---- one key, one meaning: s on an open message cycles the stars as the row does
   // Message 102 starts with no star and a label of the reader's own, which no
   // star may touch; the mock keeps the flag and the keywords as the server
   // does. The open message's row is watched as well: its star must follow.
   const kept102 = { flagged: !!flaggedById[102], keywords: (keywordsById[102] || []).slice() };
   const starStageOf = (button) => !button ? -1 : !button.classList.contains('on') ? 0 : button.classList.contains('s3') ? 3 : button.classList.contains('s2') ? 2 : 1;
   const starKeywordsOf = (id) => (keywordsById[id] || []).filter((k) => /^\$star/i.test(k));
   const flagsOf = (id) => (flaggedById[id] ? 'flagged' : 'unflagged') + ' [' + (keywordsById[id] || []).join(' ') + ']';
   const starPuts = (before, id) => since(before).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/messages/' + id + '/flags' && /"flagged"/.test(r.body || ''));
   const pressS = async (id) => {
      const before = requests.length;
      document.dispatchEvent(makeEvent('keydown', { key: 's', target: document.body }));
      await flush();
      return starPuts(before, id);
   };
   const openFresh = async (id) => { location.hash = '#/f/1'; await flush(); location.hash = '#/m/' + id; await flush(); };
   const secondRowStage = () => { const row = rowBySubject('Second'); return row ? starStageOf(starOf(row)) : -1; };
   flaggedById[102] = false;
   keywordsById[102] = ['Home'];
   await openFresh(102);
   const paneStar = document.getElementById('message-flag');
   check('the open message starts with no star', document.getElementById('message-view').hidden === false && starStageOf(paneStar) === 0 && secondRowStage() === 0,
      flagsOf(102) + ' pane ' + paneStar.className + ' row ' + secondRowStage());
   const wantedSteps = [{ stage: 1, flagged: true, stars: '' }, { stage: 2, flagged: true, stars: '$Star2' }, { stage: 3, flagged: true, stars: '$Star3' }, { stage: 0, flagged: false, stars: '' }];
   const openSteps = [];
   for (let press = 0; press < 4; press += 1) {
      const puts = await pressS(102);
      openSteps.push({ requests: puts.length, flagged: !!flaggedById[102], stars: starKeywordsOf(102).join(), label: (keywordsById[102] || []).indexOf('Home') >= 0, pane: starStageOf(paneStar), row: secondRowStage() });
   }
   check('s on an open message cycles it as the row does: a star, the second, the third, none - one request a press',
      openSteps.length === 4 && openSteps.every((s, i) => s.requests === 1 && s.pane === wantedSteps[i].stage && s.flagged === wantedSteps[i].flagged), JSON.stringify(openSteps));
   check('with the keywords right after every press, and the reader\'s own label left alone',
      openSteps.length === 4 && openSteps.every((s, i) => s.stars === wantedSteps[i].stars && s.label), JSON.stringify(openSteps));
   check('the star on the open message\'s row follows it', openSteps.length === 4 && openSteps.every((s, i) => s.row === wantedSteps[i].stage), JSON.stringify(openSteps.map((s) => s.row)));
   check('taken off from the open message, the star leaves no $Star keyword behind', !flaggedById[102] && starKeywordsOf(102).length === 0 && (keywordsById[102] || []).join() === 'Home', flagsOf(102));

   // A message an older page left with the flag cleared and $Star3 still on
   // it shows no star; the first press gives it the first star and takes the
   // leftover off in the same request.
   keywordsById[102] = ['$Star3', 'Home'];
   await openFresh(102);
   check('a $Star3 left under a cleared flag shows no star', starStageOf(paneStar) === 0, flagsOf(102) + ' ' + paneStar.className);
   const leftover = await pressS(102);
   check('and s gives it the first star, taking the leftover off in the one request', leftover.length === 1 && !!flaggedById[102] && starKeywordsOf(102).length === 0 && starStageOf(paneStar) === 1, flagsOf(102));

   // From the list: the row under the cursor at the third star; s takes the
   // flag and both keywords off in one request.
   flaggedById[102] = true;
   keywordsById[102] = ['$Star3', 'Home'];
   location.hash = '#/f/1';
   await flush();
   const secondAt = rows().indexOf(rowBySubject('Second'));
   for (let guard = 0; guard < 10 && onCursor() < secondAt; guard += 1) { document.dispatchEvent(makeEvent('keydown', { key: 'j', target: document.body })); }
   for (let guard = 0; guard < 10 && onCursor() > secondAt; guard += 1) { document.dispatchEvent(makeEvent('keydown', { key: 'k', target: document.body })); }
   check('the row at the third star shows it, under the cursor', secondAt >= 0 && onCursor() === secondAt && secondRowStage() === 3, 'row ' + secondAt + ' cursor ' + onCursor() + ' stage ' + secondRowStage() + ' ' + flagsOf(102));
   const fromRow = await pressS(102);
   const fromRowBody = fromRow.length ? JSON.parse(fromRow[0].body) : {};
   check('s on the row takes the star off: the flag and both $Star keywords in one request, none left behind',
      fromRow.length === 1 && fromRowBody.flagged === false && (fromRowBody.keywords_remove || []).indexOf('$Star2') >= 0 && (fromRowBody.keywords_remove || []).indexOf('$Star3') >= 0
      && !flaggedById[102] && starKeywordsOf(102).length === 0 && secondRowStage() === 0, JSON.stringify(fromRowBody) + ' ' + flagsOf(102) + ' stage ' + secondRowStage());

   // The row's menu names the star it will give, and gives it the same way.
   const menuSteps = [];
   for (let press = 0; press < 4; press += 1) {
      rowBySubject('Second').dispatchEvent(makeEvent('contextmenu', { clientX: 300, clientY: 200 }));
      const item = menuItem('star');
      const label = item ? item.textContent : 'no star item';
      const before = requests.length;
      if (item) { item.dispatchEvent(makeEvent('click', { target: item })); }
      await flush();
      menuSteps.push({ label, requests: starPuts(before, 102).length, flagged: !!flaggedById[102], stars: starKeywordsOf(102).join(), row: secondRowStage() });
   }
   check('the row menu offers Star, Second star, Third star and Unstar in turn, each a step of the same cycle',
      JSON.stringify(menuSteps.map((s) => s.label)) === JSON.stringify(['Star', 'Second star', 'Third star', 'Unstar'])
      && menuSteps.every((s, i) => s.requests === 1 && s.flagged === wantedSteps[i].flagged && s.stars === wantedSteps[i].stars && s.row === wantedSteps[i].stage), JSON.stringify(menuSteps));

   // Clear the flag of a follow-up at the third star: the stars' keywords go with it.
   flaggedById[102] = true;
   keywordsById[102] = ['$Star3', '$FollowUp', '$Due-2026-09-20', 'Home'];
   await openFresh(102);
   check('a third-starred follow-up shows the third star', starStageOf(paneStar) === 3, flagsOf(102) + ' ' + paneStar.className);
   document.getElementById('followup-clear').dispatchEvent(makeEvent('click'));
   await flush();
   check('and Clear the flag leaves no $Star keyword on it', !flaggedById[102] && starKeywordsOf(102).length === 0 && (keywordsById[102] || []).join() === 'Home' && starStageOf(paneStar) === 0, flagsOf(102));

   // The star over the open message is the same function as the row's: two
   // clicks give the first star and then the second, one request each - not
   // the flag on and off again, which is what it did before.
   flaggedById[102] = false;
   keywordsById[102] = ['Home'];
   await openFresh(102);
   const paneClicks = [];
   for (let press = 0; press < 2; press += 1) {
      const before = requests.length;
      paneStar.dispatchEvent(makeEvent('click'));
      await flush();
      paneClicks.push({ requests: starPuts(before, 102).length, stars: starKeywordsOf(102).join(), pane: starStageOf(paneStar), row: secondRowStage() });
   }
   check('the star over the open message gives the next star, not the flag alone',
      JSON.stringify(paneClicks) === JSON.stringify([{ requests: 1, stars: '', pane: 1, row: 1 }, { requests: 1, stars: '$Star2', pane: 2, row: 2 }]), JSON.stringify(paneClicks));

   // The palette's entry for the open message names the star it will give,
   // and gives it through the same cycle. The message is at the second star,
   // so the entry reads Third star and then Unstar.
   const paletteStarItem = () => {
      document.dispatchEvent(makeEvent('keydown', { key: 'k', ctrlKey: true, target: document.body }));
      const input = document.getElementById('palette-input');
      input.value = 'star';
      input.dispatchEvent(makeEvent('input'));
      return document.getElementById('palette-list').children[0];
   };
   const paletteSteps = [];
   for (let press = 0; press < 2; press += 1) {
      const item = paletteStarItem();
      const label = item ? item.childNodes[0].textContent : 'no entry';
      const before = requests.length;
      if (item) { item.dispatchEvent(makeEvent('mousedown', { target: item })); }
      await flush();
      paletteSteps.push({ label, requests: starPuts(before, 102).length, stars: starKeywordsOf(102).join(), pane: starStageOf(paneStar) });
   }
   check('the palette names the star it will give and gives it the same way',
      JSON.stringify(paletteSteps) === JSON.stringify([{ label: 'Third star', requests: 1, stars: '$Star3', pane: 3 }, { label: 'Unstar', requests: 1, stars: '', pane: 0 }]), JSON.stringify(paletteSteps));
   // Closed, whatever the palette did: the keys the checks below press are
   // ignored by the page while it is open, so a failure here would read as a
   // failure there instead.
   document.dispatchEvent(makeEvent('keydown', { key: 'Escape', target: document.body }));
   await flush();

   // A follow-up given a day to a message that shows no star gives it the
   // first star: a $Star3 an older page left under a cleared flag goes in the
   // same request, so the message does not come back at the third star.
   flaggedById[102] = false;
   keywordsById[102] = ['$Star3', 'Home'];
   await openFresh(102);
   const beforeFollowStar = requests.length;
   document.getElementById('followup-tomorrow').dispatchEvent(makeEvent('click'));
   await flush();
   const followStarPut = starPuts(beforeFollowStar, 102)[0];
   const followStarBody = followStarPut ? JSON.parse(followStarPut.body) : {};
   check('a follow-up on a message showing no star gives it the first star, taking a leftover keyword off with it',
      !!followStarPut && followStarBody.flagged === true
      && (followStarBody.keywords_remove || []).indexOf('$Star2') >= 0 && (followStarBody.keywords_remove || []).indexOf('$Star3') >= 0
      && starKeywordsOf(102).length === 0 && starStageOf(paneStar) === 1, JSON.stringify(followStarBody) + ' ' + flagsOf(102));
   dismissAllToasts();

   // Two presses before the first answer comes back: the second counts from
   // the star the first asked for, so the pair gives the second star rather
   // than asking twice for the first and stopping there.
   flaggedById[102] = false;
   keywordsById[102] = ['Home'];
   await openFresh(102);
   const beforeQuick = requests.length;
   document.dispatchEvent(makeEvent('keydown', { key: 's', target: document.body }));
   document.dispatchEvent(makeEvent('keydown', { key: 's', target: document.body }));
   await flush();
   const quickPresses = starPuts(beforeQuick, 102).map((r) => JSON.parse(r.body));
   check('two presses before the first answer comes back give the second star, not the first twice',
      quickPresses.length === 2 && (quickPresses[0].keywords_add || []).join() === '' && (quickPresses[1].keywords_add || []).join() === '$Star2'
      && !!flaggedById[102] && starKeywordsOf(102).join() === '$Star2' && starStageOf(paneStar) === 2 && secondRowStage() === 2,
      JSON.stringify(quickPresses) + ' ' + flagsOf(102) + ' row ' + secondRowStage());
   dismissAllToasts();

   // The label menu is not a way to star. It offers the reader's labels, and
   // the keywords this page keeps for itself - the two stars, the follow-up
   // and its day - are not among them: ticked off there, $Star3 came off the
   // message while the flag stayed, which drops it to the first star with
   // nothing repainted and the third star still showing.
   flaggedById[102] = true;
   keywordsById[102] = ['$Star3', '$FollowUp', '$Due-2026-09-20', 'Home'];
   await openFresh(102);
   const labelMenu = document.getElementById('label-menu');
   const beforeMenu = requests.length;
   document.getElementById('message-label').dispatchEvent(makeEvent('click'));
   await flush();
   const labelOffers = [];
   (function walk(n) {
      for (let i = 0; i < n.childNodes.length; i++) {
         const c = n.childNodes[i];
         if (c.tagName === 'INPUT' && c.type === 'checkbox') { labelOffers.push(String(c.getAttribute('aria-label'))); }
         if (c.childNodes) { walk(c); }
      }
   })(labelMenu);
   check('the label menu offers the reader\'s own labels and none of the page\'s $ keywords, so no star can be taken off behind the star\'s back',
      labelMenu.hidden === false && labelOffers.indexOf('Home') >= 0 && labelOffers.every((name) => name.charAt(0) !== '$'),
      JSON.stringify(labelOffers));
   check('and the message is still at its third star, with nothing asked of the server by opening the menu',
      starStageOf(paneStar) === 3 && starKeywordsOf(102).join() === '$Star3' && requests.length === beforeMenu, flagsOf(102) + ' ' + paneStar.className);

   // What a screen reader reads out on a row: the star the press will give.
   // aria-label wins over title, so a row left at "Star" for ever named the
   // wrong press for every star but the first.
   flaggedById[102] = false;
   keywordsById[102] = ['Home'];
   await openFresh(102);
   const starNames = () => { const row = rowBySubject('Second'); const s = row ? starOf(row) : null; return (s ? s.getAttribute('aria-label') : 'no row star') + ' / ' + paneStar.getAttribute('aria-label'); };
   const namedSteps = [starNames()];
   for (let press = 0; press < 4; press += 1) { await pressS(102); namedSteps.push(starNames()); }
   check('a row\'s star is read out as the star the press will give, as the open message\'s star is',
      JSON.stringify(namedSteps) === JSON.stringify(['Star / Star', 'Second star / Second star', 'Third star / Third star', 'Unstar / Unstar', 'Star / Star']),
      JSON.stringify(namedSteps));

   // A press whose work throws - an answer the page cannot use, a repaint
   // that fails - must not leave the press behind it chained to a promise
   // that will never run: that would stop this message's star for the rest
   // of the session, and silently. The next press starts again from the star
   // the message shows.
   flaggedById[102] = false;
   keywordsById[102] = ['Home'];
   await openFresh(102);
   brokenFlagsAnswerOnce = true;
   const brokePress = await pressS(102);
   const afterBroke = await pressS(102);
   brokenFlagsAnswerOnce = false;
   check('a press whose answer the page cannot use does not stop that message\'s star: the next press is sent, and paints',
      brokePress.length === 1 && afterBroke.length === 1 && !!flaggedById[102] && starKeywordsOf(102).join() === '$Star2'
      && starStageOf(paneStar) === 2 && secondRowStage() === 2,
      brokePress.length + '+' + afterBroke.length + ' ' + flagsOf(102) + ' pane ' + paneStar.className + ' row ' + secondRowStage());

   flaggedById[102] = kept102.flagged;
   keywordsById[102] = kept102.keywords;
   location.hash = '#/f/1';
   await flush();

   // ---- snooze presets and swipe actions as settings
   location.hash = '#/settings';
   await flush();
   document.getElementById('pref-swipe-right').value = 'read';
   document.getElementById('pref-swipe-left').value = 'none';
   document.getElementById('pref-snooze-hours').value = '5';
   document.getElementById('pref-snooze-hour').value = '8';
   const beforeSwipePrefs = requests.length;
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const swipePut = since(beforeSwipePrefs).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/preferences').map((r) => JSON.parse(r.body))[0];
   check('the swipe actions and the snooze presets are kept with the account', !!swipePut && swipePut.swipe_right === 'read' && swipePut.swipe_left === 'none' && swipePut.snooze_hours === '5' && swipePut.snooze_hour === '8',
      JSON.stringify(swipePut));
   location.hash = '#/m/102';
   await flush();
   check('the snooze menu offers the presets as set', document.getElementById('snooze-3h').textContent === 'in 5 hours' && document.getElementById('snooze-tomorrow').textContent === 'tomorrow at 8' && document.getElementById('snooze-week').textContent === 'next week at 8',
      document.getElementById('snooze-3h').textContent + ' | ' + document.getElementById('snooze-tomorrow').textContent);
   location.hash = '#/f/1';
   await flush();
   const swipeRow = Array.from(document.getElementById('message-list').children).find((r) => r.tagName !== 'DIV' || true);
   const beforeSwipe = requests.length;
   swipeRow.dispatchEvent(makeEvent('touchstart', { touches: [{ clientX: 100 }] }));
   swipeRow.dispatchEvent(makeEvent('touchend', { changedTouches: [{ clientX: 320 }] }));
   await flush();
   check('a swipe to the right does what was chosen for it', since(beforeSwipe).some((r) => r.method === 'PUT' && /\/flags$/.test(r.path) && /"seen":true/.test(r.body || '')),
      JSON.stringify(since(beforeSwipe).map((r) => r.method + ' ' + r.path).slice(0, 4)));
   const beforeSwipeLeft = requests.length;
   swipeRow.dispatchEvent(makeEvent('touchstart', { touches: [{ clientX: 320 }] }));
   swipeRow.dispatchEvent(makeEvent('touchend', { changedTouches: [{ clientX: 100 }] }));
   await flush();
   check('and a swipe set to nothing does nothing', !since(beforeSwipeLeft).some((r) => r.method !== 'GET'), JSON.stringify(since(beforeSwipeLeft).map((r) => r.method + ' ' + r.path).slice(0, 4)));
   location.hash = '#/settings';
   await flush();
   document.getElementById('pref-swipe-right').value = 'archive';
   document.getElementById('pref-swipe-left').value = 'delete';
   document.getElementById('pref-snooze-hours').value = '3';
   document.getElementById('pref-snooze-hour').value = '9';
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();

   // ---- trackers, counted: the remote-image notice says what it kept out
   location.hash = '#/m/102';
   await flush();
   check('the notice counts the remote images, their hosts and the pixels among them', document.getElementById('message-remote-count').textContent === '1 remote image(s) from 1 host(s) were not loaded; 1 look like tracking pixels.',
      document.getElementById('message-remote-count').textContent);
   check('and remembers the sender\'s habit in this browser', document.getElementById('message-remote-habit').textContent === 'This sender has used tracking pixels in 1 message(s).' && String(store.get('hmPortalTrackers')).indexOf('b@example.net') >= 0,
      document.getElementById('message-remote-habit').textContent);

   // ---- undo more: a contact removed and a rule saved come back from the toast
   location.hash = '#/contacts';
   await flush();
   const contactRows = document.getElementById('contact-rows');
   check('what the reader kept is the address book, and what they merely wrote to is offered apart as other contacts',
      contactRows.children.length === 2 && document.getElementById('contact-other').hidden === false && document.getElementById('contact-other-rows').children.length === 1 &&
      document.getElementById('contact-other-rows').textContent.indexOf('al@example.org') >= 0,
      contactRows.children.length + ' kept, other hidden=' + document.getElementById('contact-other').hidden);
   const carolRow = Array.from(contactRows.children).find((tr) => tr.textContent.indexOf('carol@example.org') >= 0);
   const carolAvatar = carolRow ? Array.from(carolRow.children[0].children).find((n) => n.className === 'avatar' && typeof n.src === 'string') : null;
   check('a card\'s photo is the avatar, from the contact\'s own photo route, and its birthday, phone and note are read beside it',
      !!carolAvatar && String(carolAvatar.src).indexOf('/api/v1/me/contacts/3/photo') >= 0 &&
      carolRow.textContent.indexOf('1985-04-12') >= 0 && carolRow.textContent.indexOf('+1 555 0100') >= 0 && carolRow.textContent.indexOf('Met at the conference.') >= 0,
      carolRow ? String(carolRow.textContent).slice(0, 300) : 'no row');
   const beforeKeep = requests.length;
   Array.from(document.getElementById('contact-other-rows').children[0].children[2].children).find((b) => b.textContent === 'Keep').dispatchEvent(makeEvent('click'));
   await flush();
   const keptBody = since(beforeKeep).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/contacts/2').map((r) => JSON.parse(r.body))[0];
   check('Keep asks the server to make the collected address a contact, and says so', !!keptBody && keptBody.source === 'manual' && document.getElementById('toasts').textContent.indexOf('Kept as a contact.') >= 0,
      JSON.stringify(keptBody) + ' | ' + document.getElementById('toasts').textContent);
   const removeButtons = Array.from(contactRows.children).map((tr) => Array.from(tr.children[3].children).find((b) => b.textContent === 'Remove'));
   const beforeContactRemove = requests.length;
   removeButtons[0].dispatchEvent(makeEvent('click'));
   await flush();
   check('removing a contact offers Undo', called(beforeContactRemove, 'DELETE', '/api/v1/me/contacts/1') && document.getElementById('toasts').textContent.indexOf('Contact removed.') >= 0 && document.getElementById('toasts').textContent.indexOf('Undo') >= 0,
      document.getElementById('toasts').textContent);
   const contactUndo = Array.from(document.getElementById('toasts').children[0].children).find((b) => b.textContent === 'Undo');
   const beforeContactUndo = requests.length;
   contactUndo.dispatchEvent(makeEvent('click'));
   await flush();
   const contactBack = since(beforeContactUndo).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/contacts').map((r) => JSON.parse(r.body))[0];
   check('and Undo adds the contact back as it was', !!contactBack && contactBack.address === 'alice@example.net' && contactBack.name === 'Alice Example', JSON.stringify(contactBack));
   location.hash = '#/filters';
   await flush();
   const scriptBefore = document.getElementById('filter-script').value;
   document.getElementById('rule-field').value = 'from';
   document.getElementById('rule-text').value = 'news@example.org';
   document.getElementById('rule-action').value = 'delete';
   document.getElementById('rule-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('saving a rule offers Undo', document.getElementById('toasts').textContent.indexOf('Rules saved.') >= 0 && document.getElementById('filter-script').value !== scriptBefore,
      document.getElementById('toasts').textContent);
   const ruleUndo = Array.from(document.getElementById('toasts').children[0].children).find((b) => b.textContent === 'Undo');
   const beforeRuleUndo = requests.length;
   ruleUndo.dispatchEvent(makeEvent('click'));
   await flush();
   const scriptBack = since(beforeRuleUndo).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/filters').map((r) => JSON.parse(r.body).script)[0];
   check('and Undo writes the rules back as they were', scriptBack === scriptBefore && document.getElementById('filter-script').value === scriptBefore, JSON.stringify(scriptBack).slice(0, 80));

   // ---- the reader's own time zone: every date the page shows, rendered in it
   location.hash = '#/m/102';
   await flush();
   const dateHere = document.getElementById('message-date').textContent;
   location.hash = '#/settings';
   await flush();
   const zoneSelect = document.getElementById('pref-timezone');
   check('the settings offer the zones the browser knows', zoneSelect.children.length > 20 && Array.from(zoneSelect.children).some((o) => o.value === 'Asia/Tokyo'), String(zoneSelect.children.length));
   zoneSelect.value = 'Asia/Tokyo';
   const beforeZone = requests.length;
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const zonePut = since(beforeZone).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/preferences')[0];
   check('the zone is kept with the account', !!zonePut && JSON.parse(zonePut.body).timezone === 'Asia/Tokyo', zonePut ? zonePut.body : 'no PUT');
   location.hash = '#/m/102';
   await flush();
   const expectedThere = new Date('2026-09-08 09:00').toLocaleString('en', { weekday: 'short', year: 'numeric', month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit', timeZone: 'Asia/Tokyo' });
   check('and the open message is dated in that zone', document.getElementById('message-date').textContent === expectedThere && document.getElementById('message-date').textContent !== dateHere,
      document.getElementById('message-date').textContent + ' vs ' + expectedThere + ' (was ' + dateHere + ')');
   location.hash = '#/settings';
   await flush();
   document.getElementById('pref-timezone').value = '';
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();

   // ---- saved searches: named, kept with the account, listed as folders, run afresh
   location.hash = '#/f/1';
   await flush();
   document.getElementById('search-tune').dispatchEvent(makeEvent('click'));
   await flush();
   document.getElementById('adv-from').value = 'boss@example.net';
   document.getElementById('adv-unread').checked = true;
   document.getElementById('adv-save-name').value = 'Unread from the boss';
   const beforeSave = requests.length;
   document.getElementById('adv-save').dispatchEvent(makeEvent('click'));
   await flush();
   const savedPut = since(beforeSave).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/preferences')[0];
   const savedList = savedPut ? JSON.parse(JSON.parse(savedPut.body).saved_searches) : [];
   check('a search saved with a name is kept with the account', savedList.length === 1 && savedList[0].name === 'Unread from the boss' && /from:boss@example\.net/.test(savedList[0].q) && /is:unread/.test(savedList[0].q),
      JSON.stringify(savedList));
   const savedEntry = Array.from(document.getElementById('folder-nav').children).find((b) => (b.getAttribute('data-route') || '').indexOf('saved=0') >= 0);
   check('and listed among the folders', !!savedEntry && savedEntry.textContent.indexOf('Unread from the boss') >= 0,
      savedEntry ? savedEntry.getAttribute('data-route') : 'no entry');
   const beforeSavedOpen = requests.length;
   savedEntry.dispatchEvent(makeEvent('click'));
   await flush();
   check('opening it runs the search again', called(beforeSavedOpen, 'GET', /\/api\/v1\/me\/search\?/) && document.getElementById('view-title').textContent === 'Unread from the boss' && savedEntry.classList.contains('on'),
      document.getElementById('view-title').textContent + ' | ' + JSON.stringify(since(beforeSavedOpen).map((r) => r.path).slice(0, 3)));

   // ---- contacts in and out: vCard and CSV out, either back in
   location.hash = '#/contacts';
   await flush();
   const contactsSection = document.getElementById('contacts-section');
   document.getElementById('contact-export-vcf').dispatchEvent(makeEvent('click'));
   await flush();
   const vcf = contactsSection.getAttribute('data-last-export') || '';
   check('the address book exports as vCard 3.0, one card per contact', /BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Alice Example\r\nN:Example;Alice;;;\r\nEMAIL;TYPE=INTERNET:alice@example.net\r\nEND:VCARD/.test(vcf) && (vcf.match(/BEGIN:VCARD/g) || []).length === 3 && vcf.indexOf('TEL:+1 555 0100') >= 0,
      vcf.slice(0, 160));
   document.getElementById('contact-export-csv').dispatchEvent(makeEvent('click'));
   await flush();
   const csv = contactsSection.getAttribute('data-last-export') || '';
   check('and as CSV with the header Outlook reads', csv.indexOf('Name,E-mail Address\r\nAlice Example,alice@example.net\r\n') === 0, csv.slice(0, 80));
   const beforeImport = requests.length;
   const importInput = document.getElementById('contact-import-file');
   importInput.files = [new File(['BEGIN:VCARD\r\nVERSION:3.0\r\nFN:New Person\r\nEMAIL:new@example.org\r\nEND:VCARD\r\nBEGIN:VCARD\r\nVERSION:3.0\r\nFN:Alice Again\r\nEMAIL:alice@example.net\r\nEND:VCARD\r\n'], 'contacts.vcf', { type: 'text/vcard' })];
   importInput.dispatchEvent(makeEvent('change'));
   await flush();
   const posted = since(beforeImport).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/contacts').map((r) => JSON.parse(r.body));
   check('a vCard file imports the contacts that are not there yet', posted.length === 1 && posted[0].address === 'new@example.org' && posted[0].name === 'New Person', JSON.stringify(posted));
   check('and says how many came in and how many were known', document.getElementById('contact-status').textContent === '1 contact(s) imported, 1 already there.', document.getElementById('contact-status').textContent);
   const beforeCsv = requests.length;
   importInput.files = [new File(['Name,Given Name,Family Name,E-mail 1 - Value,E-mail 2 - Value\r\n"Cara, Csv",Cara,Csv,cara@example.org,\r\n'], 'google.csv', { type: 'text/csv' })];
   importInput.dispatchEvent(makeEvent('change'));
   await flush();
   const postedCsv = since(beforeCsv).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/contacts').map((r) => JSON.parse(r.body));
   check('a Google Contacts CSV imports by its own header row, quoted names intact', postedCsv.length === 1 && postedCsv[0].address === 'cara@example.org' && postedCsv[0].name === 'Cara, Csv', JSON.stringify(postedCsv));

   // ---- the installed app: the badge, a mailto: link, a share, the handler
   if (app.hidden) {
      document.getElementById('address').value = 'user@example.com';
      document.getElementById('password').value = 'a-real-password';
      document.getElementById('signin-form').dispatchEvent(makeEvent('submit'));
      await flush();
   }
   const titled = (document.title.match(/^\((\d+)\)/) || [])[1];
   check('the installed icon carries the unread count the title carries', badges.length > 0 && (titled ? badges[badges.length - 1] === Number(titled) : badges[badges.length - 1] === 0),
      'title=' + document.title + ' badges=' + JSON.stringify(badges.slice(-3)));
   location.hash = '#/compose?mailto=' + encodeURIComponent('mailto:x@example.org?subject=Hi%20there&cc=c%40example.org&body=First%20line');
   await flush();
   check('a mailto: link opens a new message to its address', document.getElementById('compose-to').value === 'x@example.org', document.getElementById('compose-to').value);
   check('with its subject, its cc and its body', document.getElementById('compose-subject').value === 'Hi there' && document.getElementById('compose-cc').value === 'c@example.org' && document.getElementById('compose-text').value.indexOf('First line') === 0,
      document.getElementById('compose-subject').value + ' | ' + document.getElementById('compose-cc').value + ' | ' + document.getElementById('compose-text').value.slice(0, 30));
   const shareStore = await cachesStub.open('hm-portal-share');
   await shareStore.put('/portal/share-text', new Response(JSON.stringify({ title: 'A photo', text: 'Look at this', url: 'https://example.org/p', files: ['photo.png'] }), { headers: { 'Content-Type': 'application/json' } }));
   await shareStore.put('/portal/share-file/0', new Response(new Blob([Uint8Array.from([1, 2, 3])], { type: 'image/png' })));
   location.hash = '#/compose?share=1';
   await flush();
   check('a share from another app becomes a new message with its title and text', document.getElementById('compose-subject').value === 'A photo' && document.getElementById('compose-text').value.indexOf('Look at this\nhttps://example.org/p') === 0,
      document.getElementById('compose-subject').value + ' | ' + document.getElementById('compose-text').value.slice(0, 40));
   check('and the shared file attached', document.getElementById('compose-files-note').textContent.indexOf('photo.png') >= 0, document.getElementById('compose-files-note').textContent);
   check('and the share is read once', !cacheStores.has('hm-portal-share'));
   location.hash = '#/settings';
   await flush();
   document.getElementById('pref-mailto').dispatchEvent(makeEvent('click'));
   await flush();
   check('the mailto: handler is offered to the browser for this page', handlers.length === 1 && handlers[0].scheme === 'mailto' && handlers[0].url === 'http://portal.test/portal#/compose?mailto=%s',
      JSON.stringify(handlers));


   // ---- written right to left. Arabic, Hebrew and Persian mirror the page:
   // the root carries dir and lang, and the stylesheet's logical properties
   // follow. A message keeps its own direction whichever way the page runs,
   // so the panes it is shown in and the fields it is written in are dir=auto.
   const arabic = JSON.parse(fs.readFileSync(cataloguesDir + 'ar.json', 'utf8'));
   const german = JSON.parse(fs.readFileSync(cataloguesDir + 'de.json', 'utf8'));
   germanCatalogue = german;
   check('the Arabic catalogue holds Arabic, not the English', /[؀-ۿ]/.test(arabic['Keyboard shortcuts']) && arabic['Keyboard shortcuts'] !== 'Keyboard shortcuts',
      String(arabic['Keyboard shortcuts']));
   const beforeArabic = requests.length;
   document.getElementById('pref-language').value = 'ar';
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('the catalogue is fetched from the route the server answers', since(beforeArabic).filter((r) => r.path === '/portal-lang/ar.json').length === 1,
      JSON.stringify(since(beforeArabic).map((r) => r.path)));
   check('choosing Arabic writes the direction on the document', document.documentElement.getAttribute('dir') === 'rtl',
      String(document.documentElement.getAttribute('dir')));
   check('and the language with it', document.documentElement.getAttribute('lang') === 'ar',
      String(document.documentElement.getAttribute('lang')));
   check('a caption of the markup is what the Arabic catalogue says', document.getElementById('keys-title').textContent === arabic['Keyboard shortcuts'],
      document.getElementById('keys-title').textContent);
   check('and so is an attribute a screen reader reads out', document.getElementById('keys-btn').getAttribute('aria-label') === arabic['Keyboard shortcuts'],
      String(document.getElementById('keys-btn').getAttribute('aria-label')));
   check('a message body is left to find its own direction', document.getElementById('message-text').getAttribute('dir') === 'auto' && document.getElementById('message-html').getAttribute('dir') === 'auto',
      String(document.getElementById('message-text').getAttribute('dir')) + ' | ' + String(document.getElementById('message-html').getAttribute('dir')));
   check('and so is what the reader writes', document.getElementById('compose-subject').getAttribute('dir') === 'auto' && document.getElementById('compose-text').getAttribute('dir') === 'auto',
      String(document.getElementById('compose-subject').getAttribute('dir')) + ' | ' + String(document.getElementById('compose-text').getAttribute('dir')));
   document.getElementById('pref-language').value = 'de';
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('choosing German again puts the document back the other way', document.documentElement.getAttribute('dir') === 'ltr' && document.documentElement.getAttribute('lang') === 'de',
      String(document.documentElement.getAttribute('dir')) + ' ' + String(document.documentElement.getAttribute('lang')));
   check('and the caption is the German', document.getElementById('keys-title').textContent === german['Keyboard shortcuts'],
      document.getElementById('keys-title').textContent);

   // ---- a refusal the server gives a reason for, in the reader's language:
   // the password breach check's two, which arrive as English sentences with a
   // reason word beside them.
   const changePassword = async () => {
      document.getElementById('current').value = 'a-real-password';
      document.getElementById('new').value = 'password';
      document.getElementById('confirm').value = 'password';
      document.getElementById('password-form').dispatchEvent(makeEvent('submit'));
      await flush();
      return document.getElementById('password-status').textContent;
   };
   const breached = 'This password appears in a published data breach, so it would be among the first tried against this account. Choose a different one.';
   const unavailable = 'The password could not be checked against published data breaches just now, and this server is set to refuse a change it cannot check. Try again later.';
   passwordAnswer = { status: 400, body: { error: breached, reason: 'breached' } };
   let passwordSaid = await changePassword();
   check('German: a password found in a published breach is refused in German, not in the server\'s English',
      passwordSaid === german[breached] && german[breached] !== breached, passwordSaid);
   passwordAnswer = { status: 503, body: { error: unavailable, reason: 'breach_check_unavailable' } };
   passwordSaid = await changePassword();
   check('German: a change the breach check could not make is refused in German too', passwordSaid === german[unavailable] && german[unavailable] !== unavailable, passwordSaid);
   passwordAnswer = { status: 400, body: { error: 'A sentence the page has no word for.', reason: 'something_new' } };
   passwordSaid = await changePassword();
   check('a reason the page does not know shows the server\'s own sentence', passwordSaid === 'A sentence the page has no word for.', passwordSaid);
   document.getElementById('pref-language').value = 'ar';
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();
   passwordAnswer = { status: 400, body: { error: breached, reason: 'breached' } };
   passwordSaid = await changePassword();
   check('Arabic, written right to left: the same refusal in Arabic', passwordSaid === arabic[breached] && /[\u0600-\u06FF]/.test(passwordSaid), passwordSaid);
   passwordAnswer = null;
   document.getElementById('pref-language').value = 'de';
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();

   // ---- the tour: teaching the page while somebody uses it
   // Every check here is a way a tour can be wrong that nobody would notice
   // until they walked it: a step that hangs on something this page has hidden,
   // a tour that moves on before the reader has read it, a Back that closes it,
   // a finished record written for a walk somebody left, a tour that only works
   // in English. The record is kept with the ACCOUNT rather than with this
   // browser, so a reader who starts on a laptop finishes on a phone - which is
   // why what is checked below is a PUT and not a localStorage key.
   document.getElementById('pref-language').value = 'en';
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const tourCard = document.getElementById('tour-card');
   const tourRing = document.getElementById('tour-ring');
   const tourStepOf = () => document.getElementById('tour-step').textContent;
   const lastPrefs = (from) => {
      const puts = since(from).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/preferences');
      return puts.length ? JSON.parse(puts[puts.length - 1].body) : null;
   };

   // The compose window is left open by earlier sections of this file; the step
   // about it waits for it to be OPENED, and a condition already true when the
   // step arrives is deliberately not one that moves the tour on.
   if (document.getElementById('compose-section').hidden === false) {
      document.getElementById('compose-to').value = '';
      document.getElementById('compose-subject').value = '';
      document.getElementById('compose-text').value = '';
      document.getElementById('compose-close').dispatchEvent(makeEvent('click'));
      await flush();
   }
   check('the compose window is closed before the walk begins', document.getElementById('compose-section').hidden === true);

   document.getElementById('account-btn').dispatchEvent(makeEvent('click'));
   check('the header menu carries the way in', document.getElementById('account-menu').hidden === false &&
      document.getElementById('menu-tour').textContent.indexOf('Show me around') >= 0,
      document.getElementById('menu-tour').textContent);
   let beforeTour = requests.length;
   document.getElementById('menu-tour').dispatchEvent(makeEvent('click'));
   await flush();
   check('starting the tour opens the mail view and closes the menu',
      tourCard.hidden === false && document.getElementById('mail-section').hidden === false &&
      document.getElementById('account-menu').hidden === true, 'card=' + tourCard.hidden);
   check('the card names the tour and says where the reader is',
      document.getElementById('tour-name').textContent === 'Show me around' && tourStepOf() === 'Step 1 of 4',
      document.getElementById('tour-name').textContent + ' | ' + tourStepOf());
   check('and carries the step\'s own sentence',
      document.getElementById('tour-text').textContent.indexOf('Choose a message in the list') === 0,
      document.getElementById('tour-text').textContent);
   check('there is nowhere to go back to on the first stop', document.getElementById('tour-back').hidden === true);
   check('starting it asks the server for nothing of its own', !called(beforeTour, 'GET', /tour/),
      JSON.stringify(since(beforeTour).map((r) => r.path)));

   // The ring goes where the element is, the card under it.
   document.getElementById('pane-empty').rect = { left: 400, top: 120, width: 360, height: 40 };
   document.getElementById('tour-back').dispatchEvent(makeEvent('click'));
   await flush();
   check('Back on the first stop stands still rather than closing the tour',
      tourCard.hidden === false && tourStepOf() === 'Step 1 of 4', tourStepOf());
   check('the ring is drawn over the element the step points at',
      tourRing.hidden === false && tourRing.style.left === '400px' && tourRing.style.top === '120px' &&
      tourRing.style.width === '360px' && tourRing.style.height === '40px', JSON.stringify(tourRing.style));
   check('and the card is put under it rather than over it', tourCard.style.top === '172px', tourCard.style.top);

   // The reading-pane step waits for a message to be opened. Opening one hides
   // the very element the ring is on, which must take the ring away and move
   // the tour on rather than leave it pointing at nothing.
   // Opened through its own address, which is where a click on the row lands.
   location.hash = '#/m/102';
   await flush();
   check('opening a message hides what the step pointed at',
      document.getElementById('message-view').hidden === false && document.getElementById('pane-empty').hidden === true,
      'view=' + document.getElementById('message-view').hidden + ' empty=' + document.getElementById('pane-empty').hidden);
   fireIntervals();
   await flush();
   check('and the condition the step was waiting for moves the tour on by itself',
      tourStepOf() === 'Step 2 of 4' && document.getElementById('tour-text').textContent.indexOf('Compose opens') === 0,
      tourStepOf() + ' | ' + document.getElementById('tour-text').textContent);
   fireIntervals();
   await flush();
   check('a step whose condition has not happened yet is not moved on by anything but the reader',
      tourStepOf() === 'Step 2 of 4', tourStepOf());
   document.getElementById('nav-compose').dispatchEvent(makeEvent('click'));
   await flush();
   fireIntervals();
   await flush();
   check('opening the compose window ends the step about it', tourStepOf() === 'Step 3 of 4', tourStepOf());
   check('and Back is offered from the second stop on', document.getElementById('tour-back').hidden === false);

   // A stop whose element this page happens to be hiding is skipped rather
   // than waited on: half of this page is hidden at any moment, and a tour
   // that stopped at the first thing it could not see would stop at once.
   document.getElementById('tour-back').dispatchEvent(makeEvent('click'));
   await flush();
   check('Back returns to the stop before', tourStepOf() === 'Step 2 of 4', tourStepOf());
   document.getElementById('label-nav').hidden = true;
   document.getElementById('tour-next').dispatchEvent(makeEvent('click'));
   await flush();
   check('a stop whose element the page is hiding is skipped, and the tour carries on past it',
      document.getElementById('tour-card').hidden === false && tourStepOf() === 'Step 4 of 4', tourStepOf());
   document.getElementById('label-nav').hidden = false;
   document.getElementById('tour-back').dispatchEvent(makeEvent('click'));
   await flush();
   check('and Back finds it again once the page is showing it', tourStepOf() === 'Step 3 of 4', tourStepOf());
   document.getElementById('compose-to').value = '';
   document.getElementById('compose-subject').value = '';
   document.getElementById('compose-text').value = '';
   document.getElementById('compose-close').dispatchEvent(makeEvent('click'));
   await flush();

   // Leaving records where the reader got to, with the account.
   beforeTour = requests.length;
   document.getElementById('tour-skip').dispatchEvent(makeEvent('click'));
   await flush();
   check('leaving takes the ring and the card off the screen', tourCard.hidden === true && tourRing.hidden === true);
   check('and says so rather than vanishing in silence',
      document.getElementById('toasts').textContent.indexOf('picks up where you left off') > 0,
      document.getElementById('toasts').textContent);
   check('where it was left is kept with the account, in the form the other two surfaces keep it in',
      lastPrefs(beforeTour) && lastPrefs(beforeTour).tours === '|firstrun@2v1', JSON.stringify(lastPrefs(beforeTour)));
   document.getElementById('menu-tour').dispatchEvent(makeEvent('click'));
   await flush();
   check('and the next start picks it up there', tourStepOf() === 'Step 3 of 4', tourStepOf());

   // Escape belongs to the page unless the keyboard is already inside the card.
   document.getElementById('mail-search').focus();
   document.dispatchEvent(makeEvent('keydown', { key: 'Escape', target: document.getElementById('mail-search') }));
   await flush();
   check('Escape with the keyboard outside the card leaves the tour alone', tourCard.hidden === false);
   document.getElementById('tour-next').focus();
   document.dispatchEvent(makeEvent('keydown', { key: 'Escape', target: document.getElementById('tour-next') }));
   await flush();
   check('Escape inside the card leaves the tour', tourCard.hidden === true);

   // Walked to its end.
   document.getElementById('menu-tour').dispatchEvent(makeEvent('click'));
   await flush();
   check('the last stop offers Finish rather than promising another',
      tourStepOf() === 'Step 3 of 4' && document.getElementById('tour-next').textContent === 'Next', document.getElementById('tour-next').textContent);
   document.getElementById('tour-next').dispatchEvent(makeEvent('click'));
   await flush();
   check('the last stop is the search box, where a reader who learns nothing else should still end up',
      tourStepOf() === 'Step 4 of 4' && document.getElementById('tour-next').textContent === 'Finish',
      tourStepOf() + ' | ' + document.getElementById('tour-next').textContent);
   beforeTour = requests.length;
   document.getElementById('tour-next').dispatchEvent(makeEvent('click'));
   await flush();
   check('finishing says so', tourCard.hidden === true && document.getElementById('toasts').textContent.indexOf('That is the end of') >= 0,
      document.getElementById('toasts').textContent);
   check('and is recorded as finished, with no resume point left behind',
      lastPrefs(beforeTour) && lastPrefs(beforeTour).tours === 'firstrun=1|', JSON.stringify(lastPrefs(beforeTour)));

   // In the reader's own language, from the catalogues the page already speaks.
   document.getElementById('pref-language').value = 'de';
   document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
   await flush();
   document.getElementById('menu-tour').dispatchEvent(makeEvent('click'));
   await flush();
   check('the tour is drawn in the reader\'s language: its name',
      document.getElementById('tour-name').textContent === german['Show me around'], document.getElementById('tour-name').textContent);
   check('its sentence',
      document.getElementById('tour-text').textContent === german['Choose a message in the list and it opens here beside it. The More menu moves this pane, or turns it off.'],
      document.getElementById('tour-text').textContent);
   check('where the reader is, with its numbers',
      tourStepOf() === german['Step {0} of {1}'].replace('{0}', '1').replace('{1}', '4'), tourStepOf());
   check('and the card\'s own buttons, which the markup walk translates',
      document.getElementById('tour-next').textContent === german['Next'] && document.getElementById('tour-skip').textContent === german['Skip the tour'],
      document.getElementById('tour-next').textContent + ' | ' + document.getElementById('tour-skip').textContent);
   document.getElementById('tour-skip').dispatchEvent(makeEvent('click'));
   await flush();

   // ---- the devices card: every client that has signed in as this person, drawn
   // in the reader's language, which the tour above left at German
   location.hash = '#/security';
   await flush();
   const deviceRows = () => document.getElementById('device-rows').children;
   check('the security view lists what has signed in, one row per client',
      called(0, 'GET', '/api/v1/me/devices') && deviceRows().length === 2, deviceRows().length + ' rows');
   check('a row names the protocol, the address, the program and the credential - the app password by its name',
      deviceRows()[0].textContent.indexOf('imap') >= 0 && deviceRows()[0].textContent.indexOf('203.0.113.9') >= 0 &&
      deviceRows()[0].textContent.indexOf('Phone') >= 0 && deviceRows()[0].textContent.indexOf('-') >= 0,
      deviceRows()[0].textContent);
   check('and the client this page is marked as this browser',
      deviceRows()[1].textContent.indexOf(german['This browser']) >= 0 && deviceRows()[0].textContent.indexOf(german['This browser']) < 0,
      deviceRows()[1].textContent);
   check('the empty note is hidden while there are rows',
      document.getElementById('device-empty').hidden === true && document.getElementById('device-table').hidden === false);
   const beforeRevoke = requests.length;
   deviceRows()[0].children[5].children[0].dispatchEvent(makeEvent('click'));
   await flush();
   const revokeCall = since(beforeRevoke).filter((r) => r.method === 'DELETE')[0];
   check('revoking a client asks for that client by its id, and for nothing else',
      !!revokeCall && revokeCall.path === '/api/v1/me/devices/0123456789abcdef0123456789abcdef',
      revokeCall ? revokeCall.path : 'no DELETE');
   check('with the header a change on a session carries', !!revokeCall && revokeCall.headers['X-Requested-With'] === 'hMailServer');
   check('says what it did, and reads the list again',
      document.getElementById('device-status').textContent === german['Revoked. Anything it had open has been closed.'] && called(beforeRevoke, 'GET', '/api/v1/me/devices'),
      document.getElementById('device-status').textContent);
   document.getElementById('device-credential').checked = true;
   const beforeCredential = requests.length;
   deviceRows()[0].children[5].children[0].dispatchEvent(makeEvent('click'));
   await flush();
   const credentialCall = since(beforeCredential).filter((r) => r.method === 'DELETE')[0];
   check('with the box ticked the revoke asks for the app password behind the client too',
      !!credentialCall && credentialCall.path === '/api/v1/me/devices/0123456789abcdef0123456789abcdef?credential=revoke',
      credentialCall ? credentialCall.path : 'no DELETE');
   document.getElementById('device-credential').checked = false;

   {
   // ---- the calendar: month, week and day over the events route, the editor,
   // drag to move and drag to resize, the calendars beside it; the tasks; the
   // invitation card a message shows
   const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
   const wall = (d) => new Intl.DateTimeFormat('en-GB', { hourCycle: 'h23', hour: '2-digit', minute: '2-digit', timeZone: Intl.DateTimeFormat().resolvedOptions().timeZone }).format(d);
   const findAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
   const hasClass = (n, c) => String(n.className || '').split(/\s+/).indexOf(c) >= 0;
   const bodyOf = (from, method, pattern) => { const hit = since(from).filter((r) => r.method === method && (typeof pattern === 'string' ? r.path === pattern : pattern.test(r.path)))[0]; return hit ? JSON.parse(hit.body || '{}') : null; };
   const windowOf = (from) => { const hit = since(from).filter((r) => r.method === 'GET' && /^\/api\/v1\/me\/calendar\/events\?/.test(r.path)).pop(); if (!hit) { return null; } const m = /start=(\d{8}T\d{6}Z)&end=(\d{8}T\d{6}Z)/.exec(hit.path); return m ? { start: m[1], end: m[2], days: Math.round((utcOf(m[2]) - utcOf(m[1])) / 86400000) } : null; };
   const utcOf = (s) => Date.UTC(+s.slice(0, 4), +s.slice(4, 6) - 1, +s.slice(6, 8), +s.slice(9, 11), +s.slice(11, 13), +s.slice(13, 15));
   const beforeCalendar = requests.length;
   location.hash = '#/calendar';
   await flush();
   check('the calendar page reads the calendars and a window of events', called(beforeCalendar, 'GET', '/api/v1/me/calendars') && !!windowOf(beforeCalendar),
      JSON.stringify(since(beforeCalendar).map((r) => r.path).slice(0, 4)));
   const monthWindow = windowOf(beforeCalendar);
   check('the month view asks for six weeks', monthWindow && monthWindow.days === 42, JSON.stringify(monthWindow));
   check('the page says which zone the times are in', document.getElementById('cal-zone').textContent.indexOf(words('Times are shown in {0}.').split('{0}')[0]) === 0, document.getElementById('cal-zone').textContent);
   const calSection = document.getElementById('calendar-section');
   check('the month is a grid of 42 days', findAll(calSection, (n) => hasClass(n, 'cal-cell')).length === 42, findAll(calSection, (n) => hasClass(n, 'cal-cell')).length + ' cells');
   const chips = findAll(calSection, (n) => hasClass(n, 'cal-chip'));
   check('an occurrence is a chip on its day, in its calendar\'s colour, with its time', chips.length >= 2 && chips.some((c) => c.textContent.indexOf('Standup') >= 0 && c.style.background === '#1a73e8'),
      chips.map((c) => c.textContent + '|' + c.style.background).join(' ; '));
   check('the calendars are listed with a box each and the shared one named by its owner', findAll(document.getElementById('cal-list'), (n) => hasClass(n, 'cal-row')).length === 3 &&
      document.getElementById('cal-list').textContent.indexOf('Team (b@example.net)') >= 0, document.getElementById('cal-list').textContent);

   // ---- the week and the day
   const viewButton = (v) => findAll(document.getElementById('cal-views'), (n) => n.getAttribute('data-cal-view') === v)[0];
   let beforeWeek = requests.length;
   viewButton('week').dispatchEvent(makeEvent('click'));
   await flush();
   check('the week view asks for seven days', windowOf(beforeWeek) && windowOf(beforeWeek).days === 7, JSON.stringify(windowOf(beforeWeek)));
   check('and draws 24 hour slots in each of seven columns', findAll(calSection, (n) => hasClass(n, 'cal-col')).length === 7 && findAll(calSection, (n) => hasClass(n, 'cal-slot')).length === 24 * 7,
      findAll(calSection, (n) => hasClass(n, 'cal-col')).length + ' columns');
   const blocks = findAll(calSection, (n) => hasClass(n, 'cal-ev'));
   check('a timed occurrence is a block placed by its hour', blocks.length >= 1 && /px$/.test(blocks[0].style.top) && /px$/.test(blocks[0].style.height), blocks.length + ' blocks');
   const beforeDay = requests.length;
   viewButton('day').dispatchEvent(makeEvent('click'));
   await flush();
   check('the day view asks for one day and draws one column', windowOf(beforeDay) && windowOf(beforeDay).days === 1 && findAll(calSection, (n) => hasClass(n, 'cal-col')).length === 1, JSON.stringify(windowOf(beforeDay)));
   const beforeAgenda = requests.length;
   viewButton('agenda').dispatchEvent(makeEvent('click'));
   await flush();
   check('the agenda lists the occurrences by day', windowOf(beforeAgenda) && windowOf(beforeAgenda).days === 31 && findAll(calSection, (n) => hasClass(n, 'cal-agenda-row')).length >= 1, JSON.stringify(windowOf(beforeAgenda)));
   const beforeNext = requests.length;
   viewButton('month').dispatchEvent(makeEvent('click'));
   await flush();
   document.getElementById('cal-next').dispatchEvent(makeEvent('click'));
   await flush();
   check('Next moves a month on and reads it', windowOf(beforeNext) && utcOf(windowOf(beforeNext).start) > utcOf(monthWindow.start) + 20 * 86400000, JSON.stringify(windowOf(beforeNext)));
   document.getElementById('cal-today').dispatchEvent(makeEvent('click'));
   await flush();

   // ---- the editor: a new event with a rule, a reminder and guests
   document.getElementById('cal-new').dispatchEvent(makeEvent('click'));
   await flush();
   check('New event opens the editor', document.getElementById('event-overlay').hidden === false && document.getElementById('event-summary').value === '');
   document.getElementById('event-summary').value = 'Planning';
   document.getElementById('event-start').value = '2026-10-19T09:00';
   document.getElementById('event-end').value = '2026-10-19T10:00';
   document.getElementById('event-location').value = 'Room 4';
   document.getElementById('event-repeat').value = 'weekly';
   document.getElementById('event-repeat').dispatchEvent(makeEvent('change'));
   findAll(document.getElementById('event-days'), (n) => n.tagName === 'INPUT' && n.value === 'MO')[0].checked = true;
   document.getElementById('event-ends').value = 'after';
   document.getElementById('event-ends').dispatchEvent(makeEvent('change'));
   document.getElementById('event-count').value = '3';
   document.getElementById('event-alarm').value = '10';
   document.getElementById('event-attendees').value = 'Guest <guest@example.com>, b@example.net';
   const beforeSave = requests.length;
   document.getElementById('event-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const created = bodyOf(beforeSave, 'POST', '/api/v1/me/calendar/events');
   check('saving posts the event with its wall-clock times, the zone, the rule, the reminder and the guests, and asks for the invitations',
      !!created && created.summary === 'Planning' && created.start === '2026-10-19T09:00' && created.end === '2026-10-19T10:00' && typeof created.tz === 'string' && created.tz.length > 0 &&
      created.rrule === 'FREQ=WEEKLY;BYDAY=MO;COUNT=3' && created.alarm_minutes === 10 && created.location === 'Room 4' &&
      JSON.stringify(created.attendees) === '["Guest <guest@example.com>","b@example.net"]' && created.invite === true && created.all_day === false,
      JSON.stringify(created));
   check('the editor closes and the events are read again', document.getElementById('event-overlay').hidden === true && !!windowOf(beforeSave), JSON.stringify(since(beforeSave).map((r) => r.method + ' ' + r.path)));

   // ---- an occurrence opened: the editor shows its answer and offers the scope
   const standup = findAll(calSection, (n) => hasClass(n, 'cal-chip') && n.textContent.indexOf('Standup') >= 0)[0];
   standup.dispatchEvent(makeEvent('click'));
   await flush();
   check('opening a recurring occurrence offers this occurrence or the series, and shows the guests with their answers',
      document.getElementById('event-overlay').hidden === false && document.getElementById('event-scope-row').hidden === false && document.getElementById('event-summary').value === 'Standup' &&
      document.getElementById('event-attendee-list').textContent.indexOf('Guest Person') >= 0 && document.getElementById('event-attendee-list').textContent.indexOf(words('accepted')) >= 0,
      document.getElementById('event-attendee-list').textContent);
   document.getElementById('event-summary').value = 'Standup (moved)';
   const beforeOccurrence = requests.length;
   document.getElementById('event-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const occurrence = bodyOf(beforeOccurrence, 'PUT', '/api/v1/me/calendar/events/10');
   check('saving the occurrence PUTs the event with instance set to its recurrence id and no rule', !!occurrence && occurrence.instance === '20260921T090000Z' && occurrence.summary === 'Standup (moved)' && occurrence.rrule === undefined, JSON.stringify(occurrence));

   // ---- drag a chip to another day: the wall time is kept, the day changes
   confirmAnswer = false;   // the whole series
   const chipToDrag = findAll(calSection, (n) => hasClass(n, 'cal-chip') && n.textContent.indexOf('Standup') >= 0)[0];
   const fromCell = (() => { let n = chipToDrag.parentNode; while (n && !hasClass(n, 'cal-cell')) { n = n.parentNode; } return n; })();
   const cells = findAll(calSection, (n) => hasClass(n, 'cal-cell'));
   const targetCell = cells[cells.indexOf(fromCell) + 2];
   const beforeDrag = requests.length;
   chipToDrag.dispatchEvent(makeEvent('dragstart', { dataTransfer: { setData() {}, effectAllowed: '' } }));
   targetCell.dispatchEvent(makeEvent('dragover'));
   targetCell.dispatchEvent(makeEvent('drop'));
   await flush();
   const dragged = bodyOf(beforeDrag, 'PUT', '/api/v1/me/calendar/events/10');
   check('dropping the chip two days on moves the series to that day at the same time',
      !!dragged && dragged.start === targetCell.getAttribute('data-day') + 'T' + wall(calWall.start) && dragged.end === targetCell.getAttribute('data-day') + 'T' + wall(calWall.end) && dragged.instance === undefined && typeof dragged.tz === 'string',
      JSON.stringify(dragged) + ' onto ' + targetCell.getAttribute('data-day') + ' expected ' + wall(calWall.start) + '-' + wall(calWall.end));
   confirmAnswer = true;

   // ---- resize in the week view: the bottom edge dragged an hour down
   viewButton('week').dispatchEvent(makeEvent('click'));
   await flush();
   const handle = findAll(calSection, (n) => hasClass(n, 'cal-ev-resize'))[0];
   check('a writable block has a resize handle', !!handle);
   const beforeResize = requests.length;
   handle.dispatchEvent(makeEvent('mousedown', { clientY: 100, preventDefault() {}, stopPropagation() {} }));
   document.dispatchEvent(makeEvent('mousemove', { clientY: 148 }));
   document.dispatchEvent(makeEvent('mouseup', { clientY: 148 }));
   await flush();
   const resized = bodyOf(beforeResize, 'PUT', '/api/v1/me/calendar/events/10');
   check('releasing it an hour lower PUTs the end an hour later, for this occurrence', !!resized && resized.end === resized.start.slice(0, 11) + wall(new Date(calWall.end.getTime() + 3600000)) && resized.instance === '20260921T090000Z', JSON.stringify(resized) + ' expected ' + wall(new Date(calWall.end.getTime() + 3600000)));
   viewButton('month').dispatchEvent(makeEvent('click'));
   await flush();

   // ---- the calendars beside the grid: one added, one shared
   document.getElementById('cal-add-name').value = 'Projects';
   document.getElementById('cal-add-color').value = '#188038';
   const beforeCalAdd = requests.length;
   document.getElementById('cal-add-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const calAdded = bodyOf(beforeCalAdd, 'POST', '/api/v1/me/calendars');
   check('adding a calendar posts its name and colour and reads the list again', !!calAdded && calAdded.display_name === 'Projects' && calAdded.color === '#188038' && called(beforeCalAdd, 'GET', '/api/v1/me/calendars'), JSON.stringify(calAdded));
   const editButtons = findAll(document.getElementById('cal-list'), (n) => n.tagName === 'BUTTON');
   editButtons[1].dispatchEvent(makeEvent('click'));
   await flush();
   check('the calendar\'s details open with its name, its shares and the share form', document.getElementById('cal-detail').hidden === false && document.getElementById('cal-detail-name').value === 'Work' &&
      document.getElementById('cal-detail').textContent.indexOf('b@example.net') >= 0, document.getElementById('cal-detail').textContent.slice(0, 200));
   document.getElementById('cal-share-address').value = 'carol@example.org';
   document.getElementById('cal-share-write').checked = true;
   const beforeShare = requests.length;
   findAll(document.getElementById('cal-detail'), (n) => n.tagName === 'FORM')[1].dispatchEvent(makeEvent('submit'));
   await flush();
   const shared = bodyOf(beforeShare, 'PUT', '/api/v1/me/calendars/2/shares/carol%40example.org');
   check('sharing PUTs the address under the calendar with whether they may write', !!shared && shared.write === true, JSON.stringify(since(beforeShare).map((r) => r.method + ' ' + r.path)));
   const beforeHide = requests.length;
   const boxes = findAll(document.getElementById('cal-list'), (n) => n.tagName === 'INPUT' && n.type === 'checkbox');
   boxes[1].checked = false;
   boxes[1].dispatchEvent(makeEvent('change'));
   await flush();
   check('unticking a calendar hides its chips without another request and remembers it in this browser',
      findAll(calSection, (n) => hasClass(n, 'cal-chip') && n.textContent.indexOf('Standup') >= 0).length === 0 && since(beforeHide).length === 0 && String(store.get('hmPortalCalendarsHidden')).indexOf('2') >= 0,
      String(store.get('hmPortalCalendarsHidden')) + ' | ' + since(beforeHide).length + ' requests');
   boxes[1].checked = true;
   boxes[1].dispatchEvent(makeEvent('change'));
   await flush();

   // ---- tasks
   const beforeTasks = requests.length;
   location.hash = '#/tasks';
   await flush();
   check('the tasks page reads the tasks', called(beforeTasks, 'GET', '/api/v1/me/tasks'));
   const taskRows = () => findAll(document.getElementById('task-list'), (n) => hasClass(n, 'task-row'));
   check('open tasks are listed with their due day and the completed one is hidden until asked for', taskRows().length === 2 && document.getElementById('task-list').textContent.indexOf('Buy milk') >= 0 &&
      document.getElementById('task-list').textContent.indexOf('Old chore') < 0, document.getElementById('task-list').textContent);
   check('an overdue task is marked', taskRows().some((r) => hasClass(r, 'overdue')), taskRows().map((r) => r.className).join(','));
   document.getElementById('task-show-done').checked = true;
   document.getElementById('task-show-done').dispatchEvent(makeEvent('change'));
   check('showing completed tasks lists the done one struck through', taskRows().length === 3 && taskRows().some((r) => hasClass(r, 'done')));
   document.getElementById('task-add-summary').value = 'Call the bank';
   document.getElementById('task-add-due').value = '2026-09-25';
   const beforeTaskAdd = requests.length;
   document.getElementById('task-add-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const taskAdded = bodyOf(beforeTaskAdd, 'POST', '/api/v1/me/tasks');
   check('adding a task posts its title, due day and list', !!taskAdded && taskAdded.summary === 'Call the bank' && taskAdded.due === '2026-09-25' && typeof taskAdded.calendar === 'number', JSON.stringify(taskAdded));
   const milkRow = taskRows().filter((r) => r.textContent.indexOf('Buy milk') >= 0)[0];
   const milkBox = findAll(milkRow, (n) => n.tagName === 'INPUT')[0];
   const beforeTick = requests.length;
   milkBox.checked = true;
   milkBox.dispatchEvent(makeEvent('change'));
   await flush();
   const ticked = bodyOf(beforeTick, 'PUT', '/api/v1/me/tasks/21');
   check('ticking a task PUTs completed', !!ticked && ticked.completed === true, JSON.stringify(ticked));
   findAll(milkRow, (n) => hasClass(n, 'task-title'))[0].dispatchEvent(makeEvent('click'));
   await flush();
   check('a task opens in its editor with its notes', document.getElementById('task-overlay').hidden === false && document.getElementById('task-summary').value === 'Buy milk' && document.getElementById('task-notes').value === 'Two litres.');
   document.getElementById('task-priority').value = '1';
   const beforeTaskSave = requests.length;
   document.getElementById('task-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const taskSaved = bodyOf(beforeTaskSave, 'PUT', '/api/v1/me/tasks/21');
   check('saving it PUTs the priority and the due day', !!taskSaved && taskSaved.priority === 1 && taskSaved.due === '2026-09-20' && taskSaved.all_day === true, JSON.stringify(taskSaved));
   document.getElementById('task-show-done').checked = false;
   document.getElementById('task-show-done').dispatchEvent(makeEvent('change'));

   // ---- the invitation a message carries
   const beforeInvite = requests.length;
   location.hash = '#/m/106';
   await flush();
   check('a message marked as carrying an invitation asks for it', called(beforeInvite, 'GET', '/api/v1/me/messages/106/invitation'), JSON.stringify(since(beforeInvite).map((r) => r.path)));
   const inviteBox = document.getElementById('message-invitation');
   check('the card says what, when, where and who, and that it is not answered', inviteBox.hidden === false && inviteBox.textContent.indexOf('Board meeting') >= 0 && inviteBox.textContent.indexOf('Room 9') >= 0 &&
      inviteBox.textContent.indexOf('Sam Self') >= 0 && inviteBox.textContent.indexOf(words('Not answered yet.')) >= 0, inviteBox.textContent.slice(0, 300));
   const acceptButton = findAll(inviteBox, (n) => n.getAttribute('data-invite') === 'accept')[0];
   check('Accept, Maybe and Decline are offered', !!acceptButton && findAll(inviteBox, (n) => n.getAttribute('data-invite') === 'decline').length === 1);
   const beforeAccept = requests.length;
   acceptButton.dispatchEvent(makeEvent('click'));
   await flush();
   const answered = bodyOf(beforeAccept, 'POST', '/api/v1/me/messages/106/invitation/reply');
   check('Accept posts the answer and the card says the event is in the calendar', !!answered && answered.action === 'accept' && inviteBox.textContent.indexOf(words('You {0}.').replace('{0}', words('accepted'))) >= 0 && inviteBox.textContent.indexOf(words('It is in your calendar.')) >= 0,
      JSON.stringify(answered) + ' | ' + inviteBox.textContent.slice(0, 300));
   const proposeButton = findAll(inviteBox, (n) => n.tagName === 'BUTTON' && n.textContent === words('Propose a new time'))[0];
   check('the answered card still offers a new time', !!proposeButton, inviteBox.textContent.slice(0, 400) + ' | buttons: ' + findAll(inviteBox, (n) => n.tagName === 'BUTTON').map((b) => b.textContent).join(','));
   if (proposeButton) {
   proposeButton.dispatchEvent(makeEvent('click'));
   document.getElementById('invite-propose-start').value = '2026-10-19T11:00';
   document.getElementById('invite-propose-end').value = '2026-10-19T12:00';
   const beforeCounter = requests.length;
   findAll(inviteBox, (n) => n.tagName === 'BUTTON' && n.textContent === words('Send proposal'))[0].dispatchEvent(makeEvent('click'));
   await flush();
   const countered = bodyOf(beforeCounter, 'POST', '/api/v1/me/messages/106/invitation/reply');
   check('proposing a new time posts a counter with the wall-clock times and the zone', !!countered && countered.action === 'counter' && countered.start === '2026-10-19T11:00' && countered.end === '2026-10-19T12:00' && typeof countered.tz === 'string', JSON.stringify(countered));
   }
   location.hash = '#/m/102';
   await flush();
   check('a message without an invitation shows no card', document.getElementById('message-invitation').hidden === true);

   }

   // ---- the Subscriptions page: each sender that offers a way out, and the
   // one button that takes it
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const walkAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const subsRows = () => walkAll(document.getElementById('subs-rows'), (n) => n.tagName === 'TR');
      const rowOf = (address) => subsRows().filter((r) => r.textContent.indexOf(address) >= 0)[0];
      const buttonIn = (row, english) => walkAll(row, (n) => n.tagName === 'BUTTON' && n.textContent === words(english))[0];
      const beforeSubs = requests.length;
      location.hash = '#/subscriptions';
      await flush();
      check('the Subscriptions page reads the senders of the last ninety days', called(beforeSubs, 'GET', '/api/v1/me/subscriptions?days=90'),
         JSON.stringify(since(beforeSubs).map((r) => r.path)));
      check('it is a page of the settings, beside Safe senders', document.getElementById('subscriptions-section').hidden === false && document.getElementById('settings-head').hidden === false);
      check('a row per sender, the most first', subsRows().length === 3 && subsRows()[0].textContent.indexOf('news@example.com') >= 0, subsRows().map((r) => r.textContent).join(' | '));
      check('a row names the sender, the list and how much came',
         rowOf('news@example.com').textContent.indexOf('Weekly News') >= 0 && rowOf('news@example.com').textContent.indexOf('weekly.example.com') >= 0 &&
         rowOf('news@example.com').textContent.indexOf('12') >= 0 && rowOf('news@example.com').textContent.indexOf('4') >= 0, rowOf('news@example.com').textContent);
      const pageLink = walkAll(rowOf('blog@example.com'), (n) => n.tagName === 'A')[0];
      check('a list that offers only a page is a link to it, opened apart and told nothing of this page',
         !!pageLink && pageLink.getAttribute('href') === 'https://blog.example.com/settings' && pageLink.getAttribute('target') === '_blank' &&
         pageLink.getAttribute('rel') === 'noopener noreferrer' && !buttonIn(rowOf('blog@example.com'), 'Unsubscribe'), pageLink ? pageLink.getAttribute('href') : 'no link');

      const beforeLeave = requests.length;
      buttonIn(rowOf('news@example.com'), 'Unsubscribe').dispatchEvent(makeEvent('click'));
      await flush();
      check('Unsubscribe asks the newest message of the sender to be unsubscribed from', called(beforeLeave, 'POST', '/api/v1/me/messages/902/unsubscribe'),
         JSON.stringify(since(beforeLeave).map((r) => r.method + ' ' + r.path)));
      check('and says it was done, naming the sender', document.getElementById('subs-status').textContent.indexOf('news@example.com') >= 0, document.getElementById('subs-status').textContent);
      const leftPref = since(beforeLeave).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/preferences')[0];
      const leftMap = leftPref ? JSON.parse(JSON.parse(leftPref.body).unsubscribed || '{}') : {};
      check('what was left is kept with the account, with the day', /^\d{4}-\d{2}-\d{2}$/.test(leftMap['news@example.com'] || ''), leftPref ? leftPref.body : 'no PUT');
      check('and the row says so from then on, and offers to ask again',
         walkAll(rowOf('news@example.com'), (n) => String(n.className || '').indexOf('badge') >= 0).length === 1 && !!buttonIn(rowOf('news@example.com'), 'Unsubscribe again'),
         rowOf('news@example.com').textContent);

      const beforeMail = requests.length;
      buttonIn(rowOf('shop@example.com'), 'Unsubscribe').dispatchEvent(makeEvent('click'));
      await flush();
      check('a list that is left by mail says where the request went',
         called(beforeMail, 'POST', '/api/v1/me/messages/911/unsubscribe') && document.getElementById('subs-status').textContent.indexOf('shop-leave@example.com') >= 0,
         document.getElementById('subs-status').textContent);

      const beforeBlock = requests.length;
      buttonIn(rowOf('shop@example.com'), 'Block').dispatchEvent(makeEvent('click'));
      await flush();
      const blocked = since(beforeBlock).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/sender-list')[0];
      check('Block puts the sender on the reader\'s own blocked list', !!blocked && JSON.parse(blocked.body).value === 'shop@example.com' && JSON.parse(blocked.body).blocked === true,
         blocked ? blocked.body : 'no POST');

      confirmAnswer = false;
      const beforeNoDelete = requests.length;
      buttonIn(rowOf('blog@example.com'), 'Delete their mail').dispatchEvent(makeEvent('click'));
      await flush();
      check('deleting a sender\'s mail asks first, and a no deletes nothing', confirms[confirms.length - 1].indexOf('blog@example.com') >= 0 && !called(beforeNoDelete, 'GET', /^\/api\/v1\/me\/search/),
         confirms[confirms.length - 1]);
      confirmAnswer = true;
      const beforeDelete = requests.length;
      buttonIn(rowOf('blog@example.com'), 'Delete their mail').dispatchEvent(makeEvent('click'));
      await flush();
      check('a yes finds their mail in every folder and deletes what is from that address alone',
         called(beforeDelete, 'GET', /^\/api\/v1\/me\/search\?q=from%3Ablog%40example\.com/) &&
         subscriptionDeletes.join(',') === '931,932', subscriptionDeletes.join(','));
      check('and the page reads the senders again', called(beforeDelete, 'GET', '/api/v1/me/subscriptions?days=90'), JSON.stringify(since(beforeDelete).map((r) => r.path)));

      const beforeDays = requests.length;
      subscriptionsMode = 'partial';
      document.getElementById('subs-days').value = '30';
      document.getElementById('subs-days').dispatchEvent(makeEvent('change'));
      await flush();
      check('another window reads that window', called(beforeDays, 'GET', '/api/v1/me/subscriptions?days=30'), JSON.stringify(since(beforeDays).map((r) => r.path)));
      check('and when not all of it was read, the page says how much was',
         document.getElementById('subs-partial').hidden === false && document.getElementById('subs-partial').textContent.indexOf('5000') >= 0, document.getElementById('subs-partial').textContent);

      subscriptionsMode = 'empty';
      document.getElementById('subs-days').dispatchEvent(makeEvent('change'));
      await flush();
      check('with nothing to leave the page says so rather than showing an empty table',
         document.getElementById('subs-empty').hidden === false && document.getElementById('subs-table').hidden === true && document.getElementById('subs-partial').hidden === true);

      subscriptionsMode = 'refused';
      document.getElementById('subs-days').dispatchEvent(makeEvent('change'));
      await flush();
      check('a refused read is said as an error', String(document.getElementById('subs-status').className).indexOf('error') >= 0, document.getElementById('subs-status').className);
      subscriptionsMode = 'full';
      document.getElementById('subs-days').value = '90';
   }

   // ---- brand logos: the avatar of mail the server judged dmarc=pass for a brand
   {
      const findIn = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const brandImages = (root) => findIn(root, (n) => n.tagName === 'IMG' && String(n.className || '') === 'brand');
      location.hash = '#/f/1';
      await flush();
      const listRows = () => Array.from(document.getElementById('message-list').children);
      const branded = listRows().filter((r) => r.textContent.indexOf('Second') >= 0)[0];
      const plain = listRows().filter((r) => r.textContent.indexOf('First') >= 0)[0];
      const logos = branded ? brandImages(branded) : [];
      check('a row the server names a brand for shows the brand\'s logo in its avatar, from the logo route',
         logos.length === 1 && logos[0].getAttribute('src') === '/api/v1/me/brand-logo?domain=example.net' && logos[0].getAttribute('alt') === '',
         logos.length ? logos[0].getAttribute('src') : 'no logo in ' + (branded ? branded.textContent : 'no row'));
      check('and a row with no brand keeps its initials', !!plain && brandImages(plain).length === 0);
      if (logos.length) {
         const holder = logos[0].parentNode;
         logos[0].dispatchEvent(makeEvent('error'));
         check('a logo the server will not give puts the initials back', brandImages(branded).length === 0 && !!holder && holder.textContent.length > 0, holder ? holder.textContent : 'no holder');
      }
      location.hash = '#/m/102';
      await flush();
      const paneLogos = brandImages(document.getElementById('message-avatar'));
      check('the open message\'s avatar shows the logo too', paneLogos.length === 1 && paneLogos[0].getAttribute('src') === '/api/v1/me/brand-logo?domain=example.net', String(paneLogos.length));
      location.hash = '#/m/106';
      await flush();
      check('and the next message without a brand has its initials, not the last one\'s logo', brandImages(document.getElementById('message-avatar')).length === 0 && document.getElementById('message-avatar').textContent.length > 0,
         document.getElementById('message-avatar').textContent);
      location.hash = '#/f/1';
      await flush();
   }

   // ---- Other accounts: another mailbox collected into this one
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const walkAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const fetchRows = () => walkAll(document.getElementById('fetch-rows'), (n) => n.tagName === 'TR');
      const rowNamed = (name) => fetchRows().filter((r) => r.textContent.indexOf(name) >= 0)[0];
      const buttonIn = (row, english) => walkAll(row, (n) => n.tagName === 'BUTTON' && n.textContent === words(english))[0];
      const bodyOf = (from, method, path) => { const hit = since(from).filter((r) => r.method === method && r.path === path)[0]; return hit ? JSON.parse(hit.body || '{}') : null; };
      const beforeAccounts = requests.length;
      location.hash = '#/accounts';
      await flush();
      check('the Other accounts page reads the reader\'s other accounts', called(beforeAccounts, 'GET', '/api/v1/me/fetch-accounts'));
      check('it is a page of the settings', document.getElementById('accounts-section').hidden === false && document.getElementById('settings-head').hidden === false);
      check('a row per account, with where it collects from', fetchRows().length === 2 && rowNamed('Old provider').textContent.indexOf('pop.old-provider.example:995') >= 0,
         fetchRows().map((r) => r.textContent).join(' | '));
      check('an account an administrator set up is shown and not offered for a change',
         !!rowNamed('Catch-all') && !buttonIn(rowNamed('Catch-all'), 'Edit') && !buttonIn(rowNamed('Catch-all'), 'Remove') && !!buttonIn(rowNamed('Catch-all'), 'Collect now'),
         rowNamed('Catch-all') ? rowNamed('Catch-all').textContent : 'no row');

      document.getElementById('fetch-preset').value = 'gmail';
      document.getElementById('fetch-preset').dispatchEvent(makeEvent('change'));
      await flush();
      check('choosing Gmail fills its server and says what Gmail asks for',
         document.getElementById('fetch-host').value === 'imap.gmail.com' && document.getElementById('fetch-port').value === '993' &&
         document.getElementById('fetch-type').value === 'imap' && document.getElementById('fetch-security').value === 'tls' &&
         document.getElementById('fetch-hint').hidden === false && document.getElementById('fetch-name').value === 'Gmail',
         document.getElementById('fetch-host').value + ' ' + document.getElementById('fetch-hint').textContent);

      document.getElementById('fetch-name').value = '';
      document.getElementById('fetch-host').value = '';
      const beforeEmpty = requests.length;
      document.getElementById('fetch-form').dispatchEvent(makeEvent('submit'));
      await flush();
      check('an account with no name or server is refused here and never sent', !called(beforeEmpty, 'POST', '/api/v1/me/fetch-accounts') &&
         String(document.getElementById('fetch-status').className).indexOf('error') >= 0);

      document.getElementById('fetch-name').value = 'Gmail';
      document.getElementById('fetch-host').value = 'imap.gmail.com';
      document.getElementById('fetch-user').value = 'me@gmail.example';
      document.getElementById('fetch-password').value = 'app-password';
      document.getElementById('fetch-keep').value = '-1';
      document.getElementById('fetch-mirror').checked = true;
      const beforeAdd = requests.length;
      document.getElementById('fetch-form').dispatchEvent(makeEvent('submit'));
      await flush();
      const added = bodyOf(beforeAdd, 'POST', '/api/v1/me/fetch-accounts');
      check('adding sends the server, the account, the password, what to leave there and the folders',
         !!added && added.server_address === 'imap.gmail.com' && added.port === 993 && added.server_type === 'imap' && added.connection_security === 'tls' &&
         added.username === 'me@gmail.example' && added.password === 'app-password' && added.days_to_keep_messages === -1 && added.mirror_folders === true &&
         added.minutes_between_fetch === 30, JSON.stringify(added));
      check('and the page lists it and clears the password', fetchRows().length === 3 && document.getElementById('fetch-password').value === '');

      const beforeCollect = requests.length;
      buttonIn(rowNamed('Old provider'), 'Collect now').dispatchEvent(makeEvent('click'));
      await flush();
      check('Collect now asks that account to collect', called(beforeCollect, 'POST', '/api/v1/me/fetch-accounts/71/download') &&
         document.getElementById('fetch-status').textContent.indexOf('Old provider') >= 0, document.getElementById('fetch-status').textContent);

      const beforePause = requests.length;
      buttonIn(rowNamed('Old provider'), 'Pause').dispatchEvent(makeEvent('click'));
      await flush();
      const paused = bodyOf(beforePause, 'PUT', '/api/v1/me/fetch-accounts/71');
      check('Pause switches it off and the row says so', !!paused && paused.enabled === false && !!buttonIn(rowNamed('Old provider'), 'Resume'), JSON.stringify(paused));

      buttonIn(rowNamed('Old provider'), 'Edit').dispatchEvent(makeEvent('click'));
      await flush();
      check('Edit puts the account in the form, the password left blank',
         document.getElementById('fetch-host').value === 'pop.old-provider.example' && document.getElementById('fetch-port').value === '995' &&
         document.getElementById('fetch-password').value === '' && document.getElementById('fetch-cancel').hidden === false);
      document.getElementById('fetch-port').value = '110';
      const beforeSave = requests.length;
      document.getElementById('fetch-form').dispatchEvent(makeEvent('submit'));
      await flush();
      const saved = bodyOf(beforeSave, 'PUT', '/api/v1/me/fetch-accounts/71');
      check('saving it changes it where it is, and a password left blank is not sent, so the one saved stays',
         !!saved && saved.port === 110 && !('password' in saved) && !called(beforeSave, 'POST', '/api/v1/me/fetch-accounts'), JSON.stringify(saved));

      confirmAnswer = true;
      const beforeRemove = requests.length;
      buttonIn(rowNamed('Old provider'), 'Remove').dispatchEvent(makeEvent('click'));
      await flush();
      check('Remove asks, saying the mail collected stays, and removes it', confirms[confirms.length - 1].indexOf('Old provider') >= 0 &&
         called(beforeRemove, 'DELETE', '/api/v1/me/fetch-accounts/71') && !rowNamed('Old provider'), confirms[confirms.length - 1]);

      fetchMode = 'off';
      location.hash = '#/settings';
      await flush();
      location.hash = '#/accounts';
      await flush();
      check('a server that does not offer it says so and shows no form',
         document.getElementById('fetch-off').hidden === false && document.getElementById('fetch-form').hidden === true);
      fetchMode = 'on';
   }

   // ---- Importing with a sign-in: the provider, the folders the reader picks
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const walkAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const fetchRows = () => walkAll(document.getElementById('fetch-rows'), (n) => n.tagName === 'TR');
      const rowNamed = (name) => fetchRows().filter((r) => r.textContent.indexOf(name) >= 0)[0];
      const buttonIn = (row, english) => walkAll(row, (n) => n.tagName === 'BUTTON' && n.textContent === words(english) && !n.hidden)[0];
      const bodyOf = (from, method, path) => { const hit = since(from).filter((r) => r.method === method && r.path === path)[0]; return hit ? JSON.parse(hit.body || '{}') : null; };

      fetchAccountsState.push({ id: 90, name: 'Gmail - reader@gmail.example', server_address: 'imap.gmail.com', port: 993, server_type: 'imap', username: 'reader@gmail.example',
         minutes_between_fetch: 30, days_to_keep_messages: -1, connection_security: 'tls', mime_recipient_headers: '', enabled: true,
         process_mime_recipients: false, process_mime_date: false, use_antispam: true, use_antivirus: true, enable_route_recipients: false,
         mirror_folders: true, locked: false, next_download_time: '2026-09-17 14:00:00', oauth_provider: 'google', mirror_pending: true,
         mirror_seen: ['INBOX', 'Receipts', '[Gmail]/Alle Nachrichten', '[Gmail]/Markiert'], mirror_copies: ['[Gmail]/Alle Nachrichten', '[Gmail]/Markiert'], mirror_skip: [], collected_messages: 0 });

      const beforeOffer = requests.length;
      location.hash = '#/settings';
      await flush();
      location.hash = '#/accounts?imported=90';
      await flush();
      check('the page asks which providers it can sign in to', called(beforeOffer, 'GET', '/api/v1/me/fetch-accounts/import'));
      check('Gmail is offered and Outlook.com, not set up, is not',
         document.getElementById('import-signin').hidden === false && document.getElementById('import-google').hidden === false && document.getElementById('import-microsoft').hidden === true);
      check('back from the provider, the page says it is signed in and puts the address back',
         String(document.getElementById('fetch-status').className).indexOf('ok') >= 0 && location.hash === '#/accounts', location.hash + ' ' + document.getElementById('fetch-status').textContent);
      const signedIn = rowNamed('reader@gmail.example');
      check('an account signed in with Google says so, and offers no edit of its server', !!signedIn &&
         signedIn.textContent.indexOf(words('Signed in with Google')) >= 0 && !buttonIn(signedIn, 'Edit') && !!buttonIn(signedIn, 'Choose the folders'),
         signedIn ? signedIn.textContent : 'no row');

      buttonIn(rowNamed('reader@gmail.example'), 'Choose the folders').dispatchEvent(makeEvent('click'));
      await flush();
      const boxes = walkAll(document.getElementById('fetch-folder-list'), (n) => n.tagName === 'INPUT');
      const box = (name) => boxes.filter((b) => b.getAttribute('data-folder') === name)[0];
      check('the folders the other server listed are offered, the views of the others unticked whatever they are called',
         document.getElementById('fetch-folders').hidden === false && boxes.length === 4 && box('INBOX').checked && box('Receipts').checked &&
         !box('[Gmail]/Alle Nachrichten').checked && !box('[Gmail]/Markiert').checked, boxes.map((b) => b.getAttribute('data-folder') + '=' + b.checked).join(' '));

      box('Receipts').checked = false;
      const beforeChoice = requests.length;
      document.getElementById('fetch-folders-save').dispatchEvent(makeEvent('click'));
      await flush();
      const choice = bodyOf(beforeChoice, 'PUT', '/api/v1/me/fetch-accounts/90');
      check('bringing them sends the folders not ticked, and starts the collection',
         !!choice && JSON.stringify(choice.mirror_skip) === JSON.stringify(['Receipts', '[Gmail]/Alle Nachrichten', '[Gmail]/Markiert']) &&
         called(beforeChoice, 'POST', '/api/v1/me/fetch-accounts/90/download') && document.getElementById('fetch-folders').hidden === true, JSON.stringify(choice));

      document.getElementById('import-mirror').checked = true;
      document.getElementById('import-keep').value = '-1';
      const beforeStart = requests.length;
      document.getElementById('import-google').dispatchEvent(makeEvent('click'));
      await flush();
      const start = bodyOf(beforeStart, 'POST', '/api/v1/me/fetch-accounts/import');
      check('Sign in to Gmail starts the sign-in with the reader\'s choices and goes to the provider',
         !!start && start.provider === 'google' && start.mirror_folders === true && start.days_to_keep_messages === -1 &&
         navigatedTo[navigatedTo.length - 1] === 'https://accounts.google.example/auth?state=abc&code_challenge=xyz', JSON.stringify(start) + ' ' + navigatedTo.join(' '));

      location.hash = '#/settings';
      await flush();
      location.hash = '#/accounts?import_error=' + encodeURIComponent('this sign-in was started in another browser');
      await flush();
      check('a sign-in that did not finish is said, with the server\'s reason',
         String(document.getElementById('fetch-status').className).indexOf('error') >= 0 &&
         document.getElementById('fetch-status').textContent.indexOf('this sign-in was started in another browser') >= 0, document.getElementById('fetch-status').textContent);
      fetchAccountsState.pop();
   }

   // ---- Send mail as: an address elsewhere, proved with a code
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const walkAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const items = () => walkAll(document.getElementById('sendas-list'), (n) => n.tagName === 'LI');
      const itemFor = (address) => items().filter((li) => li.textContent.indexOf(address) >= 0)[0];
      const buttonIn = (row, english) => walkAll(row, (n) => n.tagName === 'BUTTON' && n.textContent === words(english))[0];
      const status = () => document.getElementById('sendas-status');

      const beforeLoad = requests.length;
      location.hash = '#/settings';
      await flush();
      location.hash = '#/accounts';
      await flush();
      check('the Other accounts page reads the addresses sent as', called(beforeLoad, 'GET', '/api/v1/me/sender-addresses') && document.getElementById('sendas').hidden === false);
      const waiting = itemFor('me@strict.example');
      check('an address waiting for its code offers the code, a new one and removal, and says its domain may refuse the mail',
         !!waiting && !!buttonIn(waiting, 'Verify') && !!buttonIn(waiting, 'Send a new code') && !!buttonIn(waiting, 'Remove') &&
         walkAll(waiting, (n) => n.getAttribute && n.getAttribute('data-sendas-code') === '5').length === 1 && waiting.textContent.indexOf('strict.example') >= 0,
         waiting ? waiting.textContent : 'no item');
      const proved = itemFor('old@relaxed.example');
      check('a verified address offers only its removal', !!proved && !buttonIn(proved, 'Verify') && !!buttonIn(proved, 'Remove'), proved ? proved.textContent : 'no item');

      walkAll(itemFor('me@strict.example'), (n) => n.getAttribute && n.getAttribute('data-sendas-code') === '5')[0].value = '000000';
      buttonIn(itemFor('me@strict.example'), 'Verify').dispatchEvent(makeEvent('click'));
      await flush();
      check('a wrong code says how many tries are left', String(status().className).indexOf('error') >= 0 && status().textContent.indexOf('4') >= 0, status().textContent);

      walkAll(itemFor('me@strict.example'), (n) => n.getAttribute && n.getAttribute('data-sendas-code') === '5')[0].value = ' 123456 ';
      const beforeVerify = requests.length;
      buttonIn(itemFor('me@strict.example'), 'Verify').dispatchEvent(makeEvent('click'));
      await flush();
      const verifyBody = since(beforeVerify).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/sender-addresses/5/verify')[0];
      check('the right code, trimmed, verifies it and the From picker is read again',
         !!verifyBody && JSON.parse(verifyBody.body).code === '123456' && called(beforeVerify, 'GET', '/api/v1/me/identities') && !buttonIn(itemFor('me@strict.example'), 'Verify'),
         verifyBody ? verifyBody.body : 'no request');

      senderAddresses.push({ id: 7, address: 'new@later.example', name: '', verified: false, created: '2026-09-17 12:00:00', dmarc_policy: '' });
      location.hash = '#/settings';
      await flush();
      location.hash = '#/accounts';
      await flush();
      buttonIn(itemFor('new@later.example'), 'Send a new code').dispatchEvent(makeEvent('click'));
      await flush();
      check('a new code asked for too soon says how long to wait', status().textContent.indexOf('42') >= 0, status().textContent);

      document.getElementById('sendas-address').value = 'Second@Elsewhere.example';
      document.getElementById('sendas-name').value = 'Second';
      const beforeAdd = requests.length;
      document.getElementById('sendas-form').dispatchEvent(makeEvent('submit'));
      await flush();
      const added = since(beforeAdd).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/sender-addresses')[0];
      check('adding one sends the address and the name, and says the code is on its way',
         !!added && JSON.parse(added.body).address === 'Second@Elsewhere.example' && JSON.parse(added.body).name === 'Second' &&
         String(status().className).indexOf('ok') >= 0 && !!itemFor('second@elsewhere.example'), added ? added.body : 'no request');

      confirmAnswer = true;
      const beforeRemove = requests.length;
      buttonIn(itemFor('old@relaxed.example'), 'Remove').dispatchEvent(makeEvent('click'));
      await flush();
      check('removing one asks and removes it', called(beforeRemove, 'DELETE', '/api/v1/me/sender-addresses/6') && !itemFor('old@relaxed.example'));
   }

   // ---- Masked addresses: one for each sign-up, switched off when it turns to spam
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const walkAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const rows = () => walkAll(document.getElementById('masked-rows'), (n) => n.tagName === 'TR');
      const rowFor = (address) => rows().filter((r) => r.textContent.indexOf(address) >= 0)[0];
      const buttonIn = (row, english) => walkAll(row, (n) => n.tagName === 'BUTTON' && n.textContent === words(english))[0];
      const status = () => document.getElementById('masked-status');

      const beforeLoad = requests.length;
      location.hash = '#/masked';
      await flush();
      check('the Masked addresses page reads them and is a page of the settings',
         called(beforeLoad, 'GET', '/api/v1/me/masked-addresses') && document.getElementById('masked-section').hidden === false && document.getElementById('settings-head').hidden === false);
      check('each is listed with what it is for and how much it has brought, and the one switched off says so',
         rows().length === 2 && rowFor('bookshop.k3v9qa@example.com').textContent.indexOf('Bookshop') >= 0 &&
         rowFor('news.a2b3c4@example.com').textContent.indexOf(words('Switched off: mail to it is refused')) >= 0 && !!buttonIn(rowFor('news.a2b3c4@example.com'), 'Switch on'),
         rows().map((r) => r.textContent).join(' | '));

      document.getElementById('masked-label').value = ' Garden centre ';
      const beforeMake = requests.length;
      document.getElementById('masked-form').dispatchEvent(makeEvent('submit'));
      await flush();
      const made = since(beforeMake).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/masked-addresses')[0];
      check('making one sends the label, lists the address, copies it and reads the From picker again',
         !!made && JSON.parse(made.body).label === 'Garden centre' && !!rowFor('garden.p7q8r9@example.com') &&
         clipboardText === 'garden.p7q8r9@example.com' && called(beforeMake, 'GET', '/api/v1/me/identities'), made ? made.body : 'no request');

      const beforeOff = requests.length;
      buttonIn(rowFor('bookshop.k3v9qa@example.com'), 'Switch off').dispatchEvent(makeEvent('click'));
      await flush();
      const off = since(beforeOff).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/masked-addresses/3')[0];
      check('Switch off switches it off and says mail to it is refused from now on',
         !!off && JSON.parse(off.body).active === false && !!buttonIn(rowFor('bookshop.k3v9qa@example.com'), 'Switch on') &&
         status().textContent.indexOf('bookshop.k3v9qa@example.com') >= 0, off ? off.body : 'no request');

      confirmAnswer = true;
      const beforeRemove = requests.length;
      buttonIn(rowFor('news.a2b3c4@example.com'), 'Remove').dispatchEvent(makeEvent('click'));
      await flush();
      check('Remove asks, saying mail to it will be refused, and removes it',
         confirms[confirms.length - 1].indexOf('news.a2b3c4@example.com') >= 0 && called(beforeRemove, 'DELETE', '/api/v1/me/masked-addresses/2') && !rowFor('news.a2b3c4@example.com'));
   }

   // ---- Recover deleted items: put back, or deleted for good
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const walkAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const rows = () => walkAll(document.getElementById('recover-rows'), (n) => n.tagName === 'TR');
      const rowFor = (subject) => rows().filter((r) => r.textContent.indexOf(subject) >= 0)[0];
      const buttonIn = (row, english) => walkAll(row, (n) => n.tagName === 'BUTTON' && n.textContent === words(english))[0];

      const beforeLoad = requests.length;
      location.hash = '#/recover';
      await flush();
      check('the Recover deleted items page lists what was deleted within the window, with where it was',
         called(beforeLoad, 'GET', '/api/v1/me/recoverable') && document.getElementById('recover-section').hidden === false && rows().length === 2 &&
         rowFor('Seed order').textContent.indexOf('Projects.Garden') >= 0 && document.getElementById('recover-note').textContent.indexOf('14') >= 0,
         rows().map((r) => r.textContent).join(' | '));

      const beforeRestore = requests.length;
      buttonIn(rowFor('Quarterly figures'), 'Put back').dispatchEvent(makeEvent('click'));
      await flush();
      check('Put back asks the server to restore it, and it leaves the list',
         called(beforeRestore, 'POST', '/api/v1/me/recoverable/41/restore') && !rowFor('Quarterly figures') &&
         String(document.getElementById('recover-status').className).indexOf('ok') >= 0);

      confirmAnswer = true;
      const beforePurge = requests.length;
      buttonIn(rowFor('Seed order'), 'Delete for good').dispatchEvent(makeEvent('click'));
      await flush();
      check('Delete for good asks first, and deletes it', confirms[confirms.length - 1].indexOf('Seed order') >= 0 &&
         called(beforePurge, 'DELETE', '/api/v1/me/recoverable/40') && !rowFor('Seed order') && document.getElementById('recover-empty').hidden === false);

      recoverable.days = 0;
      location.hash = '#/settings';
      await flush();
      location.hash = '#/recover';
      await flush();
      check('a server that keeps nothing says so', document.getElementById('recover-note').textContent === words('Your server keeps nothing that is deleted.') &&
         document.getElementById('recover-empty').hidden === true, document.getElementById('recover-note').textContent);
      recoverable.days = 14;
   }

   // ---- Messages that expire: an expiry and a passcode on the compose form, and the list with Withdraw
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const walkAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const rows = () => walkAll(document.getElementById('confidential-rows'), (n) => n.tagName === 'TR');
      const rowFor = (subject) => rows().filter((r) => r.textContent.indexOf(subject) >= 0)[0];
      const buttonIn = (row, english) => walkAll(row, (n) => n.tagName === 'BUTTON' && n.textContent === words(english))[0];
      const expires = document.getElementById('compose-expires');
      const passcode = document.getElementById('compose-passcode');

      document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
      await flush();
      check('a server that offers a message that expires shows the expiry on the compose form, with no passcode field until one is chosen',
         document.getElementById('expires-row').hidden === false && passcode.hidden === true && document.getElementById('compose-expires-note').hidden === true,
         'row hidden=' + document.getElementById('expires-row').hidden + ' passcode hidden=' + passcode.hidden);
      expires.value = '3600';
      expires.dispatchEvent(makeEvent('change'));
      check('choosing an expiry shows the passcode field and says what the expiry does and does not do',
         passcode.hidden === false && document.getElementById('compose-expires-note').hidden === false &&
         document.getElementById('compose-expires-note').textContent.length > 40,
         'passcode hidden=' + passcode.hidden);
      document.getElementById('compose-to').value = 'ann@example.net';
      document.getElementById('compose-subject').value = 'For your eyes only';
      document.getElementById('compose-text').value = 'The figures are in the room, not in this message.';
      passcode.value = 'orange';
      confirmAnswer = true;
      const beforeExpiring = requests.length;
      document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
      await flush();
      fireTimers();
      await flush();
      await flush();
      const expiringSend = since(beforeExpiring).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/messages').map((r) => JSON.parse(r.body))[0];
      check('the send carries expires_in in seconds and the passcode', !!expiringSend && expiringSend.expires_in === 3600 && expiringSend.passcode === 'orange',
         expiringSend ? JSON.stringify(expiringSend) : 'no POST');
      check('and the next message starts without an expiry', expires.value === '0' && passcode.hidden === true && passcode.value === '', 'value=' + expires.value);
      dismissAllToasts();

      // Send later would deliver the draft in full at the hour, with no expiry:
      // the row goes while an expiry is chosen, and the button refuses.
      check('with no expiry chosen, Send at is offered', document.getElementById('later-row').hidden === false, 'later-row hidden=' + document.getElementById('later-row').hidden);
      expires.value = '86400';
      expires.dispatchEvent(makeEvent('change'));
      document.getElementById('compose-to').value = 'ann@example.net';
      document.getElementById('compose-later').value = '2030-01-01T09:00';
      const beforeLater = requests.length;
      document.getElementById('compose-send-later').dispatchEvent(makeEvent('click'));
      await flush();
      check('a message that expires cannot be scheduled: Send at is hidden while an expiry is chosen, and Schedule refuses without saving a draft',
         document.getElementById('later-row').hidden === true && !called(beforeLater, 'POST', '/api/v1/me/drafts') &&
         document.getElementById('compose-status').textContent === words('A message that expires cannot be scheduled: send it now, or set Expires to Never.'),
         'later-row hidden=' + document.getElementById('later-row').hidden + ' status=' + document.getElementById('compose-status').textContent);
      expires.value = '0';
      expires.dispatchEvent(makeEvent('change'));
      check('and Send at comes back when the expiry is set to Never', document.getElementById('later-row').hidden === false, 'later-row hidden=' + document.getElementById('later-row').hidden);
      document.getElementById('compose-later').value = '';

      const beforeList = requests.length;
      location.hash = '#/confidential';
      await flush();
      check('the Messages that expire page lists what was sent, to whom, and who opened it',
         called(beforeList, 'GET', '/api/v1/me/confidential') && document.getElementById('confidential-section').hidden === false && rows().length === 2 &&
         rowFor('Salary review').textContent.indexOf('bob@example.net') >= 0 && rowFor('The board pack').textContent.indexOf(words('Not yet')) >= 0 &&
         rowFor('The board pack').textContent.indexOf(words('Passcode')) >= 0,
         rows().map((r) => r.textContent).join(' | '));

      confirmAnswer = true;
      const beforeRevoke = requests.length;
      buttonIn(rowFor('The board pack'), 'Withdraw').dispatchEvent(makeEvent('click'));
      await flush();
      check('Withdraw asks first, saying the copy is deleted, and the row is marked withdrawn with nothing more to do to it',
         confirms[confirms.length - 1].indexOf('The board pack') >= 0 && called(beforeRevoke, 'POST', '/api/v1/me/confidential/12/revoke') &&
         rowFor('The board pack').textContent.indexOf(words('Withdrawn')) >= 0 && !buttonIn(rowFor('The board pack'), 'Withdraw') &&
         String(document.getElementById('confidential-status').className).indexOf('ok') >= 0,
         rowFor('The board pack') ? rowFor('The board pack').textContent : 'no row');
      confidential[0].revoked = false;
   }

   // ---- Offline for the last N days: the choice under the ceiling, the store
   // filled in the background, a message and a search answered from it with no
   // connection, the worker answering a kept part, Clear, and a ceiling of 0
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const fill = (text, ...values) => values.reduce((s, v, i) => s.split('{' + i + '}').join(String(v)), words(text));
      const sizeText = (bytes) => bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' MB' : bytes >= 1024 ? (bytes / 1024).toFixed(0) + ' KB' : bytes + ' bytes';
      const listRows = () => document.getElementById('message-list').children.filter((n) => n.tagName === 'DIV' && String(n.className).indexOf('msg') === 0);
      const select = document.getElementById('pref-offline-days');
      const values = () => select.children.map((o) => o.value);

      location.hash = '#/settings';
      await flush();
      check('the Settings card offers the steps up to the ceiling and nothing above it, and the reader has chosen nothing yet',
         document.getElementById('offline-form').hidden === false && values().join(' ') === '0 7 14 30' && select.value === '0' && called(0, 'GET', '/api/v1/me/offline'),
         values().join(' ') + ' value=' + select.value);
      check('nothing is fetched in the background while nothing is chosen', !called(0, 'GET', /^\/api\/v1\/me\/offline\/messages/) && idbStore('rows').size === 0);

      // Fourteen days: the choice goes to the server, then the window is walked
      // and the bodies come in newest first, each with its HTML document and its
      // attachments, into the store beside the folder tree.
      const beforeChoice = requests.length;
      select.value = '14';
      document.getElementById('offline-form').dispatchEvent(makeEvent('submit'));
      await flush(); await flush();
      const put = since(beforeChoice).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/offline')[0];
      check('the choice is saved on the server as a number of days', !!put && JSON.parse(put.body).days === 14, put ? put.body : 'no PUT');
      const fetched = since(beforeChoice).filter((r) => r.method === 'GET' && /^\/api\/v1\/me\/messages\/\d+$/.test(r.path)).map((r) => r.path.split('/').pop());
      check('the window is walked and the bodies are fetched newest first',
         called(beforeChoice, 'GET', /^\/api\/v1\/me\/offline\/messages\?after_id=0/) && fetched.join(' ') === '103 102 101', fetched.join(' '));
      check('the HTML document and every attachment of a kept message are fetched with it',
         called(beforeChoice, 'GET', '/api/v1/me/messages/102/html') && [0, 1, 2].every((i) => called(beforeChoice, 'GET', '/api/v1/me/messages/102/attachments/' + i)));
      check('the three messages are in the store with their listing rows, their bodies and the parts',
         idbStore('rows').size === 3 && idbStore('messages').size === 3 && ['102/html', '102/0', '102/1', '102/2'].every((k) => idbStore('parts').has(k)),
         'rows=' + idbStore('rows').size + ' messages=' + idbStore('messages').size + ' parts=' + Array.from(idbStore('parts').keys()).join(','));
      check('the words a search looks through are the subject, the sender and the body, from the HTML when there is no text',
         String(idbStore('rows').get(102).words).indexOf('hello') >= 0 && String(idbStore('rows').get(101).words).indexOf('plain') >= 0);
      check('the folder tree is kept with them, for a page opened with no connection', idbStore('meta').has('folders') && !!store.get('hmPortalOfflineOwner'));
      const keptBytes = Array.from(idbStore('rows').values()).reduce((n, r) => n + r.bytes, 0);
      check('the card says how much of the window is kept here, and what the browser allows',
         document.getElementById('offline-kept').textContent === fill('{0} of the {1} messages of the last {2} days are kept here, {3}.', 3, 3, 14, sizeText(keptBytes)) + ' ' + fill('This browser allows this site about {0}.', '2.0 GB') &&
         document.getElementById('offline-room').hidden === true && document.getElementById('offline-clear').hidden === false,
         document.getElementById('offline-kept').textContent);

      // A server that cannot read the choice for a moment says so with a 500,
      // and the page leaves what it keeps alone: a failed read is not a
      // reader who chose nothing. Then a listing that says nothing is chosen
      // while the page holds fourteen days is not pruned by either.
      const partsKept = idbStore('parts').size;
      offlineRefused = true;
      location.hash = '#/f/1';
      await flush();
      const beforeRefused = requests.length;
      location.hash = '#/settings';
      await flush(); await flush();
      check('a window the server could not read (500) leaves the store, the owner and the choice as they were',
         called(beforeRefused, 'GET', '/api/v1/me/offline') && !called(beforeRefused, 'GET', /^\/api\/v1\/me\/offline\/messages/) &&
         idbStore('rows').size === 3 && idbStore('parts').size === partsKept && store.get('hmPortalOfflineOwner') === account && select.value === '14' && document.getElementById('offline-form').hidden === false,
         'rows=' + idbStore('rows').size + ' owner=' + store.get('hmPortalOfflineOwner') + ' value=' + select.value);
      offlineRefused = false;
      offlineListingBlank = true;
      location.hash = '#/f/1';
      await flush();
      const beforeBlank = requests.length;
      location.hash = '#/settings';
      await flush(); await flush();
      check('a listing that says nothing is chosen, while the page holds a choice, drops nothing',
         called(beforeBlank, 'GET', /^\/api\/v1\/me\/offline\/messages/) && idbStore('rows').size === 3 && idbStore('parts').size === partsKept,
         'rows=' + idbStore('rows').size + ' parts=' + idbStore('parts').size);
      offlineListingBlank = false;

      // A browser with room for less than the window: said plainly, and the run stops keeping.
      storageEstimate = { usage: 3000, quota: 4000 };
      location.hash = '#/f/1';
      await flush();
      location.hash = '#/settings';
      await flush(); await flush();
      check('a window that will not fit is said to be kept newest first, as far as it goes', document.getElementById('offline-room').hidden === false &&
         document.getElementById('offline-room').textContent.indexOf('14') >= 0, document.getElementById('offline-room').textContent);
      storageEstimate = { usage: 5 * 1024 * 1024, quota: 2 * 1024 * 1024 * 1024 };

      // No connection. The last visit's saved copies are put aside, so what is
      // read comes from the store: a message, the folder, and a search.
      store.delete('hmPortalOffline');
      networkDown = true;
      location.hash = '#/m/101';
      await flush();
      check('with no connection a kept message opens from the store, and the page says so',
         document.getElementById('message-subject').textContent === idbStore('messages').get(101).data.subject && document.getElementById('message-text').textContent === 'plain' &&
         document.getElementById('offline-note').hidden === false, document.getElementById('message-subject').textContent + ' / ' + document.getElementById('message-text').textContent);
      location.hash = '#/f/1';
      await flush();
      // The three kept copies answer to one message id, so the folder shows them as one conversation of three.
      const conversation = listRows()[0];
      const tally = conversation ? conversation.children.map((c) => c.children || []).reduce((a, b) => a.concat(b), []).filter((n) => n.className === 'tc')[0] : null;
      check('the folder lists what the store holds for it', listRows().length === 1 && !!tally && tally.textContent === '3',
         listRows().length + ' rows, tally ' + (tally ? tally.textContent : 'none'));
      location.hash = '#/search?q=hello';
      await flush();
      check('a search with no connection is answered from the kept words', listRows().length === 1 && listRows()[0].textContent.indexOf('Second') >= 0, listRows().length + ' rows');
      location.hash = '#/search?q=plain%20-hello';
      await flush();
      check('with the operators the words can answer: -word', listRows().length === 2, listRows().length + ' rows');
      // The walk brought the server's flags in: the listing says the first is read.
      location.hash = '#/search?q=is:unread%20has:attachment';
      await flush();
      const unread = listRows().length;
      location.hash = '#/search?q=is:read';
      await flush();
      check('and is: and has:, with the flags as the server last listed them', unread === 2 && listRows().length === 1 && String(idbStore('rows').get(101).row.flags.seen) === 'true', unread + ' unread, ' + listRows().length + ' read');
      // The three kept copies are 800 bytes, received 2026-09-08 09:00:00, with
      // notes.txt, logo.png and chart.svg: a size, a day, a span and a file
      // name are answered against those, each way, on this browser's clock.
      const rowsFor = async (q) => { location.hash = '#/search?q=' + encodeURIComponent(q); await flush(); return listRows().length; };
      const counts = [await rowsFor('larger:750'), await rowsFor('larger:800'), await rowsFor('smaller:1k'), await rowsFor('before:2026-09-09'), await rowsFor('before:2026-09-08'),
                      await rowsFor('after:2026-09-07'), await rowsFor('older_than:1d'), await rowsFor('newer_than:1d')];
      check('and larger:, smaller:, before:, after:, older_than: and newer_than:, against the kept size and received time', counts.join(' ') === '3 0 3 3 0 3 3 0', counts.join(' '));
      const named = [await rowsFor('filename:logo'), await rowsFor('filename:nope'), await rowsFor('-filename:logo'), await rowsFor('has:link'), await rowsFor('larger:lots')];
      check('and filename: against the kept attachments\' names; an operator the store cannot answer, or a value that is not one, lets a message through', named.join(' ') === '3 0 0 3 3', named.join(' '));

      // The worker answers a kept message's parts when the network fails, under
      // the server's own headers, and nothing for a message it does not hold.
      const workerPath = scriptPath.slice(0, Math.max(scriptPath.lastIndexOf('/'), scriptPath.lastIndexOf('\\')) + 1) + 'PortalServiceWorker.js';
      const worker = { listeners: Object.create(null), addEventListener(name, fn) { this.listeners[name] = fn; }, skipWaiting: () => Promise.resolve(), clients: { claim: () => Promise.resolve() }, registration: {} };
      const workerContext = vm.createContext({ self: worker, caches: cachesStub, indexedDB: indexedDBStub, Response, URL, console, fetch: () => Promise.reject(new TypeError('Failed to fetch')), Promise, JSON, String, Number, Object, Array });
      vm.runInContext(fs.readFileSync(workerPath, 'utf8'), workerContext, { filename: 'portal-sw.js' });
      const ask = async (path) => {
         let answered = null;
         worker.listeners.fetch({ request: { method: 'GET', url: 'http://portal.test' + path }, respondWith: (p) => { answered = p; } });
         return answered ? await answered : null;
      };
      const image = await ask('/api/v1/me/messages/102/attachments/1');
      check('the worker answers a kept attachment from the store when the network fails, as a download under a sandbox',
         !!image && image.status === 200 && image.headers.get('content-type') === 'image/png' && image.headers.get('content-security-policy') === 'sandbox' &&
         String(image.headers.get('content-disposition')).indexOf('logo.png') >= 0, image ? image.status + ' ' + image.headers.get('content-type') : 'not answered');
      const html = await ask('/api/v1/me/messages/102/html');
      check('and the HTML document under the policy the server sends it with', !!html && html.status === 200 && String(html.headers.get('content-type')).indexOf('text/html') === 0 &&
         String(html.headers.get('content-security-policy')).indexOf("default-src 'none'") === 0 && String(html.headers.get('content-security-policy')).indexOf('https:') < 0);
      const unknown = await ask('/api/v1/me/messages/999/attachments/0');
      check('a part it does not hold is not made up', !!unknown && unknown.status === 503);
      check('a request of another shape is left to the network', worker.listeners.fetch({ request: { method: 'GET', url: 'http://portal.test/api/v1/me/folders' }, respondWith: () => { throw new Error('answered'); } }) === undefined);

      // The connection is back: the page carries on.
      networkDown = false;
      window.dispatchEvent(makeEvent('online'));
      await flush();

      // The page asks for the account again - the same load() a cold open
      // takes, here the one a password change makes - and the connection goes
      // the moment the change is answered. What this browser kept is shown
      // under the kept folders, no session is asked for, a folder and a
      // message read from the store; and when the connection is back the
      // account is asked for and the kept mail gives way to the real thing.
      location.hash = '#/settings';
      await flush();
      const beforeCut = requests.length;
      networkDownAfter = 'POST /api/v1/me/password';
      document.getElementById('current').value = 'a-real-password';
      document.getElementById('new').value = 'Offline-Passw0rd';
      document.getElementById('confirm').value = 'Offline-Passw0rd';
      document.getElementById('password-form').dispatchEvent(makeEvent('submit'));
      await flush(); await flush();
      const folderNav = document.getElementById('folder-nav');
      check('a page that cannot reach the server, in a browser that kept mail, shows the account under the kept folders and asks for no session',
         networkDown && called(beforeCut, 'GET', '/api/v1/me') && !called(beforeCut, 'POST', '/api/v1/session') &&
         document.getElementById('account').hidden === false && document.getElementById('signin').hidden === true &&
         document.getElementById('who').textContent === account && document.getElementById('offline-note').hidden === false &&
         document.getElementById('probe-state').textContent === words('No connection') && folderNav.children.length > 0,
         'account hidden=' + document.getElementById('account').hidden + ' who=' + document.getElementById('who').textContent + ' state=' + document.getElementById('probe-state').textContent + ' folders=' + folderNav.children.length);
      store.delete('hmPortalOffline');
      location.hash = '#/f/1';
      await flush();
      check('and lists the kept messages of a folder', listRows().length === 1, listRows().length + ' rows');
      location.hash = '#/m/102';
      await flush();
      check('and opens one', document.getElementById('message-subject').textContent === 'Second' && document.getElementById('message-view').hidden === false,
         document.getElementById('message-subject').textContent);
      // While the server stays out of reach the page asks for the account
      // again by itself, at the probe's slow pace, and stays on what it shows:
      // a listener that is down fails a request exactly as a network that
      // went does, with the browser online all along, and no online event
      // will ever come for that.
      const beforeRetry = requests.length;
      fireTimers();
      await flush();
      check('while the server stays out of reach the page asks for the account again on a timer, asks for no session, and stays with the kept message it was showing',
         called(beforeRetry, 'GET', '/api/v1/me') && !called(beforeRetry, 'POST', '/api/v1/session') && !called(beforeRetry, 'GET', /changes/) &&
         document.getElementById('account').hidden === false && document.getElementById('offline-note').hidden === false &&
         document.getElementById('message-subject').textContent === 'Second' && timers.length > 0,
         JSON.stringify(since(beforeRetry).map((r) => r.method + ' ' + r.path)) + ' timers=' + timers.length);
      const beforeBack = requests.length;
      networkDown = false;
      window.dispatchEvent(makeEvent('online'));
      await flush(); await flush();
      check('the connection back, the account, its folders and the window are asked for again and the page is the server\'s',
         called(beforeBack, 'GET', '/api/v1/me') && called(beforeBack, 'GET', '/api/v1/me/folders') && called(beforeBack, 'GET', '/api/v1/me/offline') &&
         document.getElementById('offline-note').hidden === true && document.getElementById('probe-state').textContent !== words('No connection'),
         JSON.stringify(since(beforeBack).map((r) => r.method + ' ' + r.path)) + ' note hidden=' + document.getElementById('offline-note').hidden);
      location.hash = '#/f/1';
      await flush();
      check('and a folder is read from the server again, not the store', called(beforeBack, 'GET', /\/folders\/1\/messages/) && listRows().length > 1, listRows().length + ' rows');

      // Cut again, and this time it is the listener that comes back, not the
      // network: no online event is fired, and the timer alone brings the
      // page back to the server's.
      location.hash = '#/settings';
      await flush();
      networkDownAfter = 'POST /api/v1/me/password';
      document.getElementById('current').value = 'Offline-Passw0rd';
      document.getElementById('new').value = 'Offline-Passw0rd-2';
      document.getElementById('confirm').value = 'Offline-Passw0rd-2';
      document.getElementById('password-form').dispatchEvent(makeEvent('submit'));
      await flush(); await flush();
      check('a listener that fails with the network up puts the page on the kept mail as a network that went does',
         networkDown && document.getElementById('account').hidden === false && document.getElementById('offline-note').hidden === false && document.getElementById('probe-state').textContent === words('No connection'),
         'account hidden=' + document.getElementById('account').hidden + ' state=' + document.getElementById('probe-state').textContent);
      networkDown = false;
      const beforeListener = requests.length;
      fireTimers();
      await flush(); await flush();
      check('and when the server answers again the timer, with no online event, asks for the account, its folders and the window, and the page is the server\'s',
         called(beforeListener, 'GET', '/api/v1/me') && called(beforeListener, 'GET', '/api/v1/me/folders') && called(beforeListener, 'GET', '/api/v1/me/offline') &&
         document.getElementById('offline-note').hidden === true && document.getElementById('probe-state').textContent !== words('No connection'),
         JSON.stringify(since(beforeListener).map((r) => r.method + ' ' + r.path)) + ' note hidden=' + document.getElementById('offline-note').hidden);

      // Sign-out removes it all - the database and the name of whose mail it
      // held - and a new sign-in fills it again from the choice the account
      // carries. Then a change the probe reports - one arrival - brings the
      // store up to date a few seconds later: the arrival is fetched into it.
      document.getElementById('signout').dispatchEvent(makeEvent('click'));
      await flush();
      check('sign-out removes everything kept, and whose it was',
         document.getElementById('signin').hidden === false && idbStore('rows').size === 0 && idbStore('parts').size === 0 && idbStore('meta').size === 0 && !store.get('hmPortalOfflineOwner'),
         'rows=' + idbStore('rows').size + ' parts=' + idbStore('parts').size + ' owner=' + store.get('hmPortalOfflineOwner'));
      document.getElementById('address').value = account;
      document.getElementById('password').value = 'a-real-password';
      document.getElementById('signin-form').dispatchEvent(makeEvent('submit'));
      await flush(); await flush();
      check('a new sign-in fills it again from the choice the account carries', idbStore('rows').size === 3 && store.get('hmPortalOfflineOwner') === account && idbStore('meta').has('folders'),
         'rows=' + idbStore('rows').size + ' owner=' + store.get('hmPortalOfflineOwner'));
      offlineArrival = true;
      changeAgain = true;
      const beforeReport = requests.length;
      fireTimers();
      await flush();
      check('a change the probe reports arms one walk of the window for a few seconds later, not one at once',
         called(beforeReport, 'GET', /changes/) && !called(beforeReport, 'GET', /^\/api\/v1\/me\/offline\/messages/) && timers.length > 0,
         timers.length + ' timers, ' + JSON.stringify(since(beforeReport).map((r) => r.path)));
      const beforeArrival = requests.length;
      fireTimers();
      await flush(); await flush();
      check('and the walk fetches what arrived into the store',
         called(beforeArrival, 'GET', /^\/api\/v1\/me\/offline\/messages/) && called(beforeArrival, 'GET', '/api/v1/me/messages/104') && idbStore('rows').has(104) && idbStore('rows').size === 4,
         'rows=' + Array.from(idbStore('rows').keys()).join(','));
      offlineArrival = false;

      // Clear takes everything out.
      location.hash = '#/settings';
      await flush();
      const beforeClear = requests.length;
      document.getElementById('offline-clear').dispatchEvent(makeEvent('click'));
      await flush();
      const cleared = since(beforeClear).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/offline')[0];
      check('Clear empties the store, takes the choice down to nothing and says so',
         !!cleared && JSON.parse(cleared.body).days === 0 && idbStore('rows').size === 0 && idbStore('messages').size === 0 && idbStore('parts').size === 0 &&
         select.value === '0' && !store.get('hmPortalOfflineOwner') && String(document.getElementById('offline-status').className).indexOf('ok') >= 0,
         'rows=' + idbStore('rows').size + ' parts=' + idbStore('parts').size + ' value=' + select.value);

      // An administrator who offers none: the choice is not shown, and the reader is told what still works.
      offlineWindow.days_max = 0;
      location.hash = '#/f/1';
      await flush();
      location.hash = '#/settings';
      await flush();
      check('a ceiling of 0 hides the choice and says the listing and the last opened messages are still kept',
         document.getElementById('offline-form').hidden === true && document.getElementById('offline-off').hidden === false);
      offlineWindow.days_max = 30;
   }

   // ---- Web Push: the devices, and the service worker's own handler
   await pushChecks();

   // ---- signing out ends the session and stops the probe
   const beforeOut = requests.length;
   document.getElementById('signout').dispatchEvent(makeEvent('click'));
   await flush();
   check('signing out ends the session', called(beforeOut, 'DELETE', '/api/v1/session'));
   check('and shows the sign-in form again', gate.hidden === false && app.hidden === true);
   const stillPolling = requests.length;
   fireTimers();
   await flush();
   check('nothing is polled once signed out', !called(stillPolling, 'GET', /changes/),
      JSON.stringify(since(stillPolling).map((r) => r.path)));

   // ---- and it forgets the mailbox, which is the half that matters on a
   // shared browser. The page renders an open message and a listing from
   // memory BEFORE it asks the server, so anything left here is shown to
   // whoever signs in next, without a request that could have been refused.
   check('the message pane is emptied', document.getElementById('message-text').innerHTML === '',
      document.getElementById('message-text').innerHTML.slice(0, 80));
   check('the message list is emptied', document.getElementById('message-list').innerHTML === '',
      document.getElementById('message-list').innerHTML.slice(0, 80));
   check('the folder tree is emptied', document.getElementById('folder-nav').innerHTML === '',
      document.getElementById('folder-nav').innerHTML.slice(0, 80));
   check('the attachment list is emptied',
      document.getElementById('message-attachments').innerHTML === '',
      document.getElementById('message-attachments').innerHTML.slice(0, 80));
   check('the html frame keeps no document', !document.getElementById('message-html').getAttribute('srcdoc'),
      String(document.getElementById('message-html').getAttribute('srcdoc')).slice(0, 80));
   check('the address bar names no message or folder', location.hash === '' || location.hash === '#/',
      location.hash);
   check('the draft is blank again', document.getElementById('compose-to').value === '',
      document.getElementById('compose-to').value);
   check('the recent searches go with the account', !store.has('hmPortalSearches'), String(store.get('hmPortalSearches')));

   // ---- and it forgets the keys, which is the other half that matters on a
   // shared browser. The unlocked map holds the account's private keys
   // themselves, and the page hands every one of them to the S/MIME module
   // whenever a sealed message is opened - so a key left there after a
   // sign-out is offered to the next person who signs in, against their mail.
   // Signed in twice here, as two different people in one tab.
   window.hmSmime = smimeStub;
   smimeStores['user@example.com'] = { own: [ownKeyOf('user@example.com', 'a-real-password')], recipients: [] };
   smimeStores['second@example.com'] = { own: [], recipients: [] };
   const signInAs = async (address, password) => {
      account = address;
      document.getElementById('address').value = address;
      document.getElementById('password').value = password;
      document.getElementById('signin-form').dispatchEvent(makeEvent('submit'));
      await flush();
   };
   const openSealed = async () => {
      location.hash = '#/m/601';
      await flush();
      location.hash = '#/f/1';
      await flush();
   };
   await signInAs('user@example.com', 'a-real-password');
   check('the key of the account signing in is listed', document.getElementById('smime-own-rows').children.length === 1,
      document.getElementById('smime-own-rows').children.length + ' rows');
   await openSealed();
   check('a sealed message is read with the keys the password at the gate unlocked',
      readWith.length === 1 && readWith[0].length === 1 && readWith[0][0] === 'user@example.com', JSON.stringify(readWith));
   // A password typed into the unlock box and never submitted is a password
   // in a field, and the field survives a sign-out unless it is emptied.
   document.getElementById('smime-unlock-password').value = 'typed-and-not-sent';
   document.getElementById('smime-passphrase').value = 'the-file-passphrase';
   document.getElementById('signout').dispatchEvent(makeEvent('click'));
   await flush();
   check('sign-out empties the certificate store on the page',
      document.getElementById('smime-own-rows').children.length === 0 && document.getElementById('smime-own-table').hidden === true,
      document.getElementById('smime-own-rows').children.length + ' rows, table hidden=' + document.getElementById('smime-own-table').hidden);
   check('and keeps no password in either box',
      document.getElementById('smime-unlock-password').value === '' && document.getElementById('smime-passphrase').value === '',
      JSON.stringify([document.getElementById('smime-unlock-password').value, document.getElementById('smime-passphrase').value]));
   await signInAs('second@example.com', 'a-different-password');
   await openSealed();
   check('and the next account to sign in is handed none of the first account\'s keys',
      readWith.length === 2 && readWith[1].length === 0, JSON.stringify(readWith));

   // ---- the promise the unlock dialogue makes, kept
   // An administrator has changed the password: the keys are still wrapped
   // under the old one, so the password that signs in will not open them. The
   // page's refusal says that entering the old one re-wraps them with the
   // current one, and this is that sentence being true - from the refusal to
   // the PUT that stores the new wrapping.
   document.getElementById('signout').dispatchEvent(makeEvent('click'));
   await flush();
   smimeStores['third@example.com'] = { own: [ownKeyOf('third@example.com', 'the-old-password')], recipients: [] };
   await signInAs('third@example.com', 'the-current-password');
   location.hash = '#/m/601';
   await flush();
   check('a sealed message whose keys the gate did not open offers to unlock them',
      document.getElementById('message-smime-unlock').hidden === false);
   document.getElementById('message-smime-unlock').dispatchEvent(makeEvent('click'));
   await flush();
   check('and asks for a password', document.getElementById('smime-unlock').hidden === false);
   document.getElementById('smime-unlock-password').value = 'neither-password';
   document.getElementById('smime-unlock-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('a password that opens nothing is refused with the promise this session can keep, in the reader\'s language',
      document.getElementById('smime-unlock-status').textContent === german['Wrong password. If an administrator changed your password, enter the one you had before; the keys are then re-wrapped with the current one.'],
      document.getElementById('smime-unlock-status').textContent);
   const beforeRewrap = requests.length;
   document.getElementById('smime-unlock-password').value = 'the-old-password';
   document.getElementById('smime-unlock-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const rewrap = since(beforeRewrap).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/smime/own').pop();
   check('the password the keys were wrapped under opens them, and they are wrapped again with the current one',
      !!rewrap && JSON.parse(rewrap.body).key.data === 'the-current-password', rewrap ? rewrap.body : 'nothing was stored again');
   check('so what the server holds is the new wrapping',
      smimeStores['third@example.com'].own[0].key.data === 'the-current-password',
      JSON.stringify(smimeStores['third@example.com'].own[0].key));
   check('and the dialogue is gone', document.getElementById('smime-unlock').hidden === true);

   // ---- the page closing during the undo countdown does not send a sealed
   // message in the clear. Signing and encryption happen in smimePrepare on
   // the way through submitMessage; beforeunload cannot wait for them, so the
   // body it could send is the plaintext with an encrypt flag the server
   // ignores. The right thing is to send nothing and warn, and this is the
   // check that fails on the tree that sent it.
   await writeMessage('For your eyes only.');
   document.getElementById('compose-encrypt').checked = true;
   const beforeClose = requests.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('a send with Encrypt ticked waits out the undo delay', timers.length > 0 && !called(beforeClose, 'POST', '/api/v1/me/messages'),
      timers.length + ' timers, ' + JSON.stringify(since(beforeClose).map((r) => r.method + ' ' + r.path)));
   const closing = makeEvent('beforeunload');
   window.dispatchEvent(closing);
   await flush();
   check('and the page closing during the wait sends nothing, rather than the plaintext',
      !called(beforeClose, 'POST', '/api/v1/me/messages'), JSON.stringify(since(beforeClose).map((r) => r.method + ' ' + r.path + ' ' + r.body)));
   check('and warns that a message is still waiting', closing.defaultPrevented === true);
   document.getElementById('undo-send').dispatchEvent(makeEvent('click'));
   await flush();
   // The negative control: the same message with nothing to seal still goes
   // in the request the browser keeps alive after the page, as it always did.
   document.getElementById('compose-to').value = 'a@example.net';
   document.getElementById('compose-subject').value = 'For anyone';
   document.getElementById('compose-text').value = 'Nothing to seal.';
   document.getElementById('compose-encrypt').checked = false;
   document.getElementById('compose-sign').checked = false;
   const beforePlain = requests.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const closingPlain = makeEvent('beforeunload');
   window.dispatchEvent(closingPlain);
   await flush();
   const keptAlive = since(beforePlain).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/messages').pop();
   check('a plain message waiting when the page closes goes in a request kept alive after it, unwarned',
      !!keptAlive && closingPlain.defaultPrevented === false && JSON.parse(keptAlive.body).encrypt === undefined,
      keptAlive ? keptAlive.body : 'no POST; prevented=' + closingPlain.defaultPrevented);
   dismissAllToasts();

   // ---- a page the browser gave no cryptography. Over plain HTTP - behind
   // a reverse proxy that speaks HTTPS to the world and HTTP to this server -
   // the page is not a secure context and crypto.subtle is withheld. The
   // controls stay away and the page says why, instead of offering Sign and
   // Encrypt and failing at send time with the name of a missing property.
   document.getElementById('signout').dispatchEvent(makeEvent('click'));
   await flush();
   window.hmSmime = Object.assign({}, smimeStub, { available: false });
   window.isSecureContext = false;
   const readsBefore = readWith.length;
   const beforeInsecure = requests.length;
   await signInAs('user@example.com', 'a-real-password');
   check('on a page without cryptography the Security card says signing and encryption are unavailable, and why',
      document.getElementById('smime-unavailable').hidden === false && document.getElementById('smime-body').hidden === true
         && document.getElementById('smime-unavailable').textContent.indexOf('HTTPS') >= 0,
      'unavailable hidden=' + document.getElementById('smime-unavailable').hidden + ' body hidden=' + document.getElementById('smime-body').hidden);
   check('and the key store is not even asked for', !called(beforeInsecure, 'GET', '/api/v1/me/smime'),
      JSON.stringify(since(beforeInsecure).filter((r) => /smime/.test(r.path)).map((r) => r.method + ' ' + r.path)));
   location.hash = '#/m/601';
   await flush();
   check('a sealed message says so in the reader rather than failing',
      document.getElementById('message-smime').hidden === false && document.getElementById('message-smime-text').textContent.indexOf('HTTPS') >= 0
         && document.getElementById('message-smime-unlock').hidden === true,
      document.getElementById('message-smime-text').textContent);
   check('and no key is handed to a module that cannot use one', readWith.length === readsBefore, readWith.length + ' reads');
   document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
   await flush();
   check('and the compose form offers neither Sign nor Encrypt',
      document.getElementById('compose-sign-label').hidden === true && document.getElementById('compose-encrypt-label').hidden === true);
   window.hmSmime = smimeStub;
   delete window.isSecureContext;

   if (failures.length) {
      console.error('\nportal script: ' + failures.length + ' of ' + checks + ' checks failed\n');
      failures.forEach((f) => console.error('  FAILED: ' + f));
      console.error('');
      process.exit(1);
   }
   console.log('portal script: ' + checks + ' checks passed');
}

// Web Push. Run at the end of main(), on the signed-in page, and then the
// service worker is loaded into a world of its own and handed a push event -
// which is the only way to find out what a reader would actually see.
let germanCatalogue = null;
async function pushChecks() {
   // The page has been switched to German by the checks above, so every text a
   // reader sees here is the catalogue's. Asserting the English would prove the
   // page was NOT translated.
   const said = (english) => (germanCatalogue && germanCatalogue[english]) || english;
   // The page is on Settings, which is where the devices are.
   location.hash = '#/settings';
   await flush();

   const before = requests.length;
   check('the devices are read when the settings page is opened', called(before - 1, 'GET', '/api/v1/me/push') || pushDevices.length >= 0);

   // Nothing is subscribed yet, so the form is hidden and the button is offered.
   check('no device is listed before one is subscribed', document.getElementById('push-empty').hidden === false,
      'empty=' + document.getElementById('push-empty').hidden);
   check('the choices are hidden until this device is subscribed', document.getElementById('push-form').hidden === true);

   // A browser that refuses permission is told so, and nothing is sent.
   NotificationStub.permission = 'denied';
   const beforeRefusal = requests.length;
   document.getElementById('push-subscribe').dispatchEvent(makeEvent('click'));
   await flush();
   check('a refused permission is explained, in the reader\'s language, and nothing is sent',
      document.getElementById('push-status').textContent === said('Notifications were refused for this site. Allow them in the browser and try again.') &&
      !called(beforeRefusal, 'POST', '/api/v1/me/push/subscriptions'),
      document.getElementById('push-status').textContent);

   // Granted: the page subscribes and hands the server what the browser gave it.
   NotificationStub.permission = 'granted';
   const beforeSubscribe = requests.length;
   document.getElementById('push-subscribe').dispatchEvent(makeEvent('click'));
   await flush();

   const sent = since(beforeSubscribe).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/push/subscriptions');
   check('the subscription is handed to the server exactly once', sent.length === 1, sent.length + ' posts');
   const body = sent.length ? JSON.parse(sent[0].body) : {};
   check('the endpoint the browser gave is what is sent', body.endpoint === 'https://push.example.net/push/JzLQ3raZJfFBR0aqvOMsLrt54w4rJUsV', String(body.endpoint));
   check('both keys are sent, base64url, at the lengths RFC 8291 needs',
      body.keys && body.keys.p256dh === SUBSCRIPTION_P256DH && body.keys.auth === SUBSCRIPTION_AUTH,
      JSON.stringify(body.keys));
   check('the device is named from the browser rather than left blank', /Android/.test(String(body.device)), String(body.device));
   check('the browser was asked for a user-visible subscription with the server key',
      subscribeCalls.length === 1 && subscribeCalls[0].userVisibleOnly === true && subscribeCalls[0].applicationServerKey.length === 65,
      JSON.stringify(subscribeCalls.length));
   check('the reader is told notifications are on, in their own language',
      document.getElementById('push-status').textContent === said('Notifications are on for this device.'),
      document.getElementById('push-status').textContent);
   check('the device is now listed', document.getElementById('push-rows').children.length === 1,
      document.getElementById('push-rows').children.length + ' rows');
   check('and the choices are offered', document.getElementById('push-form').hidden === false);
   check('nothing that could be used to send is kept in this browser',
      [...store.values()].every((v) => v.indexOf(SUBSCRIPTION_AUTH) < 0 && v.indexOf('push.example.net') < 0),
      JSON.stringify([...store.entries()]));

   // The choices: what may be said, from whom, and the quiet hours.
   document.getElementById('push-detail').value = '2';
   document.getElementById('push-senders').value = '@work.example';
   document.getElementById('push-labels').value = 'Urgent';
   document.getElementById('push-quiet-from').value = '22:30';
   document.getElementById('push-quiet-to').value = '07:00';
   const beforeSave = requests.length;
   document.getElementById('push-form').dispatchEvent(makeEvent('submit'));
   await flush();

   const saved = since(beforeSave).filter((r) => r.method === 'PUT' && /\/api\/v1\/me\/push\/subscriptions\/\d+$/.test(r.path));
   check('the choices are saved on the device that was subscribed', saved.length === 1, saved.length + ' puts');
   const choice = saved.length ? JSON.parse(saved[0].body) : {};
   check('the quiet hours are sent as minutes of the device day', choice.quiet_from === 1350 && choice.quiet_to === 420,
      JSON.stringify([choice.quiet_from, choice.quiet_to]));
   check('what a notification may say is sent as the level chosen', choice.detail === 2, String(choice.detail));
   check('the senders and labels are sent as written', choice.senders === '@work.example' && choice.labels === 'Urgent',
      JSON.stringify([choice.senders, choice.labels]));
   check('and the reader is told it was saved', document.getElementById('push-status').className.indexOf('ok') >= 0,
      document.getElementById('push-status').className);

   // A test push, and then the device is removed - which also unsubscribes the
   // browser, so the next "turn it on" makes a fresh subscription.
   const beforeTest = requests.length;
   const buttons = document.getElementById('push-rows').children[0].children[3];
   buttons.children[0].dispatchEvent(makeEvent('click'));
   await flush();
   check('the test button asks the server to send one now', called(beforeTest, 'POST', '/api/v1/me/push/test'));

   const beforeRemove = requests.length;
   document.getElementById('push-rows').children[0].children[3].children[1].dispatchEvent(makeEvent('click'));
   await flush();
   check('removing the device removes the row', called(beforeRemove, 'DELETE', /\/api\/v1\/me\/push\/subscriptions\/\d+$/));
   check('and the browser no longer holds a subscription', liveSubscription === null);
   check('the list is empty again', document.getElementById('push-rows').children.length === 0,
      document.getElementById('push-rows').children.length + ' rows');

   // ---- the service worker's push handler, run for real
   const workerPath = scriptPath.slice(0, Math.max(scriptPath.lastIndexOf('/'), scriptPath.lastIndexOf('\\')) + 1) + 'PortalServiceWorker.js';
   const shown = [];
   const workerCaches = cachesStub;
   const worker = {
      listeners: Object.create(null),
      addEventListener(name, fn) { this.listeners[name] = fn; },
      registration: { showNotification: (title, options) => { shown.push({ title, options }); return Promise.resolve(); } },
      skipWaiting: () => Promise.resolve(),
      clients: { claim: () => Promise.resolve(), matchAll: () => Promise.resolve([]), openWindow: (url) => { shown.push({ opened: url }); return Promise.resolve(); } }
   };
   const workerContext = vm.createContext({
      self: worker, caches: workerCaches, Response, URL, console,
      fetch: () => Promise.resolve({ ok: false }), Promise, JSON, String, Number, Object, Array
   });
   vm.runInContext(fs.readFileSync(workerPath, 'utf8'), workerContext, { filename: 'portal-sw.js' });
   check('the service worker registers a push handler', typeof worker.listeners.push === 'function');
   check('and a handler for a notification being clicked', typeof worker.listeners.notificationclick === 'function');

   const fire = async (payload) => {
      shown.length = 0;
      let waited = null;
      worker.listeners.push({ data: payload === null ? null : { json: () => payload }, waitUntil: (p) => { waited = p; } });
      await waited;
      await flush();
      return shown[0];
   };

   const full = await fire({ v: 1, kind: 'mail', count: 1, from: 'ada@example.com', name: 'Ada Lovelace', subject: 'The engine', folder: 'INBOX' });
   check('a push with a sender and a subject shows both', full && full.title === 'Ada Lovelace' && full.options.body === 'The engine',
      JSON.stringify(full));

   // The words come from the cache the page filled when it registered the
   // worker, so a reader who chose German is not shown English by the one part
   // of the webmail that runs with no page open.
   const counted = await fire({ v: 1, kind: 'mail', count: 3, folder: 'INBOX' });
   check('a push carrying only a count says how many, names nobody, and says it in the reader\'s language',
      counted && counted.title === said('New mail') &&
      counted.options.body === said('{0} new messages').replace('{0}', '3') &&
      counted.options.body !== '3 new messages',
      JSON.stringify(counted));

   const one = await fire({ v: 1, kind: 'mail', count: 1, folder: 'INBOX' });
   check('a single message with no detail allowed says only that there is new mail',
      one && one.title === said('New mail') && one.options.body === said('New mail'), JSON.stringify(one));

   const tested = await fire({ v: 1, kind: 'mail', count: 1, test: true, folder: '' });
   check('a test push says it is a test', tested && tested.options.body === said('A test notification'), JSON.stringify(tested));

   // A push whose body could not be read still shows something: a browser that
   // has granted permission expects a notification for every push and shows its
   // own when none comes.
   const empty = await fire(null);
   check('a push with no readable payload still shows a notification', empty && empty.title === said('New mail'), JSON.stringify(empty));
   check('every notification replaces the last rather than stacking up',
      shown.length === 1 && shown[0].options.tag === 'hm-push', JSON.stringify(shown[0] && shown[0].options));

   // And with no words left for it at all - a browser with no Cache Storage, or
   // a worker woken before the page ever ran - it still says something.
   await workerCaches.delete('hm-portal-push');
   const bare = await fire({ v: 1, kind: 'mail', count: 2, folder: 'INBOX' });
   check('a worker with no words left for it falls back to English rather than showing nothing',
      bare && bare.title === 'New mail' && bare.options.body === '2 new messages', JSON.stringify(bare));
}

main().catch((why) => {
   console.error('the portal script did not survive being run:');
   console.error(why && why.stack ? why.stack : why);
   process.exit(1);
});
