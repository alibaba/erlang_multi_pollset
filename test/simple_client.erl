-module(simple_client).
-export([start/0]).

start() ->
    %% MUST start gen_socket app!!!
    application:start(gen_socket),
    {ok, S} = gen_socket:connect("127.0.0.1", 8008, []),
    ok = gen_socket:setopts(S, [{active, true}]),
    loop(S, 5).

loop(S, 0) ->
    gen_socket:close(S);
loop(S, N) ->
    gen_socket:send(S, <<"hello">>), 
    receive
        {tcp, S, D} ->
	    io:format("client receive data: ~p~n", [D]),
	    loop(S, N-1);
	{tcp_closed, S} ->
	    io:format("client socket closed~n", []);
        {tcp_error, S, R} ->
	    io:format("client socket error, Reason:~p ~n", [R]);
	Err ->
	    io:format("client socket receive error, Err:~p ~n", [Err])
    end.
