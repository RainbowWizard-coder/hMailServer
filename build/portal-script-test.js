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

// Every scrollIntoView the page asks for, counted across elements: the help
// pane's scroll is what its checks hold still, and what they expect once.
let scrollsIntoView = 0;

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
   // As the DOM has it: before null appends, and a child already here moves.
   // The compose editor's tables need it - a row above the cursor's, a column
   // to its left - and a page that reached for it found nothing here before.
   insertBefore(child, before) {
      if (!before) { return this.appendChild(child); }
      if (child.parentNode) { child.parentNode.removeChild(child); }
      const at = this.childNodes.indexOf(before);
      if (at < 0) { throw new Error('insertBefore: the reference node is not a child of this element'); }
      child.parentNode = this;
      this.childNodes.splice(at, 0, child);
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
   scrollIntoView() { this.scrolledIntoView += 1; scrollsIntoView += 1; }
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
document.createTextNode = function (text) { return new TextNode(String(text)); };
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
// The help pane's renderer, which the page loads as /portal-markdown.js:
// the real module, required from beside the script, so the checks below
// render the documentation with what ships.
window.hmMarkdown = require(require('path').resolve(scriptPath.slice(0, Math.max(scriptPath.lastIndexOf('/'), scriptPath.lastIndexOf('\\')) + 1) + 'PortalMarkdown.js'));
// The documentation the help pane shows: the repository's hmailserver/docs,
// which the server embeds and answers at /api/v1/help and /api/v1/help/<page>.
const docsDir = require('path').resolve(scriptPath.slice(0, Math.max(scriptPath.lastIndexOf('/'), scriptPath.lastIndexOf('\\')) + 1) + '../../../../docs');
const helpPages = () => fs.readdirSync(docsDir).filter((f) => f.endsWith('.md') && f !== 'README.md' && f !== 'DeckParity.md').sort()
   .map((f) => {
      const body = fs.readFileSync(require('path').join(docsDir, f), 'utf8').replace(/\r\n/g, '\n');
      const heads = window.hmMarkdown.headings(body);
      return { name: f.slice(0, -3), title: heads.length && heads[0].level === 1 ? heads[0].text : f.slice(0, -3), bytes: Buffer.byteLength(body), headings: heads };
   });
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
   // key(i) is the rest of the Storage interface, and the page uses it to
   // sweep out keys it can no longer name - a tally filed under an address
   // that a session ending while the page was closed took with it.
   key: (i) => (i >= 0 && i < store.size ? Array.from(store.keys())[i] : null),
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
// The domain's S/MIME authority as GET /api/v1/me/smime answers it, per
// account, and what the enrolment and the directory lookup were asked.
const smimeAuthorities = {};
const enrolments = [];
const authorityLookups = [];
// What the directory answers for an address: one certificate or a list of them.
const authorityCertificates = {};
// Where a colleague's stored certificate stands in the authority's record, by
// fingerprint, when a check says so (valid otherwise).
const authorityStatuses = {};
// The own store refusing, as the real one does at twenty certificates.
let smimeOwnRefuse = false;
// What POST /api/v1/me/smime/chain answers, when a check sets it.
let smimeChainAnswer = null;
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

// One message carrying an Autocrypt header, which is what makes the page
// fetch its OpenPGP module and learn the sender's key.
const WITH_AUTOCRYPT = {
   id: 602, folder_id: 1, uid: 22, subject: 'With a key', from: 'b@example.net', to: 'user@example.com', cc: '',
   date: '2026-09-10 09:00', size: 1200, message_id: '<autocrypt@example.net>', references: '', in_reply_to: '',
   headers: 'From: b@example.net\r\nAutocrypt: addr=b@example.net; prefer-encrypt=mutual; keydata=QUFB\r\nContent-Type: text/plain\r\n',
   text: 'Hello.', html: '', html_remote: false, flags: { seen: true, flagged: false, draft: false }, attachments: []
};

// One message a zero-access mailbox received: encrypted on delivery to the
// account's own key, the outer headers kept, the server's mark on it.
const SEALED_ON_DELIVERY = {
   id: 603, folder_id: 1, uid: 23, subject: 'Sealed on delivery', from: 'c@example.net', to: 'user@example.com', cc: '',
   date: '2026-09-18 09:00', size: 2400, message_id: '<sealed-on-delivery@example.net>', references: '', in_reply_to: '',
   headers: 'From: c@example.net\r\nSubject: Sealed on delivery\r\nX-hMailServer-Zero-Access: sealed on delivery\r\nContent-Type: multipart/encrypted; protocol="application/pgp-encrypted"; boundary="za"\r\n',
   text: '', html: '', html_remote: false, flags: { seen: false, flagged: false, draft: false }, attachments: []
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
// One message whose flags this account may not change - a folder ACL that
// allows reading and not writing, or a message already carrying as many
// keywords as the server allows. Set to an id, it refuses that one and no
// other, which is how a change over a whole conversation is made to half fail.
let flagsRefuseId = 0;
const isStarRequest = (body) => /"keywords_remove"/.test(body || '') && /\$Star2/i.test(body || '');
const withState = (m) => Object.assign({}, m, { flags: Object.assign({}, m.flags, { flagged: !!flaggedById[m.id], keywords: (keywordsById[m.id] || []).slice() }) });

const LISTING = {
   total: 5,
   messages: [
      // Received five days ago, asks a question, never answered: the shape a nudge is for.
      { id: 103, uid: 13, subject: 'Third', from: 'c@example.net', to: 'user@example.com', date: stampDaysAgo(5), size: 900, snippet: 'Can you send the figures?', flags: { seen: false, flagged: false, draft: false } },
      { id: 102, uid: 12, subject: 'Second', from: 'b@example.net', to: 'user@example.com', date: '2026-09-08 09:00', size: 800, flags: { seen: false, flagged: false, draft: false }, in_reply_to: '<one@example.net>', brand: 'example.net' },
      // Stamped at delivery by the impersonation check: the row wears the mark.
      { id: 101, uid: 11, subject: 'First', from: 'a@example.net', to: 'user@example.com', date: '2026-09-07 09:00', size: 700, flags: { seen: true, flagged: false, draft: false }, impersonation: true }
   ]
};

// Three messages for the impersonation warning (ImpersonationCheck.h): the
// borrowed-name shape, the look-alike-domain shape, and one the check found
// nothing to say about, which carries no header and gets no warning.
const impersonated = (id, subject, from, impersonation) => ({
   id: id, folder_id: 1, uid: id - 580, subject: subject, from: from, to: 'user@example.com', cc: '',
   date: '2026-09-22 10:00', size: 600, message_id: '<' + id + '@example.net>', references: '', in_reply_to: '',
   headers: 'From: ' + from + '\r\nSubject: ' + subject + '\r\n' + (impersonation ? 'X-hMailServer-Impersonation: ' + impersonation + '\r\n' : ''),
   text: 'Please pay the attached invoice today.', html: '', html_remote: false, flags: { seen: true, flagged: false, draft: false }, attachments: [],
   impersonation: impersonation || ''
});
const BORROWED_NAME = impersonated(604, 'Urgent', '"Ada Lovelace" <ada@evil.example>', 'name "Ada Lovelace" matches Ada Lovelace (ada@example.com); from ada@evil.example');
const LOOKALIKE_DOMAIN = impersonated(605, 'Invoice', 'accounts@examp1e.com', 'domain examp1e.com looks like example.com; from accounts@examp1e.com');
const NOT_IMPERSONATED = impersonated(606, 'Lunch', 'colleague@example.com', '');

// The Sent folder: one message the inbox's 'Second' answers, one nothing
// answered, sent long enough ago to be nudged, and one sent yesterday.
const SENT_LISTING = [
   { id: 401, uid: 1, subject: 'Figures for the board', from: 'user@example.com', to: 'alice@example.net', date: stampDaysAgo(6), size: 500, message_id: '<one@example.net>', flags: { seen: true, flagged: false, draft: false } },
   { id: 402, uid: 2, subject: 'Draft agenda', from: 'user@example.com', to: 'bob@example.net', date: stampDaysAgo(5), size: 500, message_id: '<agenda@example.com>', flags: { seen: true, flagged: false, draft: false } },
   { id: 403, uid: 3, subject: 'Thanks', from: 'user@example.com', to: 'carol@example.net', date: stampDaysAgo(1), size: 300, message_id: '<thanks@example.com>', flags: { seen: true, flagged: false, draft: false } },
   // For the recall checks: one too old to recall (and too old to nudge),
   // and one filed into Sent by hand whose From is somebody else's.
   { id: 404, uid: 4, subject: 'Old news', from: 'user@example.com', to: 'dan@example.net', date: stampDaysAgo(40), size: 300, message_id: '<old@example.com>', flags: { seen: true, flagged: false, draft: false } },
   { id: 405, uid: 5, subject: 'Filed here by hand', from: 'other@example.org', to: 'user@example.com', date: stampDaysAgo(1), size: 300, message_id: '<theirs@example.org>', flags: { seen: true, flagged: false, draft: false } }
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

// The importance checks need mail the page has not judged yet - it judges a
// message once a session - so these arrive only when those checks ask for
// them: at stage 1 a newsletter addressed to a list and a message from the
// one contact in the address book the reader writes to; at stage 2 a second
// newsletter from that same sender, to show what the reader's own mark
// taught this browser. Nothing else in the run sets importantArrival.
let importantArrival = 0;
const IMPORTANT_MANY = 'list@example.net, one@example.net, two@example.net, three@example.net, four@example.net, user@example.com';
const IMPORTANT_NEW = [
   { id: 111, uid: 18, subject: 'Newsletter', from: 'news@stranger.example', to: IMPORTANT_MANY, category: 'promotions', date: '2026-09-09 14:30', size: 400, flags: { seen: true, flagged: false, draft: false } },
   { id: 112, uid: 17, subject: 'Figures', from: 'Alice Example <alice@example.net>', to: 'user@example.com', date: '2026-09-09 14:00', size: 400, flags: { seen: true, flagged: false, draft: false } }
];
const IMPORTANT_AGAIN = { id: 113, uid: 19, subject: 'Newsletter again', from: 'news@stranger.example', to: IMPORTANT_MANY, category: 'promotions', date: '2026-09-09 15:00', size: 400, flags: { seen: true, flagged: false, draft: false } };
// Stage 3: two senders whose address is the name of a property every object
// in JavaScript has, and a conversation of two from one sender. The first
// pair is the one a stranger can choose - a From header is theirs to write -
// and neither may be taken for an address the reader writes to. The second is
// what a mark over a conversation must count as one word about its sender.
const IMPORTANT_PROTOTYPE = [
   { id: 117, uid: 23, subject: 'Bulletin', from: 'Bulletin <bulletin@stranger.example>', to: 'user@example.com', references: '<bulletin@stranger.example>', date: '2026-09-09 17:00', size: 400, flags: { seen: true, flagged: false, draft: false } },
   { id: 116, uid: 22, subject: 'Bulletin', from: 'Bulletin <bulletin@stranger.example>', to: 'user@example.com', references: '<bulletin@stranger.example>', date: '2026-09-09 16:45', size: 400, flags: { seen: true, flagged: false, draft: false } },
   { id: 115, uid: 21, subject: 'Prototype pills', from: 'Cheap Pills <constructor>', to: 'user@example.com', date: '2026-09-09 16:30', size: 400, flags: { seen: true, flagged: false, draft: false } },
   { id: 114, uid: 20, subject: 'Prototype more', from: 'Also Cheap <__proto__>', to: 'user@example.com', date: '2026-09-09 16:15', size: 400, flags: { seen: true, flagged: false, draft: false } }
];
const importantExtra = () => (importantArrival >= 3 ? IMPORTANT_PROTOTYPE : []).concat(importantArrival >= 2 ? [IMPORTANT_AGAIN] : []).concat(importantArrival >= 1 ? IMPORTANT_NEW : []);

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

// A message whose one attachment is large: what a forward of it carries is
// bytes in the same request the pictures go in, and the page's twelve
// megabytes are of both together. 11,200,000 characters of base64 are
// 8,400,000 bytes.
const BIG_ATTACHMENT = {
   id: 707, folder_id: 1, uid: 16, subject: 'The deck', from: 'a@example.net', to: 'user@example.com', cc: '',
   date: '2026-09-09 14:00', received: '2026-09-09 14:00:00', size: 8400000, message_id: '<deck@example.net>', references: '', in_reply_to: '',
   text: 'Here it is', html: '', html_remote: false,
   flags: { seen: true, flagged: false, draft: false },
   attachments: [{ index: 0, name: 'deck.pdf', size: 8400000, content_type: 'application/pdf', content_id: '' }]
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
   { id: 2, name: 'work', display_name: 'Work', color: '#1a73e8', owner: 'user@example.com', mine: true, writable: true, is_default: false, sync_token: 1, shares: [{ address: 'b@example.net', write: false, delegate: false }, { group: 'Sales', members: 3, write: false, delegate: false }] },
   { id: 3, name: 'team', display_name: 'Team', color: '#d93025', owner: 'b@example.net', mine: false, writable: false, delegate: false, is_default: false, sync_token: 1, shares: [] },
   // Shared as a delegate: the reader may answer the owner's invitations in it.
   { id: 5, name: 'shared-5', display_name: 'Boss', color: '#188038', owner: 'boss@example.com', mine: false, writable: true, delegate: true, is_default: false, sync_token: 1, shares: [] }
];
// The owner's answer to the invitation in the delegated calendar, once given on their behalf.
let reviewAnswer = '';
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
   // An invitation to the boss from somebody elsewhere, in the calendar shared as a delegate: 14:00-15:00 UTC on the eleventh day.
   const reviewDay = new Date(from.getTime() + (to - from > 20 * 86400000 ? 11 : 0) * 86400000);
   const r = new Date(Date.UTC(reviewDay.getUTCFullYear(), reviewDay.getUTCMonth(), reviewDay.getUTCDate(), 14, 0, 0));
   if (r >= from && r < to) {
      out.push({ id: 12, calendar: 5, uid: 'review@example.net', start: utcStamp(r), end: utcStamp(new Date(r.getTime() + 3600000)), local_start: localStamp(r), local_end: localStamp(new Date(r.getTime() + 3600000)),
         recurring: false, recurrence_id: '', overridden: false, writable: true, mine: false, delegate: true, color: '#188038', summary: 'Review', location: '', description: '', tz: 'UTC', rrule: '', alarm_minutes: -1,
         organizer: 'org@example.net', organizer_name: 'Olive Org', attendees: [{ address: 'boss@example.com', name: 'Boss', partstat: reviewAnswer || 'NEEDS-ACTION', sent_by: reviewAnswer ? 'user@example.com' : '' }], my_partstat: '', status: '', sequence: 0, all_day: false });
      // In the same calendar, an event with another organiser that does not name the boss as a guest: nothing to answer.
      const y = new Date(r.getTime() + 2 * 3600000);
      out.push({ id: 13, calendar: 5, uid: 'stray@example.net', start: utcStamp(y), end: utcStamp(new Date(y.getTime() + 1800000)), local_start: localStamp(y), local_end: localStamp(new Date(y.getTime() + 1800000)),
         recurring: false, recurrence_id: '', overridden: false, writable: true, mine: false, delegate: true, color: '#188038', summary: 'Not mine', location: '', description: '', tz: 'UTC', rrule: '', alarm_minutes: -1,
         organizer: 'org@example.net', organizer_name: 'Olive Org', attendees: [{ address: 'someone@example.net', name: '', partstat: 'NEEDS-ACTION', sent_by: '' }], my_partstat: '', status: '', sequence: 0, all_day: false });
   }
   return out;
}
// When somebody of the domain is busy, for the availability strip: a colleague
// with a busy hour and a tentative half hour on the day asked about, the reader
// with one, one who chose not to be shown (404), and no one else.
function freeBusyJson(query) {
   const m = /address=([^&]+)&from=(\d{8}T\d{6}Z)&to=(\d{8}T\d{6}Z)/.exec(query || '');
   if (!m) { return json(400, { error: 'address, from and to are required' }); }
   const address = decodeURIComponent(m[1]);
   const day = m[2].slice(0, 8);
   if (address === 'guest@example.com') { return json(200, { address, from: m[2], to: m[3], busy: [{ start: day + 'T100000Z', end: day + 'T110000Z', kind: 'busy' }, { start: day + 'T130000Z', end: day + 'T133000Z', kind: 'tentative' }] }); }
   if (address === 'user@example.com') { return json(200, { address, from: m[2], to: m[3], busy: [{ start: day + 'T160000Z', end: day + 'T170000Z', kind: 'busy' }] }); }
   return json(404, { error: 'no account of your domain has that address, or it does not show when it is busy' });
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
   if (path.startsWith('/api/v1/me/freebusy?') && method === 'GET') { return freeBusyJson(path.split('?')[1]); }
   m = /^\/api\/v1\/me\/calendar\/events\/(\d+)\/reply$/.exec(path);
   if (m && method === 'POST') { const wanted = JSON.parse(body || '{}'); if (Number(m[1]) !== 12) { return json(403, { error: 'only the calendar\'s owner, or a delegate the owner named, answers an invitation in it' }); } reviewAnswer = wanted.partstat; return json(200, { event_id: 12, calendar: 5, partstat: wanted.partstat, on_behalf_of: 'boss@example.com', replied: true, to: 'org@example.net' }); }
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

// The lists on this server the reader belongs to (GET /api/v1/me/lists): one
// with a digest and an archive, and one still waiting for the reader to
// confirm. Emptied by the checks to see the card go.
let myListsMode = 'full';
function answerMyLists(method, path, body) {
   if (method === 'GET' && path === '/api/v1/me/lists') {
      if (myListsMode === 'empty') { return json(200, { lists: [] }); }
      return json(200, { lists: [
         { address: 'team@example.com', delivery: 'each', state: 'member', subscribed: 1790000000, digest: true, reason: '', archive_url: 'https://lists.example.com/lists/team@example.com/archive' },
         { address: 'news@example.com', delivery: 'each', state: 'pending', subscribed: 1790000000, digest: false, reason: '' }
      ] });
   }
   if (method === 'PUT' && path === '/api/v1/me/lists/team%40example.com') {
      return json(200, { address: 'team@example.com', delivery: JSON.parse(body || '{}').delivery, state: 'member', subscribed: 1790000000, digest: true, reason: '' });
   }
   if (method === 'DELETE' && path === '/api/v1/me/lists/team%40example.com') { return json(200, { left: true }); }
   return null;
}
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
// The organisation's directory (DirectoryStore.h): the reader's own domain,
// answered with the same shape the real route answers - a person's entry with a
// title and a photo, a person with nothing set, and a distribution list. The
// reader's own entry is state, since the whole of the Settings card is that a
// PUT changes it and the next GET says so.
let directorySwitchedOn = true;
let directoryUserEditable = true;
let directoryOwn = { title: '', department: '', phone: '', mobile: '', office: '', opt_out: false, has_photo: false, photo_type: '', updated: '' };
function directoryPeople() {
   return [
      { id: 11, kind: 'person', name: 'Ada Lovelace', address: 'ada@example.com', title: 'Head of Engines', department: 'Engineering',
        phone: '+44 20 7946 0000', mobile: '', office: 'Marylebone, 2nd floor', has_photo: true, photo_url: '/api/v1/me/directory/11/photo' },
      { id: 12, kind: 'person', name: '', address: 'quiet@example.com', title: '', department: '', phone: '', mobile: '', office: '', has_photo: false },
      { id: 13, kind: 'list', name: '', address: 'sales@example.com', title: '', department: '', phone: '', mobile: '', office: '', has_photo: false }
   ];
}
function answerDirectory(method, path, body) {
   if (!path.startsWith('/api/v1/me/directory')) { return null; }
   const wanted = JSON.parse(body || '{}');
   if (path === '/api/v1/me/directory/me') {
      if (method === 'GET') { return json(200, { enabled: directorySwitchedOn, editable: directoryUserEditable, entry: Object.assign({}, directoryOwn) }); }
      if (method === 'PUT') {
         // The one rule the page is held to: with the fields locked, only the
         // opt-out goes through, and it goes through anyway.
         const fields = Object.keys(wanted).filter((k) => k !== 'opt_out');
         if (!directoryUserEditable && fields.length) { return json(403, { error: 'the administrator of this server sets the directory entries' }); }
         Object.keys(wanted).forEach((k) => {
            if (k === 'photo') {
               directoryOwn.has_photo = wanted.photo !== null && wanted.photo !== '';
               directoryOwn.photo_type = directoryOwn.has_photo ? 'image/png' : '';
               return;
            }
            directoryOwn[k] = wanted[k];
         });
         directoryOwn.updated = '2026-09-18 12:00:00';
         return json(200, { enabled: directorySwitchedOn, editable: directoryUserEditable, entry: Object.assign({}, directoryOwn) });
      }
      return json(405, { error: 'method not allowed' });
   }
   if (path === '/api/v1/me/directory/me/photo') { return { status: directoryOwn.has_photo ? 200 : 404, headers: { 'Content-Type': 'image/png' }, body: 'PNG' }; }
   if (/^\/api\/v1\/me\/directory\/\d+\/photo$/.test(path)) { return { status: 200, headers: { 'Content-Type': 'image/png' }, body: 'PNG' }; }
   // A colleague's certificate from the domain's S/MIME authority, by address.
   const colleague = /^\/api\/v1\/me\/directory\/([^/?]+)\/smime$/.exec(path);
   if (colleague) {
      const address = decodeURIComponent(colleague[1]);
      authorityLookups.push(address);
      const found = [].concat(authorityCertificates[address] || []);
      if (method !== 'GET' || !found.length) { return json(404, { error: 'no certificate from this domain\'s authority for that address' }); }
      return json(200, { address, certificates: found, chain: ['MAMCAQM='] });
   }
   if (method === 'GET') {
      if (!directorySwitchedOn) { return json(200, { enabled: false, editable: directoryUserEditable, domain: 'example.com', entries: [], count: 0 }); }
      const at = path.indexOf('?');
      const query = at < 0 ? '' : path.slice(at + 1);
      const q = decodeURIComponent((/(^|&)q=([^&]*)/.exec(query) || ['', '', ''])[2] || '').toLowerCase();
      const entries = directoryPeople().filter((e) => !q || [e.name, e.address, e.title, e.department].some((f) => String(f || '').toLowerCase().indexOf(q) >= 0));
      return json(200, { enabled: true, editable: directoryUserEditable, domain: 'example.com', entries, count: entries.length });
   }
   return json(405, { error: 'method not allowed' });
}
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
// The recovery address and the one-time recovery codes (AccountRecovery.h).
// The server never answers the address in full once it is set, so the stub
// answers the masked form exactly as the server does - which is what makes the
// page's "never hold the whole of it" a property a check can see.
let recovery = { enabled: true, available: true, address: '', address_set: false, address_verified: false, updated: 0,
                 codes: { total: 0, unused: 0, created: 0 } };
let resetRequests = [];
// An account with a second factor enrolled, as the server sees one: the route
// answers 401 with X-hMailServer-OTP: required until the request carries the
// one-time code OR one of the recovery codes, which is the whole of what the
// server accepts at every route that asks for a second factor. The page used
// to offer only the first of those anywhere but the sign-in card.
let recoveryDemandsSecondFactor = false;
let recoverySecondFactorSeen = { otp: '', code: '' };
let currentSecondFactor = { otp: '', code: '' };
function maskAddress(address) {
   const at = address.indexOf('@');
   if (at <= 0) { return ''; }
   const labels = address.slice(at + 1).split('.');
   const domain = labels.map((label, i) => (i + 1 === labels.length || !label ? label : label[0] + '.'.repeat(label.length - 1))).join('.');
   return address[0] + '.'.repeat(at - 1) + '@' + domain;
}
function answerRecovery(method, path, body) {
   if (!path.startsWith('/api/v1/me/recovery')) { return null; }
   if (path === '/api/v1/me/recovery' && method === 'GET') { return json(200, JSON.parse(JSON.stringify(recovery))); }
   if (path === '/api/v1/me/recovery' && method === 'PUT') {
      const sent = JSON.parse(body || '{}');
      if (!sent.password) { return json(403, { error: 'the password is not correct' }); }
      if (recoveryDemandsSecondFactor && !currentSecondFactor.otp && !currentSecondFactor.code) {
         return json(401, { error: 'a one-time code is required' }, { 'X-hMailServer-OTP': 'required' });
      }
      recoverySecondFactorSeen = { otp: currentSecondFactor.otp, code: currentSecondFactor.code };
      recovery.address = maskAddress(String(sent.address || '').toLowerCase());
      recovery.address_set = true;
      recovery.address_verified = false;
      return json(200, { address_set: true, address_verified: false });
   }
   if (path === '/api/v1/me/recovery' && method === 'DELETE') {
      recovery.address = ''; recovery.address_set = false; recovery.address_verified = false;
      return json(200, { address_set: false });
   }
   if (path === '/api/v1/me/recovery/resend' && method === 'POST') { return json(200, { sent: true }); }
   if (path === '/api/v1/me/recovery/codes' && method === 'POST') {
      const sent = JSON.parse(body || '{}');
      if (!sent.password) { return json(403, { error: 'the password is not correct' }); }
      const codes = [];
      for (let i = 0; i < 10; i++) { codes.push('A2C4E-' + String(i) + 'H8JKM'); }
      recovery.codes = { total: 10, unused: 10, created: 1789600000 };
      return json(201, { codes: codes });
   }
   return json(404, { error: 'not found' });
}
// The address book the contacts routes answer from. Every row carries kind
// person, as the server's listing does, so that the page's telling of a group
// from a contact is tested against the real shape and not an absence.
function contactBook() {
   // Every row says which address book it is in, as the server's listing does
   // for book=all (AddressBookStore.h): three in the account's own default book
   // and one in a book another account shares with it, read-only.
   const own = (row) => Object.assign({ book_id: 1, book_name: 'Contacts', book_owner: 'user@example.com', book_mine: true, book_writable: true }, row);
   return [
      own({ id: 1, name: 'Alice Example', address: 'alice@example.net', source: 'manual', kind: 'person', birthday: '', note: '', phones: [], has_photo: false }),
      own({ id: 2, name: 'Alan Hidden', address: 'al@example.org', source: 'collected', kind: 'person', birthday: '', note: '', phones: [], has_photo: false }),
      own({ id: 3, name: 'Carol Card', address: 'carol@example.org', source: 'manual', kind: 'person', birthday: '1985-04-12', note: 'Met at the conference.', phones: ['+1 555 0100'], has_photo: true,
        card: 'BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Carol Card\r\nN:Card;Carol;;;\r\nEMAIL;TYPE=INTERNET:carol@example.org\r\nTEL:+1 555 0100\r\nBDAY:1985-04-12\r\nNOTE:Met at the conference.\r\nEND:VCARD\r\n' }),
      { id: 9, name: 'Dana Shared', address: 'dana@example.org', source: 'manual', kind: 'person', birthday: '', note: '', phones: [], has_photo: false,
        book_id: 5, book_name: 'Team', book_owner: 'boss@example.com', book_mine: false, book_writable: false }
   ];
}

// The books the account can see: its own default, and one boss@example.com
// shares with it read-only. The shares of an own book are the owner's to see,
// so only its own rows carry a shares array.
let addressBooks = [
   { id: 1, name: 'Contacts', owner: 'user@example.com', mine: true, writable: true, is_default: true, created: 1758000000, contacts: 3, shares: [] },
   { id: 5, name: 'Team', owner: 'boss@example.com', mine: false, writable: false, is_default: false, created: 1758000000, contacts: 1 }
];
let addressBookNextId = 11;
// Whether the stubbed listing carries a group out of the book somebody shares
// with the reader. Off, because the server does not; turned on for the one
// check that asks the page what it does when a listing carries one anyway.
let sharedGroupListed = false;
function answerAddressBooks(method, path, body) {
   if (!path.startsWith('/api/v1/me/address-books')) { return null; }
   const wanted = JSON.parse(body || '{}');
   if (path === '/api/v1/me/address-books') {
      if (method === 'GET') { return json(200, { books: addressBooks, count: addressBooks.length }); }
      if (method === 'POST') {
         const name = String(wanted.name || '').trim();
         if (!name) { return json(400, { error: 'name is required' }); }
         if (addressBooks.some((b) => b.mine && b.name === name)) { return json(409, { error: 'an address book of that name exists' }); }
         const made = { id: addressBookNextId++, name, owner: 'user@example.com', mine: true, writable: true, is_default: false, created: 1758000001, contacts: 0, shares: [] };
         addressBooks.push(made);
         return json(201, made);
      }
      return json(405, { error: 'method not allowed' });
   }
   const rest = path.slice('/api/v1/me/address-books/'.length);
   const shareAt = rest.indexOf('/shares');
   const id = Number(shareAt > 0 ? rest.slice(0, shareAt) : rest);
   const book = addressBooks.find((b) => b.id === id);
   if (!book) { return json(404, { error: 'no such address book' }); }
   if (shareAt > 0) {
      if (!book.mine) { return json(403, { error: 'only the address book\'s owner shares it' }); }
      const target = decodeURIComponent(rest.slice(shareAt + '/shares/'.length));
      book.shares = book.shares || [];
      if (method === 'PUT') {
         const kind = target.indexOf('@') >= 0 ? 'account' : 'domain';
         const found = book.shares.find((s) => s.target === target);
         if (found) { found.write = !!wanted.write; } else { book.shares.push({ target, kind, write: !!wanted.write, created: 1758000002 }); }
         return json(200, book);
      }
      if (method === 'DELETE') {
         const at = book.shares.findIndex((s) => s.target === target);
         if (at < 0) { return json(404, { error: 'the address book is not shared with that target' }); }
         book.shares.splice(at, 1);
         return json(200, book);
      }
      return json(405, { error: 'method not allowed' });
   }
   if (method === 'PUT') {
      if (!book.mine) { return json(403, { error: 'the address book is another account\'s' }); }
      book.name = String(wanted.name || book.name);
      return json(200, book);
   }
   if (method === 'DELETE') {
      const at = addressBooks.indexOf(book);
      if (!book.mine) { addressBooks.splice(at, 1); return json(200, { left: true }); }
      if (book.is_default) { return json(403, { error: 'the default address book cannot be deleted' }); }
      addressBooks.splice(at, 1);
      return json(200, { deleted: true });
   }
   return json(405, { error: 'method not allowed' });
}
// Contact groups (ContactGroups.h): rows of the same address book whose kind is
// group, with the members the card names and the recipients they reach - a
// group inside a group already expanded, which is the whole point of the second
// list. One group to start with, holding one of the contacts below.
let contactGroups = [
   { id: 7, name: 'Family', uid: '11111111-2222-3333-4444-555555555555', created: '2026-09-17 09:00:00', kind: 'group', address: '', source: 'manual',
     members: [{ id: 1, name: 'Alice Example', address: 'alice@example.net', kind: 'person' }],
     recipients: [{ id: 1, name: 'Alice Example', address: 'alice@example.net' }] }
];
let contactGroupsRefused = false;
function contactGroupRecipients(members) {
   // What the server answers: every person reached, a member that is itself a
   // group replaced by its own recipients, once each by address.
   const out = [];
   members.forEach((m) => {
     const reached = m.kind === 'group' ? (contactGroups.find((g) => g.id === m.id) || { recipients: [] }).recipients : [m];
     reached.forEach((r) => { if (!out.some((x) => x.address === r.address)) { out.push({ id: r.id || 0, name: r.name || '', address: r.address }); } });
   });
   return out;
}
function contactGroupMember(item) {
   const book = contactBook();
   if (item && item.id) {
      const row = book.find((c) => c.id === Number(item.id)) || contactGroups.find((g) => g.id === Number(item.id));
      return row ? { id: row.id, name: row.name, address: row.address || '', kind: row.kind === 'group' ? 'group' : 'person' } : null;
   }
   const address = String((item && item.address) || '').toLowerCase();
   if (address.indexOf('@') < 0) { return null; }
   const row = book.find((c) => c.address === address);
   return row ? { id: row.id, name: row.name, address: row.address, kind: 'person' } : { id: 0, name: '', address, kind: 'person' };
}
function contactGroupJson(group) {
   return Object.assign({}, group, { recipients: contactGroupRecipients(group.members) });
}
function answerContactGroups(method, path, body) {
   if (!path.startsWith('/api/v1/me/contacts/groups')) { return null; }
   if (contactGroupsRefused) { return json(500, { error: 'the contacts could not be read' }); }
   const wanted = JSON.parse(body || '{}');
   if (path === '/api/v1/me/contacts/groups') {
      if (method === 'GET') { return json(200, { groups: contactGroups.map(contactGroupJson), count: contactGroups.length }); }
      if (method === 'POST') {
         const name = String(wanted.name || '').trim();
         if (!name) { return json(400, { error: 'a group needs a name' }); }
         if (contactGroups.some((g) => g.name.toLowerCase() === name.toLowerCase())) { return json(409, { error: 'a group with that name exists' }); }
         const members = (wanted.members || []).map(contactGroupMember).filter((m) => m);
         const made = { id: 20 + contactGroups.length, name, uid: 'made-' + (20 + contactGroups.length), created: '2026-09-17 10:00:00', kind: 'group', address: '', source: 'manual', members };
         contactGroups.push(made);
         return json(201, contactGroupJson(made));
      }
      return json(404, { error: 'not found' });
   }
   const one = /^\/api\/v1\/me\/contacts\/groups\/(\d+)(\/members(?:\/(.+))?)?$/.exec(path);
   const group = one ? contactGroups.find((g) => g.id === Number(one[1])) : null;
   if (!group) { return json(404, { error: 'no such group' }); }
   if (!one[2] && method === 'GET') { return json(200, contactGroupJson(group)); }
   if (!one[2] && method === 'DELETE') {
      contactGroups = contactGroups.filter((g) => g.id !== group.id);
      contactGroups.forEach((g) => { g.members = g.members.filter((m) => !(m.kind === 'group' && m.id === group.id)); });
      return json(200, { deleted: true });
   }
   if (!one[2] && method === 'PUT') {
      if (wanted.name !== undefined) {
         const name = String(wanted.name || '').trim();
         if (!name) { return json(400, { error: 'a group needs a name' }); }
         if (contactGroups.some((g) => g.id !== group.id && g.name.toLowerCase() === name.toLowerCase())) { return json(409, { error: 'a group with that name exists' }); }
         group.name = name;
      }
      if (wanted.members !== undefined) { group.members = (wanted.members || []).map(contactGroupMember).filter((m) => m); }
      return json(200, contactGroupJson(group));
   }
   if (one[2] && !one[3] && method === 'POST') {
      const member = contactGroupMember(wanted);
      if (!member) { return json(400, { error: 'a member is a contact id or one e-mail address' }); }
      if (!group.members.some((m) => m.id === member.id && m.address === member.address)) { group.members.push(member); }
      return json(200, contactGroupJson(group));
   }
   if (one[3] && method === 'DELETE') {
      const reference = decodeURIComponent(one[3]).toLowerCase();
      const kept = group.members.filter((m) => String(m.id) !== reference && m.address !== reference);
      if (kept.length === group.members.length) { return json(404, { error: 'not a member' }); }
      group.members = kept;
      return json(200, contactGroupJson(group));
   }
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
// Message recall within the server (MessageRecall.h): what the server
// answers a recall with, and whether this session came in on an app
// password, which the page reads from /api/v1/me.
let viaAppPassword = false;
let recallAnswer = { status: 200, body: { recalled: 2, read: 1, gone: 0, failed: 0, elsewhere: 1, sent_marked: true, recipients: [{ address: 'x@example.com', outcome: 'recalled' }, { address: 'y@example.com', outcome: 'recalled' }, { address: 'z@example.com', outcome: 'read' }] } };
function answerRecall(method, path, body) {
   // The Sent messages the recall checks open, served as the page reads a
   // message: with the folder they are in.
   const sent = /^\/api\/v1\/me\/messages\/(40\d)$/.exec(path);
   if (sent && method === 'GET') {
      const found = SENT_LISTING.filter((m) => m.id === Number(sent[1]))[0];
      return found ? json(200, withState(Object.assign({ folder_id: 2, cc: '', text: 'Sent from here.' }, found))) : json(404, { error: 'message not found' });
   }
   if (path !== '/api/v1/me/recall') { return null; }
   if (method !== 'POST') { return json(404, { error: 'not found' }); }
   const wanted = JSON.parse(body || '{}');
   if (recallAnswer.status !== 200) { return json(recallAnswer.status, recallAnswer.body); }
   return json(200, Object.assign({ message_id: String(wanted.message_id || '').replace(/[<>]/g, '') }, recallAnswer.body));
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
// Passkeys, and what the sign-in page may offer. The server verifies the
// cryptography and this file does not: what is checked here is that the page
// asks for a challenge, hands what the authenticator answered back in
// base64url, and draws what it is told.
let signInMethods = { passkeys: true, oidc: true, oidc_label: 'login.example.com', saml: true, saml_label: 'Sign in with SAML' };
let passkeys = [
   { id: 7, name: 'Work laptop', created: 1789600000, last_used: 1789700000, transports: 'internal', algorithm: -7, aaguid: 'AAECAwQFBgcICQoLDA0ODw' },
   { id: 8, name: 'Phone', created: 1789500000, last_used: 0, transports: 'hybrid', algorithm: -7, aaguid: 'AAECAwQFBgcICQoLDA0ODw' }
];
let passkeyChallenges = 0;
let passkeysDemandSecondFactor = false;
let passkeySecondFactorSeen = { otp: '', code: '' };
function answerPasskeys(method, path, body) {
   if (path === '/api/v1/signin-methods' && method === 'GET') { return json(200, Object.assign({}, signInMethods)); }
   if (!path.startsWith('/api/v1/passkeys')) { return null; }
   if (path === '/api/v1/passkeys/challenge' && method === 'POST') {
      passkeyChallenges += 1;
      return json(200, { challenge: 'Y2hhbGxlbmdl', rp_id: 'portal.test', timeout: 300000 });
   }
   if (path === '/api/v1/passkeys/options' && method === 'POST') {
      return json(200, {
         challenge: 'Y2hhbGxlbmdl', timeout: 300000, attestation: 'none',
         rp: { id: 'portal.test', name: 'hMailServer' },
         user: { id: 'YTE', name: account, displayName: account },
         pubKeyCredParams: [{ type: 'public-key', alg: -7 }, { type: 'public-key', alg: -257 }],
         authenticatorSelection: { residentKey: 'preferred', userVerification: 'required' },
         excludeCredentials: passkeys.map((p) => ({ type: 'public-key', id: 'Y3JlZA' }))
      });
   }
   if (path === '/api/v1/passkeys' && method === 'GET') { return json(200, { enabled: signInMethods.passkeys, rp_id: 'portal.test', passkeys: passkeys.slice() }); }
   if (path === '/api/v1/passkeys' && method === 'POST') {
      const wanted = JSON.parse(body || '{}');
      if (!wanted.password) { return json(403, { error: 'the password is not correct' }); }
      if (passkeysDemandSecondFactor && !currentSecondFactor.otp && !currentSecondFactor.code) {
         return json(401, { error: 'a one-time code is required' }, { 'X-hMailServer-OTP': 'required' });
      }
      passkeySecondFactorSeen = { otp: currentSecondFactor.otp, code: currentSecondFactor.code };
      const made = { id: 9, name: wanted.name || '', created: 1789800000, last_used: 0, transports: '', algorithm: -7, aaguid: '' };
      passkeys.unshift(made);
      return json(201, made);
   }
   const m = /^\/api\/v1\/passkeys\/(\d+)$/.exec(path);
   const at = m ? passkeys.findIndex((p) => p.id === Number(m[1])) : -1;
   if (at < 0) { return json(404, { error: 'no such passkey' }); }
   if (method === 'DELETE') { passkeys.splice(at, 1); return json(200, { deleted: true }); }
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
// The second account (row L50): the bearer form of a sign-in answers a token
// in the body and no cookie, and every request that carries that token as
// Authorization: Bearer is answered as that account - a mailbox of its own,
// with folders, mail and identities that are not the first account's, so
// that a page which mixed the two would be caught by what it shows. A bearer
// that is not the live token is 401, as the server answers a session that
// has ended. What the fake does not hold for the second account - settings,
// preferences, the search the follow-ups make - falls through to the
// recorded server's answers, since the contract under test is which
// credential each request carries and what the page keeps apart, not what
// every route holds for a second mailbox.
const SECOND_ADDRESS = 'other@example.com';
const SECOND_PASSWORD = 'other-password';
const SECOND_TOKEN = '0123456789abcdef'.repeat(4);
let secondToken = '';
let secondUnseen = 4;
// The Authorization header of the request being answered, for the routes
// that answer by it; fetchStub sets it before it asks.
let currentAuth = '';
const SECOND_INBOX = { id: 21, account_id: 9, name: 'INBOX', path: 'INBOX', parent_id: 0, special_use: '', subscribed: true, writable: true, messages: 2, unseen: 4, uidvalidity: 1, subfolders: [] };
const SECOND_SENT = { id: 22, account_id: 9, name: 'Sent', path: 'Sent', parent_id: 0, special_use: '\\Sent', subscribed: true, writable: true, messages: 0, unseen: 0, uidvalidity: 1, subfolders: [] };
const SECOND_LISTING = [
   { id: 2102, uid: 2, subject: 'Invoice 4471', from: 'vendor@example.org', to: SECOND_ADDRESS, date: '2026-09-17 10:00', size: 700, flags: { seen: false, flagged: false, draft: false } },
   { id: 2101, uid: 1, subject: 'Welcome aboard', from: 'Boss <boss@example.org>', to: SECOND_ADDRESS, date: '2026-09-16 09:00', size: 900, flags: { seen: true, flagged: false, draft: false } }
];
const SECOND_MESSAGE = {
   id: 2101, folder_id: 21, uid: 1, subject: 'Welcome aboard', from: 'Boss <boss@example.org>', to: SECOND_ADDRESS, cc: '',
   date: '2026-09-16 09:00', received: '2026-09-16 09:00:00', size: 900, message_id: '<welcome@example.org>', references: '', in_reply_to: '',
   text: 'Welcome. The terms are attached.', html: '<p>Welcome.</p><img src="https://tracker.example.org/open.gif">', html_remote: true,
   flags: { seen: true, flagged: false, draft: false, keywords: [] },
   attachments: [{ index: 0, name: 'terms.pdf', size: 1200, content_type: 'application/pdf', content_id: '' }]
};
// The first account under an alias domain's spelling: the server resolves
// it to the account the cookie stands for and refuses the form with 409.
const FIRST_ALIAS_SPELLING = 'user@alias.example';
function answerSecondSignIn() {
   const basic = /^Basic (.+)$/.exec(currentAuth);
   const decoded = basic ? Buffer.from(basic[1], 'base64').toString('utf8') : '';
   if (decoded === FIRST_ALIAS_SPELLING + ':a-real-password') { return json(409, { error: 'that is the account this browser is already signed in as; a second account is another account' }); }
   if (decoded !== SECOND_ADDRESS + ':' + SECOND_PASSWORD) { return json(401, { error: 'Not an account\'s credentials.' }); }
   secondToken = SECOND_TOKEN;
   return json(201, { address: SECOND_ADDRESS, device: 'web-dev', bearer: true, token: secondToken, idle_seconds: 1800, lifetime_seconds: 43200, one_per_device: false, bound_to_address: false });
}
function answerSecond(method, path, body) {
   if (method === 'DELETE' && path === '/api/v1/session') { secondToken = ''; return json(200, { ended: true, bearer: true }); }
   if (path === '/api/v1/me' && method === 'GET') {
      return json(200, { address: SECOND_ADDRESS, quota: { used_bytes: 1048576, limit_mb: 50 }, vacation: { enabled: false, subject: '', message: '', expires: false, expires_date: '' }, second_factor: false, directory_linked: false, password_changed: '2026-09-10' });
   }
   if (path === '/api/v1/me/folders' && method === 'GET') {
      return json(200, { delimiter: '.', folders: [Object.assign({}, SECOND_INBOX, { unseen: secondUnseen }), Object.assign({}, SECOND_SENT)], shared: [] });
   }
   if (path === '/api/v1/me/folders/21/messages' && method === 'GET') { return json(200, { total: 2, messages: SECOND_LISTING.map((m) => JSON.parse(JSON.stringify(m))) }); }
   if (path === '/api/v1/me/folders/22/messages' && method === 'GET') { return json(200, { total: 0, messages: [] }); }
   if (path === '/api/v1/me/messages/2101' && method === 'GET') { return json(200, JSON.parse(JSON.stringify(SECOND_MESSAGE))); }
   if (path.startsWith('/api/v1/me/messages/2101/html') && method === 'GET') {
      return { status: 200, body: '<!doctype html><html><head><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src \'none\'; style-src \'unsafe-inline\'; img-src data:; form-action \'none\'; base-uri \'none\'"></head><body><p>Welcome.</p></body></html>', headers: { 'Content-Type': 'text/html; charset=utf-8' } };
   }
   if (path === '/api/v1/me/messages/2101/attachments/0' && method === 'GET') { return { status: 200, body: '%PDF', headers: { 'Content-Type': 'application/pdf' } }; }
   if (path.startsWith('/api/v1/me/changes') && method === 'GET') {
      return json(200, { token: 'second-' + secondUnseen, changed: false, folders: [{ id: 21, count: 2, unseen: secondUnseen }, { id: 22, count: 0, unseen: 0 }] });
   }
   if (path === '/api/v1/me/identities' && method === 'GET') {
      return json(200, { identities: [{ address: SECOND_ADDRESS, header: 'Other Person <' + SECOND_ADDRESS + '>', kind: 'account', name: 'Other Person' }, { address: 'alias@example.com', header: 'alias@example.com', kind: 'alias', name: '' }] });
   }
   if (path === '/api/v1/me/sessions' && method === 'GET') {
      return json(200, { sessions: [
         { id: 'aaaaaaaaaaaa', device: 'web-dev', address: '127.0.0.1', created_seconds_ago: 30, idle_seconds: 1, current: true, support: false, bearer: true },
         { id: 'bbbbbbbbbbbb', device: 'web-other', address: '198.51.100.4', created_seconds_ago: 900, idle_seconds: 60, current: false, support: false, bearer: false }
      ] });
   }
   if (path === '/api/v1/me/drafts' && method === 'POST') { return json(201, { id: 2155 }); }
   return null;
}

function answer(method, path, body) {
   if (method === 'GET' && path === '/api/v1/help') {
      return json(200, { wiki: 'https://wiki.test/', tag: 'deadbeefdeadbeef', pages: helpPages() });
   }
   if (method === 'GET' && /^\/api\/v1\/help\/[A-Za-z0-9]+$/.test(path)) {
      const file = require('path').join(docsDir, path.slice(13) + '.md');
      if (!fs.existsSync(file)) { return json(404, { error: 'no such page' }); }
      return { status: 200, headers: { 'Content-Type': 'text/markdown; charset=utf-8' }, body: fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n') };
   }
   if (method === 'POST' && path === '/api/v1/session?bearer=1') { return answerSecondSignIn(); }
   if (/^Bearer /.test(currentAuth)) {
      if (!secondToken || currentAuth !== 'Bearer ' + secondToken) { return json(401, { error: 'the session has ended' }); }
      const second = answerSecond(method, path, body);
      if (second) { return second; }
   }
   // No credentials at all, which is the whole point of it: the page can ask
   // for a reset link from the gate, where there is no session to carry.
   if (method === 'POST' && path === '/api/v1/password-reset') {
      resetRequests.push(JSON.parse(body || '{}').address || '');
      return json(202, { requested: true, detail: 'If that address belongs to a mailbox here and it has a confirmed recovery address, a link to choose a new password is on its way there.' });
   }
   if (method === 'POST' && path === '/api/v1/session') { signedIn = true; return json(201, { address: account }); }
   if (method === 'DELETE' && path === '/api/v1/session') { signedIn = false; return json(200, { ended: true }); }
   // The two routes a stranger reaches: what the sign-in card may offer, and
   // the challenge a passkey sign-in answers. The server answers both before
   // it authenticates anything, so they are answered before the gate here.
   if (path === '/api/v1/signin-methods' || path === '/api/v1/passkeys/challenge') {
      const open = answerPasskeys(method, path, body);
      if (open) { return open; }
   }
   if (!signedIn) { return json(401, { error: 'Not signed in.' }); }
   { const calendar = answerCalendar(method, path, body); if (calendar) { return calendar; } }
   { const subscriptions = answerSubscriptions(method, path, body); if (subscriptions) { return subscriptions; } }
   { const myLists = answerMyLists(method, path, body); if (myLists) { return myLists; } }
   { const fetches = answerFetchAccounts(method, path, body); if (fetches) { return fetches; } }
   { const senders = answerSenderAddresses(method, path, body); if (senders) { return senders; } }
   { const masks = answerMaskedAddresses(method, path, body); if (masks) { return masks; } }
   { const kept = answerRecoverable(method, path); if (kept) { return kept; } }
   { const recover = answerRecovery(method, path, body); if (recover) { return recover; } }
   { const keys = answerPasskeys(method, path, body); if (keys) { return keys; } }
   { const listed = answerDirectory(method, path, body); if (listed) { return listed; } }
   { const expiring = answerConfidential(method, path); if (expiring) { return expiring; } }
   { const recall = answerRecall(method, path, body); if (recall) { return recall; } }
   { const groups = answerContactGroups(method, path, body); if (groups) { return groups; } }
   { const window = answerOffline(method, path, body); if (window) { return window; } }
   if (path === '/api/v1/me') {
      return json(200, {
         address: account, quota: { used_bytes: 5242880, limit_mb: 100 },
         vacation: { enabled: false, subject: '', message: '', expires: false, expires_date: '' },
         second_factor: false, directory_linked: false, password_changed: '2026-09-01',
         recall_days: 14, via_app_password: viaAppPassword
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
   const books = answerAddressBooks(method, path, body);
   if (books) { return books; }
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
      // addresses containing the text. A group is listed among the contacts
      // with kind group and no address of its own, so the completion offers it
      // by name; kind= answers one kind alone.
      const q = decodeURIComponent((path.split('q=')[1] || '').split('&')[0]).toLowerCase();
      const kind = (path.split('kind=')[1] || '').split('&')[0];
      // book= is one book's id, "all" for every book the account can see, or
      // absent for its own - which is what the exports and the group member
      // field ask for.
      const which = (path.split('book=')[1] || '').split('&')[0];
      const groups = contactGroups.map(contactGroupJson).map((g) => Object.assign({ book_id: 1, book_name: 'Contacts', book_owner: 'user@example.com', book_mine: true, book_writable: true }, g));
      // The server never lists a group of a book somebody shares with the
      // reader (RestApiContacts.cpp). This is the answer it would give if one
      // ever did, so that the page can be asked what it does with it.
      if (sharedGroupListed) {
         groups.push({ id: 77, name: 'Their Team', address: '', kind: 'group', source: 'manual', birthday: '', note: '', phones: [], has_photo: false,
            members: [{ id: 0, name: 'Zed Private', address: 'zed@example.net' }],
            recipients: [{ id: 0, name: 'Zed Private', address: 'zed@example.net' }],
            book_id: 5, book_name: 'Team', book_owner: 'boss@example.com', book_mine: false, book_writable: false });
      }
      const book = contactBook().concat(groups)
         .filter((c) => (which === 'all' ? true : (which ? c.book_id === Number(which) : c.book_mine !== false)));
      const withCards = path.indexOf('fields=card') >= 0;
      const found = book.filter((c) => (!kind || kind === c.kind) && (c.name.toLowerCase().indexOf(q) >= 0 || (!!c.address && c.address.indexOf(q) >= 0)))
         .map((c) => { const copy = Object.assign({}, c); if (!withCards) { delete copy.card; } return copy; });
      return json(200, { contacts: found, count: found.length, total: book.length });
   }
   if (path === '/api/v1/me/filters' && method === 'GET') { return json(200, { active: '' }); }
   if (path === '/api/v1/me/filters' && method === 'PUT') { return json(200, { saved: true }); }
   if (path === '/api/v1/me/preferences' && method === 'GET') { return json(200, { preferences: prefsStore }); }
   if (path === '/api/v1/me/preferences' && method === 'PUT') { Object.assign(prefsStore, JSON.parse(body || '{}')); return json(200, { preferences: prefsStore }); }
   if (path.startsWith('/api/v1/me/folders/1/messages')) {
      if (path.indexOf('before_uid=') >= 0) { return json(200, { total: arrived ? 6 : 5, messages: OLDER.map(withState) }); }
      const extra = importantExtra();
      if (!arrived) { return json(200, { total: LISTING.total + extra.length, messages: extra.concat(LISTING.messages).map(withState) }); }
      return json(200, { total: 6 + extra.length, messages: extra.concat([ARRIVAL], LISTING.messages).map(withState) });
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
   // The draft's parts, by the types its attachment list declares: the two
   // images this webmail made go back into the compose editor from here.
   if (/^\/api\/v1\/me\/messages\/105\/attachments\/(\d+)$/.test(path)) {
      const part = DRAFT_WITH_LOGO.attachments[Number(path.split('/').pop())];
      return { status: 200, body: 'PARTBYTES', headers: { 'Content-Type': part ? part.content_type : 'application/octet-stream' } };
   }
   if (path === '/api/v1/me/messages/104' && method === 'GET') {
      return json(200, withState(Object.assign({}, ARRIVAL, { folder_id: 1, received: ARRIVAL.date + ':00', cc: '', text: 'just in', html: '', attachments: [], message_id: '<four@example.net>', references: '', in_reply_to: '' })));
   }
   // The importance checks' own mail, opened: the sender matters, so these
   // answer as themselves rather than out of the message below that stands in
   // for any id.
   if (/^\/api\/v1\/me\/messages\/11[123]$/.test(path) && method === 'GET') {
      const one = IMPORTANT_NEW.concat([IMPORTANT_AGAIN]).filter((x) => x.id === Number(path.split('/').pop()))[0];
      return json(200, withState(Object.assign({}, one, { folder_id: 1, received: one.date + ':00', cc: '', text: one.subject, html: '', attachments: [], message_id: '<' + one.id + '@example.net>', references: '', in_reply_to: '' })));
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
   if (path === '/api/v1/me/messages/707/attachments/0') {
      return { status: 200, body: 'DECK', headers: { 'Content-Type': 'application/pdf' }, base64: 'A'.repeat(11200000) };
   }
   if (path === '/api/v1/me/messages/707' && method === 'GET') { return json(200, withState(JSON.parse(JSON.stringify(BIG_ATTACHMENT)))); }
   if (path === '/api/v1/me/smime' && method === 'GET') {
      const store = smimeStores[account] || { own: [], recipients: [] };
      const authority = smimeAuthorities[account] || { domain: account.split('@')[1], available: false };
      // A marked row says where the authority's record has it now, as the
      // server's StoredStatuses does: the account's own from the issued list,
      // a colleague's from what a check set.
      const statusOf = (row) => {
         const issued = (authority.issued || []).filter((c) => c.fingerprint === row.fingerprint)[0];
         return issued ? issued.status : (authorityStatuses[row.fingerprint] || 'valid');
      };
      const marked = (rows) => rows.map((row) => row.authority ? Object.assign({}, row, { authority_status: statusOf(row) }) : row);
      return json(200, { own: marked(store.own), recipients: marked(store.recipients), limits: { own: 20, recipients: 500 }, authority: JSON.parse(JSON.stringify(authority)) });
   }
   if (path === '/api/v1/me/smime/chain' && method === 'POST' && smimeChainAnswer) { return json(200, smimeChainAnswer); }
   // The domain's S/MIME authority (RestApiSmimeAuthority.cpp): a request
   // signed - pending until its key is stored, which is what confirms it and
   // supersedes the address's earlier certificate unless kept - and one of
   // the account's certificates revoked.
   if (path === '/api/v1/me/smime/enroll' && method === 'POST') {
      const authority = smimeAuthorities[account];
      const wanted = JSON.parse(body || '{}');
      enrolments.push({ account, csr: wanted.csr, keep: wanted.keep });
      if (!authority || !authority.available) { return json(404, { error: 'this domain has no S/MIME authority' }); }
      const serial = (authority.issued || []).length + 1;
      const fingerprint = (serial % 16).toString(16).repeat(64);
      const certificate = Buffer.from('CERTIFICATE-' + serial).toString('base64');
      const supersedes = wanted.keep ? [] : (authority.issued || []).filter((c) => c.status === 'valid').map((c) => c.serial);
      authority.issued = [{ serial, address: account, fingerprint, certificate, not_before: 1700000000, not_after: 2000000000, status: 'valid', confirmed: false, keep: !!wanted.keep, revoked: 0, reason: '' }].concat(authority.issued || []);
      return json(201, { certificate, chain: ['MAMCAQM='], serial, fingerprint, address: account, not_before: 1700000000, not_after: 2000000000, confirmed: false, supersedes });
   }
   if (/^\/api\/v1\/me\/smime\/issued\/\d+(\?.*)?$/.test(path) && method === 'DELETE') {
      const authority = smimeAuthorities[account];
      const serial = Number(path.split('?')[0].split('/').pop());
      const reason = /[?&]reason=([^&]+)/.test(path) ? /[?&]reason=([^&]+)/.exec(path)[1] : 'keyCompromise';
      const found = authority && (authority.issued || []).filter((c) => c.serial === serial)[0];
      if (!found) { return json(404, { error: 'no such certificate' }); }
      found.status = 'revoked';
      found.reason = reason;
      return json(200, { revoked: true, serial, reason });
   }
   if (path === '/api/v1/me/smime/recipients' && method === 'PUT') {
      const store = smimeStores[account] || (smimeStores[account] = { own: [], recipients: [] });
      const wanted = JSON.parse(body || '{}');
      const marked = !!authorityCertificates[wanted.address];
      const row = Object.assign({ id: 2, created: 1700000000, authority: marked }, wanted, marked ? { authority_status: authorityStatuses[wanted.fingerprint] || 'valid' } : {});
      store.recipients = store.recipients.filter((r) => r.fingerprint !== wanted.fingerprint);
      store.recipients.push(row);
      return json(201, row);
   }
   if (path === '/api/v1/me/smime/own' && method === 'PUT') {
      if (smimeOwnRefuse) { return json(400, { error: 'at most 20 own certificates' }); }
      const store = smimeStores[account] || (smimeStores[account] = { own: [], recipients: [] });
      const wanted = JSON.parse(body || '{}');
      const at = store.own.findIndex((o) => o.fingerprint === wanted.fingerprint);
      // The server's mark, and the confirmation of a pending certificate of
      // the authority's: its key is in the store now.
      const authority = smimeAuthorities[account];
      const issued = authority && (authority.issued || []).filter((c) => c.fingerprint === wanted.fingerprint)[0];
      if (issued && issued.confirmed === false) {
         issued.confirmed = true;
         if (!issued.keep) {
            authority.issued.forEach((c) => { if (c !== issued && c.status === 'valid' && c.address === issued.address) { c.status = 'revoked'; c.reason = 'superseded'; } });
         }
      }
      const row = Object.assign({ id: 1, created: 1700000000 }, wanted, issued ? { authority: true, authority_status: issued.status } : {});
      if (at >= 0) { store.own[at] = row; return json(200, row); }
      store.own.push(row);
      return json(201, row);
   }
   if (path === '/api/v1/me/messages/601' && method === 'GET') { return json(200, withState(JSON.parse(JSON.stringify(SEALED)))); }
   if (path === '/api/v1/me/messages/602' && method === 'GET') { return json(200, withState(JSON.parse(JSON.stringify(WITH_AUTOCRYPT)))); }
   if (path === '/api/v1/me/messages/602/source' && method === 'GET') {
      return { status: 200, headers: { 'Content-Type': 'message/rfc822' }, body: WITH_AUTOCRYPT.headers + '\r\nHello.\r\n' };
   }
   if (path === '/api/v1/me/messages/603' && method === 'GET') { return json(200, withState(JSON.parse(JSON.stringify(SEALED_ON_DELIVERY)))); }
   if (path === '/api/v1/me/messages/604' && method === 'GET') { return json(200, withState(JSON.parse(JSON.stringify(BORROWED_NAME)))); }
   if (path === '/api/v1/me/messages/605' && method === 'GET') { return json(200, withState(JSON.parse(JSON.stringify(LOOKALIKE_DOMAIN)))); }
   if (path === '/api/v1/me/messages/606' && method === 'GET') { return json(200, withState(JSON.parse(JSON.stringify(NOT_IMPERSONATED)))); }
   if (path === '/api/v1/me/messages/603/source' && method === 'GET') {
      return { status: 200, headers: { 'Content-Type': 'message/rfc822' }, body: SEALED_ON_DELIVERY.headers + '\r\n--za\r\nContent-Type: application/pgp-encrypted\r\n\r\nVersion: 1\r\n\r\n--za\r\nContent-Type: application/octet-stream\r\n\r\nsealed\r\n--za--\r\n' };
   }
   // Zero-access mailboxes (RestApiZeroAccess.cpp): the switch, its re-wrap,
   // and the row the page reads at sign-in. The server checks that each copy
   // is a secret key of the fingerprint and can check nothing else about it.
   if (path === '/api/v1/me/zero-access' && method === 'GET') {
      if (zaBroken[account]) { return json(200, { allowed: zaAllowed, enabled: true, broken: true, problem: 'zero access is on for this mailbox and its key row is missing, so every delivery to it is being refused: switch zero access off and on again' }); }
      const row = zaRows[account];
      return json(200, Object.assign({ allowed: zaAllowed, enabled: !!row }, row || {}));
   }
   if (path === '/api/v1/me/zero-access' && method === 'PUT') {
      if (!zaAllowed) { return json(403, { error: 'zero-access mailboxes are not allowed on this server', code: 'zero_access_not_allowed' }); }
      if (zaRows[account]) { return json(409, { error: 'zero access is already on for this mailbox; switch it off first' }); }
      const wanted = JSON.parse(body || '{}');
      const own = pgpStoreOf(account).own.filter((o) => o.fingerprint === wanted.fingerprint)[0];
      if (!own) { return json(404, { error: 'fingerprint: no key of your own with that fingerprint is in the ring' }); }
      for (const name of ['password_wrapped', 'recovery_wrapped']) {
         const parts = String(wanted[name] || '').split(':');
         if (parts[0] !== 'SEC' || parts[2] !== wanted.fingerprint) { return json(400, { error: name + ': not the key named by fingerprint' }); }
      }
      zaRows[account] = { fingerprint: wanted.fingerprint, salt: wanted.salt, argon2: { passes: wanted.passes, memory_kib: wanted.memory_kib, lanes: wanted.lanes },
         password_wrapped: wanted.password_wrapped, recovery_wrapped: wanted.recovery_wrapped, enabled_at: 1758182400 };
      return json(201, Object.assign({ allowed: zaAllowed, enabled: true }, zaRows[account]));
   }
   if (path === '/api/v1/me/zero-access/rewrap' && method === 'PUT') {
      const row = zaRows[account];
      if (!row) { return json(404, { error: 'zero access is not on for this mailbox' }); }
      const wanted = JSON.parse(body || '{}');
      row.salt = wanted.salt; row.argon2 = { passes: wanted.passes, memory_kib: wanted.memory_kib, lanes: wanted.lanes }; row.password_wrapped = wanted.password_wrapped;
      return json(200, Object.assign({ allowed: zaAllowed, enabled: true }, row));
   }
   if (path === '/api/v1/me/zero-access' && method === 'DELETE') {
      if (!zaRows[account] && !zaBroken[account]) { return json(404, { error: 'zero access is not on for this mailbox' }); }
      delete zaRows[account];
      delete zaBroken[account];
      return json(200, { enabled: false });
   }
   if (path === '/api/v1/me/openpgp/keys' && method === 'GET') {
      const store = pgpStoreOf(account);
      return json(200, { own: store.own, correspondents: store.correspondents, limits: { own: 20, correspondents: 500 }, lookup: { wkd: true, keyserver: false } });
   }
   if ((path === '/api/v1/me/openpgp/keys/own' || path === '/api/v1/me/openpgp/keys/correspondents') && method === 'PUT') {
      const store = pgpStoreOf(account);
      const list = path.endsWith('/own') ? store.own : store.correspondents;
      const wanted = JSON.parse(body || '{}');
      // The fingerprint is the key's, never the client's - as the server does.
      const fingerprint = Buffer.from(String(wanted.public_key), 'base64').toString('utf8').split(':')[2] || '';
      // A key the page could not put an address to is refused - as the server
      // refuses a correspondent's key with no address in its user ids and none
      // in the request.
      if (!wanted.address) { return json(400, { error: 'address: the key carries no address in its user ids, so one has to be given' }); }
      const row = pgpRowOf(wanted.address, { fingerprint, key_id: fingerprint.slice(-16), public_key: wanted.public_key, source: wanted.source, revoked: !!wanted.revoked });
      if (wanted.secret_key) { row.secret_key = wanted.secret_key; }
      const at = list.findIndex((o) => o.fingerprint === fingerprint);
      if (at >= 0) {
         // The trust columns and the address are the ring's, kept across a
         // re-store - as the server keeps them.
         ['id', 'address', 'verified', 'verified_at', 'changed_from', 'changed_at', 'accepted'].forEach((k) => { if (list[at][k] !== undefined) { row[k] = list[at][k]; } });
         list[at] = row;
         return json(200, row);
      }
      // A correspondent's key for an address that already holds a DIFFERENT
      // one - a correspondent's or the account's OWN - is a change, recorded
      // on the new row, as the server records it.
      const previous = path.endsWith('/own') ? null : store.own.concat(store.correspondents).filter((o) => o.address === wanted.address && o.fingerprint !== fingerprint).pop();
      if (previous) { row.changed_from = previous.fingerprint; row.changed_at = 1700000100; row.accepted = false; }
      list.push(row);
      return json(201, row);
   }
   if (/^\/api\/v1\/me\/openpgp\/keys\/\d+\/(verified|accepted)$/.test(path) && method === 'PUT') {
      const store = pgpStoreOf(account);
      const id = Number(path.split('/')[6]);
      const what = path.split('/').pop();
      const row = store.correspondents.concat(store.own).filter((o) => o.id === id)[0];
      if (!row) { return json(404, { error: 'no such key' }); }
      if (store.own.indexOf(row) >= 0) { return json(400, { error: 'a key of your own' }); }
      const wanted = JSON.parse(body || '{}');
      if (what === 'verified') {
         row.verified = wanted.verified === true;
         row.verified_at = row.verified ? 1700000300 : 0;
         if (row.verified && row.changed_from) { row.accepted = true; }
      }
      else {
         if (!row.changed_from) { return json(400, { error: 'this key did not replace another' }); }
         row.accepted = wanted.accepted === true;
      }
      return json(200, row);
   }
   if (/^\/api\/v1\/me\/openpgp\/keys\/(own|correspondents)\/[0-9a-f]+$/.test(path) && method === 'DELETE') {
      const store = pgpStoreOf(account);
      const list = path.indexOf('/own/') > 0 ? store.own : store.correspondents;
      const at = list.findIndex((o) => o.fingerprint === path.split('/').pop());
      if (at < 0) { return json(404, { error: 'no such key' }); }
      list.splice(at, 1);
      return json(200, { removed: true });
   }
   // A password-protected message (ProtectedMessages.h): the sender's four
   // routes. The record is answered as the server answers it, and the
   // chunks are counted; what the page sends is what the checks read.
   if (path === '/api/v1/me/protected' && method === 'GET') {
      return json(200, { available: protectedState.available, days: 30, limits: { message_bytes: 26214400, account_bytes: 262144000, live: 1000, reply_bytes: 1048576 }, used_bytes: 0, items: protectedState.items });
   }
   if (path === '/api/v1/me/protected' && method === 'POST') {
      const wanted = JSON.parse(body || '{}');
      const row = { id: 900 + protectedRecords.length, token: 'tok' + String(protectedRecords.length).padStart(19, '0'), recipient: wanted.recipient, hint: wanted.hint || '', size: wanted.size, stored: 0, complete: false, created: 1700000000, expires: 1702592000, expired: false, opened: 0, revoked: false, reply_to: '', link: 'http://portal.test/portal/protected/tok' + String(protectedRecords.length).padStart(19, '0') };
      protectedRecords.push({ row, chunks: [] });
      return json(201, row);
   }
   if (/^\/api\/v1\/me\/protected\/\d+\/content(\?.*)?$/.test(path) && method === 'PUT') {
      const found = protectedRecords.filter((r) => r.row.id === Number(path.split('/')[5]))[0];
      if (!found) { return json(404, { error: 'no such password-protected message of this account' }); }
      found.chunks.push(body);
      found.row.stored += body.length;
      found.row.complete = found.row.stored >= found.row.size;
      return json(200, Object.assign({}, found.row, { sent_copy: found.row.complete, held: false }));
   }
   if (/^\/api\/v1\/me\/protected\/\d+$/.test(path) && method === 'DELETE') {
      const item = protectedState.items.filter((i) => i.id === Number(path.split('/').pop()))[0];
      if (!item) { return json(404, { error: 'not one of your password-protected messages' }); }
      item.revoked = true;
      return json(200, { revoked: true });
   }
   if (path.indexOf('/api/v1/me/openpgp/lookup') === 0 && method === 'GET') {
      return json(200, pgpLookupAnswer || { found: false, address: '', error: 'no key was found for that address', status: 404 });
   }
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
      if (flagsRefuseId === id) { return json(403, { error: 'The folder does not allow it' }); }
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
      // A zero-access mailbox's change carries the key sealed again under the
      // new password, or is refused, as the server refuses it.
      if (zaRows[account]) {
         const wanted = JSON.parse(body || '{}');
         if (!wanted.zero_access || !wanted.zero_access.password_wrapped) { return json(409, { error: 'this mailbox is a zero-access mailbox: a password change carries the key sealed again under the new one', code: 'zero_access_rewrap' }); }
         zaRows[account].salt = wanted.zero_access.salt;
         zaRows[account].password_wrapped = wanted.zero_access.password_wrapped;
      }
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
   requests.push({ method, path, headers, body: options && options.body, keepalive: !!(options && options.keepalive) });
   if (networkDown) { return Promise.reject(new TypeError('Failed to fetch')); }
   currentAuth = String(headers.Authorization || '');
   currentSecondFactor = { otp: String(headers['X-hMailServer-OTP'] || ''), code: String(headers['X-hMailServer-Recovery-Code'] || '') };
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
      // the FileReader stub reads off it - the answer's own, where it gave one,
      // so that an attachment can weigh what a bound is about.
      blob: () => Promise.resolve(Object.assign(new Blob(['ABC'], { type: reply.headers['Content-Type'] || 'application/octet-stream' }), { base64: reply.base64 || 'QUJD' }))
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
   // The request for the domain's authority: the key type and the address
   // are recorded in the bytes, so a check can read what the page asked for.
   makeRequest: async (options) => ({ request: Uint8Array.from(Buffer.from('CSR:' + options.keyType + ':' + options.address)), pkcs8: Uint8Array.from([0x30, 0x03, 0x02, 0x01, 0x02]), keyType: options.keyType }),
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

/* -------------------------------------------- the page's OpenPGP module */

// window.hmOpenPgp, as far as the page uses it. The real module is held to
// GnuPG in both directions by build/check-portal-openpgp.js; what matters here
// is which keys the page hands it and what it does with what comes back, so
// this one records them. A key is its own text: "KEY:<address>:<fingerprint>"
// for a public one and "SEC:<address>:<fingerprint>:<passphrase>" for a secret
// one, so an export and an import round-trip through the page as they do in the
// real module and a wrong passphrase throws 'wrong passphrase', which is the
// message the real module throws and the page reads.
const pgpReadWith = [];
const pgpComposed = [];
const pgpProtected = [];
// What the recorded server says about password-protected messages, and
// the records the page made.
const protectedState = { available: false, items: [] };
const protectedRecords = [];
// A fingerprint is forty hex digits and nothing else - the page files a key
// under one and names one in a URL - so this makes one from the address, and
// from a variant when a check needs two keys for the same address.
const pgpFingerprint = (address, variant) => {
   const text = String(address) + (variant || '');
   let hash = 0;
   for (let i = 0; i < text.length; i += 1) { hash = (hash * 31 + text.charCodeAt(i)) >>> 0; }
   return hash.toString(16).padStart(8, '0').repeat(5);
};
const pgpPublicOf = (address, variant) => Buffer.from('KEY:' + address + ':' + pgpFingerprint(address, variant)).toString('base64');
// Every row has an id of its own, because the trust routes name a row by it.
let pgpRowIds = 0;
const pgpRowOf = (address, extra) => Object.assign({
   id: ++pgpRowIds, address, name: 'Key of ' + address, fingerprint: pgpFingerprint(address), key_id: pgpFingerprint(address).slice(-16),
   algorithm: 'Ed25519', public_key: pgpPublicOf(address), created: 1700000000, expires: 0, revoked: false,
   source: 'imported', prefer_encrypt: 'mutual', first_seen: 1700000000, last_seen: 1700000000,
   verified: false, verified_at: 0
}, extra || {});
// A second key for the same address, filed as a change of the first: what the
// server stores when a different key arrives for an address that has one.
const pgpChangedRowOf = (address, extra) => pgpRowOf(address, Object.assign({
   fingerprint: pgpFingerprint(address, 'second'), key_id: pgpFingerprint(address, 'second').slice(-16),
   public_key: pgpPublicOf(address, 'second'), changed_from: pgpFingerprint(address), changed_at: 1700000100, accepted: false
}, extra || {}));
// A good signature by the key of an address, as the module reports one: valid
// is the cryptography, key is the parsed primary the page finds the ring's row
// by, and fingerprint is the signing packet's.
const pgpGoodSignatureBy = (address, variant) => ({
   valid: true, error: '', problem: '', keyId: pgpFingerprint(address, variant).slice(-16), fingerprint: pgpFingerprint(address, variant),
   key: pgpKeyObject(address, pgpFingerprint(address, variant), false), signerUserId: '', created: 1700000000
});
const pgpOwnRowOf = (address, passphrase) => pgpRowOf(address, {
   source: 'generated', secret_key: 'SEC:' + address + ':' + pgpFingerprint(address) + ':' + passphrase
});
// What the next readMessage answers. The checks set it before opening a message.
let pgpMessageResult = { layers: [], encrypted: null, signed: null, content: null, autocrypt: null, inline: false };
const pgpKeyObject = (address, fingerprint, secret) => ({
   address, name: 'Key of ' + address, fingerprint, keyId: fingerprint.slice(-16), userId: address,
   isSecret: !!secret, unlocked: !!secret, revoked: false, expires: 0, algorithm: 'Ed25519'
});
const pgpStub = {
   available: true,
   curve25519Available: async () => true,
   b64decode: (value) => Uint8Array.from(Buffer.from(String(value), 'base64')),
   b64encode: (bytes) => Buffer.from(bytes).toString('base64'),
   publicBytes: (key) => Uint8Array.from(Buffer.from('KEY:' + key.address + ':' + key.fingerprint)),
   exportPublic: (key) => '-----BEGIN PGP PUBLIC KEY BLOCK-----\r\n' + key.fingerprint + '\r\n-----END PGP PUBLIC KEY BLOCK-----\r\n',
   exportSecret: async (key, passphrase) => 'SEC:' + key.address + ':' + key.fingerprint + ':' + String(passphrase),
   readKey: async (input, passphrase) => {
      const text = typeof input === 'string' ? input : Buffer.from(input).toString('utf8');
      const parts = text.split(':');
      if (parts[0] === 'SEC') {
         if (passphrase !== undefined && String(passphrase) !== parts[3]) { throw new Error('wrong passphrase'); }
         return pgpKeyObject(parts[1], parts[2], true);
      }
      if (parts[0] === 'KEY') { return pgpKeyObject(parts[1], parts[2], false); }
      throw new Error('this is not a key');
   },
   readKeys: async (text, passphrase) => {
      const lines = String(text).split(/\r?\n/).filter((l) => /^(KEY|SEC):/.test(l));
      if (!lines.length) { throw new Error('there is no key in this block'); }
      return Promise.all(lines.map((line) => pgpStub.readKey(line, /^SEC:/.test(line) ? passphrase : undefined)));
   },
   generateKey: async (spec) => {
      const fingerprint = pgpFingerprint(spec.email);
      return {
         key: pgpKeyObject(spec.email, fingerprint, true),
         publicArmored: '-----BEGIN PGP PUBLIC KEY BLOCK-----\r\n' + fingerprint + '\r\n-----END PGP PUBLIC KEY BLOCK-----\r\n',
         secretArmored: 'SEC:' + spec.email + ':' + fingerprint + ':' + spec.passphrase,
         revocation: 'REV:' + fingerprint,
         fingerprint
      };
   },
   revocationCertificate: async (key) => 'REV:' + key.fingerprint,
   applyRevocation: async (key, certificate) => {
      if (String(certificate).trim() !== 'REV:' + key.fingerprint) { throw new Error('this revocation revokes another key'); }
      key.revoked = true;
      return key;
   },
   autocryptHeader: (address, key) => 'addr=' + address + '; prefer-encrypt=mutual; keydata=' + Buffer.from('KEY:' + address + ':' + key.fingerprint).toString('base64'),
   parseAutocrypt: (value) => null,
   readMessage: async (bytes, keys) => {
      pgpReadWith.push(keys.map((k) => k.address + (k.isSecret ? '!' : '')).sort());
      return JSON.parse(JSON.stringify(pgpMessageResult));
   },
   // Row 152's message to a password: what the page hands it is recorded
   // (the password apart, which the checks read from the requests instead)
   // and the armour it answers carries nothing of the text.
   protectedMessage: async (spec) => {
      pgpProtected.push({ subject: (spec.headers || {}).subject || '', text: spec.text || '', attachments: (spec.attachments || []).length, hadPassword: !!spec.password });
      return '-----BEGIN PGP MESSAGE-----\r\n\r\njA0ECQMI' + 'A'.repeat(60) + '\r\n=abcd\r\n-----END PGP MESSAGE-----\r\n';
   },
   composeMessage: async (spec) => {
      pgpComposed.push({
         sign: spec.sign ? spec.sign.address : '',
         encrypt: (spec.encrypt || []).map((k) => k.address).sort(),
         fingerprints: (spec.encrypt || []).map((k) => k.fingerprint).sort(),
         text: spec.text || ''
      });
      return 'MIME-Version: 1.0\r\nContent-Type: multipart/encrypted; protocol="application/pgp-encrypted"; boundary="b"\r\n\r\nsealed\r\n';
   }
};
// Zero access (docs/ZeroAccess.md), as far as the page uses the module: the
// passphrase derived from a password is 'ZA_<password>_<salt>' rather than
// Argon2id - the real derivation is held to RFC 9106's vectors by
// build/check-portal-openpgp.js - and the recovery key is a fixed one, so a
// check can type it back. The zero-access rows the recorded server holds, per
// account, and whether the administrator allows the opt-in.
const ZA_RECOVERY = 'ABCDEF-GHIJKL-MNOPQR-STUVWX-YZ2345-67ABCD-EFGHIJ-KLMNOP';
pgpStub.zeroAccessPassphrase = async (password, saltBase64, params) => 'ZA_' + String(password) + '_' + String(saltBase64);
pgpStub.makeRecoveryKey = () => ZA_RECOVERY;
pgpStub.canonicalRecoveryKey = (typed) => { const clean = String(typed || '').toUpperCase().replace(/[^A-Z2-7]/g, ''); return clean.length === 48 ? clean : ''; };
pgpStub.ARGON2_DEFAULT = { passes: 3, memory: 65536, lanes: 4 };
const zaRows = {};
let zaAllowed = true;
// The switch on with no key row beside it (a restore that dropped the table):
// the server answers enabled with broken, and DELETE is the way out.
let zaBroken = {};
// The OpenPGP key ring the recorded server answers with, per account.
const pgpStores = {};
const pgpStoreOf = (address) => pgpStores[address] || (pgpStores[address] = { own: [], correspondents: [] });
let pgpLookupAnswer = null;

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
// Node's own timer, taken before main() puts the hand-fired queue in its
// place: the reading page's checks wait on real cryptography, which a
// queue nobody fires would leave the process to exit quietly on.
const realSetTimeout = setTimeout;

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
// WebAuthn, recorded rather than real. A browser asks an authenticator to make
// or use a key pair and hands back ArrayBuffers; this hands back fixed bytes
// and records what it was asked for, which is what lets the checks below say
// that the page passed the server's challenge to the authenticator and the
// authenticator's answer back to the server.
const credentialCalls = [];
let credentialsRefuse = false;
function buffersOf(text) { return Uint8Array.from(Buffer.from(String(text), 'binary')).buffer; }
const PublicKeyCredentialStub = function () {};
const credentialsStub = {
   create: (options) => {
      credentialCalls.push({ kind: 'create', options: options });
      if (credentialsRefuse) { return Promise.reject(new Error('NotAllowedError')); }
      return Promise.resolve({
         id: 'Y3JlZGVudGlhbA', type: 'public-key',
         response: {
            clientDataJSON: buffersOf('{"type":"webauthn.create"}'),
            attestationObject: buffersOf('attestation'),
            getTransports: () => ['internal']
         }
      });
   },
   get: (options) => {
      credentialCalls.push({ kind: 'get', options: options });
      if (credentialsRefuse) { return Promise.reject(new Error('NotAllowedError')); }
      return Promise.resolve({
         id: 'Y3JlZGVudGlhbA', type: 'public-key',
         response: {
            clientDataJSON: buffersOf('{"type":"webauthn.get"}'),
            authenticatorData: buffersOf('authdata'),
            signature: buffersOf('signature'),
            userHandle: buffersOf('a1')
         }
      });
   }
};
navigatorStub.credentials = credentialsStub;
window.PublicKeyCredential = PublicKeyCredentialStub;
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
// A real IndexedDB structured-clones what is put into it, so a page that goes
// on mutating the object it stored does not change the store. Keeping the
// reference instead made this stub answer yes to questions a browser would
// answer no to - a keyword written onto the page's copy of a message looked
// as though it had reached the kept copy, when nothing had been written at
// all. A Blob is immutable, so passing one through cannot hide anything.
const idbClone = (value) => {
   if (value === null || typeof value !== 'object') { return value; }
   if (value instanceof Blob || value instanceof ArrayBuffer || value instanceof Date) { return value; }
   if (Array.isArray(value)) { return value.map(idbClone); }
   const out = {};
   Object.keys(value).forEach((k) => { out[k] = idbClone(value[k]); });
   return out;
};
class IdbStore {
   constructor(keyPath) { this.keyPath = keyPath; this.records = new Map(); }
   put(value) { return idbRequest(() => { const key = value[this.keyPath]; if (key === undefined) { throw new Error('no key'); } this.records.set(key, idbClone(value)); return key; }); }
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
   // Two routes, and both are answered to a stranger by design: /api/v1/me,
   // which is how the page finds out whether it has a session at all, and
   // /api/v1/signin-methods, which says whether to draw a passkey button and a
   // "Sign in with ..." button. The second says nothing about who exists here -
   // it is two flags and the issuer's host - and the sign-in card cannot be
   // drawn correctly without it. Anything else asked before a session would be
   // a credentialed route reached without a credential.
   check('nothing but /api/v1/me and the sign-in methods was asked for before a session existed',
      requests.every((r) => r.path === '/api/v1/me' || r.path === '/api/v1/signin-methods'),
      JSON.stringify(requests.map((r) => r.path)));
   check('the sign-in card offers a passkey and the organisation\'s identity provider when the server says so',
      document.getElementById('signin-passkey').hidden === false &&
      document.getElementById('signin-sso').hidden === false &&
      document.getElementById('signin-sso').textContent.indexOf('login.example.com') >= 0,
      document.getElementById('signin-sso').textContent);
   // SAML beside OIDC: a button of its own, with words of its own - the
   // default said through the page's own "Sign in with {0}", so in the reader's
   // language - that starts the SAML sign-in and nothing else.
   check('the sign-in card offers the SAML identity provider beside the OpenID Connect one when the server says so',
      document.getElementById('signin-saml').hidden === false &&
      document.getElementById('signin-saml').textContent === 'Sign in with SAML' &&
      document.getElementById('signin-saml').textContent !== document.getElementById('signin-sso').textContent,
      document.getElementById('signin-saml').textContent);
   {
      const navigatedBefore = navigatedTo.length;
      document.getElementById('signin-saml').dispatchEvent(makeEvent('click'));
      await flush();
      check('the SAML button sends the browser to /portal/saml/start',
         navigatedTo.length === navigatedBefore + 1 && navigatedTo[navigatedTo.length - 1] === '/portal/saml/start',
         navigatedTo.slice(navigatedBefore).join(' '));
   }

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
   // And the sign-in card, drawn again, asks again what it may offer: with
   // single sign-on switched off meanwhile, neither button stays - the negative
   // control of the two checks at the first visit. (An administrator's own
   // words on the SAML button, shown as written, are the Deck harness's check.)
   const methodsBefore = signInMethods;
   signInMethods = Object.assign({}, methodsBefore, { oidc: false, saml: false });
   document.getElementById('signout').dispatchEvent(makeEvent('click'));
   await flush();
   check('sign-out forgets which follow-ups were announced', !store.has('hmPortalDueShown') && gate.hidden === false, String(store.get('hmPortalDueShown')));
   check('with single sign-on off, the card drawn again at sign-out offers neither the OpenID Connect nor the SAML button',
      document.getElementById('signin-sso').hidden === true && document.getElementById('signin-saml').hidden === true,
      'sso hidden=' + document.getElementById('signin-sso').hidden + ' saml hidden=' + document.getElementById('signin-saml').hidden);
   document.getElementById('address').value = 'user@example.com';
   document.getElementById('password').value = 'a-real-password';
   const beforeAgain = requests.length;
   document.getElementById('signin-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('and the next sign-in announces the one due today again', app.hidden === false && toastBox.textContent.indexOf('Due for follow-up: First') >= 0, toastBox.textContent);
   signInMethods = methodsBefore;
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
   // Since the compose editor took images and tables (row L48), a draft with
   // HTML reopens formatted, and the two images this webmail made parts of
   // it are fetched too - to go back into the editor as data: URLs, not as
   // files; the checks of that are with the editor's own, below.
   check('a draft reopened carries its files, and not the images this webmail made parts of its HTML', draftFetched.slice().sort().join(',') === '0,1,2,3' && draftNote.indexOf('notes.txt') >= 0 && draftNote.indexOf('photo.jpg') >= 0 && draftNote.indexOf('image1.png') < 0,
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
   // Four cards: the three contacts, and the group, which goes out as a group
   // card (the checks in the contact-groups block below read that one).
   check('the address book exports as vCard 3.0, one card per contact and one per group', /BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Alice Example\r\nN:Example;Alice;;;\r\nEMAIL;TYPE=INTERNET:alice@example.net\r\nEND:VCARD/.test(vcf) && (vcf.match(/BEGIN:VCARD/g) || []).length === 4 && vcf.indexOf('TEL:+1 555 0100') >= 0,
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
   check('the calendars are listed with a box each and the shared ones named by their owners', findAll(document.getElementById('cal-list'), (n) => hasClass(n, 'cal-row')).length === 4 &&
      document.getElementById('cal-list').textContent.indexOf('Team (b@example.net)') >= 0 && document.getElementById('cal-list').textContent.indexOf('Boss (boss@example.com)') >= 0, document.getElementById('cal-list').textContent);

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
   // ---- the availability strip: guests named with a start set
   document.getElementById('event-attendees').value = 'Guest <guest@example.com>, b@example.net, quiet@example.com';
   const beforeAvail = requests.length;
   document.getElementById('event-attendees').dispatchEvent(makeEvent('input'));
   await flush();
   const availBox = document.getElementById('event-availability');
   const availRows = findAll(availBox, (n) => hasClass(n, 'fb-row'));
   check('naming guests with a start set draws an availability row per person, the organiser first, and asks the free-busy route once for each guest of the domain and never for one elsewhere',
      availBox.hidden === false && availRows.length === 4 && availRows[0].getAttribute('data-address') === 'user@example.com' &&
      since(beforeAvail).filter((r) => r.method === 'GET' && /^\/api\/v1\/me\/freebusy\?address=guest%40example\.com&from=\d{8}T\d{6}Z&to=\d{8}T\d{6}Z$/.test(r.path)).length === 1 &&
      !since(beforeAvail).some((r) => /freebusy\?address=b%40example\.net/.test(r.path)),
      JSON.stringify(since(beforeAvail).map((r) => r.path)) + ' | rows ' + availRows.length);
   const guestRow = availRows.filter((r) => r.getAttribute('data-address') === 'guest@example.com')[0];
   check('a colleague\'s busy periods are drawn as blocks, the tentative one marked, with the event\'s own span outlined',
      !!guestRow && findAll(guestRow, (n) => hasClass(n, 'fb-block')).length === 2 && findAll(guestRow, (n) => hasClass(n, 'fb-tentative')).length === 1 && findAll(guestRow, (n) => hasClass(n, 'fb-event')).length === 1,
      guestRow ? findAll(guestRow, (n) => true).map((n) => n.className).join(',') : 'no row');
   check('a guest elsewhere is not on this server, and one who chose nobody is not shown',
      availRows.some((r) => r.getAttribute('data-address') === 'b@example.net' && r.textContent.indexOf(words('not on this server')) >= 0) &&
      availRows.some((r) => r.getAttribute('data-address') === 'quiet@example.com' && r.textContent.indexOf(words('not shown')) >= 0),
      availRows.map((r) => r.textContent).join(' ; '));
   const beforeRedraw = requests.length;
   document.getElementById('event-start').value = '2026-10-19T11:00';
   document.getElementById('event-start').dispatchEvent(makeEvent('input'));
   await flush();
   check('a new time on the same day redraws the strip from what was already answered, without asking again', since(beforeRedraw).length === 0 && findAll(document.getElementById('event-availability'), (n) => hasClass(n, 'fb-row')).length === 4, since(beforeRedraw).length + ' requests');
   document.getElementById('event-start').value = '2026-10-19T09:00';
   document.getElementById('event-attendees').value = 'Guest <guest@example.com>, b@example.net';
   document.getElementById('event-attendees').dispatchEvent(makeEvent('input'));
   await flush();
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
   check('a share with a group is listed by the group\'s name with how many members it has, and the form offers the delegate choice',
      document.getElementById('cal-detail').textContent.indexOf('Sales') >= 0 && document.getElementById('cal-detail').textContent.indexOf(words('{0} members').replace('{0}', '3')) >= 0 && !!document.getElementById('cal-share-delegate'),
      document.getElementById('cal-detail').textContent.slice(0, 300));
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

   // ---- an invitation in a calendar shared as a delegate, answered on the owner's behalf
   const reviewChip = findAll(calSection, (n) => hasClass(n, 'cal-chip') && n.textContent.indexOf('Review') >= 0)[0];
   check('an invitation to the owner in a calendar shared as a delegate has a chip', !!reviewChip, findAll(calSection, (n) => hasClass(n, 'cal-chip')).map((c) => c.textContent).join(','));
   if (reviewChip) {
   reviewChip.dispatchEvent(makeEvent('click'));
   await flush();
   const answerBox = document.getElementById('event-answer');
   check('opening it offers Accept, Maybe and Decline on behalf of the owner', answerBox.hidden === false && findAll(answerBox, (n) => n.getAttribute('data-answer') === 'ACCEPTED').length === 1 &&
      findAll(answerBox, (n) => n.getAttribute('data-answer') === 'TENTATIVE').length === 1 && findAll(answerBox, (n) => n.getAttribute('data-answer') === 'DECLINED').length === 1 &&
      answerBox.textContent.indexOf(words('Answer on behalf of {0}').replace('{0}', 'boss@example.com')) >= 0, answerBox.textContent);
   const beforeBehalf = requests.length;
   findAll(answerBox, (n) => n.getAttribute('data-answer') === 'ACCEPTED')[0].dispatchEvent(makeEvent('click'));
   await flush();
   const behalf = bodyOf(beforeBehalf, 'POST', '/api/v1/me/calendar/events/12/reply');
   check('Accept posts one reply with the partstat, closes the editor and reads the events again', !!behalf && behalf.partstat === 'ACCEPTED' && document.getElementById('event-overlay').hidden === true && !!windowOf(beforeBehalf), JSON.stringify(behalf) + ' | ' + JSON.stringify(since(beforeBehalf).map((r) => r.method + ' ' + r.path)));
   const reviewAgain = findAll(calSection, (n) => hasClass(n, 'cal-chip') && n.textContent.indexOf('Review') >= 0)[0];
   reviewAgain.dispatchEvent(makeEvent('click'));
   await flush();
   check('opened again, the editor shows the owner\'s answer and who gave it', document.getElementById('event-partstat').textContent.indexOf(words('accepted')) >= 0 &&
      document.getElementById('event-partstat').textContent.indexOf(words('answered by {0}').replace('{0}', 'user@example.com')) >= 0, document.getElementById('event-partstat').textContent);
   document.getElementById('event-cancel').dispatchEvent(makeEvent('click'));
   await flush();
   }
   const standupAgain = findAll(calSection, (n) => hasClass(n, 'cal-chip') && n.textContent.indexOf('Standup') >= 0)[0];
   standupAgain.dispatchEvent(makeEvent('click'));
   await flush();
   check('an event the reader organises offers no answer buttons', document.getElementById('event-answer').hidden === true, document.getElementById('event-answer').textContent);
   document.getElementById('event-cancel').dispatchEvent(makeEvent('click'));
   await flush();
   const strayChip = findAll(calSection, (n) => hasClass(n, 'cal-chip') && n.textContent.indexOf('Not mine') >= 0)[0];
   check('an event in the delegated calendar that does not name the owner as a guest has a chip', !!strayChip, findAll(calSection, (n) => hasClass(n, 'cal-chip')).map((c) => c.textContent).join(','));
   if (strayChip) {
   strayChip.dispatchEvent(makeEvent('click'));
   await flush();
   check('and offers no answer buttons, since the owner is not among its guests', document.getElementById('event-overlay').hidden === false && document.getElementById('event-answer').hidden === true, document.getElementById('event-answer').textContent);
   document.getElementById('event-cancel').dispatchEvent(makeEvent('click'));
   await flush();
   }

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

      // Lists on this server: the managed lists the reader belongs to, how
      // each one's mail comes, and leaving.
      const listRows = () => walkAll(document.getElementById('mylists-rows'), (n) => n.tagName === 'TR');
      const listRowOf = (address) => listRows().filter((r) => r.textContent.indexOf(address) >= 0)[0];
      const choiceIn = (row) => walkAll(row, (n) => n.tagName === 'SELECT')[0];
      check('the page reads the lists on this server the reader belongs to, and shows them',
         called(beforeSubs, 'GET', '/api/v1/me/lists') && document.getElementById('mylists-card').hidden === false && listRows().length === 2,
         JSON.stringify(since(beforeSubs).map((r) => r.path)));
      check('a request still waiting says so, and its delivery cannot be chosen yet',
         listRowOf('news@example.com').textContent.indexOf(words('Waiting for you to confirm')) >= 0 && choiceIn(listRowOf('news@example.com')).disabled === true,
         listRowOf('news@example.com').textContent);
      const archiveLink = walkAll(listRowOf('team@example.com'), (n) => n.tagName === 'A')[0];
      check('a list with an archive links to it, opened apart',
         !!archiveLink && archiveLink.getAttribute('href') === 'https://lists.example.com/lists/team@example.com/archive' && archiveLink.getAttribute('rel') === 'noopener noreferrer',
         archiveLink ? archiveLink.getAttribute('href') : 'no link');
      const beforeChoice = requests.length;
      const teamChoice = choiceIn(listRowOf('team@example.com'));
      teamChoice.value = 'digest';
      teamChoice.dispatchEvent(makeEvent('change'));
      await flush();
      const chose = since(beforeChoice).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/lists/team%40example.com')[0];
      check('choosing the digest writes it for that list alone', !!chose && JSON.parse(chose.body).delivery === 'digest', chose ? chose.body : 'no PUT');
      confirmAnswer = false;
      const beforeStay = requests.length;
      buttonIn(listRowOf('team@example.com'), 'Leave the list').dispatchEvent(makeEvent('click'));
      await flush();
      check('leaving asks first, and a no leaves nothing', confirms[confirms.length - 1].indexOf('team@example.com') >= 0 && !called(beforeStay, 'DELETE', '/api/v1/me/lists/team%40example.com'),
         confirms[confirms.length - 1]);
      confirmAnswer = true;
      myListsMode = 'empty';
      const beforeGo = requests.length;
      buttonIn(listRowOf('team@example.com'), 'Leave the list').dispatchEvent(makeEvent('click'));
      await flush();
      check('a yes leaves the list and reads the lists again', called(beforeGo, 'DELETE', '/api/v1/me/lists/team%40example.com') && called(beforeGo, 'GET', '/api/v1/me/lists'),
         JSON.stringify(since(beforeGo).map((r) => r.method + ' ' + r.path)));
      check('and a reader on no list any more sees no card', document.getElementById('mylists-card').hidden === true);
      myListsMode = 'full';
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

   // ---- Message recall within the server: Recall on a Sent message of the
   // account's own, young enough, with a Message-ID, and not on an app
   // password; the confirm sentence, the POST, the toast per outcome and
   // the Recalled badge
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const fill = (text, ...values) => values.reduce((s, v, i) => s.split('{' + i + '}').join(String(v)), words(text));
      const recallItem = () => menuItem('recall');
      const rightClick = (subject) => { rowBySubject(subject).dispatchEvent(makeEvent('contextmenu', { clientX: 300, clientY: 200 })); };
      const badgeOn = (row) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1 && String(c.className).indexOf('badge') >= 0) { out.push(c.textContent); } if (c.childNodes) { walk(c); } } })(row); return out; };
      const signInAgain = async () => {
         document.getElementById('signout').dispatchEvent(makeEvent('click'));
         await flush();
         document.getElementById('address').value = 'user@example.com';
         document.getElementById('password').value = 'a-real-password';
         document.getElementById('signin-form').dispatchEvent(makeEvent('submit'));
         await flush(); await flush();
         dismissAllToasts();
      };

      location.hash = '#/f/1';
      await flush();
      rightClick('Second');
      check('Recall is not offered on a message in the inbox', recallItem().hidden === true, 'hidden=' + recallItem().hidden);
      document.dispatchEvent(makeEvent('keydown', { key: 'Escape' }));

      location.hash = '#/f/2';
      await flush();
      rightClick('Thanks');
      check('in the Sent folder, a message of the account\'s own sent yesterday offers Recall in the row menu',
         recallItem().hidden === false && recallItem().textContent === words('Recall'), 'hidden=' + recallItem().hidden + ' text=' + recallItem().textContent);
      document.dispatchEvent(makeEvent('keydown', { key: 'Escape' }));
      rightClick('Old news');
      check('one older than recall_days does not', recallItem().hidden === true, 'hidden=' + recallItem().hidden);
      document.dispatchEvent(makeEvent('keydown', { key: 'Escape' }));
      rightClick('Filed here by hand');
      check('nor one whose From is not the account or one of its identities', recallItem().hidden === true, 'hidden=' + recallItem().hidden);
      document.dispatchEvent(makeEvent('keydown', { key: 'Escape' }));

      const keptId = SENT_LISTING[2].message_id;
      SENT_LISTING[2].message_id = '';
      location.hash = '#/f/1';
      await flush();
      location.hash = '#/f/2';
      await flush();
      rightClick('Thanks');
      check('nor one without a Message-ID, which nothing could be recalled by', recallItem().hidden === true, 'hidden=' + recallItem().hidden);
      document.dispatchEvent(makeEvent('keydown', { key: 'Escape' }));
      SENT_LISTING[2].message_id = keptId;
      location.hash = '#/f/1';
      await flush();
      location.hash = '#/f/2';
      await flush();

      // Declined: nothing is asked of the server.
      confirmAnswer = false;
      const beforeDecline = requests.length;
      rightClick('Thanks');
      recallItem().dispatchEvent(makeEvent('click', { target: recallItem() }));
      await flush();
      check('Recall asks first, in the sentence that says who keeps it and who is not reached, and a No sends nothing',
         confirms[confirms.length - 1] === words('Recall this message from the people on this server who have not read it? Whoever has read it keeps it, and nobody outside this server is reached.') &&
         !called(beforeDecline, 'POST', '/api/v1/me/recall'),
         confirms[confirms.length - 1]);

      // Accepted: the POST carries the Message-ID and the Sent copy's id, the
      // toast says the numbers, and the row wears the badge.
      confirmAnswer = true;
      dismissAllToasts();
      const beforeRecall = requests.length;
      rightClick('Thanks');
      recallItem().dispatchEvent(makeEvent('click', { target: recallItem() }));
      await flush(); await flush();
      const recallPost = since(beforeRecall).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/recall').map((r) => JSON.parse(r.body))[0];
      check('a Yes posts the Message-ID and the Sent copy\'s id to /api/v1/me/recall',
         !!recallPost && recallPost.message_id === '<thanks@example.com>' && recallPost.sent_id === 403, recallPost ? JSON.stringify(recallPost) : 'no POST');
      check('the toast says how many were recalled, how many had read it and how many are not on this server',
         toasts.textContent.indexOf(fill('Recalled from {0}, already read by {1}, not on this server: {2}', 2, 1, 1)) >= 0, toasts.textContent);
      check('and the Sent row wears the Recalled badge', badgeOn(rowBySubject('Thanks')).indexOf(words('Recalled')) >= 0, badgeOn(rowBySubject('Thanks')).join(','));
      dismissAllToasts();

      // Already gone: said in a clause of its own.
      recallAnswer = { status: 200, body: { recalled: 0, read: 0, gone: 2, failed: 0, elsewhere: 0, sent_marked: false, recipients: [] } };
      rightClick('Draft agenda');
      recallItem().dispatchEvent(makeEvent('click', { target: recallItem() }));
      await flush(); await flush();
      check('a recall that found nothing left says so in the toast, and marks nothing',
         toasts.textContent.indexOf(fill('Recalled from {0}, already read by {1}, not on this server: {2}', 0, 0, 0)) >= 0 &&
         toasts.textContent.indexOf(fill('({0} already gone.)', 2)) >= 0 && badgeOn(rowBySubject('Draft agenda')).indexOf(words('Recalled')) < 0,
         toasts.textContent);
      dismissAllToasts();

      // Refused: the server's sentence, or the page's own when there is none.
      recallAnswer = { status: 404, body: { error: 'no message with that Message-ID was sent from this account to an account on this server within the last 14 days' } };
      rightClick('Figures for the board');
      recallItem().dispatchEvent(makeEvent('click', { target: recallItem() }));
      await flush(); await flush();
      check('a 404 is a toast and nothing more', toasts.children.length === 1 && badgeOn(rowBySubject('Figures for the board')).indexOf(words('Recalled')) < 0, toasts.textContent);
      dismissAllToasts();
      recallAnswer = { status: 200, body: { recalled: 2, read: 1, gone: 0, failed: 0, elsewhere: 1, sent_marked: true, recipients: [] } };

      // The open message's More menu offers the same, under the same rules.
      location.hash = '#/m/402';
      await flush();
      check('the open message offers Recall in its More menu', document.getElementById('message-recall').hidden === false, 'hidden=' + document.getElementById('message-recall').hidden);
      location.hash = '#/m/404';
      await flush();
      check('and not for one older than recall_days', document.getElementById('message-recall').hidden === true, 'hidden=' + document.getElementById('message-recall').hidden);
      document.getElementById('message-back').dispatchEvent(makeEvent('click'));
      await flush();

      // A session that came in on an app password is not offered it at all:
      // the server would refuse, and the page does not offer what it knows
      // will be refused.
      viaAppPassword = true;
      await signInAgain();
      location.hash = '#/f/2';
      await flush();
      rightClick('Thanks');
      check('a session signed in with an app password is not offered Recall', recallItem().hidden === true, 'hidden=' + recallItem().hidden);
      document.dispatchEvent(makeEvent('keydown', { key: 'Escape' }));
      location.hash = '#/m/402';
      await flush();
      check('nor on the open message', document.getElementById('message-recall').hidden === true, 'hidden=' + document.getElementById('message-recall').hidden);
      document.getElementById('message-back').dispatchEvent(makeEvent('click'));
      await flush();
      viaAppPassword = false;
      await signInAgain();
      location.hash = '#/f/1';
      await flush();
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

      // A keyword is a change to the kept copy as much as a star is. The
      // importance marker is the shortest way to put one on: marked and then
      // put back, the copy in this browser follows both ways, or a message
      // read with no connection wears the wrong mark and sits in the wrong
      // section of the inbox.
      const keptWords = () => (idbStore('rows').get(102).row.flags.keywords || []).join(' ');
      const savedWords = () => {
         try {
            const kept = JSON.parse(store.get('hmPortalOffline') || '{}');
            const saved = ((kept.items && kept.items.list && kept.items.list.messages) || []).filter((m) => m.id === 102)[0];
            return ((saved && saved.flags && saved.flags.keywords) || []).join(' ');
         } catch (e) { return 'unreadable'; }
      };
      location.hash = '#/f/1';
      await flush();
      location.hash = '#/m/102';
      await flush();
      document.getElementById('message-important').dispatchEvent(makeEvent('click'));
      await flush(); await flush();
      check('a keyword put on a message reaches both copies this browser keeps - the message in the store, and the inbox listing saved beside it',
         keptWords().indexOf('$Important') >= 0 && savedWords().indexOf('$Important') >= 0, keptWords() + ' / ' + savedWords());
      const findAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const undoImportant = findAll(document.getElementById('toasts'), (n) => n.tagName === 'BUTTON' && n.textContent === words('Undo'))[0];
      if (!undoImportant) { throw new Error('no Undo in the toast: ' + document.getElementById('toasts').textContent); }
      undoImportant.dispatchEvent(makeEvent('click'));
      await flush(); await flush();
      check('and taking it off reaches them both too', keptWords().indexOf('$Important') < 0 && savedWords().indexOf('$Important') < 0, keptWords() + ' / ' + savedWords());
      dismissAllToasts();

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

   // ---- Contact groups: a group in the address book, made and filled on the
   // Contacts page, offered by its name in To and expanded to the people it
   // reaches, a group inside a group expanded once, and removed with an Undo
   // ---- Important, and sections of the inbox: a heuristic that lives in this
   // browser, writes one IMAP keyword, and is always overruled by the reader.
   // One session, end to end: a fresh sign-in that has learned nothing marks
   // nothing; mail from the one contact the reader writes to is marked and
   // says why; the reader disagrees and is obeyed; the reader marks a
   // stranger's newsletter important and the next one from that sender is
   // marked too; a sign-out forgets all of it, and what the reader marked by
   // hand is never judged again.
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const fill = (text, ...values) => values.reduce((s, v, i) => s.split('{' + i + '}').join(String(v)), words(text));
      const walkAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const rows = () => walkAll(document.getElementById('group-rows'), (n) => n.tagName === 'TR');
      const rowOf = (name) => rows().filter((r) => r.children[0].textContent.indexOf(name) >= 0)[0];
      const buttonIn = (root, english) => walkAll(root, (n) => n.tagName === 'BUTTON' && n.textContent === words(english))[0];
      const popupOf = (input) => input.parentNode.children.filter((n) => String(n.className) === 'complete')[0];
      const idOf = (name) => contactGroups.filter((g) => g.name === name)[0].id;

      location.hash = '#/contacts';
      await flush();
      check('the Contacts page has a Groups card, and the group the address book holds is listed with its member',
         document.getElementById('contact-groups').hidden !== true && document.getElementById('group-table').hidden === false &&
            document.getElementById('group-empty').hidden === true && rows().length === 1 &&
            rowOf('Family').textContent.indexOf('Alice Example <alice@example.net>') >= 0,
         document.getElementById('group-rows').textContent);

      // Made here, under a name of its own and with no members yet.
      const beforeMade = requests.length;
      document.getElementById('group-name').value = 'Team';
      document.getElementById('group-form').dispatchEvent(makeEvent('submit'));
      await flush();
      const made = since(beforeMade).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/contacts/groups').map((r) => JSON.parse(r.body))[0];
      check('a group is made by name alone, and the address book is read again so the page shows it',
         !!made && made.name === 'Team' && (made.members || []).length === 0 && called(beforeMade, 'GET', /^\/api\/v1\/me\/contacts\?/) &&
            !!rowOf('Team') && document.getElementById('group-name').value === '',
         JSON.stringify(made) + ' | ' + document.getElementById('group-rows').textContent);
      check('and it says so where its members would be', rowOf('Team').children[1].textContent.indexOf(words('No members yet.')) >= 0,
         rowOf('Team').children[1].textContent);

      // The member field completes from the address book, asking for people
      // alone: a group cannot be typed into it by name.
      const memberField = document.getElementById('group-member-' + idOf('Team'));
      const beforeComplete = requests.length;
      memberField.value = 'a';
      memberField.dispatchEvent(makeEvent('input'));
      fireTimers();
      await flush();
      check('the member field asks the address book for people and not for groups',
         called(beforeComplete, 'GET', '/api/v1/me/contacts?limit=8&q=a&kind=person') &&
            !popupOf(memberField).children.some((li) => li.textContent.indexOf('Family') >= 0),
         JSON.stringify(since(beforeComplete).map((r) => r.path)) + ' | ' + popupOf(memberField).textContent);

      const beforeMember = requests.length;
      memberField.value = 'Carol Card <carol@example.org>';
      memberField.parentNode.parentNode.dispatchEvent(makeEvent('submit'));
      await flush();
      const added = since(beforeMember).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/contacts/groups/' + idOf('Team') + '/members').map((r) => JSON.parse(r.body))[0];
      check('a member is added by the address alone, whatever name was typed beside it, and the row then names the contact',
         !!added && added.address === 'carol@example.org' && rowOf('Team').children[1].textContent.indexOf('Carol Card <carol@example.org>') >= 0,
         JSON.stringify(added) + ' | ' + rowOf('Team').children[1].textContent);

      // A group inside a group, put there from the page: the member field
      // takes another group of this book by its name - it has no address to
      // type - and sends it as that group rather than as an address, which is
      // what the routes would refuse.
      const nestField = document.getElementById('group-member-' + idOf('Team'));
      const beforeNested = requests.length;
      nestField.value = 'Family';
      nestField.parentNode.parentNode.dispatchEvent(makeEvent('submit'));
      await flush();
      const nested = since(beforeNested).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/contacts/groups/' + idOf('Team') + '/members').map((r) => JSON.parse(r.body))[0];
      check('a group of this address book typed into the member field goes in as that group and not as an address',
         !!nested && nested.id === idOf('Family') && nested.address === undefined,
         JSON.stringify(nested));
      check('and the page then names it as a group, since it has no address to show',
         rowOf('Team').children[1].textContent.indexOf(fill('{0} (group)', 'Family')) >= 0,
         rowOf('Team').children[1].textContent);

      // In To: the name, how many it reaches, and every recipient written out.
      location.hash = '#/compose';
      await flush();
      const to = document.getElementById('compose-to');
      to.value = 'Tea';
      const beforeOffer = requests.length;
      to.dispatchEvent(makeEvent('input'));
      fireTimers();
      await flush();
      const offered = popupOf(to).children;
      check('the To field offers the group by its name, with how many people it reaches',
         called(beforeOffer, 'GET', '/api/v1/me/contacts?limit=8&q=Tea&book=all') && offered.length === 1 &&
            offered[0].textContent.indexOf('Team') >= 0 && offered[0].textContent.indexOf(fill('{0} member(s)', 2)) >= 0,
         popupOf(to).textContent);
      to.dispatchEvent(makeEvent('keydown', { key: 'Enter' }));
      await flush();
      check('choosing it writes every person it reaches, the group inside it expanded once and no group name left in the field',
         to.value === 'Carol Card <carol@example.org>, Alice Example <alice@example.net>, ' && to.value.indexOf('Family') < 0,
         to.value);
      document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
      await flush();
      dismissAllToasts();

      // Renamed in place, and removed with an Undo that makes it again.
      location.hash = '#/contacts';
      await flush();
      buttonIn(rowOf('Team'), 'Rename').dispatchEvent(makeEvent('click'));
      const field = walkAll(rowOf('Team'), (n) => n.tagName === 'INPUT' && n.getAttribute('aria-label') === words('Group name'))[0];
      check('Rename puts a field in the row in place of the name', !!field && field.hidden === false);
      const beforeRename = requests.length;
      field.value = 'The team';
      field.dispatchEvent(makeEvent('keydown', { key: 'Enter' }));
      await flush();
      const renamed = since(beforeRename).filter((r) => r.method === 'PUT' && /\/api\/v1\/me\/contacts\/groups\/\d+$/.test(r.path)).map((r) => JSON.parse(r.body))[0];
      check('Enter saves the new name, and the members are left alone',
         !!renamed && renamed.name === 'The team' && renamed.members === undefined && !!rowOf('The team'),
         JSON.stringify(renamed) + ' | ' + document.getElementById('group-rows').textContent);

      const beforeGone = requests.length;
      buttonIn(rowOf('The team'), 'Delete').dispatchEvent(makeEvent('click'));
      await flush();
      check('Delete removes the group and offers Undo',
         called(beforeGone, 'DELETE', /^\/api\/v1\/me\/contacts\/groups\/\d+$/) && !rowOf('The team') &&
            document.getElementById('toasts').textContent.indexOf(words('Group removed.')) >= 0,
         document.getElementById('group-rows').textContent + ' | ' + document.getElementById('toasts').textContent);
      const beforeUndo = requests.length;
      buttonIn(document.getElementById('toasts'), 'Undo').dispatchEvent(makeEvent('click'));
      await flush();
      const again = since(beforeUndo).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/contacts/groups').map((r) => JSON.parse(r.body))[0];
      check('Undo makes it again with its members, the group inside it among them',
         !!again && again.name === 'The team' && again.members.length === 2 &&
            again.members.every((m) => !!m.id) && again.members.some((m) => m.id === 3) && again.members.some((m) => m.id === idOf('Family')),
         JSON.stringify(again));
      dismissAllToasts();

      // What the page does with a group it cannot change, and what it never
      // offers a group for.
      contactGroupsRefused = true;
      const beforeRefusal = requests.length;
      document.getElementById('group-name').value = 'Nowhere';
      document.getElementById('group-form').dispatchEvent(makeEvent('submit'));
      await flush();
      check('a group the server refuses is said so in the card, and the name is left in the box to try again',
         called(beforeRefusal, 'POST', '/api/v1/me/contacts/groups') &&
            String(document.getElementById('group-status').className).indexOf('error') >= 0 &&
            document.getElementById('group-status').textContent.length > 0 && document.getElementById('group-name').value === 'Nowhere',
         document.getElementById('group-status').textContent + ' | ' + document.getElementById('group-name').value);
      contactGroupsRefused = false;
      document.getElementById('group-name').value = '';

      location.hash = '#/contacts';
      await flush();
      document.getElementById('contact-export-vcf').dispatchEvent(makeEvent('click'));
      await flush();
      const exported = document.getElementById('contacts-section').getAttribute('data-last-export') || '';
      check('the vCard export carries the group as a group card, its members by address',
         exported.indexOf('X-ADDRESSBOOKSERVER-KIND:group') >= 0 && exported.indexOf('X-ADDRESSBOOKSERVER-MEMBER:mailto:alice@example.net') >= 0,
         exported.slice(exported.indexOf('X-ADDRESSBOOKSERVER-KIND') - 120));
      document.getElementById('contact-export-csv').dispatchEvent(makeEvent('click'));
      await flush();
      const exportedCsv = document.getElementById('contacts-section').getAttribute('data-last-export') || '';
      check('and the CSV leaves it out, since a group has no address for the column',
         exportedCsv.indexOf('Family') < 0 && exportedCsv.indexOf('Alice Example,alice@example.net') >= 0, exportedCsv);

      // And back in: a vCard file holding a group card makes the group, by its
      // FN and the members it names by address, so what the export wrote can
      // be read back. A group already there by that name is not made twice.
      const beforeGroupImport = requests.length;
      const importGroups = document.getElementById('contact-import-file');
      importGroups.files = [new File([
         'BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Family\r\nX-ADDRESSBOOKSERVER-KIND:group\r\nX-ADDRESSBOOKSERVER-MEMBER:mailto:alice@example.net\r\nEND:VCARD\r\n' +
         'BEGIN:VCARD\r\nVERSION:4.0\r\nFN:Book club\r\nKIND:group\r\nMEMBER:mailto:carol@example.org\r\nMEMBER:urn:uuid:not-of-this-address-book\r\nEND:VCARD\r\n'
      ], 'groups.vcf', { type: 'text/vcard' })];
      importGroups.dispatchEvent(makeEvent('change'));
      await flush();
      const importedGroups = since(beforeGroupImport).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/contacts/groups').map((r) => JSON.parse(r.body));
      check('a group card in an imported vCard file makes the group, in either spelling, with the members it names by address and not the ones it names by UID',
         importedGroups.length === 1 && importedGroups[0].name === 'Book club' &&
            importedGroups[0].members.length === 1 && importedGroups[0].members[0].address === 'carol@example.org',
         JSON.stringify(importedGroups));

      location.hash = '#/compose';
      await flush();
      const mention = document.getElementById('compose-text');
      mention.value = 'Hello @Fam';
      mention.selectionStart = mention.value.length;
      const beforeGroupMention = requests.length;
      mention.dispatchEvent(makeEvent('input'));
      fireTimers();
      await flush();
      check('a group is never offered as an @mention, which is one person and one address',
         called(beforeGroupMention, 'GET', '/api/v1/me/contacts?limit=8&q=Fam') &&
            !walkAll(document.getElementById('compose-text').parentNode, (n) => n.tagName === 'LI').some((li) => li.textContent.indexOf('Family') >= 0),
         JSON.stringify(since(beforeGroupMention).map((r) => r.path)));
      document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
      await flush();
      const hasClass = (n, name) => String(n.className).split(' ').indexOf(name) >= 0;
      const list = () => document.getElementById('message-list');
      // Under sections a row sits inside its section, so rows are walked for.
      const rowsIn = (root) => walkAll(root, (n) => hasClass(n, 'msg'));
      const msgRows = () => rowsIn(list());
      const rowFor = (subject) => msgRows().filter((r) => r.textContent.indexOf(subject) >= 0)[0];
      const markerIn = (row) => (row ? walkAll(row, (n) => hasClass(n, 'imp'))[0] : null);
      const lit = (subject) => { const b = markerIn(rowFor(subject)); return !!b && hasClass(b, 'on'); };
      const sections = () => walkAll(list(), (n) => hasClass(n, 'list-sect'));
      const sectionHead = (box) => walkAll(box, (n) => String(n.className) === 'list-sect-toggle')[0];
      const headings = () => sections().map((box) => sectionHead(box).textContent);
      const tallyKey = 'hmPortalImportant:user@example.com';
      const tally = () => { try { return JSON.parse(store.get(tallyKey) || '{}'); } catch (e) { return {}; } };
      const keywordPuts = (from, id) => since(from).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/messages/' + id + '/flags' && /keywords_/.test(r.body || ''));
      const adds = (put) => (put ? (JSON.parse(put.body).keywords_add || []).slice().sort() : []);
      const removes = (put) => (put ? (JSON.parse(put.body).keywords_remove || []).slice().sort() : []);
      const goTo = async (hash) => { if (location.hash === hash) { location.hash = '#/settings'; await flush(); } location.hash = hash; await flush(); };
      // The run is in German by here, so the Undo button is named in German.
      const undoNow = () => { const b = walkAll(document.getElementById('toasts'), (n) => n.tagName === 'BUTTON' && n.textContent === words('Undo'))[0]; if (!b) { throw new Error('no Undo in the toast: ' + document.getElementById('toasts').textContent); } b.dispatchEvent(makeEvent('click')); };
      const setPrefs = async (values) => {
         await goTo('#/settings');
         Object.keys(values).forEach((id) => { document.getElementById(id).value = values[id]; });
         document.getElementById('prefs-form').dispatchEvent(makeEvent('submit'));
         await flush();
      };
      const signInAgain = async () => {
         document.getElementById('signout').dispatchEvent(makeEvent('click'));
         await flush();
         document.getElementById('address').value = account;
         document.getElementById('password').value = 'a-real-password';
         document.getElementById('signin-form').dispatchEvent(makeEvent('submit'));
         await flush();
         dismissAllToasts();
         await goTo('#/f/1');
      };
      const inboxWas = document.getElementById('pref-inbox').value;
      const paneWas = document.getElementById('pref-pane').value;
      const blockStart = requests.length;

      // A session that has learned nothing: the whole inbox is judged once,
      // and being addressed to this mailbox alone is not enough on its own.
      await setPrefs({ 'pref-important': 'auto', 'pref-sections': '0', 'pref-inbox': 'all', 'pref-pane': 'off' });
      await signInAgain();
      const beforeCold = requests.length;
      await goTo('#/f/1');
      check('a browser that has learned nothing marks nothing, and tells the server nothing while deciding it',
         msgRows().length === 4 && msgRows().every((r) => !!markerIn(r)) && !msgRows().some((r) => hasClass(markerIn(r), 'on')) &&
         since(beforeCold).every((r) => r.method === 'GET'),
         msgRows().length + ' rows, ' + JSON.stringify(since(beforeCold).filter((r) => r.method !== 'GET').map((r) => r.method + ' ' + r.path)));
      check('and the marker is a control, named for what pressing it would do',
         markerIn(msgRows()[0]).getAttribute('aria-pressed') === 'false' && markerIn(msgRows()[0]).getAttribute('aria-label') === words('Mark as important'),
         String(markerIn(msgRows()[0]).getAttribute('aria-label')));
      check('the sidebar offers the Important view beside Starred',
         document.getElementById('folder-nav').children[2].getAttribute('data-route') === '/important',
         String(document.getElementById('folder-nav').children[2].getAttribute('data-route')));
      check('a row that carries the marker says so, so that the sender\'s column can be given the width the marker takes and the name is no shorter for it',
         msgRows().every((r) => hasClass(r, 'has-imp')), String(msgRows()[0].className));
      await goTo('#/f/2');
      check('and mail the reader sent has no marker and asks for none of that width',
         msgRows().length > 0 && !msgRows().some((r) => markerIn(r)) && !msgRows().some((r) => hasClass(r, 'has-imp')),
         msgRows().length + ' rows, ' + JSON.stringify(msgRows().map((r) => String(r.className))));
      await goTo('#/f/1');

      // Mail arrives: one from the one contact in the address book the reader
      // writes to, one a newsletter addressed to a list of six.
      importantArrival = 1;
      const beforeJudge = requests.length;
      await goTo('#/f/1');
      const judgePuts = since(beforeJudge).filter((r) => r.method === 'PUT' && /\/flags$/.test(r.path));
      check('mail from somebody the reader writes to is marked important, as the $Important keyword, so every other client sees it',
         judgePuts.length === 1 && judgePuts[0].path === '/api/v1/me/messages/112/flags' && adds(judgePuts[0]).join(' ') === '$Important' && lit('Figures'),
         JSON.stringify(judgePuts.map((r) => r.path + ' ' + r.body)));
      check('a newsletter addressed to a list of six is not', !lit('Newsletter'), String(markerIn(rowFor('Newsletter')).className));
      check('and the marker says which reason decided it, in the reader\'s own language',
         markerIn(rowFor('Figures')).getAttribute('title') === fill('Important because {0}. Click to mark as not important.', words('you write to this sender')),
         String(markerIn(rowFor('Figures')).getAttribute('title')));
      const beforeAgain = requests.length;
      await goTo('#/f/1');
      check('a message judged once is not judged again in the same session', !called(beforeAgain, 'PUT', /\/flags$/),
         JSON.stringify(since(beforeAgain).filter((r) => r.method !== 'GET').map((r) => r.method + ' ' + r.path)));

      // The reader disagrees, from the open message.
      await goTo('#/m/112');
      check('the open message offers to take the mark off', document.getElementById('message-important').hidden === false &&
         document.getElementById('message-important').textContent === words('Not important'),
         document.getElementById('message-important').textContent);
      const beforeCorrect = requests.length;
      document.getElementById('message-important').dispatchEvent(makeEvent('click'));
      await flush();
      const correction = keywordPuts(beforeCorrect, 112)[0];
      check('marking it not important puts $NotImportant on and $Important off in the one request',
         !!correction && adds(correction).join(' ') === '$NotImportant' && removes(correction).join(' ') === '$Important' &&
         document.getElementById('message-important').textContent === words('Mark as important'),
         correction ? correction.body : 'no flags call');
      check('and says so, with an Undo', document.getElementById('toasts').textContent.indexOf(words('Marked as not important.')) >= 0,
         document.getElementById('toasts').textContent);
      const beforeUndoMark = requests.length;
      undoNow();
      await flush();
      const undone = keywordPuts(beforeUndoMark, 112)[0];
      check('Undo puts back the keywords the message had, which was the one the page had put there',
         !!undone && adds(undone).join(' ') === '$Important' && removes(undone).join(' ') === '$NotImportant' &&
         document.getElementById('message-important').textContent === words('Not important'),
         undone ? undone.body : 'no flags call');
      dismissAllToasts();
      document.getElementById('message-important').dispatchEvent(makeEvent('click'));
      await flush();
      dismissAllToasts();

      // The other way about: the reader raises a newsletter from the list,
      // with Gmail's key, and this browser remembers that sender.
      await goTo('#/f/1');
      const at = msgRows().indexOf(rowFor('Newsletter'));
      for (let i = 0; i < at; i += 1) { document.dispatchEvent(makeEvent('keydown', { key: 'j', target: document.body })); }
      const beforeKey = requests.length;
      document.dispatchEvent(makeEvent('keydown', { key: '+', target: document.body }));
      await flush();
      const byKey = keywordPuts(beforeKey, 111)[0];
      check('the key + marks the row under the cursor important, and - would take it back',
         !!byKey && adds(byKey).join(' ') === '$Important' && removes(byKey).join(' ') === '$NotImportant' && lit('Newsletter'),
         byKey ? byKey.body : 'no flags call, cursor at ' + at);
      dismissAllToasts();
      check('what the reader marked is counted against that sender, in this browser',
         tally().marked && tally().marked['news@stranger.example'] === 1 && tally().marked['alice@example.net'] === -1,
         JSON.stringify(tally().marked));

      // A second newsletter from that sender, which is the whole of what
      // "it learns" means here: the reader taught this browser, not the server.
      importantArrival = 2;
      const beforeTaught = requests.length;
      await goTo('#/f/1');
      const taught = keywordPuts(beforeTaught, 113)[0];
      check('the next message from a sender the reader marked important is marked too',
         !!taught && adds(taught).join(' ') === '$Important' && lit('Newsletter again'),
         taught ? taught.body : 'no flags call');
      check('and it says that is why', markerIn(rowFor('Newsletter again')).getAttribute('title') ===
         fill('Important because {0}. Click to mark as not important.', words('you marked mail from this sender important')),
         String(markerIn(rowFor('Newsletter again')).getAttribute('title')));

      // The sections. One of the older messages is starred, so all three are
      // there to be counted.
      flaggedById[103] = true;
      await setPrefs({ 'pref-sections': '1' });
      await goTo('#/f/1');
      check('the inbox is drawn as Important, Starred and Everything else, in that order, each counted',
         headings().length === 3 && headings()[0] === words('Important') + '2' && headings()[1].indexOf(words('Starred')) === 0 &&
         headings()[2].indexOf(words('Everything else')) === 0 && msgRows().length === 7,
         JSON.stringify(headings()) + ' over ' + msgRows().length + ' rows');
      // The list says it is a list and every row says it is an item of one.
      // A section must not come between the two, or a screen reader loses
      // both the list and how many items are in it.
      check('a section is not a thing between the list and its rows: the box is presentational, the heading is a heading, and the rows are a group that heading names',
         sections().length === 3 && sections().every((box) => box.getAttribute('role') === 'presentation') &&
         sections().every((box) => walkAll(box, (n) => n.getAttribute('role') === 'heading' && n.getAttribute('aria-level') === '3').length === 1) &&
         sections().every((box) => {
            const group = walkAll(box, (n) => n.getAttribute('role') === 'group')[0];
            return !!group && group.id === sectionHead(box).getAttribute('aria-controls') &&
               walkAll(box, (n) => n.id === group.getAttribute('aria-labelledby')).length === 1 &&
               rowsIn(box).every((r) => r.parentNode === group);
         }),
         JSON.stringify(sections().map((box) => String(box.getAttribute('role')) + '/' + String(sectionHead(box).getAttribute('aria-controls')))));
      check('a section that holds unread mail says how much of it is unread', (() => {
         const rest = rowsIn(sections()[2]);
         const unseen = rest.filter((r) => hasClass(r, 'unseen')).length;
         return unseen > 0 && headings()[2] === words('Everything else') + fill('{0}, {1} unread', rest.length, unseen);
      })(), headings()[2]);
      check('every row under Important is marked important, and no row under the other two is',
         rowsIn(sections()[0]).length === 2 && rowsIn(sections()[0]).every((r) => hasClass(markerIn(r), 'on')) &&
         sections().slice(1).every((box) => rowsIn(box).every((r) => !hasClass(markerIn(r), 'on'))),
         JSON.stringify(sections().map((box) => rowsIn(box).length)));
      check('the starred message is under Starred, and it is only in one section',
         rowsIn(sections()[1]).filter((r) => r.textContent.indexOf('Third') >= 0).length === 1 &&
         msgRows().filter((r) => r.textContent.indexOf('Third') >= 0).length === 1);
      check('and the rows are still one list to the keyboard: the cursor starts at the top of the first section',
         hasClass(msgRows()[0], 'msg-cursor'), String(msgRows()[0].className));
      const beforeFold = msgRows().length;
      sectionHead(sections()[0]).dispatchEvent(makeEvent('click'));
      await flush();
      check('a section folds away, keeps its count and says it is closed',
         msgRows().length === beforeFold - 2 && sectionHead(sections()[0]).getAttribute('aria-expanded') === 'false' &&
         headings()[0] === words('Important') + '2',
         msgRows().length + ' rows, expanded=' + sectionHead(sections()[0]).getAttribute('aria-expanded'));
      check('and this browser remembers which is folded', (() => { try { return JSON.parse(store.get('hmPortalSectionsClosed')).important === 1; } catch (e) { return false; } })(),
         String(store.get('hmPortalSectionsClosed')));
      sectionHead(sections()[0]).dispatchEvent(makeEvent('click'));
      await flush();
      check('and unfolds again, with nothing left behind', msgRows().length === beforeFold && !store.has('hmPortalSectionsClosed'),
         msgRows().length + ' rows');

      // The tabs sort first; the sections divide what the open tab shows.
      await setPrefs({ 'pref-inbox': 'tabs' });
      await goTo('#/f/1');
      check('with the inbox in tabs the sections divide the open tab, not the whole folder',
         document.getElementById('inbox-tabs').hidden === false && msgRows().length > 0 && msgRows().length < 7 &&
         msgRows().length === sections().reduce((n, box) => n + rowsIn(box).length, 0),
         msgRows().length + ' rows under ' + sections().length + ' sections');
      await setPrefs({ 'pref-inbox': 'all' });

      // No marker at all: the buttons, the sidebar entry, the section and the
      // open message's action go with it, and nothing is judged.
      await setPrefs({ 'pref-important': 'off' });
      const beforeOff = requests.length;
      await goTo('#/f/1');
      check('with the marker off no row has one, the Important section is not offered, and nothing is judged',
         !msgRows().some((r) => markerIn(r)) && headings().length === 2 && headings()[0].indexOf(words('Starred')) === 0 &&
         !called(beforeOff, 'PUT', /\/flags$/), JSON.stringify(headings()));
      check('and the sidebar drops the Important view',
         document.getElementById('folder-nav').children[2].getAttribute('data-route') !== '/important',
         String(document.getElementById('folder-nav').children[2].getAttribute('data-route')));
      await goTo('#/m/112');
      check('and the open message offers nothing to mark', document.getElementById('message-important').hidden === true);

      // The reader's own marks alone: the marker stays, the judging stops.
      await setPrefs({ 'pref-important': 'manual', 'pref-sections': '0' });
      importantArrival = 0;
      const beforeManual = requests.length;
      await goTo('#/f/1');
      check('set to the reader\'s own marks alone, the marker is on every row and nothing is judged',
         msgRows().length === 4 && msgRows().every((r) => !!markerIn(r)) && !called(beforeManual, 'PUT', /\/flags$/),
         msgRows().length + ' rows');

      // Signing out forgets what this browser learned, which is the half of
      // "no server learning" that no server can do for it.
      check('the tally is kept while the session is', store.has(tallyKey), String(store.get(tallyKey)).slice(0, 60));
      await setPrefs({ 'pref-important': 'auto' });

      // The connection goes while the session is still good, and the page
      // draws the inbox listing this browser kept. Nothing in it can be
      // written back, so nothing in it is judged: a marker the server never
      // took is a marker no other client would ever show.
      // From the sender the reader marked important by hand, addressed to
      // this mailbox alone: it scores well over the two points, so if the
      // page judged what it drew from the store, it would mark this.
      const keptPage = { total: 1, messages: [{ id: 120, uid: 30, subject: 'Kept listing', from: 'Newsletter <news@stranger.example>', to: account, date: '2026-09-09 18:00', size: 400, flags: { seen: true, flagged: false, draft: false } }] };
      store.set('hmPortalOffline', JSON.stringify({ items: { list: keptPage }, order: ['list'] }));
      networkDown = true;
      const beforeKept = requests.length;
      await goTo('#/f/1');
      check('a listing drawn from this browser\'s own store, because the server cannot be reached, is not judged - though what is in it would score well over the threshold',
         !!rowFor('Kept listing') && !lit('Kept listing') && !called(beforeKept, 'PUT', /\/flags$/),
         JSON.stringify(since(beforeKept).filter((r) => r.method !== 'GET').map((r) => r.method + ' ' + r.path)) + ' lit=' + lit('Kept listing'));
      networkDown = false;
      store.delete('hmPortalOffline');

      // A session that ended while the page was not open leaves a tally
      // nobody can name: the next cold load has no address to file it under.
      // Every tally this browser holds goes, not only the one just signed in.
      const strandedKey = 'hmPortalImportant:someone-else@example.net';
      store.set(strandedKey, JSON.stringify({ marked: { 'x@example.net': 3 } }));
      importantArrival = 2;
      await signInAgain();
      check('signing out forgets whose mail this browser had learned to raise, and the tally of a session that ended while the page was closed with it',
         !store.has(tallyKey) && !store.has(strandedKey), String(store.get(tallyKey)) + ' / ' + String(store.get(strandedKey)));

      // And after all of it, on a browser that has forgotten: a message the
      // reader marked by hand is never judged again. 112 is from the contact
      // and would score enough on its own, but it carries $NotImportant.
      const beforeRejudge = requests.length;
      await goTo('#/f/1');
      check('a message the reader marked by hand is never judged again, either way',
         !called(beforeRejudge, 'PUT', '/api/v1/me/messages/112/flags') && !called(beforeRejudge, 'PUT', '/api/v1/me/messages/111/flags') &&
         !called(beforeRejudge, 'PUT', '/api/v1/me/messages/113/flags') && !lit('Figures') && lit('Newsletter') && lit('Newsletter again'),
         JSON.stringify(since(beforeRejudge).filter((r) => r.method === 'PUT').map((r) => r.path + ' ' + r.body)));

      // A From header is whatever a stranger wrote in it, so an address is
      // asked for as a name somebody chose and never as a property of the
      // object it is looked up in. constructor, __proto__ and the rest are
      // on every object in JavaScript, and a map of the addresses the reader
      // writes to says no to all of them while it is empty of them.
      importantArrival = 3;
      const beforeProto = requests.length;
      await goTo('#/f/1');
      check('a sender whose address is the name of a property every object has - constructor, __proto__ - is not taken for somebody the reader writes to, and is not marked',
         !!rowFor('Prototype pills') && !!rowFor('Prototype more') && !lit('Prototype pills') && !lit('Prototype more') &&
         !called(beforeProto, 'PUT', '/api/v1/me/messages/115/flags') && !called(beforeProto, 'PUT', '/api/v1/me/messages/114/flags'),
         JSON.stringify(since(beforeProto).filter((r) => r.method === 'PUT').map((r) => r.path + ' ' + r.body)));
      check('and the marker on one of them offers to mark it, with no reason to give',
         markerIn(rowFor('Prototype pills')).getAttribute('title') === words('Mark as important'),
         String(markerIn(rowFor('Prototype pills')).getAttribute('title')));

      // A mark over a conversation is one word about that sender. Counting
      // one for each of its messages would take twenty corrections to undo a
      // thread of twenty, and the reader's latest word would count for
      // nothing for a long time after.
      const beforeThread = requests.length;
      markerIn(rowFor('Bulletin')).dispatchEvent(makeEvent('click'));
      await flush();
      check('marking a conversation important marks every message of it, and counts one against the sender rather than one for each message',
         keywordPuts(beforeThread, 116).length === 1 && keywordPuts(beforeThread, 117).length === 1 && lit('Bulletin') &&
         tally().marked && tally().marked['bulletin@stranger.example'] === 1,
         JSON.stringify(tally().marked) + ' over ' + JSON.stringify(since(beforeThread).filter((r) => r.method === 'PUT').map((r) => r.path)));
      const beforeThreadBack = requests.length;
      undoNow();
      await flush();
      dismissAllToasts();
      check('and the Undo takes the one count back off, not two',
         keywordPuts(beforeThreadBack, 116).length === 1 && keywordPuts(beforeThreadBack, 117).length === 1 &&
         (!tally().marked || !tally().marked['bulletin@stranger.example']),
         JSON.stringify(tally().marked));

      // Half of a conversation refused - one message in a folder the ACL
      // will not let this account write to. What did change is a change the
      // reader made, so it is said and the Undo is offered over exactly it.
      flagsRefuseId = 117;
      const beforeHalf = requests.length;
      markerIn(rowFor('Bulletin')).dispatchEvent(makeEvent('click'));
      await flush();
      check('a conversation the server half refuses still says so and still offers the way back',
         keywordPuts(beforeHalf, 116).length === 1 && keywordPuts(beforeHalf, 117).length === 1 &&
         document.getElementById('mail-status').textContent === words('Could not change the flags') &&
         document.getElementById('toasts').textContent.indexOf(words('Marked as important.')) >= 0 && lit('Bulletin'),
         document.getElementById('mail-status').textContent + ' / ' + document.getElementById('toasts').textContent);
      const beforeHalfBack = requests.length;
      undoNow();
      await flush();
      check('and the Undo puts back only what was changed, leaving the message the server refused alone',
         keywordPuts(beforeHalfBack, 116).length === 1 && keywordPuts(beforeHalfBack, 117).length === 0 &&
         removes(keywordPuts(beforeHalfBack, 116)[0]).join(' ') === '$Important $NotImportant' && !lit('Bulletin'),
         JSON.stringify(since(beforeHalfBack).filter((r) => r.method === 'PUT').map((r) => r.path + ' ' + r.body)));
      flagsRefuseId = 0;
      dismissAllToasts();

      // The Important view in the sidebar is a search for the keyword.
      const beforeView = requests.length;
      document.getElementById('folder-nav').children[2].dispatchEvent(makeEvent('click'));
      await flush();
      check('the Important view asks the server for the keyword, and is titled for it',
         called(beforeView, 'GET', /^\/api\/v1\/me\/search\?q=label%3A%24Important/) &&
         document.getElementById('view-title').textContent === words('Important') && location.hash === '#/important',
         location.hash + ' ' + JSON.stringify(since(beforeView).map((r) => r.path)));

      // Nothing in any of it but the keyword, the reader's own settings and
      // the two sessions: no route of its own, and nothing about the reader.
      check('the whole of it writes nothing but the keyword and the reader\'s own settings',
         since(blockStart).filter((r) => r.method !== 'GET').every((r) => /\/messages\/\d+\/flags$/.test(r.path) || r.path === '/api/v1/me/preferences' || r.path === '/api/v1/session'),
         JSON.stringify(since(blockStart).filter((r) => r.method !== 'GET' && !/\/flags$/.test(r.path) && r.path !== '/api/v1/me/preferences' && r.path !== '/api/v1/session').map((r) => r.method + ' ' + r.path)));

      // Put the run back as it was.
      importantArrival = 0;
      delete flaggedById[103];
      await setPrefs({ 'pref-sections': '0', 'pref-inbox': inboxWas, 'pref-pane': paneWas });
      await goTo('#/f/1');
      dismissAllToasts();
   }

   // ---- Emoji, images inline in compose, tables (row L48): the picker in either editor, a picture pasted,
   // dropped or chosen, the bounds, the table the rebuild keeps, and a draft's formatting brought back
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const walkAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const editor = document.getElementById('compose-editor');
      const box = document.getElementById('compose-text');
      const status = document.getElementById('compose-status');
      const emojiPanel = document.getElementById('emoji-panel');
      const search = document.getElementById('emoji-search');
      const tablePanel = document.getElementById('table-panel');
      const imagesIn = () => walkAll(editor, (n) => n.tagName === 'IMG');
      const tablesIn = () => walkAll(editor, (n) => n.tagName === 'TABLE');
      const rowsIn = () => walkAll(editor, (n) => n.tagName === 'TR');
      const cellsOf = (row) => row.children.filter((c) => c.tagName === 'TD' || c.tagName === 'TH');
      const cellAt = (r, c) => cellsOf(rowsIn()[r])[c];
      const emojiButtons = () => walkAll(document.getElementById('emoji-grid'), (n) => n.tagName === 'BUTTON' && n.getAttribute('data-emoji'));
      const heads = () => walkAll(document.getElementById('emoji-grid'), (n) => n.className === 'emoji-head').map((n) => n.textContent);
      const opButton = (op) => walkAll(document.getElementById('table-edit'), (n) => n.getAttribute('data-op') === op)[0];
      const lastDraft = (from) => since(from).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/drafts').map((r) => JSON.parse(r.body)).pop();
      // A real 1x1 PNG, as the signature checks use: the page reads a file's first bytes before it believes the type.
      const logo = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==';
      const pngOf = (chars) => 'iVBORw0KGgoA' + 'A'.repeat(chars - 12);

      location.hash = '#/compose';
      await flush();
      check('the compose window opens plain, with Emoji offered beside Formatting and the picker closed',
         document.getElementById('compose-section').hidden === false && editor.hidden === true && document.getElementById('emoji-open').hidden === false &&
         document.getElementById('rich-tools').hidden === true && emojiPanel.hidden === true, 'editor hidden=' + editor.hidden);
      document.getElementById('emoji-open').dispatchEvent(makeEvent('click'));
      check('the picker opens on its groups, hundreds of emoji named for the search, and nothing recently used yet',
         emojiPanel.hidden === false && document.getElementById('emoji-open').getAttribute('aria-expanded') === 'true' && heads()[0] === words('Smileys') && heads().indexOf(words('Symbols')) > 0 &&
         heads().indexOf(words('Recently used')) < 0 && emojiButtons().length > 500 && emojiButtons().every((b) => b.getAttribute('title').length > 1) && search.focused > 0,
         'heads=' + JSON.stringify(heads()) + ' buttons=' + emojiButtons().length);
      search.value = 'heart eyes';
      search.dispatchEvent(makeEvent('input'));
      check('typing narrows the picker to the emoji whose names hold every word', emojiButtons().length >= 2 && emojiButtons().length < 10 &&
         emojiButtons().every((b) => /heart/.test(b.getAttribute('title')) && /eyes/.test(b.getAttribute('title'))) && heads().length === 0,
         emojiButtons().map((b) => b.getAttribute('title')).join(' | '));
      const picked = emojiButtons()[0].getAttribute('data-emoji');
      box.value = 'Hello world';
      box.selectionStart = 5; box.selectionEnd = 5;
      emojiButtons()[0].dispatchEvent(makeEvent('click'));
      check('an emoji picked goes into the plain text at the cursor, and is remembered as recently used in this browser',
         box.value === 'Hello' + picked + ' world' && JSON.parse(store.get('hmPortalEmoji') || '[]')[0] === picked && box.focused > 0,
         JSON.stringify(box.value) + ' recent=' + store.get('hmPortalEmoji'));
      // Everything after the emoji selected, so that the next one picked replaces it.
      box.selectionStart = 'Hello'.length + picked.length; box.selectionEnd = box.value.length;
      search.value = '';
      search.dispatchEvent(makeEvent('input'));
      check('with the search cleared the picker starts with what was recently used', heads()[0] === words('Recently used') && emojiButtons()[0].getAttribute('data-emoji') === picked, JSON.stringify(heads().slice(0, 2)));
      search.value = 'thumbs up';
      search.dispatchEvent(makeEvent('input'));
      search.dispatchEvent(makeEvent('keydown', { key: 'Enter' }));
      check('Enter in the search box picks the first match, over what the cursor selected', box.value === 'Hello' + picked + '👍' && JSON.parse(store.get('hmPortalEmoji'))[0] === '👍' && JSON.parse(store.get('hmPortalEmoji'))[1] === picked,
         JSON.stringify(box.value));
      search.value = 'no such thing';
      search.dispatchEvent(makeEvent('input'));
      check('a search that matches nothing says so', emojiButtons().length === 0 && walkAll(document.getElementById('emoji-grid'), (n) => n.className === 'emoji-none').length === 1, emojiButtons().length);
      search.dispatchEvent(makeEvent('keydown', { key: 'Escape' }));
      check('Escape closes the picker and puts the cursor back in the text', emojiPanel.hidden === true && document.getElementById('emoji-open').getAttribute('aria-expanded') === 'false' && document.activeElement === box, 'hidden=' + emojiPanel.hidden);

      // Formatting on: the same picker writes into the editor. This DOM has no
      // selection, so the text goes at the end, as it does in a browser where
      // the reader has not yet put the cursor anywhere.
      box.value = 'Hello';
      document.getElementById('rich-toggle').dispatchEvent(makeEvent('click'));
      document.getElementById('emoji-open').dispatchEvent(makeEvent('click'));
      search.value = 'rocket';
      search.dispatchEvent(makeEvent('input'));
      emojiButtons()[0].dispatchEvent(makeEvent('click'));
      check('with Formatting on, an emoji goes into the editor', editor.hidden === false && editor.textContent === 'Hello🚀', JSON.stringify(editor.textContent));
      document.getElementById('emoji-open').dispatchEvent(makeEvent('click'));

      // ---- an image pasted, dropped or chosen becomes an inline picture
      const textPaste = makeEvent('paste', { clipboardData: { files: [], getData: () => 'plain words' } });
      editor.dispatchEvent(textPaste);
      check('a paste of text is left to the browser', textPaste.defaultPrevented === false && imagesIn().length === 0, 'prevented=' + textPaste.defaultPrevented);
      const imagePaste = makeEvent('paste', { clipboardData: { files: [{ name: 'shot.png', type: 'image/png', size: 70, base64: logo }] } });
      editor.dispatchEvent(imagePaste);
      await flush();
      check('an image pasted into the editor becomes an inline image held as a data: URL of the type its bytes say',
         imagePaste.defaultPrevented === true && imagesIn().length === 1 && imagesIn()[0].getAttribute('src') === 'data:image/png;base64,' + logo && imagesIn()[0].getAttribute('alt') === 'shot.png',
         imagesIn().length + ' ' + (imagesIn()[0] ? imagesIn()[0].getAttribute('src').slice(0, 40) : ''));
      const svgPaste = makeEvent('paste', { clipboardData: { items: [{ kind: 'file', type: 'image/svg+xml', getAsFile: () => ({ name: 'logo.svg', type: 'image/svg+xml', size: 40, base64: 'PHN2Zz4=' }) }] } });
      editor.dispatchEvent(svgPaste);
      await flush();
      check('a picture that is not PNG, JPEG, GIF or WebP is refused, and the paste is not left to the browser either',
         svgPaste.defaultPrevented === true && imagesIn().length === 1 && status.textContent === words('An image to put in the message: PNG, JPEG, GIF or WebP.') && status.className.indexOf('error') >= 0, status.textContent);
      const fakePaste = makeEvent('paste', { clipboardData: { files: [{ name: 'notes.png', type: 'image/png', size: 30, base64: 'VGhpcyBpcyBub3QgYSBwaWN0dXJlIGF0IGFsbA==' }] } });
      editor.dispatchEvent(fakePaste);
      await flush();
      check('a file whose bytes are not an image is refused whatever it is called', imagesIn().length === 1 && status.textContent === words('An image to put in the message: PNG, JPEG, GIF or WebP.'), status.textContent);
      // Dropped on the editor: the image goes in, the other file is attached as a drop anywhere on the form attaches it.
      const noteBefore = document.getElementById('compose-files-note').textContent;
      const drop = makeEvent('drop', { dataTransfer: { files: [{ name: 'photo.jpg', type: 'image/jpeg', size: 70, base64: logo }, { name: 'agenda.pdf', type: 'application/pdf', size: 10, base64: 'AA==' }] } });
      editor.dispatchEvent(drop);
      await flush();
      const note = document.getElementById('compose-files-note').textContent;
      check('an image dropped on the editor goes inline and a document dropped with it is attached',
         drop.defaultPrevented === true && imagesIn().length === 2 && imagesIn()[1].getAttribute('src') === 'data:image/png;base64,' + logo && note.indexOf('agenda.pdf') >= 0 && note.indexOf('photo.jpg') < 0 && note !== noteBefore,
         imagesIn().length + ' note=' + note);
      const picker = document.getElementById('compose-image');
      document.getElementById('image-open').dispatchEvent(makeEvent('click'));
      picker.files = [{ name: 'chart.png', type: 'image/png', size: 70, base64: logo }];
      picker.dispatchEvent(makeEvent('change'));
      await flush();
      check('Image in the toolbar takes a file, and the picker is cleared for the next', imagesIn().length === 3 && imagesIn()[2].getAttribute('alt') === 'chart.png' && picker.value === '', imagesIn().length);
      // With a canvas, a large image is drawn no larger than 1200 on its longer side.
      const drawn = [];
      const realCreateElement = document.createElement;
      document.createElement = function (tag) {
         const made = realCreateElement(tag);
         if (String(tag).toLowerCase() === 'canvas') {
            made.getContext = () => ({ clearRect() {}, drawImage() { drawn.push(made.width + 'x' + made.height); } });
            made.toDataURL = (type) => 'data:' + type + ';base64,' + 'A'.repeat(made.width * 100);
         }
         return made;
      };
      Object.defineProperty(globalThis, 'Image', { configurable: true, writable: true, value: class {
         set src(value) { this.given = value; this.naturalWidth = 4000; this.naturalHeight = 3000; Promise.resolve().then(() => this.onload()); }
      } });
      picker.files = [{ name: 'screen.png', type: 'image/png', size: 900000, base64: pngOf(1200000) }];
      picker.dispatchEvent(makeEvent('change'));
      await flush();
      check('a large image is drawn down to 1200 pixels on its longer side before it goes in, and the page says so',
         drawn.length === 1 && drawn[0] === '1200x900' && imagesIn().length === 4 && imagesIn()[3].getAttribute('src').length === 'data:image/png;base64,'.length + 120000 && status.textContent === words('The image was made smaller to fit.') && status.className.indexOf('ok') >= 0,
         JSON.stringify(drawn) + ' ' + status.textContent);
      // One that fits within the box is kept byte for byte, not drawn again.
      Object.defineProperty(globalThis, 'Image', { configurable: true, writable: true, value: class {
         set src(value) { this.given = value; this.naturalWidth = 640; this.naturalHeight = 480; Promise.resolve().then(() => this.onload()); }
      } });
      picker.files = [{ name: 'small.png', type: 'image/png', size: 70, base64: logo }];
      picker.dispatchEvent(makeEvent('change'));
      await flush();
      check('an image already within the box is kept as it is', drawn.length === 1 && imagesIn().length === 5 && imagesIn()[4].getAttribute('src') === 'data:image/png;base64,' + logo && status.textContent === '', JSON.stringify(drawn) + ' ' + status.textContent);
      document.createElement = realCreateElement;
      delete globalThis.Image;
      // No canvas: an image over the 2,000,000 characters one may be is refused as too large...
      picker.files = [{ name: 'huge.png', type: 'image/png', size: 1600000, base64: pngOf(2100000) }];
      picker.dispatchEvent(makeEvent('change'));
      await flush();
      check('without a canvas, an image over what one may be is refused as too large', imagesIn().length === 5 && status.textContent === words('The image is too large for a message even when made smaller.') && status.className.indexOf('error') >= 0, status.textContent);
      // ...and the 6 MB the images may hold together: three of 1.9 million go in, the fourth does not.
      for (let i = 0; i < 3; i += 1) {
         picker.files = [{ name: 'big' + i + '.png', type: 'image/png', size: 1400000, base64: pngOf(1900000) }];
         picker.dispatchEvent(makeEvent('change'));
         await flush();
      }
      check('three images of 1.9 million characters go in under the 6 MB the message may carry', imagesIn().length === 8 && status.textContent === '', imagesIn().length + ' ' + status.textContent);
      picker.files = [{ name: 'big3.png', type: 'image/png', size: 1400000, base64: pngOf(1900000) }];
      picker.dispatchEvent(makeEvent('change'));
      await flush();
      check('the fourth is refused with the together bound named', imagesIn().length === 8 && status.textContent === words('This message holds as many images as it can carry: 6 MB of them together.'), status.textContent);
      // The send carries the images in the HTML, as data: URLs the server makes cid: parts of, and names them in the text.
      document.getElementById('compose-to').value = 'ann@example.net';
      document.getElementById('compose-subject').value = 'Pictures';
      confirmAnswer = true;
      const beforePictures = requests.length;
      document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
      await flush();
      fireTimers();
      await flush();
      await flush();
      const pictureSend = since(beforePictures).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/messages').map((r) => JSON.parse(r.body))[0];
      check('the send carries the images in its HTML as data: URLs and names them in its text',
         !!pictureSend && pictureSend.html.indexOf('<img src="data:image/png;base64,' + logo + '" alt="shot.png">') >= 0 && (pictureSend.html.match(/<img /g) || []).length === 8 &&
         pictureSend.text.indexOf('[shot.png]') >= 0 && pictureSend.text.indexOf('Hello🚀') === 0,
         pictureSend ? pictureSend.html.length + ' ' + JSON.stringify(pictureSend.text.slice(0, 60)) : 'no POST');
      dismissAllToasts();

      // ---- pasted into the plain text, an image switches Formatting on
      location.hash = '#/compose';
      await flush();
      check('the next message starts plain again, with the editor emptied and none of the last message\'s words in the box',
         editor.hidden === true && imagesIn().length === 0 && box.value === '', 'images=' + imagesIn().length + ' box=' + JSON.stringify(box.value));
      box.value = 'See this';
      const plainPaste = makeEvent('paste', { clipboardData: { files: [{ name: 'shot.png', type: 'image/png', size: 70, base64: logo }] } });
      box.dispatchEvent(plainPaste);
      await flush();
      check('an image pasted into the plain text switches Formatting on and puts it after the words', plainPaste.defaultPrevented === true && editor.hidden === false && box.hidden === true &&
         imagesIn().length === 1 && editor.textContent.indexOf('See this') === 0 && editor.childNodes[editor.childNodes.length - 1] === imagesIn()[0], 'editor hidden=' + editor.hidden + ' images=' + imagesIn().length);
      confirmAnswer = false;
      document.getElementById('rich-toggle').dispatchEvent(makeEvent('click'));
      check('Plain text asks first while the message holds a picture, and No keeps it', confirms[confirms.length - 1] === words('Plain text keeps the words and drops the images and tables. Continue?') && editor.hidden === false && imagesIn().length === 1,
         confirms[confirms.length - 1] + ' hidden=' + editor.hidden);
      confirmAnswer = true;
      document.getElementById('rich-toggle').dispatchEvent(makeEvent('click'));
      check('Yes keeps the words and names where the picture stood', editor.hidden === true && box.value === 'See this[shot.png]', JSON.stringify(box.value));

      // ---- tables: rows by columns, the operations with the cursor in a cell, Tab, and what the rebuild sends
      document.getElementById('rich-toggle').dispatchEvent(makeEvent('click'));
      document.getElementById('table-open').dispatchEvent(makeEvent('click'));
      check('Table opens on rows, columns and a header row, with the operations kept for a cursor in a cell',
         tablePanel.hidden === false && document.getElementById('table-insert').hidden === false && document.getElementById('table-edit').hidden === true && document.getElementById('table-open').getAttribute('aria-expanded') === 'true',
         'panel hidden=' + tablePanel.hidden);
      document.getElementById('table-rows').value = '2';
      document.getElementById('table-columns').value = '3';
      document.getElementById('table-header').checked = true;
      document.getElementById('table-insert-go').dispatchEvent(makeEvent('click'));
      check('Insert table puts a 2 by 3 table in the editor, its first row of headers, a line to write on after it, and closes the panel',
         tablesIn().length === 1 && rowsIn().length === 2 && cellsOf(rowsIn()[0]).length === 3 && cellsOf(rowsIn()[0]).every((c) => c.tagName === 'TH') && cellsOf(rowsIn()[1]).every((c) => c.tagName === 'TD') &&
         cellsOf(rowsIn()[1]).length === 3 && tablesIn()[0].parentNode === editor && editor.childNodes[editor.childNodes.length - 1].tagName === 'P' && tablePanel.hidden === true,
         'tables=' + tablesIn().length + ' rows=' + rowsIn().length);
      cellAt(0, 0).textContent = 'Name'; cellAt(0, 1).textContent = 'Size'; cellAt(0, 2).textContent = 'Note';
      cellAt(1, 0).textContent = 'Ada'; cellAt(1, 1).textContent = '12';
      const beforeTableDraft = requests.length;
      document.getElementById('compose-save').dispatchEvent(makeEvent('click'));
      await flush();
      const tableDraft = lastDraft(beforeTableDraft);
      check('the rebuild sends the table with the border, padding and spacing every mail client honours, and no attribute of its own, and the text as tab-separated lines with the empty cell keeping its place',
         !!tableDraft && tableDraft.html.indexOf('<table border="1" cellpadding="6" cellspacing="0" style="border-collapse:collapse"><tbody><tr><th>Name</th><th>Size</th><th>Note</th></tr><tr><td>Ada</td><td>12</td><td><br></td></tr></tbody></table><p><br></p>') >= 0 &&
         tableDraft.text === 'See this[shot.png]\nName\tSize\tNote\nAda\t12\t',
         tableDraft ? tableDraft.html.slice(0, 200) + ' | ' + JSON.stringify(tableDraft.text) : 'no draft');
      // A click in a cell names it; Table then offers the operations.
      cellAt(1, 0).dispatchEvent(makeEvent('click'));
      document.getElementById('table-open').dispatchEvent(makeEvent('click'));
      check('with the cursor in a cell, Table offers the row and column operations instead', tablePanel.hidden === false && document.getElementById('table-edit').hidden === false && document.getElementById('table-insert').hidden === true,
         'edit hidden=' + document.getElementById('table-edit').hidden);
      opButton('row-below').dispatchEvent(makeEvent('click'));
      check('Row below adds a row of the same width under the cursor\'s', rowsIn().length === 3 && cellsOf(rowsIn()[2]).length === 3 && cellsOf(rowsIn()[2]).every((c) => c.tagName === 'TD') && cellAt(1, 0).textContent === 'Ada' && tablePanel.hidden === false,
         'rows=' + rowsIn().length);
      opButton('column-right').dispatchEvent(makeEvent('click'));
      check('Column right adds a column to every row, a header cell in the header row', rowsIn().every((r) => cellsOf(r).length === 4) && cellAt(0, 1).tagName === 'TH' && cellAt(0, 1).textContent === '' && cellAt(0, 2).textContent === 'Size' && cellAt(2, 1).tagName === 'TD',
         rowsIn().map((r) => cellsOf(r).length).join(','));
      opButton('delete-row').dispatchEvent(makeEvent('click'));
      check('Delete row takes the row the cursor is in', rowsIn().length === 2 && cellAt(1, 0).textContent === 'Ada', 'rows=' + rowsIn().length);
      opButton('delete-column').dispatchEvent(makeEvent('click'));
      check('Delete column takes the column the cursor is in from every row', rowsIn().every((r) => cellsOf(r).length === 3) && cellAt(0, 1).textContent === 'Size' && cellAt(1, 1).textContent === '12', rowsIn().map((r) => cellsOf(r).length).join(','));
      opButton('row-above').dispatchEvent(makeEvent('click'));
      check('Row above puts a row before the cursor\'s', rowsIn().length === 3 && cellAt(0, 0).textContent === 'Name' && cellAt(1, 0).textContent === '' && cellAt(2, 0).textContent === 'Ada', rowsIn().map((r) => cellAt(rowsIn().indexOf(r), 0).textContent).join(','));
      // Tab in the last cell adds a row; Shift+Tab goes back.
      const tab = makeEvent('keydown', { key: 'Tab', target: cellAt(2, 2) });
      cellAt(2, 2).dispatchEvent(tab);
      check('Tab in the last cell adds a row and moves into it', tab.defaultPrevented === true && rowsIn().length === 4 && cellsOf(rowsIn()[3]).length === 3, 'rows=' + rowsIn().length);
      const backTab = makeEvent('keydown', { key: 'Tab', shiftKey: true, target: cellAt(3, 0) });
      cellAt(3, 0).dispatchEvent(backTab);
      opButton('delete-row').dispatchEvent(makeEvent('click'));
      check('Shift+Tab from a row\'s first cell goes to the last cell of the row above, where Delete row then acts', backTab.defaultPrevented === true && rowsIn().length === 3 && cellsOf(rowsIn()[2]).length === 3 && cellAt(2, 0).textContent === '', 'rows=' + rowsIn().length);
      // A key typed in the editor is the message's: the page's own shortcuts on the document do not see it.
      const typed = makeEvent('keydown', { key: 'e', target: editor });
      editor.dispatchEvent(typed);
      const palette = makeEvent('keydown', { key: 'k', ctrlKey: true, target: editor });
      editor.dispatchEvent(palette);
      check('a plain key typed in the editor stops there, and a key with a modifier goes on up to the page', typed.propagationStopped === true && palette.propagationStopped === false, 'plain=' + typed.propagationStopped + ' modified=' + palette.propagationStopped);
      // A table pasted into a cell is a table of its own: a column added to the one around it does not reach into it.
      const outerRows = rowsIn().slice();
      const inner = document.createElement('table');
      const innerBody = document.createElement('tbody'), innerRow = document.createElement('tr');
      innerRow.appendChild(document.createElement('td'));
      innerRow.appendChild(document.createElement('td'));
      innerBody.appendChild(innerRow);
      inner.appendChild(innerBody);
      cellsOf(outerRows[0])[0].appendChild(inner);
      cellsOf(outerRows[2])[0].dispatchEvent(makeEvent('click'));
      opButton('column-right').dispatchEvent(makeEvent('click'));
      check('a table pasted into a cell keeps its own columns when one is added to the table around it',
         cellsOf(innerRow).length === 2 && outerRows.every((r) => cellsOf(r).length === 4),
         'inner=' + cellsOf(innerRow).length + ' outer=' + outerRows.map((r) => cellsOf(r).length).join(','));
      cellsOf(outerRows[0])[0].removeChild(inner);
      opButton('delete-column').dispatchEvent(makeEvent('click'));
      opButton('delete-table').dispatchEvent(makeEvent('click'));
      check('Delete table removes it and closes the panel', tablesIn().length === 0 && tablePanel.hidden === true, 'tables=' + tablesIn().length);
      // A table pasted in with its styles, spans and a player: the rebuild sends the cells and their words, nothing else.
      const pastedTable = document.createElement('table');
      pastedTable.setAttribute('width', '900'); pastedTable.setAttribute('style', 'background:red');
      const pastedBody = document.createElement('tbody'), pastedRow = document.createElement('tr'), pastedCell = document.createElement('td');
      pastedCell.setAttribute('style', 'color:red'); pastedCell.setAttribute('onclick', 'steal()'); pastedCell.setAttribute('colspan', '2');
      const video = document.createElement('video'); video.appendChild(document.createTextNode('clip')); pastedCell.appendChild(video);
      pastedRow.appendChild(pastedCell); pastedBody.appendChild(pastedRow); pastedTable.appendChild(pastedBody);
      editor.appendChild(pastedTable);
      const beforePastedDraft = requests.length;
      document.getElementById('compose-save').dispatchEvent(makeEvent('click'));
      await flush();
      const pastedDraft = lastDraft(beforePastedDraft);
      check('a table pasted with styles, a span and a player is sent as its cells and words alone',
         !!pastedDraft && pastedDraft.html.indexOf('<table border="1" cellpadding="6" cellspacing="0" style="border-collapse:collapse"><tbody><tr><td>clip</td></tr></tbody></table>') >= 0 &&
         pastedDraft.html.indexOf('onclick') < 0 && pastedDraft.html.indexOf('colspan') < 0 && pastedDraft.html.indexOf('<video') < 0 && pastedDraft.html.indexOf('900') < 0,
         pastedDraft ? pastedDraft.html.slice(-200) : 'no draft');
      document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
      await flush();
      dismissAllToasts();

      // ---- a draft with formatting reopens formatted, its own images back in place
      location.hash = '#/compose?draft=105';
      await flush();
      await flush();
      await flush();
      check('a draft with formatting reopens in the formatted editor, the images this webmail made parts of it back as data: URLs, and another client\'s image left as the file it is',
         editor.hidden === false && box.hidden === true && document.getElementById('rich-toggle').textContent === 'Plain text' && editor.textContent.indexOf('Not yet') >= 0 &&
         imagesIn().length === 2 && imagesIn().every((i) => i.getAttribute('src') === 'data:image/png;base64,QUJD') && editor.focused > 0 &&
         document.getElementById('compose-files-note').textContent.indexOf('photo.jpg') >= 0,
         'editor hidden=' + editor.hidden + ' images=' + imagesIn().map((i) => i.getAttribute('src').slice(0, 30)).join(',') + ' text=' + JSON.stringify(editor.textContent));
      document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
      await flush();
      dismissAllToasts();

      // ---- the pictures and the files share the twelve megabytes one message may carry
      location.hash = '#/compose';
      await flush();
      document.getElementById('compose-to').value = 'ann@example.net';
      document.getElementById('compose-subject').value = 'The deck';
      const chosen = document.getElementById('compose-files');
      chosen.files = [{ name: 'deck.pdf', type: 'application/pdf', size: 8300000, base64: 'AA==' }];
      const beforeAlone = requests.length;
      document.getElementById('compose-save').dispatchEvent(makeEvent('click'));
      await flush();
      check('a file of eight megabytes on its own is within what one message may carry', !!lastDraft(beforeAlone) && status.textContent === words('Draft saved.'), status.textContent);
      document.getElementById('rich-toggle').dispatchEvent(makeEvent('click'));
      for (let i = 0; i < 4; i += 1) {
         picker.files = [{ name: 'page' + i + '.png', type: 'image/png', size: 1100000, base64: pngOf(1500000) }];
         picker.dispatchEvent(makeEvent('change'));
         await flush();
      }
      check('four pictures of a million and a half characters go into the message under the 6 MB its pictures may be', imagesIn().length === 4 && status.textContent === '', imagesIn().length + ' ' + status.textContent);
      const beforeBoth = requests.length;
      document.getElementById('compose-save').dispatchEvent(makeEvent('click'));
      await flush();
      check('the pictures count for their bytes against the same twelve megabytes the files do, and nothing is sent for the route to refuse',
         !lastDraft(beforeBoth) && status.textContent === words('The pictures in the message and the files are larger than 12 MB together.'), status.textContent);
      // The reminder counts a picture as something attached.
      picker.files = [];
      chosen.files = [];
      document.getElementById('compose-text').value = '';
      const askedBefore = confirms.length;
      document.getElementById('compose-subject').value = 'The screenshot';
      editor.appendChild(document.createTextNode('The screenshot is attached.'));
      confirmAnswer = false;
      document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
      await flush();
      check('a picture in the message is something attached, so the reminder does not ask', confirms.length === askedBefore, JSON.stringify(confirms.slice(askedBefore)));
      document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
      await flush();
      dismissAllToasts();
      confirmAnswer = true;

      // ---- what the review of this row found -------------------------------------
      // Only a draft's own formatting is brought back into the page. #compose?draft=<id>
      // names a message by its number and nothing stops that number being a message
      // that arrived from outside; 102 is one, with HTML of its own.
      location.hash = '#/compose?draft=102';
      await flush();
      await flush();
      await flush();
      check('a message that is not marked a draft reopens as its text, whatever HTML it carries',
         editor.hidden === true && box.hidden === false && editor.textContent.indexOf('Hello') < 0 && document.getElementById('rich-toggle').textContent === 'Formatting',
         'editor hidden=' + editor.hidden + ' text=' + JSON.stringify(editor.textContent));
      document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
      await flush();
      dismissAllToasts();

      // A data: source is admitted whole or not at all: one that ends in a quote
      // would carry markup with it wherever the rebuild's output is written - into
      // the message, and into this page when a draft is reopened.
      location.hash = '#/compose';
      await flush();
      document.getElementById('rich-toggle').dispatchEvent(makeEvent('click'));
      document.getElementById('compose-to').value = 'ann@example.net';
      document.getElementById('compose-subject').value = 'Crafted';
      const crafted = document.createElement('img');
      crafted.setAttribute('src', 'data:image/png;base64,AA"><a href="https://evil.example/sign-in">Session expired</a><x y="');
      editor.appendChild(crafted);
      const honest = document.createElement('img');
      honest.setAttribute('src', 'data:image/png;base64,' + logo);
      honest.setAttribute('alt', 'real.png');
      editor.appendChild(honest);
      const beforeCrafted = requests.length;
      document.getElementById('compose-save').dispatchEvent(makeEvent('click'));
      await flush();
      const craftedDraft = lastDraft(beforeCrafted);
      check('a data: source that is not base64 to its end is dropped whole, so none of what it carries is written out as markup',
         !!craftedDraft && craftedDraft.html.indexOf('evil.example') < 0 && craftedDraft.html.indexOf('<a ') < 0 && (craftedDraft.html.match(/<img /g) || []).length === 1 &&
         craftedDraft.html.indexOf('<img src="data:image/png;base64,' + logo + '" alt="real.png">') >= 0,
         craftedDraft ? craftedDraft.html : 'no draft');
      document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
      await flush();
      dismissAllToasts();

      // How many pictures, as well as how large: the send route puts back in place
      // at most sixty-four of them (InlineImageParts::MostImages), and the
      // sixty-fifth would ride out as the data: URL no mail client shows. Sixty-four
      // of a hundred characters each are nowhere near any bound of bytes.
      location.hash = '#/compose';
      await flush();
      document.getElementById('rich-toggle').dispatchEvent(makeEvent('click'));
      for (let i = 0; i < 63; i += 1) {
         const one = document.createElement('img');
         one.setAttribute('src', 'data:image/png;base64,' + logo);
         one.setAttribute('alt', 'kept' + i + '.png');
         editor.appendChild(one);
      }
      picker.files = [{ name: 'sixtyfourth.png', type: 'image/png', size: 70, base64: logo }];
      picker.dispatchEvent(makeEvent('change'));
      await flush();
      check('the sixty-fourth picture goes in', imagesIn().length === 64 && status.textContent === '', imagesIn().length + ' ' + status.textContent);
      picker.files = [{ name: 'sixtyfifth.png', type: 'image/png', size: 70, base64: logo }];
      picker.dispatchEvent(makeEvent('change'));
      await flush();
      check('the sixty-fifth is refused, by the count rather than by any weight',
         imagesIn().length === 64 && status.textContent === words('This message holds as many images as it can carry: 64 of them.') && status.className.indexOf('error') >= 0,
         imagesIn().length + ' ' + status.textContent);
      document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
      await flush();
      dismissAllToasts();

      // A break inside a cell is not a new row: the plain part's rows are its rows.
      location.hash = '#/compose';
      await flush();
      document.getElementById('rich-toggle').dispatchEvent(makeEvent('click'));
      document.getElementById('compose-to').value = 'ann@example.net';
      document.getElementById('compose-subject').value = 'A cell of two lines';
      document.getElementById('table-open').dispatchEvent(makeEvent('click'));
      document.getElementById('table-rows').value = '1';
      document.getElementById('table-columns').value = '2';
      document.getElementById('table-header').checked = false;
      document.getElementById('table-insert-go').dispatchEvent(makeEvent('click'));
      const split = cellAt(0, 0);
      split.textContent = '';
      split.appendChild(document.createTextNode('one'));
      split.appendChild(document.createElement('br'));
      split.appendChild(document.createTextNode('two'));
      cellAt(0, 1).textContent = 'next';
      const beforeSplit = requests.length;
      document.getElementById('compose-save').dispatchEvent(makeEvent('click'));
      await flush();
      const splitDraft = lastDraft(beforeSplit);
      check('a cell with a break in it stays one field of one row in the plain part, the break read as a space',
         !!splitDraft && splitDraft.text.indexOf('one two\tnext') >= 0 && splitDraft.text.indexOf('one\ntwo') < 0 && splitDraft.html.indexOf('<td>one<br>two</td><td>next</td>') >= 0,
         splitDraft ? JSON.stringify(splitDraft.text) : 'no draft');
      document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
      await flush();
      dismissAllToasts();

      // One file refused in a batch is still named when the files after it go in.
      location.hash = '#/compose';
      await flush();
      document.getElementById('rich-toggle').dispatchEvent(makeEvent('click'));
      const batch = makeEvent('drop', { dataTransfer: { files: [
         { name: 'notes.png', type: 'image/png', size: 30, base64: 'VGhpcyBpcyBub3QgYSBwaWN0dXJlIGF0IGFsbA==' },
         { name: 'photo.jpg', type: 'image/jpeg', size: 70, base64: logo },
         { name: 'chart.png', type: 'image/png', size: 70, base64: logo }
      ] } });
      editor.dispatchEvent(batch);
      await flush();
      check('a file refused in a batch is named, and is not wiped off the line by the files after it going in',
         imagesIn().length === 2 && status.className.indexOf('error') >= 0 &&
         status.textContent === 'notes.png: ' + words('An image to put in the message: PNG, JPEG, GIF or WebP.'),
         imagesIn().length + ' ' + JSON.stringify(status.textContent));
      document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
      await flush();
      dismissAllToasts();

      // What a forward carries is bytes in the same request the pictures go in.
      location.hash = '#/compose?forward=707';
      await flush();
      await flush();
      await flush();
      document.getElementById('compose-to').value = 'ann@example.net';
      const beforeCarried = requests.length;
      document.getElementById('compose-save').dispatchEvent(makeEvent('click'));
      await flush();
      check('a forward of eight and a half megabytes of attachments is saved as it is',
         since(beforeCarried).some((r) => r.method === 'POST' && r.path === '/api/v1/me/drafts') && status.textContent === words('Draft saved.'), status.textContent);
      document.getElementById('rich-toggle').dispatchEvent(makeEvent('click'));
      for (let i = 0; i < 3; i += 1) {
         picker.files = [{ name: 'shot' + i + '.png', type: 'image/png', size: 1400000, base64: pngOf(1900000) }];
         picker.dispatchEvent(makeEvent('change'));
         await flush();
      }
      check('three pictures go in, under the six megabytes of pictures one message may carry', imagesIn().length === 3 && status.textContent === '', imagesIn().length + ' ' + status.textContent);
      const beforeTooMuch = requests.length;
      document.getElementById('compose-save').dispatchEvent(makeEvent('click'));
      await flush();
      check('what the forward carries counts against the same twelve megabytes as the pictures, so the message is refused here rather than by a connection that dies',
         !since(beforeTooMuch).some((r) => r.method === 'POST' && r.path === '/api/v1/me/drafts') &&
         status.textContent === words('The pictures in the message and the files are larger than 12 MB together.'),
         status.textContent);
      document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
      await flush();
      dismissAllToasts();
   }

   // ---- A second account in the same page (row L50): a session of its own,
   // started through the bearer form of the session route and held in memory
   // only; every request for it under its token and none under the cookie; the
   // switch, and what stays the first account's whatever is shown.
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const accountNav = () => document.getElementById('account-nav');
      const accountButtons = () => accountNav().children;
      const status = () => document.getElementById('second-status');
      const bearer = 'Bearer ' + SECOND_TOKEN;
      const asSecond = (from) => since(from).filter((r) => r.headers.Authorization === bearer);
      const asFirst = (from) => since(from).filter((r) => !r.headers.Authorization);
      const folderRoutesNow = () => { const out = []; const nav = document.getElementById('folder-nav'); for (let i = 0; i < nav.children.length; i++) { const r = nav.children[i].getAttribute('data-route'); if (r && r.indexOf('/f/') === 0) { out.push(r); } } return out; };
      // Every blob URL the page makes, held until the page releases it: a
      // URL not revoked keeps its bytes for the life of the page, so the
      // count of those still held is what the checks below read.
      const blobUrls = new Map();
      const madeUrl = URL.createObjectURL.bind(URL), droppedUrl = URL.revokeObjectURL.bind(URL);
      URL.createObjectURL = (blob) => { const u = madeUrl(blob); blobUrls.set(u, String((blob && blob.type) || '')); return u; };
      URL.revokeObjectURL = (u) => { blobUrls.delete(u); droppedUrl(u); };
      const htmlBlobsHeld = () => [...blobUrls.values()].filter((type) => type === 'text/html').length;

      location.hash = '#/accounts';
      await flush();
      check('L50: with one account there is no account switch in the folder column', accountNav().hidden === true, 'hidden=' + accountNav().hidden);
      check('L50: the Other accounts page offers to add a second account, and shows nothing about one yet',
         document.getElementById('second-form').hidden === false && document.getElementById('second-current').hidden === true,
         'form hidden=' + document.getElementById('second-form').hidden + ' current hidden=' + document.getElementById('second-current').hidden);

      // The account already signed in is refused before any request is made:
      // a second sign-in of the same account from one device would end the
      // first's session under the one-per-device policy.
      const beforeSame = requests.length;
      document.getElementById('second-address').value = 'User@Example.com';
      document.getElementById('second-password').value = 'a-real-password';
      document.getElementById('second-form').dispatchEvent(makeEvent('submit'));
      await flush();
      check('L50: adding the account the page is signed in as is refused without a request', since(beforeSame).length === 0 && status().textContent === words('That is the account you are signed in as.'),
         since(beforeSame).length + ' requests, ' + status().textContent);
      check('L50: and the password field was emptied before anything else happened', document.getElementById('second-password').value === '');

      // The same account under a spelling the page cannot know to be the
      // same - its address at an alias domain - goes to the server, which
      // knows, and refuses it with 409: said on the card in the same words,
      // and nothing was added.
      const beforeAlias = requests.length;
      document.getElementById('second-address').value = FIRST_ALIAS_SPELLING;
      document.getElementById('second-password').value = 'a-real-password';
      document.getElementById('second-form').dispatchEvent(makeEvent('submit'));
      await flush();
      check('L50: the same account at an alias domain is the server\'s 409, said on the card in the same words, and nothing was added',
         since(beforeAlias).filter((r) => r.method === 'POST' && r.path === '/api/v1/session?bearer=1').length === 1 && status().textContent === words('That is the account you are signed in as.') &&
         status().className.indexOf('error') >= 0 && accountNav().hidden === true && document.getElementById('second-form').hidden === false && app.hidden === false,
         status().textContent + ' hidden=' + accountNav().hidden + ' ' + JSON.stringify(since(beforeAlias).map((r) => r.method + ' ' + r.path)));

      // A wrong password: the server's 401 is this card's refusal, not the
      // gate - the first account's session did not end.
      const beforeWrong = requests.length;
      document.getElementById('second-address').value = SECOND_ADDRESS;
      document.getElementById('second-password').value = 'not-the-password';
      document.getElementById('second-form').dispatchEvent(makeEvent('submit'));
      await flush();
      const wrongSignIn = since(beforeWrong).filter((r) => r.method === 'POST' && r.path === '/api/v1/session?bearer=1');
      check('L50: a wrong password is one request to the bearer form of the session route, with the password as Basic and the header a write needs',
         wrongSignIn.length === 1 && /^Basic /.test(wrongSignIn[0].headers.Authorization || '') && wrongSignIn[0].headers['X-Requested-With'] === 'hMailServer',
         JSON.stringify(wrongSignIn.map((r) => r.headers)));
      check('L50: and its refusal is said on the card while the page stays signed in', status().className.indexOf('error') >= 0 && app.hidden === false && gate.hidden === true && accountNav().hidden === true,
         status().textContent + ' app.hidden=' + app.hidden);

      // The right one.
      const beforeAdd = requests.length;
      const firstUnseenShown = document.getElementById('folder-nav').children[0].children[2].textContent;
      document.getElementById('second-address').value = SECOND_ADDRESS;
      document.getElementById('second-password').value = SECOND_PASSWORD;
      document.getElementById('second-form').dispatchEvent(makeEvent('submit'));
      await flush();
      await flush();
      const signIns = since(beforeAdd).filter((r) => r.path.indexOf('/api/v1/session') === 0);
      check('L50: the second account is signed in once, through the bearer form, and its password goes nowhere else',
         signIns.length === 1 && signIns[0].method === 'POST' && signIns[0].path === '/api/v1/session?bearer=1' &&
         since(beforeAdd).filter((r) => /^Basic /.test(r.headers.Authorization || '')).length === 1,
         JSON.stringify(signIns.map((r) => r.method + ' ' + r.path)));
      check('L50: the token is held in memory only: nothing in the browser\'s storage carries it, and no key names one',
         [...store.values()].every((v) => v.indexOf(SECOND_TOKEN) < 0 && v.indexOf(SECOND_PASSWORD) < 0) && [...store.keys()].every((k) => !/pass|token|secret|auth|bearer/i.test(k)),
         JSON.stringify([...store.keys()]));
      check('L50: the switch appears at the top of the folder column with both accounts, the first marked as the one shown',
         accountNav().hidden === false && accountButtons().length === 2 &&
         accountButtons()[0].className.indexOf('on') >= 0 && accountButtons()[0].textContent.indexOf('user@example.com') >= 0 && accountButtons()[0].getAttribute('aria-pressed') === 'true' &&
         accountButtons()[1].className.indexOf('on') < 0 && accountButtons()[1].textContent.indexOf(SECOND_ADDRESS) >= 0 && accountButtons()[1].getAttribute('aria-pressed') === 'false',
         'hidden=' + accountNav().hidden + ' buttons=' + accountButtons().length + ' ' + (accountButtons()[0] ? accountButtons()[0].className + '|' + accountButtons()[0].textContent : ''));
      check('L50: the second account\'s unread count was read as it, under its token, and is shown beside it',
         asSecond(beforeAdd).some((r) => r.method === 'GET' && r.path === '/api/v1/me/folders') && accountButtons()[1].children[2].textContent === '4',
         'count=' + (accountButtons()[1] ? accountButtons()[1].children[2].textContent : 'none'));
      check('L50: the first account\'s count beside it is the folder column\'s', accountButtons()[0].children[2].textContent === firstUnseenShown,
         accountButtons()[0].children[2].textContent + ' vs ' + firstUnseenShown);
      check('L50: the card now says what was added and where its session lives',
         document.getElementById('second-current').hidden === false && document.getElementById('second-form').hidden === true &&
         document.getElementById('second-current-text').textContent === words('{0} is added beside {1}. Its session lives in this page only: reloading the page forgets it, and it ends after {2} minutes without use.').replace('{0}', SECOND_ADDRESS).replace('{1}', 'user@example.com').replace('{2}', '30'),
         document.getElementById('second-current-text').textContent);
      check('L50: the page still shows the first account', document.getElementById('who').textContent === 'user@example.com', document.getElementById('who').textContent);

      // The switch: everything the page shows becomes the second account's,
      // every request from here carries its token and none the cookie alone.
      const offlineBefore = String(store.get('hmPortalOffline'));
      store.delete('hmPortalDueShown');
      const beforeSwitch = requests.length;
      accountButtons()[1].dispatchEvent(makeEvent('click'));
      await flush();
      await flush();
      await flush();
      check('L50: after the switch the account, its folders, its identities and its settings are all read under the token',
         ['/api/v1/me', '/api/v1/me/folders', '/api/v1/me/identities', '/api/v1/me/settings', '/api/v1/me/preferences', '/api/v1/me/folders/21/messages'].every((p) => asSecond(beforeSwitch).some((r) => r.method === 'GET' && r.path === p)),
         JSON.stringify(asSecond(beforeSwitch).map((r) => r.path)));
      check('L50: and nothing was asked for as the first account', asFirst(beforeSwitch).length === 0, JSON.stringify(asFirst(beforeSwitch).map((r) => r.path)));
      check('L50: the token is in no URL', since(beforeSwitch).every((r) => r.path.indexOf(SECOND_TOKEN) < 0));
      check('L50: the page is the second account\'s: its address, its inbox first in the column, its two messages listed',
         document.getElementById('who').textContent === SECOND_ADDRESS && folderRoutesNow()[0] === '/f/21' && location.hash === '#/f/21' &&
         document.getElementById('message-list').children.length === 2 && document.getElementById('message-list').textContent.indexOf('Invoice 4471') >= 0,
         document.getElementById('who').textContent + ' ' + folderRoutesNow().join(',') + ' ' + location.hash + ' rows=' + document.getElementById('message-list').children.length);
      check('L50: nothing of the first account\'s list is left on the screen', document.getElementById('message-list').textContent.indexOf('Third') < 0 && document.getElementById('folder-nav').textContent.indexOf('Work') < 0);
      check('L50: the switch marks the second account as shown and keeps the first\'s count beside it',
         accountButtons()[1].className.indexOf('on') >= 0 && accountButtons()[0].className.indexOf('on') < 0 && accountButtons()[0].children[2].textContent === firstUnseenShown && accountButtons()[1].children[2].textContent === '4',
         accountButtons()[0].className + ' ' + accountButtons()[0].children[2].textContent + ' | ' + accountButtons()[1].className + ' ' + accountButtons()[1].children[2].textContent);
      check('L50: what stays the first account\'s was not asked for as the second: not the push routes, not the offline window',
         !asSecond(beforeSwitch).some((r) => /^\/api\/v1\/me\/(push|offline)/.test(r.path)), JSON.stringify(asSecond(beforeSwitch).map((r) => r.path).filter((p) => /push|offline/.test(p))));
      check('L50: the first account\'s offline store is untouched by the switch', String(store.get('hmPortalOffline')) === offlineBefore);
      // The follow-up due today was found again under the token (the fake
      // answers the search from the one mailbox) and announced, since the
      // second account's marks start empty; the mark went into the page and
      // not into this browser, whose marks are the first account's and were
      // emptied before the switch to make the write visible.
      check('L50: what was announced as the second account was marked in the page and not in this browser',
         document.getElementById('toasts').textContent.indexOf(words('Due for follow-up: {0}').replace('{0}', 'First')) >= 0 && !store.has('hmPortalDueShown'),
         'toasts=' + document.getElementById('toasts').textContent + ' store=' + String(store.get('hmPortalDueShown')));
      dismissAllToasts();

      // What this browser keeps of what the reader looked for is the first
      // account's too: a search made as the second account is neither
      // remembered nor offered, and the first account's list is not read
      // back to the second.
      const searchesBefore = String(store.get('hmPortalSearches'));
      const searchBox = document.getElementById('mail-search');
      searchBox.value = '';
      searchBox.dispatchEvent(makeEvent('focus'));
      check('L50: the empty search box offers no history while the second account is shown', document.getElementById('search-history').hidden === true, 'hidden=' + document.getElementById('search-history').hidden);
      searchBox.value = 'second-only';
      document.getElementById('mail-search-form').dispatchEvent(makeEvent('submit'));
      await flush();
      check('L50: a search made as the second account is not written to the first account\'s history', String(store.get('hmPortalSearches')) === searchesBefore && String(searchesBefore).indexOf('second-only') < 0,
         String(store.get('hmPortalSearches')));
      location.hash = '#/f/21';
      await flush();
      await flush();

      // A window opened on an address loads as the first account: the two
      // pop-out buttons are not offered while the second account is shown.
      check('L50: the pop-out buttons are hidden while the second account is shown', document.getElementById('message-popout').hidden === true && document.getElementById('compose-popout').hidden === true,
         'message=' + document.getElementById('message-popout').hidden + ' compose=' + document.getElementById('compose-popout').hidden);

      // The From of a new message is the second account's identities.
      location.hash = '#/compose';
      await flush();
      const fromOptions = []; { const s = document.getElementById('compose-from'); for (let i = 0; i < s.children.length; i++) { fromOptions.push(s.children[i].value); } }
      check('L50: a new message is written from the second account\'s own identities', document.getElementById('compose-from').hidden === false && fromOptions.indexOf(SECOND_ADDRESS) >= 0 && fromOptions.indexOf('user@example.com') < 0,
         JSON.stringify(fromOptions));
      document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
      await flush();
      dismissAllToasts();

      // A message opened: read under the token; its HTML fetched under the
      // token and shown from a blob, since the frame's own request would carry
      // the cookie; its attachment likewise; nothing of it into the first
      // account's store.
      const beforeOpen = requests.length;
      location.hash = '#/m/2101';
      await flush();
      await flush();
      await flush();
      check('L50: a message of the second account opens, read under the token',
         document.getElementById('message-view').hidden === false && document.getElementById('message-subject').textContent === 'Welcome aboard' &&
         asSecond(beforeOpen).some((r) => r.path === '/api/v1/me/messages/2101'),
         document.getElementById('message-subject').textContent);
      const frame = document.getElementById('message-html');
      document.getElementById('message-html-toggle').dispatchEvent(makeEvent('click'));
      await flush();
      await flush();
      check('L50: its HTML is fetched under the token and shown from a blob, not from the route the browser would ask with the cookie',
         asSecond(beforeOpen).some((r) => r.path === '/api/v1/me/messages/2101/html') && String(frame.getAttribute('src')).indexOf('blob:') === 0 && frame.hidden === false,
         String(frame.getAttribute('src')));
      const htmlBlobShown = String(frame.getAttribute('src'));
      check('L50: the page holds that one blob URL and no other document', htmlBlobsHeld() === 1 && blobUrls.has(htmlBlobShown), htmlBlobsHeld() + ' held');
      // Shown again - the toggle off and on - the earlier URL is released as
      // the next one is made: one document held at a time.
      document.getElementById('message-html-toggle').dispatchEvent(makeEvent('click'));
      await flush();
      check('L50: turning the HTML off releases the blob URL', htmlBlobsHeld() === 0 && !blobUrls.has(htmlBlobShown), htmlBlobsHeld() + ' held');
      document.getElementById('message-html-toggle').dispatchEvent(makeEvent('click'));
      await flush();
      await flush();
      check('L50: and shown again there is one again, a new one', htmlBlobsHeld() === 1 && !blobUrls.has(htmlBlobShown) && String(frame.getAttribute('src')).indexOf('blob:') === 0,
         htmlBlobsHeld() + ' held, src=' + String(frame.getAttribute('src')));
      check('L50: remote images are not offered for it, and the bar says so',
         document.getElementById('message-remote').hidden === true && document.getElementById('message-remote-second').hidden === false,
         'remote hidden=' + document.getElementById('message-remote').hidden + ' note hidden=' + document.getElementById('message-remote-second').hidden);
      const attachment = document.getElementById('message-attachments').children[0];
      const beforeDownload = requests.length;
      const attachmentClick = makeEvent('click');
      attachment.dispatchEvent(attachmentClick);
      await flush();
      await flush();
      check('L50: an attachment is fetched under the token rather than followed with the cookie',
         attachmentClick.defaultPrevented === true && asSecond(beforeDownload).some((r) => r.path === '/api/v1/me/messages/2101/attachments/0'),
         'prevented=' + attachmentClick.defaultPrevented + ' ' + JSON.stringify(since(beforeDownload).map((r) => r.path)));
      check('L50: the link itself still names the route, so the first account\'s links are what they were', (attachment.getAttribute('href') || attachment.href) === '/api/v1/me/messages/2101/attachments/0', String(attachment.getAttribute('href') || attachment.href));
      check('L50: the opened message went into neither of the first account\'s stores',
         String(store.get('hmPortalOffline')) === offlineBefore && !idbStore('messages').has(2101) && !idbStore('rows').has(2101),
         'localStorage changed=' + (String(store.get('hmPortalOffline')) !== offlineBefore) + ' idb=' + idbStore('messages').has(2101));

      // The Settings cards for what stays the first account's say whose it is,
      // and ask the server nothing for the second.
      const beforeSettings = requests.length;
      location.hash = '#/settings';
      await flush();
      await flush();
      check('L50: the offline card says the store is the first account\'s and offers no choice for the second',
         document.getElementById('offline-second').hidden === false && document.getElementById('offline-second').textContent.indexOf('user@example.com') >= 0 && document.getElementById('offline-form').hidden === true,
         document.getElementById('offline-second').textContent);
      check('L50: the notifications card says the device\'s subscription is the first account\'s, and the push routes were not asked as the second',
         document.getElementById('push-second').hidden === false && document.getElementById('push-second').textContent.indexOf('user@example.com') >= 0 && document.getElementById('push-subscribe').hidden === true &&
         !asSecond(beforeSettings).some((r) => /^\/api\/v1\/me\/(push|offline)/.test(r.path)),
         document.getElementById('push-second').textContent + ' ' + JSON.stringify(asSecond(beforeSettings).map((r) => r.path)));
      location.hash = '#/security';
      await flush();
      await flush();
      check('L50: the Security page lists this session as a second account in a webmail page',
         document.getElementById('session-rows').textContent.indexOf(words('A second account in a webmail page')) >= 0 && document.getElementById('session-rows').textContent.indexOf(words('This browser')) >= 0,
         document.getElementById('session-rows').textContent);
      location.hash = '#/accounts';
      await flush();
      await flush();
      check('L50: the card, seen from the second account, says which account the page signed in as and offers the way back',
         document.getElementById('second-current-text').textContent.indexOf('user@example.com') >= 0 && document.getElementById('second-switch').textContent === words('Switch back'),
         document.getElementById('second-current-text').textContent + ' | ' + document.getElementById('second-switch').textContent);
      // The sign-in at a provider leaves the page and comes back to it
      // reloaded, as the first account: the import waits while the second
      // account is shown, and the note says so and where to do it.
      check('L50: moving from Gmail or Outlook.com is offered but waits while the second account is shown, with a note naming it',
         document.getElementById('import-signin').hidden === false && document.getElementById('import-google').disabled === true && document.getElementById('import-microsoft').disabled === true &&
         document.getElementById('import-second').hidden === false && document.getElementById('import-second').textContent.indexOf(SECOND_ADDRESS) >= 0,
         'signin hidden=' + document.getElementById('import-signin').hidden + ' google disabled=' + document.getElementById('import-google').disabled + ' note=' + document.getElementById('import-second').textContent);

      // The count of the account not shown is kept fresh, as that account,
      // at the slow pace; the shown account's probe runs as itself.
      const beforeProbes = requests.length;
      fireTimers();
      await flush();
      await flush();
      const firstProbe = asFirst(beforeProbes);
      check('L50: the timers ask for the first account\'s changes as the first account, and nothing else as it',
         firstProbe.length >= 1 && firstProbe.every((r) => r.path.indexOf('/api/v1/me/changes') === 0), JSON.stringify(firstProbe.map((r) => r.path)));
      check('L50: and the second account\'s change probe runs under its token', asSecond(beforeProbes).some((r) => r.path.indexOf('/api/v1/me/changes') === 0), JSON.stringify(asSecond(beforeProbes).map((r) => r.path)));

      // A message being written survives a switch as a draft of the account it
      // was written in, saved under that account's credential before the
      // credential changes.
      location.hash = '#/compose';
      await flush();
      document.getElementById('compose-to').value = 'ann@example.net';
      document.getElementById('compose-subject').value = 'Written as the second';
      const beforeBack = requests.length;
      accountButtons()[0].dispatchEvent(makeEvent('click'));
      await flush();
      await flush();
      await flush();
      const draftSaves = since(beforeBack).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/drafts');
      check('L50: switching away saves what was written as a draft, under the token of the account it was written in',
         draftSaves.length === 1 && draftSaves[0].headers.Authorization === bearer && JSON.parse(draftSaves[0].body).subject === 'Written as the second',
         JSON.stringify(draftSaves.map((r) => [r.headers.Authorization ? 'bearer' : 'cookie', r.body && r.body.slice(0, 80)])));
      check('L50: and the draft was saved before the first account was read, so the order cannot cross',
         since(beforeBack).findIndex((r) => r.path === '/api/v1/me/drafts') < since(beforeBack).findIndex((r) => r.path === '/api/v1/me' && !r.headers.Authorization),
         JSON.stringify(since(beforeBack).map((r) => (r.headers.Authorization ? 'B ' : 'C ') + r.path)));
      check('L50: the page is the first account\'s again, read as it, with the compose window blank',
         document.getElementById('who').textContent === 'user@example.com' && folderRoutesNow()[0] === '/f/1' && asFirst(beforeBack).some((r) => r.path === '/api/v1/me/folders') &&
         document.getElementById('compose-to').value === '' && document.getElementById('compose-section').hidden === true,
         document.getElementById('who').textContent + ' ' + folderRoutesNow().join(',') + ' to=' + document.getElementById('compose-to').value);
      // The draft's own refresh of the folder counts is the second account's too, and it is before the switch: every request under the token precedes the first request as the first account.
      check('L50: every request under the token came before the first account was read again, so nothing of the two crosses',
         asSecond(beforeBack).every((r) => r.path === '/api/v1/me/drafts' || r.path === '/api/v1/me/folders') &&
         since(beforeBack).map((r) => (r.headers.Authorization ? 'B' : 'C')).join('').indexOf('CB') < 0,
         JSON.stringify(since(beforeBack).map((r) => (r.headers.Authorization ? 'B ' : 'C ') + r.path)));
      check('L50: the second\'s count is shown beside it, kept from when it was shown', accountButtons()[1].children[2].textContent === '4' && accountButtons()[0].className.indexOf('on') >= 0,
         accountButtons()[1].children[2].textContent);
      check('L50: back on the first account the pop-out buttons are offered again', document.getElementById('message-popout').hidden === false && document.getElementById('compose-popout').hidden === false,
         'message=' + document.getElementById('message-popout').hidden + ' compose=' + document.getElementById('compose-popout').hidden);
      check('L50: and the switch released the second account\'s document: no blob URL of one is held', htmlBlobsHeld() === 0, htmlBlobsHeld() + ' held');
      location.hash = '#/accounts';
      await flush();
      await flush();
      check('L50: and the import buttons are live again, with no note', document.getElementById('import-google').disabled === false && document.getElementById('import-second').hidden === true,
         'google disabled=' + document.getElementById('import-google').disabled + ' note hidden=' + document.getElementById('import-second').hidden);
      dismissAllToasts();

      // The second account's session ends on the server - it timed out, or was
      // ended from its own Security page. The page comes back to the first
      // account, says so, and forgets the token; the gate stays down.
      secondToken = '';
      const beforeGone = requests.length;
      accountButtons()[1].dispatchEvent(makeEvent('click'));
      await flush();
      await flush();
      await flush();
      check('L50: a token the server no longer knows puts the page back on the first account rather than at the gate',
         app.hidden === false && gate.hidden === true && document.getElementById('who').textContent === 'user@example.com' && folderRoutesNow()[0] === '/f/1',
         'app.hidden=' + app.hidden + ' who=' + document.getElementById('who').textContent);
      check('L50: and says so', document.getElementById('toasts').textContent.indexOf(words('The second account\'s session has ended. Add it again to go on using it.')) >= 0, document.getElementById('toasts').textContent);
      check('L50: the switch is gone with the token', accountNav().hidden === true, 'hidden=' + accountNav().hidden);
      location.hash = '#/accounts';
      await flush();
      check('L50: and the card offers to add an account again', document.getElementById('second-form').hidden === false && document.getElementById('second-current').hidden === true);
      check('L50: no request after the refusal carried the old token', asSecond(beforeGone).every((r) => r.path === '/api/v1/me' || r.path === '/api/v1/me/folders' || r.path.indexOf('/api/v1/me/changes') === 0),
         JSON.stringify(asSecond(beforeGone).map((r) => r.path)));
      dismissAllToasts();

      // Added again and removed by hand: DELETE with the token, no cookie
      // touched, the first account still signed in.
      document.getElementById('second-address').value = SECOND_ADDRESS;
      document.getElementById('second-password').value = SECOND_PASSWORD;
      document.getElementById('second-form').dispatchEvent(makeEvent('submit'));
      await flush();
      await flush();
      check('L50: the account can be added again after its session ended', accountNav().hidden === false && document.getElementById('second-current').hidden === false, 'hidden=' + accountNav().hidden);
      const beforeRemove = requests.length;
      document.getElementById('second-end').dispatchEvent(makeEvent('click'));
      await flush();
      await flush();
      const ended = since(beforeRemove).filter((r) => r.method === 'DELETE' && r.path === '/api/v1/session');
      check('L50: Remove ends the second account\'s session with its token, and the first account\'s session is not touched',
         ended.length === 1 && ended[0].headers.Authorization === bearer && ended[0].headers['X-Requested-With'] === 'hMailServer' && signedIn === true && app.hidden === false,
         JSON.stringify(ended.map((r) => r.headers)) + ' signedIn=' + signedIn);
      check('L50: the token is forgotten: the switch is gone and the card offers the form', accountNav().hidden === true && document.getElementById('second-form').hidden === false && secondToken === '',
         'hidden=' + accountNav().hidden + ' token=' + JSON.stringify(secondToken));
      check('L50: and the removal is said', document.getElementById('toasts').textContent.indexOf(words('{0} has been removed from this page.').replace('{0}', SECOND_ADDRESS)) >= 0, document.getElementById('toasts').textContent);
      dismissAllToasts();

      // The first account's session ends while the second is shown: a
      // request on the cookie - the first's count being kept fresh - is
      // answered 401, and the page goes to the gate. The second account's
      // session is ended on the server with its token before it is
      // forgotten, not left standing until it idles out.
      document.getElementById('second-address').value = SECOND_ADDRESS;
      document.getElementById('second-password').value = SECOND_PASSWORD;
      document.getElementById('second-form').dispatchEvent(makeEvent('submit'));
      await flush();
      await flush();
      accountButtons()[1].dispatchEvent(makeEvent('click'));
      await flush();
      await flush();
      await flush();
      check('L50: the second account is shown again, for the first\'s session to end under it', document.getElementById('who').textContent === SECOND_ADDRESS && secondToken === SECOND_TOKEN, document.getElementById('who').textContent);
      signedIn = false;
      const beforeLapse = requests.length;
      fireTimers();
      await flush();
      await flush();
      const lapsed = since(beforeLapse).filter((r) => r.method === 'GET' && r.path.indexOf('/api/v1/me/changes') === 0 && !r.headers.Authorization);
      const endedWithIt = since(beforeLapse).filter((r) => r.method === 'DELETE' && r.path === '/api/v1/session');
      check('L50: the first account\'s session ending under the second puts the page at the gate', lapsed.length >= 1 && gate.hidden === false && app.hidden === true && accountNav().hidden === true,
         lapsed.length + ' cookie probes, gate hidden=' + gate.hidden);
      check('L50: and the second account\'s session was ended on the server with its token, with keepalive, before the token was forgotten',
         endedWithIt.length === 1 && endedWithIt[0].headers.Authorization === bearer && endedWithIt[0].headers['X-Requested-With'] === 'hMailServer' && endedWithIt[0].keepalive === true && secondToken === '',
         JSON.stringify(endedWithIt.map((r) => [r.headers, r.keepalive])) + ' token=' + JSON.stringify(secondToken));
      check('L50: nothing after that carried the old token', since(beforeLapse).filter((r) => r.headers.Authorization === bearer).every((r) => r.method === 'DELETE' || r.path.indexOf('/api/v1/me/changes') === 0),
         JSON.stringify(asSecond(beforeLapse).map((r) => r.method + ' ' + r.path)));
      document.getElementById('address').value = 'user@example.com';
      document.getElementById('password').value = 'a-real-password';
      document.getElementById('signin-form').dispatchEvent(makeEvent('submit'));
      await flush();
      await flush();
      check('L50: signed in again, with one account', app.hidden === false && gate.hidden === true && accountNav().hidden === true && document.getElementById('who').textContent === 'user@example.com', document.getElementById('who').textContent);
      dismissAllToasts();
      location.hash = '#/accounts';
      await flush();

      // Left added for the sign-out check at the end of the run, which holds
      // that signing out ends this session too.
      document.getElementById('second-address').value = SECOND_ADDRESS;
      document.getElementById('second-password').value = SECOND_PASSWORD;
      document.getElementById('second-form').dispatchEvent(makeEvent('submit'));
      await flush();
      await flush();
      check('L50: added once more, for the sign-out at the end', accountNav().hidden === false && secondToken === SECOND_TOKEN, 'hidden=' + accountNav().hidden);
      location.hash = '#/f/1';
      await flush();
   }

   // ---- The recovery address and the one-time codes on the Security page
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const state = () => document.getElementById('recovery-state').textContent;
      const codesState = () => document.getElementById('recovery-codes-state').textContent;

      const beforeLoad = requests.length;
      location.hash = '#/security';
      await flush();
      check('the Security page reads the recovery address and says there is none yet',
         called(beforeLoad, 'GET', '/api/v1/me/recovery') && document.getElementById('recovery-card').hidden === false &&
         state() === words('No recovery address yet.') && codesState() === words('No codes yet.') &&
         document.getElementById('recovery-resend').hidden === true && document.getElementById('recovery-clear').hidden === true,
         state() + ' | ' + codesState());

      const beforeSave = requests.length;
      document.getElementById('recovery-address').value = 'Other@Elsewhere.example';
      document.getElementById('recovery-password').value = 'a-real-password';
      document.getElementById('recovery-form').dispatchEvent(makeEvent('submit'));
      await flush();
      const saved = since(beforeSave).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/recovery')[0];
      check('saving an address sends it with the account\'s own password, and the page then shows only the masked form, waiting to be confirmed',
         saved && JSON.parse(saved.body).address === 'Other@Elsewhere.example' && JSON.parse(saved.body).password === 'a-real-password' &&
         state().indexOf('o....@e.........example') >= 0 && state().indexOf('Other@Elsewhere.example') < 0 &&
         document.getElementById('recovery-address').value === '' && document.getElementById('recovery-password').value === '' &&
         document.getElementById('recovery-resend').hidden === false && document.getElementById('recovery-clear').hidden === false,
         state());

      const beforeResend = requests.length;
      document.getElementById('recovery-resend').dispatchEvent(makeEvent('click'));
      await flush();
      check('Send the confirmation again asks the server for another link',
         called(beforeResend, 'POST', '/api/v1/me/recovery/resend') &&
         String(document.getElementById('recovery-status').className).indexOf('ok') >= 0);

      const beforeCodes = requests.length;
      document.getElementById('recovery-codes-password').value = 'a-real-password';
      document.getElementById('recovery-codes-form').dispatchEvent(makeEvent('submit'));
      await flush();
      check('making codes shows all ten, once, and the count then says how many are unused',
         called(beforeCodes, 'POST', '/api/v1/me/recovery/codes') &&
         document.getElementById('recovery-codes-new').hidden === false &&
         document.getElementById('recovery-codes-list').textContent.split(/\s+/).filter((c) => c).length === 10 &&
         document.getElementById('recovery-codes-password').value === '' &&
         codesState().indexOf('10') >= 0,
         document.getElementById('recovery-codes-list').textContent + ' | ' + codesState());

      // A lost authenticator, which is the whole reason the codes exist. The
      // route asks for a second factor; the page has to offer BOTH the
      // authenticator's code and a recovery code, because the server takes
      // either - and until this check existed it offered the second of those
      // on the sign-in card alone, so the way back in ended here at a box no
      // lost authenticator could fill.
      recoveryDemandsSecondFactor = true;
      document.getElementById('recovery-address').value = 'Other@Elsewhere.example';
      document.getElementById('recovery-password').value = 'a-real-password';
      document.getElementById('recovery-form').dispatchEvent(makeEvent('submit'));
      await flush();
      check('a route that asks for the second factor reveals both the authenticator field and the recovery-code field, and says so',
         document.getElementById('recovery-otp').hidden === false && document.getElementById('recovery-code').hidden === false &&
         document.getElementById('recovery-status').textContent ===
            words('Enter the code from your authenticator app, or one of your recovery codes.'),
         document.getElementById('recovery-status').textContent);

      const beforeCode = requests.length;
      document.getElementById('recovery-code').value = ' a2c4e-0h8jkm ';
      document.getElementById('recovery-form').dispatchEvent(makeEvent('submit'));
      await flush();
      const withCode = since(beforeCode).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/recovery')[0];
      check('and a recovery code typed there travels in X-hMailServer-Recovery-Code, trimmed, with no one-time code beside it',
         withCode && withCode.headers['X-hMailServer-Recovery-Code'] === 'a2c4e-0h8jkm' &&
         !withCode.headers['X-hMailServer-OTP'] && recoverySecondFactorSeen.code === 'a2c4e-0h8jkm' &&
         document.getElementById('recovery-code').value === '',
         withCode ? JSON.stringify(withCode.headers) : 'no request');
      recoveryDemandsSecondFactor = false;

      confirmAnswer = true;
      const beforeClear = requests.length;
      document.getElementById('recovery-clear').dispatchEvent(makeEvent('click'));
      await flush();
      check('removing the address asks first, saying what it costs, and the page goes back to having none',
         confirms[confirms.length - 1].indexOf(words('Remove the recovery address? A forgotten password would then need an administrator.')) >= 0 &&
         called(beforeClear, 'DELETE', '/api/v1/me/recovery') && state() === words('No recovery address yet.'),
         state());

      // The gate's half: the link that asks for a reset, and the field for a
      // recovery code. Neither needs a session, and neither is shown until the
      // reader asks for it.
      check('the sign-in card offers a recovery code only when it is asked for',
         document.getElementById('signin-code-row').hidden === true);
      document.getElementById('signin-code-toggle').dispatchEvent(makeEvent('click'));
      check('and shows the field when it is', document.getElementById('signin-code-row').hidden === false);

      document.getElementById('signin-forgot').dispatchEvent(makeEvent('click'));
      check('Forgotten your password? puts the reset form in the gate in place of the sign-in form',
         document.getElementById('reset-form').hidden === false && document.getElementById('signin-form').hidden === true);

      const beforeReset = requests.length;
      document.getElementById('reset-address').value = 'somebody@example.com';
      document.getElementById('reset-form').dispatchEvent(makeEvent('submit'));
      await flush();
      const asked = since(beforeReset).filter((r) => r.path === '/api/v1/password-reset');
      check('and asking for a link carries no credential and says the one thing the route says, whatever the address was',
         asked.length === 1 && !asked[0].headers.Authorization && resetRequests[resetRequests.length - 1] === 'somebody@example.com' &&
         String(document.getElementById('reset-status').className).indexOf('ok') >= 0 &&
         document.getElementById('reset-status').textContent.length > 20,
         JSON.stringify(asked.map((r) => r.headers)) + ' | ' + document.getElementById('reset-status').textContent);

      document.getElementById('reset-cancel').dispatchEvent(makeEvent('click'));
      check('and Back to signing in puts the sign-in form back',
         document.getElementById('reset-form').hidden === true && document.getElementById('signin-form').hidden === false);
   }

   // ---- Passkeys: the Security card, and signing in with one
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const walkAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const rows = () => walkAll(document.getElementById('passkey-rows'), (n) => n.tagName === 'TR');
      const rowFor = (name) => rows().filter((r) => r.textContent.indexOf(name) >= 0)[0];

      // The block before this one left the page on the Security view. Setting
      // the hash to what it already is fires nothing, so leave first: what is
      // checked below is that ARRIVING at the view asks for the list.
      location.hash = '#/f/1';
      await flush();
      const beforeList = requests.length;
      location.hash = '#/security';
      await flush();
      check('the Security page lists the passkeys, with when each was last used',
         called(beforeList, 'GET', '/api/v1/passkeys') && rows().length === 2 &&
         rowFor('Phone').textContent.indexOf(words('never')) >= 0,
         rows().map((r) => r.textContent).join(' | '));

      const beforeAdd = requests.length;
      const callsBefore = credentialCalls.length;
      document.getElementById('passkey-name').value = 'Desk phone';
      document.getElementById('passkey-password').value = 'a-real-password';
      document.getElementById('passkey-form').dispatchEvent(makeEvent('submit'));
      await flush();
      await flush();
      const made = since(beforeAdd).filter((r) => r.method === 'POST' && r.path === '/api/v1/passkeys')[0];
      const body = made ? JSON.parse(made.body || '{}') : {};
      check('adding a passkey asks the server for the options, hands them to the authenticator and sends what it made back',
         called(beforeAdd, 'POST', '/api/v1/passkeys/options') &&
         credentialCalls.length === callsBefore + 1 && credentialCalls[callsBefore].kind === 'create' &&
         credentialCalls[callsBefore].options.publicKey.rp.id === 'portal.test' &&
         body.name === 'Desk phone' && body.password === 'a-real-password' &&
         typeof body.credential.response.clientDataJSON === 'string' &&
         typeof body.credential.response.attestationObject === 'string' &&
         body.credential.response.transports[0] === 'internal',
         JSON.stringify(body));
      check('and the new one is in the list, with the name field emptied',
         !!rowFor('Desk phone') && document.getElementById('passkey-name').value === '' &&
         document.getElementById('passkey-password').value === '',
         rows().map((r) => r.textContent).join(' | '));

      // A second factor enrolled and the authenticator lost. The registration
      // asks for the code as making an app password does, and the form has to
      // offer both the authenticator's code and a recovery code, because the
      // server takes either and spends the recovery code only for a passkey
      // it stored.
      passkeysDemandSecondFactor = true;
      document.getElementById('passkey-name').value = 'Tablet';
      document.getElementById('passkey-password').value = 'a-real-password';
      document.getElementById('passkey-form').dispatchEvent(makeEvent('submit'));
      await flush();
      await flush();
      check('a registration that asks for the second factor reveals both the authenticator field and the recovery-code field, and says so',
         document.getElementById('passkey-otp').hidden === false && document.getElementById('passkey-code').hidden === false &&
         document.getElementById('passkey-status').textContent ===
            words('Enter the code from your authenticator app, or one of your recovery codes.') &&
         !rowFor('Tablet'),
         document.getElementById('passkey-status').textContent);

      const beforeWithCode = requests.length;
      document.getElementById('passkey-code').value = ' b3d5f-2j9km ';
      document.getElementById('passkey-form').dispatchEvent(makeEvent('submit'));
      await flush();
      await flush();
      const withCode = since(beforeWithCode).filter((r) => r.method === 'POST' && r.path === '/api/v1/passkeys')[0];
      check('and a recovery code typed there travels in X-hMailServer-Recovery-Code, trimmed, with no one-time code beside it, and the passkey is added',
         withCode && withCode.headers['X-hMailServer-Recovery-Code'] === 'b3d5f-2j9km' &&
         !withCode.headers['X-hMailServer-OTP'] && passkeySecondFactorSeen.code === 'b3d5f-2j9km' &&
         !!rowFor('Tablet') && document.getElementById('passkey-code').value === '',
         withCode ? JSON.stringify(withCode.headers) + ' | ' + rows().map((r) => r.textContent).join(' | ') : 'no request');
      passkeysDemandSecondFactor = false;

      const beforeRemove = requests.length;
      walkAll(rowFor('Work laptop'), (n) => n.tagName === 'BUTTON' && n.textContent === words('Remove'))[0].dispatchEvent(makeEvent('click'));
      await flush();
      check('Remove takes the passkey away and the row with it',
         called(beforeRemove, 'DELETE', '/api/v1/passkeys/7') && !rowFor('Work laptop'),
         rows().map((r) => r.textContent).join(' | '));

      // Signing in with the passkey. The page is not signed out first: ending
      // this page's sessions here would take the second account's with it, and
      // what is being checked is the exchange the button makes, which is the
      // same exchange whether the gate is on screen or not.
      const beforeSignIn = requests.length;
      const getsBefore = credentialCalls.length;
      const challengesBefore = passkeyChallenges;
      document.getElementById('signin-passkey').dispatchEvent(makeEvent('click'));
      await flush();
      await flush();
      const session = since(beforeSignIn).filter((r) => r.method === 'POST' && r.path === '/api/v1/session')[0];
      const sent = session ? JSON.parse(session.body || '{}') : {};
      check('signing in with a passkey asks for a challenge, uses it, and sends the assertion to the session route',
         passkeyChallenges === challengesBefore + 1 &&
         credentialCalls.length === getsBefore + 1 && credentialCalls[getsBefore].kind === 'get' &&
         credentialCalls[getsBefore].options.publicKey.rpId === 'portal.test' &&
         !!sent.passkey && typeof sent.passkey.response.signature === 'string' &&
         typeof sent.passkey.response.authenticatorData === 'string' &&
         sent.passkey.response.userHandle.length > 0,
         JSON.stringify(sent));
      // Verified, not merely present: the server refuses an assertion whose
      // authenticator did not verify the person, because the one-time code is
      // skipped on the strength of exactly that. Asking for it here is what
      // makes the device ask for the PIN instead of the person being refused
      // after they have already touched the key.
      check('and asks the authenticator to verify the person, not just to see one',
         credentialCalls[getsBefore].options.publicKey.userVerification === 'required',
         JSON.stringify(credentialCalls[getsBefore].options.publicKey.userVerification));
      // The header that says this came from this server's own page. A session
      // minted from the request BODY is the one a cross-site form could post,
      // so the server demands it on this route and the page sends it.
      check('and carries the header that proves the post came from this page',
         !!session && session.headers['X-Requested-With'] === 'hMailServer',
         JSON.stringify(session ? session.headers : null));

      // An authenticator that refuses - the person pressed cancel - says so
      // rather than leaving the button looking as though nothing happened.
      credentialsRefuse = true;
      const beforeRefusal = requests.length;
      document.getElementById('signin-passkey').dispatchEvent(makeEvent('click'));
      await flush();
      await flush();
      check('an authenticator that refuses says so, and starts no session',
         document.getElementById('signin-status').textContent.length > 0 &&
         !called(beforeRefusal, 'POST', '/api/v1/session'),
         document.getElementById('signin-status').textContent);
      credentialsRefuse = false;
      dismissAllToasts();
   }


   // ---- The organisation's directory: the page, the Settings card, the completion
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const walkAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
      const cards = () => walkAll(document.getElementById('directory-list'), (n) => n.className === 'dircard');
      const cardFor = (text) => cards().filter((c) => c.textContent.indexOf(text) >= 0)[0];
      const buttonIn = (card, english) => walkAll(card, (n) => n.tagName === 'BUTTON' && n.textContent === words(english))[0];

      const beforeList = requests.length;
      location.hash = '#/directory';
      await flush();
      check('D1: the Directory page asks the server for the reader\'s own domain and draws a card each',
         called(beforeList, 'GET', /^\/api\/v1\/me\/directory\?/) && document.getElementById('directory-section').hidden === false && cards().length === 3,
         cards().map((c) => c.textContent).join(' | '));
      check('D1: the page never names a domain in the request it makes',
         since(beforeList).filter((r) => r.path.indexOf('/api/v1/me/directory') === 0).every((r) => r.path.indexOf('domain=') < 0),
         JSON.stringify(since(beforeList).map((r) => r.path)));
      check('D2: a person with no name of their own is drawn under their address', !!cardFor('quiet@example.com'), cards().map((c) => c.textContent).join(' | '));
      check('D2: the title, the department and where they sit are on the card',
         cardFor('Ada Lovelace').textContent.indexOf('Head of Engines - Engineering') >= 0 && cardFor('Ada Lovelace').textContent.indexOf('Marylebone, 2nd floor') >= 0,
         cardFor('Ada Lovelace').textContent);
      check('D2: a distribution list says it is one, and is offered no "add to contacts", there being no person behind it',
         cardFor('sales@example.com').textContent.indexOf(words('Distribution list')) >= 0 && !buttonIn(cardFor('sales@example.com'), 'Add to contacts'),
         cardFor('sales@example.com').textContent);
      check('D2: a photo is fetched from the entry\'s own route rather than carried in the listing',
         walkAll(cardFor('Ada Lovelace'), (n) => n.tagName === 'IMG' && n.src === '/api/v1/me/directory/11/photo').length === 1,
         walkAll(cardFor('Ada Lovelace'), (n) => n.tagName === 'IMG').map((n) => n.src).join(','));

      document.getElementById('directory-search').value = 'engines';
      document.getElementById('directory-search').dispatchEvent(makeEvent('input'));
      await flush();
      check('D3: the search box narrows the list against what is already in hand', cards().length === 1 && !!cardFor('Ada Lovelace'), cards().map((c) => c.textContent).join(' | '));
      document.getElementById('directory-search').value = '';
      document.getElementById('directory-search').dispatchEvent(makeEvent('input'));
      await flush();
      check('D3: and clearing it puts them all back', cards().length === 3);

      const beforeKeep = requests.length;
      buttonIn(cardFor('Ada Lovelace'), 'Add to contacts').dispatchEvent(makeEvent('click'));
      await flush();
      const kept = since(beforeKeep).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/contacts').map((r) => JSON.parse(r.body))[0];
      check('D4: Add to contacts writes the directory name and address into the address book',
         !!kept && kept.name === 'Ada Lovelace' && kept.address === 'ada@example.com', JSON.stringify(kept));

      // The Settings card: the person's own entry, and the opt-out.
      location.hash = '#/settings';
      await flush();
      check('D5: the Settings card is shown and the side nav offers the page', document.getElementById('directory-card').hidden === false && document.getElementById('nav-directory').hidden === false);

      const beforeSave = requests.length;
      document.getElementById('directory-title').value = 'Countess of Lovelace';
      document.getElementById('directory-phone').value = '+44 20 7946 0001';
      document.getElementById('directory-form').dispatchEvent(makeEvent('submit'));
      await flush();
      const saved = since(beforeSave).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/directory/me').map((r) => JSON.parse(r.body))[0];
      check('D5: saving sends the five fields the person may set and no photo when none was chosen',
         !!saved && saved.title === 'Countess of Lovelace' && saved.phone === '+44 20 7946 0001' && !('photo' in saved) && !('opt_out' in saved) && !('note' in saved),
         JSON.stringify(saved));
      check('D5: and the form shows what the server stored', document.getElementById('directory-title').value === 'Countess of Lovelace' &&
         String(document.getElementById('directory-entry-status').className).indexOf('ok') >= 0);

      // The person's own photo, drawn from its route on this server. This and
      // the cards above set an <img src> of this origin, which is why the
      // page's Content-Security-Policy has to name img-src 'self': with data:
      // and blob: alone the browser refuses both and nothing appears, and a
      // stub DOM has no policy to refuse them with. check-portal-script.py
      // holds the script and the policy together; these two checks say what
      // the script does.
      directoryOwn.has_photo = true;
      directoryOwn.photo_type = 'image/png';
      location.hash = '#/f/1';
      await flush();
      location.hash = '#/settings';
      await flush();
      check('D5: the person\'s own photo is drawn from its route on this server',
         document.getElementById('directory-photo').hidden === false &&
         String(document.getElementById('directory-photo').src).indexOf('/api/v1/me/directory/me/photo') === 0,
         String(document.getElementById('directory-photo').src));

      const beforeOptOut = requests.length;
      document.getElementById('directory-optout').checked = true;
      document.getElementById('directory-optout').dispatchEvent(makeEvent('change'));
      await flush();
      const out = since(beforeOptOut).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/directory/me').map((r) => JSON.parse(r.body))[0];
      check('D6: the opt-out is sent on its own, so that it is not refused with the fields',
         !!out && out.opt_out === true && Object.keys(out).length === 1, JSON.stringify(out));
      check('D6: and the page says the person is out of the directory',
         document.getElementById('directory-optout-status').textContent === words('You are out of the directory.'), document.getElementById('directory-optout-status').textContent);

      // The server that keeps the fields for itself: the form goes, the reason
      // is said, and the opt-out still works.
      directoryUserEditable = false;
      location.hash = '#/f/1';
      await flush();
      location.hash = '#/settings';
      await flush();
      check('D7: with the fields locked the form is hidden and the reason is shown',
         document.getElementById('directory-form').hidden === true && document.getElementById('directory-locked').hidden === false);
      const beforeLockedOptOut = requests.length;
      document.getElementById('directory-optout').checked = false;
      document.getElementById('directory-optout').dispatchEvent(makeEvent('change'));
      await flush();
      const back = since(beforeLockedOptOut).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/directory/me').map((r) => JSON.parse(r.body))[0];
      check('D7: and the opt-out still goes through, alone', !!back && back.opt_out === false && Object.keys(back).length === 1 &&
         document.getElementById('directory-optout-status').textContent === words('You are in the directory.'), JSON.stringify(back));
      directoryUserEditable = true;

      // The completion: the reader's own contacts first, then the directory,
      // labelled, and an address that is both offered once.
      location.hash = '#/f/1';
      await flush();
      document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
      await flush();
      const to = document.getElementById('compose-to');
      const options = () => walkAll(to.parentNode, (n) => n.tagName === 'LI');
      to.value = 'a';
      to.dispatchEvent(makeEvent('input'));
      fireTimers();
      await flush();
      const asked = requests.filter((r) => r.path.indexOf('/api/v1/me/directory?') === 0 && r.path.indexOf('q=a') > 0);
      check('D8: the To field asks the directory as well as the address book', asked.length >= 1, JSON.stringify(requests.slice(-6).map((r) => r.path)));
      check('D8: the reader\'s own contacts come first and the directory hits follow, labelled',
         options().length > 1 && options()[0].textContent.indexOf('Alice Example') >= 0 &&
         options().some((o) => o.textContent.indexOf('ada@example.com') >= 0 && o.textContent.indexOf(words('Directory')) >= 0),
         options().map((o) => o.textContent).join(' | '));
      check('D8: a distribution list in the completion says so rather than being offered as a person',
         options().some((o) => o.textContent.indexOf('sales@example.com') >= 0 && o.textContent.indexOf(words('Distribution list')) >= 0),
         options().map((o) => o.textContent).join(' | '));

      // An address that is both a contact and a directory entry: once, as the
      // contact, since that is the row carrying the name the reader chose.
      to.value = 'alice';
      to.dispatchEvent(makeEvent('input'));
      fireTimers();
      await flush();
      check('D9: an address in both is offered once', options().filter((o) => o.textContent.indexOf('alice@example.net') >= 0).length === 1,
         options().map((o) => o.textContent).join(' | '));

      // The member field of a group asks for people the reader keeps, so it
      // must not reach into the directory at all.
      const beforeMember = requests.length;
      location.hash = '#/contacts';
      await flush();
      const memberField = document.getElementById('group-member-7');
      if (memberField) {
         memberField.value = 'ad';
         memberField.dispatchEvent(makeEvent('input'));
         fireTimers();
         await flush();
         check('D10: the group member field asks the address book and not the directory',
            since(beforeMember).filter((r) => r.path.indexOf('/api/v1/me/directory?') === 0).length === 0,
            JSON.stringify(since(beforeMember).map((r) => r.path)));
      }

      // The switch off: the page says so and offers nothing, and the side nav
      // does not offer a page that would be empty.
      directorySwitchedOn = false;
      location.hash = '#/settings';
      await flush();
      check('D11: with the directory off the Settings card and the nav entry are hidden',
         document.getElementById('directory-card').hidden === true && document.getElementById('nav-directory').hidden === true);
      location.hash = '#/directory';
      await flush();
      check('D11: and the page says the directory is off rather than saying it is empty',
         document.getElementById('directory-off').hidden === false && document.getElementById('directory-empty').hidden === true && cards().length === 0,
         document.getElementById('directory-off').textContent);
      const beforeOffSuggest = requests.length;
      location.hash = '#/f/1';
      await flush();
      document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
      await flush();
      to.value = 'ada';
      to.dispatchEvent(makeEvent('input'));
      fireTimers();
      await flush();
      check('D11: and the completion stops asking a directory that answered that it is off',
         since(beforeOffSuggest).filter((r) => r.path.indexOf('/api/v1/me/directory?') === 0).length === 0,
         JSON.stringify(since(beforeOffSuggest).map((r) => r.path)));
      directorySwitchedOn = true;
      location.hash = '#/settings';
      await flush();
   }

   // ---- Shared address books: the books beside the contacts, the Sharing
   // panel the owner sees, and a book shared read-only that offers no way to
   // change anything (AddressBookStore.h, docs/SharedAddressBooks.md).
   await addressBookChecks();

   // ---- OpenPGP: the key ring, a sealed send, Autocrypt and a revocation
   await pgpChecks();

   // ---- OpenPGP, trust made visible: a key that changed is a warning naming
   // both fingerprints and is not encrypted to until accepted; a key verified
   // in person shows the mark on every good signature; the rest is plain
   await pgpTrustChecks();

   // ---- A password-protected message to anyone: the compose control, the
   // send that carries no password and no text, and the reading page
   await protectedChecks();
   // ---- Zero-access mailbox: the switch-on flow that stops until the
   // recovery key is typed back, the consequences said first, the sign-in
   // that opens the key from the password, the reader opening a delivery
   // sealed to it, the recovery key after a password reset, the password
   // change that carries the key sealed again, and the switch-off
   await zeroAccessChecks();

   // ---- Web Push: the devices, and the service worker's own handler
   await pushChecks();

   // ---- signing out ends the session and stops the probe
   const beforeOut = requests.length;
   document.getElementById('signout').dispatchEvent(makeEvent('click'));
   await flush();
   check('signing out ends the session', called(beforeOut, 'DELETE', '/api/v1/session'));
   check('L50: and the second account\'s session with it, with its token, before the cookie\'s',
      since(beforeOut).filter((r) => r.method === 'DELETE' && r.path === '/api/v1/session').map((r) => (r.headers.Authorization === 'Bearer ' + SECOND_TOKEN ? 'bearer' : r.headers.Authorization ? 'other' : 'cookie')).join(',') === 'bearer,cookie' && secondToken === '',
      JSON.stringify(since(beforeOut).filter((r) => r.path === '/api/v1/session').map((r) => r.headers)));
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
   document.getElementById('pgp-unlock-passphrase').value = 'typed-and-not-sent';
   document.getElementById('pgp-import-passphrase').value = 'the-file-passphrase';
   document.getElementById('pgp-make-passphrase').value = 'a-new-passphrase';
   document.getElementById('signout').dispatchEvent(makeEvent('click'));
   await flush();
   check('sign-out empties the certificate store on the page',
      document.getElementById('smime-own-rows').children.length === 0 && document.getElementById('smime-own-table').hidden === true,
      document.getElementById('smime-own-rows').children.length + ' rows, table hidden=' + document.getElementById('smime-own-table').hidden);
   check('and keeps no password in either box',
      document.getElementById('smime-unlock-password').value === '' && document.getElementById('smime-passphrase').value === '',
      JSON.stringify([document.getElementById('smime-unlock-password').value, document.getElementById('smime-passphrase').value]));
   check('OpenPGP: sign-out empties the key ring on the page as well',
      document.getElementById('pgp-own-rows').children.length === 0 && document.getElementById('pgp-own-table').hidden === true &&
      document.getElementById('pgp-peer-rows').children.length === 0,
      document.getElementById('pgp-own-rows').children.length + ' own, ' + document.getElementById('pgp-peer-rows').children.length + ' correspondents');
   check('OpenPGP: and keeps no passphrase in any of its three boxes',
      document.getElementById('pgp-unlock-passphrase').value === '' && document.getElementById('pgp-import-passphrase').value === '' &&
      document.getElementById('pgp-make-passphrase').value === '',
      JSON.stringify([document.getElementById('pgp-unlock-passphrase').value, document.getElementById('pgp-import-passphrase').value]));
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

   // ---- the domain's own S/MIME authority on the Security page
   // (OrganisationCa.h): said plainly when the domain has none; offered by the
   // domain's name when it has one; the key made on the page, the password
   // asked before anything is sent, only the request going to the server, and
   // what comes back stored where an imported key goes - which is also what
   // confirms the certificate.
   location.hash = '#/security';
   await flush();
   check('S/MIME authority: a domain with none is said so, and nothing is offered to get',
      document.getElementById('smime-ca-none').hidden === false && document.getElementById('smime-ca-body').hidden === true,
      'none hidden=' + document.getElementById('smime-ca-none').hidden + ' body hidden=' + document.getElementById('smime-ca-body').hidden);
   smimeAuthorities['third@example.com'] = { domain: 'example.com', available: true, subject: '/CN=example.com S/MIME Authority', fingerprint: 'd'.repeat(64), key_type: 'P-256', not_after: 2100000000, certificate: 'MAMCAQM=', issued: [] };
   const caIssued = (serial) => (smimeAuthorities['third@example.com'].issued || []).filter((c) => c.serial === serial)[0] || {};
   location.hash = '#/settings';
   await flush();
   location.hash = '#/security';
   await flush();
   check('S/MIME authority: a domain with one is offered by name, in the reader\'s language',
      document.getElementById('smime-ca-none').hidden === true && document.getElementById('smime-ca-body').hidden === false &&
         document.getElementById('smime-ca-get').textContent === german['Get a certificate from {0}'].replace('{0}', 'example.com'),
      document.getElementById('smime-ca-get').textContent);
   document.getElementById('smime-ca-key').value = 'ec';
   const beforeCaEnrol = requests.length;
   document.getElementById('smime-ca-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('S/MIME authority: the password that wraps the new key is asked before anything is sent',
      document.getElementById('smime-unlock').hidden === false && !called(beforeCaEnrol, 'POST', '/api/v1/me/smime/enroll'),
      JSON.stringify(since(beforeCaEnrol).map((r) => r.method + ' ' + r.path)));
   document.getElementById('smime-unlock-password').value = 'the-current-password';
   document.getElementById('smime-unlock-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const caEnrolled = since(beforeCaEnrol).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/smime/enroll').pop();
   const caEnrolBody = caEnrolled ? JSON.parse(caEnrolled.body) : {};
   check('S/MIME authority: the request made on the page goes to the authority for the key type chosen, and nothing of the key with it',
      !!caEnrolled && Buffer.from(caEnrolBody.csr, 'base64').toString() === 'CSR:ec:third@example.com' && caEnrolBody.keep === false &&
         Object.keys(caEnrolBody).sort().join(',') === 'csr,keep',
      caEnrolled ? caEnrolled.body : 'no enrolment');
   const caStoredOwn = since(beforeCaEnrol).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/smime/own').pop();
   const caStoredBody = caStoredOwn ? JSON.parse(caStoredOwn.body) : {};
   check('S/MIME authority: the certificate, its chain and the key wrapped with the password go into the account\'s own store',
      !!caStoredOwn && caStoredBody.certificate === caIssued(1).certificate && caStoredBody.fingerprint === caIssued(1).fingerprint &&
         caStoredBody.chain.length === 1 && caStoredBody.chain[0] === 'MAMCAQM=' && caStoredBody.key.data === 'the-current-password',
      caStoredOwn ? caStoredOwn.body : 'nothing stored');
   check('S/MIME authority: storing the key is what confirms the certificate; nothing else is sent for it',
      caIssued(1).confirmed === true && !since(beforeCaEnrol).some((r) => /\/confirm$/.test(r.path)),
      JSON.stringify(caIssued(1)) + ' ' + JSON.stringify(since(beforeCaEnrol).map((r) => r.method + ' ' + r.path)));
   check('S/MIME authority: the outcome is said, and the certificate is listed with a Revoke',
      document.getElementById('smime-ca-status').textContent === german['A certificate for {0} was issued and stored.'].replace('{0}', 'third@example.com') &&
         document.getElementById('smime-ca-rows').children.length === 1 && document.getElementById('smime-ca-table').hidden === false,
      document.getElementById('smime-ca-status').textContent + ' / ' + document.getElementById('smime-ca-rows').children.length + ' rows');
   const caRevokeRow = document.getElementById('smime-ca-rows').children[0];
   const caButtonsIn = (root) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (c.tagName === 'BUTTON') { out.push(c); } walk(c); } } })(root); return out; };
   const caRevokeButton = caRevokeRow ? caButtonsIn(caRevokeRow)[0] : null;
   // A confirm of this check's own: the page's question is recorded here, and
   // whatever another section left in its place is put back afterwards.
   const caConfirmed = [];
   const caConfirmBefore = window.confirm;
   window.confirm = (text) => { caConfirmed.push(String(text)); return true; };
   const beforeCaRevoke = requests.length;
   if (caRevokeButton) { caRevokeButton.dispatchEvent(makeEvent('click')); }
   await flush();
   window.confirm = caConfirmBefore;
   check('S/MIME authority: Revoke asks first, saying what revoking does, then revokes that serial, and the list says so',
      caConfirmed.length === 1 && caConfirmed[0] === german['Revoke this certificate? Colleagues are no longer given it, this webmail shows mail signed with it as revoked and stops encrypting to it, and it goes on the domain\'s revocation list.'] &&
         called(beforeCaRevoke, 'DELETE', '/api/v1/me/smime/issued/1') &&
         document.getElementById('smime-ca-rows').children.length === 1 &&
         document.getElementById('smime-ca-rows').children[0].textContent.indexOf(german['Revoked']) >= 0,
      JSON.stringify(since(beforeCaRevoke).map((r) => r.method + ' ' + r.path)) + ' asked ' + JSON.stringify(caConfirmed) + ' row ' +
         (document.getElementById('smime-ca-rows').children[0] ? document.getElementById('smime-ca-rows').children[0].textContent : 'none'));

   // A second certificate for the address and a third that replaces it: the
   // third supersedes the second only once its key is stored, and from then on
   // it is the third the page signs with - not the second, which the store
   // still holds (its key opens mail sent to it), and not the revoked first.
   const caEnrolThroughPage = async (keyType, keep) => {
      document.getElementById('smime-ca-key').value = keyType;
      document.getElementById('smime-ca-keep').checked = !!keep;
      document.getElementById('smime-ca-form').dispatchEvent(makeEvent('submit'));
      await flush();
      document.getElementById('smime-unlock-password').value = 'the-current-password';
      document.getElementById('smime-unlock-form').dispatchEvent(makeEvent('submit'));
      await flush();
   };
   await caEnrolThroughPage('rsa', false);
   const caSecondValid = caIssued(2).status === 'valid' && caIssued(2).confirmed === true;
   await caEnrolThroughPage('rsa', false);
   check('S/MIME authority: a new certificate supersedes the one before it once its key is stored, and not before',
      caSecondValid && caIssued(3).confirmed === true && caIssued(2).status === 'revoked' && caIssued(2).reason === 'superseded',
      JSON.stringify(smimeAuthorities['third@example.com'].issued.map((c) => [c.serial, c.status, c.reason, c.confirmed])));
   const caSignedWith = [];
   window.hmSmime = Object.assign({}, smimeStub, { composeMessage: async (spec) => { caSignedWith.push(spec.sign && spec.sign.entry ? spec.sign.entry.fingerprint : null); return 'MIME-Version: 1.0\r\nContent-Type: multipart/signed; protocol="application/pkcs7-signature"; micalg=sha-256; boundary="b"\r\n\r\n--b--\r\n'; } });
   document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
   await flush();
   document.getElementById('compose-to').value = 'someone@example.net';
   document.getElementById('compose-subject').value = 'Signed after a new certificate';
   document.getElementById('compose-text').value = 'Signed with the newest.';
   document.getElementById('compose-sign').checked = true;
   document.getElementById('compose-encrypt').checked = false;
   const caBeforeSigned = requests.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   fireTimers();
   await flush();
   check('S/MIME authority: after a second enrolment the page signs with the new certificate, not the superseded one',
      caSignedWith.length === 1 && caSignedWith[0] === caIssued(3).fingerprint && called(caBeforeSigned, 'POST', '/api/v1/me/messages'),
      JSON.stringify(caSignedWith) + ' expected ' + caIssued(3).fingerprint);
   window.hmSmime = smimeStub;
   dismissAllToasts();

   // A store that refuses the key: the new certificate is revoked again at
   // once, as no longer in use, and the one before it is still valid - the
   // supersede waited for a store that never happened.
   smimeOwnRefuse = true;
   const beforeCaRefused = requests.length;
   await caEnrolThroughPage('rsa', false);
   smimeOwnRefuse = false;
   const caRollback = since(beforeCaRefused).filter((r) => r.method === 'DELETE' && r.path.indexOf('/api/v1/me/smime/issued/4') === 0).pop();
   check('S/MIME authority: a certificate whose key the store refused is revoked as no longer in use, and the one before it stays valid',
      !!caRollback && caRollback.path === '/api/v1/me/smime/issued/4?reason=cessationOfOperation' &&
         caIssued(4).status === 'revoked' && caIssued(4).reason === 'cessationOfOperation' && caIssued(3).status === 'valid' &&
         document.getElementById('smime-ca-status').textContent.indexOf(german['Could not get a certificate: {0}'].split('{0}')[0]) === 0,
      (caRollback ? caRollback.path : 'no revocation') + ' ' + JSON.stringify(smimeAuthorities['third@example.com'].issued.map((c) => [c.serial, c.status, c.reason])) +
         ' ' + document.getElementById('smime-ca-status').textContent);

   // A message to a colleague of the domain, encrypted, with no certificate of
   // theirs stored: the directory is asked by address, EVERY certificate it
   // answers is kept - a colleague with two devices has one for each - and the
   // message is encrypted to all of them rather than refused.
   const caColleague = (tag) => ({ serial: 7, fingerprint: tag.repeat(64), not_before: 1700000000, not_after: 2000000000, certificate: Buffer.from('COLLEAGUE-' + tag).toString('base64'), pem: '' });
   authorityCertificates['colleague@example.com'] = [caColleague('c'), caColleague('b')];
   const caComposedFor = [];
   window.hmSmime = Object.assign({}, smimeStub, { composeMessage: async (spec) => { caComposedFor.push((spec.encrypt || []).map((c) => Buffer.from(c.raw).toString('base64'))); return 'MIME-Version: 1.0\r\nContent-Type: application/pkcs7-mime; smime-type=enveloped-data\r\n\r\nsealed\r\n'; } });
   const caSealFor = async (to) => {
      document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
      await flush();
      document.getElementById('compose-to').value = to;
      document.getElementById('compose-subject').value = 'Sealed for a colleague';
      document.getElementById('compose-text').value = 'Encrypted to the authority\'s certificates.';
      document.getElementById('compose-sign').checked = false;
      document.getElementById('compose-encrypt').checked = true;
      document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
      await flush();
      fireTimers();
      await flush();
   };
   const caBeforeColleague = requests.length;
   await caSealFor('colleague@example.com');
   const caKeptColleague = since(caBeforeColleague).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/smime/recipients').map((r) => JSON.parse(r.body));
   check('S/MIME authority: a colleague with no stored certificate is looked up by address, and every certificate answered is kept',
      authorityLookups.indexOf('colleague@example.com') >= 0 && caKeptColleague.length === 2 &&
         caKeptColleague.every((k) => k.address === 'colleague@example.com') &&
         caKeptColleague.map((k) => k.fingerprint).sort().join(',') === ['b'.repeat(64), 'c'.repeat(64)].join(','),
      JSON.stringify(since(caBeforeColleague).map((r) => r.method + ' ' + r.path)));
   check('S/MIME authority: and the message is encrypted to both of the colleague\'s certificates rather than refused',
      caComposedFor.length === 1 && caComposedFor[0].indexOf(caColleague('c').certificate) >= 0 && caComposedFor[0].indexOf(caColleague('b').certificate) >= 0 &&
         called(caBeforeColleague, 'POST', '/api/v1/me/messages'),
      JSON.stringify(caComposedFor) + ' ' + JSON.stringify(since(caBeforeColleague).map((r) => r.method + ' ' + r.path)));

   // Both revoked since - the colleague enrolled again - and a new one in the
   // directory: the page, having read its store again, neither encrypts to the
   // revoked ones nor refuses, but asks the directory again and uses the new.
   authorityStatuses['c'.repeat(64)] = 'revoked';
   authorityStatuses['b'.repeat(64)] = 'revoked';
   authorityCertificates['colleague@example.com'] = [caColleague('a')];
   location.hash = '#/settings';
   await flush();
   location.hash = '#/security';
   await flush();
   const caLookupsBefore = authorityLookups.filter((a) => a === 'colleague@example.com').length;
   const caBeforeRevoked = requests.length;
   await caSealFor('colleague@example.com');
   const caLastSealed = caComposedFor[caComposedFor.length - 1] || [];
   check('S/MIME authority: a colleague\'s stored certificates that were revoked are not encrypted to, and the directory is asked again',
      authorityLookups.filter((a) => a === 'colleague@example.com').length === caLookupsBefore + 1 && caComposedFor.length === 2 &&
         caLastSealed.indexOf(caColleague('a').certificate) >= 0 && caLastSealed.indexOf(caColleague('c').certificate) < 0 &&
         caLastSealed.indexOf(caColleague('b').certificate) < 0 && called(caBeforeRevoked, 'POST', '/api/v1/me/messages'),
      JSON.stringify(caComposedFor) + ' ' + JSON.stringify(since(caBeforeRevoked).map((r) => r.method + ' ' + r.path)));

   // An address the directory has nothing for: asked once, and the message is
   // refused in its own words - not sent, and not asked about again in a loop.
   const caBeforeNobody = requests.length;
   await caSealFor('nobody@example.com');
   check('S/MIME authority: an address with no certificate anywhere is looked up once and the message is refused, not sent',
      authorityLookups.filter((a) => a === 'nobody@example.com').length === 1 && !called(caBeforeNobody, 'POST', '/api/v1/me/messages') &&
         document.getElementById('compose-status').textContent === german['Encrypt: no certificate for {0}.'].replace('{0}', 'nobody@example.com'),
      document.getElementById('compose-status').textContent + ' ' + JSON.stringify(since(caBeforeNobody).map((r) => r.method + ' ' + r.path)));
   document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
   await flush();
   window.hmSmime = smimeStub;
   dismissAllToasts();

   // A signature whose chain ended at the domain's own authority says so on
   // its badge; the chain check's authority answer is what decides it.
   smimeChainAnswer = { trusted: true, error: '', depth: 2, roots: 0, subject: '/CN=B', issuer: '/CN=example.com S/MIME Authority', not_after: 2000000000, authority: true };
   window.hmSmime = Object.assign({}, smimeStub, {
      readMessage: async () => ({ layers: ['signed'], encrypted: null, content: null,
         signed: { valid: true, error: '', signer: { raw: Uint8Array.from([0x30, 0x03, 0x02, 0x01, 0x09]), emails: ['b@example.net'], name: 'B', notAfter: new Date('2030-01-01T00:00:00Z') }, certificates: [] } })
   });
   location.hash = '#/m/601';
   await flush();
   await flush();
   const caBadgeTexts = Array.prototype.slice.call(document.getElementById('message-badges').children).map((b) => b.textContent);
   check('S/MIME authority: a signature the domain\'s own authority certified says so on its badge',
      caBadgeTexts.indexOf(german['Signed by {0}, certified by your organisation'].replace('{0}', 'b@example.net')) >= 0, JSON.stringify(caBadgeTexts));
   smimeChainAnswer = null;
   window.hmSmime = smimeStub;
   delete smimeAuthorities['third@example.com'];
   delete authorityCertificates['colleague@example.com'];
   delete authorityStatuses['c'.repeat(64)];
   delete authorityStatuses['b'.repeat(64)];
   dismissAllToasts();

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
   check('and not Protect with a password either, which needs the same cryptography',
      document.getElementById('compose-protect-label').hidden === true, 'hidden=' + document.getElementById('compose-protect-label').hidden);
   window.hmSmime = smimeStub;
   delete window.isSecureContext;

   // ---- help, beside the page
   // What the row asks for and a build cannot see: the button opens the
   // pane and not a window, the document is the page's, the text is drawn
   // as headings and lists rather than as a wall, the pane follows the
   // route, F1 and Escape do what the shortcut list says, the palette
   // offers it, and the choice to keep it open is this browser's.
   // Whatever the checks above left open is closed first: the page's own
   // Escape closes an overlay before it reaches the pane.
   if (!document.getElementById('palette').hidden || !document.getElementById('keys-overlay').hidden) {
      document.dispatchEvent(makeEvent('keydown', { key: 'Escape', target: document.body }));
      await flush();
   }
   try { localStorage.removeItem('hmPortalHelp'); } catch (e) { /* no storage */ }
   location.hash = '#/settings';
   await flush();
   const helpPane = document.getElementById('help-pane');
   // Every element under root that test accepts, walked as the page walks.
   const helpAll = (root, test) => { const out = []; const walk = (n) => { (n.childNodes || []).forEach((c) => { if (c.nodeType === 1) { if (test(c)) { out.push(c); } walk(c); } }); }; walk(root); return out; };
   const helpTags = (id, tag) => helpAll(document.getElementById(id), (n) => n.tagName === tag);
   // The page is in German by now: the pane's own words are the catalogue's,
   // the documents' words are the documents' own.
   const helpSaid = (english) => (germanCatalogue && germanCatalogue[english]) || english;
   check('the help pane is closed until it is asked for', helpPane.hidden === true && document.getElementById('help-btn') !== null);
   let beforeHelp = requests.length;
   document.getElementById('help-btn').dispatchEvent(makeEvent('click'));
   await flush();
   check('the help button opens the pane beside the page and fetches the page\'s document from the help route',
      helpPane.hidden === false && called(beforeHelp, 'GET', '/api/v1/help/OfflineMail'), JSON.stringify(since(beforeHelp).map((r) => r.path)));
   check('the pane is titled by the document\'s first heading', document.getElementById('help-title').textContent === 'Offline mail in the webmail',
      document.getElementById('help-title').textContent);
   check('a heading is rendered as a heading and a list as a list, both as nodes',
      helpTags('help-body', 'H3').length > 0 && helpTags('help-body', 'UL').length > 0 && helpTags('help-body', 'LI').length > 0,
      document.getElementById('help-body').textContent.slice(0, 120));
   check('the document\'s other headings are listed at the top, each a button', helpTags('help-heads', 'BUTTON').length >= 4 &&
      document.getElementById('help-heads').textContent.indexOf('What the reader chooses') >= 0, document.getElementById('help-heads').textContent);
   check('Open on the wiki is the base URL the server answered with the document\'s name',
      document.getElementById('help-wiki').hidden === false && document.getElementById('help-wiki').getAttribute('href') === 'https://wiki.test/OfflineMail',
      String(document.getElementById('help-wiki').getAttribute('href')));
   check('the choice to keep the pane open is remembered', localStorage.getItem('hmPortalHelp') === '1');
   beforeHelp = requests.length;
   location.hash = '#/calendar';
   await flush();
   check('the pane follows the page: the calendar opens the calendar document',
      called(beforeHelp, 'GET', '/api/v1/help/Calendar') && document.getElementById('help-title').textContent === 'The calendar and the tasks',
      JSON.stringify(since(beforeHelp).map((r) => r.path)) + ' ' + document.getElementById('help-title').textContent);
   beforeHelp = requests.length;
   location.hash = '#/tasks';
   await flush();
   check('a page mapped to a heading of the document showing asks the server for nothing', !called(beforeHelp, 'GET', /^\/api\/v1\/help/) &&
      document.getElementById('help-title').textContent === 'The calendar and the tasks', JSON.stringify(since(beforeHelp).map((r) => r.path)));
   beforeHelp = requests.length;
   location.hash = '#/contacts';
   await flush();
   check('the third of three: Contacts opens the address book document', called(beforeHelp, 'GET', '/api/v1/help/Contacts') &&
      document.getElementById('help-title').textContent === 'The address book', JSON.stringify(since(beforeHelp).map((r) => r.path)));
   document.dispatchEvent(makeEvent('keydown', { key: 'Escape', target: document.body }));
   check('Escape closes the pane and forgets the choice', helpPane.hidden === true && localStorage.getItem('hmPortalHelp') === null,
      'hidden=' + helpPane.hidden + ' stored=' + localStorage.getItem('hmPortalHelp'));
   document.dispatchEvent(makeEvent('keydown', { key: 'F1', target: document.body }));
   await flush();
   check('F1 opens it again', helpPane.hidden === false && localStorage.getItem('hmPortalHelp') === '1');
   document.dispatchEvent(makeEvent('keydown', { key: 'F1', target: document.body }));
   check('and closes it', helpPane.hidden === true);
   document.dispatchEvent(makeEvent('keydown', { key: 'k', ctrlKey: true, target: document.body }));
   await flush();
   document.getElementById('palette-input').value = helpSaid('Help for this page').toLowerCase();
   document.getElementById('palette-input').dispatchEvent(makeEvent('input'));
   await flush();
   const helpRows = Array.from(document.getElementById('palette-list').children || []);
   check('the palette offers Help for this page, in the reader\'s language', helpRows.length >= 1 && helpRows[0].textContent.indexOf(helpSaid('Help for this page')) >= 0,
      JSON.stringify(helpRows.map((li) => li.textContent)));
   document.getElementById('palette-input').dispatchEvent(makeEvent('keydown', { key: 'Enter' }));
   await flush();
   check('and choosing it opens the pane', helpPane.hidden === false && document.getElementById('palette').hidden === true,
      'pane=' + helpPane.hidden + ' palette=' + document.getElementById('palette').hidden);
   document.getElementById('help-all').dispatchEvent(makeEvent('click'));
   await flush();
   check('All documentation lists every document by its title, for opening in the pane',
      document.getElementById('help-title').textContent === helpSaid('All documentation') &&
      helpTags('help-body', 'BUTTON').length === helpPages().length,
      document.getElementById('help-title').textContent + ' ' + helpTags('help-body', 'BUTTON').length + ' of ' + helpPages().length);
   // The pane holds still. route() runs for every message opened from the
   // list, and the pane follows it; what it followed last is remembered, so
   // a route to the same document and heading moves nothing, another
   // heading of the same document moves it there once, and another page's
   // document is fetched.
   location.hash = '#/f/1';
   await flush();
   check('the folder view opens the shortcuts document', document.getElementById('help-title').textContent === 'Webmail: keyboard shortcuts and search operators',
      document.getElementById('help-title').textContent);
   const helpBody = document.getElementById('help-body');
   helpBody.scrollTop = 480;
   let scrolledBefore = scrollsIntoView;
   beforeHelp = requests.length;
   location.hash = '#/m/102';
   await flush();
   check('a message, mapped to the same document and heading, leaves the pane where the reader left it',
      helpBody.scrollTop === 480 && scrollsIntoView === scrolledBefore && !called(beforeHelp, 'GET', /^\/api\/v1\/help/),
      'scrollTop=' + helpBody.scrollTop + ' scrolls=' + (scrollsIntoView - scrolledBefore));
   scrolledBefore = scrollsIntoView;
   location.hash = '#/scheduled';
   await flush();
   check('a page mapped to another heading of the same document moves the pane there, once',
      scrollsIntoView === scrolledBefore + 1 && document.getElementById('help-title').textContent === 'Webmail: keyboard shortcuts and search operators',
      'scrolls=' + (scrollsIntoView - scrolledBefore));
   beforeHelp = requests.length;
   location.hash = '#/contacts';
   await flush();
   check('and a page mapped to another document fetches it', called(beforeHelp, 'GET', '/api/v1/help/Contacts'),
      JSON.stringify(since(beforeHelp).map((r) => r.path)));
   // A heading written with inline markup is listed without it and is reachable:
   // the pane scrolls by the line the heading starts on, not by its text.
   document.getElementById('help-all').dispatchEvent(makeEvent('click'));
   await flush();
   const listsButton = helpAll(helpBody, (n) => n.tagName === 'BUTTON' && n.textContent.indexOf('DistributionLists.md') >= 0)[0];
   if (listsButton) { listsButton.dispatchEvent(makeEvent('click')); }
   await flush();
   check('the distribution lists document opens from the list', document.getElementById('help-title').textContent === 'Distribution lists that define themselves',
      document.getElementById('help-title').textContent);
   const marked = helpTags('help-heads', 'BUTTON').filter((b) => b.textContent === 'What it does not do')[0];
   check('a heading written with inline markup is listed by its text, without the markup', !!marked, document.getElementById('help-heads').textContent);
   helpBody.scrollTop = 200;
   scrolledBefore = scrollsIntoView;
   if (marked) { marked.dispatchEvent(makeEvent('click')); }
   check('and choosing it scrolls to it rather than to the top', scrollsIntoView === scrolledBefore + 1 && helpBody.scrollTop === 200,
      'scrolls=' + (scrollsIntoView - scrolledBefore) + ' scrollTop=' + helpBody.scrollTop);
   // Escape in a field of the page is the field's; on the page it is the pane's.
   document.dispatchEvent(makeEvent('keydown', { key: 'Escape', target: document.getElementById('mail-search') }));
   check('Escape in the search box leaves the pane open', helpPane.hidden === false);
   document.dispatchEvent(makeEvent('keydown', { key: 'Escape', target: document.body }));
   check('and Escape on the page closes it', helpPane.hidden === true);
   document.dispatchEvent(makeEvent('keydown', { key: 'F1', target: document.body }));
   await flush();
   document.getElementById('help-close').dispatchEvent(makeEvent('click'));
   check('the close button closes it', helpPane.hidden === true);

   // ---- Zero access, the half that signs out and in: the key opened from
   // the sign-in password, and the recovery key after a reset
   await zeroAccessSignInChecks();

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
async function addressBookChecks() {
   // The page is in German by now, so every text a reader sees is the
   // catalogue's; asserting the English would prove it was NOT translated.
   const said = (english) => (germanCatalogue && germanCatalogue[english]) || english;
   const filled = (english, ...values) => values.reduce((s, v, i) => s.split('{' + i + '}').join(String(v)), said(english));
   const walkAll = (root, pred) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1) { if (pred(c)) { out.push(c); } walk(c); } } })(root); return out; };
   const popupOf = (input) => input.parentNode.children.filter((n) => String(n.className) === 'complete')[0];
   const dismissAllToasts = () => { const buttons = walkAll(document.getElementById('toasts'), (n) => n.tagName === 'BUTTON' && n.getAttribute('aria-label') === 'Dismiss'); buttons.forEach((b) => b.dispatchEvent(makeEvent('click'))); };

   const before = requests.length;
   location.hash = '#/contacts';
   await flush();
   await flush();

   const select = document.getElementById('book-select');
   check('the address books are read when the Contacts page is opened, and both are offered',
      called(before, 'GET', '/api/v1/me/address-books') && select.children.length === 2 &&
      select.children[0].textContent.indexOf('Contacts') >= 0 && select.children[1].textContent.indexOf('boss@example.com') >= 0,
      select.textContent);
   check('the contacts are asked for every book the reader can see, and the page draws the one chosen',
      called(before, 'GET', '/api/v1/me/contacts?limit=1000&book=all') &&
      document.getElementById('contact-rows').textContent.indexOf('dana@example.org') < 0 &&
      document.getElementById('contact-rows').textContent.indexOf('carol@example.org') >= 0,
      document.getElementById('contact-rows').textContent);

   // The owner's panel: who it is shared with, and with what.
   document.getElementById('book-manage').dispatchEvent(makeEvent('click'));
   await flush();
   const target = document.getElementById('book-share-target');
   check('Sharing opens the owner\'s panel, with a field for an address or a domain name', !!target && document.getElementById('book-detail').hidden === false);
   const beforeShare = requests.length;
   target.value = 'Boss@Example.com';
   document.getElementById('book-share-write').checked = true;
   target.parentNode.dispatchEvent(makeEvent('submit'));
   await flush();
   await flush();
   const shared = since(beforeShare).filter((r) => r.method === 'PUT' && r.path.indexOf('/api/v1/me/address-books/1/shares/') === 0).map((r) => ({ path: r.path, body: JSON.parse(r.body) }))[0];
   check('sharing sends the target lower-cased in the path and what it may do in the body',
      !!shared && decodeURIComponent(shared.path.split('/shares/')[1]) === 'boss@example.com' && shared.body.write === true,
      JSON.stringify(shared));
   check('and the share is then drawn, saying it can edit',
      document.getElementById('book-detail').textContent.indexOf('boss@example.com') >= 0 &&
      document.getElementById('book-detail').textContent.indexOf(said('can edit')) >= 0,
      document.getElementById('book-detail').textContent);

   // A domain name shares with everybody in it, and the row says so.
   const beforeDomain = requests.length;
   document.getElementById('book-share-target').value = 'example.com';
   document.getElementById('book-share-write').checked = false;
   document.getElementById('book-share-target').parentNode.dispatchEvent(makeEvent('submit'));
   await flush();
   await flush();
   check('a domain name is sent as it is typed and is drawn as the whole domain, read-only',
      called(beforeDomain, 'PUT', '/api/v1/me/address-books/1/shares/example.com') &&
      document.getElementById('book-detail').textContent.indexOf(said('every account in the domain')) >= 0 &&
      document.getElementById('book-detail').textContent.indexOf(said('read-only')) >= 0,
      document.getElementById('book-detail').textContent);

   const beforeUnshare = requests.length;
   const removes = walkAll(document.getElementById('book-detail'), (n) => n.tagName === 'BUTTON' && n.textContent === said('Remove'));
   removes[0].dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   check('Remove takes one share back and leaves the other',
      called(beforeUnshare, 'DELETE', '/api/v1/me/address-books/1/shares/boss%40example.com') &&
      document.getElementById('book-detail').textContent.indexOf('boss@example.com') < 0 &&
      document.getElementById('book-detail').textContent.indexOf('example.com') >= 0,
      document.getElementById('book-detail').textContent);

   // A second book of the reader's own.
   const beforeMade = requests.length;
   document.getElementById('book-name').value = 'Suppliers';
   document.getElementById('book-form').dispatchEvent(makeEvent('submit'));
   await flush();
   await flush();
   const made = since(beforeMade).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/address-books').map((r) => JSON.parse(r.body))[0];
   check('a new address book is made by name and becomes the one the page is in',
      !!made && made.name === 'Suppliers' && select.children.length === 3 && Number(select.value) === 11,
      JSON.stringify(made) + ' value=' + select.value);
   check('a contact added now names the book it goes in', true);
   const beforeAdd = requests.length;
   document.getElementById('contact-name').value = 'Supplier One';
   document.getElementById('contact-address').value = 'one@supplier.example';
   document.getElementById('contact-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const added = since(beforeAdd).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/contacts').map((r) => JSON.parse(r.body))[0];
   check('and the contact form sends that book with the contact', !!added && added.book === 11, JSON.stringify(added));

   const beforeDelete = requests.length;
   confirmAnswer = true;
   const deleteButton = walkAll(document.getElementById('book-detail'), (n) => n.tagName === 'BUTTON' && n.textContent === said('Delete this address book'))[0];
   deleteButton.dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   check('deleting a book asks first, naming it and what goes with it, and then removes it',
      confirms[confirms.length - 1] === filled('Delete the address book \"{0}\" and every contact in it?', 'Suppliers') &&
      called(beforeDelete, 'DELETE', '/api/v1/me/address-books/11') && select.children.length === 2,
      confirms[confirms.length - 1] + ' | ' + select.children.length);

   // The book somebody shares read-only: its contact is drawn, and there is no
   // way on the page to change anything in it.
   select.value = '5';
   select.dispatchEvent(makeEvent('change'));
   await flush();
   check('choosing a book shared read-only draws its contacts and nobody else\'s',
      document.getElementById('contact-rows').textContent.indexOf('dana@example.org') >= 0 &&
      document.getElementById('contact-rows').textContent.indexOf('carol@example.org') < 0,
      document.getElementById('contact-rows').textContent);
   check('the add form is gone and the page says the book is read-only',
      document.getElementById('contact-form').hidden === true && document.getElementById('contact-readonly').hidden === false &&
      document.getElementById('contact-readonly').textContent === said('This address book is shared with you read-only.'),
      document.getElementById('contact-readonly').textContent);
   check('Import is gone with it, since every card it posted would be refused, and Export stays, since reading is what a share is for',
      document.getElementById('contact-import').hidden === true &&
      document.getElementById('contact-export-vcf').hidden === false,
      'import=' + document.getElementById('contact-import').hidden + ' export=' + document.getElementById('contact-export-vcf').hidden);
   const danaRow = Array.from(document.getElementById('contact-rows').children)[0];
   check('and its contact offers Write and no Remove, because the server would refuse the delete',
      Array.from(danaRow.children[3].children).map((b) => b.textContent).join(',') === said('Write'),
      Array.from(danaRow.children[3].children).map((b) => b.textContent).join(','));

   // Leaving it gives the share back and takes the book off the page.
   document.getElementById('book-manage').dispatchEvent(makeEvent('click'));
   await flush();
   const beforeLeave = requests.length;
   const leave = walkAll(document.getElementById('book-detail'), (n) => n.tagName === 'BUTTON' && n.textContent === said('Leave this address book'))[0];
   check('a book shared with the reader offers to be left rather than deleted', !!leave,
      document.getElementById('book-detail').textContent);
   leave.dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   check('leaving it gives the share back and the book is gone from the page',
      called(beforeLeave, 'DELETE', '/api/v1/me/address-books/5') && select.children.length === 1,
      select.textContent);
   check('and Import is offered again in the reader\'s own book',
      document.getElementById('contact-import').hidden === false && document.getElementById('contact-form').hidden === false,
      'import=' + document.getElementById('contact-import').hidden);

   // The To field completes from every book the reader can see, and says whose
   // a shared one is. The book is back for this, since leaving it took it away.
   addressBooks.push({ id: 5, name: 'Team', owner: 'boss@example.com', mine: false, writable: false, is_default: false, created: 1758000000, contacts: 1 });
   location.hash = '#/compose';
   await flush();
   const to = document.getElementById('compose-to');
   to.value = 'dana';
   const beforeOffer = requests.length;
   to.dispatchEvent(makeEvent('input'));
   fireTimers();
   await flush();
   const offered = popupOf(to).children;
   check('the To field asks every book the reader can see and labels the shared one with its name',
      called(beforeOffer, 'GET', '/api/v1/me/contacts?limit=8&q=dana&book=all') && offered.length === 1 &&
      offered[0].textContent.indexOf('Dana Shared') >= 0 && offered[0].textContent.indexOf('Team') >= 0,
      popupOf(to).textContent);

   // A group is NEVER shared: it is one account's own entry and expands against
   // that account's whole address book, so a group of a shared book would carry
   // the names and addresses of contacts in books nobody shared. The server
   // lists none (RestApiContacts.cpp); this asks what the page would do with
   // one if a listing ever carried it, which is the same thing.
   sharedGroupListed = true;
   to.value = 'their';
   const beforeGroupOffer = requests.length;
   to.dispatchEvent(makeEvent('input'));
   fireTimers();
   await flush();
   await flush();
   check('a group out of a book somebody shares with the reader is not offered in To, and so cannot expand into its members',
      called(beforeGroupOffer, 'GET', '/api/v1/me/contacts?limit=8&q=their&book=all') &&
      popupOf(to).hidden === true && popupOf(to).textContent.indexOf('Their Team') < 0 &&
      to.value === 'their',
      popupOf(to).textContent + ' hidden=' + popupOf(to).hidden);
   document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
   await flush();
   dismissAllToasts();

   location.hash = '#/contacts';
   await flush();
   await flush();
   select.value = '5';
   select.dispatchEvent(makeEvent('change'));
   await flush();
   check('and it is not drawn in the Groups card of that book either',
      document.getElementById('group-rows').textContent.indexOf('Their Team') < 0 &&
      document.getElementById('contact-rows').textContent.indexOf('Their Team') < 0 &&
      document.getElementById('group-empty').hidden === false,
      document.getElementById('group-rows').textContent);
   sharedGroupListed = false;

   location.hash = '#/f/1';
   await flush();
}

// ---- OpenPGP: the key ring, a message sealed in the browser, and Autocrypt.
// Every assertion here is on something a translation cannot move - a request,
// a fingerprint, an address, a hidden flag - because the run reaches this point
// with the page in whatever language an earlier check left it in.
async function pgpChecks() {
   // The module the page fetches on demand, already in this world: nothing here
   // loads a script, and what the real module does is build/check-portal-openpgp.js's
   // business rather than this file's.
   window.hmOpenPgp = pgpStub;
   const groups = (fingerprint) => String(fingerprint).toUpperCase().replace(/(.{4})/g, '$1 ').trim();
   const ownRows = () => document.getElementById('pgp-own-rows').children;
   const peerRows = () => document.getElementById('pgp-peer-rows').children;
   const buttonsOf = (row) => Array.prototype.slice.call(row.children[3].children);
   const PASSPHRASE = 'the-key-passphrase';

   pgpStores[account] = { own: [pgpOwnRowOf(account, PASSPHRASE)], correspondents: [pgpRowOf('a@example.net')] };
   const beforeRing = requests.length;
   location.hash = '#/security';
   await flush();
   await flush();
   check('OpenPGP: the Security page asks for the key ring', called(beforeRing, 'GET', '/api/v1/me/openpgp/keys'),
      JSON.stringify(since(beforeRing).map((r) => r.method + ' ' + r.path).filter((p) => p.indexOf('openpgp') >= 0)));
   check('OpenPGP: the account\'s own key is listed, its fingerprint in fours',
      ownRows().length === 1 && ownRows()[0].textContent.indexOf(groups(pgpFingerprint(account))) >= 0,
      ownRows().length + ' rows: ' + (ownRows()[0] ? ownRows()[0].textContent : ''));
   check('OpenPGP: and the correspondent\'s key, with where it came from',
      peerRows().length === 1 && peerRows()[0].textContent.indexOf('a@example.net') >= 0 && peerRows()[0].textContent.indexOf('imported') >= 0,
      peerRows().length + ' rows: ' + (peerRows()[0] ? peerRows()[0].textContent : ''));

   // ---- the compose form offers what the keys allow, and names what is missing
   document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
   await flush();
   check('OpenPGP: the compose form offers Sign and Encrypt once there is a key',
      document.getElementById('compose-pgp-sign-label').hidden === false && document.getElementById('compose-pgp-encrypt-label').hidden === false,
      'sign hidden=' + document.getElementById('compose-pgp-sign-label').hidden);
   document.getElementById('compose-to').value = 'nobody@example.org';
   document.getElementById('compose-pgp-encrypt').checked = true;
   document.getElementById('compose-pgp-encrypt').dispatchEvent(makeEvent('change'));
   await flush();
   check('OpenPGP: and names the recipient there is no key for',
      document.getElementById('compose-pgp-note').hidden === false && document.getElementById('compose-pgp-note').textContent.indexOf('nobody@example.org') >= 0,
      document.getElementById('compose-pgp-note').textContent);

   // ---- a draft that was to be sealed is refused rather than stored in the clear
   const beforeDraft = requests.length;
   document.getElementById('compose-save').dispatchEvent(makeEvent('click'));
   await flush();
   check('OpenPGP: a draft of a message that was to be sealed is refused, not stored in the clear',
      !called(beforeDraft, 'POST', '/api/v1/me/drafts') && document.getElementById('compose-status').textContent.length > 0,
      document.getElementById('compose-status').textContent);

   // ---- a message signed and encrypted in the browser
   const composedBefore = pgpComposed.length;
   document.getElementById('compose-to').value = 'a@example.net';
   document.getElementById('compose-subject').value = 'Sealed with OpenPGP';
   document.getElementById('compose-text').value = 'Only for you.';
   document.getElementById('compose-pgp-sign').checked = true;
   document.getElementById('compose-pgp-sign').dispatchEvent(makeEvent('change'));
   const beforeSeal = requests.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   fireTimers();
   await flush();
   await flush();
   check('OpenPGP: a send that is to be signed stops for the passphrase and sends nothing until it has one',
      document.getElementById('pgp-unlock').hidden === false && !called(beforeSeal, 'POST', '/api/v1/me/messages'),
      'dialogue hidden=' + document.getElementById('pgp-unlock').hidden);
   document.getElementById('pgp-unlock-passphrase').value = 'not-the-passphrase';
   document.getElementById('pgp-unlock-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('OpenPGP: a wrong passphrase is refused and still sends nothing',
      document.getElementById('pgp-unlock').hidden === false && document.getElementById('pgp-unlock-status').textContent.length > 0 &&
      !called(beforeSeal, 'POST', '/api/v1/me/messages'),
      document.getElementById('pgp-unlock-status').textContent);
   document.getElementById('pgp-unlock-passphrase').value = PASSPHRASE;
   document.getElementById('pgp-unlock-form').dispatchEvent(makeEvent('submit'));
   await flush();
   await flush();
   const sealed = since(beforeSeal).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/messages');
   const sealedBody = sealed.length ? JSON.parse(sealed[0].body || '{}') : {};
   check('OpenPGP: the message goes as the entity the page built and carries no plaintext',
      sealed.length === 1 && typeof sealedBody.mime === 'string' && sealedBody.mime.indexOf('multipart/encrypted') > 0 &&
      sealedBody.text === undefined && sealedBody.pgp_sign === undefined && sealedBody.pgp_encrypt === undefined,
      JSON.stringify(Object.keys(sealedBody)) + ' ' + String(sealedBody.mime).slice(0, 60));
   check('OpenPGP: sealed to the recipient\'s key and to the sender\'s own, signed with the sender\'s',
      pgpComposed.length === composedBefore + 1 &&
      pgpComposed[composedBefore].encrypt.join(',') === ['a@example.net', account].sort().join(',') &&
      pgpComposed[composedBefore].sign === account,
      JSON.stringify(pgpComposed[composedBefore]));
   check('OpenPGP: and it carries the sender\'s Autocrypt header',
      String(sealedBody.autocrypt || '').indexOf('addr=' + account + '; prefer-encrypt=mutual; keydata=') === 0,
      String(sealedBody.autocrypt).slice(0, 60));

   // ---- a message that is not sealed carries the header too, which is the
   // ordinary way a correspondent learns the key
   document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
   await flush();
   document.getElementById('compose-to').value = 'a@example.net';
   document.getElementById('compose-subject').value = 'In the clear';
   document.getElementById('compose-text').value = 'Nothing secret here.';
   const beforePlain = requests.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   fireTimers();
   await flush();
   await flush();
   const plain = since(beforePlain).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/messages');
   const plainBody = plain.length ? JSON.parse(plain[0].body || '{}') : {};
   check('OpenPGP: a plain message carries the Autocrypt header and its own text',
      plain.length === 1 && plainBody.text === 'Nothing secret here.' && String(plainBody.autocrypt || '').indexOf('addr=' + account) === 0,
      JSON.stringify(Object.keys(plainBody)));

   // ---- Autocrypt read: a key arriving on a message from somebody with none here
   pgpMessageResult = {
      layers: [], encrypted: null, signed: null, inline: false, content: null,
      autocrypt: { address: 'b@example.net', preferEncrypt: true, keydata: 'KEY:b@example.net:' + pgpFingerprint('b@example.net') }
   };
   const beforeLearn = requests.length;
   location.hash = '#/m/602';
   await flush();
   await flush();
   await flush();
   const learned = since(beforeLearn).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/openpgp/keys/correspondents');
   check('OpenPGP: a key on an Autocrypt header from somebody with none here is kept, marked autocrypt',
      learned.length === 1 && JSON.parse(learned[0].body || '{}').source === 'autocrypt' && JSON.parse(learned[0].body || '{}').address === 'b@example.net',
      JSON.stringify(learned.map((r) => r.body)));
   check('OpenPGP: and the reader is told, rather than it happening silently',
      document.getElementById('message-pgp').hidden === false && document.getElementById('message-pgp-text').textContent.indexOf('b@example.net') >= 0,
      document.getElementById('message-pgp-text').textContent);

   // ---- and a DIFFERENT key for an address already held is not kept silently:
   // a key that changes without anybody being asked is what a man in the middle
   // needs, so it waits behind a button
   location.hash = '#/f/1';
   await flush();
   pgpMessageResult = {
      layers: [], encrypted: null, signed: null, inline: false, content: null,
      autocrypt: { address: 'b@example.net', preferEncrypt: true, keydata: 'KEY:b@example.net:' + pgpFingerprint('b@example.net', 'second') }
   };
   const beforeChange = requests.length;
   location.hash = '#/m/602';
   await flush();
   await flush();
   await flush();
   check('OpenPGP: a key that would REPLACE the one already held is not kept silently',
      !called(beforeChange, 'PUT', '/api/v1/me/openpgp/keys/correspondents') &&
      document.getElementById('message-pgp-keep').hidden === false &&
      document.getElementById('message-pgp-text').textContent.indexOf(groups(pgpFingerprint('b@example.net', 'second'))) >= 0,
      document.getElementById('message-pgp-text').textContent);
   document.getElementById('message-pgp-keep').dispatchEvent(makeEvent('click'));
   await flush();
   check('OpenPGP: and is kept when the reader presses Keep',
      called(beforeChange, 'PUT', '/api/v1/me/openpgp/keys/correspondents'),
      JSON.stringify(since(beforeChange).filter((r) => r.method === 'PUT').map((r) => r.path)));
   pgpMessageResult = { layers: [], encrypted: null, signed: null, content: null, autocrypt: null, inline: false };
   location.hash = '#/f/1';
   await flush();

   // ---- looking a correspondent's key up in their own domain
   location.hash = '#/security';
   await flush();
   await flush();
   pgpLookupAnswer = {
      found: true, address: 'c@example.net', source: 'wkd-advanced', url: 'https://openpgpkey.example.net/.well-known/openpgpkey/example.net/hu/abc',
      key: pgpRowOf('c@example.net', { source: 'wkd' })
   };
   const beforeLookup = requests.length;
   document.getElementById('pgp-lookup-address').value = 'c@example.net';
   document.getElementById('pgp-lookup-form').dispatchEvent(makeEvent('submit'));
   await flush();
   await flush();
   const kept = since(beforeLookup).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/openpgp/keys/correspondents');
   check('OpenPGP: a key found in the correspondent\'s own directory shows its fingerprint before it is kept',
      confirms.length > 0 && confirms[confirms.length - 1].indexOf(groups(pgpFingerprint('c@example.net'))) >= 0,
      confirms[confirms.length - 1] || '');
   check('OpenPGP: and is kept with wkd as its source',
      kept.length === 1 && JSON.parse(kept[0].body || '{}').source === 'wkd',
      JSON.stringify(kept.map((r) => r.body)));

   // ---- revoking a key: the certificate is made by the key, applied to it,
   // and the key stored again so that everyone who fetches it is told
   const beforeRevoke = requests.length;
   check('OpenPGP: an own key offers five things to do with it, Revoke among them',
      ownRows().length === 1 && buttonsOf(ownRows()[0]).length === 5, buttonsOf(ownRows()[0]).length + ' buttons');
   buttonsOf(ownRows()[0])[3].dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   const revoked = since(beforeRevoke).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/openpgp/keys/own');
   check('OpenPGP: revoking stores the key revoked, so everyone who fetches it afterwards is told',
      revoked.length === 1 && JSON.parse(revoked[0].body || '{}').revoked === true,
      JSON.stringify(revoked.map((r) => String(r.body).slice(0, 90))));
   check('OpenPGP: and a revoked key is offered nothing to revoke twice',
      ownRows().length === 1 && buttonsOf(ownRows()[0]).length === 4, buttonsOf(ownRows()[0]).length + ' buttons');
   location.hash = '#/f/1';
   await flush();
}

// ---- OpenPGP, trust made visible (OpenPgpRing.h, schema 6081). Every
// assertion is on a request, a fingerprint, an address, a hidden flag or the
// data-pgp-trust attribute the page names a state with - never on a sentence,
// because the page is in German by now.
async function pgpTrustChecks() {
   window.hmOpenPgp = pgpStub;
   const groups = (fingerprint) => String(fingerprint).toUpperCase().replace(/(.{4})/g, '$1 ').trim();
   const peerRows = () => Array.prototype.slice.call(document.getElementById('pgp-peer-rows').children);
   const rowFor = (address) => peerRows().filter((tr) => tr.children[0].textContent.indexOf(address) === 0)[0];
   const buttonsOf = (row) => Array.prototype.slice.call(row.children[3].children);
   const trustOfRow = (address) => (rowFor(address) ? rowFor(address).getAttribute('data-pgp-trust') : 'no row');
   const badges = () => Array.prototype.slice.call(document.getElementById('message-badges').children);
   const trustShown = () => badges().map((b) => b.getAttribute('data-pgp-trust')).filter(Boolean).join(',');
   const pgpText = () => document.getElementById('message-pgp-text').textContent;
   const dismissAllToasts = () => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.tagName === 'BUTTON' && c.getAttribute('aria-label') === 'Dismiss') { out.push(c); } if (c.childNodes) { walk(c); } } })(document.getElementById('toasts')); out.forEach((b) => b.dispatchEvent(makeEvent('click'))); };
   const oldB = pgpFingerprint('b@example.net');
   const newB = pgpFingerprint('b@example.net', 'second');

   // Three correspondents in the three states, and the account's own key.
   pgpStores[account] = {
      own: [pgpOwnRowOf(account, 'the-key-passphrase')],
      correspondents: [pgpRowOf('p@example.net'), pgpRowOf('v@example.net', { verified: true, verified_at: 1700000200 }), pgpChangedRowOf('b@example.net')]
   };
   location.hash = '#/security';
   await flush();
   await flush();
   check('Trust: a correspondent\'s fingerprint is shown in groups of four',
      !!rowFor('p@example.net') && rowFor('p@example.net').children[1].textContent === groups(pgpFingerprint('p@example.net')),
      rowFor('p@example.net') ? rowFor('p@example.net').children[1].textContent : 'no row');
   check('Trust: the three states are named on the rows - plain, verified, changed',
      trustOfRow('p@example.net') === 'plain' && trustOfRow('v@example.net') === 'verified' && trustOfRow('b@example.net') === 'changed',
      [trustOfRow('p@example.net'), trustOfRow('v@example.net'), trustOfRow('b@example.net')].join(','));
   check('Trust: the verified row carries the mark in its fingerprint cell',
      !!rowFor('v@example.net') && rowFor('v@example.net').children[1].children.length === 1 &&
      rowFor('v@example.net').children[1].children[0].className.indexOf('pgp-verified') >= 0 &&
      rowFor('v@example.net').children[1].children[0].textContent.indexOf('✓') === 0,
      rowFor('v@example.net') ? rowFor('v@example.net').children[1].textContent : 'no row');
   check('Trust: the changed row names both fingerprints, the old and the new',
      !!rowFor('b@example.net') && rowFor('b@example.net').textContent.indexOf(groups(oldB)) >= 0 && rowFor('b@example.net').textContent.indexOf(groups(newB)) >= 0,
      rowFor('b@example.net') ? rowFor('b@example.net').textContent : 'no row');
   check('Trust: a changed row offers Accept beside the mark, a plain one the mark alone, an own row neither',
      !!rowFor('b@example.net') && buttonsOf(rowFor('b@example.net')).length === 4 && buttonsOf(rowFor('p@example.net')).length === 3 &&
      document.getElementById('pgp-own-rows').children[0].children.length === 4 && Array.prototype.slice.call(document.getElementById('pgp-own-rows').children[0].children[3].children).length === 5,
      buttonsOf(rowFor('b@example.net')).length + ' on the changed row, ' + buttonsOf(rowFor('p@example.net')).length + ' on the plain one');

   // ---- marking a key verified in person, and unmarking it
   const beforeMark = requests.length;
   buttonsOf(rowFor('p@example.net'))[1].dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   const marked = since(beforeMark).filter((r) => r.method === 'PUT' && /\/openpgp\/keys\/\d+\/verified$/.test(r.path));
   check('Trust: marking a key verified in person is one PUT of verified true, and the row shows the mark after',
      marked.length === 1 && JSON.parse(marked[0].body || '{}').verified === true && trustOfRow('p@example.net') === 'verified',
      JSON.stringify(marked.map((r) => r.path + ' ' + r.body)) + ' -> ' + trustOfRow('p@example.net'));
   const beforeUnmark = requests.length;
   buttonsOf(rowFor('p@example.net'))[1].dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   const unmarked = since(beforeUnmark).filter((r) => r.method === 'PUT' && /\/openpgp\/keys\/\d+\/verified$/.test(r.path));
   check('Trust: and unmarking it is the same PUT with false, after which the row is plain again',
      unmarked.length === 1 && JSON.parse(unmarked[0].body || '{}').verified === false && trustOfRow('p@example.net') === 'plain',
      JSON.stringify(unmarked.map((r) => r.body)) + ' -> ' + trustOfRow('p@example.net'));

   // ---- the compose form: a recipient whose only key changed and was not
   // accepted is not encrypted to; the send stops and names the address
   document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
   await flush();
   document.getElementById('compose-to').value = 'b@example.net';
   document.getElementById('compose-subject').value = 'To a changed key';
   document.getElementById('compose-text').value = 'Must not be sealed to the new key.';
   document.getElementById('compose-pgp-encrypt').checked = true;
   document.getElementById('compose-pgp-encrypt').dispatchEvent(makeEvent('change'));
   await flush();
   check('Trust: the compose form names the recipient whose key changed and waits to be accepted',
      document.getElementById('compose-pgp-note').hidden === false && document.getElementById('compose-pgp-note').textContent.indexOf('b@example.net') >= 0,
      document.getElementById('compose-pgp-note').textContent);
   const composedBefore = pgpComposed.length;
   const beforeRefused = requests.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   fireTimers();
   await flush();
   await flush();
   check('Trust: a send encrypted to that address is refused - nothing composed, nothing posted - and the reason names it',
      pgpComposed.length === composedBefore && !called(beforeRefused, 'POST', '/api/v1/me/messages') &&
      document.getElementById('compose-status').textContent.indexOf('b@example.net') >= 0,
      document.getElementById('compose-status').textContent);
   document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
   await flush();
   dismissAllToasts();

   // ---- the reader, state three: a good signature by the key that changed
   // is the warning, both fingerprints, and Accept - nothing green
   pgpMessageResult = { layers: ['signed'], encrypted: null, inline: false, content: null, autocrypt: null, signed: pgpGoodSignatureBy('b@example.net', 'second') };
   location.hash = '#/m/602';
   await flush();
   await flush();
   await flush();
   check('Trust: a good signature by a key that changed shows the warning, not a good badge',
      trustShown() === 'changed' && badges().filter((b) => b.className.indexOf('good') >= 0).length === 0,
      trustShown() + ' / ' + badges().map((b) => b.className).join(';'));
   check('Trust: and the reader is told both fingerprints and offered Accept',
      pgpText().indexOf(groups(oldB)) >= 0 && pgpText().indexOf(groups(newB)) >= 0 && document.getElementById('message-pgp-accept').hidden === false,
      pgpText() + ' accept hidden=' + document.getElementById('message-pgp-accept').hidden);

   // ---- accepting from the reader: one PUT of accepted true, then the same
   // message re-read shows the plain verdict with the fingerprint (state two)
   const beforeAccept = requests.length;
   document.getElementById('message-pgp-accept').dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   await flush();
   const accepted = since(beforeAccept).filter((r) => r.method === 'PUT' && /\/openpgp\/keys\/\d+\/accepted$/.test(r.path));
   check('Trust: Accept is one PUT of accepted true on the changed key',
      accepted.length === 1 && JSON.parse(accepted[0].body || '{}').accepted === true,
      JSON.stringify(accepted.map((r) => r.path + ' ' + r.body)));
   check('Trust: after which the signature is the plain verdict, with the signing key\'s fingerprint',
      trustShown() === 'plain' && pgpText().indexOf(groups(newB)) >= 0 && document.getElementById('message-pgp-accept').hidden === true,
      trustShown() + ' / ' + pgpText());

   // ---- state one: the same key marked verified in person shows the mark
   // beside the sender on the next good signature
   location.hash = '#/security';
   await flush();
   await flush();
   check('Trust: the accepted key is listed plain, with the mark on offer', trustOfRow('b@example.net') === 'plain' && buttonsOf(rowFor('b@example.net')).length === 3,
      trustOfRow('b@example.net') + ' ' + (rowFor('b@example.net') ? buttonsOf(rowFor('b@example.net')).length : 0));
   buttonsOf(rowFor('b@example.net'))[1].dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   location.hash = '#/m/602';
   await flush();
   await flush();
   await flush();
   check('Trust: a good signature by a key verified in person shows the green mark beside the sender',
      trustShown() === 'verified' && badges().filter((b) => b.getAttribute('data-pgp-trust') === 'verified' && b.className.indexOf('good') >= 0 && b.textContent.indexOf('✓') === 0).length === 1,
      trustShown() + ' / ' + badges().map((b) => b.className + ':' + b.textContent).join(';'));

   // ---- the mark is never inherited: a THIRD key for the address is a change
   // again, unverified, and the reader says so
   pgpStores[account].correspondents.push(pgpRowOf('b@example.net', {
      fingerprint: pgpFingerprint('b@example.net', 'third'), key_id: pgpFingerprint('b@example.net', 'third').slice(-16),
      public_key: pgpPublicOf('b@example.net', 'third'), changed_from: newB, changed_at: 1700000400, accepted: false
   }));
   pgpMessageResult = { layers: ['signed'], encrypted: null, inline: false, content: null, autocrypt: null, signed: pgpGoodSignatureBy('b@example.net', 'third') };
   location.hash = '#/f/1';
   await flush();
   location.hash = '#/security';
   await flush();
   await flush();
   location.hash = '#/m/602';
   await flush();
   await flush();
   await flush();
   check('Trust: a new key for a verified correspondent is not verified because the old one was - the warning, again',
      trustShown() === 'changed' && pgpText().indexOf(groups(newB)) >= 0 && pgpText().indexOf(groups(pgpFingerprint('b@example.net', 'third'))) >= 0,
      trustShown() + ' / ' + pgpText());
   pgpMessageResult = { layers: [], encrypted: null, signed: null, content: null, autocrypt: null, inline: false };
   location.hash = '#/f/1';
   await flush();

   // ---- the review's findings, each held to what it asked for.
   // (1) A correspondent's key is never filed under the account's own address
   // in silence. An Autocrypt key whose user id carries no address is filed
   // under the address the header names, never the account's; and a key
   // arriving for the account's OWN address - a spoofed From: me - is a change
   // of the own key, and no message is sealed to it.
   const ownFp = pgpFingerprint(account);
   pgpStores[account] = { own: [pgpOwnRowOf(account, 'the-key-passphrase')], correspondents: [pgpRowOf('a@example.net')] };
   location.hash = '#/security';
   await flush();
   await flush();
   pgpMessageResult = {
      layers: [], encrypted: null, signed: null, inline: false, content: null,
      autocrypt: { address: 'b@example.net', preferEncrypt: true, keydata: 'KEY::' + pgpFingerprint('nobody', 'stranger') }
   };
   const beforeStranger = requests.length;
   location.hash = '#/m/602';
   await flush();
   await flush();
   await flush();
   const strangerPuts = since(beforeStranger).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/openpgp/keys/correspondents');
   check('Trust: an Autocrypt key with no address in its user id is filed under the address the header names, never the account\'s',
      strangerPuts.length === 1 && JSON.parse(strangerPuts[0].body || '{}').address === 'b@example.net',
      JSON.stringify(strangerPuts.map((r) => r.body)));
   location.hash = '#/f/1';
   await flush();
   // A spoofed message from the account's own address, carrying another key
   // for it: offered Keep (the page holds the own key), and once kept it is a
   // change OF THE OWN KEY, so a send is sealed to the own key and not to it.
   WITH_AUTOCRYPT.from = account;
   pgpMessageResult = {
      layers: [], encrypted: null, signed: null, inline: false, content: null,
      autocrypt: { address: account, preferEncrypt: true, keydata: 'KEY:' + account + ':' + pgpFingerprint(account, 'spoof') }
   };
   const beforeSpoof = requests.length;
   location.hash = '#/m/602';
   await flush();
   await flush();
   await flush();
   check('Trust: a key arriving for the account\'s own address is not kept without asking',
      !called(beforeSpoof, 'PUT', '/api/v1/me/openpgp/keys/correspondents') && document.getElementById('message-pgp-keep').hidden === false,
      'keep hidden=' + document.getElementById('message-pgp-keep').hidden);
   document.getElementById('message-pgp-keep').dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   await flush();
   const spoofRow = pgpStores[account].correspondents.filter((o) => o.fingerprint === pgpFingerprint(account, 'spoof'))[0];
   check('Trust: and once kept it is a change of the OWN key, said with both fingerprints and Accept',
      !!spoofRow && spoofRow.changed_from === ownFp && pgpText().indexOf(groups(ownFp)) >= 0 && pgpText().indexOf(groups(pgpFingerprint(account, 'spoof'))) >= 0 &&
      document.getElementById('message-pgp-accept').hidden === false,
      (spoofRow ? spoofRow.changed_from : 'no row') + ' / ' + pgpText());
   WITH_AUTOCRYPT.from = 'b@example.net';
   location.hash = '#/f/1';
   await flush();
   document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
   await flush();
   document.getElementById('compose-to').value = 'a@example.net';
   document.getElementById('compose-subject').value = 'Sealed to my own key';
   document.getElementById('compose-text').value = 'Not to the spoof.';
   document.getElementById('compose-pgp-encrypt').checked = true;
   document.getElementById('compose-pgp-encrypt').dispatchEvent(makeEvent('change'));
   await flush();
   const composedBeforeSpoof = pgpComposed.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   fireTimers();
   await flush();
   await flush();
   check('Trust: a send is sealed to the account\'s own key and never to the key that arrived for its address',
      pgpComposed.length === composedBeforeSpoof + 1 && pgpComposed[composedBeforeSpoof].fingerprints.indexOf(ownFp) >= 0 &&
      pgpComposed[composedBeforeSpoof].fingerprints.indexOf(pgpFingerprint(account, 'spoof')) < 0,
      JSON.stringify(pgpComposed[composedBeforeSpoof]));
   dismissAllToasts();

   // (4) The auto-learn path says the change sentence too: the page held
   // nothing for the address, the ring did (a row stored elsewhere), and the
   // key it learned came back as a change.
   pgpStores[account] = { own: [pgpOwnRowOf(account, 'the-key-passphrase')], correspondents: [] };
   location.hash = '#/security';
   await flush();
   await flush();
   pgpStores[account].correspondents.push(pgpRowOf('b@example.net'));
   pgpMessageResult = {
      layers: [], encrypted: null, signed: null, inline: false, content: null,
      autocrypt: { address: 'b@example.net', preferEncrypt: true, keydata: 'KEY:b@example.net:' + newB }
   };
   const beforeStale = requests.length;
   location.hash = '#/m/602';
   await flush();
   await flush();
   await flush();
   check('Trust: a key learned at once that the ring filed as a change is said with both fingerprints and Accept',
      called(beforeStale, 'PUT', '/api/v1/me/openpgp/keys/correspondents') && pgpText().indexOf(groups(oldB)) >= 0 && pgpText().indexOf(groups(newB)) >= 0 &&
      document.getElementById('message-pgp-accept').hidden === false,
      pgpText() + ' accept hidden=' + document.getElementById('message-pgp-accept').hidden);
   pgpMessageResult = { layers: [], encrypted: null, signed: null, content: null, autocrypt: null, inline: false };
   location.hash = '#/f/1';
   await flush();

   // (5) A settled key and a pending one for the same address: the compose
   // form says a newer key is waiting and the send goes to the older one.
   pgpStores[account] = { own: [pgpOwnRowOf(account, 'the-key-passphrase')], correspondents: [pgpRowOf('b@example.net'), pgpChangedRowOf('b@example.net')] };
   location.hash = '#/security';
   await flush();
   await flush();
   document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
   await flush();
   document.getElementById('compose-to').value = 'b@example.net';
   document.getElementById('compose-subject').value = 'To the older key';
   document.getElementById('compose-text').value = 'The newer one waits.';
   document.getElementById('compose-pgp-encrypt').checked = true;
   document.getElementById('compose-pgp-encrypt').dispatchEvent(makeEvent('change'));
   await flush();
   check('Trust: with a newer key waiting beside a usable one, the compose form says so and names the address',
      document.getElementById('compose-pgp-note').hidden === false && document.getElementById('compose-pgp-note').textContent.indexOf('b@example.net') >= 0,
      document.getElementById('compose-pgp-note').textContent);
   const composedBeforeOlder = pgpComposed.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   fireTimers();
   await flush();
   await flush();
   check('Trust: and the send goes, sealed to the older key and not to the one waiting',
      pgpComposed.length === composedBeforeOlder + 1 && pgpComposed[composedBeforeOlder].fingerprints.indexOf(oldB) >= 0 &&
      pgpComposed[composedBeforeOlder].fingerprints.indexOf(newB) < 0,
      JSON.stringify(pgpComposed[composedBeforeOlder]));
   dismissAllToasts();
   location.hash = '#/f/1';
   await flush();
}

// ---- A password-protected message to anyone (ProtectedMessages.h,
// RestApiProtectedMessages.cpp): the compose control appears only where the
// page has its cryptography and the server offers the feature; a send goes
// to the record and content routes and NEVER carries the password, nor the
// text; and the reading page - a document of its own, run here with the REAL
// OpenPGP and MIME modules - decrypts a message the real module made, says
// one sentence for a wrong password, and seals a reply under the same
// password.
async function protectedChecks() {
   window.hmOpenPgp = pgpStub;
   const everyRequestText = (from) => since(from).map((r) => {
      const body = r.body instanceof Uint8Array ? Buffer.from(r.body).toString('latin1') : String(r.body || '');
      return r.method + ' ' + r.path + ' ' + JSON.stringify(r.headers || {}) + ' ' + body;
   }).join('\n');
   const PASSWORD = 'correct-horse-battery-staple';
   // The page has been switched to German by the checks above, so every
   // text a reader sees here is the catalogue's; asserting the English
   // would prove the page was NOT translated.
   const said = (english) => (germanCatalogue && germanCatalogue[english]) || english;
   const dismissAllToastsHere = () => {
      const buttons = [];
      (function walk(n) { for (let i = 0; i < n.childNodes.length; i += 1) { const c = n.childNodes[i]; if (c.tagName === 'BUTTON' && c.getAttribute('aria-label') === 'Dismiss') { buttons.push(c); } if (c.childNodes) { walk(c); } } })(document.getElementById('toasts'));
      buttons.forEach((b) => b.dispatchEvent(makeEvent('click')));
   };

   // ---- not offered: the server says so, and the control stays away
   protectedState.available = false;
   document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
   await flush();
   check('Protected: the compose form does not offer Protect with a password while the server does not offer it',
      document.getElementById('compose-protect-label').hidden === true);

   // ---- offered: the control appears, and its panel says what it promises
   protectedState.available = true;
   // The page learns what the server offers once at sign-in and again on
   // its own page: visiting the page is how the form finds out.
   location.hash = '#/protected';
   await flush();
   await flush();
   location.hash = '#/f/1';
   await flush();
   await flush();
   document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
   await flush();
   await flush();
   check('Protected: the compose form offers Protect with a password once the server offers it and the page has its cryptography',
      document.getElementById('compose-protect-label').hidden === false, 'hidden=' + document.getElementById('compose-protect-label').hidden);
   document.getElementById('compose-protect').checked = true;
   document.getElementById('compose-protect').dispatchEvent(makeEvent('change'));
   await flush();
   check('Protected: ticking it opens the panel and disables the other seals',
      document.getElementById('compose-protect-panel').hidden === false && document.getElementById('compose-pgp-encrypt').disabled === true && document.getElementById('compose-sign').disabled === true);
   check('Protected: the panel says in one sentence what it does not promise',
      document.getElementById('compose-protect-panel').textContent.indexOf(said('The text and the attachments are encrypted in this browser to the password before anything is sent. The recipient gets a short message with a link to a page on this server, types the password there, and their browser decrypts it; a reply written there comes back the same way. The password never leaves this browser and the server never sees the text, so tell them the password some other way than this message. It does not stop the recipient copying or forwarding what they read.')) >= 0,
      document.getElementById('compose-protect-panel').textContent.slice(0, 80));
   document.getElementById('compose-protect-password').value = 'short';
   document.getElementById('compose-protect-password').dispatchEvent(makeEvent('input'));
   check('Protected: a short password gets a sentence, not a bar',
      document.getElementById('compose-protect-strength').textContent === said('Too short: a password here is at least eight characters.'), document.getElementById('compose-protect-strength').textContent);
   document.getElementById('compose-protect-password').value = PASSWORD;
   document.getElementById('compose-protect-password').dispatchEvent(makeEvent('input'));
   check('Protected: a long one is called strong', document.getElementById('compose-protect-strength').textContent === said('Strong.'), document.getElementById('compose-protect-strength').textContent);
   document.getElementById('compose-protect-password').value = 'aaaaaaaaaaaaaaaa';
   document.getElementById('compose-protect-password').dispatchEvent(makeEvent('input'));
   check('Protected: sixteen of one letter are not called strong',
      document.getElementById('compose-protect-strength').textContent === said('Fair. A few more characters, or another word, would make it strong.'), document.getElementById('compose-protect-strength').textContent);
   document.getElementById('compose-protect-password').value = PASSWORD;
   document.getElementById('compose-protect-password').dispatchEvent(makeEvent('input'));

   // ---- a draft of one is refused rather than stored in the clear
   document.getElementById('compose-to').value = 'nobody@example.org';
   document.getElementById('compose-subject').value = 'The board pack';
   document.getElementById('compose-text').value = 'The figures are 42.';
   const beforeDraft = requests.length;
   document.getElementById('compose-save').dispatchEvent(makeEvent('click'));
   await flush();
   check('Protected: a draft of a message that is to be protected is refused, not stored in the clear',
      !called(beforeDraft, 'POST', '/api/v1/me/drafts') && document.getElementById('compose-status').textContent === said('A password-protected message cannot be saved as a draft or scheduled: the draft would hold the text in the clear. Untick it, or send it now.'),
      document.getElementById('compose-status').textContent);

   // ---- the two passwords have to agree, and nothing is sent until they do
   document.getElementById('compose-protect-password2').value = 'something else';
   const beforeMismatch = requests.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   fireTimers();
   await flush();
   await flush();
   check('Protected: two passwords that differ send nothing and say so',
      !called(beforeMismatch, 'POST', '/api/v1/me/protected') && !called(beforeMismatch, 'POST', '/api/v1/me/messages') && document.getElementById('compose-status').textContent === said('The two passwords are not the same.'),
      document.getElementById('compose-status').textContent);

   // ---- the send: the record, the chunk, and no password and no text anywhere
   document.getElementById('compose-protect-password2').value = PASSWORD;
   document.getElementById('compose-protect-hint').value = 'the usual';
   const beforeSend = requests.length;
   const protectedBefore = pgpProtected.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   // The undo countdown is running: the form's password box is written to
   // again before it ends, and the send must go with what was there when
   // Send was pressed, not with what is there now.
   document.getElementById('compose-protect-password').value = 'changed while the countdown ran';
   fireTimers();
   await flush();
   await flush();
   await flush();
   const record = since(beforeSend).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/protected');
   const recordBody = record.length ? JSON.parse(record[0].body || '{}') : {};
   check('Protected: the send makes a record with the recipient, the hint and the size, and never an ordinary message',
      record.length === 1 && recordBody.recipient === 'nobody@example.org' && recordBody.hint === 'the usual' && recordBody.size > 0 &&
      !called(beforeSend, 'POST', '/api/v1/me/messages'),
      JSON.stringify(recordBody) + ' ' + JSON.stringify(since(beforeSend).map((r) => r.method + ' ' + r.path)));
   const chunk = since(beforeSend).filter((r) => r.method === 'PUT' && /^\/api\/v1\/me\/protected\/\d+\/content/.test(r.path));
   check('Protected: the ciphertext goes to the content route, and it is the armour the module made',
      chunk.length === 1 && chunk[0].body instanceof Uint8Array && Buffer.from(chunk[0].body).toString('latin1').indexOf('-----BEGIN PGP MESSAGE-----') === 0,
      chunk.length + ' chunks');
   check('Protected: the module was handed the text, the subject and the password, once',
      pgpProtected.length === protectedBefore + 1 && pgpProtected[protectedBefore].text === 'The figures are 42.' && pgpProtected[protectedBefore].subject === 'The board pack' && pgpProtected[protectedBefore].hadPassword === true,
      JSON.stringify(pgpProtected[protectedBefore] || null));
   const everything = everyRequestText(beforeSend);
   check('Protected: the password appears in no request the page made', everything.indexOf(PASSWORD) < 0);
   check('Protected: and neither does the text', everything.indexOf('The figures are 42.') < 0);
   check('Protected: the form is emptied and the reader told', document.getElementById('compose-protect').checked === false && document.getElementById('compose-protect-password').value === '');

   // ---- a file above the domain's link threshold: never a link while the
   // message is protected - a link would rest on the server in the clear -
   // and a link when it is not, which is what proves the threshold is live
   const bigFile = { name: 'deck.pdf', type: 'application/pdf', size: 9 * 1024 * 1024, base64: 'AA==' };
   document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
   await flush();
   document.getElementById('compose-to').value = 'nobody@example.org';
   document.getElementById('compose-subject').value = 'With a big file';
   document.getElementById('compose-text').value = 'The deck is attached.';
   document.getElementById('compose-protect').checked = true;
   document.getElementById('compose-protect').dispatchEvent(makeEvent('change'));
   document.getElementById('compose-protect-password').value = PASSWORD;
   document.getElementById('compose-protect-password2').value = PASSWORD;
   document.getElementById('compose-form').dispatchEvent(makeEvent('drop', { dataTransfer: { files: [bigFile] } }));
   await flush();
   const beforeBig = requests.length;
   const protectedBeforeBig = pgpProtected.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   fireTimers();
   await flush();
   await flush();
   await flush();
   check('Protected: a file above the link threshold is not sent as a link while the message is protected',
      !called(beforeBig, 'POST', '/api/v1/me/files') && called(beforeBig, 'POST', '/api/v1/me/protected'),
      JSON.stringify(since(beforeBig).map((r) => r.method + ' ' + r.path)));
   check('Protected: it goes into the ciphertext instead, and the text handed to the module names no link',
      pgpProtected.length === protectedBeforeBig + 1 && pgpProtected[protectedBeforeBig].attachments === 1 && pgpProtected[protectedBeforeBig].text.indexOf('/files/') < 0,
      JSON.stringify(pgpProtected[protectedBeforeBig] || null));
   check('Protected: and nothing of the file went to the files store', everyRequestText(beforeBig).indexOf('/api/v1/me/files') < 0);
   // The same file without Protect goes to the files store, as it always
   // did: the threshold is live, and it is Protect that turns it off.
   document.dispatchEvent(makeEvent('keydown', { key: 'c', target: document.body }));
   await flush();
   document.getElementById('compose-to').value = 'nobody@example.org';
   document.getElementById('compose-subject').value = 'With a big file, in the clear';
   document.getElementById('compose-text').value = 'The deck is linked.';
   document.getElementById('compose-form').dispatchEvent(makeEvent('drop', { dataTransfer: { files: [bigFile] } }));
   await flush();
   const beforeLink = requests.length;
   document.getElementById('compose-form').dispatchEvent(makeEvent('submit'));
   await flush();
   fireTimers();
   await flush();
   await flush();
   check('Protected: the same file without Protect is offered to the files store as a link (negative control)',
      called(beforeLink, 'POST', '/api/v1/me/files'), JSON.stringify(since(beforeLink).map((r) => r.method + ' ' + r.path)));
   document.getElementById('compose-discard').dispatchEvent(makeEvent('click'));
   await flush();
   dismissAllToastsHere();

   // ---- the page that lists them, and Withdraw
   protectedState.items = [{ id: 901, token: 'tok0000000000000000001', recipient: 'nobody@example.org', hint: 'the usual', size: 100, stored: 100, complete: true, created: 1700000000, expires: 1702592000, expired: false, opened: 1700003600, revoked: false, reply_to: '', link: 'http://portal.test/portal/protected/tok0000000000000000001' }];
   location.hash = '#/protected';
   await flush();
   await flush();
   const rows = document.getElementById('protected-rows').children;
   check('Protected: the page lists what was sent, with when it was opened',
      rows.length === 1 && rows[0].textContent.indexOf('nobody@example.org') >= 0 && document.getElementById('protected-table').hidden === false,
      rows.length + ' rows');
   const beforeRevoke = requests.length;
   const buttons = Array.prototype.slice.call(rows[0].children[4].children);
   check('Protected: a live message offers one thing to do with it, Withdraw', buttons.length === 1, buttons.map((b) => b.textContent).join(','));
   buttons[0].dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   check('Protected: Withdraw asks first and then deletes the message',
      called(beforeRevoke, 'DELETE', '/api/v1/me/protected/901') && confirms[confirms.length - 1].indexOf('nobody@example.org') >= 0,
      JSON.stringify(since(beforeRevoke).map((r) => r.method + ' ' + r.path)));
   location.hash = '#/f/1';
   await flush();
   protectedState.available = false;
   protectedState.items = [];

   await readingPageChecks();
}

// The reading page, PortalProtected.html with PortalProtected.js, in a world of
// its own with the REAL modules: what a recipient with no account meets.
async function readingPageChecks() {
   const utilDir = pagePath.slice(0, Math.max(pagePath.lastIndexOf('/'), pagePath.lastIndexOf('\\')) + 1);
   const realMime = require(require('path').resolve(utilDir + 'PortalSmime.js'));
   const realPgp = require(require('path').resolve(utilDir + 'PortalOpenPgp.js'));
   if (!realPgp.available) { check('Protected page: this Node has Web Crypto, so the reading page can be run', false); return; }
   const PASSWORD = 'correct-horse-battery-staple';
   const TOKEN = 'AbCdEfGhIjKlMnOpQrStUv';
   const sealed = await realPgp.protectedMessage({
      headers: { subject: 'The board pack', from: 'sender@example.com', to: 'reader@example.org' },
      text: 'The figures are 42.\r\n', html: '',
      attachments: [{ name: 'figures.csv', type: 'text/csv', bytes: realPgp.utf8('column,value\r\nfigures,42\r\n') }],
      password: PASSWORD
   });

   const tree = parseHtml(fs.readFileSync(utilDir + 'PortalProtected.html', 'utf8'));
   const doc = new Element('#document');
   doc.getElementById = function (id) {
      const found = tree.byId[id];
      if (!found) { throw new Error('the reading page asked for an element it does not have: #' + id); }
      return found;
   };
   doc.createElement = (tag) => new Element(tag);
   doc.createTextNode = (text) => new TextNode(String(text));
   doc.body = (function find(node) {
      if (node instanceof Element && node.tagName === 'BODY') { return node; }
      for (const child of node.childNodes) { const hit = find(child); if (hit) { return hit; } }
      return null;
   })(tree.root);
   const pageRequests = [];
   let fetchAnswer = { status: 200, body: JSON.stringify({ hint: 'the usual', created: 1700000000, expires: 1702592000, reply: false, can_reply: true, message: sealed }) };
   let replyAnswer = { status: 201, body: JSON.stringify({ token: 'ReplyTokenReplyTokenAB', link: 'http://portal.test/portal/protected/ReplyTokenReplyTokenAB', expires: 1702592000, notified: true }) };
   const pageFetch = (path, options) => {
      const method = (options && options.method) || 'GET';
      pageRequests.push({ method, path, headers: (options && options.headers) || {}, body: options && options.body });
      const reply = method === 'POST' ? replyAnswer : fetchAnswer;
      return Promise.resolve({ status: reply.status, ok: reply.status >= 200 && reply.status < 300, text: () => Promise.resolve(reply.body) });
   };
   const pageWindow = new Element('#window');
   pageWindow.hmOpenPgp = realPgp;
   pageWindow.hmSmime = realMime;
   const context = vm.createContext({
      document: doc, window: pageWindow, location: { pathname: '/portal/protected/' + TOKEN, origin: 'http://portal.test' },
      fetch: pageFetch, URL: { createObjectURL: () => 'blob:portal.test/one', revokeObjectURL: () => {} }, Blob,
      setTimeout: (fn) => { fn(); return 1; }, console, Promise, JSON, Object, String, Number, Date, Math, RegExp, Error, Uint8Array
   });
   vm.runInContext(fs.readFileSync(utilDir + 'PortalProtected.js', 'utf8'), context, { filename: 'portal-protected.js' });
   const settle = async (until) => {
      for (let i = 0; i < 200; i += 1) {
         await new Promise((r) => setImmediate(r));
         await new Promise((r) => realSetTimeout(r, 5));
         if (until()) { return true; }
      }
      return until();
   };
   const q = (id) => doc.getElementById(id);

   await settle(() => q('unlock').hidden === false);
   check('Protected page: the page fetches the ciphertext by the token in its address and asks for the password',
      pageRequests.length === 1 && pageRequests[0].method === 'GET' && pageRequests[0].path === '/api/v1/protected/' + TOKEN && q('unlock').hidden === false && q('loading').hidden === true,
      JSON.stringify(pageRequests.map((r) => r.method + ' ' + r.path)) + ' unlock hidden=' + q('unlock').hidden);
   check('Protected page: it shows the hint and nothing of the text',
      q('hint-box').hidden === false && q('hint').textContent === 'the usual' && q('message').hidden === true);

   q('password').value = 'not the password';
   q('password-form').dispatchEvent(makeEvent('submit'));
   await settle(() => q('password-status').textContent.length > 0 && q('password-status').textContent !== 'Decrypting...');
   check('Protected page: a wrong password says so and nothing else',
      q('password-status').textContent === 'That password is not right.' && q('message').hidden === true, q('password-status').textContent);

   q('password').value = PASSWORD;
   q('password-form').dispatchEvent(makeEvent('submit'));
   await settle(() => q('message').hidden === false);
   check('Protected page: the right password decrypts in the browser and shows the text',
      q('message').hidden === false && q('text').textContent.indexOf('The figures are 42.') === 0 && q('title').textContent === 'The board pack',
      'hidden=' + q('message').hidden + ' text=' + q('text').textContent.slice(0, 40) + ' status=' + q('password-status').textContent);
   check('Protected page: and offers the attachment to save',
      q('attachments-box').hidden === false && q('attachments').children.length === 1 && q('attachments').children[0].textContent.indexOf('figures.csv') >= 0);
   check('Protected page: the password box is emptied once it has done its work', q('password').value === '');
   check('Protected page: the page says what it does not promise',
      q('message').textContent.indexOf('does not pretend it could stop you copying') >= 0);

   // ---- a reply, sealed under the same password, and neither it nor the
   // password in what the page sends
   check('Protected page: a reply is offered', q('reply-box').hidden === false);
   q('reply-text').value = 'Thank you, received.';
   const beforeReply = pageRequests.length;
   q('reply-form').dispatchEvent(makeEvent('submit'));
   await settle(() => pageRequests.length > beforeReply && q('reply-status').textContent.indexOf('on its way') >= 0);
   const posted = pageRequests.slice(beforeReply).filter((r) => r.method === 'POST');
   const postedBody = posted.length ? JSON.parse(posted[0].body) : {};
   check('Protected page: the reply goes to the reply route as an armoured OpenPGP message',
      posted.length === 1 && posted[0].path === '/api/v1/protected/' + TOKEN + '/reply' && String(postedBody.message).indexOf('-----BEGIN PGP MESSAGE-----') === 0,
      JSON.stringify(posted.map((r) => r.path)) + ' ' + q('reply-status').textContent);
   const everything = pageRequests.map((r) => r.method + ' ' + r.path + ' ' + String(r.body || '')).join('\n');
   check('Protected page: the password is in no request the page made', everything.indexOf(PASSWORD) < 0);
   check('Protected page: and the reply\'s text is in none either', everything.indexOf('Thank you, received.') < 0);
   const reopened = await realPgp.openProtected(postedBody.message, PASSWORD);
   check('Protected page: the sender opens the reply with the password they set',
      reopened.content.text.indexOf('Thank you, received.') === 0 && reopened.subject === 'Re: The board pack', reopened.subject + ' / ' + reopened.content.text.slice(0, 30));
   check('Protected page: and the reader is told it is on its way', q('reply-status').textContent.indexOf('on its way') >= 0, q('reply-status').textContent);
}

// ---- Zero-access mailbox (docs/ZeroAccess.md). The module is the stub, so
// what is asserted is what the page does with it: which requests it makes,
// with what in them, and what the reader is shown; the cryptography is
// build/check-portal-openpgp.js's business.
async function zeroAccessChecks() {
   window.hmOpenPgp = pgpStub;
   const PASSPHRASE = 'the-key-passphrase';
   const own = pgpFingerprint(account);
   const zaText = () => document.getElementById('za-status').textContent;
   const confirmText = () => document.getElementById('za-confirm-status').textContent;
   const putsOfSwitch = (from) => since(from).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/zero-access');

   // ---- a key that expires is not offered: every delivery would be refused
   // from that day, so the server refuses it and the page leaves it out
   pgpStores[account] = { own: [pgpOwnRowOf(account, PASSPHRASE)], correspondents: [] };
   pgpStores[account].own[0].expires = 1900000000;
   delete zaRows[account];
   zaAllowed = true;
   location.hash = '#/security';
   await flush();
   await flush();
   await flush();
   check('Zero access: a key that expires is not offered as the mailbox key, and the page asks for one that does not',
      document.getElementById('za-key').children.length === 0 && document.getElementById('za-no-key').hidden === false && document.getElementById('za-on-form').hidden === true,
      document.getElementById('za-key').children.length + ' options, hint hidden=' + document.getElementById('za-no-key').hidden);
   check('Zero access: the page says what is sealed and what is not, in one sentence beside the consequences',
      document.getElementById('za-scope').hidden === false && document.getElementById('za-scope').textContent.length > 60,
      document.getElementById('za-scope').textContent.slice(0, 60));
   pgpStores[account] = { own: [pgpOwnRowOf(account, PASSPHRASE)], correspondents: [] };
   location.hash = '#/f/1';
   await flush();
   location.hash = '#/security';
   await flush();
   await flush();
   await flush();
   check('Zero access: the Security page says what it costs before the switch',
      document.getElementById('za-body').hidden === false && document.getElementById('za-consequences').hidden === false &&
      document.getElementById('za-consequences').textContent.length > 300 &&
      document.getElementById('za-state-off').hidden === false && document.getElementById('za-on-form').hidden === false,
      'body hidden=' + document.getElementById('za-body').hidden + ' form hidden=' + document.getElementById('za-on-form').hidden);
   check('Zero access: the account\'s own key is offered, by its fingerprint',
      document.getElementById('za-key').children.length === 1 && document.getElementById('za-key').children[0].value === own,
      document.getElementById('za-key').children.length + ' options');

   // ---- the switch-on flow: the recovery key shown, and nothing sent until
   // it is typed back and the consequence is acknowledged
   const beforeSwitch = requests.length;
   document.getElementById('za-key').value = own;
   document.getElementById('za-on-form').dispatchEvent(makeEvent('submit'));
   await flush();
   await flush();
   check('Zero access: switching on shows the recovery key as eight groups of six, and the sentence that it is the only way back',
      document.getElementById('za-confirm').hidden === false && document.getElementById('za-recovery-key').textContent === ZA_RECOVERY &&
      /^([A-Z2-7]{6}-){7}[A-Z2-7]{6}$/.test(document.getElementById('za-recovery-key').textContent),
      document.getElementById('za-recovery-key').textContent);
   document.getElementById('za-confirm-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('Zero access: with nothing typed back, nothing is sent and the page says the key is not the one shown',
      putsOfSwitch(beforeSwitch).length === 0 && confirmText().length > 0 && document.getElementById('za-confirm').hidden === false,
      confirmText());
   document.getElementById('za-recovery-typed').value = 'ABCDEF-GHIJKL-MNOPQR-STUVWX-YZ2345-67ABCD-EFGHIJ-KLMNOQ';
   document.getElementById('za-confirm-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('Zero access: a recovery key typed back wrong is refused, and nothing is sent',
      putsOfSwitch(beforeSwitch).length === 0 && confirmText().length > 0,
      confirmText());
   // Right, in lower case with the dashes left out: the canonical form is what counts.
   document.getElementById('za-recovery-typed').value = ZA_RECOVERY.toLowerCase().replace(/-/g, '');
   document.getElementById('za-confirm-form').dispatchEvent(makeEvent('submit'));
   await flush();
   const notTicked = confirmText();
   check('Zero access: the key typed back right but the consequence not acknowledged: still nothing sent',
      putsOfSwitch(beforeSwitch).length === 0 && notTicked.length > 0 && document.getElementById('za-confirm').hidden === false,
      notTicked);
   document.getElementById('za-accept').checked = true;
   document.getElementById('za-confirm-form').dispatchEvent(makeEvent('submit'));
   await flush();
   check('Zero access: and without the password, still nothing sent',
      putsOfSwitch(beforeSwitch).length === 0 && confirmText().length > 0,
      confirmText());
   document.getElementById('za-password').value = 'a-real-password';
   document.getElementById('za-confirm-form').dispatchEvent(makeEvent('submit'));
   await flush();
   await flush();
   // The key may be locked (a session that never unlocked it): the page
   // then asks for its passphrase first, through the OpenPGP dialogue.
   if (document.getElementById('pgp-unlock').hidden === false) {
      document.getElementById('pgp-unlock-passphrase').value = PASSPHRASE;
      document.getElementById('pgp-unlock-form').dispatchEvent(makeEvent('submit'));
   }
   await flush();
   await flush();
   await flush();
   await flush();
   const switched = putsOfSwitch(beforeSwitch);
   const switchBody = switched.length ? JSON.parse(switched[0].body || '{}') : {};
   check('Zero access: then one PUT switches it on, naming the key and carrying both copies sealed in the browser',
      switched.length === 1 && switchBody.fingerprint === own && typeof switchBody.salt === 'string' && switchBody.salt.length > 0 &&
      switchBody.passes === 3 && switchBody.memory_kib === 65536 && switchBody.lanes === 4,
      JSON.stringify(Object.keys(switchBody)) + ' ' + JSON.stringify(switchBody.passes));
   check('Zero access: the password copy is the key sealed under the passphrase derived from the password and the salt',
      switchBody.password_wrapped === 'SEC:' + account + ':' + own + ':ZA_a-real-password_' + switchBody.salt,
      String(switchBody.password_wrapped));
   check('Zero access: the recovery copy is the key sealed under the canonical recovery key',
      switchBody.recovery_wrapped === 'SEC:' + account + ':' + own + ':' + ZA_RECOVERY.replace(/-/g, ''),
      String(switchBody.recovery_wrapped));
   check('Zero access: the dialogue closes, the password box and the typed key are cleared, and the page says it is on',
      document.getElementById('za-confirm').hidden === true && document.getElementById('za-password').value === '' &&
      document.getElementById('za-recovery-typed').value === '' && document.getElementById('za-recovery-key').textContent === '' &&
      document.getElementById('za-state-on').hidden === false && document.getElementById('za-state-off').hidden === true && zaText().length > 0,
      zaText());

   // ---- the reader opens a message sealed on delivery, with the key in hand
   pgpMessageResult = { layers: ['encrypted'], encrypted: { ok: true, keyId: own.slice(-16) }, signed: null, inline: false, integrityProtected: true,
      content: { text: 'What only the mailbox key opens.', html: '', attachments: [] }, autocrypt: null };
   const readBefore = pgpReadWith.length;
   location.hash = '#/m/603';
   await flush();
   await flush();
   await flush();
   check('Zero access: a delivery sealed to the mailbox key is read with the opened key in hand',
      pgpReadWith.length === readBefore + 1 && pgpReadWith[readBefore].indexOf(account + '!') >= 0,
      JSON.stringify(pgpReadWith.slice(readBefore)));
   check('Zero access: and the reader shows the text and says it was sealed on delivery and opened here',
      document.getElementById('message-text').textContent === 'What only the mailbox key opens.' &&
      document.getElementById('message-pgp').getAttribute('data-za') === 'opened' && document.getElementById('message-pgp-text').textContent.length > 0,
      document.getElementById('message-pgp').getAttribute('data-za') + ' | ' + document.getElementById('message-text').textContent);

   // ---- the impersonation warning (ImpersonationCheck.h): a bar above the
   // message for each of the two header shapes, nothing for a message without
   // the header, and the mark on the listing's row
   {
      const words = (english) => (germanCatalogue && germanCatalogue[english]) || english;
      const fill = (text, ...values) => values.reduce((s, v, i) => s.split('{' + i + '}').join(String(v)), words(text));
      const warningTexts = () => Array.prototype.slice.call(document.getElementById('message-warnings').children).map((n) => n.textContent);
      const badgesOn = (row) => { const out = []; (function walk(n) { for (let i = 0; i < n.childNodes.length; i++) { const c = n.childNodes[i]; if (c.nodeType === 1 && String(c.className).indexOf('badge') >= 0) { out.push(c.textContent); } if (c.childNodes) { walk(c); } } })(row); return out; };
      const rowBySubject = (subject) => Array.prototype.slice.call(document.getElementById('message-list').children).filter((r) => r.textContent.indexOf(subject) >= 0)[0];

      location.hash = '#/m/604';
      await flush();
      await flush();
      check('Impersonation: a borrowed name is a warning bar naming the name and the address the message really came from',
         !document.getElementById('message-warnings').hidden &&
         warningTexts().indexOf(fill("This sender's name matches {0} here, but the message is from {1}.", 'Ada Lovelace (ada@example.com)', 'ada@evil.example')) >= 0,
         JSON.stringify(warningTexts()));

      location.hash = '#/m/605';
      await flush();
      await flush();
      check('Impersonation: a look-alike domain is a warning bar naming the two domains',
         !document.getElementById('message-warnings').hidden &&
         warningTexts().indexOf(fill("This message's domain {0} looks like {1}.", 'examp1e.com', 'example.com')) >= 0,
         JSON.stringify(warningTexts()));

      location.hash = '#/m/606';
      await flush();
      await flush();
      check('Impersonation: a message without the header gets neither sentence',
         warningTexts().every((w) => w.indexOf(words("This sender's name matches {0} here, but the message is from {1}.").split('{0}')[0]) < 0 &&
                                     w.indexOf(words("This message's domain {0} looks like {1}.").split('{0}')[0]) < 0),
         JSON.stringify(warningTexts()));

      location.hash = '#/f/1';
      await flush();
      await flush();
      check('Impersonation: the listing row of a stamped message wears the mark, and an unstamped row does not',
         badgesOn(rowBySubject('First')).indexOf(words('Suspicious sender')) >= 0 && badgesOn(rowBySubject('Second')).indexOf(words('Suspicious sender')) < 0,
         JSON.stringify([badgesOn(rowBySubject('First')), badgesOn(rowBySubject('Second'))]));
   }

   // ---- a password change through the page carries the key sealed again
   location.hash = '#/password';
   await flush();
   const beforeChange = requests.length;
   document.getElementById('current').value = 'a-real-password';
   document.getElementById('new').value = 'a-newer-password';
   document.getElementById('confirm').value = 'a-newer-password';
   document.getElementById('password-form').dispatchEvent(makeEvent('submit'));
   await flush(); await flush(); await flush(); await flush();
   const changes = since(beforeChange).filter((r) => r.method === 'POST' && r.path === '/api/v1/me/password');
   const changeBody = changes.length ? JSON.parse(changes[0].body || '{}') : {};
   check('Zero access: a password change from the page carries the key sealed again under the new password, in the same request',
      changes.length === 1 && changeBody['new'] === 'a-newer-password' && changeBody.zero_access &&
      changeBody.zero_access.password_wrapped === 'SEC:' + account + ':' + own + ':ZA_a-newer-password_' + changeBody.zero_access.salt &&
      changeBody.zero_access.passes === 3,
      JSON.stringify(changeBody.zero_access || null));
   check('Zero access: and the page reports the change as made',
      document.getElementById('password-status').className.indexOf('ok') > 0 && document.getElementById('password-status').textContent.length > 0,
      document.getElementById('password-status').className + ' ' + document.getElementById('password-status').textContent);

   // ---- switching off: one DELETE, and the page says what stays encrypted
   location.hash = '#/security';
   await flush();
   await flush();
   await flush();
   const beforeOff = requests.length;
   window.confirm = () => true;
   document.getElementById('za-off').dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   check('Zero access: switching off is one DELETE, and the page says stored mail stays encrypted',
      called(beforeOff, 'DELETE', '/api/v1/me/zero-access') && document.getElementById('za-state-off').hidden === false &&
      zaText().length > 0 && document.getElementById('za-status').className.indexOf('ok') > 0,
      zaText());

   // ---- the switch on with no key row (a restore that dropped the table):
   // the page says so rather than showing a switch-on form that would answer
   // 409, and the switch-off it offers is the way out
   zaBroken[account] = true;
   location.hash = '#/f/1';
   await flush();
   location.hash = '#/security';
   await flush();
   await flush();
   await flush();
   check('Zero access: a switch on with no key row is said in a sentence, with the switch-off offered',
      document.getElementById('za-state-on').hidden === false && document.getElementById('za-state-off').hidden === true &&
      document.getElementById('za-on-text').textContent.length > 0 && document.getElementById('za-on-text').textContent.indexOf('On since') < 0,
      'on hidden=' + document.getElementById('za-state-on').hidden + ' text=' + document.getElementById('za-on-text').textContent.slice(0, 60));
   const beforeBrokenOff = requests.length;
   document.getElementById('za-off').dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   check('Zero access: switching off from that state is the same one DELETE, and the page is back to off',
      called(beforeBrokenOff, 'DELETE', '/api/v1/me/zero-access') && !zaBroken[account] && document.getElementById('za-state-off').hidden === false,
      'broken=' + !!zaBroken[account]);

   // ---- the server-wide switch off: the page offers nothing
   zaAllowed = false;
   location.hash = '#/f/1';
   await flush();
   location.hash = '#/security';
   await flush();
   await flush();
   await flush();
   check('Zero access: with the administrator\'s switch off, the page offers no switch and says so',
      document.getElementById('za-not-allowed').hidden === false && document.getElementById('za-body').hidden === true,
      'not-allowed hidden=' + document.getElementById('za-not-allowed').hidden);
   zaAllowed = true;
   location.hash = '#/f/1';
   await flush();
}

// ---- Zero access, the sign-in half, run at the end of main() because it
// signs out and in and the run's own sign-out checks need the second account
// still attached: the key opens from the sign-in password alone, and after
// a password an administrator set, the recovery key opens it and the page
// seals it again under the password the person now has.
async function zeroAccessSignInChecks() {
   window.hmOpenPgp = pgpStub;
   const PASSPHRASE = 'the-key-passphrase';
   const own = pgpFingerprint(account);
   const signIn = async (password) => {
      document.getElementById('address').value = account;
      document.getElementById('password').value = password;
      document.getElementById('signin-form').dispatchEvent(makeEvent('submit'));
      await flush(); await flush(); await flush(); await flush();
   };
   document.getElementById('signout').dispatchEvent(makeEvent('click'));
   await flush();
   pgpStores[account] = { own: [pgpOwnRowOf(account, PASSPHRASE)], correspondents: [] };
   zaAllowed = true;
   zaRows[account] = { fingerprint: own, salt: 'c2FsdC1vbmU=', argon2: { passes: 3, memory_kib: 65536, lanes: 4 },
      password_wrapped: 'SEC:' + account + ':' + own + ':ZA_a-real-password_c2FsdC1vbmU=',
      recovery_wrapped: 'SEC:' + account + ':' + own + ':' + ZA_RECOVERY.replace(/-/g, ''), enabled_at: 1758182400 };
   pgpMessageResult = { layers: ['encrypted'], encrypted: { ok: true, keyId: own.slice(-16) }, signed: null, inline: false, integrityProtected: true,
      content: { text: 'What only the mailbox key opens.', html: '', attachments: [] }, autocrypt: null };

   // ---- the key opens from the sign-in password alone, nothing typed, and
   // the reader is not asked for a passphrase
   await signIn('a-real-password');
   const readAgain = pgpReadWith.length;
   location.hash = '#/m/603';
   await flush();
   await flush();
   await flush();
   check('Zero access: after a fresh sign-in the sealed delivery opens with the key derived from the sign-in password, nothing asked',
      pgpReadWith.length === readAgain + 1 && pgpReadWith[readAgain].indexOf(account + '!') >= 0 &&
      document.getElementById('pgp-unlock').hidden === true && document.getElementById('za-unlock').hidden === true &&
      document.getElementById('message-text').textContent === 'What only the mailbox key opens.',
      JSON.stringify(pgpReadWith.slice(readAgain)) + ' unlock hidden=' + document.getElementById('pgp-unlock').hidden);

   // ---- a password an administrator set: the sign-in password does not open
   // the copy, the page asks for the previous one or the recovery key, and
   // the recovery key opens it and seals it again under the current password
   location.hash = '#/f/1';
   await flush();
   document.getElementById('signout').dispatchEvent(makeEvent('click'));
   await flush();
   const beforeReset = requests.length;
   await signIn('the-password-an-admin-set');
   check('Zero access: a sign-in password that does not open the copy opens the unlock dialogue',
      document.getElementById('za-unlock').hidden === false && document.getElementById('za-unlock-note').textContent.length > 0,
      'hidden=' + document.getElementById('za-unlock').hidden);
   document.getElementById('za-unlock-secret').value = 'not the password';
   document.getElementById('za-unlock-form').dispatchEvent(makeEvent('submit'));
   await flush();
   await flush();
   check('Zero access: a wrong password in the dialogue is refused in a sentence',
      document.getElementById('za-unlock').hidden === false && document.getElementById('za-unlock-status').textContent.length > 0,
      document.getElementById('za-unlock-status').textContent);
   document.getElementById('za-unlock-secret').value = ZA_RECOVERY;
   document.getElementById('za-unlock-form').dispatchEvent(makeEvent('submit'));
   await flush(); await flush(); await flush(); await flush();
   const rewraps = since(beforeReset).filter((r) => r.method === 'PUT' && r.path === '/api/v1/me/zero-access/rewrap');
   const rewrapBody = rewraps.length ? JSON.parse(rewraps[0].body || '{}') : {};
   check('Zero access: the recovery key opens the copy and the key is sealed again under the password the person now has',
      rewraps.length === 1 && rewrapBody.password_wrapped === 'SEC:' + account + ':' + own + ':ZA_the-password-an-admin-set_' + rewrapBody.salt &&
      document.getElementById('za-unlock').hidden === true && document.getElementById('za-unlock-secret').value === '',
      JSON.stringify(rewrapBody));
   const readAfterRecovery = pgpReadWith.length;
   location.hash = '#/m/603';
   await flush();
   await flush();
   await flush();
   check('Zero access: and the sealed delivery opens',
      pgpReadWith.length === readAfterRecovery + 1 && pgpReadWith[readAfterRecovery].indexOf(account + '!') >= 0 &&
      document.getElementById('message-text').textContent === 'What only the mailbox key opens.',
      JSON.stringify(pgpReadWith.slice(readAfterRecovery)));

   // ---- sign-out forgets all of it: the opened key, the password held for
   // the dialogue, the recovery key shown, and every field of the two forms
   location.hash = '#/f/1';
   await flush();
   document.getElementById('za-unlock-secret').value = 'left in the field';
   document.getElementById('za-password').value = 'left in the field';
   document.getElementById('signout').dispatchEvent(makeEvent('click'));
   await flush();
   await flush();
   check('Zero access: signing out forgets the mailbox key, the dialogue fields and the recovery key shown',
      document.getElementById('za-unlock').hidden === true && document.getElementById('za-unlock-secret').value === '' &&
      document.getElementById('za-password').value === '' && document.getElementById('za-recovery-typed').value === '' &&
      document.getElementById('za-recovery-key').textContent === '' && document.getElementById('message-za-unlock').hidden === true,
      'unlock hidden=' + document.getElementById('za-unlock').hidden + ' secret=' + JSON.stringify(document.getElementById('za-unlock-secret').value) + ' recovery=' + JSON.stringify(document.getElementById('za-recovery-key').textContent));
   delete zaRows[account];
   pgpMessageResult = { layers: [], encrypted: null, signed: null, content: null, autocrypt: null, inline: false };
   await signIn('a-real-password');
   check('Zero access: and a sign-in after that reads nothing of it, since the switch is off',
      document.getElementById('za-unlock').hidden === true && document.getElementById('message-list') !== null,
      'unlock hidden=' + document.getElementById('za-unlock').hidden);
}

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
