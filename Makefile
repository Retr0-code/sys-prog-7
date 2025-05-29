all:
	@mkdir -p build
	@cc -ggdb -O0 -o build/pat_search src/main.c
