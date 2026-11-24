# Backend

C++ (standard set by `CMAKE_CXX_STANDARD`), clang/clang++ only. All dependencies come from `FetchContent` in `CMakeLists.txt` (Boost asio/beast/uuid, libdatachannel, spdlog, glaze, opus, whisper.cpp, kissfft). OpenSSL must be installed on the system.

## Build and run

Run from `backend/`:

```sh
cmake --preset clang++-debug              # first configure fetches all deps and is slow
cmake --build out/build/clang++-debug
./out/build/clang++-debug/backend         # address and port: src/api/Listener.cpp
```

- Other presets: `clang++-debug-UB-A-L` (ASan/UBSan/LSan), `clang++-debug-T` (TSan), `clang++-debug-M` (MSan), `clang++-cpu` / `clang++-vulkan` / `clang++-cuda` (force a Whisper backend), `clang++-release`.
- `WHISPER_BACKEND=AUTO` picks Metal, then CUDA, then Vulkan, then CPU. whisper/ggml/opus/kissfft are built optimised even in Debug, so don't remove that block.
- Whisper models are gitignored and must be put in `backend/models/` by hand. The file names the build expects are in `WHISPER_MODEL_PATH` and `WHISPER_VAD_MODEL_PATH` in `CMakeLists.txt`.
- **New `.cpp` files must be added by hand** in `CMakeLists.txt`, since there's no glob. Source files go in `add_library(audiopilot_core ...)`, and test files go in `add_executable(backend_tests ...)`. `backend` is only `main.cpp` linked against `audiopilot_core`.

## Lint and format

- `clang-tidy -p out/build/clang++-debug src/api/<File>.cpp`. Fix warnings in code you touch. Don't add `NOLINT` without asking.
- `clang-format -i <files>`. The style is in `.clang-format`.

## Testing

```sh
cmake --build out/build/clang++-debug --target backend_tests
ctest --preset clang++-debug                       # or ./out/build/clang++-debug/backend_tests
ctest --preset clang++-debug-UB-A-L                # same tests under ASan/UBSan
```

- Every new feature gets Catch2 unit tests, written in the same change without being asked.
- Tests live in `tests/`, one `<Class>Tests.cpp` per class, with the class name as the tag (`[CommandParser]`).
- Test the public behaviour: the normal case, edge cases, and error paths (the `std::unexpected` results).
- Unit tests must not need Whisper models, a GPU, or the network. Split pure logic out so it can be tested on its own. `CommandParser` is the model to follow.
- Build test inputs from the class's constants (`CommandParser::kMaxAgc`, `OpusTranscoder::kSampleRate`), not copied numbers, so retuning doesn't break tests.
- Generated test signals use fixed seeds so runs are repeatable.
- `tests/.clang-tidy` turns off checks that Catch2's macros trip. Add to it rather than using `NOLINT`.

## Naming (enforced by `.clang-tidy`)

| Kind | Style | Example |
|---|---|---|
| Classes, structs, enums, namespaces, type aliases | `CamelCase` | `PTTHandler`, `CAM::API` |
| Functions, variables, parameters | `lower_case` | `process_packet`, `session_id` |
| Members | `lower_case_` | `stt_handler_` |
| Constants, constexpr, enum values | `k` + CamelCase | `kMaxTokens` |

The exception is `src/api/types.hpp`: its field names are the JSON wire format, so that file is wrapped in `NOLINTBEGIN/END`.

## Code layout and patterns

- All code is in `namespace CAM::API` (`CAM::Utils` for `src/utils/`). Each class gets a `.hpp` + `.cpp` pair in `src/api/`.
- `main.cpp` builds `STTHandler`, the managers and `Coordinator`, then runs one `io_context`.
- `Coordinator` owns the managers and `HttpRouter` and creates all the callbacks. Wire new components in there, not in `main` or the sessions. New HTTP endpoints go in `HttpRouter::route`.
- Errors are returned as `std::expected<T, std::string>`, not thrown. Classes that can fail to build use a private constructor plus a `static create(...)` that returns `std::expected`. Use `spdlog` for logging.
- Resource-owning classes delete copy and move.
- **Threading:** libdatachannel callbacks and the STT worker run on their own threads. `boost::asio::post` to the io executor before touching `WebsocketManager`, `PeerConnectionManager` or `BridgeManager`.
- Tunable numbers are `static constexpr` members with a comment saying why the value was chosen.

## Voice commands

- `CommandParser` turns a Whisper transcript into a canonical command such as `reset`, `agc <level>`, `mixing <object>` or `pitch <offset>`. The exact forms and ranges are documented in `CommandParser.hpp`.
- `grammars/commands.gbnf` (strict) and `commands-flexible.gbnf` limit what Whisper can output. `main.cpp` picks which one is used.
- **When the vocabulary changes, keep these in sync:**
  - New mixing object: add it to `CommandParser::kMixingObjects` and both `.gbnf` files. The Whisper prompt picks it up from `kMixingObjects` automatically.
  - New command or changed limit: update the parser, both `.gbnf` files, and the hardcoded example lines in `CommandParser::whisper_prompt()`.

## Do not edit

- `src/ThirdParty/RtpCpp/`: vendored code.
