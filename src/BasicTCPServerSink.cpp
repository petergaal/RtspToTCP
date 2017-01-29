/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 3 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// Author: Peter Gaal
// A simple TCP server sink (i.e., without RTP or other headers added); one frame per packet
// Implementation
// it supports MJPEG and H.264 video streaming on TCP server port to each connected client
// other media content should also work but this wasn't tested

#include "BasicTCPServerSink.h"
#include <GroupsockHelper.hh>


BasicTCPServerSink* BasicTCPServerSink::createNew(UsageEnvironment& env, Port ourPort,
                    unsigned maxPayloadSize) {
  int ourSocket = setUpOurSocket(env, ourPort);
  if (ourSocket == -1) return NULL;
  return new BasicTCPServerSink(env, ourSocket, ourPort, maxPayloadSize);

}

BasicTCPServerSink::BasicTCPServerSink(UsageEnvironment& env, 
    int ourSocket, Port ourPort, unsigned maxPayloadSize)
  : MediaSink(env),
    fServerSocket(ourSocket), fServerPort(ourPort),
    fMaxPayloadSize(maxPayloadSize),
    H264(False),
    fServerMediaSessions(HashTable::create(STRING_HASH_KEYS)),
    fClientConnections(HashTable::create(ONE_WORD_HASH_KEYS)),
    fClientSessions(HashTable::create(STRING_HASH_KEYS)) {
  fOutputBuffer = new unsigned char[fMaxPayloadSize];
  ignoreSigPipeOnSocket(fServerSocket); // so that clients on the same host that are killed don't also kill us

  // Arrange to handle connections from others:
  env.taskScheduler().turnOnBackgroundReadHandling(fServerSocket, incomingConnectionHandler, this);

  //fOutputBuffer = new unsigned char[fMaxPayloadSize];
}

BasicTCPServerSink::~BasicTCPServerSink() {
  delete[] fOutputBuffer;
  envir().taskScheduler().turnOffBackgroundReadHandling(fServerSocket);
  ::closeSocket(fServerSocket);
}

void BasicTCPServerSink::cleanup() {
  // This member function must be called in the destructor of any subclass of
  // "BasicTCPServerSink".  (We don't call this in the destructor of "BasicTCPServerSink" itself,
  // because by that time, the subclass destructor will already have been called, and this may
  // affect (break) the destruction of the "ClientSession" and "ClientConnection" objects, which
  // themselves will have been subclassed.)

  // Close all client session objects:
  /*
  BasicTCPServerSink::ClientSession* clientSession;
  while ((clientSession = (BasicTCPServerSink::ClientSession*)fClientSessions->getFirst()) != NULL) {
    delete clientSession;
  }
  delete fClientSessions;
  */

  // Close all client connection objects:
  BasicTCPServerSink::ClientConnection* connection;
  while ((connection = (BasicTCPServerSink::ClientConnection*)fClientConnections->getFirst()) != NULL) {
    delete connection;
  }
  delete fClientConnections;

  /*
  // Delete all server media sessions
  ServerMediaSession* serverMediaSession;
  while ((serverMediaSession = (ServerMediaSession*)fServerMediaSessions->getFirst()) != NULL) {
    removeServerMediaSession(serverMediaSession); // will delete it, because it no longer has any 'client session' objects using it
  }
  delete fServerMediaSessions;
  */

}

#define LISTEN_BACKLOG_SIZE 20

int BasicTCPServerSink::setUpOurSocket(UsageEnvironment& env, Port& ourPort) {
  int ourSocket = -1;

  do {
    // The following statement is enabled by default.
    // Don't disable it (by defining ALLOW_SERVER_PORT_REUSE) unless you know what you're doing.
#if !defined(ALLOW_SERVER_PORT_REUSE) && !defined(ALLOW_RTSP_SERVER_PORT_REUSE)
    // ALLOW_RTSP_SERVER_PORT_REUSE is for backwards-compatibility #####
    NoReuse dummy(env); // Don't use this socket if there's already a local server using it
#endif

    ourSocket = setupStreamSocket(env, ourPort);
    if (ourSocket < 0) break;

    // Make sure we have a big send buffer:
    if (!increaseSendBufferTo(env, ourSocket, 50 * 1024)) break;

    // Allow multiple simultaneous connections:
    if (listen(ourSocket, LISTEN_BACKLOG_SIZE) < 0) {
      env.setResultErrMsg("listen() failed: ");
      break;
    }

    if (ourPort.num() == 0) {
      // bind() will have chosen a port for us; return it also:
      if (!getSourcePort(env, ourSocket, ourPort)) break;
    }

    return ourSocket;
  } while (0);

  if (ourSocket != -1) ::closeSocket(ourSocket);
  return -1;
}

void BasicTCPServerSink::incomingConnectionHandler(void* instance, int /*mask*/) {
  BasicTCPServerSink* server = (BasicTCPServerSink*)instance;
  server->incomingConnectionHandler();
}
void BasicTCPServerSink::incomingConnectionHandler() {
  incomingConnectionHandlerOnSocket(fServerSocket);
}

void BasicTCPServerSink::incomingConnectionHandlerOnSocket(int serverSocket) {
  struct sockaddr_in clientAddr;
  SOCKLEN_T clientAddrLen = sizeof clientAddr;
  int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
  if (clientSocket < 0) {
    int err = envir().getErrno();
    if (err != EWOULDBLOCK) {
      envir().setResultErrMsg("accept() failed: ");
    }
    return;
  }
  ignoreSigPipeOnSocket(clientSocket); // so that clients on the same host that are killed don't also kill us
  makeSocketNonBlocking(clientSocket);
  increaseSendBufferTo(envir(), clientSocket, 50 * 1024);

#ifdef DEBUG
  envir() << "accept()ed connection from " << AddressString(clientAddr).val() << "\n";
#endif
  envir() << "accept()ed connection from " << AddressString(clientAddr).val() << ", clientSocket=" << clientSocket << "\n";

  // Create a new object for handling this connection:
  (void)createNewClientConnection(clientSocket, clientAddr);
}

BasicTCPServerSink::ClientConnection
::ClientConnection(BasicTCPServerSink& ourServer, int clientSocket, struct sockaddr_in clientAddr)
  : fOurServer(ourServer), fOurSocket(clientSocket), fClientAddr(clientAddr), 
  fClientOutputSocket(fOurSocket), fClientInputSocket(fOurSocket), fIsActive(True) {
  // Add ourself to our 'client connections' table:
  fOurServer.fClientConnections->Add((char const*)this, this);

  // Arrange to handle incoming requests:
  resetRequestBuffer();
  envir().taskScheduler()
    .setBackgroundHandling(fOurSocket, SOCKET_READABLE | SOCKET_EXCEPTION, incomingRequestHandler, this);
}

BasicTCPServerSink::ClientConnection::~ClientConnection() {
  // Remove ourself from the server's 'client connections' hash table before we go:
  fOurServer.fClientConnections->Remove((char const*)this);
  envir() << "closed connection from " << AddressString(fClientAddr).val() << ", clientSocket=" << fOurSocket << "\n";

  closeSockets();
}

void BasicTCPServerSink::ClientConnection::closeSockets() {
  // Turn off background handling on our socket:
  envir().taskScheduler().disableBackgroundHandling(fOurSocket);
  if (fOurSocket >= 0) ::closeSocket(fOurSocket);

  fOurSocket = -1;
}

void BasicTCPServerSink::ClientConnection::closeSocketsTCPServer() {
  // First, tell our server to stop any streaming that it might be doing over our output socket:
  fOurServer.stopTCPStreamingOnSocket(fClientOutputSocket);

  // Turn off background handling on our input socket (and output socket, if different); then close it (or them):
  if (fClientOutputSocket != fClientInputSocket) {
    envir().taskScheduler().disableBackgroundHandling(fClientOutputSocket);
    ::closeSocket(fClientOutputSocket);
  }
  fClientOutputSocket = -1;

  closeSockets(); // closes fClientInputSocket
}




void BasicTCPServerSink::ClientConnection::incomingRequestHandler(void* instance, int /*mask*/) {
  ClientConnection* connection = (ClientConnection*)instance;
  connection->incomingRequestHandler();
}

void BasicTCPServerSink::ClientConnection::incomingRequestHandler() {
  struct sockaddr_in dummy; // 'from' address, meaningless in this case

  int bytesRead = readSocket(envir(), fOurSocket, &fRequestBuffer[fRequestBytesAlreadySeen], fRequestBufferBytesLeft, dummy);
  handleRequestBytes(bytesRead);
}

void BasicTCPServerSink::ClientConnection::resetRequestBuffer() {
  fRequestBytesAlreadySeen = 0;
  fRequestBufferBytesLeft = sizeof fRequestBuffer;
}


void BasicTCPServerSink::ClientConnection::handleRequestBytes(int newBytesRead) {
  int numBytesRemaining = 0;
  // ignore any incomming bytes

  if (newBytesRead < 0 || (unsigned)newBytesRead >= fRequestBufferBytesLeft) {
    // Either the client socket has died, or the request was too big for us.
    // Terminate this connection:
#ifdef DEBUG
    fprintf(stderr, "RTSPClientConnection[%p]::handleRequestBytes() read %d new bytes (of %d); terminating connection!\n", this, newBytesRead, fRequestBufferBytesLeft);
#endif
    fIsActive = False;
//    break;
  }

  if (!fIsActive) {
//    fOurServer.fClientConnections->Add((char const*)this, this);
//    fOurServer.fClientConnections->Remove((char const*)this);
//    closeSockets();
    delete this;
  }

}

void BasicTCPServerSink::stopTCPStreamingOnSocket(int socketNum) {
  // Close any stream that is streaming over "socketNum" (using RTP/RTCP-over-TCP streaming):
  /*
  streamingOverTCPRecord* sotcp
    = (streamingOverTCPRecord*)fTCPStreamingDatabase->Lookup((char const*)socketNum);
  if (sotcp != NULL) {
    do {
      RTSPClientSession* clientSession
        = (RTSPServer::RTSPClientSession*)lookupClientSession(sotcp->fSessionId);
      if (clientSession != NULL) {
        clientSession->deleteStreamByTrack(sotcp->fTrackNum);
      }

      streamingOverTCPRecord* sotcpNext = sotcp->fNext;
      sotcp->fNext = NULL;
      delete sotcp;
      sotcp = sotcpNext;
    } while (sotcp != NULL);
    fTCPStreamingDatabase->Remove((char const*)socketNum);
  }
  */
}


Boolean BasicTCPServerSink::continuePlaying() {
  // Record the fact that we're starting to play now:
  gettimeofday(&fNextSendTime, NULL);

  // Arrange to get and send the first payload.
  // (This will also schedule any future sends.)
  continuePlaying1();
  return True;
}

void BasicTCPServerSink::continuePlaying1() {
  nextTask() = NULL;
  if (fSource != NULL) {
    fSource->getNextFrame(fOutputBuffer, fMaxPayloadSize,
			  afterGettingFrame, this,
			  onSourceClosure, this);
  }
}

void BasicTCPServerSink::afterGettingFrame(void* clientData, unsigned frameSize,
				     unsigned numTruncatedBytes,
				     struct timeval presentationTime,
				     unsigned durationInMicroseconds) {
  BasicTCPServerSink* sink = (BasicTCPServerSink*)clientData;
  sink->afterGettingFrame1(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

void BasicTCPServerSink::afterGettingFrame1(unsigned frameSize, unsigned numTruncatedBytes, struct timeval presentationTime,
				      unsigned durationInMicroseconds) {
  if (numTruncatedBytes > 0) {
    envir() << "BasicTCPServerSink::afterGettingFrame1(): The input frame data was too large for our spcified maximum payload size ("
	    << fMaxPayloadSize << ").  "
	    << numTruncatedBytes << " bytes of trailing data was dropped!\n";
  }

  static int counter = 0;
  char filename[50];
  sprintf(filename, "frame%4.4d.264", counter);
  const char nalbytes[4] = { 0, 0, 0, 1 };

  // Send the packet:
  //fGS->output(envir(), fOutputBuffer, frameSize);
//  send(fClientOutputSocket, (char const*)fResponseBuffer, strlen((char*)fResponseBuffer), 0);

/*
 // this is just for debugging output into file
FILE *f;
f = fopen(filename, "w+b");
if (f != NULL) {
counter++;
fwrite(fOutputBuffer, 1, frameSize, f);
fclose(f);
}
*/

//  while ((connection = (BasicTCPServerSink::ClientConnection*)fClientConnections->getFirst()) != NULL) {
//    delete connection;
    //send(connection->fClientOutputSocket, (char const*)fOutputBuffer, frameSize, 0);
//  }
//  delete fClientConnections;
  char fResponseBuffer[256];
  sprintf(fResponseBuffer, "frameSize: %d bytes, dur: %d us\r\n", frameSize, durationInMicroseconds);
  
  HashTable::Iterator* iter = HashTable::Iterator::create(*fClientConnections);
  BasicTCPServerSink::ClientConnection* clientConnection;
  char const* key; // dummy
  while ((clientConnection = (BasicTCPServerSink::ClientConnection*)(iter->next(key))) != NULL) {
    if (clientConnection->fIsActive) {
      //send(clientConnection->fClientOutputSocket, (char const*)fResponseBuffer, strlen((char*)fResponseBuffer), 0);
      if (H264) {
        send(clientConnection->fClientOutputSocket, (char const*)nalbytes, 4, 0);
      }
      send(clientConnection->fClientOutputSocket, (char const*)fOutputBuffer, frameSize, 0);
    }
  }
  delete iter;

//#ifdef DEBUG
  envir() << "Received " << frameSize << " bytes";
  if (numTruncatedBytes > 0) envir() << " (with " << numTruncatedBytes << " bytes truncated)";
  char uSecsStr[6 + 1]; // used to output the 'microseconds' part of the presentation time
  sprintf(uSecsStr, "%06u", (unsigned)presentationTime.tv_usec);
  envir() << ".\tPresentation time: " << (int)presentationTime.tv_sec << "." << uSecsStr;
  envir() << "\n";
//#endif


  // Figure out the time at which the next packet should be sent, based
  // on the duration of the payload that we just read:
  fNextSendTime.tv_usec += durationInMicroseconds;
  fNextSendTime.tv_sec += fNextSendTime.tv_usec/1000000;
  fNextSendTime.tv_usec %= 1000000;


  struct timeval timeNow;
  gettimeofday(&timeNow, NULL);
  int secsDiff = fNextSendTime.tv_sec - timeNow.tv_sec;
  int64_t uSecondsToGo = secsDiff*1000000 + (fNextSendTime.tv_usec - timeNow.tv_usec);
  if (uSecondsToGo < 0 || secsDiff < 0) { // sanity check: Make sure that the time-to-delay is non-negative:
    uSecondsToGo = 0;
  }

  // Delay this amount of time:
  nextTask() = envir().taskScheduler().scheduleDelayedTask(uSecondsToGo,
							   (TaskFunc*)sendNext, this);
}

// The following is called after each delay between packet sends:
void BasicTCPServerSink::sendNext(void* firstArg) {
  BasicTCPServerSink* sink = (BasicTCPServerSink*)firstArg;
  sink->continuePlaying1();
}



BasicTCPServerSink::ClientConnection* 
BasicTCPServerSink::createNewClientConnection(int clientSocket, struct sockaddr_in clientAddr) {
  //return new RTSPClientConnection(*this, clientSocket, clientAddr);
  return new BasicTCPServerSink::ClientConnection(*this, clientSocket, clientAddr);
}
