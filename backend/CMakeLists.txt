cmake_minimum_required(VERSION 3.15)

set(CMAKE_C_COMPILER "clang")
set(CMAKE_CXX_COMPILER "clang++")

project(AudioPilot LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CheckLanguage)

set(WHISPER_BACKEND "AUTO" CACHE STRING "GPU backend: AUTO, CUDA, VULKAN, METAL, CPU")

set(GGML_CUDA OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
set(GGML_METAL OFF CACHE BOOL "" FORCE)
set(GGML_HIPBLAS OFF CACHE BOOL "" FORCE)
set(WHISPER_CUDA OFF CACHE BOOL "" FORCE)

if(WHISPER_BACKEND STREQUAL "AUTO")
    if(APPLE)
        set(WHISPER_BACKEND "METAL")
    else()
        check_language(CUDA)
        if(CMAKE_CUDA_COMPILER)
            set(WHISPER_BACKEND "CUDA")
        else()
            find_package(Vulkan QUIET)
            if(Vulkan_FOUND)
                set(WHISPER_BACKEND "VULKAN")
            else()
                set(WHISPER_BACKEND "CPU")
            endif()
        endif()
    endif()
endif()

if(WHISPER_BACKEND STREQUAL "CUDA")
    enable_language(CUDA)
    set(GGML_CUDA ON CACHE BOOL "" FORCE)
    set(WHISPER_CUDA ON CACHE BOOL "" FORCE)
    message(STATUS "Whisper backend selected: CUDA")
elseif(WHISPER_BACKEND STREQUAL "VULKAN")
    find_package(Vulkan REQUIRED)
    set(GGML_VULKAN ON CACHE BOOL "" FORCE)
    message(STATUS "Whisper backend selected: Vulkan")
elseif(WHISPER_BACKEND STREQUAL "METAL")
    set(GGML_METAL ON CACHE BOOL "" FORCE)
    message(STATUS "Whisper backend selected: Metal")
else()
    message(STATUS "Whisper backend selected: CPU (Fallback)")
endif()


include(FetchContent)

set(BOOST_INCLUDE_LIBRARIES asio beast uuid system CACHE STRING "" FORCE)
set(BOOST_ENABLE_CMAKE ON CACHE BOOL "" FORCE)
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)


FetchContent_Declare(
    boost
    GIT_REPOSITORY https://github.com/boostorg/boost.git
    GIT_TAG boost-1.92.0
)

FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.24.5
)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
)

FetchContent_Declare(
    glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG        v9.0.0
)

set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.5.2
    GIT_SHALLOW    TRUE
)

set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)

set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)


FetchContent_Declare(
    whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG        v1.9.5
)

set(KISSFFT_DATATYPE "float" CACHE STRING "" FORCE)
set(KISSFFT_STATIC OFF CACHE BOOL "" FORCE)
set(KISSFFT_TEST OFF CACHE BOOL "" FORCE)
set(KISSFFT_TOOLS OFF CACHE BOOL "" FORCE)
set(KISSFFT_PKGCONFIG OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    kissfft
    GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
    GIT_TAG        131.2.0
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(boost libdatachannel spdlog glaze opus whisper kissfft)

# whisper/ggml inference at -O0 is several times slower, which makes Debug builds unusable for testing STT latency.
# keep the heavy third-party DSP/inference targets optimised in every configuration. our own code keeps the preset's flags.
foreach(optimised_target IN ITEMS whisper ggml ggml-base ggml-cpu ggml-vulkan ggml-cuda opus kissfft)
    if(TARGET ${optimised_target})
        target_compile_options(${optimised_target} PRIVATE $<$<CONFIG:Debug>:-O2>)
    endif()
endforeach()

find_package(OpenSSL REQUIRED)

# everything except main.cpp, so the server and the unit tests link the same code
add_library(audiopilot_core STATIC "src/api/Listener.cpp" "src/api/WebsocketSession.cpp"
"src/api/HttpSession.cpp" "src/api/WebsocketManager.cpp" "src/api/coordinator.cpp"
"src/api/HttpRouter.cpp"
"src/api/PeerConnection.cpp" "src/api/PeerConnectionManager.cpp" "src/ThirdParty/RtpCpp/SmallVector.cpp"
"src/api/Bridge.cpp" "src/api/BridgeManager.cpp"
"src/api/OpusTranscoder.cpp" "src/api/PTTHandler.cpp" "src/api/STTHandler.cpp" "src/api/NoiseSuppressor.cpp"
"src/api/CommandParser.cpp" "src/api/CommandGrammar.cpp"
"${whisper_SOURCE_DIR}/examples/grammar-parser.cpp")

# whisper.cpp's GBNF parser is in its examples, not the library
target_include_directories(audiopilot_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${whisper_SOURCE_DIR}/examples")

target_link_libraries(audiopilot_core PUBLIC
    Boost::asio
    Boost::beast
    Boost::uuid
    datachannel
    spdlog::spdlog
    glaze::glaze
    OpenSSL::SSL
    OpenSSL::Crypto
    Opus::opus
    whisper
    kissfft::kissfft
)

set(COMMAND_GRAMMAR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/grammars")

add_executable(backend "src/main.cpp")
target_link_libraries(backend PRIVATE audiopilot_core)
target_compile_definitions(backend PRIVATE
    WHISPER_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/models/ggml-base.en.bin"
    WHISPER_VAD_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/models/ggml-silero-v5.1.2.bin"
    COMMAND_GRAMMAR_DIR="${COMMAND_GRAMMAR_DIR}"
)

option(AUDIOPILOT_BUILD_TESTS "Build the Catch2 unit tests" ON)

if(AUDIOPILOT_BUILD_TESTS)
    FetchContent_Declare(
        catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.16.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(catch2)

    enable_testing()
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)

    add_executable(backend_tests
        "tests/CommandParserTests.cpp"
        "tests/CommandGrammarTests.cpp"
        "tests/OpusTranscoderTests.cpp"
        "tests/NoiseSuppressorTests.cpp"
        "tests/JsonUtilsTests.cpp"
        "tests/UtilsTests.cpp"
        "tests/BridgeTests.cpp"
    )
    target_link_libraries(backend_tests PRIVATE audiopilot_core Catch2::Catch2WithMain)
    target_compile_definitions(backend_tests PRIVATE COMMAND_GRAMMAR_DIR="${COMMAND_GRAMMAR_DIR}")

    catch_discover_tests(backend_tests)
endif()