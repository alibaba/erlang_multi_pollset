> README for erlang_multi_pollset 
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
```
	git clone https://github.com/max-feng/erlang_multi_pollset.gi
	cd gen_socket && make
```
## Example ##
### server side ###
* start gen_socket application
```
	max@max-gentoo ~/Code/gen_socket $ erl -pa ./ebin/
	Erlang/OTP 17 [erts-6.4] [source] [64-bit] [smp:4:4] [async-threads:10] [hipe] [kernel-poll:false]
	
	Eshell V6.4  (abort with ^G)
	1> application:start(gen_socket).
	BindPoller=true, BindSocketPolicy=1
```
* listen
```
	2> {ok, L } = gen_socket:listen(8008, []).
	{ok,{140091975271824,<<>>}}
```
* accept
   When client side connect to 8008, accept() will return a gen_socket:
```
	3> {ok, S} = gen_socket:accept(L).
	{ok,{140091975272200,<<>>}}
```
* send
```
	4> gen_socket:send(S, <<"hello">>).
	ok
```
### client side ###
* start gen_socket application
```
	max@max-gentoo ~/Code/gen_socket $ erl -pa ./ebin/
	Erlang/OTP 17 [erts-6.4] [source] [64-bit] [smp:4:4] [async-threads:10] [hipe] [kernel-poll:false]
	
	Eshell V6.4  (abort with ^G)
	1> application:start(gen_socket).
	BindPoller=true, BindSocketPolicy=1
```
* connect
```
	2> {ok, S} = gen_socket:connect("127.0.0.1", 8008, []).
	{ok,{140532682066528,<<>>}}
```
* recv
```
	3> gen_socket:recv(S, 0).
	{ok,<<"hello">>}
```
## Design ##

The gen_socket supports one PollSet per scheduler. The key concept is PollSet struct, Poller Process and Watcher Thread.

PollSet struct is IO events's affiliated point. PollSet is based on libev.

Poller Process is an Erlang process which is executed in Erlang scheduler. It polls IO events from PollSet in nonblocking way.

Watcher Thread is an os thread. When there is no IO events in PollSet, Watcher Thread will take over the right of polling the PollSet.

### Current Architecture ###
![enter description here][1]

In Erlang VM all schedulers are in follower/leader mode. There is at most one scheduler thread handing IO events.

### Multi PollSet ###
![enter description here][2]

In gen_socket module, there is a PollSet per scheduler.

In above image, One PollSet has 2 Poller Process.

Poller Process call evrun() in nonblocking way through Erlang nif interface.

When evrun() return some IO events, callback will be processed in Poller Process. Callbck just ::recv() data, convert received data to Erlang binary, and send it to the socket's owner;
When evrun() return zero IO events, Poller Process will give up the right to use PollSet, Watcher Thread will take over the right to use PollSet. Poller Process will be blocked on receive primitive.

Watcher Thread call evrun() in blocking way. So when there is no IO events in PollSet, Watch Thread will be blocked and in interruptible state; When  there are some new IO events coming, Watch Thread will be waked up, and send msg to Poller Process. Poller Process will enter into next loop.

### Binding ###
![enter description here][3]

Process's socket managed by PollSet. 
When creating a new socket, gen_socket selects a minimum PollSet by a min-heap. And bind this socket to this PollSet, meanwhile bind this process to PollSet's scheduler.

This binding way can reach the effect that an IO event handed by only one scheduler.


  [1]: ./images/gen_tcp_pollset.PNG "gen_tcp_pollset.PNG"
  [2]: ./images/gen_socket_pollset.PNG "gen_socket_pollset.PNG"
  [3]: ./images/gen_socket_pollet_binding.PNG "gen_socket_pollet_binding.PNG"
