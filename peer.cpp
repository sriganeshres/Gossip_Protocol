/**
 * peer.cpp
 *
 * Implements a Peer Node for the P2P Gossip network.
 *
 * Overview:
 * - Reads seed node details from a config file (each line: "IP:Port").
 * - Randomly selects at least ⌊(n/2)⌋+1 seeds and registers with them by sending:
 *      Register:<self.IP>:<self.Port>:0
 *   Each seed responds with its current Peer List (format: "IP:Port:degree;...").
 * - The peer unions these lists and selects a subset of peers to connect with using weighted
 *   random selection (weight = degree+2) – ensuring a power–law distribution.
 * - Establishes persistent TCP connections with the selected peers.
 * - Runs its own server (listening on its own IP and Port) to accept incoming persistent connections.
 *   When a persistent connection is established (via a registration message), it is added to the peer’s list.
 * - Maintains a Message List (ML) (a set of processed message IDs) so that each gossip message is forwarded only once.
 * - Every 5 seconds the peer generates a gossip message (for a total of 10 messages) of the form:
 *      <timestamp>:<self.IP>:<MsgID>
 *   New gossip messages are logged and then forwarded to all persistent connections (except to the sender).
 * - Every 13 seconds, the peer pings each connected peer using a temporary connection (using the Ping/Pong protocol)
 *   to check liveliness. (These temporary connections are not added to the persistent list and do not update the degree.)
 *   If a peer fails 3 consecutive pings, a dead–node message is sent to all seeds:
 *      Dead Node:<deadIP>:<deadPort>:<timestamp>:<self.IP>
 *   and the persistent connection to that peer is closed.
 *
 * All events are logged to both the console and to "peer-<selfPort>.log". Liveliness events are additionally
 * logged to "liveliness-<selfPort>.txt".
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
#include <map>
#include <unordered_set>
#include <algorithm>
#include <cstdlib>

struct PeerInfo {
    std::string ip;
    int port;
    int degree;
    PeerInfo(const std::string &ipAddr, int p, int deg = 0)
        : ip(ipAddr), port(p), degree(deg) {}
};

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

    // Load seed nodes from config file (each line: "IP:Port").
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

    // Register with at least ⌊(n/2)⌋+1 seeds.
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
            // Send registration message (degree is set only at first registration).
            std::string regMsg = "Register:" + localIP + ":" + std::to_string(localPort) + ":0\n";
            send(sock, regMsg.c_str(), regMsg.size(), 0);
            {
                std::lock_guard<std::mutex> lock(connMutex);
                seedConns.emplace_back(sock, seed.ip, seed.port);
            }
            // Read peer list response.
            char buffer[1024] = {0};
            ssize_t bytesRead = read(sock, buffer, sizeof(buffer) - 1);
            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                std::string listStr(buffer);
                log("Received peer list from seed " + seed.ip + ":" + std::to_string(seed.port) +
                    " -> " + listStr);
                parsePeerList(listStr);
            }
        }
    }

    // From the union of peer lists, select a subset using weighted random selection (weight = degree+2)
    // and connect to those peers.
    void selectAndConnectPeers() {
        if (allPeers.empty()) {
            log("No peers discovered from seeds.");
            return;
        }
        int total = allPeers.size();
        int numConnections = std::max(1, static_cast<int>(std::log(total)));
        numConnections = std::min(total, numConnections);
        double totalWeight = 0.0;
        for (const auto &peer : allPeers)
            totalWeight += (peer.degree + 2);
        std::vector<bool> selected(allPeers.size(), false);
        for (int i = 0; i < numConnections; ++i) {
            double r = randomDouble(0, totalWeight);
            double cumulative = 0.0;
            for (size_t j = 0; j < allPeers.size(); ++j) {
                if (selected[j])
                    continue;
                cumulative += (allPeers[j].degree + 2);
                if (r <= cumulative) {
                    selectedPeers.push_back(allPeers[j]);
                    selected[j] = true;
                    totalWeight -= (allPeers[j].degree + 2);
                    break;
                }
            }
        }
        // Connect to each selected peer.
        for (const auto &peer : selectedPeers) {
            connectToPeer(peer);
        }
    }

    // Start the peer's own server to accept incoming persistent connections.
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

    // Generate and broadcast 10 gossip messages (one every 5 seconds).
    // Format: <timestamp>:<self.IP>:<MsgID>
    void startGossip() {
        for (int i = 0; i < 10; ++i) {
            std::string msgID = generateGossipID();
            std::string timestamp = currentTimestamp();
            std::string gossip = timestamp + ":" + localIP + ":" + msgID;
            log("Broadcasting gossip: " + gossip);
            // Process our own gossip locally so that it gets logged and forwarded.
            processGossipMessage(gossip, -1);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }

    // Every 13 seconds, ping each persistent peer using a temporary connection with a Ping/Pong handshake.
    // If 3 consecutive pings fail for a peer, send a dead–node message to all seeds and close its persistent connection.
    void monitorLiveness() {
        while (true) {
            for (const auto &peer : selectedPeers) {
                std::string key = peer.ip + ":" + std::to_string(peer.port);
                bool alive = checkPeerLiveness(peer);
                if (alive) {
                    missCounts[key] = 0;
                    logLivelinessEvent("Liveliness OK for peer " + key);
                } else {
                    missCounts[key]++;
                    logLivelinessEvent("No response from peer " + key + ". Miss count: " + std::to_string(missCounts[key]));
                    if (missCounts[key] >= 3) {
                        reportDeadPeer(peer);
                        missCounts[key] = 0;
                    }
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
    std::vector<PeerInfo> allPeers;           // Union of peers from seeds.
    std::vector<PeerInfo> selectedPeers;      // Peers chosen for persistent connection.
    std::vector<Connection> seedConns;        // Persistent connections to seeds.
    std::vector<Connection> peerConns;        // Persistent connections to peers.
    std::mutex connMutex;
    std::mutex peersMutex;
    std::map<std::string, int> missCounts;    // Key: "ip:port", consecutive ping failures.
    std::unordered_set<std::string> gossipHistory; // Set of processed gossip message IDs.
    std::mt19937 rng;

    std::string currentTimestamp() {
        std::time_t now = std::time(nullptr);
        std::tm *ltm = std::localtime(&now);
        std::ostringstream oss;
        oss << std::put_time(ltm, "%d-%m-%Y %H-%M-%S");
        return oss.str();
    }

    void log(const std::string &message) {
        std::string timeStr = currentTimestamp();
        std::string logMsg = "[" + timeStr + "] " + message;
        std::cout << logMsg << std::endl;
        std::ofstream outfile("peer-" + std::to_string(localPort) + ".log", std::ios::app);
        if (outfile)
            outfile << logMsg << std::endl;
    }

    void logLivelinessEvent(const std::string &msg) {
        std::string timeStr = currentTimestamp();
        std::string logMsg = "[" + timeStr + "] " + msg;
        std::cout << logMsg << std::endl;
        std::ofstream outfile("liveliness-" + std::to_string(localPort) + ".txt", std::ios::app);
        if (outfile)
            outfile << logMsg << std::endl;
    }

    double randomDouble(double min, double max) {
        std::uniform_real_distribution<double> dist(min, max);
        return dist(rng);
    }

    std::string generateGossipID() {
        std::uniform_int_distribution<int> dist(0, 99999999);
        return "Msg" + std::to_string(dist(rng));
    }

    // Parse a peer list string received from a seed.
    // Format: "IP:Port:degree;IP:Port:degree;..."
    void parsePeerList(const std::string &listStr) {
        std::istringstream iss(listStr);
        std::string token;
        while (std::getline(iss, token, ';')) {
            if (token.empty())
                continue;
            size_t pos1 = token.find(":");
            size_t pos2 = token.find(":", pos1 + 1);
            if (pos1 == std::string::npos || pos2 == std::string::npos)
                continue;
            std::string ip = token.substr(0, pos1);
            int port = std::stoi(token.substr(pos1 + 1, pos2 - pos1 - 1));
            int degree = std::stoi(token.substr(pos2 + 1));
            if (ip == localIP && port == localPort)
                continue;
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

    // Connect to a selected peer and add the connection to the persistent list.
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
            peerConns.push_back(Connection(sock, peer.ip, peer.port));
        }
        // Send registration message on the persistent connection.
        std::string regMsg = "Register:" + localIP + ":" + std::to_string(localPort) + ":0\n";
        send(sock, regMsg.c_str(), regMsg.size(), 0);
        log("Connected to peer " + peer.ip + ":" + std::to_string(peer.port));
        std::thread(&PeerNode::handlePeerConnection, this, sock).detach();
    }

    // Handle an incoming connection.
    // If the first message is a registration, add the connection to the persistent list.
    // If the first message is a Ping, process it and close the connection.
    void handlePeerConnection(int clientSock) {
        char buffer[1024];
        std::string dataBuffer;
        bool persistent = false;
        // Read first complete line.
        ssize_t bytesRead = read(clientSock, buffer, sizeof(buffer) - 1);
        if (bytesRead <= 0) { close(clientSock); return; }
        buffer[bytesRead] = '\0';
        dataBuffer.append(buffer);
        size_t pos = dataBuffer.find('\n');
        if (pos != std::string::npos) {
            std::string firstMsg = dataBuffer.substr(0, pos);
            dataBuffer.erase(0, pos + 1);
            std::istringstream iss(firstMsg);
            std::string token;
            std::getline(iss, token, ':');
            if (token == "Register") {
                // This is a persistent connection.
                std::string peerIP, peerPortStr, degreeStr;
                std::getline(iss, peerIP, ':');
                std::getline(iss, peerPortStr, ':');
                persistent = true;
                {
                    std::lock_guard<std::mutex> lock(connMutex);
                    bool exists = false;
                    for (const auto &conn : peerConns) {
                        if (conn.sockfd == clientSock) { exists = true; break; }
                    }
                    if (!exists)
                        peerConns.push_back(Connection(clientSock, peerIP, std::stoi(peerPortStr)));
                }
                processReceivedMessage(firstMsg, clientSock);
            } else if (token == "Ping") {
                // Ephemeral ping: reply and close.
                processReceivedMessage(firstMsg, clientSock);
                close(clientSock);
                return;
            } else {
                // Otherwise, process normally.
                processReceivedMessage(firstMsg, clientSock);
            }
        }
        // Process subsequent messages.
        while (true) {
            bytesRead = read(clientSock, buffer, sizeof(buffer) - 1);
            if (bytesRead <= 0)
                break;
            buffer[bytesRead] = '\0';
            dataBuffer.append(buffer);
            while ((pos = dataBuffer.find('\n')) != std::string::npos) {
                std::string msg = dataBuffer.substr(0, pos);
                dataBuffer.erase(0, pos + 1);
                processReceivedMessage(msg, clientSock);
            }
        }
        if (persistent) {
            std::lock_guard<std::mutex> lock(connMutex);
            peerConns.erase(std::remove_if(peerConns.begin(), peerConns.end(),
                           [clientSock](const Connection &c) { return c.sockfd == clientSock; }),
                           peerConns.end());
        }
        close(clientSock);
    }

    // Process a message received from a peer.
    // Distinguishes between registration, ping, and gossip messages.
    void processReceivedMessage(const std::string &msg, int senderSock) {
        std::istringstream iss(msg);
        std::string token;
        if (!std::getline(iss, token, ':'))
            return;
        if (token == "Register") {
            // Format: Register:<peerIP>:<peerPort>:<degree>
            std::string peerIP, peerPortStr, degreeStr;
            std::getline(iss, peerIP, ':');
            std::getline(iss, peerPortStr, ':');
            std::getline(iss, degreeStr, ':');
            log("Received registration from peer " + peerIP + ":" + peerPortStr);
            {
                std::lock_guard<std::mutex> lock(peersMutex);
                bool exists = false;
                for (const auto &p : allPeers)
                    if (p.ip == peerIP && p.port == std::stoi(peerPortStr))
                        exists = true;
                if (!exists)
                    allPeers.emplace_back(peerIP, std::stoi(peerPortStr), degreeStr.empty() ? 0 : std::stoi(degreeStr));
            }
        } else if (token == "Ping") {
            // Respond to ping.
            std::string query;
            std::getline(iss, query, ':');
            std::string reply = "Pong:" + std::to_string(localPort);
            send(senderSock, reply.c_str(), reply.size(), 0);
        } else {
            // Otherwise, treat as gossip message.
            processGossipMessage(msg, senderSock);
        }
    }

    // Process a gossip message.
    // Format: <timestamp>:<senderIP>:<MsgID>
    // If new, log and forward to all persistent connections (except the sender).
    void processGossipMessage(const std::string &msg, int senderSock) {
        std::istringstream iss(msg);
        std::string timestamp, senderIP, msgID;
        if (!std::getline(iss, timestamp, ':')) return;
        if (!std::getline(iss, senderIP, ':')) return;
        if (!std::getline(iss, msgID, ':')) return;
        {
            std::lock_guard<std::mutex> lock(msgMutex);
            if (gossipHistory.find(msgID) != gossipHistory.end())
                return;
            gossipHistory.insert(msgID);
        }
        log("Received gossip: " + msg + " (from " + senderIP + ")");
        // Forward to all persistent connections except sender.
        std::lock_guard<std::mutex> lock(connMutex);
        for (const auto &conn : peerConns) {
            if (conn.sockfd == senderSock)
                continue;
            std::string fwd = msg + "\n";
            if (send(conn.sockfd, fwd.c_str(), fwd.size(), 0) < 0)
                perror("Peer: Error forwarding gossip");
        }
    }

    std::mutex msgMutex; // Protects gossipHistory.

    // Check liveness of a peer using a temporary connection with Ping/Pong.
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
        std::string ping = "Ping:Are you Alive\n";
        send(sock, ping.c_str(), ping.size(), 0);
        char buffer[256] = {0};
        ssize_t bytesRead = read(sock, buffer, sizeof(buffer) - 1);
        close(sock);
        if (bytesRead > 0) {
            buffer[bytesRead] = '\0';
            std::istringstream iss(buffer);
            std::string token;
            std::getline(iss, token, ':');
            if (token == "Pong") {
                std::string replyPortStr;
                std::getline(iss, replyPortStr, ':');
                int replyPort = std::stoi(replyPortStr);
                return (replyPort == peer.port);
            }
        }
        return false;
    }

    // Report a peer as dead: send a dead–node message to all seeds and remove it from persistent connections.
    void reportDeadPeer(const PeerInfo &peer) {
        log("Peer " + peer.ip + ":" + std::to_string(peer.port) + " is not responding. Reporting as dead.");
        std::string timeStr = currentTimestamp();
        std::string deadMsg = "Dead Node:" + peer.ip + ":" + std::to_string(peer.port) +
                              ":" + timeStr + ":" + localIP + "\n";
        {
            std::lock_guard<std::mutex> lock(connMutex);
            for (const auto &seedConn : seedConns) {
                send(seedConn.sockfd, deadMsg.c_str(), deadMsg.size(), 0);
            }
        }
        {
            std::lock_guard<std::mutex> lock(peersMutex);
            allPeers.erase(std::remove_if(allPeers.begin(), allPeers.end(),
                                          [peer](const PeerInfo &p) {
                                              return (p.ip == peer.ip && p.port == peer.port);
                                          }), allPeers.end());
            selectedPeers.erase(std::remove_if(selectedPeers.begin(), selectedPeers.end(),
                                               [peer](const PeerInfo &p) {
                                                   return (p.ip == peer.ip && p.port == peer.port);
                                               }), selectedPeers.end());
        }
        logLivelinessEvent("Reported dead peer: " + peer.ip + ":" + std::to_string(peer.port));
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

    std::thread serverThread(&PeerNode::startPeerServer, &peer);
    std::thread gossipThread(&PeerNode::startGossip, &peer);
    std::thread livenessThread(&PeerNode::monitorLiveness, &peer);

    serverThread.join();
    gossipThread.join();
    livenessThread.join();

    return EXIT_SUCCESS;
}