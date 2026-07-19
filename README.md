# ring-buffer

A bounded multi-producer / multi-consumer (MPMC) ring buffer in C++20 — header-only,
zero dependencies. Non-blocking `try_push` / `try_pop` built on the Vyukov-style
sequence-number protocol with CAS slot claiming, plus batch operations that amortise
atomic overhead across up to N elements per call. Lockless / non-blocking by design,
though not formally lock-free — that distinction, and every other design decision, is
covered in [DESIGN.md](DESIGN.md).

**Status:** early development. The SPSC baseline is implemented and tested
(including under ThreadSanitizer); the MPMC protocol, stress-test suite, and
benchmark harness are landing incrementally.

## Build & test

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires CMake ≥ 3.20 and a C++20 compiler (clang and gcc tested in CI).

## License

[MIT](LICENSE)
