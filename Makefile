# Тонкая обёртка над CMake (прежний macOS-only Makefile больше не нужен).
.PHONY: build test demo-data
build:
	cmake -S . -B build && cmake --build build -j
test: build
	ctest --test-dir build --output-on-failure
demo-data:
	python3 scripts/make_demo_data.py demo_inbox
clean:
	rm -rf build
