-module(gensocket_test).
-compile(export_all).

-ifdef(TEST).
-include_lib("eunit/include/eunit.hrl").
-define(PROXY_PORT, 8009).
-define(SERVER_PORT, 8010).
-define(IP, "127.0.0.1").
-define(OPTS, [{backlog, 128}, {reuseaddr, true}, {nodelay, true}]).
-define(MSG, <<"abc">>).
-define(INTEGER, 88).
-define(ACTIVEN, 43).

init_test() ->
    application:start(gen_socket).

basic_socket_operation_test_() ->
    {timeout, 60, ?_test(basic_socket_operation_test__())}.

basic_socket_operation_test__() ->
    PollSetSocketInfo = raw_socket:socketonline_info(),
    ?debugFmt("==================== basic_socket_operation_test begin ===================================", []),
    LSocket = listen(?SERVER_PORT),
    {ServerSocket, ClientSocket, ClientPid} = get_socket_pair(LSocket),
    ?assertNotMatch(ServerSocket, ClientSocket),

    %% Test setopts(), getopts()
    Opts1 = [{reuseaddr, false}, {nodelay, false}],
    ?assertMatch(ok, gen_socket:setopts(LSocket, Opts1)),
    ?assertMatch({ok, Opts1}, gen_socket:getopts(LSocket, [reuseaddr, nodelay])),

    Opts2 = [{reuseaddr, true}, {nodelay, false}],
    ?assertMatch(ok, gen_socket:setopts(LSocket, Opts2)),
    ?assertMatch({ok, Opts2}, gen_socket:getopts(LSocket, [reuseaddr, nodelay])),

    
    %% Test peername(), sockname()
    PeerNameServer = gen_socket:peername(ServerSocket),
    SockNameServer = gen_socket:sockname(ServerSocket),
    PeernameClient = gen_socket:peername(ClientSocket),
    SocknameClient = gen_socket:sockname(ClientSocket),

    %%?debugFmt("peer_server: ~p, sock_server: ~p, peer_client: ~p, sock_client: ~p, ~n", [PeerNameServer, SockNameServer, PeernameClient, SocknameClient]),

    ?assertMatch(PeerNameServer, SocknameClient),
    ?assertMatch(SockNameServer, PeernameClient),

    %% Test send(), recv(), controlling_process()
    ?assertMatch(ok, gen_socket:send(ClientSocket, [[[?INTEGER]], "abc",  [<<>>], [?MSG , <<>>, ?MSG]])),
    ?assertMatch(ok, gen_socket:send(ClientSocket, ?MSG)),
    ?assertMatch(ok, gen_socket:send(ClientSocket, ?MSG)),

    ?debugFmt("basic_socket_operation_test() send ok~n", []),
    
    %% Test {error,enomem}
    catch gen_socket:recv(ServerSocket, -1),
    ?assertMatch({error,enomem}, gen_socket:recv(ServerSocket, 16#4000000 + 1)),
    case gen_socket:recv(ServerSocket, 1) of
        {ok, Bin0} ->
            ?assertMatch(Bin0, <<?INTEGER>>),
            ok;
        Error0 ->
            ?assert(false)
    end,

    case gen_socket:recv(ServerSocket, erlang:size(?MSG)) of
        {ok, Bin1} ->
            ?assertMatch(Bin1, ?MSG),
            ok;
        Error1 ->
            ?assert(false)
    end,

    case gen_socket:recv(ServerSocket, erlang:size(?MSG)) of
        {ok, Bin2} ->
            ?assertMatch(Bin2, ?MSG),
            ok;
        Error2 ->
            ?assert(false)
    end,

    case gen_socket:recv(ServerSocket, erlang:size(?MSG)) of
        {ok, Bin3} ->
            ?assertMatch(Bin3, ?MSG),
            ok;
        Error3 ->
            ?assert(false)
    end,

    ?assertMatch({ok, <<"abcabc">>}, gen_socket:recv(ServerSocket, 0)),
    ?debugFmt("pollset socket count info:~p~n", [raw_socket:socketonline_info()]),
    ?debugFmt("ClientSocket ev_info:~p~n", [raw_socket:ev_info(ClientSocket)]),
    ?debugFmt("ClientSocket bind_id:~p~n", [raw_socket:socket_info(LSocket, bind_id)]),
    ?debugFmt("ClientSocket bind_id:~p~n", [raw_socket:socket_info(ServerSocket, bind_id)]),
    ?debugFmt("ClientSocket bind_id:~p~n", [raw_socket:socket_info(ClientSocket, bind_id)]),

    ?assertMatch({bind_id, 24}, raw_socket:socket_info(LSocket, bind_id)),
    ?assertMatch({bind_id, 23}, raw_socket:socket_info(ClientSocket, bind_id)),
    ?assertMatch({bind_id, 22}, raw_socket:socket_info(ServerSocket, bind_id)),

    %% Test getstat()
    ClientStats = [{recv_avg,0},
                   {recv_cnt,0},
                   {recv_dvi,0},
                   {recv_max,0},
                   {recv_oct,0},
                   {send_avg,5}, %% (3 + 3 + 3 + 1) / 2 = 5
                   {send_cnt,2},
                   {send_max,7},
                   {send_oct,10},
                   {send_pend,0}],

    ServerStats = [{recv_avg,2},
                   {recv_cnt,4},
                   {recv_dvi,0},
                   {recv_max,3},
                   {recv_oct,10},
                   {send_avg,0},
                   {send_cnt,0},
                   {send_max,0},
                   {send_oct,0},
                   {send_pend,0}],

    ?debugFmt("client:~n ~p~n", [gen_socket:getstat(ClientSocket)]),
    ?debugFmt("server:~n ~p~n", [gen_socket:getstat(ServerSocket)]),

    %%?assertMatch({ok, ClientStats}, gen_socket:getstat(ClientSocket)),
    %%?assertMatch({ok, ServerStats}, gen_socket:getstat(ServerSocket)),
    %%?debugFmt("getstat~p~n", [gen_socket:getstat(ServerSocket)]),

    %% Test close()
    ?assertMatch(ok, gen_socket:close(ServerSocket)),
    ?assertMatch(ok, gen_socket:close(ClientSocket)),
    ?assertMatch(ok, gen_socket:close(LSocket)),

    ?assertMatch({error, closed}, gen_socket:send(ClientSocket, ?MSG)),
    ?assertMatch({error, closed}, gen_socket:send(ClientSocket, [?MSG])),

    ?assertMatch(PollSetSocketInfo, raw_socket:socketonline_info()).

listen(Port) ->
    {ok, LSocket} = gen_socket:listen(Port, ?OPTS),
    ?debugFmt("Listen ok ~p~n", [Port]),
    LSocket.

accept(Parent, LSocket) ->
    receive
        ready -> ok
    end,
    ?debugFmt("Begin Accept LSocket: ~p~n", [LSocket]),
    {ok, S} = gen_socket:accept(LSocket),
    ?debugFmt("Accept NewSocket: ~p~n", [S]),
    ok = gen_socket:controlling_process(S, Parent),
    ok = gen_socket:controlling_process(LSocket, Parent),
    Parent ! {server, S}.

connect(Parent) ->
    {ok, S} = gen_socket:connect(?IP, ?SERVER_PORT, []),
    ?debugFmt("Connect ok ~p~n", [S]),
    ok = gen_socket:controlling_process(S, Parent),
    Parent ! {client, S},
    receive
        hold -> hold
    end.

%% Spwan 2 process: client & server
%% Return: {ServerSocket, ClientSocket, ClientPid}.
get_socket_pair(LSocket)  ->
    Pid = self(),
    ServerPid = spawn(fun() -> accept(Pid, LSocket) end),
    ok = gen_socket:controlling_process(LSocket, ServerPid),
    ServerPid ! ready,
    
    ClientPid = spawn(fun() -> connect(Pid) end),
    ?debugFmt("Spawn client&server process ok~n", []),

    ServerSocket = receive
                       {server, SS1} -> SS1
                   end,
    ClientSocket = receive
                       {client, SS2} -> SS2
                   end,
    {ServerSocket, ClientSocket, ClientPid}.            


proxy_test_() ->
    {timeout, 60, ?_test(proxy_test__())}.
proxy_test__() ->
    ?debugFmt("==================== proxy_test begin ===================================", []),
    ServerPid = spawn(fun() -> echo_server() end),
    ProxyPid = spawn(fun() -> proxy_server() end),

    {ok, ClientSocket} = gen_socket:connect(?IP, ?PROXY_PORT, []),
    ?assertMatch(ok, gen_socket:send(ClientSocket, ?MSG)),
    {ok, Data} = gen_socket:recv(ClientSocket, erlang:size(?MSG)),
    ?assertMatch(Data, ?MSG),
    ?assertMatch(ok, gen_socket:close(ClientSocket)),
    ?debugFmt("Proxy Test End", []),
    ?debugFmt("==================== proxy_test end ===================================~n~n", []).

echo_server() ->
    {ok, LSocket} = gen_socket:listen(?SERVER_PORT, ?OPTS),
    {ok, ServerSocket} = gen_socket:accept(LSocket),
    {ok, Data} = gen_socket:recv(ServerSocket, erlang:size(?MSG)),
    ?assertMatch(ok, gen_socket:send(ServerSocket, Data)),

    ?assertMatch(ok, gen_socket:close(LSocket)),
    ?assertMatch(ok, gen_socket:close(ServerSocket)).
        
proxy_server() ->
    {ok, LSocket} = gen_socket:listen(?PROXY_PORT, ?OPTS),
    {ok, ClientSocket} = gen_socket:accept(LSocket),
    {ok, ServerSocket} = gen_socket:connect(?IP, ?SERVER_PORT, []),

    ?assertMatch(ok, gen_socket:setopts(ClientSocket, [{active, ?ACTIVEN}])),
    ?assertMatch(ok, gen_socket:setopts(ServerSocket, [{active, ?ACTIVEN}])),
    proxy_loop(ServerSocket, ClientSocket).

proxy_loop(ServerSocket, ClientSocket) ->
    receive
        {tcp, ServerSocket, Data} ->
            ?debugFmt("Proxy Server, received a msg from server, Socket: ~p, msg: ~p~n", [ServerSocket, Data]),
            ?assertMatch(ok, gen_socket:send(ClientSocket, Data)),
            proxy_loop(ServerSocket, ClientSocket);
        {tcp, ClientSocket, Data} ->
            ?debugFmt("Proxy Server, received a msg from client, Socket: ~p, msg: ~p~n", [ClientSocket , Data]),
            ?assertMatch(ok, gen_socket:send(ServerSocket, Data)),
            proxy_loop(ServerSocket, ClientSocket);
        {tcp_closed, ServerSocket} -> 
            ?debugFmt("Proxy Server, ServerSocket closed: ~p~n", [ServerSocket]),
            proxy_loop(ServerSocket, ClientSocket);
        {tcp_closed, ClientSocket} -> 
            ?debugFmt("Proxy Server, ClientSocket closed: ~p~n", [ClientSocket]),
            proxy_loop(ServerSocket, ClientSocket);
        {tcp_passive, Socket} ->
            ?debugFmt("Proxy Server, Passive received, Socket: ~p~n", [Socket]),
            ok = gen_socket:setopts(Socket, [{active, 1}]),
            proxy_loop(ServerSocket, ClientSocket);
        {tcp_error, Socket, _Reason} -> 
            ?debugFmt("Proxy Server, Error, Socket: ~p, Reason~n", [Socket, _Reason]),
            {stop, ssock_error};
        _Msg -> 
            ?debugFmt("Proxy Server, Unkown Msg: ~p~n", [_Msg]),
            proxy_loop(ServerSocket, ClientSocket)
    end.


multi_process_accept_test_() ->
    {timeout, 60, ?_test(multi_process_accept_test__())}.
multi_process_accept_test__() ->    
    ?debugFmt("==================== multi_process_accept_test begin ===================================", []),
    LSocket = listen(?SERVER_PORT),
    Pid = self(),
    P1 = spawn(fun() -> do_multi_process_accept(LSocket, Pid) end),
    receive
        ready -> ok;
        _ -> ?assert(false)
    end,

    P2 = spawn(fun() -> do_multi_process_accept(LSocket, Pid) end),
    receive
        ready -> ok;
        _ -> ?assert(false)
    end,

    L1 = spawn(fun() -> do_multi_process_connect() end),
    L2 = spawn(fun() -> do_multi_process_connect() end),

    receive
        {fin, P11} when P11=:=P1 orelse P11=:=P2 ->
            ?debugFmt("multi_process_accept_test, P1 accept ok~n", []),
            ?assert(true);
        R1 -> 
            ?debugFmt("1, multi_process_accept_test, unknown msg: ~p~n", [R1]),
            ?assert(false)
    end,

    receive
        {fin, P22} when P22=:=P1 orelse P22=:=P2 ->
            ?assert(true),
            ?debugFmt("multi_process_accept_test, P2 accept ok~n", []);
        R2 -> 
            ?debugFmt("2, multi_process_accept_test, unknown msg: ~p~n", [R2]),
            ?assert(false)
    end,
                    
    L3 = spawn(fun() -> do_multi_process_connect() end),
    L4 = spawn(fun() -> do_multi_process_connect() end),
    L5 = spawn(fun() -> do_multi_process_connect() end),

    P3 = spawn(fun() -> do_multi_process_accept(LSocket, Pid) end),
    receive
        ready -> ok
    end,

    P4 = spawn(fun() -> do_multi_process_accept(LSocket, Pid) end),
    receive
        ready -> ok
    end,

    P5 = spawn(fun() -> do_multi_process_accept(LSocket, Pid) end),
    receive
        ready -> ok
    end,
    
    receive
        {fin, P33} when P33=:=P3 orelse P33=:=P4 orelse P33=:=P5 ->
            ?debugFmt("multi_process_accept_test, P3 accept ok~n", []), 
            ?assert(true);
        R3 -> 
            ?debugFmt("3, multi_process_accept_test, unknown msg: ~p~n", [R3]),
            ?assert(false)
    end,

    receive
        {fin, P44} when P44=:=P3 orelse P44=:=P4 orelse P44=:=P5 -> 
            ?debugFmt("multi_process_accept_test, P4 accept ok", []), 
            ?assert(true);
        R4 -> 
            ?debugFmt("4, multi_process_accept_test, unknown msg: ~p", [R4]),
            ?assert(false)
    end,

    receive
        {fin, P55} when P55=:=P3 orelse P55=:=P4 orelse P55=:=P5 -> 
            ?debugFmt("multi_process_accept_test, P5 accept ok~n", []), 
            ?assert(true);
        R5 -> 
            ?debugFmt("5, multi_process_accept_test, unknown msg: ~p~n", [R5]),
            ?assert(false)
    end,

    ?assertMatch(ok, gen_socket:close(LSocket)),
    ?debugFmt("==================== multi_process_accept_test end ===================================~n~n", []).

do_multi_process_connect() ->
    {ok, ServerSocket} = gen_socket:connect(?IP, ?SERVER_PORT, []),
    ?assertMatch(ok, gen_socket:close(ServerSocket)).
    
do_multi_process_accept(LSocket, Parent) ->
    {ok, NewSock} = gen_socket:dup_socket(LSocket),
    ?debugFmt("do_multi_process_accept begin, LSocket: ~p, NewLSocket: ~p~n", [LSocket, NewSock]),
    Parent !  ready,
    {ok, ClientSocket} = gen_socket:accept(NewSock),
    ?assertMatch(ok, gen_socket:close(ClientSocket)),
    ?debugFmt("do_multi_process_accept end~n", []), 
    Parent ! {fin, self()}.

%% receive_msg in other process
multi_process_recv_test_() ->
    {timeout, 60, ?_test(multi_process_recv_test__())}.

multi_process_recv_test__() ->
    ?debugFmt("==================== multi_process_recv_test begin ===================================", []),
    LSocket = listen(?SERVER_PORT),
    {ServerSocket, ClientSocket, ClientPid} = get_socket_pair(LSocket),
    ?assertNotMatch(ServerSocket, ClientSocket),

    Opts = [{active, false}],
    ?assertMatch(ok, gen_socket:setopts(ServerSocket, Opts)),
    ?assertMatch(ok, gen_socket:setopts(ClientSocket, Opts)),

    Self = self(),
    ReceivePid = spawn(fun() -> do_receive(ServerSocket, ClientSocket, Self)  end),

    receive
        ready -> ok;
        R0 ->
            ?debugFmt("0, multi_process_recv_test received an unkonw msg: ~p~n", [R0]),
            ?assert(false)
    end,

    %% Insure do_receive recv() run before send
    timer:sleep(1000),
    ?assertMatch(ok, gen_socket:send(ClientSocket, [?INTEGER])),

    receive
        ready -> ok;
        R1 ->
            ?debugFmt("1, multi_process_recv_test received an unkonw msg: ~p~n", [R1]),
            ?assert(false)
    end,
    %% Insure do_receive recv() run before send
    timer:sleep(1000),
    ?assertMatch(ok, gen_socket:send(ClientSocket, ?MSG)),

    receive
        fin -> ok;
        R2 ->
            ?debugFmt("multi_process_recv_test received an unkonw msg: ~p~n", [R2]),
            ?assert(false)
    end,
    ?assertMatch(ok, gen_socket:send(ClientSocket, ?MSG)),

    case gen_socket:recv(ServerSocket, erlang:size(?MSG)) of
        {ok, Bin3} ->
            ?assertMatch(Bin3, ?MSG),
            ok;
        Error3 ->
            ?assert(false)
    end,

    %% Test close()
    ?assertMatch(ok, gen_socket:close(ServerSocket)),
    ?assertMatch(ok, gen_socket:close(ClientSocket)),
    ?assertMatch(ok, gen_socket:close(LSocket)),
    ?debugFmt("==================== multi_process_recv_test end ===================================~n~n", []).



do_receive(ServerSocket, ClientSocket, Parent) ->
    ?debugFmt("Another process begin to recv 3 msg~n", []),
    Parent ! ready,
    case gen_socket:recv(ServerSocket, 1) of
        {ok, Bin0} ->
            ?assertMatch(Bin0, <<?INTEGER>>),
            ok;
        Error0 ->
            ?assert(false)
    end,

    Parent ! ready,
    case gen_socket:recv(ServerSocket, erlang:size(?MSG)) of
        {ok, Bin1} ->
            ?assertMatch(Bin1, ?MSG),
            ok;
        Error1 ->
            ?assert(false)
    end,


    ?assertMatch(ok, gen_socket:send(ClientSocket, ?MSG)),
    case gen_socket:recv(ServerSocket, erlang:size(?MSG)) of
        {ok, Bin2} ->
            ?assertMatch(Bin2, ?MSG),
            ok;
        Error2 ->
            ?assert(false)
    end,

    ?debugFmt("Another process end to recv 3 msg~n", []),
    Parent ! fin.
    
drain_message() ->
    receive
        _Msg -> 
            ?debugFmt("Message: ~p~n", [_Msg]),
            drain_message()
    after 0 ->
        ok
    end.

-endif.
