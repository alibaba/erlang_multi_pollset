#!/usr/bin/env sh

if [ ! -d priv ];
then
    mkdir priv
fi

cd c__src && make all && cd -
