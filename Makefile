all: libGLX_subprime.so.0

libGLX_subprime.so: subprime.cpp
	$(CXX) -g -pthread -std=c++17 -lX11 -fPIC -shared -O3 -o $@ $^

libGLX_subprime.so.0: libGLX_subprime.so
	ln -sf $^ $@
