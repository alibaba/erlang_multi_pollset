-module(gen_socket).
-export([connect/3, connect/4, listen/2, setopts/2, accept/1, accept/2,
         send/2, recv/2, recv/3, controlling_process/2, getstat/1, getstat/2, 
         socket_info/2, peername/1, sockname/1, getopts/2, close/1,
         dup_socket/1]).

-include("gen_socket.hrl").

connect(Address, Port, Options) ->
    connect(Address, Port, Options, infinity).

connect(Address, Port, Options, Timeout) ->
    {ok, IPAddress} = to_ip_address(Address),
    {PostOptions, PreOptions} = partition_options(Options, [active]),
    case create_socket(Options) of
        {ok, Socket} ->
            case do_connect_1(IPAddress, Port, PreOptions, Timeout, Socket) of
                ok    -> case setopts(Socket, PostOptions) of
                            ok -> {ok, Socket};
                            Error -> raw_socket:close(Socket), Error
                        end;
                Error -> raw_socket:close(Socket), Error
            end;
        Error -> Error
    end.

do_connect_1(IPAddress, Port, _Options, Timeout, Socket) ->
    Address = inet:ntoa(IPAddress),
    case raw_socket:connect(Socket, Address, Port) of
        {error, einprogress} ->
            wait_for_connect_finish(Timeout, Socket);
        Error ->
            Error
    end.

wait_for_connect_finish(Timeout, Socket) -> 
    case raw_socket:enable_events(Socket, [readable, writable], self()) of
        ok    -> wait_for_connect_events(Timeout, Socket);
        Error -> flush_all_events(Socket),
                 Error
    end.

wait_for_connect_events(Timeout, Socket) ->
    receive
        {socket_event, _, Socket} ->
            flush_all_events(Socket),
            case raw_socket:getsockopt(Socket, errorcode) of
                {ok, 0, _} -> ok;
                {ok, _Code, Err} -> {error, Err};
                Error      -> Error
            end
    after Timeout ->
            flush_all_events(Socket),
            {error, etimedout}
    end.

is_member(Key, List) ->
   lists:any(fun(K) -> K == Key end, List).
 
partition_options(Options, Keys) ->
   lists:partition( 
        fun(T) when is_tuple(T) ->
                is_member(element(1, T), Keys);
           (_T) ->
                false
        end, Options).

controlling_process(Socket, Pid) ->
    raw_socket:controlling_process(Socket, Pid).

listen(Port, Options) ->
    case create_socket(Options) of
        {ok, Socket} ->
            case do_listen_1(Socket, Port, Options) of
               ok    -> {ok, Socket};
               Error -> raw_socket:close(Socket), Error
            end;
        Error ->
            Error
    end.

do_listen_1(Socket, Port, Options) ->
    Address0 = proplists:get_value(ip, Options, {0,0,0,0}),
    {ok, Address1} = to_ip_address(Address0),
    Address2 = inet:ntoa(Address1),
    Backlog  = proplists:get_value(backlog, Options, 128),
    case raw_socket:bind(Socket, Address2, Port) of
        ok    -> raw_socket:listen(Socket, Backlog);
        Error -> Error
    end.

create_socket(Options) ->
    %_Family = get_family(Options),
    case raw_socket:socket() of
        {ok, Socket} ->
            case setopts(Socket, Options) of
                ok    -> {ok, Socket};
                Error -> raw_socket:close(Socket), Error
            end;
        Error -> Error
    end.

setopts(_Socket, []) ->
    ok;
setopts(Socket, [{active, false} | T]) ->
    case raw_socket:setsockopt(Socket, {active, ?ACTIVE_FALSE}) of
        ok    -> setopts(Socket, T);
        Error -> Error
    end;
setopts(Socket, [{active, true} | T]) ->
    case raw_socket:setsockopt(Socket, {active, ?ACTIVE_TRUE}) of
        ok    -> setopts(Socket, T);
        Error -> Error
    end;
setopts(Socket, [{active, once} | T]) ->
    case raw_socket:setsockopt(Socket, {active, ?ACTIVE_ONCE}) of
        ok    -> setopts(Socket, T);
        Error -> Error
    end;
setopts(Socket, [{active, _} = H | T]) ->
    case raw_socket:setsockopt(Socket, H) of
        ok    -> setopts(Socket, T);
        Error -> Error
    end;
setopts(Socket, [{nodelay, _} = H | T]) ->
    case raw_socket:setsockopt(Socket, H) of
        ok    -> setopts(Socket, T);
        Error -> Error
    end;
setopts(Socket, [{reuseaddr, _} = H | T]) ->
    case raw_socket:setsockopt(Socket, H) of
        ok    -> setopts(Socket, T);
        Error -> Error
    end;
setopts(Socket, [{keepalive, _} = H | T]) ->
    case raw_socket:setsockopt(Socket, H) of
        ok    -> setopts(Socket, T);
        Error -> Error
    end;
setopts(Socket, [{recbuf, _} = H | T]) ->
    case raw_socket:setsockopt(Socket, H) of
        ok    -> setopts(Socket, T);
        Error -> Error
    end;
setopts(Socket, [{sndbuf, _} = H | T]) ->
    case raw_socket:setsockopt(Socket, H) of
        ok    -> setopts(Socket, T);
        Error -> Error
    end;
setopts(Socket, [{send_timeout, Timo} | T]) ->
    Timeout = case Timo of
        0 -> infinity;
        _ -> Timo
    end,
    case raw_socket:setsockopt(Socket, {send_timeout, Timeout}) of
        ok    -> setopts(Socket, T);
        Error -> Error
    end;
setopts(Socket, [{backlog, _} | T]) ->
    setopts(Socket, T);
setopts(Socket, [{ip, _} | T]) ->
    setopts(Socket, T);
setopts(_Socket, [_|_]) ->
    %setopts(Socket, T).
    {error, unknown_option}.

getopts(Socket, Opts) ->
    try internal_getopts(Socket, Opts) of
        Result -> {ok, Result}
    catch 
        Error -> {error, Error}
    end.
                      
internal_getopts(_Socket, []) ->
    [];
internal_getopts(Socket, [nodelay = H | T]) ->
    V = case raw_socket:getsockopt(Socket, H) of
            {ok, 1} -> true;
            {ok, 0} -> false;
            Error -> throw(Error)
        end,
    [{nodelay, V} | internal_getopts(Socket, T)];
internal_getopts(Socket, [reuseaddr = H | T]) ->
    V = case raw_socket:getsockopt(Socket, H) of
            {ok, 1} -> true;
            {ok, 0} -> false;
            Error -> throw(Error)
        end,
    [{reuseaddr, V} | internal_getopts(Socket, T)];
internal_getopts(Socket, [errorcode = H | T]) ->
    V = case raw_socket:getsockopt(Socket, H) of
            {ok, N} -> N;
            Error -> throw(Error)
        end,
    [{errorcode, V} | internal_getopts(Socket, T)];
internal_getopts(Socket, [ionread = H | T]) ->
    V = case raw_socket:getsockopt(Socket, H) of
            {ok, N} -> N;
            Error -> throw(Error)
        end,
    [{ionread, V} | internal_getopts(Socket, T)];
internal_getopts(Socket, [active = H | T]) ->
    V = case raw_socket:getsockopt(Socket, H) of
            {ok, N} -> 
                case N of
                    ?ACTIVE_FALSE -> false;
                    ?ACTIVE_TRUE -> true;
                    ?ACTIVE_ONCE -> once;
                    N when N>0 -> N;
                    _ -> throw(N)
                end;
            Error -> throw(Error)
        end,
    [{active, V} | internal_getopts(Socket, T)].

to_ip_address(Address) when is_atom(Address) ->
    IPAddress = atom_to_list(Address),
    to_ip_address(IPAddress);
to_ip_address(Address) when is_list(Address) ->
    case inet:getaddrs(Address, inet) of
        {ok, [H|_]} -> {ok, H};
        Error       -> Error
    end;
to_ip_address({_,_,_,_} = Address) ->
    {ok, Address}.

accept(Socket) ->
    accept(Socket, infinity).

accept(Socket, Timeout) ->
    case raw_socket:accept(Socket) of
        {ok, NewSocket} -> {ok, NewSocket};
        {error, eagain} -> 
            case wait_for_readable(Timeout, Socket) of
                ok -> accept(Socket, Timeout);
                Error -> Error
            end;
        Error -> Error
    end.

send(Socket, Bin) when is_binary(Bin)->
    send(Socket, Bin, 0, size(Bin));
send(Socket, DataList) when is_list(DataList) ->
    {IoList, Size} = raw_socket:iolist_to_binary_list(DataList),
    sendv(Socket, IoList, 0, Size).

%% send(Socket, DataList) when is_list(DataList) ->
%%     IoList1 = lists:flatten(DataList),
%%     {IoList2, Size} = convert_to_iolist(IoList1, {[], 0}),
%%     sendv(Socket, IoList2, 0, Size).

%% convert_to_iolist([], {L, Size}) ->
%%     {lists:reverse(L), Size};
%% convert_to_iolist([H|_] = IoList, {L, Size}) when is_integer(H) andalso H >=0
%%     andalso H =< 255 ->
%%     {String, Remain} = get_string(IoList, []),
%%     Bin = list_to_binary(String),
%%     convert_to_iolist(Remain, {[Bin | L], Size + size(Bin)});
%% convert_to_iolist([H|T], {L, Size}) when is_binary(H) ->
%%     convert_to_iolist(T, {[H|L], Size + size(H)});
%% % may support other types ?
%% convert_to_iolist(_, _) ->
%%     error(badarg).
    
%% get_string([], Acc) ->
%%     {lists:reverse(Acc), []};
%% get_string([H|T], Acc) when is_integer(H) andalso H >= 0 andalso H =< 255 ->
%%     NewAcc = [H|Acc],
%%     get_string(T, NewAcc);
%% get_string([H|_], _Acc) when is_integer(H) ->
%%     error(badarg);
%% get_string(IoList, Acc) ->
%%     {lists:reverse(Acc), IoList}.

send(_Socket, _Bin, End, End) ->
    ok;
send(Socket, Bin, Off, End) ->
    case raw_socket:send(Socket, Bin, Off, End-Off) of
        {ok, Off2, _} ->
            send(Socket, Bin, Off2, End);
        {error, eagain} ->
            Timeout = get_send_timeout(Socket),
            case wait_for_writable(Timeout, Socket) of
                ok    -> send(Socket, Bin, Off, End);
                Error -> Error
            end;
        Error -> Error
    end.

sendv(_Socket, _IoList, End, End) ->
    ok;
sendv(Socket, IoList, Off, End) ->
    case raw_socket:sendv(Socket, IoList, Off) of
        {ok, Off2, _} ->
            sendv(Socket, IoList, Off2, End);
        {error, eagain} ->
            Timeout = get_send_timeout(Socket),
            case wait_for_writable(Timeout, Socket) of
                ok    -> sendv(Socket, IoList, Off, End);
                Error -> Error
            end;
        Error -> Error
    end.

recv(Socket, Length) when is_integer(Length), Length >= 0 ->
    recv(Socket, Length, infinity).

recv(Socket, 0, Timeout) ->
    case raw_socket:getsockopt(Socket, ionread) of
        {ok, 0} -> 
            case wait_for_readable(Timeout, Socket) of
                ok ->
                   case raw_socket:getsockopt(Socket, ionread) of
                    {ok, 0} -> % socket closed ?
                        recv(Socket, 100, Timeout);
                    {ok, Count0} when Count0 > ?TCP_MAX_PACKET_SIZE->
                        recv(Socket, ?TCP_MAX_PACKET_SIZE, Timeout);
                    {ok, Count1} ->
                        recv(Socket, Count1, Timeout);
                    Error -> Error
                   end;
                Error -> Error
            end;
        {ok, Count0} when Count0 > ?TCP_MAX_PACKET_SIZE->
            recv(Socket, ?TCP_MAX_PACKET_SIZE, Timeout);
        {ok, Count1} ->
            recv(Socket, Count1, Timeout);
        Error ->
            Error
    end;

recv(Socket, Length, Timeout) when is_integer(Length), Length >= 0->
    case raw_socket:alloc_buf(Length, Length) of
        {ok, Buf} -> recv_buf(Socket, Buf, Timeout);
        Error     -> Error
    end.

recv_buf(Socket, Buf, Timeout) ->
    case raw_socket:recv_buf(Socket, Buf) of
        ok ->
            raw_socket:convert_buf_to_binary(Buf);
        {error, eagain} ->
            case wait_for_readable(Timeout, Socket) of
                ok    -> 
                    recv_buf(Socket, Buf, Timeout);
                Error -> Error
            end;
        Error -> Error
    end.


wait_for_readable(Timeout, Socket) ->
    case raw_socket:enable_events(Socket, [readable], self()) of
        ok    -> wait_for_event_readable(Timeout, Socket);
        Error -> Error
    end.
wait_for_event_readable(Timeout, Socket) ->
    receive
        {socket_event, readable, Socket} ->
            flush_event(Socket, readable)
    after Timeout ->
            flush_event(Socket, readable),
            {error, etimedout}
    end.


wait_for_writable(Timeout, Socket) ->
    case raw_socket:enable_events(Socket, [writable], self()) of
        ok    -> wait_for_event_writable(Timeout, Socket);
        Error -> Error
    end.
wait_for_event_writable(Timeout, Socket) ->
    receive
        {socket_event, writable, Socket} ->
            flush_event(Socket, writable);
        {tcp_error, Socket, Reason} ->
            {error, Reason};
        {tcp_closed, Socket} ->
            {error, closed}
    after Timeout ->
            flush_event(Socket, writable),
            {error, etimedout}
    end.

flush_all_events(Socket) ->
    case raw_socket:disable_events(Socket, [readable, writable]) of
        ok -> do_flush_all_events(Socket);
        Error -> Error
    end.

do_flush_all_events(Socket) ->
    receive
        {socket_event, _, Socket} -> do_flush_all_events(Socket)
    after 0 ->
        ok
    end.

flush_event(Socket, Event) ->
    case raw_socket:disable_events(Socket, [Event]) of
        ok -> do_flush_event(Socket, Event);
        Error -> Error
    end.

do_flush_event(Socket, Event) ->
    receive
        {socket_event, Event, Socket} -> do_flush_event(Socket, Event)
    after 0 ->
        ok
    end.

close(Socket) ->
    raw_socket:close(Socket).

socket_info(Socket, Opt) ->
    raw_socket:socket_info(Socket, Opt).

peername(Socket) ->
    case raw_socket:peername(Socket) of
        {ok, Bin} ->
            [F, P1,P0 | Addr] = erlang:binary_to_list(Bin),
            {IP, _} = get_ip(F, Addr),
            {ok, { IP, ?u16(P1, P0) }};
        Error -> Error
    end.

sockname(Socket) ->
    case raw_socket:sockname(Socket) of
        {ok, Bin} ->
            [F, P1,P0 | Addr] = erlang:binary_to_list(Bin),
            {IP, _} = get_ip(F, Addr),
            {ok, { IP, ?u16(P1, P0) }};
        Error -> Error
    end.

getstat(Socket) ->
    getstat(Socket, 
            lists:map(fun(T) -> element(1, T) end, stats())).

getstat(Socket, L) ->
    case encode_stats(L) of
        {ok, Bytes} ->
            try raw_socket:getstat(Socket, Bytes) of
                {ok, Data} -> {ok, decode_stats(Data)}
            catch 
                E -> {error, E}
            end;
        Error -> Error
    end.

decode_stats(L) ->
    lists:map(fun({StatIdx, StatValue})-> 
                      {element(1, lists:keyfind(StatIdx, 2, stats())), StatValue}
              end, L).

encode_stats(L) ->
    try enc_stats(lists:usort(L)) of
        Result ->
             {ok, Result}
    catch
        Error  ->
             {error, Error}
    end.

enc_stats(L) ->
    lists:map(fun(X) ->
                      case lists:keyfind(X, 1, stats()) of
                          false -> throw(einval);
                          T -> element(2, T)
                      end
              end, L).

stats() ->
    [
     {recv_cnt, ?GEN_SOCKET_STAT_RECV_CNT}, 
     {recv_max, ?GEN_SOCKET_STAT_RECV_MAX},
     {recv_avg, ?GEN_SOCKET_STAT_RECV_AVG}, 
     {recv_dvi, ?GEN_SOCKET_STAT_RECV_DVI}, 
     {recv_oct, ?GEN_SOCKET_STAT_RECV_OCT},
     {send_cnt, ?GEN_SOCKET_STAT_SEND_CNT}, 
     {send_max, ?GEN_SOCKET_STAT_SEND_MAX}, 
     {send_avg, ?GEN_SOCKET_STAT_SEND_AVG}, 
     {send_pend, ?GEN_SOCKET_STAT_SEND_PEND}, 
     {send_oct, ?GEN_SOCKET_STAT_SEND_OCT}
    ].

get_ip(?INET_AF_INET, Addr)  ->
    get_ip4(Addr).
get_ip4([A,B,C,D | T]) -> {{A,B,C,D},T}.


dup_socket(S) ->
    raw_socket:dup_socket(S).

get_send_timeout(Socket) ->
    {ok, Timeout} = raw_socket:getsockopt(Socket, send_timeout),
    case Timeout of
        0 -> infinity;
        _ -> Timeout
    end.
