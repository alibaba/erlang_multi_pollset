REBAR=./rebar
all:clean compile

compile:
	@$(REBAR) compile

clean:
	@$(REBAR) clean
	@rm -f priv/*.so c__src/*.o .eunit


eunit: clean_eunit
	@mkdir -p .eunit
	@mkdir -p .eunit/log
	@$(REBAR) skip_deps=true eunit

clean_eunit:
	@rm -rf .eunit
