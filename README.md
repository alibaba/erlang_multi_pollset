 README for OTP_GEN_SOCKET
 ========================================================================

## TABLE OF CONTENT ##

* [Introduction](#introduction)
* [Compile](#compile)
* [Example](#example)
* [Design](#Design)

## Introduction ##

We all know gen_tcp is a standard network module in Erlang world.
But Erlang vm supports only one PollSet which means there is at most one
os thread handling events in the same time.
The way only one PollSet in the vm is not scalable, especially with a rapid growth of NIC bandwidth and CPU cores on one single machine.

In order to make full use of multi-core in Eralng network programming, we develop gen_socket module.
The gen_socket is completely compatible with gen_tcp interface, but with amazing features:
- PollSet per scheduler. Meaning the gen_sockete perfectly solved the problem there is at most one PollSet in erlang vm.
- Socket binding policy. The gen_socket supports binding socket to specific scheduler and specific PollSet. Which reduces thread switchings and cache missing.

According to our experiment, gen_socket increases throughput of echo server by **110%** on a 24-core, 1000Mb/s machine based on redhat6.2.

gen_socket has already deployed in the Aliyun RDS production environment.
gen_socket is released under GPLv2.

## Compile ##

1) git clone https://github.com/max-feng/otp_gen_socket.git
2) cd gen_socket && make

## Example ##
### server side ###
* start gen_socket application

   max@max-gentoo ~/Code/gen_socket $ erl -pa ./ebin/
   Erlang/OTP 17 [erts-6.4] [source] [64-bit] [smp:4:4] [async-threads:10] [hipe] [kernel-poll:false]

   Eshell V6.4  (abort with ^G)
   1> application:start(gen_socket).
   BindPoller=true, BindSocketPolicy=1

* listen

   2> {ok, L } = gen_socket:listen(8008, []).
      {ok,{140091975271824,<<>>}}

* accept
   When client side connect to 8008, accept() will return a gen_socket:

   3> {ok, S} = gen_socket:accept(L).
      {ok,{140091975272200,<<>>}}

* send

   4> gen_socket:send(S, <<"hello">>).
      ok

### client side ###
* start gen_socket application

    max@max-gentoo ~/Code/gen_socket $ erl -pa ./ebin/
    Erlang/OTP 17 [erts-6.4] [source] [64-bit] [smp:4:4] [async-threads:10] [hipe] [kernel-poll:false]

    Eshell V6.4  (abort with ^G)
    1> application:start(gen_socket).
    BindPoller=true, BindSocketPolicy=1

* connect

    2> {ok, S} = gen_socket:connect("127.0.0.1", 8008, []).
       {ok,{140532682066528,<<>>}}

* recv

    3> gen_socket:recv(S, 0).
       {ok,<<"hello">>}

## Design ##

    The gen_socket supports one PollSet per scheduler. The key concept is PollSet struct, Poller Process and Watcher Thread.
    PollSet struct is IO events affiliated point. PollSet is based on libev.
    Poller Process is an Erlang process which is executed in Erlang scheduler. It polls IO events from PollSet in nonblocking way.
    Watcher Thread is an os thread. When no IO events in PollSet, Watcher Thread will take over the right of polling the PollSet.

### Multi PollSet ###
### Binding ###

