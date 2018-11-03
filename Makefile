default: server client

server:
	g++ -std=c++14 dapper.cpp -o dapper

client:
	g++ -std=c++14 dapperc.cpp -o dapperc
