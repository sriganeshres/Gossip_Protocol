

# Gossip Protocol-based P2P Network

This project implements a gossip protocol over a peer-to-peer (P2P) network. The network consists of **Seed Nodes** and **Peer Nodes**. The seed nodes act as bootstrapping servers, helping new peers join the network by providing them with a list of active peers. The peers then establish connections with a subset of these peers, exchange gossip messages, and periodically check the liveness of their neighbors.

## Features & Tasks Implemented

- **Seed Node Functionality:**
  - Listens for incoming connections from peers.
  - Registers new peers upon receiving a `Register` message.
  - Maintains and updates a peer list (with each peer’s IP, port, and degree).
  - Responds to peer requests by sending the current peer list.
  - Handles updates (via `UpdateDegree` messages) and dead node reports (`DeadNode` messages).

- **Peer Node Functionality:**
  - Loads a list of seed nodes from a configuration file.
  - Registers with at least ⌊n/2⌋+1 seed nodes (where n is the number of seeds).
  - Retrieves and unifies peer lists from different seeds.
  - Selects a subset of peers (using a weighted, power-law based random selection) to establish TCP connections.
  - Runs its own server to accept incoming connections from other peers.
  - Broadcasts gossip messages periodically (every 5 seconds) for a fixed number of messages.
  - Periodically monitors liveness of connected peers (every 13 seconds) by sending heartbeat messages. If a peer fails to respond to three consecutive heartbeats, it is reported as dead to all seed nodes.

- **Gossip Protocol:**
  - Each gossip message is formatted as `<timestamp>:<senderIP>:<senderPort>:<messageID>`.
  - Each peer keeps track of messages it has forwarded to avoid duplicates.

## Project Structure

- `seed.cpp`: Implements the seed node functionality.
- `peer.cpp`: Implements the peer node functionality.
- `seed.config`: Sample configuration file for seeds (used by peers).

## Requirements

- A C++ compiler supporting C++11 (or later).
- POSIX-compliant environment (Linux or macOS) for socket APIs.
- `g++` is recommended.

## Building the Project

Compile the seed and peer nodes separately using the following commands:

```bash
g++ -std=c++11 -pthread seed.cpp -o seed
g++ -std=c++11 -pthread peer.cpp -o peer
```

## Running the Project

### Step 1: Start Seed Nodes

For testing, run **4 seed nodes**. For example, open 4 separate terminal windows and run:

```bash
./seed 127.0.0.1 5000
./seed 127.0.0.1 5001
./seed 127.0.0.1 5002
./seed 127.0.0.1 5003
```

Each seed node will output log messages to the console and write logs to files like `seed-5000.log`.

### Step 2: Create a Seed Configuration File for Peers

Create a file named `seed.config` with the following content (each line represents a seed node):

```
127.0.0.1:5000
127.0.0.1:5001
127.0.0.1:5002
127.0.0.1:5003
```

Place this file in the same directory as the `peer` executable (or provide its path).

### Step 3: Start Peer Nodes

For testing, run **7-8 peer nodes**. Open several terminal windows and execute commands like:

```bash
./peer 127.0.0.1 6000 seed.config
./peer 127.0.0.1 6001 seed.config
./peer 127.0.0.1 6002 seed.config
./peer 127.0.0.1 6003 seed.config
./peer 127.0.0.1 6004 seed.config
./peer 127.0.0.1 6005 seed.config
./peer 127.0.0.1 6006 seed.config
```

Each peer node will:
- Load the seed configuration.
- Connect to at least ⌊(n/2)⌋+1 seeds (in this example, at least 3 seeds).
- Retrieve the peer list and then connect to a subset of the discovered peers.
- Start its own server for incoming connections.
- Begin broadcasting gossip messages.
- Monitor the liveness of connected peers.

Peer logs will be written to files named `peer-<port>.log` (e.g., `peer-6000.log`).

## Troubleshooting

- **No Seeds Loaded Error:**  
  If you see an error like "No seeds loaded," ensure that your `seed.config` file is correctly formatted and placed in the expected directory.
  
- **Port Conflicts:**  
  Ensure that the chosen ports (for both seed and peer nodes) are available and not blocked by your firewall.

- **Debugging:**  
  Both seed and peer programs log their activities. Check the console output and corresponding log files (e.g., `seed-5000.log` or `peer-6000.log`) for detailed debugging information.

## Final Remarks

This project demonstrates a robust implementation of a gossip protocol over a P2P network with dynamic peer discovery, registration, gossip broadcasting, and liveness monitoring. The system is designed for educational purposes and can be extended further to improve security and scalability.


