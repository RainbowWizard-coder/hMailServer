// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The Trusted Types policies of the Control Deck and the webmail, executed.
//
// Both pages are served with require-trusted-types-for 'script' and
// trusted-types default, and each makes the one policy, named default, that
// every string passes through on its way to an HTML sink or to a script's
// address (index.html, at the top of its script; Portal.js, at the top of
// its function). The stub DOMs of deck-script-test.js and portal-script-test.js
// have no Trusted Types, so nothing there calls the policy; a browser does,
// and a policy that refused what the page builds would leave a view empty in
// every browser that has them. So this lifts each page's policy out of the
// page, runs it against a recording trustedTypes, and holds it to what it
// must do:
//
//   the Deck   one policy, named default, with createHTML and nothing else;
//              the markup the views build goes through unchanged - escaped
//              text included, whatever it spells - and markup the Deck never
//              builds (a script, a style or link element, a meta refresh, a
//              base, a frame, an object, a form...) is refused, in any case
//              and as a start or an end tag;
//   the portal one policy, named default; the HTML of a message given to
//              DOMParser goes through as it is; anything put into the page
//              itself loses the elements the page never builds, and is refused
//              when one would still be there; two script addresses, the
//              worker's and the OpenPGP module's, and no other.
//
// And neither policy is made, nor anything thrown, in a browser without them.
//
// Usage: node build/trusted-types-test.js deck <index.html>
//        node build/trusted-types-test.js portal <Portal.js>
// Run by build/check-deck-script.py and build/check-portal-script.py.
'use strict';

const fs = require('fs');
const vm = require('vm');

const [which, file] = process.argv.slice(2);
if (!(which === 'deck' || which === 'portal') || !file) {
   console.error('usage: node build/trusted-types-test.js deck <index.html> | portal <Portal.js>');
   process.exit(2);
}

let failures = 0, passed = 0;
function check(name, ok, detail) {
   if (ok) { passed++; return; }
   failures++;
   console.error('FAIL  ' + name + (detail === undefined ? '' : ': ' + detail));
}

const source = fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n');

// The policy's block, from its heading to the next section of the page.
function block() {
   if (which === 'deck') {
      const at = source.indexOf('/* ---------- Trusted Types ----------');
      if (at < 0) { return null; }
      const end = source.indexOf('})();', at);
      return end < 0 ? null : source.slice(at, end + 5);
   }
   const at = source.indexOf('  // ---- Trusted Types.');
   if (at < 0) { return null; }
   const end = source.indexOf('\n  // ---- ', at + 10);
   return end < 0 ? null : source.slice(at, end);
}

// Runs the block with a trustedTypes that records the policies made, or with
// none at all.
function run(code, withTrustedTypes) {
   const made = [];
   const trustedTypes = withTrustedTypes ? {
      createPolicy(name, rules) { made.push({ name, rules }); return rules; }
   } : undefined;
   const window = { trustedTypes };
   const context = vm.createContext({ window, trustedTypes, TypeError, String, RegExp });
   vm.runInContext(code, context, { filename: which + '-trusted-types.js' });
   return made;
}

function throws(fn) {
   try { fn(); return false; } catch (e) { return e instanceof Error || (e && e.name === 'TypeError'); }
}

const code = block();
check('the page makes a Trusted Types policy (its block is where this test looks for it)', !!code);
if (!code) {
   console.error(which + ' trusted types: ' + failures + ' failed');
   process.exit(1);
}

check('without Trusted Types the block makes no policy and throws nothing', (() => {
   try { return run(code, false).length === 0; } catch (e) { return false; }
})());

const made = run(code, true);
check('exactly one policy is made', made.length === 1, made.length);
const policy = made.length ? made[0] : { name: '', rules: {} };
check('the policy is named default, the one name the page\'s trusted-types directive allows', policy.name === 'default', policy.name);
const html = (value, sink) => policy.rules.createHTML(value, 'TrustedHTML', sink || 'Element innerHTML');

// Markup no page ever builds, each as a start tag and an end tag, in three cases.
const NEVER = ['script', 'style', 'link', 'meta', 'base', 'iframe', 'frame', 'frameset', 'object', 'embed', 'applet', 'form',
   'portal', 'template', 'noscript', 'xmp', 'plaintext', 'noembed', 'noframes', 'math', 'foreignObject', 'animate',
   'animateMotion', 'animateTransform', 'set', 'use'];
const forms = (name) => ['<' + name + '>', '<' + name.toUpperCase() + ' a="b">', '<' + name[0].toUpperCase() + name.slice(1) + '/>',
   '<' + name + '\n', 'x</' + name + '>', '<div>' + '<' + name];

if (which === 'deck') {
   check('the policy makes HTML and nothing else: no script and no script address', typeof policy.rules.createHTML === 'function' &&
      policy.rules.createScript === undefined && policy.rules.createScriptURL === undefined, Object.keys(policy.rules).join(','));
   // What the views build: tables, forms' fields, buttons, the drawings, links,
   // and escaped values that spell what is refused.
   const built = [
      '<div class="grid g4 view"><div class="card"><h2>Queue</h2><table class="grid"><thead><tr><th>Id</th></tr></thead><tbody><tr><td>1</td></tr></tbody></table></div></div>',
      '<label>Name <input type="text" id="n" value="&lt;meta http-equiv=&quot;refresh&quot;&gt;"></label><select><option value="1">One</option></select><textarea>&lt;script&gt;</textarea>',
      '<button class="btn" data-act="save">Save</button> <a href="/api/v1/logs/x" target="_blank" rel="noopener">log</a>',
      '<svg viewBox="0 0 100 20"><title>t</title><g><path d="M0 0L1 1"/><polyline points="0,0 1,1"/><rect/><circle/><line/><text>3</text></svg>',
      '<pre><code>&lt;form action="x"&gt;&lt;/form&gt; &lt;iframe src=x&gt; &lt;style&gt;</code></pre><dl><dt><kbd>?</kbd></dt><dd>help</dd></dl>',
      '<p>a &lt;set&gt; of rules, settings, user&lt;use&gt;, a &lt;base&gt; path, <b>bold</b> <strong>x</strong> <i>y</i></p><ul><li>one</li></ul>',
      '<img src="/api/v1/accounts/a%40b/directory/photo" alt="" width="48" height="48"><br><span class="badge good">Running</span>',
      '', 'plain text with no markup at all', '<settings-panel></settings-panel><setup></setup><user-list></user-list><formula></formula><metadata></metadata>'
   ];
   for (const markup of built) {
      let out;
      try { out = html(markup); } catch (e) { out = e; }
      check('markup the Deck builds goes through unchanged: ' + markup.slice(0, 60), out === markup, out instanceof Error ? out.message : out);
   }
   for (const name of NEVER) {
      for (const form of forms(name)) {
         check('markup the Deck never builds is refused: ' + JSON.stringify(form), throws(() => html(form)));
      }
   }
   check('a meta refresh in the middle of a view is refused', throws(() => html('<div class="view"><p>x</p><meta http-equiv="refresh" content="0;url=https://elsewhere.example/"></div>')));
} else {
   check('the policy makes HTML and script addresses, and no script', typeof policy.rules.createHTML === 'function' &&
      typeof policy.rules.createScriptURL === 'function' && policy.rules.createScript === undefined, Object.keys(policy.rules).join(','));
   // A message's HTML, read for its text or cleaned: DOMParser makes a
   // document that is never shown and runs nothing, so it goes through as it is.
   const message = '<html><head><meta charset="utf-8"><style>p{color:red}</style><link rel="stylesheet" href="https://x/"></head>' +
      '<body><form action="https://x/"><input name="a"></form><script>alert(1)</script><iframe src="https://x/"></iframe><p>Hello</p></body></html>';
   check('a message\'s HTML goes to DOMParser as it is', html(message, 'DOMParser parseFromString') === message);
   check('a list being cleared', html('') === '');
   check('a signature the page could have built goes through unchanged', html('<p>Kind regards,<br><b>Ann</b> <a href="https://example.com/">site</a> <img src="data:image/png;base64,AA" alt="logo"></p><table><tr><td>x</td></tr></table>') ===
      '<p>Kind regards,<br><b>Ann</b> <a href="https://example.com/">site</a> <img src="data:image/png;base64,AA" alt="logo"></p><table><tr><td>x</td></tr></table>');
   check('the compose editor\'s signature block goes through unchanged', html('<br>-- <br><p>Ann</p>') === '<br>-- <br><p>Ann</p>');
   // A signature saved with a whole document's furniture, as one pasted from
   // another program can be: the page keeps the signature and drops the rest.
   const pasted = html('<meta charset="utf-8"><style>p { margin: 0 }</style><link rel="stylesheet" href="x.css"><p>Ann</p><STYLE type="text/css">b{}</STYLE><b>Smith</b>');
   check('a signature loses a style element, its rules with it, a meta and a link, and keeps the rest', pasted === '<p>Ann</p><b>Smith</b>', pasted);
   check('a signature loses a frame whose inline document loads a script', html('<p>x</p><iframe srcdoc="<script src=/files/abc></script>"></iframe>') === '<p>x</p>');
   check('a signature loses a script element and its text', html('<p>x</p><script>fetch("/api/v1/me")</script><p>y</p>') === '<p>x</p><p>y</p>');
   check('a signature loses a form, and what was inside it stays as it was', html('<form action="/x"><input name="p"></form><p>z</p>') === '<input name="p"><p>z</p>');
   check('a signature loses a meta refresh', html('<p>x</p><meta http-equiv="refresh" content="0;url=https://elsewhere.example/">') === '<p>x</p>');
   check('a string that would still hold a refused element once the others are out is refused', throws(() => html('<scr<meta>ipt>alert(1)</script>')));
   const rebuilt = (() => { try { return html('<me<style></style>ta http-equiv="refresh" content="0;url=https://x/">'); } catch (e) { return null; } })();
   check('one that rebuilds a meta refresh as the others come out keeps none', rebuilt === null || !/<meta/i.test(rebuilt), rebuilt);
   // An element goes with its text up to its own end tag, in one pass: what
   // the one pattern that says so would leave - held to it on the shapes
   // below - in a time that grows with the string, not with its square.
   const refused = new RegExp('<\\/?(?:' + NEVER.join('|') + ')(?=[\\s/>]|$)', 'i');
   const byPattern = (s) => {
      const kept = s.replace(/<(script|style|template|noscript|xmp|noembed|noframes|iframe|object|math)(?=[\s/>])[\s\S]*?<\/\1\s*>/gi, '')
         .replace(new RegExp('<\\/?(?:' + NEVER.join('|') + ')(?=[\\s/>]|$)[^>]*>?', 'gi'), '');
      return refused.test(kept) ? 'refused' : kept;
   };
   for (const shape of ['<style>a</style><p>b</p><style>c', '<STYLE >x</style >y<script>z</SCRIPT>w', '<style>a<style>b</style>c</style>d',
      '<script>a</style>b</script>c', '<math><mi>x</mi></math><p>y</p><math>', '<style>a</styles>b</style>c', '<style>a</style\n>b',
      '<iframe srcdoc="<script></script>">q</iframe>r', '<object data=x><embed src=y></object>z<template><p>t</template>', '<style/>a</style>b',
      '<noscript><p>n</noscript><xmp>x</xmp><noembed>e</noembed><noframes>f</noframes>g', '<p>' + 'a<style '.repeat(40) + '</style>tail',
      '<style>x' + '<script '.repeat(20) + '</style>y<script>z', '<sty<style>s</style>le>q</style>', '<script>a</script><style>b']) {
      let out;
      try { out = html(shape); } catch (e) { out = 'refused'; }
      check('taken out as the pattern says: ' + JSON.stringify(shape.slice(0, 50)), out === byPattern(shape), out);
   }
   const many = '<style '.repeat(150000);
   const started = Date.now();
   try { html(many); } catch (e) { /* refused or not, it is the time that is asked */ }
   const took = Date.now() - started;
   check('a megabyte of start tags with no end tag is read in time that grows with it (' + took + ' ms)', took < 2000, took);
   for (const name of NEVER) {
      for (const form of forms(name)) {
         let out;
         try { out = html(form); } catch (e) { out = null; }
         check('no element the page never builds survives: ' + JSON.stringify(form), out === null || !new RegExp('<\\/?' + name + '(?=[\\s/>]|$)', 'i').test(out), out);
      }
   }
   const url = (value) => policy.rules.createScriptURL(value, 'TrustedScriptURL', 'HTMLScriptElement src');
   check('the worker\'s address is a script address', url('/portal-sw.js') === '/portal-sw.js');
   check('the OpenPGP module\'s address is a script address', url('/portal-openpgp.js') === '/portal-openpgp.js');
   for (const other of ['/files/abc', '/api/v1/me/messages/1/attachments/0', 'https://elsewhere.example/x.js', '/portal-sw.js?x=1',
      '//elsewhere.example/portal-sw.js', 'data:text/javascript,1', '/portal.js', ' /portal-sw.js']) {
      check('no other address is a script address: ' + other, throws(() => url(other)));
   }
}

if (failures) {
   console.error(which + ' trusted types: ' + failures + ' of ' + (failures + passed) + ' checks failed');
   process.exit(1);
}
console.log(which + ' trusted types: ' + passed + ' checks passed');
