# RtspToTCP
---

This is a protocol bridge between RTSP and TCP running as a command line application. This program acts as an RTSP client and feeds the stream into TCP server. It was made to help debug problems with IP CCTV cameras (RTSP / ONVIF compatible).
The program is using Live555 library and I extended the functionality to make this tool.

If you run it without parameters the program will print out all the parameters:
```
Usage: RtspToTcp.exe [-t] [-u <username> <password>] [-g user-agent] [-p tcp-server-port] [-K] <url>
```

The program will request at least one parameter as an RTSP URL. Other parameters are not mandatory.

Parameters explained:  
`-t`: Stream RTP and RTCP over the TCP 'control' connection (by default UDP is used). Useful if you are not on the same network (typically behind NAT) or you need to deliver frames without any loss in the data.  
`-u <username> <password>`: When the RTSP source is protected by password you need to use this parameter (RTSP server returns 401 error without username and password)  
`-g user-agent`: Supply an own user-agent string  
`-K`: Send periodic 'keep-alive' requests to keep broken server sessions alive  
`<url>`: Has to be supplied as a last parameter which is the RTSP URL for the video source. This is a mandatory parameter.  
`-p tcp-server-port`: Specifies a TCP server port number, by default it is 9001 if you don't use this parameter.

Not everything has been tested but it should work. I didn't test -K and -g parameters.

## How to compile
---

This program was compiled in Visual Studio 2015. All the Visual Studio related files are in vs2015 folder. The original Live555 library is in the live folder (version 2016.11.28, latest version of live555 source code is [here](http://www.live555.com/liveMedia/public/)). In the src folder there are my modifications (BasicTCPServerSink.cpp, BasicTCPServerSink.h, RtspToTCP.cpp). Together it will make this program. To compile and build the project, should be enough to open the Visual Studio solution file (RtspToTcp.sln) and build it.  
During the development I found a bug in Visual Studio linker (VS2015 Update 3, latest updates as it was at 27th of January 2017). So there is a switch to /LTCG instead of the default /LTCG:incremental, otherwise it won't build the project in x86 Release mode. I used /MT instead of /MD switch so you shouldn't need to install C++ Redistributable libraries (works on a default Windows installation, also on Windows XP).

### Linux support
---
The program should work also in Linux environment, however this was not tested by me. You will need probably to make a makefile for this. The Live555 media works fine on Linux so this program should work too. I tried to avoid any Windows specific functions. If you want to make it work on Linux send me the makefile and I will definitely include this into the project. Probably will make it by myself some time later.

## Feedback
---
Any feedback will be appreciated. If you find a bug, contact me over GitHub (open an Issue or different way). You can contact me also using email on peter.gaal.sk [at] gmail dot com.

## Source code documentation
---
Because the original Live555 doesn't have a class for TCP server (which just broadcasts one media sub-session) I made a new class for this - named BasicTCPServerSink. It uses a lot of things from BasicUDPSink class, other parts are from GenericMediaServer and RTSPServer classes (as a TCP server is used in these classes).  
Then a test programs were modified to make RtspToTCP program. Originally I used testRTSPClient.cpp and then some code was applied from openRTSP.cpp and playCommon.cpp to make it work using command line parameters.  
The code will need some polishing and some things might be removed from it but it works with this current state (I needed to make this very quickly for debugging one problem). I published it because it might be useful for other people who work with RTSP and IP CCTV cameras.
