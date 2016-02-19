-module(simple_server).
-export([start/0]).

start() ->
    %% MUST start gen_socket app!!!
    application:start(gen_socket),
    {ok, L} = gen_socket:listen(8008, 
				[{backlog, 128}, 
				 {reuseaddr, true}, 
				 {nodelay, true}]),
    do_accept(L).

do_accept(L) ->
    {ok, S} = gen_socket:accept(L),
    io:format("server accept a new socket~n"),
    Pid = spawn(fun() -> loop(S) end),
    gen_socket:controlling_process(S, Pid),
    do_accept(L).

loop(S) ->
    case gen_socket:recv(S, 0) of
	{ok, D} ->
	    io:format("server receive data: ~p~n", [D]),
	    gen_socket:send(S, D),
	    loop(S);
	{error, closed} ->
	    io:format("server socket closed~n", []);
	Err ->
	    io:format("server socket error: ~p~n", [Err])
    end.
