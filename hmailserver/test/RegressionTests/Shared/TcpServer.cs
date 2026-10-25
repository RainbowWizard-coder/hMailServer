// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

using System;
using System.Text;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography.X509Certificates;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using RegressionTests.Infrastructure;

namespace RegressionTests.Shared
{
   public class TcpServer : IDisposable
   {
      private readonly ManualResetEvent _clientCompleted = new ManualResetEvent(false);
      private readonly ManualResetEvent _listenThreadStarted = new ManualResetEvent(false);

      private readonly int _maxNumberOfConnections;
      private readonly int _port;
      private readonly ManualResetEvent _stopServerEvent = new ManualResetEvent(false);

      private readonly ManualResetEvent _workerThreadFinished = new ManualResetEvent(false);

      // Held while the listener is made and while it is stopped, which happens once:
      // the listen thread and Dispose both stop it, and .NET Framework's
      // TcpListener.Stop is not safe against itself - two threads in it at once can
      // each find its socket there and one then find it gone, which it answers with
      // NullReferenceException (SMTP.BounceReports in batch44's gate, 9 October 2026).
      private readonly object _listenerLock = new object();
      private bool _listenerStopped;

      protected eConnectionSecurity _connectionSecurity;

      private X509Certificate2 _localCertificate;

      private int _numberOfConnectedClients;
      private Thread _serverThread;
      protected TcpConnection _tcpConnection;
      private TcpListener _tcpListener;
      private Exception _workerThreadException;

      // What ended the accepted client's conversation, when something did. Not
      // a failure on its own - a test that got what it wanted before the
      // connection went away has nothing to answer for - but the first thing
      // worth reading when a test does fail on a message that never arrived.
      private Exception _clientException;

      public TcpServer(int maxNumberOfConnections, int port, eConnectionSecurity connectionSecurity)
      {
         _maxNumberOfConnections = maxNumberOfConnections;
         _port = port;
         _connectionSecurity = connectionSecurity;

         SecondsToWaitBeforeTerminate = 15;
      }

      public int SecondsToWaitBeforeTerminate { get; set; }

      /// <summary>
      ///    How many clients have connected so far. For a test whose point is
      ///    that a second connection was NOT made - a destination in a back-off
      ///    window - while the simulator still listens for one.
      /// </summary>
      public int NumberOfConnectedClients => _numberOfConnectedClients;

      // Appended in linear time: a large message arriving in small reads
      // used to re-copy the whole conversation on every read.
      private readonly StringBuilder _conversation = new StringBuilder();
      public string Conversation => _conversation.ToString();

      public void Dispose()
      {
         _stopServerEvent.Set();

         var finished = _workerThreadFinished.WaitOne(TimeSpan.FromSeconds(15), false);

         // The listen thread stops the listener before it says it has finished, so
         // this finds it stopped - unless that thread did not finish, and then the
         // listener is stopped under it, and the thread's own stop finds it done.
         StopListener();

         DisposeSocket();

         Assert.IsTrue(finished, "The simulated server's listen thread did not finish within 15 seconds of being told to stop.");
      }

      public void SetCertificate(X509Certificate2 certificate)
      {
         _localCertificate = certificate;
      }

      public void StartListen()
      {
         Console.WriteLine("Starting listen...");

         _listenThreadStarted.Reset();


         _serverThread = new Thread(StartListenWorker);
         _serverThread.Name = "Listen thread";
         _serverThread.Start();

         // Don't return until we have started to listen. This is done to prevent
         // someone from trying to continue the test before we're ready.
         Assert.IsTrue(_listenThreadStarted.WaitOne(TimeSpan.FromSeconds(15), false));

         if (_workerThreadException != null)
            throw _workerThreadException;
      }

      private void StartListenWorker()
      {
         try
         {
            var local = IPAddress.Parse("0.0.0.0");

            lock (_listenerLock)
            {
               if (_listenerStopped)
                  throw new InvalidOperationException("The simulated server was stopped before it began to listen; a TcpServer listens once.");

               _tcpListener = new TcpListener(local, _port)
               {
                  ExclusiveAddressUse = false
               };

               _tcpListener.Start();
            }

            HandleClients();
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            _workerThreadException = e;
         }
         finally
         {
            // The listener is stopped before this thread says it has finished, since
            // WaitForCompletion and Dispose return on that word and the test goes on.
            // Until 9 October 2026 the stop came after it, so a Dispose called at once
            // stopped the same listener at the same moment, and a listener whose
            // construction had thrown (a port out of range) left null to stop - a
            // NullReferenceException here, on a thread where nothing catches it: on .NET
            // (the Linux suite) that ends the test host and the run; NUnit's agent on
            // Windows runs with the legacy unhandled-exception policy, which lets the
            // thread die alone and says nothing.
            try
            {
               StopListener();
            }
            catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
            {
               if (_workerThreadException == null)
                  _workerThreadException = e;
            }

            _listenThreadStarted.Set();
            _workerThreadFinished.Set();
         }
      }

      /// <summary>
      ///    Stops the listener, the first time it is called and only then, whichever
      ///    thread calls it. A listener whose Start threw is stopped all the same, which
      ///    both runtimes allow; one whose construction threw is null, and there is
      ///    nothing to stop.
      /// </summary>
      private void StopListener()
      {
         lock (_listenerLock)
         {
            if (_listenerStopped)
               return;

            _listenerStopped = true;

            _tcpListener?.Stop();
         }
      }

      private void HandleClients()
      {
         for (var i = 0; i < _maxNumberOfConnections; i++)
         {
            _clientCompleted.Reset();

            try
            {
               // BeginAcceptTcpClient, because OnAcceptSocket ends the accept with
               // EndAcceptTcpClient. .NET Framework lets the two be mixed; .NET
               // answers the mismatch with an ArgumentException, which the handler
               // below treats as "the listener was stopped" - so on Linux every
               // client the simulator accepted was dropped unanswered, and a
               // server relaying to it saw a reset or twenty seconds of silence.
               _tcpListener.BeginAcceptTcpClient(OnAcceptSocket, null);
            }
            finally
            {
               _listenThreadStarted.Set();
            }

            WaitHandle.WaitAny(new WaitHandle[]
            {
               _stopServerEvent,
               _clientCompleted
            });

            if (_stopServerEvent.WaitOne(TimeSpan.FromMilliseconds(1), false))
               return;
         }
      }

      private void OnAcceptSocket(IAsyncResult asyncResult)
      {
         try
         {
            _tcpConnection = null;

            try
            {
               _tcpConnection = new TcpConnection(_tcpListener.EndAcceptTcpClient(asyncResult));
            }
            catch (ObjectDisposedException)
            {
               return;
            }
            catch (ArgumentException)
            {
               // The same case on .NET (the Linux suite): a listener stopped with an
               // accept pending completes that accept with ArgumentException rather
               // than ObjectDisposedException, on a thread-pool thread, where an
               // exception nobody catches ends the test host.
               return;
            }
            catch (SocketException e)
            {
               // .NET's TcpListener.Stop disposes its socket and only then marks itself
               // stopped, and EndAcceptTcpClient turns the aborted accept's SocketException
               // into ObjectDisposedException only once it is marked: a thread-pool thread
               // that gets here between the two gets the SocketException itself (125,
               // "Operation canceled", on Linux), which nothing caught and which ended the
               // Linux suite's test host. An accept that fails for any other reason ends
               // this way too, on either runtime. Either way there is no client to serve;
               // a failure while the server is not being stopped is kept for
               // WaitForCompletion to report.
               if (!_stopServerEvent.WaitOne(0, false))
                  _clientException = e;
               return;
            }

            try
            {
               // Inside the handler below with the conversation: a handshake that
               // fails, or that Dispose cuts short by closing the connection under
               // it, threw from here straight out of the thread-pool thread.
               if (_connectionSecurity == eConnectionSecurity.eCSTLS) _tcpConnection.HandshakeAsServer(_localCertificate);

               _numberOfConnectedClients++;

               HandleClient();
            }
            catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
            {
               // This runs on a thread-pool thread, where an exception nobody
               // catches ends the process - the whole test run, not the test.
               // A conversation that ends abruptly (the client resets, or this
               // server is disposed while a read is outstanding, which is what
               // the test's own using block does when an assertion has already
               // failed) is not worth a run: the test that owns this simulator
               // fails on what it was actually asserting, and this is kept so
               // that WaitForCompletion can say what happened to the client.
               _clientException = e;
            }
         }
         finally
         {
            DisposeSocket();

            _clientCompleted.Set();
         }
      }

      private void DisposeSocket()
      {
         // Dispose and the thread-pool thread that served the client both get here
         // when a test disposes the server with a conversation still open. Each takes
         // the connection out of the field in one step, and the one that finds it gone
         // does nothing: a test of the field and a use of it apart let the other's null
         // fall between the two, a NullReferenceException that nothing catches on the
         // thread-pool thread.
         Interlocked.Exchange(ref _tcpConnection, null)?.Dispose();
      }

      protected virtual void HandleClient()
      {
      }

      public void WaitForCompletion()
      {
         if (_workerThreadException != null)
            throw new Exception("An error occurred when calling AcceptSocket.", _workerThreadException);

         for (var i = 0; i < SecondsToWaitBeforeTerminate; i++)
            if (_workerThreadFinished.WaitOne(1000, true))
               return;

         var log = LogHandler.ReadCurrentDefaultLog();

         if (_clientException != null)
            Assert.Fail($"At {DateTime.Now} - the simulated server's conversation with its client ended: " +
                        _clientException + "\r\nConversation so far:\r\n" + Conversation + "\r\nLog:\r\n" + log);

         if (_numberOfConnectedClients < _maxNumberOfConnections)
            Assert.Fail(
               $"At {DateTime.Now} - Client did not connect to simulated server. Expected connection count: {_maxNumberOfConnections}, Actual: {_numberOfConnectedClients}\r\nLog:\r\n{log}");
         else
            Assert.Fail(
               $"At {DateTime.Now} - Client did not disconnect from simulated server. Expected connection count: {_maxNumberOfConnections}, Actual: {_numberOfConnectedClients}\r\nLog:\r\n{log}");
      }

      public void Disconnect()
      {
         _tcpConnection.Disconnect();
      }

      public void Send(string s)
      {
         _conversation.Append(s);
         _tcpConnection.Send(s);
      }

      public string Receive()
      {
         var data = _tcpConnection.Receive();
         _conversation.Append(data);
         return data;
      }

      public string ReadUntil(string text)
      {
         var data = _tcpConnection.ReadUntil(text);
         _conversation.Append(data);
         return data;
      }

      public string ReadUntil(List<string> possibleReplies)
      {
         var data = _tcpConnection.ReadUntil(possibleReplies);
         _conversation.Append(data);
         return data;
      }
   }
}