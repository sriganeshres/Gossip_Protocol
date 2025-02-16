# Project Overview

This project implements a peer-to-peer network with seed nodes and peer nodes. The seed nodes help peers discover each other, while the peer nodes communicate and share messages using a gossip protocol.

## Project Structure

### Files

- [`.clang-format`](.clang-format ): Configuration file for the `clang-format` tool.
- [`.gitignore`](.gitignore ): Specifies files and directories to be ignored by Git.
- [`common.hpp`](common.hpp ): Contains common definitions and utilities used by both seed and peer nodes.
- [`conf.clang`](conf.clang ): Configuration file for Clang.
- [`config.txt`](config.txt ): Configuration file listing seed nodes.
- [`peer`](peer ): Executable for the peer node.
- [`peer.cpp`](peer.cpp ): Source code for the peer node.
- [`seed`](seed ): Executable for the seed node.
- [`seed.cpp`](seed.cpp ): Source code for the seed node.

```
.
├── common.hpp
├── conf.clang
├── config.txt
├── peer.cpp
├── README.md
└── seed.cpp
```

## Detailed Explanation

### [`common.hpp`](common.hpp )

This file contains common definitions and utilities used by both seed and peer nodes.

#### Constants

- [`MAX_BUFFER_SIZE`](common.hpp ): Maximum buffer size for network communication.
- [`PING_INTERVAL`](common.hpp ): Interval for pinging peers to check their liveness.
- [`MAX_MISSED_PINGS`](common.hpp ): Maximum number of missed pings before considering a peer dead.
- [`MESSAGE_GENERATION_INTERVAL`](common.hpp ): Interval for generating gossip messages.
- [`MAX_MESSAGES`](common.hpp ): Maximum number of messages to generate.

#### Structs

- [`Node`](common.hpp ): Represents a node in the network with [`ip`](common.hpp ) and [`port`](common.hpp ). Implements comparison operators for use in sets.

#### Enums

- [`MessageType`](common.hpp ): Enum for different message types used in protocol communication.

#### Classes

- [`Message`](common.hpp ): Handles message creation and parsing.
- [`ThreadSafeQueue`](common.hpp ): A thread-safe queue for message processing.
- [`PowerLawDegreeGenerator`](common.hpp ): Generates degrees based on a power-law distribution.
- [`NetworkError`](common.hpp ): Custom exception class for network errors.
- [`Logger`](common.hpp ): Handles logging with thread safety.

### [`seed.cpp`](seed.cpp )

This file contains the implementation of the seed node.

#### [`SeedNode`](seed.cpp ) Class

- **Private Members**:
  - [`self`](seed.cpp ): Represents the seed node itself.
  - [`peerList`](seed.cpp ): List of registered peers.
  - [`peerListMutex`](seed.cpp ): Mutex for thread-safe access to [`peerList`](seed.cpp ).
  - [`logger`](peer.cpp ): Logger instance.
  - [`serverSocket`](peer.cpp ): Socket for accepting connections.
  - [`running`](peer.cpp ): Flag to indicate if the node is running.
  - [`workerThreads`](peer.cpp ): Vector of worker threads.
  - [`clientQueue`](seed.cpp ): Queue for handling client connections.

- **Private Methods**:
  - [`initializeSocket()`](peer.cpp ): Initializes the server socket.
  - [`acceptConnections()`](seed.cpp ): Accepts incoming connections.
  - [`handleClient()`](seed.cpp ): Handles client requests.
  - [`handleRegistration()`](seed.cpp ): Handles peer registration.
  - [`handlePeerListRequest()`](seed.cpp ): Handles requests for the peer list.
  - [`handleDeadNodeNotification()`](seed.cpp ): Handles notifications of dead nodes.
  - [`clientHandler()`](seed.cpp ): Processes client connections from the queue.

- **Public Methods**:
  - [`SeedNode()`](seed.cpp ): Constructor to initialize the seed node.
  - `start()`: Starts the seed node.
  - `~SeedNode()`: Destructor to clean up resources.

#### [`main`](peer.cpp ) Function

- Parses command-line arguments to get IP and port.
- Creates and starts a [`SeedNode`](seed.cpp ) instance.

### [`peer.cpp`](peer.cpp )

This file contains the implementation of the peer node.

#### [`PeerNode`](peer.cpp ) Class

- **Private Members**:
  - [`self`](peer.cpp ): Represents the peer node itself.
  - [`seedNodes`](peer.cpp ): List of seed nodes.
  - [`connectedPeers`](peer.cpp ): Set of connected peers.
  - [`messageList`](peer.cpp ): Map of messages and the peers that have seen them.
  - [`logger`](peer.cpp ): Logger instance.
  - [`peersMutex`](peer.cpp ): Mutex for thread-safe access to [`connectedPeers`](peer.cpp ).
  - [`messageMutex`](peer.cpp ): Mutex for thread-safe access to [`messageList`](peer.cpp ).
  - [`degreeGen`](peer.cpp ): Power-law degree generator.
  - [`messageQueue`](peer.cpp ): Queue for handling messages.
  - [`serverSocket`](peer.cpp ): Socket for accepting connections.
  - [`running`](peer.cpp ): Flag to indicate if the node is running.
  - [`messageCount`](peer.cpp ): Counter for generated messages.
  - [`workerThreads`](peer.cpp ): Vector of worker threads.
  - [`missedPings`](peer.cpp ): Map of peers and their missed pings.
  - [`peerSockets`](peer.cpp ): Map of peers and their sockets.

- **Private Methods**:
  - [`loadConfig()`](peer.cpp ): Loads seed nodes from the configuration file.
  - [`initializeSocket()`](peer.cpp ): Initializes the server socket.
  - [`connectToSeeds()`](peer.cpp ): Connects to seed nodes.
  - [`connectToSeed()`](peer.cpp ): Connects to a specific seed node.
  - [`processPeerList()`](peer.cpp ): Processes the peer list received from a seed node.
  - [`connectToPeer()`](peer.cpp ): Connects to a specific peer node.
  - [`generateGossipMessage()`](peer.cpp ): Generates gossip messages.
  - [`broadcastMessage()`](peer.cpp ): Broadcasts a message to connected peers.
  - [`messageSender()`](peer.cpp ): Sends messages from the queue to peers.
  - [`checkPeerLiveness()`](peer.cpp ): Checks the liveness of connected peers.
  - [`pingPeer()`](peer.cpp ): Pings a peer to check its liveness.
  - [`handleDeadPeer()`](peer.cpp ): Handles a dead peer.

- **Public Methods**:
  - [`PeerNode()`](peer.cpp ): Constructor to initialize the peer node.
  - `start()`: Starts the peer node.
  - `~PeerNode()`: Destructor to clean up resources.

#### [`main`](peer.cpp ) Function

- Parses command-line arguments to get IP, port, and configuration file.
- Creates and starts a [`PeerNode`](peer.cpp ) instance.

### [`config.txt`](config.txt )

This file lists the seed nodes in the format `IP:PORT`.

### [`.clang-format`](.clang-format )

Configuration file for the `clang-format` tool, specifying formatting rules for the codebase.

### [`.gitignore`](.gitignore )

Specifies files and directories to be ignored by Git, including:

- [`.clang-format`](.clang-format )
- [`peer`](peer )
- [`seed`](seed )
- `*.log`

## Usage

### Building the Project

To build the project, use the following commands:

```sh
g++ -o seed seed.cpp -lpthread -lssl -lcrypto
g++ -o peer peer.cpp -lpthread -lssl -lcrypto
```
Running the Seed Node
To run the seed node, use the following command:
./peer <ip> <port>
Running the Peer Node
./peer <ip> <port> <config_file>
To run the peer node, use the following command:
./peer 127.0.0.1 6000 config.txt

Example

./seed 127.0.0.1 5000