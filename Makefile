make_peer: peer.cpp 
	g++ -o peer peer.cpp -std=c++17 -pthread -lcrypto
make_seed: seed.cpp
	g++ -o seed seed.cpp -std=c++17 -pthread -lcrypto 

remove_logs: *.log
	rm -f *.log

gen_seeds:
	./seed 127.0.0.1 5000 &
	./seed 127.0.0.1 5001 &
	./seed 127.0.0.1 5002 &

gen_peers:
	./peer 127.0.0.1 6000 config.txt &
	./peer 127.0.0.1 6001 config.txt &
	./peer 127.0.0.1 6002 config.txt &