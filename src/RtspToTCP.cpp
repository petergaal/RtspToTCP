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
// RtspToTCP - this program is a converter from RTSP protocol (which is used on CCTV IP cameras)
//             to TCP server, where more clients can connect and receive data in a simple format.
//             Usually you can feed directly the stream to another component which is able
//             to decode the video stream. It supports MJPEG and H.264 streams (both were tested).
//             All streams are generated without timing headers as it is usual in a CCTV industry.
// it has been modified from live555 demo applications to support this solution


#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "BasicTCPServerSink.h"

// Forward function definitions:

void checkSessionTimeoutBrokenServer(void* clientData);

// RTSP 'response handlers':
void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString);

// Other event handler functions:
void subsessionAfterPlaying(void* clientData); // called when a stream's subsession (e.g., audio or video substream) ends
void subsessionByeHandler(void* clientData); // called when a RTCP "BYE" is received for a subsession
void streamTimerHandler(void* clientData);
  // called at the end of a stream's expected duration (if the stream has not already signaled its end using a RTCP "BYE")

// The main streaming routine (for each "rtsp://" URL):
void openURL(UsageEnvironment& env, char const* progName, char const* rtspURL);

// Used to iterate through each stream's 'subsessions', setting up each one:
void setupNextSubsession(RTSPClient* rtspClient);

// Used to shut down and close a stream (including its "RTSPClient" object):
void shutdownStream(RTSPClient* rtspClient, int exitCode = 1);

// A function that outputs a string that identifies each stream (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const RTSPClient& rtspClient) {
  return env << "[URL:\"" << rtspClient.url() << "\"]: ";
}

// A function that outputs a string that identifies each subsession (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const MediaSubsession& subsession) {
  return env << subsession.mediumName() << "/" << subsession.codecName();
}

void shutdown(int exitCode = 1);

char eventLoopWatchVariable = 0;

// Authenticator object
Authenticator* ourAuthenticator = NULL;

Boolean controlConnectionUsesTCP = True;
Boolean streamUsingTCP = False;
//Boolean forceMulticastOnUnspecified = False;
Boolean sendKeepAlivesToBrokenServers = False;
Boolean waitForResponseToTEARDOWN = True;

char* username = NULL;
char* password = NULL;
char* userAgent = NULL;
char const* streamURL = NULL;
char const* progName = NULL;
MediaSession* session = NULL;
Medium* ourClient = NULL;
RTSPClient* globalRTSPClient = NULL;
portNumBits tcpServerPort = 9001;

TaskToken sessionTimeoutBrokenServerTask = NULL;
unsigned sessionTimeoutParameter = 0;
UsageEnvironment* env;

/*
void usage(UsageEnvironment& env, char const* progName) {
  env << "Usage: " << progName << " <rtsp-url>\n";
  env << "\t(where <rtsp-url> is a \"rtsp://\" URL)\n";
}
*/

Boolean areAlreadyShuttingDown = False;
int shutdownExitCode;

void usage() {
  *env << "Usage: " << progName
    << (controlConnectionUsesTCP ? " [-t]" : "")
    << " [-u <username> <password>"
    << " [-g user-agent]"
    << " [-p tcp-server-port]"
    << " [-K]"
    << " <url>\n";
  shutdown();
}

void closeMediaSinks() {

  if (session == NULL) return;
  MediaSubsessionIterator iter(*session);
  MediaSubsession* subsession;
  while ((subsession = iter.next()) != NULL) {
    Medium::close(subsession->sink);
    subsession->sink = NULL;
  }
}

void continueAfterTEARDOWN(RTSPClient*, int /*resultCode*/, char* resultString) {
  delete[] resultString;

  // Now that we've stopped any more incoming data from arriving, close our output files:
  closeMediaSinks();
  Medium::close(session);

  // Finally, shut down our client:
  delete ourAuthenticator;
  Medium::close(ourClient);

  // Adios...
  exit(shutdownExitCode);
}

void tearDownSession(MediaSession* session, RTSPClient::responseHandler* afterFunc) {
  globalRTSPClient->sendTeardownCommand(*session, afterFunc, ourAuthenticator);
}

void shutdown(int exitCode) {
  if (areAlreadyShuttingDown) return; // in case we're called after receiving a RTCP "BYE" while in the middle of a "TEARDOWN".
  areAlreadyShuttingDown = True;

  shutdownExitCode = exitCode;
  if (env != NULL) {
    //env->taskScheduler().unscheduleDelayedTask(periodicFileOutputTask);
    //env->taskScheduler().unscheduleDelayedTask(sessionTimerTask);
    env->taskScheduler().unscheduleDelayedTask(sessionTimeoutBrokenServerTask);
    //env->taskScheduler().unscheduleDelayedTask(arrivalCheckTimerTask);
    //env->taskScheduler().unscheduleDelayedTask(interPacketGapCheckTimerTask);
    //env->taskScheduler().unscheduleDelayedTask(qosMeasurementTimerTask);
  }

  //if (qosMeasurementIntervalMS > 0) {
  //  printQOSData(exitCode);
  //}

  // Teardown, then shutdown, any outstanding RTP/RTCP subsessions
  Boolean shutdownImmediately = True; // by default
  if (session != NULL) {
    RTSPClient::responseHandler* responseHandlerForTEARDOWN = NULL; // unless:
    if (waitForResponseToTEARDOWN) {
      shutdownImmediately = False;
      responseHandlerForTEARDOWN = continueAfterTEARDOWN;
    }
    tearDownSession(session, responseHandlerForTEARDOWN);
  }

  if (shutdownImmediately) continueAfterTEARDOWN(NULL, 0, NULL);
}

int main(int argc, char** argv) {
  // Begin by setting up our usage environment:
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  env = BasicUsageEnvironment::createNew(*scheduler);

  progName = argv[0];
  // We need at least one "rtsp://" URL argument:
  if (argc < 2) {
    usage();
    return 1;
  }



  while (argc > 1) {
    char* const opt = argv[1];
    if (opt[0] != '-') {
      if (argc == 2) break; // only the URL is left
      usage();
    }

    switch (opt[1]) {
    case 't': {
      // stream RTP and RTCP over the TCP 'control' connection
      if (controlConnectionUsesTCP) {
        streamUsingTCP = True;
      }
      else {
        usage();
      }
      break;
    }

    case 'u': { // specify a username and password
      if (argc < 4) usage(); // there's no argv[3] (for the "password")
      username = argv[2];
      password = argv[3];
      argv += 2; argc -= 2;

      ourAuthenticator = new Authenticator(username, password);
      break;
    }

    case 'K': { // Send periodic 'keep-alive' requests to keep broken server sessions alive
      sendKeepAlivesToBrokenServers = True;
      break;
    }

    case 'g': { // specify a user agent name to use in outgoing requests
      userAgent = argv[2];
      ++argv; --argc;
      break;
    }

    case 'p': {
      if (argc > 3 && argv[2][0] != '-') {
        // The next argument is the HTTP server port number:
        if (sscanf(argv[2], "%hu", &tcpServerPort) == 1
          && tcpServerPort > 0) {
          ++argv; --argc;
          break;
        }
      }

      // If we get here, the option was specified incorrectly:
      usage();
      break;
    }

    default: {
      *env << "Invalid option: " << opt << "\n";
      usage();
      break;
    }
    }

    ++argv; --argc;
  }

  streamURL = argv[1];

  // There are argc-1 URLs: argv[1] through argv[argc-1].  Open and start streaming each one:
//  for (int i = 1; i <= argc-1; ++i) {
//    openURL(*env, argv[0], argv[i]);
//  }

  openURL(*env, progName, streamURL);

  // All subsequent activity takes place within the event loop:
  env->taskScheduler().doEventLoop(&eventLoopWatchVariable);
    // This function call does not return, unless, at some point in time, "eventLoopWatchVariable" gets set to something non-zero.

  return 0;

  // If you choose to continue the application past this point (i.e., if you comment out the "return 0;" statement above),
  // and if you don't intend to do anything more with the "TaskScheduler" and "UsageEnvironment" objects,
  // then you can also reclaim the (small) memory used by these objects by uncommenting the following code:
  /*
    env->reclaim(); env = NULL;
    delete scheduler; scheduler = NULL;
  */
}

// Define a class to hold per-stream state that we maintain throughout each stream's lifetime:

class StreamClientState {
public:
  StreamClientState();
  virtual ~StreamClientState();

public:
  MediaSubsessionIterator* iter;
  MediaSession* session;
  MediaSubsession* subsession;
  TaskToken streamTimerTask;
  double duration;
};

// If you're streaming just a single stream (i.e., just from a single URL, once), then you can define and use just a single
// "StreamClientState" structure, as a global variable in your application.  However, because - in this demo application - we're
// showing how to play multiple streams, concurrently, we can't do that.  Instead, we have to have a separate "StreamClientState"
// structure for each "RTSPClient".  To do this, we subclass "RTSPClient", and add a "StreamClientState" field to the subclass:

class ourRTSPClient: public RTSPClient {
public:
  static ourRTSPClient* createNew(UsageEnvironment& env, char const* rtspURL,
				  int verbosityLevel = 0,
				  char const* applicationName = NULL,
				  portNumBits tunnelOverHTTPPortNum = 0);

protected:
  ourRTSPClient(UsageEnvironment& env, char const* rtspURL,
		int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum);
    // called only by createNew();
  virtual ~ourRTSPClient();

public:
  StreamClientState scs;
};

// Define a data sink (a subclass of "MediaSink") to receive the data for each subsession (i.e., each audio or video 'substream').
// In practice, this might be a class (or a chain of classes) that decodes and then renders the incoming audio or video.
// Or it might be a "FileSink", for outputting the received data into a file (as is done by the "openRTSP" application).
// In this example code, however, we define a simple 'dummy' sink that receives incoming data, but does nothing with it.


#define RTSP_CLIENT_VERBOSITY_LEVEL 1 // by default, print verbose output from each "RTSPClient"

static unsigned rtspClientCount = 0; // Counts how many streams (i.e., "RTSPClient"s) are currently in use.


void getOptions(RTSPClient::responseHandler* afterFunc) {
  globalRTSPClient->sendOptionsCommand(afterFunc, ourAuthenticator);
}


void openURL(UsageEnvironment& env, char const* progName, char const* rtspURL) {
  // Begin by creating a "RTSPClient" object.  Note that there is a separate "RTSPClient" object for each stream that we wish
  // to receive (even if more than stream uses the same "rtsp://" URL).
  RTSPClient* rtspClient = ourRTSPClient::createNew(env, rtspURL, RTSP_CLIENT_VERBOSITY_LEVEL, progName);
  if (rtspClient == NULL) {
    env << "Failed to create a RTSP client for URL \"" << rtspURL << "\": " << env.getResultMsg() << "\n";
    return;
  }

  if (rtspClientCount == 0) {
    globalRTSPClient = rtspClient;
  }

  ++rtspClientCount;

  // Next, send a RTSP "DESCRIBE" command, to get a SDP description for the stream.
  // Note that this command - like all RTSP commands - is sent asynchronously; we do not block, waiting for a response.
  // Instead, the following function call returns immediately, and we handle the RTSP response later, from within the event loop:
  rtspClient->sendDescribeCommand(continueAfterDESCRIBE, ourAuthenticator);
}


// Implementation of the RTSP 'response handlers':

void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString) {
  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to get a SDP description: " << resultString << "\n";
      delete[] resultString;
      break;
    }

    char* const sdpDescription = resultString;
    env << *rtspClient << "Got a SDP description:\n" << sdpDescription << "\n";

    // Create a media session object from this SDP description:
    scs.session = MediaSession::createNew(env, sdpDescription);
    session = scs.session;

    delete[] sdpDescription; // because we don't need it anymore
    if (scs.session == NULL) {
      env << *rtspClient << "Failed to create a MediaSession object from the SDP description: " << env.getResultMsg() << "\n";
      break;
    } else if (!scs.session->hasSubsessions()) {
      env << *rtspClient << "This session has no media subsessions (i.e., no \"m=\" lines)\n";
      break;
    }

    // Then, create and set up our data source objects for the session.  We do this by iterating over the session's 'subsessions',
    // calling "MediaSubsession::initiate()", and then sending a RTSP "SETUP" command, on each one.
    // (Each 'subsession' will have its own data source.)
    scs.iter = new MediaSubsessionIterator(*scs.session);
    setupNextSubsession(rtspClient);
    return;
  } while (0);

  // An unrecoverable error occurred with this stream.
  shutdownStream(rtspClient);
}

// By default, we request that the server stream its data using RTP/UDP.
// If, instead, you want to request that the server stream via RTP-over-TCP, change the following to True:
//#define REQUEST_STREAMING_OVER_TCP False
//#define REQUEST_STREAMING_OVER_TCP True

void setupNextSubsession(RTSPClient* rtspClient) {
  UsageEnvironment& env = rtspClient->envir(); // alias
  StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias
  
  scs.subsession = scs.iter->next();
  if (scs.subsession != NULL) {
    if (!scs.subsession->initiate()) {
      env << *rtspClient << "Failed to initiate the \"" << *scs.subsession << "\" subsession: " << env.getResultMsg() << "\n";
      setupNextSubsession(rtspClient); // give up on this subsession; go to the next one
    } else {
      env << *rtspClient << "Initiated the \"" << *scs.subsession << "\" subsession (";
      if (scs.subsession->rtcpIsMuxed()) {
	env << "client port " << scs.subsession->clientPortNum();
      } else {
	env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum()+1;
      }
      env << ")\n";

      // Continue setting up this subsession, by sending a RTSP "SETUP" command:
      rtspClient->sendSetupCommand(*scs.subsession, continueAfterSETUP, False, streamUsingTCP);
    }
    return;
  }

  // We've finished setting up all of the subsessions.  Now, send a RTSP "PLAY" command to start the streaming:
  if (scs.session->absStartTime() != NULL) {
    // Special case: The stream is indexed by 'absolute' time, so send an appropriate "PLAY" command:
    rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY, scs.session->absStartTime(), scs.session->absEndTime());
  } else {
    scs.duration = scs.session->playEndTime() - scs.session->playStartTime();
    rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY);
  }
}

void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString) {
  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to set up the \"" << *scs.subsession << "\" subsession: " << resultString << "\n";
      break;
    }

    env << *rtspClient << "Set up the \"" << *scs.subsession << "\" subsession (";
    if (scs.subsession->rtcpIsMuxed()) {
      env << "client port " << scs.subsession->clientPortNum();
    } else {
      env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum()+1;
    }
    env << ")\n";

    // Having successfully setup the subsession, create a data sink for it, and call "startPlaying()" on it.
    // (This will prepare the data sink to receive data; the actual flow of data from the client won't start happening until later,
    // after we've sent a RTSP "PLAY" command.)

    if (strcmp(scs.subsession->mediumName(), "video") == 0) {
      if ( (strcmp(scs.subsession->codecName(), "H264") == 0) || (strcmp(scs.subsession->codecName(), "JPEG") == 0) ) {

        scs.subsession->sink = BasicTCPServerSink::createNew(env, tcpServerPort, 1024 * 1024);
        // perhaps use your own custom "MediaSink" subclass instead
        if (scs.subsession->sink == NULL) {
          env << *rtspClient << "Failed to create a data sink for the \"" << *scs.subsession
            << "\" subsession: " << env.getResultMsg() << "\n";
          break;
        }
        
        if (strcmp(scs.subsession->codecName(), "H264") == 0) {
          BasicTCPServerSink *sink_h264 = (BasicTCPServerSink *)scs.subsession->sink;
          sink_h264->H264 = True;
        }

        env << *rtspClient << "Created a data sink for the \"" << *scs.subsession << "\" subsession\n";
        scs.subsession->miscPtr = rtspClient; // a hack to let subsession handler functions get the "RTSPClient" from the subsession 
        scs.subsession->sink->startPlaying(*(scs.subsession->readSource()),
          subsessionAfterPlaying, scs.subsession);
        // Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
        if (scs.subsession->rtcpInstance() != NULL) {
          scs.subsession->rtcpInstance()->setByeHandler(subsessionByeHandler, scs.subsession);
        }
      }
    }
  } while (0);
  delete[] resultString;

  // Set up the next subsession, if any:
  setupNextSubsession(rtspClient);
}

void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString) {
  Boolean success = False;

  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to start playing session: " << resultString << "\n";
      break;
    }

    // Set a timer to be handled at the end of the stream's expected duration (if the stream does not already signal its end
    // using a RTCP "BYE").  This is optional.  If, instead, you want to keep the stream active - e.g., so you can later
    // 'seek' back within it and do another RTSP "PLAY" - then you can omit this code.
    // (Alternatively, if you don't want to receive the entire stream, you could set this timer for some shorter value.)
    if (scs.duration > 0) {
      unsigned const delaySlop = 2; // number of seconds extra to delay, after the stream's expected duration.  (This is optional.)
      scs.duration += delaySlop;
      unsigned uSecsToDelay = (unsigned)(scs.duration*1000000);
      scs.streamTimerTask = env.taskScheduler().scheduleDelayedTask(uSecsToDelay, (TaskFunc*)streamTimerHandler, rtspClient);
    }

    env << *rtspClient << "Started playing session";
    if (scs.duration > 0) {
      env << " (for up to " << scs.duration << " seconds)";
    }
    env << "...\n";

    success = True;
  } while (0);
  delete[] resultString;

  if (!success) {
    // An unrecoverable error occurred with this stream.
    shutdownStream(rtspClient);
    return;
  }

  sessionTimeoutBrokenServerTask = NULL;
  checkSessionTimeoutBrokenServer(NULL);
}


// Implementation of the other event handlers:

void subsessionAfterPlaying(void* clientData) {
  MediaSubsession* subsession = (MediaSubsession*)clientData;
  RTSPClient* rtspClient = (RTSPClient*)(subsession->miscPtr);

  // Begin by closing this subsession's stream:
  Medium::close(subsession->sink);
  subsession->sink = NULL;

  // Next, check whether *all* subsessions' streams have now been closed:
  MediaSession& session = subsession->parentSession();
  MediaSubsessionIterator iter(session);
  while ((subsession = iter.next()) != NULL) {
    if (subsession->sink != NULL) return; // this subsession is still active
  }

  // All subsessions' streams have now been closed, so shutdown the client:
  shutdownStream(rtspClient);
}

void subsessionByeHandler(void* clientData) {
  MediaSubsession* subsession = (MediaSubsession*)clientData;
  RTSPClient* rtspClient = (RTSPClient*)subsession->miscPtr;
  UsageEnvironment& env = rtspClient->envir(); // alias

  env << *rtspClient << "Received RTCP \"BYE\" on \"" << *subsession << "\" subsession\n";

  // Now act as if the subsession had closed:
  subsessionAfterPlaying(subsession);
}

void streamTimerHandler(void* clientData) {
  ourRTSPClient* rtspClient = (ourRTSPClient*)clientData;
  StreamClientState& scs = rtspClient->scs; // alias

  scs.streamTimerTask = NULL;

  // Shut down the stream:
  shutdownStream(rtspClient);
}

void shutdownStream(RTSPClient* rtspClient, int exitCode) {
  UsageEnvironment& env = rtspClient->envir(); // alias
  StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

  // First, check whether any subsessions have still to be closed:
  if (scs.session != NULL) { 
    Boolean someSubsessionsWereActive = False;
    MediaSubsessionIterator iter(*scs.session);
    MediaSubsession* subsession;

    while ((subsession = iter.next()) != NULL) {
      if (subsession->sink != NULL) {
	Medium::close(subsession->sink);
	subsession->sink = NULL;

	if (subsession->rtcpInstance() != NULL) {
	  subsession->rtcpInstance()->setByeHandler(NULL, NULL); // in case the server sends a RTCP "BYE" while handling "TEARDOWN"
	}

	someSubsessionsWereActive = True;
      }
    }

    if (someSubsessionsWereActive) {
      // Send a RTSP "TEARDOWN" command, to tell the server to shutdown the stream.
      // Don't bother handling the response to the "TEARDOWN".
      rtspClient->sendTeardownCommand(*scs.session, NULL);
    }
  }

  env << *rtspClient << "Closing the stream.\n";
  Medium::close(rtspClient);
    // Note that this will also cause this stream's "StreamClientState" structure to get reclaimed.

  if (--rtspClientCount == 0) {
    // The final stream has ended, so exit the application now.
    // (Of course, if you're embedding this code into your own application, you might want to comment this out,
    // and replace it with "eventLoopWatchVariable = 1;", so that we leave the LIVE555 event loop, and continue running "main()".)
    exit(exitCode);
  }
}


// Implementation of "ourRTSPClient":

ourRTSPClient* ourRTSPClient::createNew(UsageEnvironment& env, char const* rtspURL,
					int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum) {
  return new ourRTSPClient(env, rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum);
}

ourRTSPClient::ourRTSPClient(UsageEnvironment& env, char const* rtspURL,
			     int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum)
  : RTSPClient(env,rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum, -1) {
}

ourRTSPClient::~ourRTSPClient() {
}


// Implementation of "StreamClientState":

StreamClientState::StreamClientState()
  : iter(NULL), session(NULL), subsession(NULL), streamTimerTask(NULL), duration(0.0) {
}

StreamClientState::~StreamClientState() {
  delete iter;
  if (session != NULL) {
    // We also need to delete "session", and unschedule "streamTimerTask" (if set)
    UsageEnvironment& env = session->envir(); // alias

    env.taskScheduler().unscheduleDelayedTask(streamTimerTask);
    Medium::close(session);
  }
}

void checkSessionTimeoutBrokenServer(void* ) {
  if (!sendKeepAlivesToBrokenServers) return; // we're not checking

                                              // Send an "OPTIONS" request, starting with the second call
  if (sessionTimeoutBrokenServerTask != NULL) {
    getOptions(NULL);
  }

  unsigned sessionTimeout = sessionTimeoutParameter == 0 ? 60 : sessionTimeoutParameter;
  unsigned secondsUntilNextKeepAlive = sessionTimeout <= 5 ? 1 : sessionTimeout - 5;
  // Reduce the interval a little, to be on the safe side

  sessionTimeoutBrokenServerTask
    = env->taskScheduler().scheduleDelayedTask(secondsUntilNextKeepAlive * 1000000,
    (TaskFunc*)checkSessionTimeoutBrokenServer, NULL);

}

