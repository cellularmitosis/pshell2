default: build install

build:
	./build.sh
.PHONY: build

clean:
	rm -rf build
.PHONY: clean

install:
	./install.sh
.PHONY: install

format:
	clang-format -i pshell/*.c cc/*.h cc/*.c
.PHONY: format
