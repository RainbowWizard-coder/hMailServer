# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The NixOS module's edit of hMailServer.ini (nix/module.nix): the keys of
# [Directories] and [Database] its options name are set to the values it gives in
# the environment (HM_<KEY>, the key in capitals), and every other line is kept as
# it is - the comments, [Settings], and [Security], where the server and its
# administrator write the administrator's password hash, the administrator's TOTP
# secret and the stored-secrets key, which the database's sealed values cannot be
# opened without. A key of the list below whose variable the environment does not
# have is kept as it is too: the module leaves Password and Passwordencryption out
# when no passwordFile names a password, so that one typed into the file is not
# blanked at the next start. A key the file lacks is added at the end of its section, and a
# section the file lacks at the end of the file. Keys and sections are matched
# without regard to case, as the server reads them; values are taken whole from the
# environment, never through a shell or a regular expression.
#
#   awk -f ini-merge.awk hMailServer.ini > new.ini

function trim(text) {
   sub(/^[ \t\r]+/, "", text)
   sub(/[ \t\r]+$/, "", text)
   return text
}

# The keys of section sec not yet written, written.
function flush(sec,   key) {
   for (key in section)
      if (section[key] == sec && !(key in done)) {
         print key "=" ENVIRON["HM_" toupper(key)]
         done[key] = 1
      }
}

BEGIN {
   count = split("directories:ProgramFolder directories:DataFolder directories:LogFolder " \
                 "directories:TempFolder directories:EventFolder directories:DatabaseFolder " \
                 "database:Type database:Server database:Port database:Database " \
                 "database:Username database:Password database:Passwordencryption", specs, " ")
   for (i = 1; i <= count; i++) {
      split(specs[i], part, ":")
      if (("HM_" toupper(part[2])) in ENVIRON)
         section[part[2]] = part[1]
   }
   current = ""
}

/^[ \t]*\[/ {
   flush(current)
   header = trim($0)
   close_at = index(header, "]")
   current = tolower(trim(substr(header, 2, close_at > 0 ? close_at - 2 : length(header) - 1)))
   print
   next
}

{
   equals = index($0, "=")
   if (equals > 1 && substr(trim($0), 1, 1) != ";") {
      name = tolower(trim(substr($0, 1, equals - 1)))
      for (key in section) {
         if (tolower(key) == name && section[key] == current) {
            if (!(key in done)) {
               print key "=" ENVIRON["HM_" toupper(key)]
               done[key] = 1
            }
            next
         }
      }
   }
   print
}

END {
   flush(current)
   split("directories database", order, " ")
   for (i = 1; i <= 2; i++) {
      pending = 0
      for (key in section)
         if (section[key] == order[i] && !(key in done))
            pending = 1
      if (pending) {
         print ""
         print (order[i] == "directories" ? "[Directories]" : "[Database]")
         flush(order[i])
      }
   }
}
