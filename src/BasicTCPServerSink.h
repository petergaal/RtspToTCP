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
// "liveMedia"
// Copyright (c) 1996-2017 Live Networks, Inc.  All rights reserved.
// A simple TCP server sink (i.e., without RTP or other headers added); one frame per packet
// C++ header
// it supports MJPEG and H.264 video streaming on TCP server port to each connected client
// other media content should also work but this wasn't tested

#ifndef _BASIC_TCP_SERVER_SINK_HH
#define _BASIC_TCP_SERVER_SINK_HH

#ifndef _MEDIA_SINK_HH
#include "MediaSink.hh"
#endif
#ifndef _GROUPSOCK_HH
#include <Groupsock.hh>
#endif

#ifndef REQUEST_BUFFER_SIZE
#define REQUEST_BUFFER_SIZE 20000 // for incoming requests
#endif
#ifndef RESPONSE_BUFFER_SIZE
#define RESPONSE_BUFFER_SIZE 20000
#endif 

class BasicTCPServerSink: public MediaSink {
public:
  static BasicTCPServerSink* createNew(UsageEnvironment& env, Port ourPort = 9001,
				  unsigned maxPayloadSize = 1450);
  Boolean H264;

protected:
  BasicTCPServerSink::BasicTCPServerSink(UsageEnvironment& env,
    int ourSocket, Port ourPort, unsigned maxPayloadSize);
      // called only by createNew()
  virtual ~BasicTCPServerSink();
  void cleanup(); // MUST be called in the destructor of any subclass of us

  static int setUpOurSocket(UsageEnvironment& env, Port& ourPort);

  static void incomingConnectionHandler(void*, int /*mask*/);
  void incomingConnectionHandler();
  void incomingConnectionHandlerOnSocket(int serverSocket);
  void stopTCPStreamingOnSocket(int socketNum);

public: // should be protected, but some old compilers complain otherwise
  // The state of a TCP connection used by a client:
  class ClientConnection {
  protected:
    ClientConnection(BasicTCPServerSink& ourServer, int clientSocket, struct sockaddr_in clientAddr);

    virtual ~ClientConnection();

    UsageEnvironment& envir() { return fOurServer.envir(); }
    void closeSockets();

    static void incomingRequestHandler(void*, int /*mask*/);
    void incomingRequestHandler();
    virtual void handleRequestBytes(int newBytesRead);
    void resetRequestBuffer();
    void closeSocketsTCPServer();
    

  protected:
    friend class BasicTCPServerSink;
//    virtual ClientConnection* createNewClientConnection(int clientSocket, struct sockaddr_in clientAddr) = 0;
    BasicTCPServerSink& fOurServer;
    Groupsock* fGS;
    int fOurSocket;
    int& fClientInputSocket; // aliased to ::fOurSocket
    int fClientOutputSocket;
    Boolean fIsActive;

    struct sockaddr_in fClientAddr;
    unsigned char fRequestBuffer[REQUEST_BUFFER_SIZE];
    unsigned char fResponseBuffer[RESPONSE_BUFFER_SIZE];
    unsigned fRequestBytesAlreadySeen, fRequestBufferBytesLeft;
  };

protected:
  ClientConnection* createNewClientConnection(int clientSocket, struct sockaddr_in clientAddr);

private: // redefined virtual functions:
  virtual Boolean continuePlaying();

private:
  void continuePlaying1();

  static void afterGettingFrame(void* clientData, unsigned frameSize,
				unsigned numTruncatedBytes,
				struct timeval presentationTime,
				unsigned durationInMicroseconds);
  void afterGettingFrame1(unsigned frameSize, unsigned numTruncatedBytes, struct timeval presentationTime,
			  unsigned durationInMicroseconds);

  static void sendNext(void* firstArg);

private:
  Port fServerPort;
  int fServerSocket;
  Groupsock* fGS;
  unsigned fMaxPayloadSize;
  unsigned char* fOutputBuffer;
  struct timeval fNextSendTime;

private:
  HashTable* fServerMediaSessions; // maps 'stream name' strings to "ServerMediaSession" objects
  HashTable* fClientConnections; // the "ClientConnection" objects that we're using
  HashTable* fClientSessions; // maps 'session id' strings to "ClientSession" objects
};

#endif
