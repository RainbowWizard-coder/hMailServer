# OpenPGP fixtures

Everything here was made by `build/make-openpgp-fixtures.sh` with GnuPG 2.4 and
is read by `build/check-portal-openpgp.js`, which is how the webmail's OpenPGP
module is checked against an implementation that is not this one.

**The secret keys here are test keys and their passphrase is
`openpgp-fixture-pass`** (the symmetric message's is
`openpgp-fixture-symmetric`). They were made for this check, their addresses
are in the `.test` top-level domain, which can never be delivered to, and
nothing anywhere trusts them. They are committed so that the style workflow
needs no GnuPG; where the check finds `gpg` on the PATH it also runs the other
direction - what the module made, handed to GnuPG - which is the half that
cannot be proved from a file.

Never copy a key from here into anything real.
