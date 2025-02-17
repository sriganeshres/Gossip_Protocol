/**
 * seed.cpp
 *
 * Implements a Seed Node for the P2P Gossip network.
 *
 * - On startup, the seed listens for incoming TCP connections.
 * - When a peer sends a registration message:
 *      Register:<peerIP>:<peerPort>:<degree>
 *   the seed adds the peer (if not already present) to its Peer List (PL)
 *   and immediately sends back the current PL (a semicolon-separated list
 *   of entries of the form <IP>:<Port>:<degree>).
 * - (Any subsequent update messages are ignored so that the degree remains fixed.)
 * - When a dead–node message is received:
 *      Dead Node:<deadIP>:<deadPort>:<timestamp>:<reporterIP>
 *   the seed removes that peer from its PL.
 * - Every 5 seconds, the seed writes its current PL (with degrees) to "seed_metrics.txt".
 *
 * All events are logged to both the console and a log file ("seed-<SeedPort>.log").
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
#include <cstdlib>

struct PeerRecord {
    std::string ip;
    int port;
    int degree;
    PeerRecord(const std::string &ipAddr, int portNum, int deg = 0)
        : ip(ipAddr), port(portNum), degree(deg) {}
};

class SeedNode {
public:
    SeedNode(const std::string &ip, int port)
        : seedIP(ip), seedPort(port), serverSock(-1) {}

    void start() {
        // Create and bind server socket.
        serverSock = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSock < 0) {
            perror("Seed: Socket creation failed");
            exit(EXIT_FAILURE);
        }
        int opt = 1;
        if (setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("Seed: setsockopt failed");
            exit(EXIT_FAILURE);
        }
        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = inet_addr(seedIP.c_str());
        serverAddr.sin_port = htons(seedPort);
        if (bind(serverSock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
            perror("Seed: Bind failed");
            exit(EXIT_FAILURE);
        }
        if (listen(serverSock, 10) < 0) {
            perror("Seed: Listen failed");
            exit(EXIT_FAILURE);
        }
        log("Seed node listening on " + seedIP + ":" + std::to_string(seedPort));

        // Start metrics logging thread (every 5 seconds).
        std::thread metricsThread(&SeedNode::logPeerMetrics, this);
        metricsThread.detach();

        // Accept incoming connections.
        while (true) {
            sockaddr_in clientAddr;
            socklen_t addrLen = sizeof(clientAddr);
            int clientSock = accept(serverSock, (struct sockaddr *)&clientAddr, &addrLen);
            if (clientSock < 0) {
                perror("Seed: Accept failed");
                continue;
            }
            std::thread(&SeedNode::handleConnection, this, clientSock).detach();
        }
        close(serverSock);
    }

private:
    std::string seedIP;
    int seedPort;
    int serverSock;
    std::vector<PeerRecord> peerList;
    std::mutex peerMutex;

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
        std::ofstream outfile("seed-" + std::to_string(seedPort) + ".log", std::ios::app);
        if (outfile)
            outfile << logMsg << std::endl;
    }

    // Format the current Peer List (PL) as a semicolon-separated list of "IP:Port:degree" entries.
    std::string formatPeerList() {
        std::lock_guard<std::mutex> lock(peerMutex);
        std::ostringstream oss;
        for (const auto &peer : peerList) {
            oss << peer.ip << ":" << peer.port << ":" << peer.degree << ";";
        }
        return oss.str();
    }

    void handleConnection(int clientSock) {
        char buffer[1024];
        std::string dataBuffer;
        while (true) {
            ssize_t bytesRead = read(clientSock, buffer, sizeof(buffer) - 1);
            if (bytesRead <= 0)
                break;
            buffer[bytesRead] = '\0';
            dataBuffer.append(buffer);
            size_t pos;
            while ((pos = dataBuffer.find('\n')) != std::string::npos) {
                std::string msg = dataBuffer.substr(0, pos);
                dataBuffer.erase(0, pos + 1);
                processMessage(msg, clientSock);
            }
        }
        close(clientSock);
    }

    void processMessage(const std::string &msg, int clientSock) {
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
            int degree = degreeStr.empty() ? 0 : std::stoi(degreeStr);
            {
                std::lock_guard<std::mutex> lock(peerMutex);
                bool exists = false;
                for (const auto &rec : peerList) {
                    if (rec.ip == peerIP && rec.port == peerPort) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    // Set degree only once at first registration.
                    peerList.emplace_back(peerIP, peerPort, degree);
                    log("Registered new peer: " + peerIP + ":" + peerPortStr +
                        " (degree: " + std::to_string(degree) + ")");
                }
            }
            // Immediately respond with the current PL.
            std::string response = formatPeerList();
            send(clientSock, response.c_str(), response.size(), 0);
        } else if (command == "Dead Node") {
            // Format: Dead Node:<deadIP>:<deadPort>:<timestamp>:<reporterIP>
            std::string deadIP, deadPortStr, timestamp, reporterIP;
            std::getline(iss, deadIP, ':');
            std::getline(iss, deadPortStr, ':');
            std::getline(iss, timestamp, ':');
            std::getline(iss, reporterIP, ':');
            int deadPort = std::stoi(deadPortStr);
            {
                std::lock_guard<std::mutex> lock(peerMutex);
                auto it = std::remove_if(peerList.begin(), peerList.end(),
                                         [deadIP, deadPort](const PeerRecord &rec) {
                                             return (rec.ip == deadIP && rec.port == deadPort);
                                         });
                if (it != peerList.end()) {
                    peerList.erase(it, peerList.end());
                    log("Removed dead peer: " + deadIP + ":" + deadPortStr +
                        " reported at " + timestamp);
                }
            }
        } else {
            log("Unknown command received: " + msg);
        }
    }

    // Log peer metrics (current PL) every 5 seconds to seed_metrics.txt.
    void logPeerMetrics() {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(peerMutex);
                std::ofstream outfile("seed_metrics.txt", std::ios::app);
                if (outfile) {
                    outfile << "[" << currentTimestamp() << "] Current Peer List and Degrees:\n";
                    for (const auto &rec : peerList) {
                        outfile << "Peer: " << rec.ip << ":" << rec.port
                                << " - Degree: " << rec.degree << "\n";
                    }
                    outfile << "--------------------------------------\n";
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
};

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: ./seed <SeedIP> <SeedPort>" << std::endl;
        return EXIT_FAILURE;
    }
    std::string ip = argv[1];
    int port = std::stoi(argv[2]);
    SeedNode seed(ip, port);
    seed.start();
    return EXIT_SUCCESS;
}