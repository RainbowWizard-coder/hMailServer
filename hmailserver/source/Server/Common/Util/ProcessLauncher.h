// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class ProcessLauncher
   {
   public:
      // Why a Launch that returned false failed. The two are diagnosed
      // oppositely - a bad path is the administrator's to fix, a timeout is the
      // scanner's own behaviour - so a caller that says "unable to launch" for a
      // process that launched perfectly and was then killed for running too long
      // sends the reader looking in exactly the wrong place.
      enum class FailureReason
      {
         None,           // Launch returned true.
         StartFailed,    // CreateProcess itself failed - bad path, permissions.
         TimedOut,       // Ran past ExternalProcessTimeout and was terminated.
         OutputFileFailed // Not started: SetStandardOutputFile's file could not be created (HM7541).
      };

      ProcessLauncher(const String &commandLine, const String &workingDirectory);
      ProcessLauncher(const String &commandLine);
      ~ProcessLauncher(void);

      bool Launch(unsigned int &exitCode);
      bool Launch(unsigned int &exitCode, FailureReason &failureReason);

      void SetErrorLogTimeout(unsigned int milliseconds);

      // A hard ceiling on the whole run, in milliseconds; zero (the default)
      // leaves the policy above alone. Whichever of the two bounds is tighter is
      // the one that applies.
      //
      // It is separate from SetErrorLogTimeout because it answers a different
      // question. That one means "warn about this process if it is slow, and stop
      // it at ExternalProcessTimeout" - a number an administrator may set to zero
      // to mean "wait for my virus scanner however long it takes". This one is
      // the CALLER's own limit, set because of where the calling thread came from
      // rather than because of what the process is: a request-serving thread that
      // must be given back, whatever the ini file says. Nothing that writes a
      // backup sets it - 7za legitimately runs for hours over a large message
      // store, and killing it would truncate the archive.
      void SetMaximumRunTime(unsigned int milliseconds);

      // Gives the process these bytes as its whole standard input - it reads
      // them and then the end of the input.
      //
      // Its standard OUTPUT is left alone on Linux and sent to the null device on
      // Windows, which is not an inconsistency but the same decision in two
      // places: a Windows service has no console, so a child's output goes
      // nowhere a person can read either way and the handle list has to name
      // something; a Linux server's standard output and error are the journal's,
      // where the archiver's own account of a failure - an unsupported switch, a
      // full disk, a refused path - is the only explanation anybody gets beyond
      // an exit code, and discarding it would make every failure quieter than it
      // was before this class could redirect anything.
      //
      // Two uses, and both are the backup's. A secret handed to a program this
      // way is not on its command line, which any user of the machine can read
      // from the process list; the backup passphrase reaches 7-Zip like this
      // and no other way. And a program that stops to ask a question - 7-Zip
      // asking for the passphrase of an archive nobody gave one for - reads the
      // end of the input and fails, instead of waiting for ever at a prompt.
      //
      // The bytes are written before the process is started and must fit in
      // the pipe's buffer, so that nothing can wait on a reader: at most
      // MaximumStandardInput bytes, and Launch refuses more.
      void SetStandardInput(const AnsiString &input);

      // Sends the process's standard output to this file, created or emptied
      // first, instead of where SetStandardInput's comment says it goes; its
      // standard error goes where it went before. For a program whose answer is
      // what it prints - "7za l -slt", which the restore reads to check an
      // archive's entries before anything is extracted - written to a file of the
      // caller's own scratch directory rather than read through a pipe, so that
      // no output of any size can leave either process waiting on the other.
      //
      // It gives the process a standard input as well: the one SetStandardInput
      // gave, or an empty one, because Windows sets the standard handles all
      // together or not at all. A file that cannot be created is a process not
      // started (HM7541).
      void SetStandardOutputFile(const String &path);

      static const int MaximumStandardInput = 4096;

   private:

      unsigned int error_log_timeout_;
      unsigned int maximum_run_time_;

      String command_line_;
      String working_directory_;

      bool redirect_standard_input_;
      AnsiString standard_input_;

      String standard_output_file_;
   };
}