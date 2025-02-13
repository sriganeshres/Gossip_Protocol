#include "common.hpp"

class PeerNode
{
private:
  Node self;
  std::vector<Node> seedNodes;
  std::set<Node> connectedPeers;
  std::map<std::string, std::set<Node>>
    messageList; // Hash -> set of peers that have seen the message
  std::unique_ptr<Logger> logger;
  std::mutex peersMutex;
  std::mutex messageMutex;
  PowerLawDegreeGenerator degreeGen;
  ThreadSafeQueue<std::pair<std::string, Node>> messageQueue;

  int serverSocket;
  bool running;
  int messageCount;
  std::vector<std::thread> workerThreads;
  std::map<Node, int> missedPings;
  std::map<Node, int> peerSockets;

  void loadConfig (const std::string &configFile)
  {
    std::ifstream file (configFile);
    if (!file)
      {
	throw std::runtime_error ("Unable to open config file: " + configFile);
      }

    std::string line;
    while (std::getline (file, line))
      {
	std::istringstream iss (line);
	std::string ip;
	int port;

	std::getline (iss, ip, ':');
	iss >> port;

	seedNodes.push_back ({ip, port});
      }

    if (seedNodes.empty ())
      {
	throw std::runtime_error ("No seed nodes found in config file");
      }
  }

  void initializeSocket ()
  {
    serverSocket = socket (AF_INET, SOCK_STREAM, 0);
    if (serverSocket == -1)
      {
	throw NetworkError ("Failed to create socket");
      }

    int opt = 1;
    if (setsockopt (serverSocket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
		    sizeof (opt)))
      {
	throw NetworkError ("Failed to set socket options");
      }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons (self.port);

    if (bind (serverSocket, (struct sockaddr *) &address, sizeof (address)) < 0)
      {
	throw NetworkError ("Failed to bind socket");
      }

    if (listen (serverSocket, SOMAXCONN) < 0)
      {
	throw NetworkError ("Failed to listen on socket");
      }
  }

  void connectToSeeds ()
  {
    // Calculate minimum required seed connections
    int minSeedConnections = (seedNodes.size () / 2) + 1;

    // Randomly shuffle seed nodes
    std::vector<Node> shuffledSeeds = seedNodes;
    std::random_device rd;
    std::mt19937 gen (rd ());
    std::shuffle (shuffledSeeds.begin (), shuffledSeeds.end (), gen);

    int connectedCount = 0;
    for (const auto &seed : shuffledSeeds)
      {
	if (connectToSeed (seed))
	  {
	    connectedCount++;
	    if (connectedCount >= minSeedConnections)
	      break;
	  }
      }

    if (connectedCount < minSeedConnections)
      {
	throw NetworkError ("Failed to connect to minimum required seed nodes");
      }
  }

  bool connectToSeed (const Node &seed)
  {
    int sockfd = socket (AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
      return false;

    struct sockaddr_in seedAddr;
    seedAddr.sin_family = AF_INET;
    seedAddr.sin_port = htons (seed.port);

    if (inet_pton (AF_INET, seed.ip.c_str (), &seedAddr.sin_addr) <= 0)
      {
	close (sockfd);
	return false;
      }

    if (connect (sockfd, (struct sockaddr *) &seedAddr, sizeof (seedAddr)) < 0)
      {
	close (sockfd);
	return false;
      }

    // Register with seed
    std::string regMsg = Message::createRegistrationMessage (self);
    send (sockfd, regMsg.c_str (), regMsg.length (), 0);

    // Get peer list
    std::string getPeersMsg = "GET_PEERS";
    send (sockfd, getPeersMsg.c_str (), getPeersMsg.length (), 0);

    char buffer[MAX_BUFFER_SIZE];
    ssize_t bytesRead = recv (sockfd, buffer, MAX_BUFFER_SIZE - 1, 0);
    if (bytesRead > 0)
      {
	buffer[bytesRead] = '\0';
	processPeerList (std::string (buffer));
      }

    close (sockfd);
    return true;
  }

  void processPeerList (const std::string &peerListStr)
  {
    std::istringstream iss (peerListStr);
    std::string peerInfo;
    std::vector<Node> availablePeers;

    while (std::getline (iss, peerInfo, ';'))
      {
	if (peerInfo.empty ())
	  continue;

	std::istringstream peerStream (peerInfo);
	std::string ip;
	int port;

	std::getline (peerStream, ip, ':');
	peerStream >> port;

	if (ip != self.ip || port != self.port)
	  {
	    availablePeers.push_back ({ip, port});
	  }
      }

    // Use power-law distribution to determine number of connections
    if (!availablePeers.empty ())
      {
	int maxDegree = std::min (15, (int) availablePeers.size ());
	int degree = degreeGen.generateDegree (2, maxDegree);

	std::shuffle (availablePeers.begin (), availablePeers.end (),
		      std::mt19937 (std::random_device () ()));

	for (int i = 0; i < degree && i < availablePeers.size (); ++i)
	  {
	    connectToPeer (availablePeers[i]);
	  }
      }
  }

  void connectToPeer (const Node &peer)
  {
    int sockfd = socket (AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
      return;

    struct sockaddr_in peerAddr;
    peerAddr.sin_family = AF_INET;
    peerAddr.sin_port = htons (peer.port);

    if (inet_pton (AF_INET, peer.ip.c_str (), &peerAddr.sin_addr) <= 0)
      {
	close (sockfd);
	return;
      }

    if (connect (sockfd, (struct sockaddr *) &peerAddr, sizeof (peerAddr)) < 0)
      {
	close (sockfd);
	return;
      }

    {
      std::lock_guard<std::mutex> lock (peersMutex);
      connectedPeers.insert (peer);
      peerSockets[peer] = sockfd;
      missedPings[peer] = 0;
    }

    logger->log ("Connected to peer: " + peer.ip + ":"
		 + std::to_string (peer.port));
  }

  void generateGossipMessage ()
  {
    while (running && messageCount < MAX_MESSAGES)
      {
	std::this_thread::sleep_for (
	  std::chrono::seconds (MESSAGE_GENERATION_INTERVAL));

	std::string message
	  = Message::createGossipMessage (self, messageCount++);
	broadcastMessage (message, Node{"", -1});

	logger->log ("Generated gossip message: " + message);
      }
  }

  void broadcastMessage (const std::string &message, const Node &sender)
  {
    std::string messageHash = Message::calculateHash (message);

    {
      std::lock_guard<std::mutex> lock (messageMutex);
      if (messageList.find (messageHash) != messageList.end ())
	{
	  return; // Already seen this message
	}
      messageList[messageHash] = {sender};
    }

    logger->log ("Received new message: " + message);

    std::lock_guard<std::mutex> lock (peersMutex);
    for (const auto &peer : connectedPeers)
      {
	if (peer == sender)
	  continue;

	messageQueue.push ({message, peer});
      }
  }

  void messageSender ()
  {
    while (running)
      {
	std::pair<std::string, Node> item;
	messageQueue.wait_and_pop (item);

	auto &[message, peer] = item;
	int sockfd = -1;

	{
	  std::lock_guard<std::mutex> lock (peersMutex);
	  auto it = peerSockets.find (peer);
	  if (it != peerSockets.end ())
	    {
	      sockfd = it->second;
	    }
	}

	if (sockfd != -1)
	  {
	    send (sockfd, message.c_str (), message.length (), 0);
	  }
      }
  }

  void checkPeerLiveness ()
  {
    while (running)
      {
	std::this_thread::sleep_for (std::chrono::seconds (PING_INTERVAL));

	std::vector<Node> peersToCheck;
	{
	  std::lock_guard<std::mutex> lock (peersMutex);
	  for (const auto &peer : connectedPeers)
	    {
	      peersToCheck.push_back (peer);
	    }
	}

	for (const auto &peer : peersToCheck)
	  {
	    if (!pingPeer (peer))
	      {
		handleDeadPeer (peer);
	      }
	  }
      }
  }

  bool pingPeer (const Node &peer)
  {
    std::string cmd = "ping -c 1 -W 2 " + peer.ip;
    int result = system (cmd.c_str ());

    if (result == 0)
      {
	std::lock_guard<std::mutex> lock (peersMutex);
	missedPings[peer] = 0;
	return true;
      }

    std::lock_guard<std::mutex> lock (peersMutex);
    missedPings[peer]++;
    return missedPings[peer] < MAX_MISSED_PINGS;
  }

  void handleDeadPeer (const Node &peer)
  {
    logger->log ("Peer appears to be dead: " + peer.ip + ":"
		 + std::to_string (peer.port));

    // Notify seeds
    for (const auto &seed : seedNodes)
      {
	int sockfd = socket (AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
	  continue;

	struct sockaddr_in seedAddr;
	seedAddr.sin_family = AF_INET;
	seedAddr.sin_port = htons (seed.port);

	if (inet_pton (AF_INET, seed.ip.c_str (), &seedAddr.sin_addr) <= 0)
	  {
	    close (sockfd);
	    continue;
	  }

	if (connect (sockfd, (struct sockaddr *) &seedAddr, sizeof (seedAddr))
	    < 0)
	  {
	    close (sockfd);
	    continue;
	  }

	std::string deadNodeMsg = Message::createDeadNodeMessage (peer, self);
	send (sockfd, deadNodeMsg.c_str (), deadNodeMsg.length (), 0);
	close (sockfd);
      }

    // Remove peer from our lists
    {
      std::lock_guard<std::mutex> lock (peersMutex);
      if (peerSockets.find (peer) != peerSockets.end ())
	{
	  close (peerSockets[peer]);
	  peerSockets.erase (peer);
	}
      connectedPeers.erase (peer);
      missedPings.erase (peer);
    }
  }

public:
  PeerNode (const std::string &ip, int port, const std::string &configFile)
    : self{ip, port}, running (true), messageCount (0),
      logger (std::make_unique<Logger> ("peer_" + ip + "_"
					+ std::to_string (port) + ".log"))
  {
    loadConfig (configFile);
    initializeSocket ();
  }

  void start ()
  {
    logger->log ("Starting peer node on " + self.ip + ":"
		 + std::to_string (self.port));

    try
      {
	connectToSeeds ();
      }
    catch (const NetworkError &e)
      {
	logger->log ("Failed to connect to seeds: " + std::string (e.what ()));
	throw;
      }

    // Start worker threads
    workerThreads.emplace_back (&PeerNode::generateGossipMessage, this);
    workerThreads.emplace_back (&PeerNode::checkPeerLiveness, this);
    workerThreads.emplace_back (&PeerNode::messageSender, this);

    // Handle shutdown signal
    signal (SIGINT, [] (int) {
      // Cleanup will be handled by destructor
      exit (0);
    });

    // Main loop to accept incoming connections
    while (running)
      {
	struct sockaddr_in clientAddr;
	socklen_t addrLen = sizeof (clientAddr);

	int clientSocket
	  = accept (serverSocket, (struct sockaddr *) &clientAddr, &addrLen);
	if (clientSocket < 0)
	  continue;

	// Handle incoming message
	char buffer[MAX_BUFFER_SIZE];
	ssize_t bytesRead = recv (clientSocket, buffer, MAX_BUFFER_SIZE - 1, 0);

	if (bytesRead > 0)
	  {
	    buffer[bytesRead] = '\0';
	    std::string message (buffer);

	    char ipStr[INET_ADDRSTRLEN];
	    inet_ntop (AF_INET, &clientAddr.sin_addr, ipStr, INET_ADDRSTRLEN);

	    Node sender{ipStr, ntohs (clientAddr.sin_port)};
	    broadcastMessage (message, sender);
	  }

	close (clientSocket);
      }
  }

  ~PeerNode ()
  {
    running = false;

    // Close all connections
    {
      std::lock_guard<std::mutex> lock (peersMutex);
      for (const auto &[peer, sockfd] : peerSockets)
	{
	  if (sockfd != -1)
	    {
	      close (sockfd);
	    }
	}
    }

    // Close server socket
    if (serverSocket != -1)
      {
	close (serverSocket);
      }

    // Wait for all worker threads
    for (auto &thread : workerThreads)
      {
	if (thread.joinable ())
	  {
	    thread.join ();
	  }
      }

    logger->log ("Peer node shut down");
  }
};

int
main (int argc, char *argv[])
{
  if (argc != 4)
    {
      std::cerr << "Usage: " << argv[0] << " <ip> <port> <config_file>\n";
      return 1;
    }

  try
    {
      PeerNode peer (argv[1], std::stoi (argv[2]), argv[3]);

      // Set up signal handling for graceful shutdown
      struct sigaction sa;
      sa.sa_handler = [] (int) {
	std::cout << "\nShutting down peer node...\n";
	exit (0);
      };
      sigemptyset (&sa.sa_mask);
      sa.sa_flags = 0;
      sigaction (SIGINT, &sa, nullptr);
      sigaction (SIGTERM, &sa, nullptr);

      // Start the peer node
      peer.start ();
    }
  catch (const std::invalid_argument &e)
    {
      std::cerr << "Invalid port number: " << e.what () << std::endl;
      return 1;
    }
  catch (const std::exception &e)
    {
      std::cerr << "Fatal error: " << e.what () << std::endl;
      return 1;
    }

  return 0;
}