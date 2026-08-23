CC = gcc
CFLAGS = -Wall -g -Iinclude -m32
all: bin/test_implicit bin/test_explicit
#for implicit 
#linking step
bin/test_implicit: build/memlib.o build/mm_implicit.o build/main_implicit.o
	mkdir -p bin
	$(CC) $(CFLAGS) $^ -o $@

    
build/mm_implicit.o: src/mm_implicit.c
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/main_implicit.o: src/main_implicit.c
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@



# Explicit Allocator
bin/test_explicit: build/memlib.o build/mm_explicit.o build/main_explicit.o
	mkdir -p bin
	$(CC) $(CFLAGS) $^ -o $@
 
build/mm_explicit.o: src/mm_explicit.c
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
 
build/main_explicit.o: src/main_explicit.c
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@




#Shared memory instantiation
build/memlib.o: lib/memlib.c
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# Garbage collector (links against the EXPLICIT allocator)
# Note: mm_explicit.o but NOT main_explicit.o - both main files define main().
bin/test_gc: build/memlib.o build/mm_explicit.o build/gc.o build/main_gc.o
	mkdir -p bin
	$(CC) $(CFLAGS) $^ -o $@

build/gc.o: src/gc.c
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/main_gc.o: src/main_gc.c
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

#Run targets
run-implicit: bin/test_implicit
	./bin/test_implicit
 
run-explicit: bin/test_explicit
	./bin/test_explicit
 
run-gc: bin/test_gc
	./bin/test_gc

run-all: run-implicit run-explicit
# works like a loop %.o matches all .o/.c files $< first dependency $@ first target

run-bench: bin/bench_m bin/bench_glibc
	./bin/bench_m >> bench_results.txt && ./bin/bench_glibc >> bench_results.txt
.PHONY: clean all run-implicit run-explicit run-gc run-all run-bench# tells make that clean is just a command
clean:
	rm -f build/*.o bin/test_implicit bin/test_explicit bin/test_gc bin/program bin/bench_m bin/bench_glibc bench_results.txt


#benchmarking
bin/bench_m:
	gcc src/bench_mark1_throughput.c src/mm_explicit.c lib/memlib.c -DUSE_MM -O2 -o bin/bench_m -Iinclude -m32
	gcc src/bench_mark1_throughput.c -O2 -o bin/bench_glibc -m32



