all: libGLX_subprime.so.0

libGLX_subprime.so: subprime.cpp
	$(CXX) -std=c++17 -Wall -fPIC -shared -o $@ $^

libGLX_subprime.so.0: libGLX_subprime.so
	ln -s $^ $@
