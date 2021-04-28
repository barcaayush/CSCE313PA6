# makefile

all: server client

common.o: common.h common.cpp
	g++ -g -w -std=c++11 -c common.cpp

TCPreqchannel.o: TCPreqchannel.h TCPreqchannel.cpp
	g++ -g -w -std=c++11 -c TCPreqchannel.cpp

Histogram.o: Histogram.cpp Histogram.h HistogramCollection.h
	g++ -g -w -std=c++11 -c Histogram.cpp

client: client.cpp Histogram.o TCPreqchannel.o common.o BoundedBuffer.h HistogramCollection.h
	g++ -g -w -std=c++11 -o client client.cpp Histogram.o TCPreqchannel.o common.o -lpthread -fsanitize=address -fno-omit-frame-pointer

server: server.cpp  TCPreqchannel.o common.o 
	g++ -g -w -std=c++11 -o  server server.cpp TCPreqchannel.o common.o -lpthread -fsanitize=address -fno-omit-frame-pointer

clean:
	rm -rf *.o *.csv fifo* server client data*_*
