-module(raw_socket).

-ifdef(TEST).
-include_lib("eunit/include/eunit.hrl").
-endif.

-export([init/0, socket/0, bind/3, listen/2, close/1,
    enable_events/3, disable_events/2,
    recv/1,
    send/4,
    accept/1,
    connect/3,
    setsockopt/2,
    getsockopt/2,
    shutdown/2,
    controlling_process/2,
    alloc_buf/2,
    realloc_buf/3,
    free_buf/1,
    get_buf_info/1,
    get_buf_data/3,
    recv_buf/2,
    convert_buf_to_binary/1,
    alloc_psmon/1,
    enable_psmon/1,
    disable_psmon/1,
    total_pollsets/0,
    evrun/2,
    bind_pollset/2,
    getstat/2,
    socket_info/2,
    peername/1,
    sockname/1,
    set_scheduler_id/1,
    dup_socket/1,
    sendv/3,
    ev_info/1,
    socketonline_info/0,
    set_bind_policy/1,
    iolist_to_binary_list/1
    ]).
-on_load(init/0).

-define(NOT_LOAD, erlang:nif_error(nif_not_loaded)).

init() ->
    Cmd = priv_dir() ++ "/raw_socket",
    erlang:load_nif(Cmd, 0).

socket() -> ?NOT_LOAD.

bind(_Sock, _Ip, _Port) -> ?NOT_LOAD.

listen(_Sock, _Backlog) -> ?NOT_LOAD.

close(_Sock) -> ?NOT_LOAD.

enable_events(_Sock, _Events, _Pid) -> ?NOT_LOAD.

disable_events(_Sock, _Events) -> ?NOT_LOAD.

accept(_Sock) -> ?NOT_LOAD.

connect(_Sock, _Ip, _Port) -> ?NOT_LOAD.

setsockopt(_Sock, _Opt) -> ?NOT_LOAD.

getsockopt(_Sock, _Opt) -> ?NOT_LOAD.

recv(_Sock) -> ?NOT_LOAD.

send(_Sock, _Bin, _Off, _Len) -> ?NOT_LOAD.

shutdown(_Sock, _Flags) -> ?NOT_LOAD.

controlling_process(_Sock, _NewOwer) -> ?NOT_LOAD.

alloc_buf(_Cap, _Size) -> ?NOT_LOAD.

realloc_buf(_BufObj, _Cap,  _Size) -> ?NOT_LOAD.

free_buf(_BufObj) -> ?NOT_LOAD.

get_buf_info(_BufObj) -> ?NOT_LOAD.

get_buf_data(_BufObj, _Off, _Len) -> ?NOT_LOAD.

recv_buf(_Sock, _BufObj) -> ?NOT_LOAD.

convert_buf_to_binary(_BufObj) -> ?NOT_LOAD.

alloc_psmon(_PsId) -> ?NOT_LOAD.

enable_psmon(_PsRes) -> ?NOT_LOAD.

disable_psmon(_PsRes) -> ?NOT_LOAD.

total_pollsets() -> ?NOT_LOAD.

evrun(_PsId, _MaxInvoking) -> ?NOT_LOAD.

bind_pollset(_Sock, _Pollset) -> ?NOT_LOAD.

getstat(_Sock, _Bytes) -> ?NOT_LOAD.

socket_info(_Sock, _Opt) -> ?NOT_LOAD.

peername(_Sockete) -> ?NOT_LOAD.

sockname(_Sockete) -> ?NOT_LOAD.

set_scheduler_id(_SchedId) -> ?NOT_LOAD.

dup_socket(_Sock) -> ?NOT_LOAD.

sendv(_Sock, _IoList, _Off) -> ?NOT_LOAD.

ev_info(_Sock) -> ?NOT_LOAD.

socketonline_info() -> ?NOT_LOAD.

set_bind_policy(_Policy) -> ?NOT_LOAD.

iolist_to_binary_list(_IOList) -> ?NOT_LOAD.

priv_dir() ->
    case code:priv_dir(acs) of
        {error, bad_name} ->
            case code:which(?MODULE) of
                File when not is_list(File) -> "../priv";
                File -> filename:join([filename:dirname(File),"../priv"])
            end;
        F -> F
    end.
