# The address book

What the webmail's Contacts page holds, what a phone sees of it, and what is collected on its own.

## Kept, and other contacts

A contact is a name and one address, in `hm_contacts`, with a `source`: **manual** for what the person added, imported, or kept, and **collected** for every address the person has written to - from the webmail's own send and, since 17 September 2026, from a mail client over an authenticated SMTP submission, under the account that signed in. A collected address is offered on the Contacts page under **Other contacts** and by the To, Cc and Bcc completion, and nowhere else: CardDAV serves only what the person kept, so a phone's address book is not filled with everyone ever written to. **Keep** (`PUT /api/v1/me/contacts/{id}` with `source: manual`) makes a collected address a contact, from then on served to every phone; a phone creating a card for an address the book only collected makes that row the contact, under the phone's name, rather than being refused. `GET /api/v1/me/contacts?source=manual|collected` lists either half.

## What a card carries

A card a CardDAV client sends is kept as sent (`contactvcard`, schema 6040) and served back byte for byte. The page reads from it what the listing answers beside the name and address: `birthday` (`BDAY`, as YYYY-MM-DD, or --MM-DD when the card has no year), `note`, `phones` (every `TEL`) and `has_photo`. The photo itself is `GET /api/v1/me/contacts/{id}/photo`, the embedded image as it is (a base64 `PHOTO` of vCard 3 or a `data:` URI of vCard 4; a photo that is only a URL is not the server's to fetch), which the page uses as the avatar; the birthday is marked on the day.

## Export and import

`GET /api/v1/me/contacts?fields=card` puts the stored card on each contact, and the page's vCard export sends that card whole - photo, phones, note and all - making a card only for a row that has none. A vCard file the page imports is sent as `card` on `POST /api/v1/me/contacts` and kept as sent, so a card exported from a phone and brought in here loses nothing; the row's name and address come from the card's FN and preferred EMAIL unless given beside it. The CSV export and import stay name and address.

## What is tested

`API/RestApiSelfService.cs`: the SMTP submission collecting, the two halves of the listing, keeping and the one direction there is. `API/CardDav.cs`: a collected address served only once kept, the phone's PUT taking a collected row, and the card's photo, birthday, note and phones read, the photo served, the card exported whole and an imported card kept as sent. The webmail's harness draws both tables, the photo as the avatar, the details, Keep, and the export with the stored card.
