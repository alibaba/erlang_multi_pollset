
-module(gen_socket_sup).

-behaviour(supervisor).

%% API
-export([start_link/0]).

%% Supervisor callbacks
-export([init/1]).

%% Helper macro for declaring children of supervisor
-define(CHILD(I, M, Type, Arg), {I, {M, start_link, [Arg]}, permanent, 5000, Type, [M]}).

%% ===================================================================
%% API functions
%% ===================================================================

start_link() ->
    supervisor:start_link({local, ?MODULE}, ?MODULE, []).

%% ===================================================================
%% Supervisor callbacks
%% ===================================================================

init([]) ->
    set_scheduler_ids(),
    BindPoller = application:get_env(gen_socket, bind_scheduler, true),
    BindSocketPolicy = application:get_env(gen_socket, bind_socket_policy, 1),
    io:format("BindPoller=~p, BindSocketPolicy=~p~n", [BindPoller, BindSocketPolicy]),
    N = raw_socket:total_pollsets(),
    raw_socket:set_bind_policy(BindSocketPolicy),
    Children = [?CHILD(child_name(Id), gen_socket_poller, worker, {Id, BindPoller})
        || Id <- lists:seq(0, N-1)],
    {ok, { {one_for_one, 5, 10}, Children} }.

child_name(Id) ->
    list_to_atom("gen_socket_poller" ++ integer_to_list(Id)).

set_scheduler_ids() ->
    OldFlag = process_flag(trap_exit, true),
    Pid = spawn_link(fun set_sched_id/0),
    receive
       {'EXIT', Pid, Reason} -> Reason = normal
    end,
    process_flag(trap_exit, OldFlag).
 
set_sched_id() ->
    Scheds = erlang:system_info(schedulers),
    set_sched_id(Scheds).
set_sched_id(0) ->
    ok;
set_sched_id(N) ->
    process_flag(scheduler, N),
    erlang:yield(),
    ok = raw_socket:set_scheduler_id(N),
    set_sched_id(N-1).
