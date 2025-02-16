/**
 * peer.cpp
 *
 * This program implements a Peer Node for the Gossip Protocol-based P2P network.
 * The peer:
 *   - Loads a list of seed nodes from a configuration file.
 *   - Randomly selects at least ⌊n/2⌋+1 seeds to register with.
 *   - Sends a registration message to each seed, then receives and unions their peer lists.
 *   - Selects a subset of peers (using a weighted power-law scheme) to form TCP connections.
 *   - Listens for incoming peer connections.
 *   - Periodically broadcasts gossip messages and monitors peer liveness.
 *
 * Compile with:
 *    g++ -std=c++11 -pthread peer.cpp -o peer
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

// Structure representing a peer’s information.
struct PeerInfo {
    std::string ip;
    int port;
    int degree;
    PeerInfo(const std::string &ipAddr, int p, int deg = 0)
        : ip(ipAddr), port(p), degree(deg) {}
};

// A simple wrapper for a connected socket.
struct Connection {
    int sockfd;
    std::string ip;
    int port;
    Connection(int fd, const std::string &ipAddr, int p)
        : sockfd(fd), ip(ipAddr), port(p) {}
};

class PeerNode {
public:
    PeerNode(const std::string &ip, int port)
        : localIP(ip), localPort(port), serverSock(-1),
          rng(static_cast<unsigned int>(std::chrono::steady_clock::now().time_since_epoch().count()))
    { }

    // Load seed node details from the config file.
    // Each line should be "IP:Port"
    void loadSeeds(const std::string &configFile) {
        std::ifstream infile(configFile);
        if (!infile) {
            std::cerr << "Peer: Unable to open config file " << configFile << std::endl;
            return;
        }
        std::string line;
        while (std::getline(infile, line)) {
            if (line.empty()) continue;
            size_t delim = line.find(":");
            if (delim == std::string::npos) continue;
            std::string ip = line.substr(0, delim);
            int port = std::stoi(line.substr(delim + 1));
            seedNodes.emplace_back(ip, port);
        }
    }

    // Connect to a subset of seed nodes (at least ⌊n/2⌋+1) and register with them.
    void registerWithSeeds() {
        int totalSeeds = seedNodes.size();
        if (totalSeeds == 0) {
            std::cerr << "Peer: No seeds loaded." << std::endl;
            return;
        }
        int required = totalSeeds / 2 + 1;
        std::vector<int> indices(totalSeeds);
        for (int i = 0; i < totalSeeds; ++i)
            indices[i] = i;
        std::shuffle(indices.begin(), indices.end(), rng);

        for (int i = 0; i < required; ++i) {
            PeerInfo seed = seedNodes[indices[i]];
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                perror("Peer: Seed socket creation error");
                continue;
            }
            sockaddr_in seedAddr;
            seedAddr.sin_family = AF_INET;
            seedAddr.sin_port = htons(seed.port);
            if (inet_pton(AF_INET, seed.ip.c_str(), &seedAddr.sin_addr) <= 0) {
                std::cerr << "Peer: Invalid seed address: " << seed.ip << std::endl;
                close(sock);
                continue;
            }
            if (connect(sock, (struct sockaddr *)&seedAddr, sizeof(seedAddr)) < 0) {
                std::cerr << "Peer: Connection to seed " << seed.ip << ":" << seed.port << " failed." << std::endl;
                close(sock);
                continue;
            }
            // Send registration message (note: degree is 0 at registration).
            std::string regMsg = "Register:" + localIP + ":" + std::to_string(localPort) + ":0\n";
            send(sock, regMsg.c_str(), regMsg.size(), 0);
            // Save the seed connection for later use.
            {
                std::lock_guard<std::mutex> lock(connMutex);
                seedConns.emplace_back(sock, seed.ip, seed.port);
            }
            // Read the peer list returned by the seed.
            char buffer[1024] = {0};
            ssize_t bytesRead = read(sock, buffer, sizeof(buffer) - 1);
            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                std::string listStr(buffer);
                log("Received peer list from seed " + seed.ip + ":" + std::to_string(seed.port) +
                    " -> " + listStr);
                parsePeerList(listStr);
            }
            // Do not close the connection – the protocol says peers keep their seed connection open.
        }
    }

    // From the union of peer lists received from seeds, select a subset of peers
    // to connect with, using a weighted (power-law) selection.
    void selectAndConnectPeers() {
        if (allPeers.empty()) {
            log("No peers discovered from seeds.");
            return;
        }
        // Determine number of connections: use a Poisson or log-based function.
        int total = allPeers.size();
        int numConnections = std::max(1, static_cast<int>(std::log(total)));
        numConnections = std::min(total, numConnections);

        // Compute total weight (weight = degree + 2).
        double totalWeight = 0.0;
        for (const auto &peer : allPeers)
            totalWeight += (peer.degree + 2);

        std::vector<bool> selected(allPeers.size(), false);
        for (int i = 0; i < numConnections; ++i) {
            double r = randomDouble(0, totalWeight);
            double cumulative = 0.0;
            for (size_t j = 0; j < allPeers.size(); ++j) {
                if (selected[j]) continue;
                cumulative += (allPeers[j].degree + 2);
                if (r <= cumulative) {
                    selectedPeers.push_back(allPeers[j]);
                    selected[j] = true;
                    totalWeight -= (allPeers[j].degree + 2);
                    break;
                }
            }
        }
        // Now, connect to each selected peer.
        for (const auto &peer : selectedPeers) {
            connectToPeer(peer);
        }
    }

    // Start the peer’s own server to accept incoming connections.
    void startPeerServer() {
        serverSock = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSock < 0) {
            perror("Peer: Server socket creation failed");
            exit(EXIT_FAILURE);
        }
        int opt = 1;
        if (setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("Peer: setsockopt failed");
            exit(EXIT_FAILURE);
        }
        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = inet_addr(localIP.c_str());
        serverAddr.sin_port = htons(localPort);
        if (bind(serverSock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
            perror("Peer: Bind failed");
            exit(EXIT_FAILURE);
        }
        if (listen(serverSock, 10) < 0) {
            perror("Peer: Listen failed");
            exit(EXIT_FAILURE);
        }
        log("Peer server listening on " + localIP + ":" + std::to_string(localPort));
        // Accept incoming connections.
        while (true) {
            sockaddr_in clientAddr;
            socklen_t addrLen = sizeof(clientAddr);
            int clientSock = accept(serverSock, (struct sockaddr *)&clientAddr, &addrLen);
            if (clientSock < 0) {
                perror("Peer: Accept failed");
                continue;
            }
            std::thread(&PeerNode::handlePeerConnection, this, clientSock).detach();
        }
        close(serverSock);
    }

    // Periodically generate and broadcast gossip messages.
    void startGossip() {
        for (int i = 0; i < 10; ++i) {
            std::string msgID = generateGossipID();
            std::string timestamp = currentTimestamp();
            std::string gossip = timestamp + ":" + localIP + ":" + std::to_string(localPort) + ":" + msgID;
            log("Broadcasting gossip: " + gossip);
            broadcastToPeers(gossip + "\n");
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }

    // Periodically check the liveness of connected peers by sending a heartbeat message.
    void monitorLiveness() {
        while (true) {
            for (const auto &peer : selectedPeers) {
                if (!checkPeerLiveness(peer)) {
                    reportDeadPeer(peer);
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(13));
        }
    }

private:
    std::string localIP;
    int localPort;
    int serverSock;
    std::vector<PeerInfo> seedNodes;        // Loaded from config.
    std::vector<PeerInfo> allPeers;           // Union of peers received from seeds.
    std::vector<PeerInfo> selectedPeers;      // Peers chosen for direct connection.
    std::vector<Connection> seedConns;        // Open connections to seed nodes.
    std::vector<Connection> peerConns;        // Open connections to peers.
    std::mutex connMutex;
    std::mutex peersMutex;
    std::mt19937 rng;

    // Utility: Log message to console and file.
    void log(const std::string &message) {
        std::string timeStr = currentTimestamp();
        std::string logMsg = "[" + timeStr + "] " + message;
        std::cout << logMsg << std::endl;
        std::ofstream outfile("peer-" + std::to_string(localPort) + ".log", std::ios::app);
        if (outfile) {
            outfile << logMsg << std::endl;
        }
    }

    // Utility: Get current timestamp.
    std::string currentTimestamp() {
        std::time_t now = std::time(nullptr);
        std::tm *ltm = std::localtime(&now);
        std::ostringstream oss;
        oss << std::put_time(ltm, "%d-%m-%Y %H-%M-%S");
        return oss.str();
    }

    // Utility: Generate a random double between min and max.
    double randomDouble(double min, double max) {
        std::uniform_real_distribution<double> dist(min, max);
        return dist(rng);
    }

    // Utility: Generate a unique gossip message ID.
    std::string generateGossipID() {
        std::uniform_int_distribution<int> dist(0, 99999999);
        return "Msg" + std::to_string(dist(rng));
    }

    // Parse a peer list string received from a seed.
    // Expected format: "IP:Port:degree;IP:Port:degree;..."
    void parsePeerList(const std::string &listStr) {
        std::istringstream iss(listStr);
        std::string token;
        while (std::getline(iss, token, ';')) {
            if (token.empty()) continue;
            size_t pos1 = token.find(":");
            size_t pos2 = token.find(":", pos1 + 1);
            if (pos1 == std::string::npos || pos2 == std::string::npos)
                continue;
            std::string ip = token.substr(0, pos1);
            int port = std::stoi(token.substr(pos1 + 1, pos2 - pos1 - 1));
            int degree = std::stoi(token.substr(pos2 + 1));
            // Exclude self.
            if (ip == localIP && port == localPort)
                continue;
            // Avoid duplicates.
            {
                std::lock_guard<std::mutex> lock(peersMutex);
                bool exists = false;
                for (const auto &p : allPeers) {
                    if (p.ip == ip && p.port == port) {
                        exists = true;
                        break;
                    }
                }
                if (!exists)
                    allPeers.emplace_back(ip, port, degree);
            }
        }
    }

    // Connect to a selected peer.
    void connectToPeer(const PeerInfo &peer) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("Peer: Creating socket for peer failed");
            return;
        }
        sockaddr_in peerAddr;
        peerAddr.sin_family = AF_INET;
        peerAddr.sin_port = htons(peer.port);
        if (inet_pton(AF_INET, peer.ip.c_str(), &peerAddr.sin_addr) <= 0) {
            std::cerr << "Peer: Invalid peer address: " << peer.ip << std::endl;
            close(sock);
            return;
        }
        if (connect(sock, (struct sockaddr *)&peerAddr, sizeof(peerAddr)) < 0) {
            std::cerr << "Peer: Connection to peer " << peer.ip << ":" << peer.port << " failed." << std::endl;
            close(sock);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(connMutex);
            peerConns.emplace_back(sock, peer.ip, peer.port);
        }
        // Send our registration message to the peer.
        std::string regMsg = "Register:" + localIP + ":" + std::to_string(localPort) + ":0\n";
        send(sock, regMsg.c_str(), regMsg.size(), 0);
        log("Connected to peer " + peer.ip + ":" + std::to_string(peer.port));
        // Optionally, start a thread to handle incoming messages on this connection.
        std::thread(&PeerNode::handlePeerConnection, this, sock).detach();
    }

    // Handle an incoming connection from another peer.
    void handlePeerConnection(int clientSock) {
        char buffer[1024];
        std::string dataBuffer;
        while (true) {
            ssize_t bytesRead = read(clientSock, buffer, sizeof(buffer) - 1);
            if (bytesRead <= 0) break;
            buffer[bytesRead] = '\0';
            dataBuffer.append(buffer);
            size_t pos;
            while ((pos = dataBuffer.find('\n')) != std::string::npos) {
                std::string msg = dataBuffer.substr(0, pos);
                dataBuffer.erase(0, pos + 1);
                processPeerMessage(msg, clientSock);
            }
        }
        close(clientSock);
    }

    // Process a message received from a connected peer.
    void processPeerMessage(const std::string &msg, int sock) {
        // For simplicity, we handle a few commands:
        // "Register:" messages (registration from a new peer)
        // "Liveliness:" messages (ping/pong for liveness checking)
        std::istringstream iss(msg);
        std::string command;
        std::getline(iss, command, ':');
        if (command == "Register") {
            // Format: Register:<peerIP>:<peerPort>:<degree>
            std::string peerIP, peerPortStr, degreeStr;
            std::getline(iss, peerIP, ':');
            std::getline(iss, peerPortStr, ':');
            std::getline(iss, degreeStr, ':');
            int peerPort = std::stoi(peerPortStr);
            log("Received registration from peer " + peerIP + ":" + peerPortStr);
            // Optionally, add this peer to our union (if not already present)
            {
                std::lock_guard<std::mutex> lock(peersMutex);
                bool exists = false;
                for (const auto &p : allPeers)
                    if (p.ip == peerIP && p.port == peerPort)
                        exists = true;
                if (!exists)
                    allPeers.emplace_back(peerIP, peerPort, degreeStr.empty() ? 0 : std::stoi(degreeStr));
            }
        } else if (command == "Liveliness") {
            // Respond to a liveness ping.
            std::string query;
            std::getline(iss, query, ':');
            // Simply respond with our port number.
            std::string reply = std::to_string(localPort);
            send(sock, reply.c_str(), reply.size(), 0);
        } else {
            log("Received unknown message: " + msg);
        }
    }

    // Broadcast a message to all directly connected peers.
    void broadcastToPeers(const std::string &msg) {
        std::lock_guard<std::mutex> lock(connMutex);
        for (const auto &conn : peerConns) {
            if (send(conn.sockfd, msg.c_str(), msg.size(), 0) < 0)
                perror("Peer: Error sending gossip");
        }
    }

    // Check the liveness of a peer by opening a short-lived connection and sending a ping.
    bool checkPeerLiveness(const PeerInfo &peer) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("Peer: Liveness socket creation failed");
            return false;
        }
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(peer.port);
        if (inet_pton(AF_INET, peer.ip.c_str(), &addr.sin_addr) <= 0) {
            close(sock);
            return false;
        }
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(sock);
            return false;
        }
        std::string ping = "Liveliness:Are you Alive\n";
        send(sock, ping.c_str(), ping.size(), 0);
        char buffer[256] = {0};
        ssize_t bytesRead = read(sock, buffer, sizeof(buffer) - 1);
        close(sock);
        if (bytesRead > 0) {
            buffer[bytesRead] = '\0';
            int replyPort = std::stoi(std::string(buffer));
            return (replyPort == peer.port);
        }
        return false;
    }

    // If a peer is detected as dead, report it to all seed nodes.
    void reportDeadPeer(const PeerInfo &peer) {
        log("Peer " + peer.ip + ":" + std::to_string(peer.port) + " is not responding. Reporting as dead.");
        std::string timeStr = currentTimestamp();
        std::string deadMsg = "DeadNode:" + peer.ip + ":" + std::to_string(peer.port) +
                              ":" + timeStr + ":" + localIP + ":" + std::to_string(localPort) + "\n";
        std::lock_guard<std::mutex> lock(connMutex);
        for (const auto &seedConn : seedConns) {
            send(seedConn.sockfd, deadMsg.c_str(), deadMsg.size(), 0);
        }
        // Also remove the dead peer from our lists.
        {
            std::lock_guard<std::mutex> lock2(peersMutex);
            allPeers.erase(std::remove_if(allPeers.begin(), allPeers.end(),
                                          [peer](const PeerInfo &p) {
                                              return (p.ip == peer.ip && p.port == peer.port);
                                          }), allPeers.end());
            selectedPeers.erase(std::remove_if(selectedPeers.begin(), selectedPeers.end(),
                                               [peer](const PeerInfo &p) {
                                                   return (p.ip == peer.ip && p.port == peer.port);
                                               }), selectedPeers.end());
        }
    }
};

int main(int argc, char *argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: ./peer <LocalIP> <LocalPort> <ConfigFile>" << std::endl;
        return EXIT_FAILURE;
    }
    std::string localIP = argv[1];
    int localPort = std::stoi(argv[2]);
    std::string configFile = argv[3];

    PeerNode peer(localIP, localPort);
    peer.loadSeeds(configFile);
    peer.registerWithSeeds();
    peer.selectAndConnectPeers();

    // Start the peer server in a separate thread.
    std::thread serverThread(&PeerNode::startPeerServer, &peer);
    // Start gossip broadcast thread.
    std::thread gossipThread(&PeerNode::startGossip, &peer);
    // Start liveness monitor thread.
    std::thread livenessThread(&PeerNode::monitorLiveness, &peer);

    serverThread.join();
    gossipThread.join();
    livenessThread.join();

    return EXIT_SUCCESS;
}