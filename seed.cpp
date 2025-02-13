#include "common.hpp"

class SeedNode
{
private:
  Node self;
  std::set<Node> peerList;
  std::mutex peerListMutex;
  std::unique_ptr<Logger> logger;
  int serverSocket;
  bool running;
  std::vector<std::thread> workerThreads;
  ThreadSafeQueue<std::pair<int, sockaddr_in>> clientQueue;

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

  void acceptConnections ()
  {
    while (running)
      {
	struct sockaddr_in clientAddr;
	socklen_t addrLen = sizeof (clientAddr);

	int clientSocket
	  = accept (serverSocket, (struct sockaddr *) &clientAddr, &addrLen);
	if (clientSocket < 0)
	  {
	    if (running)
	      {
		logger->log ("Failed to accept connection");
	      }
	    continue;
	  }

	clientQueue.push ({clientSocket, clientAddr});
      }
  }

  void handleClient (int clientSocket, sockaddr_in clientAddr)
  {
    char buffer[MAX_BUFFER_SIZE];
    ssize_t bytesRead = recv (clientSocket, buffer, MAX_BUFFER_SIZE - 1, 0);

    if (bytesRead <= 0)
      {
	close (clientSocket);
	return;
      }

    buffer[bytesRead] = '\0';
    std::string message (buffer);

    if (message.substr (0, 4) == "REG:")
      {
	handleRegistration (message, clientSocket, clientAddr);
      }
    else if (message == "GET_PEERS")
      {
	handlePeerListRequest (clientSocket);
      }
    else if (message.substr (0, 10) == "Dead Node:")
      {
	handleDeadNodeNotification (message);
      }

    close (clientSocket);
  }

  void handleRegistration (const std::string &message, int clientSocket,
			   const sockaddr_in &clientAddr)
  {
    std::istringstream iss (message.substr (4));
    std::string ip;
    int port;

    std::getline (iss, ip, ':');
    iss >> port;

    Node newPeer{ip, port};

    {
      std::lock_guard<std::mutex> lock (peerListMutex);
      peerList.insert (newPeer);
    }

    logger->log ("New peer registered: " + ip + ":" + std::to_string (port));

    // Send acknowledgment
    std::string ack = "Registration successful";
    send (clientSocket, ack.c_str (), ack.length (), 0);
  }

  void handlePeerListRequest (int clientSocket)
  {
    std::string peerListStr;
    {
      std::lock_guard<std::mutex> lock (peerListMutex);
      for (const auto &peer : peerList)
	{
	  peerListStr += peer.ip + ":" + std::to_string (peer.port) + ";";
	}
    }

    send (clientSocket, peerListStr.c_str (), peerListStr.length (), 0);
  }

  void handleDeadNodeNotification (const std::string &message)
  {
    std::istringstream iss (message.substr (10));
    std::string deadIp;
    int deadPort;

    std::getline (iss, deadIp, ':');
    iss >> deadPort;

    Node deadNode{deadIp, deadPort};

    {
      std::lock_guard<std::mutex> lock (peerListMutex);
      peerList.erase (deadNode);
    }

    logger->log ("Removed dead node: " + deadIp + ":"
		 + std::to_string (deadPort));
  }

  void clientHandler ()
  {
    while (running)
      {
	std::pair<int, sockaddr_in> client;
	clientQueue.wait_and_pop (client);

	try
	  {
	    handleClient (client.first, client.second);
	  }
	catch (const std::exception &e)
	  {
	    logger->log ("Error handling client: " + std::string (e.what ()));
	  }
      }
  }

public:
  SeedNode (const std::string &ip, int port)
    : self{ip, port}, running (true),
      logger (std::make_unique<Logger> ("seed_" + ip + "_"
					+ std::to_string (port) + ".log"))
  {
    try
      {
	initializeSocket ();
      }
    catch (const NetworkError &e)
      {
	logger->log ("Failed to initialize seed node: "
		     + std::string (e.what ()));
	throw;
      }
  }

  void start ()
  {
    logger->log ("Starting seed node on " + self.ip + ":"
		 + std::to_string (self.port));

    // Start worker threads
    for (int i = 0; i < std::thread::hardware_concurrency (); ++i)
      {
	workerThreads.emplace_back (&SeedNode::clientHandler, this);
      }

    // Start accept thread
    std::thread acceptThread (&SeedNode::acceptConnections, this);

    // Wait for shutdown signal
    signal (SIGINT, [] (int) {
      // Cleanup will be handled by destructor
      exit (0);
    });

    acceptThread.join ();
  }

  ~SeedNode ()
  {
    running = false;

    // Close server socket to stop accept
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

    logger->log ("Seed node shut down");
  }
};

int
main (int argc, char *argv[])
{
  if (argc != 3)
    {
      std::cerr << "Usage: " << argv[0] << " <ip> <port>\n";
      return 1;
    }

  try
    {
      SeedNode seed (argv[1], std::stoi (argv[2]));
      seed.start ();
    }
  catch (const std::exception &e)
    {
      std::cerr << "Fatal error: " << e.what () << std::endl;
      return 1;
    }

  return 0;
}