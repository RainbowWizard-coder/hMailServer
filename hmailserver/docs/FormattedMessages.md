Writing a formatted message: emoji, pictures and tables
=======================================================

The webmail's compose window has two editors behind one button. *Formatting* turns the
plain box into a rich editor, and what the reader writes there is rebuilt from an allowed
subset of HTML and sent as `multipart/alternative` - the HTML, and a plain text made from
the same content, so that a reader who takes only text still gets the words.

The editor took text, links, lists and quotes. This adds the three things people reach for
next: an emoji picker, a picture put straight into the message, and a simple table.

All of it is the page. There is no new route, no new setting and no schema change; the one
thing the server already did for this - moving a `data:` image into a `cid:` part so that
Gmail and Outlook display it - is [Signatures.md](Signatures.md), and it is what carries
the pictures here too.

What the rebuild admits
-----------------------

Nothing reaches a recipient that the rebuild does not put there. It walks the editor and
writes a new document from what it recognises, taking from each element its tag and
nothing else:

| Written | From |
| --- | --- |
| `b` `i` `u` | bold, italic, underline, and `strong`, `em`, `h1`-`h3` |
| `br` `p` | breaks, paragraphs and `div` |
| `ul` `ol` `li` | lists |
| `blockquote` | quotes |
| `a href` | a link to `http:`, `https:` or `mailto:` - any other address is dropped and the words kept |
| `img src` | an image the editor holds as a `data:` URL of PNG, JPEG, GIF or WebP, under 2,000,000 characters, with its `alt` |
| `table` `tr` `td` `th` | a table; `thead`, `tbody` and `tfoot` all become one `tbody` |

Everything else is dropped and its words kept: a style, a width, a class, an `onclick`, a
`colspan`, a `<video>`, a `<script>`, a remote `<img src="https://...">`. A table pasted
from a spreadsheet or a web page therefore arrives as its cells and their words, with the
border this page gives every table and none of the borders, widths or colours it had.

Every table is written with the border, padding and spacing that the reader saw, as the
HTML attributes rather than only as a stylesheet:

```html
<table border="1" cellpadding="6" cellspacing="0" style="border-collapse:collapse">
```

Outlook renders mail with Word's engine, which ignores most of a stylesheet and honours
those attributes; a table styled only in CSS arrives there without lines.

Emoji
-----

*Emoji* beside *Formatting* opens a picker. It is a table in the page itself - about a
thousand characters in eight groups, Smileys, People, Animals and nature, Food and drink,
Activities, Travel and places, Objects and Symbols - because the page's content policy
runs no script but its own and fetches nothing, so a library or a service is not an
option here, and a message being written should not be a reason to ask anything of the
network.

The search box narrows the grid to the emoji whose names hold every word typed, `Enter`
takes the first match, and `Esc` closes the picker and puts the cursor back where it was.
The names are English, and they are the one part of this page that is not translated: a
thousand names in twenty-three languages is a catalogue of its own, and the picker is
browsed as much as it is searched. The group headings are translated.

One picked goes where the cursor is - in the rich editor through the browser's own
editing command, so that the browser's undo takes it back, and in the plain box at the
cursor, over whatever is selected. Emoji work in a plain message as well as a formatted
one; they are text.

The last thirty used come back at the top under *Recently used*. They live in this
browser (`localStorage`, key `hmPortalEmoji`), never reach the server, and are forgotten
at sign-out with everything else the page keeps.

There are no skin tones and no flags. The table is what people reach for in a message,
not the standard.

Pictures in the message
-----------------------

Three ways in: paste one into the editor, drop the file on it, or use *Image* in the
formatting bar. A picture pasted or dropped on the *plain* box turns formatting on first,
because a plain message cannot hold a picture, and lands after the words.

What is accepted is PNG, JPEG, GIF and WebP, decided by the file's first bytes and not by
its name - `report.png` that is really a program is refused, and so is an SVG, which is a
document rather than a picture and is not displayed by mail clients anyway.

A large picture is drawn down to 1,200 pixels on its longer side before it goes in (a
screenshot of a whole screen is three times that), a JPEG re-encoded as a JPEG and
anything else as a PNG, and smaller again until it fits. The page says *The image was made
smaller to fit.* when it did. A picture already within that box is kept byte for byte, so
a JPEG is not encoded twice for nothing. A browser with no canvas does not resize, and a
picture too large for the bounds is then refused rather than sent.

The bounds:

* one picture at most 2,000,000 characters as a `data:` URL, which is what the rebuild
  admits;
* at most 64 pictures in a message, which is how many the send route puts back in place
  (`InlineImageParts::MostImages`). The sixty-fifth would ride out as the `data:` URL it
  is, and a `data:` image is the one thing Gmail and Outlook do not show, so the page
  refuses it rather than send a message whose last pictures are blank. A walkthrough with
  a small screen grab at every step reaches sixty-five long before it reaches six
  megabytes, so this is a bound of its own and not a consequence of the bytes;
* a message's pictures at most 6 MB of characters together;
* and those characters counted for what they are worth in bytes - base64 is four
  characters to three bytes - against the same 12 MB the files attached to a message may
  be, together with whatever a reply, a forward or a reopened draft carried over from the
  original, because the pictures and every one of those files travel in the one request
  the route bounds at sixteen megabytes. A message over it is refused in the page, in a
  sentence, rather than by a connection that fails.

A paste or a drop may carry several files. Each goes in in turn and the page writes one
line at the end for the whole batch, naming every file it would not take: a refusal used
to be wiped off that line by the next file of the same batch going in, and a reader who
dropped three files and had one refused was told nothing at all.

While the message is being written each picture is a `data:` URL in the page. On send, and
on a draft save, the route moves each one into a part of its own with a Content-ID and
points the HTML at it with a `cid:` URL - `multipart/related` inside the alternative, the
shape Thunderbird writes - which is what Gmail and Outlook display and a `data:` image is
not. That is `InlineImageParts`, and [Signatures.md](Signatures.md) describes it in full.

A draft written this way reopens formatted, with its own pictures back in the editor as
the `data:` URLs they were, read from the parts they became; so a draft may be reopened and
saved as often as the writer likes without the pictures turning into attachments. An
image another mail client embedded in a draft is left as the file it is and carried as an
attachment, as it always was.

Only a draft's own formatting comes back this way, and only a message marked `\Draft`.
`#compose?draft=<id>` names a message by its number, and nothing stops that number being
the number of a message that arrived from outside, whose HTML is a stranger's; this is the
one place the page puts message HTML into its own document rather than into the sandboxed
frame an open message is shown in. A message without the flag reopens as its text, as
every draft did before formatting came back at all. The rebuild admits a `data:` source
whole or not at all for the same reason: the whole value must be the base64 alphabet, which
is what the server's own reader admits (`InlineImageParts::ParseDataUrl_`), so that a value
ending in a quote cannot carry markup along with it - and the value is escaped again where
it is written, so that the encoder is right whatever the test admits.

The plain text names a picture where it stood, in brackets: `[shot.png]`, from the `alt`
the file's name gave it.

Tables
------

*Table* in the formatting bar offers rows, columns and a header row, up to 50 by 20, and
puts the table where the cursor is with a line after it to write on. With the cursor in a
cell, the same button offers the operations instead: a row above or below, a column left or
right, delete the row, the column or the table. A column added takes a header cell in a
header row and an ordinary cell elsewhere.

`Tab` moves to the next cell and, in the last cell, adds a row, as a word processor does;
`Shift`+`Tab` goes back, and from a row's first cell to the last cell of the row above.
These are the editor's own keys, not the page's shortcut map: the `?` list is the keys of
the message list and an open message.

A key typed in the formatted editor is the message's, not the page's. The page's
shortcuts - `c`, `e`, `/` and the rest - recognise a text field by its tag, and this
editor is a `div`; so a plain key stops at the editor, while a key with a modifier
(`Ctrl`+`K`, the palette) and `Esc` go on as they always did. This was a defect in the
rich editor before tables were in it: the letter `c` typed in a formatted message reopened
the compose window.

In the plain part a table is its rows, one to a line, cells separated by tabs, on a line of
its own; an empty cell keeps its place as an empty field between two tabs. A break inside a
cell - a reader pressing `Enter` in one - is read as a space and not as a new line, because
a cell is one field of one row and a newline in the middle of it would leave the rest of
the row on a line with no tabs lined up against anything.

From formatting back to plain
-----------------------------

*Plain text* asks first while the message holds a picture or a table - *Plain text keeps
the words and drops the images and tables. Continue?* - because it does: the words of a
table stay as tab-separated lines and a picture becomes its name in brackets, and neither
comes back.

What it does not do
-------------------

* **No merged cells.** `colspan` and `rowspan` are dropped by the rebuild, so a table
  pasted in with merged cells arrives with a cell where the merge was and a short row.
  Nothing in the page makes one.
* **No column widths, cell colours or borders of the reader's choosing.** Every table gets
  the same border, padding and spacing. A table pasted in loses the ones it had.
* **No resizing a picture after it is in**, by a handle or otherwise, and no way to edit
  its `alt` text; the file's name is the `alt`.
* **An animated GIF loses its animation if it has to be made smaller**, because it is
  redrawn on a canvas as a still PNG. One already within 1,200 pixels is kept whole and
  still animates.
* **No picture from a web address.** A remote `<img>` pasted in is dropped by the rebuild
  rather than fetched: a message that loads an image from someone else's server is the
  tracking pixel this project refuses to send as much as to accept.
* **The emoji names are English**, and there are no skin tones and no flags.
* **No undo of a table operation** beyond what the browser's own undo happens to hold; a
  deleted row is gone.

Where it is in the code
-----------------------

`hmailserver/source/Server/Common/Util/Portal.js`, after the rich-text section: the
rebuild (`cleanNode`, `plainOf`), *Where the reader is in the editor*, *Emoji*, *Images in
the message*, *Tables* and *A draft's formatting, brought back*; the toolbar, the two
panels and their styles are in `Portal.html`. `build/portal-script-test.js` proves them
against a stub DOM: the picker and its search, what is remembered, a picture pasted,
dropped and chosen, the type read from the bytes, the resizing, each bound and its
sentence, what the rebuild sends for a table and for a table pasted with styles and a
span, the operations, `Tab`, a draft reopened with its pictures back, a message that is not a
draft reopening as its text, a crafted `data:` source dropped whole, the sixty-fifth
picture refused, a break in a cell kept in one row of the plain part, a refusal named
when the files after it go in, and a forward whose attachments and pictures are over the
twelve megabytes together.
