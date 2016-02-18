-module(gen_socket_poller).
-behaviour(gen_server).
-export([start_link/1, poller/1]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2,
      terminate/2, code_change/3]).

-define(SPINCOUNT, 10).
-define(INVOKE_MAXCNT, 0).

-record(state, {poller=undefined}).

start_link(Arg) ->
    gen_server:start_link(?MODULE, Arg, []).

poller(To) ->
    gen_server:call(To, poller).

init({Id, BindSched}) ->                                                                     
    process_flag(trap_exit, false),
    Poller = spawn_link(fun() -> poller(Id, BindSched) end),
    State = #state{poller = Poller},
    {ok, State}.

handle_call(poller, _From, State) ->
    {reply, State#state.poller, State}.
handle_cast(stop, State) ->
    {stop, normal, State}.
handle_info(_Info, State) ->
    {noreply, State}.
terminate(_Reason, _State) ->
    ok.
code_change(_OldVsn, State, _Extra) ->
    {ok, State}.

poller(Id, BindSched) ->
    case BindSched of
        true -> process_flag(scheduler, Id+1);
        _    -> ok
    end,
    {ok, PsMon} = raw_socket:alloc_psmon(Id),
    poller_loop(Id, PsMon, ?SPINCOUNT).

poller_loop(Id, PsMon, 0) ->
    raw_socket:enable_psmon(PsMon),
    receive
        _Msg ->
            raw_socket:disable_psmon(PsMon),
            drain_message(),
            poller_loop(Id, PsMon, ?SPINCOUNT)
%%    after 300 ->
%%            raw_socket:disable_psmon(PsMon),
%%            drain_message(),
%%            poller_loop(Id, PsMon, 1)
    end;
poller_loop(Id, PsMon, SpinCount) ->
    {ok, Num} = raw_socket:evrun(Id, ?INVOKE_MAXCNT),
    case Num of
        0 -> erlang:yield(), poller_loop(Id, PsMon, SpinCount - 1);
        _ -> erlang:yield(), poller_loop(Id, PsMon, ?SPINCOUNT)
    end.

drain_message() ->
    receive
        _Msg -> drain_message()
    after 0 ->
        ok
    end.
