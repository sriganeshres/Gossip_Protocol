# P2P Gossip Protocol Implementation

This project implements a peer-to-peer (P2P) gossip protocol over a network using two types of nodes:

1. **Seed Nodes:**  
   Seed nodes are used as bootstrap nodes. They maintain a Peer List (PL) with the IP address, port, and fixed degree (i.e. the number of persistent connections) of each registered peer. When a new peer registers, a seed node adds the peer (if not already registered) and immediately returns its current Peer List. Seed nodes do not update a peer’s degree after the initial registration. They also remove a peer from the PL when they receive a dead–node notification.

2. **Peer Nodes:**  
   Peer nodes first read a configuration file (e.g., `seed.config`) that contains the list of seed nodes (one per line, in the format `IP:Port`). A peer:
   - **Registration:** Randomly selects at least ⌊(n/2)⌋+1 seed nodes (where *n* is the total number of seeds) and registers with them using a registration message (`Register:<self.IP>:<self.Port>:0`). It then receives the current peer lists from the seeds and forms a union.
   - **Peer Selection:** Using a weighted (power–law) random selection (weight = degree + 2), the peer chooses a subset of discovered peers to establish persistent TCP connections. The degree is set only at the time of the first registration.
   - **Gossip Messaging:** Every 5 seconds, a peer generates a gossip message in the format `<timestamp>:<self.IP>:<MsgID>` (for a total of 10 messages). Each peer maintains a Message List (ML) so that each message is forwarded only once. New messages are logged and forwarded to all persistent connections (except the sender).
   - **Liveliness Checking:** Every 13 seconds, the peer pings each connected peer using a temporary connection (via a Ping/Pong handshake) that does not affect the persistent connection’s degree count. If a peer fails to respond to 3 consecutive pings, a dead–node message (`Dead Node:<deadIP>:<deadPort>:<timestamp>:<self.IP>`) is sent to all seeds, and the persistent connection to that peer is closed.

## Files

- **seed.cpp:** Contains the code for the Seed Node.  
  The seed node listens for incoming connections, processes registration and dead–node messages, and logs its Peer List (with degrees) to both console and a file (`seed_metrics.txt`) every 5 seconds.

- **peer.cpp:** Contains the code for the Peer Node.  
  The peer node registers with seed nodes, discovers other peers, establishes persistent TCP connections (using weighted, power–law selection), generates and broadcasts gossip messages, and performs periodic liveliness checks.

## Compilation

Make sure you have a C++11 (or later) compliant compiler. For example, using `g++`:

```bash
g++ -std=c++11 -pthread seed.cpp -o s
g++ -std=c++11 -pthread peer.cpp -o p
```

This will create two executables:
- `s` for seed nodes.
- `p` for peer nodes.

## Running the Nodes

You can run the nodes in separate terminal tabs or windows.

### Starting Seed Nodes

For example, to start four seed nodes on the following IP/ports:

```bash
./s 127.0.0.1 5000
./s 127.0.0.1 5001
./s 127.0.0.1 5002
./s 127.0.0.1 5003
```

Each seed node will:
- Listen on its assigned port.
- Accept peer registration requests.
- Maintain and log the current Peer List and degree metrics (written to `seed_metrics.txt`).

### Running Peer Nodes

Create a configuration file (e.g., `seed.config`) with the seed node details, one per line:

```
127.0.0.1:5000
127.0.0.1:5001
127.0.0.1:5002
127.0.0.1:5003
```

Then, start multiple peer nodes on different IPs/ports. For example:

```bash
./p 127.0.0.1 9001 seed.config
./p 127.0.0.2 9002 seed.config
./p 127.0.0.3 9003 seed.config
./p 127.0.0.4 9004 seed.config
./p 127.0.0.5 9005 seed.config
```

Each peer node will:
- Read the seed configuration and randomly register with at least ⌊(n/2)⌋+1 seeds.
- Receive and union the peer lists from seeds.
- Select a subset of peers to establish persistent TCP connections using weighted random selection (weight = degree + 2).
- Generate and broadcast gossip messages (10 messages, one every 5 seconds). Each message is forwarded only once, ensuring no duplicates.
- Periodically (every 13 seconds) ping connected peers using a temporary Ping/Pong handshake. If 3 consecutive pings fail, the peer is marked as dead, a dead–node message is sent to all seeds, and the connection is closed.
- Log all events (registration, gossip, and liveliness events) to `peer-<LocalPort>.log` and `liveliness-<LocalPort>.txt`.

## Summary

- **Degree is set only during the initial registration** and remains unchanged.
- **Peer selection uses weighted (power–law) random selection** with weight = (degree + 2).
- **Gossip messages** are generated and reliably forwarded throughout the network, with each message forwarded only once.
- **Liveliness checking** uses temporary Ping/Pong connections that do not affect persistent connection counts.
- **All events are logged** appropriately to help with debugging and performance monitoring.
