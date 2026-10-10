// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.AccessControl;
using System.Security.Principal;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Whether anybody but SYSTEM, the Administrators and TrustedInstaller can change
   /// what an administrator's install of an update runs elevated - checked BEFORE
   /// Windows is asked for the elevation, because a replaced hMailServer.exe would
   /// replace its own check. hMailServer.exe /InstallUpdate applies the same rule
   /// again once elevated (Common/Util/AdminUpdateChecks.h); the two are held to one
   /// table of cases, build/testdata/install-permissions-cases.json.
   ///
   /// The rule:
   ///   - every folder from the volume's root down to {app}'s parent: nobody
   ///     untrusted may delete a child, delete the folder, change its permissions or
   ///     take it over, and its owner is trusted - what would let the next folder
   ///     down be replaced. Authenticated Users' "create folders" on C:\ passes.
   ///     A link or junction on the way is refused.
   ///   - {app}, Bin, ControlPanel and AdminUpdate, every folder below Bin and
   ///     ControlPanel, every *.exe and *.dll in them, Bin\hMailServer.INI and the
   ///     Sigstore trust files the INI names: the owner is trusted, and nobody
   ///     untrusted has any right that writes, deletes, or changes permissions or
   ///     the owner. Bin\hMailServerApiKeys.ini is not code, nothing elevated reads
   ///     it, and the service may write it, so it is not checked.
   ///   - Deny entries are ignored, which can only make the rule stricter; an entry
   ///     that applies only to children is judged on the children; CREATOR OWNER in
   ///     an entry that applies to the object itself is untrusted.
   /// </summary>
   public static class InstallationPermissions
   {
      public enum Place
      {
         /// <summary>A folder on the way to {app}.</summary>
         AboveApp,
         /// <summary>{app} and what is checked in it, and a trust file.</summary>
         Inside
      }

      public sealed class Finding
      {
         public bool Failed { get; set; }
         public string Object { get; set; } = "";
         public string Principal { get; set; } = "";
         public string Sid { get; set; } = "";
         public string Right { get; set; } = "";
         public bool Owner { get; set; }
         public bool Inherited { get; set; }
         public string InheritedFrom { get; set; } = "";
         public string Fix { get; set; } = "";
         public string Reason { get; set; } = "";
      }

      public const string TrustedInstallerSid = "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

      // The file rights, and the generic ones mapped onto them as the file system maps them.
      private const int WriteData = 0x2, AppendData = 0x4, WriteEa = 0x10, DeleteChild = 0x40, WriteAttributes = 0x100;
      private const int Delete = 0x10000, WriteDac = 0x40000, WriteOwner = 0x80000;
      private const int GenericAll = 0x10000000, GenericWrite = 0x40000000;
      private const int FileAllAccess = 0x1F01FF, FileGenericWrite = 0x120116;

      private const int WriteRights = WriteData | AppendData | WriteEa | WriteAttributes | DeleteChild | Delete | WriteDac | WriteOwner;
      private const int ReplaceRights = DeleteChild | Delete | WriteDac | WriteOwner;

      public static bool IsTrustedSid(SecurityIdentifier sid)
      {
         if (sid == null)
            return false;
         return sid.IsWellKnown(WellKnownSidType.LocalSystemSid) || sid.IsWellKnown(WellKnownSidType.BuiltinAdministratorsSid) ||
                string.Equals(sid.Value, TrustedInstallerSid, StringComparison.OrdinalIgnoreCase);
      }

      private static int Mapped_(int mask)
      {
         if ((mask & GenericAll) != 0)
            mask |= FileAllAccess;
         if ((mask & GenericWrite) != 0)
            mask |= FileGenericWrite;
         return mask;
      }

      private static string RightName_(int mask)
      {
         if ((mask & WriteOwner) != 0) return "take ownership"; // no-loc: Windows' own names for the rights, as icacls prints them
         if ((mask & WriteDac) != 0) return "change permissions"; // no-loc
         if ((mask & Delete) != 0) return "delete"; // no-loc
         if ((mask & DeleteChild) != 0) return "delete subfolders and files"; // no-loc
         if ((mask & WriteData) != 0) return "write data / create files"; // no-loc
         if ((mask & AppendData) != 0) return "append data / create folders"; // no-loc
         if ((mask & WriteEa) != 0) return "write extended attributes"; // no-loc
         if ((mask & WriteAttributes) != 0) return "write attributes"; // no-loc
         return "write"; // no-loc
      }

      public static string NameOf(SecurityIdentifier sid)
      {
         try
         {
            return sid.Translate(typeof(NTAccount)).Value;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return sid.Value;
         }
      }

      /// <summary>The rule on one descriptor, with no file access.</summary>
      public static Finding Evaluate(RawSecurityDescriptor descriptor, Place place)
      {
         var finding = new Finding();
         SecurityIdentifier owner = descriptor.Owner;
         if (owner == null)
         {
            finding.Failed = true;
            finding.Reason = "its owner could not be read"; // no-loc: joined into a translated sentence as data
            return finding;
         }
         if (!IsTrustedSid(owner))
         {
            finding.Failed = true;
            finding.Owner = true;
            finding.Sid = owner.Value;
            finding.Principal = NameOf(owner);
            finding.Right = "owner (may change the permissions)"; // no-loc
            return finding;
         }

         RawAcl dacl = descriptor.DiscretionaryAcl;
         if ((descriptor.ControlFlags & ControlFlags.DiscretionaryAclPresent) == 0 || dacl == null)
         {
            finding.Failed = true;
            finding.Sid = "S-1-1-0";
            finding.Principal = "Everyone"; // no-loc: the account's name
            finding.Right = "full control (the object has no permissions list at all)"; // no-loc
            return finding;
         }

         int counted = place == Place.AboveApp ? ReplaceRights : WriteRights;
         foreach (GenericAce ace in dacl)
         {
            if ((ace.AceFlags & AceFlags.InheritOnly) != 0)
               continue;
            bool allow = ace.AceType == AceType.AccessAllowed || ace.AceType == AceType.AccessAllowedCallback;
            bool objectType = ace.AceType == AceType.AccessAllowedObject || ace.AceType == AceType.AccessAllowedCallbackObject;
            if (!allow && !objectType)
               continue;

            SecurityIdentifier sid = (ace as KnownAce)?.SecurityIdentifier;
            int mask = objectType ? FileAllAccess : Mapped_((ace as KnownAce)?.AccessMask ?? 0);
            int offending = mask & counted;
            if (offending == 0 || (!objectType && IsTrustedSid(sid)))
               continue;

            finding.Failed = true;
            finding.Sid = sid?.Value ?? "(an entry of another kind)"; // no-loc
            finding.Principal = sid != null ? NameOf(sid) : finding.Sid;
            finding.Right = RightName_(offending);
            finding.Inherited = (ace.AceFlags & AceFlags.Inherited) != 0;
            return finding;
         }
         return finding;
      }

      /// <summary>The descriptor of the object at path: owner and DACL.</summary>
      public static RawSecurityDescriptor Read(string path)
      {
         FileSystemSecurity security = Directory.Exists(path)
            ? new DirectoryInfo(path).GetAccessControl(AccessControlSections.Owner | AccessControlSections.Access)
            : new FileInfo(path).GetAccessControl(AccessControlSections.Owner | AccessControlSections.Access);
         return new RawSecurityDescriptor(security.GetSecurityDescriptorBinaryForm(), 0);
      }

      private static string Quoted_(string path)
      {
         // icacls takes "C:\" unquoted: a backslash before the closing quote would escape it.
         return path.Length == 3 && path[1] == ':' && path[2] == '\\' ? path : "\"" + path + "\"";
      }

      /// <summary>One object: read, evaluated, and on failure the icacls line(s) that remove it.</summary>
      public static Finding CheckObject(string path, Place place, string top)
      {
         Finding finding;
         try
         {
            finding = Evaluate(Read(path), place);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return new Finding { Failed = true, Object = path, Reason = ex.Message };
         }
         if (!finding.Failed)
            return finding;

         finding.Object = path;
         if (!string.IsNullOrEmpty(finding.Reason))
            return finding;
         if (finding.Owner)
         {
            finding.Fix = "icacls " + Quoted_(path) + " /setowner *S-1-5-32-544"; // no-loc: a command
            return finding;
         }
         if (!finding.Inherited)
         {
            finding.Fix = "icacls " + Quoted_(path) + " /remove:g *" + finding.Sid; // no-loc: a command
            return finding;
         }

         finding.InheritedFrom = SourceOf_(path, finding.Sid);
         string target = path;
         if (place == Place.Inside && !string.IsNullOrEmpty(top))
         {
            for (string folder = Path.GetDirectoryName(path); !string.IsNullOrEmpty(folder) && folder.Length >= top.Length; folder = Path.GetDirectoryName(folder))
            {
               if (!HasEntry_(folder, finding.Sid, out bool inherited) || !inherited)
                  break;
               target = folder;
               if (string.Equals(folder.TrimEnd('\\'), top.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                  break;
            }
         }
         if (place == Place.Inside)
            finding.Fix = "icacls " + Quoted_(target) + " /inheritance:d && icacls " + Quoted_(target) + " /remove:g *" + finding.Sid; // no-loc: commands
         else if (!string.IsNullOrEmpty(finding.InheritedFrom))
            finding.Fix = "icacls " + Quoted_(finding.InheritedFrom) + " /remove:g *" + finding.Sid; // no-loc: a command
         else
            finding.Fix = "icacls " + Quoted_(path) + " /inheritance:d && icacls " + Quoted_(path) + " /remove:g *" + finding.Sid; // no-loc: commands
         return finding;
      }

      private static bool HasEntry_(string path, string sid, out bool inherited)
      {
         inherited = false;
         try
         {
            foreach (GenericAce ace in Read(path).DiscretionaryAcl)
            {
               if ((ace.AceFlags & AceFlags.InheritOnly) != 0 || ace is not KnownAce known || known.SecurityIdentifier.Value != sid)
                  continue;
               if (ace.AceType != AceType.AccessAllowed && ace.AceType != AceType.AccessAllowedCallback)
                  continue;
               inherited = (ace.AceFlags & AceFlags.Inherited) != 0;
               return true;
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
         }
         return false;
      }

      private static string SourceOf_(string path, string sid)
      {
         for (string folder = Path.GetDirectoryName(path); !string.IsNullOrEmpty(folder); folder = Path.GetDirectoryName(folder))
         {
            try
            {
               foreach (GenericAce ace in Read(folder).DiscretionaryAcl)
               {
                  if (ace is KnownAce known && known.SecurityIdentifier.Value == sid && (ace.AceFlags & AceFlags.Inherited) == 0 &&
                      (ace.AceType == AceType.AccessAllowed || ace.AceType == AceType.AccessAllowedCallback))
                     return folder;
               }
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               return "";
            }
         }
         return "";
      }

      private static bool IsLink_(string path) => (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;

      private static Finding Link_(string path) => new Finding
      {
         Failed = true,
         Object = path,
         Reason = "it is a link or a junction, so what is in it is not where its path says" // no-loc: joined into a translated sentence as data
      };

      private static Finding CheckAncestors_(string path)
      {
         var chain = new List<string>();
         for (string folder = Path.GetDirectoryName(path); !string.IsNullOrEmpty(folder); folder = Path.GetDirectoryName(folder))
            chain.Add(folder);
         chain.Reverse();
         foreach (string folder in chain)
         {
            if (folder.Length > 3 && IsLink_(folder))
               return Link_(folder);
            Finding finding = CheckObject(folder, Place.AboveApp, null);
            if (finding.Failed)
               return finding;
         }
         return new Finding();
      }

      private static Finding CheckTree_(string folder, string app, bool recurse)
      {
         if (!Directory.Exists(folder))
            return new Finding();
         if (IsLink_(folder))
            return Link_(folder);
         Finding finding = CheckObject(folder, Place.Inside, app);
         if (finding.Failed || !recurse)
            return finding;

         foreach (string file in Directory.EnumerateFiles(folder))
         {
            if (IsLink_(file))
               return Link_(file);
            string extension = Path.GetExtension(file);
            if (!extension.Equals(".exe", StringComparison.OrdinalIgnoreCase) && !extension.Equals(".dll", StringComparison.OrdinalIgnoreCase))
               continue;
            finding = CheckObject(file, Place.Inside, app);
            if (finding.Failed)
               return finding;
         }
         foreach (string child in Directory.EnumerateDirectories(folder))
         {
            finding = CheckTree_(child, app, true);
            if (finding.Failed)
               return finding;
         }
         return finding;
      }

      /// <summary>
      /// The whole rule for an installation, stopping at the first failure. app is
      /// the folder above Bin; extraFiles the trust files hMailServer.INI names.
      /// </summary>
      public static Finding CheckInstallation(string app, IEnumerable<string> extraFiles)
      {
         app = app.TrimEnd('\\');
         Finding finding = CheckAncestors_(app);
         if (finding.Failed)
            return finding;
         finding = CheckTree_(app, app, false);
         if (finding.Failed)
            return finding;

         string bin = Path.Combine(app, "Bin");
         if (!Directory.Exists(bin))
            return new Finding { Failed = true, Object = bin, Reason = "it is not there" }; // no-loc: joined into a translated sentence as data
         finding = CheckTree_(bin, app, true);
         if (finding.Failed)
            return finding;
         string ini = Path.Combine(bin, "hMailServer.ini");
         if (File.Exists(ini))
         {
            finding = CheckObject(ini, Place.Inside, app);
            if (finding.Failed)
               return finding;
         }
         finding = CheckTree_(Path.Combine(app, "ControlPanel"), app, true);
         if (finding.Failed)
            return finding;
         finding = CheckTree_(Path.Combine(app, "AdminUpdate"), app, false);
         if (finding.Failed)
            return finding;

         foreach (string file in (extraFiles ?? Enumerable.Empty<string>()).Where(f => !string.IsNullOrWhiteSpace(f)))
         {
            finding = CheckAncestors_(file);
            if (finding.Failed)
               return finding;
            // The file itself a link: its own permissions say nothing of the file
            // read through it.
            if (File.Exists(file) && IsLink_(file))
               return new Finding { Failed = true, Object = file, Reason = "it is a link, so what is read through it is not where its path says" }; // no-loc: joined into a translated sentence as data
            finding = CheckObject(file, Place.Inside, null);
            if (finding.Failed)
               return finding;
         }
         return new Finding();
      }
   }
}
