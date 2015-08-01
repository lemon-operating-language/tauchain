CC=g++


all: tau



release: MODE=release tau


ifeq ($(mode), release)
	CXXFLAGS=-c -std=c++11 -Wextra -DNDEBUG -O2
else
	CXXFLAGS=-c -std=c++11 -Wextra -DDEBUG  -O0 -g -ggdb 


tau: $(OBJECTS) $(EXECUTABLE)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)


CXXFLAGS += -I/usr/local/include
LDFLAGS= -L/usr/local/lib -ldl -pthread -lrt

OBJECTS=tau.o jsonld.o rdf.o misc.o object.o cli.o prover.o nquads.o
%.o: %.cpp `g++ -std=c++11 $(CXXFLAGS) -M %.cpp`


clean:
	rm -rf tau $(OBJECTS) ubi/client.o marpa.o marpa_tau.o


cl: CXXFLAGS += -DOPENCL
irc: CXXFLAGS += -DIRC -DDEBUG


with_marpa: marpa_tau.o libmarpa/dist/.libs/libmarpa.so
libmarpa/dist/.libs/libmarpa.so:
	git submodule init
	git submodule update
	cd libmarpa;	make dist;	cd dist;	./configure;	make
with_marpa: OBJECTS += marpa_tau.o
with_marpa: CXXFLAGS += -Dwith_marpa  -I libmarpa/dist -ggdb  #-Ilexertl
with_marpa: LDFLAGS += -Llibmarpa/dist/.libs -lmarpa  -ggdb -lboost_regex



with_marpa: tau

irc: $(OBJECTS) $(EXECUTABLE)
	$(CC) $(OBJECTS) -o tau $(LDFLAGS)
cl: $(OBJECTS) $(EXECUTABLE)
	$(CC) $(OBJECTS) -o tau $(LDFLAGS) -lOpenCL
ubi-tau: $(OBJECTS) ubi/client.o
	$(CC) $(OBJECTS) ubi/client.o -o $@ $(LDFLAGS)
.cpp.o:
	$(CC) $(CXXFLAGS) $< -o $@
$(EXECUTABLE): $(OBJECTS) 
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

ppjson: ppjson.cpp
	$(CC) -std=c++11 ppjson.cpp -oppjson -Wall -ggdb


# http://stackoverflow.com/questions/792217/simple-makefile-with-release-and-debug-builds-best-practices
