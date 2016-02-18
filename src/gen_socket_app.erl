-module(gen_socket_app).

-behaviour(application).

%% Application callbacks
-export([start/2, stop/1, start/0]).

%% ===================================================================
%% Application callbacks
%% ===================================================================

start(_StartType, _StartArgs) ->
    gen_socket_sup:start_link().

stop(_State) ->
    ok.

%% ===================
start() ->
    application:ensure_started(gen_socket).
