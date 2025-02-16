/**
 * seed.cpp
 *
 * This program implements a Seed Node for the Gossip Protocol-based P2P
 * network. The seed node listens for incoming connections from peers. When a
 * peer sends a "Register" message, the seed registers that peer (if not already
 * registered) and sends back its current list of peers (each as IP:Port:degree;
 * separated by semicolons). It also handles "UpdateDegree" and "DeadNode"
 * messages.
 *
 * Compile with:
 *    g++ -std=c++11 -pthread seed.cpp -o seed
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

// Data structure representing a registered peer.
struct PeerRecord
{
  std::string ip;
  int port;
  int degree;

  PeerRecord (const std::string &ipAddr, int portNum, int deg = 0)
    : ip (ipAddr), port (portNum), degree (deg)
  {}
};

class SeedNode
{
public:
  SeedNode (const std::string &ip, int port)
    : seedIP (ip), seedPort (port), serverSock (-1)
  {}

  // Start the seed server: bind, listen, and spawn threads for each incoming
  // connection.
  void start ()
  {
    // Create server socket.
    serverSock = socket (AF_INET, SOCK_STREAM, 0);
    if (serverSock < 0)
      {
	perror ("Seed: Socket creation failed");
	exit (EXIT_FAILURE);
      }
    int opt = 1;
    if (setsockopt (serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt))
	< 0)
      {
	perror ("Seed: setsockopt failed");
	exit (EXIT_FAILURE);
      }

    // Bind the socket.
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr (seedIP.c_str ());
    serverAddr.sin_port = htons (seedPort);
    if (bind (serverSock, (struct sockaddr *) &serverAddr, sizeof (serverAddr))
	< 0)
      {
	perror ("Seed: Bind failed");
	exit (EXIT_FAILURE);
      }

    // Listen for incoming connections.
    if (listen (serverSock, 10) < 0)
      {
	perror ("Seed: Listen failed");
	exit (EXIT_FAILURE);
      }
    log ("Seed node listening on " + seedIP + ":" + std::to_string (seedPort));

    // Main accept loop.
    while (true)
      {
	sockaddr_in clientAddr;
	socklen_t addrLen = sizeof (clientAddr);
	int clientSock
	  = accept (serverSock, (struct sockaddr *) &clientAddr, &addrLen);
	if (clientSock < 0)
	  {
	    perror ("Seed: Accept failed");
	    continue;
	  }
	std::thread (&SeedNode::handleConnection, this, clientSock).detach ();
      }
    close (serverSock);
  }

private:
  std::string seedIP;
  int seedPort;
  int serverSock;
  std::vector<PeerRecord> peerList;
  std::mutex peerMutex;

  // Utility: Get current timestamp string.
  std::string currentTimestamp ()
  {
    std::time_t now = std::time (nullptr);
    std::tm *ltm = std::localtime (&now);
    std::ostringstream oss;
    oss << std::put_time (ltm, "%d-%m-%Y %H-%M-%S");
    return oss.str ();
  }

  // Utility: Log message to console and append to a file.
  void log (const std::string &message)
  {
    std::string timeStr = currentTimestamp ();
    std::string logMsg = "[" + timeStr + "] " + message;
    std::cout << logMsg << std::endl;
    std::ofstream outfile ("seed-" + std::to_string (seedPort) + ".log",
			   std::ios::app);
    if (outfile)
      {
	outfile << logMsg << std::endl;
      }
  }

  // Utility: Format the current peer list into a single string.
  std::string formatPeerList ()
  {
    std::lock_guard<std::mutex> lock (peerMutex);
    std::ostringstream oss;
    for (const auto &peer : peerList)
      {
	oss << peer.ip << ":" << peer.port << ":" << peer.degree << ";";
      }
    return oss.str ();
  }

  // Main handler for an incoming connection.
  void handleConnection (int clientSock)
  {
    char buffer[1024];
    std::string dataBuffer;
    while (true)
      {
	ssize_t bytesRead = read (clientSock, buffer, sizeof (buffer) - 1);
	if (bytesRead <= 0)
	  break;
	buffer[bytesRead] = '\0';
	dataBuffer.append (buffer);
	// Process complete messages (terminated by '\n').
	size_t pos;
	while ((pos = dataBuffer.find ('\n')) != std::string::npos)
	  {
	    std::string msg = dataBuffer.substr (0, pos);
	    dataBuffer.erase (0, pos + 1);
	    processMessage (msg, clientSock);
	  }
      }
    close (clientSock);
  }

  // Process a single message from a peer.
  void processMessage (const std::string &msg, int clientSock)
  {
    std::istringstream iss (msg);
    std::string command;
    std::getline (iss, command, ':');

    if (command == "Register")
      {
	// Expected format: Register:<peerIP>:<peerPort>:<degree>
	std::string peerIP, peerPortStr, degreeStr;
	std::getline (iss, peerIP, ':');
	std::getline (iss, peerPortStr, ':');
	std::getline (iss, degreeStr, ':');
	int peerPort = std::stoi (peerPortStr);
	int degree = degreeStr.empty () ? 0 : std::stoi (degreeStr);

	{
	  std::lock_guard<std::mutex> lock (peerMutex);
	  bool exists = false;
	  for (auto &rec : peerList)
	    {
	      if (rec.ip == peerIP && rec.port == peerPort)
		{
		  exists = true;
		  break;
		}
	    }
	  if (!exists)
	    {
	      peerList.emplace_back (peerIP, peerPort, degree);
	      log ("Registered new peer: " + peerIP + ":" + peerPortStr
		   + " (degree: " + std::to_string (degree) + ")");
	    }
	}
	// Immediately respond with the current peer list.
	std::string response = formatPeerList ();
	send (clientSock, response.c_str (), response.size (), 0);
      }
    else if (command == "UpdateDegree")
      {
	// Expected format: UpdateDegree:<peerIP>:<peerPort>
	std::string peerIP, peerPortStr;
	std::getline (iss, peerIP, ':');
	std::getline (iss, peerPortStr, ':');
	int peerPort = std::stoi (peerPortStr);
	{
	  std::lock_guard<std::mutex> lock (peerMutex);
	  for (auto &rec : peerList)
	    {
	      if (rec.ip == peerIP && rec.port == peerPort)
		{
		  rec.degree++;
		  log ("Updated degree for peer: " + peerIP + ":" + peerPortStr
		       + " to " + std::to_string (rec.degree));
		  break;
		}
	    }
	}
      }
    else if (command == "DeadNode")
      {
	// Expected format:
	// DeadNode:<deadIP>:<deadPort>:<timestamp>:<reporterIP>:<reporterPort>
	std::string deadIP, deadPortStr, timestamp, reporterIP, reporterPortStr;
	std::getline (iss, deadIP, ':');
	std::getline (iss, deadPortStr, ':');
	std::getline (iss, timestamp, ':');
	std::getline (iss, reporterIP, ':');
	std::getline (iss, reporterPortStr, ':');
	int deadPort = std::stoi (deadPortStr);
	{
	  std::lock_guard<std::mutex> lock (peerMutex);
	  auto it = std::remove_if (peerList.begin (), peerList.end (),
				    [deadIP, deadPort] (const PeerRecord &rec) {
				      return (rec.ip == deadIP
					      && rec.port == deadPort);
				    });
	  if (it != peerList.end ())
	    {
	      peerList.erase (it, peerList.end ());
	      log ("Removed dead peer: " + deadIP + ":" + deadPortStr
		   + " reported at " + timestamp);
	    }
	}
      }
    else
      {
	log ("Unknown command received: " + msg);
      }
  }
};

int
main (int argc, char *argv[])
{
  if (argc != 3)
    {
      std::cerr << "Usage: ./seed <SeedIP> <SeedPort>" << std::endl;
      return EXIT_FAILURE;
    }
  std::string ip = argv[1];
  int port = std::stoi (argv[2]);
  SeedNode seed (ip, port);
  seed.start ();
  return EXIT_SUCCESS;
}