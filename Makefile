DIRS := tracer.simple tracer.annotated corpus.aggregator
prefix := /usr/local

all:
	for d in $(DIRS); do $(MAKE) -C $$d; done

clean:
	for d in $(DIRS); do $(MAKE) clean -C $$d; done
	$(RM) -r ./inst

install:
	for d in $(DIRS); do $(MAKE) install prefix=$(prefix) -C $$d; done
	install -m 0755 libs/* -t $(prefix)/lib -D
	ln --symbolic -T `find /lib -name libc.so.6 -path *i386*` $(prefix)/lib/libc.so

cleanlibs:
	rm -rf ./libs

libs:
	mkdir libs
	./libs.sh teodor-stoenescu simpletracer 0.0.3 libs.zip ./libs/libs.zip
	yes | unzip ./libs/libs.zip -d ./libs

deps:
	sudo apt-get install gcc-multilib libc-dev libc-dev:i386
