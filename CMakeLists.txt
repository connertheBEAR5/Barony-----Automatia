cmake_minimum_required(VERSION 3.10)
project(barony)

include(CTest)

option(BARONY_ENABLE_GRAPHICAL_RUNTIME_CTEST
	"Enable the full graphical Barony runtime characterization test under CTest" OFF)

if (WIN32)
	# Keep Windows' min/max macros out of the Automatia and RapidJSON code.
	add_definitions(-DNOMINMAX)
endif()

if(BUILD_TESTING)
	add_executable(
		custom_dialogue_document_tests
		tests/custom_dialogue_document_tests.cpp
		src/custom_dialogue_document.cpp
	)
	target_include_directories(
		custom_dialogue_document_tests
		PRIVATE
		"${PROJECT_SOURCE_DIR}/src"
	)
	target_compile_definitions(
		custom_dialogue_document_tests
		PRIVATE
		BARONY_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
	)
	target_compile_features(custom_dialogue_document_tests PRIVATE cxx_std_17)
	add_test(NAME custom_dialogue_document COMMAND custom_dialogue_document_tests)

    add_executable(
        automatia_foundation_tests
        tests/automatia_foundation_tests.cpp
        src/automatia_save.cpp
        src/automatia_identity.cpp
        src/party_manager.cpp
        src/party_protocol.cpp
    )
    target_include_directories(
        automatia_foundation_tests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_features(automatia_foundation_tests PRIVATE cxx_std_17)
    add_test(NAME automatia_foundation COMMAND automatia_foundation_tests)

    add_executable(
        sam_21_integration_contract_tests
        tests/sam_21_integration_contract_tests.cpp
    )
    target_include_directories(
        sam_21_integration_contract_tests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_definitions(
        sam_21_integration_contract_tests
        PRIVATE
        BARONY_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_compile_features(sam_21_integration_contract_tests PRIVATE cxx_std_17)
    add_test(NAME sam_21_integration_contract COMMAND sam_21_integration_contract_tests)

	add_executable(
		text_source_script_tests
		tests/text_source_script_tests.cpp
		src/text_source_script.cpp
		src/text_source_script_catalog.cpp
		src/text_source_script_registry.cpp
	)
	target_include_directories(
		text_source_script_tests
		PRIVATE
		"${PROJECT_SOURCE_DIR}/src"
	)
	target_compile_definitions(
		text_source_script_tests
		PRIVATE
		BARONY_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
	)
	target_compile_features(text_source_script_tests PRIVATE cxx_std_17)
	add_test(NAME text_source_script COMMAND text_source_script_tests)

	add_executable(
		minimimic_authored_characterization_tests
		tests/minimimic_authored_characterization_tests.cpp
	)
	target_compile_definitions(
		minimimic_authored_characterization_tests
		PRIVATE
		BARONY_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
	)
	target_compile_features(
		minimimic_authored_characterization_tests PRIVATE cxx_std_17)
	add_test(
		NAME minimimic_authored_characterization
		COMMAND minimimic_authored_characterization_tests)

	add_executable(
		room_group_tests
		tests/room_group_tests.cpp
		src/room_group.cpp
	)
	target_include_directories(
		room_group_tests
		PRIVATE
		"${PROJECT_SOURCE_DIR}/src"
	)
	target_compile_definitions(
		room_group_tests
		PRIVATE
		BARONY_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
	)
	target_compile_features(room_group_tests PRIVATE cxx_std_17)
	add_test(NAME room_group COMMAND room_group_tests)

	add_executable(
		procedural_room_generation_tests
		tests/procedural_room_generation_tests.cpp
		src/procedural_room.cpp
	)
	target_include_directories(
		procedural_room_generation_tests
		PRIVATE
		"${PROJECT_SOURCE_DIR}/src"
	)
	target_compile_definitions(
		procedural_room_generation_tests
		PRIVATE
		BARONY_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
	)
	target_compile_features(procedural_room_generation_tests PRIVATE cxx_std_17)
	add_test(NAME procedural_room_generation COMMAND procedural_room_generation_tests)

	add_executable(
		vertical_navigation_tests
		tests/vertical_navigation_tests.cpp
		src/vertical_navigation.cpp
	)
	target_include_directories(
		vertical_navigation_tests
		PRIVATE
		"${PROJECT_SOURCE_DIR}/src"
	)
	target_compile_features(vertical_navigation_tests PRIVATE cxx_std_17)
	add_test(NAME vertical_navigation COMMAND vertical_navigation_tests)

	add_executable(
		cross_floor_path_tests
		tests/cross_floor_path_tests.cpp
		src/cross_floor_path.cpp
		src/vertical_navigation.cpp
	)
	target_include_directories(
		cross_floor_path_tests
		PRIVATE
		"${PROJECT_SOURCE_DIR}/src"
	)
	target_compile_features(cross_floor_path_tests PRIVATE cxx_std_17)
	add_test(NAME cross_floor_path COMMAND cross_floor_path_tests)

	add_executable(
		sam_room_generation_tests
		tests/sam_room_generation_tests.cpp
		src/sam/framework/sam_rooms.cpp
		src/sam/framework/sam_logger.cpp
	)
	target_include_directories(
		sam_room_generation_tests
		PRIVATE
		"${PROJECT_SOURCE_DIR}/src"
		"${PROJECT_SOURCE_DIR}/src/sam"
		"${PROJECT_SOURCE_DIR}/src/sam/framework"
	)
	target_compile_features(sam_room_generation_tests PRIVATE cxx_std_17)
	add_test(NAME sam_room_generation COMMAND sam_room_generation_tests)

	add_executable(
		sam_framework_refresh_tests
		tests/sam_framework_refresh_tests.cpp
		src/sam/framework/sam_workshop.cpp
		src/sam/framework/sam_logger.cpp
		src/sam/framework/sam_errors.cpp
		src/sam/sam_item_registry_foundation.cpp
		src/sam/sam_class_registry_foundation.cpp
	)
	target_include_directories(
		sam_framework_refresh_tests
		PRIVATE
		"${PROJECT_SOURCE_DIR}/src"
		"${PROJECT_SOURCE_DIR}/src/sam"
		"${PROJECT_SOURCE_DIR}/src/sam/framework"
	)
	target_compile_features(sam_framework_refresh_tests PRIVATE cxx_std_17)
	add_test(NAME sam_framework_refresh COMMAND sam_framework_refresh_tests)

    add_executable(
        party_backend_tests
        tests/party_backend_tests.cpp
        src/automatia_save.cpp
        src/automatia_identity.cpp
        src/party_manager.cpp
        src/party_protocol.cpp
    )
    target_include_directories(
        party_backend_tests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_features(party_backend_tests PRIVATE cxx_std_17)
    add_test(NAME party_backend COMMAND party_backend_tests)

    add_executable(
        quest_ownership_tests
        tests/quest_ownership_tests.cpp
        src/automatia_save.cpp
        src/automatia_identity.cpp
        src/party_manager.cpp
        src/quest_ownership.cpp
    )
    target_include_directories(
        quest_ownership_tests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_definitions(
        quest_ownership_tests
        PRIVATE
        BARONY_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_compile_features(quest_ownership_tests PRIVATE cxx_std_17)
    add_test(NAME quest_ownership COMMAND quest_ownership_tests)

    add_executable(
        social_party_ui_model_tests
        tests/social_party_ui_model_tests.cpp
        src/automatia_identity.cpp
        src/party_manager.cpp
        src/party_protocol.cpp
        src/social_party_ui_model.cpp
    )
    target_include_directories(
        social_party_ui_model_tests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_features(social_party_ui_model_tests PRIVATE cxx_std_17)
    add_test(NAME social_party_ui_model COMMAND social_party_ui_model_tests)

    add_executable(
        party_chat_tests
        tests/party_chat_tests.cpp
        src/automatia_identity.cpp
        src/party_manager.cpp
        src/party_chat.cpp
    )
    target_include_directories(
        party_chat_tests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_features(party_chat_tests PRIVATE cxx_std_17)
    add_test(NAME party_chat COMMAND party_chat_tests)

	add_executable(
		illusion_magic_tests
		tests/illusion_magic_tests.cpp
		src/magic/illusion_magic_model.cpp
	)
	target_include_directories(
		illusion_magic_tests
		PRIVATE
		"${PROJECT_SOURCE_DIR}/src"
	)
	target_compile_definitions(
		illusion_magic_tests
		PRIVATE
		BARONY_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
	)
	target_compile_features(illusion_magic_tests PRIVATE cxx_std_17)
	add_test(NAME illusion_magic COMMAND illusion_magic_tests)

	add_executable(
		skill_books_tests
		tests/skill_books_tests.cpp
	)
	target_include_directories(
		skill_books_tests
		PRIVATE
		"${PROJECT_SOURCE_DIR}/src"
	)
	target_compile_definitions(
		skill_books_tests
		PRIVATE
		BARONY_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
	)
	target_compile_features(skill_books_tests PRIVATE cxx_std_17)
	add_test(NAME skill_books COMMAND skill_books_tests)

	add_executable(
		player_movement_math_tests
		tests/player_movement_math_tests.cpp
	)
	target_include_directories(
		player_movement_math_tests
		PRIVATE
		"${PROJECT_SOURCE_DIR}/src"
	)
	target_compile_definitions(
		player_movement_math_tests
		PRIVATE
		BARONY_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
	)
	target_compile_features(player_movement_math_tests PRIVATE cxx_std_17)
	add_test(NAME player_movement_math COMMAND player_movement_math_tests)

    add_executable(
        playable_z_characterization_tests
        tests/playable_z_characterization_tests.cpp
        src/automatia_save.cpp
        src/automatia_identity.cpp
        src/party_manager.cpp
        src/party_protocol.cpp
    )
    target_include_directories(
        playable_z_characterization_tests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_definitions(
        playable_z_characterization_tests
        PRIVATE
        BARONY_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        BARONY_BINARY_DIR="${PROJECT_BINARY_DIR}"
    )
    target_compile_features(
        playable_z_characterization_tests PRIVATE cxx_std_17)
    add_test(
        NAME playable_z_characterization
        COMMAND playable_z_characterization_tests)

    if(UNIX)
        add_executable(
            late_join_live_probe
            tests/late_join_live_probe.cpp
            src/automatia_identity.cpp
            src/party_manager.cpp
            src/party_protocol.cpp
        )
        target_include_directories(
            late_join_live_probe
            PRIVATE
            "${PROJECT_SOURCE_DIR}/src"
        )
        target_compile_features(late_join_live_probe PRIVATE cxx_std_17)

        add_executable(
            party_cross_map_live_probe
            tests/party_cross_map_live_probe.cpp
            src/automatia_identity.cpp
            src/party_manager.cpp
            src/party_protocol.cpp
            src/party_chat.cpp
            src/social_party_ui_model.cpp
        )
        target_include_directories(
            party_cross_map_live_probe
            PRIVATE
            "${PROJECT_SOURCE_DIR}/src"
        )
        target_compile_features(
            party_cross_map_live_probe PRIVATE cxx_std_17)
    endif()
endif()

if (NOT CMAKE_BUILD_TYPE)
	set (CMAKE_BUILD_TYPE Release)
endif(NOT CMAKE_BUILD_TYPE)

option(
	BARONY_SUPER_MULTIPLAYER
	"Build Barony with support for up to 15 players"
	OFF
)

option(
	SAM_FRAMEWORK_ENABLED
	"Build the Automatia S.A.M framework integration"
	ON
)

if (BARONY_SUPER_MULTIPLAYER)
	add_definitions(-DBARONY_SUPER_MULTIPLAYER)
	message(STATUS "Building with 15-player multiplayer support")
else()
	message(STATUS "Building with standard 4-player multiplayer support")
endif()

if (DEFINED ENV{STEAMWORKS_ENABLED})
	set (STEAMWORKS_ENABLED $ENV{STEAMWORKS_ENABLED})
endif()

if (NOT DEFINED STEAMWORKS_ENABLED)
	set (STEAMWORKS_ENABLED 0)
	option(STEAMWORKS "Enable Steamworks" OFF)
	if (STEAMWORKS)
		set (STEAMWORKS_ENABLED 1)
	endif()
endif()

if (DEFINED ENV{OPTIMIZATION_LEVEL})
	set (OPTIMIZATION_LEVEL $ENV{OPTIMIZATION_LEVEL})
else()
	set (OPTIMIZATION_LEVEL "-O3")
endif()

if (DEFINED ENV{EOS_ENABLED})
	set (EOS_ENABLED $ENV{EOS_ENABLED})
endif()

if (NOT DEFINED EOS_ENABLED)
	set (EOS_ENABLED 0)
	option(EOS "Enable Epic Online Services" OFF)
	if (EOS)
		set (EOS_ENABLED 1)
	endif()
	set(BUILD_ENV_PR "" CACHE STRING "EOS product id")
	set(BUILD_ENV_SA "" CACHE STRING "EOS sandbox")
	set(BUILD_ENV_DE "" CACHE STRING "EOS deployment")
	set(BUILD_ENV_CC "" CACHE STRING "EOS cc")
	set(BUILD_ENV_CS "" CACHE STRING "EOS cs")
	set(BUILD_ENV_GSE "" CACHE STRING "EOS gse")
endif()

if (DEFINED ENV{PLAYFAB_ENABLED})
    set (PLAYFAB_ENABLED $ENV{PLAYFAB_ENABLED})
endif()

if (NOT DEFINED PLAYFAB_ENABLED)
    set (PLAYFAB_ENABLED 0)
    option(PLAYFAB "Enable PlayFab support" OFF)
    if (PLAYFAB)
        set (PLAYFAB_ENABLED 1)
    endif()
    set(BUILD_ENV_PFTID "" CACHE STRING "PlayFab TID")
    set(BUILD_ENV_PFHID "" CACHE STRING "PlayFab HID")
endif()

if (DEFINED ENV{THEORAPLAYER_ENABLED})
    set (THEORAPLAYER_ENABLED $ENV{THEORAPLAYER_ENABLED})
endif()

if (NOT DEFINED THEORAPLAYER_ENABLED)
    set (THEORAPLAYER_ENABLED 0)
    option(THEORAPLAYER "Enable TheoraPlayer for video playback" OFF)
    if (THEORAPLAYER)
        set (THEORAPLAYER_ENABLED 1)
    endif()
endif()

if (DEFINED ENV{CURL_ENABLED})
    set (CURL_ENABLED $ENV{CURL_ENABLED})
endif()

if (NOT DEFINED CURL_ENABLED)
    set (CURL_ENABLED 0)
    option(CURL "Enable CURL support" OFF)
    if (CURL)
        set (CURL_ENABLED 1)
    endif()
endif()

if (DEFINED ENV{OPUS_ENABLED})
    set (OPUS_ENABLED $ENV{OPUS_ENABLED})
endif()

if (NOT DEFINED OPUS_ENABLED)
    set (OPUS_ENABLED 0)
    option(OPUS "Enable OPUS support" OFF)
    if (OPUS)
        set (OPUS_ENABLED 1)
    endif()
endif()

if (PLAYFAB_ENABLED AND NOT CURL_ENABLED)
	message(FATAL_ERROR "CURL must be enabled if PlayFab is enabled.")
endif()

# EOS & PlayFab Tokens

if (DEFINED ENV{BUILD_ENV_PR} AND NOT DEFINED BUILD_ENV_PR) # TODO: We should make all environment variable checks in this file not overwrite whatever was explicitly passed in on the command line...
	set (BUILD_ENV_PR $ENV{BUILD_ENV_PR})
endif()
if (EOS_ENABLED AND "${BUILD_ENV_PR}" STREQUAL "")
	message(FATAL_ERROR "BUILD_ENV_PR must be specified if EOS is enabled.")
endif()

if (DEFINED ENV{BUILD_ENV_SA} AND NOT DEFINED BUILD_ENV_SA)
	set (BUILD_ENV_SA $ENV{BUILD_ENV_SA})
endif()
if (EOS_ENABLED AND "${BUILD_ENV_SA}" STREQUAL "")
	message(FATAL_ERROR "BUILD_ENV_SA must be specified if EOS is enabled.")
endif()

if (DEFINED ENV{BUILD_ENV_DE} AND NOT DEFINED BUILD_ENV_DE)
	set (BUILD_ENV_DE $ENV{BUILD_ENV_DE})
endif()
if (EOS_ENABLED AND "${BUILD_ENV_DE}" STREQUAL "")
	message(FATAL_ERROR "BUILD_ENV_DE must be specified if EOS is enabled.")
endif()

if (DEFINED ENV{BUILD_ENV_CC} AND NOT DEFINED BUILD_ENV_CC)
	set (BUILD_ENV_CC $ENV{BUILD_ENV_CC})
endif()
if (EOS_ENABLED AND "${BUILD_ENV_CC}" STREQUAL "")
	message(FATAL_ERROR "BUILD_ENV_CC must be specified if EOS is enabled.")
endif()

if (DEFINED ENV{BUILD_ENV_CS} AND NOT DEFINED BUILD_ENV_CS)
	set (BUILD_ENV_CS $ENV{BUILD_ENV_CS})
endif()
if (EOS_ENABLED AND "${BUILD_ENV_CS}" STREQUAL "")
	message(FATAL_ERROR "BUILD_ENV_CS must be specified if EOS is enabled.")
endif()

if (DEFINED ENV{BUILD_ENV_GSE} AND NOT DEFINED BUILD_ENV_GSE)
	set (BUILD_ENV_GSE $ENV{BUILD_ENV_GSE})
endif()
if (EOS_ENABLED AND "${BUILD_ENV_GSE}" STREQUAL "")
	message(FATAL_ERROR "BUILD_ENV_GSE must be specified if EOS is enabled.")
endif()

if (DEFINED ENV{BUILD_ENV_PFTID} AND NOT DEFINED BUILD_ENV_PFTID)
	set (BUILD_ENV_PFTID $ENV{BUILD_ENV_PFTID})
endif()
if (PLAYFAB_ENABLED AND "${BUILD_ENV_PFTID}" STREQUAL "")
	message(FATAL_ERROR "BUILD_ENV_PFTID must be specified if PlayFab is enabled.")
endif()

if (DEFINED ENV{BUILD_ENV_PFHID} AND NOT DEFINED BUILD_ENV_PFHID)
	set (BUILD_ENV_PFHID $ENV{BUILD_ENV_PFHID})
endif()
if (PLAYFAB_ENABLED AND "${BUILD_ENV_PFHID}" STREQUAL "")
	message(FATAL_ERROR "BUILD_ENV_PFHID must be specified if PlayFab is enabled.")
endif()

if (DEFINED ENV{EDITOR_ENABLED})
	set (EDITOR_ENABLED $ENV{EDITOR_ENABLED})
else()
	set (EDITOR_ENABLED 1)
endif()

if (DEFINED ENV{GAME_ENABLED})
	set (GAME_ENABLED $ENV{GAME_ENABLED})
else()
	set (GAME_ENABLED 1)
endif()

# A CMake value is preferred so Windows builds can be configured from a preset
# or the CMake GUI. Keep the environment variable as a backwards-compatible
# first-configure convenience for existing Visual Studio/MinGW setups.
set(BARONY_WIN32_LIBRARIES "${BARONY_WIN32_LIBRARIES}" CACHE PATH
	"Additional dependency prefix (primarily for Windows builds)")
if (DEFINED ENV{BARONY_WIN32_LIBRARIES}
	AND "${BARONY_WIN32_LIBRARIES}" STREQUAL "")
	set(BARONY_WIN32_LIBRARIES "$ENV{BARONY_WIN32_LIBRARIES}" CACHE PATH
		"Additional dependency prefix (primarily for Windows builds)" FORCE)
endif()

# MSYS2 exports the active native-toolchain prefix. Use it only when the
# caller did not supply an explicit CMake or BARONY_WIN32_LIBRARIES value.
if (WIN32 AND MINGW
	AND "${BARONY_WIN32_LIBRARIES}" STREQUAL ""
	AND DEFINED ENV{MINGW_PREFIX})
	set(BARONY_WIN32_LIBRARIES "$ENV{MINGW_PREFIX}" CACHE PATH
		"Additional dependency prefix (primarily for Windows builds)" FORCE)
endif()

if (NOT "${BARONY_WIN32_LIBRARIES}" STREQUAL "")
	list(APPEND CMAKE_PREFIX_PATH "${BARONY_WIN32_LIBRARIES}")
endif()

# CPack needs the DLL directory in addition to import-library discovery. For
# an MSYS2 build this is normally <MINGW_PREFIX>/bin; other toolchains may set
# one or more directories explicitly as a CMake list.
set(BARONY_WINDOWS_RUNTIME_DLL_DIRS "${BARONY_WINDOWS_RUNTIME_DLL_DIRS}" CACHE STRING
	"Semicolon-separated Windows runtime DLL directories used by the installer")
if (WIN32
	AND "${BARONY_WINDOWS_RUNTIME_DLL_DIRS}" STREQUAL ""
	AND NOT "${BARONY_WIN32_LIBRARIES}" STREQUAL ""
	AND IS_DIRECTORY "${BARONY_WIN32_LIBRARIES}/bin")
	set(BARONY_WINDOWS_RUNTIME_DLL_DIRS "${BARONY_WIN32_LIBRARIES}/bin" CACHE STRING
		"Semicolon-separated Windows runtime DLL directories used by the installer" FORCE)
endif()

if (DEFINED ENV{BARONY_DATADIR} AND NOT DEFINED BARONY_DATADIR)
	# Used by Visual Studio to set the working directory when debugging Barony.
	set(BARONY_DATADIR "$ENV{BARONY_DATADIR}" CACHE PATH
		"Directory containing Barony runtime data for Windows debugging")
endif()

# Select exactly one audio implementation. AUTO preserves the historical
# FMOD_ENABLED/OPENAL_ENABLED switches so existing build scripts keep working;
# new builds should use BARONY_AUDIO_BACKEND directly.
set(BARONY_AUDIO_BACKEND "AUTO" CACHE STRING
	"Audio backend to build (AUTO, FMOD, OPENAL, or NONE)")
set_property(CACHE BARONY_AUDIO_BACKEND PROPERTY STRINGS AUTO FMOD OPENAL NONE)
option(FMOD_ENABLED "Legacy switch: prefer FMOD when BARONY_AUDIO_BACKEND=AUTO" ON)
option(OPENAL_ENABLED "Legacy switch: prefer OpenAL when BARONY_AUDIO_BACKEND=AUTO" OFF)
set(TREMOR 0) #Acts as an alias to TREMOR_FOUND
option(TREMOR_ENABLED "Use Tremor instead of libvorbis for OpenAL" OFF)

string(TOUPPER "${BARONY_AUDIO_BACKEND}" BARONY_AUDIO_BACKEND_NORMALIZED)
if(NOT BARONY_AUDIO_BACKEND_NORMALIZED MATCHES "^(AUTO|FMOD|OPENAL|NONE)$")
	message(FATAL_ERROR
		"Invalid BARONY_AUDIO_BACKEND='${BARONY_AUDIO_BACKEND}'. "
		"Choose AUTO, FMOD, OPENAL, or NONE.")
endif()

if("x${BARONY_AUDIO_BACKEND_NORMALIZED}" STREQUAL "xAUTO")
	# OPENAL_ENABLED historically won when both legacy flags were enabled.
	if(OPENAL_ENABLED)
		set(BARONY_AUDIO_BACKEND_RESOLVED "OPENAL")
		if(FMOD_ENABLED)
			message(WARNING
				"Both legacy audio switches are enabled; selecting OpenAL as before. "
				"Use -DBARONY_AUDIO_BACKEND=FMOD or OPENAL to select explicitly.")
		endif()
	elseif(FMOD_ENABLED)
		set(BARONY_AUDIO_BACKEND_RESOLVED "FMOD")
	else()
		set(BARONY_AUDIO_BACKEND_RESOLVED "NONE")
	endif()
else()
	set(BARONY_AUDIO_BACKEND_RESOLVED "${BARONY_AUDIO_BACKEND_NORMALIZED}")
endif()

# These values exclusively drive Config.hpp and link selection. Never infer the
# active backend from a stale *_FOUND value left in a reused CMake cache.
set(FMOD 0)
set(OPENAL 0)
message(STATUS "Barony audio backend: ${BARONY_AUDIO_BACKEND_RESOLVED}")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

set(CMAKE_OSX_ARCHITECTURES x86_64)

if (EDITOR_ENABLED)
	set(EDITOR_EXE_NAME "editor" CACHE STRING "Editor executable name")
endif()
option(DATA_DIR "Points to install data directory" FALSE)

if (STEAMWORKS_ENABLED)
	message("Building with steamworks")
else()
	message("Building without steamworks")
endif()

if (EOS_ENABLED)
	message("Building with EOS")
else()
	message("Building without EOS")
endif()

if (PLAYFAB_ENABLED)
	message("Building with PlayFab")
else()
	message("Building without PlayFab")
endif()

if (THEORAPLAYER_ENABLED)
    message("Building with TheoraPlayer")
else()
    message("Building without TheoraPlayer")
endif()

if (CURL_ENABLED)
	message("Building with CURL")
else()
	message("Building without CURL")
endif()

if (OPUS_ENABLED)
	message("Building with OPUS")
else()
	message("Building without OPUS")
endif()

# OS/Platform Flags
set(Apple 0)
set(Windows 0)
set(Linux 0)
set(Bsd 0)
set(Haiku 0)
set(PANDORA 0)
set(NOT_DWORD_DEFINED 0)
option(PANDORA_ENABLED "Build for the OpenPandora" OFF)
if(PANDORA_ENABLED)
	set(PANDORA 1)
endif()

# Set platform-dependent compiler & linker flags
if( NOT WIN32 )
  if(PANDORA)
	set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -ffast-math -funroll-loops -fstrict-aliasing -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp -g -fuse-ld=gold -fuse-linker-plugin")
	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ")
	set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${OPTIMIZATION_LEVEL} -ffast-math -funroll-loops -fstrict-aliasing -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp -g -fuse-ld=gold -fuse-linker-plugin -fsingle-precision-constant")
	set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${OPTIMIZATION_LEVEL} -ffast-math -funroll-loops -fstrict-aliasing -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp -g -fuse-ld=gold -fuse-linker-plugin -fsingle-precision-constant")
  else()
	set(DISABLED_WARNINGS "-Wno-pedantic -Wno-empty-body -Wno-string-plus-int -Wno-parentheses -Wno-format-security -Wno-multichar")
	set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} ${DISABLED_WARNINGS} -pg -ffast-math -funroll-loops -fstrict-aliasing")
	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${DISABLED_WARNINGS} -Wno-inconsistent-missing-override -pg")
	set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${DISABLED_WARNINGS} ${OPTIMIZATION_LEVEL} -ffast-math -funroll-loops -fstrict-aliasing")
	set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${DISABLED_WARNINGS} -Wno-inconsistent-missing-override ${OPTIMIZATION_LEVEL}")
  endif()
  if (APPLE)
	set (Apple 1)
	set (NOT_DWORD_DEFINED 1)
	#set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -x objective-c")
	#set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -x objective-c")
	set(MACOSX_BUNDLE true)
	set(MACOSX_BUNDLE_BUNDLE_NAME Barony)
	#TODO: Make version a define which is also set in src/Config.h
	set(MACOSX_BUNDLE_INFO_STRING Barony 2.0.2)
	set(MACOSX_BUNDLE_LONG_VERSION 2.0.2)
	set(MACOSX_BUNDLE_SHORT_VERSION_STRING 2.0.2)
	set(MACOSX_BUNDLE_BUNDLE_VERSION 2.0.2)
	set(MACOSX_BUNDLE_ICON_FILE "barony.icns")
	#set(MACOSX_BUNDLE_ICON_FILE "game.icns")
	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -std=c++17 -stdlib=libc++ -I/opt/local/include/")
	set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -std=c++17 -stdlib=libc++ -I/opt/local/include/")
  else()
	set (Apple 0)
	set (Linux 1) #TODO: Should not assume this? What if BSD/DOS/whatever-have-you?
	if (${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD|NetBSD|DragonFly") # OpenBSD is purposely discarded at it lacks for now current exe path retrieval feature
	   set(Bsd 1)
	endif()
	if (${CMAKE_SYSTEM_NAME} MATCHES "Haiku")
	   set(Haiku 1)
	endif()
	set (NOT_DWORD_DEFINED 1)
	if (NOT DISABLE_MALIGN_DOUBLE) #Only compile with -malign-double if not told to disable. Pass via command line flags.
	  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -malign-double")
	  set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -malign-double")
	endif()
	set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -rdynamic")
	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -std=c++17")
	set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
	set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -std=c++17")
	if (CMAKE_COMPILER_IS_GNUCXX)
		set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Wl,-rpath='$ORIGIN'")
		set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wl,-rpath='$ORIGIN'")
		set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -Wl,-rpath='$ORIGIN'")
		set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Wl,-rpath='$ORIGIN'")
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath='$ORIGIN'")
	endif()
  endif()

	if (ARCHITECTURE) # TODO: Support this on Windows too.
		set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -mcpu=${ARCHITECTURE}")
		set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_DEBUG} -mcpu=${ARCHITECTURE}")
	endif()
else( NOT WIN32 )
	# Windows was previously configured with GCC flags for C and Visual Studio
	# flags for C++, which made both toolchains fail in different ways. Keep the
	# compiler-specific flags separate and make the C++17 requirement explicit.
	set(Windows 1)
	if (MSVC)
		set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MP /std:c11 /experimental:c11atomics")
		set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MP /std:c11 /experimental:c11atomics")
		set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MP /std:c++17")
		set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MP /std:c++17")
	elseif (MINGW)
		set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -ffast-math -funroll-loops -malign-double -fstrict-aliasing")
		set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${OPTIMIZATION_LEVEL} -ffast-math -funroll-loops -malign-double -fstrict-aliasing")
		set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -std=c++17")
		set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -std=c++17 ${OPTIMIZATION_LEVEL}")
	else()
		message(WARNING
			"Windows compiler '${CMAKE_CXX_COMPILER_ID}' has no tuned flag profile. "
			"Use MSVC/clang-cl or the documented MSYS2 MinGW-w64 profile.")
	endif()
endif( NOT WIN32 )

if (APPLE)
	set ( CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -LD_RUNPATH_SEARCH_PATHS=@loader_path/../Frameworks -rpath @loader_path/../Frameworks")
endif()
set ( CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
set ( CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS}")

cmake_policy(SET CMP0072 NEW) # use new opengl libs on linux

IF (APPLE)
  #include_directories(/usr/X11R6/include/)
  #link_directories(/usr/X11R6/lib)
  #FIND_LIBRARY(OpenGL_LIBRARY OpenGL )
  #SET(EXTRA_LIBS ${OpenGL_LIBRARY} X11)
  FIND_LIBRARY(OpenGL_LIBRARY OpenGL )
  SET(EXTRA_LIBS ${OpenGL_LIBRARY})

  #set(CMAKE_LIBRARY_PATH "/Library/Frameworks/")
  find_package(SDL2 REQUIRED)
  find_package(SDL2_image REQUIRED)
  find_package(SDL2_net REQUIRED)
  find_package(SDL2_ttf REQUIRED)
  find_package(IOKit REQUIRED)
ELSE (APPLE)
	find_package(OpenGL REQUIRED)
	find_package(GLEW REQUIRED)
	include_directories(${GLEW_INCLUDE_DIRS} ${GLEW_INCLUDE_DIR})
	if (TARGET GLEW::GLEW)
		set(BARONY_GLEW_LIBRARIES GLEW::GLEW)
	else()
		set(BARONY_GLEW_LIBRARIES ${GLEW_LIBRARIES})
	endif()
	IF(NOT WIN32)
		INCLUDE(FindPkgConfig)
		PKG_SEARCH_MODULE(SDL2 REQUIRED sdl2)
		PKG_SEARCH_MODULE(SDL2IMAGE REQUIRED SDL2_image>=2.0.0)
		PKG_SEARCH_MODULE(SDL2NET REQUIRED SDL2_net)
		PKG_SEARCH_MODULE(SDL2TTF REQUIRED SDL2_ttf)
	ELSE(NOT WIN32)
		find_package(SDL2 REQUIRED)
		find_package(SDL2_image REQUIRED)
	ENDIF(NOT WIN32)
	find_package(SDL2_net REQUIRED) # ...why the duplication?
	find_package(SDL2_ttf REQUIRED)
ENDIF (APPLE)
# Prefer the repository's FindPhysFS module here.  vcpkg's config package can
# otherwise replace an explicitly supplied locally patched PhysFS library,
# leaving the linked import library and staged runtime DLL out of sync.
find_package(PhysFS REQUIRED MODULE)

# Preserve one concrete import library separately. The vcpkg package provider
# commonly reports a standard CMake `optimized;...;debug;...` list here, which
# is valid for target_link_libraries() but not for FILEPATH/EXISTS checks. The
# packaged Windows Release binaries must use the first existing (release)
# import library so their staged runtime DLL stays paired with it.
set(_barony_physfs_import_library "")
foreach(_barony_physfs_candidate IN LISTS PHYSFS_LIBRARY)
	if (EXISTS "${_barony_physfs_candidate}")
		set(_barony_physfs_import_library "${_barony_physfs_candidate}")
		break()
	endif()
endforeach()
if (NOT _barony_physfs_import_library)
	message(FATAL_ERROR "PhysFS did not provide a concrete import library: ${PHYSFS_LIBRARY}")
endif()
set(BARONY_PHYSFS_LIBRARY "${_barony_physfs_import_library}" CACHE FILEPATH
	"Exact PhysicsFS import library used by Barony targets" FORCE)
if (NOT EXISTS "${BARONY_PHYSFS_LIBRARY}")
	message(FATAL_ERROR
		"BARONY_PHYSFS_LIBRARY is not a valid PhysicsFS import library: ${BARONY_PHYSFS_LIBRARY}")
endif()

if (WIN32)
	# Keep the runtime DLL paired with the exact PhysFS import library selected
	# for this build. This matters when a locally patched PhysFS is supplied
	# ahead of a package-manager copy.
	get_filename_component(_barony_physfs_library_dir "${BARONY_PHYSFS_LIBRARY}" DIRECTORY)
	find_file(BARONY_PHYSFS_RUNTIME_DLL
		NAMES physfs.dll
		PATHS
			"${_barony_physfs_library_dir}"
			"${_barony_physfs_library_dir}/../bin"
		NO_DEFAULT_PATH)
	if (NOT BARONY_PHYSFS_RUNTIME_DLL)
		message(FATAL_ERROR
			"PhysFS was found, but its Windows runtime physfs.dll was not found beside ${BARONY_PHYSFS_LIBRARY}.")
	endif()
endif()

# The procedural-room generation characterization also exercises the
# production PhysFS scanner.  The test target is declared above so it can keep
# the normal BUILD_TESTING layout, while the dependency is attached here once
# PhysFS has been discovered.
if (BUILD_TESTING AND TARGET procedural_room_generation_tests)
	target_sources(
		procedural_room_generation_tests
		PRIVATE
		"${PROJECT_SOURCE_DIR}/src/procedural_room_catalog_runtime.cpp"
	)
	target_include_directories(
		procedural_room_generation_tests
		PRIVATE
		"${PHYSFS_INCLUDE_DIR}"
	)
	target_link_libraries(
		procedural_room_generation_tests
		PRIVATE
		${BARONY_PHYSFS_LIBRARY}
	)
endif()
find_package(RapidJson REQUIRED)
INCLUDE_DIRECTORIES(${RAPID_JSON_INCLUDE_DIR})

if (STEAMWORKS_ENABLED)
	# Keep the Steam build self-contained. The repository already vendors
	# nativefiledialog-extended, while the old FindNFD module only searches an
	# externally preinstalled binary and makes a fresh Windows configure fail.
	set(_barony_bundled_nfd_dir
		"${PROJECT_SOURCE_DIR}/build-deps/nativefiledialog-extended")
	if (EXISTS "${_barony_bundled_nfd_dir}/src/include/nfd.h")
		set(NFD_BUILD_TESTS OFF CACHE BOOL "Build nativefiledialog tests" FORCE)
		set(NFD_INSTALL OFF CACHE BOOL "Install nativefiledialog" FORCE)
		add_subdirectory("${_barony_bundled_nfd_dir}"
			"${CMAKE_BINARY_DIR}/nativefiledialog-extended" EXCLUDE_FROM_ALL)
		set(NFD_LIBRARY nfd)
		set(NFD_LIBRARIES nfd)
		set(NFD_INCLUDE_DIR "${_barony_bundled_nfd_dir}/src/include")
	else()
		find_package(NFD REQUIRED)
	endif()
	INCLUDE_DIRECTORIES(${NFD_INCLUDE_DIR})

	if (UNIX AND NOT APPLE)
		find_package(PkgConfig REQUIRED)
		pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
	endif()
endif()

INCLUDE_DIRECTORIES(${SDL2_INCLUDE_DIRS} ${SDL2_INCLUDE_DIR} ${SDL2IMAGE_INCLUDE_DIRS} ${SDL2_IMAGE_INCLUDE_DIR} ${SDL2NET_INCLUDE_DIR} ${SDL2_NET_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIR}  ${SDL2_TTF_INCLUDE_DIRS} ${PHYSFS_INCLUDE_DIR})
LINK_DIRECTORIES(${SDL2_LINK_DIRS} ${SDL2_LINK_DIR} ${SDL2IMAGE_LINK_DIRS} ${SDL2_IMAGE_LINK_DIR} ${SDL2NET_LINK_DIR} ${SDL2_NET_LINK_DIRS} ${SDL2TTF_LINK_DIR}  ${SDL2_TTF_LINK_DIRS} ${PHYSFS_LINK_DIR})

# if (NOT WIN32)
# else()
#   INCLUDE_DIRECTORIES(${SDL2_INCLUDE_DIRS} ${SDL2IMAGE_INCLUDE_DIRS})
# endif()
if ("x${BARONY_AUDIO_BACKEND_RESOLVED}" STREQUAL "xOPENAL")
  find_package(OpenAL REQUIRED)
  if(TREMOR_ENABLED)
	find_package(TREMOR REQUIRED)
  else()
	find_package(OGGVORBIS REQUIRED)
  endif()
  if(NOT OPENAL_FOUND)
	message(FATAL_ERROR "OpenAL was selected but the OpenAL library was not found.")
  endif()
  INCLUDE_DIRECTORIES(${OPENAL_INCLUDE_DIR})
  if(TREMOR_ENABLED)
	if(NOT TREMOR_FOUND)
	  message(FATAL_ERROR "OpenAL with Tremor was selected but Tremor was not found.")
	endif()
	set(TREMOR 1)
	INCLUDE_DIRECTORIES(${TREMOR_INCLUDE_DIR})
  else()
	if(NOT OGGVORBIS_FOUND)
	  message(FATAL_ERROR "OpenAL was selected but libogg/libvorbis were not found.")
	endif()
	INCLUDE_DIRECTORIES(${OGG_INCLUDE_DIR} ${VORBIS_INCLUDE_DIR})
  endif()
  set(OPENAL 1)
endif()

set(IMGUI 1)

find_package(Threads REQUIRED)
if ("x${BARONY_AUDIO_BACKEND_RESOLVED}" STREQUAL "xFMOD")
	find_package(FMOD REQUIRED)
	if (NOT FMOD_FOUND)
		message(FATAL_ERROR "FMOD was selected but the FMOD library was not found.")
	endif()
	set(FMOD 1)
	if (WIN32)
		# FMOD's import library is not the runtime DLL. Keep the DLL beside the
		# executables in developer builds as well as in the CPack payload.
		get_filename_component(_barony_fmod_library_dir "${FMOD_LIBRARY}" DIRECTORY)
		find_file(BARONY_FMOD_RUNTIME_DLL
			NAMES fmod.dll
			PATHS "${_barony_fmod_library_dir}"
			NO_DEFAULT_PATH)
		if (NOT BARONY_FMOD_RUNTIME_DLL)
			message(FATAL_ERROR
				"FMOD was found, but its Windows runtime fmod.dll was not found beside ${FMOD_LIBRARY}.")
		endif()
	endif()
endif()
if(NOT APPLE)
  #Linux needs these two.
  #Macs have their own special line.
  find_package(PNG REQUIRED)
elseif (APPLE AND FMOD)
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${FMOD_CXX_FLAGS}")
  MESSAGE("CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
  set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${FMOD_LINK_FLAGS}")
  MESSAGE("CMAKE_EXE_LINKER_FLAGS: ${CMAKE_EXE_LINKER_FLAGS}")
endif()

if(STEAMWORKS_ENABLED)
  find_package(STEAMWORKS REQUIRED)
  include_directories(${STEAMWORKS_INCLUDE_DIR})
endif()

if(EOS_ENABLED)
  find_package(EOS REQUIRED)
  include_directories(${EOS_INCLUDE_DIR})
endif()

if (PLAYFAB_ENABLED)
  find_package(PLAYFAB REQUIRED)
  include_directories(${PLAYFAB_INCLUDE_DIR})
  link_directories(${PLAYFAB_LIBRARY})
  find_package(JsonCPP REQUIRED)
  INCLUDE_DIRECTORIES(${JSONCPP_INCLUDE_DIR})
  LINK_DIRECTORIES(${JSONCPP_LIBRARY})
endif()

if (THEORAPLAYER_ENABLED)
  find_package(TheoraPlayer REQUIRED)
  include_directories(${THEORAPLAYER_INCLUDE_DIR})
  link_directories(${THEORAPLAYER_LIBRARIES})
  find_package(OGGVORBIS REQUIRED)
  include_directories(${OGG_INCLUDE_DIR} ${VORBIS_INCLUDE_DIR})
  link_directories(${OGGVORBIS_LIBRARIES})
endif()

if (CURL_ENABLED)
  find_package(OpenSSL REQUIRED)
  include_directories(${OPENSSL_INCLUDE_DIR})
  link_directories(${OPENSSL_LIBRARY})
  find_package(CURL REQUIRED)
  include_directories(${CURL_INCLUDE_DIR})
  link_directories(${CURL_LIBRARIES})
endif()

if (OPUS_ENABLED)
  find_package(Opus REQUIRED)
  include_directories(${OPUS_INCLUDE_DIR})
  link_directories(${OPUS_LIBRARIES})
endif()

# Add include directories
#include_directories(${GLM_INCLUDE_DIR})
if( NOT WIN32 )
 	include_directories(${THREADS_INCLUDE_DIR})
endif()
if(NOT APPLE)
 	include_directories(${PNG_INCLUDE_DIR})
endif()
if (FMOD)
	include_directories(${FMOD_INCLUDE_DIR})
endif()

# Add the source directory
#file(GLOB_RECURSE SOURCE_FILES src/*.c src/*.h) # Can't do this because it'd then compile barony and editor together. Which just won't work.
#file(GLOB_RECURSE BOOK_FILES books/*)
#file(GLOB_RECURSE IMAGE_FILES images/*)
#file(GLOB_RECURSE ITEM_FILES items/*)
#file(GLOB_RECURSE MAP_FILES maps/*)
#file(GLOB_RECURSE MODEL_FILES models/*)
#file(GLOB_RECURSE MUSIC_FILES music/*)
#file(GLOB_RECURSE SOUND_FILES sound/*)

#add_subdirectory(books)
 
# Keep generated build configuration private to each CMake build directory.
# This prevents build-super-posix and build (for example Steam-off vs Steam-on)
# from overwriting a single shared src/Config.hpp in the source tree.
set(BARONY_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${BARONY_GENERATED_INCLUDE_DIR}")
configure_file(
	"${PROJECT_SOURCE_DIR}/src/Config.hpp.in"
	"${BARONY_GENERATED_INCLUDE_DIR}/Config.hpp"
)
include_directories(BEFORE "${BARONY_GENERATED_INCLUDE_DIR}")
if(BUILD_TESTING)
	add_test(
		NAME audio_backend_configuration
		COMMAND "${CMAKE_COMMAND}"
			-DGENERATED_CONFIG=${BARONY_GENERATED_INCLUDE_DIR}/Config.hpp
			-DEXPECTED_BACKEND=${BARONY_AUDIO_BACKEND_RESOLVED}
			-P "${PROJECT_SOURCE_DIR}/tests/audio_backend_configuration.cmake"
	)
endif()

message("***************************")
message("Debug flags: ")
message("${CMAKE_C_FLAGS_DEBUG}")
message("${CMAKE_CXX_FLAGS_DEBUG}")
#-g -pg -ffast-math -funroll-loops -fstrict-aliasing
message("Release flags: ")
message("${CMAKE_C_FLAGS_RELEASE}")
message("${CMAKE_CXX_FLAGS_RELEASE}")
#-O3 -DNDEBUG -pg -ffast-math -funroll-loops -fstrict-aliasing
message("***************************")


add_subdirectory(src)
if( WIN32 )
	set(GAME_RESOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/barony.rc")
	set(EDITOR_RESOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/editor.rc")
endif()

#ADD_DEFINITONS(${PNG_DEFINITIONS})
if (APPLE)
  set(COPY_FRAMEWORKS ${PROJECT_SOURCE_DIR}/../Frameworks/SDL.framework ${PROJECT_SOURCE_DIR}/../Frameworks/SDL_image.framework ${PROJECT_SOURCE_DIR}/../Frameworks/SDL_mixer.framework ${PROJECT_SOURCE_DIR}/../Frameworks/SDL_net.framework)
  message("{COPY_FRAMEWORKS}: ${COPY_FRAMEWORKS}")
  if (FMOD)
	SET_SOURCE_FILES_PROPERTIES(libfmod.dylib PROPERTIES MACOSX_PACKAGE_LOCATION MacOS) # TODO: Use ${FMOD_LIBRARY} instead?
  endif()
  #add_executable(barony MACOSX_BUNDLE OSX/SDLmain.m ${GAME_SOURCES} ${COPY_FRAMEWORKS} ${BOOK_FILES} ${IMAGE_FILES} ${ITEM_FILES} ${MAP_FILES} ${MODEL_FILES} ${MUSIC_FILES} ${SOUND_FILES} libfmodex.dylib)
  #add_executable(barony MACOSX_BUNDLE OSX/SDLmain.m ${GAME_SOURCES} ${COPY_FRAMEWORKS} libfmodex.dylib /opt/local/lib/libpng16.16.dylib)
  #add_executable(barony MACOSX_BUNDLE OSX/SDLmain.m ${GAME_SOURCES} ${MACOSX_BUNDLE_ICON_FILE} libfmodex.dylib /opt/local/lib/libpng16.16.dylib)
  if (GAME_ENABLED)
	add_executable(barony MACOSX_BUNDLE ${GAME_SOURCES} ${MACOSX_BUNDLE_ICON_FILE} ${PROJECT_SOURCE_DIR}/libpng16.16.dylib)
  #add_executable(barony OSX/SDLmain.m ${GAME_SOURCES})
  #add_executable(barony ${GAME_SOURCES})
  #SET_SOURCE_FILES_PROPERTIES(${COPY_FRAMEWORKS} PROPERTIES MACOSX_PACKAGE_LOCATION Frameworks)
  #SET_SOURCE_FILES_PROPERTIES(${BOOK_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/books/)
  #SET_SOURCE_FILES_PROPERTIES(${IMAGE_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/images/)
  #SET_SOURCE_FILES_PROPERTIES(${ITEM_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/items/)
  #SET_SOURCE_FILES_PROPERTIES(${MAP_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/maps/)
  #SET_SOURCE_FILES_PROPERTIES(${MODEL_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/models/)
  #SET_SOURCE_FILES_PROPERTIES(${MUSIC_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/music/)
  #SET_SOURCE_FILES_PROPERTIES(${SOUND_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/sound/)
	set_source_files_properties(${GAME_SOURCES} PROPERTIES COMPILE_FLAGS "-x objective-c++")
  endif(GAME_ENABLED)
  SET_SOURCE_FILES_PROPERTIES(${PROJECT_SOURCE_DIR}/libpng16.16.dylib PROPERTIES MACOSX_PACKAGE_LOCATION MacOS)
  SET_SOURCE_FILES_PROPERTIES(${COPY_RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  SET_SOURCE_FILES_PROPERTIES(${MACOSX_BUNDLE_ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  #set_source_files_properties(${GAME_SOURCES} PROPERTIES COMPILE_FLAGS "-stdlib=libc++")
else()
  if (GAME_ENABLED)
	add_executable(barony ${GAME_SOURCES} ${GAME_RESOURCE_FILE})
  endif (GAME_ENABLED)
endif(APPLE)

if (GAME_ENABLED AND WIN32)
	# Only the game is a GUI subsystem executable; tests keep their console
	# subsystem so their normal int main() entry points link on Windows.
	if (BUILD_TESTING)
		set_target_properties(barony PROPERTIES WIN32_EXECUTABLE FALSE)
	else()
		set_target_properties(barony PROPERTIES WIN32_EXECUTABLE TRUE)
	endif()
endif()

if (GAME_ENABLED)
  if(BUILD_TESTING)
	target_sources(
		barony
		PRIVATE
			"${PROJECT_SOURCE_DIR}/tests/playable_z_runtime_characterization.cpp"
	)
	target_compile_definitions(
		barony
		PRIVATE
			AUTOMATIA_STAGE4B_TEST_HOOK=1
	)
	add_test(
		NAME playable_z_runtime_characterization
		COMMAND barony --automatia-stage4d-z3-transition-characterization
	)
	set_tests_properties(
		playable_z_runtime_characterization
		PROPERTIES
			WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
	)
	# This test launches the full graphical game loop. It is not a meaningful
	# unattended Windows/CI test without a display, audio device, and explicit
	# exit control; leave it visible but disabled unless an interactive runner
	# opts in.
	if (WIN32 AND NOT BARONY_ENABLE_GRAPHICAL_RUNTIME_CTEST)
		set_tests_properties(
			playable_z_runtime_characterization
			PROPERTIES
				DISABLED TRUE
				LABELS "requires-interactive-windows-runtime"
		)
	endif()
  endif()
	if (SAM_FRAMEWORK_ENABLED)
	set(SAM_RUNTIME_28_SOURCES
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_backup.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_bodies.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_catalog.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_classes.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_combat.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_effects.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_event.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_hud.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_images.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_items.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_js_runtime.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_loader.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_lua_runtime.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_models.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_monster_patches.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_monsters.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_patcher.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_races.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_recipes.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_rooms.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_sounds.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_spells.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_ui.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_workbench.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_world.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_world_state.cpp"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/quickjs/quickjs-amalgam.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lapi.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lauxlib.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lbaselib.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lcode.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lcorolib.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lctype.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/ldblib.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/ldebug.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/ldo.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/ldump.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lfunc.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lgc.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/linit.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/liolib.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/llex.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lmathlib.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lmem.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/loadlib.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lobject.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lopcodes.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/loslib.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lparser.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lstate.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lstring.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lstrlib.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/ltable.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/ltablib.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/ltm.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lundump.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lutf8lib.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lvm.c"
		"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54/lzio.c"
	)
	# MSVC exposes C11 <stdatomic.h> behind an experimental switch.  Keep this
	# source-specific because the rest of the legacy C sources do not require it.
	set_source_files_properties(
		"${PROJECT_SOURCE_DIR}/src/sam/framework/quickjs/quickjs-amalgam.c"
		PROPERTIES
			COMPILE_OPTIONS "/experimental:c11atomics"
	)
	target_sources(
		barony
		PRIVATE
			${SAM_RUNTIME_28_SOURCES}
			"${PROJECT_SOURCE_DIR}/src/sam/sam_automatia_adapter.cpp"
			"${PROJECT_SOURCE_DIR}/src/sam/sam_foundation.cpp"
			"${PROJECT_SOURCE_DIR}/src/sam/sam_class_registry_foundation.cpp"
			"${PROJECT_SOURCE_DIR}/src/sam/sam_item_registry_foundation.cpp"
			"${PROJECT_SOURCE_DIR}/src/sam/sam_content_catalog.cpp"
			"${PROJECT_SOURCE_DIR}/src/sam/sam_item_catalog_exporter.cpp"
			"${PROJECT_SOURCE_DIR}/src/sam/sam_item_definition_bridge.cpp"
			"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_logger.cpp"
			"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_errors.cpp"
			"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_sync.cpp"
			"${PROJECT_SOURCE_DIR}/src/sam/framework/sam_workshop.cpp"
	)
	target_include_directories(
		barony
		PRIVATE
			"${PROJECT_SOURCE_DIR}/src"
			"${PROJECT_SOURCE_DIR}/src/sam"
			"${PROJECT_SOURCE_DIR}/src/sam/framework"
			"${PROJECT_SOURCE_DIR}/src/sam/framework/lua54"
			"${PROJECT_SOURCE_DIR}/src/sam/framework/quickjs"
	)
	target_compile_definitions(
		barony
		PRIVATE
			SAM_FRAMEWORK_ENABLED=1
	)
	message(
		STATUS
		"Building Automatia with S.A.M 2.8.0 runtime and stable-identity adapters"
	)
  endif()

  if(WIN32)
    # Only for MinGW...
	# target_link_libraries(barony -lmingw32)
	# target_link_libraries(barony -lSDL2main)
	# target_link_libraries(barony -lSDL2)
	# target_link_libraries(barony -lSDL2_net)
	# target_link_libraries(barony -lSDL2_image)
	# target_link_libraries(barony -lSDL2_ttf)
	# if(STEAMWORKS_ENABLED)
	#   #target_link_libraries(barony -lsteamworks_cwrapper -lstdc++)
	#   target_link_libraries(barony -lsteam_api)
	# endif()
	# if(EOS_ENABLED)
	#   target_link_libraries(barony -lsteam_api) #TODO: Fix for Windows.
	# endif()
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT barony) # Otherwise, hitting "debug" will default to "ALL_BUILD" :|
	if (DEFINED BARONY_DATADIR AND NOT "${BARONY_DATADIR}" STREQUAL "")
		set_target_properties(barony PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${BARONY_DATADIR}")
	endif()
	target_link_libraries(barony
		${SDL2_LIBRARIES}
		${SDL2_LIBRARY}
		${SDL2IMAGE_LIBRARIES}
		${SDL2_IMAGE_LIBRARIES}
		${SDL2_NET_LIBRARIES}
		${SDL2_TTF_LIBRARIES}
		${SDL2TTF_LIBRARY}
	)

	if (STEAMWORKS_ENABLED)
		target_link_libraries(barony ${STEAMWORKS_LIBRARY})
		target_link_libraries(barony ${NFD_LIBRARY})

		if (UNIX AND NOT APPLE)
			target_link_libraries(barony PkgConfig::GTK3)
		endif()
	endif()
	if (EOS_ENABLED)
		target_link_libraries(barony ${EOS_LIBRARIES})
	endif()
	if (PLAYFAB_ENABLED)
	    target_link_libraries(barony ${PLAYFAB_LIBRARY})
	    target_link_libraries(barony ${JSONCPP_LIBRARY})
	endif()
	if (THEORAPLAYER_ENABLED)
	    target_link_libraries(barony ${THEORAPLAYER_LIBRARIES} ${OGGVORBIS_LIBRARIES})
	endif()
	if (OPUS_ENABLED)
	    target_link_libraries(barony ${OPUS_LIBRARIES})
	endif()
	if (CURL_ENABLED)
	  target_link_libraries(barony ${CURL_LIBRARIES} ${OPENSSL_LIBRARIES})
	  #target_link_libraries(barony OpenSSL::applink)
	  #target_link_libraries(barony OpenSSL::SSL)
	  #target_link_libraries(barony OpenSSL::Crypto)
	endif()
  else(WIN32)
	if (CURL_ENABLED)
	  target_link_libraries(barony ${CURL_LIBRARIES} ${OPENSSL_LIBRARIES})
	  #target_link_libraries(barony OpenSSL::applink)
	  #target_link_libraries(barony OpenSSL::SSL)
	  #target_link_libraries(barony OpenSSL::Crypto)
	endif()
	if (THEORAPLAYER_ENABLED)
	    target_link_libraries(barony ${THEORAPLAYER_LIBRARIES} ${OGGVORBIS_LIBRARIES})
	endif()
	if (OPUS_ENABLED)
	    target_link_libraries(barony ${OPUS_LIBRARIES})
	endif()
	if (PLAYFAB_ENABLED)
	  target_link_libraries(barony ${PLAYFAB_LIBRARY})
	  target_link_libraries(barony ${JSONCPP_LIBRARY})
	endif()
	if( ("${CMAKE_SIZEOF_VOID_P}" EQUAL "8") AND (NOT APPLE) )
	  # 64 bit
	  target_link_libraries(barony -L/usr/lib64 -lstdc++)
	endif()
	target_link_libraries(barony ${SDL2_LIBRARIES} ${SDL2_LIBRARY} ${SDL2IMAGE_LIBRARIES} ${SDL2_IMAGE_LIBRARIES} ${SDL2_NET_LIBRARIES} ${SDL2_TTF_LIBRARIES} ${SDL2TTF_LIBRARY})
	if (STEAMWORKS_ENABLED)
	target_link_libraries(barony
			${STEAMWORKS_LIBRARY}
			${NFD_LIBRARY}
		)

		if (UNIX AND NOT APPLE)
			target_link_libraries(barony PkgConfig::GTK3)
		endif()
	endif()
	if (EOS_ENABLED)
	  target_link_libraries(barony ${EOS_LIBRARIES})
	endif()
	if (APPLE)
	  target_link_libraries(barony ${PROJECT_SOURCE_DIR}/libpng16.16.dylib) #Wait...what? if(APPLE) in if(WIN32)? What was I thinking back then, haha.
	endif()
	if (${CMAKE_SYSTEM_NAME} MATCHES "BSD|DragonFly" OR ${CMAKE_SYSTEM_NAME} MATCHES "Haiku")
	  # For backtrace
	  find_path(EXECINFO_INC NAMES execinfo.h)
	  find_library(EXECINFO_LIB NAMES execinfo)
	  include_directories(${EXECINFO_INC})
	  target_link_libraries(barony ${EXECINFO_LIB})
	endif()
	#if(LINUX)
		target_link_libraries(barony -lpthread)
	#endif(LINUX)
  endif()
	  target_link_libraries(barony ${OPENGL_LIBRARIES})
	  target_link_libraries(barony ${BARONY_GLEW_LIBRARIES})
  target_link_libraries(barony ${THREADS_LIBRARIES}) # TODO: Not sure what this is doing, since it doesn't appear to be working in Linux!
  if (UNIX)
	target_link_libraries(barony -lm)
	if (NOT CMAKE_SYSTEM_NAME MATCHES "Haiku")
	  target_link_libraries(barony -lc)
	endif()
  endif()
  if (FMOD)
    target_link_libraries(barony ${FMOD_LIBRARY})
  endif()
  target_link_libraries(barony ${BARONY_PHYSFS_LIBRARY})
  if(NOT APPLE)
	#Remember, Mac isn't using find_package for PNG.
	target_link_libraries(barony ${PNG_LIBRARY})
  endif()
  if (APPLE)
	target_link_libraries(barony ${IOKit_LIBRARY})
  endif()
  # We need to link to Winsock if we're on Windows
  if(WIN32)
	target_link_libraries(barony wsock32 ws2_32)
	#target_link_libraries(barony -lpng -lfmodex) Mingw, not Visual Studio.
  endif()
  target_link_libraries(barony ${EXTRA_LIBS}) #Apple needs this for OpenGL to work.

  if (OPENAL)
	target_link_libraries(barony ${OPENAL_LIBRARY})
	if(TREMOR_ENABLED)
	  target_link_libraries(barony ${TREMOR_LIBRARY})
	else()
	  target_link_libraries(barony
		${VORBISFILE_LIBRARY} ${VORBIS_LIBRARY} ${OGG_LIBRARY})
	endif()
  endif()
endif(GAME_ENABLED)

set(BASE_DATA_DIR "./" CACHE INTERNAL "Base data dir")

option(BARONY_WINDOWS_INSTALLER
	"Enable the self-contained CPack/NSIS Windows installer target" OFF)

if (NOT APPLE AND UNIX)
  include(GNUInstallDirs)
  if (DATA_DIR)
	set(BASE_DATA_DIR "${CMAKE_INSTALL_PREFIX}")
  endif()
  message(STATUS "Base data directory ${BASE_DATA_DIR}")

  if (GAME_ENABLED)
	install(TARGETS barony
	RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}"
	COMPONENT Runtime
	)
  endif(GAME_ENABLED)
else()
	if (GAME_ENABLED AND NOT BARONY_WINDOWS_INSTALLER)
	install(TARGETS barony
	  RUNTIME DESTINATION bin COMPONENT Runtime BUNDLE DESTINATION Resources
	)
  endif()
endif()

add_definitions(-DBASE_DATA_DIR=\"${BASE_DATA_DIR}\")

if (EDITOR_ENABLED)
  if (APPLE)
	#add_executable(editor OSX/SDLmain.m ${EDITOR_SOURCES})
	#set(MACOSX_BUNDLE_ICON_FILE "${PROJECT_SOURCE_DIR}/editor.icns")
	#set(MACOSX_BUNDLE_ICON_FILE "editor.icns")
	#add_executable(editor MACOSX_BUNDLE OSX/SDLmain.m ${EDITOR_SOURCES} ${COPY_FRAMEWORKS} libfmodex.dylib  /opt/local/lib/libpng16.16.dylib)
	#add_executable(editor MACOSX_BUNDLE OSX/SDLmain.m ${EDITOR_SOURCES} libfmodex.dylib  /opt/local/lib/libpng16.16.dylib)
	set_source_files_properties("editor.icns" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
	add_executable(${EDITOR_EXE_NAME} MACOSX_BUNDLE "editor.icns" ${EDITOR_SOURCES} ${PROJECT_SOURCE_DIR}/libpng16.16.dylib)
	#SET_SOURCE_FILES_PROPERTIES(${MACOSX_BUNDLE_ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
	set_source_files_properties(${EDITOR_SOURCES} PROPERTIES COMPILE_FLAGS "-x objective-c++")
  else()
	if (WIN32)
		# add_executable(${EDITOR_EXE_NAME} EXCLUDE_FROM_ALL ${EDITOR_SOURCES} ${EDITOR_RESOURCE_FILE})
		add_executable(${EDITOR_EXE_NAME} ${EDITOR_SOURCES} ${EDITOR_RESOURCE_FILE})
		if (DEFINED BARONY_DATADIR AND NOT "${BARONY_DATADIR}" STREQUAL "")
			set_target_properties(${EDITOR_EXE_NAME} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${BARONY_DATADIR}")
		endif()
	else()
		add_executable(${EDITOR_EXE_NAME} ${EDITOR_SOURCES} ${EDITOR_RESOURCE_FILE})
	endif()
  endif()
	if (WIN32)
		set_target_properties(${EDITOR_EXE_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
	endif()
  target_compile_definitions(${EDITOR_EXE_NAME} PUBLIC EDITOR) # So that shared source files know this is the editor they're dealing with. # TODO: Try using this to disable some things we don't want in the editor?

  if (THEORAPLAYER_ENABLED)
    target_link_libraries(${EDITOR_EXE_NAME} ${THEORAPLAYER_LIBRARIES} ${OGGVORBIS_LIBRARIES})
  endif()
  if (OPUS_ENABLED)
    target_link_libraries(${EDITOR_EXE_NAME} ${OPUS_LIBRARIES})
  endif()
  if (CURL_ENABLED)
	target_link_libraries(${EDITOR_EXE_NAME} ${CURL_LIBRARIES} ${OPENSSL_LIBRARIES})
	#target_link_libraries(${EDITOR_EXE_NAME} OpenSSL::applink)
	#target_link_libraries(${EDITOR_EXE_NAME} OpenSSL::SSL)
	#target_link_libraries(${EDITOR_EXE_NAME} OpenSSL::Crypto)
  endif()
  target_link_libraries(${EDITOR_EXE_NAME} ${SDL2_LIBRARIES} ${SDL2_LIBRARY} ${SDL2IMAGE_LIBRARIES} ${SDL2_IMAGE_LIBRARIES} ${SDL2_NET_LIBRARIES} ${SDL2_TTF_LIBRARIES} ${SDL2TTF_LIBRARY})
	if (STEAMWORKS_ENABLED)
		target_link_libraries(${EDITOR_EXE_NAME} ${STEAMWORKS_LIBRARY})
		target_link_libraries(${EDITOR_EXE_NAME} ${NFD_LIBRARY})

		if (UNIX AND NOT APPLE)
			target_link_libraries(${EDITOR_EXE_NAME} PkgConfig::GTK3)
		endif()
	endif()
  if(WIN32)
	if (EOS_ENABLED)
	  target_link_libraries(${EDITOR_EXE_NAME} ${EOS_LIBRARY}) #TODO: Fix for Windows.
	endif()
  else()
	if( ("${CMAKE_SIZEOF_VOID_P}" EQUAL "8") AND (NOT APPLE))
	  # 64 bit
	  target_link_libraries(${EDITOR_EXE_NAME} -L/usr/lib64 -lstdc++)
	endif()
	#if(LINUX)
		target_link_libraries(${EDITOR_EXE_NAME} -lpthread)
	#endif(LINUX)
  endif()
  target_link_libraries(${EDITOR_EXE_NAME} ${OPENGL_LIBRARIES})
  target_link_libraries(${EDITOR_EXE_NAME} ${BARONY_GLEW_LIBRARIES})
  target_link_libraries(${EDITOR_EXE_NAME} ${THREADS_LIBRARIES})
  if (UNIX)
	target_link_libraries(${EDITOR_EXE_NAME} -lm)
	if (NOT CMAKE_SYSTEM_NAME MATCHES "Haiku")
	  target_link_libraries(${EDITOR_EXE_NAME} -lc)
	endif()
  endif()
  # We need to link to Winsock if we're on Windows
  if (FMOD)
    target_link_libraries(${EDITOR_EXE_NAME} ${FMOD_LIBRARY})
  endif()
  if(NOT APPLE)
	#Remember, Mac isn't using find_package for FMOD and PNG.
	target_link_libraries(${EDITOR_EXE_NAME} ${BARONY_PHYSFS_LIBRARY})
	target_link_libraries(${EDITOR_EXE_NAME} ${PNG_LIBRARY})
  endif()
  if (APPLE)
	target_link_libraries(${EDITOR_EXE_NAME} ${BARONY_PHYSFS_LIBRARY})
	target_link_libraries(${EDITOR_EXE_NAME} ${IOKit_LIBRARY})
	if (EOS_ENABLED)
	  target_link_libraries(${EDITOR_EXE_NAME} ${EOS_LIBRARY})
	endif()
  endif()
  if(WIN32)
	target_link_libraries(${EDITOR_EXE_NAME} wsock32 ws2_32)
  endif()
  if (APPLE)
	if (FMOD)
	  target_link_libraries(${EDITOR_EXE_NAME} ${FMOD_LIBRARY}) #Finally manually link fmod for mac.
	endif()
  endif()
  target_link_libraries(${EDITOR_EXE_NAME} ${EXTRA_LIBS}) #Apple needs this for OpenGL to work.
  if (OPENAL)
	target_link_libraries(${EDITOR_EXE_NAME} ${OPENAL_LIBRARY})
	if(TREMOR_ENABLED)
	  target_link_libraries(${EDITOR_EXE_NAME} ${TREMOR_LIBRARY})
	else()
	  target_link_libraries(${EDITOR_EXE_NAME}
		${VORBISFILE_LIBRARY} ${VORBIS_LIBRARY} ${OGG_LIBRARY})
	endif()
  endif()
endif(EDITOR_ENABLED)

# Visual Studio does not deploy FMOD's runtime DLL from its import library.
# Keep local game/editor builds runnable directly from the Release directory;
# the Windows install rules below use the same verified runtime file.
if (WIN32 AND FMOD AND BARONY_FMOD_RUNTIME_DLL)
	if (GAME_ENABLED)
		add_custom_command(TARGET barony POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${BARONY_FMOD_RUNTIME_DLL}" "$<TARGET_FILE_DIR:barony>/fmod.dll"
			COMMENT "Deploying FMOD runtime for barony"
			VERBATIM)
	endif()
	if (EDITOR_ENABLED)
		add_custom_command(TARGET ${EDITOR_EXE_NAME} POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${BARONY_FMOD_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${EDITOR_EXE_NAME}>/fmod.dll"
			COMMENT "Deploying FMOD runtime for editor"
			VERBATIM)
	endif()
endif()

# vcpkg's Visual Studio integration may deploy its own PhysFS DLL even when a
# developer explicitly selected a locally patched PhysFS import library. This
# dependent runtime target runs after the executable deployment steps, so its
# verified companion DLL is the final file in a normal full build.
if (WIN32 AND BARONY_PHYSFS_RUNTIME_DLL)
	set(_barony_physfs_runtime_targets)
	if (GAME_ENABLED)
		list(APPEND _barony_physfs_runtime_targets barony)
	endif()
	if (EDITOR_ENABLED)
		list(APPEND _barony_physfs_runtime_targets ${EDITOR_EXE_NAME})
	endif()
	if (GAME_ENABLED)
		set(_barony_physfs_runtime_destination "$<TARGET_FILE_DIR:barony>/physfs.dll")
	elseif (EDITOR_ENABLED)
		set(_barony_physfs_runtime_destination
			"$<TARGET_FILE_DIR:${EDITOR_EXE_NAME}>/physfs.dll")
	endif()
	if (_barony_physfs_runtime_targets)
		add_custom_target(barony_selected_physfs_runtime ALL
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${BARONY_PHYSFS_RUNTIME_DLL}" "${_barony_physfs_runtime_destination}"
			COMMENT "Deploying the selected PhysicsFS runtime after vcpkg dependencies"
			DEPENDS ${_barony_physfs_runtime_targets}
			VERBATIM)
	endif()
endif()

# Various install targets
# 1) <TODO: insert proper description here...I think they're being used by the BSD maintainer(s)?>.
if (NOT APPLE AND UNIX)
  if (EDITOR_ENABLED)
	install(TARGETS ${EDITOR_EXE_NAME}
	  RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}" COMPONENT Runtime
	)
  endif()

  install(DIRECTORY
	${CMAKE_SOURCE_DIR}/books
	${CMAKE_SOURCE_DIR}/data
	${CMAKE_SOURCE_DIR}/fonts
	${CMAKE_SOURCE_DIR}/images
	${CMAKE_SOURCE_DIR}/items
	${CMAKE_SOURCE_DIR}/lang
	${CMAKE_SOURCE_DIR}/maps
	${CMAKE_SOURCE_DIR}/models
	${CMAKE_SOURCE_DIR}/music
	${CMAKE_SOURCE_DIR}/sound
	DESTINATION "${CMAKE_INSTALL_PREFIX}"
  )

  install(FILES
	${CMAKE_SOURCE_DIR}/steam_appid.txt
	${CMAKE_SOURCE_DIR}/gamecontrollerdb.txt
	#TYPE INCLUDE # If your CMAKE is stupid like mine and it complains about "install FILES given no DESTINATION!", you can tell it to shove it by uncommenting this line.
	DESTINATION "${CMAKE_INSTALL_PREFIX}"
  )
  # Some source distributions do not contain the optional player-name lists.
  # Their absence must not make a complete runtime install/package fail.
  install(FILES
	${CMAKE_SOURCE_DIR}/playernames-female.txt
	${CMAKE_SOURCE_DIR}/playernames-male.txt
	OPTIONAL
	DESTINATION "${CMAKE_INSTALL_PREFIX}"
  )
endif()
# 2) Windows development deploy and self-contained installer targets.
if (WIN32)
	if (DEFINED BARONY_DATADIR AND NOT "${BARONY_DATADIR}" STREQUAL "")
		# Keep the Visual Studio convenience deploy separate from the distributable
		# Runtime component. CPack packages Runtime only, so an absolute developer
		# data path can never leak into an installer.
		install(FILES lang/en.txt DESTINATION "${BARONY_DATADIR}/lang"
			CONFIGURATIONS ${CMAKE_CONFIGURATION_TYPES} COMPONENT Development)
		if (GAME_ENABLED)
			install(TARGETS barony DESTINATION "${BARONY_DATADIR}"
				CONFIGURATIONS ${CMAKE_CONFIGURATION_TYPES} COMPONENT Development)
		endif()
		if (EDITOR_ENABLED)
			install(TARGETS ${EDITOR_EXE_NAME} DESTINATION "${BARONY_DATADIR}"
				CONFIGURATIONS ${CMAKE_CONFIGURATION_TYPES} COMPONENT Development)
		endif()
	endif()

	set(BARONY_AUTOMATIA_PACKAGE_VERSION "dev" CACHE STRING
		"Version label embedded in the Windows installer filename and metadata")
	set(BARONY_WINDOWS_DATA_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH
		"Directory containing the Windows runtime asset folders")
	set(BARONY_WINDOWS_NSIS_EXECUTABLE "" CACHE FILEPATH
		"Optional path to makensis.exe used for the Windows installer")
	option(BARONY_WINDOWS_RELEASE_PACKAGE_TARGET
		"Enable the opt-in audited Windows release staging target" OFF)
	set(BARONY_WINDOWS_RELEASE_VCPKG_ROOT "" CACHE PATH
		"vcpkg root used by the audited Windows release package")
	set(BARONY_WINDOWS_RELEASE_ASSETS_ROOT "" CACHE PATH
		"Authorized Barony asset source used by the audited Windows release package")
	set(BARONY_WINDOWS_RELEASE_OPENAL_SOURCE_ARCHIVE "" CACHE FILEPATH
		"Exact OpenAL Soft source archive paired with the staged OpenAL32.dll")
	set(BARONY_WINDOWS_RELEASE_PHYSFS_SOURCE "" CACHE PATH
		"Locally patched PhysicsFS source directory paired with physfs.dll")
	set(BARONY_WINDOWS_RELEASE_VC_REDIST "" CACHE FILEPATH
		"Official VC_redist.x64.exe from the Visual Studio Redist/MSVC directory")
	set(BARONY_WINDOWS_RELEASE_OUTPUT_DIR
		"${CMAKE_BINARY_DIR}/BaronyAutomatia-Windows-x64" CACHE PATH
		"Audited Windows release staging directory")
	option(BARONY_WINDOWS_STEAM_PATCH_INSTALLER
		"Build the Steam-integrated Automatia installer payload" OFF)
	option(BARONY_WINDOWS_STEAM_SIDEBYSIDE_INSTALLER
		"Install Automatia beside vanilla Barony and add it to Steam as a non-Steam shortcut" ON)
	if (BARONY_WINDOWS_STEAM_PATCH_INSTALLER AND NOT BARONY_WINDOWS_STEAM_SIDEBYSIDE_INSTALLER)
		message(FATAL_ERROR
			"In-place Steam patch installers are disabled for Automatia. "
			"Enable BARONY_WINDOWS_STEAM_SIDEBYSIDE_INSTALLER so vanilla Barony is read only.")
	endif()
	if (BARONY_WINDOWS_STEAM_PATCH_INSTALLER)
		add_executable(barony_automatia_steam_shortcut
			tools/barony_automatia_steam_shortcut.cpp)
		set_target_properties(barony_automatia_steam_shortcut PROPERTIES
			CXX_STANDARD 17
			CXX_STANDARD_REQUIRED ON)
	endif()

	if (BARONY_WINDOWS_INSTALLER)
		if (NOT GAME_ENABLED)
			message(FATAL_ERROR
				"BARONY_WINDOWS_INSTALLER requires GAME_ENABLED so the package has a launcher.")
		endif()
		if (CMAKE_VERSION VERSION_LESS "3.21")
			message(FATAL_ERROR
				"BARONY_WINDOWS_INSTALLER requires CMake 3.21+ for runtime DLL staging.")
		endif()

		# Collect only real directories. The MinGW prefix supplies the normal game
		# DLLs, while the optional SDK roots contribute steam_api/EOS at package time.
		set(BARONY_WINDOWS_RUNTIME_SEARCH_DIRS)
		foreach(_barony_runtime_dir ${BARONY_WINDOWS_RUNTIME_DLL_DIRS})
			if (IS_DIRECTORY "${_barony_runtime_dir}")
				list(APPEND BARONY_WINDOWS_RUNTIME_SEARCH_DIRS "${_barony_runtime_dir}")
			endif()
		endforeach()
		if (STEAMWORKS_ENABLED)
			foreach(_barony_sdk_root ${STEAMWORKS_SDK_ROOTS})
				if (NOT "${_barony_sdk_root}" STREQUAL "")
					list(APPEND BARONY_WINDOWS_RUNTIME_SEARCH_DIRS
						"${_barony_sdk_root}/sdk/redistributable_bin/win64")
				endif()
			endforeach()
		endif()
		if (EOS_ENABLED)
			foreach(_barony_sdk_root ${EOS_SDK_ROOTS})
				if (NOT "${_barony_sdk_root}" STREQUAL "")
					list(APPEND BARONY_WINDOWS_RUNTIME_SEARCH_DIRS
						"${_barony_sdk_root}/SDK/Bin")
				endif()
			endforeach()
		endif()
		set(_barony_runtime_dirs_existing)
		foreach(_barony_runtime_dir ${BARONY_WINDOWS_RUNTIME_SEARCH_DIRS})
			if (IS_DIRECTORY "${_barony_runtime_dir}")
				list(APPEND _barony_runtime_dirs_existing "${_barony_runtime_dir}")
			endif()
		endforeach()
		if ("${_barony_runtime_dirs_existing}" STREQUAL "")
			message(FATAL_ERROR
				"BARONY_WINDOWS_INSTALLER needs BARONY_WINDOWS_RUNTIME_DLL_DIRS. "
				"For MSYS2 UCRT64 this is normally <MINGW_PREFIX>/bin; set "
				"BARONY_WIN32_LIBRARIES to the matching prefix or set the DLL directory explicitly.")
		endif()

		if (BARONY_WINDOWS_STEAM_PATCH_INSTALLER)
			# A Steam install already contains all Barony assets. The patch installer
			# carries only the rebuilt executables and their non-system DLLs.
			if (NOT STEAMWORKS_ENABLED OR NOT OPENAL)
				message(FATAL_ERROR
					"BARONY_WINDOWS_STEAM_PATCH_INSTALLER requires Steamworks and the OpenAL backend.")
			endif()
			install(TARGETS barony
				RUNTIME DESTINATION "." COMPONENT Runtime)
			if (EDITOR_ENABLED)
				install(TARGETS ${EDITOR_EXE_NAME}
					RUNTIME DESTINATION "." COMPONENT Runtime)
			endif()
			install(TARGETS barony_automatia_steam_shortcut
				RUNTIME DESTINATION "." COMPONENT Runtime)
			set(_barony_patch_runtime_dll_names
				SDL2.dll
				SDL2_image.dll
				SDL2_net.dll
				SDL2_ttf.dll
				fmt.dll
				freetype.dll
				glew32.dll
				physfs.dll
				libpng16.dll
				z.dll
				bz2.dll
				brotlidec.dll
				brotlicommon.dll
				OpenAL32.dll
				ogg.dll
				vorbis.dll
				vorbisfile.dll
				steam_api64.dll)
			foreach(_barony_patch_runtime_dll_name ${_barony_patch_runtime_dll_names})
				unset(_barony_patch_runtime_dll CACHE)
				unset(_barony_patch_runtime_dll)
				if (_barony_patch_runtime_dll_name STREQUAL "physfs.dll")
					set(_barony_patch_runtime_dll "${BARONY_PHYSFS_RUNTIME_DLL}")
				else()
					find_file(_barony_patch_runtime_dll
						NAMES "${_barony_patch_runtime_dll_name}"
						PATHS ${_barony_runtime_dirs_existing}
						NO_DEFAULT_PATH)
				endif()
				if (NOT _barony_patch_runtime_dll)
					message(FATAL_ERROR
						"Could not find required Steam patch runtime DLL: ${_barony_patch_runtime_dll_name}")
				endif()
				install(FILES "${_barony_patch_runtime_dll}"
					DESTINATION "." COMPONENT Runtime)
			endforeach()
			if (BARONY_WINDOWS_STEAM_SIDEBYSIDE_INSTALLER)
				install(FILES
					"${BARONY_WINDOWS_RELEASE_OUTPUT_DIR}/steam_appid.txt"
					DESTINATION "." COMPONENT Runtime)
			else()
				install(FILES
					"${BARONY_WINDOWS_DATA_DIR}/steam_appid.txt"
					DESTINATION "." COMPONENT Runtime)
			endif()

			# The side-by-side installer contains every file it needs. It deliberately
			# does not package vanilla Barony assets: its NSIS install logic copies those
			# from the user's existing Steam installation instead.
			if (BARONY_WINDOWS_STEAM_SIDEBYSIDE_INSTALLER)
				if (NOT BARONY_WINDOWS_RELEASE_PACKAGE_TARGET)
					message(FATAL_ERROR
						"BARONY_WINDOWS_STEAM_SIDEBYSIDE_INSTALLER requires "
						"BARONY_WINDOWS_RELEASE_PACKAGE_TARGET so the single installer "
						"can embed its audited notices, corresponding source, and VC redist.")
				endif()
				install(DIRECTORY
					"${BARONY_WINDOWS_RELEASE_OUTPUT_DIR}/licenses/"
					DESTINATION "automatia_licenses" COMPONENT Runtime)
				install(DIRECTORY
					"${BARONY_WINDOWS_RELEASE_OUTPUT_DIR}/third_party_source/"
					DESTINATION "automatia_third_party_source" COMPONENT Runtime)
				install(FILES
					"${BARONY_WINDOWS_RELEASE_OUTPUT_DIR}/THIRD_PARTY_NOTICES.txt"
					"${BARONY_WINDOWS_RELEASE_OUTPUT_DIR}/THIRD_PARTY_MANIFEST.json"
					DESTINATION "automatia_licenses" COMPONENT Runtime)
				install(FILES
					"${BARONY_WINDOWS_RELEASE_OUTPUT_DIR}/prerequisites/VC_redist.x64.exe"
					DESTINATION "automatia_prerequisites" COMPONENT Runtime)
				install(FILES "${CMAKE_SOURCE_DIR}/LICENSE.txt"
					DESTINATION "automatia_licenses" RENAME "Automatia_LICENSE.txt"
					COMPONENT Runtime)
				# These original Automatia images are installed only to the private
				# payload directory. The shortcut helper places copies in the active
				# Steam user's grid directory after Steam is closed.
				install(FILES
					"${CMAKE_SOURCE_DIR}/tools/steam_artwork/barony_automatia_portrait.png"
					"${CMAKE_SOURCE_DIR}/tools/steam_artwork/barony_automatia_hero.png"
					"${CMAKE_SOURCE_DIR}/tools/steam_artwork/barony_automatia_icon.png"
					DESTINATION "automatia_steam_artwork" COMPONENT Runtime)

				set(BARONY_AUTOMATIA_SIDEBYSIDE_RUNTIME_FILES barony.exe)
				if (EDITOR_ENABLED)
					list(APPEND BARONY_AUTOMATIA_SIDEBYSIDE_RUNTIME_FILES editor.exe)
				endif()
				list(APPEND BARONY_AUTOMATIA_SIDEBYSIDE_RUNTIME_FILES
					${_barony_patch_runtime_dll_names}
					steam_appid.txt)
			endif()
		else()
			# Package the full standalone runtime, not an in-place replacement for a
			# Steam-managed game. That makes install, update, and uninstall reversible.
			install(TARGETS barony
				RUNTIME_DEPENDENCY_SET barony_windows_runtime_dependencies
				RUNTIME DESTINATION "." COMPONENT Runtime)
			if (EDITOR_ENABLED)
				install(TARGETS ${EDITOR_EXE_NAME}
					RUNTIME_DEPENDENCY_SET barony_windows_runtime_dependencies
					RUNTIME DESTINATION "." COMPONENT Runtime)
			endif()
			install(DIRECTORY
				"${BARONY_WINDOWS_DATA_DIR}/books"
				"${BARONY_WINDOWS_DATA_DIR}/data"
				"${BARONY_WINDOWS_DATA_DIR}/fonts"
				"${BARONY_WINDOWS_DATA_DIR}/images"
				"${BARONY_WINDOWS_DATA_DIR}/items"
				"${BARONY_WINDOWS_DATA_DIR}/lang"
				"${BARONY_WINDOWS_DATA_DIR}/maps"
				"${BARONY_WINDOWS_DATA_DIR}/models"
				"${BARONY_WINDOWS_DATA_DIR}/music"
				"${BARONY_WINDOWS_DATA_DIR}/sound"
				DESTINATION "." COMPONENT Runtime)
			install(FILES
				"${BARONY_WINDOWS_DATA_DIR}/steam_appid.txt"
				"${CMAKE_SOURCE_DIR}/gamecontrollerdb.txt"
				"${CMAKE_SOURCE_DIR}/README.md"
				"${CMAKE_SOURCE_DIR}/EDITING.txt"
				"${CMAKE_SOURCE_DIR}/LICENSE.txt"
				DESTINATION "." COMPONENT Runtime)
			install(FILES
				"${CMAKE_SOURCE_DIR}/playernames-female.txt"
				"${CMAKE_SOURCE_DIR}/playernames-male.txt"
				OPTIONAL DESTINATION "." COMPONENT Runtime)

			# GET_RUNTIME_DEPENDENCIES runs from the generated install script and walks
			# transitive DLLs. An unresolved dependency is an installation error rather
			# than a broken installer silently reaching players.
			install(RUNTIME_DEPENDENCY_SET barony_windows_runtime_dependencies
				DESTINATION "." COMPONENT Runtime
				DIRECTORIES ${_barony_runtime_dirs_existing}
				PRE_EXCLUDE_REGEXES
				"^api-ms-win-.*"
				"^ext-ms-.*"
				"^kernel32\\.dll$"
				"^user32\\.dll$"
				"^gdi32\\.dll$"
				"^advapi32\\.dll$"
				"^shell32\\.dll$"
				"^ole32\\.dll$"
				"^oleaut32\\.dll$"
				"^ws2_32\\.dll$"
				"^winmm\\.dll$"
				"^opengl32\\.dll$"
				"^glu32\\.dll$"
				"^comdlg32\\.dll$"
				"^comctl32\\.dll$"
				"^shlwapi\\.dll$"
				"^version\\.dll$"
				"^imm32\\.dll$"
				"^bcrypt\\.dll$"
				"^setupapi\\.dll$"
				"^(msvcp|vcruntime|ucrtbase).*\\.dll$"
				"^azureattestmanager\\.dll$"
				"^azureattestnormal\\.dll$"
				"^wpaxholder\\.dll$")
		endif()

		set(CPACK_GENERATOR "NSIS")
		if (NOT "${BARONY_WINDOWS_NSIS_EXECUTABLE}" STREQUAL "")
			if (NOT EXISTS "${BARONY_WINDOWS_NSIS_EXECUTABLE}")
				message(FATAL_ERROR
					"BARONY_WINDOWS_NSIS_EXECUTABLE does not name an existing makensis executable.")
			endif()
			set(CPACK_NSIS_EXECUTABLE "${BARONY_WINDOWS_NSIS_EXECUTABLE}")
		endif()
		if (BARONY_WINDOWS_STEAM_PATCH_INSTALLER)
			if (BARONY_WINDOWS_STEAM_SIDEBYSIDE_INSTALLER)
				set(CPACK_PACKAGE_NAME "Barony Automatia Steam Installer")
			else()
				set(CPACK_PACKAGE_NAME "Barony Automatia Steam Patch")
			endif()
		else()
			set(CPACK_PACKAGE_NAME "Barony Automatia")
		endif()
		set(CPACK_PACKAGE_VENDOR "Automatia")
		set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
			"Barony Automatia with editor, OpenAL, S.A.M., and extended multiplayer support")
		set(CPACK_PACKAGE_VERSION "${BARONY_AUTOMATIA_PACKAGE_VERSION}")
		if (BARONY_WINDOWS_STEAM_PATCH_INSTALLER)
			if (BARONY_WINDOWS_STEAM_SIDEBYSIDE_INSTALLER)
				set(CPACK_PACKAGE_FILE_NAME
					"Barony-Automatia-Steam-${BARONY_AUTOMATIA_PACKAGE_VERSION}-windows-x64")
			else()
				set(CPACK_PACKAGE_FILE_NAME
					"Barony-Automatia-Steam-Patch-${BARONY_AUTOMATIA_PACKAGE_VERSION}-windows-x64")
			endif()
		else()
			set(CPACK_PACKAGE_FILE_NAME
				"Barony-Automatia-${BARONY_AUTOMATIA_PACKAGE_VERSION}-windows-x64")
		endif()
		set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/installer")
		set(CPACK_PACKAGE_INSTALL_DIRECTORY "Barony Automatia")
		set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE.txt")
		set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
		set(CPACK_COMPONENTS_ALL Runtime)
		set(CPACK_NSIS_PACKAGE_NAME "Barony Automatia")
		set(CPACK_NSIS_DISPLAY_NAME "Barony Automatia")
		set(CPACK_NSIS_INSTALL_ROOT "$LOCALAPPDATA")
		set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/barony.ico")
		set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/barony.ico")
		set(CPACK_NSIS_INSTALLED_ICON_NAME "barony.exe")
		set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
		set(CPACK_NSIS_MUI_FINISHPAGE_RUN "barony.exe")
		set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
		set(CPACK_NSIS_MODIFY_PATH OFF)
		if (BARONY_WINDOWS_STEAM_PATCH_INSTALLER AND BARONY_WINDOWS_STEAM_SIDEBYSIDE_INSTALLER)
			# The real launch target is copied beside the user's Steam Barony assets.
			# Do not create CPack's default Start-menu/finish-page shortcuts to the
			# payload cache under LocalAppData, which intentionally has no game assets.
			set(CPACK_NSIS_MUI_FINISHPAGE_RUN "")
			set(CPACK_PACKAGE_EXECUTABLES "")
		else()
			set(CPACK_PACKAGE_EXECUTABLES "barony" "Barony Automatia")
			if (EDITOR_ENABLED)
				list(APPEND CPACK_PACKAGE_EXECUTABLES "editor" "Barony Automatia Editor")
			endif()
		endif()
		if (BARONY_WINDOWS_STEAM_PATCH_INSTALLER)
			# The payload is installed under the user's local app directory, then
			# copied into Steam's existing Barony directory. This keeps CPack's
			# uninstaller from deleting the backed-up Steam files before restoration.
			set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS [=[
SetRegView 64
ReadRegStr $R0 HKCU "Software\Valve\Steam" "SteamPath"
StrCmp $R0 "" barony_automatia_try_lm64
Goto barony_automatia_have_steam_root
barony_automatia_try_lm64:
ReadRegStr $R0 HKLM "Software\Valve\Steam" "InstallPath"
StrCmp $R0 "" barony_automatia_try_32
Goto barony_automatia_have_steam_root
barony_automatia_try_32:
SetRegView 32
ReadRegStr $R0 HKCU "Software\Valve\Steam" "SteamPath"
StrCmp $R0 "" barony_automatia_try_lm32
Goto barony_automatia_have_steam_root
barony_automatia_try_lm32:
ReadRegStr $R0 HKLM "Software\Valve\Steam" "InstallPath"
barony_automatia_have_steam_root:
StrCmp $R0 "" barony_automatia_default_steam_root
StrCpy $R1 "$R0\steamapps\common\Barony"
IfFileExists "$R1\barony.exe" barony_automatia_steam_found barony_automatia_default_steam_root
barony_automatia_default_steam_root:
StrCpy $R1 "$PROGRAMFILES32\Steam\steamapps\common\Barony"
IfFileExists "$R1\barony.exe" barony_automatia_steam_found barony_automatia_no_steam
barony_automatia_no_steam:
MessageBox MB_OK|MB_ICONSTOP "Steam's Barony installation was not found. Install Barony through Steam first, then run this installer again."
Abort
barony_automatia_steam_found:
IfFileExists "$R1\barony.exe.automatia-original" barony_automatia_skip_backup_barony 0
IfFileExists "$R1\barony.exe" 0 barony_automatia_skip_backup_barony
CopyFiles /SILENT "$R1\barony.exe" "$R1\barony.exe.automatia-original"
barony_automatia_skip_backup_barony:
IfFileExists "$R1\editor.exe.automatia-original" barony_automatia_skip_backup_editor 0
IfFileExists "$R1\editor.exe" 0 barony_automatia_skip_backup_editor
CopyFiles /SILENT "$R1\editor.exe" "$R1\editor.exe.automatia-original"
barony_automatia_skip_backup_editor:
IfFileExists "$R1\steam_api64.dll.automatia-original" barony_automatia_skip_backup_steam 0
IfFileExists "$R1\steam_api64.dll" 0 barony_automatia_skip_backup_steam
CopyFiles /SILENT "$R1\steam_api64.dll" "$R1\steam_api64.dll.automatia-original"
barony_automatia_skip_backup_steam:
IfFileExists "$R1\OpenAL32.dll.automatia-original" barony_automatia_skip_backup_openal 0
IfFileExists "$R1\OpenAL32.dll" 0 barony_automatia_skip_backup_openal
CopyFiles /SILENT "$R1\OpenAL32.dll" "$R1\OpenAL32.dll.automatia-original"
barony_automatia_skip_backup_openal:
IfFileExists "$R1\ogg.dll.automatia-original" barony_automatia_skip_backup_ogg 0
IfFileExists "$R1\ogg.dll.automatia-added" barony_automatia_skip_backup_ogg 0
IfFileExists "$R1\ogg.dll" barony_automatia_backup_ogg barony_automatia_mark_added_ogg
barony_automatia_backup_ogg:
CopyFiles /SILENT "$R1\ogg.dll" "$R1\ogg.dll.automatia-original"
Goto barony_automatia_skip_backup_ogg
barony_automatia_mark_added_ogg:
FileOpen $0 "$R1\ogg.dll.automatia-added" w
FileClose $0
barony_automatia_skip_backup_ogg:
IfFileExists "$R1\vorbis.dll.automatia-original" barony_automatia_skip_backup_vorbis 0
IfFileExists "$R1\vorbis.dll.automatia-added" barony_automatia_skip_backup_vorbis 0
IfFileExists "$R1\vorbis.dll" barony_automatia_backup_vorbis barony_automatia_mark_added_vorbis
barony_automatia_backup_vorbis:
CopyFiles /SILENT "$R1\vorbis.dll" "$R1\vorbis.dll.automatia-original"
Goto barony_automatia_skip_backup_vorbis
barony_automatia_mark_added_vorbis:
FileOpen $0 "$R1\vorbis.dll.automatia-added" w
FileClose $0
barony_automatia_skip_backup_vorbis:
IfFileExists "$R1\vorbisfile.dll.automatia-original" barony_automatia_skip_backup_vorbisfile 0
IfFileExists "$R1\vorbisfile.dll.automatia-added" barony_automatia_skip_backup_vorbisfile 0
IfFileExists "$R1\vorbisfile.dll" barony_automatia_backup_vorbisfile barony_automatia_mark_added_vorbisfile
barony_automatia_backup_vorbisfile:
CopyFiles /SILENT "$R1\vorbisfile.dll" "$R1\vorbisfile.dll.automatia-original"
Goto barony_automatia_skip_backup_vorbisfile
barony_automatia_mark_added_vorbisfile:
FileOpen $0 "$R1\vorbisfile.dll.automatia-added" w
FileClose $0
barony_automatia_skip_backup_vorbisfile:
IfFileExists "$R1\SDL2.dll.automatia-original" barony_automatia_skip_backup_sdl2 0
IfFileExists "$R1\SDL2.dll" 0 barony_automatia_skip_backup_sdl2
CopyFiles /SILENT "$R1\SDL2.dll" "$R1\SDL2.dll.automatia-original"
barony_automatia_skip_backup_sdl2:
IfFileExists "$R1\SDL2_image.dll.automatia-original" barony_automatia_skip_backup_sdl2_image 0
IfFileExists "$R1\SDL2_image.dll" 0 barony_automatia_skip_backup_sdl2_image
CopyFiles /SILENT "$R1\SDL2_image.dll" "$R1\SDL2_image.dll.automatia-original"
barony_automatia_skip_backup_sdl2_image:
IfFileExists "$R1\SDL2_net.dll.automatia-original" barony_automatia_skip_backup_sdl2_net 0
IfFileExists "$R1\SDL2_net.dll" 0 barony_automatia_skip_backup_sdl2_net
CopyFiles /SILENT "$R1\SDL2_net.dll" "$R1\SDL2_net.dll.automatia-original"
barony_automatia_skip_backup_sdl2_net:
IfFileExists "$R1\SDL2_ttf.dll.automatia-original" barony_automatia_skip_backup_sdl2_ttf 0
IfFileExists "$R1\SDL2_ttf.dll" 0 barony_automatia_skip_backup_sdl2_ttf
CopyFiles /SILENT "$R1\SDL2_ttf.dll" "$R1\SDL2_ttf.dll.automatia-original"
barony_automatia_skip_backup_sdl2_ttf:
IfFileExists "$R1\freetype.dll.automatia-original" barony_automatia_skip_backup_freetype 0
IfFileExists "$R1\freetype.dll.automatia-added" barony_automatia_skip_backup_freetype 0
IfFileExists "$R1\freetype.dll" barony_automatia_backup_freetype barony_automatia_mark_added_freetype
barony_automatia_backup_freetype:
CopyFiles /SILENT "$R1\freetype.dll" "$R1\freetype.dll.automatia-original"
Goto barony_automatia_skip_backup_freetype
barony_automatia_mark_added_freetype:
FileOpen $0 "$R1\freetype.dll.automatia-added" w
FileClose $0
barony_automatia_skip_backup_freetype:
IfFileExists "$R1\glew32.dll.automatia-original" barony_automatia_skip_backup_glew 0
IfFileExists "$R1\glew32.dll" 0 barony_automatia_skip_backup_glew
CopyFiles /SILENT "$R1\glew32.dll" "$R1\glew32.dll.automatia-original"
barony_automatia_skip_backup_glew:
IfFileExists "$R1\physfs.dll.automatia-original" barony_automatia_skip_backup_physfs 0
IfFileExists "$R1\physfs.dll" 0 barony_automatia_skip_backup_physfs
CopyFiles /SILENT "$R1\physfs.dll" "$R1\physfs.dll.automatia-original"
barony_automatia_skip_backup_physfs:
IfFileExists "$R1\libpng16.dll.automatia-original" barony_automatia_skip_backup_png 0
IfFileExists "$R1\libpng16.dll" 0 barony_automatia_skip_backup_png
CopyFiles /SILENT "$R1\libpng16.dll" "$R1\libpng16.dll.automatia-original"
barony_automatia_skip_backup_png:
IfFileExists "$R1\z.dll.automatia-original" barony_automatia_skip_backup_z 0
IfFileExists "$R1\z.dll.automatia-added" barony_automatia_skip_backup_z 0
IfFileExists "$R1\z.dll" barony_automatia_backup_z barony_automatia_mark_added_z
barony_automatia_backup_z:
CopyFiles /SILENT "$R1\z.dll" "$R1\z.dll.automatia-original"
Goto barony_automatia_skip_backup_z
barony_automatia_mark_added_z:
FileOpen $0 "$R1\z.dll.automatia-added" w
FileClose $0
barony_automatia_skip_backup_z:
IfFileExists "$R1\bz2.dll.automatia-original" barony_automatia_skip_backup_bz2 0
IfFileExists "$R1\bz2.dll.automatia-added" barony_automatia_skip_backup_bz2 0
IfFileExists "$R1\bz2.dll" barony_automatia_backup_bz2 barony_automatia_mark_added_bz2
barony_automatia_backup_bz2:
CopyFiles /SILENT "$R1\bz2.dll" "$R1\bz2.dll.automatia-original"
Goto barony_automatia_skip_backup_bz2
barony_automatia_mark_added_bz2:
FileOpen $0 "$R1\bz2.dll.automatia-added" w
FileClose $0
barony_automatia_skip_backup_bz2:
IfFileExists "$R1\brotlidec.dll.automatia-original" barony_automatia_skip_backup_brotlidec 0
IfFileExists "$R1\brotlidec.dll.automatia-added" barony_automatia_skip_backup_brotlidec 0
IfFileExists "$R1\brotlidec.dll" barony_automatia_backup_brotlidec barony_automatia_mark_added_brotlidec
barony_automatia_backup_brotlidec:
CopyFiles /SILENT "$R1\brotlidec.dll" "$R1\brotlidec.dll.automatia-original"
Goto barony_automatia_skip_backup_brotlidec
barony_automatia_mark_added_brotlidec:
FileOpen $0 "$R1\brotlidec.dll.automatia-added" w
FileClose $0
barony_automatia_skip_backup_brotlidec:
IfFileExists "$R1\brotlicommon.dll.automatia-original" barony_automatia_skip_backup_brotlicommon 0
IfFileExists "$R1\brotlicommon.dll.automatia-added" barony_automatia_skip_backup_brotlicommon 0
IfFileExists "$R1\brotlicommon.dll" barony_automatia_backup_brotlicommon barony_automatia_mark_added_brotlicommon
barony_automatia_backup_brotlicommon:
CopyFiles /SILENT "$R1\brotlicommon.dll" "$R1\brotlicommon.dll.automatia-original"
Goto barony_automatia_skip_backup_brotlicommon
barony_automatia_mark_added_brotlicommon:
FileOpen $0 "$R1\brotlicommon.dll.automatia-added" w
FileClose $0
barony_automatia_skip_backup_brotlicommon:
WriteRegStr HKCU "Software\Automatia\Barony Steam Patch" "SteamInstallDir" "$R1"
MessageBox MB_OK|MB_ICONINFORMATION "Barony Automatia will patch:$\n$R1$\nClose Barony before continuing."
]=])
			string(REPLACE "\\" "\\\\" CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS
				"${CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS}")
			string(REPLACE "\"" "\\\"" CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS
				"${CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS}")
			set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS [=[
ReadRegStr $R1 HKCU "Software\Automatia\Barony Steam Patch" "SteamInstallDir"
SetOutPath "$R1"
CopyFiles /SILENT "$INSTDIR\barony.exe" "$R1"
CopyFiles /SILENT "$INSTDIR\editor.exe" "$R1"
CopyFiles /SILENT "$INSTDIR\steam_api64.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\OpenAL32.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\ogg.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\vorbis.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\vorbisfile.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\SDL2.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\SDL2_image.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\SDL2_net.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\SDL2_ttf.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\freetype.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\glew32.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\physfs.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\libpng16.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\z.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\bz2.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\brotlidec.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\brotlicommon.dll" "$R1"
CopyFiles /SILENT "$INSTDIR\steam_appid.txt" "$R1"
]=])
			string(REPLACE "\\" "\\\\" CPACK_NSIS_EXTRA_INSTALL_COMMANDS
				"${CPACK_NSIS_EXTRA_INSTALL_COMMANDS}")
			string(REPLACE "\"" "\\\"" CPACK_NSIS_EXTRA_INSTALL_COMMANDS
				"${CPACK_NSIS_EXTRA_INSTALL_COMMANDS}")
			set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS [=[
ReadRegStr $R1 HKCU "Software\Automatia\Barony Steam Patch" "SteamInstallDir"
IfFileExists "$R1\barony.exe.automatia-original" 0 barony_automatia_no_restore_barony
Delete "$R1\barony.exe"
Rename "$R1\barony.exe.automatia-original" "$R1\barony.exe"
barony_automatia_no_restore_barony:
IfFileExists "$R1\editor.exe.automatia-original" 0 barony_automatia_no_restore_editor
Delete "$R1\editor.exe"
Rename "$R1\editor.exe.automatia-original" "$R1\editor.exe"
barony_automatia_no_restore_editor:
IfFileExists "$R1\steam_api64.dll.automatia-original" 0 barony_automatia_no_restore_steam
Delete "$R1\steam_api64.dll"
Rename "$R1\steam_api64.dll.automatia-original" "$R1\steam_api64.dll"
barony_automatia_no_restore_steam:
IfFileExists "$R1\OpenAL32.dll.automatia-original" 0 barony_automatia_no_restore_openal
Delete "$R1\OpenAL32.dll"
Rename "$R1\OpenAL32.dll.automatia-original" "$R1\OpenAL32.dll"
barony_automatia_no_restore_openal:
IfFileExists "$R1\ogg.dll.automatia-original" 0 barony_automatia_no_restore_ogg
Delete "$R1\ogg.dll"
Rename "$R1\ogg.dll.automatia-original" "$R1\ogg.dll"
barony_automatia_no_restore_ogg:
IfFileExists "$R1\ogg.dll.automatia-added" 0 barony_automatia_done_restore_ogg
Delete "$R1\ogg.dll"
Delete "$R1\ogg.dll.automatia-added"
barony_automatia_done_restore_ogg:
IfFileExists "$R1\vorbis.dll.automatia-original" 0 barony_automatia_no_restore_vorbis
Delete "$R1\vorbis.dll"
Rename "$R1\vorbis.dll.automatia-original" "$R1\vorbis.dll"
barony_automatia_no_restore_vorbis:
IfFileExists "$R1\vorbis.dll.automatia-added" 0 barony_automatia_done_restore_vorbis
Delete "$R1\vorbis.dll"
Delete "$R1\vorbis.dll.automatia-added"
barony_automatia_done_restore_vorbis:
IfFileExists "$R1\vorbisfile.dll.automatia-original" 0 barony_automatia_no_restore_vorbisfile
Delete "$R1\vorbisfile.dll"
Rename "$R1\vorbisfile.dll.automatia-original" "$R1\vorbisfile.dll"
barony_automatia_no_restore_vorbisfile:
IfFileExists "$R1\vorbisfile.dll.automatia-added" 0 barony_automatia_done_restore_vorbisfile
Delete "$R1\vorbisfile.dll"
Delete "$R1\vorbisfile.dll.automatia-added"
barony_automatia_done_restore_vorbisfile:
IfFileExists "$R1\SDL2.dll.automatia-original" 0 barony_automatia_no_restore_sdl2
Delete "$R1\SDL2.dll"
Rename "$R1\SDL2.dll.automatia-original" "$R1\SDL2.dll"
barony_automatia_no_restore_sdl2:
IfFileExists "$R1\SDL2_image.dll.automatia-original" 0 barony_automatia_no_restore_sdl2_image
Delete "$R1\SDL2_image.dll"
Rename "$R1\SDL2_image.dll.automatia-original" "$R1\SDL2_image.dll"
barony_automatia_no_restore_sdl2_image:
IfFileExists "$R1\SDL2_net.dll.automatia-original" 0 barony_automatia_no_restore_sdl2_net
Delete "$R1\SDL2_net.dll"
Rename "$R1\SDL2_net.dll.automatia-original" "$R1\SDL2_net.dll"
barony_automatia_no_restore_sdl2_net:
IfFileExists "$R1\SDL2_ttf.dll.automatia-original" 0 barony_automatia_no_restore_sdl2_ttf
Delete "$R1\SDL2_ttf.dll"
Rename "$R1\SDL2_ttf.dll.automatia-original" "$R1\SDL2_ttf.dll"
barony_automatia_no_restore_sdl2_ttf:
IfFileExists "$R1\freetype.dll.automatia-original" 0 barony_automatia_no_restore_freetype
Delete "$R1\freetype.dll"
Rename "$R1\freetype.dll.automatia-original" "$R1\freetype.dll"
barony_automatia_no_restore_freetype:
IfFileExists "$R1\freetype.dll.automatia-added" 0 barony_automatia_done_restore_freetype
Delete "$R1\freetype.dll"
Delete "$R1\freetype.dll.automatia-added"
barony_automatia_done_restore_freetype:
IfFileExists "$R1\glew32.dll.automatia-original" 0 barony_automatia_no_restore_glew
Delete "$R1\glew32.dll"
Rename "$R1\glew32.dll.automatia-original" "$R1\glew32.dll"
barony_automatia_no_restore_glew:
IfFileExists "$R1\physfs.dll.automatia-original" 0 barony_automatia_no_restore_physfs
Delete "$R1\physfs.dll"
Rename "$R1\physfs.dll.automatia-original" "$R1\physfs.dll"
barony_automatia_no_restore_physfs:
IfFileExists "$R1\libpng16.dll.automatia-original" 0 barony_automatia_no_restore_png
Delete "$R1\libpng16.dll"
Rename "$R1\libpng16.dll.automatia-original" "$R1\libpng16.dll"
barony_automatia_no_restore_png:
IfFileExists "$R1\z.dll.automatia-original" 0 barony_automatia_no_restore_z
Delete "$R1\z.dll"
Rename "$R1\z.dll.automatia-original" "$R1\z.dll"
barony_automatia_no_restore_z:
IfFileExists "$R1\z.dll.automatia-added" 0 barony_automatia_done_restore_z
Delete "$R1\z.dll"
Delete "$R1\z.dll.automatia-added"
barony_automatia_done_restore_z:
IfFileExists "$R1\bz2.dll.automatia-original" 0 barony_automatia_no_restore_bz2
Delete "$R1\bz2.dll"
Rename "$R1\bz2.dll.automatia-original" "$R1\bz2.dll"
barony_automatia_no_restore_bz2:
IfFileExists "$R1\bz2.dll.automatia-added" 0 barony_automatia_done_restore_bz2
Delete "$R1\bz2.dll"
Delete "$R1\bz2.dll.automatia-added"
barony_automatia_done_restore_bz2:
IfFileExists "$R1\brotlidec.dll.automatia-original" 0 barony_automatia_no_restore_brotlidec
Delete "$R1\brotlidec.dll"
Rename "$R1\brotlidec.dll.automatia-original" "$R1\brotlidec.dll"
barony_automatia_no_restore_brotlidec:
IfFileExists "$R1\brotlidec.dll.automatia-added" 0 barony_automatia_done_restore_brotlidec
Delete "$R1\brotlidec.dll"
Delete "$R1\brotlidec.dll.automatia-added"
barony_automatia_done_restore_brotlidec:
IfFileExists "$R1\brotlicommon.dll.automatia-original" 0 barony_automatia_no_restore_brotlicommon
Delete "$R1\brotlicommon.dll"
Rename "$R1\brotlicommon.dll.automatia-original" "$R1\brotlicommon.dll"
barony_automatia_no_restore_brotlicommon:
IfFileExists "$R1\brotlicommon.dll.automatia-added" 0 barony_automatia_done_restore_brotlicommon
Delete "$R1\brotlicommon.dll"
Delete "$R1\brotlicommon.dll.automatia-added"
barony_automatia_done_restore_brotlicommon:
DeleteRegValue HKCU "Software\Automatia\Barony Steam Patch" "SteamInstallDir"
DeleteRegKey /ifempty HKCU "Software\Automatia\Barony Steam Patch"
]=])
			string(REPLACE "\\" "\\\\" CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
				"${CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS}")
			string(REPLACE "\"" "\\\"" CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
				"${CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS}")
		endif()
		if (BARONY_WINDOWS_STEAM_PATCH_INSTALLER AND BARONY_WINDOWS_STEAM_SIDEBYSIDE_INSTALLER)
			include("${CMAKE_SOURCE_DIR}/cmake/BaronySteamSideBySideInstaller.cmake")
		endif()
		include(CPack)

		set(_barony_windows_installer_dependencies barony)
		if (EDITOR_ENABLED)
			list(APPEND _barony_windows_installer_dependencies ${EDITOR_EXE_NAME})
		endif()
		if (BARONY_WINDOWS_STEAM_PATCH_INSTALLER)
			list(APPEND _barony_windows_installer_dependencies barony_automatia_steam_shortcut)
		endif()
		add_custom_target(windows-installer
			COMMAND "${CMAKE_CPACK_COMMAND}" --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake" -G NSIS
			DEPENDS ${_barony_windows_installer_dependencies}
			WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
			COMMENT "Creating the Barony Automatia Windows NSIS installer"
			VERBATIM)
	endif()

	if (BARONY_WINDOWS_RELEASE_PACKAGE_TARGET)
		if (NOT STEAMWORKS_ENABLED OR NOT OPENAL)
			message(FATAL_ERROR
				"package_automatia_windows requires Steamworks and the OpenAL backend.")
		endif()
		foreach(_barony_release_required_path
			BARONY_WINDOWS_RELEASE_VCPKG_ROOT
			BARONY_WINDOWS_RELEASE_ASSETS_ROOT
			BARONY_WINDOWS_RELEASE_OPENAL_SOURCE_ARCHIVE
			BARONY_WINDOWS_RELEASE_PHYSFS_SOURCE
			BARONY_WINDOWS_RELEASE_VC_REDIST)
			if (NOT EXISTS "${${_barony_release_required_path}}")
				message(FATAL_ERROR
					"package_automatia_windows requires ${_barony_release_required_path} to name an existing path.")
			endif()
		endforeach()
		find_package(Python3 REQUIRED COMPONENTS Interpreter)
		set(_barony_release_editor_argument)
		if (EDITOR_ENABLED)
			list(APPEND _barony_release_editor_argument --include-editor)
		endif()
		add_custom_target(package_automatia_windows
			COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/package_windows_release.py"
				--build-dir "${CMAKE_BINARY_DIR}/$<CONFIG>"
				--output-dir "${BARONY_WINDOWS_RELEASE_OUTPUT_DIR}"
				--clean
				--vcpkg-root "${BARONY_WINDOWS_RELEASE_VCPKG_ROOT}"
				--triplet "${VCPKG_TARGET_TRIPLET}"
				--steamworks-sdk "${STEAMWORKS_ROOT}"
				--openal-root "${BARONY_WINDOWS_RELEASE_VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}"
				--openal-source-archive "${BARONY_WINDOWS_RELEASE_OPENAL_SOURCE_ARCHIVE}"
				--physfs-dll "${BARONY_PHYSFS_RUNTIME_DLL}"
				--physfs-source "${BARONY_WINDOWS_RELEASE_PHYSFS_SOURCE}"
				--assets-root "${BARONY_WINDOWS_RELEASE_ASSETS_ROOT}"
				--vc-redist "${BARONY_WINDOWS_RELEASE_VC_REDIST}"
				--compiler "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
				--windows-sdk "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
				${_barony_release_editor_argument}
			DEPENDS barony_selected_physfs_runtime
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
			COMMENT "Staging and validating the audited Barony Automatia Windows release"
			VERBATIM)
	endif()
	if (TARGET windows-installer AND TARGET package_automatia_windows)
		add_dependencies(windows-installer package_automatia_windows)
	endif()
endif()

if (APPLE)
#  SET_TARGET_PROPERTIES(
#    barony
#    PROPERTIES
#    RESOURCE ${BOOK_FILES} ${IMAGE_FILES} ${ITEM_FILES} ${MAP_FILES} ${MODEL_FILES} ${MUSIC_FILES} ${SOUND_FILES}
#  )
endif()
