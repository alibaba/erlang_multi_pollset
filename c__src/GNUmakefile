ERLANG_PATH = $(shell erl -eval 'io:format("~s", [lists:concat([code:root_dir(), "/erts-", erlang:system_info(version), "/include"])])' -s init stop -noshell)
NIF_INCLUDE:=-I./ -I$(ERLANG_PATH)

CFLAGS = -g -Wall -O2 -fno-strict-aliasing -DEV_STANDALONE $(NIF_INCLUDE)

ifneq ($(OS),Windows_NT)
	CFLAGS += -fPIC

	ifeq ($(shell uname),Darwin)
		LDFLAGS += -dynamiclib -undefined dynamic_lookup
	else
		LDFLAGS += -shared
	endif
endif

all: ../priv/raw_socket.so

raw_socket.o : %.o: %.cpp
	c++ $(CFLAGS) $(LDFLAGS) -c $<

ev.o: %.o: %.c 
	cc $(CFLAGS) -Wno-unused-value -Wno-unused-function -c $<

../priv/raw_socket.so : raw_socket.o ev.o
	c++ $(CFLAGS) $(LDFLAGS) -o $@ $^

clean:
	rm -f *.o

