// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.CodeDom.Compiler;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using Microsoft.Win32;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The verify: the second part of the roadmap's live update. The server fetches
   ///    the newer release's installer and its Sigstore bundle, and keeps the installer
   ///    only when the bundle proves it is the one this repository's release workflow
   ///    signed. The authority, the log and the workflow identity are the suite's own
   ///    (FakeSigstore), pointed at through UpdateTrustRootsFile and
   ///    UpdateLogPublicKeyFile; the identity strings are the real ones, so the server's
   ///    defaults for them are what is exercised.
   ///
   ///    Every refusal here is a file the server must not keep: not a file that was
   ///    tampered with, not one signed by another workflow, another authority or an
   ///    expired certificate, not one whose log record is forged, not one that is
   ///    another release than the one offered. Nothing is run.
   ///
   ///    The release offered is always newer than the running version, so after 6.3.3,
   ///    and it is signed as the project signs such a release: by the GitLab CI
   ///    pipeline for its own tag, at the group's path from 6.3.6
   ///    (FakeSigstore.ReleaseSigning). The installer is a
   ///    small Windows program carrying the version resource a real one does.
   /// </summary>
   [TestFixture]
   public class UpdateVerify : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9104;
      private const string AdminPassword = "testar";
      private const string LatestPath = "/repos/hmailserver/hmailserver/releases/latest";

      private FakeHttpEndpoint _feed;
      private FakeSigstore _sigstore;
      private byte[] _installer;
      private string _installerName;
      private string _updatesDirectory;

      [SetUp]
      public void StartFeed()
      {
         // Every test here cleans the Updates folder, which only SYSTEM, the
         // Administrators and the service may change.
         RequireElevation("the Updates folder admits changes from SYSTEM, the Administrators and the service alone, and these tests clean it.");

         _sigstore = new FakeSigstore();
         _installer = FakeInstaller(Newer);
         _installerName = "hMailServer-" + Newer + "-x64.exe";
         _updatesDirectory = Paths.Combine(_settings.Directories.DataDirectory, "Updates");
         CleanUpdates();

         _feed = new FakeHttpEndpoint(200, "{}");
         ServeRelease(_installer, Released(_installer));

         _settings.SetAdministratorPassword(AdminPassword);
         WriteSetting("UpdateCheckEnabled", "0");
         WriteSetting("UpdateChannel", "stable");
         WriteSetting("UpdateFeedUrl", _feed.UrlFor(LatestPath));
         WriteSetting("UpdateTrustRootsFile", _sigstore.WriteTrustRootsFile());
         WriteSetting("UpdateLogPublicKeyFile", _sigstore.WriteLogKeyFile());
         WriteSetting("UpdateRequireAuthenticode", "0");
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
      }

      [TearDown]
      public void StopFeed()
      {
         // An unelevated run set nothing up (RequireElevation).
         if (!Elevated)
            return;

         WriteSetting("UpdateCheckEnabled", "0");
         WriteSetting("UpdateFeedUrl", "");
         WriteSetting("UpdateTrustRootsFile", "");
         WriteSetting("UpdateLogPublicKeyFile", "");
         WriteSetting("UpdateRequireAuthenticode", "0");
         WriteSetting("UpdateSigningIdentity", "");
         WriteSetting("UpdateSigningIssuer", "");
         WriteSetting("UpdateSourceRepository", "");
         RestListener.Stop();
         _application.Reinitialize();
         _feed.Dispose();
         _sigstore.Dispose();
         CleanUpdates();
      }

      [Test]
      public void AVerifiedInstallerIsDownloadedAndKept()
      {
         var status = Checked();

         Assert.IsTrue(status.DownloadUpdate(), "The installer and its bundle were served and the bundle is genuine. Error: " + status.UpdateLastError);

         Assert.AreEqual(3, status.UpdateState, "State 3 is 'downloaded and verified'.");
         string path = status.UpdateInstallerPath;
         Assert.AreEqual(Paths.Combine(_updatesDirectory, _installerName), path);
         Assert.IsTrue(File.Exists(path), "The verified installer is in the data directory's Updates folder.");
         Assert.AreEqual(_installer, File.ReadAllBytes(path), "Byte for byte what was served.");
         Assert.IsTrue(File.Exists(path + ".cosign.bundle"), "The bundle is kept beside it.");
         Assert.IsFalse(File.Exists(path + ".partial"), "Nothing partial is left.");
         Assert.AreEqual(NewerIdentity, status.UpdateSignerIdentity);
         Assert.IsEmpty(status.UpdateLastError);

         // The installer's URL redirected, as a release asset's does, and the
         // redirect was followed on the loopback.
         StringAssert.Contains("GET /download/" + _installerName + " HTTP/1.0", string.Join("\n", _feed.Requests));
         StringAssert.Contains("GET /objects/" + _installerName + " HTTP/1.0", string.Join("\n", _feed.Requests));

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("Update: hMailServer " + Newer + " downloaded to " + path + " and verified: signed by " + NewerIdentity, log);

         (int code, string body) = Http("GET", "/api/v1/update");
         Assert.AreEqual(200, code, body);
         StringAssert.Contains("\"state\":3", body);
         StringAssert.Contains("\"stateName\":\"downloaded\"", body);
         StringAssert.Contains("\"downloaded\":{\"path\":\"" + path.Replace("\\", "\\\\") + "\",\"signer\":\"" + NewerIdentity + "\",\"logTime\":\"", body);
      }

      [Test]
      public void ARecheckKeepsTheVerifiedDownload()
      {
         var status = Checked();
         Assert.IsTrue(status.DownloadUpdate(), status.UpdateLastError);
         string path = status.UpdateInstallerPath;

         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.AreEqual(3, status.UpdateState, "The same release, still current, still verified on disk: nothing to do again.");
         Assert.AreEqual(path, status.UpdateInstallerPath);

         // A different newer release starts over.
         string another = FakeSigstore_Bump(Newer);
         ServeRelease(_installer, Released(_installer), another);
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.AreEqual(2, status.UpdateState);
         Assert.IsEmpty(status.UpdateInstallerPath);
      }

      [Test]
      public void ATamperedInstallerIsDeletedAndRefused()
      {
         // The bundle is genuine for the release's bytes; what is served is not them.
         // The feed's digest is made to agree with what is served, so the bundle is
         // what catches it.
         byte[] served = (byte[]) _installer.Clone();
         served[1000] ^= 0xFF;
         ServeRelease(served, Released(_installer));

         var status = Checked();
         Assert.IsFalse(status.DownloadUpdate(), "A file that is not the one signed must be refused.");
         Assert.AreEqual(5, status.UpdateState);
         StringAssert.Contains("this is not the file that was signed", status.UpdateLastError);
         AssertNothingKept();
         StringAssert.Contains("Update download failed for hMailServer " + Newer, LogHandler.ReadCurrentDefaultLog());
      }

      [Test]
      public void TheFeedsDigestMustAgreeWithTheDownload()
      {
         ServeRelease(_installer, Released(_installer), digest: "sha256:" + new string('0', 64));

         var status = Checked();
         Assert.IsFalse(status.DownloadUpdate());
         StringAssert.Contains("The feed says the installer's SHA-256 is", status.UpdateLastError);
         AssertNothingKept();
      }

      [Test]
      public void ABundleFromAnotherWorkflowIsRefused()
      {
         ServeRelease(_installer, _sigstore.Bundle(_installer, new FakeSigstore.Options
         {
            Identity = "https://github.com/someone-else/hmailserver/.github/workflows/sign-release.yml@refs/heads/master"
         }));

         var status = Checked();
         Assert.IsFalse(status.DownloadUpdate());
         StringAssert.Contains("not this repository's release workflow", status.UpdateLastError);
         AssertNothingKept();
      }

      [Test]
      public void ABundleFromAnotherIssuerOrRepositoryIsRefused()
      {
         ServeRelease(_installer, _sigstore.Bundle(_installer, new FakeSigstore.Options {Issuer = "https://accounts.google.com"}));
         var status = Checked();
         Assert.IsFalse(status.DownloadUpdate());
         StringAssert.Contains("issued on a token from https://accounts.google.com", status.UpdateLastError);
         AssertNothingKept();

         ServeRelease(_installer, _sigstore.Bundle(_installer, new FakeSigstore.Options {Repository = "https://github.com/someone-else/hmailserver"}));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsFalse(status.DownloadUpdate());
         StringAssert.Contains("is for https://github.com/someone-else/hmailserver, not https://github.com/Progressiverobot/hmailserver", status.UpdateLastError);
         AssertNothingKept();
      }

      [Test]
      public void ABundleFromThisProjectsGitLabPipelineForAReleaseTagIsAccepted()
      {
         // The pipeline at the path the project began at, which signed 6.3.5: the
         // server's default trust is a list, and this is its second entry, kept so
         // that a server goes on taking that release.
         string identity = FakeSigstore.GitLabReleaseIdentityPrefix + Newer;
         ServeRelease(_installer, _sigstore.Bundle(_installer, GitLabSigned(identity)));

         var status = Checked();
         Assert.IsTrue(status.DownloadUpdate(), "A release signed by this project's GitLab CI pipeline for its own v* tag is one the server trusts. Error: " + status.UpdateLastError);
         Assert.AreEqual(3, status.UpdateState);
         Assert.AreEqual(identity, status.UpdateSignerIdentity, "The verdict names the GitLab job's identity.");
         StringAssert.Contains("Update: hMailServer " + Newer + " downloaded to " + status.UpdateInstallerPath + " and verified: signed by " + identity,
            LogHandler.ReadCurrentDefaultLog());

         // A certificate at that path that records no project id is not refused for
         // that: the id is checked there when the certificate carries one, as this
         // entry was written before any GitLab-issued certificate had been seen.
         CleanUpdates();
         FakeSigstore.Options withoutId = GitLabSigned(identity);
         withoutId.RepositoryId = null;
         ServeRelease(_installer, _sigstore.Bundle(_installer, withoutId));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsTrue(status.DownloadUpdate(), "The path, the issuer and the repository are this project's. Error: " + status.UpdateLastError);

         // The GitHub workflow's bundles, which every release up to 6.3.3 carries,
         // are still trusted - for those releases, which UpdateApply's rollback image
         // is - and not for this one (AGitHubBundleOfferedAsALaterReleaseIsRefused).
      }

      [Test]
      public void ABundleFromTheGroupTheProjectMovedToIsAcceptedOnlyWithTheProjectsId()
      {
         // The project moved from the personal namespace to the group hmailserver on
         // 29 September 2026. A transfer keeps the project's id and changes its
         // path, so the pipeline's certificates name the group's path from 6.3.6,
         // and the server trusts it - the third entry of its default trust - with
         // the id required.
         string identity = FakeSigstore.GitLabGroupReleaseIdentityPrefix + Newer;
         ServeRelease(_installer, _sigstore.Bundle(_installer, GitLabGroupSigned(identity)));

         var status = Checked();
         Assert.IsTrue(status.DownloadUpdate(), "A release signed by this project's pipeline after its move to the group, recording the project's id. Error: " + status.UpdateLastError);
         Assert.AreEqual(3, status.UpdateState);
         Assert.AreEqual(identity, status.UpdateSignerIdentity, "The verdict names the group's identity.");
         StringAssert.Contains("Update: hMailServer " + Newer + " downloaded to " + status.UpdateInstallerPath + " and verified: signed by " + identity,
            LogHandler.ReadCurrentDefaultLog());
         CleanUpdates();

         // Whoever holds the path, were the group lost or the project moved on,
         // could sign for a v* tag of their own: without the project's id, or with
         // another project's, the certificate is nobody's.
         AssertRefused(status, GitLabGroupSigned(identity, repositoryId: null),
            "The certificate names no source repository identifier.");
         AssertRefused(status, GitLabGroupSigned(identity, repositoryId: "12345678"),
            "is for the repository with the id 12345678, not " + FakeSigstore.GitLabRepositoryId);
         // An id recorded empty is another id, not a match.
         AssertRefused(status, GitLabGroupSigned(identity, repositoryId: ""),
            "is for the repository with the id , not " + FakeSigstore.GitLabRepositoryId);

         // The first path still takes a certificate that records no id: what is
         // required is the group entry's, not every GitLab one's.
         FakeSigstore.Options firstWithoutId = GitLabSigned(FakeSigstore.GitLabReleaseIdentityPrefix + Newer);
         firstWithoutId.RepositoryId = null;
         ServeRelease(_installer, _sigstore.Bundle(_installer, firstWithoutId));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsTrue(status.DownloadUpdate(), "The first path checks the id only when the certificate carries one. Error: " + status.UpdateLastError);
         CleanUpdates();

         // The group's identity is one part of its signer, as the first path's is:
         // recording the first path's repository, on another issuer's token, or
         // for another tag than the release being installed, it is refused.
         AssertRefused(status, GitLabGroupSigned(identity, repository: FakeSigstore.GitLabRepository),
            "is for " + FakeSigstore.GitLabRepository + ", not " + FakeSigstore.GitLabGroupRepository);
         AssertRefused(status, GitLabSigned(FakeSigstore.GitLabReleaseIdentityPrefix + Newer, repository: FakeSigstore.GitLabGroupRepository),
            "is for " + FakeSigstore.GitLabGroupRepository + ", not " + FakeSigstore.GitLabRepository);
         AssertRefused(status, GitLabGroupSigned(identity, issuer: FakeSigstore.GitHubIssuer),
            "issued on a token from " + FakeSigstore.GitHubIssuer + ", not " + FakeSigstore.GitLabIssuer);
         string another = FakeSigstore_Bump(Newer);
         AssertRefused(status, GitLabGroupSigned(FakeSigstore.GitLabGroupReleaseIdentityPrefix + another),
            "The certificate was made for the tag v" + another + ", not for v" + Newer + ", the release being installed.");
      }

      [Test]
      public void AGitLabBundleFromAnotherProjectOrRefIsRefused()
      {
         var status = Checked();
         string tag = "@refs/tags/v" + Newer;

         // The identities GitLab would give any pipeline but this project's own for a
         // v* tag. Each has GitLab as its issuer and its own project as its
         // repository, as Fulcio would record them: the identity is what refuses it.
         var strangers = new[]
         {
            // Another project, for a tag of its own.
            new[] { "https://gitlab.com/someone-else/hmailserver//.gitlab-ci.yml" + tag, "https://gitlab.com/someone-else/hmailserver" },
            // A project whose path begins with this one's: a subgroup of the same name.
            new[] { "https://gitlab.com/Progressiverobot/hmailserver/fork//.gitlab-ci.yml" + tag, "https://gitlab.com/Progressiverobot/hmailserver/fork" },
            // This project, for a branch.
            new[] { "https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/heads/master", FakeSigstore.GitLabRepository },
            // This project, for a merge request's ref - a fork's code, run here.
            new[] { "https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/merge-requests/1/head", FakeSigstore.GitLabRepository },
            // This project, for a tag that is not a release's.
            new[] { "https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/build-inputs-1", FakeSigstore.GitLabRepository },
            // This project, for a v* tag, from another CI file.
            new[] { "https://gitlab.com/Progressiverobot/hmailserver//ci/other.yml" + tag, FakeSigstore.GitLabRepository },
            // The group the project moved to: a subgroup of the same name, a
            // project beside it whose path begins with its path, the same path in
            // other letters, and the project there for a branch, a merge request's
            // ref, a tag that is not a release's and another CI file.
            new[] { "https://gitlab.com/hmailserver/hmailserver/fork//.gitlab-ci.yml" + tag, "https://gitlab.com/hmailserver/hmailserver/fork" },
            new[] { "https://gitlab.com/hmailserver/hmailserver-evil//.gitlab-ci.yml" + tag, "https://gitlab.com/hmailserver/hmailserver-evil" },
            new[] { "https://gitlab.com/HMailServer/hmailserver//.gitlab-ci.yml" + tag, "https://gitlab.com/HMailServer/hmailserver" },
            new[] { "https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/heads/master", FakeSigstore.GitLabGroupRepository },
            new[] { "https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/merge-requests/1/head", FakeSigstore.GitLabGroupRepository },
            new[] { "https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/build-inputs-1", FakeSigstore.GitLabGroupRepository },
            new[] { "https://gitlab.com/hmailserver/hmailserver//ci/other.yml" + tag, FakeSigstore.GitLabGroupRepository }
         };
         foreach (string[] stranger in strangers)
         {
            AssertRefused(status, GitLabSigned(stranger[0], repository: stranger[1], repositoryId: "12345678"),
               "The certificate names " + stranger[0] + ", not this repository's release workflow");
         }

         // This project's tag identity is only one part of the signer. On a token
         // from another issuer, recording another project, recording the GitHub
         // repository - another signer's part - or another project's id, it is
         // nobody's.
         string identity = FakeSigstore.GitLabReleaseIdentityPrefix + Newer;
         AssertRefused(status, GitLabSigned(identity, issuer: FakeSigstore.GitHubIssuer),
            "issued on a token from " + FakeSigstore.GitHubIssuer + ", not " + FakeSigstore.GitLabIssuer);
         AssertRefused(status, GitLabSigned(identity, repository: "https://gitlab.com/someone-else/hmailserver"),
            "is for https://gitlab.com/someone-else/hmailserver, not " + FakeSigstore.GitLabRepository);
         AssertRefused(status, GitLabSigned(identity, repository: FakeSigstore.Repository),
            "is for " + FakeSigstore.Repository + ", not " + FakeSigstore.GitLabRepository);
         AssertRefused(status, GitLabSigned(identity, repositoryId: "12345678"),
            "is for the repository with the id 12345678, not " + FakeSigstore.GitLabRepositoryId);

         // And the GitHub workflow's identity on a GitLab token, or recording the
         // GitLab repository, is not the GitHub signer.
         AssertRefused(status, new FakeSigstore.Options { Issuer = FakeSigstore.GitLabIssuer },
            "issued on a token from " + FakeSigstore.GitLabIssuer + ", not " + FakeSigstore.GitHubIssuer);
         AssertRefused(status, new FakeSigstore.Options { Repository = FakeSigstore.GitLabRepository },
            "is for " + FakeSigstore.GitLabRepository + ", not " + FakeSigstore.Repository);
      }

      [Test]
      public void AGitHubCertificateIssuedFromTheSuspensionOnIsRefused()
      {
         var status = Checked();

         // Issued a quarter of an hour before the cut-off: the cut-off lets it
         // through, and what refuses it is only that it is offered as a release after
         // 6.3.3, the last the GitHub workflow signed.
         AssertRefused(status, new FakeSigstore.Options {SignedAt = FakeSigstore.GitHubSuspension.AddMinutes(-10)},
            "which signed no release after " + FakeSigstore.GitHubLastVersion + "; this file is offered as " + Newer + ".");

         // Issued at the cut-off to the second, and a week or more after it: whoever
         // holds the name then is not this project, which can sign nothing there.
         foreach (DateTimeOffset signedAt in new[] { FakeSigstore.GitHubSuspension.AddMinutes(5), DateTimeOffset.UtcNow })
         {
            AssertRefused(status, new FakeSigstore.Options {SignedAt = signedAt},
               "a signer this server trusts only for certificates issued before 2026-09-18T00:00:00Z, and this one was issued later");
         }
      }

      [Test]
      public void AGitHubCertificateMustRecordTheRepositorysId()
      {
         // Every re-hosted GitHub bundle records the repository's numeric id, which
         // GitHub never gives another repository.
         var status = Checked();
         AssertRefused(status, new FakeSigstore.Options {RepositoryId = "999999999"},
            "is for the repository with the id 999999999, not " + FakeSigstore.GitHubRepositoryId);
         AssertRefused(status, new FakeSigstore.Options {RepositoryId = null},
            "The certificate names no source repository identifier.");
      }

      [Test]
      public void ACertificateForAnotherReleaseTagIsRefused()
      {
         // The feed offers Newer. A file signed for another tag is that tag's
         // release, whatever the feed calls it.
         var status = Checked();
         string another = FakeSigstore_Bump(Newer);
         AssertRefused(status, GitLabSigned(FakeSigstore.GitLabReleaseIdentityPrefix + another),
            "The certificate was made for the tag v" + another + ", not for v" + Newer + ", the release being installed.");
      }

      [Test]
      public void AGitHubBundleOfferedAsALaterReleaseIsRefused()
      {
         // The genuine GitHub bundles were made by a workflow run from a branch, so
         // their identity names no release. Without a limit a feed could offer the
         // genuine 6.2.19 installer as a release after the running one, every check
         // would pass, and the server would install a release too old for its own
         // database. The GitHub workflow signed nothing after 6.3.3, so nothing it
         // signed is installed as a later release.
         var status = Checked();
         AssertRefused(status, new FakeSigstore.Options(),
            "The certificate names " + FakeSigstore.ReleaseIdentity + ", which signed no release after " + FakeSigstore.GitHubLastVersion +
            "; this file is offered as " + Newer + ".");

         // A run for a tag - 6.2.22-pre3 was signed that way - is held the same way,
         // its own tag or another.
         const string gitHubWorkflow = "https://github.com/Progressiverobot/hmailserver/.github/workflows/sign-release.yml@";
         AssertRefused(status, new FakeSigstore.Options {Identity = gitHubWorkflow + "refs/tags/v" + Newer},
            "which signed no release after " + FakeSigstore.GitHubLastVersion);
         AssertRefused(status, new FakeSigstore.Options {Identity = gitHubWorkflow + "refs/tags/v" + FakeSigstore_Bump(Newer)},
            "which signed no release after " + FakeSigstore.GitHubLastVersion);
      }

      [Test]
      public void AnInstallerThatSaysItIsAnotherReleaseIsRefused()
      {
         // Signed as the release the feed offers, but the program itself says it is
         // another version: its version resource is the signer's own statement of
         // what it installs, and the release it installs is the one it says.
         var status = Checked();
         string another = FakeSigstore_Bump(Newer);
         byte[] other = FakeInstaller(another);
         ServeRelease(other, Released(other));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsFalse(status.DownloadUpdate(), "A program that says it is " + another + " is not release " + Newer + ".");
         StringAssert.Contains("The installer is not the release it was offered as: The installer says it is version " + another + ".0, not " + Newer +
            ", the release it was offered as.", status.UpdateLastError);
         AssertNothingKept();

         // A file with no version resource says it is nothing, and is not installed as anything.
         byte[] bare = new byte[64 * 1024];
         new Random(4242).NextBytes(bare);
         ServeRelease(bare, Released(bare));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsFalse(status.DownloadUpdate(), "Random bytes are no installer.");
         StringAssert.Contains("The installer carries no version resource, so nothing in it says it is release " + Newer, status.UpdateLastError);
         AssertNothingKept();
      }

      [Test]
      public void TheThreeTrustSettingsReplaceTheirPartOfEverySigner()
      {
         // The values docs/UpdateFeed.md gives a server from 6.2.28 to 6.3.3 to pin
         // the GitLab signer at the path the project began at, still set after the
         // upgrade. They restate that signer's own values, which keeps every check
         // it has; the GitHub entry, turned by them into GitLab's identity without
         // GitLab's checks, is dropped rather than tried first, and so is the
         // group's, turned into the first path's without the id. A release signed
         // there, as 6.3.5 was, installs.
         WriteSetting("UpdateSigningIdentity", FakeSigstore.GitLabReleaseIdentityPrefix);
         WriteSetting("UpdateSigningIssuer", FakeSigstore.GitLabIssuer);
         WriteSetting("UpdateSourceRepository", FakeSigstore.GitLabRepository);
         _application.Reinitialize();
         ServeRelease(_installer, _sigstore.Bundle(_installer, GitLabSigned(FakeSigstore.GitLabReleaseIdentityPrefix + Newer)));

         var status = Checked();
         Assert.IsTrue(status.DownloadUpdate(), "The pinned GitLab signer. Error: " + status.UpdateLastError);
         CleanUpdates();
         AssertRefused(status, new FakeSigstore.Options(),
            "The certificate names " + FakeSigstore.ReleaseIdentity + ", not this repository's release workflow");
         AssertRefused(status, GitLabSigned(FakeSigstore.GitLabReleaseIdentityPrefix + FakeSigstore_Bump(Newer)),
            "The certificate was made for the tag v" + FakeSigstore_Bump(Newer) + ", not for v" + Newer);
         AssertRefused(status, GitLabSigned(FakeSigstore.GitLabReleaseIdentityPrefix + Newer, repositoryId: "12345678"),
            "is for the repository with the id 12345678, not " + FakeSigstore.GitLabRepositoryId);
         // So a server that keeps them set trusts the first path alone, and refuses
         // what the project signs since its move, from 6.3.6, until they are cleared.
         AssertRefused(status, GitLabGroupSigned(FakeSigstore.GitLabGroupReleaseIdentityPrefix + Newer),
            "The certificate names " + FakeSigstore.GitLabGroupReleaseIdentityPrefix + Newer + ", not this repository's release workflow");

         // Another issuer only: it replaces the GitHub entry's, which then takes no
         // GitHub certificate, and leaves GitLab's alone, release check and all.
         WriteSetting("UpdateSigningIdentity", "");
         WriteSetting("UpdateSourceRepository", "");
         _application.Reinitialize();
         status = Checked();
         AssertRefused(status, new FakeSigstore.Options {SignedAt = DateTimeOffset.UtcNow},
            "issued on a token from " + FakeSigstore.GitHubIssuer + ", not " + FakeSigstore.GitLabIssuer);
         AssertRefused(status, GitLabSigned(FakeSigstore.GitLabReleaseIdentityPrefix + FakeSigstore_Bump(Newer)),
            "The certificate was made for the tag v" + FakeSigstore_Bump(Newer));

         // Only the repository check off: every one of this project's signers
         // stays, none holding a certificate to its repository, the GitHub one
         // still holding it to the releases it signed, and a certificate for the
         // group's path still held to the project's id, which no setting drops:
         // it is all that tells the project from whoever holds that path.
         WriteSetting("UpdateSigningIssuer", "");
         WriteSetting("UpdateSourceRepository", "-");
         _application.Reinitialize();
         status = Checked();
         AssertRefused(status, new FakeSigstore.Options {Repository = "https://github.com/someone-else/hmailserver", RepositoryId = "999999999"},
            "which signed no release after " + FakeSigstore.GitHubLastVersion);
         // At the first path the id goes with the repository check.
         ServeRelease(_installer, _sigstore.Bundle(_installer, GitLabSigned(FakeSigstore.GitLabReleaseIdentityPrefix + Newer,
            repository: "https://gitlab.com/someone-else/hmailserver", repositoryId: "12345678")));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsTrue(status.DownloadUpdate(), "UpdateSourceRepository=- checks no repository. Error: " + status.UpdateLastError);
         CleanUpdates();
         string group = FakeSigstore.GitLabGroupReleaseIdentityPrefix + Newer;
         AssertRefused(status, GitLabGroupSigned(group, repository: "https://gitlab.com/someone-else/hmailserver", repositoryId: null),
            "The certificate names no source repository identifier.");
         AssertRefused(status, GitLabGroupSigned(group, repository: "https://gitlab.com/someone-else/hmailserver", repositoryId: "12345678"),
            "is for the repository with the id 12345678, not " + FakeSigstore.GitLabRepositoryId);
         ServeRelease(_installer, _sigstore.Bundle(_installer, GitLabGroupSigned(group, repository: "https://gitlab.com/someone-else/hmailserver")));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsTrue(status.DownloadUpdate(), "UpdateSourceRepository=- checks no repository for the group's path, and still its id. Error: " + status.UpdateLastError);
      }

      [Test]
      public void TheGroupsValuesPinnedKeepItsIdRequired()
      {
         // The three settings restating the group's entry - as a site might set
         // them since the project moved - keep every check it has, the required
         // id among them. The GitHub entry and the first path's, turned by them
         // into the group's identity, issuer and repository without the id, are
         // dropped rather than tried first.
         WriteSetting("UpdateSigningIdentity", FakeSigstore.GitLabGroupReleaseIdentityPrefix);
         WriteSetting("UpdateSigningIssuer", FakeSigstore.GitLabIssuer);
         WriteSetting("UpdateSourceRepository", FakeSigstore.GitLabGroupRepository);
         _application.Reinitialize();

         var status = Checked();
         string identity = FakeSigstore.GitLabGroupReleaseIdentityPrefix + Newer;
         AssertRefused(status, GitLabGroupSigned(identity, repositoryId: null),
            "The certificate names no source repository identifier.");
         AssertRefused(status, GitLabGroupSigned(identity, repositoryId: "12345678"),
            "is for the repository with the id 12345678, not " + FakeSigstore.GitLabRepositoryId);
         AssertRefused(status, GitLabGroupSigned(FakeSigstore.GitLabGroupReleaseIdentityPrefix + FakeSigstore_Bump(Newer)),
            "The certificate was made for the tag v" + FakeSigstore_Bump(Newer) + ", not for v" + Newer);
         AssertRefused(status, GitLabSigned(FakeSigstore.GitLabReleaseIdentityPrefix + Newer),
            "The certificate names " + FakeSigstore.GitLabReleaseIdentityPrefix + Newer + ", not this repository's release workflow");

         ServeRelease(_installer, _sigstore.Bundle(_installer, GitLabGroupSigned(identity)));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsTrue(status.DownloadUpdate(), "The group's own values, with the project's id. Error: " + status.UpdateLastError);
         CleanUpdates();

         // An identity of the site's own that still names the group's path - here
         // without the "v" - replaces every entry's, and the one kept is the copy
         // of the first, whose id went with its repository. The server still
         // refuses a certificate for the group's path without the project's id,
         // whatever the settings made of the signers.
         WriteSetting("UpdateSigningIdentity", "https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/");
         _application.Reinitialize();
         status = Checked();
         AssertRefused(status, GitLabGroupSigned(identity, repositoryId: null),
            "The certificate names no source repository identifier.");
         AssertRefused(status, GitLabGroupSigned(identity, repositoryId: "12345678"),
            "is for the repository with the id 12345678, not " + FakeSigstore.GitLabRepositoryId);
         ServeRelease(_installer, _sigstore.Bundle(_installer, GitLabGroupSigned(identity)));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsTrue(status.DownloadUpdate(), "The site's identity names the group's path, and the certificate the project's id. Error: " + status.UpdateLastError);
         CleanUpdates();

         // Nor with the widest identity and the repository check off: a
         // certificate for the group's path, or recording its repository, needs
         // the project's id.
         WriteSetting("UpdateSigningIdentity", "https://gitlab.com/");
         WriteSetting("UpdateSourceRepository", "-");
         _application.Reinitialize();
         status = Checked();
         AssertRefused(status, GitLabGroupSigned(identity, repositoryId: null),
            "The certificate names no source repository identifier.");
         AssertRefused(status, GitLabGroupSigned("https://gitlab.com/someone-else/hmailserver//.gitlab-ci.yml@refs/tags/v" + Newer, repositoryId: "12345678"),
            "is for the repository with the id 12345678, not " + FakeSigstore.GitLabRepositoryId);
      }

      [Test]
      public void ABundleFromAnotherAuthorityIsRefused()
      {
         using (var stranger = new FakeSigstore("stranger.test"))
         {
            // The stranger's certificate, recorded in the log this server trusts: the
            // chain is what refuses it.
            ServeRelease(_installer, stranger.Bundle(_installer, new FakeSigstore.Options {Log = _sigstore}));

            var status = Checked();
            Assert.IsFalse(status.DownloadUpdate());
            StringAssert.Contains("does not chain to a trusted authority", status.UpdateLastError);
            AssertNothingKept();
         }
      }

      [Test]
      public void ASignatureRecordedOutsideTheCertificatesLifetimeIsRefused()
      {
         // The log says it recorded the signature an hour after a ten-minute
         // certificate was issued: whoever signed then did not hold that identity.
         ServeRelease(_installer, _sigstore.Bundle(_installer, new FakeSigstore.Options
         {
            SignedAt = DateTimeOffset.UtcNow.AddHours(-1),
            IntegratedAt = DateTimeOffset.UtcNow
         }));

         var status = Checked();
         Assert.IsFalse(status.DownloadUpdate());
         StringAssert.Contains("after the certificate had expired", status.UpdateLastError);
         AssertNothingKept();
      }

      [Test]
      public void AForgedLogRecordIsRefused()
      {
         using (var forger = ECDsa.Create(ECCurve.NamedCurves.nistP256))
         {
            // The entry timestamp and the checkpoint signed by a key that is not the log's.
            ServeRelease(_installer, _sigstore.Bundle(_installer, new FakeSigstore.Options {OtherLogKey = forger}));
            var status = Checked();
            Assert.IsFalse(status.DownloadUpdate());
            StringAssert.Contains("signed entry timestamp does not verify", status.UpdateLastError);
            AssertNothingKept();

            // No timestamp, and an inclusion proof that does not reach the root.
            ServeRelease(_installer, _sigstore.Bundle(_installer, new FakeSigstore.Options {IncludePromise = false, BreakProof = true}));
            Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
            Assert.IsFalse(status.DownloadUpdate());
            StringAssert.Contains("inclusion proof does not lead", status.UpdateLastError);
            AssertNothingKept();

            // Neither: the log has not spoken.
            ServeRelease(_installer, _sigstore.Bundle(_installer, new FakeSigstore.Options {IncludePromise = false, IncludeProof = false}));
            Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
            Assert.IsFalse(status.DownloadUpdate());
            StringAssert.Contains("neither a signed entry timestamp nor an inclusion proof", status.UpdateLastError);
            AssertNothingKept();
         }
      }

      [Test]
      public void EitherFormOfTheLogsWordSuffices()
      {
         var status = Checked();

         // The signed entry timestamp alone.
         ServeRelease(_installer, Released(_installer, options => options.IncludeProof = false));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsTrue(status.DownloadUpdate(), status.UpdateLastError);
         Assert.AreEqual(3, status.UpdateState);
         CleanUpdates();

         // The inclusion proof and checkpoint alone.
         ServeRelease(_installer, Released(_installer, options => options.IncludePromise = false));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsTrue(status.DownloadUpdate(), status.UpdateLastError);
         Assert.AreEqual(3, status.UpdateState);
         CleanUpdates();

         // And the older bundle form, with a certificate chain.
         ServeRelease(_installer, Released(_installer, options => options.ChainForm = true));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsTrue(status.DownloadUpdate(), status.UpdateLastError);
         Assert.AreEqual(3, status.UpdateState);
      }

      [Test]
      public void ACertificateThatIsNotForCodeSigningIsRefused()
      {
         ServeRelease(_installer, _sigstore.Bundle(_installer, new FakeSigstore.Options {NoCodeSigning = true}));
         var status = Checked();
         Assert.IsFalse(status.DownloadUpdate());
         StringAssert.Contains("not a code-signing certificate", status.UpdateLastError);
         AssertNothingKept();
      }

      [Test]
      public void NothingIsDownloadedWithoutAKnownUpdate()
      {
         ServeRelease(_installer, Released(_installer), Running);
         var status = _application.Status;
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.AreEqual(1, status.UpdateState);

         Assert.IsFalse(status.DownloadUpdate(), "Nothing to download when this is the latest release.");
         Assert.AreEqual(0, CountRequests("/download/"), "Nothing was fetched.");
         AssertNothingKept();
      }

      [Test]
      public void ARedirectOffTheLoopbackOverPlainHttpIsRefused()
      {
         ServeRelease(_installer, Released(_installer));
         _feed.SetRedirect("/download/" + _installerName, "http://192.0.2.1/hMailServer.exe");

         var status = Checked();
         Assert.IsFalse(status.DownloadUpdate());
         StringAssert.Contains("plain http is only accepted to a loopback address", status.UpdateLastError);
         AssertNothingKept();
      }

      [Test]
      public void AnAuthenticodeSignatureIsRequiredWhenConfigured()
      {
         WriteSetting("UpdateRequireAuthenticode", "1");
         _application.Reinitialize();

         var status = Checked();
         Assert.IsFalse(status.DownloadUpdate(), "The fake installer carries no Authenticode signature.");
         StringAssert.Contains("The installer's Authenticode signature was refused: the file carries no Authenticode signature", status.UpdateLastError);
         AssertNothingKept();
      }

      [Test]
      public void ADownloadCanBeRunOverRest()
      {
         Checked();
         (int code, string body) = Http("POST", "/api/v1/update/download");
         Assert.AreEqual(200, code, body);
         StringAssert.Contains("\"state\":3", body);
         StringAssert.Contains("\"signer\":\"" + NewerIdentity + "\"", body);
         Assert.IsTrue(File.Exists(Paths.Combine(_updatesDirectory, _installerName)));
      }

      // ---- the Updates folder -------------------------------------------------------

      [Test]
      [Description("The Updates folder and its rollback folder admit changes from SYSTEM, the Administrators and the service alone: protected access lists, and no entry that lets anyone else write.")]
      public void TheUpdatesFolderAdmitsChangesFromTheServerAndTheAdministratorsAlone()
      {
         var status = Checked();
         Assert.IsTrue(status.DownloadUpdate(), status.UpdateLastError);

         AssertOnlyTheServerMayChange(_updatesDirectory);
         AssertOnlyTheServerMayChange(Paths.Combine(_updatesDirectory, "rollback"));
      }

      [Test]
      [Description("An Updates folder others may write to - one an earlier version made, which inherited its parent's permissions - is given the server's own before anything is downloaded into it, and the application log says what it allowed before.")]
      public void AnUpdatesFolderOthersMayWriteToIsMadeRightBeforeADownload()
      {
         // Owned by the Administrators, as a folder the server made is; its list
         // widened to BUILTIN\Users, as a folder that took its parent's could be;
         // and a DLL planted in it meanwhile, where the helper runs from.
         Directory.CreateDirectory(_updatesDirectory);
         var info = new DirectoryInfo(_updatesDirectory);
         DirectorySecurity security = info.GetAccessControl();
         security.SetOwner(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
         security.SetAccessRuleProtection(false, false);
         security.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null), FileSystemRights.Modify,
            InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
         info.SetAccessControl(security);
         string planted = Paths.Combine(_updatesDirectory, "bcrypt.dll");
         File.WriteAllText(planted, "planted while others could write here");

         var status = Checked();
         Assert.IsTrue(status.DownloadUpdate(), "The folder is made right, and the download goes ahead. Error: " + status.UpdateLastError);

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("Update: the permissions of " + _updatesDirectory + " were set to SYSTEM, the Administrators and the service alone; before, it lets ", log);
         StringAssert.Contains("(S-1-5-32-545) change what is in it; everything that was in it has been deleted.", log);
         Assert.IsFalse(File.Exists(planted), "A file planted while others could write to the folder outlived its repair.");
         AssertOnlyTheServerMayChange(_updatesDirectory);
      }

      [Test]
      [Description("An Updates folder owned by an account that is not SYSTEM, the Administrators or the service is refused, as HM6894: its owner may rewrite its permissions whatever they say, and may have left anything inside.")]
      public void AnUpdatesFolderOwnedBySomebodyElseIsRefused()
      {
         SecurityIdentifier me;
         using (WindowsIdentity identity = WindowsIdentity.GetCurrent())
            me = identity.User;
         if (me.IsWellKnown(WellKnownSidType.LocalSystemSid))
            Assert.Inconclusive("This runs as SYSTEM, which is one of the owners the server trusts, so it cannot stand in for somebody else.");

         Directory.CreateDirectory(_updatesDirectory);
         var info = new DirectoryInfo(_updatesDirectory);
         DirectorySecurity security = info.GetAccessControl();
         security.SetOwner(me);
         info.SetAccessControl(security);
         try
         {
            var status = Checked();
            Assert.IsFalse(status.DownloadUpdate(), "Nothing may be downloaded into a folder somebody else owns.");
            StringAssert.Contains("The update was refused: " + _updatesDirectory + " cannot be trusted: it is owned by ", status.UpdateLastError);
            CustomAsserts.AssertReportedError("HM6894", "cannot be trusted: it is owned by ");
         }
         finally
         {
            // The server makes it again, its own, at the next download.
            Directory.Delete(_updatesDirectory, true);
         }
      }

      [Test]
      [Description("While a download runs, the Updates folder and its rollback folder are held open, with data access and without sharing delete: neither can be renamed, so neither can be swapped for a look-alike under the server.")]
      public void TheUpdatesFoldersCannotBeRenamedWhileADownloadRuns()
      {
         var status = Checked();
         // The bundle is asked for first, and nothing is written until its answer:
         // while it stalls, the only handles under Updates are the server's holds.
         string bundlePath = "/download/" + _installerName + ".cosign.bundle";
         _feed.SetStall(bundlePath, 8);
         string rollback = Paths.Combine(_updatesDirectory, "rollback");
         var download = System.Threading.Tasks.Task.Run(() => Http("POST", "/api/v1/update/download"));
         try
         {
            DateTime deadline = DateTime.Now.AddSeconds(20);
            while (CountRequests(_installerName + ".cosign.bundle") == 0)
            {
               Assert.IsTrue(DateTime.Now < deadline, "The server did not ask for the bundle.");
               Thread.Sleep(100);
            }

            // The rollback folder has nothing held below it, so only its own hold -
            // a handle that counts in the share check because it lists - refuses
            // the rename; the Updates folder the same, and it has rollback below.
            Assert.IsTrue(RenameIsRefused(rollback), "The rollback folder could be renamed while a download held it.");
            Assert.IsTrue(RenameIsRefused(_updatesDirectory), "The Updates folder could be renamed while a download held it.");
         }
         finally
         {
            download.Wait(60000);
            _feed.SetStall(bundlePath, 0);
         }
      }

      // Tries to rename folder, and puts it back if that worked.
      private static bool RenameIsRefused(string folder)
      {
         string moved = folder + "-moved";
         try
         {
            Directory.Move(folder, moved);
         }
         catch (IOException)
         {
            return true;
         }
         catch (UnauthorizedAccessException)
         {
            return true;
         }
         Directory.Move(moved, folder);
         return false;
      }

      [Test]
      [Description("An Updates folder that is a link to another folder is refused, as HM6894: whoever made the link could point it elsewhere between the server's check and its run.")]
      public void AnUpdatesFolderThatIsALinkIsRefused()
      {
         if (Directory.Exists(_updatesDirectory))
            Directory.Delete(_updatesDirectory, true);
         // A folder of the test's own, named by a bare hex guid under the temporary
         // folder, for the link to point at.
         string target = Paths.Combine(Path.GetTempPath(), "hm-updates-link-target-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(target);
         try
         {
            var mklink = new System.Diagnostics.ProcessStartInfo("cmd.exe", "/c mklink /J \"" + _updatesDirectory + "\" \"" + target + "\"")
            {
               UseShellExecute = false,
               CreateNoWindow = true
            };
            using (var process = System.Diagnostics.Process.Start(mklink))
               Assert.IsTrue(process.WaitForExit(30000) && process.ExitCode == 0, "mklink /J did not make the junction.");
            Assert.AreNotEqual((FileAttributes) 0, File.GetAttributes(_updatesDirectory) & FileAttributes.ReparsePoint, "The Updates folder is not a link.");

            var status = Checked();
            Assert.IsFalse(status.DownloadUpdate(), "Nothing may be downloaded into a folder that is a link.");
            StringAssert.Contains("The update was refused: " + _updatesDirectory + " is a link to another folder", status.UpdateLastError);
            Assert.IsEmpty(Directory.GetFileSystemEntries(target), "Something was written where the link points.");
            CustomAsserts.AssertReportedError("HM6894", "is a link to another folder");
         }
         finally
         {
            // The junction itself, not what it points at.
            if (Directory.Exists(_updatesDirectory) && (File.GetAttributes(_updatesDirectory) & FileAttributes.ReparsePoint) != 0)
               Directory.Delete(_updatesDirectory);
            Directory.Delete(target, true);
         }
      }

      // Every allow entry on folder is SYSTEM's, the Administrators', the service
      // account's, or grants nothing that changes the folder or what is in it; and
      // the list does not take its parent's.
      private static void AssertOnlyTheServerMayChange(string folder)
      {
         const FileSystemRights changing = FileSystemRights.WriteData | FileSystemRights.AppendData | FileSystemRights.WriteExtendedAttributes |
                                           FileSystemRights.DeleteSubdirectoriesAndFiles | FileSystemRights.WriteAttributes | FileSystemRights.Delete |
                                           FileSystemRights.ChangePermissions | FileSystemRights.TakeOwnership |
                                           (FileSystemRights) 0x40000000 /* GENERIC_WRITE */ | (FileSystemRights) 0x10000000 /* GENERIC_ALL */;

         var trusted = new List<SecurityIdentifier>
         {
            new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
            new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null)
         };
         SecurityIdentifier service = ServiceAccount();
         if (service != null)
            trusted.Add(service);
         // The service's own SID, which a service not running as SYSTEM is given
         // Modify by, whatever account it logs on as.
         try
         {
            trusted.Add((SecurityIdentifier) new NTAccount(@"NT SERVICE\hMailServer").Translate(typeof(SecurityIdentifier)));
         }
         catch (IdentityNotMappedException)
         {
         }

         DirectorySecurity security = new DirectoryInfo(folder).GetAccessControl();
         Assert.IsTrue(security.AreAccessRulesProtected, folder + " takes its parent's permissions.");
         foreach (FileSystemAccessRule rule in security.GetAccessRules(true, true, typeof(SecurityIdentifier)))
         {
            if (rule.AccessControlType != AccessControlType.Allow || trusted.Contains((SecurityIdentifier) rule.IdentityReference))
               continue;
            Assert.AreEqual((FileSystemRights) 0, rule.FileSystemRights & changing, folder + " lets " + rule.IdentityReference + " " + rule.FileSystemRights);
         }
      }

      // The account the service runs as, from its registration: LocalSystem, or a
      // named or virtual account.
      private static SecurityIdentifier ServiceAccount()
      {
         using (RegistryKey key = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\hMailServer"))
         {
            string account = key?.GetValue("ObjectName") as string;
            if (string.IsNullOrEmpty(account) || string.Equals(account, "LocalSystem", StringComparison.OrdinalIgnoreCase))
               return new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
            if (account.StartsWith(@".\", StringComparison.Ordinal))
               account = Environment.MachineName + account.Substring(1);
            try
            {
               return (SecurityIdentifier) new NTAccount(account).Translate(typeof(SecurityIdentifier));
            }
            catch (IdentityNotMappedException)
            {
               return null;
            }
         }
      }

      // ---- the release --------------------------------------------------------------

      private string Running
      {
         get { return ((string) _application.Version).Split('-')[0]; }
      }

      private string Newer
      {
         get { return FakeSigstore_Bump(Running); }
      }

      private static string FakeSigstore_Bump(string version)
      {
         string[] parts = version.Split('.');
         return parts[0] + "." + parts[1] + "." + (int.Parse(parts[2]) + 1);
      }

      private Status Checked()
      {
         var status = _application.Status;
         Assert.IsTrue(status.CheckForUpdate(), "The feed must be readable. Error: " + status.UpdateLastError);
         Assert.AreEqual(2, status.UpdateState, "The fixture's release must be newer than the running version.");
         return status;
      }

      // Serves a release whose installer and bundle are what is given: the feed at
      // the latest path, the installer behind a redirect (as a GitHub asset was), the
      // bundle beside it.
      private void ServeRelease(byte[] installer, string bundle, string version = null, string digest = null)
      {
         version = version ?? Newer;
         string name = "hMailServer-" + version + "-x64.exe";
         byte[] hash;
         using (var sha = SHA256.Create())
            hash = sha.ComputeHash(installer);
         digest = digest ?? "sha256:" + FakeSigstore.Hex(hash);

         _feed.ClearRoutes();
         _feed.SetRedirect("/download/" + name, "/objects/" + name);
         _feed.SetResponse("/objects/" + name, 200, installer);
         _feed.SetResponse("/download/" + name + ".cosign.bundle", 200, Encoding.UTF8.GetBytes(bundle), "application/json");
         _feed.SetResponse(200, Release(version, name, installer.Length, digest));
      }

      private string Release(string version, string name, long size, string digest)
      {
         string download = _feed.UrlFor("/download/");
         return "{\"url\":\"https://gitlab.com/api/v4/projects/86730559/releases/v" + version + "\"," +
                "\"html_url\":\"https://gitlab.com/hmailserver/hmailserver/-/releases/v" + version + "\"," +
                "\"id\":1001,\"author\":{\"login\":\"Progressiverobot\",\"html_url\":\"https://gitlab.com/Progressiverobot\"}," +
                "\"tag_name\":\"v" + version + "\",\"name\":\"hMailServer " + version + "\",\"draft\":false,\"prerelease\":false," +
                "\"published_at\":\"2026-09-12T10:00:00Z\",\"assets\":[" +
                "{\"name\":\"" + name + "\",\"size\":" + size + ",\"digest\":\"" + digest + "\",\"browser_download_url\":\"" + download + name + "\"}," +
                "{\"name\":\"" + name + ".cosign.bundle\",\"size\":1,\"browser_download_url\":\"" + download + name + ".cosign.bundle\"}" +
                "],\"body\":\"notes\"}";
      }

      // The signer of the release offered: the GitLab pipeline for v<Newer>.
      private string NewerIdentity
      {
         get { return FakeSigstore.ReleaseSigning(Newer).Identity; }
      }

      // The bundle of the release offered, signed as the project signs it, with
      // whatever change the test asks for.
      private string Released(byte[] installer, Action<FakeSigstore.Options> change = null)
      {
         FakeSigstore.Options options = FakeSigstore.ReleaseSigning(Newer);
         change?.Invoke(options);
         return _sigstore.Bundle(installer, options);
      }

      private static readonly Dictionary<string, byte[]> FakeInstallers = new Dictionary<string, byte[]>();

      // A Windows program that does nothing, with the version resource a real
      // installer carries: Inno Setup writes <version>.0 from VersionInfoVersion,
      // and the server holds the file to the release it installs. Nothing here
      // runs it.
      private static byte[] FakeInstaller(string version)
      {
         lock (FakeInstallers)
         {
            byte[] bytes;
            if (FakeInstallers.TryGetValue(version, out bytes))
               return bytes;

            string directory = Paths.Combine(Path.GetTempPath(), "hm-fake-installers");
            Directory.CreateDirectory(directory);
            // version is this fixture's own "6.3.4", so the name is a file name.
            string path = Paths.Combine(directory, "verify-" + version + ".exe");
            string source =
               "[assembly: System.Reflection.AssemblyFileVersion(\"" + version + ".0\")]\n" +
               "class FakeInstaller\n{\n   static int Main()\n   {\n      return 0;\n   }\n}\n";
            using (CodeDomProvider provider = CodeDomProvider.CreateProvider("CSharp"))
            {
               var parameters = new CompilerParameters {GenerateExecutable = true, OutputAssembly = path, GenerateInMemory = false};
               CompilerResults results = provider.CompileAssemblyFromSource(parameters, source);
               if (results.Errors.HasErrors)
                  Assert.Fail("The fake installer did not compile: " + results.Errors[0]);
            }
            bytes = File.ReadAllBytes(path);
            FakeInstallers[version] = bytes;
            return bytes;
         }
      }

      // A bundle as this project's GitLab CI pipeline had it made at the path the
      // project began at (6.3.5's), signed now: GitLab's issuer, the GitLab project
      // and its id, unless told otherwise.
      private static FakeSigstore.Options GitLabSigned(string identity, string issuer = FakeSigstore.GitLabIssuer,
         string repository = FakeSigstore.GitLabRepository, string repositoryId = FakeSigstore.GitLabRepositoryId)
      {
         return new FakeSigstore.Options
         {
            Identity = identity,
            Issuer = issuer,
            Repository = repository,
            RepositoryId = repositoryId,
            SignedAt = DateTimeOffset.UtcNow
         };
      }

      // The same, as the pipeline has it made since the project moved to the group
      // on 29 September 2026, from 6.3.6: the group's repository, and the same
      // project's id.
      private static FakeSigstore.Options GitLabGroupSigned(string identity, string issuer = FakeSigstore.GitLabIssuer,
         string repository = FakeSigstore.GitLabGroupRepository, string repositoryId = FakeSigstore.GitLabRepositoryId)
      {
         return GitLabSigned(identity, issuer, repository, repositoryId);
      }

      // Serves the release with a bundle made as options says, checks, and requires
      // the download to be refused for the reason given, with nothing kept.
      private void AssertRefused(Status status, FakeSigstore.Options options, string reason)
      {
         ServeRelease(_installer, _sigstore.Bundle(_installer, options));
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsFalse(status.DownloadUpdate(), options.Identity + " on a token from " + options.Issuer + " for " + options.Repository + " must be refused.");
         StringAssert.Contains(reason, status.UpdateLastError);
         AssertNothingKept();
      }

      private int CountRequests(string fragment)
      {
         return _feed.Requests.Count(request => request.Contains(fragment));
      }

      private void AssertNothingKept()
      {
         if (!Directory.Exists(_updatesDirectory))
            return;
         string[] left = Directory.GetFiles(_updatesDirectory);
         Assert.IsEmpty(left, "A file that failed verification must not be left in " + _updatesDirectory + ": " + string.Join(", ", left));
      }

      private void CleanUpdates()
      {
         if (!Directory.Exists(_updatesDirectory))
            return;
         foreach (string file in Directory.GetFiles(_updatesDirectory))
         {
            try
            {
               File.Delete(file);
            }
            catch (UnauthorizedAccessException)
            {
               // apply-token carries a DACL naming SYSTEM, Administrators and the
               // service account only; the suite is none of those. The server
               // revokes it after an apply, and one never redeemed expires.
            }
         }
      }

      // ---- plumbing ----------------------------------------------------------------

      private static void WriteSetting(string key, string value)
      {
         IniFileSetting.Write(key, value);
      }

      private static (int code, string body) Http(string method, string path)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", RestPort);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  Thread.Sleep(200);
               }
            }
            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               string request =
                  method + " " + path + " HTTP/1.0\r\n" +
                  "Host: 127.0.0.1\r\n" +
                  "Authorization: Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword)) + "\r\n" +
                  "Connection: close\r\n\r\n";
               byte[] bytes = Encoding.ASCII.GetBytes(request);
               stream.Write(bytes, 0, bytes.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               int code = 0;
               Match status = Regex.Match(raw, "^HTTP/1\\.[01] (\\d{3})");
               if (status.Success)
                  code = int.Parse(status.Groups[1].Value);
               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (code, separator >= 0 ? raw.Substring(separator + 4) : "");
            }
         }
      }
   }
}
