all: libGLX.so.0 libGLX.so.0.0.0

libGLX.so: subprime.cpp
	$(CXX) -lEGL -std=c++17 -fPIC -shared -o $@ $^

libGLX.so.0: libGLX.so
	ln -sf $^ $@
libGLX.so.0.0.0: libGLX.so
	ln -sf $^ $@
