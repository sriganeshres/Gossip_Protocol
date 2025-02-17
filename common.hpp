#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <random>
#include <chrono>
#include <thread>
#include <queue>
#include <condition_variable>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <memory>
#include <cmath>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <iomanip>
#include <algorithm>

// Constants
constexpr int MAX_BUFFER_SIZE = 4096;
constexpr int PING_INTERVAL = 13; // seconds
constexpr int MAX_MISSED_PINGS = 3;
constexpr int MESSAGE_GENERATION_INTERVAL = 5; // seconds
constexpr int MAX_MESSAGES = 10;

// Struct to represent a node in the network
struct Node
{
  std::string ip;
  int port;

  bool operator== (const Node &other) const
  {
    return ip == other.ip && port == other.port;
  }

  bool operator< (const Node &other) const
  {
    if (ip != other.ip)
      return ip < other.ip;
    return port < other.port;
  }
};

// Message types for protocol communication
enum class MessageType
{
  REGISTRATION,
  PEER_LIST_REQUEST,
  PEER_LIST_RESPONSE,
  GOSSIP,
  DEAD_NODE,
  PING,
  PONG
};

// Class to handle message creation and parsing
class Message
{
public:
  static std::string createRegistrationMessage (const Node &node)
  {
    return "REG:" + node.ip + ":" + std::to_string (node.port);
  }

  static std::string createGossipMessage (const Node &node, int msgNum)
  {
    auto now = std::chrono::system_clock::now ();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds> (
		       now.time_since_epoch ())
		       .count ();
    return std::to_string (timestamp) + ":" + node.ip + ":"
	   + std::to_string (node.port) + std::to_string (msgNum);
  }

  static std::string createDeadNodeMessage (const Node &deadNode,
					    const Node &reporter)
  {
    auto now = std::chrono::system_clock::now ();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds> (
		       now.time_since_epoch ())
		       .count ();
    return "Dead Node:" + deadNode.ip + ":" + std::to_string (deadNode.port)
	   + ":" + std::to_string (timestamp) + ":" + reporter.ip + ":"
	   + std::to_string (reporter.port);
  }

  static std::string calculateHash (const std::string &message)
  {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    EVP_MD_CTX *ctx = EVP_MD_CTX_new (); // Create a new context
    if (!ctx)
      {
	throw std::runtime_error ("EVP_MD_CTX_new failed");
      }

    EVP_DigestInit_ex (ctx, EVP_sha256 (), nullptr);
    EVP_DigestUpdate (ctx, message.c_str (), message.length ());
    EVP_DigestFinal_ex (ctx, hash, nullptr);
    EVP_MD_CTX_free (ctx); // Free the context

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
      {
	ss << std::hex << std::setw (2) << std::setfill ('0') << (int) hash[i];
      }
    return ss.str ();
  }
};

// Thread-safe queue for message processing
template <typename T> class ThreadSafeQueue
{
private:
  std::queue<T> queue;
  mutable std::mutex mutex;
  std::condition_variable cond;

public:
  void push (T value)
  {
    std::lock_guard<std::mutex> lock (mutex);
    queue.push (std::move (value));
    cond.notify_one ();
  }

  bool try_pop (T &value)
  {
    std::lock_guard<std::mutex> lock (mutex);
    if (queue.empty ())
      return false;
    value = std::move (queue.front ());
    queue.pop ();
    return true;
  }

  void wait_and_pop (T &value)
  {
    std::unique_lock<std::mutex> lock (mutex);
    cond.wait (lock, [this] { return !queue.empty (); });
    value = std::move (queue.front ());
    queue.pop ();
  }
};

// Power-law degree distribution generator
class PowerLawDegreeGenerator
{
private:
  std::mt19937 gen;
  double alpha; // Power-law exponent

public:
  PowerLawDegreeGenerator (double alpha = 2.5) : alpha (alpha)
  {
    std::random_device rd;
    gen = std::mt19937 (rd ());
  }

  int generateDegree (int minDegree, int maxDegree)
  {
    std::uniform_real_distribution<> dis (0, 1);
    double x = dis (gen);

    // Using inverse transform sampling
    double xMin = minDegree;
    double xMax = maxDegree;

    double degree
      = xMin
	* pow ((1 - x) + x * pow (xMax / xMin, 1 - alpha), 1 / (1 - alpha));
    return std::min (maxDegree, std::max (minDegree, (int) round (degree)));
  }
};

// Error handling wrapper
class NetworkError : public std::runtime_error
{
public:
  NetworkError (const std::string &message) : std::runtime_error (message) {}
};

// Logger class for consistent output
class Logger
{
private:
  std::string filename;
  std::mutex mutex;
  std::ofstream file;

public:
  Logger (const std::string &filename) : filename (filename)
  {
    file.open (filename, std::ios::app);
    if (!file.is_open ())
      {
	throw std::runtime_error ("Unable to open log file: " + filename);
      }
  }

  void log (const std::string &message)
  {
    std::lock_guard<std::mutex> lock (mutex);
    auto now = std::chrono::system_clock::now ();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds> (
		       now.time_since_epoch ())
		       .count ();

    std::string logMessage
      = "[" + std::to_string (timestamp) + "] " + message + "\n";

    std::cout << logMessage;
    file << logMessage;
    file.flush ();
  }

  ~Logger ()
  {
    if (file.is_open ())
      {
	file.close ();
      }
  }
};