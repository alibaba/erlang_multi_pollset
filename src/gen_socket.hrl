%%----------------------------------------------------------------------------
%% Interface constants.
%%
%% This section must be "identical" to the corresponding in raw_socket.cpp
%%

-define(u16(X1,X0),
        (((X1) bsl 8) bor (X0))).

%% family codes to open
-define(INET_AF_INET,         1).
-define(INET_AF_INET6,        2).
-define(INET_AF_ANY,          3). % Fake for ANY in any address family
-define(INET_AF_LOOPBACK,     4). % Fake for LOOPBACK in any address family

-define(ACTIVE_FALSE, 0).
-define(ACTIVE_TRUE, -1).
-define(ACTIVE_ONCE, -2).

-define(GEN_SOCKET_STAT_RECV_CNT,  0).
-define(GEN_SOCKET_STAT_RECV_MAX,  1).
-define(GEN_SOCKET_STAT_RECV_AVG,  2).
-define(GEN_SOCKET_STAT_RECV_DVI,  3).
-define(GEN_SOCKET_STAT_RECV_OCT,  4).
-define(GEN_SOCKET_STAT_SEND_CNT,  5).
-define(GEN_SOCKET_STAT_SEND_MAX,  6).
-define(GEN_SOCKET_STAT_SEND_AVG,  7).
-define(GEN_SOCKET_STAT_SEND_PEND, 8).
-define(GEN_SOCKET_STAT_SEND_OCT,  9).

-define(TCP_MAX_PACKET_SIZE, 16#4000000).
