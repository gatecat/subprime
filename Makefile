all: libGLX_subprime.so.0

libGLX_subprime.so: subprime.cpp
	$(CXX) -g -pthread -std=c++17 -fPIC -shared -o $@ $^

libGLX_subprime.so.0: libGLX_subprime.so
	ln -sf $^ $@
