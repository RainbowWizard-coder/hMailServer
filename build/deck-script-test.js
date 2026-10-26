// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The Control Deck's script, executed. build/check-deck-script.py lifts the one
// inline script out of hmailserver/installation/WebAdmin/index.html and hands
// the page and the script here; this builds a small DOM from the markup,
// answers fetch out of a recorded server whose state the writes change, runs
// the script in that world and then asserts what the administrator would see:
// each view drawn from its answers, each form sending the JSON its route takes,
// each refusal shown in the server's own words, each change read back after
// the round trip.
//
// The DOM is deliberately small and hand-written, as the portal's is: no
// dependency, so this runs on a bare runner with nothing installed, and nothing
// in it is magic - a call the script makes that is not here fails with its
// name rather than passing quietly. What the Deck needs beyond the portal:
// innerHTML, because every view is drawn by assigning it; a selector engine
// for the shapes the page uses (#id, .class, tag, [attr], [attr=value], the
// descendant and child combinators, a comma); closest; dataset; on<event>
// handler properties beside addEventListener; a select whose value is its
// selected option; and entity decoding, because everything the server says
// goes through escHtml before it is drawn and the assertions want it back.
// Timers are a queue this file fires by hand, so the restart's polling loop is
// tested without waiting a minute.

'use strict';

const fs = require('fs');
const nodePath = require('path');
const vm = require('vm');

const out = console;

// The Deck's catalogues, which the recorded server answers at
// /deck-lang/<code>.json as the real one does from the directory beside the
// page: the files in the repository, so a check here runs against the
// translations that ship.
const CATALOGUES = nodePath.join(__dirname, '..', 'hmailserver', 'installation', 'WebAdmin', 'languages');
// The documentation the help pane shows: the repository's hmailserver/docs,
// which the server embeds (build/generate-help-pages.py) and answers at
// /api/v1/help and /api/v1/help/<page>. The list's headings are read with
// the webmail's renderer, which agrees with the generator's parser; the
// pages are the files. The Deck carries a copy of the renderer inline.
const DOCS = nodePath.join(__dirname, '..', 'hmailserver', 'docs');
const markdown = require(nodePath.join(__dirname, '..', 'hmailserver', 'source', 'Server', 'Common', 'Util', 'PortalMarkdown.js'));
// The page of the wiki made from each document, as the server lists it in
// /api/v1/help as the page's wiki_page: build/help-wiki-pages.txt, each line
// a page of the wiki and the documents made into it (build/generate-help-pages.py).
const wikiPageOf = (() => {
   const table = {};
   fs.readFileSync(nodePath.join(__dirname, 'help-wiki-pages.txt'), 'utf8').split(/\r?\n/).forEach((line) => {
      const fields = line.replace(/#.*/, '').trim().split(/\s+/).filter(Boolean);
      fields.slice(1).forEach((doc) => { table[doc.replace(/\.md$/, '')] = fields[0]; });
   });
   return (name) => table[name] || '';
})();
// The documents the server leaves out of its help: generate-help-pages.py's
// EXCLUDED, read from the generator as build/check-help-map.py reads it, so that
// the list here cannot keep a set of its own.
const HELP_EXCLUDED = (() => {
   const generator = fs.readFileSync(nodePath.join(__dirname, 'generate-help-pages.py'), 'utf8');
   const set = /^EXCLUDED\s*=\s*\{([^}]*)\}/m.exec(generator);
   const names = set ? (set[1].match(/"[^"]+"/g) || []).map((s) => s.slice(1, -1)) : [];
   if (!names.length) {
      out.error('build/generate-help-pages.py: its EXCLUDED set could not be read');
      process.exit(2);
   }
   return new Set(names);
})();
// The pages the server itself embeds, from the table generate-help-pages.py writes into HelpPagesData.cpp: what the
// list above is held to. A row is read by its head, its name to its headings, whatever follows (a wiki page, say).
const HELP_ROW = /^\s*\{ "([A-Za-z0-9]+)", \d+, Page\d+Pieces, Page\d+Headings\b/;
const HELP_EMBEDDED = fs.readFileSync(nodePath.join(__dirname, '..', 'hmailserver', 'source', 'Server', 'Common', 'Util',
   'HelpPagesData.cpp'), 'utf8').split(/\r?\n/).map((line) => HELP_ROW.exec(line)).filter(Boolean).map((m) => m[1]);
const helpPages = () => fs.readdirSync(DOCS).filter((f) => f.endsWith('.md') && !HELP_EXCLUDED.has(f)).sort()
   .map((f) => {
      const body = fs.readFileSync(nodePath.join(DOCS, f), 'utf8').replace(/\r\n/g, '\n');
      const heads = markdown.headings(body);
      return { name: f.slice(0, -3), title: heads.length && heads[0].level === 1 ? heads[0].text : f.slice(0, -3), bytes: Buffer.byteLength(body),
         wiki_page: wikiPageOf(f.slice(0, -3)), headings: heads };
   });

const [pagePath, scriptPath] = process.argv.slice(2);
if (!pagePath || !scriptPath) {
   out.error('usage: node build/deck-script-test.js <index.html> <deck.js>');
   process.exit(2);
}

/* ------------------------------------------------------------------ the DOM */

const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source', 'track', 'wbr']);
const RAW = new Set(['script', 'style']);
const NAMED = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ', mdash: '—', ndash: '–', hellip: '…' };

function decodeEntities(text) {
   if (text.indexOf('&') < 0) { return text; }
   return text.replace(/&(#x[0-9a-fA-F]+|#\d+|[a-zA-Z]+);/g, (whole, body) => {
      if (body[0] === '#') {
         const code = body[1] === 'x' || body[1] === 'X' ? parseInt(body.slice(2), 16) : parseInt(body.slice(1), 10);
         return isFinite(code) ? String.fromCodePoint(code) : whole;
      }
      return body in NAMED ? NAMED[body] : whole;
   });
}

function encodeText(text) {
   return String(text).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// Every scrollIntoView the page asks for, counted: the help pane's scroll is
// what the checks of the pane hold still, and what they expect once.
let scrollsIntoView = 0;

class TextNode {
   constructor(data) { this.data = data; this.parentNode = null; this.childNodes = []; this.nodeType = 3; }
   get textContent() { return this.data; }
   set textContent(v) { this.data = String(v); }
   get nodeValue() { return this.data; }
   set nodeValue(v) { this.data = String(v); }
}

/* Selectors: a list of chains, a chain a list of compounds from ancestor to
   target, each compound a tag, an id, classes and attribute tests. */
function parseCompound(text) {
   const c = { tag: null, id: null, classes: [], attrs: [] };
   let rest = text;
   const re = /^(?:([a-zA-Z][\w-]*)|#([\w-]+)|\.([\w-]+)|\[([\w-]+)(?:=(?:"([^"]*)"|'([^']*)'|([^\]]*)))?\]|(\*))/;
   while (rest.length) {
      const m = re.exec(rest);
      if (!m) { throw new Error('a selector this DOM does not understand: ' + text); }
      if (m[1]) { c.tag = m[1].toUpperCase(); }
      else if (m[2]) { c.id = m[2]; }
      else if (m[3]) { c.classes.push(m[3]); }
      else if (m[4]) { c.attrs.push({ name: m[4], value: m[5] !== undefined ? m[5] : m[6] !== undefined ? m[6] : m[7] !== undefined ? m[7] : null }); }
      rest = rest.slice(m[0].length);
   }
   return c;
}

function parseSelector(text) {
   return String(text).split(',').map((part) => {
      const chain = [];
      let child = false;
      part.trim().split(/\s+/).filter(Boolean).forEach((token) => {
         if (token === '>') { child = true; return; }
         chain.push({ compound: parseCompound(token), child });
         child = false;
      });
      if (!chain.length) { throw new Error('an empty selector: ' + text); }
      return chain;
   });
}

function matchesCompound(node, c) {
   if (!(node instanceof Element) || node.tagName[0] === '#') { return false; }
   if (c.tag && node.tagName !== c.tag) { return false; }
   if (c.id && node.attributes.id !== c.id) { return false; }
   const classes = node.className ? node.className.split(/\s+/) : [];
   if (c.classes.some((k) => classes.indexOf(k) < 0)) { return false; }
   return c.attrs.every((a) => (a.value === null ? a.name in node.attributes : node.attributes[a.name] === a.value));
}

function matchesChain(node, chain) {
   let at = chain.length - 1;
   if (!matchesCompound(node, chain[at].compound)) { return false; }
   let here = node;
   while (at > 0) {
      const link = chain[at];
      at -= 1;
      here = here.parentNode;
      if (link.child) {
         if (!matchesCompound(here, chain[at].compound)) { return false; }
      } else {
         while (here && !matchesCompound(here, chain[at].compound)) { here = here.parentNode; }
         if (!here) { return false; }
      }
   }
   return true;
}

function walk(node, visit) {
   node.childNodes.forEach((child) => {
      if (child instanceof Element) { visit(child); walk(child, visit); }
   });
}

function serialize(node) {
   if (node instanceof TextNode) { return encodeText(node.data); }
   const tag = node.tagName.toLowerCase();
   const attrs = Object.keys(node.attributes).map((k) => ' ' + k + '="' + String(node.attributes[k]).replace(/&/g, '&amp;').replace(/"/g, '&quot;') + '"').join('');
   if (VOID.has(tag)) { return '<' + tag + attrs + '>'; }
   return '<' + tag + attrs + '>' + node.childNodes.map(serialize).join('') + '</' + tag + '>';
}

class Element {
   constructor(tag) {
      this.tagName = String(tag).toUpperCase();
      this.nodeName = this.tagName;
      this.nodeType = 1;
      this.attributes = Object.create(null);
      this.childNodes = [];
      this.parentNode = null;
      this.listeners = Object.create(null);
      this.style = {};
      this.hidden = false;
      this.checked = false;
      this.disabled = false;
      this.scrollTop = 0;
      this.scrollHeight = 0;
      this.focused = 0;
      this._value = undefined;
   }
   get id() { return this.attributes.id || ''; }
   set id(v) { this.attributes.id = String(v); }
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
   get dataset() {
      const self = this;
      const name = (key) => 'data-' + String(key).replace(/[A-Z]/g, (c) => '-' + c.toLowerCase());
      return new Proxy({}, {
         get: (_, key) => (typeof key === 'string' ? self.attributes[name(key)] : undefined),
         set: (_, key, v) => { self.attributes[name(key)] = String(v); return true; },
         has: (_, key) => name(key) in self.attributes
      });
   }
   /* A select answers with its selected option, a textarea with its text, and
      anything assigned wins over both - as in a browser. */
   get value() {
      if (this._value !== undefined) { return this._value; }
      if (this.tagName === 'SELECT') {
         const options = [];
         walk(this, (n) => { if (n.tagName === 'OPTION') { options.push(n); } });
         const chosen = options.filter((o) => 'selected' in o.attributes)[0] || options[0];
         return chosen ? ('value' in chosen.attributes ? chosen.attributes.value : chosen.textContent) : '';
      }
      if (this.tagName === 'TEXTAREA') { return this.textContent; }
      return 'value' in this.attributes ? this.attributes.value : '';
   }
   set value(v) { this._value = String(v); }
   get textContent() {
      return this.childNodes.map((n) => (n instanceof Element ? n.textContent : n.data)).join('');
   }
   set textContent(v) {
      this.childNodes.forEach((n) => { n.parentNode = null; });
      this.childNodes = [];
      if (v !== '' && v !== null && v !== undefined) { this.appendChild(new TextNode(String(v))); }
   }
   get innerHTML() { return this.childNodes.map(serialize).join(''); }
   set innerHTML(text) {
      this.childNodes.forEach((n) => { n.parentNode = null; });
      this.childNodes = [];
      parseHtml(String(text)).root.childNodes.slice().forEach((n) => this.appendChild(n));
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
      if (name === 'value') { this._value = String(value); }
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
         const own = node['on' + event.type];
         if (typeof own === 'function') { own.call(node, event); }
         (node.listeners[event.type] || []).slice().forEach((fn) => fn.call(node, event));
         if (event.propagationStopped) { return !event.defaultPrevented; }
         node = node.parentNode;
      }
      return !event.defaultPrevented;
   }
   matches(selector) { return parseSelector(selector).some((chain) => matchesChain(this, chain)); }
   closest(selector) {
      let node = this;
      while (node instanceof Element) {
         if (node.matches(selector)) { return node; }
         node = node.parentNode;
      }
      return null;
   }
   querySelectorAll(selector) {
      const chains = parseSelector(selector);
      const found = [];
      walk(this, (n) => { if (chains.some((chain) => matchesChain(n, chain))) { found.push(n); } });
      return found;
   }
   querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
   focus() { this.focused += 1; document.activeElement = this; }
   // In the document, as a browser's isConnected says: a node removed with
   // its container is not, and has no focus to be given.
   get isConnected() { let n = this; while (n.parentNode) { n = n.parentNode; } return n === document; }
   /* There is no layout in this DOM, so an element has the rectangle a test
      gives it (el.rect = {left, top, width, height}) and otherwise none at all.
      That is enough for the tour, which asks only where the thing it points at
      is; an element with no rectangle is one the tour draws no ring around,
      which is exactly what a browser reports for something not on the screen. */
   getBoundingClientRect() {
      const r = this.rect || { left: 0, top: 0, width: 0, height: 0 };
      return { left: r.left, top: r.top, width: r.width, height: r.height, x: r.left, y: r.top,
               right: r.left + r.width, bottom: r.top + r.height };
   }
   get offsetHeight() { return (this.rect || { height: 0 }).height; }
   click() { this.dispatchEvent(makeEvent('click')); }
   blur() { }
   select() { }
   scrollIntoView() { scrollsIntoView += 1; }
}

function makeEvent(type, extra) {
   return Object.assign({
      type,
      defaultPrevented: false,
      propagationStopped: false,
      preventDefault() { this.defaultPrevented = true; },
      stopPropagation() { this.propagationStopped = true; }
   }, extra || {});
}

function parseHtml(text) {
   const root = new Element('#document-fragment');
   const stack = [root];
   let i = 0;
   const top = () => stack[stack.length - 1];
   while (i < text.length) {
      const lt = text.indexOf('<', i);
      if (lt < 0) {
         if (i < text.length) { top().appendChild(new TextNode(decodeEntities(text.slice(i)))); }
         break;
      }
      if (lt > i) { top().appendChild(new TextNode(decodeEntities(text.slice(i, lt)))); }
      if (text.startsWith('<!--', lt)) { i = text.indexOf('-->', lt); i = i < 0 ? text.length : i + 3; continue; }
      if (text.startsWith('<!', lt)) { i = text.indexOf('>', lt); i = i < 0 ? text.length : i + 1; continue; }
      const gt = text.indexOf('>', lt);
      if (gt < 0) { break; }
      const inside = text.slice(lt + 1, gt);
      i = gt + 1;
      if (inside.startsWith('/')) {
         const name = inside.slice(1).trim().toUpperCase();
         for (let n = stack.length - 1; n > 0; n -= 1) {
            if (stack[n].tagName === name) { stack.length = n; break; }
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
         const raw = match[2] !== undefined ? match[2] : match[3] !== undefined ? match[3] : match[4] !== undefined ? match[4] : '';
         element.attributes[match[1]] = decodeEntities(raw);
      }
      if ('hidden' in element.attributes) { element.hidden = true; }
      if ('checked' in element.attributes) { element.checked = true; }
      if ('disabled' in element.attributes) { element.disabled = true; }
      if ('type' in element.attributes) { element.type = element.attributes.type; }
      if ('style' in element.attributes) {
         element.attributes.style.split(';').forEach((rule) => {
            const colon = rule.indexOf(':');
            if (colon > 0) { element.style[rule.slice(0, colon).trim()] = rule.slice(colon + 1).trim(); }
         });
      }
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
   return { root };
}

const parsed = parseHtml(fs.readFileSync(pagePath, 'utf8'));

const document = new Element('#document');
document.hidden = false;
document.activeElement = null;
parsed.root.childNodes.slice().forEach((n) => document.appendChild(n));
document.documentElement = document.querySelector('html');
document.body = document.querySelector('body');
if (!document.body) { throw new Error('the page has no <body>'); }
document.getElementById = function (id) {
   let found = null;
   walk(document, (n) => { if (!found && n.attributes.id === id) { found = n; } });
   return found;
};
document.createElement = function (tag) { return new Element(tag); };
// The help pane's renderer builds text nodes rather than assigning markup,
// which is the whole of why a document can carry nothing the page runs.
document.createTextNode = function (text) { return new TextNode(String(text)); };

/* ------------------------------------------------ timers, storage, the rest */

const timers = [];
const intervals = [];
let timerId = 1;
function fireTimers() {
   const due = timers.splice(0, timers.length);
   due.forEach((t) => t.fn());
   return due.length;
}
function fireIntervals() { intervals.slice().forEach((t) => t.fn()); }

function storage() {
   const store = new Map();
   return {
      store,
      getItem: (k) => (store.has(k) ? store.get(k) : null),
      setItem: (k, v) => store.set(k, String(v)),
      removeItem: (k) => store.delete(k),
      get length() { return store.size; }
   };
}
const localStorage = storage();
const sessionStorage = storage();

let clock = 0;
const performance = { now: () => { clock += 1; return clock; } };

/* The system's colour preference, and the listeners the page attaches to it:
   the page's theme has three choices - light, dark and the system's - and
   without this the third could only be guessed at. setSystemTheme changes the
   preference and tells the listeners, which is what a reader turning their
   desktop dark does. */
let systemPrefersDark = true;
const mediaListeners = [];
function matchMedia(query) {
   return {
      media: query,
      get matches() { return String(query).indexOf('dark') >= 0 ? systemPrefersDark : !systemPrefersDark; },
      addEventListener: (type, fn) => { if (type === 'change') { mediaListeners.push(fn); } },
      removeEventListener: () => { }
   };
}
function setSystemTheme(dark) {
   systemPrefersDark = dark;
   mediaListeners.slice().forEach((fn) => fn({ matches: dark }));
}

const confirmations = [];
let confirmAnswer = true;
const consoleErrors = [];
let clipboard = null;

/* ------------------------------------------------------- the recorded server */

const requests = [];
let signedIn = false;
// What /api/v1/signin-methods answers; a check below switches single sign-on
// off to see both buttons go.
let deckSignInMethods = { passkeys: true, oidc: true, oidc_label: 'login.example.com', saml: true, saml_label: 'Contoso single sign-on' };
let otpRequired = false;
let nextRefusal = null;
// GET /api/v1/warmup answers 500 once when set: the queue page must stand.
let warmupFails = false;
// A refusal with a status and a body of its own, for the ones that carry a
// reason word beside the sentence: the breach check's 400 and 503.
let nextRefusalAnswer = null;
// Why a mailbox cannot be moved at this moment, or nothing: the server's own
// answer to GET /api/v1/accounts/{address}/move, which the page greys the
// button out for.
let moveBlockedReason = null;
let restartProbes = 0;
let nextId = 100;
// The language route's two failure shapes: a server without the catalogues
// installed, and a catalogue that lacks a key the page has.
let catalogueMissing = false;
let droppedKey = null;
// A catalogue as a file on the server's disk could hold it, which is not the
// file the build checked: a function that changes the answer, or null.
let catalogueTamper = null;
// The wiki base /api/v1/help answers with, and the spam-trap counter the hits
// route answers with (null: the number of hits it holds) - both changed by the
// checks that a value from the server is drawn only as what it must be.
let helpWikiBase = 'https://wiki.test/';
let trapHitsSinceStart = null;

const RULES_POST = 'Body: name (required, at most 100 characters, no control characters: it is written into a 550 reply and into the headers of a policy notice), active (default true), all_criteria (default true: every criterion must match; false: any one), criteria and actions as arrays of objects in the order they run. A criterion: field (from, to, cc, subject, body, message_size, recipient_list, delivery_attempts, attachment_name, sensitive_content, attachment_type, external_recipient, content, or header with the header\'s name in header), match (equals, not_equals, contains, not_contains, less_than, greater_than, regex, wildcard) and value (at most 2000 characters; a regex must compile). An action: type and the parameters that type takes - forward: to and abort_spam_flagged; reply: from_name, from_address (left out or empty for the null sender, as the Control Panel saves it: the reply\'s From is then the name before an empty address, or the empty address alone when the name holds one), subject, body and abort_spam_flagged; move_to_folder: folder; script_function: script_function (the function of the event script, by its plain name - ASCII letters, digits, _ and $, not starting with a digit, without brackets or arguments; any other name is refused, since the server would never run it); set_header: header and value; send_using_route: route_id; bind_to_address: value; add_disclaimer: body (the text appended to the message\'s plain and HTML parts); reject: value (one line, no control characters: the text of the 550 the SMTP session answers with, before the message is accepted, when the rule matches there, and of the 403 policy_rejected the self-service send route answers with before it queues; a message that reached delivery another way is dropped, and a sender this server hosts is mailed a notice saying it was not sent); warn_sender: value (the text of a notice mailed to the sender naming the rule, once per message on its first delivery attempt and not again on a release from the quarantine, only when this server hosts the sender\'s domain - the message itself is not held by this action); require_tls, quarantine, delete, stop and copy take none. The sensitive_content condition is the detectors that found something in the readable text, with ; between them: card_number (13 to 19 digits that pass the Luhn check), iban (a country in the IBAN registry, its length and check digits) and account_number (the dlp_account_number_pattern setting\'s regular expression); attachment_type is every attachment\'s type by its first bytes rather than its name (executable, elf, pdf, zip, office_xml, jar, office, rar, 7z, gzip, png, jpeg, gif, rtf, script, text, empty, binary); external_recipient is every recipient this server does not host; content is the readable text itself - the plain and HTML parts and every attachment whose bytes are text - for a pattern of the rule\'s own; a sealed message declines all but attachment_type and external_recipient, as the body condition does. value is also accepted as the parameter the listing shows under that name. An unknown key, word or type, a parameter the type does not take, or a missing one it needs, is refused naming it. Saved as the Control Panel saves a rule; it applies to the next message delivered. Server-wide; refused for domain-restricted keys.';

const ROUTE_PROPS = {
   domain_name: { type: 'string' }, description: { type: 'string' }, target_smtp_host: { type: 'string' },
   target_smtp_port: { type: 'integer' }, number_of_tries: { type: 'integer' }, minutes_between_try: { type: 'integer' },
   relayer_requires_authentication: { type: 'boolean' }, relayer_auth_username: { type: 'string' },
   relayer_auth_password: { type: 'string', writeOnly: true },
   treat_recipient_as_local_domain: { type: 'boolean' }, treat_security_as_local_domain: { type: 'boolean', deprecated: true },
   treat_sender_as_local_domain: { type: 'boolean' }, all_addresses: { type: 'boolean' },
   addresses: { type: 'array', items: { type: 'string' } },
   connection_security: { type: 'string', enum: ['none', 'starttls_optional', 'starttls_required', 'tls'] }
};
const ROUTES_POST = 'Body: domain_name and target_smtp_host (required); target_smtp_port (default 25), number_of_tries (default 3), minutes_between_try (default 10), relayer_requires_authentication (default false, and then relayer_auth_username is required), relayer_auth_username, relayer_auth_password (write-only), treat_recipient_as_local_domain or treat_security_as_local_domain (default false), treat_sender_as_local_domain (default false), all_addresses (default true), addresses (an array of e-mail addresses) and connection_security (default none). Persisted and put into effect exactly as a route saved in the Control Panel is: the next message to the domain uses it. Server-wide; refused for domain-restricted keys.';

const REMOTE_DOMAIN_PROPS = {
   domain_name: { type: 'string' }, description: { type: 'string' }, active: { type: 'boolean' },
   outbound_tls: { type: 'string', enum: ['none', 'encrypted', 'verified', 'dane'] },
   require_inbound_tls: { type: 'boolean' },
   max_message_size_kb: { type: 'integer' }, max_connections: { type: 'integer' }, max_messages_per_minute: { type: 'integer' },
   allow_automatic_replies: { type: 'boolean' }, allow_forwarding: { type: 'boolean' },
   callout_enabled: { type: 'boolean' }, callout_host: { type: 'string' }, callout_port: { type: 'integer' },
   callout_timeout_seconds: { type: 'integer' }, callout_cache_minutes: { type: 'integer' }, callout_max_per_minute: { type: 'integer' },
   encrypt: { type: 'string', enum: ['none', 'openpgp', 'smime'] },
   encrypt_key: { type: 'string', format: 'multiline', writeOnly: true, description: 'The partner\'s key.' },
   encrypt_fallback: { type: 'string', enum: ['refuse', 'send_unencrypted'] },
   encrypt_key_fingerprint: { type: 'string', readOnly: true }, encrypt_key_expires: { type: 'string', readOnly: true },
   encrypt_key_problem: { type: 'string', readOnly: true }
};
const REMOTE_DOMAINS_POST = 'Body: domain_name (required; a domain, or a wildcard pattern such as *.example.com - the most specific active match governs a delivery). description; active (default true); outbound_tls (default none); require_inbound_tls (default false); max_message_size_kb (default 0, no limit); max_connections and max_messages_per_minute (default 0, unlimited); allow_automatic_replies and allow_forwarding (default true); callout_enabled (default false), callout_host, callout_port (default 25), callout_timeout_seconds (default 10, at most 60), callout_cache_minutes (default 60) and callout_max_per_minute (default 10). encrypt (default none - openpgp encrypts mail to this domain at this server to the partner\'s OpenPGP public key as PGP/MIME, smime to the partner\'s certificate as S/MIME enveloped-data); encrypt_key (write-only: the partner\'s key; a PUT that leaves it out keeps the key held); encrypt_fallback (default refuse - a message that cannot be encrypted fails permanently with 5.7.10; send_unencrypted sends it as it was and logs it). Server-wide; refused for domain-restricted keys.';

const PORT_PROPS = {
   protocol: { type: 'string', enum: ['smtp', 'pop3', 'imap'] }, address: { type: 'string' }, port: { type: 'integer' },
   connection_security: { type: 'string', enum: ['none', 'tls', 'starttls_optional', 'starttls_required'] },
   certificate_id: { type: 'integer' },
   client_certificate_policy: { type: 'string', enum: ['off', 'request', 'require'] },
   client_certificate_ca_file: { type: 'string' },
   smtp_authentication: { type: 'string', enum: ['on', 'off', 'honeypot'] }
};
const PORTS_POST = 'The listeners are created when the server starts, so a port added here takes effect when the server restarts. address defaults to 0.0.0.0, connection_security to none, certificate_id to 0, client_certificate_policy to off and smtp_authentication to on. tls and both starttls values need a certificate_id, and a client certificate policy other than off needs a CA file and a security that runs a handshake - the same refusals the Control Panel meets, in the same words. Server-wide; refused for domain-restricted keys.';
const PORTS_PUT = 'The whole record, with the fields and defaults of POST: a field left out takes its default, so send back what GET returned with the change made. Takes effect when the server restarts. Server-wide; refused for domain-restricted keys.';
const PORTS_DELETE = 'The row is gone at once; the listener stays up until the server restarts. Server-wide; refused for domain-restricted keys.';
const REINITIALIZE = 'What the Control Panel\'s Reinitialize does: every service is stopped, the configuration reloaded and the services started again in the same process, so that a port, a certificate binding or a setting the document marks as taking effect on restart takes effect now. Answers before it happens, because the REST listener itself restarts: poll GET /api/v1/status until it answers again. Server-wide; refused for domain-restricted and read-only keys.';

const CERT_PROPS = {
   name: { type: 'string' }, certificate_file: { type: 'string' }, private_key_file: { type: 'string' },
   private_key_password: { type: 'string', writeOnly: true }
};

// The domain's keys as ApplyDomainBody in RestApiAdministration.cpp reads
// them, and its description, which names three keys in the schema and the
// rest in prose.
/* Two clients of one account: a phone fetching mail over IMAP on an app
   password, and the same person's browser on the account's own password. Two
   rows, because a client is the protocol, the address and the user agent. */
const devices = [
   { id: '0123456789abcdef0123456789abcdef', protocol: 'imap', address: '203.0.113.9', agent: '',
     credential: 'app_password', credential_name: 'Phone', first_seen: '2026-09-01 08:00:00',
     last_seen: '2026-09-16 07:30:00', sign_ins: 412, revoked: false, revoked_at: '' },
   { id: 'fedcba9876543210fedcba9876543210', protocol: 'web', address: '198.51.100.4',
     agent: 'Mozilla/5.0 (X11; Linux x86_64)', credential: 'password', credential_name: '',
     first_seen: '2026-09-14 11:02:00', last_seen: '2026-09-16 09:15:00', sign_ins: 6,
     revoked: false, revoked_at: '' }
];

const DOMAIN_KEYS = ['name', 'active', 'postmaster', 'max_message_size_kb', 'max_size_mb', 'max_account_size_mb',
   'max_accounts', 'max_aliases', 'max_lists', 'max_accounts_enabled', 'max_aliases_enabled', 'max_lists_enabled',
   'plus_addressing_enabled', 'plus_addressing_character', 'use_greylisting',
   'signature_enabled', 'signature_method', 'signature_plain_text', 'signature_html', 'signature_add_to_replies', 'signature_add_to_local_mail',
   'dkim_enabled', 'dkim_selector', 'dkim_private_key_file', 'dkim_signing_algorithm',
   'dkim_sign_aliases', 'dkim_rotation_enabled', 'dkim_rotation_interval_days', 'dkim_rotation_propagation_hours',
   'dkim_rotation_retention_days', 'dkim_rotation_key_bits',
   'message_retention_days', 'relay_host', 'relay_port', 'relay_requires_auth', 'relay_username', 'relay_password', 'relay_connection_security',
   'vacation_enabled', 'vacation_subject', 'vacation_message',
   'vacation_internal_subject', 'vacation_internal_message', 'vacation_external_override', 'vacation_days',
   'quota_warning_percent', 'quota_warning_subject', 'quota_warning_text', 'quota_grace_percent', 'quota_send_stop_percent',
   'external_tag_subject', 'external_tag_header', 'external_tag_text', 'first_contact_tip',
   'disclaimer_enabled', 'disclaimer_plain_text', 'disclaimer_html',
   'quarantine_digest', 'quarantine_digest_hour',
   'openpgp_publish', 'openpgp_publish_new_accounts', 'country_policy', 'session_idle_seconds', 'session_lifetime_seconds', 'session_one_per_device', 'session_bind_to_address',
   'report_mailbox', 'restricted_domains', 'impersonation_action', 'protected_persons', 'protected_partners'];
const DOMAIN_PUT = 'Body: active (required) and any subset of postmaster, name (a new name renames the domain and every address in it, as the Control Panel does), max_message_size_kb, max_size_mb, max_account_size_mb, max_accounts, max_aliases, max_lists and their switches max_accounts_enabled, max_aliases_enabled, max_lists_enabled, plus_addressing_enabled, plus_addressing_character, use_greylisting, signature_enabled, signature_method (set_if_not_specified, overwrite or append), signature_plain_text, signature_html, signature_add_to_replies, signature_add_to_local_mail, dkim_enabled, dkim_selector, dkim_private_key_file, dkim_signing_algorithm (sha1 or sha256), dkim_sign_aliases, dkim_rotation_enabled, dkim_rotation_interval_days, dkim_rotation_propagation_hours, dkim_rotation_retention_days and dkim_rotation_key_bits (the key rotation policy), message_retention_days, relay_host, relay_port, relay_requires_auth, relay_username, relay_password (write-only), relay_connection_security, vacation_enabled, vacation_subject, vacation_message, external_tag_subject, external_tag_header, external_tag_text, first_contact_tip, disclaimer_enabled, disclaimer_plain_text, disclaimer_html, quarantine_digest, quarantine_digest_hour, openpgp_publish (publish this domain\'s accounts\' OpenPGP public keys in the Web Key Directory) and openpgp_publish_new_accounts (what an account created in this domain from now on starts with), country_policy (which countries may sign in to the accounts of the domain: empty judges nothing, allow:GB,IE admits only those countries, block:CN,RU refuses those), session_idle_seconds, session_lifetime_seconds, session_one_per_device and session_bind_to_address (the session policy for the accounts of this domain), and report_mailbox (the address this domain\'s DMARC aggregate and TLS-RPT reports are posted to and read from; one address in a domain this server hosts, or empty). A field left out keeps its value; everything is checked before anything is applied, and an unknown field or a wrong type is a 400 naming it.';
const DOMAINS_POST = 'Body: name (required), active (default true) and postmaster. The name is judged as the Control Panel judges it - a valid domain name, not one a domain alias already has - and every other setting takes the default a new domain gets there. Server-wide; refused for domain-restricted keys.';

// The account's update schema as RestApiRoutes.cpp emits it - every field
// typed, the admin level's three words - and its description; the GET answers
// the same keys less the password.
const ACCOUNT_PROPS = {
   active: { type: 'boolean' }, password: { type: 'string' }, max_size_mb: { type: 'integer' }, first_name: { type: 'string' }, last_name: { type: 'string' },
   forward_enabled: { type: 'boolean' }, forward_address: { type: 'string' }, forward_keep_original: { type: 'boolean' },
   signature_enabled: { type: 'boolean' }, signature_plain_text: { type: 'string' }, signature_html: { type: 'string' },
   admin_level: { type: 'string', enum: ['user', 'domain', 'server'] },
   antispam_enabled: { type: 'boolean' }, spam_mark_threshold: { type: 'integer' }, spam_delete_threshold: { type: 'integer' },
   forward_abort_spam_flagged: { type: 'boolean' }, message_retention_days: { type: 'integer' },
   vacation_enabled: { type: 'boolean' }, vacation_subject: { type: 'string' }, vacation_message: { type: 'string' },
   vacation_expires: { type: 'boolean' }, vacation_expires_date: { type: 'string' }, vacation_begin_date: { type: 'string' }, vacation_abort_spam_flagged: { type: 'boolean' },
   vacation_days: { type: 'integer' }, vacation_internal_subject: { type: 'string' }, vacation_internal_message: { type: 'string' }, vacation_internal_only: { type: 'boolean' },
   ad_enabled: { type: 'boolean' }, ad_domain: { type: 'string' }, ad_username: { type: 'string' }, sieve_script: { type: 'string' },
   openpgp_publish: { type: 'boolean' }, restricted_delivery: { type: 'integer' },
   kind: { type: 'string', enum: ['person', 'room', 'equipment'] }
};
const ACCOUNT_PUT = 'Any subset of active, password, max_size_mb, first_name, last_name, forward_enabled, forward_address, forward_keep_original, signature_enabled, signature_plain_text, signature_html, admin_level (user, domain or server), antispam_enabled, spam_mark_threshold and spam_delete_threshold (-1 is the server\'s own value), forward_abort_spam_flagged, message_retention_days (-1 keeps forever, 0 follows the domain), vacation_enabled, vacation_subject, vacation_message, vacation_expires, vacation_expires_date and vacation_begin_date (YYYY-MM-DD; empty means today, as the Control Panel takes them), vacation_abort_spam_flagged, ad_enabled, ad_domain, ad_username, sieve_script and openpgp_publish (publish this account\'s OpenPGP public keys in the Web Key Directory); a field the body does not name is left as it is, and an unknown field is refused by name. Scoped to the address\'s domain.';
// An account as AccountEntryJson answers it: every field, never the password.
function accountRecord(address, active) {
   return { address, active, max_size_mb: 0, first_name: '', last_name: '', forward_enabled: false, forward_address: '', forward_keep_original: false,
      signature_enabled: false, signature_plain_text: '', signature_html: '', vacation_enabled: false, vacation_subject: '', vacation_message: '',
      vacation_expires: false, vacation_expires_date: '', message_retention_days: 0, admin_level: 'user', antispam_enabled: true,
      spam_mark_threshold: -1, spam_delete_threshold: -1, forward_abort_spam_flagged: false, vacation_begin_date: '', vacation_abort_spam_flagged: false,
      vacation_days: 0, vacation_internal_subject: '', vacation_internal_message: '', vacation_internal_only: false,
      ad_enabled: false, ad_domain: '', ad_username: '', sieve_script: '', openpgp_publish: false, restricted_delivery: 0, kind: 'person',
      // Read-only, and the address masked as every surface masks it: the four the
      // account GET carries since AccountRecovery.h arrived.
      recovery_address_set: true, recovery_address_verified: true, recovery_address: 'o....@e.........example',
      recovery_codes_unused: 7 };
}

// The distribution list's schemas and descriptions as RestApiServer.cpp emits
// them: the create takes the address and the members, the update neither.
const LIST_PROPS = {
   active: { type: 'boolean' }, require_auth: { type: 'boolean' }, mode: { type: 'string', enum: ['public', 'membership', 'announcement', 'domain_members'] },
   require_sender_address: { type: 'string' }, moderator_address: { type: 'string' }, bounce_address: { type: 'string' }, membership: { type: 'string' }
};
const LIST_CREATE_PROPS = Object.assign({ address: { type: 'string' }, members: { type: 'array', items: { type: 'string' } } }, LIST_PROPS);
const LISTS_POST = 'Body: address (required, in this domain), members (an array of addresses), active (default true), require_auth (default false), mode (public, membership, announcement or domain_members; default public, and any other word is refused as put_Mode refuses it), require_sender_address (the one sender an announcement list accepts), moderator_address, bounce_address and membership. membership is the list\'s query, evaluated when a message is sent to it and never stored: empty (the members are the rows in members, which is every list made before this existed), domain (every active account of this domain), domain:NAME (every active account of that domain, which this server has to host), pattern:WILDCARD (active accounts whose address matches it, with * for any run of characters and ? for one; the part after the @ has to name a domain this server hosts unless it is itself a wildcard) or everyone (every active account of every active domain). What it answers is in union with members and deduplicated by address, and a query list still honours require_auth, require_sender_address, mode and moderator_address. A key confined to named domains may set empty, domain, or a pattern in a domain it holds; the other forms reach accounts it does not administer and are refused. Saved as the Control Panel saves a list, with the same limitation check. Scoped to the domain.';
const LIST_PUT = 'Body: any subset of active, require_auth, mode (public, membership, announcement or domain_members), require_sender_address, moderator_address, bounce_address and membership; a field left out keeps its value, and the members are not changed here. membership takes the five forms the create documents, and an empty string turns a query list back into a static one - either way its rows are untouched. Everything is checked before anything is applied. Scoped to the address\'s domain.';

// The seven small collections as RestApiAntiSpamLists.cpp and
// RestApiBlockedAttachments.cpp describe them: the create's schema, its
// required keys and the three descriptions; the update takes any subset of
// the same keys. The entries the recorded server starts with are below.
const DNS_LIST_PROPS = { dns_host: { type: 'string' }, expected_result: { type: 'string' }, reject_message: { type: 'string' }, score: { type: 'integer' }, active: { type: 'boolean' } };
const COLLECTION_SPECS = {
   '/api/v1/dns-blacklists': { props: DNS_LIST_PROPS, required: ['dns_host'], noun: 'DNS black list',
      get: 'AntiSpam.DNSBlackLists over COM. Each entry: id, active, dns_host, expected_result, reject_message, score. Server-wide; refused for domain-restricted keys.',
      post: 'Body: dns_host (required, the zone queried), expected_result (the answers that mean listed, 127.0.0.2 or a range or wildcard), reject_message, score and active (default true). Saved as a list saved in the Control Panel is, and consulted for the next message. Server-wide; refused for domain-restricted and read-only keys.' },
   '/api/v1/surbl-servers': { props: DNS_LIST_PROPS, required: ['dns_host'], noun: 'SURBL server',
      get: 'AntiSpam.SURBLServers over COM. Each entry: id, active, dns_host, expected_result, reject_message, score. Server-wide; refused for domain-restricted keys.',
      post: 'Body: dns_host (required, the zone the domains in a message are looked up in), expected_result (the answers that mean listed; empty is any answer but a refusal code), reject_message, score and active (default true). Saved as one saved in the Control Panel is, and consulted for the next message. Server-wide; refused for domain-restricted and read-only keys.' },
   '/api/v1/whitelist-addresses': { props: { lower_ip: { type: 'string' }, upper_ip: { type: 'string' }, email_address: { type: 'string' }, description: { type: 'string' } }, required: ['lower_ip', 'upper_ip'], noun: 'white-list address',
      get: 'AntiSpam.WhiteListAddresses over COM: the senders and address ranges the spam tests skip. Each entry: id, lower_ip, upper_ip, email_address, description. Server-wide; refused for domain-restricted keys.',
      post: 'Body: lower_ip and upper_ip (required, the address range), email_address (the sender, with wildcards; empty is any) and description. Saved as one saved in the Control Panel is; the white-list cache is reloaded for the next message. Server-wide; refused for domain-restricted and read-only keys.' },
   '/api/v1/blocked-senders': { props: { address: { type: 'string' }, score: { type: 'integer' }, description: { type: 'string' } }, required: ['address'], noun: 'blocked sender',
      get: 'AntiSpam.BlockedSenders over COM: the envelope senders, by address or by domain, that score. Each entry: id, address, score, description. Server-wide; refused for domain-restricted keys.',
      post: 'Body: address (required; an address, or a domain that covers its subdomains), score and description. Saved as one saved in the Control Panel is; the blocked-sender cache is reloaded for the next message. Server-wide; refused for domain-restricted and read-only keys.' },
   '/api/v1/blocked-attachments': { props: { wildcard: { type: 'string' }, description: { type: 'string' } }, required: ['wildcard'], noun: 'blocked attachment',
      get: 'AntiVirus.BlockedAttachments over COM: the file-name wildcards stripped from incoming messages when attachment_blocking_enabled is on in the anti-virus settings. Each entry: id, wildcard, description. Server-wide; refused for domain-restricted keys.',
      post: 'Body: wildcard (required; a file-name pattern such as *.exe) and description. Saved as one saved in the Control Panel is, and consulted for the next message scanned. Server-wide; refused for domain-restricted and read-only keys.' },
   '/api/v1/greylisting-white-addresses': { props: { ip_address: { type: 'string' }, description: { type: 'string' } }, required: ['ip_address'], noun: 'greylisting white address',
      get: 'AntiSpam.GreyListingWhiteAddresses over COM: the address patterns whose connections are never greylisted. Each entry: id, ip_address (with the wildcards as typed), description. Server-wide; refused for domain-restricted keys.',
      post: 'Body: ip_address (required; an address, or a pattern such as 192.168.* or 2001:db8:*) and description. Saved as one saved in the Control Panel is, and consulted for the next connection. Server-wide; refused for domain-restricted and read-only keys.' },
   '/api/v1/sender-list': { props: { value: { type: 'string' }, blocked: { type: 'boolean' }, reason: { type: 'string' }, expires: { type: 'boolean' }, expires_time: { type: 'string' } }, required: ['value'], noun: 'list entry',
      get: "The server's allow and block list: the addresses and domains wanted, or not wanted, by every account. Each entry: id, scope, value, blocked, reason, expires, expires_time, expired, created. Server-wide; refused for domain-restricted keys.",
      post: 'Body: value (required; an address, or a domain that covers its subdomains), blocked (default false), reason, expires (default false) and expires_time. A value the list already holds is rewritten rather than duplicated. Server-wide; refused for domain-restricted and read-only keys.' },
   '/api/v1/incoming-relays': { props: { name: { type: 'string' }, lower_ip: { type: 'string' }, upper_ip: { type: 'string' } }, required: ['name', 'lower_ip', 'upper_ip'], noun: 'incoming relay',
      get: 'Settings.IncomingRelays over COM: the address ranges whose Received headers are skipped when the real sender of a message is looked for. Each entry: id, name, lower_ip, upper_ip. Server-wide; refused for domain-restricted keys.',
      post: 'Body: name, lower_ip and upper_ip (all required). Saved as one saved in the Control Panel is, and in force for the next session. Server-wide; refused for domain-restricted and read-only keys.' },
   // The greylisting bypass list (RestApiGreylistBypass.cpp): the ninth, whose
   // match is an enumeration and whose three requirements are flags.
   '/api/v1/greylisting-bypass': { props: { pattern: { type: 'string' }, match: { type: 'string', enum: ['envelope', 'helo', 'both'] }, require_spf: { type: 'boolean' }, require_dkim: { type: 'boolean' }, require_dmarc: { type: 'boolean' }, note: { type: 'string' } }, required: ['pattern'], noun: 'greylisting bypass entry',
      get: 'AntiSpam.GreyListingBypassEntries over COM: the envelope domains and HELO suffixes let past greylisting when the message carries the authentication results the entry requires. Each entry: id, pattern, match (envelope, helo or both), require_spf, require_dkim, require_dmarc, note. Server-wide; refused for domain-restricted keys.',
      post: 'Body: pattern (required; a domain such as example.com, or a suffix such as .mail.example.com), match (envelope, helo or both; default envelope), require_spf, require_dkim, require_dmarc (default false each; at least one must be true) and note. Server-wide; refused for domain-restricted and read-only keys.' },
   // The domain white and black list (RestApiDomainPolicyList.cpp): the two
   // words are enums in the document, which the form draws as selects.
   '/api/v1/domain-policy-list': { props: { pattern: { type: 'string' }, match_on: { type: 'string', 'enum': ['envelope', 'helo', 'both'] }, action: { type: 'string', 'enum': ['white', 'black'] }, note: { type: 'string' } }, required: ['pattern'], noun: 'domain policy list entry',
      get: 'The sending organisations this server has decided about by domain rather than by address (AntiSpam.DomainPolicyList over COM). Each entry: id, pattern, match_on, action, note and created. Server-wide; refused for domain-restricted keys.',
      post: 'Body: pattern (required; a domain, or .domain for the domain and everything beneath it), match_on (envelope, helo or both; both by default), action (white or black; white by default) and note. The pair of pattern and match_on is unique. In force for the next session. Server-wide; refused for domain-restricted and read-only keys.' },
   // The never-ban list by reverse name (RestApiNeverBanNames.cpp): the
   // eleventh, a pattern and a note, and the one collection with a check.
   '/api/v1/never-ban-names': { props: { pattern: { type: 'string' }, note: { type: 'string' } }, required: ['pattern'], noun: 'never-ban list entry',
      get: 'Providers this server never auto-bans, named by the reverse names of their hosts (Settings.AutoBanNeverBanNames over COM). Each entry: id, pattern, note and created. Server-wide; refused for domain-restricted keys.',
      post: 'Body: pattern (required; a host name, or .domain for the domain and every name beneath it) and note. The pattern is unique. In force for the next ban. Server-wide; refused for domain-restricted and read-only keys.' }
};
const COLLECTION_PUT = 'Body: any subset of the fields POST takes; a field left out keeps its value. Nothing changes when the body is refused. Server-wide; refused for domain-restricted and read-only keys.';
function collectionPaths() {
   const out = {};
   Object.keys(COLLECTION_SPECS).forEach((path) => {
      const c = COLLECTION_SPECS[path];
      out[path] = { get: { summary: 'List the ' + c.noun + 's', description: c.get }, post: { summary: 'Add a ' + c.noun, description: c.post, requestBody: body(c.props, c.required) } };
      out[path + '/{id}'] = { put: { summary: 'Change a ' + c.noun, description: COLLECTION_PUT, requestBody: body(c.props) }, delete: { summary: 'Remove a ' + c.noun } };
   });
   return out;
}

// The IP range's schema as the ports and certificates literal emits it, every
// field typed; the update takes any subset of the same keys.
const RANGE_PROPS = {
   name: { type: 'string' }, lower: { type: 'string' }, upper: { type: 'string' }, priority: { type: 'integer' },
   allow_smtp: { type: 'boolean' }, allow_imap: { type: 'boolean' }, allow_pop3: { type: 'boolean' },
   deliver_local_to_local: { type: 'boolean' }, deliver_local_to_remote: { type: 'boolean' }, deliver_remote_to_local: { type: 'boolean' }, deliver_remote_to_remote: { type: 'boolean' },
   require_auth_local_to_local: { type: 'boolean' }, require_auth_local_to_remote: { type: 'boolean' }, require_auth_remote_to_local: { type: 'boolean' }, require_auth_remote_to_remote: { type: 'boolean' },
   require_tls_for_auth: { type: 'boolean' }, spam_protection: { type: 'boolean' }, virus_protection: { type: 'boolean' },
   expires: { type: 'boolean' }, expires_time: { type: 'string' }
};
const RANGE_PUT = 'Body: any subset of the fields POST takes - name, lower, upper, priority, the permission flags, expires and expires_time; a field left out keeps its value, and a body that turns expiry on must carry expires_time in it. The same check as saving the range in the Control Panel; nothing changes when it is refused. Server-wide; refused for domain-restricted and read-only keys.';
// The expiry as the two handlers read it: the time only with the flag, and
// the flag turned on only with a time; a time is YYYY-MM-DD HH:MM:SS.
function expiryProblem(parsed, was) {
   const time = String(parsed.expires_time || '').trim();
   const expires = 'expires' in parsed ? parsed.expires : !!(was && was.expires);
   if (time && !expires) { return 'expires_time is taken only when expires is true'; }
   if (time && !/^\d{4}-\d{2}-\d{2}( \d{2}:\d{2}:\d{2})?$/.test(time)) { return 'expires_time must be a date and time as YYYY-MM-DD HH:MM:SS'; }
   if (!time && expires && !(was && was.expires)) { return was ? 'expires_time is required when a range is made to expire' : 'expires_time is required when a range expires'; }
   return null;
}
// The defaults HandleCreateIpRange_ applies to a flag left out.
const RANGE_CREATE_DEFAULTS = { allow_smtp: true, allow_imap: true, allow_pop3: true,
   deliver_local_to_local: true, deliver_local_to_remote: false, deliver_remote_to_local: true, deliver_remote_to_remote: false,
   require_auth_local_to_local: false, require_auth_local_to_remote: true, require_auth_remote_to_local: false, require_auth_remote_to_remote: true,
   require_tls_for_auth: false, spam_protection: true, virus_protection: true };
const IPV4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/;
function ipAddress(text) { return IPV4.test(text) || /^[0-9a-f:]+$/i.test(text) && text.indexOf(':') >= 0; }

// The fetch account's schema and description as RestApiFetchAccounts.cpp
// emits them; the update takes any subset of the same keys.
const FETCH_PROPS = {
   name: { type: 'string' }, server_address: { type: 'string' }, port: { type: 'integer' },
   server_type: { type: 'string', enum: ['pop3', 'imap'] }, username: { type: 'string' }, password: { type: 'string', writeOnly: true },
   enabled: { type: 'boolean' }, minutes_between_fetch: { type: 'integer' }, days_to_keep_messages: { type: 'integer' },
   connection_security: { type: 'string', enum: ['none', 'starttls_optional', 'starttls_required', 'tls'] },
   process_mime_recipients: { type: 'boolean' }, process_mime_date: { type: 'boolean' }, use_antispam: { type: 'boolean' }, use_antivirus: { type: 'boolean' },
   enable_route_recipients: { type: 'boolean' }, mime_recipient_headers: { type: 'string' }, mirror_folders: { type: 'boolean' }
};
const FETCH_GET = 'The remote POP3 or IMAP mailboxes the server collects into this account - Account.FetchAccounts over COM. Each entry: id, name, server_address, port, server_type (pop3 or imap), username, enabled, minutes_between_fetch, days_to_keep_messages, connection_security, process_mime_recipients, process_mime_date, use_antispam, use_antivirus, enable_route_recipients, mime_recipient_headers, mirror_folders, locked (a collection is running now) and next_download_time. The remote password is never emitted. A key restricted to named domains reaches the accounts of those domains only.';
const FETCH_POST = 'Body: name, server_address and port (required); server_type (pop3, the default, or imap), username, password (write-only), enabled (default true), minutes_between_fetch (default 30), days_to_keep_messages (default 0: every message stays on the remote server; -1 deletes each message there once it has been collected; N deletes it N days after it was collected - one meaning for POP3 and IMAP, whose remote INBOX is collected once by UID), connection_security (none, starttls_optional, starttls_required, tls; default none), process_mime_recipients, process_mime_date, use_antispam, use_antivirus, enable_route_recipients, mime_recipient_headers, mirror_folders. What InterfaceFetchAccounts.Add and Save do; the first collection is scheduled at once. Everything is checked before anything is saved: an unknown field, a value of the wrong type or a value out of range is a 400 naming it.';
const FETCH_PUT = 'Body: any subset of the fields POST takes; a field left out keeps its value, and a password left out is kept. The same checks as POST, and nothing changes when one fails. What the setters and Save do over COM.';
// A fetch account as FetchAccountJson emits it, at the constructor's defaults.
function fetchRecord(id, name) {
   return { id, name, server_address: '', port: 110, server_type: 'pop3', username: '', minutes_between_fetch: 30, days_to_keep_messages: 0,
      connection_security: 'none', mime_recipient_headers: '', enabled: true, process_mime_recipients: false, process_mime_date: false,
      use_antispam: true, use_antivirus: true, enable_route_recipients: false, mirror_folders: false, locked: false, next_download_time: '' };
}
// A negative days_to_keep_messages with no days_to_keep_meaning beside it, from a
// page (a browser session): what a page loaded before 6.3.6 sends, and what the
// server answers 409 (RestApiFetchAccounts.cpp, FromStalePage).
function staleKeep(wanted) {
   return typeof wanted.days_to_keep_messages === 'number' && wanted.days_to_keep_messages < 0 &&
      (wanted.days_to_keep_meaning === undefined || wanted.days_to_keep_meaning === null);
}
const STALE_KEEP = { error: 'This page was loaded before the server was upgraded, and may mean the opposite by what it sent: since 6.3.6, -1 deletes each message on the other server once it has been collected, and 0 leaves every message there. Nothing was saved. Reload the page, check the choice and save again.', reason: 'page_out_of_date' };
// ApplyBody's checks, the first problem being the answer.
function fetchProblem(parsed, creating) {
   for (const key of Object.keys(parsed)) { if (!(key in FETCH_PROPS) && key !== 'days_to_keep_meaning') { return 'unknown field: ' + key; } }
   if ('days_to_keep_meaning' in parsed && parsed.days_to_keep_meaning !== 6106) { return 'days_to_keep_meaning is 6106, the meaning days_to_keep_messages has had since 6.3.6: 0 leaves every message on the other server, -1 deletes each one there once it has been collected, N deletes it N days after'; }
   const range = (key, low, high) => (key in parsed && (typeof parsed[key] !== 'number' || parsed[key] < low || parsed[key] > high)) ? key + ' must be a whole number between ' + low + ' and ' + high : null;
   const problem = range('port', 1, 65535) || range('minutes_between_fetch', 1, 100000) || range('days_to_keep_messages', -1, 100000);
   if (problem) { return problem; }
   if ('server_type' in parsed && FETCH_PROPS.server_type.enum.indexOf(parsed.server_type) < 0) { return 'server_type must be pop3 or imap'; }
   if ('connection_security' in parsed && FETCH_PROPS.connection_security.enum.indexOf(parsed.connection_security) < 0) { return 'connection_security must be one of none, starttls_optional, starttls_required, tls'; }
   if (creating && !String(parsed.name || '').trim()) { return 'name is required'; }
   if (creating && !String(parsed.server_address || '').trim()) { return 'server_address is required'; }
   if (creating && !('port' in parsed)) { return 'port is required'; }
   return null;
}

// The account groups, as RestApiGroups.cpp emits them. A membership is a pair
// and not a property of either side, so the members are a collection under the
// group and a member is removed by the account's id.
const GROUPS_GET = 'Settings.Groups over COM: the named sets of accounts a folder permission can name as one principal. Each entry: id, name. Read from the database on every call, so a group the Control Panel made is here. Server-wide; refused for domain-restricted keys.';
const GROUPS_POST = 'Body: name (required, at most 255 characters). Saved as Groups.Add and Group.Save save one over COM, with the same check - a second group of the same name is refused - and added to the collection ACLManager consults, so a permission may name it at once. Server-wide; refused for domain-restricted and read-only keys.';
const GROUPS_PUT = 'Body: name. The same check as POST; nothing changes when it is refused, and the permissions that name the group by id keep naming it. Server-wide; refused for domain-restricted and read-only keys.';
const GROUPS_DELETE = 'What Groups.DeleteByDBID does over COM: the group, its membership rows and every folder permission that named it go together, and the next IMAP command decides without them. Server-wide; refused for domain-restricted and read-only keys.';
const MEMBERS_GET = 'Group.Members over COM, read from the database as ACLManager reads them for every decision. Each entry: id (the membership row), group_id, account_id, account (the address; empty when the account no longer exists).';
const MEMBERS_POST = 'Body: account_id or account (the address); both, and they must name the same account. What GroupMembers.Add, put_AccountID and Save do over COM; in force for the next IMAP command. Server-wide; refused for domain-restricted and read-only keys.';
// Every account the recorded server knows, by the id the store gave it: a
// membership names the account by its id, and the address is the way in.
const ACCOUNT_IDS = { 'anna@example.com': 1, 'bob@example.com': 2, 'carla@example.com': 3 };

// A domain as DomainEntryJson_ emits it: every field, the relay password
// never among them.
function domainRecord(name, active, postmaster) {
   return { name, active, postmaster, max_message_size_kb: 0, max_size_mb: 0, max_account_size_mb: 0,
      max_accounts: 0, max_aliases: 0, max_lists: 0, message_retention_days: 0, relay_port: 0,
      max_accounts_enabled: false, max_aliases_enabled: false, max_lists_enabled: false,
      plus_addressing_enabled: false, plus_addressing_character: '+', use_greylisting: false,
      signature_enabled: false, signature_method: 'set_if_not_specified', signature_plain_text: '', signature_html: '',
      signature_add_to_replies: false, signature_add_to_local_mail: false,
      dkim_enabled: false, dkim_selector: '', dkim_private_key_file: '', dkim_signing_algorithm: 'sha256',
      dkim_sign_aliases: false, dkim_rotation_enabled: false, dkim_rotation_interval_days: 180, dkim_rotation_propagation_hours: 72,
      dkim_rotation_retention_days: 7, dkim_rotation_key_bits: 2048,
      relay_host: '', relay_requires_auth: false, relay_username: '', relay_connection_security: 'none',
      vacation_enabled: false, vacation_subject: '', vacation_message: '',
      vacation_internal_subject: '', vacation_internal_message: '', vacation_external_override: false, vacation_days: 0,
      quota_warning_percent: -1, quota_warning_subject: '', quota_warning_text: '', quota_grace_percent: -1, quota_send_stop_percent: -1,
      external_tag_subject: false, external_tag_header: false, external_tag_text: '', first_contact_tip: false,
      disclaimer_enabled: false, disclaimer_plain_text: '', disclaimer_html: '',
      quarantine_digest: false, quarantine_digest_hour: 7,
      openpgp_publish: false, openpgp_publish_new_accounts: false, country_policy: '', report_mailbox: '', restricted_domains: '',
      impersonation_action: 0, protected_persons: '', protected_partners: '' };
}

/* One value, three levels, as SettingLevelsRest::EntryFields answers two of the
   settings: recall_days, whose domain and account levels the overlay holds (null
   is "take the level above"), and quarantine_digest, whose domain level is the
   domain's own field and whose account level the overlay holds. Every such
   setting carries <name>_effective and <name>_source. */
const LEVEL_SERVER_RECALL_DAYS = 14;
const LEVEL_KEYS_DOMAIN = ['recall_days'];
const LEVEL_KEYS_ACCOUNT = ['recall_days', 'quarantine_digest'];
function levelAnswer(domainName, address) {
   const record = state.domains.find((d) => d.name === domainName) || {};
   const d = state.levels.domains[domainName] || {};
   const a = address ? (state.levels.accounts[address] || {}) : {};
   const out = {};
   const own = address ? a.recall_days : d.recall_days;
   out.recall_days = own === undefined ? null : own;
   out.recall_days_effective = address && a.recall_days !== undefined ? a.recall_days : d.recall_days !== undefined ? d.recall_days : LEVEL_SERVER_RECALL_DAYS;
   out.recall_days_source = address && a.recall_days !== undefined ? 'account' : d.recall_days !== undefined ? 'domain' : 'server';
   if (address) { out.quarantine_digest = a.quarantine_digest === undefined ? null : a.quarantine_digest; }
   out.quarantine_digest_effective = address && a.quarantine_digest !== undefined ? a.quarantine_digest : !!record.quarantine_digest;
   out.quarantine_digest_source = address && a.quarantine_digest !== undefined ? 'account' : 'domain';
   return out;
}
function storeLevels(place, parsed, keys) {
   keys.forEach((key) => {
      if (!(key in parsed)) { return; }
      if (parsed[key] === null) { delete place[key]; } else { place[key] = parsed[key]; }
   });
}

const SUPPRESSIONS_POST = 'Body: address (required), note (kept as the entry\'s diagnostic) and expires_days (0, the default, for an entry that stays until it is removed; at most 3650). An address this server hosts is refused.';
// An entry as BounceTracker::SuppressionsJson emits it, and a count row as CountsJson does.
function suppressionRecord(address, source, category, status) {
   return { address, source, category, status, diagnostic: source === 'manual' ? '' : '550 ' + status + ' <' + address + '>: User unknown',
      remote_mta: source === 'manual' ? '' : 'mx.remote.test', created: '2026-09-14 10:00:00', expires: '', expired: false };
}


/* A webhook subscription, in the shape the OpenAPI document describes it: the
   secret is writeOnly, which is what makes the page draw a write-only box and
   leave it out of a change when it is empty. */
const WEBHOOK_PROPS = {
   name: { type: 'string', description: 'Letters, digits and - _ . at most 64 characters.' },
   url: { type: 'string', description: 'https://host/path, or http:// only to this machine.' },
   secret: { type: 'string', writeOnly: true, description: 'Write-only. The bodies are signed under it; an empty string in a change keeps the one that is set.' },
   events: { type: 'string', description: 'message.received, message.delivered, message.bounced, message.quarantined, account.locked - comma-separated. Empty: every event.' },
   domains: { type: 'string', description: 'Comma-separated domain names. Empty: every domain.' },
   addresses: { type: 'string', description: 'Comma-separated addresses. Empty: every address.' },
   enabled: { type: 'boolean' },
   include_subject: { type: 'boolean', description: 'Carry the message subject on message.received and message.quarantined. Only where the domain allows it too.' },
   max_per_minute: { type: 'integer', description: 'Events past this in one minute are dropped and counted, not queued.' }
};
const WEBHOOKS_POST = 'Body: name (required), url (required), secret (required), events, domains and addresses, ' +
   'enabled (default true), include_subject (default false) and max_per_minute (0 for the server\'s WebhookMaxPerMinute).';

// The inbound webhooks' create schema and prose (RestApiInboundWebhooks.cpp,
// docs/InboundWebhooks.md): the defaults the Deck's editor starts from are read
// out of the prose, as the server writes it.
const INBOUND_PROPS = {
   address: { type: 'string', description: 'user@example.com, or example.com for every address of the domain.' },
   url: { type: 'string', description: 'https://host/path, with no space or control character.' },
   secret: { type: 'string', writeOnly: true, description: 'Write-only. Each request is signed under it; an empty string in a change keeps the one that is set.' },
   deliver: { type: 'boolean', description: 'Deliver to the address\'s mailbox as well, where it has one.' },
   on_failure: { type: 'string', enum: ['keep', 'bounce'], description: 'What a message the endpoint never accepted becomes.' },
   format: { type: 'string', enum: ['form', 'json'] },
   attachments: { type: 'string', enum: ['include', 'names', 'none'] },
   include_raw: { type: 'boolean' },
   max_size_kb: { type: 'integer', minimum: 0, maximum: 81920 },
   enabled: { type: 'boolean' }
};
const INBOUND_POST = 'Body: address (required; an address or a whole domain of a domain this server hosts), url (required), secret (required), ' +
   'deliver (default false: the route alone receives the address\'s mail), on_failure (keep or bounce; default keep - a message the endpoint did not accept is kept, or returned to its sender), ' +
   'format (form or json; default form), attachments (include, names or none; default include), include_raw (default false), ' +
   'max_size_kb (default 10240: a message larger than this is not posted; at most 81920) and enabled (default true).';

// The migration's create schema and descriptions as RestApiMigrations.cpp
// emits them (docs/Migration.md, "Five hundred mailboxes: migration jobs").
const MIGRATION_PROPS = {
   name: { type: 'string', description: 'What the migration is called here.' },
   kind: { type: 'string', enum: ['imap', 'google', 'microsoft365', 'pst'], description: 'An IMAP server with passwords; Google Workspace or Microsoft 365, each job signed in to by its owner; or Outlook data files (PST) copied onto this server.' },
   host: { type: 'string', description: 'The source\'s IMAP host.' },
   port: { type: 'integer', minimum: 1, maximum: 65535 },
   security: { type: 'string', enum: ['tls', 'starttls_required', 'starttls_optional', 'none'] },
   dav_url: { type: 'string', description: 'Where the source\'s CardDAV and CalDAV are, when not https://<host>/.' },
   concurrency: { type: 'integer', minimum: 1, maximum: 32, description: 'How many of its jobs run at once.' },
   messages_per_minute: { type: 'integer', minimum: 0, maximum: 100000, description: 'The most messages a minute each job fetches; 0 for no limit.' }
};
const MIGRATIONS_GET = 'Every migration - a batch of mailboxes copied from another server, a job per mailbox - with its jobs counted by state and its messages totalled. A credential confined to named domains sees only the migrations every job of which copies into one of them. Needs view_accounts.';
const MIGRATIONS_POST = 'Body: name (required); kind (imap, google, microsoft365 or pst; default imap); host (required for imap); port (993 with tls and 143 otherwise, when not given); security (none, tls, starttls_optional, starttls_required; default tls); dav_url (where CardDAV and CalDAV are, when not https://<host>/ - an https URL, or plain http only to this machine); concurrency (1 to 32; default 4), how many of its jobs run at once, below MigrationConnectionsPerHost across every migration at one host; messages_per_minute (0 to 100000, 0 for no limit; default 0), each job waiting between fetches to keep below it. For google and microsoft365 the source is the provider\'s own IMAP server - imap.gmail.com or outlook.office365.com, 993, tls - and a host, port or security naming anything else is refused, since the owner\'s token is sent there. A pst migration copies Outlook data files, read by this server\'s own PST reader: it takes no host, port, security or dav_url (what is sent for them is not used), and each of its jobs names a file in the pst_import_directory setting\'s subdirectory for the target account\'s domain. It is created new; POST .../jobs adds the mailboxes and POST .../start runs them. Needs manage_accounts, from a credential not confined to named domains: a migration with no jobs is no domain\'s.';
// A migration and a job as MigrationJson and JobJson emit them, less the
// counts the recorded server works out from the jobs on every read.
function migrationRecord(id, name, fields) {
   return Object.assign({ id, name, kind: 'imap', host: '', port: 993, security: 'tls', oauth_provider: '', dav_url: '', concurrency: 4,
      messages_per_minute: 0, state: 'new', created: 1789000000, started: 0, finished: 0 }, fields || {});
}
function migrationJob(id, migration, sourceUser, target, fields) {
   return Object.assign({ id, migration, source_user: sourceUser, target, state: 'queued', phase: 'mail', awaiting_signin: false, signs_in: false,
      folders_total: 0, folders_done: 0, messages_total: 0, messages_copied: 0, bytes: 0, skipped: 0, last_error: '', attempts: 0,
      next_attempt: 0, started: 0, finished: 0 }, fields || {});
}

function body(props, required) {
   return { content: { 'application/json': { schema: Object.assign({ type: 'object', properties: props }, required ? { required } : {}) } } };
}

/* The settings groups, in the shape OpenApiProperties emits them: a type, a
   description, an enum for a word setting, readOnly for a fact, writeOnly for
   a secret, "Takes effect when the server restarts." for a key the running
   server reads only at start, and x-applies - the settings baseline's mark,
   with its sentence - on a row kept in the store that was hMailServer.ini's
   [Settings] section (docs/SettingsApply.md). */
const SETTING_GROUPS = {
   '/api/v1/settings': {
      props: {
         hostname: { type: 'string', description: 'The name the server gives in its SMTP banner.' },
         max_message_size_kb: { type: 'integer', description: 'The largest message SMTP and IMAP APPEND accept, in KB; 0 is no limit.' },
         service_smtp: { type: 'boolean', description: 'Whether the SMTP server runs. Takes effect when the server restarts.' },
         smtp_relayer_host: { type: 'string', description: 'The smart host every outbound message goes through; empty delivers by MX.' },
         smtp_relayer_connection_security: { type: 'string', description: 'How the connection to the smart host is secured.', enum: ['none', 'tls', 'starttls_optional', 'starttls_required'] },
         smtp_relayer_password: { type: 'string', description: 'The password for the smart host. Accepted here and never emitted.', writeOnly: true }
      },
      values: { hostname: 'mail.example.com', max_message_size_kb: 10240, service_smtp: true, smtp_relayer_host: '', smtp_relayer_connection_security: 'none' }
   },
   '/api/v1/settings/antispam': {
      props: {
         spam_mark_threshold: { type: 'integer', description: 'The score at which a message is marked as spam.' },
         spam_delete_threshold: { type: 'integer', description: 'The score at which a message is deleted.' },
         use_spf: { type: 'boolean', description: 'Whether SPF is checked.' },
         tarpit_count: { type: 'integer', description: 'The recipient count in one session after which each further RCPT TO is delayed; 0 is off. Kept in hMailServer.ini. Applies when it is saved.', 'x-applies': 'now' }
      },
      values: { spam_mark_threshold: 5, spam_delete_threshold: 20, use_spf: true, tarpit_count: 0 }
   },
   '/api/v1/settings/logging': {
      props: {
         enabled: { type: 'boolean', description: 'Whether anything is logged at all.' },
         log_smtp: { type: 'boolean', description: 'Whether the SMTP conversations are logged.' },
         log_directory: { type: 'string', description: 'Where the log files are written.', readOnly: true }
      },
      values: { enabled: true, log_smtp: false, log_directory: '/var/log/hmailserver' }
   },
   '/api/v1/settings/antivirus': {
      props: {
         clamav_enabled: { type: 'boolean', description: 'Whether messages are scanned by a ClamAV daemon.' },
         clamav_host: { type: 'string', description: 'The ClamAV daemon\'s host.' },
         clamav_port: { type: 'integer', description: 'The ClamAV daemon\'s port, 1 to 65535.' },
         action: { type: 'string', description: 'What is done with a message a scanner flags.', enum: ['delete_email', 'delete_attachments'] },
         maximum_message_size_kb: { type: 'integer', description: 'Messages larger than this, in KB, are not scanned; 0 scans every size.' },
         custom_scanner_executable: { type: 'string', description: 'The scanner a custom scan runs.' }
      },
      // Two keys the server's environment sets (docs/EnvironmentConfiguration.md),
      // as the group's GET answers them: <key>_environment beside each, and a
      // value a file supplied answered as (set).
      values: { clamav_enabled: false, clamav_host: 'localhost', clamav_port: 3310, clamav_port_environment: 'variable',
         action: 'delete_attachments', maximum_message_size_kb: 0,
         custom_scanner_executable: '(set)', custom_scanner_executable_environment: 'file' }
   },
   // The three groups the Settings view gained on 14 September: the scripting
   // switch and language, the cache's switch, ceilings, lives and counters
   // (the counters read-only, as the table has them), and the indexing switch
   // with its two counts.
   '/api/v1/settings/scripting': {
      props: {
         enabled: { type: 'boolean', description: 'Whether the event handlers run at all.' },
         language: { type: 'string', description: 'VBScript, JScript or JavaScript, the language the event handlers are written in; any other name is refused. The Windows builds run VBScript and JScript, the Linux builds JavaScript.' },
         current_script_file: { type: 'string', description: 'The event-handler file the server is running, in the event directory.', readOnly: true }
      },
      values: { enabled: false, language: 'VBScript', current_script_file: '/var/lib/hmailserver/events/EventHandlers.vbs' }
   },
   '/api/v1/settings/cache': {
      props: {
         enabled: { type: 'boolean', description: 'Whether the domain, account, alias and distribution-list caches are used at all.' },
         domain_cache_size_kb: { type: 'integer', description: 'What the domain cache holds now, in kilobytes.', readOnly: true },
         domain_cache_max_size_kb: { type: 'integer', description: 'The domain cache\'s ceiling, in kilobytes. Held in memory only, as over COM: the built-in 10240 returns when the server starts.' },
         domain_cache_ttl: { type: 'integer', description: 'Seconds a domain stays cached.' },
         domain_hit_rate: { type: 'integer', description: 'Of the domain lookups since the cache was last cleared, the percentage the cache answered.', readOnly: true }
      },
      values: { enabled: true, domain_cache_size_kb: 12, domain_cache_max_size_kb: 10240, domain_cache_ttl: 3600, domain_hit_rate: 97 }
   },
   '/api/v1/settings/indexing': {
      props: {
         enabled: { type: 'boolean', description: 'Whether delivered messages are indexed for search.' },
         total_indexed_count: { type: 'integer', description: 'How many messages the index holds.', readOnly: true },
         total_message_count: { type: 'integer', description: 'How many delivered messages there are to index.', readOnly: true }
      },
      values: { enabled: true, total_indexed_count: 40, total_message_count: 45 }
   },
   // Bounces read, and the suppression list: a group like the others, with a
   // word setting for the action and the sweep as its verb.
   '/api/v1/settings/bounces': {
      props: {
         processing_enabled: { type: 'boolean', description: 'The master switch: classify bounces, count them per address, and keep a suppression list per sending domain.' },
         hard_bounce_threshold: { type: 'integer', description: 'Hard bounces in a row, with no delivery between them, that put an address on its sending domain\'s list. 1 to 100; 1 by default.' },
         mailbox_full_is_hard: { type: 'boolean', description: 'Whether a full mailbox (X.2.2) counts as a hard bounce.' },
         suppression_action: { type: 'string', description: 'What happens to a listed recipient.', enum: ['refuse', 'drop', 'observe'] },
         suppression_expiry_days: { type: 'integer', description: 'Days after which an entry made by a bounce or a report stops applying.' },
         read_delivery_reports: { type: 'boolean', description: 'Whether RFC 3464 delivery reports are read.' },
         count_retention_days: { type: 'integer', description: 'Days bounce counts are kept.' },
         report_window_days: { type: 'integer', description: 'Days after a message is sent that a report about it is still matched.' }
      },
      values: { processing_enabled: false, hard_bounce_threshold: 1, mailbox_full_is_hard: false, suppression_action: 'refuse', suppression_expiry_days: 0,
         read_delivery_reports: true, count_retention_days: 90, report_window_days: 7 }
   },
   // The Web Key Directory: a group like the others, off by default.
   '/api/v1/settings/openpgp': {
      props: {
         web_key_directory_enabled: { type: 'boolean', description: 'The master switch: publish this server\'s accounts\' OpenPGP public keys under /.well-known/openpgpkey on the web services listener.' },
         key_maximum_bytes: { type: 'integer', description: 'The largest OpenPGP key this server will accept, in bytes of the key itself. 1024 to 1048576; 65536 by default.' },
         keys_per_account: { type: 'integer', description: 'How many OpenPGP keys one account may hold at once. 1 to 64; 8 by default.' },
         require_address_in_user_id: { type: 'boolean', description: 'Refuse a key whose user ids do not carry the account\'s own address.' }
      },
      values: { web_key_directory_enabled: true, key_maximum_bytes: 65536, keys_per_account: 8, require_address_in_user_id: true }
   },
   // Web Push: a switch and limits, a read-only public key that a verb makes
   // afresh, and a write-only private key for carrying an installation.
   '/api/v1/settings/push': {
      props: {
         enabled: { type: 'boolean', description: 'The master switch. Off by default, and with it off nothing is queued, nothing is signed and no push service is ever reached.' },
         subject: { type: 'string', description: 'The contact a push service may use when this server\'s subscriptions are failing: a mailto: or https: URI.' },
         time_to_live_seconds: { type: 'integer', description: 'How long a push service should hold a notification for a device that is offline.' },
         max_devices_per_account: { type: 'integer', description: 'How many devices one account may have notifications on at once. 1 to 50; 10 by default.' },
         max_attempts: { type: 'integer', description: 'How many times a push to one device is attempted before that device is dead-lettered.' },
         vapid_public_key: { type: 'string', description: 'The public half of this server\'s VAPID key pair, base64url. Derived from the private half.', readOnly: true },
         vapid_private_key: { type: 'string', description: 'The private half, base64url. Accepted so an installation can be carried to another machine, and never emitted by anything.', writeOnly: true }
      },
      values: { enabled: false, subject: '', time_to_live_seconds: 86400, max_devices_per_account: 10, max_attempts: 5, vapid_public_key: '' }
   },
   // Complaints read, the feedback loops: a group like the others, with the
   // sweep as its verb and a mailbox that decides whether anything is read.
   '/api/v1/settings/complaints': {
      props: {
         processing_enabled: { type: 'boolean', description: 'The master switch: read the RFC 5965 abuse reports the providers post to the complaint mailbox.' },
         mailbox: { type: 'string', description: 'The address this server\'s feedback loops are registered against. Only a report delivered here is read as a complaint.' },
         suppression_threshold: { type: 'integer', description: 'Complaints about one address that put it on its sending domain\'s suppression list. 0 lists nobody.' },
         retention_days: { type: 'integer', description: 'Days a complaint and the daily tally behind the complaint rate are kept.' }
      },
      values: { processing_enabled: true, mailbox: 'fbl@example.com', suppression_threshold: 1, retention_days: 180 }
   },
   '/api/v1/settings/backup': {
      props: {
         destination: { type: 'string', description: 'The directory a backup is written to.' },
         backup_domains: { type: 'boolean', description: 'Whether the domains, accounts, aliases and lists are backed up.' },
         backup_messages: { type: 'boolean', description: 'Whether the message files are backed up.' },
         backup_settings: { type: 'boolean', description: 'Whether the server settings are backed up.' },
         compress: { type: 'boolean', description: 'Whether the backup is compressed into one file.' },
         log_file: { type: 'string', description: 'The full path of the backup log, beside the other logs.', readOnly: true },
         encryption_passphrase: { type: 'string', writeOnly: true, description: 'The passphrase the backup archive is encrypted with. Accepted here and never emitted.' },
         encryption_passphrase_set: { type: 'boolean', readOnly: true, description: 'Whether a backup passphrase is set, and so whether the next backup is encrypted.' }
      },
      values: { destination: '/var/backups/hmailserver', backup_domains: true, backup_messages: true, backup_settings: true, compress: false, log_file: '/var/log/hmailserver/hmailserver_backup.log', encryption_passphrase_set: false }
   }
};
const secrets = {};

function settingsPath(path) {
   return { get: { summary: 'The group', description: 'Every key the PUT lists, as the server holds it now; the write-only ones are left out.' },
      put: { summary: 'Change the group', description: 'Body: any subset of the writable keys below.', requestBody: body(SETTING_GROUPS[path].props) } };
}

const spec = {
   openapi: '3.0.0',
   paths: {
      '/api/v1/settings': settingsPath('/api/v1/settings'),
      '/api/v1/settings/antispam': settingsPath('/api/v1/settings/antispam'),
      '/api/v1/settings/logging': settingsPath('/api/v1/settings/logging'),
      '/api/v1/settings/antivirus': settingsPath('/api/v1/settings/antivirus'),
      '/api/v1/rules': {
         get: { summary: 'List the global rules with their criteria and actions', description: 'Each entry: id, name, active, all_criteria, criteria (field, header, match, value) and actions (type, value). Server-wide; refused for domain-restricted keys.' },
         post: { summary: 'Create a global rule', description: RULES_POST, requestBody: body({ name: { type: 'string' } }, ['name']) }
      },
      '/api/v1/rules/{id}': { put: { summary: 'Replace a global rule' }, delete: { summary: 'Delete a global rule' } },
      '/api/v1/webhooks': {
         get: { summary: 'The webhook subscriptions', description: 'One entry per subscription: its name, the endpoint, whether a shared secret is set (the secret itself is never returned), the events, domains and addresses it is confined to (empty means every one), whether it is on, whether its bodies may carry a subject, its rate limit, and how many of its deliveries are pending, delivered, dead-lettered and dropped.' },
         post: { summary: 'Create a webhook subscription', description: WEBHOOKS_POST, requestBody: body(WEBHOOK_PROPS, ['name', 'url', 'secret']) }
      },
      '/api/v1/webhooks/{name}': {
         get: { summary: 'One webhook subscription' },
         put: { summary: 'Change a webhook subscription', description: 'Any subset of the fields the create takes. A field left out keeps its value, and an empty secret keeps the one that is set.', requestBody: body(WEBHOOK_PROPS) },
         delete: { summary: 'Delete a webhook subscription' }
      },
      '/api/v1/webhooks/deliveries': { get: { summary: 'The webhook delivery queue' } },
      '/api/v1/webhooks/run': { post: { summary: 'Run one webhook delivery pass now' } },
      '/api/v1/webhooks/test': { post: { summary: 'Raise a webhook event on request' } },
      '/api/v1/inbound-webhooks': {
         get: { summary: 'The inbound webhooks', description: 'Each address or domain whose mail this server posts to a URL.' },
         post: { summary: 'Route an address or a domain to a URL', description: INBOUND_POST, requestBody: body(INBOUND_PROPS, ['address', 'url', 'secret']) }
      },
      '/api/v1/inbound-webhooks/{address}': {
         get: { summary: 'One inbound webhook' },
         put: { summary: 'Change an inbound webhook', description: 'Any subset of the fields the create takes but the address; a field left out keeps its value, and an empty secret keeps the one that is set.', requestBody: body(INBOUND_PROPS) },
         delete: { summary: 'Remove an inbound webhook' }
      },
      '/api/v1/inbound-webhooks/deliveries': { get: { summary: 'The inbound webhook queue' } },
      '/api/v1/inbound-webhooks/deliveries/{id}': { delete: { summary: 'Remove an inbound webhook delivery' } },
      '/api/v1/inbound-webhooks/deliveries/{id}/retry': { post: { summary: 'Try an inbound webhook delivery again' } },
      '/api/v1/inbound-webhooks/deliveries/{id}/message': { get: { summary: 'The message an inbound webhook delivery kept' } },
      '/api/v1/inbound-webhooks/run': { post: { summary: 'Run one inbound webhook pass now' } },
      '/api/v1/routes': {
         get: { summary: 'List the SMTP routes', description: 'The routes the running server delivers by, as the Control Panel lists them.' },
         post: { summary: 'Create an SMTP route', description: ROUTES_POST, requestBody: body(ROUTE_PROPS, ['domain_name', 'target_smtp_host']) }
      },
      '/api/v1/routes/{id}': {
         put: { summary: 'Replace an SMTP route', description: 'The whole record, with the same fields, defaults and checks as the create.', requestBody: body(ROUTE_PROPS, ['domain_name', 'target_smtp_host']) },
         delete: { summary: 'Delete an SMTP route' }
      },
      '/api/v1/remote-domains': {
         get: { summary: 'List the remote domain policies', description: 'What this server will do when it talks to a named remote domain, as the running server holds them.' },
         post: { summary: 'Create a remote domain policy', description: REMOTE_DOMAINS_POST, requestBody: body(REMOTE_DOMAIN_PROPS, ['domain_name']) }
      },
      '/api/v1/remote-domains/{id}': {
         put: { summary: 'Replace a remote domain policy', description: 'The whole record, with the same fields, defaults and checks as the create.', requestBody: body(REMOTE_DOMAIN_PROPS, ['domain_name']) },
         delete: { summary: 'Delete a remote domain policy' }
      },
      '/api/v1/remote-domains/effective': { get: { summary: 'The policy that governs a domain' } },
      '/api/v1/remote-domains/verification-cache/clear': { post: { summary: 'Forget every remembered recipient verification verdict' } },
      '/api/v1/certificates': {
         get: { summary: 'List the SSL certificates', description: 'Names and file paths, never a private key password.' },
         post: { summary: 'Add an SSL certificate', description: 'A name and the paths of a PEM certificate file and its private key file on the server.', requestBody: body(CERT_PROPS, ['name', 'certificate_file', 'private_key_file']) }
      },
      '/api/v1/certificates/{id}': { delete: { summary: 'Delete an SSL certificate' } },
      '/api/v1/ports': {
         get: { summary: 'List the TCP/IP ports', description: 'Every listener the server is configured with.' },
         post: { summary: 'Add a TCP/IP port', description: PORTS_POST, requestBody: body(PORT_PROPS, ['protocol', 'port']) }
      },
      '/api/v1/ports/{id}': {
         put: { summary: 'Replace a TCP/IP port', description: PORTS_PUT, requestBody: body(PORT_PROPS, ['protocol', 'port']) },
         delete: { summary: 'Delete a TCP/IP port', description: PORTS_DELETE }
      },
      '/api/v1/server/reinitialize': { post: { summary: 'Restart the services in place', description: REINITIALIZE } },
      '/api/v1/domains': {
         get: { summary: 'List domains', description: 'A domain-restricted key sees only its own domains. Each entry: name, active, postmaster.' },
         post: { summary: 'Create a domain', description: DOMAINS_POST, requestBody: body({ name: { type: 'string' }, active: { type: 'boolean' }, postmaster: { type: 'string' } }, ['name']) }
      },
      '/api/v1/domains/{domain}': {
         put: { summary: 'Change a domain', description: DOMAIN_PUT, requestBody: body({ active: { type: 'boolean' }, postmaster: { type: 'string' }, name: { type: 'string' } }, ['active']) },
         delete: { summary: 'Delete a domain with everything in it' }
      },
      '/api/v1/domains/{domain}/domain-aliases': {
         get: { summary: 'The domain\'s aliases - other names the domain answers to', description: 'Each entry: id, name.' },
         post: { summary: 'Add a domain alias', description: 'Body: name.', requestBody: body({ name: { type: 'string' } }, ['name']) }
      },
      '/api/v1/domains/{domain}/domain-aliases/{name}': { delete: { summary: 'Remove a domain alias' } },
      '/api/v1/domains/{domain}/sender-list': {
         get: { summary: 'One domain\'s allow and block list', description: 'The same object as the server\'s list, for every account of this domain.' },
         post: { summary: 'Add an entry to a domain\'s list', requestBody: body({ value: { type: 'string' }, blocked: { type: 'boolean' }, reason: { type: 'string' } }, ['value']) }
      },
      '/api/v1/domains/{domain}/sender-list/{id}': { delete: { summary: 'Remove an entry from a domain\'s list' } },
      '/api/v1/domains/{domain}/spam-traps': {
         get: { summary: 'The domain\'s spam traps', description: 'Addresses in this domain that were never real. A message to one is refused exactly as a message to any address this server does not have.' },
         post: { summary: 'Declare an address a spam trap', requestBody: body({ address: { type: 'string' } }, ['address']) }
      },
      '/api/v1/domains/{domain}/spam-traps/hits': { get: { summary: 'What has addressed this domain\'s spam traps' } },
      '/api/v1/spam-traps/{address}': { delete: { summary: 'Stop trapping an address' } },
      '/api/v1/journal-rules': {
         get: { summary: 'List the journaling and dual-delivery rules', description: 'A copy of a domain\'s mail, or of one account\'s, sent to an address.' },
         post: { summary: 'Add a journaling rule', requestBody: body({ scope: { type: 'string' }, target: { type: 'string' }, journal_address: { type: 'string' }, direction: { type: 'string' }, mode: { type: 'string' }, enabled: { type: 'boolean' }, note: { type: 'string' } }, ['target', 'journal_address']) }
      },
      '/api/v1/journal-rules/{id}': {
         put: { summary: 'Change a journaling rule' },
         delete: { summary: 'Remove a journaling rule' }
      },
      '/api/v1/spam-traps/hits': {
         get: { summary: 'What has addressed any spam trap', description: 'Newest first, with what was done about each.' },
         delete: { summary: 'Clear the record of spam trap hits' }
      },
      '/api/v1/domains/{domain}/aliases': {
         get: { summary: 'List aliases in a domain' },
         post: { summary: 'Create an alias', description: 'Body: name (the alias address, in this domain), value (the e-mail address it delivers to) and active (default true). Judged as the Control Panel judges an alias - a name an account or a distribution list already has, or a domain at its alias limit, is refused with the same sentence - and in effect for the next message to the name. Scoped to the domain.',
            requestBody: body({ name: { type: 'string' }, value: { type: 'string' }, active: { type: 'boolean' } }, ['name', 'value']) }
      },
      '/api/v1/aliases/{address}': { delete: { summary: 'Delete an alias', description: 'Scoped to the address\'s domain.' } },
      '/api/v1/domains/{domain}/lists': {
         get: { summary: 'List the distribution lists in a domain, with their members', description: 'Each entry: address, active, require_auth, mode (public, membership, announcement or domain_members), require_sender_address, moderator_address, bounce_address, membership and members. members is the addresses kept by hand; membership is the query that finds the rest among the server\'s accounts when a message is sent to the list, and is empty for a list whose members are its rows alone. GET /api/v1/lists/{address}/members answers what the two come to now. Scoped to the domain.' },
         post: { summary: 'Create a distribution list', description: LISTS_POST, requestBody: body(LIST_CREATE_PROPS, ['address']) }
      },
      '/api/v1/lists/{address}': {
         put: { summary: 'Change a distribution list', description: LIST_PUT, requestBody: body(LIST_PROPS) },
         delete: { summary: 'Delete a distribution list' }
      },
      '/api/v1/lists/{address}/members': {
         get: { summary: 'Who a distribution list delivers to now', description: 'The list\'s own rows, then the accounts its membership query answers at the moment of the call, deduplicated by address. Each entry: address and source - row for a member kept by hand, query for one the membership found.' }
      },
      '/api/v1/accounts/{address}': {
         get: { summary: 'Read an account', description: 'The account whole, as the update answers it, less the password, the hash and the TOTP secret. Scoped to the address\'s domain.' },
         put: { summary: 'Update an account', description: ACCOUNT_PUT, requestBody: body(ACCOUNT_PROPS) },
         delete: { summary: 'Delete an account' }
      },
      '/api/v1/ipranges': {
         get: { summary: 'List the IP ranges', description: 'Server-wide; refused for domain-restricted keys.' },
         post: { summary: 'Create an IP range', requestBody: body(RANGE_PROPS, ['name', 'lower', 'upper']) }
      },
      '/api/v1/ipranges/{id}': {
         put: { summary: 'Change an IP range', description: RANGE_PUT, requestBody: body({}) },
         delete: { summary: 'Delete an IP range' }
      },
      '/api/v1/accounts/{address}/directory': {
         get: { summary: "An account's directory entry, administered" },
         put: { summary: "Set an account's directory entry",
            description: 'Any subset of title, department, phone, mobile, office, photo, opt_out and note; a member sent null is cleared.' }
      },
      '/api/v1/accounts/{address}/directory/photo': { get: { summary: "An account's directory photo" } },
      '/api/v1/accounts/{address}/move': {
         get: { summary: 'Whether this mailbox can be moved now, and what it has been through' },
         post: { summary: 'Rename a mailbox, or move it to another domain on this server',
            requestBody: { content: { 'application/json': { schema: { type: 'object', required: ['new_address'],
               properties: { new_address: { type: 'string' }, keep_alias: { type: 'boolean' } } } } } } },
      },
      '/api/v1/accounts/{address}/fetch-accounts': {
         get: { summary: 'The account\'s external (fetch) accounts', description: FETCH_GET },
         post: { summary: 'Create an external (fetch) account', description: FETCH_POST, requestBody: body(FETCH_PROPS, ['name', 'server_address', 'port']) }
      },
      '/api/v1/accounts/{address}/fetch-accounts/{id}': {
         get: { summary: 'One external (fetch) account' },
         put: { summary: 'Change an external (fetch) account', description: FETCH_PUT, requestBody: body({}) },
         delete: { summary: 'Delete an external (fetch) account' }
      },
      '/api/v1/accounts/{address}/fetch-accounts/{id}/download': { post: { summary: 'Collect from the remote mailbox now' } },
      '/api/v1/migrations': {
         get: { summary: 'The migrations', description: MIGRATIONS_GET },
         post: { summary: 'Create a migration', description: MIGRATIONS_POST, requestBody: body(MIGRATION_PROPS, ['name']) }
      },
      '/api/v1/migrations/{id}': { get: { summary: 'One migration' }, delete: { summary: 'Delete a migration' } },
      '/api/v1/migrations/{id}/start': { post: { summary: 'Start a migration' } },
      '/api/v1/migrations/{id}/pause': { post: { summary: 'Pause a migration' } },
      '/api/v1/migrations/{id}/resume': { post: { summary: 'Resume a migration' } },
      '/api/v1/migrations/{id}/summary.csv': { get: { summary: 'A migration\'s jobs as CSV' } },
      '/api/v1/migrations/{id}/jobs': { get: { summary: 'A migration\'s jobs' }, post: { summary: 'Add jobs to a migration' } },
      '/api/v1/migrations/{id}/jobs/{job}': { get: { summary: 'One job' } },
      '/api/v1/migrations/{id}/jobs/{job}/report': { get: { summary: 'A job\'s report' } },
      '/api/v1/migrations/{id}/jobs/{job}/retry': { post: { summary: 'Retry a job' } },
      '/api/v1/migrations/{id}/jobs/{job}/cancel': { post: { summary: 'Cancel a job' } },
      '/api/v1/migrations/{id}/jobs/{job}/signin': { post: { summary: 'A new sign-in link for a job' } },
      '/api/v1/groups': {
         get: { summary: 'List the account groups', description: GROUPS_GET },
         post: { summary: 'Create a group', description: GROUPS_POST, requestBody: body({ name: { type: 'string' } }, ['name']) }
      },
      '/api/v1/settings/push': settingsPath('/api/v1/settings/push'),
      '/api/v1/settings/push/vapid': { post: { summary: 'Make a new VAPID key pair', description: 'Generates a fresh P-256 key pair for Web Push and stores both halves. Every existing subscription stops working and each device must be asked again.' } },
      '/api/v1/accounts/{address}/push': {
         get: { summary: 'The Web Push devices of an account (administrator)', description: 'count, and subscriptions. The endpoint and the two keys are never in it.' },
         delete: { summary: 'Stop Web Push on every device of an account (administrator)', description: 'What a lost phone needs: every subscription of the account is removed.' }
      },
      '/api/v1/groups/{id}': {
         get: { summary: 'One group' },
         put: { summary: 'Rename a group', description: GROUPS_PUT, requestBody: body({ name: { type: 'string' } }) },
         delete: { summary: 'Delete a group', description: GROUPS_DELETE }
      },
      '/api/v1/groups/{id}/members': {
         get: { summary: 'The accounts in a group', description: MEMBERS_GET },
         post: { summary: 'Add an account to a group', description: MEMBERS_POST, requestBody: body({ account_id: { type: 'integer' }, account: { type: 'string' } }) }
      },
      '/api/v1/groups/{id}/members/{account_id}': { delete: { summary: 'Remove an account from a group', description: 'By the account\'s id, as the membership is the pair.' } },
      '/api/v1/settings/backup': settingsPath('/api/v1/settings/backup'),
      '/api/v1/settings/bounces': settingsPath('/api/v1/settings/bounces'),
      '/api/v1/settings/openpgp': settingsPath('/api/v1/settings/openpgp'),
      '/api/v1/domains/{domain}/openpgp': { get: { summary: 'Every OpenPGP key held for a domain\'s accounts (administrator)',
         description: 'domain, server_enabled, domain_enabled, publish_new_accounts, total and keys - each the key\'s row with the account\'s address, its hashed_local_part, advanced_url and direct_url, and published: whether this key is actually being served.' } },
      '/api/v1/accounts/{address}/openpgp': {
         get: { summary: 'An account\'s OpenPGP public keys (administrator)', description: 'The same document the account\'s own GET /api/v1/me/openpgp answers.' },
         put: { summary: 'Upload an OpenPGP public key for an account (administrator)',
            description: 'Body: key (required), allow_address_mismatch and publish. A SECRET key is refused here as everywhere.',
            requestBody: body({ key: { type: 'string' }, allow_address_mismatch: { type: 'boolean' }, publish: { type: 'boolean' } }, ['key']) } },
      '/api/v1/accounts/{address}/openpgp/{fingerprint}': { delete: { summary: 'Remove an account\'s OpenPGP key (administrator)', description: 'By fingerprint, through the account.' } },
      '/api/v1/bounces/expire': { post: { summary: 'Run the suppression list\'s maintenance sweep now', description: 'What the hourly task does: expired entries, old counts and old report keys are removed.' } },
      '/api/v1/domains/{domain}/suppressions': {
         get: { summary: 'The suppression list of a domain', description: 'Addresses this domain no longer sends to because mail to them bounced, or because an administrator put them there.' },
         post: { summary: 'Put an address on a domain\'s suppression list by hand', description: SUPPRESSIONS_POST, requestBody: body({ address: { type: 'string' }, note: { type: 'string' }, expires_days: { type: 'integer' } }, ['address']) }
      },
      '/api/v1/domains/{domain}/suppressions/{address}': { delete: { summary: 'Take an address off a domain\'s suppression list' } },
      '/api/v1/domains/{domain}/bounces': { get: { summary: 'The bounce counts of a domain\'s recipients', description: 'Per address this domain has sent to and had bounce: the counts, the run of hard bounces and whether the address is listed now.' } },
      '/api/v1/settings/complaints': settingsPath('/api/v1/settings/complaints'),
      '/api/v1/complaints/expire': { post: { summary: 'Run the feedback loops\' maintenance sweep now', description: 'What the hourly task does: complaints older than retention_days, and the daily tally rows behind the rate, are removed.' } },
      '/api/v1/domains/{domain}/complaints': { get: { summary: 'The abuse reports recorded about a domain\'s mail', description: 'What the large providers\' feedback loops have reported about this domain: who complained, about which message, and which provider reported it.' } },
      '/api/v1/complaints/rates': { get: { summary: 'Every domain\'s complaint rate', description: 'The complaint rate of each locally hosted domain over the last seven days, worst first.' } },
      // The reports our own domains receive, read (RestApiDomainReports.cpp).
      '/api/v1/domains/{domain}/dmarc-reports/summary': { get: { summary: 'Who sends as a domain, and with what result: the DMARC aggregate reports read from its report mailbox, charted', description: 'Over the last days days: messages, passed, failed, by_day and sources.' } },
      '/api/v1/domains/{domain}/dmarc-reports': { get: { summary: 'The DMARC aggregate reports read for a domain', description: 'Newest first, a page at a time.' } },
      '/api/v1/domains/{domain}/dmarc-reports/{id}': { get: { summary: 'One DMARC aggregate report with its records', description: 'The item as the listing shows it, and records.' } },
      '/api/v1/domains/{domain}/tls-reports': { get: { summary: 'The TLS-RPT reports read for a domain', description: 'One item per policy of each report, newest first.' } },
      '/api/v1/domain-reports/expire': { post: { summary: 'Run the domain reports\' retention sweep now', description: 'Reports read more than domain_report_retention_days ago are removed.' } },
      ...collectionPaths(),
      '/api/v1/settings/messages': { get: { summary: 'The server\'s message texts', description: 'The texts the server puts in the mail it writes itself - Settings.ServerMessages over COM. Each entry: id, name, text. Server-wide; refused for domain-restricted keys.' } },
      '/api/v1/settings/messages/{name}': { put: { summary: 'Change one server message text', description: 'Body: text. The name is the table\'s own, as the listing shows it. In use once the messages are read again after the save, which is at once unless that read fails, and the application log says when it does. Server-wide; refused for domain-restricted and read-only keys.', requestBody: body({ text: { type: 'string' } }, ['text']) } },
      '/api/v1/settings/scripting': settingsPath('/api/v1/settings/scripting'),
      '/api/v1/settings/scripting/reload': { post: { summary: 'Load the event-handler script again', description: 'What Scripting.Reload does over COM: the script file is read again and the handlers it defines take over from the next event.' } },
      '/api/v1/settings/scripting/check': { post: { summary: 'Check the event-handler script\'s syntax', description: 'What Scripting.CheckSyntax does over COM: result is empty when the script parses, and the parser\'s message otherwise. Nothing is changed.' } },
      '/api/v1/settings/breach-check/test': { post: { summary: 'Ask the password breach check endpoint for a range', description: 'What Settings.TestBreachCheckEndpoint does over COM: the endpoint is asked for the prefix 00000 and the answer is reported. Nothing is changed.' } },
      '/api/v1/settings/cache': settingsPath('/api/v1/settings/cache'),
      '/api/v1/settings/cache/clear': { post: { summary: 'Empty the caches', description: 'What Settings.Cache.Clear does over COM: the domain, account, alias and distribution-list caches are emptied.' } },
      '/api/v1/settings/indexing': settingsPath('/api/v1/settings/indexing'),
      '/api/v1/settings/indexing/index': { post: { summary: 'Index the messages not yet indexed, now', description: 'What Settings.MessageIndexing.Index does over COM: the indexer runs at once rather than at its next tick; the answer does not wait for it.' } },
      '/api/v1/settings/indexing/clear': { post: { summary: 'Empty the message index', description: 'What Settings.MessageIndexing.Clear does over COM: every indexed message\'s metadata, the full-text terms and the backfill cursor go.' } },
      '/api/v1/apikeys': {
         get: { summary: 'List API keys', description: 'Administrator password only.' },
         post: { summary: 'Create an API key', description: 'Administrator password only. The token is returned once, at creation.' }
      },
      '/api/v1/apikeys/{id}': { delete: { summary: 'Revoke an API key', description: 'Administrator password only.' } },
      '/api/v1/quarantine': { get: { summary: 'List quarantined messages', description: 'Server-wide; refused for domain-restricted keys. Bounded to the newest 1000.' } },
      '/api/v1/quarantine/{id}': { delete: { summary: 'Delete a quarantined message' } },
      '/api/v1/quarantine/{id}/release': { post: { summary: 'Release a quarantined message to its original recipients', description: 'Delivery is direct rather than back through the filters: a release is an administrator overruling them.' } },
      '/api/v1/review': { get: { summary: 'Messages held for review by the compromised-account detection', description: 'Server-wide, like the delivery queue. Refused to an API key.' } },
      '/api/v1/review/{id}': { delete: { summary: 'Discard a held message' } },
      '/api/v1/review/{id}/release': { post: { summary: 'Release a held message to the delivery queue' } },
      '/api/v1/accounts/{address}/review': {
         get: { summary: 'An account\'s state under the compromised-account detection' },
         put: { summary: 'Set an account\'s exemption or sensitivity', description: 'Body: any of exempt (true or false) and sensitivity (default, relaxed or strict).' }
      },
      '/api/v1/accounts/{address}/review/unlock': { post: { summary: 'Clear the compromise lock alone' } },
      '/api/v1/archive': { get: { summary: 'Search the archive index', description: 'Query parameters: domain and mailbox (exact), sender, recipient and subject (contains), since and until (YYYY-MM-DD HH:MM:SS), hold=1 for held copies only, limit (1-1000, default 200). Newest first.' } },
      '/api/v1/archive/{id}/hold': {
         post: { summary: 'Put an archived copy on legal hold', description: 'A held copy is never removed by the retention sweep or by an address erasure.' },
         delete: { summary: 'Lift the hold' }
      },
      '/api/v1/discovery/cases': {
         get: { summary: 'eDiscovery: the cases', description: 'A case holds its searches, its holds and its exports; a search and an export are queued and done one at a time. docs/Discovery.md.' },
         post: { summary: 'eDiscovery: open a case', description: 'Body: name (required) and description.' }
      },
      '/api/v1/metrics/history': { get: { summary: 'The history of one metric', description: 'Query parameters: metric (a name from the exporter without the hmailserver_ prefix, e.g. sessions_smtp or processed_messages_total) and range (24h, 7d or 30d - samples averaged per minute, per ten minutes and per hour).' } },
      '/api/v1/update': { get: { summary: 'The update check\'s verdict', description: 'state: 0 not checked since the service started, 1 up to date, 2 a newer release is available, 3 its installer is downloaded and verified, 4 installing, 5 the last check failed (lastError).' } },
      '/api/v1/update/check': { post: { summary: 'Read the release feed now', description: 'Runs the update check on this request, whether or not the scheduled check is on, and returns the verdict as GET /api/v1/update does. Refused for read-only keys.' } },
      '/api/v1/update/download': { post: { summary: 'Download and verify the newer release\'s installer', description: 'Fetches the installer the last check found and its Sigstore bundle into the data directory\'s Updates folder and verifies the installer against the bundle.' } },
      '/api/v1/update/install': { post: { summary: 'Apply the verified installer', description: 'Verifies the downloaded installer once more and hands it to the update helper, which stops the service, installs and starts it again.' } },
      '/api/v1/srv': { get: { summary: 'Recommended client-discovery SRV records for the enabled listeners', description: 'One record set per active domain; a domain-restricted key sees only its own domains.' } },
      '/api/v1/accounts/{address}/folders': { get: { summary: 'An account\'s folders (administrator)', description: 'The account\'s own folder tree - what Account.IMAPFolders is over COM: delimiter, and folders each with its id, name, path, parent_id and subfolders.' } },
      '/api/v1/accounts/{address}/folders/{id}/permissions': {
         get: { summary: 'A folder\'s access-control list (administrator)', description: 'The rows of the folder\'s ACL - IMAPFolder.Permissions over COM, GETACL over IMAP. Each: id, folder_id, type (user, group or anyone), account_id and account, group_id and group, rights - the eleven RFC 4314 rights - and rights_text, the letters SETACL takes.' },
         post: { summary: 'Grant a permission on a folder (administrator)', description: 'Body: type (required: user, group or anyone); for a user permission account_id or account, for a group permission group_id or group; rights, an object of the eleven names.' }
      },
      '/api/v1/accounts/{address}/folders/{id}/permissions/{pid}': {
         put: { summary: 'Change a permission on a folder (administrator)', description: 'The whole permission, as POST takes it.' },
         delete: { summary: 'Revoke a permission on a folder (administrator)' }
      },
      '/api/v1/diagnostics/run': { post: { summary: 'Run the server\'s diagnostic tests', description: 'What the desktop program\'s Diagnostics page runs over COM: the MX records of a domain hosted here, an outbound connection on port 25 to a domain elsewhere, a connection to this server\'s own ports, the data and backup directories, the IP ranges. Body: local_domain and test_domain. Each result: name, description, success and details. The tests connect out and wait for their timeouts, so the answer can take some seconds. Refused for read-only keys.' } },
      '/api/v1/backup': {
         get: { summary: 'The backup manager\'s status text and the last lines of the backup log' },
         post: { summary: 'Start a backup with the configured settings', description: 'Runs on the maintenance queue; poll GET for the outcome.' }
      },
      '/api/v1/backup/restore': {
         post: {
            summary: 'Restore a backup archive',
            description: 'Body: file (the archive\'s path on this server, required), passphrase (the one the archive was encrypted with), and restore_settings, restore_domains and restore_messages. Everything is checked before anything is changed.',
            requestBody: body({
               file: { type: 'string', description: 'The archive\'s path on the server' },
               passphrase: { type: 'string', writeOnly: true, description: 'The passphrase an encrypted archive was written with' },
               restore_settings: { type: 'boolean' },
               restore_domains: { type: 'boolean' },
               restore_messages: { type: 'boolean' }
            }, ['file'])
         }
      },
      '/api/v1/backup/archives': {
         get: { summary: 'The backup archives in the backup destination', description: 'Every archive this server wrote into its backup destination, newest first.' }
      },
      '/api/v1/backup/{archive}/mailboxes/{address}': {
         get: { summary: 'One account\'s folders and messages as a backup archive holds them', description: 'The archive is a name the listing answers; each message has the id the restore route takes.' }
      },
      '/api/v1/backup/{archive}/mailboxes/{address}/restore': {
         post: {
            summary: 'Restore one mailbox, or one message, from a backup archive',
            description: 'Body: mode (merge or replace), message, folder and passphrase. Merge adds what is missing; replace empties every live folder first.',
            requestBody: body({
               mode: { type: 'string', enum: ['merge', 'replace'] },
               message: { type: 'integer' },
               folder: { type: 'string' },
               passphrase: { type: 'string', writeOnly: true }
            }, [])
         }
      },
      '/api/v1/backup/restore-mailbox/{job}': {
         get: { summary: 'What became of one mailbox restore' }
      }
   }
};

const HOSTILE = 'evil<img src=x onerror=alert(1)>.test';

/* What GET /api/v1/session answers. The page reads it once at sign-in and uses
   it for one thing: leaving out a view this role may not use. Narrowed by the
   run below, to prove that a page a role may not use is not shown. */
let whoami = null;

const ROLE_CATALOGUE = [
   { name: 'serveradmin', description: 'Every permission, in every domain.', builtin: true,
     permissions: 'administer_server,view_configuration,view_accounts,view_logs,manage_roles' },
   { name: 'helpdesk', description: 'Resets a password, unlocks an account and reads a message trace.', builtin: true,
     permissions: 'view_accounts,view_message_trace,reset_password,unlock_account' },
   { name: 'auditor', description: 'Reads everything and changes nothing, the audit trail included.', builtin: true,
     permissions: 'view_configuration,view_accounts,view_logs,view_audit' },
   { name: 'logreader', description: 'Reads the logs.', builtin: false, permissions: 'view_logs' }
];

// A domain's DKIM key rotation as GET /api/v1/domains/{domain}/dkim/rotation
// answers it (DkimRotation.cpp's status document): one signing key, and for
// example.com an old selector still retiring whose last check found a record
// that is not its key - the detail is hostile, as anything the server repeats
// from DNS can be.
const ROTATION_NOW = 1789430400;
function rotationSlot(domain, selector, action, check) {
   return { selector, key_file: '/var/lib/hmailserver/DKIM/' + selector + '._domainkey.' + domain + '.pem', generated: true,
      records: [{ host: selector + '._domainkey.' + domain, type: 'TXT', value: 'v=DKIM1; k=rsa; p=KEY' + selector, action }], check };
}
function rotationDoc(domain, current, retiring) {
   const doc = { domain, now: ROTATION_NOW, policy: { enabled: false, interval_days: 180, propagation_hours: 72, retention_days: 7, key_bits: 2048 },
      phase: retiring ? 'idle' : 'off', current: null, next: null, retiring: null, retired: null, waiting: [], last_error: null };
   if (current) {
      doc.current = Object.assign(rotationSlot(domain, current, 'keep', { status: 'matches', checked: ROTATION_NOW, detail: '' }), { since: ROTATION_NOW - 86400, next_rotation_due: 0 });
   }
   if (retiring) {
      doc.retiring = Object.assign(rotationSlot(domain, retiring, 'keep', { status: 'different', checked: ROTATION_NOW, detail: HOSTILE }), { since: ROTATION_NOW - 3600, retire_due: ROTATION_NOW + 6 * 86400 });
      doc.waiting.push({ step: 'retire', reason: 'retention', until: ROTATION_NOW + 6 * 86400, detail: '' });
   } else {
      doc.waiting.push({ step: 'start', reason: 'not_scheduled', until: 0, detail: '' });
   }
   return doc;
}

// A domain's S/MIME authority as GET /api/v1/domains/{domain}/smime-ca answers
// it (RestApiSmimeAuthority.cpp): none until the page creates one, then a root
// whose facts the page draws and whose PEM it hands out.
const SMIME_NOW = 1789430400;
function smimeRoot(domain, id, key) {
   return { id, subject: '/CN=' + domain + ' S/MIME Authority', fingerprint: String(id).repeat(64).slice(0, 64), key_type: key === 'rsa3072' ? 'RSA-3072' : 'P-256',
      not_before: SMIME_NOW, not_after: SMIME_NOW + 3652 * 86400, created: SMIME_NOW, retired: 0, published: true, key_opens: true, next_serial: 3,
      certificate: 'MIIROOT' + id, pem: '-----BEGIN CERTIFICATE-----\r\nMIIROOT' + id + '\r\n-----END CERTIFICATE-----\r\n',
      root_path: '/smime-ca/' + domain + '/' + id + '.pem', crl_path: '/smime-ca/' + domain + '/' + id + '.crl' };
}
function smimeAuthorityDoc(domain) {
   return { domain, authority: null, retired: [], issued: [], base_url: '', root_path: '/smime-ca/' + domain + '.pem', crl_path: '/smime-ca/' + domain + '.crl', root_url: '', crl_url: '' };
}

const state = {
   // GET /api/v1/acme's answer, or null for a status refused to the credential
   // (the route then answers as one this server does not have).
   acme: null,
   // What the recorded server has been asked to do, for the checks that count it.
   reputationChecks: 0,
   // The domains' S/MIME authorities, made as the page asks for them.
   smimeAuthorities: {},
   auditReverts: 0,
   checklistReads: 0,
   // The setting profiles the checklist view offers under its items
   // (docs/Profiles.md): the list, the diff of each, and what an apply, a revert
   // and a discard were sent - the recorded server keeps one way back, as the
   // real one does, and refuses an apply that needs a confirmation without it.
   profileApplies: [],
   profileReverts: 0,
   profileDiscards: 0,
   profileDeclines: 0,
   profileDeclined: false,
   profileSnapshot: null,
   profileDiffs: {
      office: {
         name: 'office', title: 'A small office server', summary: 'One server for an office.', includes: null,
         steps: ['Give the submission port STARTTLS with a certificate.'],
         entries: [
            { store: 'database', name: 'usespf', value: '1', current: '0', stored: true, changes: true, why: 'a forged sender is the commonest spam',
              caution: null, confirm: false, route: 'PUT /api/v1/settings/antispam use_spf', applies: 'now', levels: null, environment: null, refused: null },
            { store: 'database', name: 'maxmessagesize', value: '25600', current: '25600', stored: true, changes: false, why: '25 MB',
              caution: null, confirm: false, route: 'PUT /api/v1/settings max_message_size_kb', applies: 'now', levels: 'limit', environment: null, refused: null },
            { store: 'ini', name: 'QuarantineEnabled', value: '1', current: '0', stored: false, changes: true, why: 'spam is held',
              caution: null, confirm: false, route: 'PUT /api/v1/settings/ini/QuarantineEnabled', applies: 'now', levels: null, environment: 'HM_SETTING_QuarantineEnabled', refused: null }
         ],
         changes: 1, unchanged: 1, skipped: 1, refused: 0, confirmation_needed: [], diff_token: 'tok-office', applicable: true, blocked: null, applied: null, snapshot: null
      },
      gateway: {
         name: 'gateway', title: 'Behind a filtering gateway', summary: 'Behind a gateway.', includes: null, steps: [],
         entries: [
            { store: 'database', name: 'usegreylisting', value: '0', current: '1', stored: true, changes: true, why: 'the gateway has filtered',
              caution: 'turns_filter_off', confirm: true, route: 'PUT /api/v1/settings/antispam greylisting_enabled', applies: 'now', levels: null, environment: null, refused: null },
            { store: 'ini', name: 'SMTPXClientEnabled', value: '1', current: '1', stored: true, changes: false, why: 'the gateway passes the client',
              caution: 'trusts_addresses', confirm: false, route: 'PUT /api/v1/settings/ini/SMTPXClientEnabled', applies: 'now', levels: null, environment: null, refused: null },
            // A window of days the profile would shorten: marked, with the server's
            // note that a revert does not bring back what the sweep deletes.
            { store: 'ini', name: 'MessageTraceRetentionDays', value: '90', current: '365', stored: true, changes: true, why: 'three months of the trace',
              caution: 'purges', confirm: true, note: 'The next sweep deletes what the shorter window no longer keeps; a revert puts the number back, not what was deleted.',
              route: 'PUT /api/v1/settings/ini/MessageTraceRetentionDays', applies: 'now', levels: null, environment: null, refused: null }
         ],
         changes: 2, unchanged: 1, skipped: 0, refused: 0,
         confirmation_needed: [{ store: 'database', name: 'usegreylisting', caution: 'turns_filter_off', current: '1', value: '0' },
            { store: 'ini', name: 'MessageTraceRetentionDays', caution: 'purges', current: '365', value: '90' }],
         diff_token: 'tok-gateway', applicable: true, blocked: null, applied: null, snapshot: null
      }
   },
   // The sample domain, which the checklist view draws beside the eight items.
   // It starts absent, is made by the button and is taken away by the other one.
   // The state document carries no password: the server mints one per creation and
   // keeps only its hash, so this is what a page can read back.
   sampleDomain: {
      state: 'absent', domain: 'example.test', domain_id: 0, exists: false,
      list: 'everyone@example.test', password_shown_once: true,
      accounts: ['alice@example.test', 'bob@example.test', 'carol@example.test']
   },
   // The newest journal row per address, written by a move.
   moves: {},
   // The accounts' entries in the organisation's directory, by address.
   directory: {},
   // The booking policies of rooms and equipment, by address.
   bookingPolicies: {},
   assignments: [{ address: 'anna@example.com', role: 'helpdesk', domains: '' }],
   status: { version: '6.3.3', state: 3, processedMessages: 1234, spamMessages: 56, virusesRemoved: 7, sessions: { smtp: 3, imap: 12, pop3: 1 } },
   // What GET /api/v1/store answers: the message store measured, compressed.
   store: { measured: 1790000000, measuring: false, messages: 120, logical_bytes: 52428800, stored_bytes: 20971520, compressed_files: 100,
            blobs: 3, blob_bytes: 5242880, blob_references: 9, saved_bytes: 26214400, saved_percent: 50, compression: true, dedup_min_kb: 256 },
   drain: { draining: false, since: '', sessions: 0, deliveries: 0, drained: false },
   // What GET /api/v1/server/settings/pending answers: a key marked reinitialize
   // saved and not yet running. A reinitialise takes it.
   pendingSettings: [{ name: 'ManageSieveServerPort', stored: '4190', running: '0', applies: 'reinitialize', secret: false }],
   domains: [
      domainRecord('example.com', true, 'postmaster@example.com'),
      domainRecord(HOSTILE, false, ''),
      domainRecord('second.example', true, '')
   ],
   // Each domain's own allow and block list, made on first read.
   domainSenders: { 'example.com': [{ id: 901, scope: 'domain', value: 'partner.example', blocked: false, reason: 'the partner', expires: false, expired: false }] },
   // The domain's spam traps, and what has walked into one.
   spamTraps: { 'example.com': [{ id: 701, address: 'never-used@example.com', local_part: 'never-used', created: '2026-09-15 08:00:00' }] },
   // The journaling rules, server-wide: one domain rule that is on, one
   // account rule that is off, so the listing, the switch and the delete all
   // have something to work on.
   journalRules: [
      { id: 601, scope: 'domain', target: 'example.com', journal_address: 'archive@compliance.example', direction: 'both', mode: 'report', enabled: true, note: 'the auditors' },
      { id: 602, scope: 'account', target: 'test@example.com', journal_address: 'copy@second-system.example', direction: 'outbound', mode: 'copy', enabled: false, note: '' }
   ],
   trapHits: [{ id: 801, time: '2026-09-15 09:12:00', address: 'never-used@example.com', domain: 'example.com',
      ip_address: '203.0.113.9', sender: 'harvester@spammer.example', banned: true, blocked: true,
      note: 'banned for 1440 minutes; sender blocked for 30 days' }],
   rotations: {
      'example.com': rotationDoc('example.com', 'jan2026', 'old2025'),
      [HOSTILE]: rotationDoc(HOSTILE, null, null),
      'second.example': rotationDoc('second.example', null, null)
   },
   // One value, three levels (docs/SettingLevels.md): the overlay's rows,
   // domain by domain and account by account.
   levels: { domains: {}, accounts: {} },
   domainAliases: {
      'example.com': [{ id: 1, name: 'example.net' }],
      [HOSTILE]: [],
      'second.example': []
   },
   accounts: {
      'example.com': [Object.assign(accountRecord('anna@example.com', true), { first_name: 'Anna', admin_level: 'domain', sieve_script: 'require ["fileinto"];\r\nif header :contains "subject" "[SPAM]" { fileinto "Junk"; }' }),
         accountRecord('bob@example.com', false)],
      [HOSTILE]: [accountRecord('x@' + HOSTILE, true)],
      'second.example': []
   },
   lists: {
      'example.com': [{ address: 'all@example.com', active: true, require_auth: false, mode: 'public', require_sender_address: '', moderator_address: '', bounce_address: '', membership: '',
         members: ['anna@example.com', 'bob@example.com'] }],
      [HOSTILE]: [],
      'second.example': []
   },
   aliases: {
      'example.com': [{ name: 'info@example.com', value: 'anna@example.com', active: true }],
      [HOSTILE]: [],
      'second.example': []
   },
   fetchAccounts: {
      'anna@example.com': [Object.assign(fetchRecord(7, 'ISP POP3'), { server_address: 'pop.isp.example', port: 995, connection_security: 'tls', username: 'anna.isp', next_download_time: '2026-09-14 10:30:00' })]
   },
   // Two migrations: one from an IMAP server under way, with a job copying and
   // a job that failed, and one from Google Workspace with no jobs yet.
   migrations: [
      migrationRecord(41, 'Old mail server', { host: 'imap.old.example', state: 'running', started: 1789000100 }),
      migrationRecord(42, 'Google Workspace', { kind: 'google', host: 'imap.gmail.com', oauth_provider: 'google' })
   ],
   migrationJobs: {
      41: [migrationJob(411, 41, 'anna@old.example', 'anna@example.com', { state: 'running', folders_total: 2, folders_done: 1, messages_total: 300, messages_copied: 120, bytes: 4800000, skipped: 2, attempts: 1, started: 1789000100 }),
         migrationJob(412, 41, 'bert@old.example', 'carla@example.com', { state: 'failed', last_error: 'the source refused the sign-in: NO [AUTHENTICATIONFAILED] Invalid credentials', attempts: 24 })],
      42: []
   },
   migrationReports: {
      411: { job: 411, source_user: 'anna@old.example', target: 'anna@example.com', state: 'running', phase: 'mail', attempts: 1, bytes: 4800000, duration_seconds: 95,
         messages_total: 300, messages_copied: 120, messages_skipped: 2, first_message: 1600000000, last_message: 1788990000, folders_left_out: 0,
         folders: [
            { source: 'INBOX', target: 'INBOX', special_use: '', source_messages: 250, copied: 100, skipped: 2, skipped_reasons: { too_large: 2 }, uidvalidity_restarts: 0, note: '' },
            { source: 'Sent', target: 'Sent Items', special_use: '\\Sent', source_messages: 50, copied: 20, skipped: 0, skipped_reasons: {}, uidvalidity_restarts: 1,
               note: 'the source changed its UIDVALIDITY; the folder was taken again and matched by Message-ID' }
         ],
         contacts: { state: 'skipped', copied: 0, skipped: 0, skipped_reasons: {}, reason: 'the source answers no CardDAV at /.well-known/carddav' },
         calendars: { state: 'done', copied: 3, skipped: 1, skipped_reasons: { no_uid: 1 }, reason: '' },
         errors: [{ at: 1789000160, attempt: 1, error: 'the connection to imap.old.example was closed' }] }
   },
   suppressions: {
      'example.com': [suppressionRecord('gone@remote.test', 'bounce', 'hard', '5.1.1')]
   },
   // One published key, as OpenPgpKeyStore::RowJson emits it with the address.
   openPgpKeys: {
      'example.com': [{ id: 1, address: 'anna@example.com', fingerprint: 'c29b6cce361e73ba13724cb8bbee452b9b0a0bb7', version: 4,
         algorithm: 22, algorithm_name: 'EdDSA', bits: 256, user_id: 'Anna <anna@example.com>', created: 1789526547, expires: 0,
         bytes: 413, uploaded: 1789526600, uploaded_by: 'administrator', hashed_local_part: 'ftnwpzeeynpbixr6qyd8grpy8xuzcqwt',
         advanced_url: 'https://openpgpkey.example.com/.well-known/openpgpkey/example.com/hu/ftnwpzeeynpbixr6qyd8grpy8xuzcqwt?l=anna',
         direct_url: 'https://example.com/.well-known/openpgpkey/hu/ftnwpzeeynpbixr6qyd8grpy8xuzcqwt?l=anna', published: true }]
   },
   bounceCounts: {
      'example.com': [{ address: 'gone@remote.test', hard: 2, soft: 1, mailbox_full: 0, policy: 0, other: 0, consecutive_hard: 2, last_status: '5.1.1', last_category: 'hard',
         last_bounce: '2026-09-14 10:00:00', last_delivered: '', suppressed: true }]
   },
   bounceSweeps: 0,
   complaints: {
      'example.com': [
         { address: 'reader@provider.test', sender: 'anna@example.com', account_id: 1, feedback_type: 'abuse', counts: true,
            provider: 'SomeGenerator/1.0', source_ip: '192.0.2.1', reported_domain: 'example.com', message_id: 'abc.123@example.com',
            subject: 'Our newsletter', arrival: '2026-09-15 09:00:00', received: '2026-09-15 09:04:00' },
         { address: '', sender: 'anna@example.com', account_id: 1, feedback_type: 'not-spam', counts: false,
            provider: 'SomeGenerator/1.0', source_ip: '', reported_domain: 'example.com', message_id: '',
            subject: 'Our newsletter', arrival: '', received: '2026-09-15 10:00:00' }
      ]
   },
   complaintSweeps: 0,
   // The DMARC aggregate and TLS-RPT reports read for a domain, as
   // DomainReports::SummaryJson, ReportsJson, ReportJson and TlsReportsJson
   // emit them: one report from google.com covering one day, with two records.
   dmarc: {
      'example.com': {
         reports: [
            { id: 31, org: 'google.com', email: 'noreply-dmarc-support@google.com', report_id: '12345678901234567890', begin: 1789430400, end: 1789516799,
               policy_domain: 'example.com', adkim: 'r', aspf: 'r', p: 'quarantine', sp: 'reject', pct: 100, messages: 15, passed: 12, received: 1789520000,
               source: 'google.com!example.com!1789430400!1789516799.xml.gz', records_kept: 2, message_dmarc: 'pass', message_from: 'google.com',
               records: [
                  { source_ip: '192.0.2.10', count: 12, disposition: 'none', dkim: 'pass', spf: 'pass', header_from: 'example.com', envelope_from: 'example.com',
                     dkim_auth: 'd=example.com s=s1 result=pass', spf_auth: 'd=example.com scope=mfrom result=pass' },
                  { source_ip: '198.51.100.7', count: 3, disposition: 'quarantine', dkim: 'fail', spf: 'fail', header_from: 'example.com', envelope_from: '',
                     dkim_auth: '', spf_auth: 'd=forger.test scope=mfrom result=pass' }
               ] }
         ],
         tls: [
            { id: 41, org: 'Company-X', contact: 'sts-reporting@company-x.example', report_id: '5065427c-23d3-47ca-b6e0-946ea0e8c4be', begin: 1789430400, end: 1789516799,
               policy_type: 'sts', policy_domain: 'example.com', successful: 5326, failed: 303,
               failures: [{ result_type: 'certificate-expired', sending_mta_ip: '2001:db8:abcd:12::1', receiving_mx_hostname: 'mx1.mail.example.com', receiving_ip: '203.0.113.6', count: 100 }],
               received: 1789520000, source: 'company-x.example!example.com!1789430400!1789516799!001.json.gz' }
         ]
      }
   },
   domainReportSweeps: 0,
   fetcherDown: false,
   backup: { status: '', log: ['2026-09-13 02:00:00 Backup started.', '2026-09-13 02:00:09 Backup completed.'] },
   backupRunning: false,
   backupArchives: { destination: '/var/backups/hmailserver', archives: [
      { name: 'HMBackup 2026-09-21 020000.7z', path: '/var/backups/hmailserver/HMBackup 2026-09-21 020000.7z', created: '2026-09-21 02:00:00', size: 1048576, encrypted: true },
      { name: 'HMBackup 2026-09-20 020000.7z', path: '/var/backups/hmailserver/HMBackup 2026-09-20 020000.7z', created: '2026-09-20 02:00:00', size: 1000000, encrypted: false }
   ] },
   archivedMailbox: { archive: '/var/backups/hmailserver/HMBackup 2026-09-20 020000.7z', address: 'ann@example.test', folders: [
      { path: 'INBOX', subscribed: true, special_use: 0, messages: [
         { id: 4101, folder: 'INBOX', subject: 'The Tuesday figures', from: 'bob@example.test', date: '2026-09-15 09:12:00', size: 2048, flags: 1, keywords: '' },
         { id: 4102, folder: 'INBOX', subject: 'Lunch?', from: 'carol@example.test', date: '2026-09-16 11:40:00', size: 900, flags: 0, keywords: '' } ] },
      { path: 'Projects.Alpha', subscribed: true, special_use: 0, messages: [
         { id: 4103, folder: 'Projects.Alpha', subject: 'Alpha kick-off', from: 'dave@example.test', date: '2026-09-17 15:00:00', size: 4096, flags: 1, keywords: '$label1' } ] }
   ], messages: 3, subjects: true, subjects_note: '' },
   mailboxRestoreJobs: {},
   scriptReloads: 0,
   scriptProblem: '',
   // What POST /api/v1/settings/breach-check/test answers.
   breachProbe: { reachable: true, status: 200, entries: 812, padded: true, milliseconds: 41 },
   messages: [
      { id: 1, name: 'BOUNCE_MESSAGE', text: 'Your message could not be delivered.' },
      { id: 2, name: 'VIRUS_NOTIFICATION', text: 'A virus was found in a message sent to you.' }
   ],
   collections: {
      '/api/v1/dns-blacklists': [{ id: 11, active: true, dns_host: 'zen.spamhaus.org', expected_result: '127.0.0.2-11', reject_message: 'Listed at Spamhaus', score: 5 }],
      '/api/v1/surbl-servers': [{ id: 12, active: false, dns_host: 'multi.surbl.org', expected_result: '', reject_message: '', score: 5 }],
      '/api/v1/whitelist-addresses': [{ id: 13, lower_ip: '10.0.0.1', upper_ip: '10.0.0.9', email_address: '*@partner.example', description: 'The partner' }],
      '/api/v1/blocked-senders': [{ id: 14, address: 'spam.example', score: 100, description: 'A spam domain' }],
      '/api/v1/blocked-attachments': [{ id: 15, wildcard: '*.exe', description: 'Programs' }],
      '/api/v1/greylisting-white-addresses': [{ id: 16, ip_address: '10.0.0.*', description: 'The office' }],
      '/api/v1/incoming-relays': [{ id: 17, name: 'Front relay', lower_ip: '192.0.2.1', upper_ip: '192.0.2.1' }],
      '/api/v1/sender-list': [{ id: 18, scope: 'server', value: 'partner.example', blocked: false, reason: 'Their invoices kept scoring', expires: false, expires_time: '', expired: false, created: '2026-09-15 08:00:00' }],
      '/api/v1/greylisting-bypass': [{ id: 19, pattern: '.mail.partner.example', match: 'helo', require_spf: true, require_dkim: true, require_dmarc: false, note: 'Their gateway retries on a timer' }],
      '/api/v1/domain-policy-list': [{ id: 19, pattern: '.sloppy-partner.example', match_on: 'helo', action: 'white', note: 'Their SPF has been broken since 2019', created: '2026-09-18 08:00:00' }],
      '/api/v1/never-ban-names': [{ id: 20, pattern: '.google.com', note: "Gmail's outbound hosts", created: '2026-10-01 09:00:00' }]
   },
   groups: [{ id: 3, name: 'Support' }, { id: 4, name: 'Everyone' }],
   groupMembers: { 3: [{ id: 30, group_id: 3, account_id: 1, account: 'anna@example.com' }], 4: [] },
   ranges: [
      Object.assign({ id: 1, name: 'My computer', lower: '127.0.0.1', upper: '127.0.0.1', priority: 15, expires: false, expires_time: '' }, RANGE_CREATE_DEFAULTS,
         { require_auth_local_to_remote: false, require_auth_remote_to_remote: false, deliver_local_to_remote: true, deliver_remote_to_remote: true }),
      Object.assign({ id: 2, name: 'Internet', lower: '0.0.0.0', upper: '255.255.255.255', priority: 10, expires: false, expires_time: '' }, RANGE_CREATE_DEFAULTS),
      Object.assign({ id: 3, name: 'Auto-ban: 203.0.113.9', lower: '203.0.113.9', upper: '203.0.113.9', priority: 20, expires: true, expires_time: '2026-09-14 11:30:00' }, RANGE_CREATE_DEFAULTS,
         { allow_smtp: false, allow_imap: false, allow_pop3: false })
   ],
   queue: [
      { id: 41, from: 'anna@example.com', recipients: 'far@away.example', next_try: '2026-09-14 10:00:00' },
      { id: 42, from: 'bob@example.com', recipients: 'gone@nowhere.example', next_try: '2026-09-14 10:05:00' },
      // Held for a sandbox's verdict: the queue lists it, marked held, and the
      // page draws it in the box of its own rather than as a row of this table.
      { id: 77, from: 'outsider@far.example', recipients: 'anna@example.com', next_try: '', locked: false, tries: 0, held: 'sandbox', held_since: 1790000000, submissions: ['job-7'] }
   ],
   // What the queue is holding back from: one destination that asked this
   // server to slow down, and the connections open under a ceiling. The reply
   // is the remote's own words, as the route answers them.
   shaping: {
      backoffs: [
         { destination: 'slow.example', since: '2026-09-22 12:00:00', until: '2026-09-22 12:02:00', window_minutes: 2,
           reply: '421 4.7.0 [TSS04] Messages temporarily deferred due to unexpected volume', host: 'mx.slow.example', deferred: 3, probing: false }
      ],
      connections: { destinations: { 'slow.example': 1 }, tenants: { 'example.com': 2 } }
   },
   // What is held for a sandbox's verdict on its attachments: one message whose
   // file is submitted and waiting, and one whose submit has failed twice - a
   // page of two, of three held in all, so that the box says how much it shows.
   sandbox: {
      enabled: true, active: true, timeout_minutes: 15, timeout_action: 'deliver', poll_seconds: 30, max_files: 10, total: 3, offset: 0, count: 2,
      messages: [
         { queue_id: 77, from: 'outsider@far.example', recipients: 'anna@example.com', created: '2026-09-24 09:00:00', held: 1790000000, held_seconds: 42, decision: 'pending',
           files: [{ filename: 'invoice.exe', size: 1234, sha256: 'ab12', submission: 'job-7', state: 'submitted', verdict: '', score: 0, detail: '', errors: 0 }] },
         { queue_id: 78, from: 'stranger@far.example', recipients: 'bob@example.com', created: '2026-09-24 09:01:00', held: 1790000060, held_seconds: 12, decision: 'pending',
           files: [{ filename: 'report.docm', size: 5678, sha256: 'cd34', submission: '', state: 'not_submitted', verdict: '', score: 0, detail: '', errors: 2 }] }
      ]
   },
   // The IP warm-up, beside the queue: one schedule on day 1 of the standard
   // plan, Google at its ceiling with one recipient held over it, in the shape
   // GET /api/v1/warmup answers.
   warmup: {
      today: '2026-09-24',
      schedules: [
         { id: 7, address: '192.0.2.10', plan: 'standard', started: '2026-09-24', enabled: true, day: 1, last_day: 30, complete: false, in_bind_setting: false,
           providers: [
              { provider: 'google', sent: 50, deferred: 1, ceiling: 50 },
              { provider: 'microsoft', sent: 3, deferred: 0, ceiling: 50 },
              { provider: 'yahoo', sent: 0, deferred: 0, ceiling: 50 },
              { provider: 'apple', sent: 0, deferred: 0, ceiling: 50 },
              { provider: 'proton', sent: 0, deferred: 0, ceiling: 50 },
              { provider: 'other', sent: 12, deferred: 0, ceiling: 250 }
           ] }
      ]
   },
   tlsa: { records: [{ record: '_25._tcp.mail.example.com. IN TLSA 3 1 1 0123abcd' }, { record: '_465._tcp.mail.example.com. IN TLSA 3 1 1 0123abcd' }] },
   /* The eight pages the Deck gained last: what each of their routes answers,
      in the shape the server answers it - the listings that are bare arrays
      are bare arrays here, because a page that reads an envelope round one of
      them reads undefined and shows nothing. */
   apikeys: [
      { id: 'a1b2c3d4e5f60718', label: 'the monitor', scope: 'readonly', role: '', domains: '', expires: '', allowed_from: '10.0.0.0/8', expired: false },
      { id: '0718f6e5d4c3b2a1', label: 'the old importer', scope: 'full', role: 'helpdesk', domains: 'example.com', expires: '2026-01-01 00:00:00', allowed_from: '', expired: true,
        send_from: '*@example.com', send_per_minute: 10, send_per_day: 500 }
   ],
   // The administrator's own passkeys. created and last_used are seconds since
   // 1970, as the route answers them; the second has never been used.
   passkeys: [
      { id: 12, name: 'Console laptop', created: 1789600000, last_used: 1789700000, transports: 'internal', algorithm: -7, aaguid: 'AAECAwQFBgcICQoLDA0ODw' },
      { id: 13, name: 'Yubico key', created: 1789500000, last_used: 0, transports: 'usb,nfc', algorithm: -7, aaguid: 'AAECAwQFBgcICQoLDA0ODw' }
   ],
   // What the filters held back. The subject is hostile, as a held message's
   // is: it was written by whoever the server refused to deliver for. helo is
   // the name the client gave; sender_domain and helo_domain are the
   // organizations' domains the server computes, which the page groups by.
   quarantine: [
      { id: 41, sender: 'seller@spammer.example', recipients: 'anna@example.com', subject: 'Your invoice is ready', reason: 'Score 12 (SURBL, SPF)', score: 12, size: 34816, created: '2026-09-16 21:04:11',
        helo: 'mail.spammer.example', sender_domain: 'spammer.example', helo_domain: 'spammer.example' },
      { id: 42, sender: 'notice@' + HOSTILE, recipients: 'bob@example.com', subject: HOSTILE, reason: 'Rule: Hold for review', score: 0, size: 2048, created: '2026-09-16 22:15:00',
        helo: 'localhost', sender_domain: '', helo_domain: 'localhost' },
      { id: 43, sender: 'offers@bounces.spammer.example', recipients: 'anna@example.com,bob@example.com,carol@example.com', subject: 'Weekly offers', reason: 'Score 9 (DNSBL)', score: 9, size: 4096, created: '2026-09-16 20:00:00',
        helo: 'mx.spammer-mail.example', sender_domain: 'spammer.example', helo_domain: 'spammer-mail.example' },
      { id: 44, sender: 'billing@supply.example', recipients: 'anna@example.com', subject: 'Invoice 4417', reason: 'Score 22 (SURBL)', score: 22, size: 8192, created: '2026-09-16 19:00:00',
        helo: 'mx.supply.example', sender_domain: 'supply.example', helo_domain: 'supply.example' },
      { id: 45, sender: 'news@supply.example', recipients: 'bob@example.com', subject: 'Newsletter', reason: 'Score 18 (SURBL)', score: 18, size: 8192, created: '2026-09-16 18:00:00',
        helo: 'mx.supply.example', sender_domain: 'supply.example', helo_domain: 'supply.example' }
   ],
   // What the quarantine's allow and block wrote, for the checks to read.
   quarantineListed: [],
   // What the compromised-account detection kept out of the delivery queue,
   // and the review state of the account it locked. The subject is hostile
   // here too: it was written by whoever had the password.
   review: [
      { id: 7, message_id: 9001, account: 'anna@example.com', sender: 'anna@example.com', recipients: 'one@stranger.example, two@stranger.example', subject: 'Invoice attached', size: 4096, held: 1758060000, source: 'smtp',
        reason: '31 messages in the last hour against a busiest hour of 4 in 30 days; 28 of the 31 recipients in the last hour are addresses the account has never written to' },
      { id: 8, message_id: 9002, account: 'anna@example.com', sender: 'anna@example.com', recipients: 'three@stranger.example', subject: HOSTILE, size: 2048, held: 1758060060, source: 'webmail',
        reason: 'submitted while the account was locked for review' }
   ],
   // Mailbox encryption at rest (RestApiMailboxKeys.cpp): each account's key
   // and each domain's recovery key, made as the page asks for them.
   mailboxKeys: {},
   mailboxRecovery: {},
   reviewAccounts: {
      'anna@example.com': { address: 'anna@example.com', mode: 'enforce', exempt: false, sensitivity: 'default', locked: true, locked_at: 1758060000, score: 4, signals: 'volume, recipients',
         reason: '31 messages in the last hour against a busiest hour of 4 in 30 days; 28 of the 31 recipients in the last hour are addresses the account has never written to',
         baseline: { days: 30, history: 212, minimum_history: 20, has_baseline: true, peak_hour: 4, last_hour: 31, recipients_known: 58, held: 2, hours: [8, 9, 10, 11, 14, 15, 16], networks: ['192.0.2.0/24', '2001:db8:1:2::/64'] } }
   },
   archive: [
      { id: 2750, time: '2026-09-16 09:00:00', domain: 'example.com', mailbox: 'anna', direction: 'received', sender: 'client@partner.example', subject: 'The signed contract', recipients: 'anna@example.com', message_id: '<a@partner.example>', path: 'C:\\archive\\example.com\\anna\\a.eml', size: 51200, hold: false },
      { id: 2751, time: '2026-09-16 10:30:00', domain: 'example.com', mailbox: 'anna', direction: 'sent', sender: 'anna@example.com', subject: 'Re: The signed contract', recipients: 'client@partner.example', message_id: '<b@example.com>', path: 'C:\\archive\\example.com\\anna\\b.eml', size: 20480, hold: true },
      { id: 2752, time: '2026-09-15 08:00:00', domain: 'second.example', mailbox: 'carla', direction: 'received', sender: 'list@announce.example', subject: 'The weekly notice', recipients: 'carla@second.example', message_id: '<c@announce.example>', path: 'C:\\archive\\second.example\\carla\\c.eml', size: 8192, hold: false }
   ],
   // eDiscovery: an open case with a finished search, a live hold and a
   // finished export, and a closed case, newest first as the listing is.
   discovery: {
      next: 100,
      cases: [
         { id: 41, name: 'Northwind v. Example', description: 'The supplier dispute', state: 'open', created_by: 'Administrator', created: 1758060000, closed_by: '', closed: 0,
           searches: [{ id: 7, case: 41, state: 'done', query: { custodians: ['anna@example.com'], words: 'contract', scope: 'both' }, ran_by: 'Administrator', ran: 1758060100,
              finished: 1758060160, scanned: 1200, matched: 14, unsearched: 1, truncated: false, error: '' }],
           holds: [{ id: 3, case: 41, scope: 'account', owner_id: 12, name: 'anna@example.com', placed_by: 'Administrator', placed: 1758060050, released_by: '', released: 0, live: true }],
           exports: [{ id: 5, case: 41, search: 7, state: 'done', file: 'export-5.zip', size: 2097152, sha256: '9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08',
              items: 14, withheld: 0, requested_by: 'Administrator', requested: 1758060200, finished: 1758060260, error: '', download: '/api/v1/discovery/cases/41/exports/5' }] },
         { id: 40, name: 'An old matter', description: '', state: 'closed', created_by: 'Administrator', created: 1757000000, closed_by: 'Administrator', closed: 1757100000,
           searches: [], holds: [], exports: [] }
      ]
   },
   // The history is kept, and the counter climbs by five, six and seven over
   // the four samples: the page draws the differences, not the totals.
   metricsEnabled: true,
   metricsSamples: {
      processed_messages_total: [0, 5, 11, 18],
      sessions_smtp: [3, 9, 4, 6]
   },
   update: {
      state: 0, stateName: 'not-checked', runningVersion: '6.3.3', channel: 'stable', checkEnabled: false,
      installKind: 'installer', liveUpdate: true, updateCommand: '', updateAdvice: '',
      availableVersion: '', releaseName: '', publishedAt: '', releaseUrl: '',
      installer: { name: '', url: '', size: 0, digest: '', bundleUrl: '' },
      downloaded: { path: '', signer: '', logTime: '', verifiedAt: '' },
      apply: { status: '', version: '', detail: '' },
      window: { text: '', open: false, error: '' },
      autoDownload: false, backupBeforeApply: true, lastChecked: '', lastError: ''
   },
   srv: {
      target: 'mail.example.com', count: 2,
      records: [
         { domain: 'example.com', service: '_imap._tcp', record: '_imap._tcp.example.com. IN SRV 0 1 143 mail.example.com.' },
         { domain: 'second.example', service: '_submission._tcp', record: '_submission._tcp.second.example. IN SRV 0 1 587 mail.example.com.' }
      ]
   },
   // One tree for every account there is, so the view is asked with whichever
   // address the run still has by the time it reaches it.
   folders: { delimiter: '.', folders: [
      { id: 101, name: 'INBOX', path: 'INBOX', parent_id: -1, subfolders: [
         { id: 102, name: 'Contracts', path: 'INBOX.Contracts', parent_id: 101, subfolders: [] }] },
      { id: 103, name: 'Sent', path: 'Sent', parent_id: -1, subfolders: [] }
   ] },
   permissions: {
      101: [{ id: 9001, folder_id: 101, type: 'user', account_id: 2, account: 'bob@example.com', group_id: 0, group: '',
         rights: { lookup: true, read: true, write_seen: true, write_others: false, insert: false, post: false, create: false, delete_mailbox: false, write_deleted: false, expunge: false, administer: false },
         rights_text: 'lrs' }],
      102: [],
      103: []
   },
   diagnostics: [
      { name: 'Test MX records', description: 'Looks up the MX records of the local domain.', success: true, details: 'mail.example.com (10)' },
      { name: 'Test outbound connection', description: 'Connects to the test domain on port 25.', success: false, details: 'Connection to mx.example.com:25 timed out.' },
      { name: 'Test local connect', description: 'Connects to this server on its own ports.', success: true, details: 'SMTP, IMAP and POP3 answered.' }
   ],
   diagnosticRuns: [],
   webhooks: [
      { name: 'billing', url: 'https://hooks.example/mail', secret_set: true, events: 'message.bounced', domains: 'example.com',
        addresses: '', enabled: true, include_subject: false, max_per_minute: 0, created: 1789000000,
        pending: 1, delivered: 4, dead_lettered: 0, dropped: 0 }
   ],
   webhookQueue: [
      { id: 9, created: 1789000100, attempts: 1, next_attempt: 1789000160, webhook: 'billing', event: 'message.bounced',
        state: 'pending', error: 'the endpoint answered 502', body: '{"event":"message.bounced"}' }
   ],
   inboundHooks: [
      { address: 'support@example.com', url: 'https://helpdesk.example/hooks/mail', secret_set: true, deliver: false, on_failure: 'keep',
        format: 'form', attachments: 'include', include_raw: false, private_network: false, enabled: true, max_size_kb: 10240,
        created: 1789000000, pending: 1, delivered: 3, kept: 1, bounced: 0 }
   ],
   inboundQueue: [
      { id: 41, created: 1789000100, attempts: 5, next_attempt: 0, queue_id: 700, size: 2048, route: 'support@example.com', state: 'kept',
        sender: 'someone@example.net', recipients: 'support@example.com', spam: false, message_kept: true, error: 'the endpoint answered 503' },
      { id: 42, created: 1789000200, attempts: 1, next_attempt: 0, queue_id: 701, size: 1024, route: 'support@example.com', state: 'delivered',
        sender: 'other@example.net', recipients: 'support@example.com', spam: false, message_kept: false, error: '' }
   ],
   rules: [
      { id: 1, name: 'Spam to junk', active: true, all_criteria: true,
         criteria: [{ field: 'subject', header: '', match: 'contains', value: '[SPAM]' }],
         actions: [{ type: 'move_to_folder', folder: 'Junk', value: 'Junk' }] }
   ],
   routes: [
      { id: 5, domain_name: 'partner.example', description: 'The partner', target_smtp_host: 'smtp.partner.example', target_smtp_port: 587,
         number_of_tries: 3, minutes_between_try: 10, relayer_requires_authentication: true, relayer_auth_username: 'relay',
         treat_security_as_local_domain: false, treat_recipient_as_local_domain: false, treat_sender_as_local_domain: false,
         all_addresses: true, connection_security: 'starttls_required', addresses: [] }
   ],
   remotedomains: [
      { id: 7, domain_name: 'bank.example', description: 'The regulated one', active: true, outbound_tls: 'verified',
        require_inbound_tls: true, max_message_size_kb: 20480, max_connections: 2, max_messages_per_minute: 30,
        allow_automatic_replies: false, allow_forwarding: true, callout_enabled: false, callout_host: '',
        callout_port: 25, callout_timeout_seconds: 10, callout_cache_minutes: 60, callout_max_per_minute: 10,
        encrypt: 'smime', encrypt_fallback: 'refuse', encrypt_key_set: true,
        encrypt_key_fingerprint: 'f94894af1dea80795af02d9e52e6fad0d436dc318c18ac4c8250842714a412d5',
        encrypt_key_expires: '2036-09-21T02:31:31Z', encrypt_key_subject: '/CN=gateway.bank.example', encrypt_key_problem: '', encrypt_key_warning: '' },
      // A partner whose key has expired since it was saved - and, as a server
      // that leaked it would, the key text itself, which the page must never
      // draw, prefill or send back.
      { id: 8, domain_name: 'lapsed.example', description: '', active: true, outbound_tls: 'none',
        require_inbound_tls: false, max_message_size_kb: 0, max_connections: 0, max_messages_per_minute: 0,
        allow_automatic_replies: true, allow_forwarding: true, callout_enabled: false, callout_host: '',
        callout_port: 25, callout_timeout_seconds: 10, callout_cache_minutes: 60, callout_max_per_minute: 10,
        encrypt: 'openpgp', encrypt_fallback: 'send_unencrypted', encrypt_key_set: true,
        encrypt_key: 'LEAKED-KEY-MATERIAL',
        encrypt_key_fingerprint: '3c6ff5e3458173dd5db3f4a4a14263c97de6857d',
        encrypt_key_subkey_fingerprint: '4a144aada87aad8cce8a0659eaa03821629d0473',
        encrypt_key_expires: '2027-09-24T00:00:00Z', encrypt_key_subject: 'Alice Fixture <alice@fixtures.test>',
        encrypt_key_problem: 'no message can be encrypted to this key: the key expired', encrypt_key_warning: '' }
   ],
   certificates: [{ id: 2, name: 'mail.example.com', certificate_file: '/etc/ssl/mail.pem', private_key_file: '/etc/ssl/mail.key' }],
   ports: [
      { id: 1, protocol: 'smtp', address: '0.0.0.0', port: 25, connection_security: 'starttls_optional', certificate_id: 2, client_certificate_policy: 'off', client_certificate_ca_file: '', smtp_authentication: 'honeypot' },
      { id: 2, protocol: 'imap', address: '0.0.0.0', port: 143, connection_security: 'none', certificate_id: 0, client_certificate_policy: 'off', client_certificate_ca_file: '', smtp_authentication: 'on' }
   ],
   logs: [
      { name: 'hmailserver_2026-09-14.log', size: 2048, created: '2026-09-14 00:00:01' },
      { name: 'ERROR_hmailserver_2026-09-14.log', size: 10, created: '2026-09-14 00:00:02' }
   ],
   logLines: ['"SMTPD" 1 "2026-09-14 09:00:00.000" "127.0.0.1" "SENT: 220 mail.example.com"', '"SMTPD" 1 "2026-09-14 09:00:01.000" "127.0.0.1" "RECEIVED: QUIT"'],
   // The sessions open now and lately ended, and the lines a follow of one
   // of them answers. Empty until the follow checks fill them, so that the
   // file listing's own checks see the page they always saw.
   sessions: [],
   followLines: [],
   // Whether the route left open sessions out of the listing, which it does
   // past a thousand of them.
   sessionsTruncated: false,
   // The audit trail. Two rows, one made over COM by the Control Panel and one
   // made over REST by an API key, because the column that tells them apart is
   // the point of the page.
   audit: [
      { id: 2, time: 1789000060, actor: 'key:reporting', actor_type: 'apikey', interface: 'REST', address: '192.0.2.9',
        object_type: 'domain', object: 'example.com', action: 'created', detail: 'domainname=example.com', hash: 'bb', previous_hash: 'aa' },
      { id: 1, time: 1789000000, actor: 'Administrator', actor_type: 'administrator', interface: 'COM', address: '',
        object_type: 'setting', object: 'AutoBanMinutes', action: 'updated', detail: 'AutoBanMinutes: 60 -> 120', hash: 'aa', previous_hash: '' }
   ],
   auditIntact: true,
   // The reputation watch. Four answers on purpose, because the page has to draw
   // each of them differently: an address listed, the same address on a list that
   // refused the query, an address no list has anything against, and a list that
   // does not cover that address's family.
   reputation: {
      enabled: true,
      lists: [{ zone: 'bl.example.test', family: 'any', codes: '' }, { zone: 'ipv4.example.test', family: 'ipv4', codes: '' }],
      lists_problem: '',
      addresses: [
         {
            address: '192.0.2.25', family: 'ipv4', sources: ['bind'], checked: 1789000000, listed: true,
            results: [
               { list: 'bl.example.test', status: 'listed', listed: true, code: '127.0.0.2',
                 reason: 'Listed for sending unsolicited mail', checked: 1789000000, listed_since: 1788900000 },
               { list: 'ipv4.example.test', status: 'refused', listed: null, code: '127.255.255.254',
                 reason: 'The list refuses queries that reach it through a public or open resolver.', checked: 1789000000, listed_since: 0 }
            ]
         },
         {
            address: '2001:db8::25', family: 'ipv6', sources: ['setting'], checked: 1789000000, listed: null,
            results: [
               { list: 'bl.example.test', status: 'not_listed', listed: false, code: '', reason: '', checked: 1789000000, listed_since: 0 },
               { list: 'ipv4.example.test', status: 'not_applicable', listed: null, code: '',
                 reason: 'The list covers IPv4 addresses only, so it has nothing to say about this one.', checked: 1789000000, listed_since: 0 }
            ]
         }
      ],
      skipped: [{ address: '10.0.0.5', source: 'rule', reason: 'private',
                  detail: 'A private address. It is not what another server sees, and asking a block list about it would tell the list what this network looks like inside.' }],
      last_checked: 1789000000, next_check: 1789086400, now: 1789000100,
      counts: { addresses: 2, lists: 2, listed: 1, not_listed: 1, unknown: 1, not_applicable: 1, not_checked: 0, skipped: 1 }
   },
   // The first-hour checklist. All four states at once, and the two shapes of
   // finding the page has to draw: the one whose code it knows, rendered from the
   // catalogue with the arguments, and one whose code it does not, where the
   // server's own sentence is what is shown.
   checklist: {
      generated: 1789000000, dns_looked_up: true, done: 3, action_needed: 6, cannot_tell: 1, not_needed: 0,
      items: [
         // The profile item, first, whose code the page renders in its own words
         // rather than the server's (docs/Profiles.md).
         { key: 'profile', state: 'action_needed', findings: [{ state: 'action_needed', code: 'profile_none', args: [], text: 'the server sentence the page does not use' }] },
         { key: 'domain', state: 'done', findings: [{ state: 'done', code: 'domain_count', args: ['1', '1'], text: 'Active domains: 1 of 1.' }] },
         { key: 'account', state: 'done', findings: [{ state: 'done', code: 'account_count', args: ['3'], text: 'Accounts: 3.' }] },
         { key: 'submission', state: 'action_needed', findings: [
            { state: 'action_needed', code: 'submission_plain', args: ['587'], text: 'Port 587 takes submission without TLS, so passwords and mail would cross the network in the clear.' },
            { state: 'done', code: 'submission_tls', args: ['465', 'Example'], text: 'Port 465 offers TLS with the certificate Example.' }] },
         // Passwords only over TLS (docs/SecureDefaults.md): the Internet range takes
         // one in the clear, and its finding carries the one step that puts it right
         // - the IP range update the page offers as a button. The range the server
         // judged done carries none.
         { key: 'sign_in_tls', state: 'action_needed', findings: [
            { state: 'action_needed', code: 'sign_in_plain', args: ['Internet', '25, 110, 143, 587'], text: 'the server sentence the page does not use',
              fix: { method: 'PUT', path: '/api/v1/ipranges/2', body: { require_tls_for_auth: true } } },
            { state: 'done', code: 'sign_in_tls_required', args: ['Branch office'], text: 'the server sentence the page does not use' },
            // A range whose listeners have no TLS to start: no fix - requiring TLS
            // there would lock everybody out - and a button to the Ports view.
            { state: 'action_needed', code: 'sign_in_no_tls', args: ['Lab', '8025'], text: 'the server sentence the page does not use' }] },
         // Port 25 still takes AUTH (GitLab work item #54): no fix - changing the
         // listener arms the ban - and a button to the Ports view.
         { key: 'delivery_port_auth', state: 'action_needed', findings: [
            { state: 'action_needed', code: 'delivery_port_auth_on', args: ['25'], text: 'the server sentence the page does not use' }] },
         { key: 'spf', state: 'action_needed', findings: [{ state: 'action_needed', code: 'record_missing', args: ['example.com', 'SPF', 'example.com'], text: 'example.com: no SPF record at example.com.' }] },
         { key: 'dkim', state: 'cannot_tell', findings: [{ state: 'cannot_tell', code: 'lookup_failed', args: ['example.com', 'DKIM', 'mail._domainkey.example.com'], text: 'example.com: the lookup for mail._domainkey.example.com failed, which says nothing about the DKIM record.' }] },
         { key: 'dmarc', state: 'done', findings: [{ state: 'done', code: 'record_published', args: ['example.com', 'DMARC', '_dmarc.example.com'], text: 'example.com: the DMARC record at _dmarc.example.com is published.' }] },
         { key: 'backup', state: 'action_needed', findings: [{ state: 'action_needed', code: 'backup_none', args: [], text: 'No backup destination is set.' }] },
         { key: 'second_factor', state: 'action_needed', findings: [{ state: 'action_needed', code: 'a_code_this_page_does_not_know', args: [], text: 'The server said this in its own words.' }] }
      ]
   },
   // What this server has changed from stock. Five entries chosen for the five
   // things the page has to get right: a plain ini key, a secret named and not
   // shown, a name the server ships no default for, a property of the other
   // store - so that the column that says which store is exercised too - and one
   // the file's [SettingsOverride] section is forcing, which is a value nothing
   // else on the Deck shows - and one the server's environment sets, from a file,
   // which the server answers masked and marked.
   changed: {
      changed: 6, considered: 391,
      settings: [
         { store: 'ini', name: 'LogLevel', value: '5', default: '9', secret: false, known: true },
         { store: 'ini', name: 'OAuth2HmacSecret', value: '(set)', default: '', secret: true, known: true },
         { store: 'ini', name: 'OurOwnScriptKey', value: 'yes', default: '', secret: false, known: false },
         { store: 'database', name: 'smtpnoofretries', value: '6', default: '4', secret: false, known: true },
         { store: 'ini', name: 'DnsServer', value: '10.0.0.1', default: '', secret: false, known: true, overridden: true },
         { store: 'database', name: 'welcomesmtp', value: '(set)', default: '', secret: true, known: true, environment: 'file' }
      ]
   },
   // The reports. The index says what each source is and how far back it can
   // see; the sections are the tables. One of them is deliberately switched
   // off (the trace), because the page has to SAY so rather than draw an empty
   // table that reads as "nothing happened".
   reportIndex: {
      today: '2026-09-15',
      sections: [
         { name: 'traffic', scope: 'domain', source: 'hm_messagetrace', csv: true, summary: 'Messages in and out.' },
         { name: 'storage', scope: 'server', source: 'hm_metricsamples', csv: true, summary: 'The store per day.' }
      ],
      sources: {
         message_trace: { table: 'hm_messagetrace', enabled: true, retention_days: 30, oldest: '2026-09-01 08:00:00' },
         metric_history: { table: 'hm_metricsamples', enabled: true, retention_days: 7, oldest: '2026-09-08 00:00:00' },
         message_store: { table: 'hm_messages', enabled: true, retention_days: 0, oldest: '' }
      },
      not_answerable: ['Messages processed, delivered, deferred and bounced per domain.']
   },
   reportOmitted: [],
   reportSections: {
      traffic: {
         section: 'traffic', source: 'hm_messagetrace', enabled: true, truncated: false, note: 'Counted from the message trace.',
         columns: ['day', 'domain', 'incoming', 'outgoing', 'failed_incoming', 'failed_outgoing'],
         rows: [
            { day: '2026-09-14', domain: 'example.com', incoming: 12, outgoing: 3, failed_incoming: 1, failed_outgoing: 0 },
            { day: '2026-09-14', domain: 'second.example', incoming: 4, outgoing: 0, failed_incoming: 0, failed_outgoing: 2 },
            { day: '2026-09-15', domain: 'example.com', incoming: 9, outgoing: 5, failed_incoming: 0, failed_outgoing: 0 }
         ],
         totals: { incoming: 25, outgoing: 8, failed_incoming: 1, failed_outgoing: 2 }
      },
      failures: {
         section: 'failures', source: 'hm_messagetrace', enabled: true, truncated: false, note: 'The reason is the SMTP status code.',
         columns: ['domain', 'status', 'reason', 'incoming', 'outgoing', 'total'],
         rows: [{ domain: 'example.com', status: 550, reason: 'the mailbox was unavailable, or the message was refused', incoming: 1, outgoing: 0, total: 1 }]
      },
      senders: {
         section: 'senders', source: 'hm_messagetrace', enabled: true, truncated: false, note: 'Local senders only.',
         columns: ['sender', 'domain', 'messages', 'failed'],
         rows: [{ sender: 'anna@example.com', domain: 'example.com', messages: 8, failed: 0 },
            { sender: 'bob@example.com', domain: 'example.com', messages: 3, failed: 1 }]
      },
      recipients: {
         section: 'recipients', source: 'hm_messagetrace', enabled: true, truncated: false, note: 'Local recipients only.',
         columns: ['recipient', 'domain', 'messages', 'failed'],
         rows: [{ recipient: 'anna@example.com', domain: 'example.com', messages: 21, failed: 1 }]
      },
      mailboxes: {
         section: 'mailboxes', source: 'hm_messages', enabled: true, truncated: false, note: 'A size now, not a history.',
         columns: ['address', 'domain', 'messages', 'bytes', 'megabytes'],
         rows: [{ address: 'anna@example.com', domain: 'example.com', messages: 402, bytes: 41943040, megabytes: 40 }],
         domains: [{ domain: 'example.com', mailboxes: 2, messages: 402, bytes: 41943040, megabytes: 40 }]
      },
      quota: {
         section: 'quota', source: 'hm_messages', enabled: true, truncated: false, note: 'A share now, not a history.',
         columns: ['address', 'domain', 'megabytes', 'quota_megabytes', 'percent', 'warns_at', 'sending_stops_at', 'state'],
         rows: [{ address: 'anna@example.com', domain: 'example.com', megabytes: 40, quota_megabytes: 50, percent: 80, warns_at: 90, sending_stops_at: 95, state: 'ok' }]
      },
      volume: {
         section: 'volume', source: 'hm_metricsamples', enabled: false, truncated: false,
         note: 'The metric history is switched off, so nothing is being recorded.',
         columns: ['day', 'processed', 'delivered', 'deferred', 'bounced', 'spam', 'viruses'],
         rows: []
      },
      storage: {
         section: 'storage', source: 'hm_metricsamples', enabled: true, truncated: false, note: 'Sampled hourly.',
         columns: ['day', 'bytes', 'megabytes', 'messages'],
         rows: [{ day: '2026-09-14', bytes: 41943040, megabytes: 40, messages: 400 },
            { day: '2026-09-15', bytes: 44040192, megabytes: 42, messages: 402 }],
         growth_bytes: 2097152
      }
   }
};

function json(status, payload, headers) {
   return { status, body: payload === undefined ? '' : JSON.stringify(payload), headers: headers || {} };
}
function clone(x) { return JSON.parse(JSON.stringify(x)); }
function segment(path, at) { return decodeURIComponent(path.split('?')[0].split('/')[at]); }

function answer(method, path, headers, raw) {
   // Every body the page sends is JSON but one: the CSV of a migration's
   // jobs, which the server reads as it is.
   let parsed = null;
   if (raw) { try { parsed = JSON.parse(raw); } catch (notJson) { parsed = null; } }
   if (method === 'POST' && path === '/api/v1/session') {
      // A passkey, or a ticket from the identity provider, in place of the
      // password: the server judges them in Authenticate_ and mints the same
      // session, so this answers them the same way. A body carrying neither
      // still needs the password.
      if (parsed && (parsed.passkey || parsed.sso_ticket)) { signedIn = true; return json(201, { session: true }); }
      if (!headers.Authorization) { return json(401, { error: 'A credential is required.' }, { 'WWW-Authenticate': 'Basic realm="hMailServer"' }); }
      if (headers.Authorization !== 'Basic ' + Buffer.from('Administrator:a-real-password').toString('base64')) {
         return json(401, { error: 'The password is wrong.' }, { 'WWW-Authenticate': 'Basic realm="hMailServer"' });
      }
      if (otpRequired && headers['X-hMailServer-OTP'] !== '123456') { return json(401, { error: 'A one-time code is required.', second_factor: 'required' }); }
      signedIn = true;
      return json(201, { session: true });
   }
   if (method === 'DELETE' && path === '/api/v1/session') { signedIn = false; return json(200, { ended: true }); }
   if (path === '/') { restartProbes += 1; return restartProbes < 3 ? { status: 503, body: '', headers: {} } : { status: 200, body: '<!doctype html>', headers: {} }; }
   // A catalogue, unauthenticated as the page is: the file from the
   // repository, or 404 when there is no such language or none is installed.
   if (method === 'GET' && /^\/deck-lang\/[A-Za-z-]+\.json$/.test(path)) {
      const file = nodePath.join(CATALOGUES, path.slice(11, -5) + '.json');
      if (catalogueMissing || !fs.existsSync(file)) { return json(404, { error: 'no such language' }); }
      const data = JSON.parse(fs.readFileSync(file, 'utf8'));
      if (droppedKey) { delete data[droppedKey]; }
      if (catalogueTamper) { return json(200, catalogueTamper(data)); }
      return json(200, data);
   }
   if (method === 'GET' && path === '/api/v1/help') {
      return json(200, { wiki: helpWikiBase, tag: 'deadbeefdeadbeef', pages: helpPages() });
   }
   if (method === 'GET' && /^\/api\/v1\/help\/[A-Za-z0-9]+$/.test(path)) {
      const file = nodePath.join(DOCS, path.slice(13) + '.md');
      if (!fs.existsSync(file)) { return json(404, { error: 'no such page' }); }
      return { status: 200, body: fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n'), headers: { 'Content-Type': 'text/markdown; charset=utf-8' } };
   }
   if (path === '/api/v1/signin-methods' && method === 'GET') {
      return json(200, Object.assign({}, deckSignInMethods));
   }
   if (path === '/api/v1/passkeys/challenge' && method === 'POST') {
      return json(200, { challenge: 'Y2hhbGxlbmdl', rp_id: 'deck.test', timeout: 300000 });
   }
   if (!signedIn) { return json(401, { error: 'Not signed in.' }, { 'WWW-Authenticate': 'Basic realm="hMailServer"' }); }
   if (method !== 'GET' && headers['X-Requested-With'] !== 'hMailServer') { return json(403, { error: 'A write from a browser session must carry X-Requested-With.' }); }
   if (method !== 'GET' && nextRefusal) { const sentence = nextRefusal; nextRefusal = null; return json(400, { error: sentence }); }
   if (method !== 'GET' && nextRefusalAnswer) { const refusal = nextRefusalAnswer; nextRefusalAnswer = null; return json(refusal.status, refusal.body); }

   if (path === '/api/v1/status') { return json(200, state.status); }
   if (path === '/api/v1/store') { return json(200, state.store); }
   if (path === '/api/v1/openapi.json') { return json(200, spec); }

   if (path === '/api/v1/domains' && method === 'GET') { return json(200, state.domains); }
   if (path === '/api/v1/domains' && method === 'POST') {
      const name = String(parsed.name || '').trim();
      if (!name) { return json(400, { error: 'name is required' }); }
      if (state.domains.some((d) => d.name === name)) { return json(409, { error: 'domain already exists' }); }
      const record = domainRecord(name, parsed.active !== false, parsed.postmaster || '');
      state.domains.push(record);
      state.accounts[name] = [];
      state.domainAliases[name] = [];
      return json(201, record);
   }
   if (/^\/api\/v1\/domains\/[^/]+$/.test(path)) {
      const name = segment(path, 4);
      const at = state.domains.findIndex((d) => d.name === name);
      if (at < 0) { return json(404, { error: 'domain not found' }); }
      if (method === 'GET') { return json(200, Object.assign({}, state.domains[at], levelAnswer(name, null))); }
      if (method === 'PUT') {
         if (!('active' in parsed) || parsed.active === null) { return json(400, { error: 'active is required' }); }
         for (const key of Object.keys(parsed)) {
            if (DOMAIN_KEYS.indexOf(key) < 0 && LEVEL_KEYS_DOMAIN.indexOf(key) < 0) { return json(400, { error: 'unknown field: ' + key }); }
         }
         if ('recall_days' in parsed && parsed.recall_days !== null && (parsed.recall_days < 1 || parsed.recall_days > 365)) {
            return json(400, { error: 'recall_days must be between 1 and 365.' });
         }
         const record = state.domains[at];
         state.levels.domains[name] = state.levels.domains[name] || {};
         storeLevels(state.levels.domains[name], parsed, LEVEL_KEYS_DOMAIN);
         Object.keys(parsed).forEach((key) => {
            if (key === 'relay_password') { secrets.relay_password = parsed[key]; return; }
            if (LEVEL_KEYS_DOMAIN.indexOf(key) >= 0) { return; }
            record[key] = parsed[key];
         });
         if (record.name !== name) {
            state.rotations[record.name] = state.rotations[name];
            delete state.rotations[name];
            state.accounts[record.name] = state.accounts[name] || [];
            delete state.accounts[name];
            state.domainAliases[record.name] = state.domainAliases[name] || [];
            delete state.domainAliases[name];
         }
         return json(200, record);
      }
      if (method === 'DELETE') {
         state.domains.splice(at, 1);
         delete state.accounts[name];
         delete state.domainAliases[name];
         return json(200, { deleted: true });
      }
   }
   if (/^\/api\/v1\/domains\/[^/]+\/spam-traps\/hits(\?.*)?$/.test(path) && method === 'GET') {
      const domain = segment(path, 4);
      return json(200, { items: state.trapHits.filter((h) => h.domain === domain) });
   }
   if (/^\/api\/v1\/domains\/[^/]+\/spam-traps$/.test(path)) {
      const domain = segment(path, 4);
      if (!state.domains.some((d) => d.name === domain)) { return json(404, { error: 'no such domain' }); }
      const traps = state.spamTraps[domain] || (state.spamTraps[domain] = []);
      if (method === 'GET') { return json(200, { domain: domain, spam_traps: traps }); }
      if (method === 'POST') {
         const wanted = String(parsed.address || '').trim().toLowerCase();
         if (!wanted) { return json(400, { error: 'address is required: an address in this domain, or the part before the @' }); }
         const address = wanted.indexOf('@') < 0 ? wanted + '@' + domain : wanted;
         const existing = traps.filter((t) => t.address === address)[0];
         if (existing) { return json(200, existing); }
         const trap = { id: nextId++, address: address, local_part: address.split('@')[0], created: '2026-09-16 00:00:00' };
         traps.push(trap);
         return json(201, trap);
      }
   }
   if (/^\/api\/v1\/journal-rules$/.test(path)) {
      if (method === 'GET') { return json(200, state.journalRules); }
      if (method === 'POST') {
         const target = String(parsed.target || '').trim().toLowerCase();
         const address = String(parsed.journal_address || '').trim().toLowerCase();
         if (!target) { return json(400, { error: 'A journal rule must name a domain or an account address.' }); }
         if (!address) { return json(400, { error: 'A journal rule must name the address the copy goes to.' }); }
         const rule = { id: nextId++, scope: parsed.scope || 'domain', target: target, journal_address: address,
            direction: parsed.direction || 'both', mode: parsed.mode || 'report',
            enabled: parsed.enabled === undefined ? true : !!parsed.enabled, note: String(parsed.note || '') };
         state.journalRules.push(rule);
         return json(201, rule);
      }
   }
   if (/^\/api\/v1\/journal-rules\/\d+$/.test(path)) {
      const id = Number(segment(path, 4));
      const rule = state.journalRules.filter((r) => r.id === id)[0];
      if (!rule) { return json(404, { error: 'journaling rule not found' }); }
      if (method === 'PUT') {
         if (parsed.enabled !== undefined) { rule.enabled = !!parsed.enabled; }
         if (parsed.note !== undefined) { rule.note = String(parsed.note); }
         return json(200, rule);
      }
      if (method === 'DELETE') { state.journalRules = state.journalRules.filter((r) => r.id !== id); return json(200, { deleted: true }); }
   }
   if (/^\/api\/v1\/spam-traps\/hits(\?.*)?$/.test(path)) {
      if (method === 'GET') { return json(200, { items: state.trapHits, hits_since_start: trapHitsSinceStart !== null ? trapHitsSinceStart : state.trapHits.length }); }
      if (method === 'DELETE') { const removed = state.trapHits.length; state.trapHits = []; return json(200, { removed: removed }); }
   }
   if (/^\/api\/v1\/spam-traps\/[^/]+$/.test(path) && method === 'DELETE') {
      const address = segment(path, 4);
      const domain = address.split('@')[1];
      const traps = state.spamTraps[domain] || [];
      if (!traps.some((t) => t.address === address)) { return json(404, { error: 'no such spam trap' }); }
      state.spamTraps[domain] = traps.filter((t) => t.address !== address);
      return json(200, { deleted: true });
   }
   if (/^\/api\/v1\/domains\/[^/]+\/sender-list$/.test(path)) {
      const domain = segment(path, 4);
      if (!state.domains.some((d) => d.name === domain)) { return json(404, { error: 'no such domain' }); }
      const list = state.domainSenders[domain] || (state.domainSenders[domain] = []);
      if (method === 'GET') { return json(200, { scope: 'domain', items: list }); }
      if (method === 'POST') {
         const value = String(parsed.value || '').trim().toLowerCase();
         if (!value) { return json(400, { error: 'value is required: an address, or a domain that covers its subdomains' }); }
         const entry = { id: nextId++, scope: 'domain', value: value, blocked: !!parsed.blocked, reason: parsed.reason || '', expires: false, expired: false };
         list.push(entry);
         return json(201, entry);
      }
   }
   if (/^\/api\/v1\/domains\/[^/]+\/sender-list\/\d+$/.test(path) && method === 'DELETE') {
      const domain = segment(path, 4);
      const id = Number(segment(path, 6));
      const list = state.domainSenders[domain] || [];
      if (!list.some((x) => x.id === id)) { return json(404, { error: 'no such entry' }); }
      state.domainSenders[domain] = list.filter((x) => x.id !== id);
      return json(200, { deleted: true });
   }
   // The domain's S/MIME authority, as RestApiSmimeAuthority.cpp answers it:
   // the document for the reading, the create and the rotation, and the one
   // certificate for a revoke.
   if (/^\/api\/v1\/domains\/[^/]+\/smime-ca(\/rotate|\/issued\/\d+)?(\?.*)?$/.test(path)) {
      const name = segment(path, 4);
      if (!state.domains.find((d) => d.name === name)) { return json(404, { error: 'domain not found' }); }
      const doc = state.smimeAuthorities[name] || (state.smimeAuthorities[name] = smimeAuthorityDoc(name));
      const bare = path.split('?')[0];
      const action = bare.slice(bare.indexOf('/smime-ca') + '/smime-ca'.length);
      if (method === 'GET' && action === '') { return json(200, doc); }
      if (method === 'POST' && (action === '' || action === '/rotate')) {
         for (const key of Object.keys(parsed || {})) { if (key !== 'key') { return json(400, { error: 'unknown field: ' + key }); } }
         if (action === '' && doc.authority) { return json(409, { error: 'the domain has an S/MIME authority already; rotate it to replace its root' }); }
         if (action === '/rotate' && !doc.authority) { return json(404, { error: 'the domain has no S/MIME authority to rotate; create one first' }); }
         if (doc.authority) { doc.retired = [Object.assign({}, doc.authority, { retired: SMIME_NOW, published: true })].concat(doc.retired); }
         doc.authority = smimeRoot(name, doc.authority ? doc.authority.id + 1 : 1, parsed && parsed.key);
         return json(action === '' ? 201 : 200, doc);
      }
      const revoke = /^\/issued\/(\d+)$/.exec(action);
      if (method === 'DELETE' && revoke) {
         const serial = Number(revoke[1]);
         const reason = decodeURIComponent((/[?&]reason=([^&]*)/.exec(path) || ['', 'keyCompromise'])[1]);
         const found = doc.issued.find((c) => c.serial === serial);
         if (!found) { return json(404, { error: 'no such certificate' }); }
         if (found.status === 'revoked') { return json(409, { error: 'the certificate is revoked already' }); }
         found.status = 'revoked';
         found.reason = reason;
         found.revoked = SMIME_NOW;
         return json(200, { revoked: true, serial, address: found.address, reason });
      }
      return json(404, { error: 'not found' });
   }
   // The key rotation, as RestApiDkimRotation.cpp answers it: the status
   // document for every route, and a 409 that carries the reason word and the
   // document beside the server's sentence.
   if (/^\/api\/v1\/domains\/[^/]+\/dkim\/rotation(\/[a-z]+)?$/.test(path)) {
      const name = segment(path, 4);
      const doc = state.rotations[name];
      if (!doc) { return json(404, { error: 'domain not found' }); }
      const action = path.slice(path.indexOf('/dkim/rotation') + '/dkim/rotation'.length);
      const refuse = (error, reason) => json(409, { error, reason, status: doc });
      const record = state.domains.find((d) => d.name === name);
      if (method === 'GET' && action === '') { return json(200, doc); }
      if (method === 'POST' && action === '/start') {
         for (const key of Object.keys(parsed || {})) { if (key !== 'selector') { return json(400, { error: 'unknown field: ' + key }); } }
         if (doc.next) { return refuse('A next key is already staged for this domain. Switch to it or cancel it before starting another rotation.', 'next_staged'); }
         const selector = (parsed && parsed.selector) || 's20260915';
         doc.next = Object.assign(rotationSlot(name, selector, 'publish', { status: 'unchecked', checked: 0, detail: '' }), { created: ROTATION_NOW, first_seen: 0, switch_due: 0 });
         doc.phase = 'awaiting_dns';
         doc.waiting = [{ step: 'switch', reason: 'unchecked', until: 0, detail: '' }];
         return json(201, doc);
      }
      if (method === 'POST' && action === '/check') {
         if (doc.next) {
            doc.next.check = { status: 'not_found', checked: ROTATION_NOW + 60, detail: doc.next.records[0].host };
            doc.waiting = [{ step: 'switch', reason: 'record_not_found', until: 0, detail: doc.next.records[0].host }];
         }
         return json(200, doc);
      }
      if (method === 'POST' && action === '/switch') {
         if (!doc.next) { return refuse('No next key is staged, so there is nothing to switch to.', 'nothing_staged'); }
         if (!(parsed && parsed.force === true)) { return refuse('The record has not yet been published for the propagation period; the switch is due at 2026-09-18 00:00 UTC.', 'propagation'); }
         if (doc.retiring) { return refuse('The selector old2025 is still being kept for late verifiers. Retire it first, or wait until 2026-09-21 00:00 UTC.', 'retiring_slot'); }
         doc.retiring = doc.current ? Object.assign(doc.current, { since: ROTATION_NOW, retire_due: ROTATION_NOW + 7 * 86400 }) : null;
         if (doc.retiring) { doc.retiring.records.forEach((r) => { r.action = 'keep'; }); }
         doc.current = Object.assign(doc.next, { since: ROTATION_NOW, next_rotation_due: 0 });
         doc.current.records.forEach((r) => { r.action = 'keep'; });
         doc.next = null;
         record.dkim_selector = doc.current.selector;
         record.dkim_private_key_file = doc.current.key_file;
         doc.phase = 'idle';
         doc.waiting = doc.retiring ? [{ step: 'retire', reason: 'retention', until: ROTATION_NOW + 7 * 86400, detail: '' }] : [];
         return json(200, doc);
      }
      if (method === 'POST' && action === '/retire') {
         if (!doc.retiring) { return refuse('No selector is retiring.', 'nothing_retiring'); }
         doc.retired = { selector: doc.retiring.selector, at: ROTATION_NOW, key_file_kept: '', records: doc.retiring.records.map((r) => ({ host: r.host, type: 'TXT', action: 'remove' })) };
         doc.retiring = null;
         doc.waiting = doc.waiting.filter((w) => w.step !== 'retire');
         return json(200, doc);
      }
      if (method === 'DELETE' && action === '/next') {
         if (!doc.next) { return refuse('No next key is staged, so there is nothing to cancel.', 'nothing_staged'); }
         doc.next = null;
         doc.phase = doc.retiring ? 'idle' : 'off';
         doc.waiting = doc.waiting.filter((w) => w.step !== 'switch');
         return json(200, doc);
      }
   }
   if (path === '/api/v1/groups' && method === 'GET') { return json(200, state.groups); }
   if (path === '/api/v1/groups' && method === 'POST') {
      for (const key of Object.keys(parsed || {})) { if (key !== 'name') { return json(400, { error: 'unknown field: ' + key }); } }
      const name = String((parsed && parsed.name) || '').trim();
      if (!name) { return json(400, { error: 'name is required' }); }
      if (state.groups.some((g) => g.name === name)) { return json(409, { error: 'A group with this name already exists.' }); }
      const record = { id: nextId++, name };
      state.groups.push(record);
      state.groupMembers[record.id] = [];
      return json(201, record);
   }
   if (/^\/api\/v1\/groups\/\d+$/.test(path)) {
      const id = Number(segment(path, 4));
      const at = state.groups.findIndex((g) => g.id === id);
      if (at < 0) { return json(404, { error: 'group not found' }); }
      if (method === 'GET') { return json(200, state.groups[at]); }
      if (method === 'PUT') {
         for (const key of Object.keys(parsed || {})) { if (key !== 'name') { return json(400, { error: 'unknown field: ' + key }); } }
         const name = String((parsed && parsed.name) || '').trim();
         if (!name) { return json(400, { error: 'name must not be empty' }); }
         if (state.groups.some((g) => g.name === name && g.id !== id)) { return json(409, { error: 'A group with this name already exists.' }); }
         state.groups[at].name = name;
         return json(200, state.groups[at]);
      }
      if (method === 'DELETE') { state.groups.splice(at, 1); delete state.groupMembers[id]; return json(200, { deleted: true }); }
   }
   if (/^\/api\/v1\/groups\/\d+\/members$/.test(path)) {
      const id = Number(segment(path, 4));
      if (!(id in state.groupMembers)) { return json(404, { error: 'group not found' }); }
      if (method === 'GET') { return json(200, state.groupMembers[id]); }
      if (method === 'POST') {
         const address = String((parsed && parsed.account) || '');
         const accountId = ACCOUNT_IDS[address];
         if (!accountId) { return json(400, { error: 'no account ' + address }); }
         if (state.groupMembers[id].some((m) => m.account_id === accountId)) { return json(409, { error: 'The account is already a member of the group.' }); }
         const member = { id: nextId++, group_id: id, account_id: accountId, account: address };
         state.groupMembers[id].push(member);
         return json(201, member);
      }
   }
   if (/^\/api\/v1\/groups\/\d+\/members\/\d+$/.test(path) && method === 'DELETE') {
      const id = Number(segment(path, 4));
      const accountId = Number(segment(path, 6));
      if (!(id in state.groupMembers) || !state.groupMembers[id].some((m) => m.account_id === accountId)) {
         return json(404, { error: 'the account is not a member of the group' });
      }
      state.groupMembers[id] = state.groupMembers[id].filter((m) => m.account_id !== accountId);
      return json(200, { deleted: true });
   }
   if (/^\/api\/v1\/domains\/[^/]+\/domain-aliases$/.test(path)) {
      const domain = segment(path, 4);
      if (!(domain in state.domainAliases)) { return json(404, { error: 'domain not found' }); }
      if (method === 'GET') { return json(200, state.domainAliases[domain]); }
      if (method === 'POST') {
         const alias = { id: nextId++, name: String(parsed.name || '').trim() };
         if (!alias.name) { return json(400, { error: 'name is required' }); }
         state.domainAliases[domain].push(alias);
         return json(201, alias);
      }
   }
   if (/^\/api\/v1\/domains\/[^/]+\/domain-aliases\/[^/]+$/.test(path) && method === 'DELETE') {
      const domain = segment(path, 4);
      const alias = segment(path, 6);
      if (!(domain in state.domainAliases) || !state.domainAliases[domain].some((a) => a.name === alias)) { return json(404, { error: 'domain alias not found' }); }
      state.domainAliases[domain] = state.domainAliases[domain].filter((a) => a.name !== alias);
      return json(200, { deleted: true });
   }
   if (/^\/api\/v1\/domains\/[^/]+\/accounts$/.test(path)) {
      const domain = segment(path, 4);
      if (!(domain in state.accounts)) { return json(404, { error: 'domain not found' }); }
      // The listing carries the two fields HandleListAccounts_ emits.
      if (method === 'GET') { return json(200, state.accounts[domain].map((a) => ({ address: a.address, active: a.active }))); }
      if (method === 'POST') {
         state.accounts[domain].push(accountRecord(parsed.address, true));
         return json(201, { address: parsed.address, active: true });
      }
   }
   // Renaming a mailbox, and moving one to another domain: the GET says whether
   // it would be refused now and what this mailbox has been through, the POST
   // does it. The state carries the refusal so a test can ask for one.
   // A room's or a piece of equipment's booking policy (ResourceBooking.h):
   // state, so that a PUT changes what the next GET says; a person's is read
   // and never written, as HandleAccountBookingPolicy_ answers it.
   if (/^\/api\/v1\/accounts\/[^/]+\/booking-policy$/.test(path)) {
      const address = segment(path, 4);
      const domain = Object.keys(state.accounts).filter((d) => state.accounts[d].some((a) => a.address === address))[0];
      if (!domain) { return json(404, { error: 'no such account' }); }
      const record = state.accounts[domain].filter((a) => a.address === address)[0];
      const policy = state.bookingPolicies[address] || (state.bookingPolicies[address] = { address: address, kind: 'person', stored: false,
         auto_accept: true, window_days: 180, max_duration_minutes: 1440, working_hours_only: false, work_days: ['mon', 'tue', 'wed', 'thu', 'fri'],
         work_start: '08:00', work_end: '17:00', time_zone: 'UTC', allow_conflicts: false, allow_recurring: true, bookers: [], capacity: 0, delegates: [] });
      policy.kind = record.kind || 'person';
      if (method === 'GET') { return json(200, Object.assign({}, policy)); }
      if (method === 'PUT') {
         if (policy.kind === 'person') { return json(409, { error: address + ' is a person, not a room or a piece of equipment' }); }
         Object.keys(parsed || {}).forEach((k) => { policy[k] = parsed[k]; });
         policy.stored = true;
         return json(200, Object.assign({}, policy));
      }
   }
   // The account's entry in the organisation's directory (DirectoryStore.h).
   // State, because the whole of the card is that a PUT changes it and the
   // next GET says so.
   if (/^\/api\/v1\/accounts\/[^/]+\/directory$/.test(path)) {
      const address = segment(path, 4);
      const domain = Object.keys(state.accounts).filter((d) => state.accounts[d].some((a) => a.address === address))[0];
      if (!domain) { return json(404, { error: 'no such account' }); }
      const entry = state.directory[address] || (state.directory[address] =
         { title: '', department: '', phone: '', mobile: '', office: '', note: '', opt_out: false, has_photo: false, photo_type: '', updated: '' });
      if (method === 'GET') { return json(200, { address: address, entry: Object.assign({}, entry) }); }
      if (method === 'PUT') {
         Object.keys(parsed || {}).forEach((k) => {
            if (k === 'photo') {
               entry.has_photo = parsed.photo !== null && parsed.photo !== '';
               entry.photo_type = entry.has_photo ? 'image/png' : '';
               return;
            }
            entry[k] = parsed[k];
         });
         entry.updated = '2026-09-18 12:00:00';
         return json(200, { address: address, entry: Object.assign({}, entry) });
      }
   }
   if (/^\/api\/v1\/accounts\/[^/]+\/move$/.test(path)) {
      const address = segment(path, 4);
      const domain = Object.keys(state.accounts).filter((d) => state.accounts[d].some((a) => a.address === address))[0];
      if (!domain) { return json(404, { error: 'no such account' }); }
      if (method === 'GET') {
         return json(200, { address: address, in_use: !!moveBlockedReason, in_use_reason: moveBlockedReason || '',
            last_move: state.moves[address] || null });
      }
      if (method === 'POST') {
         const to = String((parsed && parsed.new_address) || '').trim();
         if (!to) { return json(400, { error: 'new_address is required' }); }
         if (moveBlockedReason) { return json(409, { error: moveBlockedReason }); }
         const newDomain = to.split('@')[1] || '';
         if (!state.domains.some((d) => d.name === newDomain)) { return json(400, { error: 'This server does not hold the domain ' + newDomain + '.' }); }
         if (Object.keys(state.accounts).some((d) => state.accounts[d].some((a) => a.address === to))) { return json(409, { error: 'An account already exists at ' + to + '.' }); }
         const keep = parsed.keep_alias !== false;
         const record = state.accounts[domain].filter((a) => a.address === address)[0];
         state.accounts[domain] = state.accounts[domain].filter((a) => a.address !== address);
         record.address = to;
         state.accounts[newDomain] = state.accounts[newDomain] || [];
         state.accounts[newDomain].push(record);
         // Kept in address order, which is the order a listing has had all
         // along: a move back must not leave the mailbox at the end of the
         // list it came from.
         state.accounts[newDomain].sort((a, b) => (a.address < b.address ? -1 : 1));
         if (keep) { (state.aliases[domain] = state.aliases[domain] || []).push({ name: address, value: to, active: true }); }
         const done = { old_address: address, new_address: to, old_domain: domain, new_domain: newDomain, account_id: 7,
            alias_kept: keep, store_moved: true, messages: 3, folders: 2, aliases_repointed: 0, list_memberships: 0,
            queued_recipients: 0, quarantine_entries: 0, journal_id: 11 };
         state.moves[to] = { id: 11, account_id: 7, old_address: address, new_address: to, stage: 4, stage_name: 'complete',
            complete: true, keep_alias: keep, messages: 3, started: '2026-09-16 02:00:00', finished: '2026-09-16 02:00:04', detail: '' };
         return json(200, done);
      }
   }
   if (/^\/api\/v1\/accounts\/[^/]+$/.test(path)) {
      const address = segment(path, 4);
      const domain = Object.keys(state.accounts).filter((d) => state.accounts[d].some((a) => a.address === address))[0];
      if (method === 'DELETE') {
         Object.keys(state.accounts).forEach((d) => { state.accounts[d] = state.accounts[d].filter((a) => a.address !== address); });
         return json(200, { deleted: true });
      }
      if (!domain) { return json(404, { error: 'account not found' }); }
      const record = state.accounts[domain].filter((a) => a.address === address)[0];
      if (method === 'GET') { return json(200, Object.assign({}, record, levelAnswer(domain, address))); }
      if (method === 'PUT') {
         // As HandleUpdateAccount_ reads it: nothing to update refused, an
         // unknown field refused by name, the password kept aside.
         if (!Object.keys(parsed).length) { return json(400, { error: 'nothing to update' }); }
         for (const key of Object.keys(parsed)) {
            if (!(key in ACCOUNT_PROPS) && LEVEL_KEYS_ACCOUNT.indexOf(key) < 0) { return json(400, { error: 'unknown field: ' + key }); }
         }
         state.levels.accounts[address] = state.levels.accounts[address] || {};
         storeLevels(state.levels.accounts[address], parsed, LEVEL_KEYS_ACCOUNT);
         Object.keys(parsed).forEach((key) => {
            if (key === 'password') { secrets.account_password = parsed[key]; return; }
            if (LEVEL_KEYS_ACCOUNT.indexOf(key) >= 0) { return; }
            record[key] = parsed[key];
         });
         return json(200, record);
      }
   }

   if (/^\/api\/v1\/domains\/[^/]+\/aliases$/.test(path)) {
      const domain = segment(path, 4);
      if (!(domain in state.aliases)) { return json(404, { error: 'domain not found' }); }
      if (method === 'GET') { return json(200, state.aliases[domain]); }
      if (method === 'POST') {
         for (const key of Object.keys(parsed)) { if (['name', 'value', 'active'].indexOf(key) < 0) { return json(400, { error: 'unknown field: ' + key }); } }
         if (!parsed.name || !parsed.value) { return json(400, { error: 'name and value are required' }); }
         if (state.aliases[domain].some((a) => a.name === parsed.name)) { return json(409, { error: 'an alias with that name exists' }); }
         const alias = { name: parsed.name, value: parsed.value, active: parsed.active !== false };
         state.aliases[domain].push(alias);
         return json(201, alias);
      }
   }
   if (/^\/api\/v1\/aliases\/[^/]+$/.test(path) && method === 'DELETE') {
      const name = segment(path, 4);
      const domain = Object.keys(state.aliases).filter((d) => state.aliases[d].some((a) => a.name === name))[0];
      if (!domain) { return json(404, { error: 'alias not found' }); }
      state.aliases[domain] = state.aliases[domain].filter((a) => a.name !== name);
      return json(200, { deleted: true });
   }
   if (/^\/api\/v1\/domains\/[^/]+\/lists$/.test(path)) {
      const domain = segment(path, 4);
      if (!(domain in state.lists)) { return json(404, { error: 'domain not found' }); }
      if (method === 'GET') { return json(200, state.lists[domain]); }
      if (method === 'POST') {
         for (const key of Object.keys(parsed)) { if (!(key in LIST_CREATE_PROPS)) { return json(400, { error: 'unknown field: ' + key }); } }
         if (!String(parsed.address || '').trim()) { return json(400, { error: 'address is required' }); }
         if ('mode' in parsed && LIST_PROPS.mode.enum.indexOf(parsed.mode) < 0) { return json(400, { error: 'mode must be public, membership, announcement or domain_members' }); }
         if (state.lists[domain].some((l) => l.address === parsed.address)) { return json(409, { error: 'a list with that address exists' }); }
         const list = Object.assign({ active: true, require_auth: false, mode: 'public', require_sender_address: '', moderator_address: '', bounce_address: '', membership: '', members: [] }, parsed);
         state.lists[domain].push(list);
         return json(201, list);
      }
   }
   // Who a list delivers to now: its rows, then the active accounts its
   // membership query answers, deduplicated by address - the server's own
   // Members(), in the two forms this walk uses.
   if (/^\/api\/v1\/lists\/[^/]+\/members$/.test(path) && method === 'GET') {
      const address = segment(path, 4);
      const domain = Object.keys(state.lists).filter((d) => state.lists[d].some((l) => l.address === address))[0];
      if (!domain) { return json(404, { error: 'list not found' }); }
      const list = state.lists[domain].filter((l) => l.address === address)[0];
      const membership = list.membership || '';
      // A query that reaches past the domains the caller administers is refused
      // rather than answered, because a form a credential may not SET is a form
      // it may not read the answer to - the server does this in
      // RestApiDistributionLists.cpp, and the page has to open the editor
      // anyway. everyone is the form that always reaches past a confined one.
      if (membership === 'everyone') {
         return json(403, { error: 'this list\'s membership names accounts outside the domains this credential administers; only a credential that reaches them may read who it delivers to' });
      }
      const seen = {}, members = [];
      const take = (a, source) => { if (!seen[a.toLowerCase()]) { seen[a.toLowerCase()] = true; members.push({ address: a, source }); } };
      (list.members || []).forEach((m) => take(m, 'row'));
      let problem = '';
      if (membership === 'domain' || membership.indexOf('domain:') === 0) {
         const named = membership === 'domain' ? domain : membership.slice('domain:'.length);
         if (!(named in state.accounts)) { problem = 'The membership names the domain ' + named + ', which this server does not host.'; }
         else { state.accounts[named].filter((a) => a.active).map((a) => a.address).sort().forEach((a) => take(a, 'query')); }
      }
      const kind = membership === '' ? 'static' : membership === 'domain' ? 'domain'
         : membership.indexOf('domain:') === 0 ? 'named_domain'
         : membership.indexOf('pattern:') === 0 ? 'pattern' : 'everyone';
      // A managed list's rows carry what the list manager knows of each
      // member (RestApiServer::ListMemberJson_): delivery, state and bounces.
      members.forEach((e) => { if (e.source === 'row' && list.rowDetail && list.rowDetail[e.address]) { Object.assign(e, list.rowDetail[e.address]); } });
      const answer = { address, membership, membership_kind: kind, count: members.length, members };
      if (problem) { answer.problem = problem; }
      return json(200, answer);
   }
   // The owner's two verbs on one member of a managed list.
   if (/^\/api\/v1\/lists\/[^/]+\/members\/[^/]+\/(reset-bounces|approve)$/.test(path) && method === 'POST') {
      const address = segment(path, 4);
      const member = segment(path, 6);
      const domain = Object.keys(state.lists).filter((d) => state.lists[d].some((l) => l.address === address))[0];
      const list = domain ? state.lists[domain].filter((l) => l.address === address)[0] : null;
      const detail = list && list.rowDetail ? list.rowDetail[member] : null;
      if (!detail) { return json(404, { error: 'not a member or a request of this list' }); }
      if (path.endsWith('/approve')) { Object.assign(detail, { confirmed: 1, state: 'member' }); }
      else { Object.assign(detail, { hard_bounces: 0, soft_bounces: 0, reason: '', delivery: detail.delivery === 'none' ? 'each' : detail.delivery }); }
      return json(200, Object.assign({ address: member, source: 'row' }, detail));
   }
   if (/^\/api\/v1\/lists\/[^/]+$/.test(path)) {
      const address = segment(path, 4);
      const domain = Object.keys(state.lists).filter((d) => state.lists[d].some((l) => l.address === address))[0];
      if (!domain) { return json(404, { error: 'list not found' }); }
      const at = state.lists[domain].findIndex((l) => l.address === address);
      if (method === 'PUT') {
         for (const key of Object.keys(parsed)) { if (!(key in LIST_PROPS)) { return json(400, { error: 'unknown field: ' + key }); } }
         Object.assign(state.lists[domain][at], parsed);
         return json(200, state.lists[domain][at]);
      }
      if (method === 'DELETE') { state.lists[domain].splice(at, 1); return json(200, { deleted: true }); }
   }

   if (/^\/api\/v1\/migrations(\/|$)/.test(path)) {
      const parts = path.split('?')[0].split('/');
      const counted = (m) => {
         const jobs = state.migrationJobs[m.id] || [];
         const count = (word) => jobs.filter((j) => j.state === word).length;
         return Object.assign({}, m, {
            jobs: { total: jobs.length, queued: count('queued'), running: count('running'), paused: count('paused'), done: count('done'),
               failed: count('failed'), cancelled: count('cancelled'), awaiting_signin: jobs.filter((j) => j.awaiting_signin).length },
            messages_total: jobs.reduce((n, j) => n + j.messages_total, 0), messages_copied: jobs.reduce((n, j) => n + j.messages_copied, 0),
            bytes: jobs.reduce((n, j) => n + j.bytes, 0), skipped: jobs.reduce((n, j) => n + j.skipped, 0)
         });
      };
      if (parts.length === 4 && method === 'GET') { return json(200, { migrations: state.migrations.map(counted) }); }
      if (parts.length === 4 && method === 'POST') {
         for (const key of Object.keys(parsed || {})) { if (!(key in MIGRATION_PROPS)) { return json(400, { error: 'unknown field: ' + key }); } }
         if (!parsed || !String(parsed.name || '').trim()) { return json(400, { error: 'name is required' }); }
         const kind = parsed.kind || 'imap';
         if (kind === 'imap' && !String(parsed.host || '').trim()) { return json(400, { error: 'host is required for a migration from an IMAP server' }); }
         const made = migrationRecord(nextId++, parsed.name, Object.assign({}, parsed, { kind, host: parsed.host || (kind === 'google' ? 'imap.gmail.com' : kind === 'pst' ? '' : 'outlook.office365.com') }));
         state.migrations.push(made);
         state.migrationJobs[made.id] = [];
         return json(201, counted(made));
      }
      const id = Number(parts[4]);
      const m = state.migrations.filter((x) => x.id === id)[0];
      if (!m) { return json(404, { error: 'no such migration' }); }
      const jobs = state.migrationJobs[id] = state.migrationJobs[id] || [];
      if (parts.length === 5 && method === 'GET') { return json(200, counted(m)); }
      if (parts.length === 5 && method === 'DELETE') {
         if (jobs.some((j) => j.state === 'running')) { return json(409, { error: 'a job of this migration is running: pause it first' }); }
         state.migrations = state.migrations.filter((x) => x.id !== id);
         delete state.migrationJobs[id];
         return json(200, { deleted: true });
      }
      if (parts.length === 6 && method === 'POST' && parts[5] === 'start') {
         if (m.state === 'running') { return json(409, { error: 'the migration is running already' }); }
         if (m.state === 'paused') { return json(409, { error: 'the migration is paused: resume it' }); }
         m.state = 'running';
         return json(200, counted(m));
      }
      if (parts.length === 6 && method === 'POST' && parts[5] === 'pause') {
         if (m.state !== 'running') { return json(409, { error: 'the migration is not running' }); }
         m.state = 'paused';
         jobs.forEach((j) => { if (j.state === 'running' || j.state === 'queued') { j.state = 'paused'; } });
         return json(200, counted(m));
      }
      if (parts.length === 6 && method === 'POST' && parts[5] === 'resume') {
         if (m.state !== 'paused') { return json(409, { error: 'the migration is not paused' }); }
         m.state = 'running';
         jobs.forEach((j) => { if (j.state === 'paused') { j.state = 'queued'; } });
         return json(200, counted(m));
      }
      if (parts.length === 6 && method === 'GET' && parts[5] === 'summary.csv') {
         return { status: 200, body: 'job,source_user,target,state\r\n' + jobs.map((j) => [j.id, j.source_user, j.target, j.state].join(',')).join('\r\n'), headers: { 'Content-Type': 'text/csv; charset=utf-8' } };
      }
      if (parts.length === 6 && parts[5] === 'jobs' && method === 'GET') { return json(200, { jobs }); }
      if (parts.length === 6 && parts[5] === 'jobs' && method === 'POST') {
         // The server's reading of the body: a JSON object is one job, anything
         // else is CSV, every line checked before one job is made.
         const wanted = [];
         if (parsed && typeof parsed === 'object') { wanted.push({ line: 1, fields: [parsed.source_user || '', parsed.secret || '', parsed.target || ''] }); }
         else {
            const lines = String(raw || '').split('\n');
            for (let n = 0; n < lines.length; n += 1) {
               const line = lines[n].replace(/\r$/, '');
               if (!line.trim()) { continue; }
               const fields = [];
               let field = '', quoted = false;
               for (let i = 0; i < line.length; i += 1) {
                  const c = line[i];
                  if (quoted) { if (c === '"' && line[i + 1] === '"') { field += '"'; i += 1; } else if (c === '"') { quoted = false; } else { field += c; } }
                  else if (c === '"' && field === '') { quoted = true; }
                  else if (c === ',') { fields.push(field); field = ''; }
                  else { field += c; }
               }
               fields.push(field);
               if (!wanted.length && /^source_?user$/i.test(fields[0])) { continue; }
               if (fields.length !== 3) { return json(400, { error: 'line ' + (n + 1) + ': a row is sourceuser,secret,target - three fields - and this one has ' + fields.length, line: n + 1 }); }
               wanted.push({ line: n + 1, fields });
            }
         }
         if (!wanted.length) { return json(400, { error: 'the body names no job: a JSON object, or CSV rows of sourceuser,secret,target' }); }
         for (const one of wanted) {
            const target = one.fields[2].trim();
            if (!Object.keys(state.accounts).some((d) => state.accounts[d].some((a) => a.address === target))) {
               return json(400, { error: 'line ' + one.line + ': the target ' + target + ' is not an account of this server', line: one.line });
            }
         }
         const made = wanted.map((one) => {
            const signsIn = m.kind !== 'imap';
            const job = migrationJob(nextId++, id, one.fields[0].trim(), one.fields[2].trim(), { signs_in: signsIn, awaiting_signin: signsIn && one.fields[1] === '' });
            jobs.push(job);
            return job.awaiting_signin ? Object.assign({}, job, { signin_url: 'https://mail.example.com/portal/oauth/migration?job=' + job.id + '&key=' + 'ab'.repeat(32) }) : job;
         });
         return json(201, { created: made.length, jobs: made });
      }
      if (parts.length >= 7 && parts[5] === 'jobs') {
         const job = jobs.filter((j) => j.id === Number(parts[6]))[0];
         if (!job) { return json(404, { error: 'no such job in this migration' }); }
         if (parts.length === 7 && method === 'GET') { return json(200, job); }
         if (parts[7] === 'report' && method === 'GET') {
            return json(200, state.migrationReports[job.id] || { job: job.id, source_user: job.source_user, target: job.target, state: job.state, phase: job.phase, folders: [],
               contacts: { state: '', copied: 0, skipped: 0, skipped_reasons: {}, reason: '' }, calendars: { state: '', copied: 0, skipped: 0, skipped_reasons: {}, reason: '' }, errors: [] });
         }
         if (parts[7] === 'retry' && method === 'POST') {
            if (['running', 'queued', 'done'].indexOf(job.state) >= 0) { return json(409, { error: 'the job is ' + job.state }); }
            Object.assign(job, { state: 'queued', last_error: '', attempts: 0, next_attempt: 0 });
            if (m.state === 'finished') { m.state = 'running'; }
            return json(200, job);
         }
         if (parts[7] === 'cancel' && method === 'POST') {
            if (job.state === 'done' || job.state === 'cancelled') { return json(409, { error: 'the job is ' + job.state + ' already' }); }
            job.state = 'cancelled';
            return json(200, job);
         }
         if (parts[7] === 'signin' && method === 'POST') {
            if (m.kind === 'imap') { return json(409, { error: 'a migration from an IMAP server uses passwords, not sign-ins' }); }
            job.awaiting_signin = true;
            return json(200, { signin_url: 'https://mail.example.com/portal/oauth/migration?job=' + job.id + '&key=' + 'cd'.repeat(32) });
         }
      }
   }

   if (/^\/api\/v1\/accounts\/[^/]+\/fetch-accounts(\/\d+(\/download)?)?$/.test(path)) {
      const address = segment(path, 4);
      if (!Object.keys(state.accounts).some((d) => state.accounts[d].some((a) => a.address === address))) { return json(404, { error: 'account not found' }); }
      const list = state.fetchAccounts[address] = state.fetchAccounts[address] || [];
      const parts = path.split('/');
      if (parts.length === 6) {
         if (method === 'GET') { return json(200, list); }
         if (method === 'POST') {
            if (staleKeep(parsed)) { return json(409, STALE_KEEP); }
            const problem = fetchProblem(parsed, true);
            if (problem) { return json(400, { error: problem }); }
            const item = Object.assign(fetchRecord(nextId++, ''), parsed);
            delete item.days_to_keep_meaning;
            if ('password' in parsed) { secrets.fetch_password = parsed.password; delete item.password; }
            list.push(item);
            return json(201, item);
         }
      }
      const id = Number(parts[6]);
      const at = list.findIndex((x) => x.id === id);
      if (at < 0) { return json(404, { error: 'fetch account not found' }); }
      if (parts.length === 8 && method === 'POST') {
         if (state.fetcherDown) { return json(503, { error: 'the external fetcher is not running' }); }
         list[at].locked = true;
         return json(202, { queued: true });
      }
      if (parts.length === 7 && method === 'GET') { return json(200, list[at]); }
      if (parts.length === 7 && method === 'PUT') {
         if (staleKeep(parsed)) { return json(409, STALE_KEEP); }
         const problem = fetchProblem(parsed, false);
         if (problem) { return json(400, { error: problem }); }
         Object.assign(list[at], parsed);
         delete list[at].days_to_keep_meaning;
         if ('password' in parsed) { secrets.fetch_password = parsed.password; delete list[at].password; }
         return json(200, list[at]);
      }
      if (parts.length === 7 && method === 'DELETE') { list.splice(at, 1); return json(200, { deleted: true }); }
   }

   if (path === '/api/v1/backup') {
      if (method === 'GET') { return json(200, state.backup); }
      if (method === 'POST') {
         if (state.backupRunning) { return json(409, { error: 'the backup did not start', status: 'A backup is already running.' }); }
         state.backupRunning = true;
         state.backup.log.push('2026-09-14 11:00:00 Backup started.');
         return json(202, { started: true });
      }
   }

   // The restore, refusing as HandleBackupRestore_ refuses. The archive this
   // names is an encrypted one, so without the passphrase it answers the
   // server's own sentence and changes nothing.
   if (path === '/api/v1/backup/restore' && method === 'POST') {
      if (!parsed.file) { return json(400, { error: 'file is required: the path of the backup archive on this server' }); }
      if (!parsed.passphrase) {
         return json(400, { error: 'The backup archive ' + parsed.file + ' is encrypted, and no passphrase was given for it.' });
      }
      if (parsed.passphrase !== 'correct horse battery staple') {
         return json(400, { error: 'The backup archive ' + parsed.file + ' could not be opened with the passphrase given: either it is not the passphrase this archive was written with, or the archive is damaged.' });
      }
      state.backup.log.push('2026-09-14 11:05:00 Restore started');
      return json(202, { started: true, encrypted: true, contains: { settings: false, domains: true, messages: true } });
   }

   // One mailbox from an archive, as MailboxRestore and RestApiBackupMailbox
   // answer: the archives in the destination, an account's folders and
   // messages out of one, the restore queued as a job, and the job's outcome.
   if (path === '/api/v1/backup/archives' && method === 'GET') { return json(200, state.backupArchives); }
   if (path.indexOf('/api/v1/backup/restore-mailbox/') === 0 && method === 'GET') {
      const job = Number(path.substring('/api/v1/backup/restore-mailbox/'.length));
      const outcome = state.mailboxRestoreJobs[job];
      if (!outcome) { return json(404, { error: 'no such mailbox restore job; the last fifty are kept for the life of the process' }); }
      return json(200, outcome);
   }
   const mailboxMatch = /^\/api\/v1\/backup\/([^/]+)\/mailboxes\/([^/]+)(\/restore)?$/.exec(path);
   if (mailboxMatch) {
      const archive = decodeURIComponent(mailboxMatch[1]);
      const address = decodeURIComponent(mailboxMatch[2]);
      if (!state.backupArchives.archives.some((a) => a.name === archive)) { return json(404, { error: 'There is no archive ' + archive + ' in the backup destination.' }); }
      if (archive.indexOf('2026-09-21') >= 0 && (headers['X-hMailServer-Backup-Passphrase'] || headers['x-hmailserver-backup-passphrase'] || parsed.passphrase) !== 'correct horse battery staple') {
         return json(400, { error: 'The backup archive ' + archive + ' is encrypted, and no passphrase was given for it.' });
      }
      if (address !== 'ann@example.test') { return json(404, { error: 'The archive ' + archive + ' has no account ' + address + '. Nothing has been changed on this server.' }); }
      if (!mailboxMatch[3] && method === 'GET') { return json(200, state.archivedMailbox); }
      if (mailboxMatch[3] && method === 'POST') {
         if (parsed.mode !== undefined && parsed.mode !== 'merge' && parsed.mode !== 'replace') { return json(400, { error: 'mode must be merge or replace' }); }
         if (state.backupRunning) { return json(409, { error: 'A backup or restore is already running, so this mailbox restore was not started. Ask again when it has finished.', status: '' }); }
         const job = nextId++;
         const mode = parsed.message ? 'message' : (parsed.mode || 'merge');
         state.mailboxRestoreJobs[job] = { job: job, state: 'done', archive: '/var/backups/hmailserver/' + archive, address: address, mode: mode,
            message: parsed.message || 0, folders: mode === 'message' ? 1 : 3, added: mode === 'message' ? 1 : 12, skipped: mode === 'replace' ? 0 : 4, failed: 0, error: '',
            started: '2026-09-22 12:00:00', finished: '2026-09-22 12:00:03' };
         state.backup.log.push('2026-09-22 12:00:00 Mailbox restore ' + job + ' queued');
         return json(202, { job: job, state: 'queued', archive: '/var/backups/hmailserver/' + archive, address: address, mode: mode, message: parsed.message || 0, status: '/api/v1/backup/restore-mailbox/' + job });
      }
   }

   if (path === '/api/v1/ipranges' && method === 'GET') { return json(200, state.ranges); }
   if (path === '/api/v1/ipranges' && method === 'POST') {
      // As HandleCreateIpRange_ reads it: three required, the addresses
      // parsed, a flag left out taking its default, and only the id back.
      if (!parsed.name || !parsed.lower || !parsed.upper) { return json(400, { error: 'name, lower and upper are required' }); }
      if (!ipAddress(parsed.lower) || !ipAddress(parsed.upper)) { return json(400, { error: 'lower and upper must be IP addresses' }); }
      const expiry = expiryProblem(parsed, null);
      if (expiry) { return json(400, { error: expiry }); }
      const range = Object.assign({ id: nextId++, priority: 0, expires: false, expires_time: '' }, RANGE_CREATE_DEFAULTS, parsed);
      if (!range.expires) { range.expires_time = ''; }
      state.ranges.push(range);

      return json(201, { id: range.id });
   }
   if (path === '/api/v1/reputation' && method === 'GET') { return json(200, state.reputation); }
   if (path === '/api/v1/setup/checklist' && method === 'GET') { state.checklistReads += 1; return json(200, state.checklist); }
   if (path === '/api/v1/settings/profiles' && method === 'GET') {
      const names = ['office', 'gateway'];
      return json(200, { profiles: names.map((n) => { const d = state.profileDiffs[n];
         return { name: n, title: d.title, summary: d.summary, includes: null, entries: d.entries.length, changes: d.changes,
            confirmations: d.confirmation_needed.length, skipped: d.skipped, refused: 0 }; }),
         applied: state.profileSnapshot && state.profileSnapshot.profile ? { profile: state.profileSnapshot.profile, title: '', applied: 1789000000, state: 'applied', changed: 1789000000 }
            : state.profileDeclined ? { profile: '', title: '', applied: 1789000000, state: 'declined', changed: 1789000000 } : null,
         snapshot: state.profileSnapshot });
   }
   if (path === '/api/v1/settings/profiles/decline' && method === 'POST') {
      if (state.profileSnapshot) { return json(409, { error: 'A way back from a profile is held.' }); }
      state.profileDeclines += 1;
      state.profileDeclined = true;
      return json(200, { declined: 1789000100, message: 'Recorded.' });
   }
   if (path === '/api/v1/settings/profiles/revert' && method === 'POST') {
      if (!state.profileSnapshot) { return json(409, { error: 'There is nothing to revert.' }); }
      state.profileReverts += 1;
      const profile = state.profileSnapshot.profile;
      state.profileSnapshot = null;
      return json(200, { profile, reverted: 1789000100, entries: [], set: 2, unchanged: 0, skipped: 1, failed: 0, waiting: { reinitialize: 0, restart: 2 }, message: 'reverted' });
   }
   if (path === '/api/v1/settings/profiles/discard' && method === 'POST') {
      if (!state.profileSnapshot) { return json(409, { error: 'There is no way back to discard.' }); }
      state.profileDiscards += 1;
      state.profileSnapshot = null;
      return json(200, { profile: 'office', discarded: 1789000100, entries: 2, message: 'discarded' });
   }
   if (/^\/api\/v1\/settings\/profiles\/[a-z-]+\/apply$/.test(path) && method === 'POST') {
      const name = segment(path, 5);
      const diff = state.profileDiffs[name];
      if (!diff) { return json(404, { error: 'There is no profile named ' + name + '.' }); }
      state.profileApplies.push({ name, body: parsed });
      if (parsed && parsed.confirm === true && !parsed.diff_token) { return json(400, { error: 'a confirmation confirms a diff: send the diff_token' }); }
      if (state.profileSnapshot) { return json(409, { error: 'A way back is held.', confirmation: false, stale: false, confirmation_needed: [], refused: [], snapshot: state.profileSnapshot }); }
      if (parsed && parsed.diff_token && parsed.diff_token !== diff.diff_token) {
         return json(409, { error: 'The settings changed since the diff of the ' + name + ' profile was read. Look at the diff again.', confirmation: false, stale: true, confirmation_needed: [], refused: [] });
      }
      if (diff.confirmation_needed.length && !(parsed && parsed.confirm === true)) {
         return json(409, { error: 'Applying the ' + name + ' profile here changes one entry that asks for a confirmation.', confirmation: true, stale: false,
            confirmation_needed: diff.confirmation_needed, refused: [] });
      }
      state.profileSnapshot = { readable: true, profile: name, applied: 1789000050, entries: diff.changes };
      return json(200, { profile: name, applied: 1789000050, entries: [], set: diff.changes, unchanged: diff.unchanged, skipped: diff.skipped, failed: 0,
         waiting: { reinitialize: 1, restart: 0 }, message: 'applied' });
   }
   if (/^\/api\/v1\/settings\/profiles\/[a-z-]+$/.test(path) && method === 'GET') {
      const diff = state.profileDiffs[segment(path, 5)];
      if (!diff) { return json(404, { error: 'There is no profile of that name.' }); }
      const answer = clone(diff);
      answer.snapshot = state.profileSnapshot;
      answer.applicable = !state.profileSnapshot;
      answer.blocked = state.profileSnapshot ? 'A way back from an earlier profile is held: revert it or discard it before applying another.' : null;
      return json(200, answer);
   }
   if (path === '/api/v1/setup/sample-domain' && method === 'GET') { return json(200, state.sampleDomain); }
   if (path === '/api/v1/setup/sample-domain' && method === 'POST') {
      if (state.sampleDomain.exists) { return json(409, { error: 'example.test already exists' }); }
      state.sampleDomain = Object.assign({}, state.sampleDomain, { state: 'present', domain_id: 42, exists: true });
      return json(201, { created: true, domain: 'example.test', domain_id: 42, password: '7TQ4M-KJ9XD-2VWHF-BNRE5',
         accounts_created: 3, list: 'everyone@example.test', list_members: 3, rules: 1, messages: 8, deliveries: 14,
         message: 'example.test was created with 3 mailboxes, the list everyone@example.test and 8 messages on their way into it. '
            + 'All of the mailboxes sign in with the password 7TQ4M-KJ9XD-2VWHF-BNRE5, which is shown here once and is not stored anywhere.' });
   }
   if (path === '/api/v1/setup/sample-domain' && method === 'DELETE') {
      if (!state.sampleDomain.exists) { return json(409, { error: 'There is no sample domain on this server, so nothing was removed.' }); }
      state.sampleDomain = Object.assign({}, state.sampleDomain, { state: 'absent', domain_id: 0, exists: false });
      return json(200, { removed: true, domain: 'example.test', domain_id: 42, accounts: 3, lists: 1, rules: 1, messages: 14, queued: 0,
         message: 'example.test was removed: 3 mailboxes, 1 distribution list(s), 1 rule(s) and 14 message(s) went with it.' });
   }
   if (path === '/api/v1/reputation/check' && method === 'POST') {
      state.reputationChecks += 1;
      // A check is what turns an unanswered list into an answer: after it, the list
      // that refused the query answers, and says the address is not on it.
      state.reputation.addresses[0].results[1] = { list: 'ipv4.example.test', status: 'not_listed', listed: false,
         code: '', reason: '', checked: 1789000200, listed_since: 0 };
      state.reputation.last_checked = 1789000200;
      return json(200, Object.assign({ run: { as_of: 1789000200, addresses: 2, lists: 2, lookups: 8, listed: 1, unknown: 0, stored: true } },
         state.reputation));
   }
   if (/^\/api\/v1\/ipranges\/\d+$/.test(path)) {
      const id = Number(segment(path, 4));
      const at = state.ranges.findIndex((r) => r.id === id);
      if (method === 'PUT') {
         for (const key of Object.keys(parsed)) {
            if (!(key in RANGE_PROPS)) { return json(400, { error: 'unknown field: ' + key }); }
         }
         if (at < 0) { return json(404, { error: 'ip range not found' }); }
         if ('lower' in parsed && !ipAddress(parsed.lower) || 'upper' in parsed && !ipAddress(parsed.upper)) { return json(400, { error: 'lower and upper must be IP addresses' }); }
         const expiry = expiryProblem(parsed, state.ranges[at]);
         if (expiry) { return json(400, { error: expiry }); }
         const time = String(parsed.expires_time || '').trim();
         Object.assign(state.ranges[at], parsed);
         if (!state.ranges[at].expires) { state.ranges[at].expires_time = ''; }
         else if (!time) { state.ranges[at].expires_time = state.ranges[at].expires_time || ''; }
         else { state.ranges[at].expires_time = time.length === 10 ? time + ' 00:00:00' : time; }
         return json(200, state.ranges[at]);
      }
      if (at < 0) { return json(404, { error: 'ip range not found' }); }
      if (method === 'DELETE') { state.ranges.splice(at, 1); return json(200, { deleted: true }); }
   }

   // What the never-ban list would decide about an address, as
   // NeverBanNameList::Check answers it: one address the recorded server
   // spares, any other not, and text that is not an address refused.
   if (path === '/api/v1/never-ban-names/check' && method === 'POST') {
      const address = String(parsed.address || '').trim();
      if (!/^[0-9a-fA-F.:]+$/.test(address)) { return json(400, { error: 'address must be an IPv4 or IPv6 address, such as 192.0.2.10 or 2001:db8::25' }); }
      if (address === '209.85.220.41') {
         return json(200, { address: address, reverse_names: [{ name: 'mail-sor-f41.google.com', usable: true, pattern: '.google.com', resolves_back: true }], answered: true, spared: true,
            name: 'mail-sor-f41.google.com', pattern: '.google.com', reason: 'its reverse name mail-sor-f41.google.com resolves back to it and is covered by .google.com on the never-ban list' });
      }
      return json(200, { address: address, reverse_names: [{ name: 'mail.google.com', usable: true, pattern: '.google.com', resolves_back: false }], answered: true, spared: false,
         name: 'mail.google.com', pattern: '.google.com', reason: 'its reverse name mail.google.com is covered by .google.com on the never-ban list but does not resolve back to it, so anybody could have written it' });
   }

   // The seven small collections, each in the same shape: the body read
   // whole, an unknown field or a missing required one refused by name.
   for (const base of Object.keys(COLLECTION_SPECS)) {
      const c = COLLECTION_SPECS[base];
      const items = state.collections[base];
      const refused = () => {
         for (const key of Object.keys(parsed)) { if (!(key in c.props)) { return 'unknown field: ' + key; } }
         for (const key of Object.keys(parsed)) { if (c.props[key].type === 'integer' && typeof parsed[key] !== 'number') { return key + ' must be a whole number between -2000000000 and 2000000000'; } }
         return null;
      };
      if (path === base && method === 'GET') { return json(200, items); }
      if (path === base && method === 'POST') {
         const problem = refused() || c.required.filter((k) => !String(parsed[k] || '').trim()).map((k) => k + ' is required')[0];
         if (problem) { return json(400, { error: problem }); }
         const item = Object.assign({ id: nextId++ }, c.props.active ? { active: true } : {}, parsed);
         items.push(item);
         return json(201, item);
      }
      if (path.indexOf(base + '/') === 0) {
         const id = Number(path.slice(base.length + 1));
         const at = items.findIndex((x) => x.id === id);
         if (method === 'PUT') {
            const problem = refused();
            if (problem) { return json(400, { error: problem }); }
            if (at < 0) { return json(404, { error: c.noun + ' not found' }); }
            Object.assign(items[at], parsed);
            return json(200, items[at]);
         }
         if (method === 'DELETE') {
            if (at < 0) { return json(404, { error: c.noun + ' not found' }); }
            items.splice(at, 1);
            return json(200, { deleted: true });
         }
      }
   }

   if (path.split('?')[0] === '/api/v1/sandbox/held' && method === 'GET') { return json(200, state.sandbox); }
   if (/^\/api\/v1\/sandbox\/held\/\d+(\/release)?$/.test(path) && (method === 'POST' || method === 'DELETE')) {
      const id = Number(segment(path, 5));
      const release = /\/release$/.test(path);
      if (release !== (method === 'POST')) { return json(404, { error: 'not found' }); }
      const at = state.sandbox.messages.findIndex((m) => m.queue_id === id);
      if (at < 0) { return json(404, { error: 'no message with that id is held for the sandbox' }); }
      state.sandbox.messages.splice(at, 1);
      state.sandbox.total -= 1;
      state.queue = state.queue.filter((m) => m.id !== id);
      return json(200, release ? { released: true } : { discarded: true });
   }
   if (path === '/api/v1/queue' && method === 'GET') { return json(200, { messages: state.queue }); }
   if (path === '/api/v1/queue/shaping' && method === 'GET') { return json(200, state.shaping); }
   if (/^\/api\/v1\/queue\/shaping\/[^/]+$/.test(path) && method === 'DELETE') {
      const destination = segment(path, 5);
      const at = state.shaping.backoffs.findIndex((b) => b.destination === destination);
      if (at < 0) { return json(404, { error: 'no back-off is open for that destination' }); }
      state.shaping.backoffs.splice(at, 1);
      return json(200, { cleared: true });
   }
   // The IP warm-up's schedules: the listing, a new one, one changed or removed.
   if (path === '/api/v1/warmup' && method === 'GET') {
      if (warmupFails) { warmupFails = false; return json(500, { error: 'the warm-up schedules could not be read; see the error log' }); }
      return json(200, state.warmup);
   }
   if (path === '/api/v1/warmup' && method === 'POST') {
      const address = String(parsed.address || '').trim();
      if (!address) { return json(400, { error: 'address is required: the outbound IPv4 or IPv6 address the schedule is for' }); }
      if (state.warmup.schedules.some((s) => s.address === address)) { return json(409, { error: 'a warm-up schedule for that address exists: change it (PUT), or delete it first' }); }
      const entry = { id: nextId++, address, plan: parsed.plan || 'standard', started: parsed.started || state.warmup.today, enabled: parsed.enabled !== false,
         day: 1, last_day: 30, complete: false, in_bind_setting: true,
         providers: ['google', 'microsoft', 'yahoo', 'apple', 'proton', 'other'].map((p) => ({ provider: p, sent: 0, deferred: 0, ceiling: p === 'other' ? 250 : 50 })) };
      state.warmup.schedules.push(entry);
      return json(201, entry);
   }
   if (/^\/api\/v1\/warmup\/\d+$/.test(path) && (method === 'PUT' || method === 'DELETE')) {
      const id = Number(segment(path, 4));
      const at = state.warmup.schedules.findIndex((s) => s.id === id);
      if (at < 0) { return json(404, { error: 'warm-up schedule not found' }); }
      if (method === 'DELETE') { state.warmup.schedules.splice(at, 1); return json(200, { deleted: true }); }
      Object.assign(state.warmup.schedules[at], parsed);
      return json(200, state.warmup.schedules[at]);
   }
   if (/^\/api\/v1\/queue\/\d+\/retry$/.test(path) && method === 'POST') { return json(200, { rescheduled: true }); }
   if (/^\/api\/v1\/queue\/\d+$/.test(path) && method === 'DELETE') {
      const id = Number(segment(path, 4));
      state.queue = state.queue.filter((m) => m.id !== id);
      return json(200, { deleted: true });
   }

   // The suppression list and the counts, under a domain; the sweep.
   if (/^\/api\/v1\/domains\/[^/]+\/(suppressions|bounces)(\?|$)/.test(path) && method === 'GET') {
      const domain = segment(path, 4);
      const query = path.indexOf('?') < 0 ? '' : path.slice(path.indexOf('?') + 1);
      const parameter = (name) => { const hit = query.split('&').filter((p) => p.split('=')[0] === name)[0]; return hit ? decodeURIComponent(hit.split('=').slice(1).join('=')) : ''; };
      const search = parameter('search');
      const offset = Number(parameter('offset') || 0);
      const limit = Number(parameter('limit') || 100);
      const all = (path.split('?')[0].endsWith('/bounces') ? state.bounceCounts[domain] : state.suppressions[domain]) || [];
      const matching = search ? all.filter((e) => e.address.indexOf(search) >= 0) : all;
      const answerBody = { domain, total: matching.length, offset, items: matching.slice(offset, offset + limit) };
      if (!path.split('?')[0].endsWith('/bounces')) { answerBody.processing_enabled = SETTING_GROUPS['/api/v1/settings/bounces'].values.processing_enabled; answerBody.list_applies = SETTING_GROUPS['/api/v1/settings/bounces'].values.processing_enabled || SETTING_GROUPS['/api/v1/settings/complaints'].values.processing_enabled; answerBody.action = SETTING_GROUPS['/api/v1/settings/bounces'].values.suppression_action; }
      return json(200, answerBody);
   }
   if (/^\/api\/v1\/domains\/[^/]+\/suppressions$/.test(path) && method === 'POST') {
      const domain = segment(path, 4);
      const address = String(parsed.address || '').trim().toLowerCase();
      for (const key of Object.keys(parsed)) { if (['address', 'note', 'expires_days'].indexOf(key) < 0) { return json(400, { error: 'unknown field: ' + key }); } }
      if (!/^[^@\s]+@[^@\s]+$/.test(address)) { return json(400, { error: 'address is not a valid email address' }); }
      if (address.endsWith('@' + domain)) { return json(400, { error: 'that address is hosted on this server; the suppression list only applies to addresses outside it' }); }
      state.suppressions[domain] = state.suppressions[domain] || [];
      if (state.suppressions[domain].some((e) => e.address === address)) { return json(409, { error: 'that address is already on the suppression list of this domain' }); }
      const record = Object.assign(suppressionRecord(address, 'manual', 'none', ''), { diagnostic: parsed.note || '' });
      state.suppressions[domain].unshift(record);
      return json(201, record);
   }
   if (/^\/api\/v1\/domains\/[^/]+\/suppressions\/[^/]+$/.test(path) && method === 'DELETE') {
      const domain = segment(path, 4);
      const address = segment(path, 6);
      const list = state.suppressions[domain] || [];
      const at = list.findIndex((e) => e.address === address);
      if (at < 0) { return json(404, { error: 'that address is not on the suppression list of this domain' }); }
      list.splice(at, 1);
      return json(200, { removed: true });
   }
   // The OpenPGP key directory: a domain's whole listing, and one account's keys.
   if (/^\/api\/v1\/domains\/[^/]+\/openpgp$/.test(path) && method === 'GET') {
      const domain = segment(path, 4);
      const keys = state.openPgpKeys[domain] || [];
      const record = state.domains.filter((d) => d.name === domain)[0];
      const serverEnabled = SETTING_GROUPS['/api/v1/settings/openpgp'].values.web_key_directory_enabled;
      const domainEnabled = !!(record && record.active && record.openpgp_publish);
      return json(200, { domain, server_enabled: serverEnabled, domain_enabled: domainEnabled,
         publish_new_accounts: !!(record && record.openpgp_publish_new_accounts), total: keys.length,
         keys: keys.map((k) => Object.assign({}, k, { published: serverEnabled && domainEnabled && k.published })) });
   }
   if (/^\/api\/v1\/accounts\/[^/]+\/openpgp$/.test(path) && method === 'PUT') {
      const address = segment(path, 4);
      for (const key of Object.keys(parsed || {})) { if (['key', 'allow_address_mismatch', 'publish'].indexOf(key) < 0) { return json(400, { error: 'unknown field: ' + key }); } }
      const text = String((parsed || {}).key || '');
      if (text.indexOf('PRIVATE KEY') >= 0 || text.indexOf('SECRET KEY') >= 0) {
         return json(400, { error: 'This is a SECRET key, and this server never holds one. Export the public half of it - gpg --export, not gpg --export-secret-keys - and upload that.' });
      }
      const domain = address.split('@')[1];
      state.openPgpKeys[domain] = state.openPgpKeys[domain] || [];
      const record = { id: state.openPgpKeys[domain].length + 1, address, fingerprint: '800b26ccc36e0e3b4a61a26b19fe7a2fe905df20', version: 4,
         algorithm: 1, algorithm_name: 'RSA', bits: 2048, user_id: 'Bob <' + address + '>', created: 1789526561, expires: 0,
         bytes: 1030, uploaded: 1789526700, uploaded_by: 'administrator', hashed_local_part: 'wtuqgy9r3oi71mesdjqd3fwj4qwjbwuu',
         advanced_url: 'https://openpgpkey.' + domain + '/.well-known/openpgpkey/' + domain + '/hu/wtuqgy9r3oi71mesdjqd3fwj4qwjbwuu',
         direct_url: 'https://' + domain + '/.well-known/openpgpkey/hu/wtuqgy9r3oi71mesdjqd3fwj4qwjbwuu', published: !!(parsed || {}).publish };
      state.openPgpKeys[domain].push(record);
      return json(201, record);
   }
   if (/^\/api\/v1\/accounts\/[^/]+\/openpgp\/[^/]+$/.test(path) && method === 'DELETE') {
      const address = segment(path, 4);
      const fingerprint = segment(path, 6);
      const domain = address.split('@')[1];
      const list = state.openPgpKeys[domain] || [];
      const at = list.findIndex((k) => k.address === address && k.fingerprint === fingerprint);
      if (at < 0) { return json(404, { error: 'this account holds no key with that fingerprint' }); }
      list.splice(at, 1);
      return json(200, { removed: true });
   }

   if (path === '/api/v1/bounces/expire' && method === 'POST') { state.bounceSweeps += 1; return json(200, { suppressions_expired: 0, counts_pruned: 0, report_keys_pruned: 0 }); }

   // The abuse reports recorded about a domain, and the sweep.
   if (/^\/api\/v1\/domains\/[^/]+\/complaints(\?|$)/.test(path) && method === 'GET') {
      const domain = segment(path, 4);
      const query = path.indexOf('?') < 0 ? '' : path.slice(path.indexOf('?') + 1);
      const parameter = (name) => { const hit = query.split('&').filter((p) => p.split('=')[0] === name)[0]; return hit ? decodeURIComponent(hit.split('=').slice(1).join('=')) : ''; };
      const search = parameter('search');
      const offset = Number(parameter('offset') || 0);
      const limit = Number(parameter('limit') || 100);
      const all = state.complaints[domain] || [];
      const matching = search ? all.filter((e) => (e.address + ' ' + e.sender + ' ' + e.subject).indexOf(search) >= 0) : all;
      const group = SETTING_GROUPS['/api/v1/settings/complaints'].values;
      return json(200, { domain, processing_enabled: group.processing_enabled, mailbox_configured: !!group.mailbox,
         window_days: 7, sent: 4000, complaints: matching.length,
         rate_ppm: 250, rate_judged: true, total: matching.length, offset, items: matching.slice(offset, offset + limit) });
   }
   if (path === '/api/v1/complaints/expire' && method === 'POST') { state.complaintSweeps += 1; return json(200, { complaints_pruned: 0 }); }

   // The reports a domain receives, read: the summary the chart is drawn from,
   // the listing, one report with its records, and the TLS-RPT reports. A
   // domain with no report mailbox answers empty documents and says so.
   if (/^\/api\/v1\/domains\/[^/]+\/(dmarc-reports|tls-reports)(\/|\?|$)/.test(path) && method === 'GET') {
      const domain = segment(path, 4);
      const record = state.domains.find((d) => d.name === domain);
      if (!record) { return json(404, { error: 'domain not found' }); }
      const query = path.indexOf('?') < 0 ? '' : path.slice(path.indexOf('?') + 1);
      const parameter = (name) => { const hit = query.split('&').filter((p) => p.split('=')[0] === name)[0]; return hit ? decodeURIComponent(hit.split('=').slice(1).join('=')) : ''; };
      const days = Number(parameter('days') || 30);
      const offset = Number(parameter('offset') || 0);
      const limit = Number(parameter('limit') || 50);
      const held = record.report_mailbox ? (state.dmarc[domain] || { reports: [], tls: [] }) : { reports: [], tls: [] };
      const plain = path.split('?')[0];
      if (plain.endsWith('/dmarc-reports/summary')) {
         const sources = {};
         let messages = 0, passed = 0, failed = 0, dkim = 0, spf = 0, quarantined = 0, rejected = 0;
         held.reports.forEach((r) => r.records.forEach((x) => {
            const ok = x.dkim === 'pass' || x.spf === 'pass';
            const s = sources[x.source_ip] || (sources[x.source_ip] = { ip: x.source_ip, messages: 0, passed: 0, failed: 0, dkim_passed: 0, spf_passed: 0, quarantined: 0, rejected: 0, header_from: x.header_from, reporters: 1 });
            messages += x.count; s.messages += x.count;
            if (ok) { passed += x.count; s.passed += x.count; } else { failed += x.count; s.failed += x.count; }
            if (x.dkim === 'pass') { dkim += x.count; s.dkim_passed += x.count; }
            if (x.spf === 'pass') { spf += x.count; s.spf_passed += x.count; }
            if (x.disposition === 'quarantine') { quarantined += x.count; s.quarantined += x.count; }
            if (x.disposition === 'reject') { rejected += x.count; s.rejected += x.count; }
         }));
         const byDay = [];
         for (let d = 0; d < days; d += 1) { byDay.push({ day: '2026-09-' + String(d + 1).padStart(2, '0'), messages: d === 14 ? messages : 0, passed: d === 14 ? passed : 0, failed: d === 14 ? failed : 0 }); }
         return json(200, { domain, days, since: 1786838400, until: 1789430400, report_mailbox: record.report_mailbox, mailbox_configured: !!record.report_mailbox,
            reports: held.reports.length, messages, passed, failed, dkim_passed: dkim, spf_passed: spf, quarantined, rejected,
            reporters: held.reports.map((r) => ({ org: r.org, reports: 1, messages: r.messages, checked: r.message_dmarc ? 1 : 0, authenticated: r.message_dmarc === 'pass' ? 1 : 0 })),
            by_day: byDay, sources: Object.keys(sources).map((k) => sources[k]), sources_total: Object.keys(sources).length });
      }
      if (plain.endsWith('/tls-reports')) {
         return json(200, { domain, days, since: 1786838400, mailbox_configured: !!record.report_mailbox,
            sessions_succeeded: held.tls.reduce((n, r) => n + r.successful, 0), sessions_failed: held.tls.reduce((n, r) => n + r.failed, 0),
            total: held.tls.length, offset, items: held.tls.slice(offset, offset + limit) });
      }
      if (plain.endsWith('/dmarc-reports')) {
         const items = held.reports.map((r) => { const item = Object.assign({}, r); item.records = r.records_kept; delete item.records_kept; return item; });
         return json(200, { domain, report_mailbox: record.report_mailbox, mailbox_configured: !!record.report_mailbox, total: items.length, offset, items: items.slice(offset, offset + limit) });
      }
      const id = Number(plain.split('/').pop());
      const one = held.reports.find((r) => r.id === id);
      if (!one) { return json(404, { error: 'not one of this domain\'s reports' }); }
      return json(200, one);
   }
   if (path === '/api/v1/domain-reports/expire' && method === 'POST') { state.domainReportSweeps += 1; return json(200, { dmarc_reports_pruned: 0, tls_reports_pruned: 0, older_than_days: 365, complete: true }); }

   if (path === '/api/v1/tlsa') { return json(200, state.tlsa); }

   if (path === '/api/v1/settings/messages' && method === 'GET') { return json(200, state.messages); }
   if (/^\/api\/v1\/settings\/messages\/[^/]+$/.test(path) && method === 'PUT') {
      const name = segment(path, 5);
      const at = state.messages.findIndex((m) => m.name === name);
      if (at < 0) { return json(404, { error: 'server message not found' }); }
      if (typeof parsed.text !== 'string') { return json(400, { error: 'text must be a string' }); }
      state.messages[at].text = parsed.text;
      return json(200, state.messages[at]);
   }

   // The verbs beside three of the groups.
   if (method === 'POST' && path === '/api/v1/settings/scripting/reload') { state.scriptReloads += 1; return json(200, { reloaded: true }); }
   if (method === 'POST' && path === '/api/v1/settings/scripting/check') { return json(200, { result: state.scriptProblem }); }
   if (method === 'POST' && path === '/api/v1/settings/breach-check/test') { return json(200, state.breachProbe); }
   if (method === 'POST' && path === '/api/v1/settings/cache/clear') {
      SETTING_GROUPS['/api/v1/settings/cache'].values.domain_cache_size_kb = 0;
      SETTING_GROUPS['/api/v1/settings/cache'].values.domain_hit_rate = 0;
      return json(200, { cleared: true });
   }
   if (method === 'POST' && path === '/api/v1/settings/indexing/index') { SETTING_GROUPS['/api/v1/settings/indexing'].values.total_indexed_count = 45; return json(200, { started: true }); }
   if (method === 'POST' && path === '/api/v1/settings/indexing/clear') { SETTING_GROUPS['/api/v1/settings/indexing'].values.total_indexed_count = 0; return json(200, { cleared: true }); }

   if (method === 'POST' && path === '/api/v1/settings/push/vapid') {
      SETTING_GROUPS['/api/v1/settings/push'].values.vapid_public_key = 'BNewPublicKeyOfTheServer';
      return json(200, { vapid_public_key: 'BNewPublicKeyOfTheServer' });
   }
   if (path in SETTING_GROUPS) {
      const group = SETTING_GROUPS[path];
      if (method === 'GET') { return json(200, group.values); }
      if (method === 'PUT') {
         for (const key of Object.keys(parsed)) {
            if (!(key in group.props)) { return json(400, { error: 'unknown key: ' + key }); }
            if (group.props[key].readOnly) { return json(400, { error: key + ' is read-only' }); }
            if (group.props[key].writeOnly) { secrets[key] = parsed[key]; continue; }
            group.values[key] = parsed[key];
         }
         return json(200, group.values);
      }
   }

   if (path === '/api/v1/session' && method === 'GET') {
      return json(200, whoami || { identity: 'administrator', credential: 'administrator', administrative: true,
         role: 'serveradmin', permissions: ['administer_server', 'view_configuration', 'view_accounts', 'view_queue', 'view_logs', 'manage_roles'], domains: [] });
   }
   if (path === '/api/v1/roles' && method === 'GET') {
      return json(200, { count: ROLE_CATALOGUE.length, roles: ROLE_CATALOGUE, permissions: ['view_configuration', 'view_accounts'] });
   }
   // The clients that have signed in to an account, and the revoke that ends
   // what one of them holds. Two clients, so the view can show that two sign-ins
   // are listed separately; the revoke answers what it ended.
   if (/^\/api\/v1\/accounts\/[^/]+\/devices$/.test(path) && method === 'GET') {
      const who = decodeURIComponent(path.split('/')[4]);
      if (who !== 'anna@example.com') { return json(404, { error: 'account not found' }); }
      return json(200, { devices: devices.slice() });
   }
   if (/^\/api\/v1\/accounts\/[^/]+\/devices\/[0-9a-f]{32}/.test(path) && method === 'DELETE') {
      const id = path.split('/')[6].split('?')[0];
      const found = devices.filter((d) => d.id === id)[0];
      if (!found) { return json(404, { error: 'no such device of this account' }); }
      found.revoked = true;
      return json(200, { revoked: true, forgotten: false, sessions_ended: 1, connections_dropped: 2,
         credential_disabled: path.indexOf('credential=revoke') >= 0 });
   }

   if (path === '/api/v1/role-assignments' && method === 'GET') {
      return json(200, { count: state.assignments.length, assignments: state.assignments });
   }
   if (path.indexOf('/api/v1/roles/') === 0 && method === 'DELETE') {
      const name = decodeURIComponent(path.slice('/api/v1/roles/'.length));
      const at = ROLE_CATALOGUE.findIndex((r) => r.name === name);
      if (at < 0) { return json(404, { error: 'no such role' }); }
      if (ROLE_CATALOGUE[at].builtin) { return json(400, { error: 'a built-in role cannot be deleted' }); }
      const holders = state.assignments.filter((a) => a.role === name).length;
      if (holders > 0) {
         return json(409, { error: 'The role ' + name + ' is held by ' + holders + (holders === 1 ? ' account' : ' accounts') +
            ', which must be given another role or have this one taken away before the role can be deleted.' });
      }
      ROLE_CATALOGUE.splice(at, 1);
      return json(200, { deleted: true });
   }
   if (/^\/api\/v1\/accounts\/[^/]+\/password$/.test(path) && method === 'POST') {
      if (!parsed.password) { return json(400, { error: 'password must not be empty' }); }
      return json(200, { reset: true });
   }
   if (/^\/api\/v1\/accounts\/[^/]+\/unlock$/.test(path) && method === 'POST') { return json(200, { unlocked: true }); }
   // Mailbox encryption at rest (RestApiMailboxKeys.cpp): an account's key -
   // read, switched, recovered with the domain's passphrase - and a domain's
   // recovery key, as the routes answer them.
   if (/^\/api\/v1\/accounts\/[^/]+\/mailbox-encryption(\/recover)?$/.test(path)) {
      const address = segment(path, 4);
      const key = state.mailboxKeys[address] || (state.mailboxKeys[address] = { enabled: false, keyed: false, open: false, pending: false, fingerprint: '', created: 0, changed: 0, zero_access: false, domain_recovery: true, recovery_current: false });
      if (/\/recover$/.test(path) && method === 'POST') {
         if (!parsed || parsed.passphrase !== 'the right passphrase') { return json(403, { error: 'the passphrase does not open the domain\'s recovery key', code: 'mailbox_key' }); }
         key.open = true;
         key.pending = false;
         return json(200, { recovered: true });
      }
      if (method === 'PUT') {
         if (parsed && parsed.enabled) {
            Object.assign(key, { keyed: true, enabled: true, pending: true, open: true, fingerprint: '0123456789abcdef', recovery_current: true });
            return json(201, key);
         }
         key.enabled = false;
         return json(200, key);
      }
      if (method === 'GET') { return json(200, key); }
   }
   if (/^\/api\/v1\/domains\/[^/]+\/mailbox-recovery$/.test(path)) {
      const name = segment(path, 4);
      const recovery = state.mailboxRecovery[name] || (state.mailboxRecovery[name] = { exists: false, accounts: 0 });
      if (method === 'PUT') {
         if (!parsed || !parsed.passphrase || parsed.passphrase.length < 12) { return json(400, { error: 'passphrase: at least 12 characters' }); }
         if (recovery.exists && !parsed.current_passphrase) { return json(409, { error: 'current_passphrase: the domain has a recovery key, and replacing it needs the passphrase that opens it' }); }
         const made = !recovery.exists;
         Object.assign(recovery, { exists: true, fingerprint: 'fedcba9876543210', hint: parsed.hint || '', created: 1790000000 });
         return json(made ? 201 : 200, recovery);
      }
      if (method === 'GET') { return json(200, recovery); }
   }
   if (/^\/api\/v1\/accounts\/[^/]+\/review$/.test(path) && (method === 'GET' || method === 'PUT')) {
      const reviewed = state.reviewAccounts[segment(path, 4)];
      if (!reviewed) { return json(404, { error: 'no such account' }); }
      if (method === 'PUT') {
         if (parsed && typeof parsed.exempt === 'boolean') { reviewed.exempt = parsed.exempt; }
         if (parsed && typeof parsed.sensitivity === 'string') { reviewed.sensitivity = parsed.sensitivity; }
         return json(200, { exempt: reviewed.exempt, sensitivity: reviewed.sensitivity });
      }
      return json(200, reviewed);
   }
   if (/^\/api\/v1\/accounts\/[^/]+\/review\/unlock$/.test(path) && method === 'POST') {
      const reviewed = state.reviewAccounts[segment(path, 4)];
      if (!reviewed) { return json(404, { error: 'no such account' }); }
      const was = reviewed.locked;
      reviewed.locked = false;
      return json(200, { unlocked: was });
   }
   if (path === '/api/v1/review' && method === 'GET') { return json(200, { mode: 'enforce', held_days: 7, messages: state.review }); }
   if (/^\/api\/v1\/review\/\d+\/release$/.test(path) && method === 'POST') {
      const id = Number(segment(path, 4));
      const at = state.review.findIndex((m) => m.id === id);
      if (at < 0) { return json(404, { error: 'no such held message' }); }
      state.review.splice(at, 1);
      return json(200, { released: true });
   }
   if (/^\/api\/v1\/review\/\d+$/.test(path) && method === 'DELETE') {
      const id = Number(segment(path, 4));
      const at = state.review.findIndex((m) => m.id === id);
      if (at < 0) { return json(404, { error: 'no such held message' }); }
      state.review.splice(at, 1);
      return json(200, { discarded: true });
   }
   if (path === '/api/v1/addresses/erase' && method === 'POST') { state.erasures = (state.erasures || 0) + 1; return json(200, { removed: 7, include_archive: true, include_logs: true, held: 2 }); }
   if (/^\/api\/v1\/accounts\/[^/]+\/push$/.test(path) && method === 'DELETE') { return json(200, { removed: 2 }); }
   if (/^\/api\/v1\/accounts\/[^/]+\/recovery\/codes$/.test(path) && method === 'DELETE') { return json(200, { codes_cleared: 10 }); }
   if (/^\/api\/v1\/accounts\/[^/]+\/recovery$/.test(path) && method === 'DELETE') { return json(200, { address_set: false }); }
   if (path.indexOf('/api/v1/role-assignments/') === 0) {
      const address = decodeURIComponent(path.slice('/api/v1/role-assignments/'.length));
      if (method === 'PUT') {
         state.assignments = state.assignments.filter((a) => a.address !== address);
         state.assignments.push({ address: address, role: parsed.role, domains: parsed.domains || '' });
         return json(200, state.assignments[state.assignments.length - 1]);
      }
      if (method === 'DELETE') {
         if (!state.assignments.some((a) => a.address === address)) { return json(404, { error: 'that account holds no role' }); }
         state.assignments = state.assignments.filter((a) => a.address !== address);
         return json(200, { removed: true });
      }
   }

   if (path === '/api/v1/rules' && method === 'GET') { return json(200, state.rules); }
   if (/^\/api\/v1\/domains\/[^/]+\/rules$/.test(path) && method === 'GET') { return json(200, state.domainRules || []); }
   if (/^\/api\/v1\/domains\/[^/]+\/rules$/.test(path) && method === 'POST') {
      const made = Object.assign({ id: 900 + (state.domainRules || []).length }, parsed);
      state.domainRules = (state.domainRules || []).concat([made]);
      return json(201, made);
   }
   if (path === '/api/v1/rules' && method === 'POST') {
      const rule = Object.assign({ id: nextId++ }, parsed);
      state.rules.push(rule);
      return json(201, rule);
   }
   if (/^\/api\/v1\/rules\/\d+$/.test(path)) {
      const id = Number(segment(path, 4));
      const at = state.rules.findIndex((r) => r.id === id);
      if (at < 0) { return json(404, { error: 'No global rule with that id' }); }
      if (method === 'PUT') { state.rules[at] = Object.assign({ id }, parsed); return json(200, state.rules[at]); }
      if (method === 'DELETE') { state.rules.splice(at, 1); return json(200, { deleted: true }); }
   }

   if (path === '/api/v1/routes' && method === 'GET') { return json(200, state.routes); }
   if (path === '/api/v1/routes' && method === 'POST') {
      const route = Object.assign({ id: nextId++, addresses: [] }, parsed);
      delete route.relayer_auth_password;
      state.routes.push(route);
      return json(201, route);
   }
   if (/^\/api\/v1\/routes\/\d+$/.test(path)) {
      const id = Number(segment(path, 4));
      const at = state.routes.findIndex((r) => r.id === id);
      if (at < 0) { return json(404, { error: 'Unknown id' }); }
      if (method === 'PUT') {
         const route = Object.assign({ id, addresses: [] }, parsed);
         delete route.relayer_auth_password;
         state.routes[at] = route;
         return json(200, route);
      }
      if (method === 'DELETE') { state.routes.splice(at, 1); return json(200, { deleted: true }); }
   }

   if (path === '/api/v1/remote-domains' && method === 'GET') { return json(200, state.remotedomains); }
   if (path === '/api/v1/remote-domains' && method === 'POST') {
      const policy = Object.assign({ id: nextId++ }, parsed);
      state.remotedomains.push(policy);
      return json(201, policy);
   }
   if (path === '/api/v1/remote-domains/verification-cache/clear' && method === 'POST') {
      return json(200, { cleared: 3 });
   }
   if (/^\/api\/v1\/remote-domains\/\d+$/.test(path)) {
      const id = Number(segment(path, 4));
      const at = state.remotedomains.findIndex((r) => r.id === id);
      if (at < 0) { return json(404, { error: 'Unknown id' }); }
      if (method === 'PUT') {
         const policy = Object.assign({ id }, parsed);
         state.remotedomains[at] = policy;
         return json(200, policy);
      }
      if (method === 'DELETE') { state.remotedomains.splice(at, 1); return json(200, { deleted: true }); }
   }

   if (path === '/api/v1/acme' && method === 'GET' && state.acme) { return json(200, state.acme); }
   if (path === '/api/v1/certificates' && method === 'GET') { return json(200, state.certificates); }
   if (path === '/api/v1/certificates' && method === 'POST') {
      const cert = { id: nextId++, name: parsed.name, certificate_file: parsed.certificate_file, private_key_file: parsed.private_key_file };
      if (parsed.private_key_password !== undefined) { secrets.private_key_password = parsed.private_key_password; }
      state.certificates.push(cert);
      return json(201, cert);
   }
   if (/^\/api\/v1\/certificates\/\d+$/.test(path) && method === 'DELETE') {
      const id = Number(segment(path, 4));
      if (state.ports.some((p) => p.certificate_id === id)) { return json(409, { error: 'The certificate is bound by port ' + state.ports.filter((p) => p.certificate_id === id)[0].id + '. Delete or rebind the port first.' }); }
      state.certificates = state.certificates.filter((c) => c.id !== id);
      return json(200, { deleted: true });
   }

   if (path === '/api/v1/ports' && method === 'GET') { return json(200, state.ports); }
   if (path === '/api/v1/ports' && method === 'POST') {
      const port = Object.assign({ id: nextId++, address: '0.0.0.0', connection_security: 'none', certificate_id: 0, client_certificate_policy: 'off', client_certificate_ca_file: '' }, parsed);
      state.ports.push(port);
      return json(201, port);
   }
   if (/^\/api\/v1\/ports\/\d+$/.test(path)) {
      const id = Number(segment(path, 4));
      const at = state.ports.findIndex((p) => p.id === id);
      if (at < 0) { return json(404, { error: 'Unknown id' }); }
      if (method === 'PUT') { state.ports[at] = Object.assign({ id, address: '0.0.0.0', connection_security: 'none', certificate_id: 0, client_certificate_policy: 'off', client_certificate_ca_file: '' }, parsed); return json(200, state.ports[at]); }
      if (method === 'DELETE') { state.ports.splice(at, 1); return json(200, { deleted: true }); }
   }
   if (path === '/api/v1/server/reinitialize' && method === 'POST') {
      // A reinitialise takes every key marked reinitialize, and ends a drain -
      // which the answer says, as the server's does.
      const drained = state.drain.draining;
      state.pendingSettings = state.pendingSettings.filter((p) => p.applies !== 'reinitialize');
      Object.assign(state.drain, { draining: false, since: '', sessions: 0, deliveries: 0, drained: false });
      return json(202, { reinitializing: true, drained });
   }
   if (path === '/api/v1/server/settings/pending' && method === 'GET') { return json(200, state.pendingSettings); }


   if (path === '/api/v1/webhooks' && method === 'GET') {
      return json(200, { webhooks: state.webhooks, events: ['message.received', 'message.delivered', 'message.bounced', 'message.quarantined', 'account.locked'] });
   }
   if (path === '/api/v1/webhooks' && method === 'POST') {
      if (state.webhooks.some((w) => w.name === parsed.name)) { return json(409, { error: 'a subscription called ' + parsed.name + ' exists already' }); }
      const subscription = Object.assign({ secret_set: !!parsed.secret, created: 1789000200, pending: 0, delivered: 0, dead_lettered: 0, dropped: 0 }, parsed);
      delete subscription.secret;
      state.webhooks.push(subscription);
      return json(201, subscription);
   }
   if (/^\/api\/v1\/webhooks\/deliveries(\?|$)/.test(path) && method === 'GET') {
      return json(200, { deliveries: state.webhookQueue });
   }
   if (path === '/api/v1/webhooks/run' && method === 'POST') {
      state.webhookQueue = [];
      state.webhooks.forEach((w) => { w.delivered = Number(w.delivered || 0) + Number(w.pending || 0); w.pending = 0; });
      return json(200, { delivered: 1, removed: 0 });
   }
   if (path === '/api/v1/webhooks/test' && method === 'POST') {
      return json(200, { queued: state.webhooks.filter((w) => w.enabled).length });
   }
   if (path === '/api/v1/inbound-webhooks' && method === 'GET') {
      return json(200, { inbound_webhooks: state.inboundHooks });
   }
   if (path === '/api/v1/inbound-webhooks' && method === 'POST') {
      if (state.inboundHooks.some((h) => h.address === parsed.address)) { return json(409, { error: 'an inbound webhook for ' + parsed.address + ' exists already' }); }
      const route = Object.assign({ secret_set: !!parsed.secret, private_network: true, created: 1789000300, pending: 0, delivered: 0, kept: 0, bounced: 0 }, parsed);
      delete route.secret;
      state.inboundHooks.push(route);
      return json(201, route);
   }
   if (/^\/api\/v1\/inbound-webhooks\/deliveries(\?|$)/.test(path) && method === 'GET') {
      return json(200, { deliveries: state.inboundQueue });
   }
   if (/^\/api\/v1\/inbound-webhooks\/deliveries\/[^/]+\/retry$/.test(path) && method === 'POST') {
      const id = Number(segment(path, 5));
      const delivery = state.inboundQueue.filter((d) => d.id === id)[0];
      if (!delivery) { return json(404, { error: 'no such delivery' }); }
      delivery.state = 'pending';
      delivery.attempts = 0;
      return json(200, { retried: true });
   }
   if (/^\/api\/v1\/inbound-webhooks\/deliveries\/[^/]+$/.test(path) && method === 'DELETE') {
      const id = Number(segment(path, 5));
      const at = state.inboundQueue.findIndex((d) => d.id === id);
      if (at < 0) { return json(404, { error: 'no such delivery' }); }
      state.inboundQueue.splice(at, 1);
      return json(200, { deleted: true });
   }
   if (path === '/api/v1/inbound-webhooks/run' && method === 'POST') {
      return json(200, { delivered: 2, removed: 0, running: false });
   }
   if (/^\/api\/v1\/inbound-webhooks\/[^/]+$/.test(path)) {
      const address = decodeURIComponent(segment(path, 4));
      const at = state.inboundHooks.findIndex((h) => h.address === address);
      if (at < 0) { return json(404, { error: 'no inbound webhook routes that address' }); }
      if (method === 'GET') { return json(200, state.inboundHooks[at]); }
      if (method === 'PUT') {
         const changed = Object.assign({}, state.inboundHooks[at], parsed);
         if (parsed && parsed.secret !== undefined) { changed.secret_set = true; }
         delete changed.secret;
         state.inboundHooks[at] = changed;
         return json(200, changed);
      }
      if (method === 'DELETE') { state.inboundHooks.splice(at, 1); return json(200, { deleted: true }); }
   }
   if (/^\/api\/v1\/webhooks\/[^/]+$/.test(path)) {
      const name = decodeURIComponent(segment(path, 4));
      const at = state.webhooks.findIndex((w) => w.name === name);
      if (at < 0) { return json(404, { error: 'no such webhook subscription' }); }
      if (method === 'GET') { return json(200, state.webhooks[at]); }
      if (method === 'PUT') {
         const changed = Object.assign({}, state.webhooks[at], parsed);
         // The server never hands a secret back, and a body that carries one
         // only flips secret_set.
         if (parsed && parsed.secret !== undefined) { changed.secret_set = true; }
         delete changed.secret;
         state.webhooks[at] = changed;
         return json(200, changed);
      }
      if (method === 'DELETE') { state.webhooks.splice(at, 1); return json(200, { deleted: true }); }
   }

   if (path === '/api/v1/settings/changed' && method === 'GET') { return json(200, state.changed); }
   if (/^\/api\/v1\/audit\/\d+\/revert$/.test(path) && method === 'POST') {
      state.auditReverts += 1;
      const id = Number(path.split('/')[4]);
      const row = state.audit.filter((e) => e.id === id)[0];
      if (!row || row.object_type !== 'setting') { return json(409, { error: 'Only a setting\'s change can be put back; this row records a change to a domain.' }); }
      return json(200, { reverted: true, id, setting: row.object, value: '60' });
   }
   if (path === '/api/v1/audit/verify' && method === 'GET') {
      if (state.auditIntact) { return json(200, { intact: true, rows_checked: state.audit.length, first_broken_id: 0, reason: '' }); }
      return json(200, { intact: false, rows_checked: 1, first_broken_id: 2, reason: 'the row has been edited' });
   }
   if (/^\/api\/v1\/audit(\?|$)/.test(path) && method === 'GET') {
      const offset = Number((/offset=(\d+)/.exec(path) || [0, 0])[1]);
      const limit = Number((/limit=(\d+)/.exec(path) || [0, 50])[1]);
      const actor = (/actor=([^&]*)/.exec(path) || [0, ''])[1];
      const matching = actor ? state.audit.filter((e) => e.actor.indexOf(decodeURIComponent(actor)) >= 0) : state.audit;
      return json(200, { total: matching.length, offset, entries: matching.slice(offset, offset + limit) });
   }

   if (path === '/api/v1/logs' && method === 'GET') { return json(200, state.logs); }
   if (path === '/api/v1/logs/sessions' && method === 'GET') {
      return json(200, { sessions: state.sessions, open: state.sessions.filter((s) => s.open).length, ended: state.sessions.filter((s) => !s.open).length,
         truncated: !!state.sessionsTruncated, held_lines: state.followLines.length, logging_enabled: true });
   }
   if (/^\/api\/v1\/logs\/sessions\/\d+/.test(path) && method === 'GET') {
      const id = Number(segment(path, 5));
      const session = state.sessions.find((s) => s.id === id);
      if (!session) { return json(404, { error: 'no such session' }); }
      const after = Number((/after=(\d+)/.exec(path) || [0, 0])[1]);
      const lines = state.followLines.filter((l) => l.seq > after);
      return json(200, { session: id, known: true, open: session.open, logged: true, cursor: lines.length ? lines[lines.length - 1].seq : after,
         dropped: false, waited: false, protocol: session.protocol, peer: session.peer, account: session.account, started: session.started, ended: session.ended, lines });
   }
   if (/^\/api\/v1\/logs\/[^/]+/.test(path) && method === 'GET') {
      const name = segment(path, 4);
      // A file that was in the listing a moment ago and has been rotated away
      // since: listed, and then not there to read.
      if (name === 'gone.log' || !state.logs.some((l) => l.name === name)) { return json(404, { error: 'No such log file' }); }
      const lines = Number((/lines=(\d+)/.exec(path) || [0, 200])[1]);
      return json(200, { lines: state.logLines.slice(-lines) });
   }

   // The reports. The recorded answers echo the window, the domain and the row
   // count out of the query string, so that what the page ASKED FOR is what the
   // assertions can read back: the failure this catches is a picker that draws
   // a date and sends yesterday's.
   if (path.startsWith('/api/v1/reports') && method === 'GET') {
      const query = path.indexOf('?') < 0 ? '' : path.slice(path.indexOf('?') + 1);
      const parameter = (name) => {
         const hit = query.split('&').filter((p) => p.split('=')[0] === name)[0];
         return hit ? decodeURIComponent(hit.split('=').slice(1).join('=')) : '';
      };
      const section = path.split('?')[0].slice('/api/v1/reports'.length).replace(/^\//, '');
      const from = parameter('from') || '2026-08-17';
      const to = parameter('to') || '2026-09-15';
      const domain = parameter('domain');
      const top = Number(parameter('top') || 10);

      if (!section) { return json(200, Object.assign({ from, to }, state.reportIndex)); }

      const built = (name) => {
         const made = clone(state.reportSections[name]);
         if (domain) { made.rows = made.rows.filter((r) => r.domain === undefined || r.domain === domain); }
         if (name === 'senders' || name === 'recipients' || name === 'mailboxes' || name === 'quota') { made.rows = made.rows.slice(0, top); }
         return made;
      };

      if (section === 'summary') {
         const sections = {};
         Object.keys(state.reportSections).forEach((name) => { sections[name] = built(name); });
         return json(200, { section: 'summary', from, to, domain, top, sections, omitted: state.reportOmitted });
      }

      if (!state.reportSections[section]) { return json(404, { error: 'no such report section' }); }
      if (parameter('format') === 'csv') {
         const made = built(section);
         const lines = [made.columns.join(',')].concat(made.rows.map((r) => made.columns.map((c) => r[c]).join(',')));
         return { status: 200, body: lines.join('\r\n') + '\r\n', headers: { 'Content-Type': 'text/csv; charset=utf-8' } };
      }
      return json(200, Object.assign({ from, to, domain, top }, built(section)));
   }

   // ---- the eight pages the Deck gained last -------------------------------
   //
   // Each route answers the shape the server's own answers have: the API keys
   // in an envelope with a count, the quarantine and the archive as bare
   // arrays, the metric history and the update verdict as objects.

   if (path === '/api/v1/passkeys/options' && method === 'POST') {
      return json(200, {
         challenge: 'Y2hhbGxlbmdl', timeout: 300000, attestation: 'none',
         rp: { id: 'deck.test', name: 'hMailServer' },
         user: { id: 'YWRtaW4', name: 'Administrator', displayName: 'Administrator' },
         pubKeyCredParams: [{ type: 'public-key', alg: -7 }, { type: 'public-key', alg: -257 }],
         authenticatorSelection: { residentKey: 'preferred', userVerification: 'required' },
         excludeCredentials: []
      });
   }
   if (path === '/api/v1/passkeys' && method === 'GET') {
      return json(200, { enabled: true, rp_id: 'deck.test', passkeys: state.passkeys });
   }
   if (path === '/api/v1/passkeys' && method === 'POST') {
      const wanted = parsed || {};
      if (!wanted.password) { return json(403, { error: 'the password is not correct' }); }
      const made = { id: 14, name: wanted.name || '', created: 1789800000, last_used: 0, transports: 'internal', algorithm: -7, aaguid: '' };
      state.passkeys.push(made);
      return json(201, made);
   }
   if (/^\/api\/v1\/passkeys\/\d+$/.test(path) && method === 'DELETE') {
      const id = Number(path.split('/').pop());
      const at = state.passkeys.findIndex((p) => p.id === id);
      if (at < 0) { return json(404, { error: 'no such passkey' }); }
      state.passkeys.splice(at, 1);
      return json(200, { deleted: true });
   }
   if (path === '/api/v1/apikeys' && method === 'GET') {
      return json(200, { count: state.apikeys.length, keys: state.apikeys });
   }
   if (path === '/api/v1/apikeys' && method === 'POST') {
      if (!parsed || !parsed.label) { return json(400, { error: 'label is required' }); }
      nextId += 1;
      const made = { id: 'k' + nextId, label: parsed.label, scope: parsed.scope || 'full', role: parsed.role || '',
         domains: parsed.domains || '', expires: parsed.expires || '', allowed_from: parsed.allowed_from || '', expired: false,
         send_from: parsed.send_from || '', send_per_minute: parsed.send_per_minute || 0, send_per_day: parsed.send_per_day || 0 };
      state.apikeys.push(made);
      // The token, once. What the server keeps is a hash of it.
      return json(201, Object.assign({ key: 'hmapi_' + nextId + 'shown-this-once-only' }, made));
   }
   if (/^\/api\/v1\/apikeys\/[^/]+$/.test(path) && method === 'DELETE') {
      const id = segment(path, 4);
      const at = state.apikeys.findIndex((k) => k.id === id);
      if (at < 0) { return json(404, { error: 'no such key' }); }
      state.apikeys.splice(at, 1);
      return json(200, { revoked: true });
   }

   // The list as the server answers it: the bare array without q, offset and
   // limit, and with any of them a page as an object - the list's own messages,
   // or with q every held message's that match it in the sender, the
   // recipients, the subject or the reason. quarantineOlder stands in for the
   // messages held beyond the list's page, which only a search reaches; an old
   // server answers the array whatever is asked, and a failing one refuses q.
   if (path.split('?')[0] === '/api/v1/quarantine' && method === 'GET') {
      const query = path.indexOf('?') < 0 ? '' : path.slice(path.indexOf('?') + 1);
      if (!query || state.quarantineOldServer) { return json(200, state.quarantine); }
      const parameter = (name) => { const hit = query.split('&').filter((p) => p.split('=')[0] === name)[0]; return hit ? decodeURIComponent(hit.split('=').slice(1).join('=').replace(/\+/g, ' ')) : ''; };
      const q = parameter('q').trim();
      if (q && state.quarantineSearchFails) { return json(500, { error: 'the quarantine could not be searched' }); }
      const every = state.quarantine.concat(state.quarantineOlder || []);
      const wanted = q.toLowerCase();
      const matched = every.filter((m) => [m.sender, m.recipients, m.subject, m.reason].some((v) => String(v || '').toLowerCase().indexOf(wanted) >= 0));
      const offset = Number(parameter('offset') || 0);
      const limit = Number(parameter('limit') || 100);
      const page = (q ? matched : state.quarantine).slice(offset, offset + limit);
      return json(200, { query: q, held: every.length, total: q ? matched.length : every.length, offset, count: page.length, messages: page });
   }
   // One action over many held messages, answered per message, as
   // QuarantineReview::Run answers: the entries allow and block write (one per
   // distinct sender or domain), then what became of each id. A public suffix
   // is refused with 409 unless the request confirms it; a dry run writes and
   // does nothing.
   if (path === '/api/v1/quarantine/bulk' && method === 'POST') {
      const body = parsed || {};
      const ids = Array.isArray(body.ids) ? body.ids : [];
      if (!ids.length || ['release', 'delete', 'allow', 'block'].indexOf(body.action) < 0) { return json(400, { error: 'action is required: release, delete, allow or block' }); }
      const lists = body.action === 'allow' || body.action === 'block';
      const list = lists ? (body.list || 'address') : '';
      const from = list === 'domain' ? (body.domain_from || 'sender') : '';
      const entries = [];
      const results = [];
      ids.forEach((id) => {
         // Any held message, as the server acts on one by its id: the list's, or one
         // held beyond it that only a search's answer brings to the page.
         const m = state.quarantine.concat(state.quarantineOlder || []).filter((x) => x.id === id)[0];
         if (!m) { results.push({ id, ok: false, outcome: '', error: 'No quarantined message with that id.' }); return; }
         if (lists) {
            const org = from === 'helo' ? m.helo_domain : m.sender_domain;
            const value = list === 'address' ? m.sender : (org ? '.' + org : '');
            if (!value) { results.push({ id, ok: false, outcome: '', error: 'The message has the null sender, so there is no sender domain to list.' }); return; }
            let entry = entries.filter((e) => e.value === value)[0];
            if (!entry) {
               entry = { value, list, match_on: list === 'domain' ? (from === 'helo' ? 'helo' : 'envelope') : '', public_suffix: list === 'domain' && ['.localhost', '.co.uk'].indexOf(value) >= 0,
                         outcome: body.dry_run ? 'add' : 'added', error: '', ids: [] };
               entries.push(entry);
            }
            entry.ids.push(id);
         }
         results.push({ id, ok: true, outcome: '', error: '' });
      });
      const suffixes = entries.filter((e) => e.public_suffix).map((e) => e.value);
      if (suffixes.length && !body.dry_run && body.confirm_public_suffix !== true) {
         return json(409, { error: 'Nothing was changed: ' + suffixes.join(', ') + ' is a public suffix, and an entry for it covers every domain registered beneath it. Send confirm_public_suffix to list it anyway.', public_suffix: suffixes });
      }
      const releasing = body.action === 'release' || body.action === 'allow';
      results.forEach((r) => {
         if (!r.ok) { return; }
         r.outcome = body.dry_run ? (releasing ? 'release' : 'delete') : (releasing ? 'released' : 'deleted');
         if (!body.dry_run) {
            state.quarantine = state.quarantine.filter((x) => x.id !== r.id);
            if (state.quarantineOlder) { state.quarantineOlder = state.quarantineOlder.filter((x) => x.id !== r.id); }
         }
      });
      if (!body.dry_run) { entries.forEach((e) => state.quarantineListed.push({ value: e.value, list: e.list, match_on: e.match_on, blocked: body.action === 'block' })); }
      return json(200, { action: body.action, list, domain_from: from, dry_run: !!body.dry_run, entries, results,
                         succeeded: results.filter((r) => r.ok).length, failed: results.filter((r) => !r.ok).length });
   }
   // The sweep removes nothing here, unless a check says how many it is to answer
   // that it removed.
   if (path === '/api/v1/quarantine/expire' && method === 'POST') { return json(200, { removed: state.quarantineExpired || 0, retention_days: 30 }); }
   if (/^\/api\/v1\/quarantine\/\d+\/(headers|preview)$/.test(path) && method === 'GET') {
      const id = Number(segment(path, 4));
      // Any held message, as the server reads one by its id: the list's, or one
      // held beyond it that only a search's answer brings to the page.
      if (!state.quarantine.concat(state.quarantineOlder || []).some((m) => m.id === id)) { return json(404, { error: 'quarantined message not found' }); }
      // Hostile, as a held message's own words are: the page must show them as text.
      return segment(path, 5) === 'headers'
         ? json(200, { id, truncated: false, limit: 65536, headers: 'Received: from localhost ([192.0.2.9])\r\nFrom: <' + HOSTILE + '>\r\nSubject: <b>held</b>\r\n' })
         : json(200, { id, truncated: true, from_html: false, limit: 16384, text: 'Pay now <script>alert(1)</script> at ' + HOSTILE });
   }

   if (/^\/api\/v1\/archive(\?|$)/.test(path) && method === 'GET') {
      const query = path.indexOf('?') < 0 ? '' : path.slice(path.indexOf('?') + 1);
      const parameter = (name) => {
         const hit = query.split('&').filter((p) => p.split('=')[0] === name)[0];
         return hit ? decodeURIComponent(hit.split('=').slice(1).join('=')) : '';
      };
      const has = (value, text) => !text || String(value).toLowerCase().indexOf(text.toLowerCase()) >= 0;
      const limit = Number(parameter('limit') || 200);
      const entries = state.archive.filter((e) =>
         (!parameter('domain') || e.domain === parameter('domain')) &&
         (!parameter('mailbox') || e.mailbox === parameter('mailbox')) &&
         has(e.sender, parameter('sender')) && has(e.recipients, parameter('recipient')) && has(e.subject, parameter('subject')) &&
         (!parameter('since') || e.time >= parameter('since')) && (!parameter('until') || e.time <= parameter('until')) &&
         (parameter('hold') !== '1' || e.hold));
      return json(200, entries.slice(0, limit));
   }
   if (/^\/api\/v1\/archive\/\d+\/hold$/.test(path)) {
      const id = Number(segment(path, 4));
      const entry = state.archive.filter((e) => e.id === id)[0];
      if (!entry) { return json(404, { error: 'no such archived message' }); }
      if (method === 'POST') { entry.hold = true; return json(200, { held: true }); }
      if (method === 'DELETE') { entry.hold = false; return json(200, { released: true }); }
   }

   // eDiscovery, answering as RestApiDiscovery does: the listing summarises,
   // a case's own route carries its searches, holds and exports, a search and
   // an export are queued (202), and a closed case refuses every write.
   const discoverySummary = (c) => ({ id: c.id, name: c.name, description: c.description, state: c.state, created_by: c.created_by, created: c.created,
      closed_by: c.closed_by, closed: c.closed, search_count: c.searches.length, live_holds: c.holds.filter((h) => h.live).length, export_count: c.exports.length });
   if (path === '/api/v1/discovery/cases' && method === 'GET') { return json(200, { cases: state.discovery.cases.map(discoverySummary) }); }
   if (path === '/api/v1/discovery/cases' && method === 'POST') {
      if (!parsed || !parsed.name) { return json(400, { error: 'A case needs a name of 1 to 255 characters, on one line.' }); }
      const created = { id: state.discovery.next++, name: parsed.name, description: parsed.description || '', state: 'open', created_by: 'Administrator', created: 1758070000,
         closed_by: '', closed: 0, searches: [], holds: [], exports: [] };
      state.discovery.cases.unshift(created);
      return json(201, Object.assign(discoverySummary(created), { searches: [], holds: [], exports: [] }));
   }
   const discoveryRoute = /^\/api\/v1\/discovery\/cases\/(\d+)(?:\/(close|searches|holds|exports)(?:\/(\d+)(?:\/(export))?)?)?$/.exec(path);
   if (discoveryRoute) {
      const theCase = state.discovery.cases.filter((c) => c.id === Number(discoveryRoute[1]))[0];
      if (!theCase) { return json(404, { error: 'There is no such case.' }); }
      const what = discoveryRoute[2] || '';
      const item = Number(discoveryRoute[3] || 0);
      if (!what && method === 'GET') { return json(200, Object.assign(discoverySummary(theCase), clone({ searches: theCase.searches, holds: theCase.holds, exports: theCase.exports }))); }
      if (what === 'exports' && item && method === 'GET') { return { status: 200, body: 'PK', headers: { 'Content-Type': 'application/zip' } }; }
      if (method !== 'GET' && !(what === 'holds' && method === 'DELETE') && theCase.state === 'closed') { return json(409, { error: 'The case is closed: a closed case runs no search. Open a new one.' }); }
      if (what === 'close' && method === 'POST') {
         let released = 0;
         theCase.holds.forEach((h) => { if (h.live) { h.live = false; h.released = 1758070100; h.released_by = 'Administrator'; released += 1; } });
         Object.assign(theCase, { state: 'closed', closed: 1758070100, closed_by: 'Administrator' });
         return json(200, Object.assign(discoverySummary(theCase), clone({ searches: theCase.searches, holds: theCase.holds, exports: theCase.exports }), { released }));
      }
      if (what === 'searches' && !item && method === 'POST') {
         if (!parsed || !Array.isArray(parsed.custodians) || !parsed.custodians.length) { return json(400, { error: 'custodians is required: the addresses or domains whose mail is searched, or "*".' }); }
         const search = { id: state.discovery.next++, case: theCase.id, state: 'queued', query: parsed, ran_by: 'Administrator', ran: 1758070050, finished: 0,
            scanned: 0, matched: 0, unsearched: 0, truncated: false, error: '' };
         theCase.searches.unshift(search);
         return json(202, { id: search.id, case: theCase.id, state: 'queued', query: parsed, status: '/api/v1/discovery/cases/' + theCase.id + '/searches/' + search.id });
      }
      if (what === 'searches' && item && discoveryRoute[4] === 'export' && method === 'POST') {
         const search = theCase.searches.filter((x) => x.id === item)[0];
         if (!search) { return json(404, { error: 'There is no such search in this case.' }); }
         if (search.state !== 'done') { return json(409, { error: 'The search is ' + search.state + ': only a search that has finished can be exported.' }); }
         const made = { id: state.discovery.next++, case: theCase.id, search: item, state: 'queued', file: '', size: 0, sha256: '', items: 0, withheld: 0,
            requested_by: 'Administrator', requested: 1758070200, finished: 0, error: '' };
         made.download = '/api/v1/discovery/cases/' + theCase.id + '/exports/' + made.id;
         theCase.exports.unshift(made);
         return json(202, { id: made.id, case: theCase.id, search: item, state: 'queued', status: '/api/v1/discovery/cases/' + theCase.id, download: made.download });
      }
      if (what === 'holds' && !item && method === 'POST') {
         if (parsed && parsed.scope === 'archive') {
            if (!theCase.searches.some((x) => x.id === parsed.search && x.state === 'done')) { return json(404, { error: 'A hold on archived copies names a search of this case in search.' }); }
            const hold = { id: state.discovery.next++, case: theCase.id, scope: 'archive', owner_id: 2750, name: 'anna@example.com (archive #2750)', placed_by: 'Administrator',
               placed: 1758070150, released_by: '', released: 0, live: true };
            theCase.holds.push(hold);
            return json(201, { placed: 1, holds: [hold.id] });
         }
         if (!parsed || (parsed.scope !== 'account' && parsed.scope !== 'domain')) { return json(400, { error: 'scope must be account, domain or archive.' }); }
         if (theCase.holds.some((h) => h.live && h.name === parsed.target)) { return json(409, { error: 'This case already holds ' + parsed.target + '.' }); }
         const hold = { id: state.discovery.next++, case: theCase.id, scope: parsed.scope, owner_id: 20, name: parsed.target, placed_by: 'Administrator',
            placed: 1758070150, released_by: '', released: 0, live: true };
         theCase.holds.push(hold);
         return json(201, { placed: 1, holds: [clone(hold)] });
      }
      if (what === 'holds' && item && method === 'DELETE') {
         const hold = theCase.holds.filter((h) => h.id === item)[0];
         if (!hold) { return json(404, { error: 'There is no such hold in this case.' }); }
         if (!hold.live) { return json(409, { error: 'The hold was released already.' }); }
         Object.assign(hold, { live: false, released: 1758070300, released_by: 'Administrator' });
         return json(200, clone(hold));
      }
      return json(404, { error: 'There is no such route.' });
   }

   if (/^\/api\/v1\/metrics\/history(\?|$)/.test(path) && method === 'GET') {
      const query = path.indexOf('?') < 0 ? '' : path.slice(path.indexOf('?') + 1);
      const parameter = (name) => {
         const hit = query.split('&').filter((p) => p.split('=')[0] === name)[0];
         return hit ? decodeURIComponent(hit.split('=').slice(1).join('=')) : '';
      };
      const metric = parameter('metric');
      const range = parameter('range') || '24h';
      const bucket = range === '30d' ? 60 : range === '7d' ? 10 : 1;
      const back = range === '30d' ? 43200 : range === '7d' ? 10080 : 1440;
      const values = state.metricsEnabled ? (state.metricsSamples[metric] || []) : [];
      const samples = values.map((value, i) => ({ time: '2026-09-16 1' + i + ':00:00', value }));
      return json(200, { metric, known: metric in state.metricsSamples, enabled: state.metricsEnabled,
         retention_days: state.metricsEnabled ? 7 : 0, minutes_back: back, bucket_minutes: bucket, samples });
   }

   if (path === '/api/v1/update' && method === 'GET') { return json(200, state.update); }
   // Maintenance mode: the same document for all three, 409 for a change the
   // state does not allow, as the server answers.
   if (path === '/api/v1/server/drain') {
      if (method === 'GET') { return json(200, state.drain); }
      if (method === 'POST') {
         if (state.drain.draining) { return json(409, { error: 'already draining' }); }
         Object.assign(state.drain, { draining: true, since: '2026-09-23 10:15:02', sessions: 1, deliveries: 0, drained: false });
         return json(200, state.drain);
      }
      if (method === 'DELETE') {
         if (!state.drain.draining) { return json(409, { error: 'not draining' }); }
         Object.assign(state.drain, { draining: false, since: '', sessions: 0, deliveries: 0, drained: false });
         return json(200, state.drain);
      }
   }
   if (/^\/api\/v1\/update\/(check|download|install)$/.test(path) && method === 'POST') {
      const verb = segment(path, 4);
      if (verb === 'check') {
         Object.assign(state.update, { state: 2, stateName: 'update-available', availableVersion: '6.3.4',
            releaseName: 'hMailServer 6.3.4', publishedAt: '2026-09-16 12:00:00', releaseUrl: 'https://example.invalid/releases/6.3.4',
            lastChecked: '2026-09-17 09:00:00', lastError: '',
            installer: { name: 'hMailServer-6.3.4.exe', url: 'https://example.invalid/hMailServer-6.3.4.exe', size: 12582912, digest: 'sha256:abc', bundleUrl: 'https://example.invalid/hMailServer-6.3.4.exe.sigstore' } });
      } else if (verb === 'download') {
         if (state.update.state !== 2) { return json(409, { error: 'there is nothing to download' }); }
         Object.assign(state.update, { state: 3, stateName: 'downloaded',
            downloaded: { path: 'C:\\hMailServer\\Data\\Updates\\hMailServer-6.3.4.exe', signer: 'Progressive Robot Ltd',
               logTime: '2026-09-17 09:05:00', verifiedAt: '2026-09-17 09:05:02' } });
      } else {
         if (state.update.state !== 3) { return json(409, { error: 'there is no verified installer to apply' }); }
         Object.assign(state.update, { state: 4, stateName: 'installing',
            apply: { status: 'started', version: '6.3.4', detail: 'handed to the update helper' } });
      }
      return json(200, state.update);
   }

   if (path === '/api/v1/srv' && method === 'GET') { return json(200, state.srv); }

   const folderAcl = /^\/api\/v1\/accounts\/([^/]+)\/folders\/(\d+)\/permissions(?:\/(\d+))?$/.exec(path);
   if (folderAcl) {
      const address = decodeURIComponent(folderAcl[1]);
      if (!Object.keys(state.accounts).some((d) => state.accounts[d].some((a) => a.address === address))) {
         return json(404, { error: 'account not found' });
      }
      const folder = Number(folderAcl[2]);
      const list = state.permissions[folder];
      if (!list) { return json(404, { error: 'no folder with that id in the account\'s tree' }); }
      if (method === 'GET') { return json(200, list); }
      if (method === 'POST') {
         if (parsed.type !== 'anyone' && !parsed.account && !parsed.group) { return json(400, { error: 'name the account or the group' }); }
         nextId += 1;
         const rights = parsed.rights || {};
         list.push({ id: nextId, folder_id: folder, type: parsed.type, account_id: 0, account: parsed.account || '',
            group_id: 0, group: parsed.group || '', rights,
            rights_text: Object.keys(rights).filter((r) => rights[r]).map((r) => r.charAt(0)).join('') });
         return json(201, list[list.length - 1]);
      }
      const id = Number(folderAcl[3]);
      const at = list.findIndex((p) => p.id === id);
      if (at < 0) { return json(404, { error: 'no such permission' }); }
      if (method === 'PUT') {
         list[at] = Object.assign({}, list[at], parsed, { id, folder_id: folder });
         return json(200, list[at]);
      }
      if (method === 'DELETE') { list.splice(at, 1); return json(200, { deleted: true }); }
   }
   if (/^\/api\/v1\/accounts\/[^/]+\/folders$/.test(path) && method === 'GET') {
      const address = segment(path, 4);
      if (!Object.keys(state.accounts).some((d) => state.accounts[d].some((a) => a.address === address))) {
         return json(404, { error: 'account not found' });
      }
      return json(200, state.folders);
   }

   if (path === '/api/v1/diagnostics/run' && method === 'POST') {
      state.diagnosticRuns.push(parsed);
      return json(200, { local_domain: (parsed && parsed.local_domain) || '', test_domain: (parsed && parsed.test_domain) || '',
         results: state.diagnostics });
   }

   return json(404, { error: 'No such route: ' + method + ' ' + path });
}

/* A reply held back until a check lets it go, for answers that arrive in
   another order than their questions: the server answers each keep-alive
   connection on a worker of its own, so a later request can be answered
   first. holdWhen(method, path, body) names what to hold; each held reply
   is a function in heldReplies that delivers it. */
let holdWhen = null;
const heldReplies = [];
function fetchStub(path, options) {
   const method = (options && options.method) || 'GET';
   const headers = (options && options.headers) || {};
   requests.push({ method, path, headers, body: options && options.body, credentials: options && options.credentials });
   const reply = answer(method, path, headers, options && options.body);
   const response = {
      status: reply.status,
      ok: reply.status >= 200 && reply.status < 300,
      headers: { get: (name) => (name in reply.headers ? reply.headers[name] : null) },
      text: () => Promise.resolve(reply.body),
      // A browser's Response has both, and the page uses both: text() where it
      // wants the empty body of a 204 as well, json() for a catalogue.
      json: () => new Promise((resolve, reject) => {
         try { resolve(JSON.parse(reply.body)); } catch (why) { reject(why); }
      })
   };
   if (holdWhen && holdWhen(method, path, options && options.body)) {
      return new Promise((resolve) => { heldReplies.push(() => resolve(response)); });
   }
   return Promise.resolve(response);
}

/* ------------------------------------------------------------ the assertions */

const failures = [];
let checks = 0;
function check(what, ok, detail) {
   checks += 1;
   if (!ok) { failures.push(what + (detail ? ' -- ' + detail : '')); }
}
function since(n) { return requests.slice(n); }
function called(from, method, pattern) {
   return since(from).filter((r) => r.method === method && (typeof pattern === 'string' ? r.path === pattern : pattern.test(r.path)));
}
function paths(from) { return JSON.stringify(since(from).map((r) => r.method + ' ' + r.path)); }
const flush = async () => { for (let i = 0; i < 60; i += 1) { await new Promise((r) => setImmediate(r)); } };

const $ = (s) => document.querySelector(s);
const $$ = (s) => document.querySelectorAll(s);
const content = () => $('#content');
function click(el) { if (!el) { throw new Error('nothing to click'); } el.dispatchEvent(makeEvent('click')); }
function act(name, attrs) {
   const all = content().querySelectorAll('button[data-act="' + name + '"]');
   const hit = all.filter((b) => !attrs || Object.keys(attrs).every((k) => b.attributes['data-' + k] === String(attrs[k])))[0];
   if (!hit) { throw new Error('the view has no button data-act="' + name + '"' + (attrs ? ' with ' + JSON.stringify(attrs) : '') + '; it has ' + JSON.stringify(all.map((b) => b.attributes))); }
   return hit;
}
async function goTo(view) {
   const button = $('#nav button[data-view="' + view + '"]');
   if (!button) { throw new Error('the sidebar has no view ' + view); }
   click(button);
   await flush();
}
function rows() { return content().querySelectorAll('tbody tr'); }
function setValue(id, value) { const el = document.getElementById(id); if (!el) { throw new Error('no control #' + id); } el.value = value; return el; }
function setChecked(id, on) { const el = document.getElementById(id); if (!el) { throw new Error('no control #' + id); } el.checked = on; return el; }
function lastBody(from, method, pattern) { const list = called(from, method, pattern); return list.length ? JSON.parse(list[list.length - 1].body) : null; }
function toastText() { return $('#toast').textContent; }
/* A key pressed: on a control when one holds the keyboard, on the document
   when none does. The page listens on the document and the event reaches it
   by bubbling, as it does in a browser. */
function press(key, on) { (on || document).dispatchEvent(makeEvent('keydown', { key, target: on || document })); }
const styleText = () => document.querySelector('style').textContent;
const themeStored = () => localStorage.getItem('hmsTheme');

/* ------------------------------------------------------------------ the run */

// Node has globals of its own with these names (navigator, localStorage,
// fetch, performance), some of them getter-only, so each one is defined over
// rather than assigned: the script must see this file's world and nothing of
// node's.
// The address bar, and the browser's WebAuthn, recorded rather than real. The
// Deck reads location.hash to pick up what the identity provider sent it back
// with, and asks navigator.credentials to make and use a passkey; both are
// stubs here, so what the checks below say is that the page passed the server's
// challenge to the authenticator and the authenticator's answer back.
const locationStub = {
   hash: '', pathname: '/', search: '', href: 'https://deck.test/',
   get origin() { return 'https://deck.test'; }
};
const historyStub = { replaceState: (state, title, url) => { locationStub.hash = ''; locationStub.pathname = String(url || '/'); } };
const credentialCalls = [];
let credentialsRefuse = false;
function buffersOf(text) { return Uint8Array.from(Buffer.from(String(text), 'binary')).buffer; }
const credentialsStub = {
   create: (options) => {
      credentialCalls.push({ kind: 'create', options: options });
      if (credentialsRefuse) { return Promise.reject(new Error('NotAllowedError')); }
      return Promise.resolve({ id: 'Y3JlZGVudGlhbA', type: 'public-key', response: {
         clientDataJSON: buffersOf('{"type":"webauthn.create"}'),
         attestationObject: buffersOf('attestation'),
         getTransports: () => ['internal']
      } });
   },
   get: (options) => {
      credentialCalls.push({ kind: 'get', options: options });
      if (credentialsRefuse) { return Promise.reject(new Error('NotAllowedError')); }
      return Promise.resolve({ id: 'Y3JlZGVudGlhbA', type: 'public-key', response: {
         clientDataJSON: buffersOf('{"type":"webauthn.get"}'),
         authenticatorData: buffersOf('authdata'),
         signature: buffersOf('signature'),
         userHandle: buffersOf('admin')
      } });
   }
};
const windowStub = { PublicKeyCredential: function () {} };
const world = {
   document, localStorage, sessionStorage, performance,
   location: locationStub, history: historyStub, window: windowStub,
   fetch: fetchStub,
   setTimeout: (fn, ms) => { const id = timerId++; timers.push({ id, fn, ms }); return id; },
   clearTimeout: (id) => { const at = timers.findIndex((t) => t.id === id); if (at >= 0) { timers.splice(at, 1); } },
   setInterval: (fn, ms) => { const id = timerId++; intervals.push({ id, fn, ms }); return id; },
   clearInterval: (id) => { const at = intervals.findIndex((t) => t.id === id); if (at >= 0) { intervals.splice(at, 1); } },
   requestAnimationFrame: (fn) => { fn(performance.now() + 5000); return 1; },
   confirm: (question) => { confirmations.push(question); return confirmAnswer; },
   matchMedia,
   navigator: { clipboard: { writeText: (text) => { clipboard = text; return Promise.resolve(); } }, credentials: credentialsStub },
   console: { error: (...args) => consoleErrors.push(args), log: () => { }, warn: () => { } }
};
Object.keys(world).forEach((name) => {
   Object.defineProperty(globalThis, name, { value: world[name], writable: true, configurable: true });
});

async function signIn() {
   $('#user').value = 'Administrator';
   $('#pass').value = 'a-real-password';
   $('#loginForm').dispatchEvent(makeEvent('submit'));
   await flush();
}

async function main() {
   vm.runInThisContext(fs.readFileSync(scriptPath, 'utf8'), { filename: 'deck.js' });
   await flush();
   let before = 0;
   let put = null;
   let posted = null;

   // ---- a first visit: the sign-in card, and not one request
   check('a first visit shows the sign-in card', $('#gate').style.display !== 'none' && $('#app').style.display === 'none',
      'gate=' + $('#gate').style.display + ' app=' + $('#app').style.display);
   // One request, and it is answered to a stranger by design: whether to draw a
   // passkey button and a "Sign in with ..." button. It carries no credential
   // and says nothing about who exists here - two flags and the issuer's host -
   // and the card cannot be drawn correctly without it. Anything else asked
   // before a session would be a request answered 401, and a 401 from this API
   // carries WWW-Authenticate: Basic, which puts the browser's own password box
   // on the screen in front of the page's.
   check('and asks the server nothing before there is a session but what the card may offer',
      requests.every((r) => r.path === '/api/v1/signin-methods'), paths(0));
   check('the sign-in card offers a passkey and the organisation\'s identity provider when the server says so',
      $('#passkeyBtn').hidden === false && $('#ssoBtn').hidden === false &&
      $('#ssoBtn').textContent.indexOf('login.example.com') >= 0,
      $('#ssoBtn').textContent);
   // SAML beside OIDC: a button of its own, named by the identity provider's
   // sign-in host, starting a Deck sign-in.
   check('the sign-in card offers the SAML identity provider beside the OpenID Connect one when the server says so',
      $('#samlBtn').hidden === false && $('#samlBtn').textContent === 'Contoso single sign-on',
      $('#samlBtn').textContent);
   {
      const hrefBefore = locationStub.href;
      click($('#samlBtn'));
      check('the SAML button starts a Control Deck sign-in at /portal/saml/start?deck=1',
         locationStub.href === '/portal/saml/start?deck=1', String(locationStub.href));
      locationStub.href = hrefBefore;
   }

   // ---- the identity provider's sentence, only for a sign-in this tab started
   // The page reads #sso_error= out of the address when it loads. Any link can
   // put a fragment there, so the sign-in card shows the sentence only when this
   // tab went to the identity provider in the last hour, inside the page's own
   // sentence, and once; the next load of the page spends the mark whatever its
   // address holds. The return is called here as the page's load calls it.
   {
      const hrefBefore = locationStub.href;
      const ssoReturn = () => vm.runInThisContext('singleSignOnReturn()');
      const said = (text) => 'Single sign-on did not complete: ' + text;
      const other = 'A sentence this tab did not ask for.';
      sessionStorage.removeItem('hmsSsoPending');
      $('#loginErr').textContent = '';
      locationStub.hash = '#sso_error=' + encodeURIComponent(other);
      let landed = await ssoReturn();
      check('a sentence in the address of a tab that started no sign-in is not shown, and the fragment is taken out of the address',
         landed === false && $('#loginErr').textContent === '' && locationStub.hash === '', JSON.stringify($('#loginErr').textContent));

      click($('#ssoBtn'));
      check('the identity provider\'s button marks this tab as one that started a sign-in, with the time, before it leaves',
         /^\d+$/.test(String(sessionStorage.getItem('hmsSsoPending'))) && locationStub.href === '/portal/oidc/start?deck=1',
         String(sessionStorage.getItem('hmsSsoPending')) + ' ' + locationStub.href);
      locationStub.href = hrefBefore;
      locationStub.hash = '#sso_error=' + encodeURIComponent('the identity provider did not complete the sign-in (access_denied)');
      landed = await ssoReturn();
      check('back from the sign-in it started, the tab shows the server\'s sentence inside its own, and the mark is spent',
         landed === false && $('#loginErr').textContent === said('the identity provider did not complete the sign-in (access_denied)') &&
         sessionStorage.getItem('hmsSsoPending') === null && locationStub.hash === '', JSON.stringify($('#loginErr').textContent));

      $('#loginErr').textContent = '';
      locationStub.hash = '#sso_error=' + encodeURIComponent(other);
      landed = await ssoReturn();
      check('a second sentence after that one, with the mark spent, is not shown',
         landed === false && $('#loginErr').textContent === '', JSON.stringify($('#loginErr').textContent));

      // A sign-in lives ten minutes on the server, and a return after that is
      // answered with the server's own sentence saying so: the tab that started
      // it shows that one, so the mark outlives the sign-in.
      sessionStorage.setItem('hmsSsoPending', String(Date.now() - 11 * 60 * 1000));
      $('#loginErr').textContent = '';
      locationStub.hash = '#sso_error=' + encodeURIComponent('the sign-in took longer than ten minutes; start it again');
      landed = await ssoReturn();
      check('back after longer than the ten minutes a sign-in lives, the tab shows the server\'s sentence that it took too long',
         landed === false && $('#loginErr').textContent === said('the sign-in took longer than ten minutes; start it again') &&
         sessionStorage.getItem('hmsSsoPending') === null, JSON.stringify($('#loginErr').textContent));

      sessionStorage.setItem('hmsSsoPending', String(Date.now() - 61 * 60 * 1000));
      $('#loginErr').textContent = '';
      locationStub.hash = '#sso_error=' + encodeURIComponent(other);
      landed = await ssoReturn();
      check('not after a mark older than an hour, which is spent all the same',
         landed === false && $('#loginErr').textContent === '' && sessionStorage.getItem('hmsSsoPending') === null,
         JSON.stringify($('#loginErr').textContent));

      sessionStorage.setItem('hmsSsoPending', 'soon');
      $('#loginErr').textContent = '';
      locationStub.hash = '#sso_error=' + encodeURIComponent(other);
      landed = await ssoReturn();
      check('nor one after a mark that is not a time', landed === false && $('#loginErr').textContent === '', JSON.stringify($('#loginErr').textContent));

      // A sign-in abandoned at the identity provider - Back, or the Deck opened
      // again in the tab - leaves no mark for a later fragment to find.
      click($('#ssoBtn'));
      locationStub.href = hrefBefore;
      locationStub.hash = '';
      landed = await ssoReturn();
      const spentByLoad = sessionStorage.getItem('hmsSsoPending') === null;
      $('#loginErr').textContent = '';
      locationStub.hash = '#sso_error=' + encodeURIComponent(other);
      landed = await ssoReturn();
      check('a load of the page with no sign-in in its address spends the mark, and a sentence after it is not shown',
         spentByLoad && landed === false && $('#loginErr').textContent === '', 'spent=' + spentByLoad + ' ' + JSON.stringify($('#loginErr').textContent));

      click($('#samlBtn'));
      locationStub.href = hrefBefore;
      $('#loginErr').textContent = '';
      locationStub.hash = '#sso_error=' + encodeURIComponent('x'.repeat(1000));
      landed = await ssoReturn();
      check('the SAML button marks the tab too, and the sentence shown is cut at 400 characters',
         landed === false && $('#loginErr').textContent === said('x'.repeat(400)), String($('#loginErr').textContent.length));

      click($('#ssoBtn'));
      locationStub.href = hrefBefore;
      $('#loginErr').textContent = '';
      locationStub.hash = '#sso_error=%E0%A4%A';
      landed = await ssoReturn();
      check('a broken escape in a sentence the tab asked for is shown as it came rather than stopping the page',
         landed === false && $('#loginErr').textContent === said('%E0%A4%A'), JSON.stringify($('#loginErr').textContent));
      $('#loginErr').textContent = '';
   }

   // ---- safeHref: the one gate every address from the server passes to be a link
   // The page's own function, on the forms an address takes: http and https,
   // relative to this page, are drawn as the browser resolves them; every other
   // scheme, however it is spelled or padded, and nothing at all, are not links.
   {
      const safeHref = vm.runInThisContext('safeHref');
      const admitted = [
         ['https://example.invalid/releases/6.3.4?x=1#notes', 'https://example.invalid/releases/6.3.4?x=1#notes'],
         ['http://example.invalid/', 'http://example.invalid/'],
         ['HTTPS://Example.INVALID/Notes', 'https://example.invalid/Notes'],
         ['  https://example.invalid/  ', 'https://example.invalid/'],
         ['/api/v1/discovery/cases/41/exports/5', 'https://deck.test/api/v1/discovery/cases/41/exports/5'],
         ['https://wiki.test/Backup', 'https://wiki.test/Backup']];
      const wrongAdmitted = admitted.filter((pair) => safeHref(pair[0]) !== pair[1]);
      check('safeHref draws an http or https address, and one relative to this page, as the browser resolves it',
         wrongAdmitted.length === 0, JSON.stringify(wrongAdmitted.map((pair) => [pair[0], safeHref(pair[0])])));
      const refused = ['', '   ', null, undefined, '\u0001', 'javascript:void(0)', 'JAVASCRIPT:void(0)', ' javascript:void(0)',
         '\u0000javascript:void(0)', '\u001fjavascript:void(0)', 'java\tscript:void(0)', 'java\r\nscript:void(0)',
         'data:text/html,notes', 'vbscript:notes', 'file:///C:/notes.txt', 'mailto:notes@example.invalid',
         'blob:https://deck.test/00000000-0000-0000-0000-000000000000', 'about:blank', 'ftp://example.invalid/', 'ws://example.invalid/', 'http://['];
      const wrongRefused = refused.filter((address) => safeHref(address) !== '');
      check('and nothing else: every other scheme, however spelled or padded, an address that does not parse, and no address',
         wrongRefused.length === 0, JSON.stringify(wrongRefused.map((address) => [address, safeHref(address)])));
   }

   // ---- signing in with a passkey rather than the administrator password
   {
      const beforePasskey = requests.length;
      const callsBefore = credentialCalls.length;
      click($('#passkeyBtn'));
      await flush();
      await flush();
      const sent = lastBody(beforePasskey, 'POST', '/api/v1/session');
      check('the passkey button asks for a challenge, uses it and sends the assertion to the session route',
         called(beforePasskey, 'POST', '/api/v1/passkeys/challenge').length === 1 &&
         credentialCalls.length === callsBefore + 1 && credentialCalls[callsBefore].kind === 'get' &&
         credentialCalls[callsBefore].options.publicKey.rpId === 'deck.test' &&
         sent !== null && !!sent.passkey && typeof sent.passkey.response.signature === 'string' &&
         $('#gate').style.display === 'none',
         JSON.stringify(sent) + ' ' + paths(beforePasskey));
      // The authenticator is asked to verify the person, not merely to notice
      // one: the server will not accept an assertion without it, because the
      // one-time code is skipped on the strength of that verification.
      check('and asks the authenticator to verify the person, not just to see one',
         credentialCalls[callsBefore].options.publicKey.userVerification === 'required',
         JSON.stringify(credentialCalls[callsBefore].options.publicKey.userVerification));
      // The header that says this came from this server's own page. A session
      // minted from the request BODY is the one a cross-site form could post,
      // so the server demands it on this route and the page sends it.
      check('and carries the header that proves the post came from this page',
         called(beforePasskey, 'POST', '/api/v1/session').every((r) => r.headers['X-Requested-With'] === 'hMailServer'),
         JSON.stringify(called(beforePasskey, 'POST', '/api/v1/session').map((r) => r.headers)));
      check('and no password was kept anywhere by it',
         [...sessionStorage.store.values()].every((v) => v.indexOf('a-real-password') < 0),
         JSON.stringify([...sessionStorage.store.entries()]));

      // ---- the administrator's own passkeys
      before = requests.length;
      await goTo('passkeys');
      check('the Passkeys view reads them once', called(before, 'GET', '/api/v1/passkeys').length === 1, paths(before));
      check('a row per passkey, with what it is reached by and one never used',
         rows().length === 2 && rows()[1].textContent.indexOf('usb,nfc') >= 0 &&
         rows()[1].textContent.indexOf('\u2014') >= 0,
         rows().map((r) => r.textContent).join(' | '));

      before = requests.length;
      setValue('pk_name', '  ');
      click(act('passkeynew'));
      await flush();
      check('a passkey with no name is refused here and never sent',
         called(before, 'POST', '/api/v1/passkeys').length === 0 && $('#err_passkeys').textContent.length > 0,
         $('#err_passkeys').textContent + ' ' + paths(before));

      before = requests.length;
      const createsBefore = credentialCalls.length;
      setValue('pk_name', 'Desk phone');
      setValue('pk_pass', 'a-real-password');
      click(act('passkeynew'));
      await flush();
      await flush();
      await flush();
      posted = lastBody(before, 'POST', '/api/v1/passkeys');
      check('adding one asks for the options, hands them to the authenticator and sends back what it made, with the administrator password',
         called(before, 'POST', '/api/v1/passkeys/options').length === 1 &&
         credentialCalls.length === createsBefore + 1 && credentialCalls[createsBefore].kind === 'create' &&
         credentialCalls[createsBefore].options.publicKey.rp.id === 'deck.test' &&
         posted !== null && posted.name === 'Desk phone' && posted.password === 'a-real-password' &&
         typeof posted.credential.response.attestationObject === 'string',
         JSON.stringify(posted));
      check('and it is in the list', rows().length === 3, rows().map((r) => r.textContent).join(' | '));

      before = requests.length;
      click(act('passkeydel', { id: '12', name: 'Console laptop' }));
      await flush();
      await flush();
      check('removing one asks with the name in the question and removes it by id',
         confirmations[confirmations.length - 1].indexOf('Console laptop') >= 0 &&
         called(before, 'DELETE', '/api/v1/passkeys/12').length === 1 && rows().length === 2,
         confirmations[confirmations.length - 1] + ' ' + paths(before));

      // Back to where the rest of the run expects to be: the dashboard chosen,
      // and signed out at the card. The view outlives a sign-out, so leaving
      // the Passkeys page selected would draw it again at the next sign-in.
      await goTo('dash');
      // And the card drawn again at sign-out asks again what it may offer:
      // with single sign-on switched off meanwhile, neither button stays - the
      // negative control of the checks at the first visit.
      const deckMethodsBefore = deckSignInMethods;
      deckSignInMethods = Object.assign({}, deckMethodsBefore, { oidc: false, saml: false });
      click($('#logoutBtn'));
      await flush();
      await flush();
      check('with single sign-on off, the card drawn again at sign-out offers neither the OpenID Connect nor the SAML button',
         $('#ssoBtn').hidden === true && $('#samlBtn').hidden === true,
         'sso hidden=' + $('#ssoBtn').hidden + ' saml hidden=' + $('#samlBtn').hidden);
      deckSignInMethods = deckMethodsBefore;
   }

   // ---- the tokens: one palette, a dark theme beside it, and nothing else
   const css = styleText();
   const blocks = css.match(/:root\{[^}]*\}/g) || [];
   const dark = css.match(/\[data-theme="dark"\]\{[^}]*\}/g) || [];
   check('the page declares one palette and one dark theme beside it', blocks.length === 1 && dark.length === 1,
      blocks.length + ' root blocks, ' + dark.length + ' dark blocks');
   check('and its colours are the webmail\'s, value for value, rather than a second design language',
      ['--bg:#f6f8fc', '--surface:#fff', '--accent:#1a73e8', '--accent-soft:#e8f0fe', '--danger:#d93025', '--good:#188038', '--warn:#b06000', '--cursor:#e8f0fe']
         .every((d) => blocks[0].indexOf(d) >= 0), blocks[0].slice(0, 240));
   check('the dark theme is the webmail\'s dark theme',
      ['--bg:#111418', '--surface:#1b1f24', '--accent:#8ab4f8', '--danger:#f28b82', '--good:#81c995'].every((d) => dark[0].indexOf(d) >= 0), dark[0].slice(0, 240));
   check('the spacing scale, the radii and the type ramp are the desktop console\'s',
      ['--s1:4px', '--s2:8px', '--s3:12px', '--s4:16px', '--s5:24px', '--s6:32px', '--radius-control:4px', '--radius:8px', '--pill:999px',
         '--fs-caption:12px', '--fs-body:14px', '--fs-subtitle:20px', '--fs-title:28px', '--cmdbar:48px'].every((d) => blocks[0].indexOf(d) >= 0));
   const painted = [blocks[0], dark[0]].reduce((text, block) => text.replace(block, ''), css).match(/#[0-9a-fA-F]{3,8}\b|\brgba?\(/g) || [];
   check('and every colour the page paints with comes from a token: not one literal outside those two blocks',
      painted.length === 0, JSON.stringify(painted));
   check('everything that takes the keyboard is ringed while it holds it, in the focus colour',
      /:focus-visible\{[^}]*outline:2px solid var\(--focus\)/.test(css) && (css.match(/:focus-visible/g) || []).length >= 4,
      (css.match(/:focus-visible/g) || []).length + ' focus-visible rules');

   // ---- where the keyboard is: one violet, the webmail's and the Control
   // Panel's, declared for both themes and used by every ring and field
   const tokenOf = (block, name) => { const m = new RegExp('--' + name + ':([^;]+);').exec(block); return m ? m[1].trim() : null; };
   check('the focus colour and its glow are tokens of both themes: a darker violet on the light theme, a lighter one on the dark',
      tokenOf(blocks[0], 'focus') === '#7542e5' && tokenOf(dark[0], 'focus') === '#b89cff' &&
      /^rgba\(117,66,229,\.\d+\)$/.test(String(tokenOf(blocks[0], 'focus-glow'))) && /^rgba\(184,156,255,\.\d+\)$/.test(String(tokenOf(dark[0], 'focus-glow'))),
      JSON.stringify([tokenOf(blocks[0], 'focus'), tokenOf(blocks[0], 'focus-glow'), tokenOf(dark[0], 'focus'), tokenOf(dark[0], 'focus-glow')]));
   const cssRules = [];
   css.replace(/\/\*[\s\S]*?\*\//g, '').replace(/([^{}]+)\{([^{}]*)\}/g, (whole, selector, body) => { cssRules.push({ selector: selector.trim(), body }); return whole; });
   const fieldRule = cssRules.find((r) => r.selector.indexOf('input:not([type=checkbox]') === 0 && /select:focus,textarea:focus$/.test(r.selector) && r.body.indexOf('box-shadow') >= 0);
   check('a field being typed in, clicked or tabbed into, has the focus colour for its border, a pixel more of it and a glow outside, and no outline over them',
      !!fieldRule && /border-color:var\(--focus\)/.test(fieldRule.body) &&
      /box-shadow:0 0 0 1px var\(--focus\),0 0 \d+px \d+px var\(--focus-glow\)/.test(fieldRule.body) && /outline:none/.test(fieldRule.body) &&
      /^input:not\([^)]*\):focus,select:focus,textarea:focus$/.test(fieldRule.selector),
      fieldRule ? fieldRule.selector + ' { ' + fieldRule.body + ' }' : 'no field focus rule');
   check('and a box to tick, a radio button, a slider, a colour and a file are left out of it, keeping the ring',
      !!fieldRule && ['checkbox', 'radio', 'range', 'color', 'file'].every((t) => fieldRule.selector.indexOf('[type=' + t + ']') >= 0));
   const accentRings = cssRules.filter((r) => /:focus/.test(r.selector) && /var\(--accent\)/.test(r.body));
   check('no ring or field the keyboard is in is drawn in the accent any more', accentRings.length === 0,
      JSON.stringify(accentRings.map((r) => r.selector)));
   check('the command bar\'s search box and the help pane\'s find box take the focus colour too',
      cssRules.some((r) => r.selector === '.search input:focus' && /border-color:var\(--focus\)/.test(r.body)) &&
      cssRules.some((r) => r.selector === '.helppane .find input:focus' && /border-color:var\(--focus\)/.test(r.body)));
   check('a field the keyboard scrolls into view stops short of the pane\'s edge, so its ring and glow are not cut off',
      cssRules.some((r) => r.selector === 'input,select,textarea' && /scroll-margin-block:\d+px/.test(r.body)));
   check('a field marked invalid keeps the danger colour for its border when it has the keyboard, and the focus glow goes outside it',
      cssRules.some((r) => /\[aria-invalid="true"\]:focus$/.test(r.selector) && /box-shadow:0 0 0 1px var\(--danger\),0 0 \d+px \d+px var\(--focus-glow\)/.test(r.body)) &&
      cssRules.some((r) => /\[aria-invalid="true"\]:focus/.test(r.selector) && /border-color:var\(--danger\)/.test(r.body)));
   check('under a forced-colours theme a field keeps an outline in the system\'s highlight rather than the violet, and the transition stops for reduced motion',
      /@media \(forced-colors:active\)\{\s*input:not\(\[type=checkbox\][^{]*\{outline:2px solid Highlight/.test(css) &&
      /@media \(prefers-reduced-motion:reduce\)\{input,select,textarea\{transition:none\}\}/.test(css));

   // ---- the shape: the frame the desktop console has, in the Deck's markup
   check('a command bar carries the brand, the view, the search, the link\'s pill and the buttons',
      $('.cmdbar .brand') !== null && $('.cmdbar #viewTitle') !== null && $('.cmdbar #deckSearch') !== null && $('.cmdbar #connPill') !== null &&
      $('.cmdbar #themeBtn') !== null && $('.cmdbar #keysBtn') !== null && $('.cmdbar #logoutBtn') !== null);
   check('a navigation column stands beside a content area under it', $('.shell .side #nav') !== null && $('.shell .main #content') !== null);
   check('and a link carries the keyboard past the navigation into the content',
      $('a.skip').getAttribute('href') === '#content' && $('#content').getAttribute('tabindex') === '-1');
   check('every view keeps its name as a tool tip, which is what it has left when the column is a rail',
      $$('#nav button').every((b) => b.getAttribute('title') === b.textContent.replace(/^\S/, '')),
      JSON.stringify($$('#nav button').map((b) => b.getAttribute('title'))));
   check('the theme starts at the system\'s choice, which is dark here, and is the body\'s attribute',
      document.body.getAttribute('data-theme') === 'dark' && $('#themeBtn').getAttribute('aria-label') === 'Theme: the system\'s choice',
      $('#themeBtn').getAttribute('aria-label'));

   // ---- a wrong password is the server's sentence
   $('#user').value = 'Administrator';
   $('#pass').value = 'wrong';
   $('#loginForm').dispatchEvent(makeEvent('submit'));
   await flush();
   check('a refused sign-in shows the server\'s own sentence', $('#loginErr').textContent === 'The password is wrong.', $('#loginErr').textContent);
   check('and the app stays hidden', $('#app').style.display === 'none');

   // ---- a second factor, when the credential has one
   otpRequired = true;
   $('#pass').value = 'a-real-password';
   $('#loginForm').dispatchEvent(makeEvent('submit'));
   await flush();
   check('a credential with a second factor makes the code field appear', $('#otp').hidden === false && $('#loginErr').textContent.indexOf('one-time code') >= 0,
      'otp.hidden=' + $('#otp').hidden + ' err=' + $('#loginErr').textContent);
   check('the password is still there for the second attempt', $('#pass').value === 'a-real-password');
   $('#otp').value = '123456';
   $('#loginForm').dispatchEvent(makeEvent('submit'));
   await flush();
   check('the code goes in its own header', requests.filter((r) => r.headers['X-hMailServer-OTP'] === '123456').length === 1);
   otpRequired = false;

   // ---- signed in
   const withAuth = requests.filter((r) => r.headers && r.headers.Authorization);
   check('the password went only to the session route', withAuth.every((r) => r.method === 'POST' && r.path === '/api/v1/session'),
      JSON.stringify(withAuth.map((r) => r.method + ' ' + r.path)));
   check('the password field is emptied once a session exists', $('#pass').value === '' && $('#otp').value === '' && $('#otp').hidden === true);
   check('nothing secret is stored in the browser',
      [...localStorage.store.values(), ...sessionStorage.store.values()].every((v) => v.indexOf('a-real-password') < 0 && v.indexOf('123456') < 0) &&
      [...localStorage.store.keys(), ...sessionStorage.store.keys()].every((k) => !/pass|token|secret|auth/i.test(k)),
      JSON.stringify([...sessionStorage.store.entries()]));
   check('a mark says this tab has a session, so a reload asks the server instead of showing the card', sessionStorage.getItem('hmsSession') === '1');
   check('the app is shown and the card is not', $('#app').style.display === 'flex' && $('#gate').style.display === 'none');
   check('the version comes from the status', $('#ver').textContent === '6.3.3', $('#ver').textContent);
   check('every write carries the header the server demands of a browser session',
      requests.filter((r) => r.method !== 'GET' && r.method !== 'HEAD').every((r) => r.headers['X-Requested-With'] === 'hMailServer'));
   check('every request is same-origin, with the cookie and nothing else',
      requests.filter((r) => r.path !== '/').every((r) => r.credentials === 'same-origin'));

   // ---- the dashboard
   const locale = (n) => n.toLocaleString();
   check('the dashboard shows the messages processed', $('#kpiProcessed').textContent === locale(1234), $('#kpiProcessed').textContent);
   check('the spam and virus counts', $('#kpiSpam').textContent === locale(56) && $('#kpiVirus').textContent === locale(7));
   check('the state as a badge', $('#kpiState').textContent === 'Running' && $('#kpiState').querySelector('.badge.good') !== null, $('#kpiState').innerHTML);
   check('the live sessions', $('#sesSmtp').textContent === '3' && $('#sesImap').textContent === '12' && $('#sesPop3').textContent === '1');
   check('the dashboard polls on an interval', intervals.length === 1 && intervals[0].ms === 3000, intervals.length + ' intervals');
   check('the link is a status pill: a colour, a mark and a word, never a colour alone',
      $('#connPill').getAttribute('data-level') === 'good' && $('#connMark').textContent === '\u25a0' && $('#connState').textContent === 'Connected',
      $('#connPill').innerHTML);
   const tile = $('#kpiProcessed');
   const beforePoll = requests.length;
   state.status.processedMessages = 1300;
   state.status.state = 4;
   fireIntervals();
   await flush();
   check('the poll reads the status again', called(beforePoll, 'GET', '/api/v1/status').length === 1, paths(beforePoll));
   check('and updates the tiles in place rather than redrawing the view', $('#kpiProcessed') === tile && tile.textContent === locale(1300), tile.textContent);
   check('a state other than running is a warning', $('#kpiState').querySelector('.badge.warn') !== null && $('#kpiState').textContent === 'Stopping', $('#kpiState').textContent);
   // Maintenance mode is named, not numbered: the drain is the one state an
   // operator puts the server in on purpose, and "State 5" would read as a fault.
   state.status.state = 5;
   fireIntervals();
   await flush();
   check('a draining server is named as such, as a warning', $('#kpiState').querySelector('.badge.warn') !== null && $('#kpiState').textContent === 'Draining', $('#kpiState').textContent);
   state.status.state = 3;

   // The message store beside the counters, from GET /api/v1/store.
   check('the dashboard shows the message store: the messages, what their files and the parts held once take, and what is saved',
      $('#storeSect') !== null && $('#storeSect').hidden === false &&
      $('#storeLogical').textContent === '50.0 MB' && $('#storeStored').textContent === '20.0 MB' &&
      $('#storeBlobs').textContent === '5.0 MB' && $('#storeSaved').textContent === '50.0%' &&
      $('#storeNote').textContent.indexOf('new mail is stored compressed') >= 0,
      $('#storeSect') ? $('#storeSect').textContent : 'no section');
   const storeBefore = state.store;
   state.store = Object.assign({}, storeBefore, { measured: 0, measuring: true });
   fireIntervals();
   await flush();
   check('a store not measured yet says it is being measured, and claims no saving',
      $('#storeSect').hidden === false &&
      $('#storeNote').textContent === 'Measuring the message store; the figures follow on the next refresh.' && $('#storeSaved').textContent === '\u2014',
      $('#storeNote').textContent + ' / ' + $('#storeSaved').textContent);
   // A server that never turned compression on measures nothing unless asked,
   // and the section says nothing rather than "measuring" for ever.
   state.store = Object.assign({}, storeBefore, { measured: 0, measuring: false, compression: false });
   fireIntervals();
   await flush();
   check('a store nothing measured and nothing measuring leaves the section hidden',
      $('#storeSect').hidden === true, $('#storeSect').hidden);
   state.store = storeBefore;
   fireIntervals();
   await flush();
   check('and a measurement shows it again',
      $('#storeSect').hidden === false && $('#storeSaved').textContent === '50.0%', $('#storeSaved').textContent);

   // ---- domains
   before = requests.length;
   await goTo('domains');
   check('the domains view reads the domains', called(before, 'GET', '/api/v1/domains').length === 1, paths(before));
   check('the title follows the view', $('#viewTitle').textContent === 'Domains');
   check('the sidebar marks the view', $('#nav button[data-view="domains"]').classList.contains('on') && !$('#nav button[data-view="dash"]').classList.contains('on'));
   check('one row per domain', rows().length === 3, rows().length + ' rows');
   check('an inactive domain says so', rows()[1].textContent.indexOf('Disabled') >= 0 && rows()[0].textContent.indexOf('Active') >= 0);
   check('a hostile domain name is text, never markup', content().querySelectorAll('img').length === 0 && rows()[1].textContent.indexOf(HOSTILE) >= 0,
      rows()[1].innerHTML);
   check('the poll is not running away from the dashboard', (() => { const n = requests.length; fireIntervals(); return requests.length === n; })());

   before = requests.length;
   click(act('accounts', { domain: HOSTILE }));
   await flush();
   check('the accounts button carries the name as data, and the request encodes it',
      called(before, 'GET', '/api/v1/domains/' + encodeURIComponent(HOSTILE) + '/accounts').length === 1, paths(before));
   check('the accounts are listed', rows().length === 1 && rows()[0].textContent.indexOf('x@' + HOSTILE) >= 0, rows().length + ' rows');
   click(act('domains'));
   await flush();
   click(act('accounts', { domain: 'example.com' }));
   await flush();
   check('two accounts in example.com', rows().length === 2 && rows()[1].textContent.indexOf('Disabled') >= 0);

   before = requests.length;
   click(act('add', { domain: 'example.com' }));
   await flush();
   check('an empty form is refused by the page, not the server', called(before, 'POST', /accounts/).length === 0 && $('#err_account').textContent.length > 0, $('#err_account').textContent);
   setValue('newAddr', 'carla@example.com');
   setValue('newPass', 'pw-for-carla');
   before = requests.length;
   click(act('add', { domain: 'example.com' }));
   await flush();
   const created = lastBody(before, 'POST', '/api/v1/domains/example.com/accounts');
   check('creating an account posts the address and the password', !!created && created.address === 'carla@example.com' && created.password === 'pw-for-carla' && Object.keys(created).length === 2,
      JSON.stringify(created));
   check('and the listing is read again', called(before, 'GET', '/api/v1/domains/example.com/accounts').length === 1, paths(before));
   check('so the new account is in it', rows().length === 3 && rows()[2].textContent.indexOf('carla@example.com') >= 0);
   check('and the toast says so', toastText().indexOf('carla@example.com') >= 0, toastText());

   nextRefusal = 'The address is already taken by an alias.';
   setValue('newAddr', 'dupe@example.com');
   setValue('newPass', 'x');
   click(act('add', { domain: 'example.com' }));
   await flush();
   check('a refusal is shown beside the form in the server\'s words', $('#err_account').textContent === 'The address is already taken by an alias.', $('#err_account').textContent);
   check('and went to the console with the whole exchange', consoleErrors.length > 0 && JSON.stringify(consoleErrors[consoleErrors.length - 1]).indexOf('400') >= 0);

   check('every account row offers everything the server holds as one download, at the account\'s own export route',
      !!$('a[href="/api/v1/accounts/bob%40example.com/export.zip"]') && 'download' in $('a[href="/api/v1/accounts/bob%40example.com/export.zip"]').attributes,
      content().innerHTML.slice(0, 300));
   before = requests.length;
   click(act('erasetraces', { address: 'bob@example.com' }));
   await flush();
   put = lastBody(before, 'POST', '/api/v1/addresses/erase');
   check('erasing the traces asks first, naming the address, and posts the address with the archive and the logs included',
      confirmations[confirmations.length - 1].indexOf('bob@example.com') >= 0 && put && put.address === 'bob@example.com' && put.include_archive === true && put.include_logs === true,
      paths(before));
   check('and says how many traces went', toastText().indexOf('7') >= 0 && toastText().indexOf('bob@example.com') >= 0, toastText());
   check('the confirmation names the messages outside a mailbox it will discard or end, and that a legal hold outranks it',
      confirmations[confirmations.length - 1].indexOf('held for review') >= 0 && confirmations[confirmations.length - 1].indexOf('legal hold') >= 0,
      confirmations[confirmations.length - 1]);
   check('and what a legal hold kept in place, counted', toastText().indexOf('2 item(s) left in place under a legal hold') >= 0, toastText());

   confirmAnswer = false;
   before = requests.length;
   click(act('del', { address: 'bob@example.com' }));
   await flush();
   check('deleting asks first, and no means nothing is sent', confirmations[confirmations.length - 1].indexOf('bob@example.com') >= 0 && called(before, 'DELETE', /accounts/).length === 0,
      paths(before));
   confirmAnswer = true;
   click(act('del', { address: 'bob@example.com' }));
   await flush();
   check('yes deletes by address', called(before, 'DELETE', '/api/v1/accounts/bob%40example.com').length === 1, paths(before));
   check('and the listing is read again without the account', rows().length === 2 && rows().every((r) => r.textContent.indexOf('bob@') < 0));

   // A lost phone: the row's Stop notifications removes every Web Push
   // subscription of the account and says how many devices that was.
   before = requests.length;
   click(act('stoppush', { address: 'anna@example.com' }));
   await flush();
   check('Stop notifications deletes the account\'s Web Push subscriptions by address and says how many devices',
      called(before, 'DELETE', '/api/v1/accounts/anna%40example.com/push').length === 1 && toastText().indexOf('2 device(s) of anna@example.com') >= 0, toastText() + ' ' + paths(before));

   // ---- the account editor: every key of the update, as the desktop dialog groups them
   before = requests.length;
   click(act('accountedit', { address: 'anna@example.com' }));
   await flush();
   check('Edit reads the account whole', called(before, 'GET', '/api/v1/accounts/anna%40example.com').length === 1 && content().querySelector('h2').textContent.indexOf('anna@example.com') >= 0, paths(before));
   const accountHeadings = content().querySelectorAll('h2').map((h) => h.textContent.trim());
   check('the editor is grouped as the desktop dialog is', ['General', 'Forwarding', 'Auto-reply', 'Spam', 'Signature', 'Sieve', 'Windows directory', 'Directory entry'].every((g) => accountHeadings.indexOf(g) >= 0) && accountHeadings.indexOf('Other') < 0,
      JSON.stringify(accountHeadings));
   const drawnAccount = content().querySelectorAll('input, select, textarea').map((el) => el.id).filter((id) => id.indexOf('acct_') === 0).map((id) => id.slice(5)).sort();
   check('every key of the update has a control, and nothing else', JSON.stringify(drawnAccount) === JSON.stringify(Object.keys(ACCOUNT_PROPS).sort()), JSON.stringify(drawnAccount));
   check('the values are the account\'s own: the name, the level as a select of three words, the script as a text area, the password box empty',
      document.getElementById('acct_first_name').value === 'Anna' && document.getElementById('acct_admin_level').tagName === 'SELECT' &&
      document.getElementById('acct_admin_level').querySelectorAll('option').map((o) => o.attributes.value).join(',') === 'user,domain,server' && document.getElementById('acct_admin_level').value === 'domain' &&
      document.getElementById('acct_sieve_script').tagName === 'TEXTAREA' && document.getElementById('acct_sieve_script').value.indexOf('fileinto') >= 0 &&
      document.getElementById('acct_password').type === 'password' && document.getElementById('acct_password').value === '' && document.getElementById('acct_spam_mark_threshold').value === '-1' &&
      document.getElementById('acct_forward_abort_spam_flagged').closest('.fr').textContent.indexOf('Do not forward messages flagged as spam') >= 0);
   check('the route\'s own description is the note', content().textContent.indexOf('a field the body does not name is left as it is') >= 0);
   check('Restricted delivery: a select of the three words with their numbers, following the domain, under its own heading',
      accountHeadings.indexOf('Restricted delivery') >= 0 && document.getElementById('acct_restricted_delivery').tagName === 'SELECT' &&
      document.getElementById('acct_restricted_delivery').querySelectorAll('option').map((o) => o.attributes.value).join(',') === '0,1,2' &&
      document.getElementById('acct_restricted_delivery').querySelectorAll('option').map((o) => o.textContent).join('|') === 'Follows the domain|Restricted: named domains only|Exempt: may exchange mail with anyone' &&
      document.getElementById('acct_restricted_delivery').value === '0' &&
      document.getElementById('acct_restricted_delivery').closest('.fr').textContent.indexOf('Exempt lets an account in a restricted domain exchange mail with anyone') >= 0,
      document.getElementById('acct_restricted_delivery') ? document.getElementById('acct_restricted_delivery').outerHTML : 'no control');
   before = requests.length;
   click(act('accountsave'));
   await flush();
   check('saving with nothing changed sends nothing', called(before, 'PUT', /accounts/).length === 0 && toastText() === 'Nothing changed', toastText());

   // ---- one value, three levels: the account's
   const accountLevels = () => document.getElementById('accountLevels');
   check('the account editor says what is in effect for the account and where it came from',
      accountLevels() !== null && accountLevels().textContent.indexOf('In effect: 14, from the server') >= 0 &&
      accountLevels().textContent.indexOf('In effect: Off, from the domain') >= 0 &&
      document.getElementById('lvlown_recall_days').checked === false && document.getElementById('lvl_quarantine_digest').tagName === 'SELECT',
      accountLevels() ? accountLevels().textContent.slice(0, 300) : 'no card');
   setChecked('lvlown_recall_days', true);
   setValue('lvl_recall_days', '5');
   setChecked('lvlown_quarantine_digest', true);
   setValue('lvl_quarantine_digest', 'true');
   // Something typed in the editor and not saved, which saving the levels must not lose.
   setValue('acct_forward_address', 'typed-not-saved@elsewhere.example');
   before = requests.length;
   click(act('accountlevels'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/accounts/anna%40example.com');
   check('the account\'s own values go as a number and a boolean, alone', JSON.stringify(put) === '{"recall_days":5,"quarantine_digest":true}', JSON.stringify(put));
   check('saving the levels redraws the levels alone: what was typed elsewhere in the editor is still there',
      document.getElementById('acct_forward_address') !== null && document.getElementById('acct_forward_address').value === 'typed-not-saved@elsewhere.example',
      document.getElementById('acct_forward_address') ? document.getElementById('acct_forward_address').value : 'no field');
   check('and the account is read again: its own now, over the domain\'s and the server\'s', called(before, 'GET', '/api/v1/accounts/anna%40example.com').length === 1 &&
      accountLevels().textContent.indexOf('In effect: 5, from the account') >= 0 && accountLevels().textContent.indexOf('In effect: On, from the account') >= 0 &&
      toastText() === 'Levels saved', accountLevels().textContent.slice(0, 400));
   setChecked('lvlown_recall_days', false);
   before = requests.length;
   click(act('accountlevels'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/accounts/anna%40example.com');
   check('unticking sends null, and the account takes the level above again', JSON.stringify(put) === '{"recall_days":null}' &&
      accountLevels().textContent.indexOf('In effect: 14, from the server') >= 0, JSON.stringify(put));
   setChecked('lvlown_quarantine_digest', false);
   click(act('accountlevels'));
   await flush();
   setChecked('acct_forward_enabled', true);
   setValue('acct_forward_address', 'anna@elsewhere.example');
   setValue('acct_message_retention_days', '90');
   before = requests.length;
   click(act('accountsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/accounts/anna%40example.com');
   check('saving sends only the keys that changed, numbers as numbers', JSON.stringify(put) === '{"forward_enabled":true,"forward_address":"anna@elsewhere.example","message_retention_days":90}', JSON.stringify(put));
   check('then reads the account again and stays in the editor with what the server holds', called(before, 'GET', '/api/v1/accounts/anna%40example.com').length === 1 &&
      document.getElementById('acct_forward_address').value === 'anna@elsewhere.example' && document.getElementById('acct_message_retention_days').value === '90' && toastText() === 'Account saved', toastText());
   setValue('acct_password', 'a-new-one');
   before = requests.length;
   click(act('accountsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/accounts/anna%40example.com');
   check('a filled password goes alone, and its box is empty after the re-read', JSON.stringify(put) === '{"password":"a-new-one"}' && secrets.account_password === 'a-new-one' && document.getElementById('acct_password').value === '', JSON.stringify(put));
   setValue('acct_max_size_mb', 'lots');
   before = requests.length;
   click(act('accountsave'));
   await flush();
   check('a size that is not a number is refused by the page', called(before, 'PUT', /accounts/).length === 0 && document.getElementById('err_accountedit').textContent.indexOf('whole number') >= 0);
   setValue('acct_max_size_mb', '500');
   nextRefusal = 'The password does not meet the policy.';
   click(act('accountsave'));
   await flush();
   check('a refusal keeps the editor open with the server\'s sentence', document.getElementById('err_accountedit').textContent === 'The password does not meet the policy.' && document.getElementById('acct_max_size_mb').value === '500');
   nextRefusalAnswer = { status: 400, body: { error: 'This password appears in a published data breach, so it would be among the first tried against this account. Choose a different one.', reason: 'breached' } };
   click(act('accountsave'));
   await flush();
   check('a breach-check refusal is shown in English on an English page', document.getElementById('err_accountedit').textContent === 'This password appears in a published data breach, so it would be among the first tried against this account. Choose a different one.',
      document.getElementById('err_accountedit').textContent);
   nextRefusalAnswer = { status: 400, body: { error: 'A sentence the page has no word for.', reason: 'something_new' } };
   click(act('accountsave'));
   await flush();
   check('a reason the page does not know shows the server\'s own sentence', document.getElementById('err_accountedit').textContent === 'A sentence the page has no word for.', document.getElementById('err_accountedit').textContent);
   before = requests.length;
   click(act('accounts', { domain: 'example.com' }));
   await flush();
   check('the way back re-reads the accounts of the domain', called(before, 'GET', '/api/v1/domains/example.com/accounts').length === 1 && rows().length === 2 && act('accountedit', { address: 'carla@example.com' }) !== null);


   // ---- the account's directory entry: its own route, its own card
   before = requests.length;
   click(act('accountedit', { address: 'anna@example.com' }));
   await flush();
   check('the editor reads the account\'s directory entry on its own route', called(before, 'GET', '/api/v1/accounts/anna%40example.com/directory').length === 1, paths(before));
   check('the directory card is drawn with a control for each field and the opt-out',
      content().querySelectorAll('h2').map((h) => h.textContent.trim()).indexOf('Directory entry') >= 0 &&
      ['title', 'department', 'phone', 'mobile', 'office', 'note'].every((k) => !!document.getElementById('dir_' + k)) && !!document.getElementById('dirOptOut'),
      JSON.stringify(content().querySelectorAll('h2').map((h) => h.textContent.trim())));
   before = requests.length;
   click(act('accountdirsave'));
   await flush();
   check('saving the entry with nothing changed sends nothing', called(before, 'PUT', /directory/).length === 0 && toastText() === 'Nothing changed', toastText());
   setValue('dir_title', 'Head of Engines');
   setValue('dir_phone', '+44 20 7946 0000');
   setValue('dir_note', 'Asked to be listed as Ada.');
   before = requests.length;
   click(act('accountdirsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/accounts/anna%40example.com/directory');
   check('saving sends only the fields that changed, the note among them',
      JSON.stringify(put) === '{"title":"Head of Engines","phone":"+44 20 7946 0000","note":"Asked to be listed as Ada."}', JSON.stringify(put));
   check('then reads the entry again and redraws from what the server holds',
      called(before, 'GET', '/api/v1/accounts/anna%40example.com/directory').length === 1 &&
      document.getElementById('dir_title').value === 'Head of Engines' && toastText() === 'Directory entry saved', toastText());
   setChecked('dirOptOut', true);
   before = requests.length;
   click(act('accountdirsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/accounts/anna%40example.com/directory');
   check('the administrator can take the account out of the directory, and that alone is sent', JSON.stringify(put) === '{"opt_out":true}', JSON.stringify(put));
   setChecked('dirOptOut', false);
   before = requests.length;
   click(act('accountdirsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/accounts/anna%40example.com/directory');
   check('and put it back', JSON.stringify(put) === '{"opt_out":false}', JSON.stringify(put));
   check('a person has no booking policy, and the editor did not ask for one',
      called(0, 'GET', /anna%40example\.com\/booking-policy/).length === 0 &&
      content().querySelectorAll('h2').map((h) => h.textContent.trim()).indexOf('Booking policy') < 0, paths(0));
   // Back to the listing, which is where the block below begins.
   click(act('accounts', { domain: 'example.com' }));
   await flush();

   // ---- a room: the account's kind, and its booking policy on its own route
   // (ResourceBooking.h, docs/RoomsAndEquipment.md)
   const roomRecord = state.accounts['example.com'].filter((a) => a.address === 'carla@example.com')[0];
   roomRecord.kind = 'room';
   before = requests.length;
   click(act('accountedit', { address: 'carla@example.com' }));
   await flush();
   check('a room\'s kind is a select of the three words, and its booking policy is read on its own route and drawn as a card',
      !!document.getElementById('acct_kind') && document.getElementById('acct_kind').tagName === 'SELECT' && document.getElementById('acct_kind').value === 'room' &&
      document.getElementById('acct_kind').querySelectorAll('option').map((o) => o.textContent).join('|') === 'A person|A room|A piece of equipment' &&
      called(before, 'GET', '/api/v1/accounts/carla%40example.com/booking-policy').length === 1 &&
      content().querySelectorAll('h2').map((h) => h.textContent.trim()).indexOf('Booking policy') >= 0 &&
      document.getElementById('bk_window').value === '180' && document.getElementById('bk_day_mon').checked && !document.getElementById('bk_day_sat').checked,
      paths(before));
   before = requests.length;
   click(act('bookingsave'));
   await flush();
   check('saving the booking policy with nothing changed sends nothing', called(before, 'PUT', /booking-policy/).length === 0 && toastText() === 'Nothing changed', toastText());
   setValue('bk_window', '30');
   setChecked('bk_hours', true);
   setChecked('bk_day_fri', false);
   setValue('bk_bookers', 'Managers\nalice@example.com');
   before = requests.length;
   click(act('bookingsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/accounts/carla%40example.com/booking-policy');
   check('saving the booking policy sends only what changed, and draws what the server holds',
      JSON.stringify(put) === '{"working_hours_only":true,"window_days":30,"work_days":["mon","tue","wed","thu"],"bookers":["Managers","alice@example.com"]}' &&
      called(before, 'GET', '/api/v1/accounts/carla%40example.com/booking-policy').length === 1 && toastText() === 'Booking policy saved' &&
      document.getElementById('bk_window').value === '30' && !document.getElementById('bk_day_fri').checked, JSON.stringify(put));
   roomRecord.kind = 'person';
   click(act('accounts', { domain: 'example.com' }));
   await flush();

   // ---- the recovery card: what an administrator may know about an account's
   // way back in, and the two things they may do about it. Two acts and not
   // one, because they answer two reports - "that address is not mine any more"
   // and "somebody else has seen my code card".
   click(act('accountedit', { address: 'anna@example.com' }));
   await flush();
   check('the account editor shows the recovery address masked, that it is confirmed, and how many codes are unused',
      content().textContent.indexOf('o....@e.........example') >= 0 &&
      content().textContent.indexOf('7 unused recovery codes.') >= 0,
      content().textContent.slice(0, 300));
   before = requests.length;
   click(act('recoverycodesclear'));
   await flush();
   check('and Clear the recovery codes ends the set through its own route, leaving the address alone',
      called(before, 'DELETE', '/api/v1/accounts/anna%40example.com/recovery/codes').length === 1 &&
      called(before, 'DELETE', '/api/v1/accounts/anna%40example.com/recovery').length === 0, paths(before));
   click(act('accounts', { domain: 'example.com' }));
   await flush();

   // ---- encryption at rest (RestApiMailboxKeys.cpp): the account's key card,
   // the switch, and the recovery that opens the key with the domain's
   // passphrase to set a new password
   before = requests.length;
   click(act('accountedit', { address: 'anna@example.com' }));
   await flush();
   check('the account editor reads whether the mailbox is encrypted at rest, and says it is off, with no recovery to offer',
      called(before, 'GET', '/api/v1/accounts/anna%40example.com/mailbox-encryption').length === 1 &&
      content().textContent.indexOf('Encryption at rest') >= 0 && content().textContent.indexOf('Off: mail is stored as it arrives.') >= 0 &&
      content().querySelectorAll('button[data-act="mkeyrecover"]').length === 0, paths(before));
   before = requests.length;
   click(act('mkeyon'));
   await flush();
   posted = lastBody(before, 'PUT', '/api/v1/accounts/anna%40example.com/mailbox-encryption');
   check('Switch encryption on puts enabled true and draws the key waiting for the first sign-in, with the switch-off and the recovery offered',
      JSON.stringify(posted) === '{"enabled":true}' && toastText() === 'Encryption at rest switched on' &&
      content().textContent.indexOf('waits for the person') >= 0 && act('mkeyoff') !== null && act('mkeyrecover') !== null &&
      document.getElementById('mkeyStale') === null,
      JSON.stringify(posted) + ' ' + toastText());
   before = requests.length;
   click(act('mkeyrecover'));
   await flush();
   check('the recovery with nothing typed is refused by the page, and nothing is sent',
      called(before, 'POST', /mailbox-encryption\/recover/).length === 0 &&
      document.getElementById('err_mkey').textContent === 'Both the domain\'s recovery passphrase and a new password are needed.',
      document.getElementById('err_mkey').textContent);
   setValue('mkeyPassphrase', 'a wrong passphrase');
   setValue('mkeyPassword', 'New-Passw0rd!');
   click(act('mkeyrecover'));
   await flush();
   check('a passphrase that does not open the recovery key is refused in the server\'s own words',
      document.getElementById('err_mkey').textContent.indexOf('does not open') >= 0, document.getElementById('err_mkey').textContent);
   setValue('mkeyPassphrase', 'the right passphrase');
   setValue('mkeyPassword', 'New-Passw0rd!');
   before = requests.length;
   click(act('mkeyrecover'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/accounts/anna%40example.com/mailbox-encryption/recover');
   check('the recovery posts the passphrase and the new password, and the key is drawn open',
      posted !== null && posted.passphrase === 'the right passphrase' && posted.password === 'New-Passw0rd!' &&
      toastText() === 'The mailbox was recovered: the new password opens it.' && content().textContent.indexOf('The key is open') >= 0,
      JSON.stringify(posted) + ' ' + toastText());
   before = requests.length;
   click(act('mkeyoff'));
   await flush();
   posted = lastBody(before, 'PUT', '/api/v1/accounts/anna%40example.com/mailbox-encryption');
   check('Switch encryption off puts enabled false and says that what is encrypted stays so',
      JSON.stringify(posted) === '{"enabled":false}' && toastText() === 'Encryption at rest switched off' &&
      content().textContent.indexOf('What was stored encrypted stays encrypted') >= 0, JSON.stringify(posted) + ' ' + toastText());

   // A key whose recovery copy the domain's recovery key does not open - the
   // server answers recovery_current false - is said so on the card, since
   // the recovery form beside it would be refused.
   state.mailboxKeys['anna@example.com'].recovery_current = false;
   click(act('accounts', { domain: 'example.com' }));
   await flush();
   click(act('accountedit', { address: 'anna@example.com' }));
   await flush();
   check('a key the domain\'s recovery key does not open is said so on the account card',
      document.getElementById('mkeyStale') !== null &&
      document.getElementById('mkeyStale').textContent.indexOf('does not open this mailbox') >= 0,
      document.getElementById('mkeyStale') ? document.getElementById('mkeyStale').textContent : 'no note');
   state.mailboxKeys['anna@example.com'].recovery_current = true;
   click(act('accounts', { domain: 'example.com' }));
   await flush();

   // ---- renaming a mailbox, and moving one to another domain
   moveBlockedReason = 'A POP3 session is holding the mailbox of anna@example.com.';
   before = requests.length;
   click(act('accountedit', { address: 'anna@example.com' }));
   await flush();
   check('the editor asks whether the mailbox could be moved now', called(before, 'GET', '/api/v1/accounts/anna%40example.com/move').length === 1, paths(before));
   check('a mailbox in use says why and the button is disabled',
      content().textContent.indexOf('cannot be moved at the moment') >= 0 &&
      content().textContent.indexOf('A POP3 session is holding the mailbox') >= 0 &&
      act('accountmove').attributes.disabled !== undefined,
      content().textContent.slice(0, 200));
   moveBlockedReason = null;
   click(act('accounts', { domain: 'example.com' }));
   await flush();
   click(act('accountedit', { address: 'anna@example.com' }));
   await flush();
   check('with nothing holding it the card offers a new address, the alias switch on, and an enabled button',
      document.getElementById('moveAddr') !== null && document.getElementById('moveAlias').checked === true &&
      act('accountmove').attributes.disabled === undefined);
   before = requests.length;
   click(act('accountmove'));
   await flush();
   check('an empty address is refused by the page', called(before, 'POST', /move/).length === 0 &&
      document.getElementById('err_accountmove').textContent === 'A new address is required.',
      document.getElementById('err_accountmove').textContent);
   setValue('moveAddr', 'anna@nowhere.example');
   before = requests.length;
   click(act('accountmove'));
   await flush();
   check('a domain the server does not hold is shown in the server\'s own words',
      document.getElementById('err_accountmove').textContent === 'This server does not hold the domain nowhere.example.',
      document.getElementById('err_accountmove').textContent);
   setValue('moveAddr', 'anna@second.example');
   setChecked('moveAlias', false);
   before = requests.length;
   click(act('accountmove'));
   await flush();
   put = lastBody(before, 'POST', '/api/v1/accounts/anna%40example.com/move');
   check('the move sends the new address and the alias switch', JSON.stringify(put) === '{"new_address":"anna@second.example","keep_alias":false}', JSON.stringify(put));
   check('and the page goes to the accounts of the domain the mailbox is in now',
      called(before, 'GET', '/api/v1/domains/second.example/accounts').length === 1 &&
      toastText() === 'Moved anna@example.com to anna@second.example' &&
      rows().some((r) => r.textContent.indexOf('anna@second.example') >= 0), toastText());
   before = requests.length;
   click(act('accountedit', { address: 'anna@second.example', domain: 'second.example' }));
   await flush();
   check('the moved mailbox now says when it was last moved and from where',
      content().textContent.indexOf('Last moved from anna@example.com to anna@second.example') >= 0,
      content().textContent.slice(0, 300));
   // Moved back, so that the rest of this walk finds the mailbox where it left
   // it - and so that moving one twice is exercised.
   setValue('moveAddr', 'anna@example.com');
   setChecked('moveAlias', false);
   before = requests.length;
   click(act('accountmove'));
   await flush();
   put = lastBody(before, 'POST', '/api/v1/accounts/anna%40second.example/move');
   check('a mailbox moves back the same way', JSON.stringify(put) === '{"new_address":"anna@example.com","keep_alias":false}' &&
      called(before, 'GET', '/api/v1/domains/example.com/accounts').length === 1, JSON.stringify(put));
   click(act('domains'));
   await flush();
   click(act('accounts', { domain: 'example.com' }));
   await flush();

   // ---- the distribution lists of a domain
   click(act('domains'));
   await flush();
   before = requests.length;
   click(act('lists', { domain: 'example.com' }));
   await flush();
   check('Lists reads the domain\'s lists', called(before, 'GET', '/api/v1/domains/example.com/lists').length === 1, paths(before));
   check('a list row shows its address, state, who may send and its members', rows().length === 1 && rows()[0].textContent.indexOf('all@example.com') >= 0 && rows()[0].textContent.indexOf('Active') >= 0 &&
      rows()[0].textContent.indexOf('Public') >= 0 && rows()[0].textContent.indexOf('2') >= 0 && rows()[0].textContent.indexOf('anna@example.com') >= 0, rows().length ? rows()[0].textContent : 'no rows');
   const drawnListNew = content().querySelectorAll('input, select, textarea').map((el) => el.id).filter((id) => id.indexOf('listnew_') === 0).map((id) => id.slice(8)).sort();
   check('the new-list form has every key of the create and nothing else, the members as a list', JSON.stringify(drawnListNew) === JSON.stringify(Object.keys(LIST_CREATE_PROPS).sort()) &&
      document.getElementById('listnew_members').tagName === 'TEXTAREA', JSON.stringify(drawnListNew));
   check('and starts at the defaults the description states, the mode\'s word picked out of its sentence', document.getElementById('listnew_active').checked === true && document.getElementById('listnew_require_auth').checked === false &&
      document.getElementById('listnew_mode').value === 'public' && document.getElementById('listnew_mode').querySelectorAll('option').map((o) => o.attributes.value).join(',') === 'public,membership,announcement,domain_members' &&
      document.getElementById('listnew_address').closest('.fr').textContent.indexOf('required') >= 0,
      'mode=' + document.getElementById('listnew_mode').value);
   before = requests.length;
   click(act('listnew', { domain: 'example.com' }));
   await flush();
   check('an empty address is refused by the page', called(before, 'POST', /lists/).length === 0 && document.getElementById('err_listnew').textContent.indexOf('required') >= 0);
   setValue('listnew_address', 'team@example.com');
   setValue('listnew_members', 'anna@example.com\n\ncarla@example.com\n');
   setValue('listnew_mode', 'membership');
   setChecked('listnew_require_auth', true);
   before = requests.length;
   click(act('listnew', { domain: 'example.com' }));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains/example.com/lists');
   check('creating a list posts the form with the members as an array and the defaults', !!posted && posted.address === 'team@example.com' && JSON.stringify(posted.members) === '["anna@example.com","carla@example.com"]' &&
      posted.mode === 'membership' && posted.require_auth === true && posted.active === true && posted.moderator_address === '', JSON.stringify(posted));
   check('and the re-read list has it', called(before, 'GET', '/api/v1/domains/example.com/lists').length === 1 && rows().length === 2 && rows()[1].textContent.indexOf('team@example.com') >= 0 && toastText() === 'List created: team@example.com');
   nextRefusal = 'The domain has reached its list limit.';
   setValue('listnew_address', 'more@example.com');
   click(act('listnew', { domain: 'example.com' }));
   await flush();
   check('a refused create is the server\'s sentence beside the form', document.getElementById('err_listnew').textContent === 'The domain has reached its list limit.');
   // A list that defines itself: the membership query is a field of the create
   // like any other, and the row says what the query is rather than leaving the
   // reader to open the list to find out.
   check('the membership query is a field of the new-list form, with its forms spelled out',
      document.getElementById('listnew_membership') !== null &&
      document.getElementById('listnew_membership').closest('.fr').textContent.indexOf('pattern:sales-*@example.com') >= 0,
      document.getElementById('listnew_membership') ? document.getElementById('listnew_membership').closest('.fr').textContent.slice(0, 200) : 'no control');
   check('a static list says so in its row rather than showing an empty cell',
      rows()[0].textContent.indexOf('Its addresses alone') >= 0, rows()[0].textContent);
   setValue('listnew_address', 'staff@example.com');
   setValue('listnew_members', '');
   setValue('listnew_mode', 'public');
   setChecked('listnew_require_auth', false);
   setValue('listnew_membership', 'domain');
   before = requests.length;
   click(act('listnew', { domain: 'example.com' }));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains/example.com/lists');
   check('creating a query list posts the membership with the rest of the form',
      !!posted && posted.address === 'staff@example.com' && posted.membership === 'domain' && JSON.stringify(posted.members) === '[]', JSON.stringify(posted));
   check('and the query is what its row shows', rows().length === 3 && rows()[2].textContent.indexOf('domain') >= 0, rows().length ? rows()[2].textContent : 'no row');
   before = requests.length;
   click(act('listedit', { address: 'staff@example.com', domain: 'example.com' }));
   await flush();
   check('opening a list asks who it delivers to now', called(before, 'GET', '/api/v1/lists/staff%40example.com/members').length === 1, paths(before));
   check('and the answer is the active accounts the query finds, marked as the query\'s and not as rows',
      content().textContent.indexOf('Delivers to now') >= 0 && content().textContent.indexOf('anna@example.com') >= 0 &&
      content().textContent.indexOf('From the query') >= 0 && content().textContent.indexOf('bob@example.com') < 0,
      content().textContent.slice(content().textContent.indexOf('Delivers to now'), content().textContent.indexOf('Delivers to now') + 200));
   setValue('list_membership', '');
   before = requests.length;
   click(act('listsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/lists/staff%40example.com');
   check('emptying the membership sends it, which is what turns a query list back into a static one',
      JSON.stringify(put) === '{"membership":""}', JSON.stringify(put));
   check('and the list then delivers to nobody, which the page says in as many words',
      content().textContent.indexOf('Nobody: this list would deliver to no one.') >= 0,
      content().textContent.slice(0, 400));
   // A query the caller may not read the answer to: the members route refuses
   // it (403), and the editor still has to open - the answer is an extra the
   // page shows when it can get it, never the thing it is for.
   setValue('list_membership', 'everyone');
   click(act('listsave'));
   await flush();
   click(act('lists', { domain: 'example.com' }));
   await flush();
   click(act('listedit', { address: 'staff@example.com', domain: 'example.com' }));
   await flush();
   check('a members route that refuses the caller still leaves the list editable',
      document.getElementById('list_membership') !== null && document.getElementById('list_membership').value === 'everyone',
      document.getElementById('list_membership') ? document.getElementById('list_membership').value : 'no control');
   check('and the page shows no answer at all rather than an empty one it would read as the membership',
      content().textContent.indexOf('Delivers to now') < 0, content().textContent.slice(0, 400));
   click(act('lists', { domain: 'example.com' }));
   await flush();
   confirmAnswer = true;
   click(act('listdel', { address: 'staff@example.com', domain: 'example.com' }));
   await flush();
   click(act('listedit', { address: 'all@example.com' }));
   await flush();
   const drawnList = content().querySelectorAll('input, select, textarea').map((el) => el.id).filter((id) => id.indexOf('list_') === 0).map((id) => id.slice(5)).sort();
   check('editing draws the update\'s keys with the list\'s values and the members without a control', JSON.stringify(drawnList) === JSON.stringify(Object.keys(LIST_PROPS).sort()) &&
      document.getElementById('list_mode').value === 'public' && document.getElementById('list_active').checked === true && content().textContent.indexOf('bob@example.com') >= 0 &&
      content().textContent.indexOf('the update does not change them') >= 0, JSON.stringify(drawnList));
   setValue('list_mode', 'announcement');
   setValue('list_require_sender_address', 'news@example.com');
   before = requests.length;
   click(act('listsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/lists/all%40example.com');
   check('saving sends only what changed, by address', JSON.stringify(put) === '{"mode":"announcement","require_sender_address":"news@example.com"}', JSON.stringify(put));
   check('then reads the lists again and stays in the editor on the server\'s values', called(before, 'GET', '/api/v1/domains/example.com/lists').length === 1 && document.getElementById('list_mode').value === 'announcement' && toastText() === 'List saved');
   nextRefusal = 'An announcement list needs a sender.';
   setValue('list_require_sender_address', '');
   click(act('listsave'));
   await flush();
   check('a refused save keeps the editor open with the server\'s sentence', document.getElementById('err_listedit').textContent === 'An announcement list needs a sender.' && document.getElementById('list_mode') !== null);
   click(act('lists', { domain: 'example.com' }));
   await flush();
   confirmAnswer = false;
   before = requests.length;
   click(act('listdel', { address: 'team@example.com' }));
   await flush();
   check('deleting a list asks, naming it', called(before, 'DELETE', /lists/).length === 0 && confirmations[confirmations.length - 1].indexOf('team@example.com') >= 0);
   confirmAnswer = true;
   click(act('listdel', { address: 'team@example.com' }));
   await flush();
   check('yes deletes by address and the re-read list is without it', called(before, 'DELETE', '/api/v1/lists/team%40example.com').length === 1 && rows().length === 1 && rows()[0].textContent.indexOf('all@example.com') >= 0);

   // A managed list (docs/MailingLists.md): each row says how the member takes
   // its mail, or that it is a request still waiting, and what its bounces did;
   // the owner's two verbs sit beside the rows that need them and nowhere else.
   state.lists['example.com'].push({ address: 'club@example.com', active: true, require_auth: false, mode: 'public', require_sender_address: '', moderator_address: '', bounce_address: '', membership: '', managed: true,
      members: ['ann@example.com', 'bea@example.com', 'cy@example.com', 'dee@example.com'],
      rowDetail: {
         'ann@example.com': { delivery: 'each', confirmed: 1, state: 'member', hard_bounces: 0, soft_bounces: 0, reason: '' },
         'bea@example.com': { delivery: 'none', confirmed: 1, state: 'member', hard_bounces: 1, soft_bounces: 0, reason: '1 hard bounce(s), the last with status 5.1.1.' },
         'cy@example.com': { delivery: 'each', confirmed: 2, state: 'awaiting_owner', hard_bounces: 0, soft_bounces: 0, reason: '' },
         'dee@example.com': { delivery: 'none', confirmed: 3, state: 'left', hard_bounces: 1, soft_bounces: 0, reason: '' } } });
   click(act('domains'));
   await flush();
   click(act('lists', { domain: 'example.com' }));
   await flush();
   click(act('listedit', { address: 'club@example.com', domain: 'example.com' }));
   await flush();
   const clubText = content().textContent;
   check('a managed list\'s rows say how each member takes its mail, which is still a request, and what the bounces did',
      clubText.indexOf('Each message') >= 0 && clubText.indexOf('Nothing for now') >= 0 && clubText.indexOf('Waiting for approval') >= 0 &&
      clubText.indexOf('1 hard, 0 soft bounces') >= 0 && clubText.indexOf('5.1.1') >= 0, clubText.slice(clubText.indexOf('Delivers to now'), clubText.indexOf('Delivers to now') + 400));
   check('an address the query answers that has left says so, and is offered no reset whatever its counts',
      clubText.indexOf('Left the list') >= 0 && content().querySelectorAll('button[data-act="listreset"][data-address="dee@example.com"]').length === 0, clubText.slice(clubText.indexOf('dee@example.com'), clubText.indexOf('dee@example.com') + 200));
   check('Approve is beside the request waiting for the owner and Reset bounces beside the member the bounces stopped, and neither anywhere else',
      content().querySelectorAll('button[data-act="listapprove"]').length === 1 && content().querySelectorAll('button[data-act="listreset"]').length === 1 &&
      !!act('listapprove', { address: 'cy@example.com' }) && !!act('listreset', { address: 'bea@example.com' }));
   before = requests.length;
   click(act('listapprove', { address: 'cy@example.com' }));
   await flush();
   check('Approve posts the approval of that member and reads who the list delivers to again',
      called(before, 'POST', '/api/v1/lists/club%40example.com/members/cy%40example.com/approve').length === 1 &&
      called(before, 'GET', '/api/v1/lists/club%40example.com/members').length === 1 && toastText() === 'Approved: cy@example.com', paths(before));
   before = requests.length;
   click(act('listreset', { address: 'bea@example.com' }));
   await flush();
   check('Reset bounces posts the reset of that member, and the row then offers nothing more to do',
      called(before, 'POST', '/api/v1/lists/club%40example.com/members/bea%40example.com/reset-bounces').length === 1 &&
      content().querySelectorAll('button[data-act="listreset"]').length === 0 && content().querySelectorAll('button[data-act="listapprove"]').length === 0, paths(before));
   click(act('lists', { domain: 'example.com' }));
   await flush();
   click(act('listedit', { address: 'all@example.com', domain: 'example.com' }));
   await flush();
   check('an unmanaged list\'s rows say nothing of delivery and offer neither verb',
      content().textContent.indexOf('Each message') < 0 && content().querySelectorAll('button[data-act="listreset"]').length === 0 &&
      content().querySelectorAll('button[data-act="listapprove"]').length === 0, content().textContent.slice(0, 300));
   click(act('lists', { domain: 'example.com' }));
   await flush();

   // ---- the suppression list of a domain
   click(act('domains'));
   await flush();
   check('each domain row carries a way to its suppression list', act('suppressions', { domain: 'example.com' }) !== null && act('suppressions', { domain: 'second.example' }) !== null);
   before = requests.length;
   click(act('suppressions', { domain: 'example.com' }));
   await flush();
   check('Suppressions reads the domain\'s list and its counts, a page at a time', called(before, 'GET', '/api/v1/domains/example.com/suppressions?limit=50&offset=0').length === 1 &&
      called(before, 'GET', '/api/v1/domains/example.com/bounces?limit=50').length === 1, paths(before));
   check('an entry shows its address, the remote\'s words, its category and status, where it came from and that it never expires',
      rows()[0].textContent.indexOf('gone@remote.test') >= 0 && rows()[0].textContent.indexOf('User unknown') >= 0 && rows()[0].textContent.indexOf('Hard') >= 0 &&
      rows()[0].textContent.indexOf('5.1.1') >= 0 && rows()[0].textContent.indexOf('A bounce') >= 0 && rows()[0].textContent.indexOf('mx.remote.test') >= 0 && rows()[0].textContent.indexOf('Never') >= 0,
      rows().length ? rows()[0].textContent : 'no rows');
   check('the counts are drawn beside the list, the run of hard bounces and the listed mark among them', rows().length === 2 && rows()[1].textContent.indexOf('gone@remote.test') >= 0 &&
      rows()[1].textContent.indexOf('Listed') >= 0 && rows()[1].querySelectorAll('td').length === 10, rows().length ? rows()[rows().length - 1].textContent : 'no rows');
   // Bounce processing is off in the recorded settings and complaint processing is
   // on, which is the case the feedback loops introduced: the list IS applied, and
   // the page must not go on saying it is not. Both wordings are checked, because
   // they share a first sentence and a substring test on that sentence alone would
   // pass whichever one was drawn.
   check('with bounce processing off but complaints on, the page says the entries are still applied, and says what the action is',
      content().textContent.indexOf('the entries below ARE still applied') >= 0 &&
      content().textContent.indexOf('and the entries below are not applied') < 0 &&
      content().textContent.indexOf('A listed recipient is refused, with the reason.') >= 0, content().textContent.slice(0, 400));
   SETTING_GROUPS['/api/v1/settings/complaints'].values.processing_enabled = false;
   click(act('domains'));
   await flush();
   click(act('suppressions', { domain: 'example.com' }));
   await flush();
   check('with both switches off, the page says the list is not being applied',
      content().textContent.indexOf('and the entries below are not applied') >= 0 &&
      content().textContent.indexOf('the entries below ARE still applied') < 0, content().textContent.slice(0, 400));
   SETTING_GROUPS['/api/v1/settings/complaints'].values.processing_enabled = true;
   click(act('domains'));
   await flush();
   click(act('suppressions', { domain: 'example.com' }));
   await flush();
   const drawnSuppNew = content().querySelectorAll('input, select, textarea').map((el) => el.id).filter((id) => id.indexOf('suppnew_') === 0).map((id) => id.slice(8)).sort();
   check('the add form has every key of the create, the expiry at the default its description names', JSON.stringify(drawnSuppNew) === '["address","expires_days","note"]' &&
      document.getElementById('suppnew_expires_days').value === '0' && document.getElementById('suppnew_address').closest('.fr').textContent.indexOf('required') >= 0, JSON.stringify(drawnSuppNew));
   before = requests.length;
   click(act('suppnew'));
   await flush();
   check('an empty address is refused by the page', called(before, 'POST', /suppressions/).length === 0 && document.getElementById('err_suppnew').textContent === 'An address is required.');
   setValue('suppnew_address', 'dead@remote.test');
   setValue('suppnew_note', 'Bounced from the old CRM export');
   before = requests.length;
   click(act('suppnew'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains/example.com/suppressions');
   check('adding posts the address, the note and the expiry', !!posted && posted.address === 'dead@remote.test' && posted.note === 'Bounced from the old CRM export' && posted.expires_days === 0, JSON.stringify(posted));
   check('and the re-read list has it first, made by an administrator', called(before, 'GET', /\/api\/v1\/domains\/example\.com\/suppressions\?/).length === 1 &&
      rows()[0].textContent.indexOf('dead@remote.test') >= 0 && rows()[0].textContent.indexOf('An administrator') >= 0 && toastText() === 'Added to the suppression list: dead@remote.test', toastText());
   setValue('suppnew_address', 'anna@example.com');
   click(act('suppnew'));
   await flush();
   check('a hosted address is the server\'s refusal beside the form', document.getElementById('err_suppnew').textContent.indexOf('hosted on this server') >= 0, document.getElementById('err_suppnew').textContent);
   setValue('suppSearch', 'dead');
   before = requests.length;
   click(act('suppsearch'));
   await flush();
   check('searching asks the server for the substring and draws what it answers', called(before, 'GET', '/api/v1/domains/example.com/suppressions?limit=50&offset=0&search=dead').length === 1 &&
      called(before, 'GET', '/api/v1/domains/example.com/bounces?limit=50&search=dead').length === 1 && document.getElementById('suppSearch').value === 'dead', paths(before));
   confirmAnswer = false;
   before = requests.length;
   click(act('suppdel', { address: 'dead@remote.test' }));
   await flush();
   check('removing an entry asks first, naming the address and what happens next', called(before, 'DELETE', /suppressions/).length === 0 &&
      confirmations[confirmations.length - 1].indexOf('dead@remote.test') >= 0 && confirmations[confirmations.length - 1].indexOf('will be sent') >= 0);
   confirmAnswer = true;
   before = requests.length;
   click(act('suppdel', { address: 'dead@remote.test' }));
   await flush();
   check('yes deletes it by address and the list is read again without it', called(before, 'DELETE', '/api/v1/domains/example.com/suppressions/dead%40remote.test').length === 1 &&
      toastText() === 'Removed from the suppression list: dead@remote.test' && content().textContent.indexOf('dead@remote.test') < 0, paths(before));
   nextRefusal = 'The entry could not be removed.';
   click(act('suppsearch'));
   await flush();
   setValue('suppSearch', '');
   click(act('suppsearch'));
   await flush();
   click(act('suppdel', { address: 'gone@remote.test' }));
   await flush();
   check('a refused removal is the server\'s sentence above the table', document.getElementById('err_supp').textContent === 'The entry could not be removed.', document.getElementById('err_supp').textContent);
   // ---- the complaints of a domain (the feedback loops)
   click(act('domains'));
   await flush();
   check('each domain row carries a way to its complaints', act('complaints', { domain: 'example.com' }) !== null);
   before = requests.length;
   click(act('complaints', { domain: 'example.com' }));
   await flush();
   check('Complaints reads the domain\'s reports a page at a time', called(before, 'GET', '/api/v1/domains/example.com/complaints?limit=50&offset=0').length === 1, paths(before));
   check('the rate is shown with its denominator, not on its own', content().textContent.indexOf('250 in a million') >= 0 && content().textContent.indexOf('4000 message(s)') >= 0, content().textContent.slice(0, 400));
   check('a complaint is drawn with who complained, the type, the sender and the provider', rows().length === 2 &&
      rows()[0].textContent.indexOf('reader@provider.test') >= 0 && rows()[0].textContent.indexOf('Abuse') >= 0 &&
      rows()[0].textContent.indexOf('anna@example.com') >= 0 && rows()[0].textContent.indexOf('SomeGenerator/1.0') >= 0, rows().map((r) => r.textContent).join(' | '));
   check('a redacted reader reads as Not named, and a not-spam report says it counts towards nothing',
      rows()[1].textContent.indexOf('Not named') >= 0 && rows()[1].textContent.indexOf('counts towards nothing') >= 0, rows()[1].textContent);
   before = requests.length;
   setValue('cmpSearch', 'reader');
   click(act('cmpsearch'));
   await flush();
   check('searching asks the server for the substring and draws what it answers',
      called(before, 'GET', '/api/v1/domains/example.com/complaints?limit=50&offset=0&search=reader').length === 1 && rows().length === 1, paths(before));

   // ---- the reports a domain receives (DMARC aggregate and TLS-RPT)
   click(act('domains'));
   await flush();
   check('each domain row carries a way to its DMARC reports', act('dmarc', { domain: 'example.com' }) !== null && act('dmarc', { domain: 'second.example' }) !== null);
   before = requests.length;
   click(act('dmarc', { domain: 'example.com' }));
   await flush();
   check('with no report mailbox named the page says so rather than drawing an empty chart',
      called(before, 'GET', '/api/v1/domains/example.com/dmarc-reports/summary?days=30').length === 1 &&
      content().textContent.indexOf('No report mailbox is named for this domain') >= 0 && content().querySelectorAll('svg').length === 0, content().textContent.slice(0, 400));
   state.domains[0].report_mailbox = 'dmarc@example.com';
   click(act('domains'));
   await flush();
   before = requests.length;
   click(act('dmarc', { domain: 'example.com' }));
   await flush();
   check('DMARC reads the summary, the reports a page at a time and the TLS reports over the same window',
      called(before, 'GET', '/api/v1/domains/example.com/dmarc-reports/summary?days=30').length === 1 &&
      called(before, 'GET', '/api/v1/domains/example.com/dmarc-reports?limit=25&offset=0').length === 1 &&
      called(before, 'GET', '/api/v1/domains/example.com/tls-reports?days=30&limit=25&offset=0').length === 1, paths(before));
   check('the mailbox and the records to publish are named, and the chart is drawn passed against failed',
      content().textContent.indexOf('Reports arrive at dmarc@example.com') >= 0 && content().textContent.indexOf('rua=mailto:dmarc@example.com') >= 0 &&
      content().querySelectorAll('svg').length === 1 && content().querySelectorAll('svg rect').length === 60, content().textContent.slice(0, 400));
   check('the totals are the summary\'s, with the pass rate beside the count',
      content().textContent.indexOf('12 (80%)') >= 0 && content().textContent.indexOf('No mailbox') < 0, content().textContent.slice(0, 600));
   check('a source is drawn with its address, who it sends as, its pass rate and what the receivers did',
      Array.from(rows()).some((r) => r.textContent.indexOf('192.0.2.10') >= 0 && r.textContent.indexOf('100%') >= 0) &&
      Array.from(rows()).some((r) => r.textContent.indexOf('198.51.100.7') >= 0 && r.textContent.indexOf('0%') >= 0), rows().map((r) => r.textContent).join(' | '));
   check('the reporter and the report are listed, the report with the policy the reporter saw and a way into its records',
      content().textContent.indexOf('google.com') >= 0 && content().textContent.indexOf('p=quarantine sp=reject adkim=r aspf=r') >= 0 && act('dmarcreport', { id: '31' }) !== null, content().textContent.slice(0, 800));
   check('the reporter\'s row says how many of its reports arrived in a message that passed DMARC, and the report says what carried it',
      Array.from(rows()).some((r) => r.textContent.indexOf('google.com') >= 0 && r.textContent.indexOf('1 / 1') >= 0) &&
      content().textContent.indexOf('Arrived from google.com') >= 0 && Array.from(content().querySelectorAll('.badge.good')).some((b) => b.textContent === 'pass'),
      rows().map((r) => r.textContent).join(' | '));
   state.dmarc['example.com'].reports[0].message_dmarc = '';
   click(act('dmarcrange', { days: '30' }));
   await flush();
   check('a report whose carrying message this server did not evaluate says nothing about it, and the reporter is Not checked',
      content().textContent.indexOf('Arrived from') < 0 && Array.from(rows()).some((r) => r.textContent.indexOf('google.com') >= 0 && r.textContent.indexOf('Not checked') >= 0),
      rows().map((r) => r.textContent).join(' | '));
   state.dmarc['example.com'].reports[0].message_dmarc = 'pass';
   check('the TLS reports say how many sessions succeeded and failed, and each failure by its type',
      content().textContent.indexOf('5326 session(s) to this domain succeeded and 303 failed') >= 0 && content().textContent.indexOf('certificate-expired ×100 (mx1.mail.example.com)') >= 0, content().textContent.slice(-600));
   before = requests.length;
   click(act('dmarcrange', { days: '90' }));
   await flush();
   check('choosing a window asks the server for it', called(before, 'GET', '/api/v1/domains/example.com/dmarc-reports/summary?days=90').length === 1 &&
      called(before, 'GET', '/api/v1/domains/example.com/tls-reports?days=90&limit=25&offset=0').length === 1, paths(before));
   before = requests.length;
   click(act('dmarcreport', { id: '31' }));
   await flush();
   check('Records reads the one report and draws its records with the aligned verdicts and the raw authentication results',
      called(before, 'GET', '/api/v1/domains/example.com/dmarc-reports/31').length === 1 && rows().length === 2 &&
      rows()[0].textContent.indexOf('192.0.2.10') >= 0 && rows()[0].textContent.indexOf('d=example.com s=s1 result=pass') >= 0 &&
      rows()[1].textContent.indexOf('quarantine') >= 0 && rows()[1].textContent.indexOf('fail') >= 0, rows().map((r) => r.textContent).join(' | '));
   before = requests.length;
   click(act('dmarcback'));
   await flush();
   check('Back returns to the reports at the window that was chosen', called(before, 'GET', '/api/v1/domains/example.com/dmarc-reports/summary?days=90').length === 1, paths(before));
   state.domains[0].report_mailbox = '';


   // ---- the OpenPGP key directory of a domain
   click(act('domains'));
   await flush();
   check('each domain row carries a way to its OpenPGP keys', act('openpgp', { domain: 'example.com' }) !== null && act('openpgp', { domain: 'second.example' }) !== null);
   before = requests.length;
   click(act('openpgp', { domain: 'example.com' }));
   await flush();
   check('OpenPGP reads the keys held for the domain', called(before, 'GET', '/api/v1/domains/example.com/openpgp').length === 1, paths(before));
   check('a key shows whose it is, its user id, its fingerprint, its algorithm and where a client looks',
      rows().length === 1 && rows()[0].textContent.indexOf('anna@example.com') >= 0 && rows()[0].textContent.indexOf('Anna <anna@example.com>') >= 0 &&
      rows()[0].textContent.indexOf('c29b6cce361e73ba13724cb8bbee452b9b0a0bb7') >= 0 && rows()[0].textContent.indexOf('EdDSA 256') >= 0 &&
      rows()[0].textContent.indexOf('/.well-known/openpgpkey/example.com/hu/') >= 0 && rows()[0].textContent.indexOf('Never') >= 0,
      rows().length ? rows()[0].textContent : 'no rows');
   check('the domain publishes nothing, so the key is marked not published and the page says which switch is in the way',
      rows()[0].textContent.indexOf('Not published') >= 0 && content().textContent.indexOf('This domain does not publish keys') >= 0, content().textContent.slice(0, 300));
   const drawnPgp = content().querySelectorAll('input, select, textarea').map((el) => el.id).filter((id) => id.indexOf('pgpnew_') === 0).map((id) => id.slice(7)).sort();
   check('the upload form has every key the route takes, and nothing else', JSON.stringify(drawnPgp) === '["allow_address_mismatch","key","publish"]', JSON.stringify(drawnPgp));
   before = requests.length;
   click(act('pgpnew'));
   await flush();
   check('an upload with no address is refused by the page', called(before, 'PUT', /openpgp/).length === 0 &&
      document.getElementById('err_pgpnew').textContent === 'An account address is required.');
   setValue('pgpAddress', 'bob@example.com');
   before = requests.length;
   click(act('pgpnew'));
   await flush();
   check('and an upload with no key is refused by the page as well', called(before, 'PUT', /openpgp/).length === 0 &&
      document.getElementById('err_pgpnew').textContent === 'A key is required.');
   setValue('pgpnew_key', '-----BEGIN PGP PRIVATE KEY BLOCK-----\n\nAAAA\n-----END PGP PRIVATE KEY BLOCK-----');
   before = requests.length;
   click(act('pgpnew'));
   await flush();
   check('a SECRET key is refused by the server and its sentence is beside the form',
      document.getElementById('err_pgpnew').textContent.indexOf('SECRET key') >= 0, document.getElementById('err_pgpnew').textContent);
   setValue('pgpnew_key', '-----BEGIN PGP PUBLIC KEY BLOCK-----\n\nAAAA\n-----END PGP PUBLIC KEY BLOCK-----');
   setChecked('pgpnew_publish', true);
   before = requests.length;
   click(act('pgpnew'));
   await flush();
   posted = lastBody(before, 'PUT', '/api/v1/accounts/bob%40example.com/openpgp');
   check('storing sends the key to the account named in the path, with publish', !!posted && posted.key.indexOf('PUBLIC KEY BLOCK') >= 0 && posted.publish === true, JSON.stringify(posted));
   check('and the listing is read again with the new key in it', called(before, 'GET', '/api/v1/domains/example.com/openpgp').length === 1 &&
      rows().length === 2 && content().textContent.indexOf('bob@example.com') >= 0 && toastText() === 'Stored an OpenPGP key for bob@example.com', toastText());
   confirmAnswer = false;
   before = requests.length;
   click(act('pgpdel', { address: 'bob@example.com', fingerprint: '800b26ccc36e0e3b4a61a26b19fe7a2fe905df20' }));
   await flush();
   check('removing a key asks first, naming the address and what stops working', called(before, 'DELETE', /openpgp/).length === 0 &&
      confirmations[confirmations.length - 1].indexOf('bob@example.com') >= 0 && confirmations[confirmations.length - 1].indexOf('stop finding it') >= 0);
   confirmAnswer = true;
   before = requests.length;
   click(act('pgpdel', { address: 'bob@example.com', fingerprint: '800b26ccc36e0e3b4a61a26b19fe7a2fe905df20' }));
   await flush();
   check('yes removes it by fingerprint through the account and the listing is read again without it',
      called(before, 'DELETE', '/api/v1/accounts/bob%40example.com/openpgp/800b26ccc36e0e3b4a61a26b19fe7a2fe905df20').length === 1 &&
      rows().length === 1 && content().textContent.indexOf('bob@example.com') < 0 &&
      toastText() === 'Removed the OpenPGP key of bob@example.com', toastText());

   // ---- the aliases of a domain
   click(act('domains'));
   await flush();
   before = requests.length;
   click(act('aliases', { domain: 'example.com' }));
   await flush();
   check('Aliases reads the domain\'s aliases', called(before, 'GET', '/api/v1/domains/example.com/aliases').length === 1 && rows().length === 1 &&
      rows()[0].textContent.indexOf('info@example.com') >= 0 && rows()[0].textContent.indexOf('anna@example.com') >= 0 && rows()[0].textContent.indexOf('Active') >= 0, paths(before));
   check('the form has the create\'s three keys, the two required ones marked, active on by the description', document.getElementById('aliasnew_name') !== null && document.getElementById('aliasnew_value') !== null &&
      document.getElementById('aliasnew_active').checked === true && document.getElementById('aliasnew_name').closest('.fr').textContent.indexOf('required') >= 0 &&
      document.getElementById('aliasnew_active').closest('.fr').textContent.indexOf('required') < 0 && content().textContent.indexOf('delete it and make it again') >= 0);
   before = requests.length;
   click(act('aliasnew', { domain: 'example.com' }));
   await flush();
   check('an empty form is refused by the page', called(before, 'POST', /aliases/).length === 0 && document.getElementById('err_aliasnew').textContent.indexOf('required') >= 0);
   setValue('aliasnew_name', 'sales@example.com');
   setValue('aliasnew_value', 'carla@example.com');
   setChecked('aliasnew_active', false);
   before = requests.length;
   document.getElementById('aliasnew_value').dispatchEvent(makeEvent('keydown', { key: 'Enter' }));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains/example.com/aliases');
   check('Enter in the form creates the alias with the three keys', JSON.stringify(posted) === '{"name":"sales@example.com","value":"carla@example.com","active":false}', JSON.stringify(posted));
   check('and the re-read list has it, disabled', called(before, 'GET', '/api/v1/domains/example.com/aliases').length === 1 && rows().length === 2 && rows()[1].textContent.indexOf('sales@example.com') >= 0 &&
      rows()[1].textContent.indexOf('Disabled') >= 0 && toastText() === 'Alias created: sales@example.com');
   nextRefusal = 'The name is already an account.';
   setValue('aliasnew_name', 'anna@example.com');
   setValue('aliasnew_value', 'carla@example.com');
   click(act('aliasnew', { domain: 'example.com' }));
   await flush();
   check('a refused alias is the server\'s sentence beside the form', document.getElementById('err_aliasnew').textContent === 'The name is already an account.');
   confirmAnswer = false;
   before = requests.length;
   click(act('aliasdel', { name: 'sales@example.com' }));
   await flush();
   check('deleting an alias asks, naming it', called(before, 'DELETE', /aliases/).length === 0 && confirmations[confirmations.length - 1].indexOf('sales@example.com') >= 0);
   confirmAnswer = true;
   click(act('aliasdel', { name: 'sales@example.com' }));
   await flush();
   check('yes deletes by address and the re-read list is without it', called(before, 'DELETE', '/api/v1/aliases/sales%40example.com').length === 1 && rows().length === 1);
   nextRefusal = 'The alias is named by a rule.';
   click(act('aliasdel', { name: 'info@example.com' }));
   await flush();
   check('a refused delete is shown on its row', document.getElementById('err_alias_info@example.com').textContent === 'The alias is named by a rule.' && rows().length === 1);

   // ---- the domain editor
   click(act('domains'));
   await flush();
   check('the domain list shows the postmaster and offers Edit and Delete', rows()[0].textContent.indexOf('postmaster@example.com') >= 0 &&
      act('domainedit', { name: 'example.com' }) !== null && act('domaindel', { name: HOSTILE }) !== null);
   before = requests.length;
   click(act('domainedit', { name: 'example.com' }));
   await flush();
   check('Edit reads the domain\'s other names', called(before, 'GET', '/api/v1/domains/example.com/domain-aliases').length === 1, paths(before));
   check('and its own allow and block list', called(before, 'GET', '/api/v1/domains/example.com/sender-list').length === 1 && document.getElementById('domainSenders') !== null &&
      document.getElementById('domainSenders').textContent.indexOf('partner.example') >= 0 && document.getElementById('domainSenders').textContent.indexOf('Allowed') >= 0, paths(before));
   const headings = content().querySelectorAll('h2').map((h) => h.textContent.trim());
   check('the editor is grouped as the desktop dialog is', ['General', 'Names', 'Limits', 'Signature', 'Relay', 'Out of office', 'DKIM'].every((g) => headings.indexOf(g) >= 0), JSON.stringify(headings));
   check('the route\'s own description is the note', content().textContent.indexOf('A field left out keeps its value') >= 0);
   check('General: active, the name and the postmaster', document.getElementById('dom_active').checked === true && document.getElementById('dom_name').value === 'example.com' &&
      document.getElementById('dom_postmaster').value === 'postmaster@example.com');
   check('Limits: numbers as number boxes, switches as checkboxes', document.getElementById('dom_max_size_mb').type === 'number' && document.getElementById('dom_max_size_mb').value === '0' &&
      document.getElementById('dom_max_accounts_enabled').checked === false && document.getElementById('dom_plus_addressing_character').value === '+');
   check('Signature: the method\'s three words and the texts as text areas',
      document.getElementById('dom_signature_method').querySelectorAll('option').map((o) => o.attributes.value).join(',') === 'set_if_not_specified,overwrite,append' &&
      document.getElementById('dom_signature_plain_text').tagName === 'TEXTAREA' && document.getElementById('dom_signature_html').tagName === 'TEXTAREA');
   check('Relay: the four securities and a write-only password box, empty',
      document.getElementById('dom_relay_connection_security').querySelectorAll('option').map((o) => o.attributes.value).join(',') === 'none,starttls_optional,starttls_required,tls' &&
      document.getElementById('dom_relay_password').type === 'password' && document.getElementById('dom_relay_password').value === '' &&
      document.getElementById('dom_relay_password').closest('.fr').textContent.indexOf('write-only') >= 0);
   check('Out of office: the policy beside the text - a text for colleagues, the override for strangers and the window in days',
      document.getElementById('dom_vacation_internal_message').tagName === 'TEXTAREA' && document.getElementById('dom_vacation_external_override').type === 'checkbox' &&
      document.getElementById('dom_vacation_days').type === 'number' && document.getElementById('dom_vacation_days').value === '0');
   check('Out of office and DKIM', document.getElementById('dom_vacation_message').tagName === 'TEXTAREA' &&
      document.getElementById('dom_dkim_signing_algorithm').querySelectorAll('option').map((o) => o.attributes.value).join(',') === 'sha1,sha256' && document.getElementById('dom_dkim_signing_algorithm').value === 'sha256');
   check('Restricted delivery: the named domains as a text area under its own heading, empty for a domain that restricts nothing, with the sentence that says what a list does',
      headings.indexOf('Restricted delivery') >= 0 && document.getElementById('dom_restricted_domains').tagName === 'TEXTAREA' && document.getElementById('dom_restricted_domains').value === '' &&
      document.getElementById('dom_restricted_domains').closest('.fr').textContent.indexOf('an outside sender not in a named domain is refused at RCPT TO') >= 0,
      document.getElementById('dom_restricted_domains') ? document.getElementById('dom_restricted_domains').outerHTML : 'no control');
   check('Impersonation protection: the action as a number that is 0 for a domain that checks nothing, the two lists as text areas, under its own heading, with the sentence that says what the two checks are',
      headings.indexOf('Impersonation protection') >= 0 && document.getElementById('dom_impersonation_action').type === 'number' && document.getElementById('dom_impersonation_action').value === '0' &&
      document.getElementById('dom_protected_persons').tagName === 'TEXTAREA' && document.getElementById('dom_protected_persons').value === '' &&
      document.getElementById('dom_protected_partners').tagName === 'TEXTAREA' && document.getElementById('dom_protected_partners').value === '' &&
      document.getElementById('dom_impersonation_action').closest('.fr').textContent.indexOf('one edit or one look-alike character') >= 0,
      document.getElementById('dom_impersonation_action') ? document.getElementById('dom_impersonation_action').outerHTML : 'no control');
   const drawn = content().querySelectorAll('input, select, textarea').map((el) => el.id).filter((id) => id.indexOf('dom_') === 0).map((id) => id.slice(4)).sort();
   check('every key the route takes has a control, and no key it does not', JSON.stringify(drawn) === JSON.stringify(DOMAIN_KEYS.slice().sort()), JSON.stringify(drawn));
   check('the other names are listed with a Remove each', content().textContent.indexOf('example.net') >= 0 && act('daliasdel', { name: 'example.net' }) !== null);

   // ---- the DKIM key rotation card
   const openedAt = before;
   const rotationBox = () => document.getElementById('dkimRotation');
   check('Edit reads the domain\'s key rotation', called(before, 'GET', '/api/v1/domains/example.com/dkim/rotation').length === 1, paths(before));
   check('the rotation card is under its own heading and says the phase', headings.indexOf('DKIM key rotation') >= 0 && rotationBox() !== null &&
      rotationBox().textContent.indexOf('Phase: Idle') >= 0, rotationBox() ? rotationBox().textContent : 'no card');
   check('the current and retiring selectors are drawn with the records to keep', rotationBox().textContent.indexOf('jan2026._domainkey.example.com') >= 0 &&
      rotationBox().textContent.indexOf('old2025._domainkey.example.com') >= 0 && rotationBox().textContent.indexOf('Keep') >= 0);
   const pills = rotationBox().querySelectorAll('.pill');
   check('each check is a pill with its level, a mark and a word', pills.length === 2 && pills[0].attributes['data-level'] === 'good' && pills[0].textContent.indexOf('Matches') >= 0 &&
      pills[1].attributes['data-level'] === 'bad' && pills[1].textContent.indexOf('Different') >= 0 && pills[1].querySelector('.mark').textContent === '✕',
      JSON.stringify(pills.map((p) => p.textContent)));
   check('what the check found is drawn as text, however hostile', rotationBox().querySelectorAll('img').length === 0 && rotationBox().textContent.indexOf(HOSTILE) >= 0);
   check('the waiting reason is a sentence with its time', rotationBox().textContent.indexOf('The old selector is kept for mail still on its way.') >= 0 &&
      rotationBox().textContent.indexOf('until 2026-09-21 00:00:00Z') >= 0, rotationBox().textContent);
   check('with nothing staged the card offers rotate now and retire, and no switch', act('dkimrotstart') !== null && act('dkimrotretire') !== null &&
      content().querySelectorAll('button[data-act="dkimrotswitch"]').length === 0);
   click(act('dkimrotcopy', { slot: 'current', i: 0 }));
   await flush();
   check('Copy puts the record\'s value on the clipboard', clipboard === 'v=DKIM1; k=rsa; p=KEYjan2026' && toastText() === 'Copied to clipboard', String(clipboard));

   document.getElementById('dom_postmaster').value = 'still-typing@example.com';
   setValue('dkimrot_selector', 'feb2026');
   before = requests.length;
   click(act('dkimrotstart'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains/example.com/dkim/rotation/start');
   check('Rotate now posts the selector typed', JSON.stringify(posted) === '{"selector":"feb2026"}', JSON.stringify(posted));
   check('and the card is redrawn from the answer: the record to publish', rotationBox().textContent.indexOf('feb2026._domainkey.example.com') >= 0 &&
      rotationBox().textContent.indexOf('Publish') >= 0 && rotationBox().textContent.indexOf('Phase: Waiting for the DNS record') >= 0 &&
      toastText() === 'A new key was generated and staged. Publish its record.', toastText());
   check('an edit waiting in another group survives it', document.getElementById('dom_postmaster').value === 'still-typing@example.com');
   check('once a key is staged the card offers switch and cancel, and no second start', act('dkimrotswitch') !== null && act('dkimrotcancel') !== null &&
      content().querySelectorAll('button[data-act="dkimrotstart"]').length === 0);

   before = requests.length;
   click(act('dkimrotcheck'));
   await flush();
   check('Check DNS now posts the check and draws what was found', called(before, 'POST', '/api/v1/domains/example.com/dkim/rotation/check').length === 1 &&
      rotationBox().textContent.indexOf('Not found') >= 0 && rotationBox().textContent.indexOf('The new key\'s record was not found. Publish it.') >= 0);

   confirmAnswer = false;
   before = requests.length;
   click(act('dkimrotswitch'));
   await flush();
   check('switching asks first, naming the domain, and no sends nothing', confirmations[confirmations.length - 1].indexOf('example.com') >= 0 &&
      called(before, 'POST', /dkim\/rotation\/switch/).length === 0);
   confirmAnswer = true;
   before = requests.length;
   click(act('dkimrotswitch'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains/example.com/dkim/rotation/switch');
   check('a switch without force sends no force', JSON.stringify(posted) === '{}', JSON.stringify(posted));
   check('and its refusal is the server\'s sentence under the card, with the card redrawn', document.getElementById('err_dkimrot').textContent.indexOf('propagation period') >= 0 &&
      called(before, 'GET', '/api/v1/domains/example.com/dkim/rotation').length === 1, document.getElementById('err_dkimrot').textContent);

   setChecked('dkimrot_force', true);
   before = requests.length;
   click(act('dkimrotswitch'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains/example.com/dkim/rotation/switch');
   check('with the box ticked the switch asks the other question and sends force', JSON.stringify(posted) === '{"force":true}' &&
      confirmations[confirmations.length - 1].indexOf('without waiting') >= 0, JSON.stringify(posted));
   check('a busy retiring slot is refused in the server\'s words', document.getElementById('err_dkimrot').textContent.indexOf('Retire it first') >= 0);

   before = requests.length;
   click(act('dkimrotretire'));
   await flush();
   check('Retire posts, and the retired selector\'s record is listed for removal', called(before, 'POST', '/api/v1/domains/example.com/dkim/rotation/retire').length === 1 &&
      rotationBox().textContent.indexOf('Retired key') >= 0 && rotationBox().textContent.indexOf('Remove') >= 0 && toastText() === 'The old selector was retired.', toastText());

   setChecked('dkimrot_force', true);
   before = requests.length;
   click(act('dkimrotswitch'));
   await flush();
   check('the forced switch moves signing and the selector box follows it', called(before, 'POST', '/api/v1/domains/example.com/dkim/rotation/switch').length === 1 &&
      document.getElementById('dom_dkim_selector').value === 'feb2026' && toastText() === 'Signing moved to the next key.' &&
      rotationBox().textContent.indexOf('Current key') >= 0, document.getElementById('dom_dkim_selector').value);

   setValue('dkimrot_selector', 'mar2026');
   click(act('dkimrotstart'));
   await flush();
   before = requests.length;
   click(act('dkimrotcancel'));
   await flush();
   check('Cancel deletes the next key and the card offers rotate now again', called(before, 'DELETE', '/api/v1/domains/example.com/dkim/rotation/next').length === 1 &&
      act('dkimrotstart') !== null && toastText() === 'The next key was cancelled.', toastText());

   // ---- the S/MIME authority card (RestApiSmimeAuthority.cpp)
   const smimeBox = () => document.getElementById('smimeAuthority');
   check('S/MIME authority: Edit reads the domain\'s authority, and a domain with none says so and offers the create with a key choice',
      called(openedAt, 'GET', '/api/v1/domains/example.com/smime-ca').length === 1 && headings.indexOf('S/MIME authority') >= 0 && smimeBox() !== null &&
         smimeBox().textContent.indexOf('This domain has no S/MIME authority.') >= 0 && act('smimecacreate') !== null &&
         document.getElementById('smimeca_key') !== null && content().querySelectorAll('button[data-act="smimecarotate"]').length === 0,
      smimeBox() ? smimeBox().textContent.slice(0, 200) : 'no card');
   check('S/MIME authority: the card says where it is managed and that a client trusts it only once the root is installed',
      smimeBox().textContent.indexOf('the Control Panel shows none of it') >= 0 && smimeBox().textContent.indexOf('once the root is installed on it') >= 0);
   setValue('smimeca_key', 'rsa3072');
   before = requests.length;
   click(act('smimecacreate'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains/example.com/smime-ca');
   check('S/MIME authority: Create posts the key chosen and draws the root from the answer', JSON.stringify(posted) === '{"key":"rsa3072"}' &&
      toastText() === 'S/MIME authority created' && smimeBox().textContent.indexOf('/CN=example.com S/MIME Authority') >= 0 &&
      smimeBox().textContent.indexOf('RSA-3072') >= 0 && content().querySelectorAll('button[data-act="smimecacreate"]').length === 0 && act('smimecarotate') !== null,
      JSON.stringify(posted) + ' ' + toastText());
   const download = document.getElementById('smimeca_download');
   check('S/MIME authority: the root is offered as a PEM download named after the domain',
      download !== null && download.attributes.download === 'example.com-smime-root.pem' &&
         decodeURIComponent(String(download.attributes.href).replace('data:application/x-pem-file;charset=utf-8,', '')).indexOf('-----BEGIN CERTIFICATE-----') === 0,
      download ? download.attributes.href : 'no download');
   check('S/MIME authority: with no base URL the card says that no CRL distribution point is written', smimeBox().textContent.indexOf('smime_authority_base_url') >= 0);

   // Certificates it issued - one of them to an address that is hostile, as a
   // stored address can be - and a key that no longer opens on this machine.
   state.smimeAuthorities['example.com'].issued = [
      { serial: 2, address: 'bob@example.com', account_id: 5, authority_id: 1, fingerprint: 'b'.repeat(64), not_before: SMIME_NOW, not_after: SMIME_NOW + 365 * 86400, status: 'valid', revoked: 0, reason: '' },
      { serial: 1, address: HOSTILE, account_id: 0, authority_id: 1, fingerprint: 'a'.repeat(64), not_before: SMIME_NOW, not_after: SMIME_NOW + 365 * 86400, status: 'revoked', revoked: SMIME_NOW, reason: 'cessationOfOperation' }];
   confirmAnswer = false;
   before = requests.length;
   click(act('smimecarotate'));
   await flush();
   check('S/MIME authority: rotating asks first, naming the domain, and no sends nothing', confirmations[confirmations.length - 1].indexOf('example.com') >= 0 &&
      called(before, 'POST', /smime-ca\/rotate/).length === 0);
   confirmAnswer = true;
   before = requests.length;
   click(act('smimecarotate'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains/example.com/smime-ca/rotate');
   check('S/MIME authority: Rotate posts the key chosen and draws the new root, the retired one and the issued list',
      JSON.stringify(posted) === '{"key":"p256"}' && toastText() === 'S/MIME authority rotated' && smimeBox().textContent.indexOf('Retired key') >= 0 &&
         smimeBox().textContent.indexOf('bob@example.com') >= 0 && smimeBox().querySelectorAll('.pill').length === 2,
      JSON.stringify(posted) + ' ' + toastText());
   check('S/MIME authority: an address is drawn as text, however hostile, and a revoked certificate says why with no Revoke of its own',
      smimeBox().querySelectorAll('img').length === 0 && smimeBox().textContent.indexOf(HOSTILE) >= 0 && smimeBox().textContent.indexOf('Cessation of operation') >= 0 &&
         content().querySelectorAll('button[data-act="smimecarevoke"]').length === 1);
   state.smimeAuthorities['example.com'].authority.key_opens = false;
   setValue('smimeca_reason', 'superseded');
   before = requests.length;
   click(act('smimecarevoke', { serial: '2' }));
   await flush();
   check('S/MIME authority: Revoke asks, naming the serial and the address, and sends the reason chosen',
      confirmations[confirmations.length - 1].indexOf('bob@example.com') >= 0 && confirmations[confirmations.length - 1].indexOf('2') >= 0 &&
         called(before, 'DELETE', '/api/v1/domains/example.com/smime-ca/issued/2?reason=superseded').length === 1 && toastText() === 'Certificate 2 revoked',
      paths(before) + ' ' + toastText());
   check('S/MIME authority: the card is read again and draws the revocation, and a root whose key does not open here is a warning',
      called(before, 'GET', '/api/v1/domains/example.com/smime-ca').length === 1 && smimeBox().textContent.indexOf('Superseded') >= 0 &&
         content().querySelectorAll('button[data-act="smimecarevoke"]').length === 0 &&
         smimeBox().textContent.indexOf('does not open on this machine') >= 0,
      smimeBox().textContent.slice(0, 400));
   state.smimeAuthorities['example.com'].authority.key_opens = true;

   setChecked('dom_dkim_rotation_enabled', true);
   setValue('dom_dkim_rotation_interval_days', '90');
   before = requests.length;
   click(act('domainsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/domains/example.com');
   check('the policy is saved as the domain\'s own keys, and only what changed - the selector the rotation moved is not sent back',
      JSON.stringify(put) === '{"active":true,"postmaster":"still-typing@example.com","dkim_rotation_enabled":true,"dkim_rotation_interval_days":90}', JSON.stringify(put));
   check('and the rotation is read again with the domain', called(before, 'GET', '/api/v1/domains/example.com/dkim/rotation').length === 1);
   setChecked('dom_dkim_rotation_enabled', false);
   setValue('dom_dkim_rotation_interval_days', '180');
   setValue('dom_postmaster', 'postmaster@example.com');
   click(act('domainsave'));
   await flush();

   before = requests.length;
   click(act('domainsave'));
   await flush();
   check('saving a domain with nothing changed sends nothing', called(before, 'PUT', /domains/).length === 0 && toastText() === 'Nothing changed', toastText());

   // ---- the domain's recovery key for mailbox encryption at rest
   // (RestApiMailboxKeys.cpp): read with the domain, made, then replaced
   const recoveryBox = () => document.getElementById('mailboxRecovery');
   check('the domain editor reads the domain\'s mailbox recovery key and says there is none',
      called(openedAt, 'GET', '/api/v1/domains/example.com/mailbox-recovery').length === 1 && recoveryBox() !== null &&
      recoveryBox().textContent.indexOf('No recovery key') >= 0 && act('mrecsave').textContent === 'Make the recovery key' &&
      document.getElementById('mrecCurrent') === null, recoveryBox() ? recoveryBox().textContent.slice(0, 200) : 'no card');
   setValue('mrecPassphrase', 'short');
   click(act('mrecsave'));
   await flush();
   check('a passphrase the server refuses is shown under the card', document.getElementById('err_mrec').textContent.indexOf('12 characters') >= 0,
      document.getElementById('err_mrec').textContent);
   setValue('mrecPassphrase', 'a passphrase kept elsewhere');
   setValue('mrecHint', 'the safe in the office');
   before = requests.length;
   click(act('mrecsave'));
   await flush();
   posted = lastBody(before, 'PUT', '/api/v1/domains/example.com/mailbox-recovery');
   check('Make the recovery key puts the passphrase and the hint, and the card shows the key, the hint and the replacement',
      JSON.stringify(posted) === '{"passphrase":"a passphrase kept elsewhere","hint":"the safe in the office"}' && toastText() === 'The recovery key was saved.' &&
      recoveryBox().textContent.indexOf('fedcba9876543210') >= 0 && recoveryBox().textContent.indexOf('the safe in the office') >= 0 &&
      act('mrecsave').textContent === 'Replace the recovery key' && document.getElementById('mrecCurrent') !== null &&
      document.getElementById('mrecUnfinished') === null,
      JSON.stringify(posted) + ' ' + toastText());
   setValue('mrecCurrent', 'a passphrase kept elsewhere');
   setValue('mrecPassphrase', 'another passphrase kept elsewhere');
   // The server says a replacement did not finish (MailboxKeys::SetRecovery):
   // the card says what that means and how to finish it.
   state.mailboxRecovery['example.com'].replacement_unfinished = true;
   before = requests.length;
   click(act('mrecsave'));
   await flush();
   posted = lastBody(before, 'PUT', '/api/v1/domains/example.com/mailbox-recovery');
   check('Replace the recovery key sends the current passphrase beside the new one',
      posted !== null && posted.current_passphrase === 'a passphrase kept elsewhere' && posted.passphrase === 'another passphrase kept elsewhere',
      JSON.stringify(posted));
   check('a replacement that did not finish is said so on the recovery card, with how to finish it',
      document.getElementById('mrecUnfinished') !== null &&
      document.getElementById('mrecUnfinished').textContent.indexOf('did not finish') >= 0,
      recoveryBox() ? recoveryBox().textContent.slice(0, 300) : 'no card');

   // ---- one value, three levels: the domain's
   const levelsBox = () => document.getElementById('domainLevels');
   check('the domain editor reads the domain itself for its levels, and says what is in effect and where it came from',
      levelsBox() !== null && levelsBox().textContent.indexOf('recall_days') >= 0 &&
      levelsBox().textContent.indexOf('In effect: 14, from the server') >= 0 &&
      document.getElementById('lvlown_recall_days').checked === false,
      levelsBox() ? levelsBox().textContent.slice(0, 300) : 'no card');
   check('a setting the domain keeps in a field of its own is shown with what is in effect and left to that field',
      levelsBox().textContent.indexOf('quarantine_digest') >= 0 && levelsBox().textContent.indexOf('In effect: Off, from the domain') >= 0 &&
      levelsBox().textContent.indexOf('Edited above.') >= 0 && document.getElementById('lvlown_quarantine_digest') === null,
      levelsBox().textContent.slice(0, 400));
   before = requests.length;
   click(act('domainlevels'));
   await flush();
   check('saving the levels with nothing changed sends nothing', called(before, 'PUT', /domains/).length === 0 && toastText() === 'Nothing changed', toastText());
   setChecked('lvlown_recall_days', true);
   setValue('lvl_recall_days', 'a month');
   before = requests.length;
   click(act('domainlevels'));
   await flush();
   check('a level that is not a whole number is refused by the page and nothing is sent', called(before, 'PUT', /domains/).length === 0 &&
      document.getElementById('err_domainlevels').textContent === 'recall_days must be a whole number.', document.getElementById('err_domainlevels').textContent);
   setValue('lvl_recall_days', '30');
   before = requests.length;
   click(act('domainlevels'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/domains/example.com');
   check('a level of the domain\'s own goes as a number, with active, which the route requires', JSON.stringify(put) === '{"recall_days":30,"active":true}', JSON.stringify(put));
   check('then the domain is read again and says the value is the domain\'s now', called(before, 'GET', '/api/v1/domains/example.com').length === 1 &&
      levelsBox().textContent.indexOf('In effect: 30, from the domain') >= 0 && document.getElementById('lvlown_recall_days').checked === true &&
      document.getElementById('lvl_recall_days').value === '30' && toastText() === 'Levels saved', levelsBox().textContent.slice(0, 300));
   setValue('dom_max_size_mb', '500');
   setChecked('dom_use_greylisting', true);
   before = requests.length;
   click(act('domainsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/domains/example.com');
   check('saving sends active, which the route requires, and only what changed', JSON.stringify(put) === '{"active":true,"max_size_mb":500,"use_greylisting":true}', JSON.stringify(put));
   check('then reads the domains again and stays in the editor with what the server holds', called(before, 'GET', '/api/v1/domains').length === 1 &&
      document.getElementById('dom_max_size_mb').value === '500' && document.getElementById('dom_use_greylisting').checked === true && toastText() === 'Domain saved', toastText());
   setValue('dom_relay_password', 'relay-secret');
   before = requests.length;
   click(act('domainsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/domains/example.com');
   check('a filled relay password goes with active and nothing else', JSON.stringify(put) === '{"active":true,"relay_password":"relay-secret"}', JSON.stringify(put));
   check('and its box is empty again after the re-read', document.getElementById('dom_relay_password').value === '');
   setValue('dom_max_accounts', 'many');
   before = requests.length;
   click(act('domainsave'));
   await flush();
   check('a limit that is not a number is refused by the page', called(before, 'PUT', /domains/).length === 0 && document.getElementById('err_domainedit').textContent.indexOf('whole number') >= 0,
      document.getElementById('err_domainedit').textContent);
   setValue('dom_max_accounts', '10');
   nextRefusal = 'The domain name is not valid.';
   click(act('domainsave'));
   await flush();
   check('a refusal keeps the editor open with the server\'s sentence', document.getElementById('err_domainedit').textContent === 'The domain name is not valid.' && document.getElementById('dom_max_accounts').value === '10');
   setValue('dom_name', 'renamed.example');
   before = requests.length;
   click(act('domainsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/domains/example.com');
   check('a new name is PUT to the old name, with the other change', !!put && put.name === 'renamed.example' && put.max_accounts === 10 && Object.keys(put).length === 3, JSON.stringify(put));
   check('and the editor is redrawn under the new name', content().querySelector('h2').textContent.indexOf('renamed.example') >= 0 && document.getElementById('dom_name').value === 'renamed.example' &&
      toastText() === 'Domain renamed to renamed.example', toastText());

   document.getElementById('dom_postmaster').value = 'keep-me@renamed.example';
   setValue('newDomainAlias', 'alias.example');
   before = requests.length;
   document.getElementById('newDomainAlias').dispatchEvent(makeEvent('keydown', { key: 'Enter' }));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains/renamed.example/domain-aliases');
   check('Enter in the name box adds the name, under the domain\'s new name', JSON.stringify(posted) === '{"name":"alias.example"}', JSON.stringify(posted));
   check('the names are read again and the new one is there', called(before, 'GET', '/api/v1/domains/renamed.example/domain-aliases').length === 1 && act('daliasdel', { name: 'alias.example' }) !== null);
   check('and an edit made in another group survives it', document.getElementById('dom_postmaster').value === 'keep-me@renamed.example');
   before = requests.length;
   click(act('daliasdel', { name: 'example.net' }));
   await flush();
   check('removing a name asks, deletes it by name and re-reads', confirmations[confirmations.length - 1].indexOf('example.net') >= 0 &&
      called(before, 'DELETE', '/api/v1/domains/renamed.example/domain-aliases/example.net').length === 1 && content().querySelectorAll('button[data-act="daliasdel"]').length === 1);
   setValue('newDomainSender', 'Spam.Example');
   setValue('newDomainSenderReason', 'kept sending');
   setChecked('newDomainSenderBlocked', true);
   before = requests.length;
   document.getElementById('newDomainSender').dispatchEvent(makeEvent('keydown', { key: 'Enter' }));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains/renamed.example/sender-list');
   check('Enter in the list box adds a blocked entry to the domain\'s list, with its reason', JSON.stringify(posted) === '{"value":"Spam.Example","blocked":true,"reason":"kept sending"}', JSON.stringify(posted));
   check('the list is read again and shows it blocked', called(before, 'GET', '/api/v1/domains/renamed.example/sender-list').length === 1 &&
      document.getElementById('domainSenders').textContent.indexOf('spam.example') >= 0 && document.getElementById('domainSenders').textContent.indexOf('Blocked') >= 0);
   check('and an edit made in another group survives that too', document.getElementById('dom_postmaster').value === 'keep-me@renamed.example');
   const senderId = act('dsenderdel', { value: 'spam.example' }).attributes['data-id'];
   before = requests.length;
   click(act('dsenderdel', { value: 'spam.example' }));
   await flush();
   check('removing an entry asks, naming it, and deletes it by id', confirmations[confirmations.length - 1].indexOf('spam.example') >= 0 &&
      called(before, 'DELETE', '/api/v1/domains/renamed.example/sender-list/' + senderId).length === 1 && document.getElementById('domainSenders').textContent.indexOf('spam.example') < 0);
   // An address's local part may carry braces (RFC 5322 atext): the question
   // quotes the value as it is, and the next value is not put into it.
   setValue('newDomainSender', 'a{1}b@spam.example');
   setChecked('newDomainSenderBlocked', false);
   document.getElementById('newDomainSender').dispatchEvent(makeEvent('keydown', { key: 'Enter' }));
   await flush();
   before = requests.length;
   click(act('dsenderdel', { value: 'a{1}b@spam.example' }));
   await flush();
   check('a value that carries a placeholder\'s braces is named as it is in the question',
      confirmations[confirmations.length - 1] === 'Remove a{1}b@spam.example from the list of renamed.example?' && called(before, 'DELETE', /\/sender-list\/\d+$/).length === 1,
      confirmations[confirmations.length - 1]);
   // ---- the domain's spam traps
   check('the domain editor reads the domain\'s spam traps and what has addressed them',
      document.getElementById('domainTraps') !== null &&
      document.getElementById('domainTraps').textContent.indexOf('never-used@example.com') >= 0 &&
      document.getElementById('domainTraps').textContent.indexOf('harvester@spammer.example') >= 0 &&
      document.getElementById('domainTraps').textContent.indexOf('banned for 1440 minutes') >= 0,
      document.getElementById('domainTraps') === null ? 'no card' : document.getElementById('domainTraps').textContent.slice(0, 300));
   setValue('newSpamTrap', 'sales-2004');
   before = requests.length;
   click(act('dtrapadd'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains/renamed.example/spam-traps');
   check('a trap is declared under its domain, by the part before the @ alone',
      JSON.stringify(posted) === '{"address":"sales-2004"}', JSON.stringify(posted));
   check('and the list is read again and shows the whole address',
      called(before, 'GET', '/api/v1/domains/renamed.example/spam-traps').length === 1 &&
      document.getElementById('domainTraps').textContent.indexOf('sales-2004@renamed.example') >= 0);
   before = requests.length;
   click(act('dtrapdel', { address: 'sales-2004@renamed.example' }));
   await flush();
   check('removing a trap asks, naming it, and deletes it at its own address',
      confirmations[confirmations.length - 1].indexOf('sales-2004@renamed.example') >= 0 &&
      called(before, 'DELETE', '/api/v1/spam-traps/sales-2004%40renamed.example').length === 1 &&
      document.getElementById('domainTraps').textContent.indexOf('sales-2004@renamed.example') < 0,
      paths(before));
   setValue('newSpamTrap', '');
   before = requests.length;
   click(act('dtrapadd'));
   await flush();
   check('an empty trap address is refused by the page and nothing is sent',
      called(before, 'POST', /spam-traps/).length === 0 && document.getElementById('err_dtrap').textContent === 'A trap address is required.');

   setValue('newDomainSender', '');
   before = requests.length;
   click(act('dsenderadd'));
   await flush();
   check('an empty entry is refused by the page and nothing is sent', called(before, 'POST', /sender-list/).length === 0 && document.getElementById('err_dsender').textContent === 'An address or a domain is required.');
   nextRefusal = 'That name is already a domain.';
   setValue('newDomainAlias', 'second.example');
   click(act('daliasadd'));
   await flush();
   check('a refused name is the server\'s sentence in the names box', document.getElementById('err_dalias').textContent === 'That name is already a domain.');
   setChecked('dom_active', false);
   before = requests.length;
   click(act('domainsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/domains/renamed.example');
   check('switching the domain off sends active false, with the postmaster edit that was waiting', JSON.stringify(put) === '{"active":false,"postmaster":"keep-me@renamed.example"}', JSON.stringify(put));
   click(act('cancel'));
   await flush();
   check('Cancel returns to the re-read list, which shows the rename and the state', rows().length === 3 && rows()[0].textContent.indexOf('renamed.example') >= 0 && rows()[0].textContent.indexOf('Disabled') >= 0 &&
      rows()[0].textContent.indexOf('keep-me@renamed.example') >= 0, rows()[0].textContent);
   before = requests.length;
   click(act('domainedit', { name: HOSTILE }));
   await flush();
   check('a hostile name is encoded in the path and drawn as text in the editor', called(before, 'GET', '/api/v1/domains/' + encodeURIComponent(HOSTILE) + '/domain-aliases').length === 1 &&
      content().querySelectorAll('img').length === 0 && document.getElementById('dom_name').value === HOSTILE);
   click(act('cancel'));
   await flush();

   setValue('newDomain', 'example.com');
   before = requests.length;
   click(act('domainnew'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/domains');
   check('creating a domain posts its name, active and postmaster', JSON.stringify(posted) === '{"name":"example.com","active":true,"postmaster":""}', JSON.stringify(posted));
   check('and opens the editor on the domain as read back', called(before, 'GET', '/api/v1/domains').length === 1 && document.getElementById('dom_name') !== null &&
      document.getElementById('dom_name').value === 'example.com' && toastText() === 'Domain created: example.com', toastText());
   click(act('cancel'));
   await flush();
   setValue('newDomain', 'second.example');
   before = requests.length;
   click(act('domainnew'));
   await flush();
   check('a domain that exists is the server\'s refusal beside the form', document.getElementById('err_domainnew').textContent === 'domain already exists' && rows().length === 4,
      document.getElementById('err_domainnew').textContent);
   setValue('newDomain', '   ');
   click(act('domainnew'));
   await flush();
   check('an empty name is refused by the page', called(before, 'POST', '/api/v1/domains').length === 1 && document.getElementById('err_domainnew').textContent.indexOf('required') >= 0);
   confirmAnswer = false;
   before = requests.length;
   click(act('domaindel', { name: 'example.com' }));
   await flush();
   check('deleting a domain asks, naming it and what goes with it', called(before, 'DELETE', /domains/).length === 0 && confirmations[confirmations.length - 1].indexOf('example.com') >= 0 &&
      confirmations[confirmations.length - 1].indexOf('account') >= 0, confirmations[confirmations.length - 1]);
   confirmAnswer = true;
   click(act('domaindel', { name: 'example.com' }));
   await flush();
   check('yes deletes by name and the re-read list is without it', called(before, 'DELETE', '/api/v1/domains/example.com').length === 1 && rows().length === 3 && rows().every((r) => r.textContent.indexOf('example.com') < 0));
   nextRefusal = 'The domain is named by a route.';
   click(act('domaindel', { name: 'second.example' }));
   await flush();
   check('a refused delete is shown on the domain\'s row', document.getElementById('err_domain_second.example').textContent === 'The domain is named by a route.' && rows().length === 3);

   // ---- IP ranges
   before = requests.length;
   await goTo('ipranges');
   check('the IP ranges view reads the ranges', called(before, 'GET', '/api/v1/ipranges').length === 1 && $('#viewTitle').textContent === 'IP ranges', paths(before));
   check('one row per range with the desktop list\'s columns', rows().length === 3 && rows()[1].textContent.indexOf('Internet') >= 0 && rows()[1].textContent.indexOf('0.0.0.0') >= 0 &&
      rows()[1].textContent.indexOf('255.255.255.255') >= 0 && rows()[1].textContent.indexOf('10') >= 0, rows().length ? rows()[1].textContent : 'no rows');
   check('the connection flags are shown as yes and no', rows()[1].querySelectorAll('.badge.good').length === 3 && rows()[2].querySelectorAll('.badge.warn').length === 3);
   check('a range that expires says so, with its time', rows()[2].textContent.indexOf('expires 2026-09-14 11:30:00') >= 0 && rows()[0].textContent.indexOf('expires') < 0);
   click(act('rangenew'));
   await flush();
   let rangeHeadings = content().querySelectorAll('h2').map((h) => h.textContent.trim());
   check('the editor is grouped as the desktop dialog is, the expiry in a group of its own', ['General', 'Connections', 'Relaying', 'Require auth', 'Protection', 'Expiry'].every((g) => rangeHeadings.indexOf(g) >= 0) && rangeHeadings.indexOf('Other') < 0,
      JSON.stringify(rangeHeadings));
   check('a new range does not expire and has no time', document.getElementById('range_expires').checked === false && document.getElementById('range_expires_time').value === '' &&
      document.getElementById('range_expires_time').closest('.fr').textContent.indexOf("server's clock") >= 0);
   const drawnRange = content().querySelectorAll('input, select, textarea').map((el) => el.id).filter((id) => id.indexOf('range_') === 0).map((id) => id.slice(6)).sort();
   check('every key of the schema has a control, and nothing else', JSON.stringify(drawnRange) === JSON.stringify(Object.keys(RANGE_PROPS).sort()), JSON.stringify(drawnRange));
   check('a new range starts where the create handler puts a key left out, with the desktop\'s priority',
      Object.keys(RANGE_CREATE_DEFAULTS).every((k) => document.getElementById('range_' + k).checked === RANGE_CREATE_DEFAULTS[k]) && document.getElementById('range_priority').value === '15' &&
      document.getElementById('range_name').value === '' && document.getElementById('range_lower').value === '',
      Object.keys(RANGE_CREATE_DEFAULTS).map((k) => k + '=' + document.getElementById('range_' + k).checked).join(' '));
   check('the required keys are marked and the flags carry the desktop\'s wording', document.getElementById('range_lower').closest('.fr').textContent.indexOf('required') >= 0 &&
      document.getElementById('range_priority').closest('.fr').textContent.indexOf('required') < 0 && document.getElementById('range_deliver_remote_to_remote').closest('.fr').textContent.indexOf('open relay') >= 0);
   setValue('range_name', 'Office');
   setValue('range_lower', '10.0.0.0');
   setValue('range_upper', '10.0.0.255');
   setValue('range_priority', 'high');
   before = requests.length;
   click(act('rangesave'));
   await flush();
   check('a priority that is not a number is refused by the page', called(before, 'POST', /ipranges/).length === 0 && document.getElementById('err_rangeedit').textContent.indexOf('whole number') >= 0);
   setValue('range_priority', '25');
   setChecked('range_deliver_local_to_remote', true);
   setValue('range_upper', 'ten.0.0.255');
   click(act('rangesave'));
   await flush();
   check('an address that does not parse is the server\'s refusal, shown in the editor', called(before, 'POST', '/api/v1/ipranges').length === 1 &&
      document.getElementById('err_rangeedit').textContent === 'lower and upper must be IP addresses' && document.getElementById('range_name').value === 'Office');
   setValue('range_upper', '10.0.0.255');
   before = requests.length;
   click(act('rangesave'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/ipranges');
   check('saving a new range posts every field, the priority as a number', !!posted && Object.keys(posted).length === Object.keys(RANGE_PROPS).length && posted.name === 'Office' && posted.lower === '10.0.0.0' &&
      posted.priority === 25 && posted.deliver_local_to_remote === true && posted.deliver_remote_to_remote === false, JSON.stringify(posted));
   check('and the re-read list has it', called(before, 'GET', '/api/v1/ipranges').length === 1 && rows().length === 4 && rows()[3].textContent.indexOf('Office') >= 0 && toastText() === 'IP range saved');
   click(act('rangeedit', { id: 2 }));
   await flush();
   check('editing shows the range\'s own values', document.getElementById('range_name').value === 'Internet' && document.getElementById('range_upper').value === '255.255.255.255' &&
      document.getElementById('range_priority').value === '10' && document.getElementById('range_require_auth_local_to_remote').checked === true);
   setChecked('range_allow_pop3', false);
   setChecked('range_require_tls_for_auth', true);
   before = requests.length;
   click(act('rangesave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/ipranges/2');
   check('saving an existing range PUTs the whole record by id', !!put && put.allow_pop3 === false && put.require_tls_for_auth === true && put.name === 'Internet' && Object.keys(put).length === Object.keys(RANGE_PROPS).length,
      JSON.stringify(put));
   check('and the re-read list shows the change', rows()[1].querySelectorAll('.badge.warn').length === 1);
   click(act('rangeedit', { id: 2 }));
   await flush();
   nextRefusal = 'The range overlaps My computer with the same priority.';
   click(act('rangesave'));
   await flush();
   check('a refused save keeps the editor open with the server\'s sentence', document.getElementById('err_rangeedit').textContent === 'The range overlaps My computer with the same priority.' && $('#range_name') !== null);
   setChecked('range_expires', true);
   before = requests.length;
   click(act('rangesave'));
   await flush();
   check('turning expiry on without a time is the server\'s refusal in the editor', called(before, 'PUT', '/api/v1/ipranges/2').length === 1 &&
      document.getElementById('err_rangeedit').textContent === 'expires_time is required when a range is made to expire' && document.getElementById('range_expires').checked === true);
   setValue('range_expires_time', '2030-01-02');
   before = requests.length;
   click(act('rangesave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/ipranges/2');
   check('with a time, the PUT carries the flag and the time and the row shows when', !!put && put.expires === true && put.expires_time === '2030-01-02' && rows()[1].textContent.indexOf('expires 2030-01-02 00:00:00') >= 0, JSON.stringify(put));
   click(act('rangeedit', { id: 3 }));
   await flush();
   check('editing a range the auto-ban placed shows its expiry', document.getElementById('range_expires').checked === true && document.getElementById('range_expires_time').value === '2026-09-14 11:30:00');
   click(act('cancel'));
   await flush();
   confirmAnswer = false;
   before = requests.length;
   click(act('rangedel', { id: 3 }));
   await flush();
   check('deleting a range asks, naming it and its addresses', called(before, 'DELETE', /ipranges/).length === 0 && confirmations[confirmations.length - 1].indexOf('Auto-ban: 203.0.113.9 (203.0.113.9 – 203.0.113.9)') >= 0,
      confirmations[confirmations.length - 1]);
   confirmAnswer = true;
   click(act('rangedel', { id: 3 }));
   await flush();
   check('yes deletes by id and the re-read list is without it', called(before, 'DELETE', '/api/v1/ipranges/3').length === 1 && rows().length === 3 && rows().every((r) => r.textContent.indexOf('Auto-ban') < 0));
   nextRefusal = 'The last range cannot be deleted.';
   click(act('rangedel', { id: 1 }));
   await flush();
   check('a refused delete is shown on its row', document.getElementById('err_range_1').textContent === 'The last range cannot be deleted.' && rows().length === 3);

   // ---- fetch accounts, under the account that owns them
   const fetchList = '/api/v1/accounts/anna%40example.com/fetch-accounts';
   before = requests.length;
   await goTo('fetch');
   check('the fetch view reads the domains, the first domain\'s accounts and the first account\'s external accounts',
      called(before, 'GET', '/api/v1/domains').length === 1 && called(before, 'GET', '/api/v1/domains/renamed.example/accounts').length === 1 && called(before, 'GET', fetchList).length === 1, paths(before));
   check('the pickers hold the domains and the accounts, the first of each chosen', $('#fetchDomain').querySelectorAll('option').length === 3 && $('#fetchDomain').value === 'renamed.example' &&
      $('#fetchAccount').querySelectorAll('option').map((o) => o.attributes.value).join(',') === 'anna@example.com,carla@example.com' && $('#fetchAccount').value === 'anna@example.com');
   check('one row per external account with its server, security, schedule and state', rows().length === 1 && rows()[0].textContent.indexOf('pop.isp.example:995') >= 0 && rows()[0].textContent.indexOf('TLS') >= 0 &&
      rows()[0].textContent.indexOf('30 min') >= 0 && rows()[0].textContent.indexOf('On') >= 0 && rows()[0].textContent.indexOf('2026-09-14 10:30:00') >= 0, rows().length ? rows()[0].textContent : 'no rows');
   before = requests.length;
   $('#fetchAccount').value = 'carla@example.com';
   $('#fetchAccount').dispatchEvent(makeEvent('change'));
   await flush();
   check('choosing an account reads its external accounts', called(before, 'GET', '/api/v1/accounts/carla%40example.com/fetch-accounts').length === 1 && $('#fetchAccount').value === 'carla@example.com' &&
      rows().length === 1 && rows()[0].textContent.indexOf('No external accounts') >= 0, paths(before));
   before = requests.length;
   $('#fetchDomain').value = 'second.example';
   $('#fetchDomain').dispatchEvent(makeEvent('change'));
   await flush();
   check('choosing a domain reads its accounts, and one without any has nothing to add to', called(before, 'GET', '/api/v1/domains/second.example/accounts').length === 1 &&
      $('#fetchAccount').querySelectorAll('option').length === 0 && content().querySelectorAll('button[data-act="fetchnew"]').length === 0 && called(before, 'GET', /fetch-accounts/).length === 0, paths(before));
   $('#fetchDomain').value = 'renamed.example';
   $('#fetchDomain').dispatchEvent(makeEvent('change'));
   await flush();
   check('back on a domain with accounts, its first is chosen again', $('#fetchAccount').value === 'anna@example.com' && rows().length === 1 && rows()[0].textContent.indexOf('ISP POP3') >= 0);
   click(act('fetchnew'));
   await flush();
   const fetchHeadings = content().querySelectorAll('h2').map((h) => h.textContent.trim());
   check('the editor names the account and is grouped', fetchHeadings[0].indexOf('anna@example.com') >= 0 && ['External account', 'Schedule', 'Downloaded mail'].every((g) => fetchHeadings.indexOf(g) >= 0) && fetchHeadings.indexOf('Other') < 0,
      JSON.stringify(fetchHeadings));
   const drawnFetch = content().querySelectorAll('input, select, textarea').map((el) => el.id).filter((id) => id.indexOf('fetch_') === 0).map((id) => id.slice(6)).sort();
   check('every key of the schema has a control, and nothing else', JSON.stringify(drawnFetch) === JSON.stringify(Object.keys(FETCH_PROPS).sort()), JSON.stringify(drawnFetch));
   check('a new one starts at the defaults the description states, read out of its sentence', document.getElementById('fetch_enabled').checked === true && document.getElementById('fetch_minutes_between_fetch').value === '30' &&
      document.getElementById('fetch_days_to_keep_messages').value === '0' && document.getElementById('fetch_server_type').value === 'pop3' && document.getElementById('fetch_connection_security').value === 'none' &&
      document.getElementById('fetch_port').value === '' && document.getElementById('fetch_name').value === '',
      'minutes=' + document.getElementById('fetch_minutes_between_fetch').value + ' days=' + document.getElementById('fetch_days_to_keep_messages').value + ' type=' + document.getElementById('fetch_server_type').value + ' sec=' + document.getElementById('fetch_connection_security').value);
   check('the three required keys are marked, the password box is write-only and empty, the port carries the desktop\'s advice',
      ['name', 'server_address', 'port'].every((k) => document.getElementById('fetch_' + k).closest('.fr').textContent.indexOf('required') >= 0) &&
      document.getElementById('fetch_username').closest('.fr').textContent.indexOf('required') < 0 &&
      document.getElementById('fetch_password').type === 'password' && document.getElementById('fetch_password').value === '' && document.getElementById('fetch_password').closest('.fr').textContent.indexOf('write-only') >= 0 &&
      document.getElementById('fetch_port').closest('.fr').textContent.indexOf('995') >= 0);
   setValue('fetch_name', 'ISP mailbox');
   setValue('fetch_server_address', 'mail.isp.example');
   setValue('fetch_port', '110');
   setValue('fetch_username', 'anna');
   setValue('fetch_password', 'isp-secret');
   setValue('fetch_connection_security', 'starttls_required');
   before = requests.length;
   click(act('fetchsave'));
   await flush();
   posted = lastBody(before, 'POST', fetchList);
   check('saving a new one posts the form under the account, numbers as numbers and the password as typed', !!posted && posted.name === 'ISP mailbox' && posted.server_address === 'mail.isp.example' && posted.port === 110 &&
      posted.minutes_between_fetch === 30 && posted.days_to_keep_messages === 0 && posted.password === 'isp-secret' && posted.connection_security === 'starttls_required' && posted.server_type === 'pop3' && posted.enabled === true &&
      posted.days_to_keep_meaning === 6106,
      JSON.stringify(posted));
   check('and the re-read list has it, for the same account', called(before, 'GET', fetchList).length === 1 && rows().length === 2 && rows()[1].textContent.indexOf('ISP mailbox') >= 0 &&
      rows()[1].textContent.indexOf('mail.isp.example:110') >= 0 && toastText() === 'External account saved', toastText());
   click(act('fetchedit', { id: 7 }));
   await flush();
   check('editing shows its values and an empty password box', document.getElementById('fetch_name').value === 'ISP POP3' && document.getElementById('fetch_port').value === '995' &&
      document.getElementById('fetch_connection_security').value === 'tls' && document.getElementById('fetch_password').value === '');
   setValue('fetch_minutes_between_fetch', '0');
   before = requests.length;
   click(act('fetchsave'));
   await flush();
   check('a schedule the server refuses is its sentence in the editor, with the edits kept', called(before, 'PUT', fetchList + '/7').length === 1 &&
      document.getElementById('err_fetchedit').textContent === 'minutes_between_fetch must be a whole number between 1 and 100000' && document.getElementById('fetch_minutes_between_fetch').value === '0');
   setValue('fetch_minutes_between_fetch', '15');
   before = requests.length;
   click(act('fetchsave'));
   await flush();
   put = lastBody(before, 'PUT', fetchList + '/7');
   check('saving an existing one PUTs the form by id without a password it was not given', !!put && put.minutes_between_fetch === 15 && put.name === 'ISP POP3' && !('password' in put), JSON.stringify(put));
   check('and the list shows the change', rows()[0].textContent.indexOf('15 min') >= 0);
   // What is left on the remote server, as the server means it for POP3 and IMAP
   // alike (FetchAccount.h): 0 everything, -1 nothing once collected, N days.
   // Until 6.3.6 the editor's hint had 0 and -1 the other way round, and the list
   // read "keep 0 days" for an account that keeps everything.
   check('the list says an account at 0 keeps its mail on the server', rows()[0].textContent.indexOf('keep on the server') >= 0 && rows()[0].textContent.indexOf('keep 0 days') < 0,
      rows()[0].textContent);
   click(act('fetchedit', { id: 7 }));
   await flush();
   check('the editor says 0 keeps every message and -1 deletes it after the download',
      document.getElementById('fetch_days_to_keep_messages').closest('.fr').textContent.indexOf('0 keeps every message, -1 deletes it after the download') >= 0,
      document.getElementById('fetch_days_to_keep_messages').closest('.fr').textContent);
   setValue('fetch_days_to_keep_messages', '-1');
   before = requests.length;
   click(act('fetchsave'));
   await flush();
   put = lastBody(before, 'PUT', fetchList + '/7');
   check('-1 is saved as -1, said to mean what 6.3.6 means by it, and the list says the mail is deleted after the download', !!put && put.days_to_keep_messages === -1 && put.days_to_keep_meaning === 6106 &&
      rows()[0].textContent.indexOf('delete after the download') >= 0, JSON.stringify(put) + ' ' + (rows()[0] ? rows()[0].textContent : 'no list: ' + document.getElementById('err_fetchedit').textContent));
   click(act('fetchedit', { id: 7 }));
   await flush();
   setValue('fetch_days_to_keep_messages', '7');
   click(act('fetchsave'));
   await flush();
   check('and 7 days is 7 days', rows()[0].textContent.indexOf('keep 7 days') >= 0, rows()[0].textContent);
   before = requests.length;
   click(act('fetchnow', { id: 7 }));
   await flush();
   check('Collect now posts to the download route and re-reads, showing the collection under way', called(before, 'POST', fetchList + '/7/download').length === 1 && called(before, 'GET', fetchList).length === 1 &&
      rows()[0].textContent.indexOf('collecting') >= 0 && toastText() === 'Collection queued', toastText());
   state.fetcherDown = true;
   click(act('fetchnow', { id: 7 }));
   await flush();
   check('a fetcher that is not running is the server\'s sentence on the row', document.getElementById('err_fetch_7').textContent === 'the external fetcher is not running');
   state.fetcherDown = false;
   confirmAnswer = false;
   before = requests.length;
   click(act('fetchdel', { id: 7 }));
   await flush();
   check('deleting asks, naming it and what stays', called(before, 'DELETE', /fetch-accounts/).length === 0 && confirmations[confirmations.length - 1].indexOf('ISP POP3') >= 0 &&
      confirmations[confirmations.length - 1].indexOf('already collected stays') >= 0, confirmations[confirmations.length - 1]);
   confirmAnswer = true;
   click(act('fetchdel', { id: 7 }));
   await flush();
   check('yes deletes by id under the account and the re-read list is without it', called(before, 'DELETE', fetchList + '/7').length === 1 && rows().length === 1 && rows()[0].textContent.indexOf('ISP mailbox') >= 0);

   // ---- migrations: many mailboxes from another server, a job per mailbox
   before = requests.length;
   await goTo('migrations');
   const migRows = () => rows().map((r) => r.textContent);
   check('the Migrations view reads the list and draws a row per migration', called(before, 'GET', '/api/v1/migrations').length === 1 &&
      $('#viewTitle').textContent === 'Migrations' && rows().length === 2, paths(before) + ' ' + rows().length + ' rows');
   check('each row names the migration, its source, its state, its progress and its jobs counted',
      migRows()[0].indexOf('Old mail server') >= 0 && migRows()[0].indexOf('imap.old.example') >= 0 && migRows()[0].indexOf('120 of 300 messages') >= 0 &&
      migRows()[0].indexOf('2 jobs: 0 done, 1 failed') >= 0 && rows()[0].querySelector('.badge').attributes['data-state'] === 'running' &&
      rows()[0].querySelector('.meter i').attributes.style === 'width:40%' && migRows()[1].indexOf('0 jobs: 0 done, 0 failed') >= 0,
      JSON.stringify(migRows()));
   check('a running migration offers Pause and a new one Start, and neither the other',
      !!act('migpause', { id: 41 }) && !!act('migstart', { id: 42 }) &&
      content().querySelectorAll('button[data-act="migstart"]').filter((b) => b.attributes['data-id'] === '41').length === 0 &&
      content().querySelectorAll('button[data-act="migpause"]').filter((b) => b.attributes['data-id'] === '42').length === 0);

   before = requests.length;
   click(act('migopen', { id: 41 }));
   await flush();
   check('opening a migration reads it and its jobs', called(before, 'GET', '/api/v1/migrations/41').length === 1 && called(before, 'GET', '/api/v1/migrations/41/jobs').length === 1 &&
      rows().length === 2, paths(before));
   check('each job shows its mailbox, its target, its state, its progress and its last error',
      migRows()[0].indexOf('anna@old.example') >= 0 && migRows()[0].indexOf('anna@example.com') >= 0 && migRows()[0].indexOf('120 of 300 messages') >= 0 &&
      migRows()[1].indexOf('bert@old.example') >= 0 && migRows()[1].indexOf('AUTHENTICATIONFAILED') >= 0 &&
      rows()[1].querySelector('.badge').attributes['data-state'] === 'failed', JSON.stringify(migRows()));
   check('a failed job offers Retry, a running one Cancel, and a running one no Retry', !!act('migretry', { id: 412 }) && !!act('migcancel', { id: 411 }) &&
      content().querySelectorAll('button[data-act="migretry"]').filter((b) => b.attributes['data-id'] === '411').length === 0);
   check('the summary is a download of the migration\'s CSV', $('#migSummary').attributes.href === '/api/v1/migrations/41/summary.csv');
   check('an IMAP migration\'s jobs offer no sign-in link', content().querySelectorAll('button[data-act="migsignin"]').length === 0);
   before = requests.length;
   click(act('migreport', { id: 411 }));
   await flush();
   const migReport = $('#migReport') ? $('#migReport').textContent : '';
   check('Report reads the job\'s report and shows each folder with what the source held, what was copied and what was skipped and why',
      called(before, 'GET', '/api/v1/migrations/41/jobs/411/report').length === 1 && migReport.indexOf('Report of anna@old.example') >= 0 &&
      migReport.indexOf('INBOX') >= 0 && migReport.indexOf('Sent Items') >= 0 && migReport.indexOf('too_large: 2') >= 0 &&
      migReport.indexOf('UIDVALIDITY') >= 0, paths(before) + ' ' + migReport.slice(0, 300));
   check('and the contacts skipped with the reason, the calendars copied, and the errors met on the way',
      migReport.indexOf('Skipped: the source answers no CardDAV at /.well-known/carddav') >= 0 && migReport.indexOf('3 copied, 1 skipped') >= 0 &&
      migReport.indexOf('the connection to imap.old.example was closed') >= 0 && migReport.indexOf('2026-09-10 00:29:20Z') >= 0, migReport);
   check('a job that can be retried has a box for the mended password beside Retry, and a running one has none',
      !!document.getElementById('migsecret_412') && document.getElementById('migsecret_412').type === 'password' && !document.getElementById('migsecret_411'));
   setValue('migsecret_412', 'the-mended-password');
   before = requests.length;
   click(act('migretry', { id: 412 }));
   await flush();
   const retried = lastBody(before, 'POST', '/api/v1/migrations/41/jobs/412/retry');
   check('Retry posts to the job\'s retry route with the password typed, and the re-read list shows it queued',
      called(before, 'POST', '/api/v1/migrations/41/jobs/412/retry').length === 1 && !!retried && retried.secret === 'the-mended-password' &&
      rows()[1].querySelector('.badge').attributes['data-state'] === 'queued' && toastText() === 'Job queued again', JSON.stringify(retried) + ' ' + toastText());
   confirmAnswer = false;
   before = requests.length;
   click(act('migcancel', { id: 411 }));
   await flush();
   check('Cancel asks first, saying what stays', called(before, 'POST', /\/cancel$/).length === 0 &&
      confirmations[confirmations.length - 1] === 'Cancel this job? What it copied stays.', confirmations[confirmations.length - 1]);
   confirmAnswer = true;
   click(act('migcancel', { id: 411 }));
   await flush();
   check('yes cancels it', called(before, 'POST', '/api/v1/migrations/41/jobs/411/cancel').length === 1 &&
      rows()[0].querySelector('.badge').attributes['data-state'] === 'cancelled' && toastText() === 'Job cancelled', toastText());
   before = requests.length;
   click(act('migpause', { id: 41 }));
   await flush();
   check('Pause posts to the migration\'s pause route and offers Resume after it', called(before, 'POST', '/api/v1/migrations/41/pause').length === 1 &&
      !!act('migresume', { id: 41 }) && toastText() === 'Migration paused', paths(before));
   nextRefusalAnswer = { status: 409, body: { error: 'the migration\'s jobs are still stopping' } };
   click(act('migresume', { id: 41 }));
   await flush();
   check('a refused resume is the server\'s sentence beside the migration', document.getElementById('err_mig_41').textContent === 'the migration\'s jobs are still stopping');
   before = requests.length;
   click(act('migresume', { id: 41 }));
   await flush();
   check('Resume posts to the resume route and the queued jobs show it', called(before, 'POST', '/api/v1/migrations/41/resume').length === 1 &&
      !!act('migpause', { id: 41 }) && toastText() === 'Migration resumed', paths(before));
   click(act('migback'));
   await flush();

   // A Google Workspace migration: a job without a secret waits for its owner.
   click(act('migopen', { id: 42 }));
   await flush();
   check('a migration without jobs says so and asks for them', rows().length === 1 && migRows()[0].indexOf('No jobs yet') >= 0 && !!$('#migCsv'));
   setValue('migCsv', 'dora@old.example,,anna@example.com\n');
   before = requests.length;
   click(act('migjobs'));
   await flush();
   check('a job without a secret is added and its sign-in link is shown once, for its owner', called(before, 'POST', '/api/v1/migrations/42/jobs').length === 1 &&
      !!$('#migLinks') && $('#migLinks').textContent.indexOf('dora@old.example') >= 0 &&
      $('#migLinks').textContent.indexOf('/portal/oauth/migration?job=') >= 0 && migRows()[0].indexOf('Awaiting its owner') >= 0 && toastText() === '1 job(s) added',
      toastText() + ' ' + ($('#migLinks') ? $('#migLinks').textContent : 'no links'));
   const doraJob = state.migrationJobs[42][0].id;
   before = requests.length;
   click(act('migsignin', { id: doraJob }));
   await flush();
   check('Sign-in link asks for a new one and shows it', called(before, 'POST', '/api/v1/migrations/42/jobs/' + doraJob + '/signin').length === 1 &&
      $('#migLinks').textContent.indexOf('cdcdcdcd') >= 0 && $('#migLinks').textContent.indexOf('abababab') < 0, paths(before));
   click(act('migback'));
   await flush();

   // A new migration, from the schema of its create.
   click(act('mignew'));
   await flush();
   const drawnMig = content().querySelectorAll('input, select, textarea').map((el) => el.id).filter((id) => id.indexOf('mig_') === 0).map((id) => id.slice(4)).sort();
   check('New migration draws every key of the create and nothing else', JSON.stringify(drawnMig) === JSON.stringify(Object.keys(MIGRATION_PROPS).sort()), JSON.stringify(drawnMig));
   check('a new one starts at the defaults the description states, the port left for the security to decide, the name marked required',
      document.getElementById('mig_kind').value === 'imap' && document.getElementById('mig_port').value === '' && document.getElementById('mig_security').value === 'tls' &&
      document.getElementById('mig_concurrency').value === '4' && document.getElementById('mig_messages_per_minute').value === '0' &&
      document.getElementById('mig_name').closest('.fr').textContent.indexOf('required') >= 0,
      ['kind', 'port', 'security', 'concurrency', 'messages_per_minute'].map((k) => k + '=' + document.getElementById('mig_' + k).value).join(' '));
   setValue('mig_name', 'Staff mailboxes');
   setValue('mig_security', 'starttls_required');
   before = requests.length;
   click(act('migsave'));
   await flush();
   check('an IMAP migration without a host is the server\'s sentence in the editor, with the edits kept', called(before, 'POST', '/api/v1/migrations').length === 1 &&
      document.getElementById('err_migedit').textContent === 'host is required for a migration from an IMAP server' && document.getElementById('mig_name').value === 'Staff mailboxes');
   check('with no port typed, STARTTLS is asked for on 143', (lastBody(before, 'POST', '/api/v1/migrations') || {}).port === 143,
      JSON.stringify(lastBody(before, 'POST', '/api/v1/migrations')));
   setValue('mig_security', 'tls');
   setValue('mig_host', 'imap.staff.example');
   setValue('mig_messages_per_minute', '600');
   before = requests.length;
   click(act('migsave'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/migrations');
   check('saving posts the form, numbers as numbers, TLS on 993, and an empty box not at all', !!posted && posted.name === 'Staff mailboxes' && posted.kind === 'imap' &&
      posted.host === 'imap.staff.example' && posted.port === 993 && posted.security === 'tls' && posted.concurrency === 4 && posted.messages_per_minute === 600 &&
      !('dav_url' in posted), JSON.stringify(posted));
   const staffId = state.migrations[state.migrations.length - 1].id;
   check('and opens the new migration, with no jobs yet', called(before, 'GET', '/api/v1/migrations/' + staffId + '/jobs').length === 1 &&
      content().textContent.indexOf('Staff mailboxes') >= 0 && migRows()[0].indexOf('No jobs yet') >= 0 && toastText() === 'Migration created', paths(before));
   const csv = 'sourceuser,secret,target\r\nanna.staff@staff.example,first-secret,anna@example.com\r\n"bert, the second",second-secret,nobody@example.com\r\n';
   setValue('migCsv', csv);
   before = requests.length;
   click(act('migjobs'));
   await flush();
   const sentCsv = called(before, 'POST', '/api/v1/migrations/' + staffId + '/jobs');
   check('the CSV is sent as it is, not rewritten', sentCsv.length === 1 && sentCsv[0].body === csv, sentCsv.length ? JSON.stringify(sentCsv[0].body) : 'nothing sent');
   check('a line the server refuses is its sentence, naming the line, and nothing is added',
      document.getElementById('err_migjobs').textContent === 'line 3: the target nobody@example.com is not an account of this server' && state.migrationJobs[staffId].length === 0 &&
      $('#migCsv').value === csv, document.getElementById('err_migjobs').textContent);
   setValue('migCsv', csv.replace('nobody@example.com', 'carla@example.com'));
   before = requests.length;
   click(act('migjobs'));
   await flush();
   check('mended, both jobs are added and listed, with no sign-in link for an IMAP source', called(before, 'GET', '/api/v1/migrations/' + staffId + '/jobs').length === 1 &&
      rows().length === 2 && migRows()[1].indexOf('bert, the second') >= 0 && migRows()[1].indexOf('carla@example.com') >= 0 && !$('#migLinks') &&
      toastText() === '2 job(s) added', toastText());
   const csvFile = $('#migCsvFile');
   csvFile.files = [{ text: () => Promise.resolve('carla.staff@staff.example,third-secret,carla@example.com\r\n') }];
   csvFile.dispatchEvent(makeEvent('change'));
   await flush();
   check('a CSV file chosen is read into the box, to be looked at before it is sent', $('#migCsv').value === 'carla.staff@staff.example,third-secret,carla@example.com\r\n' &&
      called(before, 'POST', /\/jobs$/).length === 1, $('#migCsv').value);
   before = requests.length;
   click(act('migstart', { id: staffId }));
   await flush();
   check('Start posts to the start route', called(before, 'POST', '/api/v1/migrations/' + staffId + '/start').length === 1 && toastText() === 'Migration started', paths(before));
   click(act('migback'));
   await flush();
   check('back at the list, the new migration is the third row', rows().length === 3 && migRows()[2].indexOf('Staff mailboxes') >= 0 && migRows()[2].indexOf('2 jobs: 0 done, 0 failed') >= 0,
      JSON.stringify(migRows()));
   confirmAnswer = false;
   before = requests.length;
   click(act('migdel', { id: 42 }));
   await flush();
   check('deleting asks, naming it and what stays', called(before, 'DELETE', /migrations/).length === 0 &&
      confirmations[confirmations.length - 1] === 'Delete the migration Google Workspace? What it copied stays.', confirmations[confirmations.length - 1]);
   confirmAnswer = true;
   nextRefusalAnswer = { status: 409, body: { error: 'a job of this migration is running: pause it first' } };
   click(act('migdel', { id: 41 }));
   await flush();
   check('a delete refused while a job runs is the server\'s sentence on the row', document.getElementById('err_mig_41').textContent === 'a job of this migration is running: pause it first' &&
      rows().length === 3);
   before = requests.length;
   click(act('migdel', { id: 42 }));
   await flush();
   check('yes deletes by id and the re-read list is without it', called(before, 'DELETE', '/api/v1/migrations/42').length === 1 && rows().length === 2 &&
      migRows().every((r) => r.indexOf('Google Workspace') < 0) && toastText() === 'Migration deleted', paths(before));

   // A migration of Outlook data files, from the same form: the kind the
   // schema offers is what is posted, and no host is asked for.
   click(act('mignew'));
   await flush();
   setValue('mig_name', 'Archives');
   setValue('mig_kind', 'pst');
   before = requests.length;
   click(act('migsave'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/migrations');
   check('a migration of kind pst posts kind pst, with no host', !!posted && posted.kind === 'pst' && posted.name === 'Archives' && !('host' in posted) &&
      state.migrations[state.migrations.length - 1].kind === 'pst' && toastText() === 'Migration created', JSON.stringify(posted));
   click(act('migback'));
   await flush();

   // ---- the seven small collections, one view
   before = requests.length;
   await goTo('lists');
   check('the Lists view reads the nine collections', Object.keys(COLLECTION_SPECS).every((p) => called(before, 'GET', p).length === 1) && $('#viewTitle').textContent === 'Lists', paths(before));
   const tables = content().querySelectorAll('table');
   check('one table per collection, each with its entry\'s own columns and a row per entry', tables.length === 11 && tables.every((t) => t.querySelectorAll('tbody tr').length === 1) &&
      tables[0].querySelectorAll('th').map((h) => h.textContent).join(',') === 'Active,DNS host,Expected result,Reject message,Score,' &&
      tables[7].querySelectorAll('th').map((h) => h.textContent).join(',') === 'Name,Lower IP,Upper IP,' &&
      tables[8].querySelectorAll('th').map((h) => h.textContent).join(',') === 'Pattern,Match,Require SPF,Require DKIM,Require DMARC,Note,' &&
      tables[9].querySelectorAll('th').map((h) => h.textContent).join(',') === 'Pattern,Match on,Action,Note,' &&
      tables[10].querySelectorAll('th').map((h) => h.textContent).join(',') === 'Pattern,Note,Created,', tables.length + ' tables');
   check('a flag is yes or no, and the values are the entries\' own', tables[0].querySelector('.badge.good') !== null && tables[1].querySelector('.badge.warn') !== null &&
      tables[0].textContent.indexOf('zen.spamhaus.org') >= 0 && tables[4].textContent.indexOf('*.exe') >= 0 && tables[5].textContent.indexOf('partner.example') >= 0 &&
      tables[6].textContent.indexOf('10.0.0.*') >= 0 && tables[7].textContent.indexOf('Front relay') >= 0 && tables[8].textContent.indexOf('.mail.partner.example') >= 0 &&
      tables[9].textContent.indexOf('.sloppy-partner.example') >= 0);
   check('each section carries its route\'s own sentence and an Add', content().textContent.indexOf('AntiVirus.BlockedAttachments over COM') >= 0 &&
      COLLECTION_SPECS && ['dnsbl', 'surbl', 'whitelist', 'blocked', 'attachments', 'senderlist', 'greylist', 'relays', 'greybypass', 'domainpolicy', 'neverban'].every((id) => act('collnew', { coll: id }) !== null));
   const checkButtons = content().querySelectorAll('button[data-act="collcheck"]');
   check('the never-ban list is the one collection with a check, and the others have none', checkButtons.length === 1 && checkButtons[0].attributes['data-coll'] === 'neverban' &&
      document.getElementById('chk_neverban') !== null && ['dnsbl', 'domainpolicy', 'greybypass'].every((id) => document.getElementById('chk_' + id) === null), JSON.stringify(checkButtons.map((b) => b.attributes)));
   click(act('collnew', { coll: 'dnsbl' }));
   await flush();
   const drawnColl = content().querySelectorAll('input, select, textarea').map((el) => el.id).filter((id) => id.indexOf('coll_') === 0).map((id) => id.slice(5)).sort();
   check('Add opens an editor with every key of the create and nothing else', JSON.stringify(drawnColl) === JSON.stringify(Object.keys(DNS_LIST_PROPS).sort()), JSON.stringify(drawnColl));
   check('a new entry starts at the description\'s default and the desktop editor\'s', document.getElementById('coll_active').checked === true && document.getElementById('coll_score').value === '5' &&
      document.getElementById('coll_dns_host').value === '' && document.getElementById('coll_dns_host').closest('.fr').textContent.indexOf('required') >= 0 &&
      document.getElementById('coll_score').closest('.fr').textContent.indexOf('required') < 0);
   setValue('coll_score', 'high');
   before = requests.length;
   click(act('collsave'));
   await flush();
   check('a score that is not a number is refused by the page', called(before, 'POST', /dns-blacklists/).length === 0 && document.getElementById('err_colledit').textContent.indexOf('whole number') >= 0);
   setValue('coll_score', '3');
   click(act('collsave'));
   await flush();
   check('a create without its required key is the server\'s refusal in the editor', called(before, 'POST', '/api/v1/dns-blacklists').length === 1 && document.getElementById('err_colledit').textContent === 'dns_host is required');
   setValue('coll_dns_host', 'bl.example.test');
   setValue('coll_reject_message', 'Listed');
   before = requests.length;
   click(act('collsave'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/dns-blacklists');
   check('saving a new entry posts the form, the score as a number', JSON.stringify(posted) === '{"dns_host":"bl.example.test","expected_result":"","reject_message":"Listed","score":3,"active":true}', JSON.stringify(posted));
   check('and the re-read view has it', called(before, 'GET', '/api/v1/dns-blacklists').length === 1 && content().querySelectorAll('table')[0].querySelectorAll('tbody tr').length === 2 &&
      content().querySelectorAll('table')[0].textContent.indexOf('bl.example.test') >= 0 && toastText() === 'DNS blacklist saved', toastText());
   click(act('colledit', { coll: 'blocked', id: 14 }));
   await flush();
   check('editing shows the entry\'s own values and the update\'s sentence', document.getElementById('coll_address').value === 'spam.example' && document.getElementById('coll_score').value === '100' &&
      document.getElementById('coll_description').value === 'A spam domain' && content().textContent.indexOf('a field left out keeps its value') >= 0);
   setValue('coll_score', '50');
   before = requests.length;
   click(act('collsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/blocked-senders/14');
   check('saving an existing entry PUTs the whole record by id', JSON.stringify(put) === '{"address":"spam.example","score":50,"description":"A spam domain"}' && content().querySelectorAll('table')[3].textContent.indexOf('50') >= 0, JSON.stringify(put));
   // The server's allow and block list, which is the same collection machinery
   // as the six before it - so what is worth pinning is that its own fields are
   // drawn and that a save reaches its own path, not the mechanism again.
   click(act('collnew', { coll: 'senderlist' }));
   await flush();
   const drawnSender = content().querySelectorAll('input, select, textarea').map((el) => el.id).filter((id) => id.indexOf('coll_') === 0).map((id) => id.slice(5)).sort();
   check('the allow and block list draws its own five fields', JSON.stringify(drawnSender) === JSON.stringify(['blocked', 'expires', 'expires_time', 'reason', 'value']), JSON.stringify(drawnSender));
   check('a new entry allows rather than blocks, which is the harmless accident', document.getElementById('coll_blocked').checked === false && document.getElementById('coll_expires').checked === false);
   setValue('coll_value', 'newsletters.example');
   setValue('coll_reason', 'their invoices kept scoring');
   before = requests.length;
   click(act('collsave'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/sender-list');
   check('saving posts the entry to the sender list', JSON.stringify(posted) === '{"value":"newsletters.example","blocked":false,"reason":"their invoices kept scoring","expires":false,"expires_time":""}', JSON.stringify(posted));
   check('and the re-read table shows it', content().querySelectorAll('table')[5].textContent.indexOf('newsletters.example') >= 0,
      content().querySelectorAll('table')[5].textContent);

   click(act('collnew', { coll: 'attachments' }));
   await flush();
   check('a new attachment starts at the desktop\'s *.exe, a new relay at nothing', document.getElementById('coll_wildcard').value === '*.exe');
   click(act('cancel'));
   await flush();
   click(act('collnew', { coll: 'greylist' }));
   await flush();
   setValue('coll_ip_address', '203.0.113.*');
   nextRefusal = 'The IP address must not be empty.';
   click(act('collsave'));
   await flush();
   check('a refused save keeps the editor open with the server\'s sentence', document.getElementById('err_colledit').textContent === 'The IP address must not be empty.' && document.getElementById('coll_ip_address').value === '203.0.113.*');
   before = requests.length;
   click(act('collsave'));
   await flush();
   check('and the next save posts to the collection\'s own path', called(before, 'POST', '/api/v1/greylisting-white-addresses').length === 1 && content().querySelectorAll('table')[6].textContent.indexOf('203.0.113.*') >= 0);
   confirmAnswer = false;
   before = requests.length;
   click(act('colldel', { coll: 'relays', id: 17 }));
   await flush();
   check('deleting asks, naming the entry by its first text column', called(before, 'DELETE', /incoming-relays/).length === 0 && confirmations[confirmations.length - 1].indexOf('incoming relay Front relay') >= 0,
      confirmations[confirmations.length - 1]);
   confirmAnswer = true;
   click(act('colldel', { coll: 'relays', id: 17 }));
   await flush();
   check('yes deletes by id and the re-read table is empty', called(before, 'DELETE', '/api/v1/incoming-relays/17').length === 1 && content().querySelectorAll('table')[7].textContent.indexOf('Nothing here') >= 0);
   // The greylisting bypass list: its match is a select with the document's
   // three words, its three requirements are flags, a new entry requires SPF
   // and nothing else, and a save reaches its own path with the flags as
   // booleans - which is what the server's create needs to refuse an entry
   // that requires nothing.
   click(act('collnew', { coll: 'greybypass' }));
   await flush();
   const drawnBypass = content().querySelectorAll('input, select, textarea').map((el) => el.id).filter((id) => id.indexOf('coll_') === 0).map((id) => id.slice(5)).sort();
   check('the greylisting bypass list draws its own six fields', JSON.stringify(drawnBypass) === JSON.stringify(['match', 'note', 'pattern', 'require_dkim', 'require_dmarc', 'require_spf']), JSON.stringify(drawnBypass));
   const matchSelect = document.getElementById('coll_match');
   check('match is a select of the three words, envelope first, and a new entry requires SPF alone', matchSelect.tagName === 'SELECT' && matchSelect.querySelectorAll('option').map((o) => o.attributes.value).join(',') === 'envelope,helo,both' &&
      matchSelect.value === 'envelope' && document.getElementById('coll_require_spf').checked === true && document.getElementById('coll_require_dkim').checked === false && document.getElementById('coll_require_dmarc').checked === false, matchSelect.value);
   setValue('coll_pattern', 'partner.example');
   setChecked('coll_require_dkim', true);
   setValue('coll_note', 'signed but never tidy');
   before = requests.length;
   click(act('collsave'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/greylisting-bypass');
   check('saving posts the entry with the match word and the three flags', JSON.stringify(posted) === '{"pattern":"partner.example","match":"envelope","require_spf":true,"require_dkim":true,"require_dmarc":false,"note":"signed but never tidy"}', JSON.stringify(posted));
   check('and the re-read table shows both entries', content().querySelectorAll('table')[8].querySelectorAll('tbody tr').length === 2 && content().querySelectorAll('table')[8].textContent.indexOf('partner.example') >= 0 && toastText() === 'Greylisting bypass entry saved', toastText());
   click(act('collnew', { coll: 'greybypass' }));
   await flush();
   setValue('coll_pattern', 'nothing.example');
   setChecked('coll_require_spf', false);
   nextRefusal = 'At least one of SPF, DKIM and DMARC must be required.';
   click(act('collsave'));
   await flush();
   check('an entry requiring nothing is the server\'s refusal in the editor', document.getElementById('err_colledit').textContent === 'At least one of SPF, DKIM and DMARC must be required.' && document.getElementById('coll_pattern').value === 'nothing.example');
   click(act('cancel'));
   await flush();
   nextRefusal = 'The white list is in use.';
   click(act('colldel', { coll: 'whitelist', id: 13 }));
   await flush();
   check('a refused delete is shown on its row', document.getElementById('err_coll_whitelist_13').textContent === 'The white list is in use.');
   // The never-ban list's check: nothing is asked without an address, an
   // address the server spares says so with its reverse name, one it does not
   // says that too and why, and text that is not an address is the server's
   // refusal - each beside the case it must not be taken for.
   before = requests.length;
   click(act('collcheck', { coll: 'neverban' }));
   await flush();
   check('a check with no address asks the server nothing and says why', called(before, 'POST', '/api/v1/never-ban-names/check').length === 0 &&
      document.getElementById('err_chk_neverban').textContent === 'Type an address first.');
   setValue('chk_neverban', ' 209.85.220.41 ');
   before = requests.length;
   click(act('collcheck', { coll: 'neverban' }));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/never-ban-names/check');
   const spareOut = document.getElementById('out_chk_neverban').textContent;
   check('a check posts the trimmed address alone and shows a spared verdict with its reverse name', JSON.stringify(posted) === '{"address":"209.85.220.41"}' &&
      spareOut.indexOf('Spared from auto-ban') >= 0 && spareOut.indexOf('Not spared') < 0 && spareOut.indexOf('mail-sor-f41.google.com') >= 0 &&
      document.getElementById('out_chk_neverban').querySelectorAll('tbody tr').length === 1, spareOut);
   setValue('chk_neverban', '203.0.113.9');
   click(act('collcheck', { coll: 'neverban' }));
   await flush();
   const refuseOut = document.getElementById('out_chk_neverban').textContent;
   check('an address it does not spare says so, and why', refuseOut.indexOf('Not spared') >= 0 && refuseOut.indexOf('Spared from auto-ban') < 0 && refuseOut.indexOf('does not resolve back') >= 0, refuseOut);
   setValue('chk_neverban', 'not an address');
   click(act('collcheck', { coll: 'neverban' }));
   await flush();
   check('text that is not an address is the server\'s refusal, and the last verdict is gone', document.getElementById('err_chk_neverban').textContent.indexOf('must be an IPv4 or IPv6 address') >= 0 &&
      document.getElementById('out_chk_neverban').textContent === '');

   // ---- the delivery queue
   before = requests.length;
   await goTo('queue');
   check('the queue is read', called(before, 'GET', '/api/v1/queue').length === 1);
   check('one row per queued message with its recipients and next try', rows().length === 2 && rows()[0].textContent.indexOf('far@away.example') >= 0 && rows()[0].textContent.indexOf('2026-09-14 10:00:00') >= 0);
   before = requests.length;
   click(act('retry', { id: 41 }));
   await flush();
   check('Retry now posts to the message\'s retry route and re-reads', called(before, 'POST', '/api/v1/queue/41/retry').length === 1 && called(before, 'GET', '/api/v1/queue').length === 1, paths(before));
   nextRefusal = 'The message is being delivered right now.';
   click(act('retry', { id: 42 }));
   await flush();
   check('a refused retry is shown on its own row', document.getElementById('err_queue_42').textContent === 'The message is being delivered right now.');
   before = requests.length;
   click(act('delmsg', { id: 41 }));
   await flush();
   check('Delete confirms, deletes and re-reads', confirmations[confirmations.length - 1].indexOf('41') >= 0 && called(before, 'DELETE', '/api/v1/queue/41').length === 1 && rows().length === 1);

   // ---- the slowed destinations, beside the queue
   check('the slowed destinations are read with the queue', called(0, 'GET', '/api/v1/queue/shaping').length >= 1);
   check('a back-off is drawn with its destination, its window and the reply that opened it',
      content().textContent.indexOf('slow.example') >= 0 && content().textContent.indexOf('until 2026-09-22 12:02:00') >= 0 &&
      content().textContent.indexOf('window 2 min') >= 0 && content().textContent.indexOf('3 deferred') >= 0 &&
      content().textContent.indexOf('421 4.7.0 [TSS04]') >= 0 && content().textContent.indexOf('mx.slow.example') >= 0, content().textContent);
   check('the connections in use are drawn, destinations and tenants apart',
      content().textContent.indexOf('slow.example: 1') >= 0 && content().textContent.indexOf('example.com: 2') >= 0);
   confirmAnswer = false;
   before = requests.length;
   click(act('shresume', { id: 'slow.example' }));
   await flush();
   check('Resume asks first, naming the destination, and no sends nothing', confirmations[confirmations.length - 1].indexOf('slow.example') >= 0 &&
      called(before, 'DELETE', /queue\/shaping/).length === 0);
   confirmAnswer = true;
   before = requests.length;
   click(act('shresume', { id: 'slow.example' }));
   await flush();
   check('Resume deletes the destination\'s back-off and re-reads the queue', called(before, 'DELETE', '/api/v1/queue/shaping/slow.example').length === 1 &&
      called(before, 'GET', '/api/v1/queue/shaping').length === 1 && called(before, 'GET', '/api/v1/queue').length === 1, paths(before));
   check('and with the window gone the box says so', content().textContent.indexOf('No destination has asked this server to slow down.') >= 0 &&
      content().querySelectorAll('button[data-act="shresume"]').length === 0 && toastText() === 'Back-off cleared', toastText());

   // ---- held for a sandbox, beside the queue
   check('what is held for a sandbox is read with the queue, a page of up to a thousand', called(0, 'GET', '/api/v1/sandbox/held?limit=1000').length >= 1);
   check('a held message the queue lists is not a row of its table, where Retry and Delete would not reach it',
      Array.from(rows()).every((r) => r.textContent.indexOf('outsider@far.example') < 0) && content().querySelectorAll('button[data-act="retry"][data-id="77"]').length === 0);
   check('a held message is drawn with its id, its sender, how long it has waited, its file and the submission',
      content().textContent.indexOf('outsider@far.example') >= 0 && content().textContent.indexOf('Held for 42 s; 1 file(s)') >= 0 &&
      content().textContent.indexOf('invoice.exe') >= 0 && content().textContent.indexOf('job-7') >= 0 &&
      content().textContent.indexOf('report.docm') >= 0 && content().textContent.indexOf('not submitted yet') >= 0, content().textContent);
   check('the state of a file is words, not the machine word the list answers with',
      content().textContent.indexOf('not_submitted') < 0 && content().textContent.indexOf('submitted, waiting for the verdict') >= 0, content().textContent);
   check('a page that is not the whole list says how much of it is shown',
      content().textContent.indexOf('2 of 3 held messages are shown, oldest first.') >= 0, content().textContent);
   confirmAnswer = false;
   before = requests.length;
   click(act('sbrelease', { id: 77 }));
   await flush();
   check('Release asks first, naming the message, and no sends nothing', confirmations[confirmations.length - 1].indexOf('77') >= 0 &&
      called(before, 'POST', /sandbox\/held/).length === 0);
   confirmAnswer = true;
   before = requests.length;
   click(act('sbdiscard', { id: 78 }));
   await flush();
   check('Discard asks, naming the message, deletes it and re-reads', confirmations[confirmations.length - 1].indexOf('78') >= 0 &&
      called(before, 'DELETE', '/api/v1/sandbox/held/78').length === 1 && called(before, 'GET', '/api/v1/sandbox/held?limit=1000').length === 1 &&
      content().textContent.indexOf('report.docm') < 0 && toastText() === 'Discarded', paths(before));
   before = requests.length;
   click(act('sbrelease', { id: 77 }));
   await flush();
   check('Release posts to the message\'s release route and re-reads', called(before, 'POST', '/api/v1/sandbox/held/77/release').length === 1 &&
      called(before, 'GET', '/api/v1/sandbox/held?limit=1000').length === 1, paths(before));
   check('and with nothing held the box says so', content().textContent.indexOf('Nothing is held for a sandbox.') >= 0 &&
      content().querySelectorAll('button[data-act="sbrelease"]').length === 0 && toastText() === 'Released to the delivery queue', toastText());
   // ---- the IP warm-up, beside the queue
   const warmBox = () => document.getElementById('warmupBox');
   check('the warm-up schedules are read with the queue', called(0, 'GET', '/api/v1/warmup').length >= 1);
   check('a schedule is drawn with its address, plan and day, and each provider with what it sent of its ceiling and what was deferred',
      warmBox() !== null && warmBox().textContent.indexOf('192.0.2.10') >= 0 && warmBox().textContent.indexOf('Standard') >= 0 &&
      warmBox().textContent.indexOf('Day 1 of 30') >= 0 && warmBox().textContent.indexOf('Google: 50 / 50 · 1 deferred') >= 0 &&
      warmBox().textContent.indexOf('Microsoft: 3 / 50') >= 0 && warmBox().textContent.indexOf('Other providers: 12 / 250') >= 0,
      warmBox() ? warmBox().textContent : 'no box');
   check('an address SMTPDeliveryBindToIP does not bind is said to count nothing',
      warmBox().textContent.indexOf('SMTPDeliveryBindToIP does not bind this address') >= 0 &&
      warmBox().querySelectorAll('[data-warn="bind"]').length === 1, warmBox().textContent);
   click(act('warmedit', { id: 7 }));
   await flush();
   check('Edit opens the small editor on the schedule',
      $('#warm_address').value === '192.0.2.10' && $('#warm_plan').value === 'standard' && $('#warm_started').value === '2026-09-24' &&
      $('#warm_enabled').checked === true, [$('#warm_address').value, $('#warm_plan').value, $('#warm_started').value].join(' '));
   setValue('warm_plan', 'gentle');
   setChecked('warm_enabled', false);
   before = requests.length;
   click(act('warmsave'));
   await flush();
   const warmPut = lastBody(before, 'PUT', '/api/v1/warmup/7');
   check('Save sends the schedule to its PUT route without the address, which a PUT may not change, and the queue is drawn again',
      warmPut !== null && warmPut.plan === 'gentle' && warmPut.enabled === false && !('address' in warmPut) && warmPut.started === '2026-09-24' &&
      called(before, 'GET', '/api/v1/warmup').length === 1 && toastText() === 'Warm-up schedule saved', paths(before) + ' ' + JSON.stringify(warmPut));
   check('and the box shows the change', warmBox().textContent.indexOf('Gentle') >= 0 && warmBox().textContent.indexOf('Disabled') >= 0, warmBox().textContent);
   click(act('warmedit', { id: 7 }));
   await flush();
   check('on Edit the address is shown and cannot be typed over', $('#warm_address').attributes.readonly !== undefined, JSON.stringify($('#warm_address').attributes));
   setValue('warm_started', '');
   before = requests.length;
   click(act('warmsave'));
   await flush();
   const emptiedPut = lastBody(before, 'PUT', '/api/v1/warmup/7');
   check('an emptied Day 1 on Edit is sent as today\'s UTC date, as the hint says, not left out to keep the old start',
      emptiedPut !== null && emptiedPut.started === new Date().toISOString().slice(0, 10), JSON.stringify(emptiedPut));
   click(act('warmnew'));
   await flush();
   before = requests.length;
   click(act('warmsave'));
   await flush();
   check('Add with no address is refused in the page and sends nothing',
      document.getElementById('err_warmedit').textContent === 'An outbound address is required.' && called(before, 'POST', '/api/v1/warmup').length === 0);
   setValue('warm_address', '192.0.2.10');
   click(act('warmsave'));
   await flush();
   check('a second schedule for an address is the server\'s refusal, shown in the editor',
      document.getElementById('err_warmedit').textContent.indexOf('exists') >= 0, document.getElementById('err_warmedit').textContent);
   setValue('warm_address', '2001:db8::25');
   before = requests.length;
   click(act('warmsave'));
   await flush();
   const warmPost = lastBody(before, 'POST', '/api/v1/warmup');
   check('Add posts the new schedule, with no start when none was typed',
      warmPost !== null && warmPost.address === '2001:db8::25' && warmPost.plan === 'standard' && !('started' in warmPost) && warmPost.enabled === true,
      JSON.stringify(warmPost));
   check('and the new schedule is drawn in the box', warmBox().textContent.indexOf('2001:db8::25') >= 0, warmBox().textContent);
   check('an address the setting binds carries no warning: one schedule of the two has it',
      warmBox().querySelectorAll('[data-warn="bind"]').length === 1, String(warmBox().querySelectorAll('[data-warn="bind"]').length));
   click(act('warmedit', { id: 7 }));
   await flush();
   confirmAnswer = false;
   before = requests.length;
   click(act('warmdel', { id: 7 }));
   await flush();
   check('Delete asks first, naming the address, and no sends nothing',
      confirmations[confirmations.length - 1].indexOf('192.0.2.10') >= 0 && called(before, 'DELETE', /warmup/).length === 0);
   confirmAnswer = true;
   before = requests.length;
   click(act('warmdel', { id: 7 }));
   await flush();
   check('yes deletes the schedule and returns to the queue without it',
      called(before, 'DELETE', '/api/v1/warmup/7').length === 1 && warmBox() !== null && warmBox().textContent.indexOf('192.0.2.10') < 0 &&
      toastText() === 'Warm-up schedule deleted', paths(before));

   // A warm-up listing the server could not answer takes nothing else with it.
   warmupFails = true;
   await goTo('dash');
   await goTo('queue');
   check('a failed warm-up listing leaves the queue drawn and says so in the box',
      rows().length >= 1 && document.getElementById('err_warmup') !== null &&
      document.getElementById('err_warmup').textContent === 'The warm-up schedules could not be read; the rest of the page is shown.' &&
      content().textContent.indexOf('No destination has asked this server to slow down.') >= 0, content().textContent.slice(0, 300));


   // ---- DANE / TLSA
   before = requests.length;
   await goTo('tlsa');
   check('the TLSA view reads the records', called(before, 'GET', '/api/v1/tlsa').length === 1);
   check('and shows them one per line', $('#tlsaPre').textContent === state.tlsa.records.map((r) => r.record).join('\n'), $('#tlsaPre').textContent);
   click(act('copy'));
   await flush();
   check('Copy all puts the block on the clipboard', clipboard === $('#tlsaPre').textContent);

   // ---- settings, drawn from the OpenAPI document
   before = requests.length;
   await goTo('settings');
   check('the settings view reads the nine groups, and the document was read once for the whole session',
      called(0, 'GET', '/api/v1/openapi.json').length === 1 &&
      ['/api/v1/settings', '/api/v1/settings/antispam', '/api/v1/settings/logging', '/api/v1/settings/antivirus', '/api/v1/settings/scripting', '/api/v1/settings/cache', '/api/v1/settings/indexing'].every((p) => called(before, 'GET', p).length === 1),
      paths(before));
   check('a string is a text box with the value', document.getElementById('set_srv_hostname').type === 'text' && document.getElementById('set_srv_hostname').value === 'mail.example.com');
   check('an integer is a number box', document.getElementById('set_srv_max_message_size_kb').type === 'number' && document.getElementById('set_srv_max_message_size_kb').value === '10240');
   check('a boolean is a checkbox in its state', document.getElementById('set_srv_service_smtp').type === 'checkbox' && document.getElementById('set_srv_service_smtp').checked === true &&
      document.getElementById('set_log_log_smtp').checked === false);
   const words = document.getElementById('set_srv_smtp_relayer_connection_security');
   check('an enumeration is a select with the document\'s words', words.tagName === 'SELECT' && words.querySelectorAll('option').map((o) => o.attributes.value).join(',') === 'none,tls,starttls_optional,starttls_required' && words.value === 'none',
      words ? words.innerHTML : 'none');
   const secret = document.getElementById('set_srv_smtp_relayer_password');
   check('a write-only key is a password box, empty, marked write-only', secret.type === 'password' && secret.value === '' && secret.closest('.fr').textContent.indexOf('write-only') >= 0);
   check('a read-only key is shown and has no control', document.getElementById('set_log_log_directory') === null && content().textContent.indexOf('/var/log/hmailserver') >= 0 &&
      $$('.fr').filter((r) => r.textContent.indexOf('log_directory') >= 0)[0].textContent.indexOf('read-only') >= 0);
   check('a key that waits for a restart is marked', document.getElementById('set_srv_service_smtp').closest('.fr').textContent.indexOf('restart') >= 0 &&
      document.getElementById('set_srv_hostname').closest('.fr').querySelector('.badge.warn') === null);
   check('the description is the hint', document.getElementById('set_srv_hostname').closest('.fr').textContent.indexOf('SMTP banner') >= 0);
   // When a saved value reaches the running server (docs/SettingsApply.md): the
   // document's x-applies is the mark beside the field, and it replaces the
   // restart badge rather than joining it.
   const tarpitRow = document.getElementById('set_spam_tarpit_count').closest('.fr');
   check('a key kept in the settings store carries its mark', !!tarpitRow.querySelector('[data-applies="now"]') &&
      tarpitRow.textContent.indexOf('Applies on save') >= 0 && tarpitRow.textContent.indexOf('restart') < 0, tarpitRow.textContent);
   check('and a key without one carries none', document.getElementById('set_srv_hostname').closest('.fr').querySelector('[data-applies]') === null);
   // A key the server's environment sets: the mark and the value, and no
   // control - the server refuses a change to it, which the next start would undo.
   const portRow = $$('.fr').filter((r) => r.querySelector('[data-environment]') && r.textContent.indexOf('clamav_port') >= 0)[0];
   check('a key the environment sets is marked, shown with its value, and has no control', !!portRow &&
      document.getElementById('set_av_clamav_port') === null && portRow.querySelector('[data-environment="variable"]') !== null &&
      portRow.textContent.indexOf('Set by the environment') >= 0 && portRow.textContent.indexOf('3310') >= 0,
      portRow ? portRow.textContent : 'no row');
   const fileRow = $$('.fr').filter((r) => r.querySelector('[data-environment="file"]'))[0];
   check('one a file supplied says so and shows no value', !!fileRow && document.getElementById('set_av_custom_scanner_executable') === null &&
      fileRow.textContent.indexOf('Set by the environment, from a file') >= 0 && fileRow.textContent.indexOf('(set)') >= 0,
      fileRow ? fileRow.textContent : 'no row');
   check('the mark is no field of its own', $$('.fr').filter((r) => r.textContent.indexOf('clamav_port_environment') >= 0).length === 0);
   check('the note under the banner names the mark', $$('.note [data-environment]').length === 1 &&
      $('#content').textContent.indexOf("is set by this server's environment") >= 0);
   setValue('set_av_clamav_host', 'scanner.example.test');
   before = requests.length;
   click(act('setsave', { group: 'av' }));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/settings/antivirus');
   // What keeps a key the environment sets out of a save is that it has no
   // control (the check above): a save sends only what was edited, and always has.
   check('saving the group sends only the key that was edited', !!put && JSON.stringify(put) === '{"clamav_host":"scanner.example.test"}', JSON.stringify(put));
   check('the settings view reads what is pending, and says it above the groups', called(before, 'GET', '/api/v1/server/settings/pending').length === 1 &&
      !!$('#pendingBar') && $('#pendingBar').textContent.indexOf('1 saved setting(s) wait for a reinitialize.') >= 0 &&
      $('#pendingBar').textContent.indexOf('ManageSieveServerPort') >= 0 && !!act('applypending'), $('#pendingBar') ? $('#pendingBar').textContent : 'no bar');
   confirmAnswer = false;
   let pendingBefore = requests.length;
   click(act('applypending'));
   await flush();
   check('Apply now asks first, and a no sends nothing', called(pendingBefore, 'POST', /reinitialize/).length === 0 &&
      confirmations[confirmations.length - 1].indexOf('Reinitialize the server now?') >= 0 &&
      confirmations[confirmations.length - 1].indexOf('draining') < 0, confirmations[confirmations.length - 1]);
   state.drain.draining = true;
   pendingBefore = requests.length;
   click(act('applypending'));
   await flush();
   check('and while the server is draining it says that a reinitialize ends maintenance mode', called(pendingBefore, 'GET', '/api/v1/server/drain').length === 1 &&
      called(pendingBefore, 'POST', /reinitialize/).length === 0 && confirmations[confirmations.length - 1].indexOf('ends maintenance mode') >= 0,
      confirmations[confirmations.length - 1]);
   state.drain.draining = false;
   confirmAnswer = true;
   check('the note under the banner names the three marks and where Apply now is',
      document.querySelectorAll('.note [data-applies]').length === 3 && $('#content').textContent.indexOf('which Apply now above does when one waits') >= 0);


   before = requests.length;
   click(act('setsave', { group: 'srv' }));
   await flush();
   check('saving with nothing changed sends nothing', called(before, 'PUT', /settings/).length === 0 && toastText() === 'Nothing changed', toastText());
   setValue('set_srv_max_message_size_kb', '20480');
   before = requests.length;
   click(act('setsave', { group: 'srv' }));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/settings');
   check('saving sends only the key that changed, as a number', !!put && JSON.stringify(put) === '{"max_message_size_kb":20480}', JSON.stringify(put));
   check('then reads the groups again', called(before, 'GET', '/api/v1/settings').length === 1);
   check('and the control shows what the server holds', document.getElementById('set_srv_max_message_size_kb').value === '20480' && toastText() === '1 setting saved', toastText());
   setValue('set_srv_smtp_relayer_password', 'hunter2');
   setChecked('set_srv_service_smtp', false);
   before = requests.length;
   click(act('setsave', { group: 'srv' }));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/settings');
   check('a filled password goes with the change, and nothing unchanged', !!put && put.smtp_relayer_password === 'hunter2' && put.service_smtp === false && Object.keys(put).length === 2, JSON.stringify(put));
   check('the password box is empty again after the re-read', document.getElementById('set_srv_smtp_relayer_password').value === '' && document.getElementById('set_srv_service_smtp').checked === false);
   setValue('set_spam_spam_mark_threshold', 'five');
   before = requests.length;
   click(act('setsave', { group: 'spam' }));
   await flush();
   check('a number that is not a number is refused by the page', called(before, 'PUT', /antispam/).length === 0 && document.getElementById('err_set_spam').textContent.indexOf('whole number') >= 0,
      document.getElementById('err_set_spam').textContent);
   setValue('set_spam_spam_mark_threshold', '7');
   nextRefusal = 'The delete threshold must be above the mark threshold.';
   click(act('setsave', { group: 'spam' }));
   await flush();
   check('a refusal is the server\'s sentence beside the group', document.getElementById('err_set_spam').textContent === 'The delete threshold must be above the mark threshold.');
   const filter = $('#setFilter');
   filter.value = 'relayer';
   filter.dispatchEvent(makeEvent('input'));
   check('the filter hides the rows that do not match', document.getElementById('set_srv_hostname').closest('.fr').hidden === true && document.getElementById('set_srv_smtp_relayer_host').closest('.fr').hidden === false);
   filter.value = '';
   filter.dispatchEvent(makeEvent('input'));
   check('and clearing it shows them again', document.getElementById('set_srv_hostname').closest('.fr').hidden === false);

   // ---- the three groups with verbs beside them
   check('the cache group draws its counters read-only and its ceiling and life as number boxes', document.getElementById('set_cache_domain_cache_size_kb') === null &&
      document.getElementById('set_cache_domain_hit_rate') === null && content().textContent.indexOf('97') >= 0 &&
      document.getElementById('set_cache_domain_cache_max_size_kb').type === 'number' && document.getElementById('set_cache_domain_cache_max_size_kb').value === '10240' &&
      document.getElementById('set_cache_domain_cache_ttl').value === '3600' && document.getElementById('set_cache_enabled').checked === true);
   check('the scripting and indexing groups are drawn with their switches and the read-only facts', document.getElementById('set_script_enabled').checked === false && document.getElementById('set_script_language').value === 'VBScript' &&
      document.getElementById('set_script_current_script_file') === null && content().textContent.indexOf('EventHandlers.vbs') >= 0 &&
      document.getElementById('set_index_enabled').checked === true && document.getElementById('set_index_total_indexed_count') === null && content().textContent.indexOf('40') >= 0);
   check('each verb is a button worded from the route, with the route\'s sentence beside it', act('setpost', { group: 'script', path: '/api/v1/settings/scripting/reload' }) !== null &&
      act('setpost', { group: 'script', path: '/api/v1/settings/scripting/check' }) !== null && act('setpost', { group: 'cache', path: '/api/v1/settings/cache/clear' }) !== null &&
      act('setpost', { group: 'index', path: '/api/v1/settings/indexing/index' }) !== null && act('setpost', { group: 'index', path: '/api/v1/settings/indexing/clear' }) !== null &&
      content().textContent.indexOf('What Settings.Cache.Clear does over COM') >= 0);
   setValue('set_cache_domain_cache_max_size_kb', '2048');
   before = requests.length;
   click(act('setsave', { group: 'cache' }));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/settings/cache');
   check('saving the cache group sends only the ceiling that changed, and never a counter', JSON.stringify(put) === '{"domain_cache_max_size_kb":2048}', JSON.stringify(put));
   before = requests.length;
   click(act('setpost', { group: 'cache', path: '/api/v1/settings/cache/clear' }));
   await flush();
   check('Empty the caches posts to the verb and re-reads the group, so the counters follow', called(before, 'POST', '/api/v1/settings/cache/clear').length === 1 && called(before, 'GET', '/api/v1/settings/cache').length === 1 &&
      toastText() === 'Empty the caches: done' && $$('.fr').filter((r) => r.textContent.indexOf('domain_hit_rate') >= 0)[0].textContent.indexOf('97') < 0, toastText());
   check('the Web Push group is drawn from its schema, with the private key a field to write and the public key a fact to read',
      document.getElementById('err_set_push') !== null && $$('.fr').some((r) => r.textContent.indexOf('vapid_private_key') >= 0) &&
      $$('.fr').some((r) => r.textContent.indexOf('vapid_public_key') >= 0), $$('.fr').map((r) => r.textContent.slice(0, 40)).join('|'));
   before = requests.length;
   click(act('setpost', { group: 'push', path: '/api/v1/settings/push/vapid' }));
   await flush();
   check('Make a new VAPID key pair posts to the verb and re-reads the group, so the public key follows',
      called(before, 'POST', '/api/v1/settings/push/vapid').length === 1 && called(before, 'GET', '/api/v1/settings/push').length === 1 &&
      $$('.fr').filter((r) => r.textContent.indexOf('vapid_public_key') >= 0)[0].textContent.indexOf('BNewPublicKeyOfTheServer') >= 0, toastText());
   state.scriptProblem = 'Line 12: expected end of statement';
   before = requests.length;
   click(act('setpost', { group: 'script', path: '/api/v1/settings/scripting/check' }));
   await flush();
   check('Check the syntax shows the parser\'s message beside the group and changes nothing', called(before, 'POST', '/api/v1/settings/scripting/check').length === 1 &&
      document.getElementById('err_set_script').textContent === 'Line 12: expected end of statement' && called(before, 'GET', /settings/).length === 0, document.getElementById('err_set_script').textContent);
   state.scriptProblem = '';
   click(act('setpost', { group: 'script', path: '/api/v1/settings/scripting/check' }));
   await flush();
   check('and says so when it parses', document.getElementById('err_set_script').textContent === '' && toastText() === 'The script parses.', toastText());
   // The breach check's endpoint test, beside the server group whose keys it tests.
   check('the server group carries the breach check endpoint test as a verb', act('setpost', { group: 'srv', path: '/api/v1/settings/breach-check/test' }) !== null &&
      act('setpost', { group: 'srv', path: '/api/v1/settings/breach-check/test' }).textContent === 'Test the breach check endpoint');
   before = requests.length;
   click(act('setpost', { group: 'srv', path: '/api/v1/settings/breach-check/test' }));
   await flush();
   check('an endpoint that answered is a toast with the entries and the padding, and the group is not read again', called(before, 'POST', '/api/v1/settings/breach-check/test').length === 1 &&
      toastText() === 'The endpoint answered with 812 entries, padded.' && called(before, 'GET', /settings/).length === 0 && document.getElementById('err_set_srv').textContent === '', toastText());
   state.breachProbe = { reachable: true, status: 200, entries: 3, padded: false, milliseconds: 9 };
   click(act('setpost', { group: 'srv', path: '/api/v1/settings/breach-check/test' }));
   await flush();
   check('an answer with no padding says so', toastText() === 'The endpoint answered with 3 entries, not padded.', toastText());
   state.breachProbe = { reachable: false, status: 0, error: 'The request to mirror.example failed: connection refused', milliseconds: 2003 };
   click(act('setpost', { group: 'srv', path: '/api/v1/settings/breach-check/test' }));
   await flush();
   check('an endpoint that could not be used shows why beside the group', document.getElementById('err_set_srv').textContent === 'The endpoint could not be used: The request to mirror.example failed: connection refused',
      document.getElementById('err_set_srv').textContent);
   state.breachProbe = { reachable: true, status: 200, entries: 812, padded: true, milliseconds: 41 };
   before = requests.length;
   click(act('setpost', { group: 'script', path: '/api/v1/settings/scripting/reload' }));
   await flush();
   check('Reload the script posts to its route', called(before, 'POST', '/api/v1/settings/scripting/reload').length === 1 && state.scriptReloads === 1);
   before = requests.length;
   click(act('setpost', { group: 'index', path: '/api/v1/settings/indexing/index' }));
   await flush();
   check('Index now posts and the re-read count follows', called(before, 'POST', '/api/v1/settings/indexing/index').length === 1 && $$('.fr').filter((r) => r.textContent.indexOf('total_indexed_count') >= 0)[0].textContent.indexOf('45') >= 0);
   nextRefusal = 'The index is being rebuilt already.';
   click(act('setpost', { group: 'index', path: '/api/v1/settings/indexing/clear' }));
   await flush();
   check('a refused verb is the server\'s sentence beside its group', document.getElementById('err_set_index').textContent === 'The index is being rebuilt already.');

   // ---- the bounce settings, a group like the others with the sweep beside it
   const action = document.getElementById('set_bounce_suppression_action');
   check('the bounce group is drawn from its schema, the action as a select of its three words', action !== null && action.tagName === 'SELECT' &&
      action.querySelectorAll('option').map((o) => o.attributes.value).join(',') === 'refuse,drop,observe' && action.value === 'refuse' &&
      document.getElementById('set_bounce_processing_enabled').checked === false && document.getElementById('set_bounce_hard_bounce_threshold').value === '1',
      action ? action.innerHTML : 'no select');
   setChecked('set_bounce_processing_enabled', true);
   setValue('set_bounce_suppression_action', 'observe');
   before = requests.length;
   click(act('setsave', { group: 'bounce' }));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/settings/bounces');
   check('saving the bounce group sends the switch and the word that changed', JSON.stringify(put) === '{"processing_enabled":true,"suppression_action":"observe"}', JSON.stringify(put));
   before = requests.length;
   click(act('setpost', { group: 'bounce', path: '/api/v1/bounces/expire' }));
   await flush();
   check('Expire and prune now posts to the sweep', called(before, 'POST', '/api/v1/bounces/expire').length === 1 && state.bounceSweeps === 1 && toastText() === 'Expire and prune now: done', toastText());

   // ---- backup: a settings group of its own, beside the manager's status
   before = requests.length;
   await goTo('backup');
   check('the backup view reads its group and the status', called(before, 'GET', '/api/v1/settings/backup').length === 1 && called(before, 'GET', '/api/v1/backup').length === 1 && $('#viewTitle').textContent === 'Backup', paths(before));
   check('the settings are drawn from the document: the folder, the switches, the read-only log path', document.getElementById('set_backup_destination').value === '/var/backups/hmailserver' &&
      document.getElementById('set_backup_backup_domains').checked === true && document.getElementById('set_backup_compress').checked === false && document.getElementById('set_backup_log_file') === null &&
      content().textContent.indexOf('/var/log/hmailserver/hmailserver_backup.log') >= 0);
   check('the status card shows the log\'s tail and no failure', $('#backupLog').textContent.indexOf('Backup completed.') >= 0 && content().textContent.indexOf('No failure recorded') >= 0);
   check('and says where the schedule is', content().textContent.indexOf('ScheduledBackupTime') >= 0);
   // The schedule's keys are settings the server keeps in its database: a
   // hand edit of hMailServer.ini is put back, so the note names where they
   // are set - the Control Panel's card and the REST route - not the file.
   check('and names where the schedule is set, not a file the server puts back when edited',
      content().textContent.indexOf('ScheduledBackupMaxAgeDays') >= 0 && content().textContent.indexOf('Scheduled backups in the Control Panel') >= 0 &&
      content().textContent.indexOf('PUT /api/v1/settings/ini/') >= 0 && content().textContent.indexOf('hMailServer.ini') < 0,
      content().textContent.slice(Math.max(0, content().textContent.indexOf('ScheduledBackupTime') - 40), content().textContent.indexOf('ScheduledBackupTime') + 260));
   setValue('set_backup_destination', '/mnt/backup');
   setChecked('set_backup_compress', true);
   before = requests.length;
   click(act('setsave', { group: 'backup' }));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/settings/backup');
   check('saving the backup settings sends only what changed', JSON.stringify(put) === '{"destination":"/mnt/backup","compress":true}', JSON.stringify(put));
   check('then re-reads the group on this view, not the settings view', called(before, 'GET', '/api/v1/settings/backup').length === 1 && called(before, 'GET', '/api/v1/settings').length === 0 &&
      $('#viewTitle').textContent === 'Backup' && document.getElementById('set_backup_destination').value === '/mnt/backup' && document.getElementById('set_backup_compress').checked === true && toastText() === '2 settings saved', toastText());
   before = requests.length;
   click(act('backupstart'));
   await flush();
   check('Start backup now posts and re-reads the status', called(before, 'POST', '/api/v1/backup').length === 1 && called(before, 'GET', '/api/v1/backup').length === 1 && toastText() === 'Backup started' &&
      $('#backupLog').textContent.indexOf('2026-09-14 11:00:00 Backup started.') >= 0, toastText());
   click(act('backupstart'));
   await flush();
   check('a backup already running is the server\'s refusal beside the button', document.getElementById('err_backupstart').textContent === 'the backup did not start');
   state.backupRunning = false;
   state.backup.status = 'The destination directory could not be written.';
   before = requests.length;
   click(act('backupstatus'));
   await flush();
   check('Re-read status reads it again and shows the manager\'s last failure', called(before, 'GET', '/api/v1/backup').length === 1 && content().textContent.indexOf('The destination directory could not be written.') >= 0);
   state.backup.status = '';

   // ---- the passphrase, and the restore that is the only one a Linux server has
   check('the passphrase is a password box, the flag beside it is not drawn, and what losing it costs is said',
      document.getElementById('set_backup_encryption_passphrase') !== null &&
      document.getElementById('set_backup_encryption_passphrase').type === 'password' &&
      document.getElementById('set_backup_encryption_passphrase_set') === null &&
      content().textContent.indexOf('can be recovered, by anybody') >= 0);
   check('and Stop encrypting is offered only once one is set', $$('[data-act="backupclearpass"]').length === 0);

   setValue('set_backup_encryption_passphrase', 'correct horse battery staple');
   before = requests.length;
   click(act('setsave', { group: 'backup' }));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/settings/backup');
   check('saving sends the passphrase that was typed, and nothing else', JSON.stringify(put) === '{"encryption_passphrase":"correct horse battery staple"}', JSON.stringify(put));

   SETTING_GROUPS['/api/v1/settings/backup'].values.encryption_passphrase_set = true;
   click(act('backupstatus'));
   await flush();
   check('once one is set, Stop encrypting appears', act('backupclearpass') !== null);
   confirmAnswer = false;
   before = requests.length;
   click(act('backupclearpass'));
   await flush();
   check('and asks before it clears anything', called(before, 'PUT', '/api/v1/settings/backup').length === 0 &&
      confirmations[confirmations.length - 1].indexOf('still need their passphrase') >= 0);
   confirmAnswer = true;
   before = requests.length;
   click(act('backupclearpass'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/settings/backup');
   check('a yes clears it with an empty passphrase', JSON.stringify(put) === '{"encryption_passphrase":""}', JSON.stringify(put));
   SETTING_GROUPS['/api/v1/settings/backup'].values.encryption_passphrase_set = false;
   click(act('backupstatus'));
   await flush();

   check('the restore form is drawn from the route\'s own schema', document.getElementById('restore_file') !== null &&
      document.getElementById('restore_passphrase').type === 'password' && document.getElementById('restore_restore_domains') !== null);

   before = requests.length;
   click(act('backuprestore'));
   await flush();
   check('a restore with no archive named is refused by the page, without a request',
      document.getElementById('err_backuprestore').textContent.indexOf('path of the archive') >= 0 &&
      called(before, 'POST', '/api/v1/backup/restore').length === 0);

   setValue('restore_file', '/var/backups/hmailserver/HMBackup 2026-09-15 020000.7z');
   before = requests.length;
   click(act('backuprestore'));
   await flush();
   check('nor with nothing chosen to restore', document.getElementById('err_backuprestore').textContent.indexOf('Choose what to restore') >= 0 &&
      called(before, 'POST', '/api/v1/backup/restore').length === 0);

   setChecked('restore_restore_domains', true);
   setChecked('restore_restore_messages', true);
   confirmAnswer = false;
   before = requests.length;
   click(act('backuprestore'));
   await flush();
   check('a restore that is not confirmed sends nothing', called(before, 'POST', '/api/v1/backup/restore').length === 0 &&
      confirmations[confirmations.length - 1].indexOf('is replaced') >= 0);

   confirmAnswer = true;
   before = requests.length;
   click(act('backuprestore'));
   await flush();
   check('an encrypted archive with no passphrase shows the server\'s own sentence',
      document.getElementById('err_backuprestore').textContent.indexOf('is encrypted, and no passphrase was given') >= 0);

   setValue('restore_passphrase', 'not the passphrase');
   before = requests.length;
   click(act('backuprestore'));
   await flush();
   check('and a wrong one shows the server\'s sentence for that',
      document.getElementById('err_backuprestore').textContent.indexOf('could not be opened with the passphrase given') >= 0);

   setValue('restore_passphrase', 'correct horse battery staple');
   before = requests.length;
   click(act('backuprestore'));
   await flush();
   const restoreSent = lastBody(before, 'POST', '/api/v1/backup/restore');
   check('and with it, the restore is sent as the route takes it', JSON.stringify(restoreSent) ===
      '{"file":"/var/backups/hmailserver/HMBackup 2026-09-15 020000.7z","passphrase":"correct horse battery staple","restore_settings":false,"restore_domains":true,"restore_messages":true}',
      JSON.stringify(restoreSent));
   check('the page says so and reads the view again', toastText() === 'Restore started' && called(before, 'GET', '/api/v1/backup').length === 1, toastText());

   // ---- one mailbox, or one message, from an archive in the destination
   check('the backup view read the archive listing for its two pickers, and drew the archives newest first with the encrypted one marked',
      requests.filter((r) => r.method === 'GET' && r.path === '/api/v1/backup/archives').length >= 1 && $$('#mbr_archive option').length === 2 &&
      $$('#mbr_archive option')[0].value === 'HMBackup 2026-09-21 020000.7z' && $$('#mbr_archive option')[0].textContent.indexOf('(encrypted)') >= 0 &&
      $$('#mbm_archive option').length === 2 && document.getElementById('mbr_mode_merge').checked === true, paths(0));
   check('the two cards say what the routes say', content().textContent.indexOf('Merge adds what is missing; replace empties every live folder first.') >= 0 &&
      content().textContent.indexOf('each message has the id the restore route takes') >= 0);

   before = requests.length;
   click(act('mailboxrestore'));
   await flush();
   check('a mailbox restore with no address is refused by the page, without a request',
      document.getElementById('err_mailboxrestore').textContent.indexOf('address of the account') >= 0 &&
      called(before, 'POST', /^\/api\/v1\/backup\//).length === 0);

   setValue('mbr_address', 'ann@example.test');
   setValue('mbr_archive', 'HMBackup 2026-09-20 020000.7z');
   setChecked('mbr_mode_replace', true);
   confirmAnswer = false;
   before = requests.length;
   click(act('mailboxrestore'));
   await flush();
   check('replace asks in the words that say what it destroys, and a no sends nothing',
      confirmations[confirmations.length - 1].indexOf('Every folder is emptied first') >= 0 &&
      confirmations[confirmations.length - 1].indexOf('ann@example.test') >= 0 && called(before, 'POST', /^\/api\/v1\/backup\//).length === 0);

   confirmAnswer = true;
   before = requests.length;
   click(act('mailboxrestore'));
   await flush();
   let mailboxSent = lastBody(before, 'POST', '/api/v1/backup/HMBackup%202026-09-20%20020000.7z/mailboxes/ann%40example.test/restore');
   check('a yes sends the restore to the archive and address, encoded, with the mode and no passphrase', JSON.stringify(mailboxSent) === '{"mode":"replace"}', JSON.stringify(mailboxSent));
   check('then reads the job\'s outcome and shows its counts', called(before, 'GET', /^\/api\/v1\/backup\/restore-mailbox\//).length === 1 && toastText() === 'Mailbox restore started' &&
      document.getElementById('mbr_outcome_text').textContent.indexOf('3 folders, 12 messages added, 0 skipped as still in the mailbox, 0 failed') >= 0, toastText());

   setChecked('mbr_mode_replace', false);
   setChecked('mbr_mode_merge', true);
   setValue('mbr_archive', 'HMBackup 2026-09-21 020000.7z');
   before = requests.length;
   click(act('mailboxrestore'));
   await flush();
   check('merge asks in its own words, and an encrypted archive with no passphrase shows the server\'s own sentence',
      confirmations[confirmations.length - 1].indexOf('Nothing already there is removed') >= 0 &&
      document.getElementById('err_mailboxrestore').textContent.indexOf('is encrypted, and no passphrase was given') >= 0);

   setValue('mbr_passphrase', 'correct horse battery staple');
   before = requests.length;
   click(act('mailboxrestore'));
   await flush();
   mailboxSent = lastBody(before, 'POST', '/api/v1/backup/HMBackup%202026-09-21%20020000.7z/mailboxes/ann%40example.test/restore');
   check('with it, merge is sent with the passphrase, and the box is emptied afterwards', JSON.stringify(mailboxSent) === '{"mode":"merge","passphrase":"correct horse battery staple"}' &&
      document.getElementById('mbr_passphrase').value === '', JSON.stringify(mailboxSent));

   before = requests.length;
   click(act('mailboxrestorestatus'));
   await flush();
   check('Re-read the outcome reads the job again', called(before, 'GET', /^\/api\/v1\/backup\/restore-mailbox\//).length === 1);

   // The one-message card: the listing, then one row put back into a named folder.
   setValue('mbm_address', 'nobody@example.test');
   setValue('mbm_archive', 'HMBackup 2026-09-20 020000.7z');
   before = requests.length;
   click(act('mailboxlist'));
   await flush();
   check('an account the archive does not hold is the server\'s 404 beside the button',
      document.getElementById('err_mailboxlist').textContent.indexOf('has no account nobody@example.test') >= 0);

   setValue('mbm_address', 'ann@example.test');
   before = requests.length;
   click(act('mailboxlist'));
   await flush();
   check('the listing is asked for by archive and address, and drawn one row per message with its folder, subject, sender and a Restore button',
      called(before, 'GET', '/api/v1/backup/HMBackup%202026-09-20%20020000.7z/mailboxes/ann%40example.test').length === 1 &&
      $$('#mbm_list tbody tr').length === 3 && $$('#mbm_list tbody tr')[2].textContent.indexOf('Projects.Alpha') >= 0 &&
      $$('#mbm_list tbody tr')[0].textContent.indexOf('The Tuesday figures') >= 0 && $$('#mbm_list tbody tr')[0].textContent.indexOf('bob@example.test') >= 0 &&
      $$('[data-act="messagerestore"]').length === 3 && $$('[data-act="messagerestore"]')[1].dataset.id === '4102', paths(before));

   setValue('mbm_folder', 'Restored');
   confirmAnswer = false;
   before = requests.length;
   click($$('[data-act="messagerestore"]')[1]);
   await flush();
   check('a message restore asks first, and a no sends nothing', confirmations[confirmations.length - 1].indexOf('Put this message back') >= 0 &&
      called(before, 'POST', /^\/api\/v1\/backup\//).length === 0);

   confirmAnswer = true;
   before = requests.length;
   click($$('[data-act="messagerestore"]')[1]);
   await flush();
   const messageSent = lastBody(before, 'POST', '/api/v1/backup/HMBackup%202026-09-20%20020000.7z/mailboxes/ann%40example.test/restore');
   check('a yes sends that message\'s id and the folder typed', JSON.stringify(messageSent) === '{"message":4102,"folder":"Restored"}', JSON.stringify(messageSent));
   check('and shows the job\'s outcome', toastText() === 'Message restore started' && document.getElementById('mbr_outcome_text').textContent.indexOf('1 folders, 1 messages added') >= 0, toastText());

   state.backupRunning = true;
   before = requests.length;
   click($$('[data-act="messagerestore"]')[0]);
   await flush();
   check('a restore while a backup runs is the server\'s 409 beside the list', document.getElementById('err_mailboxlist').textContent.indexOf('already running') >= 0);
   state.backupRunning = false;

   // ---- the server messages
   before = requests.length;
   await goTo('messages');
   check('the messages view reads the messages', called(before, 'GET', '/api/v1/settings/messages').length === 1 && $('#viewTitle').textContent === 'Server messages', paths(before));
   check('one row per message, the name as the caption and the text in a text area with its own Save', $$('.fr').length === 2 && $$('.fr')[0].textContent.indexOf('BOUNCE_MESSAGE') >= 0 &&
      document.getElementById('msg_0').tagName === 'TEXTAREA' && document.getElementById('msg_0').value === 'Your message could not be delivered.' &&
      act('msgsave', { name: 'VIRUS_NOTIFICATION' }) !== null && content().textContent.indexOf('The name is the table\'s own') >= 0);
   before = requests.length;
   click(act('msgsave', { name: 'BOUNCE_MESSAGE' }));
   await flush();
   check('saving an unchanged text sends nothing', called(before, 'PUT', /messages/).length === 0 && toastText() === 'Nothing changed');
   setValue('msg_1', 'A virus was found in a message sent to you, and it was removed.');
   before = requests.length;
   click(act('msgsave', { name: 'VIRUS_NOTIFICATION' }));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/settings/messages/VIRUS_NOTIFICATION');
   check('a changed text is PUT by name, as text alone', JSON.stringify(put) === '{"text":"A virus was found in a message sent to you, and it was removed."}', JSON.stringify(put));
   check('and the messages are read again with the change', called(before, 'GET', '/api/v1/settings/messages').length === 1 && document.getElementById('msg_1').value.indexOf('it was removed') >= 0 && toastText() === 'Message saved: VIRUS_NOTIFICATION');
   setValue('msg_0', 'Undeliverable.');
   nextRefusal = 'text is not valid UTF-8';
   click(act('msgsave', { name: 'BOUNCE_MESSAGE' }));
   await flush();
   check('a refusal is the server\'s sentence under the row, with the edit kept', document.getElementById('err_msg_0').textContent === 'text is not valid UTF-8' && document.getElementById('msg_0').value === 'Undeliverable.');
   const messageFilter = $('#msgFilter');
   messageFilter.value = 'virus';
   messageFilter.dispatchEvent(makeEvent('input'));
   check('the filter hides the messages that do not match', $$('.fr')[0].hidden === true && $$('.fr')[1].hidden === false);

   // ---- rules
   before = requests.length;
   await goTo('rules');
   check('the rules view reads the rules and the routes, and the document once only', called(before, 'GET', '/api/v1/rules').length === 1 && called(before, 'GET', '/api/v1/routes').length === 1 &&
      called(0, 'GET', '/api/v1/openapi.json').length === 1, paths(before));
   check('a rule row says when and then', rows().length === 1 && rows()[0].textContent.indexOf('Subject contains') >= 0 && rows()[0].textContent.indexOf('[SPAM]') >= 0 && rows()[0].textContent.indexOf('Junk') >= 0,
      rows()[0].textContent);
   // ---- a domain's rules, chosen at the top of the page
   before = requests.length;
   const scope = content().querySelector('[data-r="rule-scope"]');
   check('the rules page offers the server and every domain as a scope', scope !== null && scope.querySelectorAll('option').map((o) => o.attributes.value).join(',') === ',' + state.domains.map((d) => d.name).join(','), scope ? scope.querySelectorAll('option').map((o) => o.attributes.value).join(',') : 'no select');
   scope.value = 'second.example';
   scope.dispatchEvent(makeEvent('change'));
   await flush();
   check('choosing a domain reads that domain\'s rules and no longer the server\'s',
      called(before, 'GET', '/api/v1/domains/second.example/rules').length === 1 && called(before, 'GET', '/api/v1/rules').length === 0 &&
      content().textContent.indexOf('No rules are defined for this domain.') >= 0, paths(before));
   click(act('rulenew'));
   await flush();
   $('#ruleName').value = 'Domain rule';
   before = requests.length;
   click(act('rulesave'));
   await flush();
   check('a rule made under a domain is posted to the domain\'s route', called(before, 'POST', '/api/v1/domains/second.example/rules').length === 1 && called(before, 'POST', '/api/v1/rules').length === 0, paths(before));
   content().querySelector('[data-r="rule-scope"]').value = '';
   content().querySelector('[data-r="rule-scope"]').dispatchEvent(makeEvent('change'));
   await flush();
   check('and the server\'s rules come back', called(before, 'GET', '/api/v1/rules').length === 1 && rows().length === 1, paths(before));

   click(act('rulenew'));
   await flush();
   check('New rule opens an empty editor', $('#ruleName') !== null && $('#ruleName').value === '' && $('#ruleActive').checked === true && content().querySelectorAll('[data-r="crit-field"]').length === 0);
   click(act('critadd'));
   await flush();
   const field = content().querySelector('[data-r="crit-field"]');
   check('a criterion\'s fields are the words of the document\'s sentence', field !== null && field.querySelectorAll('option').map((o) => o.attributes.value).join(',') === 'from,to,cc,subject,body,message_size,recipient_list,delivery_attempts,attachment_name,sensitive_content,attachment_type,external_recipient,content,header',
      field ? field.innerHTML : 'none');
   check('and its matches', content().querySelector('[data-r="crit-match"]').querySelectorAll('option').map((o) => o.attributes.value).join(',') === 'equals,not_equals,contains,not_contains,less_than,greater_than,regex,wildcard');
   check('the header name is hidden until the field is header', content().querySelector('[data-r="crit-header"]').hidden === true);
   field.value = 'header';
   field.dispatchEvent(makeEvent('change'));
   await flush();
   check('choosing header shows the header name', content().querySelector('[data-r="crit-header"]').hidden === false);
   content().querySelector('[data-r="crit-header"]').value = 'X-Spam-Flag';
   content().querySelector('[data-r="crit-match"]').value = 'equals';
   content().querySelector('[data-r="crit-value"]').value = 'YES';
   click(act('actadd'));
   await flush();
   let type = content().querySelector('[data-r="act-type"]');
   check('an action\'s types are the document\'s', type !== null && type.querySelectorAll('option').map((o) => o.attributes.value).join(',') === 'forward,reply,move_to_folder,script_function,set_header,send_using_route,bind_to_address,add_disclaimer,reject,warn_sender,require_tls,quarantine,delete,stop,copy',
      type ? type.innerHTML : 'none');
   // ---- the outbound content policy's action, and the bracketed prose after a parameter
   const actParams = () => content().querySelectorAll('[data-r]').filter((el) => el.dataset.r.indexOf('act-') === 0 && el.dataset.r !== 'act-type').map((el) => el.dataset.r).join(',');
   type = content().querySelector('[data-r="act-type"]');
   type.value = 'warn_sender';
   type.dispatchEvent(makeEvent('change'));
   await flush();
   check('warn_sender offers its text as the one value input', actParams() === 'act-value',
      actParams());
   type = content().querySelector('[data-r="act-type"]');
   type.value = 'reject';
   type.dispatchEvent(makeEvent('change'));
   await flush();
   check('the words in brackets after reject\'s value are prose, not parameters', actParams() === 'act-value',
      actParams());
   type = content().querySelector('[data-r="act-type"]');
   type.value = 'add_disclaimer';
   type.dispatchEvent(makeEvent('change'));
   await flush();
   check('add_disclaimer takes its body alone, as a text area', actParams() === 'act-body' && content().querySelector('[data-r="act-body"]').tagName === 'TEXTAREA',
      actParams());
   type = content().querySelector('[data-r="act-type"]');
   type.value = 'send_using_route';
   type.dispatchEvent(makeEvent('change'));
   await flush();
   const routeChoice = content().querySelector('[data-r="act-route_id"]');
   check('send_using_route offers the routes by name', routeChoice !== null && routeChoice.tagName === 'SELECT' && routeChoice.textContent.indexOf('partner.example') >= 0, routeChoice ? routeChoice.innerHTML : 'none');
   routeChoice.value = '5';
   click(act('actadd'));
   await flush();
   const secondType = content().querySelector('[data-r="act-type"][data-i="1"]');
   secondType.value = 'forward';
   secondType.dispatchEvent(makeEvent('change'));
   await flush();
   const abortBox = content().querySelector('[data-r="act-abort_spam_flagged"][data-i="1"]');
   check('a forward action offers its abort flag as a checkbox with the desktop\'s caption, off', abortBox !== null && abortBox.type === 'checkbox' && abortBox.checked === false &&
      abortBox.closest('label').textContent.indexOf('Abort on messages marked as spam') >= 0 && content().querySelector('[data-r="act-abort_spam_flagged"][data-i="0"]') === null,
      abortBox ? 'found' : 'no checkbox');
   content().querySelector('[data-r="act-to"][data-i="1"]').value = 'copy@example.test';
   abortBox.checked = true;
   $('#ruleName').value = 'Flagged via partner';
   $('#ruleAll').value = 'any';
   before = requests.length;
   click(act('rulesave'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/rules');
   check('saving a new rule posts the shape the route takes, the flag as a boolean', !!posted && posted.name === 'Flagged via partner' && posted.active === true && posted.all_criteria === false &&
      JSON.stringify(posted.criteria) === '[{"field":"header","match":"equals","value":"YES","header":"X-Spam-Flag"}]' &&
      JSON.stringify(posted.actions) === '[{"type":"send_using_route","route_id":5},{"type":"forward","to":"copy@example.test","abort_spam_flagged":true}]', JSON.stringify(posted));
   check('and returns to the re-read list with the new rule', called(before, 'GET', '/api/v1/rules').length === 1 && rows().length === 2 && rows()[1].textContent.indexOf('Flagged via partner') >= 0);
   click(act('ruleedit', { id: 1 }));
   await flush();
   check('editing shows the rule\'s own values', $('#ruleName').value === 'Spam to junk' && content().querySelector('[data-r="crit-value"]').value === '[SPAM]' && content().querySelector('[data-r="act-folder"]').value === 'Junk');
   $('#ruleName').value = 'Spam to Junk folder';
   nextRefusal = 'A rule named that already exists.';
   before = requests.length;
   click(act('rulesave'));
   await flush();
   check('a refused save keeps the editor open with the server\'s sentence', $('#ruleName') !== null && document.getElementById('err_ruleedit').textContent === 'A rule named that already exists.');
   click(act('rulesave'));
   await flush();
   const replaced = lastBody(before, 'PUT', '/api/v1/rules/1');
   check('saving an existing rule PUTs it by id', !!replaced && replaced.name === 'Spam to Junk folder' && JSON.stringify(replaced.actions) === '[{"type":"move_to_folder","folder":"Junk"}]', JSON.stringify(replaced));
   check('the list shows the new name', rows()[0].textContent.indexOf('Spam to Junk folder') >= 0);
   before = requests.length;
   click(act('ruledel', { id: 1 }));
   await flush();
   check('deleting a rule names it in the question and deletes by id', confirmations[confirmations.length - 1].indexOf('Spam to Junk folder') >= 0 && called(before, 'DELETE', '/api/v1/rules/1').length === 1 && rows().length === 1);
   click(act('rulenew'));
   await flush();
   click(act('cancel'));
   await flush();
   check('Cancel returns to the list', $('#ruleName') === null && rows().length === 1);

   // ---- routes
   before = requests.length;
   await goTo('routes');
   check('the routes view reads the routes', called(before, 'GET', '/api/v1/routes').length === 1 && rows().length === 1);
   check('a route row shows the target, its security and its authentication', rows()[0].textContent.indexOf('smtp.partner.example:587') >= 0 && rows()[0].textContent.indexOf('relay') >= 0 && rows()[0].textContent.indexOf('every recipient') >= 0,
      rows()[0].textContent);
   click(act('routenew'));
   await flush();
   check('a new route starts at the defaults the document states', document.getElementById('route_target_smtp_port').value === '25' && document.getElementById('route_number_of_tries').value === '3' &&
      document.getElementById('route_all_addresses').checked === true && document.getElementById('route_relayer_requires_authentication').checked === false && document.getElementById('route_connection_security').value === 'none',
      'port=' + document.getElementById('route_target_smtp_port').value + ' all=' + document.getElementById('route_all_addresses').checked);
   check('the COM spelling of the recipient flag is not drawn twice', document.getElementById('route_treat_security_as_local_domain') === null && document.getElementById('route_treat_recipient_as_local_domain') !== null);
   check('the required keys are marked', document.getElementById('route_domain_name').closest('.fr').textContent.indexOf('required') >= 0 && document.getElementById('route_description').closest('.fr').textContent.indexOf('required') < 0);
   setValue('route_domain_name', 'other.example');
   setValue('route_target_smtp_host', 'mx.other.example');
   setChecked('route_all_addresses', false);
   setValue('route_addresses', 'a@other.example\n\nb@other.example\n');
   before = requests.length;
   click(act('routesave'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/routes');
   check('saving a new route posts the form with the defaults, the list as an array, and no password', !!posted && posted.domain_name === 'other.example' && posted.target_smtp_port === 25 && posted.all_addresses === false &&
      JSON.stringify(posted.addresses) === '["a@other.example","b@other.example"]' && !('relayer_auth_password' in posted) && !('treat_security_as_local_domain' in posted), JSON.stringify(posted));
   check('and the re-read list has it', rows().length === 2 && rows()[1].textContent.indexOf('2 addresses') >= 0, rows().map((r) => r.textContent).join(' | '));
   click(act('routeedit', { id: 5 }));
   await flush();
   check('editing a route shows its values and an empty password box', document.getElementById('route_target_smtp_host').value === 'smtp.partner.example' && document.getElementById('route_relayer_auth_password').value === '' &&
      document.getElementById('route_connection_security').value === 'starttls_required');
   setValue('route_relayer_auth_password', 'new-secret');
   before = requests.length;
   click(act('routesave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/routes/5');
   check('saving an existing route PUTs the whole record with the new password', !!put && put.relayer_auth_password === 'new-secret' && put.target_smtp_host === 'smtp.partner.example' && put.relayer_requires_authentication === true, JSON.stringify(put));
   before = requests.length;
   click(act('routedel', { id: 5 }));
   await flush();
   check('deleting a route names its domain and deletes by id', confirmations[confirmations.length - 1].indexOf('partner.example') >= 0 && called(before, 'DELETE', '/api/v1/routes/5').length === 1 && rows().length === 1);

   // ---- remote domains
   //
   // The row that a policy is not a route: it names a domain this server does
   // not host and says what it will and will not do when it talks to it. The
   // checks are the ones that would catch the view drawing nothing from a good
   // answer, sending a key the route refuses, or swallowing a refusal.
   before = requests.length;
   await goTo('remotedomains');
   check('the remote domains view reads the policies', called(before, 'GET', '/api/v1/remote-domains').length === 1 && rows().length === 2);
   check('a policy row shows the TLS it demands each way, its limits and whether it verifies recipients',
      rows()[0].textContent.indexOf('Verified') >= 0 && rows()[0].textContent.indexOf('20480 KB') >= 0 &&
      rows()[0].textContent.indexOf('2 connections') >= 0 && rows()[0].textContent.indexOf('30/minute') >= 0,
      rows()[0].textContent);
   check('a policy row shows its gateway encryption and the start of the key\'s fingerprint, and never the key',
      rows()[0].textContent.indexOf('S/MIME') >= 0 && rows()[0].textContent.indexOf('f94894af1dea8079') >= 0 &&
      rows()[0].textContent.indexOf('cannot encrypt') < 0,
      rows()[0].textContent);
   check('a policy whose key cannot be encrypted to is marked so, with the reason, and a key the listing carried is drawn nowhere',
      rows()[1].textContent.indexOf('OpenPGP') >= 0 && rows()[1].textContent.indexOf('cannot encrypt') >= 0 &&
      rows()[1].textContent.indexOf('the key expired') >= 0 && document.body.innerHTML.indexOf('LEAKED-KEY-MATERIAL') < 0,
      rows()[1].textContent);
   click(act('rdnew'));
   await flush();
   check('a new policy starts at the defaults the document states - it demands nothing and verifies nothing',
      document.getElementById('rd_outbound_tls').value === 'none' && document.getElementById('rd_require_inbound_tls').checked === false &&
      document.getElementById('rd_max_message_size_kb').value === '0' && document.getElementById('rd_callout_enabled').checked === false &&
      document.getElementById('rd_callout_port').value === '25' && document.getElementById('rd_callout_timeout_seconds').value === '10' &&
      document.getElementById('rd_allow_forwarding').checked === true,
      'tls=' + document.getElementById('rd_outbound_tls').value + ' size=' + document.getElementById('rd_max_message_size_kb').value);
   check('a new policy encrypts nothing, and would refuse what it could not encrypt',
      document.getElementById('rd_encrypt').value === 'none' && document.getElementById('rd_encrypt_fallback').value === 'refuse' &&
      document.getElementById('rd_encrypt_key').tagName === 'TEXTAREA' && document.getElementById('rd_encrypt_key').value === '',
      'encrypt=' + document.getElementById('rd_encrypt').value + ' fallback=' + document.getElementById('rd_encrypt_fallback').value);
   check('only the domain is required', document.getElementById('rd_domain_name').closest('.fr').textContent.indexOf('required') >= 0 &&
      document.getElementById('rd_callout_host').closest('.fr').textContent.indexOf('required') < 0);
   setValue('rd_domain_name', 'partner.example');
   setValue('rd_outbound_tls', 'dane');
   setChecked('rd_require_inbound_tls', true);
   setValue('rd_max_message_size_kb', '10240');
   before = requests.length;
   click(act('rdsave'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/remote-domains');
   check('saving a new policy posts the form with the defaults filled in',
      !!posted && posted.domain_name === 'partner.example' && posted.outbound_tls === 'dane' && posted.require_inbound_tls === true &&
      posted.max_message_size_kb === 10240 && posted.callout_port === 25 && posted.allow_automatic_replies === true, JSON.stringify(posted));
   check('an empty key box sends no key, and the read-only facts are not sent',
      !!posted && posted.encrypt === 'none' && posted.encrypt_fallback === 'refuse' && !('encrypt_key' in posted) &&
      !('encrypt_key_fingerprint' in posted) && !('encrypt_key_problem' in posted), JSON.stringify(posted));
   check('and the re-read list has it', rows().length === 3 && rows()[2].textContent.indexOf('partner.example') >= 0,
      rows().map((r) => r.textContent).join(' | '));
   click(act('rdedit', { id: 7 }));
   await flush();
   check('editing a policy shows its values', document.getElementById('rd_outbound_tls').value === 'verified' &&
      document.getElementById('rd_max_connections').value === '2' && document.getElementById('rd_allow_automatic_replies').checked === false);
   setValue('rd_outbound_tls', 'encrypted');
   before = requests.length;
   click(act('rdsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/remote-domains/7');
   check('saving an existing policy PUTs the whole record', !!put && put.outbound_tls === 'encrypted' && put.domain_name === 'bank.example' &&
      put.max_connections === 2 && put.allow_automatic_replies === false, JSON.stringify(put));
   check('and keeps the partner\'s key it was never shown: the PUT carries the method and no key',
      !!put && put.encrypt === 'smime' && !('encrypt_key' in put), JSON.stringify(put));
   click(act('rdedit', { id: 8 }));
   await flush();
   check('editing a policy never fills the key box, even from a listing that carried the key',
      document.getElementById('rd_encrypt_key').value === '' && document.body.innerHTML.indexOf('LEAKED-KEY-MATERIAL') < 0 &&
      document.getElementById('rd_encrypt').value === 'openpgp',
      'key box=' + document.getElementById('rd_encrypt_key').value.slice(0, 40));
   before = requests.length;
   click(act('rdsave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/remote-domains/8');
   check('and saving it sends no key back', !!put && put.encrypt === 'openpgp' && put.encrypt_fallback === 'send_unencrypted' &&
      !('encrypt_key' in put), JSON.stringify(put));
   before = requests.length;
   click(act('rdclearcache'));
   await flush();
   check('forgetting the remembered verdicts posts to the cache route and says how many went',
      called(before, 'POST', '/api/v1/remote-domains/verification-cache/clear').length === 1 &&
      document.body.textContent.indexOf('3 verdict(s) forgotten') >= 0, document.body.textContent.slice(-200));
   before = requests.length;
   click(act('rddel', { id: 7 }));
   await flush();
   check('deleting a policy names its domain and deletes by id',
      confirmations[confirmations.length - 1].indexOf('bank.example') >= 0 &&
      called(before, 'DELETE', '/api/v1/remote-domains/7').length === 1 && rows().length === 2);

   // ---- certificates
   before = requests.length;
   await goTo('certs');
   check('the certificates view reads the certificates', called(before, 'GET', '/api/v1/certificates').length === 1 && rows().length === 1 && rows()[0].textContent.indexOf('/etc/ssl/mail.pem') >= 0);
   check('the form has the document\'s keys and its password box is empty', document.getElementById('cert_name') !== null && document.getElementById('cert_private_key_password').type === 'password' && document.getElementById('cert_private_key_password').value === '');
   setValue('cert_name', 'second');
   setValue('cert_certificate_file', '/etc/ssl/second.pem');
   setValue('cert_private_key_file', '/etc/ssl/second.key');
   before = requests.length;
   click(act('certadd'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/certificates');
   check('adding a certificate posts the paths and no password when none was typed', !!posted && JSON.stringify(posted) === '{"name":"second","certificate_file":"/etc/ssl/second.pem","private_key_file":"/etc/ssl/second.key"}', JSON.stringify(posted));
   check('and the re-read list shows it', rows().length === 2 && rows()[1].textContent.indexOf('second') >= 0);
   before = requests.length;
   click(act('certdel', { id: 2 }));
   await flush();
   check('a certificate a port binds is refused, in the server\'s words, on its row', called(before, 'DELETE', '/api/v1/certificates/2').length === 1 && document.getElementById('err_cert_2').textContent.indexOf('bound by port 1') >= 0,
      document.getElementById('err_cert_2').textContent);
   const newCert = state.certificates[1].id;
   click(act('certdel', { id: newCert }));
   await flush();
   check('an unbound one is deleted and gone from the re-read list', rows().length === 1);

   // ---- the ACME card, from GET /api/v1/acme as AcmeClient::StatusJson writes it:
   // domains and missing_domains are the hosts the server orders for, and a name of
   // AcmeDomains no certificate can carry is in unusable_domains alone.
   const acmeCertificate = { file: '/var/lib/hmailserver/ACME/fullchain.pem', names: ['mail.example.com'], subject: 'CN=mail.example.com',
      issuer: 'CN=R11', serial: '0a1b', not_before: '2026-10-01T00:00:00Z', not_after: '2026-12-30T00:00:00Z', expired: false,
      renews_at: '2026-11-30T00:00:00Z', deployed: true };
   const acmeCard = async (status) => {
      state.acme = Object.assign({ enabled: true, directory_url: 'https://acme-v02.api.letsencrypt.org/directory', certificate_directory: '/var/lib/hmailserver/ACME',
         domains: [], unusable_domains: [], certificate: null, covers_domains: null, missing_domains: [] }, status);
      await goTo('certs');
      const card = content().querySelectorAll('.sect')[0];
      const notices = card ? card.querySelectorAll('.notice').map((n) => n.attributes['data-level'] + ': ' + n.textContent) : [];
      return { text: card ? card.textContent : '', notices };
   };
   let acmeSeen = await acmeCard({ domains: ['mail.example.com'], unusable_domains: ['*.example.com'], certificate: acmeCertificate,
      covers_domains: true, missing_domains: [] });
   check('a name of AcmeDomains no certificate can carry is named on the ACME card, and the certificate is not said to name every host',
      acmeSeen.text.indexOf('Automatic certificate (ACME)') >= 0 &&
      acmeSeen.notices.some((n) => n.indexOf('warn: ') === 0 && n.indexOf('No certificate can name *.example.com, so the server leaves it out of every check and order.') >= 0) &&
      acmeSeen.notices.some((n) => n.indexOf('info: ') === 0 && n.indexOf('The certificate names every other host in AcmeDomains.') >= 0) &&
      acmeSeen.text.indexOf('names every host in AcmeDomains') < 0, JSON.stringify(acmeSeen.notices));
   acmeSeen = await acmeCard({ domains: [], unusable_domains: ['*.example.com', '192.0.2.7'], certificate: acmeCertificate, covers_domains: true, missing_domains: [] });
   check('with every name of AcmeDomains left out, the card names them and says nothing is requested, whatever certificate is on disk',
      acmeSeen.notices.some((n) => n.indexOf('No certificate can name *.example.com, 192.0.2.7, so') >= 0) &&
      acmeSeen.notices.some((n) => n.indexOf('warn: ') === 0 && n.indexOf('AcmeDomains names no host a certificate can carry, so no certificate is requested.') >= 0) &&
      acmeSeen.text.indexOf('The certificate names every') < 0, JSON.stringify(acmeSeen.notices));
   acmeSeen = await acmeCard({ domains: [], unusable_domains: [], certificate: null, missing_domains: [] });
   check('a list of only blanks and dots, which the server reads as empty, gives the card no sentence with an empty name',
      acmeSeen.notices.length === 1 && acmeSeen.notices[0].indexOf('AcmeDomains names no host a certificate can carry') >= 0 &&
      acmeSeen.text.indexOf('No certificate can name') < 0 && acmeSeen.text.indexOf('The certificate names every') < 0, JSON.stringify(acmeSeen.notices));
   acmeSeen = await acmeCard({ domains: ['mail.example.com'], certificate: null, missing_domains: ['mail.example.com'] });
   check('with no certificate on disk the card says so and names the hosts a check issues one for',
      acmeSeen.notices.length === 1 &&
      acmeSeen.notices[0].indexOf('No ACME certificate could be read from the server\'s disk; a check issues one for mail.example.com.') >= 0,
      JSON.stringify(acmeSeen.notices));
   acmeSeen = await acmeCard({ domains: ['mail.example.com', 'mta-sts.example.com'], certificate: acmeCertificate, covers_domains: false,
      missing_domains: ['mta-sts.example.com'] });
   check('a host the certificate lacks is named, and nothing is left out',
      acmeSeen.notices.length === 1 && acmeSeen.notices[0].indexOf('warn: ') === 0 &&
      acmeSeen.notices[0].indexOf('The certificate does not name mta-sts.example.com; a check issues one that does.') >= 0, JSON.stringify(acmeSeen.notices));
   acmeSeen = await acmeCard({ domains: ['mail.example.com'], certificate: acmeCertificate, covers_domains: true, missing_domains: [] });
   check('a certificate that names every host of a list with nothing left out says so, at the information level',
      acmeSeen.notices.length === 1 && acmeSeen.notices[0].indexOf('info: ') === 0 &&
      acmeSeen.notices[0].indexOf('The certificate names every host in AcmeDomains.') >= 0, JSON.stringify(acmeSeen.notices));
   acmeSeen = await acmeCard({ domains: ['mail.example.com'], unusable_domains: ['<b>x</b>'], certificate: acmeCertificate, covers_domains: true, missing_domains: [] });
   check('a name left out is drawn as text', acmeSeen.notices.some((n) => n.indexOf('No certificate can name <b>x</b>, so') >= 0) &&
      content().querySelectorAll('.sect')[0].querySelectorAll('b').every((b) => b.textContent !== 'x'), JSON.stringify(acmeSeen.notices));
   state.acme = null;

   // ---- ports
   before = requests.length;
   await goTo('ports');
   check('the ports view reads the ports and the certificates', called(before, 'GET', '/api/v1/ports').length === 1 && called(before, 'GET', '/api/v1/certificates').length === 1 && rows().length === 2);
   check('a port row names its certificate rather than its id', rows()[0].textContent.indexOf('mail.example.com') >= 0 && rows()[0].textContent.indexOf('0.0.0.0:25') >= 0 && rows()[1].textContent.indexOf('—') >= 0);
   check('an SMTP port says its SMTP authentication, and one that is not SMTP says none, whatever the listing gives it',
      rows()[0].querySelectorAll('td')[5].textContent === 'Honeypot' && rows()[1].querySelectorAll('td')[5].textContent === '—',
      rows()[0].querySelectorAll('td')[5].textContent + ' / ' + rows()[1].querySelectorAll('td')[5].textContent);
   check('the restart wording is the document\'s', content().textContent.indexOf('takes effect when the server restarts') >= 0 && content().textContent.indexOf(REINITIALIZE.slice(0, 40)) >= 0);
   check('and it is a notice at the warning level, with the mark and the word beside the colour',
      content().querySelector('.notice[data-level="warn"]') !== null && content().querySelector('.notice .mark').textContent === '\u25b2' &&
      content().querySelector('.notice[data-level="warn"]').textContent.indexOf('Warning') >= 0,
      content().querySelector('.notice') ? content().querySelector('.notice').innerHTML : 'no notice');
   click(act('portnew'));
   await flush();
   check('a new port starts at the defaults the document states in prose', document.getElementById('port_address').value === '0.0.0.0' && document.getElementById('port_connection_security').value === 'none' &&
      document.getElementById('port_client_certificate_policy').value === 'off' && document.getElementById('port_certificate_id').tagName === 'SELECT' && document.getElementById('port_certificate_id').value === '0' &&
      document.getElementById('port_smtp_authentication').tagName === 'SELECT' && document.getElementById('port_smtp_authentication').value === 'on',
      'address=' + document.getElementById('port_address').value + ' cert=' + document.getElementById('port_certificate_id').value);
   check('the certificate choice lists the certificates by name', document.getElementById('port_certificate_id').textContent.indexOf('mail.example.com') >= 0 && document.getElementById('port_certificate_id').textContent.indexOf('(none)') >= 0);
   setValue('port_protocol', 'pop3');
   setValue('port_port', '995');
   setValue('port_connection_security', 'tls');
   setValue('port_certificate_id', '2');
   before = requests.length;
   click(act('portsave'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/ports');
   check('saving a new port posts the record with the certificate id as a number', !!posted && posted.protocol === 'pop3' && posted.port === 995 && posted.connection_security === 'tls' && posted.certificate_id === 2 && posted.address === '0.0.0.0' &&
      posted.smtp_authentication === 'on',
      JSON.stringify(posted));
   check('and the re-read list has three', rows().length === 3 && rows()[2].textContent.indexOf('0.0.0.0:995') >= 0);
   click(act('portedit', { id: 2 }));
   await flush();
   setValue('port_port', '993');
   setValue('port_connection_security', 'tls');
   before = requests.length;
   click(act('portsave'));
   await flush();
   check('a TLS port without a certificate is the server\'s refusal to make, not the page\'s', called(before, 'PUT', '/api/v1/ports/2').length === 1);
   nextRefusal = null;
   click(act('portedit', { id: 2 }));
   await flush();
   nextRefusal = 'A TLS port needs a certificate.';
   click(act('portsave'));
   await flush();
   check('and the refusal is shown in the editor', document.getElementById('err_portedit').textContent === 'A TLS port needs a certificate.' && $('#port_port') !== null);
   click(act('cancel'));
   await flush();
   before = requests.length;
   click(act('portdel', { id: 2 }));
   await flush();
   check('deleting a port names it and deletes by id', confirmations[confirmations.length - 1].indexOf('imap 0.0.0.0:993') >= 0 && called(before, 'DELETE', '/api/v1/ports/2').length === 1 && rows().length === 2,
      confirmations[confirmations.length - 1]);

   // ---- the restart, which ends this very session
   confirmAnswer = false;
   before = requests.length;
   click(act('portapply'));
   await flush();
   check('the restart asks first', called(before, 'POST', /reinitialize/).length === 0 && confirmations[confirmations.length - 1].indexOf('session ends') >= 0, confirmations[confirmations.length - 1]);
   confirmAnswer = true;
   click(act('portapply'));
   await flush();
   check('yes posts the restart', called(before, 'POST', '/api/v1/server/reinitialize').length === 1);
   check('and stops the dashboard poll', intervals.length === 0, intervals.length + ' intervals');
   for (let i = 0; i < 6 && $('#gate').style.display === 'none'; i += 1) { fireTimers(); await flush(); }
   const probes = called(before, 'GET', '/');
   check('then waits for the page to answer again, asked without a credential', probes.length === 3 && probes.every((r) => r.credentials === 'omit'), probes.length + ' probes');
   check('and returns to the sign-in card saying the session ended with the restart', $('#gate').style.display === 'grid' && $('#loginErr').textContent.indexOf('restarted') >= 0, $('#loginErr').textContent);
   check('the session mark is gone with it', sessionStorage.getItem('hmsSession') === null);

   // ---- Apply now's yes path, which ends the session as the restart above does
   // (the review of 22 September 2026, finding 14): one POST, then the wait for the
   // page to come back - and, with a key marked restart still pending, a sign-in
   // card that says the reinitialise did not take it rather than that it took the
   // saved settings.
   signedIn = false;
   await signIn();
   const pendingKept = state.pendingSettings.slice();
   state.pendingSettings = [
      { name: 'ManageSieveServerPort', stored: '4190', running: '0', applies: 'reinitialize', secret: false },
      { name: 'ServiceAccountName', stored: 'svc-hmail', running: '', applies: 'restart', secret: false }
   ];
   await goTo('settings');
   check('a key that waits for a restart is counted beside one that waits for a reinitialize',
      !!$('#pendingBar') && $('#pendingBar').textContent.indexOf('1 saved setting(s) wait for a reinitialize, and 1 for a service restart.') >= 0,
      $('#pendingBar') ? $('#pendingBar').textContent : 'no bar');
   restartProbes = 0;
   confirmAnswer = true;
   before = requests.length;
   click(act('applypending'));
   await flush();
   check('Apply now, answered yes, posts one reinitialize', called(before, 'POST', '/api/v1/server/reinitialize').length === 1, paths(before));
   for (let i = 0; i < 6 && $('#gate').style.display === 'none'; i += 1) { fireTimers(); await flush(); }
   check('then waits for the page, and says what still waits for a restart when the session ends',
      $('#gate').style.display === 'grid' && $('#loginErr').textContent.indexOf('1 still wait for a service restart') >= 0 &&
      $('#loginErr').textContent.indexOf('took the saved settings') < 0, $('#loginErr').textContent);
   state.pendingSettings = pendingKept;

   // ---- back in, to the logs
   signedIn = false;
   await signIn();
   check('signing in again works after a restart', $('#app').style.display === 'flex');
   before = requests.length;
   await goTo('logs');
   check('the logs view reads the file list', called(before, 'GET', '/api/v1/logs').length === 1 && rows().length === 2 && rows()[0].textContent.indexOf('2.0 KB') >= 0, rows().length ? rows()[0].textContent : 'no rows');
   before = requests.length;
   click(act('log', { name: 'hmailserver_2026-09-14.log', lines: 200 }));
   await flush();
   check('Last 200 asks for the tail with the count', called(before, 'GET', '/api/v1/logs/hmailserver_2026-09-14.log?lines=200').length === 1, paths(before));
   check('and shows the lines', $('#logPre').hidden === false && $('#logPre').textContent === state.logLines.join('\n'));
   click(act('log', { name: 'hmailserver_2026-09-14.log', lines: 2000 }));
   await flush();
   check('Last 2000 asks for 2000', called(before, 'GET', /lines=2000$/).length === 1);
   nextRefusal = null;
   state.logs.push({ name: 'gone.log', size: 1, created: '' });
   await goTo('logs');
   click(act('log', { name: 'gone.log', lines: 200 }));
   await flush();
   check('a file the server no longer has is an error under the list, naming it', document.getElementById('err_log').textContent.indexOf('gone.log') >= 0 && document.getElementById('err_log').textContent.indexOf('No such log file') >= 0,
      document.getElementById('err_log').textContent);
   state.logs.pop();

   // ---- one connection followed live
   state.sessions = [
      { id: 42, protocol: 'SMTP', peer: '203.0.113.9', port: 25, client: false, open: true, account: 'relay@example.com', started: '2026-09-17 09:00:04', ended: '' },
      { id: 41, protocol: 'IMAP', peer: '192.0.2.7', port: 143, client: false, open: true, account: 'anna@example.com', started: '2026-09-17 09:00:00', ended: '' },
      { id: 40, protocol: 'SMTP', peer: '198.51.100.2', port: 25, client: false, open: false, account: '', started: '2026-09-17 08:59:58', ended: '2026-09-17 08:59:59' },
      { id: 39, protocol: 'POP3', peer: '192.0.2.8', port: 110, client: false, open: false, account: 'ben@example.com', started: '2026-09-17 08:59:50', ended: '2026-09-17 08:59:51' }
   ];
   state.followLines = [
      { seq: 10, time: '2026-09-17 09:00:00.100', category: 'IMAPD', message: 'RECEIVED: A1 LOGIN anna@example.com ***' },
      { seq: 11, time: '2026-09-17 09:00:00.120', category: 'IMAPD', message: 'SENT: A1 OK LOGIN completed' }
   ];
   before = requests.length;
   await goTo('logs');
   check('the logs view lists the sessions open now and lately ended, with the ids the log carries',
      called(before, 'GET', '/api/v1/logs/sessions').length === 1 && content().textContent.indexOf('41') >= 0 && content().textContent.indexOf('anna@example.com') >= 0 &&
      content().textContent.indexOf('198.51.100.2') >= 0 && content().querySelectorAll('button[data-act="follow"]').length === 4,
      content().textContent.slice(0, 400));
   check('an open session shows when it started and an ended one when it ended',
      content().textContent.indexOf('2026-09-17 09:00:00') >= 0 && content().textContent.indexOf('2026-09-17 08:59:59') >= 0 && content().textContent.indexOf('08:59:58') < 0);
   // Past a thousand open sessions the route lists the newest thousand and says
   // it has cut the list. A page that drew them and said nothing would be
   // describing a server with a thousand connections open as though that were
   // the whole of it.
   state.sessionsTruncated = true;
   await goTo('logs');
   check('a listing the server had to cut says so rather than presenting the newest as all there are',
      content().textContent.indexOf('More connections are open than this list holds') >= 0 && content().textContent.indexOf('the newest 2 are shown') >= 0,
      content().textContent.slice(0, 400));
   state.sessionsTruncated = false;
   await goTo('logs');
   check('and says nothing of the kind when the listing is the whole of it',
      content().textContent.indexOf('More connections are open') < 0);
   before = requests.length;
   click(act('follow', { id: 41 }));
   await flush();
   check('Follow asks for the session\'s lines from the start, waiting for the next', called(before, 'GET', '/api/v1/logs/sessions/41?after=0&wait=10').length === 1, paths(before));
   check('and shows them as they come, oldest first', $('#followBox').hidden === false && $('#followTitle').textContent === 'Following session 41' &&
      $('#followPre').textContent.indexOf('IMAPD  RECEIVED: A1 LOGIN') >= 0 && $('#followPre').textContent.indexOf('SENT: A1 OK') > $('#followPre').textContent.indexOf('RECEIVED: A1 LOGIN'),
      $('#followPre').textContent);
   check('the next request continues from the cursor the server answered', called(before, 'GET', '/api/v1/logs/sessions/41?after=11&wait=10').length >= 1, paths(before));
   click(act('followcopy'));
   await flush();
   check('Copy puts the lines on the clipboard', clipboard === $('#followPre').textContent && toastText() === 'Copied to clipboard', String(clipboard));
   before = requests.length;
   click(act('followstop'));
   await flush();
   check('Stop following hides the pane', $('#followBox').hidden === true);
   // A Follow after a Stop is a follow of its own. The stopped loop is parked
   // on the pause it takes when an answer brought no line, and when it wakes it
   // has to see that it is no longer the current follow - which it can only do
   // if the new follow's ticket is a number the old one never had. fireTimers
   // wakes it here rather than waiting a second for it.
   before = requests.length;
   click(act('follow', { id: 42 }));
   await flush();
   fireTimers();
   await flush();
   check('a Follow after a Stop is its own: the stopped one wakes, sees it is not the current follow and asks for nothing more',
      called(before, 'GET', /^\/api\/v1\/logs\/sessions\/41/).length === 0 && $('#followTitle').textContent === 'Following session 42' &&
      called(before, 'GET', /^\/api\/v1\/logs\/sessions\/42/).length >= 1,
      paths(before));
   click(act('followstop'));
   await flush();
   // The session ended between the listing and the click.
   state.sessions = state.sessions.filter((s) => s.id !== 39);
   click(act('follow', { id: 39 }));
   await flush();
   check('a session the server no longer holds is a refusal under the pane, in the server\'s words',
      document.getElementById('err_follow').textContent.indexOf('39') >= 0 && document.getElementById('err_follow').textContent.indexOf('no such session') >= 0,
      document.getElementById('err_follow').textContent);
   // The pane is bounded. The server bounds an answer and its ring; nothing in
   // the page used to bound the pane, so a follow left running on a busy
   // session grew the tab until it was unusable and Copy pulled megabytes onto
   // the clipboard. What falls off the front is in the log file below it.
   state.sessions = [
      { id: 41, protocol: 'IMAP', peer: '192.0.2.7', port: 143, client: false, open: true, account: 'anna@example.com', started: '2026-09-17 09:00:00', ended: '' }
   ];
   state.followLines = [];
   for (let i = 1; i <= 5200; i++) { state.followLines.push({ seq: i, time: '2026-09-17 09:00:00.000', category: 'IMAPD', message: 'SENT: line ' + i }); }
   await goTo('logs');
   click(act('follow', { id: 41 }));
   await flush();
   const heldLines = $('#followPre').textContent.split('\n').filter((line) => line !== '');
   check('the pane keeps only the most recent lines rather than growing for as long as the tab is open',
      heldLines.length === 5000 && heldLines[0].indexOf('SENT: line 201') >= 0 && heldLines[heldLines.length - 1].indexOf('SENT: line 5200') >= 0,
      heldLines.length + ' lines, first ' + heldLines[0]);
   check('and says so, rather than letting the reader think the pane is the whole of it',
      $('#followNote').textContent.indexOf('5000') >= 0 && $('#followNote').textContent.indexOf('log file') >= 0,
      $('#followNote').textContent);
   click(act('followstop'));
   await flush();
   state.sessions = [];
   state.followLines = [];
   await goTo('logs');
   check('with no session the view says so rather than drawing an empty table',
      content().textContent.indexOf('No SMTP, POP3 or IMAP session is open') >= 0 && content().querySelectorAll('button[data-act="follow"]').length === 0 && rows().length === 2,
      String(rows().length));

   // ---- what this server has changed from stock
   before = requests.length;
   await goTo('changed');
   check('the Changed from stock view asks the one route that answers it',
      called(before, 'GET', '/api/v1/settings/changed').length === 1, paths(before));
   check('and lists a row for each setting that is not what this version ships with', rows().length === 6, String(rows().length));
   check('with the store, the name, the value and the shipped default beside each other',
      rows()[0].textContent.indexOf('ini') >= 0 && rows()[0].textContent.indexOf('LogLevel') >= 0 &&
      rows()[0].textContent.indexOf('5') >= 0 && rows()[0].textContent.indexOf('9') >= 0, rows()[0].textContent);
   check('a secret is named and its value is never on the page',
      rows()[1].textContent.indexOf('OAuth2HmacSecret') >= 0 && rows()[1].textContent.indexOf('(set)') >= 0,
      rows()[1].textContent);
   check('a name the server ships no default for says so rather than showing an empty column',
      rows()[2].textContent.indexOf('This server has no default for it') >= 0, rows()[2].textContent);
   check('the other store is named as itself', rows()[3].textContent.indexOf('database') >= 0, rows()[3].textContent);
   check('a value the file is forcing says so, because no other page of the Deck shows an override',
      rows()[4].textContent.indexOf('Forced from [SettingsOverride] in hMailServer.ini') >= 0, rows()[4].textContent);
   check('a value the server\'s environment sets is marked with where it came from, and a file\'s is not shown',
      !!rows()[5].querySelector('[data-environment="file"]') && rows()[5].textContent.indexOf('Set by the environment, from a file') >= 0 &&
      rows()[5].textContent.indexOf('(set)') >= 0 && rows()[0].querySelector('[data-environment]') === null, rows()[5].textContent);
   check('and the count is given against the number of settings there are',
      content().textContent.indexOf('6 of 391 settings') >= 0, content().textContent.slice(-200));
   check('the page only reads: it offers nothing that would change a setting from here',
      content().querySelectorAll('button').length === 0 && content().querySelectorAll('input').length === 0,
      String(content().querySelectorAll('button').length));
   state.changed = { changed: 0, considered: 391, settings: [] };
   await goTo('changed');
   check('a server that has changed nothing says so rather than drawing an empty table',
      content().textContent.indexOf('Nothing has been changed') >= 0, content().textContent.slice(0, 200));

   // ---- the audit trail
   before = requests.length;
   await goTo('audit');
   check('the audit view asks for the first page', called(before, 'GET', /^\/api\/v1\/audit\?limit=50&offset=0$/).length === 1, paths(before));
   check('and lists both changes', rows().length === 2, String(rows().length));
   check('naming the interface each was made over', rows()[0].textContent.indexOf('REST') >= 0 && rows()[1].textContent.indexOf('COM') >= 0,
      rows().length ? rows()[0].textContent + ' | ' + rows()[1].textContent : 'no rows');
   check('and showing the old and new value of a setting', rows()[1].textContent.indexOf('60 -> 120') >= 0, rows()[1].textContent);
   before = requests.length;
   click(act('auditpage', { by: '50' }));
   await flush();
   check('Older asks for the next page', called(before, 'GET', /offset=50/).length === 1, paths(before));
   before = requests.length;
   click(act('auditpage', { by: '-50' }));
   await flush();
   check('Newer comes back and never asks for a negative offset', called(before, 'GET', /offset=0/).length === 1, paths(before));
   before = requests.length;
   click(act('auditverify'));
   await flush();
   check('Verify the chain asks the server', called(before, 'GET', '/api/v1/audit/verify').length === 1, paths(before));
   check('and says so when it is intact', $('#toast').textContent.indexOf('2') >= 0, $('#toast').textContent);
   state.auditIntact = false;
   click(act('auditverify'));
   await flush();
   check('a broken chain names the row, in the error line rather than a toast',
      document.getElementById('err_audit').textContent.indexOf('2') >= 0 && document.getElementById('err_audit').textContent.indexOf('edited') >= 0,
      document.getElementById('err_audit').textContent);
   state.auditIntact = true;
   check('a setting\'s row offers Put back and a domain\'s does not',
      rows()[1].querySelector('[data-act="auditrevert"]') !== null && rows()[0].querySelector('[data-act="auditrevert"]') === null,
      rows()[1].innerHTML);
   before = requests.length;
   click(act('auditrevert', { id: '1' }));
   await flush();
   check('Put back asks the server for that row and reads the trail again',
      called(before, 'POST', '/api/v1/audit/1/revert').length === 1 && called(before, 'GET', /^\/api\/v1\/audit\?/).length === 1, paths(before));
   check('and says what is what again', toastText().indexOf('AutoBanMinutes') >= 0 && toastText().indexOf('60') >= 0, toastText());
   nextRefusal = 'A secret\'s values are never kept in the trail';
   click(act('auditrevert', { id: '1' }));
   await flush();
   check('a refused put-back is shown in the server\'s own words', document.getElementById('err_audit').textContent.indexOf('never kept in the trail') >= 0,
      document.getElementById('err_audit').textContent);
   nextRefusal = null;
   const auditRows = state.audit;
   state.audit = [];
   await goTo('audit');
   check('an empty trail says what will appear here', content().textContent.indexOf('Nothing has been recorded yet') >= 0, content().textContent.slice(0, 120));
   state.audit = auditRows;

   // ---- webhooks
   before = requests.length;
   await goTo('webhooks');
   check('the webhooks view reads the subscriptions and the queue, each once',
      called(before, 'GET', '/api/v1/webhooks').length === 1 && called(before, 'GET', /^\/api\/v1\/webhooks\/deliveries/).length === 1,
      paths(before));
   check('one row per subscription, its endpoint under the name',
      rows().length === 2 && rows()[0].textContent.indexOf('billing') >= 0 && rows()[0].textContent.indexOf('hooks.example') >= 0,
      rows().map((r) => r.textContent).join(' | '));
   check('the queue is said in words rather than in three bare numbers',
      rows()[0].textContent.indexOf('1 waiting') >= 0 && rows()[0].textContent.indexOf('4 sent') >= 0, rows()[0].textContent);
   check('the delivery below it names the event, its state and why it has not arrived',
      rows()[1].textContent.indexOf('message.bounced') >= 0 && rows()[1].textContent.indexOf('pending') >= 0 &&
      rows()[1].textContent.indexOf('502') >= 0, rows()[1].textContent);
   check('no shared secret is anywhere on the page', content().textContent.indexOf('secret_set') < 0 &&
      content().innerHTML.indexOf('"secret"') < 0, content().textContent.slice(0, 160));

   before = requests.length;
   click(act('hooknew'));
   await flush();
   check('the editor is built from the schema of the create route',
      document.getElementById('hook_name') !== null && document.getElementById('hook_url') !== null &&
      document.getElementById('hook_events') !== null && document.getElementById('hook_max_per_minute') !== null,
      content().textContent.slice(0, 160));
   check('the secret is a write-only box, because the schema says writeOnly',
      document.getElementById('hook_secret') !== null && document.getElementById('hook_secret').type === 'password',
      document.getElementById('hook_secret') ? document.getElementById('hook_secret').type : 'no box');
   check('and enabled starts on, because the create route says so in prose',
      document.getElementById('hook_enabled') !== null && document.getElementById('hook_enabled').checked === true,
      document.getElementById('hook_enabled') ? String(document.getElementById('hook_enabled').checked) : 'no box');

   setValue('hook_name', 'ticketing');
   setValue('hook_url', 'https://tickets.example/hook');
   setValue('hook_secret', 'a-shared-secret');
   setValue('hook_events', 'message.bounced,account.locked');
   click(act('hooksave'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/webhooks');
   check('a new subscription is sent with its name, endpoint, secret and events',
      posted && posted.name === 'ticketing' && posted.url === 'https://tickets.example/hook' &&
      posted.secret === 'a-shared-secret' && posted.events === 'message.bounced,account.locked',
      JSON.stringify(posted));
   check('and the list comes back with it', rows().length === 3 && content().textContent.indexOf('ticketing') >= 0,
      rows().map((r) => r.textContent).join(' | '));

   before = requests.length;
   click(act('hookedit', { id: 'billing' }));
   await flush();
   check('editing one fills the form from what the server holds and hides the name',
      document.getElementById('hook_url').value === 'https://hooks.example/mail' && document.getElementById('hook_name') === null,
      content().textContent.slice(0, 160));
   check('and its secret box is empty, since the server never handed one back',
      document.getElementById('hook_secret').value === '', document.getElementById('hook_secret').value);
   document.getElementById('hook_enabled').checked = false;
   click(act('hooksave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/webhooks/billing');
   check('a change is a PUT to its own name', put !== null && put.enabled === false, JSON.stringify(put));
   check('and an untouched secret box sends no secret, so the one that is set survives',
      put !== null && put.secret === undefined, JSON.stringify(put));

   before = requests.length;
   click(act('hooktest'));
   await flush();
   check('the test button raises one event and says how many subscriptions took it',
      called(before, 'POST', '/api/v1/webhooks/test').length === 1 && $('#toast').textContent.indexOf('1') >= 0,
      $('#toast').textContent);

   before = requests.length;
   click(act('hookrun'));
   await flush();
   check('the pass button runs one and reports what it did',
      called(before, 'POST', '/api/v1/webhooks/run').length === 1 && $('#toast').textContent.indexOf('1') >= 0,
      $('#toast').textContent);

   before = requests.length;
   click(act('hookdel', { id: 'ticketing' }));
   await flush();
   check('deleting one names it in the question and deletes by name',
      confirmations[confirmations.length - 1].indexOf('ticketing') >= 0 &&
      called(before, 'DELETE', '/api/v1/webhooks/ticketing').length === 1 && rows().length === 2,
      rows().map((r) => r.textContent).join(' | '));

   const keptWebhooks = state.webhooks;
   state.webhooks = [];
   state.webhookQueue = [];
   await goTo('webhooks');
   check('with none configured the page says what that means',
      content().textContent.indexOf('no mail event leaves this server') >= 0, content().textContent.slice(0, 200));
   state.webhooks = keptWebhooks;

   // ---- inbound webhooks
   before = requests.length;
   await goTo('inboundwebhooks');
   check('the inbound webhooks view reads the routes and the last twenty-five deliveries, each once',
      called(before, 'GET', '/api/v1/inbound-webhooks').length === 1 &&
      called(before, 'GET', /^\/api\/v1\/inbound-webhooks\/deliveries\?limit=25$/).length === 1, paths(before));
   check('one row per route, saying it posts only and naming its URL, its queue in words',
      rows()[0].textContent.indexOf('support@example.com') >= 0 && rows()[0].textContent.indexOf('posted only') >= 0 &&
      rows()[0].textContent.indexOf('helpdesk.example') >= 0 && rows()[0].textContent.indexOf('1 waiting') >= 0 &&
      rows()[0].textContent.indexOf('3 sent') >= 0 && rows()[0].textContent.indexOf('1 kept') >= 0, rows()[0].textContent);
   check('a kept delivery says why it has not arrived and offers Download, Retry and Remove',
      rows()[1].textContent.indexOf('kept') >= 0 && rows()[1].textContent.indexOf('503') >= 0 &&
      content().querySelectorAll('a[download]').length === 1 &&
      content().querySelectorAll('a[download]')[0].attributes.href === '/api/v1/inbound-webhooks/deliveries/41/message' &&
      content().querySelectorAll('button[data-act="inhookretry"]').length === 1, rows()[1].textContent);
   check('a delivered one keeps no message, so it offers no Download and no Retry but a Remove',
      content().querySelectorAll('button[data-act="inhookdrop"]').length === 2, String(content().querySelectorAll('button[data-act="inhookdrop"]').length));
   check('a credential that reaches every domain is offered the pass', content().querySelectorAll('button[data-act="inhookrun"]').length === 1);
   check('no shared secret is anywhere on the page', content().textContent.indexOf('secret_set') < 0 &&
      content().innerHTML.indexOf('"secret"') < 0, content().textContent.slice(0, 160));

   before = requests.length;
   click(act('inhooknew'));
   await flush();
   check('the inbound editor is built from the schema of the create route',
      document.getElementById('inhook_address') !== null && document.getElementById('inhook_url') !== null &&
      document.getElementById('inhook_on_failure') !== null && document.getElementById('inhook_max_size_kb') !== null,
      content().textContent.slice(0, 160));
   check('its secret is a write-only box', document.getElementById('inhook_secret') !== null &&
      document.getElementById('inhook_secret').type === 'password',
      document.getElementById('inhook_secret') ? document.getElementById('inhook_secret').type : 'no box');
   check('and it starts from the defaults the create route states in prose: posts only, keeps, 10240 KB, on',
      document.getElementById('inhook_deliver').checked === false && document.getElementById('inhook_on_failure').value === 'keep' &&
      document.getElementById('inhook_max_size_kb').value === '10240' && document.getElementById('inhook_enabled').checked === true &&
      document.getElementById('inhook_format').value === 'form',
      'deliver=' + document.getElementById('inhook_deliver').checked + ' on_failure=' + document.getElementById('inhook_on_failure').value +
      ' max=' + document.getElementById('inhook_max_size_kb').value);

   setValue('inhook_address', 'orders@example.com');
   setValue('inhook_url', 'https://crm.example/inbound');
   setValue('inhook_secret', 'an-inbound-secret');
   setChecked('inhook_deliver', true);
   click(act('inhooksave'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/inbound-webhooks');
   check('a new route is sent with its address, URL, secret and what was changed from the defaults',
      posted && posted.address === 'orders@example.com' && posted.url === 'https://crm.example/inbound' &&
      posted.secret === 'an-inbound-secret' && posted.deliver === true && posted.max_size_kb === 10240,
      JSON.stringify(posted));
   check('and the list comes back with it', content().textContent.indexOf('orders@example.com') >= 0 &&
      content().textContent.indexOf('posted and delivered') >= 0, content().textContent.slice(0, 300));

   before = requests.length;
   click(act('inhookedit', { id: 'support@example.com' }));
   await flush();
   check('editing one fills the form from what the server holds and hides the address',
      document.getElementById('inhook_url').value === 'https://helpdesk.example/hooks/mail' && document.getElementById('inhook_address') === null,
      content().textContent.slice(0, 160));
   setChecked('inhook_enabled', false);
   click(act('inhooksave'));
   await flush();
   put = lastBody(before, 'PUT', '/api/v1/inbound-webhooks/support%40example.com');
   check('a change is a PUT to its own address, and an untouched secret box sends no secret',
      put !== null && put.enabled === false && put.secret === undefined && put.address === undefined, JSON.stringify(put) + ' ' + paths(before));

   before = requests.length;
   click(act('inhookretry', { id: '41' }));
   await flush();
   check('Retry posts to the delivery\'s own retry route',
      called(before, 'POST', '/api/v1/inbound-webhooks/deliveries/41/retry').length === 1, paths(before));

   before = requests.length;
   click(act('inhookdrop', { id: '42' }));
   await flush();
   check('Remove asks first and deletes the delivery by its number',
      called(before, 'DELETE', '/api/v1/inbound-webhooks/deliveries/42').length === 1, paths(before));

   before = requests.length;
   click(act('inhookrun'));
   await flush();
   check('the pass button runs one and says what it sent',
      called(before, 'POST', '/api/v1/inbound-webhooks/run').length === 1 && toastText().indexOf('2') >= 0, toastText());

   before = requests.length;
   click(act('inhookdel', { id: 'orders@example.com' }));
   await flush();
   check('removing a route names it in the question and deletes it by its address',
      confirmations[confirmations.length - 1].indexOf('orders@example.com') >= 0 &&
      called(before, 'DELETE', '/api/v1/inbound-webhooks/orders%40example.com').length === 1, paths(before));

   const keptInbound = state.inboundHooks;
   const keptInboundQueue = state.inboundQueue;
   state.inboundHooks = [];
   state.inboundQueue = [];
   await goTo('inboundwebhooks');
   check('with no route the page says every message goes to a mailbox as it always has',
      content().textContent.indexOf('every message goes to a mailbox as it always has') >= 0 &&
      content().textContent.indexOf('Nothing has been queued yet') >= 0, content().textContent.slice(0, 200));
   state.inboundHooks = keptInbound;
   state.inboundQueue = keptInboundQueue;

   /* ---- the eight pages the Deck gained last ------------------------------
      API keys, the quarantine, the archive and its holds, the metric history,
      the update, the DNS records, a folder's access-control list and the
      diagnostic run: the last administrative pages that were in the desktop
      program and not here. Each is walked as a person walks it - read it, act
      on it, read what the page did - because the failure worth catching is a
      button that sends the wrong thing, not one that sends nothing. */
   {
      // ---- the API keys
      before = requests.length;
      await goTo('apikeys');
      check('the API keys view reads the keys once', called(before, 'GET', '/api/v1/apikeys').length === 1, paths(before));
      check('a row per key', rows().length === 2, String(rows().length));
      check('an expired key is marked and a key that names no domain and no address says so',
         rows()[1].textContent.indexOf('Expired') >= 0 && rows()[0].textContent.indexOf('every domain') >= 0 &&
         rows()[1].textContent.indexOf('anywhere') >= 0, rows().map((r) => r.textContent).join(' | '));
      check('the form says what the route makes of it',
         content().textContent.indexOf('token is returned once') >= 0, content().textContent.slice(0, 400));
      check('a key that sends shows its sender and its own ceilings, and a key that does not says so',
         rows()[1].textContent.indexOf('*@example.com') >= 0 && rows()[1].textContent.indexOf('10 a minute, 500 a day') >= 0 &&
         rows()[0].textContent.indexOf('does not send') >= 0, rows().map((r) => r.textContent).join(' | '));

      before = requests.length;
      setValue('key_label', 'the mailer');
      setValue('key_send_from', 'noreply@example.com');
      setValue('key_send_minute', 'two');
      click(act('keynew'));
      await flush();
      check('a sending ceiling that is not a whole number is refused here and never sent',
         called(before, 'POST', '/api/v1/apikeys').length === 0 && $('#err_keys').textContent.indexOf('whole numbers') >= 0,
         $('#err_keys').textContent + ' ' + paths(before));
      setValue('key_send_from', '');
      setValue('key_send_minute', '');

      before = requests.length;
      setValue('key_label', '  ');
      click(act('keynew'));
      await flush();
      check('a key with no label is refused here and never sent',
         called(before, 'POST', '/api/v1/apikeys').length === 0 && $('#err_keys').textContent.indexOf('label') >= 0,
         $('#err_keys').textContent + ' ' + paths(before));

      before = requests.length;
      setValue('key_label', 'the reporting job');
      setValue('key_scope', 'readonly');
      setValue('key_domains', 'example.com, second.example');
      setValue('key_expires', '2027-01-01');
      setValue('key_allowed', '192.0.2.0/24');
      click(act('keynew'));
      await flush();
      posted = lastBody(before, 'POST', '/api/v1/apikeys');
      check('making one sends the label, the scope, the domains, the expiry and where it may be used from',
         posted !== null && posted.label === 'the reporting job' && posted.scope === 'readonly' &&
         posted.domains === 'example.com, second.example' && posted.expires === '2027-01-01' && posted.allowed_from === '192.0.2.0/24',
         JSON.stringify(posted));
      check('and a key made without a sender sends no send_from and no ceilings',
         posted !== null && !('send_from' in posted) && !('send_per_minute' in posted) && !('send_per_day' in posted),
         JSON.stringify(posted));

      before = requests.length;
      setValue('key_label', 'the mailer');
      setValue('key_scope', 'full');
      setValue('key_domains', '');
      setValue('key_expires', '');
      setValue('key_allowed', '');
      setValue('key_send_from', 'noreply@example.com');
      setValue('key_send_minute', '2');
      setValue('key_send_day', '100');
      click(act('keynew'));
      await flush();
      posted = lastBody(before, 'POST', '/api/v1/apikeys');
      check('a key that sends carries its sender, and its two ceilings as numbers',
         posted !== null && posted.label === 'the mailer' && posted.send_from === 'noreply@example.com' &&
         posted.send_per_minute === 2 && posted.send_per_day === 100,
         JSON.stringify(posted));
      check('and the list shows what it sends as',
         rows().some((r) => r.textContent.indexOf('noreply@example.com') >= 0 && r.textContent.indexOf('2 a minute, 100 a day') >= 0),
         rows().map((r) => r.textContent).join(' | '));
      check('and the token is shown once, with the warning that it will not be again',
         $('#keyToken') !== null && $('#keyToken').textContent.indexOf('hmapi_') === 0 &&
         content().textContent.indexOf('not shown again') >= 0, $('#keyToken') && $('#keyToken').textContent);
      check('the new keys are in the list', rows().length === 4, String(rows().length));

      click(act('keycopy'));
      await flush();
      check('Copy the key puts the token on the clipboard', clipboard === $('#keyToken').textContent, String(clipboard));

      click(act('keyhide'));
      await flush();
      check('and Hide it takes the token off the page', $('#keyToken') === null);

      before = requests.length;
      click(act('keyrevoke', { id: 'a1b2c3d4e5f60718' }));
      await flush();
      check('revoking asks with the label in the question and revokes by id',
         confirmations[confirmations.length - 1].indexOf('the monitor') >= 0 &&
         called(before, 'DELETE', '/api/v1/apikeys/a1b2c3d4e5f60718').length === 1 && rows().length === 3,
         confirmations[confirmations.length - 1] + ' ' + paths(before));

      // ---- the quarantine
      before = requests.length;
      await goTo('quarantine');
      const qRows = () => rows().filter((r) => r.className.indexOf('qmsg') >= 0);
      const qGroups = () => rows().filter((r) => r.className.indexOf('qgroup') >= 0);
      const qText = () => rows().map((r) => r.textContent).join(' | ');
      const qBulk = (from) => called(from, 'POST', '/api/v1/quarantine/bulk').map((r) => JSON.parse(r.body));
      const regroup = async (value) => { setValue('qGroup', value); $('#qGroup').dispatchEvent(makeEvent('change')); await flush(); };
      // What an action came to, on its line: '' while the line is hidden, or missing.
      const outcome = () => $('#qOutcome');
      const outcomeShown = () => (outcome() !== null && outcome().hidden === false ? outcome().textContent : '');
      check('the quarantine view reads what is held, once, as a page that says how many are held in all',
         called(before, 'GET', /^\/api\/v1\/quarantine(\?|$)/).length === 1 && called(before, 'GET', '/api/v1/quarantine?offset=0&limit=1000').length === 1, paths(before));
      check('grouped by the sender\'s organization, as the server names it, the group with the newest message first',
         qGroups().length === 3 && qGroups()[0].textContent.indexOf('spammer.example') >= 0 && qGroups()[0].textContent.indexOf('2 messages') >= 0 &&
         qGroups()[1].textContent.indexOf('(no domain)') >= 0 && qGroups()[2].textContent.indexOf('supply.example') >= 0, qText());
      check('a row per held message under its group, with the HELO name, the score and the reason',
         qRows().length === 5 && qRows()[0].textContent.indexOf('mail.spammer.example') >= 0 && qRows()[0].textContent.indexOf('12') >= 0 &&
         qRows()[0].textContent.indexOf('SURBL') >= 0 && qRows()[1].textContent.indexOf('Weekly offers') >= 0, qText());
      check('a held message\'s subject and sender are drawn as text, tags and all',
         content().textContent.indexOf(HOSTILE) >= 0 && content().querySelector('img') === null, content().textContent.slice(0, 300));
      check('several recipients are the first and how many more, and the tooltip names every one',
         qRows()[1].textContent.indexOf('anna@example.com + 2 more') >= 0 &&
         qRows()[1].querySelectorAll('td').some((td) => String(td.attributes.title || '').indexOf('carol@example.com') >= 0), qRows()[1].innerHTML.slice(0, 600));
      check('a long sender is cut in its cell and whole in its tooltip',
         qRows()[1].querySelectorAll('td.qtrim').some((td) => td.attributes.title === 'offers@bounces.spammer.example'), qRows()[1].innerHTML.slice(0, 600));
      check('the box before each row, and the four icons, and the header\'s box and icons for the checked ones',
         content().querySelectorAll('button[data-act="qcheck"]').length === 1 + 3 + 5 &&
         content().querySelectorAll('button[data-act="qact"][data-scope="checked"]').length === 4 &&
         content().querySelectorAll('button[data-act="qact"][data-scope="group"]').length === 12 &&
         content().querySelectorAll('button[data-act="qact"][data-scope="row"]').length === 20, content().innerHTML.length);

      click(act('qsort', { col: 'score' }));
      await flush();
      check('a header sorts the messages inside each group, the largest score first, and leaves the groups where they were',
         qGroups()[0].textContent.indexOf('spammer.example') >= 0 && qRows()[0].textContent.indexOf('Your invoice') >= 0 &&
         qRows()[3].textContent.indexOf('Invoice 4417') >= 0 && content().querySelector('th[aria-sort="descending"]') !== null, qText());
      click(act('qsort', { col: 'score' }));
      await flush();
      check('and the same header again turns the order round',
         qRows()[0].textContent.indexOf('Weekly offers') >= 0 && content().querySelector('th[aria-sort="ascending"]') !== null, qText());

      await regroup('helo');
      check('grouped by the HELO name\'s organization instead', qGroups().length === 4 &&
         qGroups().some((r) => r.textContent.indexOf('localhost') >= 0) && qGroups().some((r) => r.textContent.indexOf('spammer-mail.example') >= 0), qText());
      await regroup('none');
      check('and not grouped at all: a row for each message and no group rows', qGroups().length === 0 && qRows().length === 5, qText());
      await regroup('sender');

      click(act('qfold', { key: 'spammer.example' }));
      await flush();
      check('a group folds: its messages go and its row stays, marked folded',
         qGroups().length === 3 && qRows().length === 3 && act('qfold', { key: 'spammer.example' }).attributes['aria-expanded'] === 'false', qText());
      click(act('qfold', { key: 'spammer.example' }));
      await flush();
      check('and unfolds', qRows().length === 5, qText());

      // The search box hides rows, not checks: the header's box takes in every
      // message, and an action over messages the search hides asks first.
      const search = async (text) => { $('#deckSearch').value = text; $('#deckSearch').dispatchEvent(makeEvent('input', { target: $('#deckSearch') })); await flush(); };
      await regroup('none');
      await search('invoice');
      check('the search leaves the two invoices of the five', qRows().filter((r) => !r.hidden).length === 2, qRows().filter((r) => !r.hidden).map((r) => r.textContent.slice(0, 40)).join(' | '));
      click(act('qcheck', { all: 1 }));
      await flush();
      before = requests.length;
      click(act('qact', { do: 'release', scope: 'checked' }));
      await flush();
      check('releasing the checked messages while the search hides three of them asks first, saying so, and No has the keyboard',
         $('#qAsk') !== null && qBulk(before).length === 0 && $('#qAskText').textContent.indexOf('The search hides 3 of the messages this acts on (5 in all).') >= 0 &&
         document.activeElement === act('qaskno'), $('#qAsk') ? $('#qAskText').textContent : 'no dialog; sent ' + JSON.stringify(qBulk(before)));
      if ($('#qAsk') !== null) { click(act('qaskno')); }
      await flush();
      check('and No releases nothing', $('#qAsk') === null && qBulk(before).length === 0, JSON.stringify(qBulk(before)));
      click(act('qact', { do: 'delete', scope: 'checked' }));
      await flush();
      check('deleting them says it in the one question a delete asks',
         $('#qAsk') !== null && $('#qAskText').textContent.indexOf('The search hides 3') === 0 && $('#qAskText').textContent.indexOf('for good') >= 0,
         $('#qAsk') ? $('#qAskText').textContent : 'no dialog');
      press('Escape', document.activeElement);
      await flush();
      await regroup('sender');
      click(act('qfold', { key: 'spammer.example' }));
      await flush();
      click(act('qact', { do: 'release', scope: 'checked' }));
      await flush();
      check('a folded group\'s messages are hidden with its row: four of the five here', $('#qAsk') !== null && $('#qAskText').textContent.indexOf('The search hides 4 ') === 0 && qBulk(before).length === 0,
         $('#qAsk') ? $('#qAskText').textContent : 'no dialog');
      press('Escape', document.activeElement);
      await flush();
      click(act('qfold', { key: 'spammer.example' }));
      await flush();
      click(act('qcheck', { all: 1 }));
      await flush();
      await search('');
      check('with the search emptied every row is back, and nothing is checked or sent', qRows().length === 5 && qRows().every((r) => !r.hidden) &&
         content().querySelectorAll('button[data-act="qcheck"][aria-checked="true"]').length === 0 && qBulk(before).length === 0, qText());

      before = requests.length;
      click(act('qact', { do: 'release', scope: 'checked' }));
      await flush();
      check('the header\'s icons with nothing checked say so and send nothing', qBulk(before).length === 0 &&
         $('#err_quarantine').textContent.indexOf('Check the messages first') >= 0, $('#err_quarantine').textContent);

      click(act('qcheck', { id: 41 }));
      await flush();
      check('checking one message of a group shows the group\'s box, and the header\'s, as partly checked',
         act('qcheck', { key: 'spammer.example' }).attributes['aria-checked'] === 'mixed' && act('qcheck', { all: 1 }).attributes['aria-checked'] === 'mixed' &&
         act('qcheck', { id: 41 }).attributes['aria-checked'] === 'true', content().innerHTML.length);
      click(act('qcheck', { key: 'spammer.example' }));
      await flush();
      check('and the group\'s box checks the rest of it', act('qcheck', { key: 'spammer.example' }).attributes['aria-checked'] === 'true' &&
         act('qcheck', { id: 43 }).attributes['aria-checked'] === 'true' && act('qcheck', { id: 44 }).attributes['aria-checked'] === 'false', content().innerHTML.length);

      // The headers and the preview: read from the server, shown as text.
      before = requests.length;
      // A browser puts the keyboard on the button it clicks; this DOM does not.
      const pressed = (button) => { button.focus(); click(button); return button; };
      const headersIcon = pressed(act('qread', { what: 'headers', id: 42 }));
      await flush();
      check('the headers icon reads that message\'s headers and shows them in a panel, as text',
         called(before, 'GET', '/api/v1/quarantine/42/headers').length === 1 && $('#qReadText') !== null &&
         $('#qReadText').textContent.indexOf('Subject: <b>held</b>') >= 0 && $('#qRead').querySelector('b') === null && $('#qRead').querySelector('img') === null,
         paths(before) + ' ' + ($('#qReadText') ? $('#qReadText').textContent : ''));
      click(act('qreadclose'));
      await flush();
      check('and Close takes the panel away, and gives the keyboard back to the icon that opened it', $('#qRead') === null && document.activeElement === headersIcon,
         String(document.activeElement && (document.activeElement.id || JSON.stringify(document.activeElement.attributes))));
      before = requests.length;
      const previewIcon = pressed(act('qread', { what: 'preview', id: 42 }));
      await flush();
      check('the preview icon reads the message\'s text, shows it as text, and says it was cut',
         called(before, 'GET', '/api/v1/quarantine/42/preview').length === 1 && $('#qReadText').textContent.indexOf('<script>') >= 0 &&
         $('#qRead').querySelector('script') === null && $('#qReadNote').hidden === false && $('#qReadNote').textContent.indexOf('16384') >= 0,
         paths(before) + ' ' + ($('#qReadNote') ? $('#qReadNote').textContent : ''));
      const paneBefore = $('#helpPane').hidden;
      const helpKey = makeEvent('keydown', { key: 'F1', target: $('#qReadText') });
      $('#qReadText').dispatchEvent(helpKey);
      await flush();
      check('F1 in the panel is kept from the browser, which would open its own help, and the panel stays',
         helpKey.defaultPrevented === true && $('#qRead') !== null && $('#helpPane').hidden === paneBefore, 'prevented=' + helpKey.defaultPrevented);
      press('Escape', $('#qReadText'));
      await flush();
      check('and Escape takes it away, the keyboard back on the icon', $('#qRead') === null && document.activeElement === previewIcon);

      // The list dialog, cancelled, gives the keyboard back too.
      const allowIcon = pressed(act('qact', { do: 'allow', scope: 'group', key: 'supply.example' }));
      await flush();
      check('the list dialog takes the keyboard', $('#qList') !== null && document.activeElement !== allowIcon);
      press('Escape', document.activeElement);
      await flush();
      check('and Escape cancels it, the keyboard back on the icon that opened it', $('#qList') === null && document.activeElement === allowIcon,
         String(document.activeElement && JSON.stringify(document.activeElement.attributes)));

      // Whitelist a group: which list, asked of the server before anything is written.
      before = requests.length;
      click(act('qact', { do: 'allow', scope: 'group', key: 'supply.example' }));
      await flush();
      let dry = qBulk(before);
      check('whitelisting a group asks which list, and asks the server what the default - the senders\' addresses - would write',
         $('#qList') !== null && dry.length === 1 && dry[0].dry_run === true && dry[0].action === 'allow' && dry[0].list === 'address' &&
         JSON.stringify(dry[0].ids) === '[44,45]' && $('#qPlan').textContent.indexOf('billing@supply.example') >= 0 && $('#qPlan').textContent.indexOf('news@supply.example') >= 0,
         JSON.stringify(dry) + ' ' + ($('#qPlan') ? $('#qPlan').textContent : ''));
      before = requests.length;
      click(act('qlistpick', { list: 'domain' }));
      await flush();
      dry = qBulk(before);
      check('the domain instead is the sender\'s organization on the domain white and black list, one entry for the group',
         dry.length === 1 && dry[0].dry_run === true && dry[0].list === 'domain' && dry[0].domain_from === 'sender' &&
         $('#qPlan').textContent.indexOf('.supply.example') >= 0 && $('#qListYes').disabled === false, JSON.stringify(dry) + ' ' + $('#qPlan').textContent);
      before = requests.length;
      click(act('qlistyes'));
      await flush();
      let sent = qBulk(before);
      check('and the button writes it and releases the group, in one request',
         sent.length === 1 && sent[0].action === 'allow' && sent[0].list === 'domain' && sent[0].domain_from === 'sender' && !sent[0].dry_run &&
         sent[0].confirm_public_suffix === undefined && state.quarantineListed.some((e) => e.value === '.supply.example' && !e.blocked) &&
         $('#qList') === null && qRows().length === 3, JSON.stringify(sent) + ' ' + qText());

      // Blacklist by the HELO name's domain, which is a public suffix here.
      await regroup('helo');
      before = requests.length;
      // The address's dry run is answered late, after the domain's: the
      // server answers each connection on a worker of its own.
      holdWhen = (method, path, body) => path === '/api/v1/quarantine/bulk' && /"list":"address"/.test(body || '') && /"dry_run":true/.test(body || '');
      click(act('qact', { do: 'block', scope: 'group', key: 'localhost' }));
      await flush();
      holdWhen = null;
      check('blacklisting asks the same question, and Cancel has the keyboard because it deletes',
         $('#qList') !== null && document.activeElement === act('qlistno') && $('#qListTitle').textContent.indexOf('Blacklist') >= 0, String(document.activeElement && document.activeElement.attributes['data-act']));
      check('while the server has not answered, the plan says so and the button waits', heldReplies.length === 1 && $('#qPlan').textContent === '…' && $('#qListYes').disabled === true,
         heldReplies.length + ' held, plan ' + JSON.stringify($('#qPlan').textContent));
      click(act('qlistpick', { list: 'domain' }));
      await flush();
      check('grouped by HELO name, the domain is the HELO\'s: a public suffix is named, and the button waits for the box',
         qBulk(before).some((b) => b.list === 'domain' && b.domain_from === 'helo' && b.dry_run === true) &&
         $('#qPlan').textContent.indexOf('public suffix') >= 0 && $('#qSuffix').hidden === false && $('#qListYes').disabled === true,
         $('#qPlan').textContent);
      heldReplies.splice(0).forEach((deliver) => deliver());
      await flush();
      check('the address\'s answer arriving after the domain\'s is dropped: the plan, the warning and the box stay the domain\'s',
         act('qlistpick', { list: 'domain' }).attributes['aria-checked'] === 'true' && $('#qPlan').textContent.indexOf('public suffix') >= 0 &&
         $('#qPlan').textContent.indexOf('.localhost') >= 0 && $('#qPlan').textContent.indexOf('@') < 0 && $('#qSuffix').hidden === false && $('#qListYes').disabled === true,
         $('#qPlan').textContent);
      click(act('qlistsuffix'));
      await flush();
      check('the box ticked, the button is there to press', $('#qSuffix').attributes['aria-checked'] === 'true' && $('#qListYes').disabled === false);
      before = requests.length;
      click(act('qlistyes'));
      await flush();
      sent = qBulk(before);
      check('and it sends the confirmation with the request, blocks the name and deletes the message',
         sent.length === 1 && sent[0].action === 'block' && sent[0].confirm_public_suffix === true &&
         state.quarantineListed.some((e) => e.value === '.localhost' && e.blocked && e.match_on === 'helo') && qRows().length === 2, JSON.stringify(sent));
      await regroup('sender');

      // Delete asks the page's own question, whose default is No.
      const confirmsBefore = confirmations.length;
      before = requests.length;
      click(act('qact', { do: 'delete', scope: 'row', id: 41 }));
      await flush();
      check('deleting asks first, in a dialog of the page\'s own rather than the browser\'s confirm, which answers OK to Enter',
         $('#qAsk') !== null && confirmations.length === confirmsBefore && qBulk(before).length === 0 &&
         $('#qAskText').textContent.indexOf('for good') >= 0, $('#qAsk') ? $('#qAsk').textContent : 'no dialog');
      check('and No has the keyboard when it opens', document.activeElement === act('qaskno'), String(document.activeElement && document.activeElement.attributes['data-act']));
      press('Enter', document.activeElement);
      await flush();
      check('Enter there is not a way past it: the page\'s own Enter does not act behind the dialog', $('#qAsk') !== null && qBulk(before).length === 0);
      press('Escape', document.activeElement);
      await flush();
      check('Escape is No: the dialog goes and nothing is sent', $('#qAsk') === null && qBulk(before).length === 0, paths(before));
      const deleteIcon = pressed(act('qact', { do: 'delete', scope: 'row', id: 41 }));
      await flush();
      click(act('qaskno'));
      await flush();
      check('No gives the keyboard back to the icon that asked', $('#qAsk') === null && document.activeElement === deleteIcon && qBulk(before).length === 0,
         String(document.activeElement && JSON.stringify(document.activeElement.attributes)));
      click(act('qact', { do: 'delete', scope: 'row', id: 41 }));
      await flush();
      click(act('qaskyes'));
      await flush();
      sent = qBulk(before);
      check('Yes deletes it, through the bulk request, and the page reads what is held again',
         sent.length === 1 && sent[0].action === 'delete' && JSON.stringify(sent[0].ids) === '[41]' && called(before, 'GET', /^\/api\/v1\/quarantine(\?|$)/).length >= 1 && qRows().length === 1,
         JSON.stringify(sent) + ' ' + qText());

      // The header's release acts on what is still checked, in one request.
      before = requests.length;
      click(act('qact', { do: 'release', scope: 'checked' }));
      await flush();
      sent = qBulk(before);
      check('the header\'s release sends every checked message in one request, not one per message, and says what happened',
         sent.length === 1 && sent[0].action === 'release' && JSON.stringify(sent[0].ids) === '[43]' && toastText().indexOf('recipients') >= 0, JSON.stringify(sent) + ' ' + toastText());
      check('with nothing held the page says so', content().textContent.indexOf('Nothing is held') >= 0, content().textContent.slice(0, 200));

      before = requests.length;
      click(act('qexpire'));
      await flush();
      check('Remove expired runs the sweep and says what it removed', called(before, 'POST', '/api/v1/quarantine/expire').length === 1 &&
         content().textContent.indexOf('Nothing was old enough') >= 0, paths(before));
      // The window is a setting the server keeps in its database: a hand edit
      // of hMailServer.ini is put back, so the sentence names where it is set.
      check('and names where the window is set, not a file the server puts back when edited',
         outcomeShown().indexOf('QuarantineRetentionDays') >= 0 && outcomeShown().indexOf('hMailServer.ini') < 0 &&
         outcomeShown().indexOf('Anti-spam settings page') >= 0 && outcomeShown().indexOf('PUT /api/v1/settings/ini/QuarantineRetentionDays') >= 0,
         outcomeShown());
      check('on a line of its own, a status, under the line that says how many are held',
         outcome() !== null && outcome().hidden === false && outcome().attributes.role === 'status' && $('#qSaid').textContent === '0 messages held.',
         (outcome() ? JSON.stringify(outcome().attributes) : 'no outcome line') + ' // ' + $('#qSaid').textContent);

      // The search of every held message (review finding F261): while the list
      // holds only the newest of what is held, the box asks the server a moment
      // after the typing stops, draws what it answers whole and says so.
      state.quarantine = [
         { id: 61, sender: 'promo@shop.example', recipients: 'anna@example.com', subject: 'Spring sale', reason: 'Score 9 (DNSBL)', score: 9, size: 2048, created: '2026-10-12 10:00:00',
           helo: 'mx.shop.example', sender_domain: 'shop.example', helo_domain: 'shop.example' },
         { id: 60, sender: 'billing@supply.example', recipients: 'anna@example.com', subject: 'Invoice 5012', reason: 'Score 22 (SURBL)', score: 22, size: 8192, created: '2026-10-12 09:00:00',
           helo: 'mx.supply.example', sender_domain: 'supply.example', helo_domain: 'supply.example' }
      ];
      state.quarantineOlder = [
         { id: 7, sender: 'old@archive.example', recipients: 'anna@example.com,ops_team@example.com', subject: 'An old invoice', reason: 'Score 30 (SURBL)', score: 30, size: 4096, created: '2026-09-01 08:00:00',
           helo: 'mx.archive.example', sender_domain: 'archive.example', helo_domain: 'archive.example' }
      ];
      before = requests.length;
      click(act('qrefresh'));
      await flush();
      check('a list holding the newest of what is held says how many are held and that a search looks through every one',
         called(before, 'GET', '/api/v1/quarantine?offset=0&limit=1000').length === 1 && qRows().length === 2 &&
         $('#qSaid').textContent === 'Held: 3. Listed here: the most recent 2; a search looks through every held message.', paths(before) + ' ' + $('#qSaid').textContent);
      before = requests.length;
      await search('ops_team');
      check('a search asks nothing at once, and says it is searching', called(before, 'GET', /\?q=/).length === 0 &&
         $('#qSaid').textContent === 'Searching every held message on the server…', paths(before) + ' ' + $('#qSaid').textContent);
      fireTimers();
      await flush();
      check('a moment after the typing stops it asks the server over every held message',
         called(before, 'GET', '/api/v1/quarantine?q=ops_team&limit=1000').length === 1, paths(before));
      check('and draws what it answers whole - a match on a second recipient, which the row shows only as "+ 1 more"',
         qRows().length === 1 && !qRows()[0].hidden && qRows()[0].textContent.indexOf('An old invoice') >= 0 && qRows()[0].textContent.indexOf('ops_team') < 0, qText());
      check('saying it searched every held message', $('#qSaid').textContent === 'Searched every held message on the server. Matches: 1.', $('#qSaid').textContent);
      // Message 7 is held beyond the list's page, and only the answer holds it: its
      // panels are titled with its own sender and subject, not the null sender's.
      click(act('qread', { what: 'headers', id: 7 }));
      await flush();
      check('the headers of a message only the answer holds are titled with its sender',
         $('#qReadTitle') !== null && $('#qReadTitle').textContent === 'Headers of the message from old@archive.example',
         $('#qReadTitle') ? $('#qReadTitle').textContent : 'no panel');
      click(act('qreadclose'));
      await flush();
      click(act('qread', { what: 'preview', id: 7 }));
      await flush();
      check('and its preview with its subject',
         $('#qReadTitle') !== null && $('#qReadTitle').textContent === 'Preview of "An old invoice"',
         $('#qReadTitle') ? $('#qReadTitle').textContent : 'no panel');
      click(act('qreadclose'));
      await flush();
      // The text changes while the answer for the old one is drawn: until the next
      // answer comes, the rows shown are the list's own, narrowed by the box, as the
      // header's box and "the checked messages" read them - not the old answer's
      // row, which matches neither text.
      await search('ops_teamx');
      check('while the next answer is awaited the rows shown are the list\'s, narrowed by the box',
         qRows().filter((r) => !r.hidden).length === 0 && qRows().every((r) => !r.dataset.keep) && $('#qSaid').textContent === 'Searching every held message on the server…',
         qRows().filter((r) => !r.hidden).map((r) => r.textContent.slice(0, 80)).join(' | ') + ' // ' + $('#qSaid').textContent);
      before = requests.length;
      fireTimers();
      await flush();
      check('and its answer is asked for once the typing stops',
         called(before, 'GET', '/api/v1/quarantine?q=ops_teamx&limit=1000').length === 1 && $('#qSaid').textContent === 'Searched every held message on the server. Matches: 0.',
         paths(before) + ' ' + $('#qSaid').textContent);
      // Back to the text answered before: its answer is drawn again, asked for once more
      // only after the typing stops.
      before = requests.length;
      await search('ops_team');
      fireTimers();
      await flush();
      check('the text answered before is asked again and its answer drawn whole',
         called(before, 'GET', '/api/v1/quarantine?q=ops_team&limit=1000').length === 1 && qRows().length === 1 && !qRows()[0].hidden &&
         qRows()[0].textContent.indexOf('An old invoice') >= 0, paths(before) + ' ' + qText());
      before = requests.length;
      await search('');
      fireTimers();
      await flush();
      check('the box emptied gives the list back and asks nothing',
         qRows().length === 2 && qRows().every((r) => !r.hidden) && called(before, 'GET', /\?q=/).length === 0, paths(before) + ' ' + qText());
      state.quarantineSearchFails = true;
      await search('invoice');
      fireTimers();
      await flush();
      check('a search the server cannot answer says so, and the list\'s own matches stay',
         $('#qSaid').textContent.indexOf('Could not search every held message on the server, so the search looks at the most recent only: the quarantine could not be searched') === 0 &&
         qRows().filter((r) => !r.hidden).length === 1, $('#qSaid').textContent + ' ' + qText());
      state.quarantineSearchFails = false;
      await search('');
      fireTimers();
      await flush();
      // A server older than the search answers the bare list, which the box narrows as it is drawn.
      state.quarantineOldServer = true;
      click(act('qrefresh'));
      await flush();
      before = requests.length;
      await search('invoice');
      fireTimers();
      await flush();
      check('a server older than the search is not asked: its list is narrowed as it is drawn',
         called(before, 'GET', /\?q=/).length === 0 && qRows().filter((r) => !r.hidden).length === 1 && $('#qSaid').textContent === '2 messages held.',
         paths(before) + ' ' + $('#qSaid').textContent);
      state.quarantineOldServer = false;
      state.quarantineOlder = [];
      await search('');
      fireTimers();
      await flush();

      // What an action came to has a line of its own, which the line over the table
      // never writes: that one says how many are held and what the search found, and
      // the search says it again as the reload an action makes asks again. So what a
      // release left where it was and why, and what Remove expired removed, stay on
      // screen through that reload and the answer it asks for, with text in the box,
      // whether the list holds the newest of what is held or every message - and go
      // when the box's text is changed, when the next action is sent, on Refresh, and
      // when the view is opened again.
      const spring = state.quarantine.filter((m) => m.id === 61)[0];
      const invoice5012 = state.quarantine.filter((m) => m.id === 60)[0];
      state.quarantineOlder = [
         { id: 7, sender: 'old@archive.example', recipients: 'anna@example.com', subject: 'An old invoice', reason: 'Score 30 (SURBL)', score: 30, size: 4096, created: '2026-09-01 08:00:00',
           helo: 'mx.archive.example', sender_domain: 'archive.example', helo_domain: 'archive.example' },
         { id: 6, sender: 'old@archive.example', recipients: 'anna@example.com', subject: 'Invoice reminder', reason: 'Score 28 (SURBL)', score: 28, size: 4096, created: '2026-08-31 08:00:00',
           helo: 'mx.archive.example', sender_domain: 'archive.example', helo_domain: 'archive.example' },
         { id: 5, sender: 'deals@outlet.example', recipients: 'bob@example.com', subject: 'Clearance', reason: 'Score 11 (DNSBL)', score: 11, size: 2048, created: '2026-08-30 08:00:00',
           helo: 'mx.outlet.example', sender_domain: 'outlet.example', helo_domain: 'outlet.example' }
      ];
      click(act('qrefresh'));
      await flush();
      await search('invoice');
      fireTimers();
      await flush();
      check('the search of every held message draws the three invoices the server finds, and there is no outcome to say',
         qRows().length === 3 && qRows().every((r) => !r.hidden) && $('#qSaid').textContent === 'Searched every held message on the server. Matches: 3.' &&
         outcome() !== null && outcome().hidden === true, qText() + ' // ' + $('#qSaid').textContent);
      // A release of the three, one of which another administrator released meanwhile.
      click(act('qcheck', { all: 1 }));
      await flush();
      state.quarantineOlder = state.quarantineOlder.filter((m) => m.id !== 7);
      before = requests.length;
      click(act('qact', { do: 'release', scope: 'checked' }));
      await flush();
      const refusedPartly = 'Released 2 messages to their recipients. Left where they were: 1. The first reason: No quarantined message with that id.';
      check('a release the server partly refuses says what it left where it was, and why, while the reload asks the search again',
         qBulk(before).length === 1 && JSON.stringify(qBulk(before)[0].ids.slice().sort((a, b) => a - b)) === '[6,7,60]' && toastText() === 'Released 2 messages to their recipients.' &&
         outcomeShown() === refusedPartly && $('#qSaid').textContent === 'Searching every held message on the server…',
         JSON.stringify(qBulk(before)) + ' // ' + outcomeShown() + ' // ' + $('#qSaid').textContent);
      fireTimers();
      await flush();
      check('and still says it once the answer asked again is drawn',
         outcomeShown() === refusedPartly && $('#qSaid').textContent === 'Searched every held message on the server. Matches: 0.' &&
         content().textContent.indexOf('No held message matches the search.') >= 0, outcomeShown() + ' // ' + $('#qSaid').textContent);
      await regroup('none');
      check('a redraw of the table, here grouped otherwise, keeps it', outcomeShown() === refusedPartly, outcomeShown());
      await regroup('sender');
      // Remove expired, with the same text in the box.
      state.quarantineExpired = 2;
      before = requests.length;
      click(act('qexpire'));
      await flush();
      check('Remove expired says what it removed while the reload asks the search again',
         called(before, 'POST', '/api/v1/quarantine/expire').length === 1 && outcomeShown() === '2 message(s) past the retention window were removed.' &&
         $('#qSaid').textContent === 'Searching every held message on the server…', outcomeShown() + ' // ' + $('#qSaid').textContent);
      fireTimers();
      await flush();
      check('and once the answer is drawn',
         outcomeShown() === '2 message(s) past the retention window were removed.' && $('#qSaid').textContent === 'Searched every held message on the server. Matches: 0.',
         outcomeShown() + ' // ' + $('#qSaid').textContent);
      state.quarantineExpired = 0;
      await search('invoices');
      check('a change of the box\'s text takes the outcome away', outcome() !== null && outcome().hidden === true, outcomeShown());
      fireTimers();
      await flush();
      // A list holding every held message, which the box narrows as it is drawn.
      state.quarantine = [spring, invoice5012,
         { id: 59, sender: 'billing@supply.example', recipients: 'bob@example.com', subject: 'Invoice 5013', reason: 'Score 21 (SURBL)', score: 21, size: 8192, created: '2026-10-12 08:00:00',
           helo: 'mx.supply.example', sender_domain: 'supply.example', helo_domain: 'supply.example' }];
      state.quarantineOlder = [];
      click(act('qrefresh'));
      await flush();
      await search('invoice');
      fireTimers();
      await flush();
      state.quarantine = state.quarantine.filter((m) => m.id !== 60);
      before = requests.length;
      click(act('qact', { do: 'release', scope: 'row', id: 60 }));
      await flush();
      check('with every held message listed, a release the server refuses says why, the box holding a search',
         qBulk(before).length === 1 && called(before, 'GET', /\?q=/).length === 0 && $('#qSaid').textContent === '2 messages held.' &&
         outcomeShown() === 'Released 0 messages to their recipients. Left where they were: 1. The first reason: No quarantined message with that id.',
         outcomeShown() + ' // ' + $('#qSaid').textContent);
      before = requests.length;
      click(act('qexpire'));
      await flush();
      check('and so does Remove expired', called(before, 'POST', '/api/v1/quarantine/expire').length === 1 &&
         outcomeShown().indexOf('Nothing was old enough to remove.') === 0 && $('#qSaid').textContent === '2 messages held.', outcomeShown() + ' // ' + $('#qSaid').textContent);
      click(act('qrefresh'));
      await flush();
      check('Refresh takes the outcome away', outcome() !== null && outcome().hidden === true && $('#qSaid').textContent === '2 messages held.', outcomeShown());
      click(act('qexpire'));
      await flush();
      before = requests.length;
      holdWhen = (method, path) => path === '/api/v1/quarantine/bulk';
      click(act('qact', { do: 'release', scope: 'row', id: 59 }));
      await flush();
      holdWhen = null;
      check('the next action takes it away as it is sent, before its answer comes',
         heldReplies.length === 1 && qBulk(before).length === 1 && outcome() !== null && outcome().hidden === true, heldReplies.length + ' held // ' + outcomeShown());
      heldReplies.splice(0).forEach((deliver) => deliver());
      await flush();
      check('and a release that leaves nothing behind says only its toast',
         JSON.stringify(qBulk(before)[0].ids) === '[59]' && toastText() === 'Released to its recipients' && outcome() !== null && outcome().hidden === true,
         outcomeShown() + ' // ' + toastText());
      // Remove expired takes it away as it is sent too; and an answer that comes after
      // the view was left is not said when it is opened again: that reload is not the
      // action's.
      click(act('qexpire'));
      await flush();
      holdWhen = (method, path) => path === '/api/v1/quarantine/expire';
      click(act('qexpire'));
      await flush();
      holdWhen = null;
      check('Remove expired sent takes the outcome before it away as well', heldReplies.length === 1 && outcome() !== null && outcome().hidden === true,
         heldReplies.length + ' held // ' + outcomeShown());
      await goTo('apikeys');
      heldReplies.splice(0).forEach((deliver) => deliver());
      await flush();
      await goTo('quarantine');
      check('an action answered after the view was left says nothing when the view is opened again',
         outcome() !== null && outcome().hidden === true && $('#qSaid').textContent === '1 message held.', outcomeShown() + ' // ' + $('#qSaid').textContent);
      state.quarantine = [];
      holdWhen = (method, path) => path === '/api/v1/quarantine/bulk';
      click(act('qact', { do: 'release', scope: 'row', id: 61 }));
      await flush();
      holdWhen = null;
      await goTo('apikeys');
      heldReplies.splice(0).forEach((deliver) => deliver());
      await flush();
      await goTo('quarantine');
      check('and nor does a release answered after the view was left, though the server refused it',
         outcome() !== null && outcome().hidden === true && $('#qSaid').textContent === '0 messages held.', outcomeShown() + ' // ' + $('#qSaid').textContent);
      state.quarantine = [spring];

      click(act('qdigest'));
      await flush();
      check('Digest settings goes to the settings with the digest\'s own filtered in', $('#viewTitle').textContent === 'Settings' &&
         $('#setFilter') !== null && $('#setFilter').value === 'quarantine_digest', $('#viewTitle').textContent);

      // ---- held for review: the compromised-account detection
      before = requests.length;
      await goTo('review');
      check('the review view reads what is held, once, and nothing about any account until asked',
         called(before, 'GET', '/api/v1/review').length === 1 && called(before, 'GET', /\/review$/).length === 1, paths(before));
      check('a row per held message, with the account, the reason and the way it came in', rows().length === 2 &&
         rows()[0].textContent.indexOf('anna@example.com') >= 0 && rows()[0].textContent.indexOf('never written to') >= 0 && rows()[0].textContent.indexOf('smtp') >= 0,
         rows().map((r) => r.textContent).join(' | '));
      check('a held message\'s subject is drawn as text, tags and all',
         content().textContent.indexOf(HOSTILE) >= 0 && content().querySelector('img') === null, content().textContent.slice(0, 300));
      check('the page says what the mode is', content().textContent.indexOf('enforce') >= 0, content().textContent.slice(0, 300));

      before = requests.length;
      click(act('rvrelease', { id: 7 }));
      await flush();
      check('releasing posts to the message\'s own release route and reads the list again',
         called(before, 'POST', '/api/v1/review/7/release').length === 1 && called(before, 'GET', '/api/v1/review').length === 1 && rows().length === 1, paths(before));
      check('and says where it went', toastText().indexOf('queue') >= 0, toastText());

      before = requests.length;
      click(act('rvdiscard', { id: 8 }));
      await flush();
      check('discarding one asks first, because nothing brings it back',
         confirmations[confirmations.length - 1].indexOf('for good') >= 0, confirmations[confirmations.length - 1]);
      check('and discards it', called(before, 'DELETE', '/api/v1/review/8').length === 1, paths(before));
      check('with nothing held for review the page says so', content().textContent.indexOf('Nothing is held for review') >= 0, content().textContent.slice(0, 200));

      before = requests.length;
      setValue('rv_address', 'anna@example.com');
      click(act('rvlookup'));
      await flush();
      check('looking an account up reads its review state by its address',
         called(before, 'GET', '/api/v1/accounts/anna%40example.com/review').length === 1, paths(before));
      check('and draws the lock, the shape and the baseline',
         content().textContent.indexOf('Locked since') >= 0 && content().textContent.indexOf('volume, recipients') >= 0 &&
         content().textContent.indexOf('192.0.2.0/24') >= 0 && content().textContent.indexOf('212') >= 0, content().textContent.slice(-900));

      before = requests.length;
      setChecked('rv_exempt', true);
      setValue('rv_sensitivity', 'relaxed');
      click(act('rvsave', { address: 'anna@example.com' }));
      await flush();
      const reviewPut = lastBody(before, 'PUT', '/api/v1/accounts/anna%40example.com/review');
      check('saving puts exactly the two choices, exempt and sensitivity, and reads the state back',
         reviewPut && reviewPut.exempt === true && reviewPut.sensitivity === 'relaxed' && Object.keys(reviewPut).length === 2 &&
         called(before, 'GET', '/api/v1/accounts/anna%40example.com/review').length === 1, JSON.stringify(reviewPut) + ' ' + paths(before));
      check('and the read-back shows the exemption', document.getElementById('rv_exempt').checked === true && document.getElementById('rv_sensitivity').value === 'relaxed');

      before = requests.length;
      click(act('rvunlock', { address: 'anna@example.com' }));
      await flush();
      check('the unlock posts to the review unlock route, which clears the compromise lock alone',
         called(before, 'POST', '/api/v1/accounts/anna%40example.com/review/unlock').length === 1 && called(before, 'POST', /\/unlock$/).length === 1, paths(before));
      check('and once unlocked the button is gone', content().querySelectorAll('button[data-act="rvunlock"]').length === 0 && content().textContent.indexOf('Not locked') >= 0,
         content().textContent.slice(-400));

      // ---- the archive and its holds
      before = requests.length;
      await goTo('archive');
      check('the archive view searches nothing until it is asked to',
         called(before, 'GET', /^\/api\/v1\/archive/).length === 0 &&
         content().textContent.indexOf('press Search') >= 0, paths(before));

      before = requests.length;
      setValue('arc_subject', 'contract');
      setValue('arc_limit', '50');
      click(act('arcsearch'));
      await flush();
      check('Search sends what the form holds, in the query string',
         called(before, 'GET', /^\/api\/v1\/archive\?.*subject=contract.*limit=50/).length === 1, paths(before));
      check('a row per copy, the held one marked', rows().length === 2 &&
         rows().filter((r) => r.textContent.indexOf('On hold') >= 0).length === 1,
         rows().map((r) => r.textContent).join(' | '));

      before = requests.length;
      click(act('archold', { id: 2750 }));
      await flush();
      check('putting a copy on hold posts to its hold route and searches again with the same query',
         called(before, 'POST', '/api/v1/archive/2750/hold').length === 1 &&
         called(before, 'GET', /^\/api\/v1\/archive\?.*subject=contract/).length === 1, paths(before));
      check('and says what a hold means', toastText().indexOf('retention') >= 0, toastText());
      check('both copies are now held', rows().filter((r) => r.textContent.indexOf('On hold') >= 0).length === 2,
         rows().map((r) => r.textContent).join(' | '));

      before = requests.length;
      click(act('arcunhold', { id: 2751 }));
      await flush();
      check('lifting one is a DELETE of the same route',
         called(before, 'DELETE', '/api/v1/archive/2751/hold').length === 1 &&
         rows().filter((r) => r.textContent.indexOf('On hold') >= 0).length === 1, paths(before));

      setValue('arc_subject', 'nothing like this was ever sent');
      click(act('arcsearch'));
      await flush();
      check('a search that matches nothing says so rather than showing an empty table',
         content().textContent.indexOf('Nothing in the archive matches') >= 0, content().textContent.slice(0, 300));

      // ---- eDiscovery: the cases, and one case's searches, holds and exports
      before = requests.length;
      await goTo('discovery');
      check('the discovery view lists the cases, the closed one marked and the live holds counted',
         called(before, 'GET', '/api/v1/discovery/cases').length === 1 && rows().length === 2 &&
         rows()[0].textContent.indexOf('Northwind v. Example') >= 0 && rows()[0].textContent.indexOf('Closed') < 0 &&
         rows()[1].textContent.indexOf('An old matter') >= 0 && rows()[1].textContent.indexOf('Closed') >= 0,
         rows().map((r) => r.textContent).join(' | '));

      before = requests.length;
      setValue('dsc_name', 'Acme matter');
      setValue('dsc_description', 'The notice of 20 September');
      click(act('dscnew'));
      await flush();
      const opened = lastBody(before, 'POST', '/api/v1/discovery/cases');
      check('Open a case posts its name and description, and opens the case it made',
         opened && opened.name === 'Acme matter' && opened.description === 'The notice of 20 September' && Object.keys(opened).length === 2 &&
         called(before, 'GET', '/api/v1/discovery/cases/100').length === 1 && content().textContent.indexOf('Acme matter') >= 0 &&
         content().querySelectorAll('button[data-act="dscsearch"]').length === 1, paths(before));

      before = requests.length;
      setValue('dsc_custodians', 'anna@example.com, example.test,');
      setValue('dsc_words', 'contract');
      setValue('dsc_from', '2026-09-01');
      setValue('dsc_attname', 'invoice');
      click(act('dscsearch'));
      await flush();
      const searched = lastBody(before, 'POST', '/api/v1/discovery/cases/100/searches');
      check('Run the search posts the custodians as a list and only the criteria given, the attachment\'s under attachment',
         searched && JSON.stringify(searched.custodians) === '["anna@example.com","example.test"]' && searched.scope === 'both' &&
         searched.words === 'contract' && searched.from === '2026-09-01' && !('to' in searched) &&
         JSON.stringify(searched.attachment) === '{"name":"invoice"}', JSON.stringify(searched));
      check('and says it is queued, shows it as queued, and reads the case again while it is',
         toastText().indexOf('one search or export at a time') >= 0 && rows()[0].textContent.indexOf('queued') >= 0 &&
         timers.some((x) => x.ms === 3000), toastText() + ' ' + rows().map((r) => r.textContent).join(' | '));
      state.discovery.cases[0].searches[0].state = 'done';
      state.discovery.cases[0].searches[0].matched = 2;

      before = requests.length;
      setValue('dsc_hold', 'bob@example.com');
      click(act('dschold'));
      await flush();
      setValue('dsc_hold', 'example.test');
      click(act('dschold'));
      await flush();
      const holdsPosted = called(before, 'POST', '/api/v1/discovery/cases/100/holds').map((r) => JSON.parse(r.body));
      check('a hold on an address is an account hold and one on a name is a domain hold',
         holdsPosted.length === 2 && holdsPosted[0].scope === 'account' && holdsPosted[0].target === 'bob@example.com' &&
         holdsPosted[1].scope === 'domain' && holdsPosted[1].target === 'example.test', JSON.stringify(holdsPosted));
      const placedHolds = state.discovery.cases[0].holds;
      check('and both are drawn on hold, each with Lift the hold',
         content().querySelectorAll('button[data-act="dscrelease"]').length === 2 &&
         rows().filter((r) => r.textContent.indexOf('On hold') >= 0).length === 2, rows().map((r) => r.textContent).join(' | '));
      check('each with its scope in the reader\'s words (Account, Domain), not the server\'s',
         rows().some((r) => r.textContent.indexOf('bob@example.com') >= 0 && r.textContent.indexOf('Account') >= 0 && r.textContent.indexOf('On hold') >= 0) &&
         rows().some((r) => r.textContent.indexOf('example.test') >= 0 && r.textContent.indexOf('Domain') >= 0 && r.textContent.indexOf('On hold') >= 0),
         rows().map((r) => r.textContent).join(' | '));

      before = requests.length;
      click(act('dscrelease', { id: placedHolds[0].id }));
      await flush();
      check('Lift the hold is a DELETE of that hold, and the hold stays listed, lifted',
         called(before, 'DELETE', '/api/v1/discovery/cases/100/holds/' + placedHolds[0].id).length === 1 &&
         content().querySelectorAll('button[data-act="dscrelease"]').length === 1 &&
         rows().filter((r) => r.textContent.indexOf('Hold lifted') >= 0).length === 1, paths(before));

      before = requests.length;
      click(act('dscholdarchive', { id: state.discovery.cases[0].searches[0].id }));
      await flush();
      check('a finished search offers its archived copies for a hold, which names the search',
         JSON.stringify(lastBody(before, 'POST', '/api/v1/discovery/cases/100/holds')) === '{"scope":"archive","search":' + state.discovery.cases[0].searches[0].id + '}', paths(before));

      before = requests.length;
      confirmAnswer = false;
      click(act('dscclose'));
      await flush();
      check('closing asks first, naming the case, and no means nothing is sent',
         confirmations[confirmations.length - 1].indexOf('Acme matter') >= 0 && confirmations[confirmations.length - 1].indexOf('released') >= 0 &&
         called(before, 'POST', /\/close$/).length === 0, confirmations[confirmations.length - 1] + ' ' + paths(before));
      confirmAnswer = true;
      click(act('dscclose'));
      await flush();
      check('yes closes it; a closed case is marked and offers no search, hold, export or close',
         called(before, 'POST', '/api/v1/discovery/cases/100/close').length === 1 && content().textContent.indexOf('Closed') >= 0 &&
         content().querySelectorAll('button[data-act="dscsearch"]').length === 0 && content().querySelectorAll('button[data-act="dschold"]').length === 0 &&
         content().querySelectorAll('button[data-act="dscexport"]').length === 0 && content().querySelectorAll('button[data-act="dscclose"]').length === 0 &&
         content().querySelectorAll('button[data-act="dscrelease"]').length === 0, paths(before));

      before = requests.length;
      click(act('dscback'));
      await flush();
      click(act('dscopen', { id: 41 }));
      await flush();
      check('Back lists the cases again, and Open reads one case',
         called(before, 'GET', '/api/v1/discovery/cases').length === 1 && called(before, 'GET', '/api/v1/discovery/cases/41').length === 1, paths(before));
      const downloads = content().querySelectorAll('a[download]');
      // Drawn as the browser resolves it against the page (safeHref), so the
      // address checked is the address followed.
      check('a finished export is a download link to its own route, named for the case and the export, with its SHA-256 beside it',
         downloads.length === 1 && downloads[0].attributes.href === 'https://deck.test/api/v1/discovery/cases/41/exports/5' &&
         downloads[0].attributes.download === 'case-41-export-5.zip' &&
         content().textContent.indexOf('9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08') >= 0 &&
         content().textContent.indexOf('1200 read, 14 matched, 1 not searched') >= 0, JSON.stringify(downloads.map((a) => a.attributes)));

      before = requests.length;
      click(act('dscexport', { id: 7 }));
      await flush();
      check('Export posts to the search\'s export route and shows the export queued',
         called(before, 'POST', '/api/v1/discovery/cases/41/searches/7/export').length === 1 &&
         toastText().indexOf('one search or export at a time') >= 0 && content().querySelectorAll('a[download]').length === 1, paths(before));

      // An export's download is a link only as a web address: the server
      // builds it as a path of this origin, and anything else is no link.
      {
         const finished = state.discovery.cases.filter((c) => c.id === 41)[0].exports.filter((x) => x.id === 5)[0];
         const downloadWas = finished.download;
         for (const address of ['javascript:void(0)', 'data:application/zip,export', '']) {
            finished.download = address;
            click(act('dscback'));
            await flush();
            click(act('dscopen', { id: 41 }));
            await flush();
            check('a finished export whose download is not a web address is drawn with no link: ' + JSON.stringify(address),
               content().querySelectorAll('a[download]').length === 0 &&
               content().textContent.indexOf('9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08') >= 0,
               JSON.stringify(content().querySelectorAll('a[download]').map((a) => a.attributes)));
         }
         finished.download = downloadWas;
      }

      // ---- the metric history
      before = requests.length;
      await goTo('metrics');
      check('the metrics view reads the metric it opens on, over a day',
         called(before, 'GET', /^\/api\/v1\/metrics\/history\?metric=processed_messages_total&range=24h$/).length === 1, paths(before));
      check('a counter is drawn as what changed between samples, not as the running total',
         content().textContent.indexOf('Total over the window') >= 0 && rows()[0].textContent.indexOf('18') >= 0 &&
         rows()[0].textContent.indexOf('7') >= 0 && rows()[0].textContent.indexOf('3') >= 0,
         rows().map((r) => r.textContent).join(' | '));
      check('and the chart is drawn as a line with a point per sample',
         content().querySelectorAll('polyline').length === 1 && content().querySelectorAll('circle').length === 3,
         String(content().querySelectorAll('circle').length));

      before = requests.length;
      setValue('metName', 'sessions_smtp');
      setValue('metRange', '7d');
      click(act('metload'));
      await flush();
      check('choosing another metric and window reads that one',
         called(before, 'GET', /^\/api\/v1\/metrics\/history\?metric=sessions_smtp&range=7d$/).length === 1, paths(before));
      check('a gauge is drawn as it stood, with the last value rather than a total',
         content().textContent.indexOf('Last value') >= 0 && rows()[0].textContent.indexOf('6') >= 0 &&
         rows()[0].textContent.indexOf('9') >= 0 && rows()[0].textContent.indexOf('10 min') >= 0,
         rows().map((r) => r.textContent).join(' | '));

      state.metricsEnabled = false;
      await goTo('metrics');
      check('with the history switched off the page names the setting that turns it on',
         content().textContent.indexOf('MetricsHistoryDays') >= 0, content().textContent.slice(0, 400));
      state.metricsEnabled = true;

      // ---- the update
      before = requests.length;
      await goTo('update');
      check('the update view reads the verdict once', called(before, 'GET', '/api/v1/update').length === 1, paths(before));
      check('a server that has not checked since it started says exactly that, and offers the check',
         content().textContent.indexOf('Not checked since the service started') >= 0 &&
         content().querySelectorAll('button[data-act="upcheck"]').length === 1 &&
         content().querySelectorAll('button[data-act="updownload"]').length === 0 &&
         content().querySelectorAll('button[data-act="upinstall"]').length === 0,
         content().textContent.slice(0, 200));

      before = requests.length;
      click(act('upcheck'));
      await flush();
      check('Check now reads the feed and the page shows what it found',
         called(before, 'POST', '/api/v1/update/check').length === 1 &&
         content().textContent.indexOf('A newer release is available') >= 0 &&
         content().textContent.indexOf('6.3.4') >= 0, paths(before) + ' ' + content().textContent.slice(0, 200));
      check('and only then is there anything to download',
         content().querySelectorAll('button[data-act="updownload"]').length === 1 &&
         content().querySelectorAll('button[data-act="upinstall"]').length === 0);

      before = requests.length;
      click(act('updownload'));
      await flush();
      check('the download verifies the installer and the page names its signer and the hour it was verified',
         called(before, 'POST', '/api/v1/update/download').length === 1 &&
         content().textContent.indexOf('Progressive Robot Ltd') >= 0 &&
         content().textContent.indexOf('2026-09-17 09:05:02') >= 0, paths(before) + ' ' + content().textContent.slice(0, 400));
      check('and only a verified installer may be installed',
         content().querySelectorAll('button[data-act="upinstall"]').length === 1);
      check('the view says the server drains first',
         content().textContent.indexOf('The server drains first') >= 0, content().textContent.slice(0, 400));

      before = requests.length;
      click(act('upinstall'));
      await flush();
      check('installing asks first, saying that the service stops and starts',
         confirmations[confirmations.length - 1].indexOf('stops and starts') >= 0, confirmations[confirmations.length - 1]);
      check('and what the helper was handed is on the page afterwards',
         called(before, 'POST', '/api/v1/update/install').length === 1 &&
         content().textContent.indexOf('handed to the update helper') >= 0, paths(before));

      // ---- maintenance mode, beside the update
      check('the update view reads the drain state', called(before, 'GET', '/api/v1/server/drain').length >= 1, paths(before));
      check('and says the listeners are open, with a Drain button and no Resume',
         content().textContent.indexOf('Not draining: the listeners are open.') >= 0 &&
         content().querySelectorAll('button[data-act="updrain"]').length === 1 &&
         content().querySelectorAll('button[data-act="upresume"]').length === 0, content().textContent.slice(0, 400));
      before = requests.length;
      confirmAnswer = false;
      click(act('updrain'));
      await flush();
      check('draining asks first, naming the four listeners, and sends nothing on no',
         confirmations[confirmations.length - 1].indexOf('SMTP, IMAP, POP3 and ManageSieve') >= 0 &&
         called(before, 'POST', '/api/v1/server/drain').length === 0, confirmations[confirmations.length - 1]);
      confirmAnswer = true;
      before = requests.length;
      click(act('updrain'));
      await flush();
      check('on yes it enters the drain and re-reads the state',
         called(before, 'POST', '/api/v1/server/drain').length === 1 &&
         content().textContent.indexOf('Draining since 2026-09-23 10:15:02: 1 session(s) and 0 delivery(ies) in flight.') >= 0 &&
         content().querySelectorAll('button[data-act="upresume"]').length === 1 &&
         content().querySelectorAll('button[data-act="updrain"]').length === 0, paths(before) + ' ' + content().textContent.slice(0, 400));
      before = requests.length;
      click(act('upresume'));
      await flush();
      check('Resume leaves the drain without asking and the view says the listeners are open again',
         called(before, 'DELETE', '/api/v1/server/drain').length === 1 &&
         content().textContent.indexOf('Not draining: the listeners are open.') >= 0 &&
         content().querySelectorAll('button[data-act="updrain"]').length === 1, paths(before));

      // ---- a server something else updates: a snap, as the server reports one
      const updateWas = JSON.parse(JSON.stringify(state.update));
      Object.assign(state.update, { state: 2, stateName: 'available', availableVersion: '6.3.4',
         installKind: 'snap', liveUpdate: false, updateCommand: 'snap refresh hmailserver',
         updateAdvice: 'This server runs as the snap hmailserver, which snapd updates: snapd refreshes its snaps on its own several times a day, and snap refresh hmailserver does it now. The server downloads and installs no release file itself.',
         installer: { name: '', url: '', size: 0, digest: '', bundleUrl: '' },
         downloaded: { path: '', signer: '', logTime: '', verifiedAt: '' } });
      await goTo('update');
      check('a snap is shown as one, with the command that updates it and the server\'s own sentence',
         content().textContent.indexOf('Installed as') >= 0 && content().textContent.indexOf('snap refresh hmailserver') >= 0 &&
         $('#updateAdvice') !== null && $('#updateAdvice').textContent.indexOf('snapd refreshes its snaps') >= 0,
         content().textContent.slice(0, 600));
      check('and a newer release there offers nothing to download or install, and no drain note',
         content().querySelectorAll('button[data-act="updownload"]').length === 0 &&
         content().querySelectorAll('button[data-act="upinstall"]').length === 0 &&
         content().textContent.indexOf('The server drains first') < 0 &&
         content().querySelectorAll('button[data-act="upcheck"]').length === 1, content().textContent.slice(0, 600));

      // ---- a server from before 6.3.6 sends none of the four keys, and is offered what it always was
      const older = JSON.parse(JSON.stringify(updateWas));
      delete older.installKind; delete older.liveUpdate; delete older.updateCommand; delete older.updateAdvice;
      state.update = Object.assign(older, { state: 2, stateName: 'available', availableVersion: '6.3.4' });
      await goTo('update');
      check('a server that sends no installKind or liveUpdate is offered the download and told of the drain, as before',
         content().querySelectorAll('button[data-act="updownload"]').length === 1 &&
         content().textContent.indexOf('The server drains first') >= 0 &&
         content().textContent.indexOf('Installed as') < 0 && $('#updateAdvice') === null, content().textContent.slice(0, 600));

      // ---- what the server says of how it is updated is text, never markup
      state.update = Object.assign(JSON.parse(JSON.stringify(updateWas)), { state: 2, stateName: 'available', availableVersion: '6.3.4',
         installKind: 'distribution<i id="kindMark">k</i>', liveUpdate: false, updateCommand: '<b id="commandMark">apt</b>',
         updateAdvice: '<img id="adviceMark" src=x onerror=alert(1)>' });
      await goTo('update');
      check('the kind, the command and the sentence are shown as text, not read as HTML',
         $('#kindMark') === null && $('#commandMark') === null && $('#adviceMark') === null &&
         content().textContent.indexOf('<b id="commandMark">apt</b>') >= 0 &&
         $('#updateAdvice') !== null && $('#updateAdvice').textContent === '<img id="adviceMark" src=x onerror=alert(1)>',
         content().textContent.slice(0, 600));

      // ---- the release page the feed names is a link only as a web address
      // The server keeps html_url only as https://, and the page draws the link
      // only for http: or https: as well, so a server from before that check,
      // or anything else the answer holds, draws no link at all.
      const releaseLinks = () => content().querySelectorAll('a').filter((a) => a.textContent === 'Release notes');
      state.update = Object.assign(JSON.parse(JSON.stringify(updateWas)), { state: 2, stateName: 'available', availableVersion: '6.3.4',
         releaseUrl: 'https://example.invalid/releases/6.3.4' });
      await goTo('update');
      check('the release page is a link that opens apart from the Deck, with no opener and no referrer',
         releaseLinks().length === 1 && releaseLinks()[0].attributes.href === 'https://example.invalid/releases/6.3.4' &&
         releaseLinks()[0].attributes.target === '_blank' && releaseLinks()[0].attributes.rel === 'noopener noreferrer',
         JSON.stringify(releaseLinks().map((a) => a.attributes)));
      for (const address of ['javascript:void(0)', ' JavaScript:void(0)', 'java\tscript:void(0)', '\u0001javascript:void(0)',
         'data:text/plain,notes', 'vbscript:notes', 'file:///C:/notes.txt', 'mailto:notes@example.invalid']) {
         state.update.releaseUrl = address;
         await goTo('update');
         check('a release page that is not a web address draws no link, and the rest of the view is drawn: ' + JSON.stringify(address),
            releaseLinks().length === 0 && content().querySelectorAll('a').filter((a) => /script|data:|file:|mailto:/i.test(String(a.attributes.href || ''))).length === 0 &&
            content().textContent.indexOf('Running version') >= 0 && content().textContent.indexOf('6.3.4') >= 0,
            JSON.stringify(content().querySelectorAll('a').map((a) => a.attributes)));
      }
      state.update = updateWas;

      // ---- the DNS records
      const rotationWas = state.rotations['second.example'];
      state.rotations['second.example'] = rotationDoc('second.example', 'sep2026', null);
      const activeDomains = state.domains.filter((d) => d.active !== false).map((d) => d.name);
      before = requests.length;
      await goTo('dns');
      check('the DNS view reads the domains, the SRV records and the TLSA records once each',
         called(before, 'GET', '/api/v1/domains').length === 1 && called(before, 'GET', '/api/v1/srv').length === 1 &&
         called(before, 'GET', '/api/v1/tlsa').length === 1, paths(before));
      check('and one key rotation per active domain, and none for a domain that is switched off',
         called(before, 'GET', /\/dkim\/rotation$/).length === activeDomains.length,
         activeDomains.join(', ') + ' :: ' + paths(before));
      const dns = $('#dnsPre').textContent;
      check('every active domain has an SPF and a DMARC line to start from',
         activeDomains.every((d) => dns.indexOf(d + '. IN TXT "v=spf1 mx -all"') >= 0 && dns.indexOf('_dmarc.' + d + '. IN TXT "v=DMARC1;') >= 0),
         dns.slice(0, 400));
      check('a domain that is switched off is not in the block',
         state.domains.filter((d) => d.active === false).every((d) => dns.indexOf('; ' + d.name) < 0), dns.slice(0, 400));
      check('the key a rotation holds for a domain is published under that domain, as a record to paste',
         dns.indexOf('sep2026._domainkey.second.example. IN TXT "v=DKIM1; k=rsa; p=KEYsep2026"') >= 0, dns.slice(0, 800));
      check('an SRV record is shown under the domain it belongs to and nowhere else',
         state.srv.records.every((r) => (activeDomains.indexOf(r.domain) >= 0) === (dns.indexOf(r.record) >= 0)),
         activeDomains.join(', ') + ' :: ' + dns.slice(0, 500));
      check('and the TLSA records are a block of their own, one to a line',
         dns.indexOf('; TLSA\n' + state.tlsa.records.map((r) => r.record).join('\n')) >= 0, dns.slice(-400));

      click(act('dnscopy'));
      await flush();
      check('Copy all puts the whole block on the clipboard', clipboard === dns);
      state.rotations['second.example'] = rotationWas;

      // ---- a folder's access-control list
      const aclDomain = Object.keys(state.accounts).filter((d) => state.accounts[d].length > 0 && d.indexOf('<') < 0)[0];
      const aclAddress = state.accounts[aclDomain][0].address;
      before = requests.length;
      await goTo('folders');
      check('the folders view reads nothing until it is given an address',
         called(before, 'GET', /\/folders/).length === 0 && content().textContent.indexOf('press Show') >= 0, paths(before));

      before = requests.length;
      setValue('fldAddress', 'nobody@nowhere.invalid');
      click(act('fldshow'));
      await flush();
      check('an address nothing answers for is reported rather than left blank',
         $('#err_folders').textContent.length > 0, $('#err_folders').textContent);

      before = requests.length;
      setValue('fldAddress', aclAddress);
      click(act('fldshow'));
      await flush();
      check('Show reads the account\'s folders and the first folder\'s permissions',
         called(before, 'GET', '/api/v1/accounts/' + encodeURIComponent(aclAddress) + '/folders').length === 1 &&
         called(before, 'GET', '/api/v1/accounts/' + encodeURIComponent(aclAddress) + '/folders/101/permissions').length === 1,
         paths(before));
      check('the whole tree is offered, a subfolder indented under its parent',
         $('#fldPick').querySelectorAll('option').length === 3 &&
         $('#fldPick').querySelectorAll('option')[1].textContent.indexOf('INBOX.Contracts') >= 0,
         $('#fldPick').querySelectorAll('option').map((o) => o.textContent).join(' | '));
      check('a permission names who holds it, the letters SETACL takes and the rights in words',
         rows().length === 1 && rows()[0].textContent.indexOf('bob@example.com') >= 0 &&
         rows()[0].textContent.indexOf('lrs') >= 0 && rows()[0].textContent.indexOf('write_seen') >= 0,
         rows()[0].textContent);

      before = requests.length;
      $('#fldPick').value = '103';
      $('#fldPick').dispatchEvent(makeEvent('change', { target: $('#fldPick') }));
      await flush();
      check('choosing another folder reads that folder\'s permissions',
         called(before, 'GET', '/api/v1/accounts/' + encodeURIComponent(aclAddress) + '/folders/103/permissions').length === 1,
         paths(before));
      check('and a folder nobody else reaches says so',
         content().textContent.indexOf('Nobody else reaches this folder') >= 0, content().textContent.slice(0, 300));

      before = requests.length;
      setValue('permWho', '   ');
      click(act('permsave'));
      await flush();
      check('granting to nobody is refused here and never sent',
         called(before, 'POST', /\/permissions$/).length === 0 && $('#err_folders').textContent.indexOf('account') >= 0,
         $('#err_folders').textContent + ' ' + paths(before));

      before = requests.length;
      setValue('permType', 'user');
      setValue('permWho', 'carla@example.com');
      content().querySelectorAll('input[data-right="lookup"]')[0].checked = true;
      content().querySelectorAll('input[data-right="read"]')[0].checked = true;
      click(act('permsave'));
      await flush();
      posted = lastBody(before, 'POST', '/api/v1/accounts/' + encodeURIComponent(aclAddress) + '/folders/103/permissions');
      check('granting sends the kind, who it is for and every right as true or false',
         posted !== null && posted.type === 'user' && posted.account === 'carla@example.com' &&
         posted.rights.lookup === true && posted.rights.read === true && posted.rights.administer === false,
         JSON.stringify(posted));
      check('and the new permission is in the table', rows().length === 1 && rows()[0].textContent.indexOf('carla@example.com') >= 0,
         rows().map((r) => r.textContent).join(' | '));

      const granted = state.permissions[103][0].id;
      before = requests.length;
      click(act('permedit', { id: granted }));
      await flush();
      check('Edit fills the form with the permission as it stands',
         $('#permWho').value === 'carla@example.com' &&
         content().querySelectorAll('input[data-right="read"]')[0].checked === true &&
         content().textContent.indexOf('Change the permission') >= 0, $('#permWho').value);

      before = requests.length;
      content().querySelectorAll('input[data-right="insert"]')[0].checked = true;
      click(act('permsave'));
      await flush();
      put = lastBody(before, 'PUT', '/api/v1/accounts/' + encodeURIComponent(aclAddress) + '/folders/103/permissions/' + granted);
      check('and saving it is a PUT of the whole permission to its own address',
         put !== null && put.rights.insert === true && put.rights.read === true, JSON.stringify(put));

      before = requests.length;
      click(act('permdel', { id: granted }));
      await flush();
      check('revoking asks first and deletes the permission alone',
         confirmations[confirmations.length - 1].indexOf('Revoke') >= 0 &&
         called(before, 'DELETE', '/api/v1/accounts/' + encodeURIComponent(aclAddress) + '/folders/103/permissions/' + granted).length === 1 &&
         content().textContent.indexOf('Nobody else reaches this folder') >= 0, paths(before));

      // A folder id is a number the server writes, and the picker draws it as
      // one whatever the answer holds: nothing in it becomes markup.
      {
         const sent = state.folders.folders.filter((f) => f.path === 'Sent')[0];
         const idWas = sent.id;
         sent.id = '103" data-extra="1';
         click(act('fldshow'));
         await flush();
         const option = $('#fldPick').querySelectorAll('option').filter((o) => o.textContent.indexOf('Sent') >= 0)[0];
         check('a folder id that is not a number is drawn as 0, and nothing it carries becomes an attribute',
            !!option && option.attributes.value === '0' && !('data-extra' in option.attributes) && $('#fldPick').querySelectorAll('option').length === 3,
            JSON.stringify(option && option.attributes));
         sent.id = idWas;
      }

      // ---- the diagnostic run
      before = requests.length;
      await goTo('diagnostics');
      check('the diagnostics view runs nothing until it is asked to, and says why it waits',
         called(before, 'POST', '/api/v1/diagnostics/run').length === 0 &&
         content().textContent.indexOf('waits for its timeout') >= 0, paths(before));
      check('the local domain is filled in from the domains this server has',
         $('#diagLocal').value === state.domains[0].name && $('#diagTest').value === 'example.com',
         $('#diagLocal').value + ' / ' + $('#diagTest').value);

      before = requests.length;
      setValue('diagTest', 'elsewhere.example');
      click(act('diagrun'));
      await flush();
      posted = lastBody(before, 'POST', '/api/v1/diagnostics/run');
      check('running sends both domains to the route that runs the tests',
         posted !== null && posted.local_domain === state.domains[0].name && posted.test_domain === 'elsewhere.example',
         JSON.stringify(posted));
      check('a row per test, each said to have passed or failed, with what it found',
         rows().length === 3 && rows()[0].textContent.indexOf('OK') >= 0 &&
         rows()[1].textContent.indexOf('Failed') >= 0 && rows()[1].textContent.indexOf('timed out') >= 0,
         rows().map((r) => r.textContent).join(' | '));
   }

   // ---- the outbound reputation
   before = requests.length;
   await goTo('reputation');
   check('the reputation view reads the report', called(before, 'GET', '/api/v1/reputation').length === 1, paths(before));
   check('one row per address and list, and one more for the address that is not looked up', rows().length === 5, String(rows().length));
   check('a listing is drawn as bad, with the answer the list gave and when it started',
      rows()[0].textContent.indexOf('192.0.2.25') >= 0 && rows()[0].textContent.indexOf('Listed') >= 0 &&
      rows()[0].textContent.indexOf('127.0.0.2') >= 0 && rows()[0].textContent.indexOf('listed since') >= 0 &&
      content().querySelectorAll('.pill[data-level="bad"]').length === 1, rows()[0].textContent);
   check('a list that refused the query is neither listed nor clean',
      rows()[1].textContent.indexOf('Refused') >= 0 && rows()[1].textContent.indexOf('127.255.255.254') >= 0 &&
      rows()[1].textContent.indexOf('Not listed') < 0 && content().querySelectorAll('.pill[data-level="warn"]').length === 1,
      rows()[1].textContent);
   check('a list that says nothing is on it is drawn as good', rows()[2].textContent.indexOf('Not listed') >= 0 &&
      content().querySelectorAll('.pill[data-level="good"]').length === 1, rows()[2].textContent);
   check('a list that does not cover the address family says so rather than reading as clean',
      rows()[3].textContent.indexOf('Not covered') >= 0 && rows()[3].textContent.indexOf('IPv4') >= 0, rows()[3].textContent);
   check('an address that is deliberately not looked up is named, with the reason',
      content().textContent.indexOf('10.0.0.5') >= 0 && content().textContent.indexOf('A private address') >= 0 &&
      content().textContent.indexOf('Not looked up') >= 0);
   check('and when the last check was, and when the next one is due', content().textContent.indexOf('Last checked') >= 0,
      content().textContent.slice(0, 200));

   before = requests.length;
   click(act('repwatchcheck'));
   await flush();
   check('Check now asks the server for a check and reads the report back',
      called(before, 'POST', '/api/v1/reputation/check').length === 1 && called(before, 'GET', '/api/v1/reputation').length === 1,
      paths(before));
   check('and the answer that was refused reads as not listed once the list answers',
      rows()[1].textContent.indexOf('Not listed') >= 0 && content().querySelectorAll('.pill[data-level="warn"]').length === 0,
      rows()[1].textContent);
   check('the toast says what was checked', toastText().indexOf('2') >= 0, toastText());

   nextRefusal = 'the reputation watch could not be run';
   click(act('repwatchcheck'));
   await flush();
   check('a refused check is shown in the server\'s own words beside the button',
      document.getElementById('err_repwatch').textContent.indexOf('could not be run') >= 0,
      document.getElementById('err_repwatch').textContent);
   nextRefusal = null;

   state.reputation.enabled = false;
   state.reputation.addresses = [];
   state.reputation.skipped = [];
   await goTo('reputation');
   check('with the watch off and nothing to check, the view says both rather than drawing an empty table',
      content().textContent.indexOf('The watch is off') >= 0 && content().textContent.indexOf('No outbound address is configured') >= 0,
      content().textContent.slice(0, 300));
   state.reputation.enabled = true;

   // ---- the first-hour checklist
   before = requests.length;
   await goTo('checklist');
   check('the checklist view reads the list', called(before, 'GET', '/api/v1/setup/checklist').length === 1, paths(before));
   check('one card per item, eleven of them', content().querySelectorAll('.checkitem').length === 11,
      String(content().querySelectorAll('.checkitem').length));
   check('the first item is the profile, drawn from this page\'s own words for its code',
      content().querySelectorAll('.checkitem')[0].attributes['data-key'] === 'profile' &&
      content().querySelector('.checkitem[data-key="profile"]').textContent.indexOf('A profile for this server') >= 0 &&
      content().querySelector('.checkitem[data-key="profile"]').textContent.indexOf('No profile has been applied.') >= 0 &&
      content().querySelector('.checkitem[data-key="profile"]').textContent.indexOf('the server sentence the page does not use') < 0,
      content().querySelector('.checkitem[data-key="profile"]') ? content().querySelector('.checkitem[data-key="profile"]').textContent : 'no item');
   check('the view reads the profiles beside the list, and draws the four offered with their titles and the count of changes',
      called(before, 'GET', '/api/v1/settings/profiles').length === 1 && $('#profilesCard') !== null &&
      $('#profilesCard').textContent.indexOf('A small office server') >= 0 &&
      $('#profilesCard').textContent.indexOf('Behind a filtering gateway') >= 0 &&
      $('#profilesCard').textContent.indexOf('1 of its 3 settings would change on this server.') >= 0,
      $('#profilesCard') ? $('#profilesCard').textContent : paths(before));
   check('a done item is drawn as good, with its title and the finding rendered from the code and its arguments',
      content().querySelector('.checkitem[data-key="domain"] .pill[data-level="good"]') !== null &&
      content().querySelector('.checkitem[data-key="domain"]').textContent.indexOf('A domain') >= 0 &&
      content().querySelector('.checkitem[data-key="domain"]').textContent.indexOf('Active domains: 1 of 1.') >= 0,
      content().querySelector('.checkitem[data-key="domain"]').textContent);
   check('an item that needs action is drawn as a warning and names the port',
      content().querySelector('.checkitem[data-key="submission"] .pill[data-level="warn"]') !== null &&
      content().querySelector('.checkitem[data-key="submission"]').textContent.indexOf('Port 587 takes submission without TLS') >= 0 &&
      content().querySelector('.checkitem[data-key="submission"]').textContent.indexOf('Port 465 offers TLS with the certificate Example.') >= 0,
      content().querySelector('.checkitem[data-key="submission"]').textContent);
   check('a port 25 that still takes AUTH is drawn as a warning in this page\'s words, with the Ports view and no fix to send',
      content().querySelector('.checkitem[data-key="delivery_port_auth"] .pill[data-level="warn"]') !== null &&
      content().querySelector('.checkitem[data-key="delivery_port_auth"]').textContent.indexOf('No password on port 25') >= 0 &&
      content().querySelector('.checkitem[data-key="delivery_port_auth"]').textContent.indexOf('Port 25 still accepts AUTH') >= 0 &&
      content().querySelector('.checkitem[data-key="delivery_port_auth"]').textContent.indexOf('the server sentence the page does not use') < 0 &&
      content().querySelector('.checkitem[data-key="delivery_port_auth"] [data-act="checklistgo"][data-to="ports"]') !== null &&
      content().querySelector('.checkitem[data-key="delivery_port_auth"] [data-act="checklistfix"]') === null,
      content().querySelector('.checkitem[data-key="delivery_port_auth"]') ? content().querySelector('.checkitem[data-key="delivery_port_auth"]').innerHTML : 'no item');
   check('a lookup that failed reads as cannot tell, not as missing',
      content().querySelector('.checkitem[data-key="dkim"]').textContent.indexOf('Cannot tell') >= 0 &&
      content().querySelector('.checkitem[data-key="dkim"]').textContent.indexOf('says nothing about the DKIM record') >= 0 &&
      content().querySelector('.checkitem[data-key="dkim"]').textContent.indexOf('no DKIM record') < 0,
      content().querySelector('.checkitem[data-key="dkim"]').textContent);
   check('a code this page does not know is shown in the server\'s own words',
      content().querySelector('.checkitem[data-key="second_factor"]').textContent.indexOf('The server said this in its own words.') >= 0,
      content().querySelector('.checkitem[data-key="second_factor"]').textContent);
   check('the summary counts the four states', document.getElementById('checklistSummary').textContent === '3 done, 6 need action, 1 cannot be told, 0 not needed.',
      document.getElementById('checklistSummary').textContent);

   // ---- passwords only over TLS, and the one-click fix its finding carries
   check('the passwords item is drawn from this page\'s own words, names the range and its ports, and offers its fix as a button on that finding alone',
      content().querySelector('.checkitem[data-key="sign_in_tls"]') !== null &&
      content().querySelector('.checkitem[data-key="sign_in_tls"]').textContent.indexOf('Passwords only over TLS') >= 0 &&
      content().querySelector('.checkitem[data-key="sign_in_tls"]').textContent.indexOf('The IP range Internet takes a password without TLS on port 25, 110, 143, 587') >= 0 &&
      content().querySelector('.checkitem[data-key="sign_in_tls"]').textContent.indexOf('The IP range Branch office takes a password only over TLS.') >= 0 &&
      content().querySelector('.checkitem[data-key="sign_in_tls"]').textContent.indexOf('the server sentence the page does not use') < 0 &&
      content().querySelectorAll('button[data-act="checklistfix"]').length === 1 &&
      content().querySelector('button[data-act="checklistfix"]').textContent === 'Require TLS for sign-in from this range',
      content().querySelector('.checkitem[data-key="sign_in_tls"]') ? content().querySelector('.checkitem[data-key="sign_in_tls"]').textContent : 'no item');

   confirmAnswer = false;
   before = requests.length;
   click(act('checklistfix'));
   await flush();
   check('the fix asks first, naming the range, and a no sends nothing',
      called(before, 'PUT', /ipranges/).length === 0 &&
      confirmations[confirmations.length - 1].indexOf('Require TLS for sign-in from the IP range Internet?') >= 0,
      confirmations[confirmations.length - 1]);
   confirmAnswer = true;

   const rangeBeforeFix = Object.assign({}, state.ranges[1]);
   before = requests.length;
   click(act('checklistfix'));
   await flush();
   const fixBody = lastBody(before, 'PUT', '/api/v1/ipranges/2');
   check('a yes sends exactly the request the finding carries, and the list is read again',
      called(before, 'PUT', '/api/v1/ipranges/2').length === 1 &&
      fixBody !== null && Object.keys(fixBody).length === 1 && fixBody.require_tls_for_auth === true &&
      state.ranges[1].require_tls_for_auth === true &&
      called(before, 'GET', '/api/v1/setup/checklist').length === 1,
      paths(before) + ' ' + JSON.stringify(fixBody));
   state.ranges[1] = rangeBeforeFix;

   check('a range whose listeners have no TLS to start says to give them STARTTLS first, and offers the Ports view rather than a fix',
      content().querySelector('.checkitem[data-key="sign_in_tls"]').textContent.indexOf('The IP range Lab takes a password without TLS, and port 8025 has no TLS a client could start') >= 0 &&
      content().querySelectorAll('button[data-act="checklistfix"]').length === 1 &&
      content().querySelectorAll('.checkitem[data-key="sign_in_tls"] button[data-act="checklistgo"]').length === 1 &&
      content().querySelector('.checkitem[data-key="sign_in_tls"] button[data-act="checklistgo"]').textContent === 'Ports',
      content().querySelector('.checkitem[data-key="sign_in_tls"]').textContent);
   click(act('checklistgo'));
   await flush();
   check('the Ports button opens the Ports view', $('#viewTitle').textContent === 'Ports', $('#viewTitle').textContent);
   await goTo('checklist');

   // ---- a setting profile: the diff, the confirmation, the apply and the way back
   before = requests.length;
   click(act('profileshow', { name: 'office' }));
   await flush();
   check('Show the changes reads that profile\'s diff and draws every entry beside the value this server holds',
      called(before, 'GET', '/api/v1/settings/profiles/office').length === 1 &&
      content().querySelectorAll('tr.profilerow').length === 3 &&
      content().querySelector('tr.profilerow[data-name="usespf"]').attributes['data-changes'] === '1' &&
      content().querySelector('tr.profilerow[data-name="maxmessagesize"]').attributes['data-changes'] === '0',
      paths(before));
   check('an entry the environment sets says so and names the variable, and the steps are listed',
      content().querySelector('tr.profilerow[data-name="QuarantineEnabled"]').textContent.indexOf('Set by the environment (HM_SETTING_QuarantineEnabled)') >= 0 &&
      $('#profilesCard').textContent.indexOf('What it cannot do for you') >= 0 &&
      $('#profilesCard').textContent.indexOf('Give the submission port STARTTLS with a certificate.') >= 0,
      $('#profilesCard').textContent);
   check('a profile that asks for no confirmation draws no box', document.getElementById('profileConfirm') === null, 'a box was drawn');

   before = requests.length;
   click(act('profileshow', { name: 'gateway' }));
   await flush();
   check('an entry that turns a filter off is marked, and a confirmation box is drawn',
      called(before, 'GET', '/api/v1/settings/profiles/gateway').length === 1 &&
      content().querySelector('tr.profilerow[data-name="usegreylisting"]').textContent.indexOf('Turns a filter or a check off') >= 0 &&
      content().querySelector('tr.profilerow[data-name="SMTPXClientEnabled"]').textContent.indexOf('Trusts an address range') < 0 &&
      document.getElementById('profileConfirm') !== null,
      $('#profilesCard').textContent);
   check('a window of days the profile would shorten is marked, with the server\'s note that a revert does not bring back what is deleted, and the box says so',
      content().querySelector('tr.profilerow[data-name="MessageTraceRetentionDays"]').textContent.indexOf('Shortens how long data is kept') >= 0 &&
      content().querySelector('tr.profilerow[data-name="MessageTraceRetentionDays"]').textContent.indexOf('a revert puts the number back, not what was deleted') >= 0 &&
      $('#profilesCard').textContent.indexOf('what a shorter window deletes, a revert does not bring back') >= 0,
      content().querySelector('tr.profilerow[data-name="MessageTraceRetentionDays"]').textContent);
   check('the apply button carries the diff\'s token',
      content().querySelector('button[data-act="profileapply"]').attributes['data-token'] === 'tok-gateway',
      JSON.stringify(content().querySelector('button[data-act="profileapply"]').attributes));

   before = requests.length;
   click(act('profileapply', { name: 'gateway' }));
   await flush();
   check('Apply without the box ticked sends nothing and says why',
      called(before, 'POST', '/api/v1/settings/profiles/gateway/apply').length === 0 &&
      document.getElementById('err_profiles').textContent.indexOf('Tick the confirmation first') >= 0,
      document.getElementById('err_profiles').textContent + ' | ' + paths(before));

   setChecked('profileConfirm', true);
   confirmAnswer = false;
   before = requests.length;
   click(act('profileapply', { name: 'gateway' }));
   await flush();
   check('Apply asks first, naming how many settings change, and no sends nothing',
      called(before, 'POST', '/api/v1/settings/profiles/gateway/apply').length === 0 &&
      confirmations[confirmations.length - 1].indexOf('Apply the gateway profile? 2 settings change, and what they held is kept') >= 0,
      confirmations[confirmations.length - 1]);
   confirmAnswer = true;

   // The settings moved between the diff and the apply: the server refuses the
   // stale token, the page says why and draws the diff again with the new one.
   state.profileDiffs.gateway.diff_token = 'tok-gateway-2';
   setChecked('profileConfirm', true);
   before = requests.length;
   click(act('profileapply', { name: 'gateway' }));
   await flush();
   check('an apply from a diff the server no longer answers is refused, the page says so and draws the diff again',
      called(before, 'POST', '/api/v1/settings/profiles/gateway/apply').length === 1 &&
      state.profileApplies[state.profileApplies.length - 1].body.diff_token === 'tok-gateway' &&
      state.profileSnapshot === null &&
      document.getElementById('err_profiles').textContent.indexOf('The settings changed since the diff') >= 0 &&
      called(before, 'GET', '/api/v1/settings/profiles/gateway').length >= 1 &&
      content().querySelector('button[data-act="profileapply"]').attributes['data-token'] === 'tok-gateway-2',
      document.getElementById('err_profiles').textContent + ' | ' + paths(before));

   setChecked('profileConfirm', true);
   before = requests.length;
   click(act('profileapply', { name: 'gateway' }));
   await flush();
   const applied = state.profileApplies[state.profileApplies.length - 1];
   check('yes applies it with the confirmation and the diff\'s token on the body, says what it did and what waits, and offers the way back',
      called(before, 'POST', '/api/v1/settings/profiles/gateway/apply').length === 1 &&
      applied && applied.body && applied.body.confirm === true && applied.body.diff_token === 'tok-gateway-2' &&
      toastText() === 'gateway: 2 set, 1 unchanged, 0 skipped; 1 wait for a reinitialise and 0 for a restart.' &&
      content().querySelector('button[data-act="profilerevert"]') !== null &&
      $('#profilesCard').textContent.indexOf('The gateway profile is applied. What it replaced is kept, and can be put back.') >= 0,
      toastText() + ' | ' + paths(before));
   check('with a way back held, the diff offers no apply and says why',
      content().querySelector('button[data-act="profileapply"]') === null &&
      $('#profilesCard').textContent.indexOf('A way back from an earlier profile is held') >= 0,
      $('#profilesCard').textContent);

   confirmAnswer = false;
   before = requests.length;
   click(act('profilerevert'));
   await flush();
   check('Put back asks first, saying a filter may be turned off again, and no sends nothing',
      called(before, 'POST', '/api/v1/settings/profiles/revert').length === 0 &&
      confirmations[confirmations.length - 1].indexOf('a filter the profile turned on may be turned off again') >= 0,
      confirmations[confirmations.length - 1]);
   confirmAnswer = true;
   before = requests.length;
   click(act('profilerevert'));
   await flush();
   check('yes puts it back, says how many, and the way back is gone',
      called(before, 'POST', '/api/v1/settings/profiles/revert').length === 1 && state.profileReverts === 1 &&
      toastText() === 'gateway: 2 put back, 1 left as they were; 0 wait for a reinitialise and 2 for a restart.' &&
      content().querySelector('button[data-act="profilerevert"]') === null,
      toastText() + ' | ' + paths(before));

   // Discarding the way back keeps the settings and asks nothing: nothing changes.
   state.profileSnapshot = { readable: true, profile: 'office', applied: 1789000050, entries: 2 };
   click(act('checklistagain'));
   await flush();
   before = requests.length;
   click(act('profilediscard'));
   await flush();
   check('Keep its settings forgets the way back and redraws',
      called(before, 'POST', '/api/v1/settings/profiles/discard').length === 1 && state.profileDiscards === 1 &&
      content().querySelector('button[data-act="profilediscard"]') === null,
      paths(before));

   // A way back that cannot be read can only be discarded, and the page offers that.
   state.profileSnapshot = { readable: false };
   click(act('checklistagain'));
   await flush();
   check('a way back that cannot be read is said to be, with a Discard and no Put back',
      $('#profilesCard').textContent.indexOf('is held and cannot be read') >= 0 &&
      content().querySelector('button[data-act="profilediscard"]') !== null &&
      content().querySelector('button[data-act="profilerevert"]') === null &&
      content().querySelector('button[data-act="profiledecline"]') === null,
      $('#profilesCard').textContent);
   before = requests.length;
   click(act('profilediscard'));
   await flush();
   check('Discard the way back sends the discard, and the page offers the profiles again',
      called(before, 'POST', '/api/v1/settings/profiles/discard').length === 1 && state.profileDiscards === 2 &&
      content().querySelector('button[data-act="profilediscard"]') === null,
      paths(before));

   // Keeping the shipped settings: asked first, recorded, and no longer offered.
   confirmAnswer = false;
   before = requests.length;
   click(act('profiledecline'));
   await flush();
   check('Keep the shipped settings asks first, and no sends nothing',
      called(before, 'POST', '/api/v1/settings/profiles/decline').length === 0 &&
      confirmations[confirmations.length - 1].indexOf('Keep the settings this version ships with, and apply no profile?') >= 0,
      confirmations[confirmations.length - 1]);
   confirmAnswer = true;
   before = requests.length;
   click(act('profiledecline'));
   await flush();
   check('yes records the choice, says so, and the button is gone',
      called(before, 'POST', '/api/v1/settings/profiles/decline').length === 1 && state.profileDeclines === 1 &&
      toastText() === 'The shipped settings are kept; no profile is applied.' &&
      content().querySelector('button[data-act="profiledecline"]') === null,
      toastText() + ' | ' + paths(before));
   state.profileDeclined = false;

   // The two codes a profile's record adds, in this page's own words.
   const profileItem = state.checklist.items[0];
   const profileItemBefore = clone(profileItem);
   profileItem.state = 'action_needed';
   profileItem.findings = [{ state: 'action_needed', code: 'profile_failed', args: ['office', '2026-09-23'], text: 'the server sentence the page does not use' }];
   click(act('checklistagain'));
   await flush();
   check('a profile that could not be applied in full is drawn from this page\'s own words',
      content().querySelector('.checkitem[data-key="profile"]').textContent.indexOf('The office profile could not be applied in full on 2026-09-23') >= 0 &&
      content().querySelector('.checkitem[data-key="profile"]').textContent.indexOf('the server sentence the page does not use') < 0,
      content().querySelector('.checkitem[data-key="profile"]').textContent);
   profileItem.state = 'not_needed';
   profileItem.findings = [{ state: 'not_needed', code: 'profile_declined', args: ['2026-09-23'], text: 'the server sentence the page does not use' }];
   click(act('checklistagain'));
   await flush();
   check('the shipped settings chosen are drawn from this page\'s own words',
      content().querySelector('.checkitem[data-key="profile"]').textContent.indexOf('The settings this version ships with were chosen on 2026-09-23') >= 0,
      content().querySelector('.checkitem[data-key="profile"]').textContent);
   state.checklist.items[0] = profileItemBefore;

   state.checklist = {
      generated: 1789000200, dns_looked_up: true, done: 0, action_needed: 5, cannot_tell: 0, not_needed: 3,
      items: [
         { key: 'domain', state: 'action_needed', findings: [
            { state: 'action_needed', code: 'domain_none', args: [], text: 'No domain has been added yet.' },
            // The sample's own finding, with a text this page must NOT use: the
            // code is in CHECKLIST_TEXT, so the page has words of its own for it
            // in every language, and a code that had been left out would show
            // this English sentence instead.
            { state: 'not_needed', code: 'sample_domain', args: ['example.test'], text: 'the sentence the server sent, which this page has no need of' }] },
         { key: 'account', state: 'action_needed', findings: [{ state: 'action_needed', code: 'account_none', args: [], text: 'No account has been added in any domain.' }] },
         { key: 'submission', state: 'action_needed', findings: [{ state: 'action_needed', code: 'submission_plain', args: ['587'], text: 'Port 587 takes submission without TLS, so passwords and mail would cross the network in the clear.' }] },
         { key: 'spf', state: 'not_needed', findings: [{ state: 'not_needed', code: 'no_active_domain', args: [], text: 'No active domain, so there is nothing to publish yet.' }] },
         { key: 'dkim', state: 'not_needed', findings: [{ state: 'not_needed', code: 'no_active_domain', args: [], text: 'No active domain, so there is nothing to publish yet.' }] },
         { key: 'dmarc', state: 'not_needed', findings: [{ state: 'not_needed', code: 'no_active_domain', args: [], text: 'No active domain, so there is nothing to publish yet.' }] },
         { key: 'backup', state: 'action_needed', findings: [{ state: 'action_needed', code: 'backup_none', args: [], text: 'No backup destination is set.' }] },
         { key: 'second_factor', state: 'action_needed', findings: [{ state: 'action_needed', code: 'second_factor_off', args: [], text: 'The administrator signs in with a password alone.' }] }
      ]
   };
   before = requests.length;
   click(act('checklistagain'));
   await flush();
   check('Check again reads the list again', called(before, 'GET', '/api/v1/setup/checklist').length === 1, paths(before));
   check('a fresh server reads as nothing added, with the record items not needed rather than failed',
      content().querySelector('.checkitem[data-key="domain"]').textContent.indexOf('No domain has been added yet.') >= 0 &&
      content().querySelector('.checkitem[data-key="spf"]').textContent.indexOf('Not needed') >= 0 &&
      content().querySelector('.checkitem[data-key="spf"] .pill[data-level]') === null,
      content().querySelector('.checkitem[data-key="spf"]').textContent);
   check('the sample domain finding is drawn from this page\'s own words, not the server\'s',
      content().querySelector('.checkitem[data-key="domain"]').textContent
         .indexOf('example.test is the sample domain and is not counted here; remove it when you are done with it.') >= 0 &&
      content().querySelector('.checkitem[data-key="domain"]').textContent.indexOf('the sentence the server sent') < 0,
      content().querySelector('.checkitem[data-key="domain"]').textContent);

   // ---- the sample domain, on the same view
   check('the card offers to make one when there is none, and says what it costs',
      $('#sampleCard') !== null && $('#sampleCard').textContent.indexOf('A sample domain to learn on') >= 0 &&
      $('#sampleCard').textContent.indexOf('This makes example.test with three mailboxes') >= 0 &&
      $('#sampleCard').textContent.indexOf('mints now and shows once') >= 0 &&
      content().querySelector('button[data-act="samplecreate"]') !== null &&
      content().querySelector('button[data-act="sampleremove"]') === null,
      $('#sampleCard') ? $('#sampleCard').textContent : 'no card');

   // A sample this tab did not make: the password is not in the state document
   // and this page never had it, so the card says where it went rather than
   // leaving a blank where a secret used to be.
   state.sampleDomain = Object.assign({}, state.sampleDomain, { state: 'present', domain_id: 42, exists: true });
   click(act('checklistagain'));
   await flush();
   check('a sample this browser did not make says the password was shown once and not kept',
      $('#sampleCard').textContent.indexOf('was shown when the sample was made and was not kept') >= 0 &&
      content().querySelector('button[data-act="sampleremove"]') !== null,
      $('#sampleCard').textContent);

   // A server that could not read its domain table: neither button, because it
   // has just said it does not know whether the sample is there.
   state.sampleDomain = Object.assign({}, state.sampleDomain, { state: 'unknown', domain_id: 0, exists: false });
   click(act('checklistagain'));
   await flush();
   check('a domain table that could not be read offers neither button and says so',
      $('#sampleCard').textContent.indexOf('could not read its domain table') >= 0 &&
      content().querySelector('button[data-act="samplecreate"]') === null &&
      content().querySelector('button[data-act="sampleremove"]') === null,
      $('#sampleCard').textContent);
   state.sampleDomain = Object.assign({}, state.sampleDomain, { state: 'absent', domain_id: 0, exists: false });
   click(act('checklistagain'));
   await flush();

   before = requests.length;
   click(act('samplecreate'));
   await flush();
   check('Make a sample domain posts once and reads the view again',
      called(before, 'POST', '/api/v1/setup/sample-domain').length === 1 &&
      called(before, 'GET', '/api/v1/setup/sample-domain').length === 1, paths(before));
   check('the toast counts the mailboxes and the messages on their way',
      toastText() === 'example.test made: 3 mailboxes, and 8 messages on their way', toastText());
   check('with one there, the card names the accounts and the list, and offers to remove it',
      $('#sampleCard').textContent.indexOf('alice@example.test, bob@example.test, carol@example.test') >= 0 &&
      $('#sampleCard').textContent.indexOf('everyone@example.test') >= 0 &&
      $('#sampleCard').textContent.indexOf('not this server\'s first domain') >= 0 &&
      content().querySelector('button[data-act="sampleremove"]') !== null &&
      content().querySelector('button[data-act="samplecreate"]') === null,
      $('#sampleCard').textContent);
   // The password is shown by the card and not by the toast, because a toast goes
   // away while somebody is still writing it down - and it is the creation's
   // answer that carries it, because no read of the sample can.
   check('the password the creation minted is on the card, said to be shown once',
      $('#sampleCard').textContent.indexOf('7TQ4M-KJ9XD-2VWHF-BNRE5') >= 0 &&
      $('#sampleCard').textContent.indexOf('this server did not keep it') >= 0 &&
      toastText().indexOf('7TQ4M') < 0,
      $('#sampleCard').textContent + ' | ' + toastText());

   // Somebody else removed that sample and made another while this tab watched.
   // The password this tab holds belongs to the one that has gone, and printing
   // it beside the new sample's mailboxes would be worse than printing nothing.
   state.sampleDomain = Object.assign({}, state.sampleDomain, { domain_id: 43 });
   click(act('checklistagain'));
   await flush();
   check('a password minted for a sample that has gone is not shown beside another one',
      $('#sampleCard').textContent.indexOf('7TQ4M-KJ9XD-2VWHF-BNRE5') < 0 &&
      $('#sampleCard').textContent.indexOf('was shown when the sample was made and was not kept') >= 0,
      $('#sampleCard').textContent);
   state.sampleDomain = Object.assign({}, state.sampleDomain, { domain_id: 42 });
   click(act('checklistagain'));
   await flush();

   confirmAnswer = false;
   before = requests.length;
   click(act('sampleremove'));
   await flush();
   check('removing asks first, and no means nothing is sent',
      called(before, 'DELETE', '/api/v1/setup/sample-domain').length === 0 &&
      confirmations[confirmations.length - 1].indexOf('Anything added to it goes too') >= 0,
      confirmations[confirmations.length - 1]);
   confirmAnswer = true;

   before = requests.length;
   click(act('sampleremove'));
   await flush();
   check('yes removes it and the toast says what went',
      called(before, 'DELETE', '/api/v1/setup/sample-domain').length === 1 &&
      toastText() === 'example.test removed: 3 mailboxes and 14 messages' &&
      content().querySelector('button[data-act="samplecreate"]') !== null, toastText() + ' | ' + paths(before));

   // An example.test this server did not make: neither button, and the card says
   // why - the one refusal the whole feature exists to make.
   state.sampleDomain = Object.assign({}, state.sampleDomain, { state: 'foreign', domain_id: 0, exists: true });
   click(act('checklistagain'));
   await flush();
   check('a domain of that name this server did not make is refused in both directions',
      $('#sampleCard').textContent.indexOf('the sample did not make it') >= 0 &&
      content().querySelector('button[data-act="samplecreate"]') === null &&
      content().querySelector('button[data-act="sampleremove"]') === null,
      $('#sampleCard').textContent);
   state.sampleDomain = Object.assign({}, state.sampleDomain, { state: 'absent', exists: false });

   // ---- groups: the last thing the census counted as writable over REST and
   // reachable from no view here
   before = requests.length;
   await goTo('groups');
   check('the groups view reads the groups', called(before, 'GET', '/api/v1/groups').length === 1 && $('#viewTitle').textContent === 'Groups', paths(before));
   check('one row per group, the id under the name', rows().length === 2 && rows()[0].textContent.indexOf('Support') >= 0 && rows()[0].textContent.indexOf('id 3') >= 0,
      rows().map((r) => r.textContent).join(' | '));
   check('what a delete takes with it is said as a notice, in the route\'s own words',
      content().querySelector('.notice[data-level="info"]') !== null && content().querySelector('.notice .mark').textContent === '\u25c6' &&
      content().querySelector('.notice').textContent.indexOf('Information') >= 0 &&
      content().textContent.indexOf('every folder permission that named it go together') >= 0,
      content().querySelector('.notice') ? content().querySelector('.notice').textContent : 'no notice');

   before = requests.length;
   setValue('newGroup', '');
   click(act('groupnew'));
   await flush();
   check('an empty name is refused by the page, not the server',
      called(before, 'POST', '/api/v1/groups').length === 0 && document.getElementById('err_groupnew').textContent.indexOf('required') >= 0,
      document.getElementById('err_groupnew').textContent);

   before = requests.length;
   setValue('newGroup', 'Wardens');
   click(act('groupnew'));
   await flush();
   posted = lastBody(before, 'POST', '/api/v1/groups');
   check('creating a group posts the one key the route takes', JSON.stringify(posted) === '{"name":"Wardens"}', JSON.stringify(posted));
   check('and goes straight into its editor, on the record the server holds, with its members read',
      called(before, 'GET', '/api/v1/groups').length === 1 && called(before, 'GET', /\/api\/v1\/groups\/\d+\/members$/).length === 1 &&
      content().querySelector('h2').textContent.indexOf('Wardens') >= 0 && document.getElementById('group_name').value === 'Wardens', paths(before));
   check('the name is a form row built from the create\'s own schema, marked required',
      document.getElementById('group_name').closest('.fr').textContent.indexOf('required') >= 0 &&
      document.getElementById('group_name').closest('.fr').querySelector('.hint').textContent.indexOf('folder permission') >= 0);
   check('a group with nobody in it says so', content().textContent.indexOf('No accounts in this group.') >= 0);
   check('and the toast names it', toastText() === 'Group created: Wardens', toastText());

   before = requests.length;
   setValue('group_name', 'Support');
   click(act('groupsave'));
   await flush();
   check('a name another group already has is the server\'s sentence, with the editor kept open',
      called(before, 'PUT', /\/api\/v1\/groups\/\d+$/).length === 1 &&
      document.getElementById('err_groupedit').textContent === 'A group with this name already exists.' && document.getElementById('group_name') !== null,
      document.getElementById('err_groupedit').textContent);

   before = requests.length;
   setValue('group_name', 'Wardens of the second floor');
   click(act('groupsave'));
   await flush();
   put = lastBody(before, 'PUT', /\/api\/v1\/groups\/\d+$/);
   check('renaming PUTs the name by id, reads it back and stays in the editor',
      JSON.stringify(put) === '{"name":"Wardens of the second floor"}' && called(before, 'GET', '/api/v1/groups').length === 1 &&
      document.getElementById('group_name').value === 'Wardens of the second floor' && toastText() === 'Group saved', JSON.stringify(put));

   before = requests.length;
   setValue('newMember', 'nobody@example.com');
   click(act('memberadd'));
   await flush();
   check('an address that is nobody\'s is refused by the server, in its words, beside the box',
      called(before, 'POST', /members$/).length === 1 && document.getElementById('err_member').textContent === 'no account nobody@example.com',
      document.getElementById('err_member').textContent);

   before = requests.length;
   setValue('newMember', 'anna@example.com');
   click(act('memberadd'));
   await flush();
   posted = lastBody(before, 'POST', /members$/);
   check('adding an account posts the address, which is what the administrator has', JSON.stringify(posted) === '{"account":"anna@example.com"}', JSON.stringify(posted));
   check('and the membership is read again with it in', content().textContent.indexOf('anna@example.com') >= 0 &&
      toastText() === 'anna@example.com added to the group', toastText());

   const memberBox = setValue('newMember', 'carla@example.com');
   memberBox.dispatchEvent(makeEvent('keydown', { key: 'Enter', target: memberBox }));
   await flush();
   check('Enter in the box adds one as the button does', content().querySelectorAll('.erow').length === 2 &&
      content().textContent.indexOf('carla@example.com') >= 0, content().querySelectorAll('.erow').length + ' members');

   before = requests.length;
   click(act('memberdel', { account: 1 }));
   await flush();
   check('removing one deletes by the account\'s id, which is what a membership is made of',
      called(before, 'DELETE', /\/api\/v1\/groups\/\d+\/members\/1$/).length === 1 && content().textContent.indexOf('anna@example.com') < 0 &&
      toastText() === 'anna@example.com removed from the group', paths(before));

   before = requests.length;
   click(act('groups'));
   await flush();
   check('the way back re-reads the groups with the new one among them', called(before, 'GET', '/api/v1/groups').length === 1 && rows().length === 3);

   confirmAnswer = false;
   before = requests.length;
   click(act('groupdel', { name: 'Wardens of the second floor' }));
   await flush();
   check('deleting asks first, naming the group and what goes with it',
      called(before, 'DELETE', /groups/).length === 0 && confirmations[confirmations.length - 1].indexOf('Wardens of the second floor') >= 0 &&
      confirmations[confirmations.length - 1].indexOf('every folder permission that names it') >= 0, confirmations[confirmations.length - 1]);
   confirmAnswer = true;
   before = requests.length;
   click(act('groupdel', { name: 'Wardens of the second floor' }));
   await flush();
   check('yes deletes it by id and the re-read list is without it',
      called(before, 'DELETE', /\/api\/v1\/groups\/\d+$/).length === 1 && rows().length === 2 && toastText() === 'Group deleted', paths(before));

   // ---- the keyboard: the page with no pointer at all
   check('the shortcut list is shut until it is asked for', $('#keysOverlay').hidden === true);
   press('?');
   check('? opens it, drawn from the page\'s one table and taking the keyboard with it',
      $('#keysOverlay').hidden === false && $('#keysList').querySelectorAll('kbd').length === 7 &&
      $('#keysList').textContent.indexOf('Search this view') >= 0 && $('#keysList').textContent.indexOf('F1') >= 0 &&
      document.activeElement === $('#keysClose'), $('#keysList').textContent);
   press('Tab', $('#keysClose'));
   check('while it is open the keyboard stays on it and cannot act on what it hides',
      $('#keysOverlay').hidden === false && document.activeElement === $('#keysClose'));
   press('?');
   check('a second ? shuts it again, as the list says it does', $('#keysOverlay').hidden === true);
   press('?');
   press('Escape');
   check('Escape shuts it and hands the keyboard back to the button that opened it',
      $('#keysOverlay').hidden === true && document.activeElement === $('#keysBtn'));
   press('/');
   check('/ puts the keyboard in the search box', document.activeElement === $('#deckSearch'));
   setValue('deckSearch', 'everyone');
   $('#deckSearch').dispatchEvent(makeEvent('input', { target: $('#deckSearch') }));
   check('what is typed hides the rows that do not carry it',
      rows().filter((r) => !r.hidden).length === 1 && rows().filter((r) => !r.hidden)[0].textContent.indexOf('Everyone') >= 0,
      rows().map((r) => r.textContent + '=' + r.hidden).join(' | '));
   press('Escape', $('#deckSearch'));
   check('Escape empties the box and gives every row back', $('#deckSearch').value === '' && rows().every((r) => !r.hidden));
   press('ArrowDown');
   check('the first arrow puts the cursor on the first row', rows()[0].classList.contains('cur') && !rows()[1].classList.contains('cur'));
   press('ArrowDown');
   check('and the next moves it down one', rows()[1].classList.contains('cur') && !rows()[0].classList.contains('cur'));
   press('ArrowUp');
   check('and up again', rows()[0].classList.contains('cur'));
   before = requests.length;
   press('Enter');
   await flush();
   check('Enter opens the row the cursor is on, which is what its first button does',
      called(before, 'GET', /\/api\/v1\/groups\/\d+\/members$/).length === 1 && document.getElementById('group_name') !== null, paths(before));
   check('and a view drawn afresh starts with no row under the keyboard', rows().length === 0);
   click(act('groups'));
   await flush();
   $('#nav button[data-view="groups"]').focus();
   press('ArrowDown', $('#nav button[data-view="groups"]'));
   check('the arrows move between the views while the keyboard is in the navigation column',
      document.activeElement === $('#nav button[data-view="ipranges"]'),
      document.activeElement ? JSON.stringify(document.activeElement.attributes) : 'nothing focused');
   setValue('deckSearch', 'everyone');
   $('#deckSearch').dispatchEvent(makeEvent('input', { target: $('#deckSearch') }));
   await goTo('certs');
   check('a view arrived at shows everything it has: the search belongs to the view it was typed in',
      $('#deckSearch').value === '' && rows().every((r) => !r.hidden), rows().length + ' rows');
   before = requests.length;
   const asked = confirmations.length;
   press('ArrowDown');
   press('Enter');
   await flush();
   check('and a row whose only button deletes it is a row Enter leaves alone',
      rows()[0].classList.contains('cur') && called(before, 'DELETE', /certificates/).length === 0 && confirmations.length === asked,
      paths(before));
   // ---- the reports
   before = requests.length;
   await goTo('reports');
   check('the reports view reads the index and then one summary, not a request per section',
      called(before, 'GET', '/api/v1/reports').length === 1 && called(before, 'GET', /^\/api\/v1\/reports\/summary\?/).length === 1 &&
      called(before, 'GET', /^\/api\/v1\/reports\/traffic/).length === 0, paths(before));
   check('the window comes from the index rather than from this browser\'s clock',
      document.getElementById('repFrom').attributes.value === '2026-08-17' && document.getElementById('repTo').attributes.value === '2026-09-15',
      document.getElementById('repFrom').attributes.value + '..' + document.getElementById('repTo').attributes.value);
   check('the sources are drawn with their retention and how far back they go',
      content().textContent.indexOf('2026-09-01 08:00:00') >= 0 && content().textContent.indexOf('Message trace') >= 0, '');
   check('and what cannot be answered is shown, not hidden',
      content().textContent.indexOf('Messages processed, delivered, deferred and bounced per domain.') >= 0);
   check('a section whose source is off says so in the server\'s own words and is marked',
      content().textContent.indexOf('The metric history is switched off') >= 0 && content().textContent.indexOf('Not recorded') >= 0);
   const trafficTable = content().querySelectorAll('table')[1];
   check('the traffic table is drawn from the server\'s own columns',
      trafficTable && trafficTable.querySelectorAll('th').map((h) => h.textContent).join(',') === 'Day,Domain,Incoming,Outgoing,Failed incoming,Failed outgoing',
      trafficTable ? trafficTable.querySelectorAll('th').map((h) => h.textContent).join(',') : 'no table');
   check('and holds a row per day and domain', trafficTable && trafficTable.querySelectorAll('tbody tr').length === 3,
      trafficTable ? String(trafficTable.querySelectorAll('tbody tr').length) : 'none');
   check('the chart is drawn in the page, one bar group per day, with no library and nothing fetched',
      content().querySelectorAll('svg').length === 2 && content().querySelectorAll('rect').length === 6,
      content().querySelectorAll('svg').length + ' charts, ' + content().querySelectorAll('rect').length + ' bars');
   check('every table offers its own CSV at the same route with the same window',
      $('#csv_traffic') && $('#csv_traffic').attributes.href.indexOf('/api/v1/reports/traffic?from=2026-08-17&to=2026-09-15') === 0 &&
      $('#csv_traffic').attributes.href.indexOf('format=csv') > 0 && 'download' in $('#csv_traffic').attributes,
      $('#csv_traffic') ? $('#csv_traffic').attributes.href : 'no link');
   check('the mailboxes section is there too, with its own CSV', !!$('#csv_mailboxes') && !!$('#csv_storage'));
   check('the quota section says who is near their limit, with the points that apply', !!$('#csv_quota') && content().textContent.indexOf('anna@example.com') >= 0 &&
      content().textContent.indexOf('Sending stops at') >= 0, content().textContent.slice(0, 200));
   before = requests.length;
   setValue('repFrom', '2026-09-01');
   setValue('repTo', '2026-09-15');
   document.getElementById('repDomain').value = 'example.com';
   setValue('repTop', '5');
   click(act('repshow'));
   await flush();
   check('Show sends the window, the domain and the row count',
      called(before, 'GET', /reports\/summary\?from=2026-09-01&to=2026-09-15&domain=example\.com&top=5/).length === 1, paths(before));
   check('and the tables are redrawn for that domain alone',
      content().querySelectorAll('table')[1].querySelectorAll('tbody tr').length === 2,
      String(content().querySelectorAll('table')[1].querySelectorAll('tbody tr').length));
   check('the CSV link follows the window', $('#csv_traffic').attributes.href.indexOf('domain=example.com') > 0);
   before = requests.length;
   click(act('repdays', { days: 7 }));
   await flush();
   check('Last 7 days counts back whole days from the end of the window, in UTC',
      called(before, 'GET', /reports\/summary\?from=2026-09-09&to=2026-09-15/).length === 1, paths(before));

   // ---- a session that ends under the page
   signedIn = false;
   await goTo('queue');
   check('a 401 on any view returns to the sign-in card and says why', $('#gate').style.display === 'grid' && $('#loginErr').textContent.indexOf('session ended') >= 0, $('#loginErr').textContent);
   check('with the session mark removed', sessionStorage.getItem('hmsSession') === null);

   // ---- the theme: light, dark, or the system's, and the choice remembered
   await signIn();
   check('with nothing chosen the theme is the system\'s, and the button says which it is on',
      themeStored() === 'system' && document.body.getAttribute('data-theme') === 'dark' &&
      $('#themeBtn').getAttribute('aria-label') === 'Theme: the system\'s choice', themeStored());
   setSystemTheme(false);
   check('a reader who turns their desktop light turns the page light, no choice having been made',
      document.body.getAttribute('data-theme') === 'light' && themeStored() === 'system');
   click($('#themeBtn'));
   check('the button chooses light, says so, and remembers it',
      document.body.getAttribute('data-theme') === 'light' && themeStored() === 'light' && $('#themeBtn').getAttribute('aria-label') === 'Theme: light');
   click($('#themeBtn'));
   check('the next press chooses dark', document.body.getAttribute('data-theme') === 'dark' && themeStored() === 'dark' &&
      $('#themeBtn').getAttribute('aria-label') === 'Theme: dark');
   setSystemTheme(true);
   setSystemTheme(false);
   check('and the system no longer overrules a choice that was made', document.body.getAttribute('data-theme') === 'dark');
   click($('#themeBtn'));
   check('a third press gives the system back, and the page follows it again', themeStored() === 'system' && document.body.getAttribute('data-theme') === 'light');
   setSystemTheme(true);
   click($('#themeBtn'));
   check('the theme can be turned over', document.body.getAttribute('data-theme') === 'light');
   check('and is remembered under a name that is not a secret', localStorage.getItem('hmsTheme') === 'light');
   before = requests.length;
   // ---- the Spam traps view: the record across every domain
   click($('#nav button[data-view="spamtraps"]'));
   await flush();
   check('the Spam traps view lists what has addressed a trap, with what was done and the counter',
      content().textContent.indexOf('never-used@example.com') >= 0 &&
      content().textContent.indexOf('203.0.113.9') >= 0 &&
      content().textContent.indexOf('harvester@spammer.example') >= 0 &&
      content().textContent.indexOf('banned for 1440 minutes') >= 0 &&
      content().textContent.indexOf('Hits since the server started: 1') >= 0,
      content().textContent.slice(0, 300));
   before = requests.length;
   click(act('trapclear'));
   await flush();
   check('clearing the record asks first, says what it does not undo, and sends one DELETE',
      confirmations[confirmations.length - 1].indexOf('bans and the blocked senders they made stay') >= 0 &&
      called(before, 'DELETE', '/api/v1/spam-traps/hits').length === 1, paths(before));
   check('and the view comes back empty',
      content().textContent.indexOf('Nothing has addressed a spam trap.') >= 0,
      content().textContent.slice(0, 200));
   // The counter is a number the server writes, drawn as one whatever the
   // answer holds: nothing in it is read as markup.
   trapHitsSinceStart = '<b id="hitsMark">5</b>';
   click($('#nav button[data-view="spamtraps"]'));
   await flush();
   check('a counter that is not a number is drawn as 0, and nothing it carries is read as markup',
      $('#hitsMark') === null && content().textContent.indexOf('Hits since the server started: 0') >= 0,
      content().textContent.slice(0, 300));
   trapHitsSinceStart = null;

   // ---- the Journaling view: the rules, switched off and on, added and removed

   /* The sidebar is hand-written markup and a view's button is one line in it,
      so the way it goes wrong is a line pasted twice: this row shipped its
      Journaling button twice and every check passed, because nothing had ever
      looked at the sidebar as a whole. Now something does, for every view. */
   check('every view has exactly one button in the sidebar', (() => {
      const seen = Object.create(null);
      const twice = [];
      $$('#nav button[data-view]').forEach((button) => {
         const view = button.getAttribute('data-view');
         if (seen[view]) { twice.push(view); } else { seen[view] = true; }
      });
      return twice.length === 0 ? true : twice.join(', ');
   })() === true, $$('#nav button[data-view]').length + ' buttons');

   click($('#nav button[data-view="journal"]'));
   await flush();
   check('the Journaling view lists every rule with whose mail, which way, what is sent and where it goes',
      content().textContent.indexOf('example.com') >= 0 &&
      content().textContent.indexOf('archive@compliance.example') >= 0 &&
      content().textContent.indexOf('Envelope report') >= 0 &&
      content().textContent.indexOf('copy@second-system.example') >= 0 &&
      content().textContent.indexOf('The message itself') >= 0,
      content().textContent.slice(0, 400));
   check('a rule that is off says so, and its button offers to switch it on',
      act('jrntoggle', { id: '602', value: '1' }) !== null && act('jrntoggle', { id: '601', value: '0' }) !== null);
   before = requests.length;
   click(act('jrntoggle', { id: '601', value: '0' }));
   await flush();
   check('switching a rule off sends enabled false and nothing else',
      JSON.stringify(lastBody(before, 'PUT', '/api/v1/journal-rules/601')) === '{"enabled":false}',
      JSON.stringify(lastBody(before, 'PUT', '/api/v1/journal-rules/601')));
   setValue('newJournalTarget', '');
   setValue('newJournalAddress', 'vault@example.test');
   before = requests.length;
   click(act('jrnadd'));
   await flush();
   check('a rule with nothing to cover is refused by the page and nothing is sent',
      called(before, 'POST', /journal-rules/).length === 0 &&
      document.getElementById('err_journal').textContent === 'A domain or an account address is required.');
   setValue('newJournalTarget', 'example.com');
   setValue('newJournalAddress', '');
   before = requests.length;
   click(act('jrnadd'));
   await flush();
   check('and so is a rule with nowhere to send the copy',
      called(before, 'POST', /journal-rules/).length === 0 &&
      document.getElementById('err_journal').textContent === 'The address the copy goes to is required.');
   setValue('newJournalTarget', 'example.com');
   setValue('newJournalAddress', 'vault@example.test');
   setValue('newJournalNote', 'the retention project');
   before = requests.length;
   click(act('jrnadd'));
   await flush();
   check('a rule is added with every word the page chose',
      JSON.stringify(lastBody(before, 'POST', '/api/v1/journal-rules')) ===
         '{"scope":"domain","target":"example.com","journal_address":"vault@example.test","direction":"both","mode":"report","note":"the retention project"}',
      JSON.stringify(lastBody(before, 'POST', '/api/v1/journal-rules')));
   check('and the view is read again and shows it',
      called(before, 'GET', '/api/v1/journal-rules').length === 1 &&
      content().textContent.indexOf('vault@example.test') >= 0);
   before = requests.length;
   click(act('jrndel', { id: '602', value: 'test@example.com' }));
   await flush();
   check('removing a rule asks, naming it and saying what it does not undo, and deletes it by id',
      confirmations[confirmations.length - 1].indexOf('test@example.com') >= 0 &&
      confirmations[confirmations.length - 1].indexOf('already sent stays') >= 0 &&
      called(before, 'DELETE', '/api/v1/journal-rules/602').length === 1 &&
      content().textContent.indexOf('copy@second-system.example') < 0, paths(before));
   // A rule's id is a number the server writes, and its buttons carry it as
   // one whatever the answer holds: nothing in it becomes an attribute.
   state.journalRules.push({ id: '9" data-extra="1', scope: 'domain', target: 'odd.example', journal_address: 'odd@vault.example',
      direction: 'both', mode: 'report', enabled: true, note: '' });
   click($('#nav button[data-view="journal"]'));
   await flush();
   {
      const oddRow = rows().filter((r) => r.textContent.indexOf('odd.example') >= 0)[0];
      const oddButtons = oddRow ? oddRow.querySelectorAll('button[data-act]') : [];
      check('a rule id that is not a number is carried by its buttons as 0, and nothing it carries becomes an attribute',
         oddButtons.length === 2 && oddButtons.every((b) => b.attributes['data-id'] === '0' && !('data-extra' in b.attributes)),
         JSON.stringify(oddButtons.map((b) => b.attributes)));
   }
   state.journalRules = state.journalRules.filter((r) => r.target !== 'odd.example');

   click($('#logoutBtn'));
   await flush();
   check('signing out ends the session on the server', called(before, 'DELETE', '/api/v1/session').length === 1);
   check('and shows the card', $('#gate').style.display === 'grid' && $('#app').style.display === 'none' && sessionStorage.getItem('hmsSession') === null);
   check('and stops the poll', intervals.length === 0);
   check('no inline handler was needed anywhere: every button is data-act', $$('button[onclick]').length === 0);

   // ---- the language: the catalogues, the switch, the walk and the fallback.
   // This browser (the world above) names no language, so everything up to
   // here ran in English and asked for no catalogue; what follows chooses one.
   const catalogue = (code) => JSON.parse(fs.readFileSync(nodePath.join(CATALOGUES, code + '.json'), 'utf8'));
   const choose = async (select, code) => { select.value = code; select.dispatchEvent(makeEvent('change')); await flush(); };
   check('a browser that names no language gets English and no catalogue is asked for', called(0, 'GET', /^\/deck-lang\//).length === 0 && $('#nav button[data-view="domains"]').textContent.indexOf('Domains') >= 0);
   const codes = $('#langSel').querySelectorAll('option').map((o) => o.attributes.value);
   check('the header carries the switch: English first, then the twenty languages', codes.length === 21 && codes[0] === 'en' && codes.indexOf('de') > 0 && codes.indexOf('zh-Hans') > 0, JSON.stringify(codes));
   check('and the sign-in card carries the same switch', $('#langSelGate').querySelectorAll('option').map((o) => o.attributes.value).join() === codes.join());
   await signIn();
   await goTo('domains');
   const de = catalogue('de');
   before = requests.length;
   await choose($('#langSel'), 'de');
   const fetched = called(before, 'GET', '/deck-lang/de.json');
   check('choosing German fetches its catalogue once, same-origin', fetched.length === 1 && fetched[0].credentials === 'same-origin', paths(before));
   check('the document says which language it is in', document.documentElement.getAttribute('lang') === 'de');
   check('and the choice is remembered under a name that is not a secret', localStorage.getItem('hmsLang') === 'de');
   check('the sidebar is walked: the Domains button reads the German', $('#nav button[data-view="domains"]').textContent.indexOf(de['Domains']) >= 0, $('#nav button[data-view="domains"]').textContent);
   check('the top bar title as well', $('#viewTitle').textContent === de['Domains'], $('#viewTitle').textContent);
   check('an attribute is walked: the sign-out button\'s title', $('#logoutBtn').getAttribute('title') === de['Sign out'], $('#logoutBtn').getAttribute('title'));
   check('the theme button, whose name the script paints rather than the walk, is in the language too',
      $('#themeBtn').getAttribute('aria-label') === de['Theme: light'], $('#themeBtn').getAttribute('aria-label'));
   check('and a placeholder on the sign-in card', $('#pass').getAttribute('placeholder') === de['Password']);
   check('both switches show the choice', $('#langSel').value === 'de' && $('#langSelGate').value === 'de');
   check('the view is drawn again, in German: the table headings', called(before, 'GET', '/api/v1/domains').length === 1 && content().querySelector('th').textContent === de['Domain'], content().querySelector('th').textContent);
   check('and a button in a row', act('domainedit', { name: 'renamed.example' }).textContent === de['Edit'], act('domainedit', { name: 'renamed.example' }).textContent);
   before = requests.length;
   setValue('newDomain', 'sprache.example');
   click(act('domainnew'));
   await flush();
   check('a sentence the script builds carries its value: the toast', toastText() === de['Domain created: {0}'].replace('{0}', 'sprache.example'), toastText());
   check('the editor\'s heading too', content().querySelector('h2').textContent === de['Domain {0}'].replace('{0}', 'sprache.example'), content().querySelector('h2').textContent);
   check('a hint a table holds, marked with K, is looked up when drawn', document.getElementById('dom_active').closest('.fr').querySelector('.hint').textContent === de['Whether the domain receives mail and its accounts can sign in.'],
      document.getElementById('dom_active').closest('.fr').querySelector('.hint').textContent);
   check('and a group title', $$('h2').some((h) => h.textContent === de['General']));
   confirmAnswer = false;
   click(act('domaindel', { name: 'sprache.example' }));
   await flush();
   check('a question the script asks is German, with its value', confirmations[confirmations.length - 1] === de['Delete the domain {0} with every account, alias and list in it? Their messages go with them.'].replace('{0}', 'sprache.example'),
      confirmations[confirmations.length - 1]);
   confirmAnswer = true;
   click(act('cancel'));
   await flush();
   // Every catalogue that ships draws the view in its language.
   for (const code of codes.slice(1)) {
      const cat = catalogue(code);
      await choose($('#langSel'), code);
      check(code + ': the catalogue applied draws the Domains view in that language', document.documentElement.getAttribute('lang') === code && $('#viewTitle').textContent === cat['Domains'] && content().querySelector('th').textContent === cat['Domain'] && act('domainedit', { name: 'renamed.example' }).textContent === cat['Edit'],
         $('#viewTitle').textContent + ' | ' + content().querySelector('th').textContent);
   }
   // A key the catalogue lacks is shown in English; the rest is not.
   const fr = catalogue('fr');
   droppedKey = 'Domains';
   await choose($('#langSel'), 'fr');
   check('a key the catalogue lacks falls back to English while the rest is French', $('#viewTitle').textContent === 'Domains' && $('#nav button[data-view="domains"]').textContent.indexOf('Domains') >= 0 &&
      $('#nav button[data-view="rules"]').textContent.indexOf(fr['Rules']) >= 0 && content().querySelector('th').textContent === fr['Domain'],
      $('#viewTitle').textContent + ' | ' + $('#nav button[data-view="rules"]').textContent);
   droppedKey = null;
   // The catalogue on the server's disk is not the file the build checked, and
   // a translation is inserted where the English stood, in markup as in text:
   // a value that carries a character HTML reads, or is not a string, is left
   // out when the page loads it and the English is shown; the rest applies.
   catalogueTamper = (data) => Object.assign(JSON.parse('{"__proto__":{"tampered":"yes"}}'), data, {
      'Domain': '<b id="langMark">Domäne</b>', 'Edit': 'Bearbeiten & mehr', 'Rules': 'Regeln "neu"', 'Domains': 42 });
   await choose($('#langSel'), 'de');
   check('a catalogue value carrying markup is left out, and nothing of it becomes an element',
      $('#langMark') === null && content().querySelector('th').textContent === 'Domain', content().querySelector('th').textContent);
   check('so is one with an ampersand, one with a double quote and one that is not a string: each is shown in English',
      act('domainedit', { name: 'renamed.example' }).textContent === 'Edit' &&
      $('#nav button[data-view="rules"]').textContent.indexOf('Rules') >= 0 && $('#viewTitle').textContent === 'Domains',
      act('domainedit', { name: 'renamed.example' }).textContent + ' | ' + $('#nav button[data-view="rules"]').textContent + ' | ' + $('#viewTitle').textContent);
   check('and the rest of that catalogue applies, with nothing added to every object',
      document.documentElement.getAttribute('lang') === 'de' && $('#logoutBtn').getAttribute('title') === catalogue('de')['Sign out'] &&
      ({}).tampered === undefined, $('#logoutBtn').getAttribute('title'));
   catalogueTamper = null;
   await choose($('#langSel'), 'fr');
   // A server without the catalogues: the page stays as it was and the switch says so.
   catalogueMissing = true;
   await choose($('#langSel'), 'sv');
   check('a catalogue the server does not have leaves the page in its language, and the switch shows that language', document.documentElement.getAttribute('lang') === 'fr' && $('#langSel').value === 'fr' && $('#langSelGate').value === 'fr' &&
      localStorage.getItem('hmsLang') === 'fr' && content().querySelector('th').textContent === fr['Domain'], $('#langSel').value);
   catalogueMissing = false;
   // Back to English, which is the page itself.
   before = requests.length;
   await choose($('#langSel'), 'en');
   check('English asks the server for nothing and walks the markup back', called(before, 'GET', /^\/deck-lang\//).length === 0 && document.documentElement.getAttribute('lang') === 'en' &&
      $('#nav button[data-view="rules"]').textContent.indexOf('Rules') >= 0 && $('#logoutBtn').getAttribute('title') === 'Sign out' && $('#pass').getAttribute('placeholder') === 'Password' &&
      content().querySelector('th').textContent === 'Domain', $('#nav button[data-view="rules"]').textContent);
   check('and forgets the choice', localStorage.getItem('hmsLang') === null);
   // The sign-in card's switch drives the same mechanism.
   const ja = catalogue('ja');
   await choose($('#langSelGate'), 'ja');
   check('the card\'s switch chooses for the whole page: Japanese on the card, in the sidebar and in the header switch', $('#gate p').textContent === ja['Sign in with the hMailServer administrator credential. The password is exchanged once for a session cookie and is never kept by this page.'] &&
      $('#nav button[data-view="rules"]').textContent.indexOf(ja['Rules']) >= 0 && $('#langSel').value === 'ja', $('#gate p').textContent);
   await choose($('#langSelGate'), 'en');
   check('and back', $('#gate p').textContent.indexOf('Sign in with the hMailServer administrator credential') === 0 && $('#langSel').value === 'en');

   // ---- the tour: teaching a view while somebody uses it
   // Every check here is a way a tour can be wrong that nobody would notice
   // until they walked it on a real server: a step that hangs on a control the
   // view does not have, a tour that moves on before the reader has read it, a
   // Back that closes it, a finished record written for a walk somebody left,
   // a tour that only works in English.
   try { localStorage.removeItem('hmsTours'); } catch (e) { /* no storage */ }
   // From a view no tour visits, so the button starts the walk at its beginning
   // rather than at its stop on the view showing - which is what it does from a
   // view a walk does visit, and is checked further down.
   await goTo('logs');
   before = requests.length;
   click($('#tourBtn'));
   await flush();
   check('the header button starts the tour and opens the view its first stop is on',
      $('#tourCard').hidden === false && $('#nav button[data-view="domains"]').className.indexOf('on') >= 0,
      'card hidden=' + $('#tourCard').hidden + ' view=' + $('#viewTitle').textContent);
   check('the card names the tour and says where the reader is',
      $('#tourName').textContent === 'Show me around' && $('#tourStep').textContent === 'Step 1 of 7',
      $('#tourName').textContent + ' | ' + $('#tourStep').textContent);
   check('and carries the step\'s own sentence', $('#tourText').textContent.indexOf('Start with a domain') === 0, $('#tourText').textContent);
   check('there is nowhere to go back to on the first stop', $('#tourBack').hidden === true);
   check('the tour asks the server for nothing of its own', called(before, 'GET', /tour/).length === 0, paths(before));

   // The ring goes where the element is, the card beside it; an element with no
   // rectangle - what a browser reports for something not on the screen - takes
   // the ring away and leaves the sentence, because the sentence still stands.
   document.getElementById('newDomain').rect = { left: 120, top: 200, width: 220, height: 32 };
   click($('#tourBack'));
   await flush();
   check('Back on the first stop stands still rather than closing the tour',
      $('#tourCard').hidden === false && $('#tourStep').textContent === 'Step 1 of 7', $('#tourStep').textContent);
   check('the ring is drawn over the element the step points at',
      $('#tourRing').hidden === false && $('#tourRing').style.left === '120px' && $('#tourRing').style.top === '200px' &&
      $('#tourRing').style.width === '220px' && $('#tourRing').style.height === '32px', JSON.stringify($('#tourRing').style));
   check('and the card is put under it rather than over it', $('#tourCard').style.top === '244px', $('#tourCard').style.top);

   click($('#tourNext'));
   await flush();
   check('Next moves to the next stop', $('#tourStep').textContent === 'Step 2 of 7', $('#tourStep').textContent);
   check('and Back is offered from the second stop on', $('#tourBack').hidden === false);

   // The third stop points at a control that is only drawn when the view has
   // something to draw it for. Taken away, the step is skipped rather than
   // waited on, and the walk carries on to the stop after it - which is on
   // another view, so the view changes with it.
   content().querySelectorAll('button[data-act="accounts"]').forEach((b) => b.parentNode.removeChild(b));
   click($('#tourNext'));
   await flush();
   check('a stop whose element is not on the view is skipped rather than hung on',
      $('#tourCard').hidden === false && $('#tourStep').textContent === 'Step 4 of 7', $('#tourStep').textContent);
   check('and a stop on another view opens that view',
      $('#nav button[data-view="ports"]').className.indexOf('on') >= 0 && $('#viewTitle').textContent === 'Ports',
      $('#viewTitle').textContent);

   // Leaving records where the reader got to, and says so out loud.
   click($('#tourSkip'));
   await flush();
   check('leaving takes the ring and the card off the screen', $('#tourCard').hidden === true && $('#tourRing').hidden === true);
   check('and says so rather than vanishing in silence', toastText().indexOf('picks up where you left off') > 0, toastText());
   check('where it was left is remembered, in the form the Control Panel keeps in the registry',
      localStorage.getItem('hmsTours') === '|firstrun@3v1', localStorage.getItem('hmsTours'));
   await goTo('logs');
   click($('#tourBtn'));
   await flush();
   check('on a view no tour visits the button picks the walk up where it was left',
      $('#tourStep').textContent === 'Step 4 of 7' && $('#viewTitle').textContent === 'Ports', $('#tourStep').textContent);

   // Escape belongs to the view unless the keyboard is already inside the card.
   $('#user').focus();
   document.dispatchEvent(makeEvent('keydown', { key: 'Escape' }));
   await flush();
   check('Escape with the keyboard outside the card leaves the tour alone', $('#tourCard').hidden === false);
   $('#tourNext').focus();
   $('#tourNext').dispatchEvent(makeEvent('keydown', { key: 'Escape' }));
   await flush();
   check('Escape inside the card leaves the tour', $('#tourCard').hidden === true);

   // A condition ends a step when the thing happens - and does not end one that
   // was already true when the reader arrived, which is what stops the step
   // about domains vanishing in front of somebody who already has one.
   await goTo('ports');
   confirmAnswer = true;
   before = requests.length;
   // Every listener goes, so that the step below opens with the thing it waits
   // for genuinely not yet true. Earlier checks in this file have added
   // listeners of their own, and a condition that was already satisfied would
   // prove nothing either way.
   for (let guard = 0; guard < 20; guard += 1) {
      const ids = content().querySelectorAll('button[data-act="portdel"]').map((b) => b.attributes['data-id']);
      if (!ids.length) { break; }
      click(act('portdel', { id: ids[0] }));
      await flush();
   }
   check('no listener offers TLS now, which is what the step below waits for',
      content().querySelectorAll('button[data-act="portdel"]').length === 0 && called(before, 'DELETE', /^\/api\/v1\/ports\//).length > 0, paths(before));
   try { localStorage.removeItem('hmsTours'); } catch (e) { /* no storage */ }
   click($('#tourBtn'));
   await flush();
   check('the button starts the walk at its first stop on the view showing', $('#tourStep').textContent === 'Step 4 of 7', $('#tourStep').textContent);
   click($('#tourNext'));
   await flush();
   check('the stop that waits for a listener with TLS is showing', $('#tourStep').textContent === 'Step 5 of 7', $('#tourStep').textContent);
   fireIntervals();
   await flush();
   check('and it stays there while the thing it waits for has not happened', $('#tourStep').textContent === 'Step 5 of 7', $('#tourStep').textContent);
   click(act('portnew'));
   await flush();
   check('the card stays when the element it points at goes away, because the sentence still stands',
      $('#tourCard').hidden === false && $('#tourRing').hidden === true);
   setValue('port_protocol', 'smtp');
   setValue('port_port', '587');
   setValue('port_connection_security', 'starttls_required');
   setValue('port_certificate_id', '2');
   click(act('portsave'));
   await flush();
   fireIntervals();
   await flush();
   check('a condition that becomes true while the step is showing moves the tour on by itself',
      $('#tourStep').textContent === 'Step 6 of 7' && $('#viewTitle').textContent === 'Certificates', $('#tourStep').textContent + ' | ' + $('#viewTitle').textContent);
   fireIntervals();
   await flush();
   check('and a step with no condition is not moved on by anything but the reader', $('#tourStep').textContent === 'Step 6 of 7', $('#tourStep').textContent);

   // Walked to its end: the last stop offers Finish rather than another Next,
   // the record says finished, and no resume point is left behind.
   click($('#tourNext'));
   await flush();
   check('the last stop is on the dashboard, where the walk was always going',
      $('#tourStep').textContent === 'Step 7 of 7' && $('#viewTitle').textContent === 'Dashboard', $('#tourStep').textContent);
   check('and it offers Finish rather than promising another stop', $('#tourNext').textContent === 'Finish', $('#tourNext').textContent);
   click($('#tourNext'));
   await flush();
   check('finishing says so', $('#tourCard').hidden === true && toastText().indexOf('That is the end of') === 0, toastText());
   check('and is remembered as finished, with no resume point left behind',
      localStorage.getItem('hmsTours') === 'firstrun=1|', localStorage.getItem('hmsTours'));

   // In the reader's own language, from the catalogues the page already speaks.
   await choose($('#langSel'), 'de');
   await goTo('domains');
   click($('#tourBtn'));
   await flush();
   check('the tour is drawn in the reader\'s language: its name', $('#tourName').textContent === de['Show me around'], $('#tourName').textContent);
   check('its sentence', $('#tourText').textContent === de['Start with a domain: the part after the @ that this server accepts mail for. Add yours here.'], $('#tourText').textContent);
   check('where the reader is, with its numbers', $('#tourStep').textContent === de['Step {0} of {1}'].replace('{0}', '1').replace('{1}', '7'), $('#tourStep').textContent);
   check('and the card\'s own buttons, which the markup walk translates',
      $('#tourNext').textContent === de['Next'] && $('#tourSkip').textContent === de['Skip the tour'],
      $('#tourNext').textContent + ' | ' + $('#tourSkip').textContent);
   click($('#tourSkip'));
   await flush();
   check('the farewell carries the tour\'s name in that language too',
      toastText() === de['{0} closed. It picks up where you left off.'].replace('{0}', de['Show me around']), toastText());
   // A refusal the server gives a reason for is shown in the reader's language:
   // the breach check's two, when an account is made and when one is saved.
   await goTo('domains');
   click(act('accounts', { domain: 'renamed.example' }));
   await flush();
   setValue('newAddr', 'leaked@example.com');
   setValue('newPass', 'password');
   nextRefusalAnswer = { status: 400, body: { error: 'This password appears in a published data breach, so it would be among the first tried against this account. Choose a different one.', reason: 'breached' } };
   click(act('add', { domain: 'renamed.example' }));
   await flush();
   check('German: a breached password refused on create is shown in German, not in the server\'s English',
      $('#err_account').textContent === de['This password appears in a published data breach, so it would be among the first tried against this account. Choose a different one.'], $('#err_account').textContent);
   click(act('accountedit', { address: 'anna@example.com' }));
   await flush();
   setValue('acct_password', 'another-password');
   nextRefusalAnswer = { status: 503, body: { error: 'The password could not be checked against published data breaches just now, and this server is set to refuse a change it cannot check. Try again later.', reason: 'breach_check_unavailable' } };
   click(act('accountsave'));
   await flush();
   check('German: a change the breach check could not make, refused with 503, is shown in German too',
      document.getElementById('err_accountedit').textContent === de['The password could not be checked against published data breaches just now, and this server is set to refuse a change it cannot check. Try again later.'],
      document.getElementById('err_accountedit').textContent);
   check('German: the password hint names the breach check', document.getElementById('acct_password').closest('.fr').querySelector('.hint').textContent ===
      de['A new password goes through the password policy, the reuse history, the breach check and the configured hash exactly as when the Control Panel sets one, and the next logon uses it.'],
      document.getElementById('acct_password').closest('.fr').querySelector('.hint').textContent);
   click(act('accounts', { domain: 'renamed.example' }));
   await flush();
   await choose($('#langSel'), 'en');
   try { localStorage.removeItem('hmsTours'); } catch (e) { /* no storage */ }

   /* ---------------------------------------------------------------- roles */
   await goTo('roles');
   check('the roles view lists every role this server has, with what each may do',
      rows().length >= 3 && content().textContent.indexOf('helpdesk') >= 0 &&
      content().textContent.indexOf('reset_password') >= 0, content().textContent.slice(0, 200));
   check('and says which roles are built in', content().textContent.indexOf('Built in') >= 0);
   check('and lists who holds one, with the domains it reaches',
      content().textContent.indexOf('anna@example.com') >= 0 &&
      content().textContent.indexOf("the account's own domain") >= 0, content().textContent.slice(-400));

   before = requests.length;
   setValue('roleAddress', 'bob@example.com');
   setValue('roleName', 'auditor');
   setValue('roleDomains', 'example.com');
   click(act('assign'));
   await flush();
   put = lastBody(before, 'PUT', /^\/api\/v1\/role-assignments\//);
   check('giving a role names the account in the path and the role in the body',
      called(before, 'PUT', '/api/v1/role-assignments/bob%40example.com').length === 1 &&
      put && put.role === 'auditor' && put.domains === 'example.com', paths(before) + ' ' + JSON.stringify(put));
   check('and the listing shows it afterwards', content().textContent.indexOf('bob@example.com') >= 0);

   before = requests.length;
   setValue('roleAddress', '   ');
   click(act('assign'));
   await flush();
   check('an assignment with no address is refused here and never reaches the server',
      called(before, 'PUT', /role-assignments/).length === 0 &&
      $('#err_roles').textContent.indexOf('account address') >= 0, $('#err_roles').textContent);

   before = requests.length;
   click(act('unassign', { address: 'bob@example.com' }));
   await flush();
   check('taking a role away names the account and nothing else',
      called(before, 'DELETE', '/api/v1/role-assignments/bob%40example.com').length === 1, paths(before));
   check('and the account is gone from the listing', content().textContent.indexOf('bob@example.com') < 0);

   /* A custom role is deleted from its row, and a built-in one has no button to
      try. A role somebody holds is refused by the server with how many accounts
      hold it - deleting it would hand each of them what its admin level gives -
      and the page shows that sentence, not a code. */
   check('a custom role has a delete button and no built-in role has one',
      content().querySelectorAll('button[data-act="roledel"]').length === 1 && act('roledel', { name: 'logreader' }) !== null,
      JSON.stringify(content().querySelectorAll('button[data-act="roledel"]').map((b) => b.attributes)));

   setValue('roleAddress', 'dora@example.com');
   setValue('roleName', 'logreader');
   setValue('roleDomains', '');
   click(act('assign'));
   await flush();

   confirmAnswer = false;
   before = requests.length;
   click(act('roledel', { name: 'logreader' }));
   await flush();
   check('deleting a role asks first, naming it, and no means nothing is sent',
      called(before, 'DELETE', /^\/api\/v1\/roles\//).length === 0 && confirmations[confirmations.length - 1].indexOf('logreader') >= 0,
      confirmations[confirmations.length - 1]);
   confirmAnswer = true;

   before = requests.length;
   click(act('roledel', { name: 'logreader' }));
   await flush();
   check('a role somebody holds is refused, and the refusal is the server\'s sentence beside who holds what',
      called(before, 'DELETE', '/api/v1/roles/logreader').length === 1 &&
      $('#err_roles').textContent === 'The role logreader is held by 1 account, which must be given another role or have this one taken away before the role can be deleted.',
      $('#err_roles').textContent);
   check('and the role and its holder are both still listed',
      content().textContent.indexOf('Reads the logs.') >= 0 && content().textContent.indexOf('dora@example.com') >= 0);

   before = requests.length;
   click(act('unassign', { address: 'dora@example.com' }));
   await flush();
   click(act('roledel', { name: 'logreader' }));
   await flush();
   check('once nobody holds it the role is deleted, gone from the listing, with no refusal left showing',
      called(before, 'DELETE', '/api/v1/roles/logreader').length === 1 && content().textContent.indexOf('Reads the logs.') < 0 &&
      $('#err_roles').textContent === '', paths(before) + ' ' + $('#err_roles').textContent);

   /* -------------------------------------------------------------- devices */
   await goTo('devices');
   check('the devices view asks for an address before it asks the server for anything',
      document.getElementById('devAddress') !== null && content().textContent.indexOf('Type an account address') >= 0);
   before = requests.length;
   setValue('devAddress', 'anna@example.com');
   click(act('devshow'));
   await flush();
   check('Show reads the clients of the address that was typed',
      called(before, 'GET', '/api/v1/accounts/anna%40example.com/devices').length === 1, paths(before));
   check('two clients of one account are two rows', rows().length === 2, String(rows().length));
   check('each row names the protocol, the address, the client and which credential it used',
      content().textContent.indexOf('imap') >= 0 && content().textContent.indexOf('203.0.113.9') >= 0 &&
      content().textContent.indexOf('app_password') >= 0 && content().textContent.indexOf('Phone') >= 0 &&
      content().textContent.indexOf('Mozilla/5.0 (X11; Linux x86_64)') >= 0, content().textContent.slice(-600));

   before = requests.length;
   click(act('devrevoke', { id: '0123456789abcdef0123456789abcdef' }));
   await flush();
   check('revoking names the client in the path and asks for nothing else by default',
      called(before, 'DELETE', '/api/v1/accounts/anna%40example.com/devices/0123456789abcdef0123456789abcdef').length === 1, paths(before));
   check('and the page says what the revoke ended, in the server\'s numbers',
      toastText().indexOf('1') >= 0 && toastText().indexOf('2') >= 0, toastText());
   check('the client is listed as revoked afterwards', content().textContent.indexOf('Revoked') >= 0);

   before = requests.length;
   setChecked('devCredential', true);
   click(act('devrevoke', { id: 'fedcba9876543210fedcba9876543210' }));
   await flush();
   check('with the box ticked the revoke asks for the app password to be disabled too',
      called(before, 'DELETE', /devices\/fedcba9876543210fedcba9876543210\?credential=revoke$/).length === 1, paths(before));

   before = requests.length;
   setValue('devAddress', 'nobody@example.com');
   click(act('devshow'));
   await flush();
   check('an address the server does not know is the server\'s own refusal, not an empty table',
      document.getElementById('err_devices').textContent.indexOf('account not found') >= 0,
      document.getElementById('err_devices').textContent);

   /* A page a role may not use is not shown. The Deck is not the boundary - the
      server refuses the request whatever this page draws - but showing a view
      whose every button answers 403 is the thing this replaces. */
   click($('#logoutBtn'));
   await flush();
   whoami = { identity: 'account:helen@example.com', credential: 'account', administrative: true,
      role: 'helpdesk', permissions: ['view_accounts', 'view_message_trace', 'reset_password', 'unlock_account'],
      domains: ['example.com'] };
   before = requests.length;
   await signIn();
   check('a help desk signs in without asking for the status it may not read',
      called(before, 'GET', '/api/v1/session').length === 1 && called(before, 'GET', '/api/v1/status').length === 0, paths(before));
   check('a help desk is shown the pages it may use',
      !$('#nav button[data-view="domains"]').hidden && !$('#nav button[data-view="lists"]').hidden);
   check('and not the ones it may not: the settings, the logs, the queue and the roles are gone',
      $('#nav button[data-view="settings"]').hidden && $('#nav button[data-view="logs"]').hidden &&
      $('#nav button[data-view="queue"]').hidden && $('#nav button[data-view="roles"]').hidden);
   check('and so are the groups, the remote domains, the reports and the audit trail, which it holds no permission for',
      $('#nav button[data-view="groups"]').hidden && $('#nav button[data-view="remotedomains"]').hidden &&
      $('#nav button[data-view="reports"]').hidden && $('#nav button[data-view="audit"]').hidden);

   // The tour visits the ports, the certificates and the dashboard, none of
   // which a help desk is shown: its walk stops rather than opening a view the
   // server would refuse.
   try { localStorage.removeItem('hmsTours'); } catch (e) { /* no storage */ }
   before = requests.length;
   click($('#tourBtn'));
   await flush();
   for (let guard = 0; guard < 8 && !$('#tourCard').hidden; guard += 1) {
      click($('#tourNext'));
      await flush();
   }
   check('the tour never opens a view the role was left out of',
      called(before, 'GET', /^\/api\/v1\/(ports|certificates|status)/).length === 0 &&
      ['ports', 'certs', 'dash'].every((v) => $('#nav button[data-view="' + v + '"]').className.indexOf('on') < 0), paths(before));
   check('and says where it stopped rather than hanging', $('#tourCard').hidden === true, $('#tourStep').textContent);
   try { localStorage.removeItem('hmsTours'); } catch (e) { /* no storage */ }
   await goTo('domains');
   check('and the view it lands on is one it may use, not the dashboard it may not',
      $('#viewTitle').textContent === 'Domains', $('#viewTitle').textContent);

   // The run renamed example.com earlier; its accounts went with it.
   const helpDomain = Object.keys(state.accounts).filter((d) => state.accounts[d].length > 0 && d.indexOf('<') < 0)[0];
   const helpAddresses = state.accounts[helpDomain].map((a) => a.address);
   click(act('accounts', { domain: helpDomain }));
   await flush();
   check('its accounts list offers a password reset and an unlock on every row',
      helpAddresses.length > 0 &&
      content().querySelectorAll('button[data-act="resetpick"]').length === helpAddresses.length &&
      content().querySelectorAll('button[data-act="unlock"]').length === helpAddresses.length, helpDomain + ' ' + JSON.stringify(helpAddresses));
   check('and neither a delete nor a new-account form, which it may not use',
      content().querySelectorAll('button[data-act="del"]').length === 0 && !document.getElementById('newAddr'));
   check('nor Stop notifications, which is manage_accounts', content().querySelectorAll('button[data-act="stoppush"]').length === 0);

   click(act('accountedit', { address: helpAddresses[0] }));
   await flush();
   check('and the account editor shows no recovery card, whose two buttons are manage_accounts and would only answer 403',
      content().querySelectorAll('button[data-act="recoveryclear"]').length === 0 &&
      content().querySelectorAll('button[data-act="recoverycodesclear"]').length === 0 &&
      content().textContent.indexOf('o....@e.........example') < 0, content().textContent.slice(0, 300));
   click(act('accounts', { domain: helpDomain }));
   await flush();

   click(act('resetpick', { address: helpAddresses[0] }));
   await flush();
   check('choosing a row puts its address in the reset form', document.getElementById('helpAddr').value === helpAddresses[0]);

   before = requests.length;
   setValue('helpPass', '');
   click(act('resetpass', { domain: helpDomain }));
   await flush();
   check('a reset with no password is refused here and never sent',
      called(before, 'POST', /\/password$/).length === 0 && $('#err_help').textContent.indexOf('new password') >= 0, $('#err_help').textContent);

   before = requests.length;
   setValue('helpPass', 'a-new-password');
   click(act('resetpass', { domain: helpDomain }));
   await flush();
   put = lastBody(before, 'POST', /\/password$/);
   check('the reset posts the password to the account\'s own password route and nowhere else',
      called(before, 'POST', '/api/v1/accounts/' + encodeURIComponent(helpAddresses[0]) + '/password').length === 1 && put && put.password === 'a-new-password' &&
      called(before, 'PUT', /^\/api\/v1\/accounts\//).length === 0, paths(before));
   check('and clears the password from the page once it is sent', document.getElementById('helpPass').value === '');

   before = requests.length;
   click(act('unlock', { address: helpAddresses[0] }));
   await flush();
   check('unlock posts to the account\'s unlock route', called(before, 'POST', '/api/v1/accounts/' + encodeURIComponent(helpAddresses[0]) + '/unlock').length === 1, paths(before));

   click($('#logoutBtn'));
   await flush();
   whoami = { identity: 'account:dora@example.com', credential: 'account', administrative: true,
      role: 'domainadmin', permissions: ['administer_domain', 'view_configuration', 'view_accounts', 'view_mailbox', 'view_quarantine',
         'view_message_trace', 'manage_accounts', 'reset_password', 'unlock_account', 'manage_mailbox', 'manage_quarantine'],
      domains: ['example.com'] };
   await signIn();
   check('a domain administrator is shown its domain\'s views and the dashboard',
      !$('#nav button[data-view="domains"]').hidden && !$('#nav button[data-view="dash"]').hidden && !$('#nav button[data-view="roles"]').hidden);
   check('and not the server-wide ones it holds view_configuration for, which a credential confined to a domain is refused',
      $('#nav button[data-view="settings"]').hidden && $('#nav button[data-view="rules"]').hidden &&
      $('#nav button[data-view="ports"]').hidden && $('#nav button[data-view="certs"]').hidden && $('#nav button[data-view="ipranges"]').hidden);
   check('nor the groups and the remote domains, which are the server\'s, nor the audit trail, which it holds no view_audit for',
      $('#nav button[data-view="groups"]').hidden && $('#nav button[data-view="remotedomains"]').hidden &&
      $('#nav button[data-view="audit"]').hidden);
   check('but the reports, which are scoped to its domain rather than refused', !$('#nav button[data-view="reports"]').hidden);
   check('and not eDiscovery, which it holds no view_discovery for', $('#nav button[data-view="discovery"]').hidden);
   check('a domain administrator is shown the inbound webhooks, which view_configuration reads in its own domain',
      !$('#nav button[data-view="inboundwebhooks"]').hidden);
   state.inboundQueue.forEach((d) => { if (d.id === 41) { d.state = 'kept'; d.message_kept = true; } });
   await goTo('inboundwebhooks');
   check('and is offered the Download a kept message\'s view_mailbox allows, and not Add, Edit, Retry, Remove or the pass',
      content().querySelectorAll('a[download]').length >= 1 &&
      content().querySelectorAll('button[data-act="inhooknew"]').length === 0 &&
      content().querySelectorAll('button[data-act="inhookedit"]').length === 0 &&
      content().querySelectorAll('button[data-act="inhookretry"]').length === 0 &&
      content().querySelectorAll('button[data-act="inhookdrop"]').length === 0 &&
      content().querySelectorAll('button[data-act="inhookrun"]').length === 0, content().textContent.slice(0, 300));

   // The arrow keys walk the views that are shown and never stop on one that is not.
   $('#nav button[data-view="domains"]').focus();
   $('#nav button[data-view="domains"]').dispatchEvent(makeEvent('keydown', { key: 'ArrowDown' }));
   await flush();
   check('ArrowDown in the navigation moves to the next view that is shown, past the ones left out',
      document.activeElement === $('#nav button[data-view="fetch"]'),
      document.activeElement && document.activeElement.attributes ? JSON.stringify(document.activeElement.attributes) : String(document.activeElement));

   click($('#logoutBtn'));
   await flush();
   whoami = { identity: 'account:audra@example.com', credential: 'account', administrative: true,
      role: 'auditor', permissions: ['view_configuration', 'view_accounts', 'view_mailbox', 'view_queue', 'view_quarantine',
         'view_archive', 'view_logs', 'view_message_trace', 'view_audit', 'view_discovery'],
      domains: [] };
   await signIn();
   await goTo('discovery');
   check('an auditor, who reads the discovery cases and may not act on them, is offered no Open a case',
      content().querySelectorAll('button[data-act="dscnew"]').length === 0 && rows().length >= 2, content().textContent.slice(0, 300));
   click(act('dscopen', { id: 41 }));
   await flush();
   check('and in a case no search, hold, lift, export or close, but the download, which view_mailbox beside view_discovery allows',
      content().querySelectorAll('button[data-act="dscsearch"]').length === 0 && content().querySelectorAll('button[data-act="dschold"]').length === 0 &&
      content().querySelectorAll('button[data-act="dscrelease"]').length === 0 && content().querySelectorAll('button[data-act="dscexport"]').length === 0 &&
      content().querySelectorAll('button[data-act="dscclose"]').length === 0 && content().querySelectorAll('a[download]').length >= 1,
      content().textContent.slice(0, 300));
   check('an auditor reaching every domain is shown the audit trail, the reports and the remote domains',
      !$('#nav button[data-view="audit"]').hidden && !$('#nav button[data-view="reports"]').hidden &&
      !$('#nav button[data-view="remotedomains"]').hidden && !$('#nav button[data-view="groups"]').hidden);
   await goTo('queue');
   check('an auditor, who reads the queue and may not manage it, sees the warm-up and is offered neither Edit nor Add',
      document.getElementById('warmupBox') !== null && document.getElementById('warmupBox').textContent.indexOf('2001:db8::25') >= 0 &&
      content().querySelectorAll('button[data-act="warmedit"]').length === 0 && content().querySelectorAll('button[data-act="warmnew"]').length === 0,
      content().textContent.slice(0, 300));
   /* And not the one page whose route answers to the administrator password and
      to nothing else. An auditor holds view_configuration, so a view that named
      that permission would be put in front of it and refused on the first click
      - which is the failure this whole mechanism exists to avoid. */
   check('and not the Changed from stock page, whose route answers to the administrator password alone',
      $('#nav button[data-view="changed"]').hidden);

   // The reports need two permissions, because their senders and recipients are
   // who mails whom: a custom role that reads the configuration and not the
   // message trace is shown the settings and not the reports, and given the
   // trace as well it is shown both.
   click($('#logoutBtn'));
   await flush();
   whoami = { identity: 'account:rita@example.com', credential: 'account', administrative: true,
      role: 'config-reader', permissions: ['view_configuration'], domains: [] };
   await signIn();
   check('a role holding view_configuration and not view_message_trace is not shown the reports, which name who mails whom',
      $('#nav button[data-view="reports"]').hidden && !$('#nav button[data-view="settings"]').hidden);
   await goTo('inboundwebhooks');
   check('and is shown the inbound webhooks without the Download of a kept message, which is view_mailbox',
      rows().length >= 1 && content().querySelectorAll('a[download]').length === 0, content().textContent.slice(0, 300));

   click($('#logoutBtn'));
   await flush();
   whoami = { identity: 'account:rita@example.com', credential: 'account', administrative: true,
      role: 'config-and-trace-reader', permissions: ['view_configuration', 'view_message_trace'], domains: [] };
   await signIn();
   check('and holding both it is shown them', !$('#nav button[data-view="reports"]').hidden);

   click($('#logoutBtn'));
   await flush();
   whoami = { identity: 'account:nobody@example.com', credential: 'account', administrative: false,
      role: '', permissions: [], domains: [] };
   await signIn();
   check('a credential that reaches no page is told so rather than shown an empty Deck',
      content().textContent.indexOf('reaches none of the pages') >= 0, content().textContent.slice(0, 200));

   click($('#logoutBtn'));
   await flush();
   whoami = null;
   await signIn();
   check('and the administrator is shown everything again, the administrator-only page included',
      !$('#nav button[data-view="settings"]').hidden && !$('#nav button[data-view="roles"]').hidden &&
      !$('#nav button[data-view="changed"]').hidden);

   // ---- help, beside the view
   // What the row asks for and a build cannot see: the button opens the
   // pane and not a window, the document is the view's, the text is drawn
   // as headings and lists and tables rather than as a wall, the pane
   // follows the view, F1 and Escape do what the list says, and the
   // choice to keep it open is this browser's.
   try { localStorage.removeItem('hmsHelp'); } catch (e) { /* no storage */ }
   await goTo('backup');
   check('the help pane is closed until it is asked for', $('#helpPane').hidden === true && $('#helpBtn') !== null && $('#helpBtn').textContent === '?');
   before = requests.length;
   click($('#helpBtn'));
   await flush();
   check('the ? button opens the pane beside the view and fetches the view\'s document from the help route',
      $('#helpPane').hidden === false && called(before, 'GET', '/api/v1/help/Backup').length === 1, paths(before));
   check('the pane is titled by the document\'s first heading', $('#helpTitle').textContent === 'What the backup carries, and what it does not', $('#helpTitle').textContent);
   check('a heading is rendered as a heading, a list as a list and a table as a table, all as nodes',
      $('#helpBody').querySelector('h3') !== null && $('#helpBody').querySelector('ul li') !== null && $('#helpBody').querySelector('table td') !== null,
      $('#helpBody').textContent.slice(0, 120));
   check('the document\'s other headings are listed at the top, each a button', $('#helpHeads').querySelectorAll('button').length >= 5 &&
      $('#helpHeads').textContent.indexOf('In the archive') >= 0, $('#helpHeads').textContent);
   // The wiki names its pages its own way: the backup document's page is
   // Backup-and-Restore, and a link of the document's name, Backup, opens
   // GitLab's "page does not exist".
   check('Open on the wiki is the base URL the server answered with the page the wiki made from the document, not the document\'s own name',
      $('#helpWiki').hidden === false && $('#helpWiki').getAttribute('href') === 'https://wiki.test/Backup-and-Restore', String($('#helpWiki').getAttribute('href')));
   // A wiki base that is not a web address - refused by the server when it is
   // set, but a value can reach the database another way - is no link. The
   // index is asked once per page, so the page is made to ask again, here by
   // forgetting what it was told, and the document to be drawn again.
   {
      const askAgain = async () => { vm.runInThisContext('helpPages=null;helpShown="";helpAt="";'); await goTo('backup'); };
      for (const base of ['javascript:void(0);//', 'data:text/html,', 'file:///C:/wiki/']) {
         helpWikiBase = base;
         await askAgain();
         check('a wiki base that is not a web address draws no Open on the wiki link: ' + JSON.stringify(base),
            $('#helpWiki').hidden === true && $('#helpWiki').getAttribute('href') === null && $('#helpTitle').textContent === 'What the backup carries, and what it does not',
            String($('#helpWiki').getAttribute('href')));
      }
      helpWikiBase = 'https://wiki.test/';
      await askAgain();
      check('and the web address is a link again once the server says one',
         $('#helpWiki').hidden === false && $('#helpWiki').getAttribute('href') === 'https://wiki.test/Backup-and-Restore', String($('#helpWiki').getAttribute('href')));
   }
   check('the choice to keep the pane open is remembered', localStorage.getItem('hmsHelp') === '1');
   before = requests.length;
   await goTo('rules');
   check('the pane follows the view: Rules opens the mail flow rules document',
      called(before, 'GET', '/api/v1/help/MailFlowRules').length === 1 && $('#helpTitle').textContent === 'Mail flow rules', paths(before) + ' ' + $('#helpTitle').textContent);
   check('a document the wiki made no page from has no Open on the wiki link, rather than one to a page that does not exist: the mail flow rules',
      wikiPageOf('MailFlowRules') === '' && $('#helpWiki').hidden === true && $('#helpWiki').getAttribute('href') === null,
      JSON.stringify(wikiPageOf('MailFlowRules')) + ' ' + String($('#helpWiki').getAttribute('href')));
   before = requests.length;
   await goTo('queue');
   check('and a view mapped to a heading opens that document: the queue opens the stalled-mail runbook',
      called(before, 'GET', '/api/v1/help/DiagnosingStalledMail').length === 1 && $('#helpTitle').textContent === 'Diagnosing slow or stalled mail', paths(before));
   check('whose Open on the wiki is the wiki\'s copy of it, Diagnosing-Stalled-Mail, again by the wiki\'s name',
      $('#helpWiki').hidden === false && $('#helpWiki').getAttribute('href') === 'https://wiki.test/Diagnosing-Stalled-Mail', String($('#helpWiki').getAttribute('href')));
   before = requests.length;
   await goTo('roles');
   check('the third of three: Roles opens the roles document', called(before, 'GET', '/api/v1/help/Roles').length === 1, paths(before));
   before = requests.length;
   await goTo('roles');
   check('the same view again asks the server for nothing', called(before, 'GET', /^\/api\/v1\/help/).length === 0, paths(before));
   press('Escape');
   check('Escape closes the pane and forgets the choice', $('#helpPane').hidden === true && localStorage.getItem('hmsHelp') === null);
   press('F1');
   await flush();
   check('F1 opens it again', $('#helpPane').hidden === false && localStorage.getItem('hmsHelp') === '1');
   press('F1');
   check('and closes it', $('#helpPane').hidden === true);
   click($('#helpBtn'));
   await flush();
   click($('#helpAll'));
   await flush();
   check('All documentation lists every document by its title, for opening in the pane',
      $('#helpTitle').textContent === 'All documentation' && $('#helpBody').querySelectorAll('.doclist button').length === helpPages().length &&
      $('#helpBody').textContent.indexOf('What the backup carries') >= 0, String($('#helpBody').querySelectorAll('.doclist button').length));
   check('and the documents it lists are the ones the server embeds (HelpPagesData.cpp), no more and no fewer',
      HELP_EMBEDDED.length > 0 && JSON.stringify(helpPages().map((p) => p.name).sort()) === JSON.stringify(HELP_EMBEDDED.slice().sort()),
      helpPages().length + ' listed, ' + HELP_EMBEDDED.length + ' in HelpPagesData.cpp');
   const HELP_TABLE_ROWS = ['      { "AccountRecovery", 25656, Page0Pieces, Page0Headings },',
      '      { "AccountRecovery", 25656, Page0Pieces, Page0Headings, "Sign-in-and-Identity" },',
      '      { "AccountRecovery", 25656, Page0Pieces, Page0Headings, "" }'];
   check('and a row of that table is read by its head, with a field after its headings, such as a wiki page, or without one',
      HELP_TABLE_ROWS.every((row) => (HELP_ROW.exec(row) || [])[1] === 'AccountRecovery') &&
      HELP_ROW.exec('      const HelpHeading Page0Headings[] =') === null,
      HELP_TABLE_ROWS.map((row) => String(HELP_ROW.exec(row))).join(' | '));
   before = requests.length;
   click($('#helpBody').querySelectorAll('.doclist button')[0]);
   await flush();
   check('and one of them opens', called(before, 'GET', /^\/api\/v1\/help\/[A-Za-z]+$/).length === 1 && $('#helpBody').querySelector('h3') !== null, paths(before));
   // The pane holds still. render() runs for the dashboard's poll and for
   // every save, and the pane follows it; what it followed last is
   // remembered, so the same view again moves nothing and a different view
   // opens its document on its heading, once.
   await goTo('backup');
   const helpBody = $('#helpBody');
   helpBody.scrollTop = 480;
   let scrolledBefore = scrollsIntoView;
   await goTo('backup');
   check('the same view drawn again leaves the pane where the reader left it', helpBody.scrollTop === 480 && scrollsIntoView === scrolledBefore,
      'scrollTop=' + helpBody.scrollTop + ' scrolls=' + (scrollsIntoView - scrolledBefore));
   scrolledBefore = scrollsIntoView;
   await goTo('dash');
   check('a different view opens its document on its heading, once', scrollsIntoView === scrolledBefore + 1 && $('#helpTitle').textContent === 'The first-hour checklist',
      'scrolls=' + (scrollsIntoView - scrolledBefore) + ' ' + $('#helpTitle').textContent);
   helpBody.scrollTop = 320;
   scrolledBefore = scrollsIntoView;
   fireIntervals();
   await flush();
   check('the dashboard\'s poll redraws the view and leaves the pane where the reader left it', helpBody.scrollTop === 320 && scrollsIntoView === scrolledBefore,
      'scrollTop=' + helpBody.scrollTop + ' scrolls=' + (scrollsIntoView - scrolledBefore));
   // A heading written with inline markup is listed without it and is reachable:
   // the pane scrolls by the line the heading starts on, not by its text.
   await goTo('lists');
   check('Lists opens the distribution lists document', $('#helpTitle').textContent === 'Distribution lists that define themselves', $('#helpTitle').textContent);
   const marked = Array.from($('#helpHeads').querySelectorAll('button')).filter((b) => b.textContent === 'What it does not do')[0];
   check('a heading written with inline markup is listed by its text, without the markup', !!marked, $('#helpHeads').textContent);
   helpBody.scrollTop = 200;
   scrolledBefore = scrollsIntoView;
   if (marked) { click(marked); }
   check('and choosing it scrolls to it rather than to the top', scrollsIntoView === scrolledBefore + 1 && helpBody.scrollTop === 200,
      'scrolls=' + (scrollsIntoView - scrolledBefore) + ' scrollTop=' + helpBody.scrollTop);
   // Escape in the search box is the search box's, as the shortcut list says.
   $('#deckSearch').value = 'lists';
   press('Escape', $('#deckSearch'));
   check('Escape in the search box leaves the search box and leaves the pane open', $('#helpPane').hidden === false && $('#deckSearch').value === '',
      'hidden=' + $('#helpPane').hidden + ' search=' + JSON.stringify($('#deckSearch').value));
   press('Escape');
   check('and Escape on the page closes the pane', $('#helpPane').hidden === true);

   if (failures.length) {
      out.error('\ndeck script: ' + failures.length + ' of ' + checks + ' checks failed\n');
      failures.forEach((f) => out.error('  FAILED: ' + f));
      out.error('');
      process.exit(1);
   }
   out.log('deck script: ' + checks + ' checks passed');
}

main().catch((why) => {
   out.error('the deck script did not survive being run:');
   out.error(why && why.stack ? why.stack : why);
   process.exit(1);
});
